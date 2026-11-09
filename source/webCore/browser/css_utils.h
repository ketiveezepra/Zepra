// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * css_utils.h - CSS Processing Utilities for ZepraBrowser
 * 
 * Contains helpers for:
 * - Polyfilling shorthands (background -> background-color)
 * - Extracting styles from raw HTML (parser workaround)
 * - Color conversion
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <css/css_computed_style.hpp>

namespace ZepraBrowser {

/**
 * @brief Convert WebCore CSSColor to uint32_t RGB (0xRRGGBB)
 */
uint32_t cssColorToRGB(const Zepra::WebCore::CSSColor& c);

/**
 * @brief text-transform (CSS Text 3 §2.1): uppercase, lowercase or
 * capitalize applied to UTF-8 text, for Latin, Greek and Cyrillic letters.
 * Other values (none, full-width, ...) leave the text as it is.
 */
std::string applyTextTransform(const std::string& text, const std::string& transform);

/**
 * @brief Polyfill CSS shorthands (background -> background-color, etc.)
 */
std::string polyfillCSSShorthands(const std::string& css);

/**
 * @brief Extract CSS from raw HTML <style> tags
 * Workaround for HTMLParser issue where styles aren't added to head.
 * Includes UTF-8 safe processing and auto-calls expand/polyfill.
 */
std::vector<std::string> extractStylesFromRawHTML(const std::string& html);

std::string extractBodyInlineStyle(const std::string& html);
std::string stripOuterTags(const std::string& html);

} // namespace ZepraBrowser
