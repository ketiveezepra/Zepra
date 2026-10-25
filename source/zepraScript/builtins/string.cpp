// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file string.cpp
 * @brief String built-ins
 */

#include "builtins/string.hpp"
#include "builtins/builtin_helpers.hpp"
#include "builtins/list_iterator.hpp"
#include "builtins/regexp.hpp"
#include "runtime/execution/vm.hpp"
#include "runtime/objects/number_conversion.hpp"
#include "runtime/objects/well_known_symbols.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

namespace Zepra::Builtins {

using Runtime::Array;
using Runtime::FunctionCallInfo;
using Runtime::JSError;
using Runtime::String;
using Runtime::VM;

namespace {

constexpr size_t kMaxStringLength = size_t(1) << 29;

// ---- UTF-8 ------------------------------------------------------------------

size_t sequenceLength(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead >> 5) == 0x6) return 2;
    if ((lead >> 4) == 0xE) return 3;
    if ((lead >> 3) == 0x1E) return 4;
    return 1;  // stray continuation byte
}

bool isContinuation(unsigned char byte) { return (byte & 0xC0) == 0x80; }

// The code point that starts at text[i]; `length` receives the bytes it uses
uint32_t decodeAt(const std::string& text, size_t i, size_t& length) {
    const auto byte = [&](size_t k) { return static_cast<unsigned char>(text[k]); };
    length = std::min(sequenceLength(byte(i)), text.size() - i);
    if (length == 1) return byte(i);
    uint32_t code = byte(i) & (0x7F >> length);
    for (size_t k = 1; k < length; ++k) code = (code << 6) | (byte(i + k) & 0x3F);
    return code;
}

void appendCodePoint(std::string& out, uint32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

// Bytes from `i` to the start of the next code point
size_t stepOver(const std::string& text, size_t i) {
    size_t length;
    decodeAt(text, i, length);
    return length;
}

// ---- case mapping -----------------------------------------------------------

uint32_t lowerCodePoint(uint32_t c) {
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x100 && c <= 0x137 && c != 0x130) return (c % 2 == 0) ? c + 1 : c;
    if (c >= 0x139 && c <= 0x148) return (c % 2 == 1) ? c + 1 : c;
    if (c >= 0x14A && c <= 0x177) return (c % 2 == 0) ? c + 1 : c;
    if (c == 0x178) return 0xFF;
    if (c >= 0x179 && c <= 0x17E) return (c % 2 == 1) ? c + 1 : c;
    if (c == 0x386) return 0x3AC;
    if (c >= 0x388 && c <= 0x38A) return c + 37;
    if (c == 0x38C) return 0x3CC;
    if (c == 0x38E || c == 0x38F) return c + 63;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    return c;
}

uint32_t upperCodePoint(uint32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c == 0x131) return 'I';
    if (c == 0x17F) return 'S';
    if (c >= 0x101 && c <= 0x137) return (c % 2 == 1) ? c - 1 : c;
    if (c >= 0x13A && c <= 0x148) return (c % 2 == 0) ? c - 1 : c;
    if (c >= 0x14B && c <= 0x177) return (c % 2 == 1) ? c - 1 : c;
    if (c >= 0x17A && c <= 0x17E) return (c % 2 == 0) ? c - 1 : c;
    if (c == 0x3AC) return 0x386;
    if (c >= 0x3AD && c <= 0x3AF) return c - 37;
    if (c == 0x3CC) return 0x38C;
    if (c == 0x3CD || c == 0x3CE) return c - 63;
    if (c == 0x3C2) return 0x3A3;
    if (c >= 0x3B1 && c <= 0x3C9) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}

std::string mapCase(const std::string& text, bool upper) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        size_t length;
        const uint32_t code = decodeAt(text, i, length);
        if (upper && code == 0xDF) {
            out += "SS";  // sharp s has no single-letter capital
        } else {
            appendCodePoint(out, upper ? upperCodePoint(code) : lowerCodePoint(code));
        }
        i += length;
    }
    return out;
}

// ---- argument conversion ----------------------------------------------------

// ToString: strings and wrappers directly, other objects through valueOf/toString
std::string toText(const Value& value) {
    if (value.isString()) return value.asString()->value();
    if (value.isObject()) {
        if (auto* wrapper = dynamic_cast<String*>(value.asObject())) return wrapper->value();
        if (VM* vm = VM::current()) return vm->toPrimitiveValue(value, true).toString();
    }
    return value.toString();
}

std::string thisString(const FunctionCallInfo& info, const char* method) {
    const Value self = info.thisValue();
    if (self.isNull() || self.isUndefined()) {
        throw JSError("TypeError", std::string("String.prototype.") + method + " called on null or undefined");
    }
    return toText(self);
}

// ToIntegerOrInfinity
double integerOf(const Value& value) {
    double d = value.toNumber();
    return std::isnan(d) ? 0 : std::trunc(d);
}

// A position argument clamped into [0, length]
size_t clampPosition(const Value& value, size_t length, size_t fallback) {
    if (value.isUndefined()) return fallback;
    double d = integerOf(value);
    if (d < 0) return 0;
    return d > static_cast<double>(length) ? length : static_cast<size_t>(d);
}

// A slice-style index: negative counts from the end
size_t relativePosition(const Value& value, size_t length, size_t fallback) {
    if (value.isUndefined()) return fallback;
    double d = integerOf(value);
    if (d < 0) d = std::max(0.0, static_cast<double>(length) + d);
    return d > static_cast<double>(length) ? length : static_cast<size_t>(d);
}

RegExpObject* asRegExp(const Value& value) {
    return value.isObject() ? dynamic_cast<RegExpObject*>(value.asObject()) : nullptr;
}

std::string substringOf(const std::string& text, size_t from, size_t to) {
    return from < to ? text.substr(from, to - from) : std::string();
}

// ---- regular expression matching -------------------------------------------

struct Match {
    size_t index = 0;
    std::string matched;
    std::vector<Value> captures;  // groups 1..n, undefined when they did not take part
    Value result;                 // the exec() array
};

// First match at or after `from`. `re` must be global so that lastIndex applies.
bool findMatch(RegExpObject& re, const std::string& text, size_t from, Match& out) {
    if (from > text.size()) return false;
    re.setLastIndex(static_cast<int>(from));
    const Value result = re.exec(text);
    if (!result.isObject()) return false;
    auto* array = static_cast<Array*>(result.asObject());
    out.result = result;
    out.matched = array->get(static_cast<uint32_t>(0)).toString();
    out.index = static_cast<size_t>(array->get("index").toNumber());
    const auto& elements = array->elements();
    out.captures.assign(elements.begin() + 1, elements.end());
    return true;
}

// Every match, left to right; empty matches advance by one code point
std::vector<Match> allMatches(RegExpObject& re, const std::string& text) {
    std::vector<Match> matches;
    size_t from = 0;
    Match match;
    while (findMatch(re, text, from, match)) {
        const size_t end = match.index + match.matched.size();
        from = match.matched.empty() ? (match.index < text.size() ? match.index + stepOver(text, match.index)
                                                                   : text.size() + 1)
                                     : end;
        matches.push_back(std::move(match));
        match = Match();
    }
    re.setLastIndex(0);
    return matches;
}

// A global copy of `re` for scanning, or `re` itself when it already is global
class Scanner {
public:
    explicit Scanner(RegExpObject* re) {
        if (re->global()) {
            re_ = re;
        } else {
            std::string flags = re->flags();
            flags.erase(std::remove(flags.begin(), flags.end(), 'y'), flags.end());
            owned_ = std::make_unique<RegExpObject>(re->pattern(), flags + "g");
            re_ = owned_.get();
        }
    }
    RegExpObject& get() { return *re_; }

private:
    RegExpObject* re_;
    std::unique_ptr<RegExpObject> owned_;
};

std::unique_ptr<RegExpObject> compileRegExp(const std::string& pattern, const std::string& flags) {
    auto re = std::make_unique<RegExpObject>(pattern, flags);
    if (!re->isValid()) {
        throw JSError("SyntaxError", "Invalid regular expression: /" + pattern + "/: pattern is not valid");
    }
    return re;
}

// $$, $&, $`, $', $1..$99 in a replacement string
std::string expandReplacement(const std::string& pattern, const std::string& text, size_t position,
                              const std::string& matched, const std::vector<Value>& captures) {
    std::string out;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '$' || i + 1 >= pattern.size()) {
            out += pattern[i];
            continue;
        }
        const char next = pattern[i + 1];
        if (next == '$') { out += '$'; ++i; }
        else if (next == '&') { out += matched; ++i; }
        else if (next == '`') { out += text.substr(0, position); ++i; }
        else if (next == '\'') { out += text.substr(std::min(text.size(), position + matched.size())); ++i; }
        else if (next >= '0' && next <= '9') {
            size_t digits = 1;
            size_t number = static_cast<size_t>(next - '0');
            if (i + 2 < pattern.size() && pattern[i + 2] >= '0' && pattern[i + 2] <= '9') {
                const size_t two = number * 10 + static_cast<size_t>(pattern[i + 2] - '0');
                if (two >= 1 && two <= captures.size()) { number = two; digits = 2; }
            }
            if (number >= 1 && number <= captures.size()) {
                const Value& capture = captures[number - 1];
                if (!capture.isUndefined()) out += capture.toString();
                i += digits;
            } else {
                out += '$';
            }
        } else {
            out += '$';
        }
    }
    return out;
}

// What replaces one match: the replacer function's result or the expanded template
std::string replacementFor(const Value& replacer, const std::string& templateText, const std::string& text,
                           size_t position, const std::string& matched, const std::vector<Value>& captures) {
    if (!(replacer.isObject() && replacer.asObject()->isFunction())) {
        return expandReplacement(templateText, text, position, matched, captures);
    }
    VM* vm = VM::current();
    if (!vm) throw JSError("TypeError", "replace called outside a running script");
    std::vector<Value> args;
    args.push_back(makeString(matched));
    args.insert(args.end(), captures.begin(), captures.end());
    args.push_back(Value::number(static_cast<double>(position)));
    args.push_back(makeString(text));
    return toText(vm->executeCallback(static_cast<Runtime::Function*>(replacer.asObject()), Value::undefined(), args));
}

bool isFunction(const Value& value) { return value.isObject() && value.asObject()->isFunction(); }

// ---- the methods ------------------------------------------------------------

Value replaceImpl(const FunctionCallInfo& info, bool all) {
    const char* method = all ? "replaceAll" : "replace";
    const std::string text = thisString(info, method);
    const Value search = info.argument(0);
    const Value replacer = info.argument(1);
    const std::string templateText = isFunction(replacer) ? std::string() : toText(replacer);

    if (RegExpObject* re = asRegExp(search)) {
        if (all && !re->global()) {
            throw JSError("TypeError", "replaceAll must be called with a global RegExp");
        }
        std::vector<Match> matches;
        if (re->global()) {
            matches = allMatches(*re, text);
        } else {
            Match match;
            if (findMatch(*re, text, 0, match)) matches.push_back(std::move(match));
        }
        std::string out;
        size_t last = 0;
        for (const Match& match : matches) {
            out += text.substr(last, match.index - last);
            out += replacementFor(replacer, templateText, text, match.index, match.matched, match.captures);
            last = match.index + match.matched.size();
        }
        out += text.substr(last);
        return makeString(out);
    }

    const std::string needle = toText(search);
    std::string out;
    size_t last = 0;
    size_t from = 0;
    while (from <= text.size()) {
        const size_t found = text.find(needle, from);
        if (found == std::string::npos) break;
        out += text.substr(last, found - last);
        out += replacementFor(replacer, templateText, text, found, needle, {});
        last = found + needle.size();
        if (!all) break;
        from = needle.empty() ? (found < text.size() ? found + stepOver(text, found) : text.size() + 1) : last;
    }
    out += text.substr(std::min(last, text.size()));
    return makeString(out);
}

Value splitImpl(const FunctionCallInfo& info) {
    const std::string text = thisString(info, "split");
    const Value separator = info.argument(0);
    double limitValue = info.argument(1).isUndefined() ? 4294967295.0 : std::fmod(integerOf(info.argument(1)), 4294967296.0);
    if (limitValue < 0) limitValue += 4294967296.0;
    const size_t limit = static_cast<size_t>(limitValue);

    std::vector<Value> parts;
    if (limit == 0) return Value::object(new Array(std::move(parts)));
    if (separator.isUndefined()) {
        parts.push_back(makeString(text));
        return Value::object(new Array(std::move(parts)));
    }

    if (RegExpObject* re = asRegExp(separator)) {
        Scanner scanner(re);
        if (text.empty()) {
            Match match;
            if (!findMatch(scanner.get(), text, 0, match)) parts.push_back(makeString(text));
            scanner.get().setLastIndex(0);
            return Value::object(new Array(std::move(parts)));
        }
        size_t last = 0;   // start of the piece being built
        size_t from = 0;   // where to look for the next match
        Match match;
        while (from < text.size() && findMatch(scanner.get(), text, from, match)) {
            const size_t end = match.index + match.matched.size();
            if (match.index >= text.size()) break;
            if (end == last) {  // an empty match right after the previous cut splits nothing
                from = match.index + stepOver(text, match.index);
                continue;
            }
            parts.push_back(makeString(text.substr(last, match.index - last)));
            if (parts.size() == limit) { scanner.get().setLastIndex(0); return Value::object(new Array(std::move(parts))); }
            for (const Value& capture : match.captures) {
                parts.push_back(capture);
                if (parts.size() == limit) { scanner.get().setLastIndex(0); return Value::object(new Array(std::move(parts))); }
            }
            last = end;
            from = match.matched.empty() ? match.index + stepOver(text, match.index) : end;
        }
        scanner.get().setLastIndex(0);
        parts.push_back(makeString(text.substr(last)));
        return Value::object(new Array(std::move(parts)));
    }

    const std::string needle = toText(separator);
    if (needle.empty()) {
        for (size_t i = 0; i < text.size() && parts.size() < limit;) {
            const size_t length = stepOver(text, i);
            parts.push_back(makeString(text.substr(i, length)));
            i += length;
        }
        return Value::object(new Array(std::move(parts)));
    }
    size_t position = 0;
    while (parts.size() < limit) {
        const size_t found = text.find(needle, position);
        if (found == std::string::npos) {
            parts.push_back(makeString(text.substr(position)));
            break;
        }
        parts.push_back(makeString(text.substr(position, found - position)));
        position = found + needle.size();
    }
    return Value::object(new Array(std::move(parts)));
}

// The RegExp a match/matchAll/search argument stands for
std::unique_ptr<RegExpObject> regExpFromArgument(const Value& argument, const std::string& flags) {
    return compileRegExp(argument.isUndefined() ? "(?:)" : toText(argument), flags);
}

Value matchImpl(const FunctionCallInfo& info) {
    const std::string text = thisString(info, "match");
    std::unique_ptr<RegExpObject> owned;
    RegExpObject* re = asRegExp(info.argument(0));
    if (!re) {
        owned = regExpFromArgument(info.argument(0), "");
        re = owned.get();
    }
    if (!re->global()) {
        re->setLastIndex(0);
        return re->exec(text);
    }
    const std::vector<Match> matches = allMatches(*re, text);
    if (matches.empty()) return Value::null();
    std::vector<Value> found;
    for (const Match& match : matches) found.push_back(makeString(match.matched));
    return Value::object(new Array(std::move(found)));
}

Value matchAllImpl(const FunctionCallInfo& info) {
    const std::string text = thisString(info, "matchAll");
    std::unique_ptr<RegExpObject> owned;
    RegExpObject* re = asRegExp(info.argument(0));
    if (re) {
        if (!re->global()) throw JSError("TypeError", "String.prototype.matchAll called with a non-global RegExp argument");
    } else {
        owned = regExpFromArgument(info.argument(0), "g");
        re = owned.get();
    }
    std::vector<Value> results;
    for (const Match& match : allMatches(*re, text)) results.push_back(match.result);
    return IteratorBuiltin::makeListIterator(std::move(results));
}

Value searchImpl(const FunctionCallInfo& info) {
    const std::string text = thisString(info, "search");
    std::unique_ptr<RegExpObject> owned;
    RegExpObject* re = asRegExp(info.argument(0));
    if (!re) {
        owned = regExpFromArgument(info.argument(0), "");
        re = owned.get();
    }
    return Value::number(static_cast<double>(re->search(text)));
}

[[noreturn]] void regExpNotAllowed(const char* method) {
    throw JSError("TypeError", std::string("First argument to String.prototype.") + method +
                               " must not be a regular expression");
}

Value padImpl(const FunctionCallInfo& info, bool atStart) {
    const std::string text = thisString(info, atStart ? "padStart" : "padEnd");
    const double target = std::max(0.0, integerOf(info.argument(0)));
    const std::string filler = info.argument(1).isUndefined() ? " " : toText(info.argument(1));
    if (target <= static_cast<double>(text.size()) || filler.empty()) return makeString(text);
    if (target > static_cast<double>(kMaxStringLength)) throw JSError("RangeError", "Invalid string length");

    const size_t padding = static_cast<size_t>(target) - text.size();
    std::string fill;
    fill.reserve(padding);
    while (fill.size() < padding) fill += filler;
    fill.resize(padding);
    return makeString(atStart ? fill + text : text + fill);
}

Value trimImpl(const FunctionCallInfo& info, bool start, bool end, const char* method) {
    const std::string text = thisString(info, method);
    const auto* data = reinterpret_cast<const unsigned char*>(text.data());
    size_t from = 0;
    if (start) {
        while (from < text.size()) {
            const size_t length = Runtime::NumberConversion::whitespaceLength(data + from, text.size() - from);
            if (length == 0) break;
            from += length;
        }
    }
    size_t to = text.size();
    if (end) {
        size_t lastNonSpace = from;
        for (size_t i = from; i < text.size();) {
            const size_t length = Runtime::NumberConversion::whitespaceLength(data + i, text.size() - i);
            if (length) { i += length; } else { ++i; lastNonSpace = i; }
        }
        to = lastNonSpace;
    }
    return makeString(substringOf(text, from, to));
}

// The text of the code point that starts at a byte index, "" for any other index
std::string characterAt(const std::string& text, double index) {
    if (!(index >= 0) || index >= static_cast<double>(text.size())) return "";
    const size_t i = static_cast<size_t>(index);
    if (isContinuation(static_cast<unsigned char>(text[i]))) return "";
    size_t length;
    decodeAt(text, i, length);
    return text.substr(i, length);
}

// Primary comparison ignores case; on a tie lower case sorts before upper case
int collate(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    int tertiary = 0;
    while (i < a.size() && j < b.size()) {
        size_t la, lb;
        const uint32_t ca = decodeAt(a, i, la);
        const uint32_t cb = decodeAt(b, j, lb);
        const uint32_t pa = lowerCodePoint(ca), pb = lowerCodePoint(cb);
        if (pa != pb) return pa < pb ? -1 : 1;
        if (tertiary == 0 && ca != cb) tertiary = ca == pa ? -1 : 1;
        i += la;
        j += lb;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return tertiary;
}

} // namespace

Runtime::Object* StringBuiltin::createStringPrototype(Runtime::Context*) {
    auto* prototype = new Runtime::Object();

    defineMethod(prototype, "toString", [](const FunctionCallInfo& info) -> Value {
        const Value self = info.thisValue();
        if (self.isString()) return self;
        if (self.isObject()) {
            if (auto* wrapper = dynamic_cast<String*>(self.asObject())) return makeString(wrapper->value());
        }
        throw JSError("TypeError", "String.prototype.toString requires that 'this' be a String");
    }, 0);
    prototype->defineProperty("valueOf", *prototype->getOwnPropertyDescriptor("toString"));

    defineMethod(prototype, "charAt", [](const FunctionCallInfo& info) -> Value {
        return makeString(characterAt(thisString(info, "charAt"), integerOf(info.argument(0))));
    }, 1);

    defineMethod(prototype, "at", [](const FunctionCallInfo& info) -> Value {
        const std::string text = thisString(info, "at");
        double index = integerOf(info.argument(0));
        if (index < 0) index += static_cast<double>(text.size());
        const std::string character = characterAt(text, index);
        return character.empty() ? Value::undefined() : makeString(character);
    }, 1);

    defineMethod(prototype, "charCodeAt", [](const FunctionCallInfo& info) -> Value {
        const std::string text = thisString(info, "charCodeAt");
        const double index = integerOf(info.argument(0));
        if (!(index >= 0) || index >= static_cast<double>(text.size())) return Value::number(std::nan(""));
        size_t length;
        const uint32_t code = decodeAt(text, static_cast<size_t>(index), length);
        // Code units above the BMP are surrogate pairs; this is the high half
        return Value::number(code > 0xFFFF ? 0xD800 + ((code - 0x10000) >> 10) : code);
    }, 1);

    defineMethod(prototype, "codePointAt", [](const FunctionCallInfo& info) -> Value {
        const std::string text = thisString(info, "codePointAt");
        const double index = integerOf(info.argument(0));
        if (!(index >= 0) || index >= static_cast<double>(text.size())) return Value::undefined();
        size_t length;
        return Value::number(decodeAt(text, static_cast<size_t>(index), length));
    }, 1);

    defineMethod(prototype, "concat", [](const FunctionCallInfo& info) -> Value {
        std::string result = thisString(info, "concat");
        for (size_t i = 0; i < info.argumentCount(); i++) result += toText(info.argument(i));
        return makeString(result);
    }, 1);

    defineMethod(prototype, "includes", [](const FunctionCallInfo& info) -> Value {
        const std::string text = thisString(info, "includes");
        if (asRegExp(info.argument(0))) regExpNotAllowed("includes");
        const std::string needle = toText(info.argument(0));
        return Value::boolean(text.find(needle, clampPosition(info.argument(1), text.size(), 0)) != std::string::npos);
    }, 1);

    defineMethod(prototype, "indexOf", [](const FunctionCallInfo& info) -> Value {
        const std::string text = thisString(info, "indexOf");
        const std::string needle = toText(info.argument(0));
        const size_t found = text.find(needle, clampPosition(info.argument(1), text.size(), 0));
        return Value::number(found == std::string::npos ? -1 : static_cast<double>(found));
    }, 1);

    defineMethod(prototype, "lastIndexOf", [](const FunctionCallInfo& info) -> Value {
        const std::string text = thisString(info, "lastIndexOf");
        const std::string needle = toText(info.argument(0));
        const double requested = info.argument(1).toNumber();
        const size_t from = std::isnan(requested) ? text.size() : clampPosition(Value::number(requested), text.size(), text.size());
        const size_t found = text.rfind(needle, from);
        return Value::number(found == std::string::npos ? -1 : static_cast<double>(found));
    }, 1);

    defineMethod(prototype, "startsWith", [](const FunctionCallInfo& info) -> Value {
        const std::string text = thisString(info, "startsWith");
        if (asRegExp(info.argument(0))) regExpNotAllowed("startsWith");
        const std::string needle = toText(info.argument(0));
        const size_t from = clampPosition(info.argument(1), text.size(), 0);
        return Value::boolean(text.compare(from, needle.size(), needle) == 0 && from + needle.size() <= text.size());
    }, 1);

    defineMethod(prototype, "endsWith", [](const FunctionCallInfo& info) -> Value {
        const std::string text = thisString(info, "endsWith");
        if (asRegExp(info.argument(0))) regExpNotAllowed("endsWith");
        const std::string needle = toText(info.argument(0));
        const size_t end = clampPosition(info.argument(1), text.size(), text.size());
        if (needle.size() > end) return Value::boolean(false);
        return Value::boolean(text.compare(end - needle.size(), needle.size(), needle) == 0);
    }, 1);

    defineMethod(prototype, "slice", [](const FunctionCallInfo& info) -> Value {
        const std::string text = thisString(info, "slice");
        const size_t from = relativePosition(info.argument(0), text.size(), 0);
        const size_t to = relativePosition(info.argument(1), text.size(), text.size());
        return makeString(substringOf(text, from, to));
    }, 2);

    defineMethod(prototype, "substring", [](const FunctionCallInfo& info) -> Value {
        const std::string text = thisString(info, "substring");
        size_t from = clampPosition(info.argument(0), text.size(), 0);
        size_t to = clampPosition(info.argument(1), text.size(), text.size());
        if (from > to) std::swap(from, to);
        return makeString(substringOf(text, from, to));
    }, 2);

    defineMethod(prototype, "substr", [](const FunctionCallInfo& info) -> Value {
        const std::string text = thisString(info, "substr");
        const size_t from = relativePosition(info.argument(0), text.size(), 0);
        const double length = info.argument(1).isUndefined() ? static_cast<double>(text.size())
                                                             : std::max(0.0, integerOf(info.argument(1)));
        const size_t to = static_cast<size_t>(std::min(static_cast<double>(text.size()), static_cast<double>(from) + length));
        return makeString(substringOf(text, from, to));
    }, 2);

    defineMethod(prototype, "toLowerCase", [](const FunctionCallInfo& info) -> Value {
        return makeString(mapCase(thisString(info, "toLowerCase"), false));
    }, 0);
    defineMethod(prototype, "toUpperCase", [](const FunctionCallInfo& info) -> Value {
        return makeString(mapCase(thisString(info, "toUpperCase"), true));
    }, 0);
    defineMethod(prototype, "toLocaleLowerCase", [](const FunctionCallInfo& info) -> Value {
        return makeString(mapCase(thisString(info, "toLocaleLowerCase"), false));
    }, 0);
    defineMethod(prototype, "toLocaleUpperCase", [](const FunctionCallInfo& info) -> Value {
        return makeString(mapCase(thisString(info, "toLocaleUpperCase"), true));
    }, 0);

    defineMethod(prototype, "trim", [](const FunctionCallInfo& info) -> Value {
        return trimImpl(info, true, true, "trim");
    }, 0);
    Runtime::Function* trimStart = defineMethod(prototype, "trimStart", [](const FunctionCallInfo& info) -> Value {
        return trimImpl(info, true, false, "trimStart");
    }, 0);
    Runtime::Function* trimEnd = defineMethod(prototype, "trimEnd", [](const FunctionCallInfo& info) -> Value {
        return trimImpl(info, false, true, "trimEnd");
    }, 0);
    defineHidden(prototype, "trimLeft", Value::object(trimStart));
    defineHidden(prototype, "trimRight", Value::object(trimEnd));

    defineMethod(prototype, "padStart", [](const FunctionCallInfo& info) -> Value { return padImpl(info, true); }, 2);
    defineMethod(prototype, "padEnd", [](const FunctionCallInfo& info) -> Value { return padImpl(info, false); }, 2);

    defineMethod(prototype, "repeat", [](const FunctionCallInfo& info) -> Value {
        const std::string text = thisString(info, "repeat");
        const double count = integerOf(info.argument(0));
        if (count < 0 || std::isinf(count)) {
            throw JSError("RangeError", "Invalid count value: " + info.argument(0).toString());
        }
        if (count * static_cast<double>(text.size()) > static_cast<double>(kMaxStringLength)) {
            throw JSError("RangeError", "Invalid string length");
        }
        std::string result;
        result.reserve(text.size() * static_cast<size_t>(count));
        for (size_t i = 0; i < static_cast<size_t>(count); i++) result += text;
        return makeString(result);
    }, 1);

    defineMethod(prototype, "split", splitImpl, 2);
    defineMethod(prototype, "replace", [](const FunctionCallInfo& info) -> Value { return replaceImpl(info, false); }, 2);
    defineMethod(prototype, "replaceAll", [](const FunctionCallInfo& info) -> Value { return replaceImpl(info, true); }, 2);
    defineMethod(prototype, "match", matchImpl, 1);
    defineMethod(prototype, "matchAll", matchAllImpl, 1);
    defineMethod(prototype, "search", searchImpl, 1);

    defineMethod(prototype, "localeCompare", [](const FunctionCallInfo& info) -> Value {
        return Value::number(collate(thisString(info, "localeCompare"), toText(info.argument(0))));
    }, 1);

    // for-of and spread read strings natively; this serves explicit str[Symbol.iterator]()
    defineMethod(prototype, Runtime::symbolPropertyKey(Runtime::WellKnownSymbols::Iterator),
                 [](const FunctionCallInfo& info) -> Value {
        const std::string text = thisString(info, "[Symbol.iterator]");
        std::vector<Value> characters;
        for (size_t i = 0; i < text.size();) {
            const size_t length = stepOver(text, i);
            characters.push_back(makeString(text.substr(i, length)));
            i += length;
        }
        return IteratorBuiltin::makeListIterator(std::move(characters));
    }, 0);

    return prototype;
}

void StringBuiltin::installStatics(Runtime::Function* constructor) {
    defineMethod(constructor, "fromCharCode", [](const FunctionCallInfo& info) -> Value {
        std::string out;
        const size_t count = info.argumentCount();
        for (size_t i = 0; i < count; i++) {
            const uint32_t unit = static_cast<uint32_t>(static_cast<int64_t>(std::fmod(integerOf(info.argument(i)), 65536.0)) & 0xFFFF);
            if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < count) {
                const uint32_t low = static_cast<uint32_t>(static_cast<int64_t>(std::fmod(integerOf(info.argument(i + 1)), 65536.0)) & 0xFFFF);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    appendCodePoint(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                    ++i;
                    continue;
                }
            }
            // A lone surrogate has no UTF-8 form
            appendCodePoint(out, (unit >= 0xD800 && unit <= 0xDFFF) ? 0xFFFD : unit);
        }
        return makeString(out);
    }, 1);

    defineMethod(constructor, "fromCodePoint", [](const FunctionCallInfo& info) -> Value {
        std::string out;
        for (size_t i = 0; i < info.argumentCount(); i++) {
            const double code = info.argument(i).toNumber();
            if (!(code >= 0 && code <= 0x10FFFF) || code != std::floor(code)) {
                throw JSError("RangeError", "Invalid code point " + info.argument(i).toString());
            }
            const uint32_t point = static_cast<uint32_t>(code);
            appendCodePoint(out, (point >= 0xD800 && point <= 0xDFFF) ? 0xFFFD : point);
        }
        return makeString(out);
    }, 1);

    defineMethod(constructor, "raw", [](const FunctionCallInfo& info) -> Value {
        VM* vm = VM::current();
        const Value strings = info.argument(0);
        if (!strings.isObject()) throw JSError("TypeError", "Cannot convert undefined or null to object");
        const Value raw = vm ? vm->getMember(strings, "raw") : strings.asObject()->get("raw");
        if (!raw.isObject()) throw JSError("TypeError", "Cannot convert undefined or null to object");
        const double length = (vm ? vm->getMember(raw, "length") : raw.asObject()->get("length")).toNumber();
        std::string out;
        for (uint32_t i = 0; i < length && i < (1u << 24); i++) {
            out += toText(raw.asObject()->get(i));
            if (i + 1 < length && i + 1 < info.argumentCount()) out += toText(info.argument(i + 1));
        }
        return makeString(out);
    }, 1);
}

} // namespace Zepra::Builtins
