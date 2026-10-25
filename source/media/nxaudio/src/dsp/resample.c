/*
 * NXAudio Resampler Implementation
 * 
 * Copyright (c) 2025 KetiveeAI - KETIVEEAI License
 */

#include "resample.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SINC_WINDOW_SIZE    16
#define PI                  3.14159265358979323846

/* ============ Sinc Function ============ */

static float sinc(float x) {
    if (fabsf(x) < 1e-6f) return 1.0f;
    float pix = PI * x;
    return sinf(pix) / pix;
}

/* Lanczos window */
static float lanczos(float x, float a) {
    if (fabsf(x) >= a) return 0.0f;
    return sinc(x) * sinc(x / a);
}

/* ============ Taps ============ */

/*
 * Each mode reads `before` frames behind and `after` frames ahead of the
 * integer position. The last before+after+1 input frames are kept in
 * `history` so the next block can interpolate across the boundary.
 */
static void quality_taps(nx_resample_quality_t q, int *before, int *after) {
    switch (q) {
        case NX_RESAMPLE_HIGH:   *before = SINC_WINDOW_SIZE; *after = SINC_WINDOW_SIZE - 1; break;
        case NX_RESAMPLE_MEDIUM: *before = 1; *after = 2; break;
        default:                 *before = 0; *after = 1; break;
    }
}

/* Frame v of history + in: v < 0 reads history, v >= 0 reads the new block */
static inline float sample_at(const nx_resampler_t *rs, const float *in,
                              long v, uint32_t c) {
    if (v < 0) {
        long h = (long)(rs->history_size / rs->channels) + v;
        if (h < 0) return 0.0f;
        return rs->history[(size_t)h * rs->channels + c];
    }
    return in[(size_t)v * rs->channels + c];
}

/* Keep the last history frames of (history + in) for the next call */
static void update_history(nx_resampler_t *rs, const float *in, size_t in_frames) {
    size_t hist_frames = rs->history_size / rs->channels;
    uint32_t ch = rs->channels;

    if (in_frames >= hist_frames) {
        memcpy(rs->history, in + (in_frames - hist_frames) * ch, rs->history_size * sizeof(float));
    } else {
        size_t keep = hist_frames - in_frames;
        memmove(rs->history, rs->history + in_frames * ch, keep * ch * sizeof(float));
        memcpy(rs->history + keep * ch, in, in_frames * ch * sizeof(float));
    }
}

/* ============ Initialization ============ */

int nx_resample_init(nx_resampler_t *rs, 
                      uint32_t input_rate, 
                      uint32_t output_rate,
                      uint32_t channels,
                      nx_resample_quality_t quality) {
    if (!rs || input_rate == 0 || output_rate == 0 || channels == 0) {
        return -1;
    }
    
    memset(rs, 0, sizeof(*rs));
    
    rs->input_rate = input_rate;
    rs->output_rate = output_rate;
    rs->ratio = (double)output_rate / (double)input_rate;
    rs->channels = channels;
    rs->quality = quality;
    rs->position = 0.0;
    
    int before, after;
    quality_taps(quality, &before, &after);
    rs->history_size = (size_t)(before + after + 1) * channels;
    
    rs->history = (float*)calloc(rs->history_size, sizeof(float));
    if (!rs->history) return -1;
    
    rs->history_pos = 0;
    
    return 0;
}

void nx_resample_destroy(nx_resampler_t *rs) {
    if (!rs) return;
    
    if (rs->history) {
        free(rs->history);
    }
    memset(rs, 0, sizeof(*rs));
}

void nx_resample_reset(nx_resampler_t *rs) {
    if (!rs) return;
    
    rs->position = 0.0;
    if (rs->history) {
        memset(rs->history, 0, rs->history_size * sizeof(float));
    }
    rs->history_pos = 0;
}

size_t nx_resample_output_frames(nx_resampler_t *rs, size_t input_frames) {
    if (!rs) return 0;
    return (size_t)(input_frames * rs->ratio + 0.5);
}

size_t nx_resample_input_frames(nx_resampler_t *rs, size_t output_frames) {
    if (!rs || rs->ratio == 0) return 0;
    return (size_t)(output_frames / rs->ratio + 0.5);
}

size_t nx_resample_latency(nx_resampler_t *rs) {
    if (!rs) return 0;
    int before, after;
    quality_taps(rs->quality, &before, &after);
    return (size_t)after;
}

/* ============ Interpolation ============ */

/*
 * rs->position is the next output's position in input frames, relative to
 * the first frame of `in`. It may be negative (inside history).
 */
static size_t resample_block(nx_resampler_t *rs,
                             const float *in, size_t in_frames,
                             float *out, size_t out_frames) {
    const double step = 1.0 / rs->ratio;
    const uint32_t ch = rs->channels;
    int before, after;
    quality_taps(rs->quality, &before, &after);

    size_t out_count = 0;
    while (out_count < out_frames) {
        double fl = floor(rs->position);
        long index = (long)fl;
        if (index + after >= (long)in_frames) break;
        double frac = rs->position - fl;

        for (uint32_t c = 0; c < ch; c++) {
            float v;
            if (rs->quality == NX_RESAMPLE_MEDIUM) {
                /* Catmull-Rom spline */
                float y0 = sample_at(rs, in, index - 1, c);
                float y1 = sample_at(rs, in, index, c);
                float y2 = sample_at(rs, in, index + 1, c);
                float y3 = sample_at(rs, in, index + 2, c);
                float t = (float)frac;
                float a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
                float a1 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
                float a2 = -0.5f * y0 + 0.5f * y2;
                v = ((a0 * t + a1) * t + a2) * t + y1;
            } else if (rs->quality == NX_RESAMPLE_HIGH) {
                float sum = 0.0f, weight_sum = 0.0f;
                for (int i = -before; i <= after; i++) {
                    float w = lanczos((float)(i - frac), (float)SINC_WINDOW_SIZE);
                    sum += sample_at(rs, in, index + i, c) * w;
                    weight_sum += w;
                }
                v = (weight_sum > 0) ? sum / weight_sum : 0.0f;
            } else {
                float s0 = sample_at(rs, in, index, c);
                float s1 = sample_at(rs, in, index + 1, c);
                v = s0 + (float)frac * (s1 - s0);
            }
            out[out_count * ch + c] = v;
        }

        out_count++;
        rs->position += step;
    }

    /* Rebase onto the next block. Callers leave room for every output a
     * block yields (nx_resample_output_frames + 1); frames left unread
     * because `out` filled up are lost. */
    rs->position -= (double)in_frames;
    update_history(rs, in, in_frames);

    return out_count;
}

/* ============ Public API ============ */

size_t nx_resample_process(nx_resampler_t *rs,
                            const float *in, size_t in_frames,
                            float *out, size_t out_frames) {
    if (!rs || !in || !out || in_frames == 0) {
        return 0;
    }
    
    /* No resampling needed */
    if (rs->input_rate == rs->output_rate) {
        size_t copy = (in_frames < out_frames) ? in_frames : out_frames;
        memcpy(out, in, copy * rs->channels * sizeof(float));
        return copy;
    }
    
    return resample_block(rs, in, in_frames, out, out_frames);
}
