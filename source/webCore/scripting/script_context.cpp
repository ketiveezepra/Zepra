// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file script_context.cpp
 * @brief ScriptContext — wired to ZepraScript VM (real implementation)
 *
 * Uses the internal VM directly:
 *   SourceCode::fromString → Parser → SyntaxChecker → BytecodeGenerator → VM::execute
 */

#include "scripting/script_context.hpp"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <memory>
#include <unordered_map>

// ZepraScript internal headers
#include "runtime/execution/vm.hpp"
#include "runtime/execution/Sandbox.h"
#include "runtime/execution/context.hpp"
#include "runtime/execution/global_object.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"
#include "frontend/source_code.hpp"
#include "frontend/parser.hpp"
#include "frontend/syntax_checker.hpp"
#include "bytecode/bytecode_generator.hpp"
#include "builtins/console.hpp"

namespace Zepra::WebCore {

namespace {
// Each page script or callback runs under limits, so a runaway script
// (endless loop, unbounded allocation) is stopped instead of freezing the
// browser or exhausting the machine's memory.
class ScriptRunGuard {
public:
    explicit ScriptRunGuard(Zepra::Runtime::VM* vm) : vm_(vm), monitor_(limits()) {
        if (vm_) {
            previous_ = vm_->resourceMonitor();
            vm_->setSandbox(&monitor_);
        }
    }
    ~ScriptRunGuard() {
        if (vm_) vm_->setSandbox(previous_);
    }
    ScriptRunGuard(const ScriptRunGuard&) = delete;
    ScriptRunGuard& operator=(const ScriptRunGuard&) = delete;

private:
    static Zepra::Runtime::ExecutionLimits limits() {
        Zepra::Runtime::ExecutionLimits l;
        l.maxExecutionTime = std::chrono::seconds(10);
        l.maxMemoryGrowthBytes = size_t(1024) * 1024 * 1024;  // 1 GB per run
        l.maxHeapBytes = SIZE_MAX;  // heap counters are not maintained; RSS growth is
        return l;
    }

    Zepra::Runtime::VM* vm_;
    Zepra::Runtime::ResourceMonitor monitor_;
    Zepra::Runtime::ResourceMonitor* previous_ = nullptr;
};

// An in-memory Web Storage: string keys and values in insertion order with
// getItem, setItem, removeItem, clear, key(n) and a length property.
Zepra::Runtime::Object* makeWebStorage() {
    using namespace Zepra::Runtime;
    struct Data {
        std::vector<std::string> order;
        std::unordered_map<std::string, std::string> values;
    };
    auto data = std::make_shared<Data>();
    auto* storage = new Object();

    auto method = [storage](const char* name, size_t arity, NativeFn fn) {
        PropertyDescriptor desc;
        desc.value = Value::object(createNativeFunction(name, std::move(fn), arity));
        desc.attributes = PropertyAttribute::Writable | PropertyAttribute::Configurable;
        storage->defineProperty(name, desc);
    };
    auto text = [](const std::vector<Value>& args, size_t index) {
        return index < args.size() ? args[index].toString() : std::string("undefined");
    };

    method("getItem", 1, [data, text](Context*, const std::vector<Value>& args) -> Value {
        auto found = data->values.find(text(args, 0));
        return found == data->values.end() ? Value::null() : Value::string(new String(found->second));
    });
    method("setItem", 2, [data, text](Context*, const std::vector<Value>& args) -> Value {
        const std::string key = text(args, 0);
        if (data->values.find(key) == data->values.end()) data->order.push_back(key);
        data->values[key] = text(args, 1);
        return Value::undefined();
    });
    method("removeItem", 1, [data, text](Context*, const std::vector<Value>& args) -> Value {
        const std::string key = text(args, 0);
        if (data->values.erase(key) > 0) {
            data->order.erase(std::remove(data->order.begin(), data->order.end(), key), data->order.end());
        }
        return Value::undefined();
    });
    method("clear", 0, [data](Context*, const std::vector<Value>&) -> Value {
        data->order.clear();
        data->values.clear();
        return Value::undefined();
    });
    method("key", 1, [data](Context*, const std::vector<Value>& args) -> Value {
        const double index = args.empty() ? 0 : args[0].toNumber();
        if (!(index >= 0) || index >= static_cast<double>(data->order.size())) return Value::null();
        return Value::string(new String(data->order[static_cast<size_t>(index)]));
    });

    PropertyDescriptor length;
    length.getter = Value::object(createNativeFunction("length", [data](Context*, const std::vector<Value>&) -> Value {
        return Value::number(static_cast<double>(data->order.size()));
    }, 0));
    length.attributes = PropertyAttribute::Configurable;
    storage->defineProperty("length", length);
    return storage;
}
} // namespace

ScriptContext::ScriptContext() : vm_(nullptr), vmReady_(false) {
    timeOriginMs_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    try {
        ownedVm_ = std::make_unique<Zepra::Runtime::VM>(nullptr);
        ownedContext_ = std::make_unique<Zepra::Runtime::Context>(ownedVm_.get());
        
        // The engine's own built-ins (Object, Array, Map, JSON, Error,
        // Promise, queueMicrotask, ...). The embedder supplies the rest of the
        // page environment itself: the console and timers route into the
        // browser, and WebAssembly and the web APIs are defined in
        // setupWindowGlobals().
        auto* globalObj = new Zepra::Runtime::GlobalObject(ownedContext_.get());
        globalObj->initialize();
        globalObj->installInto(*ownedVm_, {"console", "setTimeout", "setInterval", "clearTimeout",
                                           "clearInterval", "WebAssembly"});
        globalObject_ = globalObj;

        vm_ = ownedVm_.get();
        vmReady_ = true;
        // Uncaught errors from microtasks and unhandled promise rejections
        // reach the page console
        vm_->setErrorReporter([this](const std::string& message) {
            if (consoleHandler_) consoleHandler_("error", message);
            else std::cerr << "[console.error] " << message << std::endl;
        });
        std::cout << "[ScriptContext] Per-tab VM initialized" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[ScriptContext] VM init failed: " << e.what() << std::endl;
        ownedVm_.reset();
        ownedContext_.reset();
    }
}

ScriptContext::~ScriptContext() {
    // Per-instance VM destroyed automatically by unique_ptr
}

void ScriptContext::initialize(DOMDocument* document) {
    document_ = document;
    setupGlobals();
}

ScriptResult ScriptContext::evaluate(const std::string& code, const std::string& filename) {
    ScriptResult result;
    
    if (code.empty()) {
        result.success = true;
        result.value = "undefined";
        return result;
    }
    
    if (!vm_ || !vmReady_) {
        result.success = false;
        result.error = "VM not available";
        return result;
    }
    
    // Skip oversized scripts — extremely large bundles (>512KB) are still
    // beyond what ZepraScript's single-pass compile can handle safely.
    static constexpr size_t MAX_SCRIPT_SIZE = 524288; // 512KB
    if (code.size() > MAX_SCRIPT_SIZE) {
        result.success = true;
        result.value = "undefined";
        std::cout << "[JS] Skipped large script (" << code.size() 
                  << " bytes > " << MAX_SCRIPT_SIZE << " limit)" << std::endl;
        return result;
    }
    
    auto t0 = std::chrono::steady_clock::now();
    
    try {
        // Full compile pipeline: source → AST → bytecode → execute
        auto sourceCode = Zepra::Frontend::SourceCode::fromString(
            code, filename.empty() ? "<script>" : filename);
        
        Zepra::Frontend::Parser parser(sourceCode.get());
        auto ast = parser.parseProgram();
        
        auto t1 = std::chrono::steady_clock::now();
        auto parseMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        
        // Abort if parsing alone exceeded budget (500ms for framework bundles)
        if (parseMs > 500) {
            result.success = true;
            result.value = "undefined";
            std::cout << "[JS] Parse timeout (" << parseMs << "ms > 500ms) for " 
                      << code.size() << " byte script" << std::endl;
            return result;
        }
        
        if (parser.hasErrors()) {
            result.success = false;
            result.error = "SyntaxError: " + parser.errors().front();
            if (consoleHandler_) {
                consoleHandler_("error", result.error);
            }
            return result;
        }
        
        // Fast path: skip SyntaxChecker for small scripts (<4KB).
        // The checker catches const-without-init and break-outside-loop —
        // real page inline scripts never trigger these. Saves a full AST walk.
        static constexpr size_t SYNTAX_CHECK_THRESHOLD = 4096;
        if (code.size() >= SYNTAX_CHECK_THRESHOLD) {
            Zepra::Frontend::SyntaxChecker checker;
            if (!checker.check(ast.get())) {
                result.success = false;
                result.error = "SyntaxError: Validation failed";
                if (consoleHandler_) {
                    consoleHandler_("error", result.error);
                }
                return result;
            }
        }
        
        Zepra::Bytecode::BytecodeGenerator generator;
        auto chunk = generator.compile(ast.get());
        
        if (generator.hasErrors() || !chunk) {
            result.success = false;
            result.error = generator.errors().empty()
                ? std::string("SyntaxError: Bytecode generation failed")
                : "SyntaxError: " + generator.errors().front();
            if (consoleHandler_) {
                consoleHandler_("error", result.error);
            }
            return result;
        }
        
        // Execute bytecode on VM
        ScriptRunGuard guard(vm_);
        auto execResult = vm_->execute(chunk.get());
        
        auto t2 = std::chrono::steady_clock::now();
        auto parseUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t0).count();
        
        // Always log timing for scripts >10 bytes (µs precision)
        if (code.size() > 10) {
            std::cout << "[JS] " << code.size() << "B → parse:" << parseUs 
                      << "µs total:" << totalUs << "µs";
            if (totalUs > 16000) {
                std::cout << " ⚠ OVER 16ms BUDGET";
            }
            std::cout << std::endl;
        }
        
        if (execResult.status == Zepra::Runtime::ExecutionResult::Status::Success) {
            result.success = true;
            result.value = execResult.value.toString();
        } else {
            result.success = false;
            result.error = execResult.error;
            if (consoleHandler_) {
                consoleHandler_("error", execResult.error);
            }
        }
    } catch (const std::exception& e) {
        result.success = false;
        result.error = std::string("RuntimeError: ") + e.what();
        if (consoleHandler_) {
            consoleHandler_("error", result.error);
        }
    }
    
    return result;
}

void ScriptContext::log(const std::string& message) {
    if (consoleHandler_) {
        consoleHandler_("log", message);
    } else {
        std::cout << "[console] " << message << std::endl;
    }
}

int ScriptContext::scheduleTimer(int id, std::function<void()> callback, int delay, bool repeating) {
    double now = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    timers_.push_back({id, std::move(callback), delay, repeating, now + delay});
    return id;
}

int ScriptContext::setTimeout(std::function<void()> callback, int delay) {
    return scheduleTimer(nextTimerId_++, std::move(callback), delay, false);
}

int ScriptContext::setInterval(std::function<void()> callback, int interval) {
    int id = nextTimerId_++;
    double now = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    timers_.push_back({id, callback, interval, true, now + interval});
    return id;
}

void ScriptContext::clearTimeout(int id) {
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
        [id](const TimerCallback& t) { return t.id == id; }), timers_.end());
}

void ScriptContext::clearInterval(int id) {
    clearTimeout(id);
}

void ScriptContext::processTimers() {
    double now = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    // Collect due timers first: callbacks may add or clear timers, which
    // would invalidate an iterator into timers_
    std::vector<int> due;
    for (const auto& t : timers_) {
        if (now >= t.scheduledTime) due.push_back(t.id);
    }
    for (int id : due) {
        auto it = std::find_if(timers_.begin(), timers_.end(),
            [id](const TimerCallback& t) { return t.id == id; });
        if (it == timers_.end()) continue;  // cleared by an earlier callback
        std::function<void()> callback = it->callback;
        if (it->repeating) {
            it->scheduledTime = now + std::max(it->delay, 1);
        } else {
            timers_.erase(it);
        }
        if (callback) callback();
    }
}

void ScriptContext::runAnimationFrames() {
    if (animationFrameQueue_.empty()) return;
    const double timestampMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count() - timeOriginMs_;
    // Callbacks requested while these run belong to the next frame
    std::vector<int> queue;
    queue.swap(animationFrameQueue_);
    for (int id : queue) {
        const std::string key = "r" + std::to_string(id);
        callStoredCallback(key, true, timestampMs);
        removeStoredCallback(key);
    }
}

void ScriptContext::callStoredCallback(const std::string& key, bool passTimestamp, double timestampMs) {
    using namespace Zepra::Runtime;
    if (!vm_ || !callbackStore_ || !callbackStore_->has(key)) return;
    Value fnValue = callbackStore_->get(key);
    if (!fnValue.isObject()) return;
    auto* fn = dynamic_cast<Function*>(fnValue.asObject());
    if (!fn) return;
    std::vector<Value> args;
    if (passTimestamp) args.push_back(Value::number(timestampMs));
    // The callback is a task of its own: its microtasks run right after it,
    // under the same limits, even when it threw. A run stopped by the limits
    // takes its queued microtasks with it.
    ScriptRunGuard guard(vm_);
    try {
        vm_->call(fn, Value::undefined(), args);
    } catch (const std::exception& e) {
        if (consoleHandler_) consoleHandler_("error", std::string("Uncaught exception in callback: ") + e.what());
        if (vm_->isTerminationRequested()) {
            vm_->discardJobs();
            return;
        }
    } catch (...) {
        if (consoleHandler_) consoleHandler_("error", "Uncaught exception in callback");
    }
    try {
        vm_->runMicrotasks();
    } catch (const std::exception& e) {
        if (consoleHandler_) consoleHandler_("error", std::string("Microtasks stopped: ") + e.what());
    }
}

void ScriptContext::removeStoredCallback(const std::string& key) {
    if (callbackStore_) callbackStore_->deleteProperty(key);
}

void ScriptContext::setGlobal(const std::string& name, const std::string& value) {
    if (vm_) {
        vm_->setGlobal(name, Zepra::Runtime::Value::string(
            new Zepra::Runtime::String(value)));
    }
}

void ScriptContext::fireDOMContentLoaded() {
    for (auto& listener : domContentLoadedListeners_) {
        if (listener) listener();
    }
}

void ScriptContext::fireLoadEvent() {
    for (auto& listener : loadListeners_) {
        if (listener) listener();
    }
}

void ScriptContext::addEventListener(const std::string& eventType, std::function<void()> callback) {
    if (eventType == "DOMContentLoaded") {
        domContentLoadedListeners_.push_back(callback);
    } else if (eventType == "load") {
        loadListeners_.push_back(callback);
    }
}

void ScriptContext::alert(const std::string& message) {
    if (alertHandler_) alertHandler_(message);
    else std::cout << "[alert] " << message << std::endl;
}

bool ScriptContext::confirm(const std::string& message) {
    if (confirmHandler_) return confirmHandler_(message);
    return true;
}

std::string ScriptContext::prompt(const std::string& message, const std::string& defaultValue) {
    if (promptHandler_) return promptHandler_(message, defaultValue);
    return defaultValue;
}

void ScriptContext::setupGlobals() {
    setupWindowGlobals();
    setupDocumentGlobals();
}

void ScriptContext::setupWindowGlobals() {
    if (!vm_) return;
    
    using namespace Zepra::Runtime;
    
    // window.alert(message)
    auto* alertFn = createNativeFunction("alert",
        [this](Context*, const std::vector<Value>& args) -> Value {
            std::string msg = args.empty() ? "" : args[0].toString();
            alert(msg);
            return Value::undefined();
        }, 1);
    vm_->setGlobal("alert", Value::object(alertFn));
    
    // window.confirm(message)
    auto* confirmFn = createNativeFunction("confirm",
        [this](Context*, const std::vector<Value>& args) -> Value {
            std::string msg = args.empty() ? "" : args[0].toString();
            return Value::boolean(confirm(msg));
        }, 1);
    vm_->setGlobal("confirm", Value::object(confirmFn));
    
    // window.prompt(message, default)
    auto* promptFn = createNativeFunction("prompt",
        [this](Context*, const std::vector<Value>& args) -> Value {
            std::string msg = args.empty() ? "" : args[0].toString();
            std::string def = args.size() > 1 ? args[1].toString() : "";
            return Value::string(new String(prompt(msg, def)));
        }, 2);
    vm_->setGlobal("prompt", Value::object(promptFn));
    
    // console.log — bridge to console handler. The handler is looked up on every
    // call, so one installed (or replaced) after initialize() still gets output.
    auto* logFn = createNativeFunction("log",
        [this](Context*, const std::vector<Value>& args) -> Value {
            std::string msg;
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) msg += " ";
                msg += args[i].toString();
            }
            if (consoleHandler_) consoleHandler_("log", msg);
            else std::cout << "[console.log] " << msg << std::endl;
            return Value::undefined();
        }, 0);  // variadic
    
    auto* consoleObj = new Object();
    consoleObj->set("log", Value::object(logFn));
    
    auto* warnFn = createNativeFunction("warn",
        [this](Context*, const std::vector<Value>& args) -> Value {
            std::string msg;
            for (auto& a : args) msg += a.toString() + " ";
            if (consoleHandler_) consoleHandler_("warn", msg);
            else std::cerr << "[console.warn] " << msg << std::endl;
            return Value::undefined();
        }, 0);
    consoleObj->set("warn", Value::object(warnFn));
    
    auto* errorFn = createNativeFunction("error",
        [this](Context*, const std::vector<Value>& args) -> Value {
            std::string msg;
            for (auto& a : args) msg += a.toString() + " ";
            if (consoleHandler_) consoleHandler_("error", msg);
            else std::cerr << "[console.error] " << msg << std::endl;
            return Value::undefined();
        }, 0);
    consoleObj->set("error", Value::object(errorFn));
    vm_->setGlobal("console", Value::object(consoleObj));
    
    // setTimeout(callback, delay)
    callbackStore_ = new Object();
    vm_->setGlobal("__zepraPendingCallbacks__", Value::object(callbackStore_));

    // setTimeout/setInterval(callback | code, delay)
    auto scheduleJsTimer = [this](const std::vector<Value>& args, bool repeating) -> Value {
        if (args.empty()) return Value::number(0);
        int delay = args.size() > 1 ? std::max(0, (int)args[1].toNumber()) : 0;
        int id = nextTimerId_++;
        std::function<void()> run;
        if (args[0].isObject() && args[0].asObject()->isFunction()) {
            const std::string key = "t" + std::to_string(id);
            callbackStore_->set(key, args[0]);
            run = [this, key, repeating] {
                callStoredCallback(key, false, 0);
                if (!repeating) removeStoredCallback(key);
            };
        } else {
            const std::string code = args[0].toString();
            run = [this, code] { evaluate(code, "<timer>"); };
        }
        return Value::number(scheduleTimer(id, std::move(run), delay, repeating));
    };

    auto* setTimeoutFn = createNativeFunction("setTimeout",
        [scheduleJsTimer](Context*, const std::vector<Value>& args) -> Value {
            return scheduleJsTimer(args, false);
        }, 2);
    vm_->setGlobal("setTimeout", Value::object(setTimeoutFn));
    
    // setInterval(callback, interval)
    auto* setIntervalFn = createNativeFunction("setInterval",
        [scheduleJsTimer](Context*, const std::vector<Value>& args) -> Value {
            return scheduleJsTimer(args, true);
        }, 2);
    vm_->setGlobal("setInterval", Value::object(setIntervalFn));
    
    // clearTimeout / clearInterval
    auto* clearTimeoutFn = createNativeFunction("clearTimeout",
        [this](Context*, const std::vector<Value>& args) -> Value {
            if (!args.empty()) {
                int id = (int)args[0].toNumber();
                clearTimeout(id);
                removeStoredCallback("t" + std::to_string(id));
            }
            return Value::undefined();
        }, 1);
    vm_->setGlobal("clearTimeout", Value::object(clearTimeoutFn));
    vm_->setGlobal("clearInterval", Value::object(clearTimeoutFn));
    
    // navigator.userAgent
    auto* navObj = new Object();
    navObj->set("userAgent", Value::string(new String("ZepraBrowser/1.0")));
    navObj->set("platform", Value::string(new String("Linux")));
    navObj->set("language", Value::string(new String("en-US")));
    vm_->setGlobal("navigator", Value::object(navObj));
    
    // location object
    auto* locObj = new Object();
    locObj->set("href", Value::string(new String("")));
    locObj->set("hostname", Value::string(new String("")));
    locObj->set("pathname", Value::string(new String("/")));
    locObj->set("protocol", Value::string(new String("https:")));
    vm_->setGlobal("location", Value::object(locObj));
    
    // window dimensions
    vm_->setGlobal("innerWidth", Value::number(1280));
    vm_->setGlobal("innerHeight", Value::number(720));
    
    // 'window' — self-referencing global scope alias
    // Most JS starts with window.addEventListener, window.document, etc.
    auto* windowObj = new Object();
    windowObj->set("innerWidth", Value::number(1280));
    windowObj->set("innerHeight", Value::number(720));
    windowObj->set("navigator", Value::object(navObj));
    windowObj->set("location", Value::object(locObj));
    windowObj->set("console", Value::object(consoleObj));
    windowObj->set("alert", Value::object(alertFn));
    windowObj->set("confirm", Value::object(confirmFn));
    windowObj->set("prompt", Value::object(promptFn));
    windowObj->set("setTimeout", Value::object(setTimeoutFn));
    windowObj->set("setInterval", Value::object(setIntervalFn));
    windowObj->set("clearTimeout", Value::object(clearTimeoutFn));
    windowObj->set("clearInterval", Value::object(clearTimeoutFn));
    
    // window.addEventListener — delegates to ScriptContext event system
    auto* winAddEvent = createNativeFunction("addEventListener",
        [this](Context*, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) return Value::undefined();
            std::string eventType = args[0].toString();
            addEventListener(eventType, []{});
            return Value::undefined();
        }, 2);
    windowObj->set("addEventListener", Value::object(winAddEvent));
    
    // window.removeEventListener (no-op for now)
    auto* winRemoveEvent = createNativeFunction("removeEventListener",
        [](Context*, const std::vector<Value>&) -> Value {
            return Value::undefined();
        }, 2);
    windowObj->set("removeEventListener", Value::object(winRemoveEvent));
    
    // window.getComputedStyle (stub — returns empty style object)
    auto* getComputedStyleFn = createNativeFunction("getComputedStyle",
        [](Context*, const std::vector<Value>&) -> Value {
            auto* styleObj = new Object();
            auto* getPropertyFn = createNativeFunction("getPropertyValue",
                [](Context*, const std::vector<Value>&) -> Value {
                    return Value::string(new String(""));
                }, 1);
            styleObj->set("getPropertyValue", Value::object(getPropertyFn));
            return Value::object(styleObj);
        }, 1);
    windowObj->set("getComputedStyle", Value::object(getComputedStyleFn));
    vm_->setGlobal("getComputedStyle", Value::object(getComputedStyleFn));
    
    // window.requestAnimationFrame(callback) — runs before the next paint
    auto* rafFn = createNativeFunction("requestAnimationFrame",
        [this](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isObject() || !args[0].asObject()->isFunction()) {
                return Value::number(0);
            }
            int id = nextTimerId_++;
            callbackStore_->set("r" + std::to_string(id), args[0]);
            animationFrameQueue_.push_back(id);
            return Value::number(id);
        }, 1);
    windowObj->set("requestAnimationFrame", Value::object(rafFn));

    // performance.now() — milliseconds since this page's script context began
    auto* performanceObj = new Object();
    auto* perfNowFn = createNativeFunction("now",
        [this](Context*, const std::vector<Value>&) -> Value {
            double now = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            return Value::number(now - timeOriginMs_);
        }, 0);
    performanceObj->set("now", Value::object(perfNowFn));
    windowObj->set("performance", Value::object(performanceObj));
    vm_->setGlobal("performance", Value::object(performanceObj));
    vm_->setGlobal("requestAnimationFrame", Value::object(rafFn));
    
    // window.cancelAnimationFrame (no-op)
    auto* cafFn = createNativeFunction("cancelAnimationFrame",
        [this](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::undefined();
            int id = (int)args[0].toNumber();
            animationFrameQueue_.erase(std::remove(animationFrameQueue_.begin(),
                animationFrameQueue_.end(), id), animationFrameQueue_.end());
            removeStoredCallback("r" + std::to_string(id));
            return Value::undefined();
        }, 1);
    windowObj->set("cancelAnimationFrame", Value::object(cafFn));
    vm_->setGlobal("cancelAnimationFrame", Value::object(cafFn));
    
    // self-reference
    windowObj->set("window", Value::object(windowObj));
    windowObj->set("self", Value::object(windowObj));
    windowObj->set("globalThis", Value::object(windowObj));
    
    // Next.js hydration: framework scripts push data chunks via self.__next_f.push(...)
    // Initialize as empty array so the push calls succeed.
    auto* nextFArray = new Object();
    nextFArray->set("length", Value::number(0));
    auto* pushFn = createNativeFunction("push",
        [nextFArray](Context*, const std::vector<Value>& args) -> Value {
            double len = nextFArray->get("length").toNumber();
            for (size_t i = 0; i < args.size(); i++) {
                nextFArray->set(static_cast<size_t>(len + i), args[i]);
            }
            len += args.size();
            nextFArray->set("length", Value::number(len));
            return Value::number(len);
        }, 1);
    nextFArray->set("push", Value::object(pushFn));
    windowObj->set("__next_f", Value::object(nextFArray));
    
    vm_->setGlobal("window", Value::object(windowObj));
    vm_->setGlobal("self", Value::object(windowObj));
    
    // localStorage and sessionStorage: separate in-memory stores (not persisted yet)
    auto* localStorageObj = makeWebStorage();
    auto* sessionStorageObj = makeWebStorage();
    vm_->setGlobal("localStorage", Value::object(localStorageObj));
    vm_->setGlobal("sessionStorage", Value::object(sessionStorageObj));
    windowObj->set("localStorage", Value::object(localStorageObj));
    windowObj->set("sessionStorage", Value::object(sessionStorageObj));
    
    // history object
    auto* historyObj = new Object();
    historyObj->set("length", Value::number(1));
    historyObj->set("state", Value::null());
    
    auto* pushStateFn = createNativeFunction("pushState",
        [](Context*, const std::vector<Value>&) -> Value {
            return Value::undefined();
        }, 3);
    historyObj->set("pushState", Value::object(pushStateFn));
    historyObj->set("replaceState", Value::object(pushStateFn));
    
    auto* histBackFn = createNativeFunction("back",
        [](Context*, const std::vector<Value>&) -> Value {
            return Value::undefined();
        }, 0);
    historyObj->set("back", Value::object(histBackFn));
    historyObj->set("forward", Value::object(histBackFn));
    historyObj->set("go", Value::object(histBackFn));
    
    vm_->setGlobal("history", Value::object(historyObj));
    windowObj->set("history", Value::object(historyObj));
    
    
    // MutationObserver — React uses this to track DOM mutations
    auto* mutObsCtor = createNativeFunction("MutationObserver",
        [](Context*, const std::vector<Value>&) -> Value {
            auto* obs = new Object();
            obs->set("observe", Value::object(createNativeFunction("observe",
                [](Context*, const std::vector<Value>&) -> Value { return Value::undefined(); }, 2)));
            obs->set("disconnect", Value::object(createNativeFunction("disconnect",
                [](Context*, const std::vector<Value>&) -> Value { return Value::undefined(); }, 0)));
            obs->set("takeRecords", Value::object(createNativeFunction("takeRecords",
                [](Context*, const std::vector<Value>&) -> Value {
                    auto* arr = new Object(ObjectType::Array);
                    arr->set("length", Value::number(0));
                    return Value::object(arr);
                }, 0)));
            return Value::object(obs);
        }, 1);
    vm_->setGlobal("MutationObserver", Value::object(mutObsCtor));
    windowObj->set("MutationObserver", Value::object(mutObsCtor));
    
    // IntersectionObserver — lazy loading, viewport detection
    auto* intObsCtor = createNativeFunction("IntersectionObserver",
        [](Context*, const std::vector<Value>&) -> Value {
            auto* obs = new Object();
            obs->set("observe", Value::object(createNativeFunction("observe",
                [](Context*, const std::vector<Value>&) -> Value { return Value::undefined(); }, 1)));
            obs->set("unobserve", Value::object(createNativeFunction("unobserve",
                [](Context*, const std::vector<Value>&) -> Value { return Value::undefined(); }, 1)));
            obs->set("disconnect", Value::object(createNativeFunction("disconnect",
                [](Context*, const std::vector<Value>&) -> Value { return Value::undefined(); }, 0)));
            return Value::object(obs);
        }, 1);
    vm_->setGlobal("IntersectionObserver", Value::object(intObsCtor));
    windowObj->set("IntersectionObserver", Value::object(intObsCtor));
    
    // ResizeObserver — responsive components
    auto* resObsCtor = createNativeFunction("ResizeObserver",
        [](Context*, const std::vector<Value>&) -> Value {
            auto* obs = new Object();
            obs->set("observe", Value::object(createNativeFunction("observe",
                [](Context*, const std::vector<Value>&) -> Value { return Value::undefined(); }, 1)));
            obs->set("unobserve", Value::object(createNativeFunction("unobserve",
                [](Context*, const std::vector<Value>&) -> Value { return Value::undefined(); }, 1)));
            obs->set("disconnect", Value::object(createNativeFunction("disconnect",
                [](Context*, const std::vector<Value>&) -> Value { return Value::undefined(); }, 0)));
            return Value::object(obs);
        }, 1);
    vm_->setGlobal("ResizeObserver", Value::object(resObsCtor));
    windowObj->set("ResizeObserver", Value::object(resObsCtor));
    
    // fetch() — returns a Promise-like object with .then()/.json()/.text()
    // Real network calls happen synchronously via nxhttp for now
    auto* fetchFn = createNativeFunction("fetch",
        [](Context*, const std::vector<Value>& args) -> Value {
            auto* response = new Object();
            response->set("ok", Value::boolean(true));
            response->set("status", Value::number(200));
            response->set("statusText", Value::string(new String("OK")));
            response->set("json", Value::object(createNativeFunction("json",
                [](Context*, const std::vector<Value>&) -> Value {
                    auto* p = new Object();
                    p->set("then", Value::object(createNativeFunction("then",
                        [](Context*, const std::vector<Value>&) -> Value { return Value::undefined(); }, 1)));
                    return Value::object(p);
                }, 0)));
            response->set("text", Value::object(createNativeFunction("text",
                [](Context*, const std::vector<Value>&) -> Value {
                    auto* p = new Object();
                    p->set("then", Value::object(createNativeFunction("then",
                        [](Context*, const std::vector<Value>&) -> Value { return Value::undefined(); }, 1)));
                    return Value::object(p);
                }, 0)));
            
            // Return a "thenable" wrapping the response
            auto* promise = new Object();
            promise->set("then", Value::object(createNativeFunction("then",
                [response](Context*, const std::vector<Value>& args) -> Value {
                    // Call the callback with the response if provided
                    // For now just return the response directly
                    auto* nextP = new Object();
                    nextP->set("then", Value::object(createNativeFunction("then",
                        [](Context*, const std::vector<Value>&) -> Value { return Value::undefined(); }, 1)));
                    nextP->set("catch", Value::object(createNativeFunction("catch",
                        [](Context*, const std::vector<Value>&) -> Value { return Value::undefined(); }, 1)));
                    return Value::object(nextP);
                }, 1)));
            promise->set("catch", Value::object(createNativeFunction("catch",
                [](Context*, const std::vector<Value>&) -> Value { return Value::undefined(); }, 1)));
            return Value::object(promise);
        }, 1);
    vm_->setGlobal("fetch", Value::object(fetchFn));
    windowObj->set("fetch", Value::object(fetchFn));
    
    // URL constructor
    auto* urlCtor = createNativeFunction("URL",
        [](Context*, const std::vector<Value>& args) -> Value {
            auto* urlObj = new Object();
            std::string href = args.empty() ? "" : args[0].toString();
            urlObj->set("href", Value::string(new String(href)));
            // Basic URL parsing
            size_t protoEnd = href.find("://");
            if (protoEnd != std::string::npos) {
                urlObj->set("protocol", Value::string(new String(href.substr(0, protoEnd + 1))));
                size_t hostStart = protoEnd + 3;
                size_t pathStart = href.find('/', hostStart);
                if (pathStart != std::string::npos) {
                    urlObj->set("hostname", Value::string(new String(href.substr(hostStart, pathStart - hostStart))));
                    urlObj->set("pathname", Value::string(new String(href.substr(pathStart))));
                } else {
                    urlObj->set("hostname", Value::string(new String(href.substr(hostStart))));
                    urlObj->set("pathname", Value::string(new String("/")));
                }
            } else {
                urlObj->set("protocol", Value::string(new String("")));
                urlObj->set("hostname", Value::string(new String("")));
                urlObj->set("pathname", Value::string(new String(href)));
            }
            urlObj->set("search", Value::string(new String("")));
            urlObj->set("hash", Value::string(new String("")));
            urlObj->set("origin", Value::string(new String("")));
            urlObj->set("toString", Value::object(createNativeFunction("toString",
                [href](Context*, const std::vector<Value>&) -> Value {
                    return Value::string(new String(href));
                }, 0)));
            return Value::object(urlObj);
        }, 1);
    vm_->setGlobal("URL", Value::object(urlCtor));
    windowObj->set("URL", Value::object(urlCtor));

    // URLSearchParams
    auto* uspCtor = createNativeFunction("URLSearchParams",
        [](Context*, const std::vector<Value>&) -> Value {
            auto* usp = new Object();
            usp->set("get", Value::object(createNativeFunction("get",
                [](Context*, const std::vector<Value>&) -> Value { return Value::null(); }, 1)));
            usp->set("has", Value::object(createNativeFunction("has",
                [](Context*, const std::vector<Value>&) -> Value { return Value::boolean(false); }, 1)));
            usp->set("toString", Value::object(createNativeFunction("toString",
                [](Context*, const std::vector<Value>&) -> Value { return Value::string(new String("")); }, 0)));
            return Value::object(usp);
        }, 0);
    vm_->setGlobal("URLSearchParams", Value::object(uspCtor));
    windowObj->set("URLSearchParams", Value::object(uspCtor));
    
    // window carries the engine's built-ins too (window.Array, window.JSON, ...)
    if (globalObject_) {
        for (const std::string& name : globalObject_->getOwnPropertyNames()) {
            if (name == "globalThis" || windowObj->hasOwnProperty(name)) continue;
            PropertyDescriptor desc;
            desc.value = globalObject_->get(name);
            desc.attributes = PropertyAttribute::Writable | PropertyAttribute::Configurable;
            windowObj->defineProperty(name, desc);
        }
    }

    std::cout << "[ScriptContext] Window globals registered (alert, console, setTimeout, navigator, location, JSON, localStorage, history)" << std::endl;
}

void ScriptContext::setupDocumentGlobals() {
    if (!vm_ || !document_) return;
    
    using namespace Zepra::Runtime;
    
    // Helper: wrap a DOMElement* into a JS Object with properties and methods
    auto wrapElement = [this](DOMElement* elem) -> Value {
        if (!elem) return Value::null();
        
        auto* obj = new Object();
        
        // Properties
        obj->set("tagName", Value::string(new String(elem->tagName())));
        obj->set("id", Value::string(new String(elem->id())));
        obj->set("className", Value::string(new String(elem->className())));
        obj->set("textContent", Value::string(new String(elem->textContent())));
        obj->set("nodeName", Value::string(new String(elem->nodeName())));
        
        // innerHTML (approximation: textContent)
        obj->set("innerHTML", Value::string(new String(elem->textContent())));
        
        // getAttribute(name)
        auto* getAttrFn = createNativeFunction("getAttribute",
            [elem](Context*, const std::vector<Value>& args) -> Value {
                if (args.empty()) return Value::undefined();
                std::string name = args[0].toString();
                return Value::string(new String(elem->getAttribute(name)));
            }, 1);
        obj->set("getAttribute", Value::object(getAttrFn));
        
        // setAttribute(name, value)
        auto* setAttrFn = createNativeFunction("setAttribute",
            [elem](Context*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) return Value::undefined();
                elem->setAttribute(args[0].toString(), args[1].toString());
                return Value::undefined();
            }, 2);
        obj->set("setAttribute", Value::object(setAttrFn));
        
        // style object (property bag — sets CSS inline styles)
        auto* styleObj = new Object();
        obj->set("style", Value::object(styleObj));
        
        // addEventListener (stored on the EventTarget)
        auto* addEventFn = createNativeFunction("addEventListener",
            [](Context*, const std::vector<Value>&) -> Value {
                // Event binding requires callback storage — placeholder for now
                return Value::undefined();
            }, 2);
        obj->set("addEventListener", Value::object(addEventFn));
        
        // children array
        auto* childrenArray = new Object(ObjectType::Array);
        int idx = 0;
        for (const auto& childNode : elem->childNodes()) {
            if (auto* childElem = dynamic_cast<DOMElement*>(childNode.get())) {
                auto* childObj = new Object();
                childObj->set("tagName", Value::string(new String(childElem->tagName())));
                childObj->set("id", Value::string(new String(childElem->id())));
                childObj->set("textContent", Value::string(new String(childElem->textContent())));
                childrenArray->set(idx++, Value::object(childObj));
            }
        }
        obj->set("children", Value::object(childrenArray));
        
        // querySelector on element
        auto wrapElem = [this](DOMElement* e) -> Value {
            if (!e) return Value::null();
            auto* o = new Object();
            o->set("tagName", Value::string(new String(e->tagName())));
            o->set("id", Value::string(new String(e->id())));
            o->set("className", Value::string(new String(e->className())));
            o->set("textContent", Value::string(new String(e->textContent())));
            return Value::object(o);
        };
        
        auto* qsFn = createNativeFunction("querySelector",
            [elem, wrapElem](Context*, const std::vector<Value>& args) -> Value {
                if (args.empty()) return Value::null();
                return wrapElem(elem->querySelector(args[0].toString()));
            }, 1);
        obj->set("querySelector", Value::object(qsFn));
        
        // classList object
        auto* classListObj = new Object();
        std::string cls = elem->className();
        classListObj->set("value", Value::string(new String(cls)));
        
        auto* clAddFn = createNativeFunction("add",
            [elem](Context*, const std::vector<Value>& args) -> Value {
                if (!args.empty()) {
                    std::string current = elem->className();
                    if (!current.empty()) current += " ";
                    current += args[0].toString();
                    elem->setAttribute("class", current);
                }
                return Value::undefined();
            }, 1);
        classListObj->set("add", Value::object(clAddFn));
        
        auto* clRemoveFn = createNativeFunction("remove",
            [](Context*, const std::vector<Value>&) -> Value {
                return Value::undefined();
            }, 1);
        classListObj->set("remove", Value::object(clRemoveFn));
        
        auto* clContainsFn = createNativeFunction("contains",
            [elem](Context*, const std::vector<Value>& args) -> Value {
                if (args.empty()) return Value::boolean(false);
                std::string cls = elem->className();
                return Value::boolean(cls.find(args[0].toString()) != std::string::npos);
            }, 1);
        classListObj->set("contains", Value::object(clContainsFn));
        
        auto* clToggleFn = createNativeFunction("toggle",
            [](Context*, const std::vector<Value>&) -> Value {
                return Value::boolean(false);
            }, 1);
        classListObj->set("toggle", Value::object(clToggleFn));
        obj->set("classList", Value::object(classListObj));
        
        // parentNode / parentElement
        DOMNode* parentNode = elem->parentNode();
        DOMElement* parent = parentNode ? dynamic_cast<DOMElement*>(parentNode) : nullptr;
        if (parent) {
            auto* parentObj = new Object();
            parentObj->set("tagName", Value::string(new String(parent->tagName())));
            parentObj->set("id", Value::string(new String(parent->id())));
            parentObj->set("className", Value::string(new String(parent->className())));
            obj->set("parentNode", Value::object(parentObj));
            obj->set("parentElement", Value::object(parentObj));
        } else {
            obj->set("parentNode", Value::null());
            obj->set("parentElement", Value::null());
        }
        
        // appendChild / removeChild (stubs for mutation)
        auto* appendChildFn = createNativeFunction("appendChild",
            [](Context*, const std::vector<Value>& args) -> Value {
                return args.empty() ? Value::undefined() : args[0];
            }, 1);
        obj->set("appendChild", Value::object(appendChildFn));
        
        auto* removeChildFn = createNativeFunction("removeChild",
            [](Context*, const std::vector<Value>& args) -> Value {
                return args.empty() ? Value::undefined() : args[0];
            }, 1);
        obj->set("removeChild", Value::object(removeChildFn));
        
        // getBoundingClientRect
        auto* getBCRFn = createNativeFunction("getBoundingClientRect",
            [](Context*, const std::vector<Value>&) -> Value {
                auto* rect = new Object();
                rect->set("x", Value::number(0));
                rect->set("y", Value::number(0));
                rect->set("width", Value::number(0));
                rect->set("height", Value::number(0));
                rect->set("top", Value::number(0));
                rect->set("left", Value::number(0));
                rect->set("right", Value::number(0));
                rect->set("bottom", Value::number(0));
                return Value::object(rect);
            }, 0);
        obj->set("getBoundingClientRect", Value::object(getBCRFn));
        
        return Value::object(obj);
    };
    
    // Create 'document' object
    auto* docObj = new Object();
    DOMDocument* doc = document_;
    
    // document.getElementById(id)
    auto* getById = createNativeFunction("getElementById",
        [doc, wrapElement](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::null();
            return wrapElement(doc->getElementById(args[0].toString()));
        }, 1);
    docObj->set("getElementById", Value::object(getById));
    
    // document.querySelector(selector)
    auto* querySelector = createNativeFunction("querySelector",
        [doc, wrapElement](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::null();
            return wrapElement(doc->querySelector(args[0].toString()));
        }, 1);
    docObj->set("querySelector", Value::object(querySelector));
    
    // document.querySelectorAll(selector) → returns array
    auto* querySelectorAll = createNativeFunction("querySelectorAll",
        [doc, wrapElement](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::object(new Object(ObjectType::Array));
            auto elems = doc->querySelectorAll(args[0].toString());
            auto* arr = new Object(ObjectType::Array);
            for (size_t i = 0; i < elems.size(); i++) {
                arr->set(i, wrapElement(elems[i]));
            }
            return Value::object(arr);
        }, 1);
    docObj->set("querySelectorAll", Value::object(querySelectorAll));
    
    // document.createElement(tagName)
    auto* createElement = createNativeFunction("createElement",
        [doc](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::null();
            auto elem = doc->createElement(args[0].toString());
            if (!elem) return Value::null();
            auto* obj = new Object();
            obj->set("tagName", Value::string(new String(elem->tagName())));
            return Value::object(obj);
        }, 1);
    docObj->set("createElement", Value::object(createElement));
    
    // document.createTextNode(data)
    auto* createTextNode = createNativeFunction("createTextNode",
        [doc](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::null();
            auto* obj = new Object();
            obj->set("nodeType", Value::number(3));
            obj->set("textContent", Value::string(new String(args[0].toString())));
            return Value::object(obj);
        }, 1);
    docObj->set("createTextNode", Value::object(createTextNode));
    
    // document.body
    DOMElement* bodyElem = doc->body();
    docObj->set("body", wrapElement(bodyElem));
    
    // document.documentElement
    DOMElement* htmlElem = doc->documentElement();
    docObj->set("documentElement", wrapElement(htmlElem));
    
    // document.head
    DOMElement* headElem = doc->head();
    docObj->set("head", wrapElement(headElem));
    
    // document.title
    docObj->set("title", Value::string(new String(doc->title())));
    
    // document.addEventListener
    auto* docAddEvent = createNativeFunction("addEventListener",
        [this](Context*, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) return Value::undefined();
            std::string eventType = args[0].toString();
            addEventListener(eventType, []{});
            return Value::undefined();
        }, 2);
    docObj->set("addEventListener", Value::object(docAddEvent));
    
    // document.getElementsByTagName
    auto* getByTag = createNativeFunction("getElementsByTagName",
        [doc, wrapElement](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::object(new Object(ObjectType::Array));
            auto elems = doc->querySelectorAll(args[0].toString());
            auto* arr = new Object(ObjectType::Array);
            for (size_t i = 0; i < elems.size(); i++) {
                arr->set(i, wrapElement(elems[i]));
            }
            arr->set("length", Value::number((double)elems.size()));
            return Value::object(arr);
        }, 1);
    docObj->set("getElementsByTagName", Value::object(getByTag));
    
    // document.getElementsByClassName
    auto* getByClass = createNativeFunction("getElementsByClassName",
        [doc, wrapElement](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value::object(new Object(ObjectType::Array));
            std::string selector = "." + args[0].toString();
            auto elems = doc->querySelectorAll(selector);
            auto* arr = new Object(ObjectType::Array);
            for (size_t i = 0; i < elems.size(); i++) {
                arr->set(i, wrapElement(elems[i]));
            }
            arr->set("length", Value::number((double)elems.size()));
            return Value::object(arr);
        }, 1);
    docObj->set("getElementsByClassName", Value::object(getByClass));
    
    // document.cookie (empty string)
    docObj->set("cookie", Value::string(new String("")));
    
    // document.readyState
    docObj->set("readyState", Value::string(new String("complete")));
    
    // document.URL / document.location
    docObj->set("URL", Value::string(new String("")));
    
    // document.createDocumentFragment
    auto* createFragFn = createNativeFunction("createDocumentFragment",
        [](Context*, const std::vector<Value>&) -> Value {
            auto* frag = new Object();
            frag->set("nodeType", Value::number(11));
            auto* appendChildFn = createNativeFunction("appendChild",
                [](Context*, const std::vector<Value>& args) -> Value {
                    return args.empty() ? Value::undefined() : args[0];
                }, 1);
            frag->set("appendChild", Value::object(appendChildFn));
            return Value::object(frag);
        }, 0);
    docObj->set("createDocumentFragment", Value::object(createFragFn));
    
    // Wire window.document
    vm_->setGlobal("__document_ref__", Value::object(docObj));
    
    // Register document global
    vm_->setGlobal("document", Value::object(docObj));
    
    // Patch window object with document reference
    // (window was created in setupWindowGlobals, now we add document to it)
    auto windowVal = vm_->getGlobal("window");
    if (windowVal.isObject()) {
        windowVal.asObject()->set("document", Value::object(docObj));
    }
    
    std::cout << "[ScriptContext] DOM API registered (document.getElementById, querySelector, createElement, body, title)" << std::endl;
}

// ============================================================================
// DevToolsConsole
// ============================================================================

DevToolsConsole::DevToolsConsole() {}

void DevToolsConsole::log(const std::string& message, const std::string& source, int line) {
    addEntry("log", message, source, line);
}

void DevToolsConsole::clear() {
    entries_.clear();
}

void DevToolsConsole::addEntry(const std::string& level, const std::string& message, 
                                const std::string& source, int line) {
    LogEntry entry;
    entry.level = level;
    entry.message = message;
    entry.source = source;
    entry.line = line;
    entry.timestamp = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    entries_.push_back(entry);
    
    if (entries_.size() > maxEntries_) {
        entries_.erase(entries_.begin());
    }
    if (onLog_) {
        onLog_(entry);
    }
}

// ============================================================================
// DevToolsPanel
// ============================================================================

DevToolsPanel::DevToolsPanel() {}

} // namespace Zepra::WebCore
