#pragma once

/**
 * @file parser.hpp
 * @brief JavaScript recursive descent parser
 */

#include "lexer.hpp"
#include "ast.hpp"
#include <memory>
#include <vector>
#include <stdexcept>
#include <string>
#include <functional>
#include <unordered_set>

namespace Zepra::Frontend {

/**
 * @brief JavaScript parser
 * 
 * Parses JavaScript source code into an AST using
 * recursive descent parsing.
 */
/// Thrown when a construct cannot be parsed; parseProgram catches it,
/// records the error and resynchronises at the next statement.
class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Parser {
public:
    /**
     * @brief Create a parser for the given source
     */
    explicit Parser(const SourceCode* source);
    
    /**
     * @brief Parse a complete program
     */
    std::unique_ptr<Program> parseProgram();
    
    /**
     * @brief Parse a single expression
     */
    ExprPtr parseExpression();
    
    /**
     * @brief Get all parse errors
     */
    const std::vector<std::string>& errors() const { return errors_; }
    
    /**
     * @brief Check if any errors occurred
     */
    bool hasErrors() const { return !errors_.empty(); }
    
private:
    // Token handling
    Token& current();
    const Token& peek();
    Token advance();
    bool check(TokenType type);
    bool match(TokenType type);
    bool match(std::initializer_list<TokenType> types);
    Token consume(TokenType type, const std::string& message);
    bool isAtEnd();
    
    // Synchronization for error recovery
    void synchronize();
    void requireProgress(uint32_t startOffset);  // Throws ParseError if no token was consumed
    
    // Error handling
    void error(const std::string& message);
    void error(const Token& token, const std::string& message);
    
    // Statement parsing
    StmtPtr parseStatement();
    StmtPtr parseDeclaration();
    StmtPtr parseVariableDeclaration();
    StmtPtr parseFunctionDeclaration(bool isAsync = false, bool isGenerator = false);
    StmtPtr parseClassDeclaration();
    std::unique_ptr<ClassDecl> parseClass(bool requireName);
    StmtPtr parseBlockStatement();
    StmtPtr parseIfStatement();
    StmtPtr parseWhileStatement();
    StmtPtr parseDoWhileStatement();
    StmtPtr parseForStatement();
    StmtPtr parseSwitchStatement();
    StmtPtr parseTryStatement();
    StmtPtr parseThrowStatement();
    StmtPtr parseReturnStatement();
    StmtPtr parseBreakStatement();
    StmtPtr parseContinueStatement();
    StmtPtr parseExpressionStatement();
    StmtPtr parseImportDeclaration();
    StmtPtr parseExportDeclaration();
    
    // Expression parsing (precedence climbing)
    ExprPtr parseAssignmentExpression();
    ExprPtr parseConditionalExpression();
    ExprPtr parseLogicalOrExpression();
    ExprPtr parseLogicalAndExpression();
    ExprPtr parseBitwiseOrExpression();
    ExprPtr parseBitwiseXorExpression();
    ExprPtr parseBitwiseAndExpression();
    ExprPtr parseEqualityExpression();
    ExprPtr parseRelationalExpression();
    ExprPtr parseShiftExpression();
    ExprPtr parseAdditiveExpression();
    ExprPtr parseMultiplicativeExpression();
    ExprPtr parseExponentiationExpression();
    ExprPtr parseUnaryExpression();
    ExprPtr parseUpdateExpression();
    ExprPtr parseLeftHandSideExpression();
    ExprPtr parseCallExpression(ExprPtr callee);
    ExprPtr parseMemberExpression();
    ExprPtr parseNewExpression();
    ExprPtr parsePrimaryExpression();
    
    // Literal/specific parsing
    ExprPtr parseArrayLiteral();
    ExprPtr parseObjectLiteral();
    ExprPtr parseFunctionExpression(bool isAsync = false);
    ExprPtr tryParseArrowFunction();
    ExprPtr parseClassExpression();
    ExprPtr parseTemplateLiteral();
    bool parseTemplateParts(std::vector<std::string>& cooked, std::vector<ExprPtr>& expressions);

    // Property keys and methods shared by object literals and class bodies
    ExprPtr parsePropertyKey(bool& computed);
    bool startsPropertyKey(TokenType type) const;
    std::unique_ptr<FunctionExpr> parseMethod(bool isAsync, bool isGenerator);
    std::unique_ptr<BlockStmt> parseFunctionBody(bool* superFlag);

    // Destructuring patterns
    ExprPtr parseBindingPattern();
    ExprPtr parseObjectPattern();
    ExprPtr parseArrayPattern();

    
    // Helpers
    std::vector<FunctionParam> parseParameters();
    std::vector<ExprPtr> parseArguments();
    
    Lexer lexer_;
    Token currentToken_;
    Token previousToken_;
    std::vector<std::string> errors_;
    static constexpr size_t kMaxErrors = 100;  // stop parsing hopeless input

    // Nested expressions and statements parse recursively; past this depth the
    // input is a SyntaxError instead of a stack overflow that kills the browser
    // (`x = [[[[...` or `((((...` a hundred thousand levels deep did)
public:
    static constexpr int kMaxNestingDepth = 400;
private:
    int nestingDepth_ = 0;
    struct NestingGuard {
        Parser& parser;
        explicit NestingGuard(Parser& p);
        ~NestingGuard() { --parser.nestingDepth_; }
    };
    
    // Parser state
    bool inLoop_ = false;
    bool inSwitch_ = false;
    bool inFunction_ = false;
    bool noIn_ = false;  // Suppress 'in' as binary operator (for-loop init context)
    // Set inside a method, constructor or field initializer (arrows keep the
    // enclosing one): parsing `super` marks it. Null where super is a syntax error.
    bool* superFlag_ = nullptr;
    // Token offsets where an arrow function was tried and ruled out
    std::unordered_set<uint32_t> notArrowAt_;

    // Class private names (#x). Each class body gets a scope with a unique id;
    // `#x` declared in it becomes the reserved property key
    // "@@private:<id>:x" (hidden from enumeration, unreachable from source
    // text). References are patched when the class body ends, so methods can
    // use names declared later and nested classes can use outer names.
    struct PrivateScope {
        uint32_t id;
        std::unordered_set<std::string> declared;
        std::vector<std::pair<std::string, IdentifierExpr*>> identRefs;
        std::vector<std::pair<std::string, LiteralExpr*>> literalRefs;
    };
    std::vector<PrivateScope> privateScopes_;
    static std::string privateKey(uint32_t scopeId, const std::string& name);
    ExprPtr privateNameDeclaration(const Token& token);
    ExprPtr privateNameReference(const Token& token);  // obj.#x
    ExprPtr privateNameInOperand(const Token& token);  // #x in obj
    void resolvePrivateScope();
    // True when a line terminator separates the previous and the current token
    bool newlineBeforeCurrent() const { return previousToken_.end.line < currentToken_.start.line; }

    // Labeled statement: `label: statement`
    StmtPtr parseLabeledStatement(const std::string& label);
    
#ifndef NDEBUG
    // Debug: Infinite loop detection
    size_t lastTokenOffset_ = 0;
    int noProgressCount_ = 0;
    void assertProgress();
#endif
};

/**
 * @brief Parse JavaScript source code
 * @param source The source code
 * @return Program AST or nullptr if parsing failed
 */
std::unique_ptr<Program> parse(const SourceCode* source);

/**
 * @brief Parse JavaScript source code from string
 * @param code The JavaScript code
 * @param filename Optional filename for error messages
 * @return Program AST or nullptr if parsing failed
 */
std::unique_ptr<Program> parse(std::string_view code, 
                               std::string_view filename = "<eval>");

} // namespace Zepra::Frontend
