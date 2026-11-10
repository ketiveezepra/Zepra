#pragma once

/**
 * @file data_view.hpp
 * @brief JavaScript DataView for binary data manipulation
 */

#include "../config.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"
#include "typed_array.hpp"

namespace Zepra::Runtime { class Context; }

namespace Zepra::Builtins {

using Runtime::Value;
using Runtime::Object;

/**
 * @brief JavaScript DataView object
 * 
 * Provides low-level interface to read/write binary data
 * with explicit endianness control.
 */
class DataViewObject : public Object {
public:
    DataViewObject(ArrayBufferObject* buffer, size_t byteOffset = 0, size_t byteLength = 0);
    
    ArrayBufferObject* buffer() const { return buffer_; }
    size_t byteOffset() const { return byteOffset_; }
    size_t byteLength() const { return byteLength_; }
    
    // Integer getters (littleEndian = true for LE, false for BE)
    int8_t getInt8(size_t byteOffset) const;
    uint8_t getUint8(size_t byteOffset) const;
    int16_t getInt16(size_t byteOffset, bool littleEndian = false) const;
    uint16_t getUint16(size_t byteOffset, bool littleEndian = false) const;
    int32_t getInt32(size_t byteOffset, bool littleEndian = false) const;
    uint32_t getUint32(size_t byteOffset, bool littleEndian = false) const;
    
    // Float getters
    float getFloat32(size_t byteOffset, bool littleEndian = false) const;
    double getFloat64(size_t byteOffset, bool littleEndian = false) const;
    
    // Integer setters
    void setInt8(size_t byteOffset, int8_t value);
    void setUint8(size_t byteOffset, uint8_t value);
    void setInt16(size_t byteOffset, int16_t value, bool littleEndian = false);
    void setUint16(size_t byteOffset, uint16_t value, bool littleEndian = false);
    void setInt32(size_t byteOffset, int32_t value, bool littleEndian = false);
    void setUint32(size_t byteOffset, uint32_t value, bool littleEndian = false);
    
    // Float setters
    void setFloat32(size_t byteOffset, float value, bool littleEndian = false);
    void setFloat64(size_t byteOffset, double value, bool littleEndian = false);
    
private:
    ArrayBufferObject* buffer_;
    size_t byteOffset_;
    size_t byteLength_;
    
    template<typename T>
    T readValue(size_t offset, bool littleEndian) const;
    
    template<typename T>
    void writeValue(size_t offset, T value, bool littleEndian);
};

/**
 * @brief DataView builtin functions
 */
class DataViewBuiltin {
public:
    static Value constructor(Runtime::Context* ctx, const std::vector<Value>& args);
    static Value getInt8(Runtime::Context* ctx, const std::vector<Value>& args);
    static Value getUint8(Runtime::Context* ctx, const std::vector<Value>& args);
    static Value getInt16(Runtime::Context* ctx, const std::vector<Value>& args);
    static Value getUint16(Runtime::Context* ctx, const std::vector<Value>& args);
    static Value getInt32(Runtime::Context* ctx, const std::vector<Value>& args);
    static Value getUint32(Runtime::Context* ctx, const std::vector<Value>& args);
    static Value getFloat32(Runtime::Context* ctx, const std::vector<Value>& args);
    static Value getFloat64(Runtime::Context* ctx, const std::vector<Value>& args);
    
    static Object* createDataViewPrototype();
    // The script-facing DataView constructor (prototype inherits objectPrototype)
    static Object* createConstructor(Object* objectPrototype);
};

} // namespace Zepra::Builtins
