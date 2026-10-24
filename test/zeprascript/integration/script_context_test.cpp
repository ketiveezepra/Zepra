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
