// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file custom_elements.cpp
 * @brief Custom element registry, upgrades and reactions (see custom_elements.hpp)
 */

#include "scripting/custom_elements.hpp"
#include "scripting/dom_bindings.hpp"

#include "runtime/async/promise.hpp"
#include "runtime/execution/js_error.hpp"
#include "runtime/execution/vm.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"

#include <algorithm>
#include <deque>

namespace Zepra::WebCore {

using namespace Zepra::Runtime;

namespace {

Value str(const std::string& s) { return Value::string(new String(s)); }

void defineHidden(Object* obj, const std::string& name, Value value) {
    PropertyDescriptor desc;
    desc.value = value;
    desc.attributes = PropertyAttribute::Writable | PropertyAttribute::Configurable;
    obj->defineProperty(name, desc);
}

void method(Object* obj, const std::string& name, size_t arity, BuiltinFn fn) {
    defineHidden(obj, name, Value::object(new Function(name, std::move(fn), arity)));
}

bool isCallable(const Value& v) { return v.isObject() && v.asObject()->isFunction(); }

DOMElement* asElement(DOMNode* node) {
    return node && node->nodeType() == NodeType::Element ? static_cast<DOMElement*>(node) : nullptr;
}

// ----------------------------------------------------------------------------
// Reactions: one queue per element; element queues on a stack, one per
// [CEReactions] call in progress, and the backup queue below them all
// ----------------------------------------------------------------------------

struct Reaction {
    CustomElements* owner = nullptr;
    std::shared_ptr<CustomElementDefinition> upgrade;  // set: an upgrade reaction
    Value callback;
    std::vector<Value> args;
};

struct ReactionState {
    std::vector<std::vector<Object*>> elementQueues;
    std::vector<Object*> backupQueue;
    bool backupScheduled = false;
    // Keyed by the element's wrapper (never freed, and it knows when its
    // node is gone)
    std::unordered_map<Object*, std::deque<Reaction>> reactions;
};

ReactionState& reactionState() {
    static ReactionState state;
    return state;
}

void invokeReactions(const std::vector<Object*>& queue) {
    ReactionState& s = reactionState();
    for (Object* wrapper : queue) {
        for (;;) {
            // Callbacks can queue more (for this element too): look again each time
            auto it = s.reactions.find(wrapper);
            if (it == s.reactions.end()) break;
            if (it->second.empty()) {
                s.reactions.erase(it);
                break;
            }
            Reaction r = std::move(it->second.front());
            it->second.pop_front();
            DOMElement* element = asElement(DOMBindings::nodeOf(wrapper));
            if (!element || !r.owner) continue;
            if (r.upgrade) {
                r.owner->upgrade(element, r.upgrade);
                continue;
            }
            try {
                r.owner->vm()->call(static_cast<Function*>(r.callback.asObject()), Value::object(wrapper), r.args);
            } catch (const std::exception& e) {
                r.owner->report(e);
            }
        }
    }
}

void enqueueElement(Object* wrapper, CustomElements* owner) {
    ReactionState& s = reactionState();
    if (!s.elementQueues.empty()) {
        s.elementQueues.back().push_back(wrapper);
        return;
    }
    s.backupQueue.push_back(wrapper);
    if (s.backupScheduled) return;
    s.backupScheduled = true;
    owner->vm()->enqueueJob([] {
        ReactionState& state = reactionState();
        state.backupScheduled = false;
        std::vector<Object*> queue = std::move(state.backupQueue);
        state.backupQueue.clear();
        invokeReactions(queue);
    });
}

}  // namespace

CEReactionsScope::CEReactionsScope() { reactionState().elementQueues.emplace_back(); }

void CEReactionsScope::leave() {
    if (!open_) return;
    open_ = false;
    ReactionState& s = reactionState();
    std::vector<Object*> queue = std::move(s.elementQueues.back());
    s.elementQueues.pop_back();
    if (!queue.empty()) invokeReactions(queue);
}

CEReactionsScope::~CEReactionsScope() {
    if (!open_) return;
    // Left without leave() (an exception on its way out): what was queued
    // runs with the enclosing call, or at the next checkpoint
    ReactionState& s = reactionState();
    std::vector<Object*> queue = std::move(s.elementQueues.back());
    s.elementQueues.pop_back();
    auto& into = s.elementQueues.empty() ? s.backupQueue : s.elementQueues.back();
    into.insert(into.end(), queue.begin(), queue.end());
}

void forEachShadowIncludingElement(DOMNode* root, const std::function<void(DOMElement*)>& visit) {
    if (!root) return;
    if (DOMElement* element = asElement(root)) {
        visit(element);
        if (DOMShadowRoot* shadow = element->shadowRoot()) forEachShadowIncludingElement(shadow, visit);
    }
    // Visiting can run no script, but copy the list anyway: cheap and safe
    std::vector<DOMNode*> children;
    children.reserve(root->childNodes().size());
    for (const auto& child : root->childNodes()) children.push_back(child.get());
    for (DOMNode* child : children) forEachShadowIncludingElement(child, visit);
}

// ----------------------------------------------------------------------------
// The document's mutation sink: insertion, removal and attribute steps
// ----------------------------------------------------------------------------

class CustomElements::Sink final : public DOMMutationSink {
public:
    explicit Sink(CustomElements& owner) : owner_(owner) {}
    DOMMutationSink* next = nullptr;

    void childList(DOMNode* parent, DOMNode* added, DOMNode* removed, DOMNode* previousSibling,
                   DOMNode* nextSibling) override {
        // Until something is defined no element can be custom or upgraded
        if (!owner_.definitions_.empty()) {
            if (removed && owner_.connected(parent)) owner_.disconnectedSubtree(removed);
            if (added && owner_.connected(added)) owner_.connectedSubtree(added);
        }
        if (next) next->childList(parent, added, removed, previousSibling, nextSibling);
    }
    void attribute(DOMElement* element, const std::string& name, const std::string* oldValue) override {
        if (!owner_.definitions_.empty()) owner_.attributeChanged(element, name, oldValue);
        if (next) next->attribute(element, name, oldValue);
    }
    void characterData(DOMNode* node, const std::string& oldValue) override {
        if (next) next->characterData(node, oldValue);
    }

private:
    CustomElements& owner_;
};

// ----------------------------------------------------------------------------
// CustomElements
// ----------------------------------------------------------------------------

CustomElements::CustomElements(DOMBindings& bindings, VM* vm)
    : bindings_(bindings), vm_(vm), sink_(std::make_unique<Sink>(*this)) {}

CustomElements::~CustomElements() {
    // Reactions of this page must not run after it
    for (auto& [wrapper, queue] : reactionState().reactions) {
        queue.erase(std::remove_if(queue.begin(), queue.end(), [this](const Reaction& r) { return r.owner == this; }),
                    queue.end());
    }
    DOMDocument::removeMutationSink(sink_.get());
}

DOMMutationSink* CustomElements::sink(DOMMutationSink* next) {
    sink_->next = next;
    return sink_.get();
}

void CustomElements::report(const std::exception& e) { bindings_.reportException(e); }

bool CustomElements::connected(const DOMNode* node) const {
    return node && bindings_.document_ && shadowIncludingRoot(node) == bindings_.document_;
}

std::shared_ptr<CustomElementDefinition> CustomElements::lookup(const std::string& localName) const {
    auto it = definitions_.find(localName);
    return it != definitions_.end() ? it->second : nullptr;
}

void CustomElements::enqueueCallback(DOMElement* element, const Value& callback, std::vector<Value> args) {
    if (!isCallable(callback)) return;
    Object* wrapper = bindings_.wrap(element).asObject();
    Reaction r;
    r.owner = this;
    r.callback = callback;
    r.args = std::move(args);
    reactionState().reactions[wrapper].push_back(std::move(r));
    enqueueElement(wrapper, this);
}

void CustomElements::tryToUpgrade(DOMElement* element) {
    if (element->customElementState() != CustomElementState::Undefined) return;
    auto definition = lookup(element->tagName());
    if (!definition) return;
    Object* wrapper = bindings_.wrap(element).asObject();
    Reaction r;
    r.owner = this;
    r.upgrade = definition;
    reactionState().reactions[wrapper].push_back(std::move(r));
    enqueueElement(wrapper, this);
}

void CustomElements::connectedSubtree(DOMNode* root) {
    forEachShadowIncludingElement(root, [this](DOMElement* element) {
        if (element->customElementState() == CustomElementState::Custom) {
            if (auto definition = lookup(element->tagName())) enqueueCallback(element, definition->connectedCallback, {});
        } else {
            tryToUpgrade(element);
        }
    });
}

void CustomElements::disconnectedSubtree(DOMNode* root) {
    forEachShadowIncludingElement(root, [this](DOMElement* element) {
        if (element->customElementState() != CustomElementState::Custom) return;
        if (auto definition = lookup(element->tagName())) enqueueCallback(element, definition->disconnectedCallback, {});
    });
}

void CustomElements::attributeChanged(DOMElement* element, const std::string& name, const std::string* oldValue) {
    if (element->customElementState() != CustomElementState::Custom) return;
    auto definition = lookup(element->tagName());
    if (!definition || !definition->observedAttributes.count(name)) return;
    Value newValue = element->hasAttribute(name) ? str(element->getAttribute(name)) : Value::null();
    enqueueCallback(element, definition->attributeChangedCallback,
                    {str(name), oldValue ? str(*oldValue) : Value::null(), newValue});
}

void CustomElements::enqueueUpgrades(DOMNode* root) {
    forEachShadowIncludingElement(root, [this](DOMElement* element) { tryToUpgrade(element); });
}

void CustomElements::upgradeTree(DOMNode* root) { enqueueUpgrades(root); }

void CustomElements::upgrade(DOMElement* element, const std::shared_ptr<CustomElementDefinition>& definition) {
    CustomElementState state = element->customElementState();
    if (state != CustomElementState::Undefined && state != CustomElementState::Uncustomized) return;
    element->setCustomElementState(CustomElementState::Failed);
    Object* wrapper = bindings_.wrap(element).asObject();

    // Its attributes and connection are reported once it is constructed
    if (isCallable(definition->attributeChangedCallback)) {
        std::vector<std::pair<std::string, std::string>> attributes(element->attributes().begin(),
                                                                    element->attributes().end());
        std::sort(attributes.begin(), attributes.end());
        for (const auto& [name, value] : attributes) {
            if (!definition->observedAttributes.count(name)) continue;
            enqueueCallback(element, definition->attributeChangedCallback, {str(name), Value::null(), str(value)});
        }
    }
    if (connected(element)) enqueueCallback(element, definition->connectedCallback, {});

    definition->constructionStack.push_back(element);
    const size_t depth = definition->constructionStack.size();
    try {
        Value result = vm_->construct(definition->constructor, {});
        if (definition->constructionStack.size() >= depth) definition->constructionStack.resize(depth - 1);
        if (!result.isObject() || DOMBindings::nodeOf(result.asObject()) != element) {
            throw JSError("TypeError", "Failed to upgrade '" + definition->name +
                          "': the constructor did not return the element being upgraded");
        }
    } catch (const std::exception& e) {
        if (definition->constructionStack.size() >= depth) definition->constructionStack.resize(depth - 1);
        reactionState().reactions.erase(wrapper);
        report(e);
        return;
    }
    element->setCustomElementState(CustomElementState::Custom);
}

Value CustomElements::create(const std::string& localName) {
    auto definition = lookup(localName);
    if (!definition) return Value::undefined();
    try {
        Value result = vm_->construct(definition->constructor, {});
        DOMElement* element = result.isObject() ? asElement(DOMBindings::nodeOf(result.asObject())) : nullptr;
        if (!element) {
            throw JSError("TypeError", "Failed to execute 'createElement' on 'Document': The result must implement HTMLElement interface");
        }
        if (!element->attributes().empty() || element->hasChildNodes() || element->parentNode() ||
            element->tagName() != localName) {
            throw JSError("NotSupportedError", "Failed to execute 'createElement' on 'Document': The result must not have attributes, children or a parent, and must have the local name '" + localName + "'");
        }
        return result;
    } catch (const std::exception& e) {
        report(e);
    }
    // A constructor that threw leaves an HTMLUnknownElement-like element
    auto element = std::make_unique<DOMElement>(localName);
    if (bindings_.document_) element->setOwnerDocument(bindings_.document_);
    element->setCustomElementState(CustomElementState::Failed);
    return bindings_.own(std::move(element));
}

// The HTMLElement constructor, as `super()` of a custom element class:
// a new element of the definition's name, or the element being upgraded
Value CustomElements::constructHTMLElement(const FunctionCallInfo& info) {
    Value self = info.thisValue();
    Object* proto = self.isObject() ? self.asObject()->prototype() : nullptr;
    std::shared_ptr<CustomElementDefinition> definition;
    if (proto) {
        for (const auto& [name, d] : definitions_) {
            if (d->prototype == proto) { definition = d; break; }
        }
        if (!definition) {
            Value ctor = proto->get("constructor");
            for (const auto& [name, d] : definitions_) {
                if (ctor.isObject() && ctor.asObject() == d->constructor) { definition = d; break; }
            }
        }
    }
    if (!definition) throw JSError("TypeError", "Illegal constructor");

    if (definition->constructionStack.empty()) {
        auto element = std::make_unique<DOMElement>(definition->localName);
        if (bindings_.document_) element->setOwnerDocument(bindings_.document_);
        element->setCustomElementState(CustomElementState::Custom);
        Value wrapper = bindings_.own(std::move(element));
        wrapper.asObject()->setPrototype(proto);
        return wrapper;
    }
    DOMElement* element = definition->constructionStack.back();
    if (!element) {
        throw JSError("TypeError", "Failed to construct '" + definition->name + "': this instance is already constructed");
    }
    definition->constructionStack.back() = nullptr;
    Object* wrapper = bindings_.wrap(element).asObject();
    wrapper->setPrototype(proto);
    return Value::object(wrapper);
}

Value CustomElements::define(const FunctionCallInfo& info) {
    const std::string name = info.argument(0).toString();
    Value ctorValue = info.argument(1);
    if (!isCallable(ctorValue) || !static_cast<Function*>(ctorValue.asObject())->isConstructor()) {
        throw JSError("TypeError", "Failed to execute 'define' on 'CustomElementRegistry': The provided value is not of type 'CustomElementConstructor'.");
    }
    auto* ctor = static_cast<Function*>(ctorValue.asObject());
    if (!isValidCustomElementName(name)) {
        bindings_.throwDOMException("SyntaxError", "Failed to execute 'define' on 'CustomElementRegistry': \"" + name + "\" is not a valid custom element name");
    }
    if (definitions_.count(name)) {
        bindings_.throwDOMException("NotSupportedError", "Failed to execute 'define' on 'CustomElementRegistry': the name \"" + name + "\" has already been used with this registry");
    }
    for (const auto& [n, d] : definitions_) {
        if (d->constructor == ctor) {
            bindings_.throwDOMException("NotSupportedError", "Failed to execute 'define' on 'CustomElementRegistry': this constructor has already been used with this registry");
        }
    }
    std::string localName = name;
    Value options = info.argument(2);
    if (options.isObject()) {
        Value extends = vm_->getMember(options, "extends");
        if (!extends.isUndefined()) {
            // Customized built-in elements (is="") are not supported: the
            // definition is kept, but no element is ever created from it
            std::string base = extends.toString();
            if (isValidCustomElementName(base)) {
                bindings_.throwDOMException("NotSupportedError", "Failed to execute 'define' on 'CustomElementRegistry': \"" + base + "\" is a valid custom element name");
            }
            localName = base;
        }
    }
    if (definitionRunning_) {
        bindings_.throwDOMException("NotSupportedError", "Failed to execute 'define' on 'CustomElementRegistry': an element definition is already running");
    }

    auto definition = std::make_shared<CustomElementDefinition>();
    definition->name = name;
    definition->localName = localName;
    definition->constructor = ctor;
    definitionRunning_ = true;
    try {
        Value proto = vm_->getMember(ctorValue, "prototype");
        if (!proto.isObject()) {
            throw JSError("TypeError", "Failed to execute 'define' on 'CustomElementRegistry': The prototype of the constructor is not an object");
        }
        definition->prototype = proto.asObject();
        auto callback = [&](const char* callbackName, Value& out) {
            Value v = vm_->getMember(proto, callbackName);
            if (v.isUndefined()) return;
            if (!isCallable(v)) {
                throw JSError("TypeError", std::string("Failed to execute 'define' on 'CustomElementRegistry': The '") +
                              callbackName + "' property of the prototype is not a function");
            }
            out = v;
        };
        callback("connectedCallback", definition->connectedCallback);
        callback("disconnectedCallback", definition->disconnectedCallback);
        callback("adoptedCallback", definition->adoptedCallback);
        callback("attributeChangedCallback", definition->attributeChangedCallback);
        if (isCallable(definition->attributeChangedCallback)) {
            Value observed = vm_->getMember(ctorValue, "observedAttributes");
            if (observed.isObject()) {
                Object* list = observed.asObject();
                double length = vm_->getMember(observed, "length").toNumber();
                for (uint32_t i = 0; length > 0 && i < static_cast<uint32_t>(length); i++) {
                    definition->observedAttributes.insert(list->get(i).toString());
                }
            }
        }
    } catch (...) {
        definitionRunning_ = false;
        throw;
    }
    definitionRunning_ = false;
    definitions_[name] = definition;

    // Elements already in the document are upgraded, in tree order, when
    // define() returns. Customized built-ins are not upgraded (see above).
    if (localName == name && bindings_.document_) {
        forEachShadowIncludingElement(bindings_.document_, [&](DOMElement* element) {
            if (element->tagName() == name) tryToUpgrade(element);
        });
    }

    auto pending = whenDefined_.find(name);
    if (pending != whenDefined_.end()) {
        pending->second->resolve(ctorValue);
        whenDefined_.erase(pending);
    }
    return Value::undefined();
}

void CustomElements::install(Object* window, Object* htmlElementProto) {
    // HTMLElement is constructible by custom element classes
    auto* htmlCtor = new Function("HTMLElement", [this](const FunctionCallInfo& info) -> Value {
        return constructHTMLElement(info);
    }, 0);
    htmlCtor->set("prototype", Value::object(htmlElementProto));
    defineHidden(htmlElementProto, "constructor", Value::object(htmlCtor));
    vm_->setGlobal("HTMLElement", Value::object(htmlCtor));
    defineHidden(window, "HTMLElement", Value::object(htmlCtor));

    // CustomElementRegistry and window.customElements
    auto* proto = new Object();
    proto->setPrototype(vm_->intrinsic(VM::Intrinsic::Object));
    auto* registryCtor = new Function("CustomElementRegistry", [](const FunctionCallInfo&) -> Value {
        throw JSError("TypeError", "Illegal constructor");
    }, 0);
    registryCtor->set("prototype", Value::object(proto));
    defineHidden(proto, "constructor", Value::object(registryCtor));
    vm_->setGlobal("CustomElementRegistry", Value::object(registryCtor));
    defineHidden(window, "CustomElementRegistry", Value::object(registryCtor));

    method(proto, "define", 2, [this](const FunctionCallInfo& info) -> Value {
        CEReactionsScope scope;
        Value result;
        try {
            result = define(info);
        } catch (...) {
            scope.leave();
            throw;
        }
        scope.leave();
        return result;
    });
    method(proto, "get", 1, [this](const FunctionCallInfo& info) -> Value {
        auto definition = definitions_.find(info.argument(0).toString());
        if (definition == definitions_.end()) return Value::undefined();
        return Value::object(definition->second->constructor);
    });
    method(proto, "getName", 1, [this](const FunctionCallInfo& info) -> Value {
        Value ctor = info.argument(0);
        for (const auto& [name, d] : definitions_) {
            if (ctor.isObject() && ctor.asObject() == d->constructor) return str(name);
        }
        return Value::null();
    });
    method(proto, "whenDefined", 1, [this](const FunctionCallInfo& info) -> Value {
        const std::string name = info.argument(0).toString();
        if (!isValidCustomElementName(name)) {
            auto* rejected = new Promise();
            Value error = vm_->newError("Error", "Failed to execute 'whenDefined' on 'CustomElementRegistry': \"" + name + "\" is not a valid custom element name");
            if (error.isObject()) error.asObject()->set("name", str("SyntaxError"));
            rejected->reject(error);
            return Value::object(rejected);
        }
        if (auto definition = lookup(name); definition && definition->name == name) {
            auto* resolved = new Promise();
            resolved->resolve(Value::object(definition->constructor));
            return Value::object(resolved);
        }
        Promise*& pending = whenDefined_[name];
        if (!pending) pending = new Promise();
        return Value::object(pending);
    });
    method(proto, "upgrade", 1, [this](const FunctionCallInfo& info) -> Value {
        Value root = info.argument(0);
        DOMNode* node = root.isObject() ? DOMBindings::nodeOf(root.asObject()) : nullptr;
        if (!node) {
            throw JSError("TypeError", "Failed to execute 'upgrade' on 'CustomElementRegistry': parameter 1 is not of type 'Node'.");
        }
        CEReactionsScope scope;
        upgradeTree(node);
        scope.leave();
        return Value::undefined();
    });

    auto* registry = new Object();
    registry->setPrototype(proto);
    vm_->setGlobal("customElements", Value::object(registry));
    defineHidden(window, "customElements", Value::object(registry));
}

} // namespace Zepra::WebCore
