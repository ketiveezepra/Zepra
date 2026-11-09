// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file blend_modes.h
 * @brief mix-blend-mode (Compositing and Blending 1 §5): the 16 blend
 *        modes, applied to one source colour over an opaque backdrop.
 */

#pragma once

#include <cstdint>
#include <string>

namespace ZepraBrowser {

enum class BlendMode : uint8_t {
    Normal, Multiply, Screen, Overlay, Darken, Lighten, ColorDodge, ColorBurn,
    HardLight, SoftLight, Difference, Exclusion, Hue, Saturation, Color, Luminosity,
};

/// A mix-blend-mode keyword; Normal for anything else
BlendMode parseBlendMode(const std::string& keyword);

/// The colour (0xRRGGBB) of `source` with `alpha` (0..255) blended over the
/// opaque `backdrop`: (1 - a) * Cb + a * B(Cb, Cs) (§5.1)
uint32_t blendPixel(BlendMode mode, uint32_t backdrop, uint32_t source, uint8_t alpha);

} // namespace ZepraBrowser
