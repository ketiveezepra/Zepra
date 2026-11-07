// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file mutation_observer.hpp
 * @brief MutationObserver for page scripts: the document reports each change
 *        to its tree (DOMMutationSink), observers whose registrations cover
 *        the changed node get a MutationRecord, and the records reach their
 *        callbacks in a microtask, a batch per observer.
 *
 * Not supported: transient registrations (changes inside a subtree after it
 * was removed from an observed node), attribute namespaces.
 */

#pragma once

#include "browser/dom.hpp"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Zepra::Runtime { class Object; class Function; class Value; class VM; }

namespace Zepra::WebCore {

class DOMBindings;

class MutationObservers final : public DOMMutationSink {
public:
    using ErrorReporter = std::function<void(const std::exception&)>;
    MutationObservers(Zepra::Runtime::VM* vm, DOMBindings& bindings, ErrorReporter report);
    ~MutationObservers() override;

    /// Defines MutationObserver and MutationRecord on the global object
    void install(Zepra::Runtime::Object* global);

    void childList(DOMNode* parent, DOMNode* added, DOMNode* removed,
                   DOMNode* previousSibling, DOMNode* nextSibling) override;
    void attribute(DOMElement* element, const std::string& name, const std::string* oldValue) override;
    void characterData(DOMNode* node, const std::string& oldValue) override;

private:
    struct Options {
        bool childList = false, attributes = false, characterData = false, subtree = false;
        bool attributeOldValue = false, characterDataOldValue = false;
        bool hasFilter = false;
        std::vector<std::string> attributeFilter;
    };
    struct Registration {
        DOMNode* target;   // compared, never followed: the node may be gone
        Options options;
    };
    struct Observer {
        Zepra::Runtime::Object* object = nullptr;
        Zepra::Runtime::Function* callback = nullptr;
        std::vector<Registration> registrations;
        std::vector<Zepra::Runtime::Value> records;
    };

    Observer* observerOf(const Zepra::Runtime::Value& thisValue);
    void observe(Observer& observer, DOMNode* target, const Zepra::Runtime::Value& options);
    /// Calls `match(observer, options)` for each observer with a registration
    /// covering `node`, once per observer (its first matching registration)
    void forEachInterested(DOMNode* node, const std::function<bool(const Options&)>& wants,
                           const std::function<void(Observer&, const Options&)>& match);
    Zepra::Runtime::Object* newRecord(const std::string& type, DOMNode* target);
    void enqueue(Observer& observer, Zepra::Runtime::Object* record);
    void deliver();

    Zepra::Runtime::VM* vm_;
    DOMBindings& bindings_;
    ErrorReporter report_;
    Zepra::Runtime::Object* recordProto_ = nullptr;
    std::vector<std::unique_ptr<Observer>> observers_;   // in creation order (delivery order)
    std::unordered_map<const Zepra::Runtime::Object*, Observer*> byObject_;
    bool deliveryQueued_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace Zepra::WebCore
