// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
#pragma once

/**
 * @file number_conversion.hpp
 * @brief Number <-> string conversions with ECMAScript semantics
 *
 * Number::toString (shortest digits that round-trip), the radix, fixed,
 * exponential and precision formats, and the string-to-number parsers
 * (StringToNumber, parseFloat, parseInt). The digit generation uses the C
 * library's exact printf/strtod, so it assumes the "C" numeric locale, which
 * the browser never changes.
 */

#include <cstddef>
#include <string>

namespace Zepra::Runtime::NumberConversion {

/** Number::toString(x) in radix 10: "123456789", "0.1", "1e+21", "1.5e-7". */
std::string toString(double value);

/**
 * Bytes of the JavaScript whitespace or line terminator (space, tab, NBSP,
 * BOM, U+2028, ...) that starts at `p`, or 0 when `p` starts anything else.
 */
size_t whitespaceLength(const unsigned char* p, size_t remaining);

/** Number.prototype.toString(radix) for radix 2..36, fraction included. */
std::string toStringRadix(double value, int radix);

/** Number.prototype.toFixed: digits in [0, 100]. */
std::string toFixed(double value, int digits);

/**
 * Number.prototype.toExponential. `digits` is the count after the point
 * (0..100), or -1 for "as many as are needed".
 */
std::string toExponential(double value, int digits);

/** Number.prototype.toPrecision: precision in [1, 100]. */
std::string toPrecision(double value, int precision);

/** StringToNumber: Number("  0x1f "), Number("12px") is NaN, Number("") is 0. */
double stringToNumber(const std::string& text);

/** parseFloat: the longest decimal-literal prefix, NaN when there is none. */
double parseFloat(const std::string& text);

/** parseInt: `radix` 0 means 10 (or 16 after a 0x prefix); NaN on failure. */
double parseInt(const std::string& text, int radix);

} // namespace Zepra::Runtime::NumberConversion
