/**
 * @file css_computed_style.hpp
 * @brief Computed style values and resolution
 *
 * @see https://developer.mozilla.org/en-US/docs/Web/CSS/computed_value
 */

#pragma once

#include <string>
#include <unordered_map>
#include <optional>
#include <cstdint>

namespace Zepra::WebCore {

/**
 * @brief CSS length value with unit
 */
struct CSSLength {
    enum class Unit {
        Auto,
        Px, Em, Rem, Ex, Ch,
        Vw, Vh, Vmin, Vmax,
        Percent,
        Fr,     // Grid fraction
        Cm, Mm, In, Pt, Pc,
    };

    float value = 0;
    Unit unit = Unit::Px;

    bool isAuto() const { return unit == Unit::Auto; }
    bool isPercent() const { return unit == Unit::Percent; }
    
    /// Resolve to pixels
    float toPx(float fontSize, float rootFontSize, float viewportWidth, 
               float viewportHeight, float containerSize = 0) const;

    static CSSLength parse(const std::string& str);
    static CSSLength px(float v) { return {v, Unit::Px}; }
    static CSSLength auto_() { return {0, Unit::Auto}; }
    static CSSLength percent(float v) { return {v, Unit::Percent}; }
};

/**
 * @brief RGBA Color
 */
struct CSSColor {
    uint8_t r = 0, g = 0, b = 0, a = 255;

    bool isTransparent() const { return a == 0; }
    uint32_t toRGBA() const { return (r << 24) | (g << 16) | (b << 8) | a; }
    uint32_t toARGB() const { return (a << 24) | (r << 16) | (g << 8) | b; }

    static CSSColor parse(const std::string& str);
    static CSSColor fromHex(const std::string& hex);
    static CSSColor fromRGB(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

    // Named colors
    static CSSColor black() { return {0, 0, 0, 255}; }
    static CSSColor white() { return {255, 255, 255, 255}; }
    static CSSColor transparent() { return {0, 0, 0, 0}; }
};

/**
 * @brief Display value
 */
// Save and undefine X11 macro conflict (X11/X.h defines None as 0L)
#ifdef None
#pragma push_macro("None")
#undef None
#define _CSS_NONE_WAS_DEFINED
#endif
enum class DisplayValue {
    None,
    Block,
    Inline,
    InlineBlock,
    Flex,
    InlineFlex,
    Grid,
    InlineGrid,
    Table,
    TableRow,
    TableCell,
    ListItem,
    Contents,
};
// Restore X11 None macro
#ifdef _CSS_NONE_WAS_DEFINED
#pragma pop_macro("None")
#undef _CSS_NONE_WAS_DEFINED
#endif

/**
 * @brief Position value
 */
enum class PositionValue {
    Static,
    Relative,
    Absolute,
    Fixed,
    Sticky,
};

/**
 * @brief Flex direction
 */
enum class FlexDirection {
    Row,
    RowReverse,
    Column,
    ColumnReverse,
};

/**
 * @brief Justify/Align values
 */
enum class JustifyAlign {
    Start,
    End,
    Center,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly,
    Stretch,
    FlexStart,
    FlexEnd,
    Baseline,
};

/**
 * @brief Text alignment
 */
enum class TextAlign {
    Left,
    Right,
    Center,
    Justify,
    Start,
    End,
};

/**
 * @brief Font weight
 */
enum class FontWeight {
    Normal = 400,
    Bold = 700,
    Lighter = 100,
    Bolder = 900,
    W100 = 100, W200 = 200, W300 = 300, W400 = 400,
    W500 = 500, W600 = 600, W700 = 700, W800 = 800, W900 = 900,
};

/**
 * @brief Font style
 */
enum class FontStyle {
    Normal,
    Italic,
    Oblique,
};

/**
 * @brief Overflow value
 */
enum class OverflowValue {
    Visible,
    Hidden,
    Scroll,
    Auto,
    Clip,
};

/**
 * @brief Box sizing
 */
enum class BoxSizing {
    ContentBox,
    BorderBox,
};

/**
 * @brief Visibility
 */
enum class Visibility {
    Visible,
    Hidden,
    Collapse,
};

/**
 * @brief Computed style - all resolved CSS properties
 */
class CSSComputedStyle {
public:
    CSSComputedStyle();
    ~CSSComputedStyle() = default;

    /// Create with inheritance from parent
    static CSSComputedStyle inherit(const CSSComputedStyle& parent);

    // =========================================================================
    // Box Model
    // =========================================================================

    DisplayValue display = DisplayValue::Block;  // Default to block (custom elements like <c-wiz>)
    PositionValue position = PositionValue::Static;
    Visibility visibility = Visibility::Visible;
    BoxSizing boxSizing = BoxSizing::ContentBox;
    float opacity = 1.0f;

    // Float
    int cssFloat = 0; // 0=none, 1=left, 2=right

    // Dimensions — default to auto per CSS spec (not 0px)
    CSSLength width = CSSLength::auto_();
    CSSLength height = CSSLength::auto_();
    CSSLength minWidth = CSSLength::auto_();
    CSSLength minHeight = CSSLength::auto_();
    CSSLength maxWidth = CSSLength::auto_();
    CSSLength maxHeight = CSSLength::auto_();

    // Margin
    CSSLength marginTop;
    CSSLength marginRight;
    CSSLength marginBottom;
    CSSLength marginLeft;

    // Padding
    CSSLength paddingTop;
    CSSLength paddingRight;
    CSSLength paddingBottom;
    CSSLength paddingLeft;

    // Border width
    float borderTopWidth = 0;
    float borderRightWidth = 0;
    float borderBottomWidth = 0;
    float borderLeftWidth = 0;

    // Border color
    CSSColor borderTopColor;
    CSSColor borderRightColor;
    CSSColor borderBottomColor;
    CSSColor borderLeftColor;

    // Border radius
    float borderTopLeftRadius = 0;
    float borderTopRightRadius = 0;
    float borderBottomRightRadius = 0;
    float borderBottomLeftRadius = 0;

    // Overflow
    OverflowValue overflowX = OverflowValue::Visible;
    OverflowValue overflowY = OverflowValue::Visible;

    // =========================================================================
    // Positioning
    // =========================================================================

    CSSLength top = CSSLength::auto_();
    CSSLength right = CSSLength::auto_();
    CSSLength bottom = CSSLength::auto_();
    CSSLength left = CSSLength::auto_();
    int zIndex = 0;
    bool zIndexAuto = true;

    // =========================================================================
    // Flexbox
    // =========================================================================

    FlexDirection flexDirection = FlexDirection::Row;
    bool flexWrap = false;
    bool wrapReverse = false;
    JustifyAlign justifyContent = JustifyAlign::FlexStart;
    JustifyAlign alignItems = JustifyAlign::Stretch;
    JustifyAlign alignContent = JustifyAlign::Stretch;
    JustifyAlign alignSelf = JustifyAlign::Start;
    float flexGrow = 0;
    float flexShrink = 1;
    CSSLength flexBasis = CSSLength::auto_();
    int order = 0;

    // =========================================================================
    // Grid
    // =========================================================================

    std::string gridTemplateColumns;
    std::string gridTemplateRows;
    CSSLength gap;
    CSSLength rowGap;
    CSSLength columnGap;

    // =========================================================================
    // Typography
    // =========================================================================

    CSSColor color;
    std::string fontFamily = "sans-serif";
    float fontSize = 16.0f;
    FontWeight fontWeight = FontWeight::Normal;
    FontStyle fontStyle = FontStyle::Normal;
    float lineHeight = 1.2f;
    TextAlign textAlign = TextAlign::Start;
    std::string textDecoration;
    std::string textTransform;
    float letterSpacing = 0;
    float wordSpacing = 0;
    std::string whiteSpace = "normal";

    // =========================================================================
    // Background
    // =========================================================================

    CSSColor backgroundColor = CSSColor::transparent();
    std::string backgroundImage;
    std::string backgroundPosition;
    std::string backgroundSize;
    std::string backgroundRepeat = "repeat";

    // =========================================================================
    // Effects
    // =========================================================================

    std::string boxShadow;
    std::string transform;
    std::string transition;
    std::string animation;
    std::string filter;
    std::string cursor = "auto";
    std::string pointerEvents = "auto";
    
    // Tailwind/modern CSS properties
    std::string textOverflow;       // ellipsis, clip
    std::string objectFit;          // cover, contain, fill, etc.
    std::string objectPosition;     // center, top left, etc.
    std::string aspectRatio;        // auto, 16/9, 1, etc.
    std::string backdropFilter;     // blur(), brightness(), etc.
    std::string placeItems;         // center, stretch, etc.
    std::string placeContent;       // center, space-between, etc.
    std::string isolation;          // auto, isolate
    std::string willChange;         // auto, transform, opacity
    std::string content;            // for ::before/::after
    std::string userSelect;         // none, auto, text
    std::string appearance;         // none, auto
    std::string outlineStyle;       // none, solid, dashed
    float outlineWidth = 0;
    CSSColor outlineColor;
    float outlineOffset = 0;

    // =========================================================================
    // Methods
    // =========================================================================

    /// Get property value as string
    std::string getPropertyValue(const std::string& property) const;

    /// Check if property inherits
    static bool inherits(const std::string& property);

    /// Get initial value for property
    static std::string initialValue(const std::string& property);
};

} // namespace Zepra::WebCore
