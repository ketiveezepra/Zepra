// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file text_field.hpp
 * @brief The text being edited in a DevTools field (attribute name or value,
 *        text node, CSS name or value, search box, "Edit as HTML"): its caret,
 *        its selection and what each key does to them. Pure logic: the panel
 *        draws the field and owns the clipboard.
 *
 * Offsets are byte offsets into the UTF-8 text and always sit on a code point
 * boundary. The text is always valid UTF-8: invalid bytes handed to the field
 * become U+FFFD.
 */
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "input/events.h"

namespace ZepraBrowser::DevTools {

class TextField {
public:
    /// What a key did, for the panel to act on
    enum class Result {
        None,       ///< the key is not the field's: the panel may use it (Up/Down in a single-line field, Ctrl+B...)
        Changed,    ///< the text changed
        Commit,     ///< Enter (Ctrl+Enter in a multiline field): apply the text and stop editing
        Cancel,     ///< Escape: drop the edit
        Next,       ///< Tab: commit and edit the next field
        Previous,   ///< Shift+Tab: commit and edit the previous field
        Copy,       ///< Ctrl+C / Ctrl+Insert: put selectedText() on the clipboard (nothing to do when it is empty)
        Cut,        ///< Ctrl+X / Shift+Delete: the selection is already removed; put lastCut() on the
                    ///< clipboard (nothing to do when it is empty). Changed is implied when it isn't.
        Paste,      ///< Ctrl+V / Shift+Insert: call insert() with the clipboard text
        Moved,      ///< the key was used but the text is unchanged (caret or selection moved, or nothing to do)
    };

    TextField() = default;
    /// Same as setText(text) with everything selected
    explicit TextField(std::string text, bool multiline = false);

    /// Replaces the text (kept as given, except invalid UTF-8 becomes U+FFFD); selects all of
    /// it, or puts the caret at the end
    void setText(std::string text, bool selectAll = true);
    const std::string& text() const { return text_; }
    bool multiline() const { return multiline_; }
    /// Switches between single-line and multiline (the text and caret stay)
    void setMultiline(bool multiline);

    size_t caret() const { return caret_; }
    /// The selection's first and last byte; equal (to the caret) when nothing is selected
    size_t selectionStart() const { return anchor_ < caret_ ? anchor_ : caret_; }
    size_t selectionEnd() const { return anchor_ < caret_ ? caret_ : anchor_; }
    bool hasSelection() const { return anchor_ != caret_; }
    std::string selectedText() const;
    /// The text the last Cut removed (empty when nothing was selected): what the panel puts on
    /// the clipboard after Result::Cut. Each Cut replaces it.
    const std::string& lastCut() const { return lastCut_; }

    /// Typed or pasted text: replaces the selection, caret after it. Invalid UTF-8 becomes
    /// U+FFFD, control characters other than tab and newline are dropped, "\r\n" and "\r"
    /// become "\n"; in a single-line field newlines and tabs become spaces.
    void insert(const std::string& s);

    /// A key while the field has focus. `text` is the typed character(s) for printable
    /// keys (may be empty); it is ignored while Ctrl is held.
    Result onKey(NXRender::KeyCode key, const std::string& text, bool ctrl, bool shift);

    /// Puts the caret at an offset (clamped, and snapped back to a code point boundary);
    /// with extendSelection the selection's other end stays where it was
    void moveCaretTo(size_t byteOffset, bool extendSelection = false);
    void selectAll();
    /// Selects the word at an offset (double click); between words, the run of other characters
    void selectWordAt(size_t byteOffset);

    /// The text split at "\n" in a multiline field; a single-line field is one line (its whole
    /// text, even if setText() gave it newlines)
    std::vector<std::string> lines() const;
    size_t lineCount() const;
    /// Byte offset where a line starts (clamped to the last line)
    size_t lineStart(size_t line) const;
    /// The caret's line, and its column in bytes within that line
    size_t caretLine() const;
    size_t caretColumn() const;

private:
    size_t prevBoundary(size_t pos) const;
    size_t nextBoundary(size_t pos) const;
    size_t wordLeft(size_t pos) const;
    size_t wordRight(size_t pos) const;
    size_t lineStartOf(size_t pos) const;
    size_t lineEndOf(size_t pos) const;
    size_t snap(size_t pos) const;
    /// Moves the caret, keeping the anchor when extending and forgetting the Up/Down column
    void moveTo(size_t pos, bool extend);
    void moveVertically(int direction, bool extend);
    /// Removes [from, to) and puts the caret there
    void erase(size_t from, size_t to);
    /// insert(), telling whether the text changed
    bool insertText(const std::string& s);

    std::string text_;
    std::string lastCut_;
    size_t caret_ = 0;
    size_t anchor_ = 0;          ///< the selection's fixed end (== caret_ when nothing is selected)
    long preferredColumn_ = -1;  ///< code points; kept across Up/Down so a short line doesn't lose it
    bool multiline_ = false;
};

} // namespace ZepraBrowser::DevTools
