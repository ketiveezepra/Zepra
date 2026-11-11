/**
 * @file css_rule.hpp
 * @brief CSS Rule hierarchy
 *
 * @see https://developer.mozilla.org/en-US/docs/Web/API/CSSRule
 */

#pragma once

#include "css/css_style_declaration.hpp"
#include "css/media_query.hpp"
#include "css/font_face_descriptors.hpp"
#include <string>
#include <vector>
#include <memory>

namespace Zepra::WebCore {

// Forward declarations
class CSSStyleSheet;

/**
 * @brief CSS Rule types
 */
enum class CSSRuleType {
    Unknown = 0,
    Style = 1,          // CSSStyleRule
    Import = 3,         // CSSImportRule
    Media = 4,          // CSSMediaRule
    FontFace = 5,       // CSSFontFaceRule
    Page = 6,           // CSSPageRule
    Keyframes = 7,      // CSSKeyframesRule
    Keyframe = 8,       // CSSKeyframeRule
    Namespace = 10,     // CSSNamespaceRule
    Supports = 12,      // CSSSupportsRule
    FontFeatureValues = 14,
    LayerBlock = 16,
    LayerStatement = 17,
    Container = 18,
};

/**
 * @brief Base CSSRule interface
 */
class CSSRule {
public:
    virtual ~CSSRule() = default;

    /// Rule type
    virtual CSSRuleType type() const = 0;

    /// CSS text representation
    virtual std::string cssText() const = 0;
    virtual void setCssText(const std::string& text) = 0;

    /// Parent stylesheet
    CSSStyleSheet* parentStyleSheet() const { return parentStyleSheet_; }
    void setParentStyleSheet(CSSStyleSheet* sheet) { parentStyleSheet_ = sheet; }

    /// Parent rule (for nested rules)
    CSSRule* parentRule() const { return parentRule_; }
    void setParentRule(CSSRule* rule) { parentRule_ = rule; }

protected:
    CSSStyleSheet* parentStyleSheet_ = nullptr;
    CSSRule* parentRule_ = nullptr;
};

/**
 * @brief CSSStyleRule - selector { declarations }
 */
class CSSStyleRule : public CSSRule {
public:
    CSSStyleRule();
    ~CSSStyleRule() override;

    CSSRuleType type() const override { return CSSRuleType::Style; }

    /// Selector text
    std::string selectorText() const { return selectorText_; }
    void setSelectorText(const std::string& selector);

    /// Style declaration
    CSSStyleDeclaration* style() { return style_.get(); }
    const CSSStyleDeclaration* style() const { return style_.get(); }

    std::string cssText() const override;
    void setCssText(const std::string& text) override;

    /// The `@media` query lists this rule sits inside, outermost first. The
    /// cascade uses the rule only while all of them match (empty: always).
    const std::vector<std::shared_ptr<const MediaQueryList>>& media() const { return media_; }
    void setMedia(std::vector<std::shared_ptr<const MediaQueryList>> media) { media_ = std::move(media); }
    bool mediaMatches() const { return allMediaMatch(media_); }

    /// The cascade layer the rule sits in, as a dotted path ("base",
    /// "framework.utilities"); empty when it is not in a layer
    const std::string& layer() const { return layer_; }
    void setLayer(std::string layer) { layer_ = std::move(layer); }

private:
    std::string selectorText_;
    std::string layer_;
    std::unique_ptr<CSSStyleDeclaration> style_;
    std::vector<std::shared_ptr<const MediaQueryList>> media_;
};

/**
 * @brief CSSMediaRule - @media query { rules }
 */
class CSSMediaRule : public CSSRule {
public:
    CSSMediaRule();
    ~CSSMediaRule() override;

    CSSRuleType type() const override { return CSSRuleType::Media; }

    /// Media query list
    class MediaList* media() const { return media_.get(); }

    /// Condition text
    std::string conditionText() const { return conditionText_; }
    void setConditionText(const std::string& condition);

    /// Nested rules
    class CSSRuleList* cssRules() const { return cssRules_.get(); }

    /// Insert rule
    size_t insertRule(const std::string& rule, size_t index = 0);

    /// Delete rule
    void deleteRule(size_t index);

    std::string cssText() const override;
    void setCssText(const std::string& text) override;

private:
    std::string conditionText_;
    std::unique_ptr<class MediaList> media_;
    std::unique_ptr<class CSSRuleList> cssRules_;
};

/**
 * @brief CSSFontFaceRule - @font-face { descriptors }
 */
class CSSFontFaceRule : public CSSRule {
public:
    CSSFontFaceRule();
    ~CSSFontFaceRule() override;

    CSSRuleType type() const override { return CSSRuleType::FontFace; }

    /// Font family
    std::string fontFamily() const { return style_->getPropertyValue("font-family"); }

    /// Font source
    std::string src() const { return style_->getPropertyValue("src"); }

    /// Font descriptors
    CSSStyleDeclaration* style() { return style_.get(); }
    const CSSStyleDeclaration* style() const { return style_.get(); }

    /// The descriptors parsed for loading; valid() is false when the rule
    /// lacks a usable font-family or src, and then no font is loaded for it
    const FontFaceDescriptors& descriptors() const { return descriptors_; }
    bool valid() const { return valid_; }
    /// Re-read descriptors() from style()
    void updateDescriptors() { valid_ = parseFontFaceDescriptors(*style_, descriptors_); }

    /// The @media blocks (and @import media) around the rule
    const std::vector<std::shared_ptr<const MediaQueryList>>& media() const { return media_; }
    void setMedia(std::vector<std::shared_ptr<const MediaQueryList>> media) { media_ = std::move(media); }
    bool mediaMatches() const { return allMediaMatch(media_); }

    std::string cssText() const override;
    void setCssText(const std::string& text) override;

private:
    std::unique_ptr<CSSStyleDeclaration> style_;
    FontFaceDescriptors descriptors_;
    bool valid_ = false;
    std::vector<std::shared_ptr<const MediaQueryList>> media_;
};

/**
 * @brief CSSKeyframesRule - @keyframes name { keyframes }
 */
class CSSKeyframesRule : public CSSRule {
public:
    CSSKeyframesRule();
    ~CSSKeyframesRule() override;

    CSSRuleType type() const override { return CSSRuleType::Keyframes; }

    /// Animation name
    std::string name() const { return name_; }
    void setName(const std::string& name) { name_ = name; }

    /// Keyframe rules
    class CSSRuleList* cssRules() const { return cssRules_.get(); }

    /// Find keyframe by key (e.g., "from", "50%", "to")
    class CSSKeyframeRule* findRule(const std::string& key) const;

    /// Append keyframe
    void appendRule(const std::string& rule);

    /// Delete keyframe
    void deleteRule(const std::string& key);

    std::string cssText() const override;
    void setCssText(const std::string& text) override;

private:
    std::string name_;
    std::unique_ptr<class CSSRuleList> cssRules_;
};

/**
 * @brief CSSKeyframeRule - single keyframe (e.g., 50% { ... })
 */
class CSSKeyframeRule : public CSSRule {
public:
    CSSKeyframeRule();
    ~CSSKeyframeRule() override;

    CSSRuleType type() const override { return CSSRuleType::Keyframe; }

    /// Keyframe selector (e.g., "0%", "from", "50%, 75%")
    std::string keyText() const { return keyText_; }
    void setKeyText(const std::string& key) { keyText_ = key; }

    /// Style
    CSSStyleDeclaration* style() { return style_.get(); }
    const CSSStyleDeclaration* style() const { return style_.get(); }
    void setStyle(std::unique_ptr<CSSStyleDeclaration> style) {
        if (style) style_ = std::move(style);
    }

    /// The keyframe's offsets in [0, 1] ("from" 0, "to" 1, "40%" 0.4); an
    /// invalid selector gives none
    std::vector<float> offsets() const;

    std::string cssText() const override;
    void setCssText(const std::string& text) override;

private:
    std::string keyText_;
    std::unique_ptr<CSSStyleDeclaration> style_;
};

/**
 * @brief CSSImportRule - @import url(...)
 */
class CSSImportRule : public CSSRule {
public:
    CSSImportRule();
    ~CSSImportRule() override;

    CSSRuleType type() const override { return CSSRuleType::Import; }

    /// URL
    std::string href() const { return href_; }

    /// Media query
    class MediaList* media() const { return media_.get(); }

    /// Imported stylesheet
    CSSStyleSheet* styleSheet() const { return styleSheet_; }

    /// The URL as written (url("x"), url(x) or "x"), not yet resolved
    void setHref(std::string href) { href_ = std::move(href); }
    /// The media query list after the URL ("" = all media)
    const std::string& mediaText() const { return mediaText_; }
    void setMediaText(std::string text) { mediaText_ = std::move(text); }
    /// The condition of supports(...), "" when there is none
    const std::string& supportsCondition() const { return supports_; }
    void setSupportsCondition(std::string c) { supports_ = std::move(c); }

    std::string cssText() const override;
    void setCssText(const std::string& text) override;

private:
    std::string href_;
    std::string mediaText_;
    std::string supports_;
    std::unique_ptr<class MediaList> media_;
    CSSStyleSheet* styleSheet_ = nullptr;
};

/**
 * @brief CSSSupportsRule - @supports (condition) { rules }
 */
class CSSSupportsRule : public CSSRule {
public:
    CSSSupportsRule();
    ~CSSSupportsRule() override;

    CSSRuleType type() const override { return CSSRuleType::Supports; }

    /// Condition text
    std::string conditionText() const { return conditionText_; }

    /// Nested rules
    class CSSRuleList* cssRules() const { return cssRules_.get(); }

    std::string cssText() const override;
    void setCssText(const std::string& text) override;

private:
    std::string conditionText_;
    std::unique_ptr<class CSSRuleList> cssRules_;
};

} // namespace Zepra::WebCore
