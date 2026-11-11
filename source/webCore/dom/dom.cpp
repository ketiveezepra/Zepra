// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file dom.cpp
 * @brief DOM Node implementation stubs
 */

#include <unordered_set>
#include "browser/dom.hpp"
#include "css/css_engine.hpp"
#include "html/html_parser.hpp"
#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_map>

namespace Zepra::WebCore {

// ============================================================================
// Selector queries
// ============================================================================

bool selectorMatches(DOMElement* element, const std::string& selector) {
    CSSCascade cascade;
    return cascade.selectorMatches(element, selector);
}

std::vector<DOMElement*> selectDescendants(const DOMNode* root, const std::string& selector, bool firstOnly) {
    std::vector<DOMElement*> found;
    CSSCascade cascade;  // matches selectors as the style cascade does
    std::function<bool(const DOMNode*)> visit = [&](const DOMNode* node) {
        for (const auto& child : node->childNodes()) {
            if (child->nodeType() != NodeType::Element) continue;
            auto* element = static_cast<DOMElement*>(child.get());
            if (cascade.selectorMatches(element, selector)) {
                found.push_back(element);
                if (firstOnly) return true;
            }
            if (visit(element)) return true;
        }
        return false;
    };
    visit(root);
    return found;
}

std::vector<DOMElement*> descendantsWithClasses(const DOMNode* root, const std::string& names) {
    std::vector<std::string> wanted;
    size_t i = 0;
    while (i < names.size()) {
        while (i < names.size() && std::isspace(static_cast<unsigned char>(names[i]))) i++;
        size_t start = i;
        while (i < names.size() && !std::isspace(static_cast<unsigned char>(names[i]))) i++;
        if (i > start) wanted.push_back(names.substr(start, i - start));
    }
    std::vector<DOMElement*> found;
    if (wanted.empty()) return found;
    std::function<void(const DOMNode*)> visit = [&](const DOMNode* node) {
        for (const auto& child : node->childNodes()) {
            if (child->nodeType() != NodeType::Element) continue;
            auto* element = static_cast<DOMElement*>(child.get());
            std::vector<std::string> classes = element->classList();
            bool all = true;
            for (const auto& name : wanted) {
                if (std::find(classes.begin(), classes.end(), name) == classes.end()) { all = false; break; }
            }
            if (all) found.push_back(element);
            visit(element);
        }
    };
    visit(root);
    return found;
}

// ============================================================================
// DOMNode
// ============================================================================

DOMNode::DOMNode(NodeType type) : nodeType_(type) {}

DOMNode::~DOMNode() {
    if (binding_) binding_->nodeDestroyed();
    // Hover, active and focus are raw pointers in a process-wide state. A node can be
    // freed by many paths (a script removing it, DevTools edits, the document or a
    // tab going away) and none may leave them dangling: the next focus() or restyle
    // would read freed memory.
    InteractionState& state = interactionState();
    if (nodeType_ == NodeType::Element) {
        for (DOMElement** p : {&state.hovered, &state.active, &state.focused}) {
            if (*p && static_cast<const DOMNode*>(*p) == this) *p = nullptr;
        }
        auto& forced = state.forced;
        forced.erase(std::remove_if(forced.begin(), forced.end(),
                                    [this](const InteractionState::ForcedEntry& e) {
                                        return static_cast<const DOMNode*>(e.element) == this;
                                    }),
                     forced.end());
    }
}

void DOMNode::adopt(DOMNode* child) {
    if (child->document_ == document_) return;
    child->document_ = document_;
    for (const auto& grandchild : child->children_) child->adopt(grandchild.get());
    // A host's shadow tree and a template's contents move with it
    if (child->nodeType_ == NodeType::Element) {
        auto* element = static_cast<DOMElement*>(child);
        if (DOMShadowRoot* shadow = element->shadowRoot()) child->adopt(shadow);
        if (DOMDocumentFragment* content = element->existingTemplateContent()) child->adopt(content);
    }
}

DOMNode* DOMNode::previousSibling() const {
    if (!parent_) return nullptr;
    
    for (size_t i = 0; i < parent_->children_.size(); ++i) {
        if (parent_->children_[i].get() == this && i > 0) {
            return parent_->children_[i-1].get();
        }
    }
    return nullptr;
}

DOMNode* DOMNode::nextSibling() const {
    if (!parent_) return nullptr;
    
    for (size_t i = 0; i < parent_->children_.size(); ++i) {
        if (parent_->children_[i].get() == this && i + 1 < parent_->children_.size()) {
            return parent_->children_[i+1].get();
        }
    }
    return nullptr;
}

static uint64_t s_domMutationVersion = 0;
uint64_t domMutationVersion() { return s_domMutationVersion; }
void noteDomMutation() { ++s_domMutationVersion; }

namespace {
struct PaintOnlyChanges {
    std::vector<DOMElement*> order;
    std::unordered_set<const DOMElement*> pending;
    std::function<void(DOMElement*)> listener;
    std::function<void(DOMElement*)> destroyed;
};
PaintOnlyChanges& paintOnlyChanges() {
    static PaintOnlyChanges changes;
    return changes;
}
}  // namespace

void notePaintOnlyChange(DOMElement* element) {
    if (!element) return;
    PaintOnlyChanges& changes = paintOnlyChanges();
    // An embedder that does not follow paint-only changes (no listener to
    // drop the element's cached style) restyles as for any other change
    if (!changes.listener) {
        noteDomMutation();
        return;
    }
    if (changes.pending.insert(element).second) changes.order.push_back(element);
    if (changes.listener) changes.listener(element);
}

std::vector<DOMElement*> takePaintOnlyChanges() {
    PaintOnlyChanges& changes = paintOnlyChanges();
    std::vector<DOMElement*> out;
    out.swap(changes.order);
    changes.pending.clear();
    return out;
}

void setPaintOnlyChangeListener(std::function<void(DOMElement*)> listener) {
    paintOnlyChanges().listener = std::move(listener);
}

void setElementDestroyedListener(std::function<void(DOMElement*)> listener) {
    paintOnlyChanges().destroyed = std::move(listener);
}

static bool s_styleAttributeSelectors = true;
void setStyleAttributeSelectorsInUse(bool used) { s_styleAttributeSelectors = used; }
bool styleAttributeSelectorsInUse() { return s_styleAttributeSelectors; }

bool isPaintOnlyProperty(const std::string& property) {
    return property == "opacity" || property == "transform" || property == "transform-origin" ||
           property == "translate" || property == "rotate" || property == "scale";
}

// A destroyed element is no longer waiting for its boxes to be updated
static void forgetPaintOnlyChange(const DOMElement* element) {
    PaintOnlyChanges& changes = paintOnlyChanges();
    if (!changes.pending.erase(element)) return;
    changes.order.erase(std::remove(changes.order.begin(), changes.order.end(), element), changes.order.end());
}

namespace {
// Documents with a mutation sink, by address: a node's ownerDocument pointer
// is only compared, never followed (the document may be gone)
std::unordered_map<const DOMDocument*, DOMMutationSink*>& mutationSinks() {
    static std::unordered_map<const DOMDocument*, DOMMutationSink*> sinks;
    return sinks;
}

DOMMutationSink* sinkOf(const DOMNode* node) {
    auto& sinks = mutationSinks();
    if (!node || sinks.empty()) return nullptr;
    const DOMDocument* doc = node->nodeType() == NodeType::Document ? static_cast<const DOMDocument*>(node)
                                                                     : node->ownerDocument();
    auto it = sinks.find(doc);
    return it != sinks.end() ? it->second : nullptr;
}
}  // namespace

void DOMDocument::setMutationSink(DOMMutationSink* sink) {
    if (sink) mutationSinks()[this] = sink;
    else mutationSinks().erase(this);
}

DOMMutationSink* DOMDocument::mutationSink() const { return sinkOf(this); }

void DOMDocument::removeMutationSink(DOMMutationSink* sink) {
    auto& sinks = mutationSinks();
    for (auto it = sinks.begin(); it != sinks.end();) {
        it = it->second == sink ? sinks.erase(it) : std::next(it);
    }
}

void noteCharacterDataChange(DOMNode* node, const std::string& oldValue) {
    if (DOMMutationSink* sink = sinkOf(node)) sink->characterData(node, oldValue);
}

DOMNode* DOMNode::appendChild(std::unique_ptr<DOMNode> child) {
    if (!child) return nullptr;
    noteDomMutation();
    
    child->parent_ = this;
    adopt(child.get());
    DOMNode* ptr = child.get();
    DOMNode* previous = children_.empty() ? nullptr : children_.back().get();
    children_.push_back(std::move(child));
    if (DOMMutationSink* sink = sinkOf(this)) sink->childList(this, ptr, nullptr, previous, nullptr);
    return ptr;
}

DOMNode* DOMNode::insertBefore(std::unique_ptr<DOMNode> child, DOMNode* refChild) {
    if (!child) return nullptr;
    noteDomMutation();
    
    child->parent_ = this;
    adopt(child.get());
    DOMNode* ptr = child.get();
    
    DOMNode* previous = nullptr;
    DOMNode* next = nullptr;
    auto at = children_.end();
    for (auto it = children_.begin(); refChild && it != children_.end(); ++it) {
        if (it->get() == refChild) {
            at = it;
            break;
        }
    }
    if (at != children_.end()) {
        next = at->get();
        if (at != children_.begin()) previous = std::prev(at)->get();
    } else if (!children_.empty()) {
        previous = children_.back().get();
    }
    children_.insert(at, std::move(child));
    if (DOMMutationSink* sink = sinkOf(this)) sink->childList(this, ptr, nullptr, previous, next);
    return ptr;
}

std::unique_ptr<DOMNode> DOMNode::removeChild(DOMNode* child) {
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->get() == child) {
            DOMNode* previous = it == children_.begin() ? nullptr : std::prev(it)->get();
            DOMNode* next = std::next(it) == children_.end() ? nullptr : std::next(it)->get();
            std::unique_ptr<DOMNode> removed = std::move(*it);
            children_.erase(it);
            noteDomMutation();
            removed->parent_ = nullptr;
            if (DOMMutationSink* sink = sinkOf(this)) sink->childList(this, nullptr, removed.get(), previous, next);
            return removed;
        }
    }
    return nullptr;
}

DOMNode* DOMNode::replaceChild(std::unique_ptr<DOMNode> newChild, DOMNode* oldChild) {
    if (!newChild || !oldChild) return nullptr;
    
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->get() == oldChild) {
            newChild->parent_ = this;
            adopt(newChild.get());
            oldChild->parent_ = nullptr;
            DOMNode* ptr = newChild.get();
            DOMNode* previous = it == children_.begin() ? nullptr : std::prev(it)->get();
            DOMNode* next = std::next(it) == children_.end() ? nullptr : std::next(it)->get();
            std::unique_ptr<DOMNode> old = std::move(*it);
            *it = std::move(newChild);
            noteDomMutation();
            // The replaced node is destroyed on return, so it is not handed
            // to the sink (nothing in the bindings calls replaceChild)
            if (DOMMutationSink* sink = sinkOf(this)) sink->childList(this, ptr, nullptr, previous, next);
            return ptr;
        }
    }
    return nullptr;
}

bool DOMNode::contains(DOMNode* other) const {
    if (!other) return false;
    if (this == other) return true;
    
    for (const auto& child : children_) {
        if (child->contains(other)) return true;
    }
    return false;
}

// ============================================================================
// DOMText
// ============================================================================

DOMText::DOMText(const std::string& data)
    : DOMNode(NodeType::Text), data_(data) {}

std::unique_ptr<DOMNode> DOMText::cloneNode(bool) const {
    auto clone = std::make_unique<DOMText>(data_);
    clone->setOwnerDocument(document_);
    return clone;
}

// ============================================================================
// DOMComment
// ============================================================================

DOMComment::DOMComment(const std::string& data)
    : DOMNode(NodeType::Comment), data_(data) {}

std::unique_ptr<DOMNode> DOMComment::cloneNode(bool) const {
    auto clone = std::make_unique<DOMComment>(data_);
    clone->setOwnerDocument(document_);
    return clone;
}

// ============================================================================
// DOMDocumentFragment
// ============================================================================

std::unique_ptr<DOMNode> DOMDocumentFragment::cloneNode(bool deep) const {
    auto clone = std::make_unique<DOMDocumentFragment>();
    clone->setOwnerDocument(document_);
    if (deep) {
        for (const auto& child : children_) clone->appendChild(child->cloneNode(true));
    }
    return clone;
}

// ============================================================================
// DOMElement
// ============================================================================

DOMElement::DOMElement(const std::string& tagName)
    : DOMNode(NodeType::Element), tagName_(tagName),
      ceState_(isValidCustomElementName(tagName) ? CustomElementState::Undefined
                                                 : CustomElementState::Uncustomized) {}

DOMElement::~DOMElement() {
    forgetPaintOnlyChange(this);
    if (paintOnlyChanges().destroyed) paintOnlyChanges().destroyed(this);
}

void DOMElement::setStyleAttributePaintOnly(const std::string& value) {
    auto it = attributes_.find("style");
    bool had = it != attributes_.end();
    std::string old = had ? it->second : std::string();
    attributes_["style"] = value;
    notePaintOnlyChange(this);
    if (DOMMutationSink* sink = sinkOf(this)) sink->attribute(this, "style", had ? &old : nullptr);
}

static bool s_shadowRootsExist = false;
bool shadowRootsExist() { return s_shadowRootsExist; }

DOMShadowRoot* DOMElement::attachShadow(bool open) {
    s_shadowRootsExist = true;
    shadowRoot_ = std::make_unique<DOMShadowRoot>(this, open);
    shadowRoot_->setOwnerDocument(document_);
    noteDomMutation();
    return shadowRoot_.get();
}

DOMDocumentFragment* DOMElement::templateContent() {
    if (tagName_ != "template") return nullptr;
    if (!templateContent_) {
        templateContent_ = std::make_unique<DOMDocumentFragment>();
        templateContent_->setOwnerDocument(document_);
        templateContent_->setTemplateHost(this);
    }
    return templateContent_.get();
}

std::string DOMElement::getAttribute(const std::string& name) const {
    auto it = attributes_.find(name);
    return it != attributes_.end() ? it->second : "";
}

void DOMElement::setAttribute(const std::string& name, const std::string& value) {
    DOMMutationSink* sink = sinkOf(this);
    if (!sink) {
        attributes_[name] = value;
        noteDomMutation();
        return;
    }
    auto it = attributes_.find(name);
    bool had = it != attributes_.end();
    std::string old = had ? it->second : std::string();
    attributes_[name] = value;
    noteDomMutation();
    sink->attribute(this, name, had ? &old : nullptr);
}

void DOMElement::removeAttribute(const std::string& name) {
    auto it = attributes_.find(name);
    if (it == attributes_.end()) return;
    std::string old = std::move(it->second);
    attributes_.erase(it);
    noteDomMutation();
    if (DOMMutationSink* sink = sinkOf(this)) sink->attribute(this, name, &old);
}

bool DOMElement::hasAttribute(const std::string& name) const {
    return attributes_.find(name) != attributes_.end();
}

std::vector<std::string> DOMElement::classList() const {
    std::vector<std::string> classes;
    std::string cls = className();
    // Split by whitespace
    size_t start = 0, end;
    while ((end = cls.find(' ', start)) != std::string::npos) {
        if (end > start) classes.push_back(cls.substr(start, end - start));
        start = end + 1;
    }
    if (start < cls.length()) classes.push_back(cls.substr(start));
    return classes;
}

namespace {

// HTML fragment serialization (HTML Living Standard §13.3)
std::string escapeHtml(const std::string& text, bool attribute) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '"': out += attribute ? "&quot;" : "\""; break;
            case '<': out += attribute ? "<" : "&lt;"; break;
            case '>': out += attribute ? ">" : "&gt;"; break;
            default: out += c;
        }
    }
    return out;
}

bool isVoidElement(const std::string& tag) {
    static const char* const kVoid[] = {"area", "base", "br", "col", "embed", "hr", "img", "input",
                                         "link", "meta", "source", "track", "wbr"};
    for (const char* v : kVoid) if (tag == v) return true;
    return false;
}

void serializeChildren(const DOMElement& element, std::string& out);

void serializeElement(const DOMElement& element, std::string& out) {
    out += '<';
    out += element.tagName();
    for (const auto& attr : element.attributes()) {
        out += ' ';
        out += attr.first;
        out += "=\"";
        out += escapeHtml(attr.second, true);
        out += '"';
    }
    out += '>';
    if (isVoidElement(element.tagName())) return;
    serializeChildren(element, out);
    out += "</";
    out += element.tagName();
    out += '>';
}

void serializeNodes(const DOMNode& parent, bool raw, std::string& out);

void serializeChildren(const DOMElement& element, std::string& out) {
    // Script and style text is raw text: written as it is
    bool raw = element.tagName() == "script" || element.tagName() == "style";
    // A <template> serializes its contents (HTML §13.3, step 3)
    const DOMNode* parent = &element;
    if (const DOMDocumentFragment* content = element.existingTemplateContent()) parent = content;
    serializeNodes(*parent, raw, out);
}

void serializeNodes(const DOMNode& parent, bool raw, std::string& out) {
    for (const auto& child : parent.childNodes()) {
        switch (child->nodeType()) {
            case NodeType::Element:
                serializeElement(static_cast<const DOMElement&>(*child), out);
                break;
            case NodeType::Text:
                out += raw ? child->nodeValue() : escapeHtml(child->nodeValue(), false);
                break;
            case NodeType::Comment:
                out += "<!--" + child->nodeValue() + "-->";
                break;
            default:
                break;
        }
    }
}

}  // namespace

std::string serializeChildNodes(const DOMNode* node) {
    std::string out;
    if (!node) return out;
    if (node->nodeType() == NodeType::Element) serializeChildren(*static_cast<const DOMElement*>(node), out);
    else serializeNodes(*node, false, out);
    return out;
}

std::string DOMElement::innerHTML() const {
    std::string out;
    serializeChildren(*this, out);
    return out;
}

void DOMElement::setInnerHTML(const std::string& html) {
    DOMNode* target = tagName_ == "template" ? static_cast<DOMNode*>(templateContent()) : this;
    while (DOMNode* last = target->lastChild()) target->removeChild(last);
    noteDomMutation();
    HTMLParser parser;
    for (auto& node : parser.parseFragmentNodes(html, tagName_)) target->appendChild(std::move(node));
}

std::string DOMElement::textContent() const {
    std::string result;
    for (const auto& child : children_) {
        if (child->nodeType() == NodeType::Text) {
            result += child->nodeValue();
        } else if (child->nodeType() == NodeType::Element) {
            result += static_cast<DOMElement*>(child.get())->textContent();
        }
    }
    return result;
}

void DOMElement::setTextContent(const std::string& text) {
    children_.clear();
    noteDomMutation();
    if (!text.empty()) {
        appendChild(std::make_unique<DOMText>(text));
    }
}

std::string DOMElement::outerHTML() const {
    std::string out;
    serializeElement(*this, out);
    return out;
}

DOMElement* DOMElement::getElementById(const std::string& id) {
    if (this->id() == id) return this;
    for (const auto& child : children_) {
        if (child->nodeType() == NodeType::Element) {
            DOMElement* el = static_cast<DOMElement*>(child.get());
            DOMElement* found = el->getElementById(id);
            if (found) return found;
        }
    }
    return nullptr;
}

std::vector<DOMElement*> DOMElement::getElementsByTagName(const std::string& name) {
    std::vector<DOMElement*> result;
    for (const auto& child : children_) {
        if (child->nodeType() == NodeType::Element) {
            DOMElement* el = static_cast<DOMElement*>(child.get());
            if (el->tagName() == name || name == "*") {
                result.push_back(el);
            }
            auto childResults = el->getElementsByTagName(name);
            result.insert(result.end(), childResults.begin(), childResults.end());
        }
    }
    return result;
}

std::vector<DOMElement*> DOMElement::getElementsByClassName(const std::string& className) {
    return descendantsWithClasses(this, className);
}

DOMElement* DOMElement::querySelector(const std::string& selector) {
    auto found = selectDescendants(this, selector, true);
    return found.empty() ? nullptr : found.front();
}

std::vector<DOMElement*> DOMElement::querySelectorAll(const std::string& selector) {
    return selectDescendants(this, selector, false);
}

std::string DOMElement::style(const std::string& property) const {
    auto it = styleMap_.find(property);
    return it != styleMap_.end() ? it->second : "";
}

void DOMElement::setStyle(const std::string& property, const std::string& value) {
    styleMap_[property] = value;
}

std::unique_ptr<DOMNode> DOMElement::cloneNode(bool deep) const {
    auto clone = std::make_unique<DOMElement>(tagName_);
    clone->document_ = document_;
    clone->attributes_ = attributes_;
    clone->styleMap_ = styleMap_;
    // A cloned form control keeps what was typed and checked
    clone->dirtyValue_ = dirtyValue_;
    clone->valueDirty_ = valueDirty_;
    clone->checkedDirty_ = checkedDirty_;
    clone->checked_ = checked_;
    // A cloned script that already ran does not run again
    clone->scriptAlreadyStarted_ = scriptAlreadyStarted_;
    clone->svgNamespace_ = svgNamespace_;

    if (deep) {
        for (const auto& child : children_) {
            clone->appendChild(child->cloneNode(true));
        }
        // A template's cloning steps copy its contents (HTML §4.12.3)
        if (templateContent_) {
            DOMDocumentFragment* content = clone->templateContent();
            for (const auto& child : templateContent_->childNodes()) content->appendChild(child->cloneNode(true));
        }
    }
    return clone;
}

// ============================================================================
// Custom element names, shadow trees and the flat tree
// ============================================================================

bool isValidCustomElementName(const std::string& name) {
    if (name.size() < 2 || name[0] < 'a' || name[0] > 'z') return false;
    bool hyphen = false;
    for (char c : name) {
        if (c >= 'A' && c <= 'Z') return false;
        if (c == '-') hyphen = true;
        // PCENChar: ASCII lowercase, digits, - . _ and non-ASCII
        unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x80 && !(std::islower(u) || std::isdigit(u) || c == '-' || c == '.' || c == '_')) return false;
    }
    if (!hyphen) return false;
    static const char* const kReserved[] = {"annotation-xml", "color-profile", "font-face", "font-face-src",
                                            "font-face-uri", "font-face-format", "font-face-name",
                                            "missing-glyph"};
    for (const char* r : kReserved) if (name == r) return false;
    return true;
}

DOMShadowRoot* asShadowRoot(const DOMNode* node) {
    if (!node || node->nodeType() != NodeType::DocumentFragment) return nullptr;
    auto* fragment = static_cast<const DOMDocumentFragment*>(node);
    return fragment->isShadowRoot() ? static_cast<DOMShadowRoot*>(const_cast<DOMDocumentFragment*>(fragment))
                                    : nullptr;
}

DOMNode* treeRoot(const DOMNode* node) {
    if (!node) return nullptr;
    while (node->parentNode()) node = node->parentNode();
    return const_cast<DOMNode*>(node);
}

DOMNode* shadowIncludingRoot(const DOMNode* node) {
    DOMNode* root = treeRoot(node);
    while (DOMShadowRoot* shadow = asShadowRoot(root)) root = treeRoot(shadow->host());
    return root;
}

namespace {
bool isSlot(const DOMNode* node) {
    return node && node->nodeType() == NodeType::Element &&
           static_cast<const DOMElement*>(node)->tagName() == "slot";
}

// The first slot in tree order below `node` named `name`
const DOMElement* firstSlotNamed(const DOMNode* node, const std::string& name) {
    for (const auto& child : node->childNodes()) {
        if (child->nodeType() != NodeType::Element) continue;
        auto* el = static_cast<const DOMElement*>(child.get());
        if (el->tagName() == "slot" && el->getAttribute("name") == name) return el;
        if (const DOMElement* found = firstSlotNamed(el, name)) return found;
    }
    return nullptr;
}

void appendFlattened(const DOMNode* node, std::vector<DOMNode*>& out);
}  // namespace

std::string slotNameOf(const DOMNode* node) {
    if (node && node->nodeType() == NodeType::Element) return static_cast<const DOMElement*>(node)->getAttribute("slot");
    return "";
}

std::vector<DOMNode*> assignedNodes(const DOMElement* slot, bool flatten) {
    std::vector<DOMNode*> out;
    DOMShadowRoot* shadow = asShadowRoot(treeRoot(slot));
    if (shadow && shadow->host()) {
        const std::string name = slot->getAttribute("name");
        // Only the first slot of a name gets the host's children of that name
        if (firstSlotNamed(shadow, name) == slot) {
            for (const auto& child : shadow->host()->childNodes()) {
                NodeType type = child->nodeType();
                if (type != NodeType::Element && type != NodeType::Text) continue;
                if (slotNameOf(child.get()) != name) continue;
                if (flatten && isSlot(child.get())) appendFlattened(child.get(), out);
                else out.push_back(child.get());
            }
        }
    }
    if (flatten && out.empty()) {
        // Fallback content
        for (const auto& child : slot->childNodes()) {
            if (isSlot(child.get())) appendFlattened(child.get(), out);
            else out.push_back(child.get());
        }
    }
    return out;
}

namespace {
void appendFlattened(const DOMNode* slot, std::vector<DOMNode*>& out) {
    // A slot outside a shadow tree is an ordinary element
    if (!asShadowRoot(treeRoot(slot))) {
        out.push_back(const_cast<DOMNode*>(slot));
        return;
    }
    for (DOMNode* n : assignedNodes(static_cast<const DOMElement*>(slot), true)) out.push_back(n);
}
}  // namespace

DOMElement* assignedSlot(const DOMNode* node) {
    DOMNode* parent = node ? node->parentNode() : nullptr;
    if (!parent || parent->nodeType() != NodeType::Element) return nullptr;
    DOMShadowRoot* shadow = static_cast<DOMElement*>(parent)->shadowRoot();
    if (!shadow) return nullptr;
    NodeType type = node->nodeType();
    if (type != NodeType::Element && type != NodeType::Text) return nullptr;
    return const_cast<DOMElement*>(firstSlotNamed(shadow, slotNameOf(node)));
}

std::vector<DOMNode*> flatTreeChildren(const DOMNode* node) {
    std::vector<DOMNode*> out;
    const DOMNode* source = node;
    if (node->nodeType() == NodeType::Element) {
        if (DOMShadowRoot* shadow = static_cast<const DOMElement*>(node)->shadowRoot()) source = shadow;
    }
    out.reserve(source->childNodes().size());
    int inShadowTree = -1;  // looked up at the first slot
    for (const auto& child : source->childNodes()) {
        if (isSlot(child.get())) {
            if (inShadowTree < 0) inShadowTree = asShadowRoot(treeRoot(source)) != nullptr;
            if (inShadowTree) {
                appendFlattened(child.get(), out);
                continue;
            }
        }
        out.push_back(child.get());
    }
    return out;
}

DOMElement* flatTreeParentElement(const DOMNode* node) {
    if (!node) return nullptr;
    DOMNode* parent = node->parentNode();
    if (!parent) return nullptr;
    if (parent->nodeType() == NodeType::Element) {
        if (static_cast<DOMElement*>(parent)->shadowRoot()) return assignedSlot(node);
        return static_cast<DOMElement*>(parent);
    }
    if (DOMShadowRoot* shadow = asShadowRoot(parent)) return shadow->host();
    return nullptr;
}

// ============================================================================
// DOMDocument
// ============================================================================

DOMDocument::DOMDocument() : DOMNode(NodeType::Document) {
    document_ = this;
}

DOMDocument::~DOMDocument() {
    mutationSinks().erase(this);
}

DOMElement* DOMDocument::body() const {
    if (!documentElement_) return nullptr;
    for (const auto& child : documentElement_->childNodes()) {
        if (child->nodeType() == NodeType::Element) {
            DOMElement* el = static_cast<DOMElement*>(child.get());
            if (el->tagName() == "BODY" || el->tagName() == "body") {
                return el;
            }
        }
    }
    return nullptr;
}

DOMElement* DOMDocument::head() const {
    if (!documentElement_) return nullptr;
    for (const auto& child : documentElement_->childNodes()) {
        if (child->nodeType() == NodeType::Element) {
            DOMElement* el = static_cast<DOMElement*>(child.get());
            if (el->tagName() == "HEAD" || el->tagName() == "head") {
                return el;
            }
        }
    }
    return nullptr;
}

std::string DOMDocument::title() const {
    DOMElement* h = head();
    if (!h) return "";
    for (const auto& child : h->childNodes()) {
        if (child->nodeType() == NodeType::Element) {
            DOMElement* el = static_cast<DOMElement*>(child.get());
            if (el->tagName() == "TITLE" || el->tagName() == "title") {
                return el->textContent();
            }
        }
    }
    return "";
}

void DOMDocument::setTitle(const std::string& title) {
    // Stub
}

std::unique_ptr<DOMElement> DOMDocument::createElement(const std::string& tagName) {
    auto elem = std::make_unique<DOMElement>(tagName);
    elem->setOwnerDocument(this);
    return elem;
}

std::unique_ptr<DOMText> DOMDocument::createTextNode(const std::string& data) {
    auto text = std::make_unique<DOMText>(data);
    text->setOwnerDocument(this);
    return text;
}

std::unique_ptr<DOMComment> DOMDocument::createComment(const std::string& data) {
    auto comment = std::make_unique<DOMComment>(data);
    comment->setOwnerDocument(this);
    return comment;
}

std::unique_ptr<DOMDocumentFragment> DOMDocument::createDocumentFragment() {
    auto fragment = std::make_unique<DOMDocumentFragment>();
    fragment->setOwnerDocument(this);
    return fragment;
}

DOMElement* DOMDocument::getElementById(const std::string& id) {
    if (documentElement_) {
        return documentElement_->getElementById(id);
    }
    return nullptr;
}

std::vector<DOMElement*> DOMDocument::getElementsByTagName(const std::string& tagName) {
    if (documentElement_) {
        return documentElement_->getElementsByTagName(tagName);
    }
    return {};
}

std::vector<DOMElement*> DOMDocument::getElementsByClassName(const std::string& className) {
    return descendantsWithClasses(this, className);
}

DOMElement* DOMDocument::querySelector(const std::string& selector) {
    auto found = selectDescendants(this, selector, true);
    return found.empty() ? nullptr : found.front();
}

std::vector<DOMElement*> DOMDocument::querySelectorAll(const std::string& selector) {
    return selectDescendants(this, selector, false);
}

std::unique_ptr<DOMNode> DOMDocument::cloneNode(bool deep) const {
    auto clone = std::make_unique<DOMDocument>();
    if (deep && documentElement_) {
        auto elemClone = documentElement_->cloneNode(true);
        clone->setDocumentElement(static_cast<DOMElement*>(elemClone.get()));
        clone->appendChild(std::move(elemClone));
    }
    return clone;
}

} // namespace Zepra::WebCore
