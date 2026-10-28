// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file devtools_panel.cpp
 * @brief Docked developer tools: Inspector (DOM tree, box model, computed
 *        styles, element picker), Console (page output, JS input) and
 *        Network (the tab's NetworkMonitor).
 */
#include "devtools_panel.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>

#include "browser/dom.hpp"
#include "css/css_engine.hpp"
#include "scripting/script_context.hpp"
#include "browser/layout_engine.h"
#include "network_monitor.h"

using namespace Zepra::WebCore;

namespace ZepraBrowser::DevTools {

namespace {

// Firefox-dark-theme palette
constexpr uint32_t kBg          = 0x1C1B22;
constexpr uint32_t kBgAlt       = 0x202029;
constexpr uint32_t kToolbar     = 0x2B2A33;
constexpr uint32_t kBorder      = 0x3A3944;
constexpr uint32_t kText        = 0xFBFBFE;
constexpr uint32_t kMuted       = 0x9A9AA6;
constexpr uint32_t kAccent      = 0x00B3F4;
constexpr uint32_t kTag         = 0x75BFFF;
constexpr uint32_t kAttrName    = 0xFF7DE9;
constexpr uint32_t kAttrValue   = 0x86DE74;
constexpr uint32_t kComment     = 0x939393;
constexpr uint32_t kSelection   = 0x204E8A;
constexpr uint32_t kHover       = 0x2E2E38;
constexpr uint32_t kErrorBg     = 0x3B2222;
constexpr uint32_t kErrorText   = 0xFF8A8A;
constexpr uint32_t kWarnBg      = 0x3A3118;
constexpr uint32_t kWarnText    = 0xFFD36A;
constexpr uint32_t kResultText  = 0xB5CEA8;
constexpr uint32_t kInputText   = 0x9CDCFE;
constexpr uint32_t kOk          = 0x86DE74;

// Box-model colours (as Chrome/Firefox paint them over the page)
constexpr uint32_t kMarginColor  = 0xF6B26B;
constexpr uint32_t kBorderColor  = 0xFFE599;
constexpr uint32_t kPaddingColor = 0x93C47D;
constexpr uint32_t kContentColor = 0x6FA8DC;

constexpr float kToolbarH = 30;
constexpr float kRowH = 18;
constexpr float kFont = 12;
constexpr float kSmallFont = 11;
constexpr float kIndent = 14;
constexpr float kResizeGrip = 6;
constexpr float kMinHeight = 120;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// Collapses whitespace runs and trims; "" for blank text
std::string collapseText(const std::string& in) {
    std::string out;
    bool space = true;
    for (unsigned char c : in) {
        if (std::isspace(c)) {
            if (!space) out += ' ';
            space = true;
        } else {
            out += (char)c;
            space = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string truncate(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    return s.substr(0, max - 3) + "...";
}

std::string fmtPx(float v) {
    char buf[32];
    if (std::fabs(v - std::round(v)) < 0.05f) std::snprintf(buf, sizeof buf, "%d", (int)std::lround(v));
    else std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}

std::string lengthStr(const CSSLength& l) {
    if (l.isAuto()) return "auto";
    std::string n = fmtPx(l.value);
    switch (l.unit) {
        case CSSLength::Unit::Px: return n + "px";
        case CSSLength::Unit::Em: return n + "em";
        case CSSLength::Unit::Rem: return n + "rem";
        case CSSLength::Unit::Ex: return n + "ex";
        case CSSLength::Unit::Ch: return n + "ch";
        case CSSLength::Unit::Vw: return n + "vw";
        case CSSLength::Unit::Vh: return n + "vh";
        case CSSLength::Unit::Vmin: return n + "vmin";
        case CSSLength::Unit::Vmax: return n + "vmax";
        case CSSLength::Unit::Percent: return n + "%";
        case CSSLength::Unit::Fr: return n + "fr";
        case CSSLength::Unit::Cm: return n + "cm";
        case CSSLength::Unit::Mm: return n + "mm";
        case CSSLength::Unit::In: return n + "in";
        case CSSLength::Unit::Pt: return n + "pt";
        case CSSLength::Unit::Pc: return n + "pc";
        default: return n;
    }
}

std::string colorStr(const CSSColor& c) {
    if (c.a == 0) return "transparent";
    char buf[48];
    if (c.a == 255) std::snprintf(buf, sizeof buf, "rgb(%d, %d, %d)", c.r, c.g, c.b);
    else std::snprintf(buf, sizeof buf, "rgba(%d, %d, %d, %.2f)", c.r, c.g, c.b, c.a / 255.0);
    return buf;
}

const char* displayStr(DisplayValue d) {
    switch (d) {
        case DisplayValue::None: return "none";
        case DisplayValue::Block: return "block";
        case DisplayValue::Inline: return "inline";
        case DisplayValue::InlineBlock: return "inline-block";
        case DisplayValue::Flex: return "flex";
        case DisplayValue::InlineFlex: return "inline-flex";
        case DisplayValue::Grid: return "grid";
        case DisplayValue::InlineGrid: return "inline-grid";
        case DisplayValue::Table: return "table";
        case DisplayValue::TableRow: return "table-row";
        case DisplayValue::TableCell: return "table-cell";
        case DisplayValue::ListItem: return "list-item";
        case DisplayValue::Contents: return "contents";
        case DisplayValue::TableRowGroup: return "table-row-group";
        case DisplayValue::TableHeaderGroup: return "table-header-group";
        case DisplayValue::TableFooterGroup: return "table-footer-group";
        case DisplayValue::TableColumn: return "table-column";
        case DisplayValue::TableColumnGroup: return "table-column-group";
        case DisplayValue::TableCaption: return "table-caption";
        case DisplayValue::InlineTable: return "inline-table";
    }
    return "block";
}

const char* positionStr(PositionValue p) {
    switch (p) {
        case PositionValue::Static: return "static";
        case PositionValue::Relative: return "relative";
        case PositionValue::Absolute: return "absolute";
        case PositionValue::Fixed: return "fixed";
        case PositionValue::Sticky: return "sticky";
    }
    return "static";
}

const char* justifyStr(JustifyAlign j) {
    switch (j) {
        case JustifyAlign::Start: return "start";
        case JustifyAlign::End: return "end";
        case JustifyAlign::Center: return "center";
        case JustifyAlign::SpaceBetween: return "space-between";
        case JustifyAlign::SpaceAround: return "space-around";
        case JustifyAlign::SpaceEvenly: return "space-evenly";
        case JustifyAlign::Stretch: return "stretch";
        case JustifyAlign::FlexStart: return "flex-start";
        case JustifyAlign::FlexEnd: return "flex-end";
        case JustifyAlign::Baseline: return "baseline";
        case JustifyAlign::Auto: return "auto";
    }
    return "auto";
}

const char* overflowStr(OverflowValue o) {
    switch (o) {
        case OverflowValue::Visible: return "visible";
        case OverflowValue::Hidden: return "hidden";
        case OverflowValue::Scroll: return "scroll";
        case OverflowValue::Auto: return "auto";
        case OverflowValue::Clip: return "clip";
    }
    return "visible";
}

const char* textAlignStr(TextAlign t) {
    switch (t) {
        case TextAlign::Left: return "left";
        case TextAlign::Right: return "right";
        case TextAlign::Center: return "center";
        case TextAlign::Justify: return "justify";
        case TextAlign::Start: return "start";
        case TextAlign::End: return "end";
    }
    return "start";
}

std::vector<std::pair<std::string, std::string>> computedProperties(const CSSComputedStyle& s) {
    std::vector<std::pair<std::string, std::string>> out;
    auto add = [&](const char* n, const std::string& v) { out.emplace_back(n, v); };
    add("display", displayStr(s.display));
    add("position", positionStr(s.position));
    add("visibility", s.visibility == Visibility::Visible ? "visible" :
                      s.visibility == Visibility::Hidden ? "hidden" : "collapse");
    add("box-sizing", s.boxSizing == BoxSizing::BorderBox ? "border-box" : "content-box");
    add("opacity", fmtPx(s.opacity));
    add("float", s.cssFloat == 1 ? "left" : s.cssFloat == 2 ? "right" : "none");
    add("width", lengthStr(s.width));
    add("height", lengthStr(s.height));
    add("min-width", lengthStr(s.minWidth));
    add("min-height", lengthStr(s.minHeight));
    add("max-width", lengthStr(s.maxWidth));
    add("max-height", lengthStr(s.maxHeight));
    add("margin-top", lengthStr(s.marginTop));
    add("margin-right", lengthStr(s.marginRight));
    add("margin-bottom", lengthStr(s.marginBottom));
    add("margin-left", lengthStr(s.marginLeft));
    add("padding-top", lengthStr(s.paddingTop));
    add("padding-right", lengthStr(s.paddingRight));
    add("padding-bottom", lengthStr(s.paddingBottom));
    add("padding-left", lengthStr(s.paddingLeft));
    add("border-top-width", fmtPx(s.borderTopWidth) + "px");
    add("border-right-width", fmtPx(s.borderRightWidth) + "px");
    add("border-bottom-width", fmtPx(s.borderBottomWidth) + "px");
    add("border-left-width", fmtPx(s.borderLeftWidth) + "px");
    add("border-top-color", colorStr(s.borderTopColor));
    add("border-radius", fmtPx(s.borderTopLeftRadius) + "px");
    add("overflow-x", overflowStr(s.overflowX));
    add("overflow-y", overflowStr(s.overflowY));
    add("top", lengthStr(s.top));
    add("right", lengthStr(s.right));
    add("bottom", lengthStr(s.bottom));
    add("left", lengthStr(s.left));
    add("z-index", s.zIndexAuto ? "auto" : std::to_string(s.zIndex));
    if (s.display == DisplayValue::Flex || s.display == DisplayValue::InlineFlex) {
        const char* dir = s.flexDirection == FlexDirection::Row ? "row" :
                          s.flexDirection == FlexDirection::RowReverse ? "row-reverse" :
                          s.flexDirection == FlexDirection::Column ? "column" : "column-reverse";
        add("flex-direction", dir);
        add("flex-wrap", s.flexWrap ? (s.wrapReverse ? "wrap-reverse" : "wrap") : "nowrap");
        add("justify-content", justifyStr(s.justifyContent));
        add("align-items", justifyStr(s.alignItems));
        add("align-content", justifyStr(s.alignContent));
        add("gap", lengthStr(s.gap));
    }
    add("align-self", justifyStr(s.alignSelf));
    add("flex-grow", fmtPx(s.flexGrow));
    add("flex-shrink", fmtPx(s.flexShrink));
    add("flex-basis", lengthStr(s.flexBasis));
    if (s.order) add("order", std::to_string(s.order));
    if (!s.gridTemplateColumns.empty()) add("grid-template-columns", s.gridTemplateColumns);
    if (!s.gridTemplateRows.empty()) add("grid-template-rows", s.gridTemplateRows);
    add("color", colorStr(s.color));
    add("background-color", colorStr(s.backgroundColor));
    if (!s.backgroundImage.empty()) add("background-image", s.backgroundImage);
    add("font-family", s.fontFamily);
    add("font-size", fmtPx(s.fontSize) + "px");
    add("font-weight", std::to_string((int)s.fontWeight));
    add("font-style", s.fontStyle == FontStyle::Normal ? "normal" :
                      s.fontStyle == FontStyle::Italic ? "italic" : "oblique");
    add("line-height", s.lineHeight > 4 ? fmtPx(s.lineHeight) + "px" : fmtPx(s.lineHeight));
    add("text-align", textAlignStr(s.textAlign));
    if (!s.textDecoration.empty()) add("text-decoration", s.textDecoration);
    if (!s.textTransform.empty()) add("text-transform", s.textTransform);
    if (s.letterSpacing != 0) add("letter-spacing", fmtPx(s.letterSpacing) + "px");
    add("white-space", s.whiteSpace);
    add("vertical-align", s.verticalAlign);
    add("cursor", s.cursor);
    add("pointer-events", s.pointerEvents);
    if (!s.boxShadow.empty()) add("box-shadow", s.boxShadow);
    if (!s.transform.empty()) add("transform", s.transform);
    if (!s.transition.empty()) add("transition", s.transition);
    return out;
}

LayoutBox* findBoxForElement(LayoutBox* box, const DOMElement* element) {
    if (!box || !element) return nullptr;
    if (box->domElement == element && !box->anonymous) return box;
    for (auto& child : box->children) {
        if (LayoutBox* found = findBoxForElement(&child, element)) return found;
    }
    return nullptr;
}

// Children the tree shows: elements, comments and non-blank text
std::vector<DOMNode*> shownChildren(const DOMNode* node) {
    std::vector<DOMNode*> out;
    for (const auto& child : node->childNodes()) {
        DOMNode* c = child.get();
        if (c->nodeType() == NodeType::Text) {
            if (collapseText(static_cast<DOMText*>(c)->data()).empty()) continue;
        } else if (c->nodeType() != NodeType::Element && c->nodeType() != NodeType::Comment) {
            continue;
        }
        out.push_back(c);
    }
    return out;
}

// An element drawn on one row: no shown children, or a single text child
bool isLeafRow(const DOMNode* element) {
    auto kids = shownChildren(element);
    return kids.empty() || (kids.size() == 1 && kids[0]->nodeType() == NodeType::Text);
}

std::string elementSelector(const DOMElement* el) {
    std::string s = lower(el->tagName());
    std::string id = el->getAttribute("id");
    if (!id.empty()) s += "#" + id;
    for (const auto& cls : el->classList()) s += "." + cls;
    return s;
}

std::vector<std::pair<std::string, std::string>> sortedAttributes(const DOMElement* el) {
    std::vector<std::pair<std::string, std::string>> attrs(el->attributes().begin(), el->attributes().end());
    std::sort(attrs.begin(), attrs.end(), [](const auto& a, const auto& b) {
        auto rank = [](const std::string& n) { return n == "id" ? 0 : n == "class" ? 1 : 2; };
        int ra = rank(a.first), rb = rank(b.first);
        return ra != rb ? ra < rb : a.first < b.first;
    });
    return attrs;
}

std::string urlHost(const std::string& url) {
    size_t p = url.find("://");
    if (p == std::string::npos) return "";
    size_t start = p + 3;
    size_t end = url.find_first_of("/?#", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string urlFile(const std::string& url) {
    std::string path = url;
    size_t q = path.find_first_of("?#");
    if (q != std::string::npos) path = path.substr(0, q);
    size_t p = path.find("://");
    size_t slash = path.rfind('/');
    if (slash == std::string::npos || (p != std::string::npos && slash < p + 3)) return path;
    std::string file = path.substr(slash + 1);
    return file.empty() ? "/" : file;
}

const char* resourceTypeStr(zepra::NetworkResourceType t) {
    using T = zepra::NetworkResourceType;
    switch (t) {
        case T::DOCUMENT: return "html";
        case T::STYLESHEET: return "css";
        case T::SCRIPT: return "js";
        case T::IMAGE: return "img";
        case T::FONT: return "font";
        case T::XHR: return "xhr";
        case T::FETCH: return "fetch";
        case T::WEBSOCKET: return "ws";
        case T::MEDIA: return "media";
        default: return "other";
    }
}

float clampScroll(float v, float max) { return std::max(0.0f, std::min(v, std::max(0.0f, max))); }

} // namespace

bool DevToolsPanel::Rect::contains(float px, float py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
}

DevToolsPanel::DevToolsPanel() = default;

void DevToolsPanel::hide() {
    visible_ = false;
    picker_ = false;
    pickHover_ = nullptr;
    consoleFocused_ = false;
    resizing_ = false;
}

void DevToolsPanel::toggle() {
    if (visible_) hide(); else show();
}

bool DevToolsPanel::contains(float x, float y) const {
    return visible_ && panel_.contains(x, y);
}

void DevToolsPanel::selectNode(DOMNode* node) {
    selected_ = node;
    for (DOMNode* p = node ? node->parentNode() : nullptr; p; p = p->parentNode()) expanded_.insert(p);
    scrollSelectedIntoView_ = true;
    tab_ = PanelTab::Inspector;
    visible_ = true;
}

void DevToolsPanel::pickerSelect(DOMElement* element) {
    picker_ = false;
    pickHover_ = nullptr;
    if (element) selectNode(element);
}

DOMElement* DevToolsPanel::elementForStyles() const {
    DOMNode* n = selected_;
    while (n && n->nodeType() != NodeType::Element) n = n->parentNode();
    return static_cast<DOMElement*>(n);
}

// Forgets state that belonged to a previous document; drops nodes a script removed
void DevToolsPanel::syncWithDocument(const Page& page) {
    if (page.document != document_) {
        document_ = page.document;
        expanded_.clear();
        selected_ = nullptr;
        hoveredRow_ = nullptr;
        pickHover_ = nullptr;
        treeScroll_ = 0;
        stylesScroll_ = 0;
        networkSelected_ = -1;
        if (document_) {
            if (DOMElement* html = document_->documentElement()) expanded_.insert(html);
            if (DOMElement* body = document_->body()) expanded_.insert(body);
        }
        domVersion_ = page.domVersion;
    }
    if (page.domVersion != domVersion_) {
        domVersion_ = page.domVersion;
        if (selected_ && (!document_ || !document_->contains(selected_))) selected_ = nullptr;
        if (hoveredRow_ && (!document_ || !document_->contains(hoveredRow_))) hoveredRow_ = nullptr;
        if (pickHover_ && (!document_ || !document_->contains(pickHover_))) pickHover_ = nullptr;
    }
}

void DevToolsPanel::rebuildRows(const Page& page) {
    rows_.clear();
    if (!page.document) return;
    std::function<void(DOMNode*, int)> visit = [&](DOMNode* node, int depth) {
        rows_.push_back({node, depth, false});
        if (node->nodeType() != NodeType::Element) return;
        if (isLeafRow(node) || !expanded_.count(node)) return;
        for (DOMNode* child : shownChildren(node)) visit(child, depth + 1);
        rows_.push_back({node, depth, true});
    };
    for (DOMNode* child : shownChildren(page.document)) visit(child, 0);
}

// ============================================================================
// Drawing
// ============================================================================

void DevToolsPanel::draw(const Painter& p, const Page& page, float x, float y, float w, float h,
                         float mouseX, float mouseY, bool mouseDown) {
    if (!visible_) return;
    syncWithDocument(page);
    caretBlink_++;

    // Resize by dragging the top edge
    if (resizing_) {
        if (!mouseDown) {
            resizing_ = false;
        } else {
            float bottom = y + h;
            height_ = std::max(kMinHeight, std::min(bottom - mouseY + resizeGrabOffset_, bottom - 160));
        }
    }

    panel_ = {x, y, w, h};
    p.rect(x, y, w, h, kBg, 255);
    bool gripHover = mouseX >= x && mouseX < x + w && mouseY >= y && mouseY < y + kResizeGrip;
    p.rect(x, y, w, 1, (gripHover || resizing_) ? kAccent : kBorder, 255);

    drawToolbar(p, page, mouseX, mouseY);

    Rect content{x, y + kToolbarH, w, h - kToolbarH};
    p.pushClip(content.x, content.y, content.w, content.h);
    switch (tab_) {
        case PanelTab::Inspector: drawInspector(p, page, content, mouseX, mouseY); break;
        case PanelTab::Console: drawConsole(p, page, content, mouseX, mouseY); break;
        case PanelTab::Network: drawNetwork(p, page, content, mouseX, mouseY); break;
    }
    p.popClip();
}

void DevToolsPanel::drawToolbar(const Painter& p, const Page& page, float mouseX, float mouseY) {
    toolbar_ = {panel_.x, panel_.y + 1, panel_.w, kToolbarH - 1};
    p.rect(toolbar_.x, toolbar_.y, toolbar_.w, toolbar_.h, kToolbar, 255);
    p.rect(toolbar_.x, toolbar_.y + toolbar_.h - 1, toolbar_.w, 1, kBorder, 255);

    // Element picker button: a cursor arrow over a box
    pickerButton_ = {panel_.x + 6, toolbar_.y + 4, 26, kToolbarH - 9};
    bool pickerHover = pickerButton_.contains(mouseX, mouseY);
    if (picker_ || pickerHover) {
        p.rect(pickerButton_.x, pickerButton_.y, pickerButton_.w, pickerButton_.h, picker_ ? kAccent : kHover, 255);
    }
    {
        uint32_t ic = picker_ ? kBg : kMuted;
        float bx = pickerButton_.x + 6, by = pickerButton_.y + 4;
        p.rect(bx, by, 11, 1, ic, 255); p.rect(bx, by + 10, 7, 1, ic, 255);
        p.rect(bx, by, 1, 11, ic, 255); p.rect(bx + 10, by, 1, 7, ic, 255);
        p.triangle(bx + 7, by + 6, bx + 7, by + 15, bx + 13, by + 11, ic);
    }
    p.rect(pickerButton_.x + pickerButton_.w + 6, toolbar_.y + 6, 1, toolbar_.h - 12, kBorder, 255);

    // Tabs
    int errorCount = 0;
    if (page.consoleLog) {
        for (const auto& e : *page.consoleLog) if (e.rfind("[error]", 0) == 0) errorCount++;
    }
    const char* names[] = {"Inspector", "Console", "Network"};
    tabButtons_.clear();
    float tx = pickerButton_.x + pickerButton_.w + 14;
    for (int i = 0; i < 3; i++) {
        bool active = (int)tab_ == i;
        float labelW = p.textWidth(names[i], kFont);
        std::string badge;
        float badgeW = 0;
        if (i == 1 && errorCount > 0) {
            badge = std::to_string(errorCount);
            badgeW = p.textWidth(badge, kSmallFont) + 10;
        }
        Rect r{tx, toolbar_.y, labelW + 20 + badgeW + (badgeW > 0 ? 4 : 0), toolbar_.h};
        tabButtons_.push_back(r);
        if (!active && r.contains(mouseX, mouseY)) p.rect(r.x, r.y, r.w, r.h - 1, kHover, 255);
        p.text(names[i], r.x + 10, r.y + 19, active ? kText : kMuted, kFont);
        if (badgeW > 0) {
            float bx = r.x + 10 + labelW + 6;
            p.rect(bx, r.y + 8, badgeW, 14, 0xE22850, 255);
            p.text(badge, bx + 5, r.y + 19, kText, kSmallFont);
        }
        if (active) p.rect(r.x, r.y + r.h - 3, r.w, 2, kAccent, 255);
        tx += r.w;
    }

    // Close button
    closeButton_ = {panel_.x + panel_.w - 30, toolbar_.y + 4, 24, kToolbarH - 9};
    if (closeButton_.contains(mouseX, mouseY))
        p.rect(closeButton_.x, closeButton_.y, closeButton_.w, closeButton_.h, kHover, 255);
    float cx = closeButton_.x + closeButton_.w / 2, cy = closeButton_.y + closeButton_.h / 2;
    p.line(cx - 4, cy - 4, cx + 4, cy + 4, kMuted);
    p.line(cx - 4, cy + 4, cx + 4, cy - 4, kMuted);
}

// ---------------------------------------------------------------------------
// Inspector
// ---------------------------------------------------------------------------

void DevToolsPanel::drawInspector(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY) {
    float treeW = std::floor(area.w * 0.55f);
    tree_ = {area.x, area.y, treeW, area.h};
    styles_ = {area.x + treeW, area.y, area.w - treeW, area.h};
    if (!page.document) {
        p.text("No document", area.x + 12, area.y + 24, kMuted, kFont);
        return;
    }
    drawTree(p, page, tree_, mouseX, mouseY);
    p.rect(styles_.x, styles_.y, 1, styles_.h, kBorder, 255);
    drawStyles(p, page, styles_);
}

void DevToolsPanel::drawTree(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY) {
    rebuildRows(page);
    treeMax_ = rows_.size() * kRowH - area.h + 8;
    if (scrollSelectedIntoView_) {
        scrollSelectedIntoView_ = false;
        for (size_t i = 0; i < rows_.size(); i++) {
            if (rows_[i].node == selected_ && !rows_[i].closing) {
                float rowTop = i * kRowH;
                if (rowTop < treeScroll_) treeScroll_ = rowTop;
                else if (rowTop + kRowH > treeScroll_ + area.h) treeScroll_ = rowTop + kRowH - area.h + kRowH;
                break;
            }
        }
    }
    treeScroll_ = clampScroll(treeScroll_, treeMax_);

    p.pushClip(area.x, area.y, area.w, area.h);
    hoveredRow_ = nullptr;
    bool mouseInTree = area.contains(mouseX, mouseY);
    float baseY = area.y + 2 - treeScroll_;
    for (size_t i = 0; i < rows_.size(); i++) {
        float ry = baseY + i * kRowH;
        if (ry + kRowH < area.y || ry > area.y + area.h) continue;
        const Row& row = rows_[i];
        bool selected = row.node == selected_;
        bool hovered = mouseInTree && mouseY >= ry && mouseY < ry + kRowH;
        if (hovered) hoveredRow_ = row.node;
        if (selected) p.rect(area.x, ry, area.w, kRowH, kSelection, 255);
        else if (hovered) p.rect(area.x, ry, area.w, kRowH, kHover, 255);

        float x = area.x + 8 + row.depth * kIndent;
        float baseline = ry + 13;
        auto put = [&](const std::string& s, uint32_t c) {
            p.text(s, x, baseline, c, kFont);
            x += p.textWidth(s, kFont);
        };

        if (row.node->nodeType() == NodeType::Text) {
            put(truncate(collapseText(static_cast<DOMText*>(row.node)->data()), 120), kText);
            continue;
        }
        if (row.node->nodeType() == NodeType::Comment) {
            put("<!-- " + truncate(collapseText(static_cast<DOMComment*>(row.node)->data()), 100) + " -->", kComment);
            continue;
        }
        auto* el = static_cast<DOMElement*>(row.node);
        std::string tag = lower(el->tagName());
        if (row.closing) {
            put("</" + tag + ">", kTag);
            continue;
        }
        bool leaf = isLeafRow(el);
        if (!leaf) {
            bool open = expanded_.count(el) > 0;
            float ax = x + 3, ay = ry + kRowH / 2;
            if (open) p.triangle(ax - 1, ay - 2, ax + 7, ay - 2, ax + 3, ay + 3, kMuted);
            else p.triangle(ax, ay - 4, ax, ay + 4, ax + 5, ay, kMuted);
        }
        x += 12;
        put("<" + tag, kTag);
        for (const auto& [name, value] : sortedAttributes(el)) {
            put(" " + name, kAttrName);
            put("=\"", kTag);
            put(truncate(value, 60), kAttrValue);
            put("\"", kTag);
        }
        put(">", kTag);
        if (leaf) {
            auto kids = shownChildren(el);
            if (!kids.empty())
                put(truncate(collapseText(static_cast<DOMText*>(kids[0])->data()), 80), kText);
            put("</" + tag + ">", kTag);
        } else if (!expanded_.count(el)) {
            put("...", kMuted);
            put("</" + tag + ">", kTag);
        }
    }
    p.popClip();

    // Scrollbar
    if (treeMax_ > 0) {
        float total = rows_.size() * kRowH + 8;
        float thumbH = std::max(20.0f, area.h * area.h / total);
        float thumbY = area.y + (area.h - thumbH) * (treeScroll_ / treeMax_);
        p.rect(area.x + area.w - 8, thumbY, 6, thumbH, kBorder, 255);
    }
}

void DevToolsPanel::drawStyles(const Painter& p, const Page& page, const Rect& area) {
    DOMElement* el = elementForStyles();
    float x0 = area.x + 12;
    if (!el) {
        p.text("Select an element to see its layout and computed styles.", x0, area.y + 24, kMuted, kFont);
        return;
    }

    // Selector header
    p.rect(area.x + 1, area.y, area.w - 1, 24, kToolbar, 255);
    p.text(elementSelector(el), x0, area.y + 16, kTag, kFont);
    LayoutBox* box = page.layoutRoot ? findBoxForElement(page.layoutRoot, el) : nullptr;
    if (box) {
        std::string size = fmtPx(box->width) + " x " + fmtPx(box->height);
        p.text(size, area.x + area.w - 12 - p.textWidth(size, kSmallFont), area.y + 16, kMuted, kSmallFont);
    }

    float y = area.y + 24 - stylesScroll_;
    p.pushClip(area.x + 1, area.y + 24, area.w - 1, area.h - 24);

    // Box model
    p.text("Layout", x0, y + 18, kMuted, kSmallFont);
    y += 26;
    const float diagramH = 128;
    if (!box) {
        p.text("No layout box (display: none, or not laid out)", x0, y + 14, kMuted, kSmallFont);
        y += 30;
    } else {
        float dw = std::min(area.w - 24, 300.0f);
        float dx = x0, dy = y;
        const float band = 20;
        auto layer = [&](float inset, uint32_t color, uint8_t alpha) {
            p.rect(dx + inset, dy + inset, dw - 2 * inset, diagramH - 2 * inset, color, alpha);
        };
        layer(0, kMarginColor, 70);
        layer(band, kBorderColor, 90);
        layer(band * 2, kPaddingColor, 90);
        layer(band * 3, kContentColor, 110);
        auto centered = [&](const std::string& s, float cx, float baseline, uint32_t c) {
            p.text(s, cx - p.textWidth(s, kSmallFont) / 2, baseline, c, kSmallFont);
        };
        auto edgeLabels = [&](float inset, float t, float r, float b, float l) {
            float cx = dx + dw / 2, cy = dy + diagramH / 2;
            centered(fmtPx(t), cx, dy + inset + 14, kText);
            centered(fmtPx(b), cx, dy + diagramH - inset - 6, kText);
            centered(fmtPx(l), dx + inset + band / 2, cy + 4, kText);
            centered(fmtPx(r), dx + dw - inset - band / 2, cy + 4, kText);
        };
        p.text("margin", dx + 4, dy + 12, 0x2B1D0B, 9);
        p.text("border", dx + band + 4, dy + band + 12, 0x2B1D0B, 9);
        p.text("padding", dx + band * 2 + 4, dy + band * 2 + 12, 0x0F2B0B, 9);
        edgeLabels(0, box->marginTop, box->marginRight, box->marginBottom, box->marginLeft);
        edgeLabels(band, box->borderTop, box->borderRight, box->borderBottom, box->borderLeft);
        edgeLabels(band * 2, box->paddingTop, box->paddingRight, box->paddingBottom, box->paddingLeft);
        float contentW = box->width - box->borderLeft - box->borderRight - box->paddingLeft - box->paddingRight;
        float contentH = box->height - box->borderTop - box->borderBottom - box->paddingTop - box->paddingBottom;
        centered(fmtPx(contentW) + " x " + fmtPx(contentH), dx + dw / 2, dy + diagramH / 2 + 4, kBg);
        y += diagramH + 12;
    }

    // Computed styles
    p.rect(area.x + 1, y, area.w - 1, 1, kBorder, 255);
    y += 6;
    p.text("Computed", x0, y + 14, kMuted, kSmallFont);
    y += 22;
    const CSSComputedStyle* style = page.css ? page.css->getComputedStyle(el) : nullptr;
    if (!style) {
        p.text("No computed style", x0, y + 12, kMuted, kSmallFont);
        y += kRowH;
    } else {
        auto props = computedProperties(*style);
        float nameW = 0;
        for (const auto& pr : props) nameW = std::max(nameW, p.textWidth(pr.first, kSmallFont));
        nameW = std::min(nameW + 16, area.w * 0.45f);
        for (const auto& [name, value] : props) {
            if (y + 16 >= area.y && y <= area.y + area.h) {
                p.text(name, x0, y + 12, kAttrName, kSmallFont);
                p.text(truncate(value, 60), x0 + nameW, y + 12, kText, kSmallFont);
            }
            y += 16;
        }
    }
    p.popClip();
    stylesMax_ = (y + stylesScroll_) - (area.y + area.h) + 8;
    stylesScroll_ = clampScroll(stylesScroll_, stylesMax_);
}

void DevToolsPanel::drawPageOverlay(const Painter& p, const Page& page, float clipX, float clipY, float clipW, float clipH) {
    if (!visible_ || !page.layoutRoot) return;
    DOMElement* hover = nullptr;
    if (picker_) hover = pickHover_;
    else if (hoveredRow_ && tab_ == PanelTab::Inspector) {
        DOMNode* n = hoveredRow_;
        while (n && n->nodeType() != NodeType::Element) n = n->parentNode();
        hover = static_cast<DOMElement*>(n);
    }
    DOMElement* selected = tab_ == PanelTab::Inspector ? elementForStyles() : nullptr;
    if (!hover && !selected) return;

    p.pushClip(clipX, clipY, clipW, clipH);
    auto paint = [&](DOMElement* el, bool strong) {
        LayoutBox* b = findBoxForElement(page.layoutRoot, el);
        if (!b) return;
        uint8_t a = strong ? 120 : 80;
        float x = b->screenX, y = b->screenY, w = b->width, h = b->height;
        // margin bands (outside the border box)
        p.rect(x - b->marginLeft, y - b->marginTop, w + b->marginLeft + b->marginRight, b->marginTop, kMarginColor, a);
        p.rect(x - b->marginLeft, y + h, w + b->marginLeft + b->marginRight, b->marginBottom, kMarginColor, a);
        p.rect(x - b->marginLeft, y, b->marginLeft, h, kMarginColor, a);
        p.rect(x + w, y, b->marginRight, h, kMarginColor, a);
        // border bands
        p.rect(x, y, w, b->borderTop, kBorderColor, a);
        p.rect(x, y + h - b->borderBottom, w, b->borderBottom, kBorderColor, a);
        p.rect(x, y, b->borderLeft, h, kBorderColor, a);
        p.rect(x + w - b->borderRight, y, b->borderRight, h, kBorderColor, a);
        // padding bands
        float px = x + b->borderLeft, py = y + b->borderTop;
        float pw = w - b->borderLeft - b->borderRight, ph = h - b->borderTop - b->borderBottom;
        p.rect(px, py, pw, b->paddingTop, kPaddingColor, a);
        p.rect(px, py + ph - b->paddingBottom, pw, b->paddingBottom, kPaddingColor, a);
        p.rect(px, py, b->paddingLeft, ph, kPaddingColor, a);
        p.rect(px + pw - b->paddingRight, py, b->paddingRight, ph, kPaddingColor, a);
        // content
        p.rect(px + b->paddingLeft, py + b->paddingTop, pw - b->paddingLeft - b->paddingRight,
               ph - b->paddingTop - b->paddingBottom, kContentColor, a);
        if (strong) {
            // 1px outline so a zero-size or transparent box still shows
            p.rect(x, y, w, 1, kAccent, 255); p.rect(x, y + h - 1, w, 1, kAccent, 255);
            p.rect(x, y, 1, h, kAccent, 255); p.rect(x + w - 1, y, 1, h, kAccent, 255);
        }
        if (picker_ && el == hover) {
            std::string label = elementSelector(el);
            std::string size = fmtPx(w) + " x " + fmtPx(h);
            float lw = p.textWidth(label, kFont), sw = p.textWidth(size, kSmallFont);
            float tw = lw + sw + 24, th = 22;
            float tx = std::max(clipX, std::min(x, clipX + clipW - tw));
            float ty = y - th - 4 >= clipY ? y - th - 4 : y + h + 4;
            p.rect(tx, ty, tw, th, kToolbar, 240);
            p.rect(tx, ty, tw, 1, kBorder, 255);
            p.text(label, tx + 8, ty + 15, kTag, kFont);
            p.text(size, tx + 16 + lw, ty + 15, kMuted, kSmallFont);
        }
    };
    if (selected) paint(selected, true);
    if (hover && hover != selected) paint(hover, false);
    p.popClip();
}

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------

namespace {
enum class Level { Log, Warn, Error, Input, Result };

struct ConsoleLine { Level level; std::string text; };

ConsoleLine parseConsoleLine(const std::string& e) {
    if (e.rfind("[error]", 0) == 0) return {Level::Error, e.substr(8)};
    if (e.rfind("[warn]", 0) == 0) return {Level::Warn, e.substr(7)};
    if (e.rfind("[log]", 0) == 0) return {Level::Log, e.substr(6)};
    if (e.rfind("[info]", 0) == 0) return {Level::Log, e.substr(7)};
    if (e.rfind("> ", 0) == 0) return {Level::Input, e.substr(2)};
    if (e.rfind("< ", 0) == 0) return {Level::Result, e.substr(2)};
    return {Level::Log, e};
}
} // namespace

void DevToolsPanel::drawConsole(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY) {
    const float inputH = 28, barH = 26;
    consoleToolbar_ = {area.x, area.y, area.w, barH};
    consoleList_ = {area.x, area.y + barH, area.w, area.h - barH - inputH};
    consoleInputBox_ = {area.x, area.y + area.h - inputH, area.w, inputH};

    // Toolbar: clear, level filters with counts
    p.rect(consoleToolbar_.x, consoleToolbar_.y, consoleToolbar_.w, consoleToolbar_.h, kToolbar, 255);
    p.rect(consoleToolbar_.x, consoleToolbar_.y + barH - 1, consoleToolbar_.w, 1, kBorder, 255);
    int nErr = 0, nWarn = 0, nLog = 0;
    std::vector<ConsoleLine> lines;
    if (page.consoleLog) {
        lines.reserve(page.consoleLog->size());
        for (const auto& e : *page.consoleLog) {
            ConsoleLine l = parseConsoleLine(e);
            if (l.level == Level::Error) nErr++;
            else if (l.level == Level::Warn) nWarn++;
            else if (l.level == Level::Log) nLog++;
            lines.push_back(std::move(l));
        }
    }
    clearButton_ = {area.x + 8, area.y + 4, p.textWidth("Clear", kSmallFont) + 16, barH - 8};
    p.rect(clearButton_.x, clearButton_.y, clearButton_.w, clearButton_.h,
           clearButton_.contains(mouseX, mouseY) ? kHover : kBg, 255);
    p.text("Clear", clearButton_.x + 8, clearButton_.y + 14, kText, kSmallFont);

    levelButtons_.clear();
    struct Filter { const char* name; int count; bool* on; uint32_t color; };
    Filter filters[] = {{"Errors", nErr, &showErrors_, kErrorText},
                        {"Warnings", nWarn, &showWarnings_, kWarnText},
                        {"Logs", nLog, &showLogs_, kText}};
    float fx = area.x + area.w - 8;
    for (int i = 2; i >= 0; i--) {
        std::string label = std::string(filters[i].name) + " " + std::to_string(filters[i].count);
        float lw = p.textWidth(label, kSmallFont) + 16;
        Rect r{fx - lw, area.y + 4, lw, barH - 8};
        p.rect(r.x, r.y, r.w, r.h, *filters[i].on ? kBg : kToolbar, 255);
        if (r.contains(mouseX, mouseY)) p.rect(r.x, r.y, r.w, r.h, kHover, 255);
        p.text(label, r.x + 8, r.y + 14, *filters[i].on ? filters[i].color : kMuted, kSmallFont);
        levelButtons_.insert(levelButtons_.begin(), r);
        fx -= lw + 6;
    }

    // Entries
    std::vector<const ConsoleLine*> shown;
    for (const auto& l : lines) {
        if ((l.level == Level::Error && !showErrors_) || (l.level == Level::Warn && !showWarnings_) ||
            (l.level == Level::Log && !showLogs_)) continue;
        shown.push_back(&l);
    }
    const float lineH = 18;
    consoleMax_ = shown.size() * lineH - consoleList_.h + 4;
    if (page.consoleLog && page.consoleLog->size() != consoleSeen_) {
        consoleSeen_ = page.consoleLog->size();
        if (consoleStick_) consoleScroll_ = consoleMax_;
    }
    if (consoleStick_) consoleScroll_ = consoleMax_;
    consoleScroll_ = clampScroll(consoleScroll_, consoleMax_);

    p.pushClip(consoleList_.x, consoleList_.y, consoleList_.w, consoleList_.h);
    if (shown.empty()) {
        p.text("Page console output and uncaught errors appear here.", area.x + 12, consoleList_.y + 22, kMuted, kFont);
    }
    float ly = consoleList_.y + 2 - consoleScroll_;
    for (const ConsoleLine* l : shown) {
        if (ly + lineH >= consoleList_.y && ly <= consoleList_.y + consoleList_.h) {
            uint32_t color = kText, bg = 0; bool hasBg = false;
            switch (l->level) {
                case Level::Error: color = kErrorText; bg = kErrorBg; hasBg = true; break;
                case Level::Warn: color = kWarnText; bg = kWarnBg; hasBg = true; break;
                case Level::Input: color = kInputText; break;
                case Level::Result: color = kResultText; break;
                default: break;
            }
            if (hasBg) p.rect(consoleList_.x, ly, consoleList_.w, lineH, bg, 255);
            p.rect(consoleList_.x, ly + lineH - 1, consoleList_.w, 1, kBgAlt, 255);
            float gx = consoleList_.x + 10, gy = ly + lineH / 2;
            if (l->level == Level::Input) p.triangle(gx, gy - 4, gx, gy + 4, gx + 6, gy, kInputText);
            else if (l->level == Level::Result) p.triangle(gx + 6, gy - 4, gx + 6, gy + 4, gx, gy, kResultText);
            else if (l->level == Level::Error) p.rect(consoleList_.x, ly, 3, lineH, kErrorText, 255);
            else if (l->level == Level::Warn) p.rect(consoleList_.x, ly, 3, lineH, kWarnText, 255);
            p.text(truncate(l->text, 400), consoleList_.x + 24, ly + 13, color, kFont);
        }
        ly += lineH;
    }
    p.popClip();

    // Input line
    const Rect& ib = consoleInputBox_;
    p.rect(ib.x, ib.y, ib.w, ib.h, kBg, 255);
    p.rect(ib.x, ib.y, ib.w, 1, consoleFocused_ ? kAccent : kBorder, 255);
    float ix = ib.x + 10, iy = ib.y + ib.h / 2;
    p.triangle(ix, iy - 4, ix, iy + 4, ix + 6, iy, kAccent);
    float baseline = ib.y + 18;
    if (consoleInputText_.empty() && !consoleFocused_) {
        p.text("Evaluate JavaScript in the page", ib.x + 24, baseline, kMuted, kFont);
    } else {
        p.text(consoleInputText_, ib.x + 24, baseline, kText, kFont);
        if (consoleFocused_ && (caretBlink_ / 30) % 2 == 0) {
            float cx = ib.x + 24 + p.textWidth(consoleInputText_, kFont) + 1;
            p.rect(cx, ib.y + 7, 1, ib.h - 14, kText, 255);
        }
    }
}

void DevToolsPanel::evaluateConsoleInput(const Page& page) {
    std::string expr = consoleInputText_;
    if (expr.empty()) return;
    auto log = [&](const std::string& line) {
        if (!page.consoleLog) return;
        page.consoleLog->push_back(line);
        if (page.consoleLog->size() > 500) page.consoleLog->erase(page.consoleLog->begin());
    };
    consoleHistory_.push_back(expr);
    historyIndex_ = -1;
    consoleStick_ = true;
    log("> " + expr);
    if (expr == "clear()" || expr == "console.clear()") {
        if (page.consoleLog) page.consoleLog->clear();
    } else if (!page.script) {
        log("[error] No script context for this page");
    } else {
        ScriptResult r = page.script->evaluate(expr, "<devtools>");
        if (r.success) log("< " + (r.value.empty() ? std::string("undefined") : r.value));
        else log("[error] " + (r.error.empty() ? std::string("Evaluation failed") : r.error));
    }
    consoleInputText_.clear();
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

namespace {

// The type filter chips of the Network toolbar, in order
constexpr const char* kNetworkFilters[] = {"All", "HTML", "CSS", "JS", "Img", "XHR", "Other"};
constexpr int kNetworkFilterCount = 7;

bool networkFilterMatches(int filter, zepra::NetworkResourceType type) {
    using T = zepra::NetworkResourceType;
    switch (filter) {
        case 1: return type == T::DOCUMENT;
        case 2: return type == T::STYLESHEET;
        case 3: return type == T::SCRIPT;
        case 4: return type == T::IMAGE;
        case 5: return type == T::XHR || type == T::FETCH;
        case 6: return type == T::FONT || type == T::MEDIA || type == T::WEBSOCKET || type == T::OTHER;
        default: return true;
    }
}

} // namespace

void DevToolsPanel::drawNetwork(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY) {
    const float barH = 26, headH = 22, detailH = 22;
    networkToolbar_ = {area.x, area.y, area.w, barH};
    // A snapshot: loader threads append to the monitor while this draws
    std::vector<zepra::NetworkEntry> entries;
    if (page.network) entries = page.network->getEntries();
    std::vector<const zepra::NetworkEntry*> shown;
    shown.reserve(entries.size());
    for (const auto& e : entries) {
        if (networkFilterMatches(networkFilter_, e.type)) shown.push_back(&e);
    }

    p.rect(networkToolbar_.x, networkToolbar_.y, networkToolbar_.w, barH, kToolbar, 255);
    p.rect(networkToolbar_.x, networkToolbar_.y + barH - 1, networkToolbar_.w, 1, kBorder, 255);
    clearButton_ = {area.x + 8, area.y + 4, p.textWidth("Clear", kSmallFont) + 16, barH - 8};
    p.rect(clearButton_.x, clearButton_.y, clearButton_.w, clearButton_.h,
           clearButton_.contains(mouseX, mouseY) ? kHover : kBg, 255);
    p.text("Clear", clearButton_.x + 8, clearButton_.y + 14, kText, kSmallFont);

    // Type filter chips
    networkFilterButtons_.clear();
    float fx = clearButton_.x + clearButton_.w + 14;
    p.rect(fx - 7, area.y + 6, 1, barH - 12, kBorder, 255);
    for (int i = 0; i < kNetworkFilterCount; i++) {
        Rect b = {fx, area.y + 4, p.textWidth(kNetworkFilters[i], kSmallFont) + 14, barH - 8};
        bool active = i == networkFilter_;
        p.rect(b.x, b.y, b.w, b.h, active ? kSelection : b.contains(mouseX, mouseY) ? kHover : kToolbar, 255);
        p.text(kNetworkFilters[i], b.x + 7, b.y + 14, active ? kText : kMuted, kSmallFont);
        networkFilterButtons_.push_back(b);
        fx += b.w + 4;
    }

    // Summary: requests, bytes off the wire, first request start to last response end
    {
        size_t bytes = 0;
        int failed = 0;
        bool any = false;
        auto now = std::chrono::system_clock::now();
        std::chrono::system_clock::time_point first, last;
        for (const auto& e : entries) {
            bytes += e.response.transferred_bytes ? e.response.transferred_bytes : e.response.content_length;
            if (e.failed) failed++;
            auto start = e.request.start_time;
            auto end = e.completed ? e.response.end_time : now;
            if (!any || start < first) first = start;
            if (!any || end > last) last = end;
            any = true;
        }
        double totalMs = any ? std::chrono::duration<double, std::milli>(last - first).count() : 0;
        std::string summary;
        if (networkFilter_ != 0) summary += std::to_string(shown.size()) + " of ";
        summary += std::to_string(entries.size()) + (entries.size() == 1 && networkFilter_ == 0 ? " request" : " requests") +
            "   " + zepra::formatSize(bytes) + " transferred   " +
            (any ? zepra::formatDuration(std::max(totalMs, 1.0)) : std::string("0 ms"));
        if (failed) summary += "   " + std::to_string(failed) + " failed";
        float sw = p.textWidth(summary, kSmallFont);
        if (area.x + area.w - 12 - sw > fx + 8) {
            p.text(summary, area.x + area.w - 12 - sw, area.y + 17, kMuted, kSmallFont);
        }
    }

    // Columns
    struct Col { const char* name; float w; };
    float fixed = 56 + 60 + 180 + 70 + 80 + 76;
    Col cols[] = {{"Status", 56}, {"Method", 60}, {"File", std::max(140.0f, area.w - fixed - 8)},
                  {"Domain", 180}, {"Type", 70}, {"Size", 80}, {"Time", 76}};
    float headY = area.y + barH;
    p.rect(area.x, headY, area.w, headH, kBgAlt, 255);
    p.rect(area.x, headY + headH - 1, area.w, 1, kBorder, 255);
    float cx = area.x + 8;
    std::vector<float> colX;
    for (const auto& c : cols) {
        colX.push_back(cx);
        p.text(c.name, cx, headY + 15, kMuted, kSmallFont);
        cx += c.w;
    }

    bool hasDetail = networkSelected_ >= 0 && networkSelected_ < (int)shown.size();
    networkList_ = {area.x, headY + headH, area.w, area.h - barH - headH - (hasDetail ? detailH : 0)};
    networkMax_ = shown.size() * kRowH - networkList_.h + 4;
    networkScroll_ = clampScroll(networkScroll_, networkMax_);

    p.pushClip(networkList_.x, networkList_.y, networkList_.w, networkList_.h);
    if (entries.empty()) {
        p.text("No requests recorded for this tab yet.", area.x + 12, networkList_.y + 22, kMuted, kFont);
    } else if (shown.empty()) {
        p.text(std::string("No ") + kNetworkFilters[networkFilter_] + " requests.", area.x + 12, networkList_.y + 22, kMuted, kFont);
    }
    float ry = networkList_.y - networkScroll_;
    for (size_t i = 0; i < shown.size(); i++, ry += kRowH) {
        if (ry + kRowH < networkList_.y || ry > networkList_.y + networkList_.h) continue;
        const auto& e = *shown[i];
        bool selected = (int)i == networkSelected_;
        bool hovered = networkList_.contains(mouseX, mouseY) && mouseY >= ry && mouseY < ry + kRowH;
        p.rect(area.x, ry, area.w, kRowH, selected ? kSelection : hovered ? kHover : (i % 2 ? kBgAlt : kBg), 255);
        float baseline = ry + 13;
        int status = e.response.status_code;
        std::string statusStr = e.failed ? "(failed)" : status > 0 ? std::to_string(status) : "...";
        uint32_t statusColor = e.failed ? kErrorText : status == 0 ? kMuted :
                               status < 300 ? kOk : status < 400 ? kWarnText : kErrorText;
        p.text(statusStr, colX[0], baseline, statusColor, kSmallFont);
        p.text(e.request.method, colX[1], baseline, kText, kSmallFont);
        p.text(truncate(urlFile(e.request.url), (size_t)std::max(8.0f, cols[2].w / 6.5f)), colX[2], baseline, kText, kSmallFont);
        p.text(truncate(urlHost(e.request.url), 26), colX[3], baseline, kMuted, kSmallFont);
        p.text(resourceTypeStr(e.type), colX[4], baseline, kMuted, kSmallFont);
        p.text(e.response.content_length ? zepra::formatSize(e.response.content_length) : "-", colX[5], baseline, kMuted, kSmallFont);
        std::string timeStr = e.completed || e.failed ? zepra::formatDuration(std::max(e.response.duration_ms, 1.0)) : "pending";
        p.text(timeStr, colX[6], baseline, kMuted, kSmallFont);
    }
    p.popClip();

    if (hasDetail) {
        const auto& e = *shown[networkSelected_];
        float dy = area.y + area.h - detailH;
        p.rect(area.x, dy, area.w, detailH, kToolbar, 255);
        p.rect(area.x, dy, area.w, 1, kBorder, 255);
        std::string detail = e.request.method + " " + e.request.url;
        if (!e.response.content_type.empty()) detail += "   " + e.response.content_type;
        if (e.response.transferred_bytes) detail += "   " + zepra::formatSize(e.response.transferred_bytes) + " on the wire";
        if (e.failed && !e.error.empty()) detail += "   " + e.error;
        p.text(truncate(detail, 220), area.x + 12, dy + 15, e.failed ? kErrorText : kText, kSmallFont);
    }
}

// ============================================================================
// Input
// ============================================================================

bool DevToolsPanel::onMouseDown(float x, float y, const Page& page) {
    if (!visible_ || !panel_.contains(x, y)) return false;
    consoleFocused_ = false;
    if (y < panel_.y + kResizeGrip) {
        resizing_ = true;
        resizeGrabOffset_ = y - panel_.y;
        return true;
    }
    if (pickerButton_.contains(x, y)) { setPickerActive(!picker_); return true; }
    if (closeButton_.contains(x, y)) { hide(); return true; }
    for (size_t i = 0; i < tabButtons_.size(); i++) {
        if (tabButtons_[i].contains(x, y)) { setActiveTab((PanelTab)i); return true; }
    }
    switch (tab_) {
        case PanelTab::Inspector:
            if (tree_.contains(x, y)) {
                int idx = (int)std::floor((y - (tree_.y + 2) + treeScroll_) / kRowH);
                if (idx >= 0 && idx < (int)rows_.size()) {
                    const Row& row = rows_[idx];
                    float arrowEnd = tree_.x + 8 + row.depth * kIndent + 12;
                    bool element = row.node->nodeType() == NodeType::Element;
                    if (element && !row.closing && x < arrowEnd && !isLeafRow(row.node)) {
                        if (expanded_.count(row.node)) expanded_.erase(row.node);
                        else expanded_.insert(row.node);
                    } else {
                        selected_ = row.node;
                    }
                }
            }
            break;
        case PanelTab::Console:
            if (consoleInputBox_.contains(x, y)) consoleFocused_ = true;
            else if (clearButton_.contains(x, y)) { if (page.consoleLog) page.consoleLog->clear(); }
            else {
                bool* flags[] = {&showErrors_, &showWarnings_, &showLogs_};
                for (size_t i = 0; i < levelButtons_.size() && i < 3; i++) {
                    if (levelButtons_[i].contains(x, y)) *flags[i] = !*flags[i];
                }
            }
            break;
        case PanelTab::Network:
            if (clearButton_.contains(x, y)) {
                if (page.network) page.network->clear();
                networkSelected_ = -1;
                break;
            }
            for (size_t i = 0; i < networkFilterButtons_.size(); i++) {
                if (networkFilterButtons_[i].contains(x, y)) {
                    networkFilter_ = (int)i;
                    networkSelected_ = -1;
                    networkScroll_ = 0;
                    return true;
                }
            }
            if (networkList_.contains(x, y)) {
                int idx = (int)std::floor((y - networkList_.y + networkScroll_) / kRowH);
                networkSelected_ = idx >= 0 ? idx : -1;
            }
            break;
    }
    return true;
}

void DevToolsPanel::onMouseUp(float, float) {
    resizing_ = false;
}

bool DevToolsPanel::onMouseWheel(float x, float y, float delta) {
    if (!visible_ || !panel_.contains(x, y)) return false;
    float step = -delta * 40.0f;
    switch (tab_) {
        case PanelTab::Inspector:
            if (tree_.contains(x, y)) treeScroll_ = clampScroll(treeScroll_ + step, treeMax_);
            else if (styles_.contains(x, y)) stylesScroll_ = clampScroll(stylesScroll_ + step, stylesMax_);
            break;
        case PanelTab::Console:
            consoleScroll_ = clampScroll(consoleScroll_ + step, consoleMax_);
            consoleStick_ = consoleScroll_ >= consoleMax_ - 1;
            break;
        case PanelTab::Network:
            networkScroll_ = clampScroll(networkScroll_ + step, networkMax_);
            break;
    }
    return true;
}

bool DevToolsPanel::onKey(NXRender::KeyCode key, const std::string& text, bool ctrl, bool, const Page& page) {
    using NXRender::KeyCode;
    if (!visible_ || !consoleFocused_ || tab_ != PanelTab::Console) return false;
    if (ctrl || key == KeyCode::F12) return false;   // browser shortcuts (Ctrl+V pastes through insertConsoleText)
    std::string& input = consoleInputText_;
    switch (key) {
        case KeyCode::Enter:
            evaluateConsoleInput(page);
            return true;
        case KeyCode::Escape:
            consoleFocused_ = false;
            return true;
        case KeyCode::Backspace:
            if (!input.empty()) {
                // drop one UTF-8 code point
                do { input.pop_back(); } while (!input.empty() && ((unsigned char)input.back() & 0xC0) == 0x80);
            }
            return true;
        case KeyCode::Up:
            if (!consoleHistory_.empty()) {
                if (historyIndex_ < 0) historyIndex_ = (int)consoleHistory_.size() - 1;
                else if (historyIndex_ > 0) historyIndex_--;
                input = consoleHistory_[historyIndex_];
            }
            return true;
        case KeyCode::Down:
            if (historyIndex_ >= 0) {
                historyIndex_++;
                if (historyIndex_ >= (int)consoleHistory_.size()) { historyIndex_ = -1; input.clear(); }
                else input = consoleHistory_[historyIndex_];
            }
            return true;
        case KeyCode::Tab:
            return true;
        default:
            if (!text.empty()) input += text;
            else if (key == KeyCode::Space) input += ' ';
            return true;
    }
}

} // namespace ZepraBrowser::DevTools
