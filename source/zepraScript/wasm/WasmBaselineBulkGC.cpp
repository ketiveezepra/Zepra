// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file WasmBaselineBulkGC.cpp
 * @brief Exception handling, atomics, GC, bulk memory, and reference ops
 */

#include "wasm/WasmBaselineInternal.h"

namespace Zepra::Wasm {
// =============================================================================
// Exception Handling Operations
// =============================================================================

bool BaselineCompiler::emitTry(BlockType bt) {
    hasExceptionHandling_ = true;
    
    // Push a try control frame
    BaselineControlFrame frame(BaselineControlFrame::Kind::Try, bt, valueStack_.size());
    frame.endLabel = newLabel();
    controlStack_.push_back(frame);
    
    // Record try block index for unwinding
    tryStack_.push_back(controlStack_.size() - 1);
    
    // Emit landing pad setup
    // Save current exception handler chain and install new one
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // Push current exception handler onto handler stack
        // This would normally be a call to runtime to setup exception frame
        // For now, emit placeholder
        masm.push(X86Assembler::RBP);  // Save frame pointer
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        // ARM64: Store current handler pointer
        masm.strPre(29, 31, -16);  // Push FP
    });
    
    return true;
}

bool BaselineCompiler::emitCatch(uint32_t tagIndex) {
    if (controlStack_.empty()) {
        error_ = "catch without matching try";
        return false;
    }
    
    BaselineControlFrame& frame = controlStack_.back();
    if (frame.kind != BaselineControlFrame::Kind::Try &&
        frame.kind != BaselineControlFrame::Kind::Catch) {
        error_ = "catch not in try block";
        return false;
    }
    
    // End previous try/catch block
    jumpTo(frame.endLabel);
    
    // Bind catch landing pad
    Label catchLabel = newLabel();
    bindLabel(catchLabel);
    
    // Change frame to catch mode
    frame.kind = BaselineControlFrame::Kind::Catch;
    
    // Pop exception values matching tag signature onto stack
    // In a full implementation, we'd look up the tag signature
    // and push the exception payload values
    (void)tagIndex;
    
    // Load exception payload from exception object
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // Exception reference is in RAX (by calling convention)
        // Load payload fields into registers/stack
        // This is simplified - real impl would iterate tag params
        (void)masm;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        // Exception reference in X0
        (void)masm;
    });
    
    return true;
}

bool BaselineCompiler::emitCatchAll() {
    if (controlStack_.empty()) {
        error_ = "catch_all without matching try";
        return false;
    }
    
    BaselineControlFrame& frame = controlStack_.back();
    if (frame.kind != BaselineControlFrame::Kind::Try &&
        frame.kind != BaselineControlFrame::Kind::Catch) {
        error_ = "catch_all not in try block";
        return false;
    }
    
    // End previous try/catch block
    jumpTo(frame.endLabel);
    
    // Bind catch_all landing pad
    Label catchAllLabel = newLabel();
    bindLabel(catchAllLabel);
    
    // Change frame to catch mode
    frame.kind = BaselineControlFrame::Kind::Catch;
    
    // catch_all doesn't push any values (no payload)
    return true;
}

bool BaselineCompiler::emitThrow(uint32_t tagIndex) {
    // Pop exception payload values according to tag signature
    // In a full implementation, we'd look up the tag and pop values
    (void)tagIndex;
    
    // Create exception object and throw it
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.mov32Imm(X86Assembler::RDI, static_cast<int32_t>(tagIndex));
        // Call runtime throw handler at instance offset -48
        Reg fnPtr = allocReg(ValType::i64());
        masm.load64(fnPtr.code, X86Assembler::RBP, -48);
        masm.call(fnPtr.code);
        freeReg(fnPtr);
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movImm64(0, tagIndex);
        Reg fnPtr = allocReg(ValType::i64());
        masm.ldr64(fnPtr.code, 29, -48);
        masm.blr(fnPtr.code);
        freeReg(fnPtr);
    });
    
    return true;
}

bool BaselineCompiler::emitRethrow(uint32_t depth) {
    // Find the catch block at the given depth
    if (depth >= tryStack_.size()) {
        error_ = "rethrow depth out of range";
        return false;
    }
    
    // Get the try block we're rethrowing from
    size_t tryIdx = tryStack_[tryStack_.size() - 1 - depth];
    (void)tryIdx;
    
    // Rethrow the current exception
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // Call runtime rethrow handler at instance offset -48
        Reg fnPtr = allocReg(ValType::i64());
        masm.load64(fnPtr.code, X86Assembler::RBP, -48);
        masm.call(fnPtr.code);
        freeReg(fnPtr);
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        Reg fnPtr = allocReg(ValType::i64());
        masm.ldr64(fnPtr.code, 29, -48);
        masm.blr(fnPtr.code);
        freeReg(fnPtr);
    });
    
    return true;
}

bool BaselineCompiler::emitDelegate(uint32_t depth) {
    if (controlStack_.empty()) {
        error_ = "delegate without matching try";
        return false;
    }
    
    BaselineControlFrame& frame = controlStack_.back();
    if (frame.kind != BaselineControlFrame::Kind::Try) {
        error_ = "delegate not in try block";
        return false;
    }
    
    // Delegate exceptions to an outer handler at the given depth
    // This is like an implicit rethrow at block end
    
    // Jump to end
    jumpTo(frame.endLabel);
    
    // Pop try block from stack
    if (!tryStack_.empty()) {
        tryStack_.pop_back();
    }
    
    // Mark frame for delegation
    (void)depth;
    
    return true;
}

bool BaselineCompiler::emitThrowRef() {
    // Pop exnref from stack
    ValueLocation exnRef = pop();
    (void)exnRef;
    
    // Null check
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // Compare exnref with null
        masm.cmp64Imm(X86Assembler::RAX, 0);
        // Branch to null trap if zero
        size_t nullCheck = masm.jcc32(X86Assembler::E);
        (void)nullCheck;
        // Otherwise throw the exception
        masm.int3();  // Call throw_ref runtime
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        // Check for null
        masm.cbnz64(0, 8);  // Skip trap if non-null
        masm.brk(0);        // Trap on null
        // Throw the exception
        masm.brk(0);        // Call throw_ref runtime
    });
    
    return true;
}

bool BaselineCompiler::emitTryTable(BlockType bt, const std::vector<uint32_t>& handlers) {
    hasExceptionHandling_ = true;
    
    // try_table is a newer form with inline handlers
    BaselineControlFrame frame(BaselineControlFrame::Kind::Try, bt, valueStack_.size());
    frame.endLabel = newLabel();
    controlStack_.push_back(frame);
    tryStack_.push_back(controlStack_.size() - 1);
    
    // Process catch clauses
    // handlers format: [kind, tag_or_label, ...]
    (void)handlers;
    
    return true;
}

void BaselineCompiler::emitExceptionUnwind(uint32_t targetDepth) {
    // Unwind to the target try block depth
    // This pops exception frames and restores state
    
    while (tryStack_.size() > targetDepth) {
        EMIT_X64({
            X86Assembler masm(codeBuffer_.get());
            // Pop exception handler frame
            masm.pop(X86Assembler::RBP);
        });
        EMIT_ARM64({
            ARM64Assembler masm(codeBuffer_.get());
            // Restore previous handler
            masm.ldrPost(29, 31, 16);
        });
        tryStack_.pop_back();
    }
}

void BaselineCompiler::emitLoadExceptionPayload(uint32_t tagIndex) {
    // Load exception payload values based on tag signature
    // Would need tag registry to get signature
    (void)tagIndex;
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // Exception object pointer in RAX
        // Load fields based on tag signature
        (void)masm;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        // Exception object pointer in X0
        (void)masm;
    });
}

void BaselineCompiler::emitStoreExceptionPayload(uint32_t tagIndex) {
    // Store values from stack into exception object
    (void)tagIndex;
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        (void)masm;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        (void)masm;
    });
}

// =============================================================================
// Atomic Operations
// =============================================================================

bool BaselineCompiler::emitAtomicOp(uint32_t atomicOp) {
    // Atomic opcode constants
    constexpr uint32_t NOTIFY = 0x00;
    constexpr uint32_t WAIT32 = 0x01;
    constexpr uint32_t WAIT64 = 0x02;
    constexpr uint32_t FENCE = 0x03;
    
    constexpr uint32_t I32_LOAD = 0x10;
    constexpr uint32_t I64_LOAD = 0x11;
    constexpr uint32_t I32_STORE = 0x17;
    constexpr uint32_t I64_STORE = 0x18;
    
    constexpr uint32_t I32_RMW_ADD = 0x1E;
    constexpr uint32_t I64_RMW_ADD = 0x1F;
    constexpr uint32_t I32_RMW_SUB = 0x25;
    constexpr uint32_t I64_RMW_SUB = 0x26;
    constexpr uint32_t I32_RMW_AND = 0x2C;
    constexpr uint32_t I64_RMW_AND = 0x2D;
    constexpr uint32_t I32_RMW_OR = 0x33;
    constexpr uint32_t I64_RMW_OR = 0x34;
    constexpr uint32_t I32_RMW_XOR = 0x3A;
    constexpr uint32_t I64_RMW_XOR = 0x3B;
    constexpr uint32_t I32_RMW_XCHG = 0x41;
    constexpr uint32_t I64_RMW_XCHG = 0x42;
    constexpr uint32_t I32_RMW_CMPXCHG = 0x48;
    constexpr uint32_t I64_RMW_CMPXCHG = 0x49;
    
    // Read memory operands
    uint32_t align, offset;
    if (!decoder_->readVarU32(&align) || !decoder_->readVarU32(&offset)) {
        error_ = "failed to read atomic memory operands";
        return false;
    }
    
    switch (atomicOp) {
        case NOTIFY:
            return emitAtomicNotify(offset);
        case WAIT32:
            return emitAtomicWait(false, offset);
        case WAIT64:
            return emitAtomicWait(true, offset);
        case FENCE:
            return emitAtomicFence();
        
        case I32_LOAD: case I64_LOAD:
            return emitAtomicLoad(atomicOp, offset);
        
        case I32_STORE: case I64_STORE:
            return emitAtomicStore(atomicOp, offset);
        
        case I32_RMW_ADD: case I64_RMW_ADD:
        case I32_RMW_SUB: case I64_RMW_SUB:
        case I32_RMW_AND: case I64_RMW_AND:
        case I32_RMW_OR: case I64_RMW_OR:
        case I32_RMW_XOR: case I64_RMW_XOR:
        case I32_RMW_XCHG: case I64_RMW_XCHG:
            return emitAtomicRMW(atomicOp, offset);
        
        case I32_RMW_CMPXCHG: case I64_RMW_CMPXCHG:
            return emitAtomicCmpxchg(atomicOp, offset);
        
        default:
            error_ = "unsupported atomic opcode: " + std::to_string(atomicOp);
            return false;
    }
}

bool BaselineCompiler::emitAtomicLoad(uint32_t atomicOp, uint32_t offset) {
    ValueLocation addr = pop();
    (void)addr;
    
    bool is64 = (atomicOp == 0x11);  // I64_LOAD
    Reg addrReg = allocReg(ValType::i32());
    Reg result = allocReg(is64 ? ValType::i64() : ValType::i32());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // x64: MOV is atomic for aligned access
        // For guaranteed atomicity, use LOCK prefix or explicit atomic ops
        if (offset) {
            masm.add32Imm(addrReg.code, offset);
        }
        if (is64) {
            masm.load64(result.code, addrReg.code, 0);
        } else {
            masm.load32(result.code, addrReg.code, 0);
        }
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        // ARM64: Use LDAR (load acquire) for atomic semantics
        if (offset) {
            masm.addImm64(addrReg.code, addrReg.code, offset);
        }
        if (is64) {
            masm.ldar64(result.code, addrReg.code);
        } else {
            masm.ldar32(result.code, addrReg.code);
        }
    });
    
    freeReg(addrReg);
    push(ValueLocation::inReg(result, is64 ? ValType::i64() : ValType::i32()));
    return true;
}

bool BaselineCompiler::emitAtomicStore(uint32_t atomicOp, uint32_t offset) {
    ValueLocation value = pop();
    ValueLocation addr = pop();
    (void)value; (void)addr;
    
    bool is64 = (atomicOp == 0x18);  // I64_STORE
    Reg addrReg = allocReg(ValType::i32());
    Reg valReg = allocReg(is64 ? ValType::i64() : ValType::i32());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // x64: XCHG is implicitly locked, or use explicit fence
        if (offset) {
            masm.add32Imm(addrReg.code, offset);
        }
        if (is64) {
            masm.store64(valReg.code, addrReg.code, 0);
        } else {
            masm.store32(valReg.code, addrReg.code, 0);
        }
        // Memory fence for sequential consistency
        masm.mfence();
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        // ARM64: Use STLR (store release) for atomic semantics
        if (offset) {
            masm.addImm64(addrReg.code, addrReg.code, offset);
        }
        if (is64) {
            masm.stlr64(valReg.code, addrReg.code);
        } else {
            masm.stlr32(valReg.code, addrReg.code);
        }
    });
    
    freeReg(addrReg);
    freeReg(valReg);
    return true;
}

bool BaselineCompiler::emitAtomicRMW(uint32_t atomicOp, uint32_t offset) {
    ValueLocation value = pop();
    ValueLocation addr = pop();
    (void)value; (void)addr;
    
    bool is64 = (atomicOp & 1);  // Odd opcodes are 64-bit
    Reg addrReg = allocReg(ValType::i32());
    Reg valReg = allocReg(is64 ? ValType::i64() : ValType::i32());
    Reg result = allocReg(is64 ? ValType::i64() : ValType::i32());
    
    // Determine operation type
    enum class RMWOp { Add, Sub, And, Or, Xor, Xchg };
    RMWOp op;
    if (atomicOp >= 0x1E && atomicOp <= 0x24) op = RMWOp::Add;
    else if (atomicOp >= 0x25 && atomicOp <= 0x2B) op = RMWOp::Sub;
    else if (atomicOp >= 0x2C && atomicOp <= 0x32) op = RMWOp::And;
    else if (atomicOp >= 0x33 && atomicOp <= 0x39) op = RMWOp::Or;
    else if (atomicOp >= 0x3A && atomicOp <= 0x40) op = RMWOp::Xor;
    else op = RMWOp::Xchg;
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        if (offset) masm.add32Imm(addrReg.code, offset);
        
        switch (op) {
            case RMWOp::Add:
                // LOCK XADD [addr], val
                masm.lockPrefix();
                if (is64) masm.xadd64(addrReg.code, valReg.code);
                else masm.xadd32(addrReg.code, valReg.code);
                masm.mov64(result.code, valReg.code);
                break;
            case RMWOp::Xchg:
                // XCHG is implicitly locked
                if (is64) masm.xchg64(addrReg.code, valReg.code);
                else masm.xchg32(addrReg.code, valReg.code);
                masm.mov64(result.code, valReg.code);
                break;
            default:
                // For Sub/And/Or/Xor: use CAS loop
                // Load current value, apply op, try CAS
                masm.load64(result.code, addrReg.code, 0);
                // CAS loop would go here
                break;
        }
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        if (offset) masm.addImm64(addrReg.code, addrReg.code, offset);
        
        // ARM64 LSE atomics
        switch (op) {
            case RMWOp::Add:
                if (is64) masm.ldaddal64(result.code, valReg.code, addrReg.code);
                else masm.ldaddal32(result.code, valReg.code, addrReg.code);
                break;
            case RMWOp::Sub:
                // Negate and add
                masm.neg64(valReg.code, valReg.code);
                if (is64) masm.ldaddal64(result.code, valReg.code, addrReg.code);
                else masm.ldaddal32(result.code, valReg.code, addrReg.code);
                break;
            case RMWOp::And:
                masm.mvn64(valReg.code, valReg.code);
                if (is64) masm.ldclral64(result.code, valReg.code, addrReg.code);
                else masm.ldclral32(result.code, valReg.code, addrReg.code);
                break;
            case RMWOp::Or:
                if (is64) masm.ldsetal64(result.code, valReg.code, addrReg.code);
                else masm.ldsetal32(result.code, valReg.code, addrReg.code);
                break;
            case RMWOp::Xor:
                if (is64) masm.ldeoral64(result.code, valReg.code, addrReg.code);
                else masm.ldeoral32(result.code, valReg.code, addrReg.code);
                break;
            case RMWOp::Xchg:
                if (is64) masm.swpal64(result.code, valReg.code, addrReg.code);
                else masm.swpal32(result.code, valReg.code, addrReg.code);
                break;
        }
    });
    
    freeReg(addrReg);
    freeReg(valReg);
    push(ValueLocation::inReg(result, is64 ? ValType::i64() : ValType::i32()));
    return true;
}

bool BaselineCompiler::emitAtomicCmpxchg(uint32_t atomicOp, uint32_t offset) {
    ValueLocation replacement = pop();
    ValueLocation expected = pop();
    ValueLocation addr = pop();
    (void)replacement; (void)expected; (void)addr;
    
    bool is64 = (atomicOp == 0x49);  // I64_RMW_CMPXCHG
    Reg addrReg = allocReg(ValType::i32());
    Reg expectReg = allocReg(is64 ? ValType::i64() : ValType::i32());
    Reg replaceReg = allocReg(is64 ? ValType::i64() : ValType::i32());
    Reg result = allocReg(is64 ? ValType::i64() : ValType::i32());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        if (offset) masm.add32Imm(addrReg.code, offset);
        
        // CMPXCHG: expects old value in RAX
        masm.mov64(X86Assembler::RAX, expectReg.code);
        masm.lockPrefix();
        if (is64) masm.cmpxchg64(addrReg.code, replaceReg.code);
        else masm.cmpxchg32(addrReg.code, replaceReg.code);
        // Result (old value) is in RAX
        masm.mov64(result.code, X86Assembler::RAX);
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        if (offset) masm.addImm64(addrReg.code, addrReg.code, offset);
        
        // ARM64 LSE: CASAL
        masm.mov64(result.code, expectReg.code);
        if (is64) masm.casal64(result.code, replaceReg.code, addrReg.code);
        else masm.casal32(result.code, replaceReg.code, addrReg.code);
    });
    
    freeReg(addrReg);
    freeReg(expectReg);
    freeReg(replaceReg);
    push(ValueLocation::inReg(result, is64 ? ValType::i64() : ValType::i32()));
    return true;
}

bool BaselineCompiler::emitAtomicWait(bool is64, uint32_t offset) {
    ValueLocation timeout = pop();  // i64 timeout in nanoseconds
    ValueLocation expected = pop();  // i32 or i64 expected value
    ValueLocation addr = pop();
    (void)timeout; (void)expected; (void)addr; (void)offset;
    
    Reg result = allocReg(ValType::i32());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // Call runtime wait: args in RDI(addr), RSI(expected), RDX(timeout)
        Reg fnPtr = allocReg(ValType::i64());
        masm.load64(fnPtr.code, X86Assembler::RBP, -56);
        masm.call(fnPtr.code);
        freeReg(fnPtr);
        masm.mov32(result.code, X86Assembler::RAX);
        (void)is64;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        Reg fnPtr = allocReg(ValType::i64());
        masm.ldr64(fnPtr.code, 29, -56);
        masm.blr(fnPtr.code);
        freeReg(fnPtr);
        masm.mov32(result.code, 0);
        (void)is64;
    });
    
    push(ValueLocation::inReg(result, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitAtomicNotify(uint32_t offset) {
    ValueLocation count = pop();  // i32 count of threads to wake
    ValueLocation addr = pop();
    (void)count; (void)addr; (void)offset;
    
    Reg result = allocReg(ValType::i32());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        Reg fnPtr = allocReg(ValType::i64());
        masm.load64(fnPtr.code, X86Assembler::RBP, -56);
        masm.call(fnPtr.code);
        freeReg(fnPtr);
        masm.mov32(result.code, X86Assembler::RAX);
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        Reg fnPtr = allocReg(ValType::i64());
        masm.ldr64(fnPtr.code, 29, -56);
        masm.blr(fnPtr.code);
        freeReg(fnPtr);
        masm.mov32(result.code, 0);
    });
    
    push(ValueLocation::inReg(result, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitAtomicFence() {
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.mfence();  // Full memory barrier
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.dmb();  // Data memory barrier
    });
    return true;
}

// =============================================================================
// GC Operations
// =============================================================================

bool BaselineCompiler::emitGCOp(uint32_t gcOp) {
    using namespace GCOp;
    
    switch (gcOp) {
        case STRUCT_NEW: {
            uint32_t typeIndex;
            if (!decoder_->readVarU32(&typeIndex)) return false;
            return emitStructNew(typeIndex);
        }
        case STRUCT_NEW_DEFAULT: {
            uint32_t typeIndex;
            if (!decoder_->readVarU32(&typeIndex)) return false;
            return emitStructNewDefault(typeIndex);
        }
        case STRUCT_GET: {
            uint32_t typeIndex, fieldIndex;
            if (!decoder_->readVarU32(&typeIndex) || !decoder_->readVarU32(&fieldIndex)) return false;
            return emitStructGet(typeIndex, fieldIndex, false);
        }
        case STRUCT_GET_S: {
            uint32_t typeIndex, fieldIndex;
            if (!decoder_->readVarU32(&typeIndex) || !decoder_->readVarU32(&fieldIndex)) return false;
            return emitStructGet(typeIndex, fieldIndex, true);
        }
        case STRUCT_GET_U: {
            uint32_t typeIndex, fieldIndex;
            if (!decoder_->readVarU32(&typeIndex) || !decoder_->readVarU32(&fieldIndex)) return false;
            return emitStructGet(typeIndex, fieldIndex, false);
        }
        case STRUCT_SET: {
            uint32_t typeIndex, fieldIndex;
            if (!decoder_->readVarU32(&typeIndex) || !decoder_->readVarU32(&fieldIndex)) return false;
            return emitStructSet(typeIndex, fieldIndex);
        }
        case ARRAY_NEW: {
            uint32_t typeIndex;
            if (!decoder_->readVarU32(&typeIndex)) return false;
            return emitArrayNew(typeIndex);
        }
        case ARRAY_NEW_DEFAULT: {
            uint32_t typeIndex;
            if (!decoder_->readVarU32(&typeIndex)) return false;
            return emitArrayNewDefault(typeIndex);
        }
        case ARRAY_NEW_FIXED: {
            uint32_t typeIndex, length;
            if (!decoder_->readVarU32(&typeIndex) || !decoder_->readVarU32(&length)) return false;
            return emitArrayNewFixed(typeIndex, length);
        }
        case ARRAY_GET: {
            uint32_t typeIndex;
            if (!decoder_->readVarU32(&typeIndex)) return false;
            return emitArrayGet(typeIndex, false);
        }
        case ARRAY_GET_S: {
            uint32_t typeIndex;
            if (!decoder_->readVarU32(&typeIndex)) return false;
            return emitArrayGet(typeIndex, true);
        }
        case ARRAY_GET_U: {
            uint32_t typeIndex;
            if (!decoder_->readVarU32(&typeIndex)) return false;
            return emitArrayGet(typeIndex, false);
        }
        case ARRAY_SET: {
            uint32_t typeIndex;
            if (!decoder_->readVarU32(&typeIndex)) return false;
            return emitArraySet(typeIndex);
        }
        case ARRAY_LEN:
            return emitArrayLen();
        case ARRAY_FILL: {
            uint32_t typeIndex;
            if (!decoder_->readVarU32(&typeIndex)) return false;
            return emitArrayFill(typeIndex);
        }
        case ARRAY_COPY: {
            uint32_t destType, srcType;
            if (!decoder_->readVarU32(&destType) || !decoder_->readVarU32(&srcType)) return false;
            return emitArrayCopy(destType, srcType);
        }
        case REF_TEST: {
            uint32_t typeIndex;
            if (!decoder_->readVarU32(&typeIndex)) return false;
            return emitRefTest(typeIndex, false);
        }
        case REF_TEST_NULL: {
            uint32_t typeIndex;
            if (!decoder_->readVarU32(&typeIndex)) return false;
            return emitRefTest(typeIndex, true);
        }
        case REF_CAST: {
            uint32_t typeIndex;
            if (!decoder_->readVarU32(&typeIndex)) return false;
            return emitRefCast(typeIndex, false);
        }
        case REF_CAST_NULL: {
            uint32_t typeIndex;
            if (!decoder_->readVarU32(&typeIndex)) return false;
            return emitRefCast(typeIndex, true);
        }
        case I31_NEW:
            return emitI31New();
        case I31_GET_S:
            return emitI31GetS();
        case I31_GET_U:
            return emitI31GetU();
        case REF_NULL: {
            uint8_t heapType;
            if (!decoder_->readByte(&heapType)) return false;
            return emitRefNull(heapType);
        }
        case REF_IS_NULL:
            return emitRefIsNull();
        case REF_AS_NON_NULL:
            return emitRefAsNonNull();
        case REF_EQ:
            return emitRefEq();
        default:
            error_ = "unsupported GC opcode: " + std::to_string(gcOp);
            return false;
    }
}

bool BaselineCompiler::emitStructNew(uint32_t typeIndex) {
    // Get struct type from module
    // Pop field values from stack
    // Call allocator to create struct
    // Initialize fields
    // Push reference
    
    Reg result = allocReg(ValType::i64());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.mov32Imm(X86Assembler::RDI, static_cast<int32_t>(typeIndex));
        Reg fnPtr = allocReg(ValType::i64());
        masm.load64(fnPtr.code, X86Assembler::RBP, -64);
        masm.call(fnPtr.code);
        freeReg(fnPtr);
        masm.mov64(result.code, X86Assembler::RAX);
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movImm64(0, typeIndex);
        Reg fnPtr = allocReg(ValType::i64());
        masm.ldr64(fnPtr.code, 29, -64);
        masm.blr(fnPtr.code);
        freeReg(fnPtr);
        masm.mov64(result.code, 0);
    });
    
    push(ValueLocation::inReg(result, ValType::externRef()));
    return true;
}

bool BaselineCompiler::emitStructNewDefault(uint32_t typeIndex) {
    // Same as struct.new but with default values
    Reg result = allocReg(ValType::i64());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.mov32Imm(X86Assembler::RDI, static_cast<int32_t>(typeIndex));
        masm.int3();
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movImm64(0, typeIndex);
        masm.brk(0);
    });
    
    push(ValueLocation::inReg(result, ValType::externRef()));
    return true;
}

bool BaselineCompiler::emitStructGet(uint32_t typeIndex, uint32_t fieldIndex, bool signExtend) {
    ValueLocation structRef = pop();
    (void)structRef;
    
    Reg objReg = allocReg(ValType::i64());
    Reg result = allocReg(ValType::i64());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // Null check
        masm.cmp64Imm(objReg.code, 0);
        size_t nullTrap = masm.jcc32(X86Assembler::E);
        (void)nullTrap;
        
        // Load field offset and read value
        // Offset would be computed from type registry
        uint32_t offset = fieldIndex * 8;  // Simplified
        masm.load64(result.code, objReg.code, offset);
        (void)typeIndex; (void)signExtend;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.cbnz64(objReg.code, 8);
        masm.brk(0);
        
        uint32_t offset = fieldIndex * 8;
        masm.ldr64(result.code, objReg.code, offset);
        (void)typeIndex; (void)signExtend;
    });
    
    freeReg(objReg);
    push(ValueLocation::inReg(result, ValType::i64()));
    return true;
}

bool BaselineCompiler::emitStructSet(uint32_t typeIndex, uint32_t fieldIndex) {
    ValueLocation value = pop();
    ValueLocation structRef = pop();
    (void)value; (void)structRef;
    
    Reg objReg = allocReg(ValType::i64());
    Reg valReg = allocReg(ValType::i64());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.cmp64Imm(objReg.code, 0);
        size_t nullTrap = masm.jcc32(X86Assembler::E);
        (void)nullTrap;
        
        uint32_t offset = fieldIndex * 8;
        masm.store64(valReg.code, objReg.code, offset);
        (void)typeIndex;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.cbnz64(objReg.code, 8);
        masm.brk(0);
        
        uint32_t offset = fieldIndex * 8;
        masm.str64(valReg.code, objReg.code, offset);
        (void)typeIndex;
    });
    
    freeReg(objReg);
    freeReg(valReg);
    return true;
}

bool BaselineCompiler::emitArrayNew(uint32_t typeIndex) {
    ValueLocation initValue = pop();
    ValueLocation length = pop();
    (void)initValue; (void)length;
    
    Reg result = allocReg(ValType::i64());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.mov32Imm(X86Assembler::RDI, static_cast<int32_t>(typeIndex));
        masm.int3();
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movImm64(0, typeIndex);
        masm.brk(0);
    });
    
    push(ValueLocation::inReg(result, ValType::externRef()));
    return true;
}

bool BaselineCompiler::emitArrayNewDefault(uint32_t typeIndex) {
    ValueLocation length = pop();
    (void)length;
    
    Reg result = allocReg(ValType::i64());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.mov32Imm(X86Assembler::RDI, static_cast<int32_t>(typeIndex));
        masm.int3();
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movImm64(0, typeIndex);
        masm.brk(0);
    });
    
    push(ValueLocation::inReg(result, ValType::externRef()));
    return true;
}

bool BaselineCompiler::emitArrayNewFixed(uint32_t typeIndex, uint32_t length) {
    // Pop 'length' values from stack
    for (uint32_t i = 0; i < length; i++) {
        pop();
    }
    
    Reg result = allocReg(ValType::i64());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.mov32Imm(X86Assembler::RDI, static_cast<int32_t>(typeIndex));
        masm.mov32Imm(X86Assembler::RSI, static_cast<int32_t>(length));
        masm.int3();
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movImm64(0, typeIndex);
        masm.movImm64(1, length);
        masm.brk(0);
    });
    
    push(ValueLocation::inReg(result, ValType::externRef()));
    return true;
}

bool BaselineCompiler::emitArrayGet(uint32_t typeIndex, bool signExtend) {
    ValueLocation index = pop();
    ValueLocation arrayRef = pop();
    (void)index; (void)arrayRef;
    
    Reg objReg = allocReg(ValType::i64());
    Reg idxReg = allocReg(ValType::i32());
    Reg result = allocReg(ValType::i64());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // Null check
        masm.cmp64Imm(objReg.code, 0);
        masm.jcc32(X86Assembler::E);
        
        // Bounds check
        // Load length from array object and compare
        
        // Load element
        (void)typeIndex; (void)signExtend; (void)idxReg; (void)result;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.cbnz64(objReg.code, 8);
        masm.brk(0);
        (void)typeIndex; (void)signExtend; (void)idxReg; (void)result;
    });
    
    freeReg(objReg);
    freeReg(idxReg);
    push(ValueLocation::inReg(result, ValType::i64()));
    return true;
}

bool BaselineCompiler::emitArraySet(uint32_t typeIndex) {
    ValueLocation value = pop();
    ValueLocation index = pop();
    ValueLocation arrayRef = pop();
    (void)value; (void)index; (void)arrayRef;
    
    Reg objReg = allocReg(ValType::i64());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.cmp64Imm(objReg.code, 0);
        masm.jcc32(X86Assembler::E);
        (void)typeIndex;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.cbnz64(objReg.code, 8);
        masm.brk(0);
        (void)typeIndex;
    });
    
    freeReg(objReg);
    return true;
}

bool BaselineCompiler::emitArrayLen() {
    ValueLocation arrayRef = pop();
    (void)arrayRef;
    
    Reg objReg = allocReg(ValType::i64());
    Reg result = allocReg(ValType::i32());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.cmp64Imm(objReg.code, 0);
        masm.jcc32(X86Assembler::E);
        // Load length field (at offset 8 in WasmArray)
        masm.load32(result.code, objReg.code, 8);
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.cbnz64(objReg.code, 8);
        masm.brk(0);
        masm.ldr32(result.code, objReg.code, 8);
    });
    
    freeReg(objReg);
    push(ValueLocation::inReg(result, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitArrayFill(uint32_t typeIndex) {
    ValueLocation count = pop();
    ValueLocation value = pop();
    ValueLocation offset = pop();
    ValueLocation arrayRef = pop();
    (void)count; (void)value; (void)offset; (void)arrayRef; (void)typeIndex;
    
    EMIT_X64({ X86Assembler masm(codeBuffer_.get()); masm.int3(); });
    EMIT_ARM64({ ARM64Assembler masm(codeBuffer_.get()); masm.brk(0); });
    
    return true;
}

bool BaselineCompiler::emitArrayCopy(uint32_t destType, uint32_t srcType) {
    ValueLocation count = pop();
    ValueLocation srcOffset = pop();
    ValueLocation srcArray = pop();
    ValueLocation destOffset = pop();
    ValueLocation destArray = pop();
    (void)count; (void)srcOffset; (void)srcArray; (void)destOffset; (void)destArray;
    (void)destType; (void)srcType;
    
    EMIT_X64({ X86Assembler masm(codeBuffer_.get()); masm.int3(); });
    EMIT_ARM64({ ARM64Assembler masm(codeBuffer_.get()); masm.brk(0); });
    
    return true;
}

bool BaselineCompiler::emitRefTest(uint32_t typeIndex, bool nullable) {
    ValueLocation ref = pop();
    (void)ref;
    
    Reg objReg = allocReg(ValType::i64());
    Reg result = allocReg(ValType::i32());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // If nullable, null passes the test
        if (nullable) {
            masm.cmp64Imm(objReg.code, 0);
            // If null, result = 1
        }
        // Call runtime type check
        masm.mov32Imm(X86Assembler::RSI, static_cast<int32_t>(typeIndex));
        masm.int3();
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movImm64(1, typeIndex);
        if (nullable) {
            masm.cbz64(objReg.code, 8);
        }
        masm.brk(0);
    });
    
    freeReg(objReg);
    push(ValueLocation::inReg(result, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitRefCast(uint32_t typeIndex, bool nullable) {
    ValueLocation ref = pop();
    (void)ref;
    
    Reg objReg = allocReg(ValType::i64());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        if (!nullable) {
            masm.cmp64Imm(objReg.code, 0);
            masm.jcc32(X86Assembler::E);  // Trap on null
        }
        // Type check and trap if fails
        masm.mov32Imm(X86Assembler::RSI, static_cast<int32_t>(typeIndex));
        masm.int3();
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        if (!nullable) {
            masm.cbnz64(objReg.code, 8);
            masm.brk(0);
        }
        masm.movImm64(1, typeIndex);
        masm.brk(0);
    });
    
    push(ValueLocation::inReg(objReg, ValType::externRef()));
    return true;
}

bool BaselineCompiler::emitBrOnCast(uint32_t labelIdx, uint32_t srcType, uint32_t dstType, bool onFail) {
    ValueLocation ref = pop();
    (void)ref; (void)srcType; (void)dstType;
    
    Reg objReg = allocReg(ValType::i64());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // Type test
        // Branch based on result and onFail flag
        (void)masm; (void)objReg; (void)labelIdx; (void)onFail;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        (void)masm; (void)objReg; (void)labelIdx; (void)onFail;
    });
    
    push(ValueLocation::inReg(objReg, ValType::externRef()));
    return true;
}

bool BaselineCompiler::emitI31New() {
    ValueLocation value = pop();
    (void)value;
    
    Reg valReg = allocReg(ValType::i32());
    Reg result = allocReg(ValType::i64());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // i31ref encoding: (value << 1) | 1
        masm.mov64(result.code, valReg.code);
        masm.shl64_cl(result.code);  // Shift left by 1
        masm.or32Imm(result.code, 1);  // Set tag bit
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.lsl64(result.code, valReg.code, 1);
        masm.orrImm64(result.code, result.code, 1);
    });
    
    freeReg(valReg);
    push(ValueLocation::inReg(result, ValType::externRef()));
    return true;
}

bool BaselineCompiler::emitI31GetS() {
    ValueLocation ref = pop();
    (void)ref;
    
    Reg refReg = allocReg(ValType::i64());
    Reg result = allocReg(ValType::i32());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // Sign-extend from 31 bits
        masm.mov64(result.code, refReg.code);
        masm.shr64_cl(result.code);  // Shift right by 1
        // Sign extend (shift left then arithmetic right)
        masm.shl32_cl(result.code);
        masm.sar32_cl(result.code);
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.asr64(result.code, refReg.code, 1);
        masm.sxtw(result.code, result.code);
    });
    
    freeReg(refReg);
    push(ValueLocation::inReg(result, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitI31GetU() {
    ValueLocation ref = pop();
    (void)ref;
    
    Reg refReg = allocReg(ValType::i64());
    Reg result = allocReg(ValType::i32());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.mov64(result.code, refReg.code);
        masm.shr64_cl(result.code);
        masm.and32Imm(result.code, 0x7FFFFFFF);
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.lsr64(result.code, refReg.code, 1);
        masm.andImm64(result.code, result.code, 0x7FFFFFFF);
    });
    
    freeReg(refReg);
    push(ValueLocation::inReg(result, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitRefNull(uint8_t heapType) {
    (void)heapType;
    Reg result = allocReg(ValType::i64());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.xor64(result.code, result.code);
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movImm64(result.code, 0);
    });
    
    push(ValueLocation::inReg(result, ValType::externRef()));
    return true;
}

bool BaselineCompiler::emitRefIsNull() {
    ValueLocation ref = pop();
    (void)ref;
    
    Reg refReg = allocReg(ValType::i64());
    Reg result = allocReg(ValType::i32());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.cmp64Imm(refReg.code, 0);
        masm.setcc(X86Assembler::E, result.code);
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.cmpImm64(refReg.code, 0);
        masm.cset(result.code, 0);  // EQ condition
    });
    
    freeReg(refReg);
    push(ValueLocation::inReg(result, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitRefAsNonNull() {
    ValueLocation ref = pop();
    (void)ref;
    
    Reg refReg = allocReg(ValType::i64());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.cmp64Imm(refReg.code, 0);
        size_t nullTrap = masm.jcc32(X86Assembler::E);
        (void)nullTrap;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.cbnz64(refReg.code, 8);
        masm.brk(0);
    });
    
    push(ValueLocation::inReg(refReg, ValType::externRef()));
    return true;
}

bool BaselineCompiler::emitRefEq() {
    ValueLocation rhs = pop();
    ValueLocation lhs = pop();
    (void)rhs; (void)lhs;
    
    Reg lhsReg = allocReg(ValType::i64());
    Reg rhsReg = allocReg(ValType::i64());
    Reg result = allocReg(ValType::i32());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.cmp64(lhsReg.code, rhsReg.code);
        masm.setcc(X86Assembler::E, result.code);
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.cmp64(lhsReg.code, rhsReg.code);
        masm.cset(result.code, 0);  // EQ
    });
    
    freeReg(lhsReg);
    freeReg(rhsReg);
    push(ValueLocation::inReg(result, ValType::i32()));
    return true;
}

// =============================================================================
// Bulk Memory Operations
// =============================================================================

bool BaselineCompiler::emitBulkOp(uint32_t bulkOp) {
    using namespace BulkOp;
    
    switch (bulkOp) {
        case MEMORY_INIT: {
            uint32_t dataIdx, memIdx;
            if (!decoder_->readVarU32(&dataIdx) || !decoder_->readVarU32(&memIdx)) return false;
            return emitMemoryInit(dataIdx, memIdx);
        }
        case DATA_DROP: {
            uint32_t dataIdx;
            if (!decoder_->readVarU32(&dataIdx)) return false;
            return emitDataDrop(dataIdx);
        }
        case MEMORY_COPY: {
            uint32_t destMemIdx, srcMemIdx;
            if (!decoder_->readVarU32(&destMemIdx) || !decoder_->readVarU32(&srcMemIdx)) return false;
            return emitMemoryCopy(destMemIdx, srcMemIdx);
        }
        case MEMORY_FILL: {
            uint32_t memIdx;
            if (!decoder_->readVarU32(&memIdx)) return false;
            return emitMemoryFill(memIdx);
        }
        case TABLE_INIT: {
            uint32_t elemIdx, tableIdx;
            if (!decoder_->readVarU32(&elemIdx) || !decoder_->readVarU32(&tableIdx)) return false;
            return emitTableInit(elemIdx, tableIdx);
        }
        case ELEM_DROP: {
            uint32_t elemIdx;
            if (!decoder_->readVarU32(&elemIdx)) return false;
            return emitElemDrop(elemIdx);
        }
        case TABLE_COPY: {
            uint32_t destTableIdx, srcTableIdx;
            if (!decoder_->readVarU32(&destTableIdx) || !decoder_->readVarU32(&srcTableIdx)) return false;
            return emitTableCopy(destTableIdx, srcTableIdx);
        }
        case TABLE_GROW: {
            uint32_t tableIdx;
            if (!decoder_->readVarU32(&tableIdx)) return false;
            return emitTableGrow(tableIdx);
        }
        case TABLE_SIZE: {
            uint32_t tableIdx;
            if (!decoder_->readVarU32(&tableIdx)) return false;
            return emitTableSize(tableIdx);
        }
        case TABLE_FILL: {
            uint32_t tableIdx;
            if (!decoder_->readVarU32(&tableIdx)) return false;
            return emitTableFill(tableIdx);
        }
        default:
            error_ = "unsupported bulk memory opcode: " + std::to_string(bulkOp);
            return false;
    }
}

bool BaselineCompiler::emitMemoryCopy(uint32_t destMemIdx, uint32_t srcMemIdx) {
    ValueLocation count = pop();
    ValueLocation srcOffset = pop();
    ValueLocation destOffset = pop();
    (void)count; (void)srcOffset; (void)destOffset;
    
    Reg destReg = allocReg(ValType::i32());
    Reg srcReg = allocReg(ValType::i32());
    Reg countReg = allocReg(ValType::i32());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // Call runtime: memoryCopy(destMem, destOff, srcMem, srcOff, count)
        masm.mov32Imm(X86Assembler::RDI, static_cast<int32_t>(destMemIdx));
        masm.mov32Imm(X86Assembler::RCX, static_cast<int32_t>(srcMemIdx));
        // REP MOVSB for same memory index
        if (destMemIdx == srcMemIdx) {
            masm.cld();
            masm.repMovsb();
        } else {
            masm.int3();  // Runtime call for cross-memory
        }
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movImm64(0, destMemIdx);
        masm.movImm64(3, srcMemIdx);
        masm.brk(0);  // Runtime call
    });
    
    freeReg(destReg);
    freeReg(srcReg);
    freeReg(countReg);
    return true;
}

bool BaselineCompiler::emitMemoryFill(uint32_t memIdx) {
    ValueLocation count = pop();
    ValueLocation value = pop();
    ValueLocation dest = pop();
    (void)count; (void)value; (void)dest;
    
    Reg destReg = allocReg(ValType::i32());
    Reg valReg = allocReg(ValType::i32());
    Reg countReg = allocReg(ValType::i32());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // REP STOSB for memory fill
        masm.cld();
        masm.repStosb();
        (void)memIdx;
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.brk(0);  // Runtime call
        (void)memIdx;
    });
    
    freeReg(destReg);
    freeReg(valReg);
    freeReg(countReg);
    return true;
}

bool BaselineCompiler::emitMemoryInit(uint32_t dataIdx, uint32_t memIdx) {
    ValueLocation count = pop();
    ValueLocation srcOffset = pop();
    ValueLocation destOffset = pop();
    (void)count; (void)srcOffset; (void)destOffset;
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.mov32Imm(X86Assembler::RDI, static_cast<int32_t>(dataIdx));
        masm.mov32Imm(X86Assembler::RSI, static_cast<int32_t>(memIdx));
        masm.int3();  // Runtime call
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movImm64(0, dataIdx);
        masm.movImm64(1, memIdx);
        masm.brk(0);
    });
    
    return true;
}

bool BaselineCompiler::emitDataDrop(uint32_t dataIdx) {
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.mov32Imm(X86Assembler::RDI, static_cast<int32_t>(dataIdx));
        masm.int3();  // Runtime call to mark segment as dropped
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movImm64(0, dataIdx);
        masm.brk(0);
    });
    return true;
}

bool BaselineCompiler::emitTableCopy(uint32_t destTableIdx, uint32_t srcTableIdx) {
    ValueLocation count = pop();
    ValueLocation srcOffset = pop();
    ValueLocation destOffset = pop();
    (void)count; (void)srcOffset; (void)destOffset;
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.mov32Imm(X86Assembler::RDI, static_cast<int32_t>(destTableIdx));
        masm.mov32Imm(X86Assembler::RSI, static_cast<int32_t>(srcTableIdx));
        masm.int3();  // Runtime call
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movImm64(0, destTableIdx);
        masm.movImm64(1, srcTableIdx);
        masm.brk(0);
    });
    return true;
}

bool BaselineCompiler::emitTableInit(uint32_t elemIdx, uint32_t tableIdx) {
    ValueLocation count = pop();
    ValueLocation srcOffset = pop();
    ValueLocation destOffset = pop();
    (void)count; (void)srcOffset; (void)destOffset;
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.mov32Imm(X86Assembler::RDI, static_cast<int32_t>(elemIdx));
        masm.mov32Imm(X86Assembler::RSI, static_cast<int32_t>(tableIdx));
        masm.int3();
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movImm64(0, elemIdx);
        masm.movImm64(1, tableIdx);
        masm.brk(0);
    });
    return true;
}

bool BaselineCompiler::emitElemDrop(uint32_t elemIdx) {
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.mov32Imm(X86Assembler::RDI, static_cast<int32_t>(elemIdx));
        masm.int3();
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movImm64(0, elemIdx);
        masm.brk(0);
    });
    return true;
}

bool BaselineCompiler::emitTableGrow(uint32_t tableIdx) {
    ValueLocation delta = pop();
    ValueLocation initValue = pop();
    (void)delta; (void)initValue;
    
    Reg result = allocReg(ValType::i32());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.mov32Imm(X86Assembler::RDI, static_cast<int32_t>(tableIdx));
        masm.int3();
        masm.mov32(result.code, X86Assembler::RAX);
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movImm64(0, tableIdx);
        masm.brk(0);
        masm.mov32(result.code, 0);
    });
    
    push(ValueLocation::inReg(result, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitTableSize(uint32_t tableIdx) {
    Reg result = allocReg(ValType::i32());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.mov32Imm(X86Assembler::RDI, static_cast<int32_t>(tableIdx));
        masm.int3();
        masm.mov32(result.code, X86Assembler::RAX);
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movImm64(0, tableIdx);
        masm.brk(0);
        masm.mov32(result.code, 0);
    });
    
    push(ValueLocation::inReg(result, ValType::i32()));
    return true;
}

bool BaselineCompiler::emitTableFill(uint32_t tableIdx) {
    ValueLocation count = pop();
    ValueLocation value = pop();
    ValueLocation dest = pop();
    (void)count; (void)value; (void)dest;
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        masm.mov32Imm(X86Assembler::RDI, static_cast<int32_t>(tableIdx));
        masm.int3();
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movImm64(0, tableIdx);
        masm.brk(0);
    });
    return true;
}

// =============================================================================
// Reference Types
// =============================================================================

bool BaselineCompiler::emitRefFunc(uint32_t funcIdx) {
    Reg result = allocReg(ValType::i64());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // Load function reference from table
        masm.mov32Imm(result.code, static_cast<int32_t>(funcIdx));
        // Set funcref tag (low bit = 0 for funcref)
        masm.shl64_cl(result.code);
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movImm64(result.code, funcIdx);
        masm.lsl64(result.code, result.code, 1);
    });
    
    push(ValueLocation::inReg(result, ValType::funcRef()));
    return true;
}

bool BaselineCompiler::emitTableGet(uint32_t tableIdx) {
    ValueLocation index = pop();
    (void)index;
    
    Reg idxReg = allocReg(ValType::i32());
    Reg result = allocReg(ValType::i64());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // Bounds check
        // Load table pointer
        // Load element at index
        masm.mov32Imm(X86Assembler::RDI, static_cast<int32_t>(tableIdx));
        masm.int3();
        masm.mov64(result.code, X86Assembler::RAX);
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movImm64(0, tableIdx);
        masm.brk(0);
        masm.mov64(result.code, 0);
    });
    
    freeReg(idxReg);
    push(ValueLocation::inReg(result, ValType::externRef()));
    return true;
}

bool BaselineCompiler::emitTableSet(uint32_t tableIdx) {
    ValueLocation value = pop();
    ValueLocation index = pop();
    (void)value; (void)index;
    
    Reg idxReg = allocReg(ValType::i32());
    Reg valReg = allocReg(ValType::i64());
    
    EMIT_X64({
        X86Assembler masm(codeBuffer_.get());
        // Bounds check
        // Load table pointer
        // Store element at index
        masm.mov32Imm(X86Assembler::RDI, static_cast<int32_t>(tableIdx));
        masm.int3();
    });
    EMIT_ARM64({
        ARM64Assembler masm(codeBuffer_.get());
        masm.movImm64(0, tableIdx);
        masm.brk(0);
    });
    
    freeReg(idxReg);
    freeReg(valReg);
    return true;
}

} // namespace Zepra::Wasm
