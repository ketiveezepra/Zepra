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

#include "browser/dom.hpp"
#include "browser/layout_engine.h"
#include "css/css_engine.hpp"
#include "css/presentational_hints.hpp"
#include "css/ua_stylesheet.h"
#include "html/html_parser.hpp"

#include <memory>
#include <string>

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
