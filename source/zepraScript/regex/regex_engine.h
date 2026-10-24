// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file regex_engine.h
 * @brief NFA/backtracking regex execution engine
 */

#pragma once

#include "regex_bytecode.h"
#include <stdexcept>
#include <string>
#include <vector>
#include <optional>

namespace Zepra::Regex {

struct MatchCapture {
    int32_t start = -1;
    int32_t end = -1;
    bool matched() const { return start >= 0; }
    std::string extract(const std::string& input) const {
        if (!matched()) return "";
        return input.substr(start, end - start);
    }
};

/** Thrown when a pattern backtracks past the step budget (a catastrophic pattern). */
class RegexBudgetExceeded : public std::runtime_error {
public:
    RegexBudgetExceeded() : std::runtime_error("Regular expression is too complex: backtracking limit exceeded") {}
};

struct MatchResult {
    bool success = false;
    int32_t matchStart = -1;
    int32_t matchEnd = -1;
    std::vector<MatchCapture> captures;

    std::string group(uint32_t i) const {
        if (i >= captures.size() || !captures[i].matched()) return "";
        return "";
    }
};

class RegexEngine {
public:
    explicit RegexEngine(const RegexProgram& program, const RegexFlags& flags = {});

    MatchResult exec(const std::string& input, int32_t startPos = 0);
    bool test(const std::string& input, int32_t startPos = 0);

    // Global search: find all matches
    std::vector<MatchResult> matchAll(const std::string& input);

    // Replace
    std::string replace(const std::string& input, const std::string& replacement);
    std::string replaceAll(const std::string& input, const std::string& replacement);

    // Split
    std::vector<std::string> split(const std::string& input, int32_t limit = -1);

private:
    bool execute(const std::string& input, int32_t startPos);
    bool backtrack(const std::string& input);

    struct Thread {
        size_t pc;
        std::vector<MatchCapture> captures;
        int32_t inputPos;
        std::vector<int32_t> counters; // For counted repetition
    };

    char32_t charAt(const std::string& input, int32_t pos) const;
    bool isWordChar(char32_t c) const;

    const RegexProgram& program_;
    RegexFlags flags_;
    std::vector<Thread> stack_;
    MatchResult result_;
    uint64_t steps_ = 0;  // instructions run by the current exec(), bounded by kStepBudget
};

} // namespace Zepra::Regex
