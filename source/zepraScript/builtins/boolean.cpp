// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file boolean.cpp
 * @brief JavaScript Boolean
 */

#include "builtins/boolean.hpp"
#include "builtins/builtin_helpers.hpp"

namespace Zepra::Builtins {

using Runtime::FunctionCallInfo;
using Runtime::JSError;

BooleanObject::BooleanObject(bool value)
    : Object(Runtime::ObjectType::Ordinary)
    , value_(value) {}

namespace {

// thisBooleanValue: a boolean, or a Boolean wrapper object
bool thisBoolean(const FunctionCallInfo& info, const char* method) {
    const Value self = info.thisValue();
    if (self.isBoolean()) return self.asBoolean();
    if (self.isObject()) {
        if (auto* wrapper = dynamic_cast<BooleanObject*>(self.asObject())) return wrapper->valueOf();
    }
    throw JSError("TypeError", std::string("Boolean.prototype.") + method +
                               " requires that 'this' be a Boolean");
}

} // namespace

Object* BooleanBuiltin::createBooleanPrototype() {
    auto* prototype = new Object();
    defineMethod(prototype, "toString", [](const FunctionCallInfo& info) -> Value {
        return makeString(thisBoolean(info, "toString") ? "true" : "false");
    }, 0);
    defineMethod(prototype, "valueOf", [](const FunctionCallInfo& info) -> Value {
        return Value::boolean(thisBoolean(info, "valueOf"));
    }, 0);
    return prototype;
}

Function* BooleanBuiltin::createBooleanConstructor(Object* prototype) {
    auto* constructor = new Function("Boolean", [prototype](const FunctionCallInfo& info) -> Value {
        const bool truthy = info.argument(0).toBoolean();
        // `new Boolean(x)` arrives with a fresh object that inherits from Boolean.prototype
        const Value self = info.thisValue();
        if (self.isObject() && self.asObject()->prototype() == prototype &&
            !dynamic_cast<BooleanObject*>(self.asObject())) {
            auto* wrapper = new BooleanObject(truthy);
            wrapper->setPrototype(prototype);
            return Value::object(wrapper);
        }
        return Value::boolean(truthy);
    }, 1);
    defineHidden(constructor, "prototype", Value::object(prototype));
    defineHidden(prototype, "constructor", Value::object(constructor));
    return constructor;
}

} // namespace Zepra::Builtins
