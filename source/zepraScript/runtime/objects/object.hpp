#pragma once

/**
 * @file object.hpp
 * @brief JavaScript object model
 */

#include "config.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <optional>
#include <cstdint>
#include <limits>
#include <functional>

#include "value.hpp"

namespace Zepra::Runtime {

// Forward declarations
class GCHeap;
class HiddenClass;

/**
 * @brief Property attributes (ECMAScript [[Attributes]])
 */
enum class PropertyAttribute : uint8 {
    None       = 0,
    Writable   = 1 << 0,
    Enumerable = 1 << 1,
    Configurable = 1 << 2,
    
    // Common combinations
    Default = Writable | Enumerable | Configurable,
    ReadOnly = Enumerable | Configurable,
    Internal = None
};

inline PropertyAttribute operator|(PropertyAttribute a, PropertyAttribute b) {
    return static_cast<PropertyAttribute>(static_cast<int>(a) | static_cast<int>(b));
}

inline PropertyAttribute operator&(PropertyAttribute a, PropertyAttribute b) {
    return static_cast<PropertyAttribute>(static_cast<int>(a) & static_cast<int>(b));
}

inline bool hasAttribute(PropertyAttribute attrs, PropertyAttribute flag) {
    return (static_cast<int>(attrs) & static_cast<int>(flag)) != 0;
}

/**
 * @brief Property descriptor
 */
struct PropertyDescriptor {
    Value value = Value::undefined();
    PropertyAttribute attributes = PropertyAttribute::Default;
    
    // Getter/setter for accessor properties
    Value getter = Value::undefined();
    Value setter = Value::undefined();
    
    bool isDataDescriptor() const {
        return !value.isUndefined() || getter.isUndefined();
    }
    
    bool isAccessorDescriptor() const {
        return !getter.isUndefined() || !setter.isUndefined();
    }
    
    bool isWritable() const {
        return hasAttribute(attributes, PropertyAttribute::Writable);
    }
    
    bool isEnumerable() const {
        return hasAttribute(attributes, PropertyAttribute::Enumerable);
    }
    
    bool isConfigurable() const {
        return hasAttribute(attributes, PropertyAttribute::Configurable);
    }
};

/**
 * @brief Object types
 */
enum class ObjectType : uint8 {
    Ordinary,
    Array,
    Function,
    BoundFunction,
    String,
    Boolean,
    Number,
    Date,
    RegExp,
    Error,
    Map,
    Set,
    WeakMap,
    WeakSet,
    Promise,
    Proxy,
    ArrayBuffer,
    TypedArray,
    DataView,
    Arguments,
    Global,
    Module,
    
    // ES2021+ types
    WeakRef,
    FinalizationRegistry,
    
    // Internal types
    Environment,
    Scope,
    BigInt,
    Symbol,

    // WASM types
    Namespace,
    WasmModule,
    WasmInstance,
    WasmMemory,
    WasmTable,
    WasmGlobal,
};

/**
 * @brief Base class for all JavaScript objects
 * 
 * Uses hidden classes for property access optimization.
 */
class Object {
public:
    explicit Object(ObjectType type = ObjectType::Ordinary);
    virtual ~Object() = default;
    
    ObjectType objectType() const { return objectType_; }
    
    // Property operations
    virtual Value get(const std::string& key) const;
    virtual Value get(uint32_t index) const;
    virtual bool set(const std::string& key, Value value);
    virtual bool set(uint32_t index, Value value);
    virtual bool has(const std::string& key) const;
    virtual bool has(uint32_t index) const;
    virtual bool deleteProperty(const std::string& key);
    virtual bool deleteProperty(uint32_t index);
    
    // Slot-indexed property access for inline cache
    int32_t findPropertySlot(const std::string& key) const;
    Value getPropertyBySlot(uint32_t slot) const;
    void setPropertyBySlot(uint32_t slot, Value value);
    
    // Property descriptor operations
    bool defineProperty(const std::string& key, const PropertyDescriptor& desc);
    std::optional<PropertyDescriptor> getOwnPropertyDescriptor(const std::string& key) const;

    /**
     * @brief Finds a named property on this object or its prototype chain
     *
     * Returns false when absent. Otherwise sets either `value` (a data
     * property) or `accessor` (a getter/setter pair; `value` untouched).
     */
    bool lookupProperty(const std::string& key, Value& value,
                        const PropertyDescriptor*& accessor) const;
    
    // Property enumeration
    std::vector<std::string> getOwnPropertyNames() const;
    std::vector<std::string> keys() const;  // Enumerable only
    std::vector<std::string> orderedNamedKeys() const;  // Named keys, insertion order
    
    // Prototype chain
    Object* prototype() const { return prototype_; }
    void setPrototype(Object* proto) { prototype_ = proto; }
    
    // Type checking
    bool isArray() const { return objectType_ == ObjectType::Array; }
    bool isFunction() const { 
        return objectType_ == ObjectType::Function || 
               objectType_ == ObjectType::BoundFunction; 
    }
    bool isCallable() const;
    bool isConstructor() const;
    
    // Extensibility
    bool isExtensible() const { return extensible_; }
    void preventExtensions() { extensible_ = false; }
    
    // Shape/hidden class for inline caching
    uint32_t shapeId() const { return shapeId_; }
    
    // Alias for getOwnPropertyNames for convenience
    std::vector<std::string> ownPropertyNames() const { return getOwnPropertyNames(); }
    
    // Array-like behavior
    virtual size_t length() const;
    virtual void setLength(size_t len);
    
    // GC support
    void markGC();
    bool isMarked() const { return gcMarked_; }
    void clearMark() { gcMarked_ = false; }
    
    /**
     * @brief Visit all object references for GC traversal
     * @param visitor Callback that receives each referenced Object*
     */
    template<typename Visitor>
    void visitRefs(Visitor&& visitor) {
        // Prototype reference
        if (prototype_) visitor(prototype_);
        
        // Property values that are objects
        for (auto& [key, val] : properties_) {
            if (val.isObject()) visitor(val.asObject());
        }
        
        // Descriptor getters/setters
        for (auto& [key, desc] : descriptors_) {
            if (desc.value.isObject()) visitor(desc.value.asObject());
            if (desc.getter.isObject()) visitor(desc.getter.asObject());
            if (desc.setter.isObject()) visitor(desc.setter.asObject());
        }
        
        // Array elements
        for (auto& elem : elements_) {
            if (elem.isObject()) visitor(elem.asObject());
        }
        
        // Internal slots
        for (auto& [key, val] : internalSlots_) {
            if (val.isObject()) visitor(val.asObject());
        }
    }
    
    // GC management flag — true for objects allocated via GCHeap
    bool isGCManaged() const { return gcManaged_; }
    void setGCManaged(bool managed) { gcManaged_ = managed; }
    
    // Internal slots
    Value internalSlot(const std::string& name) const;
    void setInternalSlot(const std::string& name, Value value);
    
protected:
    ObjectType objectType_;
    Object* prototype_ = nullptr;
    bool extensible_ = true;
    bool gcMarked_ = false;
    bool gcManaged_ = false;            // True when allocated via GCHeap
    uint32_t shapeId_ = 0;          // Shape identifier for IC
    static uint32_t nextShapeId_;   // Global shape counter
    
    // Property storage
    std::unordered_map<std::string, Value> properties_;
    std::unordered_map<std::string, PropertyDescriptor> descriptors_;
    
    // Slot-indexed property names (insertion order) for IC offset access
    std::vector<std::string> propertySlots_;
    std::unordered_map<std::string, uint32_t> slotIndex_;  // name → slot
    
    // Indexed properties (for arrays)
    std::vector<Value> elements_;
    
    // Internal slots [[...]]
    std::unordered_map<std::string, Value> internalSlots_;
};

/**
 * @brief String object wrapper
 */
class String : public Object {
public:
    String(std::string value);
    
    const std::string& value() const { return value_; }
    size_t length() const override { return value_.size(); }
    
    // Character access
    Value charAt(size_t index) const;
    int charCodeAt(size_t index) const;
    
    // String methods
    String* concat(const String* other) const;
    String* substring(size_t start, size_t end) const;
    String* toLowerCase() const;
    String* toUpperCase() const;
    String* trim() const;
    
    int indexOf(const std::string& search, size_t start = 0) const;
    int lastIndexOf(const std::string& search) const;
    bool includes(const std::string& search, size_t start = 0) const;
    bool startsWith(const std::string& prefix, size_t start = 0) const;
    bool endsWith(const std::string& suffix) const;
    
private:
    std::string value_;
};

/**
 * @brief Array object
 */
class Array : public Object {
public:
    Array();
    Array(size_t length);
    Array(std::vector<Value> elements);
    
    // Array-specific operations
    void push(Value value);
    Value pop();
    void unshift(Value value);
    Value shift();
    
    Value at(size_t index) const;
    void splice(size_t start, size_t deleteCount, const std::vector<Value>& items);
    Array* slice(size_t start, size_t end) const;
    Array* concat(const Array* other) const;
    
    int indexOf(const Value& value, size_t start = 0) const;
    bool includes(const Value& value, size_t start = 0) const;
    
    // Additional modifying operations
    void reverse();
    void fill(Value value, size_t start = 0, size_t end = static_cast<size_t>(-1));
    void set(size_t index, Value value);
    
    // Iteration
    template<typename Fn>
    void forEach(Fn&& fn) const {
        for (size_t i = 0; i < elements_.size(); ++i) {
            fn(elements_[i], i);
        }
    }
    
    size_t length() const override { return elements_.size(); }
    void setLength(size_t len) override;
    
    // Direct element access
    const std::vector<Value>& elements() const { return elements_; }
};

/**
 * @brief Error object (ES2022 with cause support)
 */
class Error : public Object {
public:
    Error(const std::string& message, const std::string& name = "Error");
    Error(const std::string& message, const std::string& name, Value cause);
    
    const std::string& message() const { return message_; }
    const std::string& name() const { return name_; }
    Value cause() const { return cause_; }
    
    // Stack trace (if available)
    const std::string& stack() const { return stack_; }
    void setStack(const std::string& stack) { stack_ = stack; }
    
    // Error construction helpers
    static Error* typeError(const std::string& message);
    static Error* rangeError(const std::string& message);
    static Error* referenceError(const std::string& message);
    static Error* syntaxError(const std::string& message);
    static Error* uriError(const std::string& message);
    static Error* evalError(const std::string& message);
    static Error* aggregateError(const std::string& message, const std::vector<Value>& errors);
    
    // With cause (ES2022)
    static Error* withCause(const std::string& name, const std::string& message, Value cause);
    
private:
    std::string message_;
    std::string name_;
    Value cause_;
    std::string stack_;
};

/**
 * @brief WeakRef - holds weak reference to object (ES2021)
 */
class WeakReference : public Object {
public:
    explicit WeakReference(Object* target);
    
    // deref() - returns target if still alive, undefined otherwise
    Value deref() const;
    
    // Check if target is still alive
    bool isAlive() const { return target_ != nullptr; }
    
    // Called by GC when target is collected
    void clearTarget() { target_ = nullptr; }
    
    Object* target() const { return target_; }
    
private:
    Object* target_;
};

/**
 * @brief FinalizationRegistry - callback when objects are collected (ES2021)
 */
class FinalizationRegistry : public Object {
public:
    using CleanupCallback = std::function<void(Value heldValue)>;
    
    explicit FinalizationRegistry(CleanupCallback callback);
    
    // Register object with held value and optional unregister token
    void registerTarget(Object* target, Value heldValue, Value unregisterToken = Value::undefined());
    
    // Unregister by token
    bool unregister(Value token);
    
    // Called by GC - process any ready cleanup callbacks
    void cleanupSome();
    
    // Check if object was collected and queue callback
    void notifyCollected(Object* target);
    
private:
    struct Registration {
        Object* target;
        Value heldValue;
        Value unregisterToken;
        bool collected;
    };
    
    CleanupCallback callback_;
    std::vector<Registration> registrations_;
};

/**
 * @brief structuredClone helper for deep cloning (ES2021)
 */
class StructuredClone {
public:
    static Value clone(const Value& value);
    static Value clone(const Value& value, const std::vector<Object*>& transfer);
    
private:
    static Value cloneInternal(const Value& value, std::unordered_map<Object*, Object*>& seen);
    static Object* cloneObject(Object* obj, std::unordered_map<Object*, Object*>& seen);
    static Array* cloneArray(Array* arr, std::unordered_map<Object*, Object*>& seen);
};

} // namespace Zepra::Runtime
