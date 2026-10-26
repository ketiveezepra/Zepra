/*
 * NXAudio Output Test
 *
 * Writes WAV files, then checks:
 *   - the engine renders them at the output rate with the right length,
 *     pitch and level, and no seams at block boundaries or loop points
 *   - pause, volume and pan
 *   - the public API plays a WAV through an output device in real time
 *
 * CTest runs this with NXAUDIO_BACKEND=null. Run it by hand without that
 * to hear the last part (a short beep) on the real device.
 *
 * Copyright (c) 2025 KetiveeAI - KETIVEEAI License
 */

#include "nxaudio/nxaudio.h"
#include "dsp/engine.h"
#include "platform/nx_thread.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846

static int g_failures = 0;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
        g_failures++;                                           \
    }                                                           \
} while (0)

/* ============ WAV writer ============ */

static void put16(FILE *f, uint16_t v) { fputc(v & 0xFF, f); fputc(v >> 8, f); }
static void put32(FILE *f, uint32_t v) { put16(f, (uint16_t)(v & 0xFFFF)); put16(f, (uint16_t)(v >> 16)); }

/*
 * Sine of `freq` Hz at `amp`. Channel 1 (if any) is the negated sine.
 * bits: 16 or 24 (PCM) or 32 (float). An extra "LIST" chunk sits between
 * fmt and data, as real encoders write.
 */
static int write_wav(const char *path, uint32_t rate, uint16_t channels, uint16_t bits,
                     double freq, double seconds, double amp) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    uint32_t frames = (uint32_t)(seconds * rate + 0.5);
    uint32_t bytes_per_sample = bits / 8;
    uint32_t data_size = frames * channels * bytes_per_sample;
    const char list[] = "INFOtest";  /* 8 bytes */

    fwrite("RIFF", 1, 4, f);
    put32(f, 4 + (8 + 16) + (8 + 8) + (8 + data_size));
    fwrite("WAVE", 1, 4, f);

    fwrite("fmt ", 1, 4, f);
    put32(f, 16);
    put16(f, bits == 32 ? 3 : 1);
    put16(f, channels);
    put32(f, rate);
    put32(f, rate * channels * bytes_per_sample);
    put16(f, (uint16_t)(channels * bytes_per_sample));
    put16(f, bits);

    fwrite("LIST", 1, 4, f);
    put32(f, 8);
    fwrite(list, 1, 8, f);

    fwrite("data", 1, 4, f);
    put32(f, data_size);

    for (uint32_t i = 0; i < frames; i++) {
        double s = amp * sin(2.0 * PI * freq * i / rate);
        for (uint16_t c = 0; c < channels; c++) {
            double v = (c == 1) ? -s : s;
            if (bits == 16) {
                put16(f, (uint16_t)(int16_t)lrint(v * 32767.0));
            } else if (bits == 24) {
                int32_t x = (int32_t)lrint(v * 8388607.0);
                fputc(x & 0xFF, f); fputc((x >> 8) & 0xFF, f); fputc((x >> 16) & 0xFF, f);
            } else {
                float fv = (float)v;
                fwrite(&fv, sizeof(fv), 1, f);
            }
        }
    }

    fclose(f);
    return 0;
}

/* ============ Analysis helpers ============ */

typedef struct {
    float *left, *right;
    size_t frames;
} capture_t;

/* Render in uneven chunk sizes (as devices do) until the sound finishes or max_frames */
static capture_t render_all(nx_engine_t *engine, nx_sound_t sound, size_t max_frames) {
    static const size_t chunks[] = { 333, 4096, 1, 512, 1500, 64 };
    capture_t cap = {0};
    cap.left = (float*)calloc(max_frames, sizeof(float));
    cap.right = (float*)calloc(max_frames, sizeof(float));
    float *buf = (float*)malloc(4096 * 2 * sizeof(float));

    size_t ci = 0;
    while (cap.frames < max_frames) {
        size_t n = chunks[ci++ % (sizeof(chunks) / sizeof(chunks[0]))];
        if (n > max_frames - cap.frames) n = max_frames - cap.frames;

        nx_engine_render(engine, buf, n);
        for (size_t i = 0; i < n; i++) {
            cap.left[cap.frames + i] = buf[i * 2];
            cap.right[cap.frames + i] = buf[i * 2 + 1];
        }
        cap.frames += n;

        if (sound != NX_SOUND_INVALID && nx_engine_is_finished(engine, sound)) break;
    }
    free(buf);
    return cap;
}

static void capture_free(capture_t *cap) {
    free(cap->left);
    free(cap->right);
}

static size_t last_nonzero(const float *s, size_t n) {
    for (size_t i = n; i > 0; i--) {
        if (fabsf(s[i - 1]) > 1e-6f) return i;
    }
    return 0;
}

static float peak(const float *s, size_t from, size_t to) {
    float p = 0.0f;
    for (size_t i = from; i < to; i++) {
        if (fabsf(s[i]) > p) p = fabsf(s[i]);
    }
    return p;
}

static size_t zero_crossings(const float *s, size_t from, size_t to) {
    size_t n = 0;
    for (size_t i = from + 1; i < to; i++) {
        if ((s[i - 1] < 0.0f) != (s[i] < 0.0f)) n++;
    }
    return n;
}

static float max_step(const float *s, size_t from, size_t to) {
    float m = 0.0f;
    for (size_t i = from + 1; i < to; i++) {
        float d = fabsf(s[i] - s[i - 1]);
        if (d > m) m = d;
    }
    return m;
}

static int engine_48k(nx_engine_t *engine) {
    return nx_engine_create(engine) == NX_OK ? 0 : -1;
}

/* ============ Tests ============ */

/* 44.1 kHz mono 16-bit -> 48 kHz stereo: length, level, pitch, continuity */
static void test_resampled_mono(void) {
    printf("resampled mono (44.1k -> 48k)\n");
    const double secs = 0.5, freq = 440.0, amp = 0.5;
    if (write_wav("nxaudio_test_mono44.wav", 44100, 1, 16, freq, secs, amp) != 0) {
        CHECK(0, "cannot write WAV");
        return;
    }

    nx_engine_t engine;
    CHECK(engine_48k(&engine) == 0, "engine create");
    nx_sound_t s = nx_engine_load(&engine, "nxaudio_test_mono44.wav");
    CHECK(s != NX_SOUND_INVALID, "load");
    if (s == NX_SOUND_INVALID) { nx_engine_destroy(&engine); return; }

    CHECK(fabsf(nx_engine_get_duration(&engine, s) - (float)secs) < 0.001f, "duration %.4f",
          nx_engine_get_duration(&engine, s));

    nx_engine_play(&engine, s);
    capture_t cap = render_all(&engine, s, 48000);

    const size_t expect = (size_t)(secs * 48000);
    size_t len = last_nonzero(cap.left, cap.frames);
    CHECK(len + 8 >= expect && len <= expect + 8, "length %zu, expected ~%zu", len, expect);
    CHECK(nx_engine_is_finished(&engine, s), "sound should be finished");

    float p = peak(cap.left, 0, len);
    CHECK(p > 0.48f && p < 0.52f, "peak %.4f, expected ~0.5", p);

    int same = memcmp(cap.left, cap.right, cap.frames * sizeof(float)) == 0;
    CHECK(same, "mono source should play identically on both sides");

    size_t zc = zero_crossings(cap.left, 0, expect);
    size_t zc_expect = (size_t)(2 * freq * secs);
    CHECK(zc + 3 >= zc_expect && zc <= zc_expect + 3, "zero crossings %zu, expected ~%zu", zc, zc_expect);

    /* Largest step of a 440 Hz sine at 0.5 is 2*pi*440/48000*0.5 = 0.029 */
    float step = max_step(cap.left, 0, expect - 16);
    CHECK(step < 0.035f, "max step %.4f: discontinuity in the output", step);

    capture_free(&cap);
    nx_engine_destroy(&engine);
    remove("nxaudio_test_mono44.wav");
}

/* 48 kHz stereo at the output rate: no resampling, channels kept apart */
static void test_native_stereo(int bits) {
    printf("native stereo (48k, %d-bit)\n", bits);
    const double secs = 0.25;
    if (write_wav("nxaudio_test_st48.wav", 48000, 2, (uint16_t)bits, 1000.0, secs, 0.8) != 0) {
        CHECK(0, "cannot write WAV");
        return;
    }

    nx_engine_t engine;
    CHECK(engine_48k(&engine) == 0, "engine create");
    nx_sound_t s = nx_engine_load(&engine, "nxaudio_test_st48.wav");
    CHECK(s != NX_SOUND_INVALID, "load");
    if (s == NX_SOUND_INVALID) { nx_engine_destroy(&engine); return; }

    nx_engine_play(&engine, s);
    capture_t cap = render_all(&engine, s, 48000);

    size_t len = last_nonzero(cap.left, cap.frames);
    CHECK(len >= 11990 && len <= 12000, "length %zu, expected 12000", len);

    float worst = 0.0f;
    for (size_t i = 0; i < len; i++) {
        float d = fabsf(cap.left[i] + cap.right[i]);
        if (d > worst) worst = d;
    }
    CHECK(worst < 1e-3f, "right should be the negated left (worst %.5f)", worst);

    float p = peak(cap.left, 0, len);
    CHECK(p > 0.78f && p < 0.82f, "peak %.4f, expected ~0.8", p);

    capture_free(&cap);
    nx_engine_destroy(&engine);
    remove("nxaudio_test_st48.wav");
}

/* Looping: keeps playing past the end with no seam (44 whole cycles per loop) */
static void test_loop(void) {
    printf("loop\n");
    if (write_wav("nxaudio_test_loop.wav", 44100, 1, 16, 440.0, 0.1, 0.5) != 0) {
        CHECK(0, "cannot write WAV");
        return;
    }

    nx_engine_t engine;
    CHECK(engine_48k(&engine) == 0, "engine create");
    nx_sound_t s = nx_engine_load(&engine, "nxaudio_test_loop.wav");
    if (s == NX_SOUND_INVALID) { CHECK(0, "load"); nx_engine_destroy(&engine); return; }

    nx_engine_set_loop(&engine, s, 1);
    nx_engine_play(&engine, s);
    capture_t cap = render_all(&engine, s, 48000 * 35 / 100);

    CHECK(nx_engine_is_playing(&engine, s), "looping sound stopped");
    CHECK(cap.frames == 48000 * 35 / 100, "rendered %zu frames", cap.frames);

    float p = peak(cap.left, cap.frames - 2000, cap.frames);
    CHECK(p > 0.45f, "silent after looping (peak %.3f)", p);

    float step = max_step(cap.left, 0, cap.frames);
    CHECK(step < 0.035f, "max step %.4f: seam at the loop point", step);

    float pos = nx_engine_get_position(&engine, s);
    CHECK(pos >= 0.0f && pos < 0.1f, "position %.3f should wrap inside the 0.1 s loop", pos);

    capture_free(&cap);
    nx_engine_destroy(&engine);
    remove("nxaudio_test_loop.wav");
}

/* Pause, volume, pan, seek, restart after end */
static void test_controls(void) {
    printf("controls\n");
    if (write_wav("nxaudio_test_ctl.wav", 48000, 1, 16, 500.0, 1.0, 0.5) != 0) {
        CHECK(0, "cannot write WAV");
        return;
    }

    nx_engine_t engine;
    CHECK(engine_48k(&engine) == 0, "engine create");
    nx_sound_t s = nx_engine_load(&engine, "nxaudio_test_ctl.wav");
    if (s == NX_SOUND_INVALID) { CHECK(0, "load"); nx_engine_destroy(&engine); return; }

    nx_engine_play(&engine, s);
    capture_t a = render_all(&engine, NX_SOUND_INVALID, 4800);
    CHECK(peak(a.left, 0, a.frames) > 0.45f, "not playing");
    capture_free(&a);

    nx_engine_pause(&engine, s);
    float before = nx_engine_get_position(&engine, s);
    capture_t b = render_all(&engine, NX_SOUND_INVALID, 4800);
    CHECK(peak(b.left, 0, b.frames) == 0.0f, "paused sound is audible");
    CHECK(nx_engine_get_position(&engine, s) == before, "position moved while paused");
    CHECK(fabsf(before - 0.1f) < 0.002f, "position %.4f after 0.1 s", before);
    capture_free(&b);

    /* Volume ramps over one block, then holds */
    nx_engine_set_volume(&engine, s, 0.5f);
    nx_engine_play(&engine, s);
    capture_t c = render_all(&engine, NX_SOUND_INVALID, 9600);
    float pc = peak(c.left, 4800, c.frames);
    CHECK(pc > 0.23f && pc < 0.27f, "half volume peak %.4f", pc);
    capture_free(&c);

    /* Hard left: right side silent once the ramp is done */
    nx_engine_set_volume(&engine, s, 1.0f);
    nx_engine_set_pan(&engine, s, -1.0f);
    capture_t d = render_all(&engine, NX_SOUND_INVALID, 4800);
    capture_t e = render_all(&engine, NX_SOUND_INVALID, 4800);
    CHECK(peak(e.right, 0, e.frames) == 0.0f, "right audible when panned left");
    CHECK(peak(e.left, 0, e.frames) > 0.45f, "left too quiet when panned left");
    capture_free(&d);
    capture_free(&e);

    /* Seek near the end; it finishes, then play restarts from the top */
    nx_engine_set_pan(&engine, s, 0.0f);
    nx_engine_seek(&engine, s, 0.95f);
    CHECK(fabsf(nx_engine_get_position(&engine, s) - 0.95f) < 0.001f, "seek position");
    capture_t f = render_all(&engine, s, 48000);
    CHECK(nx_engine_is_finished(&engine, s), "should finish after seeking to 0.95 s");
    size_t len = last_nonzero(f.left, f.frames);
    CHECK(len >= 2390 && len <= 2410, "tail after seek %zu, expected ~2400", len);
    capture_free(&f);

    nx_engine_play(&engine, s);
    CHECK(nx_engine_is_playing(&engine, s), "play after end");
    CHECK(nx_engine_get_position(&engine, s) == 0.0f, "play after end should restart");

    nx_engine_destroy(&engine);
    remove("nxaudio_test_ctl.wav");
}

/* Public API end to end through an output device, in real time */
static void test_device_playback(void) {
    printf("device playback\n");
    const double secs = 0.3;
    if (write_wav("nxaudio_test_play.wav", 44100, 2, 24, 660.0, secs, 0.25) != 0) {
        CHECK(0, "cannot write WAV");
        return;
    }

    CHECK(nxaudio_init() == NXAUDIO_SUCCESS, "init");
    nxaudio_context_t ctx = nxaudio_context_create();
    CHECK(ctx != NXAUDIO_INVALID_HANDLE, "context create");
    if (ctx == NXAUDIO_INVALID_HANDLE) { nxaudio_shutdown(); return; }
    printf("  backend: %s\n", nxaudio_context_backend(ctx));

    CHECK(nxaudio_buffer_create("missing.wav") == NXAUDIO_INVALID_HANDLE, "missing file should fail");

    nxaudio_buffer_t buf = nxaudio_buffer_create("nxaudio_test_play.wav");
    CHECK(buf != NXAUDIO_INVALID_HANDLE, "buffer create");
    if (buf == NXAUDIO_INVALID_HANDLE) { nxaudio_shutdown(); return; }
    CHECK(fabsf(nxaudio_buffer_duration(buf) - (float)secs) < 0.001f, "duration %.4f",
          nxaudio_buffer_duration(buf));

    nxaudio_object_t obj = nxaudio_object_create(ctx, buf, NULL);
    CHECK(obj != NXAUDIO_INVALID_HANDLE, "object create");
    if (obj == NXAUDIO_INVALID_HANDLE) { nxaudio_shutdown(); return; }

    /* The object keeps the samples alive after the buffer handle goes */
    nxaudio_buffer_destroy(buf);
    CHECK(nxaudio_buffer_duration(buf) == 0.0f, "destroyed buffer still visible");

    uint64_t start = nx_time_ns();
    CHECK(nxaudio_object_play(obj) == NXAUDIO_SUCCESS, "play");

    float mid_time = -1.0f;
    while (nxaudio_object_is_playing(obj)) {
        double elapsed = (double)(nx_time_ns() - start) / 1e9;
        if (elapsed > 5.0) break;
        if (mid_time < 0.0f && elapsed > secs / 2) mid_time = nxaudio_object_get_time(obj);
        nx_sleep_ns(5 * 1000000ull);
    }
    double elapsed = (double)(nx_time_ns() - start) / 1e9;

    CHECK(!nxaudio_object_is_playing(obj), "still playing after 5 s");
    CHECK(elapsed > secs * 0.8 && elapsed < secs + 1.0, "played in %.3f s, expected ~%.2f", elapsed, secs);
    CHECK(mid_time > 0.0f && mid_time < (float)secs, "mid-playback time %.3f", mid_time);
    printf("  played %.2f s of audio in %.3f s\n", secs, elapsed);

    /* Suspend/resume releases and reopens the device */
    CHECK(nxaudio_context_suspend(ctx) == NXAUDIO_SUCCESS, "suspend");
    CHECK(nxaudio_context_resume(ctx) == NXAUDIO_SUCCESS, "resume");

    nxaudio_object_destroy(obj);
    nxaudio_context_destroy(ctx);
    nxaudio_shutdown();
    remove("nxaudio_test_play.wav");
}

int main(void) {
    test_resampled_mono();
    test_native_stereo(16);
    test_native_stereo(32);
    test_loop();
    test_controls();
    test_device_playback();

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
