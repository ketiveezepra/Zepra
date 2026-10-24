#pragma once

/**
 * @file ast.hpp
 * @brief Abstract Syntax Tree node definitions
 */

#include "../config.hpp"
#include <algorithm>
#include "token.hpp"
#include <memory>
#include <vector>
#include <string>
#include <variant>
#include <optional>

namespace Zepra::Frontend {

// Forward declarations
class ASTNode;
class Expression;
class Statement;
class Declaration;

// Smart pointer types
using ASTNodePtr = std::unique_ptr<ASTNode>;
using ExprPtr = std::unique_ptr<Expression>;
using StmtPtr = std::unique_ptr<Statement>;
using DeclPtr = std::unique_ptr<Declaration>;

/**
 * @brief AST node types
 */
enum class NodeType : uint8 {
    // Program
    Program,
    
    // Expressions
    Literal,
    Identifier,
    ArrayExpression,
    ObjectExpression,
    FunctionExpression,
    ArrowFunction,
    MemberExpression,
    CallExpression,
    NewExpression,
    UnaryExpression,
    BinaryExpression,
    LogicalExpression,
    ConditionalExpression,
    AssignmentExpression,
    SequenceExpression,
    UpdateExpression,
    ThisExpression,
    SpreadElement,
    TemplateLiteral,
    TaggedTemplateExpression,
    ClassExpression,
    AwaitExpression,
    YieldExpression,
    SuperExpression,

    // Statements
    BlockStatement,
    ExpressionStatement,
    EmptyStatement,
    ReturnStatement,
    BreakStatement,
    ContinueStatement,
    IfStatement,
    SwitchStatement,
    WhileStatement,
    DoWhileStatement,
    ForStatement,
    ForInStatement,
    ForOfStatement,
    ThrowStatement,
    TryStatement,
    DebuggerStatement,
    LabeledStatement,
    WithStatement,
    
    // Declarations
    VariableDeclaration,
    FunctionDeclaration,
    ClassDeclaration,
    ImportDeclaration,
    ExportDeclaration,
    
    // Patterns
    ObjectPattern,
    ArrayPattern,
    RestElement,
    AssignmentPattern,
    
    // Clauses
    SwitchCase,
    CatchClause,
    
    // Properties
    Property,
    MethodDefinition,
    
    // Module
    ImportSpecifier,
    ExportSpecifier,
};

/**
 * @brief Base class for all AST nodes
 */
class ASTNode {
public:
    explicit ASTNode(NodeType type, SourceLocation loc = {})
        : type_(type), location_(loc) {}
    virtual ~ASTNode() = default;
    
    NodeType type() const { return type_; }
    const SourceLocation& location() const { return location_; }
    
    template<typename T>
    T* as() { return static_cast<T*>(this); }
    
    template<typename T>
    const T* as() const { return static_cast<const T*>(this); }
    
protected:
    NodeType type_;
    SourceLocation location_;
};

// =============================================================================
// Expressions
// =============================================================================

/**
 * @brief Base class for expressions
 */
class Expression : public ASTNode {
public:
    using ASTNode::ASTNode;
};

/**
 * @brief Literal value (number, string, boolean, null)
 */
class LiteralExpr : public Expression {
public:
    using LiteralValue = std::variant<double, std::string, bool, std::nullptr_t>;
    
    LiteralExpr(LiteralValue value, SourceLocation loc = {})
        : Expression(NodeType::Literal, loc), value_(std::move(value)) {}
    
    const LiteralValue& value() const { return value_; }
    
    bool isNumber() const { return std::holds_alternative<double>(value_); }
    bool isString() const { return std::holds_alternative<std::string>(value_); }
    bool isBoolean() const { return std::holds_alternative<bool>(value_); }
    bool isNull() const { return std::holds_alternative<std::nullptr_t>(value_); }
    
    double asNumber() const { return std::get<double>(value_); }
    const std::string& asString() const { return std::get<std::string>(value_); }
    bool asBoolean() const { return std::get<bool>(value_); }
    
private:
    LiteralValue value_;
};

/**
 * @brief Identifier (variable name)
 */
class IdentifierExpr : public Expression {
public:
    IdentifierExpr(std::string name, SourceLocation loc = {})
        : Expression(NodeType::Identifier, loc), name_(std::move(name)) {}
    
    const std::string& name() const { return name_; }
    
private:
    std::string name_;
};

/**
 * @brief Array literal [a, b, c]
 */
class ArrayExpr : public Expression {
public:
    ArrayExpr(std::vector<ExprPtr> elements, SourceLocation loc = {})
        : Expression(NodeType::ArrayExpression, loc)
        , elements_(std::move(elements)) {}
    
    const std::vector<ExprPtr>& elements() const { return elements_; }
    
private:
    std::vector<ExprPtr> elements_;
};

/**
 * @brief Object property
 */
struct ObjectProperty {
    ExprPtr key;
    ExprPtr value;
    bool computed = false;
    bool shorthand = false;
    bool method = false;
    bool spread = false;  // { ...value }: key is null
    enum class Kind { Init, Get, Set } kind = Kind::Init;
};

/**
 * @brief Object literal {a: 1, b: 2}
 */
class ObjectExpr : public Expression {
public:
    ObjectExpr(std::vector<ObjectProperty> properties, SourceLocation loc = {})
        : Expression(NodeType::ObjectExpression, loc)
        , properties_(std::move(properties)) {}
    
    const std::vector<ObjectProperty>& properties() const { return properties_; }
    
private:
    std::vector<ObjectProperty> properties_;
};

/**
 * @brief Binary expression (a + b, a * b, etc.)
 */
class BinaryExpr : public Expression {
public:
    BinaryExpr(TokenType op, ExprPtr left, ExprPtr right, SourceLocation loc = {})
        : Expression(NodeType::BinaryExpression, loc)
        , operator_(op)
        , left_(std::move(left))
        , right_(std::move(right)) {}
    
    TokenType op() const { return operator_; }
    Expression* left() const { return left_.get(); }
    Expression* right() const { return right_.get(); }
    
private:
    TokenType operator_;
    ExprPtr left_;
    ExprPtr right_;
};

/**
 * @brief Unary expression (!a, -a, typeof a, etc.)
 */
class UnaryExpr : public Expression {
public:
    UnaryExpr(TokenType op, ExprPtr argument, bool prefix = true, SourceLocation loc = {})
        : Expression(NodeType::UnaryExpression, loc)
        , operator_(op)
        , argument_(std::move(argument))
        , prefix_(prefix) {}
    
    TokenType op() const { return operator_; }
    Expression* argument() const { return argument_.get(); }
    bool isPrefix() const { return prefix_; }
    
private:
    TokenType operator_;
    ExprPtr argument_;
    bool prefix_;
};

/**
 * @brief Update expression (++a, a++, --a, a--)
 */
class UpdateExpr : public Expression {
public:
    UpdateExpr(TokenType op, ExprPtr argument, bool prefix, SourceLocation loc = {})
        : Expression(NodeType::UpdateExpression, loc)
        , operator_(op)
        , argument_(std::move(argument))
        , prefix_(prefix) {}
    
    TokenType op() const { return operator_; }
    Expression* argument() const { return argument_.get(); }
    bool isPrefix() const { return prefix_; }
    
private:
    TokenType operator_;
    ExprPtr argument_;
    bool prefix_;
};

/**
 * @brief Logical expression (a && b, a || b, a ?? b)
 */
class LogicalExpr : public Expression {
public:
    LogicalExpr(TokenType op, ExprPtr left, ExprPtr right, SourceLocation loc = {})
        : Expression(NodeType::LogicalExpression, loc)
        , operator_(op)
        , left_(std::move(left))
        , right_(std::move(right)) {}
    
    TokenType op() const { return operator_; }
    Expression* left() const { return left_.get(); }
    Expression* right() const { return right_.get(); }
    
private:
    TokenType operator_;
    ExprPtr left_;
    ExprPtr right_;
};

/**
 * @brief Conditional/ternary expression (a ? b : c)
 */
class ConditionalExpr : public Expression {
public:
    ConditionalExpr(ExprPtr test, ExprPtr consequent, ExprPtr alternate, 
                    SourceLocation loc = {})
        : Expression(NodeType::ConditionalExpression, loc)
        , test_(std::move(test))
        , consequent_(std::move(consequent))
        , alternate_(std::move(alternate)) {}
    
    Expression* test() const { return test_.get(); }
    Expression* consequent() const { return consequent_.get(); }
    Expression* alternate() const { return alternate_.get(); }
    
private:
    ExprPtr test_;
    ExprPtr consequent_;
    ExprPtr alternate_;
};

/**
 * @brief Assignment expression (a = b, a += b, etc.)
 */
class AssignmentExpr : public Expression {
public:
    AssignmentExpr(TokenType op, ExprPtr left, ExprPtr right, SourceLocation loc = {})
        : Expression(NodeType::AssignmentExpression, loc)
        , operator_(op)
        , left_(std::move(left))
        , right_(std::move(right)) {}
    
    TokenType op() const { return operator_; }
    Expression* left() const { return left_.get(); }
    Expression* right() const { return right_.get(); }
    
private:
    TokenType operator_;
    ExprPtr left_;
    ExprPtr right_;
};

/**
 * @brief Member expression (a.b, a[b])
 */
class MemberExpr : public Expression {
public:
    MemberExpr(ExprPtr object, ExprPtr property, bool computed, 
               bool optional = false, SourceLocation loc = {})
        : Expression(NodeType::MemberExpression, loc)
        , object_(std::move(object))
        , property_(std::move(property))
        , computed_(computed)
        , optional_(optional) {}
    
    Expression* object() const { return object_.get(); }
    Expression* property() const { return property_.get(); }
    bool isComputed() const { return computed_; }
    bool isOptional() const { return optional_; }
    
private:
    ExprPtr object_;
    ExprPtr property_;
    bool computed_;
    bool optional_;
};

/**
 * @brief Function call (f(a, b))
 */
class CallExpr : public Expression {
public:
    CallExpr(ExprPtr callee, std::vector<ExprPtr> arguments, 
             bool optional = false, SourceLocation loc = {})
        : Expression(NodeType::CallExpression, loc)
        , callee_(std::move(callee))
        , arguments_(std::move(arguments))
        , optional_(optional) {}
    
    Expression* callee() const { return callee_.get(); }
    const std::vector<ExprPtr>& arguments() const { return arguments_; }
    bool isOptional() const { return optional_; }
    
private:
    ExprPtr callee_;
    std::vector<ExprPtr> arguments_;
    bool optional_;
};

/**
 * @brief New expression (new Foo(a, b))
 */
class NewExpr : public Expression {
public:
    NewExpr(ExprPtr callee, std::vector<ExprPtr> arguments, SourceLocation loc = {})
        : Expression(NodeType::NewExpression, loc)
        , callee_(std::move(callee))
        , arguments_(std::move(arguments)) {}
    
    Expression* callee() const { return callee_.get(); }
    const std::vector<ExprPtr>& arguments() const { return arguments_; }
    
private:
    ExprPtr callee_;
    std::vector<ExprPtr> arguments_;
};

/**
 * @brief This expression
 */
class ThisExpr : public Expression {
public:
    explicit ThisExpr(SourceLocation loc = {})
        : Expression(NodeType::ThisExpression, loc) {}
};

/**
 * @brief super, as the callee of super(...) or the object of super.x / super[x]
 */
class SuperExpr : public Expression {
public:
    explicit SuperExpr(SourceLocation loc = {})
        : Expression(NodeType::SuperExpression, loc) {}
};

// =============================================================================
// Statements
// =============================================================================

/**
 * @brief Base class for statements
 */
class Statement : public ASTNode {
public:
    using ASTNode::ASTNode;
};

/**
 * @brief Block statement { ... }
 */
class BlockStmt : public Statement {
public:
    BlockStmt(std::vector<StmtPtr> body, SourceLocation loc = {})
        : Statement(NodeType::BlockStatement, loc)
        , body_(std::move(body)) {}
    
    const std::vector<StmtPtr>& body() const { return body_; }
    
private:
    std::vector<StmtPtr> body_;
};

/**
 * @brief Expression statement
 */
class ExprStmt : public Statement {
public:
    ExprStmt(ExprPtr expression, SourceLocation loc = {})
        : Statement(NodeType::ExpressionStatement, loc)
        , expression_(std::move(expression)) {}
    
    Expression* expression() const { return expression_.get(); }
    
private:
    ExprPtr expression_;
};

/**
 * @brief Empty statement (;)
 */
class EmptyStmt : public Statement {
public:
    explicit EmptyStmt(SourceLocation loc = {})
        : Statement(NodeType::EmptyStatement, loc) {}
};

/**
 * @brief Labeled statement (label: statement)
 *
 * Attaches a label to a statement for break/continue targeting.
 * Example: outer: for (var i = 0; i < 10; i++) { break outer; }
 */
class LabeledStmt : public Statement {
public:
    LabeledStmt(std::string label, StmtPtr body, SourceLocation loc = {})
        : Statement(NodeType::LabeledStatement, loc)
        , label_(std::move(label))
        , body_(std::move(body)) {}

    const std::string& label() const { return label_; }
    Statement* body() const { return body_.get(); }

private:
    std::string label_;
    StmtPtr body_;
};

/**
 * @brief Return statement
 */
class ReturnStmt : public Statement {
public:
    ReturnStmt(ExprPtr argument = nullptr, SourceLocation loc = {})
        : Statement(NodeType::ReturnStatement, loc)
        , argument_(std::move(argument)) {}
    
    Expression* argument() const { return argument_.get(); }
    bool hasArgument() const { return argument_ != nullptr; }
    
private:
    ExprPtr argument_;
};

/**
 * @brief Break statement
 */
class BreakStmt : public Statement {
public:
    BreakStmt(std::string label = "", SourceLocation loc = {})
        : Statement(NodeType::BreakStatement, loc)
        , label_(std::move(label)) {}
    
    const std::string& label() const { return label_; }
    
private:
    std::string label_;
};

/**
 * @brief Continue statement
 */
class ContinueStmt : public Statement {
public:
    ContinueStmt(std::string label = "", SourceLocation loc = {})
        : Statement(NodeType::ContinueStatement, loc)
        , label_(std::move(label)) {}
    
    const std::string& label() const { return label_; }
    
private:
    std::string label_;
};

/**
 * @brief If statement
 */
class IfStmt : public Statement {
public:
    IfStmt(ExprPtr test, StmtPtr consequent, StmtPtr alternate = nullptr,
           SourceLocation loc = {})
        : Statement(NodeType::IfStatement, loc)
        , test_(std::move(test))
        , consequent_(std::move(consequent))
        , alternate_(std::move(alternate)) {}
    
    Expression* test() const { return test_.get(); }
    Statement* consequent() const { return consequent_.get(); }
    Statement* alternate() const { return alternate_.get(); }
    bool hasAlternate() const { return alternate_ != nullptr; }
    
private:
    ExprPtr test_;
    StmtPtr consequent_;
    StmtPtr alternate_;
};

/**
 * @brief While statement
 */
class WhileStmt : public Statement {
public:
    WhileStmt(ExprPtr test, StmtPtr body, SourceLocation loc = {})
        : Statement(NodeType::WhileStatement, loc)
        , test_(std::move(test))
        , body_(std::move(body)) {}
    
    Expression* test() const { return test_.get(); }
    Statement* body() const { return body_.get(); }
    
private:
    ExprPtr test_;
    StmtPtr body_;
};

/**
 * @brief Do-while statement
 */
class DoWhileStmt : public Statement {
public:
    DoWhileStmt(StmtPtr body, ExprPtr test, SourceLocation loc = {})
        : Statement(NodeType::DoWhileStatement, loc)
        , body_(std::move(body))
        , test_(std::move(test)) {}
    
    Statement* body() const { return body_.get(); }
    Expression* test() const { return test_.get(); }
    
private:
    StmtPtr body_;
    ExprPtr test_;
};

/**
 * @brief For statement
 */
class ForStmt : public Statement {
public:
    ForStmt(ASTNodePtr init, ExprPtr test, ExprPtr update, StmtPtr body,
            SourceLocation loc = {})
        : Statement(NodeType::ForStatement, loc)
        , init_(std::move(init))
        , test_(std::move(test))
        , update_(std::move(update))
        , body_(std::move(body)) {}
    
    ASTNode* init() const { return init_.get(); }
    Expression* test() const { return test_.get(); }
    Expression* update() const { return update_.get(); }
    Statement* body() const { return body_.get(); }
    
private:
    ASTNodePtr init_;
    ExprPtr test_;
    ExprPtr update_;
    StmtPtr body_;
};

/**
 * @brief For-of statement (ES6 iterator loop)
 * 
 * for (let x of iterable) { ... }
 * Uses Symbol.iterator to iterate over values
 */
class ForOfStmt : public Statement {
public:
    ForOfStmt(ASTNodePtr left, ExprPtr right, StmtPtr body,
              bool await = false, SourceLocation loc = {})
        : Statement(NodeType::ForOfStatement, loc)
        , left_(std::move(left))
        , right_(std::move(right))
        , body_(std::move(body))
        , await_(await) {}
    
    ASTNode* left() const { return left_.get(); }      // let x or x
    Expression* right() const { return right_.get(); } // iterable
    Statement* body() const { return body_.get(); }
    bool isAwait() const { return await_; }            // for await (x of asyncIterable)
    
private:
    ASTNodePtr left_;   // Variable declaration or identifier
    ExprPtr right_;     // Iterable expression
    StmtPtr body_;
    bool await_;
};

/**
 * @brief For-in statement (object key enumeration)
 * 
 * for (let key in object) { ... }
 */
class ForInStmt : public Statement {
public:
    ForInStmt(ASTNodePtr left, ExprPtr right, StmtPtr body,
              SourceLocation loc = {})
        : Statement(NodeType::ForInStatement, loc)
        , left_(std::move(left))
        , right_(std::move(right))
        , body_(std::move(body)) {}
    
    ASTNode* left() const { return left_.get(); }
    Expression* right() const { return right_.get(); }
    Statement* body() const { return body_.get(); }
    
private:
    ASTNodePtr left_;
    ExprPtr right_;
    StmtPtr body_;
};

/**
 * @brief Template literal expression: `hello ${name}!`
 * 
 * quasis: ["hello ", "!"]  (string parts between expressions)
 * expressions: [name]       (interpolated expressions)
 */
class TemplateLiteralExpr : public Expression {
public:
    TemplateLiteralExpr(std::vector<std::string> quasis,
                        std::vector<ExprPtr> expressions,
                        SourceLocation loc = {})
        : Expression(NodeType::TemplateLiteral, loc)
        , quasis_(std::move(quasis))
        , expressions_(std::move(expressions)) {}
    
    const std::vector<std::string>& quasis() const { return quasis_; }
    const std::vector<ExprPtr>& expressions() const { return expressions_; }
    
private:
    std::vector<std::string> quasis_;
    std::vector<ExprPtr> expressions_;
};

/**
 * @brief Spread element: `...iterable`
 */
class SpreadExpr : public Expression {
public:
    SpreadExpr(ExprPtr argument, SourceLocation loc = {})
        : Expression(NodeType::SpreadElement, loc)
        , argument_(std::move(argument)) {}
    
    Expression* argument() const { return argument_.get(); }
    
private:
    ExprPtr argument_;
};

/**
 * @brief Rest element: `...rest` pattern
 */
class RestElem : public Expression {
public:
    RestElem(ExprPtr argument, SourceLocation loc = {})
        : Expression(NodeType::RestElement, loc)
        , argument_(std::move(argument)) {}
    
    Expression* argument() const { return argument_.get(); }
    
private:
    ExprPtr argument_;
};

/**
 * @brief Object destructuring pattern: {a, b: c, d = 5}
 */
struct PatternProperty {
    ExprPtr key;
    ExprPtr value;     // Binding target (Identifier, nested pattern, or AssignmentPattern)
    bool computed = false;
    bool shorthand = false;
};

class ObjectPatternExpr : public Expression {
public:
    ObjectPatternExpr(std::vector<PatternProperty> properties,
                      ExprPtr rest = nullptr, SourceLocation loc = {})
        : Expression(NodeType::ObjectPattern, loc)
        , properties_(std::move(properties))
        , rest_(std::move(rest)) {}

    const std::vector<PatternProperty>& properties() const { return properties_; }
    Expression* rest() const { return rest_.get(); }

private:
    std::vector<PatternProperty> properties_;
    ExprPtr rest_;
};

/**
 * @brief Array destructuring pattern: [a, , b, ...rest]
 */
class ArrayPatternExpr : public Expression {
public:
    ArrayPatternExpr(std::vector<ExprPtr> elements,
                     ExprPtr rest = nullptr, SourceLocation loc = {})
        : Expression(NodeType::ArrayPattern, loc)
        , elements_(std::move(elements))
        , rest_(std::move(rest)) {}

    const std::vector<ExprPtr>& elements() const { return elements_; }
    Expression* rest() const { return rest_.get(); }

private:
    std::vector<ExprPtr> elements_;
    ExprPtr rest_;
};

/**
 * @brief Assignment pattern: target = defaultValue (for destructuring defaults)
 */
class AssignmentPatternExpr : public Expression {
public:
    AssignmentPatternExpr(ExprPtr left, ExprPtr right, SourceLocation loc = {})
        : Expression(NodeType::AssignmentPattern, loc)
        , left_(std::move(left)), right_(std::move(right)) {}

    Expression* left() const { return left_.get(); }
    Expression* right() const { return right_.get(); }

private:
    ExprPtr left_;
    ExprPtr right_;
};

/**
 * @brief Throw statement
 */
class ThrowStmt : public Statement {
public:
    ThrowStmt(ExprPtr argument, SourceLocation loc = {})
        : Statement(NodeType::ThrowStatement, loc)
        , argument_(std::move(argument)) {}
    
    Expression* argument() const { return argument_.get(); }
    
private:
    ExprPtr argument_;
};

/**
 * @brief Catch clause
 */
struct CatchClause {
    std::string param;
    std::unique_ptr<BlockStmt> body;
    SourceLocation location;
};

/**
 * @brief Try statement
 */
class TryStmt : public Statement {
public:
    TryStmt(std::unique_ptr<BlockStmt> block,
            std::unique_ptr<CatchClause> handler = nullptr,
            std::unique_ptr<BlockStmt> finalizer = nullptr,
            SourceLocation loc = {})
        : Statement(NodeType::TryStatement, loc)
        , block_(std::move(block))
        , handler_(std::move(handler))
        , finalizer_(std::move(finalizer)) {}
    
    BlockStmt* block() const { return block_.get(); }
    CatchClause* handler() const { return handler_.get(); }
    BlockStmt* finalizer() const { return finalizer_.get(); }
    
private:
    std::unique_ptr<BlockStmt> block_;
    std::unique_ptr<CatchClause> handler_;
    std::unique_ptr<BlockStmt> finalizer_;
};

/**
 * @brief Switch case clause (case expr: ... or default: ...)
 */
struct SwitchCase {
    ExprPtr test;                    // nullptr for default case
    std::vector<StmtPtr> consequent; // Statements in this case
    SourceLocation location;
    
    bool isDefault() const { return test == nullptr; }
};

/**
 * @brief Switch statement
 */
class SwitchStmt : public Statement {
public:
    SwitchStmt(ExprPtr discriminant, std::vector<SwitchCase> cases,
               SourceLocation loc = {})
        : Statement(NodeType::SwitchStatement, loc)
        , discriminant_(std::move(discriminant))
        , cases_(std::move(cases)) {}
    
    Expression* discriminant() const { return discriminant_.get(); }
    const std::vector<SwitchCase>& cases() const { return cases_; }
    
private:
    ExprPtr discriminant_;
    std::vector<SwitchCase> cases_;
};

// =============================================================================
// Declarations
// =============================================================================

/**
 * @brief Base class for declarations
 */
class Declaration : public Statement {
public:
    using Statement::Statement;
};

/**
 * @brief Variable declarator
 */
struct VariableDeclarator {
    ExprPtr id;   // Identifier or pattern
    ExprPtr init; // Optional initializer
};

/**
 * @brief Variable declaration (var/let/const)
 */
class VariableDecl : public Declaration {
public:
    enum class Kind { Var, Let, Const };
    
    VariableDecl(Kind kind, std::vector<VariableDeclarator> declarators,
                 SourceLocation loc = {})
        : Declaration(NodeType::VariableDeclaration, loc)
        , kind_(kind)
        , declarators_(std::move(declarators)) {}
    
    Kind kind() const { return kind_; }
    const std::vector<VariableDeclarator>& declarators() const { return declarators_; }
    
private:
    Kind kind_;
    std::vector<VariableDeclarator> declarators_;
};

/**
 * @brief Function parameter
 */
struct FunctionParam {
    ExprPtr pattern;      // Identifier or pattern
    ExprPtr defaultValue; // Optional default
    bool rest = false;    // true if ...param
};

/**
 * @brief Function declaration/expression
 */
class FunctionDecl : public Declaration {
public:
    FunctionDecl(std::string name, std::vector<FunctionParam> params,
                 std::unique_ptr<BlockStmt> body, bool async = false,
                 bool generator = false, SourceLocation loc = {})
        : Declaration(NodeType::FunctionDeclaration, loc)
        , name_(std::move(name))
        , params_(std::move(params))
        , body_(std::move(body))
        , async_(async)
        , generator_(generator) {}
    
    const std::string& name() const { return name_; }
    const std::vector<FunctionParam>& params() const { return params_; }
    BlockStmt* body() const { return body_.get(); }
    bool isAsync() const { return async_; }
    bool isGenerator() const { return generator_; }
    
private:
    std::string name_;
    std::vector<FunctionParam> params_;
    std::unique_ptr<BlockStmt> body_;
    bool async_;
    bool generator_;
};

/**
 * @brief Function expression
 */
class FunctionExpr : public Expression {
public:
    FunctionExpr(std::string name, std::vector<FunctionParam> params,
                 std::unique_ptr<BlockStmt> body, bool async = false,
                 bool generator = false, SourceLocation loc = {})
        : Expression(NodeType::FunctionExpression, loc)
        , name_(std::move(name))
        , params_(std::move(params))
        , body_(std::move(body))
        , async_(async)
        , generator_(generator) {}
    
    const std::string& name() const { return name_; }
    const std::vector<FunctionParam>& params() const { return params_; }
    BlockStmt* body() const { return body_.get(); }
    bool isAsync() const { return async_; }
    bool isGenerator() const { return generator_; }

    // A method whose body, or an arrow inside it, refers to super
    bool usesSuper() const { return usesSuper_; }
    void setUsesSuper(bool usesSuper) { usesSuper_ = usesSuper; }

private:
    std::string name_;
    std::vector<FunctionParam> params_;
    std::unique_ptr<BlockStmt> body_;
    bool async_;
    bool generator_;
    bool usesSuper_ = false;
};

/**
 * @brief Class declaration (ES6)
 */
class ClassDecl : public Declaration {
public:
    struct MethodDef {
        std::string name;      // Static key as written (empty when computed)
        ExprPtr key;           // Identifier, literal, or the computed expression
        bool computed = false;
        std::unique_ptr<FunctionExpr> function;
        bool isStatic = false;
        bool isGetter = false;
        bool isSetter = false;
    };

    /// Field: `name = value;` (value may be null), evaluated per instance or,
    /// when static, once on the class
    struct FieldDef {
        ExprPtr key;
        bool computed = false;
        ExprPtr value;
        bool isStatic = false;
    };

    ClassDecl(std::string name, ExprPtr superClass,
              std::unique_ptr<FunctionExpr> constructor,
              std::vector<MethodDef> methods,
              SourceLocation loc = {})
        : Declaration(NodeType::ClassDeclaration, loc)
        , name_(std::move(name))
        , superClass_(std::move(superClass))
        , constructor_(std::move(constructor))
        , methods_(std::move(methods)) {}

    const std::string& name() const { return name_; }
    Expression* superClass() const { return superClass_.get(); }
    FunctionExpr* constructor() const { return constructor_.get(); }
    const std::vector<MethodDef>& methods() const { return methods_; }

    // True when the class declares no constructor (one is synthesised)
    bool hasImplicitConstructor() const { return implicitConstructor_; }
    void setImplicitConstructor(bool implicit) { implicitConstructor_ = implicit; }

    const std::vector<FieldDef>& fields() const { return fields_; }
    void setFields(std::vector<FieldDef> fields, bool usesSuper) {
        fields_ = std::move(fields);
        fieldsUseSuper_ = usesSuper;
    }
    bool fieldsUseSuper() const { return fieldsUseSuper_; }

private:
    std::string name_;
    ExprPtr superClass_;
    std::unique_ptr<FunctionExpr> constructor_;
    std::vector<MethodDef> methods_;
    std::vector<FieldDef> fields_;
    bool implicitConstructor_ = false;
    bool fieldsUseSuper_ = false;
};

/**
 * @brief Class expression: `class [Name] [extends X] { ... }` used as a value
 */
class ClassExpr : public Expression {
public:
    explicit ClassExpr(std::unique_ptr<ClassDecl> decl, SourceLocation loc = {})
        : Expression(NodeType::ClassExpression, loc)
        , decl_(std::move(decl)) {}

    const ClassDecl* decl() const { return decl_.get(); }

private:
    std::unique_ptr<ClassDecl> decl_;
};

/**
 * @brief Arrow function expression
 */
class ArrowFunctionExpr : public Expression {
public:
    ArrowFunctionExpr(std::vector<FunctionParam> params,
                      std::variant<std::unique_ptr<BlockStmt>, ExprPtr> body,
                      bool async = false, SourceLocation loc = {})
        : Expression(NodeType::ArrowFunction, loc)
        , params_(std::move(params))
        , body_(std::move(body))
        , async_(async) {}
    
    const std::vector<FunctionParam>& params() const { return params_; }
    bool hasExpressionBody() const { 
        return std::holds_alternative<ExprPtr>(body_); 
    }
    BlockStmt* blockBody() const { 
        return std::get<std::unique_ptr<BlockStmt>>(body_).get(); 
    }
    Expression* expressionBody() const { 
        return std::get<ExprPtr>(body_).get(); 
    }
    bool isAsync() const { return async_; }
    
private:
    std::vector<FunctionParam> params_;
    std::variant<std::unique_ptr<BlockStmt>, ExprPtr> body_;
    bool async_;
};

// =============================================================================
// Program
// =============================================================================

/**
 * @brief Program node (root of AST)
 */
class Program : public ASTNode {
public:
    Program(std::vector<StmtPtr> body, bool isModule = false,
            SourceLocation loc = {})
        : ASTNode(NodeType::Program, loc)
        , body_(std::move(body))
        , isModule_(isModule) {}
    
    const std::vector<StmtPtr>& body() const { return body_; }
    bool isModule() const { return isModule_; }
    
private:
    std::vector<StmtPtr> body_;
    bool isModule_;
};

// =============================================================================
// Module Declarations
// =============================================================================

/**
 * @brief Import specifier (e.g., { foo as bar })
 */
struct ImportSpecifier {
    std::string imported;  // Original name in module
    std::string local;     // Local binding name
    bool isDefault = false;
};

/**
 * @brief Import declaration (e.g., import { foo } from './module.js')
 */
class ImportDecl : public Statement {
public:
    ImportDecl(std::vector<ImportSpecifier> specifiers, std::string source,
               SourceLocation loc = {})
        : Statement(NodeType::ImportDeclaration, loc)
        , specifiers_(std::move(specifiers))
        , source_(std::move(source)) {}
    
    const std::vector<ImportSpecifier>& specifiers() const { return specifiers_; }
    const std::string& source() const { return source_; }
    
private:
    std::vector<ImportSpecifier> specifiers_;
    std::string source_;
};

/**
 * @brief Export specifier (e.g., { foo as bar })
 */
struct ExportSpecifier {
    std::string local;      // Local name
    std::string exported;   // Exported name
};

/**
 * @brief Export declaration
 */
class ExportDecl : public Statement {
public:
    // Named export: export { foo, bar }
    ExportDecl(std::vector<ExportSpecifier> specifiers, SourceLocation loc = {})
        : Statement(NodeType::ExportDeclaration, loc)
        , specifiers_(std::move(specifiers))
        , isDefault_(false) {}
    
    // Export declaration: export function foo() {} or export const x = 1
    ExportDecl(StmtPtr declaration, bool isDefault = false, SourceLocation loc = {})
        : Statement(NodeType::ExportDeclaration, loc)
        , declaration_(std::move(declaration))
        , isDefault_(isDefault) {}
    
    const std::vector<ExportSpecifier>& specifiers() const { return specifiers_; }
    Statement* declaration() const { return declaration_.get(); }
    bool isDefault() const { return isDefault_; }
    bool hasDeclaration() const { return declaration_ != nullptr; }
    
private:
    std::vector<ExportSpecifier> specifiers_;
    StmtPtr declaration_;
    bool isDefault_;
};

/**
 * @brief Await expression: await expr
 */
class AwaitExpr : public Expression {
public:
    AwaitExpr(ExprPtr argument, SourceLocation loc = {})
        : Expression(NodeType::AwaitExpression, loc)
        , argument_(std::move(argument)) {}
    
    Expression* argument() const { return argument_.get(); }
    
private:
    ExprPtr argument_;
};

/**
 * @brief Yield expression: yield expr or yield* expr
 */
class YieldExpr : public Expression {
public:
    YieldExpr(ExprPtr argument, bool delegate = false, SourceLocation loc = {})
        : Expression(NodeType::YieldExpression, loc)
        , argument_(std::move(argument))
        , delegate_(delegate) {}
    
    Expression* argument() const { return argument_.get(); }
    bool delegate() const { return delegate_; }  // yield* for delegation
    
private:
    ExprPtr argument_;
    bool delegate_;
};

} // namespace Zepra::Frontend
