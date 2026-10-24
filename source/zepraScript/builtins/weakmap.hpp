#pragma once

/**
 * @file weakmap.hpp
 * @brief JavaScript WeakMap builtin
 */

#include "../config.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"
#include <unordered_map>

namespace Zepra::Runtime { class Context; }

namespace Zepra::Builtins {

using Runtime::Value;
using Runtime::Object;
using Runtime::ObjectType;

/**
 * @brief JavaScript WeakMap object
 * 
 * Keys must be objects, and entries are weakly held
 * (allow garbage collection of keys).
 */
class WeakMapObject : public Object {
public:
    WeakMapObject() : Object(ObjectType::WeakMap) {}
    
    // WeakMap API
    void set(Object* key, const Value& value);
    Value get(Object* key) const;
    bool has(Object* key) const;
    bool deleteKey(Object* key);
    
private:
    // Using raw pointer as key - GC will need to clean up dead entries
    std::unordered_map<Object*, Value> entries_;
};

/**
 * @brief WeakMap builtin functions
 */
class WeakMapBuiltin {
public:
    static Value constructor(Runtime::Context* ctx, const std::vector<Value>& args);
    static Value get(Runtime::Context* ctx, const std::vector<Value>& args);
    static Value set(Runtime::Context* ctx, const std::vector<Value>& args);
    static Value has(Runtime::Context* ctx, const std::vector<Value>& args);
    static Value deleteMethod(Runtime::Context* ctx, const std::vector<Value>& args);
    
    static Object* createWeakMapPrototype();

    /** The WeakMap constructor (`new WeakMap(iterable)`). */
    static Runtime::Function* createWeakMapConstructor(Object* prototype);
};

} // namespace Zepra::Builtins
