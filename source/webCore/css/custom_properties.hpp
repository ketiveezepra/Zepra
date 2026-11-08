// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file custom_properties.hpp
 * @brief CSS custom properties (--name) and var() substitution
 *        (CSS Custom Properties for Cascading Variables Level 1).
 *
 * Custom properties cascade like any property and are inherited. Each
 * element's computed style holds the resolved map (shared with its parent
 * while it declares none of its own). var() in an ordinary property is
 * substituted from that map at computed-value time; a value whose var()
 * cannot be resolved (no such property and no fallback) is invalid at
 * computed-value time, and the property is left as inherited or initial.
 *
 * @see https://drafts.csswg.org/css-variables-1/
 */

#pragma once

#include <map>
#include <memory>
#include <string>

namespace Zepra::WebCore {

/// Custom property name ("--x", case-sensitive) to its resolved value
using CustomPropertyMap = std::map<std::string, std::string>;

/// A custom property registered with @property (CSS Properties and Values API)
struct RegisteredCustomProperty {
    std::string syntax = "*";
    bool inherits = true;
    bool hasInitialValue = false;
    std::string initialValue;
};
using RegisteredPropertyMap = std::map<std::string, RegisteredCustomProperty>;

/// True for a custom property name: "--" followed by at least one character
bool isCustomPropertyName(const std::string& name);

/// True when the value has a var() reference (case-insensitive)
bool containsVarFunction(const std::string& value);

/**
 * Replaces every var() in `value` with the referenced property's value or,
 * when the property is missing, with its fallback (itself substituted).
 * Returns false when some var() can't be resolved, or the value is malformed.
 */
bool substituteVarFunctions(const std::string& value, const CustomPropertyMap& vars, std::string& out);

/**
 * Builds an element's custom properties: starts from the inherited map and
 * applies the element's winning declarations (raw text, in any order) which
 * may refer to each other and to inherited properties. CSS-wide keywords
 * work (initial removes the property; inherit/unset keep the parent's).
 * Properties in a dependency cycle, or whose var() can't be resolved,
 * become guaranteed-invalid (removed).
 *
 * With `registered` properties: a property that doesn't inherit starts from
 * its initial value instead of the parent's, a registered property with no
 * value takes its initial value, and an invalid value behaves as unset.
 */
std::shared_ptr<const CustomPropertyMap> resolveCustomProperties(
    const std::shared_ptr<const CustomPropertyMap>& inherited,
    const CustomPropertyMap& declared,
    const RegisteredPropertyMap* registered = nullptr);

}  // namespace Zepra::WebCore
