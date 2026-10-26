/*
 * NXAudio ALSA Device (Linux)
 *
 * Fallback for systems without a PulseAudio/PipeWire server. Opens the
 * "default" PCM with snd_pcm_set_params (ALSA resamples if the card
 * can't do our rate) and writes from a thread. libasound is loaded at
 * runtime, so there is no build dependency on it.
 *
 * Copyright (c) 2025 KetiveeAI - KETIVEEAI License
 */

#if defined(__linux__)

#include "device.h"
#include "../dsp/effects.h"
#include "../platform/nx_thread.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

/* ============ libasound ABI (alsa/pcm.h) ============ */

typedef struct _snd_pcm snd_pcm_t;
typedef unsigned long   nx_snd_uframes_t;
typedef long            nx_snd_sframes_t;

#define NX_SND_PCM_STREAM_PLAYBACK      0
#define NX_SND_PCM_ACCESS_RW_INTERLEAVED 3
#define NX_SND_PCM_FORMAT_S16_LE        2
#define NX_SND_PCM_FORMAT_FLOAT_LE      14

typedef int  (*snd_pcm_open_fn)(snd_pcm_t **pcm, const char *name, int stream, int mode);
typedef int  (*snd_pcm_set_params_fn)(snd_pcm_t *pcm, int format, int access,
                                      unsigned int channels, unsigned int rate,
                                      int soft_resample, unsigned int latency_us);
typedef nx_snd_sframes_t (*snd_pcm_writei_fn)(snd_pcm_t *pcm, const void *buf, nx_snd_uframes_t frames);
typedef int  (*snd_pcm_recover_fn)(snd_pcm_t *pcm, int err, int silent);
typedef int  (*snd_pcm_drop_fn)(snd_pcm_t *pcm);
typedef int  (*snd_pcm_close_fn)(snd_pcm_t *pcm);
typedef const char *(*snd_strerror_fn)(int err);

static struct {
    int                   tried;
    void                 *lib;
    snd_pcm_open_fn       pcm_open;
    snd_pcm_set_params_fn pcm_set_params;
    snd_pcm_writei_fn     pcm_writei;
    snd_pcm_recover_fn    pcm_recover;
    snd_pcm_drop_fn       pcm_drop;
    snd_pcm_close_fn      pcm_close;
    snd_strerror_fn       strerror_;
} g_alsa;

static int alsa_load(void) {
    if (g_alsa.tried) return g_alsa.lib ? 0 : -1;
    g_alsa.tried = 1;

    g_alsa.lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!g_alsa.lib) return -1;

    g_alsa.pcm_open       = (snd_pcm_open_fn)dlsym(g_alsa.lib, "snd_pcm_open");
    g_alsa.pcm_set_params = (snd_pcm_set_params_fn)dlsym(g_alsa.lib, "snd_pcm_set_params");
    g_alsa.pcm_writei     = (snd_pcm_writei_fn)dlsym(g_alsa.lib, "snd_pcm_writei");
    g_alsa.pcm_recover    = (snd_pcm_recover_fn)dlsym(g_alsa.lib, "snd_pcm_recover");
    g_alsa.pcm_drop       = (snd_pcm_drop_fn)dlsym(g_alsa.lib, "snd_pcm_drop");
    g_alsa.pcm_close      = (snd_pcm_close_fn)dlsym(g_alsa.lib, "snd_pcm_close");
    g_alsa.strerror_      = (snd_strerror_fn)dlsym(g_alsa.lib, "snd_strerror");

    if (!g_alsa.pcm_open || !g_alsa.pcm_set_params || !g_alsa.pcm_writei ||
        !g_alsa.pcm_recover || !g_alsa.pcm_drop || !g_alsa.pcm_close || !g_alsa.strerror_) {
        dlclose(g_alsa.lib);
        g_alsa.lib = NULL;
        return -1;
    }
    return 0;
}

/* ============ Device ============ */

typedef struct {
    snd_pcm_t  *pcm;
    int         use_s16;        /* card refused float */
    nx_thread_t thread;
    float      *buffer;
    int16_t    *buffer_s16;
} alsa_device_t;

NX_THREAD_PROC(alsa_thread, arg) {
    nx_device_t *dev = (nx_device_t*)arg;
    alsa_device_t *ad = (alsa_device_t*)dev->impl;
    const size_t samples = (size_t)dev->period_frames * dev->channels;

    while (atomic_load(&dev->running)) {
        dev->callback(dev->user, ad->buffer, dev->period_frames);

        const void *data = ad->buffer;
        if (ad->use_s16) {
            nx_dsp_float_to_s16(ad->buffer, ad->buffer_s16, samples);
            data = ad->buffer_s16;
        }

        nx_snd_uframes_t left = dev->period_frames;
        const size_t frame_bytes = dev->channels * (ad->use_s16 ? sizeof(int16_t) : sizeof(float));
        while (left > 0 && atomic_load(&dev->running)) {
            nx_snd_sframes_t n = g_alsa.pcm_writei(ad->pcm, data, left);
            if (n < 0) {
                n = g_alsa.pcm_recover(ad->pcm, (int)n, 1);
                if (n < 0) {
                    fprintf(stderr, "[NXAudio] ALSA write failed: %s\n", g_alsa.strerror_((int)n));
                    NX_THREAD_RETURN;
                }
                continue;
            }
            left -= (nx_snd_uframes_t)n;
            data = (const uint8_t*)data + (size_t)n * frame_bytes;
        }
    }
    NX_THREAD_RETURN;
}

static int alsa_start(nx_device_t *dev) {
    if (alsa_load() != 0) return -1;

    alsa_device_t *ad = (alsa_device_t*)calloc(1, sizeof(*ad));
    if (!ad) return -1;

    if (g_alsa.pcm_open(&ad->pcm, "default", NX_SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        free(ad);
        return -1;
    }

    const unsigned int latency_us =
        (unsigned int)((uint64_t)dev->period_frames * 4 * 1000000ull / dev->sample_rate);

    if (g_alsa.pcm_set_params(ad->pcm, NX_SND_PCM_FORMAT_FLOAT_LE, NX_SND_PCM_ACCESS_RW_INTERLEAVED,
                              dev->channels, dev->sample_rate, 1, latency_us) < 0) {
        ad->use_s16 = 1;
        if (g_alsa.pcm_set_params(ad->pcm, NX_SND_PCM_FORMAT_S16_LE, NX_SND_PCM_ACCESS_RW_INTERLEAVED,
                                  dev->channels, dev->sample_rate, 1, latency_us) < 0) {
            goto fail;
        }
    }

    const size_t samples = (size_t)dev->period_frames * dev->channels;
    ad->buffer = (float*)malloc(samples * sizeof(float));
    if (!ad->buffer) goto fail;
    if (ad->use_s16) {
        ad->buffer_s16 = (int16_t*)malloc(samples * sizeof(int16_t));
        if (!ad->buffer_s16) goto fail;
    }

    dev->impl = ad;
    if (nx_thread_start(&ad->thread, alsa_thread, dev) != 0) goto fail;
    return 0;

fail:
    g_alsa.pcm_close(ad->pcm);
    free(ad->buffer);
    free(ad->buffer_s16);
    free(ad);
    dev->impl = NULL;
    return -1;
}

static void alsa_stop(nx_device_t *dev) {
    alsa_device_t *ad = (alsa_device_t*)dev->impl;
    if (!ad) return;

    nx_thread_join(ad->thread);
    g_alsa.pcm_drop(ad->pcm);
    g_alsa.pcm_close(ad->pcm);
    free(ad->buffer);
    free(ad->buffer_s16);
    free(ad);
    dev->impl = NULL;
}

const nx_device_backend_t nx_backend_alsa = {
    .name  = "alsa",
    .start = alsa_start,
    .stop  = alsa_stop,
};

#endif /* __linux__ */
