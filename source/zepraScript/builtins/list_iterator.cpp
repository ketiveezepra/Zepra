// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file list_iterator.cpp
 * @brief Iterator objects over a fixed list of values
 */

#include "builtins/list_iterator.hpp"
#include "builtins/builtin_helpers.hpp"
#include "runtime/execution/vm.hpp"
#include "runtime/objects/well_known_symbols.hpp"

namespace Zepra::Builtins {

using Runtime::FunctionCallInfo;
using Runtime::JSError;
using Runtime::VM;

namespace {

Value iterationResult(const Value& value, bool done) {
    auto* result = new Object();
    if (VM* vm = VM::current()) result->setPrototype(vm->intrinsic(VM::Intrinsic::Object));
    result->set("value", value);
    result->set("done", Value::boolean(done));
    return Value::object(result);
}

} // namespace

Object* IteratorBuiltin::createIteratorPrototype() {
    auto* prototype = new Object();

    defineMethod(prototype, "next", [](const FunctionCallInfo& info) -> Value {
        const Value self = info.thisValue();
        auto* iterator = self.isObject() ? dynamic_cast<ListIterator*>(self.asObject()) : nullptr;
        if (!iterator) throw JSError("TypeError", "next method called on an incompatible receiver");
        Value value;
        if (iterator->next(value)) return iterationResult(value, false);
        return iterationResult(Value::undefined(), true);
    }, 0);

    // An iterator is its own iterable
    defineMethod(prototype, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::Iterator),
                 [](const FunctionCallInfo& info) -> Value { return info.thisValue(); }, 0);
    return prototype;
}

Value IteratorBuiltin::makeListIterator(std::vector<Value> items) {
    auto* iterator = new ListIterator(std::move(items));
    if (VM* vm = VM::current()) iterator->setPrototype(vm->intrinsic(VM::Intrinsic::Iterator));
    return Value::object(iterator);
}

} // namespace Zepra::Builtins
