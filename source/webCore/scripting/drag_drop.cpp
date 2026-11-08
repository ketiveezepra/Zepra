// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

/**
 * @file drag_drop.cpp
 * @brief HTML drag and drop inside the page (WHATWG HTML "Drag and drop")
 *
 * Pressing the primary button on a draggable element (draggable="true", or a
 * link or image that is not draggable="false") and moving the pointer more
 * than a few pixels starts a drag: dragstart at the element, where the page
 * fills the DataTransfer. Mouse events stop until the button comes up. Each
 * move fires drag at the source, dragenter/dragleave when the element under
 * the pointer changes, then dragover there; a dragover listener that calls
 * preventDefault() makes the element a drop target. On release, drop fires
 * at the target that accepted (else dragleave), then dragend at the source
 * with the dropEffect. A text field accepts plain text without any script:
 * the text goes in with beforeinput/input of type "insertFromDrop".
 *
 * Not yet: dragging files in from the desktop, dragging selected text, a
 * drag image under the pointer.
 */

#include "scripting/script_context.hpp"
#include "scripting/dom_bindings.hpp"
#include "dom/form_controls.hpp"

#include "runtime/execution/js_error.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace Zepra::WebCore {

using namespace Zepra::Runtime;

namespace {

/// How far (in CSS pixels) the pointer moves with the button down before a drag starts
constexpr double kDragThreshold = 4.0;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

Value str(const std::string& text) { return Value::string(new String(text)); }

bool editableField(const DOMElement* e) {
    return e && isTextControl(e) && !e->hasAttribute("readonly") && !isDisabledControl(e);
}

std::string argText(const std::vector<Value>& args, size_t index) {
    return index < args.size() ? args[index].toString() : std::string("undefined");
}

}  // namespace

/// A DataTransfer's drag data store and what the page may do with it now
struct DataTransferState {
    enum class Mode {
        ReadWrite,   ///< dragstart (and a script-made DataTransfer): setData works
        ReadOnly,    ///< drop: getData works
        Protected,   ///< other drag events: only the formats are visible
    };
    Mode mode = Mode::ReadWrite;
    std::vector<std::pair<std::string, std::string>> items;  ///< format (lower case), data
    std::string dropEffect = "none";
    std::string effectAllowed = "uninitialized";

    std::string get(const std::string& format) const {
        for (const auto& item : items) if (item.first == format) return item.second;
        return "";
    }
};

/// The drag in progress, from the press that may start one to the release
struct DragState {
    bool candidate = false;      ///< the button went down on a draggable element
    bool active = false;         ///< dragstart fired and was not canceled
    double startX = 0, startY = 0;
    Value source;                ///< the dragged element's wrapper (its node may be destroyed)
    Value target;                ///< the element under the pointer (the current target element)
    bool accepted = false;       ///< the last dragover was canceled (or a text field takes the text)
    bool fieldDefault = false;   ///< accepted only because the target is a text field
    std::shared_ptr<DataTransferState> data;
    Object* dataTransfer = nullptr;
};

namespace {

// "text" and "url" are the old names of text/plain and text/uri-list
std::string normalizeFormat(const std::string& format) {
    std::string f = lower(format);
    if (f == "text") return "text/plain";
    if (f == "url") return "text/uri-list";
    return f;
}

bool isOneOf(const std::string& value, std::initializer_list<const char*> options) {
    return std::any_of(options.begin(), options.end(), [&](const char* o) { return value == o; });
}

/// The dropEffect a dragover/drop starts with for this effectAllowed
std::string defaultDropEffect(const std::string& effectAllowed) {
    if (effectAllowed == "none") return "none";
    if (isOneOf(effectAllowed, {"copy", "copyLink", "copyMove", "all", "uninitialized"})) return "copy";
    if (isOneOf(effectAllowed, {"link", "linkMove"})) return "link";
    return "move";
}

/// Whether effectAllowed lets the operation `dropEffect` happen
bool effectAllowedFor(const std::string& effectAllowed, const std::string& dropEffect) {
    if (dropEffect == "none") return false;
    if (effectAllowed == "all" || effectAllowed == "uninitialized") return true;
    if (effectAllowed == "none") return false;
    std::string e = lower(effectAllowed);
    return e.find(dropEffect) != std::string::npos;
}

void defineAccessor(Object* target, const std::string& name, NativeFn getter, NativeFn setter) {
    PropertyDescriptor desc;
    desc.getter = Value::object(createNativeFunction("get " + name, std::move(getter), 0));
    if (setter) desc.setter = Value::object(createNativeFunction("set " + name, std::move(setter), 1));
    desc.attributes = PropertyAttribute::Enumerable | PropertyAttribute::Configurable;
    target->defineProperty(name, desc);
}

void defineMethod(Object* target, const std::string& name, size_t arity, NativeFn fn) {
    PropertyDescriptor desc;
    desc.value = Value::object(createNativeFunction(name, std::move(fn), arity));
    desc.attributes = PropertyAttribute::Writable | PropertyAttribute::Configurable;
    target->defineProperty(name, desc);
}

/// A DataTransfer object over `state`
Object* makeDataTransfer(const std::shared_ptr<DataTransferState>& state, Object* prototype) {
    auto* dt = new Object();
    if (prototype) dt->setPrototype(prototype);
    defineMethod(dt, "setData", 2, [state](Context*, const std::vector<Value>& args) -> Value {
        if (args.size() < 2) throw JSError("TypeError", "DataTransfer.setData: 2 arguments required");
        if (state->mode != DataTransferState::Mode::ReadWrite) return Value::undefined();
        const std::string format = normalizeFormat(argText(args, 0));
        const std::string data = argText(args, 1);
        for (auto& item : state->items) {
            if (item.first == format) {
                item.second = data;
                return Value::undefined();
            }
        }
        state->items.emplace_back(format, data);
        return Value::undefined();
    });
    defineMethod(dt, "getData", 1, [state](Context*, const std::vector<Value>& args) -> Value {
        if (args.empty()) throw JSError("TypeError", "DataTransfer.getData: 1 argument required");
        if (state->mode == DataTransferState::Mode::Protected) return str("");
        const std::string format = normalizeFormat(argText(args, 0));
        if (format == "text/uri-list" || lower(argText(args, 0)) == "url") {
            // "URL" reads the first URL of the list
            std::string list = state->get("text/uri-list");
            if (lower(argText(args, 0)) == "url") {
                size_t start = 0;
                while (start < list.size()) {
                    size_t end = list.find('\n', start);
                    std::string line = list.substr(start, end - start);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (!line.empty() && line[0] != '#') return str(line);
                    if (end == std::string::npos) break;
                    start = end + 1;
                }
                return str("");
            }
            return str(list);
        }
        return str(state->get(format));
    });
    defineMethod(dt, "clearData", 0, [state](Context*, const std::vector<Value>& args) -> Value {
        if (state->mode != DataTransferState::Mode::ReadWrite) return Value::undefined();
        if (args.empty() || args[0].isUndefined()) {
            state->items.clear();
        } else {
            const std::string format = normalizeFormat(args[0].toString());
            state->items.erase(std::remove_if(state->items.begin(), state->items.end(),
                                              [&](const auto& item) { return item.first == format; }),
                               state->items.end());
        }
        return Value::undefined();
    });
    defineMethod(dt, "setDragImage", 3, [](Context*, const std::vector<Value>&) -> Value {
        return Value::undefined();   // no drag image is drawn yet
    });
    defineAccessor(dt, "types", [state](Context*, const std::vector<Value>&) -> Value {
        auto* list = new Array();
        for (const auto& item : state->items) list->push(str(item.first));
        return Value::object(list);
    }, nullptr);
    defineAccessor(dt, "files", [](Context*, const std::vector<Value>&) -> Value {
        auto* list = new Array();   // no files are dragged in yet
        return Value::object(list);
    }, nullptr);
    defineAccessor(dt, "dropEffect", [state](Context*, const std::vector<Value>&) -> Value {
        return str(state->dropEffect);
    }, [state](Context*, const std::vector<Value>& args) -> Value {
        std::string value = argText(args, 0);
        if (isOneOf(value, {"none", "copy", "link", "move"})) state->dropEffect = value;
        return Value::undefined();
    });
    defineAccessor(dt, "effectAllowed", [state](Context*, const std::vector<Value>&) -> Value {
        return str(state->effectAllowed);
    }, [state](Context*, const std::vector<Value>& args) -> Value {
        std::string value = argText(args, 0);
        if (state->mode == DataTransferState::Mode::ReadWrite &&
            isOneOf(value, {"none", "copy", "copyLink", "copyMove", "link", "linkMove", "move", "all",
                            "uninitialized"})) {
            state->effectAllowed = value;
        }
        return Value::undefined();
    });
    return dt;
}

/// The element a press on `target` would drag: the nearest draggable one
DOMElement* draggableFrom(DOMElement* target) {
    for (DOMNode* n = target; n; n = n->parentNode()) {
        if (n->nodeType() != NodeType::Element) continue;
        auto* e = static_cast<DOMElement*>(n);
        std::string draggable = lower(e->getAttribute("draggable"));
        if (draggable == "true") return e;
        if (draggable == "false") return nullptr;
        std::string tag = lower(e->tagName());
        if ((tag == "a" && e->hasAttribute("href")) || tag == "img") return e;
        // A press inside a text field selects text instead
        if (isTextControl(e)) return nullptr;
    }
    return nullptr;
}

}  // namespace

void ScriptContext::setupDragAndDrop(Object* windowObj) {
    // new DataTransfer(): an empty, writable one (for synthetic DragEvents)
    auto* prototype = new Object();
    dataTransferProto_ = prototype;
    auto* ctor = createNativeFunction("DataTransfer", [prototype](Context*, const std::vector<Value>&) -> Value {
        return Value::object(makeDataTransfer(std::make_shared<DataTransferState>(), prototype));
    }, 0);
    ctor->set("prototype", Value::object(prototype));
    PropertyDescriptor ctorDesc;
    ctorDesc.value = Value::object(ctor);
    ctorDesc.attributes = PropertyAttribute::Writable | PropertyAttribute::Configurable;
    prototype->defineProperty("constructor", ctorDesc);
    windowObj->defineProperty("DataTransfer", ctorDesc);
}

DOMElement* ScriptContext::dragElement(const Value& wrapper) const {
    if (!wrapper.isObject() || !domBindings_) return nullptr;
    DOMNode* node = DOMBindings::nodeOf(wrapper.asObject());
    if (!node || node->nodeType() != NodeType::Element) return nullptr;
    auto* element = static_cast<DOMElement*>(node);
    return domBindings_->isConnected(element) ? element : nullptr;
}

void ScriptContext::noteDragPress(DOMElement* target, double clientX, double clientY) {
    drag_.reset();
    DOMElement* source = target ? draggableFrom(target) : nullptr;
    if (!source || !domBindings_) return;
    drag_ = std::make_shared<DragState>();
    drag_->candidate = true;
    drag_->startX = clientX;
    drag_->startY = clientY;
    drag_->source = domBindings_->wrap(source);
    // The store keeps the wrapper (and so the node's JS side) alive
    if (callbackStore_) callbackStore_->set("dragSource", drag_->source);
}

bool ScriptContext::dragging() const { return drag_ && drag_->active; }

void ScriptContext::endDrag() {
    drag_.reset();
    removeStoredCallback("dragSource");
    removeStoredCallback("dragTarget");
    removeStoredCallback("dragDataTransfer");
}

bool ScriptContext::dragMove(DOMElement* target, double clientX, double clientY, bool buttonDown) {
    if (!drag_ || !domBindings_) return false;
    if (!buttonDown) {
        // The release happened somewhere we did not hear about (off the window)
        if (drag_->active) dragRelease(nullptr, clientX, clientY);
        else endDrag();
        return false;
    }
    if (target && !domBindings_->isConnected(target)) target = nullptr;

    if (!drag_->active) {
        if (std::hypot(clientX - drag_->startX, clientY - drag_->startY) <= kDragThreshold) return false;
        DOMElement* source = dragElement(drag_->source);
        if (!source) {
            endDrag();
            return false;
        }
        // dragstart: the page fills the data store. Links and images carry
        // their URL unless the page replaces it.
        auto state = std::make_shared<DataTransferState>();
        std::string tag = lower(source->tagName());
        std::string url;
        if (tag == "a") url = source->getAttribute("href");
        else if (tag == "img") url = source->getAttribute("src");
        if (!url.empty()) {
            state->items.emplace_back("text/uri-list", url);
            state->items.emplace_back("text/plain", url);
        }
        drag_->data = state;
        drag_->dataTransfer = makeDataTransfer(state, dataTransferProto_);
        if (callbackStore_) callbackStore_->set("dragDataTransfer", Value::object(drag_->dataTransfer));
        bool started = true;
        runTask([&] {
            MouseEventInit init;
            init.buttons = 1;
            init.dataTransfer = drag_->dataTransfer;
            started = domBindings_->fireMouseEvent(source, "dragstart", drag_->startX, drag_->startY, init);
        }, "event listener");
        if (!drag_) return false;
        if (!started) {
            // A canceled dragstart: no drag, the press goes on as a plain one
            endDrag();
            return false;
        }
        drag_->candidate = false;
        drag_->active = true;
        drag_->data->mode = DataTransferState::Mode::Protected;
        // The page no longer sees the pointer as over what it was over
        domBindings_->setPointerElement(nullptr);
    }

    std::shared_ptr<DragState> drag = drag_;
    runTask([&] {
        MouseEventInit init;
        init.buttons = 1;
        init.dataTransfer = drag->dataTransfer;
        if (DOMElement* source = dragElement(drag->source)) {
            domBindings_->fireMouseEvent(source, "drag", clientX, clientY, init);
        }
        // The element under the pointer changed: dragenter there, then
        // dragleave at the one it left
        DOMElement* old = dragElement(drag->target);
        if (target != old) {
            drag->accepted = false;
            if (target) {
                MouseEventInit enter = init;
                enter.relatedTarget = old;
                domBindings_->fireMouseEvent(target, "dragenter", clientX, clientY, enter);
            }
            drag->target = target ? domBindings_->wrap(target) : Value::undefined();
            if (callbackStore_) callbackStore_->set("dragTarget", drag->target);
            if (old) {
                MouseEventInit leave = init;
                leave.relatedTarget = target;
                leave.cancelable = false;
                domBindings_->fireMouseEvent(old, "dragleave", clientX, clientY, leave);
            }
        }
        // dragover: canceled means "drop here"
        DOMElement* current = dragElement(drag->target);
        drag->accepted = false;
        drag->fieldDefault = false;
        if (!current) return;
        drag->data->dropEffect = defaultDropEffect(drag->data->effectAllowed);
        bool notCanceled = domBindings_->fireMouseEvent(current, "dragover", clientX, clientY, init);
        if (!notCanceled) {
            drag->accepted = effectAllowedFor(drag->data->effectAllowed, drag->data->dropEffect);
        } else if (editableField(current) && !drag->data->get("text/plain").empty()) {
            // A text field takes plain text by default
            drag->data->dropEffect = "copy";
            drag->accepted = effectAllowedFor(drag->data->effectAllowed, "copy");
            drag->fieldDefault = drag->accepted;
        }
        if (!drag->accepted) drag->data->dropEffect = "none";
    }, "event listener");
    return true;
}

bool ScriptContext::dragRelease(DOMElement* target, double clientX, double clientY) {
    if (!drag_ || !domBindings_) return false;
    if (!drag_->active) {
        endDrag();
        return false;
    }
    std::shared_ptr<DragState> drag = drag_;
    DOMElement* insertInto = nullptr;
    std::string droppedText;
    runTask([&] {
        MouseEventInit init;
        init.dataTransfer = drag->dataTransfer;
        DOMElement* current = dragElement(drag->target);
        // Released somewhere else than the last dragover saw: that element
        // gets no drop
        if (target != current) drag->accepted = false;
        if (current && drag->accepted) {
            drag->data->mode = DataTransferState::Mode::ReadOnly;
            bool notCanceled = domBindings_->fireMouseEvent(current, "drop", clientX, clientY, init);
            drag->data->mode = DataTransferState::Mode::Protected;
            if (notCanceled && drag->fieldDefault) {
                insertInto = current;
                droppedText = drag->data->get("text/plain");
            } else if (notCanceled) {
                // Nobody handled the drop: nothing happened
                drag->data->dropEffect = "none";
            }
        } else {
            drag->data->dropEffect = "none";
            if (current) {
                MouseEventInit leave = init;
                leave.cancelable = false;
                domBindings_->fireMouseEvent(current, "dragleave", clientX, clientY, leave);
            }
        }
        if (DOMElement* source = dragElement(drag->source)) {
            MouseEventInit end = init;
            end.cancelable = false;
            domBindings_->fireMouseEvent(source, "dragend", clientX, clientY, end);
        }
    }, "event listener");
    endDrag();
    // The text goes into the field it was dropped on, as typing would put it
    if (insertInto && domBindings_->isConnected(insertInto)) {
        runTask([&] { domBindings_->setFocus(insertInto); }, "focus listener");
        if (focusedElement() == insertInto) insertText(droppedText, "insertFromDrop");
    }
    return true;
}

}  // namespace Zepra::WebCore
