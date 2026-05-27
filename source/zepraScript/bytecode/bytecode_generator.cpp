// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file bytecode_generator.cpp
 * @brief AST to bytecode compiler implementation
 */

#include "bytecode/bytecode_generator.hpp"
#include <algorithm>
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
    // Deduplicate: reuse existing constant for identical values
    for (size_t i = 0; i < constants_.size(); i++) {
        const auto& existing = constants_[i];
        if (existing.type() != value.type()) continue;
        
        if (value.isNumber() && existing.asNumber() == value.asNumber()) return i;
        if (value.isBoolean() && existing.asBoolean() == value.asBoolean()) return i;
        if (value.isNull() && existing.isNull()) return i;
        if (value.isUndefined() && existing.isUndefined()) return i;
        if (value.isString() && existing.isString()) {
            auto* a = static_cast<Runtime::String*>(value.asObject());
            auto* b = static_cast<Runtime::String*>(existing.asObject());
            if (a->value() == b->value()) return i;
        }
    }
    
    constants_.push_back(value);
    return constants_.size() - 1;
}

void BytecodeChunk::patchJump(size_t offset) {
    size_t jump = currentOffset() - offset - 2;
    
    if (jump > 0xFFFF) {
        // Jump too long - would need a different strategy
        return;
    }
    
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
    
    Opcode instruction = static_cast<Opcode>(code_[offset]);
    
    switch (instruction) {
        case Opcode::OP_CONSTANT: {
            uint8_t constant = code_[offset + 1];
            std::cout << "OP_CONSTANT     " << (int)constant << " '";
            // Print constant value
            std::cout << "'\n";
            return offset + 2;
        }
        case Opcode::OP_NIL:
            std::cout << "OP_NIL\n";
            return offset + 1;
        case Opcode::OP_TRUE:
            std::cout << "OP_TRUE\n";
            return offset + 1;
        case Opcode::OP_FALSE:
            std::cout << "OP_FALSE\n";
            return offset + 1;
        case Opcode::OP_ADD:
            std::cout << "OP_ADD\n";
            return offset + 1;
        case Opcode::OP_SUBTRACT:
            std::cout << "OP_SUBTRACT\n";
            return offset + 1;
        case Opcode::OP_MULTIPLY:
            std::cout << "OP_MULTIPLY\n";
            return offset + 1;
        case Opcode::OP_DIVIDE:
            std::cout << "OP_DIVIDE\n";
            return offset + 1;
        case Opcode::OP_RETURN:
            std::cout << "OP_RETURN\n";
            return offset + 1;
        default:
            std::cout << "Unknown opcode " << (int)instruction << "\n";
            return offset + 1;
    }
}

// BytecodeGenerator implementation
BytecodeGenerator::BytecodeGenerator() {
}

std::unique_ptr<BytecodeChunk> BytecodeGenerator::compile(const Frontend::Program* program) {
    // Create main script state
    CompilerState mainState;
    mainState.functionName = "<script>";
    current_ = &mainState;
    
    bool lastWasExprStatement = false;
    
    // Compile all statements
    for (size_t i = 0; i < program->body().size(); i++) {
        const auto& stmt = program->body()[i];
        bool isLast = (i == program->body().size() - 1);
        
        // For the last expression statement, don't pop the result
        if (isLast && stmt->type() == Frontend::NodeType::ExpressionStatement) {
            compileExpression(static_cast<const Frontend::ExprStmt*>(stmt.get())->expression());
            lastWasExprStatement = true;
        } else {
            compileStatement(stmt.get());
        }
    }
    
    // End with a return - if last was expression, it's on stack; otherwise push nil
    if (!lastWasExprStatement) {
        emit(Opcode::OP_NIL);
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
    currentChunk()->patchJump(offset);
}

void BytecodeGenerator::emitLoop(size_t loopStart) {
    emit(Opcode::OP_LOOP);
    
    size_t offset = currentChunk()->currentOffset() - loopStart + 2;
    if (offset > 0xFFFF) {
        error("Loop body too large");
    }
    
    emitShort(static_cast<uint16_t>(offset));
}

size_t BytecodeGenerator::makeConstant(Runtime::Value value) {
    size_t constant = currentChunk()->addConstant(value);
    if (constant > 65535) {
        error("Too many constants in one chunk (max 65536)");
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
    Local local;
    local.name = name;
    local.depth = current_->scopeDepth;
    local.isCaptured = false;
    local.isConst = isConst;
    current_->locals.push_back(local);
}

int BytecodeGenerator::resolveLocal(const std::string& name) {
    for (int i = static_cast<int>(current_->locals.size()) - 1; i >= 0; i--) {
        const Local& local = current_->locals[i];
        if (local.name == name) {
            return i;
        }
    }
    return -1;
}

int BytecodeGenerator::resolveUpvalue(const std::string& name) {
    if (!current_->enclosing) return -1;
    
    // Check enclosing function's locals
    int local = -1;
    CompilerState* enclosing = current_->enclosing;
    for (int i = static_cast<int>(enclosing->locals.size()) - 1; i >= 0; i--) {
        if (enclosing->locals[i].name == name) {
            enclosing->locals[i].isCaptured = true;
            local = i;
            break;
        }
    }
    
    if (local != -1) {
        Upvalue upvalue;
        upvalue.index = static_cast<uint8_t>(local);
        upvalue.isLocal = true;
        current_->upvalues.push_back(upvalue);
        return static_cast<int>(current_->upvalues.size() - 1);
    }
    
    return -1;
}

void BytecodeGenerator::declareVariable(const std::string& name, bool isConst) {
    if (current_->scopeDepth == 0) {
        // Global variable
        return;
    }
    
    // Check for duplicate declaration
    for (int i = static_cast<int>(current_->locals.size()) - 1; i >= 0; i--) {
        const Local& local = current_->locals[i];
        if (local.depth < current_->scopeDepth) break;
        if (local.name == name) {
            error("Variable '" + name + "' already declared in this scope");
        }
    }
    
    addLocal(name, isConst);
}

void BytecodeGenerator::defineVariable(const std::string& name) {
    if (current_->scopeDepth > 0) {
        // Local variable - mark as initialized (exit temporal dead zone)
        if (!current_->locals.empty()) {
            current_->locals.back().depth = current_->scopeDepth;
        }
        return;
    }
    
    // Global variable
    size_t constant = makeConstant(Runtime::Value::string(new Runtime::String(name)));
    emit(Opcode::OP_DEFINE_GLOBAL);
    emit(static_cast<uint8_t>(constant));
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
            error("Unknown statement type");
            break;
    }
}

void BytecodeGenerator::compileBlockStatement(const Frontend::BlockStmt* stmt) {
    beginScope();
    for (const auto& s : stmt->body()) {
        compileStatement(s.get());
    }
    endScope();
}

void BytecodeGenerator::compileExpressionStatement(const Frontend::ExprStmt* stmt) {
    compileExpression(stmt->expression());
    emit(Opcode::OP_POP);
}

void BytecodeGenerator::compileVariableDeclaration(const Frontend::VariableDecl* decl) {
    bool isConstVar = decl->kind() == Frontend::VariableDecl::Kind::Const;
    
    for (const auto& declarator : decl->declarators()) {
        const auto* idExpr = declarator.id.get();
        
        if (idExpr->type() == Frontend::NodeType::Identifier) {
            const auto* id = static_cast<const Frontend::IdentifierExpr*>(idExpr);
            std::string varName = id->name();
            declareVariable(varName, isConstVar);
            if (declarator.init) {
                compileExpression(declarator.init.get());
            } else {
                emitConstant(Runtime::Value::undefined());
            }
            defineVariable(varName);
        } else if (idExpr->type() == Frontend::NodeType::ObjectPattern ||
                   idExpr->type() == Frontend::NodeType::ArrayPattern) {
            if (declarator.init) {
                compileExpression(declarator.init.get());
            } else {
                emitConstant(Runtime::Value::undefined());
            }
            emitBindingPattern(idExpr, isConstVar);
        } else {
            error("Unsupported variable declarator type");
        }
    }
}

void BytecodeGenerator::compileFunctionDeclaration(const Frontend::FunctionDecl* decl) {
    declareVariable(decl->name(), false);
    
    // Compile function body into new chunk
    CompilerState fnState;
    fnState.enclosing = current_;
    fnState.functionName = decl->name();
    current_ = &fnState;
    
    // Add parameters as locals at depth 0 (function's base scope)
    // They are already on the stack when the function is called
    // Reserve slot 0 for 'this' — matches OP_CALL/OP_NEW stack layout
    Local thisLocal;
    thisLocal.name = "this";
    thisLocal.depth = 0;
    thisLocal.isCaptured = false;
    thisLocal.isConst = true;
    current_->locals.push_back(thisLocal);
    
    int paramCount = 0;
    for (const auto& param : decl->params()) {
        const auto* id = static_cast<const Frontend::IdentifierExpr*>(param.pattern.get());
        
        // Add local at depth 0 so they don't get popped by endScope
        Local local;
        local.name = id->name();
        local.isConst = false;
        current_->locals.push_back(local);
        
        // Handle default value
        if (param.defaultValue) {
            // if (param === undefined) param = defaultValue;
            emit(Opcode::OP_GET_LOCAL);
            emit(static_cast<uint8_t>(paramCount));
            emit(Opcode::OP_NIL);
            emit(Opcode::OP_STRICT_EQUAL);
            
            size_t skipDefault = emitJump(Opcode::OP_JUMP_IF_FALSE);
            emit(Opcode::OP_POP); // Pop the 'true' boolean
            
            compileExpression(param.defaultValue.get());
            emit(Opcode::OP_SET_LOCAL);
            emit(static_cast<uint8_t>(paramCount));
            emit(Opcode::OP_POP); // Pop the set value
            
            size_t doneJump = emitJump(Opcode::OP_JUMP);
            
            patchJump(skipDefault);
            emit(Opcode::OP_POP); // Pop the 'false' boolean
            
            patchJump(doneJump);
        }
        // Handle rest parameter (must be last)
        if (param.rest) {
            // Rest parameter collects remaining arguments into an array.
            // The runtime's function call mechanism already sets up the
            // local slot with an Array of remaining args at this index.
            // Emit marker so the VM knows this slot receives rest args.
            emit(Opcode::OP_CREATE_ARRAY);
            emit(static_cast<uint8_t>(0)); // initial size = 0, filled by runtime
        }
        
        paramCount++;
    }
    
    // Compile body (each block statement inside will handle its own scope)
    for (const auto& stmt : decl->body()->body()) {
        compileStatement(stmt.get());
    }
    
    // Default return undefined
    emitConstant(Runtime::Value::undefined());
    emit(Opcode::OP_RETURN);
    
    // Create the function's bytecode chunk
    auto functionChunk = std::make_unique<BytecodeChunk>(std::move(fnState.chunk));
    
    current_ = fnState.enclosing;
    
    // Create a Function object with the compiled bytecode
    auto* function = new Runtime::Function(decl, nullptr);
    function->setBytecodeChunk(functionChunk.get());

    // Store debug metadata for frame introspection
    std::vector<std::string> paramNames;
    std::vector<std::string> localNames;
    for (const auto& param : decl->params()) {
        const auto* id = static_cast<const Frontend::IdentifierExpr*>(param.pattern.get());
        paramNames.push_back(id->name());
    }
    for (const auto& local : fnState.locals) {
        localNames.push_back(local.name);
    }
    function->setParamNames(std::move(paramNames));
    function->setLocalNames(std::move(localNames));
    
    // Store the chunk (transferred ownership to function)
    // Note: In a real implementation, this would be part of the Function object
    static std::vector<std::unique_ptr<BytecodeChunk>> compiledChunks;
    compiledChunks.push_back(std::move(functionChunk));
    
    // Add function as constant and emit OP_CLOSURE
    size_t funcIndex = makeConstant(Runtime::Value::object(function));
    emit(Opcode::OP_CLOSURE);
    emit(static_cast<uint8_t>(funcIndex));
    
    // Emit upvalue metadata: count followed by (isLocal, index) pairs
    emit(static_cast<uint8_t>(fnState.upvalues.size()));
    for (const auto& upval : fnState.upvalues) {
        emit(upval.isLocal ? 1 : 0);
        emit(upval.index);
    }
    
    defineVariable(decl->name());
}

void BytecodeGenerator::compileIfStatement(const Frontend::IfStmt* stmt) {
    compileExpression(stmt->test());
    
    size_t thenJump = emitJump(Opcode::OP_JUMP_IF_FALSE);
    emit(Opcode::OP_POP); // Pop condition
    
    compileStatement(stmt->consequent());
    
    size_t elseJump = emitJump(Opcode::OP_JUMP);
    
    patchJump(thenJump);
    emit(Opcode::OP_POP); // Pop condition
    
    if (stmt->alternate()) {
        compileStatement(stmt->alternate());
    }
    
    patchJump(elseJump);
}

void BytecodeGenerator::compileWhileStatement(const Frontend::WhileStmt* stmt) {
    size_t loopStart = currentChunk()->currentOffset();
    
    compileExpression(stmt->test());
    
    size_t exitJump = emitJump(Opcode::OP_JUMP_IF_FALSE);
    emit(Opcode::OP_POP);
    
    // Track loop for break/continue
    breakJumps_.push_back({});
    continueTargets_.push_back(loopStart);
    
    compileStatement(stmt->body());
    
    emitLoop(loopStart);
    
    patchJump(exitJump);
    emit(Opcode::OP_POP);
    
    // Patch breaks
    for (size_t breakJump : breakJumps_.back()) {
        patchJump(breakJump);
    }
    breakJumps_.pop_back();
    continueTargets_.pop_back();
}

void BytecodeGenerator::compileDoWhileStatement(const Frontend::DoWhileStmt* stmt) {
    size_t loopStart = currentChunk()->currentOffset();
    
    breakJumps_.push_back({});
    continueTargets_.push_back(loopStart);
    
    compileStatement(stmt->body());
    
    compileExpression(stmt->test());
    size_t exitJump = emitJump(Opcode::OP_JUMP_IF_FALSE);
    emit(Opcode::OP_POP);
    
    emitLoop(loopStart);
    
    patchJump(exitJump);
    emit(Opcode::OP_POP);
    
    for (size_t breakJump : breakJumps_.back()) {
        patchJump(breakJump);
    }
    breakJumps_.pop_back();
    continueTargets_.pop_back();
}

void BytecodeGenerator::compileForStatement(const Frontend::ForStmt* stmt) {
    // var is function-scoped — only create a block scope for let/const init
    bool needsScope = false;
    if (stmt->init() && stmt->init()->type() == Frontend::NodeType::VariableDeclaration) {
        auto* varDecl = static_cast<const Frontend::VariableDecl*>(stmt->init());
        needsScope = (varDecl->kind() != Frontend::VariableDecl::Kind::Var);
    }
    
    if (needsScope) beginScope();
    
    // Initializer
    if (stmt->init()) {
        if (stmt->init()->type() == Frontend::NodeType::VariableDeclaration) {
            compileStatement(static_cast<const Frontend::Statement*>(stmt->init()));
        } else {
            compileExpression(static_cast<const Frontend::Expression*>(stmt->init()));
            emit(Opcode::OP_POP);
        }
    }
    
    size_t loopStart = currentChunk()->currentOffset();
    
    // Condition
    size_t exitJump = 0;
    if (stmt->test()) {
        compileExpression(stmt->test());
        exitJump = emitJump(Opcode::OP_JUMP_IF_FALSE);
        emit(Opcode::OP_POP);
    }
    
    breakJumps_.push_back({});
    size_t updateStart = loopStart;
    
    // If there's an update, jump over it first time
    if (stmt->update()) {
        size_t bodyJump = emitJump(Opcode::OP_JUMP);
        updateStart = currentChunk()->currentOffset();
        compileExpression(stmt->update());
        emit(Opcode::OP_POP);
        emitLoop(loopStart);
        patchJump(bodyJump);
    }
    
    continueTargets_.push_back(updateStart);
    
    // Body
    compileStatement(stmt->body());
    
    // Loop back
    emitLoop(updateStart);
    
    if (stmt->test()) {
        patchJump(exitJump);
        emit(Opcode::OP_POP);
    }
    
    for (size_t breakJump : breakJumps_.back()) {
        patchJump(breakJump);
    }
    breakJumps_.pop_back();
    continueTargets_.pop_back();
    
    if (needsScope) endScope();
}

void BytecodeGenerator::compileReturnStatement(const Frontend::ReturnStmt* stmt) {
    if (stmt->argument()) {
        compileExpression(stmt->argument());
    } else {
        emitConstant(Runtime::Value::undefined());
    }
    emit(Opcode::OP_RETURN);
}

void BytecodeGenerator::compileBreakStatement(const Frontend::BreakStmt*) {
    if (breakJumps_.empty()) {
        error("'break' outside of loop");
        return;
    }
    breakJumps_.back().push_back(emitJump(Opcode::OP_JUMP));
}

void BytecodeGenerator::compileContinueStatement(const Frontend::ContinueStmt*) {
    if (continueTargets_.empty()) {
        error("'continue' outside of loop");
        return;
    }
    emitLoop(continueTargets_.back());
}

void BytecodeGenerator::compileThrowStatement(const Frontend::ThrowStmt* stmt) {
    compileExpression(stmt->argument());
    emit(Opcode::OP_THROW);
}

void BytecodeGenerator::compileTryStatement(const Frontend::TryStmt* stmt) {
    // Emit OP_TRY_BEGIN + placeholder offset (uses same pattern as emitJump)
    emit(Opcode::OP_TRY_BEGIN);
    emit(static_cast<uint8_t>(0xFF));
    emit(static_cast<uint8_t>(0xFF));
    size_t catchOffsetSlot = currentChunk()->currentOffset() - 2;
    
    // Compile try block
    compileBlockStatement(stmt->block());
    
    // End of try block (no exception) - pop handler and skip catch
    emit(Opcode::OP_TRY_END);
    size_t skipCatchJump = emitJump(Opcode::OP_JUMP);
    
    // Patch the catch offset to point here
    patchJump(catchOffsetSlot);
    
    // Catch block
    if (stmt->handler()) {
        emit(Opcode::OP_CATCH);
        if (!stmt->handler()->param.empty()) {
            beginScope();
            declareVariable(stmt->handler()->param, false);
            // The exception value is on the stack from OP_CATCH
            defineVariable(stmt->handler()->param);
        }
        compileBlockStatement(stmt->handler()->body.get());
        if (!stmt->handler()->param.empty()) {
            endScope();
        }
    }
    
    // Patch jump to skip catch block
    patchJump(skipCatchJump);
    
    // Finally block (if present)
    if (stmt->finalizer()) {
        emit(Opcode::OP_FINALLY);
        compileBlockStatement(stmt->finalizer());
    }
}

void BytecodeGenerator::compileSwitchStatement(const Frontend::SwitchStmt* stmt) {
    // Compile discriminant once
    compileExpression(stmt->discriminant());
    
    std::vector<size_t> caseEndJumps;
    std::vector<size_t> caseBodyStarts;
    size_t defaultJump = 0;
    bool hasDefault = false;
    
    // Phase 1: Generate case tests and jumps
    for (const auto& switchCase : stmt->cases()) {
        if (switchCase.isDefault()) {
            // Default case - we'll jump here if no case matches
            hasDefault = true;
            caseBodyStarts.push_back(SIZE_MAX); // Placeholder for default
        } else {
            // Duplicate discriminant for comparison
            emit(Opcode::OP_DUP);
            // Compile case test expression
            compileExpression(switchCase.test.get());
            // Compare with strict equality
            emit(Opcode::OP_STRICT_EQUAL);
            // Jump to case body if match
            size_t jumpToBody = emitJump(Opcode::OP_JUMP_IF_FALSE);
            emit(Opcode::OP_POP);  // Pop comparison result
            // Record that we need to jump to this case's body
            caseBodyStarts.push_back(currentChunk()->currentOffset());
            // Jump over case body for now (we're still testing)
            size_t skipBody = emitJump(Opcode::OP_JUMP);
            patchJump(jumpToBody);
            emit(Opcode::OP_POP);  // Pop comparison result (false path)
            caseEndJumps.push_back(skipBody);
        }
    }
    
    // Jump to default or end if no case matched
    if (hasDefault) {
        defaultJump = emitJump(Opcode::OP_JUMP);
    } else {
        defaultJump = emitJump(Opcode::OP_JUMP);
    }
    
    // Pop discriminant before cases (it's been used)
    emit(Opcode::OP_POP);
    
    // Phase 2: Generate case bodies
    breakJumps_.push_back({});
    
    size_t caseIndex = 0;
    for (const auto& switchCase : stmt->cases()) {
        // Patch jumps that should come to this case body
        if (!caseEndJumps.empty() && caseIndex < caseEndJumps.size()) {
            patchJump(caseEndJumps[caseIndex]);
        }
        
        if (switchCase.isDefault()) {
            patchJump(defaultJump);
        }
        
        // Pop discriminant at start of each case
        if (caseIndex == 0) {
            emit(Opcode::OP_POP);
        }
        
        // Compile case body statements (fall-through is natural)
        for (const auto& bodyStmt : switchCase.consequent) {
            compileStatement(bodyStmt.get());
        }
        
        caseIndex++;
    }
    
    // If no default, patch the default jump to skip all cases
    if (!hasDefault) {
        patchJump(defaultJump);
        emit(Opcode::OP_POP);  // Pop discriminant
    }
    
    // Patch all break jumps
    for (size_t breakJump : breakJumps_.back()) {
        patchJump(breakJump);
    }
    breakJumps_.pop_back();
}

// Expression compilation
void BytecodeGenerator::compileExpression(const Frontend::Expression* expr) {
    if (!expr) {
        emit(Opcode::OP_NIL);
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
            compileSpreadElement(static_cast<const Frontend::SpreadExpr*>(expr));
            break;
        case Frontend::NodeType::RestElement:
            compileRestElement(static_cast<const Frontend::RestElem*>(expr));
            break;
        case Frontend::NodeType::ObjectPattern:
        case Frontend::NodeType::ArrayPattern:
        case Frontend::NodeType::AssignmentPattern:
            // Pattern nodes are handled by emitBindingPattern, not compileExpression
            error(expr, "Pattern node used in expression context");
            break;
        default:
            error("Unknown expression type");
            break;
    }
}

void BytecodeGenerator::compileLiteral(const Frontend::LiteralExpr* expr) {
    const auto& value = expr->value();
    
    if (std::holds_alternative<double>(value)) {
        emitConstant(Runtime::Value::number(std::get<double>(value)));
    } else if (std::holds_alternative<std::string>(value)) {
        emitConstant(Runtime::Value::string(new Runtime::String(std::get<std::string>(value))));
    } else if (std::holds_alternative<bool>(value)) {
        emit(std::get<bool>(value) ? Opcode::OP_TRUE : Opcode::OP_FALSE);
    } else if (std::holds_alternative<std::nullptr_t>(value)) {
        emit(Opcode::OP_NIL);
    } else {
        emit(Opcode::OP_NIL);
    }
}

void BytecodeGenerator::compileIdentifier(const Frontend::IdentifierExpr* expr) {
    const std::string& name = expr->name();
    
    // Check for local
    int local = resolveLocal(name);
    if (local != -1) {
        emit(Opcode::OP_GET_LOCAL);
        emit(static_cast<uint8_t>(local));
        return;
    }
    
    // Check for upvalue
    int upvalue = resolveUpvalue(name);
    if (upvalue != -1) {
        emit(Opcode::OP_GET_UPVALUE);
        emit(static_cast<uint8_t>(upvalue));
        return;
    }
    
    // Global
    size_t constant = makeConstant(Runtime::Value::string(new Runtime::String(name)));
    emit(Opcode::OP_GET_GLOBAL);
    emit(static_cast<uint8_t>(constant));
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
            error("Unknown binary operator");
            break;
    }
}

void BytecodeGenerator::compileUnaryExpression(const Frontend::UnaryExpr* expr) {
    compileExpression(expr->argument());
    
    switch (expr->op()) {
        case Frontend::TokenType::Minus: emit(Opcode::OP_NEGATE); break;
        case Frontend::TokenType::Not: emit(Opcode::OP_NOT); break;
        case Frontend::TokenType::Tilde: emit(Opcode::OP_BITWISE_NOT); break;
        case Frontend::TokenType::Typeof: emit(Opcode::OP_TYPEOF); break;
        case Frontend::TokenType::Void:
            emit(Opcode::OP_POP);
            emitConstant(Runtime::Value::undefined());
            break;
        default:
            error("Unknown unary operator");
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
        emit(Opcode::OP_DUP);
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

void BytecodeGenerator::compileAssignmentExpression(const Frontend::AssignmentExpr* expr) {
    // Member assignment: obj.prop = value or obj[expr] = value
    // Must compile LHS object BEFORE RHS value so stack order matches
    // OP_SET_PROPERTY which pops: value, object
    if (expr->left()->type() == Frontend::NodeType::MemberExpression) {
        const auto* member = static_cast<const Frontend::MemberExpr*>(expr->left());
        
        // Push object reference first
        compileExpression(member->object());
        
        // For compound assignment, duplicate obj ref and get current value
        if (expr->op() != Frontend::TokenType::Assign) {
            emit(Opcode::OP_DUP);
            if (member->isComputed()) {
                compileExpression(member->property());
                emit(Opcode::OP_GET_ELEMENT);
            } else {
                const auto* prop = static_cast<const Frontend::IdentifierExpr*>(member->property());
                size_t constant = makeConstant(Runtime::Value::string(new Runtime::String(prop->name())));
                emit(Opcode::OP_GET_PROPERTY);
                emit(static_cast<uint8_t>(constant));
            }
        }
        
        // Push RHS value
        compileExpression(expr->right());
        
        // Apply compound operator
        switch (expr->op()) {
            case Frontend::TokenType::PlusAssign: emit(Opcode::OP_ADD); break;
            case Frontend::TokenType::MinusAssign: emit(Opcode::OP_SUBTRACT); break;
            case Frontend::TokenType::StarAssign: emit(Opcode::OP_MULTIPLY); break;
            case Frontend::TokenType::SlashAssign: emit(Opcode::OP_DIVIDE); break;
            case Frontend::TokenType::PercentAssign: emit(Opcode::OP_MODULO); break;
            case Frontend::TokenType::StarStarAssign: emit(Opcode::OP_POWER); break;
            case Frontend::TokenType::AmpersandAssign: emit(Opcode::OP_BITWISE_AND); break;
            case Frontend::TokenType::PipeAssign: emit(Opcode::OP_BITWISE_OR); break;
            case Frontend::TokenType::CaretAssign: emit(Opcode::OP_BITWISE_XOR); break;
            case Frontend::TokenType::LeftShiftAssign: emit(Opcode::OP_LEFT_SHIFT); break;
            case Frontend::TokenType::RightShiftAssign: emit(Opcode::OP_RIGHT_SHIFT); break;
            case Frontend::TokenType::UnsignedRightShiftAssign: emit(Opcode::OP_UNSIGNED_RIGHT_SHIFT); break;
            case Frontend::TokenType::Assign: break;
            default: break;
        }
        
        // Stack is now [obj, value] — emit property store
        if (member->isComputed()) {
            compileExpression(member->property());
            emit(Opcode::OP_SET_ELEMENT);
        } else {
            const auto* prop = static_cast<const Frontend::IdentifierExpr*>(member->property());
            size_t constant = makeConstant(Runtime::Value::string(new Runtime::String(prop->name())));
            emit(Opcode::OP_SET_PROPERTY);
            emit(static_cast<uint8_t>(constant));
        }
        return;
    }
    
    // Simple or compound assignment to identifier
    if (expr->op() != Frontend::TokenType::Assign) {
        compileExpression(expr->left());
    }
    
    compileExpression(expr->right());
    
    // Apply operator for compound assignment
    switch (expr->op()) {
        case Frontend::TokenType::PlusAssign: emit(Opcode::OP_ADD); break;
        case Frontend::TokenType::MinusAssign: emit(Opcode::OP_SUBTRACT); break;
        case Frontend::TokenType::StarAssign: emit(Opcode::OP_MULTIPLY); break;
        case Frontend::TokenType::SlashAssign: emit(Opcode::OP_DIVIDE); break;
        case Frontend::TokenType::PercentAssign: emit(Opcode::OP_MODULO); break;
        case Frontend::TokenType::StarStarAssign: emit(Opcode::OP_POWER); break;
        case Frontend::TokenType::AmpersandAssign: emit(Opcode::OP_BITWISE_AND); break;
        case Frontend::TokenType::PipeAssign: emit(Opcode::OP_BITWISE_OR); break;
        case Frontend::TokenType::CaretAssign: emit(Opcode::OP_BITWISE_XOR); break;
        case Frontend::TokenType::LeftShiftAssign: emit(Opcode::OP_LEFT_SHIFT); break;
        case Frontend::TokenType::RightShiftAssign: emit(Opcode::OP_RIGHT_SHIFT); break;
        case Frontend::TokenType::UnsignedRightShiftAssign: emit(Opcode::OP_UNSIGNED_RIGHT_SHIFT); break;
        case Frontend::TokenType::AndAssign: emit(Opcode::OP_AND); break;
        case Frontend::TokenType::OrAssign: emit(Opcode::OP_OR); break;
        case Frontend::TokenType::QuestionQuestionAssign: emit(Opcode::OP_NULLISH); break;
        case Frontend::TokenType::Assign: break;
        default: break;
    }
    
    // Store to identifier
    if (expr->left()->type() == Frontend::NodeType::Identifier) {
        const auto* id = static_cast<const Frontend::IdentifierExpr*>(expr->left());
        const std::string& identName = id->name();
        
        int local = resolveLocal(identName);
        if (local != -1) {
            if (current_->locals[local].isConst) {
                error("Cannot assign to const variable '" + identName + "'");
                return;
            }
            
            emit(Opcode::OP_SET_LOCAL);
            emit(static_cast<uint8_t>(local));
        } else {
            size_t constant = makeConstant(Runtime::Value::string(new Runtime::String(identName)));
            emit(Opcode::OP_SET_GLOBAL);
            emit(static_cast<uint8_t>(constant));
        }
    }
}

void BytecodeGenerator::compileCallExpression(const Frontend::CallExpr* expr) {
    const Frontend::Expression* callee = expr->callee();
    
    // Check if this is a method call (callee is a member expression)
    if (callee->type() == Frontend::NodeType::MemberExpression) {
        const auto* memberExpr = static_cast<const Frontend::MemberExpr*>(callee);
        
        // Compile the receiver object
        compileExpression(memberExpr->object());
        
        // Compile arguments
        for (const auto& arg : expr->arguments()) {
            compileExpression(arg.get());
        }
        
        // Emit OP_CALL_METHOD with method name and arg count
        if (!memberExpr->isComputed()) {
            const auto* prop = static_cast<const Frontend::IdentifierExpr*>(memberExpr->property());
            size_t nameConstant = makeConstant(Runtime::Value::string(new Runtime::String(prop->name())));
            emit(Opcode::OP_CALL_METHOD);
            emit(static_cast<uint8_t>(nameConstant));
            emit(static_cast<uint8_t>(expr->arguments().size()));
        } else {
            // Computed property - fall back to regular call
            compileExpression(memberExpr->property());
            emit(Opcode::OP_GET_ELEMENT);
            emit(Opcode::OP_CALL);
            emit(static_cast<uint8_t>(expr->arguments().size()));
        }
    } else {
        // Regular function call
        compileExpression(callee);
        
        for (const auto& arg : expr->arguments()) {
            compileExpression(arg.get());
        }
        
        emit(Opcode::OP_CALL);
        emit(static_cast<uint8_t>(expr->arguments().size()));
    }
}

void BytecodeGenerator::compileMemberExpression(const Frontend::MemberExpr* expr) {
    compileExpression(expr->object());
    
    if (expr->isOptional()) {
        // Optional chaining: obj?.prop or obj?.[expr]
        // If object is null/undefined, short-circuit to undefined
        emit(Opcode::OP_DUP);
        size_t nilJump = emitJump(Opcode::OP_JUMP_IF_NIL);
        
        // Not nil — do the property access
        if (expr->isComputed()) {
            compileExpression(expr->property());
            emit(Opcode::OP_GET_ELEMENT);
        } else {
            const auto* prop = static_cast<const Frontend::IdentifierExpr*>(expr->property());
            size_t constant = makeConstant(Runtime::Value::string(new Runtime::String(prop->name())));
            emit(Opcode::OP_GET_PROPERTY);
            emit(static_cast<uint8_t>(constant));
        }
        size_t doneJump = emitJump(Opcode::OP_JUMP);
        
        // Nil path — pop the nil object, push undefined
        patchJump(nilJump);
        emit(Opcode::OP_POP);
        emit(Opcode::OP_NIL);
        
        patchJump(doneJump);
    } else {
        if (expr->isComputed()) {
            compileExpression(expr->property());
            emit(Opcode::OP_GET_ELEMENT);
        } else {
            const auto* prop = static_cast<const Frontend::IdentifierExpr*>(expr->property());
            size_t constant = makeConstant(Runtime::Value::string(new Runtime::String(prop->name())));
            emit(Opcode::OP_GET_PROPERTY);
            emit(static_cast<uint8_t>(constant));
        }
    }
}

void BytecodeGenerator::compileNewExpression(const Frontend::NewExpr* expr) {
    compileExpression(expr->callee());
    
    for (const auto& arg : expr->arguments()) {
        compileExpression(arg.get());
    }
    
    emit(Opcode::OP_NEW);
    emit(static_cast<uint8_t>(expr->arguments().size()));
}

void BytecodeGenerator::compileArrayExpression(const Frontend::ArrayExpr* expr) {
    // Compile each element and push onto stack
    for (const auto& element : expr->elements()) {
        if (element) {
            compileExpression(element.get());
        } else {
            // Elision (empty slot) - push undefined
            emit(Opcode::OP_NIL);
        }
    }
    
    // Create array from all elements on stack
    emit(Opcode::OP_CREATE_ARRAY);
    emit(static_cast<uint8_t>(expr->elements().size()));
}


void BytecodeGenerator::compileObjectExpression(const Frontend::ObjectExpr* expr) {
    emit(Opcode::OP_CREATE_OBJECT);
    
    for (const auto& prop : expr->properties()) {
        if (prop.computed) {
            compileExpression(prop.key.get());
        } else if (prop.key->type() == Frontend::NodeType::Identifier) {
            const auto* id = static_cast<const Frontend::IdentifierExpr*>(prop.key.get());
            emitConstant(Runtime::Value::string(new Runtime::String(id->name())));
        }
        compileExpression(prop.value.get());
        emit(Opcode::OP_INIT_PROPERTY);
    }
}

void BytecodeGenerator::compileThisExpression(const Frontend::ThisExpr*) {
    // 'this' is stored as local slot 0 in methods and constructors
    int slot = resolveLocal("this");
    if (slot != -1) {
        emit(Opcode::OP_GET_LOCAL);
        emit(static_cast<uint8_t>(slot));
    } else {
        // Try upvalue (arrow functions capture 'this' from enclosing scope)
        int upvalue = resolveUpvalue("this");
        if (upvalue != -1) {
            emit(Opcode::OP_GET_UPVALUE);
            emit(static_cast<uint8_t>(upvalue));
        } else {
            // Global 'this' — push undefined (strict mode) or global object
            emit(Opcode::OP_NIL);
        }
    }
}

void BytecodeGenerator::compileFunctionExpression(const Frontend::FunctionExpr* expr) {
    // Create new compiler state for function
    CompilerState fnState;
    fnState.enclosing = current_;
    fnState.functionName = expr->name().empty() ? "<anonymous>" : expr->name();
    current_ = &fnState;
    
    // Reserve slot 0 for 'this' — matches OP_CALL/OP_NEW stack layout
    Local thisLocal;
    thisLocal.name = "this";
    thisLocal.depth = 0;
    thisLocal.isCaptured = false;
    thisLocal.isConst = true;
    current_->locals.push_back(thisLocal);
    
    // Add parameters as locals (starting at slot 1)
    for (const auto& param : expr->params()) {
        const auto* id = static_cast<const Frontend::IdentifierExpr*>(param.pattern.get());
        Local local;
        local.name = id->name();
        local.isConst = false;
        current_->locals.push_back(local);
    }
    
    // Emit default parameter value checks
    for (size_t i = 0; i < expr->params().size(); ++i) {
        const auto& param = expr->params()[i];
        if (param.defaultValue) {
            emit(Opcode::OP_GET_LOCAL);
            emit(static_cast<uint8_t>(i));
            emit(Opcode::OP_NIL);
            emit(Opcode::OP_STRICT_EQUAL);
            
            size_t skipDefault = emitJump(Opcode::OP_JUMP_IF_FALSE);
            emit(Opcode::OP_POP); // Pop the 'true' boolean
            
            compileExpression(param.defaultValue.get());
            emit(Opcode::OP_SET_LOCAL);
            emit(static_cast<uint8_t>(i));
            emit(Opcode::OP_POP); // Pop the set value
            
            size_t doneJump = emitJump(Opcode::OP_JUMP);
            
            patchJump(skipDefault);
            emit(Opcode::OP_POP); // Pop the 'false' boolean
            
            patchJump(doneJump);
        }
    }
    
    // Compile function body
    for (const auto& stmt : expr->body()->body()) {
        compileStatement(stmt.get());
    }
    
    // Default return undefined if no explicit return
    emitConstant(Runtime::Value::undefined());
    emit(Opcode::OP_RETURN);
    
    // Create function chunk
    auto functionChunk = std::make_unique<BytecodeChunk>(std::move(fnState.chunk));
    current_ = fnState.enclosing;
    
    // Create function object with bytecode
    auto* func = new Runtime::Function(expr, nullptr);
    func->setBytecodeChunk(functionChunk.get());

    // Debug metadata
    std::vector<std::string> paramNames;
    std::vector<std::string> localNames;
    for (const auto& param : expr->params()) {
        const auto* id = static_cast<const Frontend::IdentifierExpr*>(param.pattern.get());
        paramNames.push_back(id->name());
    }
    for (const auto& local : fnState.locals) {
        localNames.push_back(local.name);
    }
    func->setParamNames(std::move(paramNames));
    func->setLocalNames(std::move(localNames));
    
    // Store chunk (transfer ownership)
    static std::vector<std::unique_ptr<BytecodeChunk>> compiledFunctionChunks;
    compiledFunctionChunks.push_back(std::move(functionChunk));
    
    // Emit closure
    size_t funcIndex = makeConstant(Runtime::Value::object(func));
    emit(Opcode::OP_CLOSURE);
    emit(static_cast<uint8_t>(funcIndex));
    
    // Emit upvalue descriptors (captured variables from enclosing scope)
    emit(static_cast<uint8_t>(fnState.upvalues.size()));
    for (const auto& upval : fnState.upvalues) {
        emit(upval.isLocal ? uint8_t(1) : uint8_t(0));
        emit(static_cast<uint8_t>(upval.index));
    }
}

void BytecodeGenerator::compileArrowFunction(const Frontend::ArrowFunctionExpr* expr) {
    // Arrow functions are like regular functions but:
    // 1. They capture 'this' lexically
    // 2. They have implicit return for expression bodies
    
    // Create new compiler state for function
    CompilerState fnState;
    fnState.enclosing = current_;
    fnState.functionName = "<arrow>";
    current_ = &fnState;
    
    // Add parameters as locals
    for (const auto& param : expr->params()) {
        const auto* id = static_cast<const Frontend::IdentifierExpr*>(param.pattern.get());
        Local local;
        local.name = id->name();
        local.isConst = false;
        current_->locals.push_back(local);
    }
    
    // Emit default parameter value checks
    for (size_t i = 0; i < expr->params().size(); ++i) {
        const auto& param = expr->params()[i];
        if (param.defaultValue) {
            emit(Opcode::OP_GET_LOCAL);
            emit(static_cast<uint8_t>(i));
            emit(Opcode::OP_NIL);
            emit(Opcode::OP_STRICT_EQUAL);
            
            size_t skipDefault = emitJump(Opcode::OP_JUMP_IF_FALSE);
            emit(Opcode::OP_POP); // Pop the 'true' boolean
            
            compileExpression(param.defaultValue.get());
            emit(Opcode::OP_SET_LOCAL);
            emit(static_cast<uint8_t>(i));
            emit(Opcode::OP_POP); // Pop the set value
            
            size_t doneJump = emitJump(Opcode::OP_JUMP);
            
            patchJump(skipDefault);
            emit(Opcode::OP_POP); // Pop the 'false' boolean
            
            patchJump(doneJump);
        }
    }
    
    // Compile body
    if (expr->hasExpressionBody()) {
        // Expression body: implicit return
        compileExpression(expr->expressionBody());
        emit(Opcode::OP_RETURN);
    } else {
        // Block body: explicit returns
        for (const auto& stmt : expr->blockBody()->body()) {
            compileStatement(stmt.get());
        }
        // Default return undefined
        emitConstant(Runtime::Value::undefined());
        emit(Opcode::OP_RETURN);
    }
    
    // Create function chunk
    auto functionChunk = std::make_unique<BytecodeChunk>(std::move(fnState.chunk));
    current_ = fnState.enclosing;
    
    // Create arrow function object
    auto* arrowFn = new Runtime::Function(expr, nullptr);
    arrowFn->setBytecodeChunk(functionChunk.get());

    // Debug metadata
    std::vector<std::string> paramNames;
    std::vector<std::string> localNames;
    for (const auto& param : expr->params()) {
        const auto* id = static_cast<const Frontend::IdentifierExpr*>(param.pattern.get());
        paramNames.push_back(id->name());
    }
    for (const auto& local : fnState.locals) {
        localNames.push_back(local.name);
    }
    arrowFn->setParamNames(std::move(paramNames));
    arrowFn->setLocalNames(std::move(localNames));
    
    // Store chunk (transferred ownership)
    static std::vector<std::unique_ptr<BytecodeChunk>> compiledChunks;
    compiledChunks.push_back(std::move(functionChunk));
    
    // Emit closure with captured 'this'
    size_t funcIndex = makeConstant(Runtime::Value::object(arrowFn));
    emit(Opcode::OP_CLOSURE);
    emit(static_cast<uint8_t>(funcIndex));
    
    // Emit upvalue descriptors
    // Arrow functions capture upvalues from enclosing scope including 'this'
    emit(static_cast<uint8_t>(fnState.upvalues.size()));
    for (const auto& upval : fnState.upvalues) {
        emit(upval.isLocal ? uint8_t(1) : uint8_t(0));
        emit(static_cast<uint8_t>(upval.index));
    }
}

void BytecodeGenerator::compileUpdateExpression(const Frontend::UpdateExpr* expr) {
    const Frontend::Expression* arg = expr->argument();
    bool isPrefix = expr->isPrefix();
    bool isIncrement = (expr->op() == Frontend::TokenType::PlusPlus);

    if (arg->type() == Frontend::NodeType::Identifier) {
        const auto* id = static_cast<const Frontend::IdentifierExpr*>(arg);
        const std::string& name = id->name();
        int local = resolveLocal(name);

        if (local != -1) {
            // Local variable
            emit(Opcode::OP_GET_LOCAL);
            emit(static_cast<uint8_t>(local));

            if (!isPrefix) {
                // Postfix: duplicate original value to return later
                emit(Opcode::OP_DUP);
            }

            // Add or subtract 1
            emitConstant(Runtime::Value::number(1.0));
            emit(isIncrement ? Opcode::OP_ADD : Opcode::OP_SUBTRACT);

            // Store updated value
            emit(Opcode::OP_SET_LOCAL);
            emit(static_cast<uint8_t>(local));

            if (isPrefix) {
                // Prefix: result is the updated value (already on stack from SET_LOCAL)
                emit(Opcode::OP_GET_LOCAL);
                emit(static_cast<uint8_t>(local));
            }
            // Postfix: the duplicated original is already below on the stack
        } else {
            // Global variable
            size_t constant = makeConstant(Runtime::Value::string(new Runtime::String(name)));

            emit(Opcode::OP_GET_GLOBAL);
            emit(static_cast<uint8_t>(constant));

            if (!isPrefix) {
                emit(Opcode::OP_DUP);
            }

            emitConstant(Runtime::Value::number(1.0));
            emit(isIncrement ? Opcode::OP_ADD : Opcode::OP_SUBTRACT);

            emit(Opcode::OP_SET_GLOBAL);
            emit(static_cast<uint8_t>(constant));

            if (isPrefix) {
                emit(Opcode::OP_GET_GLOBAL);
                emit(static_cast<uint8_t>(constant));
            }
        }
    } else if (arg->type() == Frontend::NodeType::MemberExpression) {
        const auto* member = static_cast<const Frontend::MemberExpr*>(arg);

        // Compile object
        compileExpression(member->object());
        emit(Opcode::OP_DUP); // Keep object for set

        // Get current value
        if (member->isComputed()) {
            compileExpression(member->property());
            emit(Opcode::OP_GET_ELEMENT);
        } else {
            const auto* prop = static_cast<const Frontend::IdentifierExpr*>(member->property());
            size_t nameConst = makeConstant(Runtime::Value::string(new Runtime::String(prop->name())));
            emit(Opcode::OP_GET_PROPERTY);
            emit(static_cast<uint8_t>(nameConst));
        }

        if (!isPrefix) {
            emit(Opcode::OP_DUP); // Save original for postfix
        }

        // Increment/decrement
        emitConstant(Runtime::Value::number(1.0));
        emit(isIncrement ? Opcode::OP_ADD : Opcode::OP_SUBTRACT);

        // Store back
        if (member->isComputed()) {
            compileExpression(member->property());
            emit(Opcode::OP_SET_ELEMENT);
        } else {
            const auto* prop = static_cast<const Frontend::IdentifierExpr*>(member->property());
            size_t nameConst = makeConstant(Runtime::Value::string(new Runtime::String(prop->name())));
            emit(Opcode::OP_SET_PROPERTY);
            emit(static_cast<uint8_t>(nameConst));
        }
    } else {
        error("Invalid update expression target");
    }
}

void BytecodeGenerator::compileImportDeclaration(const Frontend::ImportDecl* decl) {
    // Emit OP_IMPORT with module path constant
    size_t pathConstant = makeConstant(
        Runtime::Value::string(new Runtime::String(decl->source())));
    emit(Opcode::OP_IMPORT);
    emit(static_cast<uint8_t>(pathConstant));
    
    // The module exports object is now on the stack
    // For each specifier, bind the imported name to a local variable
    for (const auto& spec : decl->specifiers()) {
        // Duplicate the exports object
        emit(Opcode::OP_DUP);
        
        // Get the imported property
        size_t nameConstant = makeConstant(
            Runtime::Value::string(new Runtime::String(spec.imported)));
        emit(Opcode::OP_GET_PROPERTY);
        emit(static_cast<uint8_t>(nameConstant));
        
        // Define as local variable
        declareVariable(spec.local, false);
        defineVariable(spec.local);
    }
    
    // Pop the exports object when done
    emit(Opcode::OP_POP);
}

void BytecodeGenerator::compileExportDeclaration(const Frontend::ExportDecl* decl) {
    if (decl->hasDeclaration()) {
        // Compile the declaration (function, variable, etc.)
        compileStatement(decl->declaration());
        
        // If it's a named export, register the exported name
        // Emit OP_EXPORT for the declared name
        if (auto* funcDecl = dynamic_cast<const Frontend::FunctionDecl*>(decl->declaration())) {
            int slot = resolveLocal(funcDecl->name());
            if (slot != -1) {
                emit(Opcode::OP_GET_LOCAL);
                emit(static_cast<uint8_t>(slot));
            } else {
                size_t nameConst = makeConstant(
                    Runtime::Value::string(new Runtime::String(funcDecl->name())));
                emit(Opcode::OP_GET_GLOBAL);
                emit(static_cast<uint8_t>(nameConst));
            }
            size_t exportConst = makeConstant(
                Runtime::Value::string(new Runtime::String(funcDecl->name())));
            emit(Opcode::OP_EXPORT);
            emit(static_cast<uint8_t>(exportConst));
        }
    }
    
    // Named exports: export { foo, bar }
    for (const auto& spec : decl->specifiers()) {
        // Get the local variable value
        int slot = resolveLocal(spec.local);
        if (slot != -1) {
            emit(Opcode::OP_GET_LOCAL);
            emit(static_cast<uint8_t>(slot));
        } else {
            size_t constant = makeConstant(
                Runtime::Value::string(new Runtime::String(spec.local)));
            emit(Opcode::OP_GET_GLOBAL);
            emit(static_cast<uint8_t>(constant));
        }
        
        // Register as export (requires module context)
        size_t exportNameConstant = makeConstant(
            Runtime::Value::string(new Runtime::String(spec.exported)));
        emit(Opcode::OP_EXPORT);
        emit(static_cast<uint8_t>(exportNameConstant));
    }
}

void BytecodeGenerator::compileClassDeclaration(const Frontend::ClassDecl* decl) {
    // ES6 class compilation: transpile to ES5 constructor + prototype
    // class Foo extends Bar { constructor() {} method() {} }
    // becomes:
    // function Foo() { Bar.call(this); /* constructor body */ }
    // Foo.prototype = Object.create(Bar.prototype);
    // Foo.prototype.constructor = Foo;
    // Foo.prototype.method = function() {};
    
    const std::string& className = decl->name();
    
    // Declare the class name as a variable
    declareVariable(className, false);
    
    // Compile constructor as a function
    CompilerState classState;
    classState.enclosing = current_;
    classState.functionName = className;
    classState.isMethod = false;
    current_ = &classState;
    
    // Add 'this' as first local
    Local thisLocal;
    thisLocal.name = "this";
    thisLocal.depth = 0;
    thisLocal.isCaptured = false;
    thisLocal.isConst = true;
    current_->locals.push_back(thisLocal);
    
    // Compile constructor body
    const Frontend::FunctionExpr* ctor = decl->constructor();
    if (ctor) {
        // Add constructor parameters
        for (const auto& param : ctor->params()) {
            const auto* id = static_cast<const Frontend::IdentifierExpr*>(param.pattern.get());
            Local local;
            local.name = id->name();
            local.depth = 0;
            local.isCaptured = false;
            local.isConst = false;
            current_->locals.push_back(local);
        }
        
        // If extends, call super constructor
        if (decl->superClass()) {
            // Load 'this' and superclass, then call super
            emit(Opcode::OP_GET_LOCAL);
            emit(static_cast<uint8_t>(0));  // 'this'
            compileExpression(decl->superClass());
            emit(Opcode::OP_SUPER_CALL);
            emit(static_cast<uint8_t>(0));  // arg count (default super())
            emit(Opcode::OP_POP);  // Discard super return
        }
        
        // Compile constructor body
        if (ctor->body()) {
            for (const auto& stmt : ctor->body()->body()) {
                compileStatement(stmt.get());
            }
        }
    }
    
    // Default return 'this'
    emit(Opcode::OP_GET_LOCAL);
    emit(static_cast<uint8_t>(0));  // 'this' is at slot 0
    emit(Opcode::OP_RETURN);
    
    // Create constructor function chunk
    auto constructorChunk = std::make_unique<BytecodeChunk>(std::move(classState.chunk));
    current_ = classState.enclosing;
    
    // Create constructor function object
    auto* ctorFunc = new Runtime::Function(ctor, nullptr);
    ctorFunc->setBytecodeChunk(constructorChunk.get());

    // Debug metadata
    std::vector<std::string> paramNames;
    std::vector<std::string> localNames;
    for (const auto& param : ctor->params()) {
        const auto* id = static_cast<const Frontend::IdentifierExpr*>(param.pattern.get());
        paramNames.push_back(id->name());
    }
    for (const auto& local : classState.locals) {
        localNames.push_back(local.name);
    }
    ctorFunc->setParamNames(std::move(paramNames));
    ctorFunc->setLocalNames(std::move(localNames));
    
    // Store chunk
    static std::vector<std::unique_ptr<BytecodeChunk>> compiledChunks;
    compiledChunks.push_back(std::move(constructorChunk));
    
    // Emit closure for constructor
    size_t ctorIndex = makeConstant(Runtime::Value::object(ctorFunc));
    emit(Opcode::OP_CLOSURE);
    emit(static_cast<uint8_t>(ctorIndex));
    
    // Emit upvalue descriptors for constructor
    emit(static_cast<uint8_t>(classState.upvalues.size()));
    for (const auto& upval : classState.upvalues) {
        emit(upval.isLocal ? uint8_t(1) : uint8_t(0));
        emit(static_cast<uint8_t>(upval.index));
    }
    
    // If extends, set up prototype chain
    if (decl->superClass()) {
        // Duplicate constructor
        emit(Opcode::OP_DUP);
        
        // Get superclass
        compileExpression(decl->superClass());
        
        // Set up inheritance (Object.create pattern)
        emit(Opcode::OP_INHERIT);
    }
    
    // Compile methods onto prototype
    for (const auto& method : decl->methods()) {
        // Duplicate constructor (to get its prototype)
        emit(Opcode::OP_DUP);
        
        // Compile method function
        CompilerState methodState;
        methodState.enclosing = current_;
        methodState.functionName = method.name;
        methodState.isMethod = true;
        current_ = &methodState;
        
        // Add 'this' as first local for methods
        Local thisParam;
        thisParam.name = "this";
        thisParam.depth = 0;
        thisParam.isCaptured = false;
        thisParam.isConst = true;
        current_->locals.push_back(thisParam);
        
        // Add method parameters
        for (const auto& param : method.function->params()) {
            const auto* id = static_cast<const Frontend::IdentifierExpr*>(param.pattern.get());
            Local local;
            local.name = id->name();
            local.depth = 0;
            local.isCaptured = false;
            local.isConst = false;
            current_->locals.push_back(local);
        }
        
        // Compile method body
        if (method.function->body()) {
            for (const auto& stmt : method.function->body()->body()) {
                compileStatement(stmt.get());
            }
        }
        
        // Default return undefined
        emitConstant(Runtime::Value::undefined());
        emit(Opcode::OP_RETURN);
        
        auto methodChunk = std::make_unique<BytecodeChunk>(std::move(methodState.chunk));
        current_ = methodState.enclosing;
        
        auto* methodFunc = new Runtime::Function(method.function.get(), nullptr);
        methodFunc->setBytecodeChunk(methodChunk.get());

        // Debug metadata
        std::vector<std::string> methodParamNames;
        std::vector<std::string> methodLocalNames;
        for (const auto& param : method.function->params()) {
            const auto* id = static_cast<const Frontend::IdentifierExpr*>(param.pattern.get());
            methodParamNames.push_back(id->name());
        }
        for (const auto& local : methodState.locals) {
            methodLocalNames.push_back(local.name);
        }
        methodFunc->setParamNames(std::move(methodParamNames));
        methodFunc->setLocalNames(std::move(methodLocalNames));
        compiledChunks.push_back(std::move(methodChunk));
        
        size_t methodIndex = makeConstant(Runtime::Value::object(methodFunc));
        emit(Opcode::OP_CLOSURE);
        emit(static_cast<uint8_t>(methodIndex));
        
        // Emit upvalue descriptors for method
        emit(static_cast<uint8_t>(methodState.upvalues.size()));
        for (const auto& upval : methodState.upvalues) {
            emit(upval.isLocal ? uint8_t(1) : uint8_t(0));
            emit(static_cast<uint8_t>(upval.index));
        }
        
        // Add method name constant
        size_t nameConstant = makeConstant(
            Runtime::Value::string(new Runtime::String(method.name)));
        
        if (method.isStatic) {
            // Static method: Foo.methodName = function
            emit(Opcode::OP_DEFINE_STATIC);
        } else if (method.isGetter) {
            // Getter: Object.defineProperty pattern
            emit(Opcode::OP_DEFINE_GETTER);
        } else if (method.isSetter) {
            // Setter: Object.defineProperty pattern
            emit(Opcode::OP_DEFINE_SETTER);
        } else {
            // Regular method: Foo.prototype.methodName = function
            emit(Opcode::OP_DEFINE_METHOD);
        }
        emit(static_cast<uint8_t>(nameConstant));
    }
    
    // Define the class name
    defineVariable(className);
}

void BytecodeGenerator::compileForOfStatement(const Frontend::ForOfStmt* stmt) {
    // for (let x of iterable) { body }
    // becomes:
    // let iterator = iterable[Symbol.iterator]();
    // let result;
    // while (!(result = iterator.next()).done) {
    //     let x = result.value;
    //     body
    // }
    
    beginScope();
    
    // Compile iterable and get iterator
    compileExpression(stmt->right());
    emit(Opcode::OP_GET_ITERATOR);  // Get [Symbol.iterator]() result
    
    // Store iterator in a hidden local
    addLocal("$iterator", false);
    
    size_t loopStart = currentChunk()->currentOffset();
    
    // Call iterator.next()
    emit(Opcode::OP_GET_LOCAL);
    emit(static_cast<uint8_t>(current_->locals.size() - 1));  // $iterator
    emit(Opcode::OP_ITERATOR_NEXT);  // Calls .next() and pushes result
    
    // Check if done
    emit(Opcode::OP_DUP);
    emit(Opcode::OP_GET_PROPERTY);
    size_t doneConstant = makeConstant(
        Runtime::Value::string(new Runtime::String("done")));
    emit(static_cast<uint8_t>(doneConstant));
    
    // If done, exit loop
    size_t exitJump = emitJump(Opcode::OP_JUMP_IF_TRUE);
    emit(Opcode::OP_POP);  // Pop 'done' value
    
    // Get value property
    emit(Opcode::OP_GET_PROPERTY);
    size_t valueConstant = makeConstant(
        Runtime::Value::string(new Runtime::String("value")));
    emit(static_cast<uint8_t>(valueConstant));
    
    // Declare loop variable
    const Frontend::ASTNode* left = stmt->left();
    if (left->type() == Frontend::NodeType::VariableDeclaration) {
        const auto* varDecl = static_cast<const Frontend::VariableDecl*>(left);
        if (!varDecl->declarators().empty()) {
            const auto* idExpr = varDecl->declarators()[0].id.get();
            bool isConst = varDecl->kind() == Frontend::VariableDecl::Kind::Const;
            
            if (idExpr->type() == Frontend::NodeType::ObjectPattern ||
                idExpr->type() == Frontend::NodeType::ArrayPattern) {
                // Destructuring: for (const {a, b} of items)
                emitBindingPattern(idExpr, isConst);
            } else {
                const auto* id = static_cast<const Frontend::IdentifierExpr*>(idExpr);
                declareVariable(id->name(), isConst);
                defineVariable(id->name());
            }
        }
    } else if (left->type() == Frontend::NodeType::Identifier) {
        const auto* id = static_cast<const Frontend::IdentifierExpr*>(left);
        int slot = resolveLocal(id->name());
        if (slot != -1) {
            emit(Opcode::OP_SET_LOCAL);
            emit(static_cast<uint8_t>(slot));
        } else {
            size_t constant = makeConstant(
                Runtime::Value::string(new Runtime::String(id->name())));
            emit(Opcode::OP_SET_GLOBAL);
            emit(static_cast<uint8_t>(constant));
        }
        emit(Opcode::OP_POP);
    }
    
    // Track loop for break/continue
    breakJumps_.push_back({});
    continueTargets_.push_back(loopStart);
    
    // Compile loop body
    compileStatement(stmt->body());
    
    // Loop back
    emitLoop(loopStart);
    
    // Patch exit jump
    patchJump(exitJump);
    emit(Opcode::OP_POP);  // Pop 'done' value
    emit(Opcode::OP_POP);  // Pop iterator result
    emit(Opcode::OP_POP);  // Pop iterator
    
    // Patch breaks
    for (size_t breakJump : breakJumps_.back()) {
        patchJump(breakJump);
    }
    breakJumps_.pop_back();
    continueTargets_.pop_back();
    
    endScope();
}

void BytecodeGenerator::compileAwaitExpression(const Frontend::AwaitExpr* expr) {
    // await promise
    // Compile the argument (should evaluate to a Promise)
    compileExpression(expr->argument());
    
    // Emit await opcode - this suspends execution until promise resolves
    emit(Opcode::OP_AWAIT);
    // Result value is left on stack
}

void BytecodeGenerator::compileYieldExpression(const Frontend::YieldExpr* expr) {
    // yield value or yield* iterable
    if (expr->argument()) {
        compileExpression(expr->argument());
    } else {
        emit(Opcode::OP_NIL);
    }
    
    // Emit yield opcode - this suspends the generator
    // For yield*, we need to iterate over the delegated iterator
    if (expr->delegate()) {
        emit(Opcode::OP_YIELD);
        emit(static_cast<uint8_t>(1));  // Delegate flag
    } else {
        emit(Opcode::OP_YIELD);
        emit(static_cast<uint8_t>(0));  // No delegation
    }
    // The value passed to next() is left on stack
}

void BytecodeGenerator::compileLabeledStatement(const Frontend::LabeledStmt* stmt) {
    // Labeled statement: label: innerStatement
    // The label enables targeted break/continue from nested loops.
    // Push a break target so `break label` resolves to the exit of this labeled block.
    breakJumps_.push_back({});

    compileStatement(stmt->body());

    // Patch any break jumps targeting this label
    for (size_t breakJump : breakJumps_.back()) {
        patchJump(breakJump);
    }
    breakJumps_.pop_back();
}

void BytecodeGenerator::compileForInStatement(const Frontend::ForInStmt* stmt) {
    // for (let key in object) { body }
    // Lowered to:
    //   let $keys = Object.keys(object);
    //   let $idx = 0;
    //   let key;  // declared once
    //   while ($idx < $keys.length) { key = $keys[$idx]; body; $idx++; }
    
    beginScope();
    
    // Compile object and get its keys
    compileExpression(stmt->right());
    emit(Opcode::OP_FOR_IN);  // Pushes array of keys
    addLocal("$keys", false);
    size_t keysSlot = current_->locals.size() - 1;
    
    // Store index counter (starts at 0)
    emitConstant(Runtime::Value::number(0));
    addLocal("$idx", false);
    size_t idxSlot = current_->locals.size() - 1;
    
    // Declare loop variable BEFORE the loop with a placeholder value
    size_t varSlot = SIZE_MAX;
    const Frontend::ASTNode* left = stmt->left();
    bool isVarDecl = (left->type() == Frontend::NodeType::VariableDeclaration);
    bool isGlobalVar = false;
    std::string loopVarName;
    
    if (isVarDecl) {
        const auto* varDecl = static_cast<const Frontend::VariableDecl*>(left);
        if (!varDecl->declarators().empty()) {
            const auto* id = static_cast<const Frontend::IdentifierExpr*>(
                varDecl->declarators()[0].id.get());
            loopVarName = id->name();
            bool isConst = varDecl->kind() == Frontend::VariableDecl::Kind::Const;
            
            if (current_->scopeDepth == 0) {
                // Global scope: define as global variable
                isGlobalVar = true;
                emitConstant(Runtime::Value::undefined());
                size_t constant = makeConstant(Runtime::Value::string(new Runtime::String(loopVarName)));
                emit(Opcode::OP_DEFINE_GLOBAL);
                emit(static_cast<uint8_t>(constant));
            } else {
                emitConstant(Runtime::Value::undefined());
                declareVariable(loopVarName, isConst);
                defineVariable(loopVarName);
                varSlot = current_->locals.size() - 1;
            }
        }
    }
    
    size_t loopStart = currentChunk()->currentOffset();
    
    // Check: $idx < $keys.length
    emit(Opcode::OP_GET_LOCAL);
    emit(static_cast<uint8_t>(idxSlot));
    emit(Opcode::OP_GET_LOCAL);
    emit(static_cast<uint8_t>(keysSlot));
    size_t lenConst = makeConstant(
        Runtime::Value::string(new Runtime::String("length")));
    emit(Opcode::OP_GET_PROPERTY);
    emit(static_cast<uint8_t>(lenConst));
    emit(Opcode::OP_LESS);
    
    size_t exitJump = emitJump(Opcode::OP_JUMP_IF_FALSE);
    emit(Opcode::OP_POP);  // Pop comparison result
    
    // Get current key: $keys[$idx]
    emit(Opcode::OP_GET_LOCAL);
    emit(static_cast<uint8_t>(keysSlot));
    emit(Opcode::OP_GET_LOCAL);
    emit(static_cast<uint8_t>(idxSlot));
    emit(Opcode::OP_GET_ELEMENT);
    
    // Assign key to loop variable
    if (isVarDecl) {
        if (isGlobalVar) {
            size_t constant = makeConstant(Runtime::Value::string(new Runtime::String(loopVarName)));
            emit(Opcode::OP_SET_GLOBAL);
            emit(static_cast<uint8_t>(constant));
        } else if (varSlot != SIZE_MAX) {
            emit(Opcode::OP_SET_LOCAL);
            emit(static_cast<uint8_t>(varSlot));
        }
        emit(Opcode::OP_POP);
    } else if (left->type() == Frontend::NodeType::Identifier) {
        const auto* id = static_cast<const Frontend::IdentifierExpr*>(left);
        int slot = resolveLocal(id->name());
        if (slot != -1) {
            emit(Opcode::OP_SET_LOCAL);
            emit(static_cast<uint8_t>(slot));
        } else {
            size_t constant = makeConstant(
                Runtime::Value::string(new Runtime::String(id->name())));
            emit(Opcode::OP_SET_GLOBAL);
            emit(static_cast<uint8_t>(constant));
        }
        emit(Opcode::OP_POP);
    }
    
    // Track loop
    breakJumps_.push_back({});
    continueTargets_.push_back(loopStart);
    
    // Compile body
    compileStatement(stmt->body());
    
    // Increment $idx
    emit(Opcode::OP_GET_LOCAL);
    emit(static_cast<uint8_t>(idxSlot));
    emitConstant(Runtime::Value::number(1));
    emit(Opcode::OP_ADD);
    emit(Opcode::OP_SET_LOCAL);
    emit(static_cast<uint8_t>(idxSlot));
    emit(Opcode::OP_POP);
    
    // Loop back
    emitLoop(loopStart);
    
    // Patch exit
    patchJump(exitJump);
    emit(Opcode::OP_POP);  // Pop comparison result
    
    // Patch breaks
    for (size_t breakJump : breakJumps_.back()) {
        patchJump(breakJump);
    }
    breakJumps_.pop_back();
    continueTargets_.pop_back();
    
    endScope();
}


void BytecodeGenerator::compileTemplateLiteral(const Frontend::TemplateLiteralExpr* expr) {
    // `hello ${name}, you are ${age}!`
    // => "hello " + String(name) + ", you are " + String(age) + "!"
    
    const auto& quasis = expr->quasis();
    const auto& expressions = expr->expressions();
    
    // Emit first quasi
    if (!quasis.empty()) {
        emitConstant(Runtime::Value::string(new Runtime::String(quasis[0])));
    } else {
        emitConstant(Runtime::Value::string(new Runtime::String("")));
    }
    
    // Interleave expressions and remaining quasis
    for (size_t i = 0; i < expressions.size(); ++i) {
        // Compile expression and convert to string (OP_ADD with string does coercion)
        compileExpression(expressions[i].get());
        emit(Opcode::OP_ADD);  // Concatenate with previous
        
        // Emit next quasi part
        if (i + 1 < quasis.size()) {
            emitConstant(Runtime::Value::string(new Runtime::String(quasis[i + 1])));
            emit(Opcode::OP_ADD);
        }
    }
}

void BytecodeGenerator::compileSpreadElement(const Frontend::SpreadExpr* expr) {
    // Compile the argument (iterable)
    compileExpression(expr->argument());
    
    // Emit spread opcode to expand the iterable onto the stack
    emit(Opcode::OP_SPREAD);
}

void BytecodeGenerator::compileRestElement(const Frontend::RestElem* expr) {
    // Rest element typically doesn't need standalone bytecode compilation 
    // outside of destructuring patterns or function parameters.
    // However, if compiled directly, emit undefined.
    emit(Opcode::OP_NIL);
}

} // namespace Zepra::Bytecode

// =============================================================================
// Destructuring pattern bytecode emission
// =============================================================================

namespace Zepra::Bytecode {

void BytecodeGenerator::emitBindingPattern(const Frontend::Expression* pattern, bool isConst) {
    // The source value is already on the stack

    if (pattern->type() == Frontend::NodeType::ObjectPattern) {
        const auto* objPat = static_cast<const Frontend::ObjectPatternExpr*>(pattern);

        for (const auto& prop : objPat->properties()) {
            // Dup the source object for each property extraction
            emit(Opcode::OP_DUP);

            // Get the property value from the source
            if (prop.computed) {
                compileExpression(prop.key.get());
                emit(Opcode::OP_GET_ELEMENT);
            } else {
                const auto* keyId = static_cast<const Frontend::IdentifierExpr*>(prop.key.get());
                size_t nameConst = makeConstant(
                    Runtime::Value::string(new Runtime::String(keyId->name())));
                emit(Opcode::OP_GET_PROPERTY);
                emit(static_cast<uint8_t>(nameConst));
            }

            // Handle the value binding target
            const auto* target = prop.value.get();

            if (target->type() == Frontend::NodeType::AssignmentPattern) {
                // Default value: {key = default}
                const auto* assign = static_cast<const Frontend::AssignmentPatternExpr*>(target);
                const auto* binding = assign->left();

                // If extracted value is undefined, use default
                emit(Opcode::OP_DUP);
                size_t nilJump = emitJump(Opcode::OP_JUMP_IF_NIL);
                size_t doneJump = emitJump(Opcode::OP_JUMP);

                patchJump(nilJump);
                emit(Opcode::OP_POP); // pop the dup'd undefined
                emit(Opcode::OP_POP); // pop the undefined value
                compileExpression(assign->right()); // push default

                patchJump(doneJump);

                if (binding->type() == Frontend::NodeType::Identifier) {
                    const auto* id = static_cast<const Frontend::IdentifierExpr*>(binding);
                    declareVariable(id->name(), isConst);
                    defineVariable(id->name());
                } else {
                    emitBindingPattern(binding, isConst);
                }
            } else if (target->type() == Frontend::NodeType::Identifier) {
                const auto* id = static_cast<const Frontend::IdentifierExpr*>(target);
                declareVariable(id->name(), isConst);
                defineVariable(id->name());
            } else {
                // Nested pattern
                emitBindingPattern(target, isConst);
            }
        }

        // Handle rest: {...rest}
        if (objPat->rest()) {
            // For simplicity, rest in object patterns pushes undefined for now
            // Full implementation requires creating a new object minus extracted keys
            emit(Opcode::OP_NIL);
            const auto* restId = static_cast<const Frontend::IdentifierExpr*>(objPat->rest());
            declareVariable(restId->name(), isConst);
            defineVariable(restId->name());
        }

        // Pop the source object
        emit(Opcode::OP_POP);

    } else if (pattern->type() == Frontend::NodeType::ArrayPattern) {
        const auto* arrPat = static_cast<const Frontend::ArrayPatternExpr*>(pattern);

        for (size_t i = 0; i < arrPat->elements().size(); i++) {
            const auto& elem = arrPat->elements()[i];

            if (!elem) {
                // Elision — skip this index
                continue;
            }

            // Dup the source array
            emit(Opcode::OP_DUP);

            // Get element at index i
            emitConstant(Runtime::Value::number(static_cast<double>(i)));
            emit(Opcode::OP_GET_ELEMENT);

            const auto* target = elem.get();

            if (target->type() == Frontend::NodeType::AssignmentPattern) {
                // Default value: [x = default]
                const auto* assign = static_cast<const Frontend::AssignmentPatternExpr*>(target);
                const auto* binding = assign->left();

                emit(Opcode::OP_DUP);
                size_t nilJump = emitJump(Opcode::OP_JUMP_IF_NIL);
                size_t doneJump = emitJump(Opcode::OP_JUMP);

                patchJump(nilJump);
                emit(Opcode::OP_POP);
                emit(Opcode::OP_POP);
                compileExpression(assign->right());

                patchJump(doneJump);

                if (binding->type() == Frontend::NodeType::Identifier) {
                    const auto* id = static_cast<const Frontend::IdentifierExpr*>(binding);
                    declareVariable(id->name(), isConst);
                    defineVariable(id->name());
                } else {
                    emitBindingPattern(binding, isConst);
                }
            } else if (target->type() == Frontend::NodeType::Identifier) {
                const auto* id = static_cast<const Frontend::IdentifierExpr*>(target);
                declareVariable(id->name(), isConst);
                defineVariable(id->name());
            } else {
                // Nested pattern
                emitBindingPattern(target, isConst);
            }
        }

        // Handle rest: [...rest] — collects remaining elements
        if (arrPat->rest()) {
            // Simplified: push undefined for rest (full impl needs slice)
            emit(Opcode::OP_NIL);
            const auto* restId = static_cast<const Frontend::IdentifierExpr*>(arrPat->rest());
            declareVariable(restId->name(), isConst);
            defineVariable(restId->name());
        }

        // Pop the source array
        emit(Opcode::OP_POP);

    } else if (pattern->type() == Frontend::NodeType::Identifier) {
        const auto* id = static_cast<const Frontend::IdentifierExpr*>(pattern);
        declareVariable(id->name(), isConst);
        defineVariable(id->name());
    } else {
        error(pattern, "Unsupported pattern type in destructuring");
    }
}

} // namespace Zepra::Bytecode
