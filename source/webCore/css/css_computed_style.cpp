// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file css_computed_style.cpp
 * @brief CSSComputedStyle, CSSLength, and CSSColor implementations
 */

#include "css/css_computed_style.hpp"
#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace Zepra::WebCore {

namespace {

// The number at s[pos], without throwing: std::stof threw on "." or "1e99" inside
// rgb()/hsl(), and nothing caught it, so one bad color in a page's CSS aborted
// the browser. Infinity and NaN read as 0.
bool readNumber(const std::string& s, size_t pos, float& value, size_t& used) {
    const char* begin = s.c_str() + pos;
    char* end = nullptr;
    value = std::strtof(begin, &end);
    used = static_cast<size_t>(end - begin);
    if (used == 0) return false;
    if (!std::isfinite(value)) value = 0;
    return true;
}

// A color channel 0..255 from any float (casting an out-of-range float is undefined)
uint8_t channel(float v) {
    if (!(v > 0)) return 0;
    if (v >= 255.0f) return 255;
    return static_cast<uint8_t>(v);
}

std::string trimmed(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\f"), b = s.find_last_not_of(" \t\r\n\f");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// The arguments inside a function's parentheses, split at top-level commas
std::vector<std::string> splitArguments(const std::string& inner) {
    std::vector<std::string> parts;
    std::string current;
    int depth = 0;
    for (char c : inner) {
        if (c == '(') depth++;
        else if (c == ')') depth--;
        if (c == ',' && depth == 0) {
            parts.push_back(trimmed(current));
            current.clear();
        } else {
            current += c;
        }
    }
    parts.push_back(trimmed(current));
    return parts;
}

// sRGB <-> OKLab through linear-light sRGB and LMS (CSS Color 4 §17)
float toLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float fromLinear(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

void srgbToOklab(const float rgb[3], float lab[3]) {
    float r = toLinear(rgb[0]), g = toLinear(rgb[1]), b = toLinear(rgb[2]);
    float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    lab[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    lab[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    lab[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

void oklabToSrgb(const float lab[3], float rgb[3]) {
    float l = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
    float m = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
    float s = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    rgb[0] = fromLinear(4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s);
    rgb[1] = fromLinear(-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s);
    rgb[2] = fromLinear(-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s);
}

// One color-mix() argument, "<color> [<percentage>]" with the percentage on
// either side; percent is -1 when it is not given
bool colorMixArgument(const std::string& arg, std::string& color, float& percent) {
    percent = -1;
    color = arg;
    auto readPercent = [&](const std::string& token) {
        if (token.size() < 2 || token.back() != '%') return false;
        char* end = nullptr;
        float v = std::strtof(token.c_str(), &end);
        if (end != token.c_str() + token.size() - 1 || !(v >= 0 && v <= 100)) return false;
        percent = v;
        return true;
    };
    size_t lastSpace = arg.find_last_of(' ');
    size_t firstSpace = arg.find(' ');
    if (lastSpace != std::string::npos && arg.find(')', lastSpace) == std::string::npos &&
        readPercent(arg.substr(lastSpace + 1))) {
        color = trimmed(arg.substr(0, lastSpace));
    } else if (firstSpace != std::string::npos && readPercent(arg.substr(0, firstSpace))) {
        color = trimmed(arg.substr(firstSpace + 1));
    }
    return !color.empty();
}

} // namespace

// color-mix(in <space>, <color> [p1], <color> [p2]) (CSS Color 5 §2): the
// two colors interpolated with premultiplied alpha in the given space,
// sRGB-family and polar spaces read as sRGB and lab as OKLab. The text is
// lower case; false when it is not a valid color-mix().
static bool parseColorMix(const std::string& text, CSSColor& out) {
    size_t open = text.find('(');
    if (open == std::string::npos || text.back() != ')') return false;
    auto args = splitArguments(text.substr(open + 1, text.size() - open - 2));
    if (args.size() != 3 || args[0].rfind("in ", 0) != 0) return false;
    std::string space = trimmed(args[0].substr(3));
    bool oklab = space.rfind("oklab", 0) == 0 || space.rfind("lab", 0) == 0 ||
                 space.rfind("oklch", 0) == 0 || space.rfind("lch", 0) == 0;

    std::string colorText[2];
    float pct[2];
    if (!colorMixArgument(args[1], colorText[0], pct[0]) || !colorMixArgument(args[2], colorText[1], pct[1]))
        return false;
    // Percentages: a missing one is the rest of 100%, both missing are
    // 50/50; a sum below 100% scales the result's alpha
    if (pct[0] < 0 && pct[1] < 0) pct[0] = pct[1] = 50;
    else if (pct[0] < 0) pct[0] = 100 - pct[1];
    else if (pct[1] < 0) pct[1] = 100 - pct[0];
    float sum = pct[0] + pct[1];
    if (sum <= 0) return false;
    float alphaScale = std::min(sum, 100.0f) / 100.0f;
    float w1 = pct[1] / sum;  // weight of the second color

    float comp[2][3], alpha[2];
    for (int i = 0; i < 2; i++) {
        CSSColor c = CSSColor::parse(colorText[i]);
        float rgb[3] = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f};
        alpha[i] = c.a / 255.0f;
        if (oklab) srgbToOklab(rgb, comp[i]);
        else std::copy(rgb, rgb + 3, comp[i]);
    }
    float a = alpha[0] * (1 - w1) + alpha[1] * w1;
    float mixed[3] = {0, 0, 0};
    if (a > 0) {
        for (int k = 0; k < 3; k++)
            mixed[k] = (comp[0][k] * alpha[0] * (1 - w1) + comp[1][k] * alpha[1] * w1) / a;
    }
    float rgb[3];
    if (oklab) oklabToSrgb(mixed, rgb);
    else std::copy(mixed, mixed + 3, rgb);
    auto to8 = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    out = {to8(rgb[0]), to8(rgb[1]), to8(rgb[2]), to8(a * alphaScale)};
    return true;
}

CSSComputedStyle::CSSComputedStyle() = default;

CSSComputedStyle CSSComputedStyle::inherit(const CSSComputedStyle& parent) {
    CSSComputedStyle style;
    style.color = parent.color;
    style.fontFamily = parent.fontFamily;
    style.fontSize = parent.fontSize;
    style.fontWeight = parent.fontWeight;
    style.fontStyle = parent.fontStyle;
    style.lineHeight = parent.lineHeight;
    style.lineHeightNormal = parent.lineHeightNormal;
    style.lineHeightPx = parent.lineHeightPx;
    style.textAlign = parent.textAlign;
    style.legacyAlign = parent.legacyAlign;
    style.whiteSpace = parent.whiteSpace;
    style.textTransform = parent.textTransform;
    style.letterSpacing = parent.letterSpacing;
    style.wordSpacing = parent.wordSpacing;
    style.visibility = parent.visibility;
    style.borderSpacingH = parent.borderSpacingH;
    style.borderSpacingV = parent.borderSpacingV;
    style.borderCollapse = parent.borderCollapse;
    style.hasTextFillColor = parent.hasTextFillColor;
    style.textFillColor = parent.textFillColor;
    style.customProperties = parent.customProperties;  // custom properties inherit
    return style;
}

// ============================================================================
// CSSLength
// ============================================================================

float CSSLength::toPx(float fontSize, float rootFontSize, float viewportWidth,
                      float viewportHeight, float containerSize) const {
    switch (unit) {
        case Unit::Px: return value;
        case Unit::Em: return value * fontSize;
        case Unit::Rem: return value * rootFontSize;
        case Unit::Percent:
            return (containerSize > 0 ? value / 100.0f * containerSize : value / 100.0f * fontSize) + pxOffset;
        case Unit::Vw: return value / 100.0f * viewportWidth;
        case Unit::Vh: return value / 100.0f * viewportHeight;
        case Unit::Vmin: return value / 100.0f * std::min(viewportWidth, viewportHeight);
        case Unit::Vmax: return value / 100.0f * std::max(viewportWidth, viewportHeight);
        case Unit::Pt: return value * 96.0f / 72.0f;
        case Unit::Pc: return value * 96.0f / 6.0f;
        case Unit::In: return value * 96.0f;
        case Unit::Cm: return value * 96.0f / 2.54f;
        case Unit::Mm: return value * 96.0f / 25.4f;
        case Unit::Ch: return value * fontSize * 0.5f;
        case Unit::Ex: return value * fontSize * 0.5f;
        case Unit::Fr: return value; // Grid-only, needs layout context
        case Unit::Auto: return 0;
    }
    return 0;
}

namespace {

// Lengths past this are clamped (Chromium's LayoutUnit range): Tailwind's
// rounded-full is 3.40282e38px, and a sum of such values overflows a float
constexpr float kMaxLength = 16777216.0f;

float clampLength(float v) {
    if (!std::isfinite(v)) return v > 0 ? kMaxLength : -kMaxLength;
    return std::clamp(v, -kMaxLength, kMaxLength);
}

// The unit named by a (lower-case) suffix; false for an unknown one. An
// empty suffix is px (unitless numbers are taken as px, as before)
bool lengthUnit(const std::string& u, CSSLength::Unit& out) {
    using Unit = CSSLength::Unit;
    static const std::unordered_map<std::string, Unit> units = {
        {"", Unit::Px},      {"px", Unit::Px},     {"em", Unit::Em},     {"rem", Unit::Rem},
        {"%", Unit::Percent}, {"vw", Unit::Vw},    {"vh", Unit::Vh},     {"vmin", Unit::Vmin},
        {"vmax", Unit::Vmax}, {"pt", Unit::Pt},    {"pc", Unit::Pc},     {"in", Unit::In},
        {"cm", Unit::Cm},    {"mm", Unit::Mm},     {"ch", Unit::Ch},     {"ex", Unit::Ex},
        {"fr", Unit::Fr},
        // Small/large/dynamic viewport units: the one viewport we have
        {"svw", Unit::Vw},   {"lvw", Unit::Vw},    {"dvw", Unit::Vw},
        {"svh", Unit::Vh},   {"lvh", Unit::Vh},    {"dvh", Unit::Vh},
    };
    auto it = units.find(u);
    if (it == units.end()) return false;
    out = it->second;
    return true;
}

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A number followed by a unit, or false for no number. Uses strtof, so
// "3.40282e38px" and "1e99px" read (the latter as the largest length) where
// std::stof threw. An unknown suffix is taken as px, as before.
bool readDimension(const std::string& s, float& value, CSSLength::Unit& unit) {
    // A digit, sign or point first: strtof would also read "inf" and "nan"
    if (s.empty() || !(std::isdigit(static_cast<unsigned char>(s[0])) || s[0] == '.' || s[0] == '-' || s[0] == '+')) {
        return false;
    }
    const char* begin = s.c_str();
    char* end = nullptr;
    value = std::strtof(begin, &end);  // out of range: +-infinity, clamped below
    if (end == begin) return false;
    if (!lengthUnit(lowerAscii(s.substr(static_cast<size_t>(end - begin))), unit)) unit = CSSLength::Unit::Px;
    value = clampLength(value);
    return true;
}

// ---------------------------------------------------------------------------
// Math functions (CSS Values 4 §10): calc(), min(), max(), clamp()
// ---------------------------------------------------------------------------

// An operand while evaluating: a number, a length (in `unit` until units are
// mixed, px after), a percentage, or a length plus a percentage. A number
// never mixes with the others except through * and /.
struct Term {
    double number = 0;
    double length = 0;
    double percent = 0;
    bool hasNumber = false, hasLength = false, hasPercent = false;
    CSSLength::Unit unit = CSSLength::Unit::Px;

    bool isNumber() const { return hasNumber && !hasLength && !hasPercent; }
    bool isDimension() const { return !hasNumber && (hasLength || hasPercent); }
};

class CalcParser {
public:
    CalcParser(const std::string& text, const CSSLengthContext& ctx) : s_(text), ctx_(ctx) {}

    // The whole text as one function call, nothing after it
    bool parseFunction(Term& out) {
        if (!function(out)) return false;
        ws();
        return i_ == s_.size();
    }

private:
    const std::string& s_;
    const CSSLengthContext& ctx_;
    size_t i_ = 0;
    int depth_ = 0;

    void ws() { while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) i_++; }
    char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }

    double factor(CSSLength::Unit u) const {
        return CSSLength{1.0f, u}.toPx(ctx_.fontSize, ctx_.rootFontSize, ctx_.viewportWidth, ctx_.viewportHeight, 0);
    }
    // Both lengths in the same unit, converting to px when they differ
    void unify(Term& a, Term& b) const {
        if (!a.hasLength || !b.hasLength || a.unit == b.unit) {
            if (!a.hasLength) a.unit = b.unit;
            return;
        }
        a.length *= factor(a.unit);
        b.length *= factor(b.unit);
        a.unit = b.unit = CSSLength::Unit::Px;
    }
    double px(const Term& t) const { return t.length * factor(t.unit); }

    bool add(Term& a, Term b, double sign) {
        if (a.isNumber() != b.isNumber()) return false;  // "1px + 2" has no type
        unify(a, b);
        a.number += sign * b.number;
        a.length += sign * b.length;
        a.percent += sign * b.percent;
        a.hasNumber |= b.hasNumber;
        a.hasLength |= b.hasLength;
        a.hasPercent |= b.hasPercent;
        return true;
    }
    bool multiply(Term& a, const Term& b) {
        if (!b.isNumber()) {
            if (!a.isNumber()) return false;  // "1px * 2px" is not a length
            Term r = b;
            r.number *= a.number;
            r.length *= a.number;
            r.percent *= a.number;
            a = r;
            return true;
        }
        a.number *= b.number;
        a.length *= b.number;
        a.percent *= b.number;
        return true;
    }
    bool divide(Term& a, const Term& b) {
        if (!b.isNumber() || b.number == 0) return false;
        a.number /= b.number;
        a.length /= b.number;
        a.percent /= b.number;
        return true;
    }

    // <calc-sum> = <calc-product> [ [ '+' | '-' ] <calc-product> ]*
    bool sum(Term& out) {
        if (!product(out)) return false;
        for (;;) {
            ws();
            char op = peek();
            if (op != '+' && op != '-') return true;
            i_++;
            Term rhs;
            if (!product(rhs)) return false;
            if (!add(out, rhs, op == '+' ? 1.0 : -1.0)) return false;
        }
    }
    // <calc-product> = <calc-value> [ [ '*' | '/' ] <calc-value> ]*
    bool product(Term& out) {
        if (!value(out)) return false;
        for (;;) {
            ws();
            char op = peek();
            if (op != '*' && op != '/') return true;
            i_++;
            Term rhs;
            if (!value(rhs)) return false;
            if (!(op == '*' ? multiply(out, rhs) : divide(out, rhs))) return false;
        }
    }
    // <calc-value> = <number> | <dimension> | <percentage> | ( <calc-sum> ) | <function>
    bool value(Term& out) {
        ws();
        if (++depth_ > 32) return false;
        bool ok = valueInner(out);
        depth_--;
        return ok;
    }
    bool valueInner(Term& out) {
        char c = peek();
        if (c == '(') {
            i_++;
            if (!sum(out)) return false;
            ws();
            if (peek() != ')') return false;
            i_++;
            return true;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '-') {
            size_t save = i_;
            if (function(out)) return true;
            i_ = save;
            if (c != '-') return false;
        }
        // A signed number with an optional unit or %
        size_t used;
        float v;
        if (!readNumber(s_, i_, v, used)) return false;
        i_ += used;
        size_t unitStart = i_;
        while (i_ < s_.size() && (std::isalpha(static_cast<unsigned char>(s_[i_])) || s_[i_] == '%')) i_++;
        std::string suffix = lowerAscii(s_.substr(unitStart, i_ - unitStart));
        out = Term();
        if (suffix.empty()) {
            out.hasNumber = true;
            out.number = v;
        } else if (suffix == "%") {
            out.hasPercent = true;
            out.percent = v;
        } else if (lengthUnit(suffix, out.unit) && out.unit != CSSLength::Unit::Fr) {
            out.hasLength = true;
            out.length = v;
        } else {
            return false;
        }
        return true;
    }
    // calc( <calc-sum> ), min()/max( <calc-sum># ), clamp( min, val, max )
    bool function(Term& out) {
        size_t start = i_;
        while (i_ < s_.size() && (std::isalpha(static_cast<unsigned char>(s_[i_])) || s_[i_] == '-')) i_++;
        std::string name = lowerAscii(s_.substr(start, i_ - start));
        if (peek() != '(') return false;
        i_++;
        std::vector<Term> args;
        for (;;) {
            Term t;
            if (!sum(t)) return false;
            args.push_back(t);
            ws();
            if (peek() == ',') { i_++; continue; }
            if (peek() == ')') { i_++; break; }
            return false;
        }
        if (name == "calc" || name == "-webkit-calc" || name == "-moz-calc") {
            if (args.size() != 1) return false;
            out = args[0];
            return true;
        }
        if (name == "min" || name == "max") {
            if (args.empty()) return false;
            return pick(args, name == "min" ? -1 : 1, out);
        }
        if (name == "clamp") {
            if (args.size() != 3) return false;
            Term low = args[0], high = args[2];
            // max(MIN, min(VAL, MAX))
            std::vector<Term> inner = {args[1], high};
            Term mid;
            if (!pick(inner, -1, mid)) return false;
            std::vector<Term> outer = {low, mid};
            return pick(outer, 1, out);
        }
        return false;
    }
    // The smallest (sign -1) or largest (sign 1) of comparable terms: all
    // numbers, all lengths (compared in px) or all plain percentages
    bool pick(const std::vector<Term>& args, int sign, Term& out) {
        auto key = [&](const Term& t) {
            if (t.isNumber()) return t.number;
            if (t.hasLength && !t.hasPercent) return px(t);
            return t.percent;
        };
        for (const Term& t : args) {
            bool sameKind = t.isNumber() == args[0].isNumber() && t.hasLength == args[0].hasLength &&
                            t.hasPercent == args[0].hasPercent;
            if (!sameKind || (t.hasLength && t.hasPercent)) return false;
        }
        size_t best = 0;
        for (size_t k = 1; k < args.size(); k++) {
            if (sign * (key(args[k]) - key(args[best])) > 0) best = k;
        }
        out = args[best];
        if (out.hasLength) {
            // Compared in px: unless every argument shares its unit, report px
            for (const Term& t : args) {
                if (t.unit != out.unit) { out.length = px(out); out.unit = CSSLength::Unit::Px; break; }
            }
        }
        return true;
    }
};

bool mathPrefix(const std::string& value, size_t& nameEnd) {
    size_t i = 0;
    while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) i++;
    size_t start = i;
    while (i < value.size() && (std::isalpha(static_cast<unsigned char>(value[i])) || value[i] == '-')) i++;
    if (i >= value.size() || value[i] != '(') return false;
    std::string name = lowerAscii(value.substr(start, i - start));
    nameEnd = i;
    return name == "calc" || name == "min" || name == "max" || name == "clamp" ||
           name == "-webkit-calc" || name == "-moz-calc";
}

} // namespace

bool isCSSMathFunction(const std::string& value) {
    size_t end;
    return mathPrefix(value, end);
}

bool evaluateCSSMath(const std::string& value, const CSSLengthContext& ctx, CSSMathValue& out) {
    size_t end;
    if (!mathPrefix(value, end)) return false;
    size_t b = value.find_first_not_of(" \t\r\n"), e = value.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return false;
    std::string trimmed = value.substr(b, e - b + 1);
    CalcParser parser(trimmed, ctx);
    Term t;
    if (!parser.parseFunction(t)) return false;
    out = CSSMathValue();
    if (t.isNumber()) {
        out.isNumber = true;
        out.number = static_cast<float>(t.number);
        return std::isfinite(out.number);
    }
    if (!t.isDimension()) return false;
    if (t.hasPercent) {
        out.length = CSSLength::percent(clampLength(static_cast<float>(t.percent)));
        if (t.hasLength) {
            out.length.pxOffset = clampLength(static_cast<float>(
                t.length * CSSLength{1.0f, t.unit}.toPx(ctx.fontSize, ctx.rootFontSize, ctx.viewportWidth,
                                                        ctx.viewportHeight, 0)));
        }
    } else {
        out.length = {clampLength(static_cast<float>(t.length)), t.unit};
    }
    return true;
}

CSSLength CSSLength::parse(const std::string& str, const CSSLengthContext& ctx) {
    if (str.empty() || str == "auto") return auto_();

    if (isCSSMathFunction(str)) {
        CSSMathValue math;
        if (!evaluateCSSMath(str, ctx, math)) return auto_();
        // A bare number in a length context: px, as a unitless "0" is below
        return math.isNumber ? px(math.number) : math.length;
    }

    float val;
    Unit unit;
    if (readDimension(str, val, unit)) return {val, unit};
    // Keywords (max-content, min-content, fit-content, none, ...) and
    // anything unreadable: auto, as before
    return auto_();
}

// ============================================================================
// CSSColor
// ============================================================================

static uint8_t hexCharVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return 0;
}

CSSColor CSSColor::fromHex(const std::string& hex) {
    std::string h = hex;
    if (!h.empty() && h[0] == '#') h = h.substr(1);

    CSSColor c;
    c.a = 255;

    if (h.length() == 3) {
        c.r = hexCharVal(h[0]) * 17;
        c.g = hexCharVal(h[1]) * 17;
        c.b = hexCharVal(h[2]) * 17;
    } else if (h.length() == 4) {
        c.r = hexCharVal(h[0]) * 17;
        c.g = hexCharVal(h[1]) * 17;
        c.b = hexCharVal(h[2]) * 17;
        c.a = hexCharVal(h[3]) * 17;
    } else if (h.length() == 6) {
        c.r = hexCharVal(h[0]) * 16 + hexCharVal(h[1]);
        c.g = hexCharVal(h[2]) * 16 + hexCharVal(h[3]);
        c.b = hexCharVal(h[4]) * 16 + hexCharVal(h[5]);
    } else if (h.length() == 8) {
        c.r = hexCharVal(h[0]) * 16 + hexCharVal(h[1]);
        c.g = hexCharVal(h[2]) * 16 + hexCharVal(h[3]);
        c.b = hexCharVal(h[4]) * 16 + hexCharVal(h[5]);
        c.a = hexCharVal(h[6]) * 16 + hexCharVal(h[7]);
    }
    return c;
}

bool parseBorderStyle(const std::string& keyword, BorderStyle& out) {
    static const std::unordered_map<std::string, BorderStyle> styles = {
        {"none", BorderStyle::None},     {"hidden", BorderStyle::Hidden}, {"solid", BorderStyle::Solid},
        {"dashed", BorderStyle::Dashed}, {"dotted", BorderStyle::Dotted}, {"double", BorderStyle::Double},
        {"groove", BorderStyle::Groove}, {"ridge", BorderStyle::Ridge},   {"inset", BorderStyle::Inset},
        {"outset", BorderStyle::Outset},
    };
    std::string k = keyword;
    std::transform(k.begin(), k.end(), k.begin(), ::tolower);
    auto it = styles.find(k);
    if (it == styles.end()) return false;
    out = it->second;
    return true;
}

CSSColor CSSColor::fromRGB(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return {r, g, b, a};
}

CSSColor CSSColor::parse(const std::string& str) {
    if (str.empty()) return black();

    // Hex colors
    if (str[0] == '#') return fromHex(str);

    // rgb()/rgba()
    if (str.substr(0, 4) == "rgb(" || str.substr(0, 5) == "rgba(") {
        size_t start = str.find('(') + 1;
        size_t end = str.find(')');
        if (start == std::string::npos || end == std::string::npos) return black();

        std::string inner = str.substr(start, end - start);
        int r = 0, g = 0, b = 0, a = 255;

        // Parse comma or space separated values
        size_t pos = 0;
        auto nextNum = [&]() -> int {
            while (pos < inner.length() && !std::isdigit(static_cast<unsigned char>(inner[pos])) && inner[pos] != '.' && inner[pos] != '-') pos++;
            if (pos >= inner.length()) return 0;
            size_t numPos;
            float val;
            if (!readNumber(inner, pos, val, numPos)) { pos++; return 0; }
            pos += numPos;
            // Check for percentage
            while (pos < inner.length() && inner[pos] == ' ') pos++;
            if (pos < inner.length() && inner[pos] == '%') { pos++; return channel(val * 255.0f / 100.0f); }
            return channel(val);
        };

        r = nextNum();
        g = nextNum();
        b = nextNum();
        if (str.substr(0, 5) == "rgba(") {
            // Alpha can be 0-1 float or 0-255 int
            while (pos < inner.length() && !std::isdigit(static_cast<unsigned char>(inner[pos])) && inner[pos] != '.') pos++;
            float alpha;
            size_t used;
            if (pos < inner.length() && readNumber(inner, pos, alpha, used)) {
                a = channel(alpha <= 1.0f ? alpha * 255.0f : alpha);
            }
        }
        return {(uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a};
    }
    
    // hsl()/hsla()
    if (str.substr(0, 4) == "hsl(" || str.substr(0, 5) == "hsla(") {
        size_t start = str.find('(') + 1;
        size_t end = str.find(')');
        if (start != std::string::npos && end != std::string::npos) {
            std::string inner = str.substr(start, end - start);
            size_t pos = 0;
            auto nextNum = [&]() -> float {
                while (pos < inner.length() && !std::isdigit(static_cast<unsigned char>(inner[pos])) && inner[pos] != '.' && inner[pos] != '-') pos++;
                if (pos >= inner.length()) return 0;
                size_t numPos;
                float val;
                if (!readNumber(inner, pos, val, numPos)) { pos++; return 0; }
                pos += numPos;
                while (pos < inner.length() && inner[pos] == ' ') pos++;
                if (pos < inner.length() && inner[pos] == '%') { pos++; }
                return val;
            };
            
            float h = nextNum();
            float s = nextNum() / 100.0f;
            float l = nextNum() / 100.0f;
            int a = 255;
            
            if (str.substr(0, 5) == "hsla(") {
                float alpha = nextNum();
                if (alpha > 1.0f && std::to_string(alpha).find('.') == std::string::npos) alpha /= 255.0f;
                a = channel(alpha <= 1.0f ? alpha * 255.0f : alpha);
            }
            s = std::clamp(s, 0.0f, 1.0f);
            l = std::clamp(l, 0.0f, 1.0f);

            // Normalize hue to 0-360 (fmod: subtracting 360 from 1e30 changes
            // nothing, and that loop never ended)
            h = std::fmod(h, 360.0f);
            if (h < 0) h += 360.0f;
            h /= 360.0f;
            
            auto hue2rgb = [](float p, float q, float t) -> float {
                if (t < 0.0f) t += 1.0f;
                if (t > 1.0f) t -= 1.0f;
                if (t < 1.0f/6.0f) return p + (q - p) * 6.0f * t;
                if (t < 1.0f/2.0f) return q;
                if (t < 2.0f/3.0f) return p + (q - p) * (2.0f/3.0f - t) * 6.0f;
                return p;
            };
            
            float r_f, g_f, b_f;
            if (s == 0) {
                r_f = g_f = b_f = l; // achromatic
            } else {
                float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
                float p = 2.0f * l - q;
                r_f = hue2rgb(p, q, h + 1.0f/3.0f);
                g_f = hue2rgb(p, q, h);
                b_f = hue2rgb(p, q, h - 1.0f/3.0f);
            }
            
            return {channel(r_f * 255.0f), channel(g_f * 255.0f), channel(b_f * 255.0f), (uint8_t)a};
        }
    }

    // Named colors: the 148 keywords of CSS Color 4 §6.1
    static const std::unordered_map<std::string, CSSColor> named = {
        {"aliceblue", {240, 248, 255, 255}},
        {"antiquewhite", {250, 235, 215, 255}},
        {"aqua", {0, 255, 255, 255}},
        {"aquamarine", {127, 255, 212, 255}},
        {"azure", {240, 255, 255, 255}},
        {"beige", {245, 245, 220, 255}},
        {"bisque", {255, 228, 196, 255}},
        {"black", {0, 0, 0, 255}},
        {"blanchedalmond", {255, 235, 205, 255}},
        {"blue", {0, 0, 255, 255}},
        {"blueviolet", {138, 43, 226, 255}},
        {"brown", {165, 42, 42, 255}},
        {"burlywood", {222, 184, 135, 255}},
        {"cadetblue", {95, 158, 160, 255}},
        {"chartreuse", {127, 255, 0, 255}},
        {"chocolate", {210, 105, 30, 255}},
        {"coral", {255, 127, 80, 255}},
        {"cornflowerblue", {100, 149, 237, 255}},
        {"cornsilk", {255, 248, 220, 255}},
        {"crimson", {220, 20, 60, 255}},
        {"cyan", {0, 255, 255, 255}},
        {"darkblue", {0, 0, 139, 255}},
        {"darkcyan", {0, 139, 139, 255}},
        {"darkgoldenrod", {184, 134, 11, 255}},
        {"darkgray", {169, 169, 169, 255}},
        {"darkgreen", {0, 100, 0, 255}},
        {"darkgrey", {169, 169, 169, 255}},
        {"darkkhaki", {189, 183, 107, 255}},
        {"darkmagenta", {139, 0, 139, 255}},
        {"darkolivegreen", {85, 107, 47, 255}},
        {"darkorange", {255, 140, 0, 255}},
        {"darkorchid", {153, 50, 204, 255}},
        {"darkred", {139, 0, 0, 255}},
        {"darksalmon", {233, 150, 122, 255}},
        {"darkseagreen", {143, 188, 143, 255}},
        {"darkslateblue", {72, 61, 139, 255}},
        {"darkslategray", {47, 79, 79, 255}},
        {"darkslategrey", {47, 79, 79, 255}},
        {"darkturquoise", {0, 206, 209, 255}},
        {"darkviolet", {148, 0, 211, 255}},
        {"deeppink", {255, 20, 147, 255}},
        {"deepskyblue", {0, 191, 255, 255}},
        {"dimgray", {105, 105, 105, 255}},
        {"dimgrey", {105, 105, 105, 255}},
        {"dodgerblue", {30, 144, 255, 255}},
        {"firebrick", {178, 34, 34, 255}},
        {"floralwhite", {255, 250, 240, 255}},
        {"forestgreen", {34, 139, 34, 255}},
        {"fuchsia", {255, 0, 255, 255}},
        {"gainsboro", {220, 220, 220, 255}},
        {"ghostwhite", {248, 248, 255, 255}},
        {"gold", {255, 215, 0, 255}},
        {"goldenrod", {218, 165, 32, 255}},
        {"gray", {128, 128, 128, 255}},
        {"green", {0, 128, 0, 255}},
        {"greenyellow", {173, 255, 47, 255}},
        {"grey", {128, 128, 128, 255}},
        {"honeydew", {240, 255, 240, 255}},
        {"hotpink", {255, 105, 180, 255}},
        {"indianred", {205, 92, 92, 255}},
        {"indigo", {75, 0, 130, 255}},
        {"ivory", {255, 255, 240, 255}},
        {"khaki", {240, 230, 140, 255}},
        {"lavender", {230, 230, 250, 255}},
        {"lavenderblush", {255, 240, 245, 255}},
        {"lawngreen", {124, 252, 0, 255}},
        {"lemonchiffon", {255, 250, 205, 255}},
        {"lightblue", {173, 216, 230, 255}},
        {"lightcoral", {240, 128, 128, 255}},
        {"lightcyan", {224, 255, 255, 255}},
        {"lightgoldenrodyellow", {250, 250, 210, 255}},
        {"lightgray", {211, 211, 211, 255}},
        {"lightgreen", {144, 238, 144, 255}},
        {"lightgrey", {211, 211, 211, 255}},
        {"lightpink", {255, 182, 193, 255}},
        {"lightsalmon", {255, 160, 122, 255}},
        {"lightseagreen", {32, 178, 170, 255}},
        {"lightskyblue", {135, 206, 250, 255}},
        {"lightslategray", {119, 136, 153, 255}},
        {"lightslategrey", {119, 136, 153, 255}},
        {"lightsteelblue", {176, 196, 222, 255}},
        {"lightyellow", {255, 255, 224, 255}},
        {"lime", {0, 255, 0, 255}},
        {"limegreen", {50, 205, 50, 255}},
        {"linen", {250, 240, 230, 255}},
        {"magenta", {255, 0, 255, 255}},
        {"maroon", {128, 0, 0, 255}},
        {"mediumaquamarine", {102, 205, 170, 255}},
        {"mediumblue", {0, 0, 205, 255}},
        {"mediumorchid", {186, 85, 211, 255}},
        {"mediumpurple", {147, 112, 219, 255}},
        {"mediumseagreen", {60, 179, 113, 255}},
        {"mediumslateblue", {123, 104, 238, 255}},
        {"mediumspringgreen", {0, 250, 154, 255}},
        {"mediumturquoise", {72, 209, 204, 255}},
        {"mediumvioletred", {199, 21, 133, 255}},
        {"midnightblue", {25, 25, 112, 255}},
        {"mintcream", {245, 255, 250, 255}},
        {"mistyrose", {255, 228, 225, 255}},
        {"moccasin", {255, 228, 181, 255}},
        {"navajowhite", {255, 222, 173, 255}},
        {"navy", {0, 0, 128, 255}},
        {"oldlace", {253, 245, 230, 255}},
        {"olive", {128, 128, 0, 255}},
        {"olivedrab", {107, 142, 35, 255}},
        {"orange", {255, 165, 0, 255}},
        {"orangered", {255, 69, 0, 255}},
        {"orchid", {218, 112, 214, 255}},
        {"palegoldenrod", {238, 232, 170, 255}},
        {"palegreen", {152, 251, 152, 255}},
        {"paleturquoise", {175, 238, 238, 255}},
        {"palevioletred", {219, 112, 147, 255}},
        {"papayawhip", {255, 239, 213, 255}},
        {"peachpuff", {255, 218, 185, 255}},
        {"peru", {205, 133, 63, 255}},
        {"pink", {255, 192, 203, 255}},
        {"plum", {221, 160, 221, 255}},
        {"powderblue", {176, 224, 230, 255}},
        {"purple", {128, 0, 128, 255}},
        {"rebeccapurple", {102, 51, 153, 255}},
        {"red", {255, 0, 0, 255}},
        {"rosybrown", {188, 143, 143, 255}},
        {"royalblue", {65, 105, 225, 255}},
        {"saddlebrown", {139, 69, 19, 255}},
        {"salmon", {250, 128, 114, 255}},
        {"sandybrown", {244, 164, 96, 255}},
        {"seagreen", {46, 139, 87, 255}},
        {"seashell", {255, 245, 238, 255}},
        {"sienna", {160, 82, 45, 255}},
        {"silver", {192, 192, 192, 255}},
        {"skyblue", {135, 206, 235, 255}},
        {"slateblue", {106, 90, 205, 255}},
        {"slategray", {112, 128, 144, 255}},
        {"slategrey", {112, 128, 144, 255}},
        {"snow", {255, 250, 250, 255}},
        {"springgreen", {0, 255, 127, 255}},
        {"steelblue", {70, 130, 180, 255}},
        {"tan", {210, 180, 140, 255}},
        {"teal", {0, 128, 128, 255}},
        {"thistle", {216, 191, 216, 255}},
        {"tomato", {255, 99, 71, 255}},
        {"turquoise", {64, 224, 208, 255}},
        {"violet", {238, 130, 238, 255}},
        {"wheat", {245, 222, 179, 255}},
        {"white", {255, 255, 255, 255}},
        {"whitesmoke", {245, 245, 245, 255}},
        {"yellow", {255, 255, 0, 255}},
        {"yellowgreen", {154, 205, 50, 255}},
        {"transparent", {0, 0, 0, 0}},
        {"inherit", {0, 0, 0, 255}},
        {"initial", {0, 0, 0, 255}},
        {"currentcolor", {0, 0, 0, 255}},
    };

    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    // Trim
    while (!lower.empty() && std::isspace(lower.front())) lower.erase(lower.begin());
    while (!lower.empty() && std::isspace(lower.back())) lower.pop_back();

    auto it = named.find(lower);
    if (it != named.end()) return it->second;

    CSSColor mixed;
    if (lower.rfind("color-mix(", 0) == 0 && parseColorMix(lower, mixed)) return mixed;

    return black();
}

} // namespace Zepra::WebCore
