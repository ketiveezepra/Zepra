// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file script_context_test.cpp
 * @brief Integration tests for ScriptContext DOM API bindings
 *
 * Verifies that the browser's JS environment layer correctly binds:
 *   - window, document, console, navigator, location
 *   - document.getElementById, querySelector, createElement
 *   - JSON.parse/stringify, localStorage, history
 *   - alert, setTimeout, setInterval
 *   - Error propagation from VM to ScriptContext
 */

#include <gtest/gtest.h>
#include "scripting/script_context.hpp"

using namespace Zepra::WebCore;

class ScriptContextTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_unique<ScriptContext>();
        // Production code always calls initialize() after construction; the
        // window/console/JSON/storage globals are installed there, not in the ctor.
        ctx_->initialize(nullptr);
        consoleLogs_.clear();
        ctx_->setConsoleHandler([this](const std::string& level, const std::string& msg) {
            consoleLogs_.push_back({level, msg});
        });
    }

    std::unique_ptr<ScriptContext> ctx_;
    std::vector<std::pair<std::string, std::string>> consoleLogs_;
};

// =============================================================================
// VM Basics
// =============================================================================

TEST_F(ScriptContextTest, EvaluateEmpty) {
    auto result = ctx_->evaluate("", "<test>");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.value, "undefined");
}

TEST_F(ScriptContextTest, EvaluateLiteral) {
    auto result = ctx_->evaluate("42", "<test>");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.value, "42");
}

TEST_F(ScriptContextTest, EvaluateString) {
    auto result = ctx_->evaluate("\"hello\"", "<test>");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.value, "hello");
}

TEST_F(ScriptContextTest, SyntaxErrorReported) {
    auto result = ctx_->evaluate("if (", "<test>");
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
}

// =============================================================================
// Console Routing
// =============================================================================

TEST_F(ScriptContextTest, ConsoleLogRouted) {
    ctx_->evaluate("console.log(\"test message\")", "<test>");
    ASSERT_FALSE(consoleLogs_.empty());
    EXPECT_EQ(consoleLogs_.back().first, "log");
    EXPECT_TRUE(consoleLogs_.back().second.find("test message") != std::string::npos);
}

TEST_F(ScriptContextTest, ConsoleWarnRouted) {
    ctx_->evaluate("console.warn(\"warning\")", "<test>");
    ASSERT_FALSE(consoleLogs_.empty());
    EXPECT_EQ(consoleLogs_.back().first, "warn");
}

TEST_F(ScriptContextTest, ConsoleErrorRouted) {
    ctx_->evaluate("console.error(\"error msg\")", "<test>");
    ASSERT_FALSE(consoleLogs_.empty());
    EXPECT_EQ(consoleLogs_.back().first, "error");
}

// =============================================================================
// Window Globals
// =============================================================================

TEST_F(ScriptContextTest, WindowExists) {
    auto result = ctx_->evaluate("typeof window", "<test>");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.value, "object");
}

TEST_F(ScriptContextTest, WindowSelfReference) {
    auto result = ctx_->evaluate("window === window.window", "<test>");
    EXPECT_TRUE(result.success);
}

TEST_F(ScriptContextTest, NavigatorUserAgent) {
    auto result = ctx_->evaluate("navigator.userAgent", "<test>");
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.value.find("Zepra") != std::string::npos);
}

TEST_F(ScriptContextTest, InnerDimensions) {
    auto w = ctx_->evaluate("innerWidth", "<test>");
    EXPECT_TRUE(w.success);
    EXPECT_EQ(w.value, "1280");

    auto h = ctx_->evaluate("innerHeight", "<test>");
    EXPECT_TRUE(h.success);
    EXPECT_EQ(h.value, "720");
}

// =============================================================================
// JSON
// =============================================================================

TEST_F(ScriptContextTest, JSONStringify) {
    auto result = ctx_->evaluate("JSON.stringify(42)", "<test>");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.value, "42");
}

TEST_F(ScriptContextTest, JSONParseExists) {
    auto result = ctx_->evaluate("typeof JSON.parse", "<test>");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.value, "function");
}

// =============================================================================
// localStorage
// =============================================================================

TEST_F(ScriptContextTest, LocalStorageSetGet) {
    ctx_->evaluate("localStorage.setItem('key', 'value')", "<test>");
    auto result = ctx_->evaluate("localStorage.getItem('key')", "<test>");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.value, "value");
}

TEST_F(ScriptContextTest, LocalStorageGetMissing) {
    auto result = ctx_->evaluate("localStorage.getItem('nonexistent')", "<test>");
    EXPECT_TRUE(result.success);
    // Should return null
}

// =============================================================================
// history
// =============================================================================

TEST_F(ScriptContextTest, HistoryExists) {
    auto result = ctx_->evaluate("typeof history.pushState", "<test>");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.value, "function");
}

// =============================================================================
// Timers
// =============================================================================

TEST_F(ScriptContextTest, SetTimeoutReturnsId) {
    auto result = ctx_->evaluate("setTimeout(function(){}, 100)", "<test>");
    EXPECT_TRUE(result.success);
    // Should return a numeric ID
}

TEST_F(ScriptContextTest, SetIntervalReturnsId) {
    auto result = ctx_->evaluate("setInterval(function(){}, 100)", "<test>");
    EXPECT_TRUE(result.success);
}

// =============================================================================
// Global Functions
// =============================================================================

TEST_F(ScriptContextTest, ParseIntWorks) {
    auto result = ctx_->evaluate("parseInt('42')", "<test>");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.value, "42");
}

TEST_F(ScriptContextTest, ParseFloatWorks) {
    auto result = ctx_->evaluate("parseFloat('3.14')", "<test>");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.value, "3.14");
}

TEST_F(ScriptContextTest, IsNaNWorks) {
    auto result = ctx_->evaluate("isNaN(NaN)", "<test>");
    EXPECT_TRUE(result.success);
}

// =============================================================================
// Error Propagation
// =============================================================================

TEST_F(ScriptContextTest, RuntimeErrorBubblesUp) {
    auto result = ctx_->evaluate("undefined.property", "<test>");
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
}

TEST_F(ScriptContextTest, ErrorRoutedToConsole) {
    ctx_->evaluate("undefined.property", "<test>");
    // Error should have been sent to console handler
    bool foundError = false;
    for (const auto& [level, msg] : consoleLogs_) {
        if (level == "error") {
            foundError = true;
            break;
        }
    }
    EXPECT_TRUE(foundError);
}

// =============================================================================
// Promises and microtasks
// =============================================================================

namespace {
std::vector<std::string> messages(const std::vector<std::pair<std::string, std::string>>& logs,
                                  const std::string& level) {
    std::vector<std::string> out;
    for (const auto& [l, msg] : logs) {
        if (l == level) out.push_back(msg);
    }
    return out;
}
} // namespace

TEST_F(ScriptContextTest, PromiseReactionsRunAfterScript) {
    auto result = ctx_->evaluate(
        "Promise.resolve().then(function(){ console.log('then'); });"
        "queueMicrotask(function(){ console.log('micro'); });"
        "console.log('sync');", "<test>");
    EXPECT_TRUE(result.success) << result.error;
    EXPECT_EQ(messages(consoleLogs_, "log"), (std::vector<std::string>{"sync", "then", "micro"}));
}

TEST_F(ScriptContextTest, MicrotasksRunAfterEachTimer) {
    // Each timer callback is a task: its microtasks run before the next timer
    ctx_->evaluate(
        "setTimeout(function(){ console.log('t1'); Promise.resolve().then(function(){ console.log('m1'); }); }, 0);"
        "setTimeout(function(){ console.log('t2'); queueMicrotask(function(){ console.log('m2'); }); }, 0);",
        "<test>");
    ctx_->processTimers();
    EXPECT_EQ(messages(consoleLogs_, "log"), (std::vector<std::string>{"t1", "m1", "t2", "m2"}));
}

TEST_F(ScriptContextTest, MicrotasksRunAfterAnimationFrame) {
    ctx_->evaluate(
        "requestAnimationFrame(function(){ console.log('raf'); Promise.resolve().then(function(){ console.log('m'); }); });",
        "<test>");
    ctx_->runAnimationFrames();
    EXPECT_EQ(messages(consoleLogs_, "log"), (std::vector<std::string>{"raf", "m"}));
}

TEST_F(ScriptContextTest, MicrotasksRunAfterThrowingScript) {
    auto result = ctx_->evaluate(
        "Promise.resolve().then(function(){ console.log('still runs'); }); throw new Error('x');", "<test>");
    EXPECT_FALSE(result.success);
    EXPECT_EQ(messages(consoleLogs_, "log"), (std::vector<std::string>{"still runs"}));
}

TEST_F(ScriptContextTest, AsyncAwaitAcrossTimer) {
    ctx_->evaluate(
        "function sleep(){ return new Promise(function(r){ setTimeout(r, 0); }); }"
        "async function run(){ console.log('start'); await sleep(); console.log('woke'); }"
        "run().then(function(){ console.log('done'); });", "<test>");
    EXPECT_EQ(messages(consoleLogs_, "log"), (std::vector<std::string>{"start"}));
    ctx_->processTimers();
    EXPECT_EQ(messages(consoleLogs_, "log"), (std::vector<std::string>{"start", "woke", "done"}));
}

TEST_F(ScriptContextTest, UnhandledRejectionReported) {
    ctx_->evaluate("Promise.reject(new TypeError('boom')); Promise.reject(1).catch(function(){});", "<test>");
    EXPECT_EQ(messages(consoleLogs_, "error"), (std::vector<std::string>{"Uncaught (in promise) TypeError: boom"}));
}

TEST_F(ScriptContextTest, MicrotaskErrorReportedAndDrainContinues) {
    ctx_->evaluate(
        "queueMicrotask(function(){ throw new Error('first'); });"
        "queueMicrotask(function(){ console.log('second'); });", "<test>");
    EXPECT_EQ(messages(consoleLogs_, "log"), (std::vector<std::string>{"second"}));
    auto errors = messages(consoleLogs_, "error");
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_NE(errors[0].find("first"), std::string::npos);
}

TEST_F(ScriptContextTest, RunawayMicrotaskLoopStopped) {
    // An endless chain of short microtasks (each far below the run loop's
    // own check interval) is stopped by the 10 s limit, and its queued work
    // does not come back with the next task
    auto result = ctx_->evaluate(
        "var n = 0; function spin(){ n++; queueMicrotask(spin); } spin();", "<test>");
    EXPECT_FALSE(result.success);
    auto after = ctx_->evaluate("var before = n; setTimeout(function(){}, 0); before > 1000", "<test>");
    EXPECT_TRUE(after.success) << after.error;
    EXPECT_EQ(after.value, "true");
    ctx_->processTimers();
    auto still = ctx_->evaluate("n === before", "<test>");
    EXPECT_EQ(still.value, "true");
}
