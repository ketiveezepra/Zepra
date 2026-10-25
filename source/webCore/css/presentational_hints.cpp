// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * presentational_hints.cpp - Legacy HTML attributes as style
 *
 * Follows the attribute-to-CSS mapping of the HTML Standard's Rendering
 * section (tables, legacy colours, <font>, alignment). Each hint is written
 * as a plain CSS declaration so the normal property parser applies it.
 */

#include "css/presentational_hints.hpp"
#include "css/css_computed_style.hpp"
#include "browser/dom.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace Zepra::WebCore {

namespace {

bool isHtmlSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isHtmlSpace(s[b])) b++;
    while (e > b && isHtmlSpace(s[e - 1])) e--;
    return s.substr(b, e - b);
}

std::string formatNumber(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

bool readAttr(const DOMElement* el, const char* name, std::string& out) {
    if (!el->hasAttribute(name)) return false;
    out = el->getAttribute(name);
    return true;
}

// "Rules for parsing non-negative integers"
bool parseNonNegativeInt(const std::string& value, long& out) {
    size_t i = 0;
    while (i < value.size() && isHtmlSpace(value[i])) i++;
    if (i < value.size() && value[i] == '+') i++;
    if (i >= value.size() || !std::isdigit((unsigned char)value[i])) return false;
    long n = 0;
    while (i < value.size() && std::isdigit((unsigned char)value[i])) {
        n = n * 10 + (value[i] - '0');
        if (n > 1000000) n = 1000000;
        i++;
    }
    out = n;
    return true;
}

// "Rules for parsing dimension values": "Npx" or "N%", empty on failure
// (and for zero when nonZero is set)
std::string parseDimension(const std::string& value, bool nonZero) {
    size_t i = 0;
    while (i < value.size() && isHtmlSpace(value[i])) i++;
    if (i < value.size() && value[i] == '+') i++;
    if (i >= value.size() || !std::isdigit((unsigned char)value[i])) return "";
    double n = 0;
    while (i < value.size() && std::isdigit((unsigned char)value[i])) {
        n = n * 10 + (value[i] - '0');
        i++;
    }
    if (i < value.size() && value[i] == '.') {
        i++;
        double scale = 0.1;
        while (i < value.size() && std::isdigit((unsigned char)value[i])) {
            n += (value[i] - '0') * scale;
            scale /= 10;
            i++;
        }
    }
    if (nonZero && n == 0) return "";
    bool percent = i < value.size() && value[i] == '%';
    return formatNumber(n) + (percent ? "%" : "px");
}

const DOMElement* parentElement(const DOMElement* el) {
    return dynamic_cast<const DOMElement*>(el->parentNode());
}

const DOMElement* enclosingTable(const DOMElement* el) {
    for (const DOMElement* p = parentElement(el); p; p = parentElement(p)) {
        if (toLower(p->tagName()) == "table") return p;
    }
    return nullptr;
}

// <table border>: absent → -1, present but unparsable or empty → 1
long tableBorderWidth(const DOMElement* table) {
    std::string v;
    if (!table || !readAttr(table, "border", v)) return -1;
    long n = 0;
    return parseNonNegativeInt(v, n) ? n : 1;
}

class HintWriter {
public:
    void add(const std::string& property, const std::string& value) {
        if (value.empty()) return;
        css_ += property;
        css_ += ": ";
        css_ += value;
        css_ += "; ";
    }
    void color(const std::string& property, const std::string& attrValue) {
        add(property, parseLegacyColor(attrValue));
    }
    const std::string& str() const { return css_; }

private:
    std::string css_;
};

// align on div, td, th, tr and row groups also aligns block descendants;
// -webkit-* values carry that (see the text-align parser)
void addCellOrDivAlign(HintWriter& w, const std::string& value) {
    std::string v = toLower(trim(value));
    if (v == "center" || v == "middle") w.add("text-align", "-webkit-center");
    else if (v == "left") w.add("text-align", "-webkit-left");
    else if (v == "right") w.add("text-align", "-webkit-right");
    else if (v == "justify") w.add("text-align", "justify");
}

void addTextAlign(HintWriter& w, const std::string& value) {
    std::string v = toLower(trim(value));
    if (v == "left" || v == "right" || v == "center" || v == "justify") w.add("text-align", v);
}

void addVAlign(HintWriter& w, const std::string& value) {
    std::string v = toLower(trim(value));
    if (v == "top" || v == "middle" || v == "bottom" || v == "baseline") w.add("vertical-align", v);
}

void addDimension(HintWriter& w, const DOMElement* el, const char* attr,
                  const char* property, bool nonZero) {
    std::string v;
    if (readAttr(el, attr, v)) w.add(property, parseDimension(v, nonZero));
}

void addBorderWidth(HintWriter& w, long px, const char* style) {
    std::string len = std::to_string(px) + "px";
    w.add("border-top-width", len);
    w.add("border-right-width", len);
    w.add("border-bottom-width", len);
    w.add("border-left-width", len);
    w.add("border-style", style);
}

// Legacy <font size> → px, using the CSS absolute-size keyword table
const char* kLegacyFontSizes[] = {"", "10px", "13px", "16px", "18px", "24px", "32px", "48px"};

} // namespace

std::string parseLegacyColor(const std::string& input) {
    std::string value = trim(input);
    if (value.empty()) return "";
    std::string lower = toLower(value);
    if (lower == "transparent") return "";

    // CSS named colours
    bool alpha = std::all_of(lower.begin(), lower.end(),
                             [](unsigned char c) { return std::isalpha(c); });
    if (alpha) {
        CSSColor named = CSSColor::parse(lower);
        bool isBlack = named.r == 0 && named.g == 0 && named.b == 0;
        if (!isBlack || lower == "black") return lower;
    }

    auto isHex = [](char c) { return std::isxdigit((unsigned char)c) != 0; };
    if (value.size() == 4 && value[0] == '#' && isHex(value[1]) && isHex(value[2]) && isHex(value[3])) {
        std::string out = "#";
        for (int i = 1; i < 4; i++) { out += value[i]; out += value[i]; }
        return toLower(out);
    }

    // Non-ASCII bytes count as non-hex digits
    if (value.size() > 128) value.resize(128);
    if (value[0] == '#') value.erase(0, 1);
    for (char& c : value) {
        if (!isHex(c)) c = '0';
    }
    while (value.empty() || value.size() % 3 != 0) value += '0';

    size_t len = value.size() / 3;
    std::string parts[3] = {value.substr(0, len), value.substr(len, len), value.substr(2 * len, len)};
    if (len > 8) {
        for (auto& p : parts) p = p.substr(len - 8);
        len = 8;
    }
    while (len > 2 && parts[0][0] == '0' && parts[1][0] == '0' && parts[2][0] == '0') {
        for (auto& p : parts) p.erase(0, 1);
        len--;
    }
    if (len > 2) {
        for (auto& p : parts) p.resize(2);
    }
    std::string out = "#";
    for (auto& p : parts) {
        if (p.size() == 1) p = "0" + p;
        out += p;
    }
    return toLower(out);
}

int parseLegacyFontSize(const std::string& input) {
    size_t i = 0;
    while (i < input.size() && isHtmlSpace(input[i])) i++;
    int mode = 0;  // 0 absolute, 1 relative-plus, -1 relative-minus
    if (i < input.size() && input[i] == '+') { mode = 1; i++; }
    else if (i < input.size() && input[i] == '-') { mode = -1; i++; }
    if (i >= input.size() || !std::isdigit((unsigned char)input[i])) return 0;
    int n = 0;
    while (i < input.size() && std::isdigit((unsigned char)input[i])) {
        n = std::min(n * 10 + (input[i] - '0'), 100);
        i++;
    }
    if (mode == 1) n = 3 + n;
    else if (mode == -1) n = 3 - n;
    return std::clamp(n, 1, 7);
}

std::string presentationalHintStyle(const DOMElement* el) {
    if (!el) return "";
    std::string tag = toLower(el->tagName());
    HintWriter w;
    std::string v;

    if (tag == "body") {
        if (readAttr(el, "bgcolor", v)) w.color("background-color", v);
        if (readAttr(el, "text", v)) w.color("color", v);
        const char* sides[][3] = {{"marginheight", "topmargin", "margin-top"},
                                  {"marginheight", "bottommargin", "margin-bottom"},
                                  {"marginwidth", "leftmargin", "margin-left"},
                                  {"marginwidth", "rightmargin", "margin-right"}};
        for (auto& s : sides) {
            std::string m;
            if (readAttr(el, s[0], m) || readAttr(el, s[1], m)) w.add(s[2], parseDimension(m, false));
        }
    } else if (tag == "table") {
        if (readAttr(el, "bgcolor", v)) w.color("background-color", v);
        addDimension(w, el, "width", "width", true);
        addDimension(w, el, "height", "height", false);
        long n = 0;
        if (readAttr(el, "cellspacing", v) && parseNonNegativeInt(v, n))
            w.add("border-spacing", std::to_string(n) + "px");
        long border = tableBorderWidth(el);
        if (border > 0) {
            addBorderWidth(w, border, "outset");
            w.add("border-color", "gray");
        }
        if (readAttr(el, "align", v)) {
            std::string a = toLower(trim(v));
            if (a == "left" || a == "right") w.add("float", a);
            else if (a == "center") {
                w.add("margin-left", "auto");
                w.add("margin-right", "auto");
            }
        }
    } else if (tag == "td" || tag == "th") {
        if (readAttr(el, "bgcolor", v)) w.color("background-color", v);
        addDimension(w, el, "width", "width", true);
        addDimension(w, el, "height", "height", true);
        if (readAttr(el, "align", v)) addCellOrDivAlign(w, v);
        if (readAttr(el, "valign", v)) addVAlign(w, v);
        if (el->hasAttribute("nowrap")) w.add("white-space", "nowrap");
        const DOMElement* table = enclosingTable(el);
        long n = 0;
        if (table && readAttr(table, "cellpadding", v) && parseNonNegativeInt(v, n))
            w.add("padding", std::to_string(n) + "px");
        if (tableBorderWidth(table) > 0) {
            addBorderWidth(w, 1, "inset");
            w.add("border-color", "gray");
        }
    } else if (tag == "tr" || tag == "thead" || tag == "tbody" || tag == "tfoot") {
        if (readAttr(el, "bgcolor", v)) w.color("background-color", v);
        if (tag == "tr") addDimension(w, el, "height", "height", true);
        if (readAttr(el, "align", v)) addCellOrDivAlign(w, v);
        if (readAttr(el, "valign", v)) addVAlign(w, v);
    } else if (tag == "caption") {
        if (readAttr(el, "align", v)) addTextAlign(w, v);
    } else if (tag == "div") {
        if (readAttr(el, "align", v)) addCellOrDivAlign(w, v);
    } else if (tag == "p" || tag == "h1" || tag == "h2" || tag == "h3" ||
               tag == "h4" || tag == "h5" || tag == "h6") {
        if (readAttr(el, "align", v)) addTextAlign(w, v);
    } else if (tag == "font") {
        if (readAttr(el, "color", v)) w.color("color", v);
        if (readAttr(el, "face", v) && !trim(v).empty()) w.add("font-family", trim(v));
        if (readAttr(el, "size", v)) {
            int size = parseLegacyFontSize(v);
            if (size > 0) w.add("font-size", kLegacyFontSizes[size]);
        }
    } else if (tag == "img" || tag == "object" || tag == "embed" || tag == "iframe" || tag == "video") {
        addDimension(w, el, "width", "width", false);
        addDimension(w, el, "height", "height", false);
        if (readAttr(el, "hspace", v)) {
            std::string m = parseDimension(v, false);
            w.add("margin-left", m);
            w.add("margin-right", m);
        }
        if (readAttr(el, "vspace", v)) {
            std::string m = parseDimension(v, false);
            w.add("margin-top", m);
            w.add("margin-bottom", m);
        }
        long n = 0;
        if ((tag == "img" || tag == "object") && readAttr(el, "border", v) && parseNonNegativeInt(v, n)) {
            addBorderWidth(w, n, "solid");
        }
        if (readAttr(el, "align", v)) {
            std::string a = toLower(trim(v));
            if (a == "left" || a == "right") w.add("float", a);
            else if (a == "top" || a == "middle" || a == "bottom") w.add("vertical-align", a);
        }
    } else if (tag == "hr") {
        addDimension(w, el, "width", "width", true);
        long n = 0;
        if (readAttr(el, "size", v) && parseNonNegativeInt(v, n) && n > 0)
            w.add("height", std::to_string(n) + "px");
        if (readAttr(el, "color", v)) {
            std::string c = parseLegacyColor(v);
            w.add("background-color", c);
            w.add("border-color", c);
        }
        if (readAttr(el, "align", v)) {
            std::string a = toLower(trim(v));
            if (a == "left") w.add("margin-right", "auto");
            else if (a == "right") w.add("margin-left", "auto");
            else if (a == "center") {
                w.add("margin-left", "auto");
                w.add("margin-right", "auto");
            }
        }
    }
    return w.str();
}

} // namespace Zepra::WebCore
