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

} // namespace

CSSComputedStyle::CSSComputedStyle() = default;

CSSComputedStyle CSSComputedStyle::inherit(const CSSComputedStyle& parent) {
    CSSComputedStyle style;
    style.color = parent.color;
    style.fontFamily = parent.fontFamily;
    style.fontSize = parent.fontSize;
    style.fontWeight = parent.fontWeight;
    style.fontStyle = parent.fontStyle;
    style.lineHeight = parent.lineHeight;
    style.textAlign = parent.textAlign;
    style.legacyAlign = parent.legacyAlign;
    style.whiteSpace = parent.whiteSpace;
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
            return containerSize > 0 ? value / 100.0f * containerSize : value / 100.0f * fontSize;
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

CSSLength CSSLength::parse(const std::string& str) {
    if (str.empty() || str == "auto") return auto_();

    try {
        size_t pos = 0;
        float val = std::stof(str, &pos);
        std::string u = str.substr(pos);

        if (u.empty() || u == "px") return {val, Unit::Px};
        if (u == "em") return {val, Unit::Em};
        if (u == "rem") return {val, Unit::Rem};
        if (u == "%") return {val, Unit::Percent};
        if (u == "vw") return {val, Unit::Vw};
        if (u == "vh") return {val, Unit::Vh};
        if (u == "pt") return {val, Unit::Pt};
        if (u == "pc") return {val, Unit::Pc};
        if (u == "in") return {val, Unit::In};
        if (u == "cm") return {val, Unit::Cm};
        if (u == "mm") return {val, Unit::Mm};
        if (u == "ch") return {val, Unit::Ch};
        if (u == "ex") return {val, Unit::Ex};
        if (u == "vmin") return {val, Unit::Vmin};
        if (u == "vmax") return {val, Unit::Vmax};
        if (u == "fr") return {val, Unit::Fr};
        return {val, Unit::Px};
    } catch (...) {
        return auto_();
    }
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

    return black();
}

} // namespace Zepra::WebCore
