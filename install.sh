#!/bin/bash
# =============================================================================
# ZepraBrowser Installation Script for NeolyxOS
# Installs to /Applications/ZepraBrowser/
# =============================================================================

set -e

INSTALL_DIR="/Applications/ZepraBrowser"
BUILD_DIR="$(dirname "$0")/build/bin"
RESOURCES_DIR="$(dirname "$0")/resources"

echo "==================================="
echo "ZepraBrowser Installer for NeolyxOS"
echo "==================================="
echo ""

# Check if running with proper permissions
if [ ! -w /Applications ] && [ "$EUID" -ne 0 ]; then
    echo "[!] Need write access to /Applications/"
    echo "    Run with: sudo ./install.sh"
    exit 1
fi

# Check if build exists
if [ ! -f "$BUILD_DIR/zepra_browser" ]; then
    echo "[!] Build not found. Run 'cmake --build build' first."
    exit 1
fi

echo "[1/5] Creating app directory structure..."
mkdir -p "$INSTALL_DIR"
mkdir -p "$INSTALL_DIR/Resources/web"
mkdir -p "$INSTALL_DIR/Resources/icons"
mkdir -p "$INSTALL_DIR/Resources/fonts"
mkdir -p "$INSTALL_DIR/Resources/locales"
mkdir -p "$INSTALL_DIR/lib"

echo "[2/5] Installing executable..."
cp "$BUILD_DIR/zepra_browser" "$INSTALL_DIR/ZepraBrowser"
chmod +x "$INSTALL_DIR/ZepraBrowser"

echo "[3/5] Installing Info.nxpt..."
cp "$(dirname "$0")/Info.nxpt" "$INSTALL_DIR/"

echo "[4/5] Installing resources..."
# Copy web resources (SVG icons, internal pages)
if [ -d "$RESOURCES_DIR/web" ]; then
    cp -r "$RESOURCES_DIR/web/"* "$INSTALL_DIR/Resources/web/" 2>/dev/null || true
fi

# Copy icons
if [ -d "$RESOURCES_DIR/icons" ]; then
    cp -r "$RESOURCES_DIR/icons/"* "$INSTALL_DIR/Resources/icons/" 2>/dev/null || true
fi

# Copy main app icon
if [ -f "$RESOURCES_DIR/icon.png" ]; then
    cp "$RESOURCES_DIR/icon.png" "$INSTALL_DIR/Resources/"
fi

# Copy fonts
if [ -d "$RESOURCES_DIR/fonts" ]; then
    cp -r "$RESOURCES_DIR/fonts/"* "$INSTALL_DIR/Resources/fonts/" 2>/dev/null || true
fi

echo "[5/5] Installing libraries..."
# Copy shared libraries if any
if [ -d "$BUILD_DIR/../lib" ]; then
    cp "$BUILD_DIR/../lib/"*.so "$INSTALL_DIR/lib/" 2>/dev/null || true
fi

echo ""
echo "==================================="
echo "Installation Complete!"
echo "==================================="
echo ""
echo "Installed to: $INSTALL_DIR"
echo ""
echo "Files installed:"
ls -la "$INSTALL_DIR/"
echo ""
echo "Run with: $INSTALL_DIR/ZepraBrowser"
echo ""

# Create symlink in /Library/Bin for PATH access
if [ -w /Library/Bin ] || [ "$EUID" -eq 0 ]; then
    ln -sf "$INSTALL_DIR/ZepraBrowser" /Library/Bin/zepra 2>/dev/null || true
    echo "Symlink created: /Library/Bin/zepra"
fi

exit 0
