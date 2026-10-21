// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "forms.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <regex>

namespace NXRender {
namespace Web {

// ==================================================================
// TextInputState
// ==================================================================

std::string TextInputState::selectedText() const {
    if (!hasSelection()) return "";
    int s = std::min(selectionStart, selectionEnd);
    int e = std::max(selectionStart, selectionEnd);
    if (s < 0) s = 0;
    if (e > static_cast<int>(text.size())) e = static_cast<int>(text.size());
    return text.substr(s, e - s);
}

void TextInputState::insertText(const std::string& t) {
    if (hasSelection()) deleteSelection();
    int pos = std::clamp(caretPosition, 0, static_cast<int>(text.size()));
    text.insert(pos, t);
    caretPosition = pos + static_cast<int>(t.size());
    selectionStart = selectionEnd = caretPosition;
}

void TextInputState::deleteBackward() {
    if (hasSelection()) { deleteSelection(); return; }
    if (caretPosition <= 0) return;
    text.erase(caretPosition - 1, 1);
    caretPosition--;
    selectionStart = selectionEnd = caretPosition;
}

void TextInputState::deleteForward() {
    if (hasSelection()) { deleteSelection(); return; }
    if (caretPosition >= static_cast<int>(text.size())) return;
    text.erase(caretPosition, 1);
    selectionStart = selectionEnd = caretPosition;
}

void TextInputState::moveCaret(int delta, bool extend) {
    int newPos = std::clamp(caretPosition + delta, 0, static_cast<int>(text.size()));
    caretPosition = newPos;
    if (extend) {
        selectionEnd = newPos;
    } else {
        selectionStart = selectionEnd = newPos;
    }
}

void TextInputState::moveToStart(bool extend) {
    caretPosition = 0;
    if (extend) {
        selectionEnd = 0;
    } else {
        selectionStart = selectionEnd = 0;
    }
}

void TextInputState::moveToEnd(bool extend) {
    caretPosition = static_cast<int>(text.size());
    if (extend) {
        selectionEnd = caretPosition;
    } else {
        selectionStart = selectionEnd = caretPosition;
    }
}

void TextInputState::selectAll() {
    selectionStart = 0;
    selectionEnd = static_cast<int>(text.size());
    caretPosition = selectionEnd;
}

void TextInputState::deleteSelection() {
    if (!hasSelection()) return;
    int s = std::min(selectionStart, selectionEnd);
    int e = std::max(selectionStart, selectionEnd);
    text.erase(s, e - s);
    caretPosition = s;
    selectionStart = selectionEnd = s;
}

void TextInputState::moveWordLeft(bool extend) {
    int pos = caretPosition;
    // Skip whitespace backwards
    while (pos > 0 && std::isspace(static_cast<unsigned char>(text[pos - 1]))) pos--;
    // Skip word backwards
    while (pos > 0 && !std::isspace(static_cast<unsigned char>(text[pos - 1]))) pos--;
    caretPosition = pos;
    if (extend) selectionEnd = pos;
    else selectionStart = selectionEnd = pos;
}

void TextInputState::moveWordRight(bool extend) {
    int len = static_cast<int>(text.size());
    int pos = caretPosition;
    // Skip word forward
    while (pos < len && !std::isspace(static_cast<unsigned char>(text[pos]))) pos++;
    // Skip whitespace forward
    while (pos < len && std::isspace(static_cast<unsigned char>(text[pos]))) pos++;
    caretPosition = pos;
    if (extend) selectionEnd = pos;
    else selectionStart = selectionEnd = pos;
}

// ==================================================================
// FormControlRenderer
// ==================================================================

FormControlRenderer::FormControlRenderer() {}
FormControlRenderer::~FormControlRenderer() {}

ComputedValues FormControlRenderer::baseInputStyle(const ComputedValues& cv) {
    ComputedValues style = cv;
    if (style.backgroundColor == 0)
        style.backgroundColor = theme_.controlBg;
    if (style.borderTopWidth == 0) {
        style.borderTopWidth = style.borderRightWidth = 1;
        style.borderBottomWidth = style.borderLeftWidth = 1;
        style.borderTopColor = style.borderRightColor = theme_.controlBorder;
        style.borderBottomColor = style.borderLeftColor = theme_.controlBorder;
    }
    if (style.paddingTop == 0 && style.paddingBottom == 0) {
        style.paddingTop = style.paddingBottom = 4;
        style.paddingLeft = style.paddingRight = 8;
    }
    if (style.borderTopLeftRadius == 0) {
        style.borderTopLeftRadius = style.borderTopRightRadius = 3;
        style.borderBottomLeftRadius = style.borderBottomRightRadius = 3;
    }
    return style;
}

ComputedValues FormControlRenderer::focusRingStyle(const ComputedValues& cv) {
    ComputedValues style = cv;
    style.borderTopColor = style.borderRightColor = theme_.controlFocusBorder;
    style.borderBottomColor = style.borderLeftColor = theme_.controlFocusBorder;
    style.boxShadow = "0 0 0 2px rgba(0,120,215,0.25)";
    return style;
}

ComputedValues FormControlRenderer::disabledStyle(const ComputedValues& cv) {
    ComputedValues style = cv;
    style.backgroundColor = theme_.controlDisabledBg;
    style.color = theme_.controlDisabledText;
    style.opacity = 0.6f;
    return style;
}

// Render <input type="text|email|password|search|url|tel|number">
std::unique_ptr<BoxNode> FormControlRenderer::renderInput(
    const FormControlState& state,
    const ComputedValues& cv,
    const TextInputState* textState) {

    auto box = std::make_unique<BoxNode>();
    box->setTag("input");
    box->setBoxType(BoxType::InlineBlock);

    ComputedValues style = baseInputStyle(cv);
    if (state.disabled) style = disabledStyle(style);

    // Default size: 20ch wide
    if (style.widthAuto) {
        style.width = style.fontSize * 12;
        style.widthAuto = false;
    }
    if (style.heightAuto) {
        style.height = style.fontSize * 1.5f + style.paddingTop + style.paddingBottom +
                        style.borderTopWidth + style.borderBottomWidth;
        style.heightAuto = false;
    }

    box->setComputedValues(style);

    // Content: show text or placeholder
    std::string displayText;
    if (textState && !textState->text.empty()) {
        if (state.type == "password") {
            // One bullet (U+2022, 3 bytes in UTF-8) per character of the value
            for (unsigned char c : textState->text) {
                if ((c & 0xC0) != 0x80) displayText += "\xE2\x80\xA2";
            }
        } else {
            displayText = textState->text;
        }
    } else if (!state.placeholder.empty()) {
        displayText = state.placeholder;
        // Placeholder color
        auto textBox = BoxTreeBuilder::createTextBox(displayText, ComputedValues());
        textBox->computed().color = theme_.placeholderText;
        textBox->computed().fontSize = style.fontSize;
        box->appendChild(std::move(textBox));
        return box;
    }

    if (!displayText.empty()) {
        auto textBox = BoxTreeBuilder::createTextBox(displayText, ComputedValues());
        textBox->computed().color = style.color;
        textBox->computed().fontSize = style.fontSize;
        box->appendChild(std::move(textBox));
    }

    return box;
}

// Render <textarea>
std::unique_ptr<BoxNode> FormControlRenderer::renderTextarea(
    const FormControlState& state,
    const ComputedValues& cv,
    const TextInputState* textState) {

    auto box = std::make_unique<BoxNode>();
    box->setTag("textarea");
    box->setBoxType(BoxType::Block);

    ComputedValues style = baseInputStyle(cv);
    if (state.disabled) style = disabledStyle(style);

    // Default size: 20 cols x 2 rows
    if (style.widthAuto) {
        style.width = style.fontSize * 12;
        style.widthAuto = false;
    }
    if (style.heightAuto) {
        style.height = style.fontSize * style.lineHeight * 3 +
                        style.paddingTop + style.paddingBottom;
        style.heightAuto = false;
    }

    style.overflowY = 2; // scroll
    style.whiteSpace = "pre-wrap";

    box->setComputedValues(style);

    std::string displayText = textState ? textState->text : state.value;
    if (!displayText.empty()) {
        auto textBox = BoxTreeBuilder::createTextBox(displayText, ComputedValues());
        textBox->computed().color = style.color;
        textBox->computed().fontSize = style.fontSize;
        textBox->computed().whiteSpace = "pre-wrap";
        box->appendChild(std::move(textBox));
    } else if (!state.placeholder.empty()) {
        auto textBox = BoxTreeBuilder::createTextBox(state.placeholder, ComputedValues());
        textBox->computed().color = theme_.placeholderText;
        textBox->computed().fontSize = style.fontSize;
        box->appendChild(std::move(textBox));
    }

    return box;
}

// Render <select>
std::unique_ptr<BoxNode> FormControlRenderer::renderSelect(
    const FormControlState& state,
    const ComputedValues& cv,
    const std::vector<SelectOption>& options,
    bool open) {

    auto box = std::make_unique<BoxNode>();
    box->setTag("select");
    box->setBoxType(BoxType::InlineBlock);

    ComputedValues style = baseInputStyle(cv);
    if (state.disabled) style = disabledStyle(style);
    style.paddingRight += 20; // Space for dropdown arrow

    if (style.widthAuto) {
        style.width = style.fontSize * 10;
        style.widthAuto = false;
    }
    if (style.heightAuto) {
        style.height = style.fontSize * 1.5f + style.paddingTop + style.paddingBottom +
                        style.borderTopWidth + style.borderBottomWidth;
        style.heightAuto = false;
    }

    box->setComputedValues(style);

    // Selected option text
    std::string selectedLabel;
    for (const auto& opt : options) {
        if (opt.selected) { selectedLabel = opt.label; break; }
    }
    if (selectedLabel.empty() && !options.empty()) {
        selectedLabel = options[0].label;
    }

    if (!selectedLabel.empty()) {
        auto textBox = BoxTreeBuilder::createTextBox(selectedLabel, ComputedValues());
        textBox->computed().color = style.color;
        textBox->computed().fontSize = style.fontSize;
        box->appendChild(std::move(textBox));
    }

    // Dropdown arrow (▼ as separate text node, positioned right)
    auto arrow = BoxTreeBuilder::createTextBox("\u25BC", ComputedValues());
    arrow->computed().color = theme_.selectArrow;
    arrow->computed().fontSize = style.fontSize * 0.6f;
    box->appendChild(std::move(arrow));

    // Dropdown list (if open)
    if (open) {
        auto dropdown = std::make_unique<BoxNode>();
        dropdown->setTag("select-dropdown");
        dropdown->setBoxType(BoxType::Block);

        ComputedValues ddStyle;
        ddStyle.display = 1;
        ddStyle.position = 2; // absolute
        ddStyle.backgroundColor = 0xFFFFFFFF;
        ddStyle.borderTopWidth = ddStyle.borderRightWidth = 1;
        ddStyle.borderBottomWidth = ddStyle.borderLeftWidth = 1;
        ddStyle.borderTopColor = ddStyle.borderRightColor = theme_.controlBorder;
        ddStyle.borderBottomColor = ddStyle.borderLeftColor = theme_.controlBorder;
        ddStyle.zIndex = 9999;
        ddStyle.overflowY = 3; // auto
        ddStyle.maxHeight = 200;
        dropdown->setComputedValues(ddStyle);

        for (const auto& opt : options) {
            auto optBox = std::make_unique<BoxNode>();
            optBox->setTag("option");
            optBox->setBoxType(BoxType::Block);

            ComputedValues optStyle;
            optStyle.display = 1;
            optStyle.paddingTop = optStyle.paddingBottom = 4;
            optStyle.paddingLeft = optStyle.paddingRight = 8;
            optStyle.fontSize = style.fontSize;
            if (opt.selected) {
                optStyle.backgroundColor = theme_.controlFocusBorder;
                optStyle.color = 0xFFFFFFFF;
            }
            if (opt.disabled) {
                optStyle.color = theme_.controlDisabledText;
            }
            optBox->setComputedValues(optStyle);

            auto label = BoxTreeBuilder::createTextBox(opt.label, ComputedValues());
            label->computed().fontSize = style.fontSize;
            optBox->appendChild(std::move(label));
            dropdown->appendChild(std::move(optBox));
        }

        box->appendChild(std::move(dropdown));
    }

    return box;
}

// Render <button>
std::unique_ptr<BoxNode> FormControlRenderer::renderButton(
    const FormControlState& state,
    const ComputedValues& cv,
    const std::string& label) {

    auto box = std::make_unique<BoxNode>();
    box->setTag("button");
    box->setBoxType(BoxType::InlineBlock);

    ComputedValues style = cv;
    if (style.backgroundColor == 0) style.backgroundColor = theme_.buttonBg;
    if (style.borderTopWidth == 0) {
        style.borderTopWidth = style.borderRightWidth = 1;
        style.borderBottomWidth = style.borderLeftWidth = 1;
        style.borderTopColor = style.borderRightColor = theme_.controlBorder;
        style.borderBottomColor = style.borderLeftColor = theme_.controlBorder;
    }
    if (style.paddingTop == 0) {
        style.paddingTop = style.paddingBottom = 6;
        style.paddingLeft = style.paddingRight = 16;
    }
    style.borderTopLeftRadius = style.borderTopRightRadius = 3;
    style.borderBottomLeftRadius = style.borderBottomRightRadius = 3;
    style.cursor = "pointer";

    if (state.disabled) style = disabledStyle(style);

    box->setComputedValues(style);

    if (!label.empty()) {
        auto textBox = BoxTreeBuilder::createTextBox(label, ComputedValues());
        textBox->computed().color = style.color;
        textBox->computed().fontSize = style.fontSize;
        box->appendChild(std::move(textBox));
    }

    return box;
}

// Render <input type="checkbox">
std::unique_ptr<BoxNode> FormControlRenderer::renderCheckbox(
    const FormControlState& state,
    const ComputedValues& cv) {

    auto box = std::make_unique<BoxNode>();
    box->setTag("input-checkbox");
    box->setBoxType(BoxType::InlineBlock);

    ComputedValues style;
    style.display = 3; // inline-block
    style.width = 16; style.widthAuto = false;
    style.height = 16; style.heightAuto = false;
    style.borderTopWidth = style.borderRightWidth = 1;
    style.borderBottomWidth = style.borderLeftWidth = 1;
    style.borderTopColor = style.borderRightColor = theme_.controlBorder;
    style.borderBottomColor = style.borderLeftColor = theme_.controlBorder;
    style.borderTopLeftRadius = style.borderTopRightRadius = 2;
    style.borderBottomLeftRadius = style.borderBottomRightRadius = 2;
    style.cursor = "pointer";

    if (state.checked) {
        style.backgroundColor = theme_.checkboxCheck;
        style.borderTopColor = style.borderRightColor = theme_.checkboxCheck;
        style.borderBottomColor = style.borderLeftColor = theme_.checkboxCheck;
    } else {
        style.backgroundColor = theme_.controlBg;
    }

    if (state.disabled) style = disabledStyle(style);
    box->setComputedValues(style);

    // Checkmark
    if (state.checked) {
        auto check = BoxTreeBuilder::createTextBox("\u2713", ComputedValues());
        check->computed().color = 0xFFFFFFFF;
        check->computed().fontSize = 12;
        box->appendChild(std::move(check));
    } else if (state.indeterminate) {
        auto dash = BoxTreeBuilder::createTextBox("\u2014", ComputedValues());
        dash->computed().color = theme_.checkboxCheck;
        dash->computed().fontSize = 10;
        box->appendChild(std::move(dash));
    }

    return box;
}

// Render <input type="radio">
std::unique_ptr<BoxNode> FormControlRenderer::renderRadio(
    const FormControlState& state,
    const ComputedValues& cv) {

    auto box = std::make_unique<BoxNode>();
    box->setTag("input-radio");
    box->setBoxType(BoxType::InlineBlock);

    ComputedValues style;
    style.display = 3;
    style.width = 16; style.widthAuto = false;
    style.height = 16; style.heightAuto = false;
    style.borderTopWidth = style.borderRightWidth = 1;
    style.borderBottomWidth = style.borderLeftWidth = 1;
    style.borderTopColor = style.borderRightColor = theme_.controlBorder;
    style.borderBottomColor = style.borderLeftColor = theme_.controlBorder;
    style.borderTopLeftRadius = style.borderTopRightRadius = 8;
    style.borderBottomLeftRadius = style.borderBottomRightRadius = 8;
    style.cursor = "pointer";

    if (state.checked) {
        style.borderTopColor = style.borderRightColor = theme_.checkboxCheck;
        style.borderBottomColor = style.borderLeftColor = theme_.checkboxCheck;
        style.borderTopWidth = style.borderRightWidth = 2;
        style.borderBottomWidth = style.borderLeftWidth = 2;
    }
    style.backgroundColor = theme_.controlBg;

    if (state.disabled) style = disabledStyle(style);
    box->setComputedValues(style);

    // Inner dot for checked state
    if (state.checked) {
        auto dot = std::make_unique<BoxNode>();
        dot->setTag("radio-dot");
        dot->setBoxType(BoxType::Block);
        ComputedValues dotStyle;
        dotStyle.display = 1;
        dotStyle.width = 8; dotStyle.widthAuto = false;
        dotStyle.height = 8; dotStyle.heightAuto = false;
        dotStyle.backgroundColor = theme_.checkboxCheck;
        dotStyle.borderTopLeftRadius = dotStyle.borderTopRightRadius = 4;
        dotStyle.borderBottomLeftRadius = dotStyle.borderBottomRightRadius = 4;
        dotStyle.marginTop = dotStyle.marginLeft = 2;
        dot->setComputedValues(dotStyle);
        box->appendChild(std::move(dot));
    }

    return box;
}

// Render <input type="range">
std::unique_ptr<BoxNode> FormControlRenderer::renderRange(
    const FormControlState& state,
    const ComputedValues& cv,
    const RangeState& range) {

    auto box = std::make_unique<BoxNode>();
    box->setTag("input-range");
    box->setBoxType(BoxType::InlineBlock);

    ComputedValues style;
    style.display = 3;
    if (cv.widthAuto) { style.width = 160; style.widthAuto = false; }
    else { style.width = cv.width; style.widthAuto = false; }
    style.height = 20; style.heightAuto = false;
    style.cursor = "pointer";
    if (state.disabled) style = disabledStyle(style);
    box->setComputedValues(style);

    float fraction = (range.max > range.min) ?
        (range.value - range.min) / (range.max - range.min) : 0;
    fraction = std::clamp(fraction, 0.0f, 1.0f);

    // Track
    auto track = std::make_unique<BoxNode>();
    track->setTag("range-track");
    track->setBoxType(BoxType::Block);
    ComputedValues trackStyle;
    trackStyle.display = 1;
    trackStyle.width = style.width; trackStyle.widthAuto = false;
    trackStyle.height = 4; trackStyle.heightAuto = false;
    trackStyle.backgroundColor = theme_.rangeTrack;
    trackStyle.borderTopLeftRadius = trackStyle.borderTopRightRadius = 2;
    trackStyle.borderBottomLeftRadius = trackStyle.borderBottomRightRadius = 2;
    trackStyle.marginTop = 8;
    track->setComputedValues(trackStyle);

    // Filled portion
    auto filled = std::make_unique<BoxNode>();
    filled->setTag("range-filled");
    filled->setBoxType(BoxType::Block);
    ComputedValues filledStyle;
    filledStyle.display = 1;
    filledStyle.width = style.width * fraction; filledStyle.widthAuto = false;
    filledStyle.height = 4; filledStyle.heightAuto = false;
    filledStyle.backgroundColor = theme_.rangeThumb;
    filledStyle.borderTopLeftRadius = filledStyle.borderTopRightRadius = 2;
    filledStyle.borderBottomLeftRadius = filledStyle.borderBottomRightRadius = 2;
    filled->setComputedValues(filledStyle);
    track->appendChild(std::move(filled));

    box->appendChild(std::move(track));

    // Thumb
    auto thumb = std::make_unique<BoxNode>();
    thumb->setTag("range-thumb");
    thumb->setBoxType(BoxType::Block);
    ComputedValues thumbStyle;
    thumbStyle.display = 1;
    thumbStyle.position = 2; // absolute
    thumbStyle.width = 16; thumbStyle.widthAuto = false;
    thumbStyle.height = 16; thumbStyle.heightAuto = false;
    thumbStyle.backgroundColor = theme_.rangeThumb;
    thumbStyle.borderTopLeftRadius = thumbStyle.borderTopRightRadius = 8;
    thumbStyle.borderBottomLeftRadius = thumbStyle.borderBottomRightRadius = 8;
    thumbStyle.left = style.width * fraction - 8;
    thumbStyle.leftAuto = false;
    thumbStyle.top = 2;
    thumbStyle.topAuto = false;
    thumb->setComputedValues(thumbStyle);
    box->appendChild(std::move(thumb));

    return box;
}

// Render <progress>
std::unique_ptr<BoxNode> FormControlRenderer::renderProgress(
    float value, float max, const ComputedValues& cv) {

    auto box = std::make_unique<BoxNode>();
    box->setTag("progress");
    box->setBoxType(BoxType::InlineBlock);

    ComputedValues style;
    style.display = 3;
    if (cv.widthAuto) { style.width = 160; style.widthAuto = false; }
    else { style.width = cv.width; style.widthAuto = false; }
    style.height = 16; style.heightAuto = false;
    style.backgroundColor = theme_.progressTrack;
    style.borderTopLeftRadius = style.borderTopRightRadius = 8;
    style.borderBottomLeftRadius = style.borderBottomRightRadius = 8;
    style.overflowX = 1; // hidden (clip the bar)
    box->setComputedValues(style);

    float fraction = (max > 0) ? std::clamp(value / max, 0.0f, 1.0f) : 0;

    auto bar = std::make_unique<BoxNode>();
    bar->setTag("progress-bar");
    bar->setBoxType(BoxType::Block);
    ComputedValues barStyle;
    barStyle.display = 1;
    barStyle.width = style.width * fraction; barStyle.widthAuto = false;
    barStyle.height = 16; barStyle.heightAuto = false;
    barStyle.backgroundColor = theme_.progressBar;
    barStyle.borderTopLeftRadius = barStyle.borderTopRightRadius = 8;
    barStyle.borderBottomLeftRadius = barStyle.borderBottomRightRadius = 8;
    bar->setComputedValues(barStyle);
    box->appendChild(std::move(bar));

    return box;
}

// Render <meter>
std::unique_ptr<BoxNode> FormControlRenderer::renderMeter(
    const MeterState& state, const ComputedValues& cv) {

    auto box = std::make_unique<BoxNode>();
    box->setTag("meter");
    box->setBoxType(BoxType::InlineBlock);

    ComputedValues style;
    style.display = 3;
    if (cv.widthAuto) { style.width = 160; style.widthAuto = false; }
    else { style.width = cv.width; style.widthAuto = false; }
    style.height = 16; style.heightAuto = false;
    style.backgroundColor = theme_.progressTrack;
    style.borderTopLeftRadius = style.borderTopRightRadius = 8;
    style.borderBottomLeftRadius = style.borderBottomRightRadius = 8;
    style.overflowX = 1;
    box->setComputedValues(style);

    float range = state.max - state.min;
    float fraction = (range > 0) ? std::clamp((state.value - state.min) / range, 0.0f, 1.0f) : 0;

    // Determine color based on value relative to low/high/optimum
    uint32_t barColor;
    if (state.value < state.low) {
        barColor = 0xEE4400FF; // danger
    } else if (state.value > state.high) {
        barColor = 0xEE4400FF; // danger
    } else if (std::abs(state.value - state.optimum) < (range * 0.2f)) {
        barColor = 0x00AA00FF; // optimal
    } else {
        barColor = 0xDDBB00FF; // suboptimal
    }

    auto bar = std::make_unique<BoxNode>();
    bar->setTag("meter-bar");
    bar->setBoxType(BoxType::Block);
    ComputedValues barStyle;
    barStyle.display = 1;
    barStyle.width = style.width * fraction; barStyle.widthAuto = false;
    barStyle.height = 16; barStyle.heightAuto = false;
    barStyle.backgroundColor = barColor;
    barStyle.borderTopLeftRadius = barStyle.borderTopRightRadius = 8;
    barStyle.borderBottomLeftRadius = barStyle.borderBottomRightRadius = 8;
    bar->setComputedValues(barStyle);
    box->appendChild(std::move(bar));

    return box;
}

// Render <input type="color">
std::unique_ptr<BoxNode> FormControlRenderer::renderColorPicker(
    const std::string& color, const ComputedValues& cv) {

    auto box = std::make_unique<BoxNode>();
    box->setTag("input-color");
    box->setBoxType(BoxType::InlineBlock);

    ComputedValues style = baseInputStyle(cv);
    style.width = 44; style.widthAuto = false;
    style.height = 24; style.heightAuto = false;
    style.paddingTop = style.paddingBottom = 2;
    style.paddingLeft = style.paddingRight = 2;
    style.cursor = "pointer";
    box->setComputedValues(style);

    // Color swatch
    auto swatch = std::make_unique<BoxNode>();
    swatch->setTag("color-swatch");
    swatch->setBoxType(BoxType::Block);
    ComputedValues swatchStyle;
    swatchStyle.display = 1;
    swatchStyle.width = 36; swatchStyle.widthAuto = false;
    swatchStyle.height = 16; swatchStyle.heightAuto = false;
    // Parse hex color
    uint32_t c = 0x000000FF;
    if (color.size() >= 7 && color[0] == '#') {
        unsigned int r = 0, g = 0, b = 0;
        if (sscanf(color.c_str(), "#%02x%02x%02x", &r, &g, &b) == 3) {
            c = (r << 24) | (g << 16) | (b << 8) | 0xFF;
        }
    }
    swatchStyle.backgroundColor = c;
    swatchStyle.borderTopLeftRadius = swatchStyle.borderTopRightRadius = 2;
    swatchStyle.borderBottomLeftRadius = swatchStyle.borderBottomRightRadius = 2;
    swatch->setComputedValues(swatchStyle);
    box->appendChild(std::move(swatch));

    return box;
}

// Render <input type="date">
std::unique_ptr<BoxNode> FormControlRenderer::renderDatePicker(
    const FormControlState& state, const ComputedValues& cv) {

    auto box = std::make_unique<BoxNode>();
    box->setTag("input-date");
    box->setBoxType(BoxType::InlineBlock);

    ComputedValues style = baseInputStyle(cv);
    if (style.widthAuto) {
        style.width = style.fontSize * 10;
        style.widthAuto = false;
    }
    style.paddingRight += 20; // Space for calendar icon
    if (state.disabled) style = disabledStyle(style);
    box->setComputedValues(style);

    std::string displayText = state.value.empty() ? "yyyy-mm-dd" : state.value;
    auto textBox = BoxTreeBuilder::createTextBox(displayText, ComputedValues());
    textBox->computed().color = state.value.empty() ? theme_.placeholderText : style.color;
    textBox->computed().fontSize = style.fontSize;
    box->appendChild(std::move(textBox));

    // Calendar icon
    auto icon = BoxTreeBuilder::createTextBox("\U0001F4C5", ComputedValues());
    icon->computed().fontSize = style.fontSize * 0.8f;
    box->appendChild(std::move(icon));

    return box;
}

// Render <input type="file">
std::unique_ptr<BoxNode> FormControlRenderer::renderFileInput(
    const FormControlState& state,
    const ComputedValues& cv,
    const std::vector<std::string>& files) {

    auto box = std::make_unique<BoxNode>();
    box->setTag("input-file");
    box->setBoxType(BoxType::InlineBlock);

    ComputedValues style = cv;
    style.display = 3;
    if (state.disabled) style = disabledStyle(style);
    box->setComputedValues(style);

    // "Choose File" button
    FormControlState btnState;
    btnState.disabled = state.disabled;
    auto btn = renderButton(btnState, cv, "Choose File");
    box->appendChild(std::move(btn));

    // File name label
    std::string fileLabel = files.empty() ? "No file chosen" :
        (files.size() == 1 ? files[0] : std::to_string(files.size()) + " files");
    auto label = BoxTreeBuilder::createTextBox(fileLabel, ComputedValues());
    label->computed().color = style.color;
    label->computed().fontSize = style.fontSize;
    label->computed().marginLeft = 8;
    box->appendChild(std::move(label));

    return box;
}

// ==================================================================
// FormDataCollector
// ==================================================================

std::vector<FormDataCollector::FormEntry> FormDataCollector::collect(const BoxNode* formRoot) {
    std::vector<FormEntry> entries;
    if (!formRoot) return entries;

    std::function<void(const BoxNode*)> walk = [&](const BoxNode* node) {
        const std::string& tag = node->tag();

        // Standard text inputs and textarea
        if (tag == "input" || tag == "textarea") {
            FormEntry entry;
            // Name is stored via computed values or from the DOM node's name attribute.
            // BoxNode stores a domNode_ pointer; the integration layer resolves
            // the name attribute at DOMBoxBuilder time and stores it in ComputedValues::content
            // as a convention: content = "name=fieldname" for form controls.
            entry.name = extractFormFieldName(node);
            if (!entry.name.empty()) {
                // Value: the text content of the first text child
                for (const auto& child : node->children()) {
                    if (child->isTextNode()) {
                        entry.value = child->text();
                        break;
                    }
                }
                entries.push_back(entry);
            }
        }
        // Checkbox and radio — only submit if checked
        else if (tag == "input-checkbox" || tag == "input-radio") {
            // A checked checkbox/radio has a checkmark/dot child
            bool checked = !node->children().empty();
            if (checked) {
                FormEntry entry;
                entry.name = extractFormFieldName(node);
                if (!entry.name.empty()) {
                    entry.value = "on"; // Default HTML value for checked checkbox
                    entries.push_back(entry);
                }
            }
        }
        // Select — find the selected option text
        else if (tag == "select") {
            FormEntry entry;
            entry.name = extractFormFieldName(node);
            if (!entry.name.empty()) {
                // Walk children to find selected option text
                for (const auto& child : node->children()) {
                    if (child->tag() == "option" || child->tag() == "select-dropdown") {
                        for (const auto& optChild : child->children()) {
                            if (optChild->isTextNode()) {
                                entry.value = optChild->text();
                                break;
                            }
                        }
                        if (!entry.value.empty()) break;
                    }
                    // Inline selected text (not inside dropdown)
                    if (child->isTextNode() && child->text() != "\u25BC") {
                        entry.value = child->text();
                    }
                }
                entries.push_back(entry);
            }
        }

        for (const auto& child : node->children()) {
            walk(child.get());
        }
    };
    walk(formRoot);

    return entries;
}

std::string FormDataCollector::extractFormFieldName(const BoxNode* node) {
    if (!node) return "";

    // Convention: DOMBoxBuilder stores form field name in ComputedValues::content
    // as "name=fieldname" for input/textarea/select elements.
    const std::string& content = node->computed().content;
    if (content.find("name=") == 0) {
        return content.substr(5);
    }

    // Fallback: use the tag itself combined with child index to generate a unique name
    // This ensures form data is still collected even without explicit naming
    if (node->parent()) {
        return node->tag() + "_" + std::to_string(node->childIndex());
    }
    return node->tag();
}

std::string FormDataCollector::urlEncode(const std::vector<FormEntry>& entries) {
    std::string result;
    for (size_t i = 0; i < entries.size(); i++) {
        if (i > 0) result += '&';

        // Percent-encode name and value
        for (char c : entries[i].name) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                result += c;
            } else if (c == ' ') {
                result += '+';
            } else {
                char buf[4];
                snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
                result += buf;
            }
        }
        result += '=';
        for (char c : entries[i].value) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                result += c;
            } else if (c == ' ') {
                result += '+';
            } else {
                char buf[4];
                snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
                result += buf;
            }
        }
    }
    return result;
}

std::string FormDataCollector::multipartEncode(const std::vector<FormEntry>& entries,
                                                   std::string& boundary) {
    boundary = "----NXRenderFormBoundary";
    // Append random suffix
    for (int i = 0; i < 16; i++) boundary += static_cast<char>('a' + (i * 7 + 3) % 26);

    std::string result;
    for (const auto& entry : entries) {
        result += "--" + boundary + "\r\n";
        if (!entry.filename.empty()) {
            result += "Content-Disposition: form-data; name=\"" + entry.name +
                        "\"; filename=\"" + entry.filename + "\"\r\n";
            result += "Content-Type: application/octet-stream\r\n";
        } else {
            result += "Content-Disposition: form-data; name=\"" + entry.name + "\"\r\n";
        }
        result += "\r\n" + entry.value + "\r\n";
    }
    result += "--" + boundary + "--\r\n";
    return result;
}

std::string FormDataCollector::jsonEncode(const std::vector<FormEntry>& entries) {
    std::string result = "{";
    for (size_t i = 0; i < entries.size(); i++) {
        if (i > 0) result += ",";
        result += "\"" + entries[i].name + "\":\"" + entries[i].value + "\"";
    }
    result += "}";
    return result;
}

// ==================================================================
// FormValidator
// ==================================================================

FormValidator::ValidityState FormValidator::validate(const FormControlState& state) {
    ValidityState v;

    // Required check
    if (state.required && state.value.empty()) {
        v.valueMissing = true;
        v.valid = false;
        v.message = "Please fill out this field.";
        return v;
    }

    if (state.value.empty()) return v; // empty optional = valid

    // Type checks
    if (state.type == "email" && !isValidEmail(state.value)) {
        v.typeMismatch = true;
        v.valid = false;
        v.message = "Please enter a valid email address.";
    }
    if (state.type == "url" && !isValidURL(state.value)) {
        v.typeMismatch = true;
        v.valid = false;
        v.message = "Please enter a valid URL.";
    }

    // Pattern
    if (!state.pattern.empty() && !matchesPattern(state.value, state.pattern)) {
        v.patternMismatch = true;
        v.valid = false;
        v.message = "Please match the requested format.";
    }

    // Length
    if (state.maxLength >= 0 && static_cast<int>(state.value.size()) > state.maxLength) {
        v.tooLong = true;
        v.valid = false;
    }
    if (state.minLength >= 0 && static_cast<int>(state.value.size()) < state.minLength) {
        v.tooShort = true;
        v.valid = false;
    }

    return v;
}

bool FormValidator::isValidEmail(const std::string& value) {
    // RFC 5322 simplified: local@domain
    size_t at = value.find('@');
    if (at == std::string::npos || at == 0 || at == value.size() - 1) return false;
    size_t dot = value.find('.', at);
    if (dot == std::string::npos || dot == at + 1 || dot == value.size() - 1) return false;
    return true;
}

bool FormValidator::isValidURL(const std::string& value) {
    // Basic: must start with http:// or https://
    return value.find("http://") == 0 || value.find("https://") == 0;
}

bool FormValidator::matchesPattern(const std::string& value, const std::string& pattern) {
    try {
        std::regex re(pattern);
        return std::regex_match(value, re);
    } catch (...) {
        return true; // invalid pattern = pass
    }
}

bool FormValidator::inRange(float value, float min, float max, float step) {
    if (value < min || value > max) return false;
    if (step > 0) {
        float remainder = std::fmod(value - min, step);
        return std::abs(remainder) < 1e-6f || std::abs(remainder - step) < 1e-6f;
    }
    return true;
}

} // namespace Web
} // namespace NXRender
