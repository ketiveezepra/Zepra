/**
 * @file nxrender_layout_bridge.h
 * @brief Bridge between ZepraBrowser layout engine and NXRender C++ graphics
 * 
 * Connects the callback-based layout engine rendering to NXRender's GpuContext.
 * Provides static callback functions matching setLayoutCallbacks() signature.
 * 
 * Usage:
 *   #include "nxrender_layout_bridge.h"
 *   NXRenderLayoutBridge::initialize(gpu);
 *   NXRenderLayoutBridge::registerCallbacks();
 *   // Layout engine will now render via NXRender
 * 
 * @copyright 2025 KetiveeAI
 */

#pragma once

#include <string>
#include <cstdint>
#include <functional>

// Forward declaration
namespace NXRender {
    class GpuContext;
}

namespace ZepraBrowser {

struct LayoutBox;

/**
 * @brief Bridges layout engine to NXRender graphics
 */
class NXRenderLayoutBridge {
public:
    /**
     * @brief Initialize the bridge with a GpuContext
     * @param gpu Pointer to active GpuContext (must remain valid)
     */
    static void initialize(NXRender::GpuContext* gpu);
    
    /**
     * @brief Shutdown and cleanup
     */
    static void shutdown();
    
    /**
     * @brief Register NXRender callbacks with layout engine
     * 
     * Calls setLayoutCallbacks() with NXRender-backed implementations.
     * Must be called after initialize().
     */
    static void registerCallbacks();
    
    /**
     * @brief Check if bridge is initialized
     */
    static bool isInitialized();
    
    // =========================================================================
    // Link registration callback
    // =========================================================================
    
    using LinkCallback = std::function<void(float x, float y, float w, float h,
                                            const std::string& href,
                                            const std::string& target)>;
    
    /**
     * @brief Set callback for link hit box registration
     * 
     * The layout engine calls this when rendering links. The browser
     * should store these for click detection.
     */
    static void setLinkCallback(LinkCallback cb);
    
private:
    // Static callback functions (match setLayoutCallbacks signature)
    static void gfxRect(float x, float y, float w, float h, uint32_t color);
    static void gfxBorder(float x, float y, float w, float h, uint32_t color, float thickness);
    static void textRender(const std::string& text, float x, float y, uint32_t color, float fontSize,
                           const LayoutBox* font);
    static float textWidth(const std::string& text, float fontSize, const LayoutBox* font);
    static void registerLink(float x, float y, float w, float h, 
                            const std::string& href, const std::string& target);
    static void gfxTexture(float x, float y, float w, float h, uint32_t textureId);
    static void gfxLine(float x1, float y1, float x2, float y2, uint32_t color, float thickness);
    
    // State
    static NXRender::GpuContext* gpu_;
    static LinkCallback linkCallback_;
    static bool initialized_;
};

} // namespace ZepraBrowser
