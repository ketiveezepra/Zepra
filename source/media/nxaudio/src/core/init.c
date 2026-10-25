/*
 * NXAudio Core - Initialization
 * 
 * System initialization and global state management
 * 
 * Copyright (c) 2025 KetiveeAI - KETIVEEAI License
 */

#include "nxaudio/nxaudio.h"
#include "../dsp/engine.h"
#include "../device/device.h"
#include "../codecs/loader.h"
#include "../platform/nx_thread.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ============ Internal Types ============ */

#define MAX_CONTEXTS    16
#define MAX_BUFFERS     256
#define MAX_OBJECTS     512

#define DEFAULT_SAMPLE_RATE   48000
#define DEFAULT_PERIOD_FRAMES 512     /* ~10.7 ms at 48 kHz */

typedef struct {
    int32_t             id;
    float              *samples;        /* decoded, interleaved float */
    size_t              frames;
    uint32_t            sample_rate;
    uint32_t            channels;
    int                 refs;           /* objects created from it */
    int                 destroyed;      /* destroy asked for while refs > 0 */
    int                 in_use;
} audio_buffer_t;

typedef struct {
    int32_t             id;
    int32_t             ctx_id;
    int32_t             buffer_id;
    nxaudio_object_config_t config;
    nx_sound_t          sound;          /* the context engine's sound */
    int                 in_use;
} audio_object_t;

typedef struct {
    int32_t             id;
    nxaudio_context_config_t config;
    nxaudio_listener_t  listener;
    float               master_gain;
    int                 hrtf_enabled;
    int                 reverb_enabled;
    float               reverb_room_size;
    float               reverb_damping;
    float               reverb_wet;
    float               reverb_dry;
    int                 suspended;
    int                 in_use;

    /* Playback: the device thread renders `engine` under `lock` */
    nx_engine_t         engine;
    nx_device_t        *device;
    nx_mutex_t          lock;
} audio_context_t;

/* ============ Global State ============ */

static struct {
    int             initialized;
    audio_context_t contexts[MAX_CONTEXTS];
    audio_buffer_t  buffers[MAX_BUFFERS];
    audio_object_t  objects[MAX_OBJECTS];
    int32_t         next_ctx_id;
    int32_t         next_buf_id;
    int32_t         next_obj_id;
} g_nxaudio = {0};

/* ============ Helper Functions ============ */

static audio_context_t* find_context(nxaudio_context_t ctx) {
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        if (g_nxaudio.contexts[i].in_use && g_nxaudio.contexts[i].id == ctx) {
            return &g_nxaudio.contexts[i];
        }
    }
    return NULL;
}

/* Includes buffers already destroyed but still referenced by objects */
static audio_buffer_t* find_buffer_any(nxaudio_buffer_t buf) {
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (g_nxaudio.buffers[i].in_use && g_nxaudio.buffers[i].id == buf) {
            return &g_nxaudio.buffers[i];
        }
    }
    return NULL;
}

static audio_buffer_t* find_buffer(nxaudio_buffer_t buf) {
    audio_buffer_t *b = find_buffer_any(buf);
    return (b && !b->destroyed) ? b : NULL;
}

static audio_object_t* find_object(nxaudio_object_t obj) {
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (g_nxaudio.objects[i].in_use && g_nxaudio.objects[i].id == obj) {
            return &g_nxaudio.objects[i];
        }
    }
    return NULL;
}

static int find_free_context_slot(void) {
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        if (!g_nxaudio.contexts[i].in_use) return i;
    }
    return -1;
}

static int find_free_buffer_slot(void) {
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (!g_nxaudio.buffers[i].in_use) return i;
    }
    return -1;
}

static int find_free_object_slot(void) {
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (!g_nxaudio.objects[i].in_use) return i;
    }
    return -1;
}

static void buffer_free(audio_buffer_t *buf) {
    free(buf->samples);
    memset(buf, 0, sizeof(*buf));
}

static void buffer_release(int32_t buffer_id) {
    audio_buffer_t *buf = find_buffer_any(buffer_id);
    if (!buf) return;
    if (--buf->refs <= 0 && buf->destroyed) buffer_free(buf);
}

/* Object's context; NULL if either is gone */
static audio_context_t* object_context(audio_object_t *o) {
    return o ? find_context(o->ctx_id) : NULL;
}

/* Push the object's config into the engine. Caller holds c->lock. */
static void object_apply_config(audio_context_t *c, audio_object_t *o) {
    nx_engine_set_volume(&c->engine, o->sound, o->config.gain);
    nx_engine_set_loop(&c->engine, o->sound, o->config.looping);
}

/* Caller holds c->lock (or the device is already closed) */
static void object_free(audio_context_t *c, audio_object_t *o) {
    nx_engine_unload(&c->engine, o->sound);
    buffer_release(o->buffer_id);
    memset(o, 0, sizeof(*o));
}

static void context_apply_gain(audio_context_t *c) {
    nx_mutex_lock(&c->lock);
    nx_engine_set_master_volume(&c->engine, c->master_gain * c->listener.gain);
    nx_mutex_unlock(&c->lock);
}

/* Device thread: mix everything playing in this context */
static void context_render(void *user, float *out, uint32_t frames) {
    audio_context_t *c = (audio_context_t*)user;
    nx_mutex_lock(&c->lock);
    nx_engine_render(&c->engine, out, frames);
    nx_mutex_unlock(&c->lock);
}

/* ============ System API ============ */

nxaudio_error_t nxaudio_init(void) {
    if (g_nxaudio.initialized) {
        return NXAUDIO_SUCCESS;
    }
    
    memset(&g_nxaudio, 0, sizeof(g_nxaudio));
    g_nxaudio.next_ctx_id = 1;
    g_nxaudio.next_buf_id = 1;
    g_nxaudio.next_obj_id = 1;
    g_nxaudio.initialized = 1;
    
    return NXAUDIO_SUCCESS;
}

void nxaudio_shutdown(void) {
    if (!g_nxaudio.initialized) return;
    
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        if (g_nxaudio.contexts[i].in_use) {
            nxaudio_context_destroy(g_nxaudio.contexts[i].id);
        }
    }
    
    /* Free all buffers */
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (g_nxaudio.buffers[i].in_use) {
            free(g_nxaudio.buffers[i].samples);
        }
    }
    
    memset(&g_nxaudio, 0, sizeof(g_nxaudio));
}

const char* nxaudio_version(void) {
    return NXAUDIO_VERSION_STRING;
}

const char* nxaudio_error_string(nxaudio_error_t error) {
    switch (error) {
        case NXAUDIO_SUCCESS:       return "Success";
        case NXAUDIO_ERROR_INIT:    return "Initialization error";
        case NXAUDIO_ERROR_INVALID: return "Invalid parameter";
        case NXAUDIO_ERROR_NO_MEMORY: return "Out of memory";
        case NXAUDIO_ERROR_NO_DEVICE: return "No audio device";
        case NXAUDIO_ERROR_IO:      return "I/O error";
        case NXAUDIO_ERROR_FORMAT:  return "Invalid format";
        case NXAUDIO_ERROR_TIMEOUT: return "Timeout";
        case NXAUDIO_ERROR_BUSY:    return "Resource busy";
        default:                    return "Unknown error";
    }
}

/* ============ Context API ============ */

nxaudio_context_t nxaudio_context_create(void) {
    nxaudio_context_config_t config = {
        .sample_rate = DEFAULT_SAMPLE_RATE,
        .buffer_size = DEFAULT_PERIOD_FRAMES,
        .max_objects = 128,
        .distance_model = NXAUDIO_DISTANCE_INVERSE,
        .doppler_factor = 1.0f,
        .speed_of_sound = 343.0f,
    };
    return nxaudio_context_create_ex(&config);
}

nxaudio_context_t nxaudio_context_create_ex(const nxaudio_context_config_t *config) {
    if (!g_nxaudio.initialized || !config) {
        return NXAUDIO_INVALID_HANDLE;
    }
    
    int slot = find_free_context_slot();
    if (slot < 0) {
        return NXAUDIO_INVALID_HANDLE;
    }
    
    audio_context_t *ctx = &g_nxaudio.contexts[slot];
    memset(ctx, 0, sizeof(*ctx));
    
    ctx->config = *config;
    if (ctx->config.sample_rate == 0) ctx->config.sample_rate = DEFAULT_SAMPLE_RATE;
    if (ctx->config.buffer_size == 0) ctx->config.buffer_size = DEFAULT_PERIOD_FRAMES;
    
    ctx->master_gain = 1.0f;
    ctx->hrtf_enabled = 1;
    ctx->reverb_enabled = 0;
    ctx->reverb_room_size = 0.5f;
    ctx->reverb_damping = 0.5f;
    ctx->reverb_wet = 0.3f;
    ctx->reverb_dry = 1.0f;
    ctx->suspended = 0;
    
    /* Default listener */
    ctx->listener.position = (nxaudio_vec3_t){0, 0, 0};
    ctx->listener.velocity = (nxaudio_vec3_t){0, 0, 0};
    ctx->listener.orientation = (nxaudio_quat_t){0, 0, 0, 1};
    ctx->listener.gain = 1.0f;
    
    nx_engine_config_t engine_config = {
        .output_sample_rate = ctx->config.sample_rate,
        .output_channels = NX_DEVICE_CHANNELS,
        .buffer_frames = 1024,
        .resample_quality = NX_RESAMPLE_MEDIUM,
    };
    if (nx_engine_create_ex(&ctx->engine, &engine_config) != NX_OK) {
        return NXAUDIO_INVALID_HANDLE;
    }
    
    nx_mutex_init(&ctx->lock);
    
    ctx->device = nx_device_open(NULL, ctx->config.sample_rate, ctx->config.buffer_size,
                                 context_render, ctx);
    if (!ctx->device) {
        nx_mutex_destroy(&ctx->lock);
        nx_engine_destroy(&ctx->engine);
        return NXAUDIO_INVALID_HANDLE;
    }
    
    ctx->id = g_nxaudio.next_ctx_id++;
    ctx->in_use = 1;
    
    return ctx->id;
}

void nxaudio_context_destroy(nxaudio_context_t ctx) {
    audio_context_t *c = find_context(ctx);
    if (!c) return;
    
    /* Stops the device thread, so nothing renders while we tear down */
    nx_device_close(c->device);
    c->device = NULL;
    
    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (g_nxaudio.objects[i].in_use && g_nxaudio.objects[i].ctx_id == ctx) {
            object_free(c, &g_nxaudio.objects[i]);
        }
    }
    
    nx_engine_destroy(&c->engine);
    nx_mutex_destroy(&c->lock);
    c->in_use = 0;
}

const char* nxaudio_context_backend(nxaudio_context_t ctx) {
    audio_context_t *c = find_context(ctx);
    if (!c) return "none";
    return nx_device_backend_name(c->device);
}

nxaudio_error_t nxaudio_context_set_listener(nxaudio_context_t ctx,
                                              const nxaudio_listener_t *listener) {
    audio_context_t *c = find_context(ctx);
    if (!c) return NXAUDIO_ERROR_INVALID;
    if (!listener) return NXAUDIO_ERROR_INVALID;
    
    c->listener = *listener;
    context_apply_gain(c);
    return NXAUDIO_SUCCESS;
}

nxaudio_error_t nxaudio_context_get_listener(nxaudio_context_t ctx,
                                              nxaudio_listener_t *listener) {
    audio_context_t *c = find_context(ctx);
    if (!c) return NXAUDIO_ERROR_INVALID;
    if (!listener) return NXAUDIO_ERROR_INVALID;
    
    *listener = c->listener;
    return NXAUDIO_SUCCESS;
}

nxaudio_error_t nxaudio_context_set_gain(nxaudio_context_t ctx, float gain) {
    audio_context_t *c = find_context(ctx);
    if (!c) return NXAUDIO_ERROR_INVALID;
    
    c->master_gain = gain < 0.0f ? 0.0f : gain;
    context_apply_gain(c);
    return NXAUDIO_SUCCESS;
}

nxaudio_error_t nxaudio_context_suspend(nxaudio_context_t ctx) {
    audio_context_t *c = find_context(ctx);
    if (!c) return NXAUDIO_ERROR_INVALID;
    
    /* Releases the hardware; sounds keep their place */
    nx_device_stop(c->device);
    c->suspended = 1;
    return NXAUDIO_SUCCESS;
}

nxaudio_error_t nxaudio_context_resume(nxaudio_context_t ctx) {
    audio_context_t *c = find_context(ctx);
    if (!c) return NXAUDIO_ERROR_INVALID;
    
    if (nx_device_start(c->device) != 0) return NXAUDIO_ERROR_NO_DEVICE;
    c->suspended = 0;
    return NXAUDIO_SUCCESS;
}

/* ============ Buffer API ============ */

static nxaudio_buffer_t buffer_store(float *samples, size_t frames,
                                     uint32_t sample_rate, uint32_t channels) {
    int slot = find_free_buffer_slot();
    if (slot < 0) {
        free(samples);
        return NXAUDIO_INVALID_HANDLE;
    }
    
    audio_buffer_t *buf = &g_nxaudio.buffers[slot];
    memset(buf, 0, sizeof(*buf));
    buf->id = g_nxaudio.next_buf_id++;
    buf->samples = samples;
    buf->frames = frames;
    buf->sample_rate = sample_rate;
    buf->channels = channels;
    buf->in_use = 1;
    
    return buf->id;
}

nxaudio_buffer_t nxaudio_buffer_create(const char *filepath) {
    if (!g_nxaudio.initialized || !filepath) {
        return NXAUDIO_INVALID_HANDLE;
    }
    
    audio_data_t audio;
    if (audio_load(filepath, &audio) != 0 || !audio.samples ||
        audio.channels == 0 || audio.sample_rate == 0) {
        audio_free(&audio);
        return NXAUDIO_INVALID_HANDLE;
    }
    
    /* Ownership of the samples moves to the buffer */
    return buffer_store(audio.samples, audio.num_frames, audio.sample_rate, audio.channels);
}

nxaudio_buffer_t nxaudio_buffer_create_mem(const void *data, size_t size,
                                            nxaudio_format_t format,
                                            uint32_t sample_rate,
                                            uint32_t channels) {
    if (!g_nxaudio.initialized || !data || size == 0 || channels == 0 || sample_rate == 0) {
        return NXAUDIO_INVALID_HANDLE;
    }
    
    size_t sample_size = 4; /* F32, S32 */
    if (format == NXAUDIO_FORMAT_S16) sample_size = 2;
    else if (format == NXAUDIO_FORMAT_S24) sample_size = 3;
    
    size_t frames = size / (sample_size * channels);
    size_t samples = frames * channels;
    if (frames == 0) return NXAUDIO_INVALID_HANDLE;
    
    float *out = (float*)malloc(samples * sizeof(float));
    if (!out) return NXAUDIO_INVALID_HANDLE;
    
    const uint8_t *src = (const uint8_t*)data;
    switch (format) {
        case NXAUDIO_FORMAT_S16:
            for (size_t i = 0; i < samples; i++) {
                int16_t v = (int16_t)(src[i * 2] | (src[i * 2 + 1] << 8));
                out[i] = (float)v / 32768.0f;
            }
            break;
        case NXAUDIO_FORMAT_S24:
            for (size_t i = 0; i < samples; i++) {
                const uint8_t *p = src + i * 3;
                int32_t v = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) |
                                      ((uint32_t)p[2] << 24)) >> 8;
                out[i] = (float)v / 8388608.0f;
            }
            break;
        case NXAUDIO_FORMAT_S32:
            for (size_t i = 0; i < samples; i++) {
                int32_t v;
                memcpy(&v, src + i * 4, 4);
                out[i] = (float)v / 2147483648.0f;
            }
            break;
        case NXAUDIO_FORMAT_F32:
        default:
            memcpy(out, src, samples * sizeof(float));
            break;
    }
    
    return buffer_store(out, frames, sample_rate, channels);
}

void nxaudio_buffer_destroy(nxaudio_buffer_t buffer) {
    audio_buffer_t *buf = find_buffer(buffer);
    if (!buf) return;
    
    /* Objects still playing it keep the samples alive until they go */
    if (buf->refs > 0) {
        buf->destroyed = 1;
        return;
    }
    buffer_free(buf);
}

float nxaudio_buffer_duration(nxaudio_buffer_t buffer) {
    audio_buffer_t *buf = find_buffer(buffer);
    if (!buf || buf->sample_rate == 0) return 0.0f;
    
    return (float)buf->frames / (float)buf->sample_rate;
}

/* ============ Object API ============ */

nxaudio_object_t nxaudio_object_create(nxaudio_context_t ctx,
                                        nxaudio_buffer_t buffer,
                                        const nxaudio_object_config_t *config) {
    if (!g_nxaudio.initialized) return NXAUDIO_INVALID_HANDLE;
    
    audio_context_t *c = find_context(ctx);
    if (!c) return NXAUDIO_INVALID_HANDLE;
    
    audio_buffer_t *buf = find_buffer(buffer);
    if (!buf) return NXAUDIO_INVALID_HANDLE;
    
    int slot = find_free_object_slot();
    if (slot < 0) return NXAUDIO_INVALID_HANDLE;
    
    audio_object_t *obj = &g_nxaudio.objects[slot];
    memset(obj, 0, sizeof(*obj));
    
    if (config) {
        obj->config = *config;
    } else {
        obj->config.position = (nxaudio_vec3_t){0, 0, 0};
        obj->config.velocity = (nxaudio_vec3_t){0, 0, 0};
        obj->config.gain = 1.0f;
        obj->config.pitch = 1.0f;
        obj->config.min_distance = 1.0f;
        obj->config.max_distance = 100.0f;
        obj->config.rolloff = 1.0f;
        obj->config.looping = 0;
        obj->config.spatial = 1;
        obj->config.reverb = 1;
        obj->config.occlusion = 0;
    }
    
    nx_mutex_lock(&c->lock);
    obj->sound = nx_engine_load_pcm(&c->engine, buf->samples, buf->frames,
                                    buf->sample_rate, (uint16_t)buf->channels);
    if (obj->sound != NX_SOUND_INVALID) {
        object_apply_config(c, obj);
    }
    nx_mutex_unlock(&c->lock);
    
    if (obj->sound == NX_SOUND_INVALID) {
        memset(obj, 0, sizeof(*obj));
        return NXAUDIO_INVALID_HANDLE;
    }
    
    obj->id = g_nxaudio.next_obj_id++;
    obj->ctx_id = ctx;
    obj->buffer_id = buffer;
    obj->in_use = 1;
    buf->refs++;
    
    return obj->id;
}

void nxaudio_object_destroy(nxaudio_object_t obj) {
    audio_object_t *o = find_object(obj);
    audio_context_t *c = object_context(o);
    if (!o || !c) return;
    
    nx_mutex_lock(&c->lock);
    object_free(c, o);
    nx_mutex_unlock(&c->lock);
}

nxaudio_error_t nxaudio_object_play(nxaudio_object_t obj) {
    audio_object_t *o = find_object(obj);
    audio_context_t *c = object_context(o);
    if (!o || !c) return NXAUDIO_ERROR_INVALID;
    
    nx_mutex_lock(&c->lock);
    nx_engine_play(&c->engine, o->sound);
    nx_mutex_unlock(&c->lock);
    return NXAUDIO_SUCCESS;
}

nxaudio_error_t nxaudio_object_pause(nxaudio_object_t obj) {
    audio_object_t *o = find_object(obj);
    audio_context_t *c = object_context(o);
    if (!o || !c) return NXAUDIO_ERROR_INVALID;
    
    nx_mutex_lock(&c->lock);
    nx_engine_pause(&c->engine, o->sound);
    nx_mutex_unlock(&c->lock);
    return NXAUDIO_SUCCESS;
}

nxaudio_error_t nxaudio_object_stop(nxaudio_object_t obj) {
    audio_object_t *o = find_object(obj);
    audio_context_t *c = object_context(o);
    if (!o || !c) return NXAUDIO_ERROR_INVALID;
    
    nx_mutex_lock(&c->lock);
    nx_engine_stop(&c->engine, o->sound);
    nx_mutex_unlock(&c->lock);
    return NXAUDIO_SUCCESS;
}

nxaudio_error_t nxaudio_object_update(nxaudio_object_t obj,
                                       const nxaudio_object_config_t *config) {
    audio_object_t *o = find_object(obj);
    audio_context_t *c = object_context(o);
    if (!o || !c || !config) return NXAUDIO_ERROR_INVALID;
    
    o->config = *config;
    nx_mutex_lock(&c->lock);
    object_apply_config(c, o);
    nx_mutex_unlock(&c->lock);
    return NXAUDIO_SUCCESS;
}

nxaudio_error_t nxaudio_object_set_position(nxaudio_object_t obj,
                                             float x, float y, float z) {
    audio_object_t *o = find_object(obj);
    if (!o) return NXAUDIO_ERROR_INVALID;
    
    /* Stored for spatialization, which the output path doesn't apply yet */
    o->config.position.x = x;
    o->config.position.y = y;
    o->config.position.z = z;
    return NXAUDIO_SUCCESS;
}

nxaudio_error_t nxaudio_object_set_gain(nxaudio_object_t obj, float gain) {
    audio_object_t *o = find_object(obj);
    audio_context_t *c = object_context(o);
    if (!o || !c) return NXAUDIO_ERROR_INVALID;
    
    o->config.gain = gain < 0.0f ? 0.0f : gain;
    nx_mutex_lock(&c->lock);
    object_apply_config(c, o);
    nx_mutex_unlock(&c->lock);
    return NXAUDIO_SUCCESS;
}

int nxaudio_object_is_playing(nxaudio_object_t obj) {
    audio_object_t *o = find_object(obj);
    audio_context_t *c = object_context(o);
    if (!o || !c) return 0;
    
    nx_mutex_lock(&c->lock);
    int playing = nx_engine_is_playing(&c->engine, o->sound);
    nx_mutex_unlock(&c->lock);
    return playing;
}

float nxaudio_object_get_time(nxaudio_object_t obj) {
    audio_object_t *o = find_object(obj);
    audio_context_t *c = object_context(o);
    if (!o || !c) return 0.0f;
    
    nx_mutex_lock(&c->lock);
    float t = nx_engine_get_position(&c->engine, o->sound);
    nx_mutex_unlock(&c->lock);
    return t;
}

nxaudio_error_t nxaudio_object_seek(nxaudio_object_t obj, float time_sec) {
    audio_object_t *o = find_object(obj);
    audio_context_t *c = object_context(o);
    if (!o || !c) return NXAUDIO_ERROR_INVALID;
    
    nx_mutex_lock(&c->lock);
    nx_error_t err = nx_engine_seek(&c->engine, o->sound, time_sec);
    nx_mutex_unlock(&c->lock);
    return err == NX_OK ? NXAUDIO_SUCCESS : NXAUDIO_ERROR_INVALID;
}

/* ============ HRTF API ============ */

nxaudio_error_t nxaudio_hrtf_load(nxaudio_context_t ctx, const char *filepath) {
    audio_context_t *c = find_context(ctx);
    if (!c || !filepath) return NXAUDIO_ERROR_INVALID;
    
    /* TODO: Load SOFA HRTF file */
    c->hrtf_enabled = 1;
    return NXAUDIO_SUCCESS;
}

nxaudio_error_t nxaudio_hrtf_load_default(nxaudio_context_t ctx) {
    audio_context_t *c = find_context(ctx);
    if (!c) return NXAUDIO_ERROR_INVALID;
    
    /* Use built-in default HRTF */
    c->hrtf_enabled = 1;
    return NXAUDIO_SUCCESS;
}

nxaudio_error_t nxaudio_hrtf_info(nxaudio_context_t ctx, nxaudio_hrtf_info_t *info) {
    audio_context_t *c = find_context(ctx);
    if (!c || !info) return NXAUDIO_ERROR_INVALID;
    
    strncpy(info->name, "Default HRTF", sizeof(info->name) - 1);
    info->sample_rate = 48000;
    info->ir_length = 256;
    info->num_positions = 1250;
    
    return NXAUDIO_SUCCESS;
}

nxaudio_error_t nxaudio_hrtf_enable(nxaudio_context_t ctx, int enabled) {
    audio_context_t *c = find_context(ctx);
    if (!c) return NXAUDIO_ERROR_INVALID;
    
    c->hrtf_enabled = enabled ? 1 : 0;
    return NXAUDIO_SUCCESS;
}

/* ============ Reverb API ============ */

nxaudio_error_t nxaudio_reverb_set(nxaudio_context_t ctx,
                                    float room_size,
                                    float damping,
                                    float wet,
                                    float dry) {
    audio_context_t *c = find_context(ctx);
    if (!c) return NXAUDIO_ERROR_INVALID;
    
    c->reverb_room_size = room_size;
    c->reverb_damping = damping;
    c->reverb_wet = wet;
    c->reverb_dry = dry;
    
    return NXAUDIO_SUCCESS;
}

nxaudio_error_t nxaudio_reverb_enable(nxaudio_context_t ctx, int enabled) {
    audio_context_t *c = find_context(ctx);
    if (!c) return NXAUDIO_ERROR_INVALID;
    
    c->reverb_enabled = enabled ? 1 : 0;
    return NXAUDIO_SUCCESS;
}
