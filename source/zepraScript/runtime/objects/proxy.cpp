// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file proxy.cpp
 * @brief JavaScript Proxy and Reflect implementation
 */

#include "runtime/objects/proxy.hpp"
#include <stdexcept>
#include <memory>
#include "runtime/objects/object.hpp"
#include "runtime/execution/vm.hpp"
#include <algorithm>
#include "runtime/objects/function.hpp"

namespace Zepra::Runtime {

// =============================================================================
// Proxy Implementation
// =============================================================================

Proxy::Proxy(Object* target, Object* handler)
    : Object(ObjectType::Proxy)
    , target_(target)
    , handler_(handler) {}

Function* Proxy::trap(const char* name) const {
    if (revoked_ || !handler_) {
        throw std::runtime_error(std::string("Cannot perform '") + name + "' on a proxy that has been revoked");
    }
    VM* vm = VM::current();
    Value fn = vm ? vm->getMember(Value::object(handler_), name) : handler_->get(name);
    if (fn.isUndefined() || fn.isNull()) return nullptr;
    if (!fn.isObject() || !fn.asObject()->isFunction()) {
        throw std::runtime_error(std::string("proxy trap '") + name + "' is not a function");
    }
    return static_cast<Function*>(fn.asObject());
}

Value Proxy::callTrap(Function* fn, std::vector<Value> args) const {
    VM* vm = VM::current();
    if (!vm) return Value::undefined();
    return vm->executeCallback(fn, Value::object(handler_), args);
}

static Value keyValue(const std::string& key) {
    if (key.compare(0, 9, "@@symbol:") == 0) {
        return Value::symbol(static_cast<uint32_t>(std::stoul(key.substr(9))));
    }
    return Value::string(new String(key));
}

Value Proxy::get(const std::string& key) const {
    if (Function* fn = trap("get")) {
        return callTrap(fn, {Value::object(target_), keyValue(key), Value::object(const_cast<Proxy*>(this))});
    }
    VM* vm = VM::current();
    return vm ? vm->getMember(Value::object(target_), key) : target_->get(key);
}

bool Proxy::set(const std::string& key, Value value) {
    if (Function* fn = trap("set")) {
        return callTrap(fn, {Value::object(target_), keyValue(key), value, Value::object(this)}).toBoolean();
    }
    if (VM* vm = VM::current()) vm->setMember(Value::object(target_), key, value);
    else target_->set(key, value);
    return true;
}

bool Proxy::has(const std::string& key) const {
    if (Function* fn = trap("has")) {
        return callTrap(fn, {Value::object(target_), keyValue(key)}).toBoolean();
    }
    for (Object* o = target_; o; o = o->prototype()) {
        if (o->has(key)) return true;
    }
    return false;
}

bool Proxy::deleteProperty(const std::string& key) {
    if (Function* fn = trap("deleteProperty")) {
        return callTrap(fn, {Value::object(target_), keyValue(key)}).toBoolean();
    }
    return target_->deleteProperty(key);
}

std::vector<std::string> Proxy::proxyOwnKeys() const {
    if (Function* fn = trap("ownKeys")) {
        std::vector<std::string> names;
        Value list = callTrap(fn, {Value::object(target_)});
        if (auto* array = list.isObject() ? dynamic_cast<Array*>(list.asObject()) : nullptr) {
            for (size_t i = 0; i < array->length(); ++i) {
                Value k = array->at(i);
                if (k.isString()) names.push_back(k.toString());
            }
        }
        return names;
    }
    return target_->getOwnPropertyNames();
}

std::vector<std::string> Proxy::proxyEnumerableKeys() const {
    if (!trap("ownKeys")) return target_->keys();
    std::vector<std::string> names = proxyOwnKeys();
    Function* describe = trap("getOwnPropertyDescriptor");
    if (!describe) return names;
    std::vector<std::string> out;
    VM* vm = VM::current();
    for (const auto& name : names) {
        Value desc = callTrap(describe, {Value::object(target_), keyValue(name)});
        if (desc.isObject() && vm && vm->getMember(desc, "enumerable").toBoolean()) out.push_back(name);
    }
    return out;
}

Object* Proxy::create(Object* target, Object* handler) {
    if (!target->isFunction()) return new Proxy(target, handler);

    // A callable target: a function that runs the apply/construct traps.
    // Property reads fall through to the target (it is the prototype), and
    // `prototype` is the target's. (get/set traps do not apply to it.)
    auto* targetFn = static_cast<Function*>(target);
    struct State { Object* handler; Function* target; Object* self = nullptr; bool revoked = false; };
    auto state = std::make_shared<State>(State{handler, targetFn});
    auto* wrapper = new Function(targetFn->name(), [state](const FunctionCallInfo& info) -> Value {
        if (state->revoked) throw std::runtime_error("Cannot perform 'apply' on a proxy that has been revoked");
        VM* vm = VM::current();
        if (!vm) return Value::undefined();
        std::vector<Value> args = info.arguments();
        Value self = info.thisValue();
        // `new proxy(...)`: the VM made a fresh instance of proxy.prototype
        const bool construct = self.isObject() && state->self &&
            self.asObject()->prototype() == vm->getMember(Value::object(state->self), "prototype").asObject() &&
            self.asObject()->getOwnPropertyNames().empty();
        Value trapFn = vm->getMember(Value::object(state->handler), construct ? "construct" : "apply");
        auto* argArray = new Array(args);
        if (construct) {
            if (trapFn.isObject() && trapFn.asObject()->isFunction()) {
                Value result = vm->executeCallback(static_cast<Function*>(trapFn.asObject()), Value::object(state->handler),
                                                   {Value::object(state->target), Value::object(argArray), Value::object(state->self)});
                if (!result.isObject()) throw std::runtime_error("proxy [[Construct]] must return an object");
                return result;
            }
            return vm->construct(state->target, args);
        }
        if (trapFn.isObject() && trapFn.asObject()->isFunction()) {
            return vm->executeCallback(static_cast<Function*>(trapFn.asObject()), Value::object(state->handler),
                                       {Value::object(state->target), self, Value::object(argArray)});
        }
        return vm->executeCallback(state->target, self, args);
    }, targetFn->arity());
    state->self = wrapper;
    wrapper->setPrototype(target);
    if (VM* vm = VM::current()) {
        Value proto = vm->getMember(Value::object(target), "prototype");
        if (proto.isObject()) wrapper->set("prototype", proto);
    }
    return wrapper;
}

std::pair<Object*, std::function<void()>> Proxy::createRevocable(Object* target, Object* handler) {
    Object* proxy = create(target, handler);
    auto* plain = dynamic_cast<Proxy*>(proxy);
    return {proxy, [plain]() { if (plain) plain->revoke(); }};
}

// =============================================================================
// Reflect Implementation
// =============================================================================

Value Reflect::get(Object* target, const std::string& key, Object*) {
    return target ? target->get(key) : Value::undefined();
}

bool Reflect::set(Object* target, const std::string& key, Value value, Object*) {
    if (target) {
        target->set(key, value);
        return true;
    }
    return false;
}

bool Reflect::has(Object* target, const std::string& key) {
    return target && !target->get(key).isUndefined();
}

bool Reflect::deleteProperty(Object* target, const std::string& key) {
    if (!target) return false;
    // Delete the property from the object
    // Returns true if deletion was successful or property didn't exist
    return target->deleteProperty(key);
}

std::vector<std::string> Reflect::ownKeys(Object* target) {
    return target ? target->keys() : std::vector<std::string>{};
}

Value Reflect::apply(Object* target, Value thisArg, const std::vector<Value>& args) {
    Function* fn = dynamic_cast<Function*>(target);
    if (!fn) return Value::undefined();
    
    // Call the function with the provided thisArg and arguments
    return fn->call(nullptr, thisArg, args);
}

Value Reflect::construct(Object* target, const std::vector<Value>& args) {
    Function* fn = dynamic_cast<Function*>(target);
    if (!fn) return Value::undefined();
    
    // Construct the object using the function as constructor
    return fn->construct(nullptr, args);
}

Value Reflect::getOwnPropertyDescriptor(Object* target, const std::string& key) {
    if (!target) return Value::undefined();
    
    auto desc = target->getOwnPropertyDescriptor(key);
    if (!desc) return Value::undefined();
    
    Object* result = new Object();
    result->set("value", desc->value);
    result->set("writable", Value::boolean(desc->isWritable()));
    result->set("enumerable", Value::boolean(desc->isEnumerable()));
    result->set("configurable", Value::boolean(desc->isConfigurable()));
    
    if (!desc->getter.isUndefined()) {
        result->set("get", desc->getter);
    }
    if (!desc->setter.isUndefined()) {
        result->set("set", desc->setter);
    }
    
    return Value::object(result);
}

bool Reflect::defineProperty(Object* target, const std::string& key, Object* descriptor) {
    if (!target || !descriptor) return false;
    
    PropertyDescriptor desc;
    Value val = descriptor->get("value");
    if (!val.isUndefined()) desc.value = val;
    
    Value writable = descriptor->get("writable");
    Value enumerable = descriptor->get("enumerable");
    Value configurable = descriptor->get("configurable");
    
    PropertyAttribute attrs = PropertyAttribute::None;
    if (writable.isUndefined() || writable.toBoolean()) attrs = attrs | PropertyAttribute::Writable;
    if (enumerable.isUndefined() || enumerable.toBoolean()) attrs = attrs | PropertyAttribute::Enumerable;
    if (configurable.isUndefined() || configurable.toBoolean()) attrs = attrs | PropertyAttribute::Configurable;
    desc.attributes = attrs;
    
    desc.getter = descriptor->get("get");
    desc.setter = descriptor->get("set");
    
    return target->defineProperty(key, desc);
}

Object* Reflect::getPrototypeOf(Object* target) {
    return target ? target->prototype() : nullptr;
}

bool Reflect::setPrototypeOf(Object* target, Object* proto) {
    if (target) {
        target->setPrototype(proto);
        return true;
    }
    return false;
}

bool Reflect::isExtensible(Object*) {
    return true; // Default
}

bool Reflect::preventExtensions(Object* target) {
    if (!target) return false;
    target->preventExtensions();
    return true;
}

// =============================================================================
// ReflectBuiltin Implementation
// =============================================================================

Value ReflectBuiltin::get(Context*, const std::vector<Value>& args) {
    if (args.size() < 2 || !args[0].isObject() || !args[1].isString()) {
        return Value::undefined();
    }
    
    std::string key = static_cast<String*>(args[1].asObject())->value();
    return Reflect::get(args[0].asObject(), key);
}

Value ReflectBuiltin::set(Context*, const std::vector<Value>& args) {
    if (args.size() < 3 || !args[0].isObject() || !args[1].isString()) {
        return Value::boolean(false);
    }
    
    std::string key = static_cast<String*>(args[1].asObject())->value();
    return Value::boolean(Reflect::set(args[0].asObject(), key, args[2]));
}

Value ReflectBuiltin::has(Context*, const std::vector<Value>& args) {
    if (args.size() < 2 || !args[0].isObject() || !args[1].isString()) {
        return Value::boolean(false);
    }
    
    std::string key = static_cast<String*>(args[1].asObject())->value();
    return Value::boolean(Reflect::has(args[0].asObject(), key));
}

Value ReflectBuiltin::deleteProperty(Context*, const std::vector<Value>& args) {
    if (args.size() < 2 || !args[0].isObject() || !args[1].isString()) {
        return Value::boolean(false);
    }
    
    std::string key = static_cast<String*>(args[1].asObject())->value();
    return Value::boolean(Reflect::deleteProperty(args[0].asObject(), key));
}

Value ReflectBuiltin::ownKeys(Context*, const std::vector<Value>& args) {
    if (args.empty() || !args[0].isObject()) {
        return Value::undefined();
    }
    
    std::vector<std::string> keys = Reflect::ownKeys(args[0].asObject());
    Array* result = new Array();
    for (const auto& key : keys) {
        result->push(Value::string(new String(key)));
    }
    return Value::object(result);
}

Value ReflectBuiltin::apply(Context*, const std::vector<Value>& args) {
    if (args.size() < 3 || !args[0].isObject()) {
        return Value::undefined();
    }
    
    std::vector<Value> fnArgs;
    if (args[2].isObject()) {
        Array* argsArray = dynamic_cast<Array*>(args[2].asObject());
        if (argsArray) {
            size_t len = argsArray->length();
            for (size_t i = 0; i < len; i++) {
                fnArgs.push_back(argsArray->get(i));
            }
        }
    }
    return Reflect::apply(args[0].asObject(), args[1], fnArgs);
}

Value ReflectBuiltin::construct(Context*, const std::vector<Value>& args) {
    if (args.size() < 2 || !args[0].isObject()) {
        return Value::undefined();
    }
    
    std::vector<Value> ctorArgs;
    if (args[1].isObject()) {
        Array* argsArray = dynamic_cast<Array*>(args[1].asObject());
        if (argsArray) {
            size_t len = argsArray->length();
            for (size_t i = 0; i < len; i++) {
                ctorArgs.push_back(argsArray->get(i));
            }
        }
    }
    return Reflect::construct(args[0].asObject(), ctorArgs);
}

} // namespace Zepra::Runtime
