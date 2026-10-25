#pragma once

/**
 * @file typed_array.hpp
 * @brief JavaScript ArrayBuffer and TypedArray builtins
 *
 * A TypedArrayObject is a view of an ArrayBufferObject: indexed reads and
 * writes (`ta[i]`) go straight to the buffer bytes, and length, byteLength,
 * byteOffset and buffer are accessors on %TypedArray%.prototype.
 */

#include "../config.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"
#include <vector>
#include <cstdint>

namespace Zepra::Runtime { class Context; class Function; }

namespace Zepra::Builtins {

using Runtime::Value;
using Runtime::Object;
using Runtime::ObjectType;

/**
 * @brief ArrayBuffer - raw binary data container
 */
class ArrayBufferObject : public Object {
public:
    explicit ArrayBufferObject(size_t byteLength);

    uint8_t* data() { return data_.data(); }
    const uint8_t* data() const { return data_.data(); }
    size_t byteLength() const { return data_.size(); }

    void slice(size_t begin, size_t end, ArrayBufferObject* target) const;

private:
    std::vector<uint8_t> data_;
};

/**
 * @brief TypedArray element types
 */
enum class TypedArrayType : uint8_t {
    Int8,
    Uint8,
    Uint8Clamped,
    Int16,
    Uint16,
    Int32,
    Uint32,
    Float32,
    Float64,
    BigInt64,
    BigUint64
};

/** Bytes per element of `type` (1, 2, 4 or 8). */
size_t bytesPerElement(TypedArrayType type);

/** The constructor name of `type` ("Float32Array", ...). */
const char* typedArrayName(TypedArrayType type);

/**
 * @brief Base TypedArray - view into ArrayBuffer
 *
 * `length` is exact: a view of a whole buffer is made by passing
 * buffer->byteLength() / bytesPerElement() (TypedArrayBuiltin::construct does).
 */
class TypedArrayObject : public Object {
public:
    TypedArrayObject(ArrayBufferObject* buffer, TypedArrayType type,
                     size_t byteOffset, size_t length);

    // Indexed access reads and writes the buffer; other keys are ordinary properties
    using Object::get;
    using Object::set;
    using Object::has;
    Value get(uint32_t index) const override;
    bool set(uint32_t index, Value value) override;
    bool has(uint32_t index) const override;
    Value get(const std::string& key) const override;
    bool set(const std::string& key, Value value) override;

    size_t length() const override { return length_; }
    void setLength(size_t) override {}  // fixed length
    size_t byteLength() const { return length_ * bytesPerElement(); }
    size_t byteOffset() const { return byteOffset_; }
    size_t bytesPerElement() const { return Builtins::bytesPerElement(type_); }

    ArrayBufferObject* buffer() const { return buffer_; }
    TypedArrayType type() const { return type_; }

    /** First byte of element 0. */
    uint8_t* data() const { return buffer_->data() + byteOffset_; }

    // Subarray view (same buffer, same prototype)
    TypedArrayObject* subarray(size_t begin, size_t end) const;

private:
    ArrayBufferObject* buffer_;
    TypedArrayType type_;
    size_t byteOffset_;
    size_t length_;
};

/**
 * @brief ArrayBuffer and TypedArray constructors and prototypes
 */
class TypedArrayBuiltin {
public:
    /** ArrayBuffer constructor with `prototype` (byteLength, slice) wired up. */
    static Runtime::Function* createArrayBufferConstructor(Object* prototype);
    static Object* createArrayBufferPrototype();

    /** %TypedArray%.prototype: the methods and accessors shared by every element type. */
    static Object* createTypedArrayPrototype();

    /**
     * A constructor such as Float32Array. `prototype` inherits from
     * %TypedArray%.prototype; new arrays get the ArrayBuffer prototype on
     * the buffers they create.
     */
    static Runtime::Function* createTypedArrayConstructor(TypedArrayType type, Object* prototype,
                                                          Object* arrayBufferPrototype);

    /**
     * new <type>Array(args...) as the spec's TypedArray constructor:
     * (), (length), (typedArray), (iterable or array-like), (buffer, byteOffset?, length?).
     * Throws JSError RangeError/TypeError like the spec.
     */
    static TypedArrayObject* construct(TypedArrayType type, const std::vector<Value>& args,
                                       Object* prototype, Object* arrayBufferPrototype);
};

} // namespace Zepra::Builtins
