// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file ZOptLoopInvariant.h
 * @brief ZOpt Loop-Invariant Code Motion (LICM) Pass
 * 
 * Hoists loop-invariant computations out of loops.
 * A computation is loop-invariant if its operands are defined
 * outside the loop or are themselves loop-invariant.
 */

#pragma once

#include "../ZOptGraph.h"
#include <unordered_set>
#include <vector>

namespace Zepra::ZOpt {

class LoopInvariantCodeMotion {
public:
    explicit LoopInvariantCodeMotion(Graph* graph) : graph_(graph) {}
    
    bool run();
    
private:
    struct LoopInfo {
        BasicBlock* header = nullptr;
        std::unordered_set<BasicBlock*> blocks;
        BasicBlock* preheader = nullptr;
    };
    
    void findLoops();
    void processLoop(LoopInfo& loop);
    bool isLoopInvariant(Value* v, const LoopInfo& loop);
    bool canHoist(Value* v);
    void hoistValue(Value* v, LoopInfo& loop);
    
    Graph* graph_;
    std::vector<LoopInfo> loops_;
};

// =============================================================================
// Implementation
// =============================================================================

inline bool LoopInvariantCodeMotion::run() {
    findLoops();
    
    if (loops_.empty()) {
        return false;
    }
    
    bool changed = false;
    
    for (auto& loop : loops_) {
        // Ensure loop has preheader
        if (!loop.preheader) {
            // Create preheader if needed
            loop.preheader = graph_->addBlock();
            // Insert before loop header
            // This is simplified - real impl needs CFG surgery
        }
        
        processLoop(loop);
        changed = true;
    }
    
    return changed;
}

inline void LoopInvariantCodeMotion::findLoops() {
    loops_.clear();
    
    // Find loop headers (blocks with back edges)
    for (uint32_t i = 0; i < graph_->numBlocks(); i++) {
        BasicBlock* block = graph_->block(i);
        if (!block) continue;
        if (block->isLoopHeader()) {
            LoopInfo loop;
            loop.header = block;
            loop.blocks.insert(block);
            
            // Find loop body (blocks that can reach header via back edge)
            for (BasicBlock* pred : block->predecessors()) {
                if (pred->index() >= block->index()) {
                    loop.blocks.insert(pred);
                }
            }
            
            // Find preheader (predecessor not in loop)
            for (BasicBlock* pred : block->predecessors()) {
                if (loop.blocks.find(pred) == loop.blocks.end()) {
                    loop.preheader = pred;
                    break;
                }
            }
            
            loops_.push_back(loop);
        }
    }
}

inline void LoopInvariantCodeMotion::processLoop(LoopInfo& loop) {
    if (!loop.preheader) return;
    
    bool changed;
    do {
        changed = false;
        
        for (BasicBlock* block : loop.blocks) {
            // Collect values to hoist (can't modify while iterating)
            std::vector<Value*> toHoist;
            
            for (Value* v : block->values()) {
                if (isLoopInvariant(v, loop) && canHoist(v)) {
                    toHoist.push_back(v);
                }
            }
            
            for (Value* v : toHoist) {
                hoistValue(v, loop);
                changed = true;
            }
        }
    } while (changed);
}

inline bool LoopInvariantCodeMotion::isLoopInvariant(Value* v, const LoopInfo& loop) {
    if (v->isDead()) return false;
    
    // Constants are always loop-invariant
    if (v->isConstant()) return true;
    
    // Phis are not loop-invariant
    if (v->opcode() == Opcode::Phi) return false;
    
    // Check all inputs
    for (uint32_t i = 0; i < v->numInputs(); ++i) {
        Value* input = v->input(i);
        if (!input) continue;
        
        BasicBlock* defBlock = input->block();
        if (!defBlock) continue;
        
        // Input must be defined outside loop or be loop-invariant
        if (loop.blocks.find(defBlock) != loop.blocks.end()) {
            // Defined inside loop - only OK if already loop-invariant
            if (!input->isConstant()) {
                return false;
            }
        }
    }
    
    return true;
}

inline bool LoopInvariantCodeMotion::canHoist(Value* v) {
    // Don't hoist operations with side effects
    if (hasSideEffects(v->opcode())) return false;
    
    // Don't hoist memory operations (may trap)
    switch (v->opcode()) {
        case Opcode::Load32:
        case Opcode::Load64:
        case Opcode::LoadF32:
        case Opcode::LoadF64:
        case Opcode::Store32:
        case Opcode::Store64:
        case Opcode::StoreF32:
        case Opcode::StoreF64:
        case Opcode::BoundsCheck:
            return false;
            
        case Opcode::Call:
        case Opcode::CallIndirect:
            return false;
            
        default:
            break;
    }
    
    // Don't hoist terminators
    if (isTerminator(v->opcode())) return false;
    
    return true;
}

inline void LoopInvariantCodeMotion::hoistValue(Value* v, LoopInfo& loop) {
    if (!loop.preheader) return;
    
    // Move value from current block to preheader
    BasicBlock* currentBlock = v->block();
    if (currentBlock) {
        currentBlock->removeValue(v);
    }
    
    // Append to preheader (before terminator if any)
    loop.preheader->appendValue(v);
    v->setBlock(loop.preheader);
}

} // namespace Zepra::ZOpt
