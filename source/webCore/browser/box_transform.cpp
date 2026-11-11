// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * box_transform.cpp - CSS transforms of layout boxes (see box_transform.h)
 */

#include "box_transform.h"

#include "layout_engine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace ZepraBrowser {

namespace {

const float kPi = 3.14159265358979f;

// The viewport vw/vh resolve against (setTransformViewport)
float s_viewW = 1920, s_viewH = 1080;

std::string lowered(std::string s) {
    for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// The arguments of a function or the parts of a value, split at top-level
// commas (or white space when `spaces`); nested parentheses stay together
std::vector<std::string> splitTop(const std::string& s, bool spaces) {
    std::vector<std::string> parts;
    std::string current;
    int depth = 0;
    auto flush = [&]() {
        std::string t = trimmed(current);
        if (!t.empty()) parts.push_back(t);
        current.clear();
    };
    for (char ch : s) {
        if (ch == '(') depth++;
        else if (ch == ')') depth = std::max(0, depth - 1);
        if (depth == 0 && (ch == ',' || (spaces && std::isspace((unsigned char)ch)))) {
            flush();
            continue;
        }
        current += ch;
    }
    flush();
    return parts;
}

// A number with an optional unit at s[pos...]; advances pos
bool readNumber(const std::string& s, size_t& pos, float& value, std::string& unit) {
    const char* start = s.c_str() + pos;
    char* end = nullptr;
    double v = std::strtod(start, &end);
    if (end == start || !std::isfinite(v)) return false;
    pos += (size_t)(end - start);
    size_t u = pos;
    while (u < s.size() && (std::isalpha((unsigned char)s[u]) || s[u] == '%')) u++;
    unit = s.substr(pos, u - pos);
    pos = u;
    value = (float)v;
    return true;
}

// A calc() value: pixels, a percentage and a plain number (the factor of
// a product), so "calc(1 / 2 * 100%) * -1" works out
struct CalcValue {
    float px = 0, percent = 0, number = 0;
    bool isNumber = false;
};

struct CalcParser {
    const std::string& s;
    float fontSize;
    size_t pos = 0;
    bool ok = true;

    void skip() {
        while (pos < s.size() && std::isspace((unsigned char)s[pos])) pos++;
    }

    CalcValue primary() {
        skip();
        CalcValue v;
        if (pos >= s.size()) { ok = false; return v; }
        if (s[pos] == '(') {
            pos++;
            v = sum();
            skip();
            if (pos < s.size() && s[pos] == ')') pos++;
            else ok = false;
            return v;
        }
        if (s.compare(pos, 5, "calc(") == 0) {
            pos += 4;
            return primary();
        }
        float n = 0;
        std::string unit;
        if (!readNumber(s, pos, n, unit)) { ok = false; return v; }
        if (unit.empty()) {
            v.number = n;
            v.isNumber = true;
        } else if (unit == "%") {
            v.percent = n;
        } else {
            float viewW = s_viewW, viewH = s_viewH;
            if (unit == "px") v.px = n;
            else if (unit == "em") v.px = n * fontSize;
            else if (unit == "rem") v.px = n * 16.0f;
            else if (unit == "vw") v.px = n * viewW / 100.0f;
            else if (unit == "vh") v.px = n * viewH / 100.0f;
            else if (unit == "vmin") v.px = n * std::min(viewW, viewH) / 100.0f;
            else if (unit == "vmax") v.px = n * std::max(viewW, viewH) / 100.0f;
            else if (unit == "pt") v.px = n * 96.0f / 72.0f;
            else ok = false;
        }
        return v;
    }

    CalcValue product() {
        CalcValue left = primary();
        for (;;) {
            skip();
            if (pos >= s.size() || (s[pos] != '*' && s[pos] != '/')) return left;
            char op = s[pos++];
            CalcValue right = primary();
            if (op == '*') {
                // One side of a product is a plain number
                if (!left.isNumber && !right.isNumber) { ok = false; return left; }
                if (left.isNumber && right.isNumber) {
                    left.number *= right.number;
                } else {
                    float k = left.isNumber ? left.number : right.number;
                    const CalcValue& len = left.isNumber ? right : left;
                    left = {len.px * k, len.percent * k, 0, false};
                }
            } else {
                if (!right.isNumber || right.number == 0) { ok = false; return left; }
                left.px /= right.number;
                left.percent /= right.number;
                left.number /= right.number;
            }
        }
    }

    CalcValue sum() {
        CalcValue left = product();
        for (;;) {
            skip();
            if (pos >= s.size() || (s[pos] != '+' && s[pos] != '-')) return left;
            float sign = s[pos++] == '+' ? 1.0f : -1.0f;
            CalcValue right = product();
            if (left.isNumber != right.isNumber) { ok = false; return left; }
            left.px += sign * right.px;
            left.percent += sign * right.percent;
            left.number += sign * right.number;
        }
    }
};

bool parseAngle(const std::string& token, float& radians) {
    std::string t = lowered(trimmed(token));
    size_t pos = 0;
    float n = 0;
    std::string unit;
    if (!readNumber(t, pos, n, unit) || pos != t.size()) return false;
    if (unit == "deg") radians = n * kPi / 180.0f;
    else if (unit == "rad") radians = n;
    else if (unit == "grad") radians = n * kPi / 200.0f;
    else if (unit == "turn") radians = n * 2.0f * kPi;
    else if (unit.empty() && n == 0) radians = 0;
    else return false;
    return true;
}

// A scale factor: a number or a percentage (scale: 105%)
bool parseFactor(const std::string& token, float& factor) {
    std::string t = lowered(trimmed(token));
    size_t pos = 0;
    float n = 0;
    std::string unit;
    if (!readNumber(t, pos, n, unit) || pos != t.size()) return false;
    if (unit.empty()) factor = n;
    else if (unit == "%") factor = n / 100.0f;
    else return false;
    return true;
}

bool parseNumbers(const std::vector<std::string>& args, std::vector<float>& out) {
    for (const std::string& arg : args) {
        float v = 0;
        if (!parseFactor(arg, v)) return false;
        out.push_back(v);
    }
    return true;
}

}  // namespace

void setTransformViewport(float width, float height) {
    if (width > 0) s_viewW = width;
    if (height > 0) s_viewH = height;
}

Affine Affine::rotation(float radians) {
    float c = std::cos(radians), s = std::sin(radians);
    return {c, s, -s, c, 0, 0};
}

bool Affine::isIdentity() const {
    return a == 1 && b == 0 && c == 0 && d == 1 && e == 0 && f == 0;
}

bool Affine::isTranslation() const { return a == 1 && b == 0 && c == 0 && d == 1; }

Affine Affine::operator*(const Affine& m) const {
    return {a * m.a + c * m.b, b * m.a + d * m.b, a * m.c + c * m.d, b * m.c + d * m.d,
            a * m.e + c * m.f + e, b * m.e + d * m.f + f};
}

bool Affine::invert(Affine& out) const {
    float det = a * d - b * c;
    if (std::fabs(det) < 1e-9f) return false;
    out.a = d / det;
    out.b = -b / det;
    out.c = -c / det;
    out.d = a / det;
    out.e = (c * f - d * e) / det;
    out.f = (b * e - a * f) / det;
    return true;
}

void Affine::map(float& x, float& y) const {
    float nx = a * x + c * y + e, ny = b * x + d * y + f;
    x = nx;
    y = ny;
}

void Affine::mapRect(float& x, float& y, float& w, float& h) const {
    float xs[4] = {x, x + w, x, x + w}, ys[4] = {y, y, y + h, y + h};
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (int i = 0; i < 4; i++) {
        map(xs[i], ys[i]);
        if (i == 0 || xs[i] < x0) x0 = xs[i];
        if (i == 0 || xs[i] > x1) x1 = xs[i];
        if (i == 0 || ys[i] < y0) y0 = ys[i];
        if (i == 0 || ys[i] > y1) y1 = ys[i];
    }
    x = x0;
    y = y0;
    w = x1 - x0;
    h = y1 - y0;
}

bool parseTransformLength(const std::string& token, float fontSize, TransformLength& out) {
    std::string t = lowered(trimmed(token));
    if (t.empty()) return false;
    CalcParser parser{t, fontSize};
    CalcValue v = parser.sum();
    parser.skip();
    if (!parser.ok || parser.pos != t.size()) return false;
    // A bare number is a length only when it is 0
    if (v.isNumber && v.number != 0) return false;
    out.px = v.px;
    out.percent = v.percent;
    return true;
}

bool parseTransformList(const std::string& css, float width, float height, float fontSize, Affine& out) {
    out = Affine();
    std::string text = trimmed(css);
    if (text.empty() || lowered(text) == "none") return true;
    size_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() && std::isspace((unsigned char)text[pos])) pos++;
        if (pos >= text.size()) break;
        size_t open = text.find('(', pos);
        if (open == std::string::npos) return false;
        std::string name = lowered(trimmed(text.substr(pos, open - pos)));
        int depth = 1;
        size_t close = open + 1;
        for (; close < text.size() && depth > 0; close++) {
            if (text[close] == '(') depth++;
            else if (text[close] == ')') depth--;
        }
        if (depth != 0) return false;
        std::vector<std::string> args = splitTop(text.substr(open + 1, close - open - 2), false);
        pos = close;

        auto length = [&](size_t i, float reference, float& px) {
            TransformLength len;
            if (i >= args.size() || !parseTransformLength(args[i], fontSize, len)) return false;
            px = len.resolve(reference);
            return true;
        };
        Affine m;
        if (name == "translate" || name == "translate3d") {
            float x = 0, y = 0;
            if (!length(0, width, x)) return false;
            if (args.size() > 1 && !length(1, height, y)) return false;
            m = Affine::translation(x, y);
        } else if (name == "translatex") {
            float x = 0;
            if (!length(0, width, x)) return false;
            m = Affine::translation(x, 0);
        } else if (name == "translatey") {
            float y = 0;
            if (!length(0, height, y)) return false;
            m = Affine::translation(0, y);
        } else if (name == "translatez" || name == "perspective") {
            // Out of the plane: nothing to draw differently without perspective
        } else if (name == "scale" || name == "scale3d") {
            float sx = 1, sy = 1;
            if (args.empty() || !parseFactor(args[0], sx)) return false;
            sy = sx;
            if (args.size() > 1 && !parseFactor(args[1], sy)) return false;
            m = Affine::scaling(sx, sy);
        } else if (name == "scalex" || name == "scaley") {
            float s = 1;
            if (args.empty() || !parseFactor(args[0], s)) return false;
            m = name == "scalex" ? Affine::scaling(s, 1) : Affine::scaling(1, s);
        } else if (name == "scalez") {
            // nothing in the plane
        } else if (name == "rotate" || name == "rotatez") {
            float angle = 0;
            if (args.empty() || !parseAngle(args[0], angle)) return false;
            m = Affine::rotation(angle);
        } else if (name == "rotatex" || name == "rotatey") {
            // Flattened: the projection of a turn about an in-plane axis
            float angle = 0;
            if (args.empty() || !parseAngle(args[0], angle)) return false;
            m = name == "rotatex" ? Affine::scaling(1, std::cos(angle)) : Affine::scaling(std::cos(angle), 1);
        } else if (name == "rotate3d") {
            std::vector<float> axis;
            float angle = 0;
            if (args.size() != 4 || !parseNumbers({args[0], args[1], args[2]}, axis) || !parseAngle(args[3], angle))
                return false;
            if (axis[0] == 0 && axis[1] == 0 && axis[2] != 0) m = Affine::rotation(axis[2] > 0 ? angle : -angle);
            else if (axis[1] == 0 && axis[2] == 0 && axis[0] != 0) m = Affine::scaling(1, std::cos(angle));
            else if (axis[0] == 0 && axis[2] == 0 && axis[1] != 0) m = Affine::scaling(std::cos(angle), 1);
        } else if (name == "skew" || name == "skewx" || name == "skewy") {
            float ax = 0, ay = 0;
            if (args.empty()) return false;
            if (name == "skewy") {
                if (!parseAngle(args[0], ay)) return false;
            } else {
                if (!parseAngle(args[0], ax)) return false;
                if (name == "skew" && args.size() > 1 && !parseAngle(args[1], ay)) return false;
            }
            m = {1, std::tan(ay), std::tan(ax), 1, 0, 0};
        } else if (name == "matrix") {
            std::vector<float> v;
            if (args.size() != 6 || !parseNumbers(args, v)) return false;
            m = {v[0], v[1], v[2], v[3], v[4], v[5]};
        } else if (name == "matrix3d") {
            // Column-major 4x4: the plane's part is the x and y columns' x
            // and y, and the translation column's
            std::vector<float> v;
            if (args.size() != 16 || !parseNumbers(args, v)) return false;
            m = {v[0], v[1], v[4], v[5], v[12], v[13]};
        } else {
            return false;
        }
        out = out * m;
    }
    return true;
}

std::string individualTransformFunctions(const std::string& translate, const std::string& rotate,
                                         const std::string& scale) {
    std::string out;
    auto none = [](const std::string& v) {
        std::string t = lowered(trimmed(v));
        return t.empty() || t == "none";
    };
    auto add = [&](const char* name, const std::vector<std::string>& args, size_t count) {
        if (!out.empty()) out += ' ';
        out += name;
        out += '(';
        for (size_t i = 0; i < count && i < args.size(); i++) {
            if (i) out += ", ";
            out += args[i];
        }
        out += ')';
    };
    if (!none(translate)) add("translate", splitTop(translate, true), 2);
    if (!none(rotate)) {
        // "45deg", "z 45deg" or "0 0 1 45deg": the angle is the last part
        std::vector<std::string> parts = splitTop(rotate, true);
        std::string axis = parts.size() > 1 ? lowered(parts[0]) : "z";
        if (parts.size() == 4) add("rotate3d", parts, 4);
        else if (axis == "x") add("rotateX", {parts.back()}, 1);
        else if (axis == "y") add("rotateY", {parts.back()}, 1);
        else add("rotate", {parts.back()}, 1);
    }
    if (!none(scale)) add("scale", splitTop(scale, true), 2);
    return out;
}

std::string resolvedTransformValue(const std::string& css, float width, float height, float fontSize) {
    std::string t = lowered(trimmed(css));
    Affine m;
    if (t.empty() || t == "none" || !parseTransformList(css, width, height, fontSize, m)) return "none";
    auto num = [](float v) {
        if (std::fabs(v) < 5e-7f) v = 0;
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.6g", v);
        return std::string(buf);
    };
    return "matrix(" + num(m.a) + ", " + num(m.b) + ", " + num(m.c) + ", " + num(m.d) + ", " + num(m.e) + ", " +
           num(m.f) + ")";
}

void parseTransformOrigin(const std::string& css, float width, float height, float fontSize, float& x, float& y) {
    x = width / 2.0f;
    y = height / 2.0f;
    std::vector<std::string> parts = splitTop(lowered(css), true);
    if (parts.empty()) return;
    // Keywords name their axis; a length or percentage takes the first
    // free one (x, then y). A third part is the z offset.
    bool haveX = false, haveY = false;
    for (size_t i = 0; i < parts.size() && i < 2; i++) {
        const std::string& p = parts[i];
        if (p == "left") { x = 0; haveX = true; }
        else if (p == "right") { x = width; haveX = true; }
        else if (p == "top") { y = 0; haveY = true; }
        else if (p == "bottom") { y = height; haveY = true; }
        else if (p == "center") {
            if (i == 0 && parts.size() > 1 && (parts[1] == "left" || parts[1] == "right")) haveY = true;
            else if (!haveX) haveX = true;
            else haveY = true;
        } else {
            TransformLength len;
            if (!parseTransformLength(p, fontSize, len)) continue;
            if (!haveX) { x = len.resolve(width); haveX = true; }
            else { y = len.resolve(height); haveY = true; }
        }
    }
}

bool boxTransform(const LayoutBox& box, float x, float y, Affine& out) {
    if (box.transform.empty()) return false;
    Affine m;
    if (!parseTransformList(box.transform, box.width, box.height, box.fontSize, m) || m.isIdentity()) return false;
    float ox = 0, oy = 0;
    parseTransformOrigin(box.transformOrigin, box.width, box.height, box.fontSize, ox, oy);
    out = Affine::translation(x + ox, y + oy) * m * Affine::translation(-(x + ox), -(y + oy));
    return true;
}

bool toBoxSpace(const LayoutBox& box, float& x, float& y) {
    Affine m, inverse;
    if (!boxTransform(box, box.screenX, box.screenY, m)) return true;
    if (!m.invert(inverse)) return false;
    inverse.map(x, y);
    return true;
}

}  // namespace ZepraBrowser
