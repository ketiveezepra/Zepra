// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file webcore_media_query_tests.cpp
 * @brief Media queries: parsing and matching a media query list, `@media`
 *        rules in the cascade as the viewport changes, and window.matchMedia()
 *        with its `change` events.
 */

#include <gtest/gtest.h>

#include "browser/dom.hpp"
#include "css/css_engine.hpp"
#include "css/media_query.hpp"
#include "css/ua_stylesheet.h"
#include "html/html_parser.hpp"
#include "scripting/script_context.hpp"

#include <memory>
#include <string>
#include <tuple>

// Viewport size the layout engine reads (zepra_browser.cpp defines these)
int g_width = 1280;
int g_height = 800;

using namespace Zepra::WebCore;

namespace {

MediaEnvironment env(double w, double h) {
    MediaEnvironment e;
    e.width = w;
    e.height = h;
    return e;
}

bool matches(const std::string& query, const MediaEnvironment& e) {
    return MediaQueryList::parse(query)->matches(e);
}

/// Restores the default environment after each test
class MediaQueryTest : public ::testing::Test {
protected:
    void SetUp() override { setMediaEnvironment(env(1280, 800)); }
    void TearDown() override { setMediaEnvironment(env(1280, 800)); }
};

}  // namespace

TEST_F(MediaQueryTest, MediaTypes) {
    MediaEnvironment e = env(1000, 700);
    EXPECT_TRUE(matches("", e));
    EXPECT_TRUE(matches("all", e));
    EXPECT_TRUE(matches("screen", e));
    EXPECT_TRUE(matches("only screen", e));
    EXPECT_TRUE(matches("SCREEN", e));
    EXPECT_FALSE(matches("print", e));
    EXPECT_TRUE(matches("not print", e));
    EXPECT_FALSE(matches("not screen", e));
    EXPECT_FALSE(matches("speech", e));
    EXPECT_FALSE(matches("tv", e));
    EXPECT_TRUE(matches("print, screen", e));
}

TEST_F(MediaQueryTest, MinMaxWidthAndHeight) {
    MediaEnvironment e = env(1000, 700);
    EXPECT_TRUE(matches("(min-width: 600px)", e));
    EXPECT_TRUE(matches("(min-width: 1000px)", e));
    EXPECT_FALSE(matches("(min-width: 1001px)", e));
    EXPECT_TRUE(matches("(max-width: 1000px)", e));
    EXPECT_FALSE(matches("(max-width: 999.98px)", e));
    EXPECT_TRUE(matches("screen and (min-width: 768px) and (max-width: 1024px)", e));
    EXPECT_FALSE(matches("screen and (max-width: 767px)", e));
    EXPECT_TRUE(matches("(width: 1000px)", e));
    EXPECT_TRUE(matches("(min-height: 700px)", e));
    EXPECT_FALSE(matches("(min-height: 701px)", e));
    EXPECT_TRUE(matches("(width)", e));
    EXPECT_TRUE(matches("not all and (max-width: 600px)", e));
    EXPECT_FALSE(matches("not all and (min-width: 600px)", e));
}

TEST_F(MediaQueryTest, LengthUnits) {
    MediaEnvironment e = env(1000, 700);
    EXPECT_TRUE(matches("(min-width: 62.5em)", e));   // 1000px at 16px
    EXPECT_FALSE(matches("(min-width: 62.6em)", e));
    EXPECT_TRUE(matches("(max-width: 62.5rem)", e));
    EXPECT_TRUE(matches("(min-width: 10in)", e));     // 960px
    EXPECT_FALSE(matches("(min-width: 30cm)", e));    // 1133px
    EXPECT_TRUE(matches("(min-width: 0)", e));
    EXPECT_FALSE(matches("(min-width: 600)", e));     // a length needs a unit
    EXPECT_TRUE(matches("(max-width: 100vw)", e));
}

TEST_F(MediaQueryTest, RangeSyntax) {
    MediaEnvironment e = env(1000, 700);
    EXPECT_TRUE(matches("(width >= 600px)", e));
    EXPECT_FALSE(matches("(width < 1000px)", e));
    EXPECT_TRUE(matches("(width <= 1000px)", e));
    EXPECT_TRUE(matches("(600px <= width)", e));
    EXPECT_TRUE(matches("(400px < width < 1200px)", e));
    EXPECT_FALSE(matches("(400px < width < 900px)", e));
    EXPECT_TRUE(matches("(1200px > width > 400px)", e));
    EXPECT_FALSE(matches("(400px < width > 300px)", e));  // mixed directions: invalid
    EXPECT_TRUE(matches("(height > 600px) and (width = 1000px)", e));
    EXPECT_FALSE(matches("(min-width >= 10px)", e));
}

TEST_F(MediaQueryTest, AndOrNotAndNesting) {
    MediaEnvironment e = env(1000, 700);
    EXPECT_TRUE(matches("(max-width: 500px) or (min-width: 900px)", e));
    EXPECT_FALSE(matches("(max-width: 500px) or (min-width: 1100px)", e));
    EXPECT_TRUE(matches("not (max-width: 500px)", e));
    EXPECT_TRUE(matches("((min-width: 900px) and (orientation: landscape)) or (max-width: 10px)", e));
    EXPECT_FALSE(matches("(min-width: 1px) and (max-width: 2px) or (width)", e));  // and/or mixed: invalid
    EXPECT_FALSE(matches("screen and (min-width: 1px) or (width)", e));
}

TEST_F(MediaQueryTest, InvalidQueriesMatchNothingButKeepTheList) {
    MediaEnvironment e = env(1000, 700);
    EXPECT_FALSE(matches("(foo-bar: 1)", e));
    EXPECT_FALSE(matches("(min-width: )", e));
    EXPECT_FALSE(matches("screen and", e));
    EXPECT_FALSE(matches("and (min-width: 1px)", e));
    EXPECT_FALSE(matches("(min-orientation: landscape)", e));
    EXPECT_TRUE(matches("(foo-bar: 1), (min-width: 10px)", e));  // one bad query does not spoil the list
    EXPECT_TRUE(matches("not (foo-bar)", e));
    EXPECT_EQ(MediaQueryList::parse("Screen  AND (MIN-WIDTH:600px) ,print")->mediaText(),
              "screen and (min-width:600px), print");
}

TEST_F(MediaQueryTest, DiscreteFeatures) {
    MediaEnvironment e = env(1000, 700);
    EXPECT_TRUE(matches("(orientation: landscape)", e));
    EXPECT_FALSE(matches("(orientation: portrait)", e));
    EXPECT_TRUE(matches("(orientation: portrait)", env(400, 800)));
    EXPECT_TRUE(matches("(prefers-color-scheme: light)", e));
    EXPECT_FALSE(matches("(prefers-color-scheme: dark)", e));
    e.prefersDark = true;
    EXPECT_TRUE(matches("(prefers-color-scheme: dark)", e));
    EXPECT_TRUE(matches("(prefers-reduced-motion: no-preference)", e));
    EXPECT_FALSE(matches("(prefers-reduced-motion)", e));
    EXPECT_TRUE(matches("(hover: hover) and (pointer: fine)", e));
    EXPECT_TRUE(matches("(any-hover: hover)", e));
    EXPECT_FALSE(matches("(pointer: coarse)", e));
    EXPECT_TRUE(matches("(color)", e));
    EXPECT_FALSE(matches("(monochrome)", e));
    EXPECT_TRUE(matches("(min-color: 8)", e));
    EXPECT_FALSE(matches("(grid)", e));
    EXPECT_FALSE(matches("(forced-colors: active)", e));
    EXPECT_TRUE(matches("(display-mode: browser)", e));
}

TEST_F(MediaQueryTest, AspectRatioAndResolution) {
    MediaEnvironment e = env(1600, 900);
    EXPECT_TRUE(matches("(aspect-ratio: 16/9)", e));
    EXPECT_TRUE(matches("(min-aspect-ratio: 4/3)", e));
    EXPECT_FALSE(matches("(max-aspect-ratio: 1/1)", e));
    EXPECT_TRUE(matches("(resolution: 1dppx)", e));
    EXPECT_TRUE(matches("(min-resolution: 96dpi)", e));
    EXPECT_FALSE(matches("(min-resolution: 2dppx)", e));
    EXPECT_TRUE(matches("(-webkit-min-device-pixel-ratio: 1)", e));
    EXPECT_FALSE(matches("(-webkit-min-device-pixel-ratio: 1.5)", e));
    e.devicePixelRatio = 2;
    EXPECT_TRUE(matches("(min-resolution: 2x)", e));
    EXPECT_TRUE(matches("(-webkit-min-device-pixel-ratio: 1.5)", e));
}

TEST_F(MediaQueryTest, EnvironmentChangeReportsOnlyResultChanges) {
    auto narrow = MediaQueryList::parse("(max-width: 600px)");
    uint64_t before = mediaMatchVersion();
    uint64_t dom = domMutationVersion();
    EXPECT_FALSE(setMediaEnvironment(env(1100, 800)));   // still wide: nothing to restyle
    EXPECT_EQ(mediaMatchVersion(), before);
    EXPECT_TRUE(setMediaEnvironment(env(500, 800)));
    EXPECT_GT(mediaMatchVersion(), before);
    EXPECT_NE(domMutationVersion(), dom);
    EXPECT_FALSE(setMediaEnvironment(env(500, 800)));    // same values
    EXPECT_TRUE(narrow->matches());
}

// ---------------------------------------------------------------------------
// @media in the cascade
// ---------------------------------------------------------------------------

namespace {

using RGB = std::tuple<int, int, int>;

class MediaCascadeTest : public MediaQueryTest {
protected:
    std::unique_ptr<DOMDocument> doc;
    std::unique_ptr<CSSEngine> css;

    void load(const std::string& sheet) {
        HTMLParser parser;
        doc = parser.parse("<!DOCTYPE html><html><body><p id='p'>x</p><div id='d'>y</div></body></html>");
        ASSERT_NE(doc, nullptr);
        css = std::make_unique<CSSEngine>();
        css->initialize(doc.get());
        css->addStyleSheet(ZepraUAStylesheet::getStylesheet(), StyleOrigin::UserAgent);
        css->addStyleSheet(sheet, StyleOrigin::Author);
    }
    RGB color(const std::string& id) {
        css->computeStyles();
        const CSSComputedStyle* s = css->getComputedStyle(doc->getElementById(id));
        if (!s) return RGB{-1, -1, -1};
        return RGB{s->color.r, s->color.g, s->color.b};
    }
};

const RGB kRed{255, 0, 0}, kGreen{0, 128, 0}, kBlue{0, 0, 255};

}  // namespace

TEST_F(MediaCascadeTest, RulesApplyOnlyWhileTheirQueryMatches) {
    load("p { color: red; }"
         "@media (min-width: 600px) { p { color: green; } }"
         "@media print { div { color: blue; } }"
         "div { color: red; }");
    setMediaEnvironment(env(1000, 700));
    EXPECT_EQ(color("p"), kGreen);
    EXPECT_EQ(color("d"), kRed);
    setMediaEnvironment(env(500, 700));
    EXPECT_EQ(color("p"), kRed);
}

TEST_F(MediaCascadeTest, SourceOrderAndSpecificityStillDecide) {
    load("@media screen { #p { color: green; } p { color: blue; } }"
         "p { color: red; }"
         "@media (min-width: 1px) { div { color: blue; } }"
         "div { color: red; }"
         "@media (min-width: 1px) { div { color: green; } }");
    setMediaEnvironment(env(1000, 700));
    EXPECT_EQ(color("p"), kGreen);   // the id rule wins over the later type rule
    EXPECT_EQ(color("d"), kGreen);   // the last of three equal rules wins
}

TEST_F(MediaCascadeTest, NestedMediaAndSupportsBlocks) {
    load("@media (min-width: 600px) { @media (max-width: 900px) { p { color: green; } } }"
         "@supports (display: grid) { @media (min-width: 1px) { div { color: blue; } } }");
    setMediaEnvironment(env(800, 700));
    EXPECT_EQ(color("p"), kGreen);
    EXPECT_EQ(color("d"), kBlue);
    setMediaEnvironment(env(1000, 700));
    EXPECT_NE(color("p"), kGreen);
}

TEST_F(MediaCascadeTest, RulesAfterMediaBlocksAreUnaffected) {
    load("@media (max-width: 10px) { p { color: blue; } } p { color: green; } @media print{} div { color: green; }");
    setMediaEnvironment(env(1000, 700));
    EXPECT_EQ(color("p"), kGreen);
    EXPECT_EQ(color("d"), kGreen);
}

// ---------------------------------------------------------------------------
// window.matchMedia()
// ---------------------------------------------------------------------------

namespace {

struct Page {
    std::unique_ptr<DOMDocument> doc;
    std::unique_ptr<ScriptContext> script;

    Page() {
        HTMLParser parser;
        doc = parser.parse("<!DOCTYPE html><html><body></body></html>");
        script = std::make_unique<ScriptContext>();
        script->initialize(doc.get());
    }
    std::string eval(const std::string& code) {
        ScriptResult r = script->evaluate(code, "test.js");
        EXPECT_TRUE(r.success) << r.error << "\n  in: " << code;
        return r.success ? r.value : "<error: " + r.error + ">";
    }
};

}  // namespace

TEST_F(MediaQueryTest, MatchMediaReportsTheQueryAndItsResult) {
    setMediaEnvironment(env(1000, 700));
    Page page;
    EXPECT_EQ(page.eval("typeof matchMedia + ' ' + typeof window.matchMedia"), "function function");
    EXPECT_EQ(page.eval("var m = matchMedia('(MIN-WIDTH: 600px)'); m.matches"), "true");
    EXPECT_EQ(page.eval("m.media"), "(min-width: 600px)");
    EXPECT_EQ(page.eval("m instanceof MediaQueryList && m instanceof EventTarget"), "true");
    EXPECT_EQ(page.eval("matchMedia('print').matches + ' ' + matchMedia('(max-width: 600px)').matches"), "false false");
    EXPECT_EQ(page.eval("matchMedia('').matches + ' ' + matchMedia('(foo)').matches"), "true false");
    EXPECT_EQ(page.eval("m.onchange"), "null");
    EXPECT_EQ(page.eval("try { matchMedia(); 'no' } catch (e) { e.name }"), "TypeError");
}

TEST_F(MediaQueryTest, MatchMediaFiresChangeWhenTheResultFlips) {
    setMediaEnvironment(env(1000, 700));
    Page page;
    page.eval("var log = []; var m = matchMedia('(max-width: 600px)');"
              "m.addEventListener('change', function (e) { log.push('listener ' + e.matches + ' ' + e.media + ' ' + (e instanceof MediaQueryListEvent)); });"
              "m.onchange = function (e) { log.push('onchange ' + e.matches); };"
              "var legacy = function (e) { log.push('legacy ' + e.matches); }; m.addListener(legacy);");
    setMediaEnvironment(env(900, 700));   // still wide
    page.script->evaluateMediaQueries();
    EXPECT_EQ(page.eval("log.length"), "0");
    setMediaEnvironment(env(500, 700));
    EXPECT_EQ(page.eval("m.matches"), "true");   // live even before the event
    page.script->evaluateMediaQueries();
    EXPECT_EQ(page.eval("log.join('|')"),
              "listener true (max-width: 600px) true|legacy true|onchange true");
    page.script->evaluateMediaQueries();         // reported once
    EXPECT_EQ(page.eval("log.length"), "3");
    page.eval("m.removeListener(legacy); log = [];");
    setMediaEnvironment(env(700, 700));
    page.script->evaluateMediaQueries();
    EXPECT_EQ(page.eval("log.join('|')"), "listener false (max-width: 600px) true|onchange false");
}
