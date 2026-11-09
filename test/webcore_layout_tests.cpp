// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * webcore_layout_tests.cpp - Default styles, presentational hints, cascade
 * fixes and table layout, without a window.
 *
 * Layout runs with the layout engine's fallback text measurement
 * (0.5 x font size per byte), so widths below are exact.
 */

#include <gtest/gtest.h>

#include <array>

#include "browser/dom.hpp"
#include "browser/gradient_raster.h"
#include "browser/layout_engine.h"
#include "css/css_engine.hpp"
#include "css/presentational_hints.hpp"
#include "css/ua_stylesheet.h"
#include "html/html_parser.hpp"

#include <memory>
#include <string>
#include <vector>

// Viewport size the layout engine reads (zepra_browser.cpp defines these)
int g_width = 1280;
int g_height = 800;

using namespace Zepra::WebCore;
using ZepraBrowser::LayoutBox;
using ZepraBrowser::LayoutType;

namespace {

struct StyledDoc {
    std::unique_ptr<DOMDocument> doc;
    std::unique_ptr<CSSEngine> css;

    explicit StyledDoc(const std::string& html, const std::string& authorCss = "") {
        HTMLParser parser;
        doc = parser.parse(html);
        css = std::make_unique<CSSEngine>();
        css->initialize(doc.get());
        css->addStyleSheet(ZepraUAStylesheet::getStylesheet(), StyleOrigin::UserAgent);
        if (doc->quirksMode()) css->addStyleSheet(ZepraUAStylesheet::getQuirksStylesheet(), StyleOrigin::UserAgent);
        if (!authorCss.empty()) css->addStyleSheet(authorCss, StyleOrigin::Author);
    }
    const CSSComputedStyle* style(const std::string& id) {
        return css->getComputedStyle(doc->getElementById(id));
    }
};

LayoutBox& addBox(LayoutBox& parent, LayoutType type) {
    parent.children.emplace_back();
    parent.children.back().type = type;
    return parent.children.back();
}

// A cell holding one text run; text width is 8px per character at 16px
LayoutBox& addCell(LayoutBox& row, const std::string& text, int colSpan = 1, int rowSpan = 1) {
    LayoutBox& cell = addBox(row, LayoutType::TableCell);
    cell.colSpan = colSpan;
    cell.rowSpan = rowSpan;
    LayoutBox& run = addBox(cell, LayoutType::Inline);
    run.text = text;
    return cell;
}

LayoutBox makeTable(float spacing = 0) {
    LayoutBox table;
    table.type = LayoutType::Table;
    table.boxSizing = 1;
    table.borderSpacingH = table.borderSpacingV = spacing;
    return table;
}

LayoutBox* child(LayoutBox& box, size_t index) {
    auto it = box.children.begin();
    std::advance(it, index);
    return &*it;
}

} // namespace

// ---------------------------------------------------------------------------
// Presentational hint parsers (HTML Standard)
// ---------------------------------------------------------------------------

TEST(LegacyColor, HexNamesAndJunk) {
    EXPECT_EQ(parseLegacyColor("#ff6600"), "#ff6600");
    EXPECT_EQ(parseLegacyColor("ff6600"), "#ff6600");
    EXPECT_EQ(parseLegacyColor("#f60"), "#ff6600");
    EXPECT_EQ(parseLegacyColor("Red"), "red");
    EXPECT_EQ(parseLegacyColor("chucknorris"), "#c00000");
    EXPECT_EQ(parseLegacyColor("transparent"), "");
    EXPECT_EQ(parseLegacyColor("  "), "");
}

TEST(LegacyFontSize, AbsoluteAndRelative) {
    EXPECT_EQ(parseLegacyFontSize("3"), 3);
    EXPECT_EQ(parseLegacyFontSize("+2"), 5);
    EXPECT_EQ(parseLegacyFontSize("-1"), 2);
    EXPECT_EQ(parseLegacyFontSize("9"), 7);
    EXPECT_EQ(parseLegacyFontSize("x"), 0);
}

// ---------------------------------------------------------------------------
// Default styles and hints through the cascade
// ---------------------------------------------------------------------------

TEST(DefaultStyles, TablePartsGetTableDisplay) {
    StyledDoc d("<!doctype html><table id=t><thead id=h><tr id=r><th id=th>a</th><td id=td>b</td></tr></thead></table>");
    EXPECT_EQ(d.style("t")->display, DisplayValue::Table);
    EXPECT_EQ(d.style("h")->display, DisplayValue::TableHeaderGroup);
    EXPECT_EQ(d.style("r")->display, DisplayValue::TableRow);
    EXPECT_EQ(d.style("td")->display, DisplayValue::TableCell);
    EXPECT_EQ(d.style("th")->display, DisplayValue::TableCell);
    EXPECT_FLOAT_EQ(d.style("t")->borderSpacingH, 2.0f);
    EXPECT_EQ(d.style("td")->verticalAlign, "middle");   // inherited from the row group
    EXPECT_GE(d.style("th")->fontWeight, FontWeight::Bold);
    EXPECT_EQ(d.style("th")->textAlign, TextAlign::Center);
}

TEST(DefaultStyles, HeadingMarginsResolveEm) {
    StyledDoc d("<!doctype html><h1 id=a>x</h1><p id=p>y</p>");
    EXPECT_FLOAT_EQ(d.style("a")->fontSize, 32.0f);
    EXPECT_EQ(d.style("a")->marginTop.unit, CSSLength::Unit::Px);
    EXPECT_NEAR(d.style("a")->marginTop.value, 21.44f, 0.01f);
    EXPECT_FLOAT_EQ(d.style("p")->marginTop.value, 16.0f);
}

TEST(PresentationalHints, TableAndCellAttributes) {
    StyledDoc d("<!doctype html><table id=t width=85% bgcolor=ff6600 cellspacing=0 cellpadding=3 border=1>"
                "<tr><td id=c align=right valign=top nowrap width=40>x</td></tr></table>");
    const CSSComputedStyle* t = d.style("t");
    EXPECT_EQ(t->width.unit, CSSLength::Unit::Percent);
    EXPECT_FLOAT_EQ(t->width.value, 85.0f);
    EXPECT_EQ(t->backgroundColor.r, 0xff);
    EXPECT_EQ(t->backgroundColor.g, 0x66);
    EXPECT_FLOAT_EQ(t->borderSpacingH, 0.0f);
    EXPECT_FLOAT_EQ(t->borderTopWidth, 1.0f);
    const CSSComputedStyle* c = d.style("c");
    EXPECT_EQ(c->textAlign, TextAlign::Right);
    EXPECT_EQ(c->verticalAlign, "top");
    EXPECT_EQ(c->whiteSpace, "nowrap");
    EXPECT_FLOAT_EQ(c->width.value, 40.0f);
    EXPECT_FLOAT_EQ(c->paddingLeft.value, 3.0f);   // cellpadding
    EXPECT_FLOAT_EQ(c->borderTopWidth, 1.0f);      // table border=1
}

TEST(PresentationalHints, AuthorRulesWinOverHints) {
    StyledDoc d("<!doctype html><table><tr><td id=c bgcolor=red>x</td></tr></table>",
                "td { background-color: #0000ff; }");
    EXPECT_EQ(d.style("c")->backgroundColor.b, 0xff);
    EXPECT_EQ(d.style("c")->backgroundColor.r, 0x00);
}

TEST(PresentationalHints, FontAndCenter) {
    StyledDoc d("<!doctype html><center id=ce><font id=f size=5 color=#123456>x</font></center>");
    EXPECT_EQ(d.style("ce")->textAlign, TextAlign::Center);
    EXPECT_EQ(d.style("ce")->legacyAlign, 2);
    EXPECT_FLOAT_EQ(d.style("f")->fontSize, 24.0f);
    EXPECT_EQ(d.style("f")->color.b, 0x56);
    EXPECT_EQ(d.style("f")->display, DisplayValue::Inline);
}

TEST(Quirks, TablesResetTextAlign) {
    const char* body = "<div style='text-align:center'><table><tr><td id=c>x</td></tr></table></div>";
    StyledDoc quirks(body);
    EXPECT_TRUE(quirks.doc->quirksMode());
    EXPECT_NE(quirks.style("c")->textAlign, TextAlign::Center);
    StyledDoc standards(std::string("<!doctype html>") + body);
    EXPECT_FALSE(standards.doc->quirksMode());
    EXPECT_EQ(standards.style("c")->textAlign, TextAlign::Center);
}

TEST(DefaultStyles, CenterStopsAtTables) {
    StyledDoc d("<!doctype html><center><div id=d>x</div><table><tr><td id=c>x</td>"
                "<td id=a align=center>y</td></tr></table></center>");
    EXPECT_EQ(d.style("d")->textAlign, TextAlign::Center);
    EXPECT_NE(d.style("c")->textAlign, TextAlign::Center);
    EXPECT_EQ(d.style("a")->textAlign, TextAlign::Center);
}

// ---------------------------------------------------------------------------
// Cascade fixes found on the way
// ---------------------------------------------------------------------------

TEST(Cascade, EverySelectorOfAListMatches) {
    StyledDoc d("<!doctype html><p id=a>x</p><span id=b>y</span>", "p, span { color: #00ff00; }");
    EXPECT_EQ(d.style("a")->color.g, 0xff);
    EXPECT_EQ(d.style("b")->color.g, 0xff);
}

TEST(Cascade, BorderShorthandReadsTheWidth) {
    StyledDoc d("<!doctype html><div id=a>x</div>", "#a { border: 2px solid #888; }");
    EXPECT_FLOAT_EQ(d.style("a")->borderTopWidth, 2.0f);
    EXPECT_EQ(d.style("a")->borderTopColor.r, 0x88);
}

TEST(Cascade, PseudoElementRulesSkipTheElement) {
    StyledDoc d("<!doctype html><div id=a>x</div>", "div::before { font-size: 4px; } div:after { color: red; }");
    EXPECT_FLOAT_EQ(d.style("a")->fontSize, 16.0f);
}

// ---------------------------------------------------------------------------
// Table layout (CSS 2.1 §17)
// ---------------------------------------------------------------------------

TEST(TableLayout, AutoWidthShrinksToContent) {
    LayoutBox table = makeTable(2);
    LayoutBox& row = addBox(table, LayoutType::TableRow);
    addCell(row, "abcd");      // 32px
    addCell(row, "abcdefgh");  // 64px
    ZepraBrowser::layoutBlock(table, 1000, 0);
    EXPECT_FLOAT_EQ(table.width, 32 + 64 + 3 * 2);
    EXPECT_FLOAT_EQ(child(row, 0)->width, 32);
    EXPECT_FLOAT_EQ(child(row, 1)->x, 2 + 32 + 2 - 2);  // relative to the row, which starts at the first column
}

TEST(TableLayout, ColumnsAlignAcrossRows) {
    LayoutBox table = makeTable(0);
    LayoutBox& r1 = addBox(table, LayoutType::TableRow);
    addCell(r1, "1.");
    addCell(r1, "A long title here");
    LayoutBox& r2 = addBox(table, LayoutType::TableRow);
    addCell(r2, "10.");
    addCell(r2, "short");
    ZepraBrowser::layoutBlock(table, 1000, 0);
    EXPECT_FLOAT_EQ(child(r1, 1)->x, child(r2, 1)->x);
    EXPECT_FLOAT_EQ(child(r1, 0)->width, 24);  // widest rank, "10."
    EXPECT_FLOAT_EQ(r2.y, r1.y + r1.height);
}

TEST(TableLayout, SpecifiedWidthSpreadsToAutoColumns) {
    LayoutBox table = makeTable(0);
    table.cssWidth = ZepraBrowser::LayoutLength::pct(50);
    LayoutBox& row = addBox(table, LayoutType::TableRow);
    LayoutBox& fixed = addCell(row, "ab");
    fixed.cssWidth = ZepraBrowser::LayoutLength::px(100);
    addCell(row, "x");
    ZepraBrowser::layoutBlock(table, 800, 0);
    EXPECT_FLOAT_EQ(table.width, 400);
    EXPECT_FLOAT_EQ(child(row, 0)->width, 100);
    EXPECT_FLOAT_EQ(child(row, 1)->width, 300);
}

TEST(TableLayout, ColspanAndRowspan) {
    LayoutBox table = makeTable(0);
    LayoutBox& group = addBox(table, LayoutType::TableRowGroup);
    LayoutBox& r1 = addBox(group, LayoutType::TableRow);
    addCell(r1, "header spanning two", 2);
    LayoutBox& r2 = addBox(group, LayoutType::TableRow);
    LayoutBox& tall = addCell(r2, "tall", 1, 2);
    addCell(r2, "b1");
    LayoutBox& r3 = addBox(group, LayoutType::TableRow);
    LayoutBox& b2 = addCell(r3, "b2");
    ZepraBrowser::layoutBlock(table, 1000, 0);
    // The spanning header sets the table width; the rowspan cell covers both rows
    EXPECT_FLOAT_EQ(table.width, 19 * 8);
    EXPECT_FLOAT_EQ(tall.height, r2.height + r3.height);
    // b2 lands in the second column, beside the rowspan cell
    EXPECT_FLOAT_EQ(b2.x, child(r2, 1)->x);
}

TEST(TableLayout, VerticalAlignMiddle) {
    LayoutBox table = makeTable(0);
    LayoutBox& row = addBox(table, LayoutType::TableRow);
    row.cssHeight = ZepraBrowser::LayoutLength::px(60);
    LayoutBox& cell = addCell(row, "mid");
    cell.verticalAlign = 2;
    ZepraBrowser::layoutBlock(table, 1000, 0);
    EXPECT_FLOAT_EQ(row.height, 60);
    float textH = 16 + 4;
    EXPECT_FLOAT_EQ(cell.children.front().y, (60 - textH) / 2);
}

TEST(TableLayout, FixedLayoutUsesFirstRow) {
    LayoutBox table = makeTable(0);
    table.tableLayoutFixed = true;
    table.cssWidth = ZepraBrowser::LayoutLength::px(300);
    LayoutBox& row = addBox(table, LayoutType::TableRow);
    LayoutBox& first = addCell(row, "a very long first cell that would be wide");
    first.cssWidth = ZepraBrowser::LayoutLength::px(60);
    addCell(row, "b");
    addCell(row, "c");
    ZepraBrowser::layoutBlock(table, 1000, 0);
    EXPECT_FLOAT_EQ(child(row, 0)->width, 60);
    EXPECT_FLOAT_EQ(child(row, 1)->width, 120);
    EXPECT_FLOAT_EQ(child(row, 2)->width, 120);
}

TEST(TableLayout, EmptyRowKeepsItsHeight) {
    LayoutBox table = makeTable(0);
    LayoutBox& spacer = addBox(table, LayoutType::TableRow);
    spacer.cssHeight = ZepraBrowser::LayoutLength::px(10);
    LayoutBox& row = addBox(table, LayoutType::TableRow);
    addCell(row, "x");
    ZepraBrowser::layoutBlock(table, 1000, 0);
    EXPECT_FLOAT_EQ(row.y, 10);
}

// ---------------------------------------------------------------------------
// Flex alignment, image sizing and repeatable layout
// ---------------------------------------------------------------------------

using ZepraBrowser::LayoutLength;

LayoutBox makeFlex(int direction, int alignItems) {
    LayoutBox flex;
    flex.type = LayoutType::Flex;
    flex.flexDirection = direction;
    flex.alignItems = alignItems;
    flex.boxSizing = 1;
    return flex;
}

LayoutBox& addImage(LayoutBox& parent, float naturalW, float naturalH) {
    LayoutBox& img = addBox(parent, LayoutType::InlineBlock);
    img.isImage = true;
    img.naturalWidth = naturalW;
    img.naturalHeight = naturalH;
    return img;
}

TEST(AbsoluteLayout, InsetsStretchAnAutoSizedBox) {
    LayoutBox parent;
    parent.type = LayoutType::Block;
    parent.positionType = 1;
    parent.cssWidth = LayoutLength::px(120);
    parent.cssHeight = LayoutLength::px(80);
    parent.borderLeft = parent.borderTop = 2;
    parent.paddingLeft = parent.paddingTop = 10;
    parent.boxSizing = 1;
    LayoutBox& cover = addBox(parent, LayoutType::Block);
    cover.positionType = 2;
    cover.cssTop = cover.cssRight = cover.cssBottom = cover.cssLeft = LayoutLength::px(0);
    LayoutBox& inset = addBox(parent, LayoutType::Block);
    inset.positionType = 2;
    inset.cssTop = LayoutLength::px(5);
    inset.cssBottom = LayoutLength::px(15);
    inset.cssLeft = LayoutLength::px(4);
    inset.cssRight = LayoutLength::px(6);
    inset.paddingLeft = 3;  // content-box: the padding stays inside the stretched size
    ZepraBrowser::layoutBlock(parent, 1000, 0);
    // inset: 0 covers the padding box (inside the border, over the padding)
    EXPECT_FLOAT_EQ(cover.x, 2);
    EXPECT_FLOAT_EQ(cover.y, 2);
    EXPECT_FLOAT_EQ(cover.width, 118);
    EXPECT_FLOAT_EQ(cover.height, 78);
    EXPECT_FLOAT_EQ(inset.x, 6);
    EXPECT_FLOAT_EQ(inset.y, 7);
    EXPECT_FLOAT_EQ(inset.width, 108);
    EXPECT_FLOAT_EQ(inset.height, 58);
    // The specified (auto) sizes are kept for the next layout pass
    EXPECT_FALSE(cover.cssWidth.isSet() && !cover.cssWidth.isAuto());
}

TEST(FlexLayout, ColumnAlignCenterCentresHorizontally) {
    LayoutBox flex = makeFlex(1, 3);
    LayoutBox& item = addBox(flex, LayoutType::Block);
    addBox(item, LayoutType::Inline).text = "abcdefghij";  // 80px
    ZepraBrowser::layoutBlock(flex, 1000, 0);
    EXPECT_FLOAT_EQ(item.width, 80);
    EXPECT_FLOAT_EQ(item.x, (1000 - 80) / 2.0f);
}

TEST(FlexLayout, ColumnStretchFillsTheWidth) {
    LayoutBox flex = makeFlex(1, 0);
    LayoutBox& item = addBox(flex, LayoutType::Block);
    addBox(item, LayoutType::Inline).text = "abcdefghij";
    ZepraBrowser::layoutBlock(flex, 1000, 0);
    EXPECT_FLOAT_EQ(item.width, 1000);
    EXPECT_FLOAT_EQ(item.x, 0);
}

TEST(FlexLayout, RowAlignCenterUsesTheMinHeight) {
    LayoutBox flex = makeFlex(0, 3);
    flex.cssMinHeight = LayoutLength::px(48);
    flex.paddingTop = flex.paddingBottom = 8;
    LayoutBox& item = addBox(flex, LayoutType::Block);
    item.cssWidth = item.cssHeight = LayoutLength::px(24);
    ZepraBrowser::layoutBlock(flex, 600, 0);
    EXPECT_FLOAT_EQ(flex.height, 48);
    EXPECT_FLOAT_EQ(item.y, 8 + (32 - 24) / 2.0f);
}

TEST(FlexLayout, GrownItemLaysOutItsContentAtItsWidth) {
    LayoutBox flex = makeFlex(0, 3);
    LayoutBox& grow = addBox(flex, LayoutType::Block);
    grow.flexGrow = 1;
    grow.textAlign = 1;
    LayoutBox& run = addBox(grow, LayoutType::Inline);
    run.text = "centre";  // 48px
    addImage(flex, 24, 24);
    ZepraBrowser::layoutBlock(flex, 400, 0);
    EXPECT_FLOAT_EQ(grow.width, 376);
    EXPECT_FLOAT_EQ(run.x, (376 - 48) / 2.0f);
}

TEST(ImageLayout, CssWidthKeepsTheAspectRatio) {
    LayoutBox block;
    LayoutBox& img = addImage(block, 24, 12);
    img.cssWidth = LayoutLength::px(96);
    ZepraBrowser::layoutBlock(block, 500, 0);
    EXPECT_FLOAT_EQ(img.width, 96);
    EXPECT_FLOAT_EQ(img.height, 48);
}

TEST(ImageLayout, MaxWidthScalesDown) {
    LayoutBox block;
    LayoutBox& img = addImage(block, 400, 200);
    img.cssMaxWidth = LayoutLength::pct(50);
    ZepraBrowser::layoutBlock(block, 300, 0);
    EXPECT_FLOAT_EQ(img.width, 150);
    EXPECT_FLOAT_EQ(img.height, 75);
}

TEST(ImageLayout, CssSizeAndMarginsInAFlexRow) {
    LayoutBox flex = makeFlex(0, 1);
    flex.gap = 10;
    LayoutBox& img = addImage(flex, 24, 24);
    img.cssWidth = img.cssHeight = LayoutLength::px(48);
    img.marginLeft = 4;
    img.marginTop = 6;
    LayoutBox& next = addImage(flex, 24, 24);
    ZepraBrowser::layoutBlock(flex, 600, 0);
    EXPECT_FLOAT_EQ(img.width, 48);
    EXPECT_FLOAT_EQ(img.height, 48);
    EXPECT_FLOAT_EQ(img.x, 4);
    EXPECT_FLOAT_EQ(img.y, 6);
    EXPECT_FLOAT_EQ(next.x, 4 + 48 + 10);
    EXPECT_FLOAT_EQ(next.width, 24);
}

TEST(ImageLayout, PlaceholderUntilLoaded) {
    LayoutBox block;
    LayoutBox& img = addImage(block, 0, 0);
    img.text = "[IMG]";  // 40px
    ZepraBrowser::layoutBlock(block, 500, 0);
    EXPECT_FLOAT_EQ(img.width, 40);
    img.cssWidth = img.cssHeight = LayoutLength::px(30);
    ZepraBrowser::layoutBlock(block, 500, 0);
    EXPECT_FLOAT_EQ(img.width, 30);
    EXPECT_FLOAT_EQ(img.height, 30);
}

void collectGeometry(const LayoutBox& box, std::vector<float>& out) {
    out.insert(out.end(), {box.x, box.y, box.width, box.height, box.marginLeft, box.marginTop});
    for (const auto& c : box.children) collectGeometry(c, out);
}

TEST(RepeatableLayout, SecondPassMatchesTheFirst) {
    ZepraBrowser::setLayoutViewport(1000, 600);
    LayoutBox root = makeFlex(1, 3);
    root.justifyContent = 2;
    root.cssMinHeight = {100, LayoutLength::Unit::Vh};
    LayoutBox& logo = addBox(root, LayoutType::Flex);
    logo.justifyContent = 2;
    logo.marginBottom = 40;
    addImage(logo, 104, 113);
    LayoutBox& pill = addBox(root, LayoutType::Flex);
    pill.alignItems = 3;
    pill.boxSizing = 1;
    pill.cssWidth = LayoutLength::px(400);
    pill.cssMinHeight = LayoutLength::px(48);
    pill.marginLeftAuto = pill.marginRightAuto = true;
    LayoutBox& grow = addBox(pill, LayoutType::Block);
    grow.flexGrow = 1;
    addBox(grow, LayoutType::Inline).text = "search";
    addImage(pill, 24, 24);

    ZepraBrowser::layoutBlock(root, 1000, 0);
    std::vector<float> first, second;
    collectGeometry(root, first);
    ZepraBrowser::layoutBlock(root, 1000, 0);
    collectGeometry(root, second);
    EXPECT_EQ(first, second);
    EXPECT_FLOAT_EQ(root.height, 600);
    EXPECT_FLOAT_EQ(pill.x, 300);
    // Column content (113 + 40 + 48) centred in the 600px viewport
    EXPECT_FLOAT_EQ(logo.y, (600 - (113 + 40 + 48)) / 2.0f);
    ZepraBrowser::setLayoutViewport(0, 0);
}

TEST(Cascade, AlignSelfDefaultsToAuto) {
    StyledDoc d("<!doctype html><div id=a></div><div id=b></div>", "#b { align-self: auto; }");
    EXPECT_EQ(d.style("a")->alignSelf, JustifyAlign::Auto);
    EXPECT_EQ(d.style("b")->alignSelf, JustifyAlign::Auto);
}

// ---------------------------------------------------------------------------
// Gradient backgrounds (painted through recording callbacks)
// ---------------------------------------------------------------------------

struct GradientPiece {
    float x, y, w, h;
    uint32_t c1, c2;
    bool horizontal;
};
std::vector<GradientPiece> g_pieces;

std::vector<GradientPiece> paintGradientBox(const std::string& css, float w, float h) {
    g_pieces.clear();
    ZepraBrowser::setLayoutCallbacks(
        [](float, float, float, float, uint32_t) {}, [](float, float, float, float, uint32_t, float) {},
        nullptr, nullptr, nullptr, nullptr);
    ZepraBrowser::setLayoutCallbacks2(
        nullptr,
        [](float x, float y, float w, float h, uint32_t a, uint32_t b) { g_pieces.push_back({x, y, w, h, a, b, false}); },
        nullptr,
        [](float x, float y, float w, float h, uint32_t a, uint32_t b) { g_pieces.push_back({x, y, w, h, a, b, true}); });
    LayoutBox box;
    box.width = w;
    box.height = h;
    box.hasBgColor = true;
    box.backgroundImage = css;
    ZepraBrowser::paintBox(box, 0, 0, 10000, 0);
    ZepraBrowser::setLayoutCallbacks(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    ZepraBrowser::setLayoutCallbacks2(nullptr, nullptr, nullptr, nullptr);
    return g_pieces;
}

TEST(Gradients, ThreeStopsStackTopToBottom) {
    auto pieces = paintGradientBox("linear-gradient(#ff0000, #00ff00, #0000ff)", 100, 200);
    ASSERT_EQ(pieces.size(), 2u);
    EXPECT_FALSE(pieces[0].horizontal);
    EXPECT_FLOAT_EQ(pieces[0].y, 0);
    EXPECT_FLOAT_EQ(pieces[1].y, 100);
    EXPECT_EQ(pieces[0].c1, 0xFF0000u);
    EXPECT_EQ(pieces[0].c2, 0x00FF00u);
    EXPECT_EQ(pieces[1].c2, 0x0000FFu);
}

TEST(Gradients, ToRightWithAPosition) {
    auto pieces = paintGradientBox("linear-gradient(to right, #ff0000, #00ff00 25%, #0000ff)", 200, 50);
    ASSERT_EQ(pieces.size(), 2u);
    EXPECT_TRUE(pieces[0].horizontal);
    EXPECT_FLOAT_EQ(pieces[1].x, 50);
    EXPECT_EQ(pieces[1].c1, 0x00FF00u);
}

TEST(Gradients, ToTopRunsBottomToTop) {
    auto pieces = paintGradientBox("linear-gradient(0deg, #000000, #ffffff)", 100, 100);
    ASSERT_EQ(pieces.size(), 1u);
    // Drawn top to bottom, so the top edge has the end colour
    EXPECT_EQ(pieces[0].c1, 0xFFFFFFu);
    EXPECT_EQ(pieces[0].c2, 0x000000u);
}

TEST(Gradients, DiagonalIsDrawnInRows) {
    auto pieces = paintGradientBox("linear-gradient(135deg, #000000, #ffffff)", 100, 100);
    ASSERT_GT(pieces.size(), 10u);
    // The top-left corner starts black, the bottom-right ends white
    EXPECT_EQ(pieces.front().c1 & 0xFF, pieces.front().c1 >> 16);
    EXPECT_LT(pieces.front().c1 & 0xFF, 0x10u);
    EXPECT_GT(pieces.back().c2 & 0xFF, 0xF0u);
}

TEST(Gradients, OnlyTheTopLayerAndRadialShapes) {
    auto layered = paintGradientBox("linear-gradient(#ff0000, #00ff00), linear-gradient(#0000ff, #ffffff)", 100, 100);
    ASSERT_EQ(layered.size(), 1u);
    EXPECT_EQ(layered[0].c2, 0x00FF00u);
    // Radial gradients are drawn as their first colour, not the shape keyword
    g_pieces.clear();
    uint32_t solid = 0;
    ZepraBrowser::setLayoutCallbacks(
        [](float, float, float, float, uint32_t c) { g_pieces.push_back({0, 0, 0, 0, c, c, false}); },
        [](float, float, float, float, uint32_t, float) {}, nullptr, nullptr, nullptr, nullptr);
    ZepraBrowser::setLayoutCallbacks2(nullptr, [](float, float, float, float, uint32_t, uint32_t) {});
    LayoutBox box;
    box.width = box.height = 10;
    box.hasBgColor = true;
    box.backgroundImage = "radial-gradient(circle, #ff6b6b, #4ecdc4)";
    ZepraBrowser::paintBox(box, 0, 0, 10000, 0);
    if (!g_pieces.empty()) solid = g_pieces[0].c1;
    ZepraBrowser::setLayoutCallbacks(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    ZepraBrowser::setLayoutCallbacks2(nullptr, nullptr);
    EXPECT_EQ(solid, 0xFF6B6Bu);
}

// ---------------------------------------------------------------------------
// Gradient backgrounds drawn on the CPU (gradient_raster.cpp)
// ---------------------------------------------------------------------------

struct Raster {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    std::array<int, 4> at(int x, int y) const {
        const uint8_t* p = &px[((size_t)y * w + x) * 4];
        return {p[0], p[1], p[2], p[3]};
    }
};

Raster raster(const std::string& image, float w, float h, const std::string& size = "",
              const std::string& position = "", const std::string& repeat = "", float radius = 0) {
    ZepraBrowser::GradientBackground bg;
    bg.image = image;
    bg.size = size;
    bg.position = position;
    bg.repeat = repeat;
    bg.width = w;
    bg.height = h;
    bg.radius = radius;
    Raster r;
    ZepraBrowser::gradientPixelSize(w, h, r.w, r.h);
    EXPECT_TRUE(ZepraBrowser::rasterizeGradientBackground(bg, r.w, r.h, r.px)) << image;
    if (r.px.empty()) r.px.assign((size_t)r.w * r.h * 4, 0);
    return r;
}

using RGBA = std::array<int, 4>;

::testing::AssertionResult near(const RGBA& actual, const RGBA& expected, int tolerance) {
    for (int i = 0; i < 4; i++) {
        if (std::abs(actual[i] - expected[i]) > tolerance) {
            return ::testing::AssertionFailure() << "(" << actual[0] << "," << actual[1] << "," << actual[2] << ","
                                                 << actual[3] << ") is not within " << tolerance << " of ("
                                                 << expected[0] << "," << expected[1] << "," << expected[2] << ","
                                                 << expected[3] << ")";
        }
    }
    return ::testing::AssertionSuccess();
}

using RGBA = std::array<int, 4>;

TEST(GradientRaster, LinearRunsAlongItsAngle) {
    Raster r = raster("linear-gradient(to right, #000, #fff)", 100, 10);
    EXPECT_TRUE(near(r.at(0, 5), RGBA{1, 1, 1, 255}, 4));
    EXPECT_TRUE(near(r.at(50, 5), RGBA{129, 129, 129, 255}, 4));
    EXPECT_TRUE(near(r.at(99, 5), RGBA{254, 254, 254, 255}, 4));
    // to top right: the bottom-left corner starts, the top-right corner ends
    Raster corner = raster("linear-gradient(to top right, #f00, #00f)", 200, 50);
    EXPECT_TRUE(near(corner.at(0, 49), RGBA{253, 0, 2, 255}, 6));
    EXPECT_TRUE(near(corner.at(199, 0), RGBA{2, 0, 253, 255}, 6));
}

TEST(GradientRaster, TransparentStopsFadeWithoutDarkening) {
    // Premultiplied interpolation: half way is red at half alpha, not grey
    Raster r = raster("linear-gradient(to right, red, transparent)", 101, 1);
    RGBA mid = r.at(50, 0);
    EXPECT_EQ(mid[0], 255);
    EXPECT_NEAR(mid[3], 128, 4);
}

TEST(GradientRaster, RadialCircleAndEllipse) {
    Raster r = raster("radial-gradient(circle closest-side, #fff, #000)", 100, 60);
    EXPECT_TRUE(near(r.at(50, 30), RGBA{253, 253, 253, 255}, 6));
    // closest-side is 30px: 30px right of the centre is the end colour
    EXPECT_TRUE(near(r.at(81, 30), RGBA{0, 0, 0, 255}, 6));
    // An ellipse reaches its end at each side (farthest-side here)
    Raster e = raster("radial-gradient(farthest-side at 0 0, #fff, #000)", 100, 40);
    EXPECT_TRUE(near(e.at(99, 0), RGBA{0, 0, 0, 255}, 8));
    EXPECT_TRUE(near(e.at(0, 39), RGBA{0, 0, 0, 255}, 8));
    EXPECT_TRUE(near(e.at(49, 0), RGBA{128, 128, 128, 255}, 8));
}

TEST(GradientRaster, ConicTurnsClockwiseFromItsStart) {
    Raster r = raster("conic-gradient(#f00 0 25%, #0f0 0 50%, #00f 0 75%, #ff0 0)", 100, 100);
    EXPECT_TRUE(near(r.at(80, 20), RGBA{255, 0, 0, 255}, 0));    // top right
    EXPECT_TRUE(near(r.at(80, 80), RGBA{0, 255, 0, 255}, 0));    // bottom right
    EXPECT_TRUE(near(r.at(20, 80), RGBA{0, 0, 255, 255}, 0));    // bottom left
    EXPECT_TRUE(near(r.at(20, 20), RGBA{255, 255, 0, 255}, 0));  // top left
    Raster from = raster("conic-gradient(from 90deg, #f00 0 50%, #00f 0)", 100, 100);
    EXPECT_TRUE(near(from.at(50, 90), RGBA{255, 0, 0, 255}, 0));  // right half turns to the bottom
    EXPECT_TRUE(near(from.at(50, 10), RGBA{0, 0, 255, 255}, 0));
}

TEST(GradientRaster, RepeatingStripes) {
    Raster r = raster("repeating-linear-gradient(to right, #000 0 10px, #fff 10px 20px)", 60, 1);
    EXPECT_EQ(r.at(5, 0)[0], 0);
    EXPECT_EQ(r.at(15, 0)[0], 255);
    EXPECT_EQ(r.at(45, 0)[0], 0);
    EXPECT_EQ(r.at(55, 0)[0], 255);
}

TEST(GradientRaster, LayersWithSizePositionAndRepeat) {
    // The checkerboard of zepracolor.html: four layers, 16px tiles
    Raster r = raster("linear-gradient(45deg, #333 25%, transparent 25%),"
                      "linear-gradient(-45deg, #333 25%, transparent 25%),"
                      "linear-gradient(45deg, transparent 75%, #333 75%),"
                      "linear-gradient(-45deg, transparent 75%, #333 75%)",
                      64, 32, "16px 16px", "0 0, 0 8px, 8px -8px, -8px 0px");
    // Alternating 8px squares, the top-left one clear, and no gaps on the
    // diagonals where two layers' triangles meet
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 64; x++) {
            bool filled = ((x / 8) + (y / 8)) % 2 == 1;
            ASSERT_EQ(r.at(x, y)[3], filled ? 255 : 0) << x << "," << y;
            if (filled) ASSERT_TRUE(near(r.at(x, y), RGBA{0x33, 0x33, 0x33, 255}, 0));
        }
    }
    // The first layer is on top; no-repeat leaves the rest of the box empty
    Raster top = raster("linear-gradient(#f00, #f00), linear-gradient(#00f, #00f)", 40, 10, "10px 10px, auto",
                        "right center, 0 0", "no-repeat");
    EXPECT_TRUE(near(top.at(35, 5), RGBA{255, 0, 0, 255}, 0));
    EXPECT_TRUE(near(top.at(5, 5), RGBA{0, 0, 255, 255}, 0));
}

TEST(GradientRaster, RoundedCornersAreClipped) {
    Raster r = raster("linear-gradient(#f00, #f00)", 40, 40, "", "", "", 10);
    EXPECT_EQ(r.at(0, 0)[3], 0);
    EXPECT_EQ(r.at(20, 20)[3], 255);
    EXPECT_EQ(r.at(20, 0)[3], 255);
}

TEST(GradientRaster, OnlyGradientLayersAreDrawn) {
    EXPECT_TRUE(ZepraBrowser::isGradientBackground("radial-gradient(red, blue)"));
    EXPECT_FALSE(ZepraBrowser::isGradientBackground("url(a.png), linear-gradient(red, blue)"));
    EXPECT_FALSE(ZepraBrowser::isGradientBackground("none"));
    int w = 0, h = 0;
    ZepraBrowser::gradientPixelSize(4000, 4000, w, h);
    EXPECT_LE((double)w * h, 2.6e6);
    EXPECT_EQ(w, h);
}

// ---------------------------------------------------------------------------
// Background colour alpha
// ---------------------------------------------------------------------------

struct FillCall {
    uint32_t color;
    int alpha;  // -1: an opaque rectangle (no alpha argument)
};
std::vector<FillCall> g_fills;

std::vector<FillCall> paintFilledBox(uint8_t bgAlpha, float opacity) {
    g_fills.clear();
    ZepraBrowser::setLayoutCallbacks(
        [](float, float, float, float, uint32_t c) { g_fills.push_back({c, -1}); },
        [](float, float, float, float, uint32_t, float) {}, nullptr, nullptr, nullptr, nullptr);
    ZepraBrowser::setLayoutCallbacks2(
        [](float, float, float, float, float, uint32_t c, uint8_t a) { g_fills.push_back({c, a}); }, nullptr);
    LayoutBox box;
    box.width = box.height = 10;
    box.hasBgColor = true;
    box.bgColor = 0xFF0000;
    box.bgAlpha = bgAlpha;
    box.opacity = opacity;
    ZepraBrowser::paintBox(box, 0, 0, 10000, 0);
    ZepraBrowser::setLayoutCallbacks(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    ZepraBrowser::setLayoutCallbacks2(nullptr, nullptr);
    return g_fills;
}

TEST(Backgrounds, ColorAlphaAndOpacityReachTheFill) {
    auto opaque = paintFilledBox(255, 1.0f);
    ASSERT_EQ(opaque.size(), 1u);
    EXPECT_EQ(opaque[0].alpha, -1);
    // rgba(255, 0, 0, 0.5)
    auto half = paintFilledBox(128, 1.0f);
    ASSERT_EQ(half.size(), 1u);
    EXPECT_EQ(half[0].color, 0xFF0000u);
    EXPECT_EQ(half[0].alpha, 128);
    // opacity multiplies the colour's own alpha
    auto both = paintFilledBox(128, 0.5f);
    ASSERT_EQ(both.size(), 1u);
    EXPECT_EQ(both[0].alpha, 64);
}

// ---------------------------------------------------------------------------
// Outlines
// ---------------------------------------------------------------------------

struct RectCall {
    float x, y, w, h;
    uint32_t color;
};
std::vector<RectCall> g_outlineRects;

TEST(Outlines, DrawnOutsideTheBorderBoxAtTheirOffset) {
    g_outlineRects.clear();
    ZepraBrowser::setLayoutCallbacks([](float x, float y, float w, float h, uint32_t c) { g_outlineRects.push_back({x, y, w, h, c}); },
                                     [](float, float, float, float, uint32_t, float) {}, nullptr, nullptr, nullptr,
                                     nullptr);
    LayoutBox box;
    box.x = 10;
    box.y = 20;
    box.width = 100;
    box.height = 50;
    box.outlineWidth = 2;
    box.outlineOffset = 3;
    box.outlineColor = 0xFF00FF;
    ZepraBrowser::paintBox(box, 0, 0, 10000, 0);
    ZepraBrowser::setLayoutCallbacks(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    ASSERT_EQ(g_outlineRects.size(), 4u);
    // Top side: 5px out from the box on every side, 2px thick
    EXPECT_FLOAT_EQ(g_outlineRects[0].x, 5);
    EXPECT_FLOAT_EQ(g_outlineRects[0].y, 15);
    EXPECT_FLOAT_EQ(g_outlineRects[0].w, 110);
    EXPECT_FLOAT_EQ(g_outlineRects[0].h, 2);
    EXPECT_EQ(g_outlineRects[0].color, 0xFF00FFu);
    // Right side
    EXPECT_FLOAT_EQ(g_outlineRects[3].x, 113);
}

// ---------------------------------------------------------------------------
// Percentage border radius
// ---------------------------------------------------------------------------

float g_paintedRadius = -1;

TEST(Backgrounds, PercentRadiusResolvesAgainstTheBox) {
    g_paintedRadius = -1;
    ZepraBrowser::setLayoutCallbacks([](float, float, float, float, uint32_t) {},
                                     [](float, float, float, float, uint32_t, float) {}, nullptr, nullptr, nullptr,
                                     nullptr);
    ZepraBrowser::setLayoutCallbacks2(
        [](float, float, float, float, float r, uint32_t, uint8_t) { g_paintedRadius = r; }, nullptr);
    LayoutBox box;
    box.width = 56;
    box.height = 40;
    box.hasBgColor = true;
    box.borderRadiusPercent = 50;
    ZepraBrowser::paintBox(box, 0, 0, 10000, 0);
    ZepraBrowser::setLayoutCallbacks(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    ZepraBrowser::setLayoutCallbacks2(nullptr, nullptr);
    // Corners are circular here: half the smaller side
    EXPECT_FLOAT_EQ(g_paintedRadius, 20);
}

// ---------------------------------------------------------------------------
// Serialization (inline <svg> is drawn from its outerHTML)
// ---------------------------------------------------------------------------

TEST(Serialization, OuterAndInnerHTML) {
    StyledDoc d("<!doctype html><div id=a>x &amp; y<br><span title='say \"hi\"'>z</span></div>");
    DOMElement* div = d.doc->getElementById("a");
    ASSERT_NE(div, nullptr);
    EXPECT_EQ(div->innerHTML(), "x &amp; y<br><span title=\"say &quot;hi&quot;\">z</span>");
    EXPECT_EQ(div->outerHTML(), "<div id=\"a\">" + div->innerHTML() + "</div>");
}

// ---------------------------------------------------------------------------
// Background images: the shorthand, and the tiles painted for url() images
// ---------------------------------------------------------------------------

TEST(Cascade, BackgroundShorthandSplitsItsParts) {
    StyledDoc d("<!doctype html><div id=a></div><div id=b></div><div id=c></div>",
                "#a { background: #ddd url('img/x.svg') no-repeat center / contain }"
                "#b { background: linear-gradient(to right, red, blue) }"
                "#c { background: url(a.png); background-color: #f00 }");
    const CSSComputedStyle* a = d.style("a");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->backgroundImage, "url('img/x.svg')");
    EXPECT_EQ(a->backgroundColor.r, 0xDD);
    EXPECT_EQ(a->backgroundColor.a, 255);
    EXPECT_EQ(a->backgroundRepeat, "no-repeat");
    EXPECT_EQ(a->backgroundPosition, "center");
    EXPECT_EQ(a->backgroundSize, "contain");
    EXPECT_EQ(d.style("b")->backgroundImage, "linear-gradient(to right, red, blue)");
    EXPECT_TRUE(d.style("b")->backgroundColor.isTransparent());
    EXPECT_EQ(d.style("c")->backgroundColor.r, 0xFF);
    EXPECT_EQ(ZepraBrowser::cssUrl(a->backgroundImage), "img/x.svg");
}

struct Tile {
    float x, y, w, h;
};
std::vector<Tile> g_tiles;
std::vector<Tile> g_clips;

std::vector<Tile> paintBackgroundTiles(float w, float h, float nw, float nh, const std::string& repeat,
                                       const std::string& size, const std::string& position) {
    g_tiles.clear();
    g_clips.clear();
    ZepraBrowser::setLayoutCallbacks(
        [](float, float, float, float, uint32_t) {}, [](float, float, float, float, uint32_t, float) {},
        nullptr, nullptr, nullptr,
        [](float x, float y, float w, float h, uint32_t) { g_tiles.push_back({x, y, w, h}); });
    ZepraBrowser::setLayoutClipCallbacks([](float x, float y, float w, float h) { g_clips.push_back({x, y, w, h}); },
                                         [] {});
    LayoutBox box;
    box.width = w;
    box.height = h;
    box.bgTextureId = 7;
    box.bgNaturalWidth = nw;
    box.bgNaturalHeight = nh;
    box.bgRepeat = repeat;
    box.bgSize = size;
    box.bgPosition = position;
    ZepraBrowser::paintBox(box, 0, 0, 10000, 0);
    ZepraBrowser::setLayoutCallbacks(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    ZepraBrowser::setLayoutClipCallbacks(nullptr, nullptr);
    return g_tiles;
}

TEST(BackgroundImage, RepeatTilesTheBox) {
    auto tiles = paintBackgroundTiles(100, 50, 40, 20, "repeat", "", "");
    ASSERT_EQ(tiles.size(), 9u);  // 3 across (0, 40, 80) x 3 down (0, 20, 40)
    EXPECT_FLOAT_EQ(tiles[1].x, 40);
    EXPECT_FLOAT_EQ(tiles[3].y, 20);
    ASSERT_EQ(g_clips.size(), 1u);  // clipped to the box
    EXPECT_FLOAT_EQ(g_clips[0].w, 100);
}

TEST(BackgroundImage, RepeatXWithAPercentPosition) {
    auto tiles = paintBackgroundTiles(100, 50, 40, 20, "repeat-x", "", "0 50%");
    ASSERT_EQ(tiles.size(), 3u);
    EXPECT_FLOAT_EQ(tiles[0].y, 15);  // (50 - 20) * 50%
}

TEST(BackgroundImage, NoRepeatCentredAndContained) {
    auto tiles = paintBackgroundTiles(200, 80, 40, 20, "no-repeat", "contain", "center");
    ASSERT_EQ(tiles.size(), 1u);
    EXPECT_FLOAT_EQ(tiles[0].w, 160);
    EXPECT_FLOAT_EQ(tiles[0].h, 80);
    EXPECT_FLOAT_EQ(tiles[0].x, 20);
}

TEST(BackgroundImage, CoverAndLengthSizes) {
    auto cover = paintBackgroundTiles(100, 100, 40, 20, "no-repeat", "cover", "center");
    ASSERT_EQ(cover.size(), 1u);
    EXPECT_FLOAT_EQ(cover[0].w, 200);
    EXPECT_FLOAT_EQ(cover[0].x, -50);
    auto sized = paintBackgroundTiles(100, 100, 40, 20, "no-repeat", "32px", "right bottom");
    ASSERT_EQ(sized.size(), 1u);
    EXPECT_FLOAT_EQ(sized[0].h, 16);
    EXPECT_FLOAT_EQ(sized[0].x, 68);
    EXPECT_FLOAT_EQ(sized[0].y, 84);
}

// ---------------------------------------------------------------------------
// Borders paint inside the border box
// ---------------------------------------------------------------------------

struct Stroke {
    float x, y, w, h, thickness;
};
std::vector<Tile> g_rects;
std::vector<Stroke> g_strokes;

/// Paints one box at (10, 20) and returns the rectangles drawn (the
/// background first, when it has one)
void paintBorderBox(LayoutBox& box) {
    g_rects.clear();
    g_strokes.clear();
    ZepraBrowser::setLayoutCallbacks(
        [](float x, float y, float w, float h, uint32_t) { g_rects.push_back({x, y, w, h}); },
        [](float x, float y, float w, float h, uint32_t, float t) { g_strokes.push_back({x, y, w, h, t}); },
        nullptr, nullptr, nullptr, nullptr);
    ZepraBrowser::setLayoutCallbacks2(
        [](float x, float y, float w, float h, float, uint32_t, uint8_t) { g_rects.push_back({x, y, w, h}); },
        nullptr);
    ZepraBrowser::paintBox(box, 10, 20, 10000, 0);
    ZepraBrowser::setLayoutCallbacks(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    ZepraBrowser::setLayoutCallbacks2(nullptr, nullptr);
}

/// Whether every rectangle stays inside [x, x+w] x [y, y+h]
bool allInside(const std::vector<Tile>& rects, float x, float y, float w, float h) {
    for (const auto& r : rects) {
        if (r.x < x || r.y < y || r.x + r.w > x + w || r.y + r.h > y + h) return false;
    }
    return true;
}

TEST(Borders, UniformBorderStaysInsideTheBox) {
    LayoutBox box;
    box.width = 100;
    box.height = 50;
    box.borderTop = box.borderRight = box.borderBottom = box.borderLeft = 4;
    paintBorderBox(box);
    EXPECT_TRUE(g_strokes.empty());  // no centered stroke spilling 2px outside
    ASSERT_EQ(g_rects.size(), 4u);
    EXPECT_TRUE(allInside(g_rects, 10, 20, 100, 50));
    // Top and bottom span the width; the sides fill the height between them
    EXPECT_FLOAT_EQ(g_rects[0].x, 10);
    EXPECT_FLOAT_EQ(g_rects[0].y, 20);
    EXPECT_FLOAT_EQ(g_rects[0].w, 100);
    EXPECT_FLOAT_EQ(g_rects[0].h, 4);
    EXPECT_FLOAT_EQ(g_rects[1].y, 66);
    EXPECT_FLOAT_EQ(g_rects[2].x, 10);
    EXPECT_FLOAT_EQ(g_rects[2].y, 24);
    EXPECT_FLOAT_EQ(g_rects[2].w, 4);
    EXPECT_FLOAT_EQ(g_rects[2].h, 42);
    EXPECT_FLOAT_EQ(g_rects[3].x, 106);
    // The four pieces cover the border area exactly once: 2 * 100 * 4 + 2 * 42 * 4
    float area = 0;
    for (const auto& r : g_rects) area += r.w * r.h;
    EXPECT_FLOAT_EQ(area, 1136);
}

TEST(Borders, MixedWidthsAndOneSide) {
    LayoutBox box;
    box.width = 80;
    box.height = 30;
    box.borderTop = 1;
    box.borderLeft = 6;
    paintBorderBox(box);
    ASSERT_EQ(g_rects.size(), 2u);
    EXPECT_TRUE(allInside(g_rects, 10, 20, 80, 30));
    EXPECT_FLOAT_EQ(g_rects[1].w, 6);
    EXPECT_FLOAT_EQ(g_rects[1].h, 29);
}

TEST(Borders, BorderWiderThanTheBoxIsClamped) {
    LayoutBox box;
    box.width = 6;
    box.height = 6;
    box.borderTop = box.borderRight = box.borderBottom = box.borderLeft = 5;
    paintBorderBox(box);
    EXPECT_TRUE(allInside(g_rects, 10, 20, 6, 6));
}

TEST(Borders, RoundedBorderStrokeIsInsetByHalfItsWidth) {
    LayoutBox box;
    box.width = 100;
    box.height = 50;
    box.borderRadius = 8;
    box.borderTop = box.borderRight = box.borderBottom = box.borderLeft = 4;
    paintBorderBox(box);
    ASSERT_EQ(g_strokes.size(), 1u);
    const Stroke& s = g_strokes[0];
    EXPECT_FLOAT_EQ(s.thickness, 4);
    // The stroke's outer edge is the border box edge
    EXPECT_FLOAT_EQ(s.x - s.thickness / 2, 10);
    EXPECT_FLOAT_EQ(s.y - s.thickness / 2, 20);
    EXPECT_FLOAT_EQ(s.x + s.w + s.thickness / 2, 110);
    EXPECT_FLOAT_EQ(s.y + s.h + s.thickness / 2, 70);
}

TEST(Borders, TransparentBorderPaintsNothing) {
    LayoutBox box;
    box.width = 40;
    box.height = 40;
    box.borderTop = box.borderRight = box.borderBottom = box.borderLeft = 3;
    box.borderVisible = false;
    paintBorderBox(box);
    EXPECT_TRUE(g_rects.empty());
    EXPECT_TRUE(g_strokes.empty());
}

// ---------------------------------------------------------------------------
// Overflow: clipping, scrolled painting, floats, the overflow shorthand
// ---------------------------------------------------------------------------

std::vector<Tile> g_pushedClips;
int g_poppedClips = 0;
std::vector<Tile> g_links;

/// The first rectangle of that size (h < 0: any height)
const Tile* findRect(const std::vector<Tile>& rects, float w, float h) {
    for (const Tile& r : rects) {
        if (r.w == w && (h < 0 || r.h == h)) return &r;
    }
    return nullptr;
}

struct ClippedPaint {
    std::vector<Tile> rects, clips, links;
};

ClippedPaint paintClipped(LayoutBox& box) {
    g_rects.clear();
    g_pushedClips.clear();
    g_poppedClips = 0;
    g_links.clear();
    ZepraBrowser::setLayoutCallbacks(
        [](float x, float y, float w, float h, uint32_t) { g_rects.push_back({x, y, w, h}); },
        [](float, float, float, float, uint32_t, float) {}, nullptr, nullptr,
        [](float x, float y, float w, float h, const std::string&, const std::string&) { g_links.push_back({x, y, w, h}); },
        nullptr);
    ZepraBrowser::setLayoutClipCallbacks([](float x, float y, float w, float h) { g_pushedClips.push_back({x, y, w, h}); },
                                         [] { g_poppedClips++; });
    ZepraBrowser::paintBox(box, 0, 0, 10000, 0);
    ZepraBrowser::setLayoutCallbacks(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    ZepraBrowser::setLayoutClipCallbacks(nullptr, nullptr);
    return {g_rects, g_pushedClips, g_links};
}

/// A 100 x 50 scroller (2px border) at (10, 10) holding a 100px-tall
/// coloured child and a link scrolled above it
struct Scroller {
    std::unique_ptr<DOMDocument> doc;
    LayoutBox root;
    LayoutBox* box = nullptr;
    LayoutBox* child = nullptr;
    LayoutBox* link = nullptr;

    explicit Scroller(int overflow) {
        ZepraBrowser::elementScrollOffsets().clear();
        HTMLParser parser;
        doc = parser.parse("<!doctype html><div id=s><p id=c>c</p><a id=l href=x>l</a></div>");
        root.children.emplace_back();
        box = &root.children.back();
        box->domElement = doc->getElementById("s");
        box->x = 10; box->y = 10; box->width = 100; box->height = 50;
        box->borderTop = box->borderRight = box->borderBottom = box->borderLeft = 2;
        box->overflowX = box->overflowY = overflow;
        box->children.emplace_back();
        child = &box->children.back();
        child->domElement = doc->getElementById("c");
        child->x = 2; child->y = 2; child->width = 96; child->height = 100;
        child->hasBgColor = true;
        child->bgColor = 0x112233;
        box->children.emplace_back();
        link = &box->children.back();
        link->domElement = doc->getElementById("l");
        link->type = LayoutType::Inline;
        link->x = 2; link->y = 2; link->width = 30; link->height = 16;
        link->text = "l";
        link->isLink = true;
        link->href = "x";
    }
    ~Scroller() { ZepraBrowser::elementScrollOffsets().clear(); }
};

TEST(Overflow, ScrollerClipsToItsPaddingBoxAndPaintsScrolled) {
    Scroller s(ZepraBrowser::kOverflowAuto);
    ZepraBrowser::scrollContainerTo(*s.box, 0, 30);
    ClippedPaint p = paintClipped(s.root);
    ASSERT_EQ(p.clips.size(), 1u);
    EXPECT_FLOAT_EQ(p.clips[0].x, 12);
    EXPECT_FLOAT_EQ(p.clips[0].y, 12);
    EXPECT_FLOAT_EQ(p.clips[0].w, 96);
    EXPECT_FLOAT_EQ(p.clips[0].h, 46);
    EXPECT_EQ(g_poppedClips, 1);
    // The child's background moved up by the scroll position
    const Tile* bg = findRect(p.rects, 96, 100);
    ASSERT_NE(bg, nullptr);
    EXPECT_FLOAT_EQ(bg->y, 10 + 2 - 30);
    EXPECT_FLOAT_EQ(s.child->screenY, 10 + 2 - 30);
    // A scroll thumb on the right, inside the padding box
    const Tile* thumb = findRect(p.rects, 6, -1);
    ASSERT_NE(thumb, nullptr);
    EXPECT_GE(thumb->x, 12);
    EXPECT_LE(thumb->x + thumb->w, 108);
    EXPECT_GT(thumb->y, 12);  // scrolled 30 of 56: the thumb is off the top
    // The link (scrolled to 10 + 2 - 30 = -18 .. -2, above the scrollport) is not clickable
    EXPECT_TRUE(p.links.empty());
}

TEST(Overflow, PartlyVisibleLinkIsClickableOnlyInsideTheScrollport) {
    Scroller s(ZepraBrowser::kOverflowAuto);
    ZepraBrowser::scrollContainerTo(*s.box, 0, 10);
    ClippedPaint p = paintClipped(s.root);
    ASSERT_EQ(p.links.size(), 1u);
    // At 10 + 2 - 10 = 2 .. 18 on screen: the part above the padding edge
    // (12) is cut off
    EXPECT_FLOAT_EQ(p.links[0].y, 12);
    EXPECT_FLOAT_EQ(p.links[0].h, 6);
}

TEST(Overflow, HiddenClipsWithoutAThumbAndVisibleDoesNotClip) {
    Scroller hidden(ZepraBrowser::kOverflowHidden);
    ClippedPaint p = paintClipped(hidden.root);
    EXPECT_EQ(p.clips.size(), 1u);
    EXPECT_EQ(findRect(p.rects, 6, -1), nullptr);  // no thumb
    EXPECT_EQ(p.links.size(), 1u);

    Scroller visible(ZepraBrowser::kOverflowVisible);
    p = paintClipped(visible.root);
    EXPECT_TRUE(p.clips.empty());
    EXPECT_EQ(findRect(p.rects, 6, -1), nullptr);
}

TEST(Overflow, ClippingBoxContainsItsFloats) {
    LayoutBox outer;
    outer.domElement = reinterpret_cast<Zepra::WebCore::DOMElement*>(&outer);  // any element
    LayoutBox& f = addBox(outer, LayoutType::Block);
    f.floatType = 1;
    f.cssWidth = ZepraBrowser::LayoutLength::px(50);
    f.cssHeight = ZepraBrowser::LayoutLength::px(80);
    LayoutBox plain = outer;
    ZepraBrowser::layoutBlock(plain, 400, 0);
    EXPECT_FLOAT_EQ(plain.height, 0);  // floats hang out of an ordinary block
    outer.overflowY = ZepraBrowser::kOverflowHidden;
    ZepraBrowser::layoutBlock(outer, 400, 0);
    EXPECT_FLOAT_EQ(outer.height, 80);
}

TEST(Cascade, OverflowShorthandTakesOneOrTwoValues) {
    StyledDoc d("<!doctype html><div id=a></div><div id=b></div><div id=c></div><div id=d></div>",
                "#a { overflow: auto } #b { overflow: hidden scroll } #c { overflow-y: clip } #d { overflow: overlay }");
    EXPECT_EQ(d.style("a")->overflowX, OverflowValue::Auto);
    EXPECT_EQ(d.style("a")->overflowY, OverflowValue::Auto);
    EXPECT_EQ(d.style("b")->overflowX, OverflowValue::Hidden);
    EXPECT_EQ(d.style("b")->overflowY, OverflowValue::Scroll);
    EXPECT_EQ(d.style("c")->overflowX, OverflowValue::Visible);
    EXPECT_EQ(d.style("c")->overflowY, OverflowValue::Clip);
    EXPECT_EQ(d.style("d")->overflowY, OverflowValue::Auto);
}

// ---------------------------------------------------------------------------
// position: fixed and sticky, and z-index paint order
// ---------------------------------------------------------------------------

namespace {

struct PaintedRect {
    float x, y, w, h;
    uint32_t color;
};
std::vector<PaintedRect> g_rects;

// Paints `root` at (0, 0) in a 1280x800 viewport and returns its background
// rectangles in the order they were drawn
std::vector<PaintedRect> paintRects(const LayoutBox& root, float scrollY = 0) {
    g_rects.clear();
    ZepraBrowser::setLayoutCallbacks(
        [](float x, float y, float w, float h, uint32_t c) { g_rects.push_back({x, y, w, h, c}); },
        [](float, float, float, float, uint32_t, float) {}, nullptr, nullptr, nullptr, nullptr);
    ZepraBrowser::paintBox(root, 0, 0, 800, scrollY);
    ZepraBrowser::setLayoutCallbacks(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    return g_rects;
}

std::vector<uint32_t> colors(const std::vector<PaintedRect>& rects) {
    std::vector<uint32_t> out;
    for (const auto& r : rects) out.push_back(r.color);
    return out;
}

const PaintedRect* rectOf(const std::vector<PaintedRect>& rects, uint32_t color) {
    for (const auto& r : rects)
        if (r.color == color) return &r;
    return nullptr;
}

LayoutBox& coloured(LayoutBox& parent, uint32_t color, float height, int position = 0) {
    LayoutBox& b = addBox(parent, LayoutType::Block);
    b.hasBgColor = true;
    b.bgColor = color;
    b.cssHeight = LayoutLength{height, LayoutLength::Unit::Px};
    b.height = height;
    b.positionType = position;
    return b;
}

LayoutLength px(float v) { return LayoutLength{v, LayoutLength::Unit::Px}; }

} // namespace

TEST(FixedPosition, LaidOutAgainstTheViewport) {
    LayoutBox root;
    root.type = LayoutType::Block;
    root.paddingLeft = root.paddingTop = 30;
    LayoutBox& wrapper = addBox(root, LayoutType::Block);
    wrapper.marginTop = 200;
    LayoutBox& header = coloured(wrapper, 0x1111, 50, 3);  // top: 0; left: 0; right: 0
    header.cssTop = px(0);
    header.cssLeft = px(0);
    header.cssRight = px(0);
    LayoutBox& fab = addBox(wrapper, LayoutType::Block);  // right: 20px; bottom: 10px
    fab.positionType = 3;
    fab.cssRight = px(20);
    fab.cssBottom = px(10);
    addBox(fab, LayoutType::Inline).text = "abcdefghij";  // 80px wide
    LayoutBox& overlay = addBox(wrapper, LayoutType::Block);  // inset: 0
    overlay.positionType = 3;
    overlay.cssTop = overlay.cssRight = overlay.cssBottom = overlay.cssLeft = px(0);
    coloured(wrapper, 0x2222, 40);  // in flow
    ZepraBrowser::layoutBlock(root, 1280, 0);

    EXPECT_FLOAT_EQ(header.x, 0);
    EXPECT_FLOAT_EQ(header.y, 0);
    EXPECT_FLOAT_EQ(header.width, 1280);
    EXPECT_FLOAT_EQ(fab.width, 80);  // shrinks to its content
    EXPECT_FLOAT_EQ(fab.x, 1280 - 20 - 80);
    EXPECT_FLOAT_EQ(fab.y, 800 - 10 - fab.height);
    EXPECT_FLOAT_EQ(overlay.width, 1280);
    EXPECT_FLOAT_EQ(overlay.height, 800);
    // Out of flow: the in-flow box is the wrapper's first child as far as layout goes
    EXPECT_FLOAT_EQ(child(wrapper, 3)->y, 0);
    EXPECT_FLOAT_EQ(wrapper.height, 40);
}

TEST(FixedPosition, PaintedLastAtTheViewportWhateverTheScroll) {
    LayoutBox root;
    root.type = LayoutType::Block;
    LayoutBox& header = coloured(root, 0xAAAA, 50, 3);
    header.cssTop = px(0);
    header.cssLeft = px(0);
    header.cssRight = px(0);
    coloured(root, 0xBBBB, 2000);
    ZepraBrowser::layoutBlock(root, 1280, 0);

    for (float scroll : {0.0f, 700.0f}) {
        auto rects = paintRects(root, scroll);
        ASSERT_EQ(colors(rects), (std::vector<uint32_t>{0xBBBB, 0xAAAA})) << scroll;
        EXPECT_FLOAT_EQ(rectOf(rects, 0xAAAA)->y, 0) << scroll;
        EXPECT_FLOAT_EQ(rectOf(rects, 0xBBBB)->y, -scroll) << scroll;
    }
    auto fixed = ZepraBrowser::fixedBoxesInPaintOrder(root);
    ASSERT_EQ(fixed.size(), 1u);
    EXPECT_EQ(fixed[0], &header);
}

TEST(ZIndex, PositionedBoxesPaintByZIndex) {
    LayoutBox root;
    root.type = LayoutType::Block;
    LayoutBox& a = coloured(root, 0xA, 10, 1);
    a.zIndex = 2;
    a.zIndexAuto = false;
    LayoutBox& b = coloured(root, 0xB, 10, 1);
    b.zIndex = 1;
    b.zIndexAuto = false;
    coloured(root, 0xC, 10, 1);  // z-index: auto
    coloured(root, 0xD, 10);     // in flow
    ZepraBrowser::layoutBlock(root, 1280, 0);
    EXPECT_EQ(colors(paintRects(root)), (std::vector<uint32_t>{0xD, 0xC, 0xB, 0xA}));
}

TEST(ZIndex, NegativeZIndexGoesUnderInFlowContentThroughAutoAncestors) {
    // <div style="position: relative">       (z-index: auto: no stacking context)
    //   <div style="position: absolute; z-index: -1">grey</div>
    // </div>
    // <div>blue</div>                          grey paints under blue
    LayoutBox root;
    root.type = LayoutType::Block;
    LayoutBox& stack = coloured(root, 0x5, 10, 1);
    LayoutBox& grey = coloured(stack, 0x6, 10, 2);
    grey.zIndex = -1;
    grey.zIndexAuto = false;
    coloured(root, 0x7, 10);
    ZepraBrowser::layoutBlock(root, 1280, 0);
    EXPECT_EQ(colors(paintRects(root)), (std::vector<uint32_t>{0x6, 0x7, 0x5}));

    // A z-index on the wrapper makes it a stacking context: grey stays inside it
    stack.zIndex = 0;
    stack.zIndexAuto = false;
    EXPECT_EQ(colors(paintRects(root)), (std::vector<uint32_t>{0x7, 0x5, 0x6}));
}

TEST(ZIndex, FlexItemsUseZIndexWithoutPositioning) {
    LayoutBox flex = makeFlex(0, 0);
    LayoutBox& first = coloured(flex, 0x1, 10);
    first.zIndex = 1;
    first.zIndexAuto = false;
    coloured(flex, 0x2, 10);
    ZepraBrowser::layoutBlock(flex, 1280, 0);
    EXPECT_EQ(colors(paintRects(flex)), (std::vector<uint32_t>{0x2, 0x1}));
}

TEST(StickyPosition, SticksUntilItsParentEnds) {
    LayoutBox root;
    root.type = LayoutType::Block;
    coloured(root, 0x1, 300);  // spacer
    LayoutBox& section = coloured(root, 0x2, 500);
    LayoutBox& title = coloured(section, 0x3, 40, 4);
    title.cssTop = px(10);
    coloured(root, 0x4, 2000);  // more page
    ZepraBrowser::layoutBlock(root, 1280, 0);

    auto titleY = [&](float scroll) { return rectOf(paintRects(root, scroll), 0x3)->y; };
    EXPECT_FLOAT_EQ(titleY(0), 300);      // in flow
    EXPECT_FLOAT_EQ(titleY(200), 100);    // still in flow
    EXPECT_FLOAT_EQ(titleY(400), 10);     // stuck 10px from the top
    EXPECT_FLOAT_EQ(titleY(700), 10);
    EXPECT_FLOAT_EQ(titleY(780), -20);    // pushed out with the section's end (800 - 780 - 40)
}
