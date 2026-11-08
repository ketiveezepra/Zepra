// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file webcore_web_font_decoder_tests.cpp
 * @brief Zepra's own inflate (raw DEFLATE, zlib, gzip) against streams made
 *        by the reference zlib, and WOFF 1.0 and WOFF2 unpacking into an
 *        sfnt that FreeType loads.
 */

#include <gtest/gtest.h>

#include "core/inflate.hpp"
#include "rendering/font_face.hpp"
#include "rendering/web_font_decoder.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
#include "fixtures/web_fonts.inc"
#include "fixtures/woff2.inc"
}

#ifdef ZEPRA_HAS_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

using namespace Zepra::Compress;
using Zepra::Fonts::decodeWebFont;
using Zepra::Fonts::isSupportedFontFormatHint;
using Zepra::Fonts::sniffWebFontFormat;
using Zepra::Fonts::WebFontFormat;

namespace {

std::vector<uint8_t> expectedText() {
    std::string s;
    for (int i = 0; i < 3; ++i) s += kPlainPrefix;
    for (int i = 0; i < 256; ++i) s += static_cast<char>(i);
    for (int i = 0; i < 9; ++i) s += "abcabcabcabcabcabc";
    return std::vector<uint8_t>(s.begin(), s.end());
}

template <size_t N>
std::vector<uint8_t> bytes(const uint8_t (&a)[N]) {
    return std::vector<uint8_t>(a, a + N);
}

uint32_t be32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}
void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    v[at] = uint8_t(x >> 24);
    v[at + 1] = uint8_t(x >> 16);
    v[at + 2] = uint8_t(x >> 8);
    v[at + 3] = uint8_t(x);
}

std::vector<uint8_t> readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

/// Re-wrap an sfnt as WOFF with every table stored uncompressed.
std::vector<uint8_t> wrapAsStoredWoff(const std::vector<uint8_t>& sfnt) {
    uint16_t n = static_cast<uint16_t>((sfnt[4] << 8) | sfnt[5]);
    size_t header = 44 + 20 * size_t{n};
    std::vector<uint8_t> woff(header, 0);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* rec = sfnt.data() + 12 + 16 * i;
        uint32_t offset = be32(rec + 8), length = be32(rec + 12);
        size_t at = woff.size();
        woff.insert(woff.end(), sfnt.begin() + offset, sfnt.begin() + offset + length);
        woff.resize((woff.size() + 3) & ~size_t{3}, 0);
        size_t e = 44 + 20 * i;
        put32(woff, e, be32(rec));
        put32(woff, e + 4, static_cast<uint32_t>(at));
        put32(woff, e + 8, length);
        put32(woff, e + 12, length);
        put32(woff, e + 16, be32(rec + 4));
    }
    put32(woff, 0, 0x774F4646);  // 'wOFF'
    put32(woff, 4, be32(sfnt.data()));
    put32(woff, 8, static_cast<uint32_t>(woff.size()));
    woff[12] = uint8_t(n >> 8);
    woff[13] = uint8_t(n);
    return woff;
}

} // namespace

// --- inflate -------------------------------------------------------------

TEST(Inflate, DecodesTheThreeBlockTypes) {
    for (auto stream : {bytes(kZlibDynamic), bytes(kZlibFixed), bytes(kZlibStored)}) {
        std::vector<uint8_t> out;
        size_t consumed = 0;
        ASSERT_EQ(inflateZlib(stream.data(), stream.size(), out, kNoOutputLimit, &consumed),
                  InflateStatus::Ok);
        EXPECT_EQ(out, expectedText());
        EXPECT_EQ(consumed, stream.size());
    }
}

TEST(Inflate, DecodesGzipWithAFileName) {
    std::vector<uint8_t> out;
    ASSERT_EQ(inflateGzip(kGzipNamed, sizeof(kGzipNamed), out), InflateStatus::Ok);
    EXPECT_EQ(out, expectedText());
}

TEST(Inflate, GzipMembersConcatenate) {
    auto two = bytes(kGzipNamed);
    two.insert(two.end(), std::begin(kGzipNamed), std::end(kGzipNamed));
    std::vector<uint8_t> out;
    ASSERT_EQ(inflateGzip(two.data(), two.size(), out), InflateStatus::Ok);
    auto expected = expectedText();
    auto twice = expected;
    twice.insert(twice.end(), expected.begin(), expected.end());
    EXPECT_EQ(out, twice);
}

TEST(Inflate, EveryTruncationFails) {
    for (auto stream : {bytes(kZlibDynamic), bytes(kZlibFixed), bytes(kZlibStored)}) {
        for (size_t cut = 0; cut < stream.size(); ++cut) {
            std::vector<uint8_t> out;
            EXPECT_NE(inflateZlib(stream.data(), cut, out), InflateStatus::Ok) << "cut at " << cut;
        }
    }
    for (size_t cut = 0; cut < sizeof(kGzipNamed); ++cut) {
        std::vector<uint8_t> out;
        EXPECT_NE(inflateGzip(kGzipNamed, cut, out), InflateStatus::Ok) << "cut at " << cut;
    }
}

TEST(Inflate, ChecksumsAreVerified) {
    auto z = bytes(kZlibDynamic);
    z.back() ^= 1;
    std::vector<uint8_t> out;
    EXPECT_EQ(inflateZlib(z.data(), z.size(), out), InflateStatus::BadChecksum);

    auto g = bytes(kGzipNamed);
    g[g.size() - 8] ^= 1;  // CRC-32
    out.clear();
    EXPECT_EQ(inflateGzip(g.data(), g.size(), out), InflateStatus::BadChecksum);
}

TEST(Inflate, OutputLimitIsEnforced) {
    std::vector<uint8_t> out;
    size_t limit = expectedText().size() - 1;
    EXPECT_EQ(inflateZlib(kZlibDynamic, sizeof(kZlibDynamic), out, limit), InflateStatus::TooLarge);
    EXPECT_LE(out.size(), limit);
    out.clear();
    EXPECT_EQ(inflateZlib(kZlibStored, sizeof(kZlibStored), out, limit), InflateStatus::TooLarge);
}

TEST(Inflate, RejectsMalformedStreams) {
    std::vector<uint8_t> out;
    const uint8_t badType[] = {0x07};  // final block, type 3
    EXPECT_EQ(inflateRaw(badType, sizeof(badType), out), InflateStatus::BadData);

    const uint8_t badNlen[] = {0x01, 0x05, 0x00, 0x00, 0x00};  // stored, LEN != ~NLEN
    EXPECT_EQ(inflateRaw(badNlen, sizeof(badNlen), out), InflateStatus::BadData);

    // Fixed block whose first code is a match: nothing to copy from yet
    const uint8_t farMatch[] = {0x03, 0x02};
    EXPECT_EQ(inflateRaw(farMatch, sizeof(farMatch), out), InflateStatus::BadData);

    const uint8_t badZlibHeader[] = {0x78, 0x00};
    EXPECT_EQ(inflateZlib(badZlibHeader, sizeof(badZlibHeader), out), InflateStatus::BadData);
    const uint8_t presetDict[] = {0x78, 0xBB, 0, 0, 0, 0};
    EXPECT_EQ(inflateZlib(presetDict, sizeof(presetDict), out), InflateStatus::BadData);
}

TEST(Inflate, EmptyStoredBlock) {
    const uint8_t empty[] = {0x01, 0x00, 0x00, 0xFF, 0xFF};
    std::vector<uint8_t> out;
    EXPECT_EQ(inflateRaw(empty, sizeof(empty), out), InflateStatus::Ok);
    EXPECT_TRUE(out.empty());
}

TEST(Inflate, ChecksumFunctions) {
    const char* wiki = "Wikipedia";
    EXPECT_EQ(adler32(reinterpret_cast<const uint8_t*>(wiki), 9), 0x11E60398u);
    const char* digits = "123456789";
    EXPECT_EQ(crc32(reinterpret_cast<const uint8_t*>(digits), 9), 0xCBF43926u);
}

// --- WOFF ----------------------------------------------------------------

TEST(WebFontDecoder, SniffsFormats) {
    const uint8_t ttf[] = {0, 1, 0, 0}, otf[] = {'O', 'T', 'T', 'O'}, ttc[] = {'t', 't', 'c', 'f'};
    const uint8_t woff[] = {'w', 'O', 'F', 'F'}, woff2[] = {'w', 'O', 'F', '2'}, gif[] = {'G', 'I', 'F', '8'};
    EXPECT_EQ(sniffWebFontFormat(ttf, 4), WebFontFormat::TrueType);
    EXPECT_EQ(sniffWebFontFormat(otf, 4), WebFontFormat::OpenType);
    EXPECT_EQ(sniffWebFontFormat(ttc, 4), WebFontFormat::Collection);
    EXPECT_EQ(sniffWebFontFormat(woff, 4), WebFontFormat::Woff);
    EXPECT_EQ(sniffWebFontFormat(woff2, 4), WebFontFormat::Woff2);
    EXPECT_EQ(sniffWebFontFormat(gif, 4), WebFontFormat::Unknown);
    EXPECT_EQ(sniffWebFontFormat(ttf, 3), WebFontFormat::Unknown);
}

TEST(WebFontDecoder, FormatHints) {
    EXPECT_TRUE(isSupportedFontFormatHint(""));
    EXPECT_TRUE(isSupportedFontFormatHint("woff"));
    EXPECT_TRUE(isSupportedFontFormatHint("TrueType"));
    EXPECT_TRUE(isSupportedFontFormatHint("opentype"));
    EXPECT_TRUE(isSupportedFontFormatHint("woff2"));
    EXPECT_TRUE(isSupportedFontFormatHint("WOFF2-variations"));
    EXPECT_FALSE(isSupportedFontFormatHint("embedded-opentype"));
    EXPECT_FALSE(isSupportedFontFormatHint("svg"));
}

TEST(WebFontDecoder, UnpacksWoffTablesIntoASortedSfnt) {
    auto r = decodeWebFont(kWoffSynthetic, sizeof(kWoffSynthetic));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.format, WebFontFormat::Woff);
    const auto& s = r.sfnt;
    ASSERT_GE(s.size(), 12u + 32u);
    EXPECT_EQ(be32(s.data()), 0x00010000u);
    EXPECT_EQ((s[4] << 8) | s[5], 2);
    EXPECT_EQ((s[6] << 8) | s[7], 32);   // searchRange
    EXPECT_EQ((s[8] << 8) | s[9], 1);    // entrySelector
    EXPECT_EQ((s[10] << 8) | s[11], 0);  // rangeShift

    // The fixture lists 'zzzz' before 'aaaa'; the sfnt directory is sorted
    const uint8_t* first = s.data() + 12;
    const uint8_t* second = s.data() + 28;
    EXPECT_EQ(std::memcmp(first, "aaaa", 4), 0);
    EXPECT_EQ(std::memcmp(second, "zzzz", 4), 0);
    EXPECT_EQ(be32(first + 4), 0x12345678u);  // checksum carried over

    uint32_t aOff = be32(first + 8), aLen = be32(first + 12);
    uint32_t zOff = be32(second + 8), zLen = be32(second + 12);
    EXPECT_EQ(aOff % 4, 0u);
    EXPECT_EQ(zOff % 4, 0u);
    ASSERT_EQ(aLen, 3u);
    EXPECT_EQ(s[aOff], 7);
    EXPECT_EQ(s[aOff + 2], 11);
    std::string z;
    for (int i = 0; i < 40; ++i) z += "0123456789abcdef";
    ASSERT_EQ(zLen, z.size());  // this table was zlib-compressed in the WOFF
    EXPECT_EQ(std::memcmp(s.data() + zOff, z.data(), zLen), 0);
}

TEST(WebFontDecoder, RejectsBrokenWoff) {
    auto base = bytes(kWoffSynthetic);
    for (size_t cut : {size_t{0}, size_t{10}, size_t{43}, size_t{60}, base.size() - 1}) {
        auto r = decodeWebFont(base.data(), cut);
        EXPECT_FALSE(r.ok) << "cut at " << cut;
    }

    auto bigger = base;  // first table: compLength larger than origLength
    put32(bigger, 44 + 8, be32(bigger.data() + 44 + 12) + 1);
    EXPECT_FALSE(decodeWebFont(bigger).ok);

    auto outside = base;  // first table: offset past the end of the file
    put32(outside, 44 + 4, static_cast<uint32_t>(base.size()));
    EXPECT_FALSE(decodeWebFont(outside).ok);

    auto huge = base;  // a table claiming more than the decoded-font limit
    put32(huge, 44 + 12, 0x7FFFFFFF);
    EXPECT_FALSE(decodeWebFont(huge).ok);

    auto reserved = base;
    reserved[15] = 1;
    EXPECT_FALSE(decodeWebFont(reserved).ok);
}

TEST(WebFontDecoder, RealFontSurvivesWoffAndLoadsInFreeType) {
    auto ttf = readFile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    if (ttf.empty()) GTEST_SKIP() << "DejaVuSans.ttf is not installed";

    auto plain = decodeWebFont(ttf);
    ASSERT_TRUE(plain.ok);
    EXPECT_EQ(plain.sfnt, ttf);

    auto unpacked = decodeWebFont(wrapAsStoredWoff(ttf));
    ASSERT_TRUE(unpacked.ok) << unpacked.error;

#ifdef ZEPRA_HAS_FREETYPE
    ASSERT_TRUE(Zepra::Fonts::FontLibrary::instance().initialize());
    Zepra::Fonts::FontFace face;
    ASSERT_TRUE(face.loadFromMemory(unpacked.sfnt));
    EXPECT_EQ(face.familyName(), "DejaVu Sans");
    EXPECT_TRUE(face.hasGlyph(U'A'));
    EXPECT_TRUE(face.hasGlyph(U'é'));
#endif
}

// --- WOFF2 ---------------------------------------------------------------

namespace {

struct SfntTable {
    uint32_t tag, checksum, offset, length;
};

std::vector<SfntTable> sfntTables(const std::vector<uint8_t>& f, size_t at = 0) {
    std::vector<SfntTable> out;
    uint16_t n = static_cast<uint16_t>((f[at + 4] << 8) | f[at + 5]);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* e = f.data() + at + 12 + i * 16;
        out.push_back({be32(e), be32(e + 4), be32(e + 8), be32(e + 12)});
    }
    return out;
}

uint32_t tagOf(const char* s) {
    return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) | (uint32_t(uint8_t(s[2])) << 8) |
           uint32_t(uint8_t(s[3]));
}

uint32_t sumWords(const uint8_t* p, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i += 4) {
        uint8_t w[4] = {0, 0, 0, 0};
        for (size_t k = 0; k < 4 && i + k < n; ++k) w[k] = p[i + k];
        sum += be32(w);
    }
    return sum;
}

} // namespace

TEST(Woff2, UnpacksIntoAValidSfnt) {
    auto r = decodeWebFont(kWoff2Subset, sizeof(kWoff2Subset));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.format, WebFontFormat::Woff2);
    const auto& f = r.sfnt;
    ASSERT_GE(f.size(), 12u);
    EXPECT_EQ(be32(f.data()), 0x00010000u);
    auto tables = sfntTables(f);
    for (size_t i = 0; i < tables.size(); ++i) {
        const SfntTable& t = tables[i];
        if (i) EXPECT_LT(tables[i - 1].tag, t.tag);  // sorted for binary search
        ASSERT_LE(size_t{t.offset} + t.length, f.size());
        EXPECT_EQ(t.offset % 4, 0u);
        if (t.tag != tagOf("head")) EXPECT_EQ(sumWords(f.data() + t.offset, t.length), t.checksum);
    }
    // Checksums and head.checkSumAdjustment make the whole file sum to the magic
    EXPECT_EQ(sumWords(f.data(), f.size()), 0xB1B0AFBAu);

    // Tables stored as-is come out byte for byte
    for (const auto& want : kWoff2Tables) {
        bool found = false;
        for (const SfntTable& t : tables)
            if (t.tag == tagOf(want.tag)) {
                found = true;
                EXPECT_EQ(t.length, want.length) << want.tag;
                EXPECT_EQ(crc32(f.data() + t.offset, t.length), want.crc) << want.tag;
            }
        EXPECT_TRUE(found) << want.tag;
    }
}

#ifdef ZEPRA_HAS_FREETYPE
TEST(Woff2, RebuiltGlyphsAndMetricsMatchTheOriginalFont) {
    // glyf, loca and hmtx were transformed: points from triplets, implicit
    // bounding boxes, a composite, instructions, side bearings from xMin
    auto r = decodeWebFont(kWoff2Subset, sizeof(kWoff2Subset));
    ASSERT_TRUE(r.ok) << r.error;
    FT_Library lib;
    ASSERT_EQ(FT_Init_FreeType(&lib), 0);
    FT_Face face;
    ASSERT_EQ(FT_New_Memory_Face(lib, r.sfnt.data(), static_cast<FT_Long>(r.sfnt.size()), 0, &face), 0);
    ASSERT_EQ(face->num_glyphs, static_cast<FT_Long>(std::size(kWoff2Glyphs)));
    EXPECT_STREQ(face->family_name, "DejaVu Sans");
    for (FT_UInt g = 0; g < std::size(kWoff2Glyphs); ++g) {
        const Woff2Glyph& want = kWoff2Glyphs[g];
        ASSERT_EQ(FT_Load_Glyph(face, g, FT_LOAD_NO_SCALE), 0) << want.name;
        const FT_Outline& o = face->glyph->outline;
        EXPECT_EQ(o.n_contours, want.contours) << want.name;
        ASSERT_EQ(o.n_points, want.points) << want.name;
        std::vector<uint8_t> packed;
        for (int i = 0; i < o.n_points; ++i) {
            auto x = static_cast<uint16_t>(o.points[i].x), y = static_cast<uint16_t>(o.points[i].y);
            packed.insert(packed.end(), {uint8_t(x >> 8), uint8_t(x), uint8_t(y >> 8), uint8_t(y),
                                         uint8_t(FT_CURVE_TAG(o.tags[i]) == FT_CURVE_TAG_ON)});
        }
        EXPECT_EQ(crc32(packed.data(), packed.size()), want.crc) << want.name;
        EXPECT_EQ(face->glyph->metrics.horiAdvance, want.advance) << want.name;
        EXPECT_EQ(face->glyph->metrics.horiBearingX, want.lsb) << want.name;
        // And the hinting instructions run
        FT_Set_Pixel_Sizes(face, 0, 16);
        EXPECT_EQ(FT_Load_Glyph(face, g, FT_LOAD_DEFAULT | FT_LOAD_NO_AUTOHINT), 0) << want.name;
    }
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
}
#endif

TEST(Woff2, CollectionFontsShareTables) {
    auto r = decodeWebFont(kWoff2Collection, sizeof(kWoff2Collection));
    ASSERT_TRUE(r.ok) << r.error;
    const auto& f = r.sfnt;
    ASSERT_GE(f.size(), 20u);
    EXPECT_EQ(be32(f.data()), tagOf("ttcf"));
    ASSERT_EQ(be32(f.data() + 8), 2u);  // two fonts
    auto a = sfntTables(f, be32(f.data() + 12));
    auto b = sfntTables(f, be32(f.data() + 16));
    ASSERT_EQ(a.size(), 2u);
    ASSERT_EQ(b.size(), 3u);
    auto find = [](const std::vector<SfntTable>& ts, const char* t) {
        for (const SfntTable& s : ts)
            if (s.tag == tagOf(t)) return s;
        return SfntTable{0, 0, 0, 0};
    };
    // One copy of the shared table, two different cmaps
    EXPECT_EQ(find(a, "name").offset, find(b, "name").offset);
    EXPECT_NE(find(a, "cmap").offset, find(b, "cmap").offset);
    SfntTable name = find(a, "name");
    ASSERT_EQ(name.length, 51u);
    EXPECT_EQ(std::memcmp(f.data() + name.offset, "shared name table", 17), 0);
    SfntTable cmapB = find(b, "cmap");
    EXPECT_EQ(std::memcmp(f.data() + cmapB.offset, "\x00\x02" "cmap of font B!", 17), 0);
}

TEST(Woff2, EveryTruncationFails) {
    for (size_t cut = 0; cut < sizeof(kWoff2Subset); ++cut)
        EXPECT_FALSE(decodeWebFont(kWoff2Subset, cut).ok) << "cut at " << cut;
    for (size_t cut = 0; cut < sizeof(kWoff2Collection); ++cut)
        EXPECT_FALSE(decodeWebFont(kWoff2Collection, cut).ok) << "cut at " << cut;
}

TEST(Woff2, CorruptFilesFailCleanly) {
    // Every single-bit flip either fails or decodes to something; under the
    // sanitizers this checks no read or write leaves its buffer
    std::vector<uint8_t> bytes(kWoff2Subset, kWoff2Subset + sizeof(kWoff2Subset));
    for (size_t i = 0; i < bytes.size(); ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            bytes[i] ^= static_cast<uint8_t>(1 << bit);
            auto r = decodeWebFont(bytes);
            EXPECT_LE(r.sfnt.size(), Zepra::Fonts::kMaxDecodedFontSize);
            bytes[i] ^= static_cast<uint8_t>(1 << bit);
        }
    }
}

TEST(Woff2, RejectsBadHeaders) {
    std::vector<uint8_t> base(kWoff2Subset, kWoff2Subset + sizeof(kWoff2Subset));
    ASSERT_TRUE(decodeWebFont(base).ok);

    auto reserved = base;
    reserved[15] = 1;
    EXPECT_FALSE(decodeWebFont(reserved).ok);

    auto noTables = base;
    noTables[12] = noTables[13] = 0;
    EXPECT_FALSE(decodeWebFont(noTables).ok);

    auto longer = base;  // header claims more bytes than the file has
    put32(longer, 8, static_cast<uint32_t>(base.size() + 1));
    EXPECT_FALSE(decodeWebFont(longer).ok);

    auto squeezed = base;  // compressed size past the end of the file
    put32(squeezed, 20, 0x7FFFFFFF);
    EXPECT_FALSE(decodeWebFont(squeezed).ok);

    const uint8_t stub[] = {'w', 'O', 'F', '2', 0, 1, 0, 0};
    auto r = decodeWebFont(stub, sizeof(stub));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.format, WebFontFormat::Woff2);
    EXPECT_NE(r.error.find("WOFF2"), std::string::npos);
}
