// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file bytecode_generator.cpp
 * @brief AST to bytecode compiler implementation
 *
 * Frame model: every local is a stack slot of its function's frame, in
 * declaration order. Slot 0 is the receiver, parameters follow, then
 * `arguments` (non-arrow functions), then the function's hoisted `var` and
 * function bindings. Block-scoped bindings and compiler temporaries are
 * pushed where they are declared and popped when their block ends, so at
 * every statement boundary the operand stack holds exactly the locals.
 * The top-level script binds `var`, functions and top-level let/const as
 * globals; its blocks get locals like any function.
 */

#include "bytecode/bytecode_generator.hpp"
#include <algorithm>
#include <cstring>
#include <mutex>
#include "runtime/objects/object.hpp"
#include "runtime/objects/function.hpp"
#include <iostream>
#include <iomanip>

namespace Zepra::Bytecode {

// BytecodeChunk implementation
void BytecodeChunk::write(uint8_t byte, uint32_t line) {
    code_.push_back(byte);
    lines_.push_back(line);
}

void BytecodeChunk::write(Opcode op, uint32_t line) {
    write(static_cast<uint8_t>(op), line);
}

void BytecodeChunk::writeShort(uint16_t value, uint32_t line) {
    write(static_cast<uint8_t>((value >> 8) & 0xFF), line);
    write(static_cast<uint8_t>(value & 0xFF), line);
}

size_t BytecodeChunk::addConstant(Runtime::Value value) {
    // Strings and numbers are de-duplicated through indexes; numbers by bit
    // pattern so that 0 and -0 stay distinct. Functions are never shared.
    if (value.isString()) {
        return stringConstant(static_cast<Runtime::String*>(value.asObject())->value(), value);
    }
    if (value.isNumber()) {
        double d = value.asNumber();
        uint64_t bits;
        std::memcpy(&bits, &d, sizeof bits);
        auto it = numberConstants_.find(bits);
        if (it != numberConstants_.end()) return it->second;
        constants_.push_back(value);
        numberConstants_.emplace(bits, constants_.size() - 1);
        return constants_.size() - 1;
    }
    if (value.isBoolean() || value.isNull() || value.isUndefined()) {
        for (size_t index : simpleConstants_) {
            const auto& existing = constants_[index];
            if (existing.type() != value.type()) continue;
            if (!value.isBoolean() || existing.asBoolean() == value.asBoolean()) return index;
        }
        constants_.push_back(value);
        simpleConstants_.push_back(constants_.size() - 1);
        return constants_.size() - 1;
    }
    constants_.push_back(value);
    return constants_.size() - 1;
}

size_t BytecodeChunk::stringConstant(const std::string& text, Runtime::Value existing) {
    auto it = stringConstants_.find(text);
    if (it != stringConstants_.end()) return it->second;
    constants_.push_back(existing.isString() ? existing
                                             : Runtime::Value::string(new Runtime::String(text)));
    stringConstants_.emplace(text, constants_.size() - 1);
    return constants_.size() - 1;
}

void BytecodeChunk::patchJump(size_t offset) {
    size_t jump = currentOffset() - offset - 2;
    code_[offset] = (jump >> 8) & 0xFF;
    code_[offset + 1] = jump & 0xFF;
}

uint32_t BytecodeChunk::lineAt(size_t offset) const {
    if (offset < lines_.size()) return lines_[offset];
    return 0;
}

void BytecodeChunk::disassemble(const std::string& name) const {
    std::cout << "== " << name << " ==\n";
    for (size_t offset = 0; offset < code_.size();) {
        offset = disassembleInstruction(offset);
    }
}

size_t BytecodeChunk::disassembleInstruction(size_t offset) const {
    std::cout << std::setw(4) << std::setfill('0') << offset << " ";
    if (offset > 0 && lineAt(offset) == lineAt(offset - 1)) {
        std::cout << "   | ";
    } else {
        std::cout << std::setw(4) << lineAt(offset) << " ";
    }

    Opcode op = static_cast<Opcode>(code_[offset]);
    std::cout << opcodeName(op);
    if (op == Opcode::OP_CLOSURE && offset + 5 < code_.size()) {
        size_t upvalues = (code_[offset + 3] << 8) | code_[offset + 4];
        std::cout << " fn=" << ((code_[offset + 1] << 8) | code_[offset + 2])
                  << " upvalues=" << upvalues << "\n";
        return offset + 5 + upvalues * 3;
    }
    int operandBytes = opcodeOperandCount(op);
    for (int i = 1; i <= operandBytes && offset + i < code_.size(); i++) {
        std::cout << " " << static_cast<int>(code_[offset + i]);
    }
    std::cout << "\n";
    return offset + 1 + operandBytes;
}

namespace {

// Compiled function bodies live as long as the process: closures made from
// them can outlive the script that defined them (timers, event handlers),
// and nothing yet tracks when the last closure of a body is collected.
void retainChunk(std::unique_ptr<BytecodeChunk> chunk) {
    static std::mutex mutex;
    static std::vector<std::unique_ptr<BytecodeChunk>> chunks;
    std::lock_guard<std::mutex> lock(mutex);
    chunks.push_back(std::move(chunk));
}

// Parameters before the first one with a default or rest: the function's
// `length`.
size_t expectedArgumentCount(const std::vector<Frontend::FunctionParam>& params) {
    size_t count = 0;
    for (const auto& param : params) {
        if (param.rest || param.defaultValue) break;
        ++count;
    }
    return count;
}

bool isLoopStatement(const Frontend::Statement* stmt) {
    switch (stmt->type()) {
        case Frontend::NodeType::WhileStatement:
        case Frontend::NodeType::DoWhileStatement:
        case Frontend::NodeType::ForStatement:
        case Frontend::NodeType::ForInStatement:
        case Frontend::NodeType::ForOfStatement:
            return true;
        default:
            return false;
    }
}

const std::string* identifierName(const Frontend::Expression* expr) {
    if (!expr || expr->type() != Frontend::NodeType::Identifier) return nullptr;
    return &static_cast<const Frontend::IdentifierExpr*>(expr)->name();
}

// Name of a non-computed property key: identifier, string or number literal.
bool staticPropertyKey(const Frontend::Expression* key, std::string& out) {
    if (const std::string* name = identifierName(key)) {
        out = *name;
        return true;
    }
    if (key && key->type() == Frontend::NodeType::Literal) {
        const auto& value = static_cast<const Frontend::LiteralExpr*>(key)->value();
        if (std::holds_alternative<std::string>(value)) {
            out = std::get<std::string>(value);
            return true;
        }
        if (std::holds_alternative<double>(value)) {
            out = Runtime::Value::number(std::get<double>(value)).toString();
            return true;
        }
    }
    return false;
}

bool hasSpread(const std::vector<Frontend::ExprPtr>& items) {
    for (const auto& item : items) {
        if (item && item->type() == Frontend::NodeType::SpreadElement) return true;
    }
    return false;
}

} // namespace

// BytecodeGenerator implementation
BytecodeGenerator::BytecodeGenerator() {
}

std::unique_ptr<BytecodeChunk> BytecodeGenerator::compile(const Frontend::Program* program) {
    CompilerState mainState;
    mainState.functionName = "<script>";
    mainState.isScript = true;
    current_ = &mainState;

    const auto& body = program->body();
    hoistFunctionDeclarations(body);

    // The value of a final expression statement is the script's result
    bool lastWasExpression = false;
    for (size_t i = 0; i < body.size(); i++) {
        const Frontend::Statement* stmt = body[i].get();
        if (stmt->type() == Frontend::NodeType::FunctionDeclaration) continue;  // Hoisted
        if (i + 1 == body.size() && stmt->type() == Frontend::NodeType::ExpressionStatement) {
            currentLine_ = stmt->location().line;
            compileExpression(static_cast<const Frontend::ExprStmt*>(stmt)->expression());
            lastWasExpression = true;
        } else {
            compileStatement(stmt);
        }
    }
    if (!lastWasExpression) {
        emitConstant(Runtime::Value::undefined());
    }
    emit(Opcode::OP_RETURN);

    current_ = nullptr;
    if (hasErrors()) {
        return nullptr;
    }
    return std::make_unique<BytecodeChunk>(std::move(mainState.chunk));
}

BytecodeChunk* BytecodeGenerator::currentChunk() {
    return current_ ? &current_->chunk : nullptr;
}

// Emit helpers
void BytecodeGenerator::emit(uint8_t byte) {
    currentChunk()->write(byte, currentLine_);
}

void BytecodeGenerator::emit(Opcode op) {
    currentChunk()->write(op, currentLine_);
}

void BytecodeGenerator::emitShort(uint16_t value) {
    currentChunk()->writeShort(value, currentLine_);
}

size_t BytecodeGenerator::emitJump(Opcode op) {
    emit(op);
    emit(static_cast<uint8_t>(0xFF));
    emit(static_cast<uint8_t>(0xFF));
    return currentChunk()->currentOffset() - 2;
}

void BytecodeGenerator::patchJump(size_t offset) {
    if (currentChunk()->currentOffset() - offset - 2 > 0xFFFF) {
        error("Too much code to jump over (more than 64 KB of bytecode)");
        return;
    }
    currentChunk()->patchJump(offset);
}

void BytecodeGenerator::emitLoop(size_t loopStart) {
    emit(Opcode::OP_LOOP);
    size_t offset = currentChunk()->currentOffset() - loopStart + 2;
    if (offset > 0xFFFF) {
        error("Loop body too large (more than 64 KB of bytecode)");
    }
    emitShort(static_cast<uint16_t>(offset));
}

void BytecodeGenerator::emitArgCount(size_t count) {
    if (count > 255) {
        error("Too many arguments in one call (max 255)");
        count = 0;
    }
    emit(static_cast<uint8_t>(count));
}

void BytecodeGenerator::emitLocal(Opcode op, int slot) {
    emit(op);
    emitShort(static_cast<uint16_t>(slot));
}

size_t BytecodeGenerator::nameConstant(const std::string& name) {
    size_t index = currentChunk()->stringConstant(name, Runtime::Value::undefined());
    if (index > 0xFFFF) {
        error("Too many constants in one function (max 65536)");
        return 0;
    }
    return index;
}

void BytecodeGenerator::emitNameOperand(Opcode op, const std::string& name) {
    size_t index = nameConstant(name);
    emit(op);
    emitShort(static_cast<uint16_t>(index));
}

void BytecodeGenerator::emitClosureOperands(Runtime::Function* fn, const std::vector<Upvalue>& upvalues) {
    size_t index = makeConstant(Runtime::Value::object(fn));
    emit(Opcode::OP_CLOSURE);
    emitShort(static_cast<uint16_t>(index));
    emitShort(static_cast<uint16_t>(upvalues.size()));
    for (const auto& upvalue : upvalues) {
        emit(upvalue.isLocal ? uint8_t(1) : uint8_t(0));
        emitShort(upvalue.index);
    }
}

size_t BytecodeGenerator::makeConstant(Runtime::Value value) {
    size_t constant = currentChunk()->addConstant(value);
    if (constant > 0xFFFF) {
        error("Too many constants in one function (max 65536)");
        return 0;
    }
    return constant;
}

void BytecodeGenerator::emitConstant(Runtime::Value value) {
    size_t index = makeConstant(value);
    if (index <= 255) {
        emit(Opcode::OP_CONSTANT);
        emit(static_cast<uint8_t>(index));
    } else {
        emit(Opcode::OP_CONSTANT_LONG);
        emitShort(static_cast<uint16_t>(index));
    }
}

// Scope management
void BytecodeGenerator::beginScope() {
    current_->scopeDepth++;
}

void BytecodeGenerator::endScope() {
    current_->scopeDepth--;

    // Pop locals that are going out of scope
    while (!current_->locals.empty() &&
           current_->locals.back().depth > current_->scopeDepth) {
        if (current_->locals.back().isCaptured) {
            emit(Opcode::OP_CLOSE_UPVALUE);
        } else {
            emit(Opcode::OP_POP);
        }
        current_->locals.pop_back();
    }
}

void BytecodeGenerator::addLocal(const std::string& name, bool isConst) {
    if (current_->locals.size() >= 0xFFFF) {
        error("Too many local variables in one function (max 65535)");
    }
    Local local;
    local.name = name;
    local.depth = current_->scopeDepth;
    local.isConst = isConst;
    current_->locals.push_back(local);
}

int BytecodeGenerator::addTemporary(const char* name) {
    addLocal(std::string(" ") + name);
    return static_cast<int>(current_->locals.size()) - 1;
}

int BytecodeGenerator::resolveLocal(const std::string& name) {
    for (int i = static_cast<int>(current_->locals.size()) - 1; i >= 0; i--) {
        if (current_->locals[i].name == name) {
            if (name == "arguments") current_->usesArguments = true;
            return i;
        }
    }
    return -1;
}

int BytecodeGenerator::resolveUpvalue(const std::string& name) {
    return resolveUpvalueIn(current_, name);
}

// Finds `name` among the locals of the functions enclosing `state`, threading
// the capture through every function in between, and returns its index in
// state's upvalue list.
int BytecodeGenerator::resolveUpvalueIn(CompilerState* state, const std::string& name) {
    CompilerState* enclosing = state->enclosing;
    if (!enclosing) return -1;

    for (int i = static_cast<int>(enclosing->locals.size()) - 1; i >= 0; i--) {
        if (enclosing->locals[i].name == name) {
            enclosing->locals[i].isCaptured = true;
            if (name == "arguments") enclosing->usesArguments = true;
            return addUpvalue(state, static_cast<uint16_t>(i), true);
        }
    }

    int outer = resolveUpvalueIn(enclosing, name);
    if (outer == -1) return -1;
    return addUpvalue(state, static_cast<uint16_t>(outer), false);
}

int BytecodeGenerator::addUpvalue(CompilerState* state, uint16_t index, bool isLocal) {
    for (size_t i = 0; i < state->upvalues.size(); i++) {
        if (state->upvalues[i].index == index && state->upvalues[i].isLocal == isLocal) {
            return static_cast<int>(i);
        }
    }
    if (state->upvalues.size() >= 0xFFFF) {
        error("Too many captured variables in one function (max 65535)");
        return 0;
    }
    state->upvalues.push_back({index, isLocal});
    return static_cast<int>(state->upvalues.size()) - 1;
}

void BytecodeGenerator::declareVariable(const std::string& name, bool isConst) {
    if (current_->scopeDepth == 0) {
        return;  // Script top level: a global
    }
    for (int i = static_cast<int>(current_->locals.size()) - 1; i >= 0; i--) {
        const Local& local = current_->locals[i];
        if (local.depth < current_->scopeDepth) break;
        if (local.name == name) {
            error("Identifier '" + name + "' has already been declared");
            break;
        }
    }
    addLocal(name, isConst);
}

int BytecodeGenerator::reserveLexicalSlot(const std::string& name) {
    if (current_->isScript && current_->scopeDepth == 0) return -1;
    int slot = resolveLocal(name);
    if (slot >= 0 && current_->locals[slot].reserved &&
        current_->locals[slot].depth == current_->scopeDepth) {
        return slot;
    }
    emitConstant(Runtime::Value::undefined());
    declareVariable(name, false);
    current_->locals.back().reserved = true;
    return static_cast<int>(current_->locals.size()) - 1;
}

// Error handling
void BytecodeGenerator::error(const std::string& message) {
    errors_.push_back(message);
}

void BytecodeGenerator::error(const Frontend::ASTNode* node, const std::string& message) {
    std::string err = message;
    if (node) {
        err = "Line " + std::to_string(node->location().line) + ": " + message;
    }
    errors_.push_back(err);
}

// Statement compilation
void BytecodeGenerator::compileStatement(const Frontend::Statement* stmt) {
    if (!stmt) return;

    currentLine_ = stmt->location().line;

    switch (stmt->type()) {
        case Frontend::NodeType::ExpressionStatement:
            compileExpressionStatement(static_cast<const Frontend::ExprStmt*>(stmt));
            break;
        case Frontend::NodeType::BlockStatement:
            compileBlockStatement(static_cast<const Frontend::BlockStmt*>(stmt));
            break;
        case Frontend::NodeType::VariableDeclaration:
            compileVariableDeclaration(static_cast<const Frontend::VariableDecl*>(stmt));
            break;
        case Frontend::NodeType::FunctionDeclaration:
            compileFunctionDeclaration(static_cast<const Frontend::FunctionDecl*>(stmt));
            break;
        case Frontend::NodeType::IfStatement:
            compileIfStatement(static_cast<const Frontend::IfStmt*>(stmt));
            break;
        case Frontend::NodeType::WhileStatement:
            compileWhileStatement(static_cast<const Frontend::WhileStmt*>(stmt));
            break;
        case Frontend::NodeType::DoWhileStatement:
            compileDoWhileStatement(static_cast<const Frontend::DoWhileStmt*>(stmt));
            break;
        case Frontend::NodeType::ForStatement:
            compileForStatement(static_cast<const Frontend::ForStmt*>(stmt));
            break;
        case Frontend::NodeType::ReturnStatement:
            compileReturnStatement(static_cast<const Frontend::ReturnStmt*>(stmt));
            break;
        case Frontend::NodeType::BreakStatement:
            compileBreakStatement(static_cast<const Frontend::BreakStmt*>(stmt));
            break;
        case Frontend::NodeType::ContinueStatement:
            compileContinueStatement(static_cast<const Frontend::ContinueStmt*>(stmt));
            break;
        case Frontend::NodeType::ThrowStatement:
            compileThrowStatement(static_cast<const Frontend::ThrowStmt*>(stmt));
            break;
        case Frontend::NodeType::TryStatement:
            compileTryStatement(static_cast<const Frontend::TryStmt*>(stmt));
            break;
        case Frontend::NodeType::ImportDeclaration:
            compileImportDeclaration(static_cast<const Frontend::ImportDecl*>(stmt));
            break;
        case Frontend::NodeType::ExportDeclaration:
            compileExportDeclaration(static_cast<const Frontend::ExportDecl*>(stmt));
            break;
        case Frontend::NodeType::ClassDeclaration:
            compileClassDeclaration(static_cast<const Frontend::ClassDecl*>(stmt));
            break;
        case Frontend::NodeType::ForOfStatement:
            compileForOfStatement(static_cast<const Frontend::ForOfStmt*>(stmt));
            break;
        case Frontend::NodeType::SwitchStatement:
            compileSwitchStatement(static_cast<const Frontend::SwitchStmt*>(stmt));
            break;
        case Frontend::NodeType::ForInStatement:
            compileForInStatement(static_cast<const Frontend::ForInStmt*>(stmt));
            break;
        case Frontend::NodeType::LabeledStatement:
            compileLabeledStatement(static_cast<const Frontend::LabeledStmt*>(stmt));
            break;
        case Frontend::NodeType::EmptyStatement:
            break;
        case Frontend::NodeType::DebuggerStatement:
            break;
        default:
            error(stmt, "Unsupported statement");
            break;
    }
}

void BytecodeGenerator::compileStatementList(const std::vector<Frontend::StmtPtr>& statements) {
    hoistFunctionDeclarations(statements);
    for (const auto& stmt : statements) {
        if (stmt->type() == Frontend::NodeType::FunctionDeclaration) continue;  // Hoisted
        compileStatement(stmt.get());
    }
}

void BytecodeGenerator::hoistFunctionDeclarations(const std::vector<Frontend::StmtPtr>& statements) {
    for (const auto& stmt : statements) {
        if (stmt->type() == Frontend::NodeType::FunctionDeclaration) {
            compileFunctionDeclaration(static_cast<const Frontend::FunctionDecl*>(stmt.get()));
        }
    }
}

// Names a statement declares with `var`, not looking into nested functions.
void BytecodeGenerator::collectVarNames(const Frontend::Statement* stmt, std::vector<std::string>& names) {
    if (!stmt) return;
    switch (stmt->type()) {
        case Frontend::NodeType::VariableDeclaration: {
            const auto* decl = static_cast<const Frontend::VariableDecl*>(stmt);
            if (decl->kind() == Frontend::VariableDecl::Kind::Var) {
                for (const auto& declarator : decl->declarators()) {
                    collectPatternNames(declarator.id.get(), names);
                }
            }
            break;
        }
        case Frontend::NodeType::BlockStatement:
            for (const auto& s : static_cast<const Frontend::BlockStmt*>(stmt)->body()) {
                collectVarNames(s.get(), names);
            }
            break;
        case Frontend::NodeType::IfStatement: {
            const auto* s = static_cast<const Frontend::IfStmt*>(stmt);
            collectVarNames(s->consequent(), names);
            collectVarNames(s->alternate(), names);
            break;
        }
        case Frontend::NodeType::WhileStatement:
            collectVarNames(static_cast<const Frontend::WhileStmt*>(stmt)->body(), names);
            break;
        case Frontend::NodeType::DoWhileStatement:
            collectVarNames(static_cast<const Frontend::DoWhileStmt*>(stmt)->body(), names);
            break;
        case Frontend::NodeType::ForStatement: {
            const auto* s = static_cast<const Frontend::ForStmt*>(stmt);
            if (s->init() && s->init()->type() == Frontend::NodeType::VariableDeclaration) {
                collectVarNames(static_cast<const Frontend::Statement*>(s->init()), names);
            }
            collectVarNames(s->body(), names);
            break;
        }
        case Frontend::NodeType::ForInStatement: {
            const auto* s = static_cast<const Frontend::ForInStmt*>(stmt);
            if (s->left()->type() == Frontend::NodeType::VariableDeclaration) {
                collectVarNames(static_cast<const Frontend::Statement*>(s->left()), names);
            }
            collectVarNames(s->body(), names);
            break;
        }
        case Frontend::NodeType::ForOfStatement: {
            const auto* s = static_cast<const Frontend::ForOfStmt*>(stmt);
            if (s->left()->type() == Frontend::NodeType::VariableDeclaration) {
                collectVarNames(static_cast<const Frontend::Statement*>(s->left()), names);
            }
            collectVarNames(s->body(), names);
            break;
        }
        case Frontend::NodeType::TryStatement: {
            const auto* s = static_cast<const Frontend::TryStmt*>(stmt);
            collectVarNames(s->block(), names);
            if (s->handler()) collectVarNames(s->handler()->body.get(), names);
            collectVarNames(s->finalizer(), names);
            break;
        }
        case Frontend::NodeType::SwitchStatement:
            for (const auto& clause : static_cast<const Frontend::SwitchStmt*>(stmt)->cases()) {
                for (const auto& s : clause.consequent) collectVarNames(s.get(), names);
            }
            break;
        case Frontend::NodeType::LabeledStatement:
            collectVarNames(static_cast<const Frontend::LabeledStmt*>(stmt)->body(), names);
            break;
        default:
            break;
    }
}

void BytecodeGenerator::collectPatternNames(const Frontend::Expression* pattern, std::vector<std::string>& names) {
    if (!pattern) return;
    switch (pattern->type()) {
        case Frontend::NodeType::Identifier:
            names.push_back(static_cast<const Frontend::IdentifierExpr*>(pattern)->name());
            break;
        case Frontend::NodeType::AssignmentPattern:
            collectPatternNames(static_cast<const Frontend::AssignmentPatternExpr*>(pattern)->left(), names);
            break;
        case Frontend::NodeType::RestElement:
            collectPatternNames(static_cast<const Frontend::RestElem*>(pattern)->argument(), names);
            break;
        case Frontend::NodeType::ObjectPattern: {
            const auto* p = static_cast<const Frontend::ObjectPatternExpr*>(pattern);
            for (const auto& prop : p->properties()) collectPatternNames(prop.value.get(), names);
            collectPatternNames(p->rest(), names);
            break;
        }
        case Frontend::NodeType::ArrayPattern: {
            const auto* p = static_cast<const Frontend::ArrayPatternExpr*>(pattern);
            for (const auto& elem : p->elements()) collectPatternNames(elem.get(), names);
            collectPatternNames(p->rest(), names);
            break;
        }
        default:
            break;
    }
}

void BytecodeGenerator::compileBlockStatement(const Frontend::BlockStmt* stmt) {
    beginScope();
    compileStatementList(stmt->body());
    endScope();
}

void BytecodeGenerator::compileExpressionStatement(const Frontend::ExprStmt* stmt) {
    compileExpression(stmt->expression());
    emit(Opcode::OP_POP);
}

void BytecodeGenerator::compileVariableDeclaration(const Frontend::VariableDecl* decl) {
    BindingKind kind = BindingKind::Var;
    if (decl->kind() == Frontend::VariableDecl::Kind::Let) kind = BindingKind::Let;
    if (decl->kind() == Frontend::VariableDecl::Kind::Const) kind = BindingKind::Const;

    for (const auto& declarator : decl->declarators()) {
        const Frontend::Expression* target = declarator.id.get();
        // `var x;` only declares, and the binding exists from the start of
        // the function or script
        if (kind == BindingKind::Var && !declarator.init &&
            target->type() == Frontend::NodeType::Identifier) {
            continue;
        }
        if (declarator.init) {
            compileExpression(declarator.init.get());
        } else {
            emitConstant(Runtime::Value::undefined());
        }
        emitBinding(target, kind);
    }
}

void BytecodeGenerator::compileFunctionDeclaration(const Frontend::FunctionDecl* decl) {
    currentLine_ = decl->location().line;
    auto* fn = new Runtime::Function(decl->name(), expectedArgumentCount(decl->params()),
                                     false, decl->isGenerator(), decl->isAsync());
    FunctionSource source;
    source.name = decl->name();
    source.params = &decl->params();
    source.body = decl->body();
    compileFunction(source, fn);

    // A global at script level, the hoisted slot at a function's top level,
    // otherwise a binding of the enclosing block
    if (current_->isScript && current_->scopeDepth == 0) {
        emitNameOperand(Opcode::OP_DEFINE_GLOBAL, decl->name());
        return;
    }
    int slot = resolveLocal(decl->name());
    if (slot >= 0 && current_->locals[slot].depth == current_->scopeDepth) {
        emitLocal(Opcode::OP_SET_LOCAL, slot);
        emit(Opcode::OP_POP);
        return;
    }
    declareVariable(decl->name(), false);
}

void BytecodeGenerator::compileIfStatement(const Frontend::IfStmt* stmt) {
    compileExpression(stmt->test());

    size_t thenJump = emitJump(Opcode::OP_JUMP_IF_FALSE);
    emit(Opcode::OP_POP);  // Condition

    compileStatement(stmt->consequent());

    size_t elseJump = emitJump(Opcode::OP_JUMP);

    patchJump(thenJump);
    emit(Opcode::OP_POP);  // Condition

    if (stmt->alternate()) {
        compileStatement(stmt->alternate());
    }

    patchJump(elseJump);
}

// Control flow contexts
JumpContext& BytecodeGenerator::pushJumpContext(bool isLoop, bool takesPlainBreak) {
    JumpContext ctx;
    ctx.isLoop = isLoop;
    ctx.takesPlainBreak = takesPlainBreak;
    ctx.scopeDepth = current_->scopeDepth;
    ctx.tryDepth = current_->tryDepth;
    if (isLoop) {
        ctx.label = std::move(current_->pendingLabel);
        current_->pendingLabel.clear();
    }
    current_->jumps.push_back(std::move(ctx));
    return current_->jumps.back();
}

void BytecodeGenerator::popJumpContext() {
    JumpContext ctx = std::move(current_->jumps.back());
    current_->jumps.pop_back();
    for (size_t jump : ctx.breakJumps) {
        patchJump(jump);
    }
}

void BytecodeGenerator::patchContinueJumps() {
    JumpContext& ctx = current_->jumps.back();
    for (size_t jump : ctx.continueJumps) {
        patchJump(jump);
    }
    ctx.continueJumps.clear();
}

// Leaving blocks and try statements early (break/continue): pop the locals
// they declared and the exception handlers they installed.
void BytecodeGenerator::emitExitScopes(int targetDepth, size_t targetTryDepth) {
    for (size_t depth = current_->tryDepth; depth > targetTryDepth; --depth) {
        emit(Opcode::OP_TRY_END);
    }
    for (int i = static_cast<int>(current_->locals.size()) - 1;
         i >= 0 && current_->locals[i].depth > targetDepth; --i) {
        emit(current_->locals[i].isCaptured ? Opcode::OP_CLOSE_UPVALUE : Opcode::OP_POP);
    }
}

JumpContext* BytecodeGenerator::findJumpContext(const std::string& label, bool forContinue) {
    for (auto it = current_->jumps.rbegin(); it != current_->jumps.rend(); ++it) {
        if (forContinue && !it->isLoop) continue;
        if (label.empty()) {
            if (forContinue || it->takesPlainBreak) return &*it;
        } else if (it->label == label) {
            return &*it;
        }
    }
    return nullptr;
}

void BytecodeGenerator::compileWhileStatement(const Frontend::WhileStmt* stmt) {
    size_t loopStart = currentChunk()->currentOffset();
    pushJumpContext(true, true).continueTarget = loopStart;

    compileExpression(stmt->test());
    size_t exitJump = emitJump(Opcode::OP_JUMP_IF_FALSE);
    emit(Opcode::OP_POP);

    compileStatement(stmt->body());
    emitLoop(loopStart);

    patchJump(exitJump);
    emit(Opcode::OP_POP);
    popJumpContext();
}

void BytecodeGenerator::compileDoWhileStatement(const Frontend::DoWhileStmt* stmt) {
    size_t bodyStart = currentChunk()->currentOffset();
    pushJumpContext(true, true);  // continue goes forward, to the test

    compileStatement(stmt->body());

    patchContinueJumps();
    compileExpression(stmt->test());
    size_t exitJump = emitJump(Opcode::OP_JUMP_IF_FALSE);
    emit(Opcode::OP_POP);
    emitLoop(bodyStart);

    patchJump(exitJump);
    emit(Opcode::OP_POP);
    popJumpContext();
}

void BytecodeGenerator::compileForStatement(const Frontend::ForStmt* stmt) {
    beginScope();  // let/const of the initializer

    if (stmt->init()) {
        if (stmt->init()->type() == Frontend::NodeType::VariableDeclaration) {
            compileVariableDeclaration(static_cast<const Frontend::VariableDecl*>(stmt->init()));
        } else {
            compileExpression(static_cast<const Frontend::Expression*>(stmt->init()));
            emit(Opcode::OP_POP);
        }
    }

    size_t loopStart = currentChunk()->currentOffset();
    size_t exitJump = SIZE_MAX;
    if (stmt->test()) {
        compileExpression(stmt->test());
        exitJump = emitJump(Opcode::OP_JUMP_IF_FALSE);
        emit(Opcode::OP_POP);
    }

    pushJumpContext(true, true);
    size_t continueTarget = loopStart;
    if (stmt->update()) {
        // The update runs after the body; the first iteration jumps over it
        size_t bodyJump = emitJump(Opcode::OP_JUMP);
        continueTarget = currentChunk()->currentOffset();
        compileExpression(stmt->update());
        emit(Opcode::OP_POP);
        emitLoop(loopStart);
        patchJump(bodyJump);
    }
    current_->jumps.back().continueTarget = continueTarget;

    compileStatement(stmt->body());
    emitLoop(continueTarget);

    if (exitJump != SIZE_MAX) {
        patchJump(exitJump);
        emit(Opcode::OP_POP);
    }
    popJumpContext();
    endScope();
}

// Binds the value on top of the stack to a for-in/for-of head, then compiles
// the body. let/const heads get a fresh binding each iteration.
void BytecodeGenerator::compileLoopBody(const Frontend::ASTNode* left, const Frontend::Statement* body) {
    if (left->type() == Frontend::NodeType::VariableDeclaration) {
        const auto* decl = static_cast<const Frontend::VariableDecl*>(left);
        if (decl->declarators().empty()) {
            error(left, "Missing loop variable");
            emit(Opcode::OP_POP);
            compileStatement(body);
            return;
        }
        const Frontend::Expression* target = decl->declarators()[0].id.get();
        if (decl->kind() == Frontend::VariableDecl::Kind::Var) {
            emitBinding(target, BindingKind::Var);
            compileStatement(body);
            return;
        }
        beginScope();
        emitBinding(target, decl->kind() == Frontend::VariableDecl::Kind::Const
                                ? BindingKind::Const : BindingKind::Let);
        compileStatement(body);
        endScope();
        return;
    }
    emitBinding(static_cast<const Frontend::Expression*>(left), BindingKind::Assign);
    compileStatement(body);
}

void BytecodeGenerator::compileForInStatement(const Frontend::ForInStmt* stmt) {
    beginScope();

    compileExpression(stmt->right());
    emit(Opcode::OP_FOR_IN);  // Array of the enumerable keys
    int keysSlot = addTemporary("keys");
    emit(Opcode::OP_ZERO);
    int indexSlot = addTemporary("index");

    size_t loopStart = currentChunk()->currentOffset();
    pushJumpContext(true, true).continueTarget = loopStart;

    // while (index < keys.length)
    emitLocal(Opcode::OP_GET_LOCAL, indexSlot);
    emitLocal(Opcode::OP_GET_LOCAL, keysSlot);
    emitNameOperand(Opcode::OP_GET_PROPERTY, "length");
    emit(Opcode::OP_LESS);
    size_t exitJump = emitJump(Opcode::OP_JUMP_IF_FALSE);
    emit(Opcode::OP_POP);

    // key = keys[index++]; advancing first lets continue jump to the test
    emitLocal(Opcode::OP_GET_LOCAL, keysSlot);
    emitLocal(Opcode::OP_GET_LOCAL, indexSlot);
    emit(Opcode::OP_GET_ELEMENT);
    emitLocal(Opcode::OP_GET_LOCAL, indexSlot);
    emit(Opcode::OP_ONE);
    emit(Opcode::OP_ADD);
    emitLocal(Opcode::OP_SET_LOCAL, indexSlot);
    emit(Opcode::OP_POP);

    compileLoopBody(stmt->left(), stmt->body());
    emitLoop(loopStart);

    patchJump(exitJump);
    emit(Opcode::OP_POP);
    popJumpContext();
    endScope();
}

void BytecodeGenerator::compileForOfStatement(const Frontend::ForOfStmt* stmt) {
    beginScope();

    compileExpression(stmt->right());
    emit(Opcode::OP_GET_ITERATOR);
    int iteratorSlot = addTemporary("iterator");

    size_t loopStart = currentChunk()->currentOffset();
    pushJumpContext(true, true).continueTarget = loopStart;

    emitLocal(Opcode::OP_GET_LOCAL, iteratorSlot);
    size_t exitJump = emitJump(Opcode::OP_ITERATOR_NEXT);  // Next value, or jump when done

    compileLoopBody(stmt->left(), stmt->body());
    emitLoop(loopStart);

    patchJump(exitJump);
    popJumpContext();
    endScope();
}

void BytecodeGenerator::compileReturnStatement(const Frontend::ReturnStmt* stmt) {
    if (stmt->argument()) {
        compileExpression(stmt->argument());
    } else {
        emitConstant(Runtime::Value::undefined());
    }
    emit(Opcode::OP_RETURN);
}

void BytecodeGenerator::compileBreakStatement(const Frontend::BreakStmt* stmt) {
    JumpContext* ctx = findJumpContext(stmt->label(), false);
    if (!ctx) {
        error(stmt, stmt->label().empty() ? "Illegal break statement"
                                          : "Undefined label '" + stmt->label() + "'");
        return;
    }
    emitExitScopes(ctx->scopeDepth, ctx->tryDepth);
    ctx->breakJumps.push_back(emitJump(Opcode::OP_JUMP));
}

void BytecodeGenerator::compileContinueStatement(const Frontend::ContinueStmt* stmt) {
    JumpContext* ctx = findJumpContext(stmt->label(), true);
    if (!ctx) {
        error(stmt, stmt->label().empty() ? "Illegal continue statement"
                                          : "Undefined label '" + stmt->label() + "'");
        return;
    }
    emitExitScopes(ctx->scopeDepth, ctx->tryDepth);
    if (ctx->continueTarget != SIZE_MAX) {
        emitLoop(ctx->continueTarget);
    } else {
        ctx->continueJumps.push_back(emitJump(Opcode::OP_JUMP));
    }
}

void BytecodeGenerator::compileLabeledStatement(const Frontend::LabeledStmt* stmt) {
    if (isLoopStatement(stmt->body())) {
        // The loop's own context takes the label, so `continue label` works
        current_->pendingLabel = stmt->label();
        compileStatement(stmt->body());
        return;
    }
    pushJumpContext(false, false).label = stmt->label();
    compileStatement(stmt->body());
    popJumpContext();
}

void BytecodeGenerator::compileThrowStatement(const Frontend::ThrowStmt* stmt) {
    compileExpression(stmt->argument());
    emit(Opcode::OP_THROW);
}

void BytecodeGenerator::compileTryStatement(const Frontend::TryStmt* stmt) {
    // TRY_BEGIN's operand is the handler offset, patched like a jump
    size_t handlerJump = emitJump(Opcode::OP_TRY_BEGIN);
    current_->tryDepth++;
    compileBlockStatement(stmt->block());
    current_->tryDepth--;
    emit(Opcode::OP_TRY_END);
    size_t skipHandler = emitJump(Opcode::OP_JUMP);

    // The VM unwinds to the stack height at TRY_BEGIN; CATCH pushes the exception
    patchJump(handlerJump);
    emit(Opcode::OP_CATCH);
    beginScope();
    if (stmt->handler()) {
        const auto* handler = stmt->handler();
        if (handler->param.empty()) {
            emit(Opcode::OP_POP);
        } else {
            declareVariable(handler->param, false);
        }
        compileStatementList(handler->body->body());
    } else {
        // try/finally: run the finally block, then rethrow
        int exceptionSlot = addTemporary("exception");
        if (stmt->finalizer()) compileBlockStatement(stmt->finalizer());
        emitLocal(Opcode::OP_GET_LOCAL, exceptionSlot);
        emit(Opcode::OP_THROW);
    }
    endScope();

    patchJump(skipHandler);
    if (stmt->finalizer()) {
        compileBlockStatement(stmt->finalizer());
    }
}

void BytecodeGenerator::compileSwitchStatement(const Frontend::SwitchStmt* stmt) {
    const auto& cases = stmt->cases();
    beginScope();

    // Control can enter the clauses at any of them, so every let/const/class
    // slot is created up front instead of where it is declared
    std::vector<std::string> lexicalNames;
    for (const auto& clause : cases) {
        for (const auto& s : clause.consequent) {
            if (s->type() == Frontend::NodeType::VariableDeclaration) {
                const auto* decl = static_cast<const Frontend::VariableDecl*>(s.get());
                if (decl->kind() == Frontend::VariableDecl::Kind::Var) continue;
                for (const auto& declarator : decl->declarators()) {
                    collectPatternNames(declarator.id.get(), lexicalNames);
                }
            } else if (s->type() == Frontend::NodeType::ClassDeclaration) {
                lexicalNames.push_back(static_cast<const Frontend::ClassDecl*>(s.get())->name());
            }
        }
    }
    for (const auto& name : lexicalNames) {
        reserveLexicalSlot(name);
    }
    for (const auto& clause : cases) {
        hoistFunctionDeclarations(clause.consequent);
    }

    // Dispatch: compare the discriminant with each case in order
    compileExpression(stmt->discriminant());
    std::vector<size_t> clauseJumps(cases.size(), SIZE_MAX);
    for (size_t i = 0; i < cases.size(); i++) {
        if (cases[i].isDefault()) continue;
        emit(Opcode::OP_DUP);
        compileExpression(cases[i].test.get());
        emit(Opcode::OP_STRICT_EQUAL);
        size_t nextCase = emitJump(Opcode::OP_JUMP_IF_FALSE);
        emit(Opcode::OP_POP);  // Comparison
        emit(Opcode::OP_POP);  // Discriminant
        clauseJumps[i] = emitJump(Opcode::OP_JUMP);
        patchJump(nextCase);
        emit(Opcode::OP_POP);  // Comparison
    }
    emit(Opcode::OP_POP);  // Discriminant: no case matched
    size_t noMatch = emitJump(Opcode::OP_JUMP);

    pushJumpContext(false, true);
    bool hasDefault = false;
    for (size_t i = 0; i < cases.size(); i++) {
        if (clauseJumps[i] != SIZE_MAX) patchJump(clauseJumps[i]);
        if (cases[i].isDefault()) {
            patchJump(noMatch);
            hasDefault = true;
        }
        for (const auto& s : cases[i].consequent) {
            if (s->type() == Frontend::NodeType::FunctionDeclaration) continue;  // Hoisted
            compileStatement(s.get());
        }
    }
    if (!hasDefault) patchJump(noMatch);
    popJumpContext();
    endScope();
}

// =============================================================================
// Functions
// =============================================================================

void BytecodeGenerator::compileFunction(const FunctionSource& source, Runtime::Function* fn) {
    CompilerState fnState;
    fnState.enclosing = current_;
    fnState.functionName = source.name.empty() ? "<anonymous>" : source.name;
    fnState.isMethod = source.isMethod;
    current_ = &fnState;

    static const std::vector<Frontend::FunctionParam> noParams;
    const auto& params = source.params ? *source.params : noParams;

    // Slot 0: the receiver. Arrow functions take `this` from their scope,
    // so their slot has a name no identifier can match.
    addLocal(source.isArrow ? " receiver" : "this", true);

    bool hasRest = false;
    std::vector<std::string> paramNames;
    for (const auto& param : params) {
        const std::string* name = identifierName(param.pattern.get());
        addLocal(name ? *name : " parameter");
        paramNames.push_back(name ? *name : std::string());
        if (param.rest) hasRest = true;
    }
    if (!source.isArrow) {
        addLocal("arguments");
    }

    // Everything below lives in the function's body scope
    fnState.scopeDepth = 1;
    emitParameterPrologue(params);

    // Hoisted bindings: var names and the body's function declarations
    std::vector<std::string> hoisted;
    if (source.body) {
        for (const auto& stmt : source.body->body()) {
            collectVarNames(stmt.get(), hoisted);
            if (stmt->type() == Frontend::NodeType::FunctionDeclaration) {
                hoisted.push_back(static_cast<const Frontend::FunctionDecl*>(stmt.get())->name());
            }
        }
    }
    for (const auto& name : hoisted) {
        if (resolveLocal(name) != -1) continue;  // Parameter or already hoisted
        emitConstant(Runtime::Value::undefined());
        addLocal(name);
    }

    // A named function expression sees its own name, unless the body declares it
    if (source.bindsOwnName && !source.name.empty() && resolveLocal(source.name) == -1) {
        emit(Opcode::OP_GET_CALLEE);
        addLocal(source.name, true);
    }

    if (source.implicitSuperClass) {
        // Derived class constructor: run the parent constructor first, with
        // this constructor's arguments
        int argumentsSlot = resolveLocal("arguments");
        emitLocal(Opcode::OP_GET_LOCAL, argumentsSlot);
        emitLocal(Opcode::OP_GET_LOCAL, 0);
        compileExpression(source.implicitSuperClass);
        emit(Opcode::OP_SUPER_APPLY);
        emit(Opcode::OP_POP);
    }

    if (source.expressionBody) {
        compileExpression(source.expressionBody);
        emit(Opcode::OP_RETURN);
    } else {
        if (source.body) {
            compileStatementList(source.body->body());
        }
        emitConstant(Runtime::Value::undefined());
        emit(Opcode::OP_RETURN);
    }

    Runtime::Function::FrameLayout layout;
    layout.parameterSlots = static_cast<uint16_t>(params.size());
    layout.hasRest = hasRest;
    layout.hasArgumentsSlot = !source.isArrow;
    layout.usesArguments = fnState.usesArguments;
    fn->setFrameLayout(layout);

    std::vector<std::string> localNames;
    localNames.reserve(fnState.locals.size());
    for (const auto& local : fnState.locals) localNames.push_back(local.name);
    fn->setDebugNames(std::move(paramNames), std::move(localNames));

    auto chunk = std::make_unique<BytecodeChunk>(std::move(fnState.chunk));
    fn->setBytecodeChunk(chunk.get());
    retainChunk(std::move(chunk));

    std::vector<Upvalue> upvalues = std::move(fnState.upvalues);
    current_ = fnState.enclosing;
    emitClosureOperands(fn, upvalues);
}

// Default values and destructured parameters, in parameter order.
void BytecodeGenerator::emitParameterPrologue(const std::vector<Frontend::FunctionParam>& params) {
    for (size_t i = 0; i < params.size(); i++) {
        const auto& param = params[i];
        const int slot = static_cast<int>(i) + 1;
        if (param.defaultValue) {
            emitLocal(Opcode::OP_GET_LOCAL, slot);
            emitDefaultValue(param.defaultValue.get());
            emitLocal(Opcode::OP_SET_LOCAL, slot);
            emit(Opcode::OP_POP);
        }
        if (!identifierName(param.pattern.get())) {
            emitLocal(Opcode::OP_GET_LOCAL, slot);
            emitBinding(param.pattern.get(), BindingKind::Let);
        }
    }
}

// =============================================================================
// Bindings and destructuring
// =============================================================================

bool BytecodeGenerator::bindsToLocals(BindingKind kind) const {
    return (kind == BindingKind::Let || kind == BindingKind::Const) &&
           !(current_->isScript && current_->scopeDepth == 0);
}

// Replaces an undefined value on top of the stack with a default.
void BytecodeGenerator::emitDefaultValue(const Frontend::Expression* defaultValue) {
    emit(Opcode::OP_DUP);
    emitConstant(Runtime::Value::undefined());
    emit(Opcode::OP_STRICT_EQUAL);
    size_t keep = emitJump(Opcode::OP_JUMP_IF_FALSE);
    emit(Opcode::OP_POP);  // Comparison
    emit(Opcode::OP_POP);  // undefined
    compileExpression(defaultValue);
    size_t done = emitJump(Opcode::OP_JUMP);
    patchJump(keep);
    emit(Opcode::OP_POP);  // Comparison
    patchJump(done);
}

// Binds the value on top of the stack. Var and Assign bindings consume it;
// Let/Const bindings in a block or function turn it into the new local.
void BytecodeGenerator::emitBinding(const Frontend::Expression* target, BindingKind kind) {
    switch (target->type()) {
        case Frontend::NodeType::Identifier: {
            const std::string& name = static_cast<const Frontend::IdentifierExpr*>(target)->name();
            switch (kind) {
                case BindingKind::Var:
                    emitVarStore(name);
                    break;
                case BindingKind::Let:
                case BindingKind::Const:
                    emitLexicalBinding(name, kind == BindingKind::Const);
                    break;
                case BindingKind::Assign:
                    emitStoreVariable(name);
                    emit(Opcode::OP_POP);
                    break;
            }
            return;
        }
        case Frontend::NodeType::AssignmentPattern: {
            const auto* pattern = static_cast<const Frontend::AssignmentPatternExpr*>(target);
            emitDefaultValue(pattern->right());
            emitBinding(pattern->left(), kind);
            return;
        }
        case Frontend::NodeType::AssignmentExpression: {
            // [a = 1] = ... parses as an array literal holding an assignment
            const auto* assign = static_cast<const Frontend::AssignmentExpr*>(target);
            if (kind == BindingKind::Assign && assign->op() == Frontend::TokenType::Assign) {
                emitDefaultValue(assign->right());
                emitBinding(assign->left(), kind);
                return;
            }
            break;
        }
        case Frontend::NodeType::MemberExpression: {
            if (kind != BindingKind::Assign) break;
            const auto* member = static_cast<const Frontend::MemberExpr*>(target);
            compileExpression(member->object());
            if (member->isComputed()) {
                compileExpression(member->property());   // [value, obj, key]
                emit(Opcode::OP_PICK);
                emit(static_cast<uint8_t>(2));           // [value, obj, key, value]
                emit(Opcode::OP_SET_ELEMENT);            // [value, value]
                emit(Opcode::OP_POP);
            } else {
                emit(Opcode::OP_SWAP);                   // [obj, value]
                emitNameOperand(Opcode::OP_SET_PROPERTY,
                    static_cast<const Frontend::IdentifierExpr*>(member->property())->name());
            }
            emit(Opcode::OP_POP);
            return;
        }
        case Frontend::NodeType::ObjectPattern:
        case Frontend::NodeType::ArrayPattern:
            emitPattern(target, kind);
            return;
        case Frontend::NodeType::ObjectExpression:
        case Frontend::NodeType::ArrayExpression:
            if (kind == BindingKind::Assign) {
                emitPattern(target, kind);
                return;
            }
            break;
        default:
            break;
    }
    error(target, "Invalid destructuring target");
    emit(Opcode::OP_POP);
}

void BytecodeGenerator::emitPattern(const Frontend::Expression* pattern, BindingKind kind) {
    if (bindsToLocals(kind)) {
        // The new bindings become locals above the source, so the source
        // stays in a hidden slot until the scope ends
        int sourceSlot = addTemporary("pattern");
        emitPatternElements(pattern, kind, sourceSlot);
        return;
    }
    emitPatternElements(pattern, kind, -1);
    emit(Opcode::OP_POP);
}

// Extracts each element of the source (in sourceSlot, or on top of the stack
// when sourceSlot is -1) and binds it. Leaves the source in place.
void BytecodeGenerator::emitPatternElements(const Frontend::Expression* pattern, BindingKind kind, int sourceSlot) {
    auto fetchSource = [&]() {
        if (sourceSlot >= 0) {
            emitLocal(Opcode::OP_GET_LOCAL, sourceSlot);
        } else {
            emit(Opcode::OP_DUP);
        }
    };

    const bool isArray = pattern->type() == Frontend::NodeType::ArrayPattern ||
                         pattern->type() == Frontend::NodeType::ArrayExpression;
    if (isArray) {
        std::vector<const Frontend::Expression*> elements;
        const Frontend::Expression* rest = nullptr;
        if (pattern->type() == Frontend::NodeType::ArrayPattern) {
            const auto* p = static_cast<const Frontend::ArrayPatternExpr*>(pattern);
            for (const auto& e : p->elements()) elements.push_back(e.get());
            rest = p->rest();
        } else {
            const auto* p = static_cast<const Frontend::ArrayExpr*>(pattern);
            for (const auto& e : p->elements()) elements.push_back(e.get());
            if (!elements.empty() && elements.back() &&
                elements.back()->type() == Frontend::NodeType::SpreadElement) {
                rest = static_cast<const Frontend::SpreadExpr*>(elements.back())->argument();
                elements.pop_back();
            }
        }
        if (rest && rest->type() == Frontend::NodeType::RestElement) {
            rest = static_cast<const Frontend::RestElem*>(rest)->argument();
        }

        // Read the source through the iteration protocol, into an array
        if (sourceSlot >= 0) {
            emit(Opcode::OP_CREATE_ARRAY);
            emitShort(0);
            emitLocal(Opcode::OP_GET_LOCAL, sourceSlot);
            emit(Opcode::OP_SPREAD);
            emitLocal(Opcode::OP_SET_LOCAL, sourceSlot);
            emit(Opcode::OP_POP);
        } else {
            emit(Opcode::OP_CREATE_ARRAY);
            emitShort(0);
            emit(Opcode::OP_SWAP);
            emit(Opcode::OP_SPREAD);
        }

        for (size_t i = 0; i < elements.size(); i++) {
            if (!elements[i]) continue;  // Hole
            if (elements[i]->type() == Frontend::NodeType::SpreadElement) {
                error(elements[i], "Rest element must be last");
                continue;
            }
            fetchSource();
            emitConstant(Runtime::Value::number(static_cast<double>(i)));
            emit(Opcode::OP_GET_ELEMENT);
            emitBinding(elements[i], kind);
        }
        if (rest) {
            fetchSource();
            emitConstant(Runtime::Value::number(static_cast<double>(elements.size())));
            emit(Opcode::OP_CALL_METHOD);
            emitShort(static_cast<uint16_t>(nameConstant("slice")));
            emitArgCount(1);
            emitBinding(rest, kind);
        }
        return;
    }

    // Object pattern
    struct Entry {
        const Frontend::Expression* key;
        bool computed;
        const Frontend::Expression* target;
    };
    std::vector<Entry> entries;
    const Frontend::Expression* rest = nullptr;
    if (pattern->type() == Frontend::NodeType::ObjectPattern) {
        const auto* p = static_cast<const Frontend::ObjectPatternExpr*>(pattern);
        for (const auto& prop : p->properties()) {
            entries.push_back({prop.key.get(), prop.computed, prop.value.get()});
        }
        rest = p->rest();
    } else {
        const auto* p = static_cast<const Frontend::ObjectExpr*>(pattern);
        for (const auto& prop : p->properties()) {
            entries.push_back({prop.key.get(), prop.computed, prop.value.get()});
        }
    }
    if (rest && rest->type() == Frontend::NodeType::RestElement) {
        rest = static_cast<const Frontend::RestElem*>(rest)->argument();
    }

    for (const auto& entry : entries) {
        fetchSource();
        std::string key;
        if (entry.computed) {
            compileExpression(entry.key);
            emit(Opcode::OP_GET_ELEMENT);
        } else if (staticPropertyKey(entry.key, key)) {
            emitNameOperand(Opcode::OP_GET_PROPERTY, key);
        } else {
            error(entry.key, "Invalid property name in pattern");
            emit(Opcode::OP_POP);
            continue;
        }
        emitBinding(entry.target, kind);
    }
    if (rest) {
        // {a, ...rest}: a copy of the source without the named keys
        fetchSource();
        size_t keyCount = 0;
        for (const auto& entry : entries) {
            std::string key;
            if (entry.computed) {
                compileExpression(entry.key);
            } else if (staticPropertyKey(entry.key, key)) {
                emitConstant(Runtime::Value::string(new Runtime::String(key)));
            } else {
                continue;
            }
            keyCount++;
        }
        if (keyCount > 255) {
            error(pattern, "Too many properties before a rest element (max 255)");
            keyCount = 0;
        }
        emit(Opcode::OP_OBJECT_REST);
        emit(static_cast<uint8_t>(keyCount));
        emitBinding(rest, kind);
    }
}

// var binding: a global at script level, else the function's hoisted slot.
void BytecodeGenerator::emitVarStore(const std::string& name) {
    if (current_->isScript) {
        emitNameOperand(Opcode::OP_DEFINE_GLOBAL, name);
        return;
    }
    int slot = resolveLocal(name);
    if (slot >= 0) {
        emitLocal(Opcode::OP_SET_LOCAL, slot);
        emit(Opcode::OP_POP);
        return;
    }
    emitStoreVariable(name);
    emit(Opcode::OP_POP);
}

// let/const/class binding: a global at script top level, else a new local in
// the current block (or a slot reserved for it ahead of time).
void BytecodeGenerator::emitLexicalBinding(const std::string& name, bool isConst) {
    if (current_->isScript && current_->scopeDepth == 0) {
        emitNameOperand(Opcode::OP_DEFINE_GLOBAL, name);
        return;
    }
    int slot = resolveLocal(name);
    if (slot >= 0 && current_->locals[slot].reserved &&
        current_->locals[slot].depth == current_->scopeDepth) {
        current_->locals[slot].isConst = isConst;
        emitLocal(Opcode::OP_SET_LOCAL, slot);
        emit(Opcode::OP_POP);
        return;
    }
    declareVariable(name, isConst);
}

// Assignment to a name; the value stays on the stack as the expression's value.
void BytecodeGenerator::emitStoreVariable(const std::string& name) {
    int local = resolveLocal(name);
    if (local != -1) {
        if (current_->locals[local].isConst) {
            error("Assignment to constant variable '" + name + "'");
            return;
        }
        emitLocal(Opcode::OP_SET_LOCAL, local);
        return;
    }
    int upvalue = resolveUpvalue(name);
    if (upvalue != -1) {
        emitLocal(Opcode::OP_SET_UPVALUE, upvalue);
        return;
    }
    emitNameOperand(Opcode::OP_SET_GLOBAL, name);
}

bool BytecodeGenerator::isAssignmentPattern(const Frontend::Expression* expr) const {
    switch (expr->type()) {
        case Frontend::NodeType::ArrayExpression:
        case Frontend::NodeType::ObjectExpression:
        case Frontend::NodeType::ArrayPattern:
        case Frontend::NodeType::ObjectPattern:
            return true;
        default:
            return false;
    }
}

// =============================================================================
// Expressions
// =============================================================================

void BytecodeGenerator::compileExpression(const Frontend::Expression* expr) {
    if (!expr) {
        emitConstant(Runtime::Value::undefined());
        return;
    }

    currentLine_ = expr->location().line;

    switch (expr->type()) {
        case Frontend::NodeType::Literal:
            compileLiteral(static_cast<const Frontend::LiteralExpr*>(expr));
            break;
        case Frontend::NodeType::Identifier:
            compileIdentifier(static_cast<const Frontend::IdentifierExpr*>(expr));
            break;
        case Frontend::NodeType::BinaryExpression:
            compileBinaryExpression(static_cast<const Frontend::BinaryExpr*>(expr));
            break;
        case Frontend::NodeType::UnaryExpression:
            compileUnaryExpression(static_cast<const Frontend::UnaryExpr*>(expr));
            break;
        case Frontend::NodeType::LogicalExpression:
            compileLogicalExpression(static_cast<const Frontend::LogicalExpr*>(expr));
            break;
        case Frontend::NodeType::AssignmentExpression:
            compileAssignmentExpression(static_cast<const Frontend::AssignmentExpr*>(expr));
            break;
        case Frontend::NodeType::CallExpression:
            compileCallExpression(static_cast<const Frontend::CallExpr*>(expr));
            break;
        case Frontend::NodeType::MemberExpression:
            compileMemberExpression(static_cast<const Frontend::MemberExpr*>(expr));
            break;
        case Frontend::NodeType::ArrayExpression:
            compileArrayExpression(static_cast<const Frontend::ArrayExpr*>(expr));
            break;
        case Frontend::NodeType::ObjectExpression:
            compileObjectExpression(static_cast<const Frontend::ObjectExpr*>(expr));
            break;
        case Frontend::NodeType::ConditionalExpression:
            compileConditionalExpression(static_cast<const Frontend::ConditionalExpr*>(expr));
            break;
        case Frontend::NodeType::ThisExpression:
            compileThisExpression(static_cast<const Frontend::ThisExpr*>(expr));
            break;
        case Frontend::NodeType::ArrowFunction:
            compileArrowFunction(static_cast<const Frontend::ArrowFunctionExpr*>(expr));
            break;
        case Frontend::NodeType::FunctionExpression:
            compileFunctionExpression(static_cast<const Frontend::FunctionExpr*>(expr));
            break;
        case Frontend::NodeType::AwaitExpression:
            compileAwaitExpression(static_cast<const Frontend::AwaitExpr*>(expr));
            break;
        case Frontend::NodeType::YieldExpression:
            compileYieldExpression(static_cast<const Frontend::YieldExpr*>(expr));
            break;
        case Frontend::NodeType::UpdateExpression:
            compileUpdateExpression(static_cast<const Frontend::UpdateExpr*>(expr));
            break;
        case Frontend::NodeType::NewExpression:
            compileNewExpression(static_cast<const Frontend::NewExpr*>(expr));
            break;
        case Frontend::NodeType::TemplateLiteral:
            compileTemplateLiteral(static_cast<const Frontend::TemplateLiteralExpr*>(expr));
            break;
        case Frontend::NodeType::SpreadElement:
            error(expr, "Unexpected spread element");
            emitConstant(Runtime::Value::undefined());
            break;
        case Frontend::NodeType::RestElement:
        case Frontend::NodeType::ObjectPattern:
        case Frontend::NodeType::ArrayPattern:
        case Frontend::NodeType::AssignmentPattern:
            error(expr, "Pattern used as a value");
            emitConstant(Runtime::Value::undefined());
            break;
        default:
            error(expr, "Unsupported expression");
            emitConstant(Runtime::Value::undefined());
            break;
    }
}

void BytecodeGenerator::compileLiteral(const Frontend::LiteralExpr* expr) {
    const auto& value = expr->value();

    if (std::holds_alternative<double>(value)) {
        emitConstant(Runtime::Value::number(std::get<double>(value)));
    } else if (std::holds_alternative<std::string>(value)) {
        size_t index = nameConstant(std::get<std::string>(value));
        if (index <= 255) {
            emit(Opcode::OP_CONSTANT);
            emit(static_cast<uint8_t>(index));
        } else {
            emit(Opcode::OP_CONSTANT_LONG);
            emitShort(static_cast<uint16_t>(index));
        }
    } else if (std::holds_alternative<bool>(value)) {
        emit(std::get<bool>(value) ? Opcode::OP_TRUE : Opcode::OP_FALSE);
    } else {
        emit(Opcode::OP_NIL);  // null
    }
}

void BytecodeGenerator::compileIdentifier(const Frontend::IdentifierExpr* expr) {
    const std::string& name = expr->name();

    int local = resolveLocal(name);
    if (local != -1) {
        emitLocal(Opcode::OP_GET_LOCAL, local);
        return;
    }

    int upvalue = resolveUpvalue(name);
    if (upvalue != -1) {
        emitLocal(Opcode::OP_GET_UPVALUE, upvalue);
        return;
    }

    if (name == "undefined") {
        emitConstant(Runtime::Value::undefined());
        return;
    }
    emitNameOperand(Opcode::OP_GET_GLOBAL, name);
}

void BytecodeGenerator::compileBinaryExpression(const Frontend::BinaryExpr* expr) {
    // Comma operator: evaluate left, discard, keep right
    if (expr->op() == Frontend::TokenType::Comma) {
        compileExpression(expr->left());
        emit(Opcode::OP_POP);
        compileExpression(expr->right());
        return;
    }

    compileExpression(expr->left());
    compileExpression(expr->right());

    switch (expr->op()) {
        case Frontend::TokenType::Plus: emit(Opcode::OP_ADD); break;
        case Frontend::TokenType::Minus: emit(Opcode::OP_SUBTRACT); break;
        case Frontend::TokenType::Star: emit(Opcode::OP_MULTIPLY); break;
        case Frontend::TokenType::Slash: emit(Opcode::OP_DIVIDE); break;
        case Frontend::TokenType::Percent: emit(Opcode::OP_MODULO); break;
        case Frontend::TokenType::StarStar: emit(Opcode::OP_POWER); break;
        case Frontend::TokenType::Equal: emit(Opcode::OP_EQUAL); break;
        case Frontend::TokenType::StrictEqual: emit(Opcode::OP_STRICT_EQUAL); break;
        case Frontend::TokenType::NotEqual: emit(Opcode::OP_NOT_EQUAL); break;
        case Frontend::TokenType::StrictNotEqual: emit(Opcode::OP_STRICT_NOT_EQUAL); break;
        case Frontend::TokenType::Less: emit(Opcode::OP_LESS); break;
        case Frontend::TokenType::LessEqual: emit(Opcode::OP_LESS_EQUAL); break;
        case Frontend::TokenType::Greater: emit(Opcode::OP_GREATER); break;
        case Frontend::TokenType::GreaterEqual: emit(Opcode::OP_GREATER_EQUAL); break;
        case Frontend::TokenType::Ampersand: emit(Opcode::OP_BITWISE_AND); break;
        case Frontend::TokenType::Pipe: emit(Opcode::OP_BITWISE_OR); break;
        case Frontend::TokenType::Caret: emit(Opcode::OP_BITWISE_XOR); break;
        case Frontend::TokenType::LeftShift: emit(Opcode::OP_LEFT_SHIFT); break;
        case Frontend::TokenType::RightShift: emit(Opcode::OP_RIGHT_SHIFT); break;
        case Frontend::TokenType::UnsignedRightShift: emit(Opcode::OP_UNSIGNED_RIGHT_SHIFT); break;
        case Frontend::TokenType::Instanceof: emit(Opcode::OP_INSTANCEOF); break;
        case Frontend::TokenType::In: emit(Opcode::OP_IN); break;
        default:
            error(expr, "Unknown binary operator");
            break;
    }
}

void BytecodeGenerator::compileUnaryExpression(const Frontend::UnaryExpr* expr) {
    if (expr->op() == Frontend::TokenType::Delete) {
        const Frontend::Expression* target = expr->argument();
        if (target->type() == Frontend::NodeType::MemberExpression) {
            const auto* member = static_cast<const Frontend::MemberExpr*>(target);
            compileExpression(member->object());
            if (member->isComputed()) {
                compileExpression(member->property());
                emit(Opcode::OP_DELETE_ELEMENT);
            } else {
                emitNameOperand(Opcode::OP_DELETE_PROPERTY,
                    static_cast<const Frontend::IdentifierExpr*>(member->property())->name());
            }
        } else if (target->type() == Frontend::NodeType::Identifier) {
            emit(Opcode::OP_FALSE);  // Declared bindings cannot be deleted
        } else {
            compileExpression(target);
            emit(Opcode::OP_POP);
            emit(Opcode::OP_TRUE);
        }
        return;
    }

    compileExpression(expr->argument());

    switch (expr->op()) {
        case Frontend::TokenType::Minus: emit(Opcode::OP_NEGATE); break;
        case Frontend::TokenType::Plus: emit(Opcode::OP_TO_NUMBER); break;
        case Frontend::TokenType::Not: emit(Opcode::OP_NOT); break;
        case Frontend::TokenType::Tilde: emit(Opcode::OP_BITWISE_NOT); break;
        case Frontend::TokenType::Typeof: emit(Opcode::OP_TYPEOF); break;
        case Frontend::TokenType::Void:
            emit(Opcode::OP_POP);
            emitConstant(Runtime::Value::undefined());
            break;
        default:
            error(expr, "Unknown unary operator");
            break;
    }
}

void BytecodeGenerator::compileLogicalExpression(const Frontend::LogicalExpr* expr) {
    compileExpression(expr->left());

    if (expr->op() == Frontend::TokenType::And) {
        size_t endJump = emitJump(Opcode::OP_JUMP_IF_FALSE);
        emit(Opcode::OP_POP);
        compileExpression(expr->right());
        patchJump(endJump);
    } else if (expr->op() == Frontend::TokenType::Or) {
        size_t elseJump = emitJump(Opcode::OP_JUMP_IF_FALSE);
        size_t endJump = emitJump(Opcode::OP_JUMP);
        patchJump(elseJump);
        emit(Opcode::OP_POP);
        compileExpression(expr->right());
        patchJump(endJump);
    } else if (expr->op() == Frontend::TokenType::QuestionQuestion) {
        size_t elseJump = emitJump(Opcode::OP_JUMP_IF_NIL);
        size_t endJump = emitJump(Opcode::OP_JUMP);
        patchJump(elseJump);
        emit(Opcode::OP_POP);
        compileExpression(expr->right());
        patchJump(endJump);
    }
}

void BytecodeGenerator::compileConditionalExpression(const Frontend::ConditionalExpr* expr) {
    compileExpression(expr->test());

    size_t thenJump = emitJump(Opcode::OP_JUMP_IF_FALSE);
    emit(Opcode::OP_POP);
    compileExpression(expr->consequent());

    size_t elseJump = emitJump(Opcode::OP_JUMP);
    patchJump(thenJump);
    emit(Opcode::OP_POP);
    compileExpression(expr->alternate());
    patchJump(elseJump);
}

namespace {

// Arithmetic opcode of a compound assignment (+=, ...); OP_NOP for = and the
// logical forms, which short-circuit instead.
Opcode compoundOperator(Frontend::TokenType op) {
    switch (op) {
        case Frontend::TokenType::PlusAssign: return Opcode::OP_ADD;
        case Frontend::TokenType::MinusAssign: return Opcode::OP_SUBTRACT;
        case Frontend::TokenType::StarAssign: return Opcode::OP_MULTIPLY;
        case Frontend::TokenType::SlashAssign: return Opcode::OP_DIVIDE;
        case Frontend::TokenType::PercentAssign: return Opcode::OP_MODULO;
        case Frontend::TokenType::StarStarAssign: return Opcode::OP_POWER;
        case Frontend::TokenType::AmpersandAssign: return Opcode::OP_BITWISE_AND;
        case Frontend::TokenType::PipeAssign: return Opcode::OP_BITWISE_OR;
        case Frontend::TokenType::CaretAssign: return Opcode::OP_BITWISE_XOR;
        case Frontend::TokenType::LeftShiftAssign: return Opcode::OP_LEFT_SHIFT;
        case Frontend::TokenType::RightShiftAssign: return Opcode::OP_RIGHT_SHIFT;
        case Frontend::TokenType::UnsignedRightShiftAssign: return Opcode::OP_UNSIGNED_RIGHT_SHIFT;
        default: return Opcode::OP_NOP;
    }
}

// Short-circuit opcode of a logical assignment (&&=, ||=, ??=); OP_NOP otherwise.
Opcode logicalAssignOperator(Frontend::TokenType op) {
    switch (op) {
        case Frontend::TokenType::AndAssign: return Opcode::OP_AND;
        case Frontend::TokenType::OrAssign: return Opcode::OP_OR;
        case Frontend::TokenType::QuestionQuestionAssign: return Opcode::OP_NULLISH;
        default: return Opcode::OP_NOP;
    }
}

} // namespace

void BytecodeGenerator::compileAssignmentExpression(const Frontend::AssignmentExpr* expr) {
    const Frontend::Expression* left = expr->left();
    const Opcode arithmetic = compoundOperator(expr->op());
    const Opcode logical = logicalAssignOperator(expr->op());

    if (isAssignmentPattern(left)) {
        if (expr->op() != Frontend::TokenType::Assign) {
            error(expr, "Invalid left-hand side in assignment");
            return;
        }
        compileExpression(expr->right());
        emitPatternElements(left, BindingKind::Assign, -1);  // Leaves the source as the value
        return;
    }

    if (left->type() == Frontend::NodeType::MemberExpression) {
        const auto* member = static_cast<const Frontend::MemberExpr*>(left);
        const bool computed = member->isComputed();
        const std::string* name = computed ? nullptr
            : &static_cast<const Frontend::IdentifierExpr*>(member->property())->name();

        // Object, then the key, each evaluated once
        compileExpression(member->object());
        if (computed) compileExpression(member->property());

        auto emitRead = [&]() {
            if (computed) {
                emit(Opcode::OP_DUP2);          // [obj, key, obj, key]
                emit(Opcode::OP_GET_ELEMENT);   // [obj, key, current]
            } else {
                emit(Opcode::OP_DUP);           // [obj, obj]
                emitNameOperand(Opcode::OP_GET_PROPERTY, *name);
            }
        };
        auto emitStore = [&]() {
            if (computed) {
                emit(Opcode::OP_SET_ELEMENT);
            } else {
                emitNameOperand(Opcode::OP_SET_PROPERTY, *name);
            }
        };

        if (logical != Opcode::OP_NOP) {
            emitRead();                         // [obj, (key), current]
            size_t skip = emitJump(logical);    // Keeps current and jumps, or pops it
            compileExpression(expr->right());
            emitStore();                        // [value]
            size_t done = emitJump(Opcode::OP_JUMP);
            patchJump(skip);
            // Short-circuited: drop the object (and key) below the current value
            if (computed) {
                emit(Opcode::OP_DUP_X2);        // [current, obj, key, current]
                emit(Opcode::OP_POP);
                emit(Opcode::OP_POP);
                emit(Opcode::OP_POP);
            } else {
                emit(Opcode::OP_SWAP);          // [current, obj]
                emit(Opcode::OP_POP);
            }
            patchJump(done);
            return;
        }

        if (arithmetic != Opcode::OP_NOP) emitRead();
        compileExpression(expr->right());
        if (arithmetic != Opcode::OP_NOP) emit(arithmetic);
        emitStore();                            // Leaves the stored value
        return;
    }

    const std::string* name = identifierName(left);
    if (!name) {
        error(expr, "Invalid left-hand side in assignment");
        compileExpression(expr->right());
        return;
    }

    if (logical != Opcode::OP_NOP) {
        compileIdentifier(static_cast<const Frontend::IdentifierExpr*>(left));
        size_t skip = emitJump(logical);
        compileExpression(expr->right());
        emitStoreVariable(*name);
        patchJump(skip);
        return;
    }
    if (arithmetic != Opcode::OP_NOP) {
        compileIdentifier(static_cast<const Frontend::IdentifierExpr*>(left));
    }
    compileExpression(expr->right());
    if (arithmetic != Opcode::OP_NOP) emit(arithmetic);
    emitStoreVariable(*name);
}

void BytecodeGenerator::compileUpdateExpression(const Frontend::UpdateExpr* expr) {
    const Frontend::Expression* arg = expr->argument();
    const bool isPrefix = expr->isPrefix();
    const Opcode step = expr->op() == Frontend::TokenType::PlusPlus
        ? Opcode::OP_INCREMENT : Opcode::OP_DECREMENT;

    if (const std::string* name = identifierName(arg)) {
        compileIdentifier(static_cast<const Frontend::IdentifierExpr*>(arg));
        emit(Opcode::OP_TO_NUMBER);
        if (!isPrefix) emit(Opcode::OP_DUP);   // [old, old]
        emit(step);                             // [(old), new]
        emitStoreVariable(*name);
        if (!isPrefix) emit(Opcode::OP_POP);    // [old]
        return;
    }

    if (arg->type() != Frontend::NodeType::MemberExpression) {
        error(expr, "Invalid update expression target");
        emitConstant(Runtime::Value::undefined());
        return;
    }

    // Read the current value, keeping object (and computed key) for the store
    const auto* member = static_cast<const Frontend::MemberExpr*>(arg);
    compileExpression(member->object());
    if (member->isComputed()) {
        compileExpression(member->property());
        emit(Opcode::OP_DUP2);                      // [obj, key, obj, key]
        emit(Opcode::OP_GET_ELEMENT);               // [obj, key, old]
        emit(Opcode::OP_TO_NUMBER);
        if (!isPrefix) emit(Opcode::OP_DUP_X2);     // [old, obj, key, old]
        emit(step);
        emit(Opcode::OP_SET_ELEMENT);
    } else {
        const std::string& name = static_cast<const Frontend::IdentifierExpr*>(member->property())->name();
        emit(Opcode::OP_DUP);                       // [obj, obj]
        emitNameOperand(Opcode::OP_GET_PROPERTY, name);  // [obj, old]
        emit(Opcode::OP_TO_NUMBER);
        if (!isPrefix) emit(Opcode::OP_DUP_X1);     // [old, obj, old]
        emit(step);
        emitNameOperand(Opcode::OP_SET_PROPERTY, name);
    }
    // The store leaves the new value; postfix yields the old one saved below it
    if (!isPrefix) emit(Opcode::OP_POP);
}

// Array of call arguments, built at run time when any argument is spread.
void BytecodeGenerator::emitArgumentArray(const std::vector<Frontend::ExprPtr>& args) {
    emit(Opcode::OP_CREATE_ARRAY);
    emitShort(0);
    for (const auto& arg : args) {
        if (arg->type() == Frontend::NodeType::SpreadElement) {
            compileExpression(static_cast<const Frontend::SpreadExpr*>(arg.get())->argument());
            emit(Opcode::OP_SPREAD);
        } else {
            compileExpression(arg.get());
            emit(Opcode::OP_INIT_ELEMENT);
        }
    }
}

void BytecodeGenerator::compileCallExpression(const Frontend::CallExpr* expr) {
    const Frontend::Expression* callee = expr->callee();
    const auto& args = expr->arguments();
    const bool spread = hasSpread(args);

    if (callee->type() == Frontend::NodeType::MemberExpression) {
        const auto* member = static_cast<const Frontend::MemberExpr*>(callee);

        if (!member->isComputed() && !member->isOptional() && !expr->isOptional() && !spread) {
            compileExpression(member->object());
            for (const auto& arg : args) compileExpression(arg.get());
            emit(Opcode::OP_CALL_METHOD);
            emitShort(static_cast<uint16_t>(nameConstant(
                static_cast<const Frontend::IdentifierExpr*>(member->property())->name())));
            emitArgCount(args.size());
            return;
        }

        // General method call: look the function up, keeping the object as
        // the receiver. a?.b() and a.b?.() give undefined on a nullish link.
        compileExpression(member->object());               // [obj]
        size_t objectNil = SIZE_MAX;
        if (member->isOptional()) {
            emit(Opcode::OP_DUP);
            objectNil = emitJump(Opcode::OP_JUMP_IF_NIL);  // [obj, obj]
            emit(Opcode::OP_POP);
        }
        emit(Opcode::OP_DUP);
        if (member->isComputed()) {
            compileExpression(member->property());
            emit(Opcode::OP_GET_ELEMENT);
        } else {
            emitNameOperand(Opcode::OP_GET_PROPERTY,
                static_cast<const Frontend::IdentifierExpr*>(member->property())->name());
        }                                                   // [obj, fn]
        size_t functionNil = SIZE_MAX;
        if (expr->isOptional()) {
            emit(Opcode::OP_DUP);
            functionNil = emitJump(Opcode::OP_JUMP_IF_NIL); // [obj, fn, fn]
            emit(Opcode::OP_POP);
        }
        if (spread) {
            emitArgumentArray(args);
            emit(Opcode::OP_CALL_SPREAD);
        } else {
            for (const auto& arg : args) compileExpression(arg.get());
            emit(Opcode::OP_CALL_WITH_THIS);
            emitArgCount(args.size());
        }
        if (objectNil == SIZE_MAX && functionNil == SIZE_MAX) return;

        size_t done = emitJump(Opcode::OP_JUMP);
        if (functionNil != SIZE_MAX) {
            patchJump(functionNil);
            emit(Opcode::OP_POP);  // fn copy
            emit(Opcode::OP_POP);  // fn
            emit(Opcode::OP_POP);  // obj
            emitConstant(Runtime::Value::undefined());
        }
        if (objectNil != SIZE_MAX) {
            size_t skip = functionNil != SIZE_MAX ? emitJump(Opcode::OP_JUMP) : SIZE_MAX;
            patchJump(objectNil);
            emit(Opcode::OP_POP);  // obj copy
            emit(Opcode::OP_POP);  // obj
            emitConstant(Runtime::Value::undefined());
            if (skip != SIZE_MAX) patchJump(skip);
        }
        patchJump(done);
        return;
    }

    // Plain call: no receiver
    if (spread) {
        emitConstant(Runtime::Value::undefined());
        compileExpression(callee);
        size_t functionNil = SIZE_MAX;
        if (expr->isOptional()) {
            emit(Opcode::OP_DUP);
            functionNil = emitJump(Opcode::OP_JUMP_IF_NIL);
            emit(Opcode::OP_POP);
        }
        emitArgumentArray(args);
        emit(Opcode::OP_CALL_SPREAD);
        if (functionNil != SIZE_MAX) {
            size_t done = emitJump(Opcode::OP_JUMP);
            patchJump(functionNil);
            emit(Opcode::OP_POP);
            emit(Opcode::OP_POP);
            emit(Opcode::OP_POP);
            emitConstant(Runtime::Value::undefined());
            patchJump(done);
        }
        return;
    }

    compileExpression(callee);
    size_t functionNil = SIZE_MAX;
    if (expr->isOptional()) {
        emit(Opcode::OP_DUP);
        functionNil = emitJump(Opcode::OP_JUMP_IF_NIL);
        emit(Opcode::OP_POP);
    }
    for (const auto& arg : args) compileExpression(arg.get());
    emit(Opcode::OP_CALL);
    emitArgCount(args.size());
    if (functionNil != SIZE_MAX) {
        size_t done = emitJump(Opcode::OP_JUMP);
        patchJump(functionNil);
        emit(Opcode::OP_POP);
        emit(Opcode::OP_POP);
        emitConstant(Runtime::Value::undefined());
        patchJump(done);
    }
}

void BytecodeGenerator::compileMemberExpression(const Frontend::MemberExpr* expr) {
    compileExpression(expr->object());

    auto emitAccess = [&]() {
        if (expr->isComputed()) {
            compileExpression(expr->property());
            emit(Opcode::OP_GET_ELEMENT);
        } else {
            emitNameOperand(Opcode::OP_GET_PROPERTY,
                static_cast<const Frontend::IdentifierExpr*>(expr->property())->name());
        }
    };

    if (!expr->isOptional()) {
        emitAccess();
        return;
    }

    // obj?.prop: undefined when obj is null or undefined
    size_t nilJump = emitJump(Opcode::OP_JUMP_IF_NIL);
    emitAccess();
    size_t doneJump = emitJump(Opcode::OP_JUMP);
    patchJump(nilJump);
    emit(Opcode::OP_POP);
    emitConstant(Runtime::Value::undefined());
    patchJump(doneJump);
}

void BytecodeGenerator::compileNewExpression(const Frontend::NewExpr* expr) {
    compileExpression(expr->callee());
    if (hasSpread(expr->arguments())) {
        emitArgumentArray(expr->arguments());
        emit(Opcode::OP_NEW_SPREAD);
        return;
    }
    for (const auto& arg : expr->arguments()) {
        compileExpression(arg.get());
    }
    emit(Opcode::OP_NEW);
    emitArgCount(expr->arguments().size());
}

void BytecodeGenerator::compileArrayExpression(const Frontend::ArrayExpr* expr) {
    const auto& elements = expr->elements();
    if (hasSpread(elements)) {
        emit(Opcode::OP_CREATE_ARRAY);
        emitShort(0);
        for (const auto& element : elements) {
            if (!element) {
                emitConstant(Runtime::Value::undefined());  // Hole
                emit(Opcode::OP_INIT_ELEMENT);
            } else if (element->type() == Frontend::NodeType::SpreadElement) {
                compileExpression(static_cast<const Frontend::SpreadExpr*>(element.get())->argument());
                emit(Opcode::OP_SPREAD);
            } else {
                compileExpression(element.get());
                emit(Opcode::OP_INIT_ELEMENT);
            }
        }
        return;
    }

    if (elements.size() > 0xFFFF) {
        error(expr, "Array literal too large (max 65535 elements)");
        return;
    }
    for (const auto& element : elements) {
        if (element) {
            compileExpression(element.get());
        } else {
            emitConstant(Runtime::Value::undefined());  // Hole
        }
    }
    emit(Opcode::OP_CREATE_ARRAY);
    emitShort(static_cast<uint16_t>(elements.size()));
}

void BytecodeGenerator::compileObjectExpression(const Frontend::ObjectExpr* expr) {
    emit(Opcode::OP_CREATE_OBJECT);

    for (const auto& prop : expr->properties()) {
        std::string key;
        if (prop.computed) {
            compileExpression(prop.key.get());
        } else if (staticPropertyKey(prop.key.get(), key)) {
            size_t index = nameConstant(key);
            if (index <= 255) {
                emit(Opcode::OP_CONSTANT);
                emit(static_cast<uint8_t>(index));
            } else {
                emit(Opcode::OP_CONSTANT_LONG);
                emitShort(static_cast<uint16_t>(index));
            }
        } else {
            error(prop.key.get(), "Invalid property name");
            continue;
        }
        compileExpression(prop.value.get());
        emit(Opcode::OP_INIT_PROPERTY);
    }
}

void BytecodeGenerator::compileThisExpression(const Frontend::ThisExpr*) {
    int slot = resolveLocal("this");
    if (slot != -1) {
        emitLocal(Opcode::OP_GET_LOCAL, slot);
        return;
    }
    // Arrow functions use the `this` of the function they are written in
    int upvalue = resolveUpvalue("this");
    if (upvalue != -1) {
        emitLocal(Opcode::OP_GET_UPVALUE, upvalue);
        return;
    }
    // Script top level: the global object
    emitNameOperand(Opcode::OP_GET_GLOBAL, "globalThis");
}

void BytecodeGenerator::compileFunctionExpression(const Frontend::FunctionExpr* expr) {
    auto* fn = new Runtime::Function(expr->name(), expectedArgumentCount(expr->params()),
                                     false, expr->isGenerator(), expr->isAsync());
    FunctionSource source;
    source.name = expr->name();
    source.params = &expr->params();
    source.body = expr->body();
    source.bindsOwnName = true;
    compileFunction(source, fn);
}

void BytecodeGenerator::compileArrowFunction(const Frontend::ArrowFunctionExpr* expr) {
    auto* fn = new Runtime::Function("", expectedArgumentCount(expr->params()),
                                     true, false, expr->isAsync());
    FunctionSource source;
    source.params = &expr->params();
    source.isArrow = true;
    if (expr->hasExpressionBody()) {
        source.expressionBody = expr->expressionBody();
    } else {
        source.body = expr->blockBody();
    }
    compileFunction(source, fn);
}

void BytecodeGenerator::compileAwaitExpression(const Frontend::AwaitExpr* expr) {
    compileExpression(expr->argument());
    emit(Opcode::OP_AWAIT);  // Leaves the settled value
}

void BytecodeGenerator::compileYieldExpression(const Frontend::YieldExpr* expr) {
    if (expr->argument()) {
        compileExpression(expr->argument());
    } else {
        emitConstant(Runtime::Value::undefined());
    }
    emit(Opcode::OP_YIELD);
    emit(static_cast<uint8_t>(expr->delegate() ? 1 : 0));
    // The value passed to next() is left on the stack
}

void BytecodeGenerator::compileTemplateLiteral(const Frontend::TemplateLiteralExpr* expr) {
    // `a${x}b` => "a" + x + "b"
    const auto& quasis = expr->quasis();
    const auto& expressions = expr->expressions();

    emitConstant(Runtime::Value::string(new Runtime::String(quasis.empty() ? "" : quasis[0])));
    for (size_t i = 0; i < expressions.size(); ++i) {
        compileExpression(expressions[i].get());
        emit(Opcode::OP_ADD);
        if (i + 1 < quasis.size()) {
            emitConstant(Runtime::Value::string(new Runtime::String(quasis[i + 1])));
            emit(Opcode::OP_ADD);
        }
    }
}

// =============================================================================
// Modules and classes
// =============================================================================

void BytecodeGenerator::compileImportDeclaration(const Frontend::ImportDecl* decl) {
    emitNameOperand(Opcode::OP_IMPORT, decl->source());  // Pushes the exports object

    if (bindsToLocals(BindingKind::Const)) {
        // The bindings become locals above the exports object, which stays
        // in a hidden slot until the scope ends
        int exportsSlot = addTemporary("exports");
        for (const auto& spec : decl->specifiers()) {
            emitLocal(Opcode::OP_GET_LOCAL, exportsSlot);
            emitNameOperand(Opcode::OP_GET_PROPERTY, spec.imported);
            emitLexicalBinding(spec.local, true);
        }
        return;
    }
    for (const auto& spec : decl->specifiers()) {
        emit(Opcode::OP_DUP);
        emitNameOperand(Opcode::OP_GET_PROPERTY, spec.imported);
        emitLexicalBinding(spec.local, true);  // A global: consumes the value
    }
    emit(Opcode::OP_POP);
}

void BytecodeGenerator::compileExportDeclaration(const Frontend::ExportDecl* decl) {
    auto exportName = [&](const std::string& local, const std::string& exported) {
        Frontend::IdentifierExpr id(local);
        compileIdentifier(&id);
        emitNameOperand(Opcode::OP_EXPORT, exported);
    };

    if (decl->hasDeclaration()) {
        const Frontend::Statement* declaration = decl->declaration();
        compileStatement(declaration);
        if (declaration->type() == Frontend::NodeType::FunctionDeclaration) {
            const auto& name = static_cast<const Frontend::FunctionDecl*>(declaration)->name();
            exportName(name, name);
        } else if (declaration->type() == Frontend::NodeType::ClassDeclaration) {
            const auto& name = static_cast<const Frontend::ClassDecl*>(declaration)->name();
            exportName(name, name);
        } else if (declaration->type() == Frontend::NodeType::VariableDeclaration) {
            std::vector<std::string> names;
            for (const auto& declarator : static_cast<const Frontend::VariableDecl*>(declaration)->declarators()) {
                collectPatternNames(declarator.id.get(), names);
            }
            for (const auto& name : names) exportName(name, name);
        }
    }

    // export { local as exported }
    for (const auto& spec : decl->specifiers()) {
        exportName(spec.local, spec.exported);
    }
}

void BytecodeGenerator::compileClassDeclaration(const Frontend::ClassDecl* decl) {
    const std::string& className = decl->name();

    // The binding exists before the class body is evaluated, so methods can
    // refer to the class by name
    const int slot = reserveLexicalSlot(className);

    const Frontend::FunctionExpr* ctor = decl->constructor();
    auto* ctorFn = new Runtime::Function(className, ctor ? expectedArgumentCount(ctor->params()) : 0,
                                         false, false, false);
    FunctionSource ctorSource;
    ctorSource.name = className;
    ctorSource.isMethod = true;
    if (ctor) {
        ctorSource.params = &ctor->params();
        ctorSource.body = ctor->body();
    }
    ctorSource.implicitSuperClass = decl->superClass();
    compileFunction(ctorSource, ctorFn);           // [ctor]

    if (decl->superClass()) {
        compileExpression(decl->superClass());
        emit(Opcode::OP_INHERIT);                   // Links the prototypes; leaves [ctor]
    }

    for (const auto& method : decl->methods()) {
        const Frontend::FunctionExpr* fnExpr = method.function.get();
        auto* methodFn = new Runtime::Function(method.name, expectedArgumentCount(fnExpr->params()),
                                               false, fnExpr->isGenerator(), fnExpr->isAsync());
        FunctionSource source;
        source.name = method.name;
        source.params = &fnExpr->params();
        source.body = fnExpr->body();
        source.isMethod = true;
        compileFunction(source, methodFn);          // [ctor, method]

        Opcode define = Opcode::OP_DEFINE_METHOD;
        if (method.isStatic) define = Opcode::OP_DEFINE_STATIC;
        else if (method.isGetter) define = Opcode::OP_DEFINE_GETTER;
        else if (method.isSetter) define = Opcode::OP_DEFINE_SETTER;
        emitNameOperand(define, method.name);       // Pops the method; leaves [ctor]
    }

    if (slot < 0) {
        emitNameOperand(Opcode::OP_DEFINE_GLOBAL, className);
    } else {
        emitLocal(Opcode::OP_SET_LOCAL, slot);
        emit(Opcode::OP_POP);
    }
}

} // namespace Zepra::Bytecode
