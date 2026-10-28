<div align="center">
  <h1>🚀 ZepraBrowser Global TODO & Goals</h1>
  <p><strong>Predictable &gt; Flashy | Stable &gt; Feature-Rich | Memory-Efficient &gt; Benchmark-Winning</strong></p>
</div>

---

## 🎯 The Ultimate Goal
**"Can it render this website 1000 times without degrading?"**
Transform ZepraBrowser into a robust, memory-efficient alternative to Electron for the Neolyx OS ecosystem. Target footprint: **50–150 MB** per app context. 
This is a real user experience win for constrained hardware and the rural India/Bihar context where lower-spec machines are common.

---

## 📊 The Metric That Actually Matters for Beta (NeolyxOS Targets)
If we hit these numbers, Zepra becomes the most memory-efficient full-featured browser in existence.

| Condition | Target RSS | Measured (Windows, Release, 2026-10-25) |
| :--- | :--- | :--- |
| **Idle, 1 tab** | `< 80MB` | 77–79 MB (small local page) ✅ |
| **10 tabs, moderate sites** | `< 300MB` | not measured yet |
| **50 tabs** | `< 800MB` | not measured yet |
| **After closing all tabs (Z ≈ X)** | `< 100MB` | not measured yet |
| **24hr runtime, idle** | `< 5% memory growth` | not measured yet |

Single-site figures: Wikipedia article 105–107 MB, flat over 45 s; Hacker News 86–88 MB.
After the 1 GB runaway-script guard fires, the memory stays committed (about 1.1 GB): see Known gaps.

*Note: If memory after closing all tabs is the same as when 50 tabs were open (Z ≈ Y), we have leaks. Our Shenandoah-style GC should handle this well, but we must fix the JIT cache leak which currently acts as a floor.*

---

## 🗓️ Timeline

Merged pull requests on `main` (ketiveezepra/Zepra), newest first. Dates are merge dates.

| Date | PR | What changed |
| :--- | :--- | :--- |
| 2026-10-29 | — | **Network panel sees every request; keep-alive** (open): every request of a tab in the DevTools Network panel (document, stylesheets, scripts, images from the loader threads, fetch/XHR) with type filter chips and a summary of requests, bytes on the wire and total time; a thread-safe `NetworkMonitor`; real `fetch()` (Promise, `Response.text()/json()`, `Headers`) and `XMLHttpRequest` settled from the timer pump; HTTP/1.1 responses read to their announced end (Content-Length, chunked), a shared keep-alive pool, TLS session resumption, stylesheets and scripts requested in parallel: Hacker News `load` 5.5 s → 3.1–3.5 s here, Wikipedia 18 requests in the panel (`docs/devtools/network-timings.md`) |
| 2026-10-26 | #27 | **Live DOM for scripts** (open): one JS object per node whose properties change the C++ DOM (appendChild, innerHTML, textContent, classList, style, dataset, cloneNode, fragments); DOM events with capture/bubble, `addEventListener`, on* handlers, DOMContentLoaded/load, page clicks (a canceled click does not navigate); `querySelector` matches through the cascade; the Inspector follows script-built content |
| 2026-10-25 | #26 | **DevTools** (open): docked Inspector (live DOM tree, page highlight, box model, computed styles, picker), Console (levels, eval in the page), Network panel; F12 keeps the page usable |
| 2026-10-25 | #20 | **Promises and microtasks** (open): per-VM microtask queue drained after scripts, timers and rAF callbacks; spec Promise (thenables, then/catch/finally, all/allSettled/race/any/withResolvers/try, subclassing); async/await; try/catch and closures across await/yield |
| 2026-10-25 | #17 | **ZepraScript built-ins pass** (open): spec-correct number/string conversion, Number/Boolean/String/Object/Array as real constructors, one set of intrinsic prototypes per VM, Error chain, real Symbol, hashed Map/Set/WeakSet, Array.from, RegExp fixes, pages get the engine's built-ins instead of stubs, conformance suite |
| 2026-10-25 | #16 | Parser: `super(...)`/`super.x`, class fields and class expressions, getters/setters and keyword keys in literals, object spread, async and destructuring arrows; built-in subclassing |
| 2026-10-24 | #15 | Runaway scripts stopped (10 s / 1 GB `ScriptRunGuard`); real frame model, calls, closures, scoping and exceptions in the VM |
| 2026-10-21 | #14 | HTTPS certificates are verified |
| 2026-10-21 | #13 | Scripts can change the page: timers, `requestAnimationFrame`, re-render after DOM changes |
| 2026-10-21 | #12 | Render loop no longer spins a CPU core |
| 2026-10-21 | #11 | Page freezes, `:hover`/`:active`/`:focus`, local files on Windows |
| 2026-10-21 | #10, #8 | macOS build; NXRender on macOS (Cocoa + NSOpenGL) |
| 2026-10-21 | #9 | NxPdf test made real; NXRender tests run in CI |
| 2026-10-21 | #7 | Copy/paste on Linux (X11) |
| 2026-10-15 | #5, #6 | Linux CI (GLU) and icon-name case fixes |
| 2026-10-13 | #4 | NXRender browser builds and runs on Windows |
| 2026-10-09 | `9eda6cc` | Per-tab VM; history, settings and bookmarks saved to disk |
| 2026-06-14 | `50d4372` | DOM node classes and the JS↔DOM bridge |
| 2025-12 → 2026-06 | — | Engine, NXRender, layout/CSS, JIT and GC groundwork (pre-PR history) |

### 🔨 Active now / next
1. **Built-ins pass** (#17) — in review.
2. **Promises and microtasks** (#20) — in review. Next for async: async generators, `for await`, top-level await.
3. **DOM bindings** — the tree, attributes, classes, inline style and click events are live (#27). Still missing: layout geometry for scripts (`getBoundingClientRect`, `offsetWidth`, `scrollTop` read 0), mouse-move/keyboard/input/submit events, form control values beyond the attribute, `MutationObserver`/`IntersectionObserver`/`ResizeObserver` (stubs), `document.write`, `window.getComputedStyle`. `fetch`/XHR are real (network panel PR); still no `Request`/`Response` constructors, `arrayBuffer()`/`blob()`, `AbortController`, streaming, CORS checks.
4. **Network speed** — responses end where their headers say and connections are kept alive (network panel PR). Hacker News `load` fires at 3.1–3.5 s on a 300 ms round trip, where `curl` alone needs 1.4–1.8 s for the document. Left: HTTP/2, compression (`Accept-Encoding: identity`), a disk cache, and images start only after layout.
5. **Memory after the runaway guard** — reclaim what a killed script allocated (needs the GC stage).

### ⚠️ Known gaps (ZepraScript)
- Regex engine is not conformant: no lookaround, `{n,m}` does nothing, byte-based matching, no named groups or backreferences, slow. RegExp objects have no `source`/`flags`/`global`/`lastIndex` properties.
- Strings are byte-indexed, not UTF-16.
- Async generators, `for await` and top-level await are not supported. WebAssembly is still a fake namespace; URL/URLSearchParams and the observers in ScriptContext are stubs.
- Reading an undeclared variable does not throw a ReferenceError; tagged templates (``String.raw`x` ``) are not supported.
- A UTF-8 byte-order mark at the start of a script is a parse error.
- `Object.defineProperty` does not reject redefining a non-configurable property.
- WeakMap/WeakSet hold their keys strongly (no weak-reference hook in the collector yet).
- `globalThis` in a page is the engine's global object, not `window`; `var x` at the top level is not a `window` property.
- NodeList/HTMLCollection are plain arrays (with `item()`), not live collections. `innerText` is `textContent` (no layout-aware line breaks).

## 🛠️ High-Priority Bug Fixes (The "Unfreeze" Sprint)

### 1. ZepraScript Engine Stabilization
- [x] **Parser Fix:** Comma operator `(0, _.E)` parses and evaluates (conformance case `comma_operator`); the parser no longer loops on an unexpected token (#15).
- [~] **JIT Memory Leak (B06):** `CodeCache::compact()` now decrements `usedSize_`. Not verified: that the executable memory itself is released.
- [ ] **W^X Vulnerability:** *(not started)* Re-architect the `mprotect` calls in the JIT so compiling a new block doesn't alter permissions of existing active blocks on the same page.
- [x] **VM Isolation (B08):** `s_vm`/`s_vmContext` are gone; each tab owns its VM (`9eda6cc`), and built-in prototypes are per VM or per global (#17). Not verified: ObjectSecurity pointer reuse.

### 2. Rendering & UI Thread Unblocking
- [~] **Asynchronous JS Execution:** Scripts still run on the UI thread. Mitigated: a runaway script is stopped after 10 s or 1 GB of growth and the page keeps working (#15).

### 3. Network Reliability
- [x] **Keep-Alive Delay:** `http_client` reads each response to the end its headers announce (Content-Length, chunked, close) and reuses connections through a shared pool; TLS sessions resume (NXNet `fix/http-keepalive` + the network panel PR). HN `load`: 5.5 s → 3.1–3.5 s here, the rest is the document's own fetch.
- [x] **TLS Integration:** HTTPS works through OpenSSL in NXNet (Wikipedia, Hacker News load) and certificates are verified (#14).
- [x] **Chunked Decoding:** one reader (`http_response_reader`) frames responses for both `http_client` and the `nxhttp` C engine: chunked with extensions and trailers, Content-Length lists, 1xx/204/304/HEAD without a body; covered by `nxhttp_tests` (`HttpFraming.*`).

### 4. Storage Persistence (B13-B15)
- [x] **Data Saving:** History, settings and bookmarks are written to disk (`9eda6cc`). Not run yet: the crash-recovery test below. `localStorage` is in-memory only.

---

## 🧪 The "Ironclad" Testing Suite & Priority Sequence

To ship a Stable Beta, run tests in this exact priority:

### 1. The "Immediate Bug Exposure" Tests (Run First)
- **Memory Stability:** `valgrind --leak-check=full --track-origins=yes ./zepra_browser`
  *Goal: Catch the JIT cache leak (B06) where 50+ tabs fill 16MB and silently die.*
- **Long Runtime:**
  *Goal: Catch ObjectSecurity pointer reuse (B08) that only manifests after enough alloc/free cycles.*

### 2. The "Low Resource" Test (Run Second - After engine fixes)
- **Action:** `systemd-run --scope -p MemoryMax=1G ./zepra_browser`
- **Goal:** Immediately expose whether tab discard (TabSuspender) actually works under pressure instead of just printing to logs.

### 3. The "Heavy Site" Test (Run Third - After TLS fix)
- **GitHub:** Tests HTTPS + complex CSS + moderate JS.
- **Reddit:** Tests infinite scroll + lazy loading.
- **Gmail:** Tests heavy JS + WebSockets.
- **Discord/Figma/Docs:** Save for last (WebGL/Canvas/heavy APIs).

### 4. The "Crash Recovery" Test (Run Before Beta)
- **Action:** Open 20 tabs, `kill -9 zepra_browser`, then restart.
- **Goal:** Verify what state survives. (Currently fails due to stubbed storage).

---
> *Generated based on the new Neolyx OS engineering philosophy. Status and timeline last updated 2026-10-25.*
