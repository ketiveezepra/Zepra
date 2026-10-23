// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
#pragma once

/**
 * @file js_error.hpp
 * @brief A C++ exception that becomes a typed JavaScript error
 *
 * Built-ins throw JSError("RangeError", ...) when the specification calls
 * for a specific error type. The interpreter turns it into an instance of
 * that type's constructor, which a script's try/catch can catch. A plain
 * std::runtime_error from native code becomes a TypeError.
 */

#include <stdexcept>
#include <string>

namespace Zepra::Runtime {

class JSError : public std::runtime_error {
public:
    JSError(std::string type, const std::string& message)
        : std::runtime_error(message), type_(std::move(type)) {}

    /** "TypeError", "RangeError", "SyntaxError", ... */
    const std::string& type() const { return type_; }

private:
    std::string type_;
};

} // namespace Zepra::Runtime
