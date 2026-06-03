# Zepra Browser — Installer & Distribution Guide

> ⚠️ **Beta Release** — v0.1.0-beta

This directory contains the official installer scripts and package builders for all supported platforms.

---

## Quick Start

| Platform | Method | Command |
|---|---|---|
| **Windows** | GUI Installer | `powershell -ExecutionPolicy Bypass -File installer\windows\Install-ZepraBrowser.ps1` |
| **Windows** | Silent | `... Install-ZepraBrowser.ps1 -Silent` |
| **Linux** | .deb package | `sudo apt install ./dist/zepra-browser_0.1.0-beta_amd64.deb` |
| **Linux** | Interactive | `sudo ./installer/linux/install-zepra.sh` |
| **Linux** | AppImage | `./dist/ZepraBrowser-0.1.0-beta-x86_64.AppImage` |
| **macOS** | DMG | Mount and drag to Applications |

---

## Building Distribution Packages

### Prerequisites

Build the browser first:
```bash
# Linux / macOS
./build.sh release

# Windows
cmake -G Ninja -B build-win -DCMAKE_BUILD_TYPE=Release
cmake --build build-win --parallel
```

### Windows — Create distributable .zip

```powershell
powershell -ExecutionPolicy Bypass -File installer\windows\build-installer.ps1
# Output: dist\ZepraBrowser-0.1.0-beta-win64.zip
```

### Linux — Build .deb package

```bash
chmod +x installer/linux/build-deb.sh
./installer/linux/build-deb.sh
# Output: dist/zepra-browser_0.1.0-beta_amd64.deb
```

### Linux — Build AppImage

```bash
chmod +x installer/linux/build-appimage.sh
./installer/linux/build-appimage.sh
# Output: dist/ZepraBrowser-0.1.0-beta-x86_64.AppImage
```

### macOS — Build DMG

```bash
chmod +x installer/macos/build-dmg.sh
./installer/macos/build-dmg.sh
# Output: dist/ZepraBrowser-0.1.0-beta.dmg
```

---

## File Structure

```
installer/
├── windows/
│   ├── Install-ZepraBrowser.ps1      # WPF GUI installer (dark theme, EULA, progress)
│   ├── Uninstall-ZepraBrowser.ps1    # Clean uninstaller
│   └── build-installer.ps1           # Packages everything into a .zip
├── linux/
│   ├── build-deb.sh                  # Builds .deb with proper DEBIAN/ structure
│   ├── install-zepra.sh              # Interactive installer (GUI + terminal modes)
│   ├── build-appimage.sh             # Self-contained AppImage builder
│   └── zepra-browser.desktop         # FreeDesktop .desktop entry
└── macos/
    ├── build-dmg.sh                  # Creates .app bundle + .dmg
    └── Info.plist                    # App bundle metadata

legal/
├── EULA.md                           # Rich-text EULA (KetiveeAI)
└── EULA.txt                          # Plain-text EULA (embedded in installers)
```

---

## Install Locations

| Platform | Location |
|---|---|
| Windows | `C:\Program Files\KetiveeAI\ZepraBrowser\` |
| Linux (.deb) | `/opt/ketiveeai/zepra-browser/` |
| Linux (symlink) | `/usr/bin/zepra-browser` |
| macOS | `/Applications/ZepraBrowser.app` |

---

## Uninstalling

### Windows
- Via **Add/Remove Programs** → "Zepra Browser (Beta)"
- Or: `powershell -ExecutionPolicy Bypass -File "C:\Program Files\KetiveeAI\ZepraBrowser\Uninstall-ZepraBrowser.ps1"`

### Linux (.deb)
```bash
sudo apt remove zepra-browser
# or
sudo dpkg -r zepra-browser
```

### Linux (manual install)
```bash
sudo /opt/ketiveeai/zepra-browser/uninstall.sh
```

### macOS
Drag `ZepraBrowser.app` from Applications to Trash.

---

*Copyright © 2025-2026 KetiveeAI — Licensed under KPL-2.0*
