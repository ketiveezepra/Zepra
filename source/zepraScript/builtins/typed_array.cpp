// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file typed_array.cpp
 * @brief JavaScript ArrayBuffer and TypedArray builtin implementation
 */

#include "builtins/typed_array.hpp"
#include "builtins/builtin_helpers.hpp"
#include "builtins/list_iterator.hpp"
#include "runtime/execution/vm.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/well_known_symbols.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Zepra::Builtins {

using Runtime::FunctionCallInfo;
using Runtime::JSError;
using Runtime::VM;

// =============================================================================
// Element types
// =============================================================================

size_t bytesPerElement(TypedArrayType type) {
    switch (type) {
        case TypedArrayType::Int8:
        case TypedArrayType::Uint8:
        case TypedArrayType::Uint8Clamped:
            return 1;
        case TypedArrayType::Int16:
        case TypedArrayType::Uint16:
            return 2;
        case TypedArrayType::Int32:
        case TypedArrayType::Uint32:
        case TypedArrayType::Float32:
            return 4;
        case TypedArrayType::Float64:
        case TypedArrayType::BigInt64:
        case TypedArrayType::BigUint64:
            return 8;
    }
    return 1;
}

const char* typedArrayName(TypedArrayType type) {
    switch (type) {
        case TypedArrayType::Int8: return "Int8Array";
        case TypedArrayType::Uint8: return "Uint8Array";
        case TypedArrayType::Uint8Clamped: return "Uint8ClampedArray";
        case TypedArrayType::Int16: return "Int16Array";
        case TypedArrayType::Uint16: return "Uint16Array";
        case TypedArrayType::Int32: return "Int32Array";
        case TypedArrayType::Uint32: return "Uint32Array";
        case TypedArrayType::Float32: return "Float32Array";
        case TypedArrayType::Float64: return "Float64Array";
        case TypedArrayType::BigInt64: return "BigInt64Array";
        case TypedArrayType::BigUint64: return "BigUint64Array";
    }
    return "TypedArray";
}

namespace {

// ToInt32-style modular conversion into an unsigned `bits`-wide integer
uint64_t toModular(double num, int bits) {
    if (!std::isfinite(num)) return 0;
    double t = std::trunc(num);
    const double modulus = std::ldexp(1.0, bits);
    t = std::fmod(t, modulus);
    if (t < 0) t += modulus;
    return static_cast<uint64_t>(t);
}

// ToUint8Clamp: round half to even, NaN is 0
uint8_t toUint8Clamp(double num) {
    if (!(num > 0)) return 0;
    if (num >= 255) return 255;
    const double f = std::floor(num);
    if (f + 0.5 < num) return static_cast<uint8_t>(f + 1);
    if (num < f + 0.5) return static_cast<uint8_t>(f);
    const auto i = static_cast<uint8_t>(f);
    return (i % 2) ? static_cast<uint8_t>(i + 1) : i;
}

// A canonical array index string ("0", "17"; not "01" or "-1")
bool parseIndex(const std::string& key, uint32_t& index) {
    if (key.empty() || key.size() > 10 || (key.size() > 1 && key[0] == '0')) return false;
    uint64_t value = 0;
    for (char c : key) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    if (value > 0xFFFFFFFEull) return false;
    index = static_cast<uint32_t>(value);
    return true;
}

} // namespace

// =============================================================================
// ArrayBufferObject Implementation
// =============================================================================

ArrayBufferObject::ArrayBufferObject(size_t byteLength)
    : Object(ObjectType::ArrayBuffer)
    , data_(byteLength, 0) {}

void ArrayBufferObject::slice(size_t begin, size_t end, ArrayBufferObject* target) const {
    if (begin >= data_.size()) return;
    if (end > data_.size()) end = data_.size();
    if (begin >= end) return;

    size_t len = end - begin;
    if (target->byteLength() >= len) {
        std::memcpy(target->data(), data_.data() + begin, len);
    }
}

// =============================================================================
// TypedArrayObject Implementation
// =============================================================================

TypedArrayObject::TypedArrayObject(ArrayBufferObject* buffer, TypedArrayType type,
                                   size_t byteOffset, size_t length)
    : Object(ObjectType::TypedArray)
    , buffer_(buffer)
    , type_(type)
    , byteOffset_(byteOffset)
    , length_(length) {
    // Keeps the buffer reachable for the collector's reference walk
    if (buffer_) setInternalSlot("ViewedArrayBuffer", Value::object(buffer_));
}

Value TypedArrayObject::get(uint32_t index) const {
    if (!buffer_ || index >= length_) return Value::undefined();

    const uint8_t* ptr = data() + static_cast<size_t>(index) * bytesPerElement();
    switch (type_) {
        case TypedArrayType::Int8: {
            int8_t v; std::memcpy(&v, ptr, 1); return Value::number(v);
        }
        case TypedArrayType::Uint8:
        case TypedArrayType::Uint8Clamped:
            return Value::number(*ptr);
        case TypedArrayType::Int16: {
            int16_t v; std::memcpy(&v, ptr, 2); return Value::number(v);
        }
        case TypedArrayType::Uint16: {
            uint16_t v; std::memcpy(&v, ptr, 2); return Value::number(v);
        }
        case TypedArrayType::Int32: {
            int32_t v; std::memcpy(&v, ptr, 4); return Value::number(v);
        }
        case TypedArrayType::Uint32: {
            uint32_t v; std::memcpy(&v, ptr, 4); return Value::number(v);
        }
        case TypedArrayType::Float32: {
            float v; std::memcpy(&v, ptr, 4); return Value::number(v);
        }
        case TypedArrayType::Float64: {
            double v; std::memcpy(&v, ptr, 8); return Value::number(v);
        }
        default:
            return Value::undefined();
    }
}

bool TypedArrayObject::set(uint32_t index, Value value) {
    // Writes past the end are ignored, as for any integer-indexed exotic object
    const double num = value.toNumber();
    if (!buffer_ || index >= length_) return true;

    uint8_t* ptr = data() + static_cast<size_t>(index) * bytesPerElement();
    switch (type_) {
        case TypedArrayType::Int8:
        case TypedArrayType::Uint8:
            *ptr = static_cast<uint8_t>(toModular(num, 8));
            break;
        case TypedArrayType::Uint8Clamped:
            *ptr = toUint8Clamp(num);
            break;
        case TypedArrayType::Int16:
        case TypedArrayType::Uint16: {
            const auto v = static_cast<uint16_t>(toModular(num, 16));
            std::memcpy(ptr, &v, 2);
            break;
        }
        case TypedArrayType::Int32:
        case TypedArrayType::Uint32: {
            const auto v = static_cast<uint32_t>(toModular(num, 32));
            std::memcpy(ptr, &v, 4);
            break;
        }
        case TypedArrayType::Float32: {
            const auto v = static_cast<float>(num);
            std::memcpy(ptr, &v, 4);
            break;
        }
        case TypedArrayType::Float64:
            std::memcpy(ptr, &num, 8);
            break;
        default:
            break;
    }
    return true;
}

bool TypedArrayObject::has(uint32_t index) const {
    return index < length_;
}

Value TypedArrayObject::get(const std::string& key) const {
    uint32_t index;
    if (parseIndex(key, index)) return get(index);
    if (key == "length") return Value::number(static_cast<double>(length_));
    return Object::get(key);
}

bool TypedArrayObject::set(const std::string& key, Value value) {
    uint32_t index;
    if (parseIndex(key, index)) return set(index, value);
    return Object::set(key, value);
}

TypedArrayObject* TypedArrayObject::subarray(size_t begin, size_t end) const {
    if (begin >= length_) begin = length_;
    if (end > length_) end = length_;
    if (begin > end) begin = end;

    auto* view = new TypedArrayObject(buffer_, type_,
                                      byteOffset_ + begin * bytesPerElement(),
                                      end - begin);
    view->setPrototype(prototype());
    return view;
}

// =============================================================================
// Argument helpers
// =============================================================================

namespace {

VM& runningVM(const char* what) {
    VM* vm = VM::current();
    if (!vm) throw JSError("TypeError", std::string(what) + " called outside a running script");
    return *vm;
}

// ToIndex: an integer in [0, 2^53), RangeError otherwise
size_t toIndex(const Value& value, const char* what) {
    if (value.isUndefined()) return 0;
    double d = value.toNumber();
    d = std::isnan(d) ? 0 : std::trunc(d);
    if (d < 0 || d > 9007199254740991.0) {
        throw JSError("RangeError", std::string("Invalid ") + what);
    }
    return static_cast<size_t>(d);
}

// A relative index argument (negative counts from the end), clamped to [0, length]
size_t relativeIndex(const Value& value, size_t length, size_t fallback) {
    if (value.isUndefined()) return fallback;
    double d = value.toNumber();
    if (std::isnan(d)) return 0;
    d = std::trunc(d);
    const double len = static_cast<double>(length);
    if (d < 0) d = std::max(0.0, len + d);
    return static_cast<size_t>(std::min(d, len));
}

TypedArrayObject* thisTypedArray(const FunctionCallInfo& info, const char* method) {
    const Value self = info.thisValue();
    auto* ta = self.isObject() ? dynamic_cast<TypedArrayObject*>(self.asObject()) : nullptr;
    if (!ta) {
        throw JSError("TypeError", std::string("Method %TypedArray%.prototype.") + method +
                                   " called on incompatible receiver " + self.toString());
    }
    return ta;
}

ArrayBufferObject* thisArrayBuffer(const FunctionCallInfo& info, const char* method) {
    const Value self = info.thisValue();
    auto* buffer = self.isObject() ? dynamic_cast<ArrayBufferObject*>(self.asObject()) : nullptr;
    if (!buffer) {
        throw JSError("TypeError", std::string("Method ArrayBuffer.prototype.") + method +
                                   " called on incompatible receiver " + self.toString());
    }
    return buffer;
}

Runtime::Function* callbackArgument(const Value& value, const char* method) {
    if (!value.isObject() || !value.asObject()->isFunction()) {
        throw JSError("TypeError", value.toString() + " is not a function (TypedArray." + method + ")");
    }
    return static_cast<Runtime::Function*>(value.asObject());
}

// The ArrayBuffer prototype new buffers get: the one the source's buffer has
Object* bufferPrototypeOf(const TypedArrayObject* ta) {
    return ta->buffer() ? ta->buffer()->prototype() : nullptr;
}

// A new, zeroed array of the same type and prototype as `like`
TypedArrayObject* makeLike(const TypedArrayObject* like, size_t length) {
    auto* buffer = new ArrayBufferObject(length * like->bytesPerElement());
    buffer->setPrototype(bufferPrototypeOf(like));
    auto* ta = new TypedArrayObject(buffer, like->type(), 0, length);
    ta->setPrototype(like->prototype());
    return ta;
}

// The values an iterable or array-like produces
std::vector<Value> collectValues(const Value& source) {
    std::vector<Value> values;
    Object* obj = source.isObject() && !source.isString() ? source.asObject() : nullptr;
    if (obj && obj->isArray()) {
        values.reserve(obj->length());
        for (size_t i = 0; i < obj->length(); ++i) values.push_back(obj->get(static_cast<uint32_t>(i)));
        return values;
    }
    VM& vm = runningVM("TypedArray constructor");
    vm.forOf(source, [&](const Value& v) {
        values.push_back(v);
        return true;
    });
    return values;
}

// Comparison used by the default sort: numeric, NaN last, -0 before +0
bool defaultLess(double a, double b) {
    if (std::isnan(b)) return !std::isnan(a);
    if (std::isnan(a)) return false;
    if (a < b) return true;
    if (a > b) return false;
    return std::signbit(a) && !std::signbit(b);
}

} // namespace

// =============================================================================
// Constructors
// =============================================================================

TypedArrayObject* TypedArrayBuiltin::construct(TypedArrayType type, const std::vector<Value>& args,
                                               Object* prototype, Object* arrayBufferPrototype) {
    const size_t elementSize = bytesPerElement(type);
    auto allocate = [&](size_t length) {
        if (length > (size_t(1) << 31) / elementSize) {
            throw JSError("RangeError", "Invalid typed array length: " + std::to_string(length));
        }
        auto* buffer = new ArrayBufferObject(length * elementSize);
        buffer->setPrototype(arrayBufferPrototype);
        auto* ta = new TypedArrayObject(buffer, type, 0, length);
        ta->setPrototype(prototype);
        return ta;
    };

    const Value first = args.empty() ? Value::undefined() : args[0];
    if (!first.isObject() || first.isString()) {
        // new Float32Array(length)
        return allocate(toIndex(first, "typed array length"));
    }

    Object* obj = first.asObject();
    if (auto* buffer = dynamic_cast<ArrayBufferObject*>(obj)) {
        // new Float32Array(buffer, byteOffset?, length?)
        const size_t offset = toIndex(args.size() > 1 ? args[1] : Value::undefined(), "typed array offset");
        if (offset % elementSize != 0) {
            throw JSError("RangeError", std::string("start offset of ") + typedArrayName(type) +
                                        " should be a multiple of " + std::to_string(elementSize));
        }
        size_t length;
        const Value lengthArg = args.size() > 2 ? args[2] : Value::undefined();
        if (lengthArg.isUndefined()) {
            if (buffer->byteLength() % elementSize != 0) {
                throw JSError("RangeError", std::string("byte length of ") + typedArrayName(type) +
                                            " should be a multiple of " + std::to_string(elementSize));
            }
            if (offset > buffer->byteLength()) {
                throw JSError("RangeError", "Start offset " + std::to_string(offset) + " is outside the bounds of the buffer");
            }
            length = (buffer->byteLength() - offset) / elementSize;
        } else {
            length = toIndex(lengthArg, "typed array length");
            if (offset + length * elementSize > buffer->byteLength()) {
                throw JSError("RangeError", "Invalid typed array length: " + std::to_string(length));
            }
        }
        auto* ta = new TypedArrayObject(buffer, type, offset, length);
        ta->setPrototype(prototype);
        return ta;
    }

    if (auto* source = dynamic_cast<TypedArrayObject*>(obj)) {
        // new Float32Array(typedArray): element-wise copy with conversion
        TypedArrayObject* ta = allocate(source->length());
        if (source->type() == type) {
            std::memcpy(ta->data(), source->data(), source->byteLength());
        } else {
            for (uint32_t i = 0; i < source->length(); ++i) ta->set(i, source->get(i));
        }
        return ta;
    }

    // new Float32Array(iterable or array-like)
    const std::vector<Value> values = collectValues(first);
    TypedArrayObject* ta = allocate(values.size());
    for (size_t i = 0; i < values.size(); ++i) ta->set(static_cast<uint32_t>(i), values[i]);
    return ta;
}

Runtime::Function* TypedArrayBuiltin::createTypedArrayConstructor(TypedArrayType type, Object* prototype,
                                                                  Object* arrayBufferPrototype) {
    const char* name = typedArrayName(type);
    auto* constructor = new Function(name, [type, prototype, arrayBufferPrototype, name](const FunctionCallInfo& info) -> Value {
        // `new Float32Array()` arrives with a fresh object inheriting from the prototype; a plain call does not
        const Value self = info.thisValue();
        if (!self.isObject() || !inheritsFrom(self.asObject(), prototype)) {
            throw JSError("TypeError", std::string("Constructor ") + name + " requires 'new'");
        }
        std::vector<Value> args;
        for (size_t i = 0; i < info.argumentCount(); ++i) args.push_back(info.argument(i));
        TypedArrayObject* ta = construct(type, args, self.asObject()->prototype(), arrayBufferPrototype);
        return Value::object(ta);
    }, 3);

    defineHidden(constructor, "prototype", Value::object(prototype));
    defineHidden(prototype, "constructor", Value::object(constructor));
    const Value size = Value::number(static_cast<double>(bytesPerElement(type)));
    defineConstant(constructor, "BYTES_PER_ELEMENT", size);
    defineConstant(prototype, "BYTES_PER_ELEMENT", size);
    defineConstant(prototype, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::ToStringTag), makeString(name));

    // Float32Array.from(source, mapFn?, thisArg?)
    defineMethod(constructor, "from", [type, prototype, arrayBufferPrototype](const FunctionCallInfo& info) -> Value {
        const Value source = info.argument(0);
        if (!source.isObject() && !source.isString()) throw JSError("TypeError", source.toString() + " is not iterable");
        std::vector<Value> values = collectValues(source);
        if (!info.argument(1).isUndefined()) {
            Runtime::Function* mapFn = callbackArgument(info.argument(1), "from");
            VM& vm = runningVM("TypedArray.from");
            for (size_t i = 0; i < values.size(); ++i) {
                values[i] = vm.executeCallback(mapFn, info.argument(2), {values[i], Value::number(static_cast<double>(i))});
            }
        }
        TypedArrayObject* ta = construct(type, {Value::number(static_cast<double>(values.size()))},
                                         prototype, arrayBufferPrototype);
        for (size_t i = 0; i < values.size(); ++i) ta->set(static_cast<uint32_t>(i), values[i]);
        return Value::object(ta);
    }, 1);

    // Float32Array.of(...items)
    defineMethod(constructor, "of", [type, prototype, arrayBufferPrototype](const FunctionCallInfo& info) -> Value {
        TypedArrayObject* ta = construct(type, {Value::number(static_cast<double>(info.argumentCount()))},
                                         prototype, arrayBufferPrototype);
        for (size_t i = 0; i < info.argumentCount(); ++i) ta->set(static_cast<uint32_t>(i), info.argument(i));
        return Value::object(ta);
    }, 0);

    return constructor;
}

Object* TypedArrayBuiltin::createArrayBufferPrototype() {
    Object* proto = new Object();

    Runtime::PropertyDescriptor byteLength;
    byteLength.getter = Value::object(new Function("get byteLength", [](const FunctionCallInfo& info) -> Value {
        return Value::number(static_cast<double>(thisArrayBuffer(info, "byteLength")->byteLength()));
    }, 0));
    byteLength.attributes = Runtime::PropertyAttribute::Configurable;
    proto->defineProperty("byteLength", byteLength);

    // slice(begin?, end?) copies into a new buffer
    defineMethod(proto, "slice", [](const FunctionCallInfo& info) -> Value {
        ArrayBufferObject* buffer = thisArrayBuffer(info, "slice");
        const size_t length = buffer->byteLength();
        const size_t begin = relativeIndex(info.argument(0), length, 0);
        const size_t end = relativeIndex(info.argument(1), length, length);
        auto* copy = new ArrayBufferObject(end > begin ? end - begin : 0);
        copy->setPrototype(buffer->prototype());
        buffer->slice(begin, end, copy);
        return Value::object(copy);
    }, 2);

    defineConstant(proto, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::ToStringTag), makeString("ArrayBuffer"));
    return proto;
}

Runtime::Function* TypedArrayBuiltin::createArrayBufferConstructor(Object* prototype) {
    auto* constructor = new Function("ArrayBuffer", [prototype](const FunctionCallInfo& info) -> Value {
        const Value self = info.thisValue();
        if (!self.isObject() || !inheritsFrom(self.asObject(), prototype)) {
            throw JSError("TypeError", "Constructor ArrayBuffer requires 'new'");
        }
        const size_t byteLength = toIndex(info.argument(0), "array buffer length");
        if (byteLength > (size_t(1) << 31)) {
            throw JSError("RangeError", "Array buffer allocation failed");
        }
        auto* buffer = new ArrayBufferObject(byteLength);
        buffer->setPrototype(self.asObject()->prototype());
        return Value::object(buffer);
    }, 1);

    defineHidden(constructor, "prototype", Value::object(prototype));
    defineHidden(prototype, "constructor", Value::object(constructor));

    defineMethod(constructor, "isView", [](const FunctionCallInfo& info) -> Value {
        const Value arg = info.argument(0);
        return Value::boolean(arg.isObject() &&
            (arg.asObject()->objectType() == ObjectType::TypedArray ||
             arg.asObject()->objectType() == ObjectType::DataView));
    }, 1);
    return constructor;
}

// =============================================================================
// %TypedArray%.prototype
// =============================================================================

Object* TypedArrayBuiltin::createTypedArrayPrototype() {
    Object* proto = new Object();

    auto accessor = [proto](const char* name, Value (*read)(TypedArrayObject*)) {
        Runtime::PropertyDescriptor desc;
        desc.getter = Value::object(new Function(std::string("get ") + name,
            [name, read](const FunctionCallInfo& info) -> Value {
                return read(thisTypedArray(info, name));
            }, 0));
        desc.attributes = Runtime::PropertyAttribute::Configurable;
        proto->defineProperty(name, desc);
    };
    accessor("length", [](TypedArrayObject* ta) { return Value::number(static_cast<double>(ta->length())); });
    accessor("byteLength", [](TypedArrayObject* ta) { return Value::number(static_cast<double>(ta->byteLength())); });
    accessor("byteOffset", [](TypedArrayObject* ta) { return Value::number(static_cast<double>(ta->byteOffset())); });
    accessor("buffer", [](TypedArrayObject* ta) { return Value::object(ta->buffer()); });

    // set(source, offset?): copies an array, array-like or typed array in
    defineMethod(proto, "set", [](const FunctionCallInfo& info) -> Value {
        TypedArrayObject* ta = thisTypedArray(info, "set");
        const size_t offset = toIndex(info.argument(1), "offset");
        const Value source = info.argument(0);
        std::vector<Value> values;
        if (auto* src = source.isObject() ? dynamic_cast<TypedArrayObject*>(source.asObject()) : nullptr) {
            values.reserve(src->length());
            for (uint32_t i = 0; i < src->length(); ++i) values.push_back(src->get(i));
        } else if (source.isObject() && !source.isString()) {
            Object* obj = source.asObject();
            const size_t length = obj->isArray() ? obj->length()
                : static_cast<size_t>(std::max(0.0, runningVM("set").getMember(source, "length").toNumber()));
            values.reserve(length);
            for (size_t i = 0; i < length; ++i) values.push_back(obj->get(static_cast<uint32_t>(i)));
        } else {
            throw JSError("TypeError", "invalid_argument");
        }
        if (offset > ta->length() || values.size() > ta->length() - offset) {
            throw JSError("RangeError", "offset is out of bounds");
        }
        for (size_t i = 0; i < values.size(); ++i) ta->set(static_cast<uint32_t>(offset + i), values[i]);
        return Value::undefined();
    }, 1);

    defineMethod(proto, "subarray", [](const FunctionCallInfo& info) -> Value {
        TypedArrayObject* ta = thisTypedArray(info, "subarray");
        const size_t begin = relativeIndex(info.argument(0), ta->length(), 0);
        const size_t end = relativeIndex(info.argument(1), ta->length(), ta->length());
        return Value::object(ta->subarray(begin, end));
    }, 2);

    defineMethod(proto, "slice", [](const FunctionCallInfo& info) -> Value {
        TypedArrayObject* ta = thisTypedArray(info, "slice");
        const size_t begin = relativeIndex(info.argument(0), ta->length(), 0);
        const size_t end = std::max(begin, relativeIndex(info.argument(1), ta->length(), ta->length()));
        TypedArrayObject* copy = makeLike(ta, end - begin);
        std::memcpy(copy->data(), ta->data() + begin * ta->bytesPerElement(), copy->byteLength());
        return Value::object(copy);
    }, 2);

    defineMethod(proto, "fill", [](const FunctionCallInfo& info) -> Value {
        TypedArrayObject* ta = thisTypedArray(info, "fill");
        const Value value = Value::number(info.argument(0).toNumber());
        const size_t start = relativeIndex(info.argument(1), ta->length(), 0);
        const size_t end = relativeIndex(info.argument(2), ta->length(), ta->length());
        for (size_t i = start; i < end; ++i) ta->set(static_cast<uint32_t>(i), value);
        return info.thisValue();
    }, 1);

    defineMethod(proto, "indexOf", [](const FunctionCallInfo& info) -> Value {
        TypedArrayObject* ta = thisTypedArray(info, "indexOf");
        const Value target = info.argument(0);
        for (size_t i = relativeIndex(info.argument(1), ta->length(), 0); i < ta->length(); ++i) {
            if (ta->get(static_cast<uint32_t>(i)).strictEquals(target)) return Value::number(static_cast<double>(i));
        }
        return Value::number(-1);
    }, 1);

    defineMethod(proto, "lastIndexOf", [](const FunctionCallInfo& info) -> Value {
        TypedArrayObject* ta = thisTypedArray(info, "lastIndexOf");
        if (ta->length() == 0) return Value::number(-1);
        const Value target = info.argument(0);
        double from = info.argumentCount() > 1 ? std::trunc(info.argument(1).toNumber()) : ta->length() - 1.0;
        if (std::isnan(from)) from = 0;
        if (from < 0) from += static_cast<double>(ta->length());
        from = std::min(from, ta->length() - 1.0);
        for (double i = from; i >= 0; --i) {
            if (ta->get(static_cast<uint32_t>(i)).strictEquals(target)) return Value::number(i);
        }
        return Value::number(-1);
    }, 1);

    defineMethod(proto, "includes", [](const FunctionCallInfo& info) -> Value {
        TypedArrayObject* ta = thisTypedArray(info, "includes");
        const Value target = info.argument(0);
        const bool findNaN = target.isNumber() && std::isnan(target.asNumber());
        for (size_t i = relativeIndex(info.argument(1), ta->length(), 0); i < ta->length(); ++i) {
            const Value v = ta->get(static_cast<uint32_t>(i));
            if (v.strictEquals(target) || (findNaN && std::isnan(v.asNumber()))) return Value::boolean(true);
        }
        return Value::boolean(false);
    }, 1);

    auto join = [](TypedArrayObject* ta, const std::string& separator) {
        std::string out;
        for (size_t i = 0; i < ta->length(); ++i) {
            if (i) out += separator;
            out += ta->get(static_cast<uint32_t>(i)).toString();
        }
        return makeString(out);
    };
    defineMethod(proto, "join", [join](const FunctionCallInfo& info) -> Value {
        TypedArrayObject* ta = thisTypedArray(info, "join");
        return join(ta, info.argument(0).isUndefined() ? "," : info.argument(0).toString());
    }, 1);
    defineMethod(proto, "toString", [join](const FunctionCallInfo& info) -> Value {
        return join(thisTypedArray(info, "toString"), ",");
    }, 0);

    defineMethod(proto, "at", [](const FunctionCallInfo& info) -> Value {
        TypedArrayObject* ta = thisTypedArray(info, "at");
        double index = std::trunc(info.argument(0).toNumber());
        if (std::isnan(index)) index = 0;
        if (index < 0) index += static_cast<double>(ta->length());
        if (index < 0 || index >= static_cast<double>(ta->length())) return Value::undefined();
        return ta->get(static_cast<uint32_t>(index));
    }, 1);

    defineMethod(proto, "reverse", [](const FunctionCallInfo& info) -> Value {
        TypedArrayObject* ta = thisTypedArray(info, "reverse");
        const size_t n = ta->length();
        for (size_t i = 0; i < n / 2; ++i) {
            const Value a = ta->get(static_cast<uint32_t>(i));
            ta->set(static_cast<uint32_t>(i), ta->get(static_cast<uint32_t>(n - 1 - i)));
            ta->set(static_cast<uint32_t>(n - 1 - i), a);
        }
        return info.thisValue();
    }, 0);

    defineMethod(proto, "sort", [](const FunctionCallInfo& info) -> Value {
        TypedArrayObject* ta = thisTypedArray(info, "sort");
        std::vector<double> values(ta->length());
        for (size_t i = 0; i < values.size(); ++i) values[i] = ta->get(static_cast<uint32_t>(i)).asNumber();
        if (info.argument(0).isUndefined()) {
            std::stable_sort(values.begin(), values.end(), defaultLess);
        } else {
            Runtime::Function* compare = callbackArgument(info.argument(0), "sort");
            VM& vm = runningVM("sort");
            std::stable_sort(values.begin(), values.end(), [&](double a, double b) {
                const double r = vm.executeCallback(compare, Value::undefined(),
                                                    {Value::number(a), Value::number(b)}).toNumber();
                return r < 0;
            });
        }
        for (size_t i = 0; i < values.size(); ++i) ta->set(static_cast<uint32_t>(i), Value::number(values[i]));
        return info.thisValue();
    }, 1);

    // Callback methods: fn(value, index, array) with an optional thisArg
    enum class Each { ForEach, Map, Filter, Every, Some, Find, FindIndex };
    auto iterate = [proto](const char* name, Each kind) {
        defineMethod(proto, name, [name, kind](const FunctionCallInfo& info) -> Value {
            TypedArrayObject* ta = thisTypedArray(info, name);
            Runtime::Function* fn = callbackArgument(info.argument(0), name);
            VM& vm = runningVM(name);
            const Value thisArg = info.argument(1);
            TypedArrayObject* mapped = kind == Each::Map ? makeLike(ta, ta->length()) : nullptr;
            std::vector<Value> kept;
            for (size_t i = 0; i < ta->length(); ++i) {
                const Value v = ta->get(static_cast<uint32_t>(i));
                const Value r = vm.executeCallback(fn, thisArg, {v, Value::number(static_cast<double>(i)), info.thisValue()});
                switch (kind) {
                    case Each::ForEach: break;
                    case Each::Map: mapped->set(static_cast<uint32_t>(i), r); break;
                    case Each::Filter: if (r.toBoolean()) kept.push_back(v); break;
                    case Each::Every: if (!r.toBoolean()) return Value::boolean(false); break;
                    case Each::Some: if (r.toBoolean()) return Value::boolean(true); break;
                    case Each::Find: if (r.toBoolean()) return v; break;
                    case Each::FindIndex: if (r.toBoolean()) return Value::number(static_cast<double>(i)); break;
                }
            }
            switch (kind) {
                case Each::Map: return Value::object(mapped);
                case Each::Filter: {
                    TypedArrayObject* out = makeLike(ta, kept.size());
                    for (size_t i = 0; i < kept.size(); ++i) out->set(static_cast<uint32_t>(i), kept[i]);
                    return Value::object(out);
                }
                case Each::Every: return Value::boolean(true);
                case Each::Some: return Value::boolean(false);
                case Each::FindIndex: return Value::number(-1);
                default: return Value::undefined();
            }
        }, 1);
    };
    iterate("forEach", Each::ForEach);
    iterate("map", Each::Map);
    iterate("filter", Each::Filter);
    iterate("every", Each::Every);
    iterate("some", Each::Some);
    iterate("find", Each::Find);
    iterate("findIndex", Each::FindIndex);

    defineMethod(proto, "reduce", [](const FunctionCallInfo& info) -> Value {
        TypedArrayObject* ta = thisTypedArray(info, "reduce");
        Runtime::Function* fn = callbackArgument(info.argument(0), "reduce");
        VM& vm = runningVM("reduce");
        size_t i = 0;
        Value acc;
        if (info.argumentCount() > 1) {
            acc = info.argument(1);
        } else {
            if (ta->length() == 0) throw JSError("TypeError", "Reduce of empty array with no initial value");
            acc = ta->get(0u);
            i = 1;
        }
        for (; i < ta->length(); ++i) {
            acc = vm.executeCallback(fn, Value::undefined(),
                {acc, ta->get(static_cast<uint32_t>(i)), Value::number(static_cast<double>(i)), info.thisValue()});
        }
        return acc;
    }, 1);

    // keys/values/entries iterate a snapshot; values is also Symbol.iterator
    auto listMethod = [](const char* name, int mode) {
        return [name, mode](const FunctionCallInfo& info) -> Value {
            TypedArrayObject* ta = thisTypedArray(info, name);
            std::vector<Value> items;
            items.reserve(ta->length());
            for (size_t i = 0; i < ta->length(); ++i) {
                const Value index = Value::number(static_cast<double>(i));
                const Value v = ta->get(static_cast<uint32_t>(i));
                items.push_back(mode == 0 ? index : mode == 1 ? v : Value::object(new Runtime::Array({index, v})));
            }
            return IteratorBuiltin::makeListIterator(std::move(items));
        };
    };
    defineMethod(proto, "keys", listMethod("keys", 0), 0);
    Function* values = defineMethod(proto, "values", listMethod("values", 1), 0);
    defineMethod(proto, "entries", listMethod("entries", 2), 0);
    defineHidden(proto, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::Iterator), Value::object(values));

    return proto;
}

} // namespace Zepra::Builtins
