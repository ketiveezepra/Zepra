// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file generator.cpp
 * @brief ES6 Generator Object implementation
 *
 * Implements GeneratorObject and Generator.prototype methods (next, return, throw)
 * following the ES2024 §27.5 Generator Objects specification.
 *
 * Key pattern: GeneratorResumeKind (Next/Throw/Return) dispatches behavior.
 */

#include "builtins/generator.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/execution/vm.hpp"
#include "runtime/objects/well_known_symbols.hpp"
#include <stdexcept>

namespace Zepra::Builtins {

GeneratorObject::GeneratorObject(Runtime::Function* function, Runtime::Value thisBinding, const std::vector<Runtime::Value>& args)
    : Object(Runtime::ObjectType::Ordinary), initialArgs(args) {
    frame.function = function;
    frame.thisValue = thisBinding;
    frame.suspendedIP = 0;
    frame.stackBase = 0;
    frame.isCompleted = false;
    frame.isStarted = false;
}

// ES2024 §27.5.3.2 Generator.prototype.next(value)
Runtime::Value GeneratorBuiltin::next(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject()) {
        throw std::runtime_error("Generator.prototype.next called on incompatible receiver");
    }

    GeneratorObject* genObj = dynamic_cast<GeneratorObject*>(thisVal.asObject());
    if (!genObj) {
        throw std::runtime_error("Generator.prototype.next called on incompatible receiver");
    }

    if (genObj->frame.isCompleted) {
        Runtime::Object* iterResult = new Runtime::Object();
        iterResult->set("value", Runtime::Value::undefined());
        iterResult->set("done", Runtime::Value::boolean(true));
        return Runtime::Value::object(iterResult);
    }

    // The generator body runs on the VM executing the caller, with its globals
    Runtime::VM* vm = Runtime::VM::current();
    if (!vm) {
        throw std::runtime_error("Generator.prototype.next called outside a running script");
    }
    Runtime::Value yieldVal = info.argumentCount() > 0 ? info.argument(0) : Runtime::Value::undefined();

    Runtime::Value result = vm->resumeGenerator(&(genObj->frame), yieldVal, genObj->initialArgs);

    Runtime::Object* iterResult = new Runtime::Object();
    iterResult->set("value", result);
    iterResult->set("done", Runtime::Value::boolean(genObj->frame.isCompleted));
    return Runtime::Value::object(iterResult);
}

// ES2024 §27.5.3.4 Generator.prototype.return(value)
Runtime::Value GeneratorBuiltin::return_(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject()) {
        throw std::runtime_error("Generator.prototype.return called on incompatible receiver");
    }

    GeneratorObject* genObj = dynamic_cast<GeneratorObject*>(thisVal.asObject());
    if (!genObj) {
        throw std::runtime_error("Generator.prototype.return called on incompatible receiver");
    }

    Runtime::Value retVal = info.argumentCount() > 0 ? info.argument(0) : Runtime::Value::undefined();

    // If generator hasn't started or is already suspended, we can immediately
    // close it and return the value (ES2024 §27.5.3.4 step 5-7)
    genObj->frame.isCompleted = true;
    genObj->frame.savedStack.clear();

    Runtime::Object* iterResult = new Runtime::Object();
    iterResult->set("value", retVal);
    iterResult->set("done", Runtime::Value::boolean(true));
    return Runtime::Value::object(iterResult);
}

// ES2024 §27.5.3.3 Generator.prototype.throw(exception)
//
// the exception is set as pending and the interpreter's exception-handling
// machinery (try/catch) in the generator's bytecode picks it up.
// If uncaught, the generator completes and the exception propagates.
Runtime::Value GeneratorBuiltin::throw_(const Runtime::FunctionCallInfo& info) {
    Runtime::Value thisVal = info.thisValue();
    if (!thisVal.isObject()) {
        throw std::runtime_error("Generator.prototype.throw called on incompatible receiver");
    }

    GeneratorObject* genObj = dynamic_cast<GeneratorObject*>(thisVal.asObject());
    if (!genObj) {
        throw std::runtime_error("Generator.prototype.throw called on incompatible receiver");
    }

    Runtime::Value exception = info.argumentCount() > 0 ? info.argument(0) : Runtime::Value::undefined();

    // If the generator hasn't started yet (before first yield), or is already
    // completed, the throw immediately propagates without resuming.
    if (!genObj->frame.isStarted || genObj->frame.isCompleted) {
        genObj->frame.isCompleted = true;
        // Propagate the exception to the caller per spec
        std::string msg = exception.isString()
            ? static_cast<Runtime::String*>(exception.asObject())->value()
            : exception.toString();
        throw std::runtime_error(msg);
    }

    // Generator is suspended at a yield point. Resume execution and let the
    // VM's exception handler dispatch to any try/catch in the generator's
    // bytecode. If uncaught, the generator completes with the exception.
    genObj->frame.isCompleted = true;
    genObj->frame.savedStack.clear();

    // Propagate exception — if the generator has a try/catch around the
    // yield, the bytecode interpreter will handle it via OP_TRY/OP_CATCH.
    // For now, since we can't inject a pending exception into the miniVM's
    // run loop mid-resume, we throw directly from C++. This is correct
    // behavior for generators without try/catch around their yield points.
    std::string msg = exception.toString();
    throw std::runtime_error(msg);
}

Runtime::Object* GeneratorBuiltin::createGeneratorPrototype() {
    Runtime::Object* proto = new Runtime::Object();

    proto->set("next", Runtime::Value::object(
        new Runtime::Function("next", next, 1)));
    proto->set("return", Runtime::Value::object(
        new Runtime::Function("return", return_, 1)));
    proto->set("throw", Runtime::Value::object(
        new Runtime::Function("throw", throw_, 1)));

    // ES2024 §27.5.3.5 Generator.prototype[@@iterator]()
    proto->set(Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::Iterator), Runtime::Value::object(
        new Runtime::Function("[Symbol.iterator]",
            [](const Runtime::FunctionCallInfo& info) -> Runtime::Value {
                return info.thisValue();
            }, 0)));

    // ES2024 §27.5.3.6 Generator.prototype[@@toStringTag]
    proto->set(Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::ToStringTag),
               Runtime::Value::string(new Runtime::String("Generator")));

    return proto;
}

} // namespace Zepra::Builtins
