// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file html_parser.cpp
 * @brief HTML parser — integrates HTML5Tokenizer + HTML5TreeConstructor
 *
 * This file is the public API surface. Internally it delegates to the
 * spec-compliant HTML5 tokenizer and tree constructor.
 */

#include "html/html_parser.hpp"
#include "html/html_tokenizer.hpp"
#include "html/html_tree_constructor.hpp"
#include <algorithm>
#include <cctype>

namespace Zepra::WebCore {

// ============================================================================
// HTMLParser — public API, delegates to production implementation
// ============================================================================

std::unique_ptr<DOMDocument> HTMLParser::parse(const std::string& html) {
    HTML5TreeConstructor constructor;
    document_ = constructor.construct(html);
    return std::move(document_);
}

std::unique_ptr<DOMElement> HTMLParser::parseFragment(const std::string& html) {
    // Fragment parsing: wrap in a context element and strip it
    std::string wrapped = "<div>" + html + "</div>";
    HTML5TreeConstructor constructor;
    auto doc = constructor.construct(wrapped);
    if (!doc) return nullptr;

    // Find the div wrapper and return its first child
    if (doc->body()) {
        for (auto& child : doc->body()->childNodes()) {
            if (child->nodeType() == NodeType::Element) {
                DOMElement* divEl = static_cast<DOMElement*>(child.get());
                if (divEl->tagName() == "div") {
                    if (!divEl->childNodes().empty()) {
                        auto clone = divEl->childNodes().front()->cloneNode(true);
                        if (clone && clone->nodeType() == NodeType::Element)
                            return std::unique_ptr<DOMElement>(
                                static_cast<DOMElement*>(clone.release()));
                    }
                }
            }
        }
    }
    return nullptr;
}

// ============================================================================
// Legacy HTMLTokenizer — kept for compatibility
// ============================================================================

HTMLTokenizer::HTMLTokenizer(const std::string& html) : html_(html), pos_(0) {}

bool HTMLTokenizer::hasMoreTokens() const {
    return pos_ < html_.length();
}

char HTMLTokenizer::current() const {
    return pos_ < html_.length() ? html_[pos_] : '\0';
}

char HTMLTokenizer::peek(size_t offset) const {
    return (pos_ + offset) < html_.length() ? html_[pos_ + offset] : '\0';
}

void HTMLTokenizer::advance(size_t count) {
    pos_ += count;
}

bool HTMLTokenizer::match(const std::string& str) const {
    return html_.compare(pos_, str.length(), str) == 0;
}

void HTMLTokenizer::consumeWhitespace() {
    while (pos_ < html_.length() && std::isspace(html_[pos_])) {
        ++pos_;
    }
}

std::string HTMLTokenizer::consumeTagName() {
    std::string name;
    while (pos_ < html_.length() && !std::isspace(html_[pos_]) && 
           html_[pos_] != '>' && html_[pos_] != '/') {
        name += std::tolower(html_[pos_++]);
    }
    return name;
}

std::string HTMLTokenizer::consumeAttributeName() {
    std::string name;
    while (pos_ < html_.length() && !std::isspace(html_[pos_]) && 
           html_[pos_] != '=' && html_[pos_] != '>' && html_[pos_] != '/') {
        name += std::tolower(html_[pos_++]);
    }
    return name;
}

std::string HTMLTokenizer::consumeAttributeValue() {
    consumeWhitespace();
    if (current() == '=') {
        advance();
        consumeWhitespace();
    }
    
    std::string value;
    char quote = current();
    if (quote == '"' || quote == '\'') {
        advance();
        while (pos_ < html_.length() && current() != quote) {
            value += html_[pos_++];
        }
        if (current() == quote) advance();
    } else {
        while (pos_ < html_.length() && !std::isspace(current()) && current() != '>') {
            value += html_[pos_++];
        }
    }
    return value;
}

std::string HTMLTokenizer::consumeText() {
    std::string text;
    while (pos_ < html_.length() && current() != '<') {
        text += html_[pos_++];
    }
    return decodeEntities(text);
}

std::string HTMLTokenizer::consumeComment() {
    std::string comment;
    while (pos_ < html_.length() && !match("-->")) {
        comment += html_[pos_++];
    }
    if (match("-->")) advance(3);
    return comment;
}

std::string HTMLTokenizer::consumeDoctype() {
    std::string doctype;
    while (pos_ < html_.length() && current() != '>') {
        doctype += html_[pos_++];
    }
    if (current() == '>') advance();
    return doctype;
}

std::string HTMLTokenizer::consumeRawText(const std::string& endTag) {
    std::string text;
    std::string lower;
    while (pos_ < html_.length()) {
        if (current() == '<' && peek() == '/') {
            lower.clear();
            for (size_t i = 2; i < endTag.length() + 3 && (pos_ + i) < html_.length(); ++i) {
                lower += std::tolower(html_[pos_ + i]);
            }
            if (lower.find(endTag) == 0) break;
        }
        text += html_[pos_++];
    }
    return text;
}

std::string HTMLTokenizer::decodeEntities(const std::string& text) {
    std::string result;
    for (size_t i = 0; i < text.length(); ++i) {
        if (text[i] == '&') {
            size_t end = text.find(';', i);
            if (end != std::string::npos) {
                std::string entity = text.substr(i + 1, end - i - 1);
                if (entity == "lt") result += '<';
                else if (entity == "gt") result += '>';
                else if (entity == "amp") result += '&';
                else if (entity == "quot") result += '"';
                else if (entity == "apos") result += '\'';
                else if (entity == "nbsp") { result += '\xC2'; result += '\xA0'; }
                else result += text.substr(i, end - i + 1);
                i = end;
                continue;
            }
        }
        result += text[i];
    }
    return result;
}

HTMLToken HTMLTokenizer::nextToken() {
    HTMLToken token;
    
    if (!hasMoreTokens()) {
        token.type = HTMLTokenType::EndOfFile;
        return token;
    }
    
    if (current() != '<') {
        token.type = HTMLTokenType::Text;
        token.data = consumeText();
        return token;
    }
    
    advance(); // Skip '<'
    
    if (match("!--")) {
        advance(3);
        token.type = HTMLTokenType::Comment;
        token.data = consumeComment();
        return token;
    }
    
    if (match("!DOCTYPE") || match("!doctype")) {
        advance(8);
        token.type = HTMLTokenType::DOCTYPE;
        token.data = consumeDoctype();
        return token;
    }
    
    if (current() == '/') {
        advance();
        token.type = HTMLTokenType::EndTag;
        token.name = consumeTagName();
        consumeWhitespace();
        if (current() == '>') advance();
        return token;
    }
    
    token.type = HTMLTokenType::StartTag;
    token.name = consumeTagName();
    
    while (pos_ < html_.length() && current() != '>' && current() != '/') {
        consumeWhitespace();
        if (current() == '>' || current() == '/') break;
        
        std::string attrName = consumeAttributeName();
        consumeWhitespace();
        std::string attrValue;
        if (current() == '=') {
            attrValue = consumeAttributeValue();
        }
        if (!attrName.empty()) {
            token.attributes.emplace_back(attrName, attrValue);
        }
    }
    
    if (current() == '/') {
        token.selfClosing = true;
        advance();
    }
    if (current() == '>') advance();
    
    return token;
}

// ============================================================================
// Process Token (legacy path)
// ============================================================================

void HTMLParser::processToken(const HTMLToken& token) {
    switch (token.type) {
        case HTMLTokenType::StartTag:  processStartTag(token); break;
        case HTMLTokenType::EndTag:    processEndTag(token); break;
        case HTMLTokenType::Text:      processText(token); break;
        case HTMLTokenType::Comment:   processComment(token); break;
        default: break;
    }
}

void HTMLParser::processStartTag(const HTMLToken& token) {
    auto element = std::make_unique<DOMElement>(token.name);
    for (const auto& [name, value] : token.attributes)
        element->setAttribute(name, value);
    element->setOwnerDocument(document_.get());
    DOMElement* elemPtr = element.get();

    if (openElements_.empty()) {
        document_->appendChild(std::move(element));
        if (!document_->documentElement())
            document_->setDocumentElement(elemPtr);
    } else {
        currentElement()->appendChild(std::move(element));
    }
    if (!token.selfClosing && !isVoidElement(token.name))
        openElements_.push(elemPtr);
}

void HTMLParser::processEndTag(const HTMLToken& token) {
    if (!openElements_.empty() && currentElement()->tagName() == token.name) {
        openElements_.pop();
    }
}

void HTMLParser::processText(const HTMLToken& token) {
    if (token.data.empty()) return;
    auto text = std::make_unique<DOMText>(token.data);
    text->setOwnerDocument(document_.get());
    if (!openElements_.empty())
        currentElement()->appendChild(std::move(text));
    else
        document_->appendChild(std::move(text));
}

void HTMLParser::processComment(const HTMLToken& token) {
    auto comment = std::make_unique<DOMComment>(token.data);
    comment->setOwnerDocument(document_.get());
    if (!openElements_.empty())
        currentElement()->appendChild(std::move(comment));
    else
        document_->appendChild(std::move(comment));
}

DOMElement* HTMLParser::currentElement() const {
    return openElements_.empty() ? nullptr : openElements_.top();
}

void HTMLParser::pushElement(std::unique_ptr<DOMElement> element) {
    openElements_.push(element.get());
}

void HTMLParser::popElement() {
    if (!openElements_.empty()) openElements_.pop();
}

bool HTMLParser::isElementInScope(const std::string& tagName) const {
    return false;
}

bool HTMLParser::isVoidElement(const std::string& tagName) const {
    static const std::vector<std::string> voidElements = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr"
    };
    std::string lower = tagName;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return std::find(voidElements.begin(), voidElements.end(), lower) != voidElements.end();
}

bool HTMLParser::isFormattingElement(const std::string& tagName) const {
    return false;
}

void HTMLParser::adoptionAgencyAlgorithm(const std::string& tagName) {}

void HTMLParser::fosterParent(std::unique_ptr<DOMNode> node) {}

// ============================================================================
// HTMLSerializer
// ============================================================================

std::string HTMLSerializer::serialize(DOMNode* node) {
    if (!node) return "";
    if (node->nodeType() == NodeType::Element)
        return serializeElement(static_cast<DOMElement*>(node));
    else if (node->nodeType() == NodeType::Text)
        return serializeText(static_cast<DOMText*>(node));
    return "";
}

std::string HTMLSerializer::serializeElement(DOMElement* element) {
    std::string result = "<" + element->tagName();
    for (const auto& [name, value] : element->attributes())
        result += " " + name + "=\"" + escapeAttribute(value) + "\"";
    result += ">";
    for (const auto& child : element->childNodes())
        result += serialize(child.get());
    result += "</" + element->tagName() + ">";
    return result;
}

std::string HTMLSerializer::serializeText(DOMText* text) {
    return escapeHTML(text->data());
}

std::string HTMLSerializer::escapeHTML(const std::string& text) {
    std::string result;
    for (char c : text) {
        switch (c) {
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '&': result += "&amp;"; break;
            default:  result += c;
        }
    }
    return result;
}

std::string HTMLSerializer::escapeAttribute(const std::string& value) {
    std::string result;
    for (char c : value) {
        switch (c) {
            case '"': result += "&quot;"; break;
            case '&': result += "&amp;"; break;
            default:  result += c;
        }
    }
    return result;
}

} // namespace Zepra::WebCore
