// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file html_tokenizer.cpp
 * @brief HTML5 Tokenizer — full WHATWG state machine implementation
 *
 * Reference: https://html.spec.whatwg.org/multipage/parsing.html#tokenization
 *
 * Design notes:
 *  - Each state handler processes exactly one input character then returns.
 *  - Reconsume is implemented by pushing back one char into reconsumeChar_.
 *  - Named character references use a compact lookup table.
 *  - Parse errors are logged but do not abort parsing (error recovery).
 */

#include "html/html_tokenizer.hpp"
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace Zepra::WebCore {

// ============================================================================
// Named Character Reference table (subset — most common entities)
// Full table has ~2200 entries; we include the ~200 most common.
// ============================================================================

struct NamedCharRef { const char* name; char32_t codepoint1; char32_t codepoint2; };

static const NamedCharRef kNamedCharRefs[] = {
    {"AElig",   0x00C6, 0}, {"AMP",     0x0026, 0}, {"Aacute", 0x00C1, 0},
    {"Acirc",   0x00C2, 0}, {"Agrave",  0x00C0, 0}, {"Alpha",  0x0391, 0},
    {"Atilde",  0x00C3, 0}, {"Auml",    0x00C4, 0}, {"Beta",   0x0392, 0},
    {"COPY",    0x00A9, 0}, {"Ccedil",  0x00C7, 0}, {"Chi",    0x03A7, 0},
    {"Dagger",  0x2021, 0}, {"Delta",   0x0394, 0}, {"ETH",    0x00D0, 0},
    {"Eacute",  0x00C9, 0}, {"Ecirc",   0x00CA, 0}, {"Egrave", 0x00C8, 0},
    {"Epsilon", 0x0395, 0}, {"Eta",     0x0397, 0}, {"Euml",   0x00CB, 0},
    {"GT",      0x003E, 0}, {"Gamma",   0x0393, 0}, {"Iacute", 0x00CD, 0},
    {"Icirc",   0x00CE, 0}, {"Igrave",  0x00CC, 0}, {"Iota",   0x0399, 0},
    {"Iuml",    0x00CF, 0}, {"Kappa",   0x039A, 0}, {"LT",     0x003C, 0},
    {"Lambda",  0x039B, 0}, {"Mu",      0x039C, 0}, {"Ntilde", 0x00D1, 0},
    {"Nu",      0x039D, 0}, {"OElig",   0x0152, 0}, {"Oacute", 0x00D3, 0},
    {"Ocirc",   0x00D4, 0}, {"Ograve",  0x00D2, 0}, {"Omega",  0x03A9, 0},
    {"Omicron", 0x039F, 0}, {"Oslash",  0x00D8, 0}, {"Otilde", 0x00D5, 0},
    {"Ouml",    0x00D6, 0}, {"Phi",     0x03A6, 0}, {"Pi",     0x03A0, 0},
    {"Prime",   0x2033, 0}, {"Psi",     0x03A8, 0}, {"QUOT",   0x0022, 0},
    {"REG",     0x00AE, 0}, {"Rho",     0x03A1, 0}, {"Scaron", 0x0160, 0},
    {"Sigma",   0x03A3, 0}, {"THORN",   0x00DE, 0}, {"TRADE",  0x2122, 0},
    {"Tau",     0x03A4, 0}, {"Theta",   0x0398, 0}, {"Uacute", 0x00DA, 0},
    {"Ucirc",   0x00DB, 0}, {"Ugrave",  0x00D9, 0}, {"Upsilon",0x03A5, 0},
    {"Uuml",    0x00DC, 0}, {"Xi",      0x039E, 0}, {"Yacute", 0x00DD, 0},
    {"Yuml",    0x0178, 0}, {"Zeta",    0x0396, 0}, {"aacute", 0x00E1, 0},
    {"acirc",   0x00E2, 0}, {"acute",   0x00B4, 0}, {"aelig",  0x00E6, 0},
    {"agrave",  0x00E0, 0}, {"alefsym", 0x2135, 0}, {"alpha",  0x03B1, 0},
    {"amp",     0x0026, 0}, {"and",     0x2227, 0}, {"ang",    0x2220, 0},
    {"apos",    0x0027, 0}, {"aring",   0x00E5, 0}, {"asymp",  0x2248, 0},
    {"atilde",  0x00E3, 0}, {"auml",    0x00E4, 0}, {"bdquo",  0x201E, 0},
    {"beta",    0x03B2, 0}, {"brvbar",  0x00A6, 0}, {"bull",   0x2022, 0},
    {"cap",     0x2229, 0}, {"ccedil",  0x00E7, 0}, {"cedil",  0x00B8, 0},
    {"cent",    0x00A2, 0}, {"chi",     0x03C7, 0}, {"circ",   0x02C6, 0},
    {"clubs",   0x2663, 0}, {"cong",    0x2245, 0}, {"copy",   0x00A9, 0},
    {"crarr",   0x21B5, 0}, {"cup",     0x222A, 0}, {"curren", 0x00A4, 0},
    {"dArr",    0x21D3, 0}, {"dagger",  0x2020, 0}, {"darr",   0x2193, 0},
    {"deg",     0x00B0, 0}, {"delta",   0x03B4, 0}, {"diams",  0x2666, 0},
    {"divide",  0x00F7, 0}, {"eacute",  0x00E9, 0}, {"ecirc",  0x00EA, 0},
    {"egrave",  0x00E8, 0}, {"empty",   0x2205, 0}, {"emsp",   0x2003, 0},
    {"ensp",    0x2002, 0}, {"epsilon", 0x03B5, 0}, {"equiv",  0x2261, 0},
    {"eta",     0x03B7, 0}, {"eth",     0x00F0, 0}, {"euml",   0x00EB, 0},
    {"euro",    0x20AC, 0}, {"exist",   0x2203, 0}, {"fnof",   0x0192, 0},
    {"forall",  0x2200, 0}, {"frac12",  0x00BD, 0}, {"frac14", 0x00BC, 0},
    {"frac34",  0x00BE, 0}, {"frasl",   0x2044, 0}, {"gamma",  0x03B3, 0},
    {"ge",      0x2265, 0}, {"gt",      0x003E, 0}, {"hArr",   0x21D4, 0},
    {"harr",    0x2194, 0}, {"hearts",  0x2665, 0}, {"hellip", 0x2026, 0},
    {"iacute",  0x00ED, 0}, {"icirc",   0x00EE, 0}, {"iexcl",  0x00A1, 0},
    {"igrave",  0x00EC, 0}, {"image",   0x2111, 0}, {"infin",  0x221E, 0},
    {"int",     0x222B, 0}, {"iota",    0x03B9, 0}, {"iquest", 0x00BF, 0},
    {"isin",    0x2208, 0}, {"iuml",    0x00EF, 0}, {"kappa",  0x03BA, 0},
    {"lArr",    0x21D0, 0}, {"lambda",  0x03BB, 0}, {"lang",   0x27E8, 0},
    {"laquo",   0x00AB, 0}, {"larr",    0x2190, 0}, {"lceil",  0x2308, 0},
    {"ldquo",   0x201C, 0}, {"le",      0x2264, 0}, {"lfloor", 0x230A, 0},
    {"lowast",  0x2217, 0}, {"loz",     0x25CA, 0}, {"lrm",    0x200E, 0},
    {"lsaquo",  0x2039, 0}, {"lsquo",   0x2018, 0}, {"lt",     0x003C, 0},
    {"macr",    0x00AF, 0}, {"mdash",   0x2014, 0}, {"micro",  0x00B5, 0},
    {"middot",  0x00B7, 0}, {"minus",   0x2212, 0}, {"mu",     0x03BC, 0},
    {"nabla",   0x2207, 0}, {"nbsp",    0x00A0, 0}, {"ndash",  0x2013, 0},
    {"ne",      0x2260, 0}, {"ni",      0x220B, 0}, {"not",    0x00AC, 0},
    {"notin",   0x2209, 0}, {"nsub",    0x2284, 0}, {"ntilde", 0x00F1, 0},
    {"nu",      0x03BD, 0}, {"oacute",  0x00F3, 0}, {"ocirc",  0x00F4, 0},
    {"oelig",   0x0153, 0}, {"ograve",  0x00F2, 0}, {"oline",  0x203E, 0},
    {"omega",   0x03C9, 0}, {"omicron", 0x03BF, 0}, {"oplus",  0x2295, 0},
    {"or",      0x2228, 0}, {"ordf",    0x00AA, 0}, {"ordm",   0x00BA, 0},
    {"oslash",  0x00F8, 0}, {"otilde",  0x00F5, 0}, {"otimes", 0x2297, 0},
    {"ouml",    0x00F6, 0}, {"para",    0x00B6, 0}, {"part",   0x2202, 0},
    {"permil",  0x2030, 0}, {"perp",    0x22A5, 0}, {"phi",    0x03C6, 0},
    {"pi",      0x03C0, 0}, {"piv",     0x03D6, 0}, {"plusmn", 0x00B1, 0},
    {"pound",   0x00A3, 0}, {"prime",   0x2032, 0}, {"prod",   0x220F, 0},
    {"prop",    0x221D, 0}, {"psi",     0x03C8, 0}, {"quot",   0x0022, 0},
    {"rArr",    0x21D2, 0}, {"radic",   0x221A, 0}, {"rang",   0x27E9, 0},
    {"raquo",   0x00BB, 0}, {"rarr",    0x2192, 0}, {"rceil",  0x2309, 0},
    {"rdquo",   0x201D, 0}, {"real",    0x211C, 0}, {"reg",    0x00AE, 0},
    {"rfloor",  0x230B, 0}, {"rho",     0x03C1, 0}, {"rlm",    0x200F, 0},
    {"rsaquo",  0x203A, 0}, {"rsquo",   0x2019, 0}, {"sbquo",  0x201A, 0},
    {"scaron",  0x0161, 0}, {"sdot",    0x22C5, 0}, {"sect",   0x00A7, 0},
    {"shy",     0x00AD, 0}, {"sigma",   0x03C3, 0}, {"sigmaf", 0x03C2, 0},
    {"sim",     0x223C, 0}, {"spades",  0x2660, 0}, {"sub",    0x2282, 0},
    {"sube",    0x2286, 0}, {"sum",     0x2211, 0}, {"sup",    0x2283, 0},
    {"sup1",    0x00B9, 0}, {"sup2",    0x00B2, 0}, {"sup3",   0x00B3, 0},
    {"supe",    0x2287, 0}, {"szlig",   0x00DF, 0}, {"tau",    0x03C4, 0},
    {"there4",  0x2234, 0}, {"theta",   0x03B8, 0}, {"thetasym",0x03D1,0},
    {"thinsp",  0x2009, 0}, {"thorn",   0x00FE, 0}, {"tilde",  0x02DC, 0},
    {"times",   0x00D7, 0}, {"trade",   0x2122, 0}, {"uArr",   0x21D1, 0},
    {"uacute",  0x00FA, 0}, {"uarr",    0x2191, 0}, {"ucirc",  0x00FB, 0},
    {"ugrave",  0x00F9, 0}, {"uml",     0x00A8, 0}, {"upsih",  0x03D2, 0},
    {"upsilon", 0x03C5, 0}, {"uuml",    0x00FC, 0}, {"weierp", 0x2118, 0},
    {"xi",      0x03BE, 0}, {"yacute",  0x00FD, 0}, {"yen",    0x00A5, 0},
    {"yuml",    0x00FF, 0}, {"zeta",    0x03B6, 0}, {"zwj",    0x200D, 0},
    {"zwnj",    0x200C, 0},
};

static constexpr size_t kNamedCharRefCount =
    sizeof(kNamedCharRefs) / sizeof(kNamedCharRefs[0]);

// ============================================================================
// Helper: encode a Unicode codepoint to UTF-8
// ============================================================================
static std::string utf8Encode(char32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

// ============================================================================
// HTML5Tokenizer — Implementation
// ============================================================================

HTML5Tokenizer::HTML5Tokenizer(const std::string& source)
    : src_(source) {}

// ─── Character access ────────────────────────────────────────────────────────

char32_t HTML5Tokenizer::decodeUTF8() {
    if (pos_ >= src_.size()) return kEOF;

    unsigned char c = static_cast<unsigned char>(src_[pos_++]);

    if (c < 0x80) return c;

    char32_t cp = 0;
    int extra = 0;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else return 0xFFFD; // replacement char for bad byte

    for (int i = 0; i < extra; ++i) {
        if (pos_ >= src_.size()) return 0xFFFD;
        unsigned char cont = static_cast<unsigned char>(src_[pos_++]);
        if ((cont & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (cont & 0x3F);
    }
    return cp;
}

char32_t HTML5Tokenizer::nextChar() {
    if (hasReconsume_) {
        hasReconsume_ = false;
        return reconsumeChar_;
    }
    return decodeUTF8();
}

char32_t HTML5Tokenizer::peekChar() const {
    if (hasReconsume_) return reconsumeChar_;
    if (pos_ >= src_.size()) return kEOF;
    // fast path: ASCII
    unsigned char c = static_cast<unsigned char>(src_[pos_]);
    if (c < 0x80) return c;
    // For a proper peek on multi-byte, create a temp copy
    size_t savedPos = pos_;
    // We need a const-cast free approach — cast away const for internal decode
    HTML5Tokenizer* mutableSelf = const_cast<HTML5Tokenizer*>(this);
    char32_t result = mutableSelf->decodeUTF8();
    mutableSelf->pos_ = savedPos;
    return result;
}

void HTML5Tokenizer::reconsume(char32_t c) {
    reconsumeChar_ = c;
    hasReconsume_ = true;
}

// ─── Token emission ───────────────────────────────────────────────────────────

void HTML5Tokenizer::commitCurrentAttribute() {
    if (!currentAttrName_.empty()) {
        current_.addAttribute(currentAttrName_, currentAttrValue_);
        currentAttrName_.clear();
        currentAttrValue_.clear();
    }
}

void HTML5Tokenizer::emit(HTML5Token token) {
    if (handler_) handler_(std::move(token));
}

void HTML5Tokenizer::emitCurrentTag() {
    commitCurrentAttribute();
    emit(std::move(current_));
    current_ = {};
    currentAttrName_.clear();
    currentAttrValue_.clear();
}

void HTML5Tokenizer::emitCharacter(char32_t c) {
    HTML5Token t;
    t.type = HTML5TokenType::Character;
    t.data = utf8Encode(c);
    emit(std::move(t));
}

void HTML5Tokenizer::emitString(const std::string& s) {
    HTML5Token t;
    t.type = HTML5TokenType::Character;
    t.data = s;
    emit(std::move(t));
}

void HTML5Tokenizer::emitEOF() {
    HTML5Token t;
    t.type = HTML5TokenType::EndOfFile;
    emit(std::move(t));
}

bool HTML5Tokenizer::isAppropriateEndTagToken() const {
    return current_.type == HTML5TokenType::EndTag &&
           current_.tagName == lastStartTagName_;
}

// ─── Named char ref lookup ───────────────────────────────────────────────────

std::optional<std::u32string> HTML5Tokenizer::lookupNamedCharRef(
        const std::string& name) const {
    // Binary search on sorted table
    size_t lo = 0, hi = kNamedCharRefCount;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int cmp = std::strcmp(kNamedCharRefs[mid].name, name.c_str());
        if (cmp == 0) {
            std::u32string result;
            result += kNamedCharRefs[mid].codepoint1;
            if (kNamedCharRefs[mid].codepoint2)
                result += kNamedCharRefs[mid].codepoint2;
            return result;
        } else if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return std::nullopt;
}

// ─── Main tokenize loop ───────────────────────────────────────────────────────

void HTML5Tokenizer::tokenize() {
    while (true) {
        char32_t c = nextChar();

        switch (state_) {
        case TokenizerState::Data:                    processData(c); break;
        case TokenizerState::RCDATA:                  processRCDATA(c); break;
        case TokenizerState::RAWTEXT:                 processRAWTEXT(c); break;
        case TokenizerState::ScriptData:              processScriptData(c); break;
        case TokenizerState::PLAINTEXT:               processPLAINTEXT(c); break;
        case TokenizerState::TagOpen:                 processTagOpen(c); break;
        case TokenizerState::EndTagOpen:              processEndTagOpen(c); break;
        case TokenizerState::TagName:                 processTagName(c); break;
        case TokenizerState::BeforeAttributeName:     processBeforeAttributeName(c); break;
        case TokenizerState::AttributeName:           processAttributeName(c); break;
        case TokenizerState::AfterAttributeName:      processAfterAttributeName(c); break;
        case TokenizerState::BeforeAttributeValue:    processBeforeAttributeValue(c); break;
        case TokenizerState::AttributeValueDoubleQuoted: processAttributeValueDoubleQuoted(c); break;
        case TokenizerState::AttributeValueSingleQuoted: processAttributeValueSingleQuoted(c); break;
        case TokenizerState::AttributeValueUnquoted:  processAttributeValueUnquoted(c); break;
        case TokenizerState::AfterAttributeValueQuoted: processAfterAttributeValueQuoted(c); break;
        case TokenizerState::SelfClosingStartTag:     processSelfClosingStartTag(c); break;
        case TokenizerState::MarkupDeclarationOpen:   processMarkupDeclarationOpen(c); break;
        case TokenizerState::BogusComment:            processBogusComment(c); break;
        case TokenizerState::CommentStart:            processCommentStart(c); break;
        case TokenizerState::CommentStartDash:        processCommentStartDash(c); break;
        case TokenizerState::Comment:                          processComment(c); break;
        case TokenizerState::CommentLessThanSign:               processCommentLessThanSign(c); break;
        case TokenizerState::CommentLessThanSignBang:           processCommentLessThanSignBang(c); break;
        case TokenizerState::CommentLessThanSignBangDash:       processCommentLessThanSignBangDash(c); break;
        case TokenizerState::CommentLessThanSignBangDashDash:   processCommentLessThanSignBangDashDash(c); break;
        case TokenizerState::CommentEndDash:                    processCommentEndDash(c); break;
        case TokenizerState::CommentEnd:                        processCommentEnd(c); break;
        case TokenizerState::CommentEndBang:                    processCommentEndBang(c); break;
        case TokenizerState::DOCTYPE:                           processDOCTYPE(c); break;
        case TokenizerState::BeforeDOCTYPEName:       processBeforeDOCTYPEName(c); break;
        case TokenizerState::DOCTYPEName:             processDOCTYPEName(c); break;
        case TokenizerState::AfterDOCTYPEName:        processAfterDOCTYPEName(c); break;
        case TokenizerState::AfterDOCTYPEPublicKeyword: processAfterDOCTYPEPublicKeyword(c); break;
        case TokenizerState::BeforeDOCTYPEPublicIdentifier: processBeforeDOCTYPEPublicIdentifier(c); break;
        case TokenizerState::DOCTYPEPublicIdentifierDoubleQuoted: processDOCTYPEPublicIdentifierDoubleQuoted(c); break;
        case TokenizerState::DOCTYPEPublicIdentifierSingleQuoted: processDOCTYPEPublicIdentifierSingleQuoted(c); break;
        case TokenizerState::AfterDOCTYPEPublicIdentifier: processAfterDOCTYPEPublicIdentifier(c); break;
        case TokenizerState::BetweenDOCTYPEPublicAndSystemIdentifiers: processBetweenDOCTYPEPublicAndSystemIdentifiers(c); break;
        case TokenizerState::AfterDOCTYPESystemKeyword: processAfterDOCTYPESystemKeyword(c); break;
        case TokenizerState::BeforeDOCTYPESystemIdentifier: processBeforeDOCTYPESystemIdentifier(c); break;
        case TokenizerState::DOCTYPESystemIdentifierDoubleQuoted: processDOCTYPESystemIdentifierDoubleQuoted(c); break;
        case TokenizerState::DOCTYPESystemIdentifierSingleQuoted: processDOCTYPESystemIdentifierSingleQuoted(c); break;
        case TokenizerState::AfterDOCTYPESystemIdentifier: processAfterDOCTYPESystemIdentifier(c); break;
        case TokenizerState::BogusDOCTYPE:            processBogusDOCTYPE(c); break;
        case TokenizerState::CDATASection:            processCDATASection(c); break;
        case TokenizerState::CDATASectionBracket:     processCDATASectionBracket(c); break;
        case TokenizerState::CDATASectionEnd:         processCDATASectionEnd(c); break;
        case TokenizerState::CharacterReference:      processCharacterReference(c); break;
        case TokenizerState::NumericCharacterReference: processNumericCharacterReference(c); break;
        case TokenizerState::HexadecimalCharacterReference: processHexadecimalCharacterReference(c); break;
        case TokenizerState::DecimalCharacterReference: processDecimalCharacterReference(c); break;
        case TokenizerState::RCDATALessThanSign:      processRCDATALessThanSign(c); break;
        case TokenizerState::RCDATAEndTagOpen:        processRCDATAEndTagOpen(c); break;
        case TokenizerState::RCDATAEndTagName:        processRCDATAEndTagName(c); break;
        case TokenizerState::RAWTEXTLessThanSign:     processRAWTEXTLessThanSign(c); break;
        case TokenizerState::RAWTEXTEndTagOpen:       processRAWTEXTEndTagOpen(c); break;
        case TokenizerState::RAWTEXTEndTagName:       processRAWTEXTEndTagName(c); break;
        case TokenizerState::ScriptDataLessThanSign:  processScriptDataLessThanSign(c); break;
        case TokenizerState::ScriptDataEndTagOpen:    processScriptDataEndTagOpen(c); break;
        case TokenizerState::ScriptDataEndTagName:    processScriptDataEndTagName(c); break;
        case TokenizerState::ScriptDataEscapeStart:   processScriptDataEscapeStart(c); break;
        case TokenizerState::ScriptDataEscapeStartDash: processScriptDataEscapeStartDash(c); break;
        case TokenizerState::ScriptDataEscaped:       processScriptDataEscaped(c); break;
        case TokenizerState::ScriptDataEscapedDash:   processScriptDataEscapedDash(c); break;
        case TokenizerState::ScriptDataEscapedDashDash: processScriptDataEscapedDashDash(c); break;
        case TokenizerState::ScriptDataEscapedLessThanSign: processScriptDataEscapedLessThanSign(c); break;
        case TokenizerState::ScriptDataEscapedEndTagOpen: processScriptDataEscapedEndTagOpen(c); break;
        case TokenizerState::ScriptDataEscapedEndTagName: processScriptDataEscapedEndTagName(c); break;
        case TokenizerState::ScriptDataDoubleEscapeStart: processScriptDataDoubleEscapeStart(c); break;
        case TokenizerState::ScriptDataDoubleEscaped: processScriptDataDoubleEscaped(c); break;
        case TokenizerState::ScriptDataDoubleEscapedDash: processScriptDataDoubleEscapedDash(c); break;
        case TokenizerState::ScriptDataDoubleEscapedDashDash: processScriptDataDoubleEscapedDashDash(c); break;
        case TokenizerState::ScriptDataDoubleEscapedLessThanSign: processScriptDataDoubleEscapedLessThanSign(c); break;
        case TokenizerState::ScriptDataDoubleEscapeEnd: processScriptDataDoubleEscapeEnd(c); break;
        default:
            // Unknown state — emit char and stay
            if (c == kEOF) { emitEOF(); return; }
            emitCharacter(c);
            break;
        }

        // Check if we emitted EOF — both in handler and via state
        if (c == kEOF && !hasReconsume_) {
            // Most states handle EOF inline; if we reach here something unusual happened
            // but we already emitted EOF in the switch case, so return
            break;
        }
    }
}

// ============================================================================
// State Handlers — Data, Tag, Attribute states
// ============================================================================

void HTML5Tokenizer::processData(char32_t c) {
    if (c == '&') {
        returnState_ = TokenizerState::Data;
        state_ = TokenizerState::CharacterReference;
    } else if (c == '<') {
        state_ = TokenizerState::TagOpen;
    } else if (c == '\0') {
        // parse error — emit replacement char
        emitCharacter(0xFFFD);
    } else if (c == kEOF) {
        emitEOF();
    } else {
        emitCharacter(c);
    }
}

void HTML5Tokenizer::processRCDATA(char32_t c) {
    if (c == '&') {
        returnState_ = TokenizerState::RCDATA;
        state_ = TokenizerState::CharacterReference;
    } else if (c == '<') {
        state_ = TokenizerState::RCDATALessThanSign;
    } else if (c == '\0') {
        emitCharacter(0xFFFD);
    } else if (c == kEOF) {
        emitEOF();
    } else {
        emitCharacter(c);
    }
}

void HTML5Tokenizer::processRAWTEXT(char32_t c) {
    if (c == '<') {
        state_ = TokenizerState::RAWTEXTLessThanSign;
    } else if (c == '\0') {
        emitCharacter(0xFFFD);
    } else if (c == kEOF) {
        emitEOF();
    } else {
        emitCharacter(c);
    }
}

void HTML5Tokenizer::processScriptData(char32_t c) {
    if (c == '<') {
        state_ = TokenizerState::ScriptDataLessThanSign;
    } else if (c == '\0') {
        emitCharacter(0xFFFD);
    } else if (c == kEOF) {
        emitEOF();
    } else {
        emitCharacter(c);
    }
}

void HTML5Tokenizer::processPLAINTEXT(char32_t c) {
    if (c == '\0') {
        emitCharacter(0xFFFD);
    } else if (c == kEOF) {
        emitEOF();
    } else {
        emitCharacter(c);
    }
}

void HTML5Tokenizer::processTagOpen(char32_t c) {
    if (c == '!') {
        state_ = TokenizerState::MarkupDeclarationOpen;
    } else if (c == '/') {
        state_ = TokenizerState::EndTagOpen;
    } else if (isASCIIAlpha(c)) {
        current_ = {};
        current_.type = HTML5TokenType::StartTag;
        current_.tagName += toLowerASCII(c);
        state_ = TokenizerState::TagName;
    } else if (c == '?') {
        // parse error — bogus comment
        current_ = {};
        current_.type = HTML5TokenType::Comment;
        state_ = TokenizerState::BogusComment;
        reconsume(c);
    } else if (c == kEOF) {
        emitCharacter('<');
        emitEOF();
    } else {
        // parse error
        emitCharacter('<');
        state_ = TokenizerState::Data;
        reconsume(c);
    }
}

void HTML5Tokenizer::processEndTagOpen(char32_t c) {
    if (isASCIIAlpha(c)) {
        current_ = {};
        current_.type = HTML5TokenType::EndTag;
        current_.tagName += toLowerASCII(c);
        state_ = TokenizerState::TagName;
    } else if (c == '>') {
        // parse error — missing end tag name
        state_ = TokenizerState::Data;
    } else if (c == kEOF) {
        emitCharacter('<');
        emitCharacter('/');
        emitEOF();
    } else {
        // parse error — bogus comment
        current_ = {};
        current_.type = HTML5TokenType::Comment;
        state_ = TokenizerState::BogusComment;
        reconsume(c);
    }
}

void HTML5Tokenizer::processTagName(char32_t c) {
    if (isWhitespace(c)) {
        state_ = TokenizerState::BeforeAttributeName;
    } else if (c == '/') {
        state_ = TokenizerState::SelfClosingStartTag;
    } else if (c == '>') {
        if (current_.type == HTML5TokenType::StartTag)
            lastStartTagName_ = current_.tagName;
        emitCurrentTag();
        state_ = TokenizerState::Data;
    } else if (isASCIIUpper(c)) {
        current_.tagName += toLowerASCII(c);
    } else if (c == '\0') {
        current_.tagName += utf8Encode(0xFFFD);
    } else if (c == kEOF) {
        // parse error
        emitEOF();
    } else {
        current_.tagName += utf8Encode(c);
    }
}

void HTML5Tokenizer::processBeforeAttributeName(char32_t c) {
    if (isWhitespace(c)) {
        // ignore
    } else if (c == '/' || c == '>') {
        state_ = TokenizerState::AfterAttributeName;
        reconsume(c);
    } else if (c == kEOF) {
        state_ = TokenizerState::AfterAttributeName;
        reconsume(c);
    } else if (c == '=') {
        // parse error — unexpected '=' start
        commitCurrentAttribute();
        currentAttrName_ = "=";
        currentAttrValue_.clear();
        state_ = TokenizerState::AttributeName;
    } else {
        commitCurrentAttribute();
        currentAttrName_.clear();
        currentAttrValue_.clear();
        state_ = TokenizerState::AttributeName;
        reconsume(c);
    }
}

void HTML5Tokenizer::processAttributeName(char32_t c) {
    if (isWhitespace(c) || c == '/' || c == '>') {
        state_ = TokenizerState::AfterAttributeName;
        reconsume(c);
    } else if (c == '=') {
        state_ = TokenizerState::BeforeAttributeValue;
    } else if (isASCIIUpper(c)) {
        currentAttrName_ += toLowerASCII(c);
    } else if (c == '\0') {
        currentAttrName_ += utf8Encode(0xFFFD);
    } else if (c == kEOF) {
        // parse error
        state_ = TokenizerState::AfterAttributeName;
        reconsume(c);
    } else {
        currentAttrName_ += utf8Encode(c);
    }
}

void HTML5Tokenizer::processAfterAttributeName(char32_t c) {
    if (isWhitespace(c)) {
        // ignore
    } else if (c == '/') {
        commitCurrentAttribute();
        state_ = TokenizerState::SelfClosingStartTag;
    } else if (c == '=') {
        state_ = TokenizerState::BeforeAttributeValue;
    } else if (c == '>') {
        commitCurrentAttribute();
        if (current_.type == HTML5TokenType::StartTag)
            lastStartTagName_ = current_.tagName;
        emitCurrentTag();
        state_ = TokenizerState::Data;
    } else if (c == kEOF) {
        // parse error
        emitEOF();
    } else {
        commitCurrentAttribute();
        currentAttrName_.clear();
        currentAttrValue_.clear();
        state_ = TokenizerState::AttributeName;
        reconsume(c);
    }
}

void HTML5Tokenizer::processBeforeAttributeValue(char32_t c) {
    if (isWhitespace(c)) {
        // ignore
    } else if (c == '"') {
        state_ = TokenizerState::AttributeValueDoubleQuoted;
    } else if (c == '\'') {
        state_ = TokenizerState::AttributeValueSingleQuoted;
    } else if (c == '>') {
        // parse error
        commitCurrentAttribute();
        if (current_.type == HTML5TokenType::StartTag)
            lastStartTagName_ = current_.tagName;
        emitCurrentTag();
        state_ = TokenizerState::Data;
    } else {
        state_ = TokenizerState::AttributeValueUnquoted;
        reconsume(c);
    }
}

void HTML5Tokenizer::processAttributeValueDoubleQuoted(char32_t c) {
    if (c == '"') {
        state_ = TokenizerState::AfterAttributeValueQuoted;
    } else if (c == '&') {
        returnState_ = TokenizerState::AttributeValueDoubleQuoted;
        state_ = TokenizerState::CharacterReference;
    } else if (c == '\0') {
        currentAttrValue_ += utf8Encode(0xFFFD);
    } else if (c == kEOF) {
        // parse error
        emitEOF();
    } else {
        currentAttrValue_ += utf8Encode(c);
    }
}

void HTML5Tokenizer::processAttributeValueSingleQuoted(char32_t c) {
    if (c == '\'') {
        state_ = TokenizerState::AfterAttributeValueQuoted;
    } else if (c == '&') {
        returnState_ = TokenizerState::AttributeValueSingleQuoted;
        state_ = TokenizerState::CharacterReference;
    } else if (c == '\0') {
        currentAttrValue_ += utf8Encode(0xFFFD);
    } else if (c == kEOF) {
        emitEOF();
    } else {
        currentAttrValue_ += utf8Encode(c);
    }
}

void HTML5Tokenizer::processAttributeValueUnquoted(char32_t c) {
    if (isWhitespace(c)) {
        commitCurrentAttribute();
        state_ = TokenizerState::BeforeAttributeName;
    } else if (c == '&') {
        returnState_ = TokenizerState::AttributeValueUnquoted;
        state_ = TokenizerState::CharacterReference;
    } else if (c == '>') {
        commitCurrentAttribute();
        if (current_.type == HTML5TokenType::StartTag)
            lastStartTagName_ = current_.tagName;
        emitCurrentTag();
        state_ = TokenizerState::Data;
    } else if (c == '\0') {
        currentAttrValue_ += utf8Encode(0xFFFD);
    } else if (c == kEOF) {
        emitEOF();
    } else {
        currentAttrValue_ += utf8Encode(c);
    }
}

void HTML5Tokenizer::processAfterAttributeValueQuoted(char32_t c) {
    if (isWhitespace(c)) {
        commitCurrentAttribute();
        state_ = TokenizerState::BeforeAttributeName;
    } else if (c == '/') {
        commitCurrentAttribute();
        state_ = TokenizerState::SelfClosingStartTag;
    } else if (c == '>') {
        commitCurrentAttribute();
        if (current_.type == HTML5TokenType::StartTag)
            lastStartTagName_ = current_.tagName;
        emitCurrentTag();
        state_ = TokenizerState::Data;
    } else if (c == kEOF) {
        emitEOF();
    } else {
        // parse error
        commitCurrentAttribute();
        state_ = TokenizerState::BeforeAttributeName;
        reconsume(c);
    }
}

void HTML5Tokenizer::processSelfClosingStartTag(char32_t c) {
    if (c == '>') {
        current_.selfClosing = true;
        if (current_.type == HTML5TokenType::StartTag)
            lastStartTagName_ = current_.tagName;
        emitCurrentTag();
        state_ = TokenizerState::Data;
    } else if (c == kEOF) {
        emitEOF();
    } else {
        // parse error
        state_ = TokenizerState::BeforeAttributeName;
        reconsume(c);
    }
}

// ============================================================================
// Markup Declaration / Comment / DOCTYPE States
// ============================================================================

void HTML5Tokenizer::processMarkupDeclarationOpen(char32_t c) {
    // Check next characters for "--", "DOCTYPE", "[CDATA["
    // We must look ahead without consuming — use peekChar + pattern matching
    // Since we already consumed c in the main loop, we need to check from here

    // Put c back and scan ahead
    std::string ahead;
    ahead += utf8Encode(c);
    // Collect up to 7 more characters for lookahead
    std::vector<char32_t> peeked;
    peeked.push_back(c);
    size_t savedPos = pos_;
    bool savedReconsume = hasReconsume_;
    char32_t savedReconsumeChar = reconsumeChar_;
    hasReconsume_ = false;

    // Read up to 6 more
    for (int i = 0; i < 6; ++i) {
        char32_t nc = decodeUTF8();
        if (nc == kEOF) break;
        peeked.push_back(nc);
    }
    // Restore
    pos_ = savedPos;
    hasReconsume_ = savedReconsume;
    reconsumeChar_ = savedReconsumeChar;

    // Check "--"
    if (peeked.size() >= 2 && peeked[0] == '-' && peeked[1] == '-') {
        // Consume the two dashes
        nextChar(); nextChar();
        current_ = {};
        current_.type = HTML5TokenType::Comment;
        state_ = TokenizerState::CommentStart;
        return;
    }

    // Check "DOCTYPE" (case-insensitive)
    auto matchStr = [&](const char* s) -> bool {
        size_t len = std::strlen(s);
        if (peeked.size() < len) return false;
        for (size_t i = 0; i < len; ++i)
            if (toLowerASCII(peeked[i]) != static_cast<char32_t>(s[i])) return false;
        return true;
    };

    if (matchStr("doctype")) {
        for (int i = 0; i < 7; ++i) nextChar();
        current_ = {};
        current_.type = HTML5TokenType::DOCTYPE;
        state_ = TokenizerState::DOCTYPE;
        return;
    }

    if (matchStr("[cdata[")) {
        for (int i = 0; i < 7; ++i) nextChar();
        // If in foreign content (SVG/MathML) — emit CDATA section
        // For now, treat as bogus comment
        current_ = {};
        current_.type = HTML5TokenType::Comment;
        current_.data = "[CDATA[";
        state_ = TokenizerState::BogusComment;
        return;
    }

    // parse error — bogus comment
    current_ = {};
    current_.type = HTML5TokenType::Comment;
    state_ = TokenizerState::BogusComment;
    reconsume(c);
}

void HTML5Tokenizer::processBogusComment(char32_t c) {
    if (c == '>') {
        emit(std::move(current_));
        current_ = {};
        state_ = TokenizerState::Data;
    } else if (c == kEOF) {
        emit(std::move(current_));
        emitEOF();
    } else if (c == '\0') {
        current_.data += utf8Encode(0xFFFD);
    } else {
        current_.data += utf8Encode(c);
    }
}

void HTML5Tokenizer::processCommentStart(char32_t c) {
    if (c == '-') {
        state_ = TokenizerState::CommentStartDash;
    } else if (c == '>') {
        // parse error
        emit(std::move(current_));
        current_ = {};
        state_ = TokenizerState::Data;
    } else {
        state_ = TokenizerState::Comment;
        reconsume(c);
    }
}

void HTML5Tokenizer::processCommentStartDash(char32_t c) {
    if (c == '-') {
        state_ = TokenizerState::CommentEnd;
    } else if (c == '>') {
        // parse error
        emit(std::move(current_));
        current_ = {};
        state_ = TokenizerState::Data;
    } else if (c == kEOF) {
        // parse error
        emit(std::move(current_));
        emitEOF();
    } else {
        current_.data += '-';
        state_ = TokenizerState::Comment;
        reconsume(c);
    }
}

void HTML5Tokenizer::processComment(char32_t c) {
    if (c == '<') {
        current_.data += '<';
        state_ = TokenizerState::CommentLessThanSign;
    } else if (c == '-') {
        state_ = TokenizerState::CommentEndDash;
    } else if (c == '\0') {
        current_.data += utf8Encode(0xFFFD);
    } else if (c == kEOF) {
        // parse error
        emit(std::move(current_));
        emitEOF();
    } else {
        current_.data += utf8Encode(c);
    }
}

void HTML5Tokenizer::processCommentEndDash(char32_t c) {
    if (c == '-') {
        state_ = TokenizerState::CommentEnd;
    } else if (c == kEOF) {
        emit(std::move(current_));
        emitEOF();
    } else {
        current_.data += '-';
        state_ = TokenizerState::Comment;
        reconsume(c);
    }
}

void HTML5Tokenizer::processCommentEnd(char32_t c) {
    if (c == '>') {
        emit(std::move(current_));
        current_ = {};
        state_ = TokenizerState::Data;
    } else if (c == '!') {
        state_ = TokenizerState::CommentEndBang;
    } else if (c == '-') {
        current_.data += '-';
    } else if (c == kEOF) {
        emit(std::move(current_));
        emitEOF();
    } else {
        current_.data += '-';
        current_.data += '-';
        state_ = TokenizerState::Comment;
        reconsume(c);
    }
}

void HTML5Tokenizer::processCommentEndBang(char32_t c) {
    if (c == '-') {
        current_.data += '-';
        current_.data += '-';
        current_.data += '!';
        state_ = TokenizerState::CommentEndDash;
    } else if (c == '>') {
        // parse error
        emit(std::move(current_));
        current_ = {};
        state_ = TokenizerState::Data;
    } else if (c == kEOF) {
        emit(std::move(current_));
        emitEOF();
    } else {
        current_.data += '-';
        current_.data += '-';
        current_.data += '!';
        state_ = TokenizerState::Comment;
        reconsume(c);
    }
}

// ============================================================================
// DOCTYPE States
// ============================================================================

void HTML5Tokenizer::processDOCTYPE(char32_t c) {
    if (isWhitespace(c)) {
        state_ = TokenizerState::BeforeDOCTYPEName;
    } else if (c == '>') {
        state_ = TokenizerState::BeforeDOCTYPEName;
        reconsume(c);
    } else if (c == kEOF) {
        current_.forceQuirks = true;
        emit(std::move(current_));
        emitEOF();
    } else {
        // parse error
        state_ = TokenizerState::BeforeDOCTYPEName;
        reconsume(c);
    }
}

void HTML5Tokenizer::processBeforeDOCTYPEName(char32_t c) {
    if (isWhitespace(c)) {
        // ignore
    } else if (isASCIIUpper(c)) {
        current_.tagName += toLowerASCII(c);
        state_ = TokenizerState::DOCTYPEName;
    } else if (c == '\0') {
        current_.tagName += utf8Encode(0xFFFD);
        state_ = TokenizerState::DOCTYPEName;
    } else if (c == '>') {
        // parse error
        current_.forceQuirks = true;
        emit(std::move(current_));
        current_ = {};
        state_ = TokenizerState::Data;
    } else if (c == kEOF) {
        current_.forceQuirks = true;
        emit(std::move(current_));
        emitEOF();
    } else {
        current_.tagName += utf8Encode(c);
        state_ = TokenizerState::DOCTYPEName;
    }
}

void HTML5Tokenizer::processDOCTYPEName(char32_t c) {
    if (isWhitespace(c)) {
        state_ = TokenizerState::AfterDOCTYPEName;
    } else if (c == '>') {
        emit(std::move(current_));
        current_ = {};
        state_ = TokenizerState::Data;
    } else if (isASCIIUpper(c)) {
        current_.tagName += toLowerASCII(c);
    } else if (c == '\0') {
        current_.tagName += utf8Encode(0xFFFD);
    } else if (c == kEOF) {
        current_.forceQuirks = true;
        emit(std::move(current_));
        emitEOF();
    } else {
        current_.tagName += utf8Encode(c);
    }
}

void HTML5Tokenizer::processAfterDOCTYPEName(char32_t c) {
    if (isWhitespace(c)) {
        // ignore
    } else if (c == '>') {
        emit(std::move(current_));
        current_ = {};
        state_ = TokenizerState::Data;
    } else if (c == kEOF) {
        current_.forceQuirks = true;
        emit(std::move(current_));
        emitEOF();
    } else {
        // Check for "public" or "system" keywords (case-insensitive)
        // Peek next 5 chars
        std::vector<char32_t> peeked;
        peeked.push_back(c);
        size_t savedPos = pos_;
        bool savedR = hasReconsume_; char32_t savedRC = reconsumeChar_;
        hasReconsume_ = false;
        for (int i = 0; i < 5; ++i) {
            char32_t nc = decodeUTF8();
            if (nc == kEOF) break;
            peeked.push_back(nc);
        }
        pos_ = savedPos; hasReconsume_ = savedR; reconsumeChar_ = savedRC;

        auto matchKw = [&](const char* kw) {
            size_t len = std::strlen(kw);
            if (peeked.size() < len) return false;
            for (size_t i = 0; i < len; ++i)
                if (toLowerASCII(peeked[i]) != static_cast<char32_t>(kw[i])) return false;
            return true;
        };

        if (matchKw("public")) {
            for (int i = 0; i < 6; ++i) nextChar();
            state_ = TokenizerState::AfterDOCTYPEPublicKeyword;
        } else if (matchKw("system")) {
            for (int i = 0; i < 6; ++i) nextChar();
            state_ = TokenizerState::AfterDOCTYPESystemKeyword;
        } else {
            // parse error
            current_.forceQuirks = true;
            state_ = TokenizerState::BogusDOCTYPE;
        }
    }
}

void HTML5Tokenizer::processAfterDOCTYPEPublicKeyword(char32_t c) {
    if (isWhitespace(c)) {
        state_ = TokenizerState::BeforeDOCTYPEPublicIdentifier;
    } else if (c == '"') {
        current_.publicIdentifier = "";
        state_ = TokenizerState::DOCTYPEPublicIdentifierDoubleQuoted;
    } else if (c == '\'') {
        current_.publicIdentifier = "";
        state_ = TokenizerState::DOCTYPEPublicIdentifierSingleQuoted;
    } else if (c == '>') {
        current_.forceQuirks = true;
        emit(std::move(current_));
        current_ = {};
        state_ = TokenizerState::Data;
    } else if (c == kEOF) {
        current_.forceQuirks = true;
        emit(std::move(current_));
        emitEOF();
    } else {
        current_.forceQuirks = true;
        state_ = TokenizerState::BogusDOCTYPE;
    }
}

void HTML5Tokenizer::processBeforeDOCTYPEPublicIdentifier(char32_t c) {
    if (isWhitespace(c)) {
        // ignore
    } else if (c == '"') {
        current_.publicIdentifier = "";
        state_ = TokenizerState::DOCTYPEPublicIdentifierDoubleQuoted;
    } else if (c == '\'') {
        current_.publicIdentifier = "";
        state_ = TokenizerState::DOCTYPEPublicIdentifierSingleQuoted;
    } else if (c == '>') {
        current_.forceQuirks = true;
        emit(std::move(current_));
        current_ = {};
        state_ = TokenizerState::Data;
    } else if (c == kEOF) {
        current_.forceQuirks = true;
        emit(std::move(current_));
        emitEOF();
    } else {
        current_.forceQuirks = true;
        state_ = TokenizerState::BogusDOCTYPE;
    }
}

void HTML5Tokenizer::processDOCTYPEPublicIdentifierDoubleQuoted(char32_t c) {
    if (c == '"') {
        state_ = TokenizerState::AfterDOCTYPEPublicIdentifier;
    } else if (c == '\0') {
        *current_.publicIdentifier += utf8Encode(0xFFFD);
    } else if (c == '>') {
        current_.forceQuirks = true;
        emit(std::move(current_));
        current_ = {};
        state_ = TokenizerState::Data;
    } else if (c == kEOF) {
        current_.forceQuirks = true;
        emit(std::move(current_));
        emitEOF();
    } else {
        *current_.publicIdentifier += utf8Encode(c);
    }
}

void HTML5Tokenizer::processDOCTYPEPublicIdentifierSingleQuoted(char32_t c) {
    if (c == '\'') {
        state_ = TokenizerState::AfterDOCTYPEPublicIdentifier;
    } else if (c == '\0') {
        *current_.publicIdentifier += utf8Encode(0xFFFD);
    } else if (c == '>') {
        current_.forceQuirks = true;
        emit(std::move(current_));
        current_ = {};
        state_ = TokenizerState::Data;
    } else if (c == kEOF) {
        current_.forceQuirks = true;
        emit(std::move(current_));
        emitEOF();
    } else {
        *current_.publicIdentifier += utf8Encode(c);
    }
}

void HTML5Tokenizer::processAfterDOCTYPEPublicIdentifier(char32_t c) {
    if (isWhitespace(c)) {
        state_ = TokenizerState::BetweenDOCTYPEPublicAndSystemIdentifiers;
    } else if (c == '>') {
        emit(std::move(current_));
        current_ = {};
        state_ = TokenizerState::Data;
    } else if (c == '"') {
        current_.systemIdentifier = "";
        state_ = TokenizerState::DOCTYPESystemIdentifierDoubleQuoted;
    } else if (c == '\'') {
        current_.systemIdentifier = "";
        state_ = TokenizerState::DOCTYPESystemIdentifierSingleQuoted;
    } else if (c == kEOF) {
        current_.forceQuirks = true;
        emit(std::move(current_));
        emitEOF();
    } else {
        current_.forceQuirks = true;
        state_ = TokenizerState::BogusDOCTYPE;
    }
}

void HTML5Tokenizer::processBetweenDOCTYPEPublicAndSystemIdentifiers(char32_t c) {
    if (isWhitespace(c)) {
        // ignore
    } else if (c == '>') {
        emit(std::move(current_));
        current_ = {};
        state_ = TokenizerState::Data;
    } else if (c == '"') {
        current_.systemIdentifier = "";
        state_ = TokenizerState::DOCTYPESystemIdentifierDoubleQuoted;
    } else if (c == '\'') {
        current_.systemIdentifier = "";
        state_ = TokenizerState::DOCTYPESystemIdentifierSingleQuoted;
    } else if (c == kEOF) {
        current_.forceQuirks = true;
        emit(std::move(current_));
        emitEOF();
    } else {
        current_.forceQuirks = true;
        state_ = TokenizerState::BogusDOCTYPE;
    }
}

void HTML5Tokenizer::processAfterDOCTYPESystemKeyword(char32_t c) {
    if (isWhitespace(c)) {
        state_ = TokenizerState::BeforeDOCTYPESystemIdentifier;
    } else if (c == '"') {
        current_.systemIdentifier = "";
        state_ = TokenizerState::DOCTYPESystemIdentifierDoubleQuoted;
    } else if (c == '\'') {
        current_.systemIdentifier = "";
        state_ = TokenizerState::DOCTYPESystemIdentifierSingleQuoted;
    } else if (c == '>') {
        current_.forceQuirks = true;
        emit(std::move(current_));
        current_ = {};
        state_ = TokenizerState::Data;
    } else if (c == kEOF) {
        current_.forceQuirks = true;
        emit(std::move(current_));
        emitEOF();
    } else {
        current_.forceQuirks = true;
        state_ = TokenizerState::BogusDOCTYPE;
    }
}

void HTML5Tokenizer::processBeforeDOCTYPESystemIdentifier(char32_t c) {
    if (isWhitespace(c)) {
        // ignore
    } else if (c == '"') {
        current_.systemIdentifier = "";
        state_ = TokenizerState::DOCTYPESystemIdentifierDoubleQuoted;
    } else if (c == '\'') {
        current_.systemIdentifier = "";
        state_ = TokenizerState::DOCTYPESystemIdentifierSingleQuoted;
    } else if (c == '>') {
        current_.forceQuirks = true;
        emit(std::move(current_));
        current_ = {};
        state_ = TokenizerState::Data;
    } else if (c == kEOF) {
        current_.forceQuirks = true;
        emit(std::move(current_));
        emitEOF();
    } else {
        current_.forceQuirks = true;
        state_ = TokenizerState::BogusDOCTYPE;
    }
}

void HTML5Tokenizer::processDOCTYPESystemIdentifierDoubleQuoted(char32_t c) {
    if (c == '"') {
        state_ = TokenizerState::AfterDOCTYPESystemIdentifier;
    } else if (c == '\0') {
        *current_.systemIdentifier += utf8Encode(0xFFFD);
    } else if (c == '>') {
        current_.forceQuirks = true;
        emit(std::move(current_));
        current_ = {};
        state_ = TokenizerState::Data;
    } else if (c == kEOF) {
        current_.forceQuirks = true;
        emit(std::move(current_));
        emitEOF();
    } else {
        *current_.systemIdentifier += utf8Encode(c);
    }
}

void HTML5Tokenizer::processDOCTYPESystemIdentifierSingleQuoted(char32_t c) {
    if (c == '\'') {
        state_ = TokenizerState::AfterDOCTYPESystemIdentifier;
    } else if (c == '\0') {
        *current_.systemIdentifier += utf8Encode(0xFFFD);
    } else if (c == '>') {
        current_.forceQuirks = true;
        emit(std::move(current_));
        current_ = {};
        state_ = TokenizerState::Data;
    } else if (c == kEOF) {
        current_.forceQuirks = true;
        emit(std::move(current_));
        emitEOF();
    } else {
        *current_.systemIdentifier += utf8Encode(c);
    }
}

void HTML5Tokenizer::processAfterDOCTYPESystemIdentifier(char32_t c) {
    if (isWhitespace(c)) {
        // ignore
    } else if (c == '>') {
        emit(std::move(current_));
        current_ = {};
        state_ = TokenizerState::Data;
    } else if (c == kEOF) {
        current_.forceQuirks = true;
        emit(std::move(current_));
        emitEOF();
    } else {
        // parse error — but don't set forceQuirks
        state_ = TokenizerState::BogusDOCTYPE;
    }
}

void HTML5Tokenizer::processBogusDOCTYPE(char32_t c) {
    if (c == '>') {
        emit(std::move(current_));
        current_ = {};
        state_ = TokenizerState::Data;
    } else if (c == kEOF) {
        emit(std::move(current_));
        emitEOF();
    }
    // any other char: ignore (bogus doctype)
}

// ============================================================================
// CDATA Section States
// ============================================================================

void HTML5Tokenizer::processCDATASection(char32_t c) {
    if (c == ']') {
        state_ = TokenizerState::CDATASectionBracket;
    } else if (c == kEOF) {
        // parse error
        emitEOF();
    } else {
        emitCharacter(c);
    }
}

void HTML5Tokenizer::processCDATASectionBracket(char32_t c) {
    if (c == ']') {
        state_ = TokenizerState::CDATASectionEnd;
    } else {
        emitCharacter(']');
        state_ = TokenizerState::CDATASection;
        reconsume(c);
    }
}

void HTML5Tokenizer::processCDATASectionEnd(char32_t c) {
    if (c == ']') {
        emitCharacter(']');
    } else if (c == '>') {
        state_ = TokenizerState::Data;
    } else {
        emitCharacter(']');
        emitCharacter(']');
        state_ = TokenizerState::CDATASection;
        reconsume(c);
    }
}

// ============================================================================
// Character Reference States
// ============================================================================

void HTML5Tokenizer::processCharacterReference(char32_t c) {
    temporaryBuffer_ = "&";
    if (isASCIIAlphaNum(c)) {
        // Named character reference — start collecting
        state_ = TokenizerState::NamedCharacterReference;
        // We don't have a separate NamedCharRef state in the switch — handle inline
        // Collect the name until ';' or non-alphanumeric
        std::string name;
        name += utf8Encode(c);
        // Peek ahead collecting name chars
        while (!eof()) {
            char32_t nc = nextChar();
            if (nc == ';') {
                // Try to look up the name
                auto result = lookupNamedCharRef(name);
                if (result) {
                    for (char32_t cp : *result) {
                        if (returnState_ == TokenizerState::AttributeValueDoubleQuoted ||
                            returnState_ == TokenizerState::AttributeValueSingleQuoted ||
                            returnState_ == TokenizerState::AttributeValueUnquoted) {
                            currentAttrValue_ += utf8Encode(cp);
                        } else {
                            emitCharacter(cp);
                        }
                    }
                } else {
                    // Emit as-is
                    emitString("&" + name + ";");
                }
                state_ = returnState_;
                return;
            } else if (isASCIIAlphaNum(nc) || nc == '#') {
                name += utf8Encode(nc);
            } else {
                // No semicolon — check for attribute context
                // try matching without semicolon
                auto result = lookupNamedCharRef(name);
                if (result && (returnState_ == TokenizerState::Data ||
                               returnState_ == TokenizerState::RCDATA)) {
                    for (char32_t cp : *result) emitCharacter(cp);
                } else {
                    emitString("&" + name);
                }
                state_ = returnState_;
                reconsume(nc);
                return;
            }
        }
        state_ = returnState_;
        emitString("&" + name);
    } else if (c == '#') {
        temporaryBuffer_ += '#';
        charRefCode_ = 0;
        state_ = TokenizerState::NumericCharacterReference;
    } else {
        // Not a character reference — emit '&' and reconsume
        if (returnState_ == TokenizerState::AttributeValueDoubleQuoted ||
            returnState_ == TokenizerState::AttributeValueSingleQuoted ||
            returnState_ == TokenizerState::AttributeValueUnquoted) {
            currentAttrValue_ += '&';
        } else {
            emitCharacter('&');
        }
        state_ = returnState_;
        reconsume(c);
    }
}

void HTML5Tokenizer::processNumericCharacterReference(char32_t c) {
    charRefCode_ = 0;
    if (c == 'x' || c == 'X') {
        temporaryBuffer_ += utf8Encode(c);
        state_ = TokenizerState::HexadecimalCharacterReference;
    } else {
        state_ = TokenizerState::DecimalCharacterReference;
        reconsume(c);
    }
}

void HTML5Tokenizer::processHexadecimalCharacterReference(char32_t c) {
    if (isASCIIDigit(c)) {
        charRefCode_ = charRefCode_ * 16 + (c - '0');
    } else if (c >= 'A' && c <= 'F') {
        charRefCode_ = charRefCode_ * 16 + (c - 'A' + 10);
    } else if (c >= 'a' && c <= 'f') {
        charRefCode_ = charRefCode_ * 16 + (c - 'a' + 10);
    } else if (c == ';') {
        processNumericCharacterReferenceEnd();
        state_ = returnState_;
    } else {
        // parse error — missing semicolon
        processNumericCharacterReferenceEnd();
        state_ = returnState_;
        reconsume(c);
    }
}

void HTML5Tokenizer::processDecimalCharacterReference(char32_t c) {
    if (isASCIIDigit(c)) {
        charRefCode_ = charRefCode_ * 10 + (c - '0');
    } else if (c == ';') {
        processNumericCharacterReferenceEnd();
        state_ = returnState_;
    } else {
        processNumericCharacterReferenceEnd();
        state_ = returnState_;
        reconsume(c);
    }
}

void HTML5Tokenizer::processNumericCharacterReferenceEnd() {
    // Per WHATWG spec §13.2.5.80 — numeric character reference end state
    char32_t cp = charRefCode_;

    // Error checks and replacements per spec table
    if (cp == 0x00) cp = 0xFFFD;
    else if (cp > 0x10FFFF) cp = 0xFFFD;
    else if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD; // surrogate
    else {
        // Windows-1252 legacy mapping for 0x80–0x9F range
        static const char32_t legacyMap[32] = {
            0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
            0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
            0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
            0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178
        };
        if (cp >= 0x80 && cp <= 0x9F) {
            char32_t mapped = legacyMap[cp - 0x80];
            if (mapped) cp = mapped;
        }
    }

    std::string encoded = utf8Encode(cp);
    if (returnState_ == TokenizerState::AttributeValueDoubleQuoted ||
        returnState_ == TokenizerState::AttributeValueSingleQuoted ||
        returnState_ == TokenizerState::AttributeValueUnquoted) {
        currentAttrValue_ += encoded;
    } else {
        HTML5Token t;
        t.type = HTML5TokenType::Character;
        t.data = encoded;
        emit(std::move(t));
    }
}

// ============================================================================
// Raw-text end tag states (RCDATA / RAWTEXT / Script)
// ============================================================================

void HTML5Tokenizer::processRCDATALessThanSign(char32_t c) {
    if (c == '/') {
        temporaryBuffer_.clear();
        state_ = TokenizerState::RCDATAEndTagOpen;
    } else {
        emitCharacter('<');
        state_ = TokenizerState::RCDATA;
        reconsume(c);
    }
}

void HTML5Tokenizer::processRCDATAEndTagOpen(char32_t c) {
    if (isASCIIAlpha(c)) {
        current_ = {};
        current_.type = HTML5TokenType::EndTag;
        state_ = TokenizerState::RCDATAEndTagName;
        reconsume(c);
    } else {
        emitCharacter('<');
        emitCharacter('/');
        state_ = TokenizerState::RCDATA;
        reconsume(c);
    }
}

void HTML5Tokenizer::processRCDATAEndTagName(char32_t c) {
    if ((isWhitespace(c) || c == '/' || c == '>') && isAppropriateEndTagToken()) {
        if (isWhitespace(c)) state_ = TokenizerState::BeforeAttributeName;
        else if (c == '/') state_ = TokenizerState::SelfClosingStartTag;
        else { emitCurrentTag(); state_ = TokenizerState::Data; }
    } else if (isASCIIAlpha(c)) {
        current_.tagName += toLowerASCII(c);
        temporaryBuffer_ += utf8Encode(c);
    } else {
        emitCharacter('<');
        emitCharacter('/');
        emitString(temporaryBuffer_);
        state_ = TokenizerState::RCDATA;
        reconsume(c);
    }
}

void HTML5Tokenizer::processRAWTEXTLessThanSign(char32_t c) {
    if (c == '/') {
        temporaryBuffer_.clear();
        state_ = TokenizerState::RAWTEXTEndTagOpen;
    } else {
        emitCharacter('<');
        state_ = TokenizerState::RAWTEXT;
        reconsume(c);
    }
}

void HTML5Tokenizer::processRAWTEXTEndTagOpen(char32_t c) {
    if (isASCIIAlpha(c)) {
        current_ = {};
        current_.type = HTML5TokenType::EndTag;
        state_ = TokenizerState::RAWTEXTEndTagName;
        reconsume(c);
    } else {
        emitCharacter('<');
        emitCharacter('/');
        state_ = TokenizerState::RAWTEXT;
        reconsume(c);
    }
}

void HTML5Tokenizer::processRAWTEXTEndTagName(char32_t c) {
    if ((isWhitespace(c) || c == '/' || c == '>') && isAppropriateEndTagToken()) {
        if (isWhitespace(c)) state_ = TokenizerState::BeforeAttributeName;
        else if (c == '/') state_ = TokenizerState::SelfClosingStartTag;
        else { emitCurrentTag(); state_ = TokenizerState::Data; }
    } else if (isASCIIAlpha(c)) {
        current_.tagName += toLowerASCII(c);
        temporaryBuffer_ += utf8Encode(c);
    } else {
        emitCharacter('<');
        emitCharacter('/');
        emitString(temporaryBuffer_);
        state_ = TokenizerState::RAWTEXT;
        reconsume(c);
    }
}

void HTML5Tokenizer::processScriptDataLessThanSign(char32_t c) {
    if (c == '/') {
        temporaryBuffer_.clear();
        state_ = TokenizerState::ScriptDataEndTagOpen;
    } else if (c == '!') {
        emitCharacter('<');
        emitCharacter('!');
        state_ = TokenizerState::ScriptDataEscapeStart;
    } else {
        emitCharacter('<');
        state_ = TokenizerState::ScriptData;
        reconsume(c);
    }
}

void HTML5Tokenizer::processScriptDataEndTagOpen(char32_t c) {
    if (isASCIIAlpha(c)) {
        current_ = {};
        current_.type = HTML5TokenType::EndTag;
        state_ = TokenizerState::ScriptDataEndTagName;
        reconsume(c);
    } else {
        emitCharacter('<');
        emitCharacter('/');
        state_ = TokenizerState::ScriptData;
        reconsume(c);
    }
}

void HTML5Tokenizer::processScriptDataEndTagName(char32_t c) {
    if ((isWhitespace(c) || c == '/' || c == '>') && isAppropriateEndTagToken()) {
        if (isWhitespace(c)) state_ = TokenizerState::BeforeAttributeName;
        else if (c == '/') state_ = TokenizerState::SelfClosingStartTag;
        else { emitCurrentTag(); state_ = TokenizerState::Data; }
    } else if (isASCIIAlpha(c)) {
        current_.tagName += toLowerASCII(c);
        temporaryBuffer_ += utf8Encode(c);
    } else {
        emitCharacter('<');
        emitCharacter('/');
        emitString(temporaryBuffer_);
        state_ = TokenizerState::ScriptData;
        reconsume(c);
    }
}

void HTML5Tokenizer::processScriptDataEscapeStart(char32_t c) {
    if (c == '-') {
        emitCharacter('-');
        state_ = TokenizerState::ScriptDataEscapeStartDash;
    } else {
        state_ = TokenizerState::ScriptData;
        reconsume(c);
    }
}

void HTML5Tokenizer::processScriptDataEscapeStartDash(char32_t c) {
    if (c == '-') {
        emitCharacter('-');
        state_ = TokenizerState::ScriptDataEscapedDashDash;
    } else {
        state_ = TokenizerState::ScriptData;
        reconsume(c);
    }
}

void HTML5Tokenizer::processScriptDataEscaped(char32_t c) {
    if (c == '-') {
        emitCharacter('-');
        state_ = TokenizerState::ScriptDataEscapedDash;
    } else if (c == '<') {
        state_ = TokenizerState::ScriptDataEscapedLessThanSign;
    } else if (c == '\0') {
        emitCharacter(0xFFFD);
    } else if (c == kEOF) {
        emitEOF();
    } else {
        emitCharacter(c);
    }
}

void HTML5Tokenizer::processScriptDataEscapedDash(char32_t c) {
    if (c == '-') {
        emitCharacter('-');
        state_ = TokenizerState::ScriptDataEscapedDashDash;
    } else if (c == '<') {
        state_ = TokenizerState::ScriptDataEscapedLessThanSign;
    } else if (c == '\0') {
        emitCharacter(0xFFFD);
        state_ = TokenizerState::ScriptDataEscaped;
    } else if (c == kEOF) {
        emitEOF();
    } else {
        emitCharacter(c);
        state_ = TokenizerState::ScriptDataEscaped;
    }
}

void HTML5Tokenizer::processScriptDataEscapedDashDash(char32_t c) {
    if (c == '-') {
        emitCharacter('-');
    } else if (c == '<') {
        state_ = TokenizerState::ScriptDataEscapedLessThanSign;
    } else if (c == '>') {
        emitCharacter('>');
        state_ = TokenizerState::ScriptData;
    } else if (c == '\0') {
        emitCharacter(0xFFFD);
        state_ = TokenizerState::ScriptDataEscaped;
    } else if (c == kEOF) {
        emitEOF();
    } else {
        emitCharacter(c);
        state_ = TokenizerState::ScriptDataEscaped;
    }
}

void HTML5Tokenizer::processScriptDataEscapedLessThanSign(char32_t c) {
    if (c == '/') {
        temporaryBuffer_.clear();
        state_ = TokenizerState::ScriptDataEscapedEndTagOpen;
    } else if (isASCIIAlpha(c)) {
        temporaryBuffer_.clear();
        emitCharacter('<');
        state_ = TokenizerState::ScriptDataDoubleEscapeStart;
        reconsume(c);
    } else {
        emitCharacter('<');
        state_ = TokenizerState::ScriptDataEscaped;
        reconsume(c);
    }
}

void HTML5Tokenizer::processScriptDataEscapedEndTagOpen(char32_t c) {
    if (isASCIIAlpha(c)) {
        current_ = {};
        current_.type = HTML5TokenType::EndTag;
        state_ = TokenizerState::ScriptDataEscapedEndTagName;
        reconsume(c);
    } else {
        emitCharacter('<');
        emitCharacter('/');
        state_ = TokenizerState::ScriptDataEscaped;
        reconsume(c);
    }
}

void HTML5Tokenizer::processScriptDataEscapedEndTagName(char32_t c) {
    if ((isWhitespace(c) || c == '/' || c == '>') && isAppropriateEndTagToken()) {
        if (isWhitespace(c)) state_ = TokenizerState::BeforeAttributeName;
        else if (c == '/') state_ = TokenizerState::SelfClosingStartTag;
        else { emitCurrentTag(); state_ = TokenizerState::Data; }
    } else if (isASCIIAlpha(c)) {
        current_.tagName += toLowerASCII(c);
        temporaryBuffer_ += utf8Encode(c);
    } else {
        emitCharacter('<');
        emitCharacter('/');
        emitString(temporaryBuffer_);
        state_ = TokenizerState::ScriptDataEscaped;
        reconsume(c);
    }
}

void HTML5Tokenizer::processScriptDataDoubleEscapeStart(char32_t c) {
    if (isWhitespace(c) || c == '/' || c == '>') {
        if (temporaryBuffer_ == "script")
            state_ = TokenizerState::ScriptDataDoubleEscaped;
        else
            state_ = TokenizerState::ScriptDataEscaped;
        emitCharacter(c);
    } else if (isASCIIAlpha(c)) {
        temporaryBuffer_ += toLowerASCII(c);
        emitCharacter(c);
    } else {
        state_ = TokenizerState::ScriptDataEscaped;
        reconsume(c);
    }
}

void HTML5Tokenizer::processScriptDataDoubleEscaped(char32_t c) {
    if (c == '-') {
        emitCharacter('-');
        state_ = TokenizerState::ScriptDataDoubleEscapedDash;
    } else if (c == '<') {
        emitCharacter('<');
        state_ = TokenizerState::ScriptDataDoubleEscapedLessThanSign;
    } else if (c == '\0') {
        emitCharacter(0xFFFD);
    } else if (c == kEOF) {
        emitEOF();
    } else {
        emitCharacter(c);
    }
}

void HTML5Tokenizer::processScriptDataDoubleEscapedDash(char32_t c) {
    if (c == '-') {
        emitCharacter('-');
        state_ = TokenizerState::ScriptDataDoubleEscapedDashDash;
    } else if (c == '<') {
        emitCharacter('<');
        state_ = TokenizerState::ScriptDataDoubleEscapedLessThanSign;
    } else if (c == '\0') {
        emitCharacter(0xFFFD);
        state_ = TokenizerState::ScriptDataDoubleEscaped;
    } else if (c == kEOF) {
        emitEOF();
    } else {
        emitCharacter(c);
        state_ = TokenizerState::ScriptDataDoubleEscaped;
    }
}

void HTML5Tokenizer::processScriptDataDoubleEscapedDashDash(char32_t c) {
    if (c == '-') {
        emitCharacter('-');
    } else if (c == '<') {
        emitCharacter('<');
        state_ = TokenizerState::ScriptDataDoubleEscapedLessThanSign;
    } else if (c == '>') {
        emitCharacter('>');
        state_ = TokenizerState::ScriptData;
    } else if (c == '\0') {
        emitCharacter(0xFFFD);
        state_ = TokenizerState::ScriptDataDoubleEscaped;
    } else if (c == kEOF) {
        emitEOF();
    } else {
        emitCharacter(c);
        state_ = TokenizerState::ScriptDataDoubleEscaped;
    }
}

void HTML5Tokenizer::processScriptDataDoubleEscapedLessThanSign(char32_t c) {
    if (c == '/') {
        temporaryBuffer_.clear();
        emitCharacter('/');
        state_ = TokenizerState::ScriptDataDoubleEscapeEnd;
    } else {
        state_ = TokenizerState::ScriptDataDoubleEscaped;
        reconsume(c);
    }
}

void HTML5Tokenizer::processScriptDataDoubleEscapeEnd(char32_t c) {
    if (isWhitespace(c) || c == '/' || c == '>') {
        if (temporaryBuffer_ == "script")
            state_ = TokenizerState::ScriptDataEscaped;
        else
            state_ = TokenizerState::ScriptDataDoubleEscaped;
        emitCharacter(c);
    } else if (isASCIIAlpha(c)) {
        temporaryBuffer_ += toLowerASCII(c);
        emitCharacter(c);
    } else {
        state_ = TokenizerState::ScriptDataDoubleEscaped;
        reconsume(c);
    }
}

void HTML5Tokenizer::processCommentLessThanSign(char32_t c) {
    if (c == '!') {
        current_.data += '<';
        current_.data += '!';
        state_ = TokenizerState::CommentLessThanSignBang;
    } else {
        state_ = TokenizerState::Comment;
        reconsume(c);
    }
}

void HTML5Tokenizer::processCommentLessThanSignBang(char32_t c) {
    if (c == '-') {
        state_ = TokenizerState::CommentLessThanSignBangDash;
    } else {
        state_ = TokenizerState::Comment;
        reconsume(c);
    }
}

void HTML5Tokenizer::processCommentLessThanSignBangDash(char32_t c) {
    if (c == '-') {
        state_ = TokenizerState::CommentLessThanSignBangDashDash;
    } else {
        state_ = TokenizerState::CommentEndDash;
        reconsume(c);
    }
}

void HTML5Tokenizer::processCommentLessThanSignBangDashDash(char32_t c) {
    if (c == '>' || c == kEOF) {
        state_ = TokenizerState::CommentEnd;
        reconsume(c);
    } else {
        state_ = TokenizerState::CommentEnd;
        reconsume(c);
    }
}

} // namespace Zepra::WebCore
