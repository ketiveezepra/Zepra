// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file webcore_css_import_tests.cpp
 * @brief CSS @import: parsing the rule, loading imported sheets through the
 *        engine's fetcher (relative URLs, nesting, cycles, media queries) and
 *        their place in the cascade.
 */

#include <gtest/gtest.h>

#include "browser/dom.hpp"
#include "css/css_engine.hpp"
#include "css/media_query.hpp"
#include "css/ua_stylesheet.h"
#include "html/html_parser.hpp"

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

// Viewport size the layout engine reads (zepra_browser.cpp defines these)
int g_width = 1280;
int g_height = 800;

using namespace Zepra::WebCore;

namespace {

CSSImportRule* firstImport(CSSStyleSheet* sheet) {
    for (size_t i = 0; sheet && i < sheet->cssRules()->length(); i++) {
        if (auto* r = dynamic_cast<CSSImportRule*>(sheet->cssRules()->item(i))) return r;
    }
    return nullptr;
}

size_t importCount(CSSStyleSheet* sheet) {
    size_t n = 0;
    for (size_t i = 0; sheet && i < sheet->cssRules()->length(); i++) {
        if (dynamic_cast<CSSImportRule*>(sheet->cssRules()->item(i))) n++;
    }
    return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

TEST(CSSImportParse, UrlForms) {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"@import url(\"a.css\");", "a.css"},
        {"@import url('b.css');", "b.css"},
        {"@import url( c.css );", "c.css"},
        {"@import \"d.css\";", "d.css"},
        {"@import 'e.css'", "e.css"},  // the last statement may omit ";"
        {"@IMPORT URL(f.css);", "f.css"},
    };
    for (const auto& [css, href] : cases) {
        CSSParser parser;
        auto sheet = parser.parse(css);
        CSSImportRule* rule = firstImport(sheet.get());
        ASSERT_NE(rule, nullptr) << css;
        EXPECT_EQ(rule->href(), href) << css;
        EXPECT_EQ(rule->mediaText(), "") << css;
    }
}

TEST(CSSImportParse, LayerSupportsAndMedia) {
    CSSParser parser;
    auto sheet = parser.parse("@import url(a.css) layer(base) supports(display: grid) screen and (min-width: 600px);");
    CSSImportRule* rule = firstImport(sheet.get());
    ASSERT_NE(rule, nullptr);
    EXPECT_EQ(rule->href(), "a.css");
    EXPECT_EQ(rule->supportsCondition(), "display: grid");
    EXPECT_EQ(rule->mediaText(), "screen and (min-width: 600px)");

    sheet = parser.parse("@import \"b.css\" layer print;");
    rule = firstImport(sheet.get());
    ASSERT_NE(rule, nullptr);
    EXPECT_EQ(rule->mediaText(), "print");
}

TEST(CSSImportParse, OnlyAtTheStartOfTheSheet) {
    CSSParser parser;
    auto sheet = parser.parse("@charset \"utf-8\"; @layer a, b; @import 'a.css'; @import 'b.css';"
                              "p { color: red } @import 'late.css'; @media print { @import 'x.css'; }");
    EXPECT_EQ(importCount(sheet.get()), 2u);
    sheet = parser.parse("@media screen { p { color: red } } @import 'late.css';");
    EXPECT_EQ(importCount(sheet.get()), 0u);
    // A sheet parsed later starts over
    sheet = parser.parse("@import 'a.css'; p { color: red }");
    EXPECT_EQ(importCount(sheet.get()), 1u);
}

TEST(CSSImportParse, InvalidPreludesAreDropped) {
    CSSParser parser;
    auto sheet = parser.parse("@import ; @import foo; p { color: green }");
    EXPECT_EQ(importCount(sheet.get()), 0u);
    // The style rule after the invalid statements still parses
    bool sawStyle = false;
    for (size_t i = 0; i < sheet->cssRules()->length(); i++) {
        sawStyle = sawStyle || dynamic_cast<CSSStyleRule*>(sheet->cssRules()->item(i)) != nullptr;
    }
    EXPECT_TRUE(sawStyle);
}

// ---------------------------------------------------------------------------
// Loading and the cascade
// ---------------------------------------------------------------------------

namespace {

struct RGB {
    int r, g, b;
    bool operator==(const RGB& o) const { return std::tie(r, g, b) == std::tie(o.r, o.g, o.b); }
};
std::ostream& operator<<(std::ostream& os, const RGB& c) {
    return os << "rgb(" << c.r << "," << c.g << "," << c.b << ")";
}
const RGB kRed{255, 0, 0}, kGreen{0, 128, 0}, kBlue{0, 0, 255};

class CSSImportTest : public ::testing::Test {
protected:
    std::map<std::string, std::string> files;   // URL -> stylesheet
    std::vector<std::string> fetched;           // URLs in fetch order
    std::unique_ptr<DOMDocument> doc;
    std::unique_ptr<CSSEngine> css;

    void SetUp() override {
        MediaEnvironment env;
        env.width = 1000;
        env.height = 700;
        setMediaEnvironment(env);
    }

    /// Resolves like a browser for the simple cases used here: absolute URLs
    /// as they are, "/x" against the origin, "x" against the base's directory
    static std::string resolve(const std::string& base, const std::string& href) {
        if (href.find("://") != std::string::npos) return href;
        size_t originEnd = base.find('/', base.find("://") + 3);
        if (!href.empty() && href[0] == '/') return base.substr(0, originEnd) + href;
        std::string url = base.substr(0, base.rfind('/') + 1) + href;
        for (size_t up; (up = url.find("/../")) != std::string::npos;) {
            size_t parent = url.rfind('/', up - 1);
            url.erase(parent, up + 3 - parent);
        }
        return url;
    }

    void load(const std::string& sheet, const std::string& href = "",
              const std::string& body = "<p id='p'>x</p><div id='d'>y</div>") {
        HTMLParser parser;
        doc = parser.parse("<!DOCTYPE html><html><body>" + body + "</body></html>");
        ASSERT_NE(doc, nullptr);
        css = std::make_unique<CSSEngine>();
        css->initialize(doc.get());
        css->setBaseUrl("https://site.test/blog/post.html");
        css->setImportFetcher([this](const std::string& h, const std::string& base, std::string& url,
                                     std::string& text) {
            url = resolve(base, h);
            fetched.push_back(url);
            auto it = files.find(url);
            if (it == files.end()) return false;
            text = it->second;
            return true;
        });
        css->addStyleSheet(ZepraUAStylesheet::getStylesheet(), StyleOrigin::UserAgent);
        css->addStyleSheet(sheet, StyleOrigin::Author, href);
        css->computeStyles();
    }
    RGB color(const std::string& id) {
        const CSSComputedStyle* s = css->getComputedStyle(doc->getElementById(id));
        return s ? RGB{s->color.r, s->color.g, s->color.b} : RGB{-1, -1, -1};
    }
};

}  // namespace

TEST_F(CSSImportTest, ImportedRulesApply) {
    files["https://site.test/blog/theme.css"] = "p { color: green; }";
    load("@import 'theme.css'; div { color: blue; }");
    EXPECT_EQ(color("p"), kGreen);
    EXPECT_EQ(color("d"), kBlue);
    EXPECT_EQ(fetched, std::vector<std::string>{"https://site.test/blog/theme.css"});
}

TEST_F(CSSImportTest, ImportedSheetsComeFirstInTheCascade) {
    // The importing sheet's own rules win over equally specific imported ones
    files["https://site.test/a.css"] = "p { color: red; } div { color: green; }";
    load("@import url(/a.css); p { color: green; }");
    EXPECT_EQ(color("p"), kGreen);
    EXPECT_EQ(color("d"), kGreen);
}

TEST_F(CSSImportTest, NestedImportsResolveAgainstTheirOwnSheet) {
    files["https://cdn.test/css/main.css"] = "@import 'parts/text.css'; div { color: blue; }";
    files["https://cdn.test/css/parts/text.css"] = "@import '../base.css'; p { color: green; }";
    files["https://cdn.test/css/base.css"] = "p { color: red; } div { color: red; }";
    load("@import url(https://cdn.test/css/main.css);");
    EXPECT_EQ(color("p"), kGreen);
    EXPECT_EQ(color("d"), kBlue);
    EXPECT_EQ(fetched.size(), 3u);
    // Import order is preserved: base, then text, then main
    std::vector<std::string> order;
    for (const auto& entry : css->styleSheets()) {
        if (!entry.sheet->href().empty()) order.push_back(entry.sheet->href());
    }
    EXPECT_EQ(order, (std::vector<std::string>{"https://cdn.test/css/base.css",
                                               "https://cdn.test/css/parts/text.css",
                                               "https://cdn.test/css/main.css"}));
}

TEST_F(CSSImportTest, LinkSheetsResolveAgainstTheirOwnUrl) {
    files["https://site.test/wp-content/themes/t/blocks.css"] = "p { color: green; }";
    load("@import 'blocks.css';", "https://site.test/wp-content/themes/t/style.css");
    EXPECT_EQ(color("p"), kGreen);
}

TEST_F(CSSImportTest, CyclesStop) {
    files["https://site.test/blog/a.css"] = "@import 'b.css'; p { color: green; }";
    files["https://site.test/blog/b.css"] = "@import 'a.css'; div { color: blue; }";
    load("@import 'a.css';");
    EXPECT_EQ(color("p"), kGreen);
    EXPECT_EQ(color("d"), kBlue);
    EXPECT_LE(fetched.size(), 3u);
}

TEST_F(CSSImportTest, MediaQueriesOfTheImportApply) {
    files["https://site.test/blog/wide.css"] = "p { color: green; } @media print { div { color: red; } }";
    files["https://site.test/blog/print.css"] = "div { color: red; }";
    load("@import 'wide.css' (min-width: 600px); @import 'print.css' print; div { color: blue; }");
    EXPECT_EQ(color("p"), kGreen);
    EXPECT_EQ(color("d"), kBlue);
    MediaEnvironment env;
    env.width = 400;
    env.height = 700;
    setMediaEnvironment(env);
    css->computeStyles();
    EXPECT_NE(color("p"), kGreen);
}

TEST_F(CSSImportTest, MissingSheetsAndNoFetcher) {
    load("@import 'missing.css'; p { color: green; }");
    EXPECT_EQ(color("p"), kGreen);

    // Without a fetcher @import is ignored
    HTMLParser parser;
    doc = parser.parse("<!DOCTYPE html><html><body><p id='p'>x</p></body></html>");
    css = std::make_unique<CSSEngine>();
    css->initialize(doc.get());
    css->addStyleSheet("@import 'x.css'; p { color: green; }", StyleOrigin::Author);
    css->computeStyles();
    EXPECT_EQ(color("p"), kGreen);
}
