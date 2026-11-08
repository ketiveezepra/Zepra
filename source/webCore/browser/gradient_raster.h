// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file gradient_raster.h
 * @brief CSS gradient backgrounds drawn on the CPU into an RGBA bitmap.
 *
 * Covers linear-, radial- and conic-gradient() and their repeating-
 * forms (CSS Images 3 §3, CSS Images 4 §3.3 for conic), with colour stops,
 * double-position stops and transition hints, interpolated in premultiplied
 * sRGB. Several layers are composited bottom to top with their own
 * background-size, background-position and background-repeat (CSS
 * Backgrounds 3 §3), and the result is clipped to the border box's radius.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ZepraBrowser {

/// What a box's gradient background needs to be drawn
struct GradientBackground {
    std::string image;     ///< background-image: one or more comma-separated layers
    std::string size;      ///< background-size (comma list, "" = auto)
    std::string position;  ///< background-position (comma list, "" = 0% 0%)
    std::string repeat;    ///< background-repeat (comma list, "" = repeat)
    float width = 0, height = 0;  ///< border box, CSS px
    float borderTop = 0, borderRight = 0, borderBottom = 0, borderLeft = 0;
    float radius = 0;      ///< uniform border radius
};

/// True when every layer of `image` is a gradient this rasterizer draws
/// (no url() layers)
bool isGradientBackground(const std::string& image);

/// The bitmap size for a box: its size, scaled down to a pixel budget for
/// very large boxes (gradients stay smooth when stretched)
void gradientPixelSize(float width, float height, int& pixelWidth, int& pixelHeight);

/// Draws the background into `rgba` (straight alpha, row-major, top row
/// first) at pixelWidth x pixelHeight; the box is scaled to fit. Layers it
/// cannot read are skipped. False when nothing could be drawn.
bool rasterizeGradientBackground(const GradientBackground& bg, int pixelWidth, int pixelHeight,
                                 std::vector<uint8_t>& rgba);

/// A cache key: the same key draws the same pixels
std::string gradientBackgroundKey(const GradientBackground& bg, int pixelWidth, int pixelHeight);

} // namespace ZepraBrowser
