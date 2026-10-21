// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file ZWasmFaultHandler.cpp
 * @brief ZepraScript signal-based memory bounds checking implementation
 */

#include "wasm/ZWasmFaultHandler.h"
#include <vector>
#include <cstdio>
#include <mutex>
#include <algorithm>

#if defined(__linux__) || defined(__APPLE__)
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#if defined(__linux__)
#include <ucontext.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif
#endif

#ifdef _WIN32
#include <windows.h>
#endif

namespace Zepra::Wasm {

// =============================================================================
// Thread-Local Fault State
// =============================================================================

thread_local ZTrapInfo ZFaultHandler::lastFault_{};
thread_local bool ZFaultHandler::inFaultHandler_ = false;

// =============================================================================
// Memory Region Registry
// =============================================================================

static std::mutex registryMutex;
static std::vector<WasmMemoryRegion> memoryRegions;
static bool handlersInstalled = false;

#if defined(__linux__) || defined(__APPLE__)
static struct sigaction oldSigSegv;
static struct sigaction oldSigBus;
#endif

// =============================================================================
// Guard Page Helpers
// =============================================================================

bool GuardPageConfig::isInGuardRegion(void* addr, void* memBase, size_t memSize) {
    auto addrVal = reinterpret_cast<uintptr_t>(addr);
    auto baseVal = reinterpret_cast<uintptr_t>(memBase);
    
    // Check if addr is after the valid memory but within mapped region
    if (addrVal >= baseVal + memSize && addrVal < baseVal + memSize + GuardSize) {
        return true;
    }
    
    return false;
}

bool WasmMemoryRegion::containsFaultAddress(void* addr) const {
    auto addrVal = reinterpret_cast<uintptr_t>(addr);
    auto baseVal = reinterpret_cast<uintptr_t>(base);
    
    // Fault is in valid region or guard region
    return addrVal >= baseVal && addrVal < baseVal + mappedSize;
}

// =============================================================================
// Region Management
// =============================================================================

void ZFaultHandler::registerMemoryRegion(const WasmMemoryRegion& region) {
    std::lock_guard<std::mutex> lock(registryMutex);
    memoryRegions.push_back(region);
}

void ZFaultHandler::unregisterMemoryRegion(void* base) {
    std::lock_guard<std::mutex> lock(registryMutex);
    memoryRegions.erase(
        std::remove_if(memoryRegions.begin(), memoryRegions.end(),
            [base](const WasmMemoryRegion& r) { return r.base == base; }),
        memoryRegions.end()
    );
}

WasmMemoryRegion* ZFaultHandler::findRegion(void* addr) {
    std::lock_guard<std::mutex> lock(registryMutex);
    for (auto& region : memoryRegions) {
        if (region.containsFaultAddress(addr)) {
            return &region;
        }
    }
    return nullptr;
}

ZTrapInfo ZFaultHandler::getLastFaultInfo() {
    return lastFault_;
}

bool ZFaultHandler::isInstalled() {
    return handlersInstalled;
}

// =============================================================================
// POSIX Signal Handler Implementation
// =============================================================================

#if defined(__linux__) || defined(__APPLE__)

static void posixSignalHandler(int sig, siginfo_t* info, void* ucontext) {
    if (ZFaultHandler::inFaultHandler_) {
        _exit(128 + sig);
    }
    
    ZFaultHandler::inFaultHandler_ = true;
    
    void* faultAddr = info->si_addr;
    
    WasmMemoryRegion* region = ZFaultHandler::findRegion(faultAddr);
    
    if (region) {
        ZFaultHandler::lastFault_.type = ZTrap::OutOfBoundsMemoryAccess;
        ZFaultHandler::lastFault_.faultingAddress = faultAddr;
        
        auto ctx = PlatformExceptionContext::from(ucontext);
        
        ctx.setPC(reinterpret_cast<void*>(&wasmTrapLandingPad));
        
        ZFaultHandler::inFaultHandler_ = false;
        return;
    }
    
    ZFaultHandler::inFaultHandler_ = false;
    
    struct sigaction* oldHandler = (sig == SIGSEGV) ? &oldSigSegv : &oldSigBus;
    
    if (oldHandler->sa_flags & SA_SIGINFO) {
        oldHandler->sa_sigaction(sig, info, ucontext);
    } else if (oldHandler->sa_handler == SIG_DFL) {
        signal(sig, SIG_DFL);
        raise(sig);
    } else if (oldHandler->sa_handler != SIG_IGN) {
        oldHandler->sa_handler(sig);
    }
}

bool ZFaultHandler::install() {
    if (handlersInstalled) {
        return true;
    }
    
    struct sigaction sa;
    sa.sa_sigaction = posixSignalHandler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    
    if (sigaction(SIGSEGV, &sa, &oldSigSegv) != 0) {
        return false;
    }
    
    if (sigaction(SIGBUS, &sa, &oldSigBus) != 0) {
        sigaction(SIGSEGV, &oldSigSegv, nullptr);
        return false;
    }
    
    handlersInstalled = true;
    return true;
}

void ZFaultHandler::uninstall() {
    if (!handlersInstalled) {
        return;
    }
    
    sigaction(SIGSEGV, &oldSigSegv, nullptr);
    sigaction(SIGBUS, &oldSigBus, nullptr);
    handlersInstalled = false;
}

// Platform exception context for Linux
#if defined(__linux__)

PlatformExceptionContext PlatformExceptionContext::from(void* ucontext) {
    auto* ctx = static_cast<ucontext_t*>(ucontext);
    PlatformExceptionContext result;
    
#if defined(__x86_64__)
    result.pc = reinterpret_cast<void*>(ctx->uc_mcontext.gregs[REG_RIP]);
    result.sp = reinterpret_cast<void*>(ctx->uc_mcontext.gregs[REG_RSP]);
    result.bp = reinterpret_cast<void*>(ctx->uc_mcontext.gregs[REG_RBP]);
#elif defined(__aarch64__)
    result.pc = reinterpret_cast<void*>(ctx->uc_mcontext.pc);
    result.sp = reinterpret_cast<void*>(ctx->uc_mcontext.sp);
    result.bp = reinterpret_cast<void*>(ctx->uc_mcontext.regs[29]);
#endif
    
    return result;
}

void PlatformExceptionContext::setPC(void* newPC) {
    // Platform-specific PC modification - done in signal handler via ucontext
}

#elif defined(__APPLE__)

PlatformExceptionContext PlatformExceptionContext::from(void* ucontext) {
    PlatformExceptionContext result;
    // macOS implementation uses mach exceptions
    result.pc = nullptr;
    result.sp = nullptr;
    result.bp = nullptr;
    return result;
}

void PlatformExceptionContext::setPC(void* newPC) {
    // macOS implementation
}

#endif // __linux__ / __APPLE__

#endif // POSIX

// =============================================================================
// Windows SEH Implementation
// =============================================================================

#ifdef _WIN32

static LONG WINAPI windowsExceptionHandler(EXCEPTION_POINTERS* exInfo) {
    if (exInfo->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    
    void* faultAddr = reinterpret_cast<void*>(
        exInfo->ExceptionRecord->ExceptionInformation[1]);
    
    WasmMemoryRegion* region = ZFaultHandler::findRegion(faultAddr);
    
    if (region) {
        ZFaultHandler::lastFault_.type = ZTrap::OutOfBoundsMemoryAccess;
        ZFaultHandler::lastFault_.faultingAddress = faultAddr;
        
        exInfo->ContextRecord->Rip = reinterpret_cast<DWORD64>(&wasmTrapLandingPad);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    
    return EXCEPTION_CONTINUE_SEARCH;
}

static PVOID vehHandle = nullptr;

bool ZFaultHandler::install() {
    if (handlersInstalled) {
        return true;
    }
    
    vehHandle = AddVectoredExceptionHandler(1, windowsExceptionHandler);
    if (!vehHandle) {
        return false;
    }
    
    handlersInstalled = true;
    return true;
}

void ZFaultHandler::uninstall() {
    if (!handlersInstalled) {
        return;
    }
    
    if (vehHandle) {
        RemoveVectoredExceptionHandler(vehHandle);
        vehHandle = nullptr;
    }
    
    handlersInstalled = false;
}

PlatformExceptionContext PlatformExceptionContext::from(void* exceptionInfo) {
    auto* exInfo = static_cast<EXCEPTION_POINTERS*>(exceptionInfo);
    PlatformExceptionContext result;
    
    result.pc = reinterpret_cast<void*>(exInfo->ContextRecord->Rip);
    result.sp = reinterpret_cast<void*>(exInfo->ContextRecord->Rsp);
    result.bp = reinterpret_cast<void*>(exInfo->ContextRecord->Rbp);
    result.faultAddr = reinterpret_cast<void*>(
        exInfo->ExceptionRecord->ExceptionInformation[1]);
    
    return result;
}

void PlatformExceptionContext::setPC(void* newPC) {
    // Set via CONTEXT modification
}

#endif // _WIN32

// =============================================================================
// Trap Landing Pad (Assembly Stub)
// =============================================================================

extern "C" void handleZWasmTrap(ZTrap type, uint32_t funcIdx, uint32_t offset) {
    const char* msg = trapMessage(type);
    fprintf(stderr, "ZWasm trap: %s at func %u offset %u\n", msg, funcIdx, offset);
    abort();
}

#if defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__))
__asm__(
    ".global wasmTrapLandingPad\n"
    "wasmTrapLandingPad:\n"
    "    movq %rsp, %rdi\n"
    "    call _handleZWasmTrapFromAsm\n"
    "    ud2\n"
);
#else
extern "C" void wasmTrapLandingPad() {
    handleZWasmTrap(ZTrap::Unreachable, 0, 0);
    abort();
}
#endif

} // namespace Zepra::Wasm
