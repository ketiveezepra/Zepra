#!/bin/bash
# =============================================================================
# Zepra Browser — Interactive Installer for Linux
# Supports both GUI (zenity/yad) and terminal modes.
# Copyright (c) 2025-2026 KetiveeAI. All rights reserved.
# =============================================================================

set -e

VERSION="0.1.0-beta"
APP_NAME="Zepra Browser"
INSTALL_DIR="/opt/ketiveeai/zepra-browser"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# Detect GUI availability
HAS_ZENITY=false
HAS_YAD=false
USE_GUI=false

if command -v zenity &>/dev/null; then
    HAS_ZENITY=true
fi
if command -v yad &>/dev/null; then
    HAS_YAD=true
fi

# Use GUI if we have a display and a dialog tool
if [ -n "$DISPLAY" ] || [ -n "$WAYLAND_DISPLAY" ]; then
    if $HAS_ZENITY || $HAS_YAD; then
        USE_GUI=true
    fi
fi

# Allow force terminal mode
if [ "$1" = "--no-gui" ] || [ "$1" = "--terminal" ]; then
    USE_GUI=false
fi

# ============================================================================
# EULA Text
# ============================================================================
EULA_TEXT="KETIVEEAI - END USER LICENSE AGREEMENT (EULA)
Zepra Browser v${VERSION}

================================================================
BETA SOFTWARE NOTICE
================================================================

This is a PRE-RELEASE BETA VERSION of Zepra Browser. It is
provided for testing and evaluation purposes. This software
may contain bugs, incomplete features, and may not perform
as expected. Do not rely on this software for critical tasks.

================================================================

1. ACCEPTANCE OF TERMS

By installing Zepra Browser, you agree to be bound by the terms
of this Agreement. If you do not agree, do not install.

2. LICENSE GRANT

KetiveeAI grants you a limited, non-exclusive, revocable license
to install and use the Software under KPL-2.0.

3. BETA DISCLAIMER

THIS IS BETA SOFTWARE. The Software may contain errors, defects,
and vulnerabilities. It should not be used for sensitive activities.

4. PRIVACY

Anonymous crash reports may be collected. NO browsing history,
passwords, or tracking data is collected.

5. WARRANTY DISCLAIMER

THE SOFTWARE IS PROVIDED \"AS IS\" WITHOUT WARRANTY OF ANY KIND.

6. GOVERNING LAW

This Agreement is governed by the laws of India.

================================================================
Copyright (c) 2025-2026 KetiveeAI. All rights reserved.
Built in India. Independent by design."

# ============================================================================
# Root check
# ============================================================================
check_root() {
    if [ "$EUID" -ne 0 ]; then
        echo -e "${RED}[!] This installer requires root privileges.${NC}"
        echo -e "${YELLOW}    Run with: sudo $0${NC}"
        if $USE_GUI && $HAS_ZENITY; then
            zenity --error --title="$APP_NAME Installer" \
                --text="This installer requires root privileges.\n\nRun with:\nsudo $0" \
                --width=400 2>/dev/null
        fi
        exit 1
    fi
}

# ============================================================================
# GUI Installer (zenity)
# ============================================================================
gui_install() {
    # Welcome + Beta Warning
    zenity --info --title="$APP_NAME v$VERSION" \
        --text="<b>Welcome to $APP_NAME Installer</b>\n\nVersion: $VERSION\n\n<span foreground='#FF6B1A'><b>⚠ BETA SOFTWARE</b></span>\nThis is a pre-release version intended for testing.\nIt may contain bugs and incomplete features.\n\n<small>Copyright © 2025-2026 KetiveeAI\nBuilt in India. Independent by design.</small>" \
        --width=480 --ok-label="Continue" 2>/dev/null

    # EULA
    echo "$EULA_TEXT" | zenity --text-info \
        --title="End User License Agreement" \
        --width=600 --height=450 \
        --checkbox="I have read and accept the EULA" 2>/dev/null

    if [ $? -ne 0 ]; then
        zenity --warning --title="Installation Cancelled" \
            --text="You must accept the EULA to install $APP_NAME." \
            --width=350 2>/dev/null
        exit 0
    fi

    # Install with progress
    (
        echo "5"
        echo "# Creating directories..."
        mkdir -p "$INSTALL_DIR/bin"
        mkdir -p "$INSTALL_DIR/resources/icons"
        mkdir -p "$INSTALL_DIR/resources/web"
        mkdir -p "$INSTALL_DIR/legal"
        sleep 0.3

        echo "15"
        echo "# Copying Zepra Browser executable..."
        if [ -f "$SCRIPT_DIR/bin/zepra_browser" ]; then
            cp "$SCRIPT_DIR/bin/zepra_browser" "$INSTALL_DIR/bin/"
        elif [ -f "$SCRIPT_DIR/zepra_browser" ]; then
            cp "$SCRIPT_DIR/zepra_browser" "$INSTALL_DIR/bin/"
        fi
        chmod +x "$INSTALL_DIR/bin/zepra_browser" 2>/dev/null || true
        sleep 0.3

        echo "30"
        echo "# Copying support tools..."
        for tool in zepra-repl zepra-dump-bytecode; do
            for dir in "$SCRIPT_DIR/bin" "$SCRIPT_DIR"; do
                if [ -f "$dir/$tool" ]; then
                    cp "$dir/$tool" "$INSTALL_DIR/bin/"
                    chmod +x "$INSTALL_DIR/bin/$tool"
                    break
                fi
            done
        done
        sleep 0.2

        echo "45"
        echo "# Copying resources..."
        [ -d "$SCRIPT_DIR/resources" ] && cp -r "$SCRIPT_DIR/resources/"* "$INSTALL_DIR/resources/" 2>/dev/null || true
        sleep 0.2

        echo "55"
        echo "# Copying legal documents..."
        [ -d "$SCRIPT_DIR/legal" ] && cp -r "$SCRIPT_DIR/legal/"* "$INSTALL_DIR/legal/" 2>/dev/null || true
        [ -f "$SCRIPT_DIR/LICENSE.md" ] && cp "$SCRIPT_DIR/LICENSE.md" "$INSTALL_DIR/legal/" || true
        [ -f "$SCRIPT_DIR/RELEASE_NOTES.md" ] && cp "$SCRIPT_DIR/RELEASE_NOTES.md" "$INSTALL_DIR/" || true
        sleep 0.2

        echo "65"
        echo "# Creating system symlink..."
        ln -sf "$INSTALL_DIR/bin/zepra_browser" /usr/bin/zepra-browser 2>/dev/null || true
        sleep 0.2

        echo "75"
        echo "# Installing desktop entry..."
        if [ -f "$SCRIPT_DIR/zepra-browser.desktop" ]; then
            cp "$SCRIPT_DIR/zepra-browser.desktop" /usr/share/applications/
        fi
        if [ -f "$SCRIPT_DIR/resources/icons/zepra.svg" ]; then
            mkdir -p /usr/share/icons/hicolor/scalable/apps
            cp "$SCRIPT_DIR/resources/icons/zepra.svg" \
               /usr/share/icons/hicolor/scalable/apps/zepra-browser.svg
        fi
        sleep 0.2

        echo "85"
        echo "# Creating uninstall script..."
        cat > "$INSTALL_DIR/uninstall.sh" <<'UNINSTALL'
#!/bin/bash
echo "Uninstalling Zepra Browser..."
rm -f /usr/bin/zepra-browser
rm -f /usr/share/applications/zepra-browser.desktop
rm -f /usr/share/icons/hicolor/scalable/apps/zepra-browser.svg
rm -rf /opt/ketiveeai/zepra-browser
rmdir /opt/ketiveeai 2>/dev/null || true
echo "Zepra Browser has been uninstalled."
UNINSTALL
        chmod +x "$INSTALL_DIR/uninstall.sh"
        sleep 0.2

        echo "95"
        echo "# Updating system caches..."
        gtk-update-icon-cache -f -t /usr/share/icons/hicolor 2>/dev/null || true
        update-desktop-database /usr/share/applications 2>/dev/null || true
        sleep 0.2

        echo "100"
        echo "# Installation complete!"
    ) | zenity --progress --title="Installing $APP_NAME" \
        --text="Preparing installation..." \
        --percentage=0 --auto-close --width=450 2>/dev/null

    # Done
    zenity --info --title="Installation Complete" \
        --text="<b>$APP_NAME v$VERSION</b> has been installed!\n\nLaunch from your application menu or run:\n<tt>zepra-browser</tt>\n\n<span foreground='#FF6B1A'>⚠ This is BETA software — please report bugs!</span>\n<small>github.com/KetiveeAI/Zepra/issues</small>" \
        --width=420 --ok-label="Done" 2>/dev/null
}

# ============================================================================
# Terminal Installer
# ============================================================================
terminal_install() {
    echo -e "${CYAN}"
    echo "============================================="
    echo "  $APP_NAME v$VERSION"
    echo "  Interactive Installer"
    echo "============================================="
    echo -e "${NC}"
    echo -e "${YELLOW}⚠  BETA SOFTWARE — For testing purposes only${NC}"
    echo ""

    # Show EULA
    echo -e "${BOLD}End User License Agreement:${NC}"
    echo "---------------------------------------------"
    echo "$EULA_TEXT" | head -30
    echo "..."
    echo "---------------------------------------------"
    echo ""

    # Accept EULA
    read -rp "Do you accept the EULA? (yes/no): " ACCEPT
    if [ "$ACCEPT" != "yes" ] && [ "$ACCEPT" != "y" ]; then
        echo -e "${RED}EULA not accepted. Installation cancelled.${NC}"
        exit 0
    fi

    echo ""
    echo -e "${GREEN}EULA accepted. Starting installation...${NC}"
    echo ""

    # Install
    echo -e "[1/7] Creating directories..."
    mkdir -p "$INSTALL_DIR/bin"
    mkdir -p "$INSTALL_DIR/resources/icons"
    mkdir -p "$INSTALL_DIR/resources/web"
    mkdir -p "$INSTALL_DIR/legal"

    echo -e "[2/7] Copying executable..."
    if [ -f "$SCRIPT_DIR/bin/zepra_browser" ]; then
        cp "$SCRIPT_DIR/bin/zepra_browser" "$INSTALL_DIR/bin/"
    elif [ -f "$SCRIPT_DIR/zepra_browser" ]; then
        cp "$SCRIPT_DIR/zepra_browser" "$INSTALL_DIR/bin/"
    else
        echo -e "${YELLOW}       Warning: zepra_browser binary not found${NC}"
    fi
    chmod +x "$INSTALL_DIR/bin/zepra_browser" 2>/dev/null || true

    echo -e "[3/7] Copying resources..."
    [ -d "$SCRIPT_DIR/resources" ] && cp -r "$SCRIPT_DIR/resources/"* "$INSTALL_DIR/resources/" 2>/dev/null || true

    echo -e "[4/7] Copying legal documents..."
    [ -d "$SCRIPT_DIR/legal" ] && cp -r "$SCRIPT_DIR/legal/"* "$INSTALL_DIR/legal/" 2>/dev/null || true
    [ -f "$SCRIPT_DIR/LICENSE.md" ] && cp "$SCRIPT_DIR/LICENSE.md" "$INSTALL_DIR/legal/" || true
    [ -f "$SCRIPT_DIR/RELEASE_NOTES.md" ] && cp "$SCRIPT_DIR/RELEASE_NOTES.md" "$INSTALL_DIR/" || true

    echo -e "[5/7] Creating symlink..."
    ln -sf "$INSTALL_DIR/bin/zepra_browser" /usr/bin/zepra-browser 2>/dev/null || true

    echo -e "[6/7] Installing desktop entry..."
    if [ -f "$SCRIPT_DIR/zepra-browser.desktop" ]; then
        cp "$SCRIPT_DIR/zepra-browser.desktop" /usr/share/applications/ 2>/dev/null || true
    fi
    if [ -f "$SCRIPT_DIR/resources/icons/zepra.svg" ] || [ -f "$INSTALL_DIR/resources/icons/zepra.svg" ]; then
        mkdir -p /usr/share/icons/hicolor/scalable/apps
        cp "$INSTALL_DIR/resources/icons/zepra.svg" \
           /usr/share/icons/hicolor/scalable/apps/zepra-browser.svg 2>/dev/null || true
    fi

    echo -e "[7/7] Creating uninstall script..."
    cat > "$INSTALL_DIR/uninstall.sh" <<'UNINSTALL'
#!/bin/bash
echo "Uninstalling Zepra Browser..."
rm -f /usr/bin/zepra-browser
rm -f /usr/share/applications/zepra-browser.desktop
rm -f /usr/share/icons/hicolor/scalable/apps/zepra-browser.svg
rm -rf /opt/ketiveeai/zepra-browser
rmdir /opt/ketiveeai 2>/dev/null || true
echo "Zepra Browser has been uninstalled."
UNINSTALL
    chmod +x "$INSTALL_DIR/uninstall.sh"

    echo ""
    echo -e "${GREEN}=============================================${NC}"
    echo -e "${GREEN}  Installation Complete!${NC}"
    echo -e "${GREEN}=============================================${NC}"
    echo ""
    echo "  Installed to: $INSTALL_DIR"
    echo "  Run with:     zepra-browser"
    echo "  Uninstall:    sudo $INSTALL_DIR/uninstall.sh"
    echo ""
    echo -e "${YELLOW}  ⚠ This is BETA software — please report bugs!${NC}"
    echo "  https://github.com/KetiveeAI/Zepra/issues"
    echo ""
}

# ============================================================================
# Main
# ============================================================================
check_root

if $USE_GUI; then
    gui_install
else
    terminal_install
fi

exit 0
