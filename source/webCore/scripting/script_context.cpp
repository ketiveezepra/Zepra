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
#include "scripting/canvas_bindings.hpp"
#include "scripting/dom_bindings.hpp"
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
    // Per-instance VM destroyed automatically by unique_ptr (after the canvases)
}

void ScriptContext::initialize(DOMDocument* document) {
    document_ = document;
    setupGlobals();
}

ScriptResult ScriptContext::evaluate(const std::string& code, const std::string& filename) {
    return run(code, filename, nullptr);
}

ScriptResult ScriptContext::run(const std::string& code, const std::string& filename, Zepra::Runtime::Value* value) {
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
            if (value) *value = execResult.value;
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
    processNetworkResponses();
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
    runTask([&] { vm_->call(fn, Value::undefined(), args); }, "callback");
}

void ScriptContext::runTask(const std::function<void()>& task, const char* what) {
    // The task's microtasks run right after it, under the same limits, even
    // when it threw. A run stopped by the limits takes its queued microtasks
    // with it.
    ScriptRunGuard guard(vm_);
    try {
        task();
    } catch (const std::exception& e) {
        if (consoleHandler_) consoleHandler_("error", std::string("Uncaught exception in ") + what + ": " + e.what());
        if (vm_->isTerminationRequested()) {
            vm_->discardJobs();
            return;
        }
    } catch (...) {
        if (consoleHandler_) consoleHandler_("error", std::string("Uncaught exception in ") + what);
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
    if (vm_ && domBindings_) {
        domBindings_->setReadyState("interactive");
        runTask([this] { domBindings_->fireEvent(document_, "DOMContentLoaded", true, false); }, "DOMContentLoaded listener");
    }
    for (auto& listener : domContentLoadedListeners_) {
        if (listener) listener();
    }
}

void ScriptContext::fireLoadEvent() {
    if (vm_ && domBindings_) {
        domBindings_->setReadyState("complete");
        runTask([this] { domBindings_->fireEvent(nullptr, "load", false, false); }, "load listener");
    }
    for (auto& listener : loadListeners_) {
        if (listener) listener();
    }
}

bool ScriptContext::dispatchMouseEvent(DOMElement* target, const std::string& type, double clientX, double clientY) {
    bool notCanceled = true;
    if (vm_ && domBindings_ && target) {
        runTask([&] { notCanceled = domBindings_->fireMouseEvent(target, type, clientX, clientY); }, "event listener");
    }
    return notCanceled;
}

void ScriptContext::releaseDetachedNodes() {
    if (domBindings_) domBindings_->releaseDetachedNodes();
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
    if (vm_) {
        // The window object and the DOM interfaces (EventTarget, Node, Event, ...)
        domBindings_ = std::make_unique<DOMBindings>(vm_, [this](const std::string& message) {
            if (consoleHandler_) consoleHandler_("error", message);
            else std::cerr << "[console.error] " << message << std::endl;
        });
    }
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
    // (an EventTarget: addEventListener comes from its prototype)
    auto* windowObj = domBindings_->window();
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
    
    // fetch(), XMLHttpRequest and Headers (fetch_bindings.cpp)
    setupFetchGlobals(windowObj);
    
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

    // <canvas> contexts and Path2D
    canvasRegistry_ = std::make_unique<CanvasRegistry>(vm_, windowObj);

    std::cout << "[ScriptContext] Window globals registered (alert, console, setTimeout, navigator, location, JSON, localStorage, history)" << std::endl;
}

void ScriptContext::setupDocumentGlobals() {
    if (!vm_ || !domBindings_) return;

    using namespace Zepra::Runtime;

    // document and every node reached from it are live wrappers of the C++
    // DOM (scripting/dom_bindings.cpp); <canvas> wrappers get getContext()
    domBindings_->setCanvases(canvasRegistry_.get());
    // on<event>="..." attributes compile to functions on first dispatch
    domBindings_->setFunctionCompiler([this](const std::string& source, Value& out) {
        return run(source, "<event handler>", &out).success;
    });
    domBindings_->installDocument(document_);
    if (document_) vm_->setGlobal("__document_ref__", vm_->getGlobal("document"));
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
