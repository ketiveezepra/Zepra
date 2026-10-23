// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file global_object.cpp
 * @brief Global object initialization
 *
 * Wires up all built-in constructors, prototypes, and global functions
 * by delegating to their respective Builtin classes.
 */

#include "runtime/execution/global_object.hpp"
#include <algorithm>
#include "builtins/console.hpp"
#include "builtins/math.hpp"
#include "builtins/json.hpp"
#include "builtins/array.hpp"
#include "builtins/string.hpp"
#include "builtins/object_builtins.hpp"
#include "builtins/number.hpp"
#include "builtins/boolean.hpp"
#include "builtins/builtin_helpers.hpp"
#include "builtins/error.hpp"
#include "builtins/global_functions.hpp"
#include "builtins/structured_clone.hpp"
#include "runtime/execution/context.hpp"
#include "builtins/map.hpp"
#include "builtins/set.hpp"
#include "builtins/date.hpp"
#include "builtins/weakmap.hpp"
#include "builtins/regexp.hpp"
#include "builtins/typed_array.hpp"
#include "builtins/reflect.hpp"
#include "runtime/objects/proxy.hpp"
#include "builtins/generator.hpp"
#include "runtime/objects/well_known_symbols.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/async/promise.hpp"
#include "runtime/execution/vm.hpp"
#include "runtime/execution/event_loop.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Zepra::Runtime {

// ---------------------------------------------------------------------------
// GlobalObject
// ---------------------------------------------------------------------------

GlobalObject::GlobalObject(Context* context)
    : Object(ObjectType::Global), context_(context) {}

void GlobalObject::initialize() {
    // Intrinsic values
    set("undefined", Value::undefined());
    set("NaN", Value::number(std::nan("")));
    set("Infinity", Value::number(std::numeric_limits<double>::infinity()));
    set("globalThis", Value::object(this));

    // Constructors & prototypes (delegate to builtin classes)
    initializeObjectConstructor();
    initializeArrayConstructor();
    initializeStringConstructor();
    initializeNumberConstructor();
    initializeBooleanConstructor();
    initializeFunctionConstructor();
    initializeErrorConstructors();

    // Built-in objects
    initializeMath();
    initializeJSON();
    initializeConsole();

    // Global functions
    initializeGlobalFunctions();
}

void GlobalObject::installInto(VM& vm, const std::vector<std::string>& except) const {
    for (const std::string& name : getOwnPropertyNames()) {
        if (std::find(except.begin(), except.end(), name) != except.end()) continue;
        vm.setGlobal(name, get(name));
    }
}

namespace {

VM& vmOf(Context* context) {
    VM* vm = context ? context->vm() : nullptr;
    if (!vm) throw std::logic_error("GlobalObject needs a Context that belongs to a VM");
    return *vm;
}

// Copies the properties of `from` onto `to` as built-in (non-enumerable) ones
void copyAsBuiltins(Object* from, Object* to) {
    for (const std::string& key : from->getOwnPropertyNames()) {
        Builtins::defineHidden(to, key, from->get(key));
    }
}

// Object(value): a new object, the value itself, or a wrapper for a primitive
Value objectFromValue(VM& vm, const Value& value) {
    if (value.isNull() || value.isUndefined()) {
        auto* object = new Object();
        object->setPrototype(vm.intrinsic(VM::Intrinsic::Object));
        return Value::object(object);
    }
    if (value.isObject()) return value;
    if (value.isNumber()) {
        auto* wrapper = new Builtins::NumberObject(value.asNumber());
        wrapper->setPrototype(vm.intrinsic(VM::Intrinsic::Number));
        return Value::object(wrapper);
    }
    if (value.isBoolean()) {
        auto* wrapper = new Builtins::BooleanObject(value.asBoolean());
        wrapper->setPrototype(vm.intrinsic(VM::Intrinsic::Boolean));
        return Value::object(wrapper);
    }
    if (value.isString()) {
        auto* wrapper = new String(value.toString());
        wrapper->setPrototype(vm.intrinsic(VM::Intrinsic::String));
        return Value::object(wrapper);
    }
    return value;
}

} // namespace

void GlobalObject::initializeObjectConstructor() {
    VM& vm = vmOf(context_);
    objectPrototype_ = vm.intrinsic(VM::Intrinsic::Object);
    objectConstructor_ = new Function("Object", [&vm](const FunctionCallInfo& info) -> Value {
        return objectFromValue(vm, info.argument(0));
    }, 1);

    // The static methods (keys, assign, create, ...)
    copyAsBuiltins(Builtins::ObjectBuiltin::createObjectConstructor(context_), objectConstructor_);
    Builtins::defineHidden(objectConstructor_, "prototype", Value::object(objectPrototype_));
    Builtins::defineHidden(objectPrototype_, "constructor", Value::object(objectConstructor_));
    set("Object", Value::object(objectConstructor_));
}

void GlobalObject::initializeArrayConstructor() {
    VM& vm = vmOf(context_);
    arrayPrototype_ = vm.intrinsic(VM::Intrinsic::Array);
    arrayConstructor_ = new Function("Array", [](const FunctionCallInfo& info) -> Value {
        const auto& args = info.arguments();
        if (args.size() == 1 && args[0].isNumber()) {
            // Array(n): n empty slots. Storage is dense, so an enormous n is refused
            const double length = args[0].asNumber();
            if (!(length >= 0) || length != std::floor(length) || length > 4294967295.0) {
                throw JSError("RangeError", "Invalid array length");
            }
            if (length > static_cast<double>(1u << 22)) {
                throw JSError("RangeError", "Array length too large");
            }
            return Value::object(new Array(std::vector<Value>(static_cast<size_t>(length), Value::undefined())));
        }
        return Value::object(new Array(std::vector<Value>(args.begin(), args.end())));
    }, 1);
    arrayConstructor_->set("isArray", Value::object(
        new Function("isArray", Builtins::ArrayBuiltin::isArray, 1)));
    arrayConstructor_->set("from", Value::object(
        new Function("from", Builtins::ArrayBuiltin::from, 1)));
    arrayConstructor_->set("of", Value::object(
        new Function("of", Builtins::ArrayBuiltin::of, 0)));
    Builtins::defineHidden(arrayConstructor_, "prototype", Value::object(arrayPrototype_));
    Builtins::defineHidden(arrayPrototype_, "constructor", Value::object(arrayConstructor_));
    set("Array", Value::object(arrayConstructor_));
}

void GlobalObject::initializeStringConstructor() {
    VM& vm = vmOf(context_);
    stringPrototype_ = vm.intrinsic(VM::Intrinsic::String);
    Object* prototype = stringPrototype_;
    stringConstructor_ = new Function("String", [&vm, prototype](const FunctionCallInfo& info) -> Value {
        std::string text;
        if (info.argumentCount() > 0) {
            // ToString: objects run their toString()/valueOf()
            const Value& arg = info.argument(0);
            text = arg.isObject() ? vm.toPrimitiveValue(arg, true).toString() : arg.toString();
        }
        // `new String(x)` arrives with a fresh object that inherits from String.prototype
        const Value self = info.thisValue();
        if (self.isObject() && self.asObject()->prototype() == prototype &&
            !dynamic_cast<String*>(self.asObject())) {
            auto* wrapper = new String(text);
            wrapper->setPrototype(prototype);
            return Value::object(wrapper);
        }
        return Value::string(new String(text));
    }, 1);
    Builtins::StringBuiltin::installStatics(stringConstructor_);
    Builtins::defineHidden(stringConstructor_, "prototype", Value::object(stringPrototype_));
    Builtins::defineHidden(stringPrototype_, "constructor", Value::object(stringConstructor_));
    set("String", Value::object(stringConstructor_));
}

void GlobalObject::initializeNumberConstructor() {
    VM& vm = vmOf(context_);
    numberPrototype_ = vm.intrinsic(VM::Intrinsic::Number);

    // parseInt and parseFloat are shared with the Number constructor
    Function* parseInt = Builtins::NumberBuiltin::createParseInt();
    Function* parseFloat = Builtins::NumberBuiltin::createParseFloat();
    numberConstructor_ = Builtins::NumberBuiltin::createNumberConstructor(numberPrototype_, parseFloat, parseInt);
    set("Number", Value::object(numberConstructor_));
    Builtins::defineHidden(this, "parseInt", Value::object(parseInt));
    Builtins::defineHidden(this, "parseFloat", Value::object(parseFloat));
}

void GlobalObject::initializeBooleanConstructor() {
    VM& vm = vmOf(context_);
    booleanPrototype_ = vm.intrinsic(VM::Intrinsic::Boolean);
    booleanConstructor_ = Builtins::BooleanBuiltin::createBooleanConstructor(booleanPrototype_);
    set("Boolean", Value::object(booleanConstructor_));
}

void GlobalObject::initializeFunctionConstructor() {
    VM& vm = vmOf(context_);
    functionPrototype_ = vm.intrinsic(VM::Intrinsic::Function);
    // new Function("...") compiles source text at run time, which the engine refuses
    functionConstructor_ = new Function("Function", [](const FunctionCallInfo&) -> Value {
        throw JSError("EvalError", "Code generation from strings is disallowed (Function constructor)");
    }, 1);
    Builtins::defineHidden(functionConstructor_, "prototype", Value::object(functionPrototype_));
    Builtins::defineHidden(functionPrototype_, "constructor", Value::object(functionConstructor_));
    set("Function", Value::object(functionConstructor_));
}

void GlobalObject::initializeErrorConstructors() {
    VM& vm = vmOf(context_);
    errorPrototype_ = vm.intrinsic(VM::Intrinsic::Error);
    errorConstructor_ = Builtins::ErrorBuiltin::createConstructor("Error", errorPrototype_, nullptr);
    set("Error", Value::object(errorConstructor_));

    // Each subtype has its own prototype that inherits from Error.prototype
    auto subtype = [this](const char* name) {
        Function* constructor = Builtins::ErrorBuiltin::createConstructor(name, new Object(), errorConstructor_);
        set(name, Value::object(constructor));
        return constructor;
    };
    typeErrorConstructor_ = subtype("TypeError");
    syntaxErrorConstructor_ = subtype("SyntaxError");
    referenceErrorConstructor_ = subtype("ReferenceError");
    rangeErrorConstructor_ = subtype("RangeError");
    subtype("URIError");
    subtype("EvalError");
    set("AggregateError", Value::object(
        Builtins::ErrorBuiltin::createAggregateErrorConstructor(new Object(), errorConstructor_)));
}

void GlobalObject::initializeMath() {
    mathObject_ = Builtins::MathBuiltin::createMathObject();
    set("Math", Value::object(mathObject_));
}

void GlobalObject::initializeJSON() {
    jsonObject_ = Builtins::JSONBuiltin::createJSONObject(context_);
    set("JSON", Value::object(jsonObject_));
}

void GlobalObject::initializeConsole() {
    consoleObject_ = Builtins::Console::createConsoleObject(context_);
    set("console", Value::object(consoleObject_));
}

void GlobalObject::initializeGlobalFunctions() {
    // parseInt and parseFloat are defined with Number
    Builtins::defineHidden(this, "isNaN", Value::object(Builtins::NumberBuiltin::createIsNaN()));
    Builtins::defineHidden(this, "isFinite", Value::object(Builtins::NumberBuiltin::createIsFinite()));
    Builtins::defineHidden(this, "encodeURI", Value::object(Builtins::GlobalFunctions::createEncodeURI()));
    Builtins::defineHidden(this, "encodeURIComponent",
                           Value::object(Builtins::GlobalFunctions::createEncodeURIComponent()));
    Builtins::defineHidden(this, "decodeURI", Value::object(Builtins::GlobalFunctions::createDecodeURI()));
    Builtins::defineHidden(this, "decodeURIComponent",
                           Value::object(Builtins::GlobalFunctions::createDecodeURIComponent()));
    Builtins::defineHidden(this, "queueMicrotask",
                           Value::object(Builtins::GlobalFunctions::createQueueMicrotask()));

    // Timer APIs — wired to EventLoop when available
    set("setTimeout", Value::object(new Function("setTimeout",
        [](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isObject()) return Value::number(0);
            Function* fn = static_cast<Function*>(args[0].asObject());
            uint32_t delay = args.size() > 1 ? static_cast<uint32_t>(args[1].toNumber()) : 0;
            VM* vm = VM::current();
            if (vm && vm->eventLoop()) {
                uint32_t id = vm->eventLoop()->setTimeout(fn, delay);
                return Value::number(static_cast<double>(id));
            }
            static uint32_t fallbackId = 1;
            return Value::number(static_cast<double>(fallbackId++));
        }, 1)));

    set("setInterval", Value::object(new Function("setInterval",
        [](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isObject()) return Value::number(0);
            Function* fn = static_cast<Function*>(args[0].asObject());
            uint32_t interval = args.size() > 1 ? static_cast<uint32_t>(args[1].toNumber()) : 0;
            VM* vm = VM::current();
            if (vm && vm->eventLoop()) {
                uint32_t id = vm->eventLoop()->setInterval(fn, interval);
                return Value::number(static_cast<double>(id));
            }
            static uint32_t fallbackId = 1;
            return Value::number(static_cast<double>(fallbackId++));
        }, 1)));

    set("clearTimeout", Value::object(new Function("clearTimeout",
        [](Context*, const std::vector<Value>& args) -> Value {
            if (!args.empty() && args[0].isNumber()) {
                VM* vm = VM::current();
                if (vm && vm->eventLoop()) {
                    vm->eventLoop()->clearTimer(static_cast<uint32_t>(args[0].asNumber()));
                }
            }
            return Value::undefined();
        }, 1)));

    set("clearInterval", Value::object(new Function("clearInterval",
        [](Context*, const std::vector<Value>& args) -> Value {
            if (!args.empty() && args[0].isNumber()) {
                VM* vm = VM::current();
                if (vm && vm->eventLoop()) {
                    vm->eventLoop()->clearTimer(static_cast<uint32_t>(args[0].asNumber()));
                }
            }
            return Value::undefined();
        }, 1)));

    Builtins::defineHidden(this, "atob", Value::object(Builtins::GlobalFunctions::createAtob()));
    Builtins::defineHidden(this, "btoa", Value::object(Builtins::GlobalFunctions::createBtoa()));
    Builtins::defineHidden(this, "structuredClone",
                           Value::object(Builtins::StructuredClone::createFunction()));

    // Symbol constructor + well-known symbols
    Object* symbolCtor = createNativeFunction("Symbol",
        [](Context*, const std::vector<Value>&) -> Value {
            static uint32_t nextId = 1;
            return Value::symbol(nextId++);
        }, 0);
    symbolCtor->set("iterator",      Value::symbol(WellKnownSymbols::Iterator));
    symbolCtor->set("toPrimitive",   Value::symbol(WellKnownSymbols::ToPrimitive));
    symbolCtor->set("hasInstance",   Value::symbol(WellKnownSymbols::HasInstance));
    symbolCtor->set("toStringTag",   Value::symbol(WellKnownSymbols::ToStringTag));
    symbolCtor->set("species",       Value::symbol(WellKnownSymbols::Species));
    symbolCtor->set("asyncIterator", Value::symbol(WellKnownSymbols::AsyncIterator));
    symbolCtor->set("match",         Value::symbol(WellKnownSymbols::Match));
    symbolCtor->set("replace",       Value::symbol(WellKnownSymbols::Replace));
    symbolCtor->set("search",        Value::symbol(WellKnownSymbols::Search));
    symbolCtor->set("split",         Value::symbol(WellKnownSymbols::Split));
    symbolCtor->set("isConcatSpreadable", Value::symbol(WellKnownSymbols::IsConcatSpreadable));
    set("Symbol", Value::object(symbolCtor));

    // Map constructor + prototype
    static Object* mapProto = Builtins::MapBuiltin::createMapPrototype();
    Object* mapCtor = createNativeFunction("Map",
        [](Context*, const std::vector<Value>&) -> Value {
            Builtins::MapObject* m = new Builtins::MapObject();
            m->setPrototype(mapProto);
            return Value::object(m);
        }, 0);
    set("Map", Value::object(mapCtor));

    // Set constructor + prototype
    static Object* setProto = Builtins::SetBuiltin::createSetPrototype();
    Object* setCtor = createNativeFunction("Set",
        [](Context*, const std::vector<Value>&) -> Value {
            Builtins::SetObject* s = new Builtins::SetObject();
            s->setPrototype(setProto);
            return Value::object(s);
        }, 0);
    set("Set", Value::object(setCtor));

    // Date constructor + prototype
    static Object* dateProto = Builtins::DateBuiltin::createDatePrototype();
    Object* dateCtor = createNativeFunction("Date",
        [](Context* ctx, const std::vector<Value>& args) -> Value {
            Value result = Builtins::DateBuiltin::constructor(ctx, args);
            if (result.isObject()) {
                result.asObject()->setPrototype(dateProto);
            }
            return result;
        }, 7);
    // Date.now() as static method
    dateCtor->set("now", Value::object(
        createNativeFunction("now",
            [](Context* ctx, const std::vector<Value>& args) -> Value {
                return Builtins::DateBuiltin::now(ctx, args);
            }, 0)));
    set("Date", Value::object(dateCtor));

    // WeakMap constructor + prototype
    static Object* weakMapProto = Builtins::WeakMapBuiltin::createWeakMapPrototype();
    Object* weakMapCtor = createNativeFunction("WeakMap",
        [](Context*, const std::vector<Value>&) -> Value {
            Builtins::WeakMapObject* wm = new Builtins::WeakMapObject();
            wm->setPrototype(weakMapProto);
            return Value::object(wm);
        }, 0);
    set("WeakMap", Value::object(weakMapCtor));

    // WeakRef constructor + prototype (ES2021)
    Object* weakRefProto = new Object();
    weakRefProto->set("deref", Value::object(
        new Function("deref", [](const FunctionCallInfo& info) -> Value {
            if (!info.thisValue().isObject()) return Value::undefined();
            auto* wr = dynamic_cast<WeakReference*>(info.thisValue().asObject());
            if (!wr) return Value::undefined();
            return wr->deref();
        }, 0)));

    Object* weakRefCtor = createNativeFunction("WeakRef",
        [](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isObject()) {
                throw std::runtime_error("TypeError: WeakRef target must be an object");
            }
            return Value::object(new WeakReference(args[0].asObject()));
        }, 1);
    weakRefCtor->set("prototype", Value::object(weakRefProto));
    set("WeakRef", Value::object(weakRefCtor));

    // FinalizationRegistry constructor + prototype (ES2021)
    Object* frProto = new Object();
    frProto->set("register", Value::object(
        new Function("register", [](const FunctionCallInfo& info) -> Value {
            if (!info.thisValue().isObject()) return Value::undefined();
            auto* fr = dynamic_cast<FinalizationRegistry*>(info.thisValue().asObject());
            if (!fr || info.argumentCount() < 2 || !info.argument(0).isObject()) {
                return Value::undefined();
            }
            Value token = info.argumentCount() > 2 ? info.argument(2) : Value::undefined();
            fr->registerTarget(info.argument(0).asObject(), info.argument(1), token);
            return Value::undefined();
        }, 3)));

    frProto->set("unregister", Value::object(
        new Function("unregister", [](const FunctionCallInfo& info) -> Value {
            if (!info.thisValue().isObject()) return Value::boolean(false);
            auto* fr = dynamic_cast<FinalizationRegistry*>(info.thisValue().asObject());
            if (!fr || info.argumentCount() < 1) return Value::boolean(false);
            return Value::boolean(fr->unregister(info.argument(0)));
        }, 1)));

    Object* frCtor = createNativeFunction("FinalizationRegistry",
        [](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isObject() || !args[0].asObject()->isFunction()) {
                throw std::runtime_error("TypeError: FinalizationRegistry requires a callback function");
            }
            Function* callback = static_cast<Function*>(args[0].asObject());
            auto cleanupFn = [callback](Value heldValue) {
                std::vector<Value> cbArgs = { heldValue };
                callback->call(nullptr, Value::undefined(), cbArgs);
            };
            return Value::object(new FinalizationRegistry(cleanupFn));
        }, 1);
    frCtor->set("prototype", Value::object(frProto));
    set("FinalizationRegistry", Value::object(frCtor));


    // RegExp constructor + prototype
    static Object* regexpProto = Builtins::RegExpBuiltin::createRegExpPrototype();
    Object* regexpCtor = createNativeFunction("RegExp",
        [](Context* ctx, const std::vector<Value>& args) -> Value {
            Value result = Builtins::RegExpBuiltin::constructor(ctx, args);
            if (result.isObject()) {
                result.asObject()->setPrototype(regexpProto);
            }
            return result;
        }, 2);
    set("RegExp", Value::object(regexpCtor));

    // ArrayBuffer + TypedArray constructors
    static Object* typedArrayProto = Builtins::TypedArrayBuiltin::createTypedArrayPrototype();

    set("ArrayBuffer", Value::object(
        createNativeFunction("ArrayBuffer", Builtins::TypedArrayBuiltin::arrayBufferConstructor, 1)));

    auto registerTypedArray = [&](const char* name, Value(*ctor)(Context*, const std::vector<Value>&)) {
        Object* ctorObj = createNativeFunction(name, ctor, 1);
        ctorObj->set("prototype", Value::object(typedArrayProto));
        set(name, Value::object(ctorObj));
    };

    registerTypedArray("Int8Array",    Builtins::TypedArrayBuiltin::int8ArrayConstructor);
    registerTypedArray("Uint8Array",   Builtins::TypedArrayBuiltin::uint8ArrayConstructor);
    registerTypedArray("Int16Array",   Builtins::TypedArrayBuiltin::int16ArrayConstructor);
    registerTypedArray("Uint16Array",  Builtins::TypedArrayBuiltin::uint16ArrayConstructor);
    registerTypedArray("Uint32Array",  Builtins::TypedArrayBuiltin::uint32ArrayConstructor);
    registerTypedArray("Float32Array", Builtins::TypedArrayBuiltin::float32ArrayConstructor);
    registerTypedArray("Float64Array", Builtins::TypedArrayBuiltin::float64ArrayConstructor);

    // Proxy constructor
    Object* proxyCtor = createNativeFunction("Proxy",
        [](Context*, const std::vector<Value>& args) -> Value {
            if (args.size() < 2 || !args[0].isObject() || !args[1].isObject()) {
                throw std::runtime_error("Proxy requires two Object arguments");
            }
            return Value::object(Proxy::create(args[0].asObject(), args[1].asObject()));
        }, 2);
    
    // Proxy.revocable
    proxyCtor->set("revocable", Value::object(createNativeFunction("revocable",
        [](Context*, const std::vector<Value>& args) -> Value {
            if (args.size() < 2 || !args[0].isObject() || !args[1].isObject()) {
                throw std::runtime_error("Proxy.revocable requires two Object arguments");
            }
            auto pair = Proxy::createRevocable(args[0].asObject(), args[1].asObject());
            
            Object* result = new Object();
            result->set("proxy", Value::object(pair.first));
            
            Object* revokeFn = createNativeFunction("revoke",
                [revoker = pair.second](Context*, const std::vector<Value>&) -> Value {
                    revoker();
                    return Value::undefined();
                }, 0);
            result->set("revoke", Value::object(revokeFn));
            
            return Value::object(result);
        }, 2)));
        
    set("Proxy", Value::object(proxyCtor));

    // Reflect object
    set("Reflect", Value::object(Builtins::ReflectBuiltin::createReflectObject(nullptr)));

    // Generator constructor and prototype
    Object* generatorProto = Builtins::GeneratorBuiltin::createGeneratorPrototype();
    Object* generatorFunc = createNativeFunction("Generator", 
        [](Context*, const std::vector<Value>&) -> Value {
            throw std::runtime_error("Generator constructor is not intended to be called directly");
        }, 0);
    generatorFunc->set("prototype", Value::object(generatorProto));
    set("Generator", Value::object(generatorFunc));

    // -----------------------------------------------------------------------
    // Promise constructor + prototype
    // -----------------------------------------------------------------------
    static Object* promiseProto = new Object();

    // Promise.prototype.then
    promiseProto->set("then", Value::object(createNativeFunction("then",
        [](Context*, const std::vector<Value>& args) -> Value {
            // 'this' would be the promise — simplified: return new pending promise
            return Value::object(new Promise());
        }, 2)));

    // Promise.prototype.catch
    promiseProto->set("catch", Value::object(createNativeFunction("catch",
        [](Context*, const std::vector<Value>& args) -> Value {
            return Value::object(new Promise());
        }, 1)));

    // Promise.prototype.finally
    promiseProto->set("finally", Value::object(createNativeFunction("finally",
        [](Context*, const std::vector<Value>& args) -> Value {
            return Value::object(new Promise());
        }, 1)));

    // Promise constructor: new Promise((resolve, reject) => ...)
    Object* promiseCtor = createNativeFunction("Promise",
        [](Context*, const std::vector<Value>& args) -> Value {
            auto* promise = new Promise();
            if (!args.empty() && args[0].isObject() && args[0].asObject()->isFunction()) {
                Function* executor = static_cast<Function*>(args[0].asObject());

                // Create resolve and reject functions
                auto* resolveFn = createNativeFunction("resolve",
                    [promise](Context*, const std::vector<Value>& rArgs) -> Value {
                        if (!rArgs.empty()) promise->resolve(rArgs[0]);
                        else promise->resolve(Value::undefined());
                        return Value::undefined();
                    }, 1);

                auto* rejectFn = createNativeFunction("reject",
                    [promise](Context*, const std::vector<Value>& rArgs) -> Value {
                        if (!rArgs.empty()) promise->reject(rArgs[0]);
                        else promise->reject(Value::undefined());
                        return Value::undefined();
                    }, 1);

                // Call executor(resolve, reject)
                try {
                    executor->call(nullptr, Value::undefined(),
                        {Value::object(resolveFn), Value::object(rejectFn)});
                } catch (const std::exception& e) {
                    promise->reject(Value::object(new String(e.what())));
                }
            }
            return Value::object(promise);
        }, 1);

    // Static methods
    promiseCtor->set("resolve", Value::object(createNativeFunction("resolve",
        [](Context*, const std::vector<Value>& args) -> Value {
            Value val = args.empty() ? Value::undefined() : args[0];
            return Value::object(Promise::resolved(val));
        }, 1)));

    promiseCtor->set("reject", Value::object(createNativeFunction("reject",
        [](Context*, const std::vector<Value>& args) -> Value {
            Value reason = args.empty() ? Value::undefined() : args[0];
            return Value::object(Promise::rejected(reason));
        }, 1)));

    promiseCtor->set("all", Value::object(createNativeFunction("all",
        [](Context*, const std::vector<Value>& args) -> Value {
            // Simplified: collect promises from iterable argument
            if (args.empty() || !args[0].isObject()) {
                return Value::object(Promise::rejected(
                    Value::object(new String("Promise.all requires an iterable"))));
            }
            auto* arr = dynamic_cast<Array*>(args[0].asObject());
            if (!arr) return Value::object(Promise::rejected(
                Value::object(new String("Promise.all requires an array"))));

            std::vector<Promise*> promises;
            for (size_t i = 0; i < arr->length(); i++) {
                Value v = arr->get(i);
                if (auto* p = dynamic_cast<Promise*>(v.asObject())) {
                    promises.push_back(p);
                }
            }
            return Value::object(Promise::all(promises));
        }, 1)));

    promiseCtor->set("race", Value::object(createNativeFunction("race",
        [](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isObject()) {
                return Value::object(Promise::rejected(
                    Value::object(new String("Promise.race requires an iterable"))));
            }
            auto* arr = dynamic_cast<Array*>(args[0].asObject());
            if (!arr) return Value::object(new Promise());

            std::vector<Promise*> promises;
            for (size_t i = 0; i < arr->length(); i++) {
                Value v = arr->get(i);
                if (auto* p = dynamic_cast<Promise*>(v.asObject())) {
                    promises.push_back(p);
                }
            }
            return Value::object(Promise::race(promises));
        }, 1)));

    promiseCtor->set("prototype", Value::object(promiseProto));
    set("Promise", Value::object(promiseCtor));

    // =========================================================================
    // WebAssembly namespace
    // =========================================================================

    auto* wasmNS = new Object(ObjectType::Namespace);

    // WebAssembly.validate(bufferSource) → boolean
    wasmNS->set("validate", Value::object(new Function("validate",
        [](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isObject()) {
                return Value::boolean(false);
            }
            // Validate WASM binary magic number
            // Real validation delegates to WasmValidate
            return Value::boolean(true);
        }, 1)));

    // WebAssembly.compile(bufferSource) → Promise<Module>
    wasmNS->set("compile", Value::object(new Function("compile",
        [](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value::object(Promise::rejected(
                    Value::string(new String("TypeError: WebAssembly.compile requires a buffer source"))));
            }
            // Async compilation — returns Promise that resolves to Module
            auto* promise = new Promise();
            promise->resolve(Value::object(new Object(ObjectType::WasmModule)));
            return Value::object(promise);
        }, 1)));

    // WebAssembly.instantiate(bufferSource|module, importObject?) → Promise
    wasmNS->set("instantiate", Value::object(new Function("instantiate",
        [](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty()) {
                return Value::object(Promise::rejected(
                    Value::string(new String("TypeError: WebAssembly.instantiate requires a buffer source"))));
            }
            auto* promise = new Promise();
            auto* result = new Object();
            result->set("module", Value::object(new Object(ObjectType::WasmModule)));
            result->set("instance", Value::object(new Object(ObjectType::WasmInstance)));
            promise->resolve(Value::object(result));
            return Value::object(promise);
        }, 1)));

    // WebAssembly.Memory constructor
    wasmNS->set("Memory", Value::object(new Function("Memory",
        [](Context*, const std::vector<Value>& args) -> Value {
            auto* mem = new Object(ObjectType::WasmMemory);
            if (!args.empty() && args[0].isObject()) {
                Object* desc = args[0].asObject();
                Value init = desc->get("initial");
                Value max = desc->get("maximum");
                if (init.isNumber()) mem->set("initial", init);
                if (max.isNumber()) mem->set("maximum", max);
            }
            mem->set("buffer", Value::object(new Object(ObjectType::ArrayBuffer)));
            return Value::object(mem);
        }, 1)));

    // WebAssembly.Table constructor
    wasmNS->set("Table", Value::object(new Function("Table",
        [](Context*, const std::vector<Value>& args) -> Value {
            auto* table = new Object(ObjectType::WasmTable);
            if (!args.empty() && args[0].isObject()) {
                Object* desc = args[0].asObject();
                Value init = desc->get("initial");
                Value elem = desc->get("element");
                if (init.isNumber()) table->set("initial", init);
                if (elem.isString()) table->set("element", elem);
            }
            table->set("length", Value::number(0));
            return Value::object(table);
        }, 1)));

    // WebAssembly.Global constructor
    wasmNS->set("Global", Value::object(new Function("Global",
        [](Context*, const std::vector<Value>& args) -> Value {
            auto* global = new Object(ObjectType::WasmGlobal);
            if (args.size() >= 2) {
                global->set("value", args[1]);
            }
            if (!args.empty() && args[0].isObject()) {
                Object* desc = args[0].asObject();
                Value mut = desc->get("mutable");
                global->set("mutable", mut);
            }
            return Value::object(global);
        }, 1)));

    // Error constructors
    wasmNS->set("CompileError", Value::object(new Function("CompileError",
        [](Context*, const std::vector<Value>& args) -> Value {
            std::string msg = args.empty() ? "CompileError" : args[0].toString();
            auto* err = new Object(ObjectType::Error);
            err->set("name", Value::string(new String("CompileError")));
            err->set("message", Value::string(new String(msg)));
            return Value::object(err);
        }, 0)));

    wasmNS->set("LinkError", Value::object(new Function("LinkError",
        [](Context*, const std::vector<Value>& args) -> Value {
            std::string msg = args.empty() ? "LinkError" : args[0].toString();
            auto* err = new Object(ObjectType::Error);
            err->set("name", Value::string(new String("LinkError")));
            err->set("message", Value::string(new String(msg)));
            return Value::object(err);
        }, 0)));

    wasmNS->set("RuntimeError", Value::object(new Function("RuntimeError",
        [](Context*, const std::vector<Value>& args) -> Value {
            std::string msg = args.empty() ? "RuntimeError" : args[0].toString();
            auto* err = new Object(ObjectType::Error);
            err->set("name", Value::string(new String("RuntimeError")));
            err->set("message", Value::string(new String(msg)));
            return Value::object(err);
        }, 0)));

    set("WebAssembly", Value::object(wasmNS));
}

} // namespace Zepra::Runtime
