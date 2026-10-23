// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file structured_clone.cpp
 * @brief structuredClone(value): the HTML structured clone algorithm
 */

#include "builtins/structured_clone.hpp"
#include "builtins/boolean.hpp"
#include "builtins/builtin_helpers.hpp"
#include "builtins/date.hpp"
#include "builtins/map.hpp"
#include "builtins/number.hpp"
#include "builtins/regexp.hpp"
#include "builtins/set.hpp"
#include "runtime/execution/vm.hpp"

#include <unordered_map>

namespace Zepra::Builtins {

using Runtime::Array;
using Runtime::Error;
using Runtime::FunctionCallInfo;
using Runtime::JSError;
using Runtime::String;
using Runtime::VM;

namespace {

constexpr int kMaxDepth = 512;

[[noreturn]] void notCloneable(const std::string& what) {
    throw JSError("DataCloneError", what + " could not be cloned.");
}

class Cloner {
public:
    explicit Cloner(VM* vm) : vm_(vm) {}

    Value clone(const Value& value, int depth) {
        if (depth > kMaxDepth) throw JSError("RangeError", "Maximum call stack size exceeded");
        if (value.isSymbol()) notCloneable("Symbol");
        if (!value.isObject()) return value;  // primitives, strings and BigInts are immutable

        Object* source = value.asObject();
        auto seen = memory_.find(source);
        if (seen != memory_.end()) return Value::object(seen->second);

        if (source->isFunction()) notCloneable("function " + static_cast<Runtime::Function*>(source)->name());
        if (source->objectType() == Runtime::ObjectType::Proxy) notCloneable("#<Proxy>");

        if (auto* wrapper = dynamic_cast<NumberObject*>(source)) {
            return remember(source, copyPrototype(new NumberObject(wrapper->valueOf()), source));
        }
        if (auto* wrapper = dynamic_cast<BooleanObject*>(source)) {
            return remember(source, copyPrototype(new BooleanObject(wrapper->valueOf()), source));
        }
        if (auto* wrapper = dynamic_cast<String*>(source)) {
            return remember(source, copyPrototype(new String(wrapper->value()), source));
        }
        if (auto* date = dynamic_cast<DateObject*>(source)) {
            return remember(source, copyPrototype(new DateObject(date->getTime()), source));
        }
        if (auto* regexp = dynamic_cast<RegExpObject*>(source)) {
            return remember(source, copyPrototype(new RegExpObject(regexp->pattern(), regexp->flags()), source));
        }
        if (source->objectType() == Runtime::ObjectType::Error) return cloneError(source);
        if (source->isArray()) return cloneArray(static_cast<Array*>(source), depth);
        if (auto* map = dynamic_cast<MapObject*>(source)) return cloneMap(map, depth);
        if (auto* set = dynamic_cast<SetObject*>(source)) return cloneSet(set, depth);
        if (source->objectType() == Runtime::ObjectType::Ordinary) return clonePlain(source, depth);
        notCloneable("#<Object>");
    }

private:
    Value remember(Object* source, Object* copy) {
        memory_[source] = copy;
        return Value::object(copy);
    }

    // Clones keep the realm's prototype of the original (Map.prototype, ...)
    static Object* copyPrototype(Object* copy, const Object* source) {
        copy->setPrototype(source->prototype());
        return copy;
    }

    Value read(const Value& object, const std::string& key) {
        return vm_ ? vm_->getMember(object, key) : object.asObject()->get(key);
    }

    Value cloneError(Object* source) {
        const Value self = Value::object(source);
        std::string name = read(self, "name").toString();
        static const char* standard[] = {"Error", "EvalError", "RangeError", "ReferenceError",
                                         "SyntaxError", "TypeError", "URIError"};
        bool known = false;
        for (const char* candidate : standard) known = known || name == candidate;
        if (!known) name = "Error";

        const Value message = read(self, "message");
        Object* copy;
        if (vm_) {
            copy = vm_->newError(name, message.isUndefined() ? "" : message.toString()).asObject();
        } else {
            copy = new Error(message.isUndefined() ? "" : message.toString(), name);
        }
        memory_[source] = copy;
        const Value stack = read(self, "stack");
        if (stack.isString()) defineHidden(copy, "stack", stack);
        return Value::object(copy);
    }

    Value cloneArray(Array* source, int depth) {
        auto* copy = new Array();
        memory_[source] = copy;
        for (size_t i = 0; i < source->length(); ++i) {
            copy->push(clone(source->get(static_cast<uint32_t>(i)), depth + 1));
        }
        // Named properties on the array travel too
        for (const std::string& key : source->keys()) {
            if (isIndex(key)) continue;
            static_cast<Object*>(copy)->set(key, clone(read(Value::object(source), key), depth + 1));
        }
        return Value::object(copy);
    }

    Value cloneMap(MapObject* source, int depth) {
        auto* copy = copyPrototype(new MapObject(), source);
        memory_[source] = copy;
        for (const auto& entry : source->entries()) {
            static_cast<MapObject*>(copy)->set(clone(entry.first, depth + 1), clone(entry.second, depth + 1));
        }
        return Value::object(copy);
    }

    Value cloneSet(SetObject* source, int depth) {
        auto* copy = copyPrototype(new SetObject(), source);
        memory_[source] = copy;
        for (const Value& element : source->values()) {
            static_cast<SetObject*>(copy)->add(clone(element, depth + 1));
        }
        return Value::object(copy);
    }

    Value clonePlain(Object* source, int depth) {
        auto* copy = new Object();
        if (vm_) copy->setPrototype(vm_->intrinsic(VM::Intrinsic::Object));
        memory_[source] = copy;
        const Value self = Value::object(source);
        for (const std::string& key : source->keys()) {
            copy->set(key, clone(read(self, key), depth + 1));
        }
        return Value::object(copy);
    }

    static bool isIndex(const std::string& key) {
        if (key.empty() || key.size() > 10) return false;
        for (char c : key) {
            if (c < '0' || c > '9') return false;
        }
        return true;
    }

    VM* vm_;
    std::unordered_map<Object*, Object*> memory_;
};

} // namespace

Runtime::Function* StructuredClone::createFunction() {
    return new Runtime::Function("structuredClone", [](const FunctionCallInfo& info) -> Value {
        if (info.argumentCount() == 0) {
            throw JSError("TypeError", "structuredClone requires at least 1 argument");
        }
        Cloner cloner(VM::current());
        return cloner.clone(info.argument(0), 0);
    }, 1);
}

} // namespace Zepra::Builtins
