// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file computed_style_tests.cpp
 * @brief window.getComputedStyle() for page scripts: resolved values from a
 *        real CSS engine (author rules, inline styles, inheritance), camelCase
 *        and CSS-name access, the read-only CSSStyleDeclaration interface,
 *        detached elements, and reads after a style change.
 */

#include <gtest/gtest.h>

#include "browser/dom.hpp"
#include "css/computed_style_values.hpp"
#include "css/css_engine.hpp"
#include "css/ua_stylesheet.h"
#include "html/html_parser.hpp"
#include "scripting/script_context.hpp"

#include <functional>
#include <memory>
#include <string>

// Viewport size the layout engine reads (zepra_browser.cpp defines these)
int g_width = 1280;
int g_height = 800;

using namespace Zepra::WebCore;

namespace {

const char* kPage = R"(<!DOCTYPE html><html><head><style>
#box { color: rgb(10, 20, 30); background-color: #00ff00; width: 120px; padding-left: 4px; font-size: 20px }
.faded { opacity: 0.5; background-color: rgba(255, 0, 0, 0.5) }
.flex { display: flex; flex-direction: column }
</style></head><body>
<div id="box" style="width: 200px; margin-top: 7px"><span id="child">x</span></div>
<p id="plain">p</p>
</body></html>)";

/// A parsed page with its CSS engine and a script context reading styles
/// from it, restyled after DOM changes as the browser does
struct Page {
    std::unique_ptr<DOMDocument> doc;
    std::unique_ptr<CSSEngine> css;
    std::unique_ptr<ScriptContext> script;
    uint64_t styledVersion = 0;

    explicit Page(const std::string& html, bool withSource = true) {
        HTMLParser parser;
        doc = parser.parse(html);
        css = std::make_unique<CSSEngine>();
        css->initialize(doc.get());
        css->addStyleSheet(ZepraUAStylesheet::getStylesheet(), StyleOrigin::UserAgent);
        std::function<void(DOMNode*)> addStyles = [&](DOMNode* node) {
            auto* el = dynamic_cast<DOMElement*>(node);
            if (el && (el->tagName() == "style" || el->tagName() == "STYLE")) {
                css->addStyleSheet(el->textContent(), StyleOrigin::Author);
                return;
            }
            for (const auto& child : node->childNodes()) addStyles(child.get());
        };
        addStyles(doc.get());
        css->computeStyles();
        styledVersion = domMutationVersion();

        script = std::make_unique<ScriptContext>();
        script->initialize(doc.get());
        if (withSource) {
            script->setComputedStyleSource([this](DOMElement* element) -> const CSSComputedStyle* {
                if (domMutationVersion() != styledVersion) {
                    css->computeStyles();
                    styledVersion = domMutationVersion();
                }
                return css->getComputedStyle(element);
            });
            script->setPseudoElementStyleSource(
                [this](DOMElement* element, const std::string& pseudo) -> const CSSComputedStyle* {
                    if (domMutationVersion() != styledVersion) {
                        css->computeStyles();
                        styledVersion = domMutationVersion();
                    }
                    return css->getPseudoElementStyle(element, pseudo);
                });
        }
    }

    /// Runs code and returns its completion value as a string
    std::string eval(const std::string& code) {
        ScriptResult r = script->evaluate(code, "test.js");
        EXPECT_TRUE(r.success) << r.error << "\n  in: " << code;
        return r.success ? r.value : "<error: " + r.error + ">";
    }
};

}  // namespace

TEST(ComputedStyle, InlineStyleBeatsAuthorRule) {
    Page page(kPage);
    EXPECT_EQ(page.eval("getComputedStyle(document.getElementById('box')).width"), "200px");
    EXPECT_EQ(page.eval("getComputedStyle(document.getElementById('box')).marginTop"), "7px");
    EXPECT_EQ(page.eval("getComputedStyle(document.getElementById('box')).paddingLeft"), "4px");
    EXPECT_EQ(page.eval("getComputedStyle(document.getElementById('box')).backgroundColor"), "rgb(0, 255, 0)");
}

TEST(ComputedStyle, InheritedColorAndFontSize) {
    Page page(kPage);
    EXPECT_EQ(page.eval("getComputedStyle(document.getElementById('child')).color"), "rgb(10, 20, 30)");
    EXPECT_EQ(page.eval("getComputedStyle(document.getElementById('child')).fontSize"), "20px");
    // background-color does not inherit
    EXPECT_EQ(page.eval("getComputedStyle(document.getElementById('child')).backgroundColor"), "transparent");
}

TEST(ComputedStyle, CamelCaseAndCssNames) {
    Page page(kPage);
    EXPECT_EQ(page.eval("var cs = getComputedStyle(document.getElementById('box'));"
                        "[cs.backgroundColor, cs.getPropertyValue('background-color'), cs['background-color'],"
                        " cs.getPropertyValue('BACKGROUND-COLOR')].join('|')"),
              "rgb(0, 255, 0)|rgb(0, 255, 0)|rgb(0, 255, 0)|rgb(0, 255, 0)");
    EXPECT_EQ(page.eval("[cs.cssFloat, cs.getPropertyValue('float'), cs.display].join('|')"), "none|none|block");
    // Not a CSS name, or not a property computed here
    EXPECT_EQ(page.eval("[cs.getPropertyValue('backgroundColor'), cs.getPropertyValue('no-such-thing'),"
                        " typeof cs.noSuchThing].join('|')"), "||undefined");
}

TEST(ComputedStyle, LengthAndItemListTheResolvedProperties) {
    Page page(kPage);
    DOMElement* box = page.doc->getElementById("box");
    auto expected = computedStyleProperties(*page.css->getComputedStyle(box));
    ASSERT_FALSE(expected.empty());
    page.eval("var cs = getComputedStyle(document.getElementById('box'))");
    EXPECT_EQ(page.eval("cs.length"), std::to_string(expected.size()));
    EXPECT_EQ(page.eval("cs.item(0) + ' ' + cs[0]"), expected[0].first + " " + expected[0].first);
    EXPECT_EQ(page.eval("cs.item(cs.length - 1)"), expected.back().first);
    EXPECT_EQ(page.eval("cs.item(cs.length) + '|' + cs[cs.length]"), "|undefined");
    // Every value is the one the DevTools Computed pane shows
    std::string all;
    for (const auto& [name, value] : expected) all += name + "=" + value + ";";
    EXPECT_EQ(page.eval("var out = ''; for (var i = 0; i < cs.length; i++) out += cs[i] + '=' + cs.getPropertyValue(cs[i]) + ';'; out"),
              all);
}

TEST(ComputedStyle, InterfaceAndConstantMembers) {
    Page page(kPage);
    EXPECT_EQ(page.eval("var cs = getComputedStyle(document.getElementById('box'));"
                        "[cs instanceof CSSStyleDeclaration, document.body.style instanceof CSSStyleDeclaration,"
                        " String(cs), JSON.stringify(cs.cssText), JSON.stringify(cs.getPropertyPriority('color')),"
                        " cs.parentRule].join('|')"),
              "true|true|[object CSSStyleDeclaration]|\"\"|\"\"|");
    EXPECT_EQ(page.eval("window.getComputedStyle === getComputedStyle"), "true");
    EXPECT_EQ(page.eval("window.getComputedStyle(document.getElementById('child')).color"), "rgb(10, 20, 30)");
}

TEST(ComputedStyle, ReadOnly) {
    Page page(kPage);
    page.eval("var cs = getComputedStyle(document.getElementById('box'));"
              "function attempt(f) { try { f(); return 'no error'; } catch (e) { return e.name + ': ' + e.message; } }");
    EXPECT_EQ(page.eval("attempt(function () { cs.setProperty('color', 'red'); })"),
              "NoModificationAllowedError: Failed to execute 'setProperty' on 'CSSStyleDeclaration': "
              "These styles are computed, and therefore the 'color' property is read-only.");
    EXPECT_EQ(page.eval("attempt(function () { cs.removeProperty('width'); })").substr(0, 27),
              "NoModificationAllowedError:");
    EXPECT_EQ(page.eval("attempt(function () { cs.backgroundColor = 'red'; })"),
              "NoModificationAllowedError: Failed to set the 'backgroundColor' property on 'CSSStyleDeclaration': "
              "These styles are computed, and therefore the 'background-color' property is read-only.");
    EXPECT_EQ(page.eval("attempt(function () { cs.cssText = 'color: red'; })").substr(0, 27),
              "NoModificationAllowedError:");
    // Nothing changed, and an expando that is not a CSS property is kept
    EXPECT_EQ(page.eval("cs.color + '|' + document.getElementById('box').getAttribute('style')"),
              "rgb(10, 20, 30)|width: 200px; margin-top: 7px");
    EXPECT_EQ(page.eval("cs.myFlag = 3; cs.myFlag"), "3");
}

TEST(ComputedStyle, NonElementArgumentThrowsTypeError) {
    Page page(kPage);
    for (const char* arg : {"", "document", "{}", "null", "document.createTextNode('t')", "'box'"}) {
        EXPECT_EQ(page.eval(std::string("(function () { try { getComputedStyle(") + arg +
                            "); return 'no error'; } catch (e) { return e instanceof TypeError; } })()"),
                  "true") << arg;
    }
}

TEST(ComputedStyle, DetachedElementReadsEmpty) {
    Page page(kPage);
    EXPECT_EQ(page.eval("var d = document.createElement('div'); d.style.width = '10px';"
                        "var cs = getComputedStyle(d); JSON.stringify([cs.width, cs.length, cs.item(0)])"),
              "[\"\",0,\"\"]");
    // Taken out of the document after the style object was made
    EXPECT_EQ(page.eval("var p = document.getElementById('plain'); var pcs = getComputedStyle(p);"
                        "var before = pcs.display; p.remove(); before + '|' + JSON.stringify(pcs.display) + '|' + pcs.length"),
              "block|\"\"|0");
    // Back in: styled again
    EXPECT_EQ(page.eval("document.body.appendChild(p); pcs.display"), "block");
}

TEST(ComputedStyle, PseudoElementArgument) {
    Page page(kPage);
    // null, "" and a value without a colon mean the element itself
    EXPECT_EQ(page.eval("var box = document.getElementById('box');"
                        "[getComputedStyle(box, null).width, getComputedStyle(box, '').width,"
                        " getComputedStyle(box, 'before').width].join('|')"),
              "200px|200px|200px");
    // Not a pseudo-element: an empty declaration
    EXPECT_EQ(page.eval("[getComputedStyle(box, '::nope').length, getComputedStyle(box, ':hover').length,"
                        " getComputedStyle(box, ':marker').length].join('|')"), "0|0|0");
}

const char* kPseudoPage = R"HTML(<!DOCTYPE html><html><head><style>
#box { color: rgb(10, 20, 30); font-size: 20px; width: 300px }
#box::before { content: '\2192  '; color: rgb(200, 0, 0); padding-left: 1em }
#box:after { content: "end"; display: block; margin-top: 5px }
.tip::after { content: attr(data-tip) !important; }
.tip::after { content: "lost"; background-color: #0000ff }
p::before, li::before { content: counter(item) ". " }
#box::first-line { font-weight: 700 }
div:hover::before { color: rgb(0, 0, 1) }
</style></head><body>
<div id="box" style="color: rgb(1, 1, 1)">text</div>
<span class="tip" id="tip" data-tip="hi">t</span>
<p id="para">p</p>
<b id="plain">b</b>
</body></html>)HTML";

TEST(ComputedStyle, PseudoElementsTakeTheirOwnRules) {
    Page page(kPseudoPage);
    page.eval("var box = document.getElementById('box'); var b = getComputedStyle(box, '::before');"
              "var a = getComputedStyle(box, '::after')");
    // Their own rules, inline by default, inheriting from the element (whose
    // style attribute does not reach them)
    EXPECT_EQ(page.eval("[b.content, b.color, b.display, b.fontSize, b.paddingLeft].join('|')"),
              "\"\u2192 \"|rgb(200, 0, 0)|inline|20px|20px");
    EXPECT_EQ(page.eval("[a.content, a.color, a.display, a.marginTop, a.width].join('|')"),
              "\"end\"|rgb(1, 1, 1)|block|5px|auto");
    // Legacy one-colon argument, any case
    EXPECT_EQ(page.eval("getComputedStyle(box, ':BEFORE').color"), "rgb(200, 0, 0)");
    EXPECT_EQ(page.eval("getComputedStyle(box, '::first-line').fontWeight"), "700");
    // The element's own style is untouched by the pseudo-element rules
    EXPECT_EQ(page.eval("var cs = getComputedStyle(box); [cs.color, cs.display, cs.content].join('|')"),
              "rgb(1, 1, 1)|block|");
}

TEST(ComputedStyle, PseudoElementCascade) {
    Page page(kPseudoPage);
    // !important beats a later rule; the later rule still gives the background
    EXPECT_EQ(page.eval("var t = getComputedStyle(document.getElementById('tip'), '::after');"
                        "t.content + '|' + t.backgroundColor"), "attr(data-tip)|rgb(0, 0, 255)");
    // A selector list
    EXPECT_EQ(page.eval("getComputedStyle(document.getElementById('para'), '::before').content"),
              "counter(item) \". \"");
    // No rule: content computes to none on ::before/::after, normal elsewhere
    EXPECT_EQ(page.eval("var p = document.getElementById('plain');"
                        "[getComputedStyle(p, '::before').content, getComputedStyle(p, '::after').content,"
                        " getComputedStyle(p, '::before').display].join('|')"), "none|none|inline");
    // The cascade follows class changes
    EXPECT_EQ(page.eval("var pcs = getComputedStyle(p, '::after'); p.className = 'tip'; pcs.content"),
              "attr(data-tip)");
}

TEST(ComputedStyle, PseudoElementSelectorsDoNotMatchTheElement) {
    Page page(kPseudoPage);
    // #box::before's padding and color stay on the pseudo-element
    EXPECT_EQ(page.eval("var cs = getComputedStyle(document.getElementById('box'));"
                        "cs.paddingLeft + '|' + cs.marginTop"), "0px|0px");
}

TEST(ComputedStyle, ReadsFollowStyleChanges) {
    Page page(kPage);
    page.eval("var box = document.getElementById('box'); var child = document.getElementById('child');"
              "var cs = getComputedStyle(box); var ccs = getComputedStyle(child)");
    EXPECT_EQ(page.eval("cs.width"), "200px");
    // The same object is live
    EXPECT_EQ(page.eval("box.style.width = '250px'; cs.width"), "250px");
    EXPECT_EQ(page.eval("box.removeAttribute('style'); cs.width + '|' + cs.marginTop"), "120px|0px");
    EXPECT_EQ(page.eval("child.className = 'faded'; ccs.opacity + '|' + ccs.backgroundColor"),
              "0.5|rgba(255, 0, 0, 0.50)");
    // Inherited values follow the parent
    EXPECT_EQ(page.eval("box.style.color = 'rgb(1, 2, 3)'; ccs.color"), "rgb(1, 2, 3)");
    // Properties that only apply to flex containers appear with display: flex
    EXPECT_EQ(page.eval("var before = cs.length; box.className = 'flex';"
                        "[cs.display, cs.flexDirection, cs.length > before].join('|')"),
              "flex|column|true");
}

TEST(ComputedStyle, WithoutAStyleSourceValuesAreEmpty) {
    Page page(kPage, false);
    EXPECT_EQ(page.eval("var cs = getComputedStyle(document.getElementById('box'));"
                        "JSON.stringify([cs.width, cs.getPropertyValue('color'), cs.length])"),
              "[\"\",\"\",0]");
}

TEST(ComputedStyle, BorderStyleAndUnstyledWidths) {
    Page page(kPage);
    page.eval("var box = document.getElementById('box'); var cs = getComputedStyle(box)");
    EXPECT_EQ(page.eval("box.style.border = '2px dashed red'; [cs.borderTopWidth, cs.borderTopStyle].join(' ')"),
              "2px dashed");
    // A border without a style has no width (CSS Backgrounds 3 §3.3)
    EXPECT_EQ(page.eval("box.style.border = ''; box.style.borderWidth = '3px'; cs.borderTopWidth"), "0px");
    EXPECT_EQ(page.eval("box.style.borderStyle = 'dotted solid'; [cs.borderTopStyle, cs.borderRightStyle,"
                        " cs.borderBottomStyle, cs.borderLeftStyle, cs.borderTopWidth].join(' ')"),
              "dotted solid dotted solid 3px");
    // The shorthand's omitted width is medium
    EXPECT_EQ(page.eval("box.removeAttribute('style'); box.style.border = 'solid'; cs.borderLeftWidth"), "3px");
    EXPECT_EQ(page.eval("box.style.borderLeftStyle = 'none'; [cs.borderLeftWidth, cs.borderRightWidth].join(' ')"),
              "0px 3px");
}

TEST(ComputedStyle, InsetSetsAllFourOffsets) {
    Page page(kPage);
    page.eval("var box = document.getElementById('box'); var cs = getComputedStyle(box);"
              "box.style.position = 'absolute'");
    auto offsets = [&](const char* inset) {
        return page.eval(std::string("box.style.inset = '") + inset +
                         "'; [cs.top, cs.right, cs.bottom, cs.left].join(' ')");
    };
    EXPECT_EQ(offsets("0"), "0px 0px 0px 0px");
    EXPECT_EQ(offsets("1px 2px"), "1px 2px 1px 2px");
    EXPECT_EQ(offsets("1px 2px 3px"), "1px 2px 3px 2px");
    EXPECT_EQ(offsets("1px 2px 3px 4px"), "1px 2px 3px 4px");
    EXPECT_EQ(page.eval("box.style.inset = ''; box.style.insetInline = '5px 6px';"
                        "[cs.left, cs.right].join(' ')"), "5px 6px");
}
