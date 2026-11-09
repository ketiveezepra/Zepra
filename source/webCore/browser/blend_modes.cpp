// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file blend_modes.cpp
 * @brief The blend functions of Compositing and Blending 1 §5.3 and §5.4
 */

#include "blend_modes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>

namespace ZepraBrowser {

namespace {

struct RGB {
    float r, g, b;
};

RGB unpack(uint32_t c) {
    return {((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f};
}

uint32_t pack(RGB c) {
    auto ch = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    return (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
}

// Separable modes, one channel (§5.3)
float separable(BlendMode mode, float cb, float cs) {
    switch (mode) {
        case BlendMode::Multiply: return cb * cs;
        case BlendMode::Screen: return cb + cs - cb * cs;
        case BlendMode::Overlay: return separable(BlendMode::HardLight, cs, cb);
        case BlendMode::Darken: return std::min(cb, cs);
        case BlendMode::Lighten: return std::max(cb, cs);
        case BlendMode::ColorDodge:
            if (cb == 0) return 0;
            if (cs >= 1) return 1;
            return std::min(1.0f, cb / (1 - cs));
        case BlendMode::ColorBurn:
            if (cb >= 1) return 1;
            if (cs <= 0) return 0;
            return 1 - std::min(1.0f, (1 - cb) / cs);
        case BlendMode::HardLight:
            return cs <= 0.5f ? cb * 2 * cs : separable(BlendMode::Screen, cb, 2 * cs - 1);
        case BlendMode::SoftLight: {
            if (cs <= 0.5f) return cb - (1 - 2 * cs) * cb * (1 - cb);
            float d = cb <= 0.25f ? ((16 * cb - 12) * cb + 4) * cb : std::sqrt(cb);
            return cb + (2 * cs - 1) * (d - cb);
        }
        case BlendMode::Difference: return std::fabs(cb - cs);
        case BlendMode::Exclusion: return cb + cs - 2 * cb * cs;
        default: return cs;
    }
}

// Non-separable helpers (§5.4)
float lum(RGB c) { return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b; }

RGB clipColor(RGB c) {
    float l = lum(c);
    float n = std::min({c.r, c.g, c.b}), x = std::max({c.r, c.g, c.b});
    if (n < 0) c = {l + (c.r - l) * l / (l - n), l + (c.g - l) * l / (l - n), l + (c.b - l) * l / (l - n)};
    if (x > 1) c = {l + (c.r - l) * (1 - l) / (x - l), l + (c.g - l) * (1 - l) / (x - l), l + (c.b - l) * (1 - l) / (x - l)};
    return c;
}

RGB setLum(RGB c, float l) {
    float d = l - lum(c);
    return clipColor({c.r + d, c.g + d, c.b + d});
}

float sat(RGB c) { return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}); }

RGB setSat(RGB c, float s) {
    float* ch[3] = {&c.r, &c.g, &c.b};
    std::sort(ch, ch + 3, [](float* a, float* b) { return *a < *b; });
    float& mn = *ch[0];
    float& mid = *ch[1];
    float& mx = *ch[2];
    if (mx > mn) {
        mid = (mid - mn) * s / (mx - mn);
        mx = s;
    } else {
        mid = mx = 0;
    }
    mn = 0;
    return c;
}

} // namespace

BlendMode parseBlendMode(const std::string& keyword) {
    static const std::unordered_map<std::string, BlendMode> modes = {
        {"normal", BlendMode::Normal},         {"multiply", BlendMode::Multiply},
        {"screen", BlendMode::Screen},         {"overlay", BlendMode::Overlay},
        {"darken", BlendMode::Darken},         {"lighten", BlendMode::Lighten},
        {"color-dodge", BlendMode::ColorDodge}, {"color-burn", BlendMode::ColorBurn},
        {"hard-light", BlendMode::HardLight},  {"soft-light", BlendMode::SoftLight},
        {"difference", BlendMode::Difference}, {"exclusion", BlendMode::Exclusion},
        {"hue", BlendMode::Hue},               {"saturation", BlendMode::Saturation},
        {"color", BlendMode::Color},           {"luminosity", BlendMode::Luminosity},
    };
    std::string k = keyword;
    for (char& c : k) c = (char)std::tolower((unsigned char)c);
    auto it = modes.find(k);
    return it == modes.end() ? BlendMode::Normal : it->second;
}

uint32_t blendPixel(BlendMode mode, uint32_t backdrop, uint32_t source, uint8_t alpha) {
    RGB cb = unpack(backdrop), cs = unpack(source);
    RGB mixed;
    switch (mode) {
        case BlendMode::Hue: mixed = setLum(setSat(cs, sat(cb)), lum(cb)); break;
        case BlendMode::Saturation: mixed = setLum(setSat(cb, sat(cs)), lum(cb)); break;
        case BlendMode::Color: mixed = setLum(cs, lum(cb)); break;
        case BlendMode::Luminosity: mixed = setLum(cb, lum(cs)); break;
        default:
            mixed = {separable(mode, cb.r, cs.r), separable(mode, cb.g, cs.g), separable(mode, cb.b, cs.b)};
            break;
    }
    float a = alpha / 255.0f;
    return pack({cb.r + (mixed.r - cb.r) * a, cb.g + (mixed.g - cb.g) * a, cb.b + (mixed.b - cb.b) * a});
}

} // namespace ZepraBrowser
