/**
 * @file dom_renderer.h
 * @brief NXRender Web Layer - DOM to Widget Rendering
 * 
 * Converts WebCore DOM tree to NXRender widget tree for rendering.
 * This is the core bridge between HTML/CSS and GPU rendering.
 * 
 * @copyright 2024 KetiveeAI
 */

#pragma once

#include <memory>
#include <vector>
#include <string>
#include <functional>
#include <cstdint>

// Forward declarations (WebCore)
namespace Zepra::WebCore {
    class DOMNode;
    class DOMElement;
    class DOMText;
    class CSSComputedStyle;
    class CSSEngine;
}

// Forward declarations (NXRender)
namespace NXRender {
    class Widget;
    class Container;
    class Label;
    class Button;
    class Theme;
    struct Color;
}

namespace NXRender::Web {

// =============================================================================
// Style Resolver
// =============================================================================

/**
 * @brief Convert CSS color to NXRender color
 */
Color resolveColor(uint32_t cssColor);

/**
 * @brief Font descriptor resolved from CSS
 */
struct ResolvedFont {
    std::string family;
    float size;
    bool bold;
    bool italic;
};

/**
 * @brief Resolve font from CSS computed style
 */
ResolvedFont resolveFont(const Zepra::WebCore::CSSComputedStyle* style);

// =============================================================================
// DOM Renderer
// =============================================================================

/**
 * @brief Options for DOM rendering
 */
struct RenderOptions {
    float viewportWidth = 1920;
    float viewportHeight = 1080;
    float scrollY = 0;
    Theme* theme = nullptr;
    bool debugBorders = false;
};

/**
 * @brief Result of rendering DOM
 */
struct RenderResult {
    std::unique_ptr<Container> rootWidget;
    float contentHeight;
    std::vector<std::string> errors;
};

/**
 * @brief Main DOM renderer class
 * 
 * Converts DOM tree to NXRender widget tree:
 * - <div> → Container
 * - <p>, <h1>-<h6>, text → Label
 * - <button> → Button
 * - <input> → TextField
 * - <a> → Label (with click handler)
 */
class DOMRenderer {
public:
    DOMRenderer();
    ~DOMRenderer();
    
    /**
     * @brief Render DOM tree to widget tree
     * @param root Root DOM node (usually <body>)
     * @param cssEngine CSS engine for computed styles
     * @param options Rendering options
     * @return RenderResult with root widget
     */
    RenderResult render(
        Zepra::WebCore::DOMNode* root,
        Zepra::WebCore::CSSEngine* cssEngine,
        const RenderOptions& options
    );
    
    /**
     * @brief Set link click callback
     */
    void setLinkCallback(std::function<void(const std::string& href)> callback);
    
    /**
     * @brief Set form submit callback  
     */
    void setFormCallback(std::function<void(const std::string& action)> callback);
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// =============================================================================
// Quick Functions
// =============================================================================

/**
 * @brief Quick DOM-to-widgets for simple cases
 */
std::unique_ptr<Container> renderHTML(
    const std::string& html,
    float width,
    float height,
    Theme* theme = nullptr
);

/**
 * @brief Render single element to widget
 */
std::unique_ptr<Widget> renderElement(
    Zepra::WebCore::DOMElement* element,
    Zepra::WebCore::CSSEngine* cssEngine
);

} // namespace NXRender::Web
