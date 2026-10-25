#pragma once

/**
 * @file promise_builtin.hpp
 * @brief JavaScript Promise constructor and Promise.prototype (ES2024 §27.2)
 *
 * The promise objects and their jobs live in runtime/async/promise.hpp;
 * this file is the script-facing surface: new Promise(executor), then,
 * catch, finally, and the statics resolve, reject, all, allSettled, race,
 * any, withResolvers and try. Subclasses (`class P extends Promise`) are
 * honoured through NewPromiseCapability and Symbol.species.
 */

#include "../config.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"

namespace Zepra::Builtins {

class PromiseBuiltin {
public:
    /// Promise.prototype: then, catch, finally and Symbol.toStringTag
    static Runtime::Object* createPromisePrototype();

    /**
     * @brief The Promise constructor for `prototype`. `aggregateError` builds
     * the error Promise.any rejects with (an AggregateError named error
     * object is made when it is null).
     */
    static Runtime::Function* createPromiseConstructor(Runtime::Object* prototype,
                                                       Runtime::Function* aggregateError);
};

} // namespace Zepra::Builtins
