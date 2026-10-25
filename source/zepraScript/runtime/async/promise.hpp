#pragma once

/**
 * @file promise.hpp
 * @brief Promise objects and promise jobs (ES2024 §27.2)
 *
 * A promise settles once. Its reactions run as jobs on the owning VM's
 * microtask queue (VM::enqueueJob), never synchronously, so callbacks see
 * the order the specification defines: a script finishes before any
 * reaction, and reactions run first-registered first.
 */

#include "config.hpp"
#include "runtime/objects/value.hpp"
#include "runtime/objects/object.hpp"
#include <vector>
#include <functional>
#include <memory>

namespace Zepra::Runtime {

class VM;
class Function;

/**
 * @brief Promise states
 */
enum class PromiseState {
    Pending,
    Fulfilled,
    Rejected
};

/**
 * @brief Promise callback (C++ reactions through then())
 */
using PromiseCallback = std::function<Value(const Value&)>;

/**
 * @brief A promise capability: a promise and the functions that settle it
 *
 * For the built-in Promise constructor `intrinsic` is the new promise and
 * resolve/reject stay undefined (it is settled directly); a subclass
 * constructor hands out its own resolve and reject functions.
 */
struct PromiseCapability {
    Value promise;
    Value resolve;
    Value reject;
    class Promise* intrinsic = nullptr;

    void doResolve(const Value& value) const;
    void doReject(const Value& reason) const;
};

/**
 * @brief Promise object
 */
class Promise : public Object {
public:
    /// A pending promise. Inside a running VM it gets that VM's
    /// Promise.prototype and queues its jobs there.
    Promise();

    // State management
    PromiseState state() const { return state_; }
    Value result() const { return result_; }
    bool isHandled() const { return isHandled_; }

    /**
     * @brief The promise's own resolve function: the first call to resolve()
     * or reject() wins. A thenable is adopted through a job, a value fulfills.
     */
    void resolve(const Value& value);
    void reject(const Value& reason);

    /**
     * @brief A fresh pair of resolving functions sharing one "already
     * resolved" record (CreateResolvingFunctions), for executors and thenables.
     */
    std::pair<Function*, Function*> createResolvingFunctions();

    /**
     * @brief PerformPromiseThen with script handlers: a callable handler runs
     * with the settled value and its result (or throw) settles `capability`;
     * a missing one passes the value through. `capability` may be empty (await).
     */
    void addReactions(const Value& onFulfilled, const Value& onRejected, const PromiseCapability& capability);

    /// PerformPromiseThen with C++ continuations (await, host code)
    void addNativeReactions(std::function<void(const Value&)> onFulfilled,
                            std::function<void(const Value&)> onRejected);

    // C++ convenience API: each returns the derived promise
    Promise* then(PromiseCallback onFulfilled, PromiseCallback onRejected = nullptr);
    Promise* catchError(PromiseCallback onRejected);
    Promise* finally(PromiseCallback onFinally);

    // Static helpers for C++ callers
    static Promise* resolved(const Value& value);
    static Promise* rejected(const Value& reason);
    static Promise* all(const std::vector<Promise*>& promises);
    static Promise* race(const std::vector<Promise*>& promises);
    static Promise* allSettled(const std::vector<Promise*>& promises);
    static Promise* any(const std::vector<Promise*>& promises);
    static Object* withResolvers();

    /// PromiseResolve(%Promise%, value): a built-in promise itself, else a new one resolved with it
    static Promise* promiseResolve(const Value& value);

    /// The VM whose queue runs this promise's jobs (null outside any VM)
    VM* vm() const;

private:
    struct Reaction {
        Value onFulfilled;
        Value onRejected;
        PromiseCapability capability;
        std::function<void(const Value&)> nativeFulfilled;
        std::function<void(const Value&)> nativeRejected;
        bool isNative = false;
    };

    void resolveUnchecked(const Value& resolution);
    void settle(PromiseState state, const Value& value);
    void enqueueReaction(const Reaction& reaction);
    void enqueue(std::function<void()> job);

    PromiseState state_;
    Value result_;
    std::vector<Reaction> reactions_;
    bool alreadyResolved_ = false;  // record used by resolve()/reject()
    bool isHandled_ = false;
    VM* vm_ = nullptr;
};

/**
 * @brief Runs `body`; a JavaScript exception thrown from it (a thrown value,
 * a JSError, an engine error) lands in `thrown` as a script value and false
 * is returned. Execution limits and termination still propagate.
 */
bool catchAsValue(VM* vm, const std::function<void()>& body, Value& thrown);

/// Calls `callee` (a TypeError when it is not a function) on `vm`
Value callValue(VM* vm, const Value& callee, const Value& thisValue, const std::vector<Value>& args);

/**
 * @brief Jobs queued without a VM (C++ callers outside any script)
 *
 * Inside a running script enqueue() forwards to that VM's own queue, so
 * tabs never share jobs.
 */
class MicrotaskQueue {
public:
    static MicrotaskQueue& instance();

    void enqueue(std::function<void()> task);
    void process();
    void drain();
    bool isEmpty() const;

private:
    std::vector<std::function<void()>> queue_;
};

} // namespace Zepra::Runtime
