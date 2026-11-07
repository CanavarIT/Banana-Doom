/*
 * Music for the Android port.
 *
 *   MUS sequencer (140 Hz)  ->  2-operator FM synth (OPL2 style)
 *
 * Instruments come from the GENMIDI lump that is already inside the WAD
 * (the same OPL patch bank the original DOS DOOM used for AdLib/SoundBlaster
 * music), so no extra files or soundfonts are needed.
 *
 * Everything is rendered inside the AAudio callback (see mus_render), the
 * game thread only touches the state under a mutex, and the audio thread
 * uses trylock so it can never block.
 */
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ANDROID__
#include <android/log.h>
#define MLOG(...) __android_log_print(ANDROID_LOG_INFO, "DOOM", __VA_ARGS__)
#else
#include <stdio.h>
#define MLOG(...) (printf(__VA_ARGS__), printf("\n"))
#endif

#include "doomdef.h"
#include "doomstat.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "plat.h"

/* ------------------------------------------------------------------ */
/* tuning                                                              */
/* ------------------------------------------------------------------ */

#define NVOICES      24
#define MUS_TICKRATE 140.0
#define MUS_MASTER   5000.0f   /* overall music loudness (int16 units)   */
#define MOD_CYC      4.0f      /* FM modulation depth, in cycles (8*pi)  */
#define WAVE_N       4096

/* ------------------------------------------------------------------ */
/* GENMIDI instrument bank                                             */
/* ------------------------------------------------------------------ */

typedef struct { uint8_t trem, attack, sustain, wave, scale, level; } opdef_t;
typedef struct { opdef_t mod; uint8_t feedback; opdef_t car; int16_t note_off; } vdef_t;
typedef struct { uint16_t flags; uint8_t fine, fixed; vdef_t v[2]; } inst_t;

#define GM_FIXED  0x0001
#define GM_DOUBLE 0x0004
#define NINST     175

static inst_t inst[NINST];
static int have_inst;

static void load_genmidi(void)
{
    int lump, len, i, v;
    const uint8_t *d;

    have_inst = 0;
    lump = W_CheckNumForName("GENMIDI");
    if (lump < 0) { MLOG("music: no GENMIDI lump, music disabled"); return; }
    len = W_LumpLength(lump);
    d = (const uint8_t *)W_CacheLumpNum(lump, PU_CACHE);
    if (len < 8 + NINST * 36 || memcmp(d, "#OPL_II#", 8) != 0) {
        MLOG("music: bad GENMIDI lump, music disabled");
        return;
    }
    d += 8;
    for (i = 0; i < NINST; i++, d += 36) {
        inst[i].flags = (uint16_t)(d[0] | (d[1] << 8));
        inst[i].fine  = d[2];
        inst[i].fixed = d[3];
        for (v = 0; v < 2; v++) {
            const uint8_t *q = d + 4 + v * 16;
            opdef_t *m = &inst[i].v[v].mod, *c = &inst[i].v[v].car;
            m->trem = q[0]; m->attack = q[1]; m->sustain = q[2];
            m->wave = q[3]; m->scale = q[4]; m->level = q[5];
            inst[i].v[v].feedback = q[6];
            c->trem = q[7]; c->attack = q[8]; c->sustain = q[9];
            c->wave = q[10]; c->scale = q[11]; c->level = q[12];
            inst[i].v[v].note_off = (int16_t)(q[14] | (q[15] << 8));
        }
    }
    have_inst = 1;
    MLOG("music: GENMIDI loaded");
}

/* ------------------------------------------------------------------ */
/* synth                                                               */
/* ------------------------------------------------------------------ */

enum { ST_ATT, ST_DEC, ST_SUS, ST_REL, ST_OFF };

typedef struct {
    float ph, inc, mult;
    float env, att, dmul, rmul, sus, lvl;
    const float *wave;
    int stage, sust, am, vib;
} op_t;

typedef struct {
    int active;                 /* 0 free, 1 held, 2 released */
    int chan, note, age, vel;
    int additive;
    float fbcyc, prev0, out0;
    float semi;                 /* pitch in semitones (MIDI note scale)   */
    float gain;
    op_t o[2];
} voice_t;

typedef struct {
    int prog, vol, expr;
    float bend;                 /* semitones */
    int lastvel;
} chan_t;

static float wavetab[4][WAVE_N];
static int tables_ready;
static const float multtab[16] = { 0.5f,1,2,3,4,5,6,7,8,9,10,10,12,12,15,15 };
static const float ksltab[4]   = { 0.0f, 3.0f, 1.5f, 6.0f };
static const float fbtab[8]    = { 0, 1/32.f, 1/16.f, 1/8.f, 1/4.f, 1/2.f, 1.f, 2.f };

static voice_t voice[NVOICES];
static chan_t  chans[16];
static int     voice_age;
static float   rate = 48000.0f;

static void build_tables(void)
{
    int i;
    if (tables_ready) return;
    for (i = 0; i < WAVE_N; i++) {
        float s = sinf((float)i * 6.2831853f / WAVE_N);
        float p = (float)(i % (WAVE_N / 2)) / (WAVE_N / 2);   /* position inside a half cycle */
        wavetab[0][i] = s;
        wavetab[1][i] = s > 0 ? s : 0;
        wavetab[2][i] = fabsf(s);
        wavetab[3][i] = (p < 0.5f) ? fabsf(s) : 0.0f;
    }
    tables_ready = 1;
}

static float rate_mul(int r)            /* per-sample multiplier, 96 dB over t */
{
    float t = 39.28f / exp2f((float)(r - 1));
    return powf(10.0f, -4.8f / (t * rate));
}

static void op_init(op_t *o, const opdef_t *d, float semi)
{
    int ar = d->attack >> 4, dr = d->attack & 15;
    int sl = d->sustain >> 4, rr = d->sustain & 15;
    int ksl = (d->scale >> 6) & 3;
    float db = 0.75f * (float)(d->level & 63);

    if (ksl) db += ksltab[ksl] * fmaxf(0.0f, (semi - 48.0f) / 12.0f);

    o->mult = multtab[d->trem & 15];
    o->sust = (d->trem >> 5) & 1;
    o->vib  = (d->trem >> 6) & 1;
    o->am   = (d->trem >> 7) & 1;
    o->wave = wavetab[d->wave & 3];
    o->lvl  = powf(10.0f, -db / 20.0f);

    o->ph = 0.0f;
    if (ar >= 15)      { o->att = 1.0f; o->env = 1.0f; o->stage = ST_DEC; }
    else if (ar == 0)  { o->att = 0.0f; o->env = 0.0f; o->stage = ST_ATT; }
    else {
        float ms = 2826.0f / exp2f((float)(ar - 1));
        o->att = 1.0f / (ms * 0.001f * rate);
        o->env = 0.0f; o->stage = ST_ATT;
    }
    o->dmul = dr ? rate_mul(dr) : 1.0f;
    o->rmul = rate_mul(rr ? rr : 3);          /* rate 0 would hang forever */
    o->sus  = powf(10.0f, -(sl == 15 ? 93.0f : 3.0f * (float)sl) / 20.0f);
}

static void voice_freq(voice_t *v)
{
    float s = v->semi + chans[v->chan].bend;
    float f = 440.0f * exp2f((s - 69.0f) / 12.0f);
    int i;
    for (i = 0; i < 2; i++) {
        float inc = v->o[i].mult * f / rate;
        v->o[i].inc = inc > 0.45f ? 0.45f : inc;
    }
}

static void voice_gain(voice_t *v)
{
    const chan_t *c = &chans[v->chan];
    float f = ((float)v->vel / 127.0f) * ((float)c->vol / 127.0f) * ((float)c->expr / 127.0f);
    v->gain = f * sqrtf(f);
}

static voice_t *alloc_voice(void)
{
    int i, best = -1;
    float bestv = 1e9f;

    for (i = 0; i < NVOICES; i++)
        if (!voice[i].active) return &voice[i];

    /* steal: quietest released voice, otherwise the oldest one */
    for (i = 0; i < NVOICES; i++)
        if (voice[i].active == 2 && voice[i].o[1].env < bestv) { bestv = voice[i].o[1].env; best = i; }
    if (best < 0) {
        int oldest = 0x7fffffff;
        for (i = 0; i < NVOICES; i++)
            if (voice[i].age < oldest) { oldest = voice[i].age; best = i; }
    }
    return &voice[best];
}

static void note_on(int ch, int note, int vel)
{
    const inst_t *in;
    int idx, nv, k;

    if (!have_inst) return;
    if (vel <= 0) return;

    if (ch == 15) {
        if (note < 35 || note > 81) return;
        idx = 128 + note - 35;
    } else {
        idx = chans[ch].prog & 127;
    }
    in = &inst[idx];
    nv = (in->flags & GM_DOUBLE) ? 2 : 1;

    for (k = 0; k < nv; k++) {
        const vdef_t *vd = &in->v[k];
        voice_t *v = alloc_voice();
        float semi;

        if (in->flags & GM_FIXED) semi = (float)in->fixed;
        else                      semi = (float)(note + vd->note_off);
        if (k == 1) semi += ((float)in->fine * 0.5f - 64.0f) / 64.0f;
        if (semi < 0) semi = 0;

        memset(v, 0, sizeof(*v));
        v->active = 1;
        v->chan = ch; v->note = note; v->vel = vel;
        v->age = ++voice_age;
        v->semi = semi;
        v->additive = vd->feedback & 1;
        v->fbcyc = fbtab[(vd->feedback >> 1) & 7];
        op_init(&v->o[0], &vd->mod, semi);
        op_init(&v->o[1], &vd->car, semi);
        voice_gain(v);
        voice_freq(v);
    }
}

static void voice_release(voice_t *v)
{
    int i;
    if (v->active != 1) return;
    v->active = 2;
    for (i = 0; i < 2; i++)
        if (v->o[i].stage != ST_OFF) v->o[i].stage = ST_REL;
}

static void note_off(int ch, int note)
{
    int i;
    for (i = 0; i < NVOICES; i++)
        if (voice[i].active == 1 && voice[i].chan == ch && voice[i].note == note)
            voice_release(&voice[i]);
}

static void chan_off(int ch)
{
    int i;
    for (i = 0; i < NVOICES; i++)
        if (voice[i].active == 1 && voice[i].chan == ch) voice_release(&voice[i]);
}

static void kill_all(void)
{
    int i;
    for (i = 0; i < NVOICES; i++) voice[i].active = 0;
}

static void reset_channels(void)
{
    int i;
    for (i = 0; i < 16; i++) {
        chans[i].prog = 0; chans[i].vol = 127; chans[i].expr = 127;
        chans[i].bend = 0.0f; chans[i].lastvel = 127;
    }
}

static void refresh_chan(int ch, int gain, int freq)
{
    int i;
    for (i = 0; i < NVOICES; i++)
        if (voice[i].active && voice[i].chan == ch) {
            if (gain) voice_gain(&voice[i]);
            if (freq) voice_freq(&voice[i]);
        }
}

static inline void env_step(op_t *o)
{
    switch (o->stage) {
    case ST_ATT:
        o->env += o->att;
        if (o->env >= 1.0f) { o->env = 1.0f; o->stage = ST_DEC; }
        break;
    case ST_DEC:
        o->env *= o->dmul;
        if (o->env <= o->sus) { o->env = o->sus; o->stage = ST_SUS; }
        break;
    case ST_SUS:
        if (!o->sust) {
            o->env *= o->rmul;
            if (o->env < 1e-5f) { o->env = 0.0f; o->stage = ST_OFF; }
        }
        break;
    case ST_REL:
        o->env *= o->rmul;
        if (o->env < 1e-5f) { o->env = 0.0f; o->stage = ST_OFF; }
        break;
    default:
        break;
    }
}

static inline float voice_sample(voice_t *v, float trem, float vib)
{
    op_t *m = &v->o[0], *c = &v->o[1];
    float fb, mo, co, out;
    int idx;

    env_step(m);
    env_step(c);

    /* modulator with self feedback */
    fb  = (v->prev0 + v->out0) * 0.5f * v->fbcyc;
    idx = (int)((m->ph + fb) * (float)WAVE_N) & (WAVE_N - 1);
    mo  = m->wave[idx] * m->env * m->lvl;
    if (m->am) mo *= trem;
    v->prev0 = v->out0;
    v->out0  = mo;
    m->ph += m->vib ? m->inc * vib : m->inc;
    m->ph -= (float)(int)m->ph;

    if (v->additive) {
        idx = (int)(c->ph * (float)WAVE_N) & (WAVE_N - 1);
        co  = c->wave[idx] * c->env * c->lvl;
        out = (mo + co) * v->gain;
    } else {
        idx = (int)((c->ph + mo * MOD_CYC) * (float)WAVE_N) & (WAVE_N - 1);
        co  = c->wave[idx] * c->env * c->lvl;
        out = co * v->gain;
    }
    if (c->am) out *= trem;
    c->ph += c->vib ? c->inc * vib : c->inc;
    c->ph -= (float)(int)c->ph;

    /* voice finished? */
    if (c->stage == ST_OFF && (!v->additive || m->stage == ST_OFF)) v->active = 0;
    return out;
}

/* ------------------------------------------------------------------ */
/* MUS sequencer                                                       */
/* ------------------------------------------------------------------ */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static uint8_t *song;
static int song_len, score_start, score_end, pos;
static int playing, looping, wait_ticks;
static volatile int mpaused;
static double tick_acc, samples_per_tick = 48000.0 / MUS_TICKRATE;
static volatile float music_gain = 0.5f;

static inline int rd(void) { return pos < score_end ? song[pos++] : 0; }

static void controller(int ch, int ctl, int val)
{
    chan_t *c = &chans[ch];
    switch (ctl) {
    case 0: c->prog = val & 127; break;
    case 3: c->vol  = val; refresh_chan(ch, 1, 0); break;
    case 5: c->expr = val; refresh_chan(ch, 1, 0); break;
    default: break;                 /* bank, mod, pan, reverb... ignored */
    }
}

static void seq_restart(void)
{
    kill_all();
    reset_channels();
    pos = score_start;
    wait_ticks = 0;
}

static void seq_tick(void)
{
    int guard = 0;

    if (wait_ticks > 0) wait_ticks--;

    while (playing && wait_ticks == 0) {
        int b, last, type, ch, n, d, x;

        if (pos >= score_end) type = 6, last = 0, ch = 0;
        else {
            b = rd(); last = b & 0x80; type = (b >> 4) & 7; ch = b & 15;
        }

        switch (type) {
        case 0:
            note_off(ch, rd() & 127);
            break;
        case 1:
            n = rd();
            if (n & 0x80) chans[ch].lastvel = rd() & 127;
            note_on(ch, n & 127, chans[ch].lastvel);
            break;
        case 2:
            x = rd();
            chans[ch].bend = ((float)x - 128.0f) / 128.0f * 2.0f;
            refresh_chan(ch, 0, 1);
            break;
        case 3:
            x = rd() & 127;
            if (x == 10 || x == 11) chan_off(ch);
            else if (x == 14) { chans[ch].expr = 127; chans[ch].bend = 0.0f; refresh_chan(ch, 1, 1); }
            break;
        case 4:
            n = rd(); x = rd();
            controller(ch, n, x);
            break;
        case 5:
            break;
        case 6:                                  /* score end */
            if (looping && ++guard < 3) { seq_restart(); continue; }
            playing = 0;
            { int i; for (i = 0; i < NVOICES; i++) voice_release(&voice[i]); }
            return;
        default:
            rd();
            break;
        }

        if (last) {
            d = 0;
            do { x = rd(); d = d * 128 + (x & 127); } while ((x & 0x80) && pos < score_end);
            wait_ticks = d;
        }
    }
}

/* ------------------------------------------------------------------ */
/* audio thread entry                                                  */
/* ------------------------------------------------------------------ */

/* Fills buf[0..n) with music (int16 range, mono). Returns 0 if silent. */
int mus_render(float *buf, int n)
{
    static float lfo_t, lfo_v;
    int i, k, any = 0;
    float g;

    if (pthread_mutex_trylock(&lock) != 0) return 0;

    if (mpaused) { pthread_mutex_unlock(&lock); return 0; }

    for (k = 0; k < NVOICES; k++) if (voice[k].active) { any = 1; break; }
    if (!playing && !any) { pthread_mutex_unlock(&lock); return 0; }

    g = music_gain * MUS_MASTER;

    for (i = 0; i < n; i++) {
        float tri, trem, vib, s = 0.0f;

        if (playing) {
            tick_acc += 1.0;
            while (tick_acc >= samples_per_tick) { tick_acc -= samples_per_tick; seq_tick(); }
        }

        lfo_t += 3.7f / rate;  if (lfo_t >= 1.0f) lfo_t -= 1.0f;
        lfo_v += 6.1f / rate;  if (lfo_v >= 1.0f) lfo_v -= 1.0f;
        tri  = lfo_t < 0.5f ? lfo_t * 2.0f : 2.0f - lfo_t * 2.0f;         /* 0..1  */
        trem = 1.0f - 0.11f * tri;                                         /* ~1 dB */
        tri  = lfo_v < 0.5f ? lfo_v * 4.0f - 1.0f : 3.0f - lfo_v * 4.0f;  /* -1..1 */
        vib  = 1.0f + 0.004f * tri;                                        /* ~7 ct */

        for (k = 0; k < NVOICES; k++)
            if (voice[k].active) s += voice_sample(&voice[k], trem, vib);

        s *= g;
        if (s > 30000.0f) s = 30000.0f; else if (s < -30000.0f) s = -30000.0f;
        buf[i] = s;
    }

    pthread_mutex_unlock(&lock);
    return 1;
}

void mus_set_rate(int r)
{
    build_tables();
    pthread_mutex_lock(&lock);
    rate = (float)(r > 8000 ? r : 48000);
    samples_per_tick = (double)rate / MUS_TICKRATE;
    pthread_mutex_unlock(&lock);
}

/* ------------------------------------------------------------------ */
/* DOOM sound interface (music half)                                   */
/* ------------------------------------------------------------------ */

void I_InitMusic(void)
{
    build_tables();
    reset_channels();
    load_genmidi();
}

void I_ShutdownMusic(void)
{
    pthread_mutex_lock(&lock);
    playing = 0;
    kill_all();
    pthread_mutex_unlock(&lock);
}

void I_SetMusicVolume(int volume)
{
    float g;
    snd_MusicVolume = volume;
    g = volume <= 15 ? (float)volume / 15.0f : (float)volume / 127.0f;
    if (g > 1.0f) g = 1.0f;
    if (g < 0.0f) g = 0.0f;
    music_gain = g;
}

void I_PauseSong(int handle)  { (void)handle; mpaused = 1; }
void I_ResumeSong(int handle) { (void)handle; mpaused = 0; }

int I_RegisterSong(void *data)
{
    const uint8_t *d = (const uint8_t *)data;
    int len, start, total;

    if (!data || !have_inst || memcmp(d, "MUS\x1a", 4) != 0) return 0;
    len   = d[4] | (d[5] << 8);
    start = d[6] | (d[7] << 8);
    total = start + len;

    pthread_mutex_lock(&lock);
    playing = 0;
    kill_all();
    free(song);
    song = (uint8_t *)malloc((size_t)total);
    if (song) {
        memcpy(song, d, (size_t)total);
        song_len = total; score_start = start; score_end = total;
    }
    pthread_mutex_unlock(&lock);
    return song ? 1 : 0;
}

void I_PlaySong(int handle, int loop)
{
    (void)handle;
    pthread_mutex_lock(&lock);
    if (song) {
        kill_all();
        reset_channels();
        pos = score_start;
        wait_ticks = 0;
        tick_acc = 0.0;
        looping = loop;
        mpaused = 0;
        playing = 1;
    }
    pthread_mutex_unlock(&lock);
}

void I_StopSong(int handle)
{
    (void)handle;
    pthread_mutex_lock(&lock);
    playing = 0;
    kill_all();
    pthread_mutex_unlock(&lock);
}

void I_UnRegisterSong(int handle)
{
    (void)handle;
    pthread_mutex_lock(&lock);
    playing = 0;
    kill_all();
    free(song);
    song = NULL; song_len = 0; score_start = score_end = 0;
    pthread_mutex_unlock(&lock);
}
