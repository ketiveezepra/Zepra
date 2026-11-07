// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file mutation_observer.cpp
 * @brief MutationObserver and MutationRecord (see mutation_observer.hpp)
 */

#include "scripting/mutation_observer.hpp"
#include "scripting/dom_bindings.hpp"

#include "runtime/execution/vm.hpp"
#include "runtime/execution/js_error.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"

#include <algorithm>

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

Value nodeArray(DOMBindings& bindings, DOMNode* node) {
    auto* list = new Array();
    if (node) list->push(bindings.wrap(node));
    return Value::object(list);
}

}  // namespace

MutationObservers::MutationObservers(VM* vm, DOMBindings& bindings, ErrorReporter report)
    : vm_(vm), bindings_(bindings), report_(std::move(report)) {}

MutationObservers::~MutationObservers() {
    *alive_ = false;
    DOMDocument::removeMutationSink(this);
}

void MutationObservers::install(Object* global) {
    // MutationRecord: made only by the browser
    recordProto_ = new Object();
    recordProto_->setPrototype(vm_->intrinsic(VM::Intrinsic::Object));
    auto* recordCtor = new Function("MutationRecord", [](const FunctionCallInfo&) -> Value {
        throw JSError("TypeError", "Illegal constructor: MutationRecord");
    }, 0);
    recordCtor->set("prototype", Value::object(recordProto_));
    defineHidden(recordProto_, "constructor", Value::object(recordCtor));

    auto* proto = new Object();
    proto->setPrototype(vm_->intrinsic(VM::Intrinsic::Object));
    auto* ctor = new Function("MutationObserver", [this, proto](const FunctionCallInfo& info) -> Value {
        Value callback = info.argument(0);
        if (!callback.isObject() || !callback.asObject()->isFunction()) {
            throw JSError("TypeError", "Failed to construct 'MutationObserver': parameter 1 is not of type "
                                       "'MutationCallback'.");
        }
        auto observer = std::make_unique<Observer>();
        observer->object = new Object();
        observer->object->setPrototype(proto);
        observer->callback = static_cast<Function*>(callback.asObject());
        byObject_[observer->object] = observer.get();
        Value result = Value::object(observer->object);
        observers_.push_back(std::move(observer));
        return result;
    }, 1);
    ctor->set("prototype", Value::object(proto));
    defineHidden(proto, "constructor", Value::object(ctor));

    defineHidden(proto, "observe", Value::object(new Function("observe", [this](const FunctionCallInfo& info) -> Value {
        Observer* observer = observerOf(info.thisValue());
        if (!observer) throw JSError("TypeError", "Illegal invocation");
        Value target = info.argument(0);
        DOMNode* node = target.isObject() ? DOMBindings::nodeOf(target.asObject()) : nullptr;
        if (!node) {
            throw JSError("TypeError", "Failed to execute 'observe' on 'MutationObserver': parameter 1 is not of "
                                       "type 'Node'.");
        }
        observe(*observer, node, info.argument(1));
        return Value::undefined();
    }, 2)));
    defineHidden(proto, "disconnect", Value::object(new Function("disconnect", [this](const FunctionCallInfo& info) -> Value {
        if (Observer* observer = observerOf(info.thisValue())) {
            observer->registrations.clear();
            observer->records.clear();
        }
        return Value::undefined();
    }, 0)));
    defineHidden(proto, "takeRecords", Value::object(new Function("takeRecords", [this](const FunctionCallInfo& info) -> Value {
        auto* list = new Array();
        if (Observer* observer = observerOf(info.thisValue())) {
            for (const Value& record : observer->records) list->push(record);
            observer->records.clear();
        }
        return Value::object(list);
    }, 0)));

    for (const auto& [name, value] : {std::pair<const char*, Object*>{"MutationObserver", ctor},
                                      {"MutationRecord", recordCtor}}) {
        defineHidden(global, name, Value::object(value));
        vm_->setGlobal(name, Value::object(value));
    }
    // Old WebKit name some libraries still look for
    defineHidden(global, "WebKitMutationObserver", Value::object(ctor));
    vm_->setGlobal("WebKitMutationObserver", Value::object(ctor));
}

MutationObservers::Observer* MutationObservers::observerOf(const Value& thisValue) {
    if (!thisValue.isObject()) return nullptr;
    auto it = byObject_.find(thisValue.asObject());
    return it != byObject_.end() ? it->second : nullptr;
}

void MutationObservers::observe(Observer& observer, DOMNode* target, const Value& init) {
    Options options;
    auto member = [&](const char* name) { return init.isObject() ? vm_->getMember(init, name) : Value::undefined(); };
    auto flag = [&](const char* name, bool& out) {
        Value v = member(name);
        if (v.isUndefined()) return false;
        out = v.toBoolean();
        return true;
    };
    flag("childList", options.childList);
    flag("subtree", options.subtree);
    bool attributesGiven = flag("attributes", options.attributes);
    bool characterDataGiven = flag("characterData", options.characterData);
    flag("attributeOldValue", options.attributeOldValue);
    flag("characterDataOldValue", options.characterDataOldValue);
    Value filter = member("attributeFilter");
    if (!filter.isUndefined()) {
        options.hasFilter = true;
        if (filter.isObject()) {
            double length = vm_->getMember(filter, "length").toNumber();
            for (size_t i = 0; i < static_cast<size_t>(length); i++) {
                options.attributeFilter.push_back(filter.asObject()->get(static_cast<uint32_t>(i)).toString());
            }
        }
    }
    // attributeOldValue / attributeFilter imply attributes, characterDataOldValue characterData
    if (!attributesGiven && (options.attributeOldValue || options.hasFilter)) options.attributes = true;
    if (!characterDataGiven && options.characterDataOldValue) options.characterData = true;

    const char* prefix = "Failed to execute 'observe' on 'MutationObserver': ";
    if (!options.childList && !options.attributes && !options.characterData) {
        throw JSError("TypeError", std::string(prefix) + "The options object must set at least one of "
                                   "'attributes', 'characterData', or 'childList' to true.");
    }
    if (options.attributeOldValue && !options.attributes) {
        throw JSError("TypeError", std::string(prefix) + "The options object may only set 'attributeOldValue' "
                                   "to true when 'attributes' is true or not present.");
    }
    if (options.hasFilter && !options.attributes) {
        throw JSError("TypeError", std::string(prefix) + "The options object may only set 'attributeFilter' "
                                   "when 'attributes' is true or not present.");
    }
    if (options.characterDataOldValue && !options.characterData) {
        throw JSError("TypeError", std::string(prefix) + "The options object may only set "
                                   "'characterDataOldValue' to true when 'characterData' is true or not present.");
    }

    // Observing the same node again replaces its options
    for (Registration& r : observer.registrations) {
        if (r.target == target) {
            r.options = std::move(options);
            return;
        }
    }
    observer.registrations.push_back({target, std::move(options)});
}

void MutationObservers::forEachInterested(DOMNode* node, const std::function<bool(const Options&)>& wants,
                                          const std::function<void(Observer&, const Options&)>& match) {
    for (const auto& observer : observers_) {
        for (const Registration& r : observer->registrations) {
            bool covers = false;
            for (DOMNode* n = node; n; n = n->parentNode()) {
                if (n == r.target) {
                    covers = n == node || r.options.subtree;
                    break;
                }
            }
            if (covers && wants(r.options)) {
                match(*observer, r.options);
                break;
            }
        }
    }
}

Object* MutationObservers::newRecord(const std::string& type, DOMNode* target) {
    auto* record = new Object();
    record->setPrototype(recordProto_);
    record->set("type", str(type));
    record->set("target", bindings_.wrap(target));
    record->set("addedNodes", Value::object(new Array()));
    record->set("removedNodes", Value::object(new Array()));
    record->set("previousSibling", Value::null());
    record->set("nextSibling", Value::null());
    record->set("attributeName", Value::null());
    record->set("attributeNamespace", Value::null());
    record->set("oldValue", Value::null());
    return record;
}

void MutationObservers::enqueue(Observer& observer, Object* record) {
    observer.records.push_back(Value::object(record));
    if (deliveryQueued_) return;
    deliveryQueued_ = true;
    std::weak_ptr<bool> alive = alive_;
    vm_->enqueueJob([this, alive] {
        if (auto a = alive.lock(); a && *a) deliver();
    });
}

void MutationObservers::deliver() {
    deliveryQueued_ = false;
    // Callbacks may observe, mutate and queue more records: those go in the
    // next delivery, which their own enqueue schedules
    std::vector<Observer*> order;
    for (const auto& observer : observers_) order.push_back(observer.get());
    for (Observer* observer : order) {
        if (observer->records.empty()) continue;
        auto* list = new Array();
        for (const Value& record : observer->records) list->push(record);
        observer->records.clear();
        try {
            vm_->call(observer->callback, Value::object(observer->object),
                      {Value::object(list), Value::object(observer->object)});
        } catch (const ThrownException& ex) {
            if (report_) report_(ex);
        } catch (const JSError& ex) {
            if (report_) report_(ex);
        }
    }
}

void MutationObservers::childList(DOMNode* parent, DOMNode* added, DOMNode* removed,
                                  DOMNode* previousSibling, DOMNode* nextSibling) {
    if (observers_.empty()) return;
    forEachInterested(parent, [](const Options& o) { return o.childList; }, [&](Observer& observer, const Options&) {
        Object* record = newRecord("childList", parent);
        // Wrapping a removed node also keeps it alive for the record
        record->set("addedNodes", nodeArray(bindings_, added));
        record->set("removedNodes", nodeArray(bindings_, removed));
        record->set("previousSibling", bindings_.wrap(previousSibling));
        record->set("nextSibling", bindings_.wrap(nextSibling));
        enqueue(observer, record);
    });
}

void MutationObservers::attribute(DOMElement* element, const std::string& name, const std::string* oldValue) {
    if (observers_.empty()) return;
    forEachInterested(element, [&](const Options& o) {
        return o.attributes && (!o.hasFilter || std::find(o.attributeFilter.begin(), o.attributeFilter.end(), name) !=
                                                    o.attributeFilter.end());
    }, [&](Observer& observer, const Options& o) {
        Object* record = newRecord("attributes", element);
        record->set("attributeName", str(name));
        if (o.attributeOldValue && oldValue) record->set("oldValue", str(*oldValue));
        enqueue(observer, record);
    });
}

void MutationObservers::characterData(DOMNode* node, const std::string& oldValue) {
    if (observers_.empty()) return;
    forEachInterested(node, [](const Options& o) { return o.characterData; }, [&](Observer& observer, const Options& o) {
        Object* record = newRecord("characterData", node);
        if (o.characterDataOldValue) record->set("oldValue", str(oldValue));
        enqueue(observer, record);
    });
}

} // namespace Zepra::WebCore
