// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * layout_engine.h - CSS Layout Engine for ZepraBrowser
 * 
 * Implements Block Formatting Context (BFC) algorithm for laying out
 * HTML elements according to CSS box model rules.
 */

#pragma once

#include <string>
#include <vector>
#include <list>
#include <memory>
#include <cstdint>

namespace Zepra::WebCore { class DOMElement; }

namespace ZepraBrowser {

// =============================================================================
// LAYOUT TYPES
// =============================================================================

enum class LayoutType {
    Block,       // Block-level element (div, p, h1, etc.)
    Inline,      // Inline element (span, a, etc.)
    InlineBlock, // Inline-block element
    Flex,        // Flexbox container
    FlexItem,    // Child of flexbox container
    None,        // display: none
    Text,        // Text node
    // CSS 2.1 §17 table boxes; LayoutBox children follow the table model
    // (Table > RowGroup/Row/Caption, RowGroup > Row, Row > Cell), with
    // anonymous boxes filling the gaps (§17.2.1)
    Table,
    TableRowGroup,
    TableRow,
    TableCell,
    TableCaption
};

// =============================================================================
// LAYOUT LENGTH — Deferred CSS dimension resolution
// =============================================================================

struct LayoutLength {
    enum class Unit { None, Px, Percent, Em, Rem, Vw, Vh, Auto };

    float value = 0;
    Unit unit = Unit::None;

    bool isAuto() const { return unit == Unit::Auto; }
    bool isPercent() const { return unit == Unit::Percent; }
    bool isSet() const { return unit != Unit::None; }

    float resolve(float containerSize, float fontSize, float vpW, float vpH) const {
        switch (unit) {
            case Unit::Px:      return value;
            case Unit::Percent: return value / 100.0f * containerSize;
            case Unit::Em:      return value * fontSize;
            case Unit::Rem:     return value * 16.0f;
            case Unit::Vw:      return value / 100.0f * vpW;
            case Unit::Vh:      return value / 100.0f * vpH;
            default:            return 0;
        }
    }

    static LayoutLength none()    { return {0, Unit::None}; }
    static LayoutLength px(float v) { return {v, Unit::Px}; }
    static LayoutLength pct(float v) { return {v, Unit::Percent}; }
    static LayoutLength autoVal() { return {0, Unit::Auto}; }
};

// =============================================================================
// LAYOUT BOX
// =============================================================================

/**
 * @brief Represents a box in the layout tree with position and dimensions
 * 
 * Each LayoutBox corresponds to a DOM element and contains:
 * - Position (x, y relative to parent)
 * - Dimensions (width, height)
 * - Box model properties (margin, padding, border)
 * - Style properties for rendering
 * - Children in document order
 */
struct LayoutBox {
    // Position and dimensions
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
    
    // Cached screen position (calculated during paint)
    mutable float screenX = 0;
    mutable float screenY = 0;

    // DOM element this box was generated for (hit-testing for :hover/:active)
    Zepra::WebCore::DOMElement* domElement = nullptr;
    
    // Box model
    float marginTop = 0, marginRight = 0, marginBottom = 0, marginLeft = 0;
    float paddingTop = 0, paddingRight = 0, paddingBottom = 0, paddingLeft = 0;
    float borderTop = 0, borderRight = 0, borderBottom = 0, borderLeft = 0;
    
    // Box sizing context
    int boxSizing = 0; // 0=ContentBox, 1=BorderBox
    
    // Margin auto flags (for centering)
    bool marginLeftAuto = false;
    bool marginRightAuto = false;
    bool marginTopAuto = false;
    bool marginBottomAuto = false;
    
    // Raw CSS dimensions — resolved in layoutBlock() with correct containing width
    LayoutLength cssWidth;
    LayoutLength cssHeight;
    
    // Min/max dimension constraints
    LayoutLength cssMinWidth;
    LayoutLength cssMinHeight;
    LayoutLength cssMaxWidth;
    LayoutLength cssMaxHeight;
    
    // Display type
    LayoutType type = LayoutType::Block;
    int flexDirection = 0; // 0=Row, 1=Column, 2=RowReverse, 3=ColumnReverse
    
    // Content (for text nodes)
    std::string text;
    
    // Style properties for rendering
    uint32_t color = 0x000000;
    uint32_t bgColor = 0x00000000;
    uint32_t borderColor = 0x000000;
    bool borderVisible = true;   // false for a transparent border colour (it still takes space)
    float fontSize = 16.0f;
    bool bold = false;
    bool italic = false;
    bool hasBgColor = false;
    
    // Border radius (uniform for now)
    float borderRadius = 0;
    
    // Opacity (0.0 = transparent, 1.0 = opaque)
    float opacity = 1.0f;
    
    // Background image / gradient (CSS string)
    std::string backgroundImage;
    
    // Overflow clipping
    bool overflowHidden = false;
    
    // CSS Positioning: 0=static, 1=relative, 2=absolute, 3=fixed, 4=sticky
    int positionType = 0;
    LayoutLength cssTop;
    LayoutLength cssRight;
    LayoutLength cssBottom;
    LayoutLength cssLeft;
    int zIndex = 0;
    
    // Float: 0=none, 1=left, 2=right
    int floatType = 0;
    
    // Visibility: hidden (takes space in layout but not painted)
    bool visibilityHidden = false;
    
    // Flex container properties
    int justifyContent = 0; // 0=start, 1=end, 2=center, 3=space-between, 4=space-around, 5=space-evenly
    int alignItems = 0;     // 0=stretch, 1=start, 2=end, 3=center, 4=baseline
    int alignContent = 0;   // 0=stretch, 1=start, 2=end, 3=center, 4=space-between, 5=space-around
    bool flexWrap = false;
    bool wrapReverse = false;
    float gap = 0;
    float rowGap = 0;       // Explicit row-gap (fallback to gap)
    float columnGap = 0;    // Explicit column-gap (fallback to gap)

    // Flex item properties
    float flexGrow = 0;
    float flexShrink = 1;
    LayoutLength flexBasis;  // auto = use content size, otherwise resolved value
    int alignSelf = -1;      // -1=auto (inherit alignItems), 0=stretch, 1=start, 2=end, 3=center
    int order = 0;
    
    // Text decoration
    std::string textDecoration;
    
    // Text alignment: 0=left, 1=center, 2=right
    int textAlign = 0;
    // HTML "align descendants" (<center>, align attributes): block children
    // narrower than this box are aligned too. 0=none, 1=left, 2=center, 3=right
    int alignChildren = 0;

    // Tables
    bool anonymous = false;      // generated by the table fix-up, no element of its own
    float borderSpacingH = 0;    // Table: horizontal / vertical border-spacing
    float borderSpacingV = 0;
    bool tableLayoutFixed = false;
    int rowGroupKind = 0;        // TableRowGroup: 0=body, 1=header, 2=footer
    int colSpan = 1;             // TableCell
    int rowSpan = 1;             // TableCell: 0 = to the end of the row group
    int verticalAlign = 0;       // TableCell: 0=baseline, 1=top, 2=middle, 3=bottom
    bool noWrap = false;         // TableCell: white-space nowrap/pre

    // Min-/max-content border-box widths, cached per viewport width
    mutable float intrinsicMin = 0;
    mutable float intrinsicMax = 0;
    mutable int intrinsicKey = -1;
    
    // Input properties
    bool isInput = false;
    std::string inputType;
    std::string placeholder;
    
    // Link properties
    bool isLink = false;
    std::string href;
    std::string target;

    // Image properties
    bool isImage = false;
    uint32_t textureId = 0; // 0 = no texture
    std::string svgData;    // Raw SVG string for NxSVG rendering on main thread
    mutable std::string svgKey; // Cached hash key for NxSVG
    
    
    // Children (using list for stable pointers - vector invalidates on realloc)
    std::list<LayoutBox> children;
    
    // Helper methods
    float contentX() const { return x + marginLeft + borderLeft + paddingLeft; }
    float contentY() const { return y + marginTop + borderTop + paddingTop; }
    float contentWidth() const { return width - paddingLeft - paddingRight - borderLeft - borderRight; }
    float contentHeight() const { return height - paddingTop - paddingBottom - borderTop - borderBottom; }
    
    float totalWidth() const { return width + marginLeft + marginRight; }
    float totalHeight() const { return height + marginTop + marginBottom; }
};

// =============================================================================
// LAYOUT FUNCTIONS
// =============================================================================

/**
 * @brief Initialize layout rendering callbacks
 * 
 * Must be called before using paintBox() to connect layout engine
 * to the browser's rendering system.
 */
void setLayoutCallbacks(
    void (*gfx_rect)(float x, float y, float w, float h, uint32_t color),
    void (*gfx_border)(float x, float y, float w, float h, uint32_t color, float thickness),
    void (*text_render)(const std::string& text, float x, float y, uint32_t color, float fontSize),
    float (*text_width)(const std::string& text, float fontSize),
    void (*register_link)(float x, float y, float w, float h, const std::string& href, const std::string& target),
    void (*gfx_texture)(float x, float y, float w, float h, uint32_t textureId),
    void (*gfx_line)(float x1, float y1, float x2, float y2, uint32_t color, float thickness) = nullptr
);

/// Set extended rendering callbacks (rounded rect, gradient)
void setLayoutCallbacks2(
    void (*gfx_rrect)(float x, float y, float w, float h, float radius, uint32_t color, uint8_t alpha),
    void (*gfx_gradient)(float x, float y, float w, float h, uint32_t c1, uint32_t c2),
    void (*gfx_svg)(float x, float y, float w, float h, const std::string& svgData, const std::string& svgKey) = nullptr
);

/**
 * @brief Layout a block formatting context
 * 
 * Implements the CSS Block Formatting Context algorithm:
 * - Block boxes stack vertically
 * - Inline boxes flow horizontally with line wrapping
 * - Width is calculated from containing block
 * - Height is calculated from children
 * 
 * @param box The box to layout (modified in place)
 * @param containingWidth Width of the containing block
 * @param startY Starting Y position
 */
void layoutBlock(LayoutBox& box, float containingWidth, float startY = 0);

/**
 * @brief Lay out a Table box (CSS 2.1 §17.5): column widths from the cells'
 * min-/max-content widths (automatic or fixed algorithm, colspan/rowspan),
 * then row heights and cell vertical alignment. Called by layoutBlock.
 */
void layoutTable(LayoutBox& table, float containingWidth, float startY);

/**
 * @brief Min-content and max-content border-box widths of a box (the
 * shrink-to-fit inputs for table cells), excluding its margins
 */
void intrinsicWidths(const LayoutBox& box, float& minWidth, float& maxWidth);

/// Drop cached intrinsic widths (an image's size changed after loading)
void invalidateIntrinsicWidths();

/// Block-level in a block formatting context (blocks, flex, tables; stray
/// table-internal boxes are treated as blocks)
bool isBlockLevel(LayoutType type);

/**
 * @brief Paint a layout box and its children
 * 
 * Recursively renders the layout box tree:
 * 1. Draw background
 * 2. Draw border
 * 3. Draw text content (if text node)
 * 4. Paint children
 * 5. Register link hit boxes
 * 
 * @param box The box to paint
 * @param offsetX X offset from viewport origin
 * @param offsetY Y offset from viewport origin
 * @param viewportHeight Height of visible area (for culling)
 * @param scrollY Current scroll position
 */
void paintBox(const LayoutBox& box, float offsetX, float offsetY, 
              float viewportHeight, float scrollY);

/**
 * @brief Calculate text width using current font
 * 
 * @param text Text to measure
 * @param fontSize Font size in pixels
 * @return Width in pixels
 */
float measureTextWidth(const std::string& text, float fontSize);

/**
 * @brief Extract all text from the layout tree
 * 
 * @param root Root box
 * @return Concatenated text
 */
std::string getAllText(const LayoutBox& root);

/**
 * @brief Extract text within a screen rectangle
 * 
 * @param root Root box
 * @param x Selection X
 * @param y Selection Y
 * @param w Selection Width
 * @param h Selection Height
 * @return Text intersecting the rectangle
 */
std::string getTextInRect(const LayoutBox& root, float x, float y, float w, float h);

} // namespace ZepraBrowser
