/*
 * NXAudio Threading Helpers
 *
 * Mutex, thread and clock wrappers so the device backends and the
 * engine share one portable vocabulary (Win32 or pthreads).
 *
 * Copyright (c) 2025 KetiveeAI - KETIVEEAI License
 */

#ifndef NXAUDIO_THREAD_H
#define NXAUDIO_THREAD_H

#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef CRITICAL_SECTION nx_mutex_t;
typedef HANDLE           nx_thread_t;

#define NX_THREAD_PROC(name, arg) static DWORD WINAPI name(LPVOID arg)
#define NX_THREAD_RETURN          return 0

static inline void nx_mutex_init(nx_mutex_t *m)    { InitializeCriticalSection(m); }
static inline void nx_mutex_destroy(nx_mutex_t *m) { DeleteCriticalSection(m); }
static inline void nx_mutex_lock(nx_mutex_t *m)    { EnterCriticalSection(m); }
static inline void nx_mutex_unlock(nx_mutex_t *m)  { LeaveCriticalSection(m); }

static inline int nx_thread_start(nx_thread_t *t, LPTHREAD_START_ROUTINE fn, void *arg) {
    *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return *t ? 0 : -1;
}

static inline void nx_thread_join(nx_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

static inline uint64_t nx_time_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)((double)now.QuadPart * 1e9 / (double)freq.QuadPart);
}

static inline void nx_sleep_ns(uint64_t ns) {
    DWORD ms = (DWORD)(ns / 1000000);
    Sleep(ms ? ms : 1);
}

#else /* POSIX */
#include <pthread.h>
#include <time.h>

typedef pthread_mutex_t nx_mutex_t;
typedef pthread_t       nx_thread_t;

#define NX_THREAD_PROC(name, arg) static void *name(void *arg)
#define NX_THREAD_RETURN          return NULL

static inline void nx_mutex_init(nx_mutex_t *m)    { pthread_mutex_init(m, NULL); }
static inline void nx_mutex_destroy(nx_mutex_t *m) { pthread_mutex_destroy(m); }
static inline void nx_mutex_lock(nx_mutex_t *m)    { pthread_mutex_lock(m); }
static inline void nx_mutex_unlock(nx_mutex_t *m)  { pthread_mutex_unlock(m); }

static inline int nx_thread_start(nx_thread_t *t, void *(*fn)(void *), void *arg) {
    return pthread_create(t, NULL, fn, arg) == 0 ? 0 : -1;
}

static inline void nx_thread_join(nx_thread_t t) {
    pthread_join(t, NULL);
}

static inline uint64_t nx_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void nx_sleep_ns(uint64_t ns) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    nanosleep(&ts, NULL);
}

#endif

#endif /* NXAUDIO_THREAD_H */
