// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file css_rule.cpp
 * @brief CSS Rule implementations stub
 */

#include "css/css_rule.hpp"
#include "css/css_style_sheet.hpp"
#include <cctype>
#include <cstdlib>

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
std::string CSSFontFaceRule::cssText() const {
    std::string text = "@font-face { ";
    for (size_t i = 0; i < style_->length(); i++) {
        std::string name = style_->item(i);
        text += name + ": " + style_->getPropertyValue(name) + "; ";
    }
    return text + "}";
}
void CSSFontFaceRule::setCssText(const std::string& text) {}

// CSSKeyframesRule
CSSKeyframesRule::CSSKeyframesRule() : cssRules_(std::make_unique<CSSRuleList>()) {}
CSSKeyframesRule::~CSSKeyframesRule() = default;
void CSSKeyframesRule::appendRule(const std::string& rule) {}
void CSSKeyframesRule::deleteRule(const std::string& select) {}
CSSKeyframeRule* CSSKeyframesRule::findRule(const std::string& select) const {
    // The last keyframe with exactly this selector (CSSOM: findRule)
    CSSKeyframeRule* found = nullptr;
    for (size_t i = 0; cssRules_ && i < cssRules_->length(); i++) {
        auto* frame = dynamic_cast<CSSKeyframeRule*>(cssRules_->item(i));
        if (frame && frame->keyText() == select) found = frame;
    }
    return found;
}
std::string CSSKeyframesRule::cssText() const { return "@keyframes " + name_ + " { }"; }
void CSSKeyframesRule::setCssText(const std::string& text) {}

// CSSKeyframeRule
CSSKeyframeRule::CSSKeyframeRule() : style_(std::make_unique<CSSStyleDeclaration>()) {}
CSSKeyframeRule::~CSSKeyframeRule() = default;
std::vector<float> CSSKeyframeRule::offsets() const {
    // <keyframe-selector># : from | to | <percentage [0,100]> (CSS Animations 1 §3)
    std::vector<float> out;
    size_t start = 0;
    for (size_t i = 0; i <= keyText_.size(); i++) {
        if (i < keyText_.size() && keyText_[i] != ',') continue;
        std::string part = keyText_.substr(start, i - start);
        start = i + 1;
        size_t a = part.find_first_not_of(" \t\r\n\f"), b = part.find_last_not_of(" \t\r\n\f");
        if (a == std::string::npos) continue;
        part = part.substr(a, b - a + 1);
        for (char& c : part) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (part == "from") { out.push_back(0.0f); continue; }
        if (part == "to") { out.push_back(1.0f); continue; }
        if (part.size() < 2 || part.back() != '%') continue;
        char* end = nullptr;
        std::string number = part.substr(0, part.size() - 1);
        float value = std::strtof(number.c_str(), &end);
        if (!end || *end != '\0' || !(value >= 0.0f && value <= 100.0f)) continue;
        out.push_back(value / 100.0f);
    }
    return out;
}

std::string CSSKeyframeRule::cssText() const { return keyText_ + " { " + (style_ ? style_->cssText() : "") + " }"; }
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
