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

namespace {

// Bottom-right corner of the furthest border box below `box`, relative to
// the root box's origin (box itself at x, y)
void contentExtent(const LayoutBox& box, float x, float y, float& right, float& bottom) {
    for (const LayoutBox& child : box.children) {
        if (child.type == LayoutType::None) continue;
        float cx = x + child.x, cy = y + child.y;
        right = std::max(right, cx + child.width);
        bottom = std::max(bottom, cy + child.height);
        contentExtent(child, cx, cy, right, bottom);
    }
}

}  // namespace

void LayoutGeometryIndex::build(const PageLayout& page) {
    boxes_.clear();
    if (!page.root) return;
    if (page.body) boxes_[page.body].push_back({page.root, 0, 0});
    // Iterative walk: pages nest deeper than the stack likes
    struct Item {
        const LayoutBox* box;
        float x, y;
        const DOMElement* parentOwner;
    };
    std::vector<Item> stack;
    const DOMElement* rootOwner = page.body ? page.body : page.root->domElement;
    // Children pushed in reverse so the walk keeps tree order
    auto pushChildren = [&](const LayoutBox& box, float x, float y, const DOMElement* owner) {
        for (auto it = box.children.rbegin(); it != box.children.rend(); ++it) {
            stack.push_back({&*it, x + it->x, y + it->y, owner});
        }
    };
    pushChildren(*page.root, 0, 0, rootOwner);
    while (!stack.empty()) {
        Item item = stack.back();
        stack.pop_back();
        const LayoutBox& box = *item.box;
        if (box.type == LayoutType::None) continue;
        const DOMElement* owner = box.domElement;
        if (owner && !box.anonymous && owner != item.parentOwner) {
            boxes_[owner].push_back({item.box, item.x, item.y});
        }
        pushChildren(box, item.x, item.y, owner);
    }
}

ElementGeometry LayoutGeometryIndex::geometryOf(const PageLayout& page, const DOMElement* element) const {
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
        g.rects.push_back({left + p.x, top + p.y, p.box->width, p.box->height});
    }
    const Placed& first = found->second.front();
    const LayoutBox& box = *first.box;
    g.inlineBox = box.type == LayoutType::Inline;
    g.position = box.positionType;
    g.borderTop = box.borderTop;
    g.borderRight = box.borderRight;
    g.borderBottom = box.borderBottom;
    g.borderLeft = box.borderLeft;

    // Scrollable size: the padding box, grown to reach the furthest
    // descendant plus the end padding
    float padX = first.x + box.borderLeft, padY = first.y + box.borderTop;
    float padW = std::max(0.0f, box.width - box.borderLeft - box.borderRight);
    float padH = std::max(0.0f, box.height - box.borderTop - box.borderBottom);
    float right = padX, bottom = padY;
    contentExtent(box, first.x, first.y, right, bottom);
    g.scrollWidth = std::max(padW, right - padX + (right > padX ? box.paddingRight : 0));
    g.scrollHeight = std::max(padH, bottom - padY + (bottom > padY ? box.paddingBottom : 0));
    return g;
}

}  // namespace ZepraBrowser
