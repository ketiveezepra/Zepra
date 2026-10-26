/*
 * NXAudio Null Device
 *
 * No hardware: pulls one period at a time and throws it away, paced to
 * the wall clock so playback takes as long as it would on a speaker.
 * Used on machines without sound (CI, servers) and when asked for with
 * NXAUDIO_BACKEND=null.
 *
 * Copyright (c) 2025 KetiveeAI - KETIVEEAI License
 */

#include "device.h"
#include "../platform/nx_thread.h"
#include <stdlib.h>

typedef struct {
    nx_thread_t thread;
    float      *buffer;
} null_device_t;

NX_THREAD_PROC(null_thread, arg) {
    nx_device_t *dev = (nx_device_t*)arg;
    null_device_t *nd = (null_device_t*)dev->impl;

    const uint64_t period_ns = (uint64_t)dev->period_frames * 1000000000ull / dev->sample_rate;
    uint64_t deadline = nx_time_ns();

    while (atomic_load(&dev->running)) {
        dev->callback(dev->user, nd->buffer, dev->period_frames);

        deadline += period_ns;
        uint64_t now = nx_time_ns();
        if (deadline > now) {
            nx_sleep_ns(deadline - now);
        } else if (now - deadline > 10 * period_ns) {
            deadline = now;  /* fell far behind (suspended?): don't race to catch up */
        }
    }
    NX_THREAD_RETURN;
}

static int null_start(nx_device_t *dev) {
    null_device_t *nd = (null_device_t*)calloc(1, sizeof(*nd));
    if (!nd) return -1;

    nd->buffer = (float*)malloc((size_t)dev->period_frames * dev->channels * sizeof(float));
    if (!nd->buffer) {
        free(nd);
        return -1;
    }

    dev->impl = nd;
    if (nx_thread_start(&nd->thread, null_thread, dev) != 0) {
        free(nd->buffer);
        free(nd);
        dev->impl = NULL;
        return -1;
    }
    return 0;
}

static void null_stop(nx_device_t *dev) {
    null_device_t *nd = (null_device_t*)dev->impl;
    if (!nd) return;
    nx_thread_join(nd->thread);
    free(nd->buffer);
    free(nd);
    dev->impl = NULL;
}

const nx_device_backend_t nx_backend_null = {
    .name  = "null",
    .start = null_start,
    .stop  = null_stop,
};
