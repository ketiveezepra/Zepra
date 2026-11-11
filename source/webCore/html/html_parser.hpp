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

class HTML5TreeConstructor;

/**
 * @brief A document's own parser, run a piece at a time (HTML §13.2.1).
 *
 * The parser stops after each </script> end tag so the script runs with
 * the tree built up to that point; its driver (ScriptContext) prepares the
 * script and resumes the parser once the script ran (or, for a
 * parser-blocking external script, once it arrived and ran). While a
 * parser script runs, the insertion point is just after its end tag:
 * document.write() inserts text there, and the driver tokenizes it before
 * write() returns (pumpToInsertionPoint).
 *
 * A script-created parser (document.open()) parses into an existing
 * document; its input stays open (the insertion point is its end) until
 * close().
 *
 * Not copyable or movable: the tokenizer refers to the input string.
 */
class HTMLDocumentParser {
public:
    /// The network parser for a whole page (the input is closed)
    explicit HTMLDocumentParser(std::string html);
    /// A script-created parser building `document` (which must be empty)
    explicit HTMLDocumentParser(DOMDocument* document);
    ~HTMLDocumentParser();
    HTMLDocumentParser(const HTMLDocumentParser&) = delete;
    HTMLDocumentParser& operator=(const HTMLDocumentParser&) = delete;

    enum class Stop {
        Script,      ///< a </script> was parsed: takePausedScript()
        NeedInput,   ///< at the insertion point / the end of open input
        Done         ///< the end of the input was parsed
    };
    /// Parses as far as the input goes (pauses at scripts)
    Stop pump();
    /// Parses up to the insertion point (pauses at scripts); with no
    /// insertion point defined, like pump()
    Stop pumpToInsertionPoint();
    /// The script whose end tag stopped the last pump, once
    DOMElement* takePausedScript();

    /// The insertion point: defined (at the current position) while a
    /// parser-inserted script the parser stopped for runs. A
    /// script-created parser always has one, at the end of its input.
    void defineInsertionPoint();
    void undefineInsertionPoint();
    bool hasInsertionPoint() const;
    /// Where the insertion point is (an offset into the input)
    size_t insertionPointOffset() const { return insertionPoint_; }
    /// Puts back an insertion point saved before a nested script ran
    void restoreInsertionPoint(size_t offset);
    /// document.write: puts `text` into the input at the insertion point
    /// (after what earlier writes put there). False without one.
    bool insertAtInsertionPoint(const std::string& text);
    /// document.close: the input ends after what was written so far
    void closeInput();

    bool done() const { return done_; }
    bool scriptCreated() const { return scriptCreated_; }
    DOMDocument* document() const;
    /// Ownership of the document the network parser made; the parse can
    /// go on (the caller keeps the document alive while it does)
    std::unique_ptr<DOMDocument> releaseDocument();
    /// Elements the parser will still insert into or refer to (they must
    /// stay alive when a script removes them)
    std::vector<DOMElement*> referencedElements() const;
    /// Characters of input consumed so far (for tracing)
    size_t position() const;
    size_t inputSize() const { return input_.size(); }

private:
    Stop run(size_t limit);

    std::string input_;
    std::unique_ptr<HTML5TreeConstructor> tree_;
    bool scriptCreated_ = false;
    bool done_ = false;
    bool insertionPointDefined_ = false;
    size_t insertionPoint_ = 0;
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
