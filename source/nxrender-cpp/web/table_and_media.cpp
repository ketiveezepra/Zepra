// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "table_and_media.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <cctype>
#include <numeric>
#include <unordered_map>

namespace NXRender {
namespace Web {

// ==================================================================
// TableLayout
// ==================================================================

void TableLayout::buildTable(BoxNode* tableBox) {
    tableBox_ = tableBox;
    columns_.clear();
    rows_.clear();
    cells_.clear();

    if (!tableBox) return;

    // Parse border-spacing
    const auto& cv = tableBox->computed();
    borderCollapse = (cv.borderCollapse == 1);
    borderSpacingH = cv.borderSpacingH;
    borderSpacingV = cv.borderSpacingV;

    int currentRow = 0;
    collectCells(tableBox, currentRow);
    computeIntrinsicWidths();
}

void TableLayout::collectCells(BoxNode* node, int& currentRow) {
    for (const auto& child : node->children()) {
        BoxNode* cn = child.get();
        const std::string& tag = cn->tag();

        if (tag == "thead" || tag == "tbody" || tag == "tfoot" || tag == "colgroup") {
            collectCells(cn, currentRow);
        } else if (tag == "tr" || cn->boxType() == BoxType::TableRow) {
            // Ensure row exists
            while (static_cast<int>(rows_.size()) <= currentRow) {
                rows_.push_back({});
            }

            int colIndex = 0;
            for (const auto& cellChild : cn->children()) {
                BoxNode* cellNode = cellChild.get();
                if (cellNode->tag() == "td" || cellNode->tag() == "th" ||
                    cellNode->boxType() == BoxType::TableCell) {

                    // Skip columns already occupied by rowspans
                    while (colIndex < static_cast<int>(rows_[currentRow].cells.size()) &&
                           rows_[currentRow].cells[colIndex] != nullptr) {
                        colIndex++;
                    }

                    Cell cell;
                    cell.node = cellNode;
                    cell.rowIndex = currentRow;
                    cell.colIndex = colIndex;
                    cell.colSpan = std::max(1, static_cast<int>(cellNode->computed().colSpan));
                    cell.rowSpan = std::max(1, static_cast<int>(cellNode->computed().rowSpan));

                    cells_.push_back(cell);

                    // Ensure columns exist
                    while (static_cast<int>(columns_.size()) < colIndex + cell.colSpan) {
                        columns_.push_back({});
                    }

                    // Mark occupied slots for rowspan/colspan
                    for (int r = currentRow; r < currentRow + cell.rowSpan; r++) {
                        while (static_cast<int>(rows_.size()) <= r) rows_.push_back({});
                        while (static_cast<int>(rows_[r].cells.size()) < colIndex + cell.colSpan) {
                            rows_[r].cells.push_back(nullptr);
                        }
                        for (int c = colIndex; c < colIndex + cell.colSpan; c++) {
                            rows_[r].cells[c] = cellNode;
                        }
                    }

                    colIndex += cell.colSpan;
                }
            }
            currentRow++;
        } else if (tag == "col") {
            // Column definitions
            int span = std::max(1, static_cast<int>(cn->computed().colSpan));
            for (int i = 0; i < span; i++) {
                Column col;
                if (!cn->computed().widthAuto) {
                    col.fixedWidth = cn->computed().width;
                    col.hasFixed = true;
                }
                columns_.push_back(col);
            }
        }
    }
}

void TableLayout::computeIntrinsicWidths() {
    for (auto& cell : cells_) {
        if (!cell.node) continue;

        // Estimate min/max content width from cell content
        float textWidth = 0;
        float minWord = 0;

        // Walk text children
        std::function<void(const BoxNode*)> measureText = [&](const BoxNode* n) {
            if (n->isTextNode()) {
                const std::string& text = n->text();
                float fontSize = n->computed().fontSize;
                float charWidth = fontSize * 0.6f; // Approximate

                // Max content = total text width
                textWidth += text.size() * charWidth;

                // Min content = longest word
                std::istringstream words(text);
                std::string word;
                while (words >> word) {
                    float wordWidth = word.size() * charWidth;
                    minWord = std::max(minWord, wordWidth);
                }
            }
            for (const auto& c : n->children()) measureText(c.get());
        };
        measureText(cell.node);

        // Add padding
        const auto& cv = cell.node->computed();
        float hPad = cv.paddingLeft + cv.paddingRight;
        cell.minContentWidth = minWord + hPad;
        cell.maxContentWidth = textWidth + hPad;
        cell.minContentHeight = cv.paddingTop + cv.paddingBottom + cv.fontSize * 1.5f;

        // Distribute to columns (single-span cells only)
        if (cell.colSpan == 1 && cell.colIndex < static_cast<int>(columns_.size())) {
            auto& col = columns_[cell.colIndex];
            col.minWidth = std::max(col.minWidth, cell.minContentWidth);
            col.maxWidth = std::max(col.maxWidth, cell.maxContentWidth);

            if (!cell.node->computed().widthAuto) {
                col.fixedWidth = std::max(col.fixedWidth, cell.node->computed().width);
                col.hasFixed = true;
            }
        }
    }
}

void TableLayout::layoutFixed(float availableWidth) {
    float totalBorderSpacing = borderCollapse ? 0 :
        borderSpacingH * (columns_.size() + 1);
    float usable = availableWidth - totalBorderSpacing;

    int fixedCount = 0;
    float fixedTotal = 0;
    for (auto& col : columns_) {
        if (col.hasFixed) {
            col.resolvedWidth = col.fixedWidth;
            fixedTotal += col.fixedWidth;
            fixedCount++;
        }
    }

    int autoCount = static_cast<int>(columns_.size()) - fixedCount;
    float remaining = usable - fixedTotal;
    float autoWidth = (autoCount > 0) ? remaining / autoCount : 0;

    for (auto& col : columns_) {
        if (!col.hasFixed) {
            col.resolvedWidth = std::max(0.0f, autoWidth);
        }
    }
}

void TableLayout::layoutAuto(float availableWidth) {
    float totalBorderSpacing = borderCollapse ? 0 :
        borderSpacingH * (columns_.size() + 1);
    float usable = availableWidth - totalBorderSpacing;

    // Start with min widths
    float totalMin = 0;
    for (auto& col : columns_) {
        col.resolvedWidth = col.minWidth;
        totalMin += col.minWidth;
    }

    float excess = usable - totalMin;
    if (excess > 0) {
        // Distribute proportionally based on max-min difference
        float totalDiff = 0;
        for (auto& col : columns_) {
            totalDiff += (col.maxWidth - col.minWidth);
        }

        if (totalDiff > 0) {
            for (auto& col : columns_) {
                float share = (col.maxWidth - col.minWidth) / totalDiff;
                col.resolvedWidth += excess * share;
            }
        } else {
            // Equal distribution
            float perCol = excess / columns_.size();
            for (auto& col : columns_) {
                col.resolvedWidth += perCol;
            }
        }
    }
}

void TableLayout::distributeWidth(float availableWidth) {
    if (tableBox_ && !tableBox_->computed().widthAuto &&
        tableBox_->computed().tableLayout == 1) {
        layoutFixed(availableWidth);
    } else {
        layoutAuto(availableWidth);
    }
}

void TableLayout::computeRowHeights() {
    for (auto& row : rows_) {
        row.height = row.minHeight;
    }

    // Single-rowspan cells
    for (const auto& cell : cells_) {
        if (cell.rowSpan == 1 && cell.rowIndex < static_cast<int>(rows_.size())) {
            float cellWidth = 0;
            for (int c = cell.colIndex; c < cell.colIndex + cell.colSpan &&
                   c < static_cast<int>(columns_.size()); c++) {
                cellWidth += columns_[c].resolvedWidth;
            }

            // Estimate height from content
            float contentHeight = cell.minContentHeight;
            if (cell.maxContentWidth > 0 && cellWidth > 0) {
                float wrappedLines = std::ceil(cell.maxContentWidth / cellWidth);
                contentHeight = wrappedLines * cell.node->computed().fontSize * 1.5f +
                                 cell.node->computed().paddingTop + cell.node->computed().paddingBottom;
            }

            rows_[cell.rowIndex].height = std::max(rows_[cell.rowIndex].height, contentHeight);
        }
    }

    // Multi-rowspan: distribute excess height
    for (const auto& cell : cells_) {
        if (cell.rowSpan > 1) {
            float spannedHeight = 0;
            for (int r = cell.rowIndex; r < cell.rowIndex + cell.rowSpan &&
                   r < static_cast<int>(rows_.size()); r++) {
                spannedHeight += rows_[r].height;
            }
            spannedHeight += borderSpacingV * (cell.rowSpan - 1);

            if (cell.minContentHeight > spannedHeight) {
                float excess = cell.minContentHeight - spannedHeight;
                float perRow = excess / cell.rowSpan;
                for (int r = cell.rowIndex; r < cell.rowIndex + cell.rowSpan &&
                       r < static_cast<int>(rows_.size()); r++) {
                    rows_[r].height += perRow;
                }
            }
        }
    }
}

void TableLayout::positionCells(float tableX, float tableY) {
    float y = tableY + (borderCollapse ? 0 : borderSpacingV);

    for (int r = 0; r < static_cast<int>(rows_.size()); r++) {
        float x = tableX + (borderCollapse ? 0 : borderSpacingH);

        for (const auto& cell : cells_) {
            if (cell.rowIndex != r || !cell.node) continue;

            // Compute cell position
            float cellX = tableX + (borderCollapse ? 0 : borderSpacingH);
            for (int c = 0; c < cell.colIndex && c < static_cast<int>(columns_.size()); c++) {
                cellX += columns_[c].resolvedWidth + (borderCollapse ? 0 : borderSpacingH);
            }

            float cellWidth = 0;
            for (int c = cell.colIndex; c < cell.colIndex + cell.colSpan &&
                   c < static_cast<int>(columns_.size()); c++) {
                cellWidth += columns_[c].resolvedWidth;
                if (c > cell.colIndex) cellWidth += (borderCollapse ? 0 : borderSpacingH);
            }

            float cellHeight = 0;
            for (int rr = cell.rowIndex; rr < cell.rowIndex + cell.rowSpan &&
                   rr < static_cast<int>(rows_.size()); rr++) {
                cellHeight += rows_[rr].height;
                if (rr > cell.rowIndex) cellHeight += (borderCollapse ? 0 : borderSpacingV);
            }

            auto& lb = cell.node->layoutBox();
            lb.x = cellX;
            lb.y = y;
            lb.width = cellWidth;
            lb.height = cellHeight;
            lb.contentX = cellX + cell.node->computed().paddingLeft;
            lb.contentY = y + cell.node->computed().paddingTop;
            lb.contentWidth = cellWidth - cell.node->computed().paddingLeft - cell.node->computed().paddingRight;
            lb.contentHeight = cellHeight - cell.node->computed().paddingTop - cell.node->computed().paddingBottom;
        }

        y += rows_[r].height + (borderCollapse ? 0 : borderSpacingV);
    }

    // Set table box dimensions
    if (tableBox_) {
        auto& lb = tableBox_->layoutBox();
        lb.height = y - tableY;
        lb.contentHeight = lb.height - lb.paddingTop - lb.paddingBottom;
    }
}

float TableLayout::totalWidth() const {
    float w = 0;
    for (const auto& col : columns_) w += col.resolvedWidth;
    if (!borderCollapse) w += borderSpacingH * (columns_.size() + 1);
    return w;
}

float TableLayout::totalHeight() const {
    float h = 0;
    for (const auto& row : rows_) h += row.height;
    if (!borderCollapse) h += borderSpacingV * (rows_.size() + 1);
    return h;
}

float TableLayout::resolveCollapsedBorder(BoxNode* cell1, BoxNode* cell2, bool /*horizontal*/) const {
    float w1 = cell1 ? cell1->computed().borderRightWidth : 0;
    float w2 = cell2 ? cell2->computed().borderLeftWidth : 0;
    return std::max(w1, w2);
}

// ==================================================================
// PseudoElementRenderer
// ==================================================================

int PseudoElementRenderer::quoteDepth_ = 0;

std::unique_ptr<BoxNode> PseudoElementRenderer::generateBefore(BoxNode* parent,
                                                                     const ComputedValues& pseudoStyle) {
    if (pseudoStyle.content.empty() || pseudoStyle.content == "none" ||
        pseudoStyle.content == "normal") return nullptr;

    auto box = std::make_unique<BoxNode>();
    box->setTag("::before");
    box->setBoxType(pseudoStyle.display == 0 ? BoxType::None :
                      (pseudoStyle.display == 2 ? BoxType::Inline : BoxType::Block));
    box->setComputedValues(pseudoStyle);

    std::string text = processContent(pseudoStyle.content, parent);
    if (!text.empty()) {
        auto textNode = std::make_unique<BoxNode>();
        textNode->setTag("#text");
        textNode->setText(text);
        textNode->setBoxType(BoxType::Inline);
        ComputedValues textCv = pseudoStyle;
        textNode->setComputedValues(textCv);
        box->appendChild(std::move(textNode));
    }

    return box;
}

std::unique_ptr<BoxNode> PseudoElementRenderer::generateAfter(BoxNode* parent,
                                                                    const ComputedValues& pseudoStyle) {
    if (pseudoStyle.content.empty() || pseudoStyle.content == "none" ||
        pseudoStyle.content == "normal") return nullptr;

    auto box = std::make_unique<BoxNode>();
    box->setTag("::after");
    box->setBoxType(pseudoStyle.display == 0 ? BoxType::None :
                      (pseudoStyle.display == 2 ? BoxType::Inline : BoxType::Block));
    box->setComputedValues(pseudoStyle);

    std::string text = processContent(pseudoStyle.content, parent);
    if (!text.empty()) {
        auto textNode = std::make_unique<BoxNode>();
        textNode->setTag("#text");
        textNode->setText(text);
        textNode->setBoxType(BoxType::Inline);
        textNode->setComputedValues(pseudoStyle);
        box->appendChild(std::move(textNode));
    }

    return box;
}

std::unique_ptr<BoxNode> PseudoElementRenderer::generateMarker(BoxNode* listItem, int ordinal) {
    if (!listItem) return nullptr;

    auto box = std::make_unique<BoxNode>();
    box->setTag("::marker");
    box->setBoxType(BoxType::Inline);

    ComputedValues cv = listItem->computed();
    std::string markerText = CSSCounterManager::markerText(ordinal, cv.listStyleType);

    auto textNode = std::make_unique<BoxNode>();
    textNode->setTag("#text");
    textNode->setText(markerText + " ");
    textNode->setBoxType(BoxType::Inline);
    textNode->setComputedValues(cv);
    box->appendChild(std::move(textNode));

    return box;
}

void PseudoElementRenderer::applyFirstLine(BoxNode* block, const ComputedValues& firstLineStyle) {
    if (!block || block->children().empty()) return;

    // Find first text node and apply style overrides
    BoxNode* firstText = nullptr;
    std::function<void(BoxNode*)> findFirst = [&](BoxNode* n) {
        if (firstText) return;
        if (n->isTextNode()) { firstText = n; return; }
        for (const auto& c : n->children()) findFirst(c.get());
    };
    findFirst(block);

    if (firstText) {
        ComputedValues merged = firstText->computed();
        if (firstLineStyle.color) merged.color = firstLineStyle.color;
        if (firstLineStyle.fontSize > 0) merged.fontSize = firstLineStyle.fontSize;
        if (firstLineStyle.fontWeight > 0) merged.fontWeight = firstLineStyle.fontWeight;
        firstText->setComputedValues(merged);
    }
}

std::unique_ptr<BoxNode> PseudoElementRenderer::generateFirstLetter(BoxNode* block,
                                                                           const ComputedValues& letterStyle) {
    if (!block) return nullptr;

    // Find first text content
    std::string firstChar;
    BoxNode* textNode = nullptr;
    std::function<void(BoxNode*)> findFirst = [&](BoxNode* n) {
        if (!firstChar.empty()) return;
        if (n->isTextNode() && !n->text().empty()) {
            textNode = n;
            // Extract first letter (handle multi-byte UTF-8)
            const std::string& text = n->text();
            size_t i = 0;
            // Skip leading whitespace and punctuation
            while (i < text.size() && (std::isspace(text[i]) || std::ispunct(text[i]))) {
                firstChar += text[i];
                i++;
            }
            if (i < text.size()) {
                uint8_t byte = text[i];
                if (byte < 0x80) firstChar += text[i];
                else if ((byte & 0xE0) == 0xC0) firstChar += text.substr(i, 2);
                else if ((byte & 0xF0) == 0xE0) firstChar += text.substr(i, 3);
                else firstChar += text.substr(i, 4);
            }
            return;
        }
        for (const auto& c : n->children()) findFirst(c.get());
    };
    findFirst(block);

    if (firstChar.empty()) return nullptr;

    auto box = std::make_unique<BoxNode>();
    box->setTag("::first-letter");
    box->setBoxType(BoxType::Inline);
    box->setComputedValues(letterStyle);

    auto tn = std::make_unique<BoxNode>();
    tn->setTag("#text");
    tn->setText(firstChar);
    tn->setBoxType(BoxType::Inline);
    tn->setComputedValues(letterStyle);
    box->appendChild(std::move(tn));

    return box;
}

std::unique_ptr<BoxNode> PseudoElementRenderer::generatePlaceholder(BoxNode* input,
                                                                           const std::string& text) {
    if (!input || text.empty()) return nullptr;

    auto box = std::make_unique<BoxNode>();
    box->setTag("::placeholder");
    box->setBoxType(BoxType::Inline);

    ComputedValues cv = input->computed();
    cv.color = 0x9CA3AF80; // Gray placeholder color
    box->setComputedValues(cv);

    auto tn = std::make_unique<BoxNode>();
    tn->setTag("#text");
    tn->setText(text);
    tn->setBoxType(BoxType::Inline);
    tn->setComputedValues(cv);
    box->appendChild(std::move(tn));

    return box;
}

PseudoElementRenderer::SelectionStyle PseudoElementRenderer::getSelectionStyle(const BoxNode* /*node*/) {
    return {0xFFFFFFFF, 0x3390FFFF};
}

std::string PseudoElementRenderer::processContent(const std::string& contentValue,
                                                         const BoxNode* parent) {
    std::string result;
    size_t i = 0;
    while (i < contentValue.size()) {
        // Skip whitespace
        while (i < contentValue.size() && std::isspace(contentValue[i])) i++;
        if (i >= contentValue.size()) break;

        // Quoted string
        if (contentValue[i] == '"' || contentValue[i] == '\'') {
            char quote = contentValue[i++];
            while (i < contentValue.size() && contentValue[i] != quote) {
                if (contentValue[i] == '\\' && i + 1 < contentValue.size()) {
                    i++;
                    // Escape sequences
                    if (contentValue[i] == 'n') result += '\n';
                    else if (contentValue[i] == 't') result += '\t';
                    else if (contentValue[i] == '\\') result += '\\';
                    else result += contentValue[i];
                } else {
                    result += contentValue[i];
                }
                i++;
            }
            if (i < contentValue.size()) i++; // skip closing quote
        }
        // open-quote / close-quote
        else if (contentValue.compare(i, 10, "open-quote") == 0) {
            result += (quoteDepth_ % 2 == 0) ? "\u201C" : "\u2018"; // " or '
            quoteDepth_++;
            i += 10;
        }
        else if (contentValue.compare(i, 11, "close-quote") == 0) {
            quoteDepth_ = std::max(0, quoteDepth_ - 1);
            result += (quoteDepth_ % 2 == 0) ? "\u201D" : "\u2019"; // " or '
            i += 11;
        }
        // counter()
        else if (contentValue.compare(i, 8, "counter(") == 0) {
            size_t end = contentValue.find(')', i);
            if (end != std::string::npos) {
                result += parseContentFunction(contentValue.substr(i, end - i + 1), parent);
                i = end + 1;
            } else {
                i++;
            }
        }
        // attr()
        else if (contentValue.compare(i, 5, "attr(") == 0) {
            size_t end = contentValue.find(')', i);
            if (end != std::string::npos) {
                result += parseContentFunction(contentValue.substr(i, end - i + 1), parent);
                i = end + 1;
            } else {
                i++;
            }
        }
        else {
            i++;
        }
    }
    return result;
}

std::string PseudoElementRenderer::parseContentFunction(const std::string& func,
                                                              const BoxNode* /*parent*/) {
    if (func.find("counter(") == 0) {
        std::string name = func.substr(8, func.size() - 9);
        // Remove style parameter if present
        size_t comma = name.find(',');
        std::string style = "decimal";
        if (comma != std::string::npos) {
            style = name.substr(comma + 1);
            while (!style.empty() && std::isspace(style.front())) style.erase(0, 1);
            while (!style.empty() && std::isspace(style.back())) style.pop_back();
            name = name.substr(0, comma);
        }
        while (!name.empty() && std::isspace(name.back())) name.pop_back();
        return CSSCounterManager::instance().counterValue(name, style);
    }
    return "";
}

// ==================================================================
// CSSCounterManager
// ==================================================================

CSSCounterManager& CSSCounterManager::instance() {
    static CSSCounterManager mgr;
    return mgr;
}

void CSSCounterManager::resetCounter(const std::string& name, int value) {
    if (scopes_.empty()) pushScope();
    scopes_.back().counters[name] = value;
}

void CSSCounterManager::incrementCounter(const std::string& name, int amount) {
    // Find counter in current or ancestor scope
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->counters.find(name);
        if (found != it->counters.end()) {
            found->second += amount;
            return;
        }
    }
    // Implicitly create
    if (scopes_.empty()) pushScope();
    scopes_.back().counters[name] = amount;
}

void CSSCounterManager::setCounter(const std::string& name, int value) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->counters.find(name);
        if (found != it->counters.end()) {
            found->second = value;
            return;
        }
    }
    if (scopes_.empty()) pushScope();
    scopes_.back().counters[name] = value;
}

int CSSCounterManager::getCounterValue(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->counters.find(name);
        if (found != it->counters.end()) return found->second;
    }
    return 0;
}

std::string CSSCounterManager::counterValue(const std::string& name, const std::string& style) const {
    int val = getCounterValue(name);
    return formatNumber(val, style);
}

std::string CSSCounterManager::countersValue(const std::string& name, const std::string& separator,
                                                    const std::string& style) const {
    std::string result;
    for (const auto& scope : scopes_) {
        auto it = scope.counters.find(name);
        if (it != scope.counters.end()) {
            if (!result.empty()) result += separator;
            result += formatNumber(it->second, style);
        }
    }
    return result.empty() ? "0" : result;
}

void CSSCounterManager::pushScope() {
    scopes_.push_back({});
}

void CSSCounterManager::popScope() {
    if (!scopes_.empty()) scopes_.pop_back();
}

std::string CSSCounterManager::formatNumber(int value, const std::string& style) {
    if (style == "decimal" || style.empty()) return std::to_string(value);
    if (style == "decimal-leading-zero") {
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d", value);
        return buf;
    }
    if (style == "lower-roman") return toRoman(value, false);
    if (style == "upper-roman") return toRoman(value, true);
    if (style == "lower-alpha" || style == "lower-latin") return toAlpha(value, false);
    if (style == "upper-alpha" || style == "upper-latin") return toAlpha(value, true);
    if (style == "lower-greek") return toGreek(value);
    if (style == "cjk-decimal") return toCJK(value);
    if (style == "disc") return "\u2022";     // •
    if (style == "circle") return "\u25E6";   // ◦
    if (style == "square") return "\u25AA";   // ▪
    if (style == "none") return "";
    return std::to_string(value);
}

std::string CSSCounterManager::markerText(int ordinal, const std::string& listStyleType) {
    if (listStyleType == "none") return "";
    if (listStyleType == "disc" || listStyleType.empty()) return "\u2022";
    if (listStyleType == "circle") return "\u25E6";
    if (listStyleType == "square") return "\u25AA";
    if (listStyleType == "disclosure-open") return "\u25BE";
    if (listStyleType == "disclosure-closed") return "\u25B8";
    return formatNumber(ordinal, listStyleType) + ".";
}

std::string CSSCounterManager::toRoman(int value, bool upper) {
    if (value <= 0 || value > 3999) return std::to_string(value);

    const int vals[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
    const char* syms[] = {"m","cm","d","cd","c","xc","l","xl","x","ix","v","iv","i"};
    const char* symsU[] = {"M","CM","D","CD","C","XC","L","XL","X","IX","V","IV","I"};

    std::string result;
    for (int i = 0; i < 13; i++) {
        while (value >= vals[i]) {
            result += upper ? symsU[i] : syms[i];
            value -= vals[i];
        }
    }
    return result;
}

std::string CSSCounterManager::toAlpha(int value, bool upper) {
    if (value <= 0) return std::to_string(value);
    std::string result;
    while (value > 0) {
        value--;
        result = char((upper ? 'A' : 'a') + (value % 26)) + result;
        value /= 26;
    }
    return result;
}

std::string CSSCounterManager::toGreek(int value) {
    if (value <= 0 || value > 24) return std::to_string(value);
    const char* greek[] = {"α","β","γ","δ","ε","ζ","η","θ","ι","κ","λ","μ",
                            "ν","ξ","ο","π","ρ","σ","τ","υ","φ","χ","ψ","ω"};
    return greek[value - 1];
}

std::string CSSCounterManager::toCJK(int value) {
    if (value < 0 || value > 9) return std::to_string(value);
    const char* cjk[] = {"〇","一","二","三","四","五","六","七","八","九"};
    if (value == 0) return cjk[0];

    std::string result;
    while (value > 0) {
        result = std::string(cjk[value % 10]) + result;
        value /= 10;
    }
    return result;
}

// ==================================================================
// TextDecoration
// ==================================================================

TextDecoration TextDecoration::parse(const std::string& value) {
    TextDecoration td;
    if (value.find("underline") != std::string::npos)
        td.lines |= static_cast<uint8_t>(Line::Underline);
    if (value.find("overline") != std::string::npos)
        td.lines |= static_cast<uint8_t>(Line::Overline);
    if (value.find("line-through") != std::string::npos)
        td.lines |= static_cast<uint8_t>(Line::LineThrough);

    if (value.find("dotted") != std::string::npos) td.style = Style::Dotted;
    else if (value.find("dashed") != std::string::npos) td.style = Style::Dashed;
    else if (value.find("double") != std::string::npos) td.style = Style::Double;
    else if (value.find("wavy") != std::string::npos) td.style = Style::Wavy;

    return td;
}

// ==================================================================
// Shadow
// ==================================================================

std::vector<Shadow> Shadow::parse(const std::string& value) {
    std::vector<Shadow> shadows;
    if (value.empty() || value == "none") return shadows;

    // Split by comma (simple — doesn't handle commas inside rgb())
    std::istringstream stream(value);
    std::string segment;

    // Simplified parser: split on commas not inside parens
    std::vector<std::string> segments;
    int parenDepth = 0;
    std::string current;
    for (char c : value) {
        if (c == '(') parenDepth++;
        else if (c == ')') parenDepth--;
        else if (c == ',' && parenDepth == 0) {
            segments.push_back(current);
            current.clear();
            continue;
        }
        current += c;
    }
    if (!current.empty()) segments.push_back(current);

    for (auto& seg : segments) {
        Shadow shadow;
        std::istringstream ss(seg);
        std::string token;
        std::vector<float> numbers;

        // Check for inset
        if (seg.find("inset") != std::string::npos) {
            shadow.inset = true;
        }

        // Extract numeric values (px or plain numbers)
        std::istringstream numStream(seg);
        while (numStream >> token) {
            if (token == "inset") continue;
            // Skip color names/functions
            if (token.find("rgb") != std::string::npos ||
                token.find("hsl") != std::string::npos ||
                token.find('#') != std::string::npos) {
                // Parse color
                if (token[0] == '#' && token.size() == 7) {
                    uint32_t r = std::stoi(token.substr(1, 2), nullptr, 16);
                    uint32_t g = std::stoi(token.substr(3, 2), nullptr, 16);
                    uint32_t b = std::stoi(token.substr(5, 2), nullptr, 16);
                    shadow.color = (r << 24) | (g << 16) | (b << 8) | 0xFF;
                }
                continue;
            }

            try {
                float val = std::stof(token);
                numbers.push_back(val);
            } catch (...) {}
        }

        if (numbers.size() >= 2) {
            shadow.offsetX = numbers[0];
            shadow.offsetY = numbers[1];
            if (numbers.size() >= 3) shadow.blurRadius = numbers[2];
            if (numbers.size() >= 4) shadow.spreadRadius = numbers[3];
        }

        shadows.push_back(shadow);
    }

    return shadows;
}

// ==================================================================
// TextEffects
// ==================================================================

std::vector<TextEffects::DecorationLine> TextEffects::computeDecorations(
    const BoxNode* textNode, float baselineY, float fontSize) {

    std::vector<DecorationLine> lines;
    if (!textNode) return lines;

    const auto& cv = textNode->computed();
    TextDecoration td = TextDecoration::parse(cv.textDecoration);
    if (td.lines == 0) return lines;

    float x = textNode->layoutBox().x;
    float w = textNode->layoutBox().width;
    float thickness = std::max(1.0f, fontSize / 12.0f);

    if (td.hasUnderline()) {
        lines.push_back({x, baselineY + fontSize * 0.15f, w, thickness, td.style,
                           td.color ? td.color : cv.color});
    }
    if (td.hasOverline()) {
        lines.push_back({x, baselineY - fontSize * 0.85f, w, thickness, td.style,
                           td.color ? td.color : cv.color});
    }
    if (td.hasLineThrough()) {
        lines.push_back({x, baselineY - fontSize * 0.3f, w, thickness, td.style,
                           td.color ? td.color : cv.color});
    }

    return lines;
}

std::vector<TextEffects::ShadowParams> TextEffects::computeTextShadows(const BoxNode* node) {
    std::vector<ShadowParams> params;
    if (!node) return params;

    auto shadows = Shadow::parse(node->computed().textShadow);
    for (const auto& s : shadows) {
        ShadowParams sp;
        sp.offsetX = s.offsetX;
        sp.offsetY = s.offsetY;
        sp.blurRadius = s.blurRadius;
        sp.color = s.color;
        params.push_back(sp);
    }
    return params;
}

std::vector<Shadow> TextEffects::computeBoxShadows(const BoxNode* node) {
    if (!node) return {};
    return Shadow::parse(node->computed().boxShadow);
}

std::string TextEffects::applyTextOverflow(const std::string& text, float maxWidth,
                                                 float fontSize, const std::string& overflow) {
    if (overflow != "ellipsis" || text.empty()) return text;

    float charWidth = fontSize * 0.6f;
    int maxChars = static_cast<int>(maxWidth / charWidth);
    if (maxChars <= 0) return "";
    if (static_cast<int>(text.size()) <= maxChars) return text;

    return text.substr(0, std::max(0, maxChars - 1)) + "\u2026"; // …
}

TextEffects::WordBreak TextEffects::parseWordBreak(const std::string& value) {
    if (value == "break-all") return WordBreak::BreakAll;
    if (value == "keep-all") return WordBreak::KeepAll;
    if (value == "break-word") return WordBreak::BreakWord;
    return WordBreak::Normal;
}

bool TextEffects::shouldHyphenate(const std::string& /*word*/, const std::string& /*lang*/) {
    // Simplified: would need a hyphenation dictionary
    return false;
}

std::vector<int> TextEffects::hyphenationPoints(const std::string& word, const std::string& /*lang*/) {
    std::vector<int> points;
    // Basic syllable boundary heuristic
    for (size_t i = 2; i < word.size() - 2; i++) {
        bool prevVowel = std::string("aeiouAEIOU").find(word[i-1]) != std::string::npos;
        bool currConsonant = std::string("aeiouAEIOU").find(word[i]) == std::string::npos;
        if (prevVowel && currConsonant) {
            points.push_back(static_cast<int>(i));
        }
    }
    return points;
}

std::string TextEffects::applyTextTransform(const std::string& text, const std::string& transform) {
    if (transform == "uppercase") {
        std::string result = text;
        std::transform(result.begin(), result.end(), result.begin(), ::toupper);
        return result;
    }
    if (transform == "lowercase") {
        std::string result = text;
        std::transform(result.begin(), result.end(), result.begin(), ::tolower);
        return result;
    }
    if (transform == "capitalize") {
        std::string result = text;
        bool wordStart = true;
        for (size_t i = 0; i < result.size(); i++) {
            if (std::isspace(result[i])) { wordStart = true; continue; }
            if (wordStart) { result[i] = toupper(result[i]); wordStart = false; }
        }
        return result;
    }
    return text;
}

float TextEffects::computeLetterSpacing(const ComputedValues& cv) {
    return cv.letterSpacing;
}

float TextEffects::computeWordSpacing(const ComputedValues& cv) {
    return cv.wordSpacing;
}

TextEffects::WritingMode TextEffects::parseWritingMode(const std::string& value) {
    if (value == "vertical-rl") return WritingMode::VerticalRL;
    if (value == "vertical-lr") return WritingMode::VerticalLR;
    return WritingMode::HorizontalTB;
}

// ==================================================================
// ImageRenderer
// ==================================================================

ImageRenderer::DrawRect ImageRenderer::computeObjectFit(
    ObjectFit fit, float imageWidth, float imageHeight,
    float boxWidth, float boxHeight,
    float objectPositionX, float objectPositionY) {

    DrawRect dr;
    dr.srcX = 0; dr.srcY = 0; dr.srcW = imageWidth; dr.srcH = imageHeight;

    if (imageWidth <= 0 || imageHeight <= 0 || boxWidth <= 0 || boxHeight <= 0) {
        dr.dstX = 0; dr.dstY = 0; dr.dstW = boxWidth; dr.dstH = boxHeight;
        return dr;
    }

    float imageAspect = imageWidth / imageHeight;
    float boxAspect = boxWidth / boxHeight;

    switch (fit) {
        case ObjectFit::Fill:
            dr.dstX = 0; dr.dstY = 0;
            dr.dstW = boxWidth; dr.dstH = boxHeight;
            break;

        case ObjectFit::Contain: {
            float scale;
            if (imageAspect > boxAspect) {
                scale = boxWidth / imageWidth;
            } else {
                scale = boxHeight / imageHeight;
            }
            dr.dstW = imageWidth * scale;
            dr.dstH = imageHeight * scale;
            dr.dstX = (boxWidth - dr.dstW) * objectPositionX;
            dr.dstY = (boxHeight - dr.dstH) * objectPositionY;
            break;
        }

        case ObjectFit::Cover: {
            float scale;
            if (imageAspect > boxAspect) {
                scale = boxHeight / imageHeight;
            } else {
                scale = boxWidth / imageWidth;
            }
            float scaledW = imageWidth * scale;
            float scaledH = imageHeight * scale;

            // Clip source to visible area
            float clipX = (scaledW - boxWidth) * objectPositionX / scale;
            float clipY = (scaledH - boxHeight) * objectPositionY / scale;

            dr.srcX = clipX;
            dr.srcY = clipY;
            dr.srcW = boxWidth / scale;
            dr.srcH = boxHeight / scale;
            dr.dstX = 0; dr.dstY = 0;
            dr.dstW = boxWidth; dr.dstH = boxHeight;
            break;
        }

        case ObjectFit::None:
            dr.dstW = imageWidth;
            dr.dstH = imageHeight;
            dr.dstX = (boxWidth - imageWidth) * objectPositionX;
            dr.dstY = (boxHeight - imageHeight) * objectPositionY;
            break;

        case ObjectFit::ScaleDown: {
            if (imageWidth <= boxWidth && imageHeight <= boxHeight) {
                // None behavior
                dr.dstW = imageWidth;
                dr.dstH = imageHeight;
            } else {
                // Contain behavior
                float scale = (imageAspect > boxAspect)
                    ? boxWidth / imageWidth : boxHeight / imageHeight;
                dr.dstW = imageWidth * scale;
                dr.dstH = imageHeight * scale;
            }
            dr.dstX = (boxWidth - dr.dstW) * objectPositionX;
            dr.dstY = (boxHeight - dr.dstH) * objectPositionY;
            break;
        }
    }

    return dr;
}

ImageRenderer::ObjectFit ImageRenderer::parseObjectFit(const std::string& value) {
    if (value == "contain") return ObjectFit::Contain;
    if (value == "cover") return ObjectFit::Cover;
    if (value == "none") return ObjectFit::None;
    if (value == "scale-down") return ObjectFit::ScaleDown;
    return ObjectFit::Fill;
}

ImageRenderer::ObjectPosition ImageRenderer::parseObjectPosition(const std::string& value) {
    ObjectPosition pos;
    if (value.empty() || value == "center") return pos;

    std::istringstream ss(value);
    std::string part;
    int idx = 0;
    while (ss >> part) {
        if (part == "left") pos.x = 0;
        else if (part == "right") pos.x = 1;
        else if (part == "top") pos.y = 0;
        else if (part == "bottom") pos.y = 1;
        else if (part == "center") { if (idx == 0) pos.x = 0.5f; else pos.y = 0.5f; }
        else if (part.back() == '%') {
            float pct = std::stof(part.substr(0, part.size() - 1)) / 100.0f;
            if (idx == 0) pos.x = pct; else pos.y = pct;
        }
        idx++;
    }
    return pos;
}

ImageRenderer::AspectRatio ImageRenderer::parseAspectRatio(const std::string& value) {
    AspectRatio ar;
    if (value.empty() || value == "auto") return ar;

    size_t slash = value.find('/');
    if (slash != std::string::npos) {
        float w = std::stof(value.substr(0, slash));
        float h = std::stof(value.substr(slash + 1));
        if (h > 0) { ar.ratio = w / h; ar.hasRatio = true; }
    } else {
        try {
            ar.ratio = std::stof(value);
            ar.hasRatio = true;
        } catch (...) {}
    }
    return ar;
}

Size ImageRenderer::computeSizeWithAspectRatio(float availableW, float availableH,
                                                     const AspectRatio& ratio,
                                                     const ComputedValues& cv) {
    Size size;
    if (!ratio.hasRatio || ratio.ratio <= 0) {
        size.width = cv.widthAuto ? availableW : cv.width;
        size.height = cv.heightAuto ? availableH : cv.height;
        return size;
    }

    if (!cv.widthAuto && !cv.heightAuto) {
        size.width = cv.width;
        size.height = cv.height;
    } else if (!cv.widthAuto) {
        size.width = cv.width;
        size.height = cv.width / ratio.ratio;
    } else if (!cv.heightAuto) {
        size.height = cv.height;
        size.width = cv.height * ratio.ratio;
    } else {
        size.width = availableW;
        size.height = availableW / ratio.ratio;
        if (size.height > availableH) {
            size.height = availableH;
            size.width = availableH * ratio.ratio;
        }
    }
    return size;
}

ImageRenderer::Loading ImageRenderer::parseLoading(const std::string& value) {
    return (value == "lazy") ? Loading::Lazy : Loading::Eager;
}

ImageRenderer::Decoding ImageRenderer::parseDecoding(const std::string& value) {
    if (value == "sync") return Decoding::Sync;
    if (value == "async") return Decoding::Async;
    return Decoding::Auto;
}

// ==================================================================
// BackgroundCompositor
// ==================================================================

std::vector<BackgroundCompositor::BackgroundLayer> BackgroundCompositor::parse(const std::string& background) {
    std::vector<BackgroundLayer> layers;
    if (background.empty() || background == "none") return layers;

    // Simple single-layer parse
    BackgroundLayer layer;

    // Check for color
    if (background[0] == '#') {
        if (background.size() == 7) {
            uint32_t r = std::stoi(background.substr(1, 2), nullptr, 16);
            uint32_t g = std::stoi(background.substr(3, 2), nullptr, 16);
            uint32_t b = std::stoi(background.substr(5, 2), nullptr, 16);
            layer.color = (r << 24) | (g << 16) | (b << 8) | 0xFF;
        }
    }

    // Check for url()
    size_t urlStart = background.find("url(");
    if (urlStart != std::string::npos) {
        size_t urlEnd = background.find(')', urlStart);
        if (urlEnd != std::string::npos) {
            layer.image = background.substr(urlStart, urlEnd - urlStart + 1);
        }
    }

    // Check for gradient
    if (background.find("linear-gradient") != std::string::npos ||
        background.find("radial-gradient") != std::string::npos ||
        background.find("conic-gradient") != std::string::npos) {
        layer.image = background;
    }

    layers.push_back(layer);
    return layers;
}

void BackgroundCompositor::parsePosition(const std::string& value, float& x, float& y) {
    x = 0; y = 0;
    if (value.empty()) return;
    if (value == "center") { x = 0.5f; y = 0.5f; return; }
    if (value == "top") { x = 0.5f; y = 0; return; }
    if (value == "bottom") { x = 0.5f; y = 1; return; }
    if (value == "left") { x = 0; y = 0.5f; return; }
    if (value == "right") { x = 1; y = 0.5f; return; }

    std::istringstream ss(value);
    std::string h, v;
    ss >> h >> v;

    if (h.back() == '%') x = std::stof(h.substr(0, h.size()-1)) / 100.0f;
    else if (h == "center") x = 0.5f;
    else if (h == "left") x = 0;
    else if (h == "right") x = 1;

    if (!v.empty()) {
        if (v.back() == '%') y = std::stof(v.substr(0, v.size()-1)) / 100.0f;
        else if (v == "center") y = 0.5f;
        else if (v == "top") y = 0;
        else if (v == "bottom") y = 1;
    }
}

void BackgroundCompositor::parseSize(const std::string& value, BackgroundLayer& layer) {
    if (value == "cover") layer.sizeMode = BackgroundLayer::SizeMode::Cover;
    else if (value == "contain") layer.sizeMode = BackgroundLayer::SizeMode::Contain;
    else if (value == "auto") layer.sizeMode = BackgroundLayer::SizeMode::Auto;
    else {
        layer.sizeMode = BackgroundLayer::SizeMode::Explicit;
        std::istringstream ss(value);
        std::string w, h;
        ss >> w >> h;
        try {
            layer.sizeWidth = std::stof(w);
            if (!h.empty()) layer.sizeHeight = std::stof(h);
            else layer.sizeHeight = layer.sizeWidth;
        } catch (...) {}
    }
}

BackgroundCompositor::BackgroundLayer::Repeat BackgroundCompositor::parseRepeat(const std::string& value) {
    if (value == "repeat-x") return BackgroundLayer::Repeat::RepeatX;
    if (value == "repeat-y") return BackgroundLayer::Repeat::RepeatY;
    if (value == "no-repeat") return BackgroundLayer::Repeat::NoRepeat;
    if (value == "space") return BackgroundLayer::Repeat::Space;
    if (value == "round") return BackgroundLayer::Repeat::Round;
    return BackgroundLayer::Repeat::RepeatBoth;
}

BackgroundCompositor::BackgroundRect BackgroundCompositor::computeLayout(
    const BackgroundLayer& layer, const BoxNode* node,
    float imageW, float imageH) {

    BackgroundRect br;
    if (!node) return br;

    const auto& lb = node->layoutBox();
    const auto& cv = node->computed();

    // Origin box
    switch (layer.origin) {
        case BackgroundLayer::Box::BorderBox:
            br.originX = lb.x; br.originY = lb.y;
            br.originW = lb.width; br.originH = lb.height;
            break;
        case BackgroundLayer::Box::PaddingBox:
            br.originX = lb.x + cv.borderLeftWidth;
            br.originY = lb.y + cv.borderTopWidth;
            br.originW = lb.width - cv.borderLeftWidth - cv.borderRightWidth;
            br.originH = lb.height - cv.borderTopWidth - cv.borderBottomWidth;
            break;
        case BackgroundLayer::Box::ContentBox:
            br.originX = lb.contentX; br.originY = lb.contentY;
            br.originW = lb.contentWidth; br.originH = lb.contentHeight;
            break;
    }

    // Clip box
    switch (layer.clip) {
        case BackgroundLayer::Box::BorderBox:
            br.clipX = lb.x; br.clipY = lb.y;
            br.clipW = lb.width; br.clipH = lb.height;
            break;
        case BackgroundLayer::Box::PaddingBox:
            br.clipX = lb.x + cv.borderLeftWidth;
            br.clipY = lb.y + cv.borderTopWidth;
            br.clipW = lb.width - cv.borderLeftWidth - cv.borderRightWidth;
            br.clipH = lb.height - cv.borderTopWidth - cv.borderBottomWidth;
            break;
        case BackgroundLayer::Box::ContentBox:
            br.clipX = lb.contentX; br.clipY = lb.contentY;
            br.clipW = lb.contentWidth; br.clipH = lb.contentHeight;
            break;
    }

    // Tile size
    switch (layer.sizeMode) {
        case BackgroundLayer::SizeMode::Auto:
            br.tileW = imageW; br.tileH = imageH;
            break;
        case BackgroundLayer::SizeMode::Cover: {
            float scale = std::max(br.originW / imageW, br.originH / imageH);
            br.tileW = imageW * scale; br.tileH = imageH * scale;
            break;
        }
        case BackgroundLayer::SizeMode::Contain: {
            float scale = std::min(br.originW / imageW, br.originH / imageH);
            br.tileW = imageW * scale; br.tileH = imageH * scale;
            break;
        }
        case BackgroundLayer::SizeMode::Explicit:
            br.tileW = layer.sizeWidth; br.tileH = layer.sizeHeight;
            break;
    }

    // Position
    br.posX = br.originX + (br.originW - br.tileW) * layer.positionX;
    br.posY = br.originY + (br.originH - br.tileH) * layer.positionY;
    br.repeat = layer.repeat;

    return br;
}

// ==================================================================
// Outline
// ==================================================================

Outline Outline::parse(const ComputedValues& cv) {
    Outline o;
    o.width = cv.outlineWidth;
    o.color = cv.outlineColor;
    o.offset = cv.outlineOffset;
    o.style = cv.outlineStyle;
    return o;
}

// ==================================================================
// CursorManager
// ==================================================================

CursorManager::CursorType CursorManager::parseCursor(const std::string& value) {
    static const std::unordered_map<std::string, CursorType> map = {
        {"auto", CursorType::Auto}, {"default", CursorType::Default},
        {"none", CursorType::None}, {"context-menu", CursorType::ContextMenu},
        {"help", CursorType::Help}, {"pointer", CursorType::Pointer},
        {"progress", CursorType::Progress}, {"wait", CursorType::Wait},
        {"cell", CursorType::Cell}, {"crosshair", CursorType::Crosshair},
        {"text", CursorType::Text}, {"vertical-text", CursorType::VerticalText},
        {"alias", CursorType::Alias}, {"copy", CursorType::Copy},
        {"move", CursorType::Move}, {"no-drop", CursorType::NoDrop},
        {"not-allowed", CursorType::NotAllowed}, {"grab", CursorType::Grab},
        {"grabbing", CursorType::Grabbing},
        {"e-resize", CursorType::EResize}, {"n-resize", CursorType::NResize},
        {"ne-resize", CursorType::NEResize}, {"nw-resize", CursorType::NWResize},
        {"s-resize", CursorType::SResize}, {"se-resize", CursorType::SEResize},
        {"sw-resize", CursorType::SWResize}, {"w-resize", CursorType::WResize},
        {"ew-resize", CursorType::EWResize}, {"ns-resize", CursorType::NSResize},
        {"nesw-resize", CursorType::NESWResize}, {"nwse-resize", CursorType::NWSEResize},
        {"col-resize", CursorType::ColResize}, {"row-resize", CursorType::RowResize},
        {"all-scroll", CursorType::AllScroll},
        {"zoom-in", CursorType::ZoomIn}, {"zoom-out", CursorType::ZoomOut},
    };

    auto it = map.find(value);
    return (it != map.end()) ? it->second : CursorType::Auto;
}

CursorManager::CursorType CursorManager::cursorForNode(const BoxNode* node) {
    if (!node) return CursorType::Default;

    // Explicit cursor property
    if (!node->computed().cursor.empty() && node->computed().cursor != "auto") {
        return parseCursor(node->computed().cursor);
    }

    // Implicit cursor from element type
    const std::string& tag = node->tag();
    if (tag == "a") return CursorType::Pointer;
    if (tag == "button") return CursorType::Pointer;
    if (tag == "input" || tag == "textarea") return CursorType::Text;
    if (tag == "select") return CursorType::Pointer;

    return CursorType::Default;
}

} // namespace Web
} // namespace NXRender
