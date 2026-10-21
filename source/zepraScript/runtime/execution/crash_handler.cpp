// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file crash_handler.cpp
 * @brief Crash recovery implementation
 */


#ifndef _WIN32  // Crash handler uses POSIX signals — Windows needs SEH (TODO)

#include "runtime/execution/CrashHandler.h"
#include <algorithm>

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <thread>
#include <chrono>

#ifdef __linux__
#include <execinfo.h>
#endif

namespace Zepra::Runtime {

// =============================================================================
// Static member definitions
// =============================================================================

std::atomic<bool> CrashHandler::installed_{false};
std::vector<CrashHandler::CrashCallback> CrashHandler::callbacks_;
std::mutex CrashHandler::callbackMutex_;
CrashDumpWriter CrashHandler::dumpWriter_;

struct sigaction CrashHandler::prevSIGSEGV_{};
struct sigaction CrashHandler::prevSIGBUS_{};
struct sigaction CrashHandler::prevSIGABRT_{};
struct sigaction CrashHandler::prevSIGFPE_{};

thread_local size_t CrashHandler::activeIP_ = 0;
thread_local size_t CrashHandler::activeStackDepth_ = 0;
thread_local size_t CrashHandler::activeHeapUsed_ = 0;

// =============================================================================
// CrashContext
// =============================================================================

const char* CrashContext::signalName() const {
    switch (signal) {
        case SIGSEGV: return "SIGSEGV (Segmentation fault)";
        case SIGBUS:  return "SIGBUS (Bus error)";
        case SIGABRT: return "SIGABRT (Abort)";
        case SIGFPE:  return "SIGFPE (Floating-point exception)";
        default:      return "Unknown signal";
    }
}

std::string CrashContext::summary() const {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "ZepraScript Crash: %s\n"
        "  Fault address: %p\n"
        "  VM IP: %zu, Stack depth: %zu, Heap: %zu bytes\n"
        "  PID: %d, TID: %d\n"
        "  Stack frames: %zu captured",
        signalName(), faultAddress,
        instructionPointer, stackDepth, heapUsedBytes,
        processId, threadId,
        frameCount);
    return std::string(buf);
}

// =============================================================================
// CrashDumpWriter
// =============================================================================

CrashDumpWriter::CrashDumpWriter(const std::string& dumpDir)
    : dumpDir_(dumpDir) {}

void CrashDumpWriter::writeRaw(int fd, const char* data, size_t len) {
    while (len > 0) {
        ssize_t written = ::write(fd, data, len);
        if (written <= 0) break;
        data += written;
        len -= static_cast<size_t>(written);
    }
}

void CrashDumpWriter::writeInt(int fd, uint64_t value) {
    char buf[24];
    int len = std::snprintf(buf, sizeof(buf), "%lu", value);
    writeRaw(fd, buf, static_cast<size_t>(len));
}

void CrashDumpWriter::writeHex(int fd, uint64_t value) {
    char buf[20];
    int len = std::snprintf(buf, sizeof(buf), "0x%lx", value);
    writeRaw(fd, buf, static_cast<size_t>(len));
}

std::string CrashDumpWriter::writeDump(const CrashContext& ctx) {
    // Ensure dump directory exists
    ::mkdir(dumpDir_.c_str(), 0755);

    // Generate filename: zepra-crash-<timestamp>-<pid>.dump
    char filename[256];
    std::snprintf(filename, sizeof(filename),
        "%s/zepra-crash-%lu-%d.dump",
        dumpDir_.c_str(), ctx.timestampMs, ctx.processId);

    int fd = ::open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return "";

    // Write header (async-signal-safe: no malloc)
    const char* header = "=== ZepraScript Crash Dump ===\n";
    writeRaw(fd, header, std::strlen(header));

    const char* sig = "Signal: ";
    writeRaw(fd, sig, std::strlen(sig));
    writeRaw(fd, ctx.signalName(), std::strlen(ctx.signalName()));
    writeRaw(fd, "\n", 1);

    const char* fault = "Fault address: ";
    writeRaw(fd, fault, std::strlen(fault));
    writeHex(fd, reinterpret_cast<uint64_t>(ctx.faultAddress));
    writeRaw(fd, "\n", 1);

    const char* vmip = "VM IP: ";
    writeRaw(fd, vmip, std::strlen(vmip));
    writeInt(fd, ctx.instructionPointer);
    writeRaw(fd, "\n", 1);

    const char* sdep = "Stack depth: ";
    writeRaw(fd, sdep, std::strlen(sdep));
    writeInt(fd, ctx.stackDepth);
    writeRaw(fd, "\n", 1);

    const char* heap = "Heap used: ";
    writeRaw(fd, heap, std::strlen(heap));
    writeInt(fd, ctx.heapUsedBytes);
    writeRaw(fd, " bytes\n", 7);

    const char* pidl = "PID: ";
    writeRaw(fd, pidl, std::strlen(pidl));
    writeInt(fd, static_cast<uint64_t>(ctx.processId));
    writeRaw(fd, "\n", 1);

    // Stack trace
    const char* strace = "\n=== Stack Trace ===\n";
    writeRaw(fd, strace, std::strlen(strace));
    for (size_t i = 0; i < ctx.frameCount; i++) {
        const char* frame = "  #";
        writeRaw(fd, frame, 3);
        writeInt(fd, i);
        writeRaw(fd, ": ", 2);
        writeHex(fd, reinterpret_cast<uint64_t>(ctx.stackFrames[i]));
        writeRaw(fd, "\n", 1);
    }

    const char* footer = "\n=== End Crash Dump ===\n";
    writeRaw(fd, footer, std::strlen(footer));

    ::close(fd);
    return std::string(filename);
}

// =============================================================================
// CrashHandler
// =============================================================================

void CrashHandler::install() {
    if (installed_.exchange(true)) return;  // Already installed

    struct sigaction sa{};
    sa.sa_sigaction = signalHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;  // One-shot to avoid re-entry
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, &prevSIGSEGV_);
    sigaction(SIGBUS,  &sa, &prevSIGBUS_);
    sigaction(SIGABRT, &sa, &prevSIGABRT_);
    sigaction(SIGFPE,  &sa, &prevSIGFPE_);
}

void CrashHandler::uninstall() {
    if (!installed_.exchange(false)) return;

    sigaction(SIGSEGV, &prevSIGSEGV_, nullptr);
    sigaction(SIGBUS,  &prevSIGBUS_,  nullptr);
    sigaction(SIGABRT, &prevSIGABRT_, nullptr);
    sigaction(SIGFPE,  &prevSIGFPE_,  nullptr);
}

void CrashHandler::onCrash(CrashCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    callbacks_.push_back(std::move(callback));
}

void CrashHandler::setDumpDirectory(const std::string& dir) {
    dumpWriter_.setDumpDirectory(dir);
}

void CrashHandler::setActiveVMState(size_t ip, size_t stackDepth, size_t heapUsed) {
    activeIP_ = ip;
    activeStackDepth_ = stackDepth;
    activeHeapUsed_ = heapUsed;
}

CrashContext CrashHandler::captureContext(int sig, siginfo_t* info) {
    CrashContext ctx;
    ctx.signal = sig;
    ctx.signalCode = info ? info->si_code : 0;
    ctx.faultAddress = info ? info->si_addr : nullptr;

    ctx.instructionPointer = activeIP_;
    ctx.stackDepth = activeStackDepth_;
    ctx.heapUsedBytes = activeHeapUsed_;

    ctx.processId = ::getpid();
#ifdef SYS_gettid
    ctx.threadId = static_cast<pid_t>(::syscall(SYS_gettid));
#else
    ctx.threadId = ctx.processId;
#endif

    auto now = std::chrono::system_clock::now();
    ctx.timestampMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count());

    // Capture native stack trace
#ifdef __linux__
    ctx.frameCount = static_cast<size_t>(
        backtrace(ctx.stackFrames, CrashContext::MAX_FRAMES));
#endif

    return ctx;
}

void CrashHandler::signalHandler(int sig, siginfo_t* info, void* /*ucontext*/) {
    // Capture crash context (mostly signal-safe)
    CrashContext ctx = captureContext(sig, info);

    // Write crash dump (async-signal-safe path)
    dumpWriter_.writeDump(ctx);

    // Write summary to stderr (signal-safe)
    const char* msg = "\n[ZepraScript] Fatal signal received: ";
    ::write(STDERR_FILENO, msg, std::strlen(msg));
    ::write(STDERR_FILENO, ctx.signalName(), std::strlen(ctx.signalName()));
    ::write(STDERR_FILENO, "\n", 1);

    // Invoke callbacks (best-effort, not fully signal-safe)
    for (auto& cb : callbacks_) {
        try { cb(ctx); } catch (...) {}
    }

    // Re-raise to get default behavior (core dump)
    raise(sig);
}

// =============================================================================
// ExecutionWatchdog
// =============================================================================

ExecutionWatchdog::ExecutionWatchdog(uint32_t timeoutMs,
                                     std::function<void()> onTimeout)
    : timeoutMs_(timeoutMs)
    , onTimeout_(std::move(onTimeout)) {}

ExecutionWatchdog::~ExecutionWatchdog() {
    stop();
}

void ExecutionWatchdog::start() {
    if (running_.exchange(true)) return;
    timedOut_ = false;

    std::thread([this]() { watchdogLoop(); }).detach();
}

void ExecutionWatchdog::stop() {
    running_ = false;
}

void ExecutionWatchdog::watchdogLoop() {
    uint64_t lastProgress = progressCounter_.load(std::memory_order_relaxed);

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs_ / 10));

        if (!running_.load()) break;

        uint64_t current = progressCounter_.load(std::memory_order_relaxed);
        if (current == lastProgress) {
            // No progress â€” potential hang
            timedOut_ = true;
            if (terminationFlag_) {
                terminationFlag_->store(true, std::memory_order_release);
            }
            if (onTimeout_) {
                onTimeout_();
            }
            running_ = false;
            break;
        }
        lastProgress = current;
    }
}

} // namespace Zepra::Runtime


#endif // _WIN32

