/*
 * NXAudio Output Device - backend selection
 *
 * Copyright (c) 2025 KetiveeAI - KETIVEEAI License
 */

#include "device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const nx_device_backend_t *const g_backends[] = {
#if defined(__linux__)
    &nx_backend_pulse,
    &nx_backend_alsa,
#elif defined(_WIN32)
    &nx_backend_wasapi,
#elif defined(__APPLE__)
    &nx_backend_coreaudio,
#endif
    &nx_backend_null,
};

#define NUM_BACKENDS (sizeof(g_backends) / sizeof(g_backends[0]))

static const nx_device_backend_t *find_backend(const char *name) {
    for (size_t i = 0; i < NUM_BACKENDS; i++) {
        if (strcmp(g_backends[i]->name, name) == 0) return g_backends[i];
    }
    return NULL;
}

static int try_backend(nx_device_t *dev, const nx_device_backend_t *backend) {
    dev->backend = backend;
    dev->impl = NULL;
    atomic_store(&dev->running, 1);
    if (backend->start(dev) != 0) {
        atomic_store(&dev->running, 0);
        dev->backend = NULL;
        return -1;
    }
    dev->started = 1;
    return 0;
}

nx_device_t *nx_device_open(const char *backend,
                            uint32_t sample_rate,
                            uint32_t period_frames,
                            nx_device_callback_t callback,
                            void *user) {
    if (!callback || sample_rate == 0) return NULL;

    nx_device_t *dev = (nx_device_t*)calloc(1, sizeof(*dev));
    if (!dev) return NULL;

    dev->sample_rate = sample_rate;
    dev->channels = NX_DEVICE_CHANNELS;
    dev->period_frames = period_frames ? period_frames : 512;
    dev->callback = callback;
    dev->user = user;

    if (!backend || !*backend) {
        backend = getenv("NXAUDIO_BACKEND");
        if (backend && !*backend) backend = NULL;
    }

    if (backend) {
        const nx_device_backend_t *b = find_backend(backend);
        if (!b) {
            fprintf(stderr, "[NXAudio] Unknown backend '%s'\n", backend);
        } else if (try_backend(dev, b) == 0) {
            return dev;
        }
        free(dev);
        return NULL;
    }

    for (size_t i = 0; i < NUM_BACKENDS; i++) {
        if (try_backend(dev, g_backends[i]) == 0) {
            if (g_backends[i] == &nx_backend_null) {
                fprintf(stderr, "[NXAudio] No audio device; playing to the null device\n");
            }
            return dev;
        }
    }

    free(dev);
    return NULL;
}

int nx_device_start(nx_device_t *dev) {
    if (!dev || !dev->backend) return -1;
    if (dev->started) return 0;
    atomic_store(&dev->running, 1);
    if (dev->backend->start(dev) != 0) {
        atomic_store(&dev->running, 0);
        return -1;
    }
    dev->started = 1;
    return 0;
}

void nx_device_stop(nx_device_t *dev) {
    if (!dev || !dev->started) return;
    atomic_store(&dev->running, 0);
    dev->backend->stop(dev);
    dev->started = 0;
}

void nx_device_close(nx_device_t *dev) {
    if (!dev) return;
    nx_device_stop(dev);
    free(dev);
}

const char *nx_device_backend_name(const nx_device_t *dev) {
    return (dev && dev->backend) ? dev->backend->name : "none";
}
