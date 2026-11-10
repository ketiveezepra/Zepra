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
#include "scripting/web_storage.hpp"
#include <iostream>
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <memory>
#include <unordered_map>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <random>

// ZepraScript internal headers
#include "runtime/execution/vm.hpp"
#include "runtime/execution/js_error.hpp"
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

// A Storage object (localStorage / sessionStorage): getItem, setItem,
// removeItem, clear, key(n) and length over the area `area()` returns (looked
// up on each call: the page's origin can change before its scripts run)
Zepra::Runtime::Object* makeWebStorage(Zepra::Runtime::VM* vm, std::function<StorageArea*()> area) {
    using namespace Zepra::Runtime;
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
    auto needs = [](const std::vector<Value>& args, size_t count, const char* name) {
        if (args.size() < count) {
            throw JSError("TypeError", std::string("Failed to execute '") + name + "' on 'Storage': " +
                                       std::to_string(count) + " argument" + (count > 1 ? "s" : "") +
                                       " required, but only " + std::to_string(args.size()) + " present.");
        }
    };

    method("getItem", 1, [area, text, needs](Context*, const std::vector<Value>& args) -> Value {
        needs(args, 1, "getItem");
        auto value = area()->getItem(text(args, 0));
        return value ? Value::string(new String(*value)) : Value::null();
    });
    method("setItem", 2, [vm, area, text, needs](Context*, const std::vector<Value>& args) -> Value {
        needs(args, 2, "setItem");
        const std::string key = text(args, 0);
        if (!area()->setItem(key, text(args, 1))) {
            Value error = vm->newError("Error", "Failed to execute 'setItem' on 'Storage': Setting the value of '" +
                                                    key + "' exceeded the quota.");
            if (error.isObject()) {
                error.asObject()->set("name", Value::string(new String("QuotaExceededError")));
                error.asObject()->set("code", Value::number(22));
            }
            throw ThrownException(error);
        }
        return Value::undefined();
    });
    method("removeItem", 1, [area, text, needs](Context*, const std::vector<Value>& args) -> Value {
        needs(args, 1, "removeItem");
        area()->removeItem(text(args, 0));
        return Value::undefined();
    });
    method("clear", 0, [area](Context*, const std::vector<Value>&) -> Value {
        area()->clear();
        return Value::undefined();
    });
    method("key", 1, [area, needs](Context*, const std::vector<Value>& args) -> Value {
        needs(args, 1, "key");
        const double index = args[0].toNumber();
        if (!(index >= 0) || index >= 4294967296.0) return Value::null();
        auto key = area()->key(static_cast<size_t>(index));
        return key ? Value::string(new String(*key)) : Value::null();
    });

    PropertyDescriptor length;
    length.getter = Value::object(createNativeFunction("length", [area](Context*, const std::vector<Value>&) -> Value {
        return Value::number(static_cast<double>(area()->length()));
    }, 0));
    length.attributes = PropertyAttribute::Configurable;
    storage->defineProperty("length", length);
    return storage;
}
} // namespace

extern const char* const kPlatformShims;

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
        vm_->setErrorReporter([this](const std::string& message, const Zepra::Runtime::Value& thrown) {
            reportThrown(message, thrown);
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

ScriptResult ScriptContext::evaluate(const std::string& code, const std::string& filename, uint32_t firstLine) {
    return run(code, filename, nullptr, nullptr, firstLine);
}

namespace {

// "12:5: message" (the parser's form) -> line 12, column 5, "message"
bool splitPosition(const std::string& text, uint32_t& line, uint32_t& column, std::string& message) {
    size_t a = text.find(':'), b = a == std::string::npos ? a : text.find(':', a + 1);
    if (b == std::string::npos || a == 0) return false;
    auto digits = [&](size_t from, size_t to) {
        return to > from && std::all_of(text.begin() + (long)from, text.begin() + (long)to,
                                         [](unsigned char c) { return std::isdigit(c); });
    };
    if (!digits(0, a) || !digits(a + 1, b)) return false;
    line = (uint32_t)std::stoul(text.substr(0, a));
    column = (uint32_t)std::stoul(text.substr(a + 1, b - a - 1));
    size_t msg = b + 1;
    while (msg < text.size() && text[msg] == ' ') msg++;
    message = text.substr(msg);
    return true;
}

// The first frame of an error's stack, "    at f (file:line)" -> file, line
bool firstStackFrame(const std::string& stack, std::string& file, uint32_t& line) {
    for (size_t pos = 0; pos < stack.size();) {
        size_t end = stack.find('\n', pos);
        std::string l = stack.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? stack.size() : end + 1;
        if (l.find("    at ") != 0 || l.empty() || l.back() != ')') continue;
        size_t open = l.rfind(" (");
        size_t colon = l.rfind(':');
        if (open == std::string::npos || colon == std::string::npos || colon < open) return false;
        std::string number = l.substr(colon + 1, l.size() - colon - 2);
        if (number.empty() || !std::all_of(number.begin(), number.end(), [](unsigned char c) { return std::isdigit(c); }))
            return false;
        file = l.substr(open + 2, colon - open - 2);
        line = (uint32_t)std::stoul(number);
        return true;
    }
    return false;
}

std::string positionText(const std::string& file, uint32_t line, uint32_t column) {
    std::string s = file + ":" + std::to_string(line);
    if (column) s += ":" + std::to_string(column);
    return s;
}

} // namespace

void ScriptContext::reportError(const std::string& text, const std::string& file, uint32_t line, uint32_t column) {
    if (quietErrors_) return;
    ConsoleMessage m;
    m.level = "error";
    m.text = text;
    m.depth = consoleGroupDepth_;
    if (line && !file.empty()) {
        m.source = positionText(file, line, column);
        m.sourceUrl = file;
        m.sourceLine = line;
    }
    emitConsole(std::move(m));
}

void ScriptContext::reportThrown(const std::string& text, const Zepra::Runtime::Value& thrown) {
    // An Error carries the stack of where it was made: its first frame is the link
    std::string file;
    uint32_t line = 0;
    if (thrown.isObject()) {
        Zepra::Runtime::Value stack = thrown.asObject()->get("stack");
        if (stack.isString()) {
            firstStackFrame(stack.toString(), file, line);
            // ZEPRA_JS_STACKS=1: uncaught errors' stacks on stderr (the
            // console panel only keeps the first frame)
            static const bool printStacks = std::getenv("ZEPRA_JS_STACKS") != nullptr;
            if (printStacks) std::cerr << "[JS stack] " << stack.toString() << std::endl;
        }
    }
    reportError(text, file, line);
}

void ScriptContext::setCallerSource(ConsoleMessage& message) const {
    if (!vm_) return;
    auto frames = vm_->callStack(1);
    if (frames.empty() || !frames[0].line || frames[0].file.empty()) return;
    message.sourceUrl = frames[0].file;
    message.sourceLine = frames[0].line;
    message.source = positionText(message.sourceUrl, message.sourceLine, 0);
}

void ScriptContext::registerSource(const std::string& name, const std::string& text) {
    if (name.empty() || text.size() > kSourceBudget) return;
    for (auto it = sources_.begin(); it != sources_.end(); ++it) {
        if (it->first == name) {
            sourceBytes_ -= it->second->size();
            sources_.erase(it);
            break;
        }
    }
    while (!sources_.empty() && sourceBytes_ + text.size() > kSourceBudget) {
        sourceBytes_ -= sources_.front().second->size();
        sources_.erase(sources_.begin());
    }
    sources_.emplace_back(name, std::make_shared<const std::string>(text));
    sourceBytes_ += text.size();
}

std::shared_ptr<const std::string> ScriptContext::sourceText(const std::string& name) const {
    for (const auto& [n, text] : sources_) if (n == name) return text;
    return nullptr;
}

std::vector<std::string> ScriptContext::sourceNames() const {
    std::vector<std::string> names;
    for (const auto& entry : sources_) names.push_back(entry.first);
    return names;
}

ScriptResult ScriptContext::run(const std::string& code, const std::string& filename, Zepra::Runtime::Value* value,
                                Zepra::Runtime::Value* thrown, uint32_t firstLine) {
    const std::string name = filename.empty() ? "<script>" : filename;
    if (firstLine == 0) firstLine = 1;
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
    
    // Skip only absurdly large scripts. Real app bundles are 1–4 MB
    // (YouTube's base module is 1.2 MB, its ytBootstrapConfig inline
    // script 760 KB); the parser handles ~40 MB/s, so they fit the 500 ms
    // parse budget below with room to spare.
    static constexpr size_t MAX_SCRIPT_SIZE = size_t(16) * 1024 * 1024; // 16MB
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
        auto sourceCode = Zepra::Frontend::SourceCode::fromString(code, name);
        
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
            uint32_t line = 0, column = 0;
            std::string message;
            if (splitPosition(parser.errors().front(), line, column, message)) {
                reportError("SyntaxError: " + message, name, line + firstLine - 1, column);
            } else {
                reportError(result.error, "", 0);
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
                if (!quietErrors_) emitConsole("error", result.error);
                return result;
            }
        }
        
        Zepra::Bytecode::BytecodeGenerator generator;
        generator.setSource(name, firstLine);
        auto chunk = generator.compile(ast.get());
        
        if (generator.hasErrors() || !chunk) {
            result.success = false;
            result.error = generator.errors().empty()
                ? std::string("SyntaxError: Bytecode generation failed")
                : "SyntaxError: " + generator.errors().front();
            if (!quietErrors_) emitConsole("error", result.error);
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
            bool exception = execResult.status == Zepra::Runtime::ExecutionResult::Status::Exception;
            if (thrown && exception) *thrown = execResult.value;
            reportThrown(execResult.error, exception ? execResult.value : Zepra::Runtime::Value::undefined());
        }
    } catch (const std::exception& e) {
        result.success = false;
        result.error = std::string("RuntimeError: ") + e.what();
        if (!quietErrors_) emitConsole("error", result.error);
    }
    
    return result;
}

void ScriptContext::log(const std::string& message) {
    emitConsole("log", message);
}

void ScriptContext::emitConsole(const std::string& level, const std::string& text) {
    ConsoleMessage m;
    m.level = level;
    m.text = text;
    m.depth = consoleGroupDepth_;
    emitConsole(std::move(m));
}

void ScriptContext::emitConsole(ConsoleMessage message) {
    // ZEPRA_JS_STACKS=1 also copies console errors and warnings to stderr
    static const bool echoErrors = std::getenv("ZEPRA_JS_STACKS") != nullptr;
    if (echoErrors && (message.level == "error" || message.level == "warn")) {
        std::cerr << "[console." << message.level << "] " << message.text.substr(0, 2000) << std::endl;
    }
    if (consoleMessageHandler_) {
        consoleMessageHandler_(message);
        return;
    }
    if (message.level == "clear") return;
    if (consoleHandler_) {
        consoleHandler_(message.level, message.text);
        return;
    }
    std::ostream& out = (message.level == "error" || message.level == "warn") ? std::cerr : std::cout;
    out << "[console." << message.level << "] " << message.text << std::endl;
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
    // Scripts whose source arrived run before the timers that came due
    runReadyScripts();
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
    runMessageTasks();
}

void ScriptContext::runAnimationFrames() {
    // Intersection/resize observations are taken once per frame
    deliverObservations();
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

void ScriptContext::evaluateMediaQueries() {
    if (!vm_ || !domBindings_) return;
    runTask([this] { domBindings_->evaluateMediaQueries(); }, "media query listener");
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
    } catch (const Zepra::Runtime::ThrownException& e) {
        reportThrown(std::string("Uncaught exception in ") + what + ": " + e.what(), e.value());
    } catch (const std::exception& e) {
        emitConsole("error", std::string("Uncaught exception in ") + what + ": " + e.what());
        if (vm_->isTerminationRequested()) {
            vm_->discardJobs();
            return;
        }
    } catch (...) {
        emitConsole("error", std::string("Uncaught exception in ") + what);
    }
    try {
        vm_->runMicrotasks();
    } catch (const std::exception& e) {
        emitConsole("error", std::string("Microtasks stopped: ") + e.what());
    }
}

void ScriptContext::removeStoredCallback(const std::string& key) {
    if (callbackStore_) callbackStore_->deleteProperty(key);
}

void ScriptContext::setPageUrl(const std::string& url) {
    storageOrigin_ = storageOriginOf(url);
    pageUrl_ = url.empty() ? "about:blank" : url;
    setGlobal("__pageUrl__", url);
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
    // The load event waits for the scripts still loading (async ones and
    // those other scripts inserted): runReadyScripts() fires it after the last
    if (pendingScriptCount() > 0) {
        loadEventDelayed_ = true;
        return;
    }
    fireLoadEventNow();
}

void ScriptContext::fireLoadEventNow() {
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

void ScriptContext::setPageGeometry(PageGeometry* geometry) {
    if (domBindings_) domBindings_->setPageGeometry(geometry);
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

void ScriptContext::setComputedStyleSource(std::function<const CSSComputedStyle*(DOMElement*)> source) {
    computedStyleSource_ = std::move(source);
    if (domBindings_) domBindings_->setComputedStyleSource(computedStyleSource_);
}

void ScriptContext::setPseudoElementStyleSource(
    std::function<const CSSComputedStyle*(DOMElement*, const std::string&)> source) {
    pseudoStyleSource_ = std::move(source);
    if (domBindings_) domBindings_->setPseudoElementStyleSource(pseudoStyleSource_);
}

void ScriptContext::setupGlobals() {
    if (vm_) {
        // The window object and the DOM interfaces (EventTarget, Node, Event, ...)
        domBindings_ = std::make_unique<DOMBindings>(vm_, [this](const std::string& message) {
            emitConsole("error", message);
        });
        domBindings_->setComputedStyleSource(computedStyleSource_);
        domBindings_->setPseudoElementStyleSource(pseudoStyleSource_);
        domBindings_->setSubmitHandler([this](const FormSubmission& submission) {
            if (submitHandler_) submitHandler_(submission);
        });
        // window is the global object: `var x` at script level is window.x,
        // and window.x = 1 makes a bare x readable (UMD bundles, jQuery's $,
        // dataLayer, ...)
        vm_->setGlobalObject(domBindings_->window());
    }
    setupWindowGlobals();
    setupDocumentGlobals();
    // Needs both window and document (kPlatformShims)
    ScriptResult shims = evaluate(kPlatformShims, "<zepra-platform-shims>");
    if (!shims.success) {
        std::cerr << "[ScriptContext] platform shims failed to install: " << shims.error << std::endl;
    }
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
    
    // The console: every method formats its arguments as the DevTools panel
    // shows them (see console_format.hpp). The handler is looked up on every
    // call, so one installed (or replaced) after initialize() still gets output.
    auto* consoleObj = new Object();
    setupConsole(consoleObj);
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
    
    // 'window' — self-referencing global scope alias
    // Most JS starts with window.addEventListener, window.document, etc.
    // (an EventTarget: addEventListener comes from its prototype)
    auto* windowObj = domBindings_->window();
    windowObj->set("navigator", Value::object(navObj));
    setupLocationAndHistory(windowObj);
    windowObj->set("console", Value::object(consoleObj));
    windowObj->set("alert", Value::object(alertFn));
    windowObj->set("confirm", Value::object(confirmFn));
    windowObj->set("prompt", Value::object(promptFn));
    windowObj->set("setTimeout", Value::object(setTimeoutFn));
    windowObj->set("setInterval", Value::object(setIntervalFn));
    windowObj->set("clearTimeout", Value::object(clearTimeoutFn));
    windowObj->set("clearInterval", Value::object(clearTimeoutFn));
    
    // window.getComputedStyle comes with the DOM bindings (setComputedStyleSource)
    
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
    // performance.timeOrigin and the legacy performance.timing /
    // performance.navigation (YouTube's ytcsi reads timing.responseStart on
    // load and threw "Cannot read properties of undefined")
    {
        const double epochOriginMs = std::chrono::duration<double, std::milli>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        performanceObj->set("timeOrigin", Value::number(epochOriginMs));
        auto* timing = new Object();
        const double t = std::floor(epochOriginMs);
        for (const char* key : {"navigationStart", "fetchStart", "domainLookupStart", "domainLookupEnd",
                                "connectStart", "connectEnd", "requestStart", "responseStart",
                                "responseEnd", "domLoading", "domInteractive",
                                "domContentLoadedEventStart", "domContentLoadedEventEnd",
                                "domComplete", "loadEventStart", "loadEventEnd"}) {
            timing->set(key, Value::number(t));
        }
        for (const char* key : {"unloadEventStart", "unloadEventEnd", "redirectStart", "redirectEnd",
                                "secureConnectionStart"}) {
            timing->set(key, Value::number(0));
        }
        performanceObj->set("timing", Value::object(timing));
        auto* navigation = new Object();
        navigation->set("type", Value::number(0));
        navigation->set("redirectCount", Value::number(0));
        performanceObj->set("navigation", Value::object(navigation));
        // User Timing / Performance Timeline: accepted, nothing recorded
        auto emptyList = [](Context*, const std::vector<Value>&) -> Value {
            return Value::object(new Array());
        };
        auto noop = [](Context*, const std::vector<Value>&) -> Value { return Value::undefined(); };
        performanceObj->set("mark", Value::object(createNativeFunction("mark", noop, 1)));
        performanceObj->set("measure", Value::object(createNativeFunction("measure", noop, 1)));
        performanceObj->set("clearMarks", Value::object(createNativeFunction("clearMarks", noop, 0)));
        performanceObj->set("clearMeasures", Value::object(createNativeFunction("clearMeasures", noop, 0)));
        performanceObj->set("clearResourceTimings", Value::object(createNativeFunction("clearResourceTimings", noop, 0)));
        performanceObj->set("setResourceTimingBufferSize", Value::object(createNativeFunction("setResourceTimingBufferSize", noop, 1)));
        performanceObj->set("getEntries", Value::object(createNativeFunction("getEntries", emptyList, 0)));
        performanceObj->set("getEntriesByType", Value::object(createNativeFunction("getEntriesByType", emptyList, 1)));
        performanceObj->set("getEntriesByName", Value::object(createNativeFunction("getEntriesByName", emptyList, 1)));
    }
    windowObj->set("performance", Value::object(performanceObj));
    vm_->setGlobal("performance", Value::object(performanceObj));

    // window.crypto: getRandomValues(typedArray) and randomUUID()
    // (Cloudflare's beacon.min.js threw "crypto is not defined")
    {
        auto* cryptoObj = new Object();
        auto* getRandomValues = createNativeFunction("getRandomValues",
            [](Context*, const std::vector<Value>& args) -> Value {
                if (args.empty() || !args[0].isObject()) {
                    throw JSError("TypeError", "Failed to execute 'getRandomValues' on 'Crypto': parameter 1 is not of type 'ArrayBufferView'.");
                }
                Object* view = args[0].asObject();
                const double length = view->get("length").toNumber();
                if (!(length >= 0) || length > 65536) {
                    throw JSError("QuotaExceededError", "Failed to execute 'getRandomValues' on 'Crypto': The ArrayBufferView's byte length exceeds the number of bytes of entropy available via this API (65536).");
                }
                const double bytesPerElement = view->get("BYTES_PER_ELEMENT").toNumber();
                const double maxValue = bytesPerElement >= 4 ? 4294967296.0 : bytesPerElement == 2 ? 65536.0 : 256.0;
                std::random_device rd;
                for (size_t i = 0; i < static_cast<size_t>(length); ++i) {
                    const double r = std::fmod(static_cast<double>(rd()), maxValue);
                    view->set(std::to_string(i), Value::number(r));
                }
                return args[0];
            }, 1);
        cryptoObj->set("getRandomValues", Value::object(getRandomValues));
        auto* randomUUID = createNativeFunction("randomUUID",
            [](Context*, const std::vector<Value>&) -> Value {
                std::random_device rd;
                unsigned char bytes[16];
                for (int i = 0; i < 16; ++i) bytes[i] = static_cast<unsigned char>(rd() & 0xFF);
                bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);  // version 4
                bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);  // variant 10
                char text[37];
                std::snprintf(text, sizeof(text),
                              "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                              bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                              bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
                return Value::string(new String(text));
            }, 0);
        cryptoObj->set("randomUUID", Value::object(randomUUID));
        windowObj->set("crypto", Value::object(cryptoObj));
        vm_->setGlobal("crypto", Value::object(cryptoObj));
    }

    // window.devicePixelRatio: the page is rendered 1:1
    windowObj->set("devicePixelRatio", Value::number(1));
    vm_->setGlobal("devicePixelRatio", Value::number(1));
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
    
    // localStorage: the origin's area, kept on disk by the browser
    // (setPageUrl picks the origin); sessionStorage: this page's own, in memory
    opaqueStorage_ = std::make_unique<StorageArea>();
    auto* localStorageObj = makeWebStorage(vm_, [this]() -> StorageArea* {
        StorageArea* area = localStorageFor(storageOrigin_);
        return area ? area : opaqueStorage_.get();
    });
    auto sessionArea = std::make_shared<StorageArea>();
    auto* sessionStorageObj = makeWebStorage(vm_, [sessionArea]() { return sessionArea.get(); });
    vm_->setGlobal("localStorage", Value::object(localStorageObj));
    vm_->setGlobal("sessionStorage", Value::object(sessionStorageObj));
    windowObj->set("localStorage", Value::object(localStorageObj));
    windowObj->set("sessionStorage", Value::object(sessionStorageObj));
    
    // history: with location (setupLocationAndHistory)
    
    
    // MutationObserver is live (scripting/mutation_observer.cpp, set up by DOMBindings)

    // IntersectionObserver, ResizeObserver, MessageChannel: installWebTaskApis()

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

    // DataTransfer (drag and drop events carry one)
    setupDragAndDrop(windowObj);

    installWebTaskApis(windowObj);

    std::cout << "[ScriptContext] Window globals registered (alert, console, setTimeout, navigator, location, JSON, localStorage, history)" << std::endl;
}

namespace {

// MessageChannel/MessagePort, window.postMessage, IntersectionObserver and
// ResizeObserver, written in script on two native hooks:
//  - __zepraPostTask(fn) queues fn as a task (drained by processTimers()
//    within a time budget, so a scheduler that posts itself a message per
//    slice, like React's, gets several slices per frame, unclamped)
//  - __zepraDeliverObservations() is called by runAnimationFrames() every
//    frame: observers measure their targets with getBoundingClientRect()
//    against the viewport (or their root) and call back on changes. A newly
//    observed target is always reported on the next frame (IO: whether it
//    intersects; RO: its size), as the specs' initial observation requires.
const char* const kWebTaskApis = R"JS(
var MessagePort, MessageChannel, IntersectionObserver, IntersectionObserverEntry, ResizeObserver, ResizeObserverEntry;
(function () {
  var post = __zepraPostTask;
  var clone = function (d) { try { return structuredClone(d); } catch (e) { return d; } };

  MessagePort = function MessagePort() {
    this.__other = null; this.__listeners = []; this.__started = false;
    this.__queue = []; this.__onmessage = null; this.__closed = false;
  };
  Object.defineProperty(MessagePort.prototype, 'onmessage', {
    get: function () { return this.__onmessage; },
    set: function (f) { this.__onmessage = f; this.start(); },
    configurable: true
  });
  MessagePort.prototype.postMessage = function (data) {
    var target = this.__other;
    if (!target || this.__closed) return;
    var payload = clone(data);
    post(function () { target.__deliver(payload); });
  };
  MessagePort.prototype.__deliver = function (data) {
    if (this.__closed) return;
    if (!this.__started) { this.__queue.push(data); return; }
    var ev;
    try { ev = new MessageEvent('message'); } catch (e) { ev = { type: 'message' }; }
    try { Object.defineProperty(ev, 'data', { value: data, configurable: true }); } catch (e) { ev.data = data; }
    ev.ports = []; ev.origin = ''; ev.lastEventId = ''; ev.source = null;
    if (typeof this.__onmessage === 'function') this.__onmessage.call(this, ev);
    var listeners = this.__listeners.slice();
    for (var i = 0; i < listeners.length; i++) {
      var l = listeners[i];
      if (typeof l === 'function') l.call(this, ev);
      else if (l && typeof l.handleEvent === 'function') l.handleEvent(ev);
    }
  };
  MessagePort.prototype.start = function () {
    if (this.__started) return;
    this.__started = true;
    var queued = this.__queue, self = this;
    this.__queue = [];
    queued.forEach(function (d) { post(function () { self.__deliver(d); }); });
  };
  MessagePort.prototype.close = function () { this.__closed = true; };
  MessagePort.prototype.addEventListener = function (type, l) {
    if (type === 'message' && l && this.__listeners.indexOf(l) < 0) this.__listeners.push(l);
  };
  MessagePort.prototype.removeEventListener = function (type, l) {
    var i = this.__listeners.indexOf(l);
    if (i >= 0) this.__listeners.splice(i, 1);
  };
  MessagePort.prototype.dispatchEvent = function () { return true; };

  MessageChannel = function MessageChannel() {
    var a = new MessagePort(), b = new MessagePort();
    a.__other = b; b.__other = a;
    this.port1 = a; this.port2 = b;
  };

  if (typeof window.postMessage !== 'function') {
    window.postMessage = function (message) {
      var data = clone(message);
      post(function () {
        var ev;
        try { ev = new MessageEvent('message'); } catch (e) { return; }
        try { Object.defineProperty(ev, 'data', { value: data, configurable: true }); } catch (e) { ev.data = data; }
        ev.origin = (typeof location !== 'undefined' && location.origin) || '';
        ev.source = window;
        window.dispatchEvent(ev);
      });
    };
  }

  // ---- observers ----
  var observers = [];
  var now = function () { return (typeof performance !== 'undefined' && performance.now) ? performance.now() : Date.now(); };
  var report = function (e) { setTimeout(function () { throw e; }, 0); };
  var register = function (o) { if (observers.indexOf(o) < 0) observers.push(o); };
  var unregister = function (o) { var i = observers.indexOf(o); if (i >= 0) observers.splice(i, 1); };
  var rect = function (x, y, w, h) {
    return { x: x, y: y, width: w, height: h, top: y, left: x, right: x + w, bottom: y + h,
             toJSON: function () { return { x: x, y: y, width: w, height: h, top: y, left: x, right: x + w, bottom: y + h }; } };
  };
  var measure = function (el) {
    var r = el.getBoundingClientRect ? el.getBoundingClientRect() : null;
    if (!r) return null;
    var x = r.left !== undefined ? r.left : r.x, y = r.top !== undefined ? r.top : r.y;
    return rect(x, y, r.width || 0, r.height || 0);
  };
  var connected = function (el) { return el.isConnected !== false; };
  var parseMargin = function (text) {
    var parts = String(text || '0px').trim().split(/\s+/), out = [];
    for (var i = 0; i < parts.length && i < 4; i++) {
      var v = parseFloat(parts[i]) || 0;
      out.push(parts[i].indexOf('%') >= 0 ? { pct: v } : { px: v });
    }
    if (out.length === 0) out.push({ px: 0 });
    if (out.length === 1) out.push(out[0]);
    if (out.length === 2) out.push(out[0]);
    if (out.length === 3) out.push(out[1]);
    return out;  // top, right, bottom, left
  };
  var marginPx = function (m, size) { return m.pct !== undefined ? m.pct * size / 100 : m.px; };

  IntersectionObserverEntry = function IntersectionObserverEntry(init) {
    for (var k in init) this[k] = init[k];
  };
  IntersectionObserver = function IntersectionObserver(callback, options) {
    if (typeof callback !== 'function') throw new TypeError("Failed to construct 'IntersectionObserver': The callback provided as parameter 1 is not a function.");
    options = options || {};
    var t = options.threshold === undefined ? [0] : [].concat(options.threshold);
    for (var i = 0; i < t.length; i++) {
      t[i] = Number(t[i]);
      if (!(t[i] >= 0 && t[i] <= 1)) throw new RangeError("Failed to construct 'IntersectionObserver': Threshold values must be numbers between 0 and 1");
    }
    t.sort(function (a, b) { return a - b; });
    this.root = options.root || null;
    this.rootMargin = options.rootMargin || '0px 0px 0px 0px';
    this.thresholds = t;
    this.__callback = callback; this.__margin = parseMargin(this.rootMargin);
    this.__targets = []; this.__records = []; this.__checkedAt = -1e9;
  };
  IntersectionObserver.prototype.observe = function (target) {
    if (!target || typeof target !== 'object') throw new TypeError("Failed to execute 'observe' on 'IntersectionObserver': parameter 1 is not of type 'Element'.");
    for (var i = 0; i < this.__targets.length; i++) if (this.__targets[i].el === target) return;
    this.__targets.push({ el: target, state: null });
    register(this);
  };
  IntersectionObserver.prototype.unobserve = function (target) {
    this.__targets = this.__targets.filter(function (t) { return t.el !== target; });
    if (!this.__targets.length) unregister(this);
  };
  IntersectionObserver.prototype.disconnect = function () { this.__targets = []; unregister(this); };
  IntersectionObserver.prototype.takeRecords = function () { var r = this.__records; this.__records = []; return r; };
  IntersectionObserver.prototype.__check = function (time, full) {
    var rootRect;
    if (this.root) { rootRect = measure(this.root); if (!rootRect) return; }
    else rootRect = rect(0, 0, window.innerWidth || 0, window.innerHeight || 0);
    var m = this.__margin;
    var top = rootRect.top - marginPx(m[0], rootRect.height), right = rootRect.right + marginPx(m[1], rootRect.width);
    var bottom = rootRect.bottom + marginPx(m[2], rootRect.height), left = rootRect.left - marginPx(m[3], rootRect.width);
    var rootBounds = rect(left, top, right - left, bottom - top);
    for (var i = 0; i < this.__targets.length; i++) {
      var t = this.__targets[i];
      if (t.state && !full) continue;
      var box = connected(t.el) ? measure(t.el) : null;
      var hasBox = box && !(box.width === 0 && box.height === 0 && box.left === 0 && box.top === 0);
      var b = box || rect(0, 0, 0, 0);
      var il = Math.max(b.left, left), it = Math.max(b.top, top);
      var ir = Math.min(b.right, right), ib = Math.min(b.bottom, bottom);
      var intersecting = !!hasBox && ir >= il && ib >= it;
      var area = b.width * b.height;
      var ratio = intersecting ? (area > 0 ? Math.min(1, (ir - il) * (ib - it) / area) : 1) : 0;
      var index = -1;
      if (intersecting) { index = 0; while (index < this.thresholds.length && this.thresholds[index] <= ratio) index++; }
      if (t.state && t.state.index === index && t.state.intersecting === intersecting) continue;
      t.state = { index: index, intersecting: intersecting };
      this.__records.push(new IntersectionObserverEntry({
        time: time, target: t.el, rootBounds: this.root ? rootBounds : rootBounds,
        boundingClientRect: b,
        intersectionRect: intersecting ? rect(il, it, ir - il, ib - it) : rect(0, 0, 0, 0),
        isIntersecting: intersecting, intersectionRatio: ratio, isVisible: false
      }));
    }
  };

  ResizeObserverEntry = function ResizeObserverEntry(init) { for (var k in init) this[k] = init[k]; };
  ResizeObserver = function ResizeObserver(callback) {
    if (typeof callback !== 'function') throw new TypeError("Failed to construct 'ResizeObserver': The callback provided as parameter 1 is not a function.");
    this.__callback = callback; this.__targets = []; this.__records = []; this.__checkedAt = -1e9;
  };
  ResizeObserver.prototype.observe = function (target, options) {
    if (!target || typeof target !== 'object') throw new TypeError("Failed to execute 'observe' on 'ResizeObserver': parameter 1 is not of type 'Element'.");
    for (var i = 0; i < this.__targets.length; i++) if (this.__targets[i].el === target) return;
    this.__targets.push({ el: target, w: 0, h: 0, fresh: true, box: (options && options.box) || 'content-box' });
    register(this);
  };
  ResizeObserver.prototype.unobserve = IntersectionObserver.prototype.unobserve;
  ResizeObserver.prototype.disconnect = IntersectionObserver.prototype.disconnect;
  ResizeObserver.prototype.__check = function (time, full) {
    for (var i = 0; i < this.__targets.length; i++) {
      var t = this.__targets[i];
      if (!t.fresh && !full) continue;
      var b = connected(t.el) ? measure(t.el) : null;
      var pl = 0, pt = 0, pr = 0, pb = 0, bl = 0, bt = 0, br = 0, bb = 0;
      if (b && typeof getComputedStyle === 'function') {
        try {
          var cs = getComputedStyle(t.el);
          pl = parseFloat(cs.paddingLeft) || 0; pr = parseFloat(cs.paddingRight) || 0;
          pt = parseFloat(cs.paddingTop) || 0; pb = parseFloat(cs.paddingBottom) || 0;
          bl = parseFloat(cs.borderLeftWidth) || 0; br = parseFloat(cs.borderRightWidth) || 0;
          bt = parseFloat(cs.borderTopWidth) || 0; bb = parseFloat(cs.borderBottomWidth) || 0;
        } catch (e) {}
      }
      var bw = b ? b.width : 0, bh = b ? b.height : 0;
      var cw = Math.max(0, bw - pl - pr - bl - br), ch = Math.max(0, bh - pt - pb - bt - bb);
      var w = t.box === 'border-box' ? bw : cw, h = t.box === 'border-box' ? bh : ch;
      var fresh = t.fresh;
      t.fresh = false;
      if (w === t.w && h === t.h && !(fresh && (w || h))) continue;
      t.w = w; t.h = h;
      this.__records.push(new ResizeObserverEntry({
        target: t.el, contentRect: rect(pl, pt, cw, ch),
        contentBoxSize: [{ inlineSize: cw, blockSize: ch }],
        borderBoxSize: [{ inlineSize: bw, blockSize: bh }],
        devicePixelContentBoxSize: [{ inlineSize: cw * (window.devicePixelRatio || 1), blockSize: ch * (window.devicePixelRatio || 1) }]
      }));
    }
  };

  // Called once per frame; existing targets are re-measured at most every
  // 100 ms, new ones at once
  __zepraDeliverObservations = function () {
    if (!observers.length) return;
    var time = now();
    var list = observers.slice();
    for (var i = 0; i < list.length; i++) {
      var o = list[i];
      var full = time - o.__checkedAt >= 100;
      if (full) o.__checkedAt = time;
      try { o.__check(time, full); } catch (e) { report(e); }
    }
    for (var j = 0; j < list.length; j++) {
      var ob = list[j];
      if (!ob.__records.length) continue;
      var records = ob.__records;
      ob.__records = [];
      try { ob.__callback.call(ob, records, ob); } catch (e) { report(e); }
    }
  };

  window.MessagePort = MessagePort; window.MessageChannel = MessageChannel;
  window.IntersectionObserver = IntersectionObserver; window.IntersectionObserverEntry = IntersectionObserverEntry;
  window.ResizeObserver = ResizeObserver; window.ResizeObserverEntry = ResizeObserverEntry;
})();
)JS";

} // namespace


// Web platform pieces pages touch at startup that the DOM bindings do not
// provide yet, defined only when missing: Range/Selection, TextEncoder and
// TextDecoder, AbortController, DOMException, Blob/File/FormData,
// requestIdleCallback, reportError, escape/unescape, window.top/parent/...,
// and Element extras (animate, compareDocumentPosition, reflected
// attributes). Small and spec-shaped; enough for libraries to boot.
const char* const kPlatformShims = R"JS(
(function () {
  var def = __zepraDefineGlobal;
  var has = function (name) { return typeof window[name] !== 'undefined'; };
  var proto = function (C, methods) { for (var k in methods) Object.defineProperty(C.prototype, k, { value: methods[k], writable: true, configurable: true }); };
  var accessor = function (target, name, get, set) {
    if (!target || Object.getOwnPropertyDescriptor(target, name)) return;
    Object.defineProperty(target, name, { get: get, set: set, configurable: true, enumerable: true });
  };

  // ---- DOMException ----
  if (!has('DOMException')) {
    var codes = { IndexSizeError: 1, HierarchyRequestError: 3, WrongDocumentError: 4, InvalidCharacterError: 5,
                  NotFoundError: 8, NotSupportedError: 9, InvalidStateError: 11, SyntaxError: 12,
                  InvalidModificationError: 13, NamespaceError: 14, InvalidAccessError: 15, SecurityError: 18,
                  NetworkError: 19, AbortError: 20, QuotaExceededError: 22, TimeoutError: 23, DataCloneError: 25 };
    var DOMException = function DOMException(message, name) {
      this.message = message === undefined ? '' : String(message);
      this.name = name === undefined ? 'Error' : String(name);
      this.code = codes[this.name] || 0;
      try { this.stack = new Error(this.message).stack; } catch (e) {}
    };
    DOMException.prototype = Object.create(Error.prototype);
    DOMException.prototype.constructor = DOMException;
    for (var c in codes) DOMException[c.replace(/([a-z])([A-Z])/g, '$1_$2').toUpperCase().replace(/_ERROR$/, '_ERR')] = codes[c];
    def('DOMException', DOMException);
  }

  // ---- AbortController / AbortSignal ----
  if (!has('AbortController')) {
    var AbortSignal = function AbortSignal() { throw new TypeError('Illegal constructor'); };
    var makeSignal = function () {
      var s = Object.create(AbortSignal.prototype);
      s.aborted = false; s.reason = undefined; s.onabort = null; s.__listeners = [];
      return s;
    };
    proto(AbortSignal, {
      addEventListener: function (type, fn) { if (type === 'abort' && fn) this.__listeners.push(fn); },
      removeEventListener: function (type, fn) { var i = this.__listeners.indexOf(fn); if (i >= 0) this.__listeners.splice(i, 1); },
      dispatchEvent: function (ev) {
        if (typeof this.onabort === 'function') this.onabort.call(this, ev);
        var ls = this.__listeners.slice();
        for (var i = 0; i < ls.length; i++) { if (typeof ls[i] === 'function') ls[i].call(this, ev); else if (ls[i] && ls[i].handleEvent) ls[i].handleEvent(ev); }
        return true;
      },
      throwIfAborted: function () { if (this.aborted) throw this.reason; }
    });
    var abortSignal = function (s, reason) {
      if (s.aborted) return;
      s.aborted = true;
      s.reason = reason === undefined ? new DOMException('signal is aborted without reason', 'AbortError') : reason;
      s.dispatchEvent({ type: 'abort', target: s, currentTarget: s });
    };
    AbortSignal.abort = function (reason) { var s = makeSignal(); abortSignal(s, reason); return s; };
    AbortSignal.timeout = function (ms) {
      var s = makeSignal();
      setTimeout(function () { abortSignal(s, new DOMException('signal timed out', 'TimeoutError')); }, ms);
      return s;
    };
    AbortSignal.any = function (signals) {
      var s = makeSignal();
      signals.forEach(function (o) {
        if (o.aborted) abortSignal(s, o.reason);
        else o.addEventListener('abort', function () { abortSignal(s, o.reason); });
      });
      return s;
    };
    var AbortController = function AbortController() { this.signal = makeSignal(); };
    proto(AbortController, { abort: function (reason) { abortSignal(this.signal, reason); } });
    def('AbortSignal', AbortSignal);
    def('AbortController', AbortController);
  }

  // ---- TextEncoder / TextDecoder (UTF-8) ----
  if (!has('TextEncoder')) {
    var TextEncoder = function TextEncoder() {};
    proto(TextEncoder, {
      encode: function (input) {
        var s = input === undefined ? '' : String(input), out = [];
        for (var i = 0; i < s.length; i++) {
          var c = s.charCodeAt(i);
          if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.length) {
            var d = s.charCodeAt(i + 1);
            if (d >= 0xDC00 && d <= 0xDFFF) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i++; }
            else c = 0xFFFD;
          } else if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;
          if (c < 0x80) out.push(c);
          else if (c < 0x800) out.push(0xC0 | (c >> 6), 0x80 | (c & 63));
          else if (c < 0x10000) out.push(0xE0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
          else out.push(0xF0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
        }
        return new Uint8Array(out);
      },
      encodeInto: function (s, dest) {
        var bytes = this.encode(s), n = Math.min(bytes.length, dest.length);
        for (var i = 0; i < n; i++) dest[i] = bytes[i];
        return { read: s.length, written: n };
      }
    });
    TextEncoder.prototype.encoding = 'utf-8';
    def('TextEncoder', TextEncoder);
  }
  if (!has('TextDecoder')) {
    var TextDecoder = function TextDecoder(label, options) {
      var l = String(label === undefined ? 'utf-8' : label).toLowerCase();
      if (l === 'utf8' || l === 'unicode-1-1-utf-8') l = 'utf-8';
      if (l !== 'utf-8' && l !== 'utf-16le' && l !== 'latin1' && l !== 'iso-8859-1' && l !== 'ascii' && l !== 'windows-1252') throw new RangeError("The encoding label provided ('" + label + "') is invalid.");
      this.encoding = l === 'latin1' || l === 'iso-8859-1' || l === 'ascii' ? 'windows-1252' : l;
      this.fatal = !!(options && options.fatal);
      this.ignoreBOM = !!(options && options.ignoreBOM);
    };
    proto(TextDecoder, {
      decode: function (input) {
        if (input === undefined) return '';
        var b = input instanceof ArrayBuffer ? new Uint8Array(input)
              : (input && input.buffer instanceof ArrayBuffer) ? new Uint8Array(input.buffer, input.byteOffset || 0, input.byteLength) : input;
        var out = '', parts = [], i = 0, n = b.length;
        if (this.encoding === 'windows-1252') { for (; i < n; i++) out += String.fromCharCode(b[i]); return out; }
        if (this.encoding === 'utf-16le') { for (; i + 1 < n; i += 2) out += String.fromCharCode(b[i] | (b[i + 1] << 8)); return out; }
        if (!this.ignoreBOM && n >= 3 && b[0] === 0xEF && b[1] === 0xBB && b[2] === 0xBF) i = 3;
        var units = [];
        var bad = function (self) { if (self.fatal) throw new TypeError('The encoded data was not valid for encoding utf-8'); units.push(0xFFFD); };
        while (i < n) {
          var c = b[i];
          if (c < 0x80) { units.push(c); i++; }
          else {
            var need = c >= 0xF0 && c <= 0xF4 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 && c < 0xE0 ? 1 : -1;
            if (need < 0 || i + need >= n + 0 && i + need > n - 1 + 1) { if (need < 0 || i + need >= n) { bad(this); i++; continue; } }
            var cp = c & (need === 1 ? 0x1F : need === 2 ? 0x0F : 0x07), ok = true;
            for (var k = 1; k <= need; k++) { var x = b[i + k]; if ((x & 0xC0) !== 0x80) { ok = false; break; } cp = (cp << 6) | (x & 63); }
            if (!ok) { bad(this); i++; continue; }
            i += need + 1;
            if (cp >= 0x10000) { cp -= 0x10000; units.push(0xD800 + (cp >> 10), 0xDC00 + (cp & 1023)); }
            else units.push(cp);
          }
          if (units.length >= 8192) { parts.push(String.fromCharCode.apply(null, units)); units = []; }
        }
        parts.push(String.fromCharCode.apply(null, units));
        return parts.join('');
      }
    });
    def('TextDecoder', TextDecoder);
  }

  // ---- Blob / File / FormData ----
  if (!has('Blob')) {
    var Blob = function Blob(parts, options) {
      var text = '';
      (parts || []).forEach(function (p) {
        if (typeof p === 'string') text += p;
        else if (p instanceof Blob) text += p.__text;
        else if (p instanceof ArrayBuffer || (p && p.buffer instanceof ArrayBuffer)) text += new TextDecoder('latin1').decode(p);
        else text += String(p);
      });
      this.__text = text;
      this.size = new TextEncoder().encode(text).length;
      this.type = options && options.type ? String(options.type).toLowerCase() : '';
    };
    proto(Blob, {
      text: function () { return Promise.resolve(this.__text); },
      arrayBuffer: function () { return Promise.resolve(new TextEncoder().encode(this.__text).buffer); },
      slice: function (a, b, type) { return new Blob([this.__text.slice(a, b)], { type: type || this.type }); },
      stream: function () { throw new TypeError('Blob.stream is not supported'); }
    });
    var File = function File(parts, name, options) {
      Blob.call(this, parts, options);
      this.name = String(name);
      this.lastModified = options && options.lastModified !== undefined ? Number(options.lastModified) : Date.now();
    };
    File.prototype = Object.create(Blob.prototype);
    File.prototype.constructor = File;
    def('Blob', Blob);
    def('File', File);
  }
  if (!has('FormData')) {
    var FormData = function FormData(form) {
      this.__entries = [];
      if (form && form.elements) {
        for (var i = 0; i < form.elements.length; i++) {
          var el = form.elements[i];
          if (el.name && !el.disabled && ((el.type !== 'checkbox' && el.type !== 'radio') || el.checked)) this.__entries.push([el.name, String(el.value)]);
        }
      }
    };
    proto(FormData, {
      append: function (k, v) { this.__entries.push([String(k), v instanceof Blob ? v : String(v)]); },
      set: function (k, v) { this.delete(k); this.append(k, v); },
      get: function (k) { for (var i = 0; i < this.__entries.length; i++) if (this.__entries[i][0] === String(k)) return this.__entries[i][1]; return null; },
      getAll: function (k) { return this.__entries.filter(function (e) { return e[0] === String(k); }).map(function (e) { return e[1]; }); },
      has: function (k) { return this.get(k) !== null; },
      delete: function (k) { this.__entries = this.__entries.filter(function (e) { return e[0] !== String(k); }); },
      forEach: function (fn, self) { var t = this; this.__entries.forEach(function (e) { fn.call(self, e[1], e[0], t); }); },
      entries: function () { return this.__entries.map(function (e) { return [e[0], e[1]]; })[Symbol.iterator](); },
      keys: function () { return this.__entries.map(function (e) { return e[0]; })[Symbol.iterator](); },
      values: function () { return this.__entries.map(function (e) { return e[1]; })[Symbol.iterator](); }
    });
    FormData.prototype[Symbol.iterator] = FormData.prototype.entries;
    def('FormData', FormData);
  }

  // ---- Range / Selection ----
  if (!has('Range')) {
    var Range = function Range() {
      this.startContainer = document; this.startOffset = 0;
      this.endContainer = document; this.endOffset = 0;
      this.collapsed = true; this.commonAncestorContainer = document;
    };
    var update = function (r) {
      r.collapsed = r.startContainer === r.endContainer && r.startOffset === r.endOffset;
      var a = r.startContainer;
      while (a && a.contains && !a.contains(r.endContainer)) a = a.parentNode;
      r.commonAncestorContainer = a || document;
    };
    var indexOf = function (node) { var p = node.parentNode; return p ? Array.prototype.indexOf.call(p.childNodes, node) : 0; };
    proto(Range, {
      setStart: function (n, o) { this.startContainer = n; this.startOffset = o; update(this); },
      setEnd: function (n, o) { this.endContainer = n; this.endOffset = o; update(this); },
      setStartBefore: function (n) { this.setStart(n.parentNode, indexOf(n)); },
      setStartAfter: function (n) { this.setStart(n.parentNode, indexOf(n) + 1); },
      setEndBefore: function (n) { this.setEnd(n.parentNode, indexOf(n)); },
      setEndAfter: function (n) { this.setEnd(n.parentNode, indexOf(n) + 1); },
      selectNode: function (n) { this.setStartBefore(n); this.setEndAfter(n); },
      selectNodeContents: function (n) { this.setStart(n, 0); this.setEnd(n, n.nodeType === 3 ? (n.data || '').length : n.childNodes.length); },
      collapse: function (toStart) { if (toStart) this.setEnd(this.startContainer, this.startOffset); else this.setStart(this.endContainer, this.endOffset); },
      cloneRange: function () { var r = new Range(); r.setStart(this.startContainer, this.startOffset); r.setEnd(this.endContainer, this.endOffset); return r; },
      detach: function () {},
      toString: function () { var n = this.commonAncestorContainer; return n && n.textContent !== undefined && !this.collapsed ? String(n.textContent) : ''; },
      createContextualFragment: function (html) { var t = document.createElement('template'); t.innerHTML = html; return t.content; },
      getBoundingClientRect: function () {
        var n = this.startContainer && this.startContainer.nodeType === 1 ? this.startContainer : this.startContainer && this.startContainer.parentNode;
        return n && n.getBoundingClientRect ? n.getBoundingClientRect() : new DOMRect(0, 0, 0, 0);
      },
      getClientRects: function () { return [this.getBoundingClientRect()]; },
      deleteContents: function () {
        if (this.startContainer === this.endContainer && this.startContainer.nodeType === 1) {
          var kids = Array.prototype.slice.call(this.startContainer.childNodes, this.startOffset, this.endOffset);
          kids.forEach(function (k) { k.parentNode.removeChild(k); });
          this.collapse(true);
        }
      },
      insertNode: function (node) {
        var c = this.startContainer;
        if (c.nodeType === 1 || c.nodeType === 9 || c.nodeType === 11) c.insertBefore(node, c.childNodes[this.startOffset] || null);
        else if (c.parentNode) c.parentNode.insertBefore(node, c.nextSibling);
      },
      compareBoundaryPoints: function () { return 0; },
      comparePoint: function () { return 0; },
      isPointInRange: function () { return false; },
      intersectsNode: function (n) { var a = this.commonAncestorContainer; return !!(a && a.contains && a.contains(n)); }
    });
    Range.START_TO_START = 0; Range.START_TO_END = 1; Range.END_TO_END = 2; Range.END_TO_START = 3;
    def('Range', Range);
    if (!document.createRange) document.createRange = function () { return new Range(); };
  }
  if (!has('Selection')) {
    var Selection = function Selection() { throw new TypeError('Illegal constructor'); };
    var selection = Object.create(Selection.prototype);
    selection.__ranges = [];
    proto(Selection, {
      getRangeAt: function (i) { if (i >= this.__ranges.length) throw new DOMException('The index is not in the allowed range.', 'IndexSizeError'); return this.__ranges[i]; },
      addRange: function (r) { this.__ranges = [r]; },
      removeAllRanges: function () { this.__ranges = []; },
      removeRange: function () { this.__ranges = []; },
      empty: function () { this.__ranges = []; },
      collapse: function (n, o) { var r = new Range(); if (n) { r.setStart(n, o || 0); r.collapse(true); } this.__ranges = n ? [r] : []; },
      collapseToStart: function () {}, collapseToEnd: function () {},
      selectAllChildren: function (n) { var r = new Range(); r.selectNodeContents(n); this.__ranges = [r]; },
      extend: function () {}, setBaseAndExtent: function (a, ao, b, bo) { var r = new Range(); r.setStart(a, ao); r.setEnd(b, bo); this.__ranges = [r]; },
      containsNode: function () { return false; }, deleteFromDocument: function () {}, modify: function () {},
      toString: function () { return this.__ranges.length ? this.__ranges[0].toString() : ''; }
    });
    Object.defineProperty(Selection.prototype, 'rangeCount', { get: function () { return this.__ranges.length; }, configurable: true });
    Object.defineProperty(Selection.prototype, 'isCollapsed', { get: function () { return !this.__ranges.length || this.__ranges[0].collapsed; }, configurable: true });
    ['anchorNode', 'focusNode'].forEach(function (k) { Object.defineProperty(Selection.prototype, k, { get: function () { return this.__ranges.length ? this.__ranges[0].startContainer : null; }, configurable: true }); });
    ['anchorOffset', 'focusOffset'].forEach(function (k) { Object.defineProperty(Selection.prototype, k, { get: function () { return this.__ranges.length ? this.__ranges[0].startOffset : 0; }, configurable: true }); });
    Object.defineProperty(Selection.prototype, 'type', { get: function () { return !this.__ranges.length ? 'None' : this.isCollapsed ? 'Caret' : 'Range'; }, configurable: true });
    def('Selection', Selection);
    var getSelection = function () { return selection; };
    def('getSelection', getSelection);
    if (!document.getSelection) document.getSelection = getSelection;
  }

  // ---- scheduling, errors, misc globals ----
  if (!has('requestIdleCallback')) {
    var idleId = 0, idle = {};
    def('requestIdleCallback', function (cb, opts) {
      var id = ++idleId;
      idle[id] = setTimeout(function () {
        delete idle[id];
        var start = Date.now();
        cb({ didTimeout: false, timeRemaining: function () { return Math.max(0, 50 - (Date.now() - start)); } });
      }, 1);
      return id;
    });
    def('cancelIdleCallback', function (id) { if (idle[id]) { clearTimeout(idle[id]); delete idle[id]; } });
  }
  if (!has('reportError')) {
    def('reportError', function (e) { setTimeout(function () { throw e; }, 0); });
  }
  if (!has('PerformanceObserver')) {
    var PerformanceObserver = function PerformanceObserver(cb) { this.__cb = cb; };
    proto(PerformanceObserver, { observe: function () {}, disconnect: function () {}, takeRecords: function () { return []; } });
    PerformanceObserver.supportedEntryTypes = [];
    def('PerformanceObserver', PerformanceObserver);
  }
  if (!has('BroadcastChannel')) {
    var BroadcastChannel = function BroadcastChannel(name) { this.name = String(name); this.onmessage = null; };
    proto(BroadcastChannel, { postMessage: function () {}, close: function () {}, addEventListener: function () {}, removeEventListener: function () {} });
    def('BroadcastChannel', BroadcastChannel);
  }
  if (typeof escape === 'undefined') {
    def('escape', function (s) {
      s = String(s);
      var out = '', keep = /[A-Za-z0-9@*_+\-./]/;
      for (var i = 0; i < s.length; i++) {
        var c = s.charCodeAt(i);
        if (keep.test(s[i])) out += s[i];
        else if (c < 256) out += '%' + (c < 16 ? '0' : '') + c.toString(16).toUpperCase();
        else out += '%u' + ('000' + c.toString(16).toUpperCase()).slice(-4);
      }
      return out;
    });
    def('unescape', function (s) {
      return String(s).replace(/%u([0-9A-Fa-f]{4})|%([0-9A-Fa-f]{2})/g, function (m, u, h) { return String.fromCharCode(parseInt(u || h, 16)); });
    });
  }
  var selfProps = ['top', 'parent', 'frames', 'self'];
  for (var i = 0; i < selfProps.length; i++) if (typeof window[selfProps[i]] === 'undefined') window[selfProps[i]] = window;
  if (typeof window.opener === 'undefined') window.opener = null;
  if (typeof window.closed === 'undefined') window.closed = false;
  if (typeof window.name === 'undefined') window.name = '';
  if (typeof window.length === 'undefined') window.length = 0;
  if (typeof window.isSecureContext === 'undefined') window.isSecureContext = typeof location !== 'undefined' && (location.protocol === 'https:' || location.hostname === 'localhost');
  if (typeof window.origin === 'undefined' && typeof location !== 'undefined') window.origin = location.origin;
  ['screenX', 'screenY', 'screenLeft', 'screenTop'].forEach(function (k) { if (typeof window[k] === 'undefined') window[k] = 0; });
  if (typeof window.outerWidth === 'undefined') window.outerWidth = window.innerWidth;
  if (typeof window.outerHeight === 'undefined') window.outerHeight = window.innerHeight;
  if (!has('DOMRectReadOnly') && has('DOMRect')) def('DOMRectReadOnly', DOMRect);

  // ---- document extras ----
  var d = document;
  var docProps = {
    referrer: function () { return ''; },
    domain: function () { return typeof location !== 'undefined' ? location.hostname : ''; },
    baseURI: function () { var b = d.querySelector('base[href]'); return b ? b.href : (typeof location !== 'undefined' ? location.href : ''); },
    forms: function () { return d.getElementsByTagName('form'); },
    images: function () { return d.getElementsByTagName('img'); },
    links: function () { return d.querySelectorAll('a[href],area[href]'); },
    fullscreenElement: function () { return null; },
    pointerLockElement: function () { return null; },
    prerendering: function () { return false; }
  };
  for (var k in docProps) if (!(k in d)) accessor(d, k, docProps[k]);
  if (!('fullscreenEnabled' in d)) d.fullscreenEnabled = false;
  if (!d.exitFullscreen) d.exitFullscreen = function () { return Promise.resolve(); };
  if (!d.execCommand) d.execCommand = function () { return false; };
  if (!d.queryCommandSupported) d.queryCommandSupported = function () { return false; };
  if (!d.getAnimations) d.getAnimations = function () { return []; };
  if (!d.elementFromPoint) d.elementFromPoint = function () { return null; };
  if (!d.elementsFromPoint) d.elementsFromPoint = function () { return []; };
  if (!d.createNodeIterator && d.createTreeWalker) {
    d.createNodeIterator = function (root, what, filter) {
      var w = d.createTreeWalker(root, what, filter), first = true;
      return { root: root, referenceNode: root, nextNode: function () { if (first) { first = false; if (!what || what & 1) return root; } return w.nextNode(); }, previousNode: function () { return w.previousNode(); }, detach: function () {} };
    };
  }

  // ---- Element extras ----
  var E = typeof Element !== 'undefined' ? Element.prototype : null;
  var H = typeof HTMLElement !== 'undefined' ? HTMLElement.prototype : null;
  if (E) {
    var animation = function (el, keyframes, options) {
      var duration = typeof options === 'number' ? options : (options && options.duration) || 0;
      var a = { playState: 'running', currentTime: 0, effect: null, id: (options && options.id) || '', onfinish: null, oncancel: null,
                pending: false, playbackRate: 1, startTime: null, timeline: null, replaceState: 'active' };
      var resolve;
      a.finished = new Promise(function (r) { resolve = r; });
      a.ready = Promise.resolve(a);
      var frames = Array.isArray(keyframes) ? keyframes : keyframes ? [keyframes] : [];
      var last = frames.length ? frames[frames.length - 1] : null;
      var fill = options && (options.fill === 'forwards' || options.fill === 'both');
      var done = function () {
        if (a.playState === 'finished' || a.playState === 'idle') return;
        a.playState = 'finished';
        if (fill && last && el.style) for (var p in last) if (p !== 'offset' && p !== 'easing' && p !== 'composite') { try { el.style[p] = last[p]; } catch (e) {} }
        var ev = { type: 'finish', target: a, currentTime: duration, timelineTime: null };
        if (typeof a.onfinish === 'function') a.onfinish(ev);
        resolve(a);
      };
      var timer = setTimeout(done, duration);
      a.finish = function () { clearTimeout(timer); done(); };
      a.cancel = function () { clearTimeout(timer); a.playState = 'idle'; if (typeof a.oncancel === 'function') a.oncancel({ type: 'cancel' }); };
      a.play = function () {}; a.pause = function () { a.playState = 'paused'; }; a.reverse = function () {};
      a.updatePlaybackRate = function () {}; a.persist = function () {}; a.commitStyles = function () {};
      a.addEventListener = function (t, fn) { if (t === 'finish') a.finished.then(function () { fn({ type: 'finish' }); }); };
      a.removeEventListener = function () {};
      return a;
    };
    if (!E.animate) E.animate = function (k, o) { return animation(this, k, o); };
    if (!E.getAnimations) E.getAnimations = function () { return []; };
    if (!E.requestFullscreen) E.requestFullscreen = function () { return Promise.reject(new TypeError('Fullscreen is not supported')); };
    if (!E.setPointerCapture) { E.setPointerCapture = function () {}; E.releasePointerCapture = function () {}; E.hasPointerCapture = function () { return false; }; }
    if (!E.scrollIntoViewIfNeeded) E.scrollIntoViewIfNeeded = function () { if (this.scrollIntoView) this.scrollIntoView(); };
    if (!E.checkVisibility) E.checkVisibility = function () { var r = this.getBoundingClientRect(); return !!(r.width || r.height); };
    if (!E.computedStyleMap) E.computedStyleMap = function () { var cs = getComputedStyle(this); return { get: function (p) { return { toString: function () { return cs.getPropertyValue(p); } }; } }; };
  }
  var N = typeof Node !== 'undefined' ? Node.prototype : null;
  if (N) {
    if (!N.compareDocumentPosition) {
      N.compareDocumentPosition = function (other) {
        if (other === this) return 0;
        var root = function (n) { while (n.parentNode) n = n.parentNode; return n; };
        if (root(this) !== root(other)) return 1 | 32 | 2;
        if (this.contains(other)) return 16 | 4;
        if (other.contains(this)) return 8 | 2;
        var chain = function (n) { var c = []; while (n) { c.unshift(n); n = n.parentNode; } return c; };
        var a = chain(this), b = chain(other), i = 0;
        while (a[i] === b[i]) i++;
        var p = a[i - 1].childNodes;
        return Array.prototype.indexOf.call(p, b[i]) > Array.prototype.indexOf.call(p, a[i]) ? 4 : 2;
      };
      ['DOCUMENT_POSITION_DISCONNECTED', 'DOCUMENT_POSITION_PRECEDING', 'DOCUMENT_POSITION_FOLLOWING', 'DOCUMENT_POSITION_CONTAINS',
       'DOCUMENT_POSITION_CONTAINED_BY', 'DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC'].forEach(function (k, i) { N[k] = 1 << i; if (typeof Node === 'function') Node[k] = 1 << i; });
    }
    if (!N.isEqualNode) N.isEqualNode = function (o) { return !!o && this.nodeType === o.nodeType && (this.outerHTML !== undefined ? this.outerHTML === o.outerHTML : this.textContent === o.textContent); };
    if (!N.normalize) N.normalize = function () {};
    if (!N.lookupNamespaceURI) N.lookupNamespaceURI = function () { return null; };
  }
  if (H) {
    var reflectAttr = function (prop, attr, bool) {
      accessor(H, prop, function () { return bool ? this.hasAttribute(attr) : (this.getAttribute(attr) || ''); },
        function (v) { if (bool) { if (v) this.setAttribute(attr, ''); else this.removeAttribute(attr); } else this.setAttribute(attr, String(v)); });
    };
    reflectAttr('nonce', 'nonce'); reflectAttr('inputMode', 'inputmode'); reflectAttr('enterKeyHint', 'enterkeyhint');
    reflectAttr('popover', 'popover');
    accessor(H, 'draggable', function () { var v = this.getAttribute('draggable'); return v === 'true' || (v !== 'false' && (this.tagName === 'IMG' || (this.tagName === 'A' && this.hasAttribute('href')))); },
      function (v) { this.setAttribute('draggable', v ? 'true' : 'false'); });
    accessor(H, 'spellcheck', function () { return this.getAttribute('spellcheck') !== 'false'; }, function (v) { this.setAttribute('spellcheck', v ? 'true' : 'false'); });
    accessor(H, 'translate', function () { return this.getAttribute('translate') !== 'no'; }, function (v) { this.setAttribute('translate', v ? 'yes' : 'no'); });
    accessor(H, 'contentEditable', function () { var v = this.getAttribute('contenteditable'); return v === null ? 'inherit' : v === '' ? 'true' : v; },
      function (v) { if (v === 'inherit') this.removeAttribute('contenteditable'); else this.setAttribute('contenteditable', String(v)); });
    accessor(H, 'isContentEditable', function () { for (var n = this; n && n.getAttribute; n = n.parentNode) { var v = n.getAttribute('contenteditable'); if (v !== null) return v !== 'false'; } return false; });
    accessor(H, 'outerText', function () { return this.innerText; });
    accessor(H, 'role', function () { return this.getAttribute('role'); }, function (v) { this.setAttribute('role', String(v)); });
    accessor(H, 'ariaLabel', function () { return this.getAttribute('aria-label'); }, function (v) { this.setAttribute('aria-label', String(v)); });
    if (!H.showPopover) { H.showPopover = function () {}; H.hidePopover = function () {}; H.togglePopover = function () { return false; }; }
  }
})();
)JS";

void ScriptContext::installWebTaskApis(Zepra::Runtime::Object* windowObj) {
    using namespace Zepra::Runtime;
    auto* postTask = createNativeFunction("__zepraPostTask",
        [this](Context*, const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isObject() || !args[0].asObject()->isFunction()) return Value::undefined();
            const int id = nextMessageTaskId_++;
            callbackStore_->set("m" + std::to_string(id), args[0]);
            messageTasks_.push_back(id);
            return Value::undefined();
        }, 1);
    vm_->setGlobal("__zepraPostTask", Value::object(postTask));
    vm_->setGlobal("__zepraDeliverObservations", Value::undefined());
    // __zepraDefineGlobal(name, value): a global binding and window property
    auto* defineGlobal = createNativeFunction("__zepraDefineGlobal",
        [this, windowObj](Context*, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) return Value::undefined();
            const std::string name = args[0].toString();
            vm_->setGlobal(name, args[1]);
            if (windowObj) windowObj->set(name, args[1]);
            return Value::undefined();
        }, 2);
    vm_->setGlobal("__zepraDefineGlobal", Value::object(defineGlobal));
    ScriptResult result = evaluate(kWebTaskApis, "<zepra-web-apis>");
    if (!result.success) {
        std::cerr << "[ScriptContext] web task APIs failed to install: " << result.error << std::endl;
    }
}

// Runs posted message tasks (MessagePort/window.postMessage) for up to
// ~8 ms; tasks posted meanwhile run too, the rest wait for the next frame
void ScriptContext::runMessageTasks() {
    if (messageTasks_.empty() || !vm_) return;
    const auto start = std::chrono::steady_clock::now();
    while (!messageTasks_.empty()) {
        const int id = messageTasks_.front();
        messageTasks_.pop_front();
        const std::string key = "m" + std::to_string(id);
        callStoredCallback(key, false, 0);
        removeStoredCallback(key);
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(8)) break;
    }
}

void ScriptContext::deliverObservations() {
    using namespace Zepra::Runtime;
    if (!vm_) return;
    Value deliver = vm_->getGlobal("__zepraDeliverObservations");
    if (!deliver.isObject() || !deliver.asObject()->isFunction()) return;
    auto* fn = static_cast<Function*>(deliver.asObject());
    runTask([&] { vm_->call(fn, Value::undefined(), {}); }, "observer callback");
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
    // <script> elements scripts insert run (script_elements.cpp)
    setupScriptElements();
}

// ============================================================================
// Console
// ============================================================================

void ScriptContext::setupConsole(Zepra::Runtime::Object* consoleObj) {
    using namespace Zepra::Runtime;
    auto method = [&](const char* name, auto fn) {
        consoleObj->set(name, Value::object(createNativeFunction(name, fn, 0)));
    };
    auto message = [this](const std::string& level, const std::vector<Value>& args) {
        ConsoleMessage m;
        m.level = level;
        m.values = formatArguments(args);
        m.text = previewText(m.values);
        m.depth = consoleGroupDepth_;
        setCallerSource(m);
        return m;
    };
    // A text-only entry, at the line that called the console method
    auto emitAt = [this](const std::string& level, const std::string& text) {
        ConsoleMessage m;
        m.level = level;
        m.text = text;
        m.depth = consoleGroupDepth_;
        setCallerSource(m);
        emitConsole(std::move(m));
    };
    // log/info/debug/warn/error: the arguments, formatted
    for (const char* level : {"log", "info", "debug", "warn", "error"}) {
        std::string lv = level;
        method(level, [this, message, lv](Context*, const std::vector<Value>& args) -> Value {
            emitConsole(message(lv, args));
            return Value::undefined();
        });
    }
    // dir: like log, with the object opened
    method("dir", [this, message](Context*, const std::vector<Value>& args) -> Value {
        ConsoleMessage m = message("log", args);
        for (ValuePreview& v : m.values) v.expanded = v.expandable;
        emitConsole(std::move(m));
        return Value::undefined();
    });
    // table: the rows as a log entry, opened
    method("table", [this, message](Context*, const std::vector<Value>& args) -> Value {
        ConsoleMessage m = message("log", args.empty() ? args : std::vector<Value>{args[0]});
        for (ValuePreview& v : m.values) v.expanded = v.expandable;
        emitConsole(std::move(m));
        return Value::undefined();
    });
    // trace: the arguments and the call stack
    method("trace", [this, message](Context*, const std::vector<Value>& args) -> Value {
        ConsoleMessage m = message("trace", args);
        std::string stack = vm_ ? vm_->captureStackTrace() : "";
        while (!stack.empty() && stack.back() == '\n') stack.pop_back();
        m.text = "console.trace()" + (m.text.empty() ? "" : " " + m.text) + (stack.empty() ? "" : "\n" + stack);
        emitConsole(std::move(m));
        return Value::undefined();
    });
    // assert(condition, ...data)
    method("assert", [this, message](Context*, const std::vector<Value>& args) -> Value {
        if (!args.empty() && args[0].isTruthy()) return Value::undefined();
        std::vector<Value> rest(args.size() > 1 ? args.begin() + 1 : args.end(), args.end());
        ConsoleMessage m = message("error", rest);
        ValuePreview head;
        head.kind = ValuePreview::Kind::String;
        head.text = "Assertion failed" + std::string(rest.empty() ? "" : ":");
        m.values.insert(m.values.begin(), head);
        m.text = previewText(m.values);
        emitConsole(std::move(m));
        return Value::undefined();
    });
    auto label = [](const std::vector<Value>& args) {
        return args.empty() || args[0].isUndefined() ? std::string("default") : args[0].toString();
    };
    method("count", [this, label, emitAt](Context*, const std::vector<Value>& args) -> Value {
        std::string l = label(args);
        emitAt("log", l + ": " + std::to_string(++consoleCounts_[l]));
        return Value::undefined();
    });
    method("countReset", [this, label, emitAt](Context*, const std::vector<Value>& args) -> Value {
        std::string l = label(args);
        if (consoleCounts_.erase(l) == 0) emitAt("warn", "Counter \"" + l + "\" doesn't exist.");
        return Value::undefined();
    });
    auto nowMs = [] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    method("time", [this, label, nowMs, emitAt](Context*, const std::vector<Value>& args) -> Value {
        std::string l = label(args);
        if (consoleTimers_.count(l)) emitAt("warn", "Timer \"" + l + "\" already exists.");
        else consoleTimers_[l] = nowMs();
        return Value::undefined();
    });
    auto timerText = [this, nowMs](const std::string& l, bool end) -> std::string {
        auto it = consoleTimers_.find(l);
        if (it == consoleTimers_.end()) return "";
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.2fms", nowMs() - it->second);
        if (end) consoleTimers_.erase(it);
        return l + ": " + buf + (end ? " - timer ended" : "");
    };
    method("timeEnd", [label, timerText, emitAt](Context*, const std::vector<Value>& args) -> Value {
        std::string l = label(args), t = timerText(l, true);
        if (t.empty()) emitAt("warn", "Timer \"" + l + "\" doesn't exist.");
        else emitAt("log", t);
        return Value::undefined();
    });
    method("timeLog", [label, timerText, emitAt](Context*, const std::vector<Value>& args) -> Value {
        std::string l = label(args), t = timerText(l, false);
        if (t.empty()) emitAt("warn", "Timer \"" + l + "\" doesn't exist.");
        else emitAt("log", t);
        return Value::undefined();
    });
    // group / groupCollapsed / groupEnd: the entries that follow are indented
    for (const char* name : {"group", "groupCollapsed"}) {
        method(name, [this, message](Context*, const std::vector<Value>& args) -> Value {
            ConsoleMessage m = message("group", args);
            if (m.text.empty()) m.text = "console.group";
            emitConsole(std::move(m));
            consoleGroupDepth_++;
            return Value::undefined();
        });
    }
    method("groupEnd", [this](Context*, const std::vector<Value>&) -> Value {
        if (consoleGroupDepth_ > 0) consoleGroupDepth_--;
        return Value::undefined();
    });
    method("clear", [this](Context*, const std::vector<Value>&) -> Value {
        emitConsole("clear", "");
        return Value::undefined();
    });
}

Zepra::Runtime::Value ScriptContext::callDocumentMethod(const char* name, const std::vector<Zepra::Runtime::Value>& args) {
    using namespace Zepra::Runtime;
    Value doc = vm_->getGlobal("document");
    if (!doc.isObject()) return Value::null();
    Value fn = vm_->getMember(doc, name);
    if (!fn.isObject() || !fn.asObject()->isCallable()) return Value::null();
    return vm_->call(static_cast<Function*>(fn.asObject()), doc, args);
}

// The console's own helpers, as Firefox has them. A page that defines a
// global of the same name ($ for jQuery) keeps it.
void ScriptContext::installConsoleHelpers() {
    using namespace Zepra::Runtime;
    if (consoleHelpersInstalled_ || !vm_) return;
    consoleHelpersInstalled_ = true;
    auto helper = [&](const char* name, auto fn, size_t arity) {
        if (!vm_->getGlobal(name).isUndefined()) return;
        vm_->setGlobal(name, Value::object(createNativeFunction(name, fn, arity)));
    };
    helper("$", [this](Context*, const std::vector<Value>& args) -> Value {
        return callDocumentMethod("querySelector", args);
    }, 1);
    helper("$$", [this](Context*, const std::vector<Value>& args) -> Value {
        return callDocumentMethod("querySelectorAll", args);
    }, 1);
    helper("clear", [this](Context*, const std::vector<Value>&) -> Value {
        emitConsole("clear", "");
        return Value::undefined();
    }, 0);
    helper("copy", [](Context*, const std::vector<Value>&) -> Value { return Value::undefined(); }, 1);
    if (vm_->getGlobal("$_").isUndefined()) vm_->setGlobal("$_", Value::undefined());
    if (vm_->getGlobal("$0").isUndefined()) vm_->setGlobal("$0", Value::undefined());
}

void ScriptContext::setInspectedNode(DOMNode* node) {
    if (!vm_ || !domBindings_) return;
    vm_->setGlobal("$0", node ? domBindings_->wrap(node) : Zepra::Runtime::Value::undefined());
}

// Console autocomplete reads values without running page code: data
// properties, and getters only when they are native (the DOM's)
std::vector<std::string> ScriptContext::completionNames(const std::vector<std::string>& path) {
    using namespace Zepra::Runtime;
    std::vector<std::string> names;
    if (!vm_ || !vmReady_) return names;
    installConsoleHelpers();
    Value window = vm_->getGlobal("window");

    // The object whose properties are listed, or a primitive's prototype
    auto holder = [this](const Value& v) -> Object* {
        if (v.isObject()) return v.asObject()->objectType() == ObjectType::Proxy ? nullptr : v.asObject();
        if (v.isString()) return vm_->intrinsic(VM::Intrinsic::String);
        if (v.isNumber()) return vm_->intrinsic(VM::Intrinsic::Number);
        if (v.isBoolean()) return vm_->intrinsic(VM::Intrinsic::Boolean);
        return nullptr;
    };
    auto read = [&](const Value& base, const std::string& key, Value& out) -> bool {
        Object* o = holder(base);
        if (!o) return false;
        try {
            // Arrays and functions reach their prototype through the VM, not a stored link
            for (Object* h = o; h; h = vm_->prototypeOf(h)) {
                auto desc = h->getOwnPropertyDescriptor(key);
                if (!desc) continue;
                if (!desc->isAccessorDescriptor()) {
                    out = base.isObject() ? desc->value : vm_->getMember(base, key);
                    return true;
                }
                if (!desc->getter.isObject()) return false;
                auto* getter = dynamic_cast<Function*>(desc->getter.asObject());
                if (!getter || !getter->isNative()) return false;
                out = vm_->call(getter, base, {});
                return true;
            }
            // Exotic objects answer some keys themselves (array length, string length)
            if (base.isObject() ? o->has(key) : key == "length") {
                out = vm_->getMember(base, key);
                return true;
            }
        } catch (...) {
        }
        return false;
    };

    std::unordered_map<std::string, bool> seen;
    auto add = [&](const std::string& name) {
        if (name.empty() || std::isdigit((unsigned char)name[0])) return;   // array indices
        if (seen.emplace(name, true).second) names.push_back(name);
    };
    auto addChain = [&](Object* o) {
        for (int depth = 0; o && depth < 32; o = vm_->prototypeOf(o), depth++) {
            for (const std::string& n : o->getOwnPropertyNames()) add(n);
        }
    };

    if (path.empty()) {
        for (const std::string& n : vm_->globalNames()) add(n);
        if (window.isObject()) addChain(window.asObject());
        return names;
    }
    Value v = vm_->getGlobal(path[0]);
    if (v.isUndefined() && !(window.isObject() && read(window, path[0], v))) return names;
    for (size_t i = 1; i < path.size(); i++) {
        if (!read(v, path[i], v)) return names;
    }
    if (v.isString()) add("length");
    addChain(holder(v));
    return names;
}

ConsoleMessage ScriptContext::evaluateConsole(const std::string& code, DOMNode* inspected) {
    using namespace Zepra::Runtime;
    ConsoleMessage m;
    if (!vm_ || !vmReady_) {
        m.level = "error";
        m.text = "No script context for this page";
        return m;
    }
    installConsoleHelpers();
    setInspectedNode(inspected);
    Value value, thrown;
    quietErrors_ = true;
    ScriptResult r = run(code, "<console>", &value, &thrown);
    quietErrors_ = false;
    FormatOptions options;
    options.quoteStrings = true;
    if (r.success) {
        vm_->setGlobal("$_", value);
        m.level = "result";
        m.values.push_back(formatValue(value, options));
        m.text = m.values[0].text;
        return m;
    }
    m.level = "error";
    if (!thrown.isUndefined()) {
        // The thrown value itself: "TypeError: x is not a function" with its stack
        m.values.push_back(formatValue(thrown, options));
        m.text = m.values[0].text;
        return m;
    }
    m.text = r.error.empty() ? "Evaluation failed" : r.error;
    // "SyntaxError: 1:5: message" -> "SyntaxError: message" at <console>:1:5
    const std::string prefix = "SyntaxError: ";
    if (m.text.rfind(prefix, 0) == 0) {
        std::string rest = m.text.substr(prefix.size());
        size_t a = rest.find(':'), b = a == std::string::npos ? a : rest.find(':', a + 1);
        if (b != std::string::npos && a > 0 && std::isdigit((unsigned char)rest[0]) &&
            std::all_of(rest.begin() + a + 1, rest.begin() + b, [](unsigned char c) { return std::isdigit(c); })) {
            m.source = "<console>:" + rest.substr(0, b);
            size_t msg = b + 1;
            while (msg < rest.size() && rest[msg] == ' ') msg++;
            m.text = prefix + rest.substr(msg);
        }
    }
    return m;
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
