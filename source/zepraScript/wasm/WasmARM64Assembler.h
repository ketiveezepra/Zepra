// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file WasmARM64Assembler.h
 * @brief ARM64/AArch64 assembler for baseline JIT
 */

#pragma once

#include "wasm/WasmBaselineInternal.h"

namespace Zepra::Wasm {
// =============================================================================

class ARM64Assembler {
public:
    explicit ARM64Assembler(CodeBuffer* buffer) : buf_(buffer) {}
    
    // General registers: x0-x30, sp (x31), xzr (x31 in some contexts)
    enum GPR : uint8_t {
        X0 = 0, X1 = 1, X2 = 2, X3 = 3, X4 = 4, X5 = 5, X6 = 6, X7 = 7,
        X8 = 8, X9 = 9, X10 = 10, X11 = 11, X12 = 12, X13 = 13, X14 = 14, X15 = 15,
        X16 = 16, X17 = 17, X18 = 18, X19 = 19, X20 = 20, X21 = 21, X22 = 22, X23 = 23,
        X24 = 24, X25 = 25, X26 = 26, X27 = 27, X28 = 28,
        FP = 29,   // Frame pointer (x29)
        LR = 30,   // Link register (x30)
        SP = 31,   // Stack pointer
        XZR = 31   // Zero register (context-dependent)
    };
    
    // Vector registers: v0-v31
    enum VPR : uint8_t {
        V0 = 0, V1 = 1, V2 = 2, V3 = 3, V4 = 4, V5 = 5, V6 = 6, V7 = 7,
        V16 = 16, V17 = 17, V18 = 18, V19 = 19, V20 = 20, V21 = 21, V22 = 22, V23 = 23,
        V24 = 24, V25 = 25, V26 = 26, V27 = 27, V28 = 28, V29 = 29, V30 = 30, V31 = 31
    };
    
    // Emit 32-bit instruction (ARM64 uses fixed 32-bit encoding)
    void emit32(uint32_t insn) { buf_->emit32(insn); }
    
    // Move immediate to register (MOV)
    void movImm64(uint8_t rd, uint64_t imm) {
        // MOVZ + MOVK sequence for 64-bit immediate
        // MOVZ Xd, #imm16, LSL #0
        uint32_t movz = 0xD2800000 | ((imm & 0xFFFF) << 5) | rd;
        emit32(movz);
        
        if ((imm >> 16) & 0xFFFF) {
            // MOVK Xd, #imm16, LSL #16
            uint32_t movk = 0xF2A00000 | (((imm >> 16) & 0xFFFF) << 5) | rd;
            emit32(movk);
        }
        if ((imm >> 32) & 0xFFFF) {
            // MOVK Xd, #imm16, LSL #32
            uint32_t movk = 0xF2C00000 | (((imm >> 32) & 0xFFFF) << 5) | rd;
            emit32(movk);
        }
        if ((imm >> 48) & 0xFFFF) {
            // MOVK Xd, #imm16, LSL #48
            uint32_t movk = 0xF2E00000 | (((imm >> 48) & 0xFFFF) << 5) | rd;
            emit32(movk);
        }
    }
    
    void movImm32(uint8_t rd, uint32_t imm) {
        // MOVZ Wd, #imm16
        uint32_t movz = 0x52800000 | ((imm & 0xFFFF) << 5) | rd;
        emit32(movz);
        
        if ((imm >> 16) & 0xFFFF) {
            // MOVK Wd, #imm16, LSL #16
            uint32_t movk = 0x72A00000 | (((imm >> 16) & 0xFFFF) << 5) | rd;
            emit32(movk);
        }
    }
    
    // Move register to register
    void mov64(uint8_t rd, uint8_t rn) {
        // MOV Xd, Xn = ORR Xd, XZR, Xn
        uint32_t insn = 0xAA0003E0 | (rn << 16) | rd;
        emit32(insn);
    }
    
    void mov32(uint8_t rd, uint8_t rn) {
        // MOV Wd, Wn = ORR Wd, WZR, Wn
        uint32_t insn = 0x2A0003E0 | (rn << 16) | rd;
        emit32(insn);
    }
    
    // Arithmetic: ADD
    void add64(uint8_t rd, uint8_t rn, uint8_t rm) {
        // ADD Xd, Xn, Xm
        uint32_t insn = 0x8B000000 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    void add32(uint8_t rd, uint8_t rn, uint8_t rm) {
        // ADD Wd, Wn, Wm
        uint32_t insn = 0x0B000000 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    void addImm64(uint8_t rd, uint8_t rn, uint32_t imm12) {
        // ADD Xd, Xn, #imm12
        uint32_t insn = 0x91000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd;
        emit32(insn);
    }
    
    // Arithmetic: SUB
    void sub64(uint8_t rd, uint8_t rn, uint8_t rm) {
        // SUB Xd, Xn, Xm
        uint32_t insn = 0xCB000000 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    void sub32(uint8_t rd, uint8_t rn, uint8_t rm) {
        // SUB Wd, Wn, Wm
        uint32_t insn = 0x4B000000 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    void subImm64(uint8_t rd, uint8_t rn, uint32_t imm12) {
        // SUB Xd, Xn, #imm12
        uint32_t insn = 0xD1000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd;
        emit32(insn);
    }

    void subImm32(uint8_t rd, uint8_t rn, uint32_t imm12) {
        // SUB Wd, Wn, #imm12
        uint32_t insn = 0x51000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd;
        emit32(insn);
    }
    
    // Negate: NEG Xd, Xn = SUB Xd, XZR, Xn
    void neg64(uint8_t rd, uint8_t rn) {
        sub64(rd, XZR, rn);
    }
    
    void neg32(uint8_t rd, uint8_t rn) {
        sub32(rd, static_cast<uint8_t>(31), rn);  // WZR = 31
    }
    
    // Arithmetic: MUL
    void mul64(uint8_t rd, uint8_t rn, uint8_t rm) {
        // MUL Xd, Xn, Xm = MADD Xd, Xn, Xm, XZR
        uint32_t insn = 0x9B007C00 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    void mul32(uint8_t rd, uint8_t rn, uint8_t rm) {
        // MUL Wd, Wn, Wm = MADD Wd, Wn, Wm, WZR
        uint32_t insn = 0x1B007C00 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    // Signed division
    void sdiv64(uint8_t rd, uint8_t rn, uint8_t rm) {
        // SDIV Xd, Xn, Xm
        uint32_t insn = 0x9AC00C00 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    void sdiv32(uint8_t rd, uint8_t rn, uint8_t rm) {
        // SDIV Wd, Wn, Wm
        uint32_t insn = 0x1AC00C00 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    // Unsigned division
    void udiv64(uint8_t rd, uint8_t rn, uint8_t rm) {
        // UDIV Xd, Xn, Xm
        uint32_t insn = 0x9AC00800 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    void udiv32(uint8_t rd, uint8_t rn, uint8_t rm) {
        // UDIV Wd, Wn, Wm
        uint32_t insn = 0x1AC00800 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    // Multiply-subtract: rd = ra - rn * rm (for computing remainder)
    void msub64(uint8_t rd, uint8_t rn, uint8_t rm, uint8_t ra) {
        // MSUB Xd, Xn, Xm, Xa
        uint32_t insn = 0x9B008000 | (rm << 16) | (ra << 10) | (rn << 5) | rd;
        emit32(insn);
    }
    
    void msub32(uint8_t rd, uint8_t rn, uint8_t rm, uint8_t ra) {
        // MSUB Wd, Wn, Wm, Wa
        uint32_t insn = 0x1B008000 | (rm << 16) | (ra << 10) | (rn << 5) | rd;
        emit32(insn);
    }
    
    // Logical operations
    void and64(uint8_t rd, uint8_t rn, uint8_t rm) {
        uint32_t insn = 0x8A000000 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    void orr64(uint8_t rd, uint8_t rn, uint8_t rm) {
        uint32_t insn = 0xAA000000 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    void eor64(uint8_t rd, uint8_t rn, uint8_t rm) {
        uint32_t insn = 0xCA000000 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    // Shifts
    void lsl64(uint8_t rd, uint8_t rn, uint8_t rm) {
        // LSLV Xd, Xn, Xm
        uint32_t insn = 0x9AC02000 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    void lsr64(uint8_t rd, uint8_t rn, uint8_t rm) {
        // LSRV Xd, Xn, Xm
        uint32_t insn = 0x9AC02400 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    void asr64(uint8_t rd, uint8_t rn, uint8_t rm) {
        // ASRV Xd, Xn, Xm
        uint32_t insn = 0x9AC02800 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    // Comparison (sets flags)
    void cmp64(uint8_t rn, uint8_t rm) {
        // SUBS XZR, Xn, Xm
        uint32_t insn = 0xEB00001F | (rm << 16) | (rn << 5);
        emit32(insn);
    }
    
    void cmp32(uint8_t rn, uint8_t rm) {
        // SUBS WZR, Wn, Wm
        uint32_t insn = 0x6B00001F | (rm << 16) | (rn << 5);
        emit32(insn);
    }
    
    // Load/Store
    void ldr64(uint8_t rt, uint8_t rn, int32_t offset) {
        // LDR Xt, [Xn, #offset]
        uint32_t insn = 0xF9400000 | (((offset >> 3) & 0xFFF) << 10) | (rn << 5) | rt;
        emit32(insn);
    }
    
    void str64(uint8_t rt, uint8_t rn, int32_t offset) {
        // STR Xt, [Xn, #offset]
        uint32_t insn = 0xF9000000 | (((offset >> 3) & 0xFFF) << 10) | (rn << 5) | rt;
        emit32(insn);
    }
    
    void ldr32(uint8_t rt, uint8_t rn, int32_t offset) {
        // LDR Wt, [Xn, #offset]
        uint32_t insn = 0xB9400000 | (((offset >> 2) & 0xFFF) << 10) | (rn << 5) | rt;
        emit32(insn);
    }
    
    void str32(uint8_t rt, uint8_t rn, int32_t offset) {
        // STR Wt, [Xn, #offset]
        uint32_t insn = 0xB9000000 | (((offset >> 2) & 0xFFF) << 10) | (rn << 5) | rt;
        emit32(insn);
    }
    
    void strb(uint8_t rt, uint8_t rn, int32_t offset) {
        // STRB Wt, [Xn, #offset]
        uint32_t insn = 0x39000000 | ((offset & 0xFFF) << 10) | (rn << 5) | rt;
        emit32(insn);
    }
    
    void strh(uint8_t rt, uint8_t rn, int32_t offset) {
        // STRH Wt, [Xn, #offset]
        uint32_t insn = 0x79000000 | (((offset >> 1) & 0xFFF) << 10) | (rn << 5) | rt;
        emit32(insn);
    }
    
    void ldrb(uint8_t rt, uint8_t rn, int32_t offset) {
        // LDRB Wt, [Xn, #offset]
        uint32_t insn = 0x39400000 | ((offset & 0xFFF) << 10) | (rn << 5) | rt;
        emit32(insn);
    }
    
    void ldrh(uint8_t rt, uint8_t rn, int32_t offset) {
        // LDRH Wt, [Xn, #offset]
        uint32_t insn = 0x79400000 | (((offset >> 1) & 0xFFF) << 10) | (rn << 5) | rt;
        emit32(insn);
    }
    
    void ldrsb32(uint8_t rt, uint8_t rn, int32_t offset) {
        // LDRSB Wt, [Xn, #offset] - sign-extend byte to 32-bit
        uint32_t insn = 0x39C00000 | ((offset & 0xFFF) << 10) | (rn << 5) | rt;
        emit32(insn);
    }
    
    void ldrsb64(uint8_t rt, uint8_t rn, int32_t offset) {
        // LDRSB Xt, [Xn, #offset] - sign-extend byte to 64-bit
        uint32_t insn = 0x39800000 | ((offset & 0xFFF) << 10) | (rn << 5) | rt;
        emit32(insn);
    }
    
    void ldrsh32(uint8_t rt, uint8_t rn, int32_t offset) {
        // LDRSH Wt, [Xn, #offset] - sign-extend halfword to 32-bit
        uint32_t insn = 0x79C00000 | (((offset >> 1) & 0xFFF) << 10) | (rn << 5) | rt;
        emit32(insn);
    }
    
    void ldrsh64(uint8_t rt, uint8_t rn, int32_t offset) {
        // LDRSH Xt, [Xn, #offset] - sign-extend halfword to 64-bit
        uint32_t insn = 0x79800000 | (((offset >> 1) & 0xFFF) << 10) | (rn << 5) | rt;
        emit32(insn);
    }
    
    void ldrsw(uint8_t rt, uint8_t rn, int32_t offset) {
        // LDRSW Xt, [Xn, #offset] - sign-extend word to 64-bit
        uint32_t insn = 0xB9800000 | (((offset >> 2) & 0xFFF) << 10) | (rn << 5) | rt;
        emit32(insn);
    }
    // Stack operations
    void push(uint8_t rt) {
        // STR Xt, [SP, #-8]!  (pre-index)
        uint32_t insn = 0xF81F0C00 | (SP << 5) | rt;  // -8 in 9-bit signed
        emit32(insn);
    }
    
    void pop(uint8_t rt) {
        // LDR Xt, [SP], #8  (post-index)
        uint32_t insn = 0xF8408400 | (SP << 5) | rt;  // +8 post-index
        emit32(insn);
    }
    
    // Branch unconditional
    void b(int32_t offset) {
        // B label (26-bit signed immediate, shifted left 2)
        uint32_t imm26 = (offset >> 2) & 0x3FFFFFF;
        uint32_t insn = 0x14000000 | imm26;
        emit32(insn);
    }
    
    // Branch with link (call)
    void bl(int32_t offset) {
        // BL label
        uint32_t imm26 = (offset >> 2) & 0x3FFFFFF;
        uint32_t insn = 0x94000000 | imm26;
        emit32(insn);
    }
    
    // Branch to register
    void br(uint8_t rn) {
        // BR Xn
        uint32_t insn = 0xD61F0000 | (rn << 5);
        emit32(insn);
    }
    
    // Branch with link to register (indirect call)
    void blr(uint8_t rn) {
        // BLR Xn
        uint32_t insn = 0xD63F0000 | (rn << 5);
        emit32(insn);
    }
    
    // Return
    void ret() {
        // RET (defaults to X30/LR)
        uint32_t insn = 0xD65F03C0;
        emit32(insn);
    }
    
    // NOP
    void nop() {
        uint32_t insn = 0xD503201F;
        emit32(insn);
    }
    
    // BRK (debug breakpoint / trap)
    void brk(uint16_t imm16) {
        // BRK #imm16
        uint32_t insn = 0xD4200000 | (imm16 << 5);
        emit32(insn);
    }
    
    // ==========================================================================
    // Floating-Point Operations (Scalar)
    // ==========================================================================
    
    // FADD (scalar single)
    void faddS(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x1E202800 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FADD (scalar double)
    void faddD(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x1E602800 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FSUB (scalar single)
    void fsubS(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x1E203800 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FSUB (scalar double)
    void fsubD(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x1E603800 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FMUL (scalar single)
    void fmulS(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x1E200800 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FMUL (scalar double)
    void fmulD(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x1E600800 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FDIV (scalar single)
    void fdivS(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x1E201800 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FDIV (scalar double)
    void fdivD(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x1E601800 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FSQRT (scalar single)
    void fsqrtS(uint8_t vd, uint8_t vn) {
        uint32_t insn = 0x1E21C000 | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FSQRT (scalar double)
    void fsqrtD(uint8_t vd, uint8_t vn) {
        uint32_t insn = 0x1E61C000 | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FABS (scalar single)
    void fabsS(uint8_t vd, uint8_t vn) {
        uint32_t insn = 0x1E20C000 | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FABS (scalar double)
    void fabsD(uint8_t vd, uint8_t vn) {
        uint32_t insn = 0x1E60C000 | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FNEG (scalar single)
    void fnegS(uint8_t vd, uint8_t vn) {
        uint32_t insn = 0x1E214000 | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FNEG (scalar double)
    void fnegD(uint8_t vd, uint8_t vn) {
        uint32_t insn = 0x1E614000 | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FCMP (scalar single)
    void fcmpS(uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x1E202000 | (vm << 16) | (vn << 5);
        emit32(insn);
    }
    
    // FCMP (scalar double)
    void fcmpD(uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x1E602000 | (vm << 16) | (vn << 5);
        emit32(insn);
    }
    
    // Load/Store FP single: LDR S[vt], [Xn, #offset]
    void ldrS(uint8_t vt, uint8_t xn, int32_t offset) {
        uint32_t insn = 0xBD400000 | (((offset >> 2) & 0xFFF) << 10) | (xn << 5) | vt;
        emit32(insn);
    }
    
    void strS(uint8_t vt, uint8_t xn, int32_t offset) {
        uint32_t insn = 0xBD000000 | (((offset >> 2) & 0xFFF) << 10) | (xn << 5) | vt;
        emit32(insn);
    }
    
    // Load/Store FP double: LDR D[vt], [Xn, #offset]
    void ldrD(uint8_t vt, uint8_t xn, int32_t offset) {
        uint32_t insn = 0xFD400000 | (((offset >> 3) & 0xFFF) << 10) | (xn << 5) | vt;
        emit32(insn);
    }
    
    void strD(uint8_t vt, uint8_t xn, int32_t offset) {
        uint32_t insn = 0xFD000000 | (((offset >> 3) & 0xFFF) << 10) | (xn << 5) | vt;
        emit32(insn);
    }
    
    // ==========================================================================
    // Type Conversion Instructions
    // ==========================================================================
    
    // Sign-extend word to 64-bit: SXTW Xd, Wn
    void sxtw(uint8_t rd, uint8_t rn) {
        // SBFM Xd, Xn, #0, #31
        uint32_t insn = 0x93407C00 | (rn << 5) | rd;
        emit32(insn);
    }
    
    // Zero-extend word to 64-bit: UXTW Xd, Wn
    void uxtw(uint8_t rd, uint8_t rn) {
        // UBFM Xd, Xn, #0, #31 (or just use MOV W)
        uint32_t insn = 0xD3407C00 | (rn << 5) | rd;
        emit32(insn);
    }
    
    // Float to signed int (32-bit result, single source)
    void fcvtzs32S(uint8_t wd, uint8_t sn) {
        // FCVTZS Wd, Sn
        uint32_t insn = 0x1E380000 | (sn << 5) | wd;
        emit32(insn);
    }
    
    // Float to unsigned int (32-bit result, single source)
    void fcvtzu32S(uint8_t wd, uint8_t sn) {
        // FCVTZU Wd, Sn
        uint32_t insn = 0x1E390000 | (sn << 5) | wd;
        emit32(insn);
    }
    
    // Double to signed int (32-bit result, double source)
    void fcvtzs32D(uint8_t wd, uint8_t dn) {
        // FCVTZS Wd, Dn
        uint32_t insn = 0x1E780000 | (dn << 5) | wd;
        emit32(insn);
    }
    
    // Double to unsigned int (32-bit result, double source)
    void fcvtzu32D(uint8_t wd, uint8_t dn) {
        // FCVTZU Wd, Dn
        uint32_t insn = 0x1E790000 | (dn << 5) | wd;
        emit32(insn);
    }
    
    // Signed int to float (32-bit source, single result)
    void scvtfS32(uint8_t sd, uint8_t wn) {
        // SCVTF Sd, Wn
        uint32_t insn = 0x1E220000 | (wn << 5) | sd;
        emit32(insn);
    }
    
    // Unsigned int to float (32-bit source, single result)
    void ucvtfS32(uint8_t sd, uint8_t wn) {
        // UCVTF Sd, Wn
        uint32_t insn = 0x1E230000 | (wn << 5) | sd;
        emit32(insn);
    }
    
    // Signed int to double (32-bit source, double result)
    void scvtfD32(uint8_t dd, uint8_t wn) {
        // SCVTF Dd, Wn
        uint32_t insn = 0x1E620000 | (wn << 5) | dd;
        emit32(insn);
    }
    
    // Unsigned int to double (32-bit source, double result)
    void ucvtfD32(uint8_t dd, uint8_t wn) {
        // UCVTF Dd, Wn
        uint32_t insn = 0x1E630000 | (wn << 5) | dd;
        emit32(insn);
    }
    
    // Double to float (FCVT Sd, Dn)
    void fcvtSD(uint8_t sd, uint8_t dn) {
        uint32_t insn = 0x1E624000 | (dn << 5) | sd;
        emit32(insn);
    }
    
    // Float to double (FCVT Dd, Sn)
    void fcvtDS(uint8_t dd, uint8_t sn) {
        uint32_t insn = 0x1E22C000 | (sn << 5) | dd;
        emit32(insn);
    }
    // ==========================================================================
    // Bitwise Operations (32-bit versions)
    // ==========================================================================
    
    void and32(uint8_t rd, uint8_t rn, uint8_t rm) {
        uint32_t insn = 0x0A000000 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    void orr32(uint8_t rd, uint8_t rn, uint8_t rm) {
        uint32_t insn = 0x2A000000 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    void eor32(uint8_t rd, uint8_t rn, uint8_t rm) {
        uint32_t insn = 0x4A000000 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    // MVN = ORN Rd, XZR, Rm
    void mvn64(uint8_t rd, uint8_t rm) {
        uint32_t insn = 0xAA2003E0 | (rm << 16) | rd;
        emit32(insn);
    }
    
    void mvn32(uint8_t rd, uint8_t rm) {
        uint32_t insn = 0x2A2003E0 | (rm << 16) | rd;
        emit32(insn);
    }
    
    // Shifts (32-bit)
    void lsl32(uint8_t rd, uint8_t rn, uint8_t rm) {
        uint32_t insn = 0x1AC02000 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    void lsr32(uint8_t rd, uint8_t rn, uint8_t rm) {
        uint32_t insn = 0x1AC02400 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    void asr32(uint8_t rd, uint8_t rn, uint8_t rm) {
        uint32_t insn = 0x1AC02800 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    void ror32(uint8_t rd, uint8_t rn, uint8_t rm) {
        uint32_t insn = 0x1AC02C00 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    void ror64(uint8_t rd, uint8_t rn, uint8_t rm) {
        uint32_t insn = 0x9AC02C00 | (rm << 16) | (rn << 5) | rd;
        emit32(insn);
    }
    
    // Count leading zeros
    void clz32(uint8_t rd, uint8_t rn) {
        uint32_t insn = 0x5AC01000 | (rn << 5) | rd;
        emit32(insn);
    }
    
    void clz64(uint8_t rd, uint8_t rn) {
        uint32_t insn = 0xDAC01000 | (rn << 5) | rd;
        emit32(insn);
    }
    
    // Count trailing zeros
    void ctz32(uint8_t rd, uint8_t rn) {
        // RBIT + CLZ (no direct CTZ)
        uint32_t rbit = 0x5AC00000 | (rn << 5) | rd;
        emit32(rbit);
        clz32(rd, rd);
    }
    
    void ctz64(uint8_t rd, uint8_t rn) {
        uint32_t rbit = 0xDAC00000 | (rn << 5) | rd;
        emit32(rbit);
        clz64(rd, rd);
    }
    
    // Pop count (population count / bit count)
    void popcnt32(uint8_t rd, uint8_t rn) {
        // Use FMOV to vector, CNT, then ADDV (complex, stub for now)
        (void)rd; (void)rn;
    }
    
    // ==========================================================================
    // Calling Convention / Prologue / Epilogue
    // ==========================================================================
    
    // Store pair: STP Xt1, Xt2, [Xn, #offset]!
    void stpPre64(uint8_t rt1, uint8_t rt2, uint8_t rn, int32_t offset) {
        int32_t imm7 = (offset >> 3) & 0x7F;
        uint32_t insn = 0xA9800000 | (imm7 << 15) | (rt2 << 10) | (rn << 5) | rt1;
        emit32(insn);
    }
    
    // Load pair: LDP Xt1, Xt2, [Xn], #offset
    void ldpPost64(uint8_t rt1, uint8_t rt2, uint8_t rn, int32_t offset) {
        int32_t imm7 = (offset >> 3) & 0x7F;
        uint32_t insn = 0xA8C00000 | (imm7 << 15) | (rt2 << 10) | (rn << 5) | rt1;
        emit32(insn);
    }
    
    // Function prologue (standard ARM64 calling convention)
    void prologue(uint32_t frameSize) {
        // STP x29, x30, [sp, #-frameSize]!
        stpPre64(FP, LR, SP, -static_cast<int32_t>(frameSize));
        // MOV x29, sp
        mov64(FP, SP);
    }
    
    // Function epilogue
    void epilogue(uint32_t frameSize) {
        // LDP x29, x30, [sp], #frameSize
        ldpPost64(FP, LR, SP, frameSize);
        // RET
        ret();
    }
    
    // ==========================================================================
    // Conditional branches
    // ==========================================================================
    
    enum Condition : uint8_t {
        EQ = 0,  // Equal
        NE = 1,  // Not equal
        CS = 2, HS = 2,  // Carry set / unsigned higher or same
        CC = 3, LO = 3,  // Carry clear / unsigned lower
        MI = 4,  // Minus / Negative
        PL = 5,  // Plus / Positive or zero
        VS = 6,  // Overflow
        VC = 7,  // No overflow
        HI = 8,  // Unsigned higher
        LS = 9,  // Unsigned lower or same
        GE = 10, // Signed greater than or equal
        LT = 11, // Signed less than
        GT = 12, // Signed greater than
        LE = 13, // Signed less than or equal
        AL = 14, // Always
        NV = 15  // Never (architecture reserved)
    };
    
    void bCond(Condition cond, int32_t offset) {
        // B.cond label
        uint32_t imm19 = ((offset >> 2) & 0x7FFFF);
        uint32_t insn = 0x54000000 | (imm19 << 5) | cond;
        emit32(insn);
    }
    
    // Conditional select: CSEL Xd, Xn, Xm, cond
    void csel64(uint8_t rd, uint8_t rn, uint8_t rm, Condition cond) {
        uint32_t insn = 0x9A800000 | (rm << 16) | (cond << 12) | (rn << 5) | rd;
        emit32(insn);
    }
    
    void csel32(uint8_t rd, uint8_t rn, uint8_t rm, Condition cond) {
        uint32_t insn = 0x1A800000 | (rm << 16) | (cond << 12) | (rn << 5) | rd;
        emit32(insn);
    }
    
    // CSET: set 1 if condition true, 0 otherwise
    void cset64(uint8_t rd, Condition cond) {
        // CSINC Xd, XZR, XZR, invert(cond)
        Condition invCond = static_cast<Condition>(cond ^ 1);
        uint32_t insn = 0x9A9F07E0 | (invCond << 12) | rd;
        emit32(insn);
    }
    
    void cset32(uint8_t rd, Condition cond) {
        Condition invCond = static_cast<Condition>(cond ^ 1);
        uint32_t insn = 0x1A9F07E0 | (invCond << 12) | rd;
        emit32(insn);
    }
    
    // Alias for common usage (uses condition code directly)
    void cset(uint8_t rd, uint8_t condCode) {
        Condition cond = static_cast<Condition>(condCode);
        cset32(rd, cond);
    }
    
    // Compare and branch if not zero (64-bit)
    void cbnz64(uint8_t rt, int32_t offset) {
        // CBNZ Xt, label
        uint32_t imm19 = ((offset >> 2) & 0x7FFFF);
        uint32_t insn = 0xB5000000 | (imm19 << 5) | rt;
        emit32(insn);
    }
    
    // Test bit and branch if zero: TBZ Wt, #bit, label (bit 0-31, byte offset)
    static uint32_t encodeTbz32(uint8_t rt, uint8_t bit, int32_t offset) {
        uint32_t imm14 = ((offset >> 2) & 0x3FFF);
        return 0x36000000 | ((bit & 0x1Fu) << 19) | (imm14 << 5) | rt;
    }

    void tbz32(uint8_t rt, uint8_t bit, int32_t offset) {
        emit32(encodeTbz32(rt, bit, offset));
    }

    // Compare and branch if zero (64-bit)
    void cbz64(uint8_t rt, int32_t offset) {
        uint32_t imm19 = ((offset >> 2) & 0x7FFFF);
        uint32_t insn = 0xB4000000 | (imm19 << 5) | rt;
        emit32(insn);
    }
    
    // ORR immediate (64-bit) - simplified for small values
    void orrImm64(uint8_t rd, uint8_t rn, uint64_t imm) {
        // For simple case: ORR with small immediate (use MOV + ORR)
        // This is a simplified implementation
        if (imm <= 0xFFFF) {
            uint8_t temp = 16;  // Use x16 as scratch
            movImm64(temp, imm);
            orr64(rd, rn, temp);
        } else {
            orr64(rd, rn, rn);  // Fallback: no-op
        }
    }
    
    // AND immediate (64-bit)
    void andImm64(uint8_t rd, uint8_t rn, uint64_t imm) {
        // Simplified: use temp register for AND
        if (imm <= 0xFFFFFFFF) {
            uint8_t temp = 16;  // x16 as scratch
            movImm64(temp, imm);
            and64(rd, rn, temp);
        } else {
            and64(rd, rn, rn);  // Fallback
        }
    }
    
    // CMP immediate (64-bit) - compares Xn with immediate
    void cmpImm64(uint8_t rn, uint64_t imm) {
        // CMP Xn, #imm is encoded as SUBS XZR, Xn, #imm
        if (imm <= 0xFFF) {
            // SUBS XZR, Xn, #imm (immediate fits in 12 bits)
            uint32_t insn = 0xF1000000 | ((imm & 0xFFF) << 10) | (rn << 5) | 31;
            emit32(insn);
        } else {
            // Load immediate to temp and use CMP reg
            uint8_t temp = 16;
            movImm64(temp, imm);
            // SUBS XZR, Xn, Xm
            uint32_t insn = 0xEB000000 | (temp << 16) | (rn << 5) | 31;
            emit32(insn);
        }
    }
    
    // ==========================================================================
    // NEON SIMD Vector Instructions (v128 / Q registers)
    // ==========================================================================
    
    // Vector load: LDR Qd, [Xn, #offset]
    void ldrQ(uint8_t vd, uint8_t rn, int32_t offset = 0) {
        uint32_t imm12 = ((offset >> 4) & 0xFFF);
        uint32_t insn = 0x3DC00000 | (imm12 << 10) | (rn << 5) | vd;
        emit32(insn);
    }
    
    // Vector store: STR Qd, [Xn, #offset]
    void strQ(uint8_t vd, uint8_t rn, int32_t offset = 0) {
        uint32_t imm12 = ((offset >> 4) & 0xFFF);
        uint32_t insn = 0x3D800000 | (imm12 << 10) | (rn << 5) | vd;
        emit32(insn);
    }
    
    // Vector ADD (4S = 4x 32-bit integers)
    void addV4S(uint8_t vd, uint8_t vn, uint8_t vm) {
        // ADD Vd.4S, Vn.4S, Vm.4S
        uint32_t insn = 0x4EA08400 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector ADD (2D = 2x 64-bit integers)
    void addV2D(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x4EE08400 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector SUB (4S)
    void subV4S(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x6EA08400 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector SUB (2D)
    void subV2D(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x6EE08400 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector MUL (4S) - Note: no 2D integer multiply in NEON
    void mulV4S(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x4EA09C00 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector AND (bitwise)
    void andV(uint8_t vd, uint8_t vn, uint8_t vm) {
        // AND Vd.16B, Vn.16B, Vm.16B
        uint32_t insn = 0x4E201C00 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector ORR (bitwise)
    void orrV(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x4EA01C00 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector EOR (bitwise XOR)
    void eorV(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x6E201C00 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector NOT (bitwise)
    void notV(uint8_t vd, uint8_t vn) {
        // NOT Vd.16B, Vn.16B
        uint32_t insn = 0x6E205800 | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector FADD (4S = 4x float32)
    void faddV4S(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x4E20D400 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector FADD (2D = 2x float64)
    void faddV2D(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x4E60D400 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector FSUB (4S)
    void fsubV4S(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x4EA0D400 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector FSUB (2D)
    void fsubV2D(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x4EE0D400 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector FMUL (4S)
    void fmulV4S(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x6E20DC00 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector FMUL (2D)
    void fmulV2D(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x6E60DC00 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector FDIV (4S)
    void fdivV4S(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x6E20FC00 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector FDIV (2D)
    void fdivV2D(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x6E60FC00 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector FSQRT (4S)
    void fsqrtV4S(uint8_t vd, uint8_t vn) {
        uint32_t insn = 0x6EA1F800 | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector FSQRT (2D)
    void fsqrtV2D(uint8_t vd, uint8_t vn) {
        uint32_t insn = 0x6EE1F800 | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector compare equal (4S) - returns all 1s or all 0s per lane
    void cmeqV4S(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x6EA08C00 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector compare greater than (4S signed)
    void cmgtV4S(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x4EA03400 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector compare greater or equal (4S signed)
    void cmgeV4S(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x4EA03C00 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector DUP (scalar to all lanes) - GPR to 4S
    void dupV4S(uint8_t vd, uint8_t rn) {
        // DUP Vd.4S, Wn
        uint32_t insn = 0x4E040C00 | (rn << 5) | vd;
        emit32(insn);
    }
    
    // Vector DUP (scalar to all lanes) - GPR to 2D
    void dupV2D(uint8_t vd, uint8_t rn) {
        // DUP Vd.2D, Xn
        uint32_t insn = 0x4E080C00 | (rn << 5) | vd;
        emit32(insn);
    }
    
    // Vector move register
    void movV(uint8_t vd, uint8_t vn) {
        // ORR Vd.16B, Vn.16B, Vn.16B
        orrV(vd, vn, vn);
    }
    
    // Vector extract lane to GPR (4S)
    void umovW(uint8_t rd, uint8_t vn, uint8_t lane) {
        // UMOV Wd, Vn.S[lane]
        uint32_t imm5 = ((lane & 0x3) << 3) | 0x4;  // size=S
        uint32_t insn = 0x0E003C00 | (imm5 << 16) | (vn << 5) | rd;
        emit32(insn);
    }
    
    // Vector insert lane from GPR (4S)
    void insV4S(uint8_t vd, uint8_t lane, uint8_t rn) {
        // INS Vd.S[lane], Wn
        uint32_t imm5 = ((lane & 0x3) << 3) | 0x4;
        uint32_t insn = 0x4E001C00 | (imm5 << 16) | (rn << 5) | vd;
        emit32(insn);
    }
    
    // Vector TBL (table lookup for shuffle)
    void tblV16B(uint8_t vd, uint8_t vn, uint8_t vm) {
        // TBL Vd.16B, {Vn.16B}, Vm.16B
        uint32_t insn = 0x4E000000 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector ZIP1 (interleave low halves)
    void zip1V4S(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x4E803800 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Vector ZIP2 (interleave high halves)
    void zip2V4S(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x4E807800 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Load quad (128-bit) - placeholder for SIMD
    void ldq(uint8_t vd, uint8_t rn, int32_t offset = 0) {
        // LDR Q (128-bit load): 0x3DC00000 | imm12 | Rn | Rt
        uint32_t imm = (offset >> 4) & 0xFFF;
        uint32_t insn = 0x3DC00000 | (imm << 10) | (rn << 5) | vd;
        emit32(insn);
    }
    
    // Table lookup (TBL) - single register table
    void tbl(uint8_t vd, uint8_t vn, uint8_t vm) {
        // TBL Vd.16B, {Vn.16B}, Vm.16B: 0x4E000000 | Vm | 0 | Vn | Vd
        uint32_t insn = 0x4E000000 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Table lookup (TBL) - two register table
    void tbl2(uint8_t vd, uint8_t vn, uint8_t vm) {
        // TBL Vd.16B, {Vn.16B, Vn+1.16B}, Vm.16B: 0x4E002000 | Vm | Vn | Vd
        uint32_t insn = 0x4E002000 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // DUP element to vector (byte)
    void dupVB(uint8_t vd, uint8_t vn, uint8_t index) {
        // DUP Vd.16B, Vn.B[index]: 0x4E010400 | (index << 17) | Vn | Vd
        uint32_t imm5 = (index << 1) | 1;  // B = xxx01
        uint32_t insn = 0x4E000400 | (imm5 << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // DUP element to vector (half)
    void dupVH(uint8_t vd, uint8_t vn, uint8_t index) {
        uint32_t imm5 = (index << 2) | 2;  // H = xx010
        uint32_t insn = 0x4E000400 | (imm5 << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // DUP element to vector (single)
    void dupVS(uint8_t vd, uint8_t vn, uint8_t index) {
        uint32_t imm5 = (index << 3) | 4;  // S = x0100
        uint32_t insn = 0x4E000400 | (imm5 << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // DUP element to vector (double)
    void dupVD(uint8_t vd, uint8_t vn, uint8_t index) {
        uint32_t imm5 = (index << 4) | 8;  // D = 01000
        uint32_t insn = 0x4E000400 | (imm5 << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // SMOV - signed move from element to register (byte)
    void smovB(uint8_t rd, uint8_t vn, uint8_t index) {
        uint32_t imm5 = (index << 1) | 1;
        uint32_t insn = 0x4E002C00 | (imm5 << 16) | (vn << 5) | rd;
        emit32(insn);
    }
    
    // SMOV - signed move from element to register (half)
    void smovH(uint8_t rd, uint8_t vn, uint8_t index) {
        uint32_t imm5 = (index << 2) | 2;
        uint32_t insn = 0x4E002C00 | (imm5 << 16) | (vn << 5) | rd;
        emit32(insn);
    }
    
    // UMOV - unsigned move from element to register (byte)
    void umovB(uint8_t rd, uint8_t vn, uint8_t index) {
        uint32_t imm5 = (index << 1) | 1;
        uint32_t insn = 0x0E003C00 | (imm5 << 16) | (vn << 5) | rd;
        emit32(insn);
    }
    
    // UMOV - unsigned move from element to register (half)
    void umovH(uint8_t rd, uint8_t vn, uint8_t index) {
        uint32_t imm5 = (index << 2) | 2;
        uint32_t insn = 0x0E003C00 | (imm5 << 16) | (vn << 5) | rd;
        emit32(insn);
    }
    
    // UMOV - unsigned move from element to register (single/word)
    void umovS(uint8_t rd, uint8_t vn, uint8_t index) {
        uint32_t imm5 = (index << 3) | 4;
        uint32_t insn = 0x0E003C00 | (imm5 << 16) | (vn << 5) | rd;
        emit32(insn);
    }
    
    // UMOV - unsigned move from element to register (double)
    void umovD(uint8_t rd, uint8_t vn, uint8_t index) {
        uint32_t imm5 = (index << 4) | 8;
        uint32_t insn = 0x4E003C00 | (imm5 << 16) | (vn << 5) | rd;
        emit32(insn);
    }
    
    // INS - insert element from register (byte)
    void insB(uint8_t vd, uint8_t index, uint8_t rn) {
        uint32_t imm5 = (index << 1) | 1;
        uint32_t insn = 0x4E001C00 | (imm5 << 16) | (rn << 5) | vd;
        emit32(insn);
    }
    
    // INS - insert element from register (half)
    void insH(uint8_t vd, uint8_t index, uint8_t rn) {
        uint32_t imm5 = (index << 2) | 2;
        uint32_t insn = 0x4E001C00 | (imm5 << 16) | (rn << 5) | vd;
        emit32(insn);
    }
    
    // INS - insert element from register (single)
    void insS(uint8_t vd, uint8_t index, uint8_t rn) {
        uint32_t imm5 = (index << 3) | 4;
        uint32_t insn = 0x4E001C00 | (imm5 << 16) | (rn << 5) | vd;
        emit32(insn);
    }
    
    // INS - insert element from register (double)
    void insD(uint8_t vd, uint8_t index, uint8_t rn) {
        uint32_t imm5 = (index << 4) | 8;
        uint32_t insn = 0x4E001C00 | (imm5 << 16) | (rn << 5) | vd;
        emit32(insn);
    }
    
    // DUP from GPR to all lanes (2-arg overloads for splat)
    void dupVB(uint8_t vd, uint8_t rn) {
        uint32_t insn = 0x4E010C00 | (rn << 5) | vd;
        emit32(insn);
    }
    
    void dupVH(uint8_t vd, uint8_t rn) {
        uint32_t insn = 0x4E020C00 | (rn << 5) | vd;
        emit32(insn);
    }
    
    void dupVS(uint8_t vd, uint8_t rn) {
        uint32_t insn = 0x4E040C00 | (rn << 5) | vd;
        emit32(insn);
    }
    
    void dupVD(uint8_t vd, uint8_t rn) {
        uint32_t insn = 0x4E080C00 | (rn << 5) | vd;
        emit32(insn);
    }
    
    // DUP lane aliases
    void dupVS_lane(uint8_t vd, uint8_t vn, uint8_t lane) { dupVS(vd, vn, lane); }
    void dupVD_lane(uint8_t vd, uint8_t vn, uint8_t lane) { dupVD(vd, vn, lane); }
    
    // INS from SIMD element
    void insSFromS(uint8_t vd, uint8_t dstIdx, uint8_t vn, uint8_t srcIdx) {
        uint32_t imm5 = (dstIdx << 3) | 4;
        uint32_t imm4 = srcIdx << 2;
        uint32_t insn = 0x6E000400 | (imm5 << 16) | (imm4 << 11) | (vn << 5) | vd;
        emit32(insn);
    }
    
    void insDFromD(uint8_t vd, uint8_t dstIdx, uint8_t vn, uint8_t srcIdx) {
        uint32_t imm5 = (dstIdx << 4) | 8;
        uint32_t imm4 = srcIdx << 3;
        uint32_t insn = 0x6E000400 | (imm5 << 16) | (imm4 << 11) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Compare equal 16B
    void cmeq16B(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x6E208C00 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Bit select
    void bsl(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x6E601C00 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Unsigned max across vector
    void umaxv(uint8_t vd, uint8_t vn) {
        uint32_t insn = 0x6E30A800 | (vn << 5) | vd;
        emit32(insn);
    }
    
    // LDQ from constant bytes (load literal into vector)
    void ldq(uint8_t vd, const uint8_t* bytes) {
        // Encode bytes inline or use literal pool
        // For now, use mov/ins to build constant
        (void)bytes;
        // Placeholder - actual impl would emit bytes to code and load
        uint32_t insn = 0x9C000000 | vd;  // LDUR Q
        emit32(insn);
    }
    
    // TBL with 4 args (2-register table + indices)
    void tbl2(uint8_t vd, uint8_t vn, uint8_t vn2, const uint8_t* indices) {
        (void)indices;  // Indices would be loaded separately
        (void)vn2;
        uint32_t insn = 0x4E002000 | (vn << 5) | vd;
        emit32(insn);
    }
    
    // INS element-to-element (3 args - simplified)  
    void insSFromS(uint8_t vd, uint8_t idx, uint8_t vn) {
        uint32_t imm5 = (idx << 3) | 4;
        uint32_t insn = 0x6E000400 | (imm5 << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    void insDFromD(uint8_t vd, uint8_t idx, uint8_t vn) {
        uint32_t imm5 = (idx << 4) | 8;
        uint32_t insn = 0x6E000400 | (imm5 << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // BSL with 4 args (result, true, false, mask)
    void bsl(uint8_t vd, uint8_t vt, uint8_t vf, uint8_t vm) {
        (void)vt; (void)vf;  // Real impl would handle properly
        uint32_t insn = 0x6E601C00 | (vm << 16) | (vd << 5) | vd;
        emit32(insn);
    }
    
    // CMP immediate 32-bit
    void cmpImm32(uint8_t rn, uint32_t imm) {
        // CMP Wn, #imm is SUBS WZR, Wn, #imm
        if (imm <= 0xFFF) {
            uint32_t insn = 0x7100001F | (imm << 10) | (rn << 5);
            emit32(insn);
        }
    }
    
    // SMULL (signed multiply long)
    void smull(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x0E20C000 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // SMLAL2 (signed multiply-accumulate long, high half)
    void smlal2(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x4E208000 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // SADDLP (signed add long pairwise)
    void saddlp(uint8_t vd, uint8_t vn) {
        uint32_t insn = 0x0E202800 | (vn << 5) | vd;
        emit32(insn);
    }
    
    // STR pre-indexed
    void strPre(uint8_t rt, uint8_t rn, int16_t offset) {
        uint32_t imm9 = offset & 0x1FF;
        uint32_t insn = 0xF8000C00 | (imm9 << 12) | (rn << 5) | rt;
        emit32(insn);
    }
    
    // LDR post-indexed
    void ldrPost(uint8_t rt, uint8_t rn, int16_t offset) {
        uint32_t imm9 = offset & 0x1FF;
        uint32_t insn = 0xF8400400 | (imm9 << 12) | (rn << 5) | rt;
        emit32(insn);
    }
    
    // LDAR - Load-Acquire Register 64-bit
    void ldar64(uint8_t rt, uint8_t rn) {
        uint32_t insn = 0xC8DFFC00 | (rn << 5) | rt;
        emit32(insn);
    }
    
    // LDAR - Load-Acquire Register 32-bit
    void ldar32(uint8_t rt, uint8_t rn) {
        uint32_t insn = 0x88DFFC00 | (rn << 5) | rt;
        emit32(insn);
    }
    
    // STLR - Store-Release Register 64-bit
    void stlr64(uint8_t rt, uint8_t rn) {
        uint32_t insn = 0xC89FFC00 | (rn << 5) | rt;
        emit32(insn);
    }
    
    // STLR - Store-Release Register 32-bit
    void stlr32(uint8_t rt, uint8_t rn) {
        uint32_t insn = 0x889FFC00 | (rn << 5) | rt;
        emit32(insn);
    }
    
    // LDAXR - Load-Acquire Exclusive Register 64-bit
    void ldaxr64(uint8_t rt, uint8_t rn) {
        uint32_t insn = 0xC85FFC00 | (rn << 5) | rt;
        emit32(insn);
    }
    
    // STLXR - Store-Release Exclusive Register 64-bit
    void stlxr64(uint8_t rs, uint8_t rt, uint8_t rn) {
        uint32_t insn = 0xC800FC00 | (rs << 16) | (rn << 5) | rt;
        emit32(insn);
    }
    
    // LSE Atomics - Load-Add Acquire-Release
    void ldaddal64(uint8_t rs, uint8_t rt, uint8_t rn) {
        uint32_t insn = 0xF8E00000 | (rs << 16) | (rn << 5) | rt;
        emit32(insn);
    }
    
    void ldaddal32(uint8_t rs, uint8_t rt, uint8_t rn) {
        uint32_t insn = 0xB8E00000 | (rs << 16) | (rn << 5) | rt;
        emit32(insn);
    }
    
    // LSE Atomics - Load-Clear Acquire-Release
    void ldclral64(uint8_t rs, uint8_t rt, uint8_t rn) {
        uint32_t insn = 0xF8E01000 | (rs << 16) | (rn << 5) | rt;
        emit32(insn);
    }
    
    void ldclral32(uint8_t rs, uint8_t rt, uint8_t rn) {
        uint32_t insn = 0xB8E01000 | (rs << 16) | (rn << 5) | rt;
        emit32(insn);
    }
    
    // LSE Atomics - Load-Set Acquire-Release
    void ldsetal64(uint8_t rs, uint8_t rt, uint8_t rn) {
        uint32_t insn = 0xF8E03000 | (rs << 16) | (rn << 5) | rt;
        emit32(insn);
    }
    
    void ldsetal32(uint8_t rs, uint8_t rt, uint8_t rn) {
        uint32_t insn = 0xB8E03000 | (rs << 16) | (rn << 5) | rt;
        emit32(insn);
    }
    
    // LSE Atomics - Load-Xor Acquire-Release
    void ldeoral64(uint8_t rs, uint8_t rt, uint8_t rn) {
        uint32_t insn = 0xF8E02000 | (rs << 16) | (rn << 5) | rt;
        emit32(insn);
    }
    
    void ldeoral32(uint8_t rs, uint8_t rt, uint8_t rn) {
        uint32_t insn = 0xB8E02000 | (rs << 16) | (rn << 5) | rt;
        emit32(insn);
    }
    
    // LSE Atomics - Swap Acquire-Release
    void swpal64(uint8_t rs, uint8_t rt, uint8_t rn) {
        uint32_t insn = 0xF8E08000 | (rs << 16) | (rn << 5) | rt;
        emit32(insn);
    }
    
    void swpal32(uint8_t rs, uint8_t rt, uint8_t rn) {
        uint32_t insn = 0xB8E08000 | (rs << 16) | (rn << 5) | rt;
        emit32(insn);
    }
    
    // LSE Atomics - Compare and Swap Acquire-Release
    void casal64(uint8_t rs, uint8_t rt, uint8_t rn) {
        uint32_t insn = 0xC8E0FC00 | (rs << 16) | (rn << 5) | rt;
        emit32(insn);
    }
    
    void casal32(uint8_t rs, uint8_t rt, uint8_t rn) {
        uint32_t insn = 0x88E0FC00 | (rs << 16) | (rn << 5) | rt;
        emit32(insn);
    }
    
    // DMB - Data Memory Barrier
    void dmb(uint8_t option = 0xB) {  // 0xB = ISH (inner shareable)
        uint32_t insn = 0xD5033BBF | (option << 8);
        emit32(insn);
    }
    
    // DSB - Data Synchronization Barrier
    void dsb(uint8_t option = 0xB) {
        uint32_t insn = 0xD5033B9F | (option << 8);
        emit32(insn);
    }
    
    // ISB - Instruction Synchronization Barrier
    void isb() {
        uint32_t insn = 0xD5033FDF;
        emit32(insn);
    }
    
    // ==========================================================================
    // ==========================================================================
    
    // FP absolute value (vector)
    void fabsV(uint8_t vd, uint8_t vn, bool is64 = false) {
        uint32_t sz = is64 ? 1 : 0;
        uint32_t insn = 0x4EA0F800 | (sz << 22) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FP negate (vector)
    void fnegV(uint8_t vd, uint8_t vn, bool is64 = false) {
        uint32_t sz = is64 ? 1 : 0;
        uint32_t insn = 0x6EA0F800 | (sz << 22) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FP square root (vector)
    void fsqrtV(uint8_t vd, uint8_t vn, bool is64 = false) {
        uint32_t sz = is64 ? 1 : 0;
        uint32_t insn = 0x6EA1F800 | (sz << 22) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FP add (vector)
    void faddV(uint8_t vd, uint8_t vn, uint8_t vm, bool is64 = false) {
        uint32_t sz = is64 ? 1 : 0;
        uint32_t insn = 0x4E20D400 | (sz << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FP subtract (vector)
    void fsubV(uint8_t vd, uint8_t vn, uint8_t vm, bool is64 = false) {
        uint32_t sz = is64 ? 1 : 0;
        uint32_t insn = 0x4EA0D400 | (sz << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FP multiply (vector)
    void fmulV(uint8_t vd, uint8_t vn, uint8_t vm, bool is64 = false) {
        uint32_t sz = is64 ? 1 : 0;
        uint32_t insn = 0x6E20DC00 | (sz << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FP divide (vector)
    void fdivV(uint8_t vd, uint8_t vn, uint8_t vm, bool is64 = false) {
        uint32_t sz = is64 ? 1 : 0;
        uint32_t insn = 0x6E20FC00 | (sz << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FP minimum (vector)
    void fminV(uint8_t vd, uint8_t vn, uint8_t vm, bool is64 = false) {
        uint32_t sz = is64 ? 1 : 0;
        uint32_t insn = 0x4EA0F400 | (sz << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FP maximum (vector)
    void fmaxV(uint8_t vd, uint8_t vn, uint8_t vm, bool is64 = false) {
        uint32_t sz = is64 ? 1 : 0;
        uint32_t insn = 0x4E20F400 | (sz << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // ==========================================================================
    // FP Convert Operations
    // ==========================================================================
    
    // Convert FP to higher precision (S->D)
    void fcvtl(uint8_t vd, uint8_t vn) {
        uint32_t insn = 0x0E217800 | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Convert FP to lower precision (D->S)
    void fcvtn(uint8_t vd, uint8_t vn) {
        uint32_t insn = 0x0E216800 | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Signed convert to FP (vector)
    void scvtfV(uint8_t vd, uint8_t vn, bool is64 = false) {
        uint32_t sz = is64 ? 1 : 0;
        uint32_t insn = 0x4E21D800 | (sz << 22) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Unsigned convert to FP (vector)
    void ucvtfV(uint8_t vd, uint8_t vn, bool is64 = false) {
        uint32_t sz = is64 ? 1 : 0;
        uint32_t insn = 0x6E21D800 | (sz << 22) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FP convert to signed int, round toward zero (vector)
    void fcvtzsV(uint8_t vd, uint8_t vn, bool is64 = false) {
        uint32_t sz = is64 ? 1 : 0;
        uint32_t insn = 0x4EA1B800 | (sz << 22) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FP convert to unsigned int, round toward zero (vector)
    void fcvtzuV(uint8_t vd, uint8_t vn, bool is64 = false) {
        uint32_t sz = is64 ? 1 : 0;
        uint32_t insn = 0x6EA1B800 | (sz << 22) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // ==========================================================================
    // Saturating Arithmetic
    // ==========================================================================
    
    // Signed saturating add (vector)
    void sqaddV(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x4E200C00 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Unsigned saturating add (vector)
    void uqaddV(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x6E200C00 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Signed saturating subtract (vector)
    void sqsubV(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x4E202C00 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Unsigned saturating subtract (vector)
    void uqsubV(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x6E202C00 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Signed saturating doubling multiply long (16->32)
    void sqdmullV(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x0E60D000 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Signed saturating doubling multiply high (vector)
    void sqdmulhV(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x0E60B400 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Signed saturating rounding doubling multiply high (vector)
    void sqrdmulhV(uint8_t vd, uint8_t vn, uint8_t vm) {
        uint32_t insn = 0x6E60B400 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // ==========================================================================
    // Integer SIMD Arithmetic
    // ==========================================================================
    
    // Add (vector)
    void addV(uint8_t vd, uint8_t vn, uint8_t vm, uint8_t size = 2) {
        uint32_t insn = 0x4E208400 | (size << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Subtract (vector)
    void subV(uint8_t vd, uint8_t vn, uint8_t vm, uint8_t size = 2) {
        uint32_t insn = 0x6E208400 | (size << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Multiply (vector)
    void mulV(uint8_t vd, uint8_t vn, uint8_t vm, uint8_t size = 2) {
        uint32_t insn = 0x4E209C00 | (size << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Signed shift left (vector)
    void sshlV(uint8_t vd, uint8_t vn, uint8_t vm, uint8_t size = 2) {
        uint32_t insn = 0x4E204400 | (size << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Unsigned shift left (vector)
    void ushlV(uint8_t vd, uint8_t vn, uint8_t vm, uint8_t size = 2) {
        uint32_t insn = 0x6E204400 | (size << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // ==========================================================================
    // SIMD Compare
    // ==========================================================================
    
    // Compare equal (vector)
    void cmeqV(uint8_t vd, uint8_t vn, uint8_t vm, uint8_t size = 2) {
        uint32_t insn = 0x6E208C00 | (size << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Compare signed greater than (vector)
    void cmgtV(uint8_t vd, uint8_t vn, uint8_t vm, uint8_t size = 2) {
        uint32_t insn = 0x4E203400 | (size << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Compare signed greater than or equal (vector)
    void cmgeV(uint8_t vd, uint8_t vn, uint8_t vm, uint8_t size = 2) {
        uint32_t insn = 0x4E203C00 | (size << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Compare unsigned higher (vector)
    void cmhiV(uint8_t vd, uint8_t vn, uint8_t vm, uint8_t size = 2) {
        uint32_t insn = 0x6E203400 | (size << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Compare unsigned higher or same (vector)
    void cmhsV(uint8_t vd, uint8_t vn, uint8_t vm, uint8_t size = 2) {
        uint32_t insn = 0x6E203C00 | (size << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FP compare equal (vector)
    void fcmeqV(uint8_t vd, uint8_t vn, uint8_t vm, bool is64 = false) {
        uint32_t sz = is64 ? 1 : 0;
        uint32_t insn = 0x4E20E400 | (sz << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FP compare greater than (vector)
    void fcmgtV(uint8_t vd, uint8_t vn, uint8_t vm, bool is64 = false) {
        uint32_t sz = is64 ? 1 : 0;
        uint32_t insn = 0x6EA0E400 | (sz << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // FP compare greater than or equal (vector)
    void fcmgeV(uint8_t vd, uint8_t vn, uint8_t vm, bool is64 = false) {
        uint32_t sz = is64 ? 1 : 0;
        uint32_t insn = 0x6E20E400 | (sz << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // ==========================================================================
    // Relaxed SIMD Operations (WASM Relaxed SIMD Proposal)
    // ==========================================================================
    
    // Fused multiply-add: vd = va + (vn * vm)
    void fmlaV(uint8_t vd, uint8_t vn, uint8_t vm, bool is64 = false) {
        uint32_t sz = is64 ? 1 : 0;
        // FMLA (vector) encoding: 0E20CC00
        uint32_t insn = 0x0E20CC00 | (1 << 30) | (sz << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Fused multiply-subtract: vd = va - (vn * vm)
    void fmlsV(uint8_t vd, uint8_t vn, uint8_t vm, bool is64 = false) {
        uint32_t sz = is64 ? 1 : 0;
        // FMLS (vector) encoding: 0EA0CC00
        uint32_t insn = 0x0EA0CC00 | (1 << 30) | (sz << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Table lookup (swizzle): vd[i] = vm[vn[i]]
    void tblV(uint8_t vd, uint8_t vn, uint8_t vm) {
        // TBL (single register table): 0E000000
        uint32_t insn = 0x0E000000 | (1 << 30) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Relaxed min (returns NaN propagation or implementation-defined)
    void fminnmV(uint8_t vd, uint8_t vn, uint8_t vm, bool is64 = false) {
        uint32_t sz = is64 ? 1 : 0;
        // FMINNM (vector): 4EA0C400
        uint32_t insn = 0x4EA0C400 | (sz << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Relaxed max (returns NaN propagation or implementation-defined)
    void fmaxnmV(uint8_t vd, uint8_t vn, uint8_t vm, bool is64 = false) {
        uint32_t sz = is64 ? 1 : 0;
        // FMAXNM (vector): 4E20C400
        uint32_t insn = 0x4E20C400 | (sz << 22) | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Relaxed integer dot product i16x8 -> i32x4
    void sdot(uint8_t vd, uint8_t vn, uint8_t vm) {
        // SDOT (vector): 4E809400
        uint32_t insn = 0x4E809400 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    // Relaxed lane select (bitselect)
    void bslV(uint8_t vd, uint8_t vn, uint8_t vm) {
        // BSL: 6E601C00
        uint32_t insn = 0x6E601C00 | (vm << 16) | (vn << 5) | vd;
        emit32(insn);
    }
    
    size_t offset() const { return buf_->offset(); }
    
private:
    CodeBuffer* buf_;
};

} // namespace Zepra::Wasm
