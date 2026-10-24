// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

/**
 * @file js_execution_test.cpp
 * @brief Real JS execution tests — compiles and runs .js files through the
 *        full Lexer → Parser → SyntaxChecker → BytecodeGenerator → VM pipeline.
 *        Each .js script sets __result to 1 on success, 0 on failure.
 *        This is the ground-truth test for engine health.
 */

#include <gtest/gtest.h>
#include "runtime/execution/vm.hpp"
#include "runtime/execution/global_object.hpp"
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>

using namespace Zepra::Runtime;

// Path to test scripts relative to build dir
static std::string getScriptDir() {
    // Try relative to source tree (standard layout)
    std::vector<std::string> candidates = {
        SOURCE_DIR "/test/zeprascript/scripts",
        "../test/zeprascript/scripts",
        "test/zeprascript/scripts",
    };
    for (const auto& path : candidates) {
        if (std::filesystem::is_directory(path)) {
            return path;
        }
    }
    return SOURCE_DIR "/test/zeprascript/scripts";
}

static std::string readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Helper: compile + execute a .js file, return execution result + check __result global
class JSExecutionTest : public ::testing::Test {
protected:
    VM vm{nullptr};

    struct RunResult {
        bool compiled;
        bool executed;
        std::string error;
        double resultValue;
    };

    RunResult runScript(const std::string& filename) {
        RunResult r{false, false, "", -1.0};

        std::string path = getScriptDir() + "/" + filename;
        std::string source = readFile(path);
        if (source.empty()) {
            r.error = "Failed to read file: " + path;
            return r;
        }

        void* compiled = vm.compile(source, filename);
        if (!compiled) {
            r.error = "Compilation failed for: " + filename;
            return r;
        }
        r.compiled = true;

        auto execResult = vm.execute(
            static_cast<const Zepra::Bytecode::BytecodeChunk*>(compiled));

        // An uncaught exception (Status::Exception) also stops the script
        if (execResult.status != ExecutionResult::Status::Success) {
            r.error = "Runtime error in " + filename + ": " + execResult.error;
            return r;
        }
        r.executed = true;

        Value result = vm.getGlobal("__result");
        if (result.isNumber()) {
            r.resultValue = result.asNumber();
        } else {
            r.error = "__result is not a number in " + filename;
        }
        // Scripts that run several checks name the ones that failed
        Value failedNames = vm.getGlobal("__failedNames");
        if (failedNames.isString()) {
            r.error = "failed checks: " + failedNames.toString();
        }

        return r;
    }
};

// =============================================================================
// Real JS Execution Tests — one per script file
// =============================================================================

TEST_F(JSExecutionTest, Variables) {
    auto r = runScript("01_variables.js");
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.executed) << r.error;
    EXPECT_EQ(r.resultValue, 1.0) << "Variable test assertions failed — engine produced wrong values";
}

TEST_F(JSExecutionTest, Arithmetic) {
    auto r = runScript("02_arithmetic.js");
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.executed) << r.error;
    EXPECT_EQ(r.resultValue, 1.0) << "Arithmetic test assertions failed";
}

TEST_F(JSExecutionTest, Strings) {
    auto r = runScript("03_strings.js");
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.executed) << r.error;
    EXPECT_EQ(r.resultValue, 1.0) << "String operations test failed";
}

TEST_F(JSExecutionTest, ControlFlow) {
    auto r = runScript("04_control_flow.js");
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.executed) << r.error;
    EXPECT_EQ(r.resultValue, 1.0) << "Control flow test failed";
}

TEST_F(JSExecutionTest, Functions) {
    auto r = runScript("05_functions.js");
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.executed) << r.error;
    EXPECT_EQ(r.resultValue, 1.0) << "Function/closure test failed";
}

TEST_F(JSExecutionTest, Objects) {
    auto r = runScript("06_objects.js");
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.executed) << r.error;
    EXPECT_EQ(r.resultValue, 1.0) << "Object operations test failed";
}

TEST_F(JSExecutionTest, Arrays) {
    auto r = runScript("07_arrays.js");
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.executed) << r.error;
    EXPECT_EQ(r.resultValue, 1.0) << "Array operations test failed";
}

TEST_F(JSExecutionTest, Classes) {
    auto r = runScript("08_classes.js");
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.executed) << r.error;
    EXPECT_EQ(r.resultValue, 1.0) << "Class/inheritance test failed";
}

TEST_F(JSExecutionTest, ErrorHandling) {
    auto r = runScript("09_error_handling.js");
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.executed) << r.error;
    EXPECT_EQ(r.resultValue, 1.0) << "Error handling test failed";
}

TEST_F(JSExecutionTest, Operators) {
    auto r = runScript("10_operators.js");
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.executed) << r.error;
    EXPECT_EQ(r.resultValue, 1.0) << "Operator/typeof/bitwise test failed";
}

TEST_F(JSExecutionTest, ClassFeatures) {
    auto r = runScript("11_class_features.js");
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.executed) << r.error;
    EXPECT_EQ(r.resultValue, 1.0) << r.error;
}

TEST_F(JSExecutionTest, LiteralsAndArrows) {
    auto r = runScript("12_literals_and_arrows.js");
    ASSERT_TRUE(r.compiled) << r.error;
    ASSERT_TRUE(r.executed) << r.error;
    EXPECT_EQ(r.resultValue, 1.0) << r.error;
}
