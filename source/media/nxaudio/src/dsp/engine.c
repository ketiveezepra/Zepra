/*
 * NXAudio Engine Core Implementation
 *
 * decoder → stream → resampler → pending FIFO → gain/pan mix → master → device
 *
 * Each sound decodes in fixed chunks at its own rate, resamples into a
 * per-sound FIFO at the output rate, and the mixer takes exactly what the
 * device asked for. The resampler keeps its state between chunks, so
 * there are no seams at block boundaries or loop points.
 *
 * Copyright (c) 2025 KetiveeAI - KETIVEEAI License
 */

#include "engine.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_SOUNDS          64
#define DECODE_CHUNK        1024    /* source frames per decoder read */
#define DEFAULT_CHUNK       1024    /* output frames mixed per pass */

/* ============ Helpers ============ */

static nx_sound_state_t* get_sound(nx_engine_t *engine, nx_sound_t id) {
    if (!engine || id < 0 || id >= engine->max_sounds) return NULL;

    nx_sound_state_t *s = &engine->sounds[id];
    return s->active ? s : NULL;
}

static int find_free_slot(nx_engine_t *engine) {
    for (int i = 0; i < engine->max_sounds; i++) {
        if (!engine->sounds[i].active) return i;
    }
    return -1;
}

static void target_gains(const nx_sound_state_t *sound, float *left, float *right) {
    /* Balance: centre is unity on both sides, like an unpanned <audio> */
    *left  = sound->volume * (sound->pan > 0.0f ? 1.0f - sound->pan : 1.0f);
    *right = sound->volume * (sound->pan < 0.0f ? 1.0f + sound->pan : 1.0f);
}

static void reset_playback(nx_sound_state_t *sound, size_t frame) {
    nx_stream_seek(&sound->stream, frame);
    if (sound->needs_resample) nx_resample_reset(&sound->resampler);
    sound->pending_frames = 0;
    sound->source_done = 0;
}

/* Called once the stream is open in `slot`: sets up resampling and buffers */
static nx_sound_t setup_sound(nx_engine_t *engine, int slot) {
    nx_sound_state_t *sound = &engine->sounds[slot];
    uint32_t in_rate = sound->stream.sample_rate;
    uint32_t out_rate = engine->config.output_sample_rate;
    uint16_t channels = sound->stream.channels;

    sound->needs_resample = (in_rate != out_rate);
    if (sound->needs_resample) {
        if (nx_resample_init(&sound->resampler, in_rate, out_rate, channels,
                             engine->config.resample_quality) != 0) {
            nx_stream_close(&sound->stream);
            return NX_SOUND_INVALID;
        }
    }

    /* Effects chain (filters); gain and pan are applied while mixing */
    nx_effects_init(&sound->effects, (float)out_rate);

    /* Room for one mix pass plus everything a single decoder read can yield */
    size_t per_read = (size_t)((double)DECODE_CHUNK * out_rate / in_rate) + 64;
    sound->pending_capacity = engine->output_frames + per_read;

    sound->decode_buffer = (float*)malloc((size_t)DECODE_CHUNK * channels * sizeof(float));
    sound->pending = (float*)malloc(sound->pending_capacity * channels * sizeof(float));

    if (!sound->decode_buffer || !sound->pending) {
        free(sound->decode_buffer);
        free(sound->pending);
        nx_resample_destroy(&sound->resampler);
        nx_stream_close(&sound->stream);
        memset(sound, 0, sizeof(*sound));
        return NX_SOUND_INVALID;
    }

    sound->active = 1;
    sound->volume = 1.0f;
    sound->pan = 0.0f;
    sound->mixer_channel = -1;
    sound->applied_gain[0] = sound->applied_gain[1] = 1.0f;

    engine->active_sounds++;
    return slot;
}

/* Append a block of source-rate frames to the pending FIFO */
static void push_source(nx_sound_state_t *sound, const float *src, size_t frames) {
    uint16_t ch = sound->stream.channels;
    float *dst = sound->pending + sound->pending_frames * ch;
    size_t room = sound->pending_capacity - sound->pending_frames;

    if (sound->needs_resample) {
        sound->pending_frames += nx_resample_process(&sound->resampler, src, frames, dst, room);
    } else {
        size_t n = frames < room ? frames : room;
        memcpy(dst, src, n * ch * sizeof(float));
        sound->pending_frames += n;
    }
}

/* Decode until `need` output frames are pending or the source ends */
static void fill_pending(nx_sound_state_t *sound, size_t need) {
    int rewound = 0;

    while (sound->pending_frames < need && !sound->source_done) {
        size_t got = DECODE_CHUNK;
        nx_error_t err = nx_stream_read(&sound->stream, sound->decode_buffer, &got);

        if (err != NX_OK || got == 0) {
            if (sound->looping && err == NX_ERR_EOF && !rewound && sound->stream.total_frames > 0) {
                nx_stream_seek(&sound->stream, 0);
                rewound = 1;
                continue;
            }

            /* Push the resampler's held-back tail out with silence */
            if (sound->needs_resample) {
                size_t tail = nx_resample_latency(&sound->resampler) + 1;
                memset(sound->decode_buffer, 0, tail * sound->stream.channels * sizeof(float));
                push_source(sound, sound->decode_buffer, tail);
            }
            sound->source_done = 1;
            break;
        }

        rewound = 0;
        push_source(sound, sound->decode_buffer, got);
    }
}

/* Mix up to `frames` pending frames into `out`, ramping gain changes */
static void mix_sound(nx_engine_t *engine, nx_sound_state_t *sound, float *out, size_t frames) {
    size_t n = sound->pending_frames < frames ? sound->pending_frames : frames;
    if (n == 0) return;

    uint16_t sc = sound->stream.channels;
    uint32_t oc = engine->config.output_channels;

    float tl, tr;
    target_gains(sound, &tl, &tr);
    float gl = sound->applied_gain[0], gr = sound->applied_gain[1];
    float dl = (tl - gl) / (float)n, dr = (tr - gr) / (float)n;

    const float *p = sound->pending;
    for (size_t i = 0; i < n; i++) {
        gl += dl;
        gr += dr;

        float l = p[i * sc];
        float r = (sc > 1) ? p[i * sc + 1] : l;

        if (oc == 1) {
            out[i] += 0.5f * (l * gl + r * gr);
        } else {
            out[i * oc]     += l * gl;
            out[i * oc + 1] += r * gr;
        }
    }

    sound->applied_gain[0] = tl;
    sound->applied_gain[1] = tr;

    sound->pending_frames -= n;
    if (sound->pending_frames > 0) {
        memmove(sound->pending, sound->pending + n * sc, sound->pending_frames * sc * sizeof(float));
    }
}

/* ============ Initialization ============ */

nx_error_t nx_engine_create(nx_engine_t *engine) {
    nx_engine_config_t config = {
        .output_sample_rate = 48000,
        .output_channels = 2,
        .buffer_frames = DEFAULT_CHUNK,
        .resample_quality = NX_RESAMPLE_MEDIUM,
    };
    return nx_engine_create_ex(engine, &config);
}

nx_error_t nx_engine_create_ex(nx_engine_t *engine, const nx_engine_config_t *config) {
    if (!engine || !config) return NX_ERR_INVALID;
    if (config->output_sample_rate == 0 || config->output_channels == 0) return NX_ERR_INVALID;

    memset(engine, 0, sizeof(*engine));
    engine->config = *config;
    engine->max_sounds = MAX_SOUNDS;

    /* Allocate sound slots */
    engine->sounds = (nx_sound_state_t*)calloc(MAX_SOUNDS, sizeof(nx_sound_state_t));
    if (!engine->sounds) return NX_ERR_MEMORY;

    /* Initialize mixer */
    nx_mixer_init(&engine->mixer, config->output_sample_rate, config->output_channels);

    /* Scratch buffer: one mix pass */
    engine->output_frames = config->buffer_frames ? config->buffer_frames : DEFAULT_CHUNK;
    engine->output_buffer = (float*)malloc(engine->output_frames * config->output_channels * sizeof(float));
    if (!engine->output_buffer) {
        free(engine->sounds);
        return NX_ERR_MEMORY;
    }

    /* Initialize master effects */
    nx_effects_init(&engine->master_effects, config->output_sample_rate);

    engine->initialized = 1;
    engine->running = 1;

    return NX_OK;
}

void nx_engine_destroy(nx_engine_t *engine) {
    if (!engine) return;

    /* Unload all sounds */
    if (engine->sounds) {
        for (int i = 0; i < engine->max_sounds; i++) {
            if (engine->sounds[i].active) {
                nx_engine_unload(engine, i);
            }
        }
        free(engine->sounds);
    }

    /* Free mixer */
    nx_mixer_destroy(&engine->mixer);

    /* Free output buffer */
    if (engine->output_buffer) {
        free(engine->output_buffer);
    }

    memset(engine, 0, sizeof(*engine));
}

/* ============ Sound Management ============ */

nx_sound_t nx_engine_load(nx_engine_t *engine, const char *filepath) {
    if (!engine || !filepath) return NX_SOUND_INVALID;

    int slot = find_free_slot(engine);
    if (slot < 0) {
        fprintf(stderr, "[Engine] No free sound slots\n");
        return NX_SOUND_INVALID;
    }

    nx_sound_state_t *sound = &engine->sounds[slot];
    memset(sound, 0, sizeof(*sound));

    if (nx_stream_open(&sound->stream, filepath) != NX_OK) {
        return NX_SOUND_INVALID;
    }

    if (setup_sound(engine, slot) == NX_SOUND_INVALID) {
        return NX_SOUND_INVALID;
    }

    nx_metadata_load(filepath, &sound->metadata);
    return slot;
}

nx_sound_t nx_engine_load_pcm(nx_engine_t *engine, const float *samples, size_t frames,
                              uint32_t sample_rate, uint16_t channels) {
    if (!engine || !samples) return NX_SOUND_INVALID;

    int slot = find_free_slot(engine);
    if (slot < 0) return NX_SOUND_INVALID;

    nx_sound_state_t *sound = &engine->sounds[slot];
    memset(sound, 0, sizeof(*sound));

    if (nx_stream_open_pcm(&sound->stream, samples, frames, sample_rate, channels) != NX_OK) {
        return NX_SOUND_INVALID;
    }

    return setup_sound(engine, slot);
}

void nx_engine_unload(nx_engine_t *engine, nx_sound_t id) {
    nx_sound_state_t *sound = get_sound(engine, id);
    if (!sound) return;

    /* Remove from mixer */
    if (sound->mixer_channel >= 0) {
        nx_mixer_remove_channel(&engine->mixer, sound->mixer_channel);
    }

    nx_stream_close(&sound->stream);
    nx_resample_destroy(&sound->resampler);

    free(sound->decode_buffer);
    free(sound->pending);

    memset(sound, 0, sizeof(*sound));
    engine->active_sounds--;
}

/* ============ Playback Control ============ */

nx_error_t nx_engine_play(nx_engine_t *engine, nx_sound_t id) {
    nx_sound_state_t *sound = get_sound(engine, id);
    if (!sound) return NX_ERR_INVALID;

    /* Playing a finished sound starts it over */
    if (sound->stream.state == NX_STREAM_FINISHED) {
        reset_playback(sound, 0);
    }

    /* Volume changes made while stopped apply at once, not as a ramp */
    if (!nx_engine_is_playing(engine, id)) {
        target_gains(sound, &sound->applied_gain[0], &sound->applied_gain[1]);
    }

    sound->stream.state = NX_STREAM_PLAYING;
    sound->paused = 0;

    return NX_OK;
}

nx_error_t nx_engine_pause(nx_engine_t *engine, nx_sound_t id) {
    nx_sound_state_t *sound = get_sound(engine, id);
    if (!sound) return NX_ERR_INVALID;

    if (sound->stream.state == NX_STREAM_PLAYING) {
        sound->stream.state = NX_STREAM_PAUSED;
    }
    sound->paused = 1;

    return NX_OK;
}

nx_error_t nx_engine_stop(nx_engine_t *engine, nx_sound_t id) {
    nx_sound_state_t *sound = get_sound(engine, id);
    if (!sound) return NX_ERR_INVALID;

    sound->stream.state = NX_STREAM_STOPPED;
    sound->paused = 0;
    reset_playback(sound, 0);

    return NX_OK;
}

void nx_engine_set_volume(nx_engine_t *engine, nx_sound_t id, float volume) {
    nx_sound_state_t *sound = get_sound(engine, id);
    if (!sound) return;

    sound->volume = (volume < 0) ? 0 : volume;
}

void nx_engine_set_pan(nx_engine_t *engine, nx_sound_t id, float pan) {
    nx_sound_state_t *sound = get_sound(engine, id);
    if (!sound) return;

    if (pan < -1.0f) pan = -1.0f;
    if (pan > 1.0f) pan = 1.0f;
    sound->pan = pan;
}

void nx_engine_set_loop(nx_engine_t *engine, nx_sound_t id, int loop) {
    nx_sound_state_t *sound = get_sound(engine, id);
    if (!sound) return;

    sound->looping = loop ? 1 : 0;
}

nx_error_t nx_engine_seek(nx_engine_t *engine, nx_sound_t id, float time_sec) {
    nx_sound_state_t *sound = get_sound(engine, id);
    if (!sound) return NX_ERR_INVALID;

    if (time_sec < 0.0f) time_sec = 0.0f;
    reset_playback(sound, (size_t)((double)time_sec * sound->stream.sample_rate));

    /* Seeking a finished sound leaves it ready to play from there */
    if (sound->stream.state == NX_STREAM_FINISHED) {
        sound->stream.state = NX_STREAM_STOPPED;
    }

    return NX_OK;
}

float nx_engine_get_position(nx_engine_t *engine, nx_sound_t id) {
    nx_sound_state_t *sound = get_sound(engine, id);
    if (!sound || sound->stream.sample_rate == 0) return 0.0f;

    if (sound->stream.state == NX_STREAM_FINISHED) return sound->stream.duration;

    /* The decoder runs ahead of what has been mixed by the pending frames */
    double ahead = (double)sound->pending_frames * sound->stream.sample_rate
                   / engine->config.output_sample_rate;
    double frame = (double)sound->stream.current_frame - ahead;
    if (frame < 0.0) {
        frame = sound->looping ? frame + (double)sound->stream.total_frames : 0.0;
    }

    return (float)(frame / sound->stream.sample_rate);
}

float nx_engine_get_duration(nx_engine_t *engine, nx_sound_t id) {
    nx_sound_state_t *sound = get_sound(engine, id);
    if (!sound) return 0.0f;

    return sound->stream.duration;
}

const nx_metadata_t* nx_engine_get_metadata(nx_engine_t *engine, nx_sound_t id) {
    nx_sound_state_t *sound = get_sound(engine, id);
    if (!sound) return NULL;

    return &sound->metadata;
}

int nx_engine_is_playing(nx_engine_t *engine, nx_sound_t id) {
    nx_sound_state_t *sound = get_sound(engine, id);
    if (!sound) return 0;

    return sound->stream.state == NX_STREAM_PLAYING && !sound->paused;
}

int nx_engine_is_finished(nx_engine_t *engine, nx_sound_t id) {
    nx_sound_state_t *sound = get_sound(engine, id);
    if (!sound) return 0;

    return sound->stream.state == NX_STREAM_FINISHED;
}

void nx_engine_set_master_volume(nx_engine_t *engine, float volume) {
    if (!engine) return;
    if (volume < 0.0f) volume = 0.0f;
    nx_mixer_set_master(&engine->mixer, volume);
    engine->master_effects.gain = volume;
}

/* ============ Audio Rendering ============ */

size_t nx_engine_render(nx_engine_t *engine, float *output, size_t frames) {
    if (!engine || !output || !engine->initialized) return 0;

    const uint32_t oc = engine->config.output_channels;
    memset(output, 0, frames * oc * sizeof(float));

    for (size_t done = 0; done < frames; ) {
        size_t n = frames - done;
        if (n > engine->output_frames) n = engine->output_frames;
        float *out = output + done * oc;

        for (int i = 0; i < engine->max_sounds; i++) {
            nx_sound_state_t *sound = &engine->sounds[i];
            if (!sound->active) continue;
            if (sound->stream.state != NX_STREAM_PLAYING) continue;
            if (sound->paused) continue;

            fill_pending(sound, n);
            mix_sound(engine, sound, out, n);

            if (sound->source_done && sound->pending_frames == 0) {
                sound->stream.state = NX_STREAM_FINISHED;
            }
        }

        done += n;
    }

    /* Master gain, then keep the sum inside full scale */
    size_t total = frames * oc;
    if (engine->master_effects.gain != 1.0f) {
        nx_dsp_gain(output, total, engine->master_effects.gain);
    }
    nx_dsp_hard_clip(output, total, 1.0f);

    return frames;
}

size_t nx_engine_render_s16(nx_engine_t *engine, int16_t *output, size_t frames) {
    if (!engine || !output || !engine->initialized) return 0;

    const uint32_t oc = engine->config.output_channels;

    for (size_t done = 0; done < frames; ) {
        size_t n = frames - done;
        if (n > engine->output_frames) n = engine->output_frames;

        nx_engine_render(engine, engine->output_buffer, n);
        nx_dsp_float_to_s16(engine->output_buffer, output + done * oc, n * oc);

        done += n;
    }

    return frames;
}
