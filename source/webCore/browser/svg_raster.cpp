// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file svg_raster.cpp
 * @brief SVG to RGBA through nanosvg, with a rewrite pass for what it lacks
 */

#include "svg_raster.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>

#define NANOSVG_ALL_COLOR_KEYWORDS
#define NANOSVG_IMPLEMENTATION
#include "../../ThirdParty/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "../../ThirdParty/nanosvg/nanosvgrast.h"

namespace ZepraBrowser {
namespace SvgRaster {

namespace {

// ---------------------------------------------------------------------------
// A small XML tree (enough for SVG documents; entities are kept as written)
// ---------------------------------------------------------------------------

struct Node {
    std::string name;   // empty for text
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<Node> children;
    std::string text;

    const std::string* attr(const char* key) const {
        for (const auto& a : attrs) if (a.first == key) return &a.second;
        return nullptr;
    }
    void setAttr(const std::string& key, const std::string& value) {
        for (auto& a : attrs) if (a.first == key) { a.second = value; return; }
        attrs.emplace_back(key, value);
    }
    void removeAttr(const char* key) {
        attrs.erase(std::remove_if(attrs.begin(), attrs.end(),
                                   [&](const std::pair<std::string, std::string>& a) { return a.first == key; }),
                    attrs.end());
    }
};

bool isNameChar(char c) {
    return std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ':' || c == '.';
}

void parseXml(const std::string& s, Node& doc) {
    std::vector<Node*> stack{&doc};
    size_t i = 0, n = s.size();
    auto addText = [&](const std::string& t) {
        if (t.find_first_not_of(" \t\r\n") == std::string::npos) return;
        Node text;
        text.text = t;
        stack.back()->children.push_back(std::move(text));
    };
    while (i < n) {
        if (s[i] != '<') {
            size_t lt = s.find('<', i);
            if (lt == std::string::npos) lt = n;
            addText(s.substr(i, lt - i));
            i = lt;
            continue;
        }
        if (s.compare(i, 4, "<!--") == 0) {
            size_t e = s.find("-->", i + 4);
            i = e == std::string::npos ? n : e + 3;
        } else if (s.compare(i, 9, "<![CDATA[") == 0) {
            size_t e = s.find("]]>", i + 9);
            if (e == std::string::npos) e = n;
            addText(s.substr(i + 9, e - i - 9));
            i = e == n ? n : e + 3;
        } else if (s.compare(i, 2, "<?") == 0) {
            size_t e = s.find("?>", i + 2);
            i = e == std::string::npos ? n : e + 2;
        } else if (s.compare(i, 2, "<!") == 0) {
            // <!DOCTYPE ...> with an optional [internal subset]
            int depth = 0;
            for (i += 2; i < n; i++) {
                if (s[i] == '[') depth++;
                else if (s[i] == ']') depth--;
                else if (s[i] == '>' && depth <= 0) { i++; break; }
            }
        } else if (s.compare(i, 2, "</") == 0) {
            size_t e = s.find('>', i);
            std::string name = s.substr(i + 2, (e == std::string::npos ? n : e) - i - 2);
            while (!name.empty() && std::isspace((unsigned char)name.back())) name.pop_back();
            for (size_t k = stack.size(); k-- > 1;) {
                if (stack[k]->name == name) { stack.resize(k); break; }
            }
            i = e == std::string::npos ? n : e + 1;
        } else {
            size_t j = i + 1;
            while (j < n && isNameChar(s[j])) j++;
            Node el;
            el.name = s.substr(i + 1, j - i - 1);
            bool selfClosing = false;
            while (j < n) {
                while (j < n && std::isspace((unsigned char)s[j])) j++;
                if (j >= n) break;
                if (s[j] == '>') { j++; break; }
                if (s[j] == '/') { selfClosing = true; j++; continue; }
                size_t k = j;
                while (k < n && isNameChar(s[k])) k++;
                if (k == j) { j++; continue; }
                std::string key = s.substr(j, k - j);
                j = k;
                while (j < n && std::isspace((unsigned char)s[j])) j++;
                std::string value;
                if (j < n && s[j] == '=') {
                    j++;
                    while (j < n && std::isspace((unsigned char)s[j])) j++;
                    if (j < n && (s[j] == '"' || s[j] == '\'')) {
                        char q = s[j++];
                        size_t e = s.find(q, j);
                        if (e == std::string::npos) e = n;
                        value = s.substr(j, e - j);
                        j = e == n ? n : e + 1;
                    } else {
                        size_t e = j;
                        while (e < n && !std::isspace((unsigned char)s[e]) && s[e] != '>') e++;
                        value = s.substr(j, e - j);
                        j = e;
                    }
                }
                el.attrs.emplace_back(std::move(key), std::move(value));
            }
            i = j;
            if (el.name.empty()) continue;
            stack.back()->children.push_back(std::move(el));
            if (!selfClosing) stack.push_back(&stack.back()->children.back());
        }
    }
}

void serialize(const Node& node, std::string& out) {
    if (node.name.empty()) {
        out += node.text;
        return;
    }
    out += '<';
    out += node.name;
    // style last: nanosvg applies attributes in order, and CSS beats presentation attributes
    for (int pass = 0; pass < 2; pass++) {
        for (const auto& a : node.attrs) {
            if ((a.first == "style") != (pass == 1)) continue;
            char q = a.second.find('"') == std::string::npos ? '"' : '\'';
            out += ' ';
            out += a.first;
            out += '=';
            out += q;
            out += a.second;
            out += q;
        }
    }
    if (node.children.empty()) {
        out += "/>";
        return;
    }
    out += '>';
    for (const auto& c : node.children) serialize(c, out);
    out += "</";
    out += node.name;
    out += '>';
}

Node* findSvg(Node& node) {
    if (node.name == "svg") return &node;
    for (auto& c : node.children) {
        if (Node* found = findSvg(c)) return found;
    }
    return nullptr;
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ---------------------------------------------------------------------------
// Rewrite passes
// ---------------------------------------------------------------------------

// SVG names with capitals; the HTML parser hands them over lowercased
const std::unordered_map<std::string, std::string>& mixedCaseNames() {
    static const std::unordered_map<std::string, std::string> names = [] {
        static const char* const list[] = {
            // elements
            "linearGradient", "radialGradient", "clipPath", "foreignObject", "textPath",
            "animateTransform", "animateMotion", "feGaussianBlur", "feOffset", "feBlend",
            "feColorMatrix", "feComposite", "feFlood", "feMerge", "feMergeNode", "feDropShadow",
            // attributes
            "viewBox", "preserveAspectRatio", "gradientUnits", "gradientTransform", "patternUnits",
            "patternContentUnits", "patternTransform", "clipPathUnits", "maskUnits", "maskContentUnits",
            "markerWidth", "markerHeight", "markerUnits", "refX", "refY", "stdDeviation", "textLength",
            "lengthAdjust", "startOffset", "spreadMethod", "pathLength", "baseFrequency", "numOctaves"};
        std::unordered_map<std::string, std::string> m;
        for (const char* name : list) m.emplace(lower(name), name);
        return m;
    }();
    return names;
}

void fixCase(Node& node) {
    const auto& names = mixedCaseNames();
    if (!node.name.empty()) {
        auto it = names.find(node.name);
        if (it != names.end()) node.name = it->second;
        for (auto& a : node.attrs) {
            auto at = names.find(a.first);
            if (at != names.end()) a.first = at->second;
        }
    }
    for (auto& c : node.children) fixCase(c);
}

struct CssRule {
    std::string tag, id;              // empty = any
    std::vector<std::string> classes;
    int specificity = 0;
    std::string declarations;
};

void collectStyleText(const Node& node, std::string& css) {
    if (node.name == "style") {
        for (const auto& c : node.children) if (c.name.empty()) css += c.text + "\n";
        return;
    }
    for (const auto& c : node.children) collectStyleText(c, css);
}

// Compound selectors only (tag, .class, #id); anything with combinators,
// pseudo-classes or attribute selectors is skipped.
std::vector<CssRule> parseCss(std::string css) {
    for (size_t a = css.find("/*"); a != std::string::npos; a = css.find("/*", a)) {
        size_t b = css.find("*/", a + 2);
        css.erase(a, b == std::string::npos ? std::string::npos : b + 2 - a);
    }
    std::vector<CssRule> rules;
    size_t pos = 0;
    while (pos < css.size()) {
        size_t open = css.find('{', pos);
        if (open == std::string::npos) break;
        size_t close = css.find('}', open);
        if (close == std::string::npos) close = css.size();
        std::string selectors = css.substr(pos, open - pos);
        std::string decls = trim(css.substr(open + 1, close - open - 1));
        pos = close + 1;
        if (selectors.find('@') != std::string::npos) continue;
        size_t start = 0;
        while (start <= selectors.size()) {
            size_t comma = selectors.find(',', start);
            if (comma == std::string::npos) comma = selectors.size();
            std::string sel = trim(selectors.substr(start, comma - start));
            start = comma + 1;
            if (sel.empty() || sel.find_first_of(" >+~:[*") != std::string::npos) continue;
            CssRule rule;
            size_t i = 0;
            bool ok = true;
            while (i < sel.size() && ok) {
                char kind = sel[i];
                if (kind == '.' || kind == '#') i++;
                size_t j = i;
                while (j < sel.size() && sel[j] != '.' && sel[j] != '#') j++;
                std::string word = sel.substr(i, j - i);
                if (word.empty()) ok = false;
                else if (kind == '.') { rule.classes.push_back(word); rule.specificity += 10; }
                else if (kind == '#') { rule.id = word; rule.specificity += 100; }
                else { rule.tag = word; rule.specificity += 1; }
                i = j;
            }
            if (!ok) continue;
            rule.declarations = decls;
            rules.push_back(std::move(rule));
        }
    }
    std::stable_sort(rules.begin(), rules.end(),
                     [](const CssRule& a, const CssRule& b) { return a.specificity < b.specificity; });
    return rules;
}

bool matches(const CssRule& rule, const Node& node) {
    if (!rule.tag.empty() && rule.tag != node.name) return false;
    if (!rule.id.empty()) {
        const std::string* id = node.attr("id");
        if (!id || *id != rule.id) return false;
    }
    if (!rule.classes.empty()) {
        const std::string* cls = node.attr("class");
        if (!cls) return false;
        std::string padded = " " + *cls + " ";
        for (char& c : padded) if (std::isspace((unsigned char)c)) c = ' ';
        for (const auto& c : rule.classes) {
            if (padded.find(" " + c + " ") == std::string::npos) return false;
        }
    }
    return true;
}

void applyCss(Node& node, const std::vector<CssRule>& rules) {
    if (!node.name.empty()) {
        std::string merged;
        for (const auto& rule : rules) {
            if (!matches(rule, node)) continue;
            merged += rule.declarations;
            if (!merged.empty() && merged.back() != ';') merged += ';';
        }
        if (!merged.empty()) {
            const std::string* inlineStyle = node.attr("style");
            node.setAttr("style", merged + (inlineStyle ? *inlineStyle : std::string()));
        }
    }
    for (auto& c : node.children) applyCss(c, rules);
}

void indexIds(Node& node, std::unordered_map<std::string, const Node*>& ids) {
    if (const std::string* id = node.attr("id")) ids.emplace(*id, &node);
    for (auto& c : node.children) indexIds(c, ids);
}

float number(const std::string* value, float fallback) {
    if (!value || value->empty()) return fallback;
    char* end = nullptr;
    float v = std::strtof(value->c_str(), &end);
    return end == value->c_str() ? fallback : v;
}

bool parseViewBox(const std::string* value, float vb[4]) {
    if (!value) return false;
    std::string s = *value;
    for (char& c : s) if (c == ',') c = ' ';
    return std::sscanf(s.c_str(), "%f %f %f %f", &vb[0], &vb[1], &vb[2], &vb[3]) == 4 &&
           vb[2] > 0 && vb[3] > 0;
}

std::string fmt(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

// <use> -> <g transform="... translate(x y)"> + copy of the referenced element
void expandUses(Node& node, const std::unordered_map<std::string, const Node*>& ids, int depth) {
    for (auto& child : node.children) {
        if (child.name != "use") {
            expandUses(child, ids, depth);
            continue;
        }
        const std::string* href = child.attr("href");
        if (!href) href = child.attr("xlink:href");
        const Node* target = nullptr;
        if (href && href->size() > 1 && (*href)[0] == '#') {
            auto it = ids.find(href->substr(1));
            if (it != ids.end()) target = it->second;
        }
        Node g;
        g.name = "g";
        float x = number(child.attr("x"), 0), y = number(child.attr("y"), 0);
        std::string transform;
        if (const std::string* t = child.attr("transform")) transform = *t + " ";
        if (x != 0 || y != 0) transform += "translate(" + fmt(x) + " " + fmt(y) + ")";
        for (const auto& a : child.attrs) {
            if (a.first == "x" || a.first == "y" || a.first == "width" || a.first == "height" ||
                a.first == "href" || a.first == "xlink:href" || a.first == "transform" || a.first == "id")
                continue;
            g.attrs.push_back(a);
        }
        if (target && depth < 8 && target->name != "use") {
            Node copy = *target;
            copy.removeAttr("id");
            float vb[4];
            if ((copy.name == "symbol" || copy.name == "svg") && parseViewBox(copy.attr("viewBox"), vb)) {
                float w = number(child.attr("width"), vb[2]), h = number(child.attr("height"), vb[3]);
                float s = std::min(w / vb[2], h / vb[3]);
                float tx = (w - vb[2] * s) / 2 - vb[0] * s, ty = (h - vb[3] * s) / 2 - vb[1] * s;
                transform += " translate(" + fmt(tx) + " " + fmt(ty) + ") scale(" + fmt(s) + ")";
            }
            if (copy.name == "symbol" || copy.name == "svg") {
                copy.name = "g";
                copy.removeAttr("viewBox");
                copy.removeAttr("preserveAspectRatio");
                copy.removeAttr("width");
                copy.removeAttr("height");
                copy.removeAttr("x");
                copy.removeAttr("y");
            }
            expandUses(copy, ids, depth + 1);
            g.children.push_back(std::move(copy));
        }
        if (!transform.empty()) g.setAttr("transform", trim(transform));
        child = std::move(g);
    }
}

// The HTML parser ignores "/>" on SVG elements, so in an inline <svg> the
// siblings of a <circle/> or <use/> end up as its children. Shapes have no
// painted children, so those are moved back out to follow it.
void unnestShapes(Node& node) {
    static const char* const leaves[] = {"path", "rect", "circle", "ellipse", "line", "polyline",
                                         "polygon", "use", "stop", "image"};
    for (size_t i = 0; i < node.children.size(); i++) {
        unnestShapes(node.children[i]);
        Node& c = node.children[i];
        bool leaf = false;
        for (const char* name : leaves) if (c.name == name) leaf = true;
        if (!leaf || c.children.empty()) continue;
        std::vector<Node> moved;
        for (auto& g : c.children) if (!g.name.empty()) moved.push_back(std::move(g));
        c.children.clear();
        node.children.insert(node.children.begin() + (long)i + 1,
                             std::make_move_iterator(moved.begin()), std::make_move_iterator(moved.end()));
    }
}

void dropUnpainted(Node& node) {
    static const char* const unpainted[] = {"symbol", "clipPath", "mask", "pattern", "marker", "filter",
                                            "foreignObject", "style", "script", "title", "desc", "metadata"};
    node.children.erase(
        std::remove_if(node.children.begin(), node.children.end(), [](const Node& c) {
            for (const char* name : unpainted) if (c.name == name) return true;
            return false;
        }),
        node.children.end());
    for (auto& c : node.children) dropUnpainted(c);
}

bool isWhite(std::string v) {
    v = lower(trim(v));
    return v == "white" || v == "#fff" || v == "#ffffff" || v == "rgb(255,255,255)" ||
           v == "rgb(255, 255, 255)";
}

void whiteToCurrentColor(Node& node) {
    for (auto& a : node.attrs) {
        if ((a.first == "fill" || a.first == "stroke" || a.first == "stop-color") && isWhite(a.second)) {
            a.second = "currentColor";
        } else if (a.first == "style") {
            std::string out;
            size_t start = 0;
            while (start < a.second.size()) {
                size_t semi = a.second.find(';', start);
                if (semi == std::string::npos) semi = a.second.size();
                std::string decl = a.second.substr(start, semi - start);
                size_t colon = decl.find(':');
                if (colon != std::string::npos) {
                    std::string prop = trim(decl.substr(0, colon));
                    if ((prop == "fill" || prop == "stroke" || prop == "stop-color") &&
                        isWhite(decl.substr(colon + 1)))
                        decl = prop + ":currentColor";
                }
                out += decl;
                if (semi < a.second.size()) out += ';';
                start = semi + 1;
            }
            a.second = out;
        }
    }
    for (auto& c : node.children) whiteToCurrentColor(c);
}

// A length in px; percentages and unknown units give 'fallback'
float lengthPx(const std::string* value, float fallback) {
    if (!value || value->empty()) return fallback;
    char* end = nullptr;
    float v = std::strtof(value->c_str(), &end);
    if (end == value->c_str() || v <= 0) return fallback;
    std::string unit = trim(end);
    if (unit.empty() || unit == "px") return v;
    if (unit == "pt") return v * 96.0f / 72.0f;
    if (unit == "pc") return v * 16.0f;
    if (unit == "in") return v * 96.0f;
    if (unit == "cm") return v * 96.0f / 2.54f;
    if (unit == "mm") return v * 96.0f / 25.4f;
    if (unit == "em") return v * 16.0f;
    return fallback;
}

void sizeOf(const Node& svg, float& width, float& height) {
    float vb[4];
    bool hasViewBox = parseViewBox(svg.attr("viewBox"), vb);
    width = lengthPx(svg.attr("width"), -1);
    height = lengthPx(svg.attr("height"), -1);
    if (width > 0 && height > 0) return;
    if (hasViewBox) {
        if (width > 0) { height = width * vb[3] / vb[2]; return; }
        if (height > 0) { width = height * vb[2] / vb[3]; return; }
        // A ratio but no size: the default object size, 300x150, contain-fitted
        // (CSS Images 3 §4.3; what <img> does in other browsers)
        float s = std::min(300 / vb[2], 150 / vb[3]);
        width = vb[2] * s;
        height = vb[3] * s;
        return;

    }
    if (width <= 0) width = 300;
    if (height <= 0) height = 150;
}

bool buildTree(const std::string& svg, Node& doc, Node*& root) {
    parseXml(svg, doc);
    fixCase(doc);
    root = findSvg(doc);
    if (root) unnestShapes(*root);
    return root != nullptr;
}

struct RasterizerDeleter {
    void operator()(NSVGrasterizer* r) const { nsvgDeleteRasterizer(r); }
};

} // namespace

std::string prepare(const std::string& svg, const Options& options) {
    Node doc;
    Node* root = nullptr;
    if (!buildTree(svg, doc, root)) return "";

    std::string css;
    collectStyleText(*root, css);
    if (!css.empty()) applyCss(*root, parseCss(css));

    std::unordered_map<std::string, const Node*> ids;
    indexIds(*root, ids);
    Node expanded = *root;  // copies resolve against the unexpanded tree
    expandUses(expanded, ids, 0);
    dropUnpainted(expanded);
    if (options.whiteIsCurrentColor) whiteToCurrentColor(expanded);

    // Explicit px size, so nanosvg maps the viewBox the same way intrinsicSize() sizes it
    float w, h;
    sizeOf(expanded, w, h);
    expanded.setAttr("width", fmt(w));
    expanded.setAttr("height", fmt(h));

    std::string out;
    serialize(expanded, out);
    return out;
}

bool intrinsicSize(const std::string& svg, float& width, float& height) {
    Node doc;
    Node* root = nullptr;
    if (!buildTree(svg, doc, root)) return false;
    sizeOf(*root, width, height);
    return true;
}

bool rasterize(const std::string& svg, int width, int height, const Options& options,
               std::vector<uint8_t>& rgba) {
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return false;
    std::string text = prepare(svg, options);
    if (text.empty()) return false;

    NSVGimage* image = nsvgParseWithColor(&text[0], "px", 96.0f, options.currentColor);
    if (!image) return false;
    rgba.assign((size_t)width * height * 4, 0);
    if (image->width > 0 && image->height > 0) {
        float s = std::min(width / image->width, height / image->height);
        float tx = (width - image->width * s) / 2, ty = (height - image->height * s) / 2;
        thread_local std::unique_ptr<NSVGrasterizer, RasterizerDeleter> rasterizer(nsvgCreateRasterizer());
        if (rasterizer) nsvgRasterize(rasterizer.get(), image, tx, ty, s, rgba.data(), width, height, width * 4);
    }
    nsvgDelete(image);
    return true;
}

} // namespace SvgRaster
} // namespace ZepraBrowser
