// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file html_tokenizer.hpp
 * @brief HTML5 Spec-compliant Tokenizer (WHATWG HTML §13.2.5)
 *
 * Implements the full HTML5 tokenization state machine as defined by the
 * WHATWG HTML Living Standard. Each state is a discrete enum value;
 * processChar() dispatches to the correct handler.
 *
 * Token types emitted:
 *   DOCTYPE | StartTag | EndTag | Comment | Character | EndOfFile
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <cstdint>
#include <functional>

namespace Zepra::WebCore {

// ============================================================================
// Token Types
// ============================================================================

enum class HTML5TokenType {
    DOCTYPE,
    StartTag,
    EndTag,
    Comment,
    Character,
    EndOfFile
};

/**
 * @brief A single HTML5 token as emitted by the tokenizer
 */
struct HTML5Token {
    HTML5TokenType type = HTML5TokenType::Character;

    // StartTag / EndTag
    std::string tagName;
    bool selfClosing = false;
    bool selfClosingAcked = false;
    std::vector<std::pair<std::string, std::string>> attributes;

    // DOCTYPE
    std::optional<std::string> publicIdentifier;
    std::optional<std::string> systemIdentifier;
    bool forceQuirks = false;

    // Character / Comment
    std::string data; // character data or comment text

    // Helpers
    void addAttribute(const std::string& name, const std::string& value) {
        // Per spec: ignore duplicate attribute names (keep first)
        for (auto& [n, v] : attributes) {
            if (n == name) return;
        }
        attributes.emplace_back(name, value);
    }

    std::string getAttribute(const std::string& name) const {
        for (auto& [n, v] : attributes)
            if (n == name) return v;
        return {};
    }
};

// ============================================================================
// Tokenizer States (WHATWG HTML §13.2.5)
// ============================================================================

enum class TokenizerState {
    Data,
    RCDATA,
    RAWTEXT,
    ScriptData,
    PLAINTEXT,
    TagOpen,
    EndTagOpen,
    TagName,
    RCDATALessThanSign,
    RCDATAEndTagOpen,
    RCDATAEndTagName,
    RAWTEXTLessThanSign,
    RAWTEXTEndTagOpen,
    RAWTEXTEndTagName,
    ScriptDataLessThanSign,
    ScriptDataEndTagOpen,
    ScriptDataEndTagName,
    ScriptDataEscapeStart,
    ScriptDataEscapeStartDash,
    ScriptDataEscaped,
    ScriptDataEscapedDash,
    ScriptDataEscapedDashDash,
    ScriptDataEscapedLessThanSign,
    ScriptDataEscapedEndTagOpen,
    ScriptDataEscapedEndTagName,
    ScriptDataDoubleEscapeStart,
    ScriptDataDoubleEscaped,
    ScriptDataDoubleEscapedDash,
    ScriptDataDoubleEscapedDashDash,
    ScriptDataDoubleEscapedLessThanSign,
    ScriptDataDoubleEscapeEnd,
    BeforeAttributeName,
    AttributeName,
    AfterAttributeName,
    BeforeAttributeValue,
    AttributeValueDoubleQuoted,
    AttributeValueSingleQuoted,
    AttributeValueUnquoted,
    AfterAttributeValueQuoted,
    SelfClosingStartTag,
    BogusComment,
    MarkupDeclarationOpen,
    CommentStart,
    CommentStartDash,
    Comment,
    CommentLessThanSign,
    CommentLessThanSignBang,
    CommentLessThanSignBangDash,
    CommentLessThanSignBangDashDash,
    CommentEndDash,
    CommentEnd,
    CommentEndBang,
    DOCTYPE,
    BeforeDOCTYPEName,
    DOCTYPEName,
    AfterDOCTYPEName,
    AfterDOCTYPEPublicKeyword,
    BeforeDOCTYPEPublicIdentifier,
    DOCTYPEPublicIdentifierDoubleQuoted,
    DOCTYPEPublicIdentifierSingleQuoted,
    AfterDOCTYPEPublicIdentifier,
    BetweenDOCTYPEPublicAndSystemIdentifiers,
    AfterDOCTYPESystemKeyword,
    BeforeDOCTYPESystemIdentifier,
    DOCTYPESystemIdentifierDoubleQuoted,
    DOCTYPESystemIdentifierSingleQuoted,
    AfterDOCTYPESystemIdentifier,
    BogusDOCTYPE,
    CDATASection,
    CDATASectionBracket,
    CDATASectionEnd,
    CharacterReference,
    NamedCharacterReference,
    AmbiguousAmpersand,
    NumericCharacterReference,
    HexadecimalCharacterReferenceStart,
    DecimalCharacterReferenceStart,
    HexadecimalCharacterReference,
    DecimalCharacterReference,
    NumericCharacterReferenceEnd
};

// ============================================================================
// HTML5Tokenizer
// ============================================================================

/**
 * @brief Production-quality HTML5 tokenizer implementing WHATWG §13.2.5
 *
 * Usage:
 *   HTML5Tokenizer tok(htmlSource);
 *   tok.setTokenHandler([](HTML5Token t){ ... });
 *   tok.tokenize();
 */
class HTML5Tokenizer {
public:
    using TokenHandler = std::function<void(HTML5Token&&)>;

    explicit HTML5Tokenizer(const std::string& source);

    /**
     * @brief Set callback invoked for each emitted token
     */
    void setTokenHandler(TokenHandler handler) {
        handler_ = std::move(handler);
    }

    /**
     * @brief Run the tokenizer over the full source
     */
    void tokenize();

    /**
     * @brief Switch tokenizer state (called by tree constructor for raw-text elements)
     * e.g. after parsing <script>, switch to ScriptData state
     */
    void setState(TokenizerState state) { state_ = state; }
    TokenizerState state() const { return state_; }

    /**
     * @brief Set last open tag name (needed for appropriate-end-tag-token checks)
     */
    void setLastStartTagName(const std::string& name) { lastStartTagName_ = name; }

private:
    // ─── Source access ────────────────────────────────────────────────────
    const std::string& src_;
    size_t pos_ = 0;

    // Current char or EOF sentinel
    static constexpr char32_t kEOF = static_cast<char32_t>(-1);
    char32_t nextChar();      // consume and return next codepoint
    char32_t peekChar() const; // peek without consuming
    void reconsume(char32_t c); // push back one char for re-processing
    bool eof() const { return pos_ >= src_.size(); }

    // ─── State machine ────────────────────────────────────────────────────
    TokenizerState state_ = TokenizerState::Data;
    TokenizerState returnState_ = TokenizerState::Data; // for char references

    // ─── Current token being built ────────────────────────────────────────
    HTML5Token current_;
    std::string currentAttrName_;
    std::string currentAttrValue_;
    std::string temporaryBuffer_; // reused across states
    std::string lastStartTagName_;

    // Char reference accumulation
    uint32_t charRefCode_ = 0;

    // ─── Token emission ───────────────────────────────────────────────────
    TokenHandler handler_;
    char32_t reconsumeChar_ = 0;
    bool hasReconsume_ = false;

    void emit(HTML5Token token);
    void emitCurrentTag();
    void emitCharacter(char32_t c);
    void emitString(const std::string& s);
    void emitEOF();

    // Save/commit current attribute before starting new one
    void commitCurrentAttribute();

    // ─── State handlers ───────────────────────────────────────────────────
    // Each processes one character in the given state, returns true to continue
    void processData(char32_t c);
    void processRCDATA(char32_t c);
    void processRAWTEXT(char32_t c);
    void processScriptData(char32_t c);
    void processPLAINTEXT(char32_t c);
    void processTagOpen(char32_t c);
    void processEndTagOpen(char32_t c);
    void processTagName(char32_t c);
    void processBeforeAttributeName(char32_t c);
    void processAttributeName(char32_t c);
    void processAfterAttributeName(char32_t c);
    void processBeforeAttributeValue(char32_t c);
    void processAttributeValueDoubleQuoted(char32_t c);
    void processAttributeValueSingleQuoted(char32_t c);
    void processAttributeValueUnquoted(char32_t c);
    void processAfterAttributeValueQuoted(char32_t c);
    void processSelfClosingStartTag(char32_t c);
    void processMarkupDeclarationOpen(char32_t c);
    void processBogusComment(char32_t c);
    void processCommentStart(char32_t c);
    void processCommentStartDash(char32_t c);
    void processComment(char32_t c);
    void processCommentEndDash(char32_t c);
    void processCommentEnd(char32_t c);
    void processCommentEndBang(char32_t c);
    void processCommentLessThanSign(char32_t c);
    void processCommentLessThanSignBang(char32_t c);
    void processCommentLessThanSignBangDash(char32_t c);
    void processCommentLessThanSignBangDashDash(char32_t c);
    void processDOCTYPE(char32_t c);
    void processBeforeDOCTYPEName(char32_t c);
    void processDOCTYPEName(char32_t c);
    void processAfterDOCTYPEName(char32_t c);
    void processAfterDOCTYPEPublicKeyword(char32_t c);
    void processBeforeDOCTYPEPublicIdentifier(char32_t c);
    void processDOCTYPEPublicIdentifierDoubleQuoted(char32_t c);
    void processDOCTYPEPublicIdentifierSingleQuoted(char32_t c);
    void processAfterDOCTYPEPublicIdentifier(char32_t c);
    void processBetweenDOCTYPEPublicAndSystemIdentifiers(char32_t c);
    void processAfterDOCTYPESystemKeyword(char32_t c);
    void processBeforeDOCTYPESystemIdentifier(char32_t c);
    void processDOCTYPESystemIdentifierDoubleQuoted(char32_t c);
    void processDOCTYPESystemIdentifierSingleQuoted(char32_t c);
    void processAfterDOCTYPESystemIdentifier(char32_t c);
    void processBogusDOCTYPE(char32_t c);
    void processCDATASection(char32_t c);
    void processCDATASectionBracket(char32_t c);
    void processCDATASectionEnd(char32_t c);
    void processCharacterReference(char32_t c);
    void processNumericCharacterReference(char32_t c);
    void processHexadecimalCharacterReference(char32_t c);
    void processDecimalCharacterReference(char32_t c);
    void processNumericCharacterReferenceEnd();

    // Raw-text end-tag helpers (RCDATA / RAWTEXT / Script end-tag states)
    void processRCDATALessThanSign(char32_t c);
    void processRCDATAEndTagOpen(char32_t c);
    void processRCDATAEndTagName(char32_t c);
    void processRAWTEXTLessThanSign(char32_t c);
    void processRAWTEXTEndTagOpen(char32_t c);
    void processRAWTEXTEndTagName(char32_t c);
    void processScriptDataLessThanSign(char32_t c);
    void processScriptDataEndTagOpen(char32_t c);
    void processScriptDataEndTagName(char32_t c);
    void processScriptDataEscapeStart(char32_t c);
    void processScriptDataEscapeStartDash(char32_t c);
    void processScriptDataEscaped(char32_t c);
    void processScriptDataEscapedDash(char32_t c);
    void processScriptDataEscapedDashDash(char32_t c);
    void processScriptDataEscapedLessThanSign(char32_t c);
    void processScriptDataEscapedEndTagOpen(char32_t c);
    void processScriptDataEscapedEndTagName(char32_t c);
    void processScriptDataDoubleEscapeStart(char32_t c);
    void processScriptDataDoubleEscaped(char32_t c);
    void processScriptDataDoubleEscapedDash(char32_t c);
    void processScriptDataDoubleEscapedDashDash(char32_t c);
    void processScriptDataDoubleEscapedLessThanSign(char32_t c);
    void processScriptDataDoubleEscapeEnd(char32_t c);

    // Named character reference table lookup
    std::optional<std::u32string> lookupNamedCharRef(const std::string& name) const;

    // Helper: is this an appropriate end tag token?
    bool isAppropriateEndTagToken() const;

    // Helper: ASCII utils
    static bool isASCIIAlpha(char32_t c)    { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
    static bool isASCIIDigit(char32_t c)    { return c >= '0' && c <= '9'; }
    static bool isASCIIAlphaNum(char32_t c) { return isASCIIAlpha(c) || isASCIIDigit(c); }
    static bool isASCIIHexDigit(char32_t c) { return isASCIIDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
    static bool isASCIIUpper(char32_t c)    { return c >= 'A' && c <= 'Z'; }
    static bool isWhitespace(char32_t c)    { return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20; }
    static char32_t toLowerASCII(char32_t c){ return isASCIIUpper(c) ? (c + 0x20) : c; }

    // UTF-8 decode helpers
    char32_t decodeUTF8();
};

} // namespace Zepra::WebCore
