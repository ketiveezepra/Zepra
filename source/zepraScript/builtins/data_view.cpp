// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file data_view.cpp
 * @brief JavaScript DataView implementation
 */

#include "builtins/data_view.hpp"
#include "runtime/objects/function.hpp"
#include <cstring>
#include <cmath>
#include "builtins/builtin_helpers.hpp"
#include "runtime/objects/well_known_symbols.hpp"

namespace Zepra::Builtins {

DataViewObject::DataViewObject(ArrayBufferObject* buffer, size_t byteOffset, size_t byteLength)
    : Object(Runtime::ObjectType::Ordinary)
    , buffer_(buffer)
    , byteOffset_(byteOffset)
    , byteLength_(byteLength == 0 && buffer ? buffer->byteLength() - byteOffset : byteLength) {}

// Helper to swap bytes for endianness
template<typename T>
static T swapBytes(T value) {
    T result = 0;
    uint8_t* src = reinterpret_cast<uint8_t*>(&value);
    uint8_t* dst = reinterpret_cast<uint8_t*>(&result);
    for (size_t i = 0; i < sizeof(T); ++i) {
        dst[i] = src[sizeof(T) - 1 - i];
    }
    return result;
}

template<typename T>
T DataViewObject::readValue(size_t offset, bool littleEndian) const {
    if (!buffer_ || offset + sizeof(T) > byteLength_) return T{};
    
    T value;
    std::memcpy(&value, buffer_->data() + byteOffset_ + offset, sizeof(T));
    
    // Check system endianness and swap if needed
    static const uint16_t test = 1;
    bool systemIsLittleEndian = (*reinterpret_cast<const uint8_t*>(&test) == 1);
    
    if (littleEndian != systemIsLittleEndian && sizeof(T) > 1) {
        value = swapBytes(value);
    }
    
    return value;
}

template<typename T>
void DataViewObject::writeValue(size_t offset, T value, bool littleEndian) {
    if (!buffer_ || offset + sizeof(T) > byteLength_) return;
    
    static const uint16_t test = 1;
    bool systemIsLittleEndian = (*reinterpret_cast<const uint8_t*>(&test) == 1);
    
    if (littleEndian != systemIsLittleEndian && sizeof(T) > 1) {
        value = swapBytes(value);
    }
    
    std::memcpy(buffer_->data() + byteOffset_ + offset, &value, sizeof(T));
}

// Getters
int8_t DataViewObject::getInt8(size_t offset) const {
    return readValue<int8_t>(offset, true);
}

uint8_t DataViewObject::getUint8(size_t offset) const {
    return readValue<uint8_t>(offset, true);
}

int16_t DataViewObject::getInt16(size_t offset, bool littleEndian) const {
    return readValue<int16_t>(offset, littleEndian);
}

uint16_t DataViewObject::getUint16(size_t offset, bool littleEndian) const {
    return readValue<uint16_t>(offset, littleEndian);
}

int32_t DataViewObject::getInt32(size_t offset, bool littleEndian) const {
    return readValue<int32_t>(offset, littleEndian);
}

uint32_t DataViewObject::getUint32(size_t offset, bool littleEndian) const {
    return readValue<uint32_t>(offset, littleEndian);
}

float DataViewObject::getFloat32(size_t offset, bool littleEndian) const {
    return readValue<float>(offset, littleEndian);
}

double DataViewObject::getFloat64(size_t offset, bool littleEndian) const {
    return readValue<double>(offset, littleEndian);
}

// Setters
void DataViewObject::setInt8(size_t offset, int8_t value) {
    writeValue(offset, value, true);
}

void DataViewObject::setUint8(size_t offset, uint8_t value) {
    writeValue(offset, value, true);
}

void DataViewObject::setInt16(size_t offset, int16_t value, bool littleEndian) {
    writeValue(offset, value, littleEndian);
}

void DataViewObject::setUint16(size_t offset, uint16_t value, bool littleEndian) {
    writeValue(offset, value, littleEndian);
}

void DataViewObject::setInt32(size_t offset, int32_t value, bool littleEndian) {
    writeValue(offset, value, littleEndian);
}

void DataViewObject::setUint32(size_t offset, uint32_t value, bool littleEndian) {
    writeValue(offset, value, littleEndian);
}

void DataViewObject::setFloat32(size_t offset, float value, bool littleEndian) {
    writeValue(offset, value, littleEndian);
}

void DataViewObject::setFloat64(size_t offset, double value, bool littleEndian) {
    writeValue(offset, value, littleEndian);
}

// =============================================================================
// DataViewBuiltin Implementation
// =============================================================================

Value DataViewBuiltin::constructor(Runtime::Context*, const std::vector<Value>& args) {
    if (args.empty() || !args[0].isObject()) return Value::undefined();
    
    ArrayBufferObject* buffer = dynamic_cast<ArrayBufferObject*>(args[0].asObject());
    if (!buffer) return Value::undefined();
    
    size_t offset = args.size() > 1 && args[1].isNumber() 
        ? static_cast<size_t>(args[1].asNumber()) : 0;
    size_t length = args.size() > 2 && args[2].isNumber()
        ? static_cast<size_t>(args[2].asNumber()) : 0;
    
    return Value::object(new DataViewObject(buffer, offset, length));
}

Value DataViewBuiltin::getInt8(Runtime::Context*, const std::vector<Value>& args) {
    if (args.size() < 2 || !args[0].isObject() || !args[1].isNumber()) 
        return Value::undefined();
    DataViewObject* dv = dynamic_cast<DataViewObject*>(args[0].asObject());
    if (!dv) return Value::undefined();
    return Value::number(dv->getInt8(static_cast<size_t>(args[1].asNumber())));
}

Value DataViewBuiltin::getUint8(Runtime::Context*, const std::vector<Value>& args) {
    if (args.size() < 2 || !args[0].isObject() || !args[1].isNumber()) 
        return Value::undefined();
    DataViewObject* dv = dynamic_cast<DataViewObject*>(args[0].asObject());
    if (!dv) return Value::undefined();
    return Value::number(dv->getUint8(static_cast<size_t>(args[1].asNumber())));
}

Value DataViewBuiltin::getInt16(Runtime::Context*, const std::vector<Value>& args) {
    if (args.size() < 2 || !args[0].isObject() || !args[1].isNumber()) 
        return Value::undefined();
    DataViewObject* dv = dynamic_cast<DataViewObject*>(args[0].asObject());
    if (!dv) return Value::undefined();
    bool le = args.size() > 2 && args[2].isBoolean() && args[2].asBoolean();
    return Value::number(dv->getInt16(static_cast<size_t>(args[1].asNumber()), le));
}

Value DataViewBuiltin::getUint16(Runtime::Context*, const std::vector<Value>& args) {
    if (args.size() < 2 || !args[0].isObject() || !args[1].isNumber()) 
        return Value::undefined();
    DataViewObject* dv = dynamic_cast<DataViewObject*>(args[0].asObject());
    if (!dv) return Value::undefined();
    bool le = args.size() > 2 && args[2].isBoolean() && args[2].asBoolean();
    return Value::number(dv->getUint16(static_cast<size_t>(args[1].asNumber()), le));
}

Value DataViewBuiltin::getInt32(Runtime::Context*, const std::vector<Value>& args) {
    if (args.size() < 2 || !args[0].isObject() || !args[1].isNumber()) 
        return Value::undefined();
    DataViewObject* dv = dynamic_cast<DataViewObject*>(args[0].asObject());
    if (!dv) return Value::undefined();
    bool le = args.size() > 2 && args[2].isBoolean() && args[2].asBoolean();
    return Value::number(dv->getInt32(static_cast<size_t>(args[1].asNumber()), le));
}

Value DataViewBuiltin::getUint32(Runtime::Context*, const std::vector<Value>& args) {
    if (args.size() < 2 || !args[0].isObject() || !args[1].isNumber()) 
        return Value::undefined();
    DataViewObject* dv = dynamic_cast<DataViewObject*>(args[0].asObject());
    if (!dv) return Value::undefined();
    bool le = args.size() > 2 && args[2].isBoolean() && args[2].asBoolean();
    return Value::number(dv->getUint32(static_cast<size_t>(args[1].asNumber()), le));
}

Value DataViewBuiltin::getFloat32(Runtime::Context*, const std::vector<Value>& args) {
    if (args.size() < 2 || !args[0].isObject() || !args[1].isNumber()) 
        return Value::undefined();
    DataViewObject* dv = dynamic_cast<DataViewObject*>(args[0].asObject());
    if (!dv) return Value::undefined();
    bool le = args.size() > 2 && args[2].isBoolean() && args[2].asBoolean();
    return Value::number(dv->getFloat32(static_cast<size_t>(args[1].asNumber()), le));
}

Value DataViewBuiltin::getFloat64(Runtime::Context*, const std::vector<Value>& args) {
    if (args.size() < 2 || !args[0].isObject() || !args[1].isNumber()) 
        return Value::undefined();
    DataViewObject* dv = dynamic_cast<DataViewObject*>(args[0].asObject());
    if (!dv) return Value::undefined();
    bool le = args.size() > 2 && args[2].isBoolean() && args[2].asBoolean();
    return Value::number(dv->getFloat64(static_cast<size_t>(args[1].asNumber()), le));
}

Object* DataViewBuiltin::createDataViewPrototype() {
    return new Object();
}

// ---- script-facing DataView -------------------------------------------------

namespace {

DataViewObject* thisView(const Runtime::FunctionCallInfo& info, const char* method) {
    auto* view = info.thisValue().isObject() ? dynamic_cast<DataViewObject*>(info.thisValue().asObject()) : nullptr;
    if (!view) throw Runtime::JSError("TypeError", std::string("DataView.prototype.") + method + " called on an incompatible receiver");
    return view;
}

// ToIndex of the byte offset, checked against the view for `size` bytes
size_t viewIndex(DataViewObject* view, const Value& v, size_t size) {
    double d = v.isUndefined() ? 0 : v.toNumber();
    d = std::isnan(d) ? 0 : std::trunc(d);
    if (d < 0 || d + static_cast<double>(size) > static_cast<double>(view->byteLength())) {
        throw Runtime::JSError("RangeError", "Offset is outside the bounds of the DataView");
    }
    return static_cast<size_t>(d);
}

double toUint32Bits(const Value& v) {
    double d = v.toNumber();
    if (!std::isfinite(d)) return 0;
    d = std::fmod(std::trunc(d), 4294967296.0);
    if (d < 0) d += 4294967296.0;
    return d;
}

} // namespace

Object* DataViewBuiltin::createConstructor(Object* objectPrototype) {
    auto* proto = new Object();
    proto->setPrototype(objectPrototype);

    auto* ctor = Runtime::createNativeFunction("DataView",
        [proto](Runtime::Context*, const std::vector<Value>& args) -> Value {
            ArrayBufferObject* buffer = !args.empty() && args[0].isObject()
                ? dynamic_cast<ArrayBufferObject*>(args[0].asObject()) : nullptr;
            if (!buffer) throw Runtime::JSError("TypeError", "First argument to DataView constructor must be an ArrayBuffer");
            double offset = args.size() > 1 && !args[1].isUndefined() ? std::trunc(args[1].toNumber()) : 0;
            if (std::isnan(offset)) offset = 0;
            if (offset < 0 || offset > static_cast<double>(buffer->byteLength())) {
                throw Runtime::JSError("RangeError", "Start offset " + std::to_string(static_cast<long long>(offset)) + " is outside the bounds of the buffer");
            }
            double length = static_cast<double>(buffer->byteLength()) - offset;
            if (args.size() > 2 && !args[2].isUndefined()) {
                length = std::trunc(args[2].toNumber());
                if (std::isnan(length)) length = 0;
                if (length < 0 || offset + length > static_cast<double>(buffer->byteLength())) {
                    throw Runtime::JSError("RangeError", "Invalid DataView length " + std::to_string(static_cast<long long>(length)));
                }
            }
            auto* view = new DataViewObject(buffer, static_cast<size_t>(offset), static_cast<size_t>(length));
            view->setPrototype(proto);
            return Value::object(view);
        }, 1);
    defineHidden(ctor, "prototype", Value::object(proto));
    defineHidden(proto, "constructor", Value::object(ctor));
    defineHidden(proto, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::ToStringTag),
                 makeString("DataView"));

    auto getter = [proto](const char* name, std::function<Value(DataViewObject*)> read) {
        Runtime::PropertyDescriptor desc;
        desc.getter = Value::object(new Runtime::Function(std::string("get ") + name,
            [name, read](const Runtime::FunctionCallInfo& info) -> Value { return read(thisView(info, name)); }, 0));
        desc.attributes = Runtime::PropertyAttribute::Configurable;
        proto->defineProperty(name, desc);
    };
    getter("buffer", [](DataViewObject* v) { return Value::object(v->buffer()); });
    getter("byteOffset", [](DataViewObject* v) { return Value::number(static_cast<double>(v->byteOffset())); });
    getter("byteLength", [](DataViewObject* v) { return Value::number(static_cast<double>(v->byteLength())); });

#define ZEPRA_VIEW_GET(Name, Type, size, call)                                                          \
    defineMethod(proto, "get" #Name, [](const Runtime::FunctionCallInfo& info) -> Value {               \
        DataViewObject* v = thisView(info, "get" #Name);                                                 \
        size_t at = viewIndex(v, info.argument(0), size);                                                \
        bool little = info.argument(1).toBoolean();                                                      \
        (void)little;                                                                                    \
        return Value::number(static_cast<double>(call));                                                 \
    }, 1);
    ZEPRA_VIEW_GET(Int8, int8_t, 1, v->getInt8(at))
    ZEPRA_VIEW_GET(Uint8, uint8_t, 1, v->getUint8(at))
    ZEPRA_VIEW_GET(Int16, int16_t, 2, v->getInt16(at, little))
    ZEPRA_VIEW_GET(Uint16, uint16_t, 2, v->getUint16(at, little))
    ZEPRA_VIEW_GET(Int32, int32_t, 4, v->getInt32(at, little))
    ZEPRA_VIEW_GET(Uint32, uint32_t, 4, v->getUint32(at, little))
    ZEPRA_VIEW_GET(Float32, float, 4, v->getFloat32(at, little))
    ZEPRA_VIEW_GET(Float64, double, 8, v->getFloat64(at, little))
#undef ZEPRA_VIEW_GET

#define ZEPRA_VIEW_SET(Name, size, call)                                                                \
    defineMethod(proto, "set" #Name, [](const Runtime::FunctionCallInfo& info) -> Value {               \
        DataViewObject* v = thisView(info, "set" #Name);                                                 \
        size_t at = viewIndex(v, info.argument(0), size);                                                \
        const Value value = info.argument(1);                                                            \
        bool little = info.argument(2).toBoolean();                                                      \
        (void)little;                                                                                    \
        call;                                                                                            \
        return Value::undefined();                                                                       \
    }, 2);
    ZEPRA_VIEW_SET(Int8, 1, v->setInt8(at, static_cast<int8_t>(static_cast<uint32_t>(toUint32Bits(value)))))
    ZEPRA_VIEW_SET(Uint8, 1, v->setUint8(at, static_cast<uint8_t>(static_cast<uint32_t>(toUint32Bits(value)))))
    ZEPRA_VIEW_SET(Int16, 2, v->setInt16(at, static_cast<int16_t>(static_cast<uint32_t>(toUint32Bits(value))), little))
    ZEPRA_VIEW_SET(Uint16, 2, v->setUint16(at, static_cast<uint16_t>(static_cast<uint32_t>(toUint32Bits(value))), little))
    ZEPRA_VIEW_SET(Int32, 4, v->setInt32(at, static_cast<int32_t>(static_cast<uint32_t>(toUint32Bits(value))), little))
    ZEPRA_VIEW_SET(Uint32, 4, v->setUint32(at, static_cast<uint32_t>(toUint32Bits(value)), little))
    ZEPRA_VIEW_SET(Float32, 4, v->setFloat32(at, static_cast<float>(value.toNumber()), little))
    ZEPRA_VIEW_SET(Float64, 8, v->setFloat64(at, value.toNumber(), little))
#undef ZEPRA_VIEW_SET

    return ctor;
}

} // namespace Zepra::Builtins
