#pragma once

/**
 * @file vm.hpp
 * @brief Virtual Machine for bytecode execution
 */

#include "config.hpp"
#include "runtime/objects/value.hpp"
#include "runtime/objects/object.hpp"  // ObjectType (allocObject default argument)
#include "environment.hpp"
#include "bytecode/opcode.hpp"
#include <vector>
#include <memory>
#include <functional>
#include <stack>
#include <unordered_map>
#include <stdexcept>
#include "jit/jit_profiler.hpp"
#include "jit/baseline_jit.hpp"

namespace Zepra::Runtime {

// Forward declarations
class Context;
class Object;
class Function;
class GCHeap;
class RuntimeUpvalue;
class ResourceMonitor;
struct SandboxConfig;

}  // namespace Zepra::Runtime

namespace Zepra::GC {
class Heap;
}

namespace Zepra::Bytecode {
class BytecodeChunk;
}

namespace Zepra::Runtime {

/**
 * @brief Call frame for function execution
 */
struct CallFrame {
    Function* function = nullptr;
    const uint8_t* ip = nullptr;        // Instruction pointer
    size_t stackBase = 0;               // Base of this frame in operand stack
    Environment* environment = nullptr; // Current scope
    Value thisValue;                    // 'this' binding
};

/**
 * @brief Execution result
 */
struct ExecutionResult {
    enum class Status {
        Success,
        Exception,
        Error
    };
    
    Status status = Status::Success;
    Value value;
    std::string error;
};

/**
 * @brief VM call stack frame
 */
struct VMCallFrame {
    Function* function = nullptr;
    size_t returnAddress = 0;
    size_t slotBase = 0;
    Value thisValue;
    const Bytecode::BytecodeChunk* savedChunk = nullptr;  // Caller's chunk for iterative calls
    bool isConstruct = false;  // 'new' / super(): a non-object return yields 'this'
};

/**
 * @brief Exception handler for try/catch blocks
 */
struct ExceptionHandler {
    size_t catchAddress = 0;    // Jump target for catch block
    size_t finallyAddress = 0;  // Jump target for finally block
    size_t stackLevel = 0;      // Stack size when try began
    size_t callStackLevel = 0;  // Call stack size when try began
    const Bytecode::BytecodeChunk* chunk = nullptr;  // Chunk holding the handler
    size_t runDepth = 0;        // run() nesting level that installed it
};

/**
 * @brief A JavaScript exception crossing native code
 *
 * Thrown out of the interpreter when no handler in the current run() can
 * catch a value; carries it through builtins and callbacks so an enclosing
 * script's try/catch can catch it, or the host can report it.
 */
class ThrownException : public std::runtime_error {
public:
    explicit ThrownException(Value value);
    const Value& value() const { return value_; }
private:
    Value value_;
};

/**
 * @brief Generator frame for yield suspension/resumption
 */
struct GeneratorFrame {
    Function* function = nullptr;
    size_t suspendedIP = 0;          // IP when yielded
    std::vector<Value> savedStack;    // Stack state at yield
    size_t stackBase = 0;             // Base slot in stack
    Value thisValue;                  // 'this' binding
    bool isCompleted = false;         // Generator finished
    bool isStarted = false;           // Has been called at least once
};

/**
 * @brief Virtual Machine for executing JavaScript bytecode
 */
class VM {
public:
    explicit VM(Context* context);
    ~VM();
    
    /**
     * @brief Execute a bytecode chunk
     */
    ExecutionResult execute(const Bytecode::BytecodeChunk* chunk);
    
    /**
     * @brief Call a function
     */
    Value call(Function* fn, Value thisValue, const std::vector<Value>& args);
    
    /**
     * @brief Resume a suspended generator
     */
    Value resumeGenerator(GeneratorFrame* frame, Value yieldVal, const std::vector<Value>& args);
    
    /**
     * @brief Construct an object
     */
    Value construct(Function* fn, const std::vector<Value>& args);
    
    /**
     * @brief Get the current context
     */
    Context* context() const { return context_; }
    
    /**
     * @brief Stack operations
     */
    void push(Value value);
    Value pop();
    Value peek(size_t distance = 0) const;
    void popN(size_t count);
    
    /**
     * @brief Get stack size
     */
    size_t stackSize() const { return stack_.size(); }
    
    /**
     * @brief Get current call depth (for debugger)
     */
    size_t getCallDepth() const { return nativeDepth_ + heapDepth_; }
    
    /**
     * @brief Set a global variable
     */
    void setGlobal(const std::string& name, Value value) { globals_[name] = value; }
    
    /**
     * @brief Get a global variable
     */
    Value getGlobal(const std::string& name) const {
        auto it = globals_.find(name);
        return it != globals_.end() ? it->second : Value::undefined();
    }
    
    /**
     * @brief Set current module path for import resolution
     */
    void setModulePath(const std::string& path) { currentModulePath_ = path; }
    const std::string& currentModulePath() const { return currentModulePath_; }
    
    /**
     * @brief Set module loader for ES module support
     */
    void setModuleLoader(class ModuleLoader* loader) { moduleLoader_ = loader; }
    class ModuleLoader* moduleLoader() const { return moduleLoader_; }
    
    /**
     * @brief Get current executing VM (for callbacks from builtins)
     */
    static VM* current() { return currentVM_; }

    /**
     * @brief The prototypes the interpreter itself relies on
     *
     * One set per VM, created on first use. The global object's constructors
     * (Object, Array, Number, ...) take these same objects as their
     * `prototype`, so `Array.prototype.x = ...` is seen by every array, and
     * methods on primitives resolve here without a global lookup.
     */
    enum class Intrinsic { Object, Function, Array, String, Number, Boolean, Symbol, Error, Iterator, Count };
    Object* intrinsic(Intrinsic kind);

    /**
     * @brief obj.name as script code reads it: accessors run with `target` as
     * their receiver, primitives read through their prototype, and reading
     * from null or undefined is a TypeError. Built-ins use this to honour
     * user-defined properties (an Error subclass's `name`, a custom `length`).
     */
    Value getMember(const Value& target, const std::string& name) { return getProperty(target, name); }

    /** ToPrimitive: objects run valueOf()/toString() (toString first when `preferString`). */
    Value toPrimitiveValue(const Value& value, bool preferString) { return toPrimitive(value, preferString); }

    /** A new error object of the given type ("TypeError", ...) with its prototype and stack. */
    Value newError(const std::string& type, const std::string& message) { return makeError(type, message); }

    /**
     * @brief The current call stack as "    at function (bytecode:offset)" lines,
     * innermost first. Error objects store it as `stack`.
     */
    std::string captureStackTrace() const;

    /**
     * @brief Execute a callback function (for array methods like map/filter/reduce)
     */
    Value executeCallback(Function* fn, Value thisValue, const std::vector<Value>& args);
    
    /**
     * @brief Configure sandbox for execution limits and security
     */
    void setSandbox(ResourceMonitor* monitor) { resourceMonitor_ = monitor; }
    ResourceMonitor* resourceMonitor() const { return resourceMonitor_; }
    
    /**
     * @brief Set GC heap for safe-point and write barrier integration
     */
    void setGCHeap(GCHeap* heap) { gcHeap_ = heap; }
    GCHeap* gcHeap() const { return gcHeap_; }
    
    /**
     * @brief Allocate an Object via GCHeap when available, raw new otherwise.
     * All VM-internal object creation should go through this.
     */
    Object* allocObject(ObjectType type = ObjectType::Ordinary);
    
    /**
     * @brief Scan all GC roots: globals, operand stack, call frames.
     * Called by GCHeap root scanner during marking.
     */
    void scanRoots(std::function<void(Object*)> visitor) const;
    
    /**
     * @brief Check if execution should be terminated (timeout/limit)
     */
    bool shouldTerminate() const;
    
    /**
     * @brief Request termination of current execution
     */
    void requestTermination() { terminationRequested_ = true; }
    bool isTerminationRequested() const { return terminationRequested_; }
    
    // Frame accessors for debugger
    std::string getFrameFunctionName(size_t frameIdx) const;
    std::string getFrameSourceFile(size_t frameIdx) const;
    uint32_t getFrameLine(size_t frameIdx) const;
    uint32_t getFrameColumn(size_t frameIdx) const;
    Value getFrameThisValue(size_t frameIdx) const;
    std::vector<std::string> getFrameLocalNames(size_t frameIdx) const;
    Value getFrameLocal(size_t frameIdx, const std::string& name) const;
    std::vector<std::string> getFrameClosureNames(size_t frameIdx) const;
    Value getFrameClosureValue(size_t frameIdx, const std::string& name) const;
    
    // Evaluate expression in frame scope (for debugger watch/evaluate)
    Value evaluateInFrame(size_t frameIdx, const std::string& expression);
    
    // Script loading for workers
    std::string loadBundledScript(const std::string& url);
    void* compile(const std::string& source, const std::string& filename = "");
    void execute(void* compiled);
    
    // Event loop integration (browser-grade timer/microtask processing)
    class EventLoop* eventLoop() const { return eventLoop_; }
    void setEventLoop(class EventLoop* loop) { eventLoop_ = loop; }
    void runEventLoop();
    
    // JIT compilation
    JIT::BaselineJIT* jit() { return &jit_; }
    bool hasJITCode(void* key) const { return jitCache_.count(key) > 0; }
    void* getJITEntry(void* key) const {
        auto it = jitCache_.find(key);
        return it != jitCache_.end() ? it->second->entryPoint() : nullptr;
    }
    
private:
    // Thread-local current VM for callback execution
    static thread_local VM* currentVM_;

    // Intrinsic prototypes, created by intrinsic() on first use
    Object* intrinsics_[static_cast<size_t>(Intrinsic::Count)] = {};
    Object* createFunctionPrototype();

    // Bytecode execution
    void run();
    void dispatch(Zepra::Bytecode::Opcode op);
    
    // Read helpers
    uint8_t readByte();
    uint16_t readShort();
    Value readConstant();
    
    // Local variable access
    Value getLocal(size_t slot);
    void setLocal(size_t slot, Value value);

    // Calls. invoke/constructValue/superConstruct run native functions at
    // once (pushing the result) and enter compiled ones in this run loop.
    void invoke(Function* fn, Value thisValue, std::vector<Value> args);
    void constructValue(const Value& callee, std::vector<Value> args);
    void superConstruct(const Value& superclass, const Value& thisValue, std::vector<Value> args);
    void enterFrame(Function* fn, Value thisValue, const std::vector<Value>& args,
                    bool isConstruct, size_t returnAddress);
    void pushArguments(Function* fn, const std::vector<Value>& args);
    std::vector<Value> takeArguments(size_t count);
    std::vector<Value> arrayElements(const Value& array);
    Function* functionOrThrow(const Value& callee, const std::string& what);
    Value makeGenerator(Function* fn, Value thisValue, const std::vector<Value>& args);

    // Properties: one implementation of the semantics for obj.name, obj[key],
    // method calls, accessors and primitives
    const std::string& constantString(size_t index) const;
    Value getProperty(const Value& target, const std::string& name);
    Value getElement(const Value& target, const Value& key);
    void setProperty(const Value& target, const std::string& name, const Value& value);
    void setElement(const Value& target, const Value& key, const Value& value);
    bool deleteProperty(const Value& target, const Value& key);
    Object* primitivePrototype(const Value& value);
    bool readProperty(Object* holder, const Value& receiver, const std::string& name, Value& out);
    Object* ensurePrototypeObject(Function* fn);

    // Iteration (for-of, spread, array destructuring)
    Value makeIterator(const Value& iterable);
    bool iteratorStep(const Value& iterator, Value& value);

    // Conversions that may run script (valueOf/toString)
    Value toPrimitive(const Value& value, bool preferString);
    void toPrimitiveOperands(Value& a, Value& b);

    // Exceptions
    void throwValue(Value exception);
    Value makeError(const std::string& type, const std::string& message);
    void unwindTo(size_t callDepth, size_t stackSize, size_t handlerCount);
    
    Context* context_;
    
    // Current bytecode chunk
    const Bytecode::BytecodeChunk* chunk_ = nullptr;
    size_t ip_ = 0;
    uint16_t currentLine_ = 0;
    
    // Operand stack
    std::vector<Value> stack_;
    
    // ==========================================================================
    // HYBRID TWO-STACK ARCHITECTURE
    // Fast path: Native C stack for 99% of calls (zero allocation)
    // Slow path: Heap stack for deep recursion (grows as needed)
    // ==========================================================================
    
    // Stack configuration constants
    static constexpr size_t NATIVE_STACK_SIZE = 2048;      // Frames on C stack
    static constexpr size_t NATIVE_STACK_THRESHOLD = 1536; // 75% - switch point
    static constexpr size_t HEAP_STACK_PREALLOC = 1024;    // Pre-allocate frames
    static constexpr size_t HEAP_STACK_MAX = 262144;       // Max ~7MB heap stack
    
    // Native stack (FAST PATH - zero allocation)
    VMCallFrame nativeStack_[NATIVE_STACK_SIZE];
    size_t nativeDepth_ = 0;
    
    // Heap stack (SLOW PATH - for deep recursion)
    std::vector<VMCallFrame> heapStack_;
    size_t heapDepth_ = 0;
    
    // Helper: Get current call depth (total)
    size_t callDepth() const { return nativeDepth_ + heapDepth_; }
    
    // Helper: Should use heap stack?
    bool shouldUseHeapStack() const { return nativeDepth_ >= NATIVE_STACK_THRESHOLD; }
    
    // Helper: Get current frame
    VMCallFrame& currentFrame() {
        if (heapDepth_ > 0) return heapStack_[heapDepth_ - 1];
        if (nativeDepth_ > 0) return nativeStack_[nativeDepth_ - 1];
        static VMCallFrame empty;
        return empty;
    }
    
    // Helper: Push call frame (auto-switches)
    void pushCallFrame(const VMCallFrame& frame);
    
    // Helper: Pop call frame (auto-switches)  
    VMCallFrame popCallFrame();
    
    // Helper: Cleanup excess heap after returning to shallow
    void shrinkHeapIfNeeded();
    
    // Global variables
    std::unordered_map<std::string, Value> globals_;
    
    // Open upvalues (linked list, ordered by stack slot)
    RuntimeUpvalue* openUpvalues_ = nullptr;
    
    // Exception handling
    std::vector<ExceptionHandler> exceptionHandlers_;
    Value exceptionValue_;
    bool hasException_ = false;
    size_t runDepth_ = 0;  // Nesting of run(): callbacks and generators run inside builtins
    
    // Helper to capture/reuse upvalue
    RuntimeUpvalue* captureUpvalue(Value* local);
    void closeUpvalues(Value* last);
    
    // Module loading
    std::string currentModulePath_;
    class ModuleLoader* moduleLoader_ = nullptr;  // Owned externally or by Context
    
    // Generator support
    GeneratorFrame* currentGenerator_ = nullptr;  // Active generator being executed
    bool isYielding_ = false;                     // Set when yield suspends execution
    Value yieldedValue_;                          // Value passed to yield
    
    // Sandbox/Security support
    ResourceMonitor* resourceMonitor_ = nullptr;  // Resource limit tracking
    GCHeap* gcHeap_ = nullptr;                    // GC heap for safe-points/barriers
    JIT::JITProfiler jitProfiler_;                  // Hot function detection
    JIT::BaselineJIT jit_;                          // Baseline JIT compiler
    std::unordered_map<void*, std::unique_ptr<JIT::CompiledCode>> jitCache_;
    class EventLoop* eventLoop_ = nullptr;          // Event loop (owned by Context)
    bool terminationRequested_ = false;           // Termination flag
    uint64_t instructionCounter_ = 0;             // For periodic limit checks
    static constexpr uint64_t LIMIT_CHECK_INTERVAL = 4096;  // Check every N instructions
};

} // namespace Zepra::Runtime

