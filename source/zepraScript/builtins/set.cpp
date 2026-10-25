// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file set.cpp
 * @brief JavaScript Set and WeakSet
 */

#include "builtins/set.hpp"
#include "builtins/builtin_helpers.hpp"
#include "builtins/list_iterator.hpp"
#include "runtime/execution/vm.hpp"
#include "runtime/objects/well_known_symbols.hpp"

#include <cmath>

namespace Zepra::Builtins {

using Runtime::Array;
using Runtime::FunctionCallInfo;
using Runtime::JSError;
using Runtime::VM;

// =============================================================================
// SetObject
// =============================================================================

void SetObject::add(const Value& value) {
    const CollectionKey id = CollectionKey::of(value);
    if (index_.find(id) != index_.end()) return;
    // A -0 member is stored as +0
    Value stored = value;
    if (value.isNumber() && value.asNumber() == 0) stored = Value::number(0);
    index_.emplace(id, entries_.size());
    entries_.push_back({stored, true});
    ++live_;
}

bool SetObject::has(const Value& value) const {
    return index_.find(CollectionKey::of(value)) != index_.end();
}

bool SetObject::deleteValue(const Value& value) {
    auto found = index_.find(CollectionKey::of(value));
    if (found == index_.end()) return false;
    Entry& entry = entries_[found->second];
    entry.live = false;
    entry.value = Value::undefined();
    index_.erase(found);
    --live_;
    compact();
    return true;
}

void SetObject::clear() {
    if (walking_ > 0) {
        for (Entry& entry : entries_) {
            entry.live = false;
            entry.value = Value::undefined();
        }
    } else {
        entries_.clear();
    }
    index_.clear();
    live_ = 0;
}

void SetObject::compact() {
    if (walking_ > 0 || entries_.size() < 32 || live_ * 2 >= entries_.size()) return;
    std::vector<Entry> kept;
    kept.reserve(live_);
    index_.clear();
    for (Entry& entry : entries_) {
        if (!entry.live) continue;
        index_.emplace(CollectionKey::of(entry.value), kept.size());
        kept.push_back(std::move(entry));
    }
    entries_ = std::move(kept);
}

std::vector<Value> SetObject::values() const {
    std::vector<Value> result;
    result.reserve(live_);
    for (const Entry& entry : entries_) {
        if (entry.live) result.push_back(entry.value);
    }
    return result;
}

void SetObject::forEach(const std::function<void(const Value&)>& fn) {
    struct Walk {
        int& depth;
        explicit Walk(int& d) : depth(d) { ++depth; }
        ~Walk() { --depth; }
    } walk(walking_);
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (!entries_[i].live) continue;
        const Value value = entries_[i].value;
        fn(value);
    }
}

// =============================================================================
// Set built-ins
// =============================================================================

namespace {

VM& runningVM() {
    VM* vm = VM::current();
    if (!vm) throw JSError("TypeError", "Set operation outside a running script");
    return *vm;
}

SetObject* thisSet(const FunctionCallInfo& info, const char* method) {
    const Value self = info.thisValue();
    auto* set = self.isObject() ? dynamic_cast<SetObject*>(self.asObject()) : nullptr;
    if (!set) {
        throw JSError("TypeError", std::string("Method Set.prototype.") + method +
                                   " called on incompatible receiver " + self.toString());
    }
    return set;
}

Runtime::Function* callbackArgument(const Value& value) {
    if (!value.isObject() || !value.asObject()->isFunction()) {
        throw JSError("TypeError", value.toString() + " is not a function");
    }
    return static_cast<Runtime::Function*>(value.asObject());
}

// A Set or any set-like object: a size, has() and keys() (GetSetRecord)
struct SetRecord {
    Value object;
    double size = 0;
    Runtime::Function* hasFunction = nullptr;
    Runtime::Function* keysFunction = nullptr;
    SetObject* native = nullptr;  // fast path when `object` is a real Set

    explicit SetRecord(const Value& other) : object(other) {
        if (!other.isObject()) throw JSError("TypeError", "The argument must be set-like, got " + other.toString());
        VM& vm = runningVM();
        const double rawSize = vm.getMember(other, "size").toNumber();
        if (std::isnan(rawSize)) throw JSError("TypeError", "The 'size' property must be a number");
        size = std::trunc(rawSize);
        if (size < 0) throw JSError("RangeError", "'" + std::to_string(size) + "' is an invalid size");
        native = dynamic_cast<SetObject*>(other.asObject());
        if (!native) {
            hasFunction = callbackArgument(vm.getMember(other, "has"));
            keysFunction = callbackArgument(vm.getMember(other, "keys"));
        }
    }

    bool contains(const Value& value) const {
        if (native) return native->has(value);
        return runningVM().executeCallback(hasFunction, object, {value}).toBoolean();
    }

    // Calls fn for each key until it returns false
    void forEachKey(const std::function<bool(const Value&)>& fn) const {
        if (native) {
            for (const Value& value : native->values()) {
                if (!fn(value)) return;
            }
            return;
        }
        VM& vm = runningVM();
        vm.forOf(vm.executeCallback(keysFunction, object, {}), fn);
    }
};

SetObject* newSetLike(const SetObject* source) {
    auto* set = new SetObject();
    set->setPrototype(source->prototype());
    return set;
}

} // namespace

Object* SetBuiltin::createSetPrototype() {
    auto* prototype = new Object();

    defineMethod(prototype, "add", [](const FunctionCallInfo& info) -> Value {
        thisSet(info, "add")->add(info.argument(0));
        return info.thisValue();
    }, 1);

    defineMethod(prototype, "has", [](const FunctionCallInfo& info) -> Value {
        return Value::boolean(thisSet(info, "has")->has(info.argument(0)));
    }, 1);

    defineMethod(prototype, "delete", [](const FunctionCallInfo& info) -> Value {
        return Value::boolean(thisSet(info, "delete")->deleteValue(info.argument(0)));
    }, 1);

    defineMethod(prototype, "clear", [](const FunctionCallInfo& info) -> Value {
        thisSet(info, "clear")->clear();
        return Value::undefined();
    }, 0);

    defineMethod(prototype, "forEach", [](const FunctionCallInfo& info) -> Value {
        SetObject* set = thisSet(info, "forEach");
        Runtime::Function* callback = callbackArgument(info.argument(0));
        const Value thisArg = info.argument(1);
        const Value self = info.thisValue();
        VM& vm = runningVM();
        set->forEach([&](const Value& value) { vm.executeCallback(callback, thisArg, {value, value, self}); });
        return Value::undefined();
    }, 1);

    Function* values = defineMethod(prototype, "values", [](const FunctionCallInfo& info) -> Value {
        return IteratorBuiltin::makeListIterator(thisSet(info, "values")->values());
    }, 0);
    defineHidden(prototype, "keys", Value::object(values));
    defineHidden(prototype, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::Iterator), Value::object(values));

    defineMethod(prototype, "entries", [](const FunctionCallInfo& info) -> Value {
        std::vector<Value> items;
        for (const Value& value : thisSet(info, "entries")->values()) {
            items.push_back(Value::object(new Array(std::vector<Value>{value, value})));
        }
        return IteratorBuiltin::makeListIterator(std::move(items));
    }, 0);

    Runtime::PropertyDescriptor size;
    size.getter = Value::object(new Function("get size", [](const FunctionCallInfo& info) -> Value {
        return Value::number(static_cast<double>(thisSet(info, "size")->size()));
    }, 0));
    size.attributes = Runtime::PropertyAttribute::Configurable;
    prototype->defineProperty("size", size);

    // ---- set algebra (ES2025) ----

    defineMethod(prototype, "union", [](const FunctionCallInfo& info) -> Value {
        SetObject* self = thisSet(info, "union");
        SetRecord other(info.argument(0));
        SetObject* result = newSetLike(self);
        for (const Value& value : self->values()) result->add(value);
        other.forEachKey([&](const Value& key) { result->add(key); return true; });
        return Value::object(result);
    }, 1);

    defineMethod(prototype, "intersection", [](const FunctionCallInfo& info) -> Value {
        SetObject* self = thisSet(info, "intersection");
        SetRecord other(info.argument(0));
        SetObject* result = newSetLike(self);
        if (static_cast<double>(self->size()) <= other.size) {
            for (const Value& value : self->values()) {
                if (other.contains(value)) result->add(value);
            }
        } else {
            other.forEachKey([&](const Value& key) {
                if (self->has(key)) result->add(key);
                return true;
            });
        }
        return Value::object(result);
    }, 1);

    defineMethod(prototype, "difference", [](const FunctionCallInfo& info) -> Value {
        SetObject* self = thisSet(info, "difference");
        SetRecord other(info.argument(0));
        SetObject* result = newSetLike(self);
        for (const Value& value : self->values()) result->add(value);
        if (static_cast<double>(self->size()) <= other.size) {
            for (const Value& value : self->values()) {
                if (other.contains(value)) result->deleteValue(value);
            }
        } else {
            other.forEachKey([&](const Value& key) { result->deleteValue(key); return true; });
        }
        return Value::object(result);
    }, 1);

    defineMethod(prototype, "symmetricDifference", [](const FunctionCallInfo& info) -> Value {
        SetObject* self = thisSet(info, "symmetricDifference");
        SetRecord other(info.argument(0));
        SetObject* result = newSetLike(self);
        for (const Value& value : self->values()) result->add(value);
        other.forEachKey([&](const Value& key) {
            if (self->has(key)) result->deleteValue(key);
            else result->add(key);
            return true;
        });
        return Value::object(result);
    }, 1);

    defineMethod(prototype, "isSubsetOf", [](const FunctionCallInfo& info) -> Value {
        SetObject* self = thisSet(info, "isSubsetOf");
        SetRecord other(info.argument(0));
        if (static_cast<double>(self->size()) > other.size) return Value::boolean(false);
        for (const Value& value : self->values()) {
            if (!other.contains(value)) return Value::boolean(false);
        }
        return Value::boolean(true);
    }, 1);

    defineMethod(prototype, "isSupersetOf", [](const FunctionCallInfo& info) -> Value {
        SetObject* self = thisSet(info, "isSupersetOf");
        SetRecord other(info.argument(0));
        if (static_cast<double>(self->size()) < other.size) return Value::boolean(false);
        bool all = true;
        other.forEachKey([&](const Value& key) {
            if (!self->has(key)) all = false;
            return all;
        });
        return Value::boolean(all);
    }, 1);

    defineMethod(prototype, "isDisjointFrom", [](const FunctionCallInfo& info) -> Value {
        SetObject* self = thisSet(info, "isDisjointFrom");
        SetRecord other(info.argument(0));
        bool disjoint = true;
        if (static_cast<double>(self->size()) <= other.size) {
            for (const Value& value : self->values()) {
                if (other.contains(value)) return Value::boolean(false);
            }
        } else {
            other.forEachKey([&](const Value& key) {
                if (self->has(key)) disjoint = false;
                return disjoint;
            });
        }
        return Value::boolean(disjoint);
    }, 1);

    defineConstant(prototype, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::ToStringTag), makeString("Set"));
    return prototype;
}

Function* SetBuiltin::createSetConstructor(Object* prototype) {
    auto* constructor = new Function("Set", [prototype](const FunctionCallInfo& info) -> Value {
        const Value self = info.thisValue();
        if (!self.isObject() || !inheritsFrom(self.asObject(), prototype)) {
            throw JSError("TypeError", "Constructor Set requires 'new'");
        }
        auto* set = new SetObject();
        set->setPrototype(self.asObject()->prototype());

        const Value iterable = info.argument(0);
        if (!iterable.isUndefined() && !iterable.isNull()) {
            runningVM().forOf(iterable, [&](const Value& value) { set->add(value); return true; });
        }
        return Value::object(set);
    }, 0);
    defineHidden(constructor, "prototype", Value::object(prototype));
    defineHidden(prototype, "constructor", Value::object(constructor));
    return constructor;
}

// =============================================================================
// WeakSet
// =============================================================================

namespace {

WeakSetObject* thisWeakSet(const FunctionCallInfo& info, const char* method) {
    const Value self = info.thisValue();
    auto* set = self.isObject() ? dynamic_cast<WeakSetObject*>(self.asObject()) : nullptr;
    if (!set) {
        throw JSError("TypeError", std::string("Method WeakSet.prototype.") + method +
                                   " called on incompatible receiver " + self.toString());
    }
    return set;
}

} // namespace

Object* SetBuiltin::createWeakSetPrototype() {
    auto* prototype = new Object();

    defineMethod(prototype, "add", [](const FunctionCallInfo& info) -> Value {
        WeakSetObject* set = thisWeakSet(info, "add");
        if (!info.argument(0).isObject()) {
            throw JSError("TypeError", "Invalid value used in weak set");
        }
        set->add(info.argument(0).asObject());
        return info.thisValue();
    }, 1);
    defineMethod(prototype, "has", [](const FunctionCallInfo& info) -> Value {
        WeakSetObject* set = thisWeakSet(info, "has");
        return Value::boolean(info.argument(0).isObject() && set->has(info.argument(0).asObject()));
    }, 1);
    defineMethod(prototype, "delete", [](const FunctionCallInfo& info) -> Value {
        WeakSetObject* set = thisWeakSet(info, "delete");
        return Value::boolean(info.argument(0).isObject() && set->deleteValue(info.argument(0).asObject()));
    }, 1);
    defineConstant(prototype, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::ToStringTag), makeString("WeakSet"));
    return prototype;
}

Function* SetBuiltin::createWeakSetConstructor(Object* prototype) {
    auto* constructor = new Function("WeakSet", [prototype](const FunctionCallInfo& info) -> Value {
        const Value self = info.thisValue();
        if (!self.isObject() || !inheritsFrom(self.asObject(), prototype)) {
            throw JSError("TypeError", "Constructor WeakSet requires 'new'");
        }
        auto* set = new WeakSetObject();
        set->setPrototype(self.asObject()->prototype());

        const Value iterable = info.argument(0);
        if (!iterable.isUndefined() && !iterable.isNull()) {
            runningVM().forOf(iterable, [&](const Value& value) {
                if (!value.isObject()) throw JSError("TypeError", "Invalid value used in weak set");
                set->add(value.asObject());
                return true;
            });
        }
        return Value::object(set);
    }, 0);
    defineHidden(constructor, "prototype", Value::object(prototype));
    defineHidden(prototype, "constructor", Value::object(constructor));
    return constructor;
}

} // namespace Zepra::Builtins
