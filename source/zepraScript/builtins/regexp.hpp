#pragma once
// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file regexp.hpp
 * @brief JavaScript RegExp builtin — backed by Zepra::Regex NFA engine
 */

#include "../config.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"
#include "regex/regex_bytecode.h"
#include "regex/regex_compiler.h"
#include "regex/regex_engine.h"
#include <string>
#include <vector>

namespace Zepra::Runtime { class Context; }

namespace Zepra::Builtins {

using Runtime::Value;
using Runtime::Object;
using Runtime::ObjectType;

class RegExpObject : public Object {
public:
    RegExpObject(const std::string& pattern, const std::string& flags = "");

    bool test(const std::string& str);
    Value exec(const std::string& str);
    // One match at or after byte offset `byteStart` of WTF-8 `str` (ignores
    // and keeps lastIndex). The result array's index is in UTF-16 code units;
    // the match's byte range is returned through the out parameters.
    // `input` (optional) is `str` as a string value, reused for the result's
    // input property instead of copying the text once per match
    Value execAt(const std::string& str, size_t byteStart, bool ascii,
                 size_t* matchByteStart = nullptr, size_t* matchByteEnd = nullptr,
                 Value input = Value::undefined());

    const std::string& pattern() const { return pattern_; }
    const std::string& flags() const { return flagStr_; }
    bool global() const { return flags_.global; }
    bool ignoreCase() const { return flags_.ignoreCase; }
    bool multiline() const { return flags_.multiline; }
    bool dotAll() const { return flags_.dotAll; }
    bool unicode() const { return flags_.unicode; }
    bool sticky() const { return flags_.sticky; }
    bool hasIndices() const { return flags_.hasIndices; }
    bool unicodeSets() const { return flagStr_.find('v') != std::string::npos; }
    // Flags in canonical order ("dgimsuvy"), as RegExp.prototype.flags gives them
    std::string canonicalFlags() const;
    // In UTF-16 code units, as JavaScript sees it
    int lastIndex() const { return lastIndex_; }
    void setLastIndex(int idx) { lastIndex_ = idx; }
    bool isValid() const { return valid_; }

    std::string replace(const std::string& str, const std::string& replacement);
    std::string replaceAll(const std::string& str, const std::string& replacement);
    std::vector<std::string> match(const std::string& str);
    int search(const std::string& str);
    std::vector<std::string> split(const std::string& str, int limit = -1);

private:
    std::string pattern_;
    std::string flagStr_;
    Regex::RegexFlags flags_;
    Regex::RegexProgram program_;
    bool valid_ = false;
    int lastIndex_ = 0;
};

class RegExpBuiltin {
public:
    static Value constructor(Runtime::Context* ctx, const std::vector<Value>& args);
    static Value test(Runtime::Context* ctx, const std::vector<Value>& args);
    static Value exec(Runtime::Context* ctx, const std::vector<Value>& args);

    static Object* createRegExpPrototype();
};

} // namespace Zepra::Builtins
