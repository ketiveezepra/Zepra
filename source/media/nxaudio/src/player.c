/*
 * NXAudio Player - play an audio file through the default output
 *
 * Usage: nxaudio_play <file.wav> [--loop]
 * Set NXAUDIO_BACKEND=pulse|alsa|wasapi|coreaudio|null to pick the output.
 *
 * Copyright (c) 2025 KetiveeAI - KETIVEEAI License
 */

#include "nxaudio/nxaudio.h"
#include "platform/nx_thread.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file.wav> [--loop]\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    int loop = argc > 2 && strcmp(argv[2], "--loop") == 0;

    if (nxaudio_init() != NXAUDIO_SUCCESS) {
        fprintf(stderr, "nxaudio_init failed\n");
        return 1;
    }

    nxaudio_context_t ctx = nxaudio_context_create();
    if (ctx == NXAUDIO_INVALID_HANDLE) {
        fprintf(stderr, "No audio output\n");
        nxaudio_shutdown();
        return 1;
    }

    nxaudio_buffer_t buf = nxaudio_buffer_create(path);
    if (buf == NXAUDIO_INVALID_HANDLE) {
        fprintf(stderr, "Cannot load %s\n", path);
        nxaudio_shutdown();
        return 1;
    }

    nxaudio_object_config_t config = {
        .gain = 1.0f,
        .pitch = 1.0f,
        .looping = (uint8_t)loop,
    };
    nxaudio_object_t obj = nxaudio_object_create(ctx, buf, &config);
    if (obj == NXAUDIO_INVALID_HANDLE) {
        fprintf(stderr, "Cannot create audio object\n");
        nxaudio_shutdown();
        return 1;
    }

    float duration = nxaudio_buffer_duration(buf);
    printf("Playing %s (%.2f s) via %s%s\n", path, duration,
           nxaudio_context_backend(ctx), loop ? ", looping (Ctrl+C to stop)" : "");

    nxaudio_object_play(obj);
    while (nxaudio_object_is_playing(obj)) {
        printf("\r  %6.2f / %.2f s", nxaudio_object_get_time(obj), duration);
        fflush(stdout);
        nx_sleep_ns(100 * 1000000ull);
    }
    printf("\r  %6.2f / %.2f s\n", duration, duration);

    nxaudio_object_destroy(obj);
    nxaudio_buffer_destroy(buf);
    nxaudio_context_destroy(ctx);
    nxaudio_shutdown();
    return 0;
}
