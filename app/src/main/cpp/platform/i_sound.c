/* Sound effects: tiny software mixer driven by an AAudio callback.
 * Music (MUS sequencer + FM synth) lives in music.c and is mixed in here. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <aaudio/AAudio.h>
#include <android/log.h>

#include "doomdef.h"
#include "doomstat.h"
#include "i_system.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "plat.h"

#define NCHAN 16

typedef struct {
    const int16_t *data;
    uint32_t len;             /* samples */
    uint32_t pos;             /* 16.16 */
    uint32_t step;            /* 16.16 */
    int lvol, rvol;           /* 0..127 */
    int sfxid, handle, start;
    int active;               /* atomic */
} chan_t;

static chan_t chan[NCHAN];
static int16_t *sfxdata[NUMSFX];
static uint32_t sfxlen[NUMSFX], sfxrate[NUMSFX];
static int steptable[256];
static int outrate = 48000;
static int handlecounter = 100;

static AAudioStream *stream;
static volatile int need_reopen;

static void load_sfx(int id)
{
    char name[16];
    int lump, size, count, i;
    const unsigned char *raw;
    int16_t *pcm;

    snprintf(name, sizeof(name), "ds%s", S_sfx[id].name);
    lump = W_CheckNumForName(name);
    if (lump < 0) lump = W_GetNumForName("dspistol");
    size = W_LumpLength(lump);
    raw = (const unsigned char *)W_CacheLumpNum(lump, PU_CACHE);
    if (size < 8 + 32 + 1 || raw[0] != 3 || raw[1] != 0) {
        sfxdata[id] = (int16_t *)calloc(1, sizeof(int16_t));
        sfxlen[id] = 0; sfxrate[id] = 11025;
        return;
    }
    sfxrate[id] = raw[2] | (raw[3] << 8);
    count = (int)(raw[4] | (raw[5] << 8) | (raw[6] << 16) | ((unsigned)raw[7] << 24));
    if (count > size - 8) count = size - 8;
    count -= 32;                         /* 16 bytes of padding at each end */
    if (count < 1) count = 1;
    pcm = (int16_t *)malloc((size_t)(count + 1) * sizeof(int16_t));
    for (i = 0; i < count; i++) pcm[i] = (int16_t)(((int)raw[8 + 16 + i] - 128) << 8);
    pcm[count] = 0;
    sfxdata[id] = pcm;
    sfxlen[id] = (uint32_t)count;
    Z_ChangeTag((void *)raw, PU_CACHE);
}

static aaudio_data_callback_result_t audio_cb(AAudioStream *s, void *ud, void *audio, int32_t frames)
{
    int16_t *out = (int16_t *)audio;
    int done = 0, f, c;
    (void)s; (void)ud;

    while (done < frames) {
        float mus[256];
        int n = frames - done, have;
        if (n > 256) n = 256;
        have = mus_render(mus, n);

        for (f = 0; f < n; f++) {
            int l = 0, r = 0;
            for (c = 0; c < NCHAN; c++) {
                chan_t *ch = &chan[c];
                uint32_t idx;
                int a, b, frac, smp;
                if (!__atomic_load_n(&ch->active, __ATOMIC_ACQUIRE)) continue;
                idx = ch->pos >> 16;
                if (idx >= ch->len) { __atomic_store_n(&ch->active, 0, __ATOMIC_RELEASE); continue; }
                a = ch->data[idx];
                b = ch->data[idx + 1];                 /* buffer has 1 spare sample */
                frac = (int)(ch->pos & 0xffff);
                smp = a + (((b - a) * frac) >> 16);
                l += (smp * ch->lvol) >> 7;
                r += (smp * ch->rvol) >> 7;
                ch->pos += ch->step;
            }
            if (have) { int m = (int)mus[f]; l += m; r += m; }
            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
            out[2*(done + f)] = (int16_t)l; out[2*(done + f) + 1] = (int16_t)r;
        }
        done += n;
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void err_cb(AAudioStream *s, void *ud, aaudio_result_t err)
{
    (void)s; (void)ud;
    if (err == AAUDIO_ERROR_DISCONNECTED) need_reopen = 1;
}

static void open_stream(void)
{
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setDataCallback(b, audio_cb, NULL);
    AAudioStreamBuilder_setErrorCallback(b, err_cb, NULL);
    if (AAudioStreamBuilder_openStream(b, &stream) == AAUDIO_OK) {
        outrate = AAudioStream_getSampleRate(stream);
        if (outrate <= 0) outrate = 48000;
        mus_set_rate(outrate);
        AAudioStream_requestStart(stream);
    } else {
        stream = NULL;
    }
    AAudioStreamBuilder_delete(b);
}

static void close_stream(void)
{
    if (stream) { AAudioStream_requestStop(stream); AAudioStream_close(stream); stream = NULL; }
}

void I_InitSound(void)
{
    int i;
    for (i = 0; i < 256; i++) steptable[i] = (int)(exp2f((i - 128) / 64.0f) * 65536.0f);
    open_stream();
}

void I_ShutdownSound(void) { close_stream(); }

void snd_pause(int paused)
{
    if (!stream) return;
    if (paused) AAudioStream_requestPause(stream);
    else        AAudioStream_requestStart(stream);
}

void I_UpdateSound(void)
{
    if (need_reopen) { need_reopen = 0; close_stream(); open_stream(); }
}
void I_SubmitSound(void) {}
void I_SetChannels(void) {}
void I_SetSfxVolume(int volume) { snd_SfxVolume = volume; }

int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char name[16];
    int l;
    snprintf(name, sizeof(name), "ds%s", sfx->name);
    l = W_CheckNumForName(name);
    return l >= 0 ? l : W_GetNumForName("dspistol");
}

static void set_vols(chan_t *c, int vol, int sep)
{
    int left, right;
    sep += 1;
    left  = vol - ((vol * sep * sep) >> 16);
    sep   = sep - 257;
    right = vol - ((vol * sep * sep) >> 16);
    if (left < 0) left = 0; else if (left > 127) left = 127;
    if (right < 0) right = 0; else if (right > 127) right = 127;
    c->lvol = left; c->rvol = right;
}

int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
    int i, slot = -1, oldest = 0x7fffffff;
    chan_t *c;
    (void)priority;

    if (!stream || id <= 0 || id >= NUMSFX) return -1;
    if (!sfxdata[id]) load_sfx(id);
    if (sfxlen[id] == 0) return -1;

    /* chainsaw & co: one instance at a time */
    if (id == sfx_sawup || id == sfx_sawidl || id == sfx_sawful ||
        id == sfx_sawhit || id == sfx_stnmov || id == sfx_pistol)
        for (i = 0; i < NCHAN; i++)
            if (chan[i].active && chan[i].sfxid == id) {
                __atomic_store_n(&chan[i].active, 0, __ATOMIC_RELEASE);
                break;
            }

    for (i = 0; i < NCHAN; i++)
        if (!chan[i].active) { slot = i; break; }
    if (slot < 0)
        for (i = 0; i < NCHAN; i++)
            if (chan[i].start < oldest) { oldest = chan[i].start; slot = i; }

    c = &chan[slot];
    __atomic_store_n(&c->active, 0, __ATOMIC_RELEASE);
    c->data = sfxdata[id];
    c->len = sfxlen[id];
    c->pos = 0;
    c->step = (uint32_t)(((uint64_t)steptable[pitch & 255] * sfxrate[id]) / (uint32_t)outrate);
    c->sfxid = id;
    c->start = gametic;
    if (++handlecounter > 0x7ffff) handlecounter = 100;
    c->handle = (handlecounter << 5) | slot;
    set_vols(c, vol, sep);
    __atomic_store_n(&c->active, 1, __ATOMIC_RELEASE);
    return c->handle;
}

void I_StopSound(int handle)
{
    chan_t *c;
    if (handle < 0) return;
    c = &chan[handle & 31];
    if (c->handle == handle) __atomic_store_n(&c->active, 0, __ATOMIC_RELEASE);
}

int I_SoundIsPlaying(int handle)
{
    chan_t *c;
    if (handle < 0) return 0;
    c = &chan[handle & 31];
    return c->handle == handle && __atomic_load_n(&c->active, __ATOMIC_ACQUIRE);
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
    chan_t *c;
    (void)pitch;
    if (handle < 0) return;
    c = &chan[handle & 31];
    if (c->handle == handle) set_vols(c, vol, sep);
}
