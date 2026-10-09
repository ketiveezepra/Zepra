// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — jit_code_cache.cpp — Executable code cache: W^X, invalidation, aging

#include <cstdint>
#include <cassert>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <algorithm>

#ifdef __linux__
#include <sys/mman.h>
#if ZEPRA_PLATFORM_POSIX
#include <unistd.h>
#endif
#endif

namespace Zepra::JIT {

struct CodeEntry {
    uint32_t functionId;
    uint8_t* code;
    size_t codeSize;
    uint32_t tier;           // 0=baseline, 1=optimized
    uint64_t executionCount;
    uint64_t lastUsedCycle;
    bool valid;
    bool markedForEviction;

    CodeEntry() : functionId(0), code(nullptr), codeSize(0), tier(0)
        , executionCount(0), lastUsedCycle(0), valid(false), markedForEviction(false) {}
};

class CodeCache {
public:
    static constexpr size_t kDefaultCacheSize = 16 * 1024 * 1024;  // 16MB
    static constexpr size_t kPageSize = 4096;
    static constexpr size_t kAlignment = 16;

    CodeCache(size_t maxSize = kDefaultCacheSize)
        : maxSize_(maxSize), usedSize_(0), cycle_(0), base_(nullptr) {
        allocateRegion();
    }

    ~CodeCache() {
        freeRegion();
    }

    // Install compiled code. Returns executable pointer.
    uint8_t* install(uint32_t functionId, const uint8_t* code, size_t size, uint32_t tier) {
        std::lock_guard<std::mutex> lock(mutex_);

        size_t aligned = alignUp(size, kAlignment);

        // Evict if needed.
        while (usedSize_ + aligned > maxSize_) {
            if (!evictOne()) break;
        }

        if (usedSize_ + aligned > maxSize_) return nullptr;

        // Copy to executable memory.
        uint8_t* dest = base_ + usedSize_;
        makeWritable(dest, aligned);
        memcpy(dest, code, size);

        // NOP-pad alignment.
        if (aligned > size) {
            memset(dest + size, 0x90, aligned - size);
        }

        makeExecutable(dest, aligned);

        // Register entry.
        CodeEntry entry;
        entry.functionId = functionId;
        entry.code = dest;
        entry.codeSize = aligned;
        entry.tier = tier;
        entry.valid = true;
        entry.lastUsedCycle = cycle_;
        entries_[functionId] = entry;

        usedSize_ += aligned;
        stats_.installCount++;
        stats_.totalBytesInstalled += aligned;
        return dest;
    }

    // Lookup compiled code for a function.
    uint8_t* lookup(uint32_t functionId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(functionId);
        if (it == entries_.end() || !it->second.valid) return nullptr;
        it->second.executionCount++;
        it->second.lastUsedCycle = cycle_;
        return it->second.code;
    }

    // Invalidate all code for a function (deoptimization).
    void invalidate(uint32_t functionId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(functionId);
        if (it != entries_.end()) {
            it->second.valid = false;
            stats_.invalidations++;
        }
    }

    // Invalidate all code (GC moved objects).
    void invalidateAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, entry] : entries_) {
            entry.valid = false;
        }
        stats_.fullInvalidations++;
    }

    // Advance aging cycle (called per GC).
    void advanceCycle() { cycle_++; }

    // Compact: remove invalid entries and reclaim space.
    size_t compact() {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t freed = 0;
        std::vector<uint32_t> toRemove;
        for (auto& [id, entry] : entries_) {
            if (!entry.valid) {
                freed += entry.codeSize;
                toRemove.push_back(id);
            }
        }
        for (auto id : toRemove) entries_.erase(id);
        usedSize_ = (freed > usedSize_) ? 0 : usedSize_ - freed;
        stats_.compactions++;
        return freed;
    }

    size_t usedSize() const { return usedSize_; }
    size_t maxSize() const { return maxSize_; }
    size_t entryCount() const { return entries_.size(); }

    struct Stats {
        uint64_t installCount = 0;
        uint64_t totalBytesInstalled = 0;
        uint64_t invalidations = 0;
        uint64_t fullInvalidations = 0;
        uint64_t evictions = 0;
        uint64_t compactions = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    static size_t alignUp(size_t v, size_t align) {
        return (v + align - 1) & ~(align - 1);
    }

    void allocateRegion() {
#ifdef __linux__
        base_ = static_cast<uint8_t*>(mmap(nullptr, maxSize_,
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (base_ == MAP_FAILED) base_ = nullptr;
#else
        base_ = static_cast<uint8_t*>(malloc(maxSize_));
#endif
    }

    void freeRegion() {
#ifdef __linux__
        if (base_) munmap(base_, maxSize_);
#else
        free(base_);
#endif
        base_ = nullptr;
    }

    void makeWritable(uint8_t* ptr, size_t size) {
#ifdef __linux__
        mprotect(alignDown(ptr), alignUp(size + (reinterpret_cast<uintptr_t>(ptr) % kPageSize), kPageSize),
                 PROT_READ | PROT_WRITE);
#endif
    }

    void makeExecutable(uint8_t* ptr, size_t size) {
#ifdef __linux__
        mprotect(alignDown(ptr), alignUp(size + (reinterpret_cast<uintptr_t>(ptr) % kPageSize), kPageSize),
                 PROT_READ | PROT_EXEC);
#endif
    }

    uint8_t* alignDown(uint8_t* ptr) {
        return reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(ptr) & ~(kPageSize - 1));
    }

    bool evictOne() {
        // LRU eviction: find oldest entry.
        uint32_t victimId = 0;
        uint64_t oldestCycle = UINT64_MAX;

        for (auto& [id, entry] : entries_) {
            if (entry.valid && entry.lastUsedCycle < oldestCycle) {
                oldestCycle = entry.lastUsedCycle;
                victimId = id;
            }
        }

        if (oldestCycle == UINT64_MAX) return false;

        size_t victimSize = entries_[victimId].codeSize;
        usedSize_ = (victimSize > usedSize_) ? 0 : usedSize_ - victimSize;
        entries_.erase(victimId);
        stats_.evictions++;
        return true;
    }

    mutable std::mutex mutex_;
    uint8_t* base_;
    size_t maxSize_;
    size_t usedSize_;
    uint64_t cycle_;
    std::unordered_map<uint32_t, CodeEntry> entries_;
    Stats stats_;
};

} // namespace Zepra::JIT
