// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file css_rule.cpp
 * @brief CSS Rule implementations stub
 */

#include "css/css_rule.hpp"
#include "css/css_style_sheet.hpp"

namespace Zepra::WebCore {

// CSSStyleRule
CSSStyleRule::CSSStyleRule() : style_(std::make_unique<CSSStyleDeclaration>()) {}
CSSStyleRule::~CSSStyleRule() = default;

void CSSStyleRule::setSelectorText(const std::string& text) { selectorText_ = text; }

std::string CSSStyleRule::cssText() const { 
    return selectorText_ + " { " + (style_ ? style_->cssText() : "") + " }"; 
}
void CSSStyleRule::setCssText(const std::string& text) { /* stub */ }

// CSSMediaRule
CSSMediaRule::CSSMediaRule() 
    : media_(std::make_unique<MediaList>()),
      cssRules_(std::make_unique<CSSRuleList>()) {}
CSSMediaRule::~CSSMediaRule() = default;

void CSSMediaRule::setConditionText(const std::string& condition) { conditionText_ = condition; }
size_t CSSMediaRule::insertRule(const std::string& rule, size_t index) { return index; }
void CSSMediaRule::deleteRule(size_t index) {}
std::string CSSMediaRule::cssText() const { return "@media " + conditionText_ + " { }"; }
void CSSMediaRule::setCssText(const std::string& text) {}

// CSSFontFaceRule
CSSFontFaceRule::CSSFontFaceRule() : style_(std::make_unique<CSSStyleDeclaration>()) {}
CSSFontFaceRule::~CSSFontFaceRule() = default;
std::string CSSFontFaceRule::cssText() const { return "@font-face { }"; }
void CSSFontFaceRule::setCssText(const std::string& text) {}

// CSSKeyframesRule
CSSKeyframesRule::CSSKeyframesRule() : cssRules_(std::make_unique<CSSRuleList>()) {}
CSSKeyframesRule::~CSSKeyframesRule() = default;
void CSSKeyframesRule::appendRule(const std::string& rule) {}
void CSSKeyframesRule::deleteRule(const std::string& select) {}
CSSKeyframeRule* CSSKeyframesRule::findRule(const std::string& select) const { return nullptr; }
std::string CSSKeyframesRule::cssText() const { return "@keyframes " + name_ + " { }"; }
void CSSKeyframesRule::setCssText(const std::string& text) {}

// CSSKeyframeRule
CSSKeyframeRule::CSSKeyframeRule() : style_(std::make_unique<CSSStyleDeclaration>()) {}
CSSKeyframeRule::~CSSKeyframeRule() = default;
std::string CSSKeyframeRule::cssText() const { return keyText_ + " { }"; }
void CSSKeyframeRule::setCssText(const std::string& text) {}

// CSSImportRule
CSSImportRule::CSSImportRule() : media_(std::make_unique<MediaList>()) {}
CSSImportRule::~CSSImportRule() = default;
std::string CSSImportRule::cssText() const {
    std::string text = "@import url(\"" + href_ + "\")";
    if (!supports_.empty()) text += " supports(" + supports_ + ")";
    if (!mediaText_.empty()) text += " " + mediaText_;
    return text + ";";
}
void CSSImportRule::setCssText(const std::string& text) {}

// CSSSupportsRule
CSSSupportsRule::CSSSupportsRule() : cssRules_(std::make_unique<CSSRuleList>()) {}
CSSSupportsRule::~CSSSupportsRule() = default;
std::string CSSSupportsRule::cssText() const { return "@supports (" + conditionText_ + ") { }"; }
void CSSSupportsRule::setCssText(const std::string& text) {}

} // namespace Zepra::WebCore
