#pragma once

/**
 * @file token.hpp
 * @brief Token definitions for the ZepraScript lexer
 */

#include "../config.hpp"
#include <string>
#include <vector>
#include <string_view>

namespace Zepra::Frontend {

/**
 * @brief Token types in JavaScript
 */
enum class TokenType : uint8 {
    // Literals
    Number,
    String,
    Identifier,
    RegExp,
    Template,
    TemplateHead,
    TemplateMiddle,
    TemplateTail,
    PrivateName,    // #name (value includes the '#')
    
    // Keywords
    Var,
    Let,
    Const,
    Function,
    Return,
    If,
    Else,
    For,
    While,
    Do,
    Break,
    Continue,
    Switch,
    Case,
    Default,
    Try,
    Catch,
    Finally,
    Throw,
    New,
    Delete,
    Typeof,
    Instanceof,
    In,
    This,
    Class,
    Extends,
    Super,
    Static,
    Import,
    Export,
    From,
    As,
    Async,
    Await,
    Yield,
    Null,
    Undefined,
    True,
    False,
    Void,
    With,
    Debugger,
    Of,
    Get,
    Set,
    
    // Operators
    Plus,           // +
    Minus,          // -
    Star,           // *
    Slash,          // /
    Percent,        // %
    StarStar,       // **
    PlusPlus,       // ++
    MinusMinus,     // --
    
    // Comparison
    Equal,          // ==
    StrictEqual,    // ===
    NotEqual,       // !=
    StrictNotEqual, // !==
    Less,           // <
    LessEqual,      // <=
    Greater,        // >
    GreaterEqual,   // >=
    
    // Logical
    And,            // &&
    Or,             // ||
    Not,            // !
    Question,       // ?
    QuestionQuestion, // ??
    QuestionDot,    // ?.
    
    // Bitwise
    Ampersand,      // &
    Pipe,           // |
    Caret,          // ^
    Tilde,          // ~
    LeftShift,      // <<
    RightShift,     // >>
    UnsignedRightShift, // >>>
    
    // Assignment
    Assign,         // =
    PlusAssign,     // +=
    MinusAssign,    // -=
    StarAssign,     // *=
    SlashAssign,    // /=
    PercentAssign,  // %=
    StarStarAssign, // **=
    AmpersandAssign,// &=
    PipeAssign,     // |=
    CaretAssign,    // ^=
    LeftShiftAssign,// <<=
    RightShiftAssign,// >>=
    UnsignedRightShiftAssign, // >>>=
    AndAssign,      // &&=
    OrAssign,       // ||=
    QuestionQuestionAssign, // ??=
    
    // Punctuation
    LeftParen,      // (
    RightParen,     // )
    LeftBrace,      // {
    RightBrace,     // }
    LeftBracket,    // [
    RightBracket,   // ]
    Comma,          // ,
    Semicolon,      // ;
    Colon,          // :
    Dot,            // .
    DotDotDot,      // ...
    Arrow,          // =>
    
    // Special
    LineTerminator, // \n
    EndOfFile,
    Error,
    
    // Count
    TokenCount
};

/**
 * @brief Source location information
 */
struct SourceLocation {
    uint32_t line = 1;
    uint32_t column = 1;
    uint32_t offset = 0;
    
    std::string toString() const;
};

/**
 * @brief A token in the source code
 */
struct Token {
    TokenType type = TokenType::Error;
    std::string value;
    SourceLocation start;
    SourceLocation end;
    
    // For numeric literals
    double numericValue = 0.0;
    
    // For string literals
    bool hasEscapes = false;

    // For template literals: the raw text of each string part (String.raw,
    // tagged templates); `value` holds the cooked text
    std::vector<std::string> templateRaw;
    
    bool isKeyword() const;
    bool isOperator() const;
    bool isPunctuation() const;
    bool isLiteral() const;
    bool isAssignment() const;
    
    std::string toString() const;
    static const char* typeName(TokenType type);
};

/**
 * @brief Check if a string is a JavaScript keyword
 */
bool isKeyword(std::string_view str);

/**
 * @brief Get the TokenType for a keyword string
 */
TokenType keywordType(std::string_view str);

} // namespace Zepra::Frontend
