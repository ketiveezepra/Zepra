// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file webcore_forced_pseudo_tests.cpp
 * @brief :hover, :active and :focus forced by DevTools (setForcedPseudoClass):
 *        which elements match, alongside the real pointer and focus state
 */

#include <gtest/gtest.h>

#include "browser/dom.hpp"
#include "css/css_engine.hpp"
#include "css/ua_stylesheet.h"
#include "html/html_parser.hpp"

#include <memory>
#include <string>
#include <tuple>

// Viewport size the layout engine reads (zepra_browser.cpp defines these)
int g_width = 1280;
int g_height = 800;

using namespace Zepra::WebCore;

namespace {

const char* kHtml =
    "<!DOCTYPE html><html><body>"
    "<div id='outer'><a id='link' href='#'>one</a><a id='link2' href='#'>two</a><span id='sib'>s</span></div>"
    "<div id='other'><span id='inner'>i</span></div>"
    "<form id='form'><input id='in'><input id='in2'></form>"
    "</body></html>";

const char* kCss =
    "a { color: rgb(1, 1, 1); }"
    "a:hover { color: rgb(255, 0, 0); }"
    "a:active { color: rgb(128, 0, 128); }"
    "span { color: rgb(2, 2, 2); }"
    "div:hover > span { color: rgb(0, 0, 255); }"
    "div { color: rgb(3, 3, 3); }"
    "div:active { color: rgb(10, 20, 30); }"
    "form { color: rgb(4, 4, 4); }"
    "form:focus-within { color: rgb(255, 165, 0); }"
    "input { color: rgb(5, 5, 5); }"
    "input:focus { color: rgb(0, 128, 0); }";

using RGB = std::tuple<int, int, int>;
const RGB kRed{255, 0, 0}, kPurple{128, 0, 128}, kBlue{0, 0, 255}, kDivActive{10, 20, 30},
    kOrange{255, 165, 0}, kGreen{0, 128, 0};
const RGB kLinkPlain{1, 1, 1}, kSpanPlain{2, 2, 2}, kDivPlain{3, 3, 3}, kFormPlain{4, 4, 4},
    kInputPlain{5, 5, 5};

class ForcedPseudoTest : public ::testing::Test {
protected:
    std::unique_ptr<DOMDocument> doc;
    std::unique_ptr<CSSEngine> css;

    void SetUp() override {
        interactionState() = {};
        HTMLParser parser;
        doc = parser.parse(kHtml);
        ASSERT_NE(doc, nullptr);
        css = std::make_unique<CSSEngine>();
        css->initialize(doc.get());
        css->addStyleSheet(ZepraUAStylesheet::getStylesheet(), StyleOrigin::UserAgent);
        css->addStyleSheet(kCss, StyleOrigin::Author);
    }
    void TearDown() override { interactionState() = {}; }

    DOMElement* el(const std::string& id) {
        DOMElement* e = doc->getElementById(id);
        EXPECT_NE(e, nullptr) << id;
        return e;
    }
    // Restyles everything (what the browser's relayout does), then reads one color
    RGB color(const std::string& id) {
        css->invalidate();
        const CSSComputedStyle* s = css->getComputedStyle(el(id));
        if (!s) return RGB{-1, -1, -1};
        return RGB{s->color.r, s->color.g, s->color.b};
    }
};

TEST_F(ForcedPseudoTest, NothingForcedMatchesNothing) {
    EXPECT_EQ(color("link"), kLinkPlain);
    EXPECT_EQ(color("sib"), kSpanPlain);
    EXPECT_EQ(color("in"), kInputPlain);
    EXPECT_EQ(color("form"), kFormPlain);
    EXPECT_EQ(forcedPseudoClasses(el("link")), 0u);
}

TEST_F(ForcedPseudoTest, ForcedHoverMatchesElementAndAncestorsNotSiblings) {
    EXPECT_TRUE(setForcedPseudoClass(el("link"), ForcedPseudo::Hover, true));
    EXPECT_EQ(forcedPseudoClasses(el("link")), static_cast<unsigned>(ForcedPseudo::Hover));
    EXPECT_EQ(color("link"), kRed);
    // The parent div matches :hover, so `div:hover > span` reaches the span
    EXPECT_EQ(color("sib"), kBlue);
    // The sibling link is not hovered; neither is the other div's span
    EXPECT_EQ(color("link2"), kLinkPlain);
    EXPECT_EQ(color("inner"), kSpanPlain);
    // :hover is not :active
    EXPECT_EQ(color("outer"), kDivPlain);
}

TEST_F(ForcedPseudoTest, TurningHoverOffRestores) {
    setForcedPseudoClass(el("link"), ForcedPseudo::Hover, true);
    ASSERT_EQ(color("link"), kRed);
    EXPECT_TRUE(setForcedPseudoClass(el("link"), ForcedPseudo::Hover, false));
    EXPECT_EQ(forcedPseudoClasses(el("link")), 0u);
    EXPECT_TRUE(interactionState().forced.empty());
    EXPECT_EQ(color("link"), kLinkPlain);
    EXPECT_EQ(color("sib"), kSpanPlain);
}

TEST_F(ForcedPseudoTest, ForcedActiveMatchesElementAndAncestors) {
    EXPECT_TRUE(setForcedPseudoClass(el("link"), ForcedPseudo::Active, true));
    EXPECT_EQ(color("link"), kPurple);
    EXPECT_EQ(color("outer"), kDivActive);
    EXPECT_EQ(color("link2"), kLinkPlain);
    EXPECT_EQ(color("other"), kDivPlain);
    // Active alone does not make anything :hover
    EXPECT_EQ(color("sib"), kSpanPlain);
    setForcedPseudoClass(el("link"), ForcedPseudo::Active, false);
    EXPECT_EQ(color("link"), kLinkPlain);
    EXPECT_EQ(color("outer"), kDivPlain);
}

TEST_F(ForcedPseudoTest, ForcedFocusMatchesFocusOnElementAndFocusWithinOnAncestors) {
    EXPECT_TRUE(setForcedPseudoClass(el("in"), ForcedPseudo::Focus, true));
    EXPECT_EQ(color("in"), kGreen);
    EXPECT_EQ(color("form"), kOrange);
    // The other input is not focused
    EXPECT_NE(color("in2"), kGreen);
    // A focused ancestor does not make the element :focus (only :focus-within)
    setForcedPseudoClass(el("in"), ForcedPseudo::Focus, false);
    setForcedPseudoClass(el("form"), ForcedPseudo::Focus, true);
    EXPECT_EQ(color("form"), kOrange);
    EXPECT_EQ(color("in"), kInputPlain);
    setForcedPseudoClass(el("form"), ForcedPseudo::Focus, false);
    EXPECT_EQ(color("form"), kFormPlain);
}

TEST_F(ForcedPseudoTest, StatesCombineOnOneElementAndAcrossElements) {
    setForcedPseudoClass(el("link"), ForcedPseudo::Hover, true);
    setForcedPseudoClass(el("link"), ForcedPseudo::Active, true);
    setForcedPseudoClass(el("in"), ForcedPseudo::Focus, true);
    EXPECT_EQ(forcedPseudoClasses(el("link")),
              static_cast<unsigned>(ForcedPseudo::Hover) | static_cast<unsigned>(ForcedPseudo::Active));
    // a:active comes later in the sheet than a:hover
    EXPECT_EQ(color("link"), kPurple);
    EXPECT_EQ(color("sib"), kBlue);
    EXPECT_EQ(color("in"), kGreen);
    // Dropping one state keeps the other
    setForcedPseudoClass(el("link"), ForcedPseudo::Active, false);
    EXPECT_EQ(forcedPseudoClasses(el("link")), static_cast<unsigned>(ForcedPseudo::Hover));
    EXPECT_EQ(color("link"), kRed);
}

TEST_F(ForcedPseudoTest, ChangesBumpDomVersionAndNoOpsDoNot) {
    const uint64_t v0 = domMutationVersion();
    EXPECT_TRUE(setForcedPseudoClass(el("link"), ForcedPseudo::Hover, true));
    const uint64_t v1 = domMutationVersion();
    EXPECT_NE(v1, v0);
    EXPECT_FALSE(setForcedPseudoClass(el("link"), ForcedPseudo::Hover, true));
    EXPECT_FALSE(setForcedPseudoClass(el("link"), ForcedPseudo::Focus, false));
    EXPECT_FALSE(setForcedPseudoClass(nullptr, ForcedPseudo::Hover, true));
    EXPECT_EQ(domMutationVersion(), v1);
    EXPECT_TRUE(clearForcedPseudoClasses());
    EXPECT_NE(domMutationVersion(), v1);
    const uint64_t v2 = domMutationVersion();
    EXPECT_FALSE(clearForcedPseudoClasses());
    EXPECT_EQ(domMutationVersion(), v2);
}

TEST_F(ForcedPseudoTest, ClearAndResetDropForcedStates) {
    setForcedPseudoClass(el("link"), ForcedPseudo::Hover, true);
    setForcedPseudoClass(el("in"), ForcedPseudo::Focus, true);
    EXPECT_TRUE(clearForcedPseudoClasses());
    EXPECT_EQ(color("link"), kLinkPlain);
    EXPECT_EQ(color("in"), kInputPlain);

    // What the browser does when a document is replaced
    setForcedPseudoClass(el("link"), ForcedPseudo::Hover, true);
    interactionState() = {};
    EXPECT_TRUE(interactionState().forced.empty());
    EXPECT_EQ(forcedPseudoClasses(el("link")), 0u);
    EXPECT_EQ(color("link"), kLinkPlain);
    EXPECT_EQ(color("sib"), kSpanPlain);
}

TEST_F(ForcedPseudoTest, RealHoverStillWorksAlongsideForced) {
    setForcedPseudoClass(el("link"), ForcedPseudo::Hover, true);
    interactionState().hovered = el("inner");
    EXPECT_EQ(color("link"), kRed);
    EXPECT_EQ(color("sib"), kBlue);
    EXPECT_EQ(color("inner"), kBlue);   // real hover: its div matches :hover
    // The pointer moving away does not undo the forced state
    interactionState().hovered = nullptr;
    EXPECT_EQ(color("inner"), kSpanPlain);
    EXPECT_EQ(color("link"), kRed);
    // Real focus and forced focus on different inputs
    interactionState().focused = el("in2");
    setForcedPseudoClass(el("in"), ForcedPseudo::Focus, true);
    EXPECT_EQ(color("in"), kGreen);
    EXPECT_EQ(color("in2"), kGreen);
}

TEST_F(ForcedPseudoTest, RemovedElementMatchesNothing) {
    DOMElement* outer = el("outer");
    DOMElement* link = el("link");
    setForcedPseudoClass(link, ForcedPseudo::Hover, true);
    ASSERT_EQ(color("sib"), kBlue);
    {
        // Removed and destroyed: the entry goes with the element, so a new element
        // that later gets the same address is not forced by mistake
        std::unique_ptr<DOMNode> removed = outer->removeChild(link);
        ASSERT_NE(removed, nullptr);
    }
    EXPECT_EQ(color("sib"), kSpanPlain);
    EXPECT_EQ(color("link2"), kLinkPlain);
    EXPECT_EQ(color("outer"), kDivPlain);
    EXPECT_TRUE(interactionState().forced.empty());
    EXPECT_FALSE(clearForcedPseudoClasses()) << "nothing is forced any more";
}

// Hover, active and focus are raw pointers in process-wide state: freeing the
// element (or its whole document) must clear them, or the next focus() or
// restyle reads freed memory
TEST_F(ForcedPseudoTest, DestroyedElementsLeaveNoInteractionPointers) {
    InteractionState& state = interactionState();
    state.hovered = el("link");
    state.active = el("link");
    state.focused = el("in");
    {
        std::unique_ptr<DOMNode> removed = el("outer")->removeChild(el("link"));
        ASSERT_NE(removed, nullptr);
        EXPECT_EQ(state.hovered, removed.get()) << "detached but alive: kept";
    }
    EXPECT_EQ(state.hovered, nullptr);
    EXPECT_EQ(state.active, nullptr);
    EXPECT_EQ(state.focused, el("in")) << "other elements are untouched";

    setForcedPseudoClass(el("in2"), ForcedPseudo::Focus, true);
    css.reset();
    doc.reset();
    EXPECT_EQ(state.focused, nullptr);
    EXPECT_TRUE(state.forced.empty());
}

TEST_F(ForcedPseudoTest, DetachedElementStillAliveDoesNotReachTheDocument) {
    DOMElement* outer = el("outer");
    DOMElement* link = el("link");
    setForcedPseudoClass(link, ForcedPseudo::Hover, true);
    std::unique_ptr<DOMNode> removed = outer->removeChild(link);
    ASSERT_NE(removed, nullptr);
    EXPECT_EQ(color("sib"), kSpanPlain);
    // Put back: it matches again
    outer->appendChild(std::move(removed));
    EXPECT_EQ(color("sib"), kBlue);
    EXPECT_EQ(color("link"), kRed);
}

}  // namespace
