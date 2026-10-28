/**
 * @file css_style_declaration.hpp
 * @brief CSSStyleDeclaration interface
 *
 * @see https://developer.mozilla.org/en-US/docs/Web/API/CSSStyleDeclaration
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace Zepra::WebCore {

// Forward declarations
class CSSRule;

/**
 * @brief CSS property value with priority
 */
struct CSSPropertyValue {
    std::string value;
    bool important = false;
};

/**
 * @brief CSSStyleDeclaration - represents element.style or rule.style
 */
class CSSStyleDeclaration {
public:
    CSSStyleDeclaration() = default;
    explicit CSSStyleDeclaration(CSSRule* parentRule);

    /// Parent rule (null for inline styles)
    CSSRule* parentRule() const { return parentRule_; }

    /// Number of properties
    size_t length() const { return propertyOrder_.size(); }

    /// Get property name at index
    std::string item(size_t index) const;

    /// Get property value
    std::string getPropertyValue(const std::string& property) const;

    /// Get property priority ("important" or "")
    std::string getPropertyPriority(const std::string& property) const;

    /// Set property value
    void setProperty(const std::string& property, const std::string& value,
                     const std::string& priority = "");

    /// Remove property
    std::string removeProperty(const std::string& property);

    /// Whether any property is set with !important
    bool hasImportant() const;

    /// CSS text representation
    std::string cssText() const;
    void setCssText(const std::string& text);

    // =========================================================================
    // Named Property Accessors (common properties)
    // =========================================================================

    // Display & Box
    std::string display() const { return getPropertyValue("display"); }
    void setDisplay(const std::string& v) { setProperty("display", v); }

    std::string position() const { return getPropertyValue("position"); }
    void setPosition(const std::string& v) { setProperty("position", v); }

    std::string visibility() const { return getPropertyValue("visibility"); }
    void setVisibility(const std::string& v) { setProperty("visibility", v); }

    std::string overflow() const { return getPropertyValue("overflow"); }
    void setOverflow(const std::string& v) { setProperty("overflow", v); }

    std::string boxSizing() const { return getPropertyValue("box-sizing"); }
    void setBoxSizing(const std::string& v) { setProperty("box-sizing", v); }

    // Dimensions
    std::string width() const { return getPropertyValue("width"); }
    void setWidth(const std::string& v) { setProperty("width", v); }

    std::string height() const { return getPropertyValue("height"); }
    void setHeight(const std::string& v) { setProperty("height", v); }

    std::string minWidth() const { return getPropertyValue("min-width"); }
    void setMinWidth(const std::string& v) { setProperty("min-width", v); }

    std::string minHeight() const { return getPropertyValue("min-height"); }
    void setMinHeight(const std::string& v) { setProperty("min-height", v); }

    std::string maxWidth() const { return getPropertyValue("max-width"); }
    void setMaxWidth(const std::string& v) { setProperty("max-width", v); }

    std::string maxHeight() const { return getPropertyValue("max-height"); }
    void setMaxHeight(const std::string& v) { setProperty("max-height", v); }

    // Margin
    std::string margin() const { return getPropertyValue("margin"); }
    void setMargin(const std::string& v) { setProperty("margin", v); }

    std::string marginTop() const { return getPropertyValue("margin-top"); }
    void setMarginTop(const std::string& v) { setProperty("margin-top", v); }

    std::string marginRight() const { return getPropertyValue("margin-right"); }
    void setMarginRight(const std::string& v) { setProperty("margin-right", v); }

    std::string marginBottom() const { return getPropertyValue("margin-bottom"); }
    void setMarginBottom(const std::string& v) { setProperty("margin-bottom", v); }

    std::string marginLeft() const { return getPropertyValue("margin-left"); }
    void setMarginLeft(const std::string& v) { setProperty("margin-left", v); }

    // Padding
    std::string padding() const { return getPropertyValue("padding"); }
    void setPadding(const std::string& v) { setProperty("padding", v); }

    std::string paddingTop() const { return getPropertyValue("padding-top"); }
    void setPaddingTop(const std::string& v) { setProperty("padding-top", v); }

    std::string paddingRight() const { return getPropertyValue("padding-right"); }
    void setPaddingRight(const std::string& v) { setProperty("padding-right", v); }

    std::string paddingBottom() const { return getPropertyValue("padding-bottom"); }
    void setPaddingBottom(const std::string& v) { setProperty("padding-bottom", v); }

    std::string paddingLeft() const { return getPropertyValue("padding-left"); }
    void setPaddingLeft(const std::string& v) { setProperty("padding-left", v); }

    // Border
    std::string border() const { return getPropertyValue("border"); }
    void setBorder(const std::string& v) { setProperty("border", v); }

    std::string borderWidth() const { return getPropertyValue("border-width"); }
    void setBorderWidth(const std::string& v) { setProperty("border-width", v); }

    std::string borderStyle() const { return getPropertyValue("border-style"); }
    void setBorderStyle(const std::string& v) { setProperty("border-style", v); }

    std::string borderColor() const { return getPropertyValue("border-color"); }
    void setBorderColor(const std::string& v) { setProperty("border-color", v); }

    std::string borderRadius() const { return getPropertyValue("border-radius"); }
    void setBorderRadius(const std::string& v) { setProperty("border-radius", v); }

    // Background
    std::string background() const { return getPropertyValue("background"); }
    void setBackground(const std::string& v) { setProperty("background", v); }

    std::string backgroundColor() const { return getPropertyValue("background-color"); }
    void setBackgroundColor(const std::string& v) { setProperty("background-color", v); }

    std::string backgroundImage() const { return getPropertyValue("background-image"); }
    void setBackgroundImage(const std::string& v) { setProperty("background-image", v); }

    std::string backgroundPosition() const { return getPropertyValue("background-position"); }
    void setBackgroundPosition(const std::string& v) { setProperty("background-position", v); }

    std::string backgroundSize() const { return getPropertyValue("background-size"); }
    void setBackgroundSize(const std::string& v) { setProperty("background-size", v); }

    std::string backgroundRepeat() const { return getPropertyValue("background-repeat"); }
    void setBackgroundRepeat(const std::string& v) { setProperty("background-repeat", v); }

    // Typography
    std::string color() const { return getPropertyValue("color"); }
    void setColor(const std::string& v) { setProperty("color", v); }

    std::string font() const { return getPropertyValue("font"); }
    void setFont(const std::string& v) { setProperty("font", v); }

    std::string fontFamily() const { return getPropertyValue("font-family"); }
    void setFontFamily(const std::string& v) { setProperty("font-family", v); }

    std::string fontSize() const { return getPropertyValue("font-size"); }
    void setFontSize(const std::string& v) { setProperty("font-size", v); }

    std::string fontWeight() const { return getPropertyValue("font-weight"); }
    void setFontWeight(const std::string& v) { setProperty("font-weight", v); }

    std::string fontStyle() const { return getPropertyValue("font-style"); }
    void setFontStyle(const std::string& v) { setProperty("font-style", v); }

    std::string lineHeight() const { return getPropertyValue("line-height"); }
    void setLineHeight(const std::string& v) { setProperty("line-height", v); }

    std::string textAlign() const { return getPropertyValue("text-align"); }
    void setTextAlign(const std::string& v) { setProperty("text-align", v); }

    std::string textDecoration() const { return getPropertyValue("text-decoration"); }
    void setTextDecoration(const std::string& v) { setProperty("text-decoration", v); }

    std::string textTransform() const { return getPropertyValue("text-transform"); }
    void setTextTransform(const std::string& v) { setProperty("text-transform", v); }

    std::string whiteSpace() const { return getPropertyValue("white-space"); }
    void setWhiteSpace(const std::string& v) { setProperty("white-space", v); }

    // Flexbox
    std::string flexDirection() const { return getPropertyValue("flex-direction"); }
    void setFlexDirection(const std::string& v) { setProperty("flex-direction", v); }

    std::string flexWrap() const { return getPropertyValue("flex-wrap"); }
    void setFlexWrap(const std::string& v) { setProperty("flex-wrap", v); }

    std::string justifyContent() const { return getPropertyValue("justify-content"); }
    void setJustifyContent(const std::string& v) { setProperty("justify-content", v); }

    std::string alignItems() const { return getPropertyValue("align-items"); }
    void setAlignItems(const std::string& v) { setProperty("align-items", v); }

    std::string alignContent() const { return getPropertyValue("align-content"); }
    void setAlignContent(const std::string& v) { setProperty("align-content", v); }

    std::string flex() const { return getPropertyValue("flex"); }
    void setFlex(const std::string& v) { setProperty("flex", v); }

    std::string flexGrow() const { return getPropertyValue("flex-grow"); }
    void setFlexGrow(const std::string& v) { setProperty("flex-grow", v); }

    std::string flexShrink() const { return getPropertyValue("flex-shrink"); }
    void setFlexShrink(const std::string& v) { setProperty("flex-shrink", v); }

    std::string flexBasis() const { return getPropertyValue("flex-basis"); }
    void setFlexBasis(const std::string& v) { setProperty("flex-basis", v); }

    // Grid
    std::string gridTemplateColumns() const { return getPropertyValue("grid-template-columns"); }
    void setGridTemplateColumns(const std::string& v) { setProperty("grid-template-columns", v); }

    std::string gridTemplateRows() const { return getPropertyValue("grid-template-rows"); }
    void setGridTemplateRows(const std::string& v) { setProperty("grid-template-rows", v); }

    std::string gap() const { return getPropertyValue("gap"); }
    void setGap(const std::string& v) { setProperty("gap", v); }

    // Position
    std::string top() const { return getPropertyValue("top"); }
    void setTop(const std::string& v) { setProperty("top", v); }

    std::string right() const { return getPropertyValue("right"); }
    void setRight(const std::string& v) { setProperty("right", v); }

    std::string bottom() const { return getPropertyValue("bottom"); }
    void setBottom(const std::string& v) { setProperty("bottom", v); }

    std::string left() const { return getPropertyValue("left"); }
    void setLeft(const std::string& v) { setProperty("left", v); }

    std::string zIndex() const { return getPropertyValue("z-index"); }
    void setZIndex(const std::string& v) { setProperty("z-index", v); }

    // Transform & Animation
    std::string transform() const { return getPropertyValue("transform"); }
    void setTransform(const std::string& v) { setProperty("transform", v); }

    std::string transition() const { return getPropertyValue("transition"); }
    void setTransition(const std::string& v) { setProperty("transition", v); }

    std::string animation() const { return getPropertyValue("animation"); }
    void setAnimation(const std::string& v) { setProperty("animation", v); }

    std::string opacity() const { return getPropertyValue("opacity"); }
    void setOpacity(const std::string& v) { setProperty("opacity", v); }

    // Effects
    std::string boxShadow() const { return getPropertyValue("box-shadow"); }
    void setBoxShadow(const std::string& v) { setProperty("box-shadow", v); }

    std::string filter() const { return getPropertyValue("filter"); }
    void setFilter(const std::string& v) { setProperty("filter", v); }

    std::string cursor() const { return getPropertyValue("cursor"); }
    void setCursor(const std::string& v) { setProperty("cursor", v); }

    std::string pointerEvents() const { return getPropertyValue("pointer-events"); }
    void setPointerEvents(const std::string& v) { setProperty("pointer-events", v); }

private:
    CSSRule* parentRule_ = nullptr;
    std::unordered_map<std::string, CSSPropertyValue> properties_;
    std::vector<std::string> propertyOrder_;  // Maintain declaration order

    std::string normalizeProperty(const std::string& property) const;
};

} // namespace Zepra::WebCore
