#pragma once

/**
 * @file async_function.hpp
 * @brief Async function and await implementation (ES2017)
 */

#include "config.hpp"
#include <algorithm>
#include "runtime/objects/function.hpp"
#include "promise.hpp"
#include <memory>

namespace Zepra::Runtime {

// Forward declarations
class Context;
class AsyncExecutionContext;

/**
 * @brief Async function execution state
 */
enum class AsyncState {
    Running,
    Suspended,   // Waiting on await
    Completed,
    Failed
};

/**
 * @brief Async execution context - tracks state of an async function call
 * 
 * When an async function is called, an AsyncExecutionContext is created
 * to track the current execution state, suspended awaits, and the
 * result promise.
 */
class AsyncExecutionContext {
public:
    explicit AsyncExecutionContext(Function* fn, Context* ctx);
    ~AsyncExecutionContext() = default;
    
    AsyncState state() const { return state_; }
    Promise* resultPromise() const { return resultPromise_; }
    
    // Resume execution after await completes
    void resume(const Value& awaitResult);

    /**
     * @brief Consume the value stored by the last resume() call.
     * Called by the VM (OP_AWAIT handler) to push the settled value
     * back onto the operand stack after re-entering the async frame.
     */
    Value takeResumeValue() {
        Value v = resumeValue_;
        resumeValue_ = Value::undefined();
        return v;
    }

    // Complete with success
    void complete(const Value& result);
    
    // Complete with error
    void fail(const Value& error);
    
    // Suspend on await expression
    void suspend(Promise* awaitedPromise);
    
    // Current await depth (for nested awaits)
    size_t awaitDepth() const { return awaitDepth_; }
    
    // Execution context
    Context* context() const { return ctx_; }
    Function* function() const { return fn_; }
    
private:
    Function* fn_;
    Context* ctx_;
    AsyncState state_;
    Promise* resultPromise_;
    Value resumeValue_ = Value::undefined(); // Set by resume(), consumed by takeResumeValue()
    size_t awaitDepth_ = 0;

    // Stack of awaited promises for resumption
    std::vector<Promise*> awaitStack_;
};

/**
 * @brief AsyncFunction - represents an async function
 * 
 * Async functions always return a Promise. When called, they create
 * an AsyncExecutionContext and begin execution. Upon encountering
 * an await expression, execution suspends until the awaited Promise
 * settles.
 */
class AsyncFunction : public Function {
public:
    /**
     * @brief Create async function from AST
     */
    AsyncFunction(const Frontend::FunctionDecl* decl, Environment* closure);
    AsyncFunction(const Frontend::FunctionExpr* expr, Environment* closure);
    AsyncFunction(const Frontend::ArrowFunctionExpr* arrow, Environment* closure);
    
    /**
     * @brief Create async native function
     */
    AsyncFunction(std::string name, BuiltinFn builtin, size_t arity);
    
    ~AsyncFunction() override = default;
    
    /**
     * @brief Call the async function - always returns a Promise
     */
    Value callAsync(Context* ctx, Value thisValue, const std::vector<Value>& args);
    
    /**
     * @brief Get current execution context (if running)
     */
    AsyncExecutionContext* currentContext() const { return currentContext_.get(); }
    
private:
    std::unique_ptr<AsyncExecutionContext> currentContext_;
};

/**
 * @brief Await expression handler
 * 
 * Handles the await expression by:
 * 1. Converting the operand to a Promise (if not already)
 * 2. Suspending the async function execution
 * 3. Resuming when the Promise settles
 */
class AwaitHandler {
public:
    /**
     * @brief Process an await expression
     * @param value The value being awaited (converted to Promise if needed)
     * @param asyncCtx The async execution context to suspend
     * @return The awaited result (after resumption)
     */
    static Value await(const Value& value, AsyncExecutionContext* asyncCtx);
    
    /**
     * @brief Convert value to thenable/Promise
     */
    static Promise* toPromise(const Value& value);
};

/**
 * @brief Create an async wrapper around a native function
 */
inline AsyncFunction* createAsyncNativeFunction(const std::string& name,
                                                 BuiltinFn fn,
                                                 size_t arity = 0) {
    return new AsyncFunction(name, std::move(fn), arity);
}

} // namespace Zepra::Runtime
