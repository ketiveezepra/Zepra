// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file webcore_web_font_loader_tests.cpp
 * @brief Web fonts end to end: @font-face rules from the CSS engine are
 *        downloaded through a fake fetcher (src fallback, format hints,
 *        data: and local(), unicode-range), decoded and registered, and
 *        FontManager::match() then picks them over installed fonts by family,
 *        weight range and style.
 *
 * Uses the DejaVu fonts as stand-in web fonts; skipped where they aren't
 * installed.
 */

#include <gtest/gtest.h>

#include "browser/dom.hpp"
#include "css/css_engine.hpp"
#include "html/html_parser.hpp"
#include "rendering/font_manager.hpp"
#include "rendering/web_font_loader.hpp"

#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Viewport size the layout engine reads (zepra_browser.cpp defines these)
int g_width = 1280;
int g_height = 800;

using namespace Zepra::WebCore;
using Zepra::Fonts::FontManager;
using Zepra::Fonts::RegisteredFont;
using Zepra::Fonts::TextCoverage;
using Zepra::Fonts::WebFontFaceInfo;
using Zepra::Fonts::WebFontResponse;

namespace {

#include "fixtures/woff2.inc"

const char* kSans = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
const char* kSerif = "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf";
const char* kMono = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";

std::vector<uint8_t> readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
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

/// An sfnt re-wrapped as WOFF with its tables stored
std::vector<uint8_t> asWoff(const std::vector<uint8_t>& sfnt) {
    uint16_t n = static_cast<uint16_t>((sfnt[4] << 8) | sfnt[5]);
    std::vector<uint8_t> woff(44 + 20 * size_t{n}, 0);
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
    put32(woff, 0, 0x774F4646);
    put32(woff, 4, be32(sfnt.data()));
    put32(woff, 8, static_cast<uint32_t>(woff.size()));
    woff[12] = uint8_t(n >> 8);
    woff[13] = uint8_t(n);
    return woff;
}

std::string base64(const std::vector<uint8_t>& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = (uint32_t{in[i]} << 16) | (uint32_t{in[i + 1]} << 8) | in[i + 2];
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += t[v & 63];
    }
    if (i < in.size()) {
        uint32_t v = uint32_t{in[i]} << 16;
        if (i + 1 < in.size()) v |= uint32_t{in[i + 1]} << 8;
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += i + 1 < in.size() ? t[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

bool haveDejaVu() {
    return !readFile(kSans).empty() && !readFile(kSerif).empty() && !readFile(kMono).empty();
}

std::string resolve(const std::string& base, const std::string& href) {
    if (href.find("://") != std::string::npos) return href;
    return base.substr(0, base.rfind('/') + 1) + href;
}

class WebFonts : public ::testing::Test {
protected:
    void SetUp() override {
        if (!haveDejaVu()) GTEST_SKIP() << "the DejaVu fonts are not installed";
        FontManager::instance().clearWebFonts();
    }
    void TearDown() override { FontManager::instance().clearWebFonts(); }

    std::vector<CSSEngine::WebFontFace> facesFrom(const std::string& css) {
        HTMLParser parser;
        doc_ = parser.parse("<!DOCTYPE html><html><body><p>x</p></body></html>");
        css_ = std::make_unique<CSSEngine>();
        css_->initialize(doc_.get());
        css_->setBaseUrl("https://site.test/page.html");
        css_->addStyleSheet(css, StyleOrigin::Author, "https://site.test/css/site.css");
        return css_->fontFaces();
    }

    std::unique_ptr<DOMDocument> doc_;
    std::unique_ptr<CSSEngine> css_;
};

}  // namespace

TEST(TextCoverageTest, CollectsCodePointsAndChecksRanges) {
    TextCoverage text;
    text.addUtf8("Caf\xC3\xA9 \xD0\x96 \xF0\x9F\x98\x80 \xFF");  // "Café Ж 😀" and a stray byte
    EXPECT_TRUE(text.contains('C'));
    EXPECT_TRUE(text.contains(0xE9));
    EXPECT_TRUE(text.contains(0x416));
    EXPECT_TRUE(text.contains(0x1F600));
    EXPECT_FALSE(text.contains(' '));  // whitespace never picks a font
    EXPECT_EQ(text.size(), 6u);

    FontFaceDescriptors greek;
    greek.unicodeRanges = {{0x370, 0x3FF}};
    EXPECT_FALSE(text.wants(greek));
    FontFaceDescriptors cyrillic;
    cyrillic.unicodeRanges = {{0x400, 0x4FF}};
    EXPECT_TRUE(text.wants(cyrillic));
    EXPECT_TRUE(text.wants(FontFaceDescriptors{}));  // no unicode-range: always
    EXPECT_TRUE(TextCoverage{}.wants(greek));        // no text yet: load everything
}

TEST_F(WebFonts, RegisteredFacesWinByFamilyWeightAndStyle) {
    auto& fm = FontManager::instance();
    uint64_t gen = fm.webFontGeneration();
    ASSERT_TRUE(fm.registerWebFont({"Brand", 100, 400, false}, readFile(kSans)));
    ASSERT_TRUE(fm.registerWebFont({"Brand", 700, 700, false}, readFile(kSerif)));
    ASSERT_TRUE(fm.registerWebFont({"Brand", 400, 400, true}, readFile(kMono)));
    EXPECT_GT(fm.webFontGeneration(), gen);

    auto pick = [&](int weight, bool italic) -> std::vector<uint8_t> {
        const RegisteredFont* f = fm.match("Brand, sans-serif", weight, italic);
        return f && !f->isSystem ? f->data : std::vector<uint8_t>{};
    };
    EXPECT_EQ(pick(300, false), readFile(kSans));   // inside 100-400
    EXPECT_EQ(pick(800, false), readFile(kSerif));  // nearest to 700
    EXPECT_EQ(pick(550, false), readFile(kSerif));  // 150 from 400, 150 from 700: the later face wins the tie
    EXPECT_EQ(pick(400, true), readFile(kMono));    // style first
    EXPECT_EQ(pick(700, true), readFile(kMono));

    const RegisteredFont* f = fm.match("'brand'", 400, false);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->displayName, "Brand");
    EXPECT_EQ(static_cast<int>(f->weight), 100);  // the bottom of its range

    // A web font named like an installed one replaces it for the page
    const RegisteredFont* installed = fm.match("DejaVu Sans", 400, false);
    ASSERT_NE(installed, nullptr);
    EXPECT_TRUE(installed->isSystem);
    ASSERT_TRUE(fm.registerWebFont({"DejaVu Sans", 400, 400, false}, readFile(kSerif)));
    const RegisteredFont* replaced = fm.match("DejaVu Sans", 400, false);
    ASSERT_NE(replaced, nullptr);
    EXPECT_FALSE(replaced->isSystem);

    gen = fm.webFontGeneration();
    fm.clearWebFonts();
    EXPECT_GT(fm.webFontGeneration(), gen);
    EXPECT_EQ(fm.match("Brand", 400, false), nullptr);
    const RegisteredFont* back = fm.match("DejaVu Sans", 400, false);
    ASSERT_NE(back, nullptr);
    EXPECT_TRUE(back->isSystem);
}

TEST_F(WebFonts, LoaderFollowsSrcFallbacksAndRanges) {
    const std::string dataUrl = "data:font/ttf;base64," + base64(readFile(kMono));
    auto faces = facesFrom(
        "@font-face { font-family: Fallback;"
        "  src: url(f/next.woff2) format('woff2'), url(f/broken.woff) format('woff'), url(f/good.woff) format('woff'); }"
        "@font-face { font-family: CyrillicOnly; src: url(f/cyr.woff); unicode-range: U+0400-04FF; }"
        "@font-face { font-family: Local; src: local('DejaVu Serif'), url(f/never.woff); }"
        "@font-face { font-family: Inline; font-weight: 700; src: url(" + dataUrl + ") format('truetype'); }"
        "@font-face { font-family: Missing; src: url(https://cdn.test/404.ttf), url(f/also-missing.ttf); }"
        "@font-face { font-family: Shared; src: url(f/good.woff); }"
        "@font-face { font-family: Old; src: url(f/old.eot) format('embedded-opentype'); }"
        "@font-face { font-family: Packed; src: url(f/packed.woff2) format('woff2'); }");
    ASSERT_EQ(faces.size(), 8u);

    std::map<std::string, std::vector<uint8_t>> files = {
        {"https://site.test/css/f/broken.woff", {'w', 'O', 'F', 'F', 0, 0, 0, 0}},
        {"https://site.test/css/f/good.woff", asWoff(readFile(kSans))},
        {"https://site.test/css/f/cyr.woff", asWoff(readFile(kSerif))},
        {"https://site.test/css/f/packed.woff2", std::vector<uint8_t>(kWoff2Subset, kWoff2Subset + sizeof(kWoff2Subset))},
    };
    std::vector<std::vector<std::string>> rounds;
    auto fetch = [&](const std::vector<std::string>& urls) {
        rounds.push_back(urls);
        std::vector<WebFontResponse> out;
        for (const auto& u : urls) {
            WebFontResponse r;
            auto it = files.find(u);
            if (it != files.end()) {
                r.ok = true;
                r.data = it->second;
            } else {
                r.error = "HTTP 404";
            }
            out.push_back(std::move(r));
        }
        return out;
    };

    TextCoverage text;
    text.addUtf8("Hello, world");
    auto result = Zepra::Fonts::loadWebFonts(faces, &text, resolve, fetch);

    EXPECT_EQ(result.loaded, 5u);   // Fallback, Local, Inline, Shared, Packed
    EXPECT_EQ(result.failed, 2u);   // Missing, Old
    EXPECT_EQ(result.skipped, 1u);  // CyrillicOnly: the text has no Cyrillic
    EXPECT_EQ(result.messages.size(), 2u);

    // Round 1 fetches every face's first usable source at once; the .eot
    // and the data: URL never reach the network. Fallback walks next.woff2
    // (404), broken.woff, then good.woff, which round 1 already fetched for
    // Shared, so it is not downloaded twice
    ASSERT_EQ(rounds.size(), 2u);
    EXPECT_EQ(rounds[0], (std::vector<std::string>{"https://site.test/css/f/next.woff2",
                                                   "https://cdn.test/404.ttf",
                                                   "https://site.test/css/f/good.woff",
                                                   "https://site.test/css/f/packed.woff2"}));
    EXPECT_EQ(rounds[1], (std::vector<std::string>{"https://site.test/css/f/broken.woff",
                                                   "https://site.test/css/f/also-missing.ttf"}));

    auto& fm = FontManager::instance();
    const RegisteredFont* f = fm.match("Fallback", 400, false);
    ASSERT_NE(f, nullptr);
    EXPECT_FALSE(f->isSystem);
    EXPECT_EQ(f->data, readFile(kSans));  // the WOFF, unpacked

    const RegisteredFont* local = fm.match("Local", 400, false);
    ASSERT_NE(local, nullptr);
    EXPECT_FALSE(local->isSystem);
    EXPECT_NE(local->path.find("DejaVuSerif"), std::string::npos);

    const RegisteredFont* inl = fm.match("Inline", 700, false);
    ASSERT_NE(inl, nullptr);
    EXPECT_EQ(inl->data, readFile(kMono));
    EXPECT_EQ(static_cast<int>(inl->weight), 700);

    EXPECT_EQ(fm.match("Missing", 400, false), nullptr);
    EXPECT_EQ(fm.match("CyrillicOnly", 400, false), nullptr);
    ASSERT_NE(fm.match("Shared", 400, false), nullptr);

    const RegisteredFont* packed = fm.match("Packed", 400, false);  // WOFF2, unpacked
    ASSERT_NE(packed, nullptr);
    EXPECT_FALSE(packed->isSystem);
    ASSERT_GE(packed->data.size(), 4u);
    EXPECT_EQ(std::vector<uint8_t>(packed->data.begin(), packed->data.begin() + 4),
              (std::vector<uint8_t>{0, 1, 0, 0}));
}

TEST_F(WebFonts, NoTextLoadsEveryFace) {
    auto faces = facesFrom("@font-face { font-family: Cyr; src: url(c.ttf); unicode-range: U+0400-04FF; }");
    auto fetch = [&](const std::vector<std::string>& urls) {
        std::vector<WebFontResponse> out(urls.size());
        for (auto& r : out) {
            r.ok = true;
            r.data = readFile(kSerif);
        }
        return out;
    };
    auto result = Zepra::Fonts::loadWebFonts(faces, nullptr, resolve, fetch);
    EXPECT_EQ(result.loaded, 1u);
    EXPECT_NE(FontManager::instance().match("Cyr", 400, false), nullptr);
}
