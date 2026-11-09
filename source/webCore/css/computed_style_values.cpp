// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file computed_style_values.cpp
 * @brief Computed styles as property/value strings (see computed_style_values.hpp)
 */

#include "css/computed_style_values.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>

namespace Zepra::WebCore {

std::string formatComputedNumber(float v) {
    char buf[32];
    if (std::fabs(v - std::round(v)) < 0.05f) std::snprintf(buf, sizeof buf, "%d", (int)std::lround(v));
    else std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}

namespace {

std::string lengthStr(const CSSLength& l) {
    if (l.isAuto()) return "auto";
    std::string n = formatComputedNumber(l.value);
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

// Strings in `content` print double-quoted with their escapes resolved, as
// browsers serialize them ('\\2192 x' -> "→x"); keywords, counter() and attr()
// as written
std::string contentStr(const std::string& value) {
    auto appendUtf8 = [](std::string& out, unsigned long cp) {
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    };
    std::string out;
    for (size_t i = 0; i < value.size(); i++) {
        char c = value[i];
        if (c != '\'' && c != '"') { out += c; continue; }
        const char quote = c;
        std::string text;
        for (i++; i < value.size() && value[i] != quote; i++) {
            if (value[i] != '\\' || i + 1 >= value.size()) { text += value[i]; continue; }
            i++;
            if (std::isxdigit(static_cast<unsigned char>(value[i]))) {
                unsigned long cp = 0;
                int digits = 0;
                while (i < value.size() && digits < 6 && std::isxdigit(static_cast<unsigned char>(value[i]))) {
                    cp = cp * 16 + std::stoul(std::string(1, value[i]), nullptr, 16);
                    i++;
                    digits++;
                }
                if (!(i < value.size() && (value[i] == ' ' || value[i] == '\t' || value[i] == '\n'))) i--;
                appendUtf8(text, cp);
            } else if (value[i] != '\n') {
                text += value[i];
            }
        }
        out += '"';
        for (char t : text) {
            if (t == '"' || t == '\\') out += '\\';
            out += t;
        }
        out += '"';
    }
    return out;
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

// `everything` lists the properties left out when they do not apply too
std::vector<std::pair<std::string, std::string>> resolve(const CSSComputedStyle& s, bool everything) {
    std::vector<std::pair<std::string, std::string>> out;
    auto add = [&](const char* n, const std::string& v) { out.emplace_back(n, v); };
    add("display", displayStr(s.display));
    add("position", positionStr(s.position));
    add("visibility", s.visibility == Visibility::Visible ? "visible" :
                      s.visibility == Visibility::Hidden ? "hidden" : "collapse");
    add("box-sizing", s.boxSizing == BoxSizing::BorderBox ? "border-box" : "content-box");
    add("opacity", formatComputedNumber(s.opacity));
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
    add("border-top-width", formatComputedNumber(s.borderTopWidth) + "px");
    add("border-right-width", formatComputedNumber(s.borderRightWidth) + "px");
    add("border-bottom-width", formatComputedNumber(s.borderBottomWidth) + "px");
    add("border-left-width", formatComputedNumber(s.borderLeftWidth) + "px");
    static const char* kBorderStyles[] = {"none", "hidden", "solid", "dashed", "dotted",
                                          "double", "groove", "ridge", "inset", "outset"};
    add("border-top-style", kBorderStyles[(int)s.borderTopStyle]);
    add("border-right-style", kBorderStyles[(int)s.borderRightStyle]);
    add("border-bottom-style", kBorderStyles[(int)s.borderBottomStyle]);
    add("border-left-style", kBorderStyles[(int)s.borderLeftStyle]);
    add("border-top-color", colorStr(s.borderTopColor));
    add("border-radius", s.borderTopLeftRadiusPercent > 0 ? formatComputedNumber(s.borderTopLeftRadiusPercent) + "%"
                                                           : formatComputedNumber(s.borderTopLeftRadius) + "px");
    add("overflow-x", overflowStr(s.overflowX));
    add("overflow-y", overflowStr(s.overflowY));
    add("top", lengthStr(s.top));
    add("right", lengthStr(s.right));
    add("bottom", lengthStr(s.bottom));
    add("left", lengthStr(s.left));
    add("z-index", s.zIndexAuto ? "auto" : std::to_string(s.zIndex));
    if (everything || s.display == DisplayValue::Flex || s.display == DisplayValue::InlineFlex) {
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
    add("flex-grow", formatComputedNumber(s.flexGrow));
    add("flex-shrink", formatComputedNumber(s.flexShrink));
    add("flex-basis", lengthStr(s.flexBasis));
    if (everything || s.order) add("order", std::to_string(s.order));
    if (everything || !s.gridTemplateColumns.empty()) add("grid-template-columns", s.gridTemplateColumns);
    if (everything || !s.gridTemplateRows.empty()) add("grid-template-rows", s.gridTemplateRows);
    add("color", colorStr(s.color));
    add("background-color", colorStr(s.backgroundColor));
    if (everything || !s.backgroundImage.empty()) add("background-image", s.backgroundImage);
    add("font-family", s.fontFamily);
    add("font-size", formatComputedNumber(s.fontSize) + "px");
    add("font-weight", std::to_string((int)s.fontWeight));
    add("font-style", s.fontStyle == FontStyle::Normal ? "normal" :
                      s.fontStyle == FontStyle::Italic ? "italic" : "oblique");
    add("line-height", s.lineHeight > 4 ? formatComputedNumber(s.lineHeight) + "px" : formatComputedNumber(s.lineHeight));
    add("text-align", textAlignStr(s.textAlign));
    if (everything || !s.textDecoration.empty()) add("text-decoration", s.textDecoration);
    if (everything || !s.textTransform.empty()) add("text-transform", s.textTransform);
    if (everything || s.letterSpacing != 0) add("letter-spacing", formatComputedNumber(s.letterSpacing) + "px");
    add("white-space", s.whiteSpace);
    add("vertical-align", s.verticalAlign);
    if (everything || !s.content.empty()) add("content", contentStr(s.content));
    add("cursor", s.cursor);
    add("pointer-events", s.pointerEvents);
    if (everything || !s.boxShadow.empty()) add("box-shadow", s.boxShadow);
    if (everything || !s.transform.empty()) add("transform", s.transform);
    if (everything || !s.transition.empty()) add("transition", s.transition);
    return out;
}

} // namespace

std::vector<std::pair<std::string, std::string>> computedStyleProperties(const CSSComputedStyle& style) {
    auto out = resolve(style, false);
    // Custom properties come after the standard ones, as browsers list them
    if (style.customProperties) {
        for (const auto& [name, value] : *style.customProperties) out.emplace_back(name, value);
    }
    return out;
}

const std::vector<std::string>& computedStylePropertyNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const auto& entry : resolve(CSSComputedStyle(), true)) out.push_back(entry.first);
        return out;
    }();
    return names;
}

} // namespace Zepra::WebCore
