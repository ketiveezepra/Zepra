// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file select_popup.cpp
 * @brief The drop-down <select>'s option list (see select_popup.hpp)
 */

#include "select_popup.hpp"

#include "browser/dom.hpp"
#include "dom/form_controls.hpp"

#include <algorithm>
#include <cmath>

namespace ZepraBrowser {

using Zepra::WebCore::DOMElement;

namespace {
constexpr float kPadX = 10;
constexpr uint32_t kBackground = 0xFFFFFF;
constexpr uint32_t kBorder = 0xB8B8C0;
constexpr uint32_t kHighlight = 0x2F6FDB;
constexpr uint32_t kText = 0x1D2330;
constexpr uint32_t kDisabledText = 0xA0A4AC;
constexpr uint32_t kGroupText = 0x5A6070;
}  // namespace

void SelectPopup::open(DOMElement* select, float x, float y, float width, float height, float viewportTop,
                       float viewportBottom, float fontSize,
                       const std::function<float(const std::string&, float)>& textWidth) {
    close();
    if (!select) return;
    select_ = select;
    fontSize_ = fontSize > 0 ? fontSize : 14;
    itemHeight_ = std::round(fontSize_ + 9);
    // Options in order, each group's label before its options
    std::vector<DOMElement*> options = Zepra::WebCore::selectOptions(select);
    const DOMElement* group = nullptr;
    for (size_t i = 0; i < options.size(); i++) {
        DOMElement* option = options[i];
        auto* parent = dynamic_cast<DOMElement*>(option->parentNode());
        const DOMElement* optionGroup = parent && parent != select ? parent : nullptr;
        if (optionGroup && optionGroup != group) {
            items_.push_back({optionGroup->getAttribute("label"), -1, optionGroup->hasAttribute("disabled")});
        }
        group = optionGroup;
        std::string label = Zepra::WebCore::optionLabel(option);
        if (optionGroup) label = "    " + label;
        items_.push_back({label, static_cast<long>(i), Zepra::WebCore::isOptionDisabled(option)});
    }
    contentWidth_ = 0;
    for (const Item& item : items_) {
        float w = textWidth ? textWidth(item.label, fontSize_) : item.label.size() * fontSize_ * 0.6f;
        contentWidth_ = std::max(contentWidth_, w + 2 * kPadX + 12);
    }
    long selected = Zepra::WebCore::selectedIndex(select);
    highlighted_ = -1;
    for (size_t i = 0; i < items_.size(); i++) {
        if (items_[i].option == selected && selected >= 0) highlighted_ = static_cast<int>(i);
    }
    place(x, y, width, height, viewportTop, viewportBottom);
    keepHighlightVisible();
}

void SelectPopup::place(float x, float y, float width, float height, float viewportTop, float viewportBottom) {
    float rowsBelow = std::floor((viewportBottom - (y + height) - 4) / itemHeight_);
    float rowsAbove = std::floor((y - viewportTop - 4) / itemHeight_);
    int wanted = std::min(static_cast<int>(items_.size()), 12);
    bool below = rowsBelow >= wanted || rowsBelow >= rowsAbove;
    maxRows_ = std::max(1, std::min(12, static_cast<int>(below ? rowsBelow : rowsAbove)));
    x_ = x;
    w_ = std::max(width, contentWidth_);
    y_ = below ? y + height : y - (visibleRows() * itemHeight_ + 2);
    first_ = std::clamp(first_, 0, std::max(0, static_cast<int>(items_.size()) - visibleRows()));
}

int SelectPopup::visibleRows() const {
    return std::max(1, std::min(static_cast<int>(items_.size()), maxRows_));
}

long SelectPopup::highlightedOption() const {
    if (highlighted_ < 0 || highlighted_ >= static_cast<int>(items_.size())) return -1;
    return items_[highlighted_].option;
}

bool SelectPopup::contains(float x, float y) const {
    return isOpen() && x >= x_ && x < x_ + w_ && y >= y_ && y < y_ + height();
}

int SelectPopup::itemAt(float x, float y) const {
    if (!contains(x, y)) return -1;
    int row = static_cast<int>(std::floor((y - y_ - 1) / itemHeight_));
    int index = first_ + std::clamp(row, 0, visibleRows() - 1);
    if (index < 0 || index >= static_cast<int>(items_.size()) || items_[index].option < 0) return -1;
    return index;
}

void SelectPopup::hover(float x, float y) {
    int index = itemAt(x, y);
    if (index >= 0 && !items_[index].disabled) highlighted_ = index;
}

void SelectPopup::wheel(float delta) {
    int maxFirst = std::max(0, static_cast<int>(items_.size()) - visibleRows());
    first_ = std::clamp(first_ + (delta > 0 ? 3 : -3), 0, maxFirst);
}

void SelectPopup::moveHighlight(int from, int direction, int steps) {
    int last = -1;
    for (int i = from + direction; i >= 0 && i < static_cast<int>(items_.size()) && steps > 0; i += direction) {
        if (items_[i].option < 0 || items_[i].disabled) continue;
        last = i;
        steps--;
    }
    if (last >= 0) highlighted_ = last;
    keepHighlightVisible();
}

void SelectPopup::keepHighlightVisible() {
    if (highlighted_ < 0) return;
    if (highlighted_ < first_) first_ = highlighted_;
    if (highlighted_ >= first_ + visibleRows()) first_ = highlighted_ - visibleRows() + 1;
}

SelectPopup::KeyAction SelectPopup::key(const std::string& k) {
    int count = static_cast<int>(items_.size());
    int page = std::max(1, visibleRows() - 1);
    if (k == "ArrowDown") moveHighlight(highlighted_, 1, 1);
    else if (k == "ArrowUp") moveHighlight(highlighted_ < 0 ? count : highlighted_, -1, 1);
    else if (k == "Home") moveHighlight(-1, 1, 1);
    else if (k == "End") moveHighlight(count, -1, 1);
    else if (k == "PageDown") moveHighlight(highlighted_, 1, page);
    else if (k == "PageUp") moveHighlight(highlighted_ < 0 ? count : highlighted_, -1, page);
    else if (k == "Enter" || k == " ") return KeyAction::Pick;
    else if (k == "Escape" || k == "Tab") return KeyAction::Close;
    else return KeyAction::None;
    return KeyAction::Moved;
}

void SelectPopup::paint(const DevTools::Painter& p) const {
    if (!isOpen()) return;
    float h = height();
    // A soft shadow, the frame, then the rows
    p.rect(x_ + 2, y_ + 3, w_, h, 0x000000, 40);
    p.rect(x_, y_, w_, h, kBorder, 255);
    p.rect(x_ + 1, y_ + 1, w_ - 2, h - 2, kBackground, 255);
    if (p.pushClip) p.pushClip(x_ + 1, y_ + 1, w_ - 2, h - 2);
    for (int row = 0; row < visibleRows(); row++) {
        int index = first_ + row;
        if (index >= static_cast<int>(items_.size())) break;
        const Item& item = items_[index];
        float top = y_ + 1 + row * itemHeight_;
        bool lit = index == highlighted_;
        if (lit) p.rect(x_ + 1, top, w_ - 2, itemHeight_, kHighlight, 255);
        uint32_t color = lit ? 0xFFFFFF : item.option < 0 ? kGroupText : item.disabled ? kDisabledText : kText;
        float baseline = top + (itemHeight_ + fontSize_ * 0.7f) / 2;
        p.text(item.label, x_ + kPadX, baseline, color, fontSize_);
    }
    if (p.popClip) p.popClip();
    // A scroll thumb when the list is longer than the box
    int count = static_cast<int>(items_.size());
    if (count > visibleRows()) {
        float track = h - 4;
        float thumb = std::max(16.0f, track * visibleRows() / count);
        float top = y_ + 2 + (track - thumb) * first_ / std::max(1, count - visibleRows());
        p.rect(x_ + w_ - 6, top, 4, thumb, 0x9098A8, 200);
    }
}

}  // namespace ZepraBrowser
