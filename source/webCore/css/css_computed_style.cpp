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
#include <unordered_map>

namespace Zepra::WebCore {

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
            while (pos < inner.length() && !std::isdigit(inner[pos]) && inner[pos] != '.') pos++;
            if (pos >= inner.length()) return 0;
            size_t numPos;
            float val = std::stof(inner.substr(pos), &numPos);
            pos += numPos;
            // Check for percentage
            while (pos < inner.length() && inner[pos] == ' ') pos++;
            if (pos < inner.length() && inner[pos] == '%') { pos++; return (int)(val * 255.0f / 100.0f); }
            return (int)val;
        };

        r = nextNum();
        g = nextNum();
        b = nextNum();
        if (str.substr(0, 5) == "rgba(") {
            // Alpha can be 0-1 float or 0-255 int
            while (pos < inner.length() && !std::isdigit(inner[pos]) && inner[pos] != '.') pos++;
            if (pos < inner.length()) {
                float alpha = std::stof(inner.substr(pos));
                a = alpha <= 1.0f ? (int)(alpha * 255.0f) : (int)alpha;
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
                while (pos < inner.length() && !std::isdigit(inner[pos]) && inner[pos] != '.' && inner[pos] != '-') pos++;
                if (pos >= inner.length()) return 0;
                size_t numPos;
                float val = std::stof(inner.substr(pos), &numPos);
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
                if (alpha <= 1.0f) a = (int)(alpha * 255.0f);
                else a = (int)alpha;
            }
            
            // Normalize hue to 0-360
            while (h < 0) h += 360.0f;
            while (h >= 360.0f) h -= 360.0f;
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
            
            return {(uint8_t)(r_f * 255.0f), (uint8_t)(g_f * 255.0f), (uint8_t)(b_f * 255.0f), (uint8_t)a};
        }
    }

    // Named colors (most common)
    static const std::unordered_map<std::string, CSSColor> named = {
        {"black", {0, 0, 0, 255}},
        {"white", {255, 255, 255, 255}},
        {"red", {255, 0, 0, 255}},
        {"green", {0, 128, 0, 255}},
        {"blue", {0, 0, 255, 255}},
        {"yellow", {255, 255, 0, 255}},
        {"cyan", {0, 255, 255, 255}},
        {"magenta", {255, 0, 255, 255}},
        {"orange", {255, 165, 0, 255}},
        {"purple", {128, 0, 128, 255}},
        {"gray", {128, 128, 128, 255}},
        {"grey", {128, 128, 128, 255}},
        {"silver", {192, 192, 192, 255}},
        {"maroon", {128, 0, 0, 255}},
        {"olive", {128, 128, 0, 255}},
        {"lime", {0, 255, 0, 255}},
        {"aqua", {0, 255, 255, 255}},
        {"teal", {0, 128, 128, 255}},
        {"navy", {0, 0, 128, 255}},
        {"fuchsia", {255, 0, 255, 255}},
        {"pink", {255, 192, 203, 255}},
        {"brown", {165, 42, 42, 255}},
        {"coral", {255, 127, 80, 255}},
        {"gold", {255, 215, 0, 255}},
        {"indigo", {75, 0, 130, 255}},
        {"violet", {238, 130, 238, 255}},
        {"salmon", {250, 128, 114, 255}},
        {"tomato", {255, 99, 71, 255}},
        {"turquoise", {64, 224, 208, 255}},
        {"khaki", {240, 230, 140, 255}},
        {"crimson", {220, 20, 60, 255}},
        {"darkblue", {0, 0, 139, 255}},
        {"darkgreen", {0, 100, 0, 255}},
        {"darkred", {139, 0, 0, 255}},
        {"darkgray", {169, 169, 169, 255}},
        {"darkgrey", {169, 169, 169, 255}},
        {"lightblue", {173, 216, 230, 255}},
        {"lightgreen", {144, 238, 144, 255}},
        {"lightgray", {211, 211, 211, 255}},
        {"lightgrey", {211, 211, 211, 255}},
        {"lightyellow", {255, 255, 224, 255}},
        {"whitesmoke", {245, 245, 245, 255}},
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
