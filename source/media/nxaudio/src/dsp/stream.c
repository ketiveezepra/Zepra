/*
 * NXAudio Streaming Decoder Implementation
 * 
 * Copyright (c) 2025 KetiveeAI - KETIVEEAI License
 */

#include "stream.h"
#include "../codecs/wav.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define READ_BUFFER_SIZE    (64 * 1024)  /* 64KB read buffer */

/* ============ Helper Functions ============ */

static int str_ends_with(const char *str, const char *suffix) {
    size_t str_len = strlen(str);
    size_t suf_len = strlen(suffix);
    if (suf_len > str_len) return 0;
    const char *tail = str + str_len - suf_len;
    for (size_t i = 0; i < suf_len; i++) {
        if (tolower((unsigned char)tail[i]) != tolower((unsigned char)suffix[i])) return 0;
    }
    return 1;
}

static nx_codec_t detect_codec(const char *filepath) {
    if (str_ends_with(filepath, ".wav")) return NX_CODEC_WAV;
    if (str_ends_with(filepath, ".mp3")) return NX_CODEC_MP3;
    if (str_ends_with(filepath, ".flac")) return NX_CODEC_FLAC;
    if (str_ends_with(filepath, ".ogg")) return NX_CODEC_OGG;
    if (str_ends_with(filepath, ".aiff") || str_ends_with(filepath, ".aif")) return NX_CODEC_AIFF;
    if (str_ends_with(filepath, ".opus")) return NX_CODEC_OPUS;
    return NX_CODEC_UNKNOWN;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ============ WAV Streaming Codec ============ */

typedef struct {
    size_t   data_offset;
    size_t   data_size;
    size_t   bytes_per_frame;
    int      is_float;
} wav_stream_state_t;

static nx_error_t wav_stream_open(nx_audio_stream_t *stream) {
    wav_stream_state_t *state = (wav_stream_state_t*)stream->decoder_state;
    uint8_t riff[12];

    if (fread(riff, 1, 12, stream->file) != 12) return NX_ERR_IO;
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        return NX_ERR_FORMAT;
    }

    int have_fmt = 0;
    uint16_t format = 0;

    for (;;) {
        uint8_t hdr[8];
        if (fread(hdr, 1, 8, stream->file) != 8) break;

        uint32_t chunk_size = rd32(hdr + 4);
        long body = ftell(stream->file);

        if (memcmp(hdr, "fmt ", 4) == 0) {
            uint8_t fmt[40] = {0};
            size_t n = chunk_size < sizeof(fmt) ? chunk_size : sizeof(fmt);
            if (n < 16 || fread(fmt, 1, n, stream->file) != n) return NX_ERR_FORMAT;

            format = rd16(fmt);
            stream->channels = rd16(fmt + 2);
            stream->sample_rate = rd32(fmt + 4);
            stream->bits_per_sample = rd16(fmt + 14);

            /* WAVE_FORMAT_EXTENSIBLE: the real format is the sub-format GUID's first word */
            if (format == 0xFFFE && n >= 26) format = rd16(fmt + 24);
            have_fmt = 1;
        }
        else if (memcmp(hdr, "data", 4) == 0) {
            if (!have_fmt) return NX_ERR_FORMAT;

            state->data_offset = (size_t)body;
            /* Streamed WAVs may say 0 or 0xFFFFFFFF here; trust the file size */
            size_t avail = stream->file_size > (size_t)body ? stream->file_size - (size_t)body : 0;
            state->data_size = (chunk_size == 0 || chunk_size > avail) ? avail : chunk_size;
            break;
        }

        if (fseek(stream->file, body + (long)chunk_size + (long)(chunk_size & 1), SEEK_SET) != 0) break;
    }

    if (!have_fmt || state->data_offset == 0) return NX_ERR_FORMAT;
    if (stream->channels == 0 || stream->sample_rate == 0) return NX_ERR_FORMAT;

    int bits = stream->bits_per_sample;
    if (format == 3 && bits == 32) {
        state->is_float = 1;
    } else if (format != 1 || (bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
        fprintf(stderr, "[Stream] Unsupported WAV encoding (format %u, %d bit)\n", format, bits);
        return NX_ERR_FORMAT;
    }

    state->bytes_per_frame = (size_t)stream->channels * (size_t)(bits / 8);
    stream->total_frames = state->data_size / state->bytes_per_frame;
    stream->duration = (float)stream->total_frames / stream->sample_rate;

    fseek(stream->file, (long)state->data_offset, SEEK_SET);
    return NX_OK;
}

static nx_error_t wav_stream_read(nx_audio_stream_t *stream, float *out, size_t *frames) {
    wav_stream_state_t *state = (wav_stream_state_t*)stream->decoder_state;
    
    size_t want = *frames;
    size_t avail = stream->total_frames - stream->current_frame;
    if (want > avail) want = avail;
    
    if (want == 0) {
        *frames = 0;
        return NX_ERR_EOF;
    }
    
    size_t done = 0;
    const size_t chunk_frames = stream->read_buffer_size / state->bytes_per_frame;

    while (done < want) {
        size_t n = want - done;
        if (n > chunk_frames) n = chunk_frames;

        size_t got = fread(stream->read_buffer, 1, n * state->bytes_per_frame, stream->file)
                     / state->bytes_per_frame;
        if (got == 0) break;

        const uint8_t *raw = stream->read_buffer;
        float *dst = out + done * stream->channels;
        size_t samples = got * stream->channels;

        switch (stream->bits_per_sample) {
            case 8:
                for (size_t i = 0; i < samples; i++) {
                    dst[i] = ((float)raw[i] - 128.0f) / 128.0f;
                }
                break;
            case 16:
                for (size_t i = 0; i < samples; i++) {
                    dst[i] = (float)(int16_t)rd16(raw + i * 2) / 32768.0f;
                }
                break;
            case 24:
                for (size_t i = 0; i < samples; i++) {
                    const uint8_t *p = raw + i * 3;
                    int32_t val = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) |
                                            ((uint32_t)p[2] << 24)) >> 8;
                    dst[i] = (float)val / 8388608.0f;
                }
                break;
            case 32:
                if (state->is_float) {
                    memcpy(dst, raw, samples * sizeof(float));
                } else {
                    for (size_t i = 0; i < samples; i++) {
                        dst[i] = (float)(int32_t)rd32(raw + i * 4) / 2147483648.0f;
                    }
                }
                break;
        }
        done += got;
    }
    
    stream->current_frame += done;
    *frames = done;
    
    return done ? NX_OK : NX_ERR_EOF;
}

static nx_error_t wav_stream_seek(nx_audio_stream_t *stream, size_t frame) {
    wav_stream_state_t *state = (wav_stream_state_t*)stream->decoder_state;
    
    if (frame >= stream->total_frames) {
        frame = stream->total_frames;
    }
    
    size_t byte_offset = state->data_offset + frame * state->bytes_per_frame;
    fseek(stream->file, (long)byte_offset, SEEK_SET);
    stream->current_frame = frame;
    
    return NX_OK;
}

static void wav_stream_close(nx_audio_stream_t *stream) {
    /* Nothing extra to free for WAV */
    (void)stream;
}

static const nx_codec_ops_t wav_codec_ops = {
    .open = wav_stream_open,
    .read = wav_stream_read,
    .seek = wav_stream_seek,
    .close = wav_stream_close,
};

/* ============ In-memory PCM ============ */

/* Already-decoded float samples owned by the caller (e.g. an nxaudio buffer) */
typedef struct {
    const float *samples;
} pcm_stream_state_t;

static nx_error_t pcm_stream_read(nx_audio_stream_t *stream, float *out, size_t *frames) {
    pcm_stream_state_t *state = (pcm_stream_state_t*)stream->decoder_state;

    size_t want = *frames;
    size_t avail = stream->total_frames - stream->current_frame;
    if (want > avail) want = avail;

    *frames = want;
    if (want == 0) return NX_ERR_EOF;

    memcpy(out, state->samples + stream->current_frame * stream->channels,
           want * stream->channels * sizeof(float));
    stream->current_frame += want;
    return NX_OK;
}

static nx_error_t pcm_stream_seek(nx_audio_stream_t *stream, size_t frame) {
    stream->current_frame = frame < stream->total_frames ? frame : stream->total_frames;
    return NX_OK;
}

static const nx_codec_ops_t pcm_codec_ops = {
    .open = NULL,
    .read = pcm_stream_read,
    .seek = pcm_stream_seek,
    .close = NULL,
};

/* ============ Public API ============ */

nx_error_t nx_stream_open(nx_audio_stream_t *stream, const char *filepath) {
    if (!stream || !filepath) return NX_ERR_INVALID;
    
    memset(stream, 0, sizeof(*stream));
    strncpy(stream->filepath, filepath, sizeof(stream->filepath) - 1);
    
    /* Detect codec */
    stream->codec = detect_codec(filepath);
    if (stream->codec == NX_CODEC_UNKNOWN) {
        fprintf(stderr, "[Stream] Unknown format: %s\n", filepath);
        return NX_ERR_FORMAT;
    }
    
    /* Open file */
    stream->file = fopen(filepath, "rb");
    if (!stream->file) {
        fprintf(stderr, "[Stream] Cannot open: %s\n", filepath);
        return NX_ERR_IO;
    }
    
    /* Get file size */
    fseek(stream->file, 0, SEEK_END);
    stream->file_size = ftell(stream->file);
    fseek(stream->file, 0, SEEK_SET);
    
    /* Allocate decoder state */
    switch (stream->codec) {
        case NX_CODEC_WAV:
            stream->decoder_state_size = sizeof(wav_stream_state_t);
            stream->codec_ops = &wav_codec_ops;
            break;
        default:
            /* Only WAV streams from disk so far; other formats go through
             * the whole-file loader (codecs/loader.c) */
            fprintf(stderr, "[Stream] No streaming decoder for: %s\n", filepath);
            fclose(stream->file);
            stream->file = NULL;
            return NX_ERR_FORMAT;
    }
    
    stream->decoder_state = calloc(1, stream->decoder_state_size);
    if (!stream->decoder_state) {
        fclose(stream->file);
        return NX_ERR_MEMORY;
    }
    
    /* Allocate read buffer */
    stream->read_buffer = malloc(READ_BUFFER_SIZE);
    stream->read_buffer_size = READ_BUFFER_SIZE;
    if (!stream->read_buffer) {
        nx_stream_close(stream);
        return NX_ERR_MEMORY;
    }
    
    /* Open codec */
    nx_error_t err = stream->codec_ops->open(stream);
    if (err != NX_OK) {
        nx_stream_close(stream);
        return err;
    }
    
    stream->state = NX_STREAM_STOPPED;
    
    printf("[Stream] Opened: %s (%d Hz, %d ch, %.2f sec)\n",
           filepath, stream->sample_rate, stream->channels, stream->duration);
    
    return NX_OK;
}

nx_error_t nx_stream_open_pcm(nx_audio_stream_t *stream,
                              const float *samples, size_t frames,
                              uint32_t sample_rate, uint16_t channels) {
    if (!stream || !samples || sample_rate == 0 || channels == 0) return NX_ERR_INVALID;

    memset(stream, 0, sizeof(*stream));

    pcm_stream_state_t *state = (pcm_stream_state_t*)calloc(1, sizeof(*state));
    if (!state) return NX_ERR_MEMORY;
    state->samples = samples;

    stream->codec = NX_CODEC_PCM;
    stream->codec_ops = &pcm_codec_ops;
    stream->decoder_state = state;
    stream->decoder_state_size = sizeof(*state);
    stream->sample_rate = sample_rate;
    stream->channels = channels;
    stream->bits_per_sample = 32;
    stream->total_frames = frames;
    stream->duration = (float)frames / (float)sample_rate;
    stream->state = NX_STREAM_STOPPED;

    return NX_OK;
}

nx_error_t nx_stream_read(nx_audio_stream_t *stream, float *buffer, size_t *frames) {
    if (!stream || !buffer || !frames) return NX_ERR_INVALID;
    if (!stream->codec_ops) return NX_ERR_INVALID;
    
    return stream->codec_ops->read(stream, buffer, frames);
}

nx_error_t nx_stream_seek(nx_audio_stream_t *stream, size_t frame) {
    if (!stream) return NX_ERR_INVALID;
    if (!stream->codec_ops) return NX_ERR_INVALID;
    
    return stream->codec_ops->seek(stream, frame);
}

nx_error_t nx_stream_seek_time(nx_audio_stream_t *stream, float time_sec) {
    if (!stream) return NX_ERR_INVALID;
    
    size_t frame = (size_t)(time_sec * stream->sample_rate);
    return nx_stream_seek(stream, frame);
}

size_t nx_stream_tell(nx_audio_stream_t *stream) {
    return stream ? stream->current_frame : 0;
}

float nx_stream_tell_time(nx_audio_stream_t *stream) {
    if (!stream || stream->sample_rate == 0) return 0.0f;
    return (float)stream->current_frame / (float)stream->sample_rate;
}

void nx_stream_close(nx_audio_stream_t *stream) {
    if (!stream) return;
    
    if (stream->codec_ops && stream->codec_ops->close) {
        stream->codec_ops->close(stream);
    }
    
    if (stream->decoder_state) {
        free(stream->decoder_state);
    }
    if (stream->read_buffer) {
        free(stream->read_buffer);
    }
    if (stream->file) {
        fclose(stream->file);
    }
    
    memset(stream, 0, sizeof(*stream));
}

int nx_stream_eof(nx_audio_stream_t *stream) {
    if (!stream) return 1;
    return stream->current_frame >= stream->total_frames;
}

const char* nx_error_string(nx_error_t err) {
    switch (err) {
        case NX_OK:          return "Success";
        case NX_ERR_FORMAT:  return "Invalid format";
        case NX_ERR_DECODE:  return "Decode error";
        case NX_ERR_IO:      return "I/O error";
        case NX_ERR_MEMORY:  return "Out of memory";
        case NX_ERR_EOF:     return "End of file";
        case NX_ERR_INVALID: return "Invalid parameter";
        default:             return "Unknown error";
    }
}
