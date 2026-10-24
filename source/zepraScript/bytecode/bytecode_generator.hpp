#pragma once

/**
 * @file bytecode_generator.hpp
 * @brief AST to bytecode compiler
 */

#include "../config.hpp"
#include "../frontend/ast.hpp"
#include "opcode.hpp"
#include "runtime/objects/value.hpp"
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>

namespace Zepra::Runtime {
class Function;
}

namespace Zepra::Bytecode {

/**
 * @brief A chunk of bytecode with constants pool
 */
class BytecodeChunk {
public:
    BytecodeChunk() = default;
    
    // Bytecode manipulation
    void write(uint8_t byte, uint32_t line = 0);
    void write(Opcode op, uint32_t line = 0);
    void writeShort(uint16_t value, uint32_t line = 0);
    
    size_t addConstant(Runtime::Value value);
    // Index of a string constant, adding it if new; `existing` is reused as
    // the value when it already holds that string
    size_t stringConstant(const std::string& text, Runtime::Value existing);
    
    // Patching for jumps
    size_t currentOffset() const { return code_.size(); }
    void patchJump(size_t offset);
    
    // Access
    const std::vector<uint8_t>& code() const { return code_; }
    const std::vector<Runtime::Value>& constants() const { return constants_; }
    const std::vector<uint32_t>& lines() const { return lines_; }
    
    uint8_t at(size_t offset) const { return code_[offset]; }
    Runtime::Value constant(size_t index) const { return constants_[index]; }
    uint32_t lineAt(size_t offset) const;
    
    // Debug
    void disassemble(const std::string& name) const;
    size_t disassembleInstruction(size_t offset) const;
    
private:
    std::vector<uint8_t> code_;
    std::vector<Runtime::Value> constants_;
    std::vector<uint32_t> lines_;

    // Constant de-duplication
    std::unordered_map<std::string, size_t> stringConstants_;
    std::unordered_map<uint64_t, size_t> numberConstants_;
    std::vector<size_t> simpleConstants_;  // true, false, null, undefined
};

/**
 * @brief Local variable info
 *
 * Every local is a slot in the function's frame, in declaration order: the
 * slot index of locals[i] is i. Compiler temporaries use names starting with
 * a space, which no identifier can match.
 */
struct Local {
    std::string name;
    int depth = 0;       // Scope depth
    bool isCaptured = false;
    bool isConst = false;
    bool reserved = false;  // Created before its declaration runs (classes, switch clauses)
};

/**
 * @brief Upvalue info (captured variable)
 */
struct Upvalue {
    uint16_t index;   // Enclosing function's local slot, or its upvalue index
    bool isLocal;
};

/**
 * @brief A loop, switch or labeled statement that break/continue can target
 */
struct JumpContext {
    std::string label;                 // Empty when unlabeled
    bool isLoop = false;               // Accepts continue
    bool takesPlainBreak = false;      // Loops and switch take an unlabeled break
    int scopeDepth = 0;                // Locals deeper than this are popped on exit
    size_t tryDepth = 0;               // Try blocks already open outside this statement
    std::vector<size_t> breakJumps;
    std::vector<size_t> continueJumps; // Forward continues, patched to the continue target
    size_t continueTarget = SIZE_MAX;  // Backward continue target when already emitted
};

/**
 * @brief Compiler state for a function (or the top-level script)
 */
struct CompilerState {
    CompilerState* enclosing = nullptr;
    BytecodeChunk chunk;
    std::vector<Local> locals;
    std::vector<Upvalue> upvalues;
    int scopeDepth = 0;
    std::string functionName;
    bool isMethod = false;
    bool isScript = false;           // Top level: var and function bindings are globals
    bool usesArguments = false;      // The body reads `arguments`
    std::vector<JumpContext> jumps;  // break/continue targets; never cross a function
    size_t tryDepth = 0;             // Try blocks open at the current point
    std::string pendingLabel;        // Label for the loop being compiled next
};

/**
 * @brief Compiles AST to bytecode
 */
class BytecodeGenerator {
public:
    BytecodeGenerator();
    
    /**
     * @brief Compile a program to bytecode
     */
    std::unique_ptr<BytecodeChunk> compile(const Frontend::Program* program);
    
    /**
     * @brief Get compilation errors
     */
    const std::vector<std::string>& errors() const { return errors_; }
    bool hasErrors() const { return !errors_.empty(); }
    
private:
    // Statement compilation
    void compileStatement(const Frontend::Statement* stmt);
    void compileBlockStatement(const Frontend::BlockStmt* stmt);
    void compileExpressionStatement(const Frontend::ExprStmt* stmt);
    void compileVariableDeclaration(const Frontend::VariableDecl* decl);
    void compileFunctionDeclaration(const Frontend::FunctionDecl* decl);
    void compileIfStatement(const Frontend::IfStmt* stmt);
    void compileWhileStatement(const Frontend::WhileStmt* stmt);
    void compileDoWhileStatement(const Frontend::DoWhileStmt* stmt);
    void compileForStatement(const Frontend::ForStmt* stmt);
    void compileReturnStatement(const Frontend::ReturnStmt* stmt);
    void compileBreakStatement(const Frontend::BreakStmt* stmt);
    void compileContinueStatement(const Frontend::ContinueStmt* stmt);
    void compileThrowStatement(const Frontend::ThrowStmt* stmt);
    void compileTryStatement(const Frontend::TryStmt* stmt);
    void compileImportDeclaration(const Frontend::ImportDecl* decl);
    void compileExportDeclaration(const Frontend::ExportDecl* decl);
    void compileClassDeclaration(const Frontend::ClassDecl* decl);
    void compileForOfStatement(const Frontend::ForOfStmt* stmt);
    void compileForInStatement(const Frontend::ForInStmt* stmt);
    void compileTemplateLiteral(const Frontend::TemplateLiteralExpr* expr);
    void compileSwitchStatement(const Frontend::SwitchStmt* stmt);
    void compileLabeledStatement(const Frontend::LabeledStmt* stmt);
    
    // Expression compilation
    void compileExpression(const Frontend::Expression* expr);
    void compileLiteral(const Frontend::LiteralExpr* expr);
    void compileIdentifier(const Frontend::IdentifierExpr* expr);
    void compileArrayExpression(const Frontend::ArrayExpr* expr);
    void compileObjectExpression(const Frontend::ObjectExpr* expr);
    void compileBinaryExpression(const Frontend::BinaryExpr* expr);
    void compileUnaryExpression(const Frontend::UnaryExpr* expr);
    void compileUpdateExpression(const Frontend::UpdateExpr* expr);
    void compileLogicalExpression(const Frontend::LogicalExpr* expr);
    void compileConditionalExpression(const Frontend::ConditionalExpr* expr);
    void compileAssignmentExpression(const Frontend::AssignmentExpr* expr);
    void compileMemberExpression(const Frontend::MemberExpr* expr);
    void compileCallExpression(const Frontend::CallExpr* expr);
    void compileNewExpression(const Frontend::NewExpr* expr);
    void compileFunctionExpression(const Frontend::FunctionExpr* expr);
    void compileArrowFunction(const Frontend::ArrowFunctionExpr* expr);
    void compileThisExpression(const Frontend::ThisExpr* expr);
    void compileAwaitExpression(const Frontend::AwaitExpr* expr);
    void compileYieldExpression(const Frontend::YieldExpr* expr);
    void compileSpreadElement(const Frontend::SpreadExpr* expr);
    void compileRestElement(const Frontend::RestElem* expr);

    // Functions. Every function form (declaration, expression, arrow, class
    // member) is compiled here: slot 0 holds the receiver, parameters follow
    // from slot 1, then `arguments` (non-arrow functions), then hoisted vars.
    // Emits OP_CLOSURE in the enclosing function, leaving the closure on the
    // stack.
    struct FunctionSource {
        std::string name;
        const std::vector<Frontend::FunctionParam>* params = nullptr;
        const Frontend::BlockStmt* body = nullptr;            // Statement body
        const Frontend::Expression* expressionBody = nullptr; // Concise arrow body
        bool isArrow = false;
        bool isMethod = false;
        // Named function expression: its name is bound inside it
        bool bindsOwnName = false;
        // Constructor a derived class did not declare: calls the parent
        // constructor with its own arguments
        bool implicitSuperCall = false;
        // Base class constructor of a class with instance fields: runs the
        // field initializer before the body
        bool initializesFields = false;
        // The body refers to super: the running function is kept in a hidden
        // local, " method", which arrows inside reach as an upvalue
        bool usesSuper = false;
        // Field initializer: the body defines fieldClass's instance (or,
        // with staticFields, static) fields on `this`
        const Frontend::ClassDecl* fieldClass = nullptr;
        bool staticFields = false;
    };
    void compileFunction(const FunctionSource& source, Runtime::Function* fn);
    void compileClass(const Frontend::ClassDecl* decl);  // Leaves the constructor
    void compileClassExpression(const Frontend::ClassExpr* expr);
    void emitStaticFields(const Frontend::ClassDecl* decl);
    void compileMethod(const Frontend::FunctionExpr* fnExpr, const std::string& name);
    void compileFieldInitializer(const Frontend::ClassDecl* decl, bool isStatic);
    void emitPropertyKey(const Frontend::Expression* key, bool computed);

    // super: the running method, found through " method"
    void emitActiveMethod(const Frontend::Expression* at);
    void emitSuperCall(const Frontend::CallExpr* expr);
    void emitSuperProperty(const Frontend::MemberExpr* expr);  // [this, base, key?]
    void emitFieldInitialization(bool inConstructorPrologue);
    void emitParameterPrologue(const std::vector<Frontend::FunctionParam>& params);

    // Statement lists hoist their function declarations: closures are
    // created and bound before the first statement runs.
    void compileStatementList(const std::vector<Frontend::StmtPtr>& statements);
    void hoistFunctionDeclarations(const std::vector<Frontend::StmtPtr>& statements);
    void collectVarNames(const Frontend::Statement* stmt, std::vector<std::string>& names);
    void collectPatternNames(const Frontend::Expression* pattern, std::vector<std::string>& names);

    // Bindings. The value to bind is on top of the stack.
    enum class BindingKind { Var, Let, Const, Assign };
    void emitBinding(const Frontend::Expression* target, BindingKind kind);
    void emitPattern(const Frontend::Expression* pattern, BindingKind kind);
    void emitPatternElements(const Frontend::Expression* pattern, BindingKind kind, int sourceSlot);
    void emitDefaultValue(const Frontend::Expression* defaultValue);
    void emitVarStore(const std::string& name);
    void emitLexicalBinding(const std::string& name, bool isConst);
    void emitStoreVariable(const std::string& name);   // Leaves the value on the stack
    bool bindsToLocals(BindingKind kind) const;
    int reserveLexicalSlot(const std::string& name);

    // Destructuring assignment: [a, b] = ... and ({a} = ...) parse as array and
    // object literals and are bound as patterns.
    bool isAssignmentPattern(const Frontend::Expression* expr) const;

    // Control flow
    JumpContext& pushJumpContext(bool isLoop, bool takesPlainBreak);
    void popJumpContext();
    void patchContinueJumps();
    void emitExitScopes(int targetDepth, size_t targetTryDepth);
    JumpContext* findJumpContext(const std::string& label, bool forContinue);
    void compileLoopBody(const Frontend::ASTNode* left, const Frontend::Statement* body);
    void emitArgumentArray(const std::vector<Frontend::ExprPtr>& args);

    // Helpers
    void emit(uint8_t byte);
    void emit(Opcode op);
    void emitShort(uint16_t value);
    size_t emitJump(Opcode op);
    void patchJump(size_t offset);
    void emitLoop(size_t loopStart);
    void emitArgCount(size_t count);
    void emitLocal(Opcode op, int slot);   // Local and upvalue opcodes: 16-bit slot
    void emitClosureOperands(Runtime::Function* fn, const std::vector<Upvalue>& upvalues);
    void emitNameOperand(Opcode op, const std::string& name);
    size_t nameConstant(const std::string& name);

    size_t makeConstant(Runtime::Value value);
    void emitConstant(Runtime::Value value);

    // Scope management
    void beginScope();
    void endScope();
    int resolveLocal(const std::string& name);
    int resolveUpvalue(const std::string& name);
    int resolveUpvalueIn(CompilerState* state, const std::string& name);
    int addUpvalue(CompilerState* state, uint16_t index, bool isLocal);
    void addLocal(const std::string& name, bool isConst = false);
    int addTemporary(const char* name);
    void declareVariable(const std::string& name, bool isConst = false);

    // Error handling
    void error(const std::string& message);
    void error(const Frontend::ASTNode* node, const std::string& message);

    // Current state
    BytecodeChunk* currentChunk();

    CompilerState* current_ = nullptr;
    std::vector<std::string> errors_;
    uint32_t currentLine_ = 0;
};

} // namespace Zepra::Bytecode
