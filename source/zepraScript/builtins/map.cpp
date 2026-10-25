// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file map.cpp
 * @brief JavaScript Map
 */

#include "builtins/map.hpp"
#include "builtins/builtin_helpers.hpp"
#include "builtins/list_iterator.hpp"
#include "runtime/execution/vm.hpp"
#include "runtime/objects/well_known_symbols.hpp"

namespace Zepra::Builtins {

using Runtime::Array;
using Runtime::FunctionCallInfo;
using Runtime::JSError;
using Runtime::VM;

// =============================================================================
// MapObject
// =============================================================================

void MapObject::set(const Value& key, const Value& value) {
    const CollectionKey id = CollectionKey::of(key);
    auto found = index_.find(id);
    if (found != index_.end()) {
        entries_[found->second].value = value;
        return;
    }
    // A -0 key is stored as +0
    Value stored = key;
    if (key.isNumber() && key.asNumber() == 0) stored = Value::number(0);
    index_.emplace(id, entries_.size());
    entries_.push_back({stored, value, true});
    ++live_;
}

Value MapObject::get(const Value& key) const {
    auto found = index_.find(CollectionKey::of(key));
    return found == index_.end() ? Value::undefined() : entries_[found->second].value;
}

bool MapObject::has(const Value& key) const {
    return index_.find(CollectionKey::of(key)) != index_.end();
}

bool MapObject::deleteKey(const Value& key) {
    auto found = index_.find(CollectionKey::of(key));
    if (found == index_.end()) return false;
    Entry& entry = entries_[found->second];
    entry.live = false;
    entry.key = Value::undefined();
    entry.value = Value::undefined();
    index_.erase(found);
    --live_;
    compact();
    return true;
}

void MapObject::clear() {
    if (walking_ > 0) {
        for (Entry& entry : entries_) {
            entry.live = false;
            entry.key = Value::undefined();
            entry.value = Value::undefined();
        }
    } else {
        entries_.clear();
    }
    index_.clear();
    live_ = 0;
}

// Drops the dead entries once they outnumber the live ones, unless a
// forEach is holding positions in entries_
void MapObject::compact() {
    if (walking_ > 0 || entries_.size() < 32 || live_ * 2 >= entries_.size()) return;
    std::vector<Entry> kept;
    kept.reserve(live_);
    index_.clear();
    for (Entry& entry : entries_) {
        if (!entry.live) continue;
        index_.emplace(CollectionKey::of(entry.key), kept.size());
        kept.push_back(std::move(entry));
    }
    entries_ = std::move(kept);
}

std::vector<std::pair<Value, Value>> MapObject::entries() const {
    std::vector<std::pair<Value, Value>> result;
    result.reserve(live_);
    for (const Entry& entry : entries_) {
        if (entry.live) result.emplace_back(entry.key, entry.value);
    }
    return result;
}

std::vector<Value> MapObject::keys() const {
    std::vector<Value> result;
    result.reserve(live_);
    for (const Entry& entry : entries_) {
        if (entry.live) result.push_back(entry.key);
    }
    return result;
}

std::vector<Value> MapObject::values() const {
    std::vector<Value> result;
    result.reserve(live_);
    for (const Entry& entry : entries_) {
        if (entry.live) result.push_back(entry.value);
    }
    return result;
}

void MapObject::forEach(const std::function<void(const Value&, const Value&)>& fn) {
    struct Walk {
        int& depth;
        explicit Walk(int& d) : depth(d) { ++depth; }
        ~Walk() { --depth; }
    } walk(walking_);
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (!entries_[i].live) continue;
        const Value key = entries_[i].key;
        const Value value = entries_[i].value;
        fn(key, value);
    }
}

// =============================================================================
// Map built-ins
// =============================================================================

namespace {

MapObject* thisMap(const FunctionCallInfo& info, const char* method) {
    const Value self = info.thisValue();
    auto* map = self.isObject() ? dynamic_cast<MapObject*>(self.asObject()) : nullptr;
    if (!map) {
        throw JSError("TypeError", std::string("Method Map.prototype.") + method +
                                   " called on incompatible receiver " + self.toString());
    }
    return map;
}

VM& runningVM() {
    VM* vm = VM::current();
    if (!vm) throw JSError("TypeError", "Map operation outside a running script");
    return *vm;
}

Runtime::Function* callbackArgument(const Value& value, const char* method) {
    if (!value.isObject() || !value.asObject()->isFunction()) {
        throw JSError("TypeError", std::string(value.toString()) + " is not a function (Map." + method + ")");
    }
    return static_cast<Runtime::Function*>(value.asObject());
}

std::vector<Value> pair(const Value& key, const Value& value) {
    return {key, value};
}

} // namespace

Object* MapBuiltin::createMapPrototype() {
    auto* prototype = new Object();

    defineMethod(prototype, "get", [](const FunctionCallInfo& info) -> Value {
        return thisMap(info, "get")->get(info.argument(0));
    }, 1);

    defineMethod(prototype, "set", [](const FunctionCallInfo& info) -> Value {
        thisMap(info, "set")->set(info.argument(0), info.argument(1));
        return info.thisValue();
    }, 2);

    defineMethod(prototype, "has", [](const FunctionCallInfo& info) -> Value {
        return Value::boolean(thisMap(info, "has")->has(info.argument(0)));
    }, 1);

    defineMethod(prototype, "delete", [](const FunctionCallInfo& info) -> Value {
        return Value::boolean(thisMap(info, "delete")->deleteKey(info.argument(0)));
    }, 1);

    defineMethod(prototype, "clear", [](const FunctionCallInfo& info) -> Value {
        thisMap(info, "clear")->clear();
        return Value::undefined();
    }, 0);

    defineMethod(prototype, "forEach", [](const FunctionCallInfo& info) -> Value {
        MapObject* map = thisMap(info, "forEach");
        Runtime::Function* callback = callbackArgument(info.argument(0), "forEach");
        const Value thisArg = info.argument(1);
        const Value self = info.thisValue();
        VM& vm = runningVM();
        map->forEach([&](const Value& key, const Value& value) {
            vm.executeCallback(callback, thisArg, {value, key, self});
        });
        return Value::undefined();
    }, 1);

    defineMethod(prototype, "keys", [](const FunctionCallInfo& info) -> Value {
        return IteratorBuiltin::makeListIterator(thisMap(info, "keys")->keys());
    }, 0);

    defineMethod(prototype, "values", [](const FunctionCallInfo& info) -> Value {
        return IteratorBuiltin::makeListIterator(thisMap(info, "values")->values());
    }, 0);

    Function* entries = defineMethod(prototype, "entries", [](const FunctionCallInfo& info) -> Value {
        std::vector<Value> items;
        for (const auto& entry : thisMap(info, "entries")->entries()) {
            items.push_back(Value::object(new Array(pair(entry.first, entry.second))));
        }
        return IteratorBuiltin::makeListIterator(std::move(items));
    }, 0);
    defineHidden(prototype, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::Iterator), Value::object(entries));

    // size is an accessor on the prototype, not an own data property
    Runtime::PropertyDescriptor size;
    size.getter = Value::object(new Function("get size", [](const FunctionCallInfo& info) -> Value {
        return Value::number(static_cast<double>(thisMap(info, "size")->size()));
    }, 0));
    size.attributes = Runtime::PropertyAttribute::Configurable;
    prototype->defineProperty("size", size);

    defineConstant(prototype, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::ToStringTag), makeString("Map"));
    return prototype;
}

Function* MapBuiltin::createMapConstructor(Object* prototype) {
    auto* constructor = new Function("Map", [prototype](const FunctionCallInfo& info) -> Value {
        // `new Map()` arrives with a fresh object inheriting from Map.prototype; a plain call does not
        const Value self = info.thisValue();
        if (!self.isObject() || !inheritsFrom(self.asObject(), prototype)) {
            throw JSError("TypeError", "Constructor Map requires 'new'");
        }
        auto* map = new MapObject();
        map->setPrototype(self.asObject()->prototype());

        const Value iterable = info.argument(0);
        if (!iterable.isUndefined() && !iterable.isNull()) {
            runningVM().forOf(iterable, [&](const Value& entry) {
                if (!entry.isObject()) {
                    throw JSError("TypeError", "Iterator value " + entry.toString() + " is not an entry object");
                }
                map->set(entry.asObject()->get(static_cast<uint32_t>(0)), entry.asObject()->get(static_cast<uint32_t>(1)));
                return true;
            });
        }
        return Value::object(map);
    }, 0);

    defineHidden(constructor, "prototype", Value::object(prototype));
    defineHidden(prototype, "constructor", Value::object(constructor));

    // Map.groupBy(items, callback): key -> array of the items that produced it
    defineMethod(constructor, "groupBy", [prototype](const FunctionCallInfo& info) -> Value {
        Runtime::Function* callback = callbackArgument(info.argument(1), "groupBy");
        VM& vm = runningVM();
        auto* groups = new MapObject();
        groups->setPrototype(prototype);
        double index = 0;
        vm.forOf(info.argument(0), [&](const Value& item) {
            const Value key = vm.executeCallback(callback, Value::undefined(), {item, Value::number(index++)});
            Value group = groups->get(key);
            if (!group.isObject()) {
                group = Value::object(new Array());
                groups->set(key, group);
            }
            static_cast<Array*>(group.asObject())->push(item);
            return true;
        });
        return Value::object(groups);
    }, 2);
    return constructor;
}

} // namespace Zepra::Builtins
