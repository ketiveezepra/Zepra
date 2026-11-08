// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file css_supports.hpp
 * @brief Feature queries for page scripts: CSS.supports() and CSS.escape()
 *        (CSS Conditional 3 §6.1 and §8.1, CSSOM §2.1).
 *
 * A declaration is supported when the style resolver applies its property
 * (custom properties always) and its value parses as a declaration value.
 */

#pragma once

#include <string>

namespace Zepra::WebCore {

/// CSS.supports(property, value)
bool cssSupportsDeclaration(const std::string& property, const std::string& value);

/// CSS.supports(conditionText): "(display: grid) and (not (color: nope))",
/// selector(...), or a bare declaration ("display: grid")
bool cssSupportsCondition(const std::string& conditionText);

/// CSS.escape(ident): the identifier escaped so it can be used in a selector
std::string cssEscapeIdentifier(const std::string& ident);

} // namespace Zepra::WebCore
