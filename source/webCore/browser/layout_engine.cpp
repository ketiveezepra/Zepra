// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * layout_engine.cpp - CSS Layout Engine Implementation
 */

#include "layout_engine.h"
#include <algorithm>
#include <iostream>
#include <cfloat>
#include <cmath>

#include "../css/css_computed_style.hpp"

// Viewport dimensions (defined in zepra_browser.cpp)
extern int g_width;
extern int g_height;

namespace ZepraBrowser {

// =============================================================================
// RENDERING CALLBACKS (set by main browser)
// =============================================================================

// These are set by the main browser code to connect layout to rendering
static void (*s_gfx_rect)(float, float, float, float, uint32_t) = nullptr;
static void (*s_gfx_border)(float, float, float, float, uint32_t, float) = nullptr;
static void (*s_text_render)(const std::string&, float, float, uint32_t, float) = nullptr;
static float (*s_text_width)(const std::string&, float) = nullptr;
static void (*s_register_link)(float, float, float, float, const std::string&, const std::string&) = nullptr;
static void (*s_gfx_texture)(float, float, float, float, uint32_t) = nullptr;
static void (*s_gfx_line)(float, float, float, float, uint32_t, float) = nullptr;

// Extended callbacks (set via setLayoutCallbacks2)
static void (*s_gfx_rrect)(float, float, float, float, float, uint32_t, uint8_t) = nullptr;
static void (*s_gfx_gradient)(float, float, float, float, uint32_t, uint32_t) = nullptr;
static void (*s_gfx_svg)(float, float, float, float, const std::string&, const std::string&) = nullptr;
static void (*s_gfx_canvas)(float, float, float, float, Zepra::WebCore::DOMElement*) = nullptr;
static void (*s_gfx_hgradient)(float, float, float, float, uint32_t, uint32_t) = nullptr;

// Set rendering callbacks
void setLayoutCallbacks(
    void (*gfx_rect)(float, float, float, float, uint32_t),
    void (*gfx_border)(float, float, float, float, uint32_t, float),
    void (*text_render)(const std::string&, float, float, uint32_t, float),
    float (*text_width)(const std::string&, float),
    void (*register_link)(float, float, float, float, const std::string&, const std::string&),
    void (*gfx_texture)(float, float, float, float, uint32_t),
    void (*gfx_line)(float, float, float, float, uint32_t, float)
) {
    s_gfx_rect = gfx_rect;
    s_gfx_border = gfx_border;
    s_text_render = text_render;
    s_text_width = text_width;
    s_register_link = register_link;
    s_gfx_texture = gfx_texture;
    s_gfx_line = gfx_line;
}

void setLayoutCallbacks2(
    void (*gfx_rrect)(float, float, float, float, float, uint32_t, uint8_t),
    void (*gfx_gradient)(float, float, float, float, uint32_t, uint32_t),
    void (*gfx_svg)(float, float, float, float, const std::string&, const std::string&),
    void (*gfx_hgradient)(float, float, float, float, uint32_t, uint32_t)
) {
    s_gfx_rrect = gfx_rrect;
    s_gfx_gradient = gfx_gradient;
    s_gfx_svg = gfx_svg;
    s_gfx_hgradient = gfx_hgradient;
}

void setCanvasPaintCallback(void (*gfx_canvas)(float, float, float, float, Zepra::WebCore::DOMElement*)) {
    s_gfx_canvas = gfx_canvas;
}

// =============================================================================
// VIEWPORT
// =============================================================================

static float s_viewportW = 0;
static float s_viewportH = 0;

void setLayoutViewport(float width, float height) {
    s_viewportW = width;
    s_viewportH = height;
}

float layoutViewportWidth() { return s_viewportW > 0 ? s_viewportW : (float)g_width; }
float layoutViewportHeight() { return s_viewportH > 0 ? s_viewportH : (float)g_height; }

// =============================================================================
// LAYOUT ALGORITHM
// =============================================================================

// Restores what a layout pass overwrites to the values the box was built
// with (recording them on the first pass)
static void resetLayoutState(LayoutBox& box) {
    if (!box.specSaved) {
        box.specSaved = true;
        box.specType = box.type;
        box.specWidth = box.width;
        box.specHeight = box.height;
        box.specMarginTop = box.marginTop;
        box.specMarginRight = box.marginRight;
        box.specMarginBottom = box.marginBottom;
        box.specMarginLeft = box.marginLeft;
    } else {
        box.type = box.specType;
        box.width = box.specWidth;
        box.height = box.specHeight;
        box.marginTop = box.specMarginTop;
        box.marginRight = box.specMarginRight;
        box.marginBottom = box.specMarginBottom;
        box.marginLeft = box.specMarginLeft;
    }
    for (auto& child : box.children) resetLayoutState(child);
}

// Lays a flex item out again at a given border-box width (its fit-content
// or flexed width), as a block filling exactly that width
static void layoutAtWidth(LayoutBox& item, float width) {
    resetLayoutState(item);
    LayoutType type = item.type;
    LayoutLength cssWidth = item.cssWidth, cssMinWidth = item.cssMinWidth, cssMaxWidth = item.cssMaxWidth;
    bool mla = item.marginLeftAuto, mra = item.marginRightAuto;

    if (!isBlockLevel(type)) item.type = LayoutType::Block;
    item.cssWidth = item.cssMinWidth = item.cssMaxWidth = LayoutLength::none();
    item.marginLeftAuto = item.marginRightAuto = false;
    layoutBlock(item, width + item.marginLeft + item.marginRight, 0);

    item.type = type;
    item.cssWidth = cssWidth; item.cssMinWidth = cssMinWidth; item.cssMaxWidth = cssMaxWidth;
    item.marginLeftAuto = mla; item.marginRightAuto = mra;
    item.width = width;
}

void imageSize(const LayoutBox& img, float containingWidth, float& width, float& height) {
    float vpW = layoutViewportWidth(), vpH = layoutViewportHeight();
    float edgesW = img.paddingLeft + img.paddingRight + img.borderLeft + img.borderRight;
    float edgesH = img.paddingTop + img.paddingBottom + img.borderTop + img.borderBottom;
    // Content-box length, or false when it is auto (a percentage height has
    // no definite container height to resolve against)
    auto resolve = [&](const LayoutLength& len, bool horizontal, float& out) {
        if (!len.isSet() || len.isAuto()) return false;
        if (len.isPercent() && (!horizontal || containingWidth <= 0)) return false;
        out = len.resolve(containingWidth, img.fontSize, vpW, vpH);
        if (img.boxSizing == 1) out -= horizontal ? edgesW : edgesH;
        out = std::max(0.0f, out);
        return true;
    };

    float w = 0, h = 0;
    bool hasW = resolve(img.cssWidth, true, w);
    bool hasH = resolve(img.cssHeight, false, h);
    float ratio = (img.naturalWidth > 0 && img.naturalHeight > 0) ? img.naturalWidth / img.naturalHeight : 0;
    if (!hasW && !hasH) {
        if (ratio > 0) {
            w = img.naturalWidth;
            h = img.naturalHeight;
        } else if (!img.text.empty()) {
            // Not loaded (or failed): as big as its placeholder text
            w = measureTextWidth(img.text, img.fontSize);
            h = img.fontSize + 4;
        }
    } else if (!hasW) {
        w = ratio > 0 ? h * ratio : h;
    } else if (!hasH) {
        h = ratio > 0 ? w / ratio : w;
    }

    // min/max constraints keep the aspect ratio of a dimension that was not given
    float limit;
    if (resolve(img.cssMaxWidth, true, limit) && w > limit) {
        if (!hasH && ratio > 0) h = limit / ratio;
        w = limit;
    }
    if (resolve(img.cssMinWidth, true, limit) && w < limit) {
        if (!hasH && ratio > 0) h = limit / ratio;
        w = limit;
    }
    if (resolve(img.cssMaxHeight, false, limit) && h > limit) {
        if (!hasW && ratio > 0) w = limit * ratio;
        h = limit;
    }
    if (resolve(img.cssMinHeight, false, limit) && h < limit) {
        if (!hasW && ratio > 0) w = limit * ratio;
        h = limit;
    }
    width = w + edgesW;
    height = h + edgesH;
}

bool isBlockLevel(LayoutType type) {
    switch (type) {
        case LayoutType::Block:
        case LayoutType::Flex:
        case LayoutType::Table:
        case LayoutType::TableRowGroup:
        case LayoutType::TableRow:
        case LayoutType::TableCell:
        case LayoutType::TableCaption:
            return true;
        default:
            return false;
    }
}

void layoutBlock(LayoutBox& box, float containingWidth, float startY) {
    // Guard against deep recursion (complex pages like github.com with 1800+ boxes)
    static thread_local int s_depth = 0;
    if (++s_depth > 64) { --s_depth; return; }
    struct DepthGuard { ~DepthGuard() { --s_depth; } } guard;

    // A pass over the whole tree starts from the boxes as built
    if (s_depth == 1) resetLayoutState(box);

    if (box.type == LayoutType::Table) {
        layoutTable(box, containingWidth, startY);
        return;
    }

    if (box.isImage) {
        imageSize(box, containingWidth, box.width, box.height);
        box.x = box.marginLeft;
        box.y = startY + box.marginTop;
        return;
    }

    float vpW = layoutViewportWidth();
    float vpH = layoutViewportHeight();
    
    // Resolve deferred CSS width
    if (box.cssWidth.isSet() && !box.cssWidth.isAuto()) {
        box.width = box.cssWidth.resolve(containingWidth, box.fontSize, vpW, vpH);
        if (box.boxSizing == 0) {
            box.width += box.paddingLeft + box.paddingRight + box.borderLeft + box.borderRight;
        }
    } else if (isBlockLevel(box.type)) {
        // Block and Flex containers fill their containing block width (CSS2 §10.3.3)
        float w = containingWidth - box.marginLeft - box.marginRight;
        box.width = w > 0 ? w : containingWidth;
    }
    
    // Resolve deferred CSS height (container height is 0 for now — auto)
    if (box.cssHeight.isSet() && !box.cssHeight.isAuto()) {
        box.height = box.cssHeight.resolve(0, box.fontSize, vpW, vpH);
        if (box.boxSizing == 0) {
            box.height += box.paddingTop + box.paddingBottom + box.borderTop + box.borderBottom;
        }
    }
    
    // Apply min/max width constraints
    if (box.cssMinWidth.isSet() && !box.cssMinWidth.isAuto()) {
        float minW = box.cssMinWidth.resolve(containingWidth, box.fontSize, vpW, vpH);
        if (box.width < minW) box.width = minW;
    }
    if (box.cssMaxWidth.isSet() && !box.cssMaxWidth.isAuto()) {
        float maxW = box.cssMaxWidth.resolve(containingWidth, box.fontSize, vpW, vpH);
        if (box.width > maxW) box.width = maxW;
    }
    
    // Resolve margin: auto (horizontal centering for block elements with explicit width)
    if (box.marginLeftAuto && box.marginRightAuto && box.width > 0 && box.width < containingWidth) {
        float remaining = containingWidth - box.width;
        box.marginLeft = remaining / 2.0f;
        box.marginRight = remaining / 2.0f;
    } else if (box.marginLeftAuto && box.width > 0) {
        box.marginLeft = containingWidth - box.width - box.marginRight;
        if (box.marginLeft < 0) box.marginLeft = 0;
    } else if (box.marginRightAuto && box.width > 0) {
        box.marginRight = containingWidth - box.width - box.marginLeft;
        if (box.marginRight < 0) box.marginRight = 0;
    }
    
    // Position
    box.x = box.marginLeft;
    box.y = startY + box.marginTop;
    
    // Layout children
    float childY = box.paddingTop + box.borderTop;
    float childX = box.paddingLeft + box.borderLeft;
    float lineHeight = 0;
    float lineWidth = 0;
    float contentWidth = box.width - box.paddingLeft - box.paddingRight - box.borderLeft - box.borderRight;
    if (contentWidth < 0) contentWidth = 0;
    
    // Flex containers: pre-loop pass handles all children at once
    // Pre-resolve min-height for flex justify-content centering
    // cssHeight already resolved at line 93-97. Only apply min-height constraint here.
    if (box.cssMinHeight.isSet() && !box.cssMinHeight.isAuto()) {
        float minH = box.cssMinHeight.resolve(0, box.fontSize, vpW, vpH);
        if (box.boxSizing == 0) minH += box.paddingTop + box.paddingBottom + box.borderTop + box.borderBottom;
        if (box.height < minH) box.height = minH;
    }
    
    if (box.type == LayoutType::Flex) {
        float flexGap = box.gap;
        bool isColumn = (box.flexDirection == 1 || box.flexDirection == 3);
        bool isReverse = (box.flexDirection == 2 || box.flexDirection == 3);
        
        // Pre-resolve container height for column flex
        if (isColumn) {
            if (box.cssHeight.isSet() && !box.cssHeight.isAuto()) {
                float h = box.cssHeight.resolve(0, box.fontSize, vpW, vpH);
                if (h > box.height) box.height = h;
            }
            if (box.cssMinHeight.isSet() && !box.cssMinHeight.isAuto()) {
                float minH = box.cssMinHeight.resolve(0, box.fontSize, vpW, vpH);
                if (box.height < minH) box.height = minH;
            }
        }
        
        struct FlexChild {
            LayoutBox* ptr;
            float mainSize;
            float crossSize;
            float flexBasis;
            float flexGrow;
            float flexShrink;
            int alignSelf;
            int order;
        };
        
        struct FlexLine {
            std::vector<FlexChild> items;
            float mainSize = 0;
            float crossSize = 0;
            float freeSpace = 0;
        };
        std::vector<FlexLine> lines;
        lines.push_back(FlexLine());
        
        bool mainSizeAuto = false;
        float containerMainSize = isColumn
            ? (box.height - box.paddingTop - box.paddingBottom - box.borderTop - box.borderBottom)
            : contentWidth;
        if (containerMainSize <= 0 && isColumn) {
            mainSizeAuto = true;
            containerMainSize = 999999; // large value — items won't shrink
        }
        if (containerMainSize < 0) containerMainSize = 0;
        
        float containerCrossSize = isColumn ? contentWidth
            : (box.height - box.paddingTop - box.paddingBottom - box.borderTop - box.borderBottom);
        
        // Collect and measure all flex children
        std::vector<FlexChild> allItems;
        for (auto& child : box.children) {
            if (child.type == LayoutType::None) continue;
            
            float childAvailableWidth = contentWidth > 0 ? contentWidth : 200;

            // Items whose width isn't the container's (every row item, and
            // column items that aren't stretched) are as wide as their
            // content, within the space available: fit-content
            // (CSS Flexbox §9.2 step 3, §9.4 step 11)
            int align = child.alignSelf >= 0 ? child.alignSelf : box.alignItems;
            bool autoWidth = !child.cssWidth.isSet() || child.cssWidth.isAuto();
            if (autoWidth && !child.isImage && (!isColumn || align != 0) &&
                (isBlockLevel(child.type) || !child.children.empty())) {
                float minW, maxW;
                intrinsicWidths(child, minW, maxW);
                float available = childAvailableWidth - child.marginLeft - child.marginRight;
                layoutAtWidth(child, std::min(maxW, std::max(minW, available)));
            } else {
                layoutBlock(child, childAvailableWidth, 0);
            }
            
            // Shrink-to-fit: compute width from children if no explicit width was set
            if (child.width == 0 || (!child.cssWidth.isSet() || child.cssWidth.isAuto())) {
                float maxChildRight = 0;
                for (auto& grandchild : child.children) {
                    float right = grandchild.x + grandchild.width + grandchild.marginRight;
                    maxChildRight = std::max(maxChildRight, right);
                }
                if (!child.text.empty()) {
                    float tw = measureTextWidth(child.text, child.fontSize);
                    maxChildRight = std::max(maxChildRight, tw);
                }
                float shrinkWidth = maxChildRight + child.paddingRight + child.borderRight;
                if (shrinkWidth > child.width) child.width = shrinkWidth;
                // Ensure at least padding box
                float minPadBox = child.paddingLeft + child.paddingRight + child.borderLeft + child.borderRight;
                if (child.width < minPadBox) child.width = minPadBox;
            }
            // Ensure minimum height from content
            if (child.height == 0) {
                float maxChildBottom = 0;
                for (auto& grandchild : child.children) {
                    float bottom = grandchild.y + grandchild.height + grandchild.marginBottom;
                    maxChildBottom = std::max(maxChildBottom, bottom);
                }
                child.height = maxChildBottom + child.paddingBottom + child.borderBottom;
                if (child.height == 0) child.height = child.fontSize + 8;
            }
            
            FlexChild fc;
            fc.ptr = &child;
            fc.flexGrow = child.flexGrow;
            fc.flexShrink = child.flexShrink;
            fc.alignSelf = child.alignSelf;
            fc.order = child.order;
            
            // Resolve flex-basis: explicit overrides content size
            if (child.flexBasis.isSet() && !child.flexBasis.isAuto()) {
                fc.flexBasis = child.flexBasis.resolve(containerMainSize, child.fontSize, vpW, vpH);
            } else {
                fc.flexBasis = isColumn
                    ? (child.height + child.marginTop + child.marginBottom)
                    : (child.width + child.marginLeft + child.marginRight);
            }
            
            fc.mainSize = fc.flexBasis;
            fc.crossSize = isColumn
                ? (child.width + child.marginLeft + child.marginRight)
                : (child.height + child.marginTop + child.marginBottom);
            
            allItems.push_back(fc);
        }
        
        // Sort by CSS order
        std::stable_sort(allItems.begin(), allItems.end(),
            [](const FlexChild& a, const FlexChild& b) { return a.order < b.order; });
        
        // Reverse for row-reverse / column-reverse
        if (isReverse) {
            std::reverse(allItems.begin(), allItems.end());
        }
        
        // Partition into lines
        for (auto& fc : allItems) {
            FlexLine& currentLine = lines.back();
            float gapRequired = currentLine.items.empty() ? 0 : flexGap;
            
            if (box.flexWrap && !currentLine.items.empty() &&
                currentLine.mainSize + gapRequired + fc.mainSize > containerMainSize) {
                lines.push_back(FlexLine());
                FlexLine& newLine = lines.back();
                newLine.items.push_back(fc);
                newLine.mainSize = fc.mainSize;
                newLine.crossSize = fc.crossSize;
            } else {
                currentLine.items.push_back(fc);
                currentLine.mainSize += gapRequired + fc.mainSize;
                currentLine.crossSize = std::max(currentLine.crossSize, fc.crossSize);
            }
        }
        
        // Wrap-reverse: reverse line stacking order
        if (box.wrapReverse && lines.size() > 1) {
            std::reverse(lines.begin(), lines.end());
        }

        // The line of a single-line container is as big as the container's
        // definite cross size, min-height included (§9.4 step 15), so
        // align-items: center centres in the whole row
        if (lines.size() == 1 && !box.flexWrap && containerCrossSize > lines[0].crossSize) {
            lines[0].crossSize = containerCrossSize;
        }
        
        // Align-content: distribute cross-axis free space across lines
        float totalLinesCross = 0;
        for (auto& line : lines) totalLinesCross += line.crossSize;
        totalLinesCross += flexGap * std::max(0, (int)lines.size() - 1);
        
        float crossFreeSpace = containerCrossSize - totalLinesCross;
        if (crossFreeSpace < 0) crossFreeSpace = 0;
        
        float crossStart = 0;
        float lineCrossSpacing = flexGap;
        if (lines.size() > 1 && crossFreeSpace > 0) {
            int lineCount = lines.size();
            switch (box.alignContent) {
                case 1: crossStart = 0; break; // start
                case 2: crossStart = crossFreeSpace; break; // end
                case 3: crossStart = crossFreeSpace / 2.0f; break; // center
                case 4: // space-between
                    if (lineCount > 1) lineCrossSpacing = flexGap + crossFreeSpace / (lineCount - 1);
                    break;
                case 5: // space-around
                    if (lineCount > 0) {
                        float pad = crossFreeSpace / (lineCount * 2);
                        crossStart = pad;
                        lineCrossSpacing = flexGap + pad * 2;
                    }
                    break;
                default: // stretch (0)
                    if (lineCount > 0) {
                        float extra = crossFreeSpace / lineCount;
                        for (auto& line : lines) line.crossSize += extra;
                    }
                    break;
            }
        }
        
        // Resolve grow/shrink per line, then position
        float crossCursor = crossStart;
        
        for (auto& line : lines) {
            if (line.items.empty()) continue;
            
            float freeSpace = containerMainSize - line.mainSize;
            
            // Grow: distribute positive free space proportionally
            if (freeSpace > 0) {
                float totalGrow = 0;
                for (auto& fc : line.items) totalGrow += fc.flexGrow;
                if (totalGrow > 0) {
                    for (auto& fc : line.items) {
                        fc.mainSize += freeSpace * (fc.flexGrow / totalGrow);
                    }
                }
            }
            // Shrink: distribute negative overflow weighted by shrink*basis
            else if (freeSpace < 0) {
                float totalShrinkWeighted = 0;
                for (auto& fc : line.items)
                    totalShrinkWeighted += fc.flexShrink * fc.flexBasis;
                if (totalShrinkWeighted > 0) {
                    for (auto& fc : line.items) {
                        float ratio = (fc.flexShrink * fc.flexBasis) / totalShrinkWeighted;
                        fc.mainSize += freeSpace * ratio;
                        if (fc.mainSize < 0) fc.mainSize = 0;
                    }
                }
            }
            
            // Clamp to min/max constraints
            for (auto& fc : line.items) {
                LayoutBox& child = *fc.ptr;
                if (isColumn) {
                    if (child.cssMinHeight.isSet() && !child.cssMinHeight.isAuto()) {
                        float minH = child.cssMinHeight.resolve(containerMainSize, child.fontSize, vpW, vpH);
                        float minMain = minH + child.marginTop + child.marginBottom;
                        if (fc.mainSize < minMain) fc.mainSize = minMain;
                    }
                    if (child.cssMaxHeight.isSet() && !child.cssMaxHeight.isAuto()) {
                        float maxH = child.cssMaxHeight.resolve(containerMainSize, child.fontSize, vpW, vpH);
                        float maxMain = maxH + child.marginTop + child.marginBottom;
                        if (fc.mainSize > maxMain) fc.mainSize = maxMain;
                    }
                } else {
                    if (child.cssMinWidth.isSet() && !child.cssMinWidth.isAuto()) {
                        float minW = child.cssMinWidth.resolve(containerMainSize, child.fontSize, vpW, vpH);
                        float minMain = minW + child.marginLeft + child.marginRight;
                        if (fc.mainSize < minMain) fc.mainSize = minMain;
                    }
                    if (child.cssMaxWidth.isSet() && !child.cssMaxWidth.isAuto()) {
                        float maxW = child.cssMaxWidth.resolve(containerMainSize, child.fontSize, vpW, vpH);
                        float maxMain = maxW + child.marginLeft + child.marginRight;
                        if (fc.mainSize > maxMain) fc.mainSize = maxMain;
                    }
                }
            }
            
            // Recalculate line main size after flex + clamping
            line.mainSize = 0;
            for (size_t i = 0; i < line.items.size(); i++) {
                line.mainSize += line.items[i].mainSize;
                if (i < line.items.size() - 1) line.mainSize += flexGap;
            }
            line.freeSpace = containerMainSize - line.mainSize;
            if (line.freeSpace < 0) line.freeSpace = 0;

            // Row items that grew or shrank are laid out again at their new
            // width, so their contents wrap and align to it
            if (!isColumn) {
                for (auto& fc : line.items) {
                    LayoutBox& child = *fc.ptr;
                    float w = std::max(0.0f, fc.mainSize - child.marginLeft - child.marginRight);
                    if (child.children.empty() || child.isImage || std::fabs(w - child.width) < 0.5f) continue;
                    layoutAtWidth(child, w);
                    fc.crossSize = child.height + child.marginTop + child.marginBottom;
                    line.crossSize = std::max(line.crossSize, fc.crossSize);
                }
            }

            // Margin auto: absorb remaining free space before alignment
            // Per CSS Flexbox spec §9.5, auto margins consume free space first
            if (line.freeSpace > 0) {
                int autoMarginCount = 0;
                for (auto& fc : line.items) {
                    LayoutBox& child = *fc.ptr;
                    if (isColumn) {
                        if (child.marginTopAuto) autoMarginCount++;
                        if (child.marginBottomAuto) autoMarginCount++;
                    } else {
                        if (child.marginLeftAuto) autoMarginCount++;
                        if (child.marginRightAuto) autoMarginCount++;
                    }
                }
                if (autoMarginCount > 0) {
                    float perAutoMargin = line.freeSpace / autoMarginCount;
                    for (auto& fc : line.items) {
                        LayoutBox& child = *fc.ptr;
                        if (isColumn) {
                            if (child.marginTopAuto) child.marginTop = perAutoMargin;
                            if (child.marginBottomAuto) child.marginBottom = perAutoMargin;
                        } else {
                            if (child.marginLeftAuto) child.marginLeft = perAutoMargin;
                            if (child.marginRightAuto) child.marginRight = perAutoMargin;
                        }
                    }
                    line.freeSpace = 0; // Auto margins consume all free space
                }
            }
            
            // Main-axis alignment (justify-content)
            float mainOffset = 0;
            float itemSpacing = flexGap;
            int childCount = line.items.size();
            
            switch (box.justifyContent) {
                case 1: mainOffset = line.freeSpace; break;
                case 2: mainOffset = line.freeSpace / 2.0f; break;
                case 3:
                    if (childCount > 1) itemSpacing = flexGap + line.freeSpace / (childCount - 1);
                    break;
                case 4:
                    if (childCount > 0) {
                        float pad = line.freeSpace / (childCount * 2);
                        mainOffset = pad;
                        itemSpacing = flexGap + pad * 2;
                    }
                    break;
                case 5:
                    if (childCount > 0) {
                        float pad = line.freeSpace / (childCount + 1);
                        mainOffset = pad;
                        itemSpacing = flexGap + pad;
                    }
                    break;
                default: break;
            }
            
            float cursor = mainOffset;
            for (size_t i = 0; i < line.items.size(); i++) {
                auto& fc = line.items[i];
                LayoutBox& child = *fc.ptr;
                
                // Per-item align-self (-1 = inherit alignItems)
                int effectiveAlign = (fc.alignSelf >= 0) ? fc.alignSelf : box.alignItems;
                
                // Write resolved main size back to child
                if (isColumn) {
                    child.height = fc.mainSize - child.marginTop - child.marginBottom;
                    if (child.height < 0) child.height = 0;
                } else {
                    child.width = fc.mainSize - child.marginLeft - child.marginRight;
                    if (child.width < 0) child.width = 0;
                }
                
                // Cross-axis alignment
                float crossOffset = 0;
                float crossSpace = isColumn ? contentWidth : line.crossSize;
                switch (effectiveAlign) {
                    case 1: crossOffset = 0; break;
                    case 2: crossOffset = crossSpace - fc.crossSize; break;
                    case 3: crossOffset = (crossSpace - fc.crossSize) / 2.0f; break;
                    default:
                        if (isColumn)
                            child.width = contentWidth - child.marginLeft - child.marginRight;
                        else
                            child.height = line.crossSize - child.marginTop - child.marginBottom;
                        break;
                }
                
                if (isColumn) {
                    child.x = childX + child.marginLeft + crossOffset + crossCursor;
                    child.y = box.paddingTop + box.borderTop + cursor + child.marginTop;
                    cursor += fc.mainSize;
                    if (i < line.items.size() - 1) cursor += itemSpacing;
                } else {
                    child.x = childX + cursor + child.marginLeft;
                    child.y = childY + child.marginTop + crossOffset + crossCursor;
                    cursor += fc.mainSize;
                    if (i < line.items.size() - 1) cursor += itemSpacing;
                }
                child.type = LayoutType::FlexItem;
            }
            
            crossCursor += line.crossSize + lineCrossSpacing;
        }
        
        if (isColumn) {
            // For column flex, set childY to total items height for auto-height
            float totalMainSize = 0;
            for (auto& line : lines) {
                for (auto& fc : line.items) {
                    totalMainSize += fc.mainSize;
                }
                totalMainSize += flexGap * (line.items.size() > 1 ? line.items.size() - 1 : 0);
            }
            childY = box.paddingTop + box.borderTop + totalMainSize;
        } else {
            lineHeight = std::max(lineHeight, crossCursor > 0 ? crossCursor - flexGap : 0);
        }
    } else {
    // Block/Inline flow layout
    float prevBlockMarginBottom = 0;
    float floatLeftX = 0, floatRightX = contentWidth;
    float floatLeftBottom = 0, floatRightBottom = 0;
    
    for (auto& child : box.children) {
        if (child.type == LayoutType::None) continue;
        
        // Skip absolutely positioned elements from normal flow
        if (child.positionType == 2 || child.positionType == 3) continue;
        
        // Float handling
        if (child.floatType > 0) {
            layoutBlock(child, contentWidth, childY);
            if (child.floatType == 1) { // float:left
                child.x = childX + floatLeftX + child.marginLeft;
                child.y = childY + child.marginTop;
                floatLeftX += child.width + child.marginLeft + child.marginRight;
                floatLeftBottom = std::max(floatLeftBottom, child.y + child.height + child.marginBottom);
            } else { // float:right
                child.x = childX + floatRightX - child.width - child.marginRight;
                child.y = childY + child.marginTop;
                floatRightX -= child.width + child.marginLeft + child.marginRight;
                floatRightBottom = std::max(floatRightBottom, child.y + child.height + child.marginBottom);
            }
            continue;
        }
        
        if (isBlockLevel(child.type)) {
            // Flush any inline content first
            if (lineHeight > 0) {
                childY += lineHeight;
                lineHeight = 0;
                lineWidth = 0;
                prevBlockMarginBottom = 0;
            }
            
            // Margin collapsing: adjacent block margins collapse to max(prev_bottom, cur_top)
            // instead of summing (CSS2 §8.3.1, WebKit RenderBlockFlow::MarginValues)
            float effectiveMarginTop = child.marginTop;
            if (prevBlockMarginBottom > 0 && effectiveMarginTop > 0) {
                float collapsed = std::max(prevBlockMarginBottom, effectiveMarginTop);
                childY -= prevBlockMarginBottom; // undo prev bottom margin
                child.marginTop = collapsed;     // apply collapsed margin
            } else if (prevBlockMarginBottom < 0 && effectiveMarginTop < 0) {
                float collapsed = std::min(prevBlockMarginBottom, effectiveMarginTop);
                childY -= prevBlockMarginBottom;
                child.marginTop = collapsed;
            }
            
            // Adjust available width for floats
            float blockAvailWidth = contentWidth;
            float blockOffsetX = 0;
            if (childY < floatLeftBottom) {
                blockAvailWidth -= floatLeftX;
                blockOffsetX = floatLeftX;
            }
            if (childY < floatRightBottom) {
                blockAvailWidth -= (contentWidth - floatRightX);
            }
            
            layoutBlock(child, blockAvailWidth, childY);
            child.x += box.paddingLeft + box.borderLeft + blockOffsetX;

            // <center> and align attributes also align narrower block
            // children whose own margins don't position them
            if (box.alignChildren >= 2 && !child.marginLeftAuto && !child.marginRightAuto) {
                float freeSpace = blockAvailWidth - child.totalWidth();
                if (freeSpace > 0) child.x += box.alignChildren == 2 ? freeSpace / 2.0f : freeSpace;
            }
            
            childY = child.y + child.height + child.marginBottom;
            prevBlockMarginBottom = child.marginBottom;
            
            // Clear floats if child extends past them
            if (childY >= floatLeftBottom) floatLeftX = 0;
            if (childY >= floatRightBottom) floatRightX = contentWidth;
            
        } else if (child.type == LayoutType::Inline || child.type == LayoutType::Text || child.type == LayoutType::InlineBlock) {
            // Inline/Text/InlineBlock: flow horizontally with wrapping
            
            // Measure width/height
            if (child.isInput) {
                // Input defaults
                if (child.width == 0) child.width = 200;
                if (child.height == 0) child.height = std::max(24.0f, child.fontSize + 8);
            } else if (child.isImage) {
                imageSize(child, contentWidth, child.width, child.height);
            } else if (!child.children.empty()) {
                // Inline element with children — recursive layout
                float availWidth = contentWidth > 0 ? contentWidth : containingWidth;
                child.width = availWidth;
                layoutBlock(child, availWidth, 0);
                // Shrink-to-fit: compute content width from children
                float maxChildRight = 0;
                for (const auto& gc : child.children) {
                    float right = gc.x + gc.width + gc.marginRight;
                    maxChildRight = std::max(maxChildRight, right);
                }
                float contentW = maxChildRight + child.paddingRight + child.borderRight;
                if (contentW > 0 && contentW < availWidth)
                    child.width = contentW;
            } else {
                float textW = measureTextWidth(child.text, child.fontSize);
                child.width = textW;
                child.height = child.fontSize + 4;
            }
            
            // Check for line wrap
            if (lineWidth > 0 && lineWidth + child.width > contentWidth) {
                // Start new line
                childY += lineHeight;
                lineWidth = 0;
                lineHeight = 0;
            }
            
            child.x = childX + lineWidth + child.marginLeft;
            child.y = childY + child.marginTop;
            
            lineWidth += child.width + child.marginLeft + child.marginRight + 4; // Spacing
            lineHeight = std::max(lineHeight, child.height + child.marginTop + child.marginBottom);
        }
    }
    
    // Add height of the last inline line
    if (lineHeight > 0) {
        childY += lineHeight;
        lineHeight = 0;
    }
    } // end else (block/inline flow)
    
    // Shrink-to-fit for InlineBlock wrappers (MUST be before text-align so we don't include huge X shifts)
    if (box.type == LayoutType::InlineBlock || box.type == LayoutType::Inline) {
        float maxRight = 0;
        for (const auto& child : box.children) {
            maxRight = std::max(maxRight, child.x + child.width + child.marginRight);
        }
        float tightWidth = maxRight + box.paddingRight + box.borderRight;
        if (tightWidth > 0 && tightWidth < box.width) {
            box.width = tightWidth;
            // Also need to update contentWidth so text-align uses the new smaller width!
            contentWidth = box.width - box.paddingLeft - box.paddingRight - box.borderLeft - box.borderRight;
        }
    }
    
    // Apply text-align: center/right to inline children
    if (box.textAlign > 0 && contentWidth > 0) {
        // Group children into lines by Y position and shift their X
        float currentLineY = -1;
        float lineMaxRight = 0;
        std::vector<LayoutBox*> lineChildren;
        
        auto flushLine = [&]() {
            if (lineChildren.empty()) return;
            float lineContentWidth = lineMaxRight - (box.paddingLeft + box.borderLeft);
            float shift = 0;
            if (box.textAlign == 1) // center
                shift = (contentWidth - lineContentWidth) / 2.0f;
            else if (box.textAlign == 2) // right
                shift = contentWidth - lineContentWidth;
            if (shift > 0) {
                for (auto* lc : lineChildren) {
                    if (!isBlockLevel(lc->type))
                        lc->x += shift;
                }
            }
            lineChildren.clear();
        };
        
        for (auto& child : box.children) {
            if (child.type == LayoutType::None) continue;
            if (isBlockLevel(child.type)) {
                flushLine();
                currentLineY = -1;
                continue;
            }
            if (child.y != currentLineY) {
                flushLine();
                currentLineY = child.y;
                lineMaxRight = 0;
            }
            lineChildren.push_back(&child);
            lineMaxRight = std::max(lineMaxRight, child.x + child.width);
        }
        flushLine();
    }
    
    // Add remaining line height
    if (lineHeight > 0) {
        childY += lineHeight;
    }
    
    // Calculate height from children (auto height)
    // For auto-height boxes, use children's computed height but never shrink below
    // a pre-resolved min-height (e.g. flex column pre-resolve sets height from min-height)
    float computedHeight = childY + box.paddingBottom + box.borderBottom;
    if (box.cssHeight.isAuto() || !box.cssHeight.isSet()) {
        if (box.height <= 0) {
            box.height = computedHeight;
        } else {
            box.height = std::max(box.height, computedHeight);
        }
    }
    
    // Apply min/max height constraints (accounting for box-sizing)
    if (box.cssMinHeight.isSet() && !box.cssMinHeight.isAuto()) {
        float minH = box.cssMinHeight.resolve(0, box.fontSize, vpW, vpH);
        if (box.boxSizing == 0) minH += box.paddingTop + box.paddingBottom + box.borderTop + box.borderBottom;
        if (box.height < minH) box.height = minH;
    }
    if (box.cssMaxHeight.isSet() && !box.cssMaxHeight.isAuto()) {
        float maxH = box.cssMaxHeight.resolve(0, box.fontSize, vpW, vpH);
        if (box.boxSizing == 0) maxH += box.paddingTop + box.paddingBottom + box.borderTop + box.borderBottom;
        if (box.height > maxH) box.height = maxH;
    }
    
    // If block has no children but has text (leaf block), ensure minimum height
    if (box.type == LayoutType::Block && box.children.empty() && (!box.text.empty() || box.isInput)) {
        box.height = std::max(box.height, box.fontSize + box.paddingTop + box.paddingBottom + 4);
    }
    
    // Post-layout: apply CSS positioning offsets
    for (auto& child : box.children) {
        if (child.positionType == 2) {
            // Absolute: position relative to this containing block
            // First, layout the child to resolve its own dimensions
            layoutBlock(child, contentWidth, 0);
            
            float contW = box.width;
            float contH = box.height;
            
            // Resolve horizontal
            if (child.cssLeft.isSet() && !child.cssLeft.isAuto()) {
                child.x = box.paddingLeft + box.borderLeft + child.cssLeft.resolve(contW, child.fontSize, vpW, vpH);
            } else if (child.cssRight.isSet() && !child.cssRight.isAuto()) {
                child.x = contW - box.paddingRight - box.borderRight - child.width - child.cssRight.resolve(contW, child.fontSize, vpW, vpH);
            }
            
            // Resolve vertical
            if (child.cssTop.isSet() && !child.cssTop.isAuto()) {
                child.y = box.paddingTop + box.borderTop + child.cssTop.resolve(contH, child.fontSize, vpW, vpH);
            } else if (child.cssBottom.isSet() && !child.cssBottom.isAuto()) {
                child.y = contH - box.paddingBottom - box.borderBottom - child.height - child.cssBottom.resolve(contH, child.fontSize, vpW, vpH);
            }
        }
        else if (child.positionType == 1) {
            // Relative: offset from normal flow position
            if (child.cssTop.isSet() && !child.cssTop.isAuto())
                child.y += child.cssTop.resolve(0, child.fontSize, vpW, vpH);
            else if (child.cssBottom.isSet() && !child.cssBottom.isAuto())
                child.y -= child.cssBottom.resolve(0, child.fontSize, vpW, vpH);
            if (child.cssLeft.isSet() && !child.cssLeft.isAuto())
                child.x += child.cssLeft.resolve(0, child.fontSize, vpW, vpH);
            else if (child.cssRight.isSet() && !child.cssRight.isAuto())
                child.x -= child.cssRight.resolve(0, child.fontSize, vpW, vpH);
        }
    }
}

// =============================================================================
// PAINTING
// =============================================================================

namespace {

struct GradientStop {
    uint32_t color;
    float pos;  // 0..1 along the gradient line, or -1 when not given
};

uint32_t lerpColor(uint32_t a, uint32_t b, float t) {
    auto channel = [&](int shift) {
        float va = (float)((a >> shift) & 0xFF), vb = (float)((b >> shift) & 0xFF);
        return (uint32_t)std::lround(va + (vb - va) * t) << shift;
    };
    return channel(16) | channel(8) | channel(0);
}

uint32_t colorAt(const std::vector<GradientStop>& stops, float t) {
    if (t <= stops.front().pos) return stops.front().color;
    for (size_t i = 1; i < stops.size(); i++) {
        if (t <= stops[i].pos) {
            float span = stops[i].pos - stops[i - 1].pos;
            return span > 0 ? lerpColor(stops[i - 1].color, stops[i].color, (t - stops[i - 1].pos) / span)
                            : stops[i].color;
        }
    }
    return stops.back().color;
}

// CSS angle of a linear-gradient direction (0deg = to top, clockwise), or
// false when the token isn't one
bool parseGradientAngle(const std::string& token, float w, float h, float& deg) {
    if (token.compare(0, 3, "to ") == 0) {
        bool top = token.find("top") != std::string::npos, bottom = token.find("bottom") != std::string::npos;
        bool left = token.find("left") != std::string::npos, right = token.find("right") != std::string::npos;
        float dx = right ? 1.0f : left ? -1.0f : 0.0f;
        float dy = bottom ? 1.0f : top ? -1.0f : 0.0f;
        // A corner direction is perpendicular to the diagonal between the
        // other two corners (CSS Images 3 §3.1.1)
        if (dx != 0 && dy != 0) { dx *= h; dy *= w; }
        deg = std::atan2(dx, -dy) * 180.0f / 3.14159265f;
        return true;
    }
    char* end = nullptr;
    float v = std::strtof(token.c_str(), &end);
    if (end == token.c_str()) return false;
    std::string unit(end);
    if (unit == "deg") deg = v;
    else if (unit == "turn") deg = v * 360.0f;
    else if (unit == "rad") deg = v * 180.0f / 3.14159265f;
    else if (unit == "grad") deg = v * 0.9f;
    else return false;
    return true;
}

}  // namespace

// Paints the top gradient layer of a background: linear-gradient() at any
// angle with any number of stops; other gradient kinds get their first colour
static void paintGradient(const std::string& css, float x, float y, float w, float h, uint32_t fallback) {
    size_t kind = css.find("gradient(");
    bool isLinear = kind != std::string::npos && kind >= 7 && css.compare(kind - 7, 7, "linear-") == 0;
    // The gradient's own parentheses (a url() layer may come first, more
    // gradient layers may follow)
    size_t open = css.find('(', kind);
    size_t close = std::string::npos;
    for (size_t i = open, depth = 0; i < css.size(); i++) {
        if (css[i] == '(') depth++;
        else if (css[i] == ')' && --depth == 0) { close = i; break; }
    }
    float deg = 180.0f;
    std::vector<GradientStop> stops;
    if (open != std::string::npos && close != std::string::npos && close > open) {
        std::string inner = css.substr(open + 1, close - open - 1);
        // Split at top-level commas (not the ones inside rgb() or hsl())
        std::vector<std::string> tokens;
        int depth = 0;
        size_t from = 0;
        for (size_t i = 0; i <= inner.size(); i++) {
            if (i < inner.size() && inner[i] == '(') depth++;
            else if (i < inner.size() && inner[i] == ')') depth--;
            else if (i == inner.size() || (inner[i] == ',' && depth == 0)) {
                std::string t = inner.substr(from, i - from);
                size_t a = t.find_first_not_of(" \t\n"), b = t.find_last_not_of(" \t\n");
                tokens.push_back(a == std::string::npos ? "" : t.substr(a, b - a + 1));
                from = i + 1;
            }
        }
        for (size_t i = 0; i < tokens.size(); i++) {
            const std::string& token = tokens[i];
            if (token.empty()) continue;
            if (i == 0 && parseGradientAngle(token, w, h, deg)) continue;
            // Radial/conic shape, size and position ("circle at center", "from 90deg")
            if (i == 0 && !isLinear && token[0] != '#' && token.find('(') == std::string::npos &&
                (token.find("circle") != std::string::npos || token.find("ellipse") != std::string::npos ||
                 token.find("closest") != std::string::npos || token.find("farthest") != std::string::npos ||
                 token.compare(0, 3, "at ") == 0 || token.compare(0, 5, "from ") == 0)) continue;
            // "<color> [<position>]": the position follows the colour's ')' or first space
            size_t split = token.rfind(')');
            split = token.find(' ', split == std::string::npos ? 0 : split);
            std::string colorText = split == std::string::npos ? token : token.substr(0, split);
            float pos = -1;
            if (split != std::string::npos) {
                std::string p = token.substr(split + 1);
                char* end = nullptr;
                float v = std::strtof(p.c_str(), &end);
                if (end != p.c_str() && *end == '%') pos = v / 100.0f;
            }
            Zepra::WebCore::CSSColor col = Zepra::WebCore::CSSColor::parse(colorText);
            // Transparent stops have no RGB to draw
            if (col.a > 0) stops.push_back({(uint32_t)((col.r << 16) | (col.g << 8) | col.b), pos});
        }
    }
    if (stops.empty()) {
        if (css.find("transparent") == std::string::npos) s_gfx_rect(x, y, w, h, fallback);
        return;
    }
    if (stops.size() == 1 || !isLinear) {
        s_gfx_rect(x, y, w, h, stops[0].color);
        return;
    }

    // Unpositioned stops: the first at 0, the last at 1, the others spread
    // evenly between their positioned neighbours; positions never go back
    if (stops.front().pos < 0) stops.front().pos = 0;
    if (stops.back().pos < 0) stops.back().pos = 1;
    for (size_t i = 1; i < stops.size(); i++) {
        if (stops[i].pos >= 0) continue;
        size_t j = i;
        while (stops[j].pos < 0) j++;
        for (size_t k = i; k < j; k++)
            stops[k].pos = stops[i - 1].pos + (stops[j].pos - stops[i - 1].pos) * (float)(k - i + 1) / (float)(j - i + 1);
    }
    for (size_t i = 1; i < stops.size(); i++) stops[i].pos = std::max(stops[i].pos, stops[i - 1].pos);

    deg = std::fmod(std::fmod(deg, 360.0f) + 360.0f, 360.0f);
    float rad = deg * 3.14159265f / 180.0f;
    float dx = std::sin(rad), dy = -std::cos(rad);  // gradient direction, y down
    if (std::fabs(dx) < 1e-4f) dx = 0;
    if (std::fabs(dy) < 1e-4f) dy = 0;

    // Fills a band whose gradient position runs from t0 at 'start' to t1 at
    // 'start + length' along x (horizontal) or y, one 2-colour piece per stop
    auto drawBand = [&](float t0, float t1, float start, float length, bool horizontal,
                        float bx, float by, float bw, float bh) {
        std::vector<float> cuts{t0};
        for (const auto& st : stops) if (st.pos > t0 && st.pos < t1) cuts.push_back(st.pos);
        cuts.push_back(t1);
        for (size_t i = 0; i + 1 < cuts.size(); i++) {
            float a = start + (cuts[i] - t0) / (t1 - t0) * length;
            float b = start + (cuts[i + 1] - t0) / (t1 - t0) * length;
            uint32_t ca = colorAt(stops, cuts[i]), cb = colorAt(stops, cuts[i + 1]);
            if (b < a) { std::swap(a, b); std::swap(ca, cb); }
            if (horizontal) s_gfx_hgradient(a, by, b - a + 0.5f, bh, ca, cb);
            else s_gfx_gradient(bx, a, bw, b - a + 0.5f, ca, cb);
        }
    };

    if (dx == 0 || !s_gfx_hgradient) {
        // Vertical (without a horizontal primitive the angle is dropped)
        bool down = dy >= 0;
        drawBand(0, 1, down ? y : y + h, down ? h : -h, false, x, y, w, h);
    } else if (dy == 0) {
        bool right = dx > 0;
        drawBand(0, 1, right ? x : x + w, right ? w : -w, true, x, y, w, h);
    } else {
        // Any other angle: thin rows, each a horizontal gradient between
        // the positions its two ends reach (CSS Images 3 §3.1.1)
        float len = std::fabs(w * dx) + std::fabs(h * dy);
        float rowH = std::max(4.0f, h / 64.0f);
        for (float ry = 0; ry < h; ry += rowH) {
            float rh = std::min(rowH, h - ry);
            float cy = ry + rh / 2 - h / 2;
            float tl = 0.5f + (-w / 2 * dx + cy * dy) / len;
            float tr = 0.5f + (w / 2 * dx + cy * dy) / len;
            if (tl < tr) drawBand(tl, tr, x, w, true, x, y + ry, w, rh);
            else drawBand(tr, tl, x + w, -w, true, x, y + ry, w, rh);
        }
    }
}

static void dumpLayoutTree(const LayoutBox& box, int depth = 0) {
    static FILE* fp = nullptr;
    if (depth == 0) fp = fopen("/tmp/layout_dump.txt", "w");
    if (fp) {
        for (int i=0; i<depth; i++) fprintf(fp, "  ");
        fprintf(fp, "Type:%d xy=(%.1f, %.1f) wh=(%.1f, %.1f) text='%s'\n", 
            (int)box.type, box.x, box.y, box.width, box.height, box.text.c_str());
        for (const auto& c : box.children) dumpLayoutTree(c, depth + 1);
        if (depth == 0) { fclose(fp); fp = nullptr; }
    }
}

void paintBox(const LayoutBox& box, float offsetX, float offsetY,
              float viewportHeight, float scrollY) {
    // Budget: limit total painted boxes per frame to prevent UI hang on complex pages
    static thread_local int s_paintCount = 0;
    static thread_local int s_paintDepth = 0;
    if (s_paintDepth == 0) s_paintCount = 0;  // Reset at top-level call
    if (s_paintCount++ > 4000 || s_paintDepth > 64) return;
    s_paintDepth++;
    struct PaintGuard { ~PaintGuard() { --s_paintDepth; } } pg;
    
    if (box.type == LayoutType::None) return;
    if (box.opacity <= 0.001f) return;
    if (box.visibilityHidden) {
        // visibility:hidden — skip painting this box but still paint children
        // (children inherit visibility but can override to visible)
        float screenX2 = offsetX + box.x;
        float screenY2 = offsetY + box.y - scrollY;
        box.screenX = screenX2;
        box.screenY = screenY2;
        for (const auto& child : box.children) {
            paintBox(child, screenX2,
                     screenY2,
                     viewportHeight, 0);
        }
        return;
    }
    
    float screenX = offsetX + box.x;
    float screenY = offsetY + box.y - scrollY;
    
    // Save for hit testing
    box.screenX = screenX;
    box.screenY = screenY;
    
    // Skip if off-screen (culling)
    if (screenY + box.height < 0 || screenY > viewportHeight) {
        return;
    }
    
    uint8_t alpha = (uint8_t)(box.opacity * 255.0f);
    
    // Row and row-group backgrounds show through their cells only, not the
    // border-spacing between them (CSS 2.1 §17.5.1)
    if (box.hasBgColor && (box.type == LayoutType::TableRow || box.type == LayoutType::TableRowGroup)) {
        auto paintCells = [&](const LayoutBox& row, float rowX, float rowY) {
            for (const auto& cell : row.children) {
                if (cell.type == LayoutType::TableCell && s_gfx_rect)
                    s_gfx_rect(rowX + cell.x, rowY + cell.y, cell.width, cell.height, box.bgColor);
            }
        };
        if (box.type == LayoutType::TableRow) {
            paintCells(box, screenX, screenY);
        } else {
            for (const auto& row : box.children)
                paintCells(row, screenX + row.x, screenY + row.y);
        }
    } else if (box.hasBgColor) {
        if (!box.backgroundImage.empty() && box.backgroundImage.find("gradient") != std::string::npos &&
            s_gfx_gradient && s_gfx_rect) {
            paintGradient(box.backgroundImage, screenX, screenY, box.width, box.height, box.bgColor);
        } else if (box.borderRadius > 0 && s_gfx_rrect) {
            s_gfx_rrect(screenX, screenY, box.width, box.height, box.borderRadius, box.bgColor, alpha);
        } else if (s_gfx_rect) {
            s_gfx_rect(screenX, screenY, box.width, box.height, box.bgColor);
        }
    }
    
    // Draw border
    if ((box.borderTop > 0 || box.borderRight > 0 || box.borderBottom > 0 || box.borderLeft > 0) &&
        box.borderVisible && s_gfx_border) {
        bool uniform = box.borderTop == box.borderRight && box.borderTop == box.borderBottom &&
                       box.borderTop == box.borderLeft;
        if (uniform || box.borderRadius > 0 || !s_gfx_rect) {
            float thickness = std::max({box.borderTop, box.borderRight, box.borderBottom, box.borderLeft});
            s_gfx_border(screenX, screenY, box.width, box.height, box.borderColor, thickness);
        } else {
            // Only the sides that have a width (border-bottom alone is a rule, not a frame)
            if (box.borderTop > 0) s_gfx_rect(screenX, screenY, box.width, box.borderTop, box.borderColor);
            if (box.borderBottom > 0)
                s_gfx_rect(screenX, screenY + box.height - box.borderBottom, box.width, box.borderBottom, box.borderColor);
            if (box.borderLeft > 0) s_gfx_rect(screenX, screenY, box.borderLeft, box.height, box.borderColor);
            if (box.borderRight > 0)
                s_gfx_rect(screenX + box.width - box.borderRight, screenY, box.borderRight, box.height, box.borderColor);
        }
    }
    
    // Draw a canvas bitmap (stretched to the content box, as CSS sizes it)
    if (box.isCanvas) {
        if (s_gfx_canvas && box.domElement) {
            s_gfx_canvas(screenX + box.borderLeft + box.paddingLeft, screenY + box.borderTop + box.paddingTop,
                         box.contentWidth(), box.contentHeight(), box.domElement);
        }
    } else if (box.isImage) {
        if (!box.svgData.empty() && s_gfx_svg) {
            // SVG: render via NxSVG on main thread
            if (box.svgKey.empty()) {
                std::hash<std::string> hasher;
                box.svgKey = "web_svg_" + std::to_string(hasher(box.svgData));
            }
            s_gfx_svg(screenX, screenY, box.width, box.height, box.svgData, box.svgKey);
        } else if (box.textureId > 0 && s_gfx_texture) {
            s_gfx_texture(screenX, screenY, box.width, box.height, box.textureId);
        } else if (s_gfx_rect && s_gfx_border) {
            s_gfx_rect(screenX, screenY, box.width, box.height, box.bgColor);
            s_gfx_border(screenX, screenY, box.width, box.height, 0x000000, 1.0f);
        }
    }

    // Draw text content
    if ((!box.text.empty() || !box.placeholder.empty()) && 
        (box.type == LayoutType::Text || box.type == LayoutType::Inline || 
         box.type == LayoutType::Block || box.type == LayoutType::InlineBlock ||
         box.type == LayoutType::FlexItem)) {
        
        float textX = screenX + box.paddingLeft + (box.isInput ? 4 : 0);
        float textY = screenY + box.paddingTop + box.fontSize + (box.isInput ? 2 : 0);
        
        std::string drawText = box.text;
        uint32_t drawColor = box.color;
        
        if (box.isInput && drawText.empty() && !box.placeholder.empty()) {
            drawText = box.placeholder;
            drawColor = 0x999999;
        }
        
        if (box.textAlign > 0 && s_text_width && !drawText.empty()) {
            float textW = s_text_width(drawText, box.fontSize);
            if (box.textAlign == 1) textX = screenX + (box.width - textW) / 2.0f;
            else if (box.textAlign == 2) textX = screenX + box.width - box.paddingRight - textW;
        }
        
        if (s_text_render && !drawText.empty()) s_text_render(drawText, textX, textY, drawColor, box.fontSize);
        
        // Text decoration
        if (!drawText.empty()) {
            float textW = s_text_width ? s_text_width(drawText, box.fontSize) : (drawText.length() * box.fontSize * 0.5f);
            
            // Links underline unless their text-decoration says otherwise
            bool underline = box.textDecoration.find("underline") != std::string::npos ||
                             (box.isLink && box.textDecoration.empty());
            bool lineThrough = (box.textDecoration.find("line-through") != std::string::npos);
            
            if (underline) {
                float lineY = textY + 2;
                if (s_gfx_line) s_gfx_line(textX, lineY, textX + textW, lineY, drawColor, 1.0f);
                else if (s_gfx_rect) s_gfx_rect(textX, lineY, textW, 1.0f, drawColor);
            }
            if (lineThrough) {
                float lineY = textY - box.fontSize * 0.35f;
                if (s_gfx_line) s_gfx_line(textX, lineY, textX + textW, lineY, drawColor, 1.0f);
                else if (s_gfx_rect) s_gfx_rect(textX, lineY, textW, 1.0f, drawColor);
            }
        }
        
        // Register link hit box
        if (box.isLink && !box.href.empty() && s_register_link) {
            s_register_link(screenX, screenY, box.width, box.height, box.href, box.target);
        }
    }
    
    // Paint children
    for (const auto& child : box.children) {
        paintBox(child, screenX,
                 screenY,
                 viewportHeight, 0);
    }
}

// =============================================================================
// TEXT MEASUREMENT
// =============================================================================

float measureTextWidth(const std::string& text, float fontSize) {
    // Use callback if set, otherwise estimate
    if (s_text_width) return s_text_width(text, fontSize);
    return text.length() * fontSize * 0.5f;  // Rough estimate
}


// =============================================================================
// TEXT EXTRACTION
// =============================================================================

std::string getAllText(const LayoutBox& root) {
    std::string s;
    if (!root.text.empty()) s += root.text;
    
    for (const auto& child : root.children) {
        s += getAllText(child);
    }
    
    if (root.type == LayoutType::Block || root.type == LayoutType::TableRow ||
        root.type == LayoutType::TableCaption) s += "\n";
    else if (root.type == LayoutType::TableCell) s += "\t";
    return s;
}

std::string getTextInRect(const LayoutBox& root, float x, float y, float w, float h) {
    std::string s;
    
    // Check intersection (AABB)
    bool intersects = (root.screenX < x + w && root.screenX + root.width > x &&
                       root.screenY < y + h && root.screenY + root.height > y);
                       
    if (intersects && !root.text.empty()) {
        // Optimization: Could check specific character bounds, but box-level is okay for now
        s += root.text; 
    }
    
    // Optimization: If parent doesn't intersect at all, children probably don't either? 
    // BUT children are relative to parent content. If overflow is hidden... 
    // And cached screenX/Y are absolute.
    // If the box is completely outside, we might skip children, BUT layout overflow exists.
    // Culling is usually safe if we check cached screen coordinates.
    // However, generous bounds or just traversing all is safer to catch everything inside the rect.
    
    for (const auto& child : root.children) {
        s += getTextInRect(child, x, y, w, h);
    }
    
    if (intersects && root.type == LayoutType::Block && !s.empty()) {
        // Avoid double newlines if children already added them
        if (s.back() != '\n') s += "\n";
    }
    
    return s;
}

} // namespace ZepraBrowser
