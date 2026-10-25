// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * presentational_hints.hpp - Legacy HTML attributes as style
 *
 * The HTML Standard's Rendering section maps attributes such as bgcolor,
 * width, align, valign, cellpadding and <font size> to CSS. These hints sit
 * in the cascade after the user-agent sheet and before every author rule.
 */

#pragma once

#include <string>

namespace Zepra::WebCore {

class DOMElement;

/// CSS declarations ("prop: value; ...") the element's presentational
/// attributes map to; empty when it has none
std::string presentationalHintStyle(const DOMElement* element);

/// HTML "rules for parsing a legacy colour value": "#rrggbb", a CSS colour
/// name, or empty for an invalid value
std::string parseLegacyColor(const std::string& value);

/// HTML "rules for parsing a legacy font size" (<font size>): 1-7, or 0
int parseLegacyFontSize(const std::string& value);

} // namespace Zepra::WebCore
