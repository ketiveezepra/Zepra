// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file parser.cpp
 * @brief JavaScript recursive descent parser implementation
 */

#include "frontend/parser.hpp"
#include <algorithm>
#include "frontend/ast.hpp"
#include <stdexcept>

namespace Zepra::Frontend {

Parser::Parser(const SourceCode* source) : lexer_(source) {
    currentToken_ = lexer_.nextToken();
}

std::unique_ptr<Program> Parser::parseProgram() {
    std::vector<StmtPtr> body;
    
    while (!isAtEnd()) {
        const uint32_t startOffset = currentToken_.start.offset;
        const TokenType startType = currentToken_.type;
        try {
            auto stmt = parseDeclaration();
            if (stmt) {
                body.push_back(std::move(stmt));
            }
        } catch (...) {
            synchronize();
        }
        // Every iteration must consume input. A statement parser can return
        // without consuming (e.g. on a lexer error token such as an
        // unterminated string), which would otherwise loop forever.
        if (!isAtEnd() && currentToken_.start.offset == startOffset &&
            currentToken_.type == startType) {
            error(startType == TokenType::Error ? currentToken_.value : "Unexpected token");
            advance();
        }
    }
    
    return std::make_unique<Program>(std::move(body));
}

// Token handling
Token& Parser::current() { return currentToken_; }

const Token& Parser::peek() { 
    return lexer_.peek(); 
}

Token Parser::advance() {
    previousToken_ = currentToken_;
    if (!isAtEnd()) {
        currentToken_ = lexer_.nextToken();
    }
    return previousToken_;
}

bool Parser::check(TokenType type) {
    return currentToken_.type == type;
}

bool Parser::match(TokenType type) {
    if (!check(type)) return false;
    advance();
    return true;
}

bool Parser::match(std::initializer_list<TokenType> types) {
    for (TokenType type : types) {
        if (check(type)) {
            advance();
            return true;
        }
    }
    return false;
}

Token Parser::consume(TokenType type, const std::string& message) {
    if (check(type)) return advance();
    error(currentToken_, message);
    return currentToken_;
}

bool Parser::isAtEnd() {
    return currentToken_.type == TokenType::EndOfFile;
}

// Check if current token can be used as property name (identifier OR keyword)
static bool isPropertyName(TokenType type) {
    if (type == TokenType::Identifier) return true;
    // All these keywords are valid as property names after dot
    switch (type) {
        case TokenType::Var: case TokenType::Let: case TokenType::Const:
        case TokenType::Function: case TokenType::Return: case TokenType::If:
        case TokenType::Else: case TokenType::For: case TokenType::While:
        case TokenType::Do: case TokenType::Break: case TokenType::Continue:
        case TokenType::Switch: case TokenType::Case: case TokenType::Default:
        case TokenType::Try: case TokenType::Catch: case TokenType::Finally:
        case TokenType::Throw: case TokenType::New: case TokenType::Delete:
        case TokenType::Typeof: case TokenType::Instanceof: case TokenType::In:
        case TokenType::This: case TokenType::Class: case TokenType::Extends:
        case TokenType::Super: case TokenType::Static: case TokenType::Import:
        case TokenType::Export: case TokenType::From: case TokenType::As:
        case TokenType::Async: case TokenType::Await: case TokenType::Yield:
        case TokenType::Null: case TokenType::Undefined: case TokenType::True:
        case TokenType::False: case TokenType::Void: case TokenType::With:
        case TokenType::Debugger: case TokenType::Of: case TokenType::Get:
        case TokenType::Set:
            return true;
        default:
            return false;
    }
}

void Parser::synchronize() {
    advance();
    
    while (!isAtEnd()) {
        if (previousToken_.type == TokenType::Semicolon) return;
        
        switch (currentToken_.type) {
            case TokenType::Function:
            case TokenType::Var:
            case TokenType::Let:
            case TokenType::Const:
            case TokenType::Class:
            case TokenType::For:
            case TokenType::If:
            case TokenType::While:
            case TokenType::Return:
                return;
            default:
                break;
        }
        
        advance();
    }
}

void Parser::error(const std::string& message) {
    error(currentToken_, message);
}

void Parser::error(const Token& token, const std::string& message) {
    std::string err = std::to_string(token.start.line) + ":" +
                      std::to_string(token.start.column) + ": " + message;
    errors_.push_back(err);
}

#ifndef NDEBUG
void Parser::assertProgress() {
    size_t current = lexer_.offset();
    if (current == lastTokenOffset_) {
        noProgressCount_++;
        if (noProgressCount_ > 5) {
            // Fatal: Parser loop detected - helps debug future issues
            error("FATAL: Parser made no progress - possible infinite loop at " + 
                  std::to_string(currentToken_.start.line) + ":" +
                  std::to_string(currentToken_.start.column));
            throw std::runtime_error("Parser infinite loop detected");
        }
    } else {
        noProgressCount_ = 0;
        lastTokenOffset_ = current;
    }
}
#endif

// Declaration parsing
StmtPtr Parser::parseDeclaration() {
    if (match(TokenType::Var) || match(TokenType::Let) || match(TokenType::Const)) {
        return parseVariableDeclaration();
    }
    // Handle function* generators
    if (match(TokenType::Function)) {
        bool isGenerator = match(TokenType::Star);
        return parseFunctionDeclaration(false, isGenerator);
    }
    // Handle 'async function' declarations
    if (match(TokenType::Async)) {
        if (match(TokenType::Function)) {
            bool isGenerator = match(TokenType::Star);  // async function*
            return parseFunctionDeclaration(true, isGenerator);
        }
        // Async arrow function or expression - fall through to parseStatement
    }
    if (match(TokenType::Class)) {
        return parseClassDeclaration();
    }
    if (match(TokenType::Import)) {
        return parseImportDeclaration();
    }
    if (match(TokenType::Export)) {
        return parseExportDeclaration();
    }
    return parseStatement();
}

StmtPtr Parser::parseVariableDeclaration() {
    VariableDecl::Kind kind;
    if (previousToken_.type == TokenType::Var) {
        kind = VariableDecl::Kind::Var;
    } else if (previousToken_.type == TokenType::Let) {
        kind = VariableDecl::Kind::Let;
    } else {
        kind = VariableDecl::Kind::Const;
    }
    
    std::vector<VariableDeclarator> declarators;
    
    do {
        ExprPtr id;

        // Destructuring pattern or identifier
        if (check(TokenType::LeftBrace) || check(TokenType::LeftBracket)) {
            id = parseBindingPattern();
        } else {
            Token name = consume(TokenType::Identifier, "Expected variable name or destructuring pattern");
            id = std::make_unique<IdentifierExpr>(name.value, name.start);
        }

        ExprPtr init = nullptr;
        if (match(TokenType::Assign)) {
            init = parseAssignmentExpression();
        }

        declarators.push_back({std::move(id), std::move(init)});
    } while (match(TokenType::Comma));
    
    // Semicolon is optional in some contexts
    match(TokenType::Semicolon);
    
    return std::make_unique<VariableDecl>(kind, std::move(declarators));
}

StmtPtr Parser::parseFunctionDeclaration(bool isAsync, bool isGenerator) {
    Token name = consume(TokenType::Identifier, "Expected function name");
    
    consume(TokenType::LeftParen, "Expected '(' after function name");
    std::vector<FunctionParam> params = parseParameters();
    consume(TokenType::RightParen, "Expected ')' after parameters");
    
    consume(TokenType::LeftBrace, "Expected '{' before function body");
    auto body = std::unique_ptr<BlockStmt>(
        static_cast<BlockStmt*>(parseBlockStatement().release())
    );
    
    return std::make_unique<FunctionDecl>(name.value, std::move(params), std::move(body), isAsync, isGenerator);
}

StmtPtr Parser::parseClassDeclaration() {
    // class Name extends SuperClass { constructor() {} method() {} }
    Token className = consume(TokenType::Identifier, "Expected class name");
    
    // extends SuperClass?
    ExprPtr superClass = nullptr;
    if (match(TokenType::Extends)) {
        superClass = parsePrimaryExpression();
    }
    
    consume(TokenType::LeftBrace, "Expected '{' before class body");
    
    // Parse class members
    std::unique_ptr<FunctionExpr> constructor = nullptr;
    std::vector<ClassDecl::MethodDef> methods;
    
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        bool isStatic = match(TokenType::Static);
        bool isGetter = match(TokenType::Get);
        bool isSetter = !isGetter && match(TokenType::Set);
        
        Token methodName = consume(TokenType::Identifier, "Expected method name");
        consume(TokenType::LeftParen, "Expected '(' after method name");
        std::vector<FunctionParam> params = parseParameters();
        consume(TokenType::RightParen, "Expected ')' after parameters");
        consume(TokenType::LeftBrace, "Expected '{' before method body");
        auto body = std::unique_ptr<BlockStmt>(static_cast<BlockStmt*>(parseBlockStatement().release()));
        
        auto methodFunc = std::make_unique<FunctionExpr>("", std::move(params), std::move(body));
        
        if (methodName.value == "constructor") {
            constructor = std::move(methodFunc);
        } else {
            ClassDecl::MethodDef method;
            method.name = methodName.value;
            method.function = std::move(methodFunc);
            method.isStatic = isStatic;
            method.isGetter = isGetter;
            method.isSetter = isSetter;
            methods.push_back(std::move(method));
        }
    }
    
    consume(TokenType::RightBrace, "Expected '}' after class body");
    
    if (!constructor) {
        constructor = std::make_unique<FunctionExpr>("", std::vector<FunctionParam>(), std::make_unique<BlockStmt>(std::vector<StmtPtr>()));
    }
    
    return std::make_unique<ClassDecl>(className.value, std::move(superClass), std::move(constructor), std::move(methods));
}

std::vector<FunctionParam> Parser::parseParameters() {
    std::vector<FunctionParam> params;
    
    if (!check(TokenType::RightParen)) {
        do {
            bool rest = match(TokenType::DotDotDot);

            FunctionParam param;
            param.rest = rest;

            if (check(TokenType::LeftBrace) || check(TokenType::LeftBracket)) {
                // Destructured parameter: function f({a, b}, [x, y]) {}
                param.pattern = parseBindingPattern();
            } else {
                Token name = consume(TokenType::Identifier, "Expected parameter name");
                param.pattern = std::make_unique<IdentifierExpr>(name.value, name.start);
            }

            if (match(TokenType::Assign)) {
                param.defaultValue = parseAssignmentExpression();
            }

            params.push_back(std::move(param));
            
            if (rest) break;
        } while (match(TokenType::Comma));
    }
    
    return params;
}

// Statement parsing
StmtPtr Parser::parseStatement() {
#ifndef NDEBUG
    assertProgress();
#endif
    if (match(TokenType::LeftBrace)) {
        return parseBlockStatement();
    }
    if (match(TokenType::If)) {
        return parseIfStatement();
    }
    if (match(TokenType::While)) {
        return parseWhileStatement();
    }
    if (match(TokenType::Do)) {
        return parseDoWhileStatement();
    }
    if (match(TokenType::For)) {
        return parseForStatement();
    }
    if (match(TokenType::Return)) {
        return parseReturnStatement();
    }
    if (match(TokenType::Break)) {
        return parseBreakStatement();
    }
    if (match(TokenType::Continue)) {
        return parseContinueStatement();
    }
    if (match(TokenType::Throw)) {
        return parseThrowStatement();
    }
    if (match(TokenType::Try)) {
        return parseTryStatement();
    }
    if (match(TokenType::Switch)) {
        return parseSwitchStatement();
    }
    if (match(TokenType::Semicolon)) {
        return std::make_unique<EmptyStmt>();
    }
    if (match(TokenType::Debugger)) {
        match(TokenType::Semicolon);
        return std::make_unique<EmptyStmt>();
    }
    
    // Labeled statement: identifier followed by colon
    // e.g., outer: for (;;) { break outer; }
    if (check(TokenType::Identifier) && peek().type == TokenType::Colon) {
        std::string label = currentToken_.value;
        SourceLocation loc = currentToken_.start;
        advance(); // consume identifier
        advance(); // consume colon
        return parseLabeledStatement(label);
    }
    
    return parseExpressionStatement();
}

StmtPtr Parser::parseBlockStatement() {
    std::vector<StmtPtr> statements;
    
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        auto stmt = parseDeclaration();
        if (stmt) {
            statements.push_back(std::move(stmt));
        }
    }
    
    consume(TokenType::RightBrace, "Expected '}' after block");
    
    return std::make_unique<BlockStmt>(std::move(statements));
}

StmtPtr Parser::parseIfStatement() {
    consume(TokenType::LeftParen, "Expected '(' after 'if'");
    ExprPtr condition = parseExpression();
    consume(TokenType::RightParen, "Expected ')' after condition");
    
    StmtPtr consequent = parseStatement();
    StmtPtr alternate = nullptr;
    
    if (match(TokenType::Else)) {
        alternate = parseStatement();
    }
    
    return std::make_unique<IfStmt>(std::move(condition), std::move(consequent), std::move(alternate));
}

StmtPtr Parser::parseWhileStatement() {
    consume(TokenType::LeftParen, "Expected '(' after 'while'");
    ExprPtr condition = parseExpression();
    consume(TokenType::RightParen, "Expected ')' after condition");
    
    bool prevInLoop = inLoop_;
    inLoop_ = true;
    StmtPtr body = parseStatement();
    inLoop_ = prevInLoop;
    
    return std::make_unique<WhileStmt>(std::move(condition), std::move(body));
}

StmtPtr Parser::parseDoWhileStatement() {
    bool prevInLoop = inLoop_;
    inLoop_ = true;
    StmtPtr body = parseStatement();
    inLoop_ = prevInLoop;
    
    consume(TokenType::While, "Expected 'while' after do body");
    consume(TokenType::LeftParen, "Expected '(' after 'while'");
    ExprPtr condition = parseExpression();
    consume(TokenType::RightParen, "Expected ')' after condition");
    match(TokenType::Semicolon);
    
    return std::make_unique<DoWhileStmt>(std::move(body), std::move(condition));
}

StmtPtr Parser::parseForStatement() {
    // for await (...of...) — detect await before '('
    bool isAwait = match(TokenType::Await);
    consume(TokenType::LeftParen, "Expected '(' after 'for'");
    
    // Check for for...of or for...in patterns
    // for (let/const/var x of iterable) or for (x of iterable)
    // Also supports destructuring: for (const {a, b} of iterable)
    
    ASTNodePtr init = nullptr;
    bool isForOf = false;
    
    if (match(TokenType::Semicolon)) {
        // No initializer - regular for loop
    } else if (match({TokenType::Var, TokenType::Let, TokenType::Const})) {
        TokenType declType = previousToken_.type;

        // Check for destructuring pattern in for loop
        ExprPtr loopVarId;
        if (check(TokenType::LeftBrace) || check(TokenType::LeftBracket)) {
            loopVarId = parseBindingPattern();
        } else {
            Token varName = consume(TokenType::Identifier, "Expected variable name or pattern");
            loopVarId = std::make_unique<IdentifierExpr>(varName.value, varName.start);
        }
        
        // Check if this is for...of or for...in
        if (match(TokenType::Of) || match(TokenType::In)) {
            bool isIn = (previousToken_.type == TokenType::In);
            
            ExprPtr iterable = parseAssignmentExpression();
            consume(TokenType::RightParen, isIn ? "Expected ')' after for...in" : "Expected ')' after for...of");
            
            bool prevInLoop = inLoop_;
            inLoop_ = true;
            StmtPtr body = parseStatement();
            inLoop_ = prevInLoop;
            
            VariableDecl::Kind kind;
            if (declType == TokenType::Var) kind = VariableDecl::Kind::Var;
            else if (declType == TokenType::Let) kind = VariableDecl::Kind::Let;
            else kind = VariableDecl::Kind::Const;
            
            std::vector<VariableDeclarator> declarators;
            declarators.push_back({std::move(loopVarId), nullptr});
            auto varDecl = std::make_unique<VariableDecl>(kind, std::move(declarators));
            
            if (isIn) {
                return std::make_unique<ForInStmt>(std::move(varDecl), std::move(iterable), std::move(body));
            }
            return std::make_unique<ForOfStmt>(std::move(varDecl), std::move(iterable), std::move(body), isAwait);
        }
        
        // Regular for loop with initializer
        ExprPtr initValue = nullptr;
        if (match(TokenType::Assign)) {
            initValue = parseAssignmentExpression();
        }
        ExprPtr id = std::move(loopVarId);
        
        std::vector<VariableDeclarator> declarators;
        declarators.push_back({std::move(id), std::move(initValue)});
        
        // Handle multiple declarators
        while (match(TokenType::Comma)) {
            Token name = consume(TokenType::Identifier, "Expected variable name");
            ExprPtr varId = std::make_unique<IdentifierExpr>(name.value, name.start);
            ExprPtr varInit = nullptr;
            if (match(TokenType::Assign)) {
                varInit = parseAssignmentExpression();
            }
            declarators.push_back({std::move(varId), std::move(varInit)});
        }
        
        VariableDecl::Kind kind;
        if (declType == TokenType::Var) kind = VariableDecl::Kind::Var;
        else if (declType == TokenType::Let) kind = VariableDecl::Kind::Let;
        else kind = VariableDecl::Kind::Const;
        
        init = std::make_unique<VariableDecl>(kind, std::move(declarators));
        consume(TokenType::Semicolon, "Expected ';' after for initializer");
    } else {
        // Expression or identifier that might be for...of / for...in
        // Suppress 'in' as binary operator so for(x in obj) works
        bool savedNoIn = noIn_;
        noIn_ = true;
        ExprPtr expr = parseExpression();
        noIn_ = savedNoIn;
        
        // Check if this is for...of or for...in with an existing identifier
        if (match(TokenType::Of) || match(TokenType::In)) {
            bool isIn = (previousToken_.type == TokenType::In);
            
            ExprPtr iterable = parseAssignmentExpression();
            consume(TokenType::RightParen, isIn ? "Expected ')' after for...in" : "Expected ')' after for...of");
            
            bool prevInLoop = inLoop_;
            inLoop_ = true;
            StmtPtr body = parseStatement();
            inLoop_ = prevInLoop;
            
            if (isIn) {
                return std::make_unique<ForInStmt>(std::move(expr), std::move(iterable), std::move(body));
            }
            return std::make_unique<ForOfStmt>(std::move(expr), std::move(iterable), std::move(body));
        }
        
        init = std::move(expr);
        consume(TokenType::Semicolon, "Expected ';' after for initializer");
    }
    
    // Regular for loop
    ExprPtr condition = nullptr;
    if (!check(TokenType::Semicolon)) {
        condition = parseExpression();
    }
    consume(TokenType::Semicolon, "Expected ';' after for condition");
    
    ExprPtr update = nullptr;
    if (!check(TokenType::RightParen)) {
        update = parseExpression();
    }
    consume(TokenType::RightParen, "Expected ')' after for clauses");
    
    bool prevInLoop = inLoop_;
    inLoop_ = true;
    StmtPtr body = parseStatement();
    inLoop_ = prevInLoop;
    
    return std::make_unique<ForStmt>(std::move(init), std::move(condition), 
                                      std::move(update), std::move(body));
}


StmtPtr Parser::parseSwitchStatement() {
    consume(TokenType::LeftParen, "Expected '(' after 'switch'");
    ExprPtr discriminant = parseExpression();
    consume(TokenType::RightParen, "Expected ')' after switch expression");
    
    consume(TokenType::LeftBrace, "Expected '{' before switch cases");
    
    std::vector<SwitchCase> cases;
    
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        SwitchCase switchCase;
        switchCase.location = currentToken_.start;
        
        if (match(TokenType::Case)) {
            // case expression:
            switchCase.test = parseExpression();
            consume(TokenType::Colon, "Expected ':' after case expression");
        } else if (match(TokenType::Default)) {
            // default:
            switchCase.test = nullptr;  // nullptr indicates default case
            consume(TokenType::Colon, "Expected ':' after 'default'");
        } else {
            error("Expected 'case' or 'default' in switch statement");
            break;
        }
        
        // Parse consequent statements until next case/default/closing brace
        while (!check(TokenType::Case) && !check(TokenType::Default) && 
               !check(TokenType::RightBrace) && !isAtEnd()) {
            auto stmt = parseDeclaration();
            if (stmt) {
                switchCase.consequent.push_back(std::move(stmt));
            }
        }
        
        cases.push_back(std::move(switchCase));
    }
    
    consume(TokenType::RightBrace, "Expected '}' after switch cases");
    
    return std::make_unique<SwitchStmt>(std::move(discriminant), std::move(cases));
}

StmtPtr Parser::parseTryStatement() {
    consume(TokenType::LeftBrace, "Expected '{' after 'try'");
    auto block = std::unique_ptr<BlockStmt>(
        static_cast<BlockStmt*>(parseBlockStatement().release())
    );
    
    std::unique_ptr<CatchClause> handler = nullptr;
    std::unique_ptr<BlockStmt> finalizer = nullptr;
    
    if (match(TokenType::Catch)) {
        handler = std::make_unique<CatchClause>();
        
        if (match(TokenType::LeftParen)) {
            Token param = consume(TokenType::Identifier, "Expected catch parameter");
            handler->param = param.value;
            consume(TokenType::RightParen, "Expected ')' after catch parameter");
        }
        
        consume(TokenType::LeftBrace, "Expected '{' after catch");
        handler->body = std::unique_ptr<BlockStmt>(
            static_cast<BlockStmt*>(parseBlockStatement().release())
        );
    }
    
    if (match(TokenType::Finally)) {
        consume(TokenType::LeftBrace, "Expected '{' after finally");
        finalizer = std::unique_ptr<BlockStmt>(
            static_cast<BlockStmt*>(parseBlockStatement().release())
        );
    }
    
    return std::make_unique<TryStmt>(std::move(block), std::move(handler), std::move(finalizer));
}

StmtPtr Parser::parseThrowStatement() {
    ExprPtr argument = parseExpression();
    match(TokenType::Semicolon);
    return std::make_unique<ThrowStmt>(std::move(argument));
}

StmtPtr Parser::parseReturnStatement() {
    ExprPtr argument = nullptr;
    
    if (!check(TokenType::Semicolon) && !check(TokenType::RightBrace) && !isAtEnd()) {
        argument = parseExpression();
    }
    
    match(TokenType::Semicolon);
    return std::make_unique<ReturnStmt>(std::move(argument));
}

StmtPtr Parser::parseBreakStatement() {
    std::string label;
    if (check(TokenType::Identifier)) {
        label = advance().value;
    }
    match(TokenType::Semicolon);
    return std::make_unique<BreakStmt>(label);
}

StmtPtr Parser::parseContinueStatement() {
    std::string label;
    if (check(TokenType::Identifier)) {
        label = advance().value;
    }
    match(TokenType::Semicolon);
    return std::make_unique<ContinueStmt>(label);
}

StmtPtr Parser::parseExpressionStatement() {
    ExprPtr expr = parseExpression();
    match(TokenType::Semicolon);
    return std::make_unique<ExprStmt>(std::move(expr));
}

StmtPtr Parser::parseLabeledStatement(const std::string& label) {
    StmtPtr body = parseStatement();
    return std::make_unique<LabeledStmt>(label, std::move(body));
}

// Expression parsing - uses precedence climbing
ExprPtr Parser::parseExpression() {
#ifndef NDEBUG
    assertProgress();
#endif
    ExprPtr expr = parseAssignmentExpression();
    
    // Comma operator: (a, b, c) evaluates all, returns last
    while (match(TokenType::Comma)) {
        ExprPtr right = parseAssignmentExpression();
        expr = std::make_unique<BinaryExpr>(TokenType::Comma, std::move(expr), std::move(right));
    }
    
    return expr;
}

ExprPtr Parser::parseAssignmentExpression() {
    // Check for arrow function pattern: identifier => expr
    // or (params) => expr
    if (check(TokenType::Identifier)) {
        // Save lexer state for proper backtracking
        auto savedLexerState = lexer_.checkpoint();
        Token savedToken = currentToken_;
        Token id = advance();
        
        if (match(TokenType::Arrow)) {
            // Single parameter arrow: x => ...
            std::vector<FunctionParam> params;
            FunctionParam param;
            param.pattern = std::make_unique<IdentifierExpr>(id.value);
            params.push_back(std::move(param));
            
            // Parse body
            std::variant<std::unique_ptr<BlockStmt>, ExprPtr> body;
            if (check(TokenType::LeftBrace)) {
                advance(); // consume {
                body = std::unique_ptr<BlockStmt>(
                    static_cast<BlockStmt*>(parseBlockStatement().release())
                );
            } else {
                body = parseAssignmentExpression();
            }
            
            return std::make_unique<ArrowFunctionExpr>(
                std::move(params), std::move(body)
            );
        }
        
        // Not arrow, restore lexer state completely
        lexer_.restore(savedLexerState);
        currentToken_ = savedToken;
    }
    
    // Check for (params) => expr pattern
    if (check(TokenType::LeftParen)) {
        // Save lexer state for proper backtracking
        auto savedLexerState = lexer_.checkpoint();
        Token savedToken = currentToken_;
        advance(); // consume (
        
        // Try to parse as parameters
        std::vector<FunctionParam> params;
        bool couldBeArrow = true;
        
        if (!check(TokenType::RightParen)) {
            // Parse parameter list
            do {
                if (!check(TokenType::Identifier)) {
                    couldBeArrow = false;
                    break;
                }
                
                FunctionParam param;
                Token paramName = advance();
                param.pattern = std::make_unique<IdentifierExpr>(paramName.value);
                params.push_back(std::move(param));
            } while (match(TokenType::Comma));
        }
        
        if (couldBeArrow && match(TokenType::RightParen) && match(TokenType::Arrow)) {
            // It's an arrow function!
            std::variant<std::unique_ptr<BlockStmt>, ExprPtr> body;
            if (check(TokenType::LeftBrace)) {
                advance(); // consume {
                body = std::unique_ptr<BlockStmt>(
                    static_cast<BlockStmt*>(parseBlockStatement().release())
                );
            } else {
                body = parseAssignmentExpression();
            }
            
            return std::make_unique<ArrowFunctionExpr>(
                std::move(params), std::move(body)
            );
        }
        
        // Not arrow function, restore lexer state completely
        lexer_.restore(savedLexerState);
        currentToken_ = savedToken;
    }
    
    // Normal assignment expression
    ExprPtr left = parseConditionalExpression();
    
    if (match({TokenType::Assign, TokenType::PlusAssign, TokenType::MinusAssign,
               TokenType::StarAssign, TokenType::SlashAssign, TokenType::PercentAssign,
               TokenType::StarStarAssign,
               TokenType::AmpersandAssign, TokenType::PipeAssign, TokenType::CaretAssign,
               TokenType::LeftShiftAssign, TokenType::RightShiftAssign,
               TokenType::UnsignedRightShiftAssign,
               TokenType::AndAssign, TokenType::OrAssign,
               TokenType::QuestionQuestionAssign})) {
        TokenType op = previousToken_.type;
        ExprPtr right = parseAssignmentExpression();
        return std::make_unique<AssignmentExpr>(op, std::move(left), std::move(right));
    }
    
    return left;
}

ExprPtr Parser::parseConditionalExpression() {
    ExprPtr condition = parseLogicalOrExpression();
    
    if (match(TokenType::Question)) {
        ExprPtr consequent = parseAssignmentExpression();
        consume(TokenType::Colon, "Expected ':' in conditional expression");
        ExprPtr alternate = parseAssignmentExpression();
        return std::make_unique<ConditionalExpr>(std::move(condition), 
                                                  std::move(consequent), 
                                                  std::move(alternate));
    }
    
    return condition;
}

ExprPtr Parser::parseLogicalOrExpression() {
    ExprPtr left = parseLogicalAndExpression();
    
    while (match({TokenType::Or, TokenType::QuestionQuestion})) {
        TokenType op = previousToken_.type;
        ExprPtr right = parseLogicalAndExpression();
        left = std::make_unique<LogicalExpr>(op, std::move(left), std::move(right));
    }
    
    return left;
}

ExprPtr Parser::parseLogicalAndExpression() {
    ExprPtr left = parseBitwiseOrExpression();
    
    while (match(TokenType::And)) {
        ExprPtr right = parseBitwiseOrExpression();
        left = std::make_unique<LogicalExpr>(TokenType::And, std::move(left), std::move(right));
    }
    
    return left;
}

ExprPtr Parser::parseBitwiseOrExpression() {
    ExprPtr left = parseBitwiseXorExpression();
    
    while (match(TokenType::Pipe)) {
        ExprPtr right = parseBitwiseXorExpression();
        left = std::make_unique<BinaryExpr>(TokenType::Pipe, std::move(left), std::move(right));
    }
    
    return left;
}

ExprPtr Parser::parseBitwiseXorExpression() {
    ExprPtr left = parseBitwiseAndExpression();
    
    while (match(TokenType::Caret)) {
        ExprPtr right = parseBitwiseAndExpression();
        left = std::make_unique<BinaryExpr>(TokenType::Caret, std::move(left), std::move(right));
    }
    
    return left;
}

ExprPtr Parser::parseBitwiseAndExpression() {
    ExprPtr left = parseEqualityExpression();
    
    while (match(TokenType::Ampersand)) {
        ExprPtr right = parseEqualityExpression();
        left = std::make_unique<BinaryExpr>(TokenType::Ampersand, std::move(left), std::move(right));
    }
    
    return left;
}

ExprPtr Parser::parseEqualityExpression() {
    ExprPtr left = parseRelationalExpression();
    
    while (match({TokenType::Equal, TokenType::NotEqual, 
                  TokenType::StrictEqual, TokenType::StrictNotEqual})) {
        TokenType op = previousToken_.type;
        ExprPtr right = parseRelationalExpression();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }
    
    return left;
}

ExprPtr Parser::parseRelationalExpression() {
    ExprPtr left = parseShiftExpression();
    
    // When noIn_ is set (for-loop init context), exclude 'in' from
    // relational operators per ES spec [In] grammar parameter.
    // This prevents for(x in obj) from consuming 'in' as binary op.
    for (;;) {
        if (match({TokenType::Less, TokenType::LessEqual,
                   TokenType::Greater, TokenType::GreaterEqual,
                   TokenType::Instanceof})) {
            TokenType op = previousToken_.type;
            ExprPtr right = parseShiftExpression();
            left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
            continue;
        }
        if (!noIn_ && match(TokenType::In)) {
            ExprPtr right = parseShiftExpression();
            left = std::make_unique<BinaryExpr>(TokenType::In, std::move(left), std::move(right));
            continue;
        }
        break;
    }
    
    return left;
}

ExprPtr Parser::parseShiftExpression() {
    ExprPtr left = parseAdditiveExpression();
    
    while (match({TokenType::LeftShift, TokenType::RightShift, TokenType::UnsignedRightShift})) {
        TokenType op = previousToken_.type;
        ExprPtr right = parseAdditiveExpression();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }
    
    return left;
}

ExprPtr Parser::parseAdditiveExpression() {
    ExprPtr left = parseMultiplicativeExpression();
    
    while (match({TokenType::Plus, TokenType::Minus})) {
        TokenType op = previousToken_.type;
        ExprPtr right = parseMultiplicativeExpression();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }
    
    return left;
}

ExprPtr Parser::parseMultiplicativeExpression() {
    ExprPtr left = parseExponentiationExpression();
    
    while (match({TokenType::Star, TokenType::Slash, TokenType::Percent})) {
        TokenType op = previousToken_.type;
        ExprPtr right = parseExponentiationExpression();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }
    
    return left;
}

ExprPtr Parser::parseExponentiationExpression() {
    ExprPtr left = parseUnaryExpression();
    
    if (match(TokenType::StarStar)) {
        ExprPtr right = parseExponentiationExpression(); // Right associative
        return std::make_unique<BinaryExpr>(TokenType::StarStar, std::move(left), std::move(right));
    }
    
    return left;
}

ExprPtr Parser::parseUnaryExpression() {
    // Handle await expression
    if (match(TokenType::Await)) {
        ExprPtr argument = parseUnaryExpression();
        return std::make_unique<AwaitExpr>(std::move(argument));
    }
    
    // Handle yield expression (in generators)
    if (match(TokenType::Yield)) {
        bool delegate = match(TokenType::Star);  // yield*
        ExprPtr argument = nullptr;
        // yield can have an optional argument
        if (!check(TokenType::Semicolon) && !check(TokenType::RightBrace) && 
            !check(TokenType::RightParen) && !check(TokenType::Comma)) {
            argument = parseAssignmentExpression();
        }
        return std::make_unique<YieldExpr>(std::move(argument), delegate);
    }
    
    if (match({TokenType::Not, TokenType::Minus, TokenType::Plus, 
               TokenType::Tilde, TokenType::Typeof, TokenType::Void, 
               TokenType::Delete})) {
        TokenType op = previousToken_.type;
        ExprPtr argument = parseUnaryExpression();
        return std::make_unique<UnaryExpr>(op, std::move(argument), true);
    }
    
    return parseUpdateExpression();
}

ExprPtr Parser::parseUpdateExpression() {
    // Prefix increment/decrement
    if (match({TokenType::PlusPlus, TokenType::MinusMinus})) {
        TokenType op = previousToken_.type;
        ExprPtr argument = parseLeftHandSideExpression();
        return std::make_unique<UpdateExpr>(op, std::move(argument), true);
    }
    
    ExprPtr expr = parseLeftHandSideExpression();
    
    // Postfix increment/decrement (no line terminator between)
    if (match({TokenType::PlusPlus, TokenType::MinusMinus})) {
        TokenType op = previousToken_.type;
        return std::make_unique<UpdateExpr>(op, std::move(expr), false);
    }
    
    return expr;
}

ExprPtr Parser::parseLeftHandSideExpression() {
    ExprPtr expr;
    
    if (match(TokenType::New)) {
        expr = parseNewExpression();
    } else {
        expr = parseMemberExpression();
    }
    
    // Call expressions
    while (true) {
        if (match(TokenType::LeftParen)) {
            expr = parseCallExpression(std::move(expr));
        } else if (match(TokenType::Dot)) {
            // Allow keywords as property names (e.g., Array.from, obj.class)
            if (!isPropertyName(currentToken_.type)) {
                error(currentToken_, "Expected property name");
            }
            Token property = advance();
            expr = std::make_unique<MemberExpr>(
                std::move(expr),
                std::make_unique<IdentifierExpr>(property.value),
                false
            );
        } else if (match(TokenType::LeftBracket)) {
            ExprPtr property = parseExpression();
            consume(TokenType::RightBracket, "Expected ']'");
            expr = std::make_unique<MemberExpr>(std::move(expr), std::move(property), true);
        } else if (match(TokenType::QuestionDot)) {
            if (match(TokenType::LeftParen)) {
                // Optional call: obj?.(args)
                std::vector<ExprPtr> args = parseArguments();
                consume(TokenType::RightParen, "Expected ')' after arguments");
                expr = std::make_unique<CallExpr>(std::move(expr), std::move(args), true);
            } else if (match(TokenType::LeftBracket)) {
                // Optional computed access: obj?.[expr]
                ExprPtr property = parseExpression();
                consume(TokenType::RightBracket, "Expected ']' after computed property");
                expr = std::make_unique<MemberExpr>(std::move(expr), std::move(property), true, true);
            } else {
                // Optional property access: obj?.prop
                if (!isPropertyName(currentToken_.type)) {
                    error(currentToken_, "Expected property name");
                }
                Token property = advance();
                expr = std::make_unique<MemberExpr>(
                    std::move(expr),
                    std::make_unique<IdentifierExpr>(property.value),
                    false, true
                );
            }
        } else if (check(TokenType::Template)) {
            // Tagged template: tag`string` or tag`${expr}`
            advance(); // consume template token
            auto templateExpr = parseTemplateLiteral();
            std::vector<ExprPtr> args;
            args.push_back(std::move(templateExpr));
            expr = std::make_unique<CallExpr>(std::move(expr), std::move(args));
        } else {
            break;
        }
    }
    
    return expr;
}

ExprPtr Parser::parseCallExpression(ExprPtr callee) {
    std::vector<ExprPtr> args = parseArguments();
    consume(TokenType::RightParen, "Expected ')' after arguments");
    return std::make_unique<CallExpr>(std::move(callee), std::move(args));
}

std::vector<ExprPtr> Parser::parseArguments() {
    std::vector<ExprPtr> args;
    
    if (!check(TokenType::RightParen)) {
        do {
            if (match(TokenType::DotDotDot)) {
                // Spread element
                ExprPtr arg = parseAssignmentExpression();
                // Wrap in spread - for simplicity just add normally
                args.push_back(std::move(arg));
            } else {
                args.push_back(parseAssignmentExpression());
            }
        } while (match(TokenType::Comma));
    }
    
    return args;
}

ExprPtr Parser::parseMemberExpression() {
    ExprPtr expr = parsePrimaryExpression();
    
    while (true) {
        if (match(TokenType::Dot)) {
            // Allow keywords as property names
            if (!isPropertyName(currentToken_.type)) {
                error(currentToken_, "Expected property name");
            }
            Token property = advance();
            expr = std::make_unique<MemberExpr>(
                std::move(expr),
                std::make_unique<IdentifierExpr>(property.value),
                false
            );
        } else if (match(TokenType::LeftBracket)) {
            ExprPtr property = parseExpression();
            consume(TokenType::RightBracket, "Expected ']'");
            expr = std::make_unique<MemberExpr>(std::move(expr), std::move(property), true);
        } else {
            break;
        }
    }
    
    return expr;
}

ExprPtr Parser::parseNewExpression() {
    ExprPtr callee = parseMemberExpression();
    
    std::vector<ExprPtr> args;
    if (match(TokenType::LeftParen)) {
        args = parseArguments();
        consume(TokenType::RightParen, "Expected ')' after arguments");
    }
    
    return std::make_unique<NewExpr>(std::move(callee), std::move(args));
}

ExprPtr Parser::parsePrimaryExpression() {
    // Literals
    if (match(TokenType::Number)) {
        return std::make_unique<LiteralExpr>(previousToken_.numericValue, previousToken_.start);
    }
    if (match(TokenType::String)) {
        return std::make_unique<LiteralExpr>(previousToken_.value, previousToken_.start);
    }
    if (match(TokenType::True)) {
        return std::make_unique<LiteralExpr>(true, previousToken_.start);
    }
    if (match(TokenType::False)) {
        return std::make_unique<LiteralExpr>(false, previousToken_.start);
    }
    if (match(TokenType::Null)) {
        return std::make_unique<LiteralExpr>(nullptr, previousToken_.start);
    }
    if (match(TokenType::Undefined)) {
        return std::make_unique<LiteralExpr>(LiteralExpr::LiteralValue{}, previousToken_.start);
    }
    
    // Regex literal: /pattern/flags → new RegExp("pattern", "flags")
    if (match(TokenType::RegExp)) {
        std::string raw = previousToken_.value;  // "pattern/flags"
        size_t sep = raw.rfind('/');
        std::string pattern = (sep != std::string::npos) ? raw.substr(0, sep) : raw;
        std::string flags = (sep != std::string::npos) ? raw.substr(sep + 1) : "";
        
        auto regexpId = std::make_unique<IdentifierExpr>("RegExp", previousToken_.start);
        std::vector<ExprPtr> args;
        args.push_back(std::make_unique<LiteralExpr>(pattern, previousToken_.start));
        if (!flags.empty()) {
            args.push_back(std::make_unique<LiteralExpr>(flags, previousToken_.start));
        }
        return std::make_unique<NewExpr>(std::move(regexpId), std::move(args));
    }
    
    // this
    if (match(TokenType::This)) {
        return std::make_unique<ThisExpr>(previousToken_.start);
    }
    
    // Identifier
    if (match(TokenType::Identifier)) {
        return std::make_unique<IdentifierExpr>(previousToken_.value, previousToken_.start);
    }
    
    // Parenthesized expression or arrow function
    if (match(TokenType::LeftParen)) {
        ExprPtr expr = parseExpression();
        consume(TokenType::RightParen, "Expected ')'");
        return expr;
    }
    
    // Array literal
    if (match(TokenType::LeftBracket)) {
        return parseArrayLiteral();
    }
    
    // Object literal
    if (match(TokenType::LeftBrace)) {
        return parseObjectLiteral();
    }
    
    // Template literal
    if (match(TokenType::Template)) {
        return parseTemplateLiteral();
    }
    
    // Function expression
    if (match(TokenType::Function)) {
        return parseFunctionExpression();
    }
    
    error("Unexpected token: " + std::string(Token::typeName(currentToken_.type)));
    return nullptr;
}

ExprPtr Parser::parseArrayLiteral() {
    std::vector<ExprPtr> elements;
    
    while (!check(TokenType::RightBracket) && !isAtEnd()) {
        if (match(TokenType::Comma)) {
            // Elision - empty slot
            elements.push_back(nullptr);
            continue;
        }
        
        if (match(TokenType::DotDotDot)) {
            // Spread element: [...arr]
            ExprPtr spreadArg = parseAssignmentExpression();
            // Mark as spread by wrapping in special node
            // For now, just add it directly - compiler will handle
            elements.push_back(std::move(spreadArg));
        } else {
            elements.push_back(parseAssignmentExpression());
        }
        
        if (!check(TokenType::RightBracket)) {
            if (!match(TokenType::Comma)) {
                break;
            }
        }
    }
    
    consume(TokenType::RightBracket, "Expected ']' after array");
    return std::make_unique<ArrayExpr>(std::move(elements));
}

ExprPtr Parser::parseObjectLiteral() {
    std::vector<ObjectProperty> properties;
    
    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        ObjectProperty prop;
        
        // Property key
        if (match(TokenType::LeftBracket)) {
            // Computed property
            prop.key = parseAssignmentExpression();
            consume(TokenType::RightBracket, "Expected ']'");
            prop.computed = true;
        } else if (match(TokenType::String)) {
            prop.key = std::make_unique<LiteralExpr>(previousToken_.value);
        } else if (match(TokenType::Number)) {
            prop.key = std::make_unique<LiteralExpr>(previousToken_.numericValue);
        } else {
            Token name = consume(TokenType::Identifier, "Expected property name");
            prop.key = std::make_unique<IdentifierExpr>(name.value);
            
            // Shorthand: { x } = { x: x }
            if (!check(TokenType::Colon) && !check(TokenType::LeftParen)) {
                prop.value = std::make_unique<IdentifierExpr>(name.value);
                prop.shorthand = true;
                properties.push_back(std::move(prop));
                
                if (!check(TokenType::RightBrace)) {
                    consume(TokenType::Comma, "Expected ',' between properties");
                }
                continue;
            }
        }
        
        // Method shorthand: { foo() {} }
        if (match(TokenType::LeftParen)) {
            std::vector<FunctionParam> params = parseParameters();
            consume(TokenType::RightParen, "Expected ')'");
            consume(TokenType::LeftBrace, "Expected '{'");
            auto body = std::unique_ptr<BlockStmt>(
                static_cast<BlockStmt*>(parseBlockStatement().release())
            );
            prop.value = std::make_unique<FunctionExpr>("", std::move(params), std::move(body));
            prop.method = true;
        } else {
            consume(TokenType::Colon, "Expected ':' after property key");
            prop.value = parseAssignmentExpression();
        }
        
        properties.push_back(std::move(prop));
        
        if (!check(TokenType::RightBrace)) {
            consume(TokenType::Comma, "Expected ',' between properties");
        }
    }
    
    consume(TokenType::RightBrace, "Expected '}' after object");
    return std::make_unique<ObjectExpr>(std::move(properties));
}

ExprPtr Parser::parseFunctionExpression() {
    std::string name;
    if (check(TokenType::Identifier)) {
        name = advance().value;
    }
    
    consume(TokenType::LeftParen, "Expected '(' after function");
    std::vector<FunctionParam> params = parseParameters();
    consume(TokenType::RightParen, "Expected ')' after parameters");
    
    consume(TokenType::LeftBrace, "Expected '{' before function body");
    auto body = std::unique_ptr<BlockStmt>(
        static_cast<BlockStmt*>(parseBlockStatement().release())
    );
    
    return std::make_unique<FunctionExpr>(std::move(name), std::move(params), std::move(body));
}

ExprPtr Parser::parseTemplateLiteral() {
    // Parse template literal: `Hello ${name}!`
    // Token value contains parts with \x01 and \x02 markers for expressions
    
    const std::string& templateStr = previousToken_.value;
    
    // Check if it's a simple template (no expressions)
    if (templateStr.find('\x01') == std::string::npos) {
        // Simple template: just a string
        return std::make_unique<LiteralExpr>(templateStr, previousToken_.start);
    }
    
    // Complex template with expressions
    // For now, convert to string concatenation
    // Full implementation would use TemplateLiteralExpr AST node
    
    std::vector<ExprPtr> parts;
    size_t pos = 0;
    
    while (pos < templateStr.size()) {
        size_t exprStart = templateStr.find('\x01', pos);
        
        if (exprStart == std::string::npos) {
            // Remaining string part
            if (pos < templateStr.size()) {
                std::string part = templateStr.substr(pos);
                parts.push_back(std::make_unique<LiteralExpr>(part));
            }
            break;
        }
        
        // String part before expression
        if (exprStart > pos) {
            std::string part = templateStr.substr(pos, exprStart - pos);
            parts.push_back(std::make_unique<LiteralExpr>(part));
        }
        
        // Expression part
        size_t exprEnd = templateStr.find('\x02', exprStart);
        if (exprEnd == std::string::npos) {
            error("Malformed template literal");
            return nullptr;
        }
        
        // Parse embedded expression
        std::string exprCode = templateStr.substr(exprStart + 1, exprEnd - exprStart - 1);
        
        // Create a temporary parser for the expression
        auto exprSource = SourceCode::fromString(exprCode, "<template expression>");
        Parser exprParser(exprSource.get());
        ExprPtr expr = exprParser.parseExpression();
        
        if (expr) {
            parts.push_back(std::move(expr));
        }
        
        pos = exprEnd + 1;
    }
    
    // Build concatenation expression: part1 + part2 + part3...
    if (parts.empty()) {
        return std::make_unique<LiteralExpr>("");
    }
    
    if (parts.size() == 1) {
        return std::move(parts[0]);
    }
    
    // Build left-to-right concatenation tree
    ExprPtr result = std::make_unique<BinaryExpr>(
        TokenType::Plus,
        std::move(parts[0]),
        std::move(parts[1])
    );
    
    for (size_t i = 2; i < parts.size(); i++) {
        result = std::make_unique<BinaryExpr>(
            TokenType::Plus,
            std::move(result),
            std::move(parts[i])
        );
    }
    
    return result;
}

ExprPtr Parser::parseArrowFunction() {
    // This is called when we've already identified it's an arrow function
    // For simplicity, this is handled in parsePrimaryExpression for now
    return nullptr;
}

ExprPtr Parser::parseClassExpression() {
    error("Class expressions not yet implemented");
    return nullptr;
}

// Free function helpers
std::unique_ptr<Program> parse(const SourceCode* source) {
    Parser parser(source);
    return parser.parseProgram();
}

std::unique_ptr<Program> parse(std::string_view code, std::string_view filename) {
    auto source = SourceCode::fromString(std::string(code), std::string(filename));
    return parse(source.get());
}

// Module import/export parsing
StmtPtr Parser::parseImportDeclaration() {
    SourceLocation loc = currentToken_.start;
    std::vector<ImportSpecifier> specifiers;
    
    // import foo from 'module' (default import)
    if (check(TokenType::Identifier)) {
        ImportSpecifier spec;
        spec.local = advance().value;
        spec.imported = "default";
        spec.isDefault = true;
        specifiers.push_back(spec);
        
        // Can have: import foo, { bar } from 'module'
        if (match(TokenType::Comma)) {
            if (!check(TokenType::LeftBrace)) {
                error("Expected '{' after ','");
            }
        }
    }
    
    // import { foo, bar as baz } from 'module'
    if (match(TokenType::LeftBrace)) {
        do {
            if (check(TokenType::RightBrace)) break;
            
            ImportSpecifier spec;
            spec.imported = consume(TokenType::Identifier, "Expected import specifier").value;
            
            if (match(TokenType::As)) {
                spec.local = consume(TokenType::Identifier, "Expected local name after 'as'").value;
            } else {
                spec.local = spec.imported;
            }
            specifiers.push_back(spec);
        } while (match(TokenType::Comma));
        
        consume(TokenType::RightBrace, "Expected '}' after import specifiers");
    }
    
    consume(TokenType::From, "Expected 'from' after import specifiers");
    std::string source = consume(TokenType::String, "Expected module path string").value;
    
    // Remove quotes from source
    if (source.size() >= 2) {
        source = source.substr(1, source.size() - 2);
    }
    
    // Optional semicolon
    match(TokenType::Semicolon);
    
    return std::make_unique<ImportDecl>(std::move(specifiers), std::move(source), loc);
}

StmtPtr Parser::parseExportDeclaration() {
    SourceLocation loc = currentToken_.start;
    
    // export default ...
    if (match(TokenType::Default)) {
        if (match(TokenType::Function)) {
            auto decl = parseFunctionDeclaration();
            return std::make_unique<ExportDecl>(std::move(decl), true, loc);
        }
        // export default expression
        auto expr = parseExpression();
        match(TokenType::Semicolon);
        auto exprStmt = std::make_unique<ExprStmt>(std::move(expr));
        return std::make_unique<ExportDecl>(std::move(exprStmt), true, loc);
    }
    
    // export function foo() {}
    if (match(TokenType::Function)) {
        auto decl = parseFunctionDeclaration();
        return std::make_unique<ExportDecl>(std::move(decl), false, loc);
    }
    
    // export const/let/var ...
    if (match(TokenType::Const) || match(TokenType::Let) || match(TokenType::Var)) {
        auto decl = parseVariableDeclaration();
        return std::make_unique<ExportDecl>(std::move(decl), false, loc);
    }
    
    // export { foo, bar }
    if (match(TokenType::LeftBrace)) {
        std::vector<ExportSpecifier> specifiers;
        do {
            if (check(TokenType::RightBrace)) break;
            
            ExportSpecifier spec;
            spec.local = consume(TokenType::Identifier, "Expected export specifier").value;
            
            if (match(TokenType::As)) {
                spec.exported = consume(TokenType::Identifier, "Expected exported name after 'as'").value;
            } else {
                spec.exported = spec.local;
            }
            specifiers.push_back(spec);
        } while (match(TokenType::Comma));
        
        consume(TokenType::RightBrace, "Expected '}' after export specifiers");
        match(TokenType::Semicolon);
        
        return std::make_unique<ExportDecl>(std::move(specifiers), loc);
    }
    
    error("Expected export declaration");
    return nullptr;
}

} // namespace Zepra::Frontend

// =============================================================================
// Destructuring pattern parsing
// =============================================================================

namespace Zepra::Frontend {

ExprPtr Parser::parseBindingPattern() {
    if (match(TokenType::LeftBrace)) {
        return parseObjectPattern();
    }
    if (match(TokenType::LeftBracket)) {
        return parseArrayPattern();
    }
    // Single identifier (fallback)
    Token name = consume(TokenType::Identifier, "Expected identifier or pattern");
    return std::make_unique<IdentifierExpr>(name.value, name.start);
}

ExprPtr Parser::parseObjectPattern() {
    // Already consumed '{'
    std::vector<PatternProperty> properties;
    ExprPtr rest = nullptr;

    while (!check(TokenType::RightBrace) && !isAtEnd()) {
        // Rest element: {...rest}
        if (match(TokenType::DotDotDot)) {
            Token name = consume(TokenType::Identifier, "Expected identifier after '...'");
            rest = std::make_unique<IdentifierExpr>(name.value, name.start);
            break;
        }

        PatternProperty prop;

        // Computed key: {[expr]: value}
        if (match(TokenType::LeftBracket)) {
            prop.key = parseAssignmentExpression();
            consume(TokenType::RightBracket, "Expected ']'");
            prop.computed = true;
            consume(TokenType::Colon, "Expected ':' after computed key");
            // Value: identifier, nested pattern, or pattern with default
            if (check(TokenType::LeftBrace) || check(TokenType::LeftBracket)) {
                prop.value = parseBindingPattern();
            } else {
                Token valName = consume(TokenType::Identifier, "Expected identifier");
                prop.value = std::make_unique<IdentifierExpr>(valName.value, valName.start);
            }
            if (match(TokenType::Assign)) {
                ExprPtr def = parseAssignmentExpression();
                prop.value = std::make_unique<AssignmentPatternExpr>(std::move(prop.value), std::move(def));
            }
        } else {
            // Regular key: identifier or string
            Token keyToken = consume(TokenType::Identifier, "Expected property name");
            prop.key = std::make_unique<IdentifierExpr>(keyToken.value, keyToken.start);

            if (match(TokenType::Colon)) {
                // {key: target}
                prop.shorthand = false;
                if (check(TokenType::LeftBrace) || check(TokenType::LeftBracket)) {
                    prop.value = parseBindingPattern();
                } else {
                    Token valName = consume(TokenType::Identifier, "Expected identifier");
                    prop.value = std::make_unique<IdentifierExpr>(valName.value, valName.start);
                }
                if (match(TokenType::Assign)) {
                    ExprPtr def = parseAssignmentExpression();
                    prop.value = std::make_unique<AssignmentPatternExpr>(std::move(prop.value), std::move(def));
                }
            } else {
                // Shorthand: {key} or {key = default}
                prop.shorthand = true;
                prop.value = std::make_unique<IdentifierExpr>(keyToken.value, keyToken.start);
                if (match(TokenType::Assign)) {
                    ExprPtr def = parseAssignmentExpression();
                    prop.value = std::make_unique<AssignmentPatternExpr>(std::move(prop.value), std::move(def));
                }
            }
        }

        properties.push_back(std::move(prop));

        if (!check(TokenType::RightBrace)) {
            if (!match(TokenType::Comma)) break;
        }
    }

    consume(TokenType::RightBrace, "Expected '}' after object pattern");
    return std::make_unique<ObjectPatternExpr>(std::move(properties), std::move(rest));
}

ExprPtr Parser::parseArrayPattern() {
    // Already consumed '['
    std::vector<ExprPtr> elements;
    ExprPtr rest = nullptr;

    while (!check(TokenType::RightBracket) && !isAtEnd()) {
        // Elision: [, , x]
        if (match(TokenType::Comma)) {
            elements.push_back(nullptr);
            continue;
        }

        // Rest element: [...rest]
        if (match(TokenType::DotDotDot)) {
            if (check(TokenType::LeftBrace) || check(TokenType::LeftBracket)) {
                rest = parseBindingPattern();
            } else {
                Token name = consume(TokenType::Identifier, "Expected identifier after '...'");
                rest = std::make_unique<IdentifierExpr>(name.value, name.start);
            }
            break;
        }

        // Element: identifier, nested pattern, or pattern with default
        ExprPtr elem;
        if (check(TokenType::LeftBrace) || check(TokenType::LeftBracket)) {
            elem = parseBindingPattern();
        } else {
            Token name = consume(TokenType::Identifier, "Expected identifier or pattern");
            elem = std::make_unique<IdentifierExpr>(name.value, name.start);
        }

        if (match(TokenType::Assign)) {
            ExprPtr def = parseAssignmentExpression();
            elem = std::make_unique<AssignmentPatternExpr>(std::move(elem), std::move(def));
        }

        elements.push_back(std::move(elem));

        if (!check(TokenType::RightBracket)) {
            if (!match(TokenType::Comma)) break;
        }
    }

    consume(TokenType::RightBracket, "Expected ']' after array pattern");
    return std::make_unique<ArrayPatternExpr>(std::move(elements), std::move(rest));
}

} // namespace Zepra::Frontend
