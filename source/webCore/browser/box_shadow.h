// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file box_shadow.h
 * @brief box-shadow (CSS Backgrounds 3 §7): parsing, and each shadow drawn
 *        on the CPU into an RGBA bitmap with its Gaussian blur.
 *
 * An outer shadow is the border box moved by its offset and grown by its
 * spread, blurred, and never drawn under the box itself. An inset shadow is
 * drawn inside the padding box, around the padding box moved and shrunk
 * the same way.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ZepraBrowser {

struct BoxShadow {
    bool inset = false;
    float offsetX = 0, offsetY = 0;
    float blur = 0;    ///< blur radius; the Gaussian's deviation is half of it
    float spread = 0;
    uint8_t r = 0, g = 0, b = 0, a = 255;
};

/// The shadows of a box-shadow value, first (topmost) first. A shadow
/// without a colour takes `currentColor` (0xRRGGBB). "none" and unreadable
/// values give none.
std::vector<BoxShadow> parseBoxShadow(const std::string& value, uint32_t currentColor);

/// The box a shadow belongs to: its border box and border widths
struct ShadowBox {
    float width = 0, height = 0;
    float borderTop = 0, borderRight = 0, borderBottom = 0, borderLeft = 0;
    float radius = 0;  ///< uniform border radius
};

/// Where the shadow's bitmap goes, relative to the border box's top-left
struct ShadowRect {
    float x = 0, y = 0, width = 0, height = 0;
};
ShadowRect shadowRect(const ShadowBox& box, const BoxShadow& shadow);

/// The bitmap size for a shadow rectangle (scaled down for very large ones)
void shadowPixelSize(const ShadowRect& rect, int& pixelWidth, int& pixelHeight);

/// Draws the shadow over shadowRect() into `rgba` (straight alpha). False
/// when it has nothing to draw.
bool rasterizeShadow(const ShadowBox& box, const BoxShadow& shadow, int pixelWidth, int pixelHeight,
                     std::vector<uint8_t>& rgba);

/// A cache key: the same key draws the same pixels
std::string shadowKey(const ShadowBox& box, const BoxShadow& shadow, int pixelWidth, int pixelHeight);

} // namespace ZepraBrowser
