# ZepraScript JIT Tier Architecture

> **Engine:** ZepraScript 1.2.0
> **Last Updated:** 2026-04-18

---

## 1. Overview

ZepraScript uses a multi-tier execution pipeline. All code starts in the interpreter.
As functions become hot (via call count or loop iteration), they are promoted to
progressively more optimized tiers. Speculation failures cause demotion back to
the interpreter.

```
         ┌──────────────────────────────────┐
         │         Interpreter              │  Tier 0 — always available
         │  Computed-goto bytecode dispatch │
         └──────────────┬───────────────────┘
                        │ 100 calls / 200 loop iterations
         ┌──────────────▼───────────────────┐
         │        Baseline JIT              │  Tier 1 — fast compile, ~1ms
         │  1:1 bytecode → x86-64 mapping   │
         │  Inline cache stubs              │
         └──────────────┬───────────────────┘
                        │ 1000 calls / 5000 loop iterations
         ┌──────────────▼───────────────────┐
         │         ZOpt JIT                 │  Tier 2 — optimizing, ~10ms
         │  Sea-of-nodes IR                 │
         │  Type specialization, inlining   │
         └──────────────┬───────────────────┘
                        │ 10000 calls + ZOpt hotness
         ┌──────────────▼───────────────────┐
         │         ZIR / FTL                │  Tier 3 — maximum, ~50ms
         │  Low-level SSA IR                │
         │  Full register allocation, SIMD  │
         └──────────────────────────────────┘
                        │ speculation failure
                        ▼
                  Interpreter (bail-out, re-profile)
```

---

## 2. Tier Details

### Tier 0: Interpreter

**Location:** `interpreter/`

- Computed-goto threaded dispatch (6 files, ~1.5K lines)
- Bytecode handlers for all 96 opcodes
- GC safe-points every ~1K instructions
- Type feedback collection for JIT profiling
- Debug stepping support (StepInto/StepOver/StepOut)

**Cost:** Zero compilation overhead. Lowest throughput.

### Tier 1: Baseline JIT

**Location:** `jit/baseline_jit.cpp`, `jit/baseline_jit.hpp`

- One-pass bytecode → x86-64 native code translation
- No IR construction, no optimization passes
- Inline cache stubs for property access (monomorphic)
- x86-64 register encoding via `CodeBuffer`
- Executable memory allocation via `mmap` with `PROT_EXEC`

**Key classes:**
- `BaselineJIT` — compiler entry point, takes `BytecodeChunk`, returns `CompiledCode`
- `CompiledCode` — owns executable memory, provides `entryPoint()`
- `CodeBuffer` — byte-level x86-64 instruction emission

**Compile cost:** ~1ms. ~2-5x interpreter throughput.

### Tier 2: ZOpt JIT (Zepra Optimizer)

**Location:** `zopt/`

- Sea-of-nodes intermediate representation
- Type specialization based on profiler feedback
- Optimization passes (header-only currently):
  - `ZOptConstFold` — constant folding
  - `ZOptDeadCodeElim` — dead code elimination
  - `ZOptCopyProp` — copy propagation
  - `ZOptCommonSubexpr` — common subexpression elimination
  - `ZOptLoopInvariant` — loop-invariant code motion
  - `ZOptStrengthReduce` — strength reduction
  - `EscapeAnalysis` — scalar replacement of aggregates
  - `SpeculativeInliner` — speculative function inlining

**Key classes:**
- `ZOptGraph` — sea-of-nodes graph
- `ZOptBuilder` — IR construction from bytecode + type feedback
- `ZOptPipeline` — pass scheduling and execution
- `ZOptLowering` — IR → machine code lowering
- `ZOptBasicBlock` — control flow representation
- `ZOptValue` — SSA value representation
- `ZOptOpcode` — IR opcode definitions

**Compile cost:** ~10ms. ~10-30x interpreter throughput.

### Tier 3: ZIR / FTL (Zepra Intermediate Representation)

**Location:** `zir/`

- Low-level SSA IR backend for maximum optimization
- Full register allocation (graph coloring)
- Instruction scheduling and SIMD vectorization
- All files are currently header-only declarations

**Key headers:**
- `ZIRGenerate` — IR generation from ZOpt graph
- `ZIRLowering` — IR → native lowering
- `ZIROpcode` — low-level opcode set
- `ZIRValue` — typed SSA values
- `ZIRProcedure` — function-level IR container
- `ZIRBasicBlock` — basic block representation
- `ZIRReduceStrength` — algebraic simplifications
- `ZIREliminateDeadCode` — DCE pass
- `ZIRPhiElimination` — phi node removal before regalloc
- `ZIRLoopPeeling` — first iteration peeling
- `ZIRTailDuplication` — tail duplication for branch reduction

**Compile cost:** ~50ms. ~50-100x interpreter throughput.

---

## 3. Tiering Policy

**Location:** `jit/jit_tier_policy.cpp`

The `TierPolicy` class manages tier-up decisions with these configurable thresholds:

| Parameter | Default | Description |
|---|---|---|
| `baselineCallThreshold` | 100 | Calls before baseline compilation |
| `optimizingCallThreshold` | 1000 | Calls before optimizing compilation |
| `baselineLoopThreshold` | 200 | Loop iterations for OSR to baseline |
| `optimizingLoopThreshold` | 5000 | Loop iterations for OSR to optimized |
| `maxDeoptCount` | 10 | Deopts before blacklisting function |
| `maxBytecodeSizeForInline` | 200 | Max bytecode bytes for inlining |
| `maxCompileTimeBudgetMs` | 50 | Max time per compilation |

### Blacklisting

Functions that deoptimize more than `maxDeoptCount` times are **blacklisted** — demoted
to Baseline tier permanently. This prevents repeated compile-deopt cycles on
pathologically polymorphic code.

### Inlining Policy

A function is eligible for inlining when:
- Bytecode size ≤ `maxBytecodeSizeForInline` (200 bytes)
- Call count ≥ 10
- Not blacklisted

---

## 4. Profiling

**Location:** `jit/jit_profiler.cpp`, `jit/jit_profiler.hpp`

The `JITProfiler` collects lightweight runtime data:

- **Call counts** — incremented on every `OP_CALL` in the VM
- **Loop iterations** — incremented on `OP_LOOP` back-edges
- Bounded to 1024 tracked functions (~16KB memory)
- Hot threshold: 100 calls → baseline candidate
- Very hot threshold: 1000 calls → optimization candidate

### Type Profiling

**Location:** `jit/type_profiler.cpp`, `jit/type_profiler.hpp`

Records observed value types at key sites:
- Property access receivers
- Arithmetic operand types
- Call target shapes

Type feedback feeds into ZOpt speculative optimizations.

---

## 5. On-Stack Replacement (OSR)

**Location:** `jit/osr.cpp`, `jit/osr.hpp`

OSR allows mid-execution transition from interpreter to JIT code, typically
at loop headers. The `OSRManager` tracks entry points mapping bytecode offsets
to native code addresses.

**Process:**
1. Interpreter detects hot loop (back-edge count exceeds threshold)
2. Baseline JIT compiles the function, recording OSR entry points
3. At next loop header hit, interpreter transfers to native code
4. Local variables and operand stack are mapped via `stackMap`

---

## 6. Deoptimization

**Location:** `jit/osr.hpp`, `jit/deoptimizer.cpp`

When a JIT speculation fails, execution bails back to the interpreter.

**Deopt reasons:**
- `TypeMismatch` — type guard failed (expected int, got string)
- `Overflow` — integer overflow on specialized arithmetic
- `DivisionByZero` — zero divisor on specialized path
- `NullReference` — null/undefined access on assumed-non-null
- `DebuggerBreakpoint` — debugger hit in JIT code
- `ProfileRedirect` — profiling data invalidated, re-collect needed

**Process:**
1. JIT code hits guard failure
2. `Deoptimizer::deoptimize()` reconstructs interpreter stack frame
3. Execution resumes in interpreter at the corresponding bytecode offset
4. Deopt counter incremented; may trigger blacklisting

---

## 7. Inline Caches

**Location:** `jit/jit_inline_cache.cpp`, `jit/InlineCache.h`

Property access inline caches accelerate repeated access on same-shape objects:

- **Monomorphic** — single shape, direct slot offset
- **Polymorphic** — small number of shapes (up to 4), linear search
- **Megamorphic** — fallback to hash table lookup

The VM's `ICManager` (wired in Phase 52) maintains per-property-access cache entries.
Shape transitions on property add/delete invalidate affected caches.

---

## 8. Implementation Status

| Component | Status | Files |
|---|---|---|
| JIT Profiler | ✅ Implemented | `jit_profiler.cpp/hpp` |
| Type Profiler | ✅ Implemented | `type_profiler.cpp/hpp` |
| Tier Policy | ✅ Implemented | `jit_tier_policy.cpp` |
| Baseline JIT | ✅ Implemented | `baseline_jit.cpp/hpp` |
| Macro Assembler (x86-64) | ✅ Implemented | `macro_assembler.cpp`, `MacroAssembler.h` |
| OSR Manager | ✅ Implemented | `osr.cpp/hpp` |
| Deoptimizer | ✅ Implemented | `deoptimizer.cpp` |
| Inline Cache | ✅ Implemented | `jit_inline_cache.cpp`, `InlineCache.h` |
| Code Cache | ✅ Implemented | `jit_code_cache.cpp` |
| IR Generation | ✅ Implemented | `jit_ir.cpp` |
| IR Lowering | ✅ Implemented | `jit_lowering.cpp` |
| Code Generation | ✅ Implemented | `jit_code_gen.cpp` |
| ZOpt Graph | ✅ Implemented | `ZOptGraph.cpp`, `ZOptBuilder.cpp` |
| ZOpt Lowering | ✅ Implemented | `ZOptLowering.cpp` |
| ZOpt Pipeline | ✅ Implemented | `ZOptPipeline.cpp` |
| ZOpt Passes | 🔴 Header-only | 8 pass headers, 0 `.cpp` |
| ZIR Backend | 🔴 Header-only | 11 headers, 0 `.cpp` |
| Tier-up trigger | 🔴 Not wired | Profiler collects, but doesn't trigger compile |
| Graph Coloring RegAlloc | 🔴 Header-only | `GraphColoringRegAlloc.h` |

---
