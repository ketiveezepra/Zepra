// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file css_style_declaration.cpp
 * @brief CSS Style Declaration implementation stub
 */

#include "css/css_style_declaration.hpp"
#include <sstream>
#include <algorithm>

namespace Zepra::WebCore {

CSSStyleDeclaration::CSSStyleDeclaration(CSSRule* parentRule) 
    : parentRule_(parentRule) {}

std::string CSSStyleDeclaration::cssText() const {
    std::ostringstream oss;
    for (const auto& prop : propertyOrder_) {
        auto it = properties_.find(prop);
        if (it != properties_.end()) {
            oss << prop << ": " << it->second.value;
            if (it->second.important) oss << " !important";
            oss << "; ";
        }
    }
    return oss.str();
}

void CSSStyleDeclaration::setCssText(const std::string& text) {
    properties_.clear();
    propertyOrder_.clear();
    // Would parse CSS text - stub
}

std::string CSSStyleDeclaration::item(size_t index) const {
    if (index >= propertyOrder_.size()) return "";
    return propertyOrder_[index];
}

std::string CSSStyleDeclaration::getPropertyValue(const std::string& property) const {
    std::string normalized = normalizeProperty(property);
    auto it = properties_.find(normalized);
    return it != properties_.end() ? it->second.value : "";
}

std::string CSSStyleDeclaration::getPropertyPriority(const std::string& property) const {
    std::string normalized = normalizeProperty(property);
    auto it = properties_.find(normalized);
    if (it != properties_.end() && it->second.important) {
        return "important";
    }
    return "";
}

void CSSStyleDeclaration::setProperty(const std::string& property, const std::string& value,
                                       const std::string& priority) {
    std::string normalized = normalizeProperty(property);
    
    if (properties_.find(normalized) == properties_.end()) {
        propertyOrder_.push_back(normalized);
    }
    
    CSSPropertyValue propVal;
    propVal.value = value;
    propVal.important = (priority == "important");
    properties_[normalized] = propVal;
}

bool CSSStyleDeclaration::hasImportant() const {
    for (const auto& entry : properties_) {
        if (entry.second.important) return true;
    }
    return false;
}

std::string CSSStyleDeclaration::removeProperty(const std::string& property) {
    std::string normalized = normalizeProperty(property);
    auto it = properties_.find(normalized);
    if (it == properties_.end()) return "";
    
    std::string value = it->second.value;
    properties_.erase(it);
    propertyOrder_.erase(
        std::remove(propertyOrder_.begin(), propertyOrder_.end(), normalized),
        propertyOrder_.end()
    );
    return value;
}

std::string CSSStyleDeclaration::normalizeProperty(const std::string& property) const {
    // Convert camelCase to kebab-case and lowercase
    std::string result;
    for (char c : property) {
        if (std::isupper(c)) {
            if (!result.empty()) result += '-';
            result += std::tolower(c);
        } else {
            result += c;
        }
    }
    return result;
}

} // namespace Zepra::WebCore
