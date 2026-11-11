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
class CustomElements;
class MutationObservers;
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

/// The members of a MouseEvent beyond its position
struct MouseEventInit {
    int button = 0;                     // which button changed: 0 primary
    int buttons = 0;                    // buttons held down: 1 primary
    int detail = 0;                     // click count for mousedown/mouseup/click
    DOMElement* relatedTarget = nullptr; // mouseover/out/enter/leave: the other element
    bool bubbles = true;
    bool cancelable = true;
    Zepra::Runtime::Object* dataTransfer = nullptr; // drag events: the event is a DragEvent carrying it
};

class DOMBindings {
public:
    using ErrorReporter = std::function<void(const std::string& message)>;
    /// Compiles `source` (a function expression) to a function value
    using FunctionCompiler = std::function<bool(const std::string& source, Zepra::Runtime::Value& out)>;
    /// An element's computed style, current with the DOM; null when it has none
    using ComputedStyleSource = std::function<const CSSComputedStyle*(DOMElement* element)>;
    /// The computed style of an element's pseudo-element ("before", "after",
    /// ...), current with the DOM; null when it has none
    using PseudoElementStyleSource =
        std::function<const CSSComputedStyle*(DOMElement* element, const std::string& pseudo)>;

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
    void setPageGeometry(PageGeometry* geometry) { geometry_ = geometry; }
    /// document.readyState: "loading", "interactive" or "complete"
    void setReadyState(const std::string& state) { readyState_ = state; }
    const std::string& readyState() const { return readyState_; }
    /// document.write()/writeln() (the text, writeln's newline included),
    /// document.open() and document.close(): the page's parser
    /// (ScriptContext). Without hooks they do nothing.
    struct DocumentStreamHooks {
        std::function<void(const std::string& text)> write;
        std::function<void()> open;
        std::function<void()> close;
    };
    void setDocumentStreamHooks(DocumentStreamHooks hooks) { streamHooks_ = std::move(hooks); }
    /// Removes every child of `parent` as a script would (removed nodes
    /// stay alive while a script can reach them): document.open()
    void removeChildrenOf(DOMNode* parent) { removeAllChildren(parent); }
    /// Where getComputedStyle() reads styles (the browser's CSS engine).
    /// Without one every computed value is "".
    void setComputedStyleSource(ComputedStyleSource source) { computedStyleSource_ = std::move(source); }
    /// Where getComputedStyle(element, '::before') reads styles. Without one
    /// pseudo-elements read as an empty declaration.
    void setPseudoElementStyleSource(PseudoElementStyleSource source) { pseudoStyleSource_ = std::move(source); }

    /// The values getComputedStyle(element[, pseudo]) reads, by CSS property
    /// name; empty for an element outside the document or without a style.
    /// `pseudo` is a pseudo-element name without colons ("before"), empty
    /// for the element itself.
    std::vector<std::pair<std::string, std::string>> computedValues(DOMElement* element,
                                                                    const std::string& pseudo = "") const;

    /// Called for each <script> element that may need preparing (HTML
    /// "prepare the script element"): one a script connected to the
    /// document (appendChild, insertBefore, append, ...), and a connected
    /// one whose text or src a script set. Runs after the insertion is
    /// complete; the hook decides whether the script runs (already started,
    /// type, ...). Scripts made by innerHTML and friends never get here.
    using ScriptElementHook = std::function<void(DOMElement* script)>;
    void setScriptElementHook(ScriptElementHook hook) { scriptHook_ = std::move(hook); }
    /// document.currentScript (null outside a classic <script>'s run)
    DOMElement* currentScript() const { return currentScript_; }
    void setCurrentScript(DOMElement* script) { currentScript_ = script; }
    /// The document's URL, which script.src resolves against
    void setDocumentUrlSource(std::function<std::string()> source) { documentUrlSource_ = std::move(source); }

    /// The JS object of a node: made on first use, the same one afterwards
    Zepra::Runtime::Value wrap(DOMNode* node);
    /// The node behind a wrapper object; null for any other object (or a destroyed node)
    static DOMNode* nodeOf(const Zepra::Runtime::Object* object);

    /// Fires a plain event at a node, or at the window when `target` is
    /// null; false when a listener canceled it
    bool fireEvent(DOMNode* target, const std::string& type, bool bubbles, bool cancelable);
    /// The same, with extra fields on the event (hashchange's oldURL/newURL)
    bool fireEvent(DOMNode* target, const std::string& type, bool bubbles, bool cancelable,
                   const std::vector<std::pair<std::string, Zepra::Runtime::Value>>& fields);
    /// Re-matches every MediaQueryList made by window.matchMedia() and fires
    /// `change` at each whose result changed since it was last reported, in
    /// creation order. Call from a script task (listeners run here).
    void evaluateMediaQueries();
    /// Fires a mouse event (clientX/clientY) at a connected element
    bool fireMouseEvent(DOMElement* target, const std::string& type, double clientX, double clientY);
    bool fireMouseEvent(DOMElement* target, const std::string& type, double clientX, double clientY,
                        const MouseEventInit& init);
    /// True when the element is in this page's document
    bool isConnected(const DOMElement* element) const;
    /// The element the primary button went down on (mousedown), until it
    /// comes up; and the element the pointer is over (mouseover/out)
    DOMElement* pressedElement() const { return pressed_; }
    void setPressedElement(DOMElement* element) { pressed_ = element; }
    DOMElement* pointerElement() const { return pointer_; }
    void setPointerElement(DOMElement* element) { pointer_ = element; }
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
    /// The user picks option `index` of a <select> (from its drop-down or
    /// with a key): input and change follow when the selection changed.
    /// False when nothing changed.
    bool pickOption(DOMElement* select, long index);
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
    /// Keeps a subtree something other than a script took out of the tree
    /// (the DevTools Inspector) as if a script had removed it: in its root's
    /// wrapper while a script can reach a node of it, otherwise until
    /// releaseDetachedNodes()
    void adoptDetachedNode(std::unique_ptr<DOMNode> root) { park(std::move(root)); }

private:
    friend class CustomElements;
    using Args = std::vector<Zepra::Runtime::Value>;
    using Info = Zepra::Runtime::FunctionCallInfo;

    Zepra::Runtime::VM* vm_;
    ErrorReporter reportError_;
    std::unique_ptr<MutationObservers> mutationObservers_;
    std::unique_ptr<CustomElements> customElements_;
    DOMElement* pressed_ = nullptr;
    DOMElement* pointer_ = nullptr;
    FunctionCompiler compileFunction_;
    DOMDocument* document_ = nullptr;
    /// Popovers with a queued toggle event, and the oldState it reports
    /// (HTML "popover toggle task tracker": later changes coalesce into it)
    std::unordered_map<DOMElement*, std::string> pendingPopoverToggles_;
    CanvasRegistry* canvases_ = nullptr;
    ComputedStyleSource computedStyleSource_;
    PseudoElementStyleSource pseudoStyleSource_;
    PageGeometry* geometry_ = nullptr;
    std::string readyState_ = "loading";
    SubmitHandler submitHandler_;
    ScriptElementHook scriptHook_;
    DocumentStreamHooks streamHooks_;
    DOMElement* currentScript_ = nullptr;
    std::function<std::string()> documentUrlSource_;
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
    Zepra::Runtime::Object* shadowRootProto_ = nullptr;
    Zepra::Runtime::Object* svgElementProto_ = nullptr;
    /// SVG element interfaces (SVGSVGElement, SVGPathElement, ...) by tag
    std::unordered_map<std::string, std::pair<std::string, Zepra::Runtime::Object*>> svgInterfaces_;
    Zepra::Runtime::Object* tokenListProto_ = nullptr;
    Zepra::Runtime::Object* eventProto_ = nullptr;
    Zepra::Runtime::Object* mouseEventProto_ = nullptr;
    Zepra::Runtime::Object* dragEventProto_ = nullptr;
    Zepra::Runtime::Object* styleDeclarationProto_ = nullptr;   // CSSStyleDeclaration
    Zepra::Runtime::Object* domRectProto_ = nullptr;
    Zepra::Runtime::Object* keyboardEventProto_ = nullptr;
    Zepra::Runtime::Object* inputEventProto_ = nullptr;
    Zepra::Runtime::Object* focusEventProto_ = nullptr;
    Zepra::Runtime::Object* submitEventProto_ = nullptr;
    Zepra::Runtime::Object* listItemFn_ = nullptr;  // item(i) on the arrays standing in for NodeLists
    Zepra::Runtime::Object* attrNamedItemFn_ = nullptr;  // getNamedItem(name) on element.attributes
    Zepra::Runtime::Object* allCollection_ = nullptr;    // document.all (one falsy HTMLAllCollection)
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
    void installMediaQueries();
    void installShadowDOM();
    void installTraversal();
    /// Documents made by document.implementation.createHTMLDocument()
    std::vector<std::unique_ptr<DOMDocument>> extraDocuments_;
    void installSVG();
    /// The SVG interface of an element in the SVG namespace (or under <svg>), else null
    const std::pair<std::string, Zepra::Runtime::Object*>* svgInterfaceOf(const DOMElement* element) const;

    // window.matchMedia(): the lists made, in creation order
    std::vector<Zepra::Runtime::Object*> mediaQueryLists_;
    Zepra::Runtime::Object* mediaQueryListEventProto_ = nullptr;

    // Layout geometry
    static constexpr double kDefaultViewportWidth = 1280, kDefaultViewportHeight = 720;
    ElementGeometry geometryOf(const DOMElement* element, bool withScrollSize = false);
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
    /// The wrapper of a new node no tree holds; the wrapper keeps it
    Zepra::Runtime::Value own(std::unique_ptr<DOMNode> node);
    void park(std::unique_ptr<DOMNode> root);
    void removeAllChildren(DOMNode* parent);
    void insert(DOMNode* parent, DOMNode* child, DOMNode* before, const char* method);
    void insertNodes(DOMNode* parent, const Info& info, DOMNode* before, const char* method);
    /// Script steps after `nodes` went into `parent` (see setScriptElementHook)
    void nodesInserted(DOMNode* parent, const std::vector<DOMNode*>& nodes);
    void scriptChildrenChanged(DOMNode* node);
    void scriptAttributeChanged(DOMElement* element, const std::string& name);
    std::string documentUrl() const;
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
