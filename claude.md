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
| 2026-11-07 | `feat/devtools-inspector-editing` | **Inspector editing** (open): double-click an attribute or a text to edit it, Edit as HTML (F2 or the context menu), Add Attribute, Delete and Duplicate Node, Copy outerHTML and Copy CSS Selector, Ctrl+Z undo; tree keys (arrows fold and move, Delete, Ctrl+F, Ctrl+C); a markup search box (text, tag, attribute or CSS selector, Enter / Shift+Enter step through matches); Rules values and names edited in place and new declarations on a block's last line, for the selected element only (through its style attribute, never the shared sheet); forced :hover / :active / :focus from the `:hov` bar or the context menu. The comment tokenizer no longer drops a comment's first character, and the browser relayouts after DOM changes on pages without scripts |
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
| 2026-11-07 | `feat/form-post` | **POST form submission** (open): a form with `method="post"` (or a submitter's `formmethod`) sends its data in the request body to the action URL (its query kept) with the form's `enctype` / `formenctype`: `application/x-www-form-urlencoded`, `multipart/form-data` (a boundary that does not occur in the data; names escaped as browsers do; a file input sends an empty file part, there is no file picker yet) or `text/plain`; `target=_blank` posts in a new tab. Redirects after a POST are followed as GET (303/302/301) and the address bar shows where the page ended up (also for redirected GET loads). Not yet: reload and Back re-request the result with GET instead of asking to resend; `method="dialog"` |
| 2026-11-07 | `claude/layout-scroll-pseudo-borders` | **Scroll containers, pseudo-element styles, borders** (open): elements with `overflow` other than visible clip to their padding box and scroll: the mouse wheel scrolls the innermost one under the pointer that can still move (then the page), `scroll`/`auto` show an overlay thumb, `hidden` scrolls from scripts only; `scrollTop`/`scrollLeft`/`scrollTo`/`scrollBy` on elements, rects and hit-testing follow every container's scroll, `scrollIntoView` scrolls the containers around the element first; a clipping box contains its floats; `overflow: x y`. `getComputedStyle(el, '::before')` (and `::after`, `::marker`, `::first-line`, `::first-letter`, `::placeholder`, `::selection`) returns the pseudo-element's cascaded style, `content` included. Borders paint inside the border box (a uniform border was a centered GL line, 2px outside for a 4px border). No `scroll` events yet |
| 2026-11-07 | `feat/beta-missing-apis` | **window is the global object; mouse events; template literals** (open): `var`/function declarations at script level are `window` properties and `window.x = 1` is a bare `x` in later scripts (UMD bundles, jQuery's `$`, `dataLayer`); top-level `this` and `globalThis` are `window`; reading an undeclared name throws `ReferenceError` (`typeof` does not); a script's `var` names exist before it runs; `window.scrollY`/`innerWidth`/... are `[Replaceable]`. mousedown on the press (a canceled one keeps focus where it was), mouseup and click on the release, the click at the element holding both (a link is followed only when pressed and released on it and the click was not canceled); mouseover/mouseout/mouseenter/mouseleave/mousemove as the pointer moves; `pageX/Y` include the scroll. `` `${1}${2}` `` is `"12"` (was 3); tagged templates get a strings array with `.raw`, one per site; `String.raw`; `\x` escapes, surrogate pairs as two escapes, line continuations; `` `${"}"}` `` parses; a UTF-8 byte-order mark at the start of a script is skipped |
| 2026-11-07 | `feat/mutation-observer` | **MutationObserver** (open): a real observer in place of the stub: `observe(target, {childList, attributes, characterData, subtree, attributeOldValue, characterDataOldValue, attributeFilter})` with the spec's option checks, records (`type`, `target`, `addedNodes`/`removedNodes`, `previousSibling`/`nextSibling`, `attributeName`, `oldValue`) delivered in a microtask, one batch per observer, `takeRecords()`, `disconnect()`, `MutationRecord`. The C++ DOM reports every child, attribute and text change to its document's sink (`DOMMutationSink`); a callback that throws goes to the console. Not yet: transient registrations for removed subtrees; `innerHTML` gives one record per node rather than one per call |
| 2026-11-07 | #35 | **`window.getComputedStyle`** (open): a live, read-only `CSSStyleDeclaration` with the values the DevTools Computed pane shows (the serializer moved to `webCore/css/computed_style_values` and is shared): `getPropertyValue`, camelCase and CSS-name reads, `length`/`item(i)`, `NoModificationAllowedError` on writes. A read after a DOM change recomputes styles first. No pseudo-elements; properties the pane leaves out read `""` |
| 2026-11-07 | #34 | **Layout geometry for scripts** (open): `getBoundingClientRect`, `getClientRects`, `offsetWidth/Height/Top/Left`, `offsetParent`, `clientWidth/Height/Top/Left`, `scrollWidth/Height` read the layout boxes (the same positions `paintBox` draws); `window.innerWidth/innerHeight` are the content area, `scrollX/scrollY`, `scrollTo/scrollBy`, `documentElement.scrollTop` and `scrollIntoView` read and move the page scroll. A read after a DOM change restyles and lays the page out first (removed nodes are freed at the end of the frame). Elements other than the page do not scroll yet |
| 2026-11-07 | #36 | **Keys, typing and forms on the live DOM** (open): keydown/keypress/keyup at the focused element (else the body) with `key`/`code`/`keyCode`/modifiers mapped from the platform key; typing goes through beforeinput/input into the field, Backspace deletes a whole character, pasted and IME text too, `maxlength`/`readonly` honored; a key the page cancels does nothing else, a key a field uses no longer scrolls the page. Focus is the page's: clicks, `focus()`/`blur()` and `document.activeElement`, with focus/blur/focusin/focusout and change when the user changed the value (on blur or Enter). Controls keep a live `value`/`checked` apart from their attributes (`defaultValue`/`defaultChecked`), which layout, `:checked` and `:placeholder-shown` read. Clicks activate: checkboxes and radios toggle before click listeners (back if canceled), labels click their control, submit/reset buttons submit or reset; Enter submits through the default button; `form.submit()/requestSubmit()/reset()/elements`, `control.form`; SubmitEvent with `submitter`; GET submissions navigate (POST not yet) |
| 2026-11-07 | #33 | **Console input, filter and source links** (open): autocomplete of globals and properties while typing (`doc` -> `document`, `document.bo` -> `body`; Tab/Enter accept, Up/Down move, Ctrl+Space opens; values are read through data properties and native getters only, so no page script runs); multi-line input (Shift+Enter, Enter on an unfinished expression adds a line, history from the first/last line); a text filter (substring, `-text` hides, `/regex/`) and Errors/Warnings/Logs/Info/Debug buttons with a hidden count; every message shows `file.js:line`, and a click opens the new Sources tab at that line. ZepraScript chunks now know their script, inline scripts count lines from their document, and stack traces read `at f (file:line)` instead of a bytecode offset |
| 2026-11-07 | #32 | **Network request details** (open): click a request in the Network panel for a details pane with Headers (General, response headers as received, request headers as sent: Host, cookies and defaults included), Response (the start of a text body, JSON re-indented, minified lines wrapped, up to 64 KB kept per request) and Timings (Blocked, Redirect, DNS, Connecting, TLS, Sending, Waiting, Receiving, measured in NXNet's HTTP client; a kept-alive connection shows no DNS/connect/TLS); a Waterfall column; a "Filter URLs" box read the Firefox way (`-term`, `status-code:`, `method:`, `domain:`, `mime-type:`, `larger-than:`, `is:failed`); "Disable cache" while the panel is open sends `Cache-Control`/`Pragma: no-cache` and fetches images an earlier load cached again (into the same texture). Peak RSS unchanged (`docs/devtools/network-details.md`) |
| 2026-10-29 | #31 | **Inspector Rules pane** (open): Firefox-style Rules view between the DOM tree and Layout/Computed: `element { }` (style attribute) first, then every matched rule from the engine's cascade, most specific first, with origin and source (`site.css, rule 4`, `<style> 1, rule 2`, `user agent, rule 31`); declarations that lose the cascade struck through (per longhand, so a shorthand is struck only when all of its longhands lose); "Inherited from <tag>" groups with inherited properties only; clicking a selector highlights every element it matches on the page; clicking a declaration switches it off for that element only (through its style attribute, never the shared sheet); hovering a Computed property shows the rule it came from. The cascade now honors `!important` (author, then style attribute, then user, then user-agent), and each `<style>` block is registered once (the raw-HTML fallback added every one a second time) |
| 2026-10-29 | #30 | **Network panel sees every request; keep-alive** (open): every request of a tab in the DevTools Network panel (document, stylesheets, scripts, images from the loader threads, fetch/XHR) with type filter chips and a summary of requests, bytes on the wire and total time; a thread-safe `NetworkMonitor`; real `fetch()` (Promise, `Response.text()/json()`, `Headers`) and `XMLHttpRequest` settled from the timer pump; HTTP/1.1 responses read to their announced end (Content-Length, chunked), a shared keep-alive pool, TLS session resumption, stylesheets and scripts requested in parallel: Hacker News `load` 5.5 s → 3.1–3.5 s here, Wikipedia 18 requests in the panel (`docs/devtools/network-timings.md`) |
| 2026-10-29 | #29 | **Console values**: `console.log(obj)` shows Firefox-style previews (`Object { a: 1, b: (2) […] }`, `Array(3) [ … ]`, `Map(2) { "a" → 1 }`, `<div id="x">`, `function f()`, errors with their stack, getters shown not run) as expandable rows; `console.info/debug/dir/table/trace/assert/count/time/group`; console helpers `$0` (Inspector selection), `$`, `$$`, `$_`, `clear()`; input errors as `name: message` with their stack, syntax errors with `<console>:line:col` |
| 2026-10-26 | #27 | **Live DOM for scripts** (open): one JS object per node whose properties change the C++ DOM (appendChild, innerHTML, textContent, classList, style, dataset, cloneNode, fragments); DOM events with capture/bubble, `addEventListener`, on* handlers, DOMContentLoaded/load, page clicks (a canceled click does not navigate); `querySelector` matches through the cascade; the Inspector follows script-built content |
| 2026-10-25 | #26 | **DevTools** (open): docked Inspector (live DOM tree, page highlight, box model, computed styles, picker; matched rules came with the Rules pane row above), Console (levels, eval in the page), Network panel; F12 keeps the page usable |
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
3. **DOM bindings** — the tree, attributes, classes, inline style and click events are live (#27). Keys, typing, focus and form submission (GET and POST) are live too (input events PR, form POST PR). Layout geometry is live (`getBoundingClientRect`, `offset*`, `client*`, page scrolling; #34), and elements with `overflow` scroll (scroll containers PR). Mouse events are live (mousedown/up, click on release, over/out/enter/leave/move). Still missing: `scroll` events, keyboard scrolling inside elements, drag and drop, `contextmenu`/`dblclick`/`wheel` events, a caret position in fields (typing goes at the end), `<select>` picking, file uploads, `IntersectionObserver`/`ResizeObserver` (stubs; `MutationObserver` is live), `document.write`. `window.getComputedStyle` is live (#35), pseudo-elements too (scroll containers PR). `fetch`/XHR are real (network panel PR); still no `Request`/`Response` constructors, `arrayBuffer()`/`blob()`, `AbortController`, streaming, CORS checks.
4. **Network speed** — responses end where their headers say and connections are kept alive (network panel PR). Hacker News `load` fires at 3.1–3.5 s on a 300 ms round trip, where `curl` alone needs 1.4–1.8 s for the document. Left: HTTP/2, compression (`Accept-Encoding: identity`), a disk cache, and images start only after layout.
5. **Memory after the runaway guard** — reclaim what a killed script allocated (needs the GC stage).

### ⚠️ Known gaps (ZepraScript)
- Regex engine is not conformant: no lookaround, `{n,m}` does nothing, byte-based matching, no named groups or backreferences, slow. RegExp objects have no `source`/`flags`/`global`/`lastIndex` properties.
- Strings are byte-indexed, not UTF-16.
- Async generators, `for await` and top-level await are not supported. WebAssembly is still a fake namespace; URL/URLSearchParams, IntersectionObserver and ResizeObserver in ScriptContext are stubs.
- `Object.defineProperty` does not reject redefining a non-configurable property.
- WeakMap/WeakSet hold their keys strongly (no weak-reference hook in the collector yet).
- `let`/`const`/`class` at script level are `window` properties (in browsers they are globals but not properties of `window`).
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
> *Generated based on the new Neolyx OS engineering philosophy. Status and timeline last updated 2026-11-07.*
