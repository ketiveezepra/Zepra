// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file console_input.cpp
 * @brief The Console's editor, unfinished-input check, autocomplete query and
 *        ranking, and output filter.
 */
#include "console_input.hpp"

#include <algorithm>
#include <cctype>

#include "scripting/console_format.hpp"

namespace ZepraBrowser::DevTools {

namespace {

bool isContinuationByte(unsigned char c) { return (c & 0xC0) == 0x80; }

bool isIdentChar(unsigned char c) {
    return std::isalnum(c) || c == '_' || c == '$' || c >= 0x80;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

/// Where a scan of JavaScript stopped: inside a string, a comment or a
/// template's text, and the brackets still open (`{`, `(`, `[`, a template
/// "`" and a template substitution "$")
struct ScanState {
    char quote = 0;            // ' or " while inside a string
    bool escaped = false;      // the previous character was a backslash in a string or template
    bool lineComment = false;
    bool blockComment = false;
    std::vector<char> open;

    bool inTemplateText() const { return !open.empty() && open.back() == '`'; }
    bool inCode() const { return !quote && !lineComment && !blockComment && !inTemplateText(); }
};

ScanState scan(const std::string& code, size_t upto) {
    ScanState st;
    for (size_t i = 0; i < upto && i < code.size(); i++) {
        char c = code[i];
        char next = i + 1 < code.size() ? code[i + 1] : 0;
        if (st.lineComment) {
            if (c == '\n') st.lineComment = false;
        } else if (st.blockComment) {
            if (c == '*' && next == '/') { st.blockComment = false; i++; }
        } else if (st.quote) {
            if (st.escaped) st.escaped = false;
            else if (c == '\\') st.escaped = true;
            else if (c == st.quote || c == '\n') st.quote = 0;   // a newline ends it (a syntax error, not an open string)
        } else if (st.inTemplateText()) {
            if (st.escaped) st.escaped = false;
            else if (c == '\\') st.escaped = true;
            else if (c == '`') st.open.pop_back();
            else if (c == '$' && next == '{') { st.open.push_back('$'); i++; }
        } else if (c == '/' && next == '/') {
            st.lineComment = true;
            i++;
        } else if (c == '/' && next == '*') {
            st.blockComment = true;
            i++;
        } else if (c == '"' || c == '\'') {
            st.quote = c;
        } else if (c == '`') {
            st.open.push_back('`');
        } else if (c == '(' || c == '[' || c == '{') {
            st.open.push_back(c);
        } else if (c == ')' || c == ']' || c == '}') {
            char want = c == ')' ? '(' : c == ']' ? '[' : '{';
            if (!st.open.empty() && (st.open.back() == want || (c == '}' && st.open.back() == '$'))) st.open.pop_back();
            else if (st.open.empty()) {}   // a stray closer: a syntax error the engine reports
            else st.open.clear();          // mismatched: let the engine report it
        }
    }
    return st;
}

} // namespace

// ---------------------------------------------------------------------------
// ConsoleEditor
// ---------------------------------------------------------------------------

void ConsoleEditor::setText(std::string text) {
    text_ = std::move(text);
    caret_ = text_.size();
}

void ConsoleEditor::setCaret(size_t offset) {
    caret_ = std::min(offset, text_.size());
    while (caret_ > 0 && caret_ < text_.size() && isContinuationByte((unsigned char)text_[caret_])) caret_--;
}

void ConsoleEditor::insert(const std::string& s) {
    std::string clean;
    clean.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\r') {
            if (i + 1 < s.size() && s[i + 1] == '\n') continue;
            clean += '\n';
        } else {
            clean += s[i];
        }
    }
    text_.insert(caret_, clean);
    caret_ += clean.size();
}

void ConsoleEditor::replaceBeforeCaret(size_t from, const std::string& s) {
    from = std::min(from, caret_);
    text_.replace(from, caret_ - from, s);
    caret_ = from + s.size();
}

void ConsoleEditor::backspace() {
    if (caret_ == 0) return;
    size_t from = caret_ - 1;
    while (from > 0 && isContinuationByte((unsigned char)text_[from])) from--;
    text_.erase(from, caret_ - from);
    caret_ = from;
}

void ConsoleEditor::deleteForward() {
    if (caret_ >= text_.size()) return;
    size_t to = caret_ + 1;
    while (to < text_.size() && isContinuationByte((unsigned char)text_[to])) to++;
    text_.erase(caret_, to - caret_);
}

void ConsoleEditor::left() {
    if (caret_ == 0) return;
    caret_--;
    while (caret_ > 0 && isContinuationByte((unsigned char)text_[caret_])) caret_--;
}

void ConsoleEditor::right() {
    if (caret_ >= text_.size()) return;
    caret_++;
    while (caret_ < text_.size() && isContinuationByte((unsigned char)text_[caret_])) caret_++;
}

size_t ConsoleEditor::lineStart(size_t offset) const {
    while (offset > 0 && text_[offset - 1] != '\n') offset--;
    return offset;
}

size_t ConsoleEditor::lineEnd(size_t offset) const {
    while (offset < text_.size() && text_[offset] != '\n') offset++;
    return offset;
}

void ConsoleEditor::home() { caret_ = lineStart(caret_); }
void ConsoleEditor::end() { caret_ = lineEnd(caret_); }

bool ConsoleEditor::up() {
    size_t start = lineStart(caret_);
    if (start == 0) return false;
    size_t column = caret_ - start;
    size_t prevStart = lineStart(start - 1);
    setCaret(std::min(prevStart + column, start - 1));
    return true;
}

bool ConsoleEditor::down() {
    size_t end = lineEnd(caret_);
    if (end >= text_.size()) return false;
    size_t column = caret_ - lineStart(caret_);
    size_t nextStart = end + 1;
    setCaret(std::min(nextStart + column, lineEnd(nextStart)));
    return true;
}

size_t ConsoleEditor::lineCount() const {
    return 1 + (size_t)std::count(text_.begin(), text_.end(), '\n');
}

size_t ConsoleEditor::caretLine() const {
    return (size_t)std::count(text_.begin(), text_.begin() + (long)caret_, '\n');
}

std::string ConsoleEditor::caretLinePrefix() const {
    size_t start = lineStart(caret_);
    return text_.substr(start, caret_ - start);
}

std::vector<std::string> ConsoleEditor::lines() const {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t nl = text_.find('\n', start);
        out.push_back(text_.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

void ConsoleEditor::moveTo(size_t line, size_t column) {
    size_t start = 0;
    for (size_t l = 0; l < line; l++) {
        size_t nl = text_.find('\n', start);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    setCaret(std::min(start + column, lineEnd(start)));
}

// ---------------------------------------------------------------------------
// Unfinished input, autocomplete
// ---------------------------------------------------------------------------

bool isIncompleteInput(const std::string& code) {
    ScanState st = scan(code, code.size());
    if (st.blockComment || !st.open.empty()) return true;
    return st.quote && st.escaped;   // a backslash continues the string on the next line
}

CompletionQuery completionQuery(const std::string& text, size_t caret) {
    CompletionQuery q;
    caret = std::min(caret, text.size());
    if (!scan(text, caret).inCode()) return q;
    if (caret < text.size() && isIdentChar((unsigned char)text[caret])) return q;   // inside a word

    size_t i = caret;
    while (i > 0 && isIdentChar((unsigned char)text[i - 1])) i--;
    q.prefix = text.substr(i, caret - i);
    q.replaceFrom = i;
    if (!q.prefix.empty() && std::isdigit((unsigned char)q.prefix[0])) return q;   // a number

    // The chain of names before it, each followed by "." or "?."
    size_t j = i;
    while (j > 0 && text[j - 1] == '.') {
        size_t k = j - 1;
        if (k > 0 && text[k - 1] == '.') {
            // "..." spread (or "x..y"): only the names after it count
            if (k > 1 && text[k - 2] == '.') break;
            return q;
        }
        if (k > 0 && text[k - 1] == '?') k--;
        size_t e = k;
        while (k > 0 && isIdentChar((unsigned char)text[k - 1])) k--;
        if (k == e) return q;                                      // f().x, a[0].x, "s".x
        std::string name = text.substr(k, e - k);
        if (std::isdigit((unsigned char)name[0])) return q;        // 1.5
        q.path.insert(q.path.begin(), name);
        j = k;
    }
    q.valid = true;
    return q;
}

std::vector<std::string> rankCompletions(const std::vector<std::string>& names, const std::string& prefix, size_t max) {
    std::vector<std::string> exact, folded;
    const std::string lp = lower(prefix);
    const bool internal = prefix.rfind("__", 0) == 0;
    for (const std::string& n : names) {
        if (!internal && n.rfind("__", 0) == 0) continue;   // engine internals (__pageUrl__)
        if (n.rfind(prefix, 0) == 0) exact.push_back(n);
        else if (lower(n).rfind(lp, 0) == 0) folded.push_back(n);
    }
    std::sort(exact.begin(), exact.end());
    std::sort(folded.begin(), folded.end());
    exact.erase(std::unique(exact.begin(), exact.end()), exact.end());
    folded.erase(std::unique(folded.begin(), folded.end()), folded.end());
    exact.insert(exact.end(), folded.begin(), folded.end());
    if (exact.size() > max) exact.resize(max);
    return exact;
}

// ---------------------------------------------------------------------------
// Filter
// ---------------------------------------------------------------------------

std::string filterLevel(const Zepra::WebCore::ConsoleMessage& m) {
    if (m.level == "input" || m.level == "result" || m.level == "clear") return "";
    if (m.level == "error" || m.level == "warn" || m.level == "info" || m.level == "debug") return m.level;
    return "log";   // log, trace, group, dir, table, count, time
}

ConsoleFilterMatcher::ConsoleFilterMatcher(const ConsoleFilter& filter) : filter_(filter) {
    std::string t = filter.text;
    if (t.size() > 1 && t[0] == '-') {
        exclude_ = true;
        t = t.substr(1);
    }
    // "/regex/" or "/regex/i"
    if (t.size() > 2 && t[0] == '/') {
        size_t close = t.rfind('/');
        std::string flags = t.substr(close + 1);
        if (close > 0 && (flags.empty() || flags == "i")) {
            try {
                auto options = std::regex::ECMAScript;
                if (flags == "i") options |= std::regex::icase;
                regex_ = std::make_shared<std::regex>(t.substr(1, close - 1), options);
                return;
            } catch (const std::regex_error&) {
                // not a valid expression: match the text as typed
            }
        }
    }
    needle_ = lower(t);
}

bool ConsoleFilterMatcher::passes(const Zepra::WebCore::ConsoleMessage& m) const {
    std::string level = filterLevel(m);
    if ((level == "error" && !filter_.errors) || (level == "warn" && !filter_.warnings) ||
        (level == "log" && !filter_.logs) || (level == "info" && !filter_.info) ||
        (level == "debug" && !filter_.debug)) {
        return false;
    }
    if (!hasText()) return true;
    std::string haystack = m.text;
    if (!m.source.empty()) haystack += "\n" + m.source;
    bool found = regex_ ? std::regex_search(haystack, *regex_) : lower(haystack).find(needle_) != std::string::npos;
    return found != exclude_;
}

std::string sourceDisplayName(const std::string& url) {
    std::string u = url.substr(0, url.find_first_of("?#"));
    while (u.size() > 1 && u.back() == '/') u.pop_back();
    size_t slash = u.rfind('/');
    std::string name = slash == std::string::npos ? u : u.substr(slash + 1);
    return name.empty() ? url : name;
}

} // namespace ZepraBrowser::DevTools
