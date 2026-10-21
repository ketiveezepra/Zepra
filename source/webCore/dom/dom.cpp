// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file dom.cpp
 * @brief DOM Node implementation stubs
 */

#include "browser/dom.hpp"
#include <algorithm>

namespace Zepra::WebCore {

// ============================================================================
// DOMNode
// ============================================================================

DOMNode::DOMNode(NodeType type) : nodeType_(type) {}

DOMNode::~DOMNode() = default;

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

DOMNode* DOMNode::appendChild(std::unique_ptr<DOMNode> child) {
    if (!child) return nullptr;
    noteDomMutation();
    
    child->parent_ = this;
    child->document_ = this->document_;
    DOMNode* ptr = child.get();
    children_.push_back(std::move(child));
    return ptr;
}

DOMNode* DOMNode::insertBefore(std::unique_ptr<DOMNode> child, DOMNode* refChild) {
    if (!child) return nullptr;
    noteDomMutation();
    
    child->parent_ = this;
    child->document_ = this->document_;
    DOMNode* ptr = child.get();
    
    if (!refChild) {
        children_.push_back(std::move(child));
    } else {
        for (auto it = children_.begin(); it != children_.end(); ++it) {
            if (it->get() == refChild) {
                children_.insert(it, std::move(child));
                return ptr;
            }
        }
        children_.push_back(std::move(child));
    }
    return ptr;
}

std::unique_ptr<DOMNode> DOMNode::removeChild(DOMNode* child) {
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->get() == child) {
            std::unique_ptr<DOMNode> removed = std::move(*it);
            children_.erase(it);
            noteDomMutation();
            removed->parent_ = nullptr;
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
            newChild->document_ = this->document_;
            oldChild->parent_ = nullptr;
            DOMNode* ptr = newChild.get();
            *it = std::move(newChild);
            noteDomMutation();
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
    return std::make_unique<DOMText>(data_);
}

// ============================================================================
// DOMComment
// ============================================================================

DOMComment::DOMComment(const std::string& data)
    : DOMNode(NodeType::Comment), data_(data) {}

std::unique_ptr<DOMNode> DOMComment::cloneNode(bool) const {
    return std::make_unique<DOMComment>(data_);
}

// ============================================================================
// DOMElement
// ============================================================================

DOMElement::DOMElement(const std::string& tagName)
    : DOMNode(NodeType::Element), tagName_(tagName) {}

std::string DOMElement::getAttribute(const std::string& name) const {
    auto it = attributes_.find(name);
    return it != attributes_.end() ? it->second : "";
}

void DOMElement::setAttribute(const std::string& name, const std::string& value) {
    attributes_[name] = value;
    noteDomMutation();
}

void DOMElement::removeAttribute(const std::string& name) {
    if (attributes_.erase(name)) noteDomMutation();
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

std::string DOMElement::innerHTML() const {
    // Stub
    return "";
}

void DOMElement::setInnerHTML(const std::string& html) {
    // Clear children first
    children_.clear();
    noteDomMutation();
    // Would parse HTML here - stub
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
    // Stub
    return "";
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
    std::vector<DOMElement*> result;
    // Stub
    return result;
}

DOMElement* DOMElement::querySelector(const std::string& selector) {
    // Stub
    return nullptr;
}

std::vector<DOMElement*> DOMElement::querySelectorAll(const std::string& selector) {
    std::vector<DOMElement*> result;
    // Stub
    return result;
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
    clone->attributes_ = attributes_;
    clone->styleMap_ = styleMap_;
    
    if (deep) {
        for (const auto& child : children_) {
            clone->appendChild(child->cloneNode(true));
        }
    }
    return clone;
}

// ============================================================================
// DOMDocument
// ============================================================================

DOMDocument::DOMDocument() : DOMNode(NodeType::Document) {
    document_ = this;
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
    return {};
}

DOMElement* DOMDocument::querySelector(const std::string& selector) {
    return nullptr;
}

std::vector<DOMElement*> DOMDocument::querySelectorAll(const std::string& selector) {
    return {};
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
