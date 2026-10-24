#pragma once

/**
 * @file set.hpp
 * @brief JavaScript Set and WeakSet
 */

#include "../config.hpp"
#include "builtins/collection_key.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Zepra::Builtins {

using Runtime::Function;
using Runtime::Object;
using Runtime::ObjectType;
using Runtime::Value;

/**
 * @brief A Set: insertion-ordered values with O(1) lookup by SameValueZero
 */
class SetObject : public Object {
public:
    SetObject() : Object(ObjectType::Set) {}

    void add(const Value& value);
    bool has(const Value& value) const;
    bool deleteValue(const Value& value);
    void clear();
    size_t size() const { return live_; }

    /** Snapshot of the live values in insertion order. */
    std::vector<Value> values() const;

    /** Calls `fn(value)` for every value, including ones `fn` adds and skipping ones it deletes. */
    void forEach(const std::function<void(const Value&)>& fn);

    size_t length() const override { return live_; }

private:
    struct Entry {
        Value value;
        bool live = true;
    };

    void compact();

    std::vector<Entry> entries_;
    std::unordered_map<CollectionKey, size_t, CollectionKeyHash> index_;
    size_t live_ = 0;
    int walking_ = 0;
};

/**
 * @brief A WeakSet. Without a collector the members are simply held.
 */
class WeakSetObject : public Object {
public:
    WeakSetObject() : Object(ObjectType::WeakSet) {}

    void add(Object* value) { members_.insert(value); }
    bool has(Object* value) const { return members_.count(value) != 0; }
    bool deleteValue(Object* value) { return members_.erase(value) != 0; }

private:
    std::unordered_set<Object*> members_;
};

/**
 * @brief Set and WeakSet built-ins
 */
class SetBuiltin {
public:
    /**
     * Set.prototype: add, has, delete, clear, forEach, values, keys, entries,
     * size, [Symbol.iterator], and the ES2025 set algebra (union, intersection,
     * difference, symmetricDifference, isSubsetOf, isSupersetOf, isDisjointFrom).
     */
    static Object* createSetPrototype();

    /** The Set constructor (`new Set(iterable)`). */
    static Function* createSetConstructor(Object* prototype);

    /** WeakSet.prototype (add, has, delete) and the WeakSet constructor. */
    static Object* createWeakSetPrototype();
    static Function* createWeakSetConstructor(Object* prototype);
};

} // namespace Zepra::Builtins
