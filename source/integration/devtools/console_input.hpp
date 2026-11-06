// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file console_input.hpp
 * @brief The DevTools Console's input and output filter, apart from drawing:
 *        a multi-line editor with a caret, the check that tells an unfinished
 *        expression from one ready to run, what autocomplete should look up at
 *        the caret and how candidates are ranked, and which messages the text
 *        and level filter lets through.
 */
#pragma once

#include <cstddef>
#include <memory>
#include <regex>
#include <string>
#include <vector>

namespace Zepra::WebCore { struct ConsoleMessage; }

namespace ZepraBrowser::DevTools {

/// Multi-line text with a caret. Offsets are bytes into UTF-8 text; the caret
/// never sits inside a code point.
class ConsoleEditor {
public:
    const std::string& text() const { return text_; }
    size_t caret() const { return caret_; }
    bool empty() const { return text_.empty(); }

    /// Replaces the text; the caret goes to its end
    void setText(std::string text);
    void clear() { text_.clear(); caret_ = 0; }
    void setCaret(size_t offset);

    /// Inserts at the caret (a paste may hold newlines; "\r\n" becomes "\n")
    void insert(const std::string& s);
    /// Replaces [from, caret) with `s` (an accepted completion)
    void replaceBeforeCaret(size_t from, const std::string& s);
    void backspace();
    void deleteForward();

    void left();
    void right();
    void home();   ///< start of the caret's line
    void end();    ///< end of the caret's line
    /// One line up / down, keeping the column. False on the first / last line
    /// (the console then walks its history).
    bool up();
    bool down();

    size_t lineCount() const;
    size_t caretLine() const;
    /// The caret's line up to the caret (for placing it when drawing)
    std::string caretLinePrefix() const;
    std::vector<std::string> lines() const;
    /// Moves the caret to `line` at the byte column closest to `column`
    void moveTo(size_t line, size_t column);

private:
    std::string text_;
    size_t caret_ = 0;
    size_t lineStart(size_t offset) const;
    size_t lineEnd(size_t offset) const;
};

/// True while `code` is unfinished: an open bracket, string, template or block
/// comment. Enter then adds a line instead of running it, as in Firefox.
bool isIncompleteInput(const std::string& code);

/// What autocomplete looks up at the caret
struct CompletionQuery {
    bool valid = false;
    std::vector<std::string> path;   ///< the object before the last '.' ("document", "body"); empty: globals
    std::string prefix;              ///< the part of the name typed so far
    size_t replaceFrom = 0;          ///< where `prefix` starts (an accepted name replaces it)
};

/// The identifier chain that ends at `caret`: `doc` -> {[], "doc"}, `document.bo` ->
/// {["document"], "bo"}, `a.b.` -> {["a","b"], ""}. Not valid inside a string or
/// comment, after a number, or after a call or index (`f().x`, `a[0].x`), which
/// could only be answered by running code.
CompletionQuery completionQuery(const std::string& text, size_t caret);

/// The names that complete `prefix`: those starting with it as typed, then those
/// matching it ignoring case, each group in alphabetical order, at most `max`.
std::vector<std::string> rankCompletions(const std::vector<std::string>& names, const std::string& prefix,
                                         size_t max = 200);

/// The Console's output filter: a text and the levels shown
struct ConsoleFilter {
    /// As typed: a substring (ignoring case), "-text" to hide what matches,
    /// "/regex/" (or "/regex/i") for a regular expression
    std::string text;
    bool errors = true, warnings = true, logs = true, info = true, debug = true;
};

/// The level button a message belongs to: "error", "warn", "log", "info", "debug",
/// or "" for console input and results (always shown)
std::string filterLevel(const Zepra::WebCore::ConsoleMessage& message);

/// A ConsoleFilter made ready to test messages (its regular expression compiled once)
class ConsoleFilterMatcher {
public:
    explicit ConsoleFilterMatcher(const ConsoleFilter& filter = {});
    /// Whether the message's level is shown and its text (or source) matches
    bool passes(const Zepra::WebCore::ConsoleMessage& message) const;
    /// Whether a text is set (the level buttons aside)
    bool hasText() const { return !needle_.empty() || regex_ != nullptr; }

private:
    ConsoleFilter filter_;
    bool exclude_ = false;
    std::string needle_;                   // lower case
    std::shared_ptr<std::regex> regex_;
};

/// "https://a.org/js/app.js?v=2" -> "app.js": the name a source link shows
std::string sourceDisplayName(const std::string& url);

} // namespace ZepraBrowser::DevTools
