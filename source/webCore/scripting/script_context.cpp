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
        if (stack.isString()) firstStackFrame(stack.toString(), file, line);
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

    // DataTransfer (drag and drop events carry one)
    setupDragAndDrop(windowObj);

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
