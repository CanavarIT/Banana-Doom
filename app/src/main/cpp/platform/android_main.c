/* Android entry point (native_app_glue), system layer and input. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <android/asset_manager.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android_native_app_glue.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_event.h"
#include "d_main.h"
#include "g_game.h"
#include "m_argv.h"
#include "m_menu.h"
#include "m_misc.h"
#include "i_system.h"
#include "i_sound.h"
#include "i_video.h"
#include "plat.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "DOOM", __VA_ARGS__)

static struct android_app *app;
static int paused_flag;
int plat_loop_started;
int plat_tap_exit;
static int in_error;

/* ------------------------------------------------------------------ */
/* time / memory                                                       */
/* ------------------------------------------------------------------ */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int I_GetTime(void)
{
    static long long base;
    long long t = now_ms();
    if (!base) base = t;
    return (int)((t - base) * TICRATE / 1000);
}

void I_Sleep(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

void I_WaitVBL(int count) { I_Sleep(count * 1000 / 70); }
void I_BeginRead(void) {}
void I_EndRead(void) {}
void I_Tactile(int on, int off, int total) { (void)on; (void)off; (void)total; }

static ticcmd_t emptycmd;
ticcmd_t *I_BaseTiccmd(void) { return &emptycmd; }

#define ZONE_MB 16
byte *I_ZoneBase(int *size)
{
    *size = ZONE_MB * 1024 * 1024;
    return (byte *)malloc(*size);
}

byte *I_AllocLow(int length)
{
    return (byte *)calloc(1, length);
}

void I_Init(void)
{
    I_InitSound();
    I_InitMusic();
}

/* ------------------------------------------------------------------ */
/* IWAD lookup                                                         */
/* ------------------------------------------------------------------ */

static int file_exists(const char *p) { return access(p, R_OK) == 0; }

static int copy_asset(const char *name, const char *dest)
{
    AAsset *a = AAssetManager_open(app->activity->assetManager, name, AASSET_MODE_STREAMING);
    char tmp[1100], buf[65536];
    int fd, n;
    if (!a) return 0;
    snprintf(tmp, sizeof(tmp), "%s.tmp", dest);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { AAsset_close(a); return 0; }
    while ((n = AAsset_read(a, buf, sizeof(buf))) > 0)
        if (write(fd, buf, n) != n) { n = -1; break; }
    close(fd);
    AAsset_close(a);
    if (n < 0) { unlink(tmp); return 0; }
    return rename(tmp, dest) == 0;
}

const char *I_FindIWAD(const char *name)
{
    static char path[1100];
    const char *dirs[2];
    char up[64];
    int d, v, i;
    const char *names[2];

    for (i = 0; name[i] && i < 63; i++) up[i] = (name[i] >= 'a' && name[i] <= 'z') ? name[i] - 32 : name[i];
    up[i] = 0;
    names[0] = name; names[1] = up;
    dirs[0] = app->activity->externalDataPath;
    dirs[1] = app->activity->internalDataPath;

    for (d = 0; d < 2; d++) {
        if (!dirs[d]) continue;
        for (v = 0; v < 2; v++) {
            snprintf(path, sizeof(path), "%s/%s", dirs[d], names[v]);
            if (file_exists(path)) return path;
        }
    }
    /* bundled in the APK (app/src/main/assets): extract once */
    snprintf(path, sizeof(path), "%s/%s", app->activity->internalDataPath, name);
    for (v = 0; v < 2; v++)
        if (copy_asset(names[v], path)) return path;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* fatal error screen                                                  */
/* ------------------------------------------------------------------ */

static void pump(int block);

static int wrap_text(vtx_t *out, int max, const char *s, float x, float y,
                     float sc, int cpl, int *lines_used)
{
    int n = 0, len = (int)strlen(s), i = 0, lines = 0;
    char line[160];
    while (i < len && n < max - 700) {
        int j = 0;
        while (i < len && s[i] != '\n' && j < cpl && j < (int)sizeof(line) - 1) line[j++] = s[i++];
        if (i < len && s[i] == '\n') i++;
        line[j] = 0;
        n += text_build(out + n, max - n, line, x, y + lines * 9.0f * sc, sc, 1, 1, 1, 1, 0);
        lines++;
    }
    *lines_used = lines;
    return n;
}

static void fatal_screen(const char *msg)
{
    static vtx_t v[9000];
    char full[1500];
    int n, lines, w, h, cpl;
    float sc;

    snprintf(full, sizeof(full), "DOOM ERROR\n\n%s\n", msg);
    if (!strncmp(msg, "No IWAD", 7)) {
        strncat(full, "\nCopy doom1.wad (or doom.wad / doom2.wad) to:\n",
                sizeof(full) - strlen(full) - 1);
        strncat(full, app->activity->externalDataPath ? app->activity->externalDataPath : "?",
                sizeof(full) - strlen(full) - 1);
        strncat(full, "\n\nor put it in app/src/main/assets before building.\n",
                sizeof(full) - strlen(full) - 1);
    }
    strncat(full, "\nTap to exit.", sizeof(full) - strlen(full) - 1);

    for (;;) {
        int tap = 0;
        pump(0);
        if (gl_ready()) {
            gl_surface_size(&w, &h);
            sc = h / 180.0f; if (sc < 2) sc = 2;
            cpl = (int)(w * 0.92f / (6 * sc));
            if (cpl > 150) cpl = 150;
            n = wrap_text(v, (int)(sizeof(v) / sizeof(v[0])), full, w * 0.04f, h * 0.06f, sc, cpl, &lines);
            gl_draw_overlay_only(v, n);
        }
        tap = plat_tap_exit;
        if (tap) { gl_detach(); exit(1); }
        I_Sleep(40);
    }
}

void I_Error(char *error, ...)
{
    char msg[900];
    va_list ap;
    va_start(ap, error);
    vsnprintf(msg, sizeof(msg), error, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_ERROR, "DOOM", "I_Error: %s", msg);
    if (in_error) exit(1);
    in_error = 1;
    plat_tap_exit = 0;
    fatal_screen(msg);
}

void I_Quit(void)
{
    M_SaveDefaults();
    I_ShutdownSound();
    gl_detach();
    ANativeActivity_finish(app->activity);
    exit(0);
}

/* ------------------------------------------------------------------ */
/* input                                                               */
/* ------------------------------------------------------------------ */

static void post_key(int down, int key)
{
    event_t e;
    e.type = down ? ev_keydown : ev_keyup;
    e.data1 = key; e.data2 = e.data3 = 0;
    D_PostEvent(&e);
}

/* returns Doom key, or -1/-2 for weapon prev/next, 0 = unhandled */
static int map_key(int code)
{
    if (code >= AKEYCODE_0 && code <= AKEYCODE_9) return '0' + (code - AKEYCODE_0);
    if (code >= AKEYCODE_F1 && code <= AKEYCODE_F10) return KEY_F1 + (code - AKEYCODE_F1);
    switch (code) {
    case AKEYCODE_DPAD_UP: case AKEYCODE_W:     return KEY_UPARROW;
    case AKEYCODE_DPAD_DOWN: case AKEYCODE_S:   return KEY_DOWNARROW;
    case AKEYCODE_DPAD_LEFT:                    return KEY_LEFTARROW;
    case AKEYCODE_DPAD_RIGHT:                   return KEY_RIGHTARROW;
    case AKEYCODE_A:                            return ',';
    case AKEYCODE_D:                            return '.';
    case AKEYCODE_CTRL_LEFT: case AKEYCODE_CTRL_RIGHT:
    case AKEYCODE_BUTTON_A: case AKEYCODE_BUTTON_R2: return KEY_RCTRL;
    case AKEYCODE_SPACE: case AKEYCODE_BUTTON_X: return ' ';
    case AKEYCODE_ENTER: case AKEYCODE_NUMPAD_ENTER: case AKEYCODE_DPAD_CENTER: return KEY_ENTER;
    case AKEYCODE_ESCAPE: case AKEYCODE_BACK: case AKEYCODE_BUTTON_B:
    case AKEYCODE_BUTTON_START: case AKEYCODE_BUTTON_SELECT: return KEY_ESCAPE;
    case AKEYCODE_TAB: case AKEYCODE_BUTTON_Y:  return KEY_TAB;
    case AKEYCODE_SHIFT_LEFT: case AKEYCODE_SHIFT_RIGHT: return KEY_RSHIFT;
    case AKEYCODE_Y:                            return 'y';
    case AKEYCODE_N:                            return 'n';
    case AKEYCODE_MINUS:                        return KEY_MINUS;
    case AKEYCODE_EQUALS:                       return KEY_EQUALS;
    case AKEYCODE_BUTTON_L1:                    return -1;
    case AKEYCODE_BUTTON_R1:                    return -2;
    }
    return 0;
}

static int32_t on_input(struct android_app *a, AInputEvent *ev)
{
    (void)a;
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION) {
        int32_t src = AInputEvent_getSource(ev);
        int32_t act = AMotionEvent_getAction(ev);
        int32_t what = act & AMOTION_EVENT_ACTION_MASK;
        int idx = (act & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
        int i, cnt;

        if ((src & AINPUT_SOURCE_CLASS_MASK) == AINPUT_SOURCE_CLASS_JOYSTICK) {
            touch_pad(AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_X, 0),
                      AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Y, 0),
                      AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Z, 0));
            return 1;
        }
        plat_tap_exit |= (what == AMOTION_EVENT_ACTION_UP);
        switch (what) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
            touch_down(AMotionEvent_getPointerId(ev, idx),
                       AMotionEvent_getX(ev, idx), AMotionEvent_getY(ev, idx));
            break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP:
            touch_up(AMotionEvent_getPointerId(ev, idx));
            break;
        case AMOTION_EVENT_ACTION_MOVE:
            cnt = (int)AMotionEvent_getPointerCount(ev);
            for (i = 0; i < cnt; i++)
                touch_move(AMotionEvent_getPointerId(ev, i),
                           AMotionEvent_getX(ev, i), AMotionEvent_getY(ev, i));
            break;
        case AMOTION_EVENT_ACTION_CANCEL:
            touch_cancel_all();
            break;
        }
        return 1;
    }
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_KEY) {
        int32_t code = AKeyEvent_getKeyCode(ev);
        int32_t act = AKeyEvent_getAction(ev);
        int k = map_key(code);
        if (!k) return 0;                       /* volume etc. -> system */
        if (code == AKEYCODE_BACK && act == AKEY_EVENT_ACTION_UP) plat_tap_exit = 1;
        if (k < 0) {
            if (act == AKEY_EVENT_ACTION_DOWN && AKeyEvent_getRepeatCount(ev) == 0)
                touch_wpn = (k == -1) ? -1 : 1;
        } else if (act == AKEY_EVENT_ACTION_DOWN) {
            post_key(1, k);
        } else if (act == AKEY_EVENT_ACTION_UP) {
            post_key(0, k);
        }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void on_cmd(struct android_app *a, int32_t cmd)
{
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (a->window) gl_attach(a->window);
        break;
    case APP_CMD_TERM_WINDOW:
        gl_detach();
        break;
    case APP_CMD_PAUSE:
        paused_flag = 1;
        snd_pause(1);
        touch_cancel_all();
        if (plat_loop_started && gamestate == GS_LEVEL && !menuactive)
            M_StartControlPanel();              /* pauses the game */
        break;
    case APP_CMD_RESUME:
        paused_flag = 0;
        snd_pause(0);
        break;
    default:
        break;
    }
}

static void pump(int block)
{
    int events;
    struct android_poll_source *src;
    while (ALooper_pollOnce(block ? -1 : 0, NULL, &events, (void **)&src) >= 0) {
        if (src) src->process(app, src);
        if (app->destroyRequested) {
            if (plat_loop_started) M_SaveDefaults();
            gl_detach();
            exit(0);
        }
        block = 0;
    }
}

void I_StartTic(void)
{
    do {
        pump(!app->window || paused_flag || !gl_ready());
    } while (!app->window || paused_flag || !gl_ready());
    kbd_poll();                                 /* system keyboard text -> save name */
    touch_tick((int)now_ms());
}

void android_main(struct android_app *state)
{
    static char *argv[] = { "doom", NULL };
    app = state;
    state->onAppCmd = on_cmd;
    state->onInputEvent = on_input;

    if (chdir(state->activity->internalDataPath) != 0)
        LOG("chdir failed: %s", strerror(errno));
    touch_init();
    kbd_init(state);

    while (!gl_ready()) pump(1);

    myargc = 1;
    myargv = argv;
    D_DoomMain();
}
