/**
 * @file font_face_descriptors.cpp
 * @brief Parsing of the @font-face descriptors: font-family, src,
 *        font-weight, font-style, font-stretch, unicode-range, font-display.
 */

#include "css/font_face_descriptors.hpp"

#include "css/css_style_declaration.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace Zepra::WebCore {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// Split on `sep` outside quotes and parentheses
std::vector<std::string> splitTopLevel(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            cur += c;
            if (c == '\\' && i + 1 < s.size()) cur += s[++i];
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '(') ++depth;
        else if (c == ')' && depth > 0) --depth;
        else if (c == sep && depth == 0) {
            parts.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    parts.push_back(cur);
    return parts;
}

/// Whitespace-separated tokens
std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (isSpace(c)) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

/// A whole quoted string, with its escapes undone. False if s isn't one.
bool unquote(const std::string& s, std::string& out) {
    if (s.size() < 2 || (s.front() != '"' && s.front() != '\'') || s.back() != s.front()) return false;
    out.clear();
    for (size_t i = 1; i + 1 < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 2 < s.size()) {
            out += s[++i];
            continue;
        }
        if (c == s.front()) return false;  // a second string follows
        out += c;
    }
    return true;
}

bool isIdent(const std::string& w) {
    if (w.empty()) return false;
    size_t i = 0;
    if (w[0] == '-') {
        if (w.size() == 1) return false;
        i = 1;
    }
    unsigned char first = static_cast<unsigned char>(w[i]);
    if (std::isdigit(first)) return false;
    for (; i < w.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(w[i]);
        if (!(std::isalnum(c) || c == '-' || c == '_' || c >= 0x80)) return false;
    }
    return true;
}

/// A family or font name: one string, or a run of identifiers joined by spaces
bool parseName(const std::string& value, std::string& name, bool rejectGeneric) {
    std::string v = trim(value);
    if (v.empty()) return false;
    if (v.front() == '"' || v.front() == '\'') return unquote(v, name) && !name.empty();
    auto ws = words(v);
    for (const auto& w : ws)
        if (!isIdent(w)) return false;
    if (rejectGeneric && ws.size() == 1) {
        static const char* const kReserved[] = {
            "serif", "sans-serif", "monospace", "cursive", "fantasy", "system-ui", "math",
            "emoji", "fangsong", "ui-serif", "ui-sans-serif", "ui-monospace", "ui-rounded",
            "inherit", "initial", "unset", "revert", "revert-layer", "default",
        };
        std::string l = lower(ws[0]);
        for (const char* r : kReserved)
            if (l == r) return false;
    }
    name.clear();
    for (const auto& w : ws) {
        if (!name.empty()) name += ' ';
        name += w;
    }
    return true;
}

/// "name(args)" at the start of s: sets fn (lowercased) and args, and how
/// much of s it used
bool readFunction(const std::string& s, size_t& pos, std::string& fn, std::string& args) {
    while (pos < s.size() && isSpace(s[pos])) ++pos;
    size_t start = pos;
    while (pos < s.size() && (std::isalnum(static_cast<unsigned char>(s[pos])) || s[pos] == '-')) ++pos;
    if (pos == start || pos >= s.size() || s[pos] != '(') return false;
    fn = lower(s.substr(start, pos - start));
    ++pos;
    int depth = 1;
    char quote = 0;
    size_t argStart = pos;
    for (; pos < s.size(); ++pos) {
        char c = s[pos];
        if (quote) {
            if (c == '\\') ++pos;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '(') ++depth;
        else if (c == ')' && --depth == 0) break;
    }
    if (pos >= s.size()) return false;
    args = s.substr(argStart, pos - argStart);
    ++pos;
    return true;
}

bool parseHintList(const std::string& args, std::vector<std::string>& out) {
    for (const auto& part : splitTopLevel(args, ',')) {
        std::string p = trim(part), v;
        if (unquote(p, v)) out.push_back(lower(v));
        else if (isIdent(p)) out.push_back(lower(p));
        else return false;
    }
    return !out.empty();
}

bool parseSource(const std::string& entry, FontFaceSource& src) {
    size_t pos = 0;
    std::string fn, args;
    if (!readFunction(entry, pos, fn, args)) return false;
    if (fn == "local") {
        src.isLocal = true;
        if (!parseName(args, src.localName, false)) return false;
    } else if (fn == "url") {
        std::string a = trim(args);
        if (!unquote(a, src.url)) src.url = a;
        if (src.url.empty()) return false;
    } else {
        return false;
    }
    while (true) {
        while (pos < entry.size() && isSpace(entry[pos])) ++pos;
        if (pos >= entry.size()) break;
        if (src.isLocal) return false;  // local() takes no hints
        if (!readFunction(entry, pos, fn, args)) return false;
        if (fn == "format") {
            if (!src.formats.empty() || !parseHintList(args, src.formats)) return false;
        } else if (fn == "tech") {
            if (!src.techs.empty() || !parseHintList(args, src.techs)) return false;
        } else {
            return false;
        }
    }
    return true;
}

bool parseNumber(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size() && std::isfinite(out);
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/// Up to six hex digits; with allowWildcard, trailing '?'s widen to a range
bool parseHexRange(const std::string& s, bool allowWildcard, uint32_t& lo, uint32_t& hi) {
    if (s.empty() || s.size() > 6) return false;
    lo = hi = 0;
    bool wild = false;
    for (char c : s) {
        if (c == '?') {
            if (!allowWildcard) return false;
            wild = true;
            lo = lo << 4;
            hi = (hi << 4) | 0xF;
            continue;
        }
        int v = hexValue(c);
        if (v < 0 || wild) return false;  // digits may not follow a '?'
        lo = (lo << 4) | static_cast<uint32_t>(v);
        hi = (hi << 4) | static_cast<uint32_t>(v);
    }
    return true;
}

} // namespace

bool FontFaceDescriptors::coversCodePoint(uint32_t cp) const {
    if (unicodeRanges.empty()) return true;
    for (const auto& r : unicodeRanges)
        if (cp >= r.first && cp <= r.last) return true;
    return false;
}

bool parseFontFaceFamily(const std::string& value, std::string& family) {
    return parseName(value, family, true);
}

bool parseFontFaceSrc(const std::string& value, std::vector<FontFaceSource>& sources) {
    // A malformed entry is dropped; the others still count
    sources.clear();
    for (const auto& part : splitTopLevel(value, ',')) {
        FontFaceSource src;
        if (parseSource(trim(part), src)) sources.push_back(std::move(src));
    }
    return !sources.empty();
}

bool parseFontFaceWeight(const std::string& value, int& min, int& max) {
    auto ws = words(lower(value));
    if (ws.empty() || ws.size() > 2) return false;
    if (ws.size() == 1 && ws[0] == "auto") {
        min = max = 400;
        return true;
    }
    int v[2];
    for (size_t i = 0; i < ws.size(); ++i) {
        double d;
        if (ws[i] == "normal") v[i] = 400;
        else if (ws[i] == "bold") v[i] = 700;
        else if (parseNumber(ws[i], d) && d >= 1 && d <= 1000) v[i] = static_cast<int>(std::lround(d));
        else return false;
    }
    min = v[0];
    max = ws.size() == 2 ? v[1] : v[0];
    if (min > max) std::swap(min, max);
    return true;
}

bool parseFontFaceStyle(const std::string& value, FontFaceStyle& style) {
    auto ws = words(lower(value));
    if (ws.empty()) return false;
    if (ws.size() == 1 && (ws[0] == "normal" || ws[0] == "auto")) {
        style = FontFaceStyle::Normal;
        return true;
    }
    if (ws.size() == 1 && ws[0] == "italic") {
        style = FontFaceStyle::Italic;
        return true;
    }
    if (ws[0] != "oblique" || ws.size() > 3) return false;
    for (size_t i = 1; i < ws.size(); ++i) {
        const std::string& a = ws[i];
        if (a.size() < 4 || a.compare(a.size() - 3, 3, "deg") != 0) return false;
        double d;
        if (!parseNumber(a.substr(0, a.size() - 3), d) || d < -90 || d > 90) return false;
    }
    style = FontFaceStyle::Oblique;
    return true;
}

bool parseFontFaceStretch(const std::string& value, float& min, float& max) {
    auto ws = words(lower(value));
    if (ws.empty() || ws.size() > 2) return false;
    if (ws.size() == 1 && ws[0] == "auto") {
        min = max = 100.f;
        return true;
    }
    static const struct { const char* name; float pct; } kKeywords[] = {
        {"ultra-condensed", 50.f}, {"extra-condensed", 62.5f}, {"condensed", 75.f},
        {"semi-condensed", 87.5f}, {"normal", 100.f},          {"semi-expanded", 112.5f},
        {"expanded", 125.f},       {"extra-expanded", 150.f},  {"ultra-expanded", 200.f},
    };
    float v[2];
    for (size_t i = 0; i < ws.size(); ++i) {
        bool found = false;
        for (const auto& k : kKeywords)
            if (ws[i] == k.name) {
                v[i] = k.pct;
                found = true;
            }
        if (found) continue;
        double d;
        if (ws[i].size() < 2 || ws[i].back() != '%' || !parseNumber(ws[i].substr(0, ws[i].size() - 1), d) || d < 0)
            return false;
        v[i] = static_cast<float>(d);
    }
    min = v[0];
    max = ws.size() == 2 ? v[1] : v[0];
    if (min > max) std::swap(min, max);
    return true;
}

bool parseUnicodeRange(const std::string& value, std::vector<UnicodeRange>& ranges) {
    std::vector<UnicodeRange> out;
    for (const auto& part : splitTopLevel(value, ',')) {
        std::string p = trim(part);
        if (p.size() < 3 || (p[0] != 'u' && p[0] != 'U') || p[1] != '+') return false;
        p = p.substr(2);
        UnicodeRange r;
        size_t dash = p.find('-');
        if (dash == std::string::npos) {
            if (!parseHexRange(p, true, r.first, r.last)) return false;
        } else {
            uint32_t ignored;
            if (!parseHexRange(p.substr(0, dash), false, r.first, ignored)) return false;
            if (!parseHexRange(p.substr(dash + 1), false, r.last, ignored)) return false;
        }
        if (r.first > 0x10FFFF || r.first > r.last) return false;
        if (r.last > 0x10FFFF) r.last = 0x10FFFF;
        out.push_back(r);
    }
    ranges = std::move(out);
    return true;
}

bool parseFontFaceDescriptors(const CSSStyleDeclaration& decl, FontFaceDescriptors& out) {
    out = FontFaceDescriptors{};
    if (!parseFontFaceFamily(decl.getPropertyValue("font-family"), out.family)) return false;
    if (!parseFontFaceSrc(decl.getPropertyValue("src"), out.sources)) return false;

    std::string v = decl.getPropertyValue("font-weight");
    if (!v.empty() && !parseFontFaceWeight(v, out.weightMin, out.weightMax)) out.weightMin = out.weightMax = 400;
    v = decl.getPropertyValue("font-style");
    if (!v.empty() && !parseFontFaceStyle(v, out.style)) out.style = FontFaceStyle::Normal;
    v = decl.getPropertyValue("font-stretch");
    if (!v.empty() && !parseFontFaceStretch(v, out.stretchMin, out.stretchMax)) out.stretchMin = out.stretchMax = 100.f;
    v = decl.getPropertyValue("unicode-range");
    if (!v.empty() && !parseUnicodeRange(v, out.unicodeRanges)) out.unicodeRanges.clear();
    v = lower(trim(decl.getPropertyValue("font-display")));
    if (v == "block" || v == "swap" || v == "fallback" || v == "optional") out.display = v;
    return true;
}

} // namespace Zepra::WebCore
