/*
 * NXAudio WASAPI Device (Windows)
 *
 * Shared-mode, event-driven render client on the default endpoint.
 * We hand WASAPI float stereo at our rate and let it convert to the
 * mix format (AUTOCONVERTPCM). All COM work happens on the audio
 * thread so the caller's apartment doesn't matter.
 *
 * Copyright (c) 2025 KetiveeAI - KETIVEEAI License
 */

#if defined(_WIN32)

#define COBJMACROS
#include "device.h"
#include "../platform/nx_thread.h"
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM      0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

/* Own copies so we don't depend on which GUIDs the SDK's import libs carry */
static const GUID NX_CLSID_MMDeviceEnumerator =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID NX_IID_IMMDeviceEnumerator =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID NX_IID_IAudioClient =
    {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID NX_IID_IAudioRenderClient =
    {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

typedef struct {
    nx_thread_t thread;
    HANDLE      ready;          /* set once the thread knows if init worked */
    int         init_ok;
} wasapi_device_t;

NX_THREAD_PROC(wasapi_thread, arg) {
    nx_device_t *dev = (nx_device_t*)arg;
    wasapi_device_t *wd = (wasapi_device_t*)dev->impl;

    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice           *endpoint = NULL;
    IAudioClient        *client = NULL;
    IAudioRenderClient  *render = NULL;
    HANDLE               event = NULL;
    UINT32               buffer_frames = 0;
    int                  started = 0;

    HRESULT com = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    HRESULT hr = CoCreateInstance(&NX_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  &NX_IID_IMMDeviceEnumerator, (void**)&enumerator);
    if (FAILED(hr)) goto done;

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &endpoint);
    if (FAILED(hr)) goto done;

    hr = IMMDevice_Activate(endpoint, &NX_IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&client);
    if (FAILED(hr)) goto done;

    WAVEFORMATEX fmt = {0};
    fmt.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels       = (WORD)dev->channels;
    fmt.nSamplesPerSec  = dev->sample_rate;
    fmt.wBitsPerSample  = 32;
    fmt.nBlockAlign     = (WORD)(fmt.nChannels * sizeof(float));
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    fmt.cbSize          = 0;

    /* Buffer of about four periods, in 100 ns units */
    REFERENCE_TIME duration = (REFERENCE_TIME)dev->period_frames * 4 * 10000000LL / dev->sample_rate;
    hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                 AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                 AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 duration, 0, &fmt, NULL);
    if (FAILED(hr)) goto done;

    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!event) goto done;

    if (FAILED(IAudioClient_SetEventHandle(client, event))) goto done;
    if (FAILED(IAudioClient_GetBufferSize(client, &buffer_frames))) goto done;
    if (FAILED(IAudioClient_GetService(client, &NX_IID_IAudioRenderClient, (void**)&render))) goto done;

    /* Pre-fill so the first period isn't a glitch */
    BYTE *data = NULL;
    if (SUCCEEDED(IAudioRenderClient_GetBuffer(render, buffer_frames, &data))) {
        dev->callback(dev->user, (float*)data, buffer_frames);
        IAudioRenderClient_ReleaseBuffer(render, buffer_frames, 0);
    }

    if (FAILED(IAudioClient_Start(client))) goto done;
    started = 1;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    wd->init_ok = 1;
    SetEvent(wd->ready);

    while (atomic_load(&dev->running)) {
        if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0) continue;

        UINT32 padding = 0;
        if (FAILED(IAudioClient_GetCurrentPadding(client, &padding))) break;

        UINT32 avail = buffer_frames - padding;
        if (avail == 0) continue;

        if (FAILED(IAudioRenderClient_GetBuffer(render, avail, &data))) break;
        dev->callback(dev->user, (float*)data, avail);
        IAudioRenderClient_ReleaseBuffer(render, avail, 0);
    }

done:
    if (!wd->init_ok) {
        fprintf(stderr, "[NXAudio] WASAPI init failed (0x%08lx)\n", (unsigned long)hr);
        SetEvent(wd->ready);
    }
    if (started) IAudioClient_Stop(client);
    if (render) IAudioRenderClient_Release(render);
    if (client) IAudioClient_Release(client);
    if (endpoint) IMMDevice_Release(endpoint);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    if (event) CloseHandle(event);
    if (SUCCEEDED(com)) CoUninitialize();
    NX_THREAD_RETURN;
}

static int wasapi_start(nx_device_t *dev) {
    wasapi_device_t *wd = (wasapi_device_t*)calloc(1, sizeof(*wd));
    if (!wd) return -1;

    wd->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!wd->ready) {
        free(wd);
        return -1;
    }

    dev->impl = wd;
    if (nx_thread_start(&wd->thread, wasapi_thread, dev) != 0) {
        CloseHandle(wd->ready);
        free(wd);
        dev->impl = NULL;
        return -1;
    }

    WaitForSingleObject(wd->ready, INFINITE);
    if (!wd->init_ok) {
        nx_thread_join(wd->thread);
        CloseHandle(wd->ready);
        free(wd);
        dev->impl = NULL;
        return -1;
    }
    return 0;
}

static void wasapi_stop(nx_device_t *dev) {
    wasapi_device_t *wd = (wasapi_device_t*)dev->impl;
    if (!wd) return;

    nx_thread_join(wd->thread);
    CloseHandle(wd->ready);
    free(wd);
    dev->impl = NULL;
}

const nx_device_backend_t nx_backend_wasapi = {
    .name  = "wasapi",
    .start = wasapi_start,
    .stop  = wasapi_stop,
};

#endif /* _WIN32 */
