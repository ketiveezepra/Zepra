// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file value.cpp
 * @brief JavaScript value implementation
 */

#include "runtime/objects/value.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/number_conversion.hpp"
#include "runtime/builtins_api/bigint_object.hpp"
#include <sstream>
#include <cmath>
#include <limits>

namespace Zepra::Runtime {

ValueType Value::type() const {
    if (isUndefined()) return ValueType::Undefined;
    if (isNull()) return ValueType::Null;
    if (isBoolean()) return ValueType::Boolean;
    if (isNumber()) return ValueType::Number;
    if (isString()) return ValueType::String;
    if (isBigInt()) return ValueType::BigInt;
    if (isObject()) return ValueType::Object;
    return ValueType::Undefined;
}

bool Value::isFalsy() const {
    if (isUndefined() || isNull()) return true;
    if (isBoolean()) return !asBoolean();
    if (isNumber()) {
        double n = asNumber();
        return n == 0.0 || std::isnan(n);
    }
    if (isString()) return asString()->value().empty();
    return false;
}

Function* Value::asFunction() const {
    if (!isObject()) return nullptr;
    Object* obj = asObject();
    if (!obj->isFunction()) return nullptr;
    return static_cast<Function*>(obj);
}

Array* Value::asArray() const {
    if (!isObject()) return nullptr;
    Object* obj = asObject();
    if (!obj->isArray()) return nullptr;
    return static_cast<Array*>(obj);
}

bool Value::isBigInt() const {
    if (!isObject()) return false;
    Object* obj = asObject();
    return obj && obj->objectType() == ObjectType::BigInt;
}

BigIntObject* Value::asBigInt() const {
    if (!isBigInt()) return nullptr;
    return static_cast<BigIntObject*>(asObject());
}

Value Value::bigint(BigIntObject* bi) {
    return Value::object(static_cast<Object*>(bi));
}

bool Value::toBoolean() const {
    return !isFalsy();
}

double Value::toNumber() const {
    switch (type()) {
        case ValueType::Undefined:
            return std::nan("");
        case ValueType::Null:
            return 0.0;
        case ValueType::Boolean:
            return asBoolean() ? 1.0 : 0.0;
        case ValueType::Number:
            return asNumber();
        case ValueType::String:
            return NumberConversion::stringToNumber(asString()->value());
        case ValueType::BigInt:
            throw std::runtime_error("TypeError: Cannot convert a BigInt value to a number");
        case ValueType::Object: {
            // ToPrimitive → valueOf → toString coercion chain
            Object* obj = asObject();
            if (obj) {
                Value valueOf = obj->get("valueOf");
                if (valueOf.isObject() && valueOf.asObject()->isFunction()) {
                    // Call valueOf on the object
                    Function* fn = static_cast<Function*>(valueOf.asObject());
                    Value result = fn->call(nullptr, Value::object(obj), {});
                    if (result.isNumber()) return result.asNumber();
                    if (result.isString()) return NumberConversion::stringToNumber(result.toString());
                }
            }
            return std::nan("");
        }
        default:
            return std::nan("");
    }
}

std::string Value::toString() const {
    switch (type()) {
        case ValueType::Undefined:
            return "undefined";
        case ValueType::Null:
            return "null";
        case ValueType::Boolean:
            return asBoolean() ? "true" : "false";
        case ValueType::Number:
            return NumberConversion::toString(asNumber());
        case ValueType::String:
            return asString()->value();
        case ValueType::Object: {
            Object* obj = asObject();
            if (obj) {
                // Check object type for appropriate string representation
                if (obj->isArray()) {
                    return "[object Array]";
                } else if (obj->isFunction()) {
                    return "[object Function]";
                }
            }
            return "[object Object]";
        }
        default:
            return "";
    }
}

Object* Value::toObject() const {
    if (isObject()) return asObject();
    
    // Boxing for primitives - wrap primitive values in objects
    // This is used when treating primitives as objects (e.g., "hello".length)
    if (isString()) {
        // String primitive is already a String object in our implementation
        return asObject();
    }
    if (isNumber()) {
        // Box number into a Number wrapper object
        Object* wrapper = new Object();
        wrapper->set("__primitiveValue__", Value::number(asNumber()));
        return wrapper;
    }
    if (isBoolean()) {
        // Box boolean into a Boolean wrapper object
        Object* wrapper = new Object();
        wrapper->set("__primitiveValue__", Value::boolean(asBoolean()));
        return wrapper;
    }
    
    // undefined and null throw TypeError in strict mode
    return nullptr;
}

bool Value::equals(const Value& other) const {
    // Same type, use strict equality
    if (type() == other.type()) {
        return strictEquals(other);
    }
    
    // null == undefined
    if ((isNull() && other.isUndefined()) ||
        (isUndefined() && other.isNull())) {
        return true;
    }
    
    // Number comparisons
    if (isNumber() && other.isNumber()) {
        return asNumber() == other.asNumber();
    }
    
    // String to number
    if (isNumber() && other.isString()) {
        return asNumber() == other.toNumber();
    }
    if (isString() && other.isNumber()) {
        return toNumber() == other.asNumber();
    }
    
    // Boolean to number
    if (isBoolean()) {
        return Value::number(toNumber()).equals(other);
    }
    if (other.isBoolean()) {
        return equals(Value::number(other.toNumber()));
    }
    
    return false;
}

bool Value::strictEquals(const Value& other) const {
    if (type() != other.type()) return false;
    
    switch (type()) {
        case ValueType::Undefined:
        case ValueType::Null:
            return true;
        case ValueType::Boolean:
            return asBoolean() == other.asBoolean();
        case ValueType::Number: {
            double a = asNumber(), b = other.asNumber();
            if (std::isnan(a) || std::isnan(b)) return false;
            return a == b;
        }
        case ValueType::String:
            return asString()->value() == other.asString()->value();
        case ValueType::Object:
            return asObject() == other.asObject();
        default:
            return false;
    }
}

// Arithmetic operations
Value Value::add(const Value& left, const Value& right) {
    // String concatenation
    if (left.isString() || right.isString()) {
        return Value::string(new String(left.toString() + right.toString()));
    }
    
    // Numeric addition
    return Value::number(left.toNumber() + right.toNumber());
}

Value Value::subtract(const Value& left, const Value& right) {
    return Value::number(left.toNumber() - right.toNumber());
}

Value Value::multiply(const Value& left, const Value& right) {
    return Value::number(left.toNumber() * right.toNumber());
}

Value Value::divide(const Value& left, const Value& right) {
    double a = left.toNumber();
    double b = right.toNumber();
    
    if (b == 0.0) {
        if (a == 0.0) return Value::number(std::nan(""));
        return Value::number(a > 0 ? std::numeric_limits<double>::infinity() 
                                   : -std::numeric_limits<double>::infinity());
    }
    
    return Value::number(a / b);
}

Value Value::modulo(const Value& left, const Value& right) {
    double a = left.toNumber();
    double b = right.toNumber();
    return Value::number(std::fmod(a, b));
}

Value Value::power(const Value& left, const Value& right) {
    return Value::number(std::pow(left.toNumber(), right.toNumber()));
}

Value Value::bitwiseAnd(const Value& left, const Value& right) {
    int32_t a = static_cast<int32_t>(left.toNumber());
    int32_t b = static_cast<int32_t>(right.toNumber());
    return Value::number(static_cast<double>(a & b));
}

Value Value::bitwiseOr(const Value& left, const Value& right) {
    int32_t a = static_cast<int32_t>(left.toNumber());
    int32_t b = static_cast<int32_t>(right.toNumber());
    return Value::number(static_cast<double>(a | b));
}

Value Value::bitwiseXor(const Value& left, const Value& right) {
    int32_t a = static_cast<int32_t>(left.toNumber());
    int32_t b = static_cast<int32_t>(right.toNumber());
    return Value::number(static_cast<double>(a ^ b));
}

Value Value::bitwiseNot(const Value& val) {
    int32_t a = static_cast<int32_t>(val.toNumber());
    return Value::number(static_cast<double>(~a));
}

Value Value::leftShift(const Value& left, const Value& right) {
    int32_t a = static_cast<int32_t>(left.toNumber());
    uint32_t b = static_cast<uint32_t>(right.toNumber()) & 0x1F;
    return Value::number(static_cast<double>(a << b));
}

Value Value::rightShift(const Value& left, const Value& right) {
    int32_t a = static_cast<int32_t>(left.toNumber());
    uint32_t b = static_cast<uint32_t>(right.toNumber()) & 0x1F;
    return Value::number(static_cast<double>(a >> b));
}

Value Value::unsignedRightShift(const Value& left, const Value& right) {
    uint32_t a = static_cast<uint32_t>(left.toNumber());
    uint32_t b = static_cast<uint32_t>(right.toNumber()) & 0x1F;
    return Value::number(static_cast<double>(a >> b));
}

Value Value::negate(const Value& val) {
    return Value::number(-val.toNumber());
}

Value Value::logicalNot(const Value& val) {
    return Value::boolean(!val.toBoolean());
}

Value Value::lessThan(const Value& left, const Value& right) {
    if (left.isString() && right.isString()) {
        return Value::boolean(left.asString()->value() < right.asString()->value());
    }
    return Value::boolean(left.toNumber() < right.toNumber());
}

Value Value::lessEqual(const Value& left, const Value& right) {
    if (left.isString() && right.isString()) {
        return Value::boolean(left.asString()->value() <= right.asString()->value());
    }
    return Value::boolean(left.toNumber() <= right.toNumber());
}

Value Value::greaterThan(const Value& left, const Value& right) {
    if (left.isString() && right.isString()) {
        return Value::boolean(left.asString()->value() > right.asString()->value());
    }
    return Value::boolean(left.toNumber() > right.toNumber());
}

Value Value::greaterEqual(const Value& left, const Value& right) {
    if (left.isString() && right.isString()) {
        return Value::boolean(left.asString()->value() >= right.asString()->value());
    }
    return Value::boolean(left.toNumber() >= right.toNumber());
}

} // namespace Zepra::Runtime
