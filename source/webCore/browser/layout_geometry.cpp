// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * layout_geometry.cpp - Element geometry for page scripts, from the
 * LayoutBox tree (see layout_geometry.h)
 */

#include "layout_geometry.h"

#include <algorithm>

namespace ZepraBrowser {

using Zepra::WebCore::DOMElement;
using Zepra::WebCore::ElementGeometry;
using Zepra::WebCore::GeometryRect;

float documentWidth(const PageLayout& page) {
    // The browser does not scroll sideways: the page is as wide as the viewport
    return page.viewportWidth;
}

float documentHeight(const PageLayout& page) {
    float height = page.root ? page.root->height : 0;
    return std::max(page.viewportHeight, height);
}

void LayoutGeometryIndex::build(const PageLayout& page) {
    boxes_.clear();
    containers_.clear();
    if (!page.root) return;
    // layoutBlock puts the root at its margin (body's), and paintBox draws it there
    const float rootX = page.root->x, rootY = page.root->y;
    if (page.body) boxes_[page.body].push_back({page.root, rootX, rootY, -1});
    // Iterative walk: pages nest deeper than the stack likes
    struct Item {
        const LayoutBox* box;
        float x, y;
        const DOMElement* parentOwner;
        int container;  // innermost scroll container around the box
    };
    std::vector<Item> stack;
    const DOMElement* rootOwner = page.body ? page.body : page.root->domElement;
    // Children pushed in reverse so the walk keeps tree order
    auto pushChildren = [&](const LayoutBox& box, float x, float y, const DOMElement* owner, int container) {
        for (auto it = box.children.rbegin(); it != box.children.rend(); ++it) {
            stack.push_back({&*it, x + it->x, y + it->y, owner, container});
        }
    };
    pushChildren(*page.root, rootX, rootY, rootOwner, -1);
    while (!stack.empty()) {
        Item item = stack.back();
        stack.pop_back();
        const LayoutBox& box = *item.box;
        if (box.type == LayoutType::None) continue;
        const DOMElement* owner = box.domElement;
        if (owner && !box.anonymous && owner != item.parentOwner) {
            boxes_[owner].push_back({item.box, item.x, item.y, item.container});
        }
        int inner = item.container;
        if (isScrollContainer(box)) {
            containers_.push_back({item.box, item.container});
            inner = (int)containers_.size() - 1;
        }
        pushChildren(box, item.x, item.y, owner, inner);
    }
}

ScrollOffset LayoutGeometryIndex::scrollAround(int container) const {
    ScrollOffset sum;
    for (int c = container; c >= 0; c = containers_[c].parent) {
        ScrollOffset o = scrollOffsetOf(*containers_[c].box);
        sum.x += o.x;
        sum.y += o.y;
    }
    return sum;
}

const LayoutBox* LayoutGeometryIndex::principalBox(const DOMElement* element) const {
    auto found = boxes_.find(element);
    return found == boxes_.end() || found->second.empty() ? nullptr : found->second.front().box;
}

ScrollOffset LayoutGeometryIndex::ancestorScroll(const DOMElement* element) const {
    auto found = boxes_.find(element);
    return found == boxes_.end() || found->second.empty() ? ScrollOffset{} : scrollAround(found->second.front().container);
}

ElementGeometry LayoutGeometryIndex::geometryOf(const PageLayout& page, const DOMElement* element,
                                                bool withScrollSize) const {
    ElementGeometry g;
    if (!element || !page.root) return g;
    if (element == page.documentElement) {
        // The whole page, scrolled with it
        g.hasBox = true;
        g.rects.push_back({-page.scrollX, -page.scrollY, documentWidth(page), documentHeight(page)});
        g.scrollWidth = documentWidth(page);
        g.scrollHeight = documentHeight(page);
        return g;
    }
    auto found = boxes_.find(element);
    if (found == boxes_.end() || found->second.empty()) return g;

    const float left = page.originX - page.scrollX, top = page.originY - page.scrollY;
    g.hasBox = true;
    for (const Placed& p : found->second) {
        ScrollOffset s = scrollAround(p.container);
        g.rects.push_back({left + p.x - s.x, top + p.y - s.y, p.box->width, p.box->height});
    }
    const Placed& first = found->second.front();
    const LayoutBox& box = *first.box;
    g.inlineBox = box.type == LayoutType::Inline;
    g.position = box.positionType;
    g.borderTop = box.borderTop;
    g.borderRight = box.borderRight;
    g.borderBottom = box.borderBottom;
    g.borderLeft = box.borderLeft;
    g.scrollContainer = isScrollContainer(box);
    if (g.scrollContainer) {
        ScrollOffset at = scrollOffsetOf(box);
        g.scrollLeft = at.x;
        g.scrollTop = at.y;
    }
    if (!withScrollSize) return g;

    // Scrollable size: the padding box, grown to reach the furthest
    // descendant plus the end padding
    float w = 0, h = 0;
    scrollableSize(box, w, h);
    g.scrollWidth = w;
    g.scrollHeight = h;
    return g;
}

void placeBoxes(const LayoutBox& root, float x, float y, float scrollY) {
    struct Item {
        const LayoutBox* box;
        float x, y;
    };
    std::vector<Item> stack{{&root, x + root.x, y + root.y - scrollY}};
    while (!stack.empty()) {
        Item item = stack.back();
        stack.pop_back();
        item.box->screenX = item.x;
        item.box->screenY = item.y;
        // paintBox draws a scroll container's content moved by its position
        ScrollOffset s = scrollOffsetOf(*item.box);
        for (const LayoutBox& child : item.box->children) {
            if (child.type != LayoutType::None)
                stack.push_back({&child, item.x + child.x - s.x, item.y + child.y - s.y});
        }
    }
}

}  // namespace ZepraBrowser
