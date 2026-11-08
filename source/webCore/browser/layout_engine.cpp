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
#include <cstdlib>
#include <sstream>

#include "../css/css_computed_style.hpp"
#include "../dom/form_controls.hpp"

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
static void (*s_text_render)(const std::string&, float, float, uint32_t, float, const LayoutBox*) = nullptr;
static float (*s_text_width)(const std::string&, float, const LayoutBox*) = nullptr;
static void (*s_register_link)(float, float, float, float, const std::string&, const std::string&) = nullptr;
static void (*s_gfx_texture)(float, float, float, float, uint32_t) = nullptr;
static void (*s_gfx_line)(float, float, float, float, uint32_t, float) = nullptr;

// Extended callbacks (set via setLayoutCallbacks2)
static void (*s_gfx_rrect)(float, float, float, float, float, uint32_t, uint8_t) = nullptr;
static void (*s_gfx_gradient)(float, float, float, float, uint32_t, uint32_t) = nullptr;
static void (*s_gfx_svg)(float, float, float, float, const std::string&, const std::string&, uint32_t) = nullptr;
static void (*s_gfx_canvas)(float, float, float, float, Zepra::WebCore::DOMElement*) = nullptr;
static void (*s_gfx_hgradient)(float, float, float, float, uint32_t, uint32_t) = nullptr;
static void (*s_push_clip)(float, float, float, float) = nullptr;
static void (*s_pop_clip)() = nullptr;

void setLayoutClipCallbacks(void (*push_clip)(float, float, float, float), void (*pop_clip)()) {
    s_push_clip = push_clip;
    s_pop_clip = pop_clip;
}

// =============================================================================
// TEXT FIELDS: the value (masked for passwords), the selection and the caret
// =============================================================================

namespace {

/// The field with keyboard focus and how far its text is scrolled to keep
/// the caret in view (only that field scrolls; the others show their start)
struct FocusedField {
    const Zepra::WebCore::DOMElement* element = nullptr;
    bool caretVisible = true;
    float scrollX = 0;
    float scrollY = 0;
} s_field;

constexpr uint32_t kSelectionColor = 0x3390FF;

bool isTextField(const LayoutBox& box) {
    return box.isInput && box.domElement && Zepra::WebCore::isTextControl(box.domElement);
}

bool isPassword(const LayoutBox& box) {
    return box.domElement && Zepra::WebCore::inputType(box.domElement) == "password" &&
           box.inputType != "textarea";
}

/// What the field shows for `text`: a bullet per character in a password
std::string shownText(const LayoutBox& box, const std::string& text) {
    if (!isPassword(box)) return text;
    std::string out;
    for (char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) out += "\xE2\x80\xA2";
    }
    return out;
}

/// The field's text area on screen and how it lays its lines out
struct FieldGeometry {
    float x0, y0, x1, y1;   // the content box
    float textX, baseline;  // where the first line starts (before scrolling)
    float lineHeight;
    bool multiline;
};

FieldGeometry fieldGeometry(const LayoutBox& box) {
    FieldGeometry g;
    g.multiline = box.inputType == "textarea";
    g.x0 = box.screenX + box.borderLeft + box.paddingLeft;
    g.y0 = box.screenY + box.borderTop + box.paddingTop;
    g.x1 = std::max(g.x0, box.screenX + box.width - box.borderRight - box.paddingRight);
    g.y1 = std::max(g.y0, box.screenY + box.height - box.borderBottom - box.paddingBottom);
    g.lineHeight = box.fontSize + 4;
    g.textX = box.screenX + box.paddingLeft + 4;
    g.baseline = g.multiline ? box.screenY + box.paddingTop + box.fontSize + 2
                             : (g.y0 + g.y1) / 2 + box.fontSize * 0.35f;
    return g;
}

std::vector<std::pair<size_t, size_t>> fieldLines(const std::string& value, bool multiline) {
    std::vector<std::pair<size_t, size_t>> lines;
    size_t start = 0;
    while (true) {
        size_t end = multiline ? value.find('\n', start) : std::string::npos;
        if (end == std::string::npos) {
            lines.push_back({start, value.size()});
            return lines;
        }
        lines.push_back({start, end});
        start = end + 1;
    }
}

float prefixWidth(const LayoutBox& box, const std::string& value, size_t lineStart, size_t offset) {
    if (!s_text_width || offset <= lineStart) return 0;
    return s_text_width(shownText(box, value.substr(lineStart, offset - lineStart)), box.fontSize, &box);
}

/// Paints a text control: placeholder or value, the selection behind it and
/// the caret, clipped to the content box. Returns false for boxes it does
/// not handle (the generic text painting draws those).
bool paintTextField(const LayoutBox& box) {
    if (!isTextField(box)) return false;
    const Zepra::WebCore::DOMElement* element = box.domElement;
    std::string value = Zepra::WebCore::formControlValue(element);
    bool focused = element == s_field.element;
    FieldGeometry g = fieldGeometry(box);
    auto lines = fieldLines(value, g.multiline);
    Zepra::WebCore::TextSelection selection = Zepra::WebCore::formControlSelection(element);

    // Scroll so the caret stays inside the content box
    if (focused) {
        size_t line = 0;
        while (line + 1 < lines.size() && selection.caret() > lines[line].second) line++;
        float caretX = g.textX + prefixWidth(box, value, lines[line].first, selection.caret());
        float room = g.x1 - 2;
        if (caretX - s_field.scrollX > room) s_field.scrollX = caretX - room;
        if (caretX - s_field.scrollX < g.x0) s_field.scrollX = std::max(0.0f, caretX - g.x0);
        float caretBottom = g.y0 + (line + 1) * g.lineHeight;
        if (caretBottom - s_field.scrollY > g.y1) s_field.scrollY = caretBottom - g.y1;
        if (g.y0 + line * g.lineHeight - s_field.scrollY < g.y0) s_field.scrollY = line * g.lineHeight;
    }
    float dx = focused ? s_field.scrollX : 0, dy = focused ? s_field.scrollY : 0;

    if (s_push_clip) s_push_clip(g.x0, g.y0, g.x1 - g.x0, g.y1 - g.y0);
    if (value.empty()) {
        std::string placeholder = element->getAttribute("placeholder");
        if (placeholder.empty() && !Zepra::WebCore::supportsTextSelection(element)) placeholder = box.placeholder;
        if (s_text_render && !placeholder.empty() && !g.multiline) {
            s_text_render(placeholder, g.textX, g.baseline, 0x999999, box.fontSize, &box);
        } else if (s_text_render && !placeholder.empty()) {
            auto placeholderLines = fieldLines(placeholder, true);
            for (size_t i = 0; i < placeholderLines.size(); i++) {
                const auto& [a, b] = placeholderLines[i];
                s_text_render(placeholder.substr(a, b - a), g.textX, g.baseline + i * g.lineHeight, 0x999999,
                              box.fontSize, &box);
            }
        }
    }
    for (size_t i = 0; i < lines.size(); i++) {
        const auto& [a, b] = lines[i];
        float baseline = g.baseline + i * g.lineHeight - dy;
        if (baseline < g.y0 - g.lineHeight || baseline > g.y1 + g.lineHeight) continue;
        // The selection behind the text
        if (focused && !selection.collapsed() && selection.start <= b && selection.end >= a) {
            size_t from = std::max(selection.start, a), to = std::min(selection.end, b);
            float x0 = g.textX + prefixWidth(box, value, a, from) - dx;
            float x1 = g.textX + prefixWidth(box, value, a, to) - dx;
            // A selected line break shows as a little space
            if (selection.end > b && i + 1 < lines.size()) x1 += box.fontSize * 0.3f;
            float top = baseline - box.fontSize - 1;
            if (s_gfx_rrect) s_gfx_rrect(x0, top, x1 - x0, box.fontSize + 5, 0, kSelectionColor, 90);
            else if (s_gfx_rect) s_gfx_rect(x0, top, x1 - x0, box.fontSize + 5, 0xB4D5FE);
        }
        if (s_text_render && b > a) s_text_render(shownText(box, value.substr(a, b - a)), g.textX - dx, baseline, box.color, box.fontSize, &box);
        // The caret
        size_t caret = selection.caret();
        bool onLine = caret >= a && caret <= b && (caret < b || i + 1 == lines.size() || caret != lines[i + 1].first);
        if (focused && s_field.caretVisible && onLine && s_gfx_rect) {
            float x = g.textX + prefixWidth(box, value, a, caret) - dx;
            s_gfx_rect(x, baseline - box.fontSize - 1, 1.0f, box.fontSize + 5, box.color);
        }
    }
    if (s_pop_clip) s_pop_clip();
    return true;
}

}  // namespace

void setFocusedTextField(const Zepra::WebCore::DOMElement* field, bool caretVisible) {
    if (field != s_field.element) s_field = {};
    s_field.element = field;
    s_field.caretVisible = caretVisible;
}

size_t textFieldOffsetAt(const LayoutBox& box, float x, float y) {
    if (!isTextField(box)) return 0;
    std::string value = Zepra::WebCore::formControlValue(box.domElement);
    FieldGeometry g = fieldGeometry(box);
    bool focused = box.domElement == s_field.element;
    float dx = focused ? s_field.scrollX : 0, dy = focused ? s_field.scrollY : 0;
    auto lines = fieldLines(value, g.multiline);
    float firstTop = g.baseline - box.fontSize - 2 - dy;
    long index = static_cast<long>(std::floor((y - firstTop) / g.lineHeight));
    size_t line = static_cast<size_t>(std::clamp(index, 0L, static_cast<long>(lines.size()) - 1));
    const auto& [a, b] = lines[line];
    // The character boundary nearest to x
    size_t best = a;
    float bestDistance = FLT_MAX;
    for (size_t offset = a;; offset = Zepra::WebCore::nextCharacter(value, offset)) {
        float distance = std::fabs(g.textX + prefixWidth(box, value, a, offset) - dx - x);
        if (distance < bestDistance) {
            best = offset;
            bestDistance = distance;
        }
        if (offset >= b) break;
    }
    return best;
}

// =============================================================================
// OVERFLOW AND SCROLL CONTAINERS
// =============================================================================

namespace {

bool clipsAxis(int overflow) { return overflow != kOverflowVisible; }

// Visible computes to auto when the other axis is neither visible nor clip
// (CSS Overflow 3, 3.1)
int usedOverflow(int self, int other) {
    if (self == kOverflowVisible && other != kOverflowVisible && other != kOverflowClip) return kOverflowAuto;
    return self;
}

// The paint clip in force (screen coordinates), for link hit boxes: a link
// scrolled out of its container must not be clickable where it would be
struct ClipRect {
    float x0, y0, x1, y1;
};
thread_local std::vector<ClipRect> t_clipStack;

bool insideClip(float& x, float& y, float& w, float& h) {
    if (t_clipStack.empty()) return true;
    const ClipRect& c = t_clipStack.back();
    float x0 = std::max(x, c.x0), y0 = std::max(y, c.y0);
    float x1 = std::min(x + w, c.x1), y1 = std::min(y + h, c.y1);
    if (x1 <= x0 || y1 <= y0) return false;
    x = x0; y = y0; w = x1 - x0; h = y1 - y0;
    return true;
}

// Bottom-right corner of the furthest border box in `box`'s content,
// relative to `box`'s border-box origin. A clipping box counts with its own
// size: its content is scrolled inside it.
void contentExtent(const LayoutBox& box, float x, float y, float& right, float& bottom) {
    for (const LayoutBox& child : box.children) {
        if (child.type == LayoutType::None || child.positionType == 3) continue;  // fixed: viewport's
        float cx = x + child.x, cy = y + child.y;
        right = std::max(right, cx + child.width);
        bottom = std::max(bottom, cy + child.height);
        if (!clipsContent(child)) contentExtent(child, cx, cy, right, bottom);
    }
}

}  // namespace

bool clipsContent(const LayoutBox& box) {
    if (!box.domElement || box.anonymous) return false;
    if (box.type == LayoutType::Inline || box.type == LayoutType::Text || box.type == LayoutType::None ||
        box.type == LayoutType::TableRow || box.type == LayoutType::TableRowGroup) return false;
    return clipsAxis(box.overflowX) || clipsAxis(box.overflowY);
}

bool isScrollContainer(const LayoutBox& box) {
    return clipsContent(box) && !(box.overflowX == kOverflowClip && box.overflowY == kOverflowClip);
}

bool userScrollableX(const LayoutBox& box) {
    int o = usedOverflow(box.overflowX, box.overflowY);
    return isScrollContainer(box) && (o == kOverflowScroll || o == kOverflowAuto);
}

bool userScrollableY(const LayoutBox& box) {
    int o = usedOverflow(box.overflowY, box.overflowX);
    return isScrollContainer(box) && (o == kOverflowScroll || o == kOverflowAuto);
}

ScrollOffset ElementScrollOffsets::get(const Zepra::WebCore::DOMElement* element) const {
    for (const auto& [el, offset] : offsets_) {
        if (el == element) return offset;
    }
    return {};
}

void ElementScrollOffsets::set(const Zepra::WebCore::DOMElement* element, ScrollOffset offset) {
    for (auto& entry : offsets_) {
        if (entry.first == element) {
            entry.second = offset;
            return;
        }
    }
    if (offset.x != 0 || offset.y != 0) offsets_.push_back({element, offset});
}

void ElementScrollOffsets::prune(const LayoutBox& root) {
    if (offsets_.empty()) return;
    std::vector<const Zepra::WebCore::DOMElement*> live;
    std::vector<const LayoutBox*> stack{&root};
    while (!stack.empty()) {
        const LayoutBox* box = stack.back();
        stack.pop_back();
        if (isScrollContainer(*box)) live.push_back(box->domElement);
        for (const LayoutBox& child : box->children) stack.push_back(&child);
    }
    offsets_.erase(std::remove_if(offsets_.begin(), offsets_.end(),
                                  [&](const auto& entry) {
                                      return std::find(live.begin(), live.end(), entry.first) == live.end();
                                  }),
                   offsets_.end());
}

ElementScrollOffsets& elementScrollOffsets() {
    static ElementScrollOffsets offsets;
    return offsets;
}

void scrollableSize(const LayoutBox& box, float& width, float& height) {
    float padX = box.borderLeft, padY = box.borderTop;
    float padW = std::max(0.0f, box.width - box.borderLeft - box.borderRight);
    float padH = std::max(0.0f, box.height - box.borderTop - box.borderBottom);
    float right = padX, bottom = padY;
    contentExtent(box, 0, 0, right, bottom);
    width = std::max(padW, right - padX + (right > padX ? box.paddingRight : 0));
    height = std::max(padH, bottom - padY + (bottom > padY ? box.paddingBottom : 0));
}

ScrollOffset maxScrollOffset(const LayoutBox& box) {
    if (!isScrollContainer(box)) return {};
    float w = 0, h = 0;
    scrollableSize(box, w, h);
    float padW = std::max(0.0f, box.width - box.borderLeft - box.borderRight);
    float padH = std::max(0.0f, box.height - box.borderTop - box.borderBottom);
    // Only the axes that clip scroll (overflow-x: hidden; overflow-y: visible
    // still makes y auto, see usedOverflow)
    ScrollOffset max;
    if (usedOverflow(box.overflowX, box.overflowY) != kOverflowVisible) max.x = std::max(0.0f, w - padW);
    if (usedOverflow(box.overflowY, box.overflowX) != kOverflowVisible) max.y = std::max(0.0f, h - padH);
    return max;
}

ScrollOffset scrollOffsetOf(const LayoutBox& box) {
    if (!isScrollContainer(box)) return {};
    ScrollOffset stored = elementScrollOffsets().get(box.domElement);
    if (stored.x == 0 && stored.y == 0) return stored;
    ScrollOffset max = maxScrollOffset(box);
    return {std::clamp(stored.x, 0.0f, max.x), std::clamp(stored.y, 0.0f, max.y)};
}

bool scrollContainerTo(const LayoutBox& box, float x, float y) {
    if (!isScrollContainer(box)) return false;
    if (!std::isfinite(x)) x = 0;
    if (!std::isfinite(y)) y = 0;
    ScrollOffset max = maxScrollOffset(box);
    elementScrollOffsets().set(box.domElement, {std::clamp(x, 0.0f, max.x), std::clamp(y, 0.0f, max.y)});
    return true;
}

const LayoutBox* userScrollContainerAt(const LayoutBox& root, float x, float y, float dx, float dy) {
    const LayoutBox* found = nullptr;
    // Depth-first along every box under the point; a deeper match wins, and
    // the content of a clipping box is only reached through the box itself
    std::vector<const LayoutBox*> stack{&root};
    while (!stack.empty()) {
        const LayoutBox* box = stack.back();
        stack.pop_back();
        if (box->type == LayoutType::None) continue;
        bool inside = x >= box->screenX && x < box->screenX + box->width &&
                      y >= box->screenY && y < box->screenY + box->height;
        if (clipsContent(*box) && !inside) continue;
        if (inside && box != &root && isScrollContainer(*box)) {
            ScrollOffset at = scrollOffsetOf(*box), max = maxScrollOffset(*box);
            bool canX = userScrollableX(*box) && ((dx > 0 && at.x < max.x) || (dx < 0 && at.x > 0));
            bool canY = userScrollableY(*box) && ((dy > 0 && at.y < max.y) || (dy < 0 && at.y > 0));
            if (canX || canY) found = box;
        }
        // Later siblings paint on top: visit them last so they win
        for (auto it = box->children.rbegin(); it != box->children.rend(); ++it) stack.push_back(&*it);
    }
    return found;
}

std::string cssUrl(const std::string& value) {
    size_t start = value.find("url(");
    if (start == std::string::npos) return "";
    start += 4;
    size_t end = value.find(')', start);
    if (end == std::string::npos) return "";
    std::string url = value.substr(start, end - start);
    size_t a = url.find_first_not_of(" \t\n\r\"'");
    size_t b = url.find_last_not_of(" \t\n\r\"'");
    return a == std::string::npos ? "" : url.substr(a, b - a + 1);
}

// Set rendering callbacks
void setLayoutCallbacks(
    void (*gfx_rect)(float, float, float, float, uint32_t),
    void (*gfx_border)(float, float, float, float, uint32_t, float),
    void (*text_render)(const std::string&, float, float, uint32_t, float, const LayoutBox*),
    float (*text_width)(const std::string&, float, const LayoutBox*),
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
    void (*gfx_svg)(float, float, float, float, const std::string&, const std::string&, uint32_t),
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
            w = measureTextWidth(img.text, img.fontSize, &img);
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

// A position: fixed box, laid out against the viewport (CSS 2.1 §10.3.7,
// §10.6.4). x and y are viewport coordinates; paintBox draws it there.
static void layoutFixed(LayoutBox& box, float vpW, float vpH) {
    auto given = [](const LayoutLength& l) { return l.isSet() && !l.isAuto(); };
    bool hasL = given(box.cssLeft), hasR = given(box.cssRight);
    bool hasT = given(box.cssTop), hasB = given(box.cssBottom);
    float left = hasL ? box.cssLeft.resolve(vpW, box.fontSize, vpW, vpH) : 0;
    float right = hasR ? box.cssRight.resolve(vpW, box.fontSize, vpW, vpH) : 0;
    float top = hasT ? box.cssTop.resolve(vpH, box.fontSize, vpW, vpH) : 0;
    float bottom = hasB ? box.cssBottom.resolve(vpH, box.fontSize, vpW, vpH) : 0;

    bool autoWidth = !box.cssWidth.isSet() || box.cssWidth.isAuto();
    if (autoWidth && hasL && hasR) {
        layoutBlock(box, std::max(0.0f, vpW - left - right), 0);  // stretched between the insets
    } else if (autoWidth && !box.isImage) {
        // Shrink-to-fit within the space the insets leave
        float minW, maxW;
        intrinsicWidths(box, minW, maxW);
        float available = vpW - left - right - box.marginLeft - box.marginRight;
        layoutAtWidth(box, std::max(0.0f, std::min(maxW, std::max(minW, available))));
    } else {
        layoutBlock(box, vpW, 0);
    }
    bool autoHeight = !box.cssHeight.isSet() || box.cssHeight.isAuto();
    if (autoHeight && hasT && hasB)
        box.height = std::max(box.height, vpH - top - bottom - box.marginTop - box.marginBottom);

    if (hasL) box.x = left + box.marginLeft;
    else if (hasR) box.x = vpW - right - box.width - box.marginRight;
    else box.x = box.marginLeft;
    if (hasT) box.y = top + box.marginTop;
    else if (hasB) box.y = vpH - bottom - box.height - box.marginBottom;
    else box.y = box.marginTop;
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
    float floatBottom = 0;  // lowest float margin edge of the block flow below
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
            if (child.positionType == 2 || child.positionType == 3) continue;  // out of flow
            
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
                    float tw = measureTextWidth(child.text, child.fontSize, &child);
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
                floatBottom = std::max(floatBottom, floatLeftBottom);
            } else { // float:right
                child.x = childX + floatRightX - child.width - child.marginRight;
                child.y = childY + child.marginTop;
                floatRightX -= child.width + child.marginLeft + child.marginRight;
                floatRightBottom = std::max(floatRightBottom, child.y + child.height + child.marginBottom);
                floatBottom = std::max(floatBottom, floatRightBottom);
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
            if (child.isInput || child.isSelect) {
                // Input defaults (form controls keep the size they were given)
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
                float textW = measureTextWidth(child.text, child.fontSize, &child);
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
    // A box that clips its overflow starts a block formatting context, which
    // grows to contain its floats (CSS 2.1 §10.6.7)
    if (clipsContent(box)) childY = std::max(childY, floatBottom);
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
        else if (child.positionType == 3) {
            layoutFixed(child, vpW, vpH);
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
// One axis of background-position: a keyword, percentage or length. The
// result is the offset of the tile in an area 'area' wide, the tile 'tile'.
static float backgroundOffset(const std::string& token, float area, float tile) {
    if (token == "left" || token == "top") return 0;
    if (token == "center") return (area - tile) / 2;
    if (token == "right" || token == "bottom") return area - tile;
    char* end = nullptr;
    float v = std::strtof(token.c_str(), &end);
    if (end == token.c_str()) return 0;
    if (*end == '%') return (area - tile) * v / 100.0f;
    return v;
}

// background-image: url() — CSS Backgrounds 3 §3.6-3.9: size (auto, cover,
// contain, lengths, percentages), position (keywords, %, px), repeat. The tile
// is positioned in the padding box and the painting clipped to the border box.
static void paintBackgroundImage(const LayoutBox& box, float x, float y) {
    float nw = box.bgNaturalWidth, nh = box.bgNaturalHeight;
    if (nw <= 0 || nh <= 0 || box.width <= 0 || box.height <= 0) return;
    float ax = x + box.borderLeft, ay = y + box.borderTop;
    float aw = box.width - box.borderLeft - box.borderRight;
    float ah = box.height - box.borderTop - box.borderBottom;
    if (aw <= 0 || ah <= 0) return;

    auto words = [](const std::string& s) {
        std::vector<std::string> out;
        std::istringstream in(s);
        for (std::string w; in >> w;) {
            for (char& c : w) c = (char)std::tolower((unsigned char)c);
            out.push_back(w);
        }
        return out;
    };

    float tw = nw, th = nh;
    std::vector<std::string> size = words(box.bgSize);
    if (!size.empty() && (size[0] == "cover" || size[0] == "contain")) {
        float s = size[0] == "cover" ? std::max(aw / nw, ah / nh) : std::min(aw / nw, ah / nh);
        tw = nw * s;
        th = nh * s;
    } else if (!size.empty()) {
        auto length = [](const std::string& t, float area) -> float {
            if (t == "auto") return -1;
            char* end = nullptr;
            float v = std::strtof(t.c_str(), &end);
            if (end == t.c_str()) return -1;
            return *end == '%' ? area * v / 100.0f : v;
        };
        float w = length(size[0], aw), h = size.size() > 1 ? length(size[1], ah) : -1;
        if (w > 0 && h > 0) { tw = w; th = h; }
        else if (w > 0) { tw = w; th = w * nh / nw; }
        else if (h > 0) { th = h; tw = h * nw / nh; }
    }
    if (tw < 0.5f || th < 0.5f) return;

    // Position: one or two values; a single vertical keyword is the y value
    std::vector<std::string> pos = words(box.bgPosition);
    std::string px = "0%", py = "0%";
    if (pos.size() == 1) {
        if (pos[0] == "top" || pos[0] == "bottom") { px = "center"; py = pos[0]; }
        else { px = pos[0]; py = "center"; }
    } else if (pos.size() >= 2) {
        px = pos[0];
        py = pos[1];
        if (px == "top" || px == "bottom" || py == "left" || py == "right") std::swap(px, py);
    }
    float ox = backgroundOffset(px, aw, tw), oy = backgroundOffset(py, ah, th);

    std::vector<std::string> rep = words(box.bgRepeat);
    bool repeatX = true, repeatY = true;
    if (rep.size() == 1) {
        if (rep[0] == "no-repeat") repeatX = repeatY = false;
        else if (rep[0] == "repeat-x") repeatY = false;
        else if (rep[0] == "repeat-y") repeatX = false;
    } else if (rep.size() >= 2) {
        repeatX = rep[0] != "no-repeat";
        repeatY = rep[1] != "no-repeat";
    }

    float x0 = ax + ox, y0 = ay + oy;
    float x1 = x0 + tw, y1 = y0 + th;
    if (repeatX) { x0 -= std::ceil((x0 - x) / tw) * tw; x1 = x + box.width; }
    if (repeatY) { y0 -= std::ceil((y0 - y) / th) * th; y1 = y + box.height; }
    if ((x1 - x0) / tw * (y1 - y0) / th > 10000) return;  // a 1px tile over a page

    if (s_push_clip) s_push_clip(x, y, box.width, box.height);
    if (box.bgSvgKey.empty() && !box.bgSvgData.empty())
        box.bgSvgKey = "bg_svg_" + std::to_string(std::hash<std::string>()(box.bgSvgData));
    for (float ty = y0; ty < y1 - 0.01f; ty += th) {
        for (float tx = x0; tx < x1 - 0.01f; tx += tw) {
            if (!box.bgSvgData.empty()) {
                // Its own document: currentColor is black, not the box's color
                if (s_gfx_svg) s_gfx_svg(tx, ty, tw, th, box.bgSvgData, box.bgSvgKey, 0x000000);
            } else if (s_gfx_texture) {
                s_gfx_texture(tx, ty, tw, th, box.bgTextureId);
            }
            if (!repeatX) break;
        }
        if (!repeatY) break;
    }
    if (s_pop_clip) s_pop_clip();
}

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

static void paintChildren(const LayoutBox& box, float screenX, float screenY, float viewportHeight);

namespace {

// A box painted out of tree order by its stacking context: where it goes on
// screen and the clip in force where it sits in the tree
struct PaintLayer {
    const LayoutBox* box;
    float x, y;                      // paintBox offset (its parent's origin)
    bool clipped;
    ClipRect clip;
    std::vector<ClipRect> scrollports;
    bool context;                    // starts a stacking context of its own
};

// One frame's paint: the viewport fixed boxes are placed in, the scroll
// containers sticky boxes stick to, and the box budget
struct PaintFrame {
    bool active = false;
    float viewX = 0, viewY = 0, viewW = 0, viewBottom = 0;
    const LayoutBox* root = nullptr;
    const LayoutBox* contextLayer = nullptr;  // the layer being painted, when it starts a context
    std::vector<ClipRect> scrollports;        // innermost last; empty: the viewport
    int count = 0;
    int depth = 0;
};
thread_local PaintFrame t_frame;

void collectFixed(const LayoutBox& box, std::vector<const LayoutBox*>& out) {
    for (const LayoutBox& child : box.children) {
        if (child.type == LayoutType::None || child.opacity <= 0.001f) continue;
        if (child.positionType == 3) out.push_back(&child);
        collectFixed(child, out);
    }
}

// Painted by its stacking context, not in tree order: positioned boxes, and
// flex items with a z-index (CSS Flexbox §4.3)
bool isLayered(const LayoutBox& child, const LayoutBox& parent) {
    return child.positionType != 0 || (parent.type == LayoutType::Flex && !child.zIndexAuto);
}

// Starts a stacking context: its layered descendants paint inside it
// (CSS 2.1 §9.9.1; fixed, sticky and opacity from CSS Positioned Layout 3
// and CSS Color 4)
bool startsContext(const LayoutBox& box, bool flexItem) {
    if (box.positionType == 3 || box.positionType == 4) return true;
    if (!box.zIndexAuto && (box.positionType != 0 || flexItem)) return true;
    return box.opacity < 0.999f;
}

int layerZ(const LayoutBox& box) { return box.zIndexAuto ? 0 : box.zIndex; }

// How far a position: sticky child moves from its in-flow place so it stays
// `top`/`bottom`/`left`/`right` inside its scrollport, without leaving its
// parent's content box (CSS Positioned Layout 3, §3.4)
void stickyShift(const LayoutBox& child, const LayoutBox& parent, float parentX, float parentY,
                 const std::vector<ClipRect>& scrollports, float& dx, float& dy) {
    dx = dy = 0;
    ClipRect port = scrollports.empty()
        ? ClipRect{t_frame.viewX, t_frame.viewY, t_frame.viewX + t_frame.viewW, t_frame.viewBottom}
        : scrollports.back();
    float vpW = layoutViewportWidth(), vpH = layoutViewportHeight();
    float parentW = parent.width, parentH = parent.height;
    if (isScrollContainer(parent)) scrollableSize(parent, parentW, parentH);
    float cbLeft = parentX + parent.borderLeft + parent.paddingLeft;
    float cbTop = parentY + parent.borderTop + parent.paddingTop;
    float cbRight = parentX + parentW - parent.borderRight - parent.paddingRight;
    float cbBottom = parentY + parentH - parent.borderBottom - parent.paddingBottom;
    float x = parentX + child.x, y = parentY + child.y;
    auto given = [](const LayoutLength& l) { return l.isSet() && !l.isAuto(); };

    if (given(child.cssTop)) {
        float want = port.y0 + child.cssTop.resolve(port.y1 - port.y0, child.fontSize, vpW, vpH) - y;
        if (want > 0) dy = std::max(0.0f, std::min(want, cbBottom - child.marginBottom - (y + child.height)));
    }
    if (dy == 0 && given(child.cssBottom)) {
        float want = port.y1 - child.cssBottom.resolve(port.y1 - port.y0, child.fontSize, vpW, vpH) - (y + child.height);
        if (want < 0) dy = std::min(0.0f, std::max(want, cbTop + child.marginTop - y));
    }
    if (given(child.cssLeft)) {
        float want = port.x0 + child.cssLeft.resolve(port.x1 - port.x0, child.fontSize, vpW, vpH) - x;
        if (want > 0) dx = std::max(0.0f, std::min(want, cbRight - child.marginRight - (x + child.width)));
    }
    if (dx == 0 && given(child.cssRight)) {
        float want = port.x1 - child.cssRight.resolve(port.x1 - port.x0, child.fontSize, vpW, vpH) - (x + child.width);
        if (want < 0) dx = std::min(0.0f, std::max(want, cbLeft + child.marginLeft - x));
    }
}

// The layered descendants a stacking context paints, in tree order, with
// where each sits. It walks the same offsets, scrolling and clips as
// paintChildren, and passes through positioned boxes with z-index: auto
// (their layered descendants belong to this context), but not into boxes
// that start a context of their own.
struct LayerWalk {
    std::vector<PaintLayer>& out;
    std::vector<ClipRect> clips;
    std::vector<ClipRect> ports;

    void children(const LayoutBox& box, float x, float y) {
        for (const LayoutBox& child : box.children) {
            if (child.type == LayoutType::None || child.opacity <= 0.001f) continue;
            if (child.positionType == 3) continue;  // fixed: the frame's root paints them
            float dx = 0, dy = 0;
            if (child.positionType == 4) stickyShift(child, box, x, y, ports, dx, dy);
            bool flexItem = box.type == LayoutType::Flex;
            bool layered = isLayered(child, box);
            bool context = startsContext(child, flexItem);
            if (layered) {
                PaintLayer layer{&child, x + dx, y + dy, !clips.empty(), {}, ports, layered && context};
                if (layer.clipped) layer.clip = clips.back();
                out.push_back(std::move(layer));
            }
            if (!context) descend(child, x + dx + child.x, y + dy + child.y);
        }
    }

    void descend(const LayoutBox& box, float x, float y) {
        if (box.children.empty()) return;
        if (!clipsContent(box)) {
            children(box, x, y);
            return;
        }
        const float padX = x + box.borderLeft, padY = y + box.borderTop;
        ClipRect clip{padX, padY, padX + std::max(0.0f, box.width - box.borderLeft - box.borderRight),
                      padY + std::max(0.0f, box.height - box.borderTop - box.borderBottom)};
        bool port = isScrollContainer(box);
        if (port) ports.push_back(clip);
        if (!clips.empty()) {
            const ClipRect& o = clips.back();
            clip = {std::max(clip.x0, o.x0), std::max(clip.y0, o.y0), std::min(clip.x1, o.x1), std::min(clip.y1, o.y1)};
        }
        clips.push_back(clip);
        ScrollOffset offset = scrollOffsetOf(box);
        children(box, x - offset.x, y - offset.y);
        clips.pop_back();
        if (port) ports.pop_back();
    }
};

void paintLayer(const PaintLayer& layer) {
    std::vector<ClipRect> clips, ports = layer.scrollports;
    clips.swap(t_clipStack);
    ports.swap(t_frame.scrollports);
    if (layer.clipped) {
        t_clipStack.push_back(layer.clip);
        if (s_push_clip) s_push_clip(layer.clip.x0, layer.clip.y0, std::max(0.0f, layer.clip.x1 - layer.clip.x0),
                                     std::max(0.0f, layer.clip.y1 - layer.clip.y0));
    }
    const LayoutBox* outer = t_frame.contextLayer;
    t_frame.contextLayer = layer.context ? layer.box : nullptr;
    paintBox(*layer.box, layer.x, layer.y, t_frame.viewBottom, 0);
    t_frame.contextLayer = outer;
    if (layer.clipped && s_pop_clip) s_pop_clip();
    clips.swap(t_clipStack);
    ports.swap(t_frame.scrollports);
}

// Paints `box`'s children whose top-left is relative to (x, y). In-flow
// children paint in tree order; layered ones are left to their stacking
// context. When `box` starts a context, it paints its layers around its
// in-flow content (CSS 2.1 Appendix E): negative z-index first, then the
// rest by z-index, z-index: auto counting as 0. The frame's root also
// paints the fixed boxes, at the viewport, unscrolled and unclipped.
void paintChildBoxes(const LayoutBox& box, float x, float y, float viewportHeight, bool context) {
    std::vector<PaintLayer> layers;
    if (context) {
        LayerWalk walk{layers, {}, t_frame.scrollports};
        if (!t_clipStack.empty()) walk.clips.push_back(t_clipStack.back());
        walk.children(box, x, y);
        if (&box == t_frame.root) {
            std::vector<const LayoutBox*> fixed;
            collectFixed(box, fixed);
            for (const LayoutBox* f : fixed)
                layers.push_back({f, t_frame.viewX, t_frame.viewY, false, {}, {}, true});
        }
        std::stable_sort(layers.begin(), layers.end(),
                         [](const PaintLayer& a, const PaintLayer& b) { return layerZ(*a.box) < layerZ(*b.box); });
    }
    size_t next = 0;
    for (; next < layers.size() && layerZ(*layers[next].box) < 0; ++next) paintLayer(layers[next]);
    for (const auto& child : box.children) {
        if (isLayered(child, box)) continue;
        paintBox(child, x, y, viewportHeight, 0);
    }
    for (; next < layers.size(); ++next) paintLayer(layers[next]);
}

} // namespace

std::vector<const LayoutBox*> fixedBoxesInPaintOrder(const LayoutBox& root) {
    std::vector<const LayoutBox*> out;
    if (root.positionType == 3) out.push_back(&root);
    collectFixed(root, out);
    std::stable_sort(out.begin(), out.end(),
                     [](const LayoutBox* a, const LayoutBox* b) { return layerZ(*a) < layerZ(*b); });
    return out;
}

static void paintBoxImpl(const LayoutBox& box, float offsetX, float offsetY, float viewportHeight, float scrollY);

void paintBox(const LayoutBox& box, float offsetX, float offsetY,
              float viewportHeight, float scrollY) {
    if (t_frame.active) {
        paintBoxImpl(box, offsetX, offsetY, viewportHeight, scrollY);
        return;
    }
    // A new frame: (offsetX, offsetY) is the viewport origin
    t_frame.active = true;
    t_frame.viewX = offsetX;
    t_frame.viewY = offsetY;
    t_frame.viewW = layoutViewportWidth();
    t_frame.viewBottom = viewportHeight;
    t_frame.root = &box;
    t_frame.contextLayer = nullptr;
    t_frame.scrollports.clear();
    t_frame.count = 0;
    t_frame.depth = 0;
    paintBoxImpl(box, offsetX, offsetY, viewportHeight, scrollY);
    t_frame.active = false;
    t_frame.root = nullptr;
}

static void paintBoxImpl(const LayoutBox& box, float offsetX, float offsetY,
                         float viewportHeight, float scrollY) {
    // Budget: limit total painted boxes per frame to prevent UI hang on complex pages
    if (t_frame.count++ > 4000 || t_frame.depth > 64) return;
    t_frame.depth++;
    struct PaintGuard { ~PaintGuard() { --t_frame.depth; } } pg;
    
    if (box.type == LayoutType::None) return;
    if (box.opacity <= 0.001f) return;
    if (box.visibilityHidden) {
        // visibility:hidden — skip painting this box but still paint children
        // (children inherit visibility but can override to visible)
        float screenX2 = offsetX + box.x;
        float screenY2 = offsetY + box.y - scrollY;
        box.screenX = screenX2;
        box.screenY = screenY2;
        paintChildren(box, screenX2, screenY2, viewportHeight);
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
        if (box.backgroundImage.find("gradient(") != std::string::npos && box.backgroundImage.find("url(") == std::string::npos &&
            s_gfx_gradient && s_gfx_rect) {
            paintGradient(box.backgroundImage, screenX, screenY, box.width, box.height, box.bgColor);
        } else if (box.borderRadius > 0 && s_gfx_rrect) {
            s_gfx_rrect(screenX, screenY, box.width, box.height, box.borderRadius, box.bgColor, alpha);
        } else if (s_gfx_rect) {
            s_gfx_rect(screenX, screenY, box.width, box.height, box.bgColor);
        }
    }
    if (box.bgTextureId > 0 || !box.bgSvgData.empty()) paintBackgroundImage(box, screenX, screenY);
    
    // Draw border, inside the border box (CSS Backgrounds 3, 4): each side is
    // a rectangle of its own width along that edge. Strokes are centered on
    // their path, so a stroke is only used for rounded boxes, inset by half
    // its width so it stays inside the box too.
    if ((box.borderTop > 0 || box.borderRight > 0 || box.borderBottom > 0 || box.borderLeft > 0) &&
        box.borderVisible && (s_gfx_rect || s_gfx_border)) {
        if ((box.borderRadius > 0 || !s_gfx_rect) && s_gfx_border) {
            float thickness = std::max({box.borderTop, box.borderRight, box.borderBottom, box.borderLeft});
            float t = std::min({thickness, box.width / 2.0f, box.height / 2.0f});
            if (t > 0) {
                s_gfx_border(screenX + t / 2.0f, screenY + t / 2.0f, box.width - t, box.height - t,
                             box.borderColor, t);
            }
        } else {
            // Top and bottom span the full width; left and right fill the
            // height between them (only the sides that have a width:
            // border-bottom alone is a rule, not a frame)
            float top = std::min(box.borderTop, box.height);
            float bottom = std::min(box.borderBottom, std::max(0.0f, box.height - top));
            float middle = box.height - top - bottom;
            float left = std::min(box.borderLeft, box.width);
            float right = std::min(box.borderRight, std::max(0.0f, box.width - left));
            if (top > 0) s_gfx_rect(screenX, screenY, box.width, top, box.borderColor);
            if (bottom > 0) s_gfx_rect(screenX, screenY + box.height - bottom, box.width, bottom, box.borderColor);
            if (middle > 0 && left > 0) s_gfx_rect(screenX, screenY + top, left, middle, box.borderColor);
            if (middle > 0 && right > 0)
                s_gfx_rect(screenX + box.width - right, screenY + top, right, middle, box.borderColor);
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
            // SVG: rasterized on the main thread; currentColor is the element's color
            if (box.svgKey.empty()) {
                std::hash<std::string> hasher;
                box.svgKey = "web_svg_" + std::to_string(hasher(box.svgData));
            }
            s_gfx_svg(screenX, screenY, box.width, box.height, box.svgData, box.svgKey, box.color);
        } else if (box.textureId > 0 && s_gfx_texture) {
            s_gfx_texture(screenX, screenY, box.width, box.height, box.textureId);
        } else if (s_gfx_rect && s_gfx_border) {
            s_gfx_rect(screenX, screenY, box.width, box.height, box.bgColor);
            // A 1px frame just inside the box (the stroke is centered on its path)
            s_gfx_border(screenX + 0.5f, screenY + 0.5f, box.width - 1.0f, box.height - 1.0f, 0x000000, 1.0f);
        }
    }

    // Draw text content (a text field paints its own)
    if (paintTextField(box)) {
        // nothing more: its value, selection and caret are drawn
    } else if (box.isSelect) {
        float x0 = screenX + box.borderLeft + box.paddingLeft;
        float x1 = screenX + box.width - box.borderRight - box.paddingRight;
        float midY = screenY + box.height / 2;
        if (s_push_clip) s_push_clip(x0, screenY, std::max(0.0f, x1 - x0), box.height);
        if (s_text_render && !box.text.empty())
            s_text_render(box.text, x0, midY + box.fontSize * 0.35f, box.color, box.fontSize, &box);
        if (s_pop_clip) s_pop_clip();
        // The arrow: a small chevron in the padding on the right
        float ax = screenX + box.width - box.borderRight - 13, ay = midY - 2;
        if (s_gfx_rect) {
            for (int row = 0; row < 4; row++) s_gfx_rect(ax + row, ay + row, 8.0f - 2 * row, 1.0f, box.color);
        }
    } else if ((!box.text.empty() || !box.placeholder.empty()) && 
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
            float textW = s_text_width(drawText, box.fontSize, &box);
            if (box.textAlign == 1) textX = screenX + (box.width - textW) / 2.0f;
            else if (box.textAlign == 2) textX = screenX + box.width - box.paddingRight - textW;
        }
        
        if (s_text_render && !drawText.empty()) s_text_render(drawText, textX, textY, drawColor, box.fontSize, &box);
        
        // Text decoration
        if (!drawText.empty()) {
            float textW = s_text_width ? s_text_width(drawText, box.fontSize, &box) : (drawText.length() * box.fontSize * 0.5f);
            
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
            float lx = screenX, ly = screenY, lw = box.width, lh = box.height;
            if (insideClip(lx, ly, lw, lh)) s_register_link(lx, ly, lw, lh, box.href, box.target);
        }
    }
    
    paintChildren(box, screenX, screenY, viewportHeight);
}

// A box's children at its screen position. A clipping box (overflow other
// than visible) keeps them inside its padding box and draws them moved by
// its scroll position, with a scroll thumb when the user can scroll it.
static void paintChildren(const LayoutBox& box, float screenX, float screenY, float viewportHeight) {
    if (box.children.empty()) return;
    // The frame's root, a layer that starts a context, or an in-flow box
    // that does (opacity) paints its own layers
    bool context = &box == t_frame.root || &box == t_frame.contextLayer ||
                   (box.positionType == 0 && startsContext(box, false));
    if (!clipsContent(box)) {
        paintChildBoxes(box, screenX, screenY, viewportHeight, context);
        return;
    }
    const float padX = screenX + box.borderLeft, padY = screenY + box.borderTop;
    const float padW = std::max(0.0f, box.width - box.borderLeft - box.borderRight);
    const float padH = std::max(0.0f, box.height - box.borderTop - box.borderBottom);
    ClipRect clip{padX, padY, padX + padW, padY + padH};
    if (!t_clipStack.empty()) {
        const ClipRect& outer = t_clipStack.back();
        clip = {std::max(clip.x0, outer.x0), std::max(clip.y0, outer.y0),
                std::min(clip.x1, outer.x1), std::min(clip.y1, outer.y1)};
    }
    t_clipStack.push_back(clip);
    if (s_push_clip) s_push_clip(padX, padY, padW, padH);
    ScrollOffset offset = scrollOffsetOf(box);
    bool scrollport = isScrollContainer(box);
    if (scrollport) t_frame.scrollports.push_back({padX, padY, padX + padW, padY + padH});
    paintChildBoxes(box, screenX - offset.x, screenY - offset.y, viewportHeight, context);
    if (scrollport) t_frame.scrollports.pop_back();
    if (s_pop_clip) s_pop_clip();
    t_clipStack.pop_back();

    // Overlay scroll thumbs (no gutter: layout keeps the full padding box)
    if (!s_gfx_rect) return;
    ScrollOffset max = maxScrollOffset(box);
    const float thick = 6, inset = 2, minThumb = 16;
    if (userScrollableY(box) && max.y > 0.5f && padH > minThumb + 2 * inset) {
        float track = padH - 2 * inset;
        float thumb = std::max(minThumb, track * padH / (padH + max.y));
        float y = padY + inset + (track - thumb) * (offset.y / max.y);
        s_gfx_rect(padX + padW - thick - inset, y, thick, thumb, 0x9AA0A6);
    }
    if (userScrollableX(box) && max.x > 0.5f && padW > minThumb + 2 * inset) {
        float track = padW - 2 * inset;
        float thumb = std::max(minThumb, track * padW / (padW + max.x));
        float x = padX + inset + (track - thumb) * (offset.x / max.x);
        s_gfx_rect(x, padY + padH - thick - inset, thumb, thick, 0x9AA0A6);
    }
}

// =============================================================================
// TEXT MEASUREMENT
// =============================================================================

float measureTextWidth(const std::string& text, float fontSize, const LayoutBox* font) {
    // Use callback if set, otherwise estimate
    if (s_text_width) return s_text_width(text, fontSize, font);
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
