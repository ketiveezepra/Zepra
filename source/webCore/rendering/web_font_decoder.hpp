/**
 * @file web_font_decoder.hpp
 * @brief Turns downloaded @font-face data into a plain sfnt (TrueType/OpenType)
 *        that FreeType can load.
 *
 * TTF, OTF and TTC pass through. WOFF 1.0 is unpacked with Zepra's own
 * inflate, WOFF2 with Zepra's own Brotli decoder, rebuilding the glyf, loca
 * and hmtx tables its transforms store in a smaller form.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Zepra::Fonts {

enum class WebFontFormat { Unknown, TrueType, OpenType, Collection, Woff, Woff2 };

/// Identify a font file by its first four bytes.
WebFontFormat sniffWebFontFormat(const uint8_t* data, size_t size);

/// Whether a src format() hint names a format decodeWebFont can handle.
/// An empty hint means none was given, which counts as supported.
bool isSupportedFontFormatHint(const std::string& format);

struct WebFontDecodeResult {
    bool ok = false;
    WebFontFormat format = WebFontFormat::Unknown;
    std::string error;          ///< why decoding failed, for the console
    std::vector<uint8_t> sfnt;  ///< the font to hand to FreeType
};

/// Largest font decodeWebFont will produce. Real fonts stay far below it;
/// the limit keeps a hostile file from claiming gigabytes.
constexpr size_t kMaxDecodedFontSize = 64u * 1024u * 1024u;

WebFontDecodeResult decodeWebFont(const uint8_t* data, size_t size);

/// WOFF2 only (woff2_decoder.cpp); decodeWebFont calls it for wOF2 data
WebFontDecodeResult decodeWoff2(const uint8_t* data, size_t size);
inline WebFontDecodeResult decodeWebFont(const std::vector<uint8_t>& data) {
    return decodeWebFont(data.data(), data.size());
}

} // namespace Zepra::Fonts
