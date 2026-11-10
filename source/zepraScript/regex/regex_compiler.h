// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file regex_compiler.h
 * @brief Regex pattern → bytecode compiler
 */

#pragma once

#include "regex_bytecode.h"
#include <algorithm>
#include <string>

namespace Zepra::Regex {

class RegexCompiler {
public:
    explicit RegexCompiler(const RegexFlags& flags = {});

    bool compile(const std::string& pattern);

    const RegexProgram& program() const { return program_; }
    RegexProgram takeProgram() { return std::move(program_); }
    const std::string& error() const { return error_; }

private:
    // Recursive descent parser
    bool parseDisjunction();
    bool parseAlternative();
    bool parseTerm();
    bool parseAtom();
    bool parseQuantifier(size_t atomStart);
    bool parseCharClass();
    bool parseEscape();
    bool parseCharacterEscape(char32_t esc, char32_t& out);
    bool addSetEscape(char32_t esc, CharClass& cc);

    bool parseGroup();

    // Helpers
    char32_t current() const;
    size_t sequenceLength() const;
    bool braceQuantifierAhead() const;
    char32_t advance();
    bool match(char32_t c);
    bool atEnd() const;
    uint32_t parseDecimal();

    // Code generation
    size_t emit(RegexInstr instr);
    size_t emitSplit(int32_t a, int32_t b);
    size_t emitJump(int32_t offset);
    void patchSplit(size_t addr, int32_t a, int32_t b);
    void patchJump(size_t addr, int32_t offset);
    uint16_t addClass(CharClass cc);

    void setError(const std::string& msg);

    std::string pattern_;
    size_t pos_ = 0;
    RegexFlags flags_;
    RegexProgram program_;
    std::string error_;
    uint32_t nextGroup_ = 1; // 0 is whole match
};

} // namespace Zepra::Regex
