/**
 * @file brotli.hpp
 * @brief Zepra's own Brotli decoder (RFC 7932).
 *
 * Needed for WOFF2 fonts and for "Content-Encoding: br". Like inflate, the
 * output is bounded by maxOut and malformed or truncated input returns an
 * error without reading out of range.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace Zepra::Compress {

enum class BrotliStatus {
    Ok,
    Truncated,  ///< input ended before the stream did
    BadData,    ///< the stream breaks the format
    TooLarge,   ///< output would pass maxOut
};

const char* brotliStatusName(BrotliStatus status);

/// Decode a whole Brotli stream, appending to out
BrotliStatus brotliDecode(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                          size_t maxOut = std::numeric_limits<size_t>::max());

/// The RFC 7932 static dictionary, for tests
const uint8_t* brotliDictionaryData();
size_t brotliDictionarySize();

} // namespace Zepra::Compress
