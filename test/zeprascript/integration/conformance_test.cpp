// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

/**
 * @file conformance_test.cpp
 * @brief Language and built-in conformance cases, each run in a fresh VM
 *
 * test/zeprascript/conformance/*.txt hold one case per line:
 *
 *     name | expression that must evaluate to true
 *
 * A name starting with '!' is a known gap: the case must still fail, and
 * the test fails the day it passes so the marker gets removed. Blank lines
 * and lines starting with '#' are ignored. Every case runs under execution
 * and memory limits, so a runaway script fails that case instead of the run.
 */

#include <gtest/gtest.h>

#include "runtime/execution/Sandbox.h"
#include "runtime/execution/context.hpp"
#include "runtime/execution/global_object.hpp"
#include "runtime/execution/vm.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace Zepra::Runtime;

namespace {

struct Case {
    std::string name;
    std::string expression;
    bool knownGap = false;
};

std::string trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) return "";
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

std::vector<Case> loadCases() {
    std::vector<Case> cases;
    const std::filesystem::path directory = std::filesystem::path(SOURCE_DIR) / "test" / "zeprascript" / "conformance";
    std::vector<std::filesystem::path> files;
    if (std::filesystem::is_directory(directory)) {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.path().extension() == ".txt") files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    for (const auto& file : files) {
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            const size_t bar = line.find('|');
            if (bar == std::string::npos) continue;
            Case c;
            c.name = trim(line.substr(0, bar));
            c.expression = trim(line.substr(bar + 1));
            if (!c.name.empty() && c.name[0] == '!') {
                c.knownGap = true;
                c.name = trim(c.name.substr(1));
            }
            // gtest parameter names are limited to letters, digits and '_'
            for (char& ch : c.name) {
                if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
            }
            cases.push_back(std::move(c));
        }
    }
    return cases;
}

void PrintTo(const Case& c, std::ostream* os) { *os << c.name; }

std::string caseName(const ::testing::TestParamInfo<Case>& info) {
    return info.param.name + (info.param.knownGap ? "_KnownGap" : "");
}

class Conformance : public ::testing::TestWithParam<Case> {};

TEST_P(Conformance, Case) {
    const Case& c = GetParam();

    VM vm(nullptr);
    Context context(&vm);
    auto* global = new GlobalObject(&context);
    global->initialize();
    global->installInto(vm);

    ExecutionLimits limits;
    limits.maxExecutionTime = std::chrono::seconds(10);
    limits.maxMemoryGrowthBytes = size_t(512) * 1024 * 1024;
    limits.maxHeapBytes = SIZE_MAX;
    ResourceMonitor monitor(limits);
    vm.setSandbox(&monitor);

    const std::string source = "var __r;\ntry { __r = (" + c.expression +
                               "); } catch (e) { __r = 'threw ' + e; }\n";
    void* chunk = vm.compile(source, c.name);

    std::string outcome;
    bool passed = false;
    if (!chunk) {
        outcome = "does not compile";
    } else {
        ExecutionResult result = vm.execute(static_cast<const Zepra::Bytecode::BytecodeChunk*>(chunk));
        if (result.status != ExecutionResult::Status::Success) {
            outcome = "failed to run: " + result.error;
        } else {
            const Value value = vm.getGlobal("__r");
            passed = value.isBoolean() && value.asBoolean();
            outcome = passed ? "true" : "evaluated to " + value.toString();
        }
    }
    vm.setSandbox(nullptr);

    if (c.knownGap) {
        EXPECT_FALSE(passed) << c.name << " now passes: remove its '!' marker from the cases file";
    } else {
        EXPECT_TRUE(passed) << c.expression << "\n  " << outcome;
    }
}

INSTANTIATE_TEST_SUITE_P(Cases, Conformance, ::testing::ValuesIn(loadCases()), caseName);

} // namespace
