// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file MacroAssembler.h
 * @brief Platform-abstracted code generation
 * 
 * Implements:
 * - Unified interface for x64/ARM64 code generation
 * - Register abstraction
 * - Memory operand abstraction
 * - Common instruction patterns
 * 
 */

#pragma once

#include <cstdint>
#include <vector>
#include <functional>

namespace Zepra::JIT {

// =============================================================================
// Register Abstraction
// =============================================================================

enum class RegClass : uint8_t {
    GPR,    // General purpose
    FPR,    // Floating point / SIMD
    Special // SP, FP, etc.
};

struct Register {
    uint8_t id;
    RegClass regClass;
    
    bool operator==(const Register& other) const {
        return id == other.id && regClass == other.regClass;
    }
    bool operator!=(const Register& other) const { return !(*this == other); }
    
    bool isGPR() const { return regClass == RegClass::GPR; }
    bool isFPR() const { return regClass == RegClass::FPR; }
    
    // Invalid register sentinel
    static Register invalid() { return {255, RegClass::GPR}; }
    bool isValid() const { return id != 255; }
};

// Platform-specific register names
namespace Reg {
#if defined(__x86_64__) || defined(_M_X64)
    // x64 registers
    constexpr Register rax = {0, RegClass::GPR};
    constexpr Register rcx = {1, RegClass::GPR};
    constexpr Register rdx = {2, RegClass::GPR};
    constexpr Register rbx = {3, RegClass::GPR};
    constexpr Register rsp = {4, RegClass::Special};
    constexpr Register rbp = {5, RegClass::Special};
    constexpr Register rsi = {6, RegClass::GPR};
    constexpr Register rdi = {7, RegClass::GPR};
    constexpr Register r8  = {8, RegClass::GPR};
    constexpr Register r9  = {9, RegClass::GPR};
    constexpr Register r10 = {10, RegClass::GPR};
    constexpr Register r11 = {11, RegClass::GPR};
    constexpr Register r12 = {12, RegClass::GPR};
    constexpr Register r13 = {13, RegClass::GPR};
    constexpr Register r14 = {14, RegClass::GPR};
    constexpr Register r15 = {15, RegClass::GPR};
    
    // Aliases
    constexpr Register returnValue = rax;
    constexpr Register arg0 = rdi;
    constexpr Register arg1 = rsi;
    constexpr Register arg2 = rdx;
    constexpr Register arg3 = rcx;
    constexpr Register scratch = r11;
    constexpr Register framePointer = rbp;
    constexpr Register stackPointer = rsp;
    
    // FPR
    constexpr Register xmm0 = {0, RegClass::FPR};
    constexpr Register xmm1 = {1, RegClass::FPR};
    constexpr Register xmm2 = {2, RegClass::FPR};
    constexpr Register xmm3 = {3, RegClass::FPR};

    // Architecture-neutral roles used by shared lowering code
    constexpr Register floatReturn = xmm0;
    constexpr Register scratch2 = r10;
    
#elif defined(__aarch64__) || defined(_M_ARM64)
    // ARM64 registers
    constexpr Register x0  = {0, RegClass::GPR};
    constexpr Register x1  = {1, RegClass::GPR};
    constexpr Register x2  = {2, RegClass::GPR};
    constexpr Register x3  = {3, RegClass::GPR};
    constexpr Register x4  = {4, RegClass::GPR};
    constexpr Register x5  = {5, RegClass::GPR};
    constexpr Register x6  = {6, RegClass::GPR};
    constexpr Register x7  = {7, RegClass::GPR};
    constexpr Register x8  = {8, RegClass::GPR};
    constexpr Register x9  = {9, RegClass::GPR};
    constexpr Register x10 = {10, RegClass::GPR};
    constexpr Register x11 = {11, RegClass::GPR};
    constexpr Register x12 = {12, RegClass::GPR};
    constexpr Register x13 = {13, RegClass::GPR};
    constexpr Register x14 = {14, RegClass::GPR};
    constexpr Register x15 = {15, RegClass::GPR};
    constexpr Register x16 = {16, RegClass::GPR};
    constexpr Register x17 = {17, RegClass::GPR};
    constexpr Register x29 = {29, RegClass::Special};  // FP
    constexpr Register x30 = {30, RegClass::Special};  // LR
    constexpr Register sp  = {31, RegClass::Special};
    
    // Aliases
    constexpr Register returnValue = x0;
    constexpr Register arg0 = x0;
    constexpr Register arg1 = x1;
    constexpr Register arg2 = x2;
    constexpr Register arg3 = x3;
    constexpr Register scratch = x16;
    constexpr Register framePointer = x29;
    constexpr Register stackPointer = sp;
    
    // FPR
    constexpr Register v0 = {0, RegClass::FPR};
    constexpr Register v1 = {1, RegClass::FPR};
    constexpr Register v2 = {2, RegClass::FPR};
    constexpr Register v3 = {3, RegClass::FPR};

    // Scalar double views of v0-v7 (AAPCS64 argument/result registers)
    constexpr Register d0 = {0, RegClass::FPR};
    constexpr Register d1 = {1, RegClass::FPR};
    constexpr Register d2 = {2, RegClass::FPR};
    constexpr Register d3 = {3, RegClass::FPR};
    constexpr Register d4 = {4, RegClass::FPR};
    constexpr Register d5 = {5, RegClass::FPR};
    constexpr Register d6 = {6, RegClass::FPR};
    constexpr Register d7 = {7, RegClass::FPR};

    // Architecture-neutral roles used by shared lowering code
    constexpr Register floatReturn = d0;
    constexpr Register scratch2 = x17;   // IP1, never allocated
#endif
}

// =============================================================================
// Memory Operand
// =============================================================================

enum class Scale : uint8_t {
    x1 = 0,
    x2 = 1,
    x4 = 2,
    x8 = 3
};

struct Address {
    Register base;
    Register index = Register::invalid();
    int32_t offset = 0;
    Scale scale = Scale::x1;
    
    Address(Register b, int32_t off = 0) : base(b), offset(off) {}
    Address(Register b, Register idx, Scale s, int32_t off = 0)
        : base(b), index(idx), offset(off), scale(s) {}
    
    bool hasIndex() const { return index.isValid(); }
};

// =============================================================================
// Condition Codes
// =============================================================================

enum class Condition : uint8_t {
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Below,          // Unsigned less
    BelowEqual,
    Above,          // Unsigned greater
    AboveEqual,
    Overflow,
    NoOverflow,
    Zero,
    NonZero
};

inline Condition invert(Condition c) {
    switch (c) {
        case Condition::Equal: return Condition::NotEqual;
        case Condition::NotEqual: return Condition::Equal;
        case Condition::Less: return Condition::GreaterEqual;
        case Condition::LessEqual: return Condition::Greater;
        case Condition::Greater: return Condition::LessEqual;
        case Condition::GreaterEqual: return Condition::Less;
        case Condition::Below: return Condition::AboveEqual;
        case Condition::BelowEqual: return Condition::Above;
        case Condition::Above: return Condition::BelowEqual;
        case Condition::AboveEqual: return Condition::Below;
        case Condition::Overflow: return Condition::NoOverflow;
        case Condition::NoOverflow: return Condition::Overflow;
        case Condition::Zero: return Condition::NonZero;
        case Condition::NonZero: return Condition::Zero;
    }
    return c;
}

// =============================================================================
// Label
// =============================================================================

struct Label {
    int32_t offset = -1;
    std::vector<int32_t> pendingJumps;
    
    bool isBound() const { return offset >= 0; }
};

// =============================================================================
// MacroAssembler
// =============================================================================

class MacroAssembler {
public:
    MacroAssembler() = default;
    ~MacroAssembler() = default;
    
    // Buffer management
    uint8_t* buffer() { return code_.data(); }
    const uint8_t* data() const { return code_.data(); }
    size_t size() const { return code_.size(); }
    size_t currentOffset() const { return code_.size(); }
    
    // ==========================================================================
    // Data movement
    // ==========================================================================
    
    void mov(Register dst, Register src);
    void mov(Register dst, int64_t imm);
    void mov(Register dst, Address src);
    void mov(Address dst, Register src);
    void mov(Address dst, int32_t imm);
    
    void movzx8(Register dst, Address src);
    void movzx16(Register dst, Address src);
    void movsx8(Register dst, Address src);
    void movsx16(Register dst, Address src);
    void movsx32(Register dst, Address src);
    
    // ==========================================================================
    // Arithmetic
    // ==========================================================================
    
    void add(Register dst, Register src);
    void add(Register dst, int32_t imm);
    void add(Register dst, Address src);
    
    void sub(Register dst, Register src);
    void sub(Register dst, int32_t imm);
    
    void mul(Register dst, Register src);
    void imul(Register dst, Register src, int32_t imm);
    
    void div(Register divisor);
    void idiv(Register divisor);
    
    void neg(Register reg);
    void inc(Register reg);
    void dec(Register reg);
    
    // ==========================================================================
    // Bitwise
    // ==========================================================================
    
    void and_(Register dst, Register src);
    void and_(Register dst, int32_t imm);
    void or_(Register dst, Register src);
    void or_(Register dst, int32_t imm);
    void xor_(Register dst, Register src);
    void xor_(Register dst, int32_t imm);
    void not_(Register reg);
    
    void shl(Register dst, uint8_t count);
    void shl(Register dst, Register count);
    void shr(Register dst, uint8_t count);
    void shr(Register dst, Register count);
    void sar(Register dst, uint8_t count);
    void sar(Register dst, Register count);
    
    // ==========================================================================
    // Comparison
    // ==========================================================================
    
    void cmp(Register lhs, Register rhs);
    void cmp(Register lhs, int32_t imm);
    void cmp(Register lhs, Address rhs);
    void test(Register lhs, Register rhs);
    void test(Register lhs, int32_t imm);
    
    // ==========================================================================
    // Branches
    // ==========================================================================
    
    void jmp(Label& target);
    void jmp(Register target);
    void jcc(Condition cond, Label& target);
    
    void call(Label& target);
    void call(Register target);
    void call(Address target);
    
    void ret();
    
    // ==========================================================================
    // Labels
    // ==========================================================================
    
    void bind(Label& label);
    
    // ==========================================================================
    // Stack operations
    // ==========================================================================
    
    void push(Register reg);
    void push(int32_t imm);
    void push(Address src);
    void pop(Register reg);
    
    void adjustStackPointer(int32_t delta);
    
    // ==========================================================================
    // Function prologue/epilogue
    // ==========================================================================
    
    void functionPrologue(size_t frameSize);
    void functionEpilogue();
    
    // ==========================================================================
    // Floating point
    // ==========================================================================
    
    void movsd(Register dst, Register src);
    void movsd(Register dst, Address src);
    void movsd(Address dst, Register src);
    
    void addsd(Register dst, Register src);
    void subsd(Register dst, Register src);
    void mulsd(Register dst, Register src);
    void divsd(Register dst, Register src);
    
    void cvtsi2sd(Register dst, Register src);
    void cvttsd2si(Register dst, Register src);
    
    // ==========================================================================
    // Memory barriers
    // ==========================================================================
    
    void mfence();
    void lfence();
    void sfence();
    
    // ==========================================================================
    // Raw emit
    // ==========================================================================
    
    void emit8(uint8_t byte) { code_.push_back(byte); }
    void emit16(uint16_t word);
    void emit32(uint32_t dword);
    void emit64(uint64_t qword);
    
    // Patch at offset
    void patch32(size_t offset, int32_t value);
    void patch64(size_t offset, int64_t value);
    
private:
    std::vector<uint8_t> code_;
    
    // Encoding helpers
    void encodeModRM(uint8_t mod, uint8_t reg, uint8_t rm);
    void encodeSIB(Scale scale, uint8_t index, uint8_t base);
    void encodeREX(bool w, bool r, bool x, bool b);
    void encodeAddress(Register reg, const Address& addr);
};

// =============================================================================
// Code Buffer
// =============================================================================

/**
 * @brief Executable code buffer
 */
class ExecutableBuffer {
public:
    ExecutableBuffer() = default;
    ~ExecutableBuffer();
    
    bool allocate(size_t size);
    bool copyFrom(const MacroAssembler& masm);
    void makeExecutable();
    
    void* code() const { return code_; }
    size_t size() const { return size_; }
    
    template<typename Fn>
    Fn as() const { return reinterpret_cast<Fn>(code_); }
    
private:
    void* code_ = nullptr;
    size_t size_ = 0;
};

} // namespace Zepra::JIT
