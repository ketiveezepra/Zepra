/**
 * @file nx_pdf_lexer.cpp
 * @brief Tokenizer and scanner for PDF syntax streams
 */

#include "nx_pdf_lexer.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace nxrender {
namespace pdf {
namespace parser {

Lexer::Lexer(std::string_view buffer) : buffer_(buffer), position_(0) {}

void Lexer::Reset() {
    position_ = 0;
}

bool Lexer::IsPdfWhitespace(char c) const {
    return c == 0x00 || c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

bool Lexer::IsDelimiter(char c) const {
    return c == '(' || c == ')' || c == '<' || c == '>' || 
           c == '[' || c == ']' || c == '{' || c == '}' || 
           c == '/' || c == '%';
}

void Lexer::SkipWhitespaceAndComments() {
    while (position_ < buffer_.size()) {
        char c = buffer_[position_];
        if (IsPdfWhitespace(c)) {
            position_++;
        } else if (c == '%') {
            // comment: skip to End of Line
            while (position_ < buffer_.size() && buffer_[position_] != 0x0A && buffer_[position_] != 0x0D) {
                position_++;
            }
        } else {
            break;
        }
    }
}

Token Lexer::NextToken() {
    SkipWhitespaceAndComments();
    
    if (position_ >= buffer_.size()) {
        Token t;
        t.type = TokenType::EndOfFile;
        return t;
    }

    char c = buffer_[position_];

    // Check single character delimiters
    if (c == '[') {
        Token t{TokenType::ArrayStart, buffer_.substr(position_, 1)};
        position_++;
        return t;
    }
    if (c == ']') {
        Token t{TokenType::ArrayEnd, buffer_.substr(position_, 1)};
        position_++;
        return t;
    }
    
    // Check multi-character delimiters
    if (c == '<' && position_ + 1 < buffer_.size() && buffer_[position_+1] == '<') {
        Token t{TokenType::DictStart, buffer_.substr(position_, 2)};
        position_ += 2;
        return t;
    }
    if (c == '>' && position_ + 1 < buffer_.size() && buffer_[position_+1] == '>') {
        Token t{TokenType::DictEnd, buffer_.substr(position_, 2)};
        position_ += 2;
        return t;
    }
    
    // Check strings and hex
    if (c == '(') return ParseStringLiteral();
    if (c == '<') return ParseHexString();
    
    // Check Names
    if (c == '/') return ParseName();

    // Check Numbers, Keywords, and booleans
    return ParseNumberOrKeyword();
}

Token Lexer::ParseStringLiteral() {
    size_t start = position_;
    position_++; // skip '('
    int openParens = 1;
    
    std::string unescaped;
    // Handles nested parentheses per PDF specification
    while (position_ < buffer_.size() && openParens > 0) {
        char c = buffer_[position_];
        if (c == '\\' && position_ + 1 < buffer_.size()) {
            // skip escaped character verbatim
            // proper impl should handle \n, \r, \t, \b, \f, \(, \), \\, \ddd
            unescaped += buffer_[position_ + 1];
            position_ += 2;
            continue;
        } else if (c == '(') {
            openParens++;
            unescaped += c;
        } else if (c == ')') {
            openParens--;
            if (openParens > 0) unescaped += c;
        } else {
            unescaped += c;
        }
        position_++;
    }
    
    Token t;
    t.type = TokenType::StringLiteral;
    t.lexeme = buffer_.substr(start, position_ - start);
    t.stringValue = std::move(unescaped);
    return t;
}

Token Lexer::ParseHexString() {
    size_t start = position_;
    position_++; // skip '<'
    
    std::string hexParsed;
    while (position_ < buffer_.size() && buffer_[position_] != '>') {
        char c = buffer_[position_];
        if (!IsPdfWhitespace(c)) {
            hexParsed += c;
        }
        position_++;
    }
    if (position_ < buffer_.size() && buffer_[position_] == '>') {
         position_++;
    }
    
    // Decode Hex logic bounding pairs
    std::string decoded;
    if (hexParsed.size() % 2 != 0) {
        hexParsed += '0'; // PDF spec says missing last digit is assumed 0
    }
    for (size_t i = 0; i < hexParsed.size(); i += 2) {
        char high = hexParsed[i];
        char low = hexParsed[i+1];
        
        uint8_t byte = 0;
        if (high >= '0' && high <= '9') byte = (high - '0') << 4;
        else if (high >= 'a' && high <= 'f') byte = (high - 'a' + 10) << 4;
        else if (high >= 'A' && high <= 'F') byte = (high - 'A' + 10) << 4;
        
        if (low >= '0' && low <= '9') byte |= (low - '0');
        else if (low >= 'a' && low <= 'f') byte |= (low - 'a' + 10);
        else if (low >= 'A' && low <= 'F') byte |= (low - 'A' + 10);
        
        decoded += static_cast<char>(byte);
    }
    
    Token t;
    t.type = TokenType::HexString;
    t.lexeme = buffer_.substr(start, position_ - start);
    t.stringValue = std::move(decoded);
    return t;
}

Token Lexer::ParseName() {
    size_t start = position_;
    position_++; // skip '/'
    
    while (position_ < buffer_.size()) {
        char c = buffer_[position_];
        if (IsPdfWhitespace(c) || IsDelimiter(c)) {
            break;
        }
        position_++;
    }
    
    Token t;
    t.type = TokenType::Name;
    t.lexeme = buffer_.substr(start, position_ - start);
    // Name processing: omit the starting slash
    t.stringValue = std::string(t.lexeme.substr(1)); 
    return t;
}

Token Lexer::ParseNumberOrKeyword() {
    size_t start = position_;
    bool hasDot = false;
    
    while (position_ < buffer_.size()) {
        char c = buffer_[position_];
        if (IsPdfWhitespace(c) || IsDelimiter(c)) break;
        if (c == '.') hasDot = true;
        position_++;
    }
    
    std::string_view word = buffer_.substr(start, position_ - start);
    
    Token t;
    t.lexeme = word;
    
    if (word == "true") {
        t.type = TokenType::Boolean;
        t.boolValue = true;
    } else if (word == "false") {
        t.type = TokenType::Boolean;
        t.boolValue = false;
    } else if (word == "null") {
        t.type = TokenType::Null;
    } else {
        bool isNumber = true;
        for (char c : word) {
            if (!std::isdigit(c) && c != '.' && c != '+' && c != '-') {
                isNumber = false;
                break;
            }
        }
        
        if (isNumber) {
            if (hasDot) {
                t.type = TokenType::Real;
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
                std::from_chars(word.data(), word.data() + word.size(), t.realValue);
#else
                // libc++ without floating-point from_chars (Apple Clang)
                t.realValue = std::strtod(std::string(word).c_str(), nullptr);
#endif
            } else {
                t.type = TokenType::Integer;
                std::from_chars(word.data(), word.data() + word.size(), t.intValue);
            }
        } else {
            t.type = TokenType::Keyword;
        }
    }
    
    return t;
}

} // namespace parser
} // namespace pdf
} // namespace nxrender
