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
    // Skip the block
    if (match('{')) {
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

std::unique_ptr<CSSKeyframesRule> CSSParser::parseKeyframesRule() {
    auto rule = std::make_unique<CSSKeyframesRule>();
    // Skip animation name
    while (!eof() && peek() != '{') consume();
    // Skip block
    if (match('{')) {
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

std::unique_ptr<CSSImportRule> CSSParser::parseImportRule() {
    // Skip to semicolon
    while (!eof() && peek() != ';') consume();
    if (peek() == ';') consume();
    return std::make_unique<CSSImportRule>();
}

std::unique_ptr<CSSRule> CSSParser::parseRule() {
    skipWhitespace();
    if (eof()) return nullptr;
    
    // Check for at-rules
    if (peek() == '@') {
        consume();
        std::string atWord = parseIdentifier();
        
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
            
            if (atWord == "layer" || atWord == "supports" || 
                atWord == "theme" || atWord == "property") {
                // Skip name/condition
                while (!eof() && peek() != '{' && peek() != ';') consume();
                if (peek() == ';') {
                    consume(); // Forward declaration: @layer name;
                    continue;
                }
                if (peek() == '{') {
                    consume(); // Enter the block
                    parseRulesInto(sheet, depth + 1); // Recursively parse inner rules
                    continue;
                }
            }
            
            // Not a recursive at-rule — restore position and use normal parseRule
            pos_ = savedPos;
        }
        
        auto rule = parseRule();
        if (rule && sheet->cssRules()) {
            sheet->cssRules()->add(std::move(rule));
        }
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

void CSSEngine::addStyleSheet(const std::string& css, StyleOrigin origin, const std::string& href) {
    usesHover_  = usesHover_  || css.find(":hover") != std::string::npos;
    usesActive_ = usesActive_ || css.find(":active") != std::string::npos;
    usesFocus_  = usesFocus_  || css.find(":focus") != std::string::npos;
    auto sheet = parser_.parse(css);
    if (sheet) {
        sheet->setHref(href);
        resolver_.addStyleSheet(std::move(sheet), origin);
    }
}

void CSSEngine::addStyleSheetFromUrl(const std::string& url, StyleOrigin origin) {
    // Network fetch would go here — for now log
    std::cout << "[CSSEngine] External stylesheet: " << url << " (fetch not connected)" << std::endl;
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
