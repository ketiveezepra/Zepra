// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file webcore_web_font_decoder_tests.cpp
 * @brief Zepra's own inflate (raw DEFLATE, zlib, gzip) against streams made
 *        by the reference zlib, and WOFF 1.0 unpacking into an sfnt that
 *        FreeType loads.
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
}

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
    EXPECT_FALSE(isSupportedFontFormatHint("woff2"));  // until the Brotli decoder lands
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

TEST(WebFontDecoder, Woff2IsReportedNotGuessed) {
    const uint8_t woff2[] = {'w', 'O', 'F', '2', 0, 1, 0, 0};
    auto r = decodeWebFont(woff2, sizeof(woff2));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.format, WebFontFormat::Woff2);
    EXPECT_NE(r.error.find("WOFF2"), std::string::npos);
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
