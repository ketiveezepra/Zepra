// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file box_shadow.cpp
 * @brief box-shadow parsing and CPU drawing (see box_shadow.h)
 */

#include "box_shadow.h"

#include "../css/css_computed_style.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ZepraBrowser {

namespace {

constexpr double kMaxPixels = 2.0e6;

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\f"), b = s.find_last_not_of(" \t\r\n\f");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// Splits at `sep` outside parentheses (' ': any run of whitespace)
std::vector<std::string> splitTop(const std::string& s, char sep) {
    std::vector<std::string> out;
    int depth = 0;
    std::string cur;
    for (char c : s) {
        if (c == '(') depth++;
        else if (c == ')') depth--;
        bool split = depth == 0 && (sep == ' ' ? std::isspace((unsigned char)c) != 0 : c == sep);
        if (split) {
            if (!trim(cur).empty() || sep != ' ') out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!trim(cur).empty() || sep != ' ') out.push_back(trim(cur));
    return out;
}

bool parsePx(const std::string& t, float& px) {
    if (t.empty()) return false;
    char* end = nullptr;
    float v = std::strtof(t.c_str(), &end);
    if (end == t.c_str()) return false;
    std::string unit(end);
    for (char& c : unit) c = (char)std::tolower((unsigned char)c);
    if (unit == "px" || (unit.empty() && v == 0)) px = v;
    else if (unit == "em" || unit == "rem") px = v * 16;
    else if (unit == "pt") px = v * 4.0f / 3.0f;
    else return false;
    return true;
}

// Coverage of the pixel centred at (x, y) by a rounded rectangle (1px soft edge)
float coverage(float x, float y, float rx, float ry, float rw, float rh, float r) {
    if (rw <= 0 || rh <= 0) return 0;
    float dx = std::max(rx - x, x - (rx + rw)), dy = std::max(ry - y, y - (ry + rh));
    r = std::max(0.0f, std::min({r, rw / 2, rh / 2}));
    if (r <= 0) {
        float cx = std::clamp(0.5f - dx, 0.0f, 1.0f), cy = std::clamp(0.5f - dy, 0.0f, 1.0f);
        return cx * cy;
    }
    float cx = std::clamp(x, rx + r, rx + rw - r), cy = std::clamp(y, ry + r, ry + rh - r);
    float d = std::hypot(x - cx, y - cy);
    return std::clamp(r - d + 0.5f, 0.0f, 1.0f);
}

// One box-blur pass of width 2*radius+1 along rows (or columns), the edges
// clamped
void boxBlur(std::vector<float>& v, int w, int h, int radius, bool horizontal) {
    if (radius <= 0) return;
    int n = horizontal ? w : h, lines = horizontal ? h : w;
    std::vector<float> line((size_t)n), out((size_t)n);
    float scale = 1.0f / (float)(2 * radius + 1);
    for (int l = 0; l < lines; l++) {
        for (int i = 0; i < n; i++) line[i] = horizontal ? v[(size_t)l * w + i] : v[(size_t)i * w + l];
        float sum = 0;
        for (int k = -radius; k <= radius; k++) sum += line[std::clamp(k, 0, n - 1)];
        for (int i = 0; i < n; i++) {
            out[i] = sum * scale;
            sum += line[std::min(i + radius + 1, n - 1)] - line[std::max(i - radius, 0)];
        }
        for (int i = 0; i < n; i++) {
            if (horizontal) v[(size_t)l * w + i] = out[i];
            else v[(size_t)i * w + l] = out[i];
        }
    }
}

// A Gaussian blur of deviation sigma, as three box blurs each way
void gaussian(std::vector<float>& v, int w, int h, float sigma) {
    if (sigma < 0.3f) return;
    int radius = (int)std::lround((std::sqrt(4.0f * sigma * sigma + 1.0f) - 1.0f) / 2.0f);
    radius = std::max(1, radius);
    for (int pass = 0; pass < 3; pass++) {
        boxBlur(v, w, h, radius, true);
        boxBlur(v, w, h, radius, false);
    }
}

} // namespace

std::vector<BoxShadow> parseBoxShadow(const std::string& value, uint32_t currentColor) {
    std::vector<BoxShadow> out;
    std::string v = trim(value);
    if (v.empty() || v == "none") return out;
    for (const std::string& layer : splitTop(v, ',')) {
        BoxShadow s;
        s.r = (currentColor >> 16) & 0xFF;
        s.g = (currentColor >> 8) & 0xFF;
        s.b = currentColor & 0xFF;
        std::vector<float> lengths;
        bool ok = true;
        for (const std::string& token : splitTop(layer, ' ')) {
            std::string t = token;
            for (char& c : t) c = (char)std::tolower((unsigned char)c);
            float px;
            if (t == "inset") s.inset = true;
            else if (parsePx(t, px)) lengths.push_back(px);
            else if (t == "currentcolor") continue;
            else {
                Zepra::WebCore::CSSColor c = Zepra::WebCore::CSSColor::parse(t);
                s.r = c.r; s.g = c.g; s.b = c.b; s.a = c.a;
            }
        }
        if (lengths.size() < 2 || lengths.size() > 4) ok = false;
        if (!ok) return {};  // an invalid value is ignored as a whole
        s.offsetX = lengths[0];
        s.offsetY = lengths[1];
        s.blur = lengths.size() > 2 ? std::max(0.0f, lengths[2]) : 0;
        s.spread = lengths.size() > 3 ? lengths[3] : 0;
        out.push_back(s);
    }
    return out;
}

ShadowRect shadowRect(const ShadowBox& box, const BoxShadow& s) {
    if (s.inset) return {0, 0, box.width, box.height};
    float extent = std::ceil(s.blur * 1.5f) + 1;
    return {s.offsetX - s.spread - extent, s.offsetY - s.spread - extent,
            box.width + 2 * (s.spread + extent), box.height + 2 * (s.spread + extent)};
}

void shadowPixelSize(const ShadowRect& rect, int& pixelWidth, int& pixelHeight) {
    double w = std::max(1.0f, std::ceil(rect.width)), h = std::max(1.0f, std::ceil(rect.height));
    double scale = w * h > kMaxPixels ? std::sqrt(kMaxPixels / (w * h)) : 1.0;
    pixelWidth = std::max(1, (int)std::lround(w * scale));
    pixelHeight = std::max(1, (int)std::lround(h * scale));
}

std::string shadowKey(const ShadowBox& box, const BoxShadow& s, int pixelWidth, int pixelHeight) {
    char key[256];
    std::snprintf(key, sizeof key, "shadow|%dx%d|%.2f,%.2f|%.2f,%.2f,%.2f,%.2f|%.2f|%d|%.2f,%.2f,%.2f,%.2f|%02x%02x%02x%02x",
                  pixelWidth, pixelHeight, box.width, box.height, box.borderTop, box.borderRight, box.borderBottom,
                  box.borderLeft, box.radius, s.inset ? 1 : 0, s.offsetX, s.offsetY, s.blur, s.spread, s.r, s.g,
                  s.b, s.a);
    return key;
}

bool rasterizeShadow(const ShadowBox& box, const BoxShadow& s, int pixelWidth, int pixelHeight,
                     std::vector<uint8_t>& rgba) {
    if (s.a == 0 || box.width <= 0 || box.height <= 0 || pixelWidth <= 0 || pixelHeight <= 0) return false;
    ShadowRect rect = shadowRect(box, s);
    float scale = (float)pixelWidth / rect.width;
    float sigma = s.blur / 2 * scale;
    // Blurring reads beyond the bitmap: work on it with a margin
    int margin = s.inset ? (int)std::ceil(s.blur * 1.5f * scale) + 2 : 0;
    int w = pixelWidth + 2 * margin, h = pixelHeight + 2 * margin;
    std::vector<float> mask((size_t)w * h);
    float maxBorder = std::max({box.borderTop, box.borderRight, box.borderBottom, box.borderLeft});

    // The shape whose blurred mask is the shadow, in rect coordinates
    float sx, sy, sw, sh, sr;
    if (s.inset) {
        float pw = box.width - box.borderLeft - box.borderRight, ph = box.height - box.borderTop - box.borderBottom;
        sx = box.borderLeft + s.offsetX + s.spread;
        sy = box.borderTop + s.offsetY + s.spread;
        sw = pw - 2 * s.spread;
        sh = ph - 2 * s.spread;
        sr = std::max(0.0f, box.radius - maxBorder - s.spread);
    } else {
        sx = s.offsetX - s.spread - rect.x;
        sy = s.offsetY - s.spread - rect.y;
        sw = box.width + 2 * s.spread;
        sh = box.height + 2 * s.spread;
        sr = box.radius > 0 ? std::max(0.0f, box.radius + s.spread) : 0;
    }
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            float x = (i - margin + 0.5f) / scale, y = (j - margin + 0.5f) / scale;
            float c = coverage(x * scale, y * scale, sx * scale, sy * scale, sw * scale, sh * scale, sr * scale);
            mask[(size_t)j * w + i] = s.inset ? 1 - c : c;
        }
    }
    gaussian(mask, w, h, sigma);

    rgba.assign((size_t)pixelWidth * pixelHeight * 4, 0);
    bool any = false;
    for (int j = 0; j < pixelHeight; j++) {
        for (int i = 0; i < pixelWidth; i++) {
            float x = rect.x + (i + 0.5f) / scale, y = rect.y + (j + 0.5f) / scale;
            float a = mask[(size_t)(j + margin) * w + (i + margin)];
            if (s.inset) {
                // Only inside the padding box
                a *= coverage(x * scale, y * scale, box.borderLeft * scale, box.borderTop * scale,
                              (box.width - box.borderLeft - box.borderRight) * scale,
                              (box.height - box.borderTop - box.borderBottom) * scale,
                              std::max(0.0f, box.radius - maxBorder) * scale);
            } else {
                // Never under the box itself
                a *= 1 - coverage(x * scale, y * scale, 0, 0, box.width * scale, box.height * scale, box.radius * scale);
            }
            a *= s.a / 255.0f;
            if (a <= 0) continue;
            uint8_t* p = &rgba[((size_t)j * pixelWidth + i) * 4];
            p[0] = s.r;
            p[1] = s.g;
            p[2] = s.b;
            p[3] = (uint8_t)std::lround(std::clamp(a, 0.0f, 1.0f) * 255);
            any = true;
        }
    }
    return any;
}

} // namespace ZepraBrowser
