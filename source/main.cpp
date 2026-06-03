// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file main.cpp
 * @brief ZepraBrowser Entry Point - Native Rendering Only
 * 
 * ============================================================================
 *                          ⚠️  IMPORTANT NOTICE  ⚠️
 * ============================================================================
 * 
 * DO NOT USE SDL IN THIS PROJECT!
 * 
 * ZepraBrowser uses ONLY native rendering systems:
 * 
 *   ┌─────────────────────────────────────────────────────────────────────┐
 *   │  RENDERING STACK (Native - No Third-Party UI Libraries)            │
 *   ├─────────────────────────────────────────────────────────────────────┤
 *   │                                                                     │
 *   │   ┌─────────────────┐    ┌──────────────────┐                      │
 *   │   │    NXGFX        │    │    NXRender      │                      │
 *   │   │  (Rust/C++)     │    │  (Rust Widgets)  │                      │
 *   │   │  Direct OpenGL  │    │  Compositor      │                      │
 *   │   └────────┬────────┘    └────────┬─────────┘                      │
 *   │            │                      │                                 │
 *   │            ▼                      ▼                                 │
 *   │   ┌────────────────────────────────────────┐                       │
 *   │   │              OpenGL / Vulkan           │                       │
 *   │   └────────────────────────────────────────┘                       │
 *   │                        │                                            │
 *   │                        ▼                                            │
 *   │   ┌────────────────────────────────────────┐                       │
 *   │   │           X11 / Wayland / OS           │                       │
 *   │   └────────────────────────────────────────┘                       │
 *   │                                                                     │
 *   └─────────────────────────────────────────────────────────────────────┘
 * 
 * NXRender Location: source/nxrender/
 *   - nxgfx/          GPU graphics backend (Rust)
 *   - nxrender-core/  Compositor & surfaces
 *   - nxrender-widgets/ UI widgets (Button, TextField, etc.)
 *   - nxrender-layout/  Flexbox/Grid layout engine
 *   - nxrender-theme/   Theming system
 *   - nxrender-input/   Mouse/keyboard/touch handling
 *   - nxrender-animation/ Animation system
 * 
 * C++ Native Components:
 *   - include/nxsvg.h   Custom SVG parser (pure C++)
 *   - include/nxfont.h  FreeType font renderer (pure C++)
 * 
 * WHY NO SDL:
 *   1. SDL is designed for games, not browsers
 *   2. Not optimized for complex UI rendering
 *   3. External dependency we cannot fully trust/control
 *   4. Real browsers (Firefox, Chrome, Safari) build their own stack
 *   5. Performance: direct OpenGL is faster for our use case
 * 
 * ============================================================================
 */

#include <iostream>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#endif

// Forward declaration - implementation in zepra_browser.cpp
extern int zepra_main(int argc, char* argv[]);

int main(int argc, char* argv[]) {
    std::cout << "╔══════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║                    ZepraBrowser v1.0.0                   ║" << std::endl;
    std::cout << "║                                                          ║" << std::endl;
    std::cout << "║  Rendering: NXRender + NXGFX (Native, No SDL)            ║" << std::endl;
    std::cout << "║  Graphics:  OpenGL Direct                                ║" << std::endl;
    std::cout << "║  Fonts:     FreeType (nxfont.h)                          ║" << std::endl;
    std::cout << "║  Icons:     NxSVG (nxsvg.h)                              ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;
    
    // Parse arguments
    if (argc > 1) {
        std::string arg = argv[1];
        
        if (arg == "--version" || arg == "-v") {
            std::cout << "ZepraBrowser 1.0.0" << std::endl;
            std::cout << "Built with:" << std::endl;
            std::cout << "  - NXRender (source/nxrender/)" << std::endl;
            std::cout << "  - NXGFX OpenGL Backend" << std::endl;
            std::cout << "  - NxSVG Icon Loader" << std::endl;
            std::cout << "  - NxFont FreeType Renderer" << std::endl;
            std::cout << "  - NO SDL (Native Rendering Only)" << std::endl;
            return 0;
        }
        
        if (arg == "--help" || arg == "-h") {
            std::cout << "ZepraBrowser - Web Browser for NeolyxOS" << std::endl;
            std::cout << std::endl;
            std::cout << "Usage: zepra_browser [OPTIONS] [URL]" << std::endl;
            std::cout << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  -h, --help       Show this help message" << std::endl;
            std::cout << "  -v, --version    Show version information" << std::endl;
            std::cout << std::endl;
            std::cout << "Rendering Stack:" << std::endl;
            std::cout << "  NXRender:  source/nxrender/" << std::endl;
            std::cout << "  NXGFX:     source/nxrender/nxgfx/" << std::endl;
            std::cout << "  NxSVG:     include/nxsvg.h" << std::endl;
            std::cout << "  NxFont:    include/nxfont.h" << std::endl;
            std::cout << std::endl;
            std::cout << "NOTE: This browser uses ONLY native rendering." << std::endl;
            std::cout << "      SDL is NOT used. See main.cpp for details." << std::endl;
            return 0;
        }
    }
    
    // Launch browser with native NXRender stack
    std::cout << "[Init] Starting NXRender engine..." << std::endl;
    std::cout << "[Init] NXRender path: source/nxrender/" << std::endl;
    std::cout << "[Init] NXGFX path: source/nxrender/nxgfx/" << std::endl;
    std::cout << std::endl;
    
    // Note: The actual browser implementation is in zepra_browser.cpp
    // which uses native platform + OpenGL + nxsvg.h + nxfont.h
    // No SDL dependency anywhere in the rendering pipeline
    
    // Initialize Winsock on Windows (required before any socket call)
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[Fatal] WSAStartup failed" << std::endl;
        return 1;
    }
    std::cout << "[Init] Winsock initialized" << std::endl;
#endif
    
    // Actually launch the browser GUI
    int result = zepra_main(argc, argv);
    
#ifdef _WIN32
    WSACleanup();
#endif
    
    return result;
}


/*
 * ============================================================================
 *                        BUILD INSTRUCTIONS
 * ============================================================================
 * 
 * To compile ZepraBrowser with native rendering:
 * 
 *   cd apps/zeprabrowser
 *   g++ src/zepra_browser.cpp -o zepra_browser \
 *       -I./src -I./include \
 *       -lX11 -lGL -lGLU -lfreetype \
 *       -I/usr/include/freetype2 \
 *       -std=c++17
 * 
 * Dependencies (system packages, NOT SDL):
 *   - libx11-dev      (X11 window system)
 *   - libgl1-mesa-dev (OpenGL)
 *   - libglu1-mesa-dev (GLU utilities)
 *   - libfreetype-dev  (Font rendering)
 * 
 * ============================================================================
 *                        ❌ DO NOT ADD SDL ❌
 * ============================================================================
 * 
 * If you are tempted to add SDL, STOP and consider:
 *   1. Use nxsvg.h for SVG icons
 *   2. Use nxfont.h for text rendering
 *   3. Use NXRender Rust FFI for widgets
 *   4. Use X11/OpenGL directly for window/graphics
 * 
 * The goal is a fully native rendering stack like Firefox/Chrome/Safari.
 * 
 * ============================================================================
 */
