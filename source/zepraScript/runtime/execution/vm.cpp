// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file vm.cpp
 * @brief JavaScript Virtual Machine implementation
 */

#include "runtime/execution/vm.hpp"
#include "runtime/execution/js_error.hpp"
#include <fstream>
#include "runtime/objects/object.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/async/async_function.hpp"
#include "runtime/execution/global_object.hpp"
#include "builtins/console.hpp"
#include "builtins/math.hpp"
#include "builtins/generator.hpp"
#include "runtime/handles/module_loader.hpp"
#include "bytecode/bytecode_generator.hpp"
#include "frontend/source_code.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "frontend/syntax_checker.hpp"
#include "runtime/execution/Sandbox.h"
#include "heap/gc_heap.hpp"
#include "builtins/string.hpp"
#include "builtins/number.hpp"
#include "builtins/boolean.hpp"
#include "builtins/error.hpp"
#include "builtins/list_iterator.hpp"
#include "builtins/array.hpp"
#include "runtime/async/promise.hpp"
#include "runtime/objects/well_known_symbols.hpp"
#include "builtins/map.hpp"
#include "builtins/set.hpp"
#include "builtins/object_builtins.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <unordered_set>
#include "runtime/execution/event_loop.hpp"

namespace Zepra::Runtime {

using Zepra::Bytecode::Opcode;
using Zepra::Bytecode::BytecodeChunk;

// Thread-local current VM for callback execution
thread_local VM* VM::currentVM_ = nullptr;

ThrownException::ThrownException(Value value)
    : std::runtime_error([&value] {
          // "Name: message" for error objects, the value itself otherwise
          if (value.isObject()) {
              Object* obj = value.asObject();
              Value name = obj->get("name");
              Value message = obj->get("message");
              if (name.isString() || message.isString()) {
                  std::string text = name.isString() ? name.toString() : "Error";
                  if (message.isString()) text += ": " + message.toString();
                  return text;
              }
          }
          return value.toString();
      }())
    , value_(value) {}

namespace {

// An element key that is an array index: an integral number, or a canonical
// numeric string such as "3" (for-in yields strings), below 2^32 - 1.
bool toArrayIndex(const Value& key, uint32_t& index) {
    if (key.isNumber()) {
        double d = key.asNumber();
        if (!(d >= 0.0) || d > 4294967294.0 || d != std::floor(d)) return false;
        index = static_cast<uint32_t>(d);
        return true;
    }
    if (key.isString()) {
        const std::string& s = static_cast<String*>(key.asObject())->value();
        if (s.empty() || s.size() > 10 || (s.size() > 1 && s[0] == '0')) return false;
        uint64_t v = 0;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + static_cast<uint64_t>(c - '0');
        }
        if (v > 4294967294ULL) return false;
        index = static_cast<uint32_t>(v);
        return true;
    }
    return false;
}

bool isNullish(const Value& value) {
    return value.isNull() || value.isUndefined();
}

// Property name of a key that is not an array index
std::string propertyKeyName(const Value& key) {
    if (key.isString()) return static_cast<String*>(key.asObject())->value();
    if (key.isSymbol()) return symbolPropertyKey(key.asSymbol());
    return key.toString();
}

// How a value is named in a TypeError message
std::string describeForError(const Value& value) {
    if (value.isUndefined()) return "undefined";
    if (value.isNull()) return "null";
    if (value.isObject()) return value.asObject()->isFunction() ? "function" : "object";
    std::string text = value.toString();
    if (value.isString()) text = "\"" + text + "\"";
    return text.size() > 40 ? text.substr(0, 40) + "..." : text;
}

// Bytes in the UTF-8 sequence that starts with `lead`
size_t utf8SequenceLength(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead >> 5) == 0x6) return 2;
    if ((lead >> 4) == 0xE) return 3;
    if ((lead >> 3) == 0x1E) return 4;
    return 1;  // Stray continuation byte: step over it
}

VM& runningVM() {
    VM* vm = VM::current();
    if (!vm) throw std::runtime_error("No script is running");
    return *vm;
}

Function* thisFunction(const FunctionCallInfo& info, const char* method) {
    Value fn = info.thisValue();
    if (!fn.isObject() || !fn.asObject()->isFunction()) {
        throw std::runtime_error(std::string("Function.prototype.") + method +
                                 " called on a value that is not a function");
    }
    return static_cast<Function*>(fn.asObject());
}

// Arguments for apply(): an array, an array-like object, or nothing
std::vector<Value> argumentList(const Value& list) {
    if (list.isNull() || list.isUndefined()) return {};
    if (!list.isObject()) {
        throw std::runtime_error("CreateListFromArrayLike called on non-object");
    }
    Object* obj = list.asObject();
    if (auto* array = dynamic_cast<Array*>(obj)) return array->elements();
    double length = obj->get("length").toNumber();
    std::vector<Value> args;
    for (uint32_t i = 0; i < length && i < 65536; i++) args.push_back(obj->get(i));
    return args;
}

// Adds call, apply, bind and toString to a Function.prototype, which every
// function inherits when it does not define the property itself
void populateFunctionPrototype(Object* p) {
    auto method = [p](const char* name, size_t arity, BuiltinFn fn) {
        PropertyDescriptor desc;
        desc.value = Value::object(new Function(name, std::move(fn), arity));
        desc.attributes = PropertyAttribute::Writable | PropertyAttribute::Configurable;
        p->defineProperty(name, desc);
    };
    method("call", 1, [](const FunctionCallInfo& info) -> Value {
        Function* fn = thisFunction(info, "call");
        const auto& all = info.arguments();
        std::vector<Value> args(all.size() > 1 ? all.begin() + 1 : all.end(), all.end());
        return runningVM().executeCallback(fn, info.argument(0), args);
    });
    method("apply", 2, [](const FunctionCallInfo& info) -> Value {
        Function* fn = thisFunction(info, "apply");
        return runningVM().executeCallback(fn, info.argument(0), argumentList(info.argument(1)));
    });
    method("bind", 1, [](const FunctionCallInfo& info) -> Value {
        Function* fn = thisFunction(info, "bind");
        const auto& all = info.arguments();
        std::vector<Value> bound(all.size() > 1 ? all.begin() + 1 : all.end(), all.end());
        return Value::object(fn->bind(info.argument(0), bound));
    });
    method("toString", 0, [](const FunctionCallInfo& info) -> Value {
        Function* fn = thisFunction(info, "toString");
        return Value::string(new String("function " + fn->name() + "() { [native code] }"));
    });
}

// State of a for-of loop, spread or array destructuring. Arrays and other
// indexed objects are read live, strings by code point, Map and Set from a
// snapshot, everything else through its [Symbol.iterator]() object. The
// values it holds are also stored as properties so a collector sees them.
class IteratorState : public Object {
public:
    enum class Kind { Indexed, String, Protocol };

    IteratorState(Kind kind, Value source) : kind_(kind), source_(source) {
        set(" source", source);
    }

    void setNextMethod(Value next) {
        nextMethod_ = next;
        set(" next", next);
    }

    void setSnapshot(Array* snapshot) {
        snapshot_ = snapshot;
        set(" snapshot", Value::object(snapshot));
    }

    Kind kind_;
    Value source_;
    Value nextMethod_;
    Array* snapshot_ = nullptr;
    size_t position_ = 0;
    bool done_ = false;
};

} // namespace

VM::VM(Context* context) : context_(context), jit_(this) {
    stack_.reserve(ZEPRA_MAX_CALL_STACK_DEPTH * 256);
    heapStack_.reserve(HEAP_STACK_PREALLOC);
    
    // Register JS spec globals so they're always available
    setGlobal("undefined", Value::undefined());
    setGlobal("NaN", Value::number(std::nan("")));
    setGlobal("Infinity", Value::number(std::numeric_limits<double>::infinity()));
}

VM::~VM() {
    // GCHeap owns all managed objects — just clear the reference map.
    // When ScriptContext destroys its GCHeap, all objects are freed.
    globals_.clear();
}

ExecutionResult VM::execute(const Bytecode::BytecodeChunk* chunk) {
    ExecutionResult result;
    
    if (!chunk) {
        result.status = ExecutionResult::Status::Error;
        result.error = "No bytecode to execute";
        return result;
    }
    
    terminationRequested_ = false;

    // Set current VM for callbacks
    VM* previousVM = currentVM_;
    currentVM_ = this;

    // The script runs in a frame of its own, like a function, so its block
    // locals are addressed relative to where it started on the stack
    const Bytecode::BytecodeChunk* savedChunk = chunk_;
    const size_t savedIP = ip_;
    const size_t savedDepth = callDepth();
    const size_t savedStack = stack_.size();
    const size_t savedHandlers = exceptionHandlers_.size();

    VMCallFrame frame;
    frame.returnAddress = SIZE_MAX;  // OP_RETURN leaves run() here
    frame.slotBase = savedStack;
    frame.savedChunk = savedChunk;
    pushCallFrame(frame);
    chunk_ = chunk;
    ip_ = 0;

    try {
        run();
        result.status = ExecutionResult::Status::Success;
        result.value = pop();

        // Drain microtask queue (promise callbacks, queueMicrotask)
        MicrotaskQueue::instance().process();
    } catch (const ThrownException& e) {
        result.status = ExecutionResult::Status::Exception;
        result.value = e.value();
        result.error = std::string("Uncaught ") + e.what();
        unwindTo(savedDepth, savedStack, savedHandlers);
    } catch (const std::exception& e) {
        result.status = ExecutionResult::Status::Error;
        result.error = e.what();
        unwindTo(savedDepth, savedStack, savedHandlers);
    }
    chunk_ = savedChunk;
    ip_ = savedIP;

    // Restore previous VM
    currentVM_ = previousVM;

    return result;
}

// Drops everything a failed run left behind: frames, stack values (closing
// any upvalues over them) and exception handlers.
void VM::unwindTo(size_t depth, size_t stackSize, size_t handlerCount) {
    while (callDepth() > depth) popCallFrame();
    if (stack_.size() > stackSize) {
        closeUpvalues(stack_.data() + stackSize);
        stack_.erase(stack_.begin() + static_cast<std::ptrdiff_t>(stackSize), stack_.end());
    }
    if (exceptionHandlers_.size() > handlerCount) exceptionHandlers_.resize(handlerCount);
    isYielding_ = false;
}

void VM::run() {
    instructionCounter_ = 0;
    ++runDepth_;
    struct RunDepthGuard {
        size_t& depth;
        ~RunDepthGuard() { --depth; }
    } runDepthGuard{runDepth_};

    // Calls switch chunk_, so the end is checked against the current chunk
    while (__builtin_expect(ip_ < chunk_->code().size(), 1)) {
        if (__builtin_expect(isYielding_ || terminationRequested_, 0)) break;
        
        instructionCounter_++;
        if (__builtin_expect(instructionCounter_ >= LIMIT_CHECK_INTERVAL, 0)) {
            instructionCounter_ = 0;
            
            if (resourceMonitor_) {
                resourceMonitor_->addInstructions(LIMIT_CHECK_INTERVAL);
                if (!resourceMonitor_->checkLimits()) {
                    terminationRequested_ = true;
                    throw SecurityError(SecurityError::Type::Timeout,
                        "Execution limit exceeded");
                }
            }
            
            if (gcHeap_) {
                gcHeap_->maybeCollect();
            }
        }
        
        Opcode op = static_cast<Opcode>(readByte());
        try {
            dispatch(op);
        } catch (const SecurityError&) {
            throw;  // Execution limits end the script; page code cannot catch them
        } catch (const ThrownException& e) {
            throwValue(e.value());  // From a builtin or callback: catchable here
        } catch (const JSError& e) {
            throwValue(makeError(e.type(), e.what()));
        } catch (const std::runtime_error& e) {
            // Errors the engine detects become catchable JavaScript errors
            const std::string message = e.what();
            const bool tooDeep = message.find("tack overflow") != std::string::npos ||
                                 message.find("call stack") != std::string::npos;
            throwValue(makeError(tooDeep ? "RangeError" : "TypeError", message));
        }
    }
}

void VM::throwValue(Value exception) {
    // Handlers left by frames that are gone (generator suspension, unwound
    // callbacks) cannot catch anything
    while (!exceptionHandlers_.empty() &&
           exceptionHandlers_.back().callStackLevel > callDepth()) {
        exceptionHandlers_.pop_back();
    }
    // Only a handler installed by this run() can catch here: one from an
    // outer run() is on the far side of native code, which must unwind first
    if (exceptionHandlers_.empty() || exceptionHandlers_.back().runDepth != runDepth_) {
        throw ThrownException(exception);
    }
    ExceptionHandler handler = exceptionHandlers_.back();
    exceptionHandlers_.pop_back();

    while (callDepth() > handler.callStackLevel) popCallFrame();
    if (stack_.size() > handler.stackLevel) {
        closeUpvalues(stack_.data() + handler.stackLevel);
        stack_.erase(stack_.begin() + static_cast<std::ptrdiff_t>(handler.stackLevel), stack_.end());
    }
    chunk_ = handler.chunk;
    ip_ = handler.catchAddress;
    exceptionValue_ = exception;
    hasException_ = true;
}

// ToPrimitive: an object's valueOf()/toString() result, in hint order. An
// array without usable methods joins its elements, as Array.prototype.toString
// does; other objects fall back to their default string form.
Value VM::toPrimitive(const Value& value, bool preferString) {
    if (!value.isObject()) return value;
    const char* first = preferString ? "toString" : "valueOf";
    const char* second = preferString ? "valueOf" : "toString";
    for (const char* name : {first, second}) {
        Value method = getProperty(value, name);
        if (method.isObject() && method.asObject()->isFunction()) {
            Value result = executeCallback(static_cast<Function*>(method.asObject()), value, {});
            if (!result.isObject()) return result;
        }
    }
    if (auto* array = dynamic_cast<Array*>(value.asObject())) {
        std::string joined;
        const auto& elements = array->elements();
        for (size_t i = 0; i < elements.size(); i++) {
            if (i > 0) joined += ",";
            if (!isNullish(elements[i])) joined += toPrimitive(elements[i], true).toString();
        }
        return Value::string(new String(joined));
    }
    return Value::string(new String(value.toString()));
}

void VM::toPrimitiveOperands(Value& a, Value& b) {
    if (a.isObject()) a = toPrimitive(a, false);
    if (b.isObject()) b = toPrimitive(b, false);
}

Value VM::makeError(const std::string& type, const std::string& message) {
    auto* error = new Error(message, type);
    Object* prototype = nullptr;
    Value ctor = getGlobal(type);
    if (ctor.isObject()) {
        Value proto = ctor.asObject()->get("prototype");
        if (proto.isObject()) prototype = proto.asObject();
    }
    if (!prototype) {
        // No global constructor of that type (a bare VM): the base Error
        // prototype, with the type named on the object itself
        prototype = intrinsic(Intrinsic::Error);
        PropertyDescriptor name;
        name.value = Value::string(new String(type));
        name.attributes = PropertyAttribute::Writable | PropertyAttribute::Configurable;
        error->defineProperty("name", name);
    }
    error->setPrototype(prototype);

    PropertyDescriptor stack;
    stack.value = Value::string(new String(type + (message.empty() ? "" : ": " + message) + "\n" +
                                           captureStackTrace()));
    stack.attributes = PropertyAttribute::Writable | PropertyAttribute::Configurable;
    error->defineProperty("stack", stack);
    return Value::object(error);
}

bool VM::shouldTerminate() const {
    if (terminationRequested_) return true;
    if (resourceMonitor_ && !resourceMonitor_->checkLimits()) return true;
    return false;
}

void VM::dispatch(Opcode op) {
    // Direct switch dispatch — GCC/Clang generate a jump table for dense
    // switch on uint8_t, so this is equivalent to computed goto without
    // the indirection overhead of the previous broken label→switch_entry path.
    switch (op) {
        case Opcode::OP_NOP:
            break;
            
        case Opcode::OP_CONSTANT: {
            uint8_t constant = readByte();
            push(chunk_->constant(constant));
            break;
        }
        
        case Opcode::OP_CONSTANT_LONG: {
            uint16_t constant = readShort();
            push(chunk_->constant(constant));
            break;
        }
        
        case Opcode::OP_NIL:
            push(Value::null());
            break;
            
        case Opcode::OP_TRUE:
            push(Value::boolean(true));
            break;
            
        case Opcode::OP_FALSE:
            push(Value::boolean(false));
            break;
            
        case Opcode::OP_POP:
            pop();
            break;
            
        case Opcode::OP_DUP:
            push(peek());
            break;
            
        case Opcode::OP_GET_LOCAL: {
            uint16_t slot = readShort();
            push(getLocal(slot));
            break;
        }

        case Opcode::OP_SET_LOCAL: {
            uint16_t slot = readShort();
            setLocal(slot, peek());
            break;
        }

        case Opcode::OP_GET_GLOBAL: {
            push(getGlobal(constantString(readShort())));
            break;
        }

        case Opcode::OP_SET_GLOBAL: {
            globals_[constantString(readShort())] = peek();
            break;
        }

        case Opcode::OP_DEFINE_GLOBAL: {
            globals_[constantString(readShort())] = pop();
            break;
        }

        case Opcode::OP_GET_UPVALUE: {
            uint16_t slot = readShort();
            RuntimeUpvalue* upvalue = currentFrame().function
                ? currentFrame().function->upvalue(slot) : nullptr;
            push(upvalue ? upvalue->get() : Value::undefined());
            break;
        }

        case Opcode::OP_SET_UPVALUE: {
            uint16_t slot = readShort();
            RuntimeUpvalue* upvalue = currentFrame().function
                ? currentFrame().function->upvalue(slot) : nullptr;
            if (upvalue) upvalue->set(peek());
            break;
        }

        case Opcode::OP_GET_PROPERTY: {
            const std::string& name = constantString(readShort());
            Value target = pop();
            push(getProperty(target, name));
            break;
        }

        case Opcode::OP_SET_PROPERTY: {
            const std::string& name = constantString(readShort());
            Value value = pop();
            Value target = pop();
            setProperty(target, name, value);
            push(value);
            break;
        }

        case Opcode::OP_GET_ELEMENT: {
            Value key = pop();
            Value target = pop();
            push(getElement(target, key));
            break;
        }

        case Opcode::OP_SET_ELEMENT: {
            Value value = pop();
            Value key = pop();
            Value target = pop();
            setElement(target, key, value);
            push(value);
            break;
        }

        // Arithmetic
        case Opcode::OP_ADD: {
            Value b = pop();
            Value a = pop();
            toPrimitiveOperands(a, b);
            push(Value::add(a, b));
            break;
        }

        case Opcode::OP_SUBTRACT: {
            Value b = pop();
            Value a = pop();
            toPrimitiveOperands(a, b);
            push(Value::subtract(a, b));
            break;
        }

        case Opcode::OP_MULTIPLY: {
            Value b = pop();
            Value a = pop();
            toPrimitiveOperands(a, b);
            push(Value::multiply(a, b));
            break;
        }

        case Opcode::OP_DIVIDE: {
            Value b = pop();
            Value a = pop();
            toPrimitiveOperands(a, b);
            push(Value::divide(a, b));
            break;
        }

        case Opcode::OP_MODULO: {
            Value b = pop();
            Value a = pop();
            toPrimitiveOperands(a, b);
            push(Value::modulo(a, b));
            break;
        }

        case Opcode::OP_POWER: {
            Value b = pop();
            Value a = pop();
            toPrimitiveOperands(a, b);
            push(Value::number(std::pow(a.toNumber(), b.toNumber())));
            break;
        }

        case Opcode::OP_NEGATE: {
            push(Value::negate(toPrimitive(pop(), false)));
            break;
        }

        // Comparison
        case Opcode::OP_EQUAL: {
            Value b = pop();
            Value a = pop();
            // An object compared loosely with a primitive (other than null
            // and undefined) is compared as its primitive value
            if (a.isObject() != b.isObject() && !isNullish(a) && !isNullish(b)) {
                toPrimitiveOperands(a, b);
            }
            push(Value::boolean(a.equals(b)));
            break;
        }

        case Opcode::OP_STRICT_EQUAL: {
            Value b = pop();
            Value a = pop();
            push(Value::boolean(a.strictEquals(b)));
            break;
        }
        
        case Opcode::OP_NOT_EQUAL: {
            Value b = pop();
            Value a = pop();
            // An object compared loosely with a primitive (other than null
            // and undefined) is compared as its primitive value
            if (a.isObject() != b.isObject() && !isNullish(a) && !isNullish(b)) {
                toPrimitiveOperands(a, b);
            }
            push(Value::boolean(!a.equals(b)));
            break;
        }

        case Opcode::OP_STRICT_NOT_EQUAL: {
            Value b = pop();
            Value a = pop();
            push(Value::boolean(!a.strictEquals(b)));
            break;
        }
        
        case Opcode::OP_LESS: {
            Value b = pop();
            Value a = pop();
            toPrimitiveOperands(a, b);
            push(Value::lessThan(a, b));
            break;
        }

        case Opcode::OP_LESS_EQUAL: {
            Value b = pop();
            Value a = pop();
            toPrimitiveOperands(a, b);
            push(Value::lessEqual(a, b));
            break;
        }

        case Opcode::OP_GREATER: {
            Value b = pop();
            Value a = pop();
            toPrimitiveOperands(a, b);
            push(Value::greaterThan(a, b));
            break;
        }

        case Opcode::OP_GREATER_EQUAL: {
            Value b = pop();
            Value a = pop();
            toPrimitiveOperands(a, b);
            push(Value::greaterEqual(a, b));
            break;
        }

        // Logical
        case Opcode::OP_NOT: {
            Value a = pop();
            push(Value::boolean(a.isFalsy()));
            break;
        }
        
        // Bitwise
        case Opcode::OP_BITWISE_AND: {
            Value b = pop();
            Value a = pop();
            toPrimitiveOperands(a, b);
            push(Value::bitwiseAnd(a, b));
            break;
        }

        case Opcode::OP_BITWISE_OR: {
            Value b = pop();
            Value a = pop();
            toPrimitiveOperands(a, b);
            push(Value::bitwiseOr(a, b));
            break;
        }

        case Opcode::OP_BITWISE_XOR: {
            Value b = pop();
            Value a = pop();
            toPrimitiveOperands(a, b);
            push(Value::bitwiseXor(a, b));
            break;
        }

        case Opcode::OP_BITWISE_NOT: {
            push(Value::bitwiseNot(toPrimitive(pop(), false)));
            break;
        }

        case Opcode::OP_LEFT_SHIFT: {
            Value b = pop();
            Value a = pop();
            toPrimitiveOperands(a, b);
            push(Value::leftShift(a, b));
            break;
        }

        case Opcode::OP_RIGHT_SHIFT: {
            Value b = pop();
            Value a = pop();
            toPrimitiveOperands(a, b);
            push(Value::rightShift(a, b));
            break;
        }

        case Opcode::OP_UNSIGNED_RIGHT_SHIFT: {
            Value b = pop();
            Value a = pop();
            toPrimitiveOperands(a, b);
            push(Value::unsignedRightShift(a, b));
            break;
        }

        // Jumps
        case Opcode::OP_JUMP: {
            uint16_t offset = readShort();
            ip_ += offset;
            break;
        }
        
        case Opcode::OP_JUMP_IF_FALSE: {
            uint16_t offset = readShort();
            if (peek().isFalsy()) {
                ip_ += offset;
            }
            break;
        }
        
        case Opcode::OP_JUMP_IF_NIL: {
            uint16_t offset = readShort();
            if (peek().isNull() || peek().isUndefined()) {
                ip_ += offset;
            }
            break;
        }
        
        case Opcode::OP_LOOP: {
            uint16_t offset = readShort();
            ip_ -= offset;
            break;
        }
        
        // Functions
        case Opcode::OP_CALL: {
            uint8_t argCount = readByte();
            std::vector<Value> args = takeArguments(argCount);
            Value callee = pop();
            invoke(functionOrThrow(callee, describeForError(callee)), Value::undefined(), std::move(args));
            break;
        }

        case Opcode::OP_RETURN: {
            Value result = pop();

            if (callDepth() == 0) {
                // A chunk run without a frame: just stop
                push(result);
                ip_ = chunk_->code().size();
                break;
            }

            VMCallFrame frame = popCallFrame();
            // Close upvalues over this frame's locals before dropping them
            if (stack_.size() > frame.slotBase) {
                closeUpvalues(stack_.data() + frame.slotBase);
                stack_.erase(stack_.begin() + static_cast<std::ptrdiff_t>(frame.slotBase), stack_.end());
            }
            // Handlers installed by the returning function go with it
            while (!exceptionHandlers_.empty() &&
                   exceptionHandlers_.back().callStackLevel > callDepth()) {
                exceptionHandlers_.pop_back();
            }

            // 'new' and super(): a non-object result means the new object
            if (frame.isConstruct && !result.isObject()) {
                result = frame.thisValue;
            }
            push(result);

            if (frame.returnAddress == SIZE_MAX) {
                // Boundary of a script, callback or generator: leave this run()
                ip_ = chunk_->code().size();
            } else {
                chunk_ = frame.savedChunk;
                ip_ = frame.returnAddress;
            }
            break;
        }

        case Opcode::OP_CALL_METHOD: {
            // [receiver, args...]; operands: method name (u16), arg count (u8)
            const std::string& methodName = constantString(readShort());
            uint8_t argCount = readByte();
            std::vector<Value> args = takeArguments(argCount);
            Value receiver = pop();

            Value method = getProperty(receiver, methodName);
            if (!method.isObject() || !method.asObject()->isFunction()) {
                throw std::runtime_error(describeForError(receiver) + "." + methodName +
                                         " is not a function");
            }
            invoke(static_cast<Function*>(method.asObject()), receiver, std::move(args));
            break;
        }

        case Opcode::OP_CLOSURE: {
            // Each evaluation makes a new closure with its own upvalues
            uint16_t funcIdx = readShort();
            uint16_t upvalueCount = readShort();
            auto* fnTemplate = static_cast<Function*>(chunk_->constant(funcIdx).asObject());
            Function* closure = fnTemplate->instantiateClosure();

            VMCallFrame& frame = currentFrame();
            for (uint16_t i = 0; i < upvalueCount; i++) {
                uint8_t isLocal = readByte();
                uint16_t index = readShort();
                RuntimeUpvalue* upvalue = nullptr;
                if (isLocal) {
                    upvalue = captureUpvalue(&stack_[frame.slotBase + index]);
                } else if (frame.function) {
                    upvalue = frame.function->upvalue(index);
                }
                closure->addUpvalue(upvalue);
            }
            push(Value::object(closure));
            break;
        }

        case Opcode::OP_CLOSE_UPVALUE: {
            // Close upvalue at top of stack
            if (!stack_.empty()) {
                closeUpvalues(&stack_.back());
            }
            pop();
            break;
        }
        
        case Opcode::OP_AWAIT: {
            // await expression - unwrap Promise/thenable
            Value awaited = pop();
            
            // Convert to Promise if needed
            Promise* promise = AwaitHandler::toPromise(awaited);
            
            if (promise->state() == PromiseState::Fulfilled) {
                // Already resolved - push result and continue
                push(promise->result());
            } else if (promise->state() == PromiseState::Rejected) {
                // Rejected - throw error
                throw std::runtime_error("Promise rejected: " + promise->result().toString());
            } else {
                // Pending — schedule microtask to resume when settled
                // In synchronous execution mode, drain the microtask queue
                // to allow the promise to resolve
                MicrotaskQueue::instance().process();
                
                // Re-check after draining
                if (promise->state() == PromiseState::Fulfilled) {
                    push(promise->result());
                } else if (promise->state() == PromiseState::Rejected) {
                    throw std::runtime_error("Promise rejected: " + promise->result().toString());
                } else {
                    // Still pending after drain — push undefined (no event loop)
                    push(Value::undefined());
                }
            }
            break;
        }
        
        case Opcode::OP_YIELD: {
            // yield expression in generator - suspend execution
            Value yielded = pop();
            uint8_t delegate = readByte();  // 1 = yield*, 0 = yield
            
            if (currentGenerator_) {
                // Save current state for resumption
                currentGenerator_->suspendedIP = ip_;
                currentGenerator_->savedStack.clear();
                size_t base = currentGenerator_->stackBase;
                for (size_t i = base; i < stack_.size(); i++) {
                    currentGenerator_->savedStack.push_back(stack_[i]);
                }
                
                // Store yielded value and set suspension flag
                yieldedValue_ = yielded;
                isYielding_ = true;
                
                // Exit run loop (will be resumed by next() call)
                ip_ = chunk_->code().size();  // Force exit from run()
                
                if (delegate) {
                    // yield* — yieldedValue_ already holds the inner iterator.
                    // The generator's next() method detects this as an iterable
                    // and drains it before resuming the outer generator.
                }
            } else {
                // Not in generator context - just push value back (fallback)
                push(yielded);
            }
            break;
        }
            
        // Object creation
        case Opcode::OP_CREATE_ARRAY: {
            uint16_t count = readShort();
            push(Value::object(new Array(takeArguments(count))));
            break;
        }

        case Opcode::OP_CREATE_OBJECT: {
            Object* obj = new Object();
            obj->setPrototype(intrinsic(Intrinsic::Object));
            push(Value::object(obj));
            break;
        }

        case Opcode::OP_INIT_PROPERTY: {
            // Object literal entry: defines an own property, no setters run
            Value value = pop();
            Value key = pop();
            Value obj = peek();
            if (obj.isObject()) {
                uint32_t index;
                if (toArrayIndex(key, index)) {
                    obj.asObject()->set(index, value);
                } else {
                    obj.asObject()->set(propertyKeyName(key), value);
                }
            }
            break;
        }

        case Opcode::OP_GET_ITERATOR: {
            Value iterable = pop();
            push(makeIterator(iterable));
            break;
        }

        case Opcode::OP_ITERATOR_NEXT: {
            uint16_t exitOffset = readShort();
            Value iterator = pop();
            Value value;
            if (iteratorStep(iterator, value)) {
                push(value);
            } else {
                ip_ += exitOffset;
            }
            break;
        }

        case Opcode::OP_SPREAD: {
            // [array, iterable] -> [array]: appends every item
            Value iterable = pop();
            auto* target = dynamic_cast<Array*>(peek().asObject());
            Value iterator = makeIterator(iterable);
            Value value;
            while (iteratorStep(iterator, value)) {
                if (target) target->push(value);
            }
            break;
        }

        case Opcode::OP_NEW: {
            uint8_t argCount = readByte();
            std::vector<Value> args = takeArguments(argCount);
            Value callee = pop();
            constructValue(callee, std::move(args));
            break;
        }

        // Type operations
        case Opcode::OP_TYPEOF: {
            Value a = pop();
            std::string type;
            if (a.isUndefined()) type = "undefined";
            else if (a.isNull()) type = "object";
            else if (a.isBoolean()) type = "boolean";
            else if (a.isNumber()) type = "number";
            else if (a.isString()) type = "string";
            else if (a.isObject() && dynamic_cast<Function*>(a.asObject())) type = "function";
            else type = "object";
            push(Value::string(new String(type)));
            break;
        }
        
        case Opcode::OP_INSTANCEOF: {
            Value constructorVal = pop();
            Value instance = pop();

            if (!constructorVal.isObject() || !constructorVal.asObject()->isFunction()) {
                throw std::runtime_error("Right-hand side of 'instanceof' is not callable");
            }
            Function* constructor = static_cast<Function*>(constructorVal.asObject());
            while (constructor->isBound() && constructor->boundTarget()) {
                constructor = constructor->boundTarget();
            }

            Object* prototype = ensurePrototypeObject(constructor);
            if (!prototype) {
                throw std::runtime_error("Function has non-object prototype in instanceof check");
            }
            bool found = false;
            if (instance.isObject()) {
                for (Object* obj = instance.asObject()->prototype(); obj; obj = obj->prototype()) {
                    if (obj == prototype) {
                        found = true;
                        break;
                    }
                }
            }
            push(Value::boolean(found));
            break;
        }

        // Exceptions
        case Opcode::OP_THROW: {
            // Pop the exception value
            exceptionValue_ = pop();
            hasException_ = true;
            
            // Error objects record their stack when they are created; one
            // that was built without it gets the stack of the throw
            if (exceptionValue_.isObject() && exceptionValue_.asObject()->objectType() == ObjectType::Error) {
                Object* errObj = exceptionValue_.asObject();
                if (!errObj->getOwnPropertyDescriptor("stack")) {
                    Value nameVal = errObj->get("name");
                    Value msgVal = errObj->get("message");
                    std::string header = nameVal.isString() ? nameVal.toString() : "Error";
                    if (msgVal.isString() && !msgVal.toString().empty()) header += ": " + msgVal.toString();
                    PropertyDescriptor stack;
                    stack.value = Value::string(new String(header + "\n" + captureStackTrace()));
                    stack.attributes = PropertyAttribute::Writable | PropertyAttribute::Configurable;
                    errObj->defineProperty("stack", stack);
                }
            }

            throwValue(exceptionValue_);
            break;
        }
            
        case Opcode::OP_TRY_BEGIN: {
            uint16_t catchOffset = readShort();
            ExceptionHandler handler;
            handler.catchAddress = ip_ + catchOffset;
            handler.stackLevel = stack_.size();
            handler.callStackLevel = callDepth();
            handler.chunk = chunk_;
            handler.runDepth = runDepth_;
            exceptionHandlers_.push_back(handler);
            break;
        }

        case Opcode::OP_TRY_END:
            // End of try block without exception - pop handler
            if (!exceptionHandlers_.empty()) {
                exceptionHandlers_.pop_back();
            }
            break;
            
        case Opcode::OP_CATCH:
            // Push exception value onto stack for catch block
            push(exceptionValue_);
            hasException_ = false;
            exceptionValue_ = Value::undefined();
            break;
            
        case Opcode::OP_FINALLY:
            // Finally block - just continues execution
            // Exception state preserved if re-throwing
            break;
        
        case Opcode::OP_IMPORT: {
            // Read module path constant index
            uint16_t pathIdx = readShort();
            Value pathValue = chunk_->constant(pathIdx);
            if (!pathValue.isString()) {
                throw std::runtime_error("Invalid module path");
            }
            std::string modulePath = static_cast<String*>(pathValue.asObject())->value();
            
            // Use ModuleLoader if available
            if (moduleLoader_) {
                Value exports = moduleLoader_->loadModule(modulePath, currentModulePath_);
                push(exports);
            } else {
                // Fallback: create empty exports object
                Object* exports = new Object();
                push(Value::object(exports));
            }
            break;
        }
        
        case Opcode::OP_EXPORT: {
            // Read export name constant index
            uint16_t nameIdx = readShort();
            Value nameValue = chunk_->constant(nameIdx);
            Value exportValue = pop();
            
            // Store export value in global scope (module exports are accessible as globals)
            if (nameValue.isString()) {
                std::string name = static_cast<String*>(nameValue.asObject())->value();
                setGlobal(name, exportValue);
            }
            break;
        }
        
        case Opcode::OP_IMPORT_BINDING: {
            // Read binding name constant index  
            readShort(); // consume binding name operand
            break;
        }
        case Opcode::OP_DEBUGGER: {
            // Debugger statement — pauses execution if debug callback is set
            break;
        }
        // Class operations
        case Opcode::OP_INHERIT: {
            // [subclass, superclass] -> [subclass]
            Value superclass = pop();
            Value subclass = peek(0);
            if (!superclass.isObject() || !superclass.asObject()->isFunction()) {
                throw std::runtime_error("Class extends value " + describeForError(superclass) +
                                         " is not a constructor");
            }
            auto* sub = static_cast<Function*>(subclass.asObject());
            auto* super = static_cast<Function*>(superclass.asObject());
            // Instances inherit the parent's methods; the class its statics
            Object* subProto = ensurePrototypeObject(sub);
            Object* superProto = ensurePrototypeObject(super);
            if (subProto && superProto) subProto->setPrototype(superProto);
            sub->setPrototype(super);
            break;
        }

        case Opcode::OP_DEFINE_METHOD:
        case Opcode::OP_DEFINE_STATIC:
        case Opcode::OP_DEFINE_GETTER:
        case Opcode::OP_DEFINE_SETTER: {
            uint16_t nameIdx = readShort();
            Value methodName = chunk_->constant(nameIdx);
            Value methodFunc = pop();
            Value classConstructor = peek(0);
            
            if (!methodName.isString() || !methodFunc.isObject() || !classConstructor.isObject()) {
                throw std::runtime_error("Invalid operands for define method/getter/setter");
            }
            
            String* nameStr = static_cast<String*>(methodName.asObject());
            Object* targetObj = classConstructor.asObject();
            
            if (op != Opcode::OP_DEFINE_STATIC && targetObj->isFunction()) {
                // Add to prototype instead of constructor directly
                if (Object* proto = ensurePrototypeObject(static_cast<Function*>(targetObj))) {
                    targetObj = proto;
                }
            }
            
            if (op == Opcode::OP_DEFINE_GETTER) {
                PropertyDescriptor desc;
                desc.getter = methodFunc;
                desc.attributes = PropertyAttribute::Configurable | PropertyAttribute::Enumerable;
                targetObj->defineProperty(nameStr->value(), desc);
            } else if (op == Opcode::OP_DEFINE_SETTER) {
                PropertyDescriptor desc;
                desc.setter = methodFunc;
                desc.attributes = PropertyAttribute::Configurable | PropertyAttribute::Enumerable;
                targetObj->defineProperty(nameStr->value(), desc);
            } else {
                targetObj->set(nameStr->value(), methodFunc);
            }
            break;
        }
        case Opcode::OP_SUPER_CALL: {
            // [args..., this, superclass]
            uint8_t argCount = readByte();
            Value superclass = pop();
            Value thisValue = pop();
            superConstruct(superclass, thisValue, takeArguments(argCount));
            break;
        }

        case Opcode::OP_FOR_IN: {
            Value obj = pop();
            if (obj.isObject()) {
                std::vector<std::string> keysStr = obj.asObject()->keys();
                std::vector<Value> keysVal;
                keysVal.reserve(keysStr.size());
                for (const auto& k : keysStr) {
                    keysVal.push_back(Value::string(new String(k)));
                }
                Array* keysArray = new Array(keysVal);
                push(Value::object(keysArray));
            } else {
                push(Value::object(new Array()));
            }
            break;
        }

        // Stack manipulation
        case Opcode::OP_SWAP: {
            Value a = pop();
            Value b = pop();
            push(a);
            push(b);
            break;
        }

        case Opcode::OP_DUP2: {         // [a, b] -> [a, b, a, b]
            Value b = peek(0);
            Value a = peek(1);
            push(a);
            push(b);
            break;
        }

        case Opcode::OP_DUP_X1: {       // [a, b] -> [b, a, b]
            Value b = pop();
            Value a = pop();
            push(b);
            push(a);
            push(b);
            break;
        }

        case Opcode::OP_DUP_X2: {       // [a, b, c] -> [c, a, b, c]
            Value c = pop();
            Value b = pop();
            Value a = pop();
            push(c);
            push(a);
            push(b);
            push(c);
            break;
        }

        case Opcode::OP_PICK: {
            uint8_t depth = readByte();
            push(peek(depth));
            break;
        }

        case Opcode::OP_TO_NUMBER:
            push(Value::number(toPrimitive(pop(), false).toNumber()));
            break;

        case Opcode::OP_DELETE_ELEMENT: {
            Value key = pop();
            Value target = pop();
            push(Value::boolean(deleteProperty(target, key)));
            break;
        }

        case Opcode::OP_CALL_WITH_THIS: {
            // [this, fn, args...]
            uint8_t argCount = readByte();
            std::vector<Value> args = takeArguments(argCount);
            Value callee = pop();
            Value thisValue = pop();
            invoke(functionOrThrow(callee, describeForError(callee)), thisValue, std::move(args));
            break;
        }

        case Opcode::OP_CALL_SPREAD: {
            // [this, fn, argsArray]
            Value argsArray = pop();
            Value callee = pop();
            Value thisValue = pop();
            invoke(functionOrThrow(callee, describeForError(callee)), thisValue, arrayElements(argsArray));
            break;
        }

        case Opcode::OP_NEW_SPREAD: {
            // [ctor, argsArray]
            Value argsArray = pop();
            Value callee = pop();
            constructValue(callee, arrayElements(argsArray));
            break;
        }

        case Opcode::OP_SUPER_APPLY: {
            // [argsArray, this, superclass]
            Value superclass = pop();
            Value thisValue = pop();
            Value argsArray = pop();
            superConstruct(superclass, thisValue, arrayElements(argsArray));
            break;
        }

        case Opcode::OP_OBJECT_REST: {
            // [source, key1..keyn] -> [copy of source's own enumerable
            // properties without those keys]
            uint8_t keyCount = readByte();
            std::vector<Value> keys = takeArguments(keyCount);
            Value source = pop();
            if (source.isNull() || source.isUndefined()) {
                throw std::runtime_error("Cannot destructure " + describeForError(source));
            }
            Object* rest = new Object();
            rest->setPrototype(intrinsic(Intrinsic::Object));
            if (source.isObject()) {
                std::unordered_set<std::string> excluded;
                for (const auto& key : keys) excluded.insert(propertyKeyName(key));
                for (const auto& name : source.asObject()->keys()) {
                    if (excluded.count(name)) continue;
                    Value key = Value::string(new String(name));
                    setElement(Value::object(rest), key, getElement(source, key));
                }
            }
            push(Value::object(rest));
            break;
        }

        case Opcode::OP_GET_CALLEE: {
            Function* callee = currentFrame().function;
            push(callee ? Value::object(callee) : Value::undefined());
            break;
        }

        // Arithmetic increment/decrement
        case Opcode::OP_INCREMENT: {
            Value val = pop();
            if (!val.isNumber()) {
                throw std::runtime_error("Cannot increment non-number value");
            }
            push(Value::number(val.asNumber() + 1.0));
            break;
        }
        case Opcode::OP_DECREMENT: {
            Value val = pop();
            if (!val.isNumber()) {
                throw std::runtime_error("Cannot decrement non-number value");
            }
            push(Value::number(val.asNumber() - 1.0));
            break;
        }

        // Conditional jump (truthy)
        case Opcode::OP_JUMP_IF_TRUE: {
            uint16_t offset = readShort();
            if (peek().isTruthy()) {
                ip_ += offset;
            }
            break;
        }

        // Property existence: `key in object`
        case Opcode::OP_IN: {
            Value obj = pop();
            Value key = pop();
            if (!obj.isObject()) {
                throw std::runtime_error("Cannot use 'in' operator to search for '" +
                                         key.toString() + "' in " + describeForError(obj));
            }
            Object* target = obj.asObject();
            uint32_t index;
            bool found;
            if (toArrayIndex(key, index)) {
                found = target->has(index);
            } else {
                std::string name = propertyKeyName(key);
                found = target->has(name) || (target->isArray() && name == "length");
            }
            push(Value::boolean(found));
            break;
        }

        // Property deletion: `delete obj.prop`
        case Opcode::OP_DELETE_PROPERTY: {
            const std::string& name = constantString(readShort());
            Value target = pop();
            push(Value::boolean(deleteProperty(target, Value::string(new String(name)))));
            break;
        }

        // Array initializer element
        case Opcode::OP_INIT_ELEMENT: {
            Value element = pop();
            Value arrayVal = peek(0);
            if (arrayVal.isObject()) {
                if (auto* arr = dynamic_cast<Array*>(arrayVal.asObject())) {
                    arr->push(element);
                }
            }
            break;
        }

        // for-of iterator creation
        case Opcode::OP_FOR_OF: {
            Value iterable = pop();
            if (iterable.isObject()) {
                Object* obj = iterable.asObject();
                if (auto* arr = dynamic_cast<Array*>(obj)) {
                    // Array: create a copy of values for iteration
                    std::vector<Value> elements;
                    elements.reserve(arr->length());
                    for (size_t i = 0; i < arr->length(); i++) {
                        elements.push_back(arr->get(i));
                    }
                    push(Value::object(new Array(elements)));
                } else {
                    // Generic iterable: collect own enumerable values
                    std::vector<std::string> keyNames = obj->keys();
                    std::vector<Value> values;
                    values.reserve(keyNames.size());
                    for (const auto& k : keyNames) {
                        values.push_back(obj->get(k));
                    }
                    push(Value::object(new Array(values)));
                }
            } else if (iterable.isString()) {
                // String: iterate characters
                std::string s = iterable.toString();
                std::vector<Value> chars;
                chars.reserve(s.size());
                for (size_t i = 0; i < s.size(); i++) {
                    chars.push_back(Value::string(new String(std::string(1, s[i]))));
                }
                push(Value::object(new Array(chars)));
            } else {
                throw std::runtime_error("Value is not iterable");
            }
            break;
        }

        // Switch statement dispatch
        case Opcode::OP_SWITCH: {
            // OP_SWITCH is a marker; actual dispatch uses OP_CASE comparisons
            // The switch discriminant is already on the stack
            break;
        }
        case Opcode::OP_CASE: {
            // Compare TOS-1 (discriminant) with TOS (case value)
            // If equal, pop case value and jump; otherwise pop case value and continue
            uint16_t offset = readShort();
            Value caseValue = pop();
            Value discriminant = peek(0);
            if (discriminant.strictEquals(caseValue)) {
                pop(); // Pop discriminant — match found
                ip_ += offset;
            }
            break;
        }

        // Fast-path constant opcodes
        case Opcode::OP_ZERO:
            push(Value::number(0));
            break;

        case Opcode::OP_ONE:
            push(Value::number(1));
            break;

        // Short-circuit logical operators
        case Opcode::OP_AND: {
            uint16_t offset = readShort();
            Value lhs = peek(0);
            if (lhs.isFalsy()) {
                // LHS is falsy — short-circuit, skip RHS
                ip_ += offset;
            } else {
                // LHS is truthy — discard it, evaluate RHS
                pop();
            }
            break;
        }

        case Opcode::OP_OR: {
            uint16_t offset = readShort();
            Value lhs = peek(0);
            if (!lhs.isFalsy()) {
                // LHS is truthy — short-circuit, skip RHS
                ip_ += offset;
            } else {
                // LHS is falsy — discard it, evaluate RHS
                pop();
            }
            break;
        }

        case Opcode::OP_NULLISH: {
            uint16_t offset = readShort();
            Value lhs = peek(0);
            if (!lhs.isNull() && !lhs.isUndefined()) {
                // LHS is not nullish — short-circuit, skip RHS
                ip_ += offset;
            } else {
                // LHS is null/undefined — discard it, evaluate RHS
                pop();
            }
            break;
        }

        // Super property access: super.prop
        case Opcode::OP_SUPER_GET: {
            uint16_t nameIndex = readShort();
            Value nameVal = chunk_->constant(nameIndex);
            std::string propName = nameVal.toString();

            // 'this' is at stack slot 0 in method context
            Value thisVal = getLocal(0);
            if (!thisVal.isObject()) {
                throw std::runtime_error("'super' used in non-object context");
            }

            Object* thisObj = thisVal.asObject();
            Object* proto = thisObj->prototype();
            if (proto) {
                // Get from super prototype (one level up)
                Object* superProto = proto->prototype();
                if (superProto) {
                    push(superProto->get(propName));
                } else {
                    push(Value::undefined());
                }
            } else {
                push(Value::undefined());
            }
            break;
        }

        // Debug line tracking
        case Opcode::OP_LINE: {
            uint16_t line = readShort();
            currentLine_ = line;
            break;
        }

        // End of bytecode
        case Opcode::OP_END: {
            // Halt execution — sentinel opcode
            ip_ = chunk_->code().size();
            break;
        }

        default:
            throw std::runtime_error(
                "Unknown opcode: 0x" + 
                std::string(1, "0123456789ABCDEF"[static_cast<uint8_t>(op) >> 4]) +
                std::string(1, "0123456789ABCDEF"[static_cast<uint8_t>(op) & 0xF])
            );
            break;
    }
}

// Stack operations
void VM::push(Value value) {
    if (__builtin_expect(stack_.size() >= ZEPRA_MAX_CALL_STACK_DEPTH * 256, 0)) {
        throw std::runtime_error("Stack overflow");
    }
    stack_.push_back(value);
}

Value VM::pop() {
    if (__builtin_expect(stack_.empty(), 0)) {
        return Value::undefined();
    }
    Value v = stack_.back();
    stack_.pop_back();
    return v;
}

Value VM::peek(size_t distance) const {
    if (__builtin_expect(distance >= stack_.size(), 0)) {
        return Value::undefined();
    }
    return stack_[stack_.size() - 1 - distance];
}

void VM::popN(size_t count) {
    while (count-- > 0 && !stack_.empty()) {
        stack_.pop_back();
    }
}

// Helper read methods — use direct vector access
uint8_t VM::readByte() {
    return chunk_->code()[ip_++];
}

uint16_t VM::readShort() {
    const auto& code = chunk_->code();
    uint8_t high = code[ip_++];
    uint8_t low = code[ip_++];
    return (high << 8) | low;
}

Value VM::readConstant() {
    return chunk_->constant(readByte());
}

// Local variable access
Value VM::getLocal(size_t slot) {
    if (callDepth() > 0) {  // HYBRID
        size_t base = currentFrame().slotBase;
        return stack_[base + slot];
    }
    return slot < stack_.size() ? stack_[slot] : Value::undefined();
}

void VM::setLocal(size_t slot, Value value) {
    if (callDepth() > 0) {  // HYBRID
        size_t base = currentFrame().slotBase;
        if (base + slot < stack_.size()) {
            stack_[base + slot] = value;
        }
    } else if (slot < stack_.size()) {
        stack_[slot] = value;
    }
}

// Function calls
Value VM::call(Function* function, Value thisValue, const std::vector<Value>& args) {
    if (!function) {
        return Value::undefined();
    }
    // A host call from outside any script (timer, animation frame) starts a
    // fresh run, like execute(): a previous run's termination does not apply
    if (callDepth() == 0) terminationRequested_ = false;

    return executeCallback(function, thisValue, args);
}

// Runs fn to completion and returns its result, for builtins (array
// callbacks, accessors, the iterator protocol) and the host. Compiled
// functions run in a nested run(); an exception reaches the caller with the
// VM state restored.
Value VM::executeCallback(Function* fn, Value thisValue, const std::vector<Value>& args) {
    if (!fn) {
        return Value::undefined();
    }

    std::vector<Value> callArgs = args;
    while (fn->isBound() && fn->boundTarget()) {
        std::vector<Value> combined = fn->boundArgs();
        combined.insert(combined.end(), callArgs.begin(), callArgs.end());
        callArgs = std::move(combined);
        thisValue = fn->boundThis();
        fn = fn->boundTarget();
    }
    if (fn->isBuiltin()) {
        FunctionCallInfo info(context_, thisValue, callArgs);
        return fn->builtinFunction()(info);
    }
    if (fn->isNative()) {
        return fn->nativeFunction()(context_, callArgs);
    }
    if (!fn->isCompiled()) {
        throw std::runtime_error("Function '" + fn->name() + "' has no compiled body");
    }
    if (fn->isGenerator()) {
        return makeGenerator(fn, thisValue, callArgs);
    }

    VM* previousVM = currentVM_;
    currentVM_ = this;
    const Bytecode::BytecodeChunk* savedChunk = chunk_;
    const size_t savedIP = ip_;
    const size_t savedDepth = callDepth();
    const size_t savedStack = stack_.size();
    const size_t savedHandlers = exceptionHandlers_.size();

    Value result;
    try {
        enterFrame(fn, thisValue, callArgs, false, SIZE_MAX);
        run();
        if (callDepth() > savedDepth) {
            throw std::runtime_error("Script execution was terminated");
        }
        result = pop();
    } catch (...) {
        unwindTo(savedDepth, savedStack, savedHandlers);
        chunk_ = savedChunk;
        ip_ = savedIP;
        currentVM_ = previousVM;
        throw;
    }
    chunk_ = savedChunk;
    ip_ = savedIP;
    currentVM_ = previousVM;
    return result;
}

Value VM::construct(Function* constructor, const std::vector<Value>& args) {
    if (!constructor) {
        return Value::undefined();
    }
    std::vector<Value> callArgs = args;
    while (constructor->isBound() && constructor->boundTarget()) {
        std::vector<Value> combined = constructor->boundArgs();
        combined.insert(combined.end(), callArgs.begin(), callArgs.end());
        callArgs = std::move(combined);
        constructor = constructor->boundTarget();
    }
    if (!constructor->isConstructor()) {
        throw std::runtime_error(constructor->name() + " is not a constructor");
    }

    Object* instance = new Object();
    Object* proto = ensurePrototypeObject(constructor);
    instance->setPrototype(proto ? proto : intrinsic(Intrinsic::Object));

    Value result = executeCallback(constructor, Value::object(instance), callArgs);
    return result.isObject() ? result : Value::object(instance);
}

// Calls fn from the interpreter loop: native and builtin functions run now
// and push their result; a compiled function gets a frame, and the loop
// continues in its bytecode.
void VM::invoke(Function* fn, Value thisValue, std::vector<Value> args) {
    while (fn->isBound() && fn->boundTarget()) {
        std::vector<Value> combined = fn->boundArgs();
        combined.insert(combined.end(), args.begin(), args.end());
        args = std::move(combined);
        thisValue = fn->boundThis();
        fn = fn->boundTarget();
    }
    if (fn->isBuiltin()) {
        FunctionCallInfo info(context_, thisValue, args);
        push(fn->builtinFunction()(info));
        return;
    }
    if (fn->isNative()) {
        push(fn->nativeFunction()(context_, args));
        return;
    }
    if (!fn->isCompiled()) {
        throw std::runtime_error("Function '" + fn->name() + "' has no compiled body");
    }
    if (fn->isGenerator()) {
        push(makeGenerator(fn, thisValue, args));
        return;
    }
    jitProfiler_.recordCall(reinterpret_cast<uintptr_t>(fn));
    enterFrame(fn, thisValue, args, false, ip_);
}

void VM::constructValue(const Value& callee, std::vector<Value> args) {
    if (!callee.isObject() || !callee.asObject()->isFunction()) {
        throw std::runtime_error(describeForError(callee) + " is not a constructor");
    }
    auto* ctor = static_cast<Function*>(callee.asObject());
    while (ctor->isBound() && ctor->boundTarget()) {
        std::vector<Value> combined = ctor->boundArgs();
        combined.insert(combined.end(), args.begin(), args.end());
        args = std::move(combined);
        ctor = ctor->boundTarget();
    }
    if (!ctor->isConstructor()) {
        throw std::runtime_error((ctor->name().empty() ? "function" : ctor->name()) +
                                 " is not a constructor");
    }

    Object* instance = new Object();
    Object* proto = ensurePrototypeObject(ctor);
    instance->setPrototype(proto ? proto : intrinsic(Intrinsic::Object));

    if (ctor->isBuiltin()) {
        FunctionCallInfo info(context_, Value::object(instance), args);
        Value result = ctor->builtinFunction()(info);
        push(result.isObject() ? result : Value::object(instance));
        return;
    }
    if (ctor->isNative()) {
        Value result = ctor->nativeFunction()(context_, args);
        push(result.isObject() ? result : Value::object(instance));
        return;
    }
    if (!ctor->isCompiled()) {
        throw std::runtime_error("Function '" + ctor->name() + "' has no compiled body");
    }
    enterFrame(ctor, Value::object(instance), args, true, ip_);
}

// super(...) in a class constructor: runs the parent constructor on the
// object being built.
void VM::superConstruct(const Value& superclass, const Value& thisValue, std::vector<Value> args) {
    if (!superclass.isObject() || !superclass.asObject()->isFunction()) {
        throw std::runtime_error("Super constructor " + describeForError(superclass) +
                                 " is not a constructor");
    }
    auto* parent = static_cast<Function*>(superclass.asObject());
    while (parent->isBound() && parent->boundTarget()) {
        std::vector<Value> combined = parent->boundArgs();
        combined.insert(combined.end(), args.begin(), args.end());
        args = std::move(combined);
        parent = parent->boundTarget();
    }
    if (parent->isBuiltin()) {
        FunctionCallInfo info(context_, thisValue, args);
        push(parent->builtinFunction()(info));
        return;
    }
    if (parent->isNative()) {
        push(parent->nativeFunction()(context_, args));
        return;
    }
    if (!parent->isCompiled()) {
        throw std::runtime_error("Function '" + parent->name() + "' has no compiled body");
    }
    enterFrame(parent, thisValue, args, true, ip_);
}

void VM::enterFrame(Function* fn, Value thisValue, const std::vector<Value>& args,
                    bool isConstruct, size_t returnAddress) {
    VMCallFrame frame;
    frame.function = fn;
    frame.returnAddress = returnAddress;
    frame.slotBase = stack_.size();
    frame.thisValue = thisValue;
    frame.isConstruct = isConstruct;
    frame.savedChunk = chunk_;
    pushCallFrame(frame);

    push(thisValue);  // Slot 0
    pushArguments(fn, args);
    chunk_ = fn->bytecodeChunk();
    ip_ = 0;
}

// Lays the arguments out in the callee's parameter slots: missing ones are
// undefined, extras are dropped (they stay reachable through `arguments`),
// and a rest parameter gets an array of the remainder.
void VM::pushArguments(Function* fn, const std::vector<Value>& args) {
    const Function::FrameLayout* layout = fn->frameLayout();
    if (!layout) {
        for (const auto& arg : args) push(arg);
        return;
    }
    const size_t fixed = layout->parameterSlots - (layout->hasRest ? 1 : 0);
    for (size_t i = 0; i < fixed; i++) {
        push(i < args.size() ? args[i] : Value::undefined());
    }
    if (layout->hasRest) {
        std::vector<Value> rest;
        if (args.size() > fixed) rest.assign(args.begin() + static_cast<std::ptrdiff_t>(fixed), args.end());
        push(Value::object(new Array(std::move(rest))));
    }
    if (layout->hasArgumentsSlot) {
        push(layout->usesArguments ? Value::object(new Array(args)) : Value::undefined());
    }
}

// Removes the top `count` values, returned in stack order.
std::vector<Value> VM::takeArguments(size_t count) {
    count = std::min(count, stack_.size());
    std::vector<Value> args(stack_.end() - static_cast<std::ptrdiff_t>(count), stack_.end());
    stack_.erase(stack_.end() - static_cast<std::ptrdiff_t>(count), stack_.end());
    return args;
}

std::vector<Value> VM::arrayElements(const Value& array) {
    if (array.isObject()) {
        if (auto* arr = dynamic_cast<Array*>(array.asObject())) {
            return arr->elements();
        }
    }
    return {};
}

Function* VM::functionOrThrow(const Value& callee, const std::string& what) {
    if (callee.isObject() && callee.asObject()->isFunction()) {
        return static_cast<Function*>(callee.asObject());
    }
    throw std::runtime_error(what + " is not a function");
}

Value VM::makeGenerator(Function* fn, Value thisValue, const std::vector<Value>& args) {
    auto* generator = new Builtins::GeneratorObject(fn, thisValue, args);
    Value ctor = getGlobal("Generator");
    Value proto = ctor.isObject() ? ctor.asObject()->get("prototype") : Value::undefined();
    if (proto.isObject()) {
        generator->setPrototype(proto.asObject());
    } else {
        static Object* generatorProto = Builtins::GeneratorBuiltin::createGeneratorPrototype();
        generator->setPrototype(generatorProto);
    }
    return Value::object(generator);
}

// =============================================================================
// Properties
// =============================================================================

const std::string& VM::constantString(size_t index) const {
    static const std::string empty;
    Value value = chunk_->constant(index);
    return value.isString() ? static_cast<String*>(value.asObject())->value() : empty;
}

Object* VM::primitivePrototype(const Value& value) {
    if (value.isString()) return intrinsic(Intrinsic::String);
    if (value.isNumber()) return intrinsic(Intrinsic::Number);
    if (value.isBoolean()) return intrinsic(Intrinsic::Boolean);
    if (value.isSymbol()) return intrinsic(Intrinsic::Symbol);
    return nullptr;
}

Object* VM::intrinsic(Intrinsic kind) {
    Object*& slot = intrinsics_[static_cast<size_t>(kind)];
    if (slot) return slot;

    switch (kind) {
        case Intrinsic::Object:
            slot = Builtins::ObjectBuiltin::createObjectPrototype(nullptr);
            return slot;
        case Intrinsic::Function:
            slot = createFunctionPrototype();
            return slot;
        case Intrinsic::Array:
            slot = Builtins::ArrayBuiltin::createArrayPrototype(nullptr);
            break;
        case Intrinsic::String:
            slot = Builtins::StringBuiltin::createStringPrototype(nullptr);
            break;
        case Intrinsic::Number:
            slot = Builtins::NumberBuiltin::createNumberPrototype();
            break;
        case Intrinsic::Boolean:
            slot = Builtins::BooleanBuiltin::createBooleanPrototype();
            break;
        case Intrinsic::Symbol:
            slot = new Object();
            break;
        case Intrinsic::Error:
            slot = Builtins::ErrorBuiltin::createErrorPrototype();
            break;
        case Intrinsic::Iterator:
            slot = Builtins::IteratorBuiltin::createIteratorPrototype();
            break;
        case Intrinsic::Count:
            return nullptr;
    }
    // Everything but Object.prototype itself inherits from it
    slot->setPrototype(intrinsic(Intrinsic::Object));
    return slot;
}

Object* VM::createFunctionPrototype() {
    auto* proto = new Object();
    proto->setPrototype(intrinsic(Intrinsic::Object));
    populateFunctionPrototype(proto);
    return proto;
}

std::string VM::captureStackTrace() const {
    std::string trace;
    auto appendFrame = [&trace](const VMCallFrame& frame) {
        trace += "    at ";
        if (frame.function) {
            trace += frame.function->name().empty() ? "<anonymous>" : frame.function->name();
        } else {
            trace += "<global>";
        }
        trace += " (bytecode:" + std::to_string(frame.returnAddress) + ")\n";
    };
    for (size_t i = heapDepth_; i-- > 0;) appendFrame(heapStack_[i]);
    for (size_t i = nativeDepth_; i-- > 0;) appendFrame(nativeStack_[i]);
    return trace;
}

// A function's `prototype` object, created on first use: most functions are
// never constructors, so building one per closure would waste an object.
Object* VM::ensurePrototypeObject(Function* fn) {
    if (!fn->isCompiled()) {
        // Built-in constructors bring their own prototype
        Value existing = fn->get("prototype");
        return existing.isObject() ? existing.asObject() : nullptr;
    }
    if (fn->getOwnPropertyDescriptor("prototype")) {
        Value existing = fn->get("prototype");
        return existing.isObject() ? existing.asObject() : nullptr;
    }
    if (!fn->isCompiled() || !fn->isConstructor()) return nullptr;
    Object* proto = new Object();
    proto->setPrototype(intrinsic(Intrinsic::Object));
    proto->set("constructor", Value::object(fn));
    fn->set("prototype", Value::object(proto));
    return proto;
}

// obj.name: getters run with obj as `this`, primitives read through their
// prototype, and reading from null or undefined is a TypeError.
Value VM::getProperty(const Value& target, const std::string& name) {
    if (target.isObject()) {
        Object* obj = target.asObject();
        if (obj->objectType() == ObjectType::Proxy) {
            return obj->get(name);
        }
        if (obj->isArray() && name == "length") {
            return Value::number(static_cast<double>(obj->length()));
        }

        // Own properties and the object's prototype chain
        Value value;
        if (readProperty(obj, target, name, value)) return value;

        // Not stored: the built-in parts of arrays and functions, then the
        // prototype every object ends up with
        if (obj->isFunction()) {
            auto* fn = static_cast<Function*>(obj);
            if (name == "prototype") {
                Object* proto = ensurePrototypeObject(fn);
                return proto ? Value::object(proto) : Value::undefined();
            }
            if (name == "length") return Value::number(static_cast<double>(fn->arity()));
            if (name == "name") return Value::string(new String(fn->name()));
        }
        Object* builtin = obj->isArray() ? intrinsic(Intrinsic::Array)
                        : obj->isFunction() ? intrinsic(Intrinsic::Function) : nullptr;
        if (builtin && readProperty(builtin, target, name, value)) return value;
        return Value::undefined();
    }

    if (target.isString()) {
        auto* str = static_cast<String*>(target.asObject());
        if (name == "length") {
            return Value::number(static_cast<double>(str->length()));
        }
        uint32_t index;
        if (toArrayIndex(Value::string(new String(name)), index)) {
            return index < str->length() ? str->charAt(index) : Value::undefined();
        }
    }

    if (target.isNull() || target.isUndefined()) {
        throw std::runtime_error("Cannot read properties of " + describeForError(target) +
                                 " (reading '" + name + "')");
    }
    Object* proto = primitivePrototype(target);
    Value value;
    return proto && readProperty(proto, target, name, value) ? value : Value::undefined();
}

// Reads `name` from `holder` and its prototype chain as `receiver`: a getter
// runs with the receiver as `this`. False when nothing on the chain has it.
bool VM::readProperty(Object* holder, const Value& receiver, const std::string& name, Value& out) {
    const PropertyDescriptor* accessor = nullptr;
    if (!holder->lookupProperty(name, out, accessor)) return false;
    if (!accessor) return true;
    out = accessor->getter.isObject() && accessor->getter.asObject()->isFunction()
        ? executeCallback(static_cast<Function*>(accessor->getter.asObject()), receiver, {})
        : Value::undefined();
    return true;
}

// obj[key]: array indices read the element storage, other keys are names.
Value VM::getElement(const Value& target, const Value& key) {
    uint32_t index;
    if (toArrayIndex(key, index)) {
        if (target.isObject() && target.asObject()->objectType() != ObjectType::Proxy) {
            return target.asObject()->get(index);
        }
        if (target.isString()) {
            auto* str = static_cast<String*>(target.asObject());
            return index < str->length() ? str->charAt(index) : Value::undefined();
        }
    }
    return getProperty(target, propertyKeyName(key));
}

// obj.name = value: a setter on the object or its prototype chain takes the
// assignment; assigning to a primitive's property does nothing.
void VM::setProperty(const Value& target, const std::string& name, const Value& value) {
    if (!target.isObject()) {
        if (target.isNull() || target.isUndefined()) {
            throw std::runtime_error("Cannot set properties of " + describeForError(target) +
                                     " (setting '" + name + "')");
        }
        return;
    }
    Object* obj = target.asObject();
    if (obj->objectType() != ObjectType::Proxy) {
        Value existing;
        const PropertyDescriptor* accessor = nullptr;
        if (obj->lookupProperty(name, existing, accessor) && accessor) {
            if (accessor->setter.isObject() && accessor->setter.asObject()->isFunction()) {
                executeCallback(static_cast<Function*>(accessor->setter.asObject()), target, {value});
            }
            return;
        }
        if (obj->isArray() && name == "length") {
            const double length = value.toNumber();
            if (!(length >= 0) || length > 4294967295.0 || length != std::floor(length)) {
                throw ThrownException(makeError("RangeError", "Invalid array length"));
            }
            // Growing by more than this would allocate the elements; the
            // dense storage cannot hold a sparse length
            if (length > static_cast<double>(obj->length()) + (1u << 20)) {
                throw ThrownException(makeError("RangeError", "Array length too large"));
            }
            obj->setLength(static_cast<size_t>(length));
            return;
        }
    }
    obj->set(name, value);
    if (value.isObject() && gcHeap_) {
        gcHeap_->writeBarrier(obj, value.asObject());
    }
}

void VM::setElement(const Value& target, const Value& key, const Value& value) {
    uint32_t index;
    if (target.isObject() && target.asObject()->objectType() != ObjectType::Proxy &&
        toArrayIndex(key, index)) {
        Object* obj = target.asObject();
        obj->set(index, value);
        if (value.isObject() && gcHeap_) {
            gcHeap_->writeBarrier(obj, value.asObject());
        }
        return;
    }
    setProperty(target, propertyKeyName(key), value);
}

bool VM::deleteProperty(const Value& target, const Value& key) {
    if (target.isNull() || target.isUndefined()) {
        throw std::runtime_error("Cannot convert " + describeForError(target) + " to object");
    }
    if (!target.isObject()) return true;
    uint32_t index;
    if (toArrayIndex(key, index)) {
        return target.asObject()->deleteProperty(index);
    }
    return target.asObject()->deleteProperty(propertyKeyName(key));
}

// =============================================================================
// Iteration
// =============================================================================

Value VM::makeIterator(const Value& iterable) {
    using Kind = IteratorState::Kind;
    if (iterable.isString()) {
        return Value::object(new IteratorState(Kind::String, iterable));
    }
    if (!iterable.isObject()) {
        throw std::runtime_error(describeForError(iterable) + " is not iterable");
    }
    Object* obj = iterable.asObject();
    if (obj->isArray()) {
        return Value::object(new IteratorState(Kind::Indexed, iterable));
    }
    if (auto* map = dynamic_cast<Builtins::MapObject*>(obj)) {
        auto* entries = new Array();
        for (const auto& entry : map->entries()) {
            entries->push(Value::object(new Array({entry.first, entry.second})));
        }
        auto* state = new IteratorState(Kind::Indexed, iterable);
        state->setSnapshot(entries);
        return Value::object(state);
    }
    if (auto* set = dynamic_cast<Builtins::SetObject*>(obj)) {
        auto* state = new IteratorState(Kind::Indexed, iterable);
        state->setSnapshot(new Array(set->values()));
        return Value::object(state);
    }

    Value factory = getProperty(iterable, symbolPropertyKey(WellKnownSymbols::Iterator));
    if (factory.isObject() && factory.asObject()->isFunction()) {
        Value iterator = executeCallback(static_cast<Function*>(factory.asObject()), iterable, {});
        if (!iterator.isObject()) {
            throw std::runtime_error("Result of the Symbol.iterator method is not an object");
        }
        auto* state = new IteratorState(Kind::Protocol, iterator);
        state->setNextMethod(getProperty(iterator, "next"));
        return Value::object(state);
    }
    // Array-likes (a length and indices), such as DOM collections
    if (getProperty(iterable, "length").isNumber()) {
        return Value::object(new IteratorState(Kind::Indexed, iterable));
    }
    throw std::runtime_error(describeForError(iterable) + " is not iterable");
}

bool VM::iteratorStep(const Value& iterator, Value& value) {
    auto* state = iterator.isObject() ? dynamic_cast<IteratorState*>(iterator.asObject()) : nullptr;
    if (!state) {
        throw std::runtime_error("Value is not an iterator");
    }
    if (state->done_) return false;

    switch (state->kind_) {
        case IteratorState::Kind::Indexed: {
            if (state->snapshot_) {
                if (state->position_ < state->snapshot_->length()) {
                    value = state->snapshot_->get(static_cast<uint32_t>(state->position_++));
                    return true;
                }
                break;
            }
            Object* source = state->source_.asObject();
            const double length = source->isArray()
                ? static_cast<double>(source->length())
                : getProperty(state->source_, "length").toNumber();
            if (static_cast<double>(state->position_) < length) {
                value = getElement(state->source_, Value::number(static_cast<double>(state->position_++)));
                return true;
            }
            break;
        }
        case IteratorState::Kind::String: {
            const std::string& text = static_cast<String*>(state->source_.asObject())->value();
            if (state->position_ < text.size()) {
                size_t bytes = std::min(utf8SequenceLength(static_cast<unsigned char>(text[state->position_])),
                                        text.size() - state->position_);
                value = Value::string(new String(text.substr(state->position_, bytes)));
                state->position_ += bytes;
                return true;
            }
            break;
        }
        case IteratorState::Kind::Protocol: {
            const Value& next = state->nextMethod_;
            if (!next.isObject() || !next.asObject()->isFunction()) {
                throw std::runtime_error("Iterator has no next() method");
            }
            Value result = executeCallback(static_cast<Function*>(next.asObject()), state->source_, {});
            if (!result.isObject()) {
                throw std::runtime_error("Iterator result " + describeForError(result) + " is not an object");
            }
            if (getProperty(result, "done").isTruthy()) break;
            value = getProperty(result, "value");
            return true;
        }
    }
    state->done_ = true;
    return false;
}

// Upvalue management
RuntimeUpvalue* VM::captureUpvalue(Value* local) {
    // Look for existing open upvalue for this location
    RuntimeUpvalue* prevUpvalue = nullptr;
    RuntimeUpvalue* upvalue = openUpvalues_;
    
    while (upvalue != nullptr && upvalue->location() > local) {
        prevUpvalue = upvalue;
        upvalue = upvalue->next;
    }
    
    // Reuse existing upvalue if found
    if (upvalue != nullptr && upvalue->location() == local) {
        return upvalue;
    }
    
    // Create new upvalue
    RuntimeUpvalue* createdUpvalue = new RuntimeUpvalue(local);
    createdUpvalue->next = upvalue;
    
    // Insert into linked list
    if (prevUpvalue == nullptr) {
        openUpvalues_ = createdUpvalue;
    } else {
        prevUpvalue->next = createdUpvalue;
    }
    
    return createdUpvalue;
}

void VM::closeUpvalues(Value* last) {
    while (openUpvalues_ != nullptr && openUpvalues_->location() >= last) {
        RuntimeUpvalue* upvalue = openUpvalues_;
        upvalue->close();
        openUpvalues_ = upvalue->next;
    }
}

// =============================================================================
// HYBRID TWO-STACK IMPLEMENTATION
// =============================================================================

void VM::pushCallFrame(const VMCallFrame& frame) {
    if (!shouldUseHeapStack()) {
        // FAST PATH: Use native C stack (zero allocation)
        if (nativeDepth_ < NATIVE_STACK_SIZE) {
            nativeStack_[nativeDepth_++] = frame;
            return;
        }
    }
    
    // SLOW PATH: Switch to heap stack for deep recursion
    if (heapDepth_ >= HEAP_STACK_MAX) {
        throw std::runtime_error("Maximum call stack size exceeded (65536 frames)");
    }
    
    if (heapDepth_ >= heapStack_.size()) {
        heapStack_.push_back(frame);
    } else {
        heapStack_[heapDepth_] = frame;
    }
    heapDepth_++;
}

VMCallFrame VM::popCallFrame() {
    // Pop from heap stack first (LIFO)
    if (heapDepth_ > 0) {
        VMCallFrame frame = heapStack_[--heapDepth_];
        shrinkHeapIfNeeded();
        return frame;
    }
    
    // Pop from native stack
    if (nativeDepth_ > 0) {
        return nativeStack_[--nativeDepth_];
    }
    
    // Empty - return default frame
    return VMCallFrame{};
}

void VM::shrinkHeapIfNeeded() {
    // Auto-cleanup excess heap memory when returning to shallow depth
    // Only shrink if heap is significantly oversized and we're back to native
    if (heapDepth_ == 0 && heapStack_.size() > HEAP_STACK_PREALLOC * 2) {
        heapStack_.resize(HEAP_STACK_PREALLOC);
        heapStack_.shrink_to_fit();
    }
}

Value VM::resumeGenerator(GeneratorFrame* frame, Value yieldVal, const std::vector<Value>& args) {
    if (!frame || frame->isCompleted) {
        return Value::undefined();
    }

    VM* previousVM = currentVM_;
    currentVM_ = this;
    GeneratorFrame* savedGen = currentGenerator_;
    const Bytecode::BytecodeChunk* savedChunk = chunk_;
    const size_t savedIP = ip_;
    const size_t savedDepth = callDepth();
    const size_t base = stack_.size();
    const size_t savedHandlers = exceptionHandlers_.size();

    // The body runs in a frame whose return ends this run()
    VMCallFrame callFrame;
    callFrame.function = frame->function;
    callFrame.returnAddress = SIZE_MAX;
    callFrame.slotBase = base;
    callFrame.thisValue = frame->thisValue;
    callFrame.savedChunk = savedChunk;
    pushCallFrame(callFrame);

    if (!frame->isStarted) {
        frame->isStarted = true;
        push(frame->thisValue);  // Slot 0
        pushArguments(frame->function, args);
        ip_ = 0;
    } else {
        for (const auto& value : frame->savedStack) {
            push(value);
        }
        push(yieldVal);  // Value of the paused yield expression
        ip_ = frame->suspendedIP;
    }
    frame->stackBase = base;
    chunk_ = frame->function->bytecodeChunk();
    currentGenerator_ = frame;
    isYielding_ = false;

    Value result;
    try {
        run();
    } catch (...) {
        frame->isCompleted = true;
        frame->savedStack.clear();
        unwindTo(savedDepth, base, savedHandlers);
        currentGenerator_ = savedGen;
        chunk_ = savedChunk;
        ip_ = savedIP;
        currentVM_ = previousVM;
        throw;
    }

    if (isYielding_) {
        // OP_YIELD saved the frame's values; drop the live ones and the frame
        result = yieldedValue_;
        unwindTo(savedDepth, base, savedHandlers);
    } else {
        frame->isCompleted = true;
        frame->savedStack.clear();
        result = pop();  // Left by OP_RETURN
    }

    currentGenerator_ = savedGen;
    chunk_ = savedChunk;
    ip_ = savedIP;
    currentVM_ = previousVM;
    return result;
}

} // namespace Zepra::Runtime

// Frame accessor implementations (in separate TU block to avoid recompiling all of vm.cpp)
namespace Zepra::Runtime {

const VMCallFrame* VM_getFrame(const VM* vm, size_t idx,
    const VMCallFrame* nativeStack, size_t nativeDepth,
    const std::vector<VMCallFrame>& heapStack, size_t heapDepth) {
    if (idx < nativeDepth) return &nativeStack[nativeDepth - 1 - idx];
    idx -= nativeDepth;
    if (idx < heapDepth) return &heapStack[heapDepth - 1 - idx];
    return nullptr;
}

std::string VM::getFrameFunctionName(size_t frameIdx) const {
    auto* f = VM_getFrame(this, frameIdx, nativeStack_, nativeDepth_, heapStack_, heapDepth_);
    if (f && f->function) return f->function->name();
    return "<anonymous>";
}

std::string VM::getFrameSourceFile(size_t frameIdx) const {
    auto* f = VM_getFrame(this, frameIdx, nativeStack_, nativeDepth_, heapStack_, heapDepth_);
    if (f && f->function) return f->function->sourceFile();
    return "<unknown>";
}

uint32_t VM::getFrameLine(size_t frameIdx) const {
    auto* f = VM_getFrame(this, frameIdx, nativeStack_, nativeDepth_, heapStack_, heapDepth_);
    if (f && f->function && f->function->isCompiled() && f->function->bytecodeChunk()) {
        size_t ip = (frameIdx == 0) ? ip_ : f->returnAddress;
        if (ip > 0) ip--; // point to the call instruction, not the return address
        return f->function->bytecodeChunk()->lineAt(ip);
    }
    return 0;
}

uint32_t VM::getFrameColumn(size_t frameIdx) const {
    (void)frameIdx;
    return 0; // BytecodeChunk tracks lines only, not columns
}

Value VM::getFrameThisValue(size_t frameIdx) const {
    auto* f = VM_getFrame(this, frameIdx, nativeStack_, nativeDepth_, heapStack_, heapDepth_);
    if (f) return f->thisValue;
    return Value::undefined();
}

std::vector<std::string> VM::getFrameLocalNames(size_t frameIdx) const {
    auto* f = VM_getFrame(this, frameIdx, nativeStack_, nativeDepth_, heapStack_, heapDepth_);
    if (f && f->function) return f->function->localNames();
    return {};
}

Value VM::getFrameLocal(size_t frameIdx, const std::string& name) const {
    auto* f = VM_getFrame(this, frameIdx, nativeStack_, nativeDepth_, heapStack_, heapDepth_);
    if (!f || !f->function) return Value::undefined();
    const auto& names = f->function->localNames();
    for (size_t i = 0; i < names.size(); i++) {
        if (names[i] == name) {
            size_t stackIdx = f->slotBase + i;
            if (stackIdx < stack_.size()) return stack_[stackIdx];
        }
    }
    return Value::undefined();
}

std::vector<std::string> VM::getFrameClosureNames(size_t frameIdx) const {
    auto* f = VM_getFrame(this, frameIdx, nativeStack_, nativeDepth_, heapStack_, heapDepth_);
    if (!f || !f->function) return {};
    // Upvalue names are the captured variable names from the enclosing scope
    // The function's localNames covers params + locals; upvalues are from outer scope
    std::vector<std::string> names;
    for (size_t i = 0; i < f->function->upvalueCount(); i++) {
        names.push_back("upvalue_" + std::to_string(i));
    }
    return names;
}

Value VM::getFrameClosureValue(size_t frameIdx, const std::string& name) const {
    auto* f = VM_getFrame(this, frameIdx, nativeStack_, nativeDepth_, heapStack_, heapDepth_);
    if (!f || !f->function) return Value::undefined();
    // Parse upvalue index from name ("upvalue_N")
    if (name.substr(0, 8) == "upvalue_") {
        size_t idx = std::stoul(name.substr(8));
        auto* uv = f->function->upvalue(idx);
        if (uv) return uv->get();
    }
    return Value::undefined();
}

Value VM::evaluateInFrame(size_t frameIdx, const std::string& expression) {
    (void)frameIdx;
    if (!context_ || expression.empty()) return Value::undefined();

    // Compile expression in isolated scope and execute
    auto sourceCode = Frontend::SourceCode::fromString(expression, "<eval>");
    Frontend::Parser parser(sourceCode.get());
    auto ast = parser.parseProgram();
    if (parser.hasErrors()) return Value::undefined();

    Frontend::SyntaxChecker checker;
    if (!checker.check(ast.get())) return Value::undefined();

    Bytecode::BytecodeGenerator generator;
    auto chunk = generator.compile(ast.get());
    if (generator.hasErrors() || !chunk) return Value::undefined();

    auto result = execute(chunk.get());
    return (result.status == ExecutionResult::Status::Success) ? result.value : Value::undefined();
}

// Compiled chunk storage — keeps unique_ptrs alive for execute(void*) callers
static std::vector<std::unique_ptr<Bytecode::BytecodeChunk>> compiledChunks_;

void* VM::compile(const std::string& source, const std::string& filename) {
    auto sourceCode = Frontend::SourceCode::fromString(source, filename.empty() ? "<worker>" : filename);
    Frontend::Parser parser(sourceCode.get());
    auto ast = parser.parseProgram();
    if (parser.hasErrors()) return nullptr;

    Frontend::SyntaxChecker checker;
    if (!checker.check(ast.get())) return nullptr;

    Bytecode::BytecodeGenerator generator;
    auto chunk = generator.compile(ast.get());
    if (generator.hasErrors() || !chunk) return nullptr;

    auto* raw = chunk.get();
    compiledChunks_.push_back(std::move(chunk));
    return static_cast<void*>(const_cast<Bytecode::BytecodeChunk*>(raw));
}

void VM::execute(void* compiled) {
    if (!compiled) return;
    auto* chunk = static_cast<const Bytecode::BytecodeChunk*>(compiled);
    execute(chunk);
}

std::string VM::loadBundledScript(const std::string& url) {
    // File-system-based script loading for workers
    // Try url as a path relative to current module
    std::string path = url;
    if (!currentModulePath_.empty() && url[0] != '/') {
        size_t lastSlash = currentModulePath_.rfind('/');
        if (lastSlash != std::string::npos) {
            path = currentModulePath_.substr(0, lastSlash + 1) + url;
        }
    }
    std::ifstream file(path);
    if (!file.is_open()) return "";
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

void VM::runEventLoop() {
    if (eventLoop_) {
        eventLoop_->run();
    }
}

} // namespace Zepra::Runtime
