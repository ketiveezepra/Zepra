// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file regexp.cpp
 * @brief JavaScript RegExp builtin — backed by Zepra::Regex NFA engine
 */

#include "builtins/regexp.hpp"
#include "runtime/execution/js_error.hpp"
#include "runtime/objects/function.hpp"

namespace Zepra::Builtins {

// =============================================================================
// RegExpObject
// =============================================================================

RegExpObject::RegExpObject(const std::string& pattern, const std::string& flags)
    : Object(ObjectType::RegExp)
    , pattern_(pattern)
    , flagStr_(flags) {

    for (char f : flags) {
        switch (f) {
            case 'g': flags_.global = true; break;
            case 'i': flags_.ignoreCase = true; break;
            case 'm': flags_.multiline = true; break;
            case 's': flags_.dotAll = true; break;
            case 'u': flags_.unicode = true; break;
            case 'y': flags_.sticky = true; break;
            case 'd': flags_.hasIndices = true; break;
        }
    }

    Regex::RegexCompiler compiler(flags_);
    valid_ = compiler.compile(pattern);
    if (valid_) {
        program_ = compiler.takeProgram();
    }
}

bool RegExpObject::test(const std::string& str) {
    if (!valid_) return false;

    Regex::RegexEngine engine(program_, flags_);
    int32_t start = flags_.sticky ? lastIndex_ : 0;
    auto result = engine.test(str, start);

    if (result && (flags_.global || flags_.sticky)) {
        Regex::RegexEngine engine2(program_, flags_);
        auto match = engine2.exec(str, start);
        lastIndex_ = match.success ? match.matchEnd : 0;
    } else if (!result && (flags_.global || flags_.sticky)) {
        lastIndex_ = 0;
    }

    return result;
}

Value RegExpObject::exec(const std::string& str) {
    if (!valid_) return Value::null();

    Regex::RegexEngine engine(program_, flags_);
    int32_t start = (flags_.global || flags_.sticky) ? lastIndex_ : 0;
    auto result = engine.exec(str, start);

    if (!result.success) {
        if (flags_.global || flags_.sticky) lastIndex_ = 0;
        return Value::null();
    }

    if (flags_.global || flags_.sticky) {
        lastIndex_ = result.matchEnd;
    }

    Runtime::Array* arr = new Runtime::Array();
    // Group 0 = full match
    arr->push(Value::string(new Runtime::String(
        str.substr(result.matchStart, result.matchEnd - result.matchStart))));

    // Capture groups
    for (size_t i = 1; i < result.captures.size(); ++i) {
        if (result.captures[i].matched()) {
            arr->push(Value::string(new Runtime::String(
                result.captures[i].extract(str))));
        } else {
            arr->push(Value::undefined());
        }
    }

    // Set index and input properties
    static_cast<Runtime::Object*>(arr)->set("index", Value::number(static_cast<double>(result.matchStart)));
    static_cast<Runtime::Object*>(arr)->set("input", Value::string(new Runtime::String(str)));
    static_cast<Runtime::Object*>(arr)->set("groups", Value::undefined());

    return Value::object(arr);
}

std::string RegExpObject::replace(const std::string& str, const std::string& replacement) {
    if (!valid_) return str;
    Regex::RegexEngine engine(program_, flags_);
    if (flags_.global) {
        return engine.replaceAll(str, replacement);
    }
    return engine.replace(str, replacement);
}

std::string RegExpObject::replaceAll(const std::string& str, const std::string& replacement) {
    if (!valid_) return str;
    Regex::RegexEngine engine(program_, flags_);
    return engine.replaceAll(str, replacement);
}

std::vector<std::string> RegExpObject::match(const std::string& str) {
    std::vector<std::string> result;
    if (!valid_) return result;

    Regex::RegexEngine engine(program_, flags_);

    if (flags_.global) {
        auto matches = engine.matchAll(str);
        for (const auto& m : matches) {
            result.push_back(str.substr(m.matchStart, m.matchEnd - m.matchStart));
        }
    } else {
        auto m = engine.exec(str, 0);
        if (m.success) {
            result.push_back(str.substr(m.matchStart, m.matchEnd - m.matchStart));
        }
    }

    return result;
}

int RegExpObject::search(const std::string& str) {
    if (!valid_) return -1;
    Regex::RegexEngine engine(program_, flags_);
    auto result = engine.exec(str, 0);
    return result.success ? result.matchStart : -1;
}

std::vector<std::string> RegExpObject::split(const std::string& str, int limit) {
    if (!valid_) return { str };
    Regex::RegexEngine engine(program_, flags_);
    return engine.split(str, limit);
}

// =============================================================================
// RegExpBuiltin
// =============================================================================

Value RegExpBuiltin::constructor(Runtime::Context*, const std::vector<Value>& args) {
    std::string pattern;
    std::string flags;

    if (!args.empty()) {
        // new RegExp(existingRegExp) copies its source and flags
        if (args[0].isObject()) {
            if (auto* existing = dynamic_cast<RegExpObject*>(args[0].asObject())) {
                pattern = existing->pattern();
                flags = existing->flags();
            } else {
                pattern = args[0].toString();
            }
        } else if (!args[0].isUndefined()) {
            pattern = args[0].toString();
        }
    }
    if (args.size() > 1 && !args[1].isUndefined()) {
        flags = args[1].toString();
    }
    if (pattern.empty()) pattern = "(?:)";

    for (size_t i = 0; i < flags.size(); ++i) {
        if (std::string("dgimsuvy").find(flags[i]) == std::string::npos || flags.find(flags[i]) != i) {  // unknown or repeated
            throw Runtime::JSError("SyntaxError", "Invalid flags supplied to RegExp constructor '" + flags + "'");
        }
    }

    // Regex literals evaluate to `new RegExp(...)`. A pattern this engine cannot
    // compile (lookbehind, named groups, ...) yields a RegExp that never
    // matches, rather than throwing and aborting the page's whole script.
    return Value::object(new RegExpObject(pattern, flags));
}

Value RegExpBuiltin::test(Runtime::Context*, const std::vector<Value>& args) {
    if (args.size() < 2 || !args[0].isObject()) return Value::boolean(false);

    RegExpObject* re = dynamic_cast<RegExpObject*>(args[0].asObject());
    if (!re) return Value::boolean(false);

    std::string str;
    if (args[1].isString()) {
        str = static_cast<Runtime::String*>(args[1].asObject())->value();
    }

    return Value::boolean(re->test(str));
}

Value RegExpBuiltin::exec(Runtime::Context*, const std::vector<Value>& args) {
    if (args.size() < 2 || !args[0].isObject()) return Value::null();

    RegExpObject* re = dynamic_cast<RegExpObject*>(args[0].asObject());
    if (!re) return Value::null();

    std::string str;
    if (args[1].isString()) {
        str = static_cast<Runtime::String*>(args[1].asObject())->value();
    }

    return re->exec(str);
}

Object* RegExpBuiltin::createRegExpPrototype() {
    Object* proto = new Object();

    proto->set("test", Value::object(
        new Runtime::Function("test", [](const Runtime::FunctionCallInfo& info) -> Value {
            if (!info.thisValue().isObject()) return Value::boolean(false);
            RegExpObject* re = dynamic_cast<RegExpObject*>(info.thisValue().asObject());
            if (!re || info.argumentCount() < 1) return Value::boolean(false);
            std::string str;
            if (info.argument(0).isString()) {
                str = static_cast<Runtime::String*>(info.argument(0).asObject())->value();
            } else {
                str = info.argument(0).toString();
            }
            return Value::boolean(re->test(str));
        }, 1)));

    proto->set("exec", Value::object(
        new Runtime::Function("exec", [](const Runtime::FunctionCallInfo& info) -> Value {
            if (!info.thisValue().isObject()) return Value::null();
            RegExpObject* re = dynamic_cast<RegExpObject*>(info.thisValue().asObject());
            if (!re || info.argumentCount() < 1) return Value::null();
            std::string str;
            if (info.argument(0).isString()) {
                str = static_cast<Runtime::String*>(info.argument(0).asObject())->value();
            } else {
                str = info.argument(0).toString();
            }
            return re->exec(str);
        }, 1)));

    proto->set("toString", Value::object(
        new Runtime::Function("toString", [](const Runtime::FunctionCallInfo& info) -> Value {
            if (!info.thisValue().isObject()) return Value::undefined();
            RegExpObject* re = dynamic_cast<RegExpObject*>(info.thisValue().asObject());
            if (!re) return Value::undefined();
            std::string result = "/" + re->pattern() + "/" + re->flags();
            return Value::string(new Runtime::String(result));
        }, 0)));

    return proto;
}

} // namespace Zepra::Builtins
