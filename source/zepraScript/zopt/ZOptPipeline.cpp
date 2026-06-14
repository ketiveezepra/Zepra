// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file ZOptPipeline.cpp
 * @brief ZOpt JIT Compilation Pipeline
 *
 * Orchestrates the full ZOpt optimization pipeline:
 *   1. Build ZOpt graph from bytecode (ZOptBuilder)
 *   2. Copy propagation (canonicalize values)
 *   3. Constant folding
 *   4. Common subexpression elimination
 *   5. Strength reduction
 *   6. Loop-invariant code motion
 *   7. Dead code elimination
 *   8. Register allocation (linear scan)
 *   9. Lowering to machine code
 *
 */

#include "zopt/ZOptGraph.h"
#include <algorithm>
#include "zopt/ZOptBuilder.h"
#include "zopt/ZOptRegAlloc.h"
#include "zopt/ZOptCodeGen.h"
#include "zopt/passes/ZOptConstFold.h"
#include "zopt/passes/ZOptDeadCodeElim.h"
#include "zopt/passes/ZOptStrengthReduce.h"
#include "zopt/passes/ZOptCopyProp.h"
#include "zopt/passes/ZOptCommonSubexpr.h"
#include "zopt/passes/ZOptLoopInvariant.h"
#include "jit/MacroAssembler.h"
#include <memory>
#include <cstdio>
#include <chrono>

namespace Zepra::ZOpt {

// Forward declaration from DFGLowering.cpp
void lowerGraph(Graph* graph, JIT::MacroAssembler& masm);

// =============================================================================
// Compilation Result
// =============================================================================

struct CompilationResult {
    bool success = false;
    std::unique_ptr<JIT::ExecutableBuffer> code;
    size_t codeSize = 0;
    double compileTimeMs = 0;
    
    // Statistics
    uint32_t numBlocksBefore = 0;
    uint32_t numValuesBefore = 0;
    uint32_t numBlocksAfter = 0;
    uint32_t numValuesAfter = 0;
    uint32_t constFoldReductions = 0;
    uint32_t strengthReductions = 0;
    uint32_t deadCodeEliminated = 0;
};

// =============================================================================
// Pipeline Phases
// =============================================================================

enum class Phase : uint8_t {
    Build,
    CopyPropagation,
    ConstantFolding,
    CommonSubexprElim,
    StrengthReduction,
    LoopInvariantCodeMotion,
    DeadCodeElimination,
    RegisterAllocation,
    Lowering,
    NumPhases
};

static const char* phaseName(Phase p) {
    switch (p) {
        case Phase::Build: return "Build";
        case Phase::CopyPropagation: return "CopyProp";
        case Phase::ConstantFolding: return "ConstFold";
        case Phase::CommonSubexprElim: return "CSE";
        case Phase::StrengthReduction: return "StrengthReduce";
        case Phase::LoopInvariantCodeMotion: return "LICM";
        case Phase::DeadCodeElimination: return "DCE";
        case Phase::RegisterAllocation: return "RegAlloc";
        case Phase::Lowering: return "Lowering";
        default: return "Unknown";
    }
}

// =============================================================================
// ZOpt Pipeline
// =============================================================================

class Pipeline {
public:
    explicit Pipeline(uint32_t funcIndex,
                      bool verbose = false)
        : funcIndex_(funcIndex)
        , verbose_(verbose)
        , module_(nullptr) {}

    void setModule(const Wasm::WasmModule* mod) { module_ = mod; }

    CompilationResult compile() {
        CompilationResult result;
        auto start = std::chrono::steady_clock::now();

        // Phase 1: Build ZOpt graph from bytecode
        if (!runPhase(Phase::Build, result)) return result;

        // Record pre-optimization stats
        result.numBlocksBefore = graph_->numBlocks();
        result.numValuesBefore = graph_->numValues();

        // Phase 2: Copy propagation (canonicalizes values, enables CSE)
        runPhase(Phase::CopyPropagation, result);

        // Phase 3: Constant folding (iterate until fixpoint)
        {
            uint32_t preValues = graph_->numValues();
            runPhase(Phase::ConstantFolding, result);
            result.constFoldReductions = preValues - graph_->numValues();
        }

        // Phase 4: Common subexpression elimination
        runPhase(Phase::CommonSubexprElim, result);

        // Phase 5: Strength reduction
        {
            uint32_t preValues = graph_->numValues();
            runPhase(Phase::StrengthReduction, result);
            result.strengthReductions = preValues - graph_->numValues();
        }

        // Phase 6: Loop-invariant code motion
        runPhase(Phase::LoopInvariantCodeMotion, result);

        // Phase 7: Dead code elimination (cleanup from all prior passes)
        {
            uint32_t preValues = graph_->numValues();
            runPhase(Phase::DeadCodeElimination, result);
            result.deadCodeEliminated = preValues - graph_->numValues();
        }

        // Record post-optimization stats
        result.numBlocksAfter = graph_->numBlocks();
        result.numValuesAfter = graph_->numValues();

        if (verbose_) {
            printf("ZOpt Pipeline: %u blocks, %u→%u values "
                   "(fold=%u, sr=%u, dce=%u)\n",
                   result.numBlocksAfter,
                   result.numValuesBefore, result.numValuesAfter,
                   result.constFoldReductions,
                   result.strengthReductions,
                   result.deadCodeEliminated);
        }

        // Phase 5: Register allocation (linear scan)
        if (!runPhase(Phase::RegisterAllocation, result)) return result;

        // Phase 6: Lower to machine code (using register allocation)
        if (!runPhase(Phase::Lowering, result)) return result;

        // Finalize: Copy to executable buffer
        auto execBuf = std::make_unique<JIT::ExecutableBuffer>();
        if (!execBuf->allocate(masm_.size())) {
            fprintf(stderr, "ZOpt: Failed to allocate executable buffer (%zu bytes)\n",
                    masm_.size());
            return result;
        }

        if (!execBuf->copyFrom(masm_)) {
            fprintf(stderr, "ZOpt: Failed to copy code to executable buffer\n");
            return result;
        }

        execBuf->makeExecutable();

        auto end = std::chrono::steady_clock::now();
        result.compileTimeMs = std::chrono::duration<double, std::milli>(end - start).count();
        result.codeSize = masm_.size();
        result.code = std::move(execBuf);
        result.success = true;

        if (verbose_) {
            printf("ZOpt Pipeline: compiled func %u → %zu bytes in %.2fms\n",
                   funcIndex_, result.codeSize, result.compileTimeMs);
        }

        return result;
    }

private:
    bool runPhase(Phase phase, CompilationResult& result) {
        auto phaseStart = std::chrono::steady_clock::now();

        switch (phase) {
            case Phase::Build: {
                graph_ = std::make_unique<Graph>();
                graph_->setFunctionIndex(funcIndex_);

                if (!module_) {
                    fprintf(stderr, "ZOpt Build: no module set for func %u\n", funcIndex_);
                    return false;
                }

                Builder builder(module_);
                if (!builder.build(funcIndex_, graph_.get())) {
                    fprintf(stderr, "ZOpt Build: failed for func %u: %s\n",
                            funcIndex_, builder.error().c_str());
                    return false;
                }

                if (graph_->numBlocks() == 0) {
                    fprintf(stderr, "ZOpt Build: empty graph for func %u\n", funcIndex_);
                    return false;
                }

                if (verbose_) {
                    printf("  [%s] %u blocks, %u values\n",
                           phaseName(phase),
                           graph_->numBlocks(), graph_->numValues());
                }
                break;
            }

            case Phase::CopyPropagation: {
                CopyPropagation cp(graph_.get());
                cp.run();
                break;
            }

            case Phase::ConstantFolding: {
                ConstantFolding cf(graph_.get());
                bool changed = true;
                int iters = 0;
                while (changed && iters < 10) {
                    changed = cf.run();
                    iters++;
                }
                if (verbose_) {
                    printf("  [%s] %d iterations\n", phaseName(phase), iters);
                }
                break;
            }

            case Phase::CommonSubexprElim: {
                CommonSubexpressionElimination cse(graph_.get());
                cse.run();
                break;
            }

            case Phase::StrengthReduction: {
                StrengthReduction sr(graph_.get());
                sr.run();
                break;
            }

            case Phase::LoopInvariantCodeMotion: {
                LoopInvariantCodeMotion licm(graph_.get());
                licm.run();
                break;
            }

            case Phase::DeadCodeElimination: {
                DeadCodeElimination dce(graph_.get());
                dce.run();
                break;
            }

            case Phase::RegisterAllocation: {
                LinearScanAllocator allocator(graph_.get());
                regAlloc_ = allocator.allocate();
                if (verbose_) {
                    uint32_t spills = regAlloc_.numSpillSlots;
                    uint32_t intervals = static_cast<uint32_t>(regAlloc_.intervals.size());
                    printf("  [%s] %u intervals, %u spill slots\n",
                           phaseName(phase), intervals, spills);
                }
                break;
            }

            case Phase::Lowering: {
                // Use register-allocated code generator
                CodeGenerator codegen(graph_.get(), regAlloc_);
                auto generated = codegen.generate();
                if (!generated.success) {
                    fprintf(stderr, "ZOpt Lowering: code generation failed\n");
                    return false;
                }
                // Copy generated code into MacroAssembler buffer for the existing
                // executable buffer path.
                for (uint8_t byte : generated.code) {
                    masm_.emit8(byte);
                }
                if (verbose_) {
                    printf("  [%s] emitted %zu bytes (frame %u)\n",
                           phaseName(phase), generated.code.size(), generated.frameSize);
                }
                break;
            }

            default:
                break;
        }

        auto phaseEnd = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(phaseEnd - phaseStart).count();

        if (verbose_) {
            printf("  [%s] %.3fms\n", phaseName(phase), ms);
        }

        return true;
    }

    uint32_t funcIndex_;
    bool verbose_;
    const Wasm::WasmModule* module_;

    std::unique_ptr<Graph> graph_;
    RegisterAllocation regAlloc_;
    JIT::MacroAssembler masm_;
};

// =============================================================================
// Public API
// =============================================================================

CompilationResult compileDFG(uint32_t funcIndex,
                              const Wasm::WasmModule* module,
                              bool verbose) {
    Pipeline pipeline(funcIndex, verbose);
    pipeline.setModule(module);
    return pipeline.compile();
}

} // namespace Zepra::ZOpt
