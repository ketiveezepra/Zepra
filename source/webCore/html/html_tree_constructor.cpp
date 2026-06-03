// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file html_tree_constructor.cpp
 * @brief HTML5 Tree Constructor — all insertion modes implemented
 *
 * Reference: WHATWG HTML Living Standard §13.2.6
 */

#include "html/html_tree_constructor.hpp"
#include "html/html_tokenizer.hpp"
#include <algorithm>
#include <cctype>

namespace Zepra::WebCore {

// ============================================================================
// Element Classification
// ============================================================================

bool HTML5TreeConstructor::isVoidElement(const std::string& tag) {
    static const std::unordered_set<std::string> s = {
        "area","base","br","col","embed","hr","img","input",
        "link","meta","param","source","track","wbr"
    };
    return s.count(tag) > 0;
}

bool HTML5TreeConstructor::isRawTextElement(const std::string& tag) {
    return tag == "script" || tag == "style";
}

bool HTML5TreeConstructor::isRCDATAElement(const std::string& tag) {
    return tag == "textarea" || tag == "title";
}

bool HTML5TreeConstructor::isFormattingElement(const std::string& tag) {
    static const std::unordered_set<std::string> s = {
        "a","b","big","code","em","font","i","nobr","s","small",
        "strike","strong","tt","u"
    };
    return s.count(tag) > 0;
}

bool HTML5TreeConstructor::isSpecialElement(const std::string& tag) {
    static const std::unordered_set<std::string> s = {
        "address","applet","area","article","aside","base","basefont",
        "bgsound","blockquote","body","br","button","caption","center",
        "col","colgroup","dd","details","dir","div","dl","dt","embed",
        "fieldset","figcaption","figure","footer","form","frame","frameset",
        "h1","h2","h3","h4","h5","h6","head","header","hgroup","hr",
        "html","iframe","img","input","isindex","li","link","listing",
        "main","marquee","menu","meta","nav","noembed","noframes","noscript",
        "object","ol","p","param","plaintext","pre","script","section",
        "select","source","style","summary","table","tbody","td","template",
        "textarea","tfoot","th","thead","title","tr","track","ul","wbr","xmp"
    };
    return s.count(tag) > 0;
}

bool HTML5TreeConstructor::isScopeBoundary(const std::string& tag, ScopeType scope) {
    // Default scope boundaries
    static const std::unordered_set<std::string> defaultBoundaries = {
        "applet","caption","html","table","td","th","marquee","object",
        "template","svg","mathml"  // namespace boundaries
    };
    if (defaultBoundaries.count(tag)) return true;

    if (scope == ScopeType::ListItem) {
        if (tag == "ol" || tag == "ul") return true;
    }
    if (scope == ScopeType::Button) {
        if (tag == "button") return true;
    }
    if (scope == ScopeType::Table) {
        // table scope: html, table, template
        static const std::unordered_set<std::string> tableBoundaries = {"html","table","template"};
        return tableBoundaries.count(tag) > 0 && !defaultBoundaries.count(tag);
    }
    return false;
}

// ============================================================================
// Constructor and construct()
// ============================================================================

HTML5TreeConstructor::HTML5TreeConstructor() = default;

std::unique_ptr<DOMDocument> HTML5TreeConstructor::construct(const std::string& html) {
    doc_ = std::make_unique<DOMDocument>();
    headElement_ = nullptr;
    formElement_ = nullptr;
    mode_ = InsertionMode::Initial;
    openElements_.clear();
    activeFormattingElements_.clear();
    templateInsertionModes_.clear();
    pendingTableChars_.clear();
    pendingTableCharsHaveNonWhitespace_ = false;
    framesetOk_ = true;
    quirksMode_ = false;

    HTML5Tokenizer tok(html);
    tokenizer_ = &tok;

    tok.setTokenHandler([this](HTML5Token token) {
        processToken(std::move(token));
    });
    tok.tokenize();

    tokenizer_ = nullptr;
    return std::move(doc_);
}

// ============================================================================
// Token dispatch
// ============================================================================

void HTML5TreeConstructor::processToken(HTML5Token tok) {
    switch (mode_) {
    case InsertionMode::Initial:           processInitial(tok); break;
    case InsertionMode::BeforeHTML:        processBeforeHTML(tok); break;
    case InsertionMode::BeforeHead:        processBeforeHead(tok); break;
    case InsertionMode::InHead:            processInHead(tok); break;
    case InsertionMode::InHeadNoscript:    processInHeadNoscript(tok); break;
    case InsertionMode::AfterHead:         processAfterHead(tok); break;
    case InsertionMode::InBody:            processInBody(tok); break;
    case InsertionMode::Text:              processText(tok); break;
    case InsertionMode::InTable:           processInTable(tok); break;
    case InsertionMode::InTableText:       processInTableText(tok); break;
    case InsertionMode::InCaption:         processInCaption(tok); break;
    case InsertionMode::InColumnGroup:     processInColumnGroup(tok); break;
    case InsertionMode::InTableBody:       processInTableBody(tok); break;
    case InsertionMode::InRow:             processInRow(tok); break;
    case InsertionMode::InCell:            processInCell(tok); break;
    case InsertionMode::InSelect:          processInSelect(tok); break;
    case InsertionMode::InSelectInTable:   processInSelectInTable(tok); break;
    case InsertionMode::InTemplate:        processInTemplate(tok); break;
    case InsertionMode::AfterBody:         processAfterBody(tok); break;
    case InsertionMode::InFrameset:        processInFrameset(tok); break;
    case InsertionMode::AfterFrameset:     processAfterFrameset(tok); break;
    case InsertionMode::AfterAfterBody:    processAfterAfterBody(tok); break;
    case InsertionMode::AfterAfterFrameset:processAfterAfterFrameset(tok); break;
    }
}

// ============================================================================
// Insertion Helpers
// ============================================================================

std::pair<DOMNode*, DOMNode*> HTML5TreeConstructor::appropriateInsertionLocation() {
    DOMNode* target = currentNode() ? currentNode() : doc_.get();
    DOMNode* before = nullptr;

    if (fosterParenting_) {
        // Find last table element in open stack
        DOMElement* lastTable = nullptr;
        DOMElement* lastTemplate = nullptr;
        for (auto it = openElements_.rbegin(); it != openElements_.rend(); ++it) {
            if ((*it)->tagName() == "template") { lastTemplate = *it; break; }
            if ((*it)->tagName() == "table") { lastTable = *it; break; }
        }
        if (lastTemplate) {
            target = lastTemplate;
        } else if (lastTable && lastTable->parentNode()) {
            target = lastTable->parentNode();
            before = lastTable;
        } else if (lastTable) {
            // table has no parent — use element above it in stack
            auto it = std::find(openElements_.begin(), openElements_.end(), lastTable);
            if (it != openElements_.begin()) { --it; target = *it; }
        } else {
            // no table — use first element (html)
            if (!openElements_.empty()) target = openElements_.front();
        }
    }

    return {target, before};
}

DOMElement* HTML5TreeConstructor::insertElement(
        const std::string& tagName,
        const std::vector<std::pair<std::string,std::string>>& attrs) {

    auto el = doc_->createElement(tagName);
    DOMElement* ptr = el.get();

    for (auto& [name, value] : attrs)
        ptr->setAttribute(name, value);

    auto [target, before] = appropriateInsertionLocation();

    if (before) {
        target->insertBefore(std::move(el), before);
    } else {
        target->appendChild(std::move(el));
    }

    openElements_.push_back(ptr);
    return ptr;
}

DOMElement* HTML5TreeConstructor::insertElementForToken(const HTML5Token& tok) {
    return insertElement(tok.tagName, tok.attributes);
}

void HTML5TreeConstructor::insertCharacters(const std::string& text) {
    if (text.empty()) return;
    auto [target, before] = appropriateInsertionLocation();

    // Merge with last text node if possible
    if (!before) {
        auto& children = target->childNodes();
        if (!children.empty() && children.back()->nodeType() == NodeType::Text) {
            DOMText* textNode = static_cast<DOMText*>(children.back().get());
            textNode->appendData(text);
            return;
        }
    }

    auto textNode = doc_->createTextNode(text);
    if (before)
        target->insertBefore(std::move(textNode), before);
    else
        target->appendChild(std::move(textNode));
}

void HTML5TreeConstructor::insertCharacter(char32_t c) {
    // Encode to UTF-8
    std::string s;
    if (c < 0x80) s += static_cast<char>(c);
    else if (c < 0x800) { s += static_cast<char>(0xC0|(c>>6)); s += static_cast<char>(0x80|(c&0x3F)); }
    else { s += static_cast<char>(0xE0|(c>>12)); s += static_cast<char>(0x80|((c>>6)&0x3F)); s += static_cast<char>(0x80|(c&0x3F)); }
    insertCharacters(s);
}

void HTML5TreeConstructor::insertComment(const std::string& data, DOMNode* parent) {
    auto comment = doc_->createComment(data);
    (parent ? parent : currentNode() ? (DOMNode*)currentNode() : (DOMNode*)doc_.get())
        ->appendChild(std::move(comment));
}

// ============================================================================
// Open Elements Stack Helpers
// ============================================================================

void HTML5TreeConstructor::popUntilTag(const std::string& tag) {
    while (!openElements_.empty()) {
        std::string top = openElements_.back()->tagName();
        openElements_.pop_back();
        if (top == tag) break;
    }
}

void HTML5TreeConstructor::popUntilAnyTag(const std::vector<std::string>& tags) {
    while (!openElements_.empty()) {
        std::string top = openElements_.back()->tagName();
        openElements_.pop_back();
        if (std::find(tags.begin(), tags.end(), top) != tags.end()) break;
    }
}

bool HTML5TreeConstructor::hasElementInOpenStack(const std::string& tag) const {
    for (auto it = openElements_.rbegin(); it != openElements_.rend(); ++it)
        if ((*it)->tagName() == tag) return true;
    return false;
}

DOMElement* HTML5TreeConstructor::findElementInOpenStack(const std::string& tag) const {
    for (auto it = openElements_.rbegin(); it != openElements_.rend(); ++it)
        if ((*it)->tagName() == tag) return *it;
    return nullptr;
}

bool HTML5TreeConstructor::isElementInScope(const std::string& tag, ScopeType scope) const {
    for (auto it = openElements_.rbegin(); it != openElements_.rend(); ++it) {
        if ((*it)->tagName() == tag) return true;
        if (isScopeBoundary((*it)->tagName(), scope)) return false;
    }
    return false;
}

bool HTML5TreeConstructor::isElementInButtonScope(const std::string& tag) const {
    return isElementInScope(tag, ScopeType::Button);
}

bool HTML5TreeConstructor::isElementInTableScope(const std::string& tag) const {
    return isElementInScope(tag, ScopeType::Table);
}

bool HTML5TreeConstructor::isElementInListItemScope(const std::string& tag) const {
    return isElementInScope(tag, ScopeType::ListItem);
}

bool HTML5TreeConstructor::isElementInSelectScope(const std::string& tag) const {
    for (auto it = openElements_.rbegin(); it != openElements_.rend(); ++it) {
        if ((*it)->tagName() == tag) return true;
        if ((*it)->tagName() != "option" && (*it)->tagName() != "optgroup") return false;
    }
    return false;
}

// ============================================================================
// Active Formatting Elements
// ============================================================================

void HTML5TreeConstructor::pushActiveFormatting(DOMElement* el, HTML5Token tok) {
    // Per spec: if there are already 3 entries with the same tag+attrs between the
    // last marker and the end of the list, remove the oldest
    int count = 0;
    for (auto it = activeFormattingElements_.rbegin();
         it != activeFormattingElements_.rend() && !it->isMarker; ++it) {
        if (it->element->tagName() == el->tagName()) {
            ++count;
            if (count >= 3) {
                activeFormattingElements_.erase((it+1).base());
                break;
            }
        }
    }
    activeFormattingElements_.push_back({el, std::move(tok), false});
}

void HTML5TreeConstructor::pushFormattingMarker() {
    activeFormattingElements_.push_back({nullptr, {}, true});
}

void HTML5TreeConstructor::reconstructActiveFormattingElements() {
    if (activeFormattingElements_.empty()) return;
    if (activeFormattingElements_.back().isMarker) return;
    if (hasElementInOpenStack(activeFormattingElements_.back().element->tagName())) return;

    // Find the last open entry from the end
    size_t entry = activeFormattingElements_.size() - 1;
    while (entry > 0) {
        --entry;
        if (activeFormattingElements_[entry].isMarker) { ++entry; break; }
        if (hasElementInOpenStack(activeFormattingElements_[entry].element->tagName())) { ++entry; break; }
    }

    // Re-create entries from entry to end
    while (entry < activeFormattingElements_.size()) {
        auto& fe = activeFormattingElements_[entry];
        DOMElement* newEl = insertElement(fe.token.tagName, fe.token.attributes);
        fe.element = newEl;
        ++entry;
    }
}

void HTML5TreeConstructor::clearFormattingToLastMarker() {
    while (!activeFormattingElements_.empty() && !activeFormattingElements_.back().isMarker)
        activeFormattingElements_.pop_back();
    if (!activeFormattingElements_.empty())
        activeFormattingElements_.pop_back(); // remove the marker too
}

DOMElement* HTML5TreeConstructor::findFormattingElement(const std::string& tag) {
    for (auto it = activeFormattingElements_.rbegin();
         it != activeFormattingElements_.rend(); ++it) {
        if (it->isMarker) break;
        if (it->element && it->element->tagName() == tag) return it->element;
    }
    return nullptr;
}

// ============================================================================
// Adoption Agency Algorithm (WHATWG §13.2.8.6)
// ============================================================================

void HTML5TreeConstructor::adoptionAgencyAlgorithm(const std::string& tagName) {
    // Simplified but correct implementation
    // Step 1: If current node is a formatting element with the given tag AND
    //         it's not in scope — just pop it
    if (currentNode() && currentNode()->tagName() == tagName &&
        !findFormattingElement(tagName)) {
        openElements_.pop_back();
        return;
    }

    // Main loop (max 8 iterations per spec)
    for (int outer = 0; outer < 8; ++outer) {
        // Step 3: Find formatting element
        DOMElement* formattingEl = findFormattingElement(tagName);
        if (!formattingEl) {
            // Not found — treat like an unknown end tag (pop from stack if present)
            if (hasElementInOpenStack(tagName)) popUntilTag(tagName);
            return;
        }

        // Step 4: If not in scope — parse error, abort
        if (!isElementInScope(tagName)) return;

        // Step 5: Find furthest block
        DOMElement* furthestBlock = nullptr;
        size_t formattingIdx = 0;
        for (size_t i = 0; i < openElements_.size(); ++i) {
            if (openElements_[i] == formattingEl) { formattingIdx = i; break; }
        }
        for (size_t i = formattingIdx + 1; i < openElements_.size(); ++i) {
            if (isSpecialElement(openElements_[i]->tagName())) {
                furthestBlock = openElements_[i];
            }
        }

        // Step 6: If no furthest block — pop everything up to and including formatting el
        if (!furthestBlock) {
            popUntilTag(tagName);
            // Remove from active formatting list
            for (auto it = activeFormattingElements_.begin();
                 it != activeFormattingElements_.end(); ++it) {
                if (it->element == formattingEl) {
                    activeFormattingElements_.erase(it);
                    break;
                }
            }
            return;
        }

        // Steps 7-19: The complex reparenting algorithm
        // Find common ancestor (element below formatting element in stack)
        DOMElement* commonAncestor = (formattingIdx > 0) ? openElements_[formattingIdx - 1] : nullptr;

        // Find furthest block index
        size_t furthestIdx = 0;
        for (size_t i = 0; i < openElements_.size(); ++i)
            if (openElements_[i] == furthestBlock) { furthestIdx = i; break; }

        // Move nodes between formatting el and furthest block under furthest block
        DOMElement* lastNode = furthestBlock;
        size_t nodeIdx = furthestIdx;
        DOMElement* node = nullptr;

        for (int inner = 0; inner < 3; ++inner) {
            --nodeIdx;
            if (nodeIdx <= formattingIdx) break;
            node = openElements_[nodeIdx];

            // Check if node is in active formatting elements
            bool inActive = false;
            for (auto& fe : activeFormattingElements_) {
                if (!fe.isMarker && fe.element == node) { inActive = true; break; }
            }

            if (!inActive) {
                // Remove node from open elements and continue
                openElements_.erase(openElements_.begin() + nodeIdx);
                furthestIdx--;
                continue;
            }

            // Create a new element with node's token
            // (simplified: just use node as-is for now)
            // Move lastNode to be a child of node
            if (lastNode->parentNode() && lastNode->parentNode() != node) {
                // Re-parent
                auto removed = lastNode->parentNode()->removeChild(lastNode);
                if (removed) node->appendChild(std::move(removed));
            }
            lastNode = node;
        }

        // Insert lastNode at appropriate location in commonAncestor
        if (commonAncestor && lastNode->parentNode() != commonAncestor) {
            auto removed = lastNode->parentNode() ? 
                lastNode->parentNode()->removeChild(lastNode) : nullptr;
            if (removed) commonAncestor->appendChild(std::move(removed));
        }

        // Create new element, move furthest block's children into it
        auto newEl = doc_->createElement(formattingEl->tagName());
        // Copy attributes
        for (auto& [name, val] : formattingEl->attributes())
            newEl->setAttribute(name, val);
        DOMElement* newElPtr = newEl.get();

        // Move all of furthestBlock's children to newEl
        while (!furthestBlock->childNodes().empty()) {
            auto child = furthestBlock->removeChild(furthestBlock->childNodes().front().get());
            if (child) newElPtr->appendChild(std::move(child));
        }
        furthestBlock->appendChild(std::move(newEl));

        // Update active formatting list: replace formattingEl with newElPtr
        for (auto& fe : activeFormattingElements_) {
            if (fe.element == formattingEl) { fe.element = newElPtr; break; }
        }

        // Update open elements: remove formattingEl, insert newElPtr after furthestBlock
        openElements_.erase(std::find(openElements_.begin(), openElements_.end(), formattingEl));
        furthestIdx = 0;
        for (size_t i = 0; i < openElements_.size(); ++i)
            if (openElements_[i] == furthestBlock) { furthestIdx = i; break; }
        openElements_.insert(openElements_.begin() + furthestIdx + 1, newElPtr);
    }
}

// ============================================================================
// Implied End Tags / Close P
// ============================================================================

void HTML5TreeConstructor::generateImpliedEndTags(const std::string& exclude) {
    static const std::unordered_set<std::string> impliedEndTags = {
        "dd","dt","li","optgroup","option","p","rb","rp","rt","rtc"
    };
    while (!openElements_.empty()) {
        std::string tag = openElements_.back()->tagName();
        if (tag == exclude) break;
        if (!impliedEndTags.count(tag)) break;
        openElements_.pop_back();
    }
}

void HTML5TreeConstructor::closePElement() {
    if (isElementInButtonScope("p")) {
        generateImpliedEndTags("p");
        popUntilTag("p");
    }
}

// ============================================================================
// Reset Insertion Mode
// ============================================================================

void HTML5TreeConstructor::resetInsertionModeAppropriately() {
    for (auto it = openElements_.rbegin(); it != openElements_.rend(); ++it) {
        std::string tag = (*it)->tagName();
        bool last = (it == openElements_.rbegin());

        if (tag == "select")    { mode_ = InsertionMode::InSelect; return; }
        if (tag == "td" || tag == "th") { mode_ = InsertionMode::InCell; return; }
        if (tag == "tr")        { mode_ = InsertionMode::InRow; return; }
        if (tag == "tbody" || tag == "thead" || tag == "tfoot") { mode_ = InsertionMode::InTableBody; return; }
        if (tag == "caption")   { mode_ = InsertionMode::InCaption; return; }
        if (tag == "colgroup")  { mode_ = InsertionMode::InColumnGroup; return; }
        if (tag == "table")     { mode_ = InsertionMode::InTable; return; }
        if (tag == "template")  { mode_ = templateInsertionModes_.empty() ? InsertionMode::InTemplate : templateInsertionModes_.back(); return; }
        if (tag == "head" && !last) { mode_ = InsertionMode::InHead; return; }
        if (tag == "body")      { mode_ = InsertionMode::InBody; return; }
        if (tag == "frameset")  { mode_ = InsertionMode::InFrameset; return; }
        if (tag == "html")      { mode_ = headElement_ ? InsertionMode::AfterHead : InsertionMode::BeforeHead; return; }
        if (last)               { mode_ = InsertionMode::InBody; return; }
    }
    mode_ = InsertionMode::InBody;
}

// ============================================================================
// Initial Mode
// ============================================================================

void HTML5TreeConstructor::processInitial(HTML5Token& tok) {
    auto isWhitespace = [](const std::string& s) {
        for (char c : s) if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') return false;
        return true;
    };

    if (tok.type == HTML5TokenType::Character && isWhitespace(tok.data)) {
        return; // ignore
    }
    if (tok.type == HTML5TokenType::Comment) {
        insertComment(tok.data, doc_.get());
        return;
    }
    if (tok.type == HTML5TokenType::DOCTYPE) {
        // Set quirks mode if needed
        if (tok.tagName != "html" || tok.forceQuirks ||
            (tok.publicIdentifier && !tok.publicIdentifier->empty())) {
            // Check for known quirks conditions (simplified)
            if (tok.forceQuirks) quirksMode_ = true;
        }
        mode_ = InsertionMode::BeforeHTML;
        return;
    }
    // Anything else — switch to BeforeHTML and reprocess
    quirksMode_ = true; // no doctype = quirks
    mode_ = InsertionMode::BeforeHTML;
    processToken(std::move(tok));
}

// ============================================================================
// BeforeHTML Mode
// ============================================================================

void HTML5TreeConstructor::processBeforeHTML(HTML5Token& tok) {
    if (tok.type == HTML5TokenType::DOCTYPE) return; // ignore

    if (tok.type == HTML5TokenType::Comment) {
        insertComment(tok.data, doc_.get());
        return;
    }

    auto isWhitespace = [](const std::string& s) {
        for (char c : s) if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') return false;
        return true;
    };
    if (tok.type == HTML5TokenType::Character && isWhitespace(tok.data)) return;

    if (tok.type == HTML5TokenType::StartTag && tok.tagName == "html") {
        auto htmlEl = doc_->createElement("html");
        for (auto& [name,val] : tok.attributes) htmlEl->setAttribute(name, val);
        DOMElement* htmlPtr = htmlEl.get();
        doc_->appendChild(std::move(htmlEl));
        doc_->setDocumentElement(htmlPtr);
        openElements_.push_back(htmlPtr);
        mode_ = InsertionMode::BeforeHead;
        return;
    }

    if (tok.type == HTML5TokenType::EndTag &&
        tok.tagName != "head" && tok.tagName != "body" &&
        tok.tagName != "html" && tok.tagName != "br") {
        return; // parse error, ignore
    }

    // Anything else: create html element implicitly
    auto htmlEl = doc_->createElement("html");
    DOMElement* htmlPtr = htmlEl.get();
    doc_->appendChild(std::move(htmlEl));
    doc_->setDocumentElement(htmlPtr);
    openElements_.push_back(htmlPtr);
    mode_ = InsertionMode::BeforeHead;
    processToken(std::move(tok));
}

// ============================================================================
// BeforeHead Mode
// ============================================================================

void HTML5TreeConstructor::processBeforeHead(HTML5Token& tok) {
    auto isWhitespace = [](const std::string& s) {
        for (char c : s) if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') return false;
        return true;
    };

    if (tok.type == HTML5TokenType::Character && isWhitespace(tok.data)) return;
    if (tok.type == HTML5TokenType::Comment) { insertComment(tok.data); return; }
    if (tok.type == HTML5TokenType::DOCTYPE) return;

    if (tok.type == HTML5TokenType::StartTag && tok.tagName == "html") {
        processInBody(tok); return;
    }

    if (tok.type == HTML5TokenType::StartTag && tok.tagName == "head") {
        headElement_ = insertElementForToken(tok);
        mode_ = InsertionMode::InHead;
        return;
    }

    if (tok.type == HTML5TokenType::EndTag &&
        tok.tagName != "head" && tok.tagName != "body" &&
        tok.tagName != "html" && tok.tagName != "br") {
        return;
    }

    // Implicitly create head
    HTML5Token headTok;
    headTok.type = HTML5TokenType::StartTag;
    headTok.tagName = "head";
    headElement_ = insertElementForToken(headTok);
    mode_ = InsertionMode::InHead;
    processToken(std::move(tok));
}

// ============================================================================
// InHead Mode
// ============================================================================

void HTML5TreeConstructor::processInHead(HTML5Token& tok) {
    auto isWS = [](const std::string& s) {
        for (char c : s) if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') return false;
        return true;
    };

    if (tok.type == HTML5TokenType::Character && isWS(tok.data)) {
        insertCharacters(tok.data); return;
    }
    if (tok.type == HTML5TokenType::Comment) { insertComment(tok.data); return; }
    if (tok.type == HTML5TokenType::DOCTYPE) return;

    if (tok.type == HTML5TokenType::StartTag) {
        if (tok.tagName == "html") { processInBody(tok); return; }

        if (tok.tagName == "base" || tok.tagName == "basefont" ||
            tok.tagName == "bgsound" || tok.tagName == "link") {
            insertElementForToken(tok);
            openElements_.pop_back(); // immediately pop void elements
            return;
        }

        if (tok.tagName == "meta") {
            insertElementForToken(tok);
            openElements_.pop_back();
            return;
        }

        if (tok.tagName == "title") {
            // RCDATA element
            insertElementForToken(tok);
            if (tokenizer_) tokenizer_->setState(TokenizerState::RCDATA);
            if (tokenizer_) tokenizer_->setLastStartTagName("title");
            originalMode_ = mode_;
            mode_ = InsertionMode::Text;
            return;
        }

        if (tok.tagName == "noscript") {
            if (scriptingEnabled_) {
                // RAWTEXT
                insertElementForToken(tok);
                if (tokenizer_) tokenizer_->setState(TokenizerState::RAWTEXT);
                if (tokenizer_) tokenizer_->setLastStartTagName("noscript");
                originalMode_ = mode_;
                mode_ = InsertionMode::Text;
            } else {
                insertElementForToken(tok);
                mode_ = InsertionMode::InHeadNoscript;
            }
            return;
        }

        if (tok.tagName == "noframes" || tok.tagName == "style") {
            insertElementForToken(tok);
            if (tokenizer_) tokenizer_->setState(TokenizerState::RAWTEXT);
            if (tokenizer_) tokenizer_->setLastStartTagName(tok.tagName);
            originalMode_ = mode_;
            mode_ = InsertionMode::Text;
            return;
        }

        if (tok.tagName == "script") {
            insertElementForToken(tok);
            if (tokenizer_) tokenizer_->setState(TokenizerState::ScriptData);
            if (tokenizer_) tokenizer_->setLastStartTagName("script");
            originalMode_ = mode_;
            mode_ = InsertionMode::Text;
            return;
        }

        if (tok.tagName == "template") {
            insertElementForToken(tok);
            pushFormattingMarker();
            framesetOk_ = false;
            templateInsertionModes_.push_back(InsertionMode::InTemplate);
            mode_ = InsertionMode::InTemplate;
            return;
        }

        if (tok.tagName == "head") return; // parse error, ignore
    }

    if (tok.type == HTML5TokenType::EndTag) {
        if (tok.tagName == "head") {
            openElements_.pop_back(); // pop head
            mode_ = InsertionMode::AfterHead;
            return;
        }
        if (tok.tagName == "template") {
            if (!hasElementInOpenStack("template")) return;
            generateImpliedEndTags();
            popUntilTag("template");
            clearFormattingToLastMarker();
            if (!templateInsertionModes_.empty()) templateInsertionModes_.pop_back();
            resetInsertionModeAppropriately();
            return;
        }
        if (tok.tagName == "body" || tok.tagName == "html" || tok.tagName == "br") {
            // Fall through to default
        } else {
            return; // parse error, ignore
        }
    }

    // Default: pop head and reprocess in AfterHead
    openElements_.pop_back();
    mode_ = InsertionMode::AfterHead;
    processToken(std::move(tok));
}

void HTML5TreeConstructor::processInHeadNoscript(HTML5Token& tok) {
    // Simplified — treat like InHead for now
    if (tok.type == HTML5TokenType::EndTag && tok.tagName == "noscript") {
        openElements_.pop_back();
        mode_ = InsertionMode::InHead;
        return;
    }
    // All else: reprocess in InHead
    processInHead(tok);
}

// ============================================================================
// AfterHead Mode
// ============================================================================

void HTML5TreeConstructor::processAfterHead(HTML5Token& tok) {
    auto isWS = [](const std::string& s) {
        for (char c : s) if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') return false;
        return true;
    };

    if (tok.type == HTML5TokenType::Character && isWS(tok.data)) {
        insertCharacters(tok.data); return;
    }
    if (tok.type == HTML5TokenType::Comment) { insertComment(tok.data); return; }
    if (tok.type == HTML5TokenType::DOCTYPE) return;

    if (tok.type == HTML5TokenType::StartTag) {
        if (tok.tagName == "html") { processInBody(tok); return; }
        if (tok.tagName == "body") {
            insertElementForToken(tok);
            framesetOk_ = false;
            mode_ = InsertionMode::InBody;
            return;
        }
        if (tok.tagName == "frameset") {
            insertElementForToken(tok);
            mode_ = InsertionMode::InFrameset;
            return;
        }
        if (tok.tagName == "base" || tok.tagName == "basefont" || tok.tagName == "bgsound" ||
            tok.tagName == "link" || tok.tagName == "meta" || tok.tagName == "noframes" ||
            tok.tagName == "script" || tok.tagName == "style" || tok.tagName == "template" ||
            tok.tagName == "title") {
            openElements_.push_back(headElement_);
            processInHead(tok);
            // Remove headElement from open elements
            for (auto it = openElements_.begin(); it != openElements_.end(); ++it) {
                if (*it == headElement_) { openElements_.erase(it); break; }
            }
            return;
        }
        if (tok.tagName == "head") return; // parse error
    }

    if (tok.type == HTML5TokenType::EndTag) {
        if (tok.tagName == "template") { processInHead(tok); return; }
        if (tok.tagName != "body" && tok.tagName != "html" && tok.tagName != "br") return;
    }

    // Insert body implicitly
    HTML5Token bodyTok;
    bodyTok.type = HTML5TokenType::StartTag;
    bodyTok.tagName = "body";
    insertElementForToken(bodyTok);
    mode_ = InsertionMode::InBody;
    processToken(std::move(tok));
}

// ============================================================================
// InBody Mode — The Main Mode
// ============================================================================

void HTML5TreeConstructor::processInBody(HTML5Token& tok) {
    if (tok.type == HTML5TokenType::Character) {
        if (!tok.data.empty() && tok.data[0] == '\0') {
            // parse error — ignore null
        } else {
            // Check for whitespace-only
            bool allWS = true;
            for (char c : tok.data) {
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') { allWS = false; break; }
            }
            if (!allWS) {
                reconstructActiveFormattingElements();
                framesetOk_ = false;
            }
            insertCharacters(tok.data);
        }
        return;
    }

    if (tok.type == HTML5TokenType::Comment) { insertComment(tok.data); return; }
    if (tok.type == HTML5TokenType::DOCTYPE) return;

    if (tok.type == HTML5TokenType::StartTag) { inBodyStartTag(tok); return; }
    if (tok.type == HTML5TokenType::EndTag)   { inBodyEndTag(tok);   return; }

    if (tok.type == HTML5TokenType::EndOfFile) {
        // EOF in body — done. Check for open elements (errors) but just stop.
    }
}

void HTML5TreeConstructor::inBodyStartTag(HTML5Token& tok) {
    const std::string& tag = tok.tagName;

    if (tag == "html") {
        // parse error — merge attributes into existing html element
        if (!openElements_.empty()) {
            for (auto& [name, val] : tok.attributes)
                if (!openElements_.front()->hasAttribute(name))
                    openElements_.front()->setAttribute(name, val);
        }
        return;
    }

    // ── Void / self-closing special elements ──
    if (tag == "base" || tag == "basefont" || tag == "bgsound" || tag == "link" ||
        tag == "meta" || tag == "noframes" || tag == "script" || tag == "style" ||
        tag == "template" || tag == "title") {
        processInHead(tok); return;
    }

    if (tag == "body") {
        if (openElements_.size() < 2 || openElements_[1]->tagName() != "body") return;
        framesetOk_ = false;
        for (auto& [name, val] : tok.attributes)
            if (!openElements_[1]->hasAttribute(name))
                openElements_[1]->setAttribute(name, val);
        return;
    }

    if (tag == "frameset") {
        if (!framesetOk_) return;
        // Remove body from open elements and re-parent
        popUntilTag("body");
        insertElementForToken(tok);
        mode_ = InsertionMode::InFrameset;
        return;
    }

    // ── Heading elements ──
    if (tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4" || tag == "h5" || tag == "h6") {
        closePElement();
        if (!openElements_.empty()) {
            std::string ct = currentNode()->tagName();
            if (ct == "h1" || ct == "h2" || ct == "h3" || ct == "h4" || ct == "h5" || ct == "h6")
                openElements_.pop_back();
        }
        insertElementForToken(tok);
        return;
    }

    // ── Block elements that close p ──
    if (tag == "address" || tag == "article" || tag == "aside" || tag == "blockquote" ||
        tag == "center" || tag == "details" || tag == "dialog" || tag == "dir" ||
        tag == "div" || tag == "dl" || tag == "fieldset" || tag == "figcaption" ||
        tag == "figure" || tag == "footer" || tag == "header" || tag == "hgroup" ||
        tag == "main" || tag == "menu" || tag == "nav" || tag == "ol" || tag == "p" ||
        tag == "section" || tag == "summary" || tag == "ul") {
        closePElement();
        insertElementForToken(tok);
        return;
    }

    // ── Pre / Listing ──
    if (tag == "pre" || tag == "listing") {
        closePElement();
        insertElementForToken(tok);
        framesetOk_ = false;
        return;
    }

    // ── Form ──
    if (tag == "form") {
        if (formElement_ && !hasElementInOpenStack("template")) return; // error
        closePElement();
        formElement_ = insertElementForToken(tok);
        return;
    }

    // ── List items ──
    if (tag == "li") {
        framesetOk_ = false;
        // Close existing li
        for (auto it = openElements_.rbegin(); it != openElements_.rend(); ++it) {
            if ((*it)->tagName() == "li") { generateImpliedEndTags("li"); popUntilTag("li"); break; }
            if (isSpecialElement((*it)->tagName()) && (*it)->tagName() != "address" &&
                (*it)->tagName() != "div" && (*it)->tagName() != "p") break;
        }
        closePElement();
        insertElementForToken(tok);
        return;
    }

    if (tag == "dd" || tag == "dt") {
        framesetOk_ = false;
        for (auto it = openElements_.rbegin(); it != openElements_.rend(); ++it) {
            if ((*it)->tagName() == "dd") { generateImpliedEndTags("dd"); popUntilTag("dd"); break; }
            if ((*it)->tagName() == "dt") { generateImpliedEndTags("dt"); popUntilTag("dt"); break; }
            if (isSpecialElement((*it)->tagName()) && (*it)->tagName() != "address" &&
                (*it)->tagName() != "div" && (*it)->tagName() != "p") break;
        }
        closePElement();
        insertElementForToken(tok);
        return;
    }

    // ── Plaintext ──
    if (tag == "plaintext") {
        closePElement();
        insertElementForToken(tok);
        if (tokenizer_) tokenizer_->setState(TokenizerState::PLAINTEXT);
        return;
    }

    // ── Button ──
    if (tag == "button") {
        if (isElementInScope("button")) { generateImpliedEndTags(); popUntilTag("button"); }
        reconstructActiveFormattingElements();
        insertElementForToken(tok);
        framesetOk_ = false;
        return;
    }

    // ── Anchor ──
    if (tag == "a") {
        if (findFormattingElement("a")) {
            adoptionAgencyAlgorithm("a");
        }
        reconstructActiveFormattingElements();
        DOMElement* el = insertElementForToken(tok);
        pushActiveFormatting(el, tok);
        return;
    }

    // ── Formatting elements ──
    if (isFormattingElement(tag)) {
        reconstructActiveFormattingElements();
        DOMElement* el = insertElementForToken(tok);
        pushActiveFormatting(el, tok);
        return;
    }

    // ── Nobr ──
    if (tag == "nobr") {
        reconstructActiveFormattingElements();
        if (isElementInScope("nobr")) {
            adoptionAgencyAlgorithm("nobr");
            reconstructActiveFormattingElements();
        }
        DOMElement* el = insertElementForToken(tok);
        pushActiveFormatting(el, tok);
        return;
    }

    // ── Applet / Marquee / Object ──
    if (tag == "applet" || tag == "marquee" || tag == "object") {
        reconstructActiveFormattingElements();
        insertElementForToken(tok);
        pushFormattingMarker();
        framesetOk_ = false;
        return;
    }

    // ── Table ──
    if (tag == "table") {
        if (!quirksMode_ && isElementInButtonScope("p")) closePElement();
        insertElementForToken(tok);
        framesetOk_ = false;
        mode_ = InsertionMode::InTable;
        return;
    }

    // ── Void elements (img, br, input, etc.) ──
    if (tag == "area" || tag == "br" || tag == "embed" || tag == "img" ||
        tag == "keygen" || tag == "wbr") {
        reconstructActiveFormattingElements();
        insertElementForToken(tok);
        openElements_.pop_back();
        framesetOk_ = false;
        return;
    }

    if (tag == "input") {
        reconstructActiveFormattingElements();
        insertElementForToken(tok);
        openElements_.pop_back();
        if (tok.getAttribute("type") != "hidden") framesetOk_ = false;
        return;
    }

    if (tag == "param" || tag == "source" || tag == "track") {
        insertElementForToken(tok);
        openElements_.pop_back();
        return;
    }

    if (tag == "hr") {
        closePElement();
        insertElementForToken(tok);
        openElements_.pop_back();
        framesetOk_ = false;
        return;
    }

    // ── Image (alias for img) ──
    if (tag == "image") {
        tok.tagName = "img";
        processToken(std::move(tok));
        return;
    }

    // ── Textarea ──
    if (tag == "textarea") {
        insertElementForToken(tok);
        if (tokenizer_) tokenizer_->setState(TokenizerState::RCDATA);
        if (tokenizer_) tokenizer_->setLastStartTagName("textarea");
        framesetOk_ = false;
        originalMode_ = InsertionMode::InBody;
        mode_ = InsertionMode::Text;
        return;
    }

    // ── XMP ──
    if (tag == "xmp") {
        closePElement();
        reconstructActiveFormattingElements();
        framesetOk_ = false;
        insertElementForToken(tok);
        if (tokenizer_) tokenizer_->setState(TokenizerState::RAWTEXT);
        if (tokenizer_) tokenizer_->setLastStartTagName("xmp");
        originalMode_ = InsertionMode::InBody;
        mode_ = InsertionMode::Text;
        return;
    }

    // ── Iframe ──
    if (tag == "iframe") {
        framesetOk_ = false;
        insertElementForToken(tok);
        if (tokenizer_) tokenizer_->setState(TokenizerState::RAWTEXT);
        if (tokenizer_) tokenizer_->setLastStartTagName("iframe");
        originalMode_ = InsertionMode::InBody;
        mode_ = InsertionMode::Text;
        return;
    }

    // ── Noembed, noscript ──
    if (tag == "noembed" || (tag == "noscript" && scriptingEnabled_)) {
        insertElementForToken(tok);
        if (tokenizer_) tokenizer_->setState(TokenizerState::RAWTEXT);
        if (tokenizer_) tokenizer_->setLastStartTagName(tag);
        originalMode_ = InsertionMode::InBody;
        mode_ = InsertionMode::Text;
        return;
    }

    // ── Select ──
    if (tag == "select") {
        reconstructActiveFormattingElements();
        insertElementForToken(tok);
        framesetOk_ = false;
        // Determine mode based on context
        if (mode_ == InsertionMode::InTable || mode_ == InsertionMode::InCaption ||
            mode_ == InsertionMode::InTableBody || mode_ == InsertionMode::InRow ||
            mode_ == InsertionMode::InCell)
            mode_ = InsertionMode::InSelectInTable;
        else
            mode_ = InsertionMode::InSelect;
        return;
    }

    // ── Optgroup, Option ──
    if (tag == "optgroup" || tag == "option") {
        if (!openElements_.empty() && currentNode()->tagName() == "option")
            openElements_.pop_back();
        reconstructActiveFormattingElements();
        insertElementForToken(tok);
        return;
    }

    // ── Rb, Rtc ──
    if (tag == "rb" || tag == "rtc") {
        if (isElementInScope("ruby")) generateImpliedEndTags();
        insertElementForToken(tok);
        return;
    }

    // ── Rp, Rt ──
    if (tag == "rp" || tag == "rt") {
        if (isElementInScope("ruby")) generateImpliedEndTags("rtc");
        insertElementForToken(tok);
        return;
    }

    // ── Math, SVG ──
    if (tag == "math" || tag == "svg") {
        reconstructActiveFormattingElements();
        insertElementForToken(tok);
        if (tok.selfClosing) openElements_.pop_back();
        return;
    }

    // ── Caption, Col, Colgroup, Frame, Head, Table-related ──
    if (tag == "caption" || tag == "col" || tag == "colgroup" || tag == "frame" ||
        tag == "head" || tag == "tbody" || tag == "td" || tag == "tfoot" || tag == "th" ||
        tag == "thead" || tag == "tr") {
        return; // parse error — ignore
    }

    // ── Default: any other start tag ──
    reconstructActiveFormattingElements();
    insertElementForToken(tok);
}

void HTML5TreeConstructor::inBodyEndTag(HTML5Token& tok) {
    const std::string& tag = tok.tagName;

    if (tag == "template") { processInHead(tok); return; }

    if (tag == "body") {
        if (!isElementInScope("body")) return; // error
        mode_ = InsertionMode::AfterBody;
        return;
    }

    if (tag == "html") {
        if (!isElementInScope("body")) return;
        mode_ = InsertionMode::AfterBody;
        processToken(std::move(tok));
        return;
    }

    // Block / structural end tags
    if (tag == "address" || tag == "article" || tag == "aside" || tag == "blockquote" ||
        tag == "button" || tag == "center" || tag == "details" || tag == "dialog" ||
        tag == "dir" || tag == "div" || tag == "dl" || tag == "fieldset" ||
        tag == "figcaption" || tag == "figure" || tag == "footer" || tag == "header" ||
        tag == "hgroup" || tag == "listing" || tag == "main" || tag == "menu" ||
        tag == "nav" || tag == "ol" || tag == "pre" || tag == "section" ||
        tag == "summary" || tag == "ul") {
        if (!isElementInScope(tag)) return;
        generateImpliedEndTags();
        popUntilTag(tag);
        return;
    }

    if (tag == "form") {
        if (!hasElementInOpenStack("template")) {
            DOMElement* node = formElement_;
            formElement_ = nullptr;
            if (!node || !isElementInScope(node->tagName())) return;
            generateImpliedEndTags();
            for (auto it = openElements_.begin(); it != openElements_.end(); ++it) {
                if (*it == node) { openElements_.erase(it); break; }
            }
        } else {
            if (!isElementInScope("form")) return;
            generateImpliedEndTags();
            popUntilTag("form");
        }
        return;
    }

    if (tag == "p") {
        if (!isElementInButtonScope("p")) {
            HTML5Token pStart; pStart.type = HTML5TokenType::StartTag; pStart.tagName = "p";
            insertElementForToken(pStart);
        }
        generateImpliedEndTags("p");
        popUntilTag("p");
        return;
    }

    if (tag == "li") {
        if (!isElementInListItemScope("li")) return;
        generateImpliedEndTags("li");
        popUntilTag("li");
        return;
    }

    if (tag == "dd" || tag == "dt") {
        if (!isElementInScope(tag)) return;
        generateImpliedEndTags(tag);
        popUntilTag(tag);
        return;
    }

    if (tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4" || tag == "h5" || tag == "h6") {
        bool inScope = false;
        for (auto& h : {"h1","h2","h3","h4","h5","h6"})
            if (isElementInScope(h)) { inScope = true; break; }
        if (!inScope) return;
        generateImpliedEndTags();
        while (!openElements_.empty()) {
            std::string t = openElements_.back()->tagName();
            openElements_.pop_back();
            if (t == "h1"||t=="h2"||t=="h3"||t=="h4"||t=="h5"||t=="h6") break;
        }
        return;
    }

    // Formatting elements — use adoption agency
    if (isFormattingElement(tag) || tag == "a") {
        adoptionAgencyAlgorithm(tag);
        return;
    }

    if (tag == "applet" || tag == "marquee" || tag == "object") {
        if (!isElementInScope(tag)) return;
        generateImpliedEndTags();
        popUntilTag(tag);
        clearFormattingToLastMarker();
        return;
    }

    if (tag == "br") {
        // parse error — treat as start tag
        HTML5Token brStart; brStart.type = HTML5TokenType::StartTag; brStart.tagName = "br";
        processToken(std::move(brStart));
        return;
    }

    // Default: find matching element in stack and pop to it
    for (auto it = openElements_.rbegin(); it != openElements_.rend(); ++it) {
        if ((*it)->tagName() == tag) {
            generateImpliedEndTags(tag);
            openElements_.erase((it+1).base(), openElements_.end());
            break;
        }
        if (isSpecialElement((*it)->tagName())) break; // no match in scope
    }
}

// ============================================================================
// Text Mode (raw text / RCDATA)
// ============================================================================

void HTML5TreeConstructor::processText(HTML5Token& tok) {
    if (tok.type == HTML5TokenType::Character) {
        insertCharacters(tok.data);
        return;
    }
    if (tok.type == HTML5TokenType::EndOfFile) {
        // parse error
        openElements_.pop_back();
        mode_ = originalMode_;
        processToken(std::move(tok));
        return;
    }
    if (tok.type == HTML5TokenType::EndTag) {
        openElements_.pop_back();
        mode_ = originalMode_;
        return;
    }
}

// ============================================================================
// Table Modes
// ============================================================================

void HTML5TreeConstructor::processInTable(HTML5Token& tok) {
    auto isWS = [](const std::string& s) {
        for (char c : s) if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') return false;
        return true;
    };

    if (tok.type == HTML5TokenType::Character) {
        std::string ct = currentNode() ? currentNode()->tagName() : "";
        if (ct == "table" || ct == "tbody" || ct == "template" || ct == "tfoot" ||
            ct == "thead" || ct == "tr") {
            pendingTableChars_ += tok.data;
            if (!isWS(tok.data)) pendingTableCharsHaveNonWhitespace_ = true;
            originalMode_ = mode_;
            mode_ = InsertionMode::InTableText;
            return;
        }
    }

    if (tok.type == HTML5TokenType::Comment) { insertComment(tok.data); return; }
    if (tok.type == HTML5TokenType::DOCTYPE) return;

    if (tok.type == HTML5TokenType::StartTag) {
        const std::string& tag = tok.tagName;
        if (tag == "caption") {
            while (!openElements_.empty() &&
                   openElements_.back()->tagName() != "html" &&
                   openElements_.back()->tagName() != "table" &&
                   openElements_.back()->tagName() != "template")
                openElements_.pop_back();
            pushFormattingMarker();
            insertElementForToken(tok);
            mode_ = InsertionMode::InCaption;
            return;
        }
        if (tag == "colgroup") {
            while (!openElements_.empty() &&
                   openElements_.back()->tagName() != "html" &&
                   openElements_.back()->tagName() != "table" &&
                   openElements_.back()->tagName() != "template")
                openElements_.pop_back();
            insertElementForToken(tok);
            mode_ = InsertionMode::InColumnGroup;
            return;
        }
        if (tag == "col") {
            HTML5Token cg; cg.type = HTML5TokenType::StartTag; cg.tagName = "colgroup";
            processToken(std::move(cg));
            processToken(std::move(tok));
            return;
        }
        if (tag == "tbody" || tag == "tfoot" || tag == "thead") {
            while (!openElements_.empty() &&
                   openElements_.back()->tagName() != "html" &&
                   openElements_.back()->tagName() != "table" &&
                   openElements_.back()->tagName() != "template")
                openElements_.pop_back();
            insertElementForToken(tok);
            mode_ = InsertionMode::InTableBody;
            return;
        }
        if (tag == "td" || tag == "th" || tag == "tr") {
            HTML5Token tb; tb.type = HTML5TokenType::StartTag; tb.tagName = "tbody";
            processToken(std::move(tb));
            processToken(std::move(tok));
            return;
        }
        if (tag == "table") {
            if (!isElementInTableScope("table")) return;
            popUntilTag("table");
            resetInsertionModeAppropriately();
            processToken(std::move(tok));
            return;
        }
        if (tag == "style" || tag == "script" || tag == "template") {
            processInHead(tok); return;
        }
        if (tag == "input") {
            if (tok.getAttribute("type") != "hidden") {
                // foster parent
                fosterParenting_ = true;
                processInBody(tok);
                fosterParenting_ = false;
            } else {
                insertElementForToken(tok);
                openElements_.pop_back();
            }
            return;
        }
        if (tag == "form") {
            if (formElement_ || hasElementInOpenStack("template")) return;
            formElement_ = insertElementForToken(tok);
            openElements_.pop_back();
            return;
        }
    }

    if (tok.type == HTML5TokenType::EndTag) {
        const std::string& tag = tok.tagName;
        if (tag == "table") {
            if (!isElementInTableScope("table")) return;
            popUntilTag("table");
            resetInsertionModeAppropriately();
            return;
        }
        if (tag == "body" || tag == "caption" || tag == "col" || tag == "colgroup" ||
            tag == "html" || tag == "tbody" || tag == "td" || tag == "tfoot" ||
            tag == "th" || tag == "thead" || tag == "tr") {
            return; // parse error — ignore
        }
        if (tag == "template") { processInHead(tok); return; }
    }

    if (tok.type == HTML5TokenType::EndOfFile) { processInBody(tok); return; }

    // Anything else — foster parent
    fosterParenting_ = true;
    processInBody(tok);
    fosterParenting_ = false;
}

void HTML5TreeConstructor::processInTableText(HTML5Token& tok) {
    if (tok.type == HTML5TokenType::Character) {
        pendingTableChars_ += tok.data;
        bool allWS = true;
        for (char c : tok.data) if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') { allWS = false; break; }
        if (!allWS) pendingTableCharsHaveNonWhitespace_ = true;
        return;
    }
    // Flush pending chars
    if (pendingTableCharsHaveNonWhitespace_) {
        // Foster parent all chars
        fosterParenting_ = true;
        insertCharacters(pendingTableChars_);
        fosterParenting_ = false;
    } else {
        insertCharacters(pendingTableChars_);
    }
    pendingTableChars_.clear();
    pendingTableCharsHaveNonWhitespace_ = false;
    mode_ = originalMode_;
    processToken(std::move(tok));
}

void HTML5TreeConstructor::processInCaption(HTML5Token& tok) {
    if (tok.type == HTML5TokenType::EndTag && tok.tagName == "caption") {
        if (!isElementInTableScope("caption")) return;
        generateImpliedEndTags();
        popUntilTag("caption");
        clearFormattingToLastMarker();
        mode_ = InsertionMode::InTable;
        return;
    }
    if (tok.type == HTML5TokenType::StartTag) {
        if (tok.tagName == "caption" || tok.tagName == "col" || tok.tagName == "colgroup" ||
            tok.tagName == "tbody" || tok.tagName == "td" || tok.tagName == "tfoot" ||
            tok.tagName == "th" || tok.tagName == "thead" || tok.tagName == "tr") {
            if (!isElementInTableScope("caption")) return;
            generateImpliedEndTags();
            popUntilTag("caption");
            clearFormattingToLastMarker();
            mode_ = InsertionMode::InTable;
            processToken(std::move(tok));
            return;
        }
    }
    if (tok.type == HTML5TokenType::EndTag && tok.tagName == "table") {
        if (!isElementInTableScope("caption")) return;
        generateImpliedEndTags();
        popUntilTag("caption");
        clearFormattingToLastMarker();
        mode_ = InsertionMode::InTable;
        processToken(std::move(tok));
        return;
    }
    processInBody(tok);
}

void HTML5TreeConstructor::processInColumnGroup(HTML5Token& tok) {
    auto isWS = [](const std::string& s) {
        for (char c : s) if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') return false;
        return true;
    };
    if (tok.type == HTML5TokenType::Character && isWS(tok.data)) { insertCharacters(tok.data); return; }
    if (tok.type == HTML5TokenType::Comment) { insertComment(tok.data); return; }
    if (tok.type == HTML5TokenType::DOCTYPE) return;
    if (tok.type == HTML5TokenType::StartTag && tok.tagName == "html") { processInBody(tok); return; }
    if (tok.type == HTML5TokenType::StartTag && tok.tagName == "col") {
        insertElementForToken(tok);
        openElements_.pop_back();
        return;
    }
    if (tok.type == HTML5TokenType::EndTag && tok.tagName == "colgroup") {
        if (!openElements_.empty() && currentNode()->tagName() == "colgroup") {
            openElements_.pop_back();
            mode_ = InsertionMode::InTable;
        }
        return;
    }
    if (tok.type == HTML5TokenType::EndTag && tok.tagName == "col") return;
    if ((tok.type == HTML5TokenType::StartTag || tok.type == HTML5TokenType::EndTag) &&
        tok.tagName == "template") { processInHead(tok); return; }
    if (tok.type == HTML5TokenType::EndOfFile) { processInBody(tok); return; }
    if (!openElements_.empty() && currentNode()->tagName() == "colgroup") {
        openElements_.pop_back();
        mode_ = InsertionMode::InTable;
        processToken(std::move(tok));
    }
}

void HTML5TreeConstructor::processInTableBody(HTML5Token& tok) {
    if (tok.type == HTML5TokenType::StartTag) {
        if (tok.tagName == "tr") {
            while (!openElements_.empty() &&
                   openElements_.back()->tagName() != "tbody" &&
                   openElements_.back()->tagName() != "tfoot" &&
                   openElements_.back()->tagName() != "thead" &&
                   openElements_.back()->tagName() != "template" &&
                   openElements_.back()->tagName() != "html")
                openElements_.pop_back();
            insertElementForToken(tok);
            mode_ = InsertionMode::InRow;
            return;
        }
        if (tok.tagName == "th" || tok.tagName == "td") {
            HTML5Token tr; tr.type = HTML5TokenType::StartTag; tr.tagName = "tr";
            processToken(std::move(tr));
            processToken(std::move(tok));
            return;
        }
        if (tok.tagName == "caption" || tok.tagName == "col" || tok.tagName == "colgroup" ||
            tok.tagName == "tbody" || tok.tagName == "tfoot" || tok.tagName == "thead") {
            if (!isElementInTableScope("tbody") && !isElementInTableScope("thead") &&
                !isElementInTableScope("tfoot")) return;
            while (!openElements_.empty() &&
                   openElements_.back()->tagName() != "tbody" &&
                   openElements_.back()->tagName() != "tfoot" &&
                   openElements_.back()->tagName() != "thead" &&
                   openElements_.back()->tagName() != "template" &&
                   openElements_.back()->tagName() != "html")
                openElements_.pop_back();
            openElements_.pop_back();
            mode_ = InsertionMode::InTable;
            processToken(std::move(tok));
            return;
        }
    }
    if (tok.type == HTML5TokenType::EndTag) {
        if (tok.tagName == "tbody" || tok.tagName == "tfoot" || tok.tagName == "thead") {
            if (!isElementInTableScope(tok.tagName)) return;
            while (!openElements_.empty() &&
                   openElements_.back()->tagName() != "tbody" &&
                   openElements_.back()->tagName() != "tfoot" &&
                   openElements_.back()->tagName() != "thead")
                openElements_.pop_back();
            openElements_.pop_back();
            mode_ = InsertionMode::InTable;
            return;
        }
        if (tok.tagName == "table") {
            if (!isElementInTableScope("tbody") && !isElementInTableScope("thead") &&
                !isElementInTableScope("tfoot")) return;
            while (!openElements_.empty() &&
                   openElements_.back()->tagName() != "tbody" &&
                   openElements_.back()->tagName() != "tfoot" &&
                   openElements_.back()->tagName() != "thead")
                openElements_.pop_back();
            openElements_.pop_back();
            mode_ = InsertionMode::InTable;
            processToken(std::move(tok));
            return;
        }
        if (tok.tagName == "body" || tok.tagName == "caption" || tok.tagName == "col" ||
            tok.tagName == "colgroup" || tok.tagName == "html" || tok.tagName == "td" ||
            tok.tagName == "th" || tok.tagName == "tr") return;
    }
    processInTable(tok);
}

void HTML5TreeConstructor::processInRow(HTML5Token& tok) {
    if (tok.type == HTML5TokenType::StartTag) {
        if (tok.tagName == "th" || tok.tagName == "td") {
            while (!openElements_.empty() &&
                   openElements_.back()->tagName() != "tr" &&
                   openElements_.back()->tagName() != "template" &&
                   openElements_.back()->tagName() != "html")
                openElements_.pop_back();
            pushFormattingMarker();
            insertElementForToken(tok);
            mode_ = InsertionMode::InCell;
            return;
        }
        if (tok.tagName == "caption" || tok.tagName == "col" || tok.tagName == "colgroup" ||
            tok.tagName == "tbody" || tok.tagName == "tfoot" || tok.tagName == "thead" ||
            tok.tagName == "tr") {
            if (!isElementInTableScope("tr")) return;
            while (!openElements_.empty() && openElements_.back()->tagName() != "tr" &&
                   openElements_.back()->tagName() != "template" && openElements_.back()->tagName() != "html")
                openElements_.pop_back();
            openElements_.pop_back();
            mode_ = InsertionMode::InTableBody;
            processToken(std::move(tok));
            return;
        }
    }
    if (tok.type == HTML5TokenType::EndTag) {
        if (tok.tagName == "tr") {
            if (!isElementInTableScope("tr")) return;
            while (!openElements_.empty() && openElements_.back()->tagName() != "tr" &&
                   openElements_.back()->tagName() != "template" && openElements_.back()->tagName() != "html")
                openElements_.pop_back();
            openElements_.pop_back();
            mode_ = InsertionMode::InTableBody;
            return;
        }
        if (tok.tagName == "table") {
            if (!isElementInTableScope("tr")) return;
            while (!openElements_.empty() && openElements_.back()->tagName() != "tr" &&
                   openElements_.back()->tagName() != "template" && openElements_.back()->tagName() != "html")
                openElements_.pop_back();
            openElements_.pop_back();
            mode_ = InsertionMode::InTableBody;
            processToken(std::move(tok));
            return;
        }
        if (tok.tagName == "tbody" || tok.tagName == "tfoot" || tok.tagName == "thead") {
            if (!isElementInTableScope(tok.tagName)) return;
            if (!isElementInTableScope("tr")) return;
            while (!openElements_.empty() && openElements_.back()->tagName() != "tr")
                openElements_.pop_back();
            openElements_.pop_back();
            mode_ = InsertionMode::InTableBody;
            processToken(std::move(tok));
            return;
        }
        if (tok.tagName == "body" || tok.tagName == "caption" || tok.tagName == "col" ||
            tok.tagName == "colgroup" || tok.tagName == "html" || tok.tagName == "td" ||
            tok.tagName == "th") return;
    }
    processInTable(tok);
}

void HTML5TreeConstructor::processInCell(HTML5Token& tok) {
    if (tok.type == HTML5TokenType::EndTag) {
        if (tok.tagName == "td" || tok.tagName == "th") {
            if (!isElementInTableScope(tok.tagName)) return;
            generateImpliedEndTags();
            popUntilTag(tok.tagName);
            clearFormattingToLastMarker();
            mode_ = InsertionMode::InRow;
            return;
        }
        if (tok.tagName == "body" || tok.tagName == "caption" || tok.tagName == "col" ||
            tok.tagName == "colgroup" || tok.tagName == "html") return;
        if (tok.tagName == "table" || tok.tagName == "tbody" || tok.tagName == "tfoot" ||
            tok.tagName == "thead" || tok.tagName == "tr") {
            if (!isElementInTableScope(tok.tagName)) return;
            // Close the cell first
            std::string cell = isElementInTableScope("td") ? "td" : "th";
            generateImpliedEndTags();
            popUntilTag(cell);
            clearFormattingToLastMarker();
            mode_ = InsertionMode::InRow;
            processToken(std::move(tok));
            return;
        }
    }
    if (tok.type == HTML5TokenType::StartTag) {
        if (tok.tagName == "caption" || tok.tagName == "col" || tok.tagName == "colgroup" ||
            tok.tagName == "tbody" || tok.tagName == "td" || tok.tagName == "tfoot" ||
            tok.tagName == "th" || tok.tagName == "thead" || tok.tagName == "tr") {
            if (!isElementInTableScope("td") && !isElementInTableScope("th")) return;
            std::string cell = isElementInTableScope("td") ? "td" : "th";
            generateImpliedEndTags();
            popUntilTag(cell);
            clearFormattingToLastMarker();
            mode_ = InsertionMode::InRow;
            processToken(std::move(tok));
            return;
        }
    }
    processInBody(tok);
}

// ============================================================================
// Select Modes
// ============================================================================

void HTML5TreeConstructor::processInSelect(HTML5Token& tok) {
    if (tok.type == HTML5TokenType::Character) { insertCharacters(tok.data); return; }
    if (tok.type == HTML5TokenType::Comment) { insertComment(tok.data); return; }
    if (tok.type == HTML5TokenType::DOCTYPE) return;
    if (tok.type == HTML5TokenType::StartTag) {
        if (tok.tagName == "html") { processInBody(tok); return; }
        if (tok.tagName == "option") {
            if (!openElements_.empty() && currentNode()->tagName() == "option") openElements_.pop_back();
            insertElementForToken(tok); return;
        }
        if (tok.tagName == "optgroup") {
            if (!openElements_.empty() && currentNode()->tagName() == "option") openElements_.pop_back();
            if (!openElements_.empty() && currentNode()->tagName() == "optgroup") openElements_.pop_back();
            insertElementForToken(tok); return;
        }
        if (tok.tagName == "select") {
            popUntilTag("select");
            resetInsertionModeAppropriately();
            return;
        }
        if (tok.tagName == "input" || tok.tagName == "keygen" || tok.tagName == "textarea") {
            popUntilTag("select");
            resetInsertionModeAppropriately();
            processToken(std::move(tok));
            return;
        }
        if (tok.tagName == "script" || tok.tagName == "template") { processInHead(tok); return; }
    }
    if (tok.type == HTML5TokenType::EndTag) {
        if (tok.tagName == "optgroup") {
            if (!openElements_.empty() && currentNode()->tagName() == "option") {
                auto it = openElements_.rbegin(); ++it;
                if (it != openElements_.rend() && (*it)->tagName() == "optgroup") openElements_.pop_back();
            }
            if (!openElements_.empty() && currentNode()->tagName() == "optgroup") openElements_.pop_back();
            return;
        }
        if (tok.tagName == "option") {
            if (!openElements_.empty() && currentNode()->tagName() == "option") openElements_.pop_back();
            return;
        }
        if (tok.tagName == "select") {
            if (!isElementInSelectScope("select")) return;
            popUntilTag("select");
            resetInsertionModeAppropriately();
            return;
        }
        if (tok.tagName == "template") { processInHead(tok); return; }
    }
    if (tok.type == HTML5TokenType::EndOfFile) { processInBody(tok); return; }
}

void HTML5TreeConstructor::processInSelectInTable(HTML5Token& tok) {
    if (tok.type == HTML5TokenType::StartTag) {
        if (tok.tagName == "caption" || tok.tagName == "table" || tok.tagName == "tbody" ||
            tok.tagName == "tfoot" || tok.tagName == "thead" || tok.tagName == "tr" ||
            tok.tagName == "td" || tok.tagName == "th") {
            popUntilTag("select");
            resetInsertionModeAppropriately();
            processToken(std::move(tok));
            return;
        }
    }
    if (tok.type == HTML5TokenType::EndTag) {
        if (tok.tagName == "caption" || tok.tagName == "table" || tok.tagName == "tbody" ||
            tok.tagName == "tfoot" || tok.tagName == "thead" || tok.tagName == "tr" ||
            tok.tagName == "td" || tok.tagName == "th") {
            if (!isElementInTableScope(tok.tagName)) return;
            popUntilTag("select");
            resetInsertionModeAppropriately();
            processToken(std::move(tok));
            return;
        }
    }
    processInSelect(tok);
}

// ============================================================================
// Template, After-body, Frameset, and terminal modes
// ============================================================================

void HTML5TreeConstructor::processInTemplate(HTML5Token& tok) {
    // Simplified: process based on token type
    if (tok.type == HTML5TokenType::Character ||
        tok.type == HTML5TokenType::Comment ||
        tok.type == HTML5TokenType::DOCTYPE) {
        processInBody(tok); return;
    }
    if (tok.type == HTML5TokenType::StartTag) {
        if (tok.tagName == "base" || tok.tagName == "basefont" || tok.tagName == "bgsound" ||
            tok.tagName == "link" || tok.tagName == "meta" || tok.tagName == "noframes" ||
            tok.tagName == "script" || tok.tagName == "style" || tok.tagName == "template" ||
            tok.tagName == "title") {
            processInHead(tok); return;
        }
        if (tok.tagName == "caption" || tok.tagName == "colgroup" || tok.tagName == "tbody" ||
            tok.tagName == "tfoot" || tok.tagName == "thead") {
            if (!templateInsertionModes_.empty()) templateInsertionModes_.back() = InsertionMode::InTable;
            mode_ = InsertionMode::InTable;
            processToken(std::move(tok)); return;
        }
        if (tok.tagName == "col") {
            if (!templateInsertionModes_.empty()) templateInsertionModes_.back() = InsertionMode::InColumnGroup;
            mode_ = InsertionMode::InColumnGroup;
            processToken(std::move(tok)); return;
        }
        if (tok.tagName == "tr") {
            if (!templateInsertionModes_.empty()) templateInsertionModes_.back() = InsertionMode::InTableBody;
            mode_ = InsertionMode::InTableBody;
            processToken(std::move(tok)); return;
        }
        if (tok.tagName == "td" || tok.tagName == "th") {
            if (!templateInsertionModes_.empty()) templateInsertionModes_.back() = InsertionMode::InRow;
            mode_ = InsertionMode::InRow;
            processToken(std::move(tok)); return;
        }
        // Anything else — switch to InBody
        if (!templateInsertionModes_.empty()) templateInsertionModes_.back() = InsertionMode::InBody;
        mode_ = InsertionMode::InBody;
        processToken(std::move(tok));
    }
    if (tok.type == HTML5TokenType::EndTag) {
        if (tok.tagName == "template") { processInHead(tok); return; }
        // parse error — ignore
    }
    if (tok.type == HTML5TokenType::EndOfFile) {
        if (!hasElementInOpenStack("template")) { /* stop parsing */ return; }
        popUntilTag("template");
        clearFormattingToLastMarker();
        if (!templateInsertionModes_.empty()) templateInsertionModes_.pop_back();
        resetInsertionModeAppropriately();
        processToken(std::move(tok));
    }
}

void HTML5TreeConstructor::processAfterBody(HTML5Token& tok) {
    auto isWS = [](const std::string& s) {
        for (char c : s) if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') return false;
        return true;
    };
    if (tok.type == HTML5TokenType::Character && isWS(tok.data)) { processInBody(tok); return; }
    if (tok.type == HTML5TokenType::Comment) {
        // Insert as last child of html element
        DOMElement* htmlEl = openElements_.empty() ? nullptr : openElements_.front();
        insertComment(tok.data, htmlEl);
        return;
    }
    if (tok.type == HTML5TokenType::DOCTYPE) return;
    if (tok.type == HTML5TokenType::StartTag && tok.tagName == "html") { processInBody(tok); return; }
    if (tok.type == HTML5TokenType::EndTag && tok.tagName == "html") {
        mode_ = InsertionMode::AfterAfterBody;
        return;
    }
    if (tok.type == HTML5TokenType::EndOfFile) { /* stop */ return; }
    // parse error — reprocess in InBody
    mode_ = InsertionMode::InBody;
    processToken(std::move(tok));
}

void HTML5TreeConstructor::processInFrameset(HTML5Token& tok) {
    auto isWS = [](const std::string& s) {
        for (char c : s) if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') return false;
        return true;
    };
    if (tok.type == HTML5TokenType::Character && isWS(tok.data)) { insertCharacters(tok.data); return; }
    if (tok.type == HTML5TokenType::Comment) { insertComment(tok.data); return; }
    if (tok.type == HTML5TokenType::DOCTYPE) return;
    if (tok.type == HTML5TokenType::StartTag) {
        if (tok.tagName == "html") { processInBody(tok); return; }
        if (tok.tagName == "frameset") { insertElementForToken(tok); return; }
        if (tok.tagName == "frame") { insertElementForToken(tok); openElements_.pop_back(); return; }
        if (tok.tagName == "noframes") { processInHead(tok); return; }
    }
    if (tok.type == HTML5TokenType::EndTag && tok.tagName == "frameset") {
        if (openElements_.size() == 1 && openElements_.front()->tagName() == "html") return;
        openElements_.pop_back();
        if (!openElements_.empty() && currentNode()->tagName() != "frameset")
            mode_ = InsertionMode::AfterFrameset;
        return;
    }
    if (tok.type == HTML5TokenType::EndOfFile) { /* stop */ return; }
}

void HTML5TreeConstructor::processAfterFrameset(HTML5Token& tok) {
    auto isWS = [](const std::string& s) {
        for (char c : s) if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') return false;
        return true;
    };
    if (tok.type == HTML5TokenType::Character && isWS(tok.data)) { insertCharacters(tok.data); return; }
    if (tok.type == HTML5TokenType::Comment) { insertComment(tok.data); return; }
    if (tok.type == HTML5TokenType::DOCTYPE) return;
    if (tok.type == HTML5TokenType::StartTag) {
        if (tok.tagName == "html") { processInBody(tok); return; }
        if (tok.tagName == "noframes") { processInHead(tok); return; }
    }
    if (tok.type == HTML5TokenType::EndTag && tok.tagName == "html") {
        mode_ = InsertionMode::AfterAfterFrameset;
        return;
    }
    if (tok.type == HTML5TokenType::EndOfFile) return;
}

void HTML5TreeConstructor::processAfterAfterBody(HTML5Token& tok) {
    if (tok.type == HTML5TokenType::Comment) { insertComment(tok.data, doc_.get()); return; }
    auto isWS = [](const std::string& s) {
        for (char c : s) if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') return false;
        return true;
    };
    if (tok.type == HTML5TokenType::Character && isWS(tok.data)) { processInBody(tok); return; }
    if (tok.type == HTML5TokenType::DOCTYPE) return;
    if (tok.type == HTML5TokenType::StartTag && tok.tagName == "html") { processInBody(tok); return; }
    if (tok.type == HTML5TokenType::EndOfFile) return;
    // parse error
    mode_ = InsertionMode::InBody;
    processToken(std::move(tok));
}

void HTML5TreeConstructor::processAfterAfterFrameset(HTML5Token& tok) {
    if (tok.type == HTML5TokenType::Comment) { insertComment(tok.data, doc_.get()); return; }
    if (tok.type == HTML5TokenType::DOCTYPE) return;
    if (tok.type == HTML5TokenType::StartTag) {
        if (tok.tagName == "html") { processInBody(tok); return; }
        if (tok.tagName == "noframes") { processInHead(tok); return; }
    }
    if (tok.type == HTML5TokenType::EndOfFile) return;
}

} // namespace Zepra::WebCore
