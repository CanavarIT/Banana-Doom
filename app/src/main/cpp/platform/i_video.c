/* Video output: EGL + OpenGL ES 2.0. DOOM's 320x200 8-bit screen is converted
 * to RGBA, uploaded as a texture and drawn 4:3 letterboxed, followed by the
 * on-screen touch controls. */
#include <string.h>
#include <stdint.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/native_window.h>
#include <android/log.h>

#include "doomdef.h"
#include "doomstat.h"
#include "v_video.h"
#include "i_video.h"
#include "plat.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "DOOM", __VA_ARGS__)

static EGLDisplay dpy = EGL_NO_DISPLAY;
static EGLSurface surf = EGL_NO_SURFACE;
static EGLContext ctx = EGL_NO_CONTEXT;
static int sw, sh;
static GLuint prog_tex, prog_col, tex;
static GLint loc_tex_res, loc_col_res;

static uint32_t pal32[256];
static uint32_t rgba[SCREENWIDTH * SCREENHEIGHT];
static vtx_t ovl[6000];

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

static GLuint link_prog(const char *vs, const char *fs)
{
    GLuint p = glCreateProgram();
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v); glAttachShader(p, f);
    glBindAttribLocation(p, 0, "aPos");
    glBindAttribLocation(p, 1, "aAttr");
    glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

static void gl_setup(void)
{
    static const char *vs_tex =
        "attribute vec2 aPos; attribute vec2 aAttr; varying vec2 vUV;\n"
        "void main(){ gl_Position = vec4(aPos, 0.0, 1.0); vUV = aAttr; }\n";
    static const char *fs_tex =
        "precision mediump float; uniform sampler2D uTex; varying vec2 vUV;\n"
        "void main(){ gl_FragColor = vec4(texture2D(uTex, vUV).rgb, 1.0); }\n";
    static const char *vs_col =
        "attribute vec2 aPos; attribute vec4 aAttr; uniform vec2 uRes; varying vec4 vCol;\n"
        "void main(){ gl_Position = vec4(aPos.x/uRes.x*2.0-1.0, 1.0-aPos.y/uRes.y*2.0, 0.0, 1.0);"
        " vCol = aAttr; }\n";
    static const char *fs_col =
        "precision mediump float; varying vec4 vCol;\n"
        "void main(){ gl_FragColor = vCol; }\n";

    prog_tex = link_prog(vs_tex, fs_tex);
    prog_col = link_prog(vs_col, fs_col);
    loc_col_res = glGetUniformLocation(prog_col, "uRes");
    (void)loc_tex_res;

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SCREENWIDTH, SCREENHEIGHT, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
}

int gl_attach(void *native_window)
{
    static const EGLint cfg_attr[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE };
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLConfig cfg; EGLint n = 0, fmt;

    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) return 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) return 0;
    eglGetConfigAttrib(dpy, cfg, EGL_NATIVE_VISUAL_ID, &fmt);
    ANativeWindow_setBuffersGeometry((ANativeWindow *)native_window, 0, 0, fmt);
    surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)native_window, NULL);
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT) return 0;
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) return 0;
    eglSwapInterval(dpy, 1);
    eglQuerySurface(dpy, surf, EGL_WIDTH, &sw);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &sh);
    gl_setup();
    touch_resize(sw, sh, 1.0f);
    LOG("GL ready %dx%d", sw, sh);
    return 1;
}

void gl_detach(void)
{
    if (dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
        if (surf != EGL_NO_SURFACE) eglDestroySurface(dpy, surf);
        eglTerminate(dpy);
    }
    dpy = EGL_NO_DISPLAY; surf = EGL_NO_SURFACE; ctx = EGL_NO_CONTEXT;
}

int gl_ready(void) { return surf != EGL_NO_SURFACE; }
void gl_surface_size(int *w, int *h) { *w = sw; *h = sh; }

static void check_size(void)
{
    EGLint w, h;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w != sw || h != sh) { sw = w; sh = h; touch_resize(sw, sh, 1.0f); }
}

static void draw_overlay(const vtx_t *v, int n)
{
    if (n <= 0) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog_col);
    glUniform2f(loc_col_res, (float)sw, (float)sh);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(vtx_t), &v->x);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(vtx_t), &v->r);
    glDrawArrays(GL_TRIANGLES, 0, n);
    glDisableVertexAttribArray(0); glDisableVertexAttribArray(1);
    glDisable(GL_BLEND);
}

static void draw_game(void)
{
    float vw = (float)sw, vh = vw * 3.0f / 4.0f, q[16];
    float nx, ny;
    if (vh > sh) { vh = (float)sh; vw = vh * 4.0f / 3.0f; }
    nx = vw / sw; ny = vh / sh;                   /* half-extent in NDC */
    /* x, y, u, v : strip order TL, TR, BL, BR */
    q[0]=-nx; q[1]= ny; q[2]=0; q[3]=0;
    q[4]= nx; q[5]= ny; q[6]=1; q[7]=0;
    q[8]=-nx; q[9]=-ny; q[10]=0; q[11]=1;
    q[12]=nx; q[13]=-ny; q[14]=1; q[15]=1;

    glUseProgram(prog_tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), q);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), q + 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0); glDisableVertexAttribArray(1);
}

/* ---- DOOM video interface ------------------------------------------ */

extern int plat_loop_started;
void I_InitGraphics(void) { plat_loop_started = 1; }
void I_ShutdownGraphics(void) { gl_detach(); }

void I_SetPalette(byte *palette)
{
    int i;
    for (i = 0; i < 256; i++) {
        uint32_t r = gammatable[usegamma][*palette++];
        uint32_t g = gammatable[usegamma][*palette++];
        uint32_t b = gammatable[usegamma][*palette++];
        pal32[i] = 0xff000000u | (b << 16) | (g << 8) | r;
    }
}

void I_UpdateNoBlit(void) {}
void I_StartFrame(void) {}

void I_ReadScreen(byte *scr)
{
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

void I_FinishUpdate(void)
{
    const byte *src = screens[0];
    int i, n;
    if (!gl_ready()) return;
    check_size();

    for (i = 0; i < SCREENWIDTH * SCREENHEIGHT; i++) rgba[i] = pal32[src[i]];

    glViewport(0, 0, sw, sh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SCREENWIDTH, SCREENHEIGHT,
                    GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    draw_game();
    n = touch_build(ovl, (int)(sizeof(ovl) / sizeof(ovl[0])));
    draw_overlay(ovl, n);
    eglSwapBuffers(dpy, surf);
}

void gl_draw_overlay_only(const vtx_t *v, int n)
{
    if (!gl_ready()) return;
    check_size();
    glViewport(0, 0, sw, sh);
    glClearColor(0.05f, 0.0f, 0.0f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_overlay(v, n);
    eglSwapBuffers(dpy, surf);
}
