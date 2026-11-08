// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file webcore_font_face_rule_tests.cpp
 * @brief @font-face rules: each descriptor's parser, whole rules as sites
 *        write them, and CSSEngine::fontFaces() across sheets, @import and
 *        @media.
 */

#include <gtest/gtest.h>

#include "browser/dom.hpp"
#include "css/css_engine.hpp"
#include "css/font_face_descriptors.hpp"
#include "css/media_query.hpp"
#include "html/html_parser.hpp"

#include <memory>
#include <string>
#include <vector>

// Viewport size the layout engine reads (zepra_browser.cpp defines these)
int g_width = 1280;
int g_height = 800;

using namespace Zepra::WebCore;

namespace {

std::vector<CSSFontFaceRule*> fontRules(CSSStyleSheet* sheet) {
    std::vector<CSSFontFaceRule*> out;
    for (size_t i = 0; sheet && i < sheet->cssRules()->length(); i++)
        if (auto* r = dynamic_cast<CSSFontFaceRule*>(sheet->cssRules()->item(i))) out.push_back(r);
    return out;
}

}  // namespace

// --- descriptor parsers --------------------------------------------------

TEST(FontFaceDescriptor, Family) {
    std::string f;
    EXPECT_TRUE(parseFontFaceFamily("\"Open Sans\"", f));
    EXPECT_EQ(f, "Open Sans");
    EXPECT_TRUE(parseFontFaceFamily("'Font\\'s Name'", f));
    EXPECT_EQ(f, "Font's Name");
    EXPECT_TRUE(parseFontFaceFamily("  Roboto   Slab ", f));
    EXPECT_EQ(f, "Roboto Slab");
    EXPECT_FALSE(parseFontFaceFamily("serif", f));  // generic families can't be redefined
    EXPECT_TRUE(parseFontFaceFamily("\"serif\"", f));  // ...but a string is just a name
    EXPECT_FALSE(parseFontFaceFamily("inherit", f));
    EXPECT_FALSE(parseFontFaceFamily("A, B", f));
    EXPECT_FALSE(parseFontFaceFamily("1Font", f));
    EXPECT_FALSE(parseFontFaceFamily("", f));
}

TEST(FontFaceDescriptor, SrcEntries) {
    std::vector<FontFaceSource> s;
    ASSERT_TRUE(parseFontFaceSrc(
        "local(\"Inter Regular\"), local(Inter-Regular), url(/f/inter.woff2) format(\"woff2\"),"
        " url('/f/inter.woff') format('woff') tech(variations), url(inter.ttf)",
        s));
    ASSERT_EQ(s.size(), 5u);
    EXPECT_TRUE(s[0].isLocal);
    EXPECT_EQ(s[0].localName, "Inter Regular");
    EXPECT_EQ(s[1].localName, "Inter-Regular");
    EXPECT_FALSE(s[2].isLocal);
    EXPECT_EQ(s[2].url, "/f/inter.woff2");
    ASSERT_EQ(s[2].formats.size(), 1u);
    EXPECT_EQ(s[2].formats[0], "woff2");
    EXPECT_EQ(s[3].url, "/f/inter.woff");
    EXPECT_EQ(s[3].formats[0], "woff");
    ASSERT_EQ(s[3].techs.size(), 1u);
    EXPECT_EQ(s[3].techs[0], "variations");
    EXPECT_EQ(s[4].url, "inter.ttf");
    EXPECT_TRUE(s[4].formats.empty());
}

TEST(FontFaceDescriptor, SrcKeepsTheGoodEntries) {
    std::vector<FontFaceSource> s;
    // The old "bulletproof" pattern, a data: URL, and two broken entries
    ASSERT_TRUE(parseFontFaceSrc(
        "url('f.eot?#iefix') format('embedded-opentype'), url(data:font/woff;base64,d09GRg==) format(\"woff\"),"
        " url(), nonsense(x), url(a.ttf) junk",
        s));
    ASSERT_EQ(s.size(), 2u);
    EXPECT_EQ(s[0].url, "f.eot?#iefix");
    EXPECT_EQ(s[0].formats[0], "embedded-opentype");
    EXPECT_EQ(s[1].url, "data:font/woff;base64,d09GRg==");
    EXPECT_FALSE(parseFontFaceSrc("url()", s));
    EXPECT_FALSE(parseFontFaceSrc("local(a) format(woff)", s));
}

TEST(FontFaceDescriptor, Weight) {
    int lo = 0, hi = 0;
    EXPECT_TRUE(parseFontFaceWeight("bold", lo, hi));
    EXPECT_EQ(lo, 700);
    EXPECT_EQ(hi, 700);
    EXPECT_TRUE(parseFontFaceWeight("100 900", lo, hi));
    EXPECT_EQ(lo, 100);
    EXPECT_EQ(hi, 900);
    EXPECT_TRUE(parseFontFaceWeight("800 300", lo, hi));  // reversed ranges are swapped
    EXPECT_EQ(lo, 300);
    EXPECT_EQ(hi, 800);
    EXPECT_TRUE(parseFontFaceWeight("normal", lo, hi));
    EXPECT_EQ(lo, 400);
    EXPECT_FALSE(parseFontFaceWeight("1001", lo, hi));
    EXPECT_FALSE(parseFontFaceWeight("bolder", lo, hi));
    EXPECT_FALSE(parseFontFaceWeight("100 200 300", lo, hi));
}

TEST(FontFaceDescriptor, StyleAndStretch) {
    FontFaceStyle st = FontFaceStyle::Normal;
    EXPECT_TRUE(parseFontFaceStyle("italic", st));
    EXPECT_EQ(st, FontFaceStyle::Italic);
    EXPECT_TRUE(parseFontFaceStyle("oblique 10deg 20deg", st));
    EXPECT_EQ(st, FontFaceStyle::Oblique);
    EXPECT_FALSE(parseFontFaceStyle("oblique 100deg", st));
    EXPECT_FALSE(parseFontFaceStyle("slanted", st));

    float lo = 0, hi = 0;
    EXPECT_TRUE(parseFontFaceStretch("condensed", lo, hi));
    EXPECT_FLOAT_EQ(lo, 75.f);
    EXPECT_TRUE(parseFontFaceStretch("75% 125%", lo, hi));
    EXPECT_FLOAT_EQ(lo, 75.f);
    EXPECT_FLOAT_EQ(hi, 125.f);
    EXPECT_FALSE(parseFontFaceStretch("wide", lo, hi));
}

TEST(FontFaceDescriptor, UnicodeRange) {
    std::vector<UnicodeRange> r;
    // Google Fonts' latin subset, shortened
    ASSERT_TRUE(parseUnicodeRange("U+0000-00FF, U+0131, U+02BB-02BC, u+2000-206f, U+4??", r));
    ASSERT_EQ(r.size(), 5u);
    EXPECT_EQ(r[0].first, 0u);
    EXPECT_EQ(r[0].last, 0xFFu);
    EXPECT_EQ(r[1].first, 0x131u);
    EXPECT_EQ(r[1].last, 0x131u);
    EXPECT_EQ(r[3].first, 0x2000u);
    EXPECT_EQ(r[3].last, 0x206Fu);
    EXPECT_EQ(r[4].first, 0x400u);
    EXPECT_EQ(r[4].last, 0x4FFu);

    EXPECT_FALSE(parseUnicodeRange("U+0100-0050", r));  // reversed
    EXPECT_FALSE(parseUnicodeRange("U+110000", r));     // past Unicode
    EXPECT_FALSE(parseUnicodeRange("U+4?4", r));        // digits after a wildcard
    EXPECT_FALSE(parseUnicodeRange("0041", r));
    EXPECT_FALSE(parseUnicodeRange("U+1234567", r));

    FontFaceDescriptors d;
    ASSERT_TRUE(parseUnicodeRange("U+0-7F, U+0400-04FF", d.unicodeRanges));
    EXPECT_TRUE(d.coversCodePoint('A'));
    EXPECT_TRUE(d.coversCodePoint(0x0416));  // Cyrillic Zhe
    EXPECT_FALSE(d.coversCodePoint(0x00E9));
    d.unicodeRanges.clear();
    EXPECT_TRUE(d.coversCodePoint(0x1F600));  // no unicode-range covers everything
}

// --- whole rules ---------------------------------------------------------

TEST(FontFaceRule, ParsesARealRule) {
    CSSParser parser;
    auto sheet = parser.parse(R"(
        /* latin */
        @font-face {
          font-family: 'Roboto';
          font-style: italic;
          font-weight: 300 700;
          font-stretch: 100%;
          font-display: swap;
          src: url(https://fonts.gstatic.com/s/roboto/v47/KFO5CnqEu92Fr1Mu53ZEC9_Vu3r1gIhOszmkC3kaSTbQWt4N.woff2) format('woff2');
          unicode-range: U+0000-00FF, U+0131, U+0152-0153;
        }
        p { color: red }
    )");
    auto rules = fontRules(sheet.get());
    ASSERT_EQ(rules.size(), 1u);
    ASSERT_TRUE(rules[0]->valid());
    const auto& d = rules[0]->descriptors();
    EXPECT_EQ(d.family, "Roboto");
    EXPECT_EQ(d.style, FontFaceStyle::Italic);
    EXPECT_EQ(d.weightMin, 300);
    EXPECT_EQ(d.weightMax, 700);
    EXPECT_TRUE(d.coversWeight(400));
    EXPECT_FALSE(d.coversWeight(800));
    EXPECT_EQ(d.display, "swap");
    ASSERT_EQ(d.sources.size(), 1u);
    EXPECT_EQ(d.sources[0].url.rfind("https://fonts.gstatic.com/s/roboto/", 0), 0u);
    EXPECT_EQ(d.unicodeRanges.size(), 3u);
    EXPECT_EQ(rules[0]->fontFamily(), "'Roboto'");
    EXPECT_NE(rules[0]->cssText().find("font-display: swap;"), std::string::npos);
    // The rule after it still parses
    EXPECT_EQ(sheet->cssRules()->length(), 2u);
}

TEST(FontFaceRule, DefaultsAndInvalidRules) {
    CSSParser parser;
    auto sheet = parser.parse(R"(
        @font-face { font-family: Plain; src: url(plain.woff); font-weight: heavy; unicode-range: nope }
        @FONT-FACE { font-family: NoSrc; }
        @font-face { src: url(nofamily.woff); }
        @font-face { font-family: serif; src: url(generic.woff); }
    )");
    auto rules = fontRules(sheet.get());
    ASSERT_EQ(rules.size(), 4u);
    ASSERT_TRUE(rules[0]->valid());
    const auto& d = rules[0]->descriptors();
    EXPECT_EQ(d.weightMin, 400);  // invalid optional descriptors keep their initial values
    EXPECT_EQ(d.style, FontFaceStyle::Normal);
    EXPECT_TRUE(d.unicodeRanges.empty());
    EXPECT_EQ(d.display, "auto");
    EXPECT_FALSE(rules[1]->valid());
    EXPECT_FALSE(rules[2]->valid());
    EXPECT_FALSE(rules[3]->valid());
}

// --- CSSEngine::fontFaces() ----------------------------------------------

class FontFaceEngine : public ::testing::Test {
protected:
    void SetUp() override {
        setMediaEnvironment(MediaEnvironment{});
        HTMLParser parser;
        doc = parser.parse("<!DOCTYPE html><html><body><p>x</p></body></html>");
        css = std::make_unique<CSSEngine>();
        css->initialize(doc.get());
        css->setBaseUrl("https://site.test/blog/post.html");
    }
    void TearDown() override { setMediaEnvironment(MediaEnvironment{}); }

    std::unique_ptr<DOMDocument> doc;
    std::unique_ptr<CSSEngine> css;
};

TEST_F(FontFaceEngine, CollectsFacesWithTheirBaseUrls) {
    css->setImportFetcher([](const std::string& href, const std::string&, std::string& url, std::string& text) {
        if (href != "fonts.css") return false;
        url = "https://cdn.test/css/fonts.css";
        text = "@font-face { font-family: Imported; src: url(imp.woff) }";
        return true;
    });
    css->addStyleSheet("@import 'fonts.css'; @font-face { font-family: Inline; src: url(in.woff) }");
    css->addStyleSheet("@font-face { font-family: Linked; src: url(../f/l.woff) }", StyleOrigin::Author,
                       "https://site.test/css/main.css");
    auto faces = css->fontFaces();
    ASSERT_EQ(faces.size(), 3u);
    EXPECT_EQ(faces[0].descriptors.family, "Imported");  // imports come before the importing sheet
    EXPECT_EQ(faces[0].baseUrl, "https://cdn.test/css/fonts.css");
    EXPECT_EQ(faces[1].descriptors.family, "Inline");
    EXPECT_EQ(faces[1].baseUrl, "https://site.test/blog/post.html");
    EXPECT_EQ(faces[2].descriptors.family, "Linked");
    EXPECT_EQ(faces[2].baseUrl, "https://site.test/css/main.css");
}

TEST_F(FontFaceEngine, MediaConditionsApply) {
    css->setImportFetcher([](const std::string&, const std::string&, std::string& url, std::string& text) {
        url = "https://site.test/print.css";
        text = "@font-face { font-family: PrintOnly; src: url(p.woff) }";
        return true;
    });
    css->addStyleSheet(R"(
        @import url(print.css) print;
        @media (max-width: 600px) { @font-face { font-family: Narrow; src: url(n.woff) } }
        @font-face { font-family: Always; src: url(a.woff) }
    )");
    auto names = [&] {
        std::vector<std::string> out;
        for (const auto& f : css->fontFaces()) out.push_back(f.descriptors.family);
        return out;
    };
    EXPECT_EQ(names(), std::vector<std::string>{"Always"});

    MediaEnvironment narrow;
    narrow.width = 500;
    setMediaEnvironment(narrow);
    EXPECT_EQ(names(), (std::vector<std::string>{"Narrow", "Always"}));
}
