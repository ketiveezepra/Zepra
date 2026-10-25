// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * layout_table.cpp - CSS 2.1 §17 table layout for the LayoutBox tree
 *
 * A table box holds captions, row groups and rows; rows hold cells (the box
 * builder adds anonymous boxes where the markup leaves gaps). Layout runs in
 * the usual order:
 *   1. place cells in a grid (colspan/rowspan, header groups first, footers last)
 *   2. min-/max-content width of every cell (shrink-to-fit pass)
 *   3. column widths: automatic algorithm (§17.5.2.2, distributing between the
 *      min, specified and max widths of the columns) or fixed (§17.5.2.1)
 *   4. lay each cell out at its column width, then row heights and rowspans
 *   5. place rows and cells, then vertical-align inside each cell
 * Structure follows the NXRender TableLayout (nxrender-cpp/layout), adapted
 * to LayoutBox and to the spec's auto-width and span rules.
 */

#include "layout_engine.h"

#include <algorithm>
#include <cmath>
#include <vector>

extern int g_width;
extern int g_height;

namespace ZepraBrowser {

namespace {

// layoutBlock leaves this gap between neighbouring inline boxes on a line
constexpr float kInlineGap = 4;

int s_intrinsicEpoch = 0;

float horizontalEdges(const LayoutBox& b) {
    return b.paddingLeft + b.paddingRight + b.borderLeft + b.borderRight;
}

float verticalEdges(const LayoutBox& b) {
    return b.paddingTop + b.paddingBottom + b.borderTop + b.borderBottom;
}

bool hasLength(const LayoutLength& len) {
    return len.isSet() && !len.isAuto();
}

// Border-box width from a non-percentage CSS width, or -1
float fixedBorderBoxWidth(const LayoutBox& b, const LayoutLength& len) {
    if (!hasLength(len) || len.isPercent()) return -1;
    float w = len.resolve(0, b.fontSize, (float)g_width, (float)g_height);
    if (b.boxSizing == 0) w += horizontalEdges(b);
    return std::max(0.0f, w);
}

float fixedBorderBoxHeight(const LayoutBox& b, const LayoutLength& len) {
    if (!hasLength(len) || len.isPercent()) return -1;
    float h = len.resolve(0, b.fontSize, (float)g_width, (float)g_height);
    if (b.boxSizing == 0) h += verticalEdges(b);
    return std::max(0.0f, h);
}

struct GridCell {
    LayoutBox* box = nullptr;
    int row = 0, col = 0, rowSpan = 1, colSpan = 1;
    float minW = 0, maxW = 0;
    float specW = -1;   // fixed border-box width from CSS / the width attribute
    float pct = -1;     // percentage width
    float naturalH = 0; // border-box height after layout at the column width
};

struct TableGrid {
    std::vector<LayoutBox*> rows;      // visual order
    std::vector<LayoutBox*> rowGroup;  // owning row group, or nullptr for a direct row
    std::vector<int> rowGroupEnd;      // one past the last row a rowspan may reach
    std::vector<LayoutBox*> groups;    // row groups in visual order
    std::vector<LayoutBox*> captions;
    std::vector<GridCell> cells;
    int cols = 0;
};

void buildGrid(LayoutBox& table, TableGrid& g) {
    // thead groups are drawn first and tfoot groups last (§17.2)
    std::vector<LayoutBox*> head, body, foot;
    for (auto& child : table.children) {
        switch (child.type) {
            case LayoutType::TableCaption: g.captions.push_back(&child); break;
            case LayoutType::TableRowGroup:
                (child.rowGroupKind == 1 ? head : child.rowGroupKind == 2 ? foot : body).push_back(&child);
                break;
            case LayoutType::TableRow: body.push_back(&child); break;
            default: break;
        }
    }
    std::vector<LayoutBox*> items;
    items.insert(items.end(), head.begin(), head.end());
    items.insert(items.end(), body.begin(), body.end());
    items.insert(items.end(), foot.begin(), foot.end());

    for (size_t i = 0; i < items.size(); i++) {
        LayoutBox* item = items[i];
        size_t first = g.rows.size();
        if (item->type == LayoutType::TableRowGroup) {
            g.groups.push_back(item);
            for (auto& row : item->children) {
                if (row.type != LayoutType::TableRow) continue;
                g.rows.push_back(&row);
                g.rowGroup.push_back(item);
            }
        } else {
            // A run of rows directly in the table acts as one row group
            while (i < items.size() && items[i]->type == LayoutType::TableRow) {
                g.rows.push_back(items[i]);
                g.rowGroup.push_back(nullptr);
                i++;
            }
            i--;
        }
        g.rowGroupEnd.resize(g.rows.size(), (int)g.rows.size());
        for (size_t r = first; r < g.rows.size(); r++) g.rowGroupEnd[r] = (int)g.rows.size();
    }

    std::vector<std::vector<char>> occupied(g.rows.size());
    auto isOccupied = [&](int r, int c) {
        return c < (int)occupied[r].size() && occupied[r][c];
    };
    for (int r = 0; r < (int)g.rows.size(); r++) {
        int c = 0;
        for (auto& cellBox : g.rows[r]->children) {
            if (cellBox.type != LayoutType::TableCell) continue;
            while (isOccupied(r, c)) c++;
            GridCell cell;
            cell.box = &cellBox;
            cell.row = r;
            cell.col = c;
            cell.colSpan = std::clamp(cellBox.colSpan, 1, 1000);
            int end = g.rowGroupEnd[r];
            cell.rowSpan = cellBox.rowSpan <= 0 ? end - r : std::min(cellBox.rowSpan, end - r);
            cell.rowSpan = std::max(1, cell.rowSpan);
            for (int rr = r; rr < r + cell.rowSpan; rr++) {
                if ((int)occupied[rr].size() < c + cell.colSpan) occupied[rr].resize(c + cell.colSpan, 0);
                for (int cc = c; cc < c + cell.colSpan; cc++) occupied[rr][cc] = 1;
            }
            g.cols = std::max(g.cols, c + cell.colSpan);
            g.cells.push_back(cell);
            c += cell.colSpan;
        }
    }
}

struct Columns {
    std::vector<float> minW, maxW, fixedW, pct, width;
};

// Spread `extra` over columns [first, first+span) in proportion to weights
// (their max widths), or evenly when they are all zero
void spread(std::vector<float>& target, const std::vector<float>& weights,
            int first, int span, float extra) {
    float total = 0;
    for (int c = first; c < first + span; c++) total += weights[c];
    for (int c = first; c < first + span; c++) {
        target[c] += total > 0 ? extra * weights[c] / total : extra / span;
    }
}

void measureCells(TableGrid& g, Columns& cols, float spacingH) {
    for (auto& cell : g.cells) {
        intrinsicWidths(*cell.box, cell.minW, cell.maxW);
        if (cell.box->noWrap) cell.minW = cell.maxW;
        cell.specW = fixedBorderBoxWidth(*cell.box, cell.box->cssWidth);
        if (hasLength(cell.box->cssWidth) && cell.box->cssWidth.isPercent())
            cell.pct = cell.box->cssWidth.value;
    }

    int n = g.cols;
    cols.minW.assign(n, 0);
    cols.maxW.assign(n, 0);
    cols.fixedW.assign(n, -1);
    cols.pct.assign(n, -1);
    for (const auto& cell : g.cells) {
        if (cell.colSpan != 1) continue;
        int c = cell.col;
        cols.minW[c] = std::max(cols.minW[c], cell.minW);
        cols.maxW[c] = std::max(cols.maxW[c], cell.maxW);
        if (cell.specW >= 0) cols.fixedW[c] = std::max(cols.fixedW[c], cell.specW);
        if (cell.pct >= 0) cols.pct[c] = std::max(cols.pct[c], cell.pct);
    }
    for (int c = 0; c < n; c++) {
        // A column with a specified width prefers exactly that width
        if (cols.fixedW[c] >= 0) cols.maxW[c] = std::max(cols.minW[c], cols.fixedW[c]);
    }

    // Spanning cells widen their columns when the columns are too narrow,
    // narrowest spans first
    std::vector<const GridCell*> spanning;
    for (const auto& cell : g.cells) {
        if (cell.colSpan > 1) spanning.push_back(&cell);
    }
    std::stable_sort(spanning.begin(), spanning.end(),
                     [](const GridCell* a, const GridCell* b) { return a->colSpan < b->colSpan; });
    for (const GridCell* cell : spanning) {
        float gaps = spacingH * (cell->colSpan - 1);
        float spanMin = gaps, spanMax = gaps;
        for (int c = cell->col; c < cell->col + cell->colSpan; c++) {
            spanMin += cols.minW[c];
            spanMax += cols.maxW[c];
        }
        if (cell->minW > spanMin) spread(cols.minW, cols.maxW, cell->col, cell->colSpan, cell->minW - spanMin);
        float cellMax = std::max(cell->maxW, cell->specW);
        if (cellMax > spanMax) {
            std::vector<float> weights = cols.maxW;
            spread(cols.maxW, weights, cell->col, cell->colSpan, cellMax - spanMax);
        }
    }
    for (int c = 0; c < n; c++) cols.maxW[c] = std::max(cols.maxW[c], cols.minW[c]);
}

// §17.5.2.2-style automatic layout: columns move from their min widths to
// their specified (fixed / percentage) widths, then to their max widths;
// space beyond that goes to auto columns in proportion to their max widths
void autoColumnWidths(Columns& cols, float space) {
    int n = (int)cols.minW.size();
    std::vector<float> g0(n), g1(n), g2(n);
    float s0 = 0, s1 = 0, s2 = 0;
    for (int c = 0; c < n; c++) {
        float target = -1;
        if (cols.pct[c] >= 0) target = std::max(cols.minW[c], cols.pct[c] / 100.0f * space);
        else if (cols.fixedW[c] >= 0) target = std::max(cols.minW[c], cols.fixedW[c]);
        g0[c] = cols.minW[c];
        g1[c] = target >= 0 ? target : cols.minW[c];
        g2[c] = target >= 0 ? target : cols.maxW[c];
        s0 += g0[c]; s1 += g1[c]; s2 += g2[c];
    }
    cols.width.assign(n, 0);
    auto blend = [&](const std::vector<float>& a, const std::vector<float>& b, float sa, float sb) {
        float t = sb > sa ? (space - sa) / (sb - sa) : 1.0f;
        for (int c = 0; c < n; c++) cols.width[c] = a[c] + (b[c] - a[c]) * t;
    };
    if (space <= s0) { cols.width = g0; return; }
    if (space <= s1) { blend(g0, g1, s0, s1); return; }
    if (space <= s2) { blend(g1, g2, s1, s2); return; }

    cols.width = g2;
    float extra = space - s2;
    std::vector<float> weights(n, 0);
    int autoCount = 0;
    for (int c = 0; c < n; c++) {
        if (cols.pct[c] < 0 && cols.fixedW[c] < 0) { weights[c] = cols.maxW[c]; autoCount++; }
    }
    if (autoCount > 0) {
        float total = 0;
        for (int c = 0; c < n; c++) total += weights[c];
        for (int c = 0; c < n; c++) {
            bool isAuto = cols.pct[c] < 0 && cols.fixedW[c] < 0;
            if (!isAuto) continue;
            cols.width[c] += total > 0 ? extra * weights[c] / total : extra / autoCount;
        }
        return;
    }
    // Every column has a specified width: grow them in proportion
    spread(cols.width, g2, 0, n, extra);
}

// §17.5.2.1 fixed layout: widths come from the first row only
void fixedColumnWidths(const TableGrid& g, Columns& cols, float space, float spacingH) {
    int n = g.cols;
    cols.width.assign(n, -1);
    for (const auto& cell : g.cells) {
        if (cell.row != 0) continue;
        float w = cell.specW >= 0 ? cell.specW : (cell.pct >= 0 ? cell.pct / 100.0f * space : -1);
        if (w < 0) continue;
        float perCol = std::max(0.0f, (w - spacingH * (cell.colSpan - 1)) / cell.colSpan);
        for (int c = cell.col; c < cell.col + cell.colSpan; c++) cols.width[c] = perCol;
    }
    float assigned = 0;
    int autoCount = 0;
    for (int c = 0; c < n; c++) {
        if (cols.width[c] >= 0) assigned += cols.width[c];
        else autoCount++;
    }
    float remaining = std::max(0.0f, space - assigned);
    if (autoCount > 0) {
        for (int c = 0; c < n; c++) {
            if (cols.width[c] < 0) cols.width[c] = remaining / autoCount;
        }
    } else if (remaining > 0 && n > 0) {
        std::vector<float> weights = cols.width;
        spread(cols.width, weights, 0, n, remaining);
    }
}

// Lays a cell (or caption) out as a block exactly `width` wide and returns
// its natural border-box height. The box's own CSS sizes don't apply here:
// the table algorithm already chose its width, and its height is a minimum.
float layoutAtWidth(LayoutBox& box, float width) {
    LayoutType type = box.type;
    LayoutLength cssWidth = box.cssWidth, cssHeight = box.cssHeight;
    LayoutLength cssMinWidth = box.cssMinWidth, cssMaxWidth = box.cssMaxWidth;
    float ml = box.marginLeft, mr = box.marginRight, mt = box.marginTop, mb = box.marginBottom;
    bool mla = box.marginLeftAuto, mra = box.marginRightAuto;

    box.type = LayoutType::Block;
    box.cssWidth = box.cssHeight = box.cssMinWidth = box.cssMaxWidth = LayoutLength::none();
    box.marginLeft = box.marginRight = box.marginTop = box.marginBottom = 0;
    box.marginLeftAuto = box.marginRightAuto = false;
    box.height = 0;
    layoutBlock(box, width, 0);

    box.type = type;
    box.cssWidth = cssWidth; box.cssHeight = cssHeight;
    box.cssMinWidth = cssMinWidth; box.cssMaxWidth = cssMaxWidth;
    box.marginLeft = ml; box.marginRight = mr; box.marginTop = mt; box.marginBottom = mb;
    box.marginLeftAuto = mla; box.marginRightAuto = mra;
    box.width = width;
    box.x = 0;
    box.y = 0;
    return box.height;
}

void tableIntrinsicWidths(const LayoutBox& table, float& minW, float& maxW) {
    // The grid holds pointers into the table but only reads them here
    LayoutBox& t = const_cast<LayoutBox&>(table);
    TableGrid g;
    buildGrid(t, g);
    Columns cols;
    measureCells(g, cols, t.borderSpacingH);
    float extra = horizontalEdges(t) + (g.cols > 0 ? (g.cols + 1) * t.borderSpacingH : 0);
    minW = maxW = extra;
    for (int c = 0; c < g.cols; c++) {
        minW += cols.minW[c];
        maxW += cols.maxW[c];
    }
    for (LayoutBox* cap : g.captions) {
        float cmin, cmax;
        intrinsicWidths(*cap, cmin, cmax);
        minW = std::max(minW, cmin + horizontalEdges(t));
        maxW = std::max(maxW, cmax + horizontalEdges(t));
    }
}

} // namespace

void invalidateIntrinsicWidths() {
    s_intrinsicEpoch++;
}

void intrinsicWidths(const LayoutBox& box, float& minW, float& maxW) {
    int key = g_width + s_intrinsicEpoch * 65536;
    if (box.intrinsicKey == key) {
        minW = box.intrinsicMin;
        maxW = box.intrinsicMax;
        return;
    }
    minW = maxW = 0;
    float edges = horizontalEdges(box);

    if (box.type == LayoutType::None) {
        // nothing
    } else if (box.type == LayoutType::Table) {
        tableIntrinsicWidths(box, minW, maxW);
        float spec = fixedBorderBoxWidth(box, box.cssWidth);
        if (spec >= 0) minW = maxW = std::max(spec, minW);
    } else if (box.isImage || box.isInput) {
        float w = fixedBorderBoxWidth(box, box.cssWidth);
        bool placeholder = box.isImage && box.textureId == 0 && box.svgData.empty() && !box.text.empty();
        if (w < 0) w = placeholder ? measureTextWidth(box.text, box.fontSize) : box.width;
        minW = maxW = w;
    } else if (box.children.empty()) {
        float textW = box.text.empty() ? 0 : measureTextWidth(box.text, box.fontSize);
        // Inline-level leaves are exactly as wide as their text in layoutBlock
        minW = maxW = isBlockLevel(box.type) ? textW + edges : textW;
    } else if (box.type == LayoutType::Flex && (box.flexDirection == 0 || box.flexDirection == 2)) {
        int count = 0;
        for (const auto& child : box.children) {
            if (child.type == LayoutType::None || child.positionType == 2 || child.positionType == 3) continue;
            float cmin, cmax;
            intrinsicWidths(child, cmin, cmax);
            float m = child.marginLeft + child.marginRight;
            maxW += cmax + m + (count > 0 ? box.gap : 0);
            minW = box.flexWrap ? std::max(minW, cmin + m) : minW + cmin + m + (count > 0 ? box.gap : 0);
            count++;
        }
        minW += edges;
        maxW += edges;
    } else {
        float line = 0;
        int lineCount = 0;
        for (const auto& child : box.children) {
            if (child.type == LayoutType::None || child.positionType == 2 || child.positionType == 3) continue;
            float cmin, cmax;
            intrinsicWidths(child, cmin, cmax);
            float m = child.marginLeft + child.marginRight;
            minW = std::max(minW, cmin + m);
            if (isBlockLevel(child.type) && child.floatType == 0) {
                maxW = std::max(maxW, line);
                line = 0;
                lineCount = 0;
                maxW = std::max(maxW, cmax + m);
            } else {
                line += (lineCount > 0 ? kInlineGap : 0) + cmax + m;
                lineCount++;
            }
        }
        maxW = std::max(maxW, line);
        minW += edges;
        maxW += edges;
    }

    // Widths in CSS pixels; a block with a fixed width is that wide whatever
    // its content (cells use their width as a column hint instead)
    if (box.type != LayoutType::TableCell && box.type != LayoutType::Table &&
        box.type != LayoutType::Inline && box.type != LayoutType::Text) {
        float spec = fixedBorderBoxWidth(box, box.cssWidth);
        if (spec >= 0) minW = maxW = spec;
    }
    float maxLimit = fixedBorderBoxWidth(box, box.cssMaxWidth);
    if (maxLimit >= 0) { maxW = std::min(maxW, maxLimit); minW = std::min(minW, maxLimit); }
    float minLimit = fixedBorderBoxWidth(box, box.cssMinWidth);
    if (minLimit >= 0) { maxW = std::max(maxW, minLimit); minW = std::max(minW, minLimit); }

    // Whole pixels, so a column given its max width never wraps on rounding
    minW = std::ceil(minW);
    maxW = std::ceil(std::max(maxW, minW));
    box.intrinsicMin = minW;
    box.intrinsicMax = maxW;
    box.intrinsicKey = key;
}

void layoutTable(LayoutBox& table, float containingWidth, float startY) {
    float vpW = (float)g_width, vpH = (float)g_height;
    float spacingH = table.borderSpacingH, spacingV = table.borderSpacingV;
    float edges = horizontalEdges(table);

    TableGrid g;
    buildGrid(table, g);
    Columns cols;
    measureCells(g, cols, spacingH);
    int n = g.cols;
    float spacingTotal = n > 0 ? (n + 1) * spacingH : 0;

    float tableMin, tableMax;
    intrinsicWidths(table, tableMin, tableMax);
    bool fixedLayout = table.tableLayoutFixed && hasLength(table.cssWidth);
    if (fixedLayout) {
        // Fixed layout ignores cell content (§17.5.2.1)
        tableMin = spacingTotal + edges;
    } else if (hasLength(table.cssWidth)) {
        // intrinsicWidths folded a fixed width in; the grid's own minimum is
        // what a specified width can't go below
        float sumMin = 0;
        for (int c = 0; c < n; c++) sumMin += cols.minW[c];
        tableMin = sumMin + spacingTotal + edges;
    }

    // Used width (§17.5.2): a specified width, else shrink-to-fit
    float available = containingWidth - table.marginLeft - table.marginRight;
    float used;
    bool specified = hasLength(table.cssWidth);
    if (specified) {
        used = table.cssWidth.resolve(containingWidth, table.fontSize, vpW, vpH);
        if (table.boxSizing == 0) used += edges;
    } else {
        used = std::min(available, tableMax);
    }
    float maxLimit = hasLength(table.cssMaxWidth) ? table.cssMaxWidth.resolve(containingWidth, table.fontSize, vpW, vpH) : -1;
    float minLimit = hasLength(table.cssMinWidth) ? table.cssMinWidth.resolve(containingWidth, table.fontSize, vpW, vpH) : -1;
    if (table.boxSizing == 0) {
        if (maxLimit >= 0) maxLimit += edges;
        if (minLimit >= 0) minLimit += edges;
    }
    if (maxLimit >= 0) used = std::min(used, maxLimit);
    if (minLimit >= 0) used = std::max(used, minLimit);
    used = std::max(used, tableMin);
    table.width = used;

    // margin: auto centres the table like a block
    if (table.marginLeftAuto && table.marginRightAuto && used < containingWidth) {
        table.marginLeft = table.marginRight = (containingWidth - used) / 2.0f;
    } else if (table.marginLeftAuto) {
        table.marginLeft = std::max(0.0f, containingWidth - used - table.marginRight);
    }
    table.x = table.marginLeft;
    table.y = startY + table.marginTop;

    float colSpace = std::max(0.0f, used - edges - spacingTotal);
    if (fixedLayout) fixedColumnWidths(g, cols, colSpace, spacingH);
    else autoColumnWidths(cols, colSpace);

    std::vector<float> colX(n + 1, 0);
    float x = table.borderLeft + table.paddingLeft + spacingH;
    for (int c = 0; c < n; c++) {
        colX[c] = x;
        x += cols.width[c] + spacingH;
    }
    colX[n] = x;
    float rowLeft = table.borderLeft + table.paddingLeft + spacingH;
    float rowWidth = n > 0 ? colX[n] - spacingH - rowLeft : 0;

    // Captions sit above the grid, as wide as the table's content box
    float y = table.borderTop + table.paddingTop;
    for (LayoutBox* cap : g.captions) {
        float h = layoutAtWidth(*cap, std::max(0.0f, used - edges));
        cap->x = table.borderLeft + table.paddingLeft;
        cap->y = y;
        cap->height = h;
        y += h;
    }

    // Cells at their final widths; their heights size the rows
    auto cellWidth = [&](const GridCell& cell) {
        float w = spacingH * (cell.colSpan - 1);
        for (int c = cell.col; c < cell.col + cell.colSpan; c++) w += cols.width[c];
        return w;
    };
    for (auto& cell : g.cells) {
        cell.naturalH = layoutAtWidth(*cell.box, cellWidth(cell));
        float specH = fixedBorderBoxHeight(*cell.box, cell.box->cssHeight);
        cell.naturalH = std::max(cell.naturalH, specH);
    }

    int rowCount = (int)g.rows.size();
    std::vector<float> rowH(rowCount, 0);
    for (int r = 0; r < rowCount; r++) {
        float specH = fixedBorderBoxHeight(*g.rows[r], g.rows[r]->cssHeight);
        rowH[r] = std::max(0.0f, specH);
    }
    for (const auto& cell : g.cells) {
        if (cell.rowSpan == 1) rowH[cell.row] = std::max(rowH[cell.row], cell.naturalH);
    }
    std::vector<const GridCell*> tall;
    for (const auto& cell : g.cells) {
        if (cell.rowSpan > 1) tall.push_back(&cell);
    }
    std::stable_sort(tall.begin(), tall.end(),
                     [](const GridCell* a, const GridCell* b) { return a->rowSpan < b->rowSpan; });
    for (const GridCell* cell : tall) {
        float have = spacingV * (cell->rowSpan - 1);
        for (int r = cell->row; r < cell->row + cell->rowSpan; r++) have += rowH[r];
        if (cell->naturalH > have) {
            float perRow = (cell->naturalH - have) / cell->rowSpan;
            for (int r = cell->row; r < cell->row + cell->rowSpan; r++) rowH[r] += perRow;
        }
    }

    // Rows, in table coordinates first
    std::vector<float> rowY(rowCount, 0);
    if (rowCount > 0) y += spacingV;
    for (int r = 0; r < rowCount; r++) {
        rowY[r] = y;
        y += rowH[r] + spacingV;
    }

    for (LayoutBox* group : g.groups) {
        int first = -1, last = -1;
        for (int r = 0; r < rowCount; r++) {
            if (g.rowGroup[r] != group) continue;
            if (first < 0) first = r;
            last = r;
        }
        group->x = rowLeft;
        group->width = rowWidth;
        group->y = first >= 0 ? rowY[first] : y;
        group->height = first >= 0 ? rowY[last] + rowH[last] - rowY[first] : 0;
    }
    for (int r = 0; r < rowCount; r++) {
        LayoutBox* row = g.rows[r];
        LayoutBox* group = g.rowGroup[r];
        row->x = group ? 0 : rowLeft;
        row->y = rowY[r] - (group ? group->y : 0);
        row->width = rowWidth;
        row->height = rowH[r];
    }

    for (auto& cell : g.cells) {
        LayoutBox& box = *cell.box;
        float h = spacingV * (cell.rowSpan - 1);
        for (int r = cell.row; r < cell.row + cell.rowSpan; r++) h += rowH[r];
        box.x = colX[cell.col] - rowLeft;
        box.y = 0;
        box.height = h;

        // vertical-align: baseline is treated as top (no baselines yet)
        float offset = 0;
        if (box.verticalAlign == 2) offset = (h - cell.naturalH) / 2.0f;
        else if (box.verticalAlign == 3) offset = h - cell.naturalH;
        if (offset > 0) {
            for (auto& child : box.children) child.y += offset;
        }
    }

    float height = y + table.paddingBottom + table.borderBottom;
    float specH = hasLength(table.cssHeight) && !table.cssHeight.isPercent()
                      ? table.cssHeight.resolve(0, table.fontSize, vpW, vpH) : -1;
    if (specH >= 0 && table.boxSizing == 0) specH += verticalEdges(table);
    table.height = std::max(height, specH);
}

} // namespace ZepraBrowser
