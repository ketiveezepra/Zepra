// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

/**
 * @file typed_array_tests.cpp
 * @brief ArrayBuffer and TypedArray: the C++ view API and the JS constructors
 */

#include <gtest/gtest.h>

#include "builtins/typed_array.hpp"
#include "runtime/execution/context.hpp"
#include "runtime/execution/global_object.hpp"
#include "runtime/execution/vm.hpp"

#include <cmath>
#include <cstring>
#include <string>

using namespace Zepra::Builtins;
using namespace Zepra::Runtime;

namespace {

// Runs `source` in a fresh VM and returns the global `r`
class TypedArrayScript : public ::testing::Test {
protected:
    TypedArrayScript() : context_(&vm_) {
        global_ = new GlobalObject(&context_);
        global_->initialize();
        global_->installInto(vm_);
    }

    Value run(const std::string& source) {
        void* chunk = vm_.compile("var r;\n" + source, "typed_array_test");
        EXPECT_NE(chunk, nullptr) << source;
        if (!chunk) return Value::undefined();
        ExecutionResult result = vm_.execute(static_cast<const Zepra::Bytecode::BytecodeChunk*>(chunk));
        EXPECT_EQ(result.status, ExecutionResult::Status::Success) << result.error;
        return vm_.getGlobal("r");
    }

    TypedArrayObject* typedArray(const Value& value) {
        return value.isObject() ? dynamic_cast<TypedArrayObject*>(value.asObject()) : nullptr;
    }

    VM vm_{nullptr};
    Context context_;
    GlobalObject* global_ = nullptr;
};

} // namespace

// =============================================================================
// C++ view API
// =============================================================================

TEST(TypedArrayView, IndexedAccessGoesThroughTheBuffer) {
    auto* buffer = new ArrayBufferObject(8);
    TypedArrayObject f32(buffer, TypedArrayType::Float32, 0, 2);
    Object& asObject = f32;

    EXPECT_TRUE(asObject.set(1u, Value::number(1.5)));
    float stored;
    std::memcpy(&stored, buffer->data() + 4, 4);
    EXPECT_EQ(stored, 1.5f);
    EXPECT_EQ(asObject.get(1u).asNumber(), 1.5);
    EXPECT_EQ(asObject.length(), 2u);
    EXPECT_TRUE(asObject.has(1u));
    EXPECT_FALSE(asObject.has(2u));

    // String keys that are indices land in the buffer too
    asObject.set(std::string("0"), Value::number(-2));
    EXPECT_EQ(asObject.get(std::string("0")).asNumber(), -2);
    EXPECT_EQ(asObject.get(std::string("length")).asNumber(), 2);

    // Out-of-range writes are ignored, reads are undefined
    asObject.set(5u, Value::number(9));
    EXPECT_TRUE(asObject.get(5u).isUndefined());
    EXPECT_EQ(buffer->byteLength(), 8u);
}

TEST(TypedArrayView, ConversionsFollowTheSpec) {
    auto* buffer = new ArrayBufferObject(16);
    TypedArrayObject i8(buffer, TypedArrayType::Int8, 0, 4);
    TypedArrayObject u8c(buffer, TypedArrayType::Uint8Clamped, 4, 4);
    TypedArrayObject u32(buffer, TypedArrayType::Uint32, 8, 2);

    i8.set(0u, Value::number(128));
    i8.set(1u, Value::number(-129));
    i8.set(2u, Value::number(NAN));
    i8.set(3u, Value::number(-1.9));
    EXPECT_EQ(i8.get(0u).asNumber(), -128);
    EXPECT_EQ(i8.get(1u).asNumber(), 127);
    EXPECT_EQ(i8.get(2u).asNumber(), 0);
    EXPECT_EQ(i8.get(3u).asNumber(), -1);

    u8c.set(0u, Value::number(300));
    u8c.set(1u, Value::number(-3));
    u8c.set(2u, Value::number(2.5));   // ties round to even
    u8c.set(3u, Value::number(3.5));
    EXPECT_EQ(u8c.get(0u).asNumber(), 255);
    EXPECT_EQ(u8c.get(1u).asNumber(), 0);
    EXPECT_EQ(u8c.get(2u).asNumber(), 2);
    EXPECT_EQ(u8c.get(3u).asNumber(), 4);

    u32.set(0u, Value::number(-1));
    u32.set(1u, Value::number(4294967296.0 + 7));
    EXPECT_EQ(u32.get(0u).asNumber(), 4294967295.0);
    EXPECT_EQ(u32.get(1u).asNumber(), 7);
}

TEST(TypedArrayView, SubarraySharesTheBuffer) {
    auto* buffer = new ArrayBufferObject(16);
    TypedArrayObject i32(buffer, TypedArrayType::Int32, 0, 4);
    TypedArrayObject* tail = i32.subarray(2, 4);
    ASSERT_NE(tail, nullptr);
    EXPECT_EQ(tail->length(), 2u);
    EXPECT_EQ(tail->byteOffset(), 8u);
    tail->set(0u, Value::number(42));
    EXPECT_EQ(i32.get(2u).asNumber(), 42);
}

TEST(TypedArrayView, ElementSizesAndNames) {
    EXPECT_EQ(bytesPerElement(TypedArrayType::Uint8Clamped), 1u);
    EXPECT_EQ(bytesPerElement(TypedArrayType::Int16), 2u);
    EXPECT_EQ(bytesPerElement(TypedArrayType::Float32), 4u);
    EXPECT_EQ(bytesPerElement(TypedArrayType::Float64), 8u);
    EXPECT_STREQ(typedArrayName(TypedArrayType::Uint8Clamped), "Uint8ClampedArray");
}

// =============================================================================
// JS constructors
// =============================================================================

TEST_F(TypedArrayScript, ConstructFromArray) {
    TypedArrayObject* ta = typedArray(run("r = new Float32Array([1, 2.5, -3]);"));
    ASSERT_NE(ta, nullptr);
    EXPECT_EQ(ta->type(), TypedArrayType::Float32);
    ASSERT_EQ(ta->length(), 3u);
    float values[3];
    std::memcpy(values, ta->data(), sizeof values);
    EXPECT_EQ(values[0], 1.0f);
    EXPECT_EQ(values[1], 2.5f);
    EXPECT_EQ(values[2], -3.0f);
}

TEST_F(TypedArrayScript, IndexAndLengthFromScript) {
    Value r = run("var a = new Uint16Array(3); a[0] = 7; a[2] = 70000; a[3] = 1;"
                  "r = a.length * 1000000 + a[0] * 1000 + (a[2] === 4464 ? 1 : 0) + (a[3] === undefined ? 10 : 0);");
    EXPECT_EQ(r.toNumber(), 3007011.0);
}

TEST_F(TypedArrayScript, ConstructFromIterableAndTypedArray) {
    Value r = run("var a = new Int32Array(new Set([4, 5, 6]));"
                  "var b = new Float64Array(a);"
                  "r = b.length === 3 && b[2] === 6 && b instanceof Float64Array && b.buffer !== a.buffer;");
    EXPECT_TRUE(r.isBoolean() && r.asBoolean());
}

TEST_F(TypedArrayScript, ViewOfArrayBuffer) {
    TypedArrayObject* ta = typedArray(run("var buf = new ArrayBuffer(12); r = new Uint8ClampedArray(buf, 4);"
                                          "r[0] = 999;"));
    ASSERT_NE(ta, nullptr);
    EXPECT_EQ(ta->type(), TypedArrayType::Uint8Clamped);
    EXPECT_EQ(ta->length(), 8u);
    EXPECT_EQ(ta->byteOffset(), 4u);
    EXPECT_EQ(ta->buffer()->data()[4], 255);
}

TEST_F(TypedArrayScript, MisalignedViewThrowsRangeError) {
    Value r = run("try { new Float32Array(new ArrayBuffer(8), 2); r = 'no'; } catch (e) { r = e instanceof RangeError; }");
    EXPECT_TRUE(r.isBoolean() && r.asBoolean());
}

TEST_F(TypedArrayScript, ArrayMethodsOnTypedArrays) {
    Value r = run("var a = Uint8Array.of(3, 1, 2);"
                  "r = a.sort().join('-') + '|' + Array.from(a.map(function(x){ return x * 100; })).join();");
    EXPECT_EQ(r.toString(), "1-2-3|100,200,44");
}
