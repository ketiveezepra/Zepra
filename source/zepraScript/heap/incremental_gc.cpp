// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file incremental_gc.cpp
 * @brief Incremental garbage collection implementation
 */

#include "heap/incremental_gc.hpp"
#include "runtime/objects/object.hpp"
#include <algorithm>
#include <unordered_set>

namespace Zepra::GC {

IncrementalGC::IncrementalGC(Heap* heap) : heap_(heap) {}

void IncrementalGC::startCollection() {
    if (phase_ != GCPhase::Idle) return;
    
    phase_ = GCPhase::Marking;
    markIndex_ = 0;
    sweepIndex_ = 0;
    markQueue_.clear();
    
    // Populate mark queue from root objects
    heap_->visitRoots([this](Runtime::Object* obj) {
        if (obj && !obj->isMarked()) {
            markQueue_.push_back(obj);
        }
    });
}

bool IncrementalGC::step() {
    stepStart_ = std::chrono::steady_clock::now();
    
    bool complete = false;
    
    switch (phase_) {
        case GCPhase::Idle:
            return true;
            
        case GCPhase::Marking:
            complete = markStep();
            if (complete) {
                phase_ = GCPhase::Sweeping;
                sweepIndex_ = 0;
            }
            break;
            
        case GCPhase::Sweeping:
            complete = sweepStep();
            if (complete) {
                finishCollection();
            }
            break;
            
        case GCPhase::Complete:
            return true;
    }
    
    auto elapsed = std::chrono::steady_clock::now() - stepStart_;
    totalPauseMs_ += std::chrono::duration<double, std::milli>(elapsed).count();
    
    return phase_ == GCPhase::Idle;
}

bool IncrementalGC::markStep() {
    size_t processed = 0;
    
    // Use a set for O(1) dedup on large heaps
    static thread_local std::unordered_set<Runtime::Object*> inQueue;
    if (markIndex_ == 0) {
        inQueue.clear();
        for (auto* obj : markQueue_) inQueue.insert(obj);
    }
    
    while (markIndex_ < markQueue_.size() && 
           processed < config_.markBatchSize) {
        Runtime::Object* obj = markQueue_[markIndex_++];
        
        if (!obj || obj->isMarked()) continue;
        
        obj->markGC();
        processed++;
        
        // Traverse object references and add to queue
        // inQueue is thread_local (not captured); visitRefs runs on this thread
        obj->visitRefs([this](Runtime::Object* ref) {
            if (ref && !ref->isMarked() && inQueue.find(ref) == inQueue.end()) {
                markQueue_.push_back(ref);
                inQueue.insert(ref);
            }
        });
        
        // Check time limit
        auto elapsed = std::chrono::steady_clock::now() - stepStart_;
        if (std::chrono::duration<double, std::milli>(elapsed).count() 
            >= config_.maxIncrementMs) {
            break;
        }
    }
    
    if (markIndex_ >= markQueue_.size()) {
        inQueue.clear();
    }
    
    return markIndex_ >= markQueue_.size();
}

bool IncrementalGC::sweepStep() {
    const auto& objects = heap_->getAllObjects();
    size_t processed = 0;
    
    while (sweepIndex_ < objects.size() && processed < config_.markBatchSize) {
        Runtime::Object* obj = static_cast<Runtime::Object*>(objects[sweepIndex_]);
        if (obj && obj->isMarked()) {
            // Survivor — clear mark for next cycle
            obj->clearMark();
        }
        // Unmarked objects will be freed by heap_->collectGarbage() after we finish
        sweepIndex_++;
        processed++;
    }
    
    // Once all objects are scanned, trigger actual sweep via heap
    if (sweepIndex_ >= objects.size()) {
        heap_->collectGarbage(false);
    }
    
    return sweepIndex_ >= objects.size();
}

void IncrementalGC::finishCollection() {
    phase_ = GCPhase::Idle;
    markQueue_.clear();
    markQueue_.shrink_to_fit();
}

// =============================================================================
// Write Barrier
// =============================================================================

void WriteBarrier::onFieldWrite(Runtime::Object* holder, Runtime::Object* newValue) {
    // If holder is black and newValue is white, we need to re-gray holder
    // This maintains the tri-color invariant
    if (holder && holder->isMarked() && 
        newValue && !newValue->isMarked()) {
        // Re-add to mark queue or mark directly
        newValue->markGC();
    }
}

void WriteBarrier::onArrayWrite(Runtime::Object* array, size_t, Runtime::Object* newValue) {
    onFieldWrite(array, newValue);
}

} // namespace Zepra::GC
