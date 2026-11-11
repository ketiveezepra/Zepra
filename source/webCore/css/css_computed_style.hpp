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
#include <memory>
#include "css/custom_properties.hpp"

namespace Zepra::WebCore {

/// What font- and viewport-relative terms of a calc() resolve against at
/// computed-value time (CSS Values 4 §10.9: mixed units are combined there)
struct CSSLengthContext {
    float fontSize = 16.0f;        ///< the element's font size (em, ex, ch)
    float rootFontSize = 16.0f;    ///< rem
    float viewportWidth = 1920.0f; ///< vw, vmin, vmax
    float viewportHeight = 1080.0f;
};

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
    /// A percentage from calc() can carry a px part ("calc(100% - 20px)" is
    /// value 100, pxOffset -20), added once the percentage is resolved.
    /// 0 for every other length.
    float pxOffset = 0;

    bool isAuto() const { return unit == Unit::Auto; }
    bool isPercent() const { return unit == Unit::Percent; }

    /// Resolve to pixels
    float toPx(float fontSize, float rootFontSize, float viewportWidth,
               float viewportHeight, float containerSize = 0) const;

    /// A length, percentage or math function (calc(), min(), max(),
    /// clamp()); auto for anything else, including the intrinsic sizing
    /// keywords (max-content, ...) and a math function that does not resolve
    static CSSLength parse(const std::string& str, const CSSLengthContext& ctx = {});
    static CSSLength px(float v) { return {v, Unit::Px}; }
    static CSSLength auto_() { return {0, Unit::Auto}; }
    static CSSLength percent(float v) { return {v, Unit::Percent}; }
};

/// The value of a math function: a number ("calc(2 / 1.5)") or a length /
/// percentage ("calc(.25rem * 6)", "clamp(4rem, 10vw, 9.5rem)")
struct CSSMathValue {
    bool isNumber = false;
    float number = 0;
    CSSLength length;
};

/// True when the value is a math function call: calc(), min(), max(),
/// clamp() or -webkit-calc() (case-insensitive)
bool isCSSMathFunction(const std::string& value);

/**
 * Evaluates a math function (CSS Values 4 §10): + - * / with parentheses and
 * nested functions; lengths of one unit stay in it ("calc(.25rem * 6)" is
 * 1.5rem), mixed units are combined in px through `ctx`, and a percentage
 * mixed with lengths keeps the percentage with the lengths in pxOffset.
 * False for a malformed or untyped expression (a number added to a length,
 * min() of a percentage and a length, division by zero, an unknown function).
 */
bool evaluateCSSMath(const std::string& value, const CSSLengthContext& ctx, CSSMathValue& out);

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
    TableRowGroup,
    TableHeaderGroup,
    TableFooterGroup,
    TableColumn,
    TableColumnGroup,
    TableCaption,
    InlineTable,
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
    Auto,       // align-self: auto (the container's align-items)
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
 * @brief border-style (CSS Backgrounds 3 §3.2)
 */
enum class BorderStyle : uint8_t {
    None,
    Hidden,
    Solid,
    Dashed,
    Dotted,
    Double,
    Groove,
    Ridge,
    Inset,
    Outset,
};

/// A border-style keyword; false for anything else
bool parseBorderStyle(const std::string& keyword, BorderStyle& out);

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

    // The initial value (CSS Display 3): elements the UA sheet does not list,
    // custom and unknown ones included, are inline as in every browser
    DisplayValue display = DisplayValue::Inline;
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

    // Border style (a side whose style is none or hidden has no width)
    BorderStyle borderTopStyle = BorderStyle::None;
    BorderStyle borderRightStyle = BorderStyle::None;
    BorderStyle borderBottomStyle = BorderStyle::None;
    BorderStyle borderLeftStyle = BorderStyle::None;

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
    // A percentage top-left radius ("50%"), resolved against the box at
    // paint time (0: the px radius above applies)
    float borderTopLeftRadiusPercent = 0;

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
    JustifyAlign alignSelf = JustifyAlign::Auto;
    float flexGrow = 0;
    float flexShrink = 1;
    CSSLength flexBasis = CSSLength::auto_();
    int order = 0;

    // =========================================================================
    // Grid
    // =========================================================================

    std::string gridTemplateColumns;
    std::string gridTemplateRows;
    std::string gridTemplateAreas;
    std::string gridAutoColumns;
    std::string gridAutoRows;
    bool gridAutoFlowColumn = false;  // grid-auto-flow: column
    bool gridAutoFlowDense = false;
    // grid-row/column-start/end as CSS text ("auto", "2", "-1", "span 2", "main")
    std::string gridColumnStart, gridColumnEnd, gridRowStart, gridRowEnd;
    JustifyAlign justifyItems = JustifyAlign::Stretch;  // normal
    JustifyAlign justifySelf = JustifyAlign::Auto;
    bool justifyContentNormal = true;  // justify-content: normal (grid tracks stretch)
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
    float lineHeight = 1.2f;         // as a multiple of font-size
    bool lineHeightNormal = true;    // line-height: normal (the font's own spacing)
    float lineHeightPx = 0;          // a length/percentage line-height in px (inherited
                                     // as is); 0 for normal and plain numbers
    /// The used line-height in px, or 0 for normal
    float usedLineHeight() const {
        if (lineHeightNormal) return 0;
        return lineHeightPx > 0 ? lineHeightPx : lineHeight * fontSize;
    }
    TextAlign textAlign = TextAlign::Start;
    // HTML "align descendants" (<center>, align attributes, -webkit-center):
    // block children are aligned too. 0=none, 1=left, 2=center, 3=right
    int legacyAlign = 0;
    std::string textDecoration;
    std::string textTransform;
    // -webkit-text-fill-color (inherited); without one, text fills with color
    bool hasTextFillColor = false;
    CSSColor textFillColor;
    // -webkit-text-stroke (inherited): width in px (0 = none) and colour
    // (currentcolor when hasTextStrokeColor is false)
    float textStrokeWidth = 0;
    bool hasTextStrokeColor = false;
    CSSColor textStrokeColor;
    float letterSpacing = 0;
    float wordSpacing = 0;
    std::string whiteSpace = "normal";
    std::string verticalAlign = "baseline";

    // =========================================================================
    // Tables
    // =========================================================================

    float borderSpacingH = 0;   // inherited
    float borderSpacingV = 0;   // inherited
    bool borderCollapse = false; // inherited
    bool tableLayoutFixed = false;

    // =========================================================================
    // Background
    // =========================================================================

    CSSColor backgroundColor = CSSColor::transparent();
    std::string backgroundImage;
    std::string backgroundPosition;
    std::string backgroundSize;
    std::string backgroundRepeat = "repeat";
    bool backgroundClipText = false;   // background-clip: text (only under the glyphs)

    // =========================================================================
    // Effects
    // =========================================================================

    std::string boxShadow;
    std::string transform;
    std::string transformOrigin;    // "" = 50% 50%
    // The individual transform properties (CSS Transforms 2 §5), as
    // declared; "" = none. They apply before `transform`.
    std::string translate;
    std::string rotate;
    std::string scale;
    std::string transition;

    /// Custom properties (--name), resolved and inherited; shared with the
    /// parent while the element declares none. Null when there are none.
    std::shared_ptr<const CustomPropertyMap> customProperties;
    std::string animation;
    // CSS Animations 1 longhands as declared (comma-separated lists, one
    // entry per animation; empty: the initial value). `animation` sets all.
    std::string animationName;
    std::string animationDuration;
    std::string animationDelay;
    std::string animationIterationCount;
    std::string animationDirection;
    std::string animationFillMode;
    std::string animationTimingFunction;
    std::string animationPlayState;
    // CSS Transitions 1 longhands as declared (comma-separated lists;
    // empty: the initial value). `transition` sets all four.
    std::string transitionProperty;
    std::string transitionDuration;
    std::string transitionDelay;
    std::string transitionTimingFunction;
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
    std::string mixBlendMode;       // normal, multiply, ... ("" = normal)
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
