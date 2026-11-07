#pragma once

/**
 * @file lexer.hpp
 * @brief JavaScript tokenizer
 */

#include "token.hpp"
#include "source_code.hpp"
#include <memory>
#include <vector>
#include <string>

namespace Zepra::Frontend {

/**
 * @brief JavaScript lexer/tokenizer
 * 
 * Converts JavaScript source code into a stream of tokens.
 */
class Lexer {
public:
    /**
     * @brief Create a lexer for the given source
     */
    explicit Lexer(const SourceCode* source);
    
    /**
     * @brief Get the next token
     */
    Token nextToken();
    
    /**
     * @brief Peek at the next token without consuming it
     */
    const Token& peek();
    
    /**
     * @brief Check if at end of file
     */
    bool isEof() const { return offset_ >= srcLen_; }
    
    /**
     * @brief Get current position
     */
    const SourceLocation& currentLocation() const { return location_; }
    
    /**
     * @brief Get current offset (for debug progress tracking)
     */
    size_t offset() const { return offset_; }
    
    /**
     * @brief Get all errors encountered
     */
    const std::vector<std::string>& errors() const { return errors_; }
    
    /**
     * @brief Check if any errors occurred
     */
    bool hasErrors() const { return !errors_.empty(); }
    
    /**
     * @brief Get current lexer state for backtracking
     */
    struct LexerState {
        size_t offset;
        SourceLocation location;
        Token peekedToken;
        bool hasPeeked;
        bool canFollowDivision;
    };
    
    LexerState checkpoint() const {
        return {offset_, location_, peekedToken_, hasPeeked_, canFollowDivision_};
    }
    
    /**
     * @brief Restore lexer to a previous state
     */
    void restore(const LexerState& state) {
        offset_ = state.offset;
        location_ = state.location;
        peekedToken_ = state.peekedToken;
        hasPeeked_ = state.hasPeeked;
        canFollowDivision_ = state.canFollowDivision;
    }
    
private:
    // Character reading
    char current() const;
    char peek(size_t ahead) const;
    char advance();
    bool match(char expected);
    void skipWhitespace();
    void skipLineComment();
    void skipBlockComment();
    
    // Token scanning
    Token scanNumber();
    Token scanString(char quote);
    Token scanIdentifierOrKeyword();
    Token scanRegExp();
    Token scanTemplate();
    Token scanOperator();
    // After a backslash: consumes one escape sequence and appends its value
    void scanEscape(std::string& out);
    // After `${`: moves to the `}` that ends the substitution (skipping
    // strings, nested templates and comments); false at end of input
    bool skipTemplateSubstitution();
    void skipNestedTemplate();
    
    // Utilities
    Token makeToken(TokenType type);
    Token makeToken(TokenType type, std::string value);
    Token errorToken(const std::string& message);
    void addError(const std::string& message);
    
    bool isDigit(char c) const;
    bool isHexDigit(char c) const;
    bool isAlpha(char c) const;
    bool isAlphaNumeric(char c) const;
    bool isIdentifierStart(char c) const;
    bool isIdentifierPart(char c) const;
    void updateDivisionContext(TokenType type);
    
    const SourceCode* source_;
    const char* src_;          // Cached raw pointer into source content
    size_t srcLen_;            // Cached source length
    size_t offset_ = 0;
    SourceLocation location_;
    SourceLocation tokenStart_;
    
    Token peekedToken_;
    bool hasPeeked_ = false;
    
    std::vector<std::string> errors_;
    
    // Context for regex vs division disambiguation
    bool canFollowDivision_ = false;
};

} // namespace Zepra::Frontend
