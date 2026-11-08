/**
 * @file data_url.cpp
 * @brief data: URL decoding (moved here from the image loader so web fonts
 *        can use it too).
 */

#include "data_url.hpp"

#include <algorithm>
#include <cctype>

namespace Zepra::WebCore {

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

std::string percentDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0) {
            out += (char)(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

bool base64Decode(const std::string& in, std::vector<uint8_t>& out) {
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=' || std::isspace((unsigned char)c)) continue;
        else return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)(acc >> bits));
        }
    }
    return true;
}

// data:[<mediatype>][;base64],<data>
bool decodeDataUrl(const std::string& url, std::vector<uint8_t>& data, std::string& contentType) {
    size_t comma = url.find(',');
    if (comma == std::string::npos) return false;
    std::string meta = url.substr(5, comma - 5);
    std::transform(meta.begin(), meta.end(), meta.begin(), ::tolower);
    bool isBase64 = meta.size() >= 7 && meta.compare(meta.size() - 7, 7, ";base64") == 0;
    contentType = meta.substr(0, meta.find(';'));
    std::string payload = percentDecode(url.substr(comma + 1));
    if (isBase64) return base64Decode(payload, data);
    data.assign(payload.begin(), payload.end());
    return true;
}

} // namespace Zepra::WebCore
