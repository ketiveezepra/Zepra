// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file reflect.cpp
 * @brief ES6 Reflect API Implementation
 *
 * Per ES2024 §26.1, Reflect methods delegate to the corresponding
 * internal object operations ([[Get]], [[Set]], [[Call]], etc.).
 */

#include "builtins/reflect.hpp"
#include "runtime/execution/js_error.hpp"
#include "runtime/execution/vm.hpp"
#include <algorithm>
#include "runtime/objects/object.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/value.hpp"
#include <vector>
#include <stdexcept>

namespace Zepra::Builtins {

// ES2024 §26.1.1 Reflect.apply(target, thisArgument, argumentsList)
Runtime::Value ReflectBuiltin::apply(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 1 || !info.argument(0).isObject() ||
        !info.argument(0).asObject()->isCallable()) {
        throw std::runtime_error("Reflect.apply: target must be callable");
    }

    Runtime::Function* fn = dynamic_cast<Runtime::Function*>(info.argument(0).asObject());
    if (!fn) {
        throw std::runtime_error("Reflect.apply: target is not a function");
    }

    Runtime::Value thisArg = info.argument(1);

    std::vector<Runtime::Value> args;
    if (info.argumentCount() >= 3 && info.argument(2).isObject()) {
        if (auto* arr = dynamic_cast<Runtime::Array*>(info.argument(2).asObject())) {
            for (size_t i = 0; i < arr->length(); i++) {
                args.push_back(arr->at(i));
            }
        }
    }

    return fn->call(info.context(), thisArg, args);
}

// ES2024 §26.1.2 Reflect.construct(target, argumentsList[, newTarget])
Runtime::Value ReflectBuiltin::construct(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 1 || !info.argument(0).isObject() ||
        !info.argument(0).asObject()->isCallable()) {
        throw std::runtime_error("Reflect.construct: target must be a constructor");
    }

    Runtime::Function* fn = dynamic_cast<Runtime::Function*>(info.argument(0).asObject());
    if (!fn) {
        throw std::runtime_error("Reflect.construct: target is not a function");
    }

    std::vector<Runtime::Value> args;
    if (info.argumentCount() >= 2 && info.argument(1).isObject()) {
        if (auto* arr = dynamic_cast<Runtime::Array*>(info.argument(1).asObject())) {
            for (size_t i = 0; i < arr->length(); i++) {
                args.push_back(arr->at(i));
            }
        }
    }

    Runtime::Object* newTarget = nullptr;
    if (info.argumentCount() >= 3) {
        const Runtime::Value nt = info.argument(2);
        if (!nt.isObject() || !nt.asObject()->isFunction()) {
            throw Runtime::JSError("TypeError", "Reflect.construct: newTarget is not a constructor");
        }
        newTarget = nt.asObject();
    }
    if (Runtime::VM* vm = Runtime::VM::current()) return vm->construct(fn, args, newTarget);
    return fn->construct(info.context(), args);
}

// ES2024 §26.1.3 Reflect.defineProperty(target, propertyKey, attributes)
Runtime::Value ReflectBuiltin::defineProperty(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 3 || !info.argument(0).isObject()) {
        throw std::runtime_error("Reflect.defineProperty: target must be an object");
    }

    Runtime::Object* obj = info.argument(0).asObject();
    std::string key = info.argument(1).toString();

    if (!info.argument(2).isObject()) {
        throw std::runtime_error("Reflect.defineProperty: descriptor must be an object");
    }

    Runtime::Object* attrsObj = info.argument(2).asObject();

    Runtime::PropertyDescriptor desc;
    Runtime::Value valueAttr = attrsObj->get("value");
    if (!valueAttr.isUndefined()) {
        desc.value = valueAttr;
    }

    Runtime::PropertyAttribute attrs = Runtime::PropertyAttribute::None;
    Runtime::Value writableAttr = attrsObj->get("writable");
    if (!writableAttr.isUndefined() && writableAttr.toBoolean()) {
        attrs = attrs | Runtime::PropertyAttribute::Writable;
    }
    Runtime::Value enumerableAttr = attrsObj->get("enumerable");
    if (!enumerableAttr.isUndefined() && enumerableAttr.toBoolean()) {
        attrs = attrs | Runtime::PropertyAttribute::Enumerable;
    }
    Runtime::Value configurableAttr = attrsObj->get("configurable");
    if (!configurableAttr.isUndefined() && configurableAttr.toBoolean()) {
        attrs = attrs | Runtime::PropertyAttribute::Configurable;
    }
    desc.attributes = attrs;

    bool success = obj->defineProperty(key, desc);
    return Runtime::Value::boolean(success);
}

// ES2024 §26.1.4 Reflect.deleteProperty(target, propertyKey)
Runtime::Value ReflectBuiltin::deleteProperty(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 2 || !info.argument(0).isObject()) {
        throw std::runtime_error("Reflect.deleteProperty: target must be an object");
    }

    return Runtime::Value::boolean(
        info.argument(0).asObject()->deleteProperty(info.argument(1).toString()));
}

// ES2024 §26.1.5 Reflect.get(target, propertyKey[, receiver])
Runtime::Value ReflectBuiltin::get(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 2 || !info.argument(0).isObject()) {
        throw std::runtime_error("Reflect.get: target must be an object");
    }

    return info.argument(0).asObject()->get(info.argument(1).toString());
}

// ES2024 §26.1.6 Reflect.getOwnPropertyDescriptor(target, propertyKey)
Runtime::Value ReflectBuiltin::getOwnPropertyDescriptor(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 2 || !info.argument(0).isObject()) {
        throw std::runtime_error("Reflect.getOwnPropertyDescriptor: target must be an object");
    }

    Runtime::Object* obj = info.argument(0).asObject();
    std::string key = info.argument(1).toString();

    auto desc = obj->getOwnPropertyDescriptor(key);
    if (!desc.has_value()) {
        return Runtime::Value::undefined();
    }

    Runtime::Object* result = new Runtime::Object();
    result->set("value", desc->value);
    result->set("writable", Runtime::Value::boolean(desc->isWritable()));
    result->set("enumerable", Runtime::Value::boolean(desc->isEnumerable()));
    result->set("configurable", Runtime::Value::boolean(desc->isConfigurable()));
    return Runtime::Value::object(result);
}

// ES2024 §26.1.8 Reflect.getPrototypeOf(target)
Runtime::Value ReflectBuiltin::getPrototypeOf(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 1 || !info.argument(0).isObject()) {
        throw std::runtime_error("Reflect.getPrototypeOf: target must be an object");
    }

    Runtime::Object* proto = info.argument(0).asObject()->prototype();
    return proto ? Runtime::Value::object(proto) : Runtime::Value::null();
}

// ES2024 §26.1.9 Reflect.has(target, propertyKey)
Runtime::Value ReflectBuiltin::has(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 2 || !info.argument(0).isObject()) {
        throw std::runtime_error("Reflect.has: target must be an object");
    }

    return Runtime::Value::boolean(
        info.argument(0).asObject()->has(info.argument(1).toString()));
}

// ES2024 §26.1.10 Reflect.isExtensible(target)
Runtime::Value ReflectBuiltin::isExtensible(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 1 || !info.argument(0).isObject()) {
        throw std::runtime_error("Reflect.isExtensible: target must be an object");
    }

    return Runtime::Value::boolean(info.argument(0).asObject()->isExtensible());
}

// ES2024 §26.1.11 Reflect.ownKeys(target)
Runtime::Value ReflectBuiltin::ownKeys(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 1 || !info.argument(0).isObject()) {
        throw std::runtime_error("Reflect.ownKeys: target must be an object");
    }

    Runtime::Object* obj = info.argument(0).asObject();
    std::vector<Runtime::Value> keys;

    for (const auto& key : obj->getOwnPropertyNames()) {
        keys.push_back(Runtime::Value::string(new Runtime::String(key)));
    }

    return Runtime::Value::object(new Runtime::Array(std::move(keys)));
}

// ES2024 §26.1.12 Reflect.preventExtensions(target)
Runtime::Value ReflectBuiltin::preventExtensions(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 1 || !info.argument(0).isObject()) {
        throw std::runtime_error("Reflect.preventExtensions: target must be an object");
    }

    info.argument(0).asObject()->preventExtensions();
    return Runtime::Value::boolean(true);
}

// ES2024 §26.1.13 Reflect.set(target, propertyKey, V[, receiver])
Runtime::Value ReflectBuiltin::set(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 3 || !info.argument(0).isObject()) {
        throw std::runtime_error("Reflect.set: target must be an object");
    }

    bool result = info.argument(0).asObject()->set(info.argument(1).toString(), info.argument(2));
    return Runtime::Value::boolean(result);
}

// ES2024 §26.1.14 Reflect.setPrototypeOf(target, proto)
Runtime::Value ReflectBuiltin::setPrototypeOf(const Runtime::FunctionCallInfo& info) {
    if (info.argumentCount() < 2 || !info.argument(0).isObject()) {
        throw std::runtime_error("Reflect.setPrototypeOf: target must be an object");
    }

    Runtime::Value proto = info.argument(1);
    if (!proto.isObject() && !proto.isNull()) {
        throw std::runtime_error("Reflect.setPrototypeOf: proto must be an object or null");
    }

    Runtime::Object* protoObj = proto.isObject() ? proto.asObject() : nullptr;
    info.argument(0).asObject()->setPrototype(protoObj);
    return Runtime::Value::boolean(true);
}

// Create Reflect global object
Runtime::Object* ReflectBuiltin::createReflectObject(Runtime::Context*) {
    Runtime::Object* reflect = new Runtime::Object();

    reflect->set("apply", Runtime::Value::object(new Runtime::Function("apply", apply, 3)));
    reflect->set("construct", Runtime::Value::object(new Runtime::Function("construct", construct, 2)));
    reflect->set("defineProperty", Runtime::Value::object(new Runtime::Function("defineProperty", defineProperty, 3)));
    reflect->set("deleteProperty", Runtime::Value::object(new Runtime::Function("deleteProperty", deleteProperty, 2)));
    reflect->set("get", Runtime::Value::object(new Runtime::Function("get", get, 2)));
    reflect->set("getOwnPropertyDescriptor", Runtime::Value::object(new Runtime::Function("getOwnPropertyDescriptor", getOwnPropertyDescriptor, 2)));
    reflect->set("getPrototypeOf", Runtime::Value::object(new Runtime::Function("getPrototypeOf", getPrototypeOf, 1)));
    reflect->set("has", Runtime::Value::object(new Runtime::Function("has", has, 2)));
    reflect->set("isExtensible", Runtime::Value::object(new Runtime::Function("isExtensible", isExtensible, 1)));
    reflect->set("ownKeys", Runtime::Value::object(new Runtime::Function("ownKeys", ownKeys, 1)));
    reflect->set("preventExtensions", Runtime::Value::object(new Runtime::Function("preventExtensions", preventExtensions, 1)));
    reflect->set("set", Runtime::Value::object(new Runtime::Function("set", set, 3)));
    reflect->set("setPrototypeOf", Runtime::Value::object(new Runtime::Function("setPrototypeOf", setPrototypeOf, 2)));

    return reflect;
}

} // namespace Zepra::Builtins
