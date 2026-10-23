// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
#pragma once

/**
 * @file global_functions.hpp
 * @brief Global functions: URI encoding, base64 (atob/btoa), queueMicrotask
 */

#include "runtime/objects/function.hpp"
#include <string>

namespace Zepra::Builtins {

using Runtime::Function;

class GlobalFunctions {
public:
    /** encodeURI / encodeURIComponent / decodeURI / decodeURIComponent (RFC 3986 on UTF-8). */
    static Function* createEncodeURI();
    static Function* createEncodeURIComponent();
    static Function* createDecodeURI();
    static Function* createDecodeURIComponent();

    /** btoa(binaryString) and atob(base64): the WHATWG forgiving-base64 algorithms. */
    static Function* createBtoa();
    static Function* createAtob();

    /** queueMicrotask(callback): runs after the current script, before timers. */
    static Function* createQueueMicrotask();

    /** The URI functions as plain strings, for embedders and tests. Throw JSError("URIError"). */
    static std::string encodeUri(const std::string& text, bool component);
    static std::string decodeUri(const std::string& text, bool component);
};

} // namespace Zepra::Builtins
