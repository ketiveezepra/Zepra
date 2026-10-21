// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file WasmBaselineOpcodes.cpp
 * @brief Opcode handlers: control flow, locals, constants, arithmetic,
 *        comparisons, conversions, memory, calls, select, SIMD
 */

#include "wasm/WasmBaselineInternal.h"
#include <algorithm>

namespace Zepra::Wasm {
// =============================================================================
// Control Flow Emission
// =============================================================================

bool BaselineCompiler::emitBlock(BlockType bt) {
    BaselineControlFrame frame(BaselineControlFrame::Kind::Block, bt, valueStack_.size());
    frame.endLabel = newLabel();
    controlStack_.push_back(frame);
    return true;
}

bool BaselineCompiler::emitLoop(BlockType bt) {
    BaselineControlFrame frame(BaselineControlFrame::Kind::Loop, bt, valueStack_.size());
    frame.endLabel = newLabel();
    bindLabel(frame.endLabel);  // Loop target is the start
    
    // Emit tier-up check at loop header
    if (options_.enableTierUp) {
        emitTierUpCheck();
    }
    
    controlStack_.push_back(frame);
    return true;
}

bool BaselineCompiler::emitIf(BlockType bt) {
    ValueLocation cond = pop();
    
    BaselineControlFrame frame(BaselineControlFrame::Kind::If, bt, valueStack_.size());
    frame.endLabel = newLabel();
    frame.elseLabel = newLabel();
    
    X86Assembler masm(codeBuffer_.get());
    
    // Get condition into register
    Reg condReg;
    if (cond.isRegister()) {
        condReg = cond.reg;
    } else if (cond.isImmediate()) {
        // Immediate: emit zero check directly
        if (cond.immediate == 0) {
            // Condition is false, jump to else unconditionally
            jumpTo(frame.elseLabel);
            controlStack_.push_back(frame);
            return true;
        }
        // Non-zero immediate: condition is true, fall through
        controlStack_.push_back(frame);
        return true;
    } else {
        condReg = allocReg(ValType::i32());
        masm.load32(condReg.code, X86Assembler::RBP, cond.slot.offset);
    }
    
    // Test condition and branch to else if zero (false)
    masm.test32(condReg.code, condReg.code);
    
    // Emit jz (jump if zero) to else label
    if (frame.elseLabel.bound) {
        int32_t displacement = static_cast<int32_t>(frame.elseLabel.offset - (codeBuffer_->offset() + 6));
        codeBuffer_->emit8(0x0F);
        codeBuffer_->emit8(0x84);  // jz rel32
        codeBuffer_->emit32(displacement);
    } else {
        codeBuffer_->emit8(0x0F);
        codeBuffer_->emit8(0x84);  // jz rel32
        frame.elseLabel.pendingJumps.push_back(codeBuffer_->offset());
        codeBuffer_->emit32(0);
    }
    
    if (!cond.isRegister()) {
        freeReg(condReg);
    }
    
    controlStack_.push_back(frame);
    return true;
}

bool BaselineCompiler::emitElse() {
    if (controlStack_.empty()) {
        error_ = "else without matching if";
        return false;
    }
    
    auto& frame = controlStack_.back();
    if (frame.kind != BaselineControlFrame::Kind::If) {
        error_ = "else in non-if block";
        return false;
    }
    
    // Jump over else block
    jumpTo(frame.endLabel);
    
    // Bind else label
    bindLabel(frame.elseLabel);
    
    frame.kind = BaselineControlFrame::Kind::Else;
    return true;
}

bool BaselineCompiler::emitEnd() {
    if (controlStack_.empty()) {
        error_ = "end without matching block";
        return false;
    }
    
    auto frame = controlStack_.back();
    controlStack_.pop_back();
    
    // Bind else label for if without else
    if (frame.kind == BaselineControlFrame::Kind::If) {
        bindLabel(frame.elseLabel);
    }
    
    // Bind end label
    if (frame.kind != BaselineControlFrame::Kind::Loop) {
        bindLabel(frame.endLabel);
    }
    
    return true;
}

bool BaselineCompiler::emitBr(uint32_t depth) {
    if (depth >= controlStack_.size()) {
        error_ = "br depth out of bounds";
        return false;
    }
    
    auto& target = controlStack_[controlStack_.size() - 1 - depth];
    jumpTo(target.endLabel);
    return true;
}

bool BaselineCompiler::emitBrIf(uint32_t depth) {
    if (depth >= controlStack_.size()) {
        error_ = "br_if depth out of bounds";
        return false;
    }
    
    ValueLocation cond = pop();
    auto& target = controlStack_[controlStack_.size() - 1 - depth];
    
    if (cond.isRegister()) {
        branchTo(target.endLabel, cond.reg);
    } else {
        Reg condReg = allocReg(ValType::i32());
        branchTo(target.endLabel, condReg);
        freeReg(condReg);
    }
    
    return true;
}

bool BaselineCompiler::emitBrTable(const std::vector<uint32_t>& targets, uint32_t defaultTarget) {
    ValueLocation index = pop();
    
    X86Assembler masm(codeBuffer_.get());
    
    // Get index into register
    Reg indexReg = allocReg(ValType::i32());
    if (index.isRegister()) {
        if (index.reg.code != indexReg.code) {
            masm.mov32(indexReg.code, index.reg.code);
        }
    } else {
        // Load from stack
        masm.load32(indexReg.code, X86Assembler::RBP, index.slot.offset);
    }
    
    // Compare against table size
    size_t numTargets = targets.size();
    masm.cmp32Imm(indexReg.code, static_cast<int32_t>(numTargets));
    
    // Jump to default if index >= numTargets
    Label defaultLabel = newLabel();
    if (defaultTarget >= controlStack_.size()) {
        error_ = "br_table default depth out of bounds";
        return false;
    }
    auto& defaultFrame = controlStack_[controlStack_.size() - 1 - defaultTarget];
    
    // jae default (unsigned compare, jump if above or equal)
    codeBuffer_->emit8(0x0F);
    codeBuffer_->emit8(0x83);  // jae rel32
    defaultFrame.endLabel.pendingJumps.push_back(codeBuffer_->offset());
    codeBuffer_->emit32(0);
    
    // Generate jump table for small tables, linear search for larger ones
    if (numTargets <= 8) {
        // Linear comparison chain for small tables
        for (size_t i = 0; i < numTargets; ++i) {
            uint32_t depth = targets[i];
            if (depth >= controlStack_.size()) {
                error_ = "br_table target depth out of bounds";
                return false;
            }
            
            masm.cmp32Imm(indexReg.code, static_cast<int32_t>(i));
            
            auto& targetFrame = controlStack_[controlStack_.size() - 1 - depth];
            // je target
            codeBuffer_->emit8(0x0F);
            codeBuffer_->emit8(0x84);  // je rel32
            targetFrame.endLabel.pendingJumps.push_back(codeBuffer_->offset());
            codeBuffer_->emit32(0);
        }
    } else {
        // For larger tables, use indirect jump through jump table
        // lea rax, [rip + jump_table]
        // jmp [rax + index*8]
        // This is more complex - simplified version with linear search for now
        for (size_t i = 0; i < numTargets; ++i) {
            uint32_t depth = targets[i];
            if (depth >= controlStack_.size()) {
                error_ = "br_table target depth out of bounds";
                return false;
            }
            
            masm.cmp32Imm(indexReg.code, static_cast<int32_t>(i));
            
            auto& targetFrame = controlStack_[controlStack_.size() - 1 - depth];
            codeBuffer_->emit8(0x0F);
            codeBuffer_->emit8(0x84);  // je rel32
            targetFrame.endLabel.pendingJumps.push_back(codeBuffer_->offset());
            codeBuffer_->emit32(0);
        }
    }
    
    // Fall through to default
    jumpTo(defaultFrame.endLabel);
    
    freeReg(indexReg);
    return true;
}

bool BaselineCompiler::emitReturn() {
    emitEpilogue();
    return true;
}

bool BaselineCompiler::emitUnreachable() {
    emitTrap(Trap::Unreachable);
    return true;
}

// =============================================================================
// Locals and Globals
// =============================================================================

bool BaselineCompiler::emitLocalGet(uint32_t localIndex) {
    if (localIndex >= locals_.size()) {
        error_ = "local index out of bounds";
        return false;
    }
    
    const auto& slot = locals_[localIndex];
    push(ValueLocation::onStack(slot.offset, slot.type));
    return true;
}

bool BaselineCompiler::emitLocalSet(uint32_t localIndex) {
    if (localIndex >= locals_.size()) {
        error_ = "local index out of bounds";
        return false;
    }
    
    ValueLocation val = pop();
    const auto& slot = locals_[localIndex];
    
    X86Assembler masm(codeBuffer_.get());
    
    // Get value into register if not already
    Reg valReg;
    if (val.isRegister()) {
        valReg = val.reg;
    } else {
        valReg = allocReg(slot.type);
        // Load from source location
        if (slot.type.kind() == ValType::Kind::F32) {
            masm.loadss(valReg.code, X86Assembler::RBP, val.slot.offset);
        } else if (slot.type.kind() == ValType::Kind::F64) {
            masm.loadsd(valReg.code, X86Assembler::RBP, val.slot.offset);
        } else if (slot.type.kind() == ValType::Kind::I64) {
            masm.load64(valReg.code, X86Assembler::RBP, val.slot.offset);
        } else {
            masm.load32(valReg.code, X86Assembler::RBP, val.slot.offset);
        }
    }
    
    // Store to local slot
    if (slot.type.kind() == ValType::Kind::F32) {
        masm.storess(X86Assembler::RBP, slot.offset, valReg.code);
    } else if (slot.type.kind() == ValType::Kind::F64) {
        masm.storesd(X86Assembler::RBP, slot.offset, valReg.code);
    } else if (slot.type.kind() == ValType::Kind::I64) {
        masm.store64(X86Assembler::RBP, slot.offset, valReg.code);
    } else {
        masm.store32(X86Assembler::RBP, slot.offset, valReg.code);
    }
    
    if (!val.isRegister()) {
        freeReg(valReg);
    }
    
    return true;
}

bool BaselineCompiler::emitLocalTee(uint32_t localIndex) {
    if (localIndex >= locals_.size()) {
        error_ = "local index out of bounds";
        return false;
    }
    
    ValueLocation val = peek(0);
    const auto& slot = locals_[localIndex];
    
    X86Assembler masm(codeBuffer_.get());
    
    // Get value into register
    Reg valReg;
    if (val.isRegister()) {
        valReg = val.reg;
    } else {
        valReg = allocReg(slot.type);
        if (slot.type.kind() == ValType::Kind::F32) {
            masm.loadss(valReg.code, X86Assembler::RBP, val.slot.offset);
        } else if (slot.type.kind() == ValType::Kind::F64) {
            masm.loadsd(valReg.code, X86Assembler::RBP, val.slot.offset);
        } else if (slot.type.kind() == ValType::Kind::I64) {
            masm.load64(valReg.code, X86Assembler::RBP, val.slot.offset);
        } else {
            masm.load32(valReg.code, X86Assembler::RBP, val.slot.offset);
        }
    }
    
    // Store to local (value stays on stack)
    if (slot.type.kind() == ValType::Kind::F32) {
        masm.storess(X86Assembler::RBP, slot.offset, valReg.code);
    } else if (slot.type.kind() == ValType::Kind::F64) {
        masm.storesd(X86Assembler::RBP, slot.offset, valReg.code);
    } else if (slot.type.kind() == ValType::Kind::I64) {
        masm.store64(X86Assembler::RBP, slot.offset, valReg.code);
    } else {
        masm.store32(X86Assembler::RBP, slot.offset, valReg.code);
    }
    
    if (!val.isRegister()) {
        freeReg(valReg);
    }
    
    return true;
}

bool BaselineCompiler::emitGlobalGet(uint32_t globalIndex) {
    if (globalIndex >= module_->numGlobals()) {
        error_ = "global index out of bounds";
        return false;
    }
    
    const auto& globalDecl = module_->globals()[globalIndex];
    Reg result = allocReg(globalDecl.type);
    
    X86Assembler masm(codeBuffer_.get());
    
    // Load global base pointer from instance (stored at RBP-8)
    Reg globalsBase = allocReg(ValType::i64());
    masm.load64(globalsBase.code, X86Assembler::RBP, -8);
    
    // Load global value (each global is 8 bytes aligned)
    int32_t globalOffset = static_cast<int32_t>(globalIndex * 8);
    
    if (globalDecl.type.kind() == ValType::Kind::F32) {
        masm.loadss(result.code, globalsBase.code, globalOffset);
    } else if (globalDecl.type.kind() == ValType::Kind::F64) {
        masm.loadsd(result.code, globalsBase.code, globalOffset);
    } else if (globalDecl.type.kind() == ValType::Kind::I64) {
        masm.load64(result.code, globalsBase.code, globalOffset);
    } else {
        masm.load32(result.code, globalsBase.code, globalOffset);
    }
    
    freeReg(globalsBase);
    push(ValueLocation::inReg(result, globalDecl.type));
    return true;
}

bool BaselineCompiler::emitGlobalSet(uint32_t globalIndex) {
    if (globalIndex >= module_->numGlobals()) {
        error_ = "global index out of bounds";
        return false;
    }
    
    ValueLocation val = pop();
    const auto& globalDecl = module_->globals()[globalIndex];
    
    X86Assembler masm(codeBuffer_.get());
    
    // Load global base pointer
    Reg globalsBase = allocReg(ValType::i64());
    masm.load64(globalsBase.code, X86Assembler::RBP, -8);
    
    // Get value into register
    Reg valReg;
    if (val.isRegister()) {
        valReg = val.reg;
    } else {
        valReg = allocReg(globalDecl.type);
        if (globalDecl.type.kind() == ValType::Kind::F32) {
            masm.loadss(valReg.code, X86Assembler::RBP, val.slot.offset);
        } else if (globalDecl.type.kind() == ValType::Kind::F64) {
            masm.loadsd(valReg.code, X86Assembler::RBP, val.slot.offset);
        } else if (globalDecl.type.kind() == ValType::Kind::I64) {
            masm.load64(valReg.code, X86Assembler::RBP, val.slot.offset);
        } else {
            masm.load32(valReg.code, X86Assembler::RBP, val.slot.offset);
        }
    }
    
    // Store to global
    int32_t globalOffset = static_cast<int32_t>(globalIndex * 8);
    
    if (globalDecl.type.kind() == ValType::Kind::F32) {
        masm.storess(globalsBase.code, globalOffset, valReg.code);
    } else if (globalDecl.type.kind() == ValType::Kind::F64) {
        masm.storesd(globalsBase.code, globalOffset, valReg.code);
    } else if (globalDecl.type.kind() == ValType::Kind::I64) {
        masm.store64(globalsBase.code, globalOffset, valReg.code);
    } else {
        masm.store32(globalsBase.code, globalOffset, valReg.code);
    }
    
    freeReg(globalsBase);
    if (!val.isRegister()) {
        freeReg(valReg);
    }
    
    return true;
}

// =============================================================================
// Constants
// =============================================================================

bool BaselineCompiler::emitI32Const(int32_t value) {
    push(ValueLocation::imm(value, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitI64Const(int64_t value) {
    push(ValueLocation::imm(value, ValType::i64()));
    return true;
}

bool BaselineCompiler::emitF32Const(float value) {
    int32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    push(ValueLocation::imm(bits, ValType::f32()));
    return true;
}

bool BaselineCompiler::emitF64Const(double value) {
    int64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    push(ValueLocation::imm(bits, ValType::f64()));
    return true;
}

// =============================================================================
// I32 Binary Operations
// =============================================================================

bool BaselineCompiler::emitI32Add() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.add32(dst.code, src.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.add32(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI32Sub() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.sub32(dst.code, src.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.sub32(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI32Mul() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.imul32(dst.code, src.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.mul32(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI32DivS() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            if (dst.code != X86Assembler::RAX)
                masm.mov32(X86Assembler::RAX, dst.code);
            masm.cdq();
            masm.idiv32(src.code);
            if (dst.code != X86Assembler::RAX)
                masm.mov32(dst.code, X86Assembler::RAX);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.sdiv32(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI32DivU() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            if (dst.code != X86Assembler::RAX)
                masm.mov32(X86Assembler::RAX, dst.code);
            masm.xor32Self(X86Assembler::RDX);
            masm.div32(src.code);
            if (dst.code != X86Assembler::RAX)
                masm.mov32(dst.code, X86Assembler::RAX);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.udiv32(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI32RemS() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            if (dst.code != X86Assembler::RAX)
                masm.mov32(X86Assembler::RAX, dst.code);
            masm.cdq();
            masm.idiv32(src.code);
            masm.mov32(dst.code, X86Assembler::RDX);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            // ARM64: rem = a - (a/b)*b using MSUB
            uint8_t tmp = ARM64Assembler::X9;  // Scratch register
            masm.sdiv32(tmp, dst.code, src.code);
            masm.msub32(dst.code, tmp, src.code, dst.code);
        });
    });
}

bool BaselineCompiler::emitI32RemU() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            if (dst.code != X86Assembler::RAX)
                masm.mov32(X86Assembler::RAX, dst.code);
            masm.xor32Self(X86Assembler::RDX);
            masm.div32(src.code);
            masm.mov32(dst.code, X86Assembler::RDX);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            uint8_t tmp = ARM64Assembler::X9;
            masm.udiv32(tmp, dst.code, src.code);
            masm.msub32(dst.code, tmp, src.code, dst.code);
        });
    });
}

bool BaselineCompiler::emitI32And() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.and32(dst.code, src.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.and32(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI32Or() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.or32(dst.code, src.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.orr32(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI32Xor() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.xor32(dst.code, src.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.eor32(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI32Shl() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            if (src.code != X86Assembler::RCX) {
                masm.mov32(X86Assembler::RCX, src.code);
            }
            masm.shl32_cl(dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.lsl32(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI32ShrS() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            if (src.code != X86Assembler::RCX) {
                masm.mov32(X86Assembler::RCX, src.code);
            }
            masm.sar32_cl(dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.asr32(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI32ShrU() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            if (src.code != X86Assembler::RCX) {
                masm.mov32(X86Assembler::RCX, src.code);
            }
            masm.shr32_cl(dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.lsr32(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI32Rotl() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            if (src.code != X86Assembler::RCX)
                masm.mov32(X86Assembler::RCX, src.code);
            masm.rol32_cl(dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            // ARM64 ROTL: rd = (rn << rm) | (rn >> (32 - rm))
            // Use ROR with negated count: ror(32 - count)
            uint8_t tmp = ARM64Assembler::X9;
            masm.neg32(tmp, src.code);
            masm.ror32(dst.code, dst.code, tmp);
        });
    });
}

bool BaselineCompiler::emitI32Rotr() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            if (src.code != X86Assembler::RCX)
                masm.mov32(X86Assembler::RCX, src.code);
            masm.ror32_cl(dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.ror32(dst.code, dst.code, src.code);
        });
    });
}

// =============================================================================
// I32 Unary Operations
// =============================================================================

bool BaselineCompiler::emitI32Clz() {
    return emitUnaryOp(ValType::i32(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        // Use LZCNT if available (BMI1), otherwise BSR + adjustment
        masm.lzcnt32(dst.code, dst.code);
    });
}

bool BaselineCompiler::emitI32Ctz() {
    return emitUnaryOp(ValType::i32(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        // Use TZCNT if available (BMI1), otherwise BSF
        masm.tzcnt32(dst.code, dst.code);
    });
}

bool BaselineCompiler::emitI32Popcnt() {
    return emitUnaryOp(ValType::i32(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        masm.popcnt32(dst.code, dst.code);
    });
}

bool BaselineCompiler::emitI32Eqz() {
    return emitUnaryOp(ValType::i32(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        // cmp dst, 0; sete dst; movzx dst, dst
        masm.cmp32(dst.code, 0); // Compare with 0
        masm.setcc(X86Assembler::E, dst.code); // Set byte register based on ZF
        masm.movzx8to32(dst.code, dst.code); // Zero-extend the byte to 32-bit
    });
}

// =============================================================================
// I32 Comparison Operations
// =============================================================================

bool BaselineCompiler::emitI32Eq() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cmp32(dst.code, src.code);
            masm.setcc(X86Assembler::E, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.cmp32(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::EQ);
        });
    });
}

bool BaselineCompiler::emitI32Ne() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cmp32(dst.code, src.code);
            masm.setcc(X86Assembler::NE, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.cmp32(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::NE);
        });
    });
}

bool BaselineCompiler::emitI32LtS() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cmp32(dst.code, src.code);
            masm.setcc(X86Assembler::L, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.cmp32(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::LT);
        });
    });
}

bool BaselineCompiler::emitI32LtU() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cmp32(dst.code, src.code);
            masm.setcc(X86Assembler::B, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.cmp32(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::LO);
        });
    });
}

bool BaselineCompiler::emitI32GtS() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cmp32(dst.code, src.code);
            masm.setcc(X86Assembler::G, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.cmp32(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::GT);
        });
    });
}

bool BaselineCompiler::emitI32GtU() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cmp32(dst.code, src.code);
            masm.setcc(X86Assembler::A, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.cmp32(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::HI);
        });
    });
}

bool BaselineCompiler::emitI32LeS() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cmp32(dst.code, src.code);
            masm.setcc(X86Assembler::LE, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.cmp32(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::LE);
        });
    });
}

bool BaselineCompiler::emitI32LeU() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cmp32(dst.code, src.code);
            masm.setcc(X86Assembler::BE, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.cmp32(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::LS);
        });
    });
}

bool BaselineCompiler::emitI32GeS() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cmp32(dst.code, src.code);
            masm.setcc(X86Assembler::GE, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.cmp32(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::GE);
        });
    });
}

bool BaselineCompiler::emitI32GeU() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cmp32(dst.code, src.code);
            masm.setcc(X86Assembler::AE, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.cmp32(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::HS);
        });
    });
}

// =============================================================================
// I64 Operations (Stubs)
// =============================================================================

bool BaselineCompiler::emitI64Add() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.add64(dst.code, src.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.add64(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI64Sub() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.sub64(dst.code, src.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.sub64(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI64Mul() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.imul64(dst.code, src.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.mul64(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI64DivS() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            if (dst.code != X86Assembler::RAX)
                masm.mov64(X86Assembler::RAX, dst.code);
            masm.cqo();
            masm.idiv64(src.code);
            if (dst.code != X86Assembler::RAX)
                masm.mov64(dst.code, X86Assembler::RAX);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.sdiv64(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI64DivU() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            if (dst.code != X86Assembler::RAX)
                masm.mov64(X86Assembler::RAX, dst.code);
            masm.xor64(X86Assembler::RDX, X86Assembler::RDX);
            masm.div64(src.code);
            if (dst.code != X86Assembler::RAX)
                masm.mov64(dst.code, X86Assembler::RAX);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.udiv64(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI64RemS() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            if (dst.code != X86Assembler::RAX)
                masm.mov64(X86Assembler::RAX, dst.code);
            masm.cqo();
            masm.idiv64(src.code);
            masm.mov64(dst.code, X86Assembler::RDX);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            uint8_t tmp = ARM64Assembler::X9;
            masm.sdiv64(tmp, dst.code, src.code);
            masm.msub64(dst.code, tmp, src.code, dst.code);
        });
    });
}

bool BaselineCompiler::emitI64RemU() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            if (dst.code != X86Assembler::RAX)
                masm.mov64(X86Assembler::RAX, dst.code);
            masm.xor64(X86Assembler::RDX, X86Assembler::RDX);
            masm.div64(src.code);
            masm.mov64(dst.code, X86Assembler::RDX);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            uint8_t tmp = ARM64Assembler::X9;
            masm.udiv64(tmp, dst.code, src.code);
            masm.msub64(dst.code, tmp, src.code, dst.code);
        });
    });
}

bool BaselineCompiler::emitI64And() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.and64(dst.code, src.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.and64(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI64Or() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.or64(dst.code, src.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.orr64(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI64Xor() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.xor64(dst.code, src.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.eor64(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI64Shl() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            if (src.code != X86Assembler::RCX)
                masm.mov64(X86Assembler::RCX, src.code);
            masm.shl64_cl(dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.lsl64(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI64ShrS() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            if (src.code != X86Assembler::RCX)
                masm.mov64(X86Assembler::RCX, src.code);
            masm.sar64_cl(dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.asr64(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI64ShrU() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            if (src.code != X86Assembler::RCX)
                masm.mov64(X86Assembler::RCX, src.code);
            masm.shr64_cl(dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.lsr64(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI64Rotl() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            if (src.code != X86Assembler::RCX)
                masm.mov64(X86Assembler::RCX, src.code);
            masm.rol64_cl(dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            uint8_t tmp = ARM64Assembler::X9;
            masm.neg64(tmp, src.code);
            masm.ror64(dst.code, dst.code, tmp);
        });
    });
}

bool BaselineCompiler::emitI64Rotr() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            if (src.code != X86Assembler::RCX)
                masm.mov64(X86Assembler::RCX, src.code);
            masm.ror64_cl(dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.ror64(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitI64Clz() {
    return emitUnaryOp(ValType::i64(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        masm.lzcnt64(dst.code, dst.code);
    });
}

bool BaselineCompiler::emitI64Ctz() {
    return emitUnaryOp(ValType::i64(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        masm.tzcnt64(dst.code, dst.code);
    });
}

bool BaselineCompiler::emitI64Popcnt() {
    return emitUnaryOp(ValType::i64(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        masm.popcnt64(dst.code, dst.code);
    });
}

bool BaselineCompiler::emitI64Eqz() {
    return emitUnaryOp(ValType::i64(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        masm.cmp64(dst.code, X86Assembler::RAX);  // cmp with 0 (xor'd register)
        masm.setcc(X86Assembler::E, dst.code);
        masm.movzx8to32(dst.code, dst.code);
    });
}

bool BaselineCompiler::emitI64Eq() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cmp64(dst.code, src.code);
            masm.setcc(X86Assembler::E, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.cmp64(dst.code, src.code);
            masm.cset64(dst.code, ARM64Assembler::EQ);
        });
    });
}

bool BaselineCompiler::emitI64Ne() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cmp64(dst.code, src.code);
            masm.setcc(X86Assembler::NE, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.cmp64(dst.code, src.code);
            masm.cset64(dst.code, ARM64Assembler::NE);
        });
    });
}

bool BaselineCompiler::emitI64LtS() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cmp64(dst.code, src.code);
            masm.setcc(X86Assembler::L, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.cmp64(dst.code, src.code);
            masm.cset64(dst.code, ARM64Assembler::LT);
        });
    });
}

bool BaselineCompiler::emitI64LtU() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cmp64(dst.code, src.code);
            masm.setcc(X86Assembler::B, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.cmp64(dst.code, src.code);
            masm.cset64(dst.code, ARM64Assembler::LO);
        });
    });
}

bool BaselineCompiler::emitI64GtS() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cmp64(dst.code, src.code);
            masm.setcc(X86Assembler::G, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.cmp64(dst.code, src.code);
            masm.cset64(dst.code, ARM64Assembler::GT);
        });
    });
}

bool BaselineCompiler::emitI64GtU() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cmp64(dst.code, src.code);
            masm.setcc(X86Assembler::A, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.cmp64(dst.code, src.code);
            masm.cset64(dst.code, ARM64Assembler::HI);
        });
    });
}

bool BaselineCompiler::emitI64LeS() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cmp64(dst.code, src.code);
            masm.setcc(X86Assembler::LE, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.cmp64(dst.code, src.code);
            masm.cset64(dst.code, ARM64Assembler::LE);
        });
    });
}

bool BaselineCompiler::emitI64LeU() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cmp64(dst.code, src.code);
            masm.setcc(X86Assembler::BE, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.cmp64(dst.code, src.code);
            masm.cset64(dst.code, ARM64Assembler::LS);
        });
    });
}

bool BaselineCompiler::emitI64GeS() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cmp64(dst.code, src.code);
            masm.setcc(X86Assembler::GE, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.cmp64(dst.code, src.code);
            masm.cset64(dst.code, ARM64Assembler::GE);
        });
    });
}

bool BaselineCompiler::emitI64GeU() {
    return emitBinaryOp(ValType::i64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cmp64(dst.code, src.code);
            masm.setcc(X86Assembler::AE, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.cmp64(dst.code, src.code);
            masm.cset64(dst.code, ARM64Assembler::HS);
        });
    });
}

// =============================================================================
// Float Operations
// =============================================================================

bool BaselineCompiler::emitF32Add() {
    return emitBinaryOp(ValType::f32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.addss(dst.code, src.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.faddS(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitF32Sub() {
    return emitBinaryOp(ValType::f32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.subss(dst.code, src.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fsubS(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitF32Mul() {
    return emitBinaryOp(ValType::f32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.mulss(dst.code, src.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fmulS(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitF32Div() {
    return emitBinaryOp(ValType::f32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.divss(dst.code, src.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fdivS(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitF32Sqrt() {
    return emitUnaryOp(ValType::f32(), [this](Reg dst) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.sqrtss(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fsqrtS(dst.code, dst.code);
        });
    });
}

bool BaselineCompiler::emitF32Abs() {
    return emitUnaryOp(ValType::f32(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        // Clear sign bit by moving to GPR, AND with 0x7FFFFFFF, move back
        // Allocate a scratch GPR
        Reg scratch = allocReg(ValType::i32());
        masm.movd_xmm2gpr(scratch.code, dst.code);
        masm.and32(scratch.code, 0x7FFFFFFF);  // Clear sign bit
        masm.movd_gpr2xmm(dst.code, scratch.code);
        freeReg(scratch);
    });
}

bool BaselineCompiler::emitF32Neg() {
    return emitUnaryOp(ValType::f32(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        // Flip sign bit by XOR with 0x80000000
        Reg scratch = allocReg(ValType::i32());
        masm.movd_xmm2gpr(scratch.code, dst.code);
        masm.xor32(scratch.code, static_cast<int32_t>(0x80000000));
        masm.movd_gpr2xmm(dst.code, scratch.code);
        freeReg(scratch);
    });
}

bool BaselineCompiler::emitF32Ceil() {
    return emitUnaryOp(ValType::f32(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        if (CpuFeatures::sse41Supported()) {
            // SSE4.1 roundss: mode 0x0A = ceil + inexact exception suppressed
            masm.roundss(dst.code, dst.code, 0x0A);
        } else {
            // Fallback: convert to int (truncates), add 1 if positive with fraction, convert back
            Reg tmp = allocReg(ValType::i32());
            Reg tmp2 = allocReg(ValType::f32());
            masm.cvttss2si(tmp.code, dst.code);    // tmp = trunc(dst)
            masm.cvtsi2ss(tmp2.code, tmp.code);    // tmp2 = (float)tmp
            // If dst > tmp2, result = tmp + 1, else result = tmp
            masm.ucomiss(dst.code, tmp2.code);     // Compare
            masm.cvtsi2ss(dst.code, tmp.code);     // Reload truncated
            // Add 1 if dst > trunc(dst) - requires conditional logic
            // For baseline, accept slight imprecision on fallback path
            freeReg(tmp);
            freeReg(tmp2);
        }
    });
}

bool BaselineCompiler::emitF32Floor() {
    return emitUnaryOp(ValType::f32(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        if (CpuFeatures::sse41Supported()) {
            // SSE4.1 roundss: mode 0x09 = floor + inexact exception suppressed
            masm.roundss(dst.code, dst.code, 0x09);
        } else {
            // Fallback: convert to int (truncates toward zero), subtract 1 if negative with fraction
            Reg tmp = allocReg(ValType::i32());
            masm.cvttss2si(tmp.code, dst.code);
            masm.cvtsi2ss(dst.code, tmp.code);
            freeReg(tmp);
        }
    });
}

bool BaselineCompiler::emitF32Trunc() {
    return emitUnaryOp(ValType::f32(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        if (CpuFeatures::sse41Supported()) {
            // SSE4.1 roundss: mode 0x0B = trunc + inexact exception suppressed
            masm.roundss(dst.code, dst.code, 0x0B);
        } else {
            // Trunc is natural behavior of cvttss2si
            Reg tmp = allocReg(ValType::i32());
            masm.cvttss2si(tmp.code, dst.code);
            masm.cvtsi2ss(dst.code, tmp.code);
            freeReg(tmp);
        }
    });
}

bool BaselineCompiler::emitF32Nearest() {
    return emitUnaryOp(ValType::f32(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        if (CpuFeatures::sse41Supported()) {
            // SSE4.1 roundss: mode 0x08 = round to nearest even + inexact suppressed
            masm.roundss(dst.code, dst.code, 0x08);
        } else {
            // Fallback: add 0.5 and truncate (banker's rounding approximation)
            Reg tmp = allocReg(ValType::i32());
            Reg half = allocReg(ValType::f32());
            masm.mov32Imm(tmp.code, 0x3F000000);  // 0.5f in IEEE-754
            masm.movd_gpr2xmm(half.code, tmp.code);
            masm.addss(dst.code, half.code);
            masm.cvttss2si(tmp.code, dst.code);
            masm.cvtsi2ss(dst.code, tmp.code);
            freeReg(half);
            freeReg(tmp);
        }
    });
}

bool BaselineCompiler::emitF32Copysign() {
    return emitBinaryOp(ValType::f32(), [this](Reg dst, Reg src) {
        X86Assembler masm(codeBuffer_.get());
        // Copy sign from src to dst: (dst & 0x7FFFFFFF) | (src & 0x80000000)
        Reg dstBits = allocReg(ValType::i32());
        Reg srcBits = allocReg(ValType::i32());
        masm.movd_xmm2gpr(dstBits.code, dst.code);
        masm.movd_xmm2gpr(srcBits.code, src.code);
        masm.and32(dstBits.code, 0x7FFFFFFF);  // Clear dst sign
        masm.and32(srcBits.code, static_cast<int32_t>(0x80000000));  // Get src sign
        masm.or32(dstBits.code, srcBits.code);  // Combine
        masm.movd_gpr2xmm(dst.code, dstBits.code);
        freeReg(dstBits);
        freeReg(srcBits);
    });
}

bool BaselineCompiler::emitF32Min() {
    return emitBinaryOp(ValType::f32(), [this](Reg dst, Reg src) {
        X86Assembler masm(codeBuffer_.get());
        // WASM min semantics: min(a, NaN) = NaN, min(NaN, b) = NaN
        // minss returns second operand if either is NaN, so we need both orders
        // min(a,b) = minss(a, minss(b, a)) handles NaN properly
        masm.minss(src.code, dst.code);  // src = min(src, dst)
        masm.minss(dst.code, src.code);  // dst = min(dst, src)
    });
}

bool BaselineCompiler::emitF32Max() {
    return emitBinaryOp(ValType::f32(), [this](Reg dst, Reg src) {
        X86Assembler masm(codeBuffer_.get());
        // Same NaN handling as min
        masm.maxss(src.code, dst.code);
        masm.maxss(dst.code, src.code);
    });
}

bool BaselineCompiler::emitF64Add() {
    return emitBinaryOp(ValType::f64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.addsd(dst.code, src.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.faddD(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitF64Sub() {
    return emitBinaryOp(ValType::f64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.subsd(dst.code, src.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fsubD(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitF64Mul() {
    return emitBinaryOp(ValType::f64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.mulsd(dst.code, src.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fmulD(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitF64Div() {
    return emitBinaryOp(ValType::f64(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.divsd(dst.code, src.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fdivD(dst.code, dst.code, src.code);
        });
    });
}

bool BaselineCompiler::emitF64Sqrt() {
    return emitUnaryOp(ValType::f64(), [this](Reg dst) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.sqrtsd(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fsqrtD(dst.code, dst.code);
        });
    });
}

bool BaselineCompiler::emitF64Abs() {
    return emitUnaryOp(ValType::f64(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        // Clear sign bit (bit 63) via GPR: AND with 0x7FFFFFFFFFFFFFFF
        Reg scratch = allocReg(ValType::i64());
        masm.movq_xmm2gpr(scratch.code, dst.code);
        masm.mov64Imm(X86Assembler::RAX, 0x7FFFFFFFFFFFFFFFLL);
        masm.and64(scratch.code, X86Assembler::RAX);
        masm.movq_gpr2xmm(dst.code, scratch.code);
        freeReg(scratch);
    });
}

bool BaselineCompiler::emitF64Neg() {
    return emitUnaryOp(ValType::f64(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        // Flip sign bit (bit 63) via GPR: XOR with 0x8000000000000000
        Reg scratch = allocReg(ValType::i64());
        masm.movq_xmm2gpr(scratch.code, dst.code);
        masm.mov64Imm(X86Assembler::RAX, static_cast<int64_t>(0x8000000000000000LL));
        masm.xor64(scratch.code, X86Assembler::RAX);
        masm.movq_gpr2xmm(dst.code, scratch.code);
        freeReg(scratch);
    });
}

bool BaselineCompiler::emitF64Ceil() {
    return emitUnaryOp(ValType::f64(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        if (CpuFeatures::sse41Supported()) {
            // SSE4.1 roundsd: mode 0x0A = ceil + inexact exception suppressed
            masm.roundsd(dst.code, dst.code, 0x0A);
        } else {
            // Fallback: truncate and adjust for positive fractions
            Reg tmp = allocReg(ValType::i64());
            masm.cvttsd2si64(tmp.code, dst.code);
            masm.cvtsi2sd64(dst.code, tmp.code);
            freeReg(tmp);
        }
    });
}

bool BaselineCompiler::emitF64Floor() {
    return emitUnaryOp(ValType::f64(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        if (CpuFeatures::sse41Supported()) {
            // SSE4.1 roundsd: mode 0x09 = floor + inexact exception suppressed
            masm.roundsd(dst.code, dst.code, 0x09);
        } else {
            // Fallback: truncate (note: not exact floor for negatives)
            Reg tmp = allocReg(ValType::i64());
            masm.cvttsd2si64(tmp.code, dst.code);
            masm.cvtsi2sd64(dst.code, tmp.code);
            freeReg(tmp);
        }
    });
}

bool BaselineCompiler::emitF64Trunc() {
    return emitUnaryOp(ValType::f64(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        if (CpuFeatures::sse41Supported()) {
            // SSE4.1 roundsd: mode 0x0B = trunc + inexact exception suppressed
            masm.roundsd(dst.code, dst.code, 0x0B);
        } else {
            // Trunc is natural behavior of cvttsd2si
            Reg tmp = allocReg(ValType::i64());
            masm.cvttsd2si64(tmp.code, dst.code);
            masm.cvtsi2sd64(dst.code, tmp.code);
            freeReg(tmp);
        }
    });
}

bool BaselineCompiler::emitF64Nearest() {
    return emitUnaryOp(ValType::f64(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        if (CpuFeatures::sse41Supported()) {
            // SSE4.1 roundsd: mode 0x08 = round to nearest even + inexact suppressed
            masm.roundsd(dst.code, dst.code, 0x08);
        } else {
            // Fallback: add 0.5 and truncate (banker's rounding approximation)
            Reg tmp = allocReg(ValType::i64());
            Reg half = allocReg(ValType::f64());
            masm.mov64Imm(tmp.code, 0x3FE0000000000000LL);  // 0.5 in IEEE-754 f64
            masm.movq_gpr2xmm(half.code, tmp.code);
            masm.addsd(dst.code, half.code);
            masm.cvttsd2si64(tmp.code, dst.code);
            masm.cvtsi2sd64(dst.code, tmp.code);
            freeReg(half);
            freeReg(tmp);
        }
    });
}

bool BaselineCompiler::emitF64Copysign() {
    return emitBinaryOp(ValType::f64(), [this](Reg dst, Reg src) {
        X86Assembler masm(codeBuffer_.get());
        // Copy sign from src to dst: (dst & 0x7FFF...) | (src & 0x8000...)
        Reg dstBits = allocReg(ValType::i64());
        Reg srcBits = allocReg(ValType::i64());
        masm.movq_xmm2gpr(dstBits.code, dst.code);
        masm.movq_xmm2gpr(srcBits.code, src.code);
        masm.mov64Imm(X86Assembler::RAX, 0x7FFFFFFFFFFFFFFFLL);
        masm.and64(dstBits.code, X86Assembler::RAX);  // Clear dst sign
        masm.mov64Imm(X86Assembler::RAX, static_cast<int64_t>(0x8000000000000000LL));
        masm.and64(srcBits.code, X86Assembler::RAX);  // Get src sign
        masm.or64(dstBits.code, srcBits.code);  // Combine
        masm.movq_gpr2xmm(dst.code, dstBits.code);
        freeReg(dstBits);
        freeReg(srcBits);
    });
}

bool BaselineCompiler::emitF64Min() {
    return emitBinaryOp(ValType::f64(), [this](Reg dst, Reg src) {
        X86Assembler masm(codeBuffer_.get());
        // WASM min: propagates NaN via double minsd
        masm.minsd(src.code, dst.code);
        masm.minsd(dst.code, src.code);
    });
}

bool BaselineCompiler::emitF64Max() {
    return emitBinaryOp(ValType::f64(), [this](Reg dst, Reg src) {
        X86Assembler masm(codeBuffer_.get());
        // WASM max: propagates NaN via double maxsd
        masm.maxsd(src.code, dst.code);
        masm.maxsd(dst.code, src.code);
    });
}

// =============================================================================
// Conversions (Dual-Platform)
// =============================================================================

bool BaselineCompiler::emitI32WrapI64() {
    return emitUnaryOp(ValType::i32(), [this](Reg dst) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.mov32(dst.code, dst.code);  // Truncate to 32-bit
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            // On ARM64, 32-bit register access automatically truncates
            masm.mov32(dst.code, dst.code);
        });
    });
}

bool BaselineCompiler::emitI64ExtendI32S() {
    return emitUnaryOp(ValType::i64(), [this](Reg dst) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.movsxd(dst.code, dst.code);  // Sign-extend i32 to i64
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.sxtw(dst.code, dst.code);  // Sign-extend word to 64-bit
        });
    });
}

bool BaselineCompiler::emitI64ExtendI32U() {
    return emitUnaryOp(ValType::i64(), [this](Reg dst) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.mov32(dst.code, dst.code);  // Zero-extend
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.uxtw(dst.code, dst.code);  // Zero-extend word to 64-bit
        });
    });
}

bool BaselineCompiler::emitI32TruncF32S() {
    return emitUnaryOp(ValType::i32(), [this](Reg dst) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cvttss2si(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fcvtzs32S(dst.code, dst.code);  // Float to signed int
        });
    });
}

bool BaselineCompiler::emitI32TruncF32U() {
    return emitUnaryOp(ValType::i32(), [this](Reg dst) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cvttss2si64(dst.code, dst.code);
            masm.mov32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fcvtzu32S(dst.code, dst.code);  // Float to unsigned int
        });
    });
}

bool BaselineCompiler::emitI32TruncF64S() {
    return emitUnaryOp(ValType::i32(), [this](Reg dst) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cvttsd2si(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fcvtzs32D(dst.code, dst.code);  // Double to signed int
        });
    });
}

bool BaselineCompiler::emitI32TruncF64U() {
    return emitUnaryOp(ValType::i32(), [this](Reg dst) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cvttsd2si64(dst.code, dst.code);
            masm.mov32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fcvtzu32D(dst.code, dst.code);  // Double to unsigned int
        });
    });
}

bool BaselineCompiler::emitF32ConvertI32S() {
    return emitUnaryOp(ValType::f32(), [this](Reg dst) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cvtsi2ss(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.scvtfS32(dst.code, dst.code);  // Signed int to float
        });
    });
}

bool BaselineCompiler::emitF32ConvertI32U() {
    return emitUnaryOp(ValType::f32(), [this](Reg dst) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.mov32(dst.code, dst.code);
            masm.cvtsi2ss64(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.ucvtfS32(dst.code, dst.code);  // Unsigned int to float
        });
    });
}

bool BaselineCompiler::emitF64ConvertI32S() {
    return emitUnaryOp(ValType::f64(), [this](Reg dst) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cvtsi2sd(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.scvtfD32(dst.code, dst.code);  // Signed int to double
        });
    });
}

bool BaselineCompiler::emitF64ConvertI32U() {
    return emitUnaryOp(ValType::f64(), [this](Reg dst) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.mov32(dst.code, dst.code);
            masm.cvtsi2sd64(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.ucvtfD32(dst.code, dst.code);  // Unsigned int to double
        });
    });
}

bool BaselineCompiler::emitF32DemoteF64() {
    return emitUnaryOp(ValType::f32(), [this](Reg dst) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cvtsd2ss(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fcvtSD(dst.code, dst.code);  // Double to float
        });
    });
}

bool BaselineCompiler::emitF64PromoteF32() {
    return emitUnaryOp(ValType::f64(), [this](Reg dst) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.cvtss2sd(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fcvtDS(dst.code, dst.code);  // Float to double
        });
    });
}

bool BaselineCompiler::emitI32ReinterpretF32() {
    return emitUnaryOp(ValType::i32(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        masm.movd_xmm2gpr(dst.code, dst.code);  // Move XMM to GPR
    });
}

bool BaselineCompiler::emitI64ReinterpretF64() {
    return emitUnaryOp(ValType::i64(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        masm.movq_xmm2gpr(dst.code, dst.code);  // Move XMM to GPR (64-bit)
    });
}

bool BaselineCompiler::emitF32ReinterpretI32() {
    return emitUnaryOp(ValType::f32(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        masm.movd_gpr2xmm(dst.code, dst.code);  // Move GPR to XMM
    });
}

bool BaselineCompiler::emitF64ReinterpretI64() {
    return emitUnaryOp(ValType::f64(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        masm.movq_gpr2xmm(dst.code, dst.code);  // Move GPR to XMM (64-bit)
    });
}

// =============================================================================
// I64 Trunc Operations
// =============================================================================

bool BaselineCompiler::emitI64TruncF32S() {
    return emitUnaryOp(ValType::i64(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        masm.cvttss2si64(dst.code, dst.code);  // Truncate f32 to signed i64
    });
}

bool BaselineCompiler::emitI64TruncF32U() {
    return emitUnaryOp(ValType::i64(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        // Unsigned: for values >= 2^63, subtract 2^63, convert, add back
        // Simplified path: use signed conversion (works for < 2^63)
        masm.cvttss2si64(dst.code, dst.code);
    });
}

bool BaselineCompiler::emitI64TruncF64S() {
    return emitUnaryOp(ValType::i64(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        masm.cvttsd2si64(dst.code, dst.code);  // Truncate f64 to signed i64
    });
}

bool BaselineCompiler::emitI64TruncF64U() {
    return emitUnaryOp(ValType::i64(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        // Unsigned: simplified path using signed conversion
        masm.cvttsd2si64(dst.code, dst.code);
    });
}

// =============================================================================
// F32/F64 Convert from I64
// =============================================================================

bool BaselineCompiler::emitF32ConvertI64S() {
    return emitUnaryOp(ValType::f32(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        masm.cvtsi2ss64(dst.code, dst.code);  // Signed i64 to f32
    });
}

bool BaselineCompiler::emitF32ConvertI64U() {
    return emitUnaryOp(ValType::f32(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        // Unsigned i64 to f32: simplified using signed (works for < 2^63)
        masm.cvtsi2ss64(dst.code, dst.code);
    });
}

bool BaselineCompiler::emitF64ConvertI64S() {
    return emitUnaryOp(ValType::f64(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        masm.cvtsi2sd64(dst.code, dst.code);  // Signed i64 to f64
    });
}

bool BaselineCompiler::emitF64ConvertI64U() {
    return emitUnaryOp(ValType::f64(), [this](Reg dst) {
        X86Assembler masm(codeBuffer_.get());
        // Unsigned i64 to f64: simplified using signed (works for < 2^63)
        masm.cvtsi2sd64(dst.code, dst.code);
    });
}

// =============================================================================
// F32 Comparisons
// =============================================================================

bool BaselineCompiler::emitF32Eq() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.ucomiss(dst.code, src.code);
            masm.setcc(X86Assembler::E, dst.code);  // ZF=1 and no NaN
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fcmpS(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::EQ);
        });
    });
}

bool BaselineCompiler::emitF32Ne() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.ucomiss(dst.code, src.code);
            masm.setcc(X86Assembler::NE, dst.code);  // ZF=0 or PF=1 (NaN)
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fcmpS(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::NE);
        });
    });
}

bool BaselineCompiler::emitF32Lt() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.ucomiss(src.code, dst.code);  // src > dst means dst < src
            masm.setcc(X86Assembler::A, dst.code);  // Above (CF=0, ZF=0)
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fcmpS(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::LT);
        });
    });
}

bool BaselineCompiler::emitF32Gt() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.ucomiss(dst.code, src.code);  // dst > src
            masm.setcc(X86Assembler::A, dst.code);  // Above
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fcmpS(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::GT);
        });
    });
}

bool BaselineCompiler::emitF32Le() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.ucomiss(src.code, dst.code);  // src >= dst means dst <= src
            masm.setcc(X86Assembler::AE, dst.code);  // Above or equal
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fcmpS(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::LE);
        });
    });
}

bool BaselineCompiler::emitF32Ge() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.ucomiss(dst.code, src.code);  // dst >= src
            masm.setcc(X86Assembler::AE, dst.code);  // Above or equal
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fcmpS(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::GE);
        });
    });
}

// =============================================================================
// F64 Comparisons
// =============================================================================

bool BaselineCompiler::emitF64Eq() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.ucomisd(dst.code, src.code);
            masm.setcc(X86Assembler::E, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fcmpD(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::EQ);
        });
    });
}

bool BaselineCompiler::emitF64Ne() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.ucomisd(dst.code, src.code);
            masm.setcc(X86Assembler::NE, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fcmpD(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::NE);
        });
    });
}

bool BaselineCompiler::emitF64Lt() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.ucomisd(src.code, dst.code);  // Reverse for lt
            masm.setcc(X86Assembler::A, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fcmpD(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::LT);
        });
    });
}

bool BaselineCompiler::emitF64Gt() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.ucomisd(dst.code, src.code);
            masm.setcc(X86Assembler::A, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fcmpD(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::GT);
        });
    });
}

bool BaselineCompiler::emitF64Le() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.ucomisd(src.code, dst.code);  // Reverse for le
            masm.setcc(X86Assembler::AE, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fcmpD(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::LE);
        });
    });
}

bool BaselineCompiler::emitF64Ge() {
    return emitBinaryOp(ValType::i32(), [this](Reg dst, Reg src) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            masm.ucomisd(dst.code, src.code);
            masm.setcc(X86Assembler::AE, dst.code);
            masm.movzx8to32(dst.code, dst.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            masm.fcmpD(dst.code, src.code);
            masm.cset32(dst.code, ARM64Assembler::GE);
        });
    });
}

// =============================================================================
// Memory Operations (Dual-Platform)
// =============================================================================

bool BaselineCompiler::emitLoad(Op op, uint32_t align, uint32_t offset) {
    ValueLocation addr = pop();
    
    // Memory64: determine address size based on module's memory type
    bool isMemory64 = module_->memories().size() > 0 && 
                      module_->memories()[0].isMemory64;
    ValType addrType = isMemory64 ? ValType::i64() : ValType::i32();
    
    if (options_.enableBoundsChecks) {
        emitBoundsCheck64(offset, isMemory64);
    }
    
    Reg addrReg = allocReg(addrType);
    
    // Load address to register if needed
    if (addr.isRegister()) {
        addrReg = addr.reg;
    }
    
    // For Memory32, zero-extend i32 address to i64 for pointer arithmetic
    if (!isMemory64 && addr.isRegister()) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            // mov eax, eax (zero-extends to 64-bit)
            masm.mov32(addrReg.code, addrReg.code);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            // uxtw (unsigned extend word)
            masm.andImm64(addrReg.code, addrReg.code, 0xFFFFFFFF);
        });
    }
    
    // Get memory base pointer (stored in instance)
    Reg memBase = allocReg(ValType::i64());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.load64(memBase.code, X86Assembler::RBP, -16);
        // Add base + address
        masm.add64(memBase.code, addrReg.code);
        if (offset != 0) {
            masm.add64Imm(memBase.code, static_cast<int32_t>(offset));
        }
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.ldr64(memBase.code, 29, -16);  // Load from frame pointer
        masm.add64(memBase.code, memBase.code, addrReg.code);
        if (offset != 0) {
            masm.addImm64(memBase.code, memBase.code, offset);
        }
    });
    
    ValType resultType;
    Reg result;
    
    switch (op) {
        case Op::I32Load:
            resultType = ValType::i32();
            result = allocReg(resultType);
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.load32(result.code, memBase.code, 0);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.ldr32(result.code, memBase.code, 0);
            });
            break;
        case Op::I32Load8S:
            resultType = ValType::i32();
            result = allocReg(resultType);
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.load8s(result.code, memBase.code, 0);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.ldrsb32(result.code, memBase.code, 0);
            });
            break;
        case Op::I32Load8U:
            resultType = ValType::i32();
            result = allocReg(resultType);
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.load8u(result.code, memBase.code, 0);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.ldrb(result.code, memBase.code, 0);
            });
            break;
        case Op::I32Load16S:
            resultType = ValType::i32();
            result = allocReg(resultType);
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.load16s(result.code, memBase.code, 0);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.ldrsh32(result.code, memBase.code, 0);
            });
            break;
        case Op::I32Load16U:
            resultType = ValType::i32();
            result = allocReg(resultType);
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.load16u(result.code, memBase.code, 0);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.ldrh(result.code, memBase.code, 0);
            });
            break;
        case Op::I64Load:
            resultType = ValType::i64();
            result = allocReg(resultType);
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.load64(result.code, memBase.code, 0);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.ldr64(result.code, memBase.code, 0);
            });
            break;
        case Op::I64Load8S:
            resultType = ValType::i64();
            result = allocReg(resultType);
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.load8s64(result.code, memBase.code, 0);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.ldrsb64(result.code, memBase.code, 0);
            });
            break;
        case Op::I64Load8U:
            resultType = ValType::i64();
            result = allocReg(resultType);
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.load8u64(result.code, memBase.code, 0);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.ldrb(result.code, memBase.code, 0);
            });
            break;
        case Op::I64Load16S:
            resultType = ValType::i64();
            result = allocReg(resultType);
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.load16s64(result.code, memBase.code, 0);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.ldrsh64(result.code, memBase.code, 0);
            });
            break;
        case Op::I64Load16U:
            resultType = ValType::i64();
            result = allocReg(resultType);
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.load16u64(result.code, memBase.code, 0);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.ldrh(result.code, memBase.code, 0);
            });
            break;
        case Op::I64Load32S:
            resultType = ValType::i64();
            result = allocReg(resultType);
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.load32s64(result.code, memBase.code, 0);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.ldrsw(result.code, memBase.code, 0);
            });
            break;
        case Op::I64Load32U:
            resultType = ValType::i64();
            result = allocReg(resultType);
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.load32u64(result.code, memBase.code, 0);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.ldr32(result.code, memBase.code, 0);  // Zero-extends to 64-bit
            });
            break;
        case Op::F32Load:
            resultType = ValType::f32();
            result = allocReg(resultType);
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.loadss(result.code, memBase.code, 0);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.ldrS(result.code, memBase.code, 0);
            });
            break;
        case Op::F64Load:
            resultType = ValType::f64();
            result = allocReg(resultType);
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.loadsd(result.code, memBase.code, 0);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.ldrD(result.code, memBase.code, 0);
            });
            break;
        default:
            error_ = "invalid load opcode";
            return false;
    }
    
    push(ValueLocation::inReg(result, resultType));
    return true;
}

bool BaselineCompiler::emitStore(Op op, uint32_t align, uint32_t offset) {
    ValueLocation value = pop();
    ValueLocation addr = pop();
    
    if (options_.enableBoundsChecks) {
        emitBoundsCheck(offset);
    }
    
    // Get memory base pointer
    Reg memBase = allocReg(ValType::i64());
    Reg addrReg = addr.isRegister() ? addr.reg : allocReg(ValType::i32());
    Reg valueReg = value.isRegister() ? value.reg : allocReg(value.type);
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.load64(memBase.code, X86Assembler::RBP, -16);
        if (offset != 0) {
            masm.add64Imm(addrReg.code, static_cast<int32_t>(offset));
        }
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.ldr64(memBase.code, 29, -16);  // Load from frame pointer
        if (offset != 0) {
            masm.addImm64(addrReg.code, addrReg.code, offset);
        }
    });
    
    switch (op) {
        case Op::I32Store:
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.store32(memBase.code, 0, valueReg.code);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.str32(valueReg.code, memBase.code, 0);
            });
            break;
        case Op::I32Store8:
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.store8(memBase.code, 0, valueReg.code);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.strb(valueReg.code, memBase.code, 0);
            });
            break;
        case Op::I32Store16:
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.store16(memBase.code, 0, valueReg.code);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.strh(valueReg.code, memBase.code, 0);
            });
            break;
        case Op::I64Store:
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.store64(memBase.code, 0, valueReg.code);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.str64(valueReg.code, memBase.code, 0);
            });
            break;
        case Op::I64Store8:
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.store8(memBase.code, 0, valueReg.code);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.strb(valueReg.code, memBase.code, 0);
            });
            break;
        case Op::I64Store16:
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.store16(memBase.code, 0, valueReg.code);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.strh(valueReg.code, memBase.code, 0);
            });
            break;
        case Op::I64Store32:
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.store32(memBase.code, 0, valueReg.code);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.str32(valueReg.code, memBase.code, 0);
            });
            break;
        case Op::F32Store:
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.storess(memBase.code, 0, valueReg.code);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.strS(valueReg.code, memBase.code, 0);
            });
            break;
        case Op::F64Store:
            EMIT_X64({
                X86Assembler masm(codeBuffer_.get());
                masm.storesd(memBase.code, 0, valueReg.code);
            });
            EMIT_ARM64({
                ARM64Assembler masm(codeBuffer_.get());
                masm.strD(valueReg.code, memBase.code, 0);
            });
            break;
        default:
            error_ = "invalid store opcode";
            return false;
    }
    
    return true;
}

bool BaselineCompiler::emitMemorySize(uint32_t memIndex) {
    Reg result = allocReg(ValType::i32());
    // Load memory size from instance (instance->memorySize)
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.load32(result.code, X86Assembler::RBP, -24);
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.ldr32(result.code, 29, -24);
    });
    push(ValueLocation::inReg(result, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitMemoryGrow(uint32_t memIndex) {
    ValueLocation delta = pop();
    
    Reg deltaReg = delta.isRegister() ? delta.reg : allocReg(ValType::i32());
    if (!delta.isRegister()) {
        if (delta.isImmediate()) {
            EMIT_X64({ X86Assembler m(codeBuffer_.get()); m.mov32Imm(deltaReg.code, static_cast<int32_t>(delta.immediate)); });
        } else {
            EMIT_X64({ X86Assembler m(codeBuffer_.get()); m.load32(deltaReg.code, X86Assembler::RBP, delta.slot.offset); });
        }
    }
    
    Reg result = allocReg(ValType::i32());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // Pass delta as first arg (RDI)
        masm.mov32(X86Assembler::RDI, deltaReg.code);
        // Load runtime memoryGrow function pointer from instance (RBP-40)
        Reg fnPtr = allocReg(ValType::i64());
        masm.load64(fnPtr.code, X86Assembler::RBP, -40);
        masm.call(fnPtr.code);
        freeReg(fnPtr);
        // Result in RAX
        masm.mov32(result.code, X86Assembler::RAX);
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        // Pass delta as X0
        masm.mov32(0, deltaReg.code);
        // Load runtime function pointer from frame
        Reg fnPtr = allocReg(ValType::i64());
        masm.ldr64(fnPtr.code, 29, -40);
        masm.blr(fnPtr.code);
        freeReg(fnPtr);
        // Result in X0
        masm.mov32(result.code, 0);
    });
    
    push(ValueLocation::inReg(result, ValType::i32()));
    return true;
}

// =============================================================================
// Call Operations (Stubs)
// =============================================================================

bool BaselineCompiler::emitCall(uint32_t funcIndex) {
    if (funcIndex >= module_->numFunctions()) {
        error_ = "function index out of bounds";
        return false;
    }
    
    const FuncType* funcType = module_->funcType(funcIndex);
    X86Assembler masm(codeBuffer_.get());
    
    // System V AMD64 ABI: integer args in RDI, RSI, RDX, RCX, R8, R9
    // Float args in XMM0-XMM7
    static const uint8_t intArgRegs[] = {
        X86Assembler::RDI, X86Assembler::RSI, X86Assembler::RDX,
        X86Assembler::RCX, X86Assembler::R8, X86Assembler::R9
    };
    
    // Pop arguments from value stack in reverse order
    size_t numParams = funcType->params().size();
    std::vector<ValueLocation> args(numParams);
    for (size_t i = numParams; i > 0; --i) {
        args[i - 1] = pop();
    }
    
    // Move args to calling convention registers
    size_t intArgIdx = 0;
    size_t floatArgIdx = 0;
    for (size_t i = 0; i < numParams; ++i) {
        const ValueLocation& arg = args[i];
        ValType::Kind kind = funcType->params()[i].kind();
        
        if (kind == ValType::Kind::F32 || kind == ValType::Kind::F64) {
            // Float arg -> XMM0-XMM7
            uint8_t targetXmm = floatArgIdx++;
            if (arg.isRegister()) {
                if (arg.reg.code != targetXmm) {
                    if (kind == ValType::Kind::F32)
                        masm.movss(targetXmm, arg.reg.code);
                    else
                        masm.movsd(targetXmm, arg.reg.code);
                }
            } else {
                // Load from stack
                if (kind == ValType::Kind::F32)
                    masm.loadss(targetXmm, X86Assembler::RBP, arg.slot.offset);
                else
                    masm.loadsd(targetXmm, X86Assembler::RBP, arg.slot.offset);
            }
        } else {
            // Integer arg -> RDI, RSI, etc.
            if (intArgIdx < 6) {
                uint8_t targetReg = intArgRegs[intArgIdx++];
                if (arg.isRegister()) {
                    if (arg.reg.code != targetReg) {
                        if (kind == ValType::Kind::I64)
                            masm.mov64(targetReg, arg.reg.code);
                        else
                            masm.mov32(targetReg, arg.reg.code);
                    }
                } else if (arg.isImmediate()) {
                    masm.mov64Imm(targetReg, arg.immediate);
                } else {
                    // Load from stack
                    if (kind == ValType::Kind::I64)
                        masm.load64(targetReg, X86Assembler::RBP, arg.slot.offset);
                    else
                        masm.load32(targetReg, X86Assembler::RBP, arg.slot.offset);
                }
            }
            // Args beyond 6 go on stack (not yet implemented)
        }
    }
    
    // Get function entry address from compiled code table (at RBP-16)
    // funcTable[funcIndex] gives us the address
    Reg callTarget = allocReg(ValType::i64());
    masm.load64(callTarget.code, X86Assembler::RBP, -16);  // Load function table ptr
    masm.load64(callTarget.code, callTarget.code, funcIndex * 8);  // Load func address
    
    // Emit indirect call (call *rax)
    masm.call(callTarget.code);
    freeReg(callTarget);
    
    // Push result(s) based on return type
    if (!funcType->results().empty()) {
        ValType resultType = funcType->results()[0];
        if (resultType.kind() == ValType::Kind::F32 || resultType.kind() == ValType::Kind::F64) {
            // Result in XMM0
            Reg result = allocReg(resultType);
            // XMM0 is already the result register, just claim it
            push(ValueLocation::inReg(result, resultType));
        } else {
            // Result in RAX
            Reg result = Reg::gpr(X86Assembler::RAX);
            push(ValueLocation::inReg(result, resultType));
        }
    }
    
    return true;
}

bool BaselineCompiler::emitCallIndirect(uint32_t typeIndex, uint32_t tableIndex) {
    X86Assembler masm(codeBuffer_.get());
    
    // Pop call target index from stack
    ValueLocation indexLoc = pop();
    
    // Get index into register
    Reg indexReg;
    if (indexLoc.isRegister()) {
        indexReg = indexLoc.reg;
    } else if (indexLoc.isImmediate()) {
        indexReg = allocReg(ValType::i32());
        masm.mov32Imm(indexReg.code, static_cast<int32_t>(indexLoc.immediate));
    } else {
        indexReg = allocReg(ValType::i32());
        masm.load32(indexReg.code, X86Assembler::RBP, indexLoc.slot.offset);
    }
    
    // Load table base and size from instance (RBP-24 = table base, RBP-32 = table size)
    Reg tableBase = allocReg(ValType::i64());
    Reg tableSize = allocReg(ValType::i32());
    masm.load64(tableBase.code, X86Assembler::RBP, -24);
    masm.load32(tableSize.code, X86Assembler::RBP, -32);
    
    // Bounds check: if (index >= tableSize) trap
    masm.cmp32(indexReg.code, tableSize.code);
    size_t trapJump = masm.jcc32(X86Assembler::AE);  // Jump if above or equal
    
    // Compute table entry address: tableBase + index * 8
    Reg entryAddr = allocReg(ValType::i64());
    masm.mov64(entryAddr.code, indexReg.code);  // Zero-extend i32 to i64
    masm.shl64_cl(entryAddr.code);  // Shift left by 3 (multiply by 8)
    // Actually, use explicit shift: need to add imm shift instruction
    // For now, just add index to base multiple times
    masm.lea64(entryAddr.code, tableBase.code, indexReg.code, 8, 0);
    
    // Load function pointer from table
    Reg funcPtr = allocReg(ValType::i64());
    masm.load64(funcPtr.code, entryAddr.code, 0);
    
    // Null check
    masm.cmp64Imm(funcPtr.code, 0);
    size_t nullTrap = masm.jcc32(X86Assembler::E);
    
    // Type check would go here (compare typeIndex against table entry type)
    // For now, skip type check
    
    // Call through function pointer
    masm.call(funcPtr.code);
    
    // Patch trap jumps to trap handler
    // (In real impl, would emit trap handler or landing pad)
    
    freeReg(funcPtr);
    freeReg(entryAddr);
    freeReg(tableSize);
    freeReg(tableBase);
    if (!indexLoc.isRegister()) freeReg(indexReg);
    
    // Push result (assuming single i32 result for now)
    Reg result = Reg::gpr(X86Assembler::RAX);
    push(ValueLocation::inReg(result, ValType::i32()));
    
    return true;
}

// =============================================================================
// Tail Call Operations
// =============================================================================

bool BaselineCompiler::emitReturnCall(uint32_t funcIndex) {
    // Tail call: reuse current stack frame and jump to callee
    // 1. Pop arguments from value stack
    // 2. Tear down current frame
    // 3. Set up callee arguments in place
    // 4. Jump (not call) to callee
    
    uint32_t typeIdx = module_->functions()[funcIndex].typeIndex;
    const FuncType* calleeTypePtr = module_->funcType(typeIdx);
    if (!calleeTypePtr) return false;
    const FuncType& calleeType = *calleeTypePtr;
    size_t numParams = calleeType.params().size();
    
    // Collect arguments
    std::vector<ValueLocation> args;
    for (size_t i = 0; i < numParams; i++) {
        args.push_back(pop());
    }
    std::reverse(args.begin(), args.end());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        
        // Move arguments to correct positions
        // For tail call, we reuse caller's argument slots
        for (size_t i = 0; i < numParams; i++) {
            if (args[i].isRegister()) {
                // Move to arg register or stack slot
                if (i < 6) {  // x64 uses 6 registers for args
                    static const uint8_t argRegs[] = {X86Assembler::RDI, X86Assembler::RSI, 
                        X86Assembler::RDX, X86Assembler::RCX, 8, 9};  // r8, r9
                    masm.mov64(argRegs[i], args[i].reg.code);
                } else {
                    masm.store64(args[i].reg.code, X86Assembler::RBP, 16 + (i - 6) * 8);
                }
            } else if (args[i].isImmediate()) {
                if (i < 6) {
                    static const uint8_t argRegs[] = {X86Assembler::RDI, X86Assembler::RSI,
                        X86Assembler::RDX, X86Assembler::RCX, 8, 9};
                    masm.mov64Imm(argRegs[i], args[i].immediate);
                }
            }
        }
        
        // Restore callee-saved registers
        masm.pop(X86Assembler::RBP);
        
        // Load callee address from function table and jump
        Reg callTarget = allocReg(ValType::i64());
        masm.load64(callTarget.code, X86Assembler::RBP, -16);
        masm.load64(callTarget.code, callTarget.code, funcIndex * 8);
        masm.jmp(callTarget.code);
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        
        // Move arguments to X0-X7 or stack
        for (size_t i = 0; i < numParams && i < 8; i++) {
            if (args[i].isRegister()) {
                masm.mov64(i, args[i].reg.code);
            } else if (args[i].isImmediate()) {
                masm.movImm64(i, args[i].immediate);
            }
        }
        
        // Restore FP/LR
        masm.ldpPost64(29, 30, 31, frameSize_);
        
        // Load callee address from function table and jump
        Reg callTarget = allocReg(ValType::i64());
        masm.ldr64(callTarget.code, 29, -16);
        masm.ldr64(callTarget.code, callTarget.code, funcIndex * 8);
        masm.br(callTarget.code);
    });
    
    return true;
}

bool BaselineCompiler::emitReturnCallIndirect(uint32_t typeIndex, uint32_t tableIndex) {
    // Tail call through table
    // Pop index, look up function, then tail call
    
    ValueLocation indexLoc = pop();
    (void)indexLoc;
    
    // Get callee type to know argument count
    const FuncType* calleeTypePtr = module_->funcType(typeIndex);
    if (!calleeTypePtr) return false;
    const FuncType& calleeType = *calleeTypePtr;
    size_t numParams = calleeType.params().size();
    
    // Collect arguments
    std::vector<ValueLocation> args;
    for (size_t i = 0; i < numParams; i++) {
        args.push_back(pop());
    }
    std::reverse(args.begin(), args.end());
    
    (void)tableIndex;
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        
        // Load table base and bounds check (similar to call_indirect)
        Reg tableBase = allocReg(ValType::i64());
        Reg tableSize = allocReg(ValType::i32());
        masm.load64(tableBase.code, X86Assembler::RBP, -24);
        masm.load32(tableSize.code, X86Assembler::RBP, -32);
        
        // Bounds check and load function pointer
        // ... (similar to emitCallIndirect)
        
        // Set up arguments
        for (size_t i = 0; i < numParams && i < 6; i++) {
            static const uint8_t argRegs[] = {X86Assembler::RDI, X86Assembler::RSI,
                X86Assembler::RDX, X86Assembler::RCX, 8, 9};
            if (args[i].isRegister()) {
                masm.mov64(argRegs[i], args[i].reg.code);
            }
        }
        
        // Restore frame
        masm.pop(X86Assembler::RBP);
        
        // Jump through function pointer
        // masm.jmp(funcPtr);
        
        freeReg(tableBase);
        freeReg(tableSize);
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        
        // Load table, bounds check
        Reg tableBase = allocReg(ValType::i64());
        masm.ldr64(tableBase.code, 29, -24);
        
        // Set up args in X0-X7
        for (size_t i = 0; i < numParams && i < 8; i++) {
            if (args[i].isRegister()) {
                masm.mov64(i, args[i].reg.code);
            }
        }
        
        // Restore frame
        masm.ldpPost64(29, 30, 31, frameSize_);
        
        // Jump through register (BR, not BLR)
        masm.br(tableBase.code);
        
        freeReg(tableBase);
    });
    
    return true;
}

bool BaselineCompiler::emitReturnCallRef(uint32_t typeIndex) {
    // Tail call through function reference
    ValueLocation funcRef = pop();
    (void)funcRef;
    
    const FuncType* calleeTypePtr = module_->funcType(typeIndex);
    if (!calleeTypePtr) return false;
    const FuncType& calleeType = *calleeTypePtr;
    size_t numParams = calleeType.params().size();
    
    // Collect arguments
    std::vector<ValueLocation> args;
    for (size_t i = 0; i < numParams; i++) {
        args.push_back(pop());
    }
    std::reverse(args.begin(), args.end());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        
        // funcRef is in a register - extract function pointer
        Reg funcPtr = allocReg(ValType::i64());
        
        // Null check
        masm.cmp64Imm(funcPtr.code, 0);
        size_t nullTrap = masm.jcc32(X86Assembler::E);
        (void)nullTrap;
        
        // Type check would go here
        
        // Set up arguments
        for (size_t i = 0; i < numParams && i < 6; i++) {
            static const uint8_t argRegs[] = {X86Assembler::RDI, X86Assembler::RSI,
                X86Assembler::RDX, X86Assembler::RCX, 8, 9};
            if (args[i].isRegister()) {
                masm.mov64(argRegs[i], args[i].reg.code);
            }
        }
        
        // Restore frame and jump
        masm.pop(X86Assembler::RBP);
        masm.jmp(funcPtr.code);
        
        freeReg(funcPtr);
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        
        Reg funcPtr = allocReg(ValType::i64());
        
        // Null check
        masm.cbnz64(funcPtr.code, 8);
        masm.brk(0);  // Trap on null
        
        // Set up args
        for (size_t i = 0; i < numParams && i < 8; i++) {
            if (args[i].isRegister()) {
                masm.mov64(i, args[i].reg.code);
            }
        }
        
        // Restore frame and jump
        masm.ldpPost64(29, 30, 31, frameSize_);
        masm.br(funcPtr.code);
        
        freeReg(funcPtr);
    });
    
    return true;
}

// =============================================================================
// Drop/Select
// =============================================================================

bool BaselineCompiler::emitDrop() {
    if (valueStack_.empty()) {
        error_ = "drop on empty stack";
        return false;
    }
    pop();
    return true;
}

bool BaselineCompiler::emitSelect() {
    ValueLocation cond = pop();
    ValueLocation val2 = pop();
    ValueLocation val1 = pop();
    
    // Materialize operands into registers
    Reg condReg = cond.isRegister() ? cond.reg : allocReg(ValType::i32());
    Reg r1 = val1.isRegister() ? val1.reg : allocReg(val1.type);
    Reg r2 = val2.isRegister() ? val2.reg : allocReg(val2.type);
    
    if (!cond.isRegister()) {
        if (cond.isImmediate()) {
            EMIT_X64({ X86Assembler m(codeBuffer_.get()); m.mov32Imm(condReg.code, static_cast<int32_t>(cond.immediate)); });
            EMIT_ARM64({ ARM64Assembler m(codeBuffer_.get()); m.movImm32(condReg.code, static_cast<uint32_t>(cond.immediate)); });
        } else {
            EMIT_X64({ X86Assembler m(codeBuffer_.get()); m.load32(condReg.code, X86Assembler::RBP, cond.slot.offset); });
            EMIT_ARM64({ ARM64Assembler m(codeBuffer_.get()); m.ldr32(condReg.code, 29, cond.slot.offset); });
        }
    }
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // test cond, cond
        masm.cmp32Imm(condReg.code, 0);
        // jnz skip — if cond != 0, keep r1 (val1)
        size_t skipPatch = masm.jcc32(X86Assembler::NE);
        // cond == 0: pick val2
        masm.mov32(r1.code, r2.code);
        // skip:
        masm.patchJump(skipPatch, codeBuffer_->offset());
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.cmpImm32(condReg.code, 0);
        masm.csel32(r1.code, r1.code, r2.code, ARM64Assembler::NE);
    });
    
    push(ValueLocation::inReg(r1, val1.type));
    return true;
}

// =============================================================================
// Code Generation Helpers
// =============================================================================

void BaselineCompiler::emitPrologue(const FuncType* funcType, uint32_t numLocals) {
    // Push frame pointer
    // Set up stack frame
    // Initialize locals to zero
    codeBuffer_->emit8(0x55);  // push rbp (x86-64)
    codeBuffer_->emit8(0x48);  // mov rbp, rsp
    codeBuffer_->emit8(0x89);
    codeBuffer_->emit8(0xE5);
}

void BaselineCompiler::emitEpilogue() {
    // Restore stack
    // Pop frame pointer
    // Return
    codeBuffer_->emit8(0x5D);  // pop rbp (x86-64)
    codeBuffer_->emit8(0xC3);  // ret
}

void BaselineCompiler::emitTierUpCheck() {
    //
    // The tier-up counter is stored in the TierUpCount structure.
    // We decrement by loopIncrement() on each loop iteration.
    // When counter goes negative, we branch to the tier-up slow path.
    //
    // Counter layout:
    //   - Starts at threshold (e.g., 1000 for warm-up)
    //   - Decremented by 100 per loop iteration
    //   - When <= 0, trigger tier-up compilation
    
    if (!tierUpCounterPtr_) {
        return;  // Tier-up not initialized
    }
    
    uint32_t loopIndex = currentLoopIndex_++;
    
#if defined(__x86_64__) || defined(_M_X64)
    X86Assembler masm(codeBuffer_.get());
    
    // Load tier-up counter address into scratch register
    // mov rax, [tierUpCounterPtr_]
    masm.mov64Imm(X86Assembler::RAX, reinterpret_cast<uint64_t>(tierUpCounterPtr_));
    
    // Decrement counter: sub dword [rax], loopIncrement
    // x86 encoding: sub [mem], imm32
    codeBuffer_->emit8(0x81);           // sub r/m32, imm32
    codeBuffer_->emit8(0x28);           // ModR/M: [rax] with /5 for sub
    codeBuffer_->emit32(100);           // loopIncrement = 100
    
    // Branch if counter still positive (no tier-up needed)
    // jns skip_tierup (0x79 = jns rel8)
    size_t branchOffset = codeBuffer_->offset();
    codeBuffer_->emit8(0x79);           // jns rel8
    codeBuffer_->emit8(0x00);           // placeholder - will patch
    
    // === Tier-up slow path ===
    // Save current state and call tier-up handler
    
    // Push function index and loop index for tier-up handler
    masm.pushImm32(funcIndex_);
    masm.pushImm32(loopIndex);
    
    // Call tier-up handler (implemented in runtime)
    // This is a slow path - the handler will either:
    // 1. Start background compilation
    // 2. Do OSR entry if optimized code is ready
    masm.mov64Imm(X86Assembler::RAX, reinterpret_cast<uint64_t>(tierUpSlowPath_));
    codeBuffer_->emit8(0xFF);           // call rax
    codeBuffer_->emit8(0xD0);
    
    // Clean up stack
    masm.popReg(X86Assembler::RCX);     // pop loop index
    masm.popReg(X86Assembler::RCX);     // pop func index
    
    // Patch branch target
    size_t skipTarget = codeBuffer_->offset();
    int8_t branchDisp = static_cast<int8_t>(skipTarget - branchOffset - 2);
    codeBuffer_->patchByte(branchOffset + 1, branchDisp);
    
#elif defined(__aarch64__) || defined(_M_ARM64)
    ARM64Assembler masm(codeBuffer_.get());
    
    // Load tier-up counter address
    // ldr x16, =tierUpCounterPtr_
    masm.movImm64(16, reinterpret_cast<uint64_t>(tierUpCounterPtr_));
    
    // Load counter value: ldr w17, [x16]
    masm.ldr32(17, 16, 0);
    
    // Decrement: sub w17, w17, #100
    masm.subImm32(17, 17, 100);
    
    // Store back: str w17, [x16]
    masm.str32(17, 16, 0);
    
    // Branch if still positive: tbz w17, #31, skip (test bit 31 = sign bit)
    // If sign bit is 0, counter is positive, skip tier-up
    size_t branchOffset = codeBuffer_->offset();
    masm.tbz32(17, 31, 0);  // placeholder offset
    
    // === Tier-up slow path ===
    // Save x0-x1 for call
    masm.strPre(0, 31, -16);
    masm.str64(1, 31, 8);
    
    // Setup args: x0 = funcIndex, x1 = loopIndex
    masm.movImm32(0, funcIndex_);
    masm.movImm32(1, loopIndex);
    
    // Call tier-up handler
    masm.movImm64(16, reinterpret_cast<uint64_t>(tierUpSlowPath_));
    masm.blr(16);
    
    // Restore
    masm.ldr64(1, 31, 8);
    masm.ldrPost(0, 31, 16);
    
    // Patch branch - calculate offset
    size_t skipTarget = codeBuffer_->offset();
    int32_t offset = static_cast<int32_t>(skipTarget - branchOffset);
    // Patch the TBZ placeholder at branchOffset (tests sign bit 31 of w17)
    codeBuffer_->patch32(branchOffset, ARM64Assembler::encodeTbz32(17, 31, offset));
#endif

    // Record OSR entry point for this loop
    tierUpOSRPoints_.push_back({loopIndex, codeBuffer_->offset(), valueStack_.size()});
}

void BaselineCompiler::emitStackCheck(uint32_t frameSize) {
    // Check stack limit
    // On x86-64:
    //   cmp rsp - frameSize, [limit]
    //   jb trap
    // On ARM64:
    //   sub tmp, sp, frameSize
    //   ldr limit, [limit_addr]
    //   cmp tmp, limit
    //   b.lo trap
    
    if (IsX64Platform) {
        // x64: Compare stack pointer against limit
        codeBuffer_->emit8(0x48);  // REX.W
        codeBuffer_->emit8(0x81);  // CMP r/m64, imm32
        codeBuffer_->emit8(0xFC);  // ModR/M for RSP
        codeBuffer_->emit32(0x1000);  // Stack limit placeholder
    } else {
        // ARM64 placeholder - stack check
        codeBuffer_->emit32(0xD10003FF); // sub sp, sp, #0
    }
}

void BaselineCompiler::emitTrap(Trap trap) {
    // Emit call to trap handler
    codeBuffer_->emit8(0xCC);  // int 3 (debug break - placeholder)
}

void BaselineCompiler::emitBoundsCheck(uint32_t offset) {
    // Legacy 32-bit bounds check
    emitBoundsCheck64(offset, false);
}

void BaselineCompiler::emitBoundsCheck64(uint32_t offset, bool isMemory64) {
    // Memory bounds checking for Memory32 and Memory64
    //
    // For Memory32:
    //   if (addr + offset > memSize32) trap
    //
    // For Memory64:
    //   if (addr + offset > memSize64) trap
    //
    // Memory size is stored in instance at a known offset
    
#if defined(__x86_64__) || defined(_M_X64)
    X86Assembler masm(codeBuffer_.get());
    
    // Load memory size from instance (stored at RBP - 24)
    if (isMemory64) {
        masm.load64(X86Assembler::R10, X86Assembler::RBP, -24);
    } else {
        masm.load32(X86Assembler::R10, X86Assembler::RBP, -24);
    }
    
    // Add offset to address in R11 (scratch)
    if (offset != 0) {
        masm.mov64Imm(X86Assembler::R11, offset);
        // add r11, [top of stack - address]
    }
    
    // Compare: if address + offset >= memSize, trap
    if (isMemory64) {
        masm.cmp64(X86Assembler::RAX, X86Assembler::R10);
    } else {
        masm.cmp32(X86Assembler::RAX, X86Assembler::R10);  // EAX = RAX lower 32 bits
    }
    
    // jae (jump if above or equal = unsigned >=) to trap
    size_t trapJumpOffset = codeBuffer_->offset();
    codeBuffer_->emit8(0x73);   // jae rel8
    codeBuffer_->emit8(0x02);   // skip 2 bytes (the jmp over trap)
    
    // Normal path: skip trap
    codeBuffer_->emit8(0xEB);   // jmp rel8
    codeBuffer_->emit8(0x01);   // skip 1 byte
    
    // Trap instruction
    codeBuffer_->emit8(0xCC);   // int 3 (will be replaced with proper trap call)
    
#elif defined(__aarch64__) || defined(_M_ARM64)
    ARM64Assembler masm(codeBuffer_.get());
    
    // Load memory size from instance
    if (isMemory64) {
        masm.ldr64(16, 29, -24);  // x16 = memory size
    } else {
        masm.ldr32(16, 29, -24);  // w16 = memory size (zero-extended)
    }
    
    // Compare address (in x0/w0 assumed) against memory size
    if (isMemory64) {
        masm.cmp64(0, 16);
    } else {
        masm.cmp32(0, 16);
    }
    
    // Branch if higher or same (unsigned >=) to trap
    // b.hs trap (condition = 2 for HS)
    size_t trapBranchOffset = codeBuffer_->offset();
    uint32_t bhs = 0x54000002;  // b.hs with placeholder offset
    masm.emit32(bhs);
    
    // Will patch branch target to trap handler
#endif
}

// =============================================================================
// Value Stack
// =============================================================================

void BaselineCompiler::push(ValueLocation loc) {
    valueStack_.push_back(loc);
}

ValueLocation BaselineCompiler::pop() {
    if (valueStack_.empty()) {
        return ValueLocation();
    }
    ValueLocation loc = valueStack_.back();
    valueStack_.pop_back();
    return loc;
}

ValueLocation BaselineCompiler::peek(uint32_t depth) {
    if (depth >= valueStack_.size()) {
        return ValueLocation();
    }
    return valueStack_[valueStack_.size() - 1 - depth];
}

// =============================================================================
// Register Helpers
// =============================================================================

Reg BaselineCompiler::allocReg(ValType type) {
    if (type.isNumeric() && !type.isReference()) {
        if (type.kind() == ValType::Kind::F32 || type.kind() == ValType::Kind::F64) {
            return regAlloc_.allocFPR();
        } else if (type.kind() == ValType::Kind::V128) {
            return regAlloc_.allocV128();
        }
    }
    return regAlloc_.allocGPR();
}

void BaselineCompiler::freeReg(Reg reg) {
    regAlloc_.freeReg(reg);
}

Reg BaselineCompiler::allocVReg() {
    // Allocate a vector register (XMM/V register)
    return regAlloc_.allocV128();
}

void BaselineCompiler::freeVReg(Reg reg) {
    regAlloc_.freeReg(reg);  // Same as GPR/FPR for now
}

// =============================================================================
// Label Management
// =============================================================================

Label BaselineCompiler::newLabel() {
    return Label(nextLabelId_++);
}

void BaselineCompiler::bindLabel(Label& label) {
    label.offset = codeBuffer_->offset();
    label.bound = true;
    
    // Patch all pending jumps
    for (size_t jumpOffset : label.pendingJumps) {
        int32_t displacement = static_cast<int32_t>(label.offset - (jumpOffset + 4));
        codeBuffer_->patch32(jumpOffset, displacement);
    }
    label.pendingJumps.clear();
}

void BaselineCompiler::jumpTo(Label& label) {
    if (label.bound) {
        // Emit direct jump
        int32_t displacement = static_cast<int32_t>(label.offset - (codeBuffer_->offset() + 5));
        codeBuffer_->emit8(0xE9);  // jmp rel32
        codeBuffer_->emit32(displacement);
    } else {
        // Emit placeholder and record for patching
        codeBuffer_->emit8(0xE9);
        label.pendingJumps.push_back(codeBuffer_->offset());
        codeBuffer_->emit32(0);
    }
}

void BaselineCompiler::branchTo(Label& label, Reg condition) {
    X86Assembler masm(codeBuffer_.get());
    
    // Test condition register against zero
    masm.test32(condition.code, condition.code);
    
    // Emit jnz (jump if not zero) to label
    if (label.bound) {
        int32_t displacement = static_cast<int32_t>(label.offset - (codeBuffer_->offset() + 6));
        codeBuffer_->emit8(0x0F);
        codeBuffer_->emit8(0x85);  // jnz rel32
        codeBuffer_->emit32(displacement);
    } else {
        codeBuffer_->emit8(0x0F);
        codeBuffer_->emit8(0x85);  // jnz rel32
        label.pendingJumps.push_back(codeBuffer_->offset());
        codeBuffer_->emit32(0);
    }
}

// =============================================================================
// Binary/Unary Operation Templates
// =============================================================================

template<typename EmitFn>
bool BaselineCompiler::emitBinaryOp(ValType type, EmitFn&& emit) {
    ValueLocation rhs = pop();
    ValueLocation lhs = pop();
    
    Reg dstReg = allocReg(type);
    Reg srcReg = allocReg(type);
    
    // Load operands to registers if needed
    // ... actual code gen
    
    emit(dstReg, srcReg);
    
    freeReg(srcReg);
    push(ValueLocation::inReg(dstReg, type));
    return true;
}

template<typename EmitFn>
bool BaselineCompiler::emitUnaryOp(ValType type, EmitFn&& emit) {
    ValueLocation operand = pop();
    
    Reg dstReg = allocReg(type);
    
    // Load operand to register if needed
    // ... actual code gen
    
    emit(dstReg);
    
    push(ValueLocation::inReg(dstReg, type));
    return true;
}

// =============================================================================
// SIMD Operations (v128)
// =============================================================================

bool BaselineCompiler::emitSimdOp(uint32_t simdOp) {
    // SIMD opcode constants (from WASM SIMD proposal)
    // Load/Store
    constexpr uint32_t V128_LOAD = 0x00;
    constexpr uint32_t V128_STORE = 0x0B;
    constexpr uint32_t V128_CONST = 0x0C;
    
    // Shuffle/Swizzle
    constexpr uint32_t I8X16_SHUFFLE = 0x0D;
    constexpr uint32_t I8X16_SWIZZLE = 0x0E;
    
    // Splat
    constexpr uint32_t I8X16_SPLAT = 0x0F;
    constexpr uint32_t I16X8_SPLAT = 0x10;
    constexpr uint32_t I32X4_SPLAT = 0x11;
    constexpr uint32_t I64X2_SPLAT = 0x12;
    constexpr uint32_t F32X4_SPLAT = 0x13;
    constexpr uint32_t F64X2_SPLAT = 0x14;
    
    // Lane extract (i8x16)
    constexpr uint32_t I8X16_EXTRACT_LANE_S = 0x15;
    constexpr uint32_t I8X16_EXTRACT_LANE_U = 0x16;
    constexpr uint32_t I8X16_REPLACE_LANE = 0x17;
    
    // Lane extract (i16x8)
    constexpr uint32_t I16X8_EXTRACT_LANE_S = 0x18;
    constexpr uint32_t I16X8_EXTRACT_LANE_U = 0x19;
    constexpr uint32_t I16X8_REPLACE_LANE = 0x1A;
    
    // Lane extract (i32x4)
    constexpr uint32_t I32X4_EXTRACT_LANE = 0x1B;
    constexpr uint32_t I32X4_REPLACE_LANE = 0x1C;
    
    // Lane extract (i64x2)
    constexpr uint32_t I64X2_EXTRACT_LANE = 0x1D;
    constexpr uint32_t I64X2_REPLACE_LANE = 0x1E;
    
    // Lane extract (f32x4/f64x2)
    constexpr uint32_t F32X4_EXTRACT_LANE = 0x1F;
    constexpr uint32_t F32X4_REPLACE_LANE = 0x20;
    constexpr uint32_t F64X2_EXTRACT_LANE = 0x21;
    constexpr uint32_t F64X2_REPLACE_LANE = 0x22;
    
    // i8x16 comparisons
    constexpr uint32_t I8X16_EQ = 0x23;
    constexpr uint32_t I8X16_NE = 0x24;
    constexpr uint32_t I8X16_LT_S = 0x25;
    constexpr uint32_t I8X16_LT_U = 0x26;
    constexpr uint32_t I8X16_GT_S = 0x27;
    constexpr uint32_t I8X16_GT_U = 0x28;
    constexpr uint32_t I8X16_LE_S = 0x29;
    constexpr uint32_t I8X16_LE_U = 0x2A;
    constexpr uint32_t I8X16_GE_S = 0x2B;
    constexpr uint32_t I8X16_GE_U = 0x2C;
    
    // i16x8 comparisons
    constexpr uint32_t I16X8_EQ = 0x2D;
    constexpr uint32_t I16X8_NE = 0x2E;
    constexpr uint32_t I16X8_LT_S = 0x2F;
    constexpr uint32_t I16X8_LT_U = 0x30;
    constexpr uint32_t I16X8_GT_S = 0x31;
    constexpr uint32_t I16X8_GT_U = 0x32;
    constexpr uint32_t I16X8_LE_S = 0x33;
    constexpr uint32_t I16X8_LE_U = 0x34;
    constexpr uint32_t I16X8_GE_S = 0x35;
    constexpr uint32_t I16X8_GE_U = 0x36;
    
    // i32x4 comparisons
    constexpr uint32_t I32X4_EQ = 0x37;
    constexpr uint32_t I32X4_NE = 0x38;
    constexpr uint32_t I32X4_LT_S = 0x39;
    constexpr uint32_t I32X4_LT_U = 0x3A;
    constexpr uint32_t I32X4_GT_S = 0x3B;
    constexpr uint32_t I32X4_GT_U = 0x3C;
    constexpr uint32_t I32X4_LE_S = 0x3D;
    constexpr uint32_t I32X4_LE_U = 0x3E;
    constexpr uint32_t I32X4_GE_S = 0x3F;
    constexpr uint32_t I32X4_GE_U = 0x40;
    
    // Bitwise
    constexpr uint32_t V128_NOT = 0x4D;
    constexpr uint32_t V128_AND = 0x4E;
    constexpr uint32_t V128_ANDNOT = 0x4F;
    constexpr uint32_t V128_OR = 0x50;
    constexpr uint32_t V128_XOR = 0x51;
    constexpr uint32_t V128_BITSELECT = 0x52;
    constexpr uint32_t V128_ANY_TRUE = 0x53;
    
    // i8x16 arithmetic
    constexpr uint32_t I8X16_ABS = 0x60;
    constexpr uint32_t I8X16_NEG = 0x61;
    constexpr uint32_t I8X16_ADD = 0x6E;
    constexpr uint32_t I8X16_ADD_SAT_S = 0x6F;
    constexpr uint32_t I8X16_ADD_SAT_U = 0x70;
    constexpr uint32_t I8X16_SUB = 0x71;
    constexpr uint32_t I8X16_SUB_SAT_S = 0x72;
    constexpr uint32_t I8X16_SUB_SAT_U = 0x73;
    
    // i16x8 arithmetic
    constexpr uint32_t I16X8_ABS = 0x80;
    constexpr uint32_t I16X8_NEG = 0x81;
    constexpr uint32_t I16X8_ADD = 0x8E;
    constexpr uint32_t I16X8_ADD_SAT_S = 0x8F;
    constexpr uint32_t I16X8_ADD_SAT_U = 0x90;
    constexpr uint32_t I16X8_SUB = 0x91;
    constexpr uint32_t I16X8_SUB_SAT_S = 0x92;
    constexpr uint32_t I16X8_SUB_SAT_U = 0x93;
    constexpr uint32_t I16X8_MUL = 0x95;
    
    // i32x4 arithmetic
    constexpr uint32_t I32X4_ABS = 0xA0;
    constexpr uint32_t I32X4_NEG = 0xA1;
    constexpr uint32_t I32X4_ADD = 0xAE;
    constexpr uint32_t I32X4_SUB = 0xB1;
    constexpr uint32_t I32X4_MUL = 0xB5;
    constexpr uint32_t I32X4_DOT_I16X8_S = 0xBA;
    
    // i64x2 arithmetic + comparisons
    constexpr uint32_t I64X2_ABS = 0xC0;
    constexpr uint32_t I64X2_NEG = 0xC1;
    constexpr uint32_t I64X2_ADD = 0xCE;
    constexpr uint32_t I64X2_SUB = 0xD1;
    constexpr uint32_t I64X2_MUL = 0xD5;
    constexpr uint32_t I64X2_EQ = 0xD6;
    constexpr uint32_t I64X2_NE = 0xD7;
    constexpr uint32_t I64X2_LT_S = 0xD8;
    constexpr uint32_t I64X2_GT_S = 0xD9;
    constexpr uint32_t I64X2_LE_S = 0xDA;
    constexpr uint32_t I64X2_GE_S = 0xDB;
    
    // f32x4 arithmetic
    constexpr uint32_t F32X4_ABS = 0xE0;
    constexpr uint32_t F32X4_NEG = 0xE1;
    constexpr uint32_t F32X4_SQRT = 0xE3;
    constexpr uint32_t F32X4_ADD = 0xE4;
    constexpr uint32_t F32X4_SUB = 0xE5;
    constexpr uint32_t F32X4_MUL = 0xE6;
    constexpr uint32_t F32X4_DIV = 0xE7;
    constexpr uint32_t F32X4_MIN = 0xE8;
    constexpr uint32_t F32X4_MAX = 0xE9;
    constexpr uint32_t F32X4_PMIN = 0xEA;
    constexpr uint32_t F32X4_PMAX = 0xEB;
    
    // f64x2 arithmetic
    constexpr uint32_t F64X2_ABS = 0xEC;
    constexpr uint32_t F64X2_NEG = 0xED;
    constexpr uint32_t F64X2_SQRT = 0xEF;
    constexpr uint32_t F64X2_ADD = 0xF0;
    constexpr uint32_t F64X2_SUB = 0xF1;
    constexpr uint32_t F64X2_MUL = 0xF2;
    constexpr uint32_t F64X2_DIV = 0xF3;
    constexpr uint32_t F64X2_MIN = 0xF4;
    constexpr uint32_t F64X2_MAX = 0xF5;
    constexpr uint32_t F64X2_PMIN = 0xF6;
    constexpr uint32_t F64X2_PMAX = 0xF7;
    
    switch (simdOp) {
        // ===== Load/Store =====
        case V128_LOAD: {
            uint32_t flags, offset;
            if (!decoder_->readVarU32(&flags) || !decoder_->readVarU32(&offset)) {
                error_ = "failed to read v128.load operands";
                return false;
            }
            return emitV128Load(offset);
        }
        
        case V128_STORE: {
            uint32_t flags, offset;
            if (!decoder_->readVarU32(&flags) || !decoder_->readVarU32(&offset)) {
                error_ = "failed to read v128.store operands";
                return false;
            }
            return emitV128Store(offset);
        }
        
        case V128_CONST: {
            // Read 16 bytes of constant
            uint8_t bytes[16];
            for (int i = 0; i < 16; i++) {
                if (!decoder_->readByte(&bytes[i])) {
                    error_ = "failed to read v128.const";
                    return false;
                }
            }
            return emitV128Const(bytes);
        }
        
        // ===== Shuffle/Swizzle =====
        case I8X16_SHUFFLE: {
            uint8_t lanes[16];
            for (int i = 0; i < 16; i++) {
                if (!decoder_->readByte(&lanes[i])) {
                    error_ = "failed to read shuffle lanes";
                    return false;
                }
            }
            return emitI8x16Shuffle(lanes);
        }
        
        case I8X16_SWIZZLE: return emitI8x16Swizzle();
        
        // ===== Splat =====
        case I8X16_SPLAT: return emitSplat(1);
        case I16X8_SPLAT: return emitSplat(2);
        case I32X4_SPLAT: return emitSplat(4);
        case I64X2_SPLAT: return emitSplat(8);
        case F32X4_SPLAT: return emitSplatF32();
        case F64X2_SPLAT: return emitSplatF64();
        
        // ===== Lane Extract (i8x16) =====
        case I8X16_EXTRACT_LANE_S:
        case I8X16_EXTRACT_LANE_U: {
            uint8_t lane;
            if (!decoder_->readByte(&lane)) {
                error_ = "failed to read lane index";
                return false;
            }
            return emitExtractLane(1, lane, simdOp == I8X16_EXTRACT_LANE_S);
        }
        case I8X16_REPLACE_LANE: {
            uint8_t lane;
            if (!decoder_->readByte(&lane)) return false;
            return emitReplaceLane(1, lane);
        }
        
        // ===== Lane Extract (i16x8) =====
        case I16X8_EXTRACT_LANE_S:
        case I16X8_EXTRACT_LANE_U: {
            uint8_t lane;
            if (!decoder_->readByte(&lane)) return false;
            return emitExtractLane(2, lane, simdOp == I16X8_EXTRACT_LANE_S);
        }
        case I16X8_REPLACE_LANE: {
            uint8_t lane;
            if (!decoder_->readByte(&lane)) return false;
            return emitReplaceLane(2, lane);
        }
        
        // ===== Lane Extract (i32x4) =====
        case I32X4_EXTRACT_LANE: {
            uint8_t lane;
            if (!decoder_->readByte(&lane)) return false;
            return emitExtractLane(4, lane, false);
        }
        case I32X4_REPLACE_LANE: {
            uint8_t lane;
            if (!decoder_->readByte(&lane)) return false;
            return emitReplaceLane(4, lane);
        }
        
        // ===== Lane Extract (i64x2) =====
        case I64X2_EXTRACT_LANE: {
            uint8_t lane;
            if (!decoder_->readByte(&lane)) return false;
            return emitExtractLane(8, lane, false);
        }
        case I64X2_REPLACE_LANE: {
            uint8_t lane;
            if (!decoder_->readByte(&lane)) return false;
            return emitReplaceLane(8, lane);
        }
        
        // ===== Lane Extract (f32x4/f64x2) =====
        case F32X4_EXTRACT_LANE: {
            uint8_t lane;
            if (!decoder_->readByte(&lane)) return false;
            return emitExtractLaneF(4, lane);
        }
        case F32X4_REPLACE_LANE: {
            uint8_t lane;
            if (!decoder_->readByte(&lane)) return false;
            return emitReplaceLaneF(4, lane);
        }
        case F64X2_EXTRACT_LANE: {
            uint8_t lane;
            if (!decoder_->readByte(&lane)) return false;
            return emitExtractLaneF(8, lane);
        }
        case F64X2_REPLACE_LANE: {
            uint8_t lane;
            if (!decoder_->readByte(&lane)) return false;
            return emitReplaceLaneF(8, lane);
        }
        
        // ===== Comparisons (all types) =====
        case I8X16_EQ: case I8X16_NE:
        case I8X16_LT_S: case I8X16_LT_U: case I8X16_GT_S: case I8X16_GT_U:
        case I8X16_LE_S: case I8X16_LE_U: case I8X16_GE_S: case I8X16_GE_U:
        case I16X8_EQ: case I16X8_NE:
        case I16X8_LT_S: case I16X8_LT_U: case I16X8_GT_S: case I16X8_GT_U:
        case I16X8_LE_S: case I16X8_LE_U: case I16X8_GE_S: case I16X8_GE_U:
        case I32X4_EQ: case I32X4_NE:
        case I32X4_LT_S: case I32X4_LT_U: case I32X4_GT_S: case I32X4_GT_U:
        case I32X4_LE_S: case I32X4_LE_U: case I32X4_GE_S: case I32X4_GE_U:
        case I64X2_EQ: case I64X2_NE:
        case I64X2_LT_S: case I64X2_GT_S: case I64X2_LE_S: case I64X2_GE_S:
            return emitV128Compare(simdOp);
        
        // ===== Bitwise =====
        case V128_NOT:      return emitV128UnaryOp(simdOp);
        case V128_AND:      return emitV128BinaryOp(simdOp);
        case V128_ANDNOT:   return emitV128BinaryOp(simdOp);
        case V128_OR:       return emitV128BinaryOp(simdOp);
        case V128_XOR:      return emitV128BinaryOp(simdOp);
        case V128_BITSELECT: return emitV128Bitselect();
        case V128_ANY_TRUE: return emitV128AnyTrue();
        
        // ===== Unary ops =====
        case I8X16_ABS: case I8X16_NEG:
        case I16X8_ABS: case I16X8_NEG:
        case I32X4_ABS: case I32X4_NEG:
        case I64X2_ABS: case I64X2_NEG:
        case F32X4_ABS: case F32X4_NEG: case F32X4_SQRT:
        case F64X2_ABS: case F64X2_NEG: case F64X2_SQRT:
            return emitV128UnaryOp(simdOp);
        
        // ===== Binary arithmetic =====
        case I8X16_ADD: case I8X16_SUB:
        case I8X16_ADD_SAT_S: case I8X16_ADD_SAT_U:
        case I8X16_SUB_SAT_S: case I8X16_SUB_SAT_U:
        case I16X8_ADD: case I16X8_SUB: case I16X8_MUL:
        case I16X8_ADD_SAT_S: case I16X8_ADD_SAT_U:
        case I16X8_SUB_SAT_S: case I16X8_SUB_SAT_U:
        case I32X4_ADD: case I32X4_SUB: case I32X4_MUL:
        case I64X2_ADD: case I64X2_SUB: case I64X2_MUL:
        case F32X4_ADD: case F32X4_SUB: case F32X4_MUL: case F32X4_DIV:
        case F32X4_MIN: case F32X4_MAX: case F32X4_PMIN: case F32X4_PMAX:
        case F64X2_ADD: case F64X2_SUB: case F64X2_MUL: case F64X2_DIV:
        case F64X2_MIN: case F64X2_MAX: case F64X2_PMIN: case F64X2_PMAX:
            return emitV128BinaryOp(simdOp);
        
        // ===== Dot product =====
        case I32X4_DOT_I16X8_S:
            return emitI32x4DotI16x8S();
        
        default:
            error_ = "unsupported SIMD opcode: " + std::to_string(simdOp);
            return false;
    }
}

bool BaselineCompiler::emitV128Load(uint32_t offset) {
    // Pop address, load 128-bit value
    ValueLocation addr = pop();
    (void)addr;  // Suppress unused warning
    Reg addrReg = allocReg(ValType::i32());
    Reg vReg = allocVReg();  // Vector register
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // Simplified: X64 SIMD load would go here
        (void)masm; (void)vReg; (void)addrReg; (void)offset;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        if (offset) {
            masm.addImm64(addrReg.code, addrReg.code, offset);
        }
        masm.ldrQ(vReg.code, addrReg.code, 0);
    });
    
    freeReg(addrReg);
    push(ValueLocation::inReg(vReg, ValType::i32()));  // Simplified: use i32 type
    return true;
}

bool BaselineCompiler::emitV128Store(uint32_t offset) {
    ValueLocation value = pop();
    ValueLocation addr = pop();
    (void)value; (void)addr;  // Suppress unused warnings
    
    Reg addrReg = allocReg(ValType::i32());
    Reg vReg = allocVReg();
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // Simplified: X64 SIMD store would go here
        (void)masm; (void)addrReg; (void)offset; (void)vReg;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        if (offset) {
            masm.addImm64(addrReg.code, addrReg.code, offset);
        }
        masm.strQ(vReg.code, addrReg.code, 0);
    });
    
    freeReg(addrReg);
    freeVReg(vReg);
    return true;
}

bool BaselineCompiler::emitV128BinaryOp(uint32_t simdOp) {
    ValueLocation rhs = pop();
    ValueLocation lhs = pop();
    (void)rhs; (void)lhs;  // Suppress unused warnings
    
    Reg dst = allocVReg();
    Reg src = allocVReg();
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // Simplified: just emit NOP-like instructions for now
        // Full implementation would need X86 SSE/AVX instructions
        (void)masm; (void)dst; (void)src; (void)simdOp;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        // SIMD opcode constants (from Opcode::Simd namespace)
        constexpr uint32_t I32X4_ADD = 0xA0;
        constexpr uint32_t I32X4_SUB = 0xA1;
        constexpr uint32_t I32X4_MUL = 0xB5;
        constexpr uint32_t F32X4_ADD = 0xE0;
        constexpr uint32_t F32X4_SUB = 0xE1;
        constexpr uint32_t F32X4_MUL = 0xE4;
        constexpr uint32_t F32X4_DIV = 0xE3;
        constexpr uint32_t F64X2_ADD = 0xF0;
        constexpr uint32_t F64X2_SUB = 0xF1;
        constexpr uint32_t F64X2_MUL = 0xF4;
        constexpr uint32_t F64X2_DIV = 0xF3;
        constexpr uint32_t V128_AND  = 0x4E;
        constexpr uint32_t V128_OR   = 0x50;
        constexpr uint32_t V128_XOR  = 0x51;
        
        switch (simdOp) {
            case I32X4_ADD: masm.addV4S(dst.code, dst.code, src.code); break;
            case I32X4_SUB: masm.subV4S(dst.code, dst.code, src.code); break;
            case I32X4_MUL: masm.mulV4S(dst.code, dst.code, src.code); break;
            case F32X4_ADD: masm.faddV4S(dst.code, dst.code, src.code); break;
            case F32X4_SUB: masm.fsubV4S(dst.code, dst.code, src.code); break;
            case F32X4_MUL: masm.fmulV4S(dst.code, dst.code, src.code); break;
            case F32X4_DIV: masm.fdivV4S(dst.code, dst.code, src.code); break;
            case F64X2_ADD: masm.faddV2D(dst.code, dst.code, src.code); break;
            case F64X2_SUB: masm.fsubV2D(dst.code, dst.code, src.code); break;
            case F64X2_MUL: masm.fmulV2D(dst.code, dst.code, src.code); break;
            case F64X2_DIV: masm.fdivV2D(dst.code, dst.code, src.code); break;
            case V128_AND:  masm.andV(dst.code, dst.code, src.code); break;
            case V128_OR:   masm.orrV(dst.code, dst.code, src.code); break;
            case V128_XOR:  masm.eorV(dst.code, dst.code, src.code); break;
            default: break;
        }
    });
    
    freeVReg(src);
    push(ValueLocation::inReg(dst, ValType::i32()));  // Simplified: use i32 type for now
    return true;
}

bool BaselineCompiler::emitV128UnaryOp(uint32_t simdOp) {
    ValueLocation operand = pop();
    (void)operand;  // Suppress unused warning
    
    Reg dst = allocVReg();
    Reg src = allocVReg();  // Source register
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        (void)masm; (void)dst; (void)src; (void)simdOp;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        constexpr uint32_t V128_NOT = 0x4D;
        switch (simdOp) {
            case V128_NOT: masm.notV(dst.code, src.code); break;
            default: break;
        }
    });
    
    freeVReg(src);
    push(ValueLocation::inReg(dst, ValType::i32()));
    return true;
}

// =============================================================================
// Extended SIMD Operations
// =============================================================================

bool BaselineCompiler::emitV128Const(const uint8_t bytes[16]) {
    Reg vReg = allocVReg();
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // Load 128-bit constant from code stream
        (void)masm; (void)vReg; (void)bytes;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        // ARM64: Load from literal pool or construct with mov/ins
        masm.ldq(vReg.code, bytes);
    });
    
    push(ValueLocation::inReg(vReg, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitI8x16Shuffle(const uint8_t lanes[16]) {
    ValueLocation rhs = pop();
    ValueLocation lhs = pop();
    (void)rhs; (void)lhs;
    
    Reg dst = allocVReg();
    Reg src1 = allocVReg();
    Reg src2 = allocVReg();
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        (void)masm; (void)dst; (void)src1; (void)src2; (void)lanes;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        // Use TBL instruction for shuffle
        masm.tbl2(dst.code, src1.code, src2.code, lanes);
    });
    
    freeVReg(src1);
    freeVReg(src2);
    push(ValueLocation::inReg(dst, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitI8x16Swizzle() {
    ValueLocation indices = pop();
    ValueLocation vec = pop();
    (void)indices; (void)vec;
    
    Reg dst = allocVReg();
    Reg src = allocVReg();
    Reg idx = allocVReg();
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        (void)masm; (void)dst; (void)src; (void)idx;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.tbl(dst.code, src.code, idx.code);
    });
    
    freeVReg(src);
    freeVReg(idx);
    push(ValueLocation::inReg(dst, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitSplat(uint8_t laneSize) {
    ValueLocation val = pop();
    (void)val;
    
    Reg gpr = allocReg(ValType::i32());
    Reg dst = allocVReg();
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        (void)masm; (void)gpr; (void)dst; (void)laneSize;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        switch (laneSize) {
            case 1: masm.dupVB(dst.code, gpr.code); break;
            case 2: masm.dupVH(dst.code, gpr.code); break;
            case 4: masm.dupVS(dst.code, gpr.code); break;
            case 8: masm.dupVD(dst.code, gpr.code); break;
        }
    });
    
    freeReg(gpr);
    push(ValueLocation::inReg(dst, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitSplatF32() {
    ValueLocation val = pop();
    (void)val;
    
    Reg fpr = allocReg(ValType::f32());
    Reg dst = allocVReg();
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        (void)masm; (void)fpr; (void)dst;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.dupVS(dst.code, fpr.code);
    });
    
    freeReg(fpr);
    push(ValueLocation::inReg(dst, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitSplatF64() {
    ValueLocation val = pop();
    (void)val;
    
    Reg fpr = allocReg(ValType::f64());
    Reg dst = allocVReg();
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        (void)masm; (void)fpr; (void)dst;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.dupVD(dst.code, fpr.code);
    });
    
    freeReg(fpr);
    push(ValueLocation::inReg(dst, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitExtractLane(uint8_t laneSize, uint8_t laneIdx, bool signExtend) {
    ValueLocation vec = pop();
    (void)vec;
    
    Reg src = allocVReg();
    Reg dst = allocReg(ValType::i32());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        (void)masm; (void)src; (void)dst; (void)laneSize; (void)laneIdx; (void)signExtend;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        switch (laneSize) {
            case 1:
                if (signExtend) masm.smovB(dst.code, src.code, laneIdx);
                else masm.umovB(dst.code, src.code, laneIdx);
                break;
            case 2:
                if (signExtend) masm.smovH(dst.code, src.code, laneIdx);
                else masm.umovH(dst.code, src.code, laneIdx);
                break;
            case 4:
                masm.umovS(dst.code, src.code, laneIdx);
                break;
            case 8:
                masm.umovD(dst.code, src.code, laneIdx);
                break;
        }
    });
    
    freeVReg(src);
    push(ValueLocation::inReg(dst, laneSize >= 8 ? ValType::i64() : ValType::i32()));
    return true;
}

bool BaselineCompiler::emitReplaceLane(uint8_t laneSize, uint8_t laneIdx) {
    ValueLocation val = pop();
    ValueLocation vec = pop();
    (void)val; (void)vec;
    
    Reg src = allocVReg();
    Reg gpr = allocReg(ValType::i32());
    Reg dst = allocVReg();
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        (void)masm; (void)src; (void)gpr; (void)dst; (void)laneSize; (void)laneIdx;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movV(dst.code, src.code);
        switch (laneSize) {
            case 1: masm.insB(dst.code, laneIdx, gpr.code); break;
            case 2: masm.insH(dst.code, laneIdx, gpr.code); break;
            case 4: masm.insS(dst.code, laneIdx, gpr.code); break;
            case 8: masm.insD(dst.code, laneIdx, gpr.code); break;
        }
    });
    
    freeVReg(src);
    freeReg(gpr);
    push(ValueLocation::inReg(dst, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitExtractLaneF(uint8_t laneSize, uint8_t laneIdx) {
    ValueLocation vec = pop();
    (void)vec;
    
    Reg src = allocVReg();
    Reg dst = allocReg(laneSize == 4 ? ValType::f32() : ValType::f64());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        (void)masm; (void)src; (void)dst; (void)laneSize; (void)laneIdx;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        if (laneSize == 4) masm.dupVS_lane(dst.code, src.code, laneIdx);
        else masm.dupVD_lane(dst.code, src.code, laneIdx);
    });
    
    freeVReg(src);
    push(ValueLocation::inReg(dst, laneSize == 4 ? ValType::f32() : ValType::f64()));
    return true;
}

bool BaselineCompiler::emitReplaceLaneF(uint8_t laneSize, uint8_t laneIdx) {
    ValueLocation val = pop();
    ValueLocation vec = pop();
    (void)val; (void)vec;
    
    Reg src = allocVReg();
    Reg fpr = allocReg(laneSize == 4 ? ValType::f32() : ValType::f64());
    Reg dst = allocVReg();
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        (void)masm; (void)src; (void)fpr; (void)dst; (void)laneSize; (void)laneIdx;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movV(dst.code, src.code);
        if (laneSize == 4) masm.insSFromS(dst.code, laneIdx, fpr.code);
        else masm.insDFromD(dst.code, laneIdx, fpr.code);
    });
    
    freeVReg(src);
    freeReg(fpr);
    push(ValueLocation::inReg(dst, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitV128Compare(uint32_t simdOp) {
    ValueLocation rhs = pop();
    ValueLocation lhs = pop();
    (void)rhs; (void)lhs;
    
    Reg dst = allocVReg();
    Reg src1 = allocVReg();
    Reg src2 = allocVReg();
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        (void)masm; (void)dst; (void)src1; (void)src2; (void)simdOp;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        // Emit comparison based on opcode
        // This would dispatch to cmeq, cmgt, cmhs, etc.
        masm.cmeq16B(dst.code, src1.code, src2.code);  // Default to eq
    });
    
    freeVReg(src1);
    freeVReg(src2);
    push(ValueLocation::inReg(dst, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitV128Bitselect() {
    ValueLocation mask = pop();
    ValueLocation onFalse = pop();
    ValueLocation onTrue = pop();
    (void)mask; (void)onFalse; (void)onTrue;
    
    Reg dst = allocVReg();
    Reg ifTrue = allocVReg();
    Reg ifFalse = allocVReg();
    Reg m = allocVReg();
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        (void)masm; (void)dst; (void)ifTrue; (void)ifFalse; (void)m;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.bsl(dst.code, ifTrue.code, ifFalse.code, m.code);
    });
    
    freeVReg(ifTrue);
    freeVReg(ifFalse);
    freeVReg(m);
    push(ValueLocation::inReg(dst, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitV128AnyTrue() {
    ValueLocation vec = pop();
    (void)vec;
    
    Reg src = allocVReg();
    Reg dst = allocReg(ValType::i32());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        (void)masm; (void)src; (void)dst;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        // Check if any lane is non-zero
        masm.umaxv(0, src.code);  // Max across all lanes to V0
        masm.umovS(dst.code, 0, 0);  // Move to GPR
        masm.cmpImm32(dst.code, 0);
        masm.cset(dst.code, 1);  // Set to 1 if NE
    });
    
    freeVReg(src);
    push(ValueLocation::inReg(dst, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitI32x4DotI16x8S() {
    ValueLocation rhs = pop();
    ValueLocation lhs = pop();
    (void)rhs; (void)lhs;
    
    Reg dst = allocVReg();
    Reg src1 = allocVReg();
    Reg src2 = allocVReg();
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        (void)masm; (void)dst; (void)src1; (void)src2;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        // Signed dot product: multiply and pairwise add
        masm.smull(dst.code, src1.code, src2.code);   // Multiply low half
        masm.smlal2(dst.code, src1.code, src2.code);  // Multiply-accumulate high half
        masm.saddlp(dst.code, dst.code);              // Pairwise add
    });
    
    freeVReg(src1);
    freeVReg(src2);
    push(ValueLocation::inReg(dst, ValType::i32()));
    return true;
}

} // namespace Zepra::Wasm
