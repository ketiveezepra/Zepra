// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file parser_robustness_tests.cpp
 * @brief Hostile input for the HTML, CSS and JavaScript parsers: each case once
 *        hung the UI thread or overflowed the stack and killed the browser.
 */

#include <gtest/gtest.h>

#include "browser/dom.hpp"
#include "css/css_computed_style.hpp"
#include "css/css_engine.hpp"
#include "css/ua_stylesheet.h"
#include "html/html_parser.hpp"
#include "frontend/parser.hpp"
#include "frontend/source_code.hpp"

#include <memory>
#include <string>

// Viewport size the layout engine reads (zepra_browser.cpp defines these)
int g_width = 1280;
int g_height = 800;

using namespace Zepra::WebCore;

namespace {

std::string repeat(const std::string& s, int n) {
    std::string out;
    out.reserve(s.size() * n);
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

// Depth of the deepest node, counted without recursion
size_t treeDepth(const DOMNode* root) {
    size_t deepest = 0;
    std::vector<std::pair<const DOMNode*, size_t>> stack{{root, 0}};
    while (!stack.empty()) {
        auto [node, depth] = stack.back();
        stack.pop_back();
        deepest = std::max(deepest, depth);
        for (const auto& child : node->childNodes()) stack.emplace_back(child.get(), depth + 1);
    }
    return deepest;
}

// ---------------------------------------------------------------------------
// CSS
// ---------------------------------------------------------------------------

// A "{ ... }" with no selector made parseRule return without consuming anything,
// and the rule loop spun forever on the UI thread
TEST(CssParserRobustness, BlockWithoutSelectorIsSkipped) {
    HTMLParser parser;
    auto doc = parser.parse("<html><body><p id='p'>x</p></body></html>");
    CSSEngine css;
    css.initialize(doc.get());
    css.addStyleSheet("{ color: red } p { color: rgb(1, 2, 3) } } {{ } p { width: 7px }", StyleOrigin::Author);
    const CSSComputedStyle* style = css.getComputedStyle(doc->getElementById("p"));
    ASSERT_NE(style, nullptr);
    EXPECT_EQ(style->color.r, 1);
    EXPECT_EQ(style->color.g, 2);
    EXPECT_EQ(style->color.b, 3);
}

TEST(CssParserRobustness, UnterminatedAndStrayBracesEnd) {
    for (const char* sheet : {"{", "}", "{{{{", "}}}}", "@media {", "@supports (x) { p {", "p { color:",
                              " { width: 20px; animation: x<!DOCTYPE -->\n<div class=\"s\">"}) {
        HTMLParser parser;
        auto doc = parser.parse("<p>x</p>");
        CSSEngine css;
        css.initialize(doc.get());
        css.addStyleSheet(sheet, StyleOrigin::Author);  // returns: no hang
    }
    SUCCEED();
}

// A selector starting with a combinator made the selector tokenizer stop without
// moving, and matching it never ended
TEST(CssParserRobustness, SelectorWithALeadingCombinatorMatchesNothing) {
    HTMLParser parser;
    auto doc = parser.parse("<html><body><div><p id='p'>x</p></div></body></html>");
    CSSEngine css;
    css.initialize(doc.get());
    css.addStyleSheet("> p { color: rgb(9, 9, 9) } div > > p { color: rgb(8, 8, 8) } "
                      "+ p { color: rgb(7, 7, 7) } div > p { width: 3px }",
                      StyleOrigin::Author);
    const CSSComputedStyle* style = css.getComputedStyle(doc->getElementById("p"));
    ASSERT_NE(style, nullptr);
    EXPECT_NE(style->color.r, 9);
    EXPECT_NE(style->color.r, 8);
    EXPECT_NE(style->color.r, 7);
}

// std::stof threw out of rgb()/hsl() on input like "." and nothing caught it: one
// bad color aborted the browser. A hue of 1e30 looped forever.
TEST(CssParserRobustness, MalformedColorsDoNotThrowOrHang) {
    for (const char* text : {"rgb(.)", "rgba(1, 2, 3, .)", "rgb(1e99, 0, 0)", "rgb(-5, 300, 1e-50)",
                             "rgba(0,0,0,1e40)", "hsl(1e30, 50%, 50%)", "hsl(-1e30, 50%, 50%)",
                             "hsla(.,.,.,.)", "hsl(nan, 1%, 1%)", "rgb(", "hsl("}) {
        EXPECT_NO_THROW(CSSColor::parse(text)) << text;
    }
    CSSColor c = CSSColor::parse("rgb(1e99, -5, 300)");
    EXPECT_EQ(c.r, 0) << "a non-finite channel reads as 0";
    EXPECT_EQ(c.g, 0);
    EXPECT_EQ(c.b, 255);
    CSSColor red = CSSColor::parse("hsl(720, 100%, 50%)");
    EXPECT_EQ(red.r, 255);
    EXPECT_EQ(red.g, 0);
    EXPECT_EQ(red.b, 0);
}

// ---------------------------------------------------------------------------
// HTML
// ---------------------------------------------------------------------------

// 100,000 nested tags overflowed the stack in every recursive walk (serializing,
// styling, freeing); the parser now stops nesting at 512 levels like Chromium
TEST(HtmlParserRobustness, NestingIsCappedSoTreeWalksCannotOverflow) {
    HTMLParser parser;
    auto doc = parser.parse("<html><body>" + repeat("<b>", 50000) + "deep" + "</body></html>");
    ASSERT_NE(doc, nullptr);
    EXPECT_LE(treeDepth(doc.get()), 520u);
    ASSERT_NE(doc->body(), nullptr);
    std::string html = doc->body()->outerHTML();
    EXPECT_NE(html.find("deep"), std::string::npos) << "the content is kept, only flattened";

    CSSEngine css;
    css.initialize(doc.get());
    css.addStyleSheet(ZepraUAStylesheet::getStylesheet(), StyleOrigin::UserAgent);
    EXPECT_NE(css.getComputedStyle(doc->body()), nullptr);
    doc.reset();  // freeing recurses too
}

TEST(HtmlParserRobustness, ShallowNestingIsUnchanged) {
    HTMLParser parser;
    auto doc = parser.parse("<html><body>" + repeat("<span>", 100) + "x" + repeat("</span>", 100) + "</body></html>");
    ASSERT_NE(doc, nullptr);
    // html > body > 100 spans > text
    EXPECT_EQ(treeDepth(doc.get()), 103u);
}

// "&name" that isn't a character reference stays in the attribute it was in.
// It used to leave the attribute and land in the page text, so Google Fonts
// links ("?family=A&family=B") lost every family after the first.
TEST(HtmlParserRobustness, AmpersandsInAttributesThatAreNotReferencesStay) {
    HTMLParser parser;
    auto doc = parser.parse(
        "<html><body>"
        "<a id='q' href='/css2?family=Lobster&family=Roboto+Slab:wght@700&display=swap'>x</a>"
        "<a id='semi' href='/x?a=1&bogus;b=2'>y</a>"
        "<a id='unq' href=/x?a=1&zzz=2>z</a>"
        "<a id='real' title='Fish &amp; chips &lt;3 &#x41;&#66;'>w</a>"
        "<p id='text'>Tom &zzz; Jerry &amp; co</p>"
        "</body></html>");
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->getElementById("q")->getAttribute("href"),
              "/css2?family=Lobster&family=Roboto+Slab:wght@700&display=swap");
    EXPECT_EQ(doc->getElementById("semi")->getAttribute("href"), "/x?a=1&bogus;b=2");
    EXPECT_EQ(doc->getElementById("unq")->getAttribute("href"), "/x?a=1&zzz=2");
    EXPECT_EQ(doc->getElementById("real")->getAttribute("title"), "Fish & chips <3 AB");
    EXPECT_EQ(doc->getElementById("text")->textContent(), "Tom &zzz; Jerry & co");
    // Nothing leaked out of the attributes into the page text
    EXPECT_EQ(doc->getElementById("q")->textContent(), "x");
}

// ---------------------------------------------------------------------------
// JavaScript
// ---------------------------------------------------------------------------

bool parses(const std::string& code) {
    auto source = Zepra::Frontend::SourceCode::fromString(code);
    Zepra::Frontend::Parser parser(source.get());
    auto program = parser.parseProgram();
    return program && !parser.hasErrors();
}

TEST(JsParserRobustness, DeepNestingIsASyntaxErrorNotACrash) {
    EXPECT_FALSE(parses("x = " + repeat("[", 100000)));
    EXPECT_FALSE(parses("x = " + repeat("(", 100000) + "1" + repeat(")", 100000)));
    EXPECT_FALSE(parses("x = " + repeat("!", 100000) + "1"));
    EXPECT_FALSE(parses(repeat("{", 100000)));
    EXPECT_FALSE(parses("x = " + repeat("a = ", 100000) + "1"));
}

TEST(JsParserRobustness, RealisticNestingStillParses) {
    const int n = 150;
    EXPECT_TRUE(parses("x = " + repeat("[", n) + repeat("]", n) + ";"));
    EXPECT_TRUE(parses("x = " + repeat("(", n) + "1" + repeat(")", n) + ";"));
    EXPECT_TRUE(parses(repeat("{", n) + repeat("}", n)));
    EXPECT_TRUE(parses(repeat("function f() {", 60) + "return 1;" + repeat("}", 60)));
}

}  // namespace
