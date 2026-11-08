// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file webcore_custom_property_tests.cpp
 * @brief CSS custom properties: var() substitution, resolving an element's
 *        custom properties (inheritance, keywords, cycles), the cascade using
 *        them, and getComputedStyle() reporting them.
 */

#include <gtest/gtest.h>

#include "browser/dom.hpp"
#include "css/css_engine.hpp"
#include "css/custom_properties.hpp"
#include "css/media_query.hpp"
#include "css/ua_stylesheet.h"
#include "html/html_parser.hpp"
#include "scripting/script_context.hpp"

#include <functional>
#include <memory>
#include <string>
#include <tuple>

// Viewport size the layout engine reads (zepra_browser.cpp defines these)
int g_width = 1280;
int g_height = 800;

using namespace Zepra::WebCore;

namespace {

std::string sub(const std::string& value, const CustomPropertyMap& vars) {
    std::string out;
    return substituteVarFunctions(value, vars, out) ? out : "<invalid>";
}

}  // namespace

// ---------------------------------------------------------------------------
// var() substitution
// ---------------------------------------------------------------------------

TEST(VarSubstitution, ReplacesReferencesAndUsesFallbacks) {
    CustomPropertyMap vars{{"--a", "10px"}, {"--color", "rgb(1, 2, 3)"}, {"--Case", "big"}};
    EXPECT_EQ(sub("var(--a)", vars), "10px");
    EXPECT_EQ(sub("var( --a ) var(--a)", vars), "10px 10px");
    EXPECT_EQ(sub("calc(var(--a) * 2)", vars), "calc(10px * 2)");
    EXPECT_EQ(sub("1px solid var(--color)", vars), "1px solid rgb(1, 2, 3)");
    EXPECT_EQ(sub("VAR(--a)", vars), "10px");
    EXPECT_EQ(sub("var(--missing, red)", vars), "red");
    EXPECT_EQ(sub("var(--missing, Arial, sans-serif)", vars), "Arial, sans-serif");
    EXPECT_EQ(sub("var(--missing, var(--also-missing, var(--a)))", vars), "10px");
    EXPECT_EQ(sub("var(--missing,)", vars), "");
    EXPECT_EQ(sub("var(--a, var(--missing))", vars), "10px");  // the unused fallback doesn't matter
    // Names are case-sensitive
    EXPECT_EQ(sub("var(--Case)", vars), "big");
    EXPECT_EQ(sub("var(--case)", vars), "<invalid>");
}

TEST(VarSubstitution, InvalidReferences) {
    CustomPropertyMap vars{{"--a", "1"}};
    EXPECT_EQ(sub("var(--missing)", vars), "<invalid>");
    EXPECT_EQ(sub("var(--missing, var(--gone))", vars), "<invalid>");
    EXPECT_EQ(sub("var(a)", vars), "<invalid>");
    EXPECT_EQ(sub("var(--)", vars), "<invalid>");
    EXPECT_EQ(sub("var(--a", vars), "<invalid>");
}

TEST(VarSubstitution, LeavesLookalikesAndStringsAlone) {
    CustomPropertyMap vars{{"--a", "1"}};
    EXPECT_EQ(sub("\"var(--a)\"", vars), "\"var(--a)\"");
    EXPECT_EQ(sub("myvar(--a)", vars), "myvar(--a)");
    EXPECT_EQ(sub("var(--missing, \"a)b\")", vars), "\"a)b\"");
    EXPECT_FALSE(containsVarFunction("avar(x)"));
    EXPECT_TRUE(containsVarFunction("calc(1px + Var(--x))"));
    EXPECT_TRUE(isCustomPropertyName("--x"));
    EXPECT_FALSE(isCustomPropertyName("--"));
    EXPECT_FALSE(isCustomPropertyName("-x"));
}

// ---------------------------------------------------------------------------
// Resolving an element's custom properties
// ---------------------------------------------------------------------------

TEST(ResolveCustomProperties, InheritsOverridesAndChains) {
    auto parent = resolveCustomProperties(nullptr, {{"--a", " 1px "}, {"--b", "var(--a) 2px"}});
    ASSERT_TRUE(parent);
    EXPECT_EQ(parent->at("--a"), "1px");
    EXPECT_EQ(parent->at("--b"), "1px 2px");

    // No declarations: the parent's map is shared, not copied
    EXPECT_EQ(resolveCustomProperties(parent, {}), parent);

    // A child overrides --a; the inherited --b keeps its resolved value
    auto child = resolveCustomProperties(parent, {{"--a", "5px"}, {"--c", "var(--a) var(--b)"}});
    EXPECT_EQ(child->at("--a"), "5px");
    EXPECT_EQ(child->at("--b"), "1px 2px");
    EXPECT_EQ(child->at("--c"), "5px 1px 2px");
}

TEST(ResolveCustomProperties, KeywordsAndInvalidValues) {
    auto parent = resolveCustomProperties(nullptr, {{"--a", "red"}, {"--b", "blue"}});
    auto child = resolveCustomProperties(parent, {{"--a", "initial"}, {"--b", "inherit"},
                                                  {"--c", "var(--nope)"}, {"--d", "var(--nope, ok)"}});
    EXPECT_EQ(child->count("--a"), 0u);
    EXPECT_EQ(child->at("--b"), "blue");
    EXPECT_EQ(child->count("--c"), 0u);
    EXPECT_EQ(child->at("--d"), "ok");
}

TEST(ResolveCustomProperties, CyclesAreInvalid) {
    auto m = resolveCustomProperties(nullptr, {{"--a", "var(--b)"}, {"--b", "var(--a, x)"},
                                               {"--self", "var(--self, 1)"},
                                               {"--user", "var(--a, fallback)"}, {"--ok", "fine"}});
    EXPECT_EQ(m->count("--a"), 0u);
    EXPECT_EQ(m->count("--b"), 0u);
    EXPECT_EQ(m->count("--self"), 0u);
    EXPECT_EQ(m->at("--user"), "fallback");  // depends on a cycle but isn't in it
    EXPECT_EQ(m->at("--ok"), "fine");
}

// ---------------------------------------------------------------------------
// The cascade
// ---------------------------------------------------------------------------

namespace {

struct RGB {
    int r, g, b;
    bool operator==(const RGB& o) const { return std::tie(r, g, b) == std::tie(o.r, o.g, o.b); }
};
std::ostream& operator<<(std::ostream& os, const RGB& c) {
    return os << "rgb(" << c.r << "," << c.g << "," << c.b << ")";
}
const RGB kRed{255, 0, 0}, kGreen{0, 128, 0}, kBlue{0, 0, 255}, kBlack{0, 0, 0};

class CustomPropertyCascadeTest : public ::testing::Test {
protected:
    std::unique_ptr<DOMDocument> doc;
    std::unique_ptr<CSSEngine> css;

    void load(const std::string& sheet,
              const std::string& body = "<div id='outer'><p id='p'>x</p><div id='inner'><p id='q'>y</p></div></div>") {
        HTMLParser parser;
        doc = parser.parse("<!DOCTYPE html><html><body>" + body + "</body></html>");
        ASSERT_NE(doc, nullptr);
        css = std::make_unique<CSSEngine>();
        css->initialize(doc.get());
        css->addStyleSheet(ZepraUAStylesheet::getStylesheet(), StyleOrigin::UserAgent);
        css->addStyleSheet(sheet, StyleOrigin::Author);
        css->computeStyles();
    }
    const CSSComputedStyle* style(const std::string& id) { return css->getComputedStyle(doc->getElementById(id)); }
    RGB color(const std::string& id) {
        const CSSComputedStyle* s = style(id);
        return s ? RGB{s->color.r, s->color.g, s->color.b} : RGB{-1, -1, -1};
    }
};

}  // namespace

TEST_F(CustomPropertyCascadeTest, RootVariablesReachEveryElement) {
    load(":root { --main: green; --gap: 12px; } p { color: var(--main); margin-top: var(--gap); }");
    EXPECT_EQ(color("p"), kGreen);
    EXPECT_EQ(color("q"), kGreen);
    EXPECT_FLOAT_EQ(style("p")->marginTop.value, 12.0f);
}

TEST_F(CustomPropertyCascadeTest, ScopedOverridesApplyToDescendantsOnly) {
    load(":root { --c: red; } #inner { --c: green; } p { color: var(--c); }");
    EXPECT_EQ(color("p"), kRed);
    EXPECT_EQ(color("q"), kGreen);
}

TEST_F(CustomPropertyCascadeTest, CustomPropertiesFollowTheCascade) {
    load("#p { --c: green; } p { --c: red; color: var(--c); }"
         "#q { --c: blue !important; } #inner p { --c: red; color: var(--c); }");
    EXPECT_EQ(color("p"), kGreen);  // the id rule wins although it comes first
    EXPECT_EQ(color("q"), kBlue);   // !important wins over a more specific rule
}

TEST_F(CustomPropertyCascadeTest, DeclarationOrderDoesNotMatter) {
    // var() sees the element's final value even when the custom property is
    // declared after the property that uses it
    load("p { color: var(--c); --c: green; }");
    EXPECT_EQ(color("p"), kGreen);
}

TEST_F(CustomPropertyCascadeTest, ShorthandsAndFallbacks) {
    load(":root { --m: 4px 8px; } #p { margin: var(--m); color: var(--none, rgb(0, 0, 255)); }");
    const CSSComputedStyle* s = style("p");
    ASSERT_TRUE(s);
    EXPECT_FLOAT_EQ(s->marginTop.value, 4.0f);
    EXPECT_FLOAT_EQ(s->marginRight.value, 8.0f);
    EXPECT_FLOAT_EQ(s->marginBottom.value, 4.0f);
    EXPECT_EQ(color("p"), kBlue);
}

TEST_F(CustomPropertyCascadeTest, UnresolvableVarLeavesThePropertyInheritedOrInitial) {
    load("#outer { color: green; } p { color: red; } #p { color: var(--missing); }"
         "#inner { --loop: var(--loop); } #q { color: var(--loop); }");
    // An invalid value is dropped at computed-value time, not at parse time,
    // so the earlier "p { color: red }" doesn't come back: color is inherited
    EXPECT_EQ(color("p"), kGreen);
    EXPECT_EQ(color("q"), kGreen);
}

TEST_F(CustomPropertyCascadeTest, InlineStylesAndKeywords) {
    load(":root { --c: red; } #inner { --c: initial; } #q { color: var(--c, green); }",
         "<div id='outer'><p id='p' style='--c: blue; color: var(--c)'>x</p>"
         "<div id='inner'><p id='q'>y</p></div></div>");
    EXPECT_EQ(color("p"), kBlue);
    EXPECT_EQ(color("q"), kGreen);
}

TEST_F(CustomPropertyCascadeTest, MediaRulesCanChangeAVariable) {
    MediaEnvironment env;
    env.width = 1000;
    setMediaEnvironment(env);
    load(":root { --c: red; } @media (min-width: 600px) { :root { --c: green; } } p { color: var(--c); }");
    EXPECT_EQ(color("p"), kGreen);
    env.width = 500;
    setMediaEnvironment(env);
    css->computeStyles();
    EXPECT_EQ(color("p"), kRed);
}

TEST_F(CustomPropertyCascadeTest, UnusedVariablesDoNotAffectStyles) {
    load(":root { --c: green; } p { color: black; }");
    EXPECT_EQ(color("p"), kBlack);
}

// ---------------------------------------------------------------------------
// getComputedStyle()
// ---------------------------------------------------------------------------

TEST(CustomPropertyScripting, GetComputedStyleReportsCustomProperties) {
    HTMLParser parser;
    auto doc = parser.parse("<!DOCTYPE html><html><body><div id='d'><p id='p'>x</p></div></body></html>");
    CSSEngine css;
    css.initialize(doc.get());
    css.addStyleSheet(ZepraUAStylesheet::getStylesheet(), StyleOrigin::UserAgent);
    css.addStyleSheet(":root { --Brand: rgb(0, 128, 0); } #d { --size: 2px; --size2: calc(var(--size) * 2); }"
                      "p { color: var(--Brand); }",
                      StyleOrigin::Author);
    css.computeStyles();
    uint64_t styled = domMutationVersion();

    ScriptContext script;
    script.initialize(doc.get());
    script.setComputedStyleSource([&](DOMElement* element) -> const CSSComputedStyle* {
        if (domMutationVersion() != styled) {
            css.computeStyles();
            styled = domMutationVersion();
        }
        return css.getComputedStyle(element);
    });
    auto eval = [&](const std::string& code) {
        ScriptResult r = script.evaluate(code, "test.js");
        EXPECT_TRUE(r.success) << r.error << "\n  in: " << code;
        return r.success ? r.value : "<error: " + r.error + ">";
    };
    EXPECT_EQ(eval("var cs = getComputedStyle(document.getElementById('p')); cs.getPropertyValue('--Brand')"),
              "rgb(0, 128, 0)");
    EXPECT_EQ(eval("cs.getPropertyValue('--size2')"), "calc(2px * 2)");
    EXPECT_EQ(eval("cs.getPropertyValue('--brand')"), "");
    EXPECT_EQ(eval("cs.color"), "rgb(0, 128, 0)");
    // Changing a variable from script restyles whatever uses it
    EXPECT_EQ(eval("document.getElementById('d').style.setProperty('--Brand', 'rgb(0, 0, 255)'); cs.color"),
              "rgb(0, 0, 255)");
}
