#pragma once

/**
 * @file symbol.hpp
 * @brief JavaScript Symbol: constructor, registry functions and prototype
 *
 * Symbols are primitive values (Value::symbol(id)); their descriptions live
 * in the process-wide Runtime::SymbolRegistry.
 */

#include "../config.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"

namespace Zepra::Builtins {

class SymbolBuiltin {
public:
    /** Symbol.prototype: toString, valueOf, the description getter and [Symbol.toPrimitive]. */
    static Runtime::Object* createSymbolPrototype();

    /**
     * The Symbol function: Symbol(description) makes a unique symbol,
     * `new Symbol()` is a TypeError. Carries Symbol.for, Symbol.keyFor and the
     * well-known symbols (Symbol.iterator, Symbol.toPrimitive, ...).
     */
    static Runtime::Function* createSymbolConstructor(Runtime::Object* prototype);
};

} // namespace Zepra::Builtins
