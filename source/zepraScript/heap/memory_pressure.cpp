// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file memory_pressure.cpp
 * @brief OS memory pressure monitoring, OOM handling, and heap advisor
 *
 * Monitors system memory state via:
 * - /proc/meminfo (physical memory)
 * - /proc/self/status (process RSS, VM size)
 * - cgroup v2 memory.current / memory.max (container limits)
 * - Linux PSI (Pressure Stall Information) via /proc/pressure/memory
 * - eventfd-based notifications from cgroup memory.events
 *
 * Responds to pressure:
 * - Low: no action
 * - Moderate: trigger incremental GC, shrink heap
 * - Critical: emergency full GC, release caches, madvise(DONTNEED)
 * - OOM: last-resort GC, log diagnostics, invoke embedder callback
 */

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <functional>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <memory>
#include <algorithm>

#ifdef __linux__
#if ZEPRA_PLATFORM_POSIX
#include <unistd.h>
#endif
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <fcntl.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace Zepra::Heap {

// =============================================================================
// Memory Pressure Levels
// =============================================================================

enum class PressureLevel : uint8_t {
    None,       // > 20% free
    Low,        // 10-20% free
    Moderate,   // 5-10% free
    Critical,   // < 5% free
    OOM,        // Allocation failure imminent
};

static const char* pressureLevelName(PressureLevel l) {
    switch (l) {
        case PressureLevel::None: return "None";
        case PressureLevel::Low: return "Low";
        case PressureLevel::Moderate: return "Moderate";
        case PressureLevel::Critical: return "Critical";
        case PressureLevel::OOM: return "OOM";
        default: return "Unknown";
    }
}

// =============================================================================
// System Memory Info
// =============================================================================

struct SystemMemoryInfo {
    size_t totalPhysical;       // Total physical RAM
    size_t availablePhysical;   // Available (free + reclaimable)
    size_t freePhysical;        // Strictly free
    size_t buffered;            // Buffer/cache (reclaimable)
    size_t swapTotal;
    size_t swapFree;

    // Derived
    double usedRatio() const {
        return totalPhysical > 0
            ? 1.0 - static_cast<double>(availablePhysical) /
                     static_cast<double>(totalPhysical)
            : 0;
    }
    double freeRatio() const {
        return 1.0 - usedRatio();
    }
    bool swapActive() const {
        return swapTotal > 0 && swapFree < swapTotal;
    }
};

// =============================================================================
// Process Memory Info
// =============================================================================

struct ProcessMemoryInfo {
    size_t rss;             // Resident set (physical memory used)
    size_t vmSize;          // Virtual memory total
    size_t vmPeak;          // Peak virtual memory
    size_t sharedMem;       // Shared memory
    size_t privateAnon;     // Private anonymous (heap + mmap)

    // cgroup
    size_t cgroupCurrent;   // memory.current
    size_t cgroupMax;       // memory.max
    size_t cgroupHigh;      // memory.high (throttle threshold)
    bool cgroupAvailable;

    double cgroupUsedRatio() const {
        return cgroupMax > 0
            ? static_cast<double>(cgroupCurrent) /
              static_cast<double>(cgroupMax)
            : 0;
    }
};

// =============================================================================
// /proc/meminfo Parser
// =============================================================================

class ProcMeminfoReader {
public:
    bool read(SystemMemoryInfo& info) {
#ifdef __linux__
        FILE* f = fopen("/proc/meminfo", "r");
        if (!f) return false;

        char line[256];
        info = {};

        while (fgets(line, sizeof(line), f)) {
            size_t value = 0;
            if (sscanf(line, "MemTotal: %zu kB", &value) == 1)
                info.totalPhysical = value * 1024;
            else if (sscanf(line, "MemAvailable: %zu kB", &value) == 1)
                info.availablePhysical = value * 1024;
            else if (sscanf(line, "MemFree: %zu kB", &value) == 1)
                info.freePhysical = value * 1024;
            else if (sscanf(line, "Buffers: %zu kB", &value) == 1)
                info.buffered += value * 1024;
            else if (sscanf(line, "Cached: %zu kB", &value) == 1)
                info.buffered += value * 1024;
            else if (sscanf(line, "SwapTotal: %zu kB", &value) == 1)
                info.swapTotal = value * 1024;
            else if (sscanf(line, "SwapFree: %zu kB", &value) == 1)
                info.swapFree = value * 1024;
        }
        fclose(f);

        // Fallback if MemAvailable not present (older kernels)
        if (info.availablePhysical == 0) {
            info.availablePhysical = info.freePhysical + info.buffered;
        }

        return info.totalPhysical > 0;
#elif defined(_WIN32)
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof(ms);
        if (!GlobalMemoryStatusEx(&ms)) return false;
        info.totalPhysical = static_cast<size_t>(ms.ullTotalPhys);
        info.availablePhysical = static_cast<size_t>(ms.ullAvailPhys);
        info.freePhysical = static_cast<size_t>(ms.ullAvailPhys);
        info.buffered = 0;  // Windows does not expose buffer/cache separately
        info.swapTotal = static_cast<size_t>(
            ms.ullTotalPageFile > ms.ullTotalPhys
                ? ms.ullTotalPageFile - ms.ullTotalPhys : 0);
        info.swapFree = static_cast<size_t>(
            ms.ullAvailPageFile > ms.ullAvailPhys
                ? ms.ullAvailPageFile - ms.ullAvailPhys : 0);
        return true;
#else
        (void)info;
        return false;
#endif
    }
};

// =============================================================================
// /proc/self/status Parser
// =============================================================================

class ProcSelfStatusReader {
public:
    bool read(ProcessMemoryInfo& info) {
#ifdef __linux__
        FILE* f = fopen("/proc/self/status", "r");
        if (!f) return false;

        char line[256];
        while (fgets(line, sizeof(line), f)) {
            size_t value = 0;
            if (sscanf(line, "VmRSS: %zu kB", &value) == 1)
                info.rss = value * 1024;
            else if (sscanf(line, "VmSize: %zu kB", &value) == 1)
                info.vmSize = value * 1024;
            else if (sscanf(line, "VmPeak: %zu kB", &value) == 1)
                info.vmPeak = value * 1024;
            else if (sscanf(line, "RssAnon: %zu kB", &value) == 1)
                info.privateAnon = value * 1024;
            else if (sscanf(line, "RssShmem: %zu kB", &value) == 1)
                info.sharedMem = value * 1024;
        }
        fclose(f);
        return info.rss > 0;
#elif defined(_WIN32)
        PROCESS_MEMORY_COUNTERS_EX pmc;
        pmc.cb = sizeof(pmc);
        if (!GetProcessMemoryInfo(GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
            return false;
        info.rss = pmc.WorkingSetSize;
        info.vmSize = pmc.PrivateUsage;
        info.vmPeak = pmc.PeakWorkingSetSize;
        info.privateAnon = pmc.PrivateUsage;
        info.sharedMem = pmc.WorkingSetSize > pmc.PrivateUsage
            ? pmc.WorkingSetSize - pmc.PrivateUsage : 0;
        return true;
#else
        (void)info;
        return false;
#endif
    }
};

// =============================================================================
// cgroup v2 Memory Reader
// =============================================================================

class CgroupMemoryReader {
public:
    bool read(ProcessMemoryInfo& info) {
#ifdef __linux__
        info.cgroupAvailable = false;

        // Detect cgroup v2 path
        char cgroupPath[512] = {};
        FILE* f = fopen("/proc/self/cgroup", "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                // cgroup v2: "0::/path"
                if (strncmp(line, "0::", 3) == 0) {
                    char* path = line + 3;
                    char* nl = strchr(path, '\n');
                    if (nl) *nl = '\0';
                    snprintf(cgroupPath, sizeof(cgroupPath),
                             "/sys/fs/cgroup%s", path);
                    break;
                }
            }
            fclose(f);
        }

        if (cgroupPath[0] == '\0') return false;

        // Read memory.current
        char filePath[600];
        snprintf(filePath, sizeof(filePath), "%s/memory.current", cgroupPath);
        info.cgroupCurrent = readCgroupValue(filePath);

        // Read memory.max
        snprintf(filePath, sizeof(filePath), "%s/memory.max", cgroupPath);
        info.cgroupMax = readCgroupValue(filePath);

        // Read memory.high
        snprintf(filePath, sizeof(filePath), "%s/memory.high", cgroupPath);
        info.cgroupHigh = readCgroupValue(filePath);

        info.cgroupAvailable = info.cgroupMax > 0;
        return info.cgroupAvailable;
#else
        (void)info;
        return false;
#endif
    }

private:
    static size_t readCgroupValue(const char* path) {
#ifdef __linux__
        FILE* f = fopen(path, "r");
        if (!f) return 0;
        char buf[64];
        if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
        fclose(f);
        // "max" means unlimited
        if (strncmp(buf, "max", 3) == 0) return SIZE_MAX;
        return static_cast<size_t>(strtoull(buf, nullptr, 10));
#else
        (void)path;
        return 0;
#endif
    }
};

// =============================================================================
// Memory Pressure Monitor
// =============================================================================

class MemoryPressureMonitor {
public:
    struct Config {
        double lowThreshold;        // Free ratio below this = Low
        double moderateThreshold;   // Free ratio below this = Moderate
        double criticalThreshold;   // Free ratio below this = Critical
        uint32_t pollIntervalMs;    // How often to check
        bool useCgroups;
        bool useProcessRSS;

        Config()
            : lowThreshold(0.20)
            , moderateThreshold(0.10)
            , criticalThreshold(0.05)
            , pollIntervalMs(1000)
            , useCgroups(true)
            , useProcessRSS(true) {}
    };

    struct State {
        PressureLevel level;
        SystemMemoryInfo system;
        ProcessMemoryInfo process;
        uint64_t lastCheckUs;
        uint32_t consecutiveCritical;
    };

    using PressureCallback = std::function<void(PressureLevel level,
                                                  const State& state)>;

    explicit MemoryPressureMonitor(const Config& config = Config{})
        : config_(config) {}

    ~MemoryPressureMonitor() { stopPolling(); }

    void setPressureCallback(PressureCallback callback) {
        callback_ = std::move(callback);
    }

    /**
     * @brief Check memory pressure now (synchronous)
     */
    State checkNow() {
        State state{};
        state.lastCheckUs = nowUs();

        meminfoReader_.read(state.system);
        if (config_.useProcessRSS) {
            statusReader_.read(state.process);
        }
        if (config_.useCgroups) {
            cgroupReader_.read(state.process);
        }

        state.level = classifyPressure(state);

        if (state.level >= PressureLevel::Critical) {
            state.consecutiveCritical = lastState_.consecutiveCritical + 1;
        } else {
            state.consecutiveCritical = 0;
        }

        // Notify if level changed
        if (state.level != lastState_.level && callback_) {
            callback_(state.level, state);
        }

        lastState_ = state;
        return state;
    }

    /**
     * @brief Start background polling
     */
    void startPolling() {
        if (polling_.load(std::memory_order_acquire)) return;
        polling_.store(true, std::memory_order_release);

        pollThread_ = std::thread([this]() {
            while (polling_.load(std::memory_order_acquire)) {
                checkNow();
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(config_.pollIntervalMs));
            }
        });
    }

    void stopPolling() {
        polling_.store(false, std::memory_order_release);
        if (pollThread_.joinable()) {
            pollThread_.join();
        }
    }

    const State& lastState() const { return lastState_; }
    const Config& config() const { return config_; }

private:
    PressureLevel classifyPressure(const State& state) const {
        double freeRatio;

        // Prefer cgroup limits when in a container
        if (state.process.cgroupAvailable &&
            state.process.cgroupMax < SIZE_MAX) {
            freeRatio = 1.0 - state.process.cgroupUsedRatio();
        } else {
            freeRatio = state.system.freeRatio();
        }

        if (freeRatio < config_.criticalThreshold)
            return PressureLevel::Critical;
        if (freeRatio < config_.moderateThreshold)
            return PressureLevel::Moderate;
        if (freeRatio < config_.lowThreshold)
            return PressureLevel::Low;
        return PressureLevel::None;
    }

    static uint64_t nowUs() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    Config config_;
    PressureCallback callback_;
    State lastState_{};

    ProcMeminfoReader meminfoReader_;
    ProcSelfStatusReader statusReader_;
    CgroupMemoryReader cgroupReader_;

    std::atomic<bool> polling_{false};
    std::thread pollThread_;
};

// =============================================================================
// OOM Handler
// =============================================================================

/**
 * @brief Last-resort OOM handler
 *
 * When the heap cannot grow and GC cannot reclaim enough:
 * 1. Log diagnostic information
 * 2. Attempt emergency GC (aggressive, full compaction)
 * 3. Release non-essential caches (compiled code, IC data)
 * 4. If still OOM, invoke embedder's fatal callback
 */
class OOMHandler {
public:
    using EmergencyGCFn = std::function<size_t()>;  // Returns bytes reclaimed
    using FatalCallback = std::function<void(size_t requestedBytes,
                                              size_t heapUsed,
                                              size_t heapLimit)>;
    using CacheReleaseFn = std::function<size_t()>; // Returns bytes freed

    void setEmergencyGC(EmergencyGCFn fn) { emergencyGC_ = std::move(fn); }
    void setFatalCallback(FatalCallback fn) { fatalCb_ = std::move(fn); }

    void addCacheReleaser(const char* name, CacheReleaseFn fn) {
        releasers_.push_back({name, std::move(fn)});
    }

    /**
     * @brief Handle an OOM situation
     * @return true if memory was freed and allocation can be retried
     */
    bool handleOOM(size_t requestedBytes, size_t heapUsed, size_t heapLimit) {
        oomCount_++;

        fprintf(stderr,
            "[ZepraBrowser GC] OOM #%u: requested=%zu heap=%zu/%zu\n",
            oomCount_, requestedBytes, heapUsed, heapLimit);

        // Step 1: Emergency GC
        if (emergencyGC_) {
            size_t reclaimed = emergencyGC_();
            fprintf(stderr,
                "[ZepraBrowser GC] Emergency GC reclaimed %zu bytes\n",
                reclaimed);
            if (reclaimed >= requestedBytes) return true;
        }

        // Step 2: Release caches
        size_t totalFreed = 0;
        for (auto& rel : releasers_) {
            size_t freed = rel.fn();
            fprintf(stderr,
                "[ZepraBrowser GC] Released %zu bytes from %s\n",
                freed, rel.name);
            totalFreed += freed;
        }
        if (totalFreed >= requestedBytes) return true;

        // Step 3: Fatal
        if (fatalCb_) {
            fatalCb_(requestedBytes, heapUsed, heapLimit);
        }

        return false;
    }

    uint32_t oomCount() const { return oomCount_; }

private:
    struct CacheReleaser {
        const char* name;
        CacheReleaseFn fn;
    };

    EmergencyGCFn emergencyGC_;
    FatalCallback fatalCb_;
    std::vector<CacheReleaser> releasers_;
    uint32_t oomCount_ = 0;
};

// =============================================================================
// Heap Advisor
// =============================================================================

/**
 * @brief Advises the heap on sizing decisions based on memory pressure
 *
 * Provides target heap size recommendations:
 * - No pressure: allow growth up to max
 * - Moderate: cap at current + small margin
 * - Critical: shrink to live data + minimal headroom
 */
class HeapAdvisor {
public:
    struct Advice {
        size_t targetHeapSize;
        bool shouldShrink;
        bool shouldReleasePages;
        bool shouldDisableJIT;      // JIT code takes memory
    };

    Advice advise(PressureLevel pressure, size_t liveBytes,
                  size_t currentHeapSize, size_t maxHeapSize) {
        Advice advice{};
        advice.shouldDisableJIT = false;

        switch (pressure) {
            case PressureLevel::None:
            case PressureLevel::Low:
                // Normal: allow growth
                advice.targetHeapSize = std::min(
                    currentHeapSize * 2, maxHeapSize);
                advice.shouldShrink = false;
                advice.shouldReleasePages = false;
                break;

            case PressureLevel::Moderate:
                // Moderate: cap growth
                advice.targetHeapSize = std::min(
                    liveBytes * 3 / 2, maxHeapSize);
                advice.shouldShrink = currentHeapSize > advice.targetHeapSize;
                advice.shouldReleasePages = true;
                break;

            case PressureLevel::Critical:
                // Critical: shrink aggressively
                advice.targetHeapSize = liveBytes * 5 / 4;  // 25% headroom
                advice.shouldShrink = true;
                advice.shouldReleasePages = true;
                advice.shouldDisableJIT = true;
                break;

            case PressureLevel::OOM:
                // OOM: absolute minimum
                advice.targetHeapSize = liveBytes + (1 << 20);  // +1MB
                advice.shouldShrink = true;
                advice.shouldReleasePages = true;
                advice.shouldDisableJIT = true;
                break;
        }

        return advice;
    }
};

} // namespace Zepra::Heap
