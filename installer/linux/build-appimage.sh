#!/bin/bash
# =============================================================================
# Zepra Browser — AppImage Builder
# Creates a self-contained AppImage for any Linux distribution.
# Copyright (c) 2025-2026 KetiveeAI. All rights reserved.
# =============================================================================

set -e

VERSION="0.1.0-beta"
APP_NAME="ZepraBrowser"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUTPUT_DIR="$ROOT_DIR/dist"
APPDIR="$OUTPUT_DIR/${APP_NAME}.AppDir"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${CYAN}"
echo "============================================="
echo "  Zepra Browser — AppImage Builder"
echo "  Version: ${VERSION}"
echo "============================================="
echo -e "${NC}"

# Detect binary location
BIN_DIR="$ROOT_DIR/build/bin"
if [ ! -f "$BIN_DIR/zepra_browser" ]; then
    BIN_DIR="$ROOT_DIR/build_ninja/bin"
fi
if [ ! -f "$BIN_DIR/zepra_browser" ]; then
    echo -e "${RED}[ERROR] zepra_browser binary not found.${NC}"
    echo "Build first with: ./build.sh release"
    exit 1
fi

# Ensure appimagetool is available
APPIMAGETOOL=$(command -v appimagetool 2>/dev/null || true)
if [ -z "$APPIMAGETOOL" ]; then
    echo -e "${YELLOW}appimagetool not found. Attempting to download...${NC}"
    TOOLS_DIR="$ROOT_DIR/tools"
    mkdir -p "$TOOLS_DIR"
    APPIMAGETOOL="$TOOLS_DIR/appimagetool-x86_64.AppImage"
    if [ ! -f "$APPIMAGETOOL" ]; then
        wget -q -O "$APPIMAGETOOL" \
            "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage"
        chmod +x "$APPIMAGETOOL"
    fi
fi

mkdir -p "$OUTPUT_DIR"

echo "[1/6] Creating AppDir structure..."
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin"
mkdir -p "$APPDIR/usr/share/applications"
mkdir -p "$APPDIR/usr/share/icons/hicolor/scalable/apps"
mkdir -p "$APPDIR/usr/share/doc/zepra-browser"
mkdir -p "$APPDIR/opt/zepra-browser/resources/icons"
mkdir -p "$APPDIR/opt/zepra-browser/resources/web"
mkdir -p "$APPDIR/opt/zepra-browser/legal"

echo "[2/6] Copying binaries..."
cp "$BIN_DIR/zepra_browser" "$APPDIR/usr/bin/"
chmod +x "$APPDIR/usr/bin/zepra_browser"
ln -sf usr/bin/zepra_browser "$APPDIR/AppRun"

for tool in zepra-repl zepra-dump-bytecode; do
    if [ -f "$BIN_DIR/$tool" ]; then
        cp "$BIN_DIR/$tool" "$APPDIR/usr/bin/"
        chmod +x "$APPDIR/usr/bin/$tool"
    fi
done

echo "[3/6] Copying resources..."
if [ -d "$ROOT_DIR/resources/icons" ]; then
    cp -r "$ROOT_DIR/resources/icons/"* "$APPDIR/opt/zepra-browser/resources/icons/" 2>/dev/null || true
fi
if [ -d "$ROOT_DIR/resources/web" ]; then
    cp -r "$ROOT_DIR/resources/web/"* "$APPDIR/opt/zepra-browser/resources/web/" 2>/dev/null || true
fi

# App icon (AppImage requires a top-level icon named after the app)
if [ -f "$ROOT_DIR/resources/icons/zepra.svg" ]; then
    cp "$ROOT_DIR/resources/icons/zepra.svg" \
       "$APPDIR/usr/share/icons/hicolor/scalable/apps/zepra-browser.svg"
    cp "$ROOT_DIR/resources/icons/zepra.svg" "$APPDIR/zepra-browser.svg"
fi

echo "[4/6] Copying legal documents..."
if [ -d "$ROOT_DIR/legal" ]; then
    cp "$ROOT_DIR/legal/"* "$APPDIR/opt/zepra-browser/legal/" 2>/dev/null || true
fi
[ -f "$ROOT_DIR/LICENSE.md" ] && cp "$ROOT_DIR/LICENSE.md" "$APPDIR/opt/zepra-browser/legal/"
[ -f "$ROOT_DIR/RELEASE_NOTES.md" ] && cp "$ROOT_DIR/RELEASE_NOTES.md" "$APPDIR/usr/share/doc/zepra-browser/"

echo "[5/6] Writing desktop entry and AppStream metadata..."

# Desktop entry (required by AppImage)
cat > "$APPDIR/zepra-browser.desktop" <<EOF
[Desktop Entry]
Name=Zepra Browser (Beta)
GenericName=Web Browser
Comment=Independent web browser by KetiveeAI — Beta Release
Exec=zepra_browser %U
Icon=zepra-browser
Terminal=false
Type=Application
Categories=Network;WebBrowser;
MimeType=text/html;text/xml;application/xhtml+xml;x-scheme-handler/http;x-scheme-handler/https;
StartupNotify=true
Keywords=browser;web;internet;zepra;ketivee;
EOF

cp "$APPDIR/zepra-browser.desktop" "$APPDIR/usr/share/applications/"

# AppStream metadata (for software centers)
mkdir -p "$APPDIR/usr/share/metainfo"
cat > "$APPDIR/usr/share/metainfo/zepra-browser.appdata.xml" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop-application">
  <id>com.ketiveeai.zepra-browser</id>
  <metadata_license>CC0-1.0</metadata_license>
  <project_license>LicenseRef-KPL-2.0</project_license>
  <name>Zepra Browser</name>
  <summary>Independent web browser by KetiveeAI</summary>
  <description>
    <p>
      Zepra Browser is a high-performance web browser built entirely from scratch
      by KetiveeAI. It features a custom JavaScript engine (ZepraScript),
      hardware-accelerated rendering engine (NXRender), and networking stack (NXNet).
    </p>
    <p>
      This is a BETA release intended for testing and evaluation.
    </p>
  </description>
  <url type="homepage">https://zepra.ketivee.com</url>
  <url type="bugtracker">https://github.com/KetiveeAI/Zepra/issues</url>
  <releases>
    <release version="0.1.0-beta" date="2026-05-27">
      <description><p>First public beta release.</p></description>
    </release>
  </releases>
  <content_rating type="oars-1.1"/>
</component>
EOF

echo "[6/6] Building AppImage..."
mkdir -p "$OUTPUT_DIR"
OUTPUT_APPIMAGE="$OUTPUT_DIR/ZepraBrowser-${VERSION}-x86_64.AppImage"

ARCH=x86_64 "$APPIMAGETOOL" "$APPDIR" "$OUTPUT_APPIMAGE"
chmod +x "$OUTPUT_APPIMAGE"

APP_SIZE=$(du -sh "$OUTPUT_APPIMAGE" | cut -f1)

echo ""
echo -e "${GREEN}=============================================${NC}"
echo -e "${GREEN}  AppImage built successfully!${NC}"
echo -e "${GREEN}=============================================${NC}"
echo ""
echo "  File: $OUTPUT_APPIMAGE"
echo "  Size: $APP_SIZE"
echo ""
echo "  Install: Just run it — no installation needed!"
echo "    chmod +x ZepraBrowser-${VERSION}-x86_64.AppImage"
echo "    ./ZepraBrowser-${VERSION}-x86_64.AppImage"
echo ""
