# ZepraScript Engine — Architecture

> A high-performance JavaScript engine for the Zepra browser.
> Target: 2M+ lines of C++. Multi-tier JIT. Custom GC. Full ES2025 compliance.

**Version:** 1.2.0
**Last Updated:** 2026-04-18

---

## Table of Contents

1. [High-Level Overview](#1-high-level-overview)
2. [Repository Layout](#2-repository-layout)
3. [Subsystem Reference](#3-subsystem-reference)
4. [Execution Pipeline](#4-execution-pipeline)
5. [JIT Tier Escalation](#5-jit-tier-escalation)
6. [GC Strategy](#6-gc-strategy)
7. [Public API Surface](#7-public-api-surface)
8. [Debug Architecture](#8-debug-architecture)
9. [Dependency Map](#9-dependency-map)
10. [Build System](#10-build-system)
11. [Testing Strategy](#11-testing-strategy)
12. [Design Principles](#12-design-principles)
13. [Development TODO](#13-development-todo)

---

## 1. High-Level Overview

ZepraScript is a multi-tier JavaScript engine with four execution tiers:

```
Source Code
    |
    v
[ Frontend ]     Lexer -> Parser -> AST -> Syntax Checker
    |
    v
[ Compiler ]     Scope analysis -> Bytecode generation -> Peephole optimizer
    |
    v
[ Interpreter ]  Bytecode dispatch, baseline profiling
    |
    v  (hot path)
[ Baseline JIT ] Fast native code + inline cache stubs
    |
    v  (type feedback)
[ ZOpt JIT ]     Speculative optimization, type-specialized IR
    |
    v  (critical hot loops)
[ ZIR / FTL ]    Low-level IR backend, aggressive inlining
    |
    v  (speculation failure)
[ Interpreter ]  Bail-out, reset profiling
```

The engine is both embedded inside the Zepra browser and exposed as a standalone library via a V8-compatible handle/isolate API.

---

## 2. Repository Layout

Each subsystem directory contains **both** `.cpp` and `.h/.hpp` files together. No `src/` + `include/` split.

```
zeprascript/
|-- frontend/          # Lexer, Parser, AST                          (14 files)
|-- bytecode/          # Opcode definitions, bytecode generator       (9 files)
|-- interpreter/       # Bytecode dispatch loop                       (6 files)
|-- jit/               # Baseline JIT, assemblers, IC, OSR           (29 files)
|-- zopt/              # Zepra Optimizer (sea-of-nodes IR)           (19 files)
|-- zir/               # Zepra IR low-level backend                  (11 files)
|
|-- runtime/
|   |-- objects/       # Value, Object, Function, Symbol, Proxy, Map (36 files)
|   |-- execution/     # VM, Environment, GlobalObject               (27 files)
|   |-- async/         # Promise, AsyncFunction, Iterator            (18 files)
|   |-- handles/       # Module, WeakMap, InlineCache                (11 files)
|   |-- builtins_api/  # ES builtin type API stubs                   (31 files)
|   |-- intl/          # Intl/locale API stubs                       (14 files)
|   |-- wasm_api/      # WASM runtime API stubs                       (5 files)
|   +-- proposals/     # TC39 proposal stubs                          (9 files)
|
|-- builtins/          # Native builtin implementations              (43 files)
|-- heap/              # Generational, incremental, concurrent GC   (191 files)
|-- memory/            # Page, arena, slab allocators                 (6 files)
|-- optimization/      # Hidden classes, property tables, speculation (4 files)
|
|-- wasm/              # WebAssembly engine                          (50 files)
|-- browser/           # DOM, Fetch, Events, Workers, Storage        (37 files)
|-- modules/           # ES Module loader, resolver                   (8 files)
|-- regex/             # Regex compiler + JIT                         (5 files)
|-- threading/         # Thread pool, concurrent queue                (3 files)
|
|-- api/               # Embedder API (Isolate, Context, Handles)    (19 files)
|-- host/              # C++ <-> JS bridge, native function binding   (2 files)
|-- debugger/          # Native debug protocol                       (18 files)
|-- profiler/          # CPU, heap, sampling profilers                (4 files)
|-- exception/         # Error objects, stack traces, try/catch       (1 files)
|
|-- core/              # Core type definitions                        (3 files)
|-- codegen/           # Code generation utilities                    (1 files)
|-- perf/              # Performance measurement utilities            (6 files)
|-- safety/            # Crash boundaries, security hardening         (2 files)
|-- security/          # Security audit                               (1 files)
|-- bridge/            # Host bridge abstractions                     (1 files)
|-- integration/       # VM-module integration layer                  (2 files)
|-- workers/           # Worker module loader                         (2 files)
|-- utils/             # String builder, platform, unicode            (3 files)
|
|-- benchmarks/        # Performance benchmarks                       (3 files)
|-- tools/             # REPL, bytecode dumper, engine health         (5 files)
|-- ci/                # Conformance gate                             (1 file)
|-- release/           # Release manifest                             (1 file)
+-- CMakeLists.txt     # Engine build definition
```

---

## 3. Subsystem Reference

### 3.1 Frontend

**Location:** `frontend/`

| File             | Role                                                                           |
| ---------------- | ------------------------------------------------------------------------------ |
| `lexer`          | Tokenizes UTF-8 source, Unicode identifiers, template literals, regex literals |
| `token`          | Token type definitions and metadata                                            |
| `parser`         | Recursive descent parser, full ES2025 AST                                      |
| `ast`            | AST node types, visitor interface                                              |
| `source_code`    | Source buffer management, line/col mapping                                     |
| `syntax_checker` | Early error detection (strict mode, duplicate params, TDZ)                     |

The parser does no semantic analysis. Scope resolution is a separate pass. This keeps the grammar parse fast and stateless.

### 3.2 Bytecode

**Location:** `bytecode/`

| File                 | Role                                             |
| -------------------- | ------------------------------------------------ |
| `opcode`             | Opcode enum, instruction widths, operand formats |
| `bytecode_generator` | Low-level instruction emitter                    |
| `peephole_optimizer` | Post-emit peephole optimization pass             |

### 3.3 Interpreter

**Location:** `interpreter/`

The baseline execution tier. Threaded dispatch with computed gotos. Maintains type feedback metadata for JIT profiling. All code starts here.

### 3.4 JIT Tiers

**Location:** `jit/`, `zopt/`, `zir/`

**Baseline JIT** (`jit/baseline_jit`) — Triggered after ~100 invocations. Emits unoptimized native code with inline cache stubs. Fast compile latency.

**ZOpt JIT** (`zopt/`) — Triggered by type feedback after ~1000 calls. Builds a sea-of-nodes IR. Type specialization, inlining, escape analysis. Includes OSR for loop-heavy code.

**ZIR / FTL** (`zir/`) — For the hottest functions under sustained load. Low-level IR, full register allocation, instruction scheduling, SIMD. Deoptimization bails back to interpreter.

**Platform assemblers:** `macro_assembler`, plus platform-specific backends in `jit/`.

### 3.5 Runtime

**Location:** `runtime/objects/`, `runtime/execution/`, `runtime/async/`, `runtime/handles/`

| Sub-group    | Contents                                                                                                                       |
| ------------ | ------------------------------------------------------------------------------------------------------------------------------ |
| `objects/`   | Tagged value (NaN-boxing), base Object, Function closures, Symbol registry, Proxy/Reflect trap dispatch, Map/Set, class fields |
| `execution/` | VM state, call dispatch, lexical scope records, global object initialization, execution context, sandbox                       |
| `async/`     | Promise state machine, async function desugaring, iterator protocol, generator support                                         |
| `handles/`   | Module records, module loader, inline cache runtime, weak collections                                                          |

**API stubs** in `builtins_api/`, `intl/`, `wasm_api/`, `proposals/` define interfaces for future builtin features.

### 3.6 Builtins

**Location:** `builtins/`

Native C++ implementations of all ECMAScript built-in objects: `Array`, `String`, `Number`, `Boolean`, `Object`, `Function`, `Math`, `Date`, `RegExp`, `JSON`, `Map`, `Set`, `TypedArray`, `DataView`, `Console`, `Symbol`, `WeakMap`, `Reflect`, `Generator`, `Intl`, `Temporal`, `Atomics`.

Registered into the global object at engine startup. Performance-critical paths have JIT-inlined intrinsics.

### 3.7 Heap (GC)

**Location:** `heap/`

Generational, incremental, concurrent garbage collector.

| Component      | Description                                                  |
| -------------- | ------------------------------------------------------------ |
| Nursery        | Young generation, bump-pointer allocation, frequent minor GC |
| Old Generation | Mark-sweep-compact with incremental slicing                  |
| Concurrent GC  | Background marking thread concurrent with JS execution       |
| Write Barrier  | Dijkstra-style barrier for cross-generation references       |
| Compaction     | Optional compacting phase to reduce fragmentation            |
| Handles        | Rooted handle system for GC-safe C++ references              |
| Finalizers     | Post-collection callbacks for native resources               |
| Weak Refs      | GC-aware per WeakRef/FinalizationRegistry spec               |

### 3.8 Memory

**Location:** `memory/`

| Allocator         | Use Case                                            |
| ----------------- | --------------------------------------------------- |
| `page_allocator`  | OS-level memory mapping (mmap/VirtualAlloc)         |
| `arena_allocator` | Region-based for AST nodes and compiler temporaries |
| `memory_pool`     | General-purpose pool with size classes              |

### 3.9 WebAssembly

**Location:** `wasm/`

Full WASM engine: binary validation, baseline compiler, interpreter, stack management, signal handlers, fault handling. 50 files covering the complete WASM spec including threads, SIMD, GC, component model, and tail calls.

### 3.10 Browser Bindings

**Location:** `browser/`

Web platform API implementations:

- **DOM:** window, document bindings
- **Networking:** fetch, WebSocket
- **Events:** event system
- **Storage:** storage API, secure storage, password vault, IndexedDB, structured clone
- **Workers:** web worker, service worker
- **Other:** URL, console API, performance, devtools bridge

### 3.11 Modules

**Location:** `modules/`

Full ES module system: `module_loader` (resolve specifiers, caching), `module_record` (parse/eval state), `import_resolver` (browser URL vs Node path), `dynamic_import`, `module_namespace`.

### 3.12 Debugger

**Location:** `debugger/`

ZepraScript's native debug protocol — independent of CDP.

| File                 | Role                                                   |
| -------------------- | ------------------------------------------------------ |
| `debug_api`          | Primary debug interface: attach, detach, set hooks     |
| `breakpoint_manager` | Source-level breakpoint registration and hit detection |
| `call_stack_info`    | Stack frame inspection during pause                    |
| `execution_control`  | Step-in, step-over, step-out, continue, pause          |
| `inspector`          | Variable and scope inspection                          |
| `profiler`           | Debug-attached profiling                               |

CDP compatibility is an optional extension (`cdp-extension/`), not part of core.

### 3.13 Optimization

**Location:** `optimization/`

| File               | Role                                                    |
| ------------------ | ------------------------------------------------------- |
| `hidden_class`     | Shape/hidden class transitions for fast property access |
| `property_table`   | Hash table for object property storage                  |
| `inline_cache_opt` | Monomorphic/polymorphic cache optimization pass         |
| `speculation`      | Type speculation guards and deoptimization triggers     |

### 3.14 API (Embedder Surface)

**Location:** `api/`

V8-compatible handle/isolate API: `Isolate`, `Context`, `LocalHandle<T>`, `PersistentHandle<T>`, `FunctionTemplate`, `ObjectTemplate`, `Script`. Embedders include `zepra_api.hpp` and nothing else.

---

## 4. Execution Pipeline

```
                     +---------------+
   Source text --->  |   Frontend    |  Lex -> Parse -> AST -> Syntax check
                     +------+--------+
                            | AST
                     +------v--------+
                     |   Compiler    |  Scope resolution -> Bytecode emit
                     +------+--------+
                            | Bytecode
                     +------v--------+
                     |  Interpreter  |  Execute + collect type feedback
                     +------+--------+
                   hot?     |
              +-------------v--------------+
              |       Baseline JIT         |  Unoptimized native + IC stubs
              +-------------+--------------+
                   hot?     | type feedback
              +-------------v--------------+
              |          ZOpt JIT          |  Speculative + type-specialized
              +-------------+--------------+
                   hot?     |
              +-------------v--------------+
              |        ZIR / FTL           |  Maximum optimization
              +----------------------------+
                            |  deopt?
                            +------------>  Interpreter (bail-out)
```

---

## 5. JIT Tier Escalation

| Tier         | Trigger                       | Compile Time | Peak Throughput     |
| ------------ | ----------------------------- | ------------ | ------------------- |
| Interpreter  | Always                        | None         | Baseline            |
| Baseline JIT | ~100 calls or loop iterations | ~1ms         | 2-5x interpreter    |
| ZOpt JIT     | ~1000 calls + type stability  | ~10ms        | 10-30x interpreter  |
| ZIR / FTL    | ~10000 calls + ZOpt hotness   | ~50ms        | 50-100x interpreter |

Deoptimization occurs when a speculation assumption is invalidated. The engine bails to interpreter with reconstructed stack state and resets the profiling counter.

---

## 6. GC Strategy

```
Heap Layout:

  +------------------------------------------------------+
  |                     Nursery (Young Gen)               |
  |    bump-pointer allocation | semi-space copy GC       |
  +------------------------------------------------------+
  +------------------------------------------------------+
  |                  Old Generation                       |
  |    mark-sweep-compact | incremental | concurrent      |
  +------------------------------------------------------+
  +------------------------------------------------------+
  |               Large Object Space                      |
  |    directly mapped pages | no compaction              |
  +------------------------------------------------------+
```

- Minor GC: Nursery collection, ~every few MB, pauses < 1ms
- Major GC: Old gen, incremental slices keep pauses < 5ms
- Concurrent marking: Background thread marks while JS runs
- Write barrier: Every pointer store, tracks cross-generation refs
- Compaction: On demand, moves objects to contiguous ranges

---

## 7. Public API Surface

Top-level headers at engine root: `zepra_api.hpp`, `script_engine.hpp`, `config.hpp`.

Embedder-facing API in `api/`, modeled after V8:

- `Isolate` — isolated JS heap and engine instance
- `Context` — JS execution context within an isolate
- `LocalHandle<T>` — stack-scoped GC-safe reference
- `PersistentHandle<T>` — heap-allocated long-lived GC-safe reference
- `FunctionTemplate` / `ObjectTemplate` — blueprints for native-backed JS objects

---

## 8. Debug Architecture

```
+-------------------------------------+
|   Zepra DevTools (Native C++ UI)    |  Primary debugging tool
|   Direct API calls, zero overhead   |
+------------------+------------------+
                   |
+------------------v------------------+
|   Native Debug API (debugger/)      |  Always present
|   Breakpoints, call stack,          |
|   variable inspection               |
+------------------+------------------+
                   |
+------------------v------------------+
|   ZepraScript Core Engine           |  Never depends on debug tools
+-------------------------------------+

+-------------------------------------+
|   CDP Extension (Optional)          |  Can be deleted
|   Chrome DevTools compatibility     |
+-------------------------------------+
```

The native debug protocol is the primary protocol. CDP is a compatibility adapter only.

---

## 9. Dependency Map

```
utils         <-  no engine deps (foundational)
memory        <-  utils
heap          <-  memory, utils
runtime       <-  heap, memory, utils
builtins      <-  runtime
interpreter   <-  bytecode, runtime
jit           <-  interpreter, optimization, runtime, memory
zopt          <-  jit, runtime
zir           <-  zopt
optimization  <-  runtime, heap
async (src)   <-  runtime (promise), threading
modules       <-  runtime
regex         <-  utils, memory
exception     <-  runtime
browser       <-  runtime, builtins, modules              <- LEAF
host          <-  runtime, heap                            <- LEAF
debugger      <-  runtime, interpreter                     <- LEAF
profiler      <-  runtime, jit, heap                       <- LEAF
api           <-  runtime, heap                            <- LEAF (embedder)
cdp-extension <-  debugger                                 <- OPTIONAL LEAF
zepra-devtools<-  debugger, profiler                       <- OPTIONAL LEAF
```

Circular dependencies between core subsystems are forbidden.

---

## 10. Build System

CMake 3.16+ with Ninja recommended.

```bash
# Configure
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DZEPRA_CDP_EXTENSION=OFF \
  -DZEPRA_DEVTOOLS=ON

# Build
cmake --build build -j$(nproc)

# Test
ctest --test-dir build
```

The engine root (`source/zepraScript/`) is the include root. All `#include` paths are relative to it:

```cpp
#include "runtime/objects/value.hpp"
#include "heap/heap.hpp"
#include "frontend/lexer.hpp"
#include "config.hpp"
```

---

## 11. Testing Strategy

```
test/zeprascript/
|-- unit/           # Per-subsystem C++ unit tests (GoogleTest)
|-- integration/    # Cross-subsystem tests
|-- gc/             # GC-specific tests
|-- jit/            # JIT-specific tests
|-- wasm/           # WASM-specific tests
|-- browser_apis/   # Browser API tests
|-- js/             # JavaScript conformance scripts
|-- spec/           # Spec compliance tests
+-- test262/        # ECMAScript conformance suite
```

| Suite       | Coverage                                                  |
| ----------- | --------------------------------------------------------- |
| Unit tests  | Lexer, parser, VM, GC, JIT, builtins, debug API           |
| Integration | Browser bindings, module loading, promise chains, workers |
| Test262     | Full ES spec conformance — target 99%+ pass rate          |
| Benchmarks  | JetStream2, Speedometer, Octane                           |

Current: 470/470 tests passing (401 unit + 69 integration).

---

## 12. Design Principles

1. **Correctness first, speed second.** The interpreter is the source of truth. JIT tiers must produce identical observable behavior.

2. **The engine does not own the event loop.** ZepraScript exposes `pump()` and `drain_microtasks()` hooks. The browser drives execution.

3. **The native debug protocol is the primary protocol.** CDP is a compatibility adapter. Internal tooling uses native protocol directly.

4. **No subsystem depends on `browser/`.** Browser bindings are a leaf layer. Core engine has no knowledge of DOM or web APIs.

5. **Public API is stable; internals are not.** `api/` and `zepra_api.hpp` follow semver. Internal headers can change freely.

6. **All allocations go through the memory layer.** No raw `new`/`malloc` in engine code. Everything routes through arena, slab, pool, or GC heap.

7. **Each subsystem directory contains both headers and implementation.** No split between `src/` and `include/`. Co-located code is easier to navigate.

---

## 13. Development TODO

### Immediate (Build Fixes) — RESOLVED

- [x] Fix `runtime/objects/symbol.cpp` type mismatch (Phase 64)
- [x] Fix `heap/generational_gc.cpp` missing closing brace (Phase 74)
- [x] Fix `runtime/execution/Sandbox.h` missing `<stdexcept>` include (Phase 68)
- [x] Fix `runtime/handles/module_loader.cpp` `starts_with` (Phase 59)
- [x] Fix `heap/GCController.h` undeclared member variables (Phase 73)

### Phase 1: Core Correctness — MOSTLY COMPLETE

- [x] Complete ES2024 spec compliance in builtins (Array, String, Date, Map, Set, WeakMap — Phase 54-56)
- [x] Implement `for-in` / `for-of` iteration protocol end-to-end (Phase 60-62)
- [x] Implement `class` syntax with inheritance, private fields, static methods (Phase 60-61)
- [x] Implement `Proxy` trap dispatch for all 13 internal methods (Phase 57)
- [x] Implement `WeakRef` and `FinalizationRegistry` (builtins/weakref.cpp)
- [x] Implement destructuring assignment (Phase 61 — SpreadElement/RestElement)
- [x] Implement generator functions and `yield` / `yield*` (Phase 57, 64)
- [ ] Reach 95%+ on Test262 ES2024 subset

### Phase 2: Performance (JIT) — FRAMEWORK

- [x] Wire Baseline JIT tier-up from interpreter (JITProfiler wired — Phase 70)
- [x] Implement inline cache stubs for property access (ICManager in VM — Phase 52)
- [ ] Implement ZOpt IR builder from bytecode + type feedback
- [ ] ZOpt type specialization for numeric operations
- [ ] ZOpt inlining for small functions
- [ ] OSR entry/exit between interpreter <-> Baseline <-> ZOpt
- [ ] ZIR lowering pass from ZOpt IR
- [ ] Deoptimization with stack reconstruction

### Phase 3: GC Hardening — COMPLETE

- [x] Concurrent marking thread with write barrier enforcement (Phase 73)
- [x] Nursery semi-space copying collector (gc_nursery — Phase 73)
- [x] Old-gen incremental mark-sweep with configurable slice budget (gc_incremental — Phase 73)
- [x] Compaction for old-gen fragmentation (incremental_compactor — Phase 73)
- [x] Large object space (gc_large_object — Phase 73)
- [x] FinalizationRegistry callback scheduling (gc_finalizer_queue — Phase 73)

### Phase 4: WASM — FRAMEWORK

- [x] WASM binary validation (all sections) (WasmValidate — 35K lines)
- [x] WASM interpreter for tier-0 execution (WasmInterpreter — 49K lines)
- [x] WASM baseline compiler for hot modules (WasmBaselineCompile — 310K lines, stubs remain)
- [x] WASM-JS interop (import/export, memory sharing) (Phase 71)
- [ ] WASM threads + shared memory (WasmThreads.h — declaration only)
- [ ] WASM SIMD instructions (partial — AArch64 popcount stub)
- [ ] WASM GC proposal (WasmGC.h — declaration only)

### Phase 5: Browser Integration — FRAMEWORK

- [x] DOM node binding via host callbacks (browser/document.cpp)
- [x] Fetch API with nxhttp backend (browser/fetch.cpp — 31K lines)
- [x] Event loop integration with browser main loop (runtime/execution/event_loop.cpp)
- [x] Web Worker isolation (browser/worker.cpp)
- [x] Service Worker lifecycle management (browser/service_worker.cpp)
- [x] IndexedDB transactional storage (browser/indexeddb.cpp — 27K lines)
- [x] WebSocket bidirectional messaging (browser/websocket.cpp)

### Phase 6: Scale to 2M+ LOC

- [ ] Split `browser/` into `browser/dom/`, `browser/networking/`, `browser/events/`, `browser/storage/`, `browser/workers/`
- [ ] Implement all ES2025 Intl APIs (14 headers, 0 implementations)
- [x] Implement Temporal API (builtins/temporal.cpp — 20K lines)
- [ ] Implement Decorators (stage 3) (DecoratorsAPI.h — declaration only)
- [ ] Implement Pattern Matching (stage 2) (PatternMatchAPI.h — declaration only)
- [ ] Implement Record & Tuple (stage 2) (RecordTupleAPI.h — declaration only)

---

_Update this document alongside any major subsystem addition or API change._
_For bytecode instruction reference see `docs/zeprascript/bytecode-spec.md`. For JIT internals see `docs/zeprascript/jit-tiers.md`._
