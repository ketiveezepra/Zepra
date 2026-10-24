// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file object_builtins.cpp
 * @brief Object builtin implementation
 */

#include "builtins/object_builtins.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/value.hpp"
#include "runtime/execution/vm.hpp"
#include "runtime/execution/js_error.hpp"
#include "runtime/objects/well_known_symbols.hpp"
#include <algorithm>
#include <stdexcept>

namespace Zepra::Builtins {

namespace {

// hasOwnProperty: stored on the object itself, plus the properties arrays
// and functions provide without storing them
bool hasOwnKey(Runtime::Object* obj, const std::string& key) {
    if (obj->hasOwnProperty(key)) return true;
    if (obj->isArray()) return key == "length";
    if (obj->isFunction()) return key == "length" || key == "name";
    return false;
}

// The prototype scripts see, including the implicit one of arrays and functions
Runtime::Object* prototypeOf(Runtime::Object* obj) {
    Runtime::VM* vm = Runtime::VM::current();
    return vm ? vm->prototypeOf(obj) : obj->prototype();
}

} // namespace

// Object.keys(obj)
Runtime::Value ObjectBuiltin::keys(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 1 || !info.argument(0).isObject()) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    Runtime::Object* obj = info.argument(0).asObject();
    std::vector<std::string> keyNames = obj->keys();
    
    std::vector<Runtime::Value> result;
    for (const auto& key : keyNames) {
        result.push_back(Runtime::Value::string(new Runtime::String(key)));
    }
    
    return Runtime::Value::object(new Runtime::Array(std::move(result)));
}

// Object.values(obj)
Runtime::Value ObjectBuiltin::values(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 1 || !info.argument(0).isObject()) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    Runtime::Object* obj = info.argument(0).asObject();
    std::vector<std::string> keyNames = obj->keys();
    
    std::vector<Runtime::Value> result;
    for (const auto& key : keyNames) {
        result.push_back(obj->get(key));
    }
    
    return Runtime::Value::object(new Runtime::Array(std::move(result)));
}

// Object.entries(obj)
Runtime::Value ObjectBuiltin::entries(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 1 || !info.argument(0).isObject()) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    Runtime::Object* obj = info.argument(0).asObject();
    std::vector<std::string> keyNames = obj->keys();
    
    std::vector<Runtime::Value> result;
    for (const auto& key : keyNames) {
        std::vector<Runtime::Value> entry = {
            Runtime::Value::string(new Runtime::String(key)),
            obj->get(key)
        };
        result.push_back(Runtime::Value::object(new Runtime::Array(std::move(entry))));
    }
    
    return Runtime::Value::object(new Runtime::Array(std::move(result)));
}

// Object.assign(target, ...sources)
Runtime::Value ObjectBuiltin::assign(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 1 || !info.argument(0).isObject()) {
        return Runtime::Value::undefined();
    }
    
    Runtime::Object* target = info.argument(0).asObject();
    
    for (size_t i = 1; i < info.argumentCount(); i++) {
        if (!info.argument(i).isObject()) continue;
        
        Runtime::Object* source = info.argument(i).asObject();
        std::vector<std::string> keyNames = source->keys();
        
        for (const auto& key : keyNames) {
            target->set(key, source->get(key));
        }
    }
    
    return Runtime::Value::object(target);
}

// Object.freeze(obj) — ES2024 §20.1.2.6
// Prevents adding new properties, marks all existing own properties
// as non-configurable and non-writable (data properties).
Runtime::Value ObjectBuiltin::freeze(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 1 || !info.argument(0).isObject()) {
        return info.argumentCount() > 0 ? info.argument(0) : Runtime::Value::undefined();
    }

    Runtime::Object* obj = info.argument(0).asObject();
    obj->preventExtensions();

    for (const auto& key : obj->getOwnPropertyNames()) {
        auto desc = obj->getOwnPropertyDescriptor(key);
        if (!desc.has_value()) continue;

        Runtime::PropertyDescriptor frozen;
        frozen.value = desc->value;
        frozen.getter = desc->getter;
        frozen.setter = desc->setter;
        frozen.attributes = Runtime::PropertyAttribute::None; // non-writable, non-enumerable, non-configurable
        if (desc->isEnumerable()) {
            frozen.attributes = frozen.attributes | Runtime::PropertyAttribute::Enumerable;
        }
        // Data properties become non-writable; accessor properties keep their getter/setter
        // but become non-configurable
        obj->defineProperty(key, frozen);
    }

    return info.argument(0);
}

// Object.seal(obj) — ES2024 §20.1.2.20
// Prevents adding new properties and marks all existing own properties
// as non-configurable (but preserves writability).
Runtime::Value ObjectBuiltin::seal(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 1 || !info.argument(0).isObject()) {
        return info.argumentCount() > 0 ? info.argument(0) : Runtime::Value::undefined();
    }

    Runtime::Object* obj = info.argument(0).asObject();
    obj->preventExtensions();

    for (const auto& key : obj->getOwnPropertyNames()) {
        auto desc = obj->getOwnPropertyDescriptor(key);
        if (!desc.has_value()) continue;

        Runtime::PropertyDescriptor sealed;
        sealed.value = desc->value;
        sealed.getter = desc->getter;
        sealed.setter = desc->setter;
        // Preserve writable and enumerable, strip configurable
        sealed.attributes = Runtime::PropertyAttribute::None;
        if (desc->isWritable()) {
            sealed.attributes = sealed.attributes | Runtime::PropertyAttribute::Writable;
        }
        if (desc->isEnumerable()) {
            sealed.attributes = sealed.attributes | Runtime::PropertyAttribute::Enumerable;
        }
        // No Configurable bit = non-configurable
        obj->defineProperty(key, sealed);
    }

    return info.argument(0);
}

// Object.isFrozen(obj) — ES2024 §20.1.2.14
// Returns true if object is non-extensible and all own properties are
// non-configurable and non-writable.
Runtime::Value ObjectBuiltin::isFrozen(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 1 || !info.argument(0).isObject()) {
        return Runtime::Value::boolean(true);  // Primitives are frozen per spec
    }

    Runtime::Object* obj = info.argument(0).asObject();
    if (obj->isExtensible()) {
        return Runtime::Value::boolean(false);
    }

    for (const auto& key : obj->getOwnPropertyNames()) {
        auto desc = obj->getOwnPropertyDescriptor(key);
        if (!desc.has_value()) continue;

        if (desc->isConfigurable()) return Runtime::Value::boolean(false);
        if (desc->isDataDescriptor() && desc->isWritable()) return Runtime::Value::boolean(false);
    }

    return Runtime::Value::boolean(true);
}

// Object.isSealed(obj) — ES2024 §20.1.2.15
// Returns true if object is non-extensible and all own properties are non-configurable.
Runtime::Value ObjectBuiltin::isSealed(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 1 || !info.argument(0).isObject()) {
        return Runtime::Value::boolean(true);  // Primitives are sealed per spec
    }

    Runtime::Object* obj = info.argument(0).asObject();
    if (obj->isExtensible()) {
        return Runtime::Value::boolean(false);
    }

    for (const auto& key : obj->getOwnPropertyNames()) {
        auto desc = obj->getOwnPropertyDescriptor(key);
        if (!desc.has_value()) continue;

        if (desc->isConfigurable()) return Runtime::Value::boolean(false);
    }

    return Runtime::Value::boolean(true);
}

// Object.create(proto, propertiesObject?)
Runtime::Value ObjectBuiltin::create(const Runtime::FunctionCallInfo& info) {
    Runtime::Object* proto = nullptr;
    
    if (info.argumentCount() > 0 && info.argument(0).isObject()) {
        proto = info.argument(0).asObject();
    } else if (info.argumentCount() > 0 && !info.argument(0).isNull()) {
        throw std::runtime_error("Object prototype may only be an Object or null");
    }
    
    Runtime::Object* obj = new Runtime::Object();
    obj->setPrototype(proto);
    
    return Runtime::Value::object(obj);
}

// Object.getPrototypeOf(obj)
Runtime::Value ObjectBuiltin::getPrototypeOf(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 1 || !info.argument(0).isObject()) {
        return Runtime::Value::null();
    }
    
    Runtime::Object* proto = prototypeOf(info.argument(0).asObject());

    return proto ? Runtime::Value::object(proto) : Runtime::Value::null();
}

// Object.setPrototypeOf(obj, proto)
Runtime::Value ObjectBuiltin::setPrototypeOf(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 2 || !info.argument(0).isObject()) {
        return info.argumentCount() > 0 ? info.argument(0) : Runtime::Value::undefined();
    }
    
    Runtime::Object* obj = info.argument(0).asObject();
    Runtime::Object* proto = nullptr;
    
    if (info.argument(1).isObject()) {
        proto = info.argument(1).asObject();
    } else if (!info.argument(1).isNull()) {
        throw std::runtime_error("Object prototype may only be an Object or null");
    }
    
    obj->setPrototype(proto);
    return info.argument(0);
}

// Object.getOwnPropertyNames(obj)
Runtime::Value ObjectBuiltin::getOwnPropertyNames(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 1 || !info.argument(0).isObject()) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    Runtime::Object* obj = info.argument(0).asObject();
    std::vector<std::string> names = obj->getOwnPropertyNames();
    
    std::vector<Runtime::Value> result;
    for (const auto& name : names) {
        result.push_back(Runtime::Value::string(new Runtime::String(name)));
    }
    
    return Runtime::Value::object(new Runtime::Array(std::move(result)));
}

// Object.getOwnPropertyDescriptor(obj, prop)
Runtime::Value ObjectBuiltin::getOwnPropertyDescriptor(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 2 || !info.argument(0).isObject()) {
        return Runtime::Value::undefined();
    }
    
    Runtime::Object* obj = info.argument(0).asObject();
    std::string key = info.argument(1).toString();
    
    auto desc = obj->getOwnPropertyDescriptor(key);
    if (!desc) {
        return Runtime::Value::undefined();
    }
    
    Runtime::Object* result = new Runtime::Object();
    if (desc->isAccessorDescriptor()) {
        result->set("get", desc->getter);
        result->set("set", desc->setter);
    } else {
        result->set("value", desc->value);
        result->set("writable", Runtime::Value::boolean(desc->isWritable()));
    }
    result->set("enumerable", Runtime::Value::boolean(desc->isEnumerable()));
    result->set("configurable", Runtime::Value::boolean(desc->isConfigurable()));
    
    return Runtime::Value::object(result);
}

// Object.defineProperty(obj, prop, descriptor)
Runtime::Value ObjectBuiltin::defineProperty(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 3 || !info.argument(0).isObject()) {
        return info.argumentCount() > 0 ? info.argument(0) : Runtime::Value::undefined();
    }
    
    Runtime::Object* obj = info.argument(0).asObject();
    std::string key = info.argument(1).toString();
    
    if (!info.argument(2).isObject()) {
        throw Runtime::JSError("TypeError", "Property description must be an object: " + info.argument(2).toString());
    }

    Runtime::Object* descObj = info.argument(2).asObject();
    const bool hasGet = descObj->has("get");
    const bool hasSet = descObj->has("set");
    const bool hasValue = descObj->has("value");
    const bool hasWritable = descObj->has("writable");
    if ((hasGet || hasSet) && (hasValue || hasWritable)) {
        throw Runtime::JSError("TypeError", "Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
    }

    // Fields the descriptor leaves out keep the property's current values;
    // for a new property they default to undefined and false
    Runtime::PropertyDescriptor desc;
    desc.attributes = Runtime::PropertyAttribute::None;
    if (auto current = obj->getOwnPropertyDescriptor(key)) desc = *current;

    auto accessorField = [descObj](const char* name) {
        const Runtime::Value fn = descObj->get(name);
        if (!fn.isUndefined() && !(fn.isObject() && fn.asObject()->isFunction())) {
            throw Runtime::JSError("TypeError", std::string(name == std::string("get") ? "Getter" : "Setter") +
                                   " must be a function: " + fn.toString());
        }
        return fn;
    };
    if (hasGet || hasSet) {
        if (!desc.isAccessorDescriptor()) desc.value = Runtime::Value::undefined();
        if (hasGet) desc.getter = accessorField("get");
        if (hasSet) desc.setter = accessorField("set");
        desc.attributes = desc.attributes & ~Runtime::PropertyAttribute::Writable;
    } else if (hasValue || hasWritable) {
        desc.getter = Runtime::Value::undefined();
        desc.setter = Runtime::Value::undefined();
        if (hasValue) desc.value = descObj->get("value");
    }

    auto applyFlag = [&](bool present, const char* name, Runtime::PropertyAttribute flag) {
        if (!present) return;
        desc.attributes = descObj->get(name).toBoolean() ? (desc.attributes | flag) : (desc.attributes & ~flag);
    };
    applyFlag(hasWritable, "writable", Runtime::PropertyAttribute::Writable);
    applyFlag(descObj->has("enumerable"), "enumerable", Runtime::PropertyAttribute::Enumerable);
    applyFlag(descObj->has("configurable"), "configurable", Runtime::PropertyAttribute::Configurable);

    obj->defineProperty(key, desc);
    
    return info.argument(0);
}

// Object.defineProperties(obj, props)
Runtime::Value ObjectBuiltin::defineProperties(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 2 || !info.argument(0).isObject() || !info.argument(1).isObject()) {
        return info.argumentCount() > 0 ? info.argument(0) : Runtime::Value::undefined();
    }
    
    Runtime::Object* props = info.argument(1).asObject();
    
    std::vector<std::string> keys = props->keys();
    for (const auto& key : keys) {
        std::vector<Runtime::Value> subArgs = {info.argument(0), Runtime::Value::string(new Runtime::String(key)), props->get(key)};
        Runtime::FunctionCallInfo subInfo(info.context(), Runtime::Value::undefined(), subArgs);
        defineProperty(subInfo);
    }
    
    return info.argument(0);
}

// Object.is(value1, value2)
Runtime::Value ObjectBuiltin::is(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 2) {
        return Runtime::Value::boolean(info.argumentCount() == 0);
    }
    
    Runtime::Value val1 = info.argument(0);
    Runtime::Value val2 = info.argument(1);
    
    // Object.is uses SameValue algorithm (handles NaN and -0)
    if (val1.isNumber() && val2.isNumber()) {
        double n1 = val1.asNumber();
        double n2 = val2.asNumber();
        
        // Handle NaN
        if (std::isnan(n1) && std::isnan(n2)) {
            return Runtime::Value::boolean(true);
        }
        
        // Handle +0/-0
        if (n1 == 0 && n2 == 0) {
            return Runtime::Value::boolean(
                (1.0 / n1 > 0) == (1.0 / n2 > 0)
            );
        }
        
        return Runtime::Value::boolean(n1 == n2);
    }
    
    return Runtime::Value::boolean(val1.strictEquals(val2));
}

// Object.fromEntries(iterable) - ES2019
Runtime::Value ObjectBuiltin::fromEntries(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 1) {
        throw std::runtime_error("Object.fromEntries requires an iterable");
    }
    
    Runtime::Object* result = new Runtime::Object();
    Runtime::Value iterable = info.argument(0);
    
    if (!iterable.isObject()) {
        return Runtime::Value::object(result);
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(iterable.asObject());
    if (arr) {
        for (size_t i = 0; i < arr->length(); i++) {
            Runtime::Value entry = arr->at(i);
            if (entry.isObject()) {
                Runtime::Array* pair = dynamic_cast<Runtime::Array*>(entry.asObject());
                if (pair && pair->length() >= 2) {
                    std::string key = pair->at(0).toString();
                    Runtime::Value value = pair->at(1);
                    result->set(key, value);
                }
            }
        }
    }
    
    return Runtime::Value::object(result);
}

// Object.hasOwn(obj, prop) - ES2022
Runtime::Value ObjectBuiltin::hasOwn(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 2) {
        return Runtime::Value::boolean(false);
    }
    
    Runtime::Value objVal = info.argument(0);
    Runtime::Value keyVal = info.argument(1);
    
    if (!objVal.isObject()) {
        // For primitives, return false
        return Runtime::Value::boolean(false);
    }
    
    return Runtime::Value::boolean(hasOwnKey(objVal.asObject(), keyVal.toString()));
}

// Object.groupBy(items, callback) - ES2024
Runtime::Value ObjectBuiltin::groupBy(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 2) {
        throw std::runtime_error("Object.groupBy requires items and callback");
    }
    
    Runtime::Value items = info.argument(0);
    if (!items.isObject()) {
        throw std::runtime_error("First argument must be iterable");
    }
    
    Runtime::Value callbackVal = info.argument(1);
    if (!callbackVal.isObject() || !callbackVal.asObject()->isCallable()) {
        throw std::runtime_error("Second argument must be a function");
    }
    
    Runtime::Function* callback = dynamic_cast<Runtime::Function*>(callbackVal.asObject());
    if (!callback) {
        throw std::runtime_error("Second argument must be a function");
    }
    
    Runtime::Object* result = new Runtime::Object();
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(items.asObject());
    if (arr) {
        for (size_t i = 0; i < arr->length(); i++) {
            Runtime::Value item = arr->at(i);
            
            // Call callback(item, index)
            std::vector<Runtime::Value> args = {item, Runtime::Value::number(static_cast<double>(i))};
            Runtime::FunctionCallInfo callInfo(info.context(), Runtime::Value::undefined(), args);
            Runtime::Value keyVal = callback->builtinFunction()(callInfo);
            std::string key = keyVal.toString();
            
            // Get or create group array
            Runtime::Value groupVal = result->get(key);
            Runtime::Array* group;
            if (!groupVal.isObject()) {
                group = new Runtime::Array();
                result->set(key, Runtime::Value::object(group));
            } else {
                group = dynamic_cast<Runtime::Array*>(groupVal.asObject());
            }
            
            if (group) {
                group->push(item);
            }
        }
    }
    
    return Runtime::Value::object(result);
}

// Object.prototype.hasOwnProperty(prop)
Runtime::Value ObjectBuiltin::hasOwnProperty(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject() || info.argumentCount() < 1) {
        return Runtime::Value::boolean(false);
    }
    
    return Runtime::Value::boolean(hasOwnKey(thisVal.asObject(), info.argument(0).toString()));
}

// Object.prototype.isPrototypeOf(obj)
Runtime::Value ObjectBuiltin::isPrototypeOf(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject() || info.argumentCount() < 1 || !info.argument(0).isObject()) {
        return Runtime::Value::boolean(false);
    }
    
    Runtime::Object* proto = thisVal.asObject();
    Runtime::Object* obj = info.argument(0).asObject();
    
    for (Runtime::Object* current = prototypeOf(obj); current != nullptr; current = prototypeOf(current)) {
        if (current == proto) return Runtime::Value::boolean(true);
    }

    return Runtime::Value::boolean(false);
}

// Object.prototype.propertyIsEnumerable(prop)
Runtime::Value ObjectBuiltin::propertyIsEnumerable(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject() || info.argumentCount() < 1) {
        return Runtime::Value::boolean(false);
    }
    
    Runtime::Object* obj = thisVal.asObject();
    std::string key = info.argument(0).toString();
    
    auto desc = obj->getOwnPropertyDescriptor(key);
    if (!desc) {
        return Runtime::Value::boolean(false);
    }
    
    return Runtime::Value::boolean(desc->isEnumerable());
}

// Object.prototype.toString()
Runtime::Value ObjectBuiltin::toString(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    
    if (thisVal.isNull()) {
        return Runtime::Value::string(new Runtime::String("[object Null]"));
    }
    if (thisVal.isUndefined()) {
        return Runtime::Value::string(new Runtime::String("[object Undefined]"));
    }
    if (!thisVal.isObject()) {
        const char* primitive = thisVal.isString() ? "String" : thisVal.isNumber() ? "Number"
                              : thisVal.isBoolean() ? "Boolean" : "Object";
        return Runtime::Value::string(new Runtime::String(std::string("[object ") + primitive + "]"));
    }

    Runtime::Object* obj = thisVal.asObject();
    std::string tag = "Object";

    // Symbol.toStringTag wins over the built-in kind (Map, Set, Promise, ...)
    if (Runtime::VM* vm = Runtime::VM::current()) {
        const Runtime::Value custom = vm->getMember(
            thisVal, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::ToStringTag));
        if (custom.isString()) {
            return Runtime::Value::string(new Runtime::String("[object " + custom.toString() + "]"));
        }
    }

    switch (obj->objectType()) {
        case Runtime::ObjectType::Array: tag = "Array"; break;
        case Runtime::ObjectType::Function: tag = "Function"; break;
        case Runtime::ObjectType::Date: tag = "Date"; break;
        case Runtime::ObjectType::RegExp: tag = "RegExp"; break;
        case Runtime::ObjectType::Error: tag = "Error"; break;
        case Runtime::ObjectType::Map: tag = "Map"; break;
        case Runtime::ObjectType::Set: tag = "Set"; break;
        case Runtime::ObjectType::Promise: tag = "Promise"; break;
        default: break;
    }
    
    return Runtime::Value::string(new Runtime::String("[object " + tag + "]"));
}

// Object.prototype.valueOf()
Runtime::Value ObjectBuiltin::valueOf(const Runtime::FunctionCallInfo& info) {
    return info.thisValue();
}

// Create Object constructor
Runtime::Object* ObjectBuiltin::createObjectConstructor(Runtime::Context*) {
    Runtime::Object* objConstructor = new Runtime::Object();
    
    // Static methods
    objConstructor->set("keys", Runtime::Value::object(new Runtime::Function("keys", keys, 1)));
    objConstructor->set("values", Runtime::Value::object(new Runtime::Function("values", values, 1)));
    objConstructor->set("entries", Runtime::Value::object(new Runtime::Function("entries", entries, 1)));
    objConstructor->set("assign", Runtime::Value::object(new Runtime::Function("assign", assign, 2)));
    objConstructor->set("freeze", Runtime::Value::object(new Runtime::Function("freeze", freeze, 1)));
    objConstructor->set("seal", Runtime::Value::object(new Runtime::Function("seal", seal, 1)));
    objConstructor->set("isFrozen", Runtime::Value::object(new Runtime::Function("isFrozen", isFrozen, 1)));
    objConstructor->set("isSealed", Runtime::Value::object(new Runtime::Function("isSealed", isSealed, 1)));
    objConstructor->set("create", Runtime::Value::object(new Runtime::Function("create", create, 2)));
    objConstructor->set("getPrototypeOf", Runtime::Value::object(new Runtime::Function("getPrototypeOf", getPrototypeOf, 1)));
    objConstructor->set("setPrototypeOf", Runtime::Value::object(new Runtime::Function("setPrototypeOf", setPrototypeOf, 2)));
    objConstructor->set("getOwnPropertyNames", Runtime::Value::object(new Runtime::Function("getOwnPropertyNames", getOwnPropertyNames, 1)));
    objConstructor->set("getOwnPropertyDescriptor", Runtime::Value::object(new Runtime::Function("getOwnPropertyDescriptor", getOwnPropertyDescriptor, 2)));
    objConstructor->set("defineProperty", Runtime::Value::object(new Runtime::Function("defineProperty", defineProperty, 3)));
    objConstructor->set("defineProperties", Runtime::Value::object(new Runtime::Function("defineProperties", defineProperties, 2)));
    objConstructor->set("is", Runtime::Value::object(new Runtime::Function("is", is, 2)));
    
    // ES2019+
    objConstructor->set("fromEntries", Runtime::Value::object(new Runtime::Function("fromEntries", fromEntries, 1)));
    
    // ES2022+
    objConstructor->set("hasOwn", Runtime::Value::object(new Runtime::Function("hasOwn", hasOwn, 2)));
    
    // ES2024
    objConstructor->set("groupBy", Runtime::Value::object(new Runtime::Function("groupBy", groupBy, 2)));
    
    return objConstructor;
}

// Create Object prototype
Runtime::Object* ObjectBuiltin::createObjectPrototype(Runtime::Context*) {
    Runtime::Object* prototype = new Runtime::Object();
    
    prototype->set("hasOwnProperty", Runtime::Value::object(new Runtime::Function("hasOwnProperty", hasOwnProperty, 1)));
    prototype->set("isPrototypeOf", Runtime::Value::object(new Runtime::Function("isPrototypeOf", isPrototypeOf, 1)));
    prototype->set("propertyIsEnumerable", Runtime::Value::object(new Runtime::Function("propertyIsEnumerable", propertyIsEnumerable, 1)));
    prototype->set("toString", Runtime::Value::object(new Runtime::Function("toString", toString, 0)));
    prototype->set("valueOf", Runtime::Value::object(new Runtime::Function("valueOf", valueOf, 0)));
    
    return prototype;
}

} // namespace Zepra::Builtins
