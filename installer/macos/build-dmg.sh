#!/bin/bash
# =============================================================================
# Zepra Browser — macOS .dmg Builder
# Creates a distributable disk image with app bundle and EULA.
# Copyright (c) 2025-2026 KetiveeAI. All rights reserved.
# =============================================================================

set -e

VERSION="0.1.0"
CHANNEL="beta"
FULL_VERSION="${VERSION}-${CHANNEL}"
APP_NAME="Zepra Browser"
BUNDLE_NAME="ZepraBrowser"
BUNDLE_ID="com.ketiveeai.zepra-browser"
DMG_NAME="ZepraBrowser-${FULL_VERSION}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/bin"
OUTPUT_DIR="$ROOT_DIR/dist"
STAGE_DIR="$OUTPUT_DIR/dmg-stage"
APP_DIR="$STAGE_DIR/${BUNDLE_NAME}.app"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${CYAN}"
echo "============================================="
echo "  Zepra Browser — macOS DMG Builder"
echo "  Version: ${FULL_VERSION}"
echo "============================================="
echo -e "${NC}"

# Check we're on macOS
if [ "$(uname)" != "Darwin" ]; then
    echo -e "${RED}[ERROR] This script must be run on macOS.${NC}"
    exit 1
fi

# Verify binary exists
if [ ! -f "$BUILD_DIR/zepra_browser" ]; then
    echo -e "${RED}[ERROR] zepra_browser binary not found in $BUILD_DIR${NC}"
    echo "Build the project first."
    exit 1
fi

# Clean previous
rm -rf "$STAGE_DIR"
mkdir -p "$OUTPUT_DIR"

echo "[1/6] Creating app bundle structure..."

# Create .app bundle
mkdir -p "$APP_DIR/Contents/MacOS"
mkdir -p "$APP_DIR/Contents/Resources"
mkdir -p "$APP_DIR/Contents/Resources/icons"
mkdir -p "$APP_DIR/Contents/Resources/web"
mkdir -p "$APP_DIR/Contents/Resources/legal"

echo "[2/6] Copying Info.plist..."

cp "$SCRIPT_DIR/Info.plist" "$APP_DIR/Contents/"

echo "[3/6] Copying executable..."

cp "$BUILD_DIR/zepra_browser" "$APP_DIR/Contents/MacOS/${BUNDLE_NAME}"
chmod +x "$APP_DIR/Contents/MacOS/${BUNDLE_NAME}"

# Copy support tools
for tool in zepra-repl zepra-dump-bytecode; do
    if [ -f "$BUILD_DIR/$tool" ]; then
        cp "$BUILD_DIR/$tool" "$APP_DIR/Contents/MacOS/"
        chmod +x "$APP_DIR/Contents/MacOS/$tool"
    fi
done

echo "[4/6] Copying resources..."

# Copy resources
if [ -d "$ROOT_DIR/resources/icons" ]; then
    cp -r "$ROOT_DIR/resources/icons/"* "$APP_DIR/Contents/Resources/icons/" 2>/dev/null || true
fi
if [ -d "$ROOT_DIR/resources/web" ]; then
    cp -r "$ROOT_DIR/resources/web/"* "$APP_DIR/Contents/Resources/web/" 2>/dev/null || true
fi

# Copy legal
if [ -d "$ROOT_DIR/legal" ]; then
    cp "$ROOT_DIR/legal/"* "$APP_DIR/Contents/Resources/legal/" 2>/dev/null || true
fi
[ -f "$ROOT_DIR/LICENSE.md" ] && cp "$ROOT_DIR/LICENSE.md" "$APP_DIR/Contents/Resources/legal/"
[ -f "$ROOT_DIR/RELEASE_NOTES.md" ] && cp "$ROOT_DIR/RELEASE_NOTES.md" "$APP_DIR/Contents/Resources/"

# Create PkgInfo
echo "APPL????" > "$APP_DIR/Contents/PkgInfo"

echo "[5/6] Creating DMG layout..."

# Create Applications symlink for drag-install
ln -sf /Applications "$STAGE_DIR/Applications"

# Create a README in the DMG
cat > "$STAGE_DIR/README.txt" <<EOF
Zepra Browser v${FULL_VERSION}
==============================

BETA SOFTWARE — For testing purposes only.

To install:
  Drag "ZepraBrowser" to the "Applications" folder.

To uninstall:
  Drag "ZepraBrowser" from Applications to Trash.

Report bugs:
  https://github.com/KetiveeAI/Zepra/issues

Copyright (c) 2025-2026 KetiveeAI
Licensed under KPL-2.0
Built in India. Independent by design.
EOF

echo "[6/6] Building DMG..."

DMG_PATH="$OUTPUT_DIR/${DMG_NAME}.dmg"
rm -f "$DMG_PATH"

# Create DMG with EULA
hdiutil create -volname "$APP_NAME (Beta)" \
    -srcfolder "$STAGE_DIR" \
    -ov -format UDZO \
    -imagekey zlib-level=9 \
    "$DMG_PATH"

DMG_SIZE=$(du -sh "$DMG_PATH" | cut -f1)

echo ""
echo -e "${GREEN}=============================================${NC}"
echo -e "${GREEN}  DMG created successfully!${NC}"
echo -e "${GREEN}=============================================${NC}"
echo ""
echo "  DMG:  $DMG_PATH"
echo "  Size: $DMG_SIZE"
echo ""

# Optional: Code signing
if [ -n "$DEVELOPER_ID" ]; then
    echo "Signing with: $DEVELOPER_ID"
    codesign --deep --force --sign "$DEVELOPER_ID" "$APP_DIR"
    codesign --verify "$APP_DIR"
    echo "App bundle signed."
else
    echo "  Note: App is unsigned. Set DEVELOPER_ID env var to sign."
fi

# Cleanup
rm -rf "$STAGE_DIR"

echo ""
echo "Done."
