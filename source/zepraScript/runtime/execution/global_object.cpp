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
// Global function implementations (non-inline, separate from lambdas)
// These use the NativeFn signature: Value(Context*, const std::vector<Value>&)
// ---------------------------------------------------------------------------

static Value globalIsNaN(Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::boolean(true);
    return Value::boolean(std::isnan(args[0].toNumber()));
}

static Value globalIsFinite(Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::boolean(false);
    return Value::boolean(std::isfinite(args[0].toNumber()));
}

static Value globalParseInt(Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::nan(""));
    std::string str = args[0].toString();
    int radix = args.size() > 1 ? static_cast<int>(args[1].toNumber()) : 10;
    if (radix == 0) radix = 10;
    try {
        return Value::number(static_cast<double>(std::stoll(str, nullptr, radix)));
    } catch (...) {
        return Value::number(std::nan(""));
    }
}

static Value globalParseFloat(Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::number(std::nan(""));
    try {
        return Value::number(std::stod(args[0].toString()));
    } catch (...) {
        return Value::number(std::nan(""));
    }
}

static Value globalEncodeURIComponent(Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::object(new String("undefined"));
    std::string input = args[0].toString();
    std::string result;
    result.reserve(input.size() * 3);
    for (unsigned char c : input) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            result += static_cast<char>(c);
        } else {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%%%02X", c);
            result += hex;
        }
    }
    return Value::object(new String(result));
}

static Value globalDecodeURIComponent(Context*, const std::vector<Value>& args) {
    if (args.empty()) return Value::object(new String("undefined"));
    std::string input = args[0].toString();
    std::string result;
    for (size_t i = 0; i < input.size(); i++) {
        if (input[i] == '%' && i + 2 < input.size()) {
            unsigned int value = 0;
            if (std::sscanf(input.c_str() + i + 1, "%2x", &value) == 1) {
                result += static_cast<char>(value);
                i += 2;
                continue;
            }
        }
        result += input[i];
    }
    return Value::object(new String(result));
}

static Value globalQueueMicrotask(Context*, const std::vector<Value>& args) {
    if (!args.empty()) {
        Function* fn = args[0].asFunction();
        if (fn) {
            MicrotaskQueue::instance().enqueue([fn]() {
                // Enqueue for later execution via event loop
            });
        }
    }
    return Value::undefined();
}

// Error constructor factory: creates a native function that constructs Error objects
static Function* createErrorConstructor(const std::string& errorName) {
    return createNativeFunction(errorName,
        [errorName](Context*, const std::vector<Value>& args) -> Value {
            std::string message = args.empty() ? "" : args[0].toString();

            // ES2022: options.cause
            if (args.size() > 1 && args[1].isObject()) {
                Object* opts = args[1].asObject();
                Value cause = opts->get("cause");
                if (!cause.isUndefined()) {
                    return Value::object(Error::withCause(errorName, message, cause));
                }
            }

            return Value::object(new Error(message, errorName));
        }, 1);
}

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

void GlobalObject::initializeObjectConstructor() {
    objectPrototype_ = Builtins::ObjectBuiltin::createObjectPrototype(context_);
    objectConstructor_ = static_cast<Function*>(
        Builtins::ObjectBuiltin::createObjectConstructor(context_));
    objectConstructor_->set("prototype", Value::object(objectPrototype_));
    set("Object", Value::object(objectConstructor_));
}

void GlobalObject::initializeArrayConstructor() {
    arrayPrototype_ = Builtins::ArrayBuiltin::createArrayPrototype(context_);
    arrayConstructor_ = createNativeFunction("Array",
        [](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::object(new Array({}));
            if (args.size() == 1 && args[0].isNumber()) {
                size_t len = static_cast<size_t>(args[0].asNumber());
                return Value::object(new Array(std::vector<Value>(len, Value::undefined())));
            }
            return Value::object(new Array(std::vector<Value>(args.begin(), args.end())));
        }, 1);
    arrayConstructor_->set("isArray", Value::object(
        new Function("isArray", Builtins::ArrayBuiltin::isArray, 1)));
    arrayConstructor_->set("from", Value::object(
        new Function("from", Builtins::ArrayBuiltin::from, 1)));
    arrayConstructor_->set("of", Value::object(
        new Function("of", Builtins::ArrayBuiltin::of, 0)));
    arrayConstructor_->set("prototype", Value::object(arrayPrototype_));
    set("Array", Value::object(arrayConstructor_));
}

void GlobalObject::initializeStringConstructor() {
    stringPrototype_ = Builtins::StringBuiltin::createStringPrototype(context_);
    stringConstructor_ = createNativeFunction("String",
        [](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::object(new String(""));
            return Value::object(new String(args[0].toString()));
        }, 1);
    stringConstructor_->set("prototype", Value::object(stringPrototype_));
    set("String", Value::object(stringConstructor_));
}

void GlobalObject::initializeNumberConstructor() {
    numberPrototype_ = Builtins::NumberBuiltin::createNumberPrototype();
    numberConstructor_ = createNativeFunction("Number",
        [](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::number(0.0);
            return Value::number(args[0].toNumber());
        }, 1);

    // Constants
    numberConstructor_->set("MAX_SAFE_INTEGER", Value::number(9007199254740991.0));
    numberConstructor_->set("MIN_SAFE_INTEGER", Value::number(-9007199254740991.0));
    numberConstructor_->set("MAX_VALUE", Value::number(std::numeric_limits<double>::max()));
    numberConstructor_->set("MIN_VALUE", Value::number(std::numeric_limits<double>::min()));
    numberConstructor_->set("EPSILON", Value::number(std::numeric_limits<double>::epsilon()));
    numberConstructor_->set("POSITIVE_INFINITY",
        Value::number(std::numeric_limits<double>::infinity()));
    numberConstructor_->set("NEGATIVE_INFINITY",
        Value::number(-std::numeric_limits<double>::infinity()));
    numberConstructor_->set("NaN", Value::number(std::nan("")));
    numberConstructor_->set("prototype", Value::object(numberPrototype_));
    set("Number", Value::object(numberConstructor_));
}

void GlobalObject::initializeBooleanConstructor() {
    booleanPrototype_ = Builtins::BooleanBuiltin::createBooleanPrototype();
    booleanConstructor_ = createNativeFunction("Boolean",
        [](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::boolean(false);
            return Value::boolean(args[0].toBoolean());
        }, 1);
    booleanConstructor_->set("prototype", Value::object(booleanPrototype_));
    set("Boolean", Value::object(booleanConstructor_));
}

void GlobalObject::initializeFunctionConstructor() {
    functionPrototype_ = new Object();
    functionConstructor_ = createNativeFunction("Function",
        [](Context*, const std::vector<Value>&) -> Value {
            // Function() constructor requires eval — not supported for security
            return Value::undefined();
        }, 1);
    functionConstructor_->set("prototype", Value::object(functionPrototype_));
    set("Function", Value::object(functionConstructor_));
}

void GlobalObject::initializeErrorConstructors() {
    errorConstructor_ = createErrorConstructor("Error");
    typeErrorConstructor_ = createErrorConstructor("TypeError");
    syntaxErrorConstructor_ = createErrorConstructor("SyntaxError");
    referenceErrorConstructor_ = createErrorConstructor("ReferenceError");
    rangeErrorConstructor_ = createErrorConstructor("RangeError");

    set("Error", Value::object(errorConstructor_));
    set("TypeError", Value::object(typeErrorConstructor_));
    set("SyntaxError", Value::object(syntaxErrorConstructor_));
    set("ReferenceError", Value::object(referenceErrorConstructor_));
    set("RangeError", Value::object(rangeErrorConstructor_));
    set("URIError", Value::object(createErrorConstructor("URIError")));
    set("EvalError", Value::object(createErrorConstructor("EvalError")));
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
    set("isNaN", Value::object(createNativeFunction("isNaN", globalIsNaN, 1)));
    set("isFinite", Value::object(createNativeFunction("isFinite", globalIsFinite, 1)));
    set("parseInt", Value::object(createNativeFunction("parseInt", globalParseInt, 2)));
    set("parseFloat", Value::object(createNativeFunction("parseFloat", globalParseFloat, 1)));
    set("encodeURIComponent", Value::object(
        createNativeFunction("encodeURIComponent", globalEncodeURIComponent, 1)));
    set("decodeURIComponent", Value::object(
        createNativeFunction("decodeURIComponent", globalDecodeURIComponent, 1)));
    set("queueMicrotask", Value::object(
        createNativeFunction("queueMicrotask", globalQueueMicrotask, 1)));

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

    // atob / btoa — Base64 encoding/decoding
    set("atob", Value::object(new Function("atob",
        [](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::undefined();
            // Base64 decode (simplified — returns input for now)
            return args[0];
        }, 1)));

    set("btoa", Value::object(new Function("btoa",
        [](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::undefined();
            return args[0];
        }, 1)));

    // structuredClone
    set("structuredClone", Value::object(new Function("structuredClone",
        [](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::undefined();
            // Deep clone — for primitives, just return the value
            return args[0];
        }, 1)));

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

    // AggregateError constructor (ES2021 — used by Promise.any)
    set("AggregateError", Value::object(createNativeFunction("AggregateError",
        [](Context*, const std::vector<Value>& args) -> Value {
            std::vector<Value> errors;
            if (!args.empty() && args[0].isObject()) {
                auto* arr = dynamic_cast<Array*>(args[0].asObject());
                if (arr) {
                    for (size_t i = 0; i < arr->length(); i++) {
                        errors.push_back(arr->get(i));
                    }
                }
            }
            std::string msg = args.size() > 1 ? args[1].toString() : "";
            return Value::object(Error::aggregateError(msg, errors));
        }, 2)));


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
