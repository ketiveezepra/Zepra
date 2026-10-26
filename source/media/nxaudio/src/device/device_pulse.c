/*
 * NXAudio PulseAudio Device (Linux)
 *
 * Uses the blocking pa_simple API from a writer thread. libpulse-simple
 * is loaded at runtime, so there is no build dependency on it. PipeWire
 * desktops serve this through pipewire-pulse.
 *
 * Copyright (c) 2025 KetiveeAI - KETIVEEAI License
 */

#if defined(__linux__)

#include "device.h"
#include "../platform/nx_thread.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

/* ============ libpulse-simple ABI (pulse/simple.h, pulse/sample.h) ============ */

typedef struct pa_simple pa_simple;

typedef struct {
    int      format;        /* pa_sample_format_t */
    uint32_t rate;
    uint8_t  channels;
} nx_pa_sample_spec;

typedef struct {
    uint32_t maxlength;
    uint32_t tlength;
    uint32_t prebuf;
    uint32_t minreq;
    uint32_t fragsize;
} nx_pa_buffer_attr;

#define NX_PA_STREAM_PLAYBACK   1
#define NX_PA_SAMPLE_FLOAT32LE  5

typedef pa_simple *(*pa_simple_new_fn)(const char *server, const char *name, int dir,
                                       const char *dev, const char *stream_name,
                                       const nx_pa_sample_spec *ss, const void *map,
                                       const nx_pa_buffer_attr *attr, int *error);
typedef int  (*pa_simple_write_fn)(pa_simple *s, const void *data, size_t bytes, int *error);
typedef int  (*pa_simple_flush_fn)(pa_simple *s, int *error);
typedef void (*pa_simple_free_fn)(pa_simple *s);

static struct {
    int                 tried;
    void               *lib;
    pa_simple_new_fn    simple_new;
    pa_simple_write_fn  simple_write;
    pa_simple_flush_fn  simple_flush;
    pa_simple_free_fn   simple_free;
} g_pa;

static int pa_load(void) {
    if (g_pa.tried) return g_pa.lib ? 0 : -1;
    g_pa.tried = 1;

    g_pa.lib = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!g_pa.lib) return -1;

    g_pa.simple_new   = (pa_simple_new_fn)dlsym(g_pa.lib, "pa_simple_new");
    g_pa.simple_write = (pa_simple_write_fn)dlsym(g_pa.lib, "pa_simple_write");
    g_pa.simple_flush = (pa_simple_flush_fn)dlsym(g_pa.lib, "pa_simple_flush");
    g_pa.simple_free  = (pa_simple_free_fn)dlsym(g_pa.lib, "pa_simple_free");

    if (!g_pa.simple_new || !g_pa.simple_write || !g_pa.simple_flush || !g_pa.simple_free) {
        dlclose(g_pa.lib);
        g_pa.lib = NULL;
        return -1;
    }
    return 0;
}

/* ============ Device ============ */

typedef struct {
    pa_simple  *stream;
    nx_thread_t thread;
    float      *buffer;
} pulse_device_t;

NX_THREAD_PROC(pulse_thread, arg) {
    nx_device_t *dev = (nx_device_t*)arg;
    pulse_device_t *pd = (pulse_device_t*)dev->impl;
    const size_t bytes = (size_t)dev->period_frames * dev->channels * sizeof(float);

    while (atomic_load(&dev->running)) {
        dev->callback(dev->user, pd->buffer, dev->period_frames);

        int err = 0;
        if (g_pa.simple_write(pd->stream, pd->buffer, bytes, &err) < 0) {
            fprintf(stderr, "[NXAudio] PulseAudio write failed (%d)\n", err);
            break;
        }
    }
    NX_THREAD_RETURN;
}

static int pulse_start(nx_device_t *dev) {
    if (pa_load() != 0) return -1;

    pulse_device_t *pd = (pulse_device_t*)calloc(1, sizeof(*pd));
    if (!pd) return -1;

    const uint32_t frame_bytes = dev->channels * sizeof(float);
    nx_pa_sample_spec spec = {
        .format = NX_PA_SAMPLE_FLOAT32LE,
        .rate = dev->sample_rate,
        .channels = (uint8_t)dev->channels,
    };
    /* About four periods queued in the server: ~40 ms at 512 frames / 48 kHz */
    nx_pa_buffer_attr attr = {
        .maxlength = (uint32_t)-1,
        .tlength   = dev->period_frames * 4 * frame_bytes,
        .prebuf    = (uint32_t)-1,
        .minreq    = dev->period_frames * frame_bytes,
        .fragsize  = (uint32_t)-1,
    };

    int err = 0;
    pd->stream = g_pa.simple_new(NULL, "Zepra", NX_PA_STREAM_PLAYBACK, NULL,
                                 "Playback", &spec, NULL, &attr, &err);
    if (!pd->stream) {
        free(pd);
        return -1;
    }

    pd->buffer = (float*)malloc((size_t)dev->period_frames * frame_bytes);
    if (!pd->buffer) goto fail;

    dev->impl = pd;
    if (nx_thread_start(&pd->thread, pulse_thread, dev) != 0) goto fail;
    return 0;

fail:
    g_pa.simple_free(pd->stream);
    free(pd->buffer);
    free(pd);
    dev->impl = NULL;
    return -1;
}

static void pulse_stop(nx_device_t *dev) {
    pulse_device_t *pd = (pulse_device_t*)dev->impl;
    if (!pd) return;

    nx_thread_join(pd->thread);

    int err = 0;
    g_pa.simple_flush(pd->stream, &err);
    g_pa.simple_free(pd->stream);
    free(pd->buffer);
    free(pd);
    dev->impl = NULL;
}

const nx_device_backend_t nx_backend_pulse = {
    .name  = "pulse",
    .start = pulse_start,
    .stop  = pulse_stop,
};

#endif /* __linux__ */
