// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "canvas_context.h"
#include "canvas_raster.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <cstdlib>

namespace NXRender {

using Canvas2D::FlatPath;
using Canvas2D::PathBuilder;
using Canvas2D::ClipMask;

namespace {
constexpr float kPi = 3.14159265358979323846f;
// Largest bitmap a canvas gets (64 M pixels = 256 MB); a bigger one is 0x0
constexpr int64_t kMaxCanvasPixels = int64_t(8192) * 8192;
constexpr int kMaxCanvasSide = 32767;

bool allFinite(std::initializer_list<float> values) {
    for (float v : values) {
        if (!std::isfinite(v)) return false;
    }
    return true;
}
} // namespace

// ==================================================================
// CanvasMatrix
// ==================================================================

bool CanvasMatrix::isFinite() const { return allFinite({a, b, c, d, e, f}); }

bool CanvasMatrix::invertible() const {
    float det = determinant();
    return isFinite() && std::isfinite(det) && std::fabs(det) > 1e-12f;
}

CanvasMatrix CanvasMatrix::inverse() const {
    float det = determinant();
    if (std::fabs(det) <= 1e-12f) return CanvasMatrix{};
    float id = 1.0f / det;
    CanvasMatrix r;
    r.a = d * id;
    r.b = -b * id;
    r.c = -c * id;
    r.d = a * id;
    r.e = (c * f - d * e) * id;
    r.f = (b * e - a * f) * id;
    return r;
}

float CanvasMatrix::scaleFactor() const {
    return std::sqrt(std::fabs(determinant()));
}

// ==================================================================
// CanvasGradient
// ==================================================================

void CanvasGradient::addColorStop(float offset, const Color& color) {
    // Stops at the same offset keep the order they were added in
    Stop stop{std::clamp(offset, 0.0f, 1.0f), color};
    auto it = std::upper_bound(stops.begin(), stops.end(), stop,
                               [](const Stop& a, const Stop& b) { return a.offset < b.offset; });
    stops.insert(it, stop);
}

// ==================================================================
// ImageData
// ==================================================================

ImageData::ImageData(int w, int h) : width(std::max(0, w)), height(std::max(0, h)) {
    data.resize(static_cast<size_t>(width) * height * 4, 0);
}

uint8_t ImageData::getR(int x, int y) const {
    if (x < 0 || x >= width || y < 0 || y >= height) return 0;
    return data[(y * width + x) * 4];
}

uint8_t ImageData::getG(int x, int y) const {
    if (x < 0 || x >= width || y < 0 || y >= height) return 0;
    return data[(y * width + x) * 4 + 1];
}

uint8_t ImageData::getB(int x, int y) const {
    if (x < 0 || x >= width || y < 0 || y >= height) return 0;
    return data[(y * width + x) * 4 + 2];
}

uint8_t ImageData::getA(int x, int y) const {
    if (x < 0 || x >= width || y < 0 || y >= height) return 0;
    return data[(y * width + x) * 4 + 3];
}

void ImageData::setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || x >= width || y < 0 || y >= height) return;
    int i = (y * width + x) * 4;
    data[i] = r; data[i+1] = g; data[i+2] = b; data[i+3] = a;
}

// ==================================================================
// Path2D
// ==================================================================

Path2D::Path2D() {}

Path2D::Path2D(const std::string& svgPathData) {
    parseSVGPathData(svgPathData);
}

void Path2D::moveTo(float x, float y) {
    Command c; c.type = Command::MoveTo; c.args[0] = x; c.args[1] = y; c.argCount = 2;
    commands_.push_back(c);
}

void Path2D::lineTo(float x, float y) {
    Command c; c.type = Command::LineTo; c.args[0] = x; c.args[1] = y; c.argCount = 2;
    commands_.push_back(c);
}

void Path2D::quadraticCurveTo(float cpx, float cpy, float x, float y) {
    Command c; c.type = Command::QuadTo;
    c.args[0] = cpx; c.args[1] = cpy; c.args[2] = x; c.args[3] = y; c.argCount = 4;
    commands_.push_back(c);
}

void Path2D::bezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y) {
    Command c; c.type = Command::CubicTo;
    c.args[0] = cp1x; c.args[1] = cp1y; c.args[2] = cp2x; c.args[3] = cp2y;
    c.args[4] = x; c.args[5] = y; c.argCount = 6;
    commands_.push_back(c);
}

void Path2D::arcTo(float x1, float y1, float x2, float y2, float radius) {
    Command c; c.type = Command::ArcTo;
    c.args[0] = x1; c.args[1] = y1; c.args[2] = x2; c.args[3] = y2;
    c.args[4] = radius; c.argCount = 5;
    commands_.push_back(c);
}

void Path2D::arc(float x, float y, float radius, float startAngle, float endAngle, bool ccw) {
    Command c; c.type = Command::Arc;
    c.args[0] = x; c.args[1] = y; c.args[2] = radius;
    c.args[3] = startAngle; c.args[4] = endAngle;
    c.args[5] = ccw ? 1.0f : 0.0f; c.argCount = 6;
    commands_.push_back(c);
}

void Path2D::ellipse(float x, float y, float rx, float ry, float rotation,
                     float startAngle, float endAngle, bool ccw) {
    Command c; c.type = Command::Ellipse;
    c.args[0] = x; c.args[1] = y; c.args[2] = rx; c.args[3] = ry;
    c.args[4] = rotation; c.args[5] = startAngle;
    c.args[6] = endAngle; c.args[7] = ccw ? 1.0f : 0.0f; c.argCount = 8;
    commands_.push_back(c);
}

void Path2D::rect(float x, float y, float w, float h) {
    Command c; c.type = Command::Rect;
    c.args[0] = x; c.args[1] = y; c.args[2] = w; c.args[3] = h; c.argCount = 4;
    commands_.push_back(c);
}

void Path2D::roundRect(float x, float y, float w, float h,
                        float tl, float tr, float br, float bl) {
    // Decompose to arcs + lines
    moveTo(x + tl, y);
    lineTo(x + w - tr, y);
    if (tr > 0) arc(x + w - tr, y + tr, tr, -kPi / 2, 0);
    lineTo(x + w, y + h - br);
    if (br > 0) arc(x + w - br, y + h - br, br, 0, kPi / 2);
    lineTo(x + bl, y + h);
    if (bl > 0) arc(x + bl, y + h - bl, bl, kPi / 2, kPi);
    lineTo(x, y + tl);
    if (tl > 0) arc(x + tl, y + tl, tl, kPi, 3 * kPi / 2);
    closePath();
}

void Path2D::closePath() {
    Command c; c.type = Command::Close; c.argCount = 0;
    commands_.push_back(c);
}

void Path2D::addPath(const Path2D& other) {
    commands_.insert(commands_.end(), other.commands_.begin(), other.commands_.end());
}

void Path2D::parseSVGPathData(const std::string& d) {
    // Simplified SVG path parser (M, L, H, V, Z, C, S, Q, T, A)
    size_t pos = 0;
    float curX = 0, curY = 0;

    auto skipWS = [&]() {
        while (pos < d.size() && (std::isspace(d[pos]) || d[pos] == ',')) pos++;
    };

    auto readFloat = [&]() -> float {
        skipWS();
        size_t start = pos;
        if (pos < d.size() && (d[pos] == '-' || d[pos] == '+')) pos++;
        while (pos < d.size() && (std::isdigit(d[pos]) || d[pos] == '.')) pos++;
        if (pos == start) return 0;
        return std::strtof(d.c_str() + start, nullptr);
    };

    while (pos < d.size()) {
        skipWS();
        if (pos >= d.size()) break;

        char cmd = d[pos++];
        bool relative = std::islower(cmd);
        cmd = std::toupper(cmd);

        switch (cmd) {
            case 'M': {
                float x = readFloat(), y = readFloat();
                if (relative) { x += curX; y += curY; }
                moveTo(x, y); curX = x; curY = y;
                break;
            }
            case 'L': {
                float x = readFloat(), y = readFloat();
                if (relative) { x += curX; y += curY; }
                lineTo(x, y); curX = x; curY = y;
                break;
            }
            case 'H': {
                float x = readFloat();
                if (relative) x += curX;
                lineTo(x, curY); curX = x;
                break;
            }
            case 'V': {
                float y = readFloat();
                if (relative) y += curY;
                lineTo(curX, y); curY = y;
                break;
            }
            case 'C': {
                float x1 = readFloat(), y1 = readFloat();
                float x2 = readFloat(), y2 = readFloat();
                float x = readFloat(), y = readFloat();
                if (relative) {
                    x1 += curX; y1 += curY;
                    x2 += curX; y2 += curY;
                    x += curX; y += curY;
                }
                bezierCurveTo(x1, y1, x2, y2, x, y);
                curX = x; curY = y;
                break;
            }
            case 'Q': {
                float cpx = readFloat(), cpy = readFloat();
                float x = readFloat(), y = readFloat();
                if (relative) {
                    cpx += curX; cpy += curY;
                    x += curX; y += curY;
                }
                quadraticCurveTo(cpx, cpy, x, y);
                curX = x; curY = y;
                break;
            }
            case 'A': {
                float rx = readFloat(), ry = readFloat();
                float rotation = readFloat();
                float largeArc = readFloat(), sweep = readFloat();
                float x = readFloat(), y = readFloat();
                if (relative) { x += curX; y += curY; }
                // Simplified: approximate arc with line
                lineTo(x, y);
                curX = x; curY = y;
                (void)rx; (void)ry; (void)rotation;
                (void)largeArc; (void)sweep;
                break;
            }
            case 'Z': closePath(); break;
            default: break;
        }
    }
}

// ==================================================================
// CanvasRenderingContext2D
// ==================================================================

CanvasRenderingContext2D::CanvasRenderingContext2D(int width, int height)
    : width_(0), height_(0), currentPath_(std::make_unique<FlatPath>()) {
    resize(width, height);
}

CanvasRenderingContext2D::~CanvasRenderingContext2D() = default;

void CanvasRenderingContext2D::resize(int w, int h) {
    w = std::clamp(w, 0, kMaxCanvasSide);
    h = std::clamp(h, 0, kMaxCanvasSide);
    if (int64_t(w) * h > kMaxCanvasPixels) w = h = 0;
    width_ = w;
    height_ = h;
    pixels_.assign(static_cast<size_t>(w) * h * 4, 0);
    pixels_.shrink_to_fit();
    state_ = State();
    stateStack_.clear();
    *currentPath_ = FlatPath();
    changed();
}

void CanvasRenderingContext2D::copyUnpremultiplied(uint8_t* out) const {
    const size_t n = static_cast<size_t>(width_) * height_;
    const uint8_t* src = pixels_.data();
    for (size_t i = 0; i < n; i++, src += 4, out += 4) {
        uint8_t a = src[3];
        if (a == 0) {
            out[0] = out[1] = out[2] = out[3] = 0;
        } else if (a == 255) {
            std::memcpy(out, src, 4);
        } else {
            for (int k = 0; k < 3; k++) out[k] = static_cast<uint8_t>(std::min(255, (src[k] * 255 + a / 2) / a));
            out[3] = a;
        }
    }
}

// ==================================================================
// State management
// ==================================================================

void CanvasRenderingContext2D::save() {
    stateStack_.push_back(state_);
}

void CanvasRenderingContext2D::restore() {
    if (!stateStack_.empty()) {
        state_ = std::move(stateStack_.back());
        stateStack_.pop_back();
    }
}

void CanvasRenderingContext2D::reset() {
    resize(width_, height_);
}

// ==================================================================
// Transform operations
// ==================================================================

void CanvasRenderingContext2D::scale(float x, float y) {
    if (!allFinite({x, y})) return;
    state_.transform = state_.transform * CanvasMatrix{x, 0, 0, y, 0, 0};
}

void CanvasRenderingContext2D::rotate(float angle) {
    if (!std::isfinite(angle)) return;
    float c = std::cos(angle), s = std::sin(angle);
    state_.transform = state_.transform * CanvasMatrix{c, s, -s, c, 0, 0};
}

void CanvasRenderingContext2D::translate(float x, float y) {
    if (!allFinite({x, y})) return;
    state_.transform = state_.transform * CanvasMatrix{1, 0, 0, 1, x, y};
}

void CanvasRenderingContext2D::transform(float a, float b, float c, float d, float e, float f) {
    if (!allFinite({a, b, c, d, e, f})) return;
    state_.transform = state_.transform * CanvasMatrix{a, b, c, d, e, f};
}

void CanvasRenderingContext2D::setTransform(float a, float b, float c, float d, float e, float f) {
    if (!allFinite({a, b, c, d, e, f})) return;
    state_.transform = CanvasMatrix{a, b, c, d, e, f};
}

void CanvasRenderingContext2D::resetTransform() {
    state_.transform = CanvasMatrix{};
}

Math::Transform2D CanvasRenderingContext2D::getTransform() const {
    const CanvasMatrix& m = state_.transform;
    return Math::Transform2D::fromCSS(m.a, m.b, m.c, m.d, m.e, m.f);
}

// ==================================================================
// Compositing and styles
// ==================================================================

void CanvasRenderingContext2D::setGlobalAlpha(float alpha) {
    // Out-of-range values are ignored, as the spec says
    if (std::isfinite(alpha) && alpha >= 0.0f && alpha <= 1.0f) state_.globalAlpha = alpha;
}

float CanvasRenderingContext2D::globalAlpha() const { return state_.globalAlpha; }
void CanvasRenderingContext2D::setGlobalCompositeOperation(CompositeOp op) { state_.compositeOp = op; }
CompositeOp CanvasRenderingContext2D::globalCompositeOperation() const { return state_.compositeOp; }

void CanvasRenderingContext2D::setFillStyle(const Color& color) {
    state_.fillStyle.type = CanvasFillStyle::Type::Color;
    state_.fillStyle.color = color;
}

void CanvasRenderingContext2D::setFillStyle(const CanvasGradient& gradient) {
    state_.fillStyle.type = CanvasFillStyle::Type::Gradient;
    state_.fillStyle.gradient = gradient;
}

void CanvasRenderingContext2D::setFillStyle(const CanvasPattern& pattern) {
    state_.fillStyle.type = CanvasFillStyle::Type::Pattern;
    state_.fillStyle.pattern = pattern;
}

void CanvasRenderingContext2D::setStrokeStyle(const Color& color) {
    state_.strokeStyle.type = CanvasFillStyle::Type::Color;
    state_.strokeStyle.color = color;
}

void CanvasRenderingContext2D::setStrokeStyle(const CanvasGradient& gradient) {
    state_.strokeStyle.type = CanvasFillStyle::Type::Gradient;
    state_.strokeStyle.gradient = gradient;
}

void CanvasRenderingContext2D::setStrokeStyle(const CanvasPattern& pattern) {
    state_.strokeStyle.type = CanvasFillStyle::Type::Pattern;
    state_.strokeStyle.pattern = pattern;
}

void CanvasRenderingContext2D::setLineWidth(float width) {
    if (std::isfinite(width) && width > 0) state_.lineWidth = width;
}
float CanvasRenderingContext2D::lineWidth() const { return state_.lineWidth; }
void CanvasRenderingContext2D::setLineCap(LineCap cap) { state_.lineCap = cap; }
void CanvasRenderingContext2D::setLineJoin(LineJoin join) { state_.lineJoin = join; }
void CanvasRenderingContext2D::setMiterLimit(float limit) {
    if (std::isfinite(limit) && limit > 0) state_.miterLimit = limit;
}

void CanvasRenderingContext2D::setLineDash(const std::vector<float>& segments) {
    for (float s : segments) {
        if (!std::isfinite(s) || s < 0) return;
    }
    state_.lineDash = segments;
    if (state_.lineDash.size() % 2 == 1) {
        state_.lineDash.insert(state_.lineDash.end(), segments.begin(), segments.end());
    }
}

std::vector<float> CanvasRenderingContext2D::getLineDash() const { return state_.lineDash; }

void CanvasRenderingContext2D::setLineDashOffset(float offset) {
    if (std::isfinite(offset)) state_.lineDashOffset = offset;
}

void CanvasRenderingContext2D::setShadowColor(const Color& color) { state_.shadowColor = color; }
void CanvasRenderingContext2D::setShadowBlur(float blur) {
    if (std::isfinite(blur) && blur >= 0) state_.shadowBlur = blur;
}
void CanvasRenderingContext2D::setShadowOffsetX(float x) { if (std::isfinite(x)) state_.shadowOffsetX = x; }
void CanvasRenderingContext2D::setShadowOffsetY(float y) { if (std::isfinite(y)) state_.shadowOffsetY = y; }

void CanvasRenderingContext2D::setTextAlign(TextAlign2D align) { state_.textAlign = align; }
void CanvasRenderingContext2D::setTextBaseline(TextBaseline baseline) { state_.textBaseline = baseline; }
void CanvasRenderingContext2D::setDirection(const std::string& dir) { state_.direction = dir; }
void CanvasRenderingContext2D::setFilter(const std::string& filter) { state_.filter = filter; }

// ==================================================================
// Path API
// ==================================================================

void CanvasRenderingContext2D::beginPath() { *currentPath_ = FlatPath(); }
void CanvasRenderingContext2D::closePath() { currentPath_->close(); }

void CanvasRenderingContext2D::moveTo(float x, float y) {
    PathBuilder(*currentPath_).moveTo(state_.transform, x, y);
}

void CanvasRenderingContext2D::lineTo(float x, float y) {
    PathBuilder(*currentPath_).lineTo(state_.transform, x, y);
}

void CanvasRenderingContext2D::quadraticCurveTo(float cpx, float cpy, float x, float y) {
    PathBuilder(*currentPath_).quadTo(state_.transform, cpx, cpy, x, y);
}

void CanvasRenderingContext2D::bezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y,
                                             float x, float y) {
    PathBuilder(*currentPath_).cubicTo(state_.transform, cp1x, cp1y, cp2x, cp2y, x, y);
}

void CanvasRenderingContext2D::arcTo(float x1, float y1, float x2, float y2, float radius) {
    PathBuilder(*currentPath_).arcTo(state_.transform, x1, y1, x2, y2, radius);
}

void CanvasRenderingContext2D::arc(float x, float y, float radius,
                                   float startAngle, float endAngle, bool ccw) {
    PathBuilder(*currentPath_).ellipse(state_.transform, x, y, radius, radius, 0,
                                       startAngle, endAngle, ccw);
}

void CanvasRenderingContext2D::ellipse(float x, float y, float rx, float ry,
                                       float rotation, float startAngle, float endAngle,
                                       bool ccw) {
    PathBuilder(*currentPath_).ellipse(state_.transform, x, y, rx, ry, rotation,
                                       startAngle, endAngle, ccw);
}

void CanvasRenderingContext2D::rect(float x, float y, float w, float h) {
    PathBuilder(*currentPath_).rect(state_.transform, x, y, w, h);
}

void CanvasRenderingContext2D::roundRect(float x, float y, float w, float h,
                                         float tl, float tr, float br, float bl) {
    if (tr < 0) tr = tl;
    if (br < 0) br = tl;
    if (bl < 0) bl = tr;
    PathBuilder(*currentPath_).roundRect(state_.transform, x, y, w, h, tl, tr, br, bl);
}

// ==================================================================
// Painting
// ==================================================================

template <typename Spans>
void CanvasRenderingContext2D::paintCoverage(Spans&& spans, const CanvasFillStyle& style) {
    if (width_ == 0 || height_ == 0 || !state_.transform.invertible()) return;
    const CompositeOp op = state_.compositeOp;
    const bool unbounded = Canvas2D::compositeIsUnbounded(op);
    Canvas2D::Paint paint(style, state_.transform.inverse(), state_.globalAlpha);
    if (paint.paintsNothing() && !unbounded) return;
    const ClipMask* clip = state_.clip.get();
    float src[4] = {0, 0, 0, 0};

    if (!unbounded) {
        spans([&](int y, int x0, int x1, const float* cov) {
            uint8_t* row = pixels_.data() + static_cast<size_t>(y) * width_ * 4;
            const uint8_t* clipRow = clip ? clip->coverage.data() + static_cast<size_t>(y) * width_ : nullptr;
            for (int x = x0; x < x1; x++) {
                float c = cov[x - x0];
                if (clipRow) c *= clipRow[x] / 255.0f;
                if (c <= 0) continue;
                paint.shade(x, y, src);
                Canvas2D::compositePixel(row + x * 4, src, c, op);
            }
        });
    } else {
        // Pixels outside the shape are composited with transparent black too
        std::vector<float> coverage(static_cast<size_t>(width_) * height_, 0.0f);
        spans([&](int y, int x0, int x1, const float* cov) {
            std::copy(cov, cov + (x1 - x0), coverage.begin() + static_cast<size_t>(y) * width_ + x0);
        });
        for (int y = 0; y < height_; y++) {
            for (int x = 0; x < width_; x++) {
                size_t i = static_cast<size_t>(y) * width_ + x;
                float clipCov = clip ? clip->coverage[i] / 255.0f : 1.0f;
                if (clipCov <= 0) continue;
                uint8_t* px = pixels_.data() + i * 4;
                uint8_t before[4] = {px[0], px[1], px[2], px[3]};
                float c = coverage[i];
                if (c > 0 && !paint.paintsNothing()) {
                    paint.shade(x, y, src);
                } else {
                    src[0] = src[1] = src[2] = src[3] = 0;
                }
                Canvas2D::compositePixel(px, src, c, op);
                if (clipCov < 1.0f) {
                    for (int k = 0; k < 4; k++) {
                        px[k] = static_cast<uint8_t>(before[k] + (px[k] - before[k]) * clipCov + 0.5f);
                    }
                }
            }
        }
    }
    changed();
}

void CanvasRenderingContext2D::paintPath(const FlatPath& path, FillRule rule,
                                         const CanvasFillStyle& style) {
    paintCoverage([&](auto&& fn) { Canvas2D::rasterize(path, rule, width_, height_, fn); }, style);
}

bool CanvasRenderingContext2D::deviceRect(float x, float y, float w, float h, float out[4]) const {
    const CanvasMatrix& m = state_.transform;
    if (m.b != 0 || m.c != 0 || !m.isFinite()) return false;
    m.map(x, y, out[0], out[1]);
    m.map(x + w, y + h, out[2], out[3]);
    return true;
}

void CanvasRenderingContext2D::strokePath(const FlatPath& path) {
    const CanvasMatrix& m = state_.transform;
    if (!m.invertible()) return;
    // The stroke is computed in user space, then transformed
    FlatPath user = path.transformed(m.inverse());
    FlatPath outline = Canvas2D::strokeOutline(user, state_.lineWidth, state_.lineCap, state_.lineJoin,
                                               state_.miterLimit, state_.lineDash, state_.lineDashOffset,
                                               0.025f / std::max(m.scaleFactor(), 1e-6f));
    paintPath(outline.transformed(m), FillRule::NonZero, state_.strokeStyle);
}

void CanvasRenderingContext2D::fill(FillRule rule) {
    paintPath(*currentPath_, rule, state_.fillStyle);
}

void CanvasRenderingContext2D::fill(const Path2D& path, FillRule rule) {
    FlatPath flat;
    PathBuilder(flat).addPath2D(state_.transform, path);
    paintPath(flat, rule, state_.fillStyle);
}

void CanvasRenderingContext2D::stroke() {
    strokePath(*currentPath_);
}

void CanvasRenderingContext2D::stroke(const Path2D& path) {
    FlatPath flat;
    PathBuilder(flat).addPath2D(state_.transform, path);
    strokePath(flat);
}

void CanvasRenderingContext2D::clip(FillRule rule) {
    auto mask = std::make_shared<ClipMask>();
    mask->width = width_;
    mask->height = height_;
    mask->coverage.assign(static_cast<size_t>(width_) * height_, 0);
    const ClipMask* previous = state_.clip.get();
    Canvas2D::rasterize(*currentPath_, rule, width_, height_, [&](int y, int x0, int x1, const float* cov) {
        for (int x = x0; x < x1; x++) {
            size_t i = static_cast<size_t>(y) * width_ + x;
            float c = cov[x - x0];
            if (previous) c *= previous->coverage[i] / 255.0f;
            mask->coverage[i] = static_cast<uint8_t>(c * 255.0f + 0.5f);
        }
    });
    state_.clip = std::move(mask);
}

void CanvasRenderingContext2D::clip(const Path2D& path, FillRule rule) {
    FlatPath saved = *currentPath_;
    *currentPath_ = FlatPath();
    PathBuilder(*currentPath_).addPath2D(state_.transform, path);
    clip(rule);
    *currentPath_ = std::move(saved);
}

namespace {
// Winding number of the path around (x, y)
int windingAt(const FlatPath& path, float x, float y) {
    int winding = 0;
    for (const auto& sp : path.subpaths) {
        size_t n = sp.pts.size();
        if (n < 2) continue;
        for (size_t i = 0; i < n; i++) {
            Canvas2D::Pt a = sp.pts[i], b = sp.pts[(i + 1) % n];
            if ((a.y <= y) == (b.y <= y)) continue;
            float cx = a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y);
            if (cx > x) winding += a.y < b.y ? 1 : -1;
        }
    }
    return winding;
}
} // namespace

bool CanvasRenderingContext2D::isPointInPath(float x, float y, FillRule rule) {
    if (!allFinite({x, y})) return false;
    int w = windingAt(*currentPath_, x, y);
    return rule == FillRule::NonZero ? w != 0 : (w & 1) != 0;
}

bool CanvasRenderingContext2D::isPointInStroke(float x, float y) {
    const CanvasMatrix& m = state_.transform;
    if (!allFinite({x, y}) || !m.invertible()) return false;
    FlatPath outline = Canvas2D::strokeOutline(currentPath_->transformed(m.inverse()), state_.lineWidth,
                                               state_.lineCap, state_.lineJoin, state_.miterLimit,
                                               state_.lineDash, state_.lineDashOffset, 0.1f);
    return windingAt(outline.transformed(m), x, y) != 0;
}

// ==================================================================
// Rectangles
// ==================================================================

void CanvasRenderingContext2D::clearRect(float x, float y, float w, float h) {
    if (!allFinite({x, y, w, h}) || width_ == 0 || height_ == 0) return;
    const ClipMask* clip = state_.clip.get();
    auto clear = [&](int py, int x0, int x1, const float* cov) {
        uint8_t* row = pixels_.data() + static_cast<size_t>(py) * width_ * 4;
        for (int px = x0; px < x1; px++) {
            float c = cov[px - x0];
            if (clip) c *= clip->coverage[static_cast<size_t>(py) * width_ + px] / 255.0f;
            if (c <= 0) continue;
            uint8_t* p = row + px * 4;
            if (c >= 0.999f) {
                p[0] = p[1] = p[2] = p[3] = 0;
            } else {
                for (int k = 0; k < 4; k++) p[k] = static_cast<uint8_t>(p[k] * (1 - c) + 0.5f);
            }
        }
    };
    float r[4];
    if (deviceRect(x, y, w, h, r)) {
        Canvas2D::rectSpans(r[0], r[1], r[2], r[3], width_, height_, clear);
    } else {
        FlatPath path;
        PathBuilder(path).rect(state_.transform, x, y, w, h);
        Canvas2D::rasterize(path, FillRule::NonZero, width_, height_, clear);
    }
    changed();
}

void CanvasRenderingContext2D::fillRect(float x, float y, float w, float h) {
    if (!allFinite({x, y, w, h}) || w == 0 || h == 0) return;
    float r[4];
    if (deviceRect(x, y, w, h, r)) {
        paintCoverage([&](auto&& fn) { Canvas2D::rectSpans(r[0], r[1], r[2], r[3], width_, height_, fn); },
                      state_.fillStyle);
        return;
    }
    FlatPath path;
    PathBuilder(path).rect(state_.transform, x, y, w, h);
    paintPath(path, FillRule::NonZero, state_.fillStyle);
}

void CanvasRenderingContext2D::strokeRect(float x, float y, float w, float h) {
    if (!allFinite({x, y, w, h})) return;
    FlatPath path;
    PathBuilder builder(path);
    if (w == 0 && h == 0) return;
    if (w == 0 || h == 0) {
        // A zero-width rectangle strokes as a line
        builder.moveTo(state_.transform, x, y);
        builder.lineTo(state_.transform, x + w, y + h);
    } else {
        builder.rect(state_.transform, x, y, w, h);
    }
    strokePath(path);
}

// ==================================================================
// Images
// ==================================================================

void CanvasRenderingContext2D::drawImage(const CanvasImage& image, float dx, float dy) {
    drawImage(image, 0, 0, static_cast<float>(image.width), static_cast<float>(image.height),
              dx, dy, static_cast<float>(image.width), static_cast<float>(image.height));
}

void CanvasRenderingContext2D::drawImage(const CanvasImage& image, float dx, float dy,
                                         float dw, float dh) {
    drawImage(image, 0, 0, static_cast<float>(image.width), static_cast<float>(image.height),
              dx, dy, dw, dh);
}

void CanvasRenderingContext2D::drawImage(const CanvasImage& image,
                                         float sx, float sy, float sw, float sh,
                                         float dx, float dy, float dw, float dh) {
    if (!allFinite({sx, sy, sw, sh, dx, dy, dw, dh})) return;
    if (image.width <= 0 || image.height <= 0 || sw == 0 || sh == 0 || dw == 0 || dh == 0) return;
    if (sw < 0) { sx += sw; sw = -sw; }
    if (sh < 0) { sy += sh; sh = -sh; }
    if (dw < 0) { dx += dw; dw = -dw; }
    if (dh < 0) { dy += dh; dh = -dh; }
    // The source rectangle is clipped to the image, and the destination with it
    float cx0 = std::max(sx, 0.0f), cy0 = std::max(sy, 0.0f);
    float cx1 = std::min(sx + sw, static_cast<float>(image.width));
    float cy1 = std::min(sy + sh, static_cast<float>(image.height));
    if (cx1 <= cx0 || cy1 <= cy0) return;
    float kx = dw / sw, ky = dh / sh;
    dx += (cx0 - sx) * kx; dy += (cy0 - sy) * ky;
    dw = (cx1 - cx0) * kx; dh = (cy1 - cy0) * ky;
    sx = cx0; sy = cy0; sw = cx1 - cx0; sh = cy1 - cy0;

    CanvasFillStyle style;
    style.type = CanvasFillStyle::Type::Pattern;
    style.pattern.image = std::shared_ptr<const CanvasImage>(&image, [](const CanvasImage*) {});
    style.pattern.repeat = RepeatMode::NoRepeat;
    // user space -> image pixels
    style.pattern.transform = CanvasMatrix{sw / dw, 0, 0, sh / dh, sx - dx * sw / dw, sy - dy * sh / dh};
    FlatPath path;
    PathBuilder(path).rect(state_.transform, dx, dy, dw, dh);
    paintPath(path, FillRule::NonZero, style);
}

std::shared_ptr<CanvasImage> CanvasRenderingContext2D::snapshot() const {
    auto image = std::make_shared<CanvasImage>();
    image->width = width_;
    image->height = height_;
    image->pixels = pixels_;
    return image;
}

// ==================================================================
// Pixel manipulation
// ==================================================================

ImageData CanvasRenderingContext2D::createImageData(int width, int height) {
    return ImageData(std::abs(width), std::abs(height));
}

ImageData CanvasRenderingContext2D::getImageData(int sx, int sy, int sw, int sh) {
    if (sw < 0) { sx += sw; sw = -sw; }
    if (sh < 0) { sy += sh; sh = -sh; }
    ImageData data(sw, sh);
    for (int y = 0; y < sh; y++) {
        int cy = sy + y;
        if (cy < 0 || cy >= height_) continue;
        for (int x = 0; x < sw; x++) {
            int cx = sx + x;
            if (cx < 0 || cx >= width_) continue;
            const uint8_t* p = pixels_.data() + (static_cast<size_t>(cy) * width_ + cx) * 4;
            uint8_t* o = data.data.data() + (static_cast<size_t>(y) * sw + x) * 4;
            uint8_t a = p[3];
            if (a == 0) continue;
            for (int k = 0; k < 3; k++) o[k] = static_cast<uint8_t>(std::min(255, (p[k] * 255 + a / 2) / a));
            o[3] = a;
        }
    }
    return data;
}

void CanvasRenderingContext2D::putImageData(const ImageData& data, int dx, int dy) {
    putImageData(data, dx, dy, 0, 0, data.width, data.height);
}

void CanvasRenderingContext2D::putImageData(const ImageData& data, int dx, int dy,
                                            int dirtyX, int dirtyY, int dirtyW, int dirtyH) {
    if (dirtyW < 0) { dirtyX += dirtyW; dirtyW = -dirtyW; }
    if (dirtyH < 0) { dirtyY += dirtyH; dirtyH = -dirtyH; }
    int x0 = std::max(dirtyX, 0), y0 = std::max(dirtyY, 0);
    int x1 = std::min(dirtyX + dirtyW, data.width), y1 = std::min(dirtyY + dirtyH, data.height);
    // Written as is: no transform, alpha, compositing or clip
    for (int y = y0; y < y1; y++) {
        int cy = dy + y;
        if (cy < 0 || cy >= height_) continue;
        for (int x = x0; x < x1; x++) {
            int cx = dx + x;
            if (cx < 0 || cx >= width_) continue;
            const uint8_t* s = data.data.data() + (static_cast<size_t>(y) * data.width + x) * 4;
            uint8_t* p = pixels_.data() + (static_cast<size_t>(cy) * width_ + cx) * 4;
            uint8_t a = s[3];
            for (int k = 0; k < 3; k++) p[k] = static_cast<uint8_t>((s[k] * a + 127) / 255);
            p[3] = a;
        }
    }
    changed();
}

// ==================================================================
// Gradient/pattern creation
// ==================================================================

CanvasGradient CanvasRenderingContext2D::createLinearGradient(float x0, float y0, float x1, float y1) {
    CanvasGradient g;
    g.type = CanvasGradient::Type::Linear;
    g.x0 = x0; g.y0 = y0; g.x1 = x1; g.y1 = y1;
    return g;
}

CanvasGradient CanvasRenderingContext2D::createRadialGradient(float x0, float y0, float r0,
                                                              float x1, float y1, float r1) {
    CanvasGradient g;
    g.type = CanvasGradient::Type::Radial;
    g.x0 = x0; g.y0 = y0; g.r0 = r0;
    g.x1 = x1; g.y1 = y1; g.r1 = r1;
    return g;
}

CanvasGradient CanvasRenderingContext2D::createConicGradient(float startAngle, float x, float y) {
    CanvasGradient g;
    g.type = CanvasGradient::Type::Conic;
    g.x0 = x; g.y0 = y; g.angle = startAngle;
    return g;
}

CanvasPattern CanvasRenderingContext2D::createPattern(std::shared_ptr<const CanvasImage> image,
                                                      RepeatMode repeat) {
    CanvasPattern p;
    p.image = std::move(image);
    p.repeat = repeat;
    return p;
}

} // namespace NXRender
