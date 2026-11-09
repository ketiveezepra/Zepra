// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file css_supports.cpp
 * @brief CSS.supports() and CSS.escape() (see css_supports.hpp)
 */

#include "css/css_supports.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <unordered_set>
#include <vector>

namespace Zepra::WebCore {

namespace {

// The properties StyleResolver::applyProperty() resolves (keep in step with it)
const std::unordered_set<std::string>& knownProperties() {
    static const std::unordered_set<std::string> names = {
        "align-content", "align-items", "align-self", "animation", "appearance", "aspect-ratio",
        "backdrop-filter", "background", "background-color", "background-image", "background-position",
        "background-repeat", "background-size", "border", "border-bottom", "border-bottom-color",
        "border-bottom-left-radius", "border-bottom-right-radius", "border-bottom-width",
        "border-collapse", "border-color", "border-left", "border-left-color", "border-left-width",
        "border-radius", "border-right", "border-right-color", "border-right-width", "border-spacing",
        "border-top", "border-top-color", "border-top-left-radius", "border-top-right-radius",
        "border-top-width", "border-width", "bottom", "box-shadow", "box-sizing", "color", "column-gap",
        "content", "cursor", "display", "filter", "flex", "flex-basis", "flex-direction", "flex-grow",
        "flex-shrink", "flex-wrap", "float", "font-family", "font-size", "font-style", "font-weight",
        "gap", "grid", "grid-area", "grid-auto-columns", "grid-auto-flow", "grid-auto-rows",
        "grid-column", "grid-column-end", "grid-column-gap", "grid-column-start", "grid-gap", "grid-row",
        "grid-row-end", "grid-row-gap", "grid-row-start", "grid-template", "grid-template-areas",
        "grid-template-columns", "grid-template-rows", "height", "isolation",
        "justify-content", "justify-items", "justify-self", "left", "letter-spacing", "line-height", "margin", "margin-bottom",
        "margin-left", "margin-right", "margin-top", "max-height", "max-width", "min-height",
        "min-width", "mix-blend-mode", "object-fit", "object-position", "opacity", "order", "outline", "outline-color",
        "outline-offset", "outline-style", "outline-width", "overflow", "overflow-x", "overflow-y",
        "padding", "padding-bottom", "padding-left", "padding-right", "padding-top", "place-content",
        "place-items", "place-self", "pointer-events", "position", "right", "row-gap", "table-layout", "text-align",
        "text-decoration", "text-decoration-line", "text-overflow", "text-transform", "top",
        "transform", "transition", "user-select", "vertical-align", "visibility",
        "-webkit-appearance", "-webkit-backdrop-filter", "-webkit-user-select", "white-space",
        "width", "will-change", "word-spacing", "z-index",
    };
    return names;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\f"), b = s.find_last_not_of(" \t\r\n\f");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// A declaration value: not empty, brackets balanced, no top-level ';' or
// '!' (an !important is not part of the value), strings closed
bool validValue(const std::string& value) {
    if (trim(value).empty()) return false;
    std::vector<char> open;
    char quote = 0;
    for (size_t i = 0; i < value.size(); i++) {
        char c = value[i];
        if (quote) {
            if (c == '\\') i++;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '(' || c == '[' || c == '{') open.push_back(c == '(' ? ')' : c == '[' ? ']' : '}');
        else if (c == ')' || c == ']' || c == '}') {
            if (open.empty() || open.back() != c) return false;
            open.pop_back();
        } else if ((c == ';' || c == '!') && open.empty()) {
            return false;
        }
    }
    return open.empty() && !quote;
}

// The condition grammar of CSS Conditional 3 §6.1, over the raw text
class ConditionParser {
public:
    explicit ConditionParser(const std::string& text) : s_(text) {}

    // <supports-condition>, and nothing after it
    bool parseAll(bool& result) {
        if (!condition(result)) return false;
        skipSpace();
        return pos_ == s_.size();
    }

private:
    const std::string& s_;
    size_t pos_ = 0;

    void skipSpace() {
        while (pos_ < s_.size() && std::isspace((unsigned char)s_[pos_])) pos_++;
    }

    // A keyword followed by whitespace or '(' ("not", "and", "or")
    bool keyword(const char* word) {
        skipSpace();
        size_t n = std::char_traits<char>::length(word);
        if (s_.size() - pos_ < n || lower(s_.substr(pos_, n)) != word) return false;
        size_t after = pos_ + n;
        if (after < s_.size() && !std::isspace((unsigned char)s_[after]) && s_[after] != '(') return false;
        pos_ = after;
        return true;
    }

    bool condition(bool& result) {
        if (keyword("not")) {
            bool inner = false;
            if (!inParens(inner)) return false;
            result = !inner;
            return true;
        }
        if (!inParens(result)) return false;
        // One kind of combinator throughout: "a and b or c" is invalid
        const char* combinator = nullptr;
        for (;;) {
            size_t save = pos_;
            const char* next = keyword("and") ? "and" : keyword("or") ? "or" : nullptr;
            if (!next) { pos_ = save; return true; }
            if (combinator && std::string(combinator) != next) return false;
            combinator = next;
            bool rhs = false;
            if (!inParens(rhs)) return false;
            result = std::string(next) == "and" ? (result && rhs) : (result || rhs);
        }
    }

    // The text up to the ')' closing the '(' just consumed
    bool block(std::string& out) {
        int depth = 1;
        char quote = 0;
        size_t start = pos_;
        for (; pos_ < s_.size(); pos_++) {
            char c = s_[pos_];
            if (quote) {
                if (c == '\\') pos_++;
                else if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '(') {
                depth++;
            } else if (c == ')' && --depth == 0) {
                out = s_.substr(start, pos_ - start);
                pos_++;
                return true;
            }
        }
        return false;
    }

    // <supports-in-parens>: ( <condition> ), ( <declaration> ), selector(),
    // or anything else balanced (<general-enclosed>, which is false)
    bool inParens(bool& result) {
        skipSpace();
        if (s_.size() - pos_ >= 9 && lower(s_.substr(pos_, 9)) == "selector(") {
            pos_ += 9;
            std::string text;
            if (!block(text)) return false;
            // Any non-empty selector (the selector parser accepts what it can
            // match and has no validity check of its own yet)
            result = !trim(text).empty();
            return true;
        }
        // Some other function: general-enclosed
        size_t name = pos_;
        while (pos_ < s_.size() && (std::isalnum((unsigned char)s_[pos_]) || s_[pos_] == '-')) pos_++;
        if (pos_ > name) {
            if (pos_ >= s_.size() || s_[pos_] != '(') return false;
            pos_++;
            std::string ignored;
            if (!block(ignored)) return false;
            result = false;
            return true;
        }
        if (pos_ >= s_.size() || s_[pos_] != '(') return false;
        pos_++;
        std::string inner;
        if (!block(inner)) return false;
        // A nested condition, else a declaration, else general-enclosed
        ConditionParser nested(inner);
        if (nested.parseAll(result)) return true;
        size_t colon = inner.find(':');
        if (colon != std::string::npos) {
            result = cssSupportsDeclaration(trim(inner.substr(0, colon)), inner.substr(colon + 1));
            return true;
        }
        result = false;
        return true;
    }
};

} // namespace

bool cssSupportsDeclaration(const std::string& property, const std::string& value) {
    std::string name = trim(property);
    if (name.empty() || !validValue(value)) return false;
    // Custom properties take any value (CSS Variables 1 §2)
    if (name.rfind("--", 0) == 0) return name.size() > 2;
    return knownProperties().count(lower(name)) > 0;
}

bool cssSupportsCondition(const std::string& conditionText) {
    bool result = false;
    ConditionParser parser(conditionText);
    if (parser.parseAll(result)) return result;
    // CSS.supports("display: grid"): a bare declaration is read as if
    // it were wrapped in parentheses (CSS Conditional 3 §8.1)
    std::string wrapped = "(" + conditionText + ")";
    ConditionParser retry(wrapped);
    return retry.parseAll(result) && result;
}

std::string cssEscapeIdentifier(const std::string& ident) {
    // CSSOM §2.1 "serialize an identifier"
    std::string out;
    for (size_t i = 0; i < ident.size(); i++) {
        unsigned char c = (unsigned char)ident[i];
        char buf[16];
        if (c == 0) {
            out += "\xEF\xBF\xBD";  // U+FFFD
        } else if ((c >= 0x01 && c <= 0x1F) || c == 0x7F || (i == 0 && c >= '0' && c <= '9') ||
                   (i == 1 && c >= '0' && c <= '9' && ident[0] == '-')) {
            std::snprintf(buf, sizeof buf, "\\%x ", c);
            out += buf;
        } else if (i == 0 && c == '-' && ident.size() == 1) {
            out += "\\-";
        } else if (c >= 0x80 || c == '-' || c == '_' || std::isalnum(c)) {
            out += (char)c;
        } else {
            out += '\\';
            out += (char)c;
        }
    }
    return out;
}

} // namespace Zepra::WebCore
