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
                advance();
                break;
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
                return;
        }
    }
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
    if (isIdentifierStart(c)) {
        Token tok = scanIdentifierOrKeyword();
        updateDivisionContext(tok.type);
        return tok;
    }
    
    // Numbers
    if (isDigit(c) || (c == '.' && isDigit(peek(0)))) {
        Token tok = scanNumber();
        updateDivisionContext(tok.type);
        return tok;
    }
    
    // Strings
    if (c == '"' || c == '\'') {
        Token tok = scanString(c);
        updateDivisionContext(tok.type);
        return tok;
    }
    
    // Template literals
    if (c == '`') {
        Token tok = scanTemplate();
        updateDivisionContext(tok.type);
        return tok;
    }
    
    // Operators and punctuation
    Token tok = scanOperator();
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

Token Lexer::scanNumber() {
    size_t start = offset_ - 1;
    
    // Handle hex, octal, binary
    if (src_[start] == '0' && offset_ < srcLen_) {
        char next = current();
        if (next == 'x' || next == 'X') {
            advance();
            while (isHexDigit(current())) advance();
            std::string value(src_ + start, offset_ - start);
            Token token = makeToken(TokenType::Number, value);
            token.numericValue = std::strtol(value.c_str() + 2, nullptr, 16);
            return token;
        }
        if (next == 'b' || next == 'B') {
            advance();
            while (current() == '0' || current() == '1') advance();
            std::string value(src_ + start, offset_ - start);
            Token token = makeToken(TokenType::Number, value);
            token.numericValue = std::strtol(value.c_str() + 2, nullptr, 2);
            return token;
        }
        if (next == 'o' || next == 'O') {
            advance();
            while (current() >= '0' && current() <= '7') advance();
            std::string value(src_ + start, offset_ - start);
            Token token = makeToken(TokenType::Number, value);
            token.numericValue = std::strtol(value.c_str() + 2, nullptr, 8);
            return token;
        }
    }
    
    // Integer part
    while (isDigit(current())) advance();
    
    // Decimal part
    if (current() == '.' && isDigit(peek(1))) {
        advance();
        while (isDigit(current())) advance();
    }
    
    // Exponent
    if (current() == 'e' || current() == 'E') {
        advance();
        if (current() == '+' || current() == '-') advance();
        while (isDigit(current())) advance();
    }
    
    std::string value(src_ + start, offset_ - start);
    Token token = makeToken(TokenType::Number, value);
    token.numericValue = std::strtod(value.c_str(), nullptr);
    return token;
}

Token Lexer::scanString(char quote) {
    std::string value;
    bool hasEscapes = false;
    
    while (current() != '\0' && current() != quote) {
        if (current() == '\\') {
            hasEscapes = true;
            advance();
            switch (current()) {
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                case 'b': value += '\b'; break;
                case 'f': value += '\f'; break;
                case 'v': value += '\v'; break;
                case '0': value += '\0'; break;
                case '\\': value += '\\'; break;
                case '\'': value += '\''; break;
                case '"': value += '"'; break;
                case 'u': {
                    advance();
                    uint32_t codepoint = 0;
                    if (current() == '{') {
                        // \u{XXXXX} — 1-6 hex digits
                        advance();
                        int digits = 0;
                        while (current() != '}' && current() != '\0' && digits < 6) {
                            if (!isHexDigit(current())) {
                                addError("Invalid Unicode escape");
                                break;
                            }
                            codepoint = (codepoint << 4) | (isDigit(current()) ? current() - '0' :
                                (current() >= 'a' ? current() - 'a' + 10 : current() - 'A' + 10));
                            advance();
                            digits++;
                        }
                        if (current() == '}') advance();
                    } else {
                        // \uXXXX — exactly 4 hex digits
                        for (int i = 0; i < 4; i++) {
                            if (!isHexDigit(current())) {
                                addError("Invalid Unicode escape");
                                codepoint = 0xFFFD;
                                break;
                            }
                            codepoint = (codepoint << 4) | (isDigit(current()) ? current() - '0' :
                                (current() >= 'a' ? current() - 'a' + 10 : current() - 'A' + 10));
                            advance();
                        }
                    }
                    // Encode codepoint as UTF-8
                    if (codepoint <= 0x7F) {
                        value += static_cast<char>(codepoint);
                    } else if (codepoint <= 0x7FF) {
                        value += static_cast<char>(0xC0 | (codepoint >> 6));
                        value += static_cast<char>(0x80 | (codepoint & 0x3F));
                    } else if (codepoint <= 0xFFFF) {
                        value += static_cast<char>(0xE0 | (codepoint >> 12));
                        value += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                        value += static_cast<char>(0x80 | (codepoint & 0x3F));
                    } else if (codepoint <= 0x10FFFF) {
                        value += static_cast<char>(0xF0 | (codepoint >> 18));
                        value += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
                        value += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                        value += static_cast<char>(0x80 | (codepoint & 0x3F));
                    } else {
                        // Invalid codepoint — replacement character
                        value += "\xEF\xBF\xBD";
                    }
                    // Don't advance again — the loop's advance() handles the next char
                    continue;
                }
                default:
                    value += current();
                    break;
            }
            advance();
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
    
    while (isIdentifierPart(current())) {
        advance();
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
    // Scan template literal: `Hello ${name}!`
    // Returns Template token with parts and expressions
    
    std::string value;
    bool hasExpressions = false;
    
    while (current() != '\0' && current() != '`') {
        if (current() == '\\') {
            // Escape sequence
            advance();
            char escaped = advance();
            switch (escaped) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case 'r': value += '\r'; break;
                case '\\': value += '\\'; break;
                case '`': value += '`'; break;
                case '$': value += '$'; break;
                default: value += escaped; break;
            }
        } else if (current() == '$' && peek(1) == '{') {
            // Embedded expression: ${...}
            hasExpressions = true;
            
            // Mark where expression starts
            value += "\x01"; // Use special marker for parser
            advance(); // $
            advance(); // {
            
            // Scan until } (with nesting support)
            int braceDepth = 1;
            while (current() != '\0' && braceDepth > 0) {
                if (current() == '{') braceDepth++;
                else if (current() == '}') braceDepth--;
                
                if (braceDepth > 0) {
                    value += advance();
                }
            }
            
            if (current() == '}') {
                advance(); // Close }
                value += "\x02"; // End expression marker
            } else {
                return errorToken("Unterminated template expression");
            }
        } else {
            value += advance();
        }
    }
    
    if (current() != '`') {
        return errorToken("Unterminated template literal");
    }
    advance(); // Closing `
    
    return makeToken(TokenType::Template, value);
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
            if (match('.')) return makeToken(TokenType::QuestionDot);
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
    return isAlpha(c) || c == '_' || c == '$';
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
