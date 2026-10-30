// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file devtools_style_rules_tests.cpp
 * @brief The Inspector's Rules pane: which rules match an element, their
 *        cascade order and sources, which declarations lose the cascade,
 *        inherited rules, and switching declarations off, editing, adding
 *        and deleting them for one element.
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
<p id="y" class="a">u</p>
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

const RuleBlock* findBlock(const std::vector<RuleBlock>& blocks, const std::string& selector,
                           const std::string& sourcePrefix = "");

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
    /// The element's edit, started as the panel starts one
    StyleEdit& edit(const std::string& id) {
        DOMElement* e = el(id);
        auto it = std::find_if(edits.begin(), edits.end(), [&](const auto& p) { return p.first == e; });
        if (it != edits.end()) return it->second;
        StyleEdit edit;
        edit.hadStyle = e->hasAttribute("style");
        edit.originalStyle = e->getAttribute("style");
        edits.push_back({e, edit});
        return edits.back().second;
    }
    /// Writes the element's edit into its style attribute, as the panel does after a change
    void write(const std::string& id) {
        DOMElement* e = el(id);
        auto it = std::find_if(edits.begin(), edits.end(), [&](const auto& p) { return p.first == e; });
        if (it == edits.end()) return;
        StyleEdit& ed = it->second;
        if (ed.empty()) {
            if (ed.hadStyle) e->setAttribute("style", ed.originalStyle);
            else e->removeAttribute("style");
            edits.erase(it);
            return;
        }
        ed.writtenStyle = styleAttributeFor(*css, e, ed);
        e->setAttribute("style", ed.writtenStyle);
    }
    const CSSStyleRule* rule(const std::string& id, const std::string& selector, const std::string& source = "") {
        const RuleBlock* b = findBlock(rules(id).blocks, selector, source);
        return b ? b->rule : nullptr;
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
                           const std::string& sourcePrefix) {
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

TEST(DevToolsStyleRules, EditingARuleValueChangesOnlyThatElement) {
    Page page;
    const CSSStyleRule* aRule = page.rule("x", ".a");
    ASSERT_NE(aRule, nullptr);
    EXPECT_TRUE(setDeclarationValue(page.edit("x"), aRule, "padding", "5px"));
    page.write("x");

    EXPECT_FLOAT_EQ(page.style("x")->paddingTop.value, 5);
    EXPECT_FLOAT_EQ(page.style("x")->paddingLeft.value, 5);
    // The other element the rule matches keeps the rule's value, and the rule is untouched
    EXPECT_FLOAT_EQ(page.style("y")->paddingTop.value, 1);
    EXPECT_EQ(aRule->style()->getPropertyValue("padding"), "1px");
    EXPECT_EQ(aRule->style()->length(), 2u);

    ElementRules rules = page.rules("x");
    const RuleDeclaration* padding = findDecl(findBlock(rules.blocks, ".a"), "padding");
    ASSERT_NE(padding, nullptr);
    EXPECT_EQ(padding->value, "5px");
    EXPECT_TRUE(padding->edited);
    EXPECT_FALSE(padding->added);
    EXPECT_FALSE(padding->overridden);
    EXPECT_FALSE(findDecl(findBlock(rules.blocks, ".a"), "color")->edited);
    // The style attribute the pane shows is still the element's own
    ASSERT_EQ(rules.blocks[0].declarations.size(), 1u);
    EXPECT_EQ(rules.blocks[0].declarations[0].value, "black");
    EXPECT_FALSE(rules.blocks[0].declarations[0].edited);
    // y's pane shows the rule as the page has it
    EXPECT_FALSE(findDecl(findBlock(page.rules("y").blocks, ".a"), "padding")->edited);
}

TEST(DevToolsStyleRules, EditedValueKeepsItsCascadeRank) {
    Page page;
    const CSSStyleRule* pRule = page.rule("x", "p", "<style>");
    ASSERT_NE(pRule, nullptr);
    EXPECT_TRUE(setDeclarationValue(page.edit("x"), pRule, "margin", "7px"));
    page.write("x");
    // #x still wins left/right/bottom, site.css's later `p` still wins top
    EXPECT_FLOAT_EQ(page.style("x")->marginLeft.value, 3);
    EXPECT_FLOAT_EQ(page.style("x")->marginBottom.value, 3);
    EXPECT_FLOAT_EQ(page.style("x")->marginTop.value, 8);
    ElementRules rules = page.rules("x");
    const RuleDeclaration* margin = findDecl(findBlock(rules.blocks, "p", "<style>"), "margin");
    EXPECT_TRUE(margin->edited);
    EXPECT_TRUE(margin->overridden);
    EXPECT_EQ(rules.block(rules.winner("margin-left"))->selector, "#x");
    EXPECT_EQ(rules.block(rules.winner("margin-top"))->source, "site.css, rule 3");

    // On y nothing more specific sets the sides: the edit shows where it ranks
    EXPECT_TRUE(setDeclarationValue(page.edit("y"), pRule, "margin", "7px"));
    page.write("y");
    EXPECT_FLOAT_EQ(page.style("y")->marginLeft.value, 7);
    EXPECT_FLOAT_EQ(page.style("y")->marginTop.value, 8);
    rules = page.rules("y");
    EXPECT_FALSE(findDecl(findBlock(rules.blocks, "p", "<style>"), "margin")->overridden);
    EXPECT_EQ(rules.block(rules.winner("margin-left"))->source, "<style> 1, rule 1");
}

TEST(DevToolsStyleRules, ImportantTypedIntoAValue) {
    Page page;
    const CSSStyleRule* idRule = page.rule("x", "#x");
    EXPECT_TRUE(setDeclarationValue(page.edit("x"), idRule, "color", "#00ff00 ! IMPORTANT;"));
    page.write("x");
    // #x !important beats .b !important and the style attribute
    EXPECT_TRUE(sameColor(page.style("x")->color, 0, 255, 0));
    ElementRules rules = page.rules("x");
    const RuleDeclaration* color = findDecl(findBlock(rules.blocks, "#x"), "color");
    EXPECT_EQ(color->value, "#00ff00");
    EXPECT_TRUE(color->important);
    EXPECT_TRUE(color->edited);
    EXPECT_EQ(rules.declaration(rules.winner("color")), color);
    EXPECT_TRUE(findDecl(findBlock(rules.blocks, ".b"), "color")->overridden);
    EXPECT_EQ(idRule->style()->getPropertyPriority("color"), "");

    // And typed into the style attribute
    EXPECT_TRUE(setDeclarationValue(page.edit("x"), nullptr, "color", "rgb(1, 2, 3) !important"));
    page.write("x");
    EXPECT_TRUE(sameColor(page.style("x")->color, 1, 2, 3));
    rules = page.rules("x");
    EXPECT_TRUE(rules.blocks[0].declarations[0].important);
    EXPECT_TRUE(rules.blocks[0].declarations[0].edited);
    EXPECT_EQ(rules.declaration(rules.winner("color")), &rules.blocks[0].declarations[0]);
}

TEST(DevToolsStyleRules, RenamingAPropertyKeepsItsPlace) {
    Page page;
    const CSSStyleRule* aRule = page.rule("x", ".a");
    EXPECT_TRUE(renameDeclaration(page.edit("x"), aRule, "padding", " Margin "));
    page.write("x");
    EXPECT_FLOAT_EQ(page.style("x")->paddingTop.value, 0);
    // .a beats both `p` rules for the top; #x still sets the other sides
    EXPECT_FLOAT_EQ(page.style("x")->marginTop.value, 1);
    EXPECT_FLOAT_EQ(page.style("x")->marginLeft.value, 3);
    ElementRules rules = page.rules("x");
    const RuleBlock* a = findBlock(rules.blocks, ".a");
    EXPECT_EQ(findDecl(a, "padding"), nullptr);
    ASSERT_NE(findDecl(a, "margin"), nullptr);
    EXPECT_TRUE(findDecl(a, "margin")->edited);
    EXPECT_EQ(a->declarations[1].property, "margin") << "it keeps its place in the block";
    EXPECT_EQ(aRule->style()->getPropertyValue("padding"), "1px");
    EXPECT_EQ(aRule->style()->getPropertyValue("margin"), "");
    EXPECT_FLOAT_EQ(page.style("y")->paddingTop.value, 1);

    // An invalid name is kept and shown, but sets nothing
    EXPECT_TRUE(renameDeclaration(page.edit("x"), aRule, "margin", "mar gin"));
    page.write("x");
    EXPECT_FLOAT_EQ(page.style("x")->marginTop.value, 8);
    EXPECT_FLOAT_EQ(page.style("x")->paddingTop.value, 0);
    rules = page.rules("x");
    const RuleDeclaration* invalid = findDecl(findBlock(rules.blocks, ".a"), "mar gin");
    ASSERT_NE(invalid, nullptr);
    EXPECT_TRUE(invalid->overridden);
    EXPECT_EQ(page.el("x")->getAttribute("style").find("mar gin"), std::string::npos);
}

TEST(DevToolsStyleRules, AddingToARuleRanksAfterItsDeclarations) {
    Page page;
    const CSSStyleRule* idRule = page.rule("x", "#x");
    EXPECT_TRUE(addDeclaration(page.edit("x"), idRule, "margin", "9px"));
    page.write("x");
    // After #x's margin-left/right/bottom, so it wins them; #x beats `p` for the top
    EXPECT_FLOAT_EQ(page.style("x")->marginLeft.value, 9);
    EXPECT_FLOAT_EQ(page.style("x")->marginBottom.value, 9);
    EXPECT_FLOAT_EQ(page.style("x")->marginTop.value, 9);
    ElementRules rules = page.rules("x");
    const RuleBlock* id = findBlock(rules.blocks, "#x");
    ASSERT_EQ(id->declarations.size(), 5u);
    EXPECT_EQ(id->declarations.back().property, "margin");
    EXPECT_TRUE(id->declarations.back().added);
    EXPECT_FALSE(id->declarations.back().edited);
    EXPECT_TRUE(findDecl(id, "margin-left")->overridden);
    EXPECT_EQ(rules.declaration(rules.winner("margin-top")), &id->declarations.back());
    EXPECT_EQ(idRule->style()->length(), 4u);
    // An empty value adds nothing
    EXPECT_FALSE(addDeclaration(page.edit("x"), idRule, "padding", "  "));
    EXPECT_FALSE(addDeclaration(page.edit("x"), idRule, " ", "1px"));
}

TEST(DevToolsStyleRules, AddingToTheStyleAttribute) {
    Page page;
    EXPECT_TRUE(addDeclaration(page.edit("x"), nullptr, "padding-left", "6px"));
    page.write("x");
    EXPECT_FLOAT_EQ(page.style("x")->paddingLeft.value, 6);
    EXPECT_FLOAT_EQ(page.style("x")->paddingTop.value, 1);
    EXPECT_EQ(page.el("x")->getAttribute("style"), "color: black; padding-left: 6px;");
    ElementRules rules = page.rules("x");
    ASSERT_EQ(rules.blocks[0].declarations.size(), 2u);
    EXPECT_TRUE(rules.blocks[0].declarations[1].added);
    EXPECT_FALSE(rules.blocks[0].declarations[1].overridden);
    EXPECT_FALSE(rules.blocks[0].declarations[0].edited);

    // An element without a style attribute
    EXPECT_FALSE(page.el("s")->hasAttribute("style"));
    EXPECT_TRUE(addDeclaration(page.edit("s"), nullptr, "color", "rgb(4, 5, 6)"));
    page.write("s");
    EXPECT_TRUE(sameColor(page.style("s")->color, 4, 5, 6));
    EXPECT_TRUE(removeDeclaration(page.edit("s"), nullptr, "color"));
    EXPECT_TRUE(page.edit("s").empty());
    page.write("s");
    EXPECT_FALSE(page.el("s")->hasAttribute("style"));
}

TEST(DevToolsStyleRules, DeletingDeclarations) {
    Page page;
    const CSSStyleRule* idRule = page.rule("x", "#x");
    EXPECT_TRUE(removeDeclaration(page.edit("x"), idRule, "margin-left"));
    EXPECT_FALSE(removeDeclaration(page.edit("x"), idRule, "margin-left"));
    page.write("x");
    EXPECT_FLOAT_EQ(page.style("x")->marginLeft.value, 0);    // p { margin: 0 }
    EXPECT_FLOAT_EQ(page.style("x")->marginRight.value, 3);
    EXPECT_FLOAT_EQ(page.style("y")->marginLeft.value, 0);
    ElementRules rules = page.rules("x");
    EXPECT_EQ(findDecl(findBlock(rules.blocks, "#x"), "margin-left"), nullptr);
    EXPECT_EQ(idRule->style()->getPropertyValue("margin-left"), "3px");

    // From the style attribute, with .b switched off: #x's color shows
    page.disable("x", page.rule("x", ".b"), "color");
    EXPECT_TRUE(removeDeclaration(page.edit("x"), nullptr, "color"));
    page.write("x");
    EXPECT_TRUE(sameColor(page.style("x")->color, 0, 0, 255));
    rules = page.rules("x");
    EXPECT_TRUE(rules.blocks[0].declarations.empty());
}

TEST(DevToolsStyleRules, AnEmptyValueDeletes) {
    Page page;
    const CSSStyleRule* aRule = page.rule("x", ".a");
    EXPECT_TRUE(setDeclarationValue(page.edit("x"), aRule, "padding", "   "));
    page.write("x");
    EXPECT_FLOAT_EQ(page.style("x")->paddingTop.value, 0);
    EXPECT_EQ(findDecl(findBlock(page.rules("x").blocks, ".a"), "padding"), nullptr);
    // An empty name too
    EXPECT_TRUE(renameDeclaration(page.edit("x"), aRule, "color", ""));
    EXPECT_EQ(findBlock(page.rules("x").blocks, ".a")->declarations.size(), 0u);
}

TEST(DevToolsStyleRules, EditingASwitchedOffDeclarationKeepsItOff) {
    Page page;
    const CSSStyleRule* idRule = page.rule("x", "#x");
    page.disable("x", idRule, "margin-left");
    EXPECT_TRUE(setDeclarationValue(page.edit("x"), idRule, "margin-left", "5px"));
    page.write("x");
    EXPECT_FLOAT_EQ(page.style("x")->marginLeft.value, 0);
    const RuleDeclaration* ml = findDecl(findBlock(page.rules("x").blocks, "#x"), "margin-left");
    EXPECT_TRUE(ml->disabled);
    EXPECT_TRUE(ml->edited);
    EXPECT_EQ(ml->value, "5px");

    // Switched back on, the edited value applies
    StyleEdit& edit = page.edit("x");
    edit.disabled.clear();
    page.write("x");
    EXPECT_FLOAT_EQ(page.style("x")->marginLeft.value, 5);
    // A renamed switched-off declaration stays off under its new name
    page.disable("x", idRule, "margin-right");
    EXPECT_TRUE(renameDeclaration(page.edit("x"), idRule, "margin-right", "padding-right"));
    page.write("x");
    EXPECT_FLOAT_EQ(page.style("x")->paddingRight.value, 1);   // .a's padding
    EXPECT_FLOAT_EQ(page.style("x")->marginRight.value, 0);    // p { margin: 0 }
    EXPECT_TRUE(findDecl(findBlock(page.rules("x").blocks, "#x"), "padding-right")->disabled);
}

TEST(DevToolsStyleRules, RevertRestoresTheOriginalAttributeExactly) {
    Page page;
    const std::string original = "color:black ;  margin-top:1px";
    page.el("x")->setAttribute("style", original);
    const CSSStyleRule* aRule = page.rule("x", ".a");
    const CSSStyleRule* idRule = page.rule("x", "#x");
    StyleEdit& edit = page.edit("x");
    setDeclarationValue(edit, aRule, "padding", "5px");
    addDeclaration(edit, idRule, "margin", "9px");
    renameDeclaration(edit, nullptr, "color", "background-color");
    edit.disabled.push_back({aRule, "color"});
    page.write("x");
    EXPECT_NE(page.el("x")->getAttribute("style"), original);
    EXPECT_FLOAT_EQ(page.style("x")->paddingTop.value, 5);

    revertStyleEdit(page.edit("x"));
    EXPECT_TRUE(page.edit("x").empty());
    EXPECT_EQ(styleAttributeFor(*page.css, page.el("x"), page.edit("x")), original);
    page.write("x");
    EXPECT_EQ(page.el("x")->getAttribute("style"), original);
    EXPECT_FLOAT_EQ(page.style("x")->paddingTop.value, 1);
    EXPECT_FLOAT_EQ(page.style("x")->marginTop.value, 1);

    // An edit put back by hand is no edit
    setDeclarationValue(page.edit("x"), aRule, "padding", "5px");
    setDeclarationValue(page.edit("x"), aRule, "padding", "1px");
    EXPECT_TRUE(page.edit("x").empty());
    EXPECT_EQ(styleAttributeFor(*page.css, page.el("x"), page.edit("x")), original);
}

TEST(DevToolsStyleRules, ShorthandEditsAndLonghands) {
    Page page;
    const CSSStyleRule* idRule = page.rule("x", "#x");
    const CSSStyleRule* aRule = page.rule("x", ".a");
    // #x { margin-left: 11px; margin: 4px (renamed from margin-right); margin-bottom: 3px }
    EXPECT_TRUE(setDeclarationValue(page.edit("x"), idRule, "margin-left", "11px"));
    EXPECT_TRUE(renameDeclaration(page.edit("x"), idRule, "margin-right", "margin"));
    EXPECT_TRUE(setDeclarationValue(page.edit("x"), idRule, "margin", "4px"));
    page.write("x");
    EXPECT_FLOAT_EQ(page.style("x")->marginLeft.value, 4);     // the later shorthand
    EXPECT_FLOAT_EQ(page.style("x")->marginRight.value, 4);
    EXPECT_FLOAT_EQ(page.style("x")->marginBottom.value, 3);   // the later longhand
    EXPECT_FLOAT_EQ(page.style("x")->marginTop.value, 4);      // #x beats site.css `p`
    ElementRules rules = page.rules("x");
    const RuleBlock* id = findBlock(rules.blocks, "#x");
    EXPECT_TRUE(findDecl(id, "margin-left")->overridden);
    EXPECT_FALSE(findDecl(id, "margin")->overridden);
    EXPECT_FALSE(findDecl(id, "margin-bottom")->overridden);
    EXPECT_TRUE(findDecl(findBlock(rules.blocks, "p", "site.css"), "margin-top")->overridden);

    // A shorthand value with four sides
    EXPECT_TRUE(setDeclarationValue(page.edit("x"), aRule, "padding", "1px 2px 3px 4px"));
    page.write("x");
    EXPECT_FLOAT_EQ(page.style("x")->paddingRight.value, 2);
    EXPECT_FLOAT_EQ(page.style("x")->paddingLeft.value, 4);
    // A longhand added to the style attribute beats the rule's shorthand for its side only
    EXPECT_TRUE(addDeclaration(page.edit("x"), nullptr, "padding-left", "9px"));
    page.write("x");
    EXPECT_FLOAT_EQ(page.style("x")->paddingLeft.value, 9);
    EXPECT_FLOAT_EQ(page.style("x")->paddingTop.value, 1);
    rules = page.rules("x");
    EXPECT_FALSE(findDecl(findBlock(rules.blocks, ".a"), "padding")->overridden);
    // y never saw any of it
    EXPECT_FLOAT_EQ(page.style("y")->marginLeft.value, 0);
    EXPECT_FLOAT_EQ(page.style("y")->paddingLeft.value, 1);
}
