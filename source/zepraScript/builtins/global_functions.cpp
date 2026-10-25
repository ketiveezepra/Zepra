// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file global_functions.cpp
 * @brief Global functions: URI encoding, base64 (atob/btoa), queueMicrotask
 */

#include "builtins/global_functions.hpp"
#include "builtins/builtin_helpers.hpp"
#include "runtime/async/promise.hpp"
#include "runtime/execution/vm.hpp"

#include <cstdint>
#include <iostream>

namespace Zepra::Builtins {

using Runtime::FunctionCallInfo;
using Runtime::JSError;
using Runtime::VM;

namespace {

// ---- URI --------------------------------------------------------------------

const char* kUriMarks = "-_.!~*'()";
const char* kUriReserved = ";/?:@&=+$,#";

bool isAsciiAlnum(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool contains(const char* set, unsigned char c) {
    for (const char* p = set; *p; ++p) {
        if (static_cast<unsigned char>(*p) == c) return true;
    }
    return false;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

[[noreturn]] void uriMalformed() {
    throw JSError("URIError", "URI malformed");
}

// Length of the well-formed UTF-8 sequence at text[i], 0 when it is not valid
size_t validUtf8Length(const std::string& text, size_t i) {
    const auto byte = [&](size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char lead = byte(i);
    size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
    if (length == 0 || i + length > text.size()) return 0;
    uint32_t code = length == 1 ? lead : lead & (0x7F >> length);
    for (size_t k = 1; k < length; ++k) {
        if ((byte(i + k) & 0xC0) != 0x80) return 0;
        code = (code << 6) | (byte(i + k) & 0x3F);
    }
    if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) || (length == 4 && code < 0x10000) ||
        code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        return 0;
    }
    return length;
}

// ---- base64 -----------------------------------------------------------------

const char* kBase64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

[[noreturn]] void invalidCharacter(const char* message) {
    throw JSError("InvalidCharacterError", message);
}

void appendUtf8(std::string& out, uint32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

} // namespace

std::string GlobalFunctions::encodeUri(const std::string& text, bool component) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            if (isAsciiAlnum(c) || contains(kUriMarks, c) || (!component && contains(kUriReserved, c))) {
                out += static_cast<char>(c);
            } else {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 0xF];
            }
            ++i;
            continue;
        }
        const size_t length = validUtf8Length(text, i);
        if (length == 0) uriMalformed();
        for (size_t k = 0; k < length; ++k) {
            const unsigned char b = static_cast<unsigned char>(text[i + k]);
            out += '%';
            out += hex[b >> 4];
            out += hex[b & 0xF];
        }
        i += length;
    }
    return out;
}

std::string GlobalFunctions::decodeUri(const std::string& text, bool component) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text[i] != '%') {
            out += text[i++];
            continue;
        }
        // The byte written as %XX at `at`, or -1 when it is not a complete escape
        auto byteAt = [&](size_t at) -> int {
            if (at + 2 >= text.size() || text[at] != '%') return -1;
            const int high = hexValue(text[at + 1]);
            const int low = hexValue(text[at + 2]);
            return (high < 0 || low < 0) ? -1 : (high << 4) | low;
        };
        const int lead = byteAt(i);
        if (lead < 0) uriMalformed();

        if (lead < 0x80) {
            // decodeURI keeps reserved characters escaped
            if (!component && contains(kUriReserved, static_cast<unsigned char>(lead))) {
                out.append(text, i, 3);
            } else {
                out += static_cast<char>(lead);
            }
            i += 3;
            continue;
        }

        const size_t length = (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
        if (length == 0) uriMalformed();
        std::string sequence(1, static_cast<char>(lead));
        size_t at = i + 3;
        for (size_t k = 1; k < length; ++k, at += 3) {
            const int next = byteAt(at);
            if (next < 0) uriMalformed();
            sequence += static_cast<char>(next);
        }
        if (validUtf8Length(sequence, 0) != length) uriMalformed();
        out += sequence;
        i = at;
    }
    return out;
}

Function* GlobalFunctions::createEncodeURI() {
    return new Function("encodeURI", [](const FunctionCallInfo& info) -> Value {
        return makeString(encodeUri(info.argument(0).toString(), false));
    }, 1);
}

Function* GlobalFunctions::createEncodeURIComponent() {
    return new Function("encodeURIComponent", [](const FunctionCallInfo& info) -> Value {
        return makeString(encodeUri(info.argument(0).toString(), true));
    }, 1);
}

Function* GlobalFunctions::createDecodeURI() {
    return new Function("decodeURI", [](const FunctionCallInfo& info) -> Value {
        return makeString(decodeUri(info.argument(0).toString(), false));
    }, 1);
}

Function* GlobalFunctions::createDecodeURIComponent() {
    return new Function("decodeURIComponent", [](const FunctionCallInfo& info) -> Value {
        return makeString(decodeUri(info.argument(0).toString(), true));
    }, 1);
}

Function* GlobalFunctions::createBtoa() {
    return new Function("btoa", [](const FunctionCallInfo& info) -> Value {
        if (info.argumentCount() == 0) {
            throw JSError("TypeError", "btoa requires 1 argument");
        }
        const std::string text = info.argument(0).toString();

        // The string is UTF-8; every code point must fit in one byte
        std::string bytes;
        for (size_t i = 0; i < text.size();) {
            const size_t length = validUtf8Length(text, i);
            if (length == 0) invalidCharacter("The string to be encoded contains characters outside of the Latin1 range.");
            uint32_t code = static_cast<unsigned char>(text[i]);
            if (length > 1) {
                code &= 0x7F >> length;
                for (size_t k = 1; k < length; ++k) code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
            }
            if (code > 0xFF) invalidCharacter("The string to be encoded contains characters outside of the Latin1 range.");
            bytes += static_cast<char>(code);
            i += length;
        }

        std::string out;
        out.reserve((bytes.size() + 2) / 3 * 4);
        for (size_t i = 0; i < bytes.size(); i += 3) {
            const uint32_t b0 = static_cast<unsigned char>(bytes[i]);
            const uint32_t b1 = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
            const uint32_t b2 = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
            const uint32_t group = (b0 << 16) | (b1 << 8) | b2;
            out += kBase64[(group >> 18) & 0x3F];
            out += kBase64[(group >> 12) & 0x3F];
            out += i + 1 < bytes.size() ? kBase64[(group >> 6) & 0x3F] : '=';
            out += i + 2 < bytes.size() ? kBase64[group & 0x3F] : '=';
        }
        return makeString(out);
    }, 1);
}

Function* GlobalFunctions::createAtob() {
    return new Function("atob", [](const FunctionCallInfo& info) -> Value {
        if (info.argumentCount() == 0) {
            throw JSError("TypeError", "atob requires 1 argument");
        }
        std::string data;
        for (char c : info.argument(0).toString()) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r') continue;
            data += c;
        }
        if (data.size() % 4 == 0 && !data.empty()) {
            if (data.back() == '=') data.pop_back();
            if (data.back() == '=') data.pop_back();
        }
        if (data.size() % 4 == 1) invalidCharacter("The string to be decoded is not correctly encoded.");
        for (char c : data) {
            if (base64Value(c) < 0) invalidCharacter("The string to be decoded is not correctly encoded.");
        }

        // Each decoded byte is one code point of the result
        std::string out;
        uint32_t buffer = 0;
        int bits = 0;
        for (char c : data) {
            buffer = (buffer << 6) | static_cast<uint32_t>(base64Value(c));
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                appendUtf8(out, (buffer >> bits) & 0xFF);
            }
        }
        return makeString(out);
    }, 1);
}

Function* GlobalFunctions::createQueueMicrotask() {
    return new Function("queueMicrotask", [](const FunctionCallInfo& info) -> Value {
        const Value callback = info.argument(0);
        if (!callback.isObject() || !callback.asObject()->isFunction()) {
            throw JSError("TypeError", "queueMicrotask requires a function");
        }
        auto* function = static_cast<Function*>(callback.asObject());
        VM* vm = VM::current();
        if (!vm) throw JSError("TypeError", "queueMicrotask called outside a running script");
        Runtime::MicrotaskQueue::instance().enqueue([vm, function]() {
            try {
                vm->executeCallback(function, Value::undefined(), {});
            } catch (const std::exception& e) {
                std::cerr << "Uncaught (in microtask) " << e.what() << std::endl;
            }
        });
        return Value::undefined();
    }, 1);
}

} // namespace Zepra::Builtins
