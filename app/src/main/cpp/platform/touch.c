/*
 * On-screen touch controls for DOOM.
 *   left side  : floating analog stick (move / strafe, or menu arrows)
 *   right side : drag to turn; FIRE / USE / weapon / MENU / MAP buttons
 */
#include <math.h>
#include <string.h>
#include "plat.h"
#include "doomdef.h"
#include "doomstat.h"
#include "d_event.h"
#include "d_main.h"
#include "g_game.h"

extern boolean messageNeedsInput;

enum { A_KEYS, A_WPN };

typedef struct {
    float cx, cy, r;
    const char *lbl_lvl, *lbl_menu;
    int act, arg;
    int keys_lvl[2], keys_menu[2];
    int show_lvl, show_menu;        /* 0 hidden, 1 shown, 2 only on yes/no prompt */
    int ptr;
    int sent[2];
    float ux, uy, ur;               /* layout in units of min(W,H) */
    float lastx;                    /* FIRE: previous x of the holding finger (drag-to-aim) */
} btn_t;

enum { B_FIRE, B_USE, B_PREV, B_NEXT, B_MENU, B_MAP, NBTN };

static btn_t btn[NBTN] = {
 /* FIRE */ {0,0,0, "FIRE","OK",   A_KEYS, 0, {KEY_RCTRL,0}, {KEY_ENTER,KEY_RCTRL}, 1,1, -1,{0,0}, 0.173f,0.18f,0.145f},
 /* USE  */ {0,0,0, "USE","YES",   A_KEYS, 0, {' ',0},       {'y',0},                1,2, -1,{0,0}, 0.343f,0.37f,0.097f},
 /* PREV */ {0,0,0, "<","",        A_WPN, -1, {0,0},         {0,0},                  1,0, -1,{0,0}, 0.343f,0.579f,0.075f},
 /* NEXT */ {0,0,0, ">","",        A_WPN,  1, {0,0},         {0,0},                  1,0, -1,{0,0}, 0.173f,0.579f,0.075f},
 /* MENU */ {0,0,0, "MENU","BACK", A_KEYS, 0, {KEY_ESCAPE,0},{KEY_ESCAPE,0},         1,1, -1,{0,0}, 0.07f,-0.07f,0.05f},
 /* MAP  */ {0,0,0, "MAP","",      A_KEYS, 0, {KEY_TAB,0},   {0,0},                  1,0, -1,{0,0}, 0.19f,-0.07f,0.05f},
};

/* ------------------------------------------------------------------ */
/* tuning                                                              */
/* ------------------------------------------------------------------ */

#define LOOK_DEG_PER_PX    0.20f
#define LOOK_UNITS_PER_DEG (65536.0f / 360.0f)
#define LOOK_MAX_UNITS     30000.0f
#define PAD_TURN_UNITS     480.0f

/* Soft radial deadzone — continuous, no hysteresis */
#define STICK_DZ           0.15f

/* Exponential smoothing of stick axes (0 = raw, 1 = frozen).
 * 0.35 is a good compromise: kills touch noise / micro-jitter
 * without making the stick feel heavy. */
#define STICK_SMOOTH       0.35f

#define HIT_PAD            1.15f

/* Size of FIRE / USE / weapon buttons: 1.0 = default, 1.2 = 20% bigger,
 * 0.85 = 15% smaller. (MAP/MENU keep their size.) */
#define BTN_SCALE          1.0f

static int W = 1280, H = 720;
static float ui;
static float stick_r, look_units_per_px;

static int stick_id = -1, look_id = -1;
static float sox, soy, scx, scy, look_lx;
static float look_units;
static float pad_lx, pad_ly, pad_rx;

/* smoothed stick axes (persist between tics) */
static float sm_ax, sm_ay;

/* menu arrow repeat */
static int menu_dir = 0, menu_next_ms = 0;

static void layout(void)
{
    int i;
    float s = (float)(W < H ? W : H);
    ui = s;
    for (i = 0; i < NBTN; i++) {
        btn_t *b = &btn[i];
        b->r  = b->ur * s * ((i == B_MAP || i == B_MENU) ? 1.0f : BTN_SCALE);
        b->cx = W - b->ux * s;
        b->cy = (b->uy >= 0.0f) ? H - b->uy * s : -b->uy * s;
        if (b->cx < b->r)      b->cx = b->r;
        if (b->cx > W - b->r)  b->cx = W - b->r;
        if (b->cy < b->r)      b->cy = b->r;
        if (b->cy > H - b->r)  b->cy = H - b->r;
    }
    stick_r = 0.16f * s;      /* stick size: compact (was 0.26) */
    look_units_per_px = LOOK_DEG_PER_PX * LOOK_UNITS_PER_DEG * (720.0f / s);
}

#ifdef __ANDROID__
#include <android/log.h>
#endif

void touch_init(void) { layout(); }

void touch_resize(int w, int h, float density)
{
    (void)density;
    W = w > 0 ? w : 1; H = h > 0 ? h : 1;
    layout();
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "DOOM", "touch layout v5: %dx%d FIRE r=%d USE r=%d WPN r=%d stick r=%d",
                        W, H, (int)btn[B_FIRE].r, (int)btn[B_USE].r, (int)btn[B_PREV].r, (int)stick_r);
#endif
}

static int menu_mode(void)
{
    return menuactive || gamestate != GS_LEVEL;
}

static int visible(const btn_t *b)
{
    int s = menu_mode() ? b->show_menu : b->show_lvl;
    return s == 1 || (s == 2 && messageNeedsInput);
}

static void post_key(int down, int k)
{
    event_t e;
    if (!k) return;
    e.type = down ? ev_keydown : ev_keyup;
    e.data1 = k; e.data2 = e.data3 = 0;
    D_PostEvent(&e);
}

static void btn_press(btn_t *b)
{
    const int *k = menu_mode() ? b->keys_menu : b->keys_lvl;
    if (b->act == A_WPN) {
        if (!menu_mode()) touch_wpn = b->arg;
        return;
    }
    b->sent[0] = k[0]; b->sent[1] = k[1];
    post_key(1, k[0]); post_key(1, k[1]);
}

static void btn_release(btn_t *b)
{
    post_key(0, b->sent[0]); post_key(0, b->sent[1]);
    b->sent[0] = b->sent[1] = 0;
}

static btn_t *hit_button(float x, float y)
{
    int i;
    btn_t *best = 0;
    float bestd = 3.0e30f;
    for (i = 0; i < NBTN; i++) {
        btn_t *b = &btn[i];
        float dx = x - b->cx, dy = y - b->cy, r = b->r * HIT_PAD + 0.012f * ui; /* small buttons stay easy to hit */
        float d2 = dx*dx + dy*dy;
        if (!visible(b) || d2 > r*r) continue;
        if (d2 < bestd) { bestd = d2; best = b; }
    }
    return best;
}

void touch_down(int id, float x, float y)
{
    btn_t *b = hit_button(x, y);
    if (b) {
        if (b->ptr < 0) { b->ptr = id; b->lastx = x; btn_press(b); }
        return;
    }
    if (x < W * 0.45f) {
        if (stick_id < 0) {
            stick_id = id; sox = scx = x; soy = scy = y;
            sm_ax = sm_ay = 0.0f;          /* start from rest */
            if (sox < stick_r) sox = scx = stick_r;
            if (sox > W - stick_r) sox = scx = W - stick_r;
            if (soy > H - stick_r) soy = scy = H - stick_r;
            if (soy < stick_r) soy = scy = stick_r;
        }
    } else if (look_id < 0) {
        look_id = id; look_lx = x;
    }
}

void touch_move(int id, float x, float y)
{
    if (id == stick_id) {
        float dx = x - sox, dy = y - soy, d = sqrtf(dx*dx + dy*dy);
        if (d > stick_r) { dx *= stick_r / d; dy *= stick_r / d; }
        scx = sox + dx; scy = soy + dy;
    } else if (id == look_id) {
        float d = x - look_lx;
        look_lx = x;
        look_units += d * look_units_per_px;
        if (look_units >  LOOK_MAX_UNITS) look_units =  LOOK_MAX_UNITS;
        if (look_units < -LOOK_MAX_UNITS) look_units = -LOOK_MAX_UNITS;
    } else if (btn[B_FIRE].ptr == id && !menu_mode()) {
        /* finger is holding FIRE: dragging it aims while shooting */
        float d = x - btn[B_FIRE].lastx;
        btn[B_FIRE].lastx = x;
        look_units += d * look_units_per_px;
        if (look_units >  LOOK_MAX_UNITS) look_units =  LOOK_MAX_UNITS;
        if (look_units < -LOOK_MAX_UNITS) look_units = -LOOK_MAX_UNITS;
    }
}

void touch_up(int id)
{
    int i;
    if (id == stick_id) {
        stick_id = -1;
        sm_ax = sm_ay = 0.0f;           /* hard stop on release */
        touch_fwd = touch_side = 0;
    }
    if (id == look_id) look_id = -1;
    for (i = 0; i < NBTN; i++)
        if (btn[i].ptr == id) { btn[i].ptr = -1; btn_release(&btn[i]); }
}

void touch_cancel_all(void)
{
    int i;
    stick_id = look_id = -1;
    look_units = 0;
    sm_ax = sm_ay = 0.0f;
    menu_dir = 0;
    for (i = 0; i < NBTN; i++)
        if (btn[i].ptr >= 0) { btn[i].ptr = -1; btn_release(&btn[i]); }
    touch_fwd = touch_side = 0;
}

void touch_pad(float lx, float ly, float rx) { pad_lx = lx; pad_ly = ly; pad_rx = rx; }

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Soft radial deadzone — continuous remap, direction preserved */
static void apply_deadzone(float *ax, float *ay)
{
    float x = *ax, y = *ay;
    float mag = sqrtf(x*x + y*y);
    if (mag <= STICK_DZ) {
        *ax = *ay = 0.0f;
        return;
    }
    float scale = (mag - STICK_DZ) / (1.0f - STICK_DZ) / mag;
    *ax = x * scale;
    *ay = y * scale;
}

/* walk / run curve */
static float curve(float v)
{
    float a = fabsf(v), r;
    r = (a < 0.7f) ? a / 0.7f * 0.5f : 0.5f + (a - 0.7f) / 0.3f * 0.5f;
    return v < 0 ? -r : r;
}

/* curve() output 0..0.5 -> 0..walk, 0.5..1 -> walk..run (keeps sign) */
static float analog_speed(float v, float walk, float run)
{
    float c = curve(v), a = fabsf(c), r;
    r = (a <= 0.5f) ? a / 0.5f * walk : walk + (a - 0.5f) / 0.5f * (run - walk);
    return c < 0 ? -r : r;
}

void touch_tick(int now_ms)
{
    float ax = 0, ay = 0;
    int menu = menu_mode();

    if (stick_id >= 0) {
        ax = clampf((scx - sox) / stick_r, -1, 1);
        ay = clampf((scy - soy) / stick_r, -1, 1);
    }
    ax = clampf(ax + pad_lx, -1, 1);
    ay = clampf(ay + pad_ly, -1, 1);

    if (menu) {
        int dir = 0, key = 0;
        touch_fwd = touch_side = 0;
        sm_ax = sm_ay = 0.0f;
        look_units = 0;
        if (fabsf(ay) > 0.50f && fabsf(ay) >= fabsf(ax))  dir = ay < 0 ? 1 : 2;
        else if (fabsf(ax) > 0.50f)                       dir = ax < 0 ? 3 : 4;
        if (dir && (dir != menu_dir || now_ms >= menu_next_ms)) {
            key = dir == 1 ? KEY_UPARROW : dir == 2 ? KEY_DOWNARROW :
                  dir == 3 ? KEY_LEFTARROW : KEY_RIGHTARROW;
            post_key(1, key); post_key(0, key);
            menu_next_ms = now_ms + (dir != menu_dir ? 350 : 130);
        }
        menu_dir = dir;
        return;
    }
    menu_dir = 0;

    apply_deadzone(&ax, &ay);

    /* exponential smoothing — kills micro-jitter from the touchscreen
       that otherwise makes left/right strafe stutter */
    sm_ax = sm_ax * STICK_SMOOTH + ax * (1.0f - STICK_SMOOTH);
    sm_ay = sm_ay * STICK_SMOOTH + ay * (1.0f - STICK_SMOOTH);

    /* snap to zero when very small so we don't keep a tiny residual */
    if (fabsf(sm_ax) < 0.04f) sm_ax = 0.0f;
    if (fabsf(sm_ay) < 0.04f) sm_ay = 0.0f;

    /* Classic DOOM speeds: half stick = walk, full stick = run.
       forwardmove[] = {25, 50}, sidemove[] = {24, 40} */
    touch_fwd  = (int)lroundf(-analog_speed(sm_ay, 25.0f, 50.0f));
    touch_side = (int)lroundf( analog_speed(sm_ax, 24.0f, 40.0f));

    {
        float want = look_units + pad_rx * PAD_TURN_UNITS;
        int units;

        if (want >  LOOK_MAX_UNITS) want =  LOOK_MAX_UNITS;
        if (want < -LOOK_MAX_UNITS) want = -LOOK_MAX_UNITS;

        units = (int)lroundf(want / 8.0f);
        look_units = want - (float)units * 8.0f;

        if (units) {
            event_t e;
            e.type = ev_mouse; e.data1 = 0;
            e.data2 = units;
            e.data3 = 0;
            D_PostEvent(&e);
        }
    }
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

typedef struct { vtx_t *v; int n, max; } vb_t;

static void vtx(vb_t *b, float x, float y, float r, float g, float bl, float a)
{
    if (b->n < b->max) { vtx_t *p = &b->v[b->n++]; p->x=x; p->y=y; p->r=r; p->g=g; p->b=bl; p->a=a; }
}

static void rect(vb_t *b, float x0, float y0, float x1, float y1, float r, float g, float bl, float a)
{
    vtx(b,x0,y0,r,g,bl,a); vtx(b,x1,y0,r,g,bl,a); vtx(b,x0,y1,r,g,bl,a);
    vtx(b,x1,y0,r,g,bl,a); vtx(b,x1,y1,r,g,bl,a); vtx(b,x0,y1,r,g,bl,a);
}

#define SEG 28
static void disc(vb_t *b, float cx, float cy, float rad, float r, float g, float bl, float a)
{
    int i;
    for (i = 0; i < SEG; i++) {
        float a0 = i * 6.2831853f / SEG, a1 = (i + 1) * 6.2831853f / SEG;
        vtx(b, cx, cy, r,g,bl,a);
        vtx(b, cx + cosf(a0)*rad, cy + sinf(a0)*rad, r,g,bl,a);
        vtx(b, cx + cosf(a1)*rad, cy + sinf(a1)*rad, r,g,bl,a);
    }
}

static void ring(vb_t *b, float cx, float cy, float rad, float th, float r, float g, float bl, float a)
{
    int i;
    for (i = 0; i < SEG; i++) {
        float a0 = i * 6.2831853f / SEG, a1 = (i + 1) * 6.2831853f / SEG;
        float c0=cosf(a0), s0=sinf(a0), c1=cosf(a1), s1=sinf(a1), ri = rad - th;
        vtx(b, cx+c0*rad, cy+s0*rad, r,g,bl,a); vtx(b, cx+c1*rad, cy+s1*rad, r,g,bl,a);
        vtx(b, cx+c0*ri,  cy+s0*ri,  r,g,bl,a);
        vtx(b, cx+c1*rad, cy+s1*rad, r,g,bl,a); vtx(b, cx+c1*ri,  cy+s1*ri,  r,g,bl,a);
        vtx(b, cx+c0*ri,  cy+s0*ri,  r,g,bl,a);
    }
}

static const unsigned char font5x7[64][5] = {
{0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
{0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
{0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
{0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
{0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
{0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
{0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
{0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
{0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
{0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
};

static float text_width(const char *s, float sc)
{
    return (float)strlen(s) * 6.0f * sc - sc;
}

static void text(vb_t *b, const char *s, float x, float y, float sc,
                 float r, float g, float bl, float a, int center)
{
    if (center) x -= text_width(s, sc) * 0.5f;
    for (; *s; s++, x += 6.0f * sc) {
        int c = (unsigned char)*s, col, row;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c < 0x20 || c > 0x5F) c = '?';
        for (col = 0; col < 5; col++) {
            unsigned char bits = font5x7[c - 0x20][col];
            for (row = 0; row < 7; row++)
                if (bits & (1 << row))
                    rect(b, x + col*sc, y + row*sc, x + (col+1)*sc, y + (row+1)*sc, r, g, bl, a);
        }
    }
}

int text_build(vtx_t *out, int max, const char *s, float x, float y,
               float scale, float r, float g, float b, float a, int center)
{
    vb_t vb = { out, 0, max };
    text(&vb, s, x, y, scale, r, g, b, a, center);
    return vb.n;
}

int touch_build(vtx_t *out, int max)
{
    vb_t vb = { out, 0, max };
    int i, menu = menu_mode();

    {
        float bx = sox, by = soy, kx = scx, ky = scy;
        float a = 0.30f;
        if (stick_id < 0) {
            bx = kx = stick_r * 1.3f; by = ky = H - stick_r * 1.3f;
            a = 0.16f;
        }
        ring(&vb, bx, by, stick_r, stick_r*0.06f, 1,1,1, a);
        disc(&vb, kx, ky, stick_r*0.42f, 1,1,1, a + 0.08f);
    }

    for (i = 0; i < NBTN; i++) {
        btn_t *b = &btn[i];
        const char *l;
        float sc;
        if (!visible(b)) continue;
        l = menu ? b->lbl_menu : b->lbl_lvl;
        if (b->ptr >= 0) disc(&vb, b->cx, b->cy, b->r, 1,1,1, 0.32f);
        else             disc(&vb, b->cx, b->cy, b->r, 0,0,0, 0.22f);
        ring(&vb, b->cx, b->cy, b->r, b->r*0.07f, 1,1,1, 0.40f);
        sc = 1.3f * b->r / (6.0f * (float)strlen(l));
        if (sc > b->r * 0.1f) sc = b->r * 0.1f;
        if (sc < 1.0f) sc = 1.0f;
        text(&vb, l, b->cx, b->cy - 3.5f*sc, sc, 1,1,1, 0.85f, 1);
    }
    return vb.n;
}
