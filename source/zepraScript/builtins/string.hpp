#pragma once

/**
 * @file string.hpp
 * @brief String built-ins
 *
 * Strings are UTF-8 internally and every index and length here counts bytes,
 * not UTF-16 code units (a known limitation). Case mapping covers ASCII,
 * Latin-1, Latin Extended-A, Greek and Cyrillic; localeCompare compares code
 * points; normalize is not provided.
 */

#include "../config.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"

namespace Zepra::Runtime { class Context; }

namespace Zepra::Builtins {

class StringBuiltin {
public:
    /** String.prototype and its methods, including the RegExp-aware replace, split, match, matchAll and search. */
    static Runtime::Object* createStringPrototype(Runtime::Context* ctx);

    /** String.fromCharCode, String.fromCodePoint and String.raw. */
    static void installStatics(Runtime::Function* constructor);
};

} // namespace Zepra::Builtins
