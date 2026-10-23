// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file number_conversion.cpp
 * @brief Number <-> string conversions with ECMAScript semantics
 */

#include "runtime/objects/number_conversion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace Zepra::Runtime::NumberConversion {

namespace {

// A positive finite number as decimal digits: value = digits * 10^(n - digits.size()),
// the form the ECMAScript algorithms use. `digits` has no leading or trailing zeros.
struct Decimal {
    std::string digits;
    int n = 0;
};

Decimal parseScientific(const char* text) {
    Decimal d;
    const char* p = text;
    while (*p && *p != 'e') {
        if (*p != '.') d.digits += *p;
        ++p;
    }
    int exponent = *p == 'e' ? std::atoi(p + 1) : 0;
    d.n = exponent + 1;
    size_t last = d.digits.find_last_not_of('0');
    d.digits.erase(last == std::string::npos ? 1 : last + 1);
    return d;
}

// Fewest digits that read back as the same double
Decimal shortestDecimal(double x) {
    char buf[48];
    for (int precision = 1; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*e", precision - 1, x);
        if (std::strtod(buf, nullptr) == x) break;
    }
    return parseScientific(buf);
}

// Every digit of the double's exact binary value (at most 767 significant digits)
Decimal exactDecimal(double x) {
    char buf[820];
    std::snprintf(buf, sizeof(buf), "%.770e", x);
    return parseScientific(buf);
}

// Add one to a string of decimal digits; returns true when it grew a digit
bool incrementDigits(std::string& digits) {
    for (size_t i = digits.size(); i-- > 0;) {
        if (digits[i] == '9') {
            digits[i] = '0';
        } else {
            ++digits[i];
            return false;
        }
    }
    digits.insert(digits.begin(), '1');
    return true;
}

// Round to `count` significant digits, ties away from zero (the larger n)
Decimal roundToSignificant(const Decimal& d, int count) {
    Decimal r;
    r.n = d.n;
    if (count >= static_cast<int>(d.digits.size())) {
        r.digits = d.digits + std::string(count - d.digits.size(), '0');
        return r;
    }
    r.digits = d.digits.substr(0, count);
    if (d.digits[count] >= '5' && incrementDigits(r.digits)) {
        r.digits.pop_back();  // 99.9 -> 100: same digit count, one more power of ten
        r.n++;
    }
    return r;
}

std::string exponentSuffix(int e) {
    return std::string("e") + (e < 0 ? "-" : "+") + std::to_string(e < 0 ? -e : e);
}

// ES Number::toString layout of digits and the decimal exponent n
std::string layoutDecimal(const Decimal& d) {
    const int k = static_cast<int>(d.digits.size());
    const int n = d.n;
    if (k <= n && n <= 21) return d.digits + std::string(n - k, '0');
    if (0 < n && n <= 21) return d.digits.substr(0, n) + "." + d.digits.substr(n);
    if (-6 < n && n <= 0) return "0." + std::string(-n, '0') + d.digits;
    std::string out(1, d.digits[0]);
    if (k > 1) out += "." + d.digits.substr(1);
    return out + exponentSuffix(n - 1);
}

bool isNonFinite(double x, std::string& out) {
    if (std::isnan(x)) { out = "NaN"; return true; }
    if (std::isinf(x)) { out = x > 0 ? "Infinity" : "-Infinity"; return true; }
    return false;
}

// ---- parsing helpers ------------------------------------------------------

size_t skipWhitespace(const std::string& s, size_t i) {
    const auto* data = reinterpret_cast<const unsigned char*>(s.data());
    while (i < s.size()) {
        size_t len = whitespaceLength(data + i, s.size() - i);
        if (len == 0) break;
        i += len;
    }
    return i;
}

// Index one past the last non-whitespace byte
size_t trimmedEnd(const std::string& s, size_t begin) {
    const auto* data = reinterpret_cast<const unsigned char*>(s.data());
    size_t end = begin;
    for (size_t i = begin; i < s.size();) {
        size_t len = whitespaceLength(data + i, s.size() - i);
        if (len) { i += len; } else { ++i; end = i; }
    }
    return end;
}

int digitValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

// Length of the StrDecimalLiteral (without sign) at the start of `s`, or 0.
// "Infinity" counts. A trailing "e" without digits is not part of it.
size_t decimalLiteralLength(const char* s) {
    if (std::strncmp(s, "Infinity", 8) == 0) return 8;
    size_t i = 0;
    size_t intDigits = 0, fracDigits = 0;
    while (s[i] >= '0' && s[i] <= '9') { ++i; ++intDigits; }
    if (s[i] == '.') {
        size_t j = i + 1;
        while (s[j] >= '0' && s[j] <= '9') { ++j; ++fracDigits; }
        if (intDigits || fracDigits) i = j;
    }
    if (!intDigits && !fracDigits) return 0;
    if (s[i] == 'e' || s[i] == 'E') {
        size_t j = i + 1;
        if (s[j] == '+' || s[j] == '-') ++j;
        if (s[j] >= '0' && s[j] <= '9') {
            while (s[j] >= '0' && s[j] <= '9') ++j;
            i = j;
        }
    }
    return i;
}

double decimalLiteralValue(const std::string& literal) {
    if (literal == "Infinity") return std::numeric_limits<double>::infinity();
    return std::strtod(literal.c_str(), nullptr);
}

// 0x / 0o / 0b literal body ("1f" for "0x1f"), NaN when a digit is out of range
double radixLiteralValue(const std::string& body, int radix) {
    if (body.empty()) return std::nan("");
    double value = 0;
    for (char c : body) {
        int d = digitValue(c);
        if (d >= radix) return std::nan("");
        value = value * radix + d;
    }
    return value;
}

} // namespace

size_t whitespaceLength(const unsigned char* p, size_t remaining) {
    if (remaining == 0) return 0;
    unsigned char c = p[0];
    if (c == ' ' || (c >= 0x09 && c <= 0x0D)) return 1;
    if (c == 0xC2 && remaining >= 2 && p[1] == 0xA0) return 2;                       // U+00A0
    if (c == 0xEF && remaining >= 3 && p[1] == 0xBB && p[2] == 0xBF) return 3;       // U+FEFF
    if (c == 0xE1 && remaining >= 3 && p[1] == 0x9A && p[2] == 0x80) return 3;       // U+1680
    if (c == 0xE3 && remaining >= 3 && p[1] == 0x80 && p[2] == 0x80) return 3;       // U+3000
    if (c == 0xE2 && remaining >= 3) {
        if (p[1] == 0x80 && ((p[2] >= 0x80 && p[2] <= 0x8A) || p[2] == 0xA8 ||
                             p[2] == 0xA9 || p[2] == 0xAF)) return 3;                // U+2000-200A, 2028, 2029, 202F
        if (p[1] == 0x81 && p[2] == 0x9F) return 3;                                  // U+205F
    }
    return 0;
}

std::string toString(double value) {
    std::string special;
    if (isNonFinite(value, special)) return special;
    if (value == 0) return "0";
    if (value < 0) return "-" + toString(-value);

    // Exact integers below 2^53 are the common case
    if (value < 9007199254740992.0 && value == std::floor(value)) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
        return buf;
    }
    return layoutDecimal(shortestDecimal(value));
}

std::string toStringRadix(double value, int radix) {
    if (radix == 10) return toString(value);
    std::string special;
    if (isNonFinite(value, special)) return special;
    if (value == 0) return "0";
    if (value < 0) return "-" + toStringRadix(-value, radix);

    static const char* chars = "0123456789abcdefghijklmnopqrstuvwxyz";
    // Same digit generation as V8: fraction digits until the remainder is below
    // half the gap to the next double, then the integer part by repeated division.
    constexpr int kHalf = 1100;
    char buffer[2 * kHalf + 2];
    int integerCursor = kHalf;
    int fractionCursor = kHalf;

    double integer = std::floor(value);
    double fraction = value - integer;
    double delta = 0.5 * (std::nextafter(value, std::numeric_limits<double>::infinity()) - value);
    delta = std::max(std::nextafter(0.0, 1.0), delta);

    if (fraction >= delta) {
        buffer[fractionCursor++] = '.';
        do {
            fraction *= radix;
            delta *= radix;
            int digit = static_cast<int>(fraction);
            buffer[fractionCursor++] = chars[digit];
            fraction -= digit;
            if (fraction > 0.5 || (fraction == 0.5 && (digit & 1))) {
                if (fraction + delta > 1) {
                    // Round up, carrying through digits that are already radix - 1
                    while (true) {
                        fractionCursor--;
                        if (fractionCursor == kHalf) {
                            integer += 1;
                            break;
                        }
                        int last = digitValue(buffer[fractionCursor]);
                        if (last + 1 < radix) {
                            buffer[fractionCursor++] = chars[last + 1];
                            break;
                        }
                    }
                    break;
                }
            }
        } while (fraction >= delta);
    }

    // Integers past 2^53 carry no digits below their precision: emit zeros
    while (std::ilogb(integer) > 52) {
        integer /= radix;
        buffer[--integerCursor] = '0';
    }
    do {
        double remainder = std::fmod(integer, radix);
        buffer[--integerCursor] = chars[static_cast<int>(remainder)];
        integer = (integer - remainder) / radix;
    } while (integer > 0);

    return std::string(buffer + integerCursor, buffer + fractionCursor);
}

std::string toFixed(double value, int digits) {
    if (std::isnan(value)) return "NaN";
    if (std::fabs(value) >= 1e21) return toString(value);

    const bool negative = value < 0;
    const double magnitude = std::fabs(value);

    // M = round-half-up(magnitude * 10^digits), built from the exact digits
    std::string scaled = "0";
    if (magnitude != 0) {
        Decimal d = exactDecimal(magnitude);
        const int keep = d.n + digits;  // digits of the expansion that survive
        if (keep >= 0) {
            std::string kept = d.digits.substr(0, std::min<size_t>(keep, d.digits.size()));
            kept.append(keep > static_cast<int>(kept.size()) ? keep - kept.size() : 0, '0');
            bool roundUp = keep < static_cast<int>(d.digits.size()) && d.digits[keep] >= '5';
            if (kept.empty()) kept = "0";
            if (roundUp) incrementDigits(kept);
            scaled = kept;
        }
    }

    std::string out;
    if (digits == 0) {
        out = scaled;
    } else {
        if (static_cast<int>(scaled.size()) <= digits) {
            scaled.insert(0, digits + 1 - scaled.size(), '0');
        }
        out = scaled.substr(0, scaled.size() - digits) + "." + scaled.substr(scaled.size() - digits);
    }
    return negative ? "-" + out : out;
}

std::string toExponential(double value, int digits) {
    std::string special;
    if (isNonFinite(value, special)) return special;

    const bool negative = value < 0;
    const double magnitude = std::fabs(value);

    Decimal d;
    if (magnitude == 0) {
        d.digits = std::string(digits < 0 ? 1 : digits + 1, '0');
        d.n = 1;
    } else if (digits < 0) {
        d = shortestDecimal(magnitude);
    } else {
        d = roundToSignificant(exactDecimal(magnitude), digits + 1);
    }

    std::string out(1, d.digits[0]);
    if (d.digits.size() > 1) out += "." + d.digits.substr(1);
    out += exponentSuffix(d.n - 1);
    return negative ? "-" + out : out;
}

std::string toPrecision(double value, int precision) {
    std::string special;
    if (isNonFinite(value, special)) return special;

    const bool negative = value < 0;
    const double magnitude = std::fabs(value);

    Decimal d;
    if (magnitude == 0) {
        d.digits = std::string(precision, '0');
        d.n = 1;
    } else {
        d = roundToSignificant(exactDecimal(magnitude), precision);
    }

    const int e = d.n - 1;
    std::string out;
    if (e < -6 || e >= precision) {
        out = std::string(1, d.digits[0]);
        if (precision > 1) out += "." + d.digits.substr(1);
        out += exponentSuffix(e);
    } else if (e == precision - 1) {
        out = d.digits;
    } else if (e >= 0) {
        out = d.digits.substr(0, e + 1) + "." + d.digits.substr(e + 1);
    } else {
        out = "0." + std::string(-(e + 1), '0') + d.digits;
    }
    return negative ? "-" + out : out;
}

double stringToNumber(const std::string& text) {
    size_t begin = skipWhitespace(text, 0);
    size_t end = trimmedEnd(text, begin);
    if (begin >= end) return 0.0;
    std::string s = text.substr(begin, end - begin);

    if (s.size() > 2 && s[0] == '0') {
        char marker = s[1];
        int radix = (marker == 'x' || marker == 'X') ? 16
                  : (marker == 'o' || marker == 'O') ? 8
                  : (marker == 'b' || marker == 'B') ? 2 : 0;
        if (radix) return radixLiteralValue(s.substr(2), radix);
    }

    size_t i = 0;
    bool negative = false;
    if (s[i] == '+' || s[i] == '-') { negative = s[i] == '-'; ++i; }
    size_t length = decimalLiteralLength(s.c_str() + i);
    if (length == 0 || i + length != s.size()) return std::nan("");
    double value = decimalLiteralValue(s.substr(i, length));
    return negative ? -value : value;
}

double parseFloat(const std::string& text) {
    size_t i = skipWhitespace(text, 0);
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) { negative = text[i] == '-'; ++i; }
    size_t length = decimalLiteralLength(text.c_str() + i);
    if (length == 0) return std::nan("");
    double value = decimalLiteralValue(text.substr(i, length));
    return negative ? -value : value;
}

double parseInt(const std::string& text, int radix) {
    size_t i = skipWhitespace(text, 0);
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) { negative = text[i] == '-'; ++i; }

    bool stripPrefix = true;
    if (radix != 0) {
        if (radix < 2 || radix > 36) return std::nan("");
        if (radix != 16) stripPrefix = false;
    } else {
        radix = 10;
    }
    if (stripPrefix && i + 1 < text.size() && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
        i += 2;
        radix = 16;
    }

    size_t start = i;
    while (i < text.size() && digitValue(text[i]) < radix) ++i;
    if (i == start) return std::nan("");

    double value;
    if (radix == 10) {
        value = std::strtod(text.substr(start, i - start).c_str(), nullptr);  // exact rounding for long inputs
    } else {
        value = 0;
        for (size_t k = start; k < i; ++k) value = value * radix + digitValue(text[k]);
    }
    return negative ? -value : value;
}

} // namespace Zepra::Runtime::NumberConversion
