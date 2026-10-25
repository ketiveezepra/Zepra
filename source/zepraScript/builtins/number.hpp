#pragma once

/**
 * @file number.hpp
 * @brief JavaScript Number: constructor, prototype methods and global helpers
 */

#include "../config.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"
#include <cmath>
#include <limits>
#include <string>

namespace Zepra::Builtins {

using Runtime::Function;
using Runtime::Object;
using Runtime::Value;

/**
 * @brief A Number wrapper object (`new Number(5)`)
 */
class NumberObject : public Object {
public:
    explicit NumberObject(double value);

    double valueOf() const { return value_; }
    std::string toString(int radix = 10) const;
    std::string toFixed(int digits) const;
    std::string toExponential(int digits) const;
    std::string toPrecision(int precision) const;

    static constexpr double MAX_VALUE = std::numeric_limits<double>::max();
    static constexpr double MIN_VALUE = std::numeric_limits<double>::denorm_min();
    static constexpr double MAX_SAFE_INTEGER = 9007199254740991.0;
    static constexpr double MIN_SAFE_INTEGER = -9007199254740991.0;
    static constexpr double POSITIVE_INFINITY = std::numeric_limits<double>::infinity();
    static constexpr double NEGATIVE_INFINITY = -std::numeric_limits<double>::infinity();
    static constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
    static constexpr double EPSILON = std::numeric_limits<double>::epsilon();

private:
    double value_;
};

/**
 * @brief Number built-ins
 */
class NumberBuiltin {
public:
    /** Number.prototype: toString, toFixed, toExponential, toPrecision, valueOf, toLocaleString. */
    static Object* createNumberPrototype();

    /**
     * The Number function: Number(x) converts, `new Number(x)` makes a wrapper.
     * Carries the constants and the static helpers (isInteger, parseFloat, ...).
     * `parseFloat` and `parseInt` are the global functions themselves, as the
     * specification requires.
     */
    static Function* createNumberConstructor(Object* prototype, Function* parseFloat, Function* parseInt);

    /** The global functions. */
    static Function* createParseInt();
    static Function* createParseFloat();
    static Function* createIsNaN();
    static Function* createIsFinite();

    /**
     * en-US Number.prototype.toLocaleString: digits grouped by commas, at
     * most three fraction digits by default. Honours the minimumFractionDigits
     * and maximumFractionDigits options; other options are ignored.
     */
    static std::string toLocaleString(double value, const Value& options);
};

} // namespace Zepra::Builtins
