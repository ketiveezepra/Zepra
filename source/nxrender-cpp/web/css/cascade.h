// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#pragma once

#include "property_ids.h"
#include "value_parser.h"
#include <vector>
#include <unordered_map>
#include <string>
#include <memory>
#include <functional>

namespace NXRender {
namespace Web {

// ==================================================================
// Cascade origin (CSS cascade spec)
// ==================================================================

enum class CascadeOrigin : uint8_t {
    UserAgent = 0,
    User = 1,
    Author = 2,
    AuthorImportant = 3,
    UserImportant = 4,
    UserAgentImportant = 5,
    Animation = 6,
    Transition = 7,
};

// ==================================================================
// Declaration — single property:value with metadata
// ==================================================================

struct CSSDeclaration {
    CSSPropertyID property;
    CSSValue value;
    bool important = false;
    CascadeOrigin origin = CascadeOrigin::Author;
    uint32_t specificity = 0;  // Packed (a << 24 | b << 16 | c << 8 | order)
    uint32_t sourceOrder = 0;

    bool operator<(const CSSDeclaration& other) const;
};

// ==================================================================
// Cascade result — per-element resolved declarations
// ==================================================================

struct CascadedValues {
    std::unordered_map<CSSPropertyID, CSSDeclaration> declarations;

    const CSSDeclaration* get(CSSPropertyID id) const;
    bool has(CSSPropertyID id) const;
    void set(const CSSDeclaration& decl);
};

// ==================================================================
// Computed values — final resolved pixel values
// ==================================================================

struct ComputedValues {
    // Box Model
    float width = 0, height = 0;
    bool widthAuto = true, heightAuto = true;
    float minWidth = 0, minHeight = 0;
    float maxWidth = 1e9f, maxHeight = 1e9f;

    // Margin (px)
    float marginTop = 0, marginRight = 0, marginBottom = 0, marginLeft = 0;
    bool marginTopAuto = false, marginRightAuto = false;
    bool marginBottomAuto = false, marginLeftAuto = false;

    // Padding (px)
    float paddingTop = 0, paddingRight = 0, paddingBottom = 0, paddingLeft = 0;

    // Border (px)
    float borderTopWidth = 0, borderRightWidth = 0;
    float borderBottomWidth = 0, borderLeftWidth = 0;
    uint32_t borderTopColor = 0, borderRightColor = 0;
    uint32_t borderBottomColor = 0, borderLeftColor = 0;
    uint8_t borderTopStyle = 0, borderRightStyle = 0;
    uint8_t borderBottomStyle = 0, borderLeftStyle = 0;
    float borderTopLeftRadius = 0, borderTopRightRadius = 0;
    float borderBottomRightRadius = 0, borderBottomLeftRadius = 0;

    // Positioning
    uint8_t display = 0;     // Maps to DisplayValue enum
    uint8_t position = 0;    // Maps to PositionValue enum
    float top = 0, right = 0, bottom = 0, left = 0;
    bool topAuto = true, rightAuto = true, bottomAuto = true, leftAuto = true;
    int zIndex = 0;
    bool zIndexAuto = true;
    uint8_t floatVal = 0;
    uint8_t clear = 0;

    // Overflow
    uint8_t overflowX = 0, overflowY = 0;

    // Flexbox
    uint8_t flexDirection = 0;
    bool flexWrap = false;
    uint8_t justifyContent = 0, alignItems = 0;
    uint8_t alignContent = 0, alignSelf = 0;
    float flexGrow = 0, flexShrink = 1;
    float flexBasis = 0;
    bool flexBasisAuto = true;
    int order = 0;
    float rowGap = 0, columnGap = 0;

    // Grid
    std::string gridTemplateColumns;
    std::string gridTemplateRows;
    std::string gridTemplateAreas;
    std::string gridAutoFlow;

    // Typography
    uint32_t color = 0x000000FF;
    std::string fontFamily = "sans-serif";
    float fontSize = 16.0f;
    uint16_t fontWeight = 400;
    uint8_t fontStyle = 0;
    float lineHeight = 1.2f;
    bool lineHeightNormal = true;
    float letterSpacing = 0;
    float wordSpacing = 0;
    uint8_t textAlign = 0;
    std::string textDecoration;
    std::string textTransform;
    float textIndent = 0;
    std::string whiteSpace = "normal";

    // Background
    uint32_t backgroundColor = 0x00000000;
    std::string backgroundImage;
    std::string backgroundSize;
    std::string backgroundPosition;
    std::string backgroundRepeat = "repeat";

    // Effects
    float opacity = 1.0f;
    uint8_t visibility = 0;  // Visible
    uint8_t boxSizing = 0;   // ContentBox
    std::string transform;
    std::string filter;
    std::string boxShadow;
    std::string textShadow;
    std::string cursor = "auto";
    std::string pointerEvents = "auto";
    std::string willChange;
    std::string isolation;

    // Outline
    float outlineWidth = 0;
    uint32_t outlineColor = 0;
    float outlineOffset = 0;
    std::string outlineStyle = "none";

    // Table (CSS 2.1 §17)
    int borderCollapse = 0;   // 0 = separate, 1 = collapse
    float borderSpacingH = 0;
    float borderSpacingV = 0;
    int tableLayout = 0;      // 0 = auto, 1 = fixed
    int colSpan = 1;
    int rowSpan = 1;

    // Pseudo-element
    std::string content;       // content property for ::before/::after
    std::string listStyleType; // disc, circle, square, decimal, etc.

    // Sizing
    std::string objectFit;
    std::string aspectRatio;
    std::string contain;
    std::string containerType;
    std::string containerName;

    // Multi-column
    int columnCount = 0;
    float columnWidth = 0;
    bool columnCountAuto = true;
    bool columnWidthAuto = true;

    // Fragmentation
    uint8_t breakBefore = 0, breakAfter = 0, breakInside = 0;
    int orphans = 2, widows = 2;
};

// ==================================================================
// Cascade engine
// ==================================================================

struct ViewportContext {
    float width = 1920;
    float height = 1080;
    float dpi = 96;
};

class CascadeEngine {
public:
    CascadeEngine();
    ~CascadeEngine();

    void setViewport(const ViewportContext& vp) { viewport_ = vp; }

    // Cascade: merge declarations from multiple sources
    CascadedValues cascade(const std::vector<CSSDeclaration>& declarations);

    // Compute: resolve cascaded values to final pixel values
    ComputedValues compute(const CascadedValues& cascaded,
                           const ComputedValues* parent,
                           float rootFontSize = 16.0f);

    // Apply inheritance for a single property
    CSSValue resolveInheritance(CSSPropertyID property,
                                 const CSSValue& cascaded,
                                 const ComputedValues* parent);

    // Shorthand expansion
    std::vector<CSSDeclaration> expandShorthand(const CSSDeclaration& decl);

private:
    ViewportContext viewport_;

    void applyProperty(CSSPropertyID id, const CSSValue& value,
                       ComputedValues& computed,
                       const ComputedValues* parent,
                       float fontSize, float rootFontSize);

    float resolveLengthPx(const CSSValue& value, float fontSize,
                           float rootFontSize, float containerSize = 0);

    uint8_t resolveDisplayKeyword(const std::string& keyword);
    uint8_t resolvePositionKeyword(const std::string& keyword);
    uint8_t resolveOverflowKeyword(const std::string& keyword);
    uint8_t resolveFlexDirectionKeyword(const std::string& keyword);
    uint8_t resolveJustifyAlignKeyword(const std::string& keyword);
    uint8_t resolveTextAlignKeyword(const std::string& keyword);
    uint8_t resolveVisibilityKeyword(const std::string& keyword);
    uint8_t resolveBoxSizingKeyword(const std::string& keyword);
};

} // namespace Web
} // namespace NXRender
