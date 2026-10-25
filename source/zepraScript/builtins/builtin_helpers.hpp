// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
#pragma once

/**
 * @file builtin_helpers.hpp
 * @brief Small helpers shared by the built-in object implementations
 */

#include "runtime/execution/js_error.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"

#include <cmath>
#include <string>

namespace Zepra::Builtins {

using Runtime::Function;
using Runtime::Object;
using Runtime::Value;

/** A JavaScript string value. */
inline Runtime::Value makeString(const std::string& text) {
    return Runtime::Value::string(new Runtime::String(text));
}

/**
 * Defines a built-in method on `target`. Like every standard method it is
 * writable and configurable but not enumerable, so for-in and Object.keys
 * do not list it.
 */
inline Runtime::Function* defineMethod(Runtime::Object* target, const std::string& name,
                                       Runtime::BuiltinFn fn, size_t arity) {
    auto* function = new Runtime::Function(name, std::move(fn), arity);
    Runtime::PropertyDescriptor desc;
    desc.value = Runtime::Value::object(function);
    desc.attributes = Runtime::PropertyAttribute::Writable | Runtime::PropertyAttribute::Configurable;
    target->defineProperty(name, desc);
    return function;
}

/** Defines a non-enumerable, non-writable constant (Number.MAX_VALUE, Math.PI, ...). */
inline void defineConstant(Runtime::Object* target, const std::string& name, Runtime::Value value) {
    Runtime::PropertyDescriptor desc;
    desc.value = value;
    desc.attributes = Runtime::PropertyAttribute::None;
    target->defineProperty(name, desc);
}

/** Defines a non-enumerable data property (e.g. Error message, constructor). */
inline void defineHidden(Runtime::Object* target, const std::string& name, Runtime::Value value) {
    Runtime::PropertyDescriptor desc;
    desc.value = value;
    desc.attributes = Runtime::PropertyAttribute::Writable | Runtime::PropertyAttribute::Configurable;
    target->defineProperty(name, desc);
}

/** ToIntegerOrInfinity clamped into int: NaN is 0, undefined is `fallback`. */
inline int integerArgument(const Runtime::Value& value, int fallback) {
    if (value.isUndefined()) return fallback;
    double d = value.toNumber();
    if (std::isnan(d)) return 0;
    d = std::trunc(d);
    if (d > 2147483647.0) return 2147483647;
    if (d < -2147483648.0) return -2147483647 - 1;
    return static_cast<int>(d);
}

/** True when `object` has `prototype` somewhere on its prototype chain. */
inline bool inheritsFrom(const Runtime::Object* object, const Runtime::Object* prototype) {
    for (const Runtime::Object* o = object ? object->prototype() : nullptr; o; o = o->prototype()) {
        if (o == prototype) return true;
    }
    return false;
}

} // namespace Zepra::Builtins
