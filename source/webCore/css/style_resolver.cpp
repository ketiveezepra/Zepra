// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * style_resolver.cpp - CSS Style Resolution
 * 
 * Implements style resolution per W3C CSS Cascade spec:
 * 1. Collect matching rules from all stylesheets
 * 2. Apply cascade to determine winning values
 * 3. Handle inheritance for inheritable properties
 * 4. Compute final values
 * 
 */

#include "css/css_engine.hpp"
#include "browser/dom.hpp"
#include <sstream>
#include <algorithm>
#include <iostream>
#include <cmath>
#include <unordered_set>

namespace Zepra::WebCore {

// ============================================================================
// Helpers
// ============================================================================

static std::vector<std::string> splitValues(const std::string& value) {
    std::vector<std::string> parts;
    std::istringstream ss(value);
    std::string token;
    while (ss >> token) {
        if (!token.empty()) parts.push_back(token);
    }
    return parts;
}

// ============================================================================
// Properties that inherit (per CSS spec)
// ============================================================================

// Static list of inheritable properties (from MDN)
static const std::unordered_set<std::string> INHERITED_PROPERTIES = {
    // Typography
    "color", "font", "font-family", "font-size", "font-style", 
    "font-weight", "font-variant", "line-height", "letter-spacing",
    "word-spacing", "text-align", "text-indent", "text-transform",
    "white-space", "direction",
    // Lists
    "list-style", "list-style-image", "list-style-position", "list-style-type",
    // Other
    "visibility", "cursor", "quotes", "orphans", "widows"
};

// ============================================================================
// StyleResolver Implementation
// ============================================================================

StyleResolver::StyleResolver() {
    // UA stylesheet now loaded externally via CSSEngine::addStyleSheet()
    // with StyleOrigin::UserAgent — no duplicate needed here
}

StyleResolver::~StyleResolver() = default;

void StyleResolver::addStyleSheet(std::shared_ptr<CSSStyleSheet> sheet, StyleOrigin origin) {
    stylesheets_.push_back({std::move(sheet), origin});
    cascade_.invalidateIndex();
}

void StyleResolver::addUserAgentStyleSheet() {
    // Default browser styles (simplified user-agent stylesheet)
    const std::string uaStyles = R"(
        html, body { display: block; margin: 8px; }
        head, script, style, meta, link, title { display: none; }
        
        h1 { display: block; font-size: 2em; font-weight: bold; margin: 0.67em 0; }
        h2 { display: block; font-size: 1.5em; font-weight: bold; margin: 0.83em 0; }
        h3 { display: block; font-size: 1.17em; font-weight: bold; margin: 1em 0; }
        h4 { display: block; font-weight: bold; margin: 1.33em 0; }
        h5 { display: block; font-size: 0.83em; font-weight: bold; margin: 1.67em 0; }
        h6 { display: block; font-size: 0.67em; font-weight: bold; margin: 2.33em 0; }
        
        p { display: block; margin: 1em 0; }
        div { display: block; }
        span { display: inline; }
        
        a { color: #0066cc; text-decoration: underline; cursor: pointer; }
        a:visited { color: #551a8b; }
        
        strong, b { font-weight: bold; }
        em, i { font-style: italic; }
        u { text-decoration: underline; }
        s, strike { text-decoration: line-through; }
        
        pre, code { font-family: monospace; }
        pre { white-space: pre; display: block; margin: 1em 0; }
        code { white-space: pre-wrap; }
        
        ul, ol { display: block; margin: 1em 0; padding-left: 40px; }
        li { display: list-item; }
        
        table { display: table; border-collapse: separate; border-spacing: 2px; }
        tr { display: table-row; }
        td, th { display: table-cell; padding: 1px; }
        th { font-weight: bold; text-align: center; }
        
        img { display: inline-block; }
        
        input, textarea, select, button { display: inline-block; }
    )";
    
    CSSParser parser;
    auto sheet = parser.parse(uaStyles);
    if (sheet) {
        stylesheets_.push_back({std::move(sheet), StyleOrigin::UserAgent});
    }
}

CSSComputedStyle StyleResolver::computeStyle(DOMElement* element, const CSSComputedStyle* parentStyle) {
    CSSComputedStyle computed;
    
    if (!element) return computed;
    
    // Step 1: Start with inherited values from parent
    if (parentStyle) {
        computed = CSSComputedStyle::inherit(*parentStyle);
    }
    
    // Step 2: Collect all matching rules WITH correct per-sheet origins
    std::vector<std::pair<CSSStyleSheet*, StyleOrigin>> sheetPairs;
    for (const auto& entry : stylesheets_) {
        sheetPairs.push_back({entry.sheet.get(), entry.origin});
    }
    
    auto matchedRules = cascade_.collectMatchingRulesWithOrigins(element, sheetPairs);
    
    // Step 3: Sort by cascade order
    cascade_.sortByCascade(matchedRules);
    
    // Step 4: Apply matched rules in order (lowest priority first)
    for (const auto& match : matchedRules) {
        if (match.rule && match.rule->style()) {
            applyDeclarations(computed, match.rule->style(), parentStyle);
        }
    }
    
    // Step 5: Apply inline styles (highest priority)
    std::string inlineStyle = element->getAttribute("style");
    if (!inlineStyle.empty()) {
        CSSParser parser;
        auto decl = parser.parseInlineStyle(inlineStyle);
        if (decl) {
            applyDeclarations(computed, decl.get(), parentStyle);
        }
    }
    
    return computed;
}

const CSSComputedStyle* StyleResolver::getComputedStyle(DOMElement* element) {
    if (!element) return nullptr;
    
    // Check cache
    auto it = styleCache_.find(element);
    if (it != styleCache_.end()) {
        return &it->second;
    }
    
    // Compute parent style first (for inheritance)
    const CSSComputedStyle* parentStyle = nullptr;
    if (auto* parent = dynamic_cast<DOMElement*>(element->parentNode())) {
        parentStyle = getComputedStyle(parent);
    }
    
    // Compute and cache
    CSSComputedStyle style = computeStyle(element, parentStyle);
    styleCache_[element] = std::move(style);
    
    return &styleCache_[element];
}

void StyleResolver::invalidateElement(DOMElement* element) {
    styleCache_.erase(element);
}

void StyleResolver::invalidateAll() {
    styleCache_.clear();
}

// ============================================================================
// Property Application
// ============================================================================

void StyleResolver::applyDeclarations(CSSComputedStyle& style, 
                                       const CSSStyleDeclaration* decl,
                                       const CSSComputedStyle* parentStyle) {
    if (!decl) return;
    
    // Apply each property in the declaration
    for (size_t i = 0; i < decl->length(); i++) {
        std::string property = decl->item(i);
        std::string value = decl->getPropertyValue(property);
        
        if (!value.empty()) {
            applyProperty(style, property, value, parentStyle);
        }
    }
}

void StyleResolver::applyProperty(CSSComputedStyle& style,
                                   const std::string& property,
                                   const std::string& rawValue,
                                   const CSSComputedStyle* parentStyle) {
    // Handle 'inherit' keyword
    if (rawValue == "inherit" && parentStyle) {
        if (property == "color") style.color = parentStyle->color;
        else if (property == "font-family") style.fontFamily = parentStyle->fontFamily;
        else if (property == "font-size") style.fontSize = parentStyle->fontSize;
        else if (property == "font-weight") style.fontWeight = parentStyle->fontWeight;
        else if (property == "font-style") style.fontStyle = parentStyle->fontStyle;
        else if (property == "line-height") style.lineHeight = parentStyle->lineHeight;
        else if (property == "text-align") style.textAlign = parentStyle->textAlign;
        else if (property == "letter-spacing") style.letterSpacing = parentStyle->letterSpacing;
        else if (property == "word-spacing") style.wordSpacing = parentStyle->wordSpacing;
        else if (property == "white-space") style.whiteSpace = parentStyle->whiteSpace;
        else if (property == "visibility") style.visibility = parentStyle->visibility;
        else if (property == "cursor") style.cursor = parentStyle->cursor;
        return;
    }
    
    // Handle 'initial' keyword
    if (rawValue == "initial") {
        if (property == "color") style.color = CSSColor::black();
        else if (property == "font-size") style.fontSize = 16.0f;
        else if (property == "font-weight") style.fontWeight = FontWeight::Normal;
        else if (property == "display") style.display = DisplayValue::Inline;
        else if (property == "position") style.position = PositionValue::Static;
        else if (property == "opacity") style.opacity = 1.0f;
        return;
    }
    
    // Handle CSS custom properties: var(--name) or var(--name, fallback)
    // We don't track custom property values yet, so extract the fallback.
    // If no fallback, skip — leave inherited/default value intact.
    std::string resolvedValue = rawValue;
    if (rawValue.find("var(") != std::string::npos) {
        // Extract fallback from var(--name, fallback)
        size_t varStart = rawValue.find("var(");
        size_t commaPos = rawValue.find(',', varStart);
        size_t closePos = rawValue.rfind(')');
        
        if (commaPos != std::string::npos && closePos != std::string::npos && commaPos < closePos) {
            // Has fallback: var(--x, red) → "red"
            resolvedValue = rawValue.substr(commaPos + 1, closePos - commaPos - 1);
            // Trim whitespace
            size_t s = resolvedValue.find_first_not_of(" \t");
            size_t e = resolvedValue.find_last_not_of(" \t");
            if (s != std::string::npos) {
                resolvedValue = resolvedValue.substr(s, e - s + 1);
            }
            // If fallback itself contains var(), give up
            if (resolvedValue.find("var(") != std::string::npos) return;
        } else {
            // No fallback: var(--x) — skip this property entirely
            return;
        }
    }
    
    // Handle calc() with var() — skip (can't resolve)
    if (resolvedValue.find("var(") != std::string::npos) return;
    
    // Use resolved value for all property checks below
    const std::string& value = resolvedValue;

    // =====================================================================
    // Display
    // =====================================================================
    if (property == "display") {
        if (value == "none") style.display = DisplayValue::None;
        else if (value == "block") style.display = DisplayValue::Block;
        else if (value == "inline") style.display = DisplayValue::Inline;
        else if (value == "inline-block") style.display = DisplayValue::InlineBlock;
        else if (value == "flex") style.display = DisplayValue::Flex;
        else if (value == "inline-flex") style.display = DisplayValue::InlineFlex;
        else if (value == "grid") style.display = DisplayValue::Grid;
        else if (value == "inline-grid") style.display = DisplayValue::InlineGrid;
        else if (value == "table") style.display = DisplayValue::Table;
        else if (value == "table-row") style.display = DisplayValue::TableRow;
        else if (value == "table-cell") style.display = DisplayValue::TableCell;
        else if (value == "list-item") style.display = DisplayValue::ListItem;
        else if (value == "contents") style.display = DisplayValue::Contents;
    }

    // =====================================================================
    // Position
    // =====================================================================
    else if (property == "position") {
        if (value == "static") style.position = PositionValue::Static;
        else if (value == "relative") style.position = PositionValue::Relative;
        else if (value == "absolute") style.position = PositionValue::Absolute;
        else if (value == "fixed") style.position = PositionValue::Fixed;
        else if (value == "sticky") style.position = PositionValue::Sticky;
    }
    else if (property == "top") { style.top = parseLength(value); }
    else if (property == "right") { style.right = parseLength(value); }
    else if (property == "bottom") { style.bottom = parseLength(value); }
    else if (property == "left") { style.left = parseLength(value); }
    else if (property == "z-index") {
        if (value == "auto") { style.zIndexAuto = true; }
        else { try { style.zIndex = std::stoi(value); style.zIndexAuto = false; } catch (...) {} }
    }
    else if (property == "float") {
        if (value == "left") style.cssFloat = 1;
        else if (value == "right") style.cssFloat = 2;
        else style.cssFloat = 0;
    }

    // =====================================================================
    // Typography
    // =====================================================================
    else if (property == "color") { style.color = parseColor(value); }
    else if (property == "font-size") {
        CSSLength len = parseLength(value);
        if (!len.isAuto()) {
            float parentFontSize = parentStyle ? parentStyle->fontSize : 16.0f;
            style.fontSize = len.toPx(parentFontSize, 16.0f, 1920, 1080, 0);
        }
    }
    else if (property == "font-family") { style.fontFamily = value; }
    else if (property == "font-weight") {
        if (value == "bold" || value == "700") style.fontWeight = FontWeight::Bold;
        else if (value == "normal" || value == "400") style.fontWeight = FontWeight::Normal;
        else if (value == "lighter" || value == "100") style.fontWeight = FontWeight::Lighter;
        else if (value == "bolder" || value == "900") style.fontWeight = FontWeight::Bolder;
        else if (value == "200") style.fontWeight = FontWeight::W200;
        else if (value == "300") style.fontWeight = FontWeight::W300;
        else if (value == "500") style.fontWeight = FontWeight::W500;
        else if (value == "600") style.fontWeight = FontWeight::W600;
        else if (value == "800") style.fontWeight = FontWeight::W800;
    }
    else if (property == "font-style") {
        if (value == "italic") style.fontStyle = FontStyle::Italic;
        else if (value == "oblique") style.fontStyle = FontStyle::Oblique;
        else style.fontStyle = FontStyle::Normal;
    }
    else if (property == "text-align") {
        if (value == "left" || value == "start") style.textAlign = TextAlign::Left;
        else if (value == "right" || value == "end") style.textAlign = TextAlign::Right;
        else if (value == "center") style.textAlign = TextAlign::Center;
        else if (value == "justify") style.textAlign = TextAlign::Justify;
    }
    else if (property == "text-decoration" || property == "text-decoration-line") {
        style.textDecoration = value;
    }
    else if (property == "text-transform") { style.textTransform = value; }
    else if (property == "letter-spacing") {
        CSSLength len = parseLength(value);
        if (!len.isAuto()) style.letterSpacing = len.toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
    else if (property == "word-spacing") {
        CSSLength len = parseLength(value);
        if (!len.isAuto()) style.wordSpacing = len.toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
    else if (property == "white-space") { style.whiteSpace = value; }
    else if (property == "line-height") {
        if (value == "normal") { style.lineHeight = 1.2f; }
        else if (value.find_first_of("0123456789.") != std::string::npos) {
            try {
                float val = std::stof(value);
                if (value.back() == '%') style.lineHeight = val / 100.0f;
                else if (value.find("px") != std::string::npos) style.lineHeight = val / style.fontSize;
                else style.lineHeight = val;
            } catch (...) {}
        }
    }

    // =====================================================================
    // Dimensions
    // =====================================================================
    else if (property == "width") { style.width = parseLength(value); }
    else if (property == "height") { style.height = parseLength(value); }
    else if (property == "min-width") { style.minWidth = parseLength(value); }
    else if (property == "min-height") { style.minHeight = parseLength(value); }
    else if (property == "max-width") { style.maxWidth = parseLength(value); }
    else if (property == "max-height") { style.maxHeight = parseLength(value); }

    // =====================================================================
    // Margin
    // =====================================================================
    else if (property == "margin") {
        auto parts = splitValues(value);
        if (parts.size() == 1) {
            CSSLength len = parseLength(parts[0]);
            style.marginTop = len; style.marginRight = len;
            style.marginBottom = len; style.marginLeft = len;
        } else if (parts.size() == 2) {
            style.marginTop = parseLength(parts[0]);
            style.marginRight = parseLength(parts[1]);
            style.marginBottom = parseLength(parts[0]);
            style.marginLeft = parseLength(parts[1]);
        } else if (parts.size() == 3) {
            style.marginTop = parseLength(parts[0]);
            style.marginRight = parseLength(parts[1]);
            style.marginBottom = parseLength(parts[2]);
            style.marginLeft = parseLength(parts[1]);
        } else if (parts.size() >= 4) {
            style.marginTop = parseLength(parts[0]);
            style.marginRight = parseLength(parts[1]);
            style.marginBottom = parseLength(parts[2]);
            style.marginLeft = parseLength(parts[3]);
        }
    }
    else if (property == "margin-top") { style.marginTop = parseLength(value); }
    else if (property == "margin-right") { style.marginRight = parseLength(value); }
    else if (property == "margin-bottom") { style.marginBottom = parseLength(value); }
    else if (property == "margin-left") { style.marginLeft = parseLength(value); }

    // =====================================================================
    // Padding
    // =====================================================================
    else if (property == "padding") {
        auto parts = splitValues(value);
        if (parts.size() == 1) {
            CSSLength len = parseLength(parts[0]);
            style.paddingTop = len; style.paddingRight = len;
            style.paddingBottom = len; style.paddingLeft = len;
        } else if (parts.size() == 2) {
            style.paddingTop = parseLength(parts[0]);
            style.paddingRight = parseLength(parts[1]);
            style.paddingBottom = parseLength(parts[0]);
            style.paddingLeft = parseLength(parts[1]);
        } else if (parts.size() == 3) {
            style.paddingTop = parseLength(parts[0]);
            style.paddingRight = parseLength(parts[1]);
            style.paddingBottom = parseLength(parts[2]);
            style.paddingLeft = parseLength(parts[1]);
        } else if (parts.size() >= 4) {
            style.paddingTop = parseLength(parts[0]);
            style.paddingRight = parseLength(parts[1]);
            style.paddingBottom = parseLength(parts[2]);
            style.paddingLeft = parseLength(parts[3]);
        }
    }
    else if (property == "padding-top") { style.paddingTop = parseLength(value); }
    else if (property == "padding-right") { style.paddingRight = parseLength(value); }
    else if (property == "padding-bottom") { style.paddingBottom = parseLength(value); }
    else if (property == "padding-left") { style.paddingLeft = parseLength(value); }

    // =====================================================================
    // Border
    // =====================================================================
    else if (property == "border-width") {
        CSSLength len = parseLength(value);
        float px = len.toPx(style.fontSize, 16.0f, 1920, 1080, 0);
        style.borderTopWidth = px; style.borderRightWidth = px;
        style.borderBottomWidth = px; style.borderLeftWidth = px;
    }
    else if (property == "border-top-width") {
        style.borderTopWidth = parseLength(value).toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
    else if (property == "border-right-width") {
        style.borderRightWidth = parseLength(value).toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
    else if (property == "border-bottom-width") {
        style.borderBottomWidth = parseLength(value).toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
    else if (property == "border-left-width") {
        style.borderLeftWidth = parseLength(value).toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
    else if (property == "border-color") {
        CSSColor c = parseColor(value);
        style.borderTopColor = c; style.borderRightColor = c;
        style.borderBottomColor = c; style.borderLeftColor = c;
    }
    else if (property == "border-top-color") { style.borderTopColor = parseColor(value); }
    else if (property == "border-right-color") { style.borderRightColor = parseColor(value); }
    else if (property == "border-bottom-color") { style.borderBottomColor = parseColor(value); }
    else if (property == "border-left-color") { style.borderLeftColor = parseColor(value); }
    else if (property == "border-radius") {
        CSSLength len = parseLength(value);
        float px = len.toPx(style.fontSize, 16.0f, 1920, 1080, 0);
        style.borderTopLeftRadius = px; style.borderTopRightRadius = px;
        style.borderBottomRightRadius = px; style.borderBottomLeftRadius = px;
    }
    else if (property == "border-top-left-radius") {
        style.borderTopLeftRadius = parseLength(value).toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
    else if (property == "border-top-right-radius") {
        style.borderTopRightRadius = parseLength(value).toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
    else if (property == "border-bottom-right-radius") {
        style.borderBottomRightRadius = parseLength(value).toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
    else if (property == "border-bottom-left-radius") {
        style.borderBottomLeftRadius = parseLength(value).toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
    else if (property == "border" || property == "border-top" || 
             property == "border-right" || property == "border-bottom" || 
             property == "border-left") {
        // Shorthand: "1px solid #000"
        // Parse width, style, color from space-separated values
        std::istringstream ss(value);
        std::string token;
        float bw = 0;
        CSSColor bc = CSSColor::black();
        while (ss >> token) {
            if (token == "none") { bw = 0; break; }
            if (token == "solid" || token == "dashed" || token == "dotted" || 
                token == "double" || token == "groove" || token == "ridge" ||
                token == "inset" || token == "outset") continue;
            if (token == "thin") { bw = 1; continue; }
            if (token == "medium") { bw = 3; continue; }
            if (token == "thick") { bw = 5; continue; }
            if (token[0] == '#' || token.substr(0,3) == "rgb" || 
                CSSColor::parse(token).a != 0 || token == "transparent") {
                bc = CSSColor::parse(token); continue;
            }
            CSSLength len = parseLength(token);
            if (!len.isAuto()) bw = len.toPx(style.fontSize, 16.0f, 1920, 1080, 0);
        }
        if (property == "border") {
            style.borderTopWidth = bw; style.borderRightWidth = bw;
            style.borderBottomWidth = bw; style.borderLeftWidth = bw;
            style.borderTopColor = bc; style.borderRightColor = bc;
            style.borderBottomColor = bc; style.borderLeftColor = bc;
        } else if (property == "border-top") { style.borderTopWidth = bw; style.borderTopColor = bc; }
        else if (property == "border-right") { style.borderRightWidth = bw; style.borderRightColor = bc; }
        else if (property == "border-bottom") { style.borderBottomWidth = bw; style.borderBottomColor = bc; }
        else if (property == "border-left") { style.borderLeftWidth = bw; style.borderLeftColor = bc; }
    }

    // =====================================================================
    // Flexbox
    // =====================================================================
    else if (property == "flex-direction") {
        if (value == "row") style.flexDirection = FlexDirection::Row;
        else if (value == "row-reverse") style.flexDirection = FlexDirection::RowReverse;
        else if (value == "column") style.flexDirection = FlexDirection::Column;
        else if (value == "column-reverse") style.flexDirection = FlexDirection::ColumnReverse;
    }
    else if (property == "flex-wrap") {
        style.flexWrap = (value == "wrap" || value == "wrap-reverse");
        style.wrapReverse = (value == "wrap-reverse");
    }
    else if (property == "justify-content") {
        if (value == "flex-start" || value == "start") style.justifyContent = JustifyAlign::FlexStart;
        else if (value == "flex-end" || value == "end") style.justifyContent = JustifyAlign::FlexEnd;
        else if (value == "center") style.justifyContent = JustifyAlign::Center;
        else if (value == "space-between") style.justifyContent = JustifyAlign::SpaceBetween;
        else if (value == "space-around") style.justifyContent = JustifyAlign::SpaceAround;
        else if (value == "space-evenly") style.justifyContent = JustifyAlign::SpaceEvenly;
    }
    else if (property == "align-items") {
        if (value == "flex-start" || value == "start") style.alignItems = JustifyAlign::FlexStart;
        else if (value == "flex-end" || value == "end") style.alignItems = JustifyAlign::FlexEnd;
        else if (value == "center") style.alignItems = JustifyAlign::Center;
        else if (value == "stretch") style.alignItems = JustifyAlign::Stretch;
        else if (value == "baseline") style.alignItems = JustifyAlign::Baseline;
    }
    else if (property == "align-content") {
        if (value == "flex-start" || value == "start") style.alignContent = JustifyAlign::FlexStart;
        else if (value == "flex-end" || value == "end") style.alignContent = JustifyAlign::FlexEnd;
        else if (value == "center") style.alignContent = JustifyAlign::Center;
        else if (value == "stretch") style.alignContent = JustifyAlign::Stretch;
        else if (value == "space-between") style.alignContent = JustifyAlign::SpaceBetween;
        else if (value == "space-around") style.alignContent = JustifyAlign::SpaceAround;
    }
    else if (property == "align-self") {
        if (value == "auto" || value == "flex-start" || value == "start") style.alignSelf = JustifyAlign::Start;
        else if (value == "flex-end" || value == "end") style.alignSelf = JustifyAlign::End;
        else if (value == "center") style.alignSelf = JustifyAlign::Center;
        else if (value == "stretch") style.alignSelf = JustifyAlign::Stretch;
        else if (value == "baseline") style.alignSelf = JustifyAlign::Baseline;
    }
    else if (property == "flex-grow") {
        try { style.flexGrow = std::stof(value); } catch (...) {}
    }
    else if (property == "flex-shrink") {
        try { style.flexShrink = std::stof(value); } catch (...) {}
    }
    else if (property == "flex-basis") { style.flexBasis = parseLength(value); }
    else if (property == "flex") {
        // Shorthand: "1" or "1 0 auto" or "none" or "auto"
        if (value == "none") { style.flexGrow = 0; style.flexShrink = 0; style.flexBasis = CSSLength::auto_(); }
        else if (value == "auto") { style.flexGrow = 1; style.flexShrink = 1; style.flexBasis = CSSLength::auto_(); }
        else {
            try { style.flexGrow = std::stof(value); style.flexShrink = 1; style.flexBasis = CSSLength::px(0); } catch (...) {}
        }
    }
    else if (property == "order") {
        try { style.order = std::stoi(value); } catch (...) {}
    }
    else if (property == "gap" || property == "grid-gap") {
        style.gap = parseLength(value);
        style.rowGap = style.gap;
        style.columnGap = style.gap;
    }
    else if (property == "row-gap") { style.rowGap = parseLength(value); }
    else if (property == "column-gap") { style.columnGap = parseLength(value); }

    // =====================================================================
    // Grid
    // =====================================================================
    else if (property == "grid-template-columns") { style.gridTemplateColumns = value; }
    else if (property == "grid-template-rows") { style.gridTemplateRows = value; }

    // =====================================================================
    // Background & Visual
    // =====================================================================
    else if (property == "background-color") { style.backgroundColor = parseColor(value); }
    else if (property == "background-image") { style.backgroundImage = value; }
    else if (property == "background-size") { style.backgroundSize = value; }
    else if (property == "background-position") { style.backgroundPosition = value; }
    else if (property == "background-repeat") { style.backgroundRepeat = value; }
    else if (property == "background") {
        // Shorthand: try color first, then store full value for image/gradient
        if (value.find("url(") != std::string::npos || value.find("gradient") != std::string::npos) {
            style.backgroundImage = value;
        } else if (value != "none") {
            style.backgroundColor = parseColor(value);
        }
    }
    else if (property == "opacity") {
        try { style.opacity = std::stof(value); } catch (...) {}
    }
    else if (property == "box-shadow") { style.boxShadow = value; }
    else if (property == "transform") { style.transform = value; }
    else if (property == "transition") { style.transition = value; }
    else if (property == "animation") { style.animation = value; }
    else if (property == "filter") { style.filter = value; }

    // =====================================================================
    // Overflow & Box
    // =====================================================================
    else if (property == "overflow") {
        OverflowValue ov = OverflowValue::Visible;
        if (value == "hidden") ov = OverflowValue::Hidden;
        else if (value == "scroll") ov = OverflowValue::Scroll;
        else if (value == "auto") ov = OverflowValue::Auto;
        else if (value == "clip") ov = OverflowValue::Clip;
        style.overflowX = ov; style.overflowY = ov;
    }
    else if (property == "overflow-x") {
        if (value == "hidden") style.overflowX = OverflowValue::Hidden;
        else if (value == "scroll") style.overflowX = OverflowValue::Scroll;
        else if (value == "auto") style.overflowX = OverflowValue::Auto;
        else style.overflowX = OverflowValue::Visible;
    }
    else if (property == "overflow-y") {
        if (value == "hidden") style.overflowY = OverflowValue::Hidden;
        else if (value == "scroll") style.overflowY = OverflowValue::Scroll;
        else if (value == "auto") style.overflowY = OverflowValue::Auto;
        else style.overflowY = OverflowValue::Visible;
    }
    else if (property == "box-sizing") {
        if (value == "border-box") style.boxSizing = BoxSizing::BorderBox;
        else style.boxSizing = BoxSizing::ContentBox;
    }
    else if (property == "visibility") {
        if (value == "hidden") style.visibility = Visibility::Hidden;
        else if (value == "collapse") style.visibility = Visibility::Collapse;
        else style.visibility = Visibility::Visible;
    }
    else if (property == "cursor") { style.cursor = value; }
    else if (property == "pointer-events") { style.pointerEvents = value; }

    // =====================================================================
    // Tailwind / Modern CSS Properties
    // =====================================================================
    else if (property == "text-overflow") { style.textOverflow = value; }
    else if (property == "object-fit") { style.objectFit = value; }
    else if (property == "object-position") { style.objectPosition = value; }
    else if (property == "aspect-ratio") { style.aspectRatio = value; }
    else if (property == "backdrop-filter" || property == "-webkit-backdrop-filter") {
        style.backdropFilter = value;
    }
    else if (property == "place-items") {
        style.placeItems = value;
        // Shorthand: sets align-items and justify-items
        auto parts = splitValues(value);
        if (parts.size() == 1) {
            if (value == "center") {
                style.alignItems = JustifyAlign::Center;
                style.justifyContent = JustifyAlign::Center;
            } else if (value == "stretch") {
                style.alignItems = JustifyAlign::Stretch;
            }
        }
    }
    else if (property == "place-content") {
        style.placeContent = value;
        auto parts = splitValues(value);
        if (parts.size() == 1) {
            if (value == "center") {
                style.alignContent = JustifyAlign::Center;
                style.justifyContent = JustifyAlign::Center;
            }
        }
    }
    else if (property == "isolation") { style.isolation = value; }
    else if (property == "will-change") { style.willChange = value; }
    else if (property == "content") { style.content = value; }
    else if (property == "user-select" || property == "-webkit-user-select") {
        style.userSelect = value;
    }
    else if (property == "appearance" || property == "-webkit-appearance") {
        style.appearance = value;
    }
    else if (property == "outline") {
        // Shorthand: "2px solid #000" or "none"
        if (value == "none" || value == "0") {
            style.outlineWidth = 0;
            style.outlineStyle = "none";
        } else {
            style.outlineStyle = "solid";
            std::istringstream ss(value);
            std::string token;
            while (ss >> token) {
                if (token == "solid" || token == "dashed" || token == "dotted") {
                    style.outlineStyle = token;
                } else if (token == "none") {
                    style.outlineWidth = 0;
                } else if (token[0] == '#' || token.substr(0,3) == "rgb") {
                    style.outlineColor = parseColor(token);
                } else {
                    CSSLength len = parseLength(token);
                    if (!len.isAuto()) style.outlineWidth = len.toPx(style.fontSize, 16.0f, 1920, 1080, 0);
                }
            }
        }
    }
    else if (property == "outline-width") {
        CSSLength len = parseLength(value);
        if (!len.isAuto()) style.outlineWidth = len.toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
    else if (property == "outline-color") { style.outlineColor = parseColor(value); }
    else if (property == "outline-style") { style.outlineStyle = value; }
    else if (property == "outline-offset") {
        CSSLength len = parseLength(value);
        if (!len.isAuto()) style.outlineOffset = len.toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
}

// ============================================================================
// Value Parsing
// ============================================================================

CSSLength StyleResolver::parseLength(const std::string& value) {
    if (value.empty() || value == "auto") {
        return CSSLength::auto_();
    }
    
    float numValue = 0;
    CSSLength::Unit unit = CSSLength::Unit::Px;
    
    try {
        size_t pos = 0;
        numValue = std::stof(value, &pos);
        
        std::string unitStr = value.substr(pos);
        
        if (unitStr == "px" || unitStr.empty()) unit = CSSLength::Unit::Px;
        else if (unitStr == "em") unit = CSSLength::Unit::Em;
        else if (unitStr == "rem") unit = CSSLength::Unit::Rem;
        else if (unitStr == "%") unit = CSSLength::Unit::Percent;
        else if (unitStr == "vw") unit = CSSLength::Unit::Vw;
        else if (unitStr == "vh") unit = CSSLength::Unit::Vh;
        else if (unitStr == "pt") unit = CSSLength::Unit::Pt;
    } catch (...) {
        return CSSLength::auto_();
    }
    
    return {numValue, unit};
}

CSSColor StyleResolver::parseColor(const std::string& value) {
    return CSSColor::parse(value);
}

} // namespace Zepra::WebCore
