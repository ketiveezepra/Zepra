// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file dom_node.cpp
 * @brief Implementation of minimal DOM Node / Element / Document / Text
 */

#include "dom_node.hpp"
#include <algorithm>

namespace Zepra::WebCore {

// -----------------------------------------------------------------------------
// DOMNode
// -----------------------------------------------------------------------------
DOMNode::DOMNode(NodeType type, DOMDocument* owner)
    : nodeType_(type), ownerDocument_(owner) {}

DOMNode* DOMNode::appendChild(DOMNode* child) {
    if (!child) return nullptr;
    // Detach from previous parent if any
    if (child->parentNode_) {
        child->parentNode_->removeChild(child);
    }
    child->parentNode_ = this;
    childNodes_.emplace_back(child); // takes ownership
    return child;
}

DOMNode* DOMNode::removeChild(DOMNode* child) {
    if (!child) return nullptr;
    auto it = std::find_if(childNodes_.begin(), childNodes_.end(),
                           [child](const auto& p) { return p.get() == child; });
    if (it != childNodes_.end()) {
        (*it)->parentNode_ = nullptr;
        // release ownership and return raw pointer (caller does not take ownership)
        return it->release();
    }
    return nullptr;
}

DOMNode* DOMNode::insertBefore(DOMNode* newChild, DOMNode* referenceChild) {
    if (!newChild) return nullptr;
    if (!referenceChild) return appendChild(newChild);

    auto it = std::find_if(childNodes_.begin(), childNodes_.end(),
                           [referenceChild](const auto& p) { return p.get() == referenceChild; });
    if (it == childNodes_.end()) return appendChild(newChild);

    if (newChild->parentNode_) newChild->parentNode_->removeChild(newChild);
    newChild->parentNode_ = this;
    childNodes_.insert(it, std::unique_ptr<DOMNode>(newChild));
    return newChild;
}

std::string DOMNode::toString() const {
    return nodeName();
}

// -----------------------------------------------------------------------------
// DOMElement
// -----------------------------------------------------------------------------
DOMElement::DOMElement(const std::string& tagName, DOMDocument* owner)
    : DOMNode(NodeType::ELEMENT_NODE, owner), tagName_(tagName) {}

const std::string& DOMElement::id() const {
    static const std::string empty;
    auto it = attributes_.find("id");
    return it != attributes_.end() ? it->second : empty;
}

void DOMElement::setId(const std::string& id) {
    attributes_["id"] = id;
}

void DOMElement::setAttribute(const std::string& name, const std::string& value) {
    attributes_[name] = value;
}

std::string DOMElement::getAttribute(const std::string& name) const {
    auto it = attributes_.find(name);
    return it != attributes_.end() ? it->second : std::string();
}

bool DOMElement::hasAttribute(const std::string& name) const {
    return attributes_.find(name) != attributes_.end();
}

void DOMElement::removeAttribute(const std::string& name) {
    attributes_.erase(name);
}

void DOMElement::setInnerHTML(const std::string& /*html*/) {
    // Placeholder – in a full implementation this would invoke an HTML parser
    // and replace children. For beta we leave the tree unchanged.
}

std::string DOMElement::innerHTML() const {
    return {}; // placeholder
}

std::string DOMElement::textContent() const {
    std::string result;
    for (const auto& child : childNodes_) {
        if (auto* text = dynamic_cast<DOMText*>(child.get())) {
            result += text->data();
        } else if (auto* elem = dynamic_cast<DOMElement*>(child.get())) {
            result += elem->textContent();
        }
    }
    return result;
}

void DOMElement::setTextContent(const std::string& text) {
    childNodes_.clear();
    if (!text.empty()) {
        auto* txt = new DOMText(text, ownerDocument_);
        appendChild(txt);
    }
}

// -----------------------------------------------------------------------------
// DOMText
// -----------------------------------------------------------------------------
DOMText::DOMText(const std::string& data, DOMDocument* owner)
    : DOMNode(NodeType::TEXT_NODE, owner), data_(data) {}

// -----------------------------------------------------------------------------
// DOMDocument
// -----------------------------------------------------------------------------
DOMDocument::DOMDocument()
    : DOMNode(NodeType::DOCUMENT_NODE, nullptr) {
    setOwnerDocument(this); // document owns itself
}

DOMElement* DOMDocument::createElement(const std::string& tagName) {
    auto* elem = new DOMElement(tagName, this);
    return elem;
}

DOMText* DOMDocument::createTextNode(const std::string& data) {
    auto* txt = new DOMText(data, this);
    return txt;
}

DOMElement* DOMDocument::documentElement() const {
    // Return the first element child (usually <html>)
    for (const auto& child : childNodes_) {
        if (auto* elem = dynamic_cast<DOMElement*>(child.get())) {
            return elem;
        }
    }
    return nullptr;
}

DOMElement* DOMDocument::body() const {
    if (auto* html = documentElement()) {
        for (const auto& child : html->childNodes()) {
            if (auto* elem = dynamic_cast<DOMElement*>(child.get())) {
                if (elem->tagName() == "body") return elem;
            }
        }
    }
    return nullptr;
}

DOMElement* DOMDocument::getElementById(const std::string& id) const {
    // Naive recursive search – sufficient for beta
    std::function<DOMElement*(const DOMNode*)> search = [&](const DOMNode* node) -> DOMElement* {
        if (auto* elem = dynamic_cast<const DOMElement*>(node)) {
            if (elem->hasAttribute("id") && elem->getAttribute("id") == id) {
                return const_cast<DOMElement*>(elem);
            }
        }
        for (const auto& child : node->childNodes()) {
            if (auto* found = search(child.get())) return found;
        }
        return nullptr;
    };
    return search(this);
}

std::vector<DOMElement*> DOMDocument::getElementsByTagName(const std::string& tagName) const {
    std::vector<DOMElement*> result;
    std::function<void(const DOMNode*)> collect = [&](const DOMNode* node) {
        if (auto* elem = dynamic_cast<const DOMElement*>(node)) {
            if (elem->tagName() == tagName) {
                result.push_back(const_cast<DOMElement*>(elem));
            }
        }
        for (const auto& child : node->childNodes()) {
            collect(child.get());
        }
    };
    collect(this);
    return result;
}

} // namespace Zepra::WebCore
