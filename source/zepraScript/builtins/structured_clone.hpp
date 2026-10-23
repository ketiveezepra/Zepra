// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
#pragma once

/**
 * @file structured_clone.hpp
 * @brief structuredClone(value): the HTML structured clone algorithm
 */

#include "runtime/objects/function.hpp"

namespace Zepra::Builtins {

class StructuredClone {
public:
    /**
     * structuredClone(value): a deep copy that keeps shared references and
     * cycles. Copies primitives, boxed primitives, Date, RegExp, Error,
     * Array, Map, Set and plain objects (own enumerable string keys, read
     * with [[Get]]). Functions, symbols, proxies and other host objects throw a
     * DataCloneError; typed arrays and ArrayBuffer are not supported yet and
     * throw one too.
     */
    static Runtime::Function* createFunction();
};

} // namespace Zepra::Builtins
