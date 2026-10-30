// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file devtools_style_rules_tests.cpp
 * @brief The Inspector's Rules pane: which rules match an element, their
 *        cascade order and sources, which declarations lose the cascade,
 *        inherited rules, and switching declarations off for one element.
 */

#include <gtest/gtest.h>

#include "browser/dom.hpp"
#include "css/css_engine.hpp"
#include "css/ua_stylesheet.h"
#include "devtools/style_rules.hpp"
#include "html/html_parser.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

// Globals the layout engine expects from the browser
int g_width = 1280;
int g_height = 800;

using namespace Zepra::WebCore;
using namespace ZepraBrowser::DevTools;

namespace {

const char* kHtml = R"(<!DOCTYPE html><html><head></head><body>
<div id="outer" class="box" style="font-size: 20px; margin: 4px">
  <p id="x" class="a b" style="color: black">text</p>
  <span id="s">t</span>
</div>
</body></html>)";

// A <style> element's sheet (no href)
const char* kStyleSheet =
    "p { color: green; margin: 0 }\n"                                                  // rule 1
    ".a { color: red; padding: 1px }\n"                                                // rule 2
    "#x { color: blue; margin-left: 3px; margin-right: 3px; margin-bottom: 3px }\n"    // rule 3
    ".b { color: purple !important }\n"                                                // rule 4
    ".unused { color: gray }\n";                                                       // rule 5

// A <link> sheet
const char* kLinkedSheet =
    "div { color: orange; font-family: serif; border: 2px solid }\n"   // rule 1
    "#outer { line-height: 2 }\n"                                     // rule 2
    "p { margin-top: 8px }\n";                                        // rule 3

struct Page {
    std::unique_ptr<DOMDocument> doc;
    std::unique_ptr<CSSEngine> css;
    std::vector<std::pair<const DOMElement*, StyleEdit>> edits;

    Page() {
        HTMLParser parser;
        doc = parser.parse(kHtml);
        css = std::make_unique<CSSEngine>();
        css->initialize(doc.get());
        css->addStyleSheet(ZepraUAStylesheet::getStylesheet(), StyleOrigin::UserAgent);
        css->addStyleSheet(kStyleSheet, StyleOrigin::Author);
        css->addStyleSheet(kLinkedSheet, StyleOrigin::Author, "https://example.com/css/site.css?v=2");
    }
    DOMElement* el(const std::string& id) { return doc->getElementById(id); }
    ElementRules rules(const std::string& id) {
        return collectElementRules(*css, el(id), [this](const DOMElement* e) -> const StyleEdit* {
            for (const auto& [owner, edit] : edits) if (owner == e) return &edit;
            return nullptr;
        });
    }
    const CSSComputedStyle* style(const std::string& id) {
        css->invalidate();
        return css->getComputedStyle(el(id));
    }
    /// Switches a declaration off on one element, as the panel does
    void disable(const std::string& id, const CSSStyleRule* rule, const std::string& property) {
        DOMElement* e = el(id);
        auto it = std::find_if(edits.begin(), edits.end(), [&](const auto& p) { return p.first == e; });
        if (it == edits.end()) {
            StyleEdit edit;
            edit.hadStyle = e->hasAttribute("style");
            edit.originalStyle = e->getAttribute("style");
            edits.push_back({e, edit});
            it = edits.end() - 1;
        }
        it->second.disabled.push_back({rule, property});
        it->second.writtenStyle = styleAttributeFor(*css, e, it->second);
        e->setAttribute("style", it->second.writtenStyle);
    }
};

std::vector<std::string> selectors(const std::vector<RuleBlock>& blocks, StyleOrigin origin) {
    std::vector<std::string> out;
    for (const auto& b : blocks) {
        if (b.kind == RuleKind::Style && b.origin == origin) out.push_back(b.selector);
    }
    return out;
}

const RuleBlock* findBlock(const std::vector<RuleBlock>& blocks, const std::string& selector,
                           const std::string& sourcePrefix = "") {
    for (const auto& b : blocks) {
        if (b.selector == selector && b.source.rfind(sourcePrefix, 0) == 0) return &b;
    }
    return nullptr;
}

const RuleDeclaration* findDecl(const RuleBlock* block, const std::string& property) {
    if (!block) return nullptr;
    for (const auto& d : block->declarations) {
        if (d.property == property) return &d;
    }
    return nullptr;
}

bool sameColor(const CSSColor& c, uint8_t r, uint8_t g, uint8_t b) {
    return c.r == r && c.g == g && c.b == b;
}

} // namespace

TEST(DevToolsStyleRules, InlineStyleFirstThenMatchedRulesMostSpecificFirst) {
    Page page;
    ElementRules rules = page.rules("x");
    ASSERT_FALSE(rules.blocks.empty());
    EXPECT_EQ(rules.blocks[0].kind, RuleKind::Inline);
    EXPECT_EQ(rules.blocks[0].selector, "element");
    ASSERT_EQ(rules.blocks[0].declarations.size(), 1u);
    EXPECT_EQ(rules.blocks[0].declarations[0].property, "color");
    EXPECT_EQ(rules.blocks[0].declarations[0].value, "black");

    // #x (1,0,0); .b and .a (0,1,0), the later one first; then the two `p` rules, later sheet first
    std::vector<std::string> expected = {"#x", ".b", ".a", "p", "p"};
    EXPECT_EQ(selectors(rules.blocks, StyleOrigin::Author), expected);
    EXPECT_EQ(findBlock(rules.blocks, ".unused"), nullptr);

    // Author rules come before the user-agent ones
    size_t lastAuthor = 0, firstUA = rules.blocks.size();
    for (size_t i = 0; i < rules.blocks.size(); i++) {
        if (rules.blocks[i].kind != RuleKind::Style) continue;
        if (rules.blocks[i].origin == StyleOrigin::Author) lastAuthor = i;
        else if (firstUA == rules.blocks.size()) firstUA = i;
    }
    EXPECT_FALSE(selectors(rules.blocks, StyleOrigin::UserAgent).empty());
    EXPECT_LT(lastAuthor, firstUA);
}

TEST(DevToolsStyleRules, SourcesNameTheSheetAndRule) {
    Page page;
    ElementRules rules = page.rules("x");
    const RuleBlock* idRule = findBlock(rules.blocks, "#x");
    ASSERT_NE(idRule, nullptr);
    EXPECT_EQ(idRule->source, "<style> 1, rule 3");
    EXPECT_NE(findBlock(rules.blocks, "p", "site.css, rule 3"), nullptr);
    EXPECT_NE(findBlock(rules.blocks, "p", "<style> 1, rule 1"), nullptr);
    for (const auto& b : rules.blocks) {
        if (b.kind == RuleKind::Style && b.origin == StyleOrigin::UserAgent) {
            EXPECT_EQ(b.source.rfind("user agent, rule ", 0), 0u) << b.source;
        }
    }
    EXPECT_TRUE(rules.blocks[0].source.empty());
}

TEST(DevToolsStyleRules, ImportantBeatsMoreSpecificRulesAndTheStyleAttribute) {
    Page page;
    ElementRules rules = page.rules("x");
    const RuleDeclaration* important = findDecl(findBlock(rules.blocks, ".b"), "color");
    ASSERT_NE(important, nullptr);
    EXPECT_TRUE(important->important);
    EXPECT_FALSE(important->overridden);
    EXPECT_TRUE(rules.blocks[0].declarations[0].overridden);                    // style="color: black"
    EXPECT_TRUE(findDecl(findBlock(rules.blocks, "#x"), "color")->overridden);
    EXPECT_TRUE(findDecl(findBlock(rules.blocks, ".a"), "color")->overridden);
    EXPECT_TRUE(findDecl(findBlock(rules.blocks, "p", "<style>"), "color")->overridden);

    DeclarationRef winner = rules.winner("color");
    EXPECT_EQ(rules.declaration(winner), important);
    // The engine agrees
    EXPECT_TRUE(sameColor(page.style("x")->color, 128, 0, 128));
}

TEST(DevToolsStyleRules, ShorthandIsOverriddenOnlyWhenEveryLonghandIs) {
    Page page;
    ElementRules rules = page.rules("x");
    // p { margin: 0 }: top lost to the later `p { margin-top }`, the rest to #x
    EXPECT_TRUE(findDecl(findBlock(rules.blocks, "p", "<style>"), "margin")->overridden);
    EXPECT_FALSE(findDecl(findBlock(rules.blocks, "p", "site.css"), "margin-top")->overridden);
    EXPECT_FALSE(findDecl(findBlock(rules.blocks, "#x"), "margin-left")->overridden);
    // .a { padding: 1px } is the only padding
    EXPECT_FALSE(findDecl(findBlock(rules.blocks, ".a"), "padding")->overridden);
    EXPECT_EQ(rules.block(rules.winner("margin-top"))->source, "site.css, rule 3");
    EXPECT_EQ(rules.block(rules.winner("padding"))->selector, ".a");
}

TEST(DevToolsStyleRules, InheritedRulesKeepOnlyInheritedProperties) {
    Page page;
    ElementRules rules = page.rules("x");
    ASSERT_FALSE(rules.inherited.empty());
    const InheritedRules& div = rules.inherited[0];
    EXPECT_EQ(div.ancestor, page.el("outer"));

    // style="font-size: 20px; margin: 4px" keeps only font-size
    ASSERT_FALSE(div.blocks.empty());
    EXPECT_EQ(div.blocks[0].kind, RuleKind::Inline);
    ASSERT_EQ(div.blocks[0].declarations.size(), 1u);
    EXPECT_EQ(div.blocks[0].declarations[0].property, "font-size");
    EXPECT_FALSE(div.blocks[0].declarations[0].overridden);

    // div { color; font-family; border }: no border; color is set on the <p> itself
    const RuleBlock* divRule = findBlock(div.blocks, "div", "site.css");
    ASSERT_NE(divRule, nullptr);
    EXPECT_EQ(findDecl(divRule, "border"), nullptr);
    EXPECT_TRUE(findDecl(divRule, "color")->overridden);
    EXPECT_FALSE(findDecl(divRule, "font-family")->overridden);
    EXPECT_NE(findBlock(div.blocks, "#outer"), nullptr);

    EXPECT_EQ(rules.winner("font-family").group, 0);
    EXPECT_EQ(rules.winner("line-height").group, 0);
    EXPECT_EQ(rules.winner("color").group, -1);
    // Non-inherited properties of ancestors never win
    EXPECT_FALSE(rules.winner("border-top-width").valid());
}

TEST(DevToolsStyleRules, InheritedFromNearestAncestorWins) {
    Page page;
    // The <span> sets no color: the div's rule gives it
    ElementRules rules = page.rules("s");
    DeclarationRef color = rules.winner("color");
    ASSERT_TRUE(color.valid());
    EXPECT_EQ(rules.inherited[color.group].ancestor, page.el("outer"));
    EXPECT_FALSE(rules.declaration(color)->overridden);
}

TEST(DevToolsStyleRules, SwitchingOffARuleDeclarationAffectsOnlyThatElement) {
    Page page;
    const RuleBlock* b = findBlock(page.rules("x").blocks, ".b");
    ASSERT_NE(b, nullptr);
    const CSSStyleRule* bRule = b->rule;

    page.disable("x", bRule, "color");
    // The style attribute wins again
    EXPECT_TRUE(sameColor(page.style("x")->color, 0, 0, 0));
    ElementRules rules = page.rules("x");
    EXPECT_EQ(rules.blocks[0].declarations.size(), 1u) << "the pane shows the original style attribute";
    EXPECT_FALSE(rules.blocks[0].declarations[0].overridden);
    EXPECT_TRUE(findDecl(findBlock(rules.blocks, ".b"), "color")->disabled);
    // The shared rule is untouched
    EXPECT_EQ(bRule->style()->getPropertyValue("color"), "purple");

    // Then the style attribute's own declaration: #x is next
    page.disable("x", nullptr, "color");
    EXPECT_TRUE(sameColor(page.style("x")->color, 0, 0, 255));
    rules = page.rules("x");
    EXPECT_TRUE(rules.blocks[0].declarations[0].disabled);
    EXPECT_FALSE(findDecl(findBlock(rules.blocks, "#x"), "color")->overridden);
}

TEST(DevToolsStyleRules, SwitchingOffALonghandFallsBackToTheShorthand) {
    Page page;
    const CSSStyleRule* idRule = findBlock(page.rules("x").blocks, "#x")->rule;
    EXPECT_FLOAT_EQ(page.style("x")->marginLeft.value, 3);
    page.disable("x", idRule, "margin-left");
    EXPECT_FLOAT_EQ(page.style("x")->marginLeft.value, 0);    // p { margin: 0 }
    EXPECT_FLOAT_EQ(page.style("x")->marginRight.value, 3);   // still #x
    EXPECT_FLOAT_EQ(page.style("x")->marginTop.value, 8);     // still site.css p
}

TEST(DevToolsStyleRules, SwitchingOffTheOnlyDeclarationUsesTheInitialValue) {
    Page page;
    const CSSStyleRule* divRule = findBlock(page.rules("outer").blocks, "div", "site.css")->rule;
    EXPECT_FLOAT_EQ(page.style("outer")->borderTopWidth, 2);
    page.disable("outer", divRule, "border");
    EXPECT_FLOAT_EQ(page.style("outer")->borderTopWidth, 0);
    // The shared rule is untouched
    EXPECT_EQ(divRule->style()->getPropertyValue("border"), "2px solid");
}

TEST(DevToolsStyleRules, Longhands) {
    EXPECT_EQ(longhandsOf("margin").size(), 4u);
    EXPECT_EQ(longhandsOf("border").size(), 12u);
    EXPECT_EQ(longhandsOf("border-left"),
              (std::vector<std::string>{"border-left-width", "border-left-style", "border-left-color"}));
    EXPECT_EQ(longhandsOf("color"), std::vector<std::string>{"color"});
}
