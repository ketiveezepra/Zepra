// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * layout_geometry_tests.cpp - Element geometry for page scripts: which
 * layout boxes belong to an element, where they are in the viewport, and the
 * CSSOM View properties scripts read from them (getBoundingClientRect,
 * offset*, client*, scroll*, window scrolling).
 *
 * The box trees are built by hand, as the browser's box builder would leave
 * them after layout (x/y relative to the parent's border box).
 */

#include <gtest/gtest.h>

#include "browser/dom.hpp"
#include "browser/layout_geometry.h"
#include "html/html_parser.hpp"
#include "scripting/dom_bindings.hpp"
#include "scripting/page_geometry.hpp"
#include "scripting/script_context.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

// Viewport size the layout engine reads (zepra_browser.cpp defines these)
int g_width = 1280;
int g_height = 800;

using namespace Zepra::WebCore;
using ZepraBrowser::LayoutBox;
using ZepraBrowser::LayoutGeometryIndex;
using ZepraBrowser::LayoutType;
using ZepraBrowser::PageLayout;

namespace {

LayoutBox& addBox(LayoutBox& parent, DOMElement* owner, float x, float y, float w, float h,
                  LayoutType type = LayoutType::Block) {
    parent.children.emplace_back();
    LayoutBox& b = parent.children.back();
    b.domElement = owner;
    b.type = type;
    b.x = x; b.y = y; b.width = w; b.height = h;
    return b;
}

void setBorder(LayoutBox& b, float w) { b.borderTop = b.borderRight = b.borderBottom = b.borderLeft = w; }

/// A page laid out by hand, behind the PageGeometry scripts read
class TestGeometry : public PageGeometry {
public:
    PageLayout page;
    LayoutGeometryIndex index;
    int queries = 0;

    void reindex() { index.build(page); }
    ElementGeometry elementGeometry(const DOMElement* element, bool withScrollSize) override {
        queries++;
        return index.geometryOf(page, element, withScrollSize);
    }
    /// Everything, scroll size included (what the index tests check)
    ElementGeometry elementGeometry(const DOMElement* element) { return elementGeometry(element, true); }
    double viewportWidth() override { return page.viewportWidth; }
    double viewportHeight() override { return page.viewportHeight; }
    double scrollX() override { return page.scrollX; }
    double scrollY() override { return page.scrollY; }
    void scrollTo(double, double y) override {
        double maxScroll = std::max(0.0f, page.root->height - page.viewportHeight);
        page.scrollY = (float)std::clamp(y, 0.0, maxScroll);
    }
    void scrollElementTo(const DOMElement* element, double x, double y) override {
        if (const LayoutBox* box = index.principalBox(element)) ZepraBrowser::scrollContainerTo(*box, (float)x, (float)y);
    }
};

const char* kPage = R"(<!DOCTYPE html><html><head></head><body>
<div id="box">text<span id="word">word</span></div>
<div id="frame"><p id="inner">inner</p></div>
<div id="fixed">fixed</div>
<div id="hidden">no box</div>
<table id="table"><tr><td id="cell"><b id="bold">b</b></td></tr></table>
</body></html>)";

struct Page {
    std::unique_ptr<DOMDocument> doc;
    std::unique_ptr<ScriptContext> script;
    TestGeometry geometry;
    LayoutBox root;

    explicit Page(const std::string& html = kPage) {
        HTMLParser parser;
        doc = parser.parse(html);
        script = std::make_unique<ScriptContext>();
        script->setConsoleHandler([](const std::string&, const std::string&) {});
        script->initialize(doc.get());

        // body: 1200 x 2000 at the page origin (20, 20), 8px padding
        root.width = 1200; root.height = 2000;
        root.paddingTop = root.paddingLeft = root.paddingRight = root.paddingBottom = 8;
        // #box: 300 x 100 at (8, 8), 2px border, 10px padding; its own text run
        // and an inline <span> with a text run of its own
        LayoutBox& box = addBox(root, byId("box"), 8, 8, 300, 100);
        setBorder(box, 2);
        box.paddingLeft = box.paddingRight = box.paddingTop = box.paddingBottom = 10;
        addBox(box, byId("box"), 12, 12, 40, 20, LayoutType::Inline).text = "text";
        LayoutBox& word = addBox(box, byId("word"), 56, 12, 50, 20, LayoutType::Inline);
        addBox(word, byId("word"), 0, 0, 50, 20, LayoutType::Inline).text = "word";
        // #frame: position: relative, 5px border, at (8, 120); <p> at (25, 30) inside
        LayoutBox& frame = addBox(root, byId("frame"), 8, 120, 400, 200);
        frame.positionType = 1;
        setBorder(frame, 5);
        addBox(frame, byId("inner"), 25, 30, 100, 40);
        // #fixed: position: fixed at (500, 0)
        addBox(root, byId("fixed"), 500, 0, 80, 30).positionType = 3;
        // the table: an anonymous row group wraps the row
        LayoutBox& table = addBox(root, byId("table"), 8, 400, 200, 50, LayoutType::Table);
        LayoutBox& group = addBox(table, byId("table"), 0, 0, 200, 50, LayoutType::TableRowGroup);
        group.anonymous = true;
        LayoutBox& row = addBox(group, doc->getElementsByTagName("tr")[0], 0, 0, 200, 50, LayoutType::TableRow);
        LayoutBox& cell = addBox(row, byId("cell"), 2, 2, 100, 46, LayoutType::TableCell);
        addBox(cell, byId("bold"), 4, 4, 20, 18, LayoutType::Inline);

        geometry.page.root = &root;
        geometry.page.body = doc->body();
        geometry.page.documentElement = doc->documentElement();
        geometry.page.originX = 20;
        geometry.page.originY = 20;
        geometry.page.viewportWidth = 1240;
        geometry.page.viewportHeight = 700;
        geometry.reindex();
        script->setPageGeometry(&geometry);
    }

    DOMElement* byId(const std::string& id) { return doc->getElementById(id); }

    std::string eval(const std::string& code) {
        ScriptResult r = script->evaluate(code, "test.js");
        EXPECT_TRUE(r.success) << r.error << "\n  in: " << code;
        return r.success ? r.value : "<error: " + r.error + ">";
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Which boxes belong to an element, and where they are
// ---------------------------------------------------------------------------

TEST(LayoutGeometryIndex, PrincipalBoxInViewportCoordinates) {
    Page page;
    ElementGeometry g = page.geometry.elementGeometry(page.byId("box"));
    ASSERT_TRUE(g.hasBox);
    // Its own text run is inside its box, not a second fragment
    ASSERT_EQ(g.rects.size(), 1u);
    EXPECT_DOUBLE_EQ(g.rects[0].x, 28);   // origin 20 + 8
    EXPECT_DOUBLE_EQ(g.rects[0].y, 28);
    EXPECT_DOUBLE_EQ(g.rects[0].width, 300);
    EXPECT_DOUBLE_EQ(g.rects[0].height, 100);
    EXPECT_DOUBLE_EQ(g.borderLeft, 2);
    EXPECT_FALSE(g.inlineBox);

    // Nested offsets add up: #inner is 25, 30 into #frame
    ElementGeometry inner = page.geometry.elementGeometry(page.byId("inner"));
    ASSERT_EQ(inner.rects.size(), 1u);
    EXPECT_DOUBLE_EQ(inner.rects[0].x, 20 + 8 + 25);
    EXPECT_DOUBLE_EQ(inner.rects[0].y, 20 + 120 + 30);
}

TEST(LayoutGeometryIndex, ScrollingMovesBoxesWithoutReindexing) {
    Page page;
    page.geometry.page.scrollY = 50;
    ElementGeometry g = page.geometry.elementGeometry(page.byId("box"));
    EXPECT_DOUBLE_EQ(g.rects[0].y, 28 - 50);
    ElementGeometry html = page.geometry.elementGeometry(page.doc->documentElement());
    EXPECT_DOUBLE_EQ(html.rects[0].y, -50);
}

TEST(LayoutGeometryIndex, InlineElementAndBody) {
    Page page;
    ElementGeometry word = page.geometry.elementGeometry(page.byId("word"));
    ASSERT_EQ(word.rects.size(), 1u);
    EXPECT_TRUE(word.inlineBox);
    EXPECT_DOUBLE_EQ(word.rects[0].x, 20 + 8 + 56);

    // The root box is the body's
    ElementGeometry body = page.geometry.elementGeometry(page.doc->body());
    ASSERT_TRUE(body.hasBox);
    EXPECT_DOUBLE_EQ(body.rects[0].x, 20);
    EXPECT_DOUBLE_EQ(body.rects[0].height, 2000);
}

TEST(LayoutGeometryIndex, TextRunsOfAnElementWithoutABox) {
    // An element whose text was laid out on two lines, in anonymous boxes of
    // its parent: one fragment per run, bounds around both
    Page page;
    DOMElement* box = page.byId("box");
    DOMElement* word = page.byId("word");
    LayoutBox& host = page.root.children.front();
    host.children.clear();
    LayoutBox& line1 = addBox(host, box, 12, 12, 280, 20);
    line1.anonymous = true;
    addBox(line1, word, 200, 0, 80, 20, LayoutType::Inline).text = "first";
    LayoutBox& line2 = addBox(host, box, 12, 32, 280, 20);
    line2.anonymous = true;
    addBox(line2, word, 0, 0, 60, 20, LayoutType::Inline).text = "second";
    page.geometry.reindex();

    ElementGeometry g = page.geometry.elementGeometry(word);
    ASSERT_EQ(g.rects.size(), 2u);
    GeometryRect bounds = g.bounds();
    EXPECT_DOUBLE_EQ(bounds.x, 20 + 8 + 12);
    EXPECT_DOUBLE_EQ(bounds.y, 20 + 8 + 12);
    EXPECT_DOUBLE_EQ(bounds.width, 280);
    EXPECT_DOUBLE_EQ(bounds.height, 40);
    // The anonymous lines are not boxes of #box
    EXPECT_EQ(page.geometry.elementGeometry(box).rects.size(), 1u);
}

TEST(LayoutGeometryIndex, NoBoxForDisplayNoneOrUnlaidElements) {
    Page page;
    EXPECT_FALSE(page.geometry.elementGeometry(page.byId("hidden")).hasBox);
    LayoutBox& none = addBox(page.root, page.byId("hidden"), 0, 0, 10, 10, LayoutType::None);
    (void)none;
    page.geometry.reindex();
    EXPECT_FALSE(page.geometry.elementGeometry(page.byId("hidden")).hasBox);
    EXPECT_FALSE(page.geometry.elementGeometry(nullptr).hasBox);
}

TEST(LayoutGeometryIndex, ScrollSizeReachesOverflowingContent) {
    Page page;
    // #frame: padding box 390 x 190; make <p> overflow to the right and bottom
    LayoutBox& frame = *std::next(page.root.children.begin());
    LayoutBox& inner = frame.children.front();
    inner.x = 300; inner.y = 180; inner.width = 200; inner.height = 50;
    page.geometry.reindex();
    ElementGeometry g = page.geometry.elementGeometry(page.byId("frame"));
    EXPECT_DOUBLE_EQ(g.scrollWidth, 300 + 200 - 5);
    EXPECT_DOUBLE_EQ(g.scrollHeight, 180 + 50 - 5);
    // Content that fits: the padding box
    ElementGeometry box = page.geometry.elementGeometry(page.byId("box"));
    EXPECT_DOUBLE_EQ(box.scrollWidth, 296);
    EXPECT_DOUBLE_EQ(box.scrollHeight, 96);
}

TEST(LayoutGeometryIndex, RootSitsAtItsMarginLikePaintBox) {
    // layoutBlock puts the root (body) at its margin: everything moves with it
    Page page;
    page.root.x = 8;
    page.root.y = 8;
    page.geometry.reindex();
    ElementGeometry body = page.geometry.elementGeometry(page.doc->body());
    EXPECT_DOUBLE_EQ(body.rects[0].x, 28);
    ElementGeometry box = page.geometry.elementGeometry(page.byId("box"));
    EXPECT_DOUBLE_EQ(box.rects[0].x, 20 + 8 + 8);
    EXPECT_DOUBLE_EQ(box.rects[0].y, 20 + 8 + 8);
}

TEST(LayoutGeometryIndex, PlaceBoxesMatchesTheIndex) {
    // Screen positions for hit-testing a tree that was not painted yet
    Page page;
    page.root.x = 8;
    ZepraBrowser::placeBoxes(page.root, 100, 50, 30);
    const LayoutBox& frame = *std::next(page.root.children.begin());
    const LayoutBox& inner = frame.children.front();
    EXPECT_FLOAT_EQ(page.root.screenX, 108);
    EXPECT_FLOAT_EQ(inner.screenX, 108 + 8 + 25);
    EXPECT_FLOAT_EQ(inner.screenY, 50 - 30 + 120 + 30);
}

TEST(LayoutGeometryIndex, ScrollSizeOnlyWhenAskedFor) {
    Page page;
    ElementGeometry g = page.geometry.elementGeometry(page.byId("box"), false);
    EXPECT_TRUE(g.hasBox);
    EXPECT_DOUBLE_EQ(g.scrollWidth, 0);
}

TEST(LayoutGeometryIndex, DocumentSize) {
    Page page;
    EXPECT_FLOAT_EQ(ZepraBrowser::documentHeight(page.geometry.page), 2000);
    EXPECT_FLOAT_EQ(ZepraBrowser::documentWidth(page.geometry.page), 1240);
    page.root.height = 100;
    EXPECT_FLOAT_EQ(ZepraBrowser::documentHeight(page.geometry.page), 700);
}

// ---------------------------------------------------------------------------
// What scripts read
// ---------------------------------------------------------------------------

TEST(ScriptGeometry, BoundingClientRect) {
    Page page;
    EXPECT_EQ(page.eval("var r = document.getElementById('box').getBoundingClientRect();"
                        "[r.x, r.y, r.width, r.height, r.top, r.right, r.bottom, r.left].join()"),
              "28,28,300,100,28,328,128,28");
    EXPECT_EQ(page.eval("r instanceof DOMRect"), "true");
    EXPECT_EQ(page.eval("JSON.stringify(r.toJSON())"),
              R"({"x":28,"y":28,"width":300,"height":100,"top":28,"right":328,"bottom":128,"left":28})");
    EXPECT_EQ(page.eval("document.getElementById('box').getClientRects().length"), "1");
    // No box: an all-zero rect and no client rects
    EXPECT_EQ(page.eval("var h = document.getElementById('hidden').getBoundingClientRect(); h.width + h.top"), "0");
    EXPECT_EQ(page.eval("document.getElementById('hidden').getClientRects().length"), "0");
    // Detached elements have no box either
    EXPECT_EQ(page.eval("var b = document.getElementById('box'); b.remove(); b.getBoundingClientRect().width"), "0");
}

TEST(ScriptGeometry, OffsetPropertiesFollowTheOffsetParent) {
    Page page;
    // Static elements under <body>: page coordinates
    EXPECT_EQ(page.eval("var box = document.getElementById('box');"
                        "[box.offsetParent === document.body, box.offsetLeft, box.offsetTop,"
                        " box.offsetWidth, box.offsetHeight].join()"),
              "true,28,28,300,100");
    // Inside a positioned element: from its padding edge (inside the 5px border)
    EXPECT_EQ(page.eval("var p = document.getElementById('inner');"
                        "[p.offsetParent.id, p.offsetLeft, p.offsetTop].join()"),
              "frame,20,25");
    // Table cells are the offset parent of static content
    EXPECT_EQ(page.eval("document.getElementById('bold').offsetParent.id"), "cell");
    // No offset parent: <body>, fixed elements, elements without a box
    EXPECT_EQ(page.eval("[document.body.offsetParent, document.getElementById('fixed').offsetParent,"
                        " document.getElementById('hidden').offsetParent].join()"),
              ",,");
    EXPECT_EQ(page.eval("document.getElementById('hidden').offsetWidth"), "0");
    // <body> itself is at offset 0, 0
    EXPECT_EQ(page.eval("document.body.offsetTop + ',' + document.body.offsetLeft"), "0,0");
    // Offsets from the page origin do not move when the page scrolls
    page.geometry.page.scrollY = 300;
    EXPECT_EQ(page.eval("box.offsetTop + ',' + box.getBoundingClientRect().top"), "28,-272");
}

TEST(ScriptGeometry, ClientAndScrollSizes) {
    Page page;
    EXPECT_EQ(page.eval("var box = document.getElementById('box');"
                        "[box.clientWidth, box.clientHeight, box.clientTop, box.clientLeft,"
                        " box.scrollWidth, box.scrollHeight].join()"),
              "296,96,2,2,296,96");
    // Inline boxes have no client area
    EXPECT_EQ(page.eval("var w = document.getElementById('word'); [w.clientWidth, w.clientTop, w.offsetWidth].join()"),
              "0,0,50");
    // <html>: the viewport, and the whole page for the scroll size
    EXPECT_EQ(page.eval("var html = document.documentElement;"
                        "[html.clientWidth, html.clientHeight, html.scrollWidth, html.scrollHeight].join()"),
              "1240,700,1240,2000");
    EXPECT_EQ(page.eval("[window.innerWidth, window.innerHeight, innerWidth].join()"), "1240,700,1240");
}

TEST(ScriptGeometry, WindowAndDocumentScrolling) {
    Page page;
    EXPECT_EQ(page.eval("window.scrollTo(0, 250); [scrollY, window.scrollY, window.pageYOffset,"
                        " document.documentElement.scrollTop].join()"),
              "250,250,250,250");
    EXPECT_FLOAT_EQ(page.geometry.page.scrollY, 250);
    EXPECT_EQ(page.eval("window.scrollBy({top: 50}); window.scrollY"), "300");
    EXPECT_EQ(page.eval("document.documentElement.scrollTop = 100; window.scrollY"), "100");
    EXPECT_EQ(page.eval("document.scrollingElement.scrollTo({top: 40}); window.scrollY"), "40");
    // Clamped to the scrollable range (2000 - 700)
    EXPECT_EQ(page.eval("scrollTo(0, 99999); scrollY"), "1300");
    EXPECT_EQ(page.eval("scrollBy(0, -99999); scrollY"), "0");
    // Other elements do not scroll
    EXPECT_EQ(page.eval("var box = document.getElementById('box'); box.scrollTop = 30; box.scrollTop + window.scrollY"),
              "0");
    // A script started after the browser scrolled sees the new position
    page.geometry.page.scrollY = 75;
    EXPECT_EQ(page.eval("scrollY + ',' + pageYOffset"), "75,75");
    // Non-finite coordinates are 0
    EXPECT_EQ(page.eval("scrollTo(0, Infinity); scrollY"), "0");
    EXPECT_EQ(page.eval("scrollTo(0, 40); document.documentElement.scrollTop = NaN; scrollY"), "0");
    // A page's own global of the same name is left alone
    EXPECT_EQ(page.eval("var scrollY = 'mine'; scrollY"), "mine");
    page.geometry.page.scrollY = 90;
    EXPECT_EQ(page.eval("scrollY + ',' + window.scrollY + ',' + pageYOffset"), "mine,90,90");
}

TEST(ScriptGeometry, ScrollIntoView) {
    Page page;
    // #inner's top is at 170: scrolled to the top of the viewport
    EXPECT_EQ(page.eval("document.getElementById('inner').scrollIntoView();"
                        "document.getElementById('inner').getBoundingClientRect().top"), "0");
    EXPECT_FLOAT_EQ(page.geometry.page.scrollY, 170);
    // false / block: 'end': its bottom at the bottom of the viewport
    page.eval("document.getElementById('table').scrollIntoView(false)");
    EXPECT_FLOAT_EQ(page.geometry.page.scrollY, 20 + 400 + 50 - 700 < 0 ? 0 : 20 + 400 + 50 - 700);
    page.geometry.page.scrollY = 1000;
    page.eval("document.getElementById('box').scrollIntoView({block: 'center'})");
    EXPECT_FLOAT_EQ(page.geometry.page.scrollY, 0);  // 28 + 50 - 350 < 0: clamped
    // nearest: a box already in view does not move the page
    page.geometry.page.scrollY = 10;
    page.eval("document.getElementById('box').scrollIntoView({block: 'nearest'})");
    EXPECT_FLOAT_EQ(page.geometry.page.scrollY, 10);
    // nearest, below the viewport: its bottom edge comes to the bottom (#inner bottom = 210)
    page.geometry.page.viewportHeight = 100;
    page.geometry.page.scrollY = 0;
    page.eval("document.getElementById('inner').scrollIntoView({block: 'nearest'})");
    EXPECT_FLOAT_EQ(page.geometry.page.scrollY, 110);
    // nearest, above the viewport: its top edge comes to the top
    page.geometry.page.scrollY = 500;
    page.eval("document.getElementById('inner').scrollIntoView({block: 'nearest'})");
    EXPECT_FLOAT_EQ(page.geometry.page.scrollY, 170);
    // nearest, taller than the viewport and covering it: no move
    page.geometry.page.viewportHeight = 30;
    page.geometry.page.scrollY = 180;
    page.eval("document.getElementById('inner').scrollIntoView({block: 'nearest'})");
    EXPECT_FLOAT_EQ(page.geometry.page.scrollY, 180);
    // null is not false: align to the top
    page.geometry.page.viewportHeight = 700;
    page.geometry.page.scrollY = 0;
    page.eval("document.getElementById('inner').scrollIntoView(null)");
    EXPECT_FLOAT_EQ(page.geometry.page.scrollY, 170);
}

TEST(ScriptGeometry, WithoutLayoutEverythingIsZero) {
    HTMLParser parser;
    auto doc = parser.parse(kPage);
    ScriptContext script;
    script.setConsoleHandler([](const std::string&, const std::string&) {});
    script.initialize(doc.get());
    ScriptResult r = script.evaluate(
        "var b = document.getElementById('box'); window.scrollTo(0, 100);"
        "[b.offsetWidth, b.clientWidth, b.getBoundingClientRect().width, b.offsetParent, window.scrollY,"
        " innerWidth].join()");
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.value, "0,0,0,,0,1280");
}

// ---------------------------------------------------------------------------
// Scroll containers: elements with overflow other than visible
// ---------------------------------------------------------------------------

namespace {

const char* kScrollPage = R"HTML(<!DOCTYPE html><html><head></head><body>
<div id="list"><p id="first">1</p><p id="middle">2</p><p id="last">3</p><div id="nested"><p id="deep">d</p></div></div>
<div id="clipped"><p id="under">u</p></div>
<div id="plain"><p id="spill">s</p></div>
</body></html>)HTML";

/// #list: overflow-y: auto, 200 x 100 at (8, 8) with a 5px border and 10px
/// padding, holding three 40px rows and a nested 60px scroller (#nested,
/// overflow: scroll) whose content is 150px tall; content height
/// 10 + 3 * 40 + 60 + 10 = 200 (padding box 190 x 90, so max scrollTop 110).
/// #clipped: overflow: hidden, 100 x 50, content 120px tall.
/// #plain: overflow visible, 100 x 50, content 120px tall.
struct ScrollPage {
    std::unique_ptr<DOMDocument> doc;
    std::unique_ptr<ScriptContext> script;
    TestGeometry geometry;
    LayoutBox root;

    ScrollPage() {
        ZepraBrowser::elementScrollOffsets().clear();
        HTMLParser parser;
        doc = parser.parse(kScrollPage);
        script = std::make_unique<ScriptContext>();
        script->setConsoleHandler([](const std::string&, const std::string&) {});
        script->initialize(doc.get());

        root.width = 1200; root.height = 2000;
        LayoutBox& list = addBox(root, byId("list"), 8, 8, 200, 100);
        setBorder(list, 5);
        list.paddingTop = list.paddingRight = list.paddingBottom = list.paddingLeft = 10;
        list.overflowY = ZepraBrowser::kOverflowAuto;
        addBox(list, byId("first"), 15, 15, 170, 40);
        addBox(list, byId("middle"), 15, 55, 170, 40);
        addBox(list, byId("last"), 15, 95, 170, 40);
        LayoutBox& nested = addBox(list, byId("nested"), 15, 135, 170, 60);
        nested.overflowX = nested.overflowY = ZepraBrowser::kOverflowScroll;
        addBox(nested, byId("deep"), 0, 100, 170, 50);  // its content reaches 150

        LayoutBox& clipped = addBox(root, byId("clipped"), 8, 300, 100, 50);
        clipped.overflowX = clipped.overflowY = ZepraBrowser::kOverflowHidden;
        addBox(clipped, byId("under"), 0, 0, 100, 120);
        LayoutBox& plain = addBox(root, byId("plain"), 8, 400, 100, 50);
        addBox(plain, byId("spill"), 0, 0, 100, 120);

        geometry.page.root = &root;
        geometry.page.body = doc->body();
        geometry.page.documentElement = doc->documentElement();
        geometry.page.originX = 20;
        geometry.page.originY = 20;
        geometry.page.viewportWidth = 1240;
        geometry.page.viewportHeight = 700;
        geometry.reindex();
        script->setPageGeometry(&geometry);
    }
    ~ScrollPage() { ZepraBrowser::elementScrollOffsets().clear(); }

    DOMElement* byId(const std::string& id) { return doc->getElementById(id); }
    const LayoutBox& box(const std::string& id) { return *geometry.index.principalBox(byId(id)); }

    std::string eval(const std::string& code) {
        ScriptResult r = script->evaluate(code, "test.js");
        EXPECT_TRUE(r.success) << r.error << "\n  in: " << code;
        return r.success ? r.value : "<error: " + r.error + ">";
    }
};

}  // namespace

TEST(ScrollContainers, WhichBoxesScroll) {
    ScrollPage page;
    EXPECT_TRUE(ZepraBrowser::isScrollContainer(page.box("list")));
    EXPECT_TRUE(ZepraBrowser::isScrollContainer(page.box("clipped")));
    EXPECT_FALSE(ZepraBrowser::isScrollContainer(page.box("plain")));
    // overflow-y: auto alone makes x auto too (visible computes to auto)
    EXPECT_TRUE(ZepraBrowser::userScrollableY(page.box("list")));
    EXPECT_TRUE(ZepraBrowser::userScrollableX(page.box("list")));
    // hidden: scripts scroll it, the user does not
    EXPECT_FALSE(ZepraBrowser::userScrollableY(page.box("clipped")));
    // overflow: clip clips without being a scroll container
    LayoutBox clip;
    clip.domElement = page.byId("plain");
    clip.overflowX = clip.overflowY = ZepraBrowser::kOverflowClip;
    EXPECT_TRUE(ZepraBrowser::clipsContent(clip));
    EXPECT_FALSE(ZepraBrowser::isScrollContainer(clip));
    // Inline boxes and anonymous boxes never clip
    clip.overflowX = clip.overflowY = ZepraBrowser::kOverflowHidden;
    clip.type = LayoutType::Inline;
    EXPECT_FALSE(ZepraBrowser::clipsContent(clip));
}

TEST(ScrollContainers, RangeAndClamping) {
    ScrollPage page;
    const LayoutBox& list = page.box("list");
    float w = 0, h = 0;
    ZepraBrowser::scrollableSize(list, w, h);
    EXPECT_FLOAT_EQ(w, 190);
    EXPECT_FLOAT_EQ(h, 200);  // the nested scroller counts with its own 60px, not its 150px content
    ZepraBrowser::ScrollOffset max = ZepraBrowser::maxScrollOffset(list);
    EXPECT_FLOAT_EQ(max.x, 0);
    EXPECT_FLOAT_EQ(max.y, 110);
    EXPECT_TRUE(ZepraBrowser::scrollContainerTo(list, 50, 500));
    EXPECT_FLOAT_EQ(ZepraBrowser::scrollOffsetOf(list).x, 0);
    EXPECT_FLOAT_EQ(ZepraBrowser::scrollOffsetOf(list).y, 110);
    ZepraBrowser::scrollContainerTo(list, 0, std::nan(""));
    EXPECT_FLOAT_EQ(ZepraBrowser::scrollOffsetOf(list).y, 0);
    EXPECT_FALSE(ZepraBrowser::scrollContainerTo(page.box("plain"), 0, 10));
    // A stored position past a range that shrank reads clamped
    ZepraBrowser::elementScrollOffsets().set(page.byId("list"), {0, 1000});
    EXPECT_FLOAT_EQ(ZepraBrowser::scrollOffsetOf(list).y, 110);
}

TEST(ScrollContainers, ContentMovesWithTheScrollPosition) {
    ScrollPage page;
    ZepraBrowser::scrollContainerTo(page.box("list"), 0, 30);
    ZepraBrowser::scrollContainerTo(page.box("nested"), 0, 25);
    // #list itself does not move; its content moves up by 30
    EXPECT_DOUBLE_EQ(page.geometry.elementGeometry(page.byId("list")).rects[0].y, 20 + 8);
    EXPECT_DOUBLE_EQ(page.geometry.elementGeometry(page.byId("middle")).rects[0].y, 20 + 8 + 55 - 30);
    // Nested containers add up
    EXPECT_DOUBLE_EQ(page.geometry.elementGeometry(page.byId("deep")).rects[0].y, 20 + 8 + 135 + 100 - 30 - 25);
    // The page scroll comes on top
    page.geometry.page.scrollY = 5;
    EXPECT_DOUBLE_EQ(page.geometry.elementGeometry(page.byId("middle")).rects[0].y, 20 + 8 + 55 - 30 - 5);
    ElementGeometry g = page.geometry.elementGeometry(page.byId("list"));
    EXPECT_TRUE(g.scrollContainer);
    EXPECT_DOUBLE_EQ(g.scrollTop, 30);
    // placeBoxes (hit-testing) agrees
    ZepraBrowser::placeBoxes(page.root, 20, 20, 0);
    EXPECT_FLOAT_EQ(page.box("middle").screenY, 20 + 8 + 55 - 30);
    EXPECT_FLOAT_EQ(page.box("deep").screenY, 20 + 8 + 135 + 100 - 30 - 25);
}

TEST(ScrollContainers, ForgottenWhenTheElementStopsScrolling) {
    ScrollPage page;
    ZepraBrowser::scrollContainerTo(page.box("list"), 0, 30);
    ZepraBrowser::scrollContainerTo(page.box("clipped"), 0, 20);
    ZepraBrowser::elementScrollOffsets().prune(page.root);
    EXPECT_FLOAT_EQ(ZepraBrowser::elementScrollOffsets().get(page.byId("list")).y, 30);
    // #clipped restyled to overflow: visible
    page.root.children.begin()->overflowY = ZepraBrowser::kOverflowAuto;  // still #list
    std::next(page.root.children.begin())->overflowX = ZepraBrowser::kOverflowVisible;
    std::next(page.root.children.begin())->overflowY = ZepraBrowser::kOverflowVisible;
    ZepraBrowser::elementScrollOffsets().prune(page.root);
    EXPECT_FLOAT_EQ(ZepraBrowser::elementScrollOffsets().get(page.byId("clipped")).y, 0);
    EXPECT_FLOAT_EQ(ZepraBrowser::elementScrollOffsets().get(page.byId("list")).y, 30);
}

TEST(ScrollContainers, WheelPicksTheInnermostScrollerThatCanMove) {
    ScrollPage page;
    ZepraBrowser::placeBoxes(page.root, 20, 20, 0);
    // Over a row of #list: #list scrolls down
    const LayoutBox* found = ZepraBrowser::userScrollContainerAt(page.root, 50, 50, 0, 40);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->domElement, page.byId("list"));
    // Up while at the top: nothing to do, the page scrolls
    EXPECT_EQ(ZepraBrowser::userScrollContainerAt(page.root, 50, 50, 0, -40), nullptr);
    // #list at its end: #nested (135 - 110 = 25 into #list, at 53 .. 113 on
    // screen) is in view. Down over it: the inner one first
    ZepraBrowser::scrollContainerTo(page.box("list"), 0, 110);
    ZepraBrowser::placeBoxes(page.root, 20, 20, 0);
    found = ZepraBrowser::userScrollContainerAt(page.root, 50, 60, 0, 40);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->domElement, page.byId("nested"));
    // Up: #nested is at its top, so #list takes over
    found = ZepraBrowser::userScrollContainerAt(page.root, 50, 60, 0, -40);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->domElement, page.byId("list"));
    // Down with both at their end: the page
    ZepraBrowser::scrollContainerTo(page.box("nested"), 0, 90);
    EXPECT_EQ(ZepraBrowser::userScrollContainerAt(page.root, 50, 60, 0, 40), nullptr);
    // overflow: hidden is not scrolled by the wheel; outside every scroller: the page
    EXPECT_EQ(ZepraBrowser::userScrollContainerAt(page.root, 30, 20 + 300 + 10, 0, 40), nullptr);
    EXPECT_EQ(ZepraBrowser::userScrollContainerAt(page.root, 900, 50, 0, 40), nullptr);
    // A point below #list's box, where its scrolled-out content lies, is not in it
    EXPECT_EQ(ZepraBrowser::userScrollContainerAt(page.root, 50, 20 + 8 + 100 + 5, 0, 40), nullptr);
}

TEST(ScrollContainers, ScriptsReadAndMoveTheScrollPosition) {
    ScrollPage page;
    page.eval("var list = document.getElementById('list')");
    EXPECT_EQ(page.eval("[list.scrollTop, list.scrollHeight, list.clientHeight].join()"), "0,200,90");
    EXPECT_EQ(page.eval("list.scrollTop = 40; list.scrollTop"), "40");
    EXPECT_EQ(page.eval("document.getElementById('middle').getBoundingClientRect().top"), "43");  // 28 + 55 - 40
    // Clamped to the range, in both directions
    EXPECT_EQ(page.eval("list.scrollTop = 9999; list.scrollTop"), "110");
    EXPECT_EQ(page.eval("list.scrollTop = -5; list.scrollTop"), "0");
    // No horizontal overflow: scrollLeft stays 0
    EXPECT_EQ(page.eval("list.scrollLeft = 30; list.scrollLeft"), "0");
    // scrollTo / scrollBy, numbers and options
    EXPECT_EQ(page.eval("list.scrollTo(0, 25); list.scrollTop"), "25");
    EXPECT_EQ(page.eval("list.scrollBy(0, 10); list.scrollTop"), "35");
    EXPECT_EQ(page.eval("list.scrollBy({top: -100}); list.scrollTop"), "0");
    EXPECT_EQ(page.eval("list.scroll({top: 60}); list.scrollTop"), "60");
    // overflow: hidden scrolls from scripts
    EXPECT_EQ(page.eval("var c = document.getElementById('clipped'); c.scrollTop = 30; c.scrollTop"), "30");
    // The page did not move
    EXPECT_FLOAT_EQ(page.geometry.page.scrollY, 0);
}

TEST(ScrollContainers, ElementsThatDoNotScrollStayAtZero) {
    ScrollPage page;
    EXPECT_EQ(page.eval("var p = document.getElementById('plain'); p.scrollTop = 30; p.scrollTo(0, 40);"
                        "p.scrollTop + ',' + p.scrollHeight"), "0,120");
    EXPECT_EQ(page.eval("var f = document.getElementById('first'); f.scrollTop = 5; f.scrollTop"), "0");
    // <html> still scrolls the page
    EXPECT_EQ(page.eval("document.documentElement.scrollTop = 50; window.scrollY"), "50");
}

TEST(ScrollContainers, ScrollIntoViewScrollsTheContainersFirst) {
    ScrollPage page;
    // #last (95..135 from #list's border edge) comes to the top of the
    // scrollport, #list's padding edge (5px down)
    page.eval("document.getElementById('last').scrollIntoView()");
    EXPECT_EQ(page.eval("document.getElementById('list').scrollTop"), "90");
    EXPECT_EQ(page.eval("var r = document.getElementById('last').getBoundingClientRect();"
                        "var l = document.getElementById('list').getBoundingClientRect(); r.top - l.top"), "5");
    // nearest: already visible, nothing moves
    page.eval("document.getElementById('last').scrollIntoView({block: 'nearest'})");
    EXPECT_EQ(page.eval("document.getElementById('list').scrollTop"), "90");
    // end, through two containers: #deep (100..150 in #nested) bottom at #nested's bottom
    page.eval("document.getElementById('deep').scrollIntoView(false)");
    EXPECT_EQ(page.eval("document.getElementById('nested').scrollTop"), "90");
    // then #deep (now 145..195 from #list's border edge) at the bottom of
    // #list's scrollport (5 + 90): 195 - 95
    EXPECT_EQ(page.eval("document.getElementById('list').scrollTop"), "100");
}
