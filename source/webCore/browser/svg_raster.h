// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file svg_raster.h
 * @brief SVG documents to RGBA pixels, through the vendored nanosvg.
 *
 * Used for every SVG the browser paints: <img> and data: SVGs, inline <svg>
 * and the browser's own icons. nanosvg parses and rasterizes; prepare() first
 * rewrites what it does not understand into what it does:
 *  - <use> becomes a <g> holding a copy of the referenced element (<symbol>s
 *    get their viewBox mapped onto the use's width/height);
 *  - <style> rules with simple selectors (tag, .class, #id) are merged into
 *    the matching elements' style attributes;
 *  - names the HTML parser lowercased (lineargradient, viewbox, ...) get their
 *    SVG case back;
 *  - elements that are never painted directly (symbol, clipPath, mask,
 *    pattern, marker, filter, foreignObject) are dropped.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ZepraBrowser {
namespace SvgRaster {

struct Options {
    uint32_t currentColor = 0x000000;  // 0xRRGGBB, the value of currentColor at the root
    bool whiteIsCurrentColor = false;  // browser icons: white paint takes the tint
};

// The document rewritten for nanosvg (exposed for tests)
std::string prepare(const std::string& svg, const Options& options = {});

// Intrinsic size: width/height attributes, then the viewBox ratio fitted into
// 300x150, then 300x150.
// Returns false when the text has no <svg> element.
bool intrinsicSize(const std::string& svg, float& width, float& height);

// Draws the document into a width x height RGBA buffer (straight alpha), scaled
// uniformly and centred (preserveAspectRatio xMidYMid meet).
bool rasterize(const std::string& svg, int width, int height, const Options& options,
               std::vector<uint8_t>& rgba);

} // namespace SvgRaster
} // namespace ZepraBrowser
