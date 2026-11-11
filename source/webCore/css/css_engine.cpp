// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file css_engine.cpp
 * @brief CSSParser implementation and CSSEngine delegation layer
 *
 * NOTE: StyleResolver is implemented in style_resolver.cpp
 *       CSSCascade is implemented in css_cascade.cpp
 *       DO NOT add duplicate definitions here.
 */

#include "css/css_engine.hpp"
#include <algorithm>
#include "browser/dom.hpp"
#include <iostream>
#include <sstream>
#include <cctype>
#include <cstring>
#include <functional>
#include <unordered_set>

namespace Zepra::WebCore {

// ============================================================================
// CSSParser Implementation
// ============================================================================

void CSSParser::skipWhitespace() {
    while (pos_ < input_.length() && 
           (std::isspace(input_[pos_]) || input_[pos_] == '\0')) {
        pos_++;
    }
    skipComment();
}

void CSSParser::skipComment() {
    while (pos_ + 1 < input_.length() && input_[pos_] == '/' && input_[pos_+1] == '*') {
        pos_ += 2;
        while (pos_ + 1 < input_.length()) {
            if (input_[pos_] == '*' && input_[pos_+1] == '/') {
                pos_ += 2;
                break;
            }
            pos_++;
        }
        // Skip any whitespace after comment
        while (pos_ < input_.length() && std::isspace(input_[pos_])) pos_++;
    }
}

char CSSParser::peek() const {
    if (pos_ >= input_.length()) return '\0';
    return input_[pos_];
}

char CSSParser::consume() {
    if (pos_ >= input_.length()) return '\0';
    return input_[pos_++];
}

bool CSSParser::match(char c) {
    skipWhitespace();
    if (pos_ < input_.length() && input_[pos_] == c) {
        pos_++;
        return true;
    }
    return false;
}

bool CSSParser::eof() const {
    return pos_ >= input_.length();
}

std::string CSSParser::parseIdentifier() {
    std::string result;
    skipWhitespace();
    while (pos_ < input_.length() && 
           (std::isalnum(input_[pos_]) || input_[pos_] == '-' || 
            input_[pos_] == '_' || input_[pos_] == '\\')) {
        result += input_[pos_++];
    }
    return result;
}

std::string CSSParser::parseString() {
    std::string result;
    char quote = consume(); // ' or "
    while (!eof() && peek() != quote) {
        if (peek() == '\\') {
            consume();
            if (!eof()) result += consume();
        } else {
            result += consume();
        }
    }
    if (!eof()) consume(); // closing quote
    return result;
}

std::string CSSParser::parseUrl() {
    std::string result;
    skipWhitespace();
    if (peek() == '"' || peek() == '\'') {
        result = parseString();
    } else {
        while (!eof() && peek() != ')') {
            result += consume();
        }
    }
    return result;
}

std::string CSSParser::parseSelector_() {
    std::string selector;
    skipWhitespace();
    int depth = 0;
    while (!eof()) {
        char c = peek();
        if (c == '{' && depth == 0) break;
        if (c == '(') depth++;
        if (c == ')') depth--;
        selector += consume();
    }
    // Trim trailing whitespace
    while (!selector.empty() && std::isspace(selector.back())) {
        selector.pop_back();
    }
    return selector;
}

std::string CSSParser::parsePropertyValue() {
    std::string value;
    skipWhitespace();
    int depth = 0;
    while (!eof()) {
        char c = peek();
        if (c == ';' && depth == 0) break;
        if (c == '}' && depth == 0) break;
        if (c == '(') depth++;
        if (c == ')') depth--;
        value += consume();
    }
    // Trim trailing whitespace
    while (!value.empty() && std::isspace(value.back())) {
        value.pop_back();
    }
    return value;
}

std::string CSSParser::parseDeclarationBlock() {
    std::string block;
    if (!match('{')) return block;
    int depth = 1;
    while (!eof() && depth > 0) {
        char c = peek();
        if (c == '{') depth++;
        if (c == '}') { depth--; if (depth == 0) { consume(); break; } }
        block += consume();
    }
    return block;
}

void CSSParser::parseDeclarations(CSSStyleDeclaration* decl) {
    if (!decl) return;
    
    skipWhitespace();
    while (!eof() && peek() != '}') {
        skipWhitespace();
        if (eof() || peek() == '}') break;
        
        // Parse property name
        std::string property;
        while (!eof() && peek() != ':' && peek() != '}' && peek() != ';') {
            property += consume();
        }
        
        // Trim
        while (!property.empty() && std::isspace(property.back())) property.pop_back();
        while (!property.empty() && std::isspace(property.front())) property.erase(property.begin());
        
        if (property.empty() || !match(':')) {
            // Skip to next declaration
            while (!eof() && peek() != ';' && peek() != '}') consume();
            if (peek() == ';') consume();
            continue;
        }
        
        // Parse value
        std::string value = parsePropertyValue();
        // Trim
        while (!value.empty() && std::isspace(value.front())) value.erase(value.begin());
        
        // Check for !important
        bool important = false;
        size_t impPos = value.find("!important");
        if (impPos != std::string::npos) {
            important = true;
            value = value.substr(0, impPos);
            while (!value.empty() && std::isspace(value.back())) value.pop_back();
        }
        
        if (!property.empty() && !value.empty()) {
            decl->setProperty(property, value, important ? "important" : "");
        }
        
        if (peek() == ';') consume();
        skipWhitespace();
    }
}

std::unique_ptr<CSSStyleRule> CSSParser::parseStyleRule() {
    std::string selector = parseSelector_();
    if (selector.empty()) return nullptr;
    
    auto rule = std::make_unique<CSSStyleRule>();
    rule->setSelectorText(selector);
    
    if (!match('{')) return nullptr;
    
    // Parse declarations into the rule's style
    parseDeclarations(rule->style());
    
    // Skip closing brace
    if (peek() == '}') consume();
    
    return rule;
}

std::unique_ptr<CSSMediaRule> CSSParser::parseMediaRule() {
    // Skip @media query { ... }
    std::string query;
    while (!eof() && peek() != '{') query += consume();
    
    auto rule = std::make_unique<CSSMediaRule>();
    if (match('{')) {
        // Parse rules inside media block
        int depth = 1;
        while (!eof() && depth > 0) {
            if (peek() == '{') depth++;
            if (peek() == '}') depth--;
            if (depth > 0) consume();
        }
        if (peek() == '}') consume();
    }
    return rule;
}

std::unique_ptr<CSSFontFaceRule> CSSParser::parseFontFaceRule() {
    auto rule = std::make_unique<CSSFontFaceRule>();
    while (!eof() && peek() != '{' && peek() != ';') consume();  // @font-face has no prelude
    if (peek() == ';') {
        consume();
        return nullptr;
    }
    if (!match('{')) return nullptr;
    parseDeclarations(rule->style());
    if (peek() == '}') consume();
    rule->updateDescriptors();
    return rule;
}

// @keyframes <name> { <keyframe-selector># { <declarations> } ... }
// (CSS Animations 1 §3). The name is an identifier or a string; a keyframe
// whose selector is invalid is dropped when the keyframes are used.
std::unique_ptr<CSSKeyframesRule> CSSParser::parseKeyframesRule() {
    auto rule = std::make_unique<CSSKeyframesRule>();
    std::string name;
    while (!eof() && peek() != '{' && peek() != ';') name += consume();
    size_t a = name.find_first_not_of(" \t\r\n\f"), b = name.find_last_not_of(" \t\r\n\f");
    name = a == std::string::npos ? "" : name.substr(a, b - a + 1);
    if (name.size() >= 2 && (name.front() == '"' || name.front() == '\'') && name.back() == name.front())
        name = name.substr(1, name.size() - 2);
    rule->setName(name);
    if (!match('{')) {
        if (peek() == ';') consume();
        return rule;
    }
    while (!eof()) {
        skipWhitespace();
        if (eof()) break;
        if (peek() == '}') {
            consume();
            break;
        }
        std::string selector;
        while (!eof() && peek() != '{' && peek() != '}') selector += consume();
        if (!match('{')) continue;   // a stray '}' ends the rule on the next turn
        // The keyframe's declarations, up to its matching '}' (strings and
        // nested blocks such as a var() fallback are stepped over whole)
        std::string body;
        int depth = 1;
        while (!eof()) {
            char c = peek();
            if (c == '"' || c == '\'') {
                body += consume();
                while (!eof() && peek() != c) {
                    if (peek() == '\\') body += consume();
                    if (!eof()) body += consume();
                }
                if (!eof()) body += consume();
                continue;
            }
            if (c == '{') depth++;
            if (c == '}' && --depth == 0) {
                consume();
                break;
            }
            body += consume();
        }
        auto frame = std::make_unique<CSSKeyframeRule>();
        frame->setKeyText(selector);
        CSSParser declarations;
        frame->setStyle(declarations.parseInlineStyle(body));
        rule->cssRules()->add(std::move(frame));
    }
    return rule;
}

// @import url("x") [layer | layer(name)] [supports(cond)] [media queries];
// The stylesheet is fetched by CSSEngine::addStyleSheet()
std::unique_ptr<CSSImportRule> CSSParser::parseImportRule() {
    std::string prelude;
    int parens = 0;
    while (!eof() && !(peek() == ';' && parens == 0)) {
        char c = consume();
        if (c == '"' || c == '\'') {
            prelude += c;
            while (!eof() && peek() != c && peek() != '\n') {
                if (peek() == '\\') prelude += consume();
                if (!eof()) prelude += consume();
            }
            if (!eof() && peek() == c) prelude += consume();
            continue;
        }
        if (c == '(') parens++;
        else if (c == ')' && parens > 0) parens--;
        else if (c == '{' && parens == 0) {
            // Not an @import statement: drop it with its block
            pos_--;
            skipInvalidRule();
            return nullptr;
        }
        prelude += c;
    }
    if (peek() == ';') consume();

    auto ws = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    size_t i = 0;
    auto skipWs = [&]() { while (i < prelude.size() && ws(prelude[i])) i++; };
    auto readString = [&](std::string& out) {
        char q = prelude[i++];
        while (i < prelude.size() && prelude[i] != q) {
            if (prelude[i] == '\\' && i + 1 < prelude.size()) i++;
            out += prelude[i++];
        }
        if (i < prelude.size()) i++;
    };
    auto startsWithWord = [&](const char* w) {
        size_t n = std::strlen(w);
        if (i + n > prelude.size()) return false;
        for (size_t k = 0; k < n; k++) {
            if (std::tolower(static_cast<unsigned char>(prelude[i + k])) != w[k]) return false;
        }
        return true;
    };
    // Index past the ")" matching the "(" at j
    auto closeParen = [&](size_t j) {
        int depth = 0;
        for (; j < prelude.size(); j++) {
            if (prelude[j] == '(') depth++;
            else if (prelude[j] == ')' && --depth == 0) return j + 1;
        }
        return prelude.size();
    };

    skipWs();
    std::string href;
    if (i < prelude.size() && (prelude[i] == '"' || prelude[i] == '\'')) {
        readString(href);
    } else if (startsWithWord("url(")) {
        i += 4;
        skipWs();
        if (i < prelude.size() && (prelude[i] == '"' || prelude[i] == '\'')) {
            readString(href);
            skipWs();
            if (i < prelude.size() && prelude[i] == ')') i++;
        } else {
            while (i < prelude.size() && prelude[i] != ')') href += prelude[i++];
            if (i < prelude.size()) i++;
            while (!href.empty() && ws(href.back())) href.pop_back();
        }
    } else {
        return nullptr;  // no URL: invalid
    }
    if (href.empty()) return nullptr;

    auto rule = std::make_unique<CSSImportRule>();
    rule->setHref(href);
    skipWs();
    if (startsWithWord("layer(")) {
        i = closeParen(i + 5);
    } else if (startsWithWord("layer") && (i + 5 == prelude.size() || ws(prelude[i + 5]))) {
        i += 5;
    }
    skipWs();
    if (startsWithWord("supports(")) {
        size_t end = closeParen(i + 8);
        rule->setSupportsCondition(prelude.substr(i + 9, end > i + 10 ? end - i - 10 : 0));
        i = end;
    }
    skipWs();
    std::string media = prelude.substr(i);
    while (!media.empty() && ws(media.back())) media.pop_back();
    rule->setMediaText(media);
    return rule;
}

std::unique_ptr<CSSRule> CSSParser::parseRule() {
    skipWhitespace();
    if (eof()) return nullptr;
    
    // Check for at-rules
    if (peek() == '@') {
        consume();
        std::string atWord = parseIdentifier();
        for (char& ch : atWord) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));  // at-keywords ignore case
        
        if (atWord == "media") return parseMediaRule();
        if (atWord == "font-face") return parseFontFaceRule();
        if (atWord == "keyframes" || atWord == "-webkit-keyframes") return parseKeyframesRule();
        if (atWord == "import") return parseImportRule();
        
        // @layer, @supports — contain normal style rules, parse them
        // (Firefox/Servo: collect_rules_in_list, WebKit: collectMatchingRules)
        if (atWord == "layer" || atWord == "supports") {
            // Skip name/condition until opening brace or semicolon
            while (!eof() && peek() != '{' && peek() != ';') consume();
            if (peek() == ';') {
                consume(); // @layer name; declaration — no block
                return nullptr;
            }
            // Handled by parse() which will re-enter the loop for inner rules
            // We return nullptr here; the caller (parse/parseLayerBlock) handles it
            return nullptr;
        }
        
        // @property, @theme (Tailwind v4) — contain variable definitions
        // Parse as :root style rules so var() fallbacks at least get the values
        if (atWord == "property" || atWord == "theme") {
            while (!eof() && peek() != '{' && peek() != ';') consume();
            if (peek() == ';') { consume(); return nullptr; }
            // Handled by parse() which will re-enter for inner rules
            return nullptr;
        }

        // Unknown at-rule — skip to next semicolon or block
        while (!eof() && peek() != ';' && peek() != '{') consume();
        if (peek() == ';') consume();
        else if (match('{')) {
            int depth = 1;
            while (!eof() && depth > 0) {
                if (peek() == '{') depth++;
                if (peek() == '}') depth--;
                if (depth > 0) consume();
            }
            if (peek() == '}') consume();
        }
        return nullptr;
    }
    
    return parseStyleRule();
}

std::unique_ptr<CSSStyleSheet> CSSParser::parse(const std::string& css) {
    input_ = css;
    pos_ = 0;
    
    mediaStack_.clear();
    sawRule_ = false;
    layerPath_.clear();
    anonymousLayers_ = 0;
    auto sheet = std::make_unique<CSSStyleSheet>();
    parseRulesInto(sheet.get(), 0);
    return sheet;
}

// Recursively parse rules, handling nested @layer/@supports blocks
void CSSParser::parseRulesInto(CSSStyleSheet* sheet, int depth) {
    while (!eof()) {
        skipWhitespace();
        if (eof()) break;
        
        // If we're inside a block (depth > 0), check for closing brace
        if (depth > 0 && peek() == '}') {
            consume();
            return;
        }
        
        // Check for @layer/@supports/@theme/@property — they need recursive descent
        if (peek() == '@') {
            size_t savedPos = pos_;
            consume();
            std::string atWord = parseIdentifier();
            for (char& ch : atWord) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));  // at-keywords ignore case
            
            // @media: its rules are kept (in source order) with the query
            // list, which the cascade matches against the viewport
            if (atWord == "media") {
                std::string prelude;
                while (!eof() && peek() != '{' && peek() != ';') prelude += consume();
                if (peek() == ';') { consume(); continue; }
                if (peek() == '{') {
                    consume();
                    sawRule_ = true;
                    mediaStack_.push_back(MediaQueryList::parse(prelude));
                    parseRulesInto(sheet, depth + 1);
                    mediaStack_.pop_back();
                    continue;
                }
                continue;  // end of input
            }

            if (atWord == "property") {
                parsePropertyRule(sheet);
                continue;
            }

            // @layer: a statement names layers (fixing their order), a block
            // puts its rules in one (CSS Cascade 5 §6.4). Names nest as a
            // dotted path under the enclosing layer.
            if (atWord == "layer") {
                std::string prelude;
                while (!eof() && peek() != '{' && peek() != ';') prelude += consume();
                auto fullName = [&](std::string name) {
                    size_t a = name.find_first_not_of(" \t\r\n\f"), b = name.find_last_not_of(" \t\r\n\f");
                    name = a == std::string::npos ? "" : name.substr(a, b - a + 1);
                    if (name.empty()) name = "\x01anonymous" + std::to_string(++anonymousLayers_);
                    return layerPath_.empty() ? name : layerPath_ + "." + name;
                };
                if (peek() == ';') {
                    consume();
                    size_t start = 0;
                    for (size_t i = 0; i <= prelude.size(); i++) {
                        if (i == prelude.size() || prelude[i] == ',') {
                            std::string name = fullName(prelude.substr(start, i - start));
                            if (name.find('\x01') == std::string::npos) sheet->declareLayer(name);
                            start = i + 1;
                        }
                    }
                    continue;
                }
                if (peek() == '{') {
                    consume();
                    sawRule_ = true;
                    std::string saved = layerPath_;
                    layerPath_ = fullName(prelude);
                    sheet->declareLayer(layerPath_);
                    parseRulesInto(sheet, depth + 1);
                    layerPath_ = saved;
                    continue;
                }
            }

            if (atWord == "supports" || atWord == "theme") {
                // Skip the condition; the block's rules apply as if unconditional
                while (!eof() && peek() != '{' && peek() != ';') consume();
                if (peek() == ';') {
                    consume();
                    continue;
                }
                if (peek() == '{') {
                    consume(); // Enter the block
                    sawRule_ = true;
                    parseRulesInto(sheet, depth + 1); // Recursively parse inner rules
                    continue;
                }
            }
            
            // Not a recursive at-rule — restore position and use normal parseRule
            pos_ = savedPos;
        }
        
        size_t before = pos_;
        auto rule = parseRule();
        // @import is only valid before every other rule of the sheet
        // (@charset and @layer statements aside)
        if (rule && rule->type() == CSSRuleType::Import && (depth > 0 || sawRule_)) rule.reset();
        if (rule && depth == 0 && rule->type() != CSSRuleType::Import) sawRule_ = true;
        if (rule && !layerPath_.empty()) {
            if (auto* styleRule = dynamic_cast<CSSStyleRule*>(rule.get())) styleRule->setLayer(layerPath_);
        }
        if (rule && !mediaStack_.empty()) {
            if (auto* styleRule = dynamic_cast<CSSStyleRule*>(rule.get())) styleRule->setMedia(mediaStack_);
            else if (auto* fontRule = dynamic_cast<CSSFontFaceRule*>(rule.get())) fontRule->setMedia(mediaStack_);
        }
        if (rule && sheet->cssRules()) {
            sheet->cssRules()->add(std::move(rule));
        }
        // A rule the parser could not read must still be stepped over, or this loop
        // spins forever on the UI thread (a stray "{ ... }" with no selector did)
        if (pos_ == before) skipInvalidRule();
    }
}

// @property --name { syntax: "<length>"; inherits: false; initial-value: 0px; }
// (CSS Properties and Values API 1, 3). An invalid rule is ignored.
void CSSParser::parsePropertyRule(CSSStyleSheet* sheet) {
    std::string name;
    while (!eof() && peek() != '{' && peek() != ';') name += consume();
    if (peek() == ';') { consume(); return; }
    if (!match('{')) return;
    CSSStyleDeclaration descriptors;
    parseDeclarations(&descriptors);
    if (peek() == '}') consume();

    auto trimmed = [](std::string v) {
        size_t b = v.find_first_not_of(" \t\r\n"), e = v.find_last_not_of(" \t\r\n");
        return b == std::string::npos ? std::string() : v.substr(b, e - b + 1);
    };
    name = trimmed(name);
    if (!isCustomPropertyName(name) || !sheet) return;
    std::string syntax = trimmed(descriptors.getPropertyValue("syntax"));
    if (syntax.size() >= 2 && (syntax.front() == '"' || syntax.front() == '\'') && syntax.back() == syntax.front()) {
        syntax = trimmed(syntax.substr(1, syntax.size() - 2));
    } else {
        syntax.clear();  // the syntax descriptor is a string
    }
    std::string inherits = descriptors.getPropertyValue("inherits");
    for (char& c : inherits) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    inherits = trimmed(inherits);
    if (syntax.empty() || (inherits != "true" && inherits != "false")) return;

    RegisteredCustomProperty reg;
    reg.syntax = syntax;
    reg.inherits = inherits == "true";
    for (size_t i = 0; i < descriptors.length(); i++) {
        if (descriptors.item(i) == "initial-value") {
            reg.hasInitialValue = true;
            reg.initialValue = trimmed(descriptors.getPropertyValue("initial-value"));
        }
    }
    // Anything but the universal syntax needs a computationally independent initial value
    if (syntax != "*" && (!reg.hasInitialValue || containsVarFunction(reg.initialValue))) return;
    sheet->addPropertyRegistration(name, std::move(reg));
}

// Error recovery for an unreadable rule (CSS Syntax 3, "consume a qualified rule"):
// drop everything up to and including its {}-block, or one character if there is none
void CSSParser::skipInvalidRule() {
    if (eof()) return;
    if (peek() != '{') {
        consume();
        return;
    }
    int depth = 0;
    while (!eof()) {
        char c = consume();
        if (c == '{') depth++;
        else if (c == '}' && --depth == 0) return;
    }
}

std::unique_ptr<CSSStyleDeclaration> CSSParser::parseInlineStyle(const std::string& style) {
    input_ = style;
    pos_ = 0;
    
    auto decl = std::make_unique<CSSStyleDeclaration>();
    parseDeclarations(decl.get());
    
    return decl;
}

Selector CSSParser::parseSelector(const std::string& selector) {
    return Selector::parse(selector);
}

std::unique_ptr<MediaList> CSSParser::parseMediaQuery(const std::string& query) {
    return std::make_unique<MediaList>();
}

// ============================================================================
// CSSEngine — Delegates to StyleResolver and CSSParser
// ============================================================================

CSSEngine::CSSEngine() {}
CSSEngine::~CSSEngine() {}

void CSSEngine::initialize(DOMDocument* doc) {
    document_ = doc;
    // StyleResolver constructor already adds user-agent stylesheet
}

void CSSEngine::noteStyleAttributeSelectors(const std::string& css) {
    if (usesStyleAttribute_) return;
    // "[style", any case, with white space allowed after the bracket
    for (size_t i = css.find('['); i != std::string::npos; i = css.find('[', i + 1)) {
        size_t j = i + 1;
        while (j < css.size() && std::isspace(static_cast<unsigned char>(css[j]))) j++;
        if (css.size() - j >= 5 && std::tolower(static_cast<unsigned char>(css[j])) == 's' &&
            std::tolower(static_cast<unsigned char>(css[j + 1])) == 't' &&
            std::tolower(static_cast<unsigned char>(css[j + 2])) == 'y' &&
            std::tolower(static_cast<unsigned char>(css[j + 3])) == 'l' &&
            std::tolower(static_cast<unsigned char>(css[j + 4])) == 'e') {
            usesStyleAttribute_ = true;
            return;
        }
    }
}

void CSSEngine::addStyleSheet(const std::string& css, StyleOrigin origin, const std::string& href) {
    usesHover_  = usesHover_  || css.find(":hover") != std::string::npos;
    usesActive_ = usesActive_ || css.find(":active") != std::string::npos;
    usesFocus_  = usesFocus_  || css.find(":focus") != std::string::npos;
    std::vector<std::string> chain;
    if (!href.empty()) chain.push_back(href);
    addSheetWithImports(css, origin, href, {}, chain);
}

void CSSEngine::addSheetWithImports(const std::string& css, StyleOrigin origin, const std::string& href,
                                    const std::vector<std::shared_ptr<const MediaQueryList>>& media,
                                    std::vector<std::string>& chain) {
    // Deep enough for real sites (WordPress themes import two or three levels)
    constexpr size_t kMaxImportDepth = 16;
    noteStyleAttributeSelectors(css);
    std::shared_ptr<CSSStyleSheet> sheet = parser_.parse(css);
    if (!sheet) return;
    sheet->setHref(href);

    // The imported sheets come first in the cascade, in order, as if their
    // rules were written where the @import is
    if (CSSRuleList* rules = sheet->cssRules(); rules && importFetcher_ && chain.size() < kMaxImportDepth) {
        for (size_t i = 0; i < rules->length(); i++) {
            auto* import = dynamic_cast<CSSImportRule*>(rules->item(i));
            if (!import) continue;
            if (!import->supportsCondition().empty() && !supportsCondition(import->supportsCondition())) continue;
            std::string url, text;
            if (!importFetcher_(import->href(), href.empty() ? baseUrl_ : href, url, text)) continue;
            if (std::find(chain.begin(), chain.end(), url) != chain.end()) continue;  // a cycle
            auto lists = media;
            if (!import->mediaText().empty()) lists.push_back(MediaQueryList::parse(import->mediaText()));
            chain.push_back(url);
            addSheetWithImports(text, origin, url, lists, chain);
            chain.pop_back();
        }
    }

    // The @import's media queries apply to every rule of the imported sheet
    if (!media.empty() && sheet->cssRules()) {
        for (size_t i = 0; i < sheet->cssRules()->length(); i++) {
            if (auto* rule = dynamic_cast<CSSStyleRule*>(sheet->cssRules()->item(i))) {
                auto lists = media;
                lists.insert(lists.end(), rule->media().begin(), rule->media().end());
                rule->setMedia(lists);
            } else if (auto* fontRule = dynamic_cast<CSSFontFaceRule*>(sheet->cssRules()->item(i))) {
                auto lists = media;
                lists.insert(lists.end(), fontRule->media().begin(), fontRule->media().end());
                fontRule->setMedia(lists);
            }
        }
    }
    resolver_.addStyleSheet(std::move(sheet), origin);
}

std::vector<CSSEngine::WebFontFace> CSSEngine::fontFaces() const {
    std::vector<WebFontFace> faces;
    for (const auto& entry : resolver_.styleSheets()) {
        CSSRuleList* rules = entry.sheet ? entry.sheet->cssRules() : nullptr;
        if (!rules) continue;
        std::string base = entry.sheet->href().empty() ? baseUrl_ : entry.sheet->href();
        for (size_t i = 0; i < rules->length(); i++) {
            auto* rule = dynamic_cast<CSSFontFaceRule*>(rules->item(i));
            if (!rule || !rule->valid() || !rule->mediaMatches()) continue;
            faces.push_back({rule->descriptors(), base, entry.origin});
        }
    }
    return faces;
}

void CSSEngine::addStyleSheetFromUrl(const std::string& url, StyleOrigin origin) {
    // Network fetch would go here — for now log
    std::cout << "[CSSEngine] External stylesheet: " << url << " (fetch not connected)" << std::endl;
}

void CSSEngine::syncShadowStyles(DOMDocument* document) {
    if (!document || (!shadowRootsExist() && !resolver_.hasShadowStyleSheets())) return;
    std::vector<std::pair<const DOMShadowRoot*, std::vector<std::shared_ptr<CSSStyleSheet>>>> scopes;
    std::unordered_set<std::string> used;
    // The <style> elements of one shadow tree, in tree order (not those of
    // shadow trees nested in it, nor of template contents)
    std::function<void(const DOMNode*, std::vector<std::shared_ptr<CSSStyleSheet>>&)> collectStyles =
        [&](const DOMNode* node, std::vector<std::shared_ptr<CSSStyleSheet>>& sheets) {
            for (const auto& child : node->childNodes()) {
                if (child->nodeType() != NodeType::Element) continue;
                auto* el = static_cast<const DOMElement*>(child.get());
                if (el->tagName() == "style") {
                    std::string css = el->textContent();
                    auto& sheet = shadowSheetCache_[css];
                    if (!sheet) {
                        noteStyleAttributeSelectors(css);
                        sheet = parser_.parse(css);
                    }
                    if (sheet) sheets.push_back(sheet);
                    used.insert(css);
                    continue;
                }
                collectStyles(el, sheets);
            }
        };
    std::function<void(const DOMNode*)> findTrees = [&](const DOMNode* node) {
        for (const auto& child : node->childNodes()) {
            if (child->nodeType() != NodeType::Element) continue;
            auto* el = static_cast<const DOMElement*>(child.get());
            if (const DOMShadowRoot* shadow = el->shadowRoot()) {
                std::vector<std::shared_ptr<CSSStyleSheet>> sheets;
                collectStyles(shadow, sheets);
                if (!sheets.empty()) scopes.push_back({shadow, std::move(sheets)});
                findTrees(shadow);
            }
            findTrees(el);
        }
    };
    findTrees(document);
    // Forget sheets no tree uses any more
    for (auto it = shadowSheetCache_.begin(); it != shadowSheetCache_.end();) {
        it = used.count(it->first) ? std::next(it) : shadowSheetCache_.erase(it);
    }
    resolver_.setShadowStyleSheets(std::move(scopes));
}

void CSSEngine::computeStyles() {
    if (!document_) return;
    resolver_.invalidateAll();
}

const CSSComputedStyle* CSSEngine::getComputedStyle(DOMElement* element) {
    return resolver_.getComputedStyle(element);
}

void CSSEngine::invalidate(DOMElement* element) {
    if (element) {
        resolver_.invalidateElement(element);
    } else {
        resolver_.invalidateAll();
    }
}

bool CSSEngine::supports(const std::string& property, const std::string& value) {
    // Basic @supports check
    static const std::string supportedProps[] = {
        "display", "color", "background-color", "font-size", "font-family",
        "font-weight", "font-style", "margin", "padding", "width", "height",
        "text-align", "line-height", "visibility"
    };
    for (const auto& p : supportedProps) {
        if (p == property) return true;
    }
    return false;
}

bool CSSEngine::supportsCondition(const std::string& condition) {
    return true; // Optimistic
}

std::string CSSEngine::escape(const std::string& ident) {
    return ident;
}

bool CSSEngine::supportsPropertySyntax(const std::string& syntax) {
    return true;
}

} // namespace Zepra::WebCore
