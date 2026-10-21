// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file nxrender.cpp
 * @brief NXRender main framework implementation — production-ready
 */

#include "nxrender_cpp.h"
#include "platform/platform.h"
#include <memory>
#include <chrono>
#include <thread>
#include <algorithm>
#include <iostream>

namespace NXRender {

// =========================================================================
// Internal State
// =========================================================================

static std::unique_ptr<GpuContext> g_gpu;
static std::unique_ptr<Compositor> g_compositor;
static std::unique_ptr<Platform> g_platform;

static EventCallback g_eventHandler;
static std::function<void()> g_renderCallback;
static ResizeCallback g_resizeCallback;

static int g_width = 0;
static int g_height = 0;
static float g_dpiScale = 1.0f;
static bool g_running = false;
static bool g_initialized = false;
static bool g_debugOverlay = false;
static bool g_quitRequested = false;

static FrameStats g_frameStats;
static std::chrono::steady_clock::time_point g_lastFrameTime;
static constexpr int FPS_SAMPLE_COUNT = 60;
static float g_frameSamples[FPS_SAMPLE_COUNT] = {};
static int g_sampleIndex = 0;

// =========================================================================
// Internal Helpers
// =========================================================================

static void updateFrameStats() {
    auto now = std::chrono::steady_clock::now();
    float deltaMs = std::chrono::duration<float, std::milli>(now - g_lastFrameTime).count();
    g_lastFrameTime = now;

    g_frameStats.frameCount++;
    g_frameStats.frameTimeMs = deltaMs;

    if (deltaMs > 0.0f && deltaMs < 10000.0f) {
        g_frameStats.minFrameTimeMs = std::min(g_frameStats.minFrameTimeMs, deltaMs);
        g_frameStats.maxFrameTimeMs = std::max(g_frameStats.maxFrameTimeMs, deltaMs);
    }

    g_frameSamples[g_sampleIndex] = deltaMs;
    g_sampleIndex = (g_sampleIndex + 1) % FPS_SAMPLE_COUNT;

    float totalMs = 0.0f;
    int validSamples = 0;
    for (int i = 0; i < FPS_SAMPLE_COUNT; i++) {
        if (g_frameSamples[i] > 0.0f) {
            totalMs += g_frameSamples[i];
            validSamples++;
        }
    }
    if (validSamples > 0 && totalMs > 0.0f) {
        g_frameStats.avgFps = 1000.0f * validSamples / totalMs;
    }

    if (g_compositor) {
        g_frameStats.layerCount = static_cast<uint32_t>(g_compositor->layers().size());
    }
}

static void renderDebugOverlay() {
    if (!g_gpu || !g_debugOverlay) return;

    Rect bgRect(4, 4, 180, 80);
    g_gpu->fillRect(bgRect, Color(0, 0, 0, 180));

    char buf[64];

    snprintf(buf, sizeof(buf), "FPS: %.1f", g_frameStats.avgFps);
    g_gpu->drawText(buf, 10, 20, Color::white(), 13.0f);

    snprintf(buf, sizeof(buf), "Frame: %.2f ms", g_frameStats.frameTimeMs);
    g_gpu->drawText(buf, 10, 38, Color(200, 200, 200), 12.0f);

    snprintf(buf, sizeof(buf), "Layers: %u", g_frameStats.layerCount);
    g_gpu->drawText(buf, 10, 54, Color(200, 200, 200), 12.0f);

    snprintf(buf, sizeof(buf), "DPI: %.1fx", g_dpiScale);
    g_gpu->drawText(buf, 10, 70, Color(200, 200, 200), 12.0f);
}

void dispatchEvent(const Event& event) {
    if (event.type == EventType::Close) {
        g_quitRequested = true;
        g_running = false;
        return;
    }

    if (event.type == EventType::Resize) {
        int newW = event.window.width;
        int newH = event.window.height;
        if (newW > 0 && newH > 0 && (newW != g_width || newH != g_height)) {
            resize(newW, newH);
        }
        // Fall through — app handler needs resize too
    }

    // Always forward to application event handler first
    // The browser does its own hit-testing and needs ALL events
    if (g_eventHandler) {
        g_eventHandler(event);
    }

    // Then let compositor widgets handle (for NXRender's own overlay UI if any)
    if (g_compositor && (event.type == EventType::MouseDown ||
                          event.type == EventType::MouseUp ||
                          event.type == EventType::MouseMove)) {
        Widget* target = g_compositor->hitTest(event.mouse.x, event.mouse.y);
        if (target) {
            EventResult result = target->handleEvent(event);
            if (result == EventResult::NeedsRedraw && g_compositor) {
                g_compositor->invalidateAll();
            }
        }
    }
}

// =========================================================================
// Init / Shutdown
// =========================================================================

bool init(int width, int height) {
    InitOptions opts;
    opts.width = width;
    opts.height = height;
    return initWithOptions(opts);
}

bool initWithOptions(const InitOptions& options) {
    if (g_initialized) {
        shutdown();
    }

    g_width = options.width;
    g_height = options.height;
    g_dpiScale = options.dpiScale;
    g_debugOverlay = options.debugOverlay;
    g_quitRequested = false;

    // Platform window
    g_platform.reset(createPlatform());
    if (!g_platform || !g_platform->init(options.width, options.height, options.title)) {
        g_platform.reset();
        return false;
    }

    // GPU context
    g_gpu = std::make_unique<GpuContext>();
    int physW = static_cast<int>(options.width * g_dpiScale);
    int physH = static_cast<int>(options.height * g_dpiScale);
    if (!g_gpu->init(physW, physH)) {
        g_gpu.reset();
        g_platform->shutdown();
        g_platform.reset();
        return false;
    }

    // Compositor
    g_compositor = std::make_unique<Compositor>();
    if (!g_compositor->init(g_gpu.get())) {
        g_compositor.reset();
        g_gpu.reset();
        g_platform->shutdown();
        g_platform.reset();
        return false;
    }

    if (options.vsync) {
        g_compositor->setVsyncEnabled(true);
        g_platform->setVSync(true);
    }

    setTheme(Theme::light());

    g_lastFrameTime = std::chrono::steady_clock::now();
    g_frameStats = FrameStats{};
    g_sampleIndex = 0;
    for (int i = 0; i < FPS_SAMPLE_COUNT; i++) g_frameSamples[i] = 0.0f;

    g_running = true;
    g_initialized = true;
    return true;
}

bool isInitialized() {
    return g_initialized;
}

void shutdown() {
    g_running = false;
    g_initialized = false;
    g_quitRequested = false;

    g_eventHandler = nullptr;
    g_renderCallback = nullptr;
    g_resizeCallback = nullptr;

    g_compositor.reset();
    g_gpu.reset();
    if (g_platform) {
        g_platform->shutdown();
        g_platform.reset();
    }

    g_width = 0;
    g_height = 0;
    g_frameStats = FrameStats{};
}

// =========================================================================
// Viewport / DPI
// =========================================================================

void resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    g_width = width;
    g_height = height;

    int physW = static_cast<int>(width * g_dpiScale);
    int physH = static_cast<int>(height * g_dpiScale);

    if (g_gpu) {
        g_gpu->setViewport(0, 0, physW, physH);
    }

    if (g_compositor) {
        g_compositor->invalidateAll();
    }

    if (g_resizeCallback) {
        g_resizeCallback(width, height);
    }
}

int viewportWidth()  { return g_width; }
int viewportHeight() { return g_height; }

float dpiScale()     { return g_dpiScale; }

void setDpiScale(float scale) {
    if (scale < 0.25f || scale > 8.0f) return;
    g_dpiScale = scale;
    if (g_initialized) {
        resize(g_width, g_height);
    }
}

int physicalWidth()  { return static_cast<int>(g_width * g_dpiScale); }
int physicalHeight() { return static_cast<int>(g_height * g_dpiScale); }

// =========================================================================
// Accessors
// =========================================================================

Compositor* compositor() { return g_compositor.get(); }

Theme* theme() { return currentTheme(); }

GpuContext* gpuContext() { return g_gpu.get(); }

// =========================================================================
// Callbacks
// =========================================================================

void setEventHandler(EventCallback handler) {
    g_eventHandler = std::move(handler);
}

void setRenderCallback(std::function<void()> callback) {
    g_renderCallback = std::move(callback);
}

void setResizeCallback(ResizeCallback callback) {
    g_resizeCallback = std::move(callback);
}

// =========================================================================
// Frame Control
// =========================================================================

void processEvents() {
    if (g_platform) {
        g_platform->pollEvents();
    }
}

void render() {
    if (!g_compositor || !g_initialized) return;

    updateFrameStats();

    g_compositor->beginFrame();
    g_compositor->render();

    if (g_renderCallback) {
        g_renderCallback();
    }

    renderDebugOverlay();

    g_compositor->endFrame();

    if (g_platform) {
        g_platform->swapBuffers();
    }
}

void run() {
    // Without vsync, swapBuffers returns immediately and the loop would spin a
    // whole CPU core; cap it at ~60 frames per second.
    constexpr auto kFrameBudget = std::chrono::microseconds(16667);
    while (g_running && !g_quitRequested) {
        auto frameStart = std::chrono::steady_clock::now();
        processEvents();
        render();
        auto elapsed = std::chrono::steady_clock::now() - frameStart;
        if (elapsed < kFrameBudget) {
            std::this_thread::sleep_for(kFrameBudget - elapsed);
        }
    }
}

void requestQuit() {
    g_quitRequested = true;
    g_running = false;
}

void setCursor(CursorType type) {
    if (g_platform) {
        Platform::CursorType ptype;
        switch (type) {
            case CursorType::Hand: ptype = Platform::CursorType::Hand; break;
            case CursorType::Text: ptype = Platform::CursorType::Text; break;
            default: ptype = Platform::CursorType::Arrow; break;
        }
        g_platform->setCursor(ptype);
    }
}

void* nativeDisplay() {
    return g_platform ? g_platform->nativeDisplay() : nullptr;
}

unsigned long nativeWindow() {
    return g_platform ? g_platform->nativeWindow() : 0;
}

void setNativeEventHook(std::function<void(void*)> hook) {
    if (g_platform) g_platform->setNativeEventHook(std::move(hook));
}

bool shouldQuit() {
    return g_quitRequested;
}

// =========================================================================
// Debug Overlay
// =========================================================================

void setDebugOverlay(bool enabled) {
    g_debugOverlay = enabled;
}

bool debugOverlayEnabled() {
    return g_debugOverlay;
}

// =========================================================================
// Frame Stats
// =========================================================================

const FrameStats& frameStats() {
    return g_frameStats;
}

void resetFrameStats() {
    g_frameStats = FrameStats{};
    g_sampleIndex = 0;
    for (int i = 0; i < FPS_SAMPLE_COUNT; i++) g_frameSamples[i] = 0.0f;
}

} // namespace NXRender
