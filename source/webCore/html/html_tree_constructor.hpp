// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file html_tree_constructor.hpp
 * @brief HTML5 Tree Construction (WHATWG HTML §13.2.6)
 *
 * Processes tokens from HTML5Tokenizer and builds a DOMDocument tree.
 *
 * Implements:
 *  - All 18 insertion modes
 *  - Open elements stack
 *  - Active formatting elements list (for adoption agency algorithm)
 *  - Foster parenting (for table misnesting)
 *  - Script/style handling
 *  - DOCTYPE detection and quirks mode
 */

#pragma once

#include "html/html_tokenizer.hpp"
#include "browser/dom.hpp"
#include <memory>
#include <vector>
#include <string>
#include <unordered_set>

namespace Zepra::WebCore {

// ============================================================================
// Insertion Modes (WHATWG HTML §13.2.6.1)
// ============================================================================

enum class InsertionMode {
    Initial,
    BeforeHTML,
    BeforeHead,
    InHead,
    InHeadNoscript,
    AfterHead,
    InBody,
    Text,
    InTable,
    InTableText,
    InCaption,
    InColumnGroup,
    InTableBody,
    InRow,
    InCell,
    InSelect,
    InSelectInTable,
    InTemplate,
    AfterBody,
    InFrameset,
    AfterFrameset,
    AfterAfterBody,
    AfterAfterFrameset
};

// ============================================================================
// Scope Sets (for isInScope checks)
// ============================================================================

// "Scope" is a set of elements that delimit scope — used by isElementInScope()
enum class ScopeType {
    Default,        // table, template, html, applet, marquee, object, desc, foreignObj
    ListItem,       // adds: ol, ul
    Button,         // adds: button
    Table,          // html, table, template
    Select          // everything except optgroup, option
};

// ============================================================================
// HTML5TreeConstructor
// ============================================================================

/**
 * @brief Production-quality HTML5 tree constructor
 *
 * Usage:
 *   HTML5TreeConstructor ctor;
 *   auto doc = ctor.construct(html);
 */
class HTML5TreeConstructor {
public:
    HTML5TreeConstructor();

    /**
     * @brief Parse HTML string and return constructed DOM document
     */
    std::unique_ptr<DOMDocument> construct(const std::string& html);

    /**
     * @brief Process a single token (called by tokenizer callback)
     */
    void processToken(HTML5Token token);

    /**
     * @brief Access the tokenizer (for state switching during construction)
     */
    HTML5Tokenizer* tokenizer() { return tokenizer_; }

private:
    // ─── Document / state ─────────────────────────────────────────────────
    std::unique_ptr<DOMDocument> doc_;
    DOMElement* headElement_ = nullptr;
    DOMElement* formElement_ = nullptr;
    InsertionMode mode_ = InsertionMode::Initial;
    InsertionMode originalMode_ = InsertionMode::Initial; // saved for text mode
    bool quirksMode_ = false;
    bool framesetOk_ = true;
    bool scriptingEnabled_ = false; // conservative default
    bool fosterParenting_ = false;

    // ─── Tokenizer reference (set during construct()) ─────────────────────
    HTML5Tokenizer* tokenizer_ = nullptr;

    // ─── Open elements stack ──────────────────────────────────────────────
    // Per WHATWG: "a stack of open elements"
    std::vector<DOMElement*> openElements_;

    DOMElement* currentNode() const {
        return openElements_.empty() ? nullptr : openElements_.back();
    }

    DOMElement* adjustedCurrentNode() const {
        // In template, the adjusted current node is the template's content
        return currentNode();
    }

    void pushOpenElement(DOMElement* el) { openElements_.push_back(el); }

    void popOpenElement() {
        if (!openElements_.empty()) openElements_.pop_back();
    }

    // Pop until a given tag is found and popped
    void popUntilTag(const std::string& tag);
    // Pop until any of the given tags
    void popUntilAnyTag(const std::vector<std::string>& tags);

    // ─── Active formatting elements ───────────────────────────────────────
    // Per WHATWG: list of active formatting elements (for adoption agency)
    struct FormattingEntry {
        DOMElement* element;
        HTML5Token token;   // the token that created it
        bool isMarker;      // scope markers (table boundaries, etc.)
    };
    std::vector<FormattingEntry> activeFormattingElements_;

    void pushActiveFormatting(DOMElement* el, HTML5Token tok);
    void pushFormattingMarker();
    void reconstructActiveFormattingElements();
    void clearFormattingToLastMarker();
    DOMElement* findFormattingElement(const std::string& tag);

    // ─── Node insertion ───────────────────────────────────────────────────

    /**
     * @brief Create and insert an element for a token
     * Returns the newly inserted element
     */
    DOMElement* insertElementForToken(const HTML5Token& token);

    /**
     * @brief Insert element at appropriate place (handling foster parenting)
     */
    DOMElement* insertElement(const std::string& tagName,
                              const std::vector<std::pair<std::string,std::string>>& attrs);

    /**
     * @brief Insert character data at appropriate insertion point
     */
    void insertCharacter(char32_t c);
    void insertCharacters(const std::string& text);

    /**
     * @brief Insert comment
     */
    void insertComment(const std::string& data, DOMNode* parent = nullptr);

    /**
     * @brief Get appropriate insertion location (for foster parenting)
     * Returns {parent, beforeSibling}
     */
    std::pair<DOMNode*, DOMNode*> appropriateInsertionLocation();

    // ─── Scope checks ─────────────────────────────────────────────────────

    bool isElementInScope(const std::string& tag,
                          ScopeType scope = ScopeType::Default) const;
    bool isElementInButtonScope(const std::string& tag) const;
    bool isElementInTableScope(const std::string& tag) const;
    bool isElementInListItemScope(const std::string& tag) const;
    bool isElementInSelectScope(const std::string& tag) const;

    // Check if the stack has an element with given tag
    bool hasElementInOpenStack(const std::string& tag) const;
    DOMElement* findElementInOpenStack(const std::string& tag) const;

    // ─── Adoption Agency Algorithm ────────────────────────────────────────

    /**
     * @brief Run the adoption agency algorithm for an end tag
     * @see WHATWG HTML §13.2.8.6 - "An end tag whose tag name is one of: a, b, big, code, em..."
     */
    void adoptionAgencyAlgorithm(const std::string& tagName);

    // ─── Insertion mode processors ────────────────────────────────────────

    void processInitial(HTML5Token& tok);
    void processBeforeHTML(HTML5Token& tok);
    void processBeforeHead(HTML5Token& tok);
    void processInHead(HTML5Token& tok);
    void processInHeadNoscript(HTML5Token& tok);
    void processAfterHead(HTML5Token& tok);
    void processInBody(HTML5Token& tok);
    void processText(HTML5Token& tok);
    void processInTable(HTML5Token& tok);
    void processInTableText(HTML5Token& tok);
    void processInCaption(HTML5Token& tok);
    void processInColumnGroup(HTML5Token& tok);
    void processInTableBody(HTML5Token& tok);
    void processInRow(HTML5Token& tok);
    void processInCell(HTML5Token& tok);
    void processInSelect(HTML5Token& tok);
    void processInSelectInTable(HTML5Token& tok);
    void processInTemplate(HTML5Token& tok);
    void processAfterBody(HTML5Token& tok);
    void processInFrameset(HTML5Token& tok);
    void processAfterFrameset(HTML5Token& tok);
    void processAfterAfterBody(HTML5Token& tok);
    void processAfterAfterFrameset(HTML5Token& tok);

    // ─── Helpers ──────────────────────────────────────────────────────────

    /** Handle a start tag in "in body" mode (bulk of the logic) */
    void inBodyStartTag(HTML5Token& tok);
    /** Handle an end tag in "in body" mode */
    void inBodyEndTag(HTML5Token& tok);

    /** Close a <p> element if one is in button scope */
    void closePElement();

    /** Generate implied end tags (except possibly for a specific tag) */
    void generateImpliedEndTags(const std::string& exclude = "");

    /** Check if tag is a formatting element (b, i, a, em, strong, etc.) */
    static bool isFormattingElement(const std::string& tag);

    /** Check if tag is a void element (no end tag) */
    static bool isVoidElement(const std::string& tag);

    /** Check if tag is a raw-text element (script, style) */
    static bool isRawTextElement(const std::string& tag);

    /** Check if tag is a RCDATA element (textarea, title) */
    static bool isRCDATAElement(const std::string& tag);

    /** Check if tag is in "special" category */
    static bool isSpecialElement(const std::string& tag);

    /** All tags that trigger scope boundaries for default scope */
    static bool isScopeBoundary(const std::string& tag, ScopeType scope);

    // Table-related accumulated character data
    std::string pendingTableChars_;
    bool pendingTableCharsHaveNonWhitespace_ = false;

    // Template insertion mode stack
    std::vector<InsertionMode> templateInsertionModes_;

    // Reset insertion mode based on current open element stack
    void resetInsertionModeAppropriately();
};

} // namespace Zepra::WebCore
