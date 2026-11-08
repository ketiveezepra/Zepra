// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file nxrender_layout_bridge.cpp
 * @brief Implementation of NXRender layout bridge
 */

#include "nxrender_layout_bridge.h"
#include <algorithm>
#include "layout_engine.h"

// NXRender C++ headers
#include <nxgfx/context.h>
#include <nxgfx/color.h>
#include <nxgfx/primitives.h>

namespace ZepraBrowser {

// Static member initialization
NXRender::GpuContext* NXRenderLayoutBridge::gpu_ = nullptr;
NXRenderLayoutBridge::LinkCallback NXRenderLayoutBridge::linkCallback_;
bool NXRenderLayoutBridge::initialized_ = false;

void NXRenderLayoutBridge::initialize(NXRender::GpuContext* gpu) {
    gpu_ = gpu;
    initialized_ = (gpu != nullptr);
}

void NXRenderLayoutBridge::shutdown() {
    gpu_ = nullptr;
    linkCallback_ = nullptr;
    initialized_ = false;
}

bool NXRenderLayoutBridge::isInitialized() {
    return initialized_;
}

void NXRenderLayoutBridge::registerCallbacks() {
    if (!initialized_) return;
    
    setLayoutCallbacks(
        gfxRect,
        gfxBorder,
        textRender,
        textWidth,
        registerLink,
        gfxTexture,
        gfxLine
    );
}

void NXRenderLayoutBridge::setLinkCallback(LinkCallback cb) {
    linkCallback_ = cb;
}

// =============================================================================
// Callback Implementations
// =============================================================================

void NXRenderLayoutBridge::gfxRect(float x, float y, float w, float h, uint32_t color) {
    if (!gpu_) return;
    
    NXRender::Rect rect(x, y, w, h);
    NXRender::Color c(
        (color >> 16) & 0xFF,  // R
        (color >> 8) & 0xFF,   // G
        color & 0xFF,          // B
        255                    // A
    );
    
    gpu_->fillRect(rect, c);
}

void NXRenderLayoutBridge::gfxBorder(float x, float y, float w, float h, 
                                      uint32_t color, float thickness) {
    if (!gpu_) return;
    
    NXRender::Rect rect(x, y, w, h);
    NXRender::Color c(
        (color >> 16) & 0xFF,
        (color >> 8) & 0xFF,
        color & 0xFF,
        255
    );
    
    gpu_->strokeRect(rect, c, thickness);
}

void NXRenderLayoutBridge::textRender(const std::string& text, float x, float y,
                                       uint32_t color, float fontSize, const LayoutBox*) {
    if (!gpu_) return;
    
    NXRender::Color c(
        (color >> 16) & 0xFF,
        (color >> 8) & 0xFF,
        color & 0xFF,
        255
    );
    
    gpu_->drawText(text, x, y, c, fontSize);
}

float NXRenderLayoutBridge::textWidth(const std::string& text, float fontSize, const LayoutBox*) {
    if (!gpu_) {
        // Fallback estimate
        return text.length() * fontSize * 0.5f;
    }
    
    NXRender::Size size = gpu_->measureText(text, fontSize);
    return size.width;
}

void NXRenderLayoutBridge::registerLink(float x, float y, float w, float h,
                                         const std::string& href, 
                                         const std::string& target) {
    if (linkCallback_) {
        linkCallback_(x, y, w, h, href, target);
    }
}

void NXRenderLayoutBridge::gfxTexture(float x, float y, float w, float h, 
                                       uint32_t textureId) {
    if (!gpu_) return;
    
    // Draw texture if valid
    if (textureId > 0) {
        NXRender::Rect dest(x, y, w, h);
        gpu_->drawTexture(textureId, dest);
    }
}

void NXRenderLayoutBridge::gfxLine(float x1, float y1, float x2, float y2,
                                    uint32_t color, float thickness) {
    if (!gpu_) return;
    
    NXRender::Color c(
        (color >> 16) & 0xFF,
        (color >> 8) & 0xFF,
        color & 0xFF,
        255
    );
    
    // Draw line as thin rect (for horizontal/vertical lines)
    float minX = std::min(x1, x2);
    float minY = std::min(y1, y2);
    float w = std::abs(x2 - x1);
    float h = std::abs(y2 - y1);
    
    // Ensure minimum width/height for visibility
    if (w < thickness) w = thickness;
    if (h < thickness) h = thickness;
    
    NXRender::Rect rect(minX, minY, w, h);
    gpu_->fillRect(rect, c);
}

} // namespace ZepraBrowser
