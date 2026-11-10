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

/// Incremented on every change to the DOM tree, text or attributes; the
/// renderer compares it to know when the layout must be rebuilt.
uint64_t domMutationVersion();
void noteDomMutation();
class DOMDocument;
class DOMNode;
class DOMElement;

/**
 * @brief Told about each change to a document's tree (MutationObserver):
 * set on the document with DOMDocument::setMutationSink. Called after the
 * change, with the nodes involved still alive (a removed node is still
 * owned by the caller of removeChild).
 */
class DOMMutationSink {
public:
    virtual ~DOMMutationSink() = default;
    /// One child added (added) or removed (removed) or replaced (both), between
    /// previousSibling and nextSibling
    virtual void childList(DOMNode* parent, DOMNode* added, DOMNode* removed,
                           DOMNode* previousSibling, DOMNode* nextSibling) = 0;
    /// An attribute set or removed; oldValue null when it was absent
    virtual void attribute(DOMElement* element, const std::string& name, const std::string* oldValue) = 0;
    /// A text or comment node's data changed
    virtual void characterData(DOMNode* node, const std::string& oldValue) = 0;
};
/// Reports a text or comment node's data change to its document's sink
void noteCharacterDataChange(DOMNode* node, const std::string& oldValue);
class RenderNode;

/// Whether an element matches a CSS selector list, as the style cascade matches it
bool selectorMatches(DOMElement* element, const std::string& selector);
/// Elements below `root` in document order that match a CSS selector list;
/// only the first one when `firstOnly`
std::vector<DOMElement*> selectDescendants(const DOMNode* root, const std::string& selector,
                                           bool firstOnly = false);
/// Elements below `root` that carry every class in a space-separated list
std::vector<DOMElement*> descendantsWithClasses(const DOMNode* root, const std::string& classNames);

/// Where an element is in the custom element life cycle (HTML §4.13.4):
/// a name that cannot be a custom element is uncustomized; a valid custom
/// element name is undefined until its definition upgrades it (custom), or
/// its constructor threw (failed)
enum class CustomElementState : uint8_t { Uncustomized, Undefined, Failed, Custom };

/// A valid custom element name: lowercase ASCII letter first, a hyphen,
/// no uppercase, and not one of the reserved SVG/MathML names (HTML §4.13.2)
bool isValidCustomElementName(const std::string& name);

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
 * @brief The script object bound to a node (one per node, made by the page's
 * bindings); told when the node is destroyed so it stops pointing at it
 */
class NodeBinding {
public:
    virtual void nodeDestroyed() = 0;
protected:
    ~NodeBinding() = default;
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

    // Script wrapper (not owned)
    NodeBinding* binding() const { return binding_; }
    void setBinding(NodeBinding* binding) { binding_ = binding; }
    
protected:
    NodeType nodeType_;
    DOMNode* parent_ = nullptr;
    DOMDocument* document_ = nullptr;
    std::vector<std::unique_ptr<DOMNode>> children_;
    RenderNode* renderNode_ = nullptr;
    NodeBinding* binding_ = nullptr;

    /// Gives an inserted subtree this node's owner document
    void adopt(DOMNode* child);
};

/**
 * @brief DOM text node
 */
class DOMText : public DOMNode {
public:
    explicit DOMText(const std::string& data = "");
    
    std::string nodeName() const override { return "#text"; }
    std::string nodeValue() const override { return data_; }
    void setNodeValue(const std::string& value) override { setData(value); }
    
    const std::string& data() const { return data_; }
    void setData(const std::string& data) {
        std::string old = std::move(data_);
        data_ = data;
        noteDomMutation();
        noteCharacterDataChange(this, old);
    }
    void appendData(const std::string& extra) { setData(data_ + extra); }
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
    void setNodeValue(const std::string& value) override {
        std::string old = std::move(data_);
        data_ = value;
        noteDomMutation();
        noteCharacterDataChange(this, old);
    }
    const std::string& data() const { return data_; }
    
    std::unique_ptr<DOMNode> cloneNode(bool deep) const override;
    
private:
    std::string data_;
};

/**
 * @brief DOM document fragment: a parentless container whose children move
 * into the tree when it is inserted
 */
class DOMDocumentFragment : public DOMNode {
public:
    DOMDocumentFragment() : DOMNode(NodeType::DocumentFragment) {}

    std::string nodeName() const override { return "#document-fragment"; }
    std::unique_ptr<DOMNode> cloneNode(bool deep) const override;

    /// A shadow root (DOMShadowRoot) rather than a plain fragment
    virtual bool isShadowRoot() const { return false; }
    /// The <template> whose contents this fragment is (template.content);
    /// null for any other fragment. Template contents are inert: never
    /// connected, never rendered, their custom elements never upgraded.
    DOMElement* templateHost() const { return templateHost_; }
    void setTemplateHost(DOMElement* host) { templateHost_ = host; }

private:
    DOMElement* templateHost_ = nullptr;
};

class DOMShadowRoot;

/**
 * @brief DOM element
 */
class DOMElement : public DOMNode {
public:
    explicit DOMElement(const std::string& tagName);
    ~DOMElement() override;
    
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

    /// The declarations the element's script animations (element.animate())
    /// apply now, as CSS text: the animations origin of the cascade, above
    /// the style attribute and below !important. A change restyles the page.
    const std::string& animationStyle() const { return animationStyle_; }
    void setAnimationStyle(const std::string& css) {
        if (css == animationStyle_) return;
        animationStyle_ = css;
        noteDomMutation();
    }
    
    // Cloning (a <template>'s contents are cloned with it when deep)
    std::unique_ptr<DOMNode> cloneNode(bool deep) const override;

    // Shadow DOM: the shadow root attached to this element (the host), or null
    DOMShadowRoot* shadowRoot() const { return shadowRoot_.get(); }
    /// Attaches a new, empty shadow root (the caller checks this element may
    /// host one and has none yet)
    DOMShadowRoot* attachShadow(bool open);

    /// A <template>'s contents (made on first use); null for other elements
    DOMDocumentFragment* templateContent();
    DOMDocumentFragment* existingTemplateContent() const { return templateContent_.get(); }

    // Custom elements
    CustomElementState customElementState() const { return ceState_; }
    /// (a change can change what :defined matches: the page restyles)
    void setCustomElementState(CustomElementState state) {
        if (ceState_ == state) return;
        ceState_ = state;
        noteDomMutation();
    }
    /// :defined - a built-in element or an upgraded custom element
    bool isDefined() const {
        return ceState_ == CustomElementState::Uncustomized || ceState_ == CustomElementState::Custom;
    }
    /// An element made in the SVG namespace (createElementNS)
    bool svgNamespace() const { return svgNamespace_; }
    void setSvgNamespace(bool svg) { svgNamespace_ = svg; }

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

    // Form control state: the value the user typed or a script set (an
    // <input>'s "dirty value"), and the checkedness a click or a script gave a
    // checkbox or radio. Until one is set the value / checked attribute gives
    // it; dom/form_controls.hpp reads the two together.
    bool valueDirty() const { return valueDirty_; }
    const std::string& dirtyValue() const { return dirtyValue_; }
    void setDirtyValue(const std::string& v) { dirtyValue_ = v; valueDirty_ = true; }
    bool checkedDirty() const { return checkedDirty_; }
    void setDirtyChecked(bool v) { checked_ = v; checkedDirty_ = true; }
    /// The text selection of a text control, as byte offsets into its value
    /// (the caret is a selection with start == end). Until one is set the
    /// caret sits at the end of the value; dom/form_controls.hpp clamps it.
    bool selectionSet() const { return selectionSet_; }
    size_t selectionStart() const { return selectionStart_; }
    size_t selectionEnd() const { return selectionEnd_; }
    bool selectionBackward() const { return selectionBackward_; }
    void setSelection(size_t start, size_t end, bool backward) {
        selectionStart_ = start; selectionEnd_ = end; selectionBackward_ = backward; selectionSet_ = true;
    }
    void clearSelection() { selectionSet_ = selectionBackward_ = false; selectionStart_ = selectionEnd_ = 0; }

    // <script> processing state (HTML "prepare the script element"): a
    // script runs at most once ("already started"), and one a script created
    // is async unless the script says otherwise ("force async")
    bool scriptAlreadyStarted() const { return scriptAlreadyStarted_; }
    void setScriptAlreadyStarted(bool v) { scriptAlreadyStarted_ = v; }
    bool scriptForceAsync() const { return scriptForceAsync_; }
    void setScriptForceAsync(bool v) { scriptForceAsync_ = v; }
    /// Back to the attributes (form reset)
    void resetControlState() {
        dirtyValue_.clear();
        valueDirty_ = checkedDirty_ = checked_ = false;
        clearSelection();
    }

private:
    std::string tagName_;
    std::unordered_map<std::string, std::string> attributes_;
    std::unordered_map<std::string, std::string> styleMap_;
    std::string animationStyle_;   // not cloned: animations stay with their element
    std::string dirtyValue_;
    std::unique_ptr<DOMShadowRoot> shadowRoot_;
    std::unique_ptr<DOMDocumentFragment> templateContent_;
    CustomElementState ceState_ = CustomElementState::Uncustomized;
    bool svgNamespace_ = false;
    bool valueDirty_ = false;
    bool checkedDirty_ = false;
    size_t selectionStart_ = 0;
    size_t selectionEnd_ = 0;
    bool selectionBackward_ = false;
    bool selectionSet_ = false;
    bool scriptAlreadyStarted_ = false;
    bool scriptForceAsync_ = false;

    // Pseudo-class state
    bool hovered_  = false;
    bool focused_  = false;
    bool active_   = false;
    bool checked_  = false;
    bool disabled_ = false;
};

/**
 * @brief A shadow root: the root of a shadow tree attached to a host element.
 * It is a document fragment with no parent; its host owns it. What renders
 * for the host is the shadow tree, with each <slot> showing the host's
 * children assigned to it (see flatTreeChildren).
 */
class DOMShadowRoot final : public DOMDocumentFragment {
public:
    DOMShadowRoot(DOMElement* host, bool open) : host_(host), open_(open) {}

    bool isShadowRoot() const override { return true; }
    DOMElement* host() const { return host_; }
    bool isOpen() const { return open_; }
    /// Shadow roots are not cloned (a plain fragment of their children)
    std::unique_ptr<DOMNode> cloneNode(bool deep) const override { return DOMDocumentFragment::cloneNode(deep); }

private:
    DOMElement* host_;
    bool open_;
};

// ----------------------------------------------------------------------------
// Shadow trees and the flat tree (DOM §4.2.2, CSS Scoping §2)
// ----------------------------------------------------------------------------

/// The HTML serialization of a node's children (innerHTML of an element,
/// a template's contents, a shadow root or any fragment)
std::string serializeChildNodes(const DOMNode* node);
/// Whether any element ever got a shadow root (lets style and layout skip
/// shadow work on the pages that have none)
bool shadowRootsExist();
/// The node as a shadow root, or null
DOMShadowRoot* asShadowRoot(const DOMNode* node);
/// The root of the node's tree, stepping from each shadow root to its host
/// (the document for every connected node)
DOMNode* shadowIncludingRoot(const DOMNode* node);
/// The root of the node's own tree (a shadow root for nodes in a shadow tree)
DOMNode* treeRoot(const DOMNode* node);
/// The slot name a host's child asks for: its slot attribute (text: "")
std::string slotNameOf(const DOMNode* node);
/// The host children a <slot> in a shadow tree gets, in tree order; with
/// `flatten`, slots among them are replaced by their own assigned nodes
/// (or fallback children). Empty for a slot outside a shadow tree.
std::vector<DOMNode*> assignedNodes(const DOMElement* slot, bool flatten);
/// The slot a host's child is assigned to, or null
DOMElement* assignedSlot(const DOMNode* node);
/// The children as rendered: a host's shadow tree instead of its children,
/// each <slot> of a shadow tree replaced by what is assigned to it, or its
/// own children when nothing is
std::vector<DOMNode*> flatTreeChildren(const DOMNode* node);
/// The element a node inherits style from: its assigned slot, the host
/// for a shadow root's children, else the parent element
DOMElement* flatTreeParentElement(const DOMNode* node);

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

    /// Quirks mode, chosen by the parser from the doctype (no doctype = quirks)
    bool quirksMode() const { return quirksMode_; }
    void setQuirksMode(bool quirks) { quirksMode_ = quirks; }
    
    // Element creation
    std::unique_ptr<DOMElement> createElement(const std::string& tagName);
    std::unique_ptr<DOMText> createTextNode(const std::string& data);
    std::unique_ptr<DOMComment> createComment(const std::string& data);
    std::unique_ptr<DOMDocumentFragment> createDocumentFragment();
    
    // Queries
    DOMElement* getElementById(const std::string& id);
    std::vector<DOMElement*> getElementsByTagName(const std::string& tagName);
    std::vector<DOMElement*> getElementsByClassName(const std::string& className);
    DOMElement* querySelector(const std::string& selector);
    std::vector<DOMElement*> querySelectorAll(const std::string& selector);
    
    // Cloning
    std::unique_ptr<DOMNode> cloneNode(bool deep) const override;

    ~DOMDocument() override;

    /// Who hears about changes to this document's nodes (null: nobody).
    /// Looked up by the document's address, so a node that outlives its
    /// document never reads the freed document.
    void setMutationSink(DOMMutationSink* sink);
    DOMMutationSink* mutationSink() const;
    /// Detaches a sink from every document it was set on
    static void removeMutationSink(DOMMutationSink* sink);
    
private:
    DOMElement* documentElement_ = nullptr;
    bool quirksMode_ = false;
};



} // namespace Zepra::WebCore
