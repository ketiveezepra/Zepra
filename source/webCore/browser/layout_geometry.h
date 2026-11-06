// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * layout_geometry.h - Where elements are, read from the LayoutBox tree, for
 * the geometry page scripts see (getBoundingClientRect, offset*, client*,
 * scroll*; see scripting/page_geometry.hpp).
 *
 * A box's x/y are its border-box origin relative to its parent box's, and
 * paintBox puts the root box at the page origin minus the scroll position, so
 * an element's place in the viewport is that origin plus the sum of the
 * offsets down to its box.
 *
 * An element owns the boxes built for it (domElement) that are not inside
 * another box of its own: its principal box, or the text runs of an element
 * that has none. Anonymous boxes belong to no element; the root box is the
 * body's.
 */

#pragma once

#include "layout_engine.h"
#include "scripting/page_geometry.hpp"

#include <unordered_map>
#include <vector>

namespace ZepraBrowser {

/// The painted page: its box tree and where it sits in the viewport
struct PageLayout {
    const LayoutBox* root = nullptr;
    const Zepra::WebCore::DOMElement* body = nullptr;             ///< owns the root box
    const Zepra::WebCore::DOMElement* documentElement = nullptr;  ///< <html>: the whole page
    float originX = 0, originY = 0;  ///< where paintBox puts the root box, viewport coordinates, unscrolled
    float scrollX = 0, scrollY = 0;
    float viewportWidth = 0, viewportHeight = 0;
};

/// Size of the scrollable page: at least the viewport (what <html> reports
/// as scrollWidth/scrollHeight; the browser scrolls by at most height - viewport)
float documentWidth(const PageLayout& page);
float documentHeight(const PageLayout& page);

/**
 * @brief The boxes of every element, found in one walk of the tree. Holds
 * pointers into the tree: build it again after the tree is rebuilt or laid
 * out again. Scrolling needs no rebuild (positions are kept unscrolled).
 */
class LayoutGeometryIndex {
public:
    void build(const PageLayout& page);
    void clear() { boxes_.clear(); }

    /// The element's boxes in viewport coordinates at the page's scroll
    /// position; `withScrollSize` also measures its content (a walk of its subtree)
    Zepra::WebCore::ElementGeometry geometryOf(const PageLayout& page, const Zepra::WebCore::DOMElement* element,
                                               bool withScrollSize) const;

private:
    struct Placed {
        const LayoutBox* box;
        float x, y;  ///< border-box origin, relative to the root box's
    };
    std::unordered_map<const Zepra::WebCore::DOMElement*, std::vector<Placed>> boxes_;
};

/**
 * @brief Sets screenX/screenY of every box the way paintBox would, without
 * painting: the root at (x + root.x, y + root.y - scrollY). Hit-testing reads
 * them, so a tree rebuilt between frames (a script read geometry after
 * changing the DOM) can be hit-tested before it is painted.
 */
void placeBoxes(const LayoutBox& root, float x, float y, float scrollY);

}  // namespace ZepraBrowser
