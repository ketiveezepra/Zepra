#pragma once

/**
 * @file dom.hpp
 * @brief Document Object Model implementation
 */

#include <stdint.h>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include "event_target.hpp"

namespace Zepra::WebCore {

class DOMNode;
class DOMElement;
class DOMDocument;
class RenderNode;

/**
 * @brief DOM node types
 */
enum class NodeType {
    Element = 1,
    Text = 3,
    Comment = 8,
    Document = 9,
    DocumentFragment = 11
};

/**
 * @brief Base DOM node
 */
class DOMNode : public EventTarget {
public:
    explicit DOMNode(NodeType type);
    virtual ~DOMNode();
    
    // Type
    NodeType nodeType() const { return nodeType_; }
    virtual std::string nodeName() const = 0;
    
    // Value
    virtual std::string nodeValue() const { return ""; }
    virtual void setNodeValue(const std::string&) {}
    
    // Tree traversal
    DOMNode* parentNode() const { return parent_; }
    DOMNode* firstChild() const { return children_.empty() ? nullptr : children_[0].get(); }
    DOMNode* lastChild() const { return children_.empty() ? nullptr : children_.back().get(); }
    DOMNode* previousSibling() const;
    DOMNode* nextSibling() const;
    const std::vector<std::unique_ptr<DOMNode>>& childNodes() const { return children_; }
    
    // Tree manipulation
    DOMNode* appendChild(std::unique_ptr<DOMNode> child);
    DOMNode* insertBefore(std::unique_ptr<DOMNode> child, DOMNode* refChild);
    std::unique_ptr<DOMNode> removeChild(DOMNode* child);
    DOMNode* replaceChild(std::unique_ptr<DOMNode> newChild, DOMNode* oldChild);
    
    // Queries
    bool hasChildNodes() const { return !children_.empty(); }
    bool contains(DOMNode* other) const;
    
    // Document
    DOMDocument* ownerDocument() const { return document_; }
    void setOwnerDocument(DOMDocument* doc) { document_ = doc; }
    
    // Cloning
    virtual std::unique_ptr<DOMNode> cloneNode(bool deep) const = 0;
    
    // Rendering
    RenderNode* renderNode() const { return renderNode_; }
    void setRenderNode(RenderNode* node) { renderNode_ = node; }
    
protected:
    NodeType nodeType_;
    DOMNode* parent_ = nullptr;
    DOMDocument* document_ = nullptr;
    std::vector<std::unique_ptr<DOMNode>> children_;
    RenderNode* renderNode_ = nullptr;
};

/**
 * @brief DOM text node
 */
class DOMText : public DOMNode {
public:
    explicit DOMText(const std::string& data = "");
    
    std::string nodeName() const override { return "#text"; }
    std::string nodeValue() const override { return data_; }
    void setNodeValue(const std::string& value) override { data_ = value; }
    
    const std::string& data() const { return data_; }
    void setData(const std::string& data) { data_ = data; }
    void appendData(const std::string& extra) { data_ += extra; }
    size_t length() const { return data_.length(); }
    
    std::unique_ptr<DOMNode> cloneNode(bool deep) const override;
    
private:
    std::string data_;
};

/**
 * @brief DOM comment node
 */
class DOMComment : public DOMNode {
public:
    explicit DOMComment(const std::string& data = "");
    
    std::string nodeName() const override { return "#comment"; }
    std::string nodeValue() const override { return data_; }
    void setNodeValue(const std::string& value) override { data_ = value; }
    const std::string& data() const { return data_; }
    
    std::unique_ptr<DOMNode> cloneNode(bool deep) const override;
    
private:
    std::string data_;
};

/**
 * @brief DOM element
 */
class DOMElement : public DOMNode {
public:
    explicit DOMElement(const std::string& tagName);
    
    std::string nodeName() const override { return tagName_; }
    const std::string& tagName() const { return tagName_; }
    
    // Attributes
    std::string getAttribute(const std::string& name) const;
    void setAttribute(const std::string& name, const std::string& value);
    void removeAttribute(const std::string& name);
    bool hasAttribute(const std::string& name) const;
    const std::unordered_map<std::string, std::string>& attributes() const { return attributes_; }
    
    // ID/Class shortcuts
    std::string id() const { return getAttribute("id"); }
    void setId(const std::string& id) { setAttribute("id", id); }
    std::string className() const { return getAttribute("class"); }
    void setClassName(const std::string& cls) { setAttribute("class", cls); }
    std::vector<std::string> classList() const;
    
    // Content
    std::string innerHTML() const;
    void setInnerHTML(const std::string& html);
    std::string textContent() const;
    void setTextContent(const std::string& text);
    std::string outerHTML() const;
    
    // Queries
    DOMElement* getElementById(const std::string& id);
    std::vector<DOMElement*> getElementsByTagName(const std::string& tagName);
    std::vector<DOMElement*> getElementsByClassName(const std::string& className);
    DOMElement* querySelector(const std::string& selector);
    std::vector<DOMElement*> querySelectorAll(const std::string& selector);
    
    // Style
    std::string style(const std::string& property) const;
    void setStyle(const std::string& property, const std::string& value);
    
    // Cloning
    std::unique_ptr<DOMNode> cloneNode(bool deep) const override;

    // Element state (for CSS pseudo-class matching)
    bool hovered() const { return hovered_; }
    bool focused() const { return focused_; }
    bool active() const { return active_; }
    bool checked() const { return checked_; }
    bool disabled() const { return disabled_; }
    void setHovered(bool v) { hovered_ = v; }
    void setFocused(bool v) { focused_ = v; }
    void setActive(bool v)  { active_ = v; }
    void setChecked(bool v) { checked_ = v; }
    void setDisabled(bool v){ disabled_ = v; }
    
private:
    std::string tagName_;
    std::unordered_map<std::string, std::string> attributes_;
    std::unordered_map<std::string, std::string> styleMap_;

    // Pseudo-class state
    bool hovered_  = false;
    bool focused_  = false;
    bool active_   = false;
    bool checked_  = false;
    bool disabled_ = false;
};

/**
 * @brief DOM document
 */
class DOMDocument : public DOMNode {
public:
    DOMDocument();
    
    std::string nodeName() const override { return "#document"; }
    
    // Document properties
    DOMElement* documentElement() const { return documentElement_; }
    void setDocumentElement(DOMElement* elem) { documentElement_ = elem; }
    DOMElement* body() const;
    DOMElement* head() const;
    std::string title() const;
    void setTitle(const std::string& title);
    
    // Element creation
    std::unique_ptr<DOMElement> createElement(const std::string& tagName);
    std::unique_ptr<DOMText> createTextNode(const std::string& data);
    std::unique_ptr<DOMComment> createComment(const std::string& data);
    
    // Queries
    DOMElement* getElementById(const std::string& id);
    std::vector<DOMElement*> getElementsByTagName(const std::string& tagName);
    std::vector<DOMElement*> getElementsByClassName(const std::string& className);
    DOMElement* querySelector(const std::string& selector);
    std::vector<DOMElement*> querySelectorAll(const std::string& selector);
    
    // Cloning
    std::unique_ptr<DOMNode> cloneNode(bool deep) const override;
    
private:
    DOMElement* documentElement_ = nullptr;
};



} // namespace Zepra::WebCore
