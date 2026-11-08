// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file gradient_raster.cpp
 * @brief CSS gradient backgrounds drawn on the CPU (see gradient_raster.h)
 */

#include "gradient_raster.h"

#include "../css/css_computed_style.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ZepraBrowser {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kRampSize = 1024;
// Bigger boxes are drawn at a lower resolution and stretched
constexpr double kMaxPixels = 2.5e6;

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\f"), b = s.find_last_not_of(" \t\r\n\f");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Splits at `sep` outside parentheses (whitespace: any run of it)
std::vector<std::string> splitTop(const std::string& s, char sep) {
    std::vector<std::string> out;
    int depth = 0;
    std::string cur;
    for (char c : s) {
        if (c == '(') depth++;
        else if (c == ')') depth--;
        bool split = depth == 0 && (sep == ' ' ? std::isspace((unsigned char)c) != 0 : c == sep);
        if (split) {
            if (sep != ' ' || !cur.empty()) out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (sep != ' ' || !trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

// A number with its unit ("12px", "50%", "45deg", "-8px", "0")
struct Dim {
    float value = 0;
    std::string unit;
};

bool parseDim(const std::string& text, Dim& out) {
    if (text.empty()) return false;
    const char* begin = text.c_str();
    char* end = nullptr;
    float v = std::strtof(begin, &end);
    if (end == begin) return false;
    out.value = v;
    out.unit = lower(std::string(end));
    return true;
}

bool parseAngle(const std::string& text, float& deg) {
    Dim d;
    if (!parseDim(text, d)) return false;
    if (d.unit == "deg") deg = d.value;
    else if (d.unit == "rad") deg = d.value * 180.0f / kPi;
    else if (d.unit == "turn") deg = d.value * 360.0f;
    else if (d.unit == "grad") deg = d.value * 0.9f;
    else if (d.unit.empty() && d.value == 0) deg = 0;
    else return false;
    return true;
}

// A length or percentage against `basis` (px, %, em/rem at 16px)
bool parseLengthPercent(const std::string& text, float basis, float& px) {
    Dim d;
    if (!parseDim(text, d)) return false;
    if (d.unit == "px" || (d.unit.empty() && d.value == 0)) px = d.value;
    else if (d.unit == "%") px = d.value / 100.0f * basis;
    else if (d.unit == "em" || d.unit == "rem") px = d.value * 16.0f;
    else if (d.unit == "pt") px = d.value * 4.0f / 3.0f;
    else return false;
    return true;
}

struct Premul {
    float r = 0, g = 0, b = 0, a = 0;  // 0..1, colour channels multiplied by alpha
};

Premul premul(const Zepra::WebCore::CSSColor& c) {
    float a = c.a / 255.0f;
    return {c.r / 255.0f * a, c.g / 255.0f * a, c.b / 255.0f * a, a};
}

Premul mix(const Premul& x, const Premul& y, float t) {
    return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t};
}

bool looksLikeColor(const std::string& token) {
    if (token.empty()) return false;
    if (token[0] == '#') return true;
    if (token.find('(') != std::string::npos) return true;  // rgb(), hsl(), ...
    return std::isalpha((unsigned char)token[0]) != 0;
}

// One colour stop (or transition hint) as written
struct RawStop {
    bool hint = false;
    Premul color;
    std::string position;  // "" when not given
};

enum class Kind { Linear, Radial, Conic };

struct Gradient {
    Kind kind = Kind::Linear;
    bool repeating = false;
    // linear
    float angle = 180;              // degrees, CSS convention (0 = to top)
    int corner = 0;                 // "to <corner>": 1 top-right, 2 bottom-right, 3 bottom-left, 4 top-left
    // radial
    bool circle = false;
    std::string sizeKeyword = "farthest-corner";
    std::vector<std::string> explicitSize;  // one (circle) or two lengths
    // radial and conic
    std::vector<std::string> at;    // position after "at"
    float from = 0;                 // conic start angle
    std::vector<RawStop> stops;
};

// "to right", "to top left", ...: false when the token is not one
bool parseDirection(const std::string& token, Gradient& g) {
    std::vector<std::string> words = splitTop(lower(token), ' ');
    if (words.size() < 2 || words[0] != "to") return false;
    bool top = false, bottom = false, left = false, right = false;
    for (size_t i = 1; i < words.size(); i++) {
        if (words[i] == "top") top = true;
        else if (words[i] == "bottom") bottom = true;
        else if (words[i] == "left") left = true;
        else if (words[i] == "right") right = true;
        else return false;
    }
    g.corner = 0;
    if (top && right) g.corner = 1;
    else if (bottom && right) g.corner = 2;
    else if (bottom && left) g.corner = 3;
    else if (top && left) g.corner = 4;
    else if (top) g.angle = 0;
    else if (right) g.angle = 90;
    else if (bottom) g.angle = 180;
    else if (left) g.angle = 270;
    else return false;
    return true;
}

// The shape/size/position prelude of radial-gradient() and conic-gradient()
bool parsePrelude(const std::string& token, Gradient& g) {
    std::vector<std::string> words = splitTop(token, ' ');
    if (words.empty()) return false;
    size_t i = 0;
    bool any = false;
    for (; i < words.size(); i++) {
        std::string w = lower(words[i]);
        if (w == "at") break;
        any = true;
        if (g.kind == Kind::Conic) {
            if (w == "from" && i + 1 < words.size() && parseAngle(words[i + 1], g.from)) { i++; continue; }
            return false;
        }
        if (w == "circle") g.circle = true;
        else if (w == "ellipse") g.circle = false;
        else if (w == "closest-side" || w == "farthest-side" || w == "closest-corner" || w == "farthest-corner")
            g.sizeKeyword = w;
        else if (Dim d; parseDim(w, d)) g.explicitSize.push_back(w);
        else return false;
    }
    if (g.kind == Kind::Radial && g.explicitSize.size() == 1 && !g.circle) {
        // A single length is a circle's radius
        bool saidEllipse = false;
        for (const auto& w : words) if (lower(w) == "ellipse") saidEllipse = true;
        if (saidEllipse) return false;
        g.circle = true;
    }
    if (i < words.size()) {
        if (i + 1 >= words.size()) return false;
        g.at.assign(words.begin() + (long)i + 1, words.end());
        any = true;
    }
    return any;
}

bool parseGradient(const std::string& layer, Gradient& g) {
    std::string text = trim(layer);
    size_t open = text.find('(');
    if (open == std::string::npos || text.back() != ')') return false;
    std::string name = lower(trim(text.substr(0, open)));
    if (name.compare(0, 8, "-webkit-") == 0) name = name.substr(8);
    if (name.compare(0, 10, "repeating-") == 0) {
        g.repeating = true;
        name = name.substr(10);
    }
    if (name == "linear-gradient") g.kind = Kind::Linear;
    else if (name == "radial-gradient") g.kind = Kind::Radial;
    else if (name == "conic-gradient") g.kind = Kind::Conic;
    else return false;

    std::vector<std::string> args = splitTop(text.substr(open + 1, text.size() - open - 2), ',');
    size_t first = 0;
    if (!args.empty()) {
        const std::string& a0 = args[0];
        if (g.kind == Kind::Linear) {
            float deg;
            if (parseAngle(a0, deg)) { g.angle = deg; first = 1; }
            else if (parseDirection(a0, g)) first = 1;
        } else if (!looksLikeColor(a0) || lower(a0).compare(0, 2, "at") == 0 ||
                   lower(a0).compare(0, 4, "from") == 0 || lower(a0).find("circle") != std::string::npos ||
                   lower(a0).find("ellipse") != std::string::npos || lower(a0).find("closest") != std::string::npos ||
                   lower(a0).find("farthest") != std::string::npos) {
            if (parsePrelude(a0, g)) first = 1;
        }
    }
    for (size_t i = first; i < args.size(); i++) {
        std::vector<std::string> parts = splitTop(args[i], ' ');
        if (parts.empty()) return false;
        if (!looksLikeColor(parts[0])) {
            // A transition hint: a lone position between two colours
            if (parts.size() != 1) return false;
            RawStop hint;
            hint.hint = true;
            hint.position = parts[0];
            g.stops.push_back(hint);
            continue;
        }
        RawStop stop;
        stop.color = premul(Zepra::WebCore::CSSColor::parse(lower(parts[0])));
        if (parts.size() >= 2) stop.position = parts[1];
        g.stops.push_back(stop);
        if (parts.size() >= 3) {
            // "red 10% 20%": the colour twice
            RawStop second = stop;
            second.position = parts[2];
            g.stops.push_back(second);
        }
    }
    // Hints need a colour on each side
    while (!g.stops.empty() && g.stops.front().hint) g.stops.erase(g.stops.begin());
    while (!g.stops.empty() && g.stops.back().hint) g.stops.pop_back();
    for (size_t i = 1; i < g.stops.size(); i++) {
        if (g.stops[i].hint && g.stops[i - 1].hint) return false;
    }
    return !g.stops.empty();
}

// Stops resolved to positions along the gradient (in its own units: px
// along the line or ray, or degrees around a conic centre)
struct Stop {
    Premul color;
    float pos = 0;
    float hint = -1;  // a transition hint between this stop and the next
};

std::vector<Stop> resolveStops(const Gradient& g, float length) {
    // `length`: what 100% is (the line, the ray, or 360 degrees)
    std::vector<Stop> stops;
    std::vector<bool> known;
    float pendingHint = -1;
    bool hasPendingHint = false;
    for (const RawStop& raw : g.stops) {
        float pos = 0;
        bool have = false;
        if (!raw.position.empty()) {
            if (g.kind == Kind::Conic) {
                Dim d;
                if (parseDim(raw.position, d) && d.unit == "%") { pos = d.value / 100.0f * length; have = true; }
                else if (float deg; parseAngle(raw.position, deg)) { pos = deg; have = true; }
            } else {
                have = parseLengthPercent(raw.position, length, pos);
            }
        }
        if (raw.hint) {
            if (have) { pendingHint = pos; hasPendingHint = true; }
            continue;
        }
        if (hasPendingHint && !stops.empty()) stops.back().hint = pendingHint;
        hasPendingHint = false;
        Stop s;
        s.color = raw.color;
        s.pos = pos;
        stops.push_back(s);
        known.push_back(have);
    }
    if (stops.empty()) return stops;
    // CSS Images 3 §3.4.3: first and last default to 0% and 100%, positions
    // never go back, and unpositioned stops share the space between
    if (!known.front()) { stops.front().pos = 0; known.front() = true; }
    if (!known.back()) { stops.back().pos = length; known.back() = true; }
    float maxSoFar = stops.front().pos;
    for (size_t i = 0; i < stops.size(); i++) {
        if (known[i]) {
            stops[i].pos = std::max(stops[i].pos, maxSoFar);
            maxSoFar = stops[i].pos;
        }
    }
    for (size_t i = 1; i < stops.size(); i++) {
        if (known[i]) continue;
        size_t j = i;
        while (!known[j]) j++;
        float a = stops[i - 1].pos, b = stops[j].pos;
        for (size_t k = i; k < j; k++) stops[k].pos = a + (b - a) * (float)(k - i + 1) / (float)(j - i + 1);
        i = j;
    }
    for (Stop& s : stops) {
        if (s.hint >= 0) s.hint = std::max(s.hint, s.pos);
    }
    return stops;
}

// The colour at position p (in stop units)
Premul colorAt(const std::vector<Stop>& stops, float p) {
    if (p <= stops.front().pos) return stops.front().color;
    if (p >= stops.back().pos) return stops.back().color;
    for (size_t i = 0; i + 1 < stops.size(); i++) {
        const Stop& a = stops[i];
        const Stop& b = stops[i + 1];
        if (p > b.pos) continue;
        float span = b.pos - a.pos;
        if (span <= 0) return b.color;
        float t = (p - a.pos) / span;
        if (a.hint >= 0 && a.hint < b.pos) {
            float h = (a.hint - a.pos) / span;
            if (h <= 0) t = 1;
            else if (h >= 1) t = 0;
            else t = std::pow(t, std::log(0.5f) / std::log(h));
        }
        return mix(a.color, b.color, t);
    }
    return stops.back().color;
}

// background-position for one layer: x then y offsets of the tile inside
// the positioning area
void layerPosition(const std::string& text, float areaW, float areaH, float tileW, float tileH, float& ox, float& oy) {
    std::vector<std::string> words = splitTop(lower(text), ' ');
    auto isY = [](const std::string& w) { return w == "top" || w == "bottom"; };
    auto isX = [](const std::string& w) { return w == "left" || w == "right"; };
    auto keywordFraction = [](const std::string& w) -> float {
        if (w == "left" || w == "top") return 0;
        if (w == "right" || w == "bottom") return 1;
        return 0.5f;  // center
    };
    auto value = [&](const std::string& w, float area, float tile) -> float {
        if (w == "left" || w == "top" || w == "right" || w == "bottom" || w == "center")
            return (area - tile) * keywordFraction(w);
        Dim d;
        if (parseDim(w, d) && d.unit == "%") return (area - tile) * d.value / 100.0f;
        float px = 0;
        parseLengthPercent(w, area, px);
        return px;
    };
    ox = 0;
    oy = 0;
    if (words.empty()) return;
    if (words.size() == 1) {
        if (isY(words[0])) { ox = value("center", areaW, tileW); oy = value(words[0], areaH, tileH); }
        else { ox = value(words[0], areaW, tileW); oy = value("center", areaH, tileH); }
        return;
    }
    if (words.size() == 2) {
        std::string x = words[0], y = words[1];
        if (isY(x) || isX(y)) std::swap(x, y);
        ox = value(x, areaW, tileW);
        oy = value(y, areaH, tileH);
        return;
    }
    // Three or four values: edge keywords, each with an optional offset
    float fx = 0, fy = 0;
    bool setX = false, setY = false;
    for (size_t i = 0; i < words.size(); i++) {
        const std::string& w = words[i];
        bool hasOffset = i + 1 < words.size() && !isX(words[i + 1]) && !isY(words[i + 1]) && words[i + 1] != "center";
        float offset = 0;
        if (hasOffset) {
            Dim d;
            if (parseDim(words[i + 1], d) && d.unit == "%") offset = (isY(w) ? areaH - tileH : areaW - tileW) * d.value / 100.0f;
            else parseLengthPercent(words[i + 1], isY(w) ? areaH : areaW, offset);
        }
        if (isX(w) || (w == "center" && !setX && !(i + 1 < words.size() && isX(words[i + 1])))) {
            fx = w == "right" ? (areaW - tileW) - offset : w == "center" ? (areaW - tileW) / 2 : offset;
            setX = true;
        } else if (isY(w) || w == "center") {
            fy = w == "bottom" ? (areaH - tileH) - offset : w == "center" ? (areaH - tileH) / 2 : offset;
            setY = true;
        }
        if (hasOffset) i++;
    }
    ox = setX ? fx : (areaW - tileW) / 2;
    oy = setY ? fy : (areaH - tileH) / 2;
}

// background-size for a gradient (no natural size): auto is the area
void layerSize(const std::string& text, float areaW, float areaH, float& tw, float& th) {
    std::vector<std::string> words = splitTop(lower(text), ' ');
    tw = areaW;
    th = areaH;
    if (words.empty() || words[0] == "cover" || words[0] == "contain") return;
    float v;
    if (words[0] != "auto" && parseLengthPercent(words[0], areaW, v)) tw = v;
    if (words.size() > 1 && words[1] != "auto" && parseLengthPercent(words[1], areaH, v)) th = v;
}

void layerRepeat(const std::string& text, bool& rx, bool& ry) {
    std::vector<std::string> words = splitTop(lower(text), ' ');
    rx = ry = true;
    if (words.size() == 1) {
        if (words[0] == "no-repeat") rx = ry = false;
        else if (words[0] == "repeat-x") ry = false;
        else if (words[0] == "repeat-y") rx = false;
    } else if (words.size() >= 2) {
        rx = words[0] != "no-repeat";
        ry = words[1] != "no-repeat";
    }
}

// The i-th entry of a comma list, the list repeating (CSS Backgrounds 3 §2.2)
std::string listItem(const std::vector<std::string>& list, size_t i) {
    if (list.empty()) return "";
    return list[i % list.size()];
}

// One layer, ready to sample at a point of its tile
struct Layer {
    Gradient g;
    float tileW = 0, tileH = 0;
    float originX = 0, originY = 0;  // the tile's top-left in border-box px
    bool repeatX = true, repeatY = true;
    // Geometry within the tile
    float cx = 0, cy = 0;        // radial/conic centre; linear: tile centre
    float dirX = 0, dirY = 0;    // linear direction
    float lineLength = 1;        // linear: gradient line; radial: x radius
    float rx = 1, ry = 1;        // radial radii
    std::vector<Stop> stops;
    float first = 0, last = 1;   // stop range (repeating)
    std::vector<Premul> ramp;    // colours sampled over [first, last] (or the stop range)
    float rampFrom = 0, rampTo = 1;
};

bool prepareLayer(Layer& L) {
    float w = L.tileW, h = L.tileH;
    const Gradient& g = L.g;
    float length = 1;
    if (g.kind == Kind::Linear) {
        float angle = g.angle;
        float a = std::atan2(h, w) * 180.0f / kPi;
        switch (g.corner) {
            case 1: angle = a; break;
            case 2: angle = 180 - a; break;
            case 3: angle = 180 + a; break;
            case 4: angle = 360 - a; break;
            default: break;
        }
        float rad = angle * kPi / 180.0f;
        L.dirX = std::sin(rad);
        L.dirY = -std::cos(rad);
        L.cx = w / 2;
        L.cy = h / 2;
        length = std::fabs(w * L.dirX) + std::fabs(h * L.dirY);
        if (length <= 0) length = 1;
        L.lineLength = length;
    } else {
        // Centre: "at <position>", default the middle
        float px = w / 2, py = h / 2;
        if (!g.at.empty()) {
            std::string at;
            for (const auto& s : g.at) at += (at.empty() ? "" : " ") + s;
            // A position of a zero-size tile in a w x h area: the offsets
            // are the point itself
            layerPosition(at, w, h, 0, 0, px, py);
        }
        L.cx = px;
        L.cy = py;
        if (g.kind == Kind::Radial) {
            float dl = std::fabs(px), dr = std::fabs(w - px), dt = std::fabs(py), db = std::fabs(h - py);
            float rx = 0, ry = 0;
            if (!g.explicitSize.empty()) {
                parseLengthPercent(g.explicitSize[0], w, rx);
                ry = rx;
                if (!g.circle && g.explicitSize.size() > 1) parseLengthPercent(g.explicitSize[1], h, ry);
            } else {
                const std::string& k = g.sizeKeyword;
                bool closest = k.compare(0, 7, "closest") == 0;
                bool corner = k.find("corner") != std::string::npos;
                float sx = closest ? std::min(dl, dr) : std::max(dl, dr);
                float sy = closest ? std::min(dt, db) : std::max(dt, db);
                if (g.circle) {
                    if (corner) rx = std::sqrt(sx * sx + sy * sy);
                    else rx = closest ? std::min(sx, sy) : std::max(sx, sy);
                    ry = rx;
                } else if (corner) {
                    // An ellipse with the side case's aspect ratio through
                    // that corner: rx = sqrt(2) * sx, ry = sqrt(2) * sy
                    rx = sx * std::sqrt(2.0f);
                    ry = sy * std::sqrt(2.0f);
                } else {
                    rx = sx;
                    ry = sy;
                }
            }
            L.rx = std::max(rx, 1e-3f);
            L.ry = std::max(ry, 1e-3f);
            length = L.rx;
        } else {
            length = 360.0f;
        }
    }
    L.stops = resolveStops(g, length);
    if (L.stops.empty()) return false;
    L.first = L.stops.front().pos;
    L.last = L.stops.back().pos;
    if (g.repeating && L.last - L.first <= 1e-4f) {
        // A zero-length repeat paints the average colour (§3.4.3); its last colour is close enough
        L.g.repeating = false;
    }
    L.rampFrom = L.first;
    L.rampTo = L.last > L.first ? L.last : L.first + 1;
    L.ramp.resize(kRampSize);
    for (int i = 0; i < kRampSize; i++) {
        float p = L.rampFrom + (L.rampTo - L.rampFrom) * (float)i / (float)(kRampSize - 1);
        L.ramp[i] = colorAt(L.stops, p);
    }
    return true;
}

// The layer's colour at a point of its tile (tile coordinates, px)
Premul sampleLayer(const Layer& L, float x, float y) {
    float p;
    if (L.g.kind == Kind::Linear) {
        // px along the gradient line, which is centred on the tile
        p = (x - L.cx) * L.dirX + (y - L.cy) * L.dirY + L.lineLength / 2;
    } else if (L.g.kind == Kind::Radial) {
        float dx = (x - L.cx) / L.rx, dy = (y - L.cy) / L.ry;
        p = std::sqrt(dx * dx + dy * dy) * L.rx;
    } else {
        // Clockwise from the top, starting at `from`
        float deg = std::atan2(x - L.cx, -(y - L.cy)) * 180.0f / kPi - L.g.from;
        p = std::fmod(std::fmod(deg, 360.0f) + 360.0f, 360.0f);
    }
    if (L.g.repeating) {
        float span = L.last - L.first;
        p = L.first + std::fmod(std::fmod(p - L.first, span) + span, span);
    } else {
        // A hair of tolerance: layers that meet at a hard stop on a diagonal
        // (checkerboards) would otherwise both miss the pixels on the seam
        if (p <= L.first + 1e-3f) return L.stops.front().color;
        if (p >= L.last - 1e-3f) return L.stops.back().color;
    }
    float f = (p - L.rampFrom) / (L.rampTo - L.rampFrom) * (kRampSize - 1);
    int i = std::clamp((int)std::lround(f), 0, kRampSize - 1);
    return L.ramp[i];
}

// Coverage of a pixel centre by the rounded border box (1px soft edge)
float roundedCoverage(float x, float y, float w, float h, float r) {
    if (r <= 0) return 1;
    r = std::min({r, w / 2, h / 2});
    float cx = x < r ? r : (x > w - r ? w - r : x);
    float cy = y < r ? r : (y > h - r ? h - r : y);
    float dx = x - cx, dy = y - cy;
    if (dx == 0 && dy == 0) return 1;
    float d = std::sqrt(dx * dx + dy * dy);
    return std::clamp(r - d + 0.5f, 0.0f, 1.0f);
}

} // namespace

void gradientPixelSize(float width, float height, int& pixelWidth, int& pixelHeight) {
    double w = std::max(1.0f, std::round(width)), h = std::max(1.0f, std::round(height));
    double scale = w * h > kMaxPixels ? std::sqrt(kMaxPixels / (w * h)) : 1.0;
    pixelWidth = std::max(1, (int)std::lround(w * scale));
    pixelHeight = std::max(1, (int)std::lround(h * scale));
}

bool isGradientBackground(const std::string& image) {
    if (image.find("gradient(") == std::string::npos) return false;
    return lower(image).find("url(") == std::string::npos;
}

std::string gradientBackgroundKey(const GradientBackground& bg, int pixelWidth, int pixelHeight) {
    char dims[160];
    std::snprintf(dims, sizeof dims, "|%dx%d|%.2f,%.2f|%.2f,%.2f,%.2f,%.2f|%.2f", pixelWidth, pixelHeight, bg.width,
                  bg.height, bg.borderTop, bg.borderRight, bg.borderBottom, bg.borderLeft, bg.radius);
    return bg.image + "|" + bg.size + "|" + bg.position + "|" + bg.repeat + dims;
}

bool rasterizeGradientBackground(const GradientBackground& bg, int pixelWidth, int pixelHeight,
                                 std::vector<uint8_t>& rgba) {
    if (pixelWidth <= 0 || pixelHeight <= 0 || bg.width <= 0 || bg.height <= 0) return false;
    // Positioning area: the padding box (background-origin: padding-box)
    float areaX = bg.borderLeft, areaY = bg.borderTop;
    float areaW = std::max(0.0f, bg.width - bg.borderLeft - bg.borderRight);
    float areaH = std::max(0.0f, bg.height - bg.borderTop - bg.borderBottom);

    std::vector<std::string> images = splitTop(bg.image, ',');
    std::vector<std::string> sizes = bg.size.empty() ? std::vector<std::string>{} : splitTop(bg.size, ',');
    std::vector<std::string> positions = bg.position.empty() ? std::vector<std::string>{} : splitTop(bg.position, ',');
    std::vector<std::string> repeats = bg.repeat.empty() ? std::vector<std::string>{} : splitTop(bg.repeat, ',');

    // The first layer is on top: draw from the last
    std::vector<Layer> layers;
    for (size_t i = images.size(); i-- > 0;) {
        Layer L;
        if (!parseGradient(images[i], L.g)) continue;
        layerSize(listItem(sizes, i), areaW, areaH, L.tileW, L.tileH);
        if (L.tileW < 0.01f || L.tileH < 0.01f) continue;
        float ox, oy;
        layerPosition(listItem(positions, i).empty() ? "0% 0%" : listItem(positions, i), areaW, areaH, L.tileW,
                      L.tileH, ox, oy);
        L.originX = areaX + ox;
        L.originY = areaY + oy;
        layerRepeat(listItem(repeats, i), L.repeatX, L.repeatY);
        if (!prepareLayer(L)) continue;
        layers.push_back(std::move(L));
    }
    if (layers.empty()) return false;

    rgba.assign((size_t)pixelWidth * pixelHeight * 4, 0);
    float sx = bg.width / (float)pixelWidth, sy = bg.height / (float)pixelHeight;
    for (int j = 0; j < pixelHeight; j++) {
        float y = (j + 0.5f) * sy;
        for (int i = 0; i < pixelWidth; i++) {
            float x = (i + 0.5f) * sx;
            Premul out;
            for (const Layer& L : layers) {
                float u = x - L.originX, v = y - L.originY;
                if (L.repeatX) u = std::fmod(std::fmod(u, L.tileW) + L.tileW, L.tileW);
                else if (u < 0 || u >= L.tileW) continue;
                if (L.repeatY) v = std::fmod(std::fmod(v, L.tileH) + L.tileH, L.tileH);
                else if (v < 0 || v >= L.tileH) continue;
                Premul c = sampleLayer(L, u, v);
                // Source over
                float k = 1 - c.a;
                out = {c.r + out.r * k, c.g + out.g * k, c.b + out.b * k, c.a + out.a * k};
            }
            float cover = roundedCoverage(x, y, bg.width, bg.height, bg.radius);
            float a = out.a * cover;
            uint8_t* px = &rgba[((size_t)j * pixelWidth + i) * 4];
            if (out.a > 0) {
                px[0] = (uint8_t)std::lround(std::clamp(out.r / out.a, 0.0f, 1.0f) * 255);
                px[1] = (uint8_t)std::lround(std::clamp(out.g / out.a, 0.0f, 1.0f) * 255);
                px[2] = (uint8_t)std::lround(std::clamp(out.b / out.a, 0.0f, 1.0f) * 255);
            }
            px[3] = (uint8_t)std::lround(std::clamp(a, 0.0f, 1.0f) * 255);
        }
    }
    return true;
}

} // namespace ZepraBrowser
