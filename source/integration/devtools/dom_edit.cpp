// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file dom_edit.cpp
 * @brief The markup view's model: DOM edits with undo, Copy CSS Selector,
 *        Copy outerHTML and the markup search.
 */
#include "dom_edit.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include "browser/dom.hpp"
#include "css/css_engine.hpp"
#include "html/html_parser.hpp"
#include "html/html_tokenizer.hpp"

using namespace Zepra::WebCore;

namespace ZepraBrowser::DevTools {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n\f");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n\f");
    return s.substr(start, end - start + 1);
}

bool equalsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

/// ASCII case-insensitive substring test; `needleLower` is already lowercase
bool containsIgnoreCase(const std::string& haystack, const std::string& needleLower) {
    if (needleLower.empty()) return true;
    auto it = std::search(haystack.begin(), haystack.end(), needleLower.begin(), needleLower.end(),
                          [](char h, char n) { return std::tolower(static_cast<unsigned char>(h)) == n; });
    return it != haystack.end();
}

DOMElement* asElement(DOMNode* node) {
    return node && node->nodeType() == NodeType::Element ? static_cast<DOMElement*>(node) : nullptr;
}

/// The node at the top of a node's tree
DOMNode* treeRoot(DOMNode* node) {
    while (node && node->parentNode()) node = node->parentNode();
    return node;
}

bool isChildOf(const DOMNode* parent, const DOMNode* node) {
    for (const auto& child : parent->childNodes()) {
        if (child.get() == node) return true;
    }
    return false;
}

/// Calls `visit` on `root` and every node below it in document order
/// (iterative: deep pages must not overflow the stack); stops when it returns false
template <typename Visit>
void walk(DOMNode* root, Visit&& visit) {
    if (!root) return;
    std::vector<std::pair<DOMNode*, size_t>> stack;
    if (!visit(root)) return;
    stack.emplace_back(root, 0);
    while (!stack.empty()) {
        auto& [node, next] = stack.back();
        const auto& kids = node->childNodes();
        if (next >= kids.size()) {
            stack.pop_back();
            continue;
        }
        DOMNode* child = kids[next++].get();
        if (!visit(child)) return;
        if (child->hasChildNodes()) stack.emplace_back(child, 0);
    }
}

/// Lets go of hover / active / focus state that points into a subtree about to be freed
void forgetInteraction(DOMNode* root) {
    InteractionState& state = interactionState();
    for (DOMElement** p : {&state.hovered, &state.active, &state.focused}) {
        if (*p && root->contains(*p)) *p = nullptr;
    }
}

std::vector<std::unique_ptr<DOMNode>> parseHTML(const std::string& html, DOMNode* context) {
    DOMElement* el = asElement(context);
    HTMLParser parser;
    return parser.parseFragmentNodes(html, el ? el->tagName() : "body");
}

}  // namespace

// ============================================================================
// Attribute text
// ============================================================================

std::vector<AttributeText> parseAttributeText(const std::string& text) {
    std::vector<AttributeText> result;
    if (trim(text).empty()) return result;
    // The tokenizer reads the text as the attributes of a start tag. A value
    // left open drops the whole tag at the end of input, so close it.
    for (const char* closing : {">", "\">", "'>"}) {
        std::string source = "<x " + text + closing;
        bool found = false;
        HTML5Tokenizer tokenizer(source);
        tokenizer.setTokenHandler([&](HTML5Token&& token) {
            if (found || token.type != HTML5TokenType::StartTag) return;
            found = true;
            result = std::move(token.attributes);
        });
        tokenizer.tokenize();
        if (found) break;
    }
    return result;
}

bool isProtectedNode(const DOMNode* node) {
    if (!node || node->nodeType() == NodeType::Document) return true;
    if (!node->parentNode()) return true;
    if (node->nodeType() == NodeType::Element) {
        std::string tag = lower(static_cast<const DOMElement*>(node)->tagName());
        if (tag == "html" || tag == "head" || tag == "body") return true;
    }
    return false;
}

// ============================================================================
// EditHistory
// ============================================================================

struct EditHistory::Record {
    enum class Kind { Attributes, Text, Structure };
    Kind kind = Kind::Structure;

    // Attributes: the element's attributes before the edit
    DOMElement* element = nullptr;
    std::unordered_map<std::string, std::string> attributes;

    // Text: the node's data before the edit
    DOMNode* textNode = nullptr;
    std::string text;

    // Structure: `removed` were taken out of `parent` and `inserted` put in
    // their place, just before `anchor` (null = at the end)
    DOMNode* parent = nullptr;
    DOMNode* anchor = nullptr;
    std::vector<std::unique_ptr<DOMNode>> removed;
    std::vector<DOMNode*> inserted;
};

EditHistory::EditHistory(DOMDocument* document, size_t limit)
    : document_(document), limit_(std::max<size_t>(limit, 1)) {}

EditHistory::~EditHistory() {
    for (auto& record : records_) discardRecord(*record, false);
    records_.clear();
    releaseDetached();
}

void EditHistory::setDocument(DOMDocument* document) {
    for (auto& record : records_) discardRecord(*record, false);
    records_.clear();
    document_ = document;
}

void EditHistory::discard(std::unique_ptr<DOMNode> root, bool useSink) {
    if (!root) return;
    if (useSink && sink_) sink_(std::move(root));
    else detached_.push_back(std::move(root));
}

void EditHistory::discardRecord(Record& record, bool useSink) {
    for (auto& root : record.removed) discard(std::move(root), useSink);
    record.removed.clear();
}

void EditHistory::push(std::unique_ptr<Record> record) {
    records_.push_back(std::move(record));
    while (records_.size() > limit_) {
        discardRecord(*records_.front());
        records_.erase(records_.begin());
    }
}

bool EditHistory::isLive(const DOMNode* node) const {
    if (!node) return false;
    auto* n = const_cast<DOMNode*>(node);
    if (document_ && (n == document_ || document_->contains(n))) return true;
    return keepsDetached(node);
}

bool EditHistory::keepsDetached(const DOMNode* node) const {
    if (!node) return false;
    auto* n = const_cast<DOMNode*>(node);
    for (const auto& record : records_) {
        for (const auto& root : record->removed) {
            if (root && root->contains(n)) return true;
        }
    }
    for (const auto& root : detached_) {
        if (root->contains(n)) return true;
    }
    return false;
}

void EditHistory::releaseDetached() {
    for (const auto& root : detached_) forgetInteraction(root.get());
    detached_.clear();
}

void EditHistory::clear() {
    for (auto& record : records_) discardRecord(*record);
    records_.clear();
}

// ---- Attributes ------------------------------------------------------------

bool EditHistory::attributeEdit(DOMElement* element, const std::function<bool(DOMElement*)>& change) {
    if (!element) return false;
    auto record = std::make_unique<Record>();
    record->kind = Record::Kind::Attributes;
    record->element = element;
    record->attributes = element->attributes();
    if (!change(element)) return false;
    push(std::move(record));
    return true;
}

bool EditHistory::setAttribute(DOMElement* element, const std::string& name, const std::string& value) {
    if (name.empty()) return false;
    return attributeEdit(element, [&](DOMElement* e) {
        e->setAttribute(name, value);
        return true;
    });
}

bool EditHistory::renameAttribute(DOMElement* element, const std::string& oldName, const std::string& newName) {
    if (!element || newName.empty() || !element->hasAttribute(oldName)) return false;
    if (oldName == newName) return true;
    return attributeEdit(element, [&](DOMElement* e) {
        std::string value = e->getAttribute(oldName);
        e->removeAttribute(oldName);
        e->setAttribute(newName, value);
        return true;
    });
}

bool EditHistory::removeAttribute(DOMElement* element, const std::string& name) {
    if (!element || !element->hasAttribute(name)) return false;
    return attributeEdit(element, [&](DOMElement* e) {
        e->removeAttribute(name);
        return true;
    });
}

bool EditHistory::editAttribute(DOMElement* element, const std::string& oldName, const std::string& typedText) {
    if (!element) return false;
    std::vector<AttributeText> typed = parseAttributeText(typedText);
    bool removesOld = !oldName.empty() && element->hasAttribute(oldName);
    if (typed.empty() && !removesOld) return false;
    return attributeEdit(element, [&](DOMElement* e) {
        if (removesOld) e->removeAttribute(oldName);
        for (const auto& [name, value] : typed) e->setAttribute(name, value);
        return true;
    });
}

// ---- Text ------------------------------------------------------------------

bool EditHistory::setText(DOMNode* node, const std::string& text) {
    if (!node || (node->nodeType() != NodeType::Text && node->nodeType() != NodeType::Comment)) return false;
    auto record = std::make_unique<Record>();
    record->kind = Record::Kind::Text;
    record->textNode = node;
    record->text = node->nodeValue();
    node->setNodeValue(text);   // DOMText / DOMComment bump domMutationVersion()
    push(std::move(record));
    return true;
}

// ---- Structure -------------------------------------------------------------

DOMNode* EditHistory::replaceWithHTML(DOMNode* node, const std::string& html) {
    if (isProtectedNode(node)) return nullptr;
    DOMNode* parent = node->parentNode();
    if (parent->nodeType() == NodeType::Document) return nullptr;
    if (document_ && treeRoot(node) != document_) return nullptr;

    auto record = std::make_unique<Record>();
    record->parent = parent;
    record->anchor = node->nextSibling();
    for (auto& fresh : parseHTML(html, parent)) {
        record->inserted.push_back(parent->insertBefore(std::move(fresh), node));
    }
    record->removed.push_back(parent->removeChild(node));
    DOMNode* first = record->inserted.empty() ? nullptr : record->inserted.front();
    push(std::move(record));
    return first;
}

bool EditHistory::setInnerHTML(DOMElement* element, const std::string& html) {
    if (!element || !element->parentNode() || lower(element->tagName()) == "html") return false;
    if (document_ && treeRoot(element) != document_) return false;

    auto record = std::make_unique<Record>();
    record->parent = element;
    while (DOMNode* first = element->firstChild()) record->removed.push_back(element->removeChild(first));
    for (auto& fresh : parseHTML(html, element)) record->inserted.push_back(element->appendChild(std::move(fresh)));
    // Even an edit that changed nothing visible bumps the version, so the view refreshes
    if (record->removed.empty() && record->inserted.empty()) noteDomMutation();
    push(std::move(record));
    return true;
}

bool EditHistory::removeNode(DOMNode* node) {
    if (isProtectedNode(node)) return false;
    if (document_ && treeRoot(node) != document_) return false;
    DOMNode* parent = node->parentNode();
    auto record = std::make_unique<Record>();
    record->parent = parent;
    record->anchor = node->nextSibling();
    record->removed.push_back(parent->removeChild(node));
    push(std::move(record));
    return true;
}

DOMNode* EditHistory::duplicateNode(DOMNode* node) {
    if (isProtectedNode(node)) return nullptr;
    DOMNode* parent = node->parentNode();
    if (parent->nodeType() == NodeType::Document) return nullptr;
    if (document_ && treeRoot(node) != document_) return nullptr;
    auto record = std::make_unique<Record>();
    record->parent = parent;
    record->anchor = node->nextSibling();
    DOMNode* clone = parent->insertBefore(node->cloneNode(true), record->anchor);
    record->inserted.push_back(clone);
    push(std::move(record));
    return clone;
}

// ---- Undo ------------------------------------------------------------------

DOMNode* EditHistory::undo() {
    if (records_.empty()) return nullptr;
    std::unique_ptr<Record> record = std::move(records_.back());
    records_.pop_back();

    switch (record->kind) {
        case Record::Kind::Attributes: {
            // The pointer is only followed once the tree (or the history) is known to hold it
            if (!isLive(record->element)) return nullptr;
            DOMElement* e = record->element;
            std::vector<std::string> current;
            for (const auto& attribute : e->attributes()) current.push_back(attribute.first);
            for (const auto& name : current) {
                if (!record->attributes.count(name)) e->removeAttribute(name);
            }
            for (const auto& [name, value] : record->attributes) {
                if (!e->hasAttribute(name) || e->getAttribute(name) != value) e->setAttribute(name, value);
            }
            noteDomMutation();
            return e;
        }
        case Record::Kind::Text:
            if (!isLive(record->textNode)) return nullptr;
            record->textNode->setNodeValue(record->text);
            return record->textNode;
        case Record::Kind::Structure:
            break;
    }

    DOMNode* parent = record->parent;
    if (!isLive(parent)) {
        discardRecord(*record);
        return nullptr;
    }
    // Where the removed nodes go back: before the anchor while it is still
    // there, else after what is left of the inserted nodes, else at the end
    DOMNode* before = nullptr;
    if (record->anchor && isChildOf(parent, record->anchor)) {
        before = record->anchor;
    } else {
        for (auto it = record->inserted.rbegin(); it != record->inserted.rend(); ++it) {
            if (isChildOf(parent, *it)) {
                const auto& kids = parent->childNodes();
                for (size_t i = 0; i < kids.size(); i++) {
                    if (kids[i].get() == *it) {
                        before = i + 1 < kids.size() ? kids[i + 1].get() : nullptr;
                        break;
                    }
                }
                break;
            }
        }
    }
    // What the undo touched: the first node put back, else (an undone
    // duplicate) the node before the first inserted one, else the parent
    DOMNode* touched = parent;
    if (!record->removed.empty()) {
        touched = record->removed.front().get();
    } else if (!record->inserted.empty() && isChildOf(parent, record->inserted.front())) {
        if (DOMNode* previous = record->inserted.front()->previousSibling()) touched = previous;
    }
    // The inserted nodes still in place come out (a script may have moved some);
    // the layout may still point at them until the next relayout
    for (DOMNode* added : record->inserted) {
        if (!isChildOf(parent, added)) continue;
        if (added == before) before = added->nextSibling();
        discard(parent->removeChild(added));
    }
    for (auto& node : record->removed) parent->insertBefore(std::move(node), before);
    record->removed.clear();
    if (record->inserted.empty() && touched == parent) noteDomMutation();
    return touched;
}

// ============================================================================
// The panel's edits (process-wide history)
// ============================================================================

EditHistory& sharedEditHistory() {
    // Never destroyed: freeing nodes at exit would race other statics (interaction state)
    static EditHistory* history = new EditHistory();
    return *history;
}

void resetEditHistory(DOMDocument* document, EditHistory::DetachedSink sink) {
    EditHistory& history = sharedEditHistory();
    history.releaseDetached();
    history.setDocument(document);
    history.setDetachedSink(std::move(sink));
}

void releaseDetachedEditNodes() {
    sharedEditHistory().releaseDetached();
}

namespace {

/// The shared history, started over when the node is in another document
EditHistory& historyFor(DOMNode* node) {
    EditHistory& history = sharedEditHistory();
    DOMNode* root = treeRoot(node);
    if (root && root->nodeType() == NodeType::Document && root != history.document()) {
        resetEditHistory(static_cast<DOMDocument*>(root));
    }
    return history;
}

}  // namespace

void replaceAttribute(DOMElement* el, const std::string& oldName, const std::string& typedText) {
    if (el) historyFor(el).editAttribute(el, oldName, typedText);
}

void setText(DOMText* text, const std::string& data) {
    if (text) historyFor(text).setText(text, data);
}

DOMNode* replaceWithHtml(DOMNode* node, const std::string& html) {
    return node ? historyFor(node).replaceWithHTML(node, html) : nullptr;
}

bool removeNode(DOMNode* node) {
    return node && historyFor(node).removeNode(node);
}

DOMNode* duplicateNode(DOMNode* node) {
    return node ? historyFor(node).duplicateNode(node) : nullptr;
}

DOMNode* undoLastEdit() {
    EditHistory& history = sharedEditHistory();
    while (history.canUndo()) {
        if (DOMNode* touched = history.undo()) return touched;
    }
    return nullptr;
}

// ============================================================================
// Copy CSS Selector
// ============================================================================

namespace {

bool isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '-';
}
bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
}

/// An identifier the matcher reads without escapes (and CSS accepts)
bool isPlainIdent(const std::string& s) {
    if (s.empty() || !isIdentStart(s[0])) return false;
    if (s[0] == '-' && (s.size() == 1 || std::isdigit(static_cast<unsigned char>(s[1])))) return false;
    return std::all_of(s.begin(), s.end(), isIdentChar);
}

/// Punctuation a class name may carry escaped ("md\:p-4", "w-1\/2"): what
/// the matcher's combinator and group splitting does not trip over
bool isEscapable(char c) {
    if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7f) return false;
    if (isIdentChar(c)) return false;
    return std::string(" >+~,[]()").find(c) == std::string::npos;
}

/// ".name" with CSS escapes, or empty when the matcher could not read it back
std::string classSelector(const std::string& name) {
    if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0]))) return "";
    if (name[0] == '-' && name.size() > 1 && std::isdigit(static_cast<unsigned char>(name[1]))) return "";
    std::string out = ".";
    for (char c : name) {
        if (isIdentChar(c)) out += c;
        else if (isEscapable(c)) { out += '\\'; out += c; }
        else return "";
    }
    return out;
}

std::string idSelector(const std::string& id) {
    if (isPlainIdent(id)) return "#" + id;
    if (id.empty() || id.find_first_of("\"],()\\") != std::string::npos) return "";
    return "[id=\"" + id + "\"]";
}

class SelectorBuilder {
public:
    explicit SelectorBuilder(DOMNode* root) : root_(root) {}

    /// Whether `selector` matches `target` and no other element of the tree
    bool unique(const std::string& selector, DOMElement* target) {
        if (selector.empty() || !cascade_.selectorMatches(target, selector)) return false;
        bool other = false;
        walk(root_, [&](DOMNode* node) {
            DOMElement* el = asElement(node);
            if (el && el != target && cascade_.selectorMatches(el, selector)) other = true;
            return !other;
        });
        return !other;
    }

    std::string build(DOMElement* el) {
        if (el->hasAttribute("id")) {
            std::string sel = idSelector(el->getAttribute("id"));
            if (unique(sel, el)) return sel;
        }
        std::string tag = lower(el->tagName());
        if (unique(tag, el)) return tag;

        std::vector<std::string> classes;
        for (const auto& name : el->classList()) {
            std::string sel = classSelector(name);
            if (!sel.empty() && std::find(classes.begin(), classes.end(), sel) == classes.end()) classes.push_back(sel);
            if (classes.size() == 8) break;   // bounded work on utility-class soup
        }
        for (const auto& cls : classes) {
            if (unique(cls, el)) return cls;
        }
        for (const auto& cls : classes) {
            if (unique(tag + cls, el)) return tag + cls;
        }
        if (classes.size() > 1) {
            std::string all;
            for (const auto& cls : classes) all += cls;
            if (unique(all, el)) return all;
            if (unique(tag + all, el)) return tag + all;
        }

        std::string step = tag + ":nth-child(" + std::to_string(elementIndex(el)) + ")";
        DOMElement* parent = asElement(el->parentNode());
        if (!parent) return step;
        return build(parent) + " > " + step;
    }

    /// The full path from the root element, by position only
    static std::string path(DOMElement* el) {
        std::string out;
        for (DOMElement* e = el; e; e = asElement(e->parentNode())) {
            std::string step = lower(e->tagName());
            if (asElement(e->parentNode())) step += ":nth-child(" + std::to_string(elementIndex(e)) + ")";
            out = out.empty() ? step : step + " > " + out;
        }
        return out;
    }

private:
    DOMNode* root_;
    CSSCascade cascade_;

    static size_t elementIndex(DOMElement* el) {
        size_t index = 1;
        DOMNode* parent = el->parentNode();
        if (!parent) return index;
        for (const auto& child : parent->childNodes()) {
            if (child.get() == el) break;
            if (child->nodeType() == NodeType::Element) index++;
        }
        return index;
    }
};

}  // namespace

std::string uniqueSelector(DOMElement* element) {
    if (!element) return "";
    DOMNode* root = treeRoot(element);
    if (!root || root->nodeType() != NodeType::Document) return "";
    SelectorBuilder builder(root);
    std::string selector = builder.build(element);
    if (builder.unique(selector, element)) return selector;
    // A step the matcher reads differently: fall back to the position path
    return SelectorBuilder::path(element);
}

// ============================================================================
// Copy outerHTML
// ============================================================================

namespace {

std::string escapeText(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c;
        }
    }
    return out;
}

}  // namespace

std::string outerHtmlOf(DOMNode* node) {
    return outerHTMLOf(node);
}

std::string outerHTMLOf(const DOMNode* node) {
    if (!node) return "";
    switch (node->nodeType()) {
        case NodeType::Element:
            return static_cast<const DOMElement*>(node)->outerHTML();
        case NodeType::Text: {
            // As the DOM serializes it: script and style text is raw text
            const DOMElement* parent = asElement(node->parentNode());
            std::string tag = parent ? lower(parent->tagName()) : "";
            return tag == "script" || tag == "style" ? node->nodeValue() : escapeText(node->nodeValue());
        }
        case NodeType::Comment:
            return "<!--" + node->nodeValue() + "-->";
        case NodeType::Document:
        case NodeType::DocumentFragment: {
            std::string out;
            for (const auto& child : node->childNodes()) out += outerHTMLOf(child.get());
            return out;
        }
    }
    return "";
}

// ============================================================================
// Markup search
// ============================================================================

namespace {

class SelectorValidator {
public:
    explicit SelectorValidator(const std::string& s) : s_(s) {}

    bool list(char terminator = '\0') {
        do {
            skipSpace();
            if (!complex(terminator)) return false;
            skipSpace();
        } while (eat(','));
        return terminator ? peek() == terminator : at_end();
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    bool at_end() const { return i_ >= s_.size(); }
    char peek() const { return at_end() ? '\0' : s_[i_]; }
    bool eat(char c) {
        if (peek() != c) return false;
        i_++;
        return true;
    }
    bool skipSpace() {
        size_t start = i_;
        while (peek() == ' ' || peek() == '\t') i_++;
        return i_ > start;
    }

    bool ident(std::string* out = nullptr) {
        size_t start = i_;
        if (!isIdentStart(peek())) return false;
        i_++;
        while (isIdentChar(peek())) i_++;
        std::string name = s_.substr(start, i_ - start);
        if (name == "-" || (name[0] == '-' && name.size() > 1 && std::isdigit(static_cast<unsigned char>(name[1])))) return false;
        if (out) *out = name;
        return true;
    }

    bool className() {
        size_t start = i_;
        bool first = true;
        while (!at_end()) {
            char c = peek();
            if (c == '\\') {
                if (i_ + 1 >= s_.size() || !isEscapable(s_[i_ + 1])) return false;
                i_ += 2;
            } else if (first ? isIdentStart(c) : isIdentChar(c)) {
                i_++;
            } else {
                break;
            }
            first = false;
        }
        return i_ > start;
    }

    bool attribute() {
        skipSpace();
        if (!ident()) return false;
        skipSpace();
        if (eat(']')) return true;
        if (!eat('=')) return false;   // ^= $= *= ~= |= are not matched by the engine
        skipSpace();
        char q = peek();
        if (q == '"' || q == '\'') {
            i_++;
            while (!at_end() && peek() != q) {
                if (std::string("],()\\").find(peek()) != std::string::npos) return false;
                i_++;
            }
            if (!eat(q)) return false;
        } else {
            size_t start = i_;
            while (isIdentChar(peek())) i_++;
            if (i_ == start) return false;
        }
        skipSpace();
        return eat(']');
    }

    bool pseudo() {
        if (eat(':')) {
            std::string name;
            if (!ident(&name)) return false;
            static const std::unordered_set<std::string> elements = {
                "before", "after", "first-line", "first-letter", "placeholder", "selection", "marker"};
            return elements.count(name) && peek() != '(';
        }
        std::string name;
        if (!ident(&name)) return false;
        static const std::unordered_set<std::string> plain = {
            "hover", "active", "focus", "focus-visible", "focus-within", "link", "any-link", "visited",
            "enabled", "disabled", "checked", "placeholder-shown", "read-only", "read-write", "target",
            "default", "indeterminate", "first-child", "last-child", "only-child", "root", "empty",
            "before", "after", "first-line", "first-letter"};
        if (plain.count(name)) return peek() != '(';
        if (!eat('(')) return false;
        skipSpace();
        bool ok = false;
        if (name == "nth-child") {
            std::string arg;
            if (ident(&arg)) {
                ok = arg == "odd" || arg == "even";
            } else {
                size_t start = i_;
                while (std::isdigit(static_cast<unsigned char>(peek()))) i_++;
                ok = i_ > start;
            }
        } else if (name == "not") {
            ok = compound();   // the engine matches :not() against one compound selector
        } else if (name == "is" || name == "matches" || name == "any" || name == "where") {
            ok = list(')');
        }
        skipSpace();
        return ok && eat(')');
    }

    bool compound() {
        size_t start = i_;
        if (eat('*')) {
        } else if (isIdentStart(peek())) {
            if (!ident()) return false;
        }
        while (!at_end()) {
            if (eat('#')) {
                if (!ident()) return false;
            } else if (eat('.')) {
                if (!className()) return false;
            } else if (eat('[')) {
                if (!attribute()) return false;
            } else if (eat(':')) {
                if (!pseudo()) return false;
            } else {
                break;
            }
        }
        return i_ > start;
    }

    bool complex(char terminator) {
        if (!compound()) return false;
        while (true) {
            bool space = skipSpace();
            char c = peek();
            if (c == '>' || c == '+' || c == '~') {
                i_++;
                skipSpace();
                if (!compound()) return false;
            } else if (space && c != ',' && c != terminator && c != '\0') {
                if (!compound()) return false;
            } else {
                return true;
            }
        }
    }
};

}  // namespace

bool isSearchableSelector(const std::string& query) {
    std::string q = trim(query);
    if (q.empty()) return false;
    SelectorValidator validator(q);
    return validator.list();
}

std::vector<DOMNode*> searchMarkup(DOMNode* root, const std::string& query, const MarkupSearchOptions& options) {
    std::vector<DOMNode*> results;
    std::string q = trim(query);
    if (!root || q.empty()) return results;
    std::string needle = lower(q);

    // "<div", "<div>", "</div>" ask for the tag
    std::string tagQuery = needle;
    if (!tagQuery.empty() && tagQuery.front() == '<') tagQuery.erase(0, 1);
    if (!tagQuery.empty() && tagQuery.front() == '/') tagQuery.erase(0, 1);
    if (!tagQuery.empty() && tagQuery.back() == '>') tagQuery.pop_back();
    tagQuery = trim(tagQuery);

    // The selector's matches first (one walk of the matcher), then one walk
    // in document order that keeps every node matching any way, once
    std::unordered_set<const DOMNode*> selected;
    if (options.selector && isSearchableSelector(q)) {
        CSSCascade cascade;
        walk(root, [&](DOMNode* node) {
            DOMElement* el = asElement(node);
            if (el && cascade.selectorMatches(el, q)) selected.insert(el);
            return true;
        });
    }

    walk(root, [&](DOMNode* node) {
        bool match = false;
        switch (node->nodeType()) {
            case NodeType::Text:
            case NodeType::Comment:
                match = options.text && containsIgnoreCase(node->nodeValue(), needle);
                break;
            case NodeType::Element: {
                auto* el = static_cast<DOMElement*>(node);
                match = selected.count(el) || (options.tags && !tagQuery.empty() && equalsIgnoreCase(el->tagName(), tagQuery));
                if (!match && options.attributes) {
                    for (const auto& [name, value] : el->attributes()) {
                        if (containsIgnoreCase(name, needle) || containsIgnoreCase(value, needle)) {
                            match = true;
                            break;
                        }
                    }
                }
                break;
            }
            default:
                break;
        }
        if (match) results.push_back(node);
        return options.maxResults == 0 || results.size() < options.maxResults;
    });
    return results;
}

std::vector<DOMNode*> searchMarkup(DOMDocument* doc, const std::string& query) {
    return searchMarkup(static_cast<DOMNode*>(doc), query, MarkupSearchOptions{});
}

} // namespace ZepraBrowser::DevTools
