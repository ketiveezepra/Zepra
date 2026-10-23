// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file number.cpp
 * @brief JavaScript Number: constructor, prototype methods and global helpers
 */

#include "builtins/number.hpp"
#include "builtins/builtin_helpers.hpp"
#include "runtime/objects/number_conversion.hpp"

#include <algorithm>
#include <cmath>

namespace Zepra::Builtins {

namespace NC = Runtime::NumberConversion;
using Runtime::FunctionCallInfo;
using Runtime::JSError;

NumberObject::NumberObject(double value)
    : Object(Runtime::ObjectType::Ordinary)
    , value_(value) {}

std::string NumberObject::toString(int radix) const {
    return NC::toStringRadix(value_, radix);
}

std::string NumberObject::toFixed(int digits) const {
    return NC::toFixed(value_, digits);
}

std::string NumberObject::toExponential(int digits) const {
    return NC::toExponential(value_, digits);
}

std::string NumberObject::toPrecision(int precision) const {
    return NC::toPrecision(value_, precision);
}

namespace {

// thisNumberValue: a number, or a Number wrapper object
double thisNumber(const FunctionCallInfo& info, const char* method) {
    const Value self = info.thisValue();
    if (self.isNumber()) return self.asNumber();
    if (self.isObject()) {
        if (auto* wrapper = dynamic_cast<NumberObject*>(self.asObject())) return wrapper->valueOf();
    }
    throw JSError("TypeError", std::string("Number.prototype.") + method +
                               " requires that 'this' be a Number");
}

void requireDigitRange(double digits, int low, int high, const char* method) {
    if (!(digits >= low && digits <= high)) {
        throw JSError("RangeError", std::string(method) + "() argument must be between " +
                                    std::to_string(low) + " and " + std::to_string(high));
    }
}

// ToIntegerOrInfinity as a double, so out-of-range values can be rejected
double integerOrInfinity(const Value& value) {
    double d = value.toNumber();
    if (std::isnan(d)) return 0;
    return std::trunc(d);
}

std::string groupThousands(const std::string& digits) {
    std::string out;
    const size_t first = digits.size() % 3;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (i % 3) == first) out += ',';
        out += digits[i];
    }
    return out;
}

} // namespace

std::string NumberBuiltin::toLocaleString(double value, const Value& options) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value > 0 ? "\xE2\x88\x9E" : "-\xE2\x88\x9E";

    int minFraction = 0;
    int maxFraction = 3;
    if (options.isObject()) {
        const Value minOption = options.asObject()->get("minimumFractionDigits");
        const Value maxOption = options.asObject()->get("maximumFractionDigits");
        if (!minOption.isUndefined()) {
            double d = integerOrInfinity(minOption);
            requireDigitRange(d, 0, 100, "minimumFractionDigits");
            minFraction = static_cast<int>(d);
            maxFraction = std::max(minFraction, maxFraction);
        }
        if (!maxOption.isUndefined()) {
            double d = integerOrInfinity(maxOption);
            requireDigitRange(d, 0, 100, "maximumFractionDigits");
            maxFraction = static_cast<int>(d);
            if (minOption.isUndefined()) minFraction = std::min(minFraction, maxFraction);
        }
        if (minFraction > maxFraction) {
            throw JSError("RangeError", "maximumFractionDigits value is out of range.");
        }
    }

    std::string fixed = NC::toFixed(std::fabs(value), maxFraction);
    std::string integer = fixed;
    std::string fraction;
    const size_t point = fixed.find('.');
    if (point != std::string::npos) {
        integer = fixed.substr(0, point);
        fraction = fixed.substr(point + 1);
    }
    while (static_cast<int>(fraction.size()) > minFraction && fraction.back() == '0') fraction.pop_back();

    std::string out = groupThousands(integer);
    if (!fraction.empty()) out += "." + fraction;
    return std::signbit(value) ? "-" + out : out;
}

Object* NumberBuiltin::createNumberPrototype() {
    auto* prototype = new Object();

    defineMethod(prototype, "toString", [](const FunctionCallInfo& info) -> Value {
        const double number = thisNumber(info, "toString");
        double radix = info.argument(0).isUndefined() ? 10 : integerOrInfinity(info.argument(0));
        if (!(radix >= 2 && radix <= 36)) {
            throw JSError("RangeError", "toString() radix must be between 2 and 36");
        }
        return makeString(NC::toStringRadix(number, static_cast<int>(radix)));
    }, 1);

    defineMethod(prototype, "toFixed", [](const FunctionCallInfo& info) -> Value {
        const double number = thisNumber(info, "toFixed");
        double digits = integerOrInfinity(info.argument(0));
        requireDigitRange(digits, 0, 100, "toFixed");
        return makeString(NC::toFixed(number, static_cast<int>(digits)));
    }, 1);

    defineMethod(prototype, "toExponential", [](const FunctionCallInfo& info) -> Value {
        const double number = thisNumber(info, "toExponential");
        const bool unspecified = info.argument(0).isUndefined();
        double digits = integerOrInfinity(info.argument(0));
        if (!std::isfinite(number)) return makeString(NC::toExponential(number, -1));
        if (!unspecified) requireDigitRange(digits, 0, 100, "toExponential");
        return makeString(NC::toExponential(number, unspecified ? -1 : static_cast<int>(digits)));
    }, 1);

    defineMethod(prototype, "toPrecision", [](const FunctionCallInfo& info) -> Value {
        const double number = thisNumber(info, "toPrecision");
        if (info.argument(0).isUndefined()) return makeString(NC::toString(number));
        double precision = integerOrInfinity(info.argument(0));
        if (!std::isfinite(number)) return makeString(NC::toString(number));
        requireDigitRange(precision, 1, 100, "toPrecision");
        return makeString(NC::toPrecision(number, static_cast<int>(precision)));
    }, 1);

    defineMethod(prototype, "valueOf", [](const FunctionCallInfo& info) -> Value {
        return Value::number(thisNumber(info, "valueOf"));
    }, 0);

    defineMethod(prototype, "toLocaleString", [](const FunctionCallInfo& info) -> Value {
        return makeString(NumberBuiltin::toLocaleString(thisNumber(info, "toLocaleString"), info.argument(1)));
    }, 0);

    return prototype;
}

Function* NumberBuiltin::createParseInt() {
    return new Function("parseInt", [](const FunctionCallInfo& info) -> Value {
        const std::string text = info.argument(0).toString();
        double radix = info.argument(1).isUndefined() ? 0 : std::fmod(integerOrInfinity(info.argument(1)), 4294967296.0);
        if (radix < -2147483648.0) radix += 4294967296.0;
        if (radix > 2147483647.0) radix -= 4294967296.0;
        return Value::number(NC::parseInt(text, static_cast<int>(radix)));
    }, 2);
}

Function* NumberBuiltin::createParseFloat() {
    return new Function("parseFloat", [](const FunctionCallInfo& info) -> Value {
        return Value::number(NC::parseFloat(info.argument(0).toString()));
    }, 1);
}

Function* NumberBuiltin::createIsNaN() {
    return new Function("isNaN", [](const FunctionCallInfo& info) -> Value {
        return Value::boolean(std::isnan(info.argument(0).toNumber()));
    }, 1);
}

Function* NumberBuiltin::createIsFinite() {
    return new Function("isFinite", [](const FunctionCallInfo& info) -> Value {
        return Value::boolean(std::isfinite(info.argument(0).toNumber()));
    }, 1);
}

Function* NumberBuiltin::createNumberConstructor(Object* prototype, Function* parseFloat, Function* parseInt) {
    auto* constructor = new Function("Number", [prototype](const FunctionCallInfo& info) -> Value {
        const double number = info.argumentCount() == 0 ? 0.0 : info.argument(0).toNumber();
        // `new Number(x)` arrives with a fresh object that inherits from Number.prototype
        const Value self = info.thisValue();
        if (self.isObject() && self.asObject()->prototype() == prototype &&
            !dynamic_cast<NumberObject*>(self.asObject())) {
            auto* wrapper = new NumberObject(number);
            wrapper->setPrototype(prototype);
            return Value::object(wrapper);
        }
        return Value::number(number);
    }, 1);

    defineHidden(constructor, "prototype", Value::object(prototype));
    defineHidden(prototype, "constructor", Value::object(constructor));

    defineConstant(constructor, "MAX_SAFE_INTEGER", Value::number(NumberObject::MAX_SAFE_INTEGER));
    defineConstant(constructor, "MIN_SAFE_INTEGER", Value::number(NumberObject::MIN_SAFE_INTEGER));
    defineConstant(constructor, "MAX_VALUE", Value::number(NumberObject::MAX_VALUE));
    defineConstant(constructor, "MIN_VALUE", Value::number(NumberObject::MIN_VALUE));
    defineConstant(constructor, "EPSILON", Value::number(NumberObject::EPSILON));
    defineConstant(constructor, "POSITIVE_INFINITY", Value::number(NumberObject::POSITIVE_INFINITY));
    defineConstant(constructor, "NEGATIVE_INFINITY", Value::number(NumberObject::NEGATIVE_INFINITY));
    defineConstant(constructor, "NaN", Value::number(NumberObject::NaN));

    defineMethod(constructor, "isNaN", [](const FunctionCallInfo& info) -> Value {
        const Value v = info.argument(0);
        return Value::boolean(v.isNumber() && std::isnan(v.asNumber()));
    }, 1);
    defineMethod(constructor, "isFinite", [](const FunctionCallInfo& info) -> Value {
        const Value v = info.argument(0);
        return Value::boolean(v.isNumber() && std::isfinite(v.asNumber()));
    }, 1);
    defineMethod(constructor, "isInteger", [](const FunctionCallInfo& info) -> Value {
        const Value v = info.argument(0);
        return Value::boolean(v.isNumber() && std::isfinite(v.asNumber()) &&
                              v.asNumber() == std::trunc(v.asNumber()));
    }, 1);
    defineMethod(constructor, "isSafeInteger", [](const FunctionCallInfo& info) -> Value {
        const Value v = info.argument(0);
        return Value::boolean(v.isNumber() && std::isfinite(v.asNumber()) &&
                              v.asNumber() == std::trunc(v.asNumber()) &&
                              std::fabs(v.asNumber()) <= NumberObject::MAX_SAFE_INTEGER);
    }, 1);

    defineHidden(constructor, "parseFloat", Value::object(parseFloat));
    defineHidden(constructor, "parseInt", Value::object(parseInt));
    return constructor;
}

} // namespace Zepra::Builtins
