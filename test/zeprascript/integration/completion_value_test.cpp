// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

/**
 * @file completion_value_test.cpp
 * @brief A script's result is its completion value (ECMA-262 14.x
 *        "Runtime Semantics: Evaluation" of each statement): the value of the
 *        last expression statement that ran, with if/loops/switch/try
 *        completing with undefined when their body leaves no value and a
 *        finally block that completes normally not changing it. The console
 *        shows this value for what the user typed.
 */

#include <gtest/gtest.h>

#include "runtime/execution/context.hpp"
#include "runtime/execution/global_object.hpp"
#include "runtime/execution/vm.hpp"

#include <string>

using namespace Zepra::Runtime;

namespace {

class CompletionValue : public ::testing::Test {
protected:
    VM vm{nullptr};
    Context context{&vm};

    void SetUp() override {
        auto* global = new GlobalObject(&context);
        global->initialize();
        global->installInto(vm);
    }

    // The script's result as `String(value)`, or "<error ...>" when it fails
    std::string run(const std::string& source) {
        void* chunk = vm.compile(source, "<test>");
        if (!chunk) return "<does not compile>";
        ExecutionResult result = vm.execute(static_cast<const Zepra::Bytecode::BytecodeChunk*>(chunk));
        if (result.status != ExecutionResult::Status::Success) return "<error " + result.error + ">";
        return result.value.toString();
    }
};

TEST_F(CompletionValue, LastExpressionStatement) {
    EXPECT_EQ(run("1; 2;"), "2");
    EXPECT_EQ(run("var a = 1;"), "undefined");
    EXPECT_EQ(run("1; var b = 2;"), "1");
    EXPECT_EQ(run("1; function f() { return 5; }"), "1");
    EXPECT_EQ(run("1; ;"), "1");
    EXPECT_EQ(run("1; { 2; }"), "2");
    EXPECT_EQ(run("1; { var c = 3; }"), "1");
}

TEST_F(CompletionValue, TryCatchFinally) {
    EXPECT_EQ(run("try { 1; } catch (e) { 2; }"), "1");
    EXPECT_EQ(run("try { throw 0; } catch (e) { 2; }"), "2");
    EXPECT_EQ(run("try { 1; throw 0; } catch (e) { }"), "undefined");
    EXPECT_EQ(run("try { throw 0; } catch { 'no binding'; }"), "no binding");
    EXPECT_EQ(run("try { 1; } finally { 2; }"), "1");
    EXPECT_EQ(run("try { throw 0; } catch (e) { 3; } finally { 4; }"), "3");
    EXPECT_EQ(run("9; try { } catch (e) { }"), "undefined");
    EXPECT_EQ(run("try { try { throw 1; } finally { 5; } } catch (e) { e; }"), "1");
    EXPECT_EQ(run("try { 'x' + 1; } catch (e) { }"), "x1");
}

TEST_F(CompletionValue, IfAndLoops) {
    EXPECT_EQ(run("1; if (true) { 2; }"), "2");
    EXPECT_EQ(run("1; if (false) { 2; }"), "undefined");
    EXPECT_EQ(run("1; if (false) 2; else 3;"), "3");
    EXPECT_EQ(run("var i = 0; while (i < 3) { i++; }"), "2");
    EXPECT_EQ(run("1; while (false) { 2; }"), "undefined");
    EXPECT_EQ(run("for (var j = 0; j < 3; j++) { j * 10; }"), "20");
    EXPECT_EQ(run("1; for (var k = 0; k < 3; k++) { var z = k; }"), "undefined");
    EXPECT_EQ(run("do { 7; break; } while (true);"), "7");
    EXPECT_EQ(run("for (var p of [1, 2]) p + 1;"), "3");
    EXPECT_EQ(run("for (var q in { a: 1 }) q;"), "a");
    EXPECT_EQ(run("outer: for (;;) { 4; break outer; }"), "4");
}

TEST_F(CompletionValue, Switch) {
    EXPECT_EQ(run("switch (2) { case 1: 'one'; break; case 2: 'two'; break; }"), "two");
    EXPECT_EQ(run("1; switch (3) { case 1: 'one'; }"), "undefined");
}

TEST_F(CompletionValue, FunctionsKeepTheirOwnValues) {
    EXPECT_EQ(run("function g() { 1; 2; } g();"), "undefined");
    EXPECT_EQ(run("(function () { try { return 6; } finally { 7; } })();"), "6");
    EXPECT_EQ(run("var h = () => { try { 8; } catch (e) { } return 9; }; h();"), "9");
}

} // namespace
