// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file math.cpp
 * @brief JavaScript Math object implementation
 */

#include "builtins/math.hpp"
#include <algorithm>
#include <cmath>
#include "runtime/objects/function.hpp"

namespace Zepra::Builtins {

namespace {
// ToUint32 (ES2024 §7.1.7): truncate, then wrap modulo 2^32; NaN and the
// infinities are 0. A plain cast is undefined outside the uint32 range.
uint32_t toUint32(double number) {
    if (number >= 0 && number < 4294967296.0) return static_cast<uint32_t>(number);
    if (!std::isfinite(number)) return 0;
    double wrapped = std::fmod(std::trunc(number), 4294967296.0);
    if (wrapped < 0) wrapped += 4294967296.0;
    return static_cast<uint32_t>(wrapped);
}
} // namespace

std::mt19937& MathBuiltin::getRandomEngine() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    return gen;
}

// Basic operations
Value MathBuiltin::abs(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::abs(args[0].toNumber()));
}

Value MathBuiltin::ceil(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::ceil(args[0].toNumber()));
}

Value MathBuiltin::floor(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::floor(args[0].toNumber()));
}

Value MathBuiltin::round(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    // floor(x + 0.5), keeping -0 for [-0.5, -0] (std::round rounds halves away from zero)
    const double x = args[0].toNumber();
    if (!std::isfinite(x) || x == 0) return Value::number(x);
    if (x < 0 && x >= -0.5) return Value::number(-0.0);
    const double f = std::floor(x);
    return Value::number(x - f >= 0.5 ? f + 1 : f);
}

Value MathBuiltin::trunc(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::trunc(args[0].toNumber()));
}

Value MathBuiltin::sign(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    double val = args[0].toNumber();
    if (val > 0) return Value::number(1);
    if (val < 0) return Value::number(-1);
    return Value::number(val); // +0, -0, or NaN
}

// Power/root
Value MathBuiltin::pow(Runtime::Context*, const std::vector<Value>& args) {
    if (args.size() < 2) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::pow(args[0].toNumber(), args[1].toNumber()));
}

Value MathBuiltin::sqrt(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::sqrt(args[0].toNumber()));
}

Value MathBuiltin::cbrt(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::cbrt(args[0].toNumber()));
}

Value MathBuiltin::hypot(Runtime::Context*, const std::vector<Value>& args) {
    double sum = 0;
    bool nan = false;
    for (const auto& arg : args) {
        const double val = arg.toNumber();
        if (std::isinf(val)) return Value::number(std::numeric_limits<double>::infinity());
        if (std::isnan(val)) nan = true;
        sum += val * val;
    }
    return Value::number(nan ? std::numeric_limits<double>::quiet_NaN() : std::sqrt(sum));
}

// Exponential/logarithmic
Value MathBuiltin::exp(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::exp(args[0].toNumber()));
}

Value MathBuiltin::expm1(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::expm1(args[0].toNumber()));
}

Value MathBuiltin::log(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::log(args[0].toNumber()));
}

Value MathBuiltin::log10(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::log10(args[0].toNumber()));
}

Value MathBuiltin::log2(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::log2(args[0].toNumber()));
}

Value MathBuiltin::log1p(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::log1p(args[0].toNumber()));
}

// Trigonometric
Value MathBuiltin::sin(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::sin(args[0].toNumber()));
}

Value MathBuiltin::cos(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::cos(args[0].toNumber()));
}

Value MathBuiltin::tan(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::tan(args[0].toNumber()));
}

Value MathBuiltin::asin(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::asin(args[0].toNumber()));
}

Value MathBuiltin::acos(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::acos(args[0].toNumber()));
}

Value MathBuiltin::atan(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::atan(args[0].toNumber()));
}

Value MathBuiltin::atan2(Runtime::Context*, const std::vector<Value>& args) {
    if (args.size() < 2) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::atan2(args[0].toNumber(), args[1].toNumber()));
}

// Hyperbolic
Value MathBuiltin::sinh(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::sinh(args[0].toNumber()));
}

Value MathBuiltin::cosh(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::cosh(args[0].toNumber()));
}

Value MathBuiltin::tanh(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::tanh(args[0].toNumber()));
}

Value MathBuiltin::asinh(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::asinh(args[0].toNumber()));
}

Value MathBuiltin::acosh(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::acosh(args[0].toNumber()));
}

Value MathBuiltin::atanh(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(std::atanh(args[0].toNumber()));
}

// Min/max
Value MathBuiltin::min(Runtime::Context*, const std::vector<Value>& args) {
    double result = std::numeric_limits<double>::infinity();
    for (const auto& arg : args) {
        const double v = arg.toNumber();
        if (std::isnan(v)) return Value::number(v);
        if (v < result || (v == 0 && result == 0 && std::signbit(v))) result = v;
    }
    return Value::number(result);
}

Value MathBuiltin::max(Runtime::Context*, const std::vector<Value>& args) {
    double result = -std::numeric_limits<double>::infinity();
    for (const auto& arg : args) {
        const double v = arg.toNumber();
        if (std::isnan(v)) return Value::number(v);
        if (v > result || (v == 0 && result == 0 && !std::signbit(v))) result = v;
    }
    return Value::number(result);
}

// Random
Value MathBuiltin::random(Runtime::Context*, const std::vector<Value>&) {
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return Value::number(dist(getRandomEngine()));
}

// Bit manipulation
Value MathBuiltin::clz32(Runtime::Context*, const std::vector<Value>& args) {
    uint32_t n = args.empty() ? 0 : toUint32(args[0].toNumber());
    if (n == 0) return Value::number(32);
    int count = 0;
    while ((n & 0x80000000) == 0) { n <<= 1; count++; }
    return Value::number(count);
}

Value MathBuiltin::imul(Runtime::Context*, const std::vector<Value>& args) {
    // 32-bit multiplication with wraparound (done unsigned: signed overflow is UB)
    const uint32_t a = toUint32(args.size() > 0 ? args[0].toNumber() : 0);
    const uint32_t b = toUint32(args.size() > 1 ? args[1].toNumber() : 0);
    return Value::number(static_cast<double>(static_cast<int32_t>(a * b)));
}

Value MathBuiltin::fround(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::numeric_limits<double>::quiet_NaN());
    return Value::number(static_cast<float>(args[0].toNumber()));
}

Object* MathBuiltin::createMathObject() {
    Object* math = new Object();
    
    // Constants
    math->set("PI", Value::number(3.141592653589793));
    math->set("E", Value::number(2.718281828459045));
    math->set("LN2", Value::number(0.6931471805599453));
    math->set("LN10", Value::number(2.302585092994046));
    math->set("LOG2E", Value::number(1.4426950408889634));
    math->set("LOG10E", Value::number(0.4342944819032518));
    math->set("SQRT2", Value::number(1.4142135623730951));
    math->set("SQRT1_2", Value::number(0.7071067811865476));
    
    // Helper to create math function
    auto fn = [](const char* name, Value(*func)(Runtime::Context*, const std::vector<Value>&), int arity) {
        return Value::object(new Runtime::Function(name, 
            [func](const Runtime::FunctionCallInfo& info) -> Value {
                std::vector<Value> args;
                for (size_t i = 0; i < info.argumentCount(); i++) {
                    args.push_back(info.argument(i));
                }
                return func(info.context(), args);
            }, arity));
    };
    
    // Basic operations
    math->set("abs", fn("abs", abs, 1));
    math->set("ceil", fn("ceil", ceil, 1));
    math->set("floor", fn("floor", floor, 1));
    math->set("round", fn("round", round, 1));
    math->set("trunc", fn("trunc", trunc, 1));
    math->set("sign", fn("sign", sign, 1));
    
    // Power/root
    math->set("pow", fn("pow", pow, 2));
    math->set("sqrt", fn("sqrt", sqrt, 1));
    math->set("cbrt", fn("cbrt", cbrt, 1));
    math->set("hypot", fn("hypot", hypot, 2));
    
    // Exponential/logarithmic
    math->set("exp", fn("exp", exp, 1));
    math->set("expm1", fn("expm1", expm1, 1));
    math->set("log", fn("log", log, 1));
    math->set("log10", fn("log10", log10, 1));
    math->set("log2", fn("log2", log2, 1));
    math->set("log1p", fn("log1p", log1p, 1));
    
    // Trigonometric
    math->set("sin", fn("sin", sin, 1));
    math->set("cos", fn("cos", cos, 1));
    math->set("tan", fn("tan", tan, 1));
    math->set("asin", fn("asin", asin, 1));
    math->set("acos", fn("acos", acos, 1));
    math->set("atan", fn("atan", atan, 1));
    math->set("atan2", fn("atan2", atan2, 2));
    
    // Hyperbolic
    math->set("sinh", fn("sinh", sinh, 1));
    math->set("cosh", fn("cosh", cosh, 1));
    math->set("tanh", fn("tanh", tanh, 1));
    math->set("asinh", fn("asinh", asinh, 1));
    math->set("acosh", fn("acosh", acosh, 1));
    math->set("atanh", fn("atanh", atanh, 1));
    
    // Min/max
    math->set("min", fn("min", min, 2));
    math->set("max", fn("max", max, 2));
    
    // Random
    math->set("random", fn("random", random, 0));
    
    // Bit manipulation
    math->set("clz32", fn("clz32", clz32, 1));
    math->set("imul", fn("imul", imul, 2));
    math->set("fround", fn("fround", fround, 1));
    
    return math;
}

} // namespace Zepra::Builtins
