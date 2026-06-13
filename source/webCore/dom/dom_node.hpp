#pragma once

/**
 * @file dom_node.hpp
 * @brief Base DOM Node interface (minimal for JS-DOM bridge)
 *
 * This is a minimal, read-only-first implementation sufficient to unblock
 * React/Next.js style SPAs. It provides the core tree + attribute surface
 * that JavaScript frameworks expect.
 */

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace Zepra::WebCore {

class DOMDocument;
class DOMElement;
class DOMText;

/**
 * @brief DOM nodeType values (DOM Level 1)
 */
enum class NodeType : uint16_t {
    ELEMENT_NODE = 1,
    TEXT_NODE = 3,
    DOCUMENT_NODE = 9
};

/**
 * @brief Base DOM Node
 *
 * Every node knows its parent, children, owner document, and type.
 * Mutation methods (appendChild, removeChild) update the tree and
 * notify observers (layout engine) when the bridge is fully wired.
 */
class DOMNode {
public:
    virtual ~DOMNode() = default;

    // Identity
    NodeType nodeType() const { return nodeType_; }
    DOMDocument* ownerDocument() const { return ownerDocument_; }

    // Tree navigation (read-only for v1)
    DOMNode* parentNode() const { return parentNode_; }
    const std::vector<std::unique_ptr<DOMNode>>& childNodes() const { return childNodes_; }

    // Mutation (minimal)
    virtual DOMNode* appendChild(DOMNode* child);
    virtual DOMNode* removeChild(DOMNode* child);
    virtual DOMNode* insertBefore(DOMNode* newChild, DOMNode* referenceChild);

    // Convenience
    size_t childCount() const { return childNodes_.size(); }
    DOMNode* firstChild() const { return childNodes_.empty() ? nullptr : childNodes_.front().get(); }
    DOMNode* lastChild() const { return childNodes_.empty() ? nullptr : childNodes_.back().get(); }

    // Debug / introspection
    virtual std::string nodeName() const = 0;
    virtual std::string toString() const;

protected:
    explicit DOMNode(NodeType type, DOMDocument* owner);
    void setOwnerDocument(DOMDocument* doc) { ownerDocument_ = doc; }

    NodeType nodeType_;
    DOMDocument* ownerDocument_ = nullptr;
    DOMNode* parentNode_ = nullptr;
    std::vector<std::unique_ptr<DOMNode>> childNodes_;
};

/**
 * @brief DOM Element node
 */
class DOMElement : public DOMNode {
public:
    explicit DOMElement(const std::string& tagName, DOMDocument* owner);

    // Element-specific
    const std::string& tagName() const { return tagName_; }
    const std::string& id() const;
    void setId(const std::string& id);

    // Attributes
    void setAttribute(const std::string& name, const std::string& value);
    std::string getAttribute(const std::string& name) const;
    bool hasAttribute(const std::string& name) const;
    void removeAttribute(const std::string& name);
    const std::unordered_map<std::string, std::string>& attributes() const { return attributes_; }

    // innerHTML (basic setter – triggers re-parse in full impl)
    void setInnerHTML(const std::string& html);
    std::string innerHTML() const; // placeholder – returns empty for now

    // textContent (concatenation of all descendant text)
    std::string textContent() const;
    void setTextContent(const std::string& text);

    std::string nodeName() const override { return tagName_; }

private:
    std::string tagName_;
    std::unordered_map<std::string, std::string> attributes_;
};

/**
 * @brief DOM Text node
 */
class DOMText : public DOMNode {
public:
    explicit DOMText(const std::string& data, DOMDocument* owner);

    const std::string& data() const { return data_; }
    void setData(const std::string& data) { data_ = data; }

    std::string nodeName() const override { return "#text"; }
    std::string nodeValue() const { return data_; }

private:
    std::string data_;
};

/**
 * @brief DOM Document node (root of every DOM tree)
 */
class DOMDocument : public DOMNode {
public:
    DOMDocument();

    // Factory methods (the ones JS frameworks call)
    DOMElement* createElement(const std::string& tagName);
    DOMText* createTextNode(const std::string& data);

    // Convenience accessors
    DOMElement* documentElement() const; // usually <html>
    DOMElement* body() const;            // <body> if present

    // Query helpers (minimal)
    DOMElement* getElementById(const std::string& id) const;
    std::vector<DOMElement*> getElementsByTagName(const std::string& tagName) const;

    std::string nodeName() const override { return "#document"; }

private:
    // We keep a weak root reference; the real <html> element is created on demand.
    DOMElement* htmlElement_ = nullptr;
};

} // namespace Zepra::WebCore
