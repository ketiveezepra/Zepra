/*
 * NXAudio Output Device
 *
 * Opens the system's default playback device and pulls audio from a
 * callback on the backend's own thread:
 *   Linux   - PulseAudio (also PipeWire's pulse server), then ALSA
 *   Windows - WASAPI shared mode
 *   macOS   - CoreAudio default output unit
 *   any     - "null": no hardware, consumes audio in real time
 *
 * PulseAudio and ALSA are loaded with dlopen, so neither is needed to
 * build and a machine without them still runs (on the null device).
 *
 * The callback always gets interleaved float stereo at the rate asked
 * for; backends convert to whatever the hardware wants.
 *
 * Copyright (c) 2025 KetiveeAI - KETIVEEAI License
 */

#ifndef NXAUDIO_DEVICE_H
#define NXAUDIO_DEVICE_H

#include <stdint.h>
#include <stdatomic.h>

#define NX_DEVICE_CHANNELS  2

/* Fill `frames` frames of interleaved float into `out`. Runs on the audio thread. */
typedef void (*nx_device_callback_t)(void *user, float *out, uint32_t frames);

typedef struct nx_device nx_device_t;

typedef struct {
    const char *name;
    /* Open the hardware and start calling the callback. 0 on success. */
    int  (*start)(nx_device_t *dev);
    /* Stop calling the callback and release the hardware. */
    void (*stop)(nx_device_t *dev);
} nx_device_backend_t;

struct nx_device {
    const nx_device_backend_t *backend;
    uint32_t             sample_rate;
    uint32_t             channels;
    uint32_t             period_frames;
    nx_device_callback_t callback;
    void                *user;

    void                *impl;      /* backend state */
    atomic_int           running;
    int                  started;
};

/*
 * Open and start an output device.
 * backend: "pulse", "alsa", "wasapi", "coreaudio" or "null"; NULL picks the
 * first one that works (the NXAUDIO_BACKEND environment variable overrides).
 * Falls back to "null" when no real device opens, unless a backend was named.
 */
nx_device_t *nx_device_open(const char *backend,
                            uint32_t sample_rate,
                            uint32_t period_frames,
                            nx_device_callback_t callback,
                            void *user);

/* Pause/resume callbacks (releases the hardware while stopped). */
int  nx_device_start(nx_device_t *dev);
void nx_device_stop(nx_device_t *dev);

void nx_device_close(nx_device_t *dev);

const char *nx_device_backend_name(const nx_device_t *dev);

/* ============ Backends ============ */
extern const nx_device_backend_t nx_backend_null;
#if defined(__linux__)
extern const nx_device_backend_t nx_backend_pulse;
extern const nx_device_backend_t nx_backend_alsa;
#elif defined(_WIN32)
extern const nx_device_backend_t nx_backend_wasapi;
#elif defined(__APPLE__)
extern const nx_device_backend_t nx_backend_coreaudio;
#endif

#endif /* NXAUDIO_DEVICE_H */
