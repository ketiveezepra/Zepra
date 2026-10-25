// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file promise_builtin.cpp
 * @brief JavaScript Promise constructor, prototype and statics
 */

#include "builtins/promise_builtin.hpp"
#include "builtins/builtin_helpers.hpp"
#include "runtime/async/promise.hpp"
#include "runtime/execution/vm.hpp"
#include "runtime/objects/well_known_symbols.hpp"

#include <memory>

namespace Zepra::Builtins {

using Runtime::Array;
using Runtime::FunctionCallInfo;
using Runtime::JSError;
using Runtime::Promise;
using Runtime::PromiseCapability;
using Runtime::ThrownException;
using Runtime::VM;
using Runtime::callValue;
using Runtime::catchAsValue;

namespace {

VM& runningVM() {
    VM* vm = VM::current();
    if (!vm) throw JSError("TypeError", "Promise used outside a running script");
    return *vm;
}

bool isCallable(const Value& value) {
    return value.isObject() && value.asObject()->isFunction();
}

bool isConstructor(const Value& value) {
    return isCallable(value) && static_cast<Function*>(value.asObject())->isConstructor();
}

std::string describe(const Value& value) {
    if (value.isString()) return "\"" + value.toString() + "\"";
    if (value.isObject()) return value.asObject()->isFunction() ? "function" : "#<Object>";
    return value.toString();
}

Promise* thisPromise(const Value& value, const char* method) {
    if (value.isObject()) {
        if (auto* promise = dynamic_cast<Promise*>(value.asObject())) return promise;
    }
    throw JSError("TypeError", std::string("Method Promise.prototype.") + method +
                               " called on incompatible receiver " + describe(value));
}

Value intrinsicConstructor(VM& vm) {
    return vm.intrinsic(VM::Intrinsic::Promise)->get("constructor");
}

// obj.name(...args)
Value invoke(VM& vm, const Value& target, const std::string& name, const std::vector<Value>& args) {
    Value method = vm.getMember(target, name);
    if (!isCallable(method)) throw JSError("TypeError", describe(target) + "." + name + " is not a function");
    return callValue(&vm, method, target, args);
}

// NewPromiseCapability(C)
PromiseCapability newCapability(VM& vm, const Value& constructor) {
    PromiseCapability capability;
    if (constructor.strictEquals(intrinsicConstructor(vm))) {
        auto* promise = new Promise();
        capability.intrinsic = promise;
        capability.promise = Value::object(promise);
        return capability;
    }
    if (!isConstructor(constructor)) {
        throw JSError("TypeError", describe(constructor) + " is not a constructor");
    }
    auto slots = std::make_shared<std::pair<Value, Value>>(Value::undefined(), Value::undefined());
    auto* executor = new Function("", [slots](const FunctionCallInfo& info) -> Value {
        if (!slots->first.isUndefined() || !slots->second.isUndefined()) {
            throw JSError("TypeError", "Promise executor has already been invoked with non-undefined arguments");
        }
        slots->first = info.argument(0);
        slots->second = info.argument(1);
        return Value::undefined();
    }, 2);
    capability.promise = vm.construct(static_cast<Function*>(constructor.asObject()), {Value::object(executor)});
    if (!isCallable(slots->first) || !isCallable(slots->second)) {
        throw JSError("TypeError", "Promise resolve or reject function is not callable");
    }
    capability.resolve = slots->first;
    capability.reject = slots->second;
    return capability;
}

// The capability's resolve or reject as a function scripts can call
Value capabilityFunction(const PromiseCapability& capability, bool resolve) {
    if (!capability.intrinsic) return resolve ? capability.resolve : capability.reject;
    Promise* promise = capability.intrinsic;
    return Value::object(new Function("", [promise, resolve](const FunctionCallInfo& info) -> Value {
        if (resolve) promise->resolve(info.argument(0));
        else promise->reject(info.argument(0));
        return Value::undefined();
    }, 1));
}

// SpeciesConstructor(promise, %Promise%)
Value speciesConstructor(VM& vm, const Value& object) {
    Value constructor = vm.getMember(object, "constructor");
    if (constructor.isUndefined()) return intrinsicConstructor(vm);
    if (!constructor.isObject()) throw JSError("TypeError", "The .constructor property is not an object");
    Value species = vm.getMember(constructor, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::Species));
    if (species.isUndefined() || species.isNull()) return intrinsicConstructor(vm);
    if (isConstructor(species)) return species;
    throw JSError("TypeError", "object.constructor[Symbol.species] is not a constructor");
}

// PromiseResolve(C, x)
Value promiseResolve(VM& vm, const Value& constructor, const Value& value) {
    if (value.isObject() && dynamic_cast<Promise*>(value.asObject())) {
        if (vm.getMember(value, "constructor").strictEquals(constructor)) return value;
    }
    PromiseCapability capability = newCapability(vm, constructor);
    capability.doResolve(value);
    return capability.promise;
}

Value settledRecord(VM& vm, bool fulfilled, const Value& value) {
    auto* record = new Object();
    record->setPrototype(vm.intrinsic(VM::Intrinsic::Object));
    record->set("status", makeString(fulfilled ? "fulfilled" : "rejected"));
    record->set(fulfilled ? "value" : "reason", value);
    return Value::object(record);
}

Value aggregateError(VM& vm, Function* constructor, const Value& errors) {
    const std::string message = "All promises were rejected";
    if (constructor) return vm.construct(constructor, {errors, makeString(message)});
    Value error = vm.newError("AggregateError", message);
    if (error.isObject()) defineHidden(error.asObject(), "errors", errors);
    return error;
}

enum class Combinator { All, AllSettled, Any, Race };

// Promise.all / allSettled / any / race: each item goes through C.resolve,
// then gets element functions that fill one slot of the result
Value combine(const FunctionCallInfo& info, Combinator kind, Function* aggregate) {
    VM& vm = runningVM();
    const Value constructor = info.thisValue();
    PromiseCapability capability = newCapability(vm, constructor);

    Value resolveMethod;
    Value thrown;
    if (!catchAsValue(&vm, [&] {
            resolveMethod = vm.getMember(constructor, "resolve");
            if (!isCallable(resolveMethod)) throw JSError("TypeError", "Promise resolve is not a function");
        }, thrown)) {
        capability.doReject(thrown);
        return capability.promise;
    }

    struct State {
        std::vector<Value> values;
        size_t remaining = 1;
    };
    auto state = std::make_shared<State>();
    auto finish = [state, kind, aggregate, capability](VM& vm) {
        Value list = Value::object(new Array(state->values));
        if (kind == Combinator::Any) capability.doReject(aggregateError(vm, aggregate, list));
        else capability.doResolve(list);
    };
    // The function that settles slot `index`; for allSettled the fulfil and
    // reject functions of one slot share their "already called" flag
    auto element = [state, kind, finish](size_t index, bool fulfilled, std::shared_ptr<bool> called) {
        return Value::object(new Function("", [state, kind, finish, index, fulfilled, called](const FunctionCallInfo& info) -> Value {
            if (*called) return Value::undefined();
            *called = true;
            VM& vm = runningVM();
            state->values[index] = kind == Combinator::AllSettled
                ? settledRecord(vm, fulfilled, info.argument(0)) : info.argument(0);
            if (--state->remaining == 0) finish(vm);
            return Value::undefined();
        }, 1));
    };

    const Value resolveFn = capabilityFunction(capability, true);
    const Value rejectFn = capabilityFunction(capability, false);
    size_t index = 0;
    if (!catchAsValue(&vm, [&] {
            vm.forOf(info.argument(0), [&](const Value& item) {
                const size_t slot = index++;
                if (kind != Combinator::Race) state->values.push_back(Value::undefined());
                Value next = callValue(&vm, resolveMethod, constructor, {item});
                auto called = std::make_shared<bool>(false);
                Value onFulfilled;
                Value onRejected;
                switch (kind) {
                    case Combinator::All:
                        onFulfilled = element(slot, true, called);
                        onRejected = rejectFn;
                        break;
                    case Combinator::AllSettled:
                        onFulfilled = element(slot, true, called);
                        onRejected = element(slot, false, called);
                        break;
                    case Combinator::Any:
                        onFulfilled = resolveFn;
                        onRejected = element(slot, false, called);
                        break;
                    case Combinator::Race:
                        onFulfilled = resolveFn;
                        onRejected = rejectFn;
                        break;
                }
                if (kind != Combinator::Race) state->remaining++;
                invoke(vm, next, "then", {onFulfilled, onRejected});
                return true;
            });
        }, thrown)) {
        capability.doReject(thrown);
        return capability.promise;
    }
    if (kind != Combinator::Race && --state->remaining == 0) finish(vm);
    return capability.promise;
}

} // namespace

Object* PromiseBuiltin::createPromisePrototype() {
    auto* prototype = new Object();

    // then(onFulfilled, onRejected): the derived promise comes from the species constructor
    defineMethod(prototype, "then", [](const FunctionCallInfo& info) -> Value {
        Promise* promise = thisPromise(info.thisValue(), "then");
        VM& vm = runningVM();
        PromiseCapability capability = newCapability(vm, speciesConstructor(vm, info.thisValue()));
        const Value onFulfilled = isCallable(info.argument(0)) ? info.argument(0) : Value::undefined();
        const Value onRejected = isCallable(info.argument(1)) ? info.argument(1) : Value::undefined();
        promise->addReactions(onFulfilled, onRejected, capability);
        return capability.promise;
    }, 2);

    // catch(onRejected) is this.then(undefined, onRejected), for any thenable
    defineMethod(prototype, "catch", [](const FunctionCallInfo& info) -> Value {
        return invoke(runningVM(), info.thisValue(), "then", {Value::undefined(), info.argument(0)});
    }, 1);

    // finally(onFinally): runs the callback, waits for what it returns, then
    // passes the original value or reason through
    defineMethod(prototype, "finally", [](const FunctionCallInfo& info) -> Value {
        VM& vm = runningVM();
        const Value self = info.thisValue();
        if (!self.isObject()) throw JSError("TypeError", "Promise.prototype.finally called on a non-object");
        const Value onFinally = info.argument(0);
        if (!isCallable(onFinally)) return invoke(vm, self, "then", {onFinally, onFinally});
        const Value constructor = speciesConstructor(vm, self);

        auto* thenFinally = new Function("", [constructor, onFinally](const FunctionCallInfo& inner) -> Value {
            VM& vm = runningVM();
            Value result = callValue(&vm, onFinally, Value::undefined(), {});
            Value promise = promiseResolve(vm, constructor, result);
            const Value value = inner.argument(0);
            auto* valueThunk = new Function("", [value](const FunctionCallInfo&) -> Value { return value; }, 0);
            return invoke(vm, promise, "then", {Value::object(valueThunk)});
        }, 1);
        auto* catchFinally = new Function("", [constructor, onFinally](const FunctionCallInfo& inner) -> Value {
            VM& vm = runningVM();
            Value result = callValue(&vm, onFinally, Value::undefined(), {});
            Value promise = promiseResolve(vm, constructor, result);
            const Value reason = inner.argument(0);
            auto* thrower = new Function("", [reason](const FunctionCallInfo&) -> Value {
                throw ThrownException(reason);
            }, 0);
            return invoke(vm, promise, "then", {Value::object(thrower)});
        }, 1);
        return invoke(vm, self, "then", {Value::object(thenFinally), Value::object(catchFinally)});
    }, 1);

    defineConstant(prototype, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::ToStringTag), makeString("Promise"));
    return prototype;
}

Function* PromiseBuiltin::createPromiseConstructor(Object* prototype, Function* aggregate) {
    auto* constructor = new Function("Promise", [prototype](const FunctionCallInfo& info) -> Value {
        // `new Promise()` arrives with a fresh object inheriting from Promise.prototype
        const Value self = info.thisValue();
        if (!self.isObject() || !inheritsFrom(self.asObject(), prototype)) {
            throw JSError("TypeError", "Promise constructor cannot be invoked without 'new'");
        }
        const Value executor = info.argument(0);
        if (!isCallable(executor)) {
            throw JSError("TypeError", "Promise resolver " + describe(executor) + " is not a function");
        }
        VM& vm = runningVM();
        auto* promise = new Promise();
        promise->setPrototype(self.asObject()->prototype());
        auto functions = promise->createResolvingFunctions();
        Value thrown;
        if (!catchAsValue(&vm, [&] {
                callValue(&vm, executor, Value::undefined(),
                          {Value::object(functions.first), Value::object(functions.second)});
            }, thrown)) {
            callValue(&vm, Value::object(functions.second), Value::undefined(), {thrown});
        }
        return Value::object(promise);
    }, 1);

    defineHidden(constructor, "prototype", Value::object(prototype));
    defineHidden(prototype, "constructor", Value::object(constructor));

    // get Promise[Symbol.species]: the constructor itself
    Runtime::PropertyDescriptor species;
    species.getter = Value::object(new Function("get [Symbol.species]",
        [](const FunctionCallInfo& info) -> Value { return info.thisValue(); }, 0));
    species.attributes = Runtime::PropertyAttribute::Configurable;
    constructor->defineProperty(Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::Species), species);

    defineMethod(constructor, "resolve", [](const FunctionCallInfo& info) -> Value {
        if (!info.thisValue().isObject()) throw JSError("TypeError", "PromiseResolve called on non-object");
        return promiseResolve(runningVM(), info.thisValue(), info.argument(0));
    }, 1);

    defineMethod(constructor, "reject", [](const FunctionCallInfo& info) -> Value {
        PromiseCapability capability = newCapability(runningVM(), info.thisValue());
        capability.doReject(info.argument(0));
        return capability.promise;
    }, 1);

    defineMethod(constructor, "all", [aggregate](const FunctionCallInfo& info) -> Value {
        return combine(info, Combinator::All, aggregate);
    }, 1);
    defineMethod(constructor, "allSettled", [aggregate](const FunctionCallInfo& info) -> Value {
        return combine(info, Combinator::AllSettled, aggregate);
    }, 1);
    defineMethod(constructor, "any", [aggregate](const FunctionCallInfo& info) -> Value {
        return combine(info, Combinator::Any, aggregate);
    }, 1);
    defineMethod(constructor, "race", [aggregate](const FunctionCallInfo& info) -> Value {
        return combine(info, Combinator::Race, aggregate);
    }, 1);

    // withResolvers(): { promise, resolve, reject }
    defineMethod(constructor, "withResolvers", [](const FunctionCallInfo& info) -> Value {
        VM& vm = runningVM();
        PromiseCapability capability = newCapability(vm, info.thisValue());
        auto* result = new Object();
        result->setPrototype(vm.intrinsic(VM::Intrinsic::Object));
        result->set("promise", capability.promise);
        result->set("resolve", capabilityFunction(capability, true));
        result->set("reject", capabilityFunction(capability, false));
        return Value::object(result);
    }, 0);

    // try(fn, ...args): fn's result or throw as a promise
    defineMethod(constructor, "try", [](const FunctionCallInfo& info) -> Value {
        VM& vm = runningVM();
        PromiseCapability capability = newCapability(vm, info.thisValue());
        std::vector<Value> args;
        for (size_t i = 1; i < info.argumentCount(); i++) args.push_back(info.argument(i));
        Value result;
        Value thrown;
        if (catchAsValue(&vm, [&] { result = callValue(&vm, info.argument(0), Value::undefined(), args); }, thrown)) {
            capability.doResolve(result);
        } else {
            capability.doReject(thrown);
        }
        return capability.promise;
    }, 1);

    return constructor;
}

} // namespace Zepra::Builtins
