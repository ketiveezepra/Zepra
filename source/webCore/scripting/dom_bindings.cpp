// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file dom_bindings.cpp
 * @brief Live DOM wrappers and DOM events for page scripts (see dom_bindings.hpp)
 */

#include "scripting/dom_bindings.hpp"
#include "scripting/canvas_bindings.hpp"
#include "scripting/mutation_observer.hpp"
#include "css/computed_style_values.hpp"
#include "css/css_engine.hpp"
#include "css/css_style_declaration.hpp"
#include "html/html_parser.hpp"

#include "runtime/execution/vm.hpp"
#include "runtime/execution/js_error.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace Zepra::WebCore {

using namespace Zepra::Runtime;

// ============================================================================
// Script objects that carry native state
// ============================================================================

struct Listener {
    std::string type;
    Value callback;
    bool capture = false;
    bool once = false;
    bool passive = false;
    bool removed = false;
};

/// window, nodes and `new EventTarget()`: an object with event listeners
class EventTargetObject : public Object {
public:
    std::vector<std::shared_ptr<Listener>> listeners;
};

class NodeWrapper final : public EventTargetObject, public NodeBinding {
public:
    explicit NodeWrapper(DOMNode* n) : node(n) {}
    void nodeDestroyed() override { node = nullptr; }

    DOMNode* node;                    // null once the node is destroyed
    std::unique_ptr<DOMNode> owned;   // a removed subtree (rooted at `node`) kept alive for the script
    Object* style = nullptr;          // made on first use, then the same object
    Object* classList = nullptr;
    Object* dataset = nullptr;
    /// on* attribute source and the function compiled from it
    std::unordered_map<std::string, std::pair<std::string, Value>> inlineHandlers;
};

class EventObject final : public Object {
public:
    std::string type;
    bool bubbles = false;
    bool cancelable = false;
    bool composed = false;
    bool trusted = false;
    bool canceled = false;
    bool stopped = false;
    bool stoppedImmediately = false;
    bool dispatching = false;
    bool inPassiveListener = false;
    int phase = 0;
    Value target = Value::null();
    Value currentTarget = Value::null();
    std::vector<Object*> path;
    double timeStamp = 0;
};

namespace {

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

Value str(const std::string& s) { return Value::string(new String(s)); }

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

void defineHidden(Object* obj, const std::string& name, Value value) {
    PropertyDescriptor desc;
    desc.value = value;
    desc.attributes = PropertyAttribute::Writable | PropertyAttribute::Configurable;
    obj->defineProperty(name, desc);
}

void method(Object* obj, const std::string& name, size_t arity, BuiltinFn fn) {
    defineHidden(obj, name, Value::object(new Function(name, std::move(fn), arity)));
}

void accessor(Object* obj, const std::string& name, BuiltinFn getter, BuiltinFn setter = nullptr) {
    PropertyDescriptor desc;
    desc.getter = Value::object(new Function("get " + name, std::move(getter), 0));
    if (setter) desc.setter = Value::object(new Function("set " + name, std::move(setter), 1));
    desc.attributes = PropertyAttribute::Configurable | PropertyAttribute::Enumerable;
    obj->defineProperty(name, desc);
}

void constant(Object* obj, const std::string& name, double value) {
    PropertyDescriptor desc;
    desc.value = Value::number(value);
    desc.attributes = PropertyAttribute::Enumerable;
    obj->defineProperty(name, desc);
}

std::string argString(const FunctionCallInfo& info, size_t index) {
    return info.argument(index).toString();
}

bool isFunction(const Value& v) { return v.isObject() && v.asObject()->isFunction(); }

/// Throws a DOMException-like error: an Error whose name is `name`
[[noreturn]] void throwDOMError(VM* vm, const std::string& name, const std::string& message) {
    Value error = vm->newError("Error", message);
    if (error.isObject()) error.asObject()->set("name", str(name));
    throw ThrownException(error);
}

[[noreturn]] void illegalInvocation(const char* name) {
    throw JSError("TypeError", std::string("Illegal invocation: '") + name + "' called on an object that does not implement it");
}

NodeWrapper* thisWrapper(const FunctionCallInfo& info, const char* name) {
    Value t = info.thisValue();
    auto* w = t.isObject() ? dynamic_cast<NodeWrapper*>(t.asObject()) : nullptr;
    if (!w) illegalInvocation(name);
    return w;
}

/// A method or accessor of nodes of type T: `this` must wrap such a node.
/// A node that was destroyed (its page navigated away) answers undefined.
template <typename T, typename F>
BuiltinFn on(const char* name, F fn) {
    return [name, fn](const FunctionCallInfo& info) -> Value {
        NodeWrapper* w = thisWrapper(info, name);
        if (!w->node) return Value::undefined();
        T* node = dynamic_cast<T*>(w->node);
        if (!node) illegalInvocation(name);
        return fn(*node, info);
    };
}

EventObject* thisEvent(const FunctionCallInfo& info, const char* name) {
    Value t = info.thisValue();
    auto* e = t.isObject() ? dynamic_cast<EventObject*>(t.asObject()) : nullptr;
    if (!e) illegalInvocation(name);
    return e;
}

bool isConnectedTo(const DOMNode* node, const DOMDocument* document) {
    const DOMNode* root = node;
    while (root->parentNode()) root = root->parentNode();
    return document && root == document;
}

DOMElement* asElement(DOMNode* node) {
    return node && node->nodeType() == NodeType::Element ? static_cast<DOMElement*>(node) : nullptr;
}

DOMElement* firstElementChild(const DOMNode& node) {
    for (const auto& c : node.childNodes()) if (auto* e = asElement(c.get())) return e;
    return nullptr;
}

DOMElement* lastElementChild(const DOMNode& node) {
    for (auto it = node.childNodes().rbegin(); it != node.childNodes().rend(); ++it) {
        if (auto* e = asElement(it->get())) return e;
    }
    return nullptr;
}

std::vector<DOMElement*> elementChildren(const DOMNode& node) {
    std::vector<DOMElement*> out;
    for (const auto& c : node.childNodes()) if (auto* e = asElement(c.get())) out.push_back(e);
    return out;
}

void collectByTag(const DOMNode& node, const std::string& tag, std::vector<DOMElement*>& out) {
    for (const auto& c : node.childNodes()) {
        if (auto* e = asElement(c.get())) {
            if (tag == "*" || lower(e->tagName()) == tag) out.push_back(e);
            collectByTag(*e, tag, out);
        }
    }
}

std::string textOf(const DOMNode& node) {
    if (node.nodeType() == NodeType::Text || node.nodeType() == NodeType::Comment) return node.nodeValue();
    std::string out;
    for (const auto& c : node.childNodes()) {
        if (c->nodeType() == NodeType::Text) out += c->nodeValue();
        else if (c->nodeType() == NodeType::Element || c->nodeType() == NodeType::DocumentFragment) out += textOf(*c);
    }
    return out;
}

std::vector<std::string> splitTokens(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
        size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) i++;
        if (i > start) {
            std::string token = s.substr(start, i - start);
            if (std::find(out.begin(), out.end(), token) == out.end()) out.push_back(token);
        }
    }
    return out;
}

std::string joinTokens(const std::vector<std::string>& tokens) {
    std::string out;
    for (const auto& t : tokens) {
        if (!out.empty()) out += ' ';
        out += t;
    }
    return out;
}

// fooBar -> foo-bar; a leading webkit/moz/ms vendor prefix gets its dash
std::string camelToKebab(const std::string& name, bool vendorPrefixes) {
    std::string out;
    for (char c : name) {
        if (std::isupper(static_cast<unsigned char>(c))) {
            out += '-';
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            out += c;
        }
    }
    if (vendorPrefixes) {
        for (const char* prefix : {"webkit-", "moz-", "ms-", "o-"}) {
            if (out.rfind(prefix, 0) == 0) return "-" + out;
        }
    }
    return out;
}

std::string kebabToCamel(const std::string& name) {
    std::string out;
    bool up = false;
    for (char c : name) {
        if (c == '-') { up = true; continue; }
        out += up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        up = false;
    }
    return out;
}

// element.style property name to a CSS property name
std::string cssPropertyName(const std::string& key) {
    if (key == "cssFloat") return "float";
    if (key.rfind("--", 0) == 0) return key;  // custom properties keep their case
    if (key.find('-') != std::string::npos) return lower(key);
    return camelToKebab(key, true);
}

bool looksLikeCssProperty(const std::string& key) {
    if (key.empty() || key.rfind("@@", 0) == 0) return false;
    for (char c : key) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') return false;
    }
    return true;
}

std::unique_ptr<CSSStyleDeclaration> parseInlineStyle(const DOMElement& element) {
    CSSParser parser;
    return parser.parseInlineStyle(element.getAttribute("style"));
}

void writeInlineStyle(DOMElement& element, const CSSStyleDeclaration& decl) {
    std::string text = decl.cssText();
    while (!text.empty() && text.back() == ' ') text.pop_back();
    if (text.empty()) {
        element.removeAttribute("style");
    } else if (text != element.getAttribute("style")) {
        element.setAttribute("style", text);
    }
}

// ----------------------------------------------------------------------------
// element.style: reads and writes the style attribute
// ----------------------------------------------------------------------------

class StyleObject final : public Object {
public:
    StyleObject(NodeWrapper* owner, Object* methods) : Object(ObjectType::Proxy), owner_(owner), methods_(methods) {}

    using Object::get;
    using Object::set;
    using Object::has;
    using Object::deleteProperty;

    DOMElement* element() const { return asElement(owner_->node); }

    Value get(const std::string& key) const override {
        Value m = methods_->get(key);
        if (!m.isUndefined()) return m;
        DOMElement* el = element();
        if (key == "cssText") return str(el ? el->getAttribute("style") : "");
        if (key == "length") return Value::number(el ? static_cast<double>(parseInlineStyle(*el)->length()) : 0);
        if (key == "parentRule") return Value::null();
        if (!looksLikeCssProperty(key)) return Object::get(key);
        return str(el ? parseInlineStyle(*el)->getPropertyValue(cssPropertyName(key)) : "");
    }

    Value get(uint32_t index) const override {
        DOMElement* el = element();
        if (!el) return Value::undefined();
        auto decl = parseInlineStyle(*el);
        return index < decl->length() ? str(decl->item(index)) : Value::undefined();
    }

    bool set(const std::string& key, Value value) override {
        DOMElement* el = element();
        if (!el) return true;
        if (key == "cssText") {
            std::string text = value.isNullOrUndefined() ? "" : value.toString();
            if (text.empty()) el->removeAttribute("style");
            else el->setAttribute("style", text);
            return true;
        }
        if (!looksLikeCssProperty(key)) return Object::set(key, value);
        auto decl = parseInlineStyle(*el);
        std::string text = value.isNull() ? "" : value.toString();
        if (text.empty()) decl->removeProperty(cssPropertyName(key));
        else decl->setProperty(cssPropertyName(key), text, "");
        writeInlineStyle(*el, *decl);
        return true;
    }

    bool has(const std::string& key) const override {
        return methods_->has(key) || looksLikeCssProperty(key) || Object::has(key);
    }

    bool deleteProperty(const std::string& key) override {
        if (DOMElement* el = element(); el && looksLikeCssProperty(key)) {
            auto decl = parseInlineStyle(*el);
            decl->removeProperty(cssPropertyName(key));
            writeInlineStyle(*el, *decl);
            return true;
        }
        return Object::deleteProperty(key);
    }

private:
    NodeWrapper* owner_;
    Object* methods_;
};

StyleObject* thisStyle(const FunctionCallInfo& info, const char* name) {
    Value t = info.thisValue();
    auto* s = t.isObject() ? dynamic_cast<StyleObject*>(t.asObject()) : nullptr;
    if (!s) illegalInvocation(name);
    return s;
}

// ----------------------------------------------------------------------------
// getComputedStyle(): an element's resolved style, read-only and live
// ----------------------------------------------------------------------------

/// "0", "12", ...: an array index (no sign, no leading zeros)
bool arrayIndex(const std::string& key, uint32_t& index) {
    if (key.empty() || key.size() > 9 || (key.size() > 1 && key[0] == '0')) return false;
    uint32_t n = 0;
    for (char c : key) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + static_cast<uint32_t>(c - '0');
    }
    index = n;
    return true;
}

/// A property getComputedStyle() can answer (by camelCase or CSS name)
bool isComputedProperty(const std::string& key) {
    static const std::unordered_set<std::string> names(computedStylePropertyNames().begin(),
                                                       computedStylePropertyNames().end());
    return looksLikeCssProperty(key) && names.count(cssPropertyName(key)) > 0;
}

[[noreturn]] void throwComputedReadOnly(VM* vm, const std::string& what, const std::string& property) {
    throwDOMError(vm, "NoModificationAllowedError", what + " on 'CSSStyleDeclaration': These styles are computed, "
                  "and therefore the '" + property + "' property is read-only.");
}

class ComputedStyleObject final : public Object {
public:
    ComputedStyleObject(VM* vm, const DOMBindings* bindings, NodeWrapper* owner, bool pseudoElement, Object* methods)
        : Object(ObjectType::Proxy), vm_(vm), bindings_(bindings), owner_(owner),
          pseudoElement_(pseudoElement), methods_(methods) {}

    using Object::get;
    using Object::set;
    using Object::has;

    /// Read again on every access, so the object follows style changes.
    /// Pseudo-elements have no computed style here: they read as empty.
    std::vector<std::pair<std::string, std::string>> values() const {
        DOMElement* el = asElement(owner_->node);
        if (!el || pseudoElement_) return {};
        return bindings_->computedValues(el);
    }

    std::string value(const std::string& property) const {
        for (const auto& [name, v] : values()) {
            if (name == property) return v;
        }
        return "";
    }

    Value get(const std::string& key) const override {
        Value m = methods_->get(key);
        if (!m.isUndefined()) return m;
        uint32_t index;
        if (arrayIndex(key, index)) return get(index);   // the VM passes cs[0] by name
        if (key == "cssText") return str("");   // as browsers give it for computed styles
        if (key == "length") return Value::number(static_cast<double>(values().size()));
        if (key == "parentRule") return Value::null();
        if (isComputedProperty(key)) return str(value(cssPropertyName(key)));
        return Object::get(key);
    }

    Value get(uint32_t index) const override {
        auto v = values();
        return index < v.size() ? str(v[index].first) : Value::undefined();
    }

    bool set(const std::string& key, Value value) override {
        if (key == "cssText") {
            throwDOMError(vm_, "NoModificationAllowedError", "Failed to set the 'cssText' property on "
                          "'CSSStyleDeclaration': These styles are computed, and therefore can't be modified.");
        }
        if (isComputedProperty(key)) throwComputedReadOnly(vm_, "Failed to set the '" + key + "' property", cssPropertyName(key));
        return Object::set(key, value);
    }

    bool has(const std::string& key) const override {
        uint32_t index;
        if (arrayIndex(key, index)) return has(index);
        return methods_->has(key) || key == "cssText" || key == "length" || key == "parentRule" ||
               isComputedProperty(key) || Object::has(key);
    }

    bool has(uint32_t index) const override { return index < values().size(); }

private:
    VM* vm_;
    const DOMBindings* bindings_;
    NodeWrapper* owner_;
    bool pseudoElement_;
    Object* methods_;
};

ComputedStyleObject* thisComputedStyle(const FunctionCallInfo& info, const char* name) {
    Value t = info.thisValue();
    auto* s = t.isObject() ? dynamic_cast<ComputedStyleObject*>(t.asObject()) : nullptr;
    if (!s) illegalInvocation(name);
    return s;
}

// ----------------------------------------------------------------------------
// element.dataset: the data-* attributes by camelCase name
// ----------------------------------------------------------------------------

class DatasetObject final : public Object {
public:
    explicit DatasetObject(NodeWrapper* owner) : Object(ObjectType::Proxy), owner_(owner) {}

    using Object::get;
    using Object::set;
    using Object::has;
    using Object::deleteProperty;

    Value get(const std::string& key) const override {
        DOMElement* el = asElement(owner_->node);
        std::string attr = "data-" + camelToKebab(key, false);
        if (el && looksLikeCssProperty(key) && el->hasAttribute(attr)) return str(el->getAttribute(attr));
        return Object::get(key);
    }
    bool set(const std::string& key, Value value) override {
        if (DOMElement* el = asElement(owner_->node); el && looksLikeCssProperty(key)) {
            el->setAttribute("data-" + camelToKebab(key, false), value.toString());
            return true;
        }
        return Object::set(key, value);
    }
    bool has(const std::string& key) const override {
        DOMElement* el = asElement(owner_->node);
        return (el && el->hasAttribute("data-" + camelToKebab(key, false))) || Object::has(key);
    }
    bool deleteProperty(const std::string& key) override {
        if (DOMElement* el = asElement(owner_->node)) el->removeAttribute("data-" + camelToKebab(key, false));
        return true;
    }

private:
    NodeWrapper* owner_;
};

// ----------------------------------------------------------------------------
// element.classList: a live view of the class attribute
// ----------------------------------------------------------------------------

class TokenListObject final : public Object {
public:
    explicit TokenListObject(NodeWrapper* owner) : owner(owner) {}

    DOMElement* element() const { return asElement(owner->node); }
    std::vector<std::string> tokens() const {
        DOMElement* el = element();
        return el ? splitTokens(el->getAttribute("class")) : std::vector<std::string>{};
    }
    void write(const std::vector<std::string>& tokens) {
        DOMElement* el = element();
        if (!el) return;
        std::string value = joinTokens(tokens);
        if (value != el->getAttribute("class") || !el->hasAttribute("class")) el->setAttribute("class", value);
    }

    NodeWrapper* owner;
};

TokenListObject* thisTokenList(const FunctionCallInfo& info, const char* name) {
    Value t = info.thisValue();
    auto* l = t.isObject() ? dynamic_cast<TokenListObject*>(t.asObject()) : nullptr;
    if (!l) illegalInvocation(name);
    return l;
}

void checkToken(const std::string& token, const char* method) {
    if (token.empty()) {
        throw JSError("SyntaxError", std::string("Failed to execute '") + method +
                      "' on 'DOMTokenList': The token provided must not be empty.");
    }
    for (char c : token) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            throw JSError("SyntaxError", std::string("Failed to execute '") + method + "' on 'DOMTokenList': The token provided ('" +
                          token + "') contains HTML space characters, which are not valid in tokens.");
        }
    }
}

double nowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Interfaces of the HTML elements pages most often test with instanceof
struct TagInterface { const char* tag; const char* name; };
constexpr TagInterface kTagInterfaces[] = {
    {"a", "HTMLAnchorElement"}, {"area", "HTMLAreaElement"}, {"audio", "HTMLAudioElement"},
    {"base", "HTMLBaseElement"}, {"blockquote", "HTMLQuoteElement"}, {"body", "HTMLBodyElement"},
    {"br", "HTMLBRElement"}, {"button", "HTMLButtonElement"}, {"canvas", "HTMLCanvasElement"},
    {"data", "HTMLDataElement"}, {"datalist", "HTMLDataListElement"}, {"details", "HTMLDetailsElement"},
    {"dialog", "HTMLDialogElement"}, {"div", "HTMLDivElement"}, {"dl", "HTMLDListElement"},
    {"embed", "HTMLEmbedElement"}, {"fieldset", "HTMLFieldSetElement"}, {"form", "HTMLFormElement"},
    {"h1", "HTMLHeadingElement"}, {"h2", "HTMLHeadingElement"}, {"h3", "HTMLHeadingElement"},
    {"h4", "HTMLHeadingElement"}, {"h5", "HTMLHeadingElement"}, {"h6", "HTMLHeadingElement"},
    {"head", "HTMLHeadElement"}, {"hr", "HTMLHRElement"}, {"html", "HTMLHtmlElement"},
    {"iframe", "HTMLIFrameElement"}, {"img", "HTMLImageElement"}, {"input", "HTMLInputElement"},
    {"label", "HTMLLabelElement"}, {"legend", "HTMLLegendElement"}, {"li", "HTMLLIElement"},
    {"link", "HTMLLinkElement"}, {"map", "HTMLMapElement"}, {"meta", "HTMLMetaElement"},
    {"meter", "HTMLMeterElement"}, {"object", "HTMLObjectElement"}, {"ol", "HTMLOListElement"},
    {"optgroup", "HTMLOptGroupElement"}, {"option", "HTMLOptionElement"}, {"output", "HTMLOutputElement"},
    {"p", "HTMLParagraphElement"}, {"picture", "HTMLPictureElement"}, {"pre", "HTMLPreElement"},
    {"progress", "HTMLProgressElement"}, {"q", "HTMLQuoteElement"}, {"script", "HTMLScriptElement"},
    {"select", "HTMLSelectElement"}, {"slot", "HTMLSlotElement"}, {"source", "HTMLSourceElement"},
    {"span", "HTMLSpanElement"}, {"style", "HTMLStyleElement"}, {"table", "HTMLTableElement"},
    {"tbody", "HTMLTableSectionElement"}, {"td", "HTMLTableCellElement"}, {"template", "HTMLTemplateElement"},
    {"textarea", "HTMLTextAreaElement"}, {"tfoot", "HTMLTableSectionElement"}, {"th", "HTMLTableCellElement"},
    {"thead", "HTMLTableSectionElement"}, {"time", "HTMLTimeElement"}, {"title", "HTMLTitleElement"},
    {"tr", "HTMLTableRowElement"}, {"track", "HTMLTrackElement"}, {"ul", "HTMLUListElement"},
    {"video", "HTMLVideoElement"},
};

}  // namespace

// ============================================================================
// Setup
// ============================================================================

DOMBindings::DOMBindings(VM* vm, ErrorReporter reportError) : vm_(vm), reportError_(std::move(reportError)) {
    installEventTarget();
    // The window is an event target of its own kind
    Object* windowProto = makeInterface("Window", eventTargetProto_);
    auto* window = new EventTargetObject();
    window->setPrototype(windowProto);
    global_ = window;
    // makeInterface put Window and EventTarget on the global object only
    for (const char* name : {"EventTarget", "Window"}) defineHidden(global_, name, vm_->getGlobal(name));
    mutationObservers_ = std::make_unique<MutationObservers>(vm_, *this, [this](const std::exception& ex) {
        reportException(ex);
    });
    mutationObservers_->install(global_);

    installNode();
    installCharacterData();
    installElement();
    installGeometry();
    installDocumentInterface();
    installEvents();
    installComputedStyle();
    installForms();

    // Bare addEventListener(...) and friends act on the window
    // (the engine's global object is not the window yet)
    for (const char* name : {"addEventListener", "removeEventListener", "dispatchEvent"}) {
        Value fn = eventTargetProto_->get(name);
        auto* target = static_cast<Function*>(fn.asObject());
        vm_->setGlobal(name, Value::object(new Function(name, [this, target](const FunctionCallInfo& info) -> Value {
            return vm_->call(target, Value::object(global_), info.arguments());
        }, target->arity())));
    }
}

DOMBindings::~DOMBindings() {
    // No more records once the page's scripts are gone
    mutationObservers_.reset();
    // Nodes that outlive the page's script objects must not point at them
    for (NodeWrapper* w : wrappers_) {
        if (w->node) w->node->setBinding(nullptr);
        w->node = nullptr;
    }
    for (NodeWrapper* w : wrappers_) w->owned.reset();
    releaseDetachedNodes();
}

Object* DOMBindings::window() const { return global_; }

Object* DOMBindings::makeInterface(const std::string& name, Object* parentProto, Object* existingProto) {
    Object* proto = existingProto ? existingProto : new Object();
    proto->setPrototype(parentProto ? parentProto : vm_->intrinsic(VM::Intrinsic::Object));
    auto* ctor = new Function(name, [name](const FunctionCallInfo&) -> Value {
        throw JSError("TypeError", "Illegal constructor: " + name);
    }, 0);
    ctor->set("prototype", Value::object(proto));
    defineHidden(proto, "constructor", Value::object(ctor));
    vm_->setGlobal(name, Value::object(ctor));
    if (global_) defineHidden(global_, name, Value::object(ctor));
    return proto;
}

// ----------------------------------------------------------------------------
// EventTarget
// ----------------------------------------------------------------------------

void DOMBindings::installEventTarget() {
    eventTargetProto_ = makeInterface("EventTarget", nullptr);
    // new EventTarget() works (libraries use it as an event emitter)
    Object* proto = eventTargetProto_;
    auto* ctor = new Function("EventTarget", [proto](const FunctionCallInfo&) -> Value {
        auto* target = new EventTargetObject();
        target->setPrototype(proto);
        return Value::object(target);
    }, 0);
    ctor->set("prototype", Value::object(proto));
    defineHidden(proto, "constructor", Value::object(ctor));
    vm_->setGlobal("EventTarget", Value::object(ctor));

    auto thisTarget = [](const FunctionCallInfo& info, const char* name) {
        Value t = info.thisValue();
        auto* target = t.isObject() ? dynamic_cast<EventTargetObject*>(t.asObject()) : nullptr;
        if (!target) illegalInvocation(name);
        return target;
    };
    // options: a boolean (capture) or { capture, once, passive }
    auto readOptions = [this](const Value& options, Listener& l) {
        if (options.isObject()) {
            l.capture = vm_->getMember(options, "capture").toBoolean();
            l.once = vm_->getMember(options, "once").toBoolean();
            l.passive = vm_->getMember(options, "passive").toBoolean();
        } else {
            l.capture = options.toBoolean();
        }
    };
    auto sameCallback = [](const Value& a, const Value& b) {
        return a.isObject() && b.isObject() && a.asObject() == b.asObject();
    };

    method(eventTargetProto_, "addEventListener", 2, [thisTarget, readOptions, sameCallback](const FunctionCallInfo& info) -> Value {
        EventTargetObject* target = thisTarget(info, "addEventListener");
        Value callback = info.argument(1);
        if (!callback.isObject()) return Value::undefined();
        auto l = std::make_shared<Listener>();
        l->type = argString(info, 0);
        l->callback = callback;
        readOptions(info.argument(2), *l);
        for (const auto& existing : target->listeners) {
            if (existing->type == l->type && existing->capture == l->capture && sameCallback(existing->callback, callback)) {
                return Value::undefined();
            }
        }
        target->listeners.push_back(std::move(l));
        return Value::undefined();
    });
    method(eventTargetProto_, "removeEventListener", 2, [thisTarget, readOptions, sameCallback](const FunctionCallInfo& info) -> Value {
        EventTargetObject* target = thisTarget(info, "removeEventListener");
        Listener match;
        readOptions(info.argument(2), match);
        const std::string type = argString(info, 0);
        auto& list = target->listeners;
        for (auto it = list.begin(); it != list.end(); ++it) {
            if ((*it)->type == type && (*it)->capture == match.capture && sameCallback((*it)->callback, info.argument(1))) {
                (*it)->removed = true;
                list.erase(it);
                break;
            }
        }
        return Value::undefined();
    });
    method(eventTargetProto_, "dispatchEvent", 1, [this, thisTarget](const FunctionCallInfo& info) -> Value {
        EventTargetObject* target = thisTarget(info, "dispatchEvent");
        Value e = info.argument(0);
        auto* event = e.isObject() ? dynamic_cast<EventObject*>(e.asObject()) : nullptr;
        if (!event) {
            throw JSError("TypeError", "Failed to execute 'dispatchEvent' on 'EventTarget': parameter 1 is not of type 'Event'.");
        }
        event->trusted = false;
        return Value::boolean(dispatch(target, event));
    });
}

// ----------------------------------------------------------------------------
// Node
// ----------------------------------------------------------------------------

void DOMBindings::installNode() {
    nodeProto_ = makeInterface("Node", eventTargetProto_);
    Object* nodeCtor = nodeProto_->get("constructor").asObject();
    const std::pair<const char*, double> kConstants[] = {
        {"ELEMENT_NODE", 1}, {"ATTRIBUTE_NODE", 2}, {"TEXT_NODE", 3}, {"CDATA_SECTION_NODE", 4},
        {"PROCESSING_INSTRUCTION_NODE", 7}, {"COMMENT_NODE", 8}, {"DOCUMENT_NODE", 9},
        {"DOCUMENT_TYPE_NODE", 10}, {"DOCUMENT_FRAGMENT_NODE", 11},
        {"DOCUMENT_POSITION_DISCONNECTED", 1}, {"DOCUMENT_POSITION_PRECEDING", 2},
        {"DOCUMENT_POSITION_FOLLOWING", 4}, {"DOCUMENT_POSITION_CONTAINS", 8},
        {"DOCUMENT_POSITION_CONTAINED_BY", 16},
    };
    for (const auto& [name, value] : kConstants) {
        constant(nodeCtor, name, value);
        constant(nodeProto_, name, value);
    }

    // item(i) for the arrays that stand in for NodeList and HTMLCollection
    listItemFn_ = new Function("item", [](const FunctionCallInfo& info) -> Value {
        Value t = info.thisValue();
        if (!t.isObject()) return Value::null();
        double i = info.argument(0).toNumber();
        if (!(i >= 0) || i >= static_cast<double>(t.asObject()->length())) return Value::null();
        return t.asObject()->get(static_cast<uint32_t>(i));
    }, 1);

    Object* p = nodeProto_;
    accessor(p, "nodeType", on<DOMNode>("nodeType", [](DOMNode& n, const Info&) {
        return Value::number(static_cast<double>(n.nodeType()));
    }));
    accessor(p, "nodeName", on<DOMNode>("nodeName", [](DOMNode& n, const Info&) {
        return str(n.nodeType() == NodeType::Element ? upper(n.nodeName()) : n.nodeName());
    }));
    accessor(p, "nodeValue",
        on<DOMNode>("nodeValue", [](DOMNode& n, const Info&) {
            bool data = n.nodeType() == NodeType::Text || n.nodeType() == NodeType::Comment;
            return data ? str(n.nodeValue()) : Value::null();
        }),
        on<DOMNode>("nodeValue", [](DOMNode& n, const Info& info) {
            Value v = info.argument(0);
            if (n.nodeType() == NodeType::Text || n.nodeType() == NodeType::Comment) {
                n.setNodeValue(v.isNull() ? "" : v.toString());
            }
            return Value::undefined();
        }));
    accessor(p, "textContent",
        on<DOMNode>("textContent", [](DOMNode& n, const Info&) {
            return n.nodeType() == NodeType::Document ? Value::null() : str(textOf(n));
        }),
        on<DOMNode>("textContent", [this](DOMNode& n, const Info& info) {
            Value v = info.argument(0);
            setText(&n, v.isNullOrUndefined() ? "" : v.toString());
            return Value::undefined();
        }));
    accessor(p, "parentNode", on<DOMNode>("parentNode", [this](DOMNode& n, const Info&) { return wrap(n.parentNode()); }));
    accessor(p, "parentElement", on<DOMNode>("parentElement", [this](DOMNode& n, const Info&) {
        return wrap(asElement(n.parentNode()));
    }));
    accessor(p, "childNodes", on<DOMNode>("childNodes", [this](DOMNode& n, const Info&) {
        std::vector<DOMNode*> nodes;
        for (const auto& c : n.childNodes()) nodes.push_back(c.get());
        return nodeList(nodes);
    }));
    accessor(p, "firstChild", on<DOMNode>("firstChild", [this](DOMNode& n, const Info&) { return wrap(n.firstChild()); }));
    accessor(p, "lastChild", on<DOMNode>("lastChild", [this](DOMNode& n, const Info&) { return wrap(n.lastChild()); }));
    accessor(p, "previousSibling", on<DOMNode>("previousSibling", [this](DOMNode& n, const Info&) { return wrap(n.previousSibling()); }));
    accessor(p, "nextSibling", on<DOMNode>("nextSibling", [this](DOMNode& n, const Info&) { return wrap(n.nextSibling()); }));
    accessor(p, "ownerDocument", on<DOMNode>("ownerDocument", [this](DOMNode& n, const Info&) {
        return n.nodeType() == NodeType::Document ? Value::null() : wrap(n.ownerDocument() ? n.ownerDocument() : document_);
    }));
    accessor(p, "isConnected", on<DOMNode>("isConnected", [this](DOMNode& n, const Info&) {
        return Value::boolean(isConnectedTo(&n, document_));
    }));

    method(p, "hasChildNodes", 0, on<DOMNode>("hasChildNodes", [](DOMNode& n, const Info&) {
        return Value::boolean(n.hasChildNodes());
    }));
    method(p, "contains", 1, on<DOMNode>("contains", [this](DOMNode& n, const Info& info) {
        DOMNode* other = nodeArg(info, 0, "contains", true);
        for (DOMNode* o = other; o; o = o->parentNode()) if (o == &n) return Value::boolean(true);
        return Value::boolean(false);
    }));
    method(p, "isSameNode", 1, on<DOMNode>("isSameNode", [this](DOMNode& n, const Info& info) {
        return Value::boolean(nodeArg(info, 0, "isSameNode", true) == &n);
    }));
    method(p, "getRootNode", 0, on<DOMNode>("getRootNode", [this](DOMNode& n, const Info&) {
        DOMNode* root = &n;
        while (root->parentNode()) root = root->parentNode();
        return wrap(root);
    }));
    method(p, "appendChild", 1, on<DOMNode>("appendChild", [this](DOMNode& n, const Info& info) {
        DOMNode* child = nodeArg(info, 0, "appendChild", false);
        insert(&n, child, nullptr, "appendChild");
        return info.argument(0);
    }));
    method(p, "insertBefore", 2, on<DOMNode>("insertBefore", [this](DOMNode& n, const Info& info) {
        DOMNode* child = nodeArg(info, 0, "insertBefore", false);
        DOMNode* before = nodeArg(info, 1, "insertBefore", true);
        insert(&n, child, before, "insertBefore");
        return info.argument(0);
    }));
    method(p, "removeChild", 1, on<DOMNode>("removeChild", [this](DOMNode& n, const Info& info) {
        DOMNode* child = nodeArg(info, 0, "removeChild", false);
        if (child->parentNode() != &n) {
            throwDOMException("NotFoundError", "Failed to execute 'removeChild' on 'Node': The node to be removed is not a child of this node.");
        }
        wrapperOf(child)->owned = n.removeChild(child);
        return info.argument(0);
    }));
    method(p, "replaceChild", 2, on<DOMNode>("replaceChild", [this](DOMNode& n, const Info& info) {
        DOMNode* replacement = nodeArg(info, 0, "replaceChild", false);
        DOMNode* old = nodeArg(info, 1, "replaceChild", false);
        if (old->parentNode() != &n) {
            throwDOMException("NotFoundError", "Failed to execute 'replaceChild' on 'Node': The node to be replaced is not a child of this node.");
        }
        if (replacement != old) {
            insert(&n, replacement, old, "replaceChild");
            wrapperOf(old)->owned = n.removeChild(old);
        }
        return info.argument(1);
    }));
    method(p, "cloneNode", 1, on<DOMNode>("cloneNode", [this](DOMNode& n, const Info& info) {
        if (n.nodeType() == NodeType::Document) {
            throwDOMException("NotSupportedError", "Failed to execute 'cloneNode' on 'Node': Cloning a document is not supported.");
        }
        std::unique_ptr<DOMNode> clone = n.cloneNode(info.argument(0).toBoolean());
        DOMNode* raw = clone.get();
        NodeWrapper* w = wrapperOf(raw);
        w->owned = std::move(clone);
        return Value::object(w);
    }));
    method(p, "toString", 0, on<DOMNode>("toString", [this](DOMNode& n, const Info&) {
        return str("[object " + interfaceName(&n) + "]");
    }));

    // ParentNode members, shared by elements, documents and fragments
    auto parentNodeMembers = [this](Object* proto) {
        accessor(proto, "children", on<DOMNode>("children", [this](DOMNode& n, const Info&) {
            return elementList(elementChildren(n));
        }));
        accessor(proto, "childElementCount", on<DOMNode>("childElementCount", [](DOMNode& n, const Info&) {
            return Value::number(static_cast<double>(elementChildren(n).size()));
        }));
        accessor(proto, "firstElementChild", on<DOMNode>("firstElementChild", [this](DOMNode& n, const Info&) {
            return wrap(firstElementChild(n));
        }));
        accessor(proto, "lastElementChild", on<DOMNode>("lastElementChild", [this](DOMNode& n, const Info&) {
            return wrap(lastElementChild(n));
        }));
        method(proto, "querySelector", 1, on<DOMNode>("querySelector", [this](DOMNode& n, const Info& info) {
            auto found = selectDescendants(&n, argString(info, 0), true);
            return found.empty() ? Value::null() : wrap(found.front());
        }));
        method(proto, "querySelectorAll", 1, on<DOMNode>("querySelectorAll", [this](DOMNode& n, const Info& info) {
            return elementList(selectDescendants(&n, argString(info, 0)));
        }));
        method(proto, "getElementsByTagName", 1, on<DOMNode>("getElementsByTagName", [this](DOMNode& n, const Info& info) {
            std::vector<DOMElement*> found;
            collectByTag(n, lower(argString(info, 0)), found);
            return elementList(found);
        }));
        method(proto, "getElementsByClassName", 1, on<DOMNode>("getElementsByClassName", [this](DOMNode& n, const Info& info) {
            return elementList(descendantsWithClasses(&n, argString(info, 0)));
        }));
        method(proto, "append", 0, on<DOMNode>("append", [this](DOMNode& n, const Info& info) {
            insertNodes(&n, info, nullptr, "append");
            return Value::undefined();
        }));
        method(proto, "prepend", 0, on<DOMNode>("prepend", [this](DOMNode& n, const Info& info) {
            insertNodes(&n, info, n.firstChild(), "prepend");
            return Value::undefined();
        }));
        method(proto, "replaceChildren", 0, on<DOMNode>("replaceChildren", [this](DOMNode& n, const Info& info) {
            removeAllChildren(&n);
            insertNodes(&n, info, nullptr, "replaceChildren");
            return Value::undefined();
        }));
    };
    elementProto_ = makeInterface("Element", nodeProto_);
    documentProto_ = makeInterface("Document", nodeProto_);
    fragmentProto_ = makeInterface("DocumentFragment", nodeProto_);
    parentNodeMembers(elementProto_);
    parentNodeMembers(documentProto_);
    parentNodeMembers(fragmentProto_);
}

// ----------------------------------------------------------------------------
// CharacterData, Text, Comment
// ----------------------------------------------------------------------------

void DOMBindings::installCharacterData() {
    characterDataProto_ = makeInterface("CharacterData", nodeProto_);
    textProto_ = makeInterface("Text", characterDataProto_);
    commentProto_ = makeInterface("Comment", characterDataProto_);
    Object* p = characterDataProto_;
    accessor(p, "data",
        on<DOMNode>("data", [](DOMNode& n, const Info&) { return str(n.nodeValue()); }),
        on<DOMNode>("data", [](DOMNode& n, const Info& info) {
            Value v = info.argument(0);
            n.setNodeValue(v.isNull() ? "" : v.toString());
            return Value::undefined();
        }));
    accessor(p, "length", on<DOMNode>("length", [](DOMNode& n, const Info&) {
        return Value::number(static_cast<double>(n.nodeValue().size()));
    }));
    method(p, "appendData", 1, on<DOMNode>("appendData", [](DOMNode& n, const Info& info) {
        n.setNodeValue(n.nodeValue() + argString(info, 0));
        return Value::undefined();
    }));
    // ChildNode members of text and comments
    method(p, "remove", 0, on<DOMNode>("remove", [this](DOMNode& n, const Info&) {
        if (DOMNode* parent = n.parentNode()) wrapperOf(&n)->owned = parent->removeChild(&n);
        return Value::undefined();
    }));
    accessor(p, "previousElementSibling", on<DOMNode>("previousElementSibling", [this](DOMNode& n, const Info&) {
        DOMNode* s = n.previousSibling();
        while (s && !asElement(s)) s = s->previousSibling();
        return wrap(s);
    }));
    accessor(p, "nextElementSibling", on<DOMNode>("nextElementSibling", [this](DOMNode& n, const Info&) {
        DOMNode* s = n.nextSibling();
        while (s && !asElement(s)) s = s->nextSibling();
        return wrap(s);
    }));
    accessor(textProto_, "wholeText", on<DOMNode>("wholeText", [](DOMNode& n, const Info&) { return str(n.nodeValue()); }));
}

// ----------------------------------------------------------------------------
// Element and HTMLElement
// ----------------------------------------------------------------------------

void DOMBindings::installElement() {
    Object* p = elementProto_;

    accessor(p, "tagName", on<DOMElement>("tagName", [](DOMElement& e, const Info&) { return str(upper(e.tagName())); }));
    accessor(p, "localName", on<DOMElement>("localName", [](DOMElement& e, const Info&) { return str(lower(e.tagName())); }));
    accessor(p, "namespaceURI", on<DOMElement>("namespaceURI", [](DOMElement&, const Info&) {
        return str("http://www.w3.org/1999/xhtml");
    }));
    // Reflected string attributes: element.id = "x" sets id="x"
    auto reflect = [](Object* proto, const char* property, const char* attribute) {
        std::string attr = attribute;
        accessor(proto, property,
            on<DOMElement>(property, [attr](DOMElement& e, const Info&) { return str(e.getAttribute(attr)); }),
            on<DOMElement>(property, [attr](DOMElement& e, const Info& info) {
                e.setAttribute(attr, info.argument(0).toString());
                return Value::undefined();
            }));
    };
    reflect(p, "id", "id");
    reflect(p, "className", "class");
    reflect(p, "slot", "slot");

    accessor(p, "classList", on<DOMElement>("classList", [this](DOMElement& e, const Info& info) {
        NodeWrapper* w = thisWrapper(info, "classList");
        if (!w->classList) {
            w->classList = new TokenListObject(w);
            w->classList->setPrototype(tokenListProto_);
        }
        (void)e;
        return Value::object(w->classList);
    }), on<DOMElement>("classList", [](DOMElement& e, const Info& info) {
        e.setAttribute("class", info.argument(0).toString());
        return Value::undefined();
    }));
    accessor(p, "attributes", on<DOMElement>("attributes", [this](DOMElement& e, const Info&) {
        auto* list = new Array();
        std::vector<std::pair<std::string, std::string>> attrs(e.attributes().begin(), e.attributes().end());
        std::sort(attrs.begin(), attrs.end());
        for (const auto& [name, value] : attrs) {
            auto* attr = new Object();
            attr->setPrototype(vm_->intrinsic(VM::Intrinsic::Object));
            attr->set("name", str(name));
            attr->set("localName", str(name));
            attr->set("value", str(value));
            list->push(Value::object(attr));
        }
        defineHidden(list, "item", Value::object(listItemFn_));
        return Value::object(list);
    }));
    method(p, "getAttribute", 1, on<DOMElement>("getAttribute", [](DOMElement& e, const Info& info) {
        std::string name = lower(argString(info, 0));
        return e.hasAttribute(name) ? str(e.getAttribute(name)) : Value::null();
    }));
    method(p, "getAttributeNS", 2, on<DOMElement>("getAttributeNS", [](DOMElement& e, const Info& info) {
        std::string name = lower(argString(info, 1));
        return e.hasAttribute(name) ? str(e.getAttribute(name)) : Value::null();
    }));
    method(p, "setAttribute", 2, on<DOMElement>("setAttribute", [this](DOMElement& e, const Info& info) {
        std::string name = lower(argString(info, 0));
        if (name.empty()) {
            throwDOMException("InvalidCharacterError", "Failed to execute 'setAttribute' on 'Element': '' is not a valid attribute name.");
        }
        e.setAttribute(name, argString(info, 1));
        if (canvases_) canvases_->attributeChanged(&e, name);
        return Value::undefined();
    }));
    method(p, "setAttributeNS", 3, on<DOMElement>("setAttributeNS", [this](DOMElement& e, const Info& info) {
        std::string name = lower(argString(info, 1));
        e.setAttribute(name, argString(info, 2));
        if (canvases_) canvases_->attributeChanged(&e, name);
        return Value::undefined();
    }));
    method(p, "removeAttribute", 1, on<DOMElement>("removeAttribute", [](DOMElement& e, const Info& info) {
        e.removeAttribute(lower(argString(info, 0)));
        return Value::undefined();
    }));
    method(p, "hasAttribute", 1, on<DOMElement>("hasAttribute", [](DOMElement& e, const Info& info) {
        return Value::boolean(e.hasAttribute(lower(argString(info, 0))));
    }));
    method(p, "hasAttributes", 0, on<DOMElement>("hasAttributes", [](DOMElement& e, const Info&) {
        return Value::boolean(!e.attributes().empty());
    }));
    method(p, "toggleAttribute", 1, on<DOMElement>("toggleAttribute", [](DOMElement& e, const Info& info) {
        std::string name = lower(argString(info, 0));
        bool present = e.hasAttribute(name);
        bool want = info.argumentCount() > 1 ? info.argument(1).toBoolean() : !present;
        if (want && !present) e.setAttribute(name, "");
        if (!want && present) e.removeAttribute(name);
        return Value::boolean(want);
    }));
    method(p, "getAttributeNames", 0, on<DOMElement>("getAttributeNames", [](DOMElement& e, const Info&) {
        std::vector<std::string> names;
        for (const auto& kv : e.attributes()) names.push_back(kv.first);
        std::sort(names.begin(), names.end());
        auto* list = new Array();
        for (const auto& n : names) list->push(str(n));
        return Value::object(list);
    }));

    accessor(p, "innerHTML",
        on<DOMElement>("innerHTML", [](DOMElement& e, const Info&) { return str(e.innerHTML()); }),
        on<DOMElement>("innerHTML", [this](DOMElement& e, const Info& info) {
            Value v = info.argument(0);
            removeAllChildren(&e);
            for (auto& node : parseFragment(v.isNull() ? "" : v.toString(), &e)) e.appendChild(std::move(node));
            return Value::undefined();
        }));
    accessor(p, "outerHTML",
        on<DOMElement>("outerHTML", [](DOMElement& e, const Info&) { return str(e.outerHTML()); }),
        on<DOMElement>("outerHTML", [this](DOMElement& e, const Info& info) {
            DOMNode* parent = e.parentNode();
            if (!parent) return Value::undefined();
            if (parent->nodeType() == NodeType::Document) {
                throwDOMException("NoModificationAllowedError", "Failed to set the 'outerHTML' property on 'Element': This element's parent is of type '#document'.");
            }
            for (auto& node : parseFragment(argString(info, 0), parent)) parent->insertBefore(std::move(node), &e);
            wrapperOf(&e)->owned = parent->removeChild(&e);
            return Value::undefined();
        }));
    // innerText as textContent (no layout-aware line breaks)
    accessor(p, "innerText",
        on<DOMElement>("innerText", [](DOMElement& e, const Info&) { return str(textOf(e)); }),
        on<DOMElement>("innerText", [this](DOMElement& e, const Info& info) {
            setText(&e, info.argument(0).toString());
            return Value::undefined();
        }));

    // insertAdjacentHTML / Element / Text: where is beforebegin, afterbegin, beforeend or afterend
    auto adjacent = [this](DOMElement& e, const std::string& where, const char* method,
                           const std::function<void(DOMNode* parent, DOMNode* before)>& insertAt) {
        std::string w = lower(where);
        if (w == "afterbegin") insertAt(&e, e.firstChild());
        else if (w == "beforeend") insertAt(&e, nullptr);
        else if (w == "beforebegin" || w == "afterend") {
            DOMNode* parent = e.parentNode();
            if (!parent) return false;
            insertAt(parent, w == "beforebegin" ? static_cast<DOMNode*>(&e) : e.nextSibling());
        } else {
            throwDOMException("SyntaxError", std::string("Failed to execute '") + method + "' on 'Element': The value provided ('" +
                              where + "') is not one of 'beforeBegin', 'afterBegin', 'beforeEnd', or 'afterEnd'.");
        }
        return true;
    };
    method(p, "insertAdjacentHTML", 2, on<DOMElement>("insertAdjacentHTML", [this, adjacent](DOMElement& e, const Info& info) {
        std::string html = argString(info, 1);
        adjacent(e, argString(info, 0), "insertAdjacentHTML", [&](DOMNode* parent, DOMNode* before) {
            for (auto& node : parseFragment(html, parent)) parent->insertBefore(std::move(node), before);
        });
        return Value::undefined();
    }));
    method(p, "insertAdjacentElement", 2, on<DOMElement>("insertAdjacentElement", [this, adjacent](DOMElement& e, const Info& info) {
        DOMNode* node = nodeArg(info, 1, "insertAdjacentElement", false);
        bool done = adjacent(e, argString(info, 0), "insertAdjacentElement", [&](DOMNode* parent, DOMNode* before) {
            insert(parent, node, before, "insertAdjacentElement");
        });
        return done ? info.argument(1) : Value::null();
    }));
    method(p, "insertAdjacentText", 2, on<DOMElement>("insertAdjacentText", [adjacent](DOMElement& e, const Info& info) {
        std::string text = argString(info, 1);
        adjacent(e, argString(info, 0), "insertAdjacentText", [&](DOMNode* parent, DOMNode* before) {
            parent->insertBefore(std::make_unique<DOMText>(text), before);
        });
        return Value::undefined();
    }));

    method(p, "matches", 1, on<DOMElement>("matches", [](DOMElement& e, const Info& info) {
        return Value::boolean(selectorMatches(&e, argString(info, 0)));
    }));
    defineHidden(p, "webkitMatchesSelector", p->get("matches"));
    method(p, "closest", 1, on<DOMElement>("closest", [this](DOMElement& e, const Info& info) {
        std::string selector = argString(info, 0);
        for (DOMNode* n = &e; n; n = n->parentNode()) {
            if (DOMElement* el = asElement(n); el && selectorMatches(el, selector)) return wrap(el);
        }
        return Value::null();
    }));

    // ChildNode members
    accessor(p, "previousElementSibling", on<DOMElement>("previousElementSibling", [this](DOMElement& e, const Info&) {
        DOMNode* s = e.previousSibling();
        while (s && !asElement(s)) s = s->previousSibling();
        return wrap(s);
    }));
    accessor(p, "nextElementSibling", on<DOMElement>("nextElementSibling", [this](DOMElement& e, const Info&) {
        DOMNode* s = e.nextSibling();
        while (s && !asElement(s)) s = s->nextSibling();
        return wrap(s);
    }));
    method(p, "remove", 0, on<DOMElement>("remove", [this](DOMElement& e, const Info&) {
        if (DOMNode* parent = e.parentNode()) wrapperOf(&e)->owned = parent->removeChild(&e);
        return Value::undefined();
    }));
    // Where the arguments go when they replace or follow this node: its first
    // following sibling that is not one of them
    auto viableNextSibling = [this](DOMNode& n, const Info& info) {
        std::unordered_set<DOMNode*> args;
        for (size_t i = 0; i < info.argumentCount(); i++) {
            Value v = info.argument(i);
            if (auto* w = v.isObject() ? dynamic_cast<NodeWrapper*>(v.asObject()) : nullptr) args.insert(w->node);
        }
        DOMNode* next = n.nextSibling();
        while (next && args.count(next)) next = next->nextSibling();
        return next;
    };
    method(p, "before", 0, on<DOMElement>("before", [this](DOMElement& e, const Info& info) {
        if (DOMNode* parent = e.parentNode()) insertNodes(parent, info, &e, "before");
        return Value::undefined();
    }));
    method(p, "after", 0, on<DOMElement>("after", [this, viableNextSibling](DOMElement& e, const Info& info) {
        if (DOMNode* parent = e.parentNode()) insertNodes(parent, info, viableNextSibling(e, info), "after");
        return Value::undefined();
    }));
    method(p, "replaceWith", 0, on<DOMElement>("replaceWith", [this, viableNextSibling](DOMElement& e, const Info& info) {
        DOMNode* parent = e.parentNode();
        if (!parent) return Value::undefined();
        DOMNode* next = viableNextSibling(e, info);
        if (e.parentNode() == parent) wrapperOf(&e)->owned = parent->removeChild(&e);
        insertNodes(parent, info, next, "replaceWith");
        return Value::undefined();
    }));

    // HTMLElement
    htmlElementProto_ = makeInterface("HTMLElement", elementProto_);
    Object* h = htmlElementProto_;
    for (const auto& [property, attribute] : std::initializer_list<std::pair<const char*, const char*>>{
             {"title", "title"}, {"lang", "lang"}, {"dir", "dir"}, {"accessKey", "accesskey"},
             {"href", "href"}, {"src", "src"}, {"alt", "alt"}, {"type", "type"}, {"name", "name"},
             {"placeholder", "placeholder"}, {"rel", "rel"}, {"target", "target"}, {"action", "action"},
             {"method", "method"}, {"htmlFor", "for"}, {"content", "content"}, {"media", "media"},
             {"charset", "charset"}, {"autocomplete", "autocomplete"}, {"pattern", "pattern"},
             {"min", "min"}, {"max", "max"}, {"step", "step"}, {"label", "label"}, {"download", "download"}}) {
        reflect(h, property, attribute);
    }
    // Reflected boolean attributes: present or absent
    for (const auto& [property, attribute] : std::initializer_list<std::pair<const char*, const char*>>{
             {"hidden", "hidden"}, {"disabled", "disabled"}, {"defaultChecked", "checked"}, {"selected", "selected"},
             {"readOnly", "readonly"}, {"required", "required"}, {"multiple", "multiple"},
             {"autofocus", "autofocus"}, {"defer", "defer"}, {"async", "async"}, {"open", "open"},
             {"noValidate", "novalidate"}, {"controls", "controls"}, {"loop", "loop"}, {"muted", "muted"},
             {"autoplay", "autoplay"}, {"inert", "inert"}}) {
        std::string attr = attribute;
        accessor(h, property,
            on<DOMElement>(property, [attr](DOMElement& e, const Info&) { return Value::boolean(e.hasAttribute(attr)); }),
            on<DOMElement>(property, [attr](DOMElement& e, const Info& info) {
                if (info.argument(0).toBoolean()) {
                    if (!e.hasAttribute(attr)) e.setAttribute(attr, "");
                } else {
                    e.removeAttribute(attr);
                }
                return Value::undefined();
            }));
    }
    accessor(h, "tabIndex",
        on<DOMElement>("tabIndex", [](DOMElement& e, const Info&) {
            std::string v = e.getAttribute("tabindex");
            return Value::number(v.empty() ? -1 : std::atof(v.c_str()));
        }),
        on<DOMElement>("tabIndex", [](DOMElement& e, const Info& info) {
            e.setAttribute("tabindex", std::to_string(static_cast<long long>(info.argument(0).toNumber())));
            return Value::undefined();
        }));
    // value: what the control holds (typed or set), not the attribute;
    // defaultValue is the attribute (a textarea's text)
    accessor(h, "value",
        on<DOMElement>("value", [](DOMElement& e, const Info&) { return str(formControlValue(&e)); }),
        on<DOMElement>("value", [this](DOMElement& e, const Info& info) {
            std::string v = info.argument(0).isNull() ? "" : info.argument(0).toString();
            std::string tag = lower(e.tagName());
            if (tag == "input" || tag == "textarea" || tag == "select") {
                setFormControlValue(&e, v);
                // A script's change is not the user's: no change event for it
                if (focusedElement() == &e) valueAtFocus_ = v;
                noteDomMutation();
            } else {
                e.setAttribute("value", v);
            }
            return Value::undefined();
        }));
    accessor(h, "defaultValue",
        on<DOMElement>("defaultValue", [](DOMElement& e, const Info&) {
            return str(lower(e.tagName()) == "textarea" ? textOf(e) : e.getAttribute("value"));
        }),
        on<DOMElement>("defaultValue", [this](DOMElement& e, const Info& info) {
            std::string v = info.argument(0).isNull() ? "" : info.argument(0).toString();
            if (lower(e.tagName()) == "textarea") setText(&e, v);
            else e.setAttribute("value", v);
            return Value::undefined();
        }));
    accessor(h, "checked",
        on<DOMElement>("checked", [](DOMElement& e, const Info&) { return Value::boolean(formControlChecked(&e)); }),
        on<DOMElement>("checked", [](DOMElement& e, const Info& info) {
            setFormControlChecked(&e, info.argument(0).toBoolean());
            noteDomMutation();
            return Value::undefined();
        }));

    // element.style
    Object* styleMethods = new Object();
    method(styleMethods, "getPropertyValue", 1, [](const FunctionCallInfo& info) -> Value {
        StyleObject* s = thisStyle(info, "getPropertyValue");
        DOMElement* el = s->element();
        return str(el ? parseInlineStyle(*el)->getPropertyValue(cssPropertyName(argString(info, 0))) : "");
    });
    method(styleMethods, "getPropertyPriority", 1, [](const FunctionCallInfo& info) -> Value {
        StyleObject* s = thisStyle(info, "getPropertyPriority");
        DOMElement* el = s->element();
        return str(el ? parseInlineStyle(*el)->getPropertyPriority(cssPropertyName(argString(info, 0))) : "");
    });
    method(styleMethods, "setProperty", 2, [](const FunctionCallInfo& info) -> Value {
        StyleObject* s = thisStyle(info, "setProperty");
        DOMElement* el = s->element();
        if (!el) return Value::undefined();
        auto decl = parseInlineStyle(*el);
        std::string name = cssPropertyName(argString(info, 0));
        std::string value = info.argument(1).isNullOrUndefined() ? "" : info.argument(1).toString();
        if (value.empty()) decl->removeProperty(name);
        else decl->setProperty(name, value, lower(info.argument(2).isUndefined() ? "" : argString(info, 2)));
        writeInlineStyle(*el, *decl);
        return Value::undefined();
    });
    method(styleMethods, "removeProperty", 1, [](const FunctionCallInfo& info) -> Value {
        StyleObject* s = thisStyle(info, "removeProperty");
        DOMElement* el = s->element();
        if (!el) return str("");
        auto decl = parseInlineStyle(*el);
        std::string old = decl->removeProperty(cssPropertyName(argString(info, 0)));
        writeInlineStyle(*el, *decl);
        return str(old);
    });
    method(styleMethods, "item", 1, [](const FunctionCallInfo& info) -> Value {
        StyleObject* s = thisStyle(info, "item");
        DOMElement* el = s->element();
        if (!el) return str("");
        return str(parseInlineStyle(*el)->item(static_cast<size_t>(info.argument(0).toNumber())));
    });
    method(styleMethods, "toString", 0, [](const FunctionCallInfo&) -> Value { return str("[object CSSStyleDeclaration]"); });
    accessor(h, "style", on<DOMElement>("style", [this, styleMethods](DOMElement&, const Info& info) {
        NodeWrapper* w = thisWrapper(info, "style");
        if (!w->style) {
            w->style = new StyleObject(w, styleMethods);
            w->style->setPrototype(styleDeclarationProto_);
        }
        return Value::object(w->style);
    }), on<DOMElement>("style", [](DOMElement& e, const Info& info) {
        e.setAttribute("style", info.argument(0).toString());
        return Value::undefined();
    }));
    accessor(h, "dataset", on<DOMElement>("dataset", [](DOMElement&, const Info& info) {
        NodeWrapper* w = thisWrapper(info, "dataset");
        if (!w->dataset) w->dataset = new DatasetObject(w);
        return Value::object(w->dataset);
    }));

    method(h, "click", 0, on<DOMElement>("click", [this](DOMElement& e, const Info&) {
        click(&e, 0, 0);
        return Value::undefined();
    }));
    method(h, "focus", 0, on<DOMElement>("focus", [this](DOMElement& e, const Info&) {
        setFocus(&e);
        return Value::undefined();
    }));
    method(h, "blur", 0, on<DOMElement>("blur", [this](DOMElement& e, const Info&) {
        if (focusedElement() == &e) setFocus(nullptr);
        return Value::undefined();
    }));

    // Per-tag interfaces below HTMLElement (HTMLMediaElement between it and audio/video)
    Object* mediaProto = makeInterface("HTMLMediaElement", htmlElementProto_);
    std::unordered_map<std::string, Object*> byName;
    for (const auto& t : kTagInterfaces) {
        Object*& proto = byName[t.name];
        if (!proto) {
            bool media = std::string(t.name) == "HTMLAudioElement" || std::string(t.name) == "HTMLVideoElement";
            proto = makeInterface(t.name, media ? mediaProto : htmlElementProto_);
        }
        tagInterfaces_[t.tag] = {t.name, proto};
    }
    makeInterface("HTMLUnknownElement", htmlElementProto_);

    // DOMTokenList
    tokenListProto_ = makeInterface("DOMTokenList", nullptr);
    Object* t = tokenListProto_;
    accessor(t, "length", [](const FunctionCallInfo& info) -> Value {
        return Value::number(static_cast<double>(thisTokenList(info, "length")->tokens().size()));
    });
    accessor(t, "value", [](const FunctionCallInfo& info) -> Value {
        TokenListObject* l = thisTokenList(info, "value");
        DOMElement* el = l->element();
        return str(el ? el->getAttribute("class") : "");
    }, [](const FunctionCallInfo& info) -> Value {
        if (DOMElement* el = thisTokenList(info, "value")->element()) el->setAttribute("class", argString(info, 0));
        return Value::undefined();
    });
    method(t, "item", 1, [](const FunctionCallInfo& info) -> Value {
        auto tokens = thisTokenList(info, "item")->tokens();
        double i = info.argument(0).toNumber();
        return i >= 0 && i < static_cast<double>(tokens.size()) ? str(tokens[static_cast<size_t>(i)]) : Value::null();
    });
    method(t, "contains", 1, [](const FunctionCallInfo& info) -> Value {
        auto tokens = thisTokenList(info, "contains")->tokens();
        return Value::boolean(std::find(tokens.begin(), tokens.end(), argString(info, 0)) != tokens.end());
    });
    method(t, "add", 0, [](const FunctionCallInfo& info) -> Value {
        TokenListObject* l = thisTokenList(info, "add");
        auto tokens = l->tokens();
        for (size_t i = 0; i < info.argumentCount(); i++) {
            std::string token = argString(info, i);
            checkToken(token, "add");
            if (std::find(tokens.begin(), tokens.end(), token) == tokens.end()) tokens.push_back(token);
        }
        l->write(tokens);
        return Value::undefined();
    });
    method(t, "remove", 0, [](const FunctionCallInfo& info) -> Value {
        TokenListObject* l = thisTokenList(info, "remove");
        auto tokens = l->tokens();
        for (size_t i = 0; i < info.argumentCount(); i++) {
            std::string token = argString(info, i);
            checkToken(token, "remove");
            tokens.erase(std::remove(tokens.begin(), tokens.end(), token), tokens.end());
        }
        DOMElement* el = l->element();
        if (el && el->hasAttribute("class")) l->write(tokens);
        return Value::undefined();
    });
    method(t, "toggle", 1, [](const FunctionCallInfo& info) -> Value {
        TokenListObject* l = thisTokenList(info, "toggle");
        std::string token = argString(info, 0);
        checkToken(token, "toggle");
        auto tokens = l->tokens();
        auto it = std::find(tokens.begin(), tokens.end(), token);
        bool present = it != tokens.end();
        bool want = info.argumentCount() > 1 && !info.argument(1).isUndefined() ? info.argument(1).toBoolean() : !present;
        if (want && !present) { tokens.push_back(token); l->write(tokens); }
        if (!want && present) { tokens.erase(it); l->write(tokens); }
        return Value::boolean(want);
    });
    method(t, "replace", 2, [](const FunctionCallInfo& info) -> Value {
        TokenListObject* l = thisTokenList(info, "replace");
        std::string from = argString(info, 0), to = argString(info, 1);
        checkToken(from, "replace");
        checkToken(to, "replace");
        auto tokens = l->tokens();
        auto it = std::find(tokens.begin(), tokens.end(), from);
        if (it == tokens.end()) return Value::boolean(false);
        if (std::find(tokens.begin(), tokens.end(), to) != tokens.end()) tokens.erase(it);
        else *it = to;
        l->write(tokens);
        return Value::boolean(true);
    });
    method(t, "forEach", 1, [this](const FunctionCallInfo& info) -> Value {
        TokenListObject* l = thisTokenList(info, "forEach");
        Value fn = info.argument(0);
        if (!isFunction(fn)) throw JSError("TypeError", "DOMTokenList.forEach: callback is not a function");
        auto tokens = l->tokens();
        for (size_t i = 0; i < tokens.size(); i++) {
            vm_->call(static_cast<Function*>(fn.asObject()), info.argument(1),
                      {str(tokens[i]), Value::number(static_cast<double>(i)), info.thisValue()});
        }
        return Value::undefined();
    });
    method(t, "toString", 0, [](const FunctionCallInfo& info) -> Value {
        DOMElement* el = thisTokenList(info, "toString")->element();
        return str(el ? el->getAttribute("class") : "");
    });
}

// ----------------------------------------------------------------------------
// Layout geometry: element boxes, the viewport and the page's scroll position
// ----------------------------------------------------------------------------

namespace {

long roundPx(double v) { return std::lround(v); }

bool isTag(const DOMElement* e, const char* tag) {
    if (!e) return false;
    const std::string& name = e->tagName();
    size_t n = std::strlen(tag);
    if (name.size() != n) return false;
    for (size_t i = 0; i < n; i++) {
        if (std::tolower(static_cast<unsigned char>(name[i])) != tag[i]) return false;
    }
    return true;
}

// x and y of scrollTo(x, y) / scrollTo({left, top}); a missing one keeps `x`/`y`
void scrollArguments(const FunctionCallInfo& info, double& x, double& y) {
    Value first = info.argument(0);
    if (info.argumentCount() == 1 && first.isObject()) {
        Value left = first.asObject()->get("left"), top = first.asObject()->get("top");
        if (!left.isUndefined()) x = left.toNumber();
        if (!top.isUndefined()) y = top.toNumber();
    } else if (info.argumentCount() >= 2) {
        x = first.toNumber();
        y = info.argument(1).toNumber();
    }
    // CSSOM View: non-finite coordinates are 0
    if (!std::isfinite(x)) x = 0;
    if (!std::isfinite(y)) y = 0;
}

double finiteOrZero(double v) { return std::isfinite(v) ? v : 0; }

}  // namespace

ElementGeometry DOMBindings::geometryOf(const DOMElement* element, bool withScrollSize) {
    if (!geometry_ || !element || !isConnectedTo(element, document_)) return {};
    return geometry_->elementGeometry(element, withScrollSize);
}

bool DOMBindings::isScrollingElement(const DOMElement* element) const {
    return document_ && element == document_->documentElement();
}

Value DOMBindings::rectObject(const GeometryRect& r) {
    auto* rect = new Object();
    rect->setPrototype(domRectProto_);
    const std::pair<const char*, double> fields[] = {
        {"x", r.x}, {"y", r.y}, {"width", r.width}, {"height", r.height},
        {"top", r.y}, {"right", r.x + r.width}, {"bottom", r.y + r.height}, {"left", r.x}};
    for (const auto& [name, value] : fields) rect->set(name, Value::number(value));
    return Value::object(rect);
}

// CSSOM View §7: the nearest positioned ancestor, else a table cell or table
// around a static element, else <body>; none for <html>, <body>, fixed
// elements and elements without a box
DOMElement* DOMBindings::offsetParentOf(DOMElement* element) {
    if (isTag(element, "html") || isTag(element, "body")) return nullptr;
    ElementGeometry g = geometryOf(element);
    if (!g.hasBox || g.position == 3) return nullptr;
    for (DOMElement* a = asElement(element->parentNode()); a; a = asElement(a->parentNode())) {
        if (isTag(a, "body")) return a;
        ElementGeometry ag = geometryOf(a);
        if (ag.hasBox && ag.position != 0) return a;
        if (g.position == 0 && (isTag(a, "td") || isTag(a, "th") || isTag(a, "table"))) return a;
    }
    return nullptr;
}

void DOMBindings::scrollPage(double x, double y) {
    if (!geometry_) return;
    geometry_->scrollTo(x, y);
}


void DOMBindings::installGeometry() {
    domRectProto_ = makeInterface("DOMRect", nullptr);
    method(domRectProto_, "toJSON", 0, [](const FunctionCallInfo& info) -> Value {
        Value self = info.thisValue();
        auto* json = new Object();
        if (self.isObject()) {
            for (const char* k : {"x", "y", "width", "height", "top", "right", "bottom", "left"}) {
                json->set(k, self.asObject()->get(k));
            }
        }
        return Value::object(json);
    });

    // Element: border boxes in viewport coordinates
    Object* p = elementProto_;
    method(p, "getBoundingClientRect", 0, on<DOMElement>("getBoundingClientRect", [this](DOMElement& e, const Info&) {
        return rectObject(geometryOf(&e).bounds());
    }));
    method(p, "getClientRects", 0, on<DOMElement>("getClientRects", [this](DOMElement& e, const Info&) {
        auto* list = new Array();
        ElementGeometry g = geometryOf(&e);
        for (const GeometryRect& r : g.rects) list->push(rectObject(r));
        return Value::object(list);
    }));
    // client*: the padding box (no scrollbars here); the viewport for <html>
    accessor(p, "clientWidth", on<DOMElement>("clientWidth", [this](DOMElement& e, const Info&) {
        if (isScrollingElement(&e)) return Value::number(roundPx(geometry_ ? geometry_->viewportWidth() : 0));
        ElementGeometry g = geometryOf(&e);
        if (!g.hasBox || g.inlineBox) return Value::number(0);
        return Value::number(roundPx(std::max(0.0, g.rects[0].width - g.borderLeft - g.borderRight)));
    }));
    accessor(p, "clientHeight", on<DOMElement>("clientHeight", [this](DOMElement& e, const Info&) {
        if (isScrollingElement(&e)) return Value::number(roundPx(geometry_ ? geometry_->viewportHeight() : 0));
        ElementGeometry g = geometryOf(&e);
        if (!g.hasBox || g.inlineBox) return Value::number(0);
        return Value::number(roundPx(std::max(0.0, g.rects[0].height - g.borderTop - g.borderBottom)));
    }));
    accessor(p, "clientTop", on<DOMElement>("clientTop", [this](DOMElement& e, const Info&) {
        ElementGeometry g = geometryOf(&e);
        return Value::number(g.hasBox && !g.inlineBox ? roundPx(g.borderTop) : 0);
    }));
    accessor(p, "clientLeft", on<DOMElement>("clientLeft", [this](DOMElement& e, const Info&) {
        ElementGeometry g = geometryOf(&e);
        return Value::number(g.hasBox && !g.inlineBox ? roundPx(g.borderLeft) : 0);
    }));
    accessor(p, "scrollWidth", on<DOMElement>("scrollWidth", [this](DOMElement& e, const Info&) {
        return Value::number(roundPx(geometryOf(&e, true).scrollWidth));
    }));
    accessor(p, "scrollHeight", on<DOMElement>("scrollHeight", [this](DOMElement& e, const Info&) {
        return Value::number(roundPx(geometryOf(&e, true).scrollHeight));
    }));
    // Only the page scrolls (no element is a scroll container yet): <html>
    // reads and moves the page's scroll position, other elements stay at 0
    accessor(p, "scrollTop",
        on<DOMElement>("scrollTop", [this](DOMElement& e, const Info&) {
            return Value::number(isScrollingElement(&e) && geometry_ ? geometry_->scrollY() : 0);
        }),
        on<DOMElement>("scrollTop", [this](DOMElement& e, const Info& info) {
            if (isScrollingElement(&e) && geometry_) scrollPage(geometry_->scrollX(), finiteOrZero(info.argument(0).toNumber()));
            return Value::undefined();
        }));
    accessor(p, "scrollLeft",
        on<DOMElement>("scrollLeft", [this](DOMElement& e, const Info&) {
            return Value::number(isScrollingElement(&e) && geometry_ ? geometry_->scrollX() : 0);
        }),
        on<DOMElement>("scrollLeft", [this](DOMElement& e, const Info& info) {
            if (isScrollingElement(&e) && geometry_) scrollPage(finiteOrZero(info.argument(0).toNumber()), geometry_->scrollY());
            return Value::undefined();
        }));
    for (const char* name : {"scrollTo", "scroll", "scrollBy"}) {
        bool relative = std::strcmp(name, "scrollBy") == 0;
        method(p, name, 0, on<DOMElement>(name, [this, relative](DOMElement& e, const Info& info) {
            if (!isScrollingElement(&e) || !geometry_) return Value::undefined();
            double x = relative ? 0 : geometry_->scrollX(), y = relative ? 0 : geometry_->scrollY();
            scrollArguments(info, x, y);
            if (relative) { x += geometry_->scrollX(); y += geometry_->scrollY(); }
            scrollPage(x, y);
            return Value::undefined();
        }));
    }
    // scrollIntoView(alignToTop) / scrollIntoView({block}): start (default),
    // end, center, or nearest (the closer edge, and only when the box is
    // partly out of view without covering the whole viewport)
    method(p, "scrollIntoView", 0, on<DOMElement>("scrollIntoView", [this](DOMElement& e, const Info& info) {
        ElementGeometry g = geometryOf(&e);
        if (!g.hasBox || !geometry_) return Value::undefined();
        std::string block = "start";
        Value arg = info.argument(0);
        if (arg.isObject()) {
            Value b = arg.asObject()->get("block");
            if (!b.isUndefined()) block = b.toString();
        } else if (arg.isBoolean() && !arg.asBoolean()) {
            block = "end";
        }
        GeometryRect r = g.bounds();
        double viewH = geometry_->viewportHeight(), sy = geometry_->scrollY();
        double target = sy + r.y;
        if (block == "end") target = sy + r.y + r.height - viewH;
        else if (block == "center") target = sy + r.y + r.height / 2 - viewH / 2;
        else if (block == "nearest") {
            bool aboveTop = r.y < 0, belowBottom = r.y + r.height > viewH;
            if (aboveTop == belowBottom) return Value::undefined();  // in view, or covering it
            bool alignEnd = (aboveTop && r.height > viewH) || (belowBottom && r.height <= viewH);
            if (alignEnd) target = sy + r.y + r.height - viewH;
        }
        scrollPage(geometry_->scrollX(), target);
        return Value::undefined();
    }));

    // HTMLElement: offset* against the offsetParent's padding edge, or the
    // page origin when that is <body> or there is none
    Object* h = htmlElementProto_;
    accessor(h, "offsetParent", on<DOMElement>("offsetParent", [this](DOMElement& e, const Info&) {
        return wrap(offsetParentOf(&e));
    }));
    auto offsetOrigin = [this](DOMElement& e, bool vertical) -> double {
        ElementGeometry g = geometryOf(&e);
        // <body> sits at the page origin as far as offset* go (CSSOM View §7)
        if (!g.hasBox || isTag(&e, "body")) return 0;
        double edge = vertical ? g.rects[0].y : g.rects[0].x;
        DOMElement* parent = offsetParentOf(&e);
        if (!parent || isTag(parent, "body")) {
            return edge + (vertical ? geometry_->scrollY() : geometry_->scrollX());
        }
        ElementGeometry pg = geometryOf(parent);
        if (!pg.hasBox) return edge;
        return vertical ? edge - (pg.rects[0].y + pg.borderTop) : edge - (pg.rects[0].x + pg.borderLeft);
    };
    accessor(h, "offsetTop", on<DOMElement>("offsetTop", [offsetOrigin](DOMElement& e, const Info&) {
        return Value::number(roundPx(offsetOrigin(e, true)));
    }));
    accessor(h, "offsetLeft", on<DOMElement>("offsetLeft", [offsetOrigin](DOMElement& e, const Info&) {
        return Value::number(roundPx(offsetOrigin(e, false)));
    }));
    accessor(h, "offsetWidth", on<DOMElement>("offsetWidth", [this](DOMElement& e, const Info&) {
        return Value::number(roundPx(geometryOf(&e).bounds().width));
    }));
    accessor(h, "offsetHeight", on<DOMElement>("offsetHeight", [this](DOMElement& e, const Info&) {
        return Value::number(roundPx(geometryOf(&e).bounds().height));
    }));

    // Window: viewport size and the page's scroll position. These are
    // [Replaceable]: assigning one (or `var scrollY = ...` at script level)
    // turns it into the page's own plain property.
    Object* w = global_;
    auto replaceable = [w](const std::string& name) -> BuiltinFn {
        return [w, name](const Info& info) -> Value {
            PropertyDescriptor desc;
            desc.value = info.argumentCount() > 0 ? info.argument(0) : Value::undefined();
            desc.attributes = PropertyAttribute::Writable | PropertyAttribute::Configurable |
                              PropertyAttribute::Enumerable;
            w->defineProperty(name, desc);
            return Value::undefined();
        };
    };
    accessor(w, "innerWidth", [this](const Info&) -> Value {
        return Value::number(geometry_ ? geometry_->viewportWidth() : kDefaultViewportWidth);
    }, replaceable("innerWidth"));
    accessor(w, "innerHeight", [this](const Info&) -> Value {
        return Value::number(geometry_ ? geometry_->viewportHeight() : kDefaultViewportHeight);
    }, replaceable("innerHeight"));
    for (const char* name : {"scrollX", "pageXOffset"}) {
        accessor(w, name, [this](const Info&) -> Value { return Value::number(geometry_ ? geometry_->scrollX() : 0); },
                 replaceable(name));
    }
    for (const char* name : {"scrollY", "pageYOffset"}) {
        accessor(w, name, [this](const Info&) -> Value { return Value::number(geometry_ ? geometry_->scrollY() : 0); },
                 replaceable(name));
    }
    for (const char* name : {"scrollTo", "scroll", "scrollBy"}) {
        bool relative = std::strcmp(name, "scrollBy") == 0;
        auto scroll = [this, relative](const Info& info) -> Value {
            if (!geometry_) return Value::undefined();
            double x = relative ? 0 : geometry_->scrollX(), y = relative ? 0 : geometry_->scrollY();
            scrollArguments(info, x, y);
            if (relative) { x += geometry_->scrollX(); y += geometry_->scrollY(); }
            scrollPage(x, y);
            return Value::undefined();
        };
        method(w, name, 2, scroll);
    }
}

// ----------------------------------------------------------------------------
// Document
// ----------------------------------------------------------------------------

void DOMBindings::installDocumentInterface() {
    Object* p = documentProto_;
    htmlDocumentProto_ = makeInterface("HTMLDocument", documentProto_);

    accessor(p, "documentElement", on<DOMDocument>("documentElement", [this](DOMDocument& d, const Info&) {
        return wrap(d.documentElement());
    }));
    accessor(p, "head", on<DOMDocument>("head", [this](DOMDocument& d, const Info&) { return wrap(d.head()); }));
    accessor(p, "body", on<DOMDocument>("body", [this](DOMDocument& d, const Info&) { return wrap(d.body()); }));
    accessor(p, "scrollingElement", on<DOMDocument>("scrollingElement", [this](DOMDocument& d, const Info&) {
        return wrap(d.documentElement());
    }));
    accessor(p, "activeElement", on<DOMDocument>("activeElement", [this](DOMDocument& d, const Info&) {
        DOMElement* focused = focusedElement();
        return wrap(focused ? focused : d.body());
    }));
    method(p, "hasFocus", 0, on<DOMDocument>("hasFocus", [](DOMDocument&, const Info&) { return Value::boolean(true); }));
    accessor(p, "title",
        on<DOMDocument>("title", [](DOMDocument& d, const Info&) {
            std::string title = d.title();
            // Whitespace stripped and collapsed
            std::string out;
            for (char c : title) {
                bool space = std::isspace(static_cast<unsigned char>(c));
                if (space) { if (!out.empty() && out.back() != ' ') out += ' '; }
                else out += c;
            }
            if (!out.empty() && out.back() == ' ') out.pop_back();
            return str(out);
        }),
        on<DOMDocument>("title", [this](DOMDocument& d, const Info& info) {
            DOMElement* head = d.head();
            if (!head) return Value::undefined();
            DOMElement* title = nullptr;
            for (const auto& c : head->childNodes()) {
                if (DOMElement* e = asElement(c.get()); e && lower(e->tagName()) == "title") { title = e; break; }
            }
            if (!title) title = static_cast<DOMElement*>(head->appendChild(d.createElement("title")));
            setText(title, info.argument(0).toString());
            return Value::undefined();
        }));
    accessor(p, "readyState", on<DOMDocument>("readyState", [this](DOMDocument&, const Info&) { return str(readyState_); }));
    auto pageUrl = [this]() {
        Value url = vm_->getGlobal("__pageUrl__");
        return url.isString() ? url.toString() : std::string();
    };
    accessor(p, "URL", on<DOMDocument>("URL", [pageUrl](DOMDocument&, const Info&) { return str(pageUrl()); }));
    accessor(p, "documentURI", on<DOMDocument>("documentURI", [pageUrl](DOMDocument&, const Info&) { return str(pageUrl()); }));
    accessor(p, "defaultView", on<DOMDocument>("defaultView", [this](DOMDocument&, const Info&) {
        return Value::object(global_);
    }));
    accessor(p, "location", on<DOMDocument>("location", [this](DOMDocument&, const Info&) {
        return vm_->getMember(Value::object(global_), "location");
    }));
    accessor(p, "compatMode", on<DOMDocument>("compatMode", [](DOMDocument& d, const Info&) {
        return str(d.quirksMode() ? "BackCompat" : "CSS1Compat");
    }));
    accessor(p, "characterSet", on<DOMDocument>("characterSet", [](DOMDocument&, const Info&) { return str("UTF-8"); }));
    defineHidden(p, "charset", str("UTF-8"));
    defineHidden(p, "contentType", str("text/html"));
    defineHidden(p, "hidden", Value::boolean(false));
    defineHidden(p, "visibilityState", str("visible"));
    defineHidden(p, "doctype", Value::null());
    defineHidden(p, "currentScript", Value::null());

    method(p, "hasFocus", 0, [](const FunctionCallInfo&) -> Value { return Value::boolean(true); });
    // document.write is not supported (the page is parsed before scripts run)
    for (const char* name : {"write", "writeln", "open", "close"}) {
        method(p, name, 0, [](const FunctionCallInfo&) -> Value { return Value::undefined(); });
    }
    auto createElement = [this](DOMDocument& d, const std::string& tag) -> Value {
        if (tag.empty()) {
            throwDOMException("InvalidCharacterError", "Failed to execute 'createElement' on 'Document': The tag name provided ('') is not a valid name.");
        }
        std::unique_ptr<DOMNode> el = d.createElement(lower(tag));
        DOMNode* raw = el.get();
        NodeWrapper* w = wrapperOf(raw);
        w->owned = std::move(el);
        return Value::object(w);
    };
    method(p, "createElement", 1, on<DOMDocument>("createElement", [createElement](DOMDocument& d, const Info& info) {
        return createElement(d, argString(info, 0));
    }));
    method(p, "createElementNS", 2, on<DOMDocument>("createElementNS", [createElement](DOMDocument& d, const Info& info) {
        std::string name = argString(info, 1);
        size_t colon = name.find(':');
        return createElement(d, colon == std::string::npos ? name : name.substr(colon + 1));
    }));
    auto own = [this](std::unique_ptr<DOMNode> node) -> Value {
        DOMNode* raw = node.get();
        NodeWrapper* w = wrapperOf(raw);
        w->owned = std::move(node);
        return Value::object(w);
    };
    method(p, "createTextNode", 1, on<DOMDocument>("createTextNode", [own](DOMDocument& d, const Info& info) {
        return own(d.createTextNode(argString(info, 0)));
    }));
    method(p, "createComment", 1, on<DOMDocument>("createComment", [own](DOMDocument& d, const Info& info) {
        return own(d.createComment(argString(info, 0)));
    }));
    method(p, "createDocumentFragment", 0, on<DOMDocument>("createDocumentFragment", [own](DOMDocument& d, const Info&) {
        return own(d.createDocumentFragment());
    }));
    method(p, "createEvent", 1, on<DOMDocument>("createEvent", [this](DOMDocument&, const Info& info) {
        std::string kind = lower(argString(info, 0));
        Object* proto = kind.rfind("mouse", 0) == 0 ? mouseEventProto_ : eventProto_;
        EventObject* e = newEvent(proto, "", Value::undefined());
        return Value::object(e);
    }));
    method(p, "getElementById", 1, on<DOMDocument>("getElementById", [this](DOMDocument& d, const Info& info) {
        return wrap(d.getElementById(argString(info, 0)));
    }));
    method(fragmentProto_, "getElementById", 1, on<DOMDocumentFragment>("getElementById", [this](DOMDocumentFragment& f, const Info& info) {
        std::string id = argString(info, 0);
        for (const auto& c : f.childNodes()) {
            if (DOMElement* e = asElement(c.get())) {
                if (DOMElement* found = e->getElementById(id)) return wrap(found);
            }
        }
        return Value::null();
    }));
    method(p, "getElementsByName", 1, on<DOMDocument>("getElementsByName", [this](DOMDocument& d, const Info& info) {
        std::string name = argString(info, 0);
        std::vector<DOMElement*> all, found;
        collectByTag(d, "*", all);
        for (DOMElement* e : all) if (e->getAttribute("name") == name) found.push_back(e);
        return elementList(found);
    }));
}

void DOMBindings::installDocument(DOMDocument* document) {
    document_ = document;
    if (!document_) return;
    document_->setMutationSink(mutationObservers_.get());
    Value doc = wrap(document_);
    // A plain property (cookies are not stored yet)
    doc.asObject()->set("cookie", str(""));
    vm_->setGlobal("document", doc);
    defineHidden(global_, "document", doc);
}

// ----------------------------------------------------------------------------
// Forms: form.submit(), requestSubmit(), reset(), elements; control.form
// ----------------------------------------------------------------------------

void DOMBindings::installForms() {
    Object* form = tagInterfaces_["form"].second;
    method(form, "submit", 0, on<DOMElement>("submit", [this](DOMElement& f, const Info&) {
        submit(&f, nullptr);
        return Value::undefined();
    }));
    method(form, "requestSubmit", 0, on<DOMElement>("requestSubmit", [this](DOMElement& f, const Info& info) {
        DOMElement* submitter = nullptr;
        if (!info.argument(0).isNullOrUndefined()) {
            submitter = asElement(nodeOf(info.argument(0).isObject() ? info.argument(0).asObject() : nullptr));
            if (!submitter || !isSubmitButton(submitter)) {
                throw JSError("TypeError", "Failed to execute 'requestSubmit' on 'HTMLFormElement': The specified element is not a submit button.");
            }
            if (formOwner(submitter) != &f) {
                throwDOMException("NotFoundError", "Failed to execute 'requestSubmit' on 'HTMLFormElement': The specified element is not owned by this form element.");
            }
        }
        requestSubmit(&f, submitter);
        return Value::undefined();
    }));
    method(form, "reset", 0, on<DOMElement>("reset", [this](DOMElement& f, const Info&) {
        resetForm(&f);
        return Value::undefined();
    }));
    method(form, "checkValidity", 0, on<DOMElement>("checkValidity", [](DOMElement&, const Info&) { return Value::boolean(true); }));
    method(form, "reportValidity", 0, on<DOMElement>("reportValidity", [](DOMElement&, const Info&) { return Value::boolean(true); }));
    accessor(form, "elements", on<DOMElement>("elements", [this](DOMElement& f, const Info&) {
        return elementList(formControls(&f));
    }));
    accessor(form, "length", on<DOMElement>("length", [](DOMElement& f, const Info&) {
        return Value::number(static_cast<double>(formControls(&f).size()));
    }));

    // control.form: the form owner (form attribute, else the <form> around it)
    for (const char* tag : {"input", "button", "select", "textarea", "fieldset", "output", "object"}) {
        auto it = tagInterfaces_.find(tag);
        if (it == tagInterfaces_.end() || it->second.second->getOwnPropertyDescriptor("form")) continue;
        accessor(it->second.second, "form", on<DOMElement>("form", [this](DOMElement& e, const Info&) {
            DOMElement* owner = formOwner(&e);
            return owner ? wrap(owner) : Value::null();
        }));
    }
    // The text selection is the end of the value (no caret position yet)
    for (const char* tag : {"input", "textarea"}) {
        Object* proto = tagInterfaces_[tag].second;
        for (const char* name : {"selectionStart", "selectionEnd"}) {
            accessor(proto, name, on<DOMElement>(name, [](DOMElement& e, const Info&) {
                return Value::number(static_cast<double>(formControlValue(&e).size()));
            }), on<DOMElement>(name, [](DOMElement&, const Info&) { return Value::undefined(); }));
        }
        for (const char* name : {"select", "setSelectionRange", "setRangeText"}) {
            method(proto, name, 0, on<DOMElement>(name, [](DOMElement&, const Info&) { return Value::undefined(); }));
        }
    }
}

// ----------------------------------------------------------------------------
// Event, CustomEvent, UIEvent, MouseEvent, KeyboardEvent, ...
// ----------------------------------------------------------------------------

void DOMBindings::installEvents() {
    eventProto_ = makeInterface("Event", nullptr);
    Object* p = eventProto_;
    const std::pair<const char*, double> kPhases[] = {{"NONE", 0}, {"CAPTURING_PHASE", 1}, {"AT_TARGET", 2}, {"BUBBLING_PHASE", 3}};
    for (const auto& [name, value] : kPhases) constant(p, name, value);

    accessor(p, "type", [](const FunctionCallInfo& info) -> Value { return str(thisEvent(info, "type")->type); });
    accessor(p, "target", [](const FunctionCallInfo& info) -> Value { return thisEvent(info, "target")->target; });
    accessor(p, "srcElement", [](const FunctionCallInfo& info) -> Value { return thisEvent(info, "srcElement")->target; });
    accessor(p, "currentTarget", [](const FunctionCallInfo& info) -> Value {
        return thisEvent(info, "currentTarget")->currentTarget;
    });
    accessor(p, "eventPhase", [](const FunctionCallInfo& info) -> Value {
        return Value::number(thisEvent(info, "eventPhase")->phase);
    });
    accessor(p, "bubbles", [](const FunctionCallInfo& info) -> Value { return Value::boolean(thisEvent(info, "bubbles")->bubbles); });
    accessor(p, "cancelable", [](const FunctionCallInfo& info) -> Value {
        return Value::boolean(thisEvent(info, "cancelable")->cancelable);
    });
    accessor(p, "composed", [](const FunctionCallInfo& info) -> Value { return Value::boolean(thisEvent(info, "composed")->composed); });
    accessor(p, "isTrusted", [](const FunctionCallInfo& info) -> Value { return Value::boolean(thisEvent(info, "isTrusted")->trusted); });
    accessor(p, "timeStamp", [](const FunctionCallInfo& info) -> Value { return Value::number(thisEvent(info, "timeStamp")->timeStamp); });
    accessor(p, "defaultPrevented", [](const FunctionCallInfo& info) -> Value {
        return Value::boolean(thisEvent(info, "defaultPrevented")->canceled);
    });
    accessor(p, "returnValue", [](const FunctionCallInfo& info) -> Value {
        return Value::boolean(!thisEvent(info, "returnValue")->canceled);
    }, [](const FunctionCallInfo& info) -> Value {
        EventObject* e = thisEvent(info, "returnValue");
        if (!info.argument(0).toBoolean() && e->cancelable && !e->inPassiveListener) e->canceled = true;
        return Value::undefined();
    });
    accessor(p, "cancelBubble", [](const FunctionCallInfo& info) -> Value {
        return Value::boolean(thisEvent(info, "cancelBubble")->stopped);
    }, [](const FunctionCallInfo& info) -> Value {
        if (info.argument(0).toBoolean()) thisEvent(info, "cancelBubble")->stopped = true;
        return Value::undefined();
    });
    method(p, "preventDefault", 0, [](const FunctionCallInfo& info) -> Value {
        EventObject* e = thisEvent(info, "preventDefault");
        if (e->cancelable && !e->inPassiveListener) e->canceled = true;
        return Value::undefined();
    });
    method(p, "stopPropagation", 0, [](const FunctionCallInfo& info) -> Value {
        thisEvent(info, "stopPropagation")->stopped = true;
        return Value::undefined();
    });
    method(p, "stopImmediatePropagation", 0, [](const FunctionCallInfo& info) -> Value {
        EventObject* e = thisEvent(info, "stopImmediatePropagation");
        e->stopped = e->stoppedImmediately = true;
        return Value::undefined();
    });
    method(p, "composedPath", 0, [](const FunctionCallInfo& info) -> Value {
        auto* list = new Array();
        for (Object* o : thisEvent(info, "composedPath")->path) list->push(Value::object(o));
        return Value::object(list);
    });
    method(p, "initEvent", 3, [](const FunctionCallInfo& info) -> Value {
        EventObject* e = thisEvent(info, "initEvent");
        if (e->dispatching) return Value::undefined();
        e->type = argString(info, 0);
        e->bubbles = info.argument(1).toBoolean();
        e->cancelable = info.argument(2).toBoolean();
        e->canceled = e->stopped = e->stoppedImmediately = false;
        return Value::undefined();
    });

    // Constructors: new Event(type, { bubbles, cancelable, composed });
    // the subclasses copy the other init members (detail, clientX, key, ...)
    auto eventInterface = [this](const std::string& name, Object* parent) {
        Object* proto = parent == nullptr ? eventProto_ : makeInterface(name, parent);
        auto* ctor = new Function(name, [this, name, proto](const FunctionCallInfo& info) -> Value {
            if (info.argumentCount() == 0) {
                throw JSError("TypeError", "Failed to construct '" + name + "': 1 argument required, but only 0 present.");
            }
            return Value::object(newEvent(proto, argString(info, 0), info.argument(1)));
        }, 1);
        ctor->set("prototype", Value::object(proto));
        defineHidden(proto, "constructor", Value::object(ctor));
        vm_->setGlobal(name, Value::object(ctor));
        defineHidden(global_, name, Value::object(ctor));
        return proto;
    };
    eventInterface("Event", nullptr);
    Object* custom = eventInterface("CustomEvent", eventProto_);
    method(custom, "initCustomEvent", 4, [](const FunctionCallInfo& info) -> Value {
        EventObject* e = thisEvent(info, "initCustomEvent");
        e->type = argString(info, 0);
        e->bubbles = info.argument(1).toBoolean();
        e->cancelable = info.argument(2).toBoolean();
        e->set("detail", info.argument(3));
        return Value::undefined();
    });
    Object* ui = eventInterface("UIEvent", eventProto_);
    mouseEventProto_ = eventInterface("MouseEvent", ui);
    Object* pointer = eventInterface("PointerEvent", mouseEventProto_);
    eventInterface("WheelEvent", mouseEventProto_);
    keyboardEventProto_ = eventInterface("KeyboardEvent", ui);
    focusEventProto_ = eventInterface("FocusEvent", ui);
    inputEventProto_ = eventInterface("InputEvent", ui);
    eventInterface("CompositionEvent", ui);
    eventInterface("TouchEvent", ui);
    eventInterface("ProgressEvent", eventProto_);
    eventInterface("ErrorEvent", eventProto_);
    eventInterface("HashChangeEvent", eventProto_);
    eventInterface("PopStateEvent", eventProto_);
    eventInterface("MessageEvent", eventProto_);
    eventInterface("AnimationEvent", eventProto_);
    eventInterface("TransitionEvent", eventProto_);
    submitEventProto_ = eventInterface("SubmitEvent", eventProto_);
    (void)pointer;

    const std::pair<const char*, double> kLocations[] = {{"DOM_KEY_LOCATION_STANDARD", 0}, {"DOM_KEY_LOCATION_LEFT", 1},
                                                         {"DOM_KEY_LOCATION_RIGHT", 2}, {"DOM_KEY_LOCATION_NUMPAD", 3}};
    for (const auto& [name, value] : kLocations) {
        constant(keyboardEventProto_, name, value);
        if (Value ctor = vm_->getGlobal("KeyboardEvent"); ctor.isObject()) constant(ctor.asObject(), name, value);
    }
    // getModifierState("Shift") reads the event's shiftKey, ...
    auto getModifierState = [this](const FunctionCallInfo& info) -> Value {
        thisEvent(info, "getModifierState");
        static const std::pair<const char*, const char*> kModifiers[] = {
            {"Alt", "altKey"}, {"Control", "ctrlKey"}, {"Shift", "shiftKey"}, {"Meta", "metaKey"}};
        std::string key = argString(info, 0);
        for (const auto& [name, member] : kModifiers) {
            if (key == name) return Value::boolean(vm_->getMember(info.thisValue(), member).toBoolean());
        }
        return Value::boolean(false);
    };
    method(keyboardEventProto_, "getModifierState", 1, getModifierState);
    method(mouseEventProto_, "getModifierState", 1, getModifierState);
}

EventObject* DOMBindings::newEvent(Object* proto, const std::string& type, const Value& init) {
    static const double origin = nowMs();
    auto* e = new EventObject();
    e->setPrototype(proto);
    e->type = type;
    e->timeStamp = nowMs() - origin;
    // Defaults of the subclass members a page may read
    if (proto != eventProto_) {
        e->set("detail", proto == mouseEventProto_ ? Value::number(0) : Value::null());
    }
    if (proto == keyboardEventProto_) {
        for (const char* k : {"key", "code"}) e->set(k, str(""));
        for (const char* k : {"keyCode", "which", "charCode", "location"}) e->set(k, Value::number(0));
        for (const char* k : {"repeat", "isComposing", "altKey", "ctrlKey", "shiftKey", "metaKey"}) e->set(k, Value::boolean(false));
        e->set("detail", Value::number(0));
    } else if (proto == inputEventProto_) {
        e->set("data", Value::null());
        e->set("inputType", str(""));
        e->set("isComposing", Value::boolean(false));
        e->set("detail", Value::number(0));
    } else if (proto == focusEventProto_) {
        e->set("relatedTarget", Value::null());
        e->set("detail", Value::number(0));
    } else if (proto == submitEventProto_) {
        e->set("submitter", Value::null());
    }
    if (init.isObject()) {
        Object* o = init.asObject();
        for (const std::string& key : o->getOwnPropertyNames()) {
            Value v = vm_->getMember(init, key);
            if (key == "bubbles") e->bubbles = v.toBoolean();
            else if (key == "cancelable") e->cancelable = v.toBoolean();
            else if (key == "composed") e->composed = v.toBoolean();
            else if (proto != eventProto_) e->set(key, v);
        }
    }
    return e;
}

// ----------------------------------------------------------------------------
// CSSStyleDeclaration and getComputedStyle()
// ----------------------------------------------------------------------------

void DOMBindings::installComputedStyle() {
    // The interface of element.style and of computed styles
    styleDeclarationProto_ = makeInterface("CSSStyleDeclaration", nullptr);

    Object* methods = new Object();
    method(methods, "getPropertyValue", 1, [](const FunctionCallInfo& info) -> Value {
        ComputedStyleObject* s = thisComputedStyle(info, "getPropertyValue");
        std::string name = argString(info, 0);
        return str(s->value(name.rfind("--", 0) == 0 ? name : lower(name)));
    });
    method(methods, "getPropertyPriority", 1, [](const FunctionCallInfo& info) -> Value {
        thisComputedStyle(info, "getPropertyPriority");
        return str("");
    });
    method(methods, "item", 1, [](const FunctionCallInfo& info) -> Value {
        auto v = thisComputedStyle(info, "item")->values();
        double i = info.argument(0).toNumber();
        return str(i >= 0 && i < static_cast<double>(v.size()) ? v[static_cast<size_t>(i)].first : "");
    });
    auto readOnly = [this](const char* name) {
        return [this, name](const FunctionCallInfo& info) -> Value {
            thisComputedStyle(info, name);
            throwComputedReadOnly(vm_, std::string("Failed to execute '") + name + "'", argString(info, 0));
        };
    };
    method(methods, "setProperty", 2, readOnly("setProperty"));
    method(methods, "removeProperty", 1, readOnly("removeProperty"));
    method(methods, "toString", 0, [](const FunctionCallInfo&) -> Value { return str("[object CSSStyleDeclaration]"); });

    // getComputedStyle(element[, pseudoElement]), on the window and bare
    auto* fn = new Function("getComputedStyle", [this, methods](const FunctionCallInfo& info) -> Value {
        Value target = info.argument(0);
        auto* w = target.isObject() ? dynamic_cast<NodeWrapper*>(target.asObject()) : nullptr;
        if (!w || !asElement(w->node)) {
            throw JSError("TypeError", "Failed to execute 'getComputedStyle' on 'Window': parameter 1 is not of type 'Element'.");
        }
        Value pseudo = info.argument(1);
        bool pseudoElement = !pseudo.isNullOrUndefined() && !pseudo.toString().empty();
        auto* style = new ComputedStyleObject(vm_, this, w, pseudoElement, methods);
        style->setPrototype(styleDeclarationProto_);
        return Value::object(style);
    }, 1);
    global_->set("getComputedStyle", Value::object(fn));
    vm_->setGlobal("getComputedStyle", Value::object(fn));
}

std::vector<std::pair<std::string, std::string>> DOMBindings::computedValues(DOMElement* element) const {
    if (!element || !computedStyleSource_ || !isConnectedTo(element, document_)) return {};
    const CSSComputedStyle* style = computedStyleSource_(element);
    if (!style) return {};
    return computedStyleProperties(*style);
}

// ============================================================================
// Wrappers and node ownership
// ============================================================================

Object* DOMBindings::prototypeFor(const DOMNode* node) const {
    switch (node->nodeType()) {
        case NodeType::Element: {
            auto found = tagInterfaces_.find(lower(static_cast<const DOMElement*>(node)->tagName()));
            return found != tagInterfaces_.end() ? found->second.second : htmlElementProto_;
        }
        case NodeType::Text: return textProto_;
        case NodeType::Comment: return commentProto_;
        case NodeType::Document: return htmlDocumentProto_;
        case NodeType::DocumentFragment: return fragmentProto_;
    }
    return nodeProto_;
}

std::string DOMBindings::interfaceName(const DOMNode* node) const {
    switch (node->nodeType()) {
        case NodeType::Element: {
            auto found = tagInterfaces_.find(lower(static_cast<const DOMElement*>(node)->tagName()));
            return found != tagInterfaces_.end() ? found->second.first : "HTMLElement";
        }
        case NodeType::Text: return "Text";
        case NodeType::Comment: return "Comment";
        case NodeType::Document: return "HTMLDocument";
        case NodeType::DocumentFragment: return "DocumentFragment";
    }
    return "Node";
}

Value DOMBindings::wrap(DOMNode* node) {
    if (!node) return Value::null();
    // The page's bindings are the only ones that bind nodes
    if (NodeBinding* bound = node->binding()) return Value::object(static_cast<NodeWrapper*>(bound));
    auto* w = new NodeWrapper(node);
    w->setPrototype(prototypeFor(node));
    node->setBinding(w);
    wrappers_.push_back(w);
    if (canvases_) {
        if (DOMElement* el = asElement(node); el && lower(el->tagName()) == "canvas") canvases_->decorate(w, el);
    }
    return Value::object(w);
}

DOMNode* DOMBindings::nodeOf(const Object* object) {
    const auto* w = dynamic_cast<const NodeWrapper*>(object);
    return w ? w->node : nullptr;
}

NodeWrapper* DOMBindings::wrapperOf(DOMNode* node) {
    return static_cast<NodeWrapper*>(wrap(node).asObject());
}

DOMNode* DOMBindings::nodeArg(const Info& info, size_t index, const char* method, bool nullable) {
    Value v = info.argument(index);
    if (nullable && v.isNullOrUndefined()) return nullptr;
    auto* w = v.isObject() ? dynamic_cast<NodeWrapper*>(v.asObject()) : nullptr;
    if (!w || !w->node) {
        throw JSError("TypeError", std::string("Failed to execute '") + method + "' on 'Node': parameter " +
                      std::to_string(index + 1) + " is not of type 'Node'.");
    }
    return w->node;
}

// Takes a node out of where it is (its parent, or the wrapper or graveyard
// keeping it) so it can go elsewhere; null for the document or a root the
// bindings do not own
std::unique_ptr<DOMNode> DOMBindings::take(DOMNode* node) {
    if (DOMNode* parent = node->parentNode()) return parent->removeChild(node);
    if (NodeBinding* bound = node->binding()) {
        auto* w = static_cast<NodeWrapper*>(bound);
        if (w->owned.get() == node) return std::move(w->owned);
    }
    for (auto it = graveyard_.begin(); it != graveyard_.end(); ++it) {
        if (it->get() == node) {
            std::unique_ptr<DOMNode> found = std::move(*it);
            graveyard_.erase(it);
            return found;
        }
    }
    return nullptr;
}

// Keeps a removed subtree: in the wrapper of its root when a script can still
// reach a node of it, otherwise until releaseDetachedNodes()
void DOMBindings::park(std::unique_ptr<DOMNode> root) {
    if (!root) return;
    std::function<bool(const DOMNode*)> reachable = [&](const DOMNode* n) {
        if (n->binding()) return true;
        for (const auto& c : n->childNodes()) if (reachable(c.get())) return true;
        return false;
    };
    if (reachable(root.get())) {
        DOMNode* raw = root.get();
        wrapperOf(raw)->owned = std::move(root);
    } else {
        graveyard_.push_back(std::move(root));
    }
}

void DOMBindings::removeAllChildren(DOMNode* parent) {
    while (DOMNode* last = parent->lastChild()) park(parent->removeChild(last));
}

void DOMBindings::insert(DOMNode* parent, DOMNode* child, DOMNode* before, const char* method) {
    auto fail = [method](const std::string& name, const std::string& why) {
        return std::make_pair(name, std::string("Failed to execute '") + method + "' on 'Node': " + why);
    };
    std::pair<std::string, std::string> error;
    if (parent->nodeType() == NodeType::Text || parent->nodeType() == NodeType::Comment) {
        error = fail("HierarchyRequestError", "This node type does not support this method.");
    } else if (child->nodeType() == NodeType::Document) {
        error = fail("HierarchyRequestError", "Nodes of type '#document' may not be inserted inside nodes of type '" +
                     parent->nodeName() + "'.");
    } else if (before && before->parentNode() != parent) {
        error = fail("NotFoundError", "The node before which the new node is to be inserted is not a child of this node.");
    } else {
        for (DOMNode* p = parent; p; p = p->parentNode()) {
            if (p == child) {
                error = fail("HierarchyRequestError", "The new child element contains the parent.");
                break;
            }
        }
    }
    if (!error.first.empty()) throwDOMException(error.first, error.second);

    if (child->nodeType() == NodeType::DocumentFragment) {
        while (DOMNode* first = child->firstChild()) parent->insertBefore(child->removeChild(first), before);
        return;
    }
    if (before == child) before = child->nextSibling();
    std::unique_ptr<DOMNode> owned = take(child);
    if (!owned) throwDOMException("HierarchyRequestError", std::string("Failed to execute '") + method + "' on 'Node': The node cannot be inserted here.");
    parent->insertBefore(std::move(owned), before);
}

// append(), prepend(), before(), after(), replaceWith(): nodes, and strings as text
void DOMBindings::insertNodes(DOMNode* parent, const Info& info, DOMNode* before, const char* method) {
    for (size_t i = 0; i < info.argumentCount(); i++) {
        Value v = info.argument(i);
        if (auto* w = v.isObject() ? dynamic_cast<NodeWrapper*>(v.asObject()) : nullptr) {
            if (!w->node) continue;
            if (w->node == before) before = before->nextSibling();
            insert(parent, w->node, before, method);
        } else {
            if (parent->nodeType() == NodeType::Text || parent->nodeType() == NodeType::Comment) continue;
            parent->insertBefore(std::make_unique<DOMText>(v.toString()), before);
        }
    }
}

std::vector<std::unique_ptr<DOMNode>> DOMBindings::parseFragment(const std::string& html, DOMNode* context) {
    DOMElement* el = asElement(context);
    HTMLParser parser;
    return parser.parseFragmentNodes(html, el ? el->tagName() : "div");
}

void DOMBindings::setText(DOMNode* node, const std::string& text) {
    switch (node->nodeType()) {
        case NodeType::Text:
        case NodeType::Comment:
            node->setNodeValue(text);
            return;
        case NodeType::Document:
            return;
        default:
            break;
    }
    // One text child nobody holds: change it in place
    const auto& kids = node->childNodes();
    if (kids.size() == 1 && kids[0]->nodeType() == NodeType::Text && !kids[0]->binding() && !text.empty()) {
        if (kids[0]->nodeValue() != text) kids[0]->setNodeValue(text);
        return;
    }
    removeAllChildren(node);
    if (!text.empty()) node->appendChild(std::make_unique<DOMText>(text));
}

Value DOMBindings::nodeList(const std::vector<DOMNode*>& nodes) {
    auto* list = new Array();
    for (DOMNode* n : nodes) list->push(wrap(n));
    defineHidden(list, "item", Value::object(listItemFn_));
    return Value::object(list);
}

Value DOMBindings::elementList(const std::vector<DOMElement*>& elements) {
    auto* list = new Array();
    for (DOMElement* e : elements) list->push(wrap(e));
    defineHidden(list, "item", Value::object(listItemFn_));
    return Value::object(list);
}

void DOMBindings::throwDOMException(const std::string& name, const std::string& message) {
    throwDOMError(vm_, name, message);
}

void DOMBindings::releaseDetachedNodes() {
    if (graveyard_.empty()) return;
    // Hover and focus must not point into what goes away
    InteractionState& state = interactionState();
    for (const auto& root : graveyard_) {
        for (DOMElement** p : {&pressed_, &pointer_}) {
            if (*p && root->contains(*p)) *p = nullptr;
        }
        for (DOMElement** p : {&state.hovered, &state.active, &state.focused}) {
            if (*p && root->contains(*p)) *p = nullptr;
        }
        auto& forced = state.forced;
        forced.erase(std::remove_if(forced.begin(), forced.end(),
                                    [&](const InteractionState::ForcedEntry& e) { return root->contains(e.element); }),
                     forced.end());
    }
    graveyard_.clear();
}

// ============================================================================
// Events
// ============================================================================

bool DOMBindings::fireEvent(DOMNode* target, const std::string& type, bool bubbles, bool cancelable) {
    EventObject* e = newEvent(eventProto_, type, Value::undefined());
    e->bubbles = bubbles;
    e->cancelable = cancelable;
    e->trusted = true;
    Object* t = target ? wrap(target).asObject() : global_;
    return dispatch(t, e);
}

bool DOMBindings::fireMouseEvent(DOMElement* target, const std::string& type, double clientX, double clientY) {
    MouseEventInit init;
    init.detail = 1;
    init.buttons = type == "mousedown" ? 1 : 0;
    return fireMouseEvent(target, type, clientX, clientY, init);
}

bool DOMBindings::fireMouseEvent(DOMElement* target, const std::string& type, double clientX, double clientY,
                                 const MouseEventInit& init) {
    if (!target || !isConnectedTo(target, document_)) return true;
    EventObject* e = newEvent(mouseEventProto_, type, Value::undefined());
    e->bubbles = init.bubbles;
    e->cancelable = init.cancelable;
    e->composed = true;
    e->trusted = true;
    // pageX/Y count from the document's origin: the client position plus the page scroll
    double sx = geometry_ ? geometry_->scrollX() : 0, sy = geometry_ ? geometry_->scrollY() : 0;
    for (const char* k : {"clientX", "screenX", "x", "offsetX"}) e->set(k, Value::number(clientX));
    for (const char* k : {"clientY", "screenY", "y", "offsetY"}) e->set(k, Value::number(clientY));
    e->set("pageX", Value::number(clientX + sx));
    e->set("pageY", Value::number(clientY + sy));
    e->set("detail", Value::number(init.detail));
    e->set("button", Value::number(init.button));
    e->set("buttons", Value::number(init.buttons));
    e->set("which", Value::number(init.button + 1));
    for (const char* k : {"altKey", "ctrlKey", "shiftKey", "metaKey"}) e->set(k, Value::boolean(false));
    DOMElement* related = init.relatedTarget && isConnectedTo(init.relatedTarget, document_) ? init.relatedTarget : nullptr;
    e->set("relatedTarget", related ? wrap(related) : Value::null());
    return dispatch(wrap(target).asObject(), e);
}

bool DOMBindings::isConnected(const DOMElement* element) const {
    return element && isConnectedTo(element, document_);
}

bool DOMBindings::click(DOMElement* target, double clientX, double clientY) {
    if (!target || !isConnectedTo(target, document_)) return true;
    if (isDisabledControl(target)) return true;   // disabled controls get no click
    // The element whose activation behavior runs: the target or the
    // nearest ancestor with one
    DOMElement* activator = nullptr;
    for (DOMNode* n = target; n && !activator; n = n->parentNode()) {
        DOMElement* e = asElement(n);
        if (e && (isCheckable(e) || isSubmitButton(e) || isResetButton(e) || lower(e->tagName()) == "label")) activator = e;
    }
    if (activator && isDisabledControl(activator)) activator = nullptr;

    // Checkbox / radio: the new state is visible to click listeners
    bool checkable = activator && isCheckable(activator);
    bool radio = checkable && inputType(activator) == "radio";
    bool wasChecked = checkable && formControlChecked(activator);
    DOMElement* previousRadio = nullptr;
    if (radio) {
        for (DOMElement* r : radioGroup(activator)) if (r != activator && formControlChecked(r)) previousRadio = r;
    }
    if (checkable) setFormControlChecked(activator, radio ? true : !wasChecked);

    bool notCanceled = fireMouseEvent(target, "click", clientX, clientY);
    if (!activator) return notCanceled;
    if (checkable) {
        if (!notCanceled) {
            setFormControlChecked(activator, wasChecked);
            if (previousRadio) setFormControlChecked(previousRadio, true);
        } else if (formControlChecked(activator) != wasChecked) {
            fireEvent(activator, "input", true, false);
            fireEvent(activator, "change", true, false);
        }
        noteDomMutation();
        return notCanceled;
    }
    if (!notCanceled) return false;
    if (lower(activator->tagName()) == "label") {
        // The labeled control: for="id", else the first one inside
        DOMElement* control = nullptr;
        if (activator->hasAttribute("for")) {
            control = document_ ? document_->getElementById(activator->getAttribute("for")) : nullptr;
        } else {
            for (DOMElement* e : selectDescendants(activator, "input, button, select, textarea", true)) control = e;
        }
        if (control && control != target && !control->contains(target)) {
            setFocus(control);
            click(control, clientX, clientY);
        }
        return true;
    }
    if (DOMElement* form = formOwner(activator)) {
        if (isSubmitButton(activator)) requestSubmit(form, activator);
        else resetForm(form);
    }
    return true;
}

bool DOMBindings::fireKeyboardEvent(DOMElement* target, const std::string& type, const KeyEventInit& key) {
    if (!target || !isConnectedTo(target, document_)) return true;
    EventObject* e = newEvent(keyboardEventProto_, type, Value::undefined());
    e->bubbles = e->cancelable = e->composed = true;
    e->trusted = true;
    e->set("key", str(key.key));
    e->set("code", str(key.code));
    e->set("location", Value::number(key.location));
    e->set("repeat", Value::boolean(key.repeat));
    e->set("altKey", Value::boolean(key.altKey));
    e->set("ctrlKey", Value::boolean(key.ctrlKey));
    e->set("shiftKey", Value::boolean(key.shiftKey));
    e->set("metaKey", Value::boolean(key.metaKey));
    // keypress carries the character code in all three legacy members;
    // keydown/keyup carry the key code and no charCode
    int charCode = 0;
    if (type == "keypress") {
        charCode = key.key == "Enter" ? 13 : (key.key.size() == 1 ? static_cast<unsigned char>(key.key[0]) : 0);
    }
    e->set("keyCode", Value::number(type == "keypress" ? charCode : key.keyCode));
    e->set("which", Value::number(type == "keypress" ? charCode : key.keyCode));
    e->set("charCode", Value::number(charCode));
    return dispatch(wrap(target).asObject(), e);
}

bool DOMBindings::fireInputEvent(DOMElement* target, const std::string& type, const std::string& inputType,
                                 const std::string& data) {
    if (!target || !isConnectedTo(target, document_)) return true;
    EventObject* e = newEvent(inputEventProto_, type, Value::undefined());
    e->bubbles = e->composed = true;
    e->cancelable = type == "beforeinput";
    e->trusted = true;
    e->set("inputType", str(inputType));
    e->set("data", data.empty() ? Value::null() : str(data));
    return dispatch(wrap(target).asObject(), e);
}

bool DOMBindings::fireFocusEvent(DOMElement* target, const std::string& type, DOMElement* related) {
    if (!target || !isConnectedTo(target, document_)) return true;
    EventObject* e = newEvent(focusEventProto_, type, Value::undefined());
    // focus and blur stay on the element; focusin and focusout bubble
    e->bubbles = type == "focusin" || type == "focusout";
    e->composed = true;
    e->trusted = true;
    e->set("relatedTarget", related ? wrap(related) : Value::null());
    return dispatch(wrap(target).asObject(), e);
}

DOMElement* DOMBindings::focusedElement() const {
    DOMElement* focused = interactionState().focused;
    return focused && isConnectedTo(focused, document_) ? focused : nullptr;
}

void DOMBindings::setFocus(DOMElement* element) {
    if (element && (!isFocusable(element) || !isConnectedTo(element, document_))) return;
    DOMElement* old = focusedElement();
    if (old == element) return;
    if (old) {
        // The page has focus while the old element hears that it lost it
        interactionState().focused = nullptr;
        if (isTextControl(old) && formControlValue(old) != valueAtFocus_) fireEvent(old, "change", true, false);
        fireFocusEvent(old, "blur", element);
        fireFocusEvent(old, "focusout", element);
        // A listener moved focus somewhere else: that move wins
        if (interactionState().focused) return;
    }
    if (!element || !isConnectedTo(element, document_)) return;
    interactionState().focused = element;
    valueAtFocus_ = formControlValue(element);
    fireFocusEvent(element, "focus", old);
    if (interactionState().focused == element) fireFocusEvent(element, "focusin", old);
}

void DOMBindings::commitFocusedValue() {
    DOMElement* focused = focusedElement();
    if (!focused || !isTextControl(focused) || formControlValue(focused) == valueAtFocus_) return;
    valueAtFocus_ = formControlValue(focused);
    fireEvent(focused, "change", true, false);
}

bool DOMBindings::requestSubmit(DOMElement* form, DOMElement* submitter) {
    if (!form || !isConnectedTo(form, document_)) return false;
    EventObject* e = newEvent(submitEventProto_, "submit", Value::undefined());
    e->bubbles = e->cancelable = true;
    e->trusted = true;
    e->set("submitter", submitter ? wrap(submitter) : Value::null());
    if (!dispatch(wrap(form).asObject(), e)) return false;
    submit(form, submitter);
    return true;
}

void DOMBindings::submit(DOMElement* form, DOMElement* submitter) {
    if (!form || !isConnectedTo(form, document_)) return;
    FormSubmission plan = planFormSubmission(form, submitter);
    if (plan.method == "dialog") return;   // closes a <dialog>: there are none yet
    if (submitHandler_) submitHandler_(plan);
}

bool DOMBindings::resetForm(DOMElement* form) {
    if (!form || !isConnectedTo(form, document_)) return false;
    if (!fireEvent(form, "reset", true, true)) return false;
    resetFormControls(form);
    if (DOMElement* focused = focusedElement(); focused && formOwner(focused) == form) {
        valueAtFocus_ = formControlValue(focused);
    }
    noteDomMutation();
    return true;
}

bool DOMBindings::dispatch(Object* target, EventObject* e) {
    if (e->dispatching) {
        throwDOMException("InvalidStateError", "Failed to execute 'dispatchEvent' on 'EventTarget': The event is already being dispatched.");
    }
    e->dispatching = true;
    e->target = Value::object(target);

    // The path: the target, its ancestors that can have listeners (wrapped,
    // or with an on<type> attribute), and the window above a connected node
    std::vector<Object*> path{target};
    if (auto* w = dynamic_cast<NodeWrapper*>(target); w && w->node) {
        const std::string attr = "on" + e->type;
        DOMNode* root = w->node;
        for (DOMNode* p = w->node->parentNode(); p; p = p->parentNode()) {
            root = p;
            DOMElement* el = asElement(p);
            if (p->binding() || (el && el->hasAttribute(attr))) path.push_back(wrap(p).asObject());
        }
        if (document_ && root == document_ && e->type != "load") path.push_back(global_);
    }
    e->path = path;

    for (size_t i = path.size(); i-- > 1 && !e->stopped;) {
        e->phase = 1;
        invokeListeners(path[i], e, true);
    }
    if (!e->stopped) {
        e->phase = 2;
        invokeListeners(path[0], e, true);
        if (!e->stopped) invokeListeners(path[0], e, false);
    }
    if (e->bubbles) {
        for (size_t i = 1; i < path.size() && !e->stopped; i++) {
            e->phase = 3;
            invokeListeners(path[i], e, false);
        }
    }

    e->phase = 0;
    e->currentTarget = Value::null();
    e->path.clear();
    e->dispatching = false;
    e->stopped = e->stoppedImmediately = false;
    return !e->canceled;
}

void DOMBindings::invokeListeners(Object* target, EventObject* e, bool capturePhase) {
    e->currentTarget = Value::object(target);
    auto* et = dynamic_cast<EventTargetObject*>(target);
    if (et && !et->listeners.empty()) {
        // Listeners added while this runs wait for the next event
        std::vector<std::shared_ptr<Listener>> listeners = et->listeners;
        for (const auto& l : listeners) {
            if (e->stoppedImmediately) return;
            if (l->removed || l->type != e->type || l->capture != capturePhase) continue;
            if (l->once) {
                l->removed = true;
                et->listeners.erase(std::remove(et->listeners.begin(), et->listeners.end(), l), et->listeners.end());
            }
            e->inPassiveListener = l->passive;
            try {
                if (isFunction(l->callback)) {
                    vm_->call(static_cast<Function*>(l->callback.asObject()), Value::object(target), {Value::object(e)});
                } else {
                    Value handle = vm_->getMember(l->callback, "handleEvent");
                    if (isFunction(handle)) {
                        vm_->call(static_cast<Function*>(handle.asObject()), l->callback, {Value::object(e)});
                    }
                }
            } catch (const ThrownException& ex) {
                reportException(ex);
            } catch (const JSError& ex) {
                reportException(ex);
            }
            e->inPassiveListener = false;
        }
    }
    if (!capturePhase && !e->stoppedImmediately) callHandler(target, e, capturePhase);
}

// The on<type> handler: a function property (el.onclick = fn), else the
// element's on<type> attribute compiled to a function. Returning false
// cancels the event.
void DOMBindings::callHandler(Object* target, EventObject* e, bool) {
    const std::string name = "on" + e->type;
    Value handler = Value::undefined();
    if (auto own = target->getOwnPropertyDescriptor(name); own && isFunction(own->value)) handler = own->value;
    if (handler.isUndefined() && target == global_) {
        Value global = vm_->getGlobal(name);
        if (isFunction(global)) handler = global;
    }
    if (handler.isUndefined()) {
        // <body onload="..."> handles the window's load
        DOMElement* source = nullptr;
        NodeWrapper* w = dynamic_cast<NodeWrapper*>(target);
        if (w) source = asElement(w->node);
        else if (target == global_ && e->type == "load" && document_) source = document_->body();
        if (source && source->hasAttribute(name) && compileFunction_) {
            NodeWrapper* holder = w ? w : wrapperOf(source);
            const std::string code = source->getAttribute(name);
            auto& cached = holder->inlineHandlers[name];
            if (cached.first != code || !isFunction(cached.second)) {
                Value fn;
                if (!compileFunction_("(function(event) {\n" + code + "\n})", fn) || !isFunction(fn)) return;
                cached = {code, fn};
            }
            handler = cached.second;
        }
    }
    if (!isFunction(handler)) return;
    try {
        Value result = vm_->call(static_cast<Function*>(handler.asObject()), Value::object(target), {Value::object(e)});
        if (result.isBoolean() && !result.toBoolean() && e->cancelable) e->canceled = true;
    } catch (const ThrownException& ex) {
        reportException(ex);
    } catch (const JSError& ex) {
        reportException(ex);
    }
}

void DOMBindings::reportException(const std::exception& ex) {
    if (reportError_) reportError_(std::string("Uncaught ") + ex.what());
}

} // namespace Zepra::WebCore
