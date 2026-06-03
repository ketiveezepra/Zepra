#!/bin/bash
# =============================================================================
# Zepra Browser — Debian Package Builder
# Builds a .deb package for Debian/Ubuntu/NeolyxOS
# Copyright (c) 2025-2026 KetiveeAI. All rights reserved.
# =============================================================================

set -e

VERSION="0.1.0"
CHANNEL="beta"
FULL_VERSION="${VERSION}-${CHANNEL}"
ARCH="amd64"
PKG_NAME="zepra-browser"
PKG_DIR="${PKG_NAME}_${FULL_VERSION}_${ARCH}"
BUILD_DIR="build"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUTPUT_DIR="$ROOT_DIR/dist"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${CYAN}"
echo "============================================="
echo "  Zepra Browser — Debian Package Builder"
echo "  Version: ${FULL_VERSION}"
echo "============================================="
echo -e "${NC}"

# Check for built binary
BIN_DIR="$ROOT_DIR/$BUILD_DIR/bin"
if [ ! -f "$BIN_DIR/zepra_browser" ]; then
    BIN_DIR="$ROOT_DIR/build_ninja/bin"
fi
if [ ! -f "$BIN_DIR/zepra_browser" ]; then
    echo -e "${RED}[ERROR] zepra_browser binary not found.${NC}"
    echo -e "${YELLOW}Build first: ./build.sh release${NC}"
    exit 1
fi

# Create output directory
mkdir -p "$OUTPUT_DIR"
cd "$OUTPUT_DIR"

# Clean previous build
rm -rf "$PKG_DIR"

echo "[1/6] Creating package structure..."

# Create directory hierarchy
mkdir -p "$PKG_DIR/DEBIAN"
mkdir -p "$PKG_DIR/opt/ketiveeai/zepra-browser/bin"
mkdir -p "$PKG_DIR/opt/ketiveeai/zepra-browser/resources/icons"
mkdir -p "$PKG_DIR/opt/ketiveeai/zepra-browser/resources/web"
mkdir -p "$PKG_DIR/opt/ketiveeai/zepra-browser/legal"
mkdir -p "$PKG_DIR/usr/bin"
mkdir -p "$PKG_DIR/usr/share/applications"
mkdir -p "$PKG_DIR/usr/share/icons/hicolor/scalable/apps"
mkdir -p "$PKG_DIR/usr/share/doc/zepra-browser"

echo "[2/6] Writing package metadata..."

# DEBIAN/control
cat > "$PKG_DIR/DEBIAN/control" <<EOF
Package: zepra-browser
Version: ${FULL_VERSION}
Section: web
Priority: optional
Architecture: ${ARCH}
Depends: libfreetype6 (>= 2.10), libx11-6, libgl1, zlib1g, libssl3 | libssl1.1
Recommends: fonts-noto, xdg-utils
Suggests: zenity
Installed-Size: $(du -sk "$BIN_DIR/zepra_browser" 2>/dev/null | cut -f1 || echo "200000")
Maintainer: KetiveeAI <support@ketivee.com>
Homepage: https://zepra.ketivee.com
Description: Zepra Browser — Independent Web Browser (Beta)
 Zepra Browser is a high-performance web browser built from scratch
 by KetiveeAI. It features a custom JavaScript engine (ZepraScript),
 custom rendering engine (NXRender), and custom networking stack (NXNet).
 .
 This is a BETA release intended for testing and evaluation.
 .
 Built in India. Independent by design.
EOF

# DEBIAN/postinst
cat > "$PKG_DIR/DEBIAN/postinst" <<'EOF'
#!/bin/bash
set -e

# Update icon cache
if command -v gtk-update-icon-cache &>/dev/null; then
    gtk-update-icon-cache -f -t /usr/share/icons/hicolor 2>/dev/null || true
fi

# Update desktop database
if command -v update-desktop-database &>/dev/null; then
    update-desktop-database /usr/share/applications 2>/dev/null || true
fi

echo ""
echo "============================================="
echo "  Zepra Browser (Beta) installed!"
echo "============================================="
echo ""
echo "  Launch from your application menu or run:"
echo "  $ zepra-browser"
echo ""
echo "  WARNING: This is BETA software."
echo "  Report bugs: github.com/KetiveeAI/Zepra/issues"
echo ""

exit 0
EOF
chmod 755 "$PKG_DIR/DEBIAN/postinst"

# DEBIAN/prerm
cat > "$PKG_DIR/DEBIAN/prerm" <<'EOF'
#!/bin/bash
set -e

# Kill running instances
pkill -f zepra_browser 2>/dev/null || true

exit 0
EOF
chmod 755 "$PKG_DIR/DEBIAN/prerm"

# DEBIAN/postrm
cat > "$PKG_DIR/DEBIAN/postrm" <<'EOF'
#!/bin/bash
set -e

# Update caches
if command -v gtk-update-icon-cache &>/dev/null; then
    gtk-update-icon-cache -f -t /usr/share/icons/hicolor 2>/dev/null || true
fi
if command -v update-desktop-database &>/dev/null; then
    update-desktop-database /usr/share/applications 2>/dev/null || true
fi

# Remove empty install directory
rmdir /opt/ketiveeai/zepra-browser 2>/dev/null || true
rmdir /opt/ketiveeai 2>/dev/null || true

exit 0
EOF
chmod 755 "$PKG_DIR/DEBIAN/postrm"

echo "[3/6] Copying binaries..."

# Copy executables
cp "$BIN_DIR/zepra_browser" "$PKG_DIR/opt/ketiveeai/zepra-browser/bin/"
chmod 755 "$PKG_DIR/opt/ketiveeai/zepra-browser/bin/zepra_browser"

for tool in zepra-repl zepra-dump-bytecode; do
    if [ -f "$BIN_DIR/$tool" ]; then
        cp "$BIN_DIR/$tool" "$PKG_DIR/opt/ketiveeai/zepra-browser/bin/"
        chmod 755 "$PKG_DIR/opt/ketiveeai/zepra-browser/bin/$tool"
    fi
done

# Create /usr/bin symlink
ln -sf /opt/ketiveeai/zepra-browser/bin/zepra_browser "$PKG_DIR/usr/bin/zepra-browser"

echo "[4/6] Copying resources..."

# Copy resources
if [ -d "$ROOT_DIR/resources/icons" ]; then
    cp -r "$ROOT_DIR/resources/icons/"* "$PKG_DIR/opt/ketiveeai/zepra-browser/resources/icons/" 2>/dev/null || true
fi
if [ -d "$ROOT_DIR/resources/web" ]; then
    cp -r "$ROOT_DIR/resources/web/"* "$PKG_DIR/opt/ketiveeai/zepra-browser/resources/web/" 2>/dev/null || true
fi

# Copy app icon for system
if [ -f "$ROOT_DIR/resources/icons/zepra.svg" ]; then
    cp "$ROOT_DIR/resources/icons/zepra.svg" \
       "$PKG_DIR/usr/share/icons/hicolor/scalable/apps/zepra-browser.svg"
fi

echo "[5/6] Copying legal and documentation..."

# Copy legal
if [ -d "$ROOT_DIR/legal" ]; then
    cp "$ROOT_DIR/legal/"* "$PKG_DIR/opt/ketiveeai/zepra-browser/legal/" 2>/dev/null || true
fi
[ -f "$ROOT_DIR/LICENSE.md" ] && cp "$ROOT_DIR/LICENSE.md" "$PKG_DIR/opt/ketiveeai/zepra-browser/legal/"

# Copy docs
[ -f "$ROOT_DIR/RELEASE_NOTES.md" ] && cp "$ROOT_DIR/RELEASE_NOTES.md" "$PKG_DIR/usr/share/doc/zepra-browser/"
[ -f "$ROOT_DIR/README.md" ] && cp "$ROOT_DIR/README.md" "$PKG_DIR/usr/share/doc/zepra-browser/"

# Copyright file (required for .deb)
cat > "$PKG_DIR/usr/share/doc/zepra-browser/copyright" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Zepra Browser
Upstream-Contact: licence@ketivee.com
Source: https://github.com/KetiveeAI/Zepra

Files: *
Copyright: 2025-2026 KetiveeAI
License: KPL-2.0
 KetiveeAI Public License Version 2.0
 See /opt/ketiveeai/zepra-browser/legal/LICENSE.md for full text.
EOF

# Desktop entry
cp "$SCRIPT_DIR/zepra-browser.desktop" "$PKG_DIR/usr/share/applications/" 2>/dev/null || \
cat > "$PKG_DIR/usr/share/applications/zepra-browser.desktop" <<EOF
[Desktop Entry]
Name=Zepra Browser (Beta)
Comment=Independent web browser by KetiveeAI — Beta Release
Exec=/opt/ketiveeai/zepra-browser/bin/zepra_browser %U
Icon=zepra-browser
Terminal=false
Type=Application
Categories=Network;WebBrowser;
MimeType=text/html;text/xml;application/xhtml+xml;x-scheme-handler/http;x-scheme-handler/https;
StartupNotify=true
StartupWMClass=zepra-browser
Keywords=browser;web;internet;zepra;ketivee;
EOF

echo "[6/6] Building .deb package..."

# Build the .deb
fakeroot dpkg-deb --build "$PKG_DIR" 2>/dev/null || dpkg-deb --build "$PKG_DIR"

DEB_FILE="${PKG_DIR}.deb"
DEB_SIZE=$(du -sh "$DEB_FILE" | cut -f1)

echo ""
echo -e "${GREEN}=============================================${NC}"
echo -e "${GREEN}  Package built successfully!${NC}"
echo -e "${GREEN}=============================================${NC}"
echo ""
echo "  Package: $OUTPUT_DIR/$DEB_FILE"
echo "  Size:    $DEB_SIZE"
echo ""
echo "  Install with:"
echo "    sudo dpkg -i $DEB_FILE"
echo "    sudo apt-get install -f  # fix dependencies"
echo ""
echo "  Or:"
echo "    sudo apt install ./$DEB_FILE"
echo ""
