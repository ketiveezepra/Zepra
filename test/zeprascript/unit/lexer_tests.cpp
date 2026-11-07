// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

// Lexer unit tests
#include <gtest/gtest.h>
#include "frontend/lexer.hpp"
#include "frontend/source_code.hpp"

using namespace Zepra::Frontend;

TEST(LexerTests, EmptySource) {
    auto source = SourceCode::fromString("");
    Lexer lexer(source.get());
    Token tok = lexer.nextToken();
    EXPECT_EQ(tok.type, TokenType::EndOfFile);
}

TEST(LexerTests, Numbers) {
    auto source = SourceCode::fromString("42 3.14 0xFF 0b1010");
    Lexer lexer(source.get());
    
    Token tok = lexer.nextToken();
    EXPECT_EQ(tok.type, TokenType::Number);
    EXPECT_EQ(tok.numericValue, 42.0);
    
    tok = lexer.nextToken();
    EXPECT_EQ(tok.type, TokenType::Number);
    EXPECT_NEAR(tok.numericValue, 3.14, 0.001);
    
    tok = lexer.nextToken();
    EXPECT_EQ(tok.type, TokenType::Number);
    EXPECT_EQ(tok.numericValue, 255.0);
    
    tok = lexer.nextToken();
    EXPECT_EQ(tok.type, TokenType::Number);
    EXPECT_EQ(tok.numericValue, 10.0);
}

TEST(LexerTests, Strings) {
    auto source = SourceCode::fromString("\"hello\" 'world'");
    Lexer lexer(source.get());
    
    Token tok = lexer.nextToken();
    EXPECT_EQ(tok.type, TokenType::String);
    EXPECT_EQ(tok.value, "hello");
    
    tok = lexer.nextToken();
    EXPECT_EQ(tok.type, TokenType::String);
    EXPECT_EQ(tok.value, "world");
}

TEST(LexerTests, Keywords) {
    auto source = SourceCode::fromString("var let const function return if else");
    Lexer lexer(source.get());
    
    EXPECT_EQ(lexer.nextToken().type, TokenType::Var);
    EXPECT_EQ(lexer.nextToken().type, TokenType::Let);
    EXPECT_EQ(lexer.nextToken().type, TokenType::Const);
    EXPECT_EQ(lexer.nextToken().type, TokenType::Function);
    EXPECT_EQ(lexer.nextToken().type, TokenType::Return);
    EXPECT_EQ(lexer.nextToken().type, TokenType::If);
    EXPECT_EQ(lexer.nextToken().type, TokenType::Else);
}

TEST(LexerTests, Operators) {
    // '/' only means division after an operand, so it follows a number here.
    auto source = SourceCode::fromString("+ - * 1 / 2 === !== && ||");
    Lexer lexer(source.get());

    EXPECT_EQ(lexer.nextToken().type, TokenType::Plus);
    EXPECT_EQ(lexer.nextToken().type, TokenType::Minus);
    EXPECT_EQ(lexer.nextToken().type, TokenType::Star);
    EXPECT_EQ(lexer.nextToken().type, TokenType::Number);
    EXPECT_EQ(lexer.nextToken().type, TokenType::Slash);
    EXPECT_EQ(lexer.nextToken().type, TokenType::Number);
    EXPECT_EQ(lexer.nextToken().type, TokenType::StrictEqual);
    EXPECT_EQ(lexer.nextToken().type, TokenType::StrictNotEqual);
    EXPECT_EQ(lexer.nextToken().type, TokenType::And);
    EXPECT_EQ(lexer.nextToken().type, TokenType::Or);
}

TEST(LexerTests, SlashAfterOperatorIsRegExp) {
    auto source = SourceCode::fromString("* /ab+/g");
    Lexer lexer(source.get());

    EXPECT_EQ(lexer.nextToken().type, TokenType::Star);
    EXPECT_EQ(lexer.nextToken().type, TokenType::RegExp);
}

TEST(LexerTests, ByteOrderMarkBeforeTheFirstToken) {
    auto source = SourceCode::fromString("\xEF\xBB\xBFvar x");
    Lexer lexer(source.get());
    Token tok = lexer.nextToken();
    EXPECT_EQ(tok.type, TokenType::Var);
    EXPECT_EQ(tok.start.line, 1u);
}

TEST(LexerTests, TemplateCookedAndRawParts) {
    auto source = SourceCode::fromString("`a\\n${ \"}\" + `x${1}` }b\\x41`");
    Lexer lexer(source.get());
    Token tok = lexer.nextToken();
    ASSERT_EQ(tok.type, TokenType::Template);
    // The substitution's source sits between \x01 and \x02, whole
    EXPECT_EQ(tok.value, std::string("a\n\x01 \"}\" + `x${1}` \x02" "bA"));
    ASSERT_EQ(tok.templateRaw.size(), 2u);
    EXPECT_EQ(tok.templateRaw[0], "a\\n");
    EXPECT_EQ(tok.templateRaw[1], "b\\x41");
    EXPECT_EQ(lexer.nextToken().type, TokenType::EndOfFile);
}

TEST(LexerTests, LineContinuationInStringsAndTemplates) {
    auto source = SourceCode::fromString("'a\\\nb' `c\\\r\nd`");
    Lexer lexer(source.get());
    Token tok = lexer.nextToken();
    ASSERT_EQ(tok.type, TokenType::String);
    EXPECT_EQ(tok.value, "ab");
    tok = lexer.nextToken();
    ASSERT_EQ(tok.type, TokenType::Template);
    EXPECT_EQ(tok.value, "cd");
}
