// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file symbol.cpp
 * @brief JavaScript Symbol: constructor, registry functions and prototype
 */

#include "builtins/symbol.hpp"
#include "builtins/builtin_helpers.hpp"
#include "runtime/objects/symbol.hpp"
#include "runtime/objects/well_known_symbols.hpp"

namespace Zepra::Builtins {

using Runtime::FunctionCallInfo;
using Runtime::JSError;
using Runtime::SymbolRegistry;

namespace {

uint32_t thisSymbol(const FunctionCallInfo& info, const char* method) {
    const Value self = info.thisValue();
    if (!self.isSymbol()) {
        throw JSError("TypeError", std::string("Symbol.prototype.") + method + " requires that 'this' be a Symbol");
    }
    return self.asSymbol();
}

} // namespace

Runtime::Object* SymbolBuiltin::createSymbolPrototype() {
    auto* prototype = new Runtime::Object();

    defineMethod(prototype, "toString", [](const FunctionCallInfo& info) -> Value {
        return makeString(Runtime::symbolDescriptiveString(thisSymbol(info, "toString")));
    }, 0);

    defineMethod(prototype, "valueOf", [](const FunctionCallInfo& info) -> Value {
        return Value::symbol(thisSymbol(info, "valueOf"));
    }, 0);

    defineMethod(prototype, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::ToPrimitive),
                 [](const FunctionCallInfo& info) -> Value { return Value::symbol(thisSymbol(info, "[Symbol.toPrimitive]")); }, 1);

    // description is an accessor: undefined for a symbol made without one
    Runtime::PropertyDescriptor description;
    description.getter = Value::object(new Function("get description", [](const FunctionCallInfo& info) -> Value {
        const auto text = Runtime::symbolDescription(thisSymbol(info, "description"));
        return text ? makeString(*text) : Value::undefined();
    }, 0));
    description.attributes = Runtime::PropertyAttribute::Configurable;
    prototype->defineProperty("description", description);

    defineConstant(prototype, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::ToStringTag), makeString("Symbol"));
    return prototype;
}

Runtime::Function* SymbolBuiltin::createSymbolConstructor(Runtime::Object* prototype) {
    auto* constructor = new Function("Symbol", [prototype](const FunctionCallInfo& info) -> Value {
        const Value self = info.thisValue();
        if (self.isObject() && inheritsFrom(self.asObject(), prototype)) {
            throw JSError("TypeError", "Symbol is not a constructor");
        }
        const std::string text = info.argument(0).isUndefined() ? std::string() : info.argument(0).toString();
        return SymbolRegistry::instance().createSymbol(text);
    }, 0);

    defineHidden(constructor, "prototype", Value::object(prototype));
    defineHidden(prototype, "constructor", Value::object(constructor));

    defineMethod(constructor, "for", [](const FunctionCallInfo& info) -> Value {
        return SymbolRegistry::instance().symbolFor(info.argument(0).toString());
    }, 1);

    defineMethod(constructor, "keyFor", [](const FunctionCallInfo& info) -> Value {
        if (!info.argument(0).isSymbol()) {
            throw JSError("TypeError", info.argument(0).toString() + " is not a symbol");
        }
        const std::string key = SymbolRegistry::instance().symbolKeyFor(info.argument(0));
        return key.empty() ? Value::undefined() : makeString(key);
    }, 1);

    namespace Known = Runtime::WellKnownSymbols;
    defineConstant(constructor, "iterator", Value::symbol(Known::Iterator));
    defineConstant(constructor, "toPrimitive", Value::symbol(Known::ToPrimitive));
    defineConstant(constructor, "hasInstance", Value::symbol(Known::HasInstance));
    defineConstant(constructor, "toStringTag", Value::symbol(Known::ToStringTag));
    defineConstant(constructor, "species", Value::symbol(Known::Species));
    defineConstant(constructor, "isConcatSpreadable", Value::symbol(Known::IsConcatSpreadable));
    defineConstant(constructor, "unscopables", Value::symbol(Known::Unscopables));
    defineConstant(constructor, "asyncIterator", Value::symbol(Known::AsyncIterator));
    defineConstant(constructor, "match", Value::symbol(Known::Match));
    defineConstant(constructor, "replace", Value::symbol(Known::Replace));
    defineConstant(constructor, "search", Value::symbol(Known::Search));
    defineConstant(constructor, "split", Value::symbol(Known::Split));
    return constructor;
}

} // namespace Zepra::Builtins
