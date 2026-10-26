/*
 * NXAudio CoreAudio Device (macOS)
 *
 * Default output AudioUnit with a render callback. The unit converts
 * our float stereo at our rate to whatever the hardware runs at, and
 * calls us on its own real-time thread.
 *
 * Copyright (c) 2025 KetiveeAI - KETIVEEAI License
 */

#if defined(__APPLE__)

#include "device.h"
#include <AudioToolbox/AudioToolbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    AudioComponentInstance unit;
} coreaudio_device_t;

static OSStatus coreaudio_render(void *ref,
                                 AudioUnitRenderActionFlags *flags,
                                 const AudioTimeStamp *timestamp,
                                 UInt32 bus,
                                 UInt32 frames,
                                 AudioBufferList *io) {
    (void)flags; (void)timestamp; (void)bus;
    nx_device_t *dev = (nx_device_t*)ref;

    for (UInt32 i = 0; i < io->mNumberBuffers; i++) {
        AudioBuffer *buf = &io->mBuffers[i];
        if (i == 0 && atomic_load(&dev->running) &&
            buf->mDataByteSize >= frames * dev->channels * sizeof(float)) {
            dev->callback(dev->user, (float*)buf->mData, frames);
        } else {
            memset(buf->mData, 0, buf->mDataByteSize);
        }
    }
    return noErr;
}

static int coreaudio_start(nx_device_t *dev) {
    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_DefaultOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };

    AudioComponent component = AudioComponentFindNext(NULL, &desc);
    if (!component) return -1;

    coreaudio_device_t *cd = (coreaudio_device_t*)calloc(1, sizeof(*cd));
    if (!cd) return -1;

    OSStatus st = AudioComponentInstanceNew(component, &cd->unit);
    if (st != noErr) {
        free(cd);
        return -1;
    }

    AudioStreamBasicDescription fmt = {
        .mSampleRate = dev->sample_rate,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = dev->channels * sizeof(float),
        .mFramesPerPacket = 1,
        .mBytesPerFrame = dev->channels * sizeof(float),
        .mChannelsPerFrame = dev->channels,
        .mBitsPerChannel = 32,
    };
    st = AudioUnitSetProperty(cd->unit, kAudioUnitProperty_StreamFormat,
                              kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));
    if (st != noErr) goto fail;

    AURenderCallbackStruct cb = { .inputProc = coreaudio_render, .inputProcRefCon = dev };
    st = AudioUnitSetProperty(cd->unit, kAudioUnitProperty_SetRenderCallback,
                              kAudioUnitScope_Input, 0, &cb, sizeof(cb));
    if (st != noErr) goto fail;

    st = AudioUnitInitialize(cd->unit);
    if (st != noErr) goto fail;

    dev->impl = cd;
    st = AudioOutputUnitStart(cd->unit);
    if (st != noErr) {
        AudioUnitUninitialize(cd->unit);
        goto fail;
    }
    return 0;

fail:
    fprintf(stderr, "[NXAudio] CoreAudio init failed (%d)\n", (int)st);
    AudioComponentInstanceDispose(cd->unit);
    free(cd);
    dev->impl = NULL;
    return -1;
}

static void coreaudio_stop(nx_device_t *dev) {
    coreaudio_device_t *cd = (coreaudio_device_t*)dev->impl;
    if (!cd) return;

    /* running is already 0, so a callback racing this stop writes silence */
    AudioOutputUnitStop(cd->unit);
    AudioUnitUninitialize(cd->unit);
    AudioComponentInstanceDispose(cd->unit);
    free(cd);
    dev->impl = NULL;
}

const nx_device_backend_t nx_backend_coreaudio = {
    .name  = "coreaudio",
    .start = coreaudio_start,
    .stop  = coreaudio_stop,
};

#endif /* __APPLE__ */
