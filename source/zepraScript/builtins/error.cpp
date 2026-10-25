// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file error.cpp
 * @brief Error and its subtypes: constructors, prototypes, toString, stack
 */

#include "builtins/error.hpp"
#include "builtins/builtin_helpers.hpp"
#include "runtime/execution/vm.hpp"

namespace Zepra::Builtins {

using Runtime::Error;
using Runtime::FunctionCallInfo;
using Runtime::JSError;
using Runtime::VM;

namespace {

// obj.name as script sees it (accessors and the prototype chain included)
Value member(const Value& object, const std::string& name) {
    if (VM* vm = VM::current()) return vm->getMember(object, name);
    return object.asObject()->get(name);
}

std::string stackHeader(const Value& error) {
    const Value name = member(error, "name");
    const Value message = member(error, "message");
    std::string header = name.isUndefined() ? "Error" : name.toString();
    const std::string text = message.isUndefined() ? "" : message.toString();
    if (header.empty()) return text;
    return text.empty() ? header : header + ": " + text;
}

std::string currentStackTrace() {
    VM* vm = VM::current();
    return vm ? vm->captureStackTrace() : std::string();
}

// The error to initialise: `this` when a subclass constructor calls super(),
// otherwise a new Error object
Object* errorTarget(const Value& self, Object* prototype, const std::string& name) {
    if (self.isObject() && self.asObject()->prototype() != prototype &&
        inheritsFrom(self.asObject(), prototype)) {
        return self.asObject();
    }
    auto* error = new Error("", name);
    // `name` and `message` come from the prototype unless the caller gives a message
    error->deleteProperty("name");
    error->deleteProperty("message");
    error->setPrototype(prototype);
    return error;
}

void initialiseError(Object* target, const Value& message, const Value& options) {
    if (!message.isUndefined()) defineHidden(target, "message", makeString(message.toString()));
    if (options.isObject() && options.asObject()->has("cause")) {
        defineHidden(target, "cause", options.asObject()->get("cause"));
    }
    defineHidden(target, "stack",
                 makeString(stackHeader(Value::object(target)) + "\n" + currentStackTrace()));
}

} // namespace

Object* ErrorBuiltin::createErrorPrototype() {
    auto* prototype = new Object();
    defineHidden(prototype, "name", makeString("Error"));
    defineHidden(prototype, "message", makeString(""));

    defineMethod(prototype, "toString", [](const FunctionCallInfo& info) -> Value {
        const Value self = info.thisValue();
        if (!self.isObject()) {
            throw JSError("TypeError", "Error.prototype.toString requires that 'this' be an Object");
        }
        const Value name = member(self, "name");
        const Value message = member(self, "message");
        const std::string nameText = name.isUndefined() ? "Error" : name.toString();
        const std::string messageText = message.isUndefined() ? "" : message.toString();
        if (nameText.empty()) return makeString(messageText);
        if (messageText.empty()) return makeString(nameText);
        return makeString(nameText + ": " + messageText);
    }, 0);
    return prototype;
}

Function* ErrorBuiltin::createConstructor(const std::string& name, Object* prototype, Function* parent) {
    auto* constructor = new Function(name, [name, prototype](const FunctionCallInfo& info) -> Value {
        Object* target = errorTarget(info.thisValue(), prototype, name);
        initialiseError(target, info.argument(0), info.argument(1));
        return Value::object(target);
    }, 1);

    defineHidden(constructor, "prototype", Value::object(prototype));
    defineHidden(prototype, "constructor", Value::object(constructor));
    if (parent) {
        // TypeError.prototype -> Error.prototype, and TypeError -> Error for statics
        prototype->setPrototype(parent->get("prototype").asObject());
        constructor->setPrototype(parent);
        defineHidden(prototype, "name", makeString(name));
        defineHidden(prototype, "message", makeString(""));
    } else {
        defineMethod(constructor, "captureStackTrace", [](const FunctionCallInfo& info) -> Value {
            if (!info.argument(0).isObject()) {
                throw JSError("TypeError", "Error.captureStackTrace requires an object");
            }
            Object* target = info.argument(0).asObject();
            defineHidden(target, "stack",
                         makeString(stackHeader(info.argument(0)) + "\n" + currentStackTrace()));
            return Value::undefined();
        }, 2);
    }
    return constructor;
}

Function* ErrorBuiltin::createAggregateErrorConstructor(Object* prototype, Function* parent) {
    auto* constructor = new Function("AggregateError", [prototype](const FunctionCallInfo& info) -> Value {
        Object* target = errorTarget(info.thisValue(), prototype, "AggregateError");
        initialiseError(target, info.argument(1), info.argument(2));

        std::vector<Value> errors;
        const Value list = info.argument(0);
        if (list.isObject()) {
            Object* source = list.asObject();
            const double length = member(list, "length").toNumber();
            for (uint32_t i = 0; i < length && i < (1u << 24); ++i) errors.push_back(source->get(i));
        } else if (!list.isUndefined()) {
            throw JSError("TypeError", "AggregateError requires an iterable of errors");
        }
        defineHidden(target, "errors", Value::object(new Runtime::Array(std::move(errors))));
        return Value::object(target);
    }, 2);

    defineHidden(constructor, "prototype", Value::object(prototype));
    defineHidden(prototype, "constructor", Value::object(constructor));
    prototype->setPrototype(parent->get("prototype").asObject());
    constructor->setPrototype(parent);
    defineHidden(prototype, "name", makeString("AggregateError"));
    defineHidden(prototype, "message", makeString(""));
    return constructor;
}

} // namespace Zepra::Builtins
