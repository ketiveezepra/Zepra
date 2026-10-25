#pragma once

/**
 * @file map.hpp
 * @brief JavaScript Map
 */

#include "../config.hpp"
#include "builtins/collection_key.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Zepra::Builtins {

using Runtime::Function;
using Runtime::Object;
using Runtime::ObjectType;
using Runtime::Value;

/**
 * @brief A Map: insertion-ordered entries with O(1) lookup by SameValueZero
 */
class MapObject : public Object {
public:
    MapObject() : Object(ObjectType::Map) {}

    void set(const Value& key, const Value& value);
    Value get(const Value& key) const;
    bool has(const Value& key) const;
    bool deleteKey(const Value& key);
    void clear();
    size_t size() const { return live_; }

    /** Snapshots of the live entries in insertion order. */
    std::vector<std::pair<Value, Value>> entries() const;
    std::vector<Value> keys() const;
    std::vector<Value> values() const;

    /**
     * Calls `fn(key, value)` for every entry in insertion order, including
     * entries added by `fn` and skipping ones it deletes.
     */
    void forEach(const std::function<void(const Value&, const Value&)>& fn);

    size_t length() const override { return live_; }

private:
    struct Entry {
        Value key;
        Value value;
        bool live = true;
    };

    void compact();

    std::vector<Entry> entries_;
    std::unordered_map<CollectionKey, size_t, CollectionKeyHash> index_;
    size_t live_ = 0;
    int walking_ = 0;  // forEach calls in progress; entries_ must not move meanwhile
};

/**
 * @brief Map built-ins
 */
class MapBuiltin {
public:
    /** Map.prototype: get, set, has, delete, clear, forEach, keys, values, entries, size, [Symbol.iterator]. */
    static Object* createMapPrototype();

    /** The Map constructor (`new Map(iterable)`) and Map.groupBy. */
    static Function* createMapConstructor(Object* prototype);
};

} // namespace Zepra::Builtins
