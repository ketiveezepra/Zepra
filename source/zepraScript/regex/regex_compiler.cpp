// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file regex_compiler.cpp
 * @brief Regex pattern → bytecode compiler
 *
 * Recursive descent parser for ES2024 regex syntax.
 * Compiles patterns into RegexProgram bytecode.
 *
 * Grammar:
 *   Disjunction  = Alternative ('|' Alternative)*
 *   Alternative  = Term*
 *   Term         = Atom Quantifier?
 *   Atom         = Char | CharClass | Group | Escape | Assertion
 *   Quantifier   = ('*' | '+' | '?' | '{n}' | '{n,}' | '{n,m}') '?'?
 *
 */

#include "regex/regex_compiler.h"
#include <algorithm>
#include <cstring>

namespace Zepra::Regex {

RegexCompiler::RegexCompiler(const RegexFlags& flags) : flags_(flags) {}

bool RegexCompiler::compile(const std::string& pattern) {
    pattern_ = pattern;
    pos_ = 0;
    error_.clear();
    program_ = RegexProgram{};
    program_.numGroups = 1; // Group 0 = whole match
    nextGroup_ = 1;

    // Wrap entire pattern in group 0 save
    RegexInstr saveStart(RegexOp::SaveStart);
    saveStart.groupIdx = 0;
    emit(saveStart);

    if (!parseDisjunction()) return false;

    RegexInstr saveEnd(RegexOp::SaveEnd);
    saveEnd.groupIdx = 0;
    emit(saveEnd);

    emit(RegexInstr(RegexOp::Match));

    if (!atEnd()) {
        setError("Unexpected character at end of pattern");
        return false;
    }

    return error_.empty();
}

// =============================================================================
// Parser
// =============================================================================

bool RegexCompiler::parseDisjunction() {
    size_t startPc = program_.code.size();

    if (!parseAlternative()) return false;

    while (!atEnd() && current() == '|') {
        advance(); // consume '|'

        size_t splitAddr = program_.code.size();

        // Emit split: try left alternative first
        // We need to patch after we know the offset
        emitSplit(0, 0);
        size_t rightStart = program_.code.size();

        // Swap: move existing code to be the "left" branch
        // Actually, we insert a Split before the left side and a Jump after it
        // Simpler: emit jump at end of left, then compile right

        // After left alternative, jump over right
        size_t jumpAddr = program_.code.size();
        emitJump(0);

        size_t rightPcStart = program_.code.size();
        if (!parseAlternative()) return false;

        // Patch: split goes to (leftStart+1, rightPcStart)
        // The split was inserted at splitAddr, but actually we need to restructure
        // Let's use the standard Thompson NFA construction:

        // Patch the jump at end of left to skip to after right
        patchJump(jumpAddr, static_cast<int32_t>(program_.code.size() - jumpAddr));

        // Patch the split: offsetA = next (fall through to left), offsetB = rightPcStart
        patchSplit(splitAddr,
                   1, // offsetA = next instruction (relative)
                   static_cast<int32_t>(rightPcStart - splitAddr));

        (void)startPc;
        (void)rightStart;
    }

    return true;
}

bool RegexCompiler::parseAlternative() {
    while (!atEnd() && current() != '|' && current() != ')') {
        if (!parseTerm()) return false;
    }
    return true;
}

bool RegexCompiler::parseTerm() {
    size_t atomStart = program_.code.size();

    if (!parseAtom()) return false;

    // Check for quantifier
    if (!atEnd()) {
        char32_t c = current();
        if (c == '*' || c == '+' || c == '?' || c == '{') {
            return parseQuantifier(atomStart);
        }
    }

    return true;
}

bool RegexCompiler::parseAtom() {
    if (atEnd()) return true; // Empty alternative is valid

    char32_t c = current();

    switch (c) {
        case '(': return parseGroup();
        case '[': return parseCharClass();
        case '\\': return parseEscape();
        case '.': {
            advance();
            emit(RegexInstr(RegexOp::AnyChar));
            return true;
        }
        case '^': {
            advance();
            emit(RegexInstr(RegexOp::LineStart));
            return true;
        }
        case '$': {
            advance();
            emit(RegexInstr(RegexOp::LineEnd));
            return true;
        }
        case ')':
        case '|':
            return true; // End of alternative/group
        case '*':
        case '+':
        case '?':
        case '{':
            setError("Quantifier without preceding atom");
            return false;
        default: {
            advance();
            RegexInstr instr(flags_.ignoreCase ? RegexOp::CharIgnoreCase : RegexOp::Char);
            instr.ch = c;
            emit(instr);
            return true;
        }
    }
}

bool RegexCompiler::parseQuantifier(size_t atomStart) {
    char32_t c = current();
    uint16_t min = 0, max = 0xFFFF; // 0xFFFF = infinity
    bool greedy = true;

    switch (c) {
        case '*': advance(); min = 0; max = 0xFFFF; break;
        case '+': advance(); min = 1; max = 0xFFFF; break;
        case '?': advance(); min = 0; max = 1; break;
        case '{': {
            advance();
            min = static_cast<uint16_t>(parseDecimal());
            if (!atEnd() && current() == ',') {
                advance();
                if (!atEnd() && current() != '}') {
                    max = static_cast<uint16_t>(parseDecimal());
                } else {
                    max = 0xFFFF; // {n,} = n to infinity
                }
            } else {
                max = min; // {n} = exact
            }
            if (atEnd() || current() != '}') {
                setError("Expected '}' in quantifier");
                return false;
            }
            advance();
            break;
        }
        default: return true;
    }

    // Check for lazy modifier
    if (!atEnd() && current() == '?') {
        advance();
        greedy = false;
    }

    // Generate code for quantifier
    size_t atomEnd = program_.code.size();
    size_t atomLen = atomEnd - atomStart;

    if (min == 0 && max == 0xFFFF) {
        // * (or *?)
        // split(body, exit) for greedy; split(exit, body) for lazy
        // body: <atom>
        // jump back to split

        // Layout, relative to the split at s: s split, s+1.. atom, then Progress,
        // then Jump back to s; the loop exits to s + atomLen + 3, past the jump
        RegexInstr splitInstr(RegexOp::Split);
        if (greedy) {
            splitInstr.split = {1, static_cast<int32_t>(atomLen + 3)};
        } else {
            splitInstr.split = {static_cast<int32_t>(atomLen + 3), 1};
        }
        program_.code.insert(program_.code.begin() + atomStart, splitInstr);

        // Add progress guard (prevent infinite empty loops)
        emit(RegexInstr(RegexOp::Progress));

        // Jump back to the split (the jump sits atomLen + 2 after it)
        emitJump(-static_cast<int32_t>(atomLen + 2));

    } else if (min == 1 && max == 0xFFFF) {
        // + (or +?)
        // <atom>
        // split(body, exit)

        emit(RegexInstr(RegexOp::Progress));

        size_t splitAddr = program_.code.size();
        if (greedy) {
            emitSplit(-static_cast<int32_t>(atomLen + 1), 1);
        } else {
            emitSplit(1, -static_cast<int32_t>(atomLen + 1));
        }
        (void)splitAddr;

    } else if (min == 0 && max == 1) {
        // ? (or ??)
        // split(body, exit)
        RegexInstr splitInstr(RegexOp::Split);
        if (greedy) {
            splitInstr.split = {1, static_cast<int32_t>(atomLen + 1)};
        } else {
            splitInstr.split = {static_cast<int32_t>(atomLen + 1), 1};
        }
        program_.code.insert(program_.code.begin() + atomStart, splitInstr);

    } else {
        // {n,m} — emit via RepeatStart/RepeatEnd
        RegexInstr repStart(RegexOp::RepeatStart);
        repStart.repeat = {min, max};
        program_.code.insert(program_.code.begin() + atomStart, repStart);

        emit(RegexInstr(RegexOp::RepeatEnd));
    }

    return true;
}

bool RegexCompiler::parseCharClass() {
    advance(); // consume '['

    CharClass cc;
    if (!atEnd() && current() == '^') {
        cc.negated = true;
        advance();
    }

    while (!atEnd() && current() != ']') {
        char32_t start = current();

        // Handle escape in character class
        if (start == '\\') {
            advance();
            if (atEnd()) { setError("Unterminated escape in char class"); return false; }
            char32_t esc = current();
            switch (esc) {
                case 'd': cc.ranges.push_back({'0', '9'}); advance(); continue;
                case 'D':
                    cc.ranges.push_back({0, '0' - 1});
                    cc.ranges.push_back({'9' + 1, 0x10FFFF}); advance(); continue;
                case 'w':
                    cc.ranges.push_back({'a', 'z'});
                    cc.ranges.push_back({'A', 'Z'});
                    cc.ranges.push_back({'0', '9'});
                    cc.ranges.push_back({'_', '_'}); advance(); continue;
                case 'W':
                    // Simplified: negate \w
                    cc.ranges.push_back({0, '0' - 1});
                    cc.ranges.push_back({'9' + 1, 'A' - 1});
                    cc.ranges.push_back({'Z' + 1, '_' - 1});
                    cc.ranges.push_back({'_' + 1, 'a' - 1});
                    cc.ranges.push_back({'z' + 1, 0x10FFFF}); advance(); continue;
                case 's':
                    cc.ranges.push_back({' ', ' '});
                    cc.ranges.push_back({'\t', '\r'}); advance(); continue;
                case 'n': start = '\n'; advance(); break;
                case 'r': start = '\r'; advance(); break;
                case 't': start = '\t'; advance(); break;
                default: start = esc; advance(); break;
            }
        } else {
            advance();
        }

        // Check for range (a-z)
        if (!atEnd() && current() == '-') {
            advance();
            if (!atEnd() && current() != ']') {
                char32_t end = current();
                advance();
                if (end < start) {
                    setError("Invalid character class range");
                    return false;
                }
                cc.ranges.push_back({start, end});
            } else {
                cc.ranges.push_back({start, start});
                cc.ranges.push_back({'-', '-'});
            }
        } else {
            cc.ranges.push_back({start, start});
        }
    }

    if (atEnd()) { setError("Unterminated character class"); return false; }
    advance(); // consume ']'

    uint16_t classIdx = addClass(std::move(cc));
    RegexInstr instr(cc.negated ? RegexOp::NegCharClass : RegexOp::CharClass);
    instr.classIdx = classIdx;
    emit(instr);

    return true;
}

bool RegexCompiler::parseEscape() {
    advance(); // consume '\\'
    if (atEnd()) { setError("Unterminated escape"); return false; }

    char32_t c = current();
    advance();

    switch (c) {
        case 'd': {
            CharClass cc;
            cc.ranges.push_back({'0', '9'});
            uint16_t idx = addClass(std::move(cc));
            RegexInstr instr(RegexOp::CharClass);
            instr.classIdx = idx;
            emit(instr);
            return true;
        }
        case 'D': {
            CharClass cc;
            cc.ranges.push_back({'0', '9'});
            cc.negated = true;
            uint16_t idx = addClass(std::move(cc));
            RegexInstr instr(RegexOp::NegCharClass);
            instr.classIdx = idx;
            emit(instr);
            return true;
        }
        case 'w': {
            CharClass cc;
            cc.ranges.push_back({'a', 'z'});
            cc.ranges.push_back({'A', 'Z'});
            cc.ranges.push_back({'0', '9'});
            cc.ranges.push_back({'_', '_'});
            uint16_t idx = addClass(std::move(cc));
            RegexInstr instr(RegexOp::CharClass);
            instr.classIdx = idx;
            emit(instr);
            return true;
        }
        case 'W': {
            CharClass cc;
            cc.ranges.push_back({'a', 'z'});
            cc.ranges.push_back({'A', 'Z'});
            cc.ranges.push_back({'0', '9'});
            cc.ranges.push_back({'_', '_'});
            cc.negated = true;
            uint16_t idx = addClass(std::move(cc));
            RegexInstr instr(RegexOp::NegCharClass);
            instr.classIdx = idx;
            emit(instr);
            return true;
        }
        case 's': {
            CharClass cc;
            cc.ranges.push_back({' ', ' '});
            cc.ranges.push_back({'\t', '\r'});
            cc.ranges.push_back({0xA0, 0xA0});
            cc.ranges.push_back({0xFEFF, 0xFEFF});
            uint16_t idx = addClass(std::move(cc));
            RegexInstr instr(RegexOp::CharClass);
            instr.classIdx = idx;
            emit(instr);
            return true;
        }
        case 'S': {
            CharClass cc;
            cc.ranges.push_back({' ', ' '});
            cc.ranges.push_back({'\t', '\r'});
            cc.negated = true;
            uint16_t idx = addClass(std::move(cc));
            RegexInstr instr(RegexOp::NegCharClass);
            instr.classIdx = idx;
            emit(instr);
            return true;
        }
        case 'b':
            emit(RegexInstr(RegexOp::WordBoundary));
            return true;
        case 'B':
            emit(RegexInstr(RegexOp::NotWordBoundary));
            return true;
        case 'n': {
            RegexInstr instr(RegexOp::Char);
            instr.ch = '\n';
            emit(instr);
            return true;
        }
        case 'r': {
            RegexInstr instr(RegexOp::Char);
            instr.ch = '\r';
            emit(instr);
            return true;
        }
        case 't': {
            RegexInstr instr(RegexOp::Char);
            instr.ch = '\t';
            emit(instr);
            return true;
        }
        case '0': {
            RegexInstr instr(RegexOp::Char);
            instr.ch = '\0';
            emit(instr);
            return true;
        }
        default:
            if (c >= '1' && c <= '9') {
                // Backreference
                uint16_t groupIdx = static_cast<uint16_t>(c - '0');
                RegexInstr instr(RegexOp::BackRef);
                instr.groupIdx = groupIdx;
                emit(instr);
                return true;
            }
            // Literal escape
            {
                RegexInstr instr(RegexOp::Char);
                instr.ch = c;
                emit(instr);
                return true;
            }
    }
}

bool RegexCompiler::parseGroup() {
    advance(); // consume '('

    bool capturing = true;
    bool isLookahead = false;
    bool isLookbehind = false;
    bool negated = false;
    std::string groupName;

    // Check for group modifiers
    if (!atEnd() && current() == '?') {
        advance();
        if (atEnd()) { setError("Unexpected end in group"); return false; }

        switch (current()) {
            case ':': // Non-capturing
                advance();
                capturing = false;
                break;
            case '=': // Positive lookahead
                advance();
                isLookahead = true;
                capturing = false;
                break;
            case '!': // Negative lookahead
                advance();
                isLookahead = true;
                negated = true;
                capturing = false;
                break;
            case '<': {
                advance();
                if (!atEnd() && current() == '=') {
                    // Positive lookbehind (?<=...)
                    advance();
                    isLookbehind = true;
                    capturing = false;
                } else if (!atEnd() && current() == '!') {
                    // Negative lookbehind (?<!...)
                    advance();
                    isLookbehind = true;
                    negated = true;
                    capturing = false;
                } else {
                    // Named capturing group (?<name>...)
                    while (!atEnd() && current() != '>') {
                        groupName += static_cast<char>(current());
                        advance();
                    }
                    if (atEnd()) { setError("Unterminated group name"); return false; }
                    advance(); // consume '>'
                }
                break;
            }
            default:
                setError("Invalid group modifier");
                return false;
        }
    }

    uint16_t groupIdx = 0;
    if (capturing) {
        groupIdx = static_cast<uint16_t>(nextGroup_++);
        program_.numGroups = nextGroup_;
        if (!groupName.empty()) {
            if (program_.groupNames.size() <= groupIdx) {
                program_.groupNames.resize(groupIdx + 1);
            }
            program_.groupNames[groupIdx] = groupName;
        }
    }

    if (isLookahead) {
        RegexInstr laStart(RegexOp::LookaheadStart);
        laStart.ch = negated ? 1 : 0;
        emit(laStart);
    } else if (isLookbehind) {
        RegexInstr lbStart(RegexOp::LookbehindStart);
        lbStart.ch = negated ? 1 : 0;
        emit(lbStart);
    } else if (capturing) {
        RegexInstr saveStart(RegexOp::SaveStart);
        saveStart.groupIdx = groupIdx;
        emit(saveStart);
    }

    if (!parseDisjunction()) return false;

    if (atEnd() || current() != ')') {
        setError("Unterminated group");
        return false;
    }
    advance(); // consume ')'

    if (isLookahead) {
        emit(RegexInstr(RegexOp::LookaheadEnd));
    } else if (isLookbehind) {
        emit(RegexInstr(RegexOp::LookbehindEnd));
    } else if (capturing) {
        RegexInstr saveEnd(RegexOp::SaveEnd);
        saveEnd.groupIdx = groupIdx;
        emit(saveEnd);
    }

    return true;
}

// =============================================================================
// Lexer helpers
// =============================================================================

char32_t RegexCompiler::current() const {
    if (pos_ >= pattern_.size()) return 0;
    return static_cast<char32_t>(static_cast<unsigned char>(pattern_[pos_]));
}

char32_t RegexCompiler::advance() {
    char32_t c = current();
    if (pos_ < pattern_.size()) pos_++;
    return c;
}

bool RegexCompiler::match(char32_t c) {
    if (current() == c) { advance(); return true; }
    return false;
}

bool RegexCompiler::atEnd() const {
    return pos_ >= pattern_.size();
}

uint32_t RegexCompiler::parseDecimal() {
    uint32_t val = 0;
    while (!atEnd() && current() >= '0' && current() <= '9') {
        val = val * 10 + (current() - '0');
        advance();
    }
    return val;
}

// =============================================================================
// Code generation
// =============================================================================

size_t RegexCompiler::emit(RegexInstr instr) {
    size_t addr = program_.code.size();
    program_.code.push_back(instr);
    return addr;
}

size_t RegexCompiler::emitSplit(int32_t a, int32_t b) {
    RegexInstr instr(RegexOp::Split);
    instr.split = {a, b};
    return emit(instr);
}

size_t RegexCompiler::emitJump(int32_t offset) {
    RegexInstr instr(RegexOp::Jump);
    instr.offset = offset;
    return emit(instr);
}

void RegexCompiler::patchSplit(size_t addr, int32_t a, int32_t b) {
    program_.code[addr].split = {a, b};
}

void RegexCompiler::patchJump(size_t addr, int32_t offset) {
    program_.code[addr].offset = offset;
}

uint16_t RegexCompiler::addClass(CharClass cc) {
    uint16_t idx = static_cast<uint16_t>(program_.classes.size());
    program_.classes.push_back(std::move(cc));
    return idx;
}

void RegexCompiler::setError(const std::string& msg) {
    if (error_.empty()) {
        error_ = msg + " at position " + std::to_string(pos_);
    }
}

} // namespace Zepra::Regex
