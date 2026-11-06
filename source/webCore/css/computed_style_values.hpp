// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file computed_style_values.hpp
 * @brief A computed style as property/value strings: what the DevTools
 *        Computed pane lists and what getComputedStyle() answers page scripts.
 */

#pragma once

#include "css/css_computed_style.hpp"

#include <string>
#include <utility>
#include <vector>

namespace Zepra::WebCore {

/// A number as computed values print it: whole numbers without decimals,
/// others with one ("12", "1.5")
std::string formatComputedNumber(float value);

/// The resolved values of a computed style by CSS property name (colors as
/// rgb()/rgba(), lengths with their unit), in the order the Computed pane
/// shows them. Properties that do not apply (flex-direction outside a flex
/// container, an empty background-image, ...) are left out.
std::vector<std::pair<std::string, std::string>> computedStyleProperties(const CSSComputedStyle& style);

/// Every property name computedStyleProperties() can list, in the same order
const std::vector<std::string>& computedStylePropertyNames();

} // namespace Zepra::WebCore
