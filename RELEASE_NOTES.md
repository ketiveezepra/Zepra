# Zepra Browser v0.1.0-beta — Release Notes

**Release Date:** May 27, 2026
**Build Channel:** Beta
**License:** KetiveeAI Public License (KPL-2.0)

---

> ⚠️ **Beta Release** — This is a pre-release version intended for testing and community feedback. It may contain bugs, incomplete features, and performance issues. Please report any issues on [GitHub](https://github.com/KetiveeAI/Zepra/issues).

---

## Why a Beta Release?

This is the **first public build** of Zepra Browser — an independent, from-scratch web browser built entirely by KetiveeAI without using V8, SpiderMonkey, WebKit, Blink, or any existing browser engine code.

### Motivation

1. **Community Validation** — We need real users testing on real hardware across Windows, Linux, and NeolyxOS to find issues we can't catch in development.

2. **Stability Benchmarking** — Our custom engine stack (ZepraScript, NXRender, NXNet) needs to be stress-tested under real-world browsing conditions before we can call it stable.

3. **Compatibility Testing** — Every web browser needs broad website compatibility testing. The beta lets us identify rendering, JavaScript, and networking gaps across the modern web.

4. **Performance Profiling** — Real usage patterns reveal performance bottlenecks that synthetic benchmarks miss. We need data from diverse hardware configurations.

5. **Security Auditing** — Exposing the browser to public testing helps identify security vulnerabilities early, before a stable release reaches a wider audience.

6. **Feedback Loop** — Your feedback directly shapes the roadmap. Features you report as missing or broken get prioritized for v0.2.0.

---

## What's Included

### Core Engine — ZepraScript
- Custom JavaScript engine with multi-tier JIT compilation
- ES2023+ syntax support (classes, async/await, generators, modules)
- WebAssembly interpreter with WASM binary parser
- Concurrent garbage collector with generational collection
- ZIR (Zepra Intermediate Representation) optimizing compiler

### Rendering — NXRender
- Hardware-accelerated OpenGL rendering pipeline
- HTML5/CSS3 parser and layout engine
- NXSVG vector graphics rasterizer
- NXFont cross-platform font rendering (FreeType integration)
- WebGL 1.0 compatible rendering context

### Networking — NXNet
- NxHTTP — lightweight HTTP/1.1 client (replaces libcurl)
- NxCrypto — TLS/SSL with OpenSSL integration
- DNS resolver with caching
- Cookie manager with secure storage
- HTTP response cache with LRU eviction
- WebSocket client support
- IDN/Punycode international domain support

### Browser Features
- Tabbed browsing with tab suspension (memory management)
- URL bar with search detection (powered by ketivee.com)
- Navigation history (per-tab back/forward)
- Download manager
- Settings panel with password manager
- Developer tools (DOM inspector, console, network panel)
- AI sidebar (powered by ZepraSearch ML services)
- Dark theme UI

### Platform Support
| Platform | Status | Notes |
|---|---|---|
| **Windows** (10/11) | ✅ Supported | MinGW/MSYS2 build, native Win32 APIs |
| **Linux** (x86_64) | ✅ Supported | X11, Freetype, OpenGL |
| **NeolyxOS** | ✅ Native | Primary target platform |
| **macOS** | 🔧 Experimental | Build scripts provided, untested |

---

## System Requirements

### Minimum
- **CPU:** x86_64, 2 cores
- **RAM:** 2 GB
- **Disk:** 200 MB free space
- **GPU:** OpenGL 2.0 capable
- **OS:** Windows 10+ / Ubuntu 20.04+ / NeolyxOS 1.0+

### Recommended
- **CPU:** x86_64, 4+ cores
- **RAM:** 4 GB+
- **GPU:** OpenGL 3.3+ capable
- **Network:** Broadband internet

---

## Installation

### Windows
Download and run the installer:
```powershell
# Run as Administrator
powershell -ExecutionPolicy Bypass -File Install-ZepraBrowser.ps1
```

### Linux (Debian/Ubuntu)
```bash
# Install .deb package
sudo dpkg -i zepra-browser_0.1.0-beta_amd64.deb
sudo apt-get install -f  # Fix any missing dependencies

# Or use the interactive installer
chmod +x install-zepra.sh
sudo ./install-zepra.sh
```

### macOS
Mount the `.dmg` and drag Zepra Browser to Applications.

---

## Known Issues

| Issue | Severity | Description |
|---|---|---|
| Limited CSS Grid support | Medium | Complex grid layouts may not render correctly |
| WebRTC not implemented | Low | Video/audio calling APIs not yet available |
| No extension API | Low | Browser extensions not supported in this release |
| PDF viewer basic | Low | NxPDF renders simple PDFs; complex layouts may fail |
| High memory usage | Medium | Tab suspension helps but GC tuning is ongoing |
| No sync | Low | No cross-device bookmark/history sync yet |

---

## How to Report Bugs

1. **GitHub Issues** (preferred): [github.com/KetiveeAI/Zepra/issues](https://github.com/KetiveeAI/Zepra/issues)
2. **Email**: support@ketivee.com
3. **Include**: OS version, steps to reproduce, expected vs. actual behavior, console output if available

---

## Roadmap — What's Next

### v0.2.0 (Planned)
- CSS Grid and Flexbox improvements
- Service Worker support
- IndexedDB persistence
- Browser extension API (basic)
- Performance optimizations (JIT tier 2)
- Improved font rendering (subpixel anti-aliasing)

### v0.3.0 (Planned)
- WebRTC support
- Cross-device sync
- PWA (Progressive Web App) installation
- HTTP/2 support
- Enhanced developer tools

### v1.0.0 (Stable)
- Feature-complete web standards support
- Full security audit
- Performance parity with major browsers
- Production-ready for daily use

---

## Credits

**Built by KetiveeAI** — an independent technology company building the future of web browsing from scratch.

- **ZepraScript** — Custom JavaScript engine
- **NXRender** — Custom rendering engine
- **NXNet** — Custom networking stack
- **NXSVG** — Custom SVG rasterizer

All components are original work by KetiveeAI, built without V8, SpiderMonkey, WebKit, Blink, or Gecko code.

---

## License

Zepra Browser is licensed under the **KetiveeAI Public License (KPL-2.0)**.
See [LICENSE.md](LICENSE.md) for full terms.

```
ZepraBrowser / ZepraScript
Copyright (c) 2025-2026 KetiveeAI
Licensed under KPL-2.0
https://zepra.ketivee.com/license
```

---

*Built in India. Independent by design.*
