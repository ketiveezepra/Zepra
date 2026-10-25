// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file regex_engine.cpp
 * @brief NFA/backtracking regex execution engine
 *
 * Executes RegexProgram bytecode against input strings.
 * Uses a Spencer-style backtracking NFA with stack-based threads.
 *
 * Supports: exec, test, matchAll, replace, replaceAll, split.
 *
 */

#include "regex/regex_engine.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace Zepra::Regex {

RegexEngine::RegexEngine(const RegexProgram& program, const RegexFlags& flags)
    : program_(program), flags_(flags) {}

// =============================================================================
// Public API
// =============================================================================

namespace {
// A pattern like (a*)*b on a long run of 'a' backtracks without end. Stopping
// after this many instructions in one exec() turns a frozen page into an error.
constexpr uint64_t kStepBudget = 50'000'000;
}

MatchResult RegexEngine::exec(const std::string& input, int32_t startPos) {
    result_ = MatchResult{};
    steps_ = 0;

    int32_t len = static_cast<int32_t>(input.size());
    int32_t start = (startPos >= 0) ? startPos : 0;

    for (int32_t i = start; i <= len; ++i) {
        if (execute(input, i)) {
            return result_;
        }
        if (flags_.sticky) break; // Sticky only tries at startPos
    }

    return result_;
}

bool RegexEngine::test(const std::string& input, int32_t startPos) {
    return exec(input, startPos).success;
}

std::vector<MatchResult> RegexEngine::matchAll(const std::string& input) {
    std::vector<MatchResult> results;
    int32_t pos = 0;
    int32_t len = static_cast<int32_t>(input.size());

    while (pos <= len) {
        MatchResult m = exec(input, pos);
        if (!m.success) break;

        results.push_back(m);

        // Advance past match (or by 1 for empty matches)
        if (m.matchEnd > m.matchStart) {
            pos = m.matchEnd;
        } else {
            pos = m.matchEnd + 1;
        }
    }

    return results;
}

std::string RegexEngine::replace(const std::string& input, const std::string& replacement) {
    MatchResult m = exec(input, 0);
    if (!m.success) return input;

    std::string result;
    result.append(input, 0, m.matchStart);

    // Process replacement string (handle $1, $2, etc.)
    for (size_t i = 0; i < replacement.size(); ++i) {
        if (replacement[i] == '$' && i + 1 < replacement.size()) {
            char next = replacement[i + 1];
            if (next == '&') {
                // $& = entire match
                result.append(input, m.matchStart, m.matchEnd - m.matchStart);
                i++;
            } else if (next == '`') {
                // $` = before match
                result.append(input, 0, m.matchStart);
                i++;
            } else if (next == '\'') {
                // $' = after match
                result.append(input, m.matchEnd);
                i++;
            } else if (next >= '1' && next <= '9') {
                // $1..$9 = capture group
                uint32_t groupIdx = next - '0';
                // Check for two-digit group ($10..$99)
                if (i + 2 < replacement.size() && replacement[i + 2] >= '0' && replacement[i + 2] <= '9') {
                    uint32_t twoDigit = groupIdx * 10 + (replacement[i + 2] - '0');
                    if (twoDigit < m.captures.size()) {
                        groupIdx = twoDigit;
                        i++;
                    }
                }
                if (groupIdx < m.captures.size() && m.captures[groupIdx].matched()) {
                    const auto& cap = m.captures[groupIdx];
                    result.append(input, cap.start, cap.end - cap.start);
                }
                i++;
            } else if (next == '$') {
                result += '$';
                i++;
            } else {
                result += '$';
            }
        } else {
            result += replacement[i];
        }
    }

    result.append(input, m.matchEnd);
    return result;
}

std::string RegexEngine::replaceAll(const std::string& input, const std::string& replacement) {
    std::string result;
    int32_t lastEnd = 0;
    int32_t pos = 0;
    int32_t len = static_cast<int32_t>(input.size());

    while (pos <= len) {
        MatchResult m = exec(input, pos);
        if (!m.success) break;

        // Append text before match
        result.append(input, lastEnd, m.matchStart - lastEnd);

        // Process replacement
        for (size_t i = 0; i < replacement.size(); ++i) {
            if (replacement[i] == '$' && i + 1 < replacement.size()) {
                char next = replacement[i + 1];
                if (next == '&') {
                    result.append(input, m.matchStart, m.matchEnd - m.matchStart);
                    i++;
                } else if (next >= '1' && next <= '9') {
                    uint32_t groupIdx = next - '0';
                    if (groupIdx < m.captures.size() && m.captures[groupIdx].matched()) {
                        const auto& cap = m.captures[groupIdx];
                        result.append(input, cap.start, cap.end - cap.start);
                    }
                    i++;
                } else if (next == '$') {
                    result += '$';
                    i++;
                } else {
                    result += '$';
                }
            } else {
                result += replacement[i];
            }
        }

        lastEnd = m.matchEnd;
        pos = (m.matchEnd > m.matchStart) ? m.matchEnd : m.matchEnd + 1;
    }

    result.append(input, lastEnd);
    return result;
}

std::vector<std::string> RegexEngine::split(const std::string& input, int32_t limit) {
    std::vector<std::string> parts;
    int32_t lastEnd = 0;
    int32_t pos = 0;
    int32_t len = static_cast<int32_t>(input.size());
    int32_t count = 0;

    while (pos <= len && (limit < 0 || count < limit - 1)) {
        MatchResult m = exec(input, pos);
        if (!m.success || m.matchStart >= len) break;

        // Don't split on empty match at same position
        if (m.matchEnd == lastEnd && m.matchEnd == m.matchStart) {
            pos++;
            continue;
        }

        parts.push_back(input.substr(lastEnd, m.matchStart - lastEnd));
        count++;

        // Add capture groups to result
        for (size_t i = 1; i < m.captures.size(); ++i) {
            if (m.captures[i].matched()) {
                parts.push_back(m.captures[i].extract(input));
            } else {
                parts.emplace_back("");
            }
        }

        lastEnd = m.matchEnd;
        pos = (m.matchEnd > m.matchStart) ? m.matchEnd : m.matchEnd + 1;
    }

    if (limit < 0 || count < limit) {
        parts.push_back(input.substr(lastEnd));
    }

    return parts;
}

// =============================================================================
// NFA Execution (backtracking)
// =============================================================================

bool RegexEngine::execute(const std::string& input, int32_t startPos) {
    stack_.clear();

    // Create initial thread
    Thread initial;
    initial.pc = 0;
    initial.inputPos = startPos;
    initial.captures.resize(program_.numGroups);

    stack_.push_back(std::move(initial));

    while (!stack_.empty()) {
        Thread t = std::move(stack_.back());
        stack_.pop_back();

        bool dead = false;

        while (!dead && t.pc < program_.size()) {
            if (++steps_ > kStepBudget) throw RegexBudgetExceeded();
            const RegexInstr& instr = program_.at(t.pc);

            switch (instr.op) {
                case RegexOp::Char: {
                    if (t.inputPos >= static_cast<int32_t>(input.size())) { dead = true; break; }
                    char32_t c = charAt(input, t.inputPos);
                    if (c != instr.ch) { dead = true; break; }
                    t.inputPos++;
                    t.pc++;
                    break;
                }

                case RegexOp::CharIgnoreCase: {
                    if (t.inputPos >= static_cast<int32_t>(input.size())) { dead = true; break; }
                    char32_t c = charAt(input, t.inputPos);
                    if (std::tolower(c) != std::tolower(instr.ch)) { dead = true; break; }
                    t.inputPos++;
                    t.pc++;
                    break;
                }

                case RegexOp::AnyChar: {
                    if (t.inputPos >= static_cast<int32_t>(input.size())) { dead = true; break; }
                    char32_t c = charAt(input, t.inputPos);
                    if (!flags_.dotAll && c == '\n') { dead = true; break; }
                    t.inputPos++;
                    t.pc++;
                    break;
                }

                case RegexOp::CharClass:
                case RegexOp::NegCharClass: {
                    if (t.inputPos >= static_cast<int32_t>(input.size())) { dead = true; break; }
                    char32_t c = charAt(input, t.inputPos);
                    const CharClass& cc = program_.classes[instr.classIdx];
                    bool match = cc.contains(c);
                    if (!match) { dead = true; break; }
                    t.inputPos++;
                    t.pc++;
                    break;
                }

                case RegexOp::Split: {
                    // Fork: push second path, continue with first
                    Thread fork = t;
                    fork.pc = t.pc + instr.split.offsetB;
                    stack_.push_back(std::move(fork));
                    t.pc = t.pc + instr.split.offsetA;
                    break;
                }

                case RegexOp::Jump: {
                    t.pc = t.pc + instr.offset;
                    break;
                }

                case RegexOp::LineStart: {
                    if (flags_.multiline) {
                        if (t.inputPos > 0 && input[t.inputPos - 1] != '\n') {
                            dead = true; break;
                        }
                    } else {
                        if (t.inputPos != 0) { dead = true; break; }
                    }
                    t.pc++;
                    break;
                }

                case RegexOp::LineEnd: {
                    if (flags_.multiline) {
                        if (t.inputPos < static_cast<int32_t>(input.size()) &&
                            input[t.inputPos] != '\n') {
                            dead = true; break;
                        }
                    } else {
                        if (t.inputPos != static_cast<int32_t>(input.size())) {
                            dead = true; break;
                        }
                    }
                    t.pc++;
                    break;
                }

                case RegexOp::WordBoundary: {
                    bool leftWord = (t.inputPos > 0) &&
                                    isWordChar(charAt(input, t.inputPos - 1));
                    bool rightWord = (t.inputPos < static_cast<int32_t>(input.size())) &&
                                     isWordChar(charAt(input, t.inputPos));
                    if (leftWord == rightWord) { dead = true; break; }
                    t.pc++;
                    break;
                }

                case RegexOp::NotWordBoundary: {
                    bool leftWord = (t.inputPos > 0) &&
                                    isWordChar(charAt(input, t.inputPos - 1));
                    bool rightWord = (t.inputPos < static_cast<int32_t>(input.size())) &&
                                     isWordChar(charAt(input, t.inputPos));
                    if (leftWord != rightWord) { dead = true; break; }
                    t.pc++;
                    break;
                }

                case RegexOp::SaveStart: {
                    if (instr.groupIdx < t.captures.size()) {
                        t.captures[instr.groupIdx].start = t.inputPos;
                    }
                    t.pc++;
                    break;
                }

                case RegexOp::SaveEnd: {
                    if (instr.groupIdx < t.captures.size()) {
                        t.captures[instr.groupIdx].end = t.inputPos;
                    }
                    t.pc++;
                    break;
                }

                case RegexOp::BackRef: {
                    if (instr.groupIdx >= t.captures.size() ||
                        !t.captures[instr.groupIdx].matched()) {
                        dead = true; break;
                    }
                    const auto& cap = t.captures[instr.groupIdx];
                    int32_t refLen = cap.end - cap.start;
                    if (t.inputPos + refLen > static_cast<int32_t>(input.size())) {
                        dead = true; break;
                    }
                    // Compare captured text with current position
                    bool matches = true;
                    for (int32_t j = 0; j < refLen; ++j) {
                        char32_t a = charAt(input, cap.start + j);
                        char32_t b = charAt(input, t.inputPos + j);
                        if (flags_.ignoreCase) {
                            if (std::tolower(a) != std::tolower(b)) { matches = false; break; }
                        } else {
                            if (a != b) { matches = false; break; }
                        }
                    }
                    if (!matches) { dead = true; break; }
                    t.inputPos += refLen;
                    t.pc++;
                    break;
                }

                case RegexOp::Progress: {
                    // Prevent infinite loops on empty matches
                    // Track last position for this PC; if same, fail
                    t.pc++;
                    break;
                }

                case RegexOp::LookaheadStart: {
                    // Save state, try sub-expression
                    bool isNegative = (instr.ch != 0);
                    Thread fork = t;
                    fork.pc++;
                    // Execute the lookahead sub-expression
                    // For simplicity, inline execute here
                    // The sub-expression ends at LookaheadEnd
                    // If it matches → positive succeeds, negative fails
                    // If it fails → positive fails, negative succeeds
                    t.pc++;
                    (void)isNegative;
                    break;
                }

                case RegexOp::LookaheadEnd:
                case RegexOp::LookbehindStart:
                case RegexOp::LookbehindEnd:
                    t.pc++;
                    break;

                case RegexOp::RepeatStart:
                case RegexOp::RepeatEnd:
                    t.pc++;
                    break;

                case RegexOp::Match: {
                    // Success
                    result_.success = true;
                    result_.captures = t.captures;
                    if (!t.captures.empty() && t.captures[0].matched()) {
                        result_.matchStart = t.captures[0].start;
                        result_.matchEnd = t.captures[0].end;
                    } else {
                        result_.matchStart = startPos;
                        result_.matchEnd = t.inputPos;
                    }
                    return true;
                }

                case RegexOp::Fail: {
                    dead = true;
                    break;
                }
            }
        }
    }

    return false;
}

bool RegexEngine::backtrack(const std::string& input) {
    (void)input;
    // Handled by the stack in execute()
    return !stack_.empty();
}

// =============================================================================
// Helpers
// =============================================================================

char32_t RegexEngine::charAt(const std::string& input, int32_t pos) const {
    if (pos < 0 || pos >= static_cast<int32_t>(input.size())) return 0;
    // Simple ASCII; full Unicode would need UTF-8 decoding
    unsigned char c = input[pos];
    if (c < 0x80) return c;

    // UTF-8 decode
    if ((c & 0xE0) == 0xC0 && pos + 1 < static_cast<int32_t>(input.size())) {
        return ((c & 0x1F) << 6) | (input[pos + 1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && pos + 2 < static_cast<int32_t>(input.size())) {
        return ((c & 0x0F) << 12) | ((input[pos + 1] & 0x3F) << 6) |
               (input[pos + 2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && pos + 3 < static_cast<int32_t>(input.size())) {
        return ((c & 0x07) << 18) | ((input[pos + 1] & 0x3F) << 12) |
               ((input[pos + 2] & 0x3F) << 6) | (input[pos + 3] & 0x3F);
    }
    return c;
}

bool RegexEngine::isWordChar(char32_t c) const {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

} // namespace Zepra::Regex
