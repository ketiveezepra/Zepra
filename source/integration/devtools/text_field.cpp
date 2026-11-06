// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file text_field.cpp
 * @brief The DevTools text field's model: caret, selection and editing keys
 *        over UTF-8 text.
 */
#include "text_field.hpp"

#include <algorithm>
#include <cstdint>

using NXRender::KeyCode;

namespace ZepraBrowser::DevTools {

namespace {

const char kReplacement[] = "\xEF\xBF\xBD";   // U+FFFD

bool isContinuation(unsigned char b) { return (b & 0xC0) == 0x80; }

/// Length of the valid UTF-8 sequence at `i`, or 0 when it is invalid; `valid` receives the
/// number of leading bytes that do form the start of a sequence (so one U+FFFD replaces them)
size_t sequenceAt(const std::string& s, size_t i, size_t& valid) {
    const size_t n = s.size();
    unsigned char b = (unsigned char)s[i];
    valid = 1;
    if (b < 0x80) return 1;
    size_t len;
    unsigned char lo = 0x80, hi = 0xBF;   // allowed range of the second byte
    if (b >= 0xC2 && b <= 0xDF) {
        len = 2;
    } else if (b >= 0xE0 && b <= 0xEF) {
        len = 3;
        if (b == 0xE0) lo = 0xA0;          // overlong
        if (b == 0xED) hi = 0x9F;          // UTF-16 surrogates
    } else if (b >= 0xF0 && b <= 0xF4) {
        len = 4;
        if (b == 0xF0) lo = 0x90;          // overlong
        if (b == 0xF4) hi = 0x8F;          // above U+10FFFF
    } else {
        return 0;                          // stray continuation byte, C0/C1, F5..FF
    }
    for (size_t k = 1; k < len; ++k) {
        if (i + k >= n) return 0;
        unsigned char c = (unsigned char)s[i + k];
        if (k == 1 ? (c < lo || c > hi) : !isContinuation(c)) return 0;
        valid = k + 1;
    }
    return len;
}

/// The text with every invalid UTF-8 sequence replaced by U+FFFD
std::string toValidUtf8(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        size_t valid = 1;
        size_t len = sequenceAt(in, i, valid);
        if (len) {
            out.append(in, i, len);
            i += len;
        } else {
            out += kReplacement;
            i += std::max<size_t>(valid, 1);
        }
    }
    return out;
}

/// The code point that starts at `i` (the text is valid UTF-8)
uint32_t codePointAt(const std::string& s, size_t i) {
    unsigned char b = (unsigned char)s[i];
    if (b < 0x80) return b;
    size_t extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : 1;
    uint32_t cp = b & (0x3Fu >> extra);
    for (size_t k = 1; k <= extra && i + k < s.size(); ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3Fu);
    return cp;
}

/// Letters, digits and '_' are words; so is non-ASCII text, except its spaces and punctuation
bool isWordChar(uint32_t cp) {
    if (cp < 0x80) {
        return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_';
    }
    if (cp == 0xA0 || cp == 0x3000) return false;                 // no-break / ideographic space
    if (cp >= 0x2000 && cp <= 0x206F) return false;               // general punctuation, spaces
    if (cp >= 0x3001 && cp <= 0x3003) return false;               // 、。〃
    if (cp >= 0xFF01 && cp <= 0xFF0F) return false;               // fullwidth punctuation
    if (cp == 0xFFFD) return false;
    return true;
}

} // namespace

TextField::TextField(std::string text, bool multiline) : multiline_(multiline) {
    setText(std::move(text), true);
}

void TextField::setText(std::string text, bool selectAll) {
    text_ = toValidUtf8(text);
    caret_ = text_.size();
    anchor_ = selectAll ? 0 : caret_;
    preferredColumn_ = -1;
}

void TextField::setMultiline(bool multiline) {
    multiline_ = multiline;
    preferredColumn_ = -1;
}

std::string TextField::selectedText() const {
    return text_.substr(selectionStart(), selectionEnd() - selectionStart());
}

// ---- positions ------------------------------------------------------------

size_t TextField::snap(size_t pos) const {
    pos = std::min(pos, text_.size());
    while (pos > 0 && pos < text_.size() && isContinuation((unsigned char)text_[pos])) --pos;
    return pos;
}

size_t TextField::prevBoundary(size_t pos) const {
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 && isContinuation((unsigned char)text_[pos])) --pos;
    return pos;
}

size_t TextField::nextBoundary(size_t pos) const {
    if (pos >= text_.size()) return text_.size();
    ++pos;
    while (pos < text_.size() && isContinuation((unsigned char)text_[pos])) ++pos;
    return pos;
}

size_t TextField::wordLeft(size_t pos) const {
    // Skip the separators before the caret, then the word before them
    while (pos > 0 && !isWordChar(codePointAt(text_, prevBoundary(pos)))) pos = prevBoundary(pos);
    while (pos > 0 && isWordChar(codePointAt(text_, prevBoundary(pos)))) pos = prevBoundary(pos);
    return pos;
}

size_t TextField::wordRight(size_t pos) const {
    // Skip the separators after the caret, then the word after them (the caret lands at its end)
    while (pos < text_.size() && !isWordChar(codePointAt(text_, pos))) pos = nextBoundary(pos);
    while (pos < text_.size() && isWordChar(codePointAt(text_, pos))) pos = nextBoundary(pos);
    return pos;
}

size_t TextField::lineStartOf(size_t pos) const {
    if (!multiline_ || pos == 0) return 0;
    size_t nl = text_.rfind('\n', pos - 1);
    return nl == std::string::npos ? 0 : nl + 1;
}

size_t TextField::lineEndOf(size_t pos) const {
    if (!multiline_) return text_.size();
    size_t nl = text_.find('\n', pos);
    return nl == std::string::npos ? text_.size() : nl;
}

// ---- lines ----------------------------------------------------------------

std::vector<std::string> TextField::lines() const {
    if (!multiline_) return {text_};
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t nl = text_.find('\n', start);
        if (nl == std::string::npos) {
            out.push_back(text_.substr(start));
            return out;
        }
        out.push_back(text_.substr(start, nl - start));
        start = nl + 1;
    }
}

size_t TextField::lineCount() const {
    if (!multiline_) return 1;
    return (size_t)std::count(text_.begin(), text_.end(), '\n') + 1;
}

size_t TextField::lineStart(size_t line) const {
    if (!multiline_) return 0;
    size_t start = 0;
    for (size_t l = 0; l < line; ++l) {
        size_t nl = text_.find('\n', start);
        if (nl == std::string::npos) break;   // past the last line: its start
        start = nl + 1;
    }
    return start;
}

size_t TextField::caretLine() const {
    if (!multiline_) return 0;
    return (size_t)std::count(text_.begin(), text_.begin() + (std::ptrdiff_t)caret_, '\n');
}

size_t TextField::caretColumn() const {
    return caret_ - lineStartOf(caret_);
}

// ---- caret and selection --------------------------------------------------

void TextField::moveTo(size_t pos, bool extend) {
    caret_ = snap(pos);
    if (!extend) anchor_ = caret_;
    preferredColumn_ = -1;
}

void TextField::moveCaretTo(size_t byteOffset, bool extendSelection) {
    moveTo(byteOffset, extendSelection);
}

void TextField::selectAll() {
    anchor_ = 0;
    caret_ = text_.size();
    preferredColumn_ = -1;
}

void TextField::selectWordAt(size_t byteOffset) {
    if (text_.empty()) {
        moveTo(0, false);
        return;
    }
    size_t pos = snap(byteOffset);
    // The run of the same kind (word or not) as the character after the offset,
    // or before it at the end of the text
    size_t probe = pos < text_.size() ? pos : prevBoundary(pos);
    bool word = isWordChar(codePointAt(text_, probe));
    size_t start = probe, end = probe;
    while (start > 0 && isWordChar(codePointAt(text_, prevBoundary(start))) == word) start = prevBoundary(start);
    while (end < text_.size() && isWordChar(codePointAt(text_, end)) == word) end = nextBoundary(end);
    anchor_ = start;
    caret_ = end;
    preferredColumn_ = -1;
}

void TextField::moveVertically(int direction, bool extend) {
    size_t start = lineStartOf(caret_);
    long column = preferredColumn_;
    if (column < 0) {
        column = 0;
        for (size_t p = start; p < caret_; p = nextBoundary(p)) ++column;
    }
    size_t targetStart;
    if (direction < 0) {
        if (start == 0) {          // first line: to the start of the text
            moveTo(0, extend);
            return;
        }
        targetStart = lineStartOf(start - 1);
    } else {
        size_t end = lineEndOf(caret_);
        if (end >= text_.size()) {   // last line: to the end of the text
            moveTo(text_.size(), extend);
            return;
        }
        targetStart = end + 1;
    }
    // Same column on the target line, or its end when it is shorter
    size_t targetEnd = lineEndOf(targetStart);
    size_t pos = targetStart;
    for (long c = 0; c < column && pos < targetEnd; ++c) pos = nextBoundary(pos);
    caret_ = pos;
    if (!extend) anchor_ = caret_;
    preferredColumn_ = column;
}

// ---- editing --------------------------------------------------------------

void TextField::erase(size_t from, size_t to) {
    if (to > from) text_.erase(from, to - from);
    caret_ = anchor_ = from;
    preferredColumn_ = -1;
}

bool TextField::insertText(const std::string& s) {
    std::string valid = toValidUtf8(s);
    std::string clean;
    clean.reserve(valid.size());
    for (size_t i = 0; i < valid.size(); ++i) {
        unsigned char c = (unsigned char)valid[i];
        if (c == '\r') {
            if (i + 1 < valid.size() && valid[i + 1] == '\n') continue;   // "\r\n": the "\n" follows
            c = '\n';
        }
        if (c == '\n' || c == '\t') {
            clean += multiline_ ? (char)c : ' ';
        } else if (c < 0x20 || c == 0x7F) {
            continue;                                                    // other control characters
        } else if (c == 0xC2 && i + 1 < valid.size() &&
                   (unsigned char)valid[i + 1] >= 0x80 && (unsigned char)valid[i + 1] <= 0x9F) {
            ++i;                                                         // C1 control characters
        } else {
            clean += (char)c;
        }
    }
    if (clean.empty()) return false;
    size_t from = selectionStart();
    text_.replace(from, selectionEnd() - from, clean);
    caret_ = anchor_ = from + clean.size();
    preferredColumn_ = -1;
    return true;
}

void TextField::insert(const std::string& s) {
    insertText(s);
}

TextField::Result TextField::onKey(KeyCode key, const std::string& text, bool ctrl, bool shift) {
    switch (key) {
    case KeyCode::Escape:
        return Result::Cancel;
    case KeyCode::Tab:
        if (ctrl) return Result::Ignored;   // Ctrl+Tab belongs to the browser
        return shift ? Result::Previous : Result::Next;
    case KeyCode::Enter:
        if (!multiline_ || ctrl) return Result::Commit;
        return insertText("\n") ? Result::Changed : Result::Moved;

    case KeyCode::Left:
        if (hasSelection() && !shift) {
            moveTo(ctrl ? wordLeft(selectionStart()) : selectionStart(), false);
        } else {
            moveTo(ctrl ? wordLeft(caret_) : prevBoundary(caret_), shift);
        }
        return Result::Moved;
    case KeyCode::Right:
        if (hasSelection() && !shift) {
            moveTo(ctrl ? wordRight(selectionEnd()) : selectionEnd(), false);
        } else {
            moveTo(ctrl ? wordRight(caret_) : nextBoundary(caret_), shift);
        }
        return Result::Moved;
    case KeyCode::Home:
        moveTo(ctrl ? 0 : lineStartOf(caret_), shift);
        return Result::Moved;
    case KeyCode::End:
        moveTo(ctrl ? text_.size() : lineEndOf(caret_), shift);
        return Result::Moved;
    case KeyCode::Up:
    case KeyCode::Down:
        if (!multiline_) return Result::Ignored;   // the panel may step a CSS number, walk history...
        moveVertically(key == KeyCode::Up ? -1 : 1, shift);
        return Result::Moved;

    case KeyCode::Backspace:
        if (hasSelection()) {
            erase(selectionStart(), selectionEnd());
            return Result::Changed;
        }
        if (caret_ == 0) return Result::Moved;
        erase(ctrl ? wordLeft(caret_) : prevBoundary(caret_), caret_);
        return Result::Changed;
    case KeyCode::Delete:
        if (shift && !ctrl) {                    // Shift+Delete: cut
            lastCut_ = selectedText();
            erase(selectionStart(), selectionEnd());
            return Result::Cut;
        }
        if (hasSelection()) {
            erase(selectionStart(), selectionEnd());
            return Result::Changed;
        }
        if (caret_ >= text_.size()) return Result::Moved;
        erase(caret_, ctrl ? wordRight(caret_) : nextBoundary(caret_));
        return Result::Changed;
    case KeyCode::Insert:
        if (ctrl && !shift) return Result::Copy;
        if (shift && !ctrl) return Result::Paste;
        return Result::Ignored;
    default:
        break;
    }

    if (ctrl) {
        switch (key) {
        case KeyCode::A:
            selectAll();
            return Result::Moved;
        case KeyCode::C:
            return Result::Copy;
        case KeyCode::X:
            lastCut_ = selectedText();
            erase(selectionStart(), selectionEnd());
            return Result::Cut;
        case KeyCode::V:
            return Result::Paste;
        default:
            return Result::Ignored;                 // other shortcuts are the panel's or the browser's
        }
    }

    if (!text.empty()) return insertText(text) ? Result::Changed : Result::Ignored;
    if (key == KeyCode::Space) return insertText(" ") ? Result::Changed : Result::Ignored;
    return Result::Ignored;
}

} // namespace ZepraBrowser::DevTools
