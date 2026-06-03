// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_page_decommit.cpp — Aggressive page decommit for RAM savings

#include <mutex>
#include <vector>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cassert>

#ifdef __linux__
#include <sys/mman.h>
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

// Decommit pages that are free (in free list but not in use).
// This returns physical pages to the OS while keeping the virtual
// address space reserved. Re-accessed pages fault in fresh zeros.

struct PageState {
    uintptr_t addr;
    size_t size;
    bool committed;
    bool hasLiveObjects;
    uint64_t lastUsedMs;
};

class PageDecommitter {
public:
    struct Config {
        uint64_t idleThresholdMs;  // Decommit after this idle time
        size_t minDecommitSize;    // Minimum decommit granularity
        double maxDecommitRatio;   // Don't decommit more than this fraction

        Config() : idleThresholdMs(10000), minDecommitSize(4096), maxDecommitRatio(0.50) {}
    };

    explicit PageDecommitter(const Config& config = Config{}) : config_(config) {}

    // Decommit idle, empty pages.
    size_t decommitIdle(const std::vector<PageState>& pages, uint64_t nowMs) {
        size_t decommitted = 0;
        size_t totalCommitted = 0;

        for (auto& p : pages) {
            if (p.committed) totalCommitted += p.size;
        }

        size_t maxDecommit = static_cast<size_t>(totalCommitted * config_.maxDecommitRatio);

        for (auto& page : pages) {
            if (decommitted >= maxDecommit) break;
            if (!page.committed || page.hasLiveObjects) continue;
            if ((nowMs - page.lastUsedMs) < config_.idleThresholdMs) continue;
            if (page.size < config_.minDecommitSize) continue;

            if (decommitPage(page.addr, page.size)) {
                decommitted += page.size;
                stats_.pagesDecommitted++;
            }
        }

        stats_.bytesDecommitted += decommitted;
        return decommitted;
    }

    // Aggressive decommit — ignore idle time, decommit all empty pages.
    size_t decommitAll(const std::vector<PageState>& pages) {
        size_t decommitted = 0;
        for (auto& page : pages) {
            if (!page.committed || page.hasLiveObjects) continue;
            if (decommitPage(page.addr, page.size)) {
                decommitted += page.size;
                stats_.pagesDecommitted++;
            }
        }
        stats_.bytesDecommitted += decommitted;
        return decommitted;
    }

    struct Stats { uint64_t pagesDecommitted; uint64_t bytesDecommitted; };
    Stats stats() const { return stats_; }

private:
    bool decommitPage(uintptr_t addr, size_t size) {
#ifdef __linux__
        return madvise(reinterpret_cast<void*>(addr), size, MADV_DONTNEED) == 0;
#elif defined(_WIN32)
        // MEM_DECOMMIT releases physical pages but keeps the virtual address
        // range reserved — equivalent to madvise(MADV_DONTNEED) on Linux.
        return VirtualFree(reinterpret_cast<void*>(addr), size, MEM_DECOMMIT) != 0;
#else
        (void)addr; (void)size;
        return true;
#endif
    }

    Config config_;
    Stats stats_{};
};

} // namespace Zepra::Heap
