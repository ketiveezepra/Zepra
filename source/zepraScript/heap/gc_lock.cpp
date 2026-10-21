// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// ZepraScript — gc_lock.cpp — GC-specific locks: heap lock, allocation lock, sweep lock

#include <cstdint>
#include <cstdio>
#include <cassert>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <chrono>

namespace Zepra::Heap {

// Spinlock for hot-path GC operations (short critical sections only).
class GCSpinLock {
public:
    GCSpinLock() : flag_(false) {}

    void lock() {
        while (flag_.exchange(true, std::memory_order_acquire)) {
            while (flag_.load(std::memory_order_relaxed)) {
#if defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
                __asm__ __volatile__("yield");
#endif
            }
        }
    }

    bool tryLock() {
        return !flag_.exchange(true, std::memory_order_acquire);
    }

    void unlock() {
        flag_.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> flag_;
};

// RAII guard for GCSpinLock.
class GCSpinLockGuard {
public:
    explicit GCSpinLockGuard(GCSpinLock& lock) : lock_(lock) { lock_.lock(); }
    ~GCSpinLockGuard() { lock_.unlock(); }
    GCSpinLockGuard(const GCSpinLockGuard&) = delete;
    GCSpinLockGuard& operator=(const GCSpinLockGuard&) = delete;
private:
    GCSpinLock& lock_;
};

// Heap lock: protects major heap mutations (arena allocation, zone creation).
// Uses read-write lock: readers = mutators, writer = GC.
class HeapLock {
public:
    void lockForMutation() { rwLock_.lock_shared(); }
    void unlockMutation() { rwLock_.unlock_shared(); }
    void lockForGC() { rwLock_.lock(); }
    void unlockGC() { rwLock_.unlock(); }
    bool tryLockForGC() { return rwLock_.try_lock(); }

private:
    std::shared_mutex rwLock_;
};

// RAII guards for HeapLock.
class MutationLockGuard {
public:
    explicit MutationLockGuard(HeapLock& lock) : lock_(lock) { lock_.lockForMutation(); }
    ~MutationLockGuard() { lock_.unlockMutation(); }
    MutationLockGuard(const MutationLockGuard&) = delete;
    MutationLockGuard& operator=(const MutationLockGuard&) = delete;
private:
    HeapLock& lock_;
};

class GCLockGuard {
public:
    explicit GCLockGuard(HeapLock& lock) : lock_(lock) { lock_.lockForGC(); }
    ~GCLockGuard() { lock_.unlockGC(); }
    GCLockGuard(const GCLockGuard&) = delete;
    GCLockGuard& operator=(const GCLockGuard&) = delete;
private:
    HeapLock& lock_;
};

// Allocation lock: per-zone, protects free list and arena allocation.
class AllocationLock {
public:
    void lock() { mutex_.lock(); }
    void unlock() { mutex_.unlock(); }
    bool tryLock() { return mutex_.try_lock(); }
private:
    std::mutex mutex_;
};

class AllocationLockGuard {
public:
    explicit AllocationLockGuard(AllocationLock& lock) : lock_(lock) { lock_.lock(); }
    ~AllocationLockGuard() { lock_.unlock(); }
    AllocationLockGuard(const AllocationLockGuard&) = delete;
    AllocationLockGuard& operator=(const AllocationLockGuard&) = delete;
private:
    AllocationLock& lock_;
};

// Sweep lock: allows concurrent sweeping while blocking allocation on swept arenas.
class SweepLock {
public:
    void lockForSweep() { rwLock_.lock(); }
    void unlockSweep() { rwLock_.unlock(); }
    void lockForRead() { rwLock_.lock_shared(); }
    void unlockRead() { rwLock_.unlock_shared(); }
    bool tryLockForSweep() { return rwLock_.try_lock(); }
private:
    std::shared_mutex rwLock_;
};

// Lock statistics.
struct LockStats {
    std::atomic<uint64_t> heapLockAcquires{0};
    std::atomic<uint64_t> heapLockContention{0};
    std::atomic<uint64_t> allocLockAcquires{0};
    std::atomic<uint64_t> spinLockSpins{0};
};

} // namespace Zepra::Heap
