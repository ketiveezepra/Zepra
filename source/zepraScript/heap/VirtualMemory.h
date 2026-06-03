// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file VirtualMemory.h
 * @brief Virtual memory management for ZepraScript GC
 *
 * OS-level memory primitives:
 * - Reserve: claim virtual address space (no physical pages)
 * - Commit: map physical pages to reserved addresses
 * - Decommit: release physical pages, keep virtual reservation
 * - Release: free entire virtual region
 *
 * All heap allocators build on this layer.
 * Linux: mmap/mprotect/madvise
 * 
 * Features:
 * - Huge page support (THP + explicit hugetlbfs)
 * - NUMA-aware allocation
 * - Guard page management
 * - Memory accounting
 * - Cgroup-aware limits
 */

#pragma once
#include "zepra_alloc.h"

#include <cstdint>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <atomic>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <functional>
#include <string>

#ifdef __linux__
#include <sys/mman.h>
#include <sys/resource.h>
#if ZEPRA_PLATFORM_POSIX
#include <unistd.h>
#endif
#include <fcntl.h>
#include <numa.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <memoryapi.h>
#endif

#ifdef _WIN32
namespace Zepra::Heap::detail {

static bool tryEnableLargePages() {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValue(NULL, SE_LOCK_MEMORY_NAME,
                              &tp.Privileges[0].Luid)) {
        CloseHandle(token);
        return false;
    }
    BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
    DWORD err = GetLastError();
    CloseHandle(token);
    return ok && err == ERROR_SUCCESS;
}

} // namespace Zepra::Heap::detail
#endif

namespace Zepra::Heap {

// =============================================================================
// Memory Protection
// =============================================================================

enum class Protection : uint32_t {
    None       = 0,
    Read       = 1,
    Write      = 2,
    Execute    = 4,
    ReadWrite  = Read | Write,
    ReadExec   = Read | Execute,
    All        = Read | Write | Execute,
};

inline Protection operator|(Protection a, Protection b) {
    return static_cast<Protection>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool operator&(Protection a, Protection b) {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

// =============================================================================
// Memory Mapping Flags
// =============================================================================

struct MappingFlags {
    bool shared = false;          // Shared with child processes
    bool fixed = false;           // Fixed address (MAP_FIXED)
    bool populate = false;        // Prefault pages (MAP_POPULATE)
    bool hugePages = false;       // Use huge pages
    bool noReserve = false;       // Don't reserve swap (overcommit)
    int numaNode = -1;           // NUMA node (-1 = any)
    size_t alignment = 0;        // Alignment requirement (0 = page)
};

// =============================================================================
// Virtual Region
// =============================================================================

/**
 * @brief Represents a contiguous virtual address range
 */
struct VirtualRegion {
    void* base = nullptr;
    size_t size = 0;
    size_t committedSize = 0;
    Protection protection = Protection::None;
    bool isHugePage = false;
    int numaNode = -1;

    bool isValid() const { return base != nullptr && size > 0; }
    void* end() const {
        return static_cast<char*>(base) + size;
    }
    bool contains(const void* ptr) const {
        return ptr >= base && ptr < end();
    }
};

// =============================================================================
// Memory Statistics
// =============================================================================

struct VirtualMemoryStats {
    // Reservations
    size_t totalReserved = 0;
    size_t totalCommitted = 0;
    size_t totalDecommitted = 0;
    size_t totalReleased = 0;
    size_t currentReserved = 0;
    size_t currentCommitted = 0;

    // Operations
    uint64_t reserveCount = 0;
    uint64_t commitCount = 0;
    uint64_t decommitCount = 0;
    uint64_t releaseCount = 0;
    uint64_t protectCount = 0;

    // Failures
    uint64_t reserveFailures = 0;
    uint64_t commitFailures = 0;
    uint64_t oomEvents = 0;

    // Huge pages
    size_t hugePageBytes = 0;
    uint64_t hugePageCount = 0;

    // Peak
    size_t peakReserved = 0;
    size_t peakCommitted = 0;

    void updatePeak() {
        if (currentReserved > peakReserved) peakReserved = currentReserved;
        if (currentCommitted > peakCommitted) peakCommitted = currentCommitted;
    }
};

// =============================================================================
// Page Size Information
// =============================================================================

struct PageInfo {
    size_t regularPageSize;     // 4KB on x86-64
    size_t hugePageSize;         // 2MB on x86-64
    size_t giganticPageSize;     // 1GB on x86-64 (if available)
    bool hugePagesAvailable;
    size_t hugePagesTotal;
    size_t hugePagesFree;

    static PageInfo query();
};

// =============================================================================
// Virtual Memory Manager
// =============================================================================

class VirtualMemory {
public:
    VirtualMemory();
    ~VirtualMemory();

    VirtualMemory(const VirtualMemory&) = delete;
    VirtualMemory& operator=(const VirtualMemory&) = delete;

    // -------------------------------------------------------------------------
    // Core Operations
    // -------------------------------------------------------------------------

    /**
     * @brief Reserve virtual address space without committing physical memory
     * @param size Size in bytes (rounded up to page size)
     * @param flags Optional mapping flags
     * @return Virtual region descriptor, or invalid region on failure
     */
    VirtualRegion reserve(size_t size, const MappingFlags& flags = {});

    /**
     * @brief Reserve at a specific address (for JIT code regions)
     */
    VirtualRegion reserveAt(void* address, size_t size,
                             const MappingFlags& flags = {});

    /**
     * @brief Commit physical memory for (part of) a reserved region
     * @param region The reserved region
     * @param offset Offset within the region
     * @param size Size to commit
     * @param prot Memory protection
     * @return true on success
     */
    bool commit(VirtualRegion& region, size_t offset, size_t size,
                Protection prot = Protection::ReadWrite);

    /**
     * @brief Commit entire region
     */
    bool commitAll(VirtualRegion& region,
                   Protection prot = Protection::ReadWrite);

    /**
     * @brief Decommit physical memory while keeping the reservation
     * The pages are returned to the OS but the virtual addresses remain valid.
     */
    bool decommit(VirtualRegion& region, size_t offset, size_t size);

    /**
     * @brief Release entire virtual region
     * After this, the region is invalid and addresses are unmapped.
     */
    void release(VirtualRegion& region);

    // -------------------------------------------------------------------------
    // Protection
    // -------------------------------------------------------------------------

    /**
     * @brief Change memory protection
     */
    bool protect(void* address, size_t size, Protection prot);

    /**
     * @brief Create a guard page (PROT_NONE)
     */
    bool setGuardPage(void* address, size_t size = 0);

    // -------------------------------------------------------------------------
    // Advise
    // -------------------------------------------------------------------------

    /**
     * @brief Hint to OS about memory usage pattern
     */
    enum class Advice {
        Normal,
        Sequential,     // Sequential access pattern
        Random,         // Random access pattern
        WillNeed,       // Pages will be needed soon
        DontNeed,       // Pages won't be needed soon (free physical pages)
        Free,           // Pages are free (stronger than DontNeed)
        HugePage,       // Request huge page backing
        NoHugePage,     // Disable huge pages for region
    };

    bool advise(void* address, size_t size, Advice advice);

    // -------------------------------------------------------------------------
    // Utilities
    // -------------------------------------------------------------------------

    /**
     * @brief Allocate aligned memory (reserve + commit)
     * Convenience method for simple allocations.
     */
    void* allocateAligned(size_t size, size_t alignment,
                          Protection prot = Protection::ReadWrite);

    /**
     * @brief Free aligned memory
     */
    void freeAligned(void* ptr, size_t size);

    /**
     * @brief Zero memory range
     */
    static void zeroMemory(void* address, size_t size);

    /**
     * @brief Prefault pages (touch each page to pre-map)
     */
    static void prefaultPages(void* address, size_t size);

    /**
     * @brief Lock pages in physical memory (prevent swapping)
     */
    bool lockPages(void* address, size_t size);

    /**
     * @brief Unlock pages
     */
    bool unlockPages(void* address, size_t size);

    // -------------------------------------------------------------------------
    // Queries
    // -------------------------------------------------------------------------

    /**
     * @brief System page size
     */
    size_t pageSize() const { return pageInfo_.regularPageSize; }

    /**
     * @brief Huge page size
     */
    size_t hugePageSize() const { return pageInfo_.hugePageSize; }

    /**
     * @brief Page info
     */
    const PageInfo& pageInfo() const { return pageInfo_; }

    /**
     * @brief Current statistics
     */
    const VirtualMemoryStats& stats() const { return stats_; }

    /**
     * @brief Get process virtual memory usage
     */
    static size_t processVirtualSize();

    /**
     * @brief Get process resident set size
     */
    static size_t processResidentSize();

    /**
     * @brief Get available physical memory
     */
    static size_t availablePhysicalMemory();

    /**
     * @brief Round up to page boundary
     */
    size_t roundUpToPage(size_t size) const {
        return (size + pageInfo_.regularPageSize - 1) &
               ~(pageInfo_.regularPageSize - 1);
    }

    /**
     * @brief Round up to huge page boundary
     */
    size_t roundUpToHugePage(size_t size) const {
        return (size + pageInfo_.hugePageSize - 1) &
               ~(pageInfo_.hugePageSize - 1);
    }

private:
    int protectionToNative(Protection prot) const;
    void trackRegion(const VirtualRegion& region);
    void untrackRegion(const VirtualRegion& region);

    PageInfo pageInfo_;
    VirtualMemoryStats stats_;

    // Region tracking
    std::unordered_map<uintptr_t, VirtualRegion> regions_;
    mutable std::mutex mutex_;
};

// =============================================================================
// Page Tracker
// =============================================================================

/**
 * @brief Tracks committed/uncommitted state per page
 *
 * Used by region allocators to know which pages have been
 * committed and can be lazily committed on first access.
 */
class PageTracker {
public:
    PageTracker() = default;

    void initialize(void* base, size_t totalSize, size_t pageSize) {
        base_ = static_cast<char*>(base);
        pageSize_ = pageSize;
        size_t pageCount = (totalSize + pageSize - 1) / pageSize;
        committed_.resize(pageCount, false);
        dirty_.resize(pageCount, false);
    }

    bool isCommitted(size_t pageIndex) const {
        return pageIndex < committed_.size() && committed_[pageIndex];
    }

    void markCommitted(size_t pageIndex) {
        if (pageIndex < committed_.size()) committed_[pageIndex] = true;
    }

    void markDecommitted(size_t pageIndex) {
        if (pageIndex < committed_.size()) {
            committed_[pageIndex] = false;
            dirty_[pageIndex] = false;
        }
    }

    bool isDirty(size_t pageIndex) const {
        return pageIndex < dirty_.size() && dirty_[pageIndex];
    }

    void markDirty(size_t pageIndex) {
        if (pageIndex < dirty_.size()) dirty_[pageIndex] = true;
    }

    void markClean(size_t pageIndex) {
        if (pageIndex < dirty_.size()) dirty_[pageIndex] = false;
    }

    size_t pageIndexFor(const void* addr) const {
        return static_cast<size_t>(
            static_cast<const char*>(addr) - base_) / pageSize_;
    }

    void* pageAddress(size_t pageIndex) const {
        return base_ + pageIndex * pageSize_;
    }

    size_t pageCount() const { return committed_.size(); }

    size_t committedPageCount() const {
        size_t count = 0;
        for (bool c : committed_) if (c) count++;
        return count;
    }

    size_t committedBytes() const {
        return committedPageCount() * pageSize_;
    }

    size_t dirtyPageCount() const {
        size_t count = 0;
        for (bool d : dirty_) if (d) count++;
        return count;
    }

    /**
     * @brief Find a range of uncommitted pages
     * @return Start page index, or SIZE_MAX if none found
     */
    size_t findUncommittedRange(size_t pageCount) const {
        size_t consecutive = 0;
        for (size_t i = 0; i < committed_.size(); i++) {
            if (!committed_[i]) {
                consecutive++;
                if (consecutive >= pageCount) {
                    return i - pageCount + 1;
                }
            } else {
                consecutive = 0;
            }
        }
        return SIZE_MAX;
    }

private:
    char* base_ = nullptr;
    size_t pageSize_ = 4096;
    std::vector<bool> committed_;
    std::vector<bool> dirty_;
};

// =============================================================================
// Address Space Layout
// =============================================================================

/**
 * @brief Manages the overall heap address space layout
 *
 * Layout:
 * [Nursery (from-space)] [Nursery (to-space)] [Old Gen Regions...] [LOS...] [Code...]
 *
 * Each section gets a contiguous chunk of the virtual address space.
 * This enables fast contains() checks (simple range comparison).
 */
class AddressSpaceLayout {
public:
    struct SpaceConfig {
        size_t nurserySize = 4 * 1024 * 1024;              // 4MB
        size_t oldGenMaxSize = 512 * 1024 * 1024;           // 512MB
        size_t losMaxSize = 256 * 1024 * 1024;              // 256MB
        size_t codeSpaceMaxSize = 128 * 1024 * 1024;        // 128MB
        size_t guardPageSize = 4096;
    };

    explicit AddressSpaceLayout(VirtualMemory& vmem,
                                 const SpaceConfig& config = SpaceConfig{});
    ~AddressSpaceLayout();

    /**
     * @brief Initialize the address space layout
     * Reserves virtual memory for all spaces.
     */
    bool initialize();

    // Space boundaries
    struct SpaceInfo {
        void* base = nullptr;
        size_t size = 0;
        const char* name = nullptr;

        bool contains(const void* ptr) const {
            auto* p = static_cast<const char*>(ptr);
            return p >= static_cast<const char*>(base) &&
                   p < static_cast<const char*>(base) + size;
        }
    };

    const SpaceInfo& nursery() const { return nursery_; }
    const SpaceInfo& oldGen() const { return oldGen_; }
    const SpaceInfo& los() const { return los_; }
    const SpaceInfo& codeSpace() const { return codeSpace_; }

    /**
     * @brief Quick space identification for a pointer
     */
    enum class Space { Unknown, Nursery, OldGen, LOS, Code };
    Space identifyPointer(const void* ptr) const;

    /**
     * @brief Is any managed heap pointer?
     */
    bool isHeapPointer(const void* ptr) const;

private:
    VirtualMemory& vmem_;
    SpaceConfig config_;

    VirtualRegion baseRegion_;
    SpaceInfo nursery_;
    SpaceInfo oldGen_;
    SpaceInfo los_;
    SpaceInfo codeSpace_;
};

// =============================================================================
// Implementation
// =============================================================================

// PageInfo

inline PageInfo PageInfo::query() {
    PageInfo info;
#ifdef __linux__
    info.regularPageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));

    // Check for huge page support
    info.hugePageSize = 2 * 1024 * 1024;  // 2MB default
    info.giganticPageSize = 1024 * 1024 * 1024;  // 1GB

    FILE* f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            size_t val = 0;
            if (sscanf(line, "HugePages_Total: %zu", &val) == 1)
                info.hugePagesTotal = val;
            else if (sscanf(line, "HugePages_Free: %zu", &val) == 1)
                info.hugePagesFree = val;
            else if (sscanf(line, "Hugepagesize: %zu kB", &val) == 1)
                info.hugePageSize = val * 1024;
        }
        fclose(f);
    }
    info.hugePagesAvailable = info.hugePagesTotal > 0;
#elif defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    info.regularPageSize = static_cast<size_t>(si.dwPageSize);
    info.hugePageSize = GetLargePageMinimum();
    if (info.hugePageSize == 0)
        info.hugePageSize = 2 * 1024 * 1024;  // 2MB fallback
    info.giganticPageSize = 1024 * 1024 * 1024;  // 1GB
    info.hugePagesAvailable = detail::tryEnableLargePages() &&
                              GetLargePageMinimum() > 0;
    info.hugePagesTotal = 0;  // Not queryable on Windows
    info.hugePagesFree = 0;
#else
    info.regularPageSize = 4096;
    info.hugePageSize = 2 * 1024 * 1024;
    info.giganticPageSize = 1024 * 1024 * 1024;
    info.hugePagesAvailable = false;
    info.hugePagesTotal = 0;
    info.hugePagesFree = 0;
#endif
    return info;
}

// VirtualMemory

inline VirtualMemory::VirtualMemory() {
    pageInfo_ = PageInfo::query();
}

inline VirtualMemory::~VirtualMemory() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [addr, region] : regions_) {
        if (region.base) {
#ifdef __linux__
            munmap(region.base, region.size);
#elif defined(_WIN32)
            VirtualFree(region.base, 0, MEM_RELEASE);
#else
            std::free(region.base);
#endif
        }
    }
    regions_.clear();
}

inline VirtualRegion VirtualMemory::reserve(size_t size,
                                             const MappingFlags& flags) {
    VirtualRegion region;
    size = roundUpToPage(size);

    if (flags.alignment > pageInfo_.regularPageSize) {
        // Over-allocate for alignment
        size_t overSize = size + flags.alignment;
#ifdef __linux__
        int mapFlags = MAP_PRIVATE | MAP_ANONYMOUS;
        if (flags.noReserve) mapFlags |= MAP_NORESERVE;

        void* raw = mmap(nullptr, overSize, PROT_NONE, mapFlags, -1, 0);
        if (raw == MAP_FAILED) {
            stats_.reserveFailures++;
            return region;
        }

        // Align within the over-allocation
        uintptr_t rawAddr = reinterpret_cast<uintptr_t>(raw);
        uintptr_t aligned = (rawAddr + flags.alignment - 1) & ~(flags.alignment - 1);
        size_t leadingWaste = aligned - rawAddr;
        size_t trailingWaste = overSize - leadingWaste - size;

        if (leadingWaste > 0) munmap(raw, leadingWaste);
        if (trailingWaste > 0) {
            munmap(reinterpret_cast<void*>(aligned + size), trailingWaste);
        }

        region.base = reinterpret_cast<void*>(aligned);
#elif defined(_WIN32)
        void* raw = VirtualAlloc(NULL, overSize, MEM_RESERVE, PAGE_NOACCESS);
        if (!raw) {
            stats_.reserveFailures++;
            return region;
        }

        uintptr_t rawAddr = reinterpret_cast<uintptr_t>(raw);
        uintptr_t aligned = (rawAddr + flags.alignment - 1) & ~(flags.alignment - 1);

        // Win32 cannot partially release — free and re-reserve at aligned address
        VirtualFree(raw, 0, MEM_RELEASE);
        region.base = VirtualAlloc(reinterpret_cast<void*>(aligned), size,
                                   MEM_RESERVE, PAGE_NOACCESS);
        if (!region.base) {
            // Fallback: accept any address
            region.base = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
            if (!region.base) {
                stats_.reserveFailures++;
                return region;
            }
        }
#else
        region.base = zepra_aligned_alloc(flags.alignment, size);
#endif
    } else {
#ifdef __linux__
        int mapFlags = MAP_PRIVATE | MAP_ANONYMOUS;
        if (flags.noReserve) mapFlags |= MAP_NORESERVE;
        if (flags.hugePages) {
#ifdef MAP_HUGETLB
            mapFlags |= MAP_HUGETLB;
#endif
        }

        void* ptr = mmap(nullptr, size, PROT_NONE, mapFlags, -1, 0);
        if (ptr == MAP_FAILED) {
            stats_.reserveFailures++;
            return region;
        }
        region.base = ptr;
#elif defined(_WIN32)
        DWORD allocType = MEM_RESERVE;
        if (flags.hugePages && pageInfo_.hugePagesAvailable) {
            allocType |= MEM_LARGE_PAGES;
            size = roundUpToHugePage(size);
        }

        void* ptr = nullptr;
        if (flags.numaNode >= 0) {
            ptr = VirtualAllocExNuma(GetCurrentProcess(), NULL, size,
                                    allocType, PAGE_NOACCESS,
                                    static_cast<DWORD>(flags.numaNode));
        } else {
            ptr = VirtualAlloc(NULL, size, allocType, PAGE_NOACCESS);
        }
        if (!ptr) {
            // If large pages failed, retry without
            if (flags.hugePages && (allocType & MEM_LARGE_PAGES)) {
                allocType &= ~static_cast<DWORD>(MEM_LARGE_PAGES);
                size = roundUpToPage(size);  // revert to normal page rounding
                ptr = VirtualAlloc(NULL, size, allocType, PAGE_NOACCESS);
            }
            if (!ptr) {
                stats_.reserveFailures++;
                return region;
            }
        }
        region.base = ptr;
#else
        region.base = zepra_aligned_alloc(pageInfo_.regularPageSize, size);
#endif
    }

    region.size = size;
    region.committedSize = 0;
    region.protection = Protection::None;
    region.isHugePage = flags.hugePages;
    region.numaNode = flags.numaNode;

    std::lock_guard<std::mutex> lock(mutex_);
    stats_.reserveCount++;
    stats_.totalReserved += size;
    stats_.currentReserved += size;
    stats_.updatePeak();
    trackRegion(region);

    return region;
}

inline VirtualRegion VirtualMemory::reserveAt(void* address, size_t size,
                                               const MappingFlags& flags) {
    VirtualRegion region;
    size = roundUpToPage(size);

#ifdef __linux__
    int mapFlags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;
    if (flags.noReserve) mapFlags |= MAP_NORESERVE;

    void* ptr = mmap(address, size, PROT_NONE, mapFlags, -1, 0);
    if (ptr == MAP_FAILED || ptr != address) {
        stats_.reserveFailures++;
        return region;
    }
    region.base = ptr;
#elif defined(_WIN32)
    void* ptr = VirtualAlloc(address, size, MEM_RESERVE, PAGE_NOACCESS);
    if (!ptr || ptr != address) {
        stats_.reserveFailures++;
        if (ptr && ptr != address) VirtualFree(ptr, 0, MEM_RELEASE);
        return region;
    }
    region.base = ptr;
#else
    (void)address;
    (void)flags;
    region.base = zepra_aligned_alloc(pageInfo_.regularPageSize, size);
#endif

    region.size = size;
    region.committedSize = 0;
    region.protection = Protection::None;

    std::lock_guard<std::mutex> lock(mutex_);
    stats_.reserveCount++;
    stats_.totalReserved += size;
    stats_.currentReserved += size;
    stats_.updatePeak();
    trackRegion(region);

    return region;
}

inline bool VirtualMemory::commit(VirtualRegion& region, size_t offset,
                                   size_t size, Protection prot) {
    if (!region.isValid()) return false;
    size = roundUpToPage(size);
    offset = offset & ~(pageInfo_.regularPageSize - 1);

    void* addr = static_cast<char*>(region.base) + offset;

#ifdef __linux__
    int nativeProt = protectionToNative(prot);
    if (mprotect(addr, size, nativeProt) != 0) {
        stats_.commitFailures++;
        return false;
    }

    // Request huge pages if available and aligned
    if (region.isHugePage && size >= pageInfo_.hugePageSize) {
#ifdef MADV_HUGEPAGE
        madvise(addr, size, MADV_HUGEPAGE);
#endif
    }
#elif defined(_WIN32)
    DWORD winProt = static_cast<DWORD>(protectionToNative(prot));
    void* result = VirtualAlloc(addr, size, MEM_COMMIT, winProt);
    if (!result) {
        stats_.commitFailures++;
        return false;
    }
#else
    (void)prot;
#endif

    region.committedSize += size;
    region.protection = prot;

    std::lock_guard<std::mutex> lock(mutex_);
    stats_.commitCount++;
    stats_.totalCommitted += size;
    stats_.currentCommitted += size;
    stats_.updatePeak();

    return true;
}

inline bool VirtualMemory::commitAll(VirtualRegion& region, Protection prot) {
    return commit(region, 0, region.size, prot);
}

inline bool VirtualMemory::decommit(VirtualRegion& region, size_t offset,
                                     size_t size) {
    if (!region.isValid()) return false;
    size = roundUpToPage(size);
    void* addr = static_cast<char*>(region.base) + offset;

#ifdef __linux__
    madvise(addr, size, MADV_DONTNEED);
    mprotect(addr, size, PROT_NONE);
#elif defined(_WIN32)
    VirtualFree(addr, size, MEM_DECOMMIT);
#endif

    region.committedSize -= std::min(region.committedSize, size);

    std::lock_guard<std::mutex> lock(mutex_);
    stats_.decommitCount++;
    stats_.totalDecommitted += size;
    stats_.currentCommitted -= std::min(stats_.currentCommitted, size);

    return true;
}

inline void VirtualMemory::release(VirtualRegion& region) {
    if (!region.isValid()) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.releaseCount++;
        stats_.totalReleased += region.size;
        stats_.currentReserved -= std::min(stats_.currentReserved, region.size);
        stats_.currentCommitted -= std::min(stats_.currentCommitted, region.committedSize);
        untrackRegion(region);
    }

#ifdef __linux__
    munmap(region.base, region.size);
#elif defined(_WIN32)
    VirtualFree(region.base, 0, MEM_RELEASE);
#else
    std::free(region.base);
#endif

    region.base = nullptr;
    region.size = 0;
    region.committedSize = 0;
}

inline bool VirtualMemory::protect(void* address, size_t size, Protection prot) {
#ifdef __linux__
    int nativeProt = protectionToNative(prot);
    bool ok = mprotect(address, roundUpToPage(size), nativeProt) == 0;
    if (ok) stats_.protectCount++;
    return ok;
#elif defined(_WIN32)
    DWORD winProt = static_cast<DWORD>(protectionToNative(prot));
    DWORD oldProt;
    bool ok = VirtualProtect(address, roundUpToPage(size), winProt, &oldProt) != 0;
    if (ok) stats_.protectCount++;
    return ok;
#else
    (void)address; (void)size; (void)prot;
    return true;
#endif
}

inline bool VirtualMemory::setGuardPage(void* address, size_t size) {
    if (size == 0) size = pageInfo_.regularPageSize;
    return protect(address, size, Protection::None);
}

inline bool VirtualMemory::advise(void* address, size_t size, Advice advice) {
#ifdef __linux__
    int nativeAdvice;
    switch (advice) {
        case Advice::Normal:     nativeAdvice = MADV_NORMAL; break;
        case Advice::Sequential: nativeAdvice = MADV_SEQUENTIAL; break;
        case Advice::Random:     nativeAdvice = MADV_RANDOM; break;
        case Advice::WillNeed:   nativeAdvice = MADV_WILLNEED; break;
        case Advice::DontNeed:   nativeAdvice = MADV_DONTNEED; break;
#ifdef MADV_FREE
        case Advice::Free:       nativeAdvice = MADV_FREE; break;
#else
        case Advice::Free:       nativeAdvice = MADV_DONTNEED; break;
#endif
#ifdef MADV_HUGEPAGE
        case Advice::HugePage:   nativeAdvice = MADV_HUGEPAGE; break;
        case Advice::NoHugePage: nativeAdvice = MADV_NOHUGEPAGE; break;
#else
        case Advice::HugePage:
        case Advice::NoHugePage: return true;
#endif
        default: return false;
    }
    return madvise(address, size, nativeAdvice) == 0;
#elif defined(_WIN32)
    switch (advice) {
        case Advice::DontNeed:
        case Advice::Free:
            // Decommit and recommit to discard physical pages
            if (VirtualFree(address, size, MEM_DECOMMIT)) {
                VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE);
            }
            return true;
        case Advice::WillNeed: {
            // PrefetchVirtualMemory is available on Win8.1+
            typedef BOOL (WINAPI *PrefetchFn)(HANDLE, ULONG_PTR,
                                              PWIN32_MEMORY_RANGE_ENTRY, ULONG);
            HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
            if (kernel) {
                auto pfn = reinterpret_cast<PrefetchFn>(
                    GetProcAddress(kernel, "PrefetchVirtualMemory"));
                if (pfn) {
                    WIN32_MEMORY_RANGE_ENTRY entry;
                    entry.VirtualAddress = address;
                    entry.NumberOfBytes = size;
                    pfn(GetCurrentProcess(), 1, &entry, 0);
                }
            }
            return true;
        }
        case Advice::Normal:
        case Advice::Sequential:
        case Advice::Random:
        case Advice::HugePage:
        case Advice::NoHugePage:
        default:
            return true;  // No-op on Windows
    }
#else
    (void)address; (void)size; (void)advice;
    return true;
#endif
}

inline void* VirtualMemory::allocateAligned(size_t size, size_t alignment,
                                             Protection prot) {
    MappingFlags flags;
    flags.alignment = alignment;
    auto region = reserve(size, flags);
    if (!region.isValid()) return nullptr;
    if (!commitAll(region, prot)) {
        release(region);
        return nullptr;
    }
    return region.base;
}

inline void VirtualMemory::freeAligned(void* ptr, size_t size) {
    if (!ptr) return;
    VirtualRegion region;
    region.base = ptr;
    region.size = roundUpToPage(size);
    region.committedSize = region.size;
    release(region);
}

inline void VirtualMemory::zeroMemory(void* address, size_t size) {
    std::memset(address, 0, size);
}

inline void VirtualMemory::prefaultPages(void* address, size_t size) {
    volatile char* ptr = static_cast<volatile char*>(address);
    size_t pageSize = 4096;
    for (size_t i = 0; i < size; i += pageSize) {
        (void)ptr[i];  // Touch each page
    }
}

inline bool VirtualMemory::lockPages(void* address, size_t size) {
#ifdef __linux__
    return mlock(address, size) == 0;
#elif defined(_WIN32)
    return VirtualLock(address, size) != 0;
#else
    (void)address; (void)size;
    return true;
#endif
}

inline bool VirtualMemory::unlockPages(void* address, size_t size) {
#ifdef __linux__
    return munlock(address, size) == 0;
#elif defined(_WIN32)
    return VirtualUnlock(address, size) != 0;
#else
    (void)address; (void)size;
    return true;
#endif
}

inline size_t VirtualMemory::processVirtualSize() {
#ifdef __linux__
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    size_t pages = 0;
    if (fscanf(f, "%zu", &pages) != 1) pages = 0;
    fclose(f);
    return pages * static_cast<size_t>(sysconf(_SC_PAGESIZE));
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc;
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                            sizeof(pmc)))
        return static_cast<size_t>(pmc.PrivateUsage);
    return 0;
#else
    return 0;
#endif
}

inline size_t VirtualMemory::processResidentSize() {
#ifdef __linux__
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    size_t virt = 0, rss = 0;
    if (fscanf(f, "%zu %zu", &virt, &rss) != 2) rss = 0;
    fclose(f);
    return rss * static_cast<size_t>(sysconf(_SC_PAGESIZE));
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc;
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                            sizeof(pmc)))
        return static_cast<size_t>(pmc.WorkingSetSize);
    return 0;
#else
    return 0;
#endif
}

inline size_t VirtualMemory::availablePhysicalMemory() {
#ifdef __linux__
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        size_t val = 0;
        if (sscanf(line, "MemAvailable: %zu kB", &val) == 1) {
            fclose(f);
            return val * 1024;
        }
    }
    fclose(f);
    return 0;
#elif defined(_WIN32)
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
        return static_cast<size_t>(ms.ullAvailPhys);
    return 0;
#else
    return 0;
#endif
}

inline int VirtualMemory::protectionToNative(Protection prot) const {
#ifdef __linux__
    int native = PROT_NONE;
    if (prot & Protection::Read)    native |= PROT_READ;
    if (prot & Protection::Write)   native |= PROT_WRITE;
    if (prot & Protection::Execute) native |= PROT_EXEC;
    return native;
#elif defined(_WIN32)
    bool r = prot & Protection::Read;
    bool w = prot & Protection::Write;
    bool x = prot & Protection::Execute;
    if (x && w)  return static_cast<int>(PAGE_EXECUTE_READWRITE);
    if (x && r)  return static_cast<int>(PAGE_EXECUTE_READ);
    if (x)       return static_cast<int>(PAGE_EXECUTE);
    if (w)       return static_cast<int>(PAGE_READWRITE);
    if (r)       return static_cast<int>(PAGE_READONLY);
    return static_cast<int>(PAGE_NOACCESS);
#else
    (void)prot;
    return 0;
#endif
}

inline void VirtualMemory::trackRegion(const VirtualRegion& region) {
    regions_[reinterpret_cast<uintptr_t>(region.base)] = region;
}

inline void VirtualMemory::untrackRegion(const VirtualRegion& region) {
    regions_.erase(reinterpret_cast<uintptr_t>(region.base));
}

// AddressSpaceLayout

inline AddressSpaceLayout::AddressSpaceLayout(VirtualMemory& vmem,
                                               const SpaceConfig& config)
    : vmem_(vmem), config_(config) {}

inline AddressSpaceLayout::~AddressSpaceLayout() {
    if (baseRegion_.isValid()) {
        vmem_.release(baseRegion_);
    }
}

inline bool AddressSpaceLayout::initialize() {
    size_t totalSize = config_.nurserySize * 2 +  // from + to space
                       config_.oldGenMaxSize +
                       config_.losMaxSize +
                       config_.codeSpaceMaxSize +
                       config_.guardPageSize * 4;  // guards between spaces

    MappingFlags flags;
    flags.noReserve = true;
    baseRegion_ = vmem_.reserve(totalSize, flags);
    if (!baseRegion_.isValid()) return false;

    char* base = static_cast<char*>(baseRegion_.base);
    size_t offset = 0;

    nursery_.base = base + offset;
    nursery_.size = config_.nurserySize * 2;
    nursery_.name = "nursery";
    offset += nursery_.size + config_.guardPageSize;

    oldGen_.base = base + offset;
    oldGen_.size = config_.oldGenMaxSize;
    oldGen_.name = "old_gen";
    offset += oldGen_.size + config_.guardPageSize;

    los_.base = base + offset;
    los_.size = config_.losMaxSize;
    los_.name = "los";
    offset += los_.size + config_.guardPageSize;

    codeSpace_.base = base + offset;
    codeSpace_.size = config_.codeSpaceMaxSize;
    codeSpace_.name = "code";

    // Set guard pages between spaces
    vmem_.setGuardPage(static_cast<char*>(nursery_.base) + nursery_.size);
    vmem_.setGuardPage(static_cast<char*>(oldGen_.base) + oldGen_.size);
    vmem_.setGuardPage(static_cast<char*>(los_.base) + los_.size);

    return true;
}

inline AddressSpaceLayout::Space
AddressSpaceLayout::identifyPointer(const void* ptr) const {
    if (nursery_.contains(ptr)) return Space::Nursery;
    if (oldGen_.contains(ptr)) return Space::OldGen;
    if (los_.contains(ptr)) return Space::LOS;
    if (codeSpace_.contains(ptr)) return Space::Code;
    return Space::Unknown;
}

inline bool AddressSpaceLayout::isHeapPointer(const void* ptr) const {
    return identifyPointer(ptr) != Space::Unknown;
}

} // namespace Zepra::Heap
