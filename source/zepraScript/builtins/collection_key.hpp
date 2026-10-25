// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
#pragma once

/**
 * @file collection_key.hpp
 * @brief Hash key for Map and Set: ECMAScript SameValueZero
 */

#include "runtime/builtins_api/bigint_object.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

namespace Zepra::Builtins {

/**
 * Two values have equal keys exactly when SameValueZero says they are the
 * same: NaN equals NaN, +0 equals -0, strings and BigInts compare by content,
 * everything else by identity.
 */
struct CollectionKey {
    enum class Kind : uint8_t { Undefined, Null, Boolean, Number, String, BigInt, Symbol, Object };

    Kind kind = Kind::Undefined;
    uint64_t bits = 0;   // boolean, number bit pattern, symbol id or object address
    std::string text;    // string or BigInt digits

    static CollectionKey of(const Runtime::Value& value) {
        CollectionKey key;
        if (value.isUndefined()) { key.kind = Kind::Undefined; }
        else if (value.isNull()) { key.kind = Kind::Null; }
        else if (value.isBoolean()) { key.kind = Kind::Boolean; key.bits = value.asBoolean() ? 1 : 0; }
        else if (value.isNumber()) {
            key.kind = Kind::Number;
            double number = value.asNumber();
            if (std::isnan(number)) number = std::nan("");
            if (number == 0) number = 0;  // -0 becomes +0
            std::memcpy(&key.bits, &number, sizeof(number));
        }
        else if (value.isString()) { key.kind = Kind::String; key.text = value.asString()->value(); }
        else if (value.isSymbol()) { key.kind = Kind::Symbol; key.bits = value.asSymbol(); }
        else if (value.isBigInt()) { key.kind = Kind::BigInt; key.text = value.asBigInt()->value().toString(); }
        else {
            key.kind = Kind::Object;
            key.bits = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value.asObject()));
        }
        return key;
    }

    bool operator==(const CollectionKey& other) const {
        return kind == other.kind && bits == other.bits && text == other.text;
    }
};

struct CollectionKeyHash {
    size_t operator()(const CollectionKey& key) const {
        size_t h = std::hash<uint64_t>()(key.bits) ^ (static_cast<size_t>(key.kind) * 0x9E3779B97F4A7C15ull);
        if (!key.text.empty()) h ^= std::hash<std::string>()(key.text) + 0x9E3779B9 + (h << 6) + (h >> 2);
        return h;
    }
};

} // namespace Zepra::Builtins
