// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_heap_sizing.cpp — Pressure-based dynamic heap growth/shrink policy

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <cmath>
#include <algorithm>

#ifdef __linux__
#include <fstream>
#include <string>
#if ZEPRA_PLATFORM_POSIX
#include <unistd.h>
#endif
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Zepra::Heap {

// =========================================================================
// Pressure Levels (0 = relaxed, 3 = emergency)
//
// GC aggressiveness scales with pressure level.
// Foreground tab is NEVER pressured — only background tabs get compacted.
// No hardcoded RAM floor — the OS pressure signal decides everything.
// =========================================================================

enum class SystemPressure : uint8_t {
    Relaxed   = 0,   // Plenty of RAM — allow growth
    Moderate  = 1,   // Starting to fill — cap growth
    High      = 2,   // Low available RAM — shrink actively
    Emergency = 3,   // Critical — shrink aggressively, decommit, flush caches
};

// Query the OS for available physical memory.
// Returns bytes of available RAM, or 0 if unknown.
static size_t queryAvailableRAM() {
#ifdef __linux__
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        while (std::getline(meminfo, line)) {
            size_t val = 0;
            if (sscanf(line.c_str(), "MemAvailable: %zu kB", &val) == 1) {
                return val * 1024;
            }
        }
    }
    return 0;
#elif defined(_WIN32)
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        return static_cast<size_t>(ms.ullAvailPhys);
    }
    return 0;
#else
    return 0;
#endif
}

// Query the OS for total physical memory.
static size_t queryTotalRAM() {
#ifdef __linux__
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        while (std::getline(meminfo, line)) {
            size_t val = 0;
            if (sscanf(line.c_str(), "MemTotal: %zu kB", &val) == 1) {
                return val * 1024;
            }
        }
    }
    return 0;
#elif defined(_WIN32)
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        return static_cast<size_t>(ms.ullTotalPhys);
    }
    return 0;
#else
    return 0;
#endif
}

// Classify system memory pressure from OS signals.
// Uses the ratio of available/total RAM — no hardcoded thresholds on absolute
// memory amounts.  The OS decides what "available" means (free + reclaimable).
static SystemPressure classifyPressure() {
    size_t total = queryTotalRAM();
    size_t avail = queryAvailableRAM();
    if (total == 0 || avail == 0) return SystemPressure::Relaxed;

    double freeRatio = static_cast<double>(avail) / static_cast<double>(total);

    if (freeRatio < 0.05) return SystemPressure::Emergency;
    if (freeRatio < 0.10) return SystemPressure::High;
    if (freeRatio < 0.20) return SystemPressure::Moderate;
    return SystemPressure::Relaxed;
}

// After each major GC, decide whether to grow or shrink the heap.
// Goal: keep GC overhead (time in GC / total time) below target,
// modulated by system memory pressure.
//
// If overhead is high → grow heap (fewer collections needed).
// If overhead is low  → shrink heap (return memory to OS).
// System pressure overrides: under high pressure, never grow.

class HeapSizingPolicy {
public:
    struct Config {
        double targetOverhead;     // Target GC overhead ratio (default 5%)
        double growthFactor;       // Multiply capacity by this when growing
        double shrinkFactor;       // Multiply capacity by this when shrinking
        size_t minHeapSize;
        size_t maxHeapSize;
        size_t pageSize;           // Grow/shrink in page multiples

        Config()
            : targetOverhead(0.05)
            , growthFactor(1.5)
            , shrinkFactor(0.75)
            , minHeapSize(4 * 1024 * 1024)
            , maxHeapSize(512 * 1024 * 1024)
            , pageSize(256 * 1024) {}
    };

    explicit HeapSizingPolicy(const Config& config = Config{})
        : config_(config) {}

    struct GCResult {
        size_t liveBytes;
        size_t heapCapacity;
        double gcDurationMs;
        double mutatorDurationMs;
        bool isForegroundTab;      // Foreground tab is exempt from pressure
    };

    // Returns new recommended heap capacity.
    size_t recommend(const GCResult& result) {
        double overhead = 0;
        double total = result.gcDurationMs + result.mutatorDurationMs;
        if (total > 0) overhead = result.gcDurationMs / total;

        // Query system pressure from OS every call (~2s cadence via GC).
        SystemPressure pressure = classifyPressure();
        lastPressure_ = pressure;

        size_t newCapacity = result.heapCapacity;

        // Foreground tab: use overhead-based sizing only — never pressured.
        if (result.isForegroundTab) {
            newCapacity = overheadBasedSize(result, overhead);
        } else {
            // Background tab: modulate by system pressure.
            newCapacity = pressureAwareSize(result, overhead, pressure);
        }

        // Ensure we have enough room for live data + growth margin.
        size_t minRequired = static_cast<size_t>(result.liveBytes * 1.25);
        newCapacity = std::max(newCapacity, minRequired);

        // Clamp to bounds.
        newCapacity = std::max(newCapacity, config_.minHeapSize);
        newCapacity = std::min(newCapacity, config_.maxHeapSize);

        // Align to page boundary.
        newCapacity = alignUp(newCapacity, config_.pageSize);

        lastOverhead_ = overhead;
        lastRecommended_ = newCapacity;
        return newCapacity;
    }

    double lastOverhead() const { return lastOverhead_; }
    size_t lastRecommended() const { return lastRecommended_; }
    SystemPressure lastPressure() const { return lastPressure_; }

private:
    // Pure overhead-based sizing (used for foreground tabs).
    size_t overheadBasedSize(const GCResult& result, double overhead) {
        size_t newCapacity = result.heapCapacity;

        if (overhead > config_.targetOverhead * 1.5) {
            // High overhead → grow aggressively.
            newCapacity = static_cast<size_t>(
                result.heapCapacity * config_.growthFactor);
        } else if (overhead > config_.targetOverhead) {
            // Slightly above target → grow modestly.
            newCapacity = static_cast<size_t>(
                result.heapCapacity * 1.2);
        } else if (overhead < config_.targetOverhead * 0.25 &&
                   result.liveBytes < result.heapCapacity * 0.3) {
            // Very low overhead and low occupancy → shrink.
            newCapacity = static_cast<size_t>(
                result.heapCapacity * config_.shrinkFactor);
        }

        return newCapacity;
    }

    // Pressure-aware sizing (used for background tabs).
    // The higher the pressure, the more aggressively we shrink.
    size_t pressureAwareSize(const GCResult& result, double overhead,
                             SystemPressure pressure) {
        size_t newCapacity = result.heapCapacity;

        switch (pressure) {
            case SystemPressure::Relaxed:
                // No pressure — use normal overhead-based policy.
                newCapacity = overheadBasedSize(result, overhead);
                break;

            case SystemPressure::Moderate:
                // Cap at current size — no growth allowed.
                // Shrink only if low occupancy.
                if (result.liveBytes < result.heapCapacity * 0.4) {
                    newCapacity = static_cast<size_t>(
                        result.heapCapacity * 0.85);
                }
                break;

            case SystemPressure::High:
                // Actively shrink — target 1.5x live data.
                newCapacity = static_cast<size_t>(result.liveBytes * 1.5);
                newCapacity = std::min(newCapacity, result.heapCapacity);
                break;

            case SystemPressure::Emergency:
                // Emergency — shrink to absolute minimum (1.25x live data).
                newCapacity = static_cast<size_t>(result.liveBytes * 1.25);
                break;
        }

        return newCapacity;
    }

    static size_t alignUp(size_t v, size_t align) {
        return (v + align - 1) & ~(align - 1);
    }

    Config config_;
    double lastOverhead_ = 0;
    size_t lastRecommended_ = 0;
    SystemPressure lastPressure_ = SystemPressure::Relaxed;
};

} // namespace Zepra::Heap


