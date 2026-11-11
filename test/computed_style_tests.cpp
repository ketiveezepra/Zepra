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
#include "browser/style_change_log.hpp"
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

TEST(ComputedStyle, PercentageBorderRadiusIsKept) {
    Page page(kPage);
    page.eval("var box = document.getElementById('box'); var cs = getComputedStyle(box)");
    EXPECT_EQ(page.eval("box.style.borderRadius = '50%'; cs.borderRadius"), "50%");
    EXPECT_EQ(page.eval("box.style.borderRadius = '8px'; cs.borderRadius"), "8px");
}

TEST(ComputedStyle, LetterSpacingParsesAndInherits) {
    Page page(kPage);
    page.eval("var box = document.getElementById('box'); var child = document.getElementById('child');"
              "var cs = getComputedStyle(box); var ccs = getComputedStyle(child)");
    EXPECT_EQ(page.eval("box.style.letterSpacing = '1.5px'; cs.letterSpacing + '|' + ccs.letterSpacing"),
              "1.5px|1.5px");
    EXPECT_EQ(page.eval("box.style.letterSpacing = 'normal'; cs.letterSpacing"),
              page.eval("box.style.letterSpacing = ''; cs.letterSpacing"));
}

// ---------------------------------------------------------------------------
// Animations origin: CSS animations' end states and element.animate()
// ---------------------------------------------------------------------------

namespace {

const char* kAnimationPage = R"(<!DOCTYPE html><html><head><style>
@keyframes fadeUp { from { opacity: 0 } 50% { color: rgb(1, 2, 3) } to { opacity: 1; color: rgb(120, 200, 255) } }
@keyframes "quoted" { 0%, 100% { opacity: 0.25 } }
.e { opacity: 0; animation: fadeUp 0.3s ease-out forwards }
.inf { opacity: 0; animation: fadeUp 2s infinite both }
.none { opacity: 0; animation: fadeUp 0.2s }
.long { opacity: 0; animation-name: fadeUp; animation-duration: 0.2s; animation-fill-mode: both }
.rev { opacity: 0.5; animation: fadeUp 1s 2 alternate forwards }
.imp { opacity: 0.3 !important; animation: fadeUp 1s forwards }
.q { opacity: 0; animation: quoted 1s forwards }
</style></head><body>
<div id="e" class="e">e</div><div id="inf" class="inf">i</div><div id="none" class="none">n</div>
<div id="long" class="long">l</div><div id="rev" class="rev">r</div><div id="imp" class="imp">m</div>
<div id="q" class="q">q</div>
<div id="w" style="opacity: 0">w</div>
</body></html>)";

}  // namespace

TEST(Animations, FinishedFillingCssAnimationsKeepTheirEndKeyframe) {
    Page page(kAnimationPage);
    auto op = [&](const char* id) { return page.eval(std::string("getComputedStyle(document.getElementById('") + id + "')).opacity"); };
    EXPECT_EQ(op("e"), "1");
    EXPECT_EQ(page.eval("getComputedStyle(document.getElementById('e')).color"), "rgb(120, 200, 255)");
    EXPECT_EQ(op("long"), "1");       // the longhands
    EXPECT_EQ(op("q"), "0.25");       // a quoted name, a selector list
    // An infinite animation never ends; fill-mode none drops the effect
    EXPECT_EQ(op("inf"), "0");
    EXPECT_EQ(op("none"), "0");
    // Two alternate iterations end where the first began: the from keyframe
    EXPECT_EQ(op("rev"), "0");
    // !important beats the animations origin
    EXPECT_EQ(op("imp"), "0.3");
}

TEST(Animations, ElementAnimateAppliesItsEffectWithoutTouchingTheStyleAttribute) {
    Page page(kAnimationPage);
    page.eval("var w = document.getElementById('w');"
              "var a = w.animate([{opacity: 0}, {opacity: 1}], {duration: 1000, fill: 'forwards'});"
              "a.pause(); a.currentTime = 500;");
    EXPECT_EQ(page.eval("getComputedStyle(w).opacity"), "0.5");
    EXPECT_EQ(page.eval("a.playState + '|' + w.getAnimations().length + '|' + document.getAnimations().length"), "paused|1|1");
    page.eval("a.finish()");
    EXPECT_EQ(page.eval("a.playState + '|' + getComputedStyle(w).opacity + '|' + w.getAttribute('style')"),
              "finished|1|opacity: 0");
    page.eval("a.cancel()");
    EXPECT_EQ(page.eval("a.playState + '|' + getComputedStyle(w).opacity + '|' + w.getAnimations().length"), "idle|0|0");
}

TEST(Animations, TimingAndKeyframesAreValidatedAndInterpolated) {
    Page page(kAnimationPage);
    page.eval("var w = document.getElementById('w');"
              "var a = w.animate({opacity: [0, 1], color: ['#ff0000', 'rgb(0, 0, 255)']},"
              "                  {duration: 1000, delay: 100, iterations: 2, direction: 'alternate'});"
              "a.pause();");
    // In the delay, with fill auto (none): no effect
    page.eval("a.currentTime = 50");
    EXPECT_EQ(page.eval("getComputedStyle(w).opacity"), "0");
    // Second iteration, alternate: running backwards
    page.eval("a.currentTime = 1100 + 250");
    EXPECT_EQ(page.eval("getComputedStyle(w).opacity"), "0.75");
    EXPECT_EQ(page.eval("a.effect.getComputedTiming().currentIteration + '|' + a.effect.getComputedTiming().activeDuration"), "1|2000");
    page.eval("a.currentTime = 600");
    EXPECT_EQ(page.eval("getComputedStyle(w).color"), "rgb(128, 0, 128)");
    // Invalid input throws TypeError
    EXPECT_EQ(page.eval("var r = []; ["
                        " function () { w.animate([{opacity: 1, offset: 0.8}, {opacity: 0, offset: 0.2}], 10) },"
                        " function () { w.animate([{opacity: 1}], {duration: -1}) },"
                        " function () { w.animate([{opacity: 1}], {easing: 'bogus'}) }"
                        "].forEach(function (f) { try { f(); r.push('ok') } catch (e) { r.push(e.name) } }); r.join(',')"),
              "TypeError,TypeError,TypeError");
    EXPECT_EQ(page.eval("typeof Animation + typeof KeyframeEffect + typeof document.timeline.currentTime"),
              "functionfunctionnumber");
}

// ---------------------------------------------------------------------------
// Transforms in getComputedStyle, paint-only style changes, CSS animations
// and transitions over time
// ---------------------------------------------------------------------------

#include "css/css_animations.hpp"

namespace {

const char* kTransformPage = R"(<!DOCTYPE html><html><head><style>
#t { transform: rotate(90deg) translateX(10px); transform-origin: left top }
#i { translate: 0 100%; scale: 1.5 }
@keyframes slide { from { transform: translateX(0) } to { transform: translateX(200px) } }
@keyframes fade { 50% { opacity: 0.5 } }
#a { animation: slide 1s linear 2 alternate both }
#f { animation: fade 2s linear infinite }
#tr { transition: opacity 1s linear, background-color 1s linear; opacity: 1; background-color: rgb(0, 0, 0) }
#tr.on { opacity: 0; background-color: rgb(200, 100, 0) }
</style></head><body>
<div id="t">t</div><div id="i">i</div><div id="n" style="color: red">n</div>
<div id="a">a</div><div id="f">f</div><div id="tr">tr</div>
</body></html>)";

}  // namespace

TEST(Transforms, ComputedTransformIsAMatrix) {
    Page page(kTransformPage);
    auto cs = [&](const char* id, const char* prop) {
        return page.eval(std::string("getComputedStyle(document.getElementById('") + id + "'))." + prop);
    };
    EXPECT_EQ(cs("t", "transform"), "matrix(0, 1, -1, 0, 0, 10)");
    EXPECT_EQ(cs("t", "transformOrigin"), "left top");
    EXPECT_EQ(cs("n", "transform"), "none");
    EXPECT_EQ(cs("n", "translate"), "none");
    // The individual properties stay as declared; transform does not include them
    EXPECT_EQ(cs("i", "translate"), "0 100%");
    EXPECT_EQ(cs("i", "scale"), "1.5");
    EXPECT_EQ(cs("i", "transform"), "none");
    page.eval("document.getElementById('n').style.transform = 'translate(5px, 6px)'");
    EXPECT_EQ(cs("n", "transform"), "matrix(1, 0, 0, 1, 5, 6)");
}

TEST(Transforms, OpacityAndTransformWritesArePaintOnly) {
    Page page(kTransformPage);
    std::vector<DOMElement*> heard;
    setPaintOnlyChangeListener([&](DOMElement* e) {
        heard.push_back(e);
        page.css->invalidate(e);
    });
    takePaintOnlyChanges();
    uint64_t before = domMutationVersion();
    page.eval("var n = document.getElementById('n'); n.style.opacity = '0.25'; n.style.transform = 'scale(2)';"
              "n.style.setProperty('opacity', '0.5')");
    EXPECT_EQ(domMutationVersion(), before);   // no restyle of the page
    auto changed = takePaintOnlyChanges();
    ASSERT_EQ(changed.size(), 1u);              // the element, once
    EXPECT_EQ(changed[0]->getAttribute("id"), "n");
    EXPECT_EQ(heard.size(), 3u);
    EXPECT_EQ(page.eval("n.getAttribute('style')"), "color: red; opacity: 0.5; transform: scale(2);");
    EXPECT_EQ(page.eval("getComputedStyle(n).opacity + '|' + getComputedStyle(n).transform"),
              "0.5|matrix(2, 0, 0, 2, 0, 0)");
    // Anything else restyles as before
    page.eval("n.style.width = '10px'");
    EXPECT_NE(domMutationVersion(), before);
    // Adding the style attribute changes what [style] matches: a full change
    before = domMutationVersion();
    page.eval("document.getElementById('t').style.opacity = '0.5'");
    EXPECT_NE(domMutationVersion(), before);
    // ...unless no sheet selects on it
    EXPECT_FALSE(page.css->usesStyleAttributeSelectors());
    setStyleAttributeSelectorsInUse(false);
    before = domMutationVersion();
    page.eval("document.getElementById('i').style.transform = 'scale(3)'");
    EXPECT_EQ(domMutationVersion(), before);
    setStyleAttributeSelectorsInUse(true);
    page.css->addStyleSheet("div[style*=red] { color: blue }", StyleOrigin::Author);
    EXPECT_TRUE(page.css->usesStyleAttributeSelectors());
    setPaintOnlyChangeListener(nullptr);
    takePaintOnlyChanges();
    // Without a listener, paint-only writes restyle like any other
    before = domMutationVersion();
    page.eval("n.style.opacity = '0.75'");
    EXPECT_NE(domMutationVersion(), before);
}

TEST(CssAnimations, EasingAndInterpolation) {
    EXPECT_DOUBLE_EQ(cssEasing("linear", 0.3), 0.3);
    EXPECT_NEAR(cssEasing("ease", 0.5), 0.8024, 1e-3);
    EXPECT_NEAR(cssEasing("cubic-bezier(0.4, 0, 0.2, 1)", 0.5), 0.7755, 1e-3);
    EXPECT_DOUBLE_EQ(cssEasing("steps(4, end)", 0.3), 0.25);
    EXPECT_DOUBLE_EQ(cssEasing("steps(4, start)", 0.3), 0.5);
    EXPECT_DOUBLE_EQ(cssEasing("step-end", 0.99), 0.0);
    EXPECT_DOUBLE_EQ(cssTimeMs("0.3s"), 300);
    EXPECT_DOUBLE_EQ(cssTimeMs("150ms"), 150);
    EXPECT_EQ(interpolateCssValue("width", "10px", "30px", 0.5), "20px");
    EXPECT_EQ(interpolateCssValue("transform", "translateX(0)", "translateX(200px)", 0.25), "translateX(50px)");
    EXPECT_EQ(interpolateCssValue("transform", "none", "translate(10px, 20%) scale(3)", 0.5),
              "translate(5px, 10%) scale(2)");
    EXPECT_EQ(interpolateCssValue("color", "rgb(0, 0, 0)", "rgb(200, 100, 0)", 0.5), "rgba(100, 50, 0, 1)");
    // Premultiplied: fading in from transparent keeps the target colour
    EXPECT_EQ(interpolateCssValue("background-color", "rgba(0, 0, 0, 0)", "rgb(200, 0, 0)", 0.5),
              "rgba(200, 0, 0, 0.5)");
    // Different shapes flip at the midpoint
    EXPECT_EQ(interpolateCssValue("display", "block", "none", 0.4), "block");
    EXPECT_EQ(interpolateCssValue("display", "block", "none", 0.6), "none");
}

TEST(CssAnimations, AnimationsRunOverTimeAndSendTheirEvents) {
    setCssAnimationsDriven(true);
    {
        Page page(kTransformPage);
        DOMElement* a = page.doc->getElementById("a");
        DOMElement* f = page.doc->getElementById("f");
        CSSAnimationController controller;
        controller.update(page.doc.get(), page.css->resolver(), 1000);
        controller.tick(1250);
        EXPECT_EQ(a->cssAnimationStyle(), "transform: translateX(50px);");
        // 50% of fade is 0.5; the implicit 0% and 100% are the element's own 1
        EXPECT_EQ(f->cssAnimationStyle(), "opacity: 0.875;");
        controller.tick(2500);   // second iteration, alternate: back from the end
        EXPECT_EQ(a->cssAnimationStyle(), "transform: translateX(100px);");
        controller.tick(3500);   // finished, fill both: where the last iteration ends
        EXPECT_EQ(a->cssAnimationStyle(), "transform: translateX(0px);");
        std::vector<std::string> events;
        for (const auto& e : controller.takeEvents())
            if (e.target == a) events.push_back(e.type + ":" + e.name);
        EXPECT_EQ(events, (std::vector<std::string>{"animationstart:slide", "animationiteration:slide",
                                                    "animationend:slide"}));
        // The cascade applies the animated values (the animations origin)
        page.css->computeStyles();
        EXPECT_EQ(page.eval("getComputedStyle(document.getElementById('f')).opacity"), "0.75");
        controller.tick(4000);   // fade: 3000 ms in, half way through its second cycle
        page.css->computeStyles();
        EXPECT_EQ(page.eval("getComputedStyle(document.getElementById('f')).opacity"), "0.5");
        // Removing the animation drops its values
        page.eval("document.getElementById('a').style.animation = 'none'");
        page.css->computeStyles();
        controller.update(page.doc.get(), page.css->resolver(), 4100);
        controller.tick(4100);
        EXPECT_EQ(a->cssAnimationStyle(), "");
    }
    setCssAnimationsDriven(false);
}

TEST(CssAnimations, TransitionsStartFromTheOldValue) {
    setCssAnimationsDriven(true);
    {
        Page page(kTransformPage);
        DOMElement* tr = page.doc->getElementById("tr");
        CSSAnimationController controller;
        controller.update(page.doc.get(), page.css->resolver(), 0);
        controller.tick(0);
        EXPECT_EQ(tr->cssAnimationStyle(), "");   // nothing changed yet
        page.eval("document.getElementById('tr').className = 'on'");
        page.css->computeStyles();
        controller.update(page.doc.get(), page.css->resolver(), 1000);
        controller.tick(1500);
        EXPECT_EQ(tr->cssAnimationStyle(), "background-color: rgba(100, 50, 0, 1); opacity: 0.5;");
        // Reversed half way: back from where it is
        page.eval("document.getElementById('tr').className = ''");
        page.css->computeStyles();
        controller.update(page.doc.get(), page.css->resolver(), 1500);
        controller.tick(2000);
        EXPECT_EQ(tr->cssAnimationStyle(), "background-color: rgba(50, 25, 0, 1); opacity: 0.75;");
        controller.tick(2600);
        EXPECT_EQ(tr->cssAnimationStyle(), "");
        std::vector<std::string> events;
        for (const auto& e : controller.takeEvents())
            if (e.name == "opacity") events.push_back(e.type);
        EXPECT_EQ(events, (std::vector<std::string>{"transitionrun", "transitionstart", "transitionrun",
                                                    "transitionstart", "transitionend"}));
    }
    setCssAnimationsDriven(false);
}

// ============================================================================
// Targeted restyles: a style pass drops only the styles a change can reach
// ============================================================================

namespace {

const char* kRestylePage = R"(<!DOCTYPE html><html><head><style>
.on { color: rgb(1, 0, 0) }
.wrap .item { color: rgb(2, 0, 0) }
.mark + .item { color: rgb(3, 0, 0) }
li:first-child { color: rgb(4, 0, 0) }
[data-k="v"] { color: rgb(5, 0, 0) }
#solo { color: rgb(6, 0, 0) }
.tint { background-color: rgb(9, 9, 9); font-size: 30px }
input:checked + label { color: rgb(7, 0, 0) }
.zzz ~ p { font-size: 22px }
:not(.wrap) > .mark { background-color: rgb(4, 4, 4) }
p:empty { font-size: 9px }
li:nth-child(2n of .on) { font-size: 13px }
.on > :is(.item, .zzz) span { background-color: rgb(6, 6, 6) }
</style></head><body>
<div id="w"><p id="a" class="item">a</p><p id="b" class="item">b</p><p id="c">c<span id="deep">d</span></p></div>
<ul id="list"><li id="l1">1</li><li id="l2">2</li></ul>
<div id="x"><span id="y">y</span></div>
<input id="cb" type="checkbox"><label id="lab">l</label>
</body></html>)";

struct RestylePage {
    std::unique_ptr<DOMDocument> doc;
    std::unique_ptr<CSSEngine> css;

    RestylePage() {
        HTMLParser parser;
        doc = parser.parse(kRestylePage);
        css = std::make_unique<CSSEngine>();
        css->initialize(doc.get());
        css->addStyleSheet(ZepraUAStylesheet::getStylesheet(), StyleOrigin::UserAgent);
        for (DOMElement* style : doc->getElementsByTagName("style")) css->addStyleSheet(style->textContent());
        pass();
    }
    /// A style pass and every element's style read, as a layout would
    std::string pass() {
        css->computeStyles();
        return snapshot();
    }
    std::string snapshot() {
        std::string out;
        std::function<void(DOMNode*)> walk = [&](DOMNode* node) {
            for (const auto& child : node->childNodes()) {
                auto* el = dynamic_cast<DOMElement*>(child.get());
                if (!el) continue;
                const CSSComputedStyle* s = css->getComputedStyle(el);
                out += el->tagName() + "#" + el->getAttribute("id") + ":" + std::to_string((int)s->color.r) + "/" +
                       std::to_string((int)s->fontSize) + "/" + std::to_string((int)s->backgroundColor.r) + " ";
                walk(el);
            }
        };
        walk(doc.get());
        return out;
    }
    /// The same page restyled from scratch
    std::string reference() {
        css->invalidate();
        return snapshot();
    }
    int red(const char* id) { return (int)css->getComputedStyle(doc->getElementById(id))->color.r; }
};

}  // namespace

TEST(Restyle, ChangesReachOnlyWhatTheirSelectorsSee) {
    RestylePage page;
    // A class no selector names: nothing restyles
    page.doc->getElementById("x")->setAttribute("class", "nothing-uses-this");
    page.pass();
    EXPECT_FALSE(page.css->lastPassFull());
    EXPECT_EQ(page.css->lastPassDropped(), 0u);
    // A subject class: the element and its subtree
    page.doc->getElementById("x")->setAttribute("class", "on");
    page.pass();
    EXPECT_FALSE(page.css->lastPassFull());
    EXPECT_EQ(page.css->lastPassDropped(), 2u);
    EXPECT_EQ(page.red("x"), 1);
    EXPECT_EQ(page.red("y"), 1);   // inherited
    // An ancestor class: the subtree under it
    page.doc->getElementById("w")->setAttribute("class", "wrap");
    page.pass();
    EXPECT_EQ(page.css->lastPassDropped(), 5u);
    EXPECT_EQ(page.red("a"), 2);
    EXPECT_NE(page.red("c"), 2);   // not an .item
    // A sibling-combinator class: the parent's children
    page.doc->getElementById("a")->setAttribute("class", "item mark");
    page.pass();
    EXPECT_EQ(page.red("b"), 3);
    // An attribute selector; an id
    page.doc->getElementById("c")->setAttribute("data-k", "v");
    page.doc->getElementById("l2")->setAttribute("id", "solo");
    page.pass();
    EXPECT_EQ(page.red("c"), 5);
    EXPECT_EQ(page.red("deep"), 5);
    EXPECT_EQ(page.red("solo"), 6);
    EXPECT_EQ(page.snapshot(), page.reference());
}

TEST(Restyle, StructureStateAndSheetChanges) {
    RestylePage page;
    DOMElement* list = page.doc->getElementById("list");
    EXPECT_EQ(page.red("l1"), 4);
    // A new first child: :first-child moves
    auto li = page.doc->createElement("li");
    li->setAttribute("id", "l0");
    list->insertBefore(std::move(li), page.doc->getElementById("l1"));
    page.pass();
    EXPECT_EQ(page.red("l0"), 4);
    EXPECT_NE(page.red("l1"), 4);
    list->removeChild(page.doc->getElementById("l0"));
    page.pass();
    EXPECT_EQ(page.red("l1"), 4);
    // A checkbox state, through its sibling selector
    noteStateChange(page.doc->getElementById("cb"), "checked");
    page.doc->getElementById("cb")->setAttribute("checked", "");
    page.pass();
    EXPECT_FALSE(page.css->lastPassFull());
    EXPECT_EQ(page.snapshot(), page.reference());
    // A sheet added and removed: only elements its subjects may match
    page.pass();
    auto entries = page.css->resolver().styleSheets();
    auto sheet = CSSParser().parse(".tint { background-color: rgb(8, 8, 8) }");
    std::shared_ptr<CSSStyleSheet> shared(std::move(sheet));
    page.doc->getElementById("y")->setAttribute("class", "tint");
    page.pass();
    EXPECT_EQ(page.css->getComputedStyle(page.doc->getElementById("y"))->backgroundColor.r, 9);
    auto withSheet = entries;
    withSheet.push_back({shared, StyleOrigin::Author});
    page.css->resolver().setStyleSheets(withSheet);
    page.pass();
    EXPECT_FALSE(page.css->lastPassFull());
    EXPECT_EQ(page.css->lastPassDropped(), 1u);
    EXPECT_EQ(page.css->getComputedStyle(page.doc->getElementById("y"))->backgroundColor.r, 8);
    page.css->resolver().setStyleSheets(entries);
    page.pass();
    EXPECT_EQ(page.css->getComputedStyle(page.doc->getElementById("y"))->backgroundColor.r, 9);
    EXPECT_EQ(page.snapshot(), page.reference());
    // An undescribed change restyles everything
    noteDomMutation();
    page.pass();
    EXPECT_TRUE(page.css->lastPassFull());
}

// Random DOM changes: after each style pass the styles are the ones a
// restyle from scratch gives
TEST(Restyle, RandomChangesMatchAFullRestyle) {
    RestylePage page;
    const char* ids[] = {"w", "a", "b", "c", "deep", "list", "l1", "l2", "x", "y", "cb", "lab"};
    const char* classes[] = {"on", "wrap", "item", "mark", "tint", "", "item mark", "zzz"};
    unsigned seed = 12345;
    auto next = [&](unsigned n) { seed = seed * 1103515245u + 12345u; return (seed >> 16) % n; };
    int created = 0;
    for (int step = 0; step < 300; step++) {
        DOMElement* e = page.doc->getElementById(ids[next(12)]);
        if (!e) continue;
        switch (next(5)) {
            case 0: e->setAttribute("class", classes[next(8)]); break;
            case 1: if (next(2)) e->setAttribute("data-k", "v"); else e->removeAttribute("data-k"); break;
            case 2: {
                auto child = page.doc->createElement(next(2) ? "li" : "p");
                child->setAttribute("class", classes[next(8)]);
                child->setAttribute("id", "n" + std::to_string(created++));
                e->insertBefore(std::move(child), e->firstChild());
                break;
            }
            case 3: if (DOMNode* first = e->firstChild()) e->removeChild(first); break;
            case 4: e->setAttribute("style", next(2) ? "color: rgb(11, 0, 0)" : ""); break;
        }
        std::string incremental = page.pass();
        std::string full = page.reference();
        ASSERT_EQ(incremental, full) << "after step " << step;
    }
    EXPECT_GT(page.css->styleStats().targetedPasses, 200u);
}
