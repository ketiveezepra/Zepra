// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_rss_limiter.cpp — Resident set size control for low RAM

#include <atomic>
#include <algorithm>
#include <mutex>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cassert>

#ifdef __linux__
#if ZEPRA_PLATFORM_POSIX
#include <unistd.h>
#endif
#include <fstream>
#include <string>
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

// ZepraBrowser targets low RAM per tab. The RSS limiter monitors
// actual physical memory usage and triggers actions when limits
// are approached.

class RSSLimiter {
public:
    struct Config {
        size_t perTabLimitMB;     // Max RSS per tab (default 128MB)
        size_t warningThresholdMB; // Warn and start GC at this level
        size_t criticalThresholdMB; // Emergency GC + decommit

        Config() : perTabLimitMB(128), warningThresholdMB(96), criticalThresholdMB(112) {}
    };

    struct Actions {
        std::function<void()> triggerIncrementalGC;
        std::function<void()> triggerFullGC;
        std::function<void()> decommitPages;
        std::function<void()> shrinkHeap;
        std::function<void()> flushCaches;
    };

    explicit RSSLimiter(const Config& config = Config{})
        : config_(config) {}

    void setActions(Actions a) { actions_ = std::move(a); }

    // Called periodically by the browser process.
    void check() {
        size_t rssMB = getCurrentRSSMB();
        currentRSSMB_ = rssMB;

        if (rssMB >= config_.criticalThresholdMB) {
            // Critical: emergency measures.
            if (actions_.triggerFullGC) actions_.triggerFullGC();
            if (actions_.decommitPages) actions_.decommitPages();
            if (actions_.shrinkHeap) actions_.shrinkHeap();
            if (actions_.flushCaches) actions_.flushCaches();
            stats_.criticalHits++;
        } else if (rssMB >= config_.warningThresholdMB) {
            // Warning: incremental GC + trim.
            if (actions_.triggerIncrementalGC) actions_.triggerIncrementalGC();
            if (actions_.decommitPages) actions_.decommitPages();
            stats_.warningHits++;
        }

        stats_.checks++;
    }

    size_t currentRSSMB() const { return currentRSSMB_; }
    bool isOverLimit() const { return currentRSSMB_ >= config_.perTabLimitMB; }
    bool isWarning() const { return currentRSSMB_ >= config_.warningThresholdMB; }

    struct Stats { uint64_t checks; uint64_t warningHits; uint64_t criticalHits; };
    Stats stats() const { return stats_; }

private:
    static size_t getCurrentRSSMB() {
#ifdef __linux__
        std::ifstream statm("/proc/self/statm");
        if (statm.is_open()) {
            size_t virt, rss;
            statm >> virt >> rss;
            long pageSize = sysconf(_SC_PAGESIZE);
            return (rss * pageSize) / (1024 * 1024);
        }
#elif defined(_WIN32)
        PROCESS_MEMORY_COUNTERS pmc;
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
            return pmc.WorkingSetSize / (1024 * 1024);
        }
#endif
        return 0;
    }

    Config config_;
    Actions actions_;
    Stats stats_{};
    size_t currentRSSMB_ = 0;
};

} // namespace Zepra::Heap
