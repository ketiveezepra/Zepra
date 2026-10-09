// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file gc_heap.cpp
 * @brief Generational garbage collector implementation
 * 
 * Two-generation mark-sweep collector:
 * - Young generation: Bump-pointer allocation, frequent minor GC
 * - Old generation: Free-list allocation, infrequent major GC
 */

#include "heap/gc_heap.hpp"
#include "runtime/objects/object.hpp"
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace Zepra::Runtime {

// =============================================================================
// GCHeap Constructor/Destructor
// =============================================================================

GCHeap::GCHeap() {
    // Allocate young generation space
    youngStart_ = static_cast<char*>(std::malloc(YOUNG_GEN_SIZE));
    if (youngStart_) {
        youngEnd_ = youngStart_ + YOUNG_GEN_SIZE;
        youngPtr_ = youngStart_;
        std::memset(youngStart_, 0, YOUNG_GEN_SIZE);
    }
    
    stats_.youngGenSize = 0;
    stats_.oldGenSize = 0;
}

GCHeap::~GCHeap() {
    // Free all old generation objects
    ObjectHeader* header = oldList_;
    while (header) {
        ObjectHeader* next = header->next;
        Object* obj = static_cast<Object*>(header->object());
        obj->~Object();
        std::free(header);
        header = next;
    }
    
    // Free young generation objects (call destructors)
    header = youngList_;
    while (header) {
        Object* obj = static_cast<Object*>(header->object());
        obj->~Object();
        header = header->next;
    }
    
    // Free young generation space
    if (youngStart_) {
        std::free(youngStart_);
    }
}

// =============================================================================
// Allocation
// =============================================================================

void* GCHeap::allocateYoung(size_t size) {
    // Align to 8 bytes
    size = (size + 7) & ~7;
    
    if (youngPtr_ + size > youngEnd_) {
        return nullptr;  // Young gen full
    }
    
    void* ptr = youngPtr_;
    youngPtr_ += size;
    return ptr;
}

void* GCHeap::allocateOld(size_t size) {
    // Align to 8 bytes
    size = (size + 7) & ~7;
    
    // Old gen uses malloc directly
    void* ptr = std::malloc(size);
    if (ptr) {
        stats_.oldGenSize += size;
    }
    return ptr;
}

// =============================================================================
// Root Management
// =============================================================================

void GCHeap::addRoot(Object** root) {
    roots_.insert(root);
}

void GCHeap::removeRoot(Object** root) {
    roots_.erase(root);
}

// =============================================================================
// Write Barrier
// =============================================================================

void GCHeap::writeBarrier(Object* source, Object* target) {
    if (!source || !target) return;
    
    ObjectHeader* sourceHeader = ObjectHeader::fromObject(source);
    ObjectHeader* targetHeader = ObjectHeader::fromObject(target);
    
    // Only care about old → young references
    if (sourceHeader->generation == Generation::Old &&
        targetHeader->generation == Generation::Young) {
        rememberedSet_.insert(source);
    }
}

// =============================================================================
// Tracer Registration
// =============================================================================

void GCHeap::registerTracer(TraceFn tracer) {
    tracers_.push_back(std::move(tracer));
}

// =============================================================================
// Object Tracing
// =============================================================================

void GCHeap::traceObject(Object* obj) {
    if (!obj) return;
    
    auto marker = [this](Object* ref) {
        this->markObject(ref);
    };
    
    for (auto& tracer : tracers_) {
        tracer(obj, marker);
    }
}

// =============================================================================
// Marking Phase
// =============================================================================

void GCHeap::markObject(Object* obj) {
    if (!obj) return;
    
    // Only mark objects allocated via GCHeap (have ObjectHeader).
    // Static prototypes (stringProto, arrayProto, etc.) are raw new'd
    // and must not be treated as GCHeap-managed.
    if (!obj->isGCManaged()) return;
    
    ObjectHeader* header = ObjectHeader::fromObject(obj);
    if (header->marked) return;
    
    header->marked = true;
    traceObject(obj);
}

void GCHeap::markFromRoots() {
    // Mark from explicit roots (Object** pointers)
    for (Object** root : roots_) {
        if (*root) {
            markObject(*root);
        }
    }
    
    // Mark from root scanner callback (VM globals, stack, call frames)
    if (rootScanner_) {
        rootScanner_([this](Object* obj) {
            if (obj) markObject(obj);
        });
    }
    
    // Mark from remembered set (old→young cross-gen refs)
    for (Object* obj : rememberedSet_) {
        traceObject(obj);
    }
}

// =============================================================================
// Minor GC (Young Generation)
// =============================================================================

void GCHeap::collectMinor() {
    stats_.minorCollections++;
    
    // Mark phase - only mark objects reachable from roots/remembered set
    markFromRoots();
    
    // Sweep young generation
    sweepYoung();
    
    // Clear remembered set (will be rebuilt as mutator runs)
    rememberedSet_.clear();
}

void GCHeap::sweepYoung() {
    ObjectHeader** ptr = &youngList_;
    std::vector<ObjectHeader*> survivors;
    
    while (*ptr) {
        ObjectHeader* header = *ptr;
        
        if (header->marked) {
            // Object survived
            header->marked = false;
            header->age++;
            
            if (header->age >= PROMOTION_AGE) {
                // Promote to old generation
                promoteObject(header);
                // Remove from young list
                *ptr = header->next;
                if (header->next) {
                    header->next->prev = header->prev;
                }
            } else {
                survivors.push_back(header);
                ptr = &header->next;
            }
        } else {
            // Object is garbage
            Object* obj = static_cast<Object*>(header->object());
            obj->~Object();  // Call destructor
            
            stats_.objectsFreed++;
            stats_.totalFreed += sizeof(ObjectHeader) + header->size;
            stats_.youngGenSize -= sizeof(ObjectHeader) + header->size;
            
            // Remove from list (memory stays in young gen space)
            *ptr = header->next;
            if (header->next) {
                header->next->prev = header->prev;
            }
        }
    }
    
    // Compact young generation if mostly empty
    if (survivors.empty()) {
        youngPtr_ = youngStart_;  // Reset bump pointer
    }
}

// =============================================================================
// Object Promotion
// =============================================================================

void GCHeap::promoteObject(ObjectHeader* header) {
    size_t totalSize = sizeof(ObjectHeader) + header->size;
    
    // Allocate in old generation
    void* newMem = allocateOld(totalSize);
    if (!newMem) {
        // OOM in old gen - trigger major GC and retry
        collectMajor();
        newMem = allocateOld(totalSize);
        if (!newMem) return;  // Still OOM
    }
    
    // Copy object to old generation
    std::memcpy(newMem, header, totalSize);
    
    ObjectHeader* newHeader = static_cast<ObjectHeader*>(newMem);
    newHeader->generation = Generation::Old;
    newHeader->age = 0;
    newHeader->marked = false;
    
    // Add to old list
    newHeader->next = oldList_;
    newHeader->prev = nullptr;
    if (oldList_) oldList_->prev = newHeader;
    oldList_ = newHeader;
    
    stats_.promotions++;
    stats_.youngGenSize -= totalSize;
}

// =============================================================================
// Major GC (Full Collection)
// =============================================================================

void GCHeap::collectMajor() {
    stats_.majorCollections++;
    
    // Mark everything from roots
    markFromRoots();
    
    // Also mark from old generation roots
    ObjectHeader* header = oldList_;
    while (header) {
        if (header->marked) {
            traceObject(static_cast<Object*>(header->object()));
        }
        header = header->next;
    }
    
    // Sweep both generations
    sweepYoung();
    sweepOld();
}

void GCHeap::sweepOld() {
    ObjectHeader** ptr = &oldList_;
    
    while (*ptr) {
        ObjectHeader* header = *ptr;
        
        if (header->marked) {
            // Object survives
            header->marked = false;
            ptr = &header->next;
        } else {
            // Object is garbage
            Object* obj = static_cast<Object*>(header->object());
            obj->~Object();
            
            size_t totalSize = sizeof(ObjectHeader) + header->size;
            stats_.objectsFreed++;
            stats_.totalFreed += totalSize;
            stats_.oldGenSize -= totalSize;
            
            // Remove from list and free memory
            ObjectHeader* next = header->next;
            if (header->next) {
                header->next->prev = header->prev;
            }
            *ptr = next;
            
            std::free(header);
        }
    }
}

// =============================================================================
// Adaptive Collection
// =============================================================================

void GCHeap::maybeCollect() {
    // Trigger minor GC if young gen is 75% full
    size_t youngUsed = static_cast<size_t>(youngPtr_ - youngStart_);
    size_t threshold = (YOUNG_GEN_SIZE * 3) / 4;
    
    if (youngUsed > threshold) {
        collectMinor();
    }
    
    // Trigger major GC if old gen is too large (> 10 MB)
    constexpr size_t OLD_GEN_THRESHOLD = 10 * 1024 * 1024;
    if (stats_.oldGenSize > OLD_GEN_THRESHOLD) {
        collectMajor();
    }
}

} // namespace Zepra::Runtime
