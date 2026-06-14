// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file WasmStackManager.cpp
 * @brief WebAssembly Stack Overflow Protection Implementation
 *
 * Manages per-thread stack limits and guard regions for safe WASM execution.
 * Prevents stack overflow in WASM code from crashing the process.
 *
 * Platform strategy:
 *   Linux  — pthread_getattr_np() + pthread_attr_getstack()
 *   macOS  — pthread_get_stackaddr_np() + pthread_get_stacksize_np()
 *   Windows — VirtualQuery() on current stack pointer
 *
 * Guard reservation: 64 KB above the platform limit (traps before SIGSEGV).
 */

#include "wasm/WasmStackManager.h"

#ifdef __linux__
#  include <pthread.h>
#  include <sys/resource.h>
#elif defined(_WIN32)
#  include <windows.h>
#  include <winnt.h>
#elif defined(__APPLE__)
#  include <pthread.h>
#endif

#include <cstdint>
#include <cstring>

namespace Zepra::Wasm {

thread_local uintptr_t StackManager::threadLimit_ = 0;

// ---------------------------------------------------------------------------
// Internal: probe the current thread's stack base and size.
// Returns false if the platform query fails — caller falls back to rlimit.
// ---------------------------------------------------------------------------

static bool probeThreadStack(void** outBase, size_t* outSize) {
#if defined(__linux__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return false;
    int rc = pthread_attr_getstack(&attr, outBase, outSize);
    pthread_attr_destroy(&attr);
    return rc == 0;

#elif defined(__APPLE__)
    pthread_t self = pthread_self();
    void* addr = pthread_get_stackaddr_np(self);
    size_t sz   = pthread_get_stacksize_np(self);
    if (!addr || sz == 0) return false;
    // pthread_get_stackaddr_np returns the *top* (high address) of the stack.
    *outBase = static_cast<uint8_t*>(addr) - sz;
    *outSize = sz;
    return true;

#elif defined(_WIN32)
    // Walk the virtual address space of the current stack pointer.
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t sp;
#  if defined(_M_X64) || defined(__x86_64__)
    sp = (uintptr_t)_AddressOfReturnAddress();
#  else
    sp = (uintptr_t)&sp;
#  endif
    if (VirtualQuery(reinterpret_cast<LPCVOID>(sp), &mbi, sizeof(mbi)) == 0)
        return false;
    *outBase = mbi.AllocationBase;
    *outSize = static_cast<size_t>(
        reinterpret_cast<uintptr_t>(mbi.AllocationBase) +
        mbi.RegionSize - reinterpret_cast<uintptr_t>(mbi.AllocationBase));
    return true;

#else
    (void)outBase; (void)outSize;
    return false;
#endif
}

// ---------------------------------------------------------------------------
// initThreadStack — called once per thread before WASM execution begins.
// ---------------------------------------------------------------------------

void StackManager::initThreadStack(size_t guardReserve) {
    void*  stackBase = nullptr;
    size_t stackSize = 0;

    if (!probeThreadStack(&stackBase, &stackSize)) {
        // Fallback: query soft stack limit via getrlimit (POSIX) or use a
        // conservative 1 MB floor so execution can at least detect overflow.
#if !defined(_WIN32)
        struct rlimit rl;
        if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
            stackSize = static_cast<size_t>(rl.rlim_cur);
        } else {
            stackSize = 1u << 20; // 1 MB conservative floor
        }
        // Approximate base from current SP.
        uintptr_t sp;
#if defined(__x86_64__) || defined(_M_X64)
        __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
#elif defined(__aarch64__)
        __asm__ volatile("mov %0, sp" : "=r"(sp));
#else
        // Generic C fallback: address of a local is near the current SP.
        sp = reinterpret_cast<uintptr_t>(&sp);
#endif
        // Round down to nearest page.
        sp &= ~(uintptr_t)(4096 - 1);
        stackBase = reinterpret_cast<void*>(sp - stackSize);
#else
        stackSize = 1u << 20;
        stackBase = nullptr;
#endif
    }

    // Set the limit: bottom of stack + guard reserve.
    // Any SP below this value is in the danger zone.
    uintptr_t base = reinterpret_cast<uintptr_t>(stackBase);
    threadLimit_   = base + guardReserve;
}

// ---------------------------------------------------------------------------
// checkStackDepth — inline-friendly overflow check used by the WASM interp.
// ---------------------------------------------------------------------------

bool StackManager::isStackSafe(uintptr_t currentSP) const {
    if (threadLimit_ == 0) return true; // Not yet initialized — permissive.
    return currentSP > threadLimit_;
}

} // namespace Zepra::Wasm

