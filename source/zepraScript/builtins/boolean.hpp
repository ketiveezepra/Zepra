#pragma once

/**
 * @file boolean.hpp
 * @brief JavaScript Boolean
 */

#include "../config.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"
#include <string>

namespace Zepra::Builtins {

using Runtime::Function;
using Runtime::Object;
using Runtime::Value;

/**
 * @brief A Boolean wrapper object (`new Boolean(false)`)
 */
class BooleanObject : public Object {
public:
    explicit BooleanObject(bool value);

    bool valueOf() const { return value_; }
    std::string toString() const { return value_ ? "true" : "false"; }

private:
    bool value_;
};

/**
 * @brief Boolean built-ins
 */
class BooleanBuiltin {
public:
    /** Boolean.prototype: toString and valueOf. */
    static Object* createBooleanPrototype();

    /** The Boolean function: Boolean(x) converts, `new Boolean(x)` makes a wrapper. */
    static Function* createBooleanConstructor(Object* prototype);
};

} // namespace Zepra::Builtins
