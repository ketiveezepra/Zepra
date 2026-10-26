#pragma once

/**
 * @file html_parser.hpp
 * @brief HTML parsing and DOM construction
 */

#include "browser/dom.hpp"
#include <string>
#include <stack>
#include <memory>
#include <vector>

namespace Zepra::WebCore {

/**
 * @brief HTML token types
 */
enum class HTMLTokenType {
    DOCTYPE,
    StartTag,
    EndTag,
    Text,
    Comment,
    EndOfFile
};

/**
 * @brief HTML token
 */
struct HTMLToken {
    HTMLTokenType type;
    std::string name;           // Tag name or doctype name
    std::string data;           // Text or comment data
    std::vector<std::pair<std::string, std::string>> attributes;
    bool selfClosing = false;
};

/**
 * @brief HTML tokenizer (lexer)
 */
class HTMLTokenizer {
public:
    explicit HTMLTokenizer(const std::string& html);
    
    HTMLToken nextToken();
    bool hasMoreTokens() const;
    
private:
    void consumeWhitespace();
    std::string consumeTagName();
    std::string consumeAttributeName();
    std::string consumeAttributeValue();
    std::string consumeText();
    std::string consumeComment();
    std::string consumeDoctype();
    std::string consumeRawText(const std::string& endTag);
    static std::string decodeEntities(const std::string& text);
    
    char current() const;
    char peek(size_t offset = 1) const;
    void advance(size_t count = 1);
    bool match(const std::string& str) const;
    
    std::string html_;
    size_t pos_ = 0;
};

/**
 * @brief HTML parser (builds DOM from tokens)
 */
class HTMLParser {
public:
    /**
     * @brief Parse HTML string into DOM document
     */
    std::unique_ptr<DOMDocument> parse(const std::string& html);
    
    /**
     * @brief Parse HTML fragment into element
     */
    std::unique_ptr<DOMElement> parseFragment(const std::string& html);

    /**
     * @brief Parse HTML as the content of an element named `contextTag`
     * (innerHTML): the top-level nodes, without a parent
     */
    std::vector<std::unique_ptr<DOMNode>> parseFragmentNodes(const std::string& html,
                                                             const std::string& contextTag);
    
private:
    // Tree construction
    void processToken(const HTMLToken& token);
    void processStartTag(const HTMLToken& token);
    void processEndTag(const HTMLToken& token);
    void processText(const HTMLToken& token);
    void processComment(const HTMLToken& token);
    
    // Stack operations
    DOMElement* currentElement() const;
    void pushElement(std::unique_ptr<DOMElement> element);
    void popElement();
    bool isElementInScope(const std::string& tagName) const;
    
    // Special element handling
    bool isVoidElement(const std::string& tagName) const;
    bool isFormattingElement(const std::string& tagName) const;
    void adoptionAgencyAlgorithm(const std::string& tagName);
    
    // Foster parenting (for table elements)
    void fosterParent(std::unique_ptr<DOMNode> node);
    
    std::unique_ptr<DOMDocument> document_;
    std::stack<DOMElement*> openElements_;
    std::vector<DOMElement*> activeFormattingElements_;
};

/**
 * @brief HTML serializer (DOM to string)
 */
class HTMLSerializer {
public:
    std::string serialize(DOMNode* node);
    std::string serializeElement(DOMElement* element);
    std::string serializeText(DOMText* text);
    
private:
    static std::string escapeHTML(const std::string& text);
    static std::string escapeAttribute(const std::string& value);
};

} // namespace Zepra::WebCore
