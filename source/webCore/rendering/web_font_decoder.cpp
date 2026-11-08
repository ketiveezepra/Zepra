/**
 * @file web_font_decoder.cpp
 * @brief WOFF 1.0 unpacking (https://www.w3.org/TR/WOFF/) and font sniffing.
 *        WOFF2 is in woff2_decoder.cpp.
 */

#include "web_font_decoder.hpp"

#include "core/inflate.hpp"

#include <algorithm>
#include <cctype>

namespace Zepra::Fonts {

namespace {

uint32_t be32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

void put32(std::vector<uint8_t>& out, size_t at, uint32_t v) {
    out[at] = static_cast<uint8_t>(v >> 24);
    out[at + 1] = static_cast<uint8_t>(v >> 16);
    out[at + 2] = static_cast<uint8_t>(v >> 8);
    out[at + 3] = static_cast<uint8_t>(v);
}
void put16(std::vector<uint8_t>& out, size_t at, uint16_t v) {
    out[at] = static_cast<uint8_t>(v >> 8);
    out[at + 1] = static_cast<uint8_t>(v);
}

constexpr uint32_t tag(char a, char b, char c, char d) {
    return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) |
           (uint32_t(uint8_t(c)) << 8) | uint32_t(uint8_t(d));
}

constexpr size_t kWoffHeaderSize = 44;
constexpr size_t kWoffEntrySize = 20;
constexpr size_t kSfntHeaderSize = 12;
constexpr size_t kSfntEntrySize = 16;

struct WoffTable {
    uint32_t tag, offset, compLength, origLength, checksum;
};

WebFontDecodeResult fail(WebFontFormat format, std::string why) {
    WebFontDecodeResult r;
    r.format = format;
    r.error = std::move(why);
    return r;
}

WebFontDecodeResult decodeWoff(const uint8_t* data, size_t size) {
    constexpr WebFontFormat kFmt = WebFontFormat::Woff;
    if (size < kWoffHeaderSize) return fail(kFmt, "WOFF header is truncated");
    uint32_t flavor = be32(data + 4);
    uint32_t length = be32(data + 8);
    uint16_t numTables = be16(data + 12);
    uint16_t reserved = be16(data + 14);
    if (length > size) return fail(kFmt, "WOFF file is shorter than its header says");
    if (numTables == 0) return fail(kFmt, "WOFF file has no tables");
    if (reserved != 0) return fail(kFmt, "WOFF header reserved field is not zero");
    if (kWoffHeaderSize + size_t{numTables} * kWoffEntrySize > length)
        return fail(kFmt, "WOFF table directory is truncated");

    std::vector<WoffTable> tables(numTables);
    size_t sfntSize = kSfntHeaderSize + size_t{numTables} * kSfntEntrySize;
    for (size_t i = 0; i < numTables; ++i) {
        const uint8_t* e = data + kWoffHeaderSize + i * kWoffEntrySize;
        WoffTable& t = tables[i];
        t = {be32(e), be32(e + 4), be32(e + 8), be32(e + 12), be32(e + 16)};
        if (t.offset > length || length - t.offset < t.compLength)
            return fail(kFmt, "WOFF table data lies outside the file");
        if (t.compLength > t.origLength)
            return fail(kFmt, "WOFF table is larger compressed than uncompressed");
        sfntSize += (size_t{t.origLength} + 3) & ~size_t{3};
        if (sfntSize > kMaxDecodedFontSize) return fail(kFmt, "WOFF font is too large");
    }

    // sfnt readers binary-search the directory, so it must be sorted by tag
    std::sort(tables.begin(), tables.end(),
              [](const WoffTable& a, const WoffTable& b) { return a.tag < b.tag; });
    for (size_t i = 1; i < tables.size(); ++i)
        if (tables[i].tag == tables[i - 1].tag) return fail(kFmt, "WOFF file repeats a table");

    WebFontDecodeResult result;
    result.format = kFmt;
    std::vector<uint8_t>& out = result.sfnt;
    out.assign(kSfntHeaderSize + size_t{numTables} * kSfntEntrySize, 0);
    out.reserve(sfntSize);

    uint16_t entrySelector = 0;
    while ((2u << entrySelector) <= numTables) ++entrySelector;
    uint16_t searchRange = static_cast<uint16_t>((1u << entrySelector) * 16);
    put32(out, 0, flavor);
    put16(out, 4, numTables);
    put16(out, 6, searchRange);
    put16(out, 8, entrySelector);
    put16(out, 10, static_cast<uint16_t>(numTables * 16 - searchRange));

    for (size_t i = 0; i < tables.size(); ++i) {
        const WoffTable& t = tables[i];
        size_t at = out.size();
        const uint8_t* src = data + t.offset;
        if (t.compLength == t.origLength) {
            out.insert(out.end(), src, src + t.origLength);
        } else {
            auto status = Compress::inflateZlib(src, t.compLength, out, t.origLength);
            if (status != Compress::InflateStatus::Ok)
                return fail(kFmt, std::string("WOFF table could not be decompressed: ") +
                                      Compress::inflateStatusName(status));
            if (out.size() - at != t.origLength)
                return fail(kFmt, "WOFF table decompressed to the wrong size");
        }
        out.resize((out.size() + 3) & ~size_t{3}, 0);

        size_t entry = kSfntHeaderSize + i * kSfntEntrySize;
        put32(out, entry, t.tag);
        put32(out, entry + 4, t.checksum);
        put32(out, entry + 8, static_cast<uint32_t>(at));
        put32(out, entry + 12, t.origLength);
    }

    result.ok = true;
    return result;
}

} // namespace

WebFontFormat sniffWebFontFormat(const uint8_t* data, size_t size) {
    if (size < 4) return WebFontFormat::Unknown;
    switch (be32(data)) {
        case 0x00010000u:
        case tag('t', 'r', 'u', 'e'): return WebFontFormat::TrueType;
        case tag('O', 'T', 'T', 'O'): return WebFontFormat::OpenType;
        case tag('t', 't', 'c', 'f'): return WebFontFormat::Collection;
        case tag('w', 'O', 'F', 'F'): return WebFontFormat::Woff;
        case tag('w', 'O', 'F', '2'): return WebFontFormat::Woff2;
        default: return WebFontFormat::Unknown;
    }
}

bool isSupportedFontFormatHint(const std::string& format) {
    std::string f;
    for (char c : format) f += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (f.empty()) return true;
    static const char* const kSupported[] = {
        "truetype", "opentype", "woff", "woff2", "collection",
        "truetype-variations", "opentype-variations", "woff-variations", "woff2-variations",
    };
    for (const char* s : kSupported)
        if (f == s) return true;
    return false;
}

WebFontDecodeResult decodeWebFont(const uint8_t* data, size_t size) {
    WebFontFormat format = sniffWebFontFormat(data, size);
    switch (format) {
        case WebFontFormat::TrueType:
        case WebFontFormat::OpenType:
        case WebFontFormat::Collection: {
            if (size > kMaxDecodedFontSize) return fail(format, "font is too large");
            WebFontDecodeResult r;
            r.ok = true;
            r.format = format;
            r.sfnt.assign(data, data + size);
            return r;
        }
        case WebFontFormat::Woff: return decodeWoff(data, size);
        case WebFontFormat::Woff2: return decodeWoff2(data, size);
        case WebFontFormat::Unknown: break;
    }
    return fail(format, "not a font file");
}

} // namespace Zepra::Fonts
