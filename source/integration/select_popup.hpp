// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file select_popup.hpp
 * @brief The list a drop-down <select> opens: its options (under their
 *        <optgroup> labels), the highlighted one, where it sits beside the
 *        select and scrolling when it is long. The browser feeds it the
 *        pointer and keys while it is open and picks the option it names
 *        through ScriptContext::chooseOption.
 */

#pragma once

#include "devtools/devtools_panel.hpp"

#include <string>
#include <vector>

namespace Zepra::WebCore { class DOMElement; }

namespace ZepraBrowser {

class SelectPopup {
public:
    struct Item {
        std::string label;
        long option = -1;       ///< index in select.options; -1 for a group label
        bool disabled = false;
    };
    /// What a key did while the list is open
    enum class KeyAction { None, Moved, Pick, Close };

    /// Opens the list for `select` under its box (x, y, width, height on
    /// screen), or above it when there is no room below `viewportBottom`.
    /// `textWidth` measures labels (the list is at least as wide as the box).
    void open(Zepra::WebCore::DOMElement* select, float x, float y, float width, float height,
              float viewportTop, float viewportBottom, float fontSize,
              const std::function<float(const std::string&, float)>& textWidth);
    /// Moves the list with its select (the page scrolled or was laid out again)
    void place(float x, float y, float width, float height, float viewportTop, float viewportBottom);
    void close() { select_ = nullptr; items_.clear(); }
    bool isOpen() const { return select_ != nullptr; }
    Zepra::WebCore::DOMElement* select() const { return select_; }

    const std::vector<Item>& items() const { return items_; }
    /// The highlighted item (-1: none) and the option it stands for
    int highlighted() const { return highlighted_; }
    long highlightedOption() const;

    bool contains(float x, float y) const;
    /// The item under the point, -1 outside the list or on a group label
    int itemAt(float x, float y) const;
    /// The pointer moved: the enabled option under it is highlighted
    void hover(float x, float y);
    /// Scrolls a long list by wheel notches (positive: down)
    void wheel(float delta);
    /// ArrowUp/Down, Home/End, PageUp/Down move the highlight; Enter and
    /// Space pick it; Escape and Tab close the list
    KeyAction key(const std::string& domKey);

    void paint(const DevTools::Painter& p) const;

    // Geometry, for the browser and tests
    float x() const { return x_; }
    float y() const { return y_; }
    float width() const { return w_; }
    float height() const { return visibleRows() * itemHeight_ + 2; }
    float itemHeight() const { return itemHeight_; }
    int firstVisible() const { return first_; }
    int visibleRows() const;

private:
    void moveHighlight(int from, int direction, int steps);
    void keepHighlightVisible();

    Zepra::WebCore::DOMElement* select_ = nullptr;
    std::vector<Item> items_;
    int highlighted_ = -1;
    int first_ = 0;
    float x_ = 0, y_ = 0, w_ = 0;
    float contentWidth_ = 0;
    float itemHeight_ = 22;
    float fontSize_ = 14;
    int maxRows_ = 12;
};

}  // namespace ZepraBrowser
