// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file page_geometry.hpp
 * @brief What page scripts can learn about the layout: where an element's
 *        boxes are (getBoundingClientRect, offset*, client*, scroll*), the
 *        viewport size and the page's scroll position.
 *
 * The browser implements PageGeometry from its layout boxes and hands it to
 * the script context. A query made after the DOM changed sees the new layout:
 * the browser lays the page out again before answering, as other browsers do.
 */

#pragma once

#include <vector>

namespace Zepra::WebCore {

class DOMElement;

struct GeometryRect {
    double x = 0, y = 0, width = 0, height = 0;
};

/// One element's layout, in viewport coordinates (CSS pixels)
struct ElementGeometry {
    bool hasBox = false;          ///< false: display:none, detached, or not laid out
    bool inlineBox = false;       ///< an inline box: clientWidth/clientHeight are 0
    int position = 0;             ///< 0 static, 1 relative, 2 absolute, 3 fixed, 4 sticky
    std::vector<GeometryRect> rects;  ///< border boxes, first one first (getClientRects)
    double borderTop = 0, borderRight = 0, borderBottom = 0, borderLeft = 0;
    double scrollWidth = 0, scrollHeight = 0;  ///< padding box grown to fit the content (if asked for)
    bool scrollContainer = false;  ///< overflow other than visible/clip: scrollTop/scrollLeft move it
    double scrollTop = 0, scrollLeft = 0;  ///< its scroll position
    /// The first box's border-box size before any transform (what the
    /// percentages of its `transform` are of)
    double boxWidth = 0, boxHeight = 0;

    /// The smallest rectangle holding every border box (getBoundingClientRect)
    GeometryRect bounds() const;
};

class PageGeometry {
public:
    virtual ~PageGeometry() = default;
    /// The element's boxes, after laying out the page again if the DOM
    /// changed. scrollWidth/scrollHeight are filled in only `withScrollSize`
    /// (measuring them walks the element's subtree).
    virtual ElementGeometry elementGeometry(const DOMElement* element, bool withScrollSize) = 0;
    /// The page's content area, without the browser's own bars
    virtual double viewportWidth() = 0;
    virtual double viewportHeight() = 0;
    /// The monitor the browser is on (window.screen), in CSS pixels; a
    /// browser that cannot tell reports the viewport
    virtual double screenWidth() { return viewportWidth(); }
    virtual double screenHeight() { return viewportHeight(); }
    /// How far the page is scrolled
    virtual double scrollX() = 0;
    virtual double scrollY() = 0;
    /// Scrolls the page; positions outside the scrollable range are clamped
    virtual void scrollTo(double x, double y) = 0;
    /// Scrolls an element that is a scroll container (elementGeometry's
    /// scrollContainer) to (x, y), clamped to its range; other elements
    /// do not move. Lays the page out first if the DOM changed.
    virtual void scrollElementTo(const DOMElement* element, double x, double y) {
        (void)element; (void)x; (void)y;
    }
};

inline GeometryRect ElementGeometry::bounds() const {
    if (rects.empty()) return {};
    double left = rects[0].x, top = rects[0].y;
    double right = left + rects[0].width, bottom = top + rects[0].height;
    for (const GeometryRect& r : rects) {
        if (r.x < left) left = r.x;
        if (r.y < top) top = r.y;
        if (r.x + r.width > right) right = r.x + r.width;
        if (r.y + r.height > bottom) bottom = r.y + r.height;
    }
    return {left, top, right - left, bottom - top};
}

}  // namespace Zepra::WebCore
