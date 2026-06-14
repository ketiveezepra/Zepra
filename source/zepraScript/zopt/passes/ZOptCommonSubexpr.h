// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file ZOptCommonSubexpr.h
 * @brief ZOpt Common Subexpression Elimination Pass
 * 
 * Finds duplicate computations and replaces them with the first occurrence.
 */

#pragma once

#include "../ZOptGraph.h"
#include <unordered_map>
#include <unordered_set>

namespace Zepra::ZOpt {

// Hash for value identity (opcode + inputs)
struct ValueHash {
    size_t operator()(const Value* v) const {
        size_t hash = static_cast<size_t>(v->opcode());
        for (uint32_t i = 0; i < v->numInputs(); ++i) {
            hash ^= reinterpret_cast<size_t>(v->input(i)) << ((i + 1) * 8);
        }
        if (v->isConstant()) {
            hash ^= static_cast<size_t>(v->constant().i64);
        }
        return hash;
    }
};

struct ValueEqual {
    bool operator()(const Value* a, const Value* b) const {
        if (a->opcode() != b->opcode()) return false;
        if (a->type() != b->type()) return false;
        if (a->numInputs() != b->numInputs()) return false;
        
        for (uint32_t i = 0; i < a->numInputs(); ++i) {
            if (a->input(i) != b->input(i)) return false;
        }
        
        if (a->isConstant() && b->isConstant()) {
            return a->constant().i64 == b->constant().i64;
        }
        
        return !a->isConstant() && !b->isConstant();
    }
};

class CommonSubexpressionElimination {
public:
    explicit CommonSubexpressionElimination(Graph* graph) : graph_(graph) {}
    
    bool run() {
        bool changed = false;
        
        // Process blocks in dominator order (approximated by RPO)
        for (BasicBlock* block : graph_->reversePostOrder()) {
            changed |= processBlock(block);
        }
        
        if (changed) {
            graph_->removeDeadValues();
        }
        
        return changed;
    }
    
private:
    bool processBlock(BasicBlock* block) {
        bool changed = false;
        
        for (Value* v : block->values()) {
            if (v->isDead()) continue;
            if (!canCSE(v)) continue;
            
            auto it = available_.find(v);
            if (it != available_.end()) {
                // Found equivalent value, replace
                v->replaceAllUsesWith(*it);
                v->markDead();
                changed = true;
            } else {
                // First occurrence, add to available set
                available_.insert(v);
            }
        }
        
        return changed;
    }
    
    bool canCSE(Value* v) {
        // Cannot CSE values with side effects
        if (hasSideEffects(v->opcode())) return false;
        
        // Cannot CSE terminators
        if (isTerminator(v->opcode())) return false;
        
        // Cannot CSE phis (handled specially)
        if (v->opcode() == Opcode::Phi) return false;
        
        // Cannot CSE loads (may alias)
        switch (v->opcode()) {
            case Opcode::Load8S:
            case Opcode::Load8U:
            case Opcode::Load16S:
            case Opcode::Load16U:
            case Opcode::Load32:
            case Opcode::Load64:
            case Opcode::LoadF32:
            case Opcode::LoadF64:
                return false;
            default:
                break;
        }
        
        return true;
    }
    
    Graph* graph_;
    std::unordered_set<Value*, ValueHash, ValueEqual> available_;
};

} // namespace Zepra::ZOpt
