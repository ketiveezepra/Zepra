/**
 * @file inflate.hpp
 * @brief Zepra's own DEFLATE decoder (RFC 1951) with the zlib (RFC 1950)
 *        and gzip (RFC 1952) wrappers around it.
 *
 * Used for WOFF font tables now, and for gzip/deflate Content-Encoding later.
 * Every function is bounded: output never grows past maxOut, and malformed
 * or truncated input returns an error instead of reading out of range.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace Zepra::Compress {

enum class InflateStatus {
    Ok,
    Truncated,     ///< input ended before the stream did
    BadData,       ///< the stream breaks the format
    BadChecksum,   ///< zlib Adler-32 or gzip CRC-32/size didn't match
    TooLarge,      ///< output would pass maxOut
};

const char* inflateStatusName(InflateStatus status);

constexpr size_t kNoOutputLimit = std::numeric_limits<size_t>::max();

/// Decode a raw DEFLATE stream, appending to out. consumed (if given) gets
/// the number of input bytes the stream used, rounded up to a whole byte.
InflateStatus inflateRaw(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                         size_t maxOut = kNoOutputLimit, size_t* consumed = nullptr);

/// Decode a zlib stream (2-byte header, DEFLATE data, Adler-32).
InflateStatus inflateZlib(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                          size_t maxOut = kNoOutputLimit, size_t* consumed = nullptr);

/// Decode a gzip file: one or more members, each checked against its CRC-32 and size.
InflateStatus inflateGzip(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                          size_t maxOut = kNoOutputLimit);

uint32_t adler32(const uint8_t* data, size_t size, uint32_t start = 1);
uint32_t crc32(const uint8_t* data, size_t size, uint32_t start = 0);

} // namespace Zepra::Compress
