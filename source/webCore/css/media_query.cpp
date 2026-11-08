// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file media_query.cpp
 * @brief Media query parsing and matching (see media_query.hpp)
 */

#include "css/media_query.hpp"
#include "browser/dom.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <mutex>

namespace Zepra::WebCore {

// ----------------------------------------------------------------------------
// Environment and the registry of live lists
// ----------------------------------------------------------------------------

namespace {

MediaEnvironment s_env;
uint64_t s_matchVersion = 0;
std::mutex s_registryMutex;
std::vector<std::weak_ptr<MediaQueryList>>& registry() {
    static std::vector<std::weak_ptr<MediaQueryList>> lists;
    return lists;
}

}  // namespace

const MediaEnvironment& mediaEnvironment() { return s_env; }
uint64_t mediaMatchVersion() { return s_matchVersion; }

bool setMediaEnvironment(const MediaEnvironment& env) {
    if (env == s_env) return false;
    s_env = env;
    bool changed = false;
    std::vector<std::shared_ptr<MediaQueryList>> live;
    {
        std::lock_guard<std::mutex> lock(s_registryMutex);
        auto& lists = registry();
        lists.erase(std::remove_if(lists.begin(), lists.end(),
                                   [](const std::weak_ptr<MediaQueryList>& w) { return w.expired(); }),
                    lists.end());
        for (auto& w : lists) {
            if (auto list = w.lock()) live.push_back(std::move(list));
        }
    }
    for (auto& list : live) {
        int now = list->matches(env) ? 1 : 0;
        if (list->lastResult_ != now) {
            if (list->lastResult_ != -1) changed = true;
            list->lastResult_ = now;
        }
    }
    if (changed) {
        ++s_matchVersion;
        noteDomMutation();
    }
    return changed;
}

// ----------------------------------------------------------------------------
// Tokens
// ----------------------------------------------------------------------------

namespace {

struct Token {
    enum Kind { Ident, Number, LParen, RParen, Colon, Slash, Less, LessEq, Greater, GreaterEq, Equal, Other, End };
    Kind kind = End;
    std::string text;  // identifier (lowercase) or the unit of a number
    double number = 0;
};

std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    size_t i = 0;
    auto isIdentStart = [](char c) {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
               static_cast<unsigned char>(c) >= 0x80;
    };
    auto isIdentChar = [&](char c) { return isIdentStart(c) || std::isdigit(static_cast<unsigned char>(c)); };
    while (i < s.size()) {
        char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) { i++; continue; }
        Token t;
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {  // comment
            size_t end = s.find("*/", i + 2);
            i = end == std::string::npos ? s.size() : end + 2;
            continue;
        }
        bool numberStart = std::isdigit(static_cast<unsigned char>(c)) ||
                           (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) ||
                           ((c == '+' || c == '-') && i + 1 < s.size() &&
                            (std::isdigit(static_cast<unsigned char>(s[i + 1])) || s[i + 1] == '.'));
        if (numberStart) {
            size_t j = i + 1;
            while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.')) j++;
            if (j < s.size() && (s[j] == 'e' || s[j] == 'E') && j + 1 < s.size() &&
                (std::isdigit(static_cast<unsigned char>(s[j + 1])) ||
                 ((s[j + 1] == '+' || s[j + 1] == '-') && j + 2 < s.size() &&
                  std::isdigit(static_cast<unsigned char>(s[j + 2]))))) {
                j += 2;
                while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) j++;
            }
            t.kind = Token::Number;
            t.number = std::strtod(s.substr(i, j - i).c_str(), nullptr);
            size_t u = j;
            if (u < s.size() && s[u] == '%') u++;
            else while (u < s.size() && std::isalpha(static_cast<unsigned char>(s[u]))) u++;
            for (size_t k = j; k < u; k++) t.text += static_cast<char>(std::tolower(static_cast<unsigned char>(s[k])));
            i = u;
        } else if (isIdentStart(c)) {
            size_t j = i;
            while (j < s.size() && isIdentChar(s[j])) j++;
            t.kind = Token::Ident;
            for (size_t k = i; k < j; k++) t.text += static_cast<char>(std::tolower(static_cast<unsigned char>(s[k])));
            i = j;
        } else {
            i++;
            switch (c) {
                case '(': t.kind = Token::LParen; break;
                case ')': t.kind = Token::RParen; break;
                case ':': t.kind = Token::Colon; break;
                case '/': t.kind = Token::Slash; break;
                case '=': t.kind = Token::Equal; break;
                case '<':
                case '>':
                    if (i < s.size() && s[i] == '=') {
                        i++;
                        t.kind = c == '<' ? Token::LessEq : Token::GreaterEq;
                    } else {
                        t.kind = c == '<' ? Token::Less : Token::Greater;
                    }
                    break;
                default: t.kind = Token::Other; t.text = std::string(1, c); break;
            }
        }
        out.push_back(std::move(t));
    }
    out.push_back(Token{});
    return out;
}

/// Splits a media query list at its top-level commas
std::vector<std::string> splitQueries(const std::string& text) {
    std::vector<std::string> parts;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); i++) {
        char c = i < text.size() ? text[i] : ',';
        if (c == '(' || c == '[' || c == '{') depth++;
        else if ((c == ')' || c == ']' || c == '}') && depth > 0) depth--;
        else if (c == ',' && depth == 0) {
            parts.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n\f");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n\f");
    return s.substr(b, e - b + 1);
}

/// Lowercases outside of nothing in particular and collapses runs of whitespace
std::string normalize(const std::string& s) {
    std::string out;
    bool space = false;
    for (char c : trim(s)) {
        if (std::isspace(static_cast<unsigned char>(c))) { space = true; continue; }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

// ----------------------------------------------------------------------------
// Parsed queries
// ----------------------------------------------------------------------------

struct MediaQueryList::Node {
    enum Kind { False, Type, Not, And, Or, Feature, Range };
    struct Value {
        enum Kind { None, Number, Ident, Ratio } kind = None;
        double number = 0;
        double denominator = 1;
        std::string unit;   // for Number
        std::string ident;  // for Ident
    };
    Kind kind = False;
    std::string name;  // media type, or feature name without min-/max-
    std::vector<std::shared_ptr<const Node>> kids;
    // Feature: prefix -1 = min-, 1 = max-, 0 none; hasValue false = boolean context
    int prefix = 0;
    bool hasValue = false;
    Value value;
    // Range: [low op1] name [op2 high]; ops are Token kinds (Less, LessEq, ...)
    // written as "value op name" (left) and "name op value" (right)
    bool hasLeft = false, hasRight = false;
    int leftOp = 0, rightOp = 0;
    Value left, right;
};

namespace {

using Node = MediaQueryList::Node;
using NodePtr = std::shared_ptr<const Node>;

NodePtr falseNode() { return std::make_shared<Node>(); }

class QueryParser {
public:
    explicit QueryParser(const std::string& text) : tokens_(tokenize(text)) {}

    /// <media-query>; null when it cannot be parsed
    NodePtr parseQuery() {
        NodePtr result;
        if (peek().kind == Token::LParen || (isKeyword("not") && peek(1).kind == Token::LParen)) {
            result = parseCondition(true);
        } else {
            bool negate = false;
            if (isKeyword("not")) { negate = true; next(); }
            else if (isKeyword("only")) next();
            if (peek().kind != Token::Ident || isReserved(peek().text)) return nullptr;
            auto type = std::make_shared<Node>();
            type->kind = Node::Type;
            type->name = next().text;
            NodePtr query = type;
            if (isKeyword("and")) {
                next();
                NodePtr condition = parseCondition(false);
                if (!condition) return nullptr;
                auto both = std::make_shared<Node>();
                both->kind = Node::And;
                both->kids = {type, condition};
                query = both;
            }
            if (negate) {
                auto n = std::make_shared<Node>();
                n->kind = Node::Not;
                n->kids = {query};
                query = n;
            }
            result = query;
        }
        if (!result || peek().kind != Token::End) return nullptr;
        return result;
    }

private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;

    const Token& peek(size_t ahead = 0) const {
        size_t i = std::min(pos_ + ahead, tokens_.size() - 1);
        return tokens_[i];
    }
    const Token& next() {
        const Token& t = tokens_[pos_];
        if (pos_ + 1 < tokens_.size()) pos_++;
        return t;
    }
    bool isKeyword(const char* word) const { return peek().kind == Token::Ident && peek().text == word; }
    static bool isReserved(const std::string& w) {
        return w == "not" || w == "and" || w == "or" || w == "only" || w == "layer";
    }

    /// <media-condition> (allowOr) or <media-condition-without-or>
    NodePtr parseCondition(bool allowOr) {
        if (isKeyword("not")) {
            next();
            NodePtr inner = parseInParens();
            if (!inner) return nullptr;
            auto n = std::make_shared<Node>();
            n->kind = Node::Not;
            n->kids = {inner};
            return n;
        }
        NodePtr first = parseInParens();
        if (!first) return nullptr;
        if (!isKeyword("and") && !(allowOr && isKeyword("or"))) return first;
        std::string joiner = peek().text;
        auto group = std::make_shared<Node>();
        group->kind = joiner == "and" ? Node::And : Node::Or;
        group->kids.push_back(first);
        while (isKeyword(joiner.c_str())) {
            next();
            NodePtr more = parseInParens();
            if (!more) return nullptr;
            group->kids.push_back(more);
        }
        // "a and b or c" is invalid: mixing needs parentheses
        if (isKeyword("and") || isKeyword("or")) return nullptr;
        return group;
    }

    /// <media-in-parens>: a nested condition, a feature, or <general-enclosed> (false)
    NodePtr parseInParens() {
        if (peek().kind != Token::LParen) return nullptr;
        size_t start = pos_;
        next();
        // A nested condition
        if (peek().kind == Token::LParen || isKeyword("not")) {
            size_t save = pos_;
            NodePtr inner = parseCondition(true);
            if (inner && peek().kind == Token::RParen) {
                next();
                return inner;
            }
            pos_ = save;
        }
        NodePtr feature = parseFeature();
        if (feature && peek().kind == Token::RParen) {
            next();
            return feature;
        }
        // <general-enclosed>: anything balanced; matches nothing
        pos_ = start;
        return skipEnclosed() ? falseNode() : nullptr;
    }

    bool skipEnclosed() {
        int depth = 0;
        while (peek().kind != Token::End) {
            Token::Kind k = next().kind;
            if (k == Token::LParen) depth++;
            else if (k == Token::RParen && --depth == 0) return true;
        }
        return false;
    }

    bool parseValue(Node::Value& out) {
        const Token& t = peek();
        if (t.kind == Token::Number) {
            next();
            out.kind = Node::Value::Number;
            out.number = t.number;
            out.unit = t.text;
            if (out.unit.empty() && peek().kind == Token::Slash && peek(1).kind == Token::Number &&
                peek(1).text.empty()) {
                next();
                out.kind = Node::Value::Ratio;
                out.denominator = next().number;
            }
            return true;
        }
        if (t.kind == Token::Ident && !isReserved(t.text)) {
            next();
            out.kind = Node::Value::Ident;
            out.ident = t.text;
            return true;
        }
        return false;
    }

    static bool isComparison(Token::Kind k) {
        return k == Token::Less || k == Token::LessEq || k == Token::Greater || k == Token::GreaterEq ||
               k == Token::Equal;
    }

    NodePtr parseFeature() {
        auto n = std::make_shared<Node>();
        // "name", "name: value", "name op value"
        if (peek().kind == Token::Ident && (peek(1).kind == Token::RParen || peek(1).kind == Token::Colon ||
                                            isComparison(peek(1).kind))) {
            std::string name = next().text;
            if (peek().kind == Token::RParen) {
                n->kind = Node::Feature;
                n->name = name;
                return n;
            }
            if (peek().kind == Token::Colon) {
                next();
                n->kind = Node::Feature;
                if (name.rfind("min-", 0) == 0) { n->prefix = -1; name = name.substr(4); }
                else if (name.rfind("max-", 0) == 0) { n->prefix = 1; name = name.substr(4); }
                else if (name.rfind("-webkit-min-", 0) == 0) { n->prefix = -1; name = "-webkit-" + name.substr(12); }
                else if (name.rfind("-webkit-max-", 0) == 0) { n->prefix = 1; name = "-webkit-" + name.substr(12); }
                n->name = name;
                n->hasValue = true;
                return parseValue(n->value) ? n : nullptr;
            }
            n->kind = Node::Range;
            n->name = name;
            n->hasRight = true;
            n->rightOp = next().kind;
            return parseValue(n->right) ? n : nullptr;
        }
        // "value op name [op value]"
        n->kind = Node::Range;
        if (!parseValue(n->left) || !isComparison(peek().kind)) return nullptr;
        n->hasLeft = true;
        n->leftOp = next().kind;
        if (peek().kind != Token::Ident) return nullptr;
        n->name = next().text;
        if (isComparison(peek().kind)) {
            n->hasRight = true;
            n->rightOp = next().kind;
            if (!parseValue(n->right)) return nullptr;
            // Both comparisons must point the same way
            bool leftLess = n->leftOp == Token::Less || n->leftOp == Token::LessEq;
            bool rightLess = n->rightOp == Token::Less || n->rightOp == Token::LessEq;
            if (n->leftOp == Token::Equal || n->rightOp == Token::Equal || leftLess != rightLess) return nullptr;
        }
        return n;
    }
};

// ----------------------------------------------------------------------------
// Matching
// ----------------------------------------------------------------------------

enum class FeatureType { Unknown, Length, Ratio, Resolution, Integer, Number, Discrete };

FeatureType featureType(const std::string& name) {
    if (name == "width" || name == "height" || name == "device-width" || name == "device-height") return FeatureType::Length;
    if (name == "aspect-ratio" || name == "device-aspect-ratio") return FeatureType::Ratio;
    if (name == "resolution") return FeatureType::Resolution;
    if (name == "color" || name == "color-index" || name == "monochrome") return FeatureType::Integer;
    if (name == "-webkit-device-pixel-ratio") return FeatureType::Number;
    static const char* discrete[] = {"orientation", "prefers-color-scheme", "prefers-reduced-motion",
        "prefers-reduced-transparency", "prefers-reduced-data", "prefers-contrast", "hover", "any-hover",
        "pointer", "any-pointer", "scripting", "update", "display-mode", "forced-colors", "inverted-colors",
        "color-gamut", "dynamic-range", "video-dynamic-range", "grid", "overflow-block", "overflow-inline", "scan"};
    for (const char* d : discrete) if (name == d) return FeatureType::Discrete;
    return FeatureType::Unknown;
}

/// A length in CSS pixels; false for a value that is not a length
bool toPixels(const Node::Value& v, const MediaEnvironment& env, double& px) {
    if (v.kind != Node::Value::Number) return false;
    const std::string& u = v.unit;
    double n = v.number;
    if (u.empty()) { px = 0; return n == 0; }
    if (u == "px") px = n;
    else if (u == "em" || u == "rem") px = n * 16;  // media queries use the initial font size
    else if (u == "ex" || u == "ch") px = n * 8;
    else if (u == "in") px = n * 96;
    else if (u == "cm") px = n * 96 / 2.54;
    else if (u == "mm") px = n * 96 / 25.4;
    else if (u == "q") px = n * 96 / 101.6;
    else if (u == "pt") px = n * 96 / 72;
    else if (u == "pc") px = n * 16;
    else if (u == "vw") px = n * env.width / 100;
    else if (u == "vh") px = n * env.height / 100;
    else if (u == "vmin") px = n * std::min(env.width, env.height) / 100;
    else if (u == "vmax") px = n * std::max(env.width, env.height) / 100;
    else return false;
    return true;
}

/// A resolution in dppx
bool toDppx(const Node::Value& v, double& dppx) {
    if (v.kind != Node::Value::Number) return false;
    if (v.unit == "dppx" || v.unit == "x") dppx = v.number;
    else if (v.unit == "dpi") dppx = v.number / 96;
    else if (v.unit == "dpcm") dppx = v.number * 2.54 / 96;
    else return false;
    return true;
}

/// The environment's value of a range feature, and the query's value in the same units
bool rangeValues(const std::string& name, const Node::Value& v, const MediaEnvironment& env,
                 double& actual, double& wanted) {
    switch (featureType(name)) {
        case FeatureType::Length:
            actual = (name == "width" || name == "device-width") ? env.width : env.height;
            return toPixels(v, env, wanted);
        case FeatureType::Ratio:
            actual = env.height > 0 ? env.width / env.height : 0;
            if (v.kind == Node::Value::Ratio) {
                if (v.denominator == 0) return false;
                wanted = v.number / v.denominator;
                return true;
            }
            if (v.kind == Node::Value::Number && v.unit.empty()) { wanted = v.number; return true; }
            return false;
        case FeatureType::Resolution:
            actual = env.devicePixelRatio;
            if (v.kind == Node::Value::Ident && v.ident == "infinite") { wanted = INFINITY; return true; }
            return toDppx(v, wanted);
        case FeatureType::Integer:
            actual = name == "color" ? 8 : 0;
            if (v.kind != Node::Value::Number || !v.unit.empty() || v.number != std::floor(v.number)) return false;
            wanted = v.number;
            return true;
        case FeatureType::Number:
            actual = env.devicePixelRatio;
            if (v.kind != Node::Value::Number || !v.unit.empty()) return false;
            wanted = v.number;
            return true;
        default:
            return false;
    }
}

bool compare(double a, int op, double b) {
    const double eps = 1e-9;
    switch (op) {
        case Token::Less: return a < b - eps;
        case Token::LessEq: return a <= b + eps;
        case Token::Greater: return a > b + eps;
        case Token::GreaterEq: return a >= b - eps;
        case Token::Equal: return std::fabs(a - b) <= eps;
        default: return false;
    }
}

/// The current keyword of a discrete feature
std::string discreteValue(const std::string& name, const MediaEnvironment& env) {
    if (name == "orientation") return env.height >= env.width ? "portrait" : "landscape";
    if (name == "prefers-color-scheme") return env.prefersDark ? "dark" : "light";
    if (name == "prefers-reduced-motion") return env.prefersReducedMotion ? "reduce" : "no-preference";
    if (name == "hover" || name == "any-hover") return "hover";
    if (name == "pointer" || name == "any-pointer") return "fine";
    if (name == "scripting") return "enabled";
    if (name == "update") return "fast";
    if (name == "display-mode") return "browser";
    if (name == "color-gamut") return "srgb";
    if (name == "dynamic-range" || name == "video-dynamic-range") return "standard";
    if (name == "overflow-block") return "scroll";
    if (name == "overflow-inline") return "scroll";
    if (name == "scan") return "progressive";
    if (name == "grid") return "0";
    if (name == "forced-colors" || name == "inverted-colors") return "none";
    return "no-preference";  // prefers-contrast, -reduced-transparency, -reduced-data
}

/// A discrete feature in a boolean context ("(hover)"): false for its "off" value
bool discreteTrue(const std::string& name, const MediaEnvironment& env) {
    std::string v = discreteValue(name, env);
    return v != "none" && v != "no-preference" && v != "0";
}

bool matchNode(const Node& n, const MediaEnvironment& env) {
    switch (n.kind) {
        case Node::False: return false;
        case Node::Type: return n.name == "all" || n.name == "screen";
        case Node::Not: return !matchNode(*n.kids[0], env);
        case Node::And:
            for (const auto& k : n.kids) if (!matchNode(*k, env)) return false;
            return true;
        case Node::Or:
            for (const auto& k : n.kids) if (matchNode(*k, env)) return true;
            return false;
        case Node::Feature: {
            FeatureType type = featureType(n.name);
            if (type == FeatureType::Unknown) return false;
            if (!n.hasValue) {
                if (type == FeatureType::Discrete) return discreteTrue(n.name, env);
                Node::Value zero;
                zero.kind = Node::Value::Number;
                double actual = 0, ignored = 0;
                rangeValues(n.name, zero, env, actual, ignored);
                return actual != 0;
            }
            if (type == FeatureType::Discrete) {
                if (n.prefix != 0) return false;  // min-/max- only on range features
                if (n.name == "grid" && n.value.kind == Node::Value::Number) return n.value.number == 0;
                return n.value.kind == Node::Value::Ident && n.value.ident == discreteValue(n.name, env);
            }
            double actual = 0, wanted = 0;
            if (!rangeValues(n.name, n.value, env, actual, wanted)) return false;
            if (n.prefix < 0) return compare(actual, Token::GreaterEq, wanted);
            if (n.prefix > 0) return compare(actual, Token::LessEq, wanted);
            return compare(actual, Token::Equal, wanted);
        }
        case Node::Range: {
            FeatureType type = featureType(n.name);
            if (type == FeatureType::Unknown || type == FeatureType::Discrete) return false;
            if (n.name.rfind("min-", 0) == 0 || n.name.rfind("max-", 0) == 0) return false;
            double actual = 0, wanted = 0;
            if (n.hasLeft) {
                // "value op name": compare value op actual
                if (!rangeValues(n.name, n.left, env, actual, wanted)) return false;
                if (!compare(wanted, n.leftOp, actual)) return false;
            }
            if (n.hasRight) {
                if (!rangeValues(n.name, n.right, env, actual, wanted)) return false;
                if (!compare(actual, n.rightOp, wanted)) return false;
            }
            return true;
        }
    }
    return false;
}

}  // namespace

MediaQueryList::~MediaQueryList() = default;

std::shared_ptr<MediaQueryList> MediaQueryList::parse(const std::string& text) {
    std::shared_ptr<MediaQueryList> list(new MediaQueryList());
    std::vector<std::string> parts;
    std::string trimmed = trim(text);
    if (!trimmed.empty()) parts = splitQueries(trimmed);
    for (const std::string& part : parts) {
        std::string query = normalize(part);
        list->text_ += (list->text_.empty() ? "" : ", ") + (query.empty() ? std::string("not all") : query);
        list->queries_.push_back(query.empty() ? nullptr : QueryParser(query).parseQuery());
    }
    list->lastResult_ = list->matches(s_env) ? 1 : 0;
    std::lock_guard<std::mutex> lock(s_registryMutex);
    auto& lists = registry();
    if (lists.size() > 256 && lists.size() % 256 == 0) {
        lists.erase(std::remove_if(lists.begin(), lists.end(),
                                   [](const std::weak_ptr<MediaQueryList>& w) { return w.expired(); }),
                    lists.end());
    }
    lists.push_back(list);
    return list;
}

bool MediaQueryList::matches(const MediaEnvironment& env) const {
    if (queries_.empty()) return true;
    for (const auto& q : queries_) {
        if (q && matchNode(*q, env)) return true;
    }
    return false;
}

bool allMediaMatch(const std::vector<std::shared_ptr<const MediaQueryList>>& lists) {
    for (const auto& l : lists) {
        if (l && !l->matches()) return false;
    }
    return true;
}

}  // namespace Zepra::WebCore
