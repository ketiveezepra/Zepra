// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file weakmap.cpp
 * @brief JavaScript WeakMap builtin implementation
 */

#include "builtins/weakmap.hpp"
#include "builtins/builtin_helpers.hpp"
#include "runtime/execution/vm.hpp"
#include "runtime/objects/function.hpp"

namespace Zepra::Builtins {

// =============================================================================
// WeakMapObject Implementation
// =============================================================================

void WeakMapObject::set(Object* key, const Value& value) {
    if (key) {
        entries_[key] = value;
    }
}

Value WeakMapObject::get(Object* key) const {
    if (!key) return Value::undefined();
    
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        return it->second;
    }
    return Value::undefined();
}

bool WeakMapObject::has(Object* key) const {
    if (!key) return false;
    return entries_.find(key) != entries_.end();
}

bool WeakMapObject::deleteKey(Object* key) {
    if (!key) return false;
    return entries_.erase(key) > 0;
}

// =============================================================================
// WeakMapBuiltin Implementation
// =============================================================================

Value WeakMapBuiltin::constructor(Runtime::Context*, const std::vector<Value>&) {
    return Value::object(new WeakMapObject());
}

Value WeakMapBuiltin::get(Runtime::Context*, const std::vector<Value>& args) {
    if (args.size() < 2 || !args[0].isObject()) return Value::undefined();
    
    WeakMapObject* wm = dynamic_cast<WeakMapObject*>(args[0].asObject());
    if (!wm) return Value::undefined();
    
    if (!args[1].isObject()) return Value::undefined();
    return wm->get(args[1].asObject());
}

Value WeakMapBuiltin::set(Runtime::Context*, const std::vector<Value>& args) {
    if (args.size() < 3 || !args[0].isObject()) return Value::undefined();
    
    WeakMapObject* wm = dynamic_cast<WeakMapObject*>(args[0].asObject());
    if (!wm) return Value::undefined();
    
    if (!args[1].isObject()) return Value::undefined();
    wm->set(args[1].asObject(), args[2]);
    return args[0]; // Return WeakMap for chaining
}

Value WeakMapBuiltin::has(Runtime::Context*, const std::vector<Value>& args) {
    if (args.size() < 2 || !args[0].isObject()) return Value::boolean(false);
    
    WeakMapObject* wm = dynamic_cast<WeakMapObject*>(args[0].asObject());
    if (!wm) return Value::boolean(false);
    
    if (!args[1].isObject()) return Value::boolean(false);
    return Value::boolean(wm->has(args[1].asObject()));
}

Value WeakMapBuiltin::deleteMethod(Runtime::Context*, const std::vector<Value>& args) {
    if (args.size() < 2 || !args[0].isObject()) return Value::boolean(false);
    
    WeakMapObject* wm = dynamic_cast<WeakMapObject*>(args[0].asObject());
    if (!wm) return Value::boolean(false);
    
    if (!args[1].isObject()) return Value::boolean(false);
    return Value::boolean(wm->deleteKey(args[1].asObject()));
}

Object* WeakMapBuiltin::createWeakMapPrototype() {
    Object* proto = new Object();

    proto->set("get", Value::object(
        new Runtime::Function("get", [](const Runtime::FunctionCallInfo& info) -> Value {
            if (!info.thisValue().isObject()) return Value::undefined();
            WeakMapObject* wm = dynamic_cast<WeakMapObject*>(info.thisValue().asObject());
            if (!wm || info.argumentCount() < 1 || !info.argument(0).isObject()) return Value::undefined();
            return wm->get(info.argument(0).asObject());
        }, 1)));

    proto->set("set", Value::object(
        new Runtime::Function("set", [](const Runtime::FunctionCallInfo& info) -> Value {
            if (!info.thisValue().isObject()) return Value::undefined();
            WeakMapObject* wm = dynamic_cast<WeakMapObject*>(info.thisValue().asObject());
            if (!wm) return info.thisValue();
            if (!info.argument(0).isObject()) {
                throw Runtime::JSError("TypeError", "Invalid value used as weak map key");
            }
            wm->set(info.argument(0).asObject(), info.argument(1));
            return info.thisValue();
        }, 2)));

    proto->set("has", Value::object(
        new Runtime::Function("has", [](const Runtime::FunctionCallInfo& info) -> Value {
            if (!info.thisValue().isObject()) return Value::boolean(false);
            WeakMapObject* wm = dynamic_cast<WeakMapObject*>(info.thisValue().asObject());
            if (!wm || info.argumentCount() < 1 || !info.argument(0).isObject()) return Value::boolean(false);
            return Value::boolean(wm->has(info.argument(0).asObject()));
        }, 1)));

    proto->set("delete", Value::object(
        new Runtime::Function("delete", [](const Runtime::FunctionCallInfo& info) -> Value {
            if (!info.thisValue().isObject()) return Value::boolean(false);
            WeakMapObject* wm = dynamic_cast<WeakMapObject*>(info.thisValue().asObject());
            if (!wm || info.argumentCount() < 1 || !info.argument(0).isObject()) return Value::boolean(false);
            return Value::boolean(wm->deleteKey(info.argument(0).asObject()));
        }, 1)));

    return proto;
}

Runtime::Function* WeakMapBuiltin::createWeakMapConstructor(Object* prototype) {
    auto* constructor = new Runtime::Function("WeakMap", [prototype](const Runtime::FunctionCallInfo& info) -> Value {
        const Value self = info.thisValue();
        if (!self.isObject() || !inheritsFrom(self.asObject(), prototype)) {
            throw Runtime::JSError("TypeError", "Constructor WeakMap requires 'new'");
        }
        auto* map = new WeakMapObject();
        map->setPrototype(self.asObject()->prototype());

        const Value iterable = info.argument(0);
        if (!iterable.isUndefined() && !iterable.isNull()) {
            Runtime::VM* vm = Runtime::VM::current();
            if (!vm) throw Runtime::JSError("TypeError", "WeakMap constructed outside a running script");
            vm->forOf(iterable, [&](const Value& entry) {
                if (!entry.isObject()) {
                    throw Runtime::JSError("TypeError", "Iterator value " + entry.toString() + " is not an entry object");
                }
                const Value key = entry.asObject()->get(static_cast<uint32_t>(0));
                if (!key.isObject()) throw Runtime::JSError("TypeError", "Invalid value used as weak map key");
                map->set(key.asObject(), entry.asObject()->get(static_cast<uint32_t>(1)));
                return true;
            });
        }
        return Value::object(map);
    }, 0);
    defineHidden(constructor, "prototype", Value::object(prototype));
    defineHidden(prototype, "constructor", Value::object(constructor));
    return constructor;
}

} // namespace Zepra::Builtins
