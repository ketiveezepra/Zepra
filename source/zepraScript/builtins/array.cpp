// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file array.cpp
 * @brief Array builtin implementation
 */

#include "builtins/array.hpp"
#include "builtins/builtin_helpers.hpp"
#include "builtins/list_iterator.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/value.hpp"
#include "runtime/execution/vm.hpp"
#include "runtime/execution/js_error.hpp"
#include "runtime/objects/well_known_symbols.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace Zepra::Builtins {

namespace {

// A relative index argument: negative counts back from the end. Clamped to
// [0, length]; `fallback` when the argument is undefined.
size_t relativeIndex(const Runtime::Value& arg, size_t length, size_t fallback) {
    if (arg.isUndefined()) return fallback;
    double d = arg.toNumber();
    if (std::isnan(d)) return 0;
    d = d < 0 ? std::ceil(d) : std::floor(d);
    if (d < 0) d = std::max(0.0, static_cast<double>(length) + d);
    return static_cast<size_t>(std::min(d, static_cast<double>(length)));
}

// Array.prototype.sort order: stable, undefined values last, and by string
// form unless a compare function is given. An exception thrown by the
// compare function ends the sort and reaches the caller.
void sortValues(std::vector<Runtime::Value>& values, const Runtime::Value& compare) {
    auto defined = std::stable_partition(values.begin(), values.end(),
        [](const Runtime::Value& v) { return !v.isUndefined(); });

    Runtime::Function* compareFn = nullptr;
    if (compare.isObject() && compare.asObject()->isFunction()) {
        compareFn = static_cast<Runtime::Function*>(compare.asObject());
    } else if (!compare.isUndefined()) {
        throw std::runtime_error("The comparison function must be either a function or undefined");
    }
    Runtime::VM* vm = Runtime::VM::current();
    if (compareFn && !vm) {
        throw std::runtime_error("Array.prototype.sort called outside a running script");
    }

    // A merge sort: an inconsistent compare function gives an unspecified
    // order, never an out-of-bounds access
    std::stable_sort(values.begin(), defined, [&](const Runtime::Value& a, const Runtime::Value& b) {
        if (compareFn) {
            double result = vm->executeCallback(compareFn, Runtime::Value::undefined(), {a, b}).toNumber();
            return result < 0;
        }
        return a.toString() < b.toString();
    });
}

} // namespace

// Array.isArray(value)
Runtime::Value ArrayBuiltin::isArray(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 1) {
        return Runtime::Value::boolean(false);
    }
    
    Runtime::Value arg = info.argument(0);
    if (!arg.isObject()) {
        return Runtime::Value::boolean(false);
    }
    
    return Runtime::Value::boolean(
        dynamic_cast<Runtime::Array*>(arg.asObject()) != nullptr
    );
}

// Array.from(arrayLike, mapFn?, thisArg?)
Runtime::Value ArrayBuiltin::from(const Runtime::FunctionCallInfo& info) {
    const Runtime::Value source = info.argument(0);
    if (source.isNull() || source.isUndefined()) {
        throw Runtime::JSError("TypeError", source.toString() + " is not iterable");
    }

    Runtime::Function* mapFn = nullptr;
    if (!info.argument(1).isUndefined()) {
        if (!info.argument(1).isObject() || !info.argument(1).asObject()->isFunction()) {
            throw Runtime::JSError("TypeError", info.argument(1).toString() + " is not a function");
        }
        mapFn = static_cast<Runtime::Function*>(info.argument(1).asObject());
    }
    const Runtime::Value thisArg = info.argument(2);

    Runtime::VM* vm = Runtime::VM::current();
    if (!vm) throw Runtime::JSError("TypeError", "Array.from called outside a running script");

    // Strings, arrays, Map/Set and anything with [Symbol.iterator] iterate;
    // other objects with a numeric length are read by index; the rest are empty
    bool readable = source.isString();
    if (source.isObject()) {
        readable = source.asObject()->isArray() ||
                   vm->getMember(source, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::Iterator)).isObject() ||
                   vm->getMember(source, "length").isNumber() ||
                   source.asObject()->objectType() == Runtime::ObjectType::Map ||
                   source.asObject()->objectType() == Runtime::ObjectType::Set;
    }

    std::vector<Runtime::Value> elements;
    if (readable) {
        double index = 0;
        vm->forOf(source, [&](const Runtime::Value& value) {
            elements.push_back(mapFn ? vm->executeCallback(mapFn, thisArg, {value, Runtime::Value::number(index)}) : value);
            index++;
            return true;
        });
    }
    return Runtime::Value::object(new Runtime::Array(std::move(elements)));
}

// Array.of(...items)
Runtime::Value ArrayBuiltin::of(const Runtime::FunctionCallInfo& info) {
    std::vector<Runtime::Value> elements;
    
    for (size_t i = 0; i < info.argumentCount(); i++) {
        elements.push_back(info.argument(i));
    }
    
    return Runtime::Value::object(new Runtime::Array(std::move(elements)));
}

// Array.prototype methods

// push(...items) - returns new length
Runtime::Value ArrayBuiltin::push(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject()) {
        return Runtime::Value::undefined();
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) {
        return Runtime::Value::undefined();
    }
    
    for (size_t i = 0; i < info.argumentCount(); i++) {
        arr->push(info.argument(i));
    }
    
    return Runtime::Value::number(static_cast<double>(arr->length()));
}

// pop() - returns removed element
Runtime::Value ArrayBuiltin::pop(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject()) {
        return Runtime::Value::undefined();
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr || arr->length() == 0) {
        return Runtime::Value::undefined();
    }
    
    return arr->pop();
}

// shift() - removes first element
Runtime::Value ArrayBuiltin::shift(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject()) {
        return Runtime::Value::undefined();
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr || arr->length() == 0) {
        return Runtime::Value::undefined();
    }
    
    return arr->shift();
}

// unshift(...items) - adds to beginning
Runtime::Value ArrayBuiltin::unshift(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject()) {
        return Runtime::Value::undefined();
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) {
        return Runtime::Value::undefined();
    }
    
    for (size_t i = info.argumentCount(); i > 0; i--) {
        arr->unshift(info.argument(i - 1));
    }
    
    return Runtime::Value::number(static_cast<double>(arr->length()));
}

// slice(start?, end?)
Runtime::Value ArrayBuiltin::slice(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject()) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    int64_t len = static_cast<int64_t>(arr->length());
    int64_t start = 0;
    int64_t end = len;
    
    if (info.argumentCount() > 0 && info.argument(0).isNumber()) {
        start = static_cast<int64_t>(info.argument(0).asNumber());
        if (start < 0) start = std::max(len + start, int64_t(0));
    }
    
    if (info.argumentCount() > 1 && info.argument(1).isNumber()) {
        end = static_cast<int64_t>(info.argument(1).asNumber());
        if (end < 0) end = std::max(len + end, int64_t(0));
    }
    
    std::vector<Runtime::Value> result;
    for (int64_t i = start; i < end && i < len; i++) {
        result.push_back(arr->at(static_cast<size_t>(i)));
    }
    
    return Runtime::Value::object(new Runtime::Array(std::move(result)));
}

// concat(...items)
Runtime::Value ArrayBuiltin::concat(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    std::vector<Runtime::Value> result;
    
    if (thisVal.isObject()) {
        if (auto* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject())) {
            for (size_t i = 0; i < arr->length(); i++) {
                result.push_back(arr->at(i));
            }
        }
    }
    
    for (size_t i = 0; i < info.argumentCount(); i++) {
        Runtime::Value arg = info.argument(i);
        if (arg.isObject()) {
            if (auto* arr = dynamic_cast<Runtime::Array*>(arg.asObject())) {
                for (size_t j = 0; j < arr->length(); j++) {
                    result.push_back(arr->at(j));
                }
                continue;
            }
        }
        result.push_back(arg);
    }
    
    return Runtime::Value::object(new Runtime::Array(std::move(result)));
}

// indexOf(searchElement, fromIndex?)
Runtime::Value ArrayBuiltin::indexOf(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject() || info.argumentCount() < 1) {
        return Runtime::Value::number(-1);
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) {
        return Runtime::Value::number(-1);
    }
    
    Runtime::Value searchElement = info.argument(0);
    size_t fromIndex = 0;
    
    if (info.argumentCount() > 1 && info.argument(1).isNumber()) {
        int64_t idx = static_cast<int64_t>(info.argument(1).asNumber());
        if (idx >= 0) {
            fromIndex = static_cast<size_t>(idx);
        } else {
            fromIndex = std::max(int64_t(0), static_cast<int64_t>(arr->length()) + idx);
        }
    }
    
    for (size_t i = fromIndex; i < arr->length(); i++) {
        if (arr->at(i).strictEquals(searchElement)) {
            return Runtime::Value::number(static_cast<double>(i));
        }
    }
    
    return Runtime::Value::number(-1);
}

// includes(searchElement, fromIndex?)
Runtime::Value ArrayBuiltin::includes(const Runtime::FunctionCallInfo& info) {
    Runtime::Value result = indexOf(info);
    return Runtime::Value::boolean(result.asNumber() >= 0);
}

// join(separator?)
Runtime::Value ArrayBuiltin::join(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject()) {
        return Runtime::Value::string(new Runtime::String(""));
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) {
        return Runtime::Value::string(new Runtime::String(""));
    }
    
    std::string separator = ",";
    if (!info.argument(0).isUndefined()) separator = info.argument(0).toString();

    // An array that contains itself joins as "" at the inner occurrence
    static thread_local std::vector<const Runtime::Array*> joining;
    if (std::find(joining.begin(), joining.end(), arr) != joining.end()) {
        return Runtime::Value::string(new Runtime::String(""));
    }
    joining.push_back(arr);
    struct Pop { ~Pop() { joining.pop_back(); } } pop;

    std::ostringstream ss;
    for (size_t i = 0; i < arr->length(); i++) {
        if (i > 0) ss << separator;
        Runtime::Value elem = arr->at(i);
        if (!elem.isNull() && !elem.isUndefined()) {
            ss << elem.toString();
        }
    }

    return Runtime::Value::string(new Runtime::String(ss.str()));
}

// reverse()
Runtime::Value ArrayBuiltin::reverse(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject()) {
        return thisVal;
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) {
        return thisVal;
    }
    
    arr->reverse();
    return thisVal;
}

// fill(value, start?, end?)
Runtime::Value ArrayBuiltin::fill(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject() || info.argumentCount() < 1) {
        return thisVal;
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) {
        return thisVal;
    }
    
    Runtime::Value fillValue = info.argument(0);
    int64_t len = static_cast<int64_t>(arr->length());
    int64_t start = 0;
    int64_t end = len;
    
    if (info.argumentCount() > 1 && info.argument(1).isNumber()) {
        start = static_cast<int64_t>(info.argument(1).asNumber());
        if (start < 0) start = std::max(len + start, int64_t(0));
    }
    
    if (info.argumentCount() > 2 && info.argument(2).isNumber()) {
        end = static_cast<int64_t>(info.argument(2).asNumber());
        if (end < 0) end = std::max(len + end, int64_t(0));
    }
    
    for (int64_t i = start; i < end && i < len; i++) {
        arr->set(static_cast<size_t>(i), fillValue);
    }
    
    return thisVal;
}

// forEach(callback, thisArg?)
Runtime::Value ArrayBuiltin::forEach(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject() || info.argumentCount() < 1) {
        return Runtime::Value::undefined();
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) {
        return Runtime::Value::undefined();
    }
    
    Runtime::Value callback = info.argument(0);
    if (!callback.isObject() || !callback.asObject()->isCallable()) {
        return Runtime::Value::undefined();
    }
    
    Runtime::Function* fn = dynamic_cast<Runtime::Function*>(callback.asObject());
    if (!fn) {
        return Runtime::Value::undefined();
    }
    
    Runtime::Value callThisArg = info.argumentCount() > 1 ? info.argument(1) : Runtime::Value::undefined();
    
    for (size_t i = 0; i < arr->length(); i++) {
        std::vector<Runtime::Value> args = {
            arr->at(i),
            Runtime::Value::number(static_cast<double>(i)),
            thisVal
        };
        // Execute callback using current VM
        if (auto* vm = Runtime::VM::current()) {
            vm->executeCallback(fn, callThisArg, args);
        }
    }
    
    return Runtime::Value::undefined();
}

// map(callback, thisArg?)
Runtime::Value ArrayBuiltin::map(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject() || info.argumentCount() < 1) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    Runtime::Value callback = info.argument(0);
    if (!callback.isObject() || !callback.asObject()->isCallable()) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    Runtime::Function* fn = dynamic_cast<Runtime::Function*>(callback.asObject());
    if (!fn) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    Runtime::Value callThisArg = info.argumentCount() > 1 ? info.argument(1) : Runtime::Value::undefined();
    std::vector<Runtime::Value> result;
    
    for (size_t i = 0; i < arr->length(); i++) {
        std::vector<Runtime::Value> args = {
            arr->at(i),
            Runtime::Value::number(static_cast<double>(i)),
            thisVal
        };
        Runtime::Value mapped = Runtime::Value::undefined();
        if (auto* vm = Runtime::VM::current()) {
            mapped = vm->executeCallback(fn, callThisArg, args);
        }
        result.push_back(mapped);
    }
    
    return Runtime::Value::object(new Runtime::Array(std::move(result)));
}

// filter(callback, thisArg?)
Runtime::Value ArrayBuiltin::filter(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject() || info.argumentCount() < 1) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    Runtime::Value callback = info.argument(0);
    if (!callback.isObject() || !callback.asObject()->isCallable()) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    Runtime::Function* fn = dynamic_cast<Runtime::Function*>(callback.asObject());
    if (!fn) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    Runtime::Value callThisArg = info.argumentCount() > 1 ? info.argument(1) : Runtime::Value::undefined();
    std::vector<Runtime::Value> result;
    
    for (size_t i = 0; i < arr->length(); i++) {
        Runtime::Value element = arr->at(i);
        std::vector<Runtime::Value> args = {
            element,
            Runtime::Value::number(static_cast<double>(i)),
            thisVal
        };
        Runtime::Value passed = Runtime::Value::boolean(false);
        if (auto* vm = Runtime::VM::current()) {
            passed = vm->executeCallback(fn, callThisArg, args);
        }
        if (passed.toBoolean()) {
            result.push_back(element);
        }
    }
    
    return Runtime::Value::object(new Runtime::Array(std::move(result)));
}

// reduce(callback, initialValue?)
Runtime::Value ArrayBuiltin::reduce(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject() || info.argumentCount() < 1) {
        return Runtime::Value::undefined();
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr || arr->length() == 0) {
        return info.argumentCount() > 1 ? info.argument(1) : Runtime::Value::undefined();
    }
    
    Runtime::Value callback = info.argument(0);
    if (!callback.isObject() || !callback.asObject()->isCallable()) {
        return Runtime::Value::undefined();
    }
    
    Runtime::Function* fn = dynamic_cast<Runtime::Function*>(callback.asObject());
    if (!fn) {
        return Runtime::Value::undefined();
    }
    
    size_t startIndex = 0;
    Runtime::Value accumulator;
    
    if (info.argumentCount() > 1) {
        accumulator = info.argument(1);
    } else {
        accumulator = arr->at(0);
        startIndex = 1;
    }
    
    for (size_t i = startIndex; i < arr->length(); i++) {
        std::vector<Runtime::Value> args = {
            accumulator,
            arr->at(i),
            Runtime::Value::number(static_cast<double>(i)),
            thisVal
        };
        if (auto* vm = Runtime::VM::current()) {
            accumulator = vm->executeCallback(fn, Runtime::Value::undefined(), args);
        }
    }
    
    return accumulator;
}

// length getter
Runtime::Value ArrayBuiltin::getLength(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject()) {
        return Runtime::Value::number(0);
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) {
        return Runtime::Value::number(0);
    }
    
    return Runtime::Value::number(static_cast<double>(arr->length()));
}

// find(callback, thisArg?) - returns first element satisfying callback
Runtime::Value ArrayBuiltin::find(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject() || info.argumentCount() < 1) {
        return Runtime::Value::undefined();
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) return Runtime::Value::undefined();
    
    Runtime::Value callback = info.argument(0);
    if (!callback.isObject() || !callback.asObject()->isCallable()) {
        return Runtime::Value::undefined();
    }
    
    Runtime::Function* fn = dynamic_cast<Runtime::Function*>(callback.asObject());
    if (!fn) return Runtime::Value::undefined();
    
    Runtime::Value callThisArg = info.argumentCount() > 1 ? info.argument(1) : Runtime::Value::undefined();
    
    for (size_t i = 0; i < arr->length(); i++) {
        Runtime::Value element = arr->at(i);
        std::vector<Runtime::Value> args = {element, Runtime::Value::number(static_cast<double>(i)), thisVal};
        Runtime::Value result = Runtime::Value::boolean(false);
        if (auto* vm = Runtime::VM::current()) {
            result = vm->executeCallback(fn, callThisArg, args);
        }
        if (result.toBoolean()) {
            return element;
        }
    }
    
    return Runtime::Value::undefined();
}

// findIndex(callback, thisArg?) - returns index of first element satisfying callback
Runtime::Value ArrayBuiltin::findIndex(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject() || info.argumentCount() < 1) {
        return Runtime::Value::number(-1);
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) return Runtime::Value::number(-1);
    
    Runtime::Value callback = info.argument(0);
    if (!callback.isObject() || !callback.asObject()->isCallable()) {
        return Runtime::Value::number(-1);
    }
    
    Runtime::Function* fn = dynamic_cast<Runtime::Function*>(callback.asObject());
    if (!fn) return Runtime::Value::number(-1);
    
    Runtime::Value callThisArg = info.argumentCount() > 1 ? info.argument(1) : Runtime::Value::undefined();
    
    for (size_t i = 0; i < arr->length(); i++) {
        Runtime::Value element = arr->at(i);
        std::vector<Runtime::Value> args = {element, Runtime::Value::number(static_cast<double>(i)), thisVal};
        Runtime::Value result = Runtime::Value::boolean(false);
        if (auto* vm = Runtime::VM::current()) {
            result = vm->executeCallback(fn, callThisArg, args);
        }
        if (result.toBoolean()) {
            return Runtime::Value::number(static_cast<double>(i));
        }
    }
    
    return Runtime::Value::number(-1);
}

// findLast(callback, thisArg?) - returns last element satisfying callback
Runtime::Value ArrayBuiltin::findLast(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject() || info.argumentCount() < 1) {
        return Runtime::Value::undefined();
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) return Runtime::Value::undefined();
    
    Runtime::Value callback = info.argument(0);
    if (!callback.isObject() || !callback.asObject()->isCallable()) {
        return Runtime::Value::undefined();
    }
    
    Runtime::Function* fn = dynamic_cast<Runtime::Function*>(callback.asObject());
    if (!fn) return Runtime::Value::undefined();
    
    Runtime::Value callThisArg = info.argumentCount() > 1 ? info.argument(1) : Runtime::Value::undefined();
    
    for (int64_t i = static_cast<int64_t>(arr->length()) - 1; i >= 0; i--) {
        Runtime::Value element = arr->at(static_cast<size_t>(i));
        std::vector<Runtime::Value> args = {element, Runtime::Value::number(static_cast<double>(i)), thisVal};
        Runtime::Value result = Runtime::Value::boolean(false);
        if (auto* vm = Runtime::VM::current()) {
            result = vm->executeCallback(fn, callThisArg, args);
        }
        if (result.toBoolean()) {
            return element;
        }
    }
    
    return Runtime::Value::undefined();
}

// every(callback, thisArg?) - returns true if all elements satisfy callback
Runtime::Value ArrayBuiltin::every(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject() || info.argumentCount() < 1) {
        return Runtime::Value::boolean(true);
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) return Runtime::Value::boolean(true);
    
    Runtime::Value callback = info.argument(0);
    if (!callback.isObject() || !callback.asObject()->isCallable()) {
        return Runtime::Value::boolean(true);
    }
    
    Runtime::Function* fn = dynamic_cast<Runtime::Function*>(callback.asObject());
    if (!fn) return Runtime::Value::boolean(true);
    
    Runtime::Value callThisArg = info.argumentCount() > 1 ? info.argument(1) : Runtime::Value::undefined();
    
    for (size_t i = 0; i < arr->length(); i++) {
        Runtime::Value element = arr->at(i);
        std::vector<Runtime::Value> args = {element, Runtime::Value::number(static_cast<double>(i)), thisVal};
        Runtime::Value result = Runtime::Value::boolean(true);
        if (auto* vm = Runtime::VM::current()) {
            result = vm->executeCallback(fn, callThisArg, args);
        }
        if (!result.toBoolean()) {
            return Runtime::Value::boolean(false);
        }
    }
    
    return Runtime::Value::boolean(true);
}

// some(callback, thisArg?) - returns true if any element satisfies callback
Runtime::Value ArrayBuiltin::some(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject() || info.argumentCount() < 1) {
        return Runtime::Value::boolean(false);
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) return Runtime::Value::boolean(false);
    
    Runtime::Value callback = info.argument(0);
    if (!callback.isObject() || !callback.asObject()->isCallable()) {
        return Runtime::Value::boolean(false);
    }
    
    Runtime::Function* fn = dynamic_cast<Runtime::Function*>(callback.asObject());
    if (!fn) return Runtime::Value::boolean(false);
    
    Runtime::Value callThisArg = info.argumentCount() > 1 ? info.argument(1) : Runtime::Value::undefined();
    
    for (size_t i = 0; i < arr->length(); i++) {
        Runtime::Value element = arr->at(i);
        std::vector<Runtime::Value> args = {element, Runtime::Value::number(static_cast<double>(i)), thisVal};
        Runtime::Value result = Runtime::Value::boolean(false);
        if (auto* vm = Runtime::VM::current()) {
            result = vm->executeCallback(fn, callThisArg, args);
        }
        if (result.toBoolean()) {
            return Runtime::Value::boolean(true);
        }
    }
    
    return Runtime::Value::boolean(false);
}

// flat(depth?) - flattens nested arrays
Runtime::Value ArrayBuiltin::flat(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject()) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) return Runtime::Value::object(new Runtime::Array({}));
    
    int64_t depth = 1;
    if (info.argumentCount() > 0 && info.argument(0).isNumber()) {
        depth = static_cast<int64_t>(info.argument(0).asNumber());
    }
    
    std::vector<Runtime::Value> result;
    
    std::function<void(Runtime::Array*, int64_t)> flattenHelper;
    flattenHelper = [&result, &flattenHelper](Runtime::Array* a, int64_t d) {
        for (size_t i = 0; i < a->length(); i++) {
            Runtime::Value elem = a->at(i);
            if (d > 0 && elem.isObject()) {
                if (auto* nested = dynamic_cast<Runtime::Array*>(elem.asObject())) {
                    flattenHelper(nested, d - 1);
                    continue;
                }
            }
            result.push_back(elem);
        }
    };
    
    flattenHelper(arr, depth);
    return Runtime::Value::object(new Runtime::Array(std::move(result)));
}

// at(index) - ES2022 - allows negative indexing
Runtime::Value ArrayBuiltin::at(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject()) {
        return Runtime::Value::undefined();
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) return Runtime::Value::undefined();
    
    if (info.argumentCount() < 1) {
        return arr->length() > 0 ? arr->at(0) : Runtime::Value::undefined();
    }
    
    int64_t index = static_cast<int64_t>(info.argument(0).toNumber());
    int64_t len = static_cast<int64_t>(arr->length());
    
    // Handle negative indices
    if (index < 0) {
        index = len + index;
    }
    
    if (index < 0 || index >= len) {
        return Runtime::Value::undefined();
    }
    
    return arr->at(static_cast<size_t>(index));
}

// toSorted(compareFn?) - ES2023 - returns new sorted array (non-mutating)
Runtime::Value ArrayBuiltin::toSorted(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject()) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) return Runtime::Value::object(new Runtime::Array({}));
    
    std::vector<Runtime::Value> copy = arr->elements();
    sortValues(copy, info.argument(0));
    return Runtime::Value::object(new Runtime::Array(std::move(copy)));
}

// sort(compareFn?) - sorts in place and returns the array
Runtime::Value ArrayBuiltin::sort(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    Runtime::Array* arr = thisVal.isObject() ? dynamic_cast<Runtime::Array*>(thisVal.asObject()) : nullptr;
    if (!arr) return thisVal;

    std::vector<Runtime::Value> sorted = arr->elements();
    sortValues(sorted, info.argument(0));
    for (size_t i = 0; i < sorted.size(); i++) {
        arr->set(i, sorted[i]);
    }
    return thisVal;
}

// splice(start, deleteCount?, ...items) - returns the removed elements
Runtime::Value ArrayBuiltin::splice(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    Runtime::Array* arr = thisVal.isObject() ? dynamic_cast<Runtime::Array*>(thisVal.asObject()) : nullptr;
    if (!arr) return Runtime::Value::object(new Runtime::Array());

    const size_t length = arr->length();
    const size_t start = relativeIndex(info.argument(0), length, 0);
    size_t deleteCount = 0;
    if (info.argumentCount() == 1) {
        deleteCount = length - start;
    } else if (info.argumentCount() > 1) {
        double d = info.argument(1).toNumber();
        if (!std::isnan(d) && d > 0) {
            deleteCount = static_cast<size_t>(std::min(std::floor(d), static_cast<double>(length - start)));
        }
    }

    const auto& elements = arr->elements();
    std::vector<Runtime::Value> removed(elements.begin() + static_cast<std::ptrdiff_t>(start),
                                        elements.begin() + static_cast<std::ptrdiff_t>(start + deleteCount));
    std::vector<Runtime::Value> items;
    for (size_t i = 2; i < info.argumentCount(); i++) {
        items.push_back(info.argument(i));
    }
    arr->splice(start, deleteCount, items);
    return Runtime::Value::object(new Runtime::Array(std::move(removed)));
}

// lastIndexOf(searchElement, fromIndex?)
Runtime::Value ArrayBuiltin::lastIndexOf(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    Runtime::Array* arr = thisVal.isObject() ? dynamic_cast<Runtime::Array*>(thisVal.asObject()) : nullptr;
    if (!arr || arr->length() == 0) return Runtime::Value::number(-1);

    const size_t length = arr->length();
    double from = info.argumentCount() > 1 ? info.argument(1).toNumber() : static_cast<double>(length - 1);
    if (std::isnan(from)) from = 0;
    if (from < 0) from += static_cast<double>(length);
    if (from < 0) return Runtime::Value::number(-1);
    size_t i = static_cast<size_t>(std::min(std::floor(from), static_cast<double>(length - 1))) + 1;
    while (i-- > 0) {
        if (arr->at(i).strictEquals(info.argument(0))) {
            return Runtime::Value::number(static_cast<double>(i));
        }
    }
    return Runtime::Value::number(-1);
}

// copyWithin(target, start, end?) - copies a range inside the array
Runtime::Value ArrayBuiltin::copyWithin(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    Runtime::Array* arr = thisVal.isObject() ? dynamic_cast<Runtime::Array*>(thisVal.asObject()) : nullptr;
    if (!arr) return thisVal;

    const size_t length = arr->length();
    const size_t target = relativeIndex(info.argument(0), length, 0);
    const size_t start = relativeIndex(info.argument(1), length, 0);
    const size_t end = relativeIndex(info.argument(2), length, length);
    if (start >= end || target >= length) return thisVal;

    const size_t count = std::min(end - start, length - target);
    const auto& elements = arr->elements();
    std::vector<Runtime::Value> range(elements.begin() + static_cast<std::ptrdiff_t>(start),
                                      elements.begin() + static_cast<std::ptrdiff_t>(start + count));
    for (size_t i = 0; i < count; i++) {
        arr->set(target + i, range[i]);
    }
    return thisVal;
}

// toReversed() - ES2023 - returns new reversed array (non-mutating)
Runtime::Value ArrayBuiltin::toReversed(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject()) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) return Runtime::Value::object(new Runtime::Array({}));
    
    std::vector<Runtime::Value> copy;
    for (int64_t i = static_cast<int64_t>(arr->length()) - 1; i >= 0; i--) {
        copy.push_back(arr->at(static_cast<size_t>(i)));
    }
    
    return Runtime::Value::object(new Runtime::Array(std::move(copy)));
}

// toSpliced(start, deleteCount, ...items) - ES2023 - non-mutating splice
Runtime::Value ArrayBuiltin::toSpliced(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject()) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) return Runtime::Value::object(new Runtime::Array({}));
    
    int64_t len = static_cast<int64_t>(arr->length());
    int64_t start = 0;
    int64_t deleteCount = len;
    
    if (info.argumentCount() > 0 && info.argument(0).isNumber()) {
        start = static_cast<int64_t>(info.argument(0).asNumber());
        if (start < 0) start = std::max(len + start, int64_t(0));
        if (start > len) start = len;
    }
    
    if (info.argumentCount() > 1 && info.argument(1).isNumber()) {
        deleteCount = static_cast<int64_t>(info.argument(1).asNumber());
        if (deleteCount < 0) deleteCount = 0;
        if (deleteCount > len - start) deleteCount = len - start;
    }
    
    std::vector<Runtime::Value> result;
    
    // Copy elements before start
    for (int64_t i = 0; i < start; i++) {
        result.push_back(arr->at(static_cast<size_t>(i)));
    }
    
    // Add inserted elements
    for (size_t i = 2; i < info.argumentCount(); i++) {
        result.push_back(info.argument(i));
    }
    
    // Copy elements after deleted section
    for (int64_t i = start + deleteCount; i < len; i++) {
        result.push_back(arr->at(static_cast<size_t>(i)));
    }
    
    return Runtime::Value::object(new Runtime::Array(std::move(result)));
}

// with(index, value) - ES2023 - returns new array with element replaced
Runtime::Value ArrayBuiltin::with(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject() || info.argumentCount() < 2) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) return Runtime::Value::object(new Runtime::Array({}));
    
    int64_t index = static_cast<int64_t>(info.argument(0).toNumber());
    Runtime::Value value = info.argument(1);
    int64_t len = static_cast<int64_t>(arr->length());
    
    // Handle negative index
    if (index < 0) {
        index = len + index;
    }
    
    if (index < 0 || index >= len) {
        throw std::runtime_error("Invalid index for Array.prototype.with");
    }
    
    std::vector<Runtime::Value> copy;
    for (size_t i = 0; i < arr->length(); i++) {
        if (static_cast<int64_t>(i) == index) {
            copy.push_back(value);
        } else {
            copy.push_back(arr->at(i));
        }
    }
    
    return Runtime::Value::object(new Runtime::Array(std::move(copy)));
}

// flatMap(callback, thisArg?) - ES2019
Runtime::Value ArrayBuiltin::flatMap(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject() || info.argumentCount() < 1) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) return Runtime::Value::object(new Runtime::Array({}));
    
    Runtime::Value callback = info.argument(0);
    if (!callback.isObject() || !callback.asObject()->isCallable()) {
        return Runtime::Value::object(new Runtime::Array({}));
    }
    
    Runtime::Function* fn = dynamic_cast<Runtime::Function*>(callback.asObject());
    if (!fn) return Runtime::Value::object(new Runtime::Array({}));
    
    Runtime::Value callThisArg = info.argumentCount() > 1 ? info.argument(1) : Runtime::Value::undefined();
    std::vector<Runtime::Value> result;
    
    for (size_t i = 0; i < arr->length(); i++) {
        Runtime::Value element = arr->at(i);
        std::vector<Runtime::Value> args = {element, Runtime::Value::number(static_cast<double>(i)), thisVal};
        Runtime::FunctionCallInfo callInfo(info.context(), callThisArg, args);
        Runtime::Value mapped = fn->builtinFunction()(callInfo);
        
        // Flatten one level
        if (mapped.isObject()) {
            if (auto* nested = dynamic_cast<Runtime::Array*>(mapped.asObject())) {
                for (size_t j = 0; j < nested->length(); j++) {
                    result.push_back(nested->at(j));
                }
                continue;
            }
        }
        result.push_back(mapped);
    }
    
    return Runtime::Value::object(new Runtime::Array(std::move(result)));
}

// reduceRight(callback, initialValue?) - reduces from right to left
Runtime::Value ArrayBuiltin::reduceRight(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject() || info.argumentCount() < 1) {
        return Runtime::Value::undefined();
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr || arr->length() == 0) {
        return info.argumentCount() > 1 ? info.argument(1) : Runtime::Value::undefined();
    }
    
    Runtime::Value callback = info.argument(0);
    if (!callback.isObject() || !callback.asObject()->isCallable()) {
        return Runtime::Value::undefined();
    }
    
    Runtime::Function* fn = dynamic_cast<Runtime::Function*>(callback.asObject());
    if (!fn) return Runtime::Value::undefined();
    
    int64_t len = static_cast<int64_t>(arr->length());
    int64_t startIndex = len - 1;
    Runtime::Value accumulator;
    
    if (info.argumentCount() > 1) {
        accumulator = info.argument(1);
    } else {
        accumulator = arr->at(static_cast<size_t>(startIndex));
        startIndex--;
    }
    
    for (int64_t i = startIndex; i >= 0; i--) {
        std::vector<Runtime::Value> args = {
            accumulator,
            arr->at(static_cast<size_t>(i)),
            Runtime::Value::number(static_cast<double>(i)),
            thisVal
        };
        Runtime::FunctionCallInfo callInfo(info.context(), Runtime::Value::undefined(), args);
        accumulator = fn->builtinFunction()(callInfo);
    }
    
    return accumulator;
}

// findLastIndex(callback, thisArg?) - returns index of last element satisfying callback
Runtime::Value ArrayBuiltin::findLastIndex(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject() || info.argumentCount() < 1) {
        return Runtime::Value::number(-1);
    }
    
    Runtime::Array* arr = dynamic_cast<Runtime::Array*>(thisVal.asObject());
    if (!arr) return Runtime::Value::number(-1);
    
    Runtime::Value callback = info.argument(0);
    if (!callback.isObject() || !callback.asObject()->isCallable()) {
        return Runtime::Value::number(-1);
    }
    
    Runtime::Function* fn = dynamic_cast<Runtime::Function*>(callback.asObject());
    if (!fn) return Runtime::Value::number(-1);
    
    Runtime::Value callThisArg = info.argumentCount() > 1 ? info.argument(1) : Runtime::Value::undefined();
    
    for (int64_t i = static_cast<int64_t>(arr->length()) - 1; i >= 0; i--) {
        Runtime::Value element = arr->at(static_cast<size_t>(i));
        std::vector<Runtime::Value> args = {element, Runtime::Value::number(static_cast<double>(i)), thisVal};
        Runtime::FunctionCallInfo callInfo(info.context(), callThisArg, args);
        Runtime::Value result = fn->builtinFunction()(callInfo);
        if (result.toBoolean()) {
            return Runtime::Value::number(static_cast<double>(i));
        }
    }
    
    return Runtime::Value::number(-1);
}

// Create Array prototype object
Runtime::Object* ArrayBuiltin::createArrayPrototype(Runtime::Context*) {
    Runtime::Object* prototype = new Runtime::Object();
    
    // Instance methods
    prototype->set("push", Runtime::Value::object(new Runtime::Function("push", push, 1)));
    prototype->set("pop", Runtime::Value::object(new Runtime::Function("pop", pop, 0)));
    prototype->set("shift", Runtime::Value::object(new Runtime::Function("shift", shift, 0)));
    prototype->set("unshift", Runtime::Value::object(new Runtime::Function("unshift", unshift, 1)));
    prototype->set("slice", Runtime::Value::object(new Runtime::Function("slice", slice, 2)));
    prototype->set("concat", Runtime::Value::object(new Runtime::Function("concat", concat, 1)));
    prototype->set("indexOf", Runtime::Value::object(new Runtime::Function("indexOf", indexOf, 1)));
    prototype->set("lastIndexOf", Runtime::Value::object(new Runtime::Function("lastIndexOf", lastIndexOf, 1)));
    prototype->set("splice", Runtime::Value::object(new Runtime::Function("splice", splice, 2)));
    prototype->set("sort", Runtime::Value::object(new Runtime::Function("sort", sort, 1)));
    prototype->set("copyWithin", Runtime::Value::object(new Runtime::Function("copyWithin", copyWithin, 2)));
    prototype->set("includes", Runtime::Value::object(new Runtime::Function("includes", includes, 1)));
    prototype->set("join", Runtime::Value::object(new Runtime::Function("join", join, 1)));
    prototype->set("reverse", Runtime::Value::object(new Runtime::Function("reverse", reverse, 0)));
    prototype->set("fill", Runtime::Value::object(new Runtime::Function("fill", fill, 3)));
    
    // Iteration methods
    prototype->set("forEach", Runtime::Value::object(new Runtime::Function("forEach", forEach, 1)));
    prototype->set("map", Runtime::Value::object(new Runtime::Function("map", map, 1)));
    prototype->set("filter", Runtime::Value::object(new Runtime::Function("filter", filter, 1)));
    prototype->set("reduce", Runtime::Value::object(new Runtime::Function("reduce", reduce, 1)));
    
    // ES6 methods
    prototype->set("find", Runtime::Value::object(new Runtime::Function("find", find, 1)));
    prototype->set("findIndex", Runtime::Value::object(new Runtime::Function("findIndex", findIndex, 1)));
    prototype->set("findLast", Runtime::Value::object(new Runtime::Function("findLast", findLast, 1)));
    prototype->set("findLastIndex", Runtime::Value::object(new Runtime::Function("findLastIndex", findLastIndex, 1)));
    prototype->set("every", Runtime::Value::object(new Runtime::Function("every", every, 1)));
    prototype->set("some", Runtime::Value::object(new Runtime::Function("some", some, 1)));
    prototype->set("flat", Runtime::Value::object(new Runtime::Function("flat", flat, 1)));
    prototype->set("flatMap", Runtime::Value::object(new Runtime::Function("flatMap", flatMap, 1)));
    prototype->set("reduceRight", Runtime::Value::object(new Runtime::Function("reduceRight", reduceRight, 1)));
    
    // ES2022+ methods
    prototype->set("at", Runtime::Value::object(new Runtime::Function("at", at, 1)));
    
    // ES2023+ methods (non-mutating array operations)
    prototype->set("toSorted", Runtime::Value::object(new Runtime::Function("toSorted", toSorted, 1)));
    prototype->set("toReversed", Runtime::Value::object(new Runtime::Function("toReversed", toReversed, 0)));
    prototype->set("toSpliced", Runtime::Value::object(new Runtime::Function("toSpliced", toSpliced, 2)));
    prototype->set("with", Runtime::Value::object(new Runtime::Function("with", with, 2)));

    // Conversion and iteration. toString joins with commas, so String([1, 2])
    // and [1, 2] + "" give "1,2".
    defineMethod(prototype, "toString", [](const Runtime::FunctionCallInfo& info) -> Runtime::Value {
        const Runtime::Value self = info.thisValue();
        if (self.isObject() && dynamic_cast<Runtime::Array*>(self.asObject())) {
            const std::vector<Runtime::Value> none;
            return join(Runtime::FunctionCallInfo(info.context(), self, none));
        }
        return Runtime::Value::string(new Runtime::String("[object Object]"));
    }, 0);

    auto elementsOf = [](const Runtime::FunctionCallInfo& info, const char* method) -> const std::vector<Runtime::Value>& {
        const Runtime::Value self = info.thisValue();
        auto* array = self.isObject() ? dynamic_cast<Runtime::Array*>(self.asObject()) : nullptr;
        if (!array) {
            throw Runtime::JSError("TypeError", std::string("Array.prototype.") + method + " called on a non-array");
        }
        return array->elements();
    };

    defineMethod(prototype, "keys", [elementsOf](const Runtime::FunctionCallInfo& info) -> Runtime::Value {
        const auto& elements = elementsOf(info, "keys");
        std::vector<Runtime::Value> keys;
        for (size_t i = 0; i < elements.size(); i++) keys.push_back(Runtime::Value::number(static_cast<double>(i)));
        return IteratorBuiltin::makeListIterator(std::move(keys));
    }, 0);

    Runtime::Function* values = defineMethod(prototype, "values", [elementsOf](const Runtime::FunctionCallInfo& info) -> Runtime::Value {
        return IteratorBuiltin::makeListIterator(elementsOf(info, "values"));
    }, 0);
    defineHidden(prototype, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::Iterator), Runtime::Value::object(values));

    defineMethod(prototype, "entries", [elementsOf](const Runtime::FunctionCallInfo& info) -> Runtime::Value {
        const auto& elements = elementsOf(info, "entries");
        std::vector<Runtime::Value> pairs;
        for (size_t i = 0; i < elements.size(); i++) {
            pairs.push_back(Runtime::Value::object(new Runtime::Array(
                std::vector<Runtime::Value>{Runtime::Value::number(static_cast<double>(i)), elements[i]})));
        }
        return IteratorBuiltin::makeListIterator(std::move(pairs));
    }, 0);

    return prototype;
}

} // namespace Zepra::Builtins

