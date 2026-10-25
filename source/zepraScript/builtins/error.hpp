// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
#pragma once

/**
 * @file error.hpp
 * @brief Error and its subtypes: constructors, prototypes, toString, stack
 */

#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"
#include <string>

namespace Zepra::Builtins {

using Runtime::Function;
using Runtime::Object;

class ErrorBuiltin {
public:
    /**
     * Error.prototype: `name` ("Error"), `message` ("") and toString(). The
     * caller links it to Object.prototype.
     */
    static Object* createErrorPrototype();

    /**
     * The constructor of one error type (Error, TypeError, ...).
     *
     * `new TypeError(msg, {cause})` and `TypeError(msg)` build an Error with
     * own non-enumerable `message`, `cause` and `stack`; `name` comes from
     * the prototype. When called from a subclass constructor (`super(msg)`)
     * it initialises `this` instead, so `class MyError extends Error` works.
     *
     * @param prototype  The type's prototype object (created by the caller)
     * @param parent     The Error constructor for a subtype, so the subtype's
     *                   prototype and constructor inherit from Error's; null
     *                   when creating Error itself
     */
    static Function* createConstructor(const std::string& name, Object* prototype, Function* parent);

    /** AggregateError(errors, message): `errors` is an array or array-like. */
    static Function* createAggregateErrorConstructor(Object* prototype, Function* parent);
};

} // namespace Zepra::Builtins
