// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file WasmSignalHandlers.cpp
 * @brief WebAssembly Signal Handling Implementation
 */

#include "wasm/WasmSignalHandlers.h"
#include <vector>
#include <mutex>
#include <algorithm>
#include <csignal>
#include <cstring>
#include <iostream>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#if defined(__linux__)
#include <ucontext.h>   // macOS: ucontext_t comes from <signal.h>
#endif
#endif

namespace Zepra::Wasm {

// Forward declaration for trap registry
struct GlobalTrapRegistry {
    static void notifyTrap(void*) {}
};

// =============================================================================
// JIT Registry (detect if crash is in WASM code)
// =============================================================================

struct CodeRegion {
    uintptr_t start;
    uintptr_t end;
    
    bool contains(uintptr_t pc) const {
        return pc >= start && pc < end;
    }
};

static std::vector<CodeRegion> s_jitRegions;
static std::mutex s_regionMutex;

// Thread-local flag to indicate we are running JIT code
// This creates a fast path so we don't check regions for non-WASM crashes
static thread_local bool s_inJITCode = false;

void SignalHandlers::registerJITRegion(uintptr_t start, size_t size) {
    std::lock_guard<std::mutex> lock(s_regionMutex);
    s_jitRegions.push_back({start, start + size});
    // Keep sorted for binary search
    std::sort(s_jitRegions.begin(), s_jitRegions.end(), 
        [](const CodeRegion& a, const CodeRegion& b) {
            return a.start < b.start;
        });
}

void SignalHandlers::unregisterJITRegion(uintptr_t start, size_t size) {
    std::lock_guard<std::mutex> lock(s_regionMutex);
    auto it = std::remove_if(s_jitRegions.begin(), s_jitRegions.end(),
        [start](const CodeRegion& r) { return r.start == start; });
    s_jitRegions.erase(it, s_jitRegions.end());
}

bool SignalHandlers::isJITAddress(uintptr_t pc) {
    if (!s_inJITCode) return false;
    
    // Don't lock inside signal handler if possible, but safe here since we just read
    // Real implementation should use a lock-free structure or spinlock
    // std::lock_guard<std::mutex> lock(s_regionMutex); // Unsafe in signal handler!
    
    // Linear scan is safe(ish) if we assume vector doesn't reallocate during execution
    // (In production this needs a safe read-copy-update mechanism)
    for (const auto& region : s_jitRegions) {
        if (region.contains(pc)) return true;
    }
    return false;
}

void SignalHandlers::enterJITCode() {
    s_inJITCode = true;
}

void SignalHandlers::exitJITCode() {
    s_inJITCode = false;
}

// =============================================================================
// Signal Handler Implementation
// =============================================================================

#if defined(__linux__) || defined(__APPLE__)

static struct sigaction s_oldSegvHandler;
static struct sigaction s_oldBusHandler;

#include "wasm/WasmTrapHandler.h"

// ... existing code ...

static void handleWasmFault(int signum, siginfo_t* info, void* context) {
    ucontext_t* ctx = static_cast<ucontext_t*>(context);
    
    // Get PC (Program Counter) from context
    uintptr_t pc = 0;
#if defined(__APPLE__) && defined(__aarch64__)
    pc = ctx->uc_mcontext->__ss.__pc;
#elif defined(__APPLE__) && defined(__x86_64__)
    pc = ctx->uc_mcontext->__ss.__rip;
#elif defined(__x86_64__)
    pc = ctx->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    pc = ctx->uc_mcontext.pc;
#endif

    // Check if crash happened in WASM JIT code
    if (SignalHandlers::isJITAddress(pc)) {
        // Identify trap reason
        uintptr_t faultAddr = reinterpret_cast<uintptr_t>(info->si_addr);
        WasmTrapException::TrapReason reason = WasmTrapException::TrapReason::OutOfBounds;
        
        // Guess based on fault address
        if (faultAddr == 0) {
            reason = WasmTrapException::TrapReason::NullDereference;
        }
        
        // TODO: Redirect to thunk
        const char* msg = "[ZepraWasm] Trapped signal in JIT code! Recovering...\n";
        write(STDERR_FILENO, msg, strlen(msg));
        
        // Stop recursion for now
        signal(signum, SIG_DFL);
        return;
    }
    
    // ... existing chain logic ...

    // Not our crash, chain to old handler
    if (signum == SIGSEGV) {
        if (s_oldSegvHandler.sa_flags & SA_SIGINFO) {
            s_oldSegvHandler.sa_sigaction(signum, info, context);
        } else if (s_oldSegvHandler.sa_handler != SIG_DFL && s_oldSegvHandler.sa_handler != SIG_IGN) {
            s_oldSegvHandler.sa_handler(signum);
        } else {
            // Restore default and re-raise
             signal(signum, SIG_DFL);
             raise(signum);
        }
    }
}

void SignalHandlers::installHandlers() {
    struct sigaction sa;
    sa.sa_sigaction = handleWasmFault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER; // Allow recursive signals if needed
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGSEGV, &sa, &s_oldSegvHandler) != 0) {
        perror("Failed to install SIGSEGV handler");
    }
    if (sigaction(SIGBUS, &sa, &s_oldBusHandler) != 0) {
         perror("Failed to install SIGBUS handler");
    }
}

#else

void SignalHandlers::installHandlers() {
    // Windows/Other implementation
}

#endif

void SignalHandlers::init() {
    static bool initialized = false;
    if (!initialized) {
        installHandlers();
        initialized = true;
    }
}

} // namespace Zepra::Wasm
