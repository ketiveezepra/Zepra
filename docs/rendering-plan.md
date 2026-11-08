# Rendering plan: WordPress, React and YouTube

As of 2026-11-08, audited on `main` at `f8d15b1` (PR #54). Living copy with comments: the Claude Doc "Zepra plan: WordPress, React and YouTube".

## Goal and how we measure it

Zepra should render three classes of site correctly, each a step harder than the last: classic WordPress (server HTML + CSS + jQuery), client-rendered React apps, and YouTube watch pages with working playback. "Renders" means a pixel and behaviour check against Firefox on the same page, not just "no crash".

| Target | Done when | Main dependency |
| --- | --- | --- |
| WordPress (classic + block themes, WooCommerce) | Home, post, archive, search, comment form and a shop page match Firefox layout within a few pixels; menus, sliders and forms work | CSS layout (flex, grid, floats, positioning), fonts, images, jQuery |
| React / SPA apps | A Vite/CRA React 18 app, Next.js site and a TodoMVC hydrate, route with pushState and update on input | ES2020+ language, live DOM, events, fetch, modules, microtasks |
| YouTube | Watch page loads, player shows, video plays with sound, seek works, 1 hour without a crash | Polymer/Lit Web Components, fetch + streaming, MSE, H.264/VP9/AV1 + AAC/Opus decode, performance |

## Where Zepra stands today

The DOM, events, forms, fetch/XHR and DevTools are in good shape after PRs #27 to #54; the biggest blockers now sit in CSS and in how scripts load.

| Area | Works today | Blocking gaps (checked in code) |
| --- | --- | --- |
| CSS parsing and cascade | Selectors incl. `:is()`/`:where()`/`:not()`, `!important`, `@layer`, `@supports`, inline style, pseudo-elements | `@media` blocks are skipped whole, so every responsive rule is lost (`css_engine.cpp` `parseMediaRule`). `var()` is a text substitution of the first `:root` block in inline `<style>` only; external sheets get no expansion (`css_utils.cpp`). `@font-face` skipped. `@import` returns an empty rule |
| Layout | Block, inline text, tables (auto and fixed), flexbox incl. wrap/order/gap, relative/absolute positioning, floats (basic), scroll containers, overflow clip | No CSS Grid (`display:grid` parsed, never laid out). No `position: fixed`/`sticky` behaviour, no z-index stacking contexts. One border colour and one radius per box. No transforms, transitions, animations, filters, box-shadow. No bidi, textarea width bug |
| Text and fonts | nxfont UTF-8 with fallback fonts (#52), bold/italic | No web fonts (WOFF/WOFF2), no `font-family` matching beyond system fonts, no icon fonts (Dashicons, Font Awesome) |
| Script loading | Inline and external classic scripts, run in order after the whole document parses | No `defer`/`async` semantics, no parser-blocking scripts, no `document.write`. Scripts added by JS (`createElement('script')`) never load. `type="module"` runs as a classic script, so `import`/`export` fail. No import maps, no dynamic `import()` |
| ZepraScript language | ES2015 to ES2022 core, classes, async/await, Promises, generators, template literals, lexical globals | Regex not conformant (`{n,m}` ignored, no named groups, no `flags`/`source`). Strings are bytes, not UTF-16. Private fields, static blocks and BigInt literals do not parse. No `Intl`, `DataView`, `eval`. No `for await`. WeakMap holds keys strongly. Scripts run on the UI thread |
| Web APIs | Live DOM, events, MutationObserver, getComputedStyle, geometry, location/pushState, localStorage, fetch/XHR, canvas 2D (partial) | No Custom Elements or Shadow DOM, no `matchMedia`, IntersectionObserver/ResizeObserver are stubs, no `URL`, `Blob`/`FormData`/`FileReader`, no `crypto`, no `AbortController`, no streams, no IndexedDB, no `popstate`, no Workers, no WebSocket binding, iframes not rendered |
| Network | HTTPS with verified certs, keep-alive pool, chunked framing, cookie manager in NXNet, every request in DevTools | `Accept-Encoding: identity` (no gzip/br), HTTP/2 code exists in `nxhttp/http2.cpp` but the browser path uses HTTP/1.1, no disk cache, stylesheets fetched synchronously on the UI thread, no CORS. Load failures are being fixed in a separate thread |
| Media | `<img>` (PNG/JPEG/WebP/SVG), nxaudio decodes WAV/MP3/FLAC/Ogg | `<video>`/`<audio>` elements have no playback. `nxvideo` is not built and its decode thread is simulated (`player.c`). No H.264/VP9/AV1, no AAC/Opus, no MSE, no EME |
| Performance | Idle tab 77 to 79 MB, Wikipedia 105 MB, HN load 3.1 to 3.5 s | JIT not on the page path, full relayout after each DOM change, no incremental style, no off-thread parsing or decoding |

### Feature probe

[`test/manualtest/feature_probe.html`](../test/manualtest/feature_probe.html) runs 69 checks, one `<script>` each. In the browser built from `main` it passed 24; 4 more did not even parse. Screenshot: [`rendering-plan/feature-probe-2026-11-08.png`](rendering-plan/feature-probe-2026-11-08.png).

- **CSS:** a rule inside `@media (min-width: 100px)` is not applied; `display: grid` stacks its items; `var()` in an inline `<style>` works.
- **Syntax that does not parse:** private fields (`#x`), class static blocks, BigInt literals (`2n`), dynamic `import()`. One such script fails whole, so a bundle using any of them does not run.
- **Missing built-ins:** `Intl`, `DataView`, `TextEncoder`, `eval`; `new Function` throws "Code generation from strings is disallowed".
- **Wrong results:** `/a{2,3}/`, named groups, regex `flags`/`source`/sticky, UTF-16 `length`, ISO date parsing, a Proxy `get` trap.
- **Missing Web APIs:** `matchMedia`, `customElements`, `attachShadow`, `<template>.content`, `MessageChannel`, `requestIdleCallback`, `crypto`, `Blob`, `FormData`, `URL`, `URLSearchParams`, `AbortController`, `Request`/`Response`, `ReadableStream`, `indexedDB`, `Worker`, `WebSocket`, `Element.animate`, `CSS.supports`, `Image`, iframe `contentWindow`, `video.play`/`canPlayType`, `MediaSource`.
- **Passed:** lookahead/lookbehind, backreferences, async generators, optional chaining, logical assignment, `Reflect`, `WeakRef`, `structuredClone`, `JSON`, `queueMicrotask`, `document.cookie`, `CustomEvent`, `PointerEvent`, `performance.now`, `getComputedStyle` with `var()`.
- A script added with `createElement('script')` + `appendChild` did not run.

## Gap map by target

Fourteen gaps stand between Zepra and the three targets; the first four alone break nearly every WordPress theme and every React build. P0 blocks the target outright, P1 breaks visible parts of it, P2 is polish or speed.

| Gap | WordPress | React / SPA | YouTube | Module |
| --- | --- | --- | --- | --- |
| `@media` rules applied, `matchMedia` | P0 (every theme is responsive) | P1 | P0 | webCore/css |
| Real custom properties (`var()` through the cascade, inheritance, all sheets) | P0 (block themes, `theme.json` presets) | P0 (Tailwind, CSS-in-JS) | P0 (Polymer theme vars) | webCore/css |
| Dynamic `<script>` insertion, `async`/`defer`, `document.currentScript` | P0 (jQuery plugins, analytics, lazy loaders) | P0 (webpack/Vite chunks) | P0 | integration, webCore/scripting |
| ES modules: `type="module"`, `import`/`export`, dynamic `import()`, import maps | P2 | P0 (Vite output) | P1 | zepraScript/modules |
| CSS Grid | P0 (block themes, galleries, WooCommerce) | P1 | P1 | webCore/browser layout |
| Web fonts (`@font-face`, WOFF2, icon fonts) | P1 | P1 | P1 (YouTube Sans, icons) | webCore/rendering fonts |
| Regex conformance and UTF-16 strings | P1 (jQuery, sanitizers) | P0 (bundles, routers, i18n) | P0 | zepraScript/regex, core |
| `position: fixed`/`sticky`, z-index stacking, per-side borders, box-shadow, transforms, transitions | P1 (headers, menus, modals) | P1 | P0 (player overlays, masthead) | webCore/browser layout + paint |
| Custom Elements, Shadow DOM, `<template>`, `<slot>` | P2 | P2 | P0 (Polymer/Lit) | webCore/dom + scripting |
| Observers (Intersection, Resize), `MessageChannel`, `requestIdleCallback`, `crypto`, `URL`, `Blob`/`FormData`, `AbortController`, `popstate` | P1 (lazy images, comments) | P0 (React 18, routers, data libs) | P0 | webCore/scripting |
| gzip/brotli, HTTP/2, disk cache, async stylesheets | P1 (speed) | P1 | P0 (hundreds of requests) | NXNet |
| `<video>`/`<audio>` playback, MSE, H.264/VP9/AV1, AAC/Opus, A/V sync | P2 (embeds) | P2 | P0 | media/nxvideo, nxaudio |
| Off-main-thread work, incremental style/layout, JIT on page code | P2 | P1 | P0 (YouTube is about 8 MB of JS) | zepraScript, webCore |
| iframes | P1 (YouTube/maps embeds) | P2 | P1 (ads, embeds) | integration |

## Phased plan

Five phases, each closed by a gate measured on fixed test pages: WordPress first (CSS), then React (JS and Web APIs), then the YouTube page shell (components, compositing, network), then playback, then speed. Durations are estimates for one developer working with parallel threads, about 24 weeks in total.

| Phase | Weeks | Focus | Gate |
| --- | --- | --- | --- |
| 0 Measure | 1 | Saved site corpus, Firefox compare, WPT and test262 runners | Baseline scores checked in |
| 1 WordPress | 5 | `@media`, `var()`, Grid, web fonts, dynamic scripts | 6 themes within 2% of Firefox |
| 2 React and SPAs | 6 | ES modules, regex, UTF-16, Web APIs, observers | 6 SPAs hydrate, test262 at 90% |
| 3 YouTube shell | 5 | Custom Elements, Shadow DOM, layers, gzip, HTTP/2, JIT | Home page in 5 s, 30 fps scrolling |
| 4 YouTube plays | 6 | Decoders, demuxers, Media Source Extensions, A/V clock | 1080p for 10 min, under 1% dropped |
| 5 Fast and steady | ongoing | Memory targets, hardware decode, nightly corpus in CI | No score drop in CI |

### Phase 0: Measure (1 week)

1. Site corpus: save each test page with all its resources (SingleFile or `wget -p`) into `test/sites/`, so pages load offline in CI and in cloud containers.
2. Compare tool: render each page in Zepra (`ZEPRA_CAPTURE`) and headless Firefox at 1280 and 390 px wide, store a pixel-difference score and a console-error count per page.
3. Conformance runners: a Web Platform Tests subset (css/, dom/, html/, fetch/) through `testharness.js`, and a test262 subset for ZepraScript, each with a pass-rate number in CI.

Gate: a baseline table of scores for every page in this plan, checked into `docs/`.

### Phase 1: WordPress renders (5 weeks)

1. `@media` rules kept and matched against the viewport (width, height, orientation, `prefers-color-scheme`, `prefers-reduced-motion`, resolution), re-matched on resize; `window.matchMedia` with `change` events.
2. Custom properties in the cascade: `--x` stored per element, inherited, `var()` resolved at computed-value time with fallback and cycles, for every sheet and style attribute; remove the text-substitution hack in `css_utils.cpp`.
3. `@import` fetched and cascaded in order; stylesheets loaded off the UI thread.
4. `@font-face` with WOFF2 (needs a Brotli decoder), WOFF and TTF, `font-display`, `unicode-range`; icon fonts (Dashicons, Font Awesome) draw their glyphs.
5. Box painting: per-side border widths, colours and styles, per-corner radius, `box-shadow`, `outline`.
6. Positioning: `position: fixed` and `sticky`, z-index and stacking contexts painted in the right order.
7. CSS Grid: templates, `repeat()`, `minmax()`, `fr`, `auto-fill`/`auto-fit`, named areas, auto-placement, gaps, alignment.
8. Script loading: scripts inserted by JS load and run, `async`/`defer` order, `document.currentScript`, `onload`/`onerror` on scripts and images.
9. Basic `<iframe>`: a nested document with its own script context, so YouTube and map embeds show.

Test sites: a local WordPress (the `wordpress:latest` Docker image) with Twenty Twenty-One (classic), Twenty Twenty-Four and Twenty Twenty-Five (block themes), Astra, GeneratePress and WooCommerce Storefront; live: wordpress.org/news, techcrunch.com.

Gate: home, single post, archive, search, comment form and shop page of all six themes score within 2% of Firefox at both widths; menus, search and comments work.

### Phase 2: React and SPAs run (6 weeks)

1. ES modules: fetch the module graph, link, evaluate in order, `import.meta`, top-level await, dynamic `import()`, `<script type="importmap">`, `modulepreload`.
2. Regex engine: a backtracking matcher on UTF-16 with lookahead/lookbehind, `{n,m}`, named groups, backreferences, `u`/`v`/`y`/`s`/`d` flags, `lastIndex`, `source`/`flags`. Porting QuickJS's MIT-licensed `libregexp` is faster than a rewrite.
3. UTF-16 strings in the engine (or a Latin-1/UTF-16 dual representation), so `length`, `charCodeAt` and surrogate pairs match other browsers.
4. Language: private fields and methods (`#x`), class static blocks, BigInt literals, async generators, `for await`, real WeakMap/FinalizationRegistry, `Object.defineProperty` checks, Proxy traps on all paths, ISO date parsing.
5. Built-ins bundles need: `Intl` (NumberFormat, DateTimeFormat, PluralRules, Collator with an ICU subset), `DataView`, `eval` and `new Function` (behind a per-site policy rather than off everywhere).
6. Scheduling APIs React needs: `MessageChannel` (the React scheduler uses it), `requestIdleCallback`, `queueMicrotask`, `setTimeout` clamping.
7. Data APIs: `URL`/`URLSearchParams`, `Request`/`Response` constructors, `AbortController` on fetch, `ReadableStream` bodies, `Blob`/`File`/`FormData`/`URL.createObjectURL`, `crypto.getRandomValues`/`randomUUID`, `TextEncoder`/`TextDecoder`, `Image`.
8. Observers and events: real IntersectionObserver and ResizeObserver, `popstate` with session history, wheel/dblclick/contextmenu, pointer events, `scroll` events.
9. Incremental relayout: restyle and lay out only dirty subtrees, so React re-renders do not rebuild the whole layout tree.

Test sites: TodoMVC React, the default Vite React template (built locally), a Next.js static export, react.dev, GitHub repository page, Reddit (from `claude.md`).

Gate: all six hydrate, follow 10 in-app route changes and update on input with no console errors; test262 `language/` and `built-ins/RegExp` at 90% or more.

### Phase 3: Heavy page shell (YouTube without video) (5 weeks)

1. Custom Elements v1 (define, upgrade, lifecycle callbacks, `observedAttributes`).
2. Shadow DOM: `attachShadow`, slots, `:host`, `::slotted`, `::part`, scoped styles, event retargeting; `<template>` content; constructable and adopted stylesheets.
3. Paint and compositing: transforms, opacity layers, `transition` and `@keyframes`, `filter`, `clip-path`, scrolling layers that do not repaint the page.
4. Network: gzip and Brotli responses, HTTP/2 on the browser path (`nxhttp/http2.cpp`), a disk cache with `Cache-Control`/ETag, `preconnect`/`preload`, CORS checks.
5. Script speed: the baseline JIT on page code, bytecode cache for repeat visits, parsing off the UI thread.
6. Web Workers and `postMessage` with structured clone.

Test sites: youtube.com home, search and watch page (player area empty is fine), the Polymer and Lit starter apps, WPT `custom-elements/` and `shadow-dom/`.

Gate: the YouTube home page shows its grid of thumbnails, titles and sidebar within 5 s on a 300 ms round trip, scrolls at 30 fps or more, stays under 400 MB.

### Phase 4: YouTube plays (6 weeks)

1. HTMLMediaElement: `src`, `play()` returning a Promise, `pause`, `currentTime`, `duration`, `buffered`, `readyState`, `seeking`, volume, the full event sequence, `canPlayType`.
2. Decoders behind the `nxvideo` interface: H.264, VP9 and AV1 video, AAC and Opus audio (see the codec decision below); YUV frames uploaded to GL textures and converted in a shader.
3. Demuxers: MP4/fMP4 and WebM.
4. Media Source Extensions: `MediaSource`, `SourceBuffer` (`appendBuffer`, `remove`, `timestampOffset`, segments mode), `isTypeSupported`, `MediaCapabilities.decodingInfo`.
5. Playback clock: audio through nxaudio as the master clock, video frames presented on vsync, dropped-frame counting (`getVideoPlaybackQuality`).
6. Controls and fullscreen: Fullscreen API, keyboard shortcuts reaching the player.

Test pages: a local MP4 in `<video>`, the dash.js and Shaka Player demos with Big Buck Bunny served from `test/sites/`, then YouTube watch pages at 360p, 720p and 1080p.

Gate: 1080p VP9 plays 10 minutes with under 1% dropped frames, a seek resumes in under 1 s, audio and video stay within 45 ms, one hour of playback without a crash.

### Phase 5: Fast and steady (ongoing, 1 week of setup)

1. Memory targets from `claude.md`: YouTube watch page under 500 MB, 10 tabs under 300 MB, reclaim memory after the runaway-script guard.
2. Hardware video decode (VA-API on Linux) to cut CPU use.
3. Run the whole corpus nightly and fail CI on any score drop.

## Testing and tracking

Every work item lands as its own PR against `main` with a manual test page and a before/after screenshot, as the beta threads do now; progress is read from three numbers in CI.

| Measure | Source | Where it runs | Target by Phase 4 |
| --- | --- | --- | --- |
| Site score (pixel difference vs Firefox, console errors) | `test/sites/` corpus, 1280 and 390 px | Nightly job, cloud container | Under 2% on WordPress pages, under 5% on SPAs and YouTube |
| WPT pass rate | css/, dom/, html/, fetch/, custom-elements/, shadow-dom/, media-source/ subsets | CI on each PR | 80% on the chosen subsets |
| test262 pass rate | language/, built-ins/ | CI on each PR | 90% |
| Memory and timing | RSS and `load` time per corpus page | Nightly | The `claude.md` RSS table, plus YouTube under 500 MB |

`claude.md` keeps the timeline; this plan's phase and gate status goes in a new "Rendering targets" section there, updated by the docs PR that follows each phase.

## Risks and decisions

The codec choice is the one decision that changes the schedule; the rest are risks with a known fallback.

| Item | Why it matters | Recommendation |
| --- | --- | --- |
| Codecs: own decoders or a library | Writing H.264, VP9, AV1, AAC and Opus decoders is years of work; FFmpeg's libavcodec (LGPL, linked dynamically) or dav1d + libvpx + libopus give all five now | Use libavcodec behind `nxvideo` for Phase 4; replace one codec at a time later if Zepra must own them |
| DRM (Widevine) | Some YouTube content (films, some music) needs EME; Widevine licences are hard for a small browser to get | Leave EME out; most YouTube videos play without it |
| YouTube serves a reduced page to unknown browsers | It sniffs the User-Agent and features | Send a Firefox-compatible UA token and pass `MediaSource.isTypeSupported` honestly |
| Two CSS engines in the tree | `nxrender-cpp/web/css` (cascade, custom properties, typed OM) is built but the page path uses `webCore/css` | Pick `webCore/css` as the one engine; borrow `custom_properties.cpp` and `css_math.cpp` from nxrender for Phase 1, then delete the unused copy |
| Scripts on the UI thread | YouTube and big React apps run long tasks; the 10 s guard stops them | Phase 3 moves parsing off-thread; a full script thread is a later project |
| Regex rewrite size | Every bundle uses regexes; a home-grown engine takes months to reach conformance | Port QuickJS `libregexp` (MIT) in Phase 2 |
| Memory goal vs heavy sites | The 50 to 150 MB target from `claude.md` cannot hold for a YouTube watch page | Keep the target for light pages; set 500 MB for YouTube |
