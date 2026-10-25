#pragma once

/**
 * @file function.hpp
 * @brief JavaScript function objects
 */

#include "config.hpp"
#include <algorithm>
#include "object.hpp"
#include "value.hpp"
#include "frontend/ast.hpp"
#include <functional>
#include <vector>
#include <memory>
#include <string>

namespace Zepra::Bytecode {
class BytecodeChunk;
}

namespace Zepra::Runtime {

// Forward declarations
class VM;
class Environment;
class Context;

/**
 * @brief Runtime upvalue for capturing variables in closures
 * 
 * An upvalue is either "open" (pointing to a stack slot) or 
 * "closed" (containing the captured value directly).
 */
class RuntimeUpvalue {
public:
    explicit RuntimeUpvalue(Value* location) 
        : location_(location), closed_(Value::undefined()), isClosed_(false) {}
    
    Value* location() const { return location_; }
    
    Value get() const {
        return isClosed_ ? closed_ : *location_;
    }
    
    void set(Value value) {
        if (isClosed_) {
            closed_ = value;
        } else {
            *location_ = value;
        }
    }
    
    // Close the upvalue (copy value from stack to this object)
    void close() {
        if (!isClosed_) {
            closed_ = *location_;
            isClosed_ = true;
        }
    }
    
    // Point a closed upvalue at a stack slot again (a generator or async
    // function resuming); the caller has copied the closed value there
    void reopen(Value* location) {
        location_ = location;
        isClosed_ = false;
    }

    bool isClosed() const { return isClosed_; }
    
    // Linked list for open upvalues
    RuntimeUpvalue* next = nullptr;
    
private:
    Value* location_;   // Points to stack slot when open
    Value closed_;      // Holds value when closed
    bool isClosed_;
};

/**
 * @brief Information about a function call, used by native builtins
 */
class FunctionCallInfo {
public:
    FunctionCallInfo(Context* ctx, Value thisValue, const std::vector<Value>& args)
        : ctx_(ctx), thisValue_(thisValue), args_(args) {}
    
    /// Get the context
    Context* context() const { return ctx_; }
    
    /// Get the 'this' value
    Value thisValue() const { return thisValue_; }
    
    /// Get argument count
    size_t argumentCount() const { return args_.size(); }
    
    /// Get argument at index (returns undefined if out of bounds)
    Value argument(size_t index) const {
        if (index >= args_.size()) {
            return Value::undefined();
        }
        return args_[index];
    }
    
    /// Get all arguments
    const std::vector<Value>& arguments() const { return args_; }
    
private:
    Context* ctx_;
    Value thisValue_;
    const std::vector<Value>& args_;
};

/**
 * @brief Native function callback type (simple form)
 */
using NativeFn = std::function<Value(Context*, const std::vector<Value>&)>;

/**
 * @brief Native builtin function callback type (with FunctionCallInfo)
 */
using BuiltinFn = std::function<Value(const FunctionCallInfo&)>;

/**
 * @brief JavaScript function object
 */
class Function : public Object {
public:
    /**
     * @brief Create a JavaScript function from AST
     */
    Function(const Frontend::FunctionDecl* decl, Environment* closure);
    Function(const Frontend::FunctionExpr* expr, Environment* closure);
    Function(const Frontend::ArrowFunctionExpr* arrow, Environment* closure);
    
    /**
     * @brief Create a native function
     */
    Function(std::string name, NativeFn native, size_t arity);
    
    /**
     * @brief Create a builtin function (with FunctionCallInfo)
     */
    Function(std::string name, BuiltinFn builtin, size_t arity);
    
    /**
     * @brief Create a bound function
     */
    Function(Function* target, Value boundThis, std::vector<Value> boundArgs);

    /**
     * @brief Create a bytecode function; the compiler then sets its chunk and
     * frame layout. arity is the expected argument count (fn.length).
     */
    Function(std::string name, size_t arity, bool isArrow, bool isGenerator, bool isAsync);

    /**
     * @brief Frame layout of a bytecode function, set by the compiler
     *
     * Parameter slots follow the receiver in slot 0; a rest parameter takes
     * the last of them. Non-arrow functions reserve the slot after the
     * parameters for `arguments`, which is filled only when the body reads it.
     */
    struct FrameLayout {
        uint16_t parameterSlots = 0;
        bool hasRest = false;
        bool hasArgumentsSlot = false;
        bool usesArguments = false;
    };
    void setFrameLayout(const FrameLayout& layout) { layout_ = layout; hasLayout_ = true; }
    const FrameLayout* frameLayout() const { return hasLayout_ ? &layout_ : nullptr; }

    /**
     * @brief A new closure of this compiled function, made each time its
     * function expression is evaluated: shares the bytecode and metadata,
     * starts with no upvalues and no own properties.
     */
    Function* instantiateClosure() const;
    
    ~Function() override = default;
    
    // Function info
    const std::string& name() const { return name_; }
    size_t arity() const { return arity_; }
    bool isNative() const { return native_ != nullptr || builtin_ != nullptr; }
    bool isBuiltin() const { return builtin_ != nullptr; }
    bool isBound() const { return objectType_ == ObjectType::BoundFunction; }
    bool isArrow() const { return isArrow_; }
    bool isAsync() const { return isAsync_; }
    bool isGenerator() const { return isGenerator_; }
    bool isConstructor() const { return !isArrow_ && !isBound(); }
    
    // Closure access
    Environment* closure() const { return closure_; }
    
    // AST access (for interpreted execution)
    const Frontend::FunctionDecl* functionDecl() const { return functionDecl_; }
    const Frontend::FunctionExpr* functionExpr() const { return functionExpr_; }
    const Frontend::ArrowFunctionExpr* arrowExpr() const { return arrowExpr_; }
    
    // Native function access
    const NativeFn& nativeFunction() const { return native_; }
    const BuiltinFn& builtinFunction() const { return builtin_; }
    
    // Bytecode access (for compiled functions)
    const Bytecode::BytecodeChunk* bytecodeChunk() const { return bytecodeChunk_; }
    void setBytecodeChunk(const Bytecode::BytecodeChunk* chunk) { bytecodeChunk_ = chunk; }
    bool isCompiled() const { return bytecodeChunk_ != nullptr; }
    
    // Bound function access
    Function* boundTarget() const { return boundTarget_; }
    const Value& boundThis() const { return boundThis_; }
    const std::vector<Value>& boundArgs() const { return boundArgs_; }
    
    // Upvalue access
    void addUpvalue(RuntimeUpvalue* upvalue) { upvalues_.push_back(upvalue); }
    RuntimeUpvalue* upvalue(size_t index) const { 
        return index < upvalues_.size() ? upvalues_[index] : nullptr; 
    }
    size_t upvalueCount() const { return upvalues_.size(); }

    // The object a method was defined on (a class prototype, a class, or an
    // object literal): super.x reads from its prototype
    Object* homeObject() const { return homeObject_; }
    void setHomeObject(Object* home) { homeObject_ = home; }

    // A class constructor's instance field initializers, run on each new
    // object (base classes on entry, derived ones after super())
    Function* fieldInitializer() const { return fieldInitializer_; }
    void setFieldInitializer(Function* initializer) { fieldInitializer_ = initializer; }

    // Source metadata for debugging
    const std::string& sourceFile() const { return sourceFile_; }
    uint32_t sourceLine() const { return sourceLine_; }
    void setSourceFile(const std::string& file) { sourceFile_ = file; }
    void setSourceLine(uint32_t line) { sourceLine_ = line; }
    
    // Closure environment access for debugger
    Environment* getClosureEnvironment() const { return closure_; }

    // Debug metadata for frame introspection (shared by every closure of
    // the same compiled function)
    const std::vector<std::string>& localNames() const;
    const std::vector<std::string>& paramNames() const;
    void setDebugNames(std::vector<std::string> params, std::vector<std::string> locals);

    // Call support
    Value call(Context* ctx, Value thisValue, const std::vector<Value>& args);
    Value construct(Context* ctx, const std::vector<Value>& args);
    
    // Function.prototype methods
    Function* bind(Value thisArg, const std::vector<Value>& args);
    Value apply(Context* ctx, Value thisArg, const std::vector<Value>& args);
    
private:
    std::string name_;
    size_t arity_ = 0;
    
    // For JavaScript functions
    Environment* closure_ = nullptr;
    const Frontend::FunctionDecl* functionDecl_ = nullptr;
    const Frontend::FunctionExpr* functionExpr_ = nullptr;
    const Frontend::ArrowFunctionExpr* arrowExpr_ = nullptr;
    
    // For bytecode-compiled functions
    const Bytecode::BytecodeChunk* bytecodeChunk_ = nullptr;
    
    // For native functions
    NativeFn native_;
    BuiltinFn builtin_;
    
    // For bound functions
    Function* boundTarget_ = nullptr;
    Value boundThis_;
    std::vector<Value> boundArgs_;
    
    // Upvalues for closures
    std::vector<RuntimeUpvalue*> upvalues_;

    Object* homeObject_ = nullptr;
    Function* fieldInitializer_ = nullptr;

    // Function flags
    bool isArrow_ = false;
    bool isAsync_ = false;
    bool isGenerator_ = false;
    std::string sourceFile_;
    struct DebugNames {
        std::vector<std::string> params;
        std::vector<std::string> locals;
    };
    std::shared_ptr<const DebugNames> debugNames_;
    uint32_t sourceLine_ = 0;
    FrameLayout layout_;
    bool hasLayout_ = false;
};

/**
 * @brief Create a native function
 */
inline Function* createNativeFunction(const std::string& name, 
                                       NativeFn fn, 
                                       size_t arity = 0) {
    return new Function(name, std::move(fn), arity);
}

} // namespace Zepra::Runtime
