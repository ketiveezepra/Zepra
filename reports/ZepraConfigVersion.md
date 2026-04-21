# ZepraScript Engine — Comprehensive Audit Report

> **Engine Version:** 1.0.2 (TODO.md) / 1.2.0 (architecture.md)
> **Audit Date:** 2026-04-18
> **Scope:** All files under `source/zepraScript/`

---

## 1. Aggregate Stats

| Metric | Value |
|---|---|
| Total source files (`.cpp` + `.hpp` + `.h`) | **674** |
| Total lines of code | **~195,500** |
| Subdirectories | **34** |
| `.h` header files (total) | **261** |
| Header-only (no matching `.cpp`) | **~120** |
| Build targets in CMakeLists | 1 static lib (`zepra-core`) |
| Test pass rate (per TODO.md) | 470/470 (401 unit + 69 integration) |

---

## 2. Per-Subsystem Breakdown

| Subsystem | Files | Lines | Maturity | Notes |
|---|---|---|---|---|
| **heap** | 191 | 57,395 | ✅ Production | 80K+ GC target achieved |
| **runtime** | 164 | 36,754 | ✅ Production | VM, objects, async, execution, GC bridges |
| **wasm** | 50 | 27,721 | ⚠️ Framework | 37 header-only, baseline compiler has stubs |
| **browser** | 37 | 11,325 | ⚠️ Framework | DOM, fetch, storage, workers — mostly functional |
| **builtins** | 43 | 10,148 | ✅ Production | Full ES2024 builtin set |
| **jit** | 29 | 6,890 | ⚠️ Framework | 14 header-only; tier-up not wired |
| **debugger** | 18 | 5,822 | ✅ Stabilized | 5 header-only API declarations |
| **zopt** | 19 | 5,679 | ⚠️ Framework | 13 header-only; 4 `.cpp` files |
| **frontend** | 14 | 5,329 | ✅ Production | Lexer/parser/AST/syntax checker |
| **api** | 19 | 4,920 | ⚠️ Framework | 13 header-only API declarations |
| **bytecode** | 9 | 3,861 | ✅ Production | Generator, opcodes, peephole |
| **modules** | 8 | 2,244 | ✅ Stabilized | ES modules, dynamic import |
| **zir** | 11 | 2,034 | 🔴 Declarations | 11 header-only, 0 `.cpp` files |
| **perf** | 6 | 1,957 | ⚠️ Framework | 6 header-only |
| **interpreter** | 6 | 1,479 | ✅ Production | Dispatch, builtins, debug |
| **regex** | 5 | 1,403 | ✅ Production | Compiler + engine |
| **optimization** | 4 | 1,255 | 🔴 Not compiled | CMakeLists comment: "stubs removed" |
| **core** | 3 | 1,024 | ⚠️ Framework | 3 header-only |
| **safety** | 2 | 1,016 | ⚠️ Framework | CrashBoundary + SecurityHardening |
| **profiler** | 4 | 994 | ✅ Stabilized | CPU, heap, sampling, timeline |
| **memory** | 6 | 854 | ✅ Production | Page, arena, slab, object pool |
| **security** | 1 | 450 | ⚠️ Framework | SecurityAudit.h only |
| **utils** | 3 | 437 | ✅ Production | Unicode, string builder, platform |
| **integration** | 2 | 407 | ⚠️ Stabilized | VMModuleIntegration |
| **bridge** | 1 | 382 | 🔴 Declarations | WasmJSBridge.h only |
| **workers** | 2 | 378 | ⚠️ Stabilized | WorkerModuleLoader |
| **codegen** | 1 | 371 | 🔴 Declarations | DeterministicCodegen.h only |
| **exception** | 1 | 346 | ⚠️ Minimal | stack_trace.cpp only |
| **threading** | 3 | 317 | ⚠️ Minimal | Thread pool + concurrent queue |
| **host** | 2 | 131 | ✅ Production | Native function + script engine |

---

## 3. What's Missing — Critical Gaps

### 3.1 Missing Directories (Referenced in architecture.md)

| Directory | Status | Impact |
|---|---|---|
| `docs/` | **MISSING** | Architecture doc references `docs/bytecode-spec.md`, `docs/jit-tiers.md` — none exist |
| `cmake/` | **MISSING** | Referenced as "Build configuration modules" |
| `cdp-extension/` | **MISSING** | Chrome DevTools Protocol adapter — referenced as optional |
| `zepra-devtools/` | **MISSING** | Native DevTools UI — referenced as optional |
| `tests/` (in-tree) | **MISSING** | Tests live in `/test/zeprascript/` (outside engine dir) — architecture.md shows `tests/` in-tree |

### 3.2 Subsystems with Zero Compiled Sources

These subsystems exist as header declarations only — no `.cpp` implementations, no CMakeLists entries:

| Subsystem | Headers | Description |
|---|---|---|
| **zir/** (Zepra IR) | 11 files | FTL/B3 backend IR — ZIRGenerate, ZIRLowering, ZIROpcode, ZIRValue, etc. |
| **optimization/** | 4 files | `hidden_class`, `inline_cache_opt`, `property_table`, `speculation` — **explicitly removed from build** |
| **codegen/** | 1 file | DeterministicCodegen.h |
| **bridge/** | 1 file | WasmJSBridge.h |
| **core/** | 3 files | EmbedderAPI, TaskRunner, APIVersion |

### 3.3 Header-Only API Declarations (~120 files)

These headers define interfaces with no `.cpp` backing:

**Runtime Builtins API (26 headers):**
ArrayAPI, BigIntAPI, RegExpAPI, StringAPI, JSONAPI, MathAPI, NumberAPI, DateAPI, ErrorAPI, ConsoleAPI, TypedArrayAPI, AtomicsAPI, BufferAPI, URIAPI, Base64API, TextEncodingAPI, SharedArrayBufferAPI, DataViewExtendedAPI, SuppressedErrorAPI, and enhancement APIs for Array, String, RegExp, Error, Atomics, PromiseEnhancements, SymbolEnhancements

**Runtime Intl (14 headers):**
TemporalAPI, LocaleAPI, TimezoneAPI, ZonedDateTimeAPI, InstantAPI, DurationFormatAPI, FormattingAPI, IntlCollator, IntlListFormat, IntlNumberFormat, IntlPluralRules, IntlRelativeTimeFormat, IntlSegmenter, DisplayNames

**Runtime Proposals (9 headers):**
ECMAScriptConformance, DecoratorsAPI, DisposableAPI, GroupingAPI, PatternMatchAPI, PipelineAPI, RecordTupleAPI, ResourceManagementAPI, ShadowRealmAPI

**Runtime Async (10 headers):**
PromiseAPI, AsyncAPI, GeneratorAPI, IteratorAPI, AsyncContextAPI, AsyncIteratorHelpersAPI, IteratorHelpersAPI, IteratorFromAPI, ObservableAPI, PromiseEnhancementsAPI

**Runtime Handles (5 headers):**
ModuleAPI, WeakRefAPI, ImportAssertionsAPI, MessageChannelAPI, TransferableAPI

**Runtime Objects (13 headers):**
ObjectAPI, ProxyAPI, ReflectAPI, FunctionAPI, SymbolAPI, NativeBinding, PropertyStorage, ObjectLayout, Shape, CoercionAPI, MapSetAPI, SetMethodsAPI, SymbolEnhancementsAPI

**Runtime Execution (11 headers):**
VMInternals, Sandbox, StackFrame, ExecutionContext, CrashHandler, Interpreter, IsolatedGlobal, SchedulerAPI, SecurityAPI, EvalAPI, GlobalEnhancementsAPI

**Runtime WASM API (5 headers):**
WasmComponentAPI, WasmExceptionAPI, WasmGCAPI, WasmInteropAPI, WasmRelaxedSimdAPI

**Browser (11 headers):**
DOMBindings, IndexedDBAPI, WebRTCAPI, WebSocketAPI, PerformanceAPI, DevToolsAPI, EventLoopValidator, ServiceWorker, ConsoleAPI, StructuredClone, ResizeObserverAPI

**API embedder (13 headers):**
FetchAPI, StreamsAPI, StructuredCloneAPI, URLAPI, IntlAPI, PerformanceAPI, StorageAPI, WorkerAPI, BlobAPI, CryptoAPI, EncodingAPI, FormDataAPI, TimerAPI

**JIT (14 headers):**
MacroAssembler, InlineCache, Deoptimization, ZepraTierBuilder, CodePatching, Fuses, GraphColoringRegAlloc, LinearScan, LivenessAnalysis, + jit/zopt/ (4 files)

**Other:**
Debugger (5), Perf (6), WASM (37), ZOpt passes (8), Safety (2), Security (1), CI (1), Release (1), Benchmarks (1)

### 3.4 Known Stubs in Compiled Code

| File | Issue |
|---|---|
| `wasm/WasmBaselineCompile.cpp` | I64 operations, call operations, AArch64 SIMD popcount marked as stubs |
| `wasm/WasmSignalHandlers.cpp` | `TODO: Redirect to thunk` |
| `wasm/ZWasmTierController.h` | `TODO: rename to ZWasmConstants.h` |
| `browser/console_api.cpp` | Three inline stubs in ConsoleAPI.h need real logic |

### 3.5 Architecture Doc Staleness

| Issue | Detail |
|---|---|
| Version mismatch | `architecture.md` says 1.2.0, `TODO.md` says 1.0.2, `config.hpp` says 1.0.0 |
| Directory names | Still references `dfg/` and `b3/` — renamed to `zopt/` and `zir/` in Phase 63 |
| File counts | Repository layout section has outdated counts (e.g., interpreter listed as 2 files, actually 6) |
| TODO items | `architecture.md` Phase 1-6 TODOs list items already completed per `TODO.md` (for-in, class, Proxy, generators, etc.) |
| Test count | architecture.md says 266/267, TODO.md says 470/470 |

---

## 4. Gap Closure Roadmap

### Tier 0 — Immediate (Hygiene)

- [ ] Sync version strings across `config.hpp`, `TODO.md`, `architecture.md` to single source of truth
- [ ] Update `architecture.md` directory listing: `dfg/`→`zopt/`, `b3/`→`zir/`, correct file counts
- [ ] Update `architecture.md` TODO section — mark completed items, remove duplicates
- [ ] Create `docs/` directory with `bytecode-spec.md` and `jit-tiers.md` (referenced but missing)

---

### Tier 1 — Structural (Medium Effort)

- [ ] **optimization/** — Re-enable and implement `hidden_class.cpp`, `inline_cache_opt.cpp`, `property_table.cpp`, `speculation.cpp` (4 files, currently excluded from build)
- [ ] **zir/** — Create `.cpp` implementations for at least `ZIRGenerate.cpp`, `ZIRLowering.cpp`, `ZIROpcode.cpp` (FTL backend foundations)
- [ ] **bridge/** — Implement `WasmJSBridge.cpp` (JS ↔ WASM interop runtime)
- [ ] **codegen/** — Implement `DeterministicCodegen.cpp` (reproducible code generation)
- [ ] **core/** — Implement `EmbedderAPI.cpp`, `TaskRunner.cpp` (embedder runtime)
- [ ] Create `cmake/` with modular build configs (feature toggles, platform detection modules)

---

### Tier 2 — API Surface (High Effort)

Priority order based on web compatibility impact:

1. **Intl implementations** — 14 headers, zero `.cpp` files. Critical for i18n. Start with `IntlNumberFormat`, `IntlDateTimeFormat`, `IntlCollator`.
2. **TC39 Proposals** — `DecoratorsAPI`, `DisposableAPI` (using/await using), `ShadowRealmAPI`. These are stage 3+.
3. **Browser WebRTC** — `WebRTCAPI.h` declared, no implementation. Needed for video conferencing sites.
4. **Streams API** — `StreamsAPI.h` in api/. Critical for modern fetch body handling.
5. **Crypto API** — `CryptoAPI.h`. Required for any HTTPS-dependent JS.
6. **Blob/FormData** — `BlobAPI.h`, `FormDataAPI.h`. Required for file uploads.

---

### Tier 3 — JIT Maturation (Highest Effort)

The JIT pipeline is the largest gap between "framework" and "production":

| Component | Current State | Required |
|---|---|---|
| Baseline tier-up | Profiler wired, no actual codegen trigger | Wire `JITProfiler::shouldCompile()` → `BaselineJIT::compile()` |
| ZOpt optimizer | 4 `.cpp` files, 8 header-only passes | Implement at least ConstFold, DeadCodeElim, CopyProp |
| ZIR backend | 11 headers, 0 cpp | Full IR lowering pipeline needed |
| Register allocator | Graph coloring header-only | Implement `GraphColoringRegAlloc.cpp` or at minimum `LinearScan.cpp` |
| Deoptimization | Header-only | Critical for speculation bailout to interpreter |
| OSR | `osr.cpp` exists | Verify on-stack replacement actually functions end-to-end |

---

### Tier 4 — WASM Completion

The WASM subsystem has 50 files / 27K lines but ~37 are header-only:

- I64 operations in baseline compiler (marked as stubs)
- Call operations in baseline compiler (marked as stubs)
- No AOT compilation (`WasmAOT.h` is declaration-only)
- Component model (`WasmComponent.h`) unimplemented
- Multi-memory (`WasmMultiMemory.h`) unimplemented

---

## 5. Files Not in CMakeLists (Exist on Disk But Not Compiled)

| File | Status |
|---|---|
| `browser/video_bindings.cpp` | On disk (16K), **not in CMakeLists** |
| `optimization/hidden_class.cpp` | On disk, explicitly excluded |
| `optimization/inline_cache_opt.cpp` | On disk, explicitly excluded |
| `optimization/property_table.cpp` | On disk, explicitly excluded |
| `optimization/speculation.cpp` | On disk, explicitly excluded |
| `builtins/weakref.cpp` | In CMakeLists but no `.hpp` pair visible |

---

## 6. Test Infrastructure

Tests live at `/test/zeprascript/` with subdivisions:

```
test/zeprascript/
├── benchmarks/
├── browser_apis/
├── gc/
├── integration/
├── interpreter/
├── jit/
├── js/
├── runtime/
├── scripts/
├── spec/
├── test262/
├── unit/
└── wasm/
```

**Test262 directory exists** but conformance pass rate is not tracked in TODO.md. The Phase 57 item `Test262 compliance pass` remains unchecked.

---

## 7. Summary Verdict

| Area | Rating | Detail |
|---|---|---|
| **VM + Runtime + GC** | 🟢 Strong | 80K+ GC, 96 opcodes, NaN-boxing, full dispatch |
| **Frontend** | 🟢 Strong | Lexer/parser/AST robust, ES2025 syntax |
| **Builtins** | 🟢 Strong | Full ES2024 set compiled |
| **JIT** | 🟡 Incomplete | Framework exists, tier-up not wired, ZIR/ZOpt mostly declarations |
| **WASM** | 🟡 Incomplete | 310K baseline compiler exists but has major stubs |
| **Browser APIs** | 🟡 Partial | Core APIs (fetch, DOM, storage) work; WebRTC, Streams, Crypto missing |
| **Intl** | 🔴 Missing | 14 headers, zero implementations |
| **TC39 Proposals** | 🔴 Missing | 9 headers, zero implementations |
| **Docs** | 🔴 Missing | Referenced docs directory does not exist |
| **Optimization** | 🔴 Broken | 4 files on disk, explicitly excluded from build |
