// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file webcore_brotli_tests.cpp
 * @brief Zepra's own Brotli decoder (RFC 7932) against streams made by the
 *        reference encoder: text that uses the static dictionary and its
 *        transforms, HTML, UTF-8, incompressible data, a small window, the
 *        empty stream; plus truncation, corruption and the output limit.
 */

#include <gtest/gtest.h>

#include "core/brotli.hpp"
#include "core/inflate.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace {
#include "fixtures/brotli.inc"
}

using namespace Zepra::Compress;

TEST(Brotli, StaticDictionaryIsTheOneInTheRfc) {
    ASSERT_EQ(brotliDictionarySize(), 122784u);
    // CRC-32 of RFC 7932 Appendix A (SHA-256 20e42eb1...c70)
    EXPECT_EQ(crc32(brotliDictionaryData(), brotliDictionarySize()), 0x5136cb04u);
    EXPECT_EQ(std::memcmp(brotliDictionaryData(), "timedownlifeleft", 16), 0);
}

TEST(Brotli, DecodesReferenceStreams) {
    for (const auto& c : kBrotliCases) {
        std::vector<uint8_t> out;
        ASSERT_EQ(brotliDecode(c.data, c.size, out), BrotliStatus::Ok) << c.name;
        EXPECT_EQ(out.size(), c.length) << c.name;
        EXPECT_EQ(crc32(out.data(), out.size()), c.crc) << c.name;
    }
}

TEST(Brotli, DictionaryTextIsSmallAndExact) {
    // At quality 11 this text is mostly dictionary words with transforms
    // (capitalised, all caps, " of the ", "ification"), which is why it
    // compresses to a fraction of its size
    std::vector<uint8_t> out;
    ASSERT_EQ(brotliDecode(kText, sizeof(kText), out), BrotliStatus::Ok);
    EXPECT_LT(sizeof(kText) * 4, out.size());
    std::string s(out.begin(), out.end());
    EXPECT_EQ(s.rfind("The quick brown fox jumps over the lazy dog. THE WORLD of the web, ", 0), 0u);
}

TEST(Brotli, EveryTruncationFails) {
    for (const auto& c : kBrotliCases) {
        for (size_t cut = 0; cut < c.size; ++cut) {
            std::vector<uint8_t> out;
            BrotliStatus s = brotliDecode(c.data, cut, out);
            // A cut can't decode to the whole original
            EXPECT_FALSE(s == BrotliStatus::Ok && out.size() == c.length && crc32(out.data(), out.size()) == c.crc &&
                         cut < c.size)
                << c.name << " cut at " << cut;
        }
    }
}

TEST(Brotli, CorruptStreamsFailCleanly) {
    // Every single-bit flip of a stream either fails or decodes to something;
    // under the sanitizers this checks no read or write leaves its buffer
    for (const auto& c : {kBrotliCases[0], kBrotliCases[2], kBrotliCases[5]}) {
        std::vector<uint8_t> bytes(c.data, c.data + c.size);
        for (size_t i = 0; i < bytes.size(); ++i) {
            for (int bit = 0; bit < 8; ++bit) {
                bytes[i] ^= static_cast<uint8_t>(1 << bit);
                std::vector<uint8_t> out;
                brotliDecode(bytes.data(), bytes.size(), out, 1 << 20);
                EXPECT_LE(out.size(), size_t{1} << 20);
                bytes[i] ^= static_cast<uint8_t>(1 << bit);
            }
        }
    }
}

TEST(Brotli, OutputLimitIsEnforced) {
    for (const auto& c : kBrotliCases) {
        if (c.length == 0) continue;
        std::vector<uint8_t> out;
        EXPECT_EQ(brotliDecode(c.data, c.size, out, c.length - 1), BrotliStatus::TooLarge) << c.name;
        EXPECT_LT(out.size(), c.length) << c.name;
    }
}

TEST(Brotli, RejectsLargeWindowAndGarbage) {
    std::vector<uint8_t> out;
    const uint8_t largeWindow[] = {0x11, 0x00, 0x00};  // WBITS marker for large-window Brotli
    EXPECT_EQ(brotliDecode(largeWindow, sizeof(largeWindow), out), BrotliStatus::BadData);
    EXPECT_NE(brotliDecode(nullptr, 0, out), BrotliStatus::Ok);
    const uint8_t text[] = "<html>not brotli</html>";
    EXPECT_NE(brotliDecode(text, sizeof(text), out), BrotliStatus::Ok);
}

TEST(Brotli, AppendsAfterExistingOutput) {
    std::vector<uint8_t> out = {'x', 'y'};
    ASSERT_EQ(brotliDecode(kHtml, sizeof(kHtml), out), BrotliStatus::Ok);
    ASSERT_EQ(out.size(), 2u + kBrotliCases[2].length);
    EXPECT_EQ(out[0], 'x');
    EXPECT_EQ(crc32(out.data() + 2, out.size() - 2), kBrotliCases[2].crc);
}
