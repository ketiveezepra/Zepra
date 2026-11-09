// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
#pragma once

/**
 * @file string_utf16.hpp
 * @brief UTF-16 view of ZepraScript strings
 *
 * JavaScript strings are sequences of UTF-16 code units: length, indices,
 * charCodeAt, slice and friends all count code units. ZepraScript keeps
 * string contents as 8-bit text in "WTF-8" form: ordinary UTF-8, except that
 * a lone surrogate code unit (which UTF-8 cannot express) is written as its
 * 3-byte generalized encoding (ED A0..BF xx). Every JS string therefore has
 * exactly one byte form, and converting between the two is lossless.
 *
 * Most strings in real pages are ASCII, where a byte index *is* a code-unit
 * index: those take the fast path with no conversion at all. Non-ASCII
 * strings get a UTF-16 copy built on first indexed access and cached on the
 * String object (see Runtime::String::utf16()).
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace Zepra::Runtime::Utf16 {

// True when every byte is below 0x80; scans a word at a time
inline bool isAscii(const char* data, size_t size) {
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t word;
        std::memcpy(&word, data + i, 8);
        if (word & 0x8080808080808080ull) return false;
    }
    for (; i < size; ++i) {
        if (static_cast<unsigned char>(data[i]) & 0x80) return false;
    }
    return true;
}

inline bool isAscii(const std::string& text) { return isAscii(text.data(), text.size()); }

inline bool isLeadSurrogate(uint32_t unit) { return unit >= 0xD800 && unit <= 0xDBFF; }
inline bool isTrailSurrogate(uint32_t unit) { return unit >= 0xDC00 && unit <= 0xDFFF; }

// Appends one code point (or lone surrogate) in WTF-8
inline void appendWtf8(std::string& out, uint32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code <= 0x10FFFF) {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += "\xEF\xBF\xBD";
    }
}

// Decodes the code point starting at text[i]; `length` receives its byte
// count. A malformed byte decodes as itself (Latin-1) so no input is lost.
inline uint32_t decodeAt(const char* text, size_t size, size_t i, size_t& length) {
    const auto byte = [&](size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char lead = byte(i);
    size_t need = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
    if (need <= 1 || i + need > size) {
        length = 1;
        return lead;
    }
    uint32_t code = lead & (0x7F >> need);
    for (size_t k = 1; k < need; ++k) {
        if ((byte(i + k) & 0xC0) != 0x80) {
            length = 1;
            return lead;
        }
        code = (code << 6) | (byte(i + k) & 0x3F);
    }
    length = need;
    return code;
}

// Number of UTF-16 code units in WTF-8 text
inline size_t length(const char* text, size_t size) {
    size_t units = 0;
    for (size_t i = 0; i < size;) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) { ++units; ++i; continue; }
        size_t n;
        const uint32_t code = decodeAt(text, size, i, n);
        units += code > 0xFFFF ? 2 : 1;
        i += n;
    }
    return units;
}

inline size_t length(const std::string& text) { return length(text.data(), text.size()); }

inline std::u16string toUtf16(const char* text, size_t size) {
    std::u16string out;
    out.reserve(size);
    for (size_t i = 0; i < size;) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) { out += static_cast<char16_t>(lead); ++i; continue; }
        size_t n;
        const uint32_t code = decodeAt(text, size, i, n);
        if (code > 0xFFFF) {
            out += static_cast<char16_t>(0xD800 + ((code - 0x10000) >> 10));
            out += static_cast<char16_t>(0xDC00 + ((code - 0x10000) & 0x3FF));
        } else {
            out += static_cast<char16_t>(code);
        }
        i += n;
    }
    return out;
}

inline std::u16string toUtf16(const std::string& text) { return toUtf16(text.data(), text.size()); }

// UTF-16 code units back to WTF-8: pairs join into one code point, lone
// surrogates keep their 3-byte form
inline std::string fromUtf16(const char16_t* units, size_t count) {
    std::string out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t unit = units[i];
        if (unit < 0x80) { out += static_cast<char>(unit); continue; }
        if (isLeadSurrogate(unit) && i + 1 < count && isTrailSurrogate(units[i + 1])) {
            appendWtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (units[i + 1] - 0xDC00));
            ++i;
            continue;
        }
        appendWtf8(out, unit);
    }
    return out;
}

inline std::string fromUtf16(const std::u16string& units) { return fromUtf16(units.data(), units.size()); }

// Byte offset of code unit `unit` (clamped to the end). An index that falls
// between the halves of a surrogate pair maps to the pair's start.
inline size_t byteOffsetOfUnit(const std::string& text, size_t unit) {
    size_t units = 0, i = 0;
    while (i < text.size() && units < unit) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) { ++units; ++i; continue; }
        size_t n;
        const uint32_t code = decodeAt(text.data(), text.size(), i, n);
        const size_t width = code > 0xFFFF ? 2 : 1;
        if (units + width > unit) break;
        units += width;
        i += n;
    }
    return i;
}

// Code-unit index of byte offset `byte` (which must start a code point)
inline size_t unitOffsetOfByte(const std::string& text, size_t byte) {
    return length(text.data(), byte < text.size() ? byte : text.size());
}

// a + b, joining a trailing lone lead surrogate of `a` with a leading lone
// trail surrogate of `b` into the 4-byte form of their code point, so that
// '\uD83D' + '\uDE00' === '\u{1F600}'
inline bool needsSurrogateJoin(const std::string& a, const std::string& b) {
    if (a.size() < 3 || b.size() < 3) return false;
    const auto* x = reinterpret_cast<const unsigned char*>(a.data() + a.size() - 3);
    const auto* y = reinterpret_cast<const unsigned char*>(b.data());
    return x[0] == 0xED && (x[1] & 0xF0) == 0xA0 && y[0] == 0xED && (y[1] & 0xF0) == 0xB0;
}

inline std::string joinSurrogates(const std::string& a, const std::string& b) {
    size_t n;
    const uint32_t lead = decodeAt(a.data(), a.size(), a.size() - 3, n);
    const uint32_t trail = decodeAt(b.data(), b.size(), 0, n);
    std::string out(a, 0, a.size() - 3);
    out.reserve(a.size() + b.size() - 2);
    appendWtf8(out, 0x10000 + ((lead - 0xD800) << 10) + (trail - 0xDC00));
    out.append(b, 3, std::string::npos);
    return out;
}

} // namespace Zepra::Runtime::Utf16
