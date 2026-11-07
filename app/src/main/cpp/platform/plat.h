/* Shared declarations between the Android platform modules. */
#ifndef PLAT_H
#define PLAT_H

typedef struct { float x, y, r, g, b, a; } vtx_t;

/* touch.c: on-screen controls (triangle list in pixel coordinates) */
void touch_init(void);
void touch_resize(int w, int h, float density);
void touch_down(int id, float x, float y);
void touch_move(int id, float x, float y);
void touch_up(int id);
void touch_cancel_all(void);
void touch_pad(float lx, float ly, float rx);   /* gamepad axes */
void touch_tick(int now_ms);                    /* posts Doom events */
int  touch_build(vtx_t *out, int max);          /* returns vertex count */
int  text_build(vtx_t *out, int max, const char *s, float x, float y,
                float scale, float r, float g, float b, float a, int center);

/* i_video.c */
int  gl_attach(void *native_window);            /* ANativeWindow* */
void gl_detach(void);
int  gl_ready(void);
void gl_surface_size(int *w, int *h);
void gl_draw_overlay_only(const vtx_t *v, int n); /* clear + overlay + swap */

/* textinput.c: system soft keyboard (savegame names) */
struct android_app;
void kbd_init(struct android_app *app);         /* once, from android_main */
void kbd_poll(void);                            /* every tic, game thread */

/* android_main.c */
void plat_pause_sound(int paused);

/* i_sound.c */
void snd_pause(int paused);

/* music.c */
int  mus_render(float *buf, int n);             /* mono, int16 range; 0 = silent */
void mus_set_rate(int hz);

#endif
