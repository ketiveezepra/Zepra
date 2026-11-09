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

// a|b|c: each `|` wraps everything parsed so far as the preferred branch:
//   split(1, R) <left...> jump(end) R: <right...>
// (the split is inserted before the left side; all offsets are relative, so
// the code it shifts stays valid)
bool RegexCompiler::parseDisjunction() {
    const size_t startPc = program_.code.size();

    if (!parseAlternative()) return false;

    while (!atEnd() && current() == '|') {
        advance(); // consume '|'

        RegexInstr splitInstr(RegexOp::Split);
        program_.code.insert(program_.code.begin() + startPc, splitInstr);

        const size_t jumpAddr = program_.code.size();
        emitJump(0);

        const size_t rightStart = program_.code.size();
        if (!parseAlternative()) return false;

        patchJump(jumpAddr, static_cast<int32_t>(program_.code.size() - jumpAddr));
        patchSplit(startPc, 1, static_cast<int32_t>(rightStart - startPc));
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
        // {n}, {n,}, {n,m}: the engine has no counters, so the atom is
        // unrolled: n required copies, then either a `*` loop ({n,}) or
        // m - n nested optional copies. All jumps are relative, so copies
        // of the atom's code run unchanged.
        const bool unbounded = max == 0xFFFF;
        const size_t copies = static_cast<size_t>(min) + (unbounded ? 1 : static_cast<size_t>(max - min));
        if (max < min) { setError("numbers out of order in {} quantifier"); return false; }
        if (atomLen * copies > 50000) { setError("Quantifier too large"); return false; }
        std::vector<RegexInstr> atom(program_.code.begin() + atomStart, program_.code.end());
        program_.code.resize(atomStart);
        for (uint16_t i = 0; i < min; ++i) {
            program_.code.insert(program_.code.end(), atom.begin(), atom.end());
        }
        if (unbounded) {
            RegexInstr splitInstr(RegexOp::Split);
            if (greedy) splitInstr.split = {1, static_cast<int32_t>(atomLen + 3)};
            else splitInstr.split = {static_cast<int32_t>(atomLen + 3), 1};
            emit(splitInstr);
            program_.code.insert(program_.code.end(), atom.begin(), atom.end());
            emit(RegexInstr(RegexOp::Progress));
            emitJump(-static_cast<int32_t>(atomLen + 2));
        } else {
            const size_t optional = static_cast<size_t>(max - min);
            const size_t end = program_.code.size() + optional * (atomLen + 1);
            for (size_t j = 0; j < optional; ++j) {
                const int32_t toEnd = static_cast<int32_t>(end - program_.code.size());
                RegexInstr splitInstr(RegexOp::Split);
                if (greedy) splitInstr.split = {1, toEnd};
                else splitInstr.split = {toEnd, 1};
                emit(splitInstr);
                program_.code.insert(program_.code.end(), atom.begin(), atom.end());
            }
        }
    }

    return true;
}

namespace {

void addUnicodeSpaces(CharClass& cc) {
    cc.ranges.push_back({' ', ' '});
    cc.ranges.push_back({'\t', '\r'});
    cc.ranges.push_back({0xA0, 0xA0});
    cc.ranges.push_back({0x1680, 0x1680});
    cc.ranges.push_back({0x2000, 0x200A});
    cc.ranges.push_back({0x2028, 0x2029});
    cc.ranges.push_back({0x202F, 0x202F});
    cc.ranges.push_back({0x205F, 0x205F});
    cc.ranges.push_back({0x3000, 0x3000});
    cc.ranges.push_back({0xFEFF, 0xFEFF});
}

void addWordChars(CharClass& cc) {
    cc.ranges.push_back({'a', 'z'});
    cc.ranges.push_back({'A', 'Z'});
    cc.ranges.push_back({'0', '9'});
    cc.ranges.push_back({'_', '_'});
}

// The complement of `ranges` within [0, 0x10FFFF]
std::vector<CharRange> complement(std::vector<CharRange> ranges) {
    std::sort(ranges.begin(), ranges.end(), [](const CharRange& a, const CharRange& b) { return a.lo < b.lo; });
    std::vector<CharRange> out;
    char32_t next = 0;
    for (const auto& r : ranges) {
        if (r.lo > next) out.push_back({next, r.lo - 1});
        if (r.hi + 1 > next) next = r.hi + 1;
    }
    if (next <= 0x10FFFF) out.push_back({next, 0x10FFFF});
    return out;
}

// Unicode property classes for \p{...}. Without the Unicode tables these are
// approximations by block: good for "is this a letter / digit / emoji"
// checks on common scripts, not exact General_Category membership.
bool addProperty(const std::string& name, CharClass& cc) {
    auto letters = [&cc]() {
        cc.ranges.push_back({'A', 'Z'});
        cc.ranges.push_back({'a', 'z'});
        cc.ranges.push_back({0xAA, 0xAA});
        cc.ranges.push_back({0xB5, 0xB5});
        cc.ranges.push_back({0xBA, 0xBA});
        cc.ranges.push_back({0xC0, 0xD6});
        cc.ranges.push_back({0xD8, 0xF6});
        cc.ranges.push_back({0xF8, 0x2FF});
        cc.ranges.push_back({0x370, 0x3FF});
        cc.ranges.push_back({0x400, 0x52F});
        cc.ranges.push_back({0x531, 0x587});
        cc.ranges.push_back({0x5D0, 0x5EA});
        cc.ranges.push_back({0x620, 0x64A});
        cc.ranges.push_back({0x671, 0x6D3});
        cc.ranges.push_back({0x904, 0x939});
        cc.ranges.push_back({0x958, 0x961});
        cc.ranges.push_back({0xE01, 0xE30});
        cc.ranges.push_back({0x10A0, 0x10FF});
        cc.ranges.push_back({0x1100, 0x11FF});
        cc.ranges.push_back({0x1E00, 0x1FFF});
        cc.ranges.push_back({0x3041, 0x3096});
        cc.ranges.push_back({0x30A1, 0x30FA});
        cc.ranges.push_back({0x3400, 0x4DBF});
        cc.ranges.push_back({0x4E00, 0x9FFF});
        cc.ranges.push_back({0xAC00, 0xD7A3});
        cc.ranges.push_back({0xF900, 0xFAFF});
        cc.ranges.push_back({0xFF21, 0xFF3A});
        cc.ranges.push_back({0xFF41, 0xFF5A});
        cc.ranges.push_back({0x20000, 0x2FA1F});
    };
    std::string key = name;
    if (key.rfind("General_Category=", 0) == 0) key = key.substr(17);
    if (key.rfind("gc=", 0) == 0) key = key.substr(3);
    if (key == "L" || key == "Letter" || key == "Alphabetic" || key == "Alpha" || key == "ID_Start" || key == "IDS") {
        letters();
    } else if (key == "Lu" || key == "Uppercase_Letter" || key == "Uppercase") {
        cc.ranges.push_back({'A', 'Z'});
        cc.ranges.push_back({0xC0, 0xD6});
        cc.ranges.push_back({0xD8, 0xDE});
        cc.ranges.push_back({0x391, 0x3A9});
        cc.ranges.push_back({0x410, 0x42F});
    } else if (key == "Ll" || key == "Lowercase_Letter" || key == "Lowercase") {
        cc.ranges.push_back({'a', 'z'});
        cc.ranges.push_back({0xDF, 0xF6});
        cc.ranges.push_back({0xF8, 0xFF});
        cc.ranges.push_back({0x3B1, 0x3C9});
        cc.ranges.push_back({0x430, 0x44F});
    } else if (key == "N" || key == "Number" || key == "Nd" || key == "Decimal_Number" || key == "digit") {
        cc.ranges.push_back({'0', '9'});
        cc.ranges.push_back({0x660, 0x669});
        cc.ranges.push_back({0x6F0, 0x6F9});
        cc.ranges.push_back({0x966, 0x96F});
        cc.ranges.push_back({0xFF10, 0xFF19});
        if (key == "N" || key == "Number") {
            cc.ranges.push_back({0xB2, 0xB3});
            cc.ranges.push_back({0xB9, 0xB9});
            cc.ranges.push_back({0xBC, 0xBE});
            cc.ranges.push_back({0x2150, 0x218B});
        }
    } else if (key == "ID_Continue" || key == "IDC" || key == "Alnum") {
        letters();
        cc.ranges.push_back({'0', '9'});
        cc.ranges.push_back({'_', '_'});
        cc.ranges.push_back({0x300, 0x36F});
    } else if (key == "P" || key == "Punctuation") {
        cc.ranges.push_back({'!', '#'});
        cc.ranges.push_back({'%', '*'});
        cc.ranges.push_back({',', '/'});
        cc.ranges.push_back({':', ';'});
        cc.ranges.push_back({'?', '@'});
        cc.ranges.push_back({'[', ']'});
        cc.ranges.push_back({'_', '_'});
        cc.ranges.push_back({'{', '{'});
        cc.ranges.push_back({'}', '}'});
        cc.ranges.push_back({0xA1, 0xA1});
        cc.ranges.push_back({0xA7, 0xA7});
        cc.ranges.push_back({0xAB, 0xAB});
        cc.ranges.push_back({0xB6, 0xB7});
        cc.ranges.push_back({0xBB, 0xBB});
        cc.ranges.push_back({0xBF, 0xBF});
        cc.ranges.push_back({0x2010, 0x2027});
        cc.ranges.push_back({0x2030, 0x205E});
        cc.ranges.push_back({0x3001, 0x3003});
    } else if (key == "White_Space" || key == "space" || key == "Zs") {
        addUnicodeSpaces(cc);
    } else if (key == "Emoji" || key == "Extended_Pictographic" || key == "Emoji_Presentation" || key == "ExtPict") {
        cc.ranges.push_back({0x1F000, 0x1FAFF});
        cc.ranges.push_back({0x2600, 0x27BF});
        cc.ranges.push_back({0x2B00, 0x2BFF});
        cc.ranges.push_back({0x2190, 0x21FF});
        if (key == "Emoji") {
            cc.ranges.push_back({'#', '#'});
            cc.ranges.push_back({'*', '*'});
            cc.ranges.push_back({'0', '9'});
        }
    } else if (key == "Emoji_Modifier") {
        cc.ranges.push_back({0x1F3FB, 0x1F3FF});
    } else if (key == "Regional_Indicator" || key == "RI") {
        cc.ranges.push_back({0x1F1E6, 0x1F1FF});
    } else if (key == "M" || key == "Mark" || key == "Mn" || key == "Nonspacing_Mark") {
        cc.ranges.push_back({0x300, 0x36F});
        cc.ranges.push_back({0x483, 0x489});
        cc.ranges.push_back({0x591, 0x5BD});
        cc.ranges.push_back({0x610, 0x61A});
        cc.ranges.push_back({0x64B, 0x65F});
        cc.ranges.push_back({0x900, 0x903});
        cc.ranges.push_back({0x93A, 0x94F});
        cc.ranges.push_back({0x1AB0, 0x1AFF});
        cc.ranges.push_back({0x1DC0, 0x1DFF});
        cc.ranges.push_back({0x20D0, 0x20FF});
        cc.ranges.push_back({0xFE00, 0xFE0F});
        cc.ranges.push_back({0xFE20, 0xFE2F});
    } else if (key == "S" || key == "Symbol" || key == "Sm" || key == "Sc") {
        cc.ranges.push_back({'$', '$'});
        cc.ranges.push_back({'+', '+'});
        cc.ranges.push_back({'<', '>'});
        cc.ranges.push_back({'^', '^'});
        cc.ranges.push_back({'`', '`'});
        cc.ranges.push_back({'|', '|'});
        cc.ranges.push_back({'~', '~'});
        cc.ranges.push_back({0xA2, 0xA6});
        cc.ranges.push_back({0x20A0, 0x20C0});
        cc.ranges.push_back({0x2190, 0x22FF});
    } else if (key == "Any") {
        cc.ranges.push_back({0, 0x10FFFF});
    } else if (key == "ASCII") {
        cc.ranges.push_back({0, 0x7F});
    } else if (key == "Script=Latin" || key == "sc=Latin" || key == "Script_Extensions=Latin" || key == "scx=Latin") {
        cc.ranges.push_back({'A', 'Z'});
        cc.ranges.push_back({'a', 'z'});
        cc.ranges.push_back({0xC0, 0x24F});
        cc.ranges.push_back({0x1E00, 0x1EFF});
    } else if (key == "Script=Han" || key == "sc=Han" || key == "scx=Han" || key == "Script_Extensions=Han") {
        cc.ranges.push_back({0x3400, 0x4DBF});
        cc.ranges.push_back({0x4E00, 0x9FFF});
        cc.ranges.push_back({0xF900, 0xFAFF});
        cc.ranges.push_back({0x20000, 0x2FA1F});
    } else if (key == "Script=Cyrillic" || key == "sc=Cyrillic") {
        cc.ranges.push_back({0x400, 0x52F});
    } else if (key == "Script=Greek" || key == "sc=Greek") {
        cc.ranges.push_back({0x370, 0x3FF});
        cc.ranges.push_back({0x1F00, 0x1FFF});
    } else {
        return false;
    }
    return true;
}

}  // namespace

// After the letter of a character escape (n, t, xHH, uXXXX, u{X}, cX, ...):
// its code point. False when `esc` is not a character escape.
bool RegexCompiler::parseCharacterEscape(char32_t esc, char32_t& out) {
    auto hex = [](char32_t c) -> int {
        if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<int>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<int>(c - 'A' + 10);
        return -1;
    };
    auto readHex = [&](int digits, char32_t& value) {
        size_t save = pos_;
        value = 0;
        for (int i = 0; i < digits; ++i) {
            int d = atEnd() ? -1 : hex(current());
            if (d < 0) { pos_ = save; return false; }
            value = value * 16 + static_cast<char32_t>(d);
            advance();
        }
        return true;
    };
    switch (esc) {
        case 'n': out = '\n'; return true;
        case 'r': out = '\r'; return true;
        case 't': out = '\t'; return true;
        case 'f': out = '\f'; return true;
        case 'v': out = '\v'; return true;
        case '0':
            if (!atEnd() && current() >= '0' && current() <= '9') return false;
            out = 0;
            return true;
        case 'x':
            if (!readHex(2, out)) out = 'x';
            return true;
        case 'c':
            if (!atEnd() && ((current() >= 'a' && current() <= 'z') || (current() >= 'A' && current() <= 'Z'))) {
                out = current() % 32;
                advance();
            } else {
                out = '\\';
                pos_--;  // `\c` not followed by a letter is a backslash then `c`
            }
            return true;
        case 'u': {
            if (!atEnd() && current() == '{') {
                size_t save = pos_;
                advance();
                char32_t value = 0;
                int digits = 0;
                while (!atEnd() && hex(current()) >= 0) {
                    value = value * 16 + static_cast<char32_t>(hex(current()));
                    advance();
                    ++digits;
                }
                if (digits && !atEnd() && current() == '}' && value <= 0x10FFFF) {
                    advance();
                    out = value;
                    return true;
                }
                pos_ = save;
                out = 'u';
                return true;
            }
            if (!readHex(4, out)) { out = 'u'; return true; }
            // A surrogate pair written as two escapes is one code point
            if (out >= 0xD800 && out <= 0xDBFF && pos_ + 1 < pattern_.size() &&
                pattern_[pos_] == '\\' && pattern_[pos_ + 1] == 'u') {
                size_t save = pos_;
                pos_ += 2;
                char32_t low;
                if (readHex(4, low) && low >= 0xDC00 && low <= 0xDFFF) {
                    out = 0x10000 + ((out - 0xD800) << 10) + (low - 0xDC00);
                } else {
                    pos_ = save;
                }
            }
            return true;
        }
        default:
            return false;
    }
}

// After the letter of a set escape (\d \D \w \W \s \S \p{..} \P{..}): adds
// its ranges to `cc`. False when `esc` is not one; sets an error on a bad \p.
bool RegexCompiler::addSetEscape(char32_t esc, CharClass& cc) {
    CharClass set;
    switch (esc) {
        case 'd': case 'D': set.ranges.push_back({'0', '9'}); break;
        case 'w': case 'W': addWordChars(set); break;
        case 's': case 'S': addUnicodeSpaces(set); break;
        case 'p': case 'P': {
            if (atEnd() || current() != '{') { setError("Invalid property name"); return true; }
            advance();
            std::string name;
            while (!atEnd() && current() != '}') name += static_cast<char>(advance());
            if (atEnd()) { setError("Invalid property name"); return true; }
            advance();
            if (!addProperty(name, set)) { setError("Invalid property name"); return true; }
            break;
        }
        default:
            return false;
    }
    const bool negate = esc == 'D' || esc == 'W' || esc == 'S' || esc == 'P';
    const auto ranges = negate ? complement(set.ranges) : set.ranges;
    cc.ranges.insert(cc.ranges.end(), ranges.begin(), ranges.end());
    return true;
}

bool RegexCompiler::parseCharClass() {
    advance(); // consume '['

    CharClass cc;
    if (!atEnd() && current() == '^') {
        cc.negated = true;
        advance();
    }

    // One class atom: a character (possibly escaped) in `out`, or a set
    // escape (\d, \w, ...) added straight to cc (isSet)
    auto readAtom = [&](char32_t& out, bool& isSet) -> bool {
        isSet = false;
        char32_t c = advance();
        if (c != '\\') { out = c; return true; }
        if (atEnd()) { setError("Unterminated escape in char class"); return false; }
        char32_t esc = advance();
        if (addSetEscape(esc, cc)) { isSet = true; return error_.empty(); }
        if (esc == 'b') { out = '\b'; return true; }
        if (esc == '-') { out = '-'; return true; }
        if (parseCharacterEscape(esc, out)) return true;
        out = esc;  // identity escape
        return true;
    };

    while (!atEnd() && current() != ']') {
        char32_t start;
        bool startIsSet;
        if (!readAtom(start, startIsSet)) return false;
        if (startIsSet) continue;

        // Range a-z (a '-' before ']' or next to a set escape is literal)
        if (!atEnd() && current() == '-' && pos_ + 1 < pattern_.size() && pattern_[pos_ + 1] != ']') {
            advance();
            char32_t end;
            bool endIsSet;
            if (!readAtom(end, endIsSet)) return false;
            if (endIsSet) {
                cc.ranges.push_back({start, start});
                cc.ranges.push_back({'-', '-'});
                continue;
            }
            if (end < start) {
                setError("Invalid character class range");
                return false;
            }
            cc.ranges.push_back({start, end});
        } else {
            cc.ranges.push_back({start, start});
        }
    }

    if (atEnd()) { setError("Unterminated character class"); return false; }
    advance(); // consume ']'

    const bool negated = cc.negated;
    uint16_t classIdx = addClass(std::move(cc));
    RegexInstr instr(negated ? RegexOp::NegCharClass : RegexOp::CharClass);
    instr.classIdx = classIdx;
    emit(instr);
    return true;
}

bool RegexCompiler::parseEscape() {
    advance(); // consume '\\'
    if (atEnd()) { setError("Unterminated escape"); return false; }

    char32_t c = current();
    advance();

    // \s, \S, \p{...}, \P{...} (and \d \w with their complements)
    if (c == 's' || c == 'S' || c == 'p' || c == 'P') {
        CharClass cc;
        addSetEscape(c, cc);
        if (!error_.empty()) return false;
        uint16_t idx = addClass(std::move(cc));
        RegexInstr instr(RegexOp::CharClass);
        instr.classIdx = idx;
        emit(instr);
        return true;
    }
    // \k<name>: backreference to a named group
    if (c == 'k' && !atEnd() && current() == '<') {
        advance();
        std::string name;
        while (!atEnd() && current() != '>') name += static_cast<char>(advance());
        if (atEnd()) { setError("Invalid named reference"); return false; }
        advance();
        // Groups are named as they open, so a backward reference resolves
        // now; a forward one (always empty at that point) refers to no group
        uint16_t groupIdx = 0xFFFF;
        for (size_t i = 1; i < program_.groupNames.size(); ++i) {
            if (program_.groupNames[i] == name) groupIdx = static_cast<uint16_t>(i);
        }
        RegexInstr instr(RegexOp::BackRef);
        instr.groupIdx = groupIdx;
        emit(instr);
        return true;
    }
    if (c != 'b' && c != 'B' && !(c >= '1' && c <= '9')) {
        char32_t code;
        if (parseCharacterEscape(c, code)) {
            RegexInstr instr(flags_.ignoreCase ? RegexOp::CharIgnoreCase : RegexOp::Char);
            instr.ch = code;
            emit(instr);
            return true;
        }
    }

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
                // Backreference (\1 .. \99)
                uint16_t groupIdx = static_cast<uint16_t>(c - '0');
                if (!atEnd() && current() >= '0' && current() <= '9') {
                    groupIdx = static_cast<uint16_t>(groupIdx * 10 + (current() - '0'));
                    advance();
                }
                RegexInstr instr(RegexOp::BackRef);
                instr.groupIdx = groupIdx;
                emit(instr);
                return true;
            }
            // Literal escape
            {
                RegexInstr instr(flags_.ignoreCase ? RegexOp::CharIgnoreCase : RegexOp::Char);
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

// Pattern characters are code points: a UTF-8 sequence reads as one
size_t RegexCompiler::sequenceLength() const {
    const unsigned char c = static_cast<unsigned char>(pattern_[pos_]);
    size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    return pos_ + n <= pattern_.size() ? n : 1;
}

char32_t RegexCompiler::current() const {
    if (pos_ >= pattern_.size()) return 0;
    const unsigned char c = static_cast<unsigned char>(pattern_[pos_]);
    const size_t n = sequenceLength();
    if (n == 1) return c;
    char32_t code = c & (0x7F >> n);
    for (size_t k = 1; k < n; ++k) code = (code << 6) | (static_cast<unsigned char>(pattern_[pos_ + k]) & 0x3F);
    return code;
}

char32_t RegexCompiler::advance() {
    char32_t c = current();
    if (pos_ < pattern_.size()) pos_ += sequenceLength();
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
