// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file devtools_panel.cpp
 * @brief Docked developer tools: Inspector (DOM tree, style rules, box model,
 *        computed styles, element picker), Console (page output, JS input
 *        with autocomplete, filter), Network (the tab's NetworkMonitor) and
 *        Sources (the scripts the Console links to).
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

// The longest prefix of `s` that fits in `maxW`, with "..." when cut
std::string fitText(const Painter& p, const std::string& s, float size, float maxW) {
    if (p.textWidth(s, size) <= maxW) return s;
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (p.textWidth(s.substr(0, mid) + "...", size) <= maxW) lo = mid; else hi = mid - 1;
    }
    return s.substr(0, lo) + "...";
}

// Every layout box of an element in `elements` (one walk of the box tree)
void collectBoxes(LayoutBox* box, const std::unordered_set<const DOMElement*>& elements,
                  std::vector<LayoutBox*>& out, size_t limit) {
    if (!box || out.size() >= limit) return;
    if (box->domElement && !box->anonymous && elements.count(box->domElement)) out.push_back(box);
    for (auto& child : box->children) collectBoxes(&child, elements, out, limit);
}

constexpr float kRuleLineH = 16;
constexpr size_t kMaxHighlighted = 2000;

} // namespace

bool DevToolsPanel::Rect::contains(float px, float py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
}

DevToolsPanel::DevToolsPanel() = default;

void DevToolsPanel::hide() {
    visible_ = false;
    picker_ = false;
    pickHover_ = nullptr;
    blurConsole();
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
        rulesScroll_ = 0;
        networkSelected_ = -1;
        styleEdits_.clear();
        highlightKey_ = nullptr;
        highlightSelector_.clear();
        highlighted_.clear();
        rulesElement_ = nullptr;
        rulesDirty_ = true;
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
        // An edit is dropped when its element left the page or a script rewrote its style attribute
        styleEdits_.erase(std::remove_if(styleEdits_.begin(), styleEdits_.end(), [&](const auto& e) {
            return !document_ || !document_->contains(e.first) || e.first->getAttribute("style") != e.second.writtenStyle;
        }), styleEdits_.end());
        refreshHighlight(page);
        rulesDirty_ = true;
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
    textWidth_ = p.textWidth;
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
        case PanelTab::Sources: drawSources(p, page, content, mouseX, mouseY); break;
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
        for (const auto& e : *page.consoleLog) if (e.level == "error") errorCount++;
    }
    const char* names[] = {"Inspector", "Console", "Network", "Sources"};
    tabButtons_.clear();
    float tx = pickerButton_.x + pickerButton_.w + 14;
    for (int i = 0; i < 4; i++) {
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
    // DOM tree | Rules | Layout and Computed, as Firefox's three-pane Inspector
    float treeW = std::floor(area.w * 0.42f);
    float rulesW = std::floor(area.w * 0.31f);
    tree_ = {area.x, area.y, treeW, area.h};
    rules_ = {area.x + treeW, area.y, rulesW, area.h};
    styles_ = {area.x + treeW + rulesW, area.y, area.w - treeW - rulesW, area.h};
    if (!page.document) {
        p.text("No document", area.x + 12, area.y + 24, kMuted, kFont);
        return;
    }
    drawTree(p, page, tree_, mouseX, mouseY);
    p.rect(rules_.x, rules_.y, 1, rules_.h, kBorder, 255);
    drawRules(p, page, rules_, mouseX, mouseY);
    p.rect(styles_.x, styles_.y, 1, styles_.h, kBorder, 255);
    drawStyles(p, page, styles_, mouseX, mouseY);
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

void DevToolsPanel::drawStyles(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY) {
    DOMElement* el = elementForStyles();
    float x0 = area.x + 12;
    computedHits_.clear();
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
        const ComputedHit* hovered = nullptr;
        for (const auto& [name, value] : props) {
            if (y + 16 >= area.y && y <= area.y + area.h) {
                Rect row{area.x + 1, y, area.w - 1, 16};
                computedHits_.push_back({row, name});
                if (row.contains(mouseX, mouseY) && mouseY >= area.y + 24) {
                    p.rect(row.x, row.y, row.w, row.h, kHover, 255);
                    hovered = &computedHits_.back();
                }
                p.text(name, x0, y + 12, kAttrName, kSmallFont);
                p.text(truncate(value, 60), x0 + nameW, y + 12, kText, kSmallFont);
            }
            y += 16;
        }
        // Hovering a property shows the declaration it comes from
        if (hovered && page.css) {
            const ElementRules& rules = rulesFor(page, el);
            DeclarationRef ref = rules.winner(hovered->property);
            const RuleBlock* block = rules.block(ref);
            const RuleDeclaration* decl = rules.declaration(ref);
            std::string line1, line2;
            if (!block || !decl) {
                line1 = "No rule sets it: initial or inherited default";
            } else {
                if (ref.group >= 0) line1 = "Inherited from " + elementSelector(rules.inherited[ref.group].ancestor) + ": ";
                if (block->kind == RuleKind::Inline) line1 += "element { } (style attribute)";
                else if (block->kind == RuleKind::Hints) line1 += "the element's presentational attributes";
                else line1 += block->selector + "  (" + block->source + ")";
                line2 = decl->property + ": " + decl->value + (decl->important ? " !important" : "") + ";";
            }
            float tw = area.w - 24;
            line1 = fitText(p, line1, kSmallFont, tw - 16);
            line2 = fitText(p, line2, kSmallFont, tw - 16);
            float th = line2.empty() ? 22 : 38;
            float tx = x0, ty = hovered->r.y + hovered->r.h + 2;
            if (ty + th > area.y + area.h) ty = hovered->r.y - th - 2;
            p.rect(tx, ty, tw, th, kToolbar, 255);
            p.rect(tx, ty, tw, 1, kAccent, 255);
            p.text(line1, tx + 8, ty + 15, kTag, kSmallFont);
            if (!line2.empty()) p.text(line2, tx + 8, ty + 31, kText, kSmallFont);
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
    bool ruleHighlight = tab_ == PanelTab::Inspector && !highlighted_.empty();
    if (!hover && !selected && !ruleHighlight) return;

    p.pushClip(clipX, clipY, clipW, clipH);
    // Elements matching the clicked selector of a rule: content box and outline
    if (ruleHighlight) {
        std::unordered_set<const DOMElement*> wanted(highlighted_.begin(), highlighted_.end());
        std::vector<LayoutBox*> boxes;
        collectBoxes(page.layoutRoot, wanted, boxes, kMaxHighlighted);
        for (LayoutBox* b : boxes) {
            float x = b->screenX, y = b->screenY, w = b->width, h = b->height;
            p.rect(x, y, w, h, kContentColor, 70);
            p.rect(x, y, w, 1, kAttrName, 255); p.rect(x, y + h - 1, w, 1, kAttrName, 255);
            p.rect(x, y, 1, h, kAttrName, 255); p.rect(x + w - 1, y, 1, h, kAttrName, 255);
        }
    }
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
// Rules
// ---------------------------------------------------------------------------

const StyleEdit* DevToolsPanel::styleEditFor(const DOMElement* element) const {
    for (const auto& [owner, edit] : styleEdits_) {
        if (owner == element) return &edit;
    }
    return nullptr;
}

const ElementRules& DevToolsPanel::rulesFor(const Page& page, DOMElement* element) {
    size_t sheets = page.css ? page.css->styleSheets().size() : 0;
    if (rulesDirty_ || element != rulesElement_ || page.domVersion != rulesDomVersion_ || sheets != rulesSheetCount_) {
        if (element != rulesElement_) rulesScroll_ = 0;
        rulesElement_ = element;
        rulesDomVersion_ = page.domVersion;
        rulesSheetCount_ = sheets;
        rulesDirty_ = false;
        rulesModel_ = page.css && element
            ? collectElementRules(*page.css, element, [this](const DOMElement* e) { return styleEditFor(e); })
            : ElementRules{};
    }
    return rulesModel_;
}

void DevToolsPanel::drawRules(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY) {
    ruleHits_.clear();
    float x0 = area.x + 12;
    p.rect(area.x + 1, area.y, area.w - 1, 24, kToolbar, 255);
    p.text("Rules", x0, area.y + 16, kText, kFont);
    DOMElement* el = elementForStyles();
    if (!el) {
        p.text("Select an element to see its rules.", x0, area.y + 44, kMuted, kFont);
        return;
    }
    if (!page.css) {
        p.text("No stylesheets", x0, area.y + 44, kMuted, kFont);
        return;
    }
    const ElementRules& rules = rulesFor(page, el);
    Rect body{area.x + 1, area.y + 24, area.w - 1, area.h - 24};
    bool mouseIn = body.contains(mouseX, mouseY);
    float y = body.y + 6 - rulesScroll_;
    p.pushClip(body.x, body.y, body.w, body.h);

    auto drawBlock = [&](const RuleBlock& block, int group, int blockIndex) {
        if (block.kind == RuleKind::Hints) return;
        bool visible = y + kRuleLineH * (block.declarations.size() + 2) >= body.y && y <= body.y + body.h;
        // Selector line: the selector (click: highlight what it matches) and where the rule is from
        std::string source = block.kind == RuleKind::Inline ? "style attribute" : block.source;
        float sourceW = p.textWidth(source, kSmallFont);
        std::string selector = fitText(p, block.selector, kFont, std::max(40.0f, area.w - 40 - sourceW - 16));
        const void* key = block.kind == RuleKind::Inline ? (const void*)block.element : (const void*)block.rule;
        bool active = highlightKey_ && highlightKey_ == key;
        float selW = p.textWidth(selector, kFont);
        Rect selRect{x0 - 2, y, selW + 4, kRuleLineH};
        if (visible) {
            if (active) p.rect(selRect.x, selRect.y, selRect.w, selRect.h, kSelection, 255);
            else if (mouseIn && selRect.contains(mouseX, mouseY)) p.rect(selRect.x, selRect.y, selRect.w, selRect.h, kHover, 255);
            p.text(selector, x0, y + 12, active ? kText : kTag, kFont);
            p.text(" {", x0 + selW, y + 12, kMuted, kFont);
            p.text(source, area.x + area.w - 12 - sourceW, y + 12, kMuted, kSmallFont);
        }
        ruleHits_.push_back({selRect, group, blockIndex, -1});
        y += kRuleLineH;

        // Declarations: struck through when they lose the cascade or are switched off
        for (size_t i = 0; i < block.declarations.size(); i++) {
            const RuleDeclaration& d = block.declarations[i];
            Rect row{body.x, y, body.w, kRuleLineH};
            bool hovered = mouseIn && row.contains(mouseX, mouseY);
            if (y + kRuleLineH >= body.y && y <= body.y + body.h) {
                if (hovered) p.rect(row.x, row.y, row.w, row.h, kHover, 255);
                // Checkbox: shown unchecked on a switched-off declaration, checked on hover
                float cx = x0 + 4, cy = y + 3;
                if (d.disabled || hovered) {
                    p.rect(cx, cy, 10, 1, kMuted, 255); p.rect(cx, cy + 9, 10, 1, kMuted, 255);
                    p.rect(cx, cy, 1, 10, kMuted, 255); p.rect(cx + 9, cy, 1, 10, kMuted, 255);
                    if (!d.disabled) {
                        p.rect(cx + 1, cy + 1, 8, 8, kAccent, 255);
                        p.line(cx + 2, cy + 5, cx + 4, cy + 7, kBg);
                        p.line(cx + 4, cy + 7, cx + 8, cy + 2, kBg);
                    }
                }
                bool struck = d.disabled || d.overridden;
                float x = x0 + 20;
                auto put = [&](const std::string& t, uint32_t c) {
                    p.text(t, x, y + 12, struck ? kMuted : c, kSmallFont);
                    x += p.textWidth(t, kSmallFont);
                };
                float startX = x;
                put(d.property, kAttrName);
                put(": ", kMuted);
                std::string value = d.value + (d.important ? " !important" : "");
                float room = area.x + area.w - 12 - x - p.textWidth(";", kSmallFont);
                put(fitText(p, value, kSmallFont, std::max(20.0f, room)), kText);
                put(";", kMuted);
                if (struck) p.rect(startX, y + 8, x - startX, 1, d.disabled ? kMuted : kText, 255);
            }
            ruleHits_.push_back({row, group, blockIndex, (int)i});
            y += kRuleLineH;
        }
        if (visible) p.text("}", x0, y + 12, kMuted, kFont);
        y += kRuleLineH + 6;
    };

    for (size_t b = 0; b < rules.blocks.size(); b++) drawBlock(rules.blocks[b], -1, (int)b);
    for (size_t g = 0; g < rules.inherited.size(); g++) {
        const InheritedRules& group = rules.inherited[g];
        bool any = std::any_of(group.blocks.begin(), group.blocks.end(),
                               [](const RuleBlock& b) { return b.kind != RuleKind::Hints; });
        if (!any) continue;
        p.rect(body.x, y, body.w, 20, kBgAlt, 255);
        p.rect(body.x, y, body.w, 1, kBorder, 255);
        float labelW = p.textWidth("Inherited from ", kSmallFont);
        p.text("Inherited from ", x0, y + 14, kMuted, kSmallFont);
        p.text(fitText(p, elementSelector(group.ancestor), kSmallFont, area.w - 24 - labelW), x0 + labelW, y + 14,
               kTag, kSmallFont);
        y += 26;
        for (size_t b = 0; b < group.blocks.size(); b++) drawBlock(group.blocks[b], (int)g, (int)b);
    }
    p.popClip();
    rulesMax_ = (y + rulesScroll_) - (body.y + body.h) + 8;
    rulesScroll_ = clampScroll(rulesScroll_, rulesMax_);
    if (rulesMax_ > 0) {
        float total = body.h + rulesMax_;
        float thumbH = std::max(20.0f, body.h * body.h / total);
        float thumbY = body.y + (body.h - thumbH) * (rulesScroll_ / rulesMax_);
        p.rect(area.x + area.w - 8, thumbY, 6, thumbH, kBorder, 255);
    }
}

void DevToolsPanel::onRulesClick(const Page& page, float x, float y) {
    DOMElement* el = elementForStyles();
    if (!el || !page.css || y < rules_.y + 24) return;   // the header, or rows scrolled under it
    for (const RuleHit& hit : ruleHits_) {
        if (!hit.r.contains(x, y)) continue;
        if (hit.declaration < 0) {
            const ElementRules& rules = rulesFor(page, el);
            if (const RuleBlock* block = rules.block({hit.group, hit.block, 0})) highlightRule(page, *block);
        } else {
            toggleDeclaration(page, hit);
        }
        return;
    }
}

// Clicking a selector highlights the elements it matches; clicking it again stops
void DevToolsPanel::highlightRule(const Page& page, const RuleBlock& block) {
    const void* key = block.kind == RuleKind::Inline ? (const void*)block.element : (const void*)block.rule;
    if (highlightKey_ == key) {
        highlightKey_ = nullptr;
        highlightSelector_.clear();
        highlighted_.clear();
        return;
    }
    highlightKey_ = key;
    highlightSelector_ = block.kind == RuleKind::Inline ? std::string() : block.selector;
    highlighted_.clear();
    if (block.kind == RuleKind::Inline) highlighted_.push_back(block.element);
    refreshHighlight(page);
}

// The matched elements again after the DOM changed (the cascade's own matcher)
void DevToolsPanel::refreshHighlight(const Page& page) {
    if (!highlightKey_) return;
    if (highlightSelector_.empty()) {
        if (!document_ || highlighted_.empty() || !document_->contains(highlighted_[0])) {
            highlightKey_ = nullptr;
            highlighted_.clear();
        }
        return;
    }
    highlighted_ = page.document ? selectDescendants(page.document, highlightSelector_) : std::vector<DOMElement*>{};
    if (highlighted_.size() > kMaxHighlighted) highlighted_.resize(kMaxHighlighted);
}

// Switches a declaration off (or back on) for the element it applies to: the
// element's style attribute is rewritten, the shared stylesheets stay as they are
void DevToolsPanel::toggleDeclaration(const Page& page, const RuleHit& hit) {
    DOMElement* el = elementForStyles();
    const ElementRules& rules = rulesFor(page, el);
    const RuleBlock* block = rules.block({hit.group, hit.block, hit.declaration});
    const RuleDeclaration* decl = rules.declaration({hit.group, hit.block, hit.declaration});
    if (!block || !decl || block->kind == RuleKind::Hints || !block->element) return;
    DOMElement* owner = block->element;
    DisabledDeclaration key{block->kind == RuleKind::Style ? block->rule : nullptr, decl->property};

    auto it = std::find_if(styleEdits_.begin(), styleEdits_.end(), [&](const auto& e) { return e.first == owner; });
    if (it == styleEdits_.end()) {
        StyleEdit edit;
        edit.hadStyle = owner->hasAttribute("style");
        edit.originalStyle = owner->getAttribute("style");
        styleEdits_.push_back({owner, std::move(edit)});
        it = styleEdits_.end() - 1;
    }
    StyleEdit& edit = it->second;
    auto found = std::find(edit.disabled.begin(), edit.disabled.end(), key);
    if (found != edit.disabled.end()) edit.disabled.erase(found);
    else edit.disabled.push_back(key);

    if (edit.disabled.empty()) {
        // Everything is back on: the original attribute, exactly
        if (edit.hadStyle) owner->setAttribute("style", edit.originalStyle);
        else owner->removeAttribute("style");
        styleEdits_.erase(it);
    } else {
        edit.writtenStyle = styleAttributeFor(*page.css, owner, edit);
        owner->setAttribute("style", edit.writtenStyle);
    }
    // The attribute change bumps domMutationVersion(): the browser restyles and lays out again
    rulesDirty_ = true;
}

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------

namespace {

using Zepra::WebCore::ConsoleMessage;
using Zepra::WebCore::ValuePreview;
using Kind = ValuePreview::Kind;

constexpr uint32_t kKeyText    = 0xFF7DE9;   // property names
constexpr uint32_t kStringText = 0x86DE74;
constexpr uint32_t kNumberText = 0x75BFFF;   // numbers, booleans
constexpr uint32_t kNodeText   = 0x75BFFF;
constexpr uint32_t kGroupText  = 0xFBFBFE;
constexpr uint32_t kLinkLine   = 0x3A3118;   // the line a source link points at

constexpr float kLineH = 18;           // console rows, input lines, popup rows, source lines
constexpr size_t kMaxInputLines = 8;   // the input grows to this many lines, then scrolls
constexpr int kPopupRows = 8;          // autocomplete candidates shown at once

uint32_t kindColor(Kind k, uint32_t plain) {
    switch (k) {
        case Kind::String: return kStringText;
        case Kind::Number: case Kind::Boolean: case Kind::BigInt: case Kind::Symbol: return kNumberText;
        case Kind::Undefined: case Kind::Null: case Kind::Getter: case Kind::More: return kMuted;
        case Kind::Node: return kNodeText;
        case Kind::RegExp: return kStringText;
        default: return plain;
    }
}

// The first line of a preview, and the lines after it (an error's stack frames)
std::string firstLine(const std::string& s) {
    size_t nl = s.find('\n');
    return nl == std::string::npos ? s : s.substr(0, nl);
}

std::vector<std::string> restLines(const std::string& s) {
    std::vector<std::string> lines;
    size_t pos = s.find('\n');
    while (pos != std::string::npos) {
        size_t next = s.find('\n', pos + 1);
        lines.push_back(s.substr(pos + 1, next == std::string::npos ? std::string::npos : next - pos - 1));
        pos = next;
    }
    return lines;
}

// The panel's font has ASCII glyphs only: the ellipsis and arrow of the previews drawn as ASCII
std::string drawable(std::string s) {
    for (size_t pos; (pos = s.find("\xE2\x80\xA6")) != std::string::npos;) s.replace(pos, 3, "...");   // …
    for (size_t pos; (pos = s.find("\xE2\x86\x92")) != std::string::npos;) s.replace(pos, 3, "->");    // →
    return s;
}

// Typed or loaded text as the font can draw it: a tab as four spaces, other
// code points past ASCII as one '?', at most `max` characters
std::string asciiOnly(const std::string& s, size_t max = 1000) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (out.size() >= max) { out += "..."; break; }
        if (c == '\t') out += "    ";
        else if (c == '\r') continue;
        else if (c < 0x20) out += ' ';
        else if (c < 0x80) out += (char)c;
        else if ((c & 0xC0) == 0xC0) out += '?';   // a lead byte; its continuation bytes are dropped
    }
    return out;
}

// "app.js:12" for a link, the full text when the message names no script
std::string sourceLabel(const ConsoleMessage& m) {
    if (m.sourceUrl.empty() || m.source.rfind(m.sourceUrl, 0) != 0) return m.source;
    return sourceDisplayName(m.sourceUrl) + m.source.substr(m.sourceUrl.size());
}

} // namespace

void DevToolsPanel::setFilterText(std::string text) {
    filter_.text = std::move(text);
    filterMatcher_ = ConsoleFilterMatcher(filter_);
}

void DevToolsPanel::insertConsoleText(const std::string& text) {
    if (filterFocused_) {
        setFilterText(filter_.text + text.substr(0, text.find_first_of("\r\n")));
    } else {
        input_.insert(text);
        completionOpen_ = false;
    }
}

// The rows of the console, top to bottom: a message's line, then for each of
// its values the lines of a multi-line text and, when expanded, its children.
// Messages the filter hides are counted, not shown.
void DevToolsPanel::buildConsoleRows(const Page& page) {
    consoleRows_.clear();
    consoleHidden_ = 0;
    if (!page.consoleLog) return;
    std::function<void(ConsoleMessage&, ValuePreview&, int)> addChildren = [&](ConsoleMessage& m, ValuePreview& v, int depth) {
        for (const std::string& line : restLines(v.text)) {
            ConsoleRow r;
            r.message = &m;
            r.messageId = m.id;
            r.depth = depth + 1;
            r.line = line;
            r.continuation = true;
            consoleRows_.push_back(std::move(r));
        }
        if (!v.expanded) return;
        for (ValuePreview& c : v.children) {
            ConsoleRow r;
            r.message = &m;
            r.messageId = m.id;
            r.node = &c;
            r.depth = depth + 1;
            consoleRows_.push_back(std::move(r));
            addChildren(m, c, depth + 1);
        }
    };
    for (ConsoleMessage& m : *page.consoleLog) {
        if (m.id == 0) m.id = nextConsoleId_++;
        if (!filterMatcher_.passes(m)) {
            consoleHidden_++;
            continue;
        }
        ConsoleRow r;
        r.message = &m;
        r.messageId = m.id;
        r.depth = m.depth;
        consoleRows_.push_back(std::move(r));
        if (m.values.empty()) {
            for (const std::string& line : restLines(m.text)) {
                ConsoleRow c;
                c.message = &m;
                c.messageId = m.id;
                c.depth = m.level == "input" ? m.depth : m.depth + 1;   // typed lines stay aligned
                c.line = line;
                c.continuation = true;
                consoleRows_.push_back(std::move(c));
            }
        } else {
            for (ValuePreview& v : m.values) addChildren(m, v, m.depth);
        }
    }
}

void DevToolsPanel::drawConsole(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY) {
    const float barH = 26;
    const size_t inputLines = std::min(input_.lineCount(), kMaxInputLines);
    const float inputH = 10 + inputLines * kLineH;
    consoleToolbar_ = {area.x, area.y, area.w, barH};
    consoleList_ = {area.x, area.y + barH, area.w, area.h - barH - inputH};
    consoleInputBox_ = {area.x, area.y + area.h - inputH, area.w, inputH};

    // console.clear() / clear(): everything before the last one goes
    if (page.consoleLog) {
        auto& log = *page.consoleLog;
        for (size_t i = log.size(); i-- > 0;) {
            if (log[i].level == "clear") { log.erase(log.begin(), log.begin() + (long)i + 1); break; }
        }
    }

    // Toolbar: clear, the text filter, level buttons with counts
    p.rect(consoleToolbar_.x, consoleToolbar_.y, consoleToolbar_.w, consoleToolbar_.h, kToolbar, 255);
    p.rect(consoleToolbar_.x, consoleToolbar_.y + barH - 1, consoleToolbar_.w, 1, kBorder, 255);
    int nErr = 0, nWarn = 0, nLog = 0, nInfo = 0, nDebug = 0;
    if (page.consoleLog) {
        for (const auto& m : *page.consoleLog) {
            std::string level = filterLevel(m);
            if (level == "error") nErr++;
            else if (level == "warn") nWarn++;
            else if (level == "log") nLog++;
            else if (level == "info") nInfo++;
            else if (level == "debug") nDebug++;
        }
    }
    clearButton_ = {area.x + 8, area.y + 4, p.textWidth("Clear", kSmallFont) + 16, barH - 8};
    p.rect(clearButton_.x, clearButton_.y, clearButton_.w, clearButton_.h,
           clearButton_.contains(mouseX, mouseY) ? kHover : kBg, 255);
    p.text("Clear", clearButton_.x + 8, clearButton_.y + 14, kText, kSmallFont);

    levelButtons_.clear();
    struct Filter { const char* name; int count; bool on; uint32_t color; };
    Filter filters[] = {{"Errors", nErr, filter_.errors, kErrorText},
                        {"Warnings", nWarn, filter_.warnings, kWarnText},
                        {"Logs", nLog, filter_.logs, kText},
                        {"Info", nInfo, filter_.info, kText},
                        {"Debug", nDebug, filter_.debug, kText}};
    float fx = area.x + area.w - 8;
    for (int i = 4; i >= 0; i--) {
        std::string label = std::string(filters[i].name) + " " + std::to_string(filters[i].count);
        float lw = p.textWidth(label, kSmallFont) + 16;
        Rect r{fx - lw, area.y + 4, lw, barH - 8};
        p.rect(r.x, r.y, r.w, r.h, filters[i].on ? kBg : kToolbar, 255);
        if (r.contains(mouseX, mouseY)) p.rect(r.x, r.y, r.w, r.h, kHover, 255);
        p.text(label, r.x + 8, r.y + 14, filters[i].on ? filters[i].color : kMuted, kSmallFont);
        levelButtons_.insert(levelButtons_.begin(), r);
        fx -= lw + 6;
    }

    // Rows (built before the filter box, which shows how many the filter hid)
    buildConsoleRows(page);

    float filterX = clearButton_.x + clearButton_.w + 8;
    filterBox_ = {filterX, area.y + 4, std::max(60.0f, std::min(320.0f, fx - 6 - filterX)), barH - 8};
    {
        const Rect& fb = filterBox_;
        p.rect(fb.x, fb.y, fb.w, fb.h, kBg, 255);
        uint32_t edge = filterFocused_ ? kAccent : kBorder;
        p.rect(fb.x, fb.y, fb.w, 1, edge, 255);
        p.rect(fb.x, fb.y + fb.h - 1, fb.w, 1, edge, 255);
        p.rect(fb.x, fb.y, 1, fb.h, edge, 255);
        p.rect(fb.x + fb.w - 1, fb.y, 1, fb.h, edge, 255);
        std::string hidden = consoleHidden_ ? std::to_string(consoleHidden_) + " hidden" : "";
        float hiddenW = hidden.empty() ? 0 : p.textWidth(hidden, kSmallFont) + 8;
        p.pushClip(fb.x + 1, fb.y, fb.w - 2 - hiddenW, fb.h);
        std::string shown = asciiOnly(filter_.text, 200);
        if (shown.empty() && !filterFocused_) {
            p.text("Filter output", fb.x + 6, fb.y + 14, kMuted, kSmallFont);
        } else {
            // the end of a long filter stays in view
            float tw = p.textWidth(shown, kSmallFont);
            float tx = std::min(fb.x + 6, fb.x + fb.w - hiddenW - 8 - tw);
            p.text(shown, tx, fb.y + 14, kText, kSmallFont);
            if (filterFocused_ && (caretBlink_ / 30) % 2 == 0) p.rect(tx + tw + 1, fb.y + 4, 1, fb.h - 8, kText, 255);
        }
        p.popClip();
        if (!hidden.empty()) p.text(hidden, fb.x + fb.w - hiddenW, fb.y + 14, kMuted, kSmallFont);
    }

    consoleMax_ = consoleRows_.size() * kLineH - consoleList_.h + 4;
    if (page.consoleLog && page.consoleLog->size() != consoleSeen_) {
        consoleSeen_ = page.consoleLog->size();
        if (consoleStick_) consoleScroll_ = consoleMax_;
    }
    if (consoleStick_) consoleScroll_ = consoleMax_;
    consoleScroll_ = clampScroll(consoleScroll_, consoleMax_);

    p.pushClip(consoleList_.x, consoleList_.y, consoleList_.w, consoleList_.h);
    if (consoleRows_.empty()) {
        const char* empty = consoleHidden_ ? "No messages match the filter."
                                           : "Page console output and uncaught errors appear here.";
        p.text(empty, area.x + 12, consoleList_.y + 22, kMuted, kFont);
    }
    const float textX = consoleList_.x + 24;
    float ly = consoleList_.y + 2 - consoleScroll_;
    bool mouseInList = consoleList_.contains(mouseX, mouseY) && !(completionOpen_ && completionBox_.contains(mouseX, mouseY));
    for (ConsoleRow& row : consoleRows_) {
        if (ly + kLineH < consoleList_.y || ly > consoleList_.y + consoleList_.h) { ly += kLineH; continue; }
        ConsoleMessage& m = *row.message;
        uint32_t color = kText, bg = 0;
        bool hasBg = false;
        if (m.level == "error") { color = kErrorText; bg = kErrorBg; hasBg = true; }
        else if (m.level == "warn") { color = kWarnText; bg = kWarnBg; hasBg = true; }
        else if (m.level == "input") color = kInputText;
        else if (m.level == "result") color = kResultText;
        else if (m.level == "group") color = kGroupText;
        else if (m.level == "debug") color = kMuted;
        if (hasBg) p.rect(consoleList_.x, ly, consoleList_.w, kLineH, bg, 255);
        bool hovered = mouseInList && mouseY >= ly && mouseY < ly + kLineH;
        if (hovered && row.node && !row.node->children.empty()) p.rect(consoleList_.x, ly, consoleList_.w, kLineH, kHover, 255);
        bool messageRow = !row.node && !row.continuation;
        if (messageRow) {
            // the "last row" separator; gutter icon by level
            p.rect(consoleList_.x, ly + kLineH - 1, consoleList_.w, 1, kBgAlt, 255);
            float gx = consoleList_.x + 10, gy = ly + kLineH / 2;
            if (m.level == "input") p.triangle(gx, gy - 4, gx, gy + 4, gx + 6, gy, kInputText);
            else if (m.level == "result") p.triangle(gx + 6, gy - 4, gx + 6, gy + 4, gx, gy, kResultText);
            else if (m.level == "error") p.rect(consoleList_.x, ly, 3, kLineH, kErrorText, 255);
            else if (m.level == "warn") p.rect(consoleList_.x, ly, 3, kLineH, kWarnText, 255);
        }
        float x = textX + row.depth * kIndent;
        float baseline = ly + 13;
        auto put = [&](const std::string& raw, uint32_t c) {
            std::string s = drawable(raw);
            p.text(s, x, baseline, c, kFont);
            x += p.textWidth(s, kFont);
        };
        auto arrow = [&](bool open) {
            float ax = x + 2, ay = ly + kLineH / 2;
            if (open) p.triangle(ax - 1, ay - 2, ax + 7, ay - 2, ax + 3, ay + 3, kMuted);
            else p.triangle(ax, ay - 4, ax, ay + 4, ax + 5, ay, kMuted);
            x += 12;
        };
        if (row.continuation) {
            uint32_t c = m.level == "error" ? kErrorText : m.level == "input" ? kInputText : kMuted;
            put(truncate(asciiOnly(row.line), 400), c);
        } else if (row.node) {
            ValuePreview& v = *row.node;
            if (!v.children.empty()) arrow(v.expanded);
            else x += 12;
            if (!v.key.empty()) { put(v.key, kKeyText); put(": ", kText); }
            put(truncate(firstLine(v.text), 400), kindColor(v.kind, color));
        } else {
            row.toggles.clear();
            row.linkX0 = row.linkX1 = 0;
            if (m.values.empty()) {
                put(truncate(asciiOnly(firstLine(m.text)), 400), color);
            } else {
                for (size_t i = 0; i < m.values.size(); i++) {
                    ValuePreview& v = m.values[i];
                    if (!v.children.empty()) {
                        row.toggles.push_back({i, x, x + 12});
                        arrow(v.expanded);
                    }
                    put(truncate(firstLine(v.text), 400), kindColor(v.kind, color));
                    x += 8;
                }
            }
            if (!m.source.empty()) {
                // file:line on the right; a link to the Sources view when the text is kept
                std::string label = asciiOnly(sourceLabel(m), 120);
                float sw = p.textWidth(label, kSmallFont);
                float sx = consoleList_.x + consoleList_.w - 12 - sw;
                bool linkable = page.script && m.sourceLine && page.script->sourceText(m.sourceUrl);
                bool linkHover = linkable && hovered && mouseX >= sx && mouseX < sx + sw;
                if (hasBg || x > sx - 8) p.rect(sx - 8, ly, sw + 20, kLineH - 1, hasBg ? bg : kBg, 255);
                p.text(label, sx, baseline, linkHover ? kAccent : kMuted, kSmallFont);
                if (linkHover) p.rect(sx, baseline + 2, sw, 1, kAccent, 255);
                if (linkable) { row.linkX0 = sx; row.linkX1 = sx + sw; }
            }
        }
        ly += kLineH;
    }
    p.popClip();

    // Scrollbar
    if (consoleMax_ > 0) {
        float total = consoleRows_.size() * kLineH + 4;
        float thumbH = std::max(20.0f, consoleList_.h * consoleList_.h / total);
        float thumbY = consoleList_.y + (consoleList_.h - thumbH) * (consoleScroll_ / consoleMax_);
        p.rect(consoleList_.x + consoleList_.w - 8, thumbY, 6, thumbH, kBorder, 255);
    }

    // Input: one line per line typed (Shift+Enter), scrolling past kMaxInputLines
    const Rect& ib = consoleInputBox_;
    p.rect(ib.x, ib.y, ib.w, ib.h, kBg, 255);
    p.rect(ib.x, ib.y, ib.w, 1, consoleFocused_ ? kAccent : kBorder, 255);
    const float inputX = ib.x + 24, firstRowY = ib.y + 5;
    {
        float ix = ib.x + 10, iy = firstRowY + kLineH / 2;
        p.triangle(ix, iy - 4, ix, iy + 4, ix + 6, iy, kAccent);
    }
    const size_t caretLine = input_.caretLine();
    if (caretLine < inputScroll_) inputScroll_ = (float)caretLine;
    if (caretLine >= inputScroll_ + inputLines) inputScroll_ = (float)(caretLine - inputLines + 1);
    const size_t top = (size_t)inputScroll_;
    p.pushClip(ib.x, ib.y + 1, ib.w, ib.h - 1);
    if (input_.empty() && !consoleFocused_) {
        p.text("Evaluate JavaScript in the page", inputX, firstRowY + 13, kMuted, kFont);
    } else {
        std::vector<std::string> lines = input_.lines();
        for (size_t i = top; i < lines.size() && i < top + inputLines; i++) {
            p.text(asciiOnly(lines[i]), inputX, firstRowY + (i - top) * kLineH + 13, kText, kFont);
        }
        float caretX = inputX + p.textWidth(asciiOnly(input_.caretLinePrefix()), kFont);
        float caretY = firstRowY + (caretLine - top) * kLineH;
        // The rest of the selected candidate, greyed after the caret
        bool atLineEnd = input_.caret() == input_.text().size() || input_.text()[input_.caret()] == '\n';
        if (completionOpen_ && atLineEnd && completionIndex_ < (int)completions_.size()) {
            const std::string& pick = completions_[completionIndex_];
            if (pick.rfind(completionQuery_.prefix, 0) == 0)
                p.text(asciiOnly(pick.substr(completionQuery_.prefix.size())), caretX + 1, caretY + 13, kMuted, kFont);
        }
        if (consoleFocused_ && (caretBlink_ / 30) % 2 == 0) p.rect(caretX + 1, caretY + 2, 1, kLineH - 4, kText, 255);
    }
    p.popClip();

    // Autocomplete popup, above the line being typed
    completionBox_ = {};
    if (completionOpen_ && consoleFocused_ && !completions_.empty()) {
        int rows = std::min(kPopupRows, (int)completions_.size());
        completionIndex_ = std::max(0, std::min(completionIndex_, (int)completions_.size() - 1));
        if (completionIndex_ < completionTop_) completionTop_ = completionIndex_;
        if (completionIndex_ >= completionTop_ + rows) completionTop_ = completionIndex_ - rows + 1;
        float widest = 120;
        for (int i = 0; i < rows; i++)
            widest = std::max(widest, p.textWidth(asciiOnly(completions_[completionTop_ + i], 80), kFont) + 24);
        std::string before = input_.caretLinePrefix();
        before = before.substr(0, before.size() - std::min(before.size(), completionQuery_.prefix.size()));
        float bx = std::min(inputX + p.textWidth(asciiOnly(before), kFont) - 8, area.x + area.w - widest - 4);
        float rowTop = firstRowY + (caretLine - top) * kLineH;
        float bh = rows * kLineH + 4;
        completionBox_ = {std::max(area.x + 4, bx), std::max(area.y, rowTop - bh), widest, bh};
        const Rect& cb = completionBox_;
        p.rect(cb.x, cb.y, cb.w, cb.h, kToolbar, 255);
        p.rect(cb.x, cb.y, cb.w, 1, kBorder, 255);
        p.rect(cb.x, cb.y + cb.h - 1, cb.w, 1, kBorder, 255);
        p.rect(cb.x, cb.y, 1, cb.h, kBorder, 255);
        p.rect(cb.x + cb.w - 1, cb.y, 1, cb.h, kBorder, 255);
        for (int i = 0; i < rows; i++) {
            int idx = completionTop_ + i;
            float ry = cb.y + 2 + i * kLineH;
            if (idx == completionIndex_) p.rect(cb.x + 1, ry, cb.w - 2, kLineH, kSelection, 255);
            else if (mouseX >= cb.x && mouseX < cb.x + cb.w && mouseY >= ry && mouseY < ry + kLineH)
                p.rect(cb.x + 1, ry, cb.w - 2, kLineH, kHover, 255);
            std::string name = asciiOnly(completions_[idx], 80);
            float tx = cb.x + 12;
            size_t typed = std::min(completionQuery_.prefix.size(), name.size());
            if (typed && completions_[idx].rfind(completionQuery_.prefix, 0) == 0) {
                // the typed part in the accent colour, as Firefox marks it
                std::string head = name.substr(0, typed);
                p.text(head, tx, ry + 13, kAccent, kFont);
                tx += p.textWidth(head, kFont);
                name = name.substr(typed);
            }
            p.text(name, tx, ry + 13, kText, kFont);
        }
        if ((int)completions_.size() > rows) {
            // a thin bar shows where the visible candidates sit in the list
            float th = std::max(10.0f, (cb.h - 4) * rows / completions_.size());
            float ty = cb.y + 2 + (cb.h - 4 - th) * completionTop_ / std::max(1, (int)completions_.size() - rows);
            p.rect(cb.x + cb.w - 4, ty, 2, th, kBorder, 255);
        }
    }
}

void DevToolsPanel::evaluateConsoleInput(const Page& page) {
    std::string expr = input_.text();
    if (expr.find_first_not_of(" \t\n") == std::string::npos) return;
    auto log = [&](ConsoleMessage m) {
        if (!page.consoleLog) return;
        page.consoleLog->push_back(std::move(m));
        if (page.consoleLog->size() > 500) page.consoleLog->erase(page.consoleLog->begin());
    };
    if (consoleHistory_.empty() || consoleHistory_.back() != expr) consoleHistory_.push_back(expr);
    historyIndex_ = -1;
    historyDraft_.clear();
    consoleStick_ = true;
    ConsoleMessage input;
    input.level = "input";
    input.text = expr;
    log(std::move(input));
    if (!page.script) {
        ConsoleMessage err;
        err.level = "error";
        err.text = "No script context for this page";
        log(std::move(err));
    } else {
        // $0 is the Inspector's selection; the result (or the thrown value) comes back formatted
        log(page.script->evaluateConsole(expr, selected_));
    }
    input_.clear();
    inputScroll_ = 0;
    completionOpen_ = false;
    completionPathKey_.clear();   // the input may have defined globals
}

void DevToolsPanel::updateCompletions(const Page& page, bool force) {
    // Typing on keeps the highlighted name while it still matches
    std::string selected = completionOpen_ && completionIndex_ >= 0 && completionIndex_ < (int)completions_.size()
                         ? completions_[completionIndex_] : std::string();
    completionOpen_ = false;
    if (!page.script) return;
    CompletionQuery q = completionQuery(input_.text(), input_.caret());
    if (!q.valid || (!force && q.prefix.empty() && q.path.empty())) return;
    std::string key = "#";
    for (const std::string& part : q.path) key += part + ".";
    // The names of one object are listed once while its name is typed; a new
    // word lists them again (scripts may have added globals since)
    if (key != completionPathKey_ || page.script != completionScript_ || q.prefix.size() <= 1) {
        completionNames_ = page.script->completionNames(q.path);
        completionPathKey_ = key;
        completionScript_ = page.script;
    }
    completions_ = rankCompletions(completionNames_, q.prefix);
    if (completions_.empty() || (completions_.size() == 1 && completions_[0] == q.prefix)) return;
    auto again = std::find(completions_.begin(), completions_.end(), selected);
    completionIndex_ = again != completions_.end() ? (int)(again - completions_.begin()) : 0;
    completionQuery_ = q;
    completionOpen_ = true;
}

void DevToolsPanel::acceptCompletion() {
    if (!completionOpen_ || completionIndex_ < 0 || completionIndex_ >= (int)completions_.size()) return;
    input_.replaceBeforeCaret(completionQuery_.replaceFrom, completions_[completionIndex_]);
    completionOpen_ = false;
}

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------

void DevToolsPanel::openSource(const std::string& url, uint32_t line) {
    tab_ = PanelTab::Sources;
    sourceUrl_ = url;
    sourceLine_ = line;
    sourceScrollToLine_ = true;
    blurConsole();
}

void DevToolsPanel::drawSources(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY) {
    // The scripts the page ran (and its document, which holds the inline ones) | the selected text
    float listW = std::min(260.0f, std::floor(area.w * 0.25f));
    sourceList_ = {area.x, area.y, listW, area.h};
    sourceView_ = {area.x + listW, area.y, area.w - listW, area.h};
    std::vector<std::string> names = page.script ? page.script->sourceNames() : std::vector<std::string>{};
    if (sourceUrl_.empty() && !names.empty()) sourceUrl_ = names.front();

    p.rect(sourceList_.x, sourceList_.y, sourceList_.w, sourceList_.h, kBgAlt, 255);
    p.rect(sourceList_.x + sourceList_.w - 1, sourceList_.y, 1, sourceList_.h, kBorder, 255);
    sourceListHits_.clear();
    p.pushClip(sourceList_.x, sourceList_.y, sourceList_.w - 1, sourceList_.h);
    if (names.empty()) p.text("No scripts on this page", sourceList_.x + 10, sourceList_.y + 20, kMuted, kSmallFont);
    float maxListScroll = names.size() * kLineH + 8 - sourceList_.h;
    sourceListScroll_ = clampScroll(sourceListScroll_, maxListScroll);
    float ly = sourceList_.y + 4 - sourceListScroll_;
    for (const std::string& name : names) {
        Rect r{sourceList_.x, ly, sourceList_.w - 1, kLineH};
        if (name == sourceUrl_) p.rect(r.x, r.y, r.w, r.h, kSelection, 255);
        else if (r.contains(mouseX, mouseY)) p.rect(r.x, r.y, r.w, r.h, kHover, 255);
        p.text(truncate(asciiOnly(sourceDisplayName(name), 60), 40), r.x + 10, r.y + 13, kText, kSmallFont);
        sourceListHits_.push_back({r, name});
        ly += kLineH;
    }
    p.popClip();

    // Header: the full name and the line a link pointed at
    const float headH = 22;
    p.rect(sourceView_.x, sourceView_.y, sourceView_.w, headH, kToolbar, 255);
    p.rect(sourceView_.x, sourceView_.y + headH - 1, sourceView_.w, 1, kBorder, 255);
    std::string head = sourceUrl_.empty() ? "Sources" : asciiOnly(sourceUrl_, 200);
    if (sourceLine_) head += "   line " + std::to_string(sourceLine_);
    p.text(head, sourceView_.x + 10, sourceView_.y + 15, kText, kSmallFont);

    auto text = page.script && !sourceUrl_.empty() ? page.script->sourceText(sourceUrl_) : nullptr;
    if (text != sourceText_) {
        sourceText_ = text;
        sourceLineStarts_.clear();
        if (sourceText_) {
            sourceLineStarts_.push_back(0);
            for (size_t i = 0; i < sourceText_->size(); i++)
                if ((*sourceText_)[i] == '\n') sourceLineStarts_.push_back(i + 1);
        }
    }
    Rect body{sourceView_.x, sourceView_.y + headH, sourceView_.w, sourceView_.h - headH};
    if (!sourceText_) {
        const char* why = sourceUrl_.empty() ? "Click a file:line link in the Console to open its source here."
                                             : "This source is not kept for the page.";
        p.text(why, body.x + 12, body.y + 22, kMuted, kFont);
        return;
    }
    size_t lineCount = sourceLineStarts_.size();
    float gutterW = p.textWidth(std::to_string(lineCount), kSmallFont) + 20;
    sourceMax_ = lineCount * kLineH + 4 - body.h;
    if (sourceScrollToLine_) {
        // the linked line a third of the way down
        sourceScroll_ = sourceLine_ ? (sourceLine_ - 1) * kLineH - body.h / 3 : 0;
        sourceScrollToLine_ = false;
    }
    sourceScroll_ = clampScroll(sourceScroll_, sourceMax_);

    p.pushClip(body.x, body.y, body.w, body.h);
    p.rect(body.x, body.y, gutterW, body.h, kBgAlt, 255);
    size_t first = (size_t)std::max(0.0f, std::floor(sourceScroll_ / kLineH));
    for (size_t i = first; i < lineCount; i++) {
        float y = body.y + 2 + i * kLineH - sourceScroll_;
        if (y > body.y + body.h) break;
        if (i + 1 == sourceLine_) p.rect(body.x, y, body.w, kLineH, kLinkLine, 255);
        std::string number = std::to_string(i + 1);
        p.text(number, body.x + gutterW - 10 - p.textWidth(number, kSmallFont), y + 13,
               i + 1 == sourceLine_ ? kWarnText : kMuted, kSmallFont);
        size_t start = sourceLineStarts_[i];
        size_t end = i + 1 < lineCount ? sourceLineStarts_[i + 1] - 1 : sourceText_->size();
        // a minified bundle's line can be megabytes: only its start is drawn
        std::string line = sourceText_->substr(start, std::min<size_t>(end - start, 2000));
        p.text(asciiOnly(line, 400), body.x + gutterW + 8, y + 13, kText, kFont);
    }
    p.popClip();
    if (sourceMax_ > 0) {
        float total = lineCount * kLineH + 4;
        float thumbH = std::max(20.0f, body.h * body.h / total);
        float thumbY = body.y + (body.h - thumbH) * (sourceScroll_ / sourceMax_);
        p.rect(body.x + body.w - 8, thumbY, 6, thumbH, kBorder, 255);
    }
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
    bool popupClick = completionOpen_ && completionBox_.contains(x, y);
    consoleFocused_ = false;
    filterFocused_ = false;
    if (!popupClick) completionOpen_ = false;
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
            } else if (rules_.contains(x, y)) {
                onRulesClick(page, x, y);
            }
            break;
        case PanelTab::Console:
            if (popupClick) {
                int idx = (int)std::floor((y - completionBox_.y - 2) / kLineH);
                if (idx >= 0) {
                    completionIndex_ = completionTop_ + idx;
                    acceptCompletion();
                }
                consoleFocused_ = true;
            } else if (consoleInputBox_.contains(x, y)) {
                consoleFocused_ = true;
                // The caret goes where the click is
                size_t line = (size_t)inputScroll_ + (size_t)std::max(0.0f, std::floor((y - consoleInputBox_.y - 5) / kLineH));
                std::vector<std::string> lines = input_.lines();
                line = std::min(line, lines.size() - 1);
                const std::string& l = lines[line];
                size_t column = 0;
                float target = x - (consoleInputBox_.x + 24);
                while (column < l.size() && textWidth_) {
                    size_t next = column + 1;
                    while (next < l.size() && ((unsigned char)l[next] & 0xC0) == 0x80) next++;
                    float mid = (textWidth_(asciiOnly(l.substr(0, column)), kFont) +
                                 textWidth_(asciiOnly(l.substr(0, next)), kFont)) / 2;
                    if (mid > target) break;
                    column = next;
                }
                input_.moveTo(line, column);
            } else if (filterBox_.contains(x, y)) {
                filterFocused_ = true;
            } else if (clearButton_.contains(x, y)) {
                if (page.consoleLog) page.consoleLog->clear();
            } else if (consoleList_.contains(x, y)) {
                // A triangle (or a property row) opens or closes one level of a value; a
                // source link opens the Sources view. The page may have logged since the
                // last draw (the log vector moved), so the rows are rebuilt and the
                // clicked one is matched by its message id.
                int idx = (int)std::floor((y - (consoleList_.y + 2) + consoleScroll_) / kLineH);
                std::vector<ConsoleRow> drawn = std::move(consoleRows_);
                buildConsoleRows(page);
                if (idx >= 0 && idx < (int)drawn.size() && idx < (int)consoleRows_.size() &&
                    drawn[idx].messageId == consoleRows_[idx].messageId) {
                    const ConsoleRow& row = consoleRows_[idx];
                    if (row.node) {
                        if (!row.node->children.empty()) row.node->expanded = !row.node->expanded;
                    } else if (!row.continuation) {
                        if (drawn[idx].linkX1 > drawn[idx].linkX0 && x >= drawn[idx].linkX0 && x < drawn[idx].linkX1) {
                            openSource(row.message->sourceUrl, row.message->sourceLine);
                            return true;
                        }
                        for (const auto& t : drawn[idx].toggles) {
                            if (x >= t.x0 - 4 && x <= t.x1 + 4 && t.value < row.message->values.size()) {
                                ValuePreview& v = row.message->values[t.value];
                                v.expanded = !v.expanded;
                                break;
                            }
                        }
                    }
                }
            } else {
                bool* flags[] = {&filter_.errors, &filter_.warnings, &filter_.logs, &filter_.info, &filter_.debug};
                for (size_t i = 0; i < levelButtons_.size() && i < 5; i++) {
                    if (levelButtons_[i].contains(x, y)) *flags[i] = !*flags[i];
                }
                setFilterText(filter_.text);
            }
            break;
        case PanelTab::Sources:
            for (const auto& [r, name] : sourceListHits_) {
                if (sourceList_.contains(x, y) && r.contains(x, y)) {
                    sourceUrl_ = name;
                    sourceLine_ = 0;
                    sourceScroll_ = 0;
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
            else if (rules_.contains(x, y)) rulesScroll_ = clampScroll(rulesScroll_ + step, rulesMax_);
            else if (styles_.contains(x, y)) stylesScroll_ = clampScroll(stylesScroll_ + step, stylesMax_);
            break;
        case PanelTab::Console:
            consoleScroll_ = clampScroll(consoleScroll_ + step, consoleMax_);
            consoleStick_ = consoleScroll_ >= consoleMax_ - 1;
            break;
        case PanelTab::Network:
            networkScroll_ = clampScroll(networkScroll_ + step, networkMax_);
            break;
        case PanelTab::Sources:
            if (sourceList_.contains(x, y)) sourceListScroll_ = std::max(0.0f, sourceListScroll_ + step);
            else sourceScroll_ = clampScroll(sourceScroll_ + step, sourceMax_);
            break;
    }
    return true;
}

bool DevToolsPanel::onKey(NXRender::KeyCode key, const std::string& text, bool ctrl, bool shift, const Page& page) {
    if (!visible_ || tab_ != PanelTab::Console) return false;
    if (filterFocused_) return !ctrl && onFilterKey(key, text);
    if (!consoleFocused_) return false;
    return onConsoleInputKey(key, text, ctrl, shift, page);
}

bool DevToolsPanel::onFilterKey(NXRender::KeyCode key, const std::string& text) {
    using NXRender::KeyCode;
    if (key == KeyCode::F12) return false;
    std::string t = filter_.text;
    switch (key) {
        case KeyCode::Escape:
            if (t.empty()) filterFocused_ = false;
            else setFilterText("");
            return true;
        case KeyCode::Backspace:
            if (!t.empty()) {
                do { t.pop_back(); } while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80);
                setFilterText(t);
            }
            return true;
        case KeyCode::Tab:
            filterFocused_ = false;
            consoleFocused_ = true;
            return true;
        case KeyCode::Enter: case KeyCode::Up: case KeyCode::Down: case KeyCode::Left: case KeyCode::Right:
            return true;
        default:
            if (!text.empty() && (unsigned char)text[0] >= 0x20) setFilterText(t + text);
            else if (key == KeyCode::Space) setFilterText(t + " ");
            return true;
    }
}

bool DevToolsPanel::onConsoleInputKey(NXRender::KeyCode key, const std::string& text, bool ctrl, bool shift,
                                      const Page& page) {
    using NXRender::KeyCode;
    if (ctrl) {
        if (key == KeyCode::Space) { updateCompletions(page, true); return true; }
        return false;   // browser shortcuts (Ctrl+V pastes through insertConsoleText)
    }
    if (key == KeyCode::F12) return false;
    if (completionOpen_) {
        int n = (int)completions_.size();
        switch (key) {
            case KeyCode::Tab:
            case KeyCode::Enter:
                if (key == KeyCode::Enter && shift) break;
                acceptCompletion();
                return true;
            case KeyCode::Up: completionIndex_ = (completionIndex_ + n - 1) % n; return true;
            case KeyCode::Down: completionIndex_ = (completionIndex_ + 1) % n; return true;
            case KeyCode::PageUp: completionIndex_ = std::max(0, completionIndex_ - kPopupRows); return true;
            case KeyCode::PageDown: completionIndex_ = std::min(n - 1, completionIndex_ + kPopupRows); return true;
            case KeyCode::Escape: completionOpen_ = false; return true;
            default: break;
        }
    }
    switch (key) {
        case KeyCode::Enter:
            // Shift+Enter, or an unfinished expression (an open bracket, string or comment), adds a line
            if (shift || isIncompleteInput(input_.text())) {
                input_.insert("\n");
                completionOpen_ = false;
            } else {
                evaluateConsoleInput(page);
            }
            return true;
        case KeyCode::Escape:
            consoleFocused_ = false;
            return true;
        case KeyCode::Backspace:
            input_.backspace();
            if (completionOpen_) updateCompletions(page);
            return true;
        case KeyCode::Delete: input_.deleteForward(); completionOpen_ = false; return true;
        case KeyCode::Left: input_.left(); completionOpen_ = false; return true;
        case KeyCode::Right: input_.right(); completionOpen_ = false; return true;
        case KeyCode::Home: input_.home(); completionOpen_ = false; return true;
        case KeyCode::End: input_.end(); completionOpen_ = false; return true;
        case KeyCode::Up:
            // Within a multi-line input first; the history from its first line
            if (!input_.up() && !consoleHistory_.empty()) {
                if (historyIndex_ < 0) {
                    historyDraft_ = input_.text();
                    historyIndex_ = (int)consoleHistory_.size() - 1;
                } else if (historyIndex_ > 0) {
                    historyIndex_--;
                }
                input_.setText(consoleHistory_[historyIndex_]);
            }
            return true;
        case KeyCode::Down:
            if (!input_.down() && historyIndex_ >= 0) {
                historyIndex_++;
                if (historyIndex_ >= (int)consoleHistory_.size()) {
                    historyIndex_ = -1;
                    input_.setText(historyDraft_);
                } else {
                    input_.setText(consoleHistory_[historyIndex_]);
                }
            }
            return true;
        case KeyCode::Tab:
            // Tab completes: one candidate goes in at once, several open the list
            updateCompletions(page, true);
            if (completionOpen_ && completions_.size() == 1) acceptCompletion();
            return true;
        default:
            if (!text.empty() && (unsigned char)text[0] >= 0x20) input_.insert(text);
            else if (key == KeyCode::Space) input_.insert(" ");
            else return true;
            updateCompletions(page);
            return true;
    }
}

} // namespace ZepraBrowser::DevTools
