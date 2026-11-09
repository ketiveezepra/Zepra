/**
 * @file css_style_sheet.hpp
 * @brief CSSOM StyleSheet interfaces
 *
 * @see https://developer.mozilla.org/en-US/docs/Web/API/CSSStyleSheet
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <utility>
#include "css/custom_properties.hpp"

namespace Zepra::WebCore {

// Forward declarations
class CSSRule;
class CSSStyleSheet;
class DOMElement;
class MediaList;

/**
 * @brief Base StyleSheet interface
 */
class StyleSheet {
public:
    virtual ~StyleSheet() = default;

    /// Type of stylesheet ("text/css")
    virtual std::string type() const = 0;

    /// URL of the stylesheet
    std::string href() const { return href_; }
    void setHref(const std::string& href) { href_ = href; }

    /// Owner node (link or style element)
    DOMElement* ownerNode() const { return ownerNode_; }
    void setOwnerNode(DOMElement* node) { ownerNode_ = node; }

    /// Parent stylesheet (@import)
    StyleSheet* parentStyleSheet() const { return parentStyleSheet_; }
    void setParentStyleSheet(StyleSheet* parent) { parentStyleSheet_ = parent; }

    /// Title
    std::string title() const { return title_; }
    void setTitle(const std::string& title) { title_ = title; }

    /// Media queries
    MediaList* media() const { return media_.get(); }

    /// Disabled state
    bool disabled() const { return disabled_; }
    void setDisabled(bool disabled) { disabled_ = disabled; }

protected:
    std::string href_;
    DOMElement* ownerNode_ = nullptr;
    StyleSheet* parentStyleSheet_ = nullptr;
    std::string title_;
    std::unique_ptr<MediaList> media_;
    bool disabled_ = false;
};

/**
 * @brief Collection of stylesheets
 */
class StyleSheetList {
public:
    size_t length() const { return sheets_.size(); }
    StyleSheet* item(size_t index) const;
    StyleSheet* operator[](size_t index) const { return item(index); }

    void add(std::shared_ptr<StyleSheet> sheet);
    void remove(StyleSheet* sheet);

private:
    std::vector<std::shared_ptr<StyleSheet>> sheets_;
};

/**
 * @brief Collection of CSS rules
 */
class CSSRuleList {
public:
    size_t length() const { return rules_.size(); }
    CSSRule* item(size_t index) const;
    CSSRule* operator[](size_t index) const { return item(index); }

    void add(std::unique_ptr<CSSRule> rule);
    std::unique_ptr<CSSRule> remove(size_t index);
    void insertAt(size_t index, std::unique_ptr<CSSRule> rule);

private:
    std::vector<std::unique_ptr<CSSRule>> rules_;
};

/**
 * @brief CSS stylesheet
 */
class CSSStyleSheet : public StyleSheet {
public:
    CSSStyleSheet();
    ~CSSStyleSheet() override;

    std::string type() const override { return "text/css"; }

    /// Owner CSS rule (@import)
    CSSRule* ownerRule() const { return ownerRule_; }
    void setOwnerRule(CSSRule* rule) { ownerRule_ = rule; }

    /// All CSS rules
    CSSRuleList* cssRules() const { return cssRules_.get(); }

    /// Insert rule at index
    size_t insertRule(const std::string& rule, size_t index = 0);

    /// Delete rule at index
    void deleteRule(size_t index);

    /// Replace all rules
    void replaceSync(const std::string& text);

    /// Parse and set content
    void parseContent(const std::string& cssText);

    /// The sheet's valid @property rules, in source order
    const std::vector<std::pair<std::string, RegisteredCustomProperty>>& propertyRegistrations() const {
        return propertyRegistrations_;
    }
    void addPropertyRegistration(std::string name, RegisteredCustomProperty reg) {
        propertyRegistrations_.emplace_back(std::move(name), std::move(reg));
    }

    /// The cascade layers the sheet names (@layer blocks and statements),
    /// as dotted paths in the order they first appear
    const std::vector<std::string>& layerNames() const { return layerNames_; }
    void declareLayer(const std::string& name) {
        for (const auto& known : layerNames_) if (known == name) return;
        layerNames_.push_back(name);
    }

private:
    CSSRule* ownerRule_ = nullptr;
    std::vector<std::string> layerNames_;
    std::unique_ptr<CSSRuleList> cssRules_;
    std::vector<std::pair<std::string, RegisteredCustomProperty>> propertyRegistrations_;
};

/**
 * @brief Media query list
 */
class MediaList {
public:
    MediaList() = default;

    /// Media text
    std::string mediaText() const { return mediaText_; }
    void setMediaText(const std::string& text);

    /// Number of media queries
    size_t length() const { return queries_.size(); }

    /// Get query at index
    std::string item(size_t index) const;

    /// Add media query
    void appendMedium(const std::string& medium);

    /// Remove media query
    void deleteMedium(const std::string& medium);

    /// Check if matches current environment
    bool matches(int viewportWidth, int viewportHeight) const;

private:
    std::string mediaText_;
    std::vector<std::string> queries_;
};

} // namespace Zepra::WebCore
