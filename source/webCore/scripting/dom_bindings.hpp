// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file dom_bindings.hpp
 * @brief The DOM as page scripts see it: one live JS object per node, whose
 *        properties read and change the C++ DOM, and DOM events with
 *        capture, target and bubble phases.
 *
 * A node's wrapper is made the first time a script reaches the node and is
 * the same object afterwards (el === document.getElementById(id)). Changes
 * go straight to the C++ tree, which bumps domMutationVersion(), so the
 * browser relayouts and DevTools follow without an observer.
 *
 * Nodes a script takes out of the tree are never destroyed while the
 * browser may still point at them (layout boxes, hover state): a node a
 * wrapper refers to is kept by that wrapper, and the rest wait for
 * releaseDetachedNodes(), which the browser calls after a relayout.
 */

#pragma once

#include "browser/dom.hpp"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Zepra::Runtime { class Object; class Value; class VM; class FunctionCallInfo; }

namespace Zepra::WebCore {

class CanvasRegistry;
class NodeWrapper;
class EventObject;

class DOMBindings {
public:
    using ErrorReporter = std::function<void(const std::string& message)>;
    /// Compiles `source` (a function expression) to a function value
    using FunctionCompiler = std::function<bool(const std::string& source, Zepra::Runtime::Value& out)>;

    /// Makes the window object and the DOM interfaces (Node, Element,
    /// Event, ...), and puts the interfaces on the global object and window
    DOMBindings(Zepra::Runtime::VM* vm, ErrorReporter reportError);
    ~DOMBindings();
    DOMBindings(const DOMBindings&) = delete;
    DOMBindings& operator=(const DOMBindings&) = delete;

    /// The page's window object (filled in by ScriptContext)
    Zepra::Runtime::Object* window() const;

    /// Puts `document` on the global object and window
    void installDocument(DOMDocument* document);
    void setCanvases(CanvasRegistry* canvases) { canvases_ = canvases; }
    void setFunctionCompiler(FunctionCompiler compiler) { compileFunction_ = std::move(compiler); }
    /// document.readyState: "loading", "interactive" or "complete"
    void setReadyState(const std::string& state) { readyState_ = state; }

    /// The JS object of a node: made on first use, the same one afterwards
    Zepra::Runtime::Value wrap(DOMNode* node);
    /// The node behind a wrapper object; null for any other object (or a destroyed node)
    static DOMNode* nodeOf(const Zepra::Runtime::Object* object);

    /// Fires a plain event at a node, or at the window when `target` is
    /// null; false when a listener canceled it
    bool fireEvent(DOMNode* target, const std::string& type, bool bubbles, bool cancelable);
    /// Fires a mouse event (clientX/clientY) at a connected element
    bool fireMouseEvent(DOMElement* target, const std::string& type, double clientX, double clientY);

    /// Frees the nodes scripts removed that no script can reach any more.
    /// Call once nothing outside the DOM points at them (after a relayout).
    void releaseDetachedNodes();
    size_t detachedNodeCount() const { return graveyard_.size(); }

private:
    using Args = std::vector<Zepra::Runtime::Value>;
    using Info = Zepra::Runtime::FunctionCallInfo;

    Zepra::Runtime::VM* vm_;
    ErrorReporter reportError_;
    FunctionCompiler compileFunction_;
    DOMDocument* document_ = nullptr;
    CanvasRegistry* canvases_ = nullptr;
    std::string readyState_ = "loading";

    Zepra::Runtime::Object* global_ = nullptr;   // window
    Zepra::Runtime::Object* eventTargetProto_ = nullptr;
    Zepra::Runtime::Object* nodeProto_ = nullptr;
    Zepra::Runtime::Object* elementProto_ = nullptr;
    Zepra::Runtime::Object* htmlElementProto_ = nullptr;
    Zepra::Runtime::Object* characterDataProto_ = nullptr;
    Zepra::Runtime::Object* textProto_ = nullptr;
    Zepra::Runtime::Object* commentProto_ = nullptr;
    Zepra::Runtime::Object* documentProto_ = nullptr;
    Zepra::Runtime::Object* htmlDocumentProto_ = nullptr;
    Zepra::Runtime::Object* fragmentProto_ = nullptr;
    Zepra::Runtime::Object* tokenListProto_ = nullptr;
    Zepra::Runtime::Object* eventProto_ = nullptr;
    Zepra::Runtime::Object* mouseEventProto_ = nullptr;
    Zepra::Runtime::Object* listItemFn_ = nullptr;  // item(i) on the arrays standing in for NodeLists
    /// Per-tag interfaces (HTMLDivElement, HTMLCanvasElement, ...) by lowercase tag
    std::unordered_map<std::string, std::pair<std::string, Zepra::Runtime::Object*>> tagInterfaces_;

    std::vector<NodeWrapper*> wrappers_;                 // every wrapper made, to unbind them at the end
    std::vector<std::unique_ptr<DOMNode>> graveyard_;    // removed subtrees no wrapper refers to

    // Setup
    Zepra::Runtime::Object* makeInterface(const std::string& name, Zepra::Runtime::Object* parentProto,
                                          Zepra::Runtime::Object* existingProto = nullptr);
    void installEventTarget();
    void installNode();
    void installElement();
    void installCharacterData();
    void installDocumentInterface();
    void installEvents();

    // Wrappers and node ownership
    Zepra::Runtime::Object* prototypeFor(const DOMNode* node) const;
    std::string interfaceName(const DOMNode* node) const;
    NodeWrapper* wrapperOf(DOMNode* node);
    DOMNode* nodeArg(const Info& info, size_t index, const char* method, bool nullable);
    std::unique_ptr<DOMNode> take(DOMNode* node);
    void park(std::unique_ptr<DOMNode> root);
    void removeAllChildren(DOMNode* parent);
    void insert(DOMNode* parent, DOMNode* child, DOMNode* before, const char* method);
    void insertNodes(DOMNode* parent, const Info& info, DOMNode* before, const char* method);
    std::vector<std::unique_ptr<DOMNode>> parseFragment(const std::string& html, DOMNode* context);
    void setText(DOMNode* node, const std::string& text);
    Zepra::Runtime::Value nodeList(const std::vector<DOMNode*>& nodes);
    Zepra::Runtime::Value elementList(const std::vector<DOMElement*>& elements);
    [[noreturn]] void throwDOMException(const std::string& name, const std::string& message);

    // Events
    EventObject* newEvent(Zepra::Runtime::Object* proto, const std::string& type, const Zepra::Runtime::Value& init);
    bool dispatch(Zepra::Runtime::Object* target, EventObject* event);
    void invokeListeners(Zepra::Runtime::Object* target, EventObject* event, bool capturePhase);
    void callHandler(Zepra::Runtime::Object* target, EventObject* event, bool capturePhase);
    void reportException(const std::exception& e);
};

} // namespace Zepra::WebCore
