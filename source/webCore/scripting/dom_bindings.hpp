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
#include "scripting/page_geometry.hpp"
#include "dom/form_controls.hpp"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Zepra::Runtime { class Object; class Value; class VM; class FunctionCallInfo; }

namespace Zepra::WebCore {

class CanvasRegistry;
class CSSComputedStyle;
class NodeWrapper;
class EventObject;

/// The members of a KeyboardEvent the browser fills in for a key the user pressed
struct KeyEventInit {
    std::string key;       // "a", "A", "Enter", "ArrowLeft", " "
    std::string code;      // physical key: "KeyA", "Enter", "Space"
    int keyCode = 0;       // legacy keyCode / which (65 for A, 13 for Enter)
    int location = 0;      // DOM_KEY_LOCATION_*
    bool repeat = false;
    bool altKey = false;
    bool ctrlKey = false;
    bool shiftKey = false;
    bool metaKey = false;
};

class DOMBindings {
public:
    using ErrorReporter = std::function<void(const std::string& message)>;
    /// Compiles `source` (a function expression) to a function value
    using FunctionCompiler = std::function<bool(const std::string& source, Zepra::Runtime::Value& out)>;
    /// An element's computed style, current with the DOM; null when it has none
    using ComputedStyleSource = std::function<const CSSComputedStyle*(DOMElement* element)>;

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
    /// Where getBoundingClientRect, offset*, client*, scroll* and the
    /// window's size and scroll position come from (the browser's layout).
    /// Without one, elements have no boxes and the page does not scroll.
    void setPageGeometry(PageGeometry* geometry) { geometry_ = geometry; refreshWindowGlobals(); }
    /// Copies the viewport size and scroll position to the bare globals
    /// (innerWidth, scrollY, ...), which are not the window's accessors
    void refreshWindowGlobals();
    /// document.readyState: "loading", "interactive" or "complete"
    void setReadyState(const std::string& state) { readyState_ = state; }
    /// Where getComputedStyle() reads styles (the browser's CSS engine).
    /// Without one every computed value is "".
    void setComputedStyleSource(ComputedStyleSource source) { computedStyleSource_ = std::move(source); }

    /// The values getComputedStyle(element) reads, by CSS property name;
    /// empty for an element outside the document or without a style
    std::vector<std::pair<std::string, std::string>> computedValues(DOMElement* element) const;

    /// The JS object of a node: made on first use, the same one afterwards
    Zepra::Runtime::Value wrap(DOMNode* node);
    /// The node behind a wrapper object; null for any other object (or a destroyed node)
    static DOMNode* nodeOf(const Zepra::Runtime::Object* object);

    /// Fires a plain event at a node, or at the window when `target` is
    /// null; false when a listener canceled it
    bool fireEvent(DOMNode* target, const std::string& type, bool bubbles, bool cancelable);
    /// Fires a mouse event (clientX/clientY) at a connected element
    bool fireMouseEvent(DOMElement* target, const std::string& type, double clientX, double clientY);
    /// A click as the user makes it (and el.click()): a checkbox or radio
    /// toggles before listeners run and back when one cancels; otherwise it
    /// gets input and change, a submit button submits its form, a reset
    /// button resets it, a <label> clicks its control. False when canceled.
    bool click(DOMElement* target, double clientX, double clientY);
    /// Fires keydown, keypress or keyup at a connected element; false when canceled
    bool fireKeyboardEvent(DOMElement* target, const std::string& type, const KeyEventInit& key);
    /// Fires beforeinput (cancelable) or input at a connected element, with
    /// its inputType ("insertText", "deleteContentBackward", ...) and data
    /// (null when `data` is empty); false when canceled
    bool fireInputEvent(DOMElement* target, const std::string& type, const std::string& inputType,
                        const std::string& data);

    // Focus and forms
    /// The element with keyboard focus (also what :focus matches); null when
    /// the page itself has it (document.activeElement is then the body)
    DOMElement* focusedElement() const;
    /// Moves focus to `element` (null: the page). The element losing focus
    /// gets change first when the user changed its value, then blur and
    /// focusout; the new one gets focus and focusin. An element that cannot
    /// take focus, or is not in the page, is ignored.
    void setFocus(DOMElement* element);
    /// The focused control's value counts as committed (Enter in a text
    /// field fires change without a blur)
    void commitFocusedValue();
    /// What a form submission does once it is not canceled (the browser
    /// navigates); without a handler a submission does nothing
    using SubmitHandler = std::function<void(const FormSubmission& submission)>;
    void setSubmitHandler(SubmitHandler handler) { submitHandler_ = std::move(handler); }
    /// requestSubmit(): a submit event (with its submitter) and, unless a
    /// listener canceled it, the submission; false when canceled
    bool requestSubmit(DOMElement* form, DOMElement* submitter);
    /// reset(): a reset event and, unless canceled, every control back to its attributes
    bool resetForm(DOMElement* form);

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
    ComputedStyleSource computedStyleSource_;
    PageGeometry* geometry_ = nullptr;
    std::string readyState_ = "loading";
    SubmitHandler submitHandler_;
    /// The focused control's value when it got focus (or last committed);
    /// a different value when it loses focus fires change
    std::string valueAtFocus_;

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
    Zepra::Runtime::Object* styleDeclarationProto_ = nullptr;   // CSSStyleDeclaration
    Zepra::Runtime::Object* domRectProto_ = nullptr;
    Zepra::Runtime::Object* keyboardEventProto_ = nullptr;
    Zepra::Runtime::Object* inputEventProto_ = nullptr;
    Zepra::Runtime::Object* focusEventProto_ = nullptr;
    Zepra::Runtime::Object* submitEventProto_ = nullptr;
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
    void installComputedStyle();
    void installGeometry();

    // Layout geometry
    static constexpr double kDefaultViewportWidth = 1280, kDefaultViewportHeight = 720;
    ElementGeometry geometryOf(const DOMElement* element, bool withScrollSize = false);
    /// Bare globals mirroring the window's (refreshWindowGlobals), and the
    /// values last written to them
    static constexpr size_t kWindowGlobals = 6;
    static constexpr const char* kWindowGlobalNames[kWindowGlobals] = {
        "innerWidth", "innerHeight", "scrollX", "scrollY", "pageXOffset", "pageYOffset"};
    double windowGlobals_[kWindowGlobals] = {};
    bool windowGlobalsSet_ = false;
    bool isScrollingElement(const DOMElement* element) const;
    Zepra::Runtime::Value rectObject(const GeometryRect& rect);
    DOMElement* offsetParentOf(DOMElement* element);
    void scrollPage(double x, double y);
    void installForms();

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
    bool fireFocusEvent(DOMElement* target, const std::string& type, DOMElement* related);
    /// The submission itself (form.submit() and an uncanceled submit event)
    void submit(DOMElement* form, DOMElement* submitter);
    void invokeListeners(Zepra::Runtime::Object* target, EventObject* event, bool capturePhase);
    void callHandler(Zepra::Runtime::Object* target, EventObject* event, bool capturePhase);
    void reportException(const std::exception& e);
};

} // namespace Zepra::WebCore
