// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file ZOptLowering.cpp
 * @brief ZOpt Graph → Machine Code Lowering
 *
 * Converts ZOpt SSA IR into x86-64 machine code using the MacroAssembler.
 * This is the final stage of the ZOpt JIT pipeline:
 *   Bytecode → ZOptBuilder → Graph → ConstFold → DCE → StrengthReduce → Lowering → native code
 *
 */

#include "zopt/ZOptGraph.h"
#include "zopt/ZOptBasicBlock.h"
#include "zopt/ZOptValue.h"
#include "zopt/ZOptOpcode.h"
#include "jit/MacroAssembler.h"
#include <unordered_map>
#include <vector>
#include <cassert>
#include <cstring>
#include <cstdio>

namespace Zepra::ZOpt {

// =============================================================================
// Register assignment map (vreg → physical)
// =============================================================================

struct RegAssignment {
    JIT::Register reg = JIT::Register::invalid();
    int32_t stackSlot = -1;

    bool isReg() const { return reg.isValid(); }
    bool isStack() const { return stackSlot >= 0; }
};

// =============================================================================
// ZOpt Lowering Pass
// =============================================================================

class Lowering {
public:
    Lowering(Graph* graph, JIT::MacroAssembler& masm)
        : graph_(graph), masm_(masm) {}

    void run() {
        allocateRegisters();
        emitPrologue();
        for (BasicBlock* block : graph_->reversePostOrder()) {
            emitBlock(block);
        }
    }

private:
    // =========================================================================
    // Register allocation (simple greedy for baseline)
    // =========================================================================

    void allocateRegisters() {
#if defined(__x86_64__) || defined(_M_X64)
        availableGPRs_ = {
            JIT::Reg::rcx, JIT::Reg::rdx, JIT::Reg::rsi, JIT::Reg::rdi,
            JIT::Reg::r8,  JIT::Reg::r9,  JIT::Reg::r10, JIT::Reg::r11
        };
        availableFPRs_ = {
            JIT::Reg::xmm0, JIT::Reg::xmm1, JIT::Reg::xmm2, JIT::Reg::xmm3
        };
#elif defined(__aarch64__) || defined(_M_ARM64)
        // AArch64: x0–x15 are caller-saved, x16–x17 are IP scratch,
        // x18 is platform register, x19–x28 are callee-saved.
        availableGPRs_ = {
            JIT::Reg::x0,  JIT::Reg::x1,  JIT::Reg::x2,  JIT::Reg::x3,
            JIT::Reg::x4,  JIT::Reg::x5,  JIT::Reg::x6,  JIT::Reg::x7,
            JIT::Reg::x8,  JIT::Reg::x9,  JIT::Reg::x10, JIT::Reg::x11,
            JIT::Reg::x12, JIT::Reg::x13, JIT::Reg::x14, JIT::Reg::x15
        };
        availableFPRs_ = {
            JIT::Reg::d0, JIT::Reg::d1, JIT::Reg::d2, JIT::Reg::d3,
            JIT::Reg::d4, JIT::Reg::d5, JIT::Reg::d6, JIT::Reg::d7
        };
#else
        static_assert(false, "ZOptLowering: unsupported architecture — add GPR/FPR sets");
#endif
        gprIdx_ = 0;
        fprIdx_ = 0;
        nextStackSlot_ = 0;

        // Pre-assign parameters
        for (BasicBlock* block : graph_->reversePostOrder()) {
            block->forEachValue([&](Value* v) {
                if (v->isDead()) return;
                if (!hasResult(v->opcode())) return;
                assignValue(v);
            });
        }
    }

    void assignValue(Value* v) {
        RegAssignment ra;
        if (isFloat(v->type())) {
            if (fprIdx_ < availableFPRs_.size()) {
                ra.reg = availableFPRs_[fprIdx_++];
            } else {
                ra.stackSlot = nextStackSlot_++;
            }
        } else {
            if (gprIdx_ < availableGPRs_.size()) {
                ra.reg = availableGPRs_[gprIdx_++];
            } else {
                ra.stackSlot = nextStackSlot_++;
            }
        }
        assignments_[v->index()] = ra;
    }

    RegAssignment regOf(Value* v) const {
        auto it = assignments_.find(v->index());
        if (it != assignments_.end()) return it->second;
        return {};
    }

    JIT::Address stackAddr(int32_t slot) const {
        // Stack slots are below the frame pointer
        return JIT::Address(JIT::Reg::framePointer, -(slot + 1) * 8);
    }

    // Load a value into a target register if it's on the stack
    void loadValue(Value* v, JIT::Register target) {
        auto ra = regOf(v);
        if (ra.isReg()) {
            if (ra.reg != target) {
                if (isFloat(v->type())) {
                    masm_.movsd(target, ra.reg);
                } else {
                    masm_.mov(target, ra.reg);
                }
            }
        } else if (ra.isStack()) {
            if (isFloat(v->type())) {
                masm_.movsd(target, stackAddr(ra.stackSlot));
            } else {
                masm_.mov(target, stackAddr(ra.stackSlot));
            }
        }
    }

    // Store a register value to the assigned location
    void storeResult(Value* v, JIT::Register resultReg) {
        auto ra = regOf(v);
        if (ra.isStack()) {
            // Spill the computed result from resultReg to the stack slot.
            if (isFloat(v->type())) {
                masm_.movsd(stackAddr(ra.stackSlot), resultReg);
            } else {
                masm_.mov(stackAddr(ra.stackSlot), resultReg);
            }
        }
        // If ra.isReg() the value is already in the right register — nothing to do.
    }

    // =========================================================================
    // Emission
    // =========================================================================

    void emitPrologue() {
        size_t frameSize = static_cast<size_t>(nextStackSlot_) * 8;
        masm_.functionPrologue(frameSize);
    }

    void emitBlock(BasicBlock* block) {
        // Bind block label
        auto& label = blockLabels_[block->index()];
        masm_.bind(label);

        // Emit phis (parallel copies — simplified)
        for (Value* phi : block->phis()) {
            emitPhi(phi);
        }

        // Emit values
        for (Value* v : block->values()) {
            if (v->isDead()) continue;
            emitValue(v);
        }
    }

    void emitPhi(Value* phi) {
        // Phi nodes are resolved by inserting moves at the end of predecessor blocks
        // For baseline, just ensure the value is in the right location
        auto ra = regOf(phi);
        if (!ra.isReg() && !ra.isStack()) return;

        // Use first non-null input as source
        for (uint32_t i = 0; i < phi->numInputs(); ++i) {
            Value* input = phi->input(i);
            if (input && !input->isDead()) {
                auto srcRa = regOf(input);
                if (ra.isReg() && srcRa.isReg() && ra.reg != srcRa.reg) {
                    masm_.mov(ra.reg, srcRa.reg);
                }
                break;
            }
        }
    }

    void emitValue(Value* v) {
        switch (v->opcode()) {
            case Opcode::Const32: emitConst32(v); break;
            case Opcode::Const64: emitConst64(v); break;
            case Opcode::ConstF64: emitConstF64(v); break;
            case Opcode::Add32: emitBinaryI32(v, BinOp::Add); break;
            case Opcode::Sub32: emitBinaryI32(v, BinOp::Sub); break;
            case Opcode::Mul32: emitBinaryI32(v, BinOp::Mul); break;
            case Opcode::And32: emitBinaryI32(v, BinOp::And); break;
            case Opcode::Or32:  emitBinaryI32(v, BinOp::Or); break;
            case Opcode::Xor32: emitBinaryI32(v, BinOp::Xor); break;
            case Opcode::Shl32: emitBinaryI32(v, BinOp::Shl); break;
            case Opcode::Shr32S: emitBinaryI32(v, BinOp::Sar); break;
            case Opcode::Shr32U: emitBinaryI32(v, BinOp::Shr); break;
            case Opcode::Add64: emitBinaryI64(v, BinOp::Add); break;
            case Opcode::Sub64: emitBinaryI64(v, BinOp::Sub); break;
            case Opcode::Mul64: emitBinaryI64(v, BinOp::Mul); break;
            case Opcode::AddF64: emitBinaryF64(v, FBinOp::Add); break;
            case Opcode::SubF64: emitBinaryF64(v, FBinOp::Sub); break;
            case Opcode::MulF64: emitBinaryF64(v, FBinOp::Mul); break;
            case Opcode::DivF64: emitBinaryF64(v, FBinOp::Div); break;
            case Opcode::Eq32:  emitCompareI32(v, JIT::Condition::Equal); break;
            case Opcode::Ne32:  emitCompareI32(v, JIT::Condition::NotEqual); break;
            case Opcode::Lt32S: emitCompareI32(v, JIT::Condition::Less); break;
            case Opcode::Le32S: emitCompareI32(v, JIT::Condition::LessEqual); break;
            case Opcode::Gt32S: emitCompareI32(v, JIT::Condition::Greater); break;
            case Opcode::Ge32S: emitCompareI32(v, JIT::Condition::GreaterEqual); break;
            case Opcode::Branch: emitBranch(v); break;
            case Opcode::Jump: emitJump(v); break;
            case Opcode::Return: emitReturn(v); break;
            case Opcode::Call: emitCall(v); break;
            case Opcode::GetLocal: emitGetLocal(v); break;
            case Opcode::SetLocal: emitSetLocal(v); break;
            case Opcode::Load32: emitLoad(v, 4); break;
            case Opcode::Load64: emitLoad(v, 8); break;
            case Opcode::Store32: emitStore(v, 4); break;
            case Opcode::Store64: emitStore(v, 8); break;
            case Opcode::Wrap64To32: emitTruncate(v); break;
            case Opcode::Extend32STo64: emitSignExtend(v); break;
            case Opcode::ConvertI32SToF64: emitIntToDouble(v); break;
            case Opcode::TruncF64ToI32S: emitDoubleToInt(v); break;
            default:
                break;
        }
    }

    // =========================================================================
    // Constant emission
    // =========================================================================

    void emitConst32(Value* v) {
        auto ra = regOf(v);
        if (ra.isReg()) {
            masm_.mov(ra.reg, static_cast<int64_t>(v->asInt32()));
        } else if (ra.isStack()) {
            masm_.mov(stackAddr(ra.stackSlot), v->asInt32());
        }
    }

    void emitConst64(Value* v) {
        auto ra = regOf(v);
        if (ra.isReg()) {
            masm_.mov(ra.reg, v->asInt64());
        }
    }

    void emitConstF64(Value* v) {
        auto ra = regOf(v);
        if (ra.isReg()) {
            // Load double constant via integer register
            int64_t bits;
            double val = v->asFloat64();
            std::memcpy(&bits, &val, 8);
            masm_.mov(JIT::Reg::scratch, bits);
            // Move integer register to XMM via memory (simplification)
            masm_.mov(JIT::Address(JIT::Reg::stackPointer, -8), JIT::Reg::scratch);
            masm_.movsd(ra.reg, JIT::Address(JIT::Reg::stackPointer, -8));
        }
    }

    // =========================================================================
    // Integer binary operations
    // =========================================================================

    enum class BinOp { Add, Sub, Mul, And, Or, Xor, Shl, Shr, Sar };

    void emitBinaryI32(Value* v, BinOp op) {
        auto dst = regOf(v);
        if (!dst.isReg()) return;

        Value* lhs = v->input(0);
        Value* rhs = v->input(1);

        loadValue(lhs, dst.reg);

        auto rhsRa = regOf(rhs);
        JIT::Register rhsReg = rhsRa.isReg() ? rhsRa.reg : JIT::Reg::scratch;
        if (!rhsRa.isReg()) {
            if (rhs->isConstant()) {
                masm_.mov(rhsReg, static_cast<int64_t>(rhs->asInt32()));
            } else {
                loadValue(rhs, rhsReg);
            }
        }

        switch (op) {
            case BinOp::Add: masm_.add(dst.reg, rhsReg); break;
            case BinOp::Sub: masm_.sub(dst.reg, rhsReg); break;
            case BinOp::Mul: masm_.mul(dst.reg, rhsReg); break;
            case BinOp::And: masm_.and_(dst.reg, rhsReg); break;
            case BinOp::Or:  masm_.or_(dst.reg, rhsReg); break;
            case BinOp::Xor: masm_.xor_(dst.reg, rhsReg); break;
            case BinOp::Shl: masm_.shl(dst.reg, rhsReg); break;
            case BinOp::Shr: masm_.shr(dst.reg, rhsReg); break;
            case BinOp::Sar: masm_.sar(dst.reg, rhsReg); break;
        }
    }

    void emitBinaryI64(Value* v, BinOp op) {
        // Same encoding as I32 in x86-64 (REX.W already set by MacroAssembler)
        emitBinaryI32(v, op);
    }

    // =========================================================================
    // Float binary operations
    // =========================================================================

    enum class FBinOp { Add, Sub, Mul, Div };

    void emitBinaryF64(Value* v, FBinOp op) {
        auto dst = regOf(v);
        if (!dst.isReg()) return;

        Value* lhs = v->input(0);
        Value* rhs = v->input(1);

        loadValue(lhs, dst.reg);

        auto rhsRa = regOf(rhs);
        JIT::Register rhsReg = rhsRa.isReg() ? rhsRa.reg : JIT::Register{3, JIT::RegClass::FPR};
        if (!rhsRa.isReg()) {
            loadValue(rhs, rhsReg);
        }

        switch (op) {
            case FBinOp::Add: masm_.addsd(dst.reg, rhsReg); break;
            case FBinOp::Sub: masm_.subsd(dst.reg, rhsReg); break;
            case FBinOp::Mul: masm_.mulsd(dst.reg, rhsReg); break;
            case FBinOp::Div: masm_.divsd(dst.reg, rhsReg); break;
        }
    }

    // =========================================================================
    // Comparisons
    // =========================================================================

    void emitCompareI32(Value* v, JIT::Condition cond) {
        auto dst = regOf(v);
        if (!dst.isReg()) return;

        Value* lhs = v->input(0);
        Value* rhs = v->input(1);

        auto lhsRa = regOf(lhs);
        auto rhsRa = regOf(rhs);

        JIT::Register lhsReg = lhsRa.isReg() ? lhsRa.reg : JIT::Reg::scratch;
        if (!lhsRa.isReg()) loadValue(lhs, lhsReg);

        if (rhsRa.isReg()) {
            masm_.cmp(lhsReg, rhsRa.reg);
        } else if (rhs->isConstant()) {
            masm_.cmp(lhsReg, rhs->asInt32());
        }

        // SETCC: encode result (0 or 1) into dst register
        // XOR dst, dst; then emit SETcc byte sequence via public emit8
        masm_.xor_(dst.reg, static_cast<int32_t>(0));
        // SETcc r/m8 — 0F 9x /0  (cc codes match jcc)
        uint8_t cc = 0;
        switch (cond) {
            case JIT::Condition::Equal:        cc = 0x04; break;
            case JIT::Condition::NotEqual:     cc = 0x05; break;
            case JIT::Condition::Less:         cc = 0x0C; break;
            case JIT::Condition::LessEqual:    cc = 0x0E; break;
            case JIT::Condition::Greater:      cc = 0x0F; break;
            case JIT::Condition::GreaterEqual: cc = 0x0D; break;
            default: cc = 0x04; break;
        }
        // REX prefix if needed (register id > 7)
        if (dst.reg.id > 7) {
            masm_.emit8(0x41); // REX.B
        }
        masm_.emit8(0x0F);
        masm_.emit8(0x90 + cc);
        // ModRM: mod=11, reg=0, rm=dst
        masm_.emit8(0xC0 | (dst.reg.id & 7));
    }

    // =========================================================================
    // Control flow
    // =========================================================================

    void emitBranch(Value* v) {
        if (v->numInputs() < 1) return;
        Value* cond = v->input(0);
        auto condRa = regOf(cond);

        JIT::Register condReg = condRa.isReg() ? condRa.reg : JIT::Reg::scratch;
        if (!condRa.isReg()) loadValue(cond, condReg);

        masm_.test(condReg, condReg);

        // True successor = block's successor[0], false = successor[1]
        BasicBlock* block = v->block();
        if (block->numSuccessors() >= 2) {
            auto& falseLabel = blockLabels_[block->successors()[1]->index()];
            masm_.jcc(JIT::Condition::Zero, falseLabel);
            // Fall through to true successor
        }
    }

    void emitJump(Value* v) {
        BasicBlock* block = v->block();
        if (block->numSuccessors() >= 1) {
            auto& target = blockLabels_[block->successors()[0]->index()];
            masm_.jmp(target);
        }
    }

    void emitReturn(Value* v) {
        if (v->numInputs() >= 1) {
            Value* retVal = v->input(0);
            if (isFloat(retVal->type())) {
                loadValue(retVal, JIT::Reg::xmm0);
            } else {
                loadValue(retVal, JIT::Reg::returnValue);
            }
        }
        masm_.functionEpilogue();
    }

    void emitCall(Value* v) {
        // Load arguments into ABI registers
        for (uint32_t i = 0; i < v->numInputs() && i < 4; ++i) {
            Value* arg = v->input(i);
            JIT::Register argReg = JIT::Register::invalid();
            switch (i) {
                case 0: argReg = JIT::Reg::arg0; break;
                case 1: argReg = JIT::Reg::arg1; break;
                case 2: argReg = JIT::Reg::arg2; break;
                case 3: argReg = JIT::Reg::arg3; break;
            }
            if (argReg.isValid()) {
                loadValue(arg, argReg);
            }
        }

        // Indirect call through function pointer stored in auxInt
        int64_t funcAddr = v->auxInt();
        masm_.mov(JIT::Reg::scratch, funcAddr);
        masm_.call(JIT::Reg::scratch);

        // Move result to assigned register
        auto dst = regOf(v);
        if (dst.isReg() && dst.reg != JIT::Reg::returnValue) {
            if (isFloat(v->type())) {
                masm_.movsd(dst.reg, JIT::Reg::xmm0);
            } else {
                masm_.mov(dst.reg, JIT::Reg::returnValue);
            }
        }
    }

    // =========================================================================
    // Locals
    // =========================================================================

    void emitGetLocal(Value* v) {
        auto dst = regOf(v);
        if (!dst.isReg()) return;

        int32_t localIdx = static_cast<int32_t>(v->auxInt());
        int32_t offset = -(localIdx + 1) * 8;
        masm_.mov(dst.reg, JIT::Address(JIT::Reg::framePointer, offset));
    }

    void emitSetLocal(Value* v) {
        if (v->numInputs() < 1) return;
        Value* src = v->input(0);

        int32_t localIdx = static_cast<int32_t>(v->auxInt());
        int32_t offset = -(localIdx + 1) * 8;

        auto srcRa = regOf(src);
        if (srcRa.isReg()) {
            masm_.mov(JIT::Address(JIT::Reg::framePointer, offset), srcRa.reg);
        } else if (src->isConstant()) {
            masm_.mov(JIT::Address(JIT::Reg::framePointer, offset),
                      static_cast<int32_t>(src->asInt32()));
        }
    }

    // =========================================================================
    // Memory
    // =========================================================================

    void emitLoad(Value* v, int size) {
        auto dst = regOf(v);
        if (!dst.isReg()) return;
        if (v->numInputs() < 1) return;

        Value* base = v->input(0);
        auto baseRa = regOf(base);
        JIT::Register baseReg = baseRa.isReg() ? baseRa.reg : JIT::Reg::scratch;
        if (!baseRa.isReg()) loadValue(base, baseReg);

        int32_t offset = (v->numInputs() > 1 && v->input(1)->isConstant())
                             ? v->input(1)->asInt32()
                             : 0;

        if (size == 4) {
            // 32-bit load (zero-extends on x64)
            masm_.mov(dst.reg, JIT::Address(baseReg, offset));
        } else {
            masm_.mov(dst.reg, JIT::Address(baseReg, offset));
        }
    }

    void emitStore(Value* v, int size) {
        if (v->numInputs() < 2) return;

        Value* base = v->input(0);
        Value* val = v->input(1);

        auto baseRa = regOf(base);
        auto valRa = regOf(val);

        JIT::Register baseReg = baseRa.isReg() ? baseRa.reg : JIT::Reg::scratch;
        if (!baseRa.isReg()) loadValue(base, baseReg);

        int32_t offset = 0;
        JIT::Register valReg = valRa.isReg() ? valRa.reg : JIT::Reg::r10;
        if (!valRa.isReg()) loadValue(val, valReg);

        (void)size;
        masm_.mov(JIT::Address(baseReg, offset), valReg);
    }

    // =========================================================================
    // Conversions
    // =========================================================================

    void emitTruncate(Value* v) {
        auto dst = regOf(v);
        if (!dst.isReg() || v->numInputs() < 1) return;
        loadValue(v->input(0), dst.reg);
        // Zero upper 32 bits: MOV r32, r32 (implicit zero-extend)
        masm_.and_(dst.reg, static_cast<int32_t>(0xFFFFFFFF));
    }

    void emitSignExtend(Value* v) {
        auto dst = regOf(v);
        if (!dst.isReg() || v->numInputs() < 1) return;
        loadValue(v->input(0), dst.reg);
        // MOVSXD r64, r/m32 — already 64-bit in our mov with REX.W
    }

    void emitIntToDouble(Value* v) {
        auto dst = regOf(v);
        if (!dst.isReg() || v->numInputs() < 1) return;
        auto srcRa = regOf(v->input(0));
        JIT::Register srcReg = srcRa.isReg() ? srcRa.reg : JIT::Reg::scratch;
        if (!srcRa.isReg()) loadValue(v->input(0), srcReg);
        masm_.cvtsi2sd(dst.reg, srcReg);
    }

    void emitDoubleToInt(Value* v) {
        auto dst = regOf(v);
        if (!dst.isReg() || v->numInputs() < 1) return;
        auto srcRa = regOf(v->input(0));
        JIT::Register srcReg = srcRa.isReg() ? srcRa.reg : JIT::Register{0, JIT::RegClass::FPR};
        if (!srcRa.isReg()) loadValue(v->input(0), srcReg);
        masm_.cvttsd2si(dst.reg, srcReg);
    }

    // =========================================================================
    // Data
    // =========================================================================

    Graph* graph_;
    JIT::MacroAssembler& masm_;
    std::unordered_map<uint32_t, RegAssignment> assignments_;
    std::unordered_map<uint32_t, JIT::Label> blockLabels_;
    std::vector<JIT::Register> availableGPRs_;
    std::vector<JIT::Register> availableFPRs_;
    size_t gprIdx_ = 0;
    size_t fprIdx_ = 0;
    int32_t nextStackSlot_ = 0;
};

// =============================================================================
// Public API
// =============================================================================

void lowerGraph(Graph* graph, JIT::MacroAssembler& masm) {
    Lowering lowering(graph, masm);
    lowering.run();
}

} // namespace Zepra::ZOpt
