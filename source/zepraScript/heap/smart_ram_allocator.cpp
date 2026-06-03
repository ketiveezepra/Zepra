// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — smart_ram_allocator.cpp — System RAM-aware dynamic tab budget allocator

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <functional>
#include <algorithm>
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
#endif

namespace Zepra::Heap {

enum class TabPriority : uint8_t {
    Active,        // Foreground tab — highest budget
    MediaPlaying,  // Background but playing audio/video — protected
    Background,    // Background tab — reduced budget
    Suspended,     // Inactive for extended period — minimal budget
};

static const char* tabPriorityName(TabPriority p) {
    switch (p) {
        case TabPriority::Active: return "Active";
        case TabPriority::MediaPlaying: return "MediaPlaying";
        case TabPriority::Background: return "Background";
        case TabPriority::Suspended: return "Suspended";
    }
    return "Unknown";
}

struct TabBudget {
    uint64_t tabId;
    TabPriority priority;
    size_t budgetBytes;
    size_t usedBytes;
    size_t peakBytes;

    size_t remaining() const { return budgetBytes > usedBytes ? budgetBytes - usedBytes : 0; }
    double usageRatio() const { return budgetBytes > 0 ? static_cast<double>(usedBytes) / budgetBytes : 0; }
    bool isOverBudget() const { return usedBytes > budgetBytes; }
};

class SmartRAMAllocator {
public:
    struct Config {
        double activePct;        // % of available RAM for active tabs
        double mediaPct;         // % for media-playing tabs
        double backgroundPct;   // % for background tabs
        double suspendedPct;    // % for suspended tabs
        size_t minTabBudgetMB;  // Minimum per-tab budget
        size_t maxTabBudgetMB;  // Maximum per-tab budget
        size_t reservedSystemMB; // RAM reserved for browser chrome + OS

        Config()
            : activePct(0.60)
            , mediaPct(0.25)
            , backgroundPct(0.10)
            , suspendedPct(0.05)
            , minTabBudgetMB(32)
            , maxTabBudgetMB(512)
            , reservedSystemMB(512) {}
    };

    struct PressureActions {
        std::function<void(uint64_t tabId)> triggerTabGC;
        std::function<void(uint64_t tabId)> suspendTab;
        std::function<void(uint64_t tabId)> evictTab;
        std::function<bool(uint64_t tabId)> isMediaPlaying;
    };

    explicit SmartRAMAllocator(const Config& config = Config{})
        : config_(config) {
        totalRAM_ = detectSystemRAM();
        availableRAM_ = totalRAM_ > (config_.reservedSystemMB * 1024 * 1024)
            ? totalRAM_ - config_.reservedSystemMB * 1024 * 1024
            : totalRAM_ / 2;
    }

    void setActions(PressureActions a) { actions_ = std::move(a); }

    void registerTab(uint64_t tabId, TabPriority priority) {
        std::lock_guard<std::mutex> lock(mutex_);
        TabBudget budget;
        budget.tabId = tabId;
        budget.priority = priority;
        budget.budgetBytes = 0;
        budget.usedBytes = 0;
        budget.peakBytes = 0;
        tabs_[tabId] = budget;
        rebalanceLocked();
    }

    void unregisterTab(uint64_t tabId) {
        std::lock_guard<std::mutex> lock(mutex_);
        tabs_.erase(tabId);
        rebalanceLocked();
    }

    void setTabPriority(uint64_t tabId, TabPriority priority) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tabs_.find(tabId);
        if (it == tabs_.end()) return;
        it->second.priority = priority;
        rebalanceLocked();
    }

    void reportTabUsage(uint64_t tabId, size_t usedBytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tabs_.find(tabId);
        if (it == tabs_.end()) return;
        it->second.usedBytes = usedBytes;
        if (usedBytes > it->second.peakBytes) {
            it->second.peakBytes = usedBytes;
        }
    }

    size_t getTabBudget(uint64_t tabId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tabs_.find(tabId);
        return it != tabs_.end() ? it->second.budgetBytes : 0;
    }

    const TabBudget* getTabInfo(uint64_t tabId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tabs_.find(tabId);
        return it != tabs_.end() ? &it->second : nullptr;
    }

    // Called on memory pressure change.
    void onPressure(uint8_t level) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (level >= 3) {
            // Critical: suspend background tabs, GC active tabs.
            for (auto& [id, tab] : tabs_) {
                if (tab.priority == TabPriority::Background) {
                    if (actions_.suspendTab) actions_.suspendTab(id);
                } else if (tab.priority == TabPriority::Active) {
                    if (actions_.triggerTabGC) actions_.triggerTabGC(id);
                }
            }
        } else if (level >= 2) {
            // Moderate: GC over-budget tabs.
            for (auto& [id, tab] : tabs_) {
                if (tab.isOverBudget() && actions_.triggerTabGC) {
                    actions_.triggerTabGC(id);
                }
            }
        }

        // Tighten budgets.
        rebalanceLocked();
    }

    // Evict lowest-priority tab (never evicts media-playing).
    bool evictLowestPriority() {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t candidate = 0;
        TabPriority lowestPrio = TabPriority::Active;
        bool found = false;

        for (auto& [id, tab] : tabs_) {
            if (tab.priority == TabPriority::MediaPlaying) continue;
            if (!found || tab.priority > lowestPrio ||
                (tab.priority == lowestPrio && tab.usedBytes > tabs_[candidate].usedBytes)) {
                candidate = id;
                lowestPrio = tab.priority;
                found = true;
            }
        }

        if (found && actions_.evictTab) {
            actions_.evictTab(candidate);
            tabs_.erase(candidate);
            rebalanceLocked();
            return true;
        }
        return false;
    }

    size_t totalRAM() const { return totalRAM_; }
    size_t availableRAM() const { return availableRAM_; }
    size_t tabCount() const { std::lock_guard<std::mutex> lock(mutex_); return tabs_.size(); }

    struct Stats {
        size_t totalAllocated;
        size_t tabCount;
        size_t activeCount;
        size_t mediaCount;
        size_t backgroundCount;
        size_t suspendedCount;
    };

    Stats computeStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Stats s{};
        s.tabCount = tabs_.size();
        for (auto& [id, tab] : tabs_) {
            s.totalAllocated += tab.usedBytes;
            switch (tab.priority) {
                case TabPriority::Active: s.activeCount++; break;
                case TabPriority::MediaPlaying: s.mediaCount++; break;
                case TabPriority::Background: s.backgroundCount++; break;
                case TabPriority::Suspended: s.suspendedCount++; break;
            }
        }
        return s;
    }

private:
    void rebalanceLocked() {
        size_t activeCount = 0, mediaCount = 0, bgCount = 0, suspCount = 0;

        for (auto& [id, tab] : tabs_) {
            switch (tab.priority) {
                case TabPriority::Active: activeCount++; break;
                case TabPriority::MediaPlaying: mediaCount++; break;
                case TabPriority::Background: bgCount++; break;
                case TabPriority::Suspended: suspCount++; break;
            }
        }

        auto perTabBudget = [&](TabPriority prio, size_t count) -> size_t {
            if (count == 0) return 0;
            double pct = 0;
            switch (prio) {
                case TabPriority::Active: pct = config_.activePct; break;
                case TabPriority::MediaPlaying: pct = config_.mediaPct; break;
                case TabPriority::Background: pct = config_.backgroundPct; break;
                case TabPriority::Suspended: pct = config_.suspendedPct; break;
            }
            size_t poolBytes = static_cast<size_t>(availableRAM_ * pct);
            size_t budget = poolBytes / count;
            budget = std::max(budget, config_.minTabBudgetMB * 1024 * 1024);
            budget = std::min(budget, config_.maxTabBudgetMB * 1024 * 1024);
            return budget;
        };

        for (auto& [id, tab] : tabs_) {
            size_t count = 0;
            switch (tab.priority) {
                case TabPriority::Active: count = activeCount; break;
                case TabPriority::MediaPlaying: count = mediaCount; break;
                case TabPriority::Background: count = bgCount; break;
                case TabPriority::Suspended: count = suspCount; break;
            }
            tab.budgetBytes = perTabBudget(tab.priority, count);
        }
    }

    static size_t detectSystemRAM() {
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
        long pages = sysconf(_SC_PHYS_PAGES);
        long pageSize = sysconf(_SC_PAGESIZE);
        if (pages > 0 && pageSize > 0) {
            return static_cast<size_t>(pages) * static_cast<size_t>(pageSize);
        }
#elif defined(_WIN32)
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            return static_cast<size_t>(ms.ullTotalPhys);
        }
#endif
        return 4ULL * 1024 * 1024 * 1024;  // 4GB fallback
    }

    Config config_;
    PressureActions actions_;
    size_t totalRAM_;
    size_t availableRAM_;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, TabBudget> tabs_;
};

} // namespace Zepra::Heap
