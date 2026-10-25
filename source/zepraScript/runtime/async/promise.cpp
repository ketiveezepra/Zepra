// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
#include "runtime/async/promise.hpp"
#include "runtime/execution/Sandbox.h"
#include "runtime/execution/js_error.hpp"
#include "runtime/execution/vm.hpp"
#include "runtime/objects/function.hpp"
#include <memory>

namespace Zepra::Runtime {

namespace {

bool isCallableValue(const Value& value) {
    return value.isObject() && value.asObject()->isFunction();
}

Value errorValue(VM* vm, const std::string& type, const std::string& message) {
    if (vm) return vm->newError(type, message);
    return Value::string(new String(type + ": " + message));
}

} // namespace

bool catchAsValue(VM* vm, const std::function<void()>& body, Value& thrown) {
    try {
        body();
        return true;
    } catch (const SecurityError&) {
        throw;  // Execution limits end the run; no script code may observe them
    } catch (const ThrownException& e) {
        thrown = e.value();
    } catch (const JSError& e) {
        thrown = errorValue(vm, e.type(), e.what());
    } catch (const std::exception& e) {
        if (vm && vm->isTerminationRequested()) throw;
        const std::string message = e.what();
        const bool tooDeep = message.find("tack overflow") != std::string::npos ||
                             message.find("call stack") != std::string::npos;
        thrown = errorValue(vm, tooDeep ? "RangeError" : "TypeError", message);
    }
    return false;
}

Value callValue(VM* vm, const Value& callee, const Value& thisValue, const std::vector<Value>& args) {
    if (!isCallableValue(callee)) {
        throw JSError("TypeError", (callee.isUndefined() ? std::string("undefined") : callee.toString()) +
                                   " is not a function");
    }
    auto* fn = static_cast<Function*>(callee.asObject());
    if (vm) return vm->executeCallback(fn, thisValue, args);
    return fn->call(nullptr, thisValue, args);
}

// ============================================================================
// PromiseCapability
// ============================================================================

void PromiseCapability::doResolve(const Value& value) const {
    if (intrinsic) {
        intrinsic->resolve(value);
    } else if (isCallableValue(resolve)) {
        callValue(VM::current(), resolve, Value::undefined(), {value});
    }
}

void PromiseCapability::doReject(const Value& reason) const {
    if (intrinsic) {
        intrinsic->reject(reason);
    } else if (isCallableValue(reject)) {
        callValue(VM::current(), reject, Value::undefined(), {reason});
    }
}

// ============================================================================
// Promise
// ============================================================================

Promise::Promise()
    : state_(PromiseState::Pending), result_(Value::undefined()), vm_(VM::current()) {
    objectType_ = ObjectType::Promise;
    if (vm_) setPrototype(vm_->intrinsic(VM::Intrinsic::Promise));
}

VM* Promise::vm() const {
    return vm_ ? vm_ : VM::current();
}

void Promise::enqueue(std::function<void()> job) {
    if (VM* owner = vm()) {
        owner->enqueueJob(std::move(job));
    } else {
        MicrotaskQueue::instance().enqueue(std::move(job));
    }
}

void Promise::resolve(const Value& value) {
    if (alreadyResolved_) return;
    alreadyResolved_ = true;
    resolveUnchecked(value);
}

void Promise::reject(const Value& reason) {
    if (alreadyResolved_) return;
    alreadyResolved_ = true;
    settle(PromiseState::Rejected, reason);
}

// The body of a promise resolve function once its record allows the call
void Promise::resolveUnchecked(const Value& resolution) {
    if (state_ != PromiseState::Pending) return;
    VM* owner = vm();
    if (resolution.isObject() && resolution.asObject() == this) {
        settle(PromiseState::Rejected, errorValue(owner, "TypeError", "Chaining cycle detected for promise #<Promise>"));
        return;
    }
    if (!resolution.isObject()) {
        settle(PromiseState::Fulfilled, resolution);
        return;
    }
    Value then;
    Value thrown;
    if (!catchAsValue(owner, [&] {
            then = owner ? owner->getMember(resolution, "then") : resolution.asObject()->get("then");
        }, thrown)) {
        settle(PromiseState::Rejected, thrown);
        return;
    }
    if (!isCallableValue(then)) {
        settle(PromiseState::Fulfilled, resolution);
        return;
    }
    // A thenable is followed from a job of its own (NewPromiseResolveThenableJob)
    Promise* self = this;
    enqueue([self, resolution, then, owner] {
        auto functions = self->createResolvingFunctions();
        Value error;
        if (!catchAsValue(owner, [&] {
                callValue(owner, then, resolution,
                          {Value::object(functions.first), Value::object(functions.second)});
            }, error)) {
            callValue(owner, Value::object(functions.second), Value::undefined(), {error});
        }
    });
}

void Promise::settle(PromiseState state, const Value& value) {
    if (state_ != PromiseState::Pending) return;
    state_ = state;
    result_ = value;
    if (state == PromiseState::Rejected && !isHandled_) {
        if (VM* owner = vm()) owner->trackPromiseRejection(this, false);
    }
    std::vector<Reaction> reactions;
    reactions.swap(reactions_);
    for (const Reaction& reaction : reactions) enqueueReaction(reaction);
}

std::pair<Function*, Function*> Promise::createResolvingFunctions() {
    auto alreadyResolved = std::make_shared<bool>(false);
    Promise* self = this;
    auto* resolveFn = new Function("", [self, alreadyResolved](const FunctionCallInfo& info) -> Value {
        if (*alreadyResolved) return Value::undefined();
        *alreadyResolved = true;
        self->resolveUnchecked(info.argument(0));
        return Value::undefined();
    }, 1);
    auto* rejectFn = new Function("", [self, alreadyResolved](const FunctionCallInfo& info) -> Value {
        if (*alreadyResolved) return Value::undefined();
        *alreadyResolved = true;
        self->settle(PromiseState::Rejected, info.argument(0));
        return Value::undefined();
    }, 1);
    return {resolveFn, rejectFn};
}

// A reaction job (NewPromiseReactionJob) for the settled state
void Promise::enqueueReaction(const Reaction& reaction) {
    const bool fulfilled = state_ == PromiseState::Fulfilled;
    const Value argument = result_;
    VM* owner = vm();
    enqueue([reaction, fulfilled, argument, owner] {
        if (reaction.isNative) {
            const auto& continuation = fulfilled ? reaction.nativeFulfilled : reaction.nativeRejected;
            if (continuation) continuation(argument);
            return;
        }
        const Value& handler = fulfilled ? reaction.onFulfilled : reaction.onRejected;
        if (!isCallableValue(handler)) {
            if (fulfilled) reaction.capability.doResolve(argument);
            else reaction.capability.doReject(argument);
            return;
        }
        Value handlerResult;
        Value thrown;
        if (catchAsValue(owner, [&] {
                handlerResult = callValue(owner, handler, Value::undefined(), {argument});
            }, thrown)) {
            reaction.capability.doResolve(handlerResult);
        } else {
            reaction.capability.doReject(thrown);
        }
    });
}

void Promise::addReactions(const Value& onFulfilled, const Value& onRejected, const PromiseCapability& capability) {
    Reaction reaction;
    reaction.onFulfilled = onFulfilled;
    reaction.onRejected = onRejected;
    reaction.capability = capability;
    if (state_ == PromiseState::Pending) {
        reactions_.push_back(std::move(reaction));
    } else {
        if (state_ == PromiseState::Rejected && !isHandled_) {
            if (VM* owner = vm()) owner->trackPromiseRejection(this, true);
        }
        enqueueReaction(reaction);
    }
    isHandled_ = true;
}

void Promise::addNativeReactions(std::function<void(const Value&)> onFulfilled,
                                 std::function<void(const Value&)> onRejected) {
    Reaction reaction;
    reaction.isNative = true;
    reaction.nativeFulfilled = std::move(onFulfilled);
    reaction.nativeRejected = std::move(onRejected);
    if (state_ == PromiseState::Pending) {
        reactions_.push_back(std::move(reaction));
    } else {
        if (state_ == PromiseState::Rejected && !isHandled_) {
            if (VM* owner = vm()) owner->trackPromiseRejection(this, true);
        }
        enqueueReaction(reaction);
    }
    isHandled_ = true;
}

Promise* Promise::promiseResolve(const Value& value) {
    if (value.isObject()) {
        if (auto* promise = dynamic_cast<Promise*>(value.asObject())) {
            // Only a promise whose constructor is %Promise% is used as is
            VM* owner = VM::current();
            if (!owner) return promise;
            Object* proto = owner->intrinsic(VM::Intrinsic::Promise);
            if (owner->getMember(value, "constructor").strictEquals(proto->get("constructor"))) {
                return promise;
            }
        }
    }
    auto* promise = new Promise();
    promise->resolve(value);
    return promise;
}

// ============================================================================
// C++ convenience API
// ============================================================================

Promise* Promise::then(PromiseCallback onFulfilled, PromiseCallback onRejected) {
    auto* derived = new Promise();
    VM* owner = vm();
    auto settleWith = [derived, owner](const PromiseCallback& callback, const Value& value) {
        Value result;
        Value thrown;
        if (catchAsValue(owner, [&] { result = callback(value); }, thrown)) derived->resolve(result);
        else derived->reject(thrown);
    };
    addNativeReactions(
        [onFulfilled, derived, settleWith](const Value& value) {
            if (onFulfilled) settleWith(onFulfilled, value);
            else derived->resolve(value);
        },
        [onRejected, derived, settleWith](const Value& reason) {
            if (onRejected) settleWith(onRejected, reason);
            else derived->reject(reason);
        });
    return derived;
}

Promise* Promise::catchError(PromiseCallback onRejected) {
    return then(nullptr, onRejected);
}

Promise* Promise::finally(PromiseCallback onFinally) {
    auto* derived = new Promise();
    addNativeReactions(
        [onFinally, derived](const Value& value) {
            if (onFinally) onFinally(Value::undefined());
            derived->resolve(value);
        },
        [onFinally, derived](const Value& reason) {
            if (onFinally) onFinally(Value::undefined());
            derived->reject(reason);
        });
    return derived;
}

Promise* Promise::resolved(const Value& value) {
    auto* promise = new Promise();
    promise->resolve(value);
    return promise;
}

Promise* Promise::rejected(const Value& reason) {
    auto* promise = new Promise();
    promise->reject(reason);
    return promise;
}

Promise* Promise::all(const std::vector<Promise*>& promises) {
    auto* result = new Promise();
    if (promises.empty()) {
        result->resolve(Value::object(new Array({})));
        return result;
    }
    struct State {
        std::vector<Value> values;
        size_t remaining;
    };
    auto state = std::make_shared<State>();
    state->values.resize(promises.size());
    state->remaining = promises.size();
    for (size_t i = 0; i < promises.size(); i++) {
        promises[i]->addNativeReactions(
            [result, state, i](const Value& value) {
                state->values[i] = value;
                if (--state->remaining == 0) result->resolve(Value::object(new Array(state->values)));
            },
            [result](const Value& reason) { result->reject(reason); });
    }
    return result;
}

Promise* Promise::race(const std::vector<Promise*>& promises) {
    auto* result = new Promise();  // An empty race never settles
    for (Promise* promise : promises) {
        promise->addNativeReactions(
            [result](const Value& value) { result->resolve(value); },
            [result](const Value& reason) { result->reject(reason); });
    }
    return result;
}

namespace {

Value settledRecord(bool fulfilled, const Value& value) {
    auto* record = new Object();
    if (VM* vm = VM::current()) record->setPrototype(vm->intrinsic(VM::Intrinsic::Object));
    record->set("status", Value::string(new String(fulfilled ? "fulfilled" : "rejected")));
    record->set(fulfilled ? "value" : "reason", value);
    return Value::object(record);
}

Value aggregateError(const std::vector<Value>& errors) {
    VM* vm = VM::current();
    Value error = errorValue(vm, "AggregateError", "All promises were rejected");
    if (!error.isObject()) {
        auto* object = new Object();
        object->set("name", Value::string(new String("AggregateError")));
        object->set("message", Value::string(new String("All promises were rejected")));
        error = Value::object(object);
    }
    PropertyDescriptor desc;
    desc.value = Value::object(new Array(errors));
    desc.attributes = PropertyAttribute::Writable | PropertyAttribute::Configurable;
    error.asObject()->defineProperty("errors", desc);
    return error;
}

} // namespace

Promise* Promise::allSettled(const std::vector<Promise*>& promises) {
    auto* result = new Promise();
    if (promises.empty()) {
        result->resolve(Value::object(new Array({})));
        return result;
    }
    struct State {
        std::vector<Value> results;
        size_t remaining;
    };
    auto state = std::make_shared<State>();
    state->results.resize(promises.size());
    state->remaining = promises.size();
    for (size_t i = 0; i < promises.size(); i++) {
        auto record = [result, state, i](bool fulfilled, const Value& value) {
            state->results[i] = settledRecord(fulfilled, value);
            if (--state->remaining == 0) result->resolve(Value::object(new Array(state->results)));
        };
        promises[i]->addNativeReactions(
            [record](const Value& value) { record(true, value); },
            [record](const Value& reason) { record(false, reason); });
    }
    return result;
}

Promise* Promise::any(const std::vector<Promise*>& promises) {
    auto* result = new Promise();
    if (promises.empty()) {
        result->reject(aggregateError({}));
        return result;
    }
    struct State {
        std::vector<Value> errors;
        size_t remaining;
    };
    auto state = std::make_shared<State>();
    state->errors.resize(promises.size());
    state->remaining = promises.size();
    for (size_t i = 0; i < promises.size(); i++) {
        promises[i]->addNativeReactions(
            [result](const Value& value) { result->resolve(value); },
            [result, state, i](const Value& reason) {
                state->errors[i] = reason;
                if (--state->remaining == 0) result->reject(aggregateError(state->errors));
            });
    }
    return result;
}

Object* Promise::withResolvers() {
    auto* promise = new Promise();
    auto functions = promise->createResolvingFunctions();
    auto* result = new Object();
    if (VM* vm = VM::current()) result->setPrototype(vm->intrinsic(VM::Intrinsic::Object));
    result->set("promise", Value::object(promise));
    result->set("resolve", Value::object(functions.first));
    result->set("reject", Value::object(functions.second));
    return result;
}

} // namespace Zepra::Runtime
