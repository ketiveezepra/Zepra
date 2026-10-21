// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file object.cpp
 * @brief JavaScript object implementation
 */

#include "runtime/objects/object.hpp"
#include "runtime/objects/function.hpp"
#include "builtins/array.hpp"
#include <algorithm>

namespace Zepra::Runtime {

// Global shape counter for inline cache validation
uint32_t Object::nextShapeId_ = 1;

// =============================================================================
// Object
// =============================================================================

Object::Object(ObjectType type)
    : objectType_(type)
    , shapeId_(nextShapeId_++)
{
}

Value Object::get(const std::string& key) const {
    // Check own properties
    auto it = properties_.find(key);
    if (it != properties_.end()) {
        return it->second;
    }
    
    // Check prototype chain
    if (prototype_) {
        return prototype_->get(key);
    }
    
    return Value::undefined();
}

Value Object::get(uint32_t index) const {
    if (index < elements_.size()) {
        return elements_[index];
    }
    
    // Fall back to string key
    return get(std::to_string(index));
}

bool Object::set(const std::string& key, Value value) {
    // Check if property is writable
    auto descIt = descriptors_.find(key);
    if (descIt != descriptors_.end()) {
        if (!descIt->second.isWritable()) {
            return false;
        }
    }
    
    // Shape transition on new property (not on update)
    bool isNew = properties_.find(key) == properties_.end();
    properties_[key] = value;
    if (isNew) {
        // Assign slot index for IC offset access
        uint32_t slot = static_cast<uint32_t>(propertySlots_.size());
        propertySlots_.push_back(key);
        slotIndex_[key] = slot;
        shapeId_ = nextShapeId_++;
    }
    return true;
}

// Indices far beyond the current end (sparse arrays such as a[4e9], and the
// well-known symbol IDs 0xFFFF00xx) go to keyed storage: growing the dense
// array to them would allocate up to 32 GB. get/has/delete by index already
// fall back to the keyed form.
static constexpr size_t kMaxDenseGap = size_t(1) << 16;

bool Object::set(uint32_t index, Value value) {
    if (index >= elements_.size()) {
        if (index - elements_.size() > kMaxDenseGap) {
            return set(std::to_string(index), value);
        }
        elements_.resize(index + 1, Value::undefined());
    }
    elements_[index] = value;
    return true;
}

bool Object::has(const std::string& key) const {
    if (properties_.find(key) != properties_.end()) {
        return true;
    }
    if (prototype_) {
        return prototype_->has(key);
    }
    return false;
}

bool Object::has(uint32_t index) const {
    if (index < elements_.size() && !elements_[index].isUndefined()) {
        return true;
    }
    return has(std::to_string(index));
}

bool Object::deleteProperty(const std::string& key) {
    auto descIt = descriptors_.find(key);
    if (descIt != descriptors_.end()) {
        if (!descIt->second.isConfigurable()) {
            return false;
        }
        descriptors_.erase(descIt);
    }
    
    if (properties_.erase(key) > 0) {
        // Invalidate slot index — rebuild on next access
        slotIndex_.erase(key);
        // Don't shrink propertySlots_ — stale entries are harmless
        // since IC validates shapeId before using the offset.
        shapeId_ = nextShapeId_++;  // Shape transition on delete
    }
    return true;
}

bool Object::deleteProperty(uint32_t index) {
    if (index < elements_.size()) {
        elements_[index] = Value::undefined();
        return true;
    }
    return deleteProperty(std::to_string(index));
}

bool Object::defineProperty(const std::string& key, const PropertyDescriptor& desc) {
    if (!extensible_ && properties_.find(key) == properties_.end()) {
        return false;
    }
    
    descriptors_[key] = desc;
    properties_[key] = desc.value;
    return true;
}

std::optional<PropertyDescriptor> Object::getOwnPropertyDescriptor(const std::string& key) const {
    auto it = descriptors_.find(key);
    if (it != descriptors_.end()) {
        return it->second;
    }
    
    auto propIt = properties_.find(key);
    if (propIt != properties_.end()) {
        PropertyDescriptor desc;
        desc.value = propIt->second;
        desc.attributes = PropertyAttribute::Default;
        return desc;
    }
    
    return std::nullopt;
}

std::vector<std::string> Object::getOwnPropertyNames() const {
    std::vector<std::string> names;
    names.reserve(properties_.size() + elements_.size());
    
    // Add indexed properties
    for (size_t i = 0; i < elements_.size(); i++) {
        if (!elements_[i].isUndefined()) {
            names.push_back(std::to_string(i));
        }
    }
    
    // Add named properties
    for (const auto& [key, _] : properties_) {
        names.push_back(key);
    }
    
    return names;
}

std::vector<std::string> Object::keys() const {
    std::vector<std::string> result;
    
    // Add indexed properties
    for (size_t i = 0; i < elements_.size(); i++) {
        if (!elements_[i].isUndefined()) {
            result.push_back(std::to_string(i));
        }
    }
    
    // Add enumerable named properties
    for (const auto& [key, _] : properties_) {
        auto descIt = descriptors_.find(key);
        if (descIt == descriptors_.end() || descIt->second.isEnumerable()) {
            result.push_back(key);
        }
    }
    
    return result;
}

bool Object::isCallable() const {
    return isFunction();
}

bool Object::isConstructor() const {
    if (!isFunction()) return false;
    return static_cast<const Function*>(this)->isConstructor();
}

size_t Object::length() const {
    return elements_.size();
}

void Object::setLength(size_t len) {
    elements_.resize(len, Value::undefined());
}

void Object::markGC() {
    if (gcMarked_) return;
    gcMarked_ = true;
    
    // Mark prototype
    if (prototype_) {
        prototype_->markGC();
    }
    
    // Mark properties
    for (const auto& [_, value] : properties_) {
        if (value.isObject()) {
            value.asObject()->markGC();
        }
    }
    
    // Mark elements
    for (const auto& value : elements_) {
        if (value.isObject()) {
            value.asObject()->markGC();
        }
    }
}

Value Object::internalSlot(const std::string& name) const {
    auto it = internalSlots_.find(name);
    if (it != internalSlots_.end()) {
        return it->second;
    }
    return Value::undefined();
}

void Object::setInternalSlot(const std::string& name, Value value) {
    internalSlots_[name] = value;
}

int32_t Object::findPropertySlot(const std::string& key) const {
    auto it = slotIndex_.find(key);
    if (it != slotIndex_.end()) {
        return static_cast<int32_t>(it->second);
    }
    return -1;
}

Value Object::getPropertyBySlot(uint32_t slot) const {
    if (slot < propertySlots_.size()) {
        const std::string& key = propertySlots_[slot];
        auto it = properties_.find(key);
        if (it != properties_.end()) {
            return it->second;
        }
    }
    return Value::undefined();
}

void Object::setPropertyBySlot(uint32_t slot, Value value) {
    if (slot < propertySlots_.size()) {
        const std::string& key = propertySlots_[slot];
        properties_[key] = value;
    }
}

// =============================================================================
// String
// =============================================================================

// Shared String prototype that gets attached to all String instances
static Object* getStringPrototype() {
    static Object* stringProto = nullptr;
    if (!stringProto) {
        stringProto = new Object();
        
        // slice(start, end?)
        stringProto->set("slice", Value::object(
            new Function("slice", [](const FunctionCallInfo& info) -> Value {
                Value thisVal = info.thisValue();
                if (!thisVal.isString()) return Value::undefined();
                String* str = static_cast<String*>(thisVal.asObject());
                const std::string& s = str->value();
                
                int64_t len = static_cast<int64_t>(s.size());
                int64_t start = info.argumentCount() > 0 ? static_cast<int64_t>(info.argument(0).toNumber()) : 0;
                int64_t end = info.argumentCount() > 1 ? static_cast<int64_t>(info.argument(1).toNumber()) : len;
                
                if (start < 0) start = std::max(len + start, int64_t(0));
                if (end < 0) end = std::max(len + end, int64_t(0));
                if (start > len) start = len;
                if (end > len) end = len;
                if (start >= end) return Value::string(new String(""));
                
                return Value::string(new String(s.substr(static_cast<size_t>(start), static_cast<size_t>(end - start))));
            }, 2)));
        
        // toUpperCase()
        stringProto->set("toUpperCase", Value::object(
            new Function("toUpperCase", [](const FunctionCallInfo& info) -> Value {
                Value thisVal = info.thisValue();
                if (!thisVal.isString()) return Value::undefined();
                String* str = static_cast<String*>(thisVal.asObject());
                std::string result = str->value();
                std::transform(result.begin(), result.end(), result.begin(), ::toupper);
                return Value::string(new String(result));
            }, 0)));
        
        // toLowerCase()
        stringProto->set("toLowerCase", Value::object(
            new Function("toLowerCase", [](const FunctionCallInfo& info) -> Value {
                Value thisVal = info.thisValue();
                if (!thisVal.isString()) return Value::undefined();
                String* str = static_cast<String*>(thisVal.asObject());
                std::string result = str->value();
                std::transform(result.begin(), result.end(), result.begin(), ::tolower);
                return Value::string(new String(result));
            }, 0)));
        
        // trim()
        stringProto->set("trim", Value::object(
            new Function("trim", [](const FunctionCallInfo& info) -> Value {
                Value thisVal = info.thisValue();
                if (!thisVal.isString()) return Value::undefined();
                String* str = static_cast<String*>(thisVal.asObject());
                const std::string& s = str->value();
                const std::string ws = " \t\n\r\f\v";
                size_t start = s.find_first_not_of(ws);
                if (start == std::string::npos) return Value::string(new String(""));
                size_t end = s.find_last_not_of(ws);
                return Value::string(new String(s.substr(start, end - start + 1)));
            }, 0)));
        
        // charAt(index)
        stringProto->set("charAt", Value::object(
            new Function("charAt", [](const FunctionCallInfo& info) -> Value {
                Value thisVal = info.thisValue();
                if (!thisVal.isString()) return Value::undefined();
                String* str = static_cast<String*>(thisVal.asObject());
                size_t idx = info.argumentCount() > 0 ? static_cast<size_t>(info.argument(0).toNumber()) : 0;
                if (idx >= str->value().size()) return Value::string(new String(""));
                return Value::string(new String(std::string(1, str->value()[idx])));
            }, 1)));
        
        // indexOf(search, start?)
        stringProto->set("indexOf", Value::object(
            new Function("indexOf", [](const FunctionCallInfo& info) -> Value {
                Value thisVal = info.thisValue();
                if (!thisVal.isString() || info.argumentCount() < 1) return Value::number(-1);
                String* str = static_cast<String*>(thisVal.asObject());
                std::string search = info.argument(0).toString();
                size_t start = info.argumentCount() > 1 ? static_cast<size_t>(info.argument(1).toNumber()) : 0;
                size_t pos = str->value().find(search, start);
                return Value::number(pos == std::string::npos ? -1 : static_cast<double>(pos));
            }, 1)));
        
        // split(separator)
        stringProto->set("split", Value::object(
            new Function("split", [](const FunctionCallInfo& info) -> Value {
                Value thisVal = info.thisValue();
                if (!thisVal.isString()) return Value::object(new Array({}));
                String* str = static_cast<String*>(thisVal.asObject());
                const std::string& s = str->value();
                
                if (info.argumentCount() < 1) {
                    std::vector<Value> parts;
                    parts.push_back(Value::string(new String(s)));
                    return Value::object(new Array(std::move(parts)));
                }
                
                std::string sep = info.argument(0).toString();
                std::vector<Value> parts;
                
                if (sep.empty()) {
                    for (char c : s) {
                        parts.push_back(Value::string(new String(std::string(1, c))));
                    }
                } else {
                    size_t pos = 0, found;
                    while ((found = s.find(sep, pos)) != std::string::npos) {
                        parts.push_back(Value::string(new String(s.substr(pos, found - pos))));
                        pos = found + sep.size();
                    }
                    parts.push_back(Value::string(new String(s.substr(pos))));
                }
                
                return Value::object(new Array(std::move(parts)));
            }, 1)));
    }
    return stringProto;
}

String::String(std::string value)
    : Object(ObjectType::String)
    , value_(std::move(value))
{
    setPrototype(getStringPrototype());
}

Value String::charAt(size_t index) const {
    if (index >= value_.size()) {
        return Value::string(new String(""));
    }
    return Value::string(new String(std::string(1, value_[index])));
}

int String::charCodeAt(size_t index) const {
    if (index >= value_.size()) {
        return -1; // NaN in JS
    }
    return static_cast<unsigned char>(value_[index]);
}

String* String::concat(const String* other) const {
    return new String(value_ + other->value_);
}

String* String::substring(size_t start, size_t end) const {
    if (start > value_.size()) start = value_.size();
    if (end > value_.size()) end = value_.size();
    if (start > end) std::swap(start, end);
    
    return new String(value_.substr(start, end - start));
}

String* String::toLowerCase() const {
    std::string result = value_;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return new String(result);
}

String* String::toUpperCase() const {
    std::string result = value_;
    std::transform(result.begin(), result.end(), result.begin(), ::toupper);
    return new String(result);
}

String* String::trim() const {
    const std::string whitespace = " \t\n\r\f\v";
    size_t start = value_.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return new String("");
    }
    size_t end = value_.find_last_not_of(whitespace);
    return new String(value_.substr(start, end - start + 1));
}

int String::indexOf(const std::string& search, size_t start) const {
    if (start >= value_.size()) return -1;
    size_t pos = value_.find(search, start);
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
}

int String::lastIndexOf(const std::string& search) const {
    size_t pos = value_.rfind(search);
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
}

bool String::includes(const std::string& search, size_t start) const {
    return indexOf(search, start) != -1;
}

bool String::startsWith(const std::string& prefix, size_t start) const {
    if (start + prefix.size() > value_.size()) return false;
    return value_.compare(start, prefix.size(), prefix) == 0;
}

bool String::endsWith(const std::string& suffix) const {
    if (suffix.size() > value_.size()) return false;
    return value_.compare(value_.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// =============================================================================
// Array
// =============================================================================

// Shared Array prototype - forward declare
namespace Zepra::Builtins { class ArrayBuiltin; }

// Shared Array prototype that gets attached to all Array instances
static Object* getArrayPrototype() {
    static Object* arrayProto = nullptr;
    if (!arrayProto) {
        // Use the full ArrayBuiltin prototype
        arrayProto = Builtins::ArrayBuiltin::createArrayPrototype(nullptr);
    }
    return arrayProto;
}

Array::Array()
    : Object(ObjectType::Array)
{
    setPrototype(getArrayPrototype());
}

Array::Array(size_t length)
    : Object(ObjectType::Array)
{
    elements_.resize(length, Value::undefined());
    setPrototype(getArrayPrototype());
}

Array::Array(std::vector<Value> elements)
    : Object(ObjectType::Array)
{
    // Assign to the base class elements_
    Object::elements_ = std::move(elements);
    setPrototype(getArrayPrototype());
}

void Array::push(Value value) {
    elements_.push_back(value);
}

Value Array::pop() {
    if (elements_.empty()) {
        return Value::undefined();
    }
    Value val = elements_.back();
    elements_.pop_back();
    return val;
}

void Array::unshift(Value value) {
    elements_.insert(elements_.begin(), value);
}

Value Array::shift() {
    if (elements_.empty()) {
        return Value::undefined();
    }
    Value val = elements_.front();
    elements_.erase(elements_.begin());
    return val;
}

Value Array::at(size_t index) const {
    if (index < elements_.size()) {
        return elements_[index];
    }
    return Value::undefined();
}

void Array::splice(size_t start, size_t deleteCount, const std::vector<Value>& items) {
    if (start > elements_.size()) start = elements_.size();
    if (start + deleteCount > elements_.size()) {
        deleteCount = elements_.size() - start;
    }
    
    elements_.erase(elements_.begin() + start, 
                    elements_.begin() + start + deleteCount);
    elements_.insert(elements_.begin() + start, items.begin(), items.end());
}

Array* Array::slice(size_t start, size_t end) const {
    if (start > elements_.size()) start = elements_.size();
    if (end > elements_.size()) end = elements_.size();
    if (start > end) return new Array();
    
    std::vector<Value> result(elements_.begin() + start, elements_.begin() + end);
    return new Array(std::move(result));
}

Array* Array::concat(const Array* other) const {
    std::vector<Value> result = elements_;
    result.insert(result.end(), other->elements_.begin(), other->elements_.end());
    return new Array(std::move(result));
}

int Array::indexOf(const Value& value, size_t start) const {
    for (size_t i = start; i < elements_.size(); i++) {
        if (elements_[i].strictEquals(value)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool Array::includes(const Value& value, size_t start) const {
    return indexOf(value, start) != -1;
}

void Array::reverse() {
    std::reverse(elements_.begin(), elements_.end());
}

void Array::fill(Value value, size_t start, size_t end) {
    if (start >= elements_.size()) return;
    if (end > elements_.size()) end = elements_.size();
    
    for (size_t i = start; i < end; i++) {
        elements_[i] = value;
    }
}

void Array::set(size_t index, Value value) {
    if (index >= elements_.size()) {
        if (index - elements_.size() > kMaxDenseGap) {
            Object::set(std::to_string(index), value);
            return;
        }
        elements_.resize(index + 1, Value::undefined());
    }
    elements_[index] = value;
}

void Array::setLength(size_t len) {
    elements_.resize(len, Value::undefined());
}

// =============================================================================
// Error (ES2022 with cause support)
// =============================================================================

Error::Error(const std::string& message, const std::string& name)
    : Object(ObjectType::Error)
    , message_(message)
    , name_(name)
    , cause_(Value::undefined())
{
    // Set standard error properties
    set("message", Value::string(new String(message)));
    set("name", Value::string(new String(name)));
}

Error::Error(const std::string& message, const std::string& name, Value cause)
    : Object(ObjectType::Error)
    , message_(message)
    , name_(name)
    , cause_(cause)
{
    set("message", Value::string(new String(message)));
    set("name", Value::string(new String(name)));
    
    // ES2022: Set cause property if provided
    if (!cause.isUndefined()) {
        set("cause", cause);
    }
}

Error* Error::typeError(const std::string& message) {
    return new Error(message, "TypeError");
}

Error* Error::rangeError(const std::string& message) {
    return new Error(message, "RangeError");
}

Error* Error::referenceError(const std::string& message) {
    return new Error(message, "ReferenceError");
}

Error* Error::syntaxError(const std::string& message) {
    return new Error(message, "SyntaxError");
}

Error* Error::uriError(const std::string& message) {
    return new Error(message, "URIError");
}

Error* Error::evalError(const std::string& message) {
    return new Error(message, "EvalError");
}

Error* Error::aggregateError(const std::string& message, const std::vector<Value>& errors) {
    Error* err = new Error(message, "AggregateError");
    err->set("errors", Value::object(new Array(errors)));
    return err;
}

Error* Error::withCause(const std::string& name, const std::string& message, Value cause) {
    return new Error(message, name, cause);
}

// =============================================================================
// WeakReference (ES2021)
// =============================================================================

WeakReference::WeakReference(Object* target)
    : Object(ObjectType::WeakRef)
    , target_(target)
{
}

Value WeakReference::deref() const {
    if (target_ != nullptr) {
        return Value::object(target_);
    }
    return Value::undefined();
}

// =============================================================================
// FinalizationRegistry (ES2021)
// =============================================================================

FinalizationRegistry::FinalizationRegistry(CleanupCallback callback)
    : Object(ObjectType::FinalizationRegistry)
    , callback_(callback)
{
}

void FinalizationRegistry::registerTarget(Object* target, Value heldValue, Value unregisterToken) {
    Registration reg;
    reg.target = target;
    reg.heldValue = heldValue;
    reg.unregisterToken = unregisterToken;
    reg.collected = false;
    registrations_.push_back(reg);
}

bool FinalizationRegistry::unregister(Value token) {
    if (token.isUndefined()) {
        return false;
    }
    
    bool found = false;
    auto it = registrations_.begin();
    while (it != registrations_.end()) {
        if (it->unregisterToken.strictEquals(token)) {
            it = registrations_.erase(it);
            found = true;
        } else {
            ++it;
        }
    }
    return found;
}

void FinalizationRegistry::cleanupSome() {
    for (auto& reg : registrations_) {
        if (reg.collected && callback_) {
            callback_(reg.heldValue);
        }
    }
    
    // Remove collected registrations
    registrations_.erase(
        std::remove_if(registrations_.begin(), registrations_.end(),
            [](const Registration& r) { return r.collected; }),
        registrations_.end());
}

void FinalizationRegistry::notifyCollected(Object* target) {
    for (auto& reg : registrations_) {
        if (reg.target == target) {
            reg.collected = true;
            reg.target = nullptr;
        }
    }
}

// =============================================================================
// StructuredClone (ES2021)
// =============================================================================

Value StructuredClone::clone(const Value& value) {
    std::unordered_map<Object*, Object*> seen;
    return cloneInternal(value, seen);
}

Value StructuredClone::clone(const Value& value, const std::vector<Object*>& transfer) {
    // Transfer semantics: move objects instead of cloning
    // For simplicity, we just clone - full transfer requires detaching ArrayBuffers etc.
    (void)transfer;
    return clone(value);
}

Value StructuredClone::cloneInternal(const Value& value, std::unordered_map<Object*, Object*>& seen) {
    // Primitives are cloned by value
    if (value.isUndefined() || value.isNull() || value.isBoolean() || 
        value.isNumber() || value.isString()) {
        // Strings need to be cloned as new String objects
        if (value.isString()) {
            return Value::string(new String(value.toString()));
        }
        return value;
    }
    
    if (!value.isObject()) {
        return value;
    }
    
    Object* obj = value.asObject();
    
    // Check for circular reference
    auto it = seen.find(obj);
    if (it != seen.end()) {
        return Value::object(it->second);
    }
    
    // Clone based on type
    switch (obj->objectType()) {
        case ObjectType::Array:
            return Value::object(cloneArray(static_cast<Array*>(obj), seen));
            
        case ObjectType::Date: {
            // Clone Date - preserve timestamp
            Object* dateClone = new Object(ObjectType::Date);
            dateClone->set("[[DateValue]]", obj->internalSlot("[[DateValue]]"));
            seen[obj] = dateClone;
            return Value::object(dateClone);
        }
        
        case ObjectType::RegExp: {
            // Clone RegExp
            Object* regexpClone = new Object(ObjectType::RegExp);
            regexpClone->set("source", obj->get("source"));
            regexpClone->set("flags", obj->get("flags"));
            seen[obj] = regexpClone;
            return Value::object(regexpClone);
        }
        
        case ObjectType::Error: {
            // Clone Error with cause
            Error* errSrc = static_cast<Error*>(obj);
            Error* errClone = new Error(errSrc->message(), errSrc->name(), errSrc->cause());
            seen[obj] = errClone;
            return Value::object(errClone);
        }
        
        case ObjectType::Map: {
            // Clone Map
            Object* mapClone = new Object(ObjectType::Map);
            seen[obj] = mapClone;
            return Value::object(mapClone);
        }
        
        case ObjectType::Set: {
            // Clone Set
            Object* setClone = new Object(ObjectType::Set);
            seen[obj] = setClone;
            return Value::object(setClone);
        }
        
        default:
            // Clone ordinary object
            return Value::object(cloneObject(obj, seen));
    }
}

Object* StructuredClone::cloneObject(Object* obj, std::unordered_map<Object*, Object*>& seen) {
    Object* clone = new Object();
    seen[obj] = clone;
    
    // Clone enumerable own properties
    std::vector<std::string> keys = obj->keys();
    for (const auto& key : keys) {
        Value propValue = obj->get(key);
        Value clonedValue = cloneInternal(propValue, seen);
        clone->set(key, clonedValue);
    }
    
    return clone;
}

Array* StructuredClone::cloneArray(Array* arr, std::unordered_map<Object*, Object*>& seen) {
    Array* clone = new Array();
    seen[arr] = clone;
    
    for (size_t i = 0; i < arr->length(); i++) {
        Value elem = arr->at(i);
        Value clonedElem = cloneInternal(elem, seen);
        clone->push(clonedElem);
    }
    
    return clone;
}

} // namespace Zepra::Runtime
