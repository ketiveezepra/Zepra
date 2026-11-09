// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file custom_elements.hpp
 * @brief Custom elements (HTML §4.13): window.customElements, the
 *        constructible HTMLElement, upgrades and lifecycle callbacks.
 *
 * A definition maps a name to an author class. Elements of that name made
 * after define() are built by the class (document.createElement('x-a') and
 * `new XA()` alike); elements that existed before are *upgraded*: the class
 * constructor runs on the existing element, whose `super()` hands back the
 * element itself (the definition's construction stack).
 *
 * Lifecycle callbacks (connected, disconnected, attributeChanged, adopted)
 * and upgrades are never run in the middle of a DOM change. They go on a
 * per-element reaction queue, and the element on the current element queue:
 * every DOM method the page calls opens one (CEReactionsScope, the spec's
 * [CEReactions]) and runs what was queued when it returns. Changes made
 * outside any DOM call (the browser itself) use the backup queue, run at the
 * next microtask checkpoint.
 */

#pragma once

#include "browser/dom.hpp"
#include "runtime/objects/value.hpp"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Zepra::Runtime { class Object; class Function; class VM; class FunctionCallInfo; class Promise; }

namespace Zepra::WebCore {

class DOMBindings;

/// [CEReactions]: opens an element queue; leave() runs the reactions
/// queued on it (also when the call threw). Nests.
class CEReactionsScope {
public:
    CEReactionsScope();
    ~CEReactionsScope();
    void leave();
private:
    bool open_ = true;
};

struct CustomElementDefinition {
    std::string name;
    std::string localName;
    Zepra::Runtime::Function* constructor = nullptr;
    Zepra::Runtime::Object* prototype = nullptr;
    Zepra::Runtime::Value connectedCallback;
    Zepra::Runtime::Value disconnectedCallback;
    Zepra::Runtime::Value adoptedCallback;
    Zepra::Runtime::Value attributeChangedCallback;
    std::unordered_set<std::string> observedAttributes;
    /// Elements being upgraded by this definition; null once `super()`
    /// has returned one (the "already constructed" marker)
    std::vector<DOMElement*> constructionStack;
};

class CustomElements {
public:
    CustomElements(DOMBindings& bindings, Zepra::Runtime::VM* vm);
    ~CustomElements();
    CustomElements(const CustomElements&) = delete;
    CustomElements& operator=(const CustomElements&) = delete;

    /// customElements, CustomElementRegistry and the HTMLElement constructor
    void install(Zepra::Runtime::Object* window, Zepra::Runtime::Object* htmlElementProto);

    /// The sink to set on the document: reacts to connections, removals and
    /// attribute changes, then passes everything on to `next`
    DOMMutationSink* sink(DOMMutationSink* next);

    /// The definition for a local name, or null
    std::shared_ptr<CustomElementDefinition> lookup(const std::string& localName) const;

    /// document.createElement() of a defined name: runs the class's
    /// constructor (synchronous custom elements flag). Empty value when the
    /// name has no definition.
    Zepra::Runtime::Value create(const std::string& localName);

    /// Queues an upgrade for every element below (and at) root that has a
    /// definition and is not upgraded yet (cloneNode, importNode)
    void enqueueUpgrades(DOMNode* root);
    /// customElements.upgrade(root): upgrades them now
    void upgradeTree(DOMNode* root);

    /// Upgrades one element with a definition (runs its constructor)
    void upgrade(DOMElement* element, const std::shared_ptr<CustomElementDefinition>& definition);
    void report(const std::exception& e);

    Zepra::Runtime::VM* vm() const { return vm_; }

private:
    class Sink;
    DOMBindings& bindings_;
    Zepra::Runtime::VM* vm_;
    std::unique_ptr<Sink> sink_;
    std::unordered_map<std::string, std::shared_ptr<CustomElementDefinition>> definitions_;
    std::unordered_map<std::string, Zepra::Runtime::Promise*> whenDefined_;
    bool definitionRunning_ = false;

    Zepra::Runtime::Value define(const Zepra::Runtime::FunctionCallInfo& info);
    Zepra::Runtime::Value constructHTMLElement(const Zepra::Runtime::FunctionCallInfo& info);
    bool connected(const DOMNode* node) const;
    void connectedSubtree(DOMNode* root);
    void disconnectedSubtree(DOMNode* root);
    void attributeChanged(DOMElement* element, const std::string& name, const std::string* oldValue);
    void tryToUpgrade(DOMElement* element);
    void enqueueCallback(DOMElement* element, const Zepra::Runtime::Value& callback,
                         std::vector<Zepra::Runtime::Value> args);
};

/// Calls `visit` on every element at and below root in shadow-including tree
/// order (shadow trees right after their host; template contents skipped)
void forEachShadowIncludingElement(DOMNode* root, const std::function<void(DOMElement*)>& visit);

} // namespace Zepra::WebCore
