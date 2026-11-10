// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file lexer.cpp
 * @brief JavaScript tokenizer implementation
 */

#include "frontend/lexer.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace Zepra::Frontend {

namespace {

// Keyword lookup table
const std::unordered_map<std::string_view, TokenType> keywords = {
    {"var", TokenType::Var},
    {"let", TokenType::Let},
    {"const", TokenType::Const},
    {"function", TokenType::Function},
    {"return", TokenType::Return},
    {"if", TokenType::If},
    {"else", TokenType::Else},
    {"for", TokenType::For},
    {"while", TokenType::While},
    {"do", TokenType::Do},
    {"break", TokenType::Break},
    {"continue", TokenType::Continue},
    {"switch", TokenType::Switch},
    {"case", TokenType::Case},
    {"default", TokenType::Default},
    {"try", TokenType::Try},
    {"catch", TokenType::Catch},
    {"finally", TokenType::Finally},
    {"throw", TokenType::Throw},
    {"new", TokenType::New},
    {"delete", TokenType::Delete},
    {"typeof", TokenType::Typeof},
    {"instanceof", TokenType::Instanceof},
    {"in", TokenType::In},
    {"this", TokenType::This},
    {"class", TokenType::Class},
    {"extends", TokenType::Extends},
    {"super", TokenType::Super},
    {"static", TokenType::Static},
    {"import", TokenType::Import},
    {"export", TokenType::Export},
    {"from", TokenType::From},
    {"as", TokenType::As},
    {"async", TokenType::Async},
    {"await", TokenType::Await},
    {"yield", TokenType::Yield},
    {"null", TokenType::Null},
    {"undefined", TokenType::Undefined},
    {"true", TokenType::True},
    {"false", TokenType::False},
    {"void", TokenType::Void},
    {"with", TokenType::With},
    {"debugger", TokenType::Debugger},
    {"of", TokenType::Of},
    {"get", TokenType::Get},
    {"set", TokenType::Set},
};

} // anonymous namespace

Lexer::Lexer(const SourceCode* source)
    : source_(source)
    , src_(source->content().data())
    , srcLen_(source->content().size())
    , offset_(0)
    , location_{1, 1, 0}
    , tokenStart_{1, 1, 0}
{
    // A UTF-8 byte-order mark before the first token is not part of the script
    if (srcLen_ >= 3 && static_cast<unsigned char>(src_[0]) == 0xEF &&
        static_cast<unsigned char>(src_[1]) == 0xBB && static_cast<unsigned char>(src_[2]) == 0xBF) {
        offset_ = 3;
        location_.offset = 3;
    }
    // Hashbang comment: `#!` at the very start runs to the end of the line
    if (offset_ + 1 < srcLen_ && src_[offset_] == '#' && src_[offset_ + 1] == '!') {
        while (offset_ < srcLen_ && src_[offset_] != '\n') offset_++;
        location_.offset = static_cast<uint32_t>(offset_);
    }
}

char Lexer::current() const {
    if (offset_ >= srcLen_) return '\0';
    return src_[offset_];
}

char Lexer::peek(size_t ahead) const {
    if (offset_ + ahead >= srcLen_) return '\0';
    return src_[offset_ + ahead];
}

char Lexer::advance() {
    char c = current();
    offset_++;
    
    if (c == '\n') {
        location_.line++;
        location_.column = 1;
    } else {
        location_.column++;
    }
    location_.offset = offset_;
    
    return c;
}

bool Lexer::match(char expected) {
    if (current() != expected) return false;
    advance();
    return true;
}

void Lexer::skipWhitespace() {
    while (true) {
        char c = current();
        switch (c) {
            case ' ':
            case '\t':
            case '\r':
                advance();
                break;
            case '\n':
            case '\v':
            case '\f':
                advance();
                break;
            case '<':
                // Annex B HTML-like comment: `<!--` starts a single-line comment
                if (peek(1) == '!' && peek(2) == '-' && peek(3) == '-') {
                    skipLineComment();
                    break;
                }
                return;
            case '-':
                // Annex B: `-->` first on a line (after whitespace) is a comment
                if (peek(1) == '-' && peek(2) == '>' && atLineStart()) {
                    skipLineComment();
                    break;
                }
                return;
            case '/':
                if (peek(1) == '/') {
                    skipLineComment();
                } else if (peek(1) == '*') {
                    skipBlockComment();
                } else {
                    return;
                }
                break;
            default:
                if (static_cast<unsigned char>(c) >= 0x80) {
                    size_t n = unicodeSpaceLength(offset_);
                    if (n) {
                        for (size_t i = 0; i < n; i++) advance();
                        break;
                    }
                }
                return;
        }
    }
}

// Byte length of a non-ASCII WhiteSpace or LineTerminator code point at `at`
// (NBSP, ZWNBSP, the Zs block, LS, PS); 0 when there is none
size_t Lexer::unicodeSpaceLength(size_t at) const {
    auto byte = [this](size_t i) -> unsigned char {
        return i < srcLen_ ? static_cast<unsigned char>(src_[i]) : 0;
    };
    unsigned char b0 = byte(at), b1 = byte(at + 1), b2 = byte(at + 2);
    if (b0 == 0xC2 && b1 == 0xA0) return 2;                       // U+00A0
    if (b0 == 0xEF && b1 == 0xBB && b2 == 0xBF) return 3;         // U+FEFF
    if (b0 == 0xE1 && b1 == 0x9A && b2 == 0x80) return 3;         // U+1680
    if (b0 == 0xE2 && b1 == 0x80 && ((b2 >= 0x80 && b2 <= 0x8A) || b2 == 0xA8 ||
                                     b2 == 0xA9 || b2 == 0xAF)) return 3;  // U+2000-200A, 2028, 2029, 202F
    if (b0 == 0xE2 && b1 == 0x81 && b2 == 0x9F) return 3;         // U+205F
    if (b0 == 0xE3 && b1 == 0x80 && b2 == 0x80) return 3;         // U+3000
    return 0;
}

// True when only spaces, tabs and block comments precede offset_ on its line
bool Lexer::atLineStart() const {
    if (lastTokenLine_ == 0) return true;
    return location_.line > lastTokenLine_;
}

void Lexer::skipLineComment() {
    // Skip //
    advance();
    advance();
    
    while (current() != '\0' && current() != '\n') {
        advance();
    }
}

void Lexer::skipBlockComment() {
    // Skip /*
    advance();
    advance();
    
    while (current() != '\0') {
        if (current() == '*' && peek(1) == '/') {
            advance();
            advance();
            return;
        }
        advance();
    }
    
    addError("Unterminated block comment");
}

Token Lexer::nextToken() {
    if (hasPeeked_) {
        hasPeeked_ = false;
        Token tok = std::move(peekedToken_);
        updateDivisionContext(tok.type);
        return tok;
    }
    
    skipWhitespace();
    tokenStart_ = location_;
    
    if (isEof()) {
        return makeToken(TokenType::EndOfFile);
    }
    
    char c = advance();
    
    // Identifiers and keywords
    if (isIdentifierStart(c) || (c == '\\' && current() == 'u')) {
        Token tok = scanIdentifierOrKeyword();
        lastTokenLine_ = tok.end.line;
        updateDivisionContext(tok.type);
        return tok;
    }

    // Class private names: #name
    if (c == '#' && (isIdentifierStart(current()) || (current() == '\\' && peek(1) == 'u'))) {
        advance();
        Token tok = scanIdentifierOrKeyword();
        tok.type = TokenType::PrivateName;
        tok.value = "#" + tok.value;
        tok.start = tokenStart_;
        lastTokenLine_ = tok.end.line;
        updateDivisionContext(tok.type);
        return tok;
    }
    
    // Numbers
    if (isDigit(c) || (c == '.' && isDigit(peek(0)))) {
        Token tok = scanNumber();
        lastTokenLine_ = tok.end.line;
        updateDivisionContext(tok.type);
        return tok;
    }
    
    // Strings
    if (c == '"' || c == '\'') {
        Token tok = scanString(c);
        lastTokenLine_ = tok.end.line;
        updateDivisionContext(tok.type);
        return tok;
    }
    
    // Template literals
    if (c == '`') {
        Token tok = scanTemplate();
        lastTokenLine_ = tok.end.line;
        updateDivisionContext(tok.type);
        return tok;
    }
    
    // Operators and punctuation
    Token tok = scanOperator();
    lastTokenLine_ = tok.end.line;
    updateDivisionContext(tok.type);
    return tok;
}

void Lexer::updateDivisionContext(TokenType type) {
    // After these tokens, `/` is division. After everything else, it's regex.
    switch (type) {
        case TokenType::Identifier:
        case TokenType::Number:
        case TokenType::String:
        case TokenType::RegExp:
        case TokenType::True:
        case TokenType::False:
        case TokenType::Null:
        case TokenType::Undefined:
        case TokenType::This:
        case TokenType::Super:
        // Contextual keywords that are also ordinary names: o.get / 2.
        // (Not `of`: `for (x of /re/...)` is a regex.)
        case TokenType::Get:
        case TokenType::Set:
        case TokenType::Static:
        case TokenType::Async:
        case TokenType::From:
        case TokenType::As:
        case TokenType::RightParen:
        case TokenType::RightBracket:
        case TokenType::RightBrace:
        case TokenType::PlusPlus:
        case TokenType::MinusMinus:
            canFollowDivision_ = true;
            break;
        default:
            canFollowDivision_ = false;
            break;
    }
}

const Token& Lexer::peek() {
    if (!hasPeeked_) {
        peekedToken_ = nextToken();
        hasPeeked_ = true;
    }
    return peekedToken_;
}

namespace {
// Digits of a numeric literal with its separators removed (`1_000` -> `1000`)
std::string stripNumericSeparators(const char* begin, size_t length) {
    std::string out;
    out.reserve(length);
    for (size_t i = 0; i < length; i++) {
        if (begin[i] != '_') out += begin[i];
    }
    return out;
}

// Value of an integer written in a power-of-two radix; exact up to 2^53 and
// correctly rounded-ish beyond (no 64-bit overflow like strtol)
double radixValue(const std::string& digits, int radix) {
    double v = 0;
    for (char c : digits) {
        int d = (c >= '0' && c <= '9') ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
        v = v * radix + d;
    }
    return v;
}
}  // namespace

// Consumes digits accepted by `isDigitFn`, allowing single `_` separators
// between two digits (ES2021 NumericLiteralSeparator)
template <typename DigitFn>
void Lexer::scanDigits(DigitFn isDigitFn) {
    while (true) {
        if (isDigitFn(current())) {
            advance();
        } else if (current() == '_' && offset_ > 0 && isDigitFn(src_[offset_ - 1]) &&
                   isDigitFn(peek(1))) {
            advance();
        } else {
            break;
        }
    }
}

Token Lexer::scanNumber() {
    size_t start = offset_ - 1;
    auto dec = [](char c) { return c >= '0' && c <= '9'; };

    // Hex, octal, binary
    if (src_[start] == '0' && offset_ < srcLen_) {
        char next = current();
        int radix = 0;
        if (next == 'x' || next == 'X') radix = 16;
        else if (next == 'b' || next == 'B') radix = 2;
        else if (next == 'o' || next == 'O') radix = 8;
        if (radix) {
            advance();
            size_t digitsStart = offset_;
            if (radix == 16) scanDigits([](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); });
            else if (radix == 8) scanDigits([](char c) { return c >= '0' && c <= '7'; });
            else scanDigits([](char c) { return c == '0' || c == '1'; });
            std::string digits = stripNumericSeparators(src_ + digitsStart, offset_ - digitsStart);
            if (digits.empty()) return errorToken("Invalid number literal");
            if (current() == 'n') return errorToken("BigInt literals are not supported");
            if (isIdentifierStart(current()) || dec(current())) return errorToken("Invalid number literal");
            Token token = makeToken(TokenType::Number, std::string(src_ + start, offset_ - start));
            token.numericValue = radixValue(digits, radix);
            return token;
        }
        // Legacy octal (Annex B, sloppy mode): 010 === 8; 08/09 stay decimal
        if (dec(next)) {
            size_t p = offset_;
            bool octal = true;
            while (p < srcLen_ && dec(src_[p])) { if (src_[p] > '7') octal = false; p++; }
            if (octal && !(p < srcLen_ && (src_[p] == '.' || src_[p] == 'e' || src_[p] == 'E' || src_[p] == '_'))) {
                while (offset_ < p) advance();
                std::string digits(src_ + start + 1, offset_ - start - 1);
                Token token = makeToken(TokenType::Number, std::string(src_ + start, offset_ - start));
                token.numericValue = radixValue(digits, 8);
                return token;
            }
        }
    }

    // Integer part (the first digit, or '.', was consumed by the caller)
    if (dec(src_[start])) scanDigits(dec);

    // Decimal part: `1.5`, `1.` and `.5`
    if (src_[start] == '.') {
        scanDigits(dec);
    } else if (current() == '.') {
        advance();
        scanDigits(dec);
    }

    // Exponent
    if ((current() == 'e' || current() == 'E') &&
        (dec(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && dec(peek(2))))) {
        advance();
        if (current() == '+' || current() == '-') advance();
        scanDigits(dec);
    }

    if (current() == 'n') return errorToken("BigInt literals are not supported");
    if (isIdentifierStart(current())) return errorToken("Invalid or unexpected token after number");

    std::string value(src_ + start, offset_ - start);
    Token token = makeToken(TokenType::Number, value);
    std::string digits = stripNumericSeparators(value.data(), value.size());
    token.numericValue = std::strtod(digits.c_str(), nullptr);
    return token;
}

namespace {
int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void appendUtf8(std::string& out, uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        out += static_cast<char>(codepoint);
    } else if (codepoint <= 0x7FF) {
        out += static_cast<char>(0xC0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0xFFFF) {
        out += static_cast<char>(0xE0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0x10FFFF) {
        out += static_cast<char>(0xF0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        out += "\xEF\xBF\xBD";  // Replacement character
    }
}
}  // namespace

void Lexer::scanEscape(std::string& out) {
    // Reads `count` hex digits; -1 (nothing consumed past the bad digit) when one is missing
    auto readHex = [this](int count) -> int32_t {
        int32_t v = 0;
        for (int i = 0; i < count; i++) {
            int d = hexValue(current());
            if (d < 0) return -1;
            v = (v << 4) | d;
            advance();
        }
        return v;
    };
    // \uXXXX or \u{X...}, after the 'u'
    auto readUnicode = [&]() -> int32_t {
        if (current() == '{') {
            advance();
            int32_t v = 0;
            int digits = 0;
            while (current() != '}' && current() != '\0') {
                int d = hexValue(current());
                if (d < 0 || digits >= 6) return -1;
                v = (v << 4) | d;
                advance();
                digits++;
            }
            if (current() != '}' || digits == 0) return -1;
            advance();
            return v;
        }
        return readHex(4);
    };

    char c = current();
    switch (c) {
        case 'n': advance(); out += '\n'; return;
        case 'r': advance(); out += '\r'; return;
        case 't': advance(); out += '\t'; return;
        case 'b': advance(); out += '\b'; return;
        case 'f': advance(); out += '\f'; return;
        case 'v': advance(); out += '\v'; return;
        case '0':
            advance();
            out += '\0';
            return;
        case '\r':
            // Line continuation: the escaped line break is not part of the value
            advance();
            if (current() == '\n') advance();
            return;
        case '\n':
            advance();
            return;
        case 'x': {
            advance();
            int32_t v = readHex(2);
            if (v < 0) {
                addError("Invalid hexadecimal escape sequence");
                return;
            }
            appendUtf8(out, static_cast<uint32_t>(v));
            return;
        }
        case 'u': {
            advance();
            int32_t v = readUnicode();
            if (v < 0) {
                addError("Invalid Unicode escape sequence");
                out += "\xEF\xBF\xBD";
                return;
            }
            // A surrogate pair written as two escapes is one code point
            if (v >= 0xD800 && v <= 0xDBFF && current() == '\\' && peek(1) == 'u') {
                size_t saveOffset = offset_;
                SourceLocation saveLocation = location_;
                advance();
                advance();
                int32_t low = readUnicode();
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    v = 0x10000 + ((v - 0xD800) << 10) + (low - 0xDC00);
                } else {
                    offset_ = saveOffset;
                    location_ = saveLocation;
                }
            }
            appendUtf8(out, static_cast<uint32_t>(v));
            return;
        }
        case '\0':
            return;
        default:
            // \\ \' \" \` \$ and any other character stand for themselves
            out += advance();
            return;
    }
}

Token Lexer::scanString(char quote) {
    std::string value;
    bool hasEscapes = false;
    
    while (current() != '\0' && current() != quote) {
        if (current() == '\\') {
            hasEscapes = true;
            advance();
            scanEscape(value);
        } else if (current() == '\n') {
            addError("Unterminated string literal");
            break;
        } else {
            value += advance();
        }
    }
    
    if (current() != quote) {
        return errorToken("Unterminated string literal");
    }
    advance(); // Closing quote
    
    Token token = makeToken(TokenType::String, value);
    token.hasEscapes = hasEscapes;
    return token;
}

Token Lexer::scanIdentifierOrKeyword() {
    size_t start = offset_ - 1;
    bool escaped = src_[start] == '\\';
    auto skipUnicodeEscapeBody = [this]() {  // after `\u`
        if (current() == '{') {
            while (current() != '\0' && current() != '}') advance();
            if (current() == '}') advance();
        } else {
            for (int i = 0; i < 4 && isHexDigit(current()); i++) advance();
        }
    };
    if (escaped) {
        advance();  // u
        skipUnicodeEscapeBody();
    }
    
    while (true) {
        char c = current();
        if (isIdentifierPart(c)) {
            if (static_cast<unsigned char>(c) >= 0x80 && unicodeSpaceLength(offset_)) break;
            advance();
        } else if (c == '\\' && peek(1) == 'u') {
            escaped = true;
            advance();
            advance();
            skipUnicodeEscapeBody();
        } else {
            break;
        }
    }
    
    if (escaped) {
        // \uXXXX / \u{X} escapes: the name is the decoded text, and an
        // escaped word is never a keyword
        std::string name;
        size_t end = offset_;
        SourceLocation endLocation = location_;
        offset_ = start;
        while (offset_ < end) {
            if (src_[offset_] == '\\') {
                offset_++;  // backslash; scanEscape reads the 'u' sequence
                scanEscape(name);
            } else {
                name += src_[offset_++];
            }
        }
        offset_ = end;
        location_ = endLocation;
        return makeToken(TokenType::Identifier, std::move(name));
    }
    
    size_t len = offset_ - start;
    const char* s = src_ + start;
    
    // Fast keyword lookup: dispatch on length, then compare.
    // Zero allocations, zero hashing — just memcmp against literals.
    TokenType kwType = TokenType::Identifier;
    
    switch (len) {
        case 2:
            if (s[0] == 'i' && s[1] == 'f') kwType = TokenType::If;
            else if (s[0] == 'i' && s[1] == 'n') kwType = TokenType::In;
            else if (s[0] == 'd' && s[1] == 'o') kwType = TokenType::Do;
            else if (s[0] == 'a' && s[1] == 's') kwType = TokenType::As;
            else if (s[0] == 'o' && s[1] == 'f') kwType = TokenType::Of;
            break;
        case 3:
            if (memcmp(s, "var", 3) == 0) kwType = TokenType::Var;
            else if (memcmp(s, "let", 3) == 0) kwType = TokenType::Let;
            else if (memcmp(s, "for", 3) == 0) kwType = TokenType::For;
            else if (memcmp(s, "new", 3) == 0) kwType = TokenType::New;
            else if (memcmp(s, "try", 3) == 0) kwType = TokenType::Try;
            else if (memcmp(s, "get", 3) == 0) kwType = TokenType::Get;
            else if (memcmp(s, "set", 3) == 0) kwType = TokenType::Set;
            break;
        case 4:
            if (memcmp(s, "this", 4) == 0) kwType = TokenType::This;
            else if (memcmp(s, "else", 4) == 0) kwType = TokenType::Else;
            else if (memcmp(s, "case", 4) == 0) kwType = TokenType::Case;
            else if (memcmp(s, "void", 4) == 0) kwType = TokenType::Void;
            else if (memcmp(s, "with", 4) == 0) kwType = TokenType::With;
            else if (memcmp(s, "null", 4) == 0) kwType = TokenType::Null;
            else if (memcmp(s, "true", 4) == 0) kwType = TokenType::True;
            else if (memcmp(s, "from", 4) == 0) kwType = TokenType::From;
            break;
        case 5:
            if (memcmp(s, "const", 5) == 0) kwType = TokenType::Const;
            else if (memcmp(s, "while", 5) == 0) kwType = TokenType::While;
            else if (memcmp(s, "break", 5) == 0) kwType = TokenType::Break;
            else if (memcmp(s, "catch", 5) == 0) kwType = TokenType::Catch;
            else if (memcmp(s, "throw", 5) == 0) kwType = TokenType::Throw;
            else if (memcmp(s, "class", 5) == 0) kwType = TokenType::Class;
            else if (memcmp(s, "super", 5) == 0) kwType = TokenType::Super;
            else if (memcmp(s, "async", 5) == 0) kwType = TokenType::Async;
            else if (memcmp(s, "await", 5) == 0) kwType = TokenType::Await;
            else if (memcmp(s, "yield", 5) == 0) kwType = TokenType::Yield;
            else if (memcmp(s, "false", 5) == 0) kwType = TokenType::False;
            break;
        case 6:
            if (memcmp(s, "return", 6) == 0) kwType = TokenType::Return;
            else if (memcmp(s, "switch", 6) == 0) kwType = TokenType::Switch;
            else if (memcmp(s, "delete", 6) == 0) kwType = TokenType::Delete;
            else if (memcmp(s, "typeof", 6) == 0) kwType = TokenType::Typeof;
            else if (memcmp(s, "import", 6) == 0) kwType = TokenType::Import;
            else if (memcmp(s, "export", 6) == 0) kwType = TokenType::Export;
            else if (memcmp(s, "static", 6) == 0) kwType = TokenType::Static;
            break;
        case 7:
            if (memcmp(s, "default", 7) == 0) kwType = TokenType::Default;
            else if (memcmp(s, "finally", 7) == 0) kwType = TokenType::Finally;
            else if (memcmp(s, "extends", 7) == 0) kwType = TokenType::Extends;
            break;
        case 8:
            if (memcmp(s, "continue", 8) == 0) kwType = TokenType::Continue;
            else if (memcmp(s, "function", 8) == 0) kwType = TokenType::Function;
            else if (memcmp(s, "debugger", 8) == 0) kwType = TokenType::Debugger;
            break;
        case 9:
            if (memcmp(s, "undefined", 9) == 0) kwType = TokenType::Undefined;
            break;
        case 10:
            if (memcmp(s, "instanceof", 10) == 0) kwType = TokenType::Instanceof;
            break;
    }
    
    std::string value(s, len);
    return makeToken(kwType, std::move(value));
}

Token Lexer::scanTemplate() {
    // `text ${expr} text`: the token's value is the cooked text with each
    // substitution's source between \x01 and \x02 markers; templateRaw holds
    // the raw text of each string part
    std::string value;
    std::vector<std::string> raw(1);

    while (current() != '\0' && current() != '`') {
        char c = current();
        if (c == '\\') {
            size_t start = offset_;
            advance();
            scanEscape(value);
            raw.back().append(src_ + start, offset_ - start);
        } else if (c == '$' && peek(1) == '{') {
            advance();  // $
            advance();  // {
            size_t start = offset_;
            if (!skipTemplateSubstitution()) {
                return errorToken("Unterminated template expression");
            }
            value += '\x01';
            value.append(src_ + start, offset_ - start);
            value += '\x02';
            advance();  // }
            raw.emplace_back();
        } else if (c == '\r') {
            // CR and CRLF read as LF, in both the cooked and the raw text
            advance();
            if (current() == '\n') advance();
            value += '\n';
            raw.back() += '\n';
        } else {
            char ch = advance();
            value += ch;
            raw.back() += ch;
        }
    }

    if (current() != '`') {
        return errorToken("Unterminated template literal");
    }
    advance(); // Closing `

    Token token = makeToken(TokenType::Template, value);
    token.templateRaw = std::move(raw);
    return token;
}

bool Lexer::skipTemplateSubstitution() {
    int depth = 0;
    // The last non-space character seen: a '/' after one of these starts a
    // regex literal rather than a division or a comment, so
    // `${e.replace(/\//g, ".")}` is not cut at its "//"
    char lastSignificant = '(';
    auto regexCanFollow = [](char p) {
        return p == '(' || p == ',' || p == '=' || p == ':' || p == '[' || p == '!' ||
               p == '&' || p == '|' || p == '?' || p == '{' || p == '}' || p == ';' ||
               p == '+' || p == '-' || p == '*' || p == '%' || p == '<' || p == '>' ||
               p == '~' || p == '^';
    };
    while (current() != '\0') {
        char c = current();
        if (c == '}' && depth == 0) return true;
        if (c == '/' && peek(1) != '/' && peek(1) != '*' && regexCanFollow(lastSignificant)) {
            // Regex literal: skip to its closing '/', honouring escapes and classes
            advance();
            bool inClass = false;
            while (current() != '\0' && current() != '\n') {
                char r = current();
                if (r == '\\') { advance(); if (current() != '\0') advance(); continue; }
                if (r == '[') inClass = true;
                else if (r == ']') inClass = false;
                else if (r == '/' && !inClass) break;
                advance();
            }
            if (current() == '/') advance();
            while (std::isalpha(static_cast<unsigned char>(current()))) advance();  // flags
            lastSignificant = ')';
            continue;
        }
        if (c == '/' && regexCanFollow(lastSignificant) && (peek(1) == '/' || peek(1) == '*')) {
            // "//" or "/*" where a regex could start is still a comment: fall through
        }
        if (!std::isspace(static_cast<unsigned char>(c)) && c != '/') lastSignificant = c;
        if (c == '{') {
            depth++;
            advance();
        } else if (c == '}') {
            depth--;
            advance();
        } else if (c == '"' || c == '\'') {
            advance();
            while (current() != '\0' && current() != c && current() != '\n') {
                if (current() == '\\') advance();
                advance();
            }
            if (current() == c) advance();
            lastSignificant = ')';
        } else if (c == '`') {
            advance();
            skipNestedTemplate();
            lastSignificant = ')';
        } else if (c == '/' && peek(1) == '/') {
            while (current() != '\0' && current() != '\n') advance();
        } else if (c == '/' && peek(1) == '*') {
            advance();
            advance();
            while (current() != '\0' && !(current() == '*' && peek(1) == '/')) advance();
            if (current() != '\0') {
                advance();
                advance();
            }
        } else {
            if (c == '/') lastSignificant = ')';  // division operator
            advance();
        }
    }
    return false;
}

void Lexer::skipNestedTemplate() {
    // After the opening backquote of a template inside a substitution
    while (current() != '\0' && current() != '`') {
        if (current() == '\\') {
            advance();
            if (current() != '\0') advance();
        } else if (current() == '$' && peek(1) == '{') {
            advance();
            advance();
            if (!skipTemplateSubstitution()) return;
            advance();  // }
        } else {
            advance();
        }
    }
    if (current() == '`') advance();
}

Token Lexer::scanOperator() {
    char c = src_[offset_ - 1];
    
    switch (c) {
        // Single character
        case '(': return makeToken(TokenType::LeftParen);
        case ')': return makeToken(TokenType::RightParen);
        case '{': return makeToken(TokenType::LeftBrace);
        case '}': return makeToken(TokenType::RightBrace);
        case '[': return makeToken(TokenType::LeftBracket);
        case ']': return makeToken(TokenType::RightBracket);
        case ',': return makeToken(TokenType::Comma);
        case ';': return makeToken(TokenType::Semicolon);
        case ':': return makeToken(TokenType::Colon);
        case '~': return makeToken(TokenType::Tilde);
        
        // Potentially multi-character
        case '.':
            if (match('.') && match('.')) {
                return makeToken(TokenType::DotDotDot);
            }
            return makeToken(TokenType::Dot);
            
        case '+':
            if (match('+')) return makeToken(TokenType::PlusPlus);
            if (match('=')) return makeToken(TokenType::PlusAssign);
            return makeToken(TokenType::Plus);
            
        case '-':
            if (match('-')) return makeToken(TokenType::MinusMinus);
            if (match('=')) return makeToken(TokenType::MinusAssign);
            return makeToken(TokenType::Minus);
            
        case '*':
            if (match('*')) {
                if (match('=')) return makeToken(TokenType::StarStarAssign);
                return makeToken(TokenType::StarStar);
            }
            if (match('=')) return makeToken(TokenType::StarAssign);
            return makeToken(TokenType::Star);
            
        case '/':
            if (!canFollowDivision_) {
                // Regex literal context
                return scanRegExp();
            }
            if (match('=')) return makeToken(TokenType::SlashAssign);
            return makeToken(TokenType::Slash);
            
        case '%':
            if (match('=')) return makeToken(TokenType::PercentAssign);
            return makeToken(TokenType::Percent);
            
        case '=':
            if (match('=')) {
                if (match('=')) return makeToken(TokenType::StrictEqual);
                return makeToken(TokenType::Equal);
            }
            if (match('>')) return makeToken(TokenType::Arrow);
            return makeToken(TokenType::Assign);
            
        case '!':
            if (match('=')) {
                if (match('=')) return makeToken(TokenType::StrictNotEqual);
                return makeToken(TokenType::NotEqual);
            }
            return makeToken(TokenType::Not);
            
        case '<':
            if (match('<')) {
                if (match('=')) return makeToken(TokenType::LeftShiftAssign);
                return makeToken(TokenType::LeftShift);
            }
            if (match('=')) return makeToken(TokenType::LessEqual);
            return makeToken(TokenType::Less);
            
        case '>':
            if (match('>')) {
                if (match('>')) {
                    if (match('=')) return makeToken(TokenType::UnsignedRightShiftAssign);
                    return makeToken(TokenType::UnsignedRightShift);
                }
                if (match('=')) return makeToken(TokenType::RightShiftAssign);
                return makeToken(TokenType::RightShift);
            }
            if (match('=')) return makeToken(TokenType::GreaterEqual);
            return makeToken(TokenType::Greater);
            
        case '&':
            if (match('&')) {
                if (match('=')) return makeToken(TokenType::AndAssign);
                return makeToken(TokenType::And);
            }
            if (match('=')) return makeToken(TokenType::AmpersandAssign);
            return makeToken(TokenType::Ampersand);
            
        case '|':
            if (match('|')) {
                if (match('=')) return makeToken(TokenType::OrAssign);
                return makeToken(TokenType::Or);
            }
            if (match('=')) return makeToken(TokenType::PipeAssign);
            return makeToken(TokenType::Pipe);
            
        case '^':
            if (match('=')) return makeToken(TokenType::CaretAssign);
            return makeToken(TokenType::Caret);
            
        case '?':
            if (match('?')) {
                if (match('=')) return makeToken(TokenType::QuestionQuestionAssign);
                return makeToken(TokenType::QuestionQuestion);
            }
            // `?.` followed by a digit is a conditional and a number:
            // `k===void 0?.01:k` (YouTube's base module)
            if (current() == '.' && !(peek(1) >= '0' && peek(1) <= '9')) {
                advance();
                return makeToken(TokenType::QuestionDot);
            }
            return makeToken(TokenType::Question);
    }
    
    return errorToken("Unexpected character");
}

Token Lexer::scanRegExp() {
    // The opening '/' has already been consumed by scanOperator's switch
    std::string pattern;
    std::string flags;
    
    // Scan pattern until unescaped closing '/'
    bool inCharClass = false;
    while (current() != '\0' && current() != '\n') {
        if (current() == '\\') {
            // Escape sequence — include both chars
            pattern += advance();
            if (current() != '\0') pattern += advance();
            continue;
        }
        if (current() == '[') inCharClass = true;
        if (current() == ']') inCharClass = false;
        if (current() == '/' && !inCharClass) break;
        pattern += advance();
    }
    
    if (current() != '/') {
        return errorToken("Unterminated regex literal");
    }
    advance(); // closing '/'
    
    // Scan flags: g, i, m, s, u, y
    while (current() != '\0' && isAlpha(current())) {
        flags += advance();
    }
    
    // Value format: "pattern/flags" for downstream use
    std::string value = pattern + "/" + flags;
    return makeToken(TokenType::RegExp, std::move(value));
}

Token Lexer::makeToken(TokenType type) {
    Token token;
    token.type = type;
    token.start = tokenStart_;
    token.end = location_;
    return token;
}

Token Lexer::makeToken(TokenType type, std::string value) {
    Token token;
    token.type = type;
    token.value = std::move(value);
    token.start = tokenStart_;
    token.end = location_;
    return token;
}

Token Lexer::errorToken(const std::string& message) {
    addError(message);
    return makeToken(TokenType::Error, message);
}

void Lexer::addError(const std::string& message) {
    errors_.push_back(std::to_string(location_.line) + ":" + 
                      std::to_string(location_.column) + ": " + message);
}

bool Lexer::isDigit(char c) const {
    return c >= '0' && c <= '9';
}

bool Lexer::isHexDigit(char c) const {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool Lexer::isAlpha(char c) const {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool Lexer::isAlphaNumeric(char c) const {
    return isAlpha(c) || isDigit(c);
}

bool Lexer::isIdentifierStart(char c) const {
    // Any non-ASCII byte: UTF-8 encoded identifier characters (whitespace
    // code points are taken out by skipWhitespace / unicodeSpaceLength first)
    return isAlpha(c) || c == '_' || c == '$' || static_cast<unsigned char>(c) >= 0x80;
}

bool Lexer::isIdentifierPart(char c) const {
    return isIdentifierStart(c) || isDigit(c);
}

// Token helper methods
std::string SourceLocation::toString() const {
    return std::to_string(line) + ":" + std::to_string(column);
}

bool Token::isKeyword() const {
    return type >= TokenType::Var && type <= TokenType::Set;
}

bool Token::isOperator() const {
    return type >= TokenType::Plus && type <= TokenType::Arrow;
}

bool Token::isPunctuation() const {
    return type >= TokenType::LeftParen && type <= TokenType::Arrow;
}

bool Token::isLiteral() const {
    return type == TokenType::Number || type == TokenType::String ||
           type == TokenType::True || type == TokenType::False ||
           type == TokenType::Null;
}

bool Token::isAssignment() const {
    return type == TokenType::Assign || type == TokenType::PlusAssign ||
           type == TokenType::MinusAssign || type == TokenType::StarAssign ||
           type == TokenType::SlashAssign || type == TokenType::PercentAssign;
}

const char* Token::typeName(TokenType type) {
    switch (type) {
        case TokenType::Number: return "Number";
        case TokenType::String: return "String";
        case TokenType::Identifier: return "Identifier";
        case TokenType::Var: return "var";
        case TokenType::Let: return "let";
        case TokenType::Const: return "const";
        case TokenType::Function: return "function";
        case TokenType::Return: return "return";
        case TokenType::If: return "if";
        case TokenType::Else: return "else";
        case TokenType::For: return "for";
        case TokenType::While: return "while";
        case TokenType::Class: return "class";
        case TokenType::Extends: return "extends";
        case TokenType::Super: return "super";
        case TokenType::Static: return "static";
        case TokenType::This: return "this";
        case TokenType::New: return "new";
        case TokenType::Async: return "async";
        case TokenType::Await: return "await";
        case TokenType::Yield: return "yield";
        case TokenType::Get: return "get";
        case TokenType::Set: return "set";
        case TokenType::Arrow: return "=>";
        case TokenType::DotDotDot: return "...";
        case TokenType::Dot: return ".";
        case TokenType::Comma: return ",";
        case TokenType::Colon: return ":";
        case TokenType::LeftBracket: return "[";
        case TokenType::RightBracket: return "]";
        case TokenType::Plus: return "+";
        case TokenType::Minus: return "-";
        case TokenType::Star: return "*";
        case TokenType::Slash: return "/";
        case TokenType::Equal: return "==";
        case TokenType::StrictEqual: return "===";
        case TokenType::Assign: return "=";
        case TokenType::LeftParen: return "(";
        case TokenType::RightParen: return ")";
        case TokenType::LeftBrace: return "{";
        case TokenType::RightBrace: return "}";
        case TokenType::Semicolon: return ";";
        case TokenType::EndOfFile: return "EOF";
        default: return "Token";
    }
}

bool isKeyword(std::string_view str) {
    return keywords.count(str) > 0;
}

TokenType keywordType(std::string_view str) {
    auto it = keywords.find(str);
    if (it != keywords.end()) {
        return it->second;
    }
    return TokenType::Identifier;
}

} // namespace Zepra::Frontend
