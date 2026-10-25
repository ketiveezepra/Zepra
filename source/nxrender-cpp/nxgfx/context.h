// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file context.h
 * @brief GPU rendering context
 */

#pragma once

#include "color.h"
#include "primitives.h"
#include <string>
#include <memory>
#include <functional>
#include <vector>
#include <cstdint>

namespace NXRender {

/**
 * @brief Blend mode for drawing operations
 */
enum class BlendMode {
    Normal,      // Standard alpha blending
    Multiply,    // Darken
    Screen,      // Lighten
    Overlay,     // Contrast
    Add,         // Additive
    NoneBlend    // No blending (replace) - named NoneBlend due to X11 conflict
};

/**
 * @brief GPU Context - handles all drawing operations
 */
class GpuContext {
public:
    GpuContext();
    ~GpuContext();
    
    // Initialize with window dimensions
    bool init(int width, int height);
    void shutdown();
    
    // Frame control
    void beginFrame();
    void endFrame();
    void present();

    /// Copy the current frame (the buffer being drawn) as RGB bytes,
    /// bottom row first; false when nothing could be read
    bool readPixels(int x, int y, int width, int height, std::vector<uint8_t>& rgb);
    
    // Viewport
    void setViewport(int x, int y, int width, int height);
    int width() const { return width_; }
    int height() const { return height_; }
    
    // Clipping
    void pushClip(const Rect& rect);
    void popClip();
    
    // Transform (simple 2D)
    void pushTransform();
    void popTransform();
    void translate(float x, float y);
    void scale(float sx, float sy);
    void rotate(float radians);
    
    // Blend mode
    void setBlendMode(BlendMode mode);
    
    // Clear
    void clear(const Color& color);
    
    // ==========================================================================
    // Drawing Primitives
    // ==========================================================================
    
    // Rectangles
    void fillRect(const Rect& rect, const Color& color);
    void strokeRect(const Rect& rect, const Color& color, float lineWidth = 1.0f);
    void fillRoundedRect(const Rect& rect, const Color& color, float radius);
    void fillRoundedRect(const Rect& rect, const Color& color, const CornerRadii& radii);
    void strokeRoundedRect(const Rect& rect, const Color& color, float radius, float lineWidth = 1.0f);
    
    // Circles
    void fillCircle(float cx, float cy, float radius, const Color& color);
    void strokeCircle(float cx, float cy, float radius, const Color& color, float lineWidth = 1.0f);
    
    // Lines
    void drawLine(float x1, float y1, float x2, float y2, const Color& color, float lineWidth = 1.0f);
    
    // Gradients
    void fillRectGradient(const Rect& rect, const Color& startColor, const Color& endColor, bool horizontal = true);
    
    // Shadows
    void drawShadow(const Rect& rect, const Color& color, float blur, float offsetX = 0, float offsetY = 0);
    
    // ==========================================================================
    // Paths
    // ==========================================================================
    
    // Simple convex path (fan)
    void fillPath(const std::vector<Point>& points, const Color& color);
    void strokePath(const std::vector<Point>& points, const Color& color, float lineWidth = 1.0f, bool closed = false);

    // Complex path (contours, holes, even-odd rule) using Stencil Buffer
    // contours: vector of closed loops
    // rule: "nonzero" or "evenodd"
    void fillComplexPath(const std::vector<std::vector<Point>>& contours, const Color& color, const std::string& rule = "nonzero");

    // ==========================================================================
    // Text
    // ==========================================================================
    
    void drawText(const std::string& text, float x, float y, const Color& color, float fontSize = 14.0f);
    Size measureText(const std::string& text, float fontSize = 14.0f);
    void setFont(const std::string& fontFamily);
    
    // ==========================================================================
    // Images
    // ==========================================================================
    
    using TextureId = uint32_t;
    TextureId loadTexture(const std::string& path);
    TextureId createTexture(int width, int height, const uint8_t* pixels);
    /// Replaces the contents (and size) of a texture from createTexture with
    /// RGBA pixels; edges clamp, so a scaled texture doesn't wrap
    void updateTexture(TextureId texture, int width, int height, const uint8_t* pixels);
    void drawTexture(TextureId texture, const Rect& dest);
    void drawTexture(TextureId texture, const Rect& src, const Rect& dest);
    void destroyTexture(TextureId texture);
    
    // Render Targets
    TextureId createRenderTarget(int width, int height);
    void setRenderTarget(TextureId target);
    void destroyRenderTarget(TextureId target);
    
private:
    int width_ = 0;
    int height_ = 0;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Global GPU context accessor
GpuContext* gpu();

} // namespace NXRender
