/**
 * @file data_url.hpp
 * @brief data: URLs (RFC 2397) and the decoding they need, shared by images
 *        and web fonts.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Zepra::WebCore {

/// %XX escapes undone; a malformed escape stays as written
std::string percentDecode(const std::string& s);

/// Standard or URL-safe base64; whitespace and padding are skipped
bool base64Decode(const std::string& in, std::vector<uint8_t>& out);

/// data:[<mediatype>][;base64],<data>. contentType gets the media type, lowercased.
bool decodeDataUrl(const std::string& url, std::vector<uint8_t>& data, std::string& contentType);

} // namespace Zepra::WebCore
