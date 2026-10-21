/**
 * @file nxrender_cpp.h
 * @brief NXRender - C++ Rendering Framework for ZepraBrowser
 *
 * Main header that includes all NXRender components.
 *
 * @copyright 2024-2025 KetiveeAI
 */

#pragma once

#include <functional>
#include <string>
#include <cstdint>

// Core types
#include "nxgfx/color.h"
#include "nxgfx/primitives.h"
#include "nxgfx/context.h"
#include "nxgfx/text.h"

// Compositor
#include "core/surface.h"
#include "core/layer.h"
#include "core/compositor.h"

// Widgets
#include "widgets/widget.h"
#include "widgets/button.h"
#include "widgets/label.h"
#include "widgets/textfield.h"
#include "widgets/container.h"

// Layout
#include "layout/constraints.h"
#include "layout/layout.h"
#include "layout/flexbox.h"

// Theme
#include "theme/colors.h"
#include "theme/theme.h"

// Input
#include "input/events.h"
#include "input/mouse.h"
#include "input/keyboard.h"

namespace NXRender {

// =========================================================================
// Init Options
// =========================================================================

struct InitOptions {
    int width = 1280;
    int height = 720;
    const char* title = "Zepra Browser";
    bool vsync = true;
    bool debugOverlay = false;
    float dpiScale = 1.0f;
    bool hardwareAccel = true;
};

// =========================================================================
// Frame Statistics
// =========================================================================

struct FrameStats {
    uint64_t frameCount = 0;
    float frameTimeMs = 0.0f;
    float avgFps = 0.0f;
    float minFrameTimeMs = 999.0f;
    float maxFrameTimeMs = 0.0f;
    uint32_t drawCalls = 0;
    uint32_t layerCount = 0;
    uint32_t widgetCount = 0;
};

// =========================================================================
// Framework API
// =========================================================================

/**
 * @brief Initialize NXRender with default options
 */
bool init(int width, int height);

/**
 * @brief Initialize NXRender with full options
 */
bool initWithOptions(const InitOptions& options);

/**
 * @brief Whether the framework is initialized and running
 */
bool isInitialized();

/**
 * @brief Shutdown NXRender and release all resources
 */
void shutdown();

/**
 * @brief Resize the viewport (propagates to GPU context + compositor)
 */
void resize(int width, int height);

/**
 * @brief Current viewport dimensions
 */
int viewportWidth();
int viewportHeight();

/**
 * @brief DPI scale factor (1.0 = standard, 2.0 = Retina/HiDPI)
 */
float dpiScale();
void setDpiScale(float scale);

/**
 * @brief Physical pixel dimensions (viewport * dpiScale)
 */
int physicalWidth();
int physicalHeight();

/**
 * @brief Get the main compositor
 */
Compositor* compositor();

/**
 * @brief Get the current theme
 */
Theme* theme();

/**
 * @brief Get the GPU context
 */
GpuContext* gpuContext();

// =========================================================================
// Event / Render Callbacks
// =========================================================================

using EventCallback = std::function<void(const Event&)>;
using RenderCallback = std::function<void()>;
using ResizeCallback = std::function<void(int width, int height)>;

void setEventHandler(EventCallback handler);
void setRenderCallback(RenderCallback callback);
void setResizeCallback(ResizeCallback callback);

// =========================================================================
// Frame Control
// =========================================================================

/**
 * @brief Process pending platform events
 */
void processEvents();

/**
 * @brief Render one frame (beginFrame → render → user callback → endFrame → swap)
 */
void render();

/**
 * @brief Run the main loop (blocking). Calls processEvents + render each iteration.
 */
void run();

/**
 * @brief Signal the main loop to stop
 */
void requestQuit();

/**
 * @brief Whether a quit has been requested
 */
bool shouldQuit();

/**
 * @brief Set mouse cursor type
 */
enum class CursorType { Arrow, Hand, Text };
void setCursor(CursorType type);

/**
 * @brief Native window-system handles of the main window (X11: Display*,
 *        Window), for OS integrations such as the clipboard. Null/0 elsewhere.
 */
void* nativeDisplay();
unsigned long nativeWindow();

/**
 * @brief Receive raw OS events the platform backend does not handle itself
 *        (X11: XEvent*, e.g. SelectionRequest).
 */
void setNativeEventHook(std::function<void(void*)> hook);

// =========================================================================
// Debug Overlay
// =========================================================================

void setDebugOverlay(bool enabled);
bool debugOverlayEnabled();

// =========================================================================
// Frame Stats
// =========================================================================

const FrameStats& frameStats();
void resetFrameStats();

} // namespace NXRender
