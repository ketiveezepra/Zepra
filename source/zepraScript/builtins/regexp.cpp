// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file regexp.cpp
 * @brief JavaScript RegExp builtin — backed by Zepra::Regex NFA engine
 */

#include "builtins/regexp.hpp"
#include "runtime/execution/js_error.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/string_utf16.hpp"
#include "builtins/builtin_helpers.hpp"

#include <functional>

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

std::string RegExpObject::canonicalFlags() const {
    std::string out;
    for (char f : std::string("dgimsuvy")) {
        if (flagStr_.find(f) != std::string::npos) out += f;
    }
    return out;
}

bool RegExpObject::test(const std::string& str) {
    return !exec(str).isNull();
}

Value RegExpObject::exec(const std::string& str) {
    if (!valid_) return Value::null();
    const bool usesLastIndex = flags_.global || flags_.sticky;
    const bool ascii = Runtime::Utf16::isAscii(str);
    size_t byteStart = 0;
    if (usesLastIndex) {
        if (lastIndex_ < 0) lastIndex_ = 0;
        const size_t unit = static_cast<size_t>(lastIndex_);
        if (unit > (ascii ? str.size() : Runtime::Utf16::length(str))) {
            lastIndex_ = 0;
            return Value::null();
        }
        byteStart = ascii ? unit : Runtime::Utf16::byteOffsetOfUnit(str, unit);
    }
    size_t matchEnd = 0;
    Value result = execAt(str, byteStart, ascii, nullptr, &matchEnd);
    if (usesLastIndex) {
        lastIndex_ = result.isNull() ? 0
                   : static_cast<int>(ascii ? matchEnd : Runtime::Utf16::unitOffsetOfByte(str, matchEnd));
    }
    return result;
}

Value RegExpObject::execAt(const std::string& str, size_t byteStart, bool ascii,
                           size_t* matchByteStart, size_t* matchByteEnd, Value input) {
    if (!valid_ || byteStart > str.size()) return Value::null();

    Regex::RegexEngine engine(program_, flags_);
    auto result = engine.exec(str, static_cast<int32_t>(byteStart));
    if (!result.success) return Value::null();
    if (flags_.sticky && static_cast<size_t>(result.matchStart) != byteStart) return Value::null();

    if (matchByteStart) *matchByteStart = static_cast<size_t>(result.matchStart);
    if (matchByteEnd) *matchByteEnd = static_cast<size_t>(result.matchEnd);

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

    const auto units = [&](size_t byte) {
        return static_cast<double>(ascii ? byte : Runtime::Utf16::unitOffsetOfByte(str, byte));
    };
    static_cast<Runtime::Object*>(arr)->set("index", Value::number(units(result.matchStart)));
    static_cast<Runtime::Object*>(arr)->set("input", input.isString() ? input : Value::string(new Runtime::String(str)));

    // Named groups: { name: capture } (undefined without named groups)
    Value groups = Value::undefined();
    const auto& names = program_.groupNames;
    for (size_t i = 1; i < names.size(); ++i) {
        if (names[i].empty()) continue;
        if (groups.isUndefined()) {
            auto* object = new Object();
            object->setPrototype(nullptr);
            groups = Value::object(object);
        }
        groups.asObject()->set(names[i], i < arr->length() ? arr->at(i) : Value::undefined());
    }
    static_cast<Runtime::Object*>(arr)->set("groups", groups);

    // `d` flag: indices[i] = [start, end] per group
    if (flags_.hasIndices) {
        auto* indices = new Runtime::Array();
        indices->push(Value::object(new Runtime::Array({Value::number(units(result.matchStart)),
                                                        Value::number(units(result.matchEnd))})));
        for (size_t i = 1; i < result.captures.size(); ++i) {
            const auto& capture = result.captures[i];
            if (capture.matched()) {
                indices->push(Value::object(new Runtime::Array({Value::number(units(capture.start)),
                                                                Value::number(units(capture.end))})));
            } else {
                indices->push(Value::undefined());
            }
        }
        static_cast<Runtime::Object*>(indices)->set("groups", Value::undefined());
        static_cast<Runtime::Object*>(arr)->set("indices", Value::object(indices));
    }

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

// The pattern as RegExp.prototype.source shows it: "/" and line breaks
// escaped so that `/${source}/` reads back as the same literal
static std::string escapedSource(const std::string& pattern) {
    if (pattern.empty()) return "(?:)";
    std::string out;
    bool inClass = false;
    for (size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '\\' && i + 1 < pattern.size()) {
            out += c;
            out += pattern[++i];
            continue;
        }
        if (c == '[') inClass = true;
        else if (c == ']') inClass = false;
        if (c == '/' && !inClass) out += "\\/";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
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

    // Flag and source accessors: RegExp.prototype.global etc. read the
    // receiver's internal flags; on RegExp.prototype itself they are undefined
    // ("(?:)" and "" for source and flags)
    auto defineGetter = [proto](const std::string& name, std::function<Value(RegExpObject*)> read,
                                Value onPrototype) {
        Runtime::PropertyDescriptor desc;
        desc.getter = Value::object(new Runtime::Function("get " + name,
            [read, onPrototype](const Runtime::FunctionCallInfo& info) -> Value {
                RegExpObject* re = info.thisValue().isObject()
                    ? dynamic_cast<RegExpObject*>(info.thisValue().asObject()) : nullptr;
                if (!re) return onPrototype;
                return read(re);
            }, 0));
        desc.attributes = Runtime::PropertyAttribute::Configurable;
        proto->defineProperty(name, desc);
    };
    auto flag = [](bool (RegExpObject::*get)() const) {
        return [get](RegExpObject* re) { return Value::boolean((re->*get)()); };
    };
    defineGetter("global", flag(&RegExpObject::global), Value::undefined());
    defineGetter("ignoreCase", flag(&RegExpObject::ignoreCase), Value::undefined());
    defineGetter("multiline", flag(&RegExpObject::multiline), Value::undefined());
    defineGetter("dotAll", flag(&RegExpObject::dotAll), Value::undefined());
    defineGetter("unicode", flag(&RegExpObject::unicode), Value::undefined());
    defineGetter("unicodeSets", flag(&RegExpObject::unicodeSets), Value::undefined());
    defineGetter("sticky", flag(&RegExpObject::sticky), Value::undefined());
    defineGetter("hasIndices", flag(&RegExpObject::hasIndices), Value::undefined());
    defineGetter("flags", [](RegExpObject* re) {
        return Value::string(new Runtime::String(re->canonicalFlags()));
    }, Value::string(new Runtime::String("")));
    defineGetter("source", [](RegExpObject* re) {
        return Value::string(new Runtime::String(escapedSource(re->pattern())));
    }, Value::string(new Runtime::String("(?:)")));

    // lastIndex (an own data property in the spec; an accessor over the
    // internal value here, so the engine and scripts share one value)
    {
        Runtime::PropertyDescriptor desc;
        desc.getter = Value::object(new Runtime::Function("get lastIndex",
            [](const Runtime::FunctionCallInfo& info) -> Value {
                RegExpObject* re = info.thisValue().isObject()
                    ? dynamic_cast<RegExpObject*>(info.thisValue().asObject()) : nullptr;
                return re ? Value::number(re->lastIndex()) : Value::undefined();
            }, 0));
        desc.setter = Value::object(new Runtime::Function("set lastIndex",
            [](const Runtime::FunctionCallInfo& info) -> Value {
                RegExpObject* re = info.thisValue().isObject()
                    ? dynamic_cast<RegExpObject*>(info.thisValue().asObject()) : nullptr;
                if (re) re->setLastIndex(integerArgument(info.argument(0), 0));
                return Value::undefined();
            }, 1));
        desc.attributes = Runtime::PropertyAttribute::Configurable;
        proto->defineProperty("lastIndex", desc);
    }

    proto->set("toString", Value::object(
        new Runtime::Function("toString", [](const Runtime::FunctionCallInfo& info) -> Value {
            if (!info.thisValue().isObject()) return Value::undefined();
            RegExpObject* re = dynamic_cast<RegExpObject*>(info.thisValue().asObject());
            if (!re) return Value::undefined();
            std::string result = "/" + escapedSource(re->pattern()) + "/" + re->canonicalFlags();
            return Value::string(new Runtime::String(result));
        }, 0)));

    return proto;
}

} // namespace Zepra::Builtins
