// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "canvas_context.h"
#include "nxgfx/gl_includes.h"
#ifdef __linux__
#include <GL/glx.h>
#endif
#include <cmath>
#include <algorithm>
#include <cstring>
#include <sstream>

namespace NXRender {

// GL 2.0+ FBO function pointers
typedef void   (*PFN_glGenFramebuffers)(GLsizei, GLuint*);
typedef void   (*PFN_glDeleteFramebuffers)(GLsizei, const GLuint*);
typedef void   (*PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void   (*PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);

static PFN_glGenFramebuffers      nx_canvas_glGenFramebuffers = nullptr;
static PFN_glDeleteFramebuffers   nx_canvas_glDeleteFramebuffers = nullptr;
static PFN_glBindFramebuffer      nx_canvas_glBindFramebuffer = nullptr;
static PFN_glFramebufferTexture2D nx_canvas_glFramebufferTexture2D = nullptr;
static bool s_canvasFBOLoaded = false;

static void loadCanvasFBOFunctions() {
    if (s_canvasFBOLoaded) return;
    s_canvasFBOLoaded = true;
#ifdef __linux__
    auto load = [](const char* name) {
        return reinterpret_cast<void*>(glXGetProcAddress(reinterpret_cast<const GLubyte*>(name)));
    };
#elif _WIN32
    auto load = [](const char* name) { return reinterpret_cast<void*>(wglGetProcAddress(name)); };
#elif defined(__APPLE__)
    auto load = [](const char* name) { return nxAppleGetProcAddress(name); };
#else
    auto load = [](const char*) -> void* { return nullptr; };
#endif
    nx_canvas_glGenFramebuffers = reinterpret_cast<PFN_glGenFramebuffers>(load("glGenFramebuffers"));
    nx_canvas_glDeleteFramebuffers = reinterpret_cast<PFN_glDeleteFramebuffers>(load("glDeleteFramebuffers"));
    nx_canvas_glBindFramebuffer = reinterpret_cast<PFN_glBindFramebuffer>(load("glBindFramebuffer"));
    nx_canvas_glFramebufferTexture2D = reinterpret_cast<PFN_glFramebufferTexture2D>(load("glFramebufferTexture2D"));
}

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH 0x8B84
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif

// ==================================================================
// CanvasGradient
// ==================================================================

void CanvasGradient::addColorStop(float offset, const Color& color) {
    stops.push_back({std::clamp(offset, 0.0f, 1.0f), color});
    std::sort(stops.begin(), stops.end(),
              [](const Stop& a, const Stop& b) { return a.offset < b.offset; });
}

// ==================================================================
// ImageData
// ==================================================================

ImageData::ImageData(int w, int h) : width(w), height(h) {
    data.resize(w * h * 4, 0);
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
    if (tr > 0) arc(x + w - tr, y + tr, tr, -M_PI / 2, 0);
    lineTo(x + w, y + h - br);
    if (br > 0) arc(x + w - br, y + h - br, br, 0, M_PI / 2);
    lineTo(x + bl, y + h);
    if (bl > 0) arc(x + bl, y + h - bl, bl, M_PI / 2, M_PI);
    lineTo(x, y + tl);
    if (tl > 0) arc(x + tl, y + tl, tl, M_PI, 3 * M_PI / 2);
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
    : width_(width), height_(height) {
    loadCanvasFBOFunctions();
    state_.fillStyle.color = Color::black();
    state_.strokeStyle.color = Color::black();
    setupFBO();
}

CanvasRenderingContext2D::~CanvasRenderingContext2D() {
    teardownFBO();
}

void CanvasRenderingContext2D::resize(int w, int h) {
    teardownFBO();
    width_ = w;
    height_ = h;
    setupFBO();
}

void CanvasRenderingContext2D::setupFBO() {
    nx_canvas_glGenFramebuffers(1, &fbo_);
    nx_canvas_glBindFramebuffer(GL_FRAMEBUFFER, fbo_);

    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width_, height_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    nx_canvas_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, texture_, 0);
    nx_canvas_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void CanvasRenderingContext2D::teardownFBO() {
    if (texture_) { glDeleteTextures(1, &texture_); texture_ = 0; }
    if (fbo_) { nx_canvas_glDeleteFramebuffers(1, &fbo_); fbo_ = 0; }
}

// ==================================================================
// State management
// ==================================================================

void CanvasRenderingContext2D::save() {
    stateStack_.push(state_);
}

void CanvasRenderingContext2D::restore() {
    if (!stateStack_.empty()) {
        state_ = stateStack_.top();
        stateStack_.pop();
    }
}

void CanvasRenderingContext2D::reset() {
    state_ = State();
    state_.fillStyle.color = Color::black();
    state_.strokeStyle.color = Color::black();
    while (!stateStack_.empty()) stateStack_.pop();
    currentPath_ = Path2D();
}

// ==================================================================
// Transform operations
// ==================================================================

void CanvasRenderingContext2D::scale(float x, float y) {
    state_.transform.scaleSelf(x, y);
}

void CanvasRenderingContext2D::rotate(float angle) {
    state_.transform.rotate(angle);
}

void CanvasRenderingContext2D::translate(float x, float y) {
    state_.transform.translate(x, y);
}

void CanvasRenderingContext2D::transform(float a, float b, float c, float d, float e, float f) {
    Math::Transform2D t = Math::Transform2D::fromCSS(a, b, c, d, e, f);
    state_.transform = state_.transform * t;
}

void CanvasRenderingContext2D::setTransform(float a, float b, float c, float d, float e, float f) {
    state_.transform = Math::Transform2D::fromCSS(a, b, c, d, e, f);
}

void CanvasRenderingContext2D::resetTransform() {
    state_.transform = Math::Transform2D::identity();
}

Math::Transform2D CanvasRenderingContext2D::getTransform() const {
    return state_.transform;
}

// ==================================================================
// Compositing
// ==================================================================

void CanvasRenderingContext2D::setGlobalAlpha(float alpha) {
    state_.globalAlpha = std::clamp(alpha, 0.0f, 1.0f);
}

float CanvasRenderingContext2D::globalAlpha() const {
    return state_.globalAlpha;
}

void CanvasRenderingContext2D::setGlobalCompositeOperation(CompositeOp op) {
    state_.compositeOp = op;
}

CompositeOp CanvasRenderingContext2D::globalCompositeOperation() const {
    return state_.compositeOp;
}

// ==================================================================
// Fill/stroke style
// ==================================================================

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

// ==================================================================
// Line style
// ==================================================================

void CanvasRenderingContext2D::setLineWidth(float width) { state_.lineWidth = std::max(0.0f, width); }
float CanvasRenderingContext2D::lineWidth() const { return state_.lineWidth; }
void CanvasRenderingContext2D::setLineCap(LineCap cap) { state_.lineCap = cap; }
void CanvasRenderingContext2D::setLineJoin(LineJoin join) { state_.lineJoin = join; }
void CanvasRenderingContext2D::setMiterLimit(float limit) { state_.miterLimit = limit; }

void CanvasRenderingContext2D::setLineDash(const std::vector<float>& segments) {
    state_.lineDash = segments;
}

std::vector<float> CanvasRenderingContext2D::getLineDash() const {
    return state_.lineDash;
}

void CanvasRenderingContext2D::setLineDashOffset(float offset) {
    state_.lineDashOffset = offset;
}

// ==================================================================
// Shadow
// ==================================================================

void CanvasRenderingContext2D::setShadowColor(const Color& color) { state_.shadowColor = color; }
void CanvasRenderingContext2D::setShadowBlur(float blur) { state_.shadowBlur = std::max(0.0f, blur); }
void CanvasRenderingContext2D::setShadowOffsetX(float x) { state_.shadowOffsetX = x; }
void CanvasRenderingContext2D::setShadowOffsetY(float y) { state_.shadowOffsetY = y; }

// ==================================================================
// Text
// ==================================================================

void CanvasRenderingContext2D::setFont(const std::string& font) { state_.font = font; }
void CanvasRenderingContext2D::setTextAlign(TextAlign2D align) { state_.textAlign = align; }
void CanvasRenderingContext2D::setTextBaseline(TextBaseline baseline) { state_.textBaseline = baseline; }
void CanvasRenderingContext2D::setDirection(const std::string& dir) { state_.direction = dir; }
void CanvasRenderingContext2D::setFilter(const std::string& filter) { state_.filter = filter; }

// ==================================================================
// Path API delegation
// ==================================================================

void CanvasRenderingContext2D::beginPath() { currentPath_ = Path2D(); }
void CanvasRenderingContext2D::closePath() { currentPath_.closePath(); }
void CanvasRenderingContext2D::moveTo(float x, float y) { currentPath_.moveTo(x, y); }
void CanvasRenderingContext2D::lineTo(float x, float y) { currentPath_.lineTo(x, y); }

void CanvasRenderingContext2D::quadraticCurveTo(float cpx, float cpy, float x, float y) {
    currentPath_.quadraticCurveTo(cpx, cpy, x, y);
}

void CanvasRenderingContext2D::bezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y,
                                               float x, float y) {
    currentPath_.bezierCurveTo(cp1x, cp1y, cp2x, cp2y, x, y);
}

void CanvasRenderingContext2D::arcTo(float x1, float y1, float x2, float y2, float radius) {
    currentPath_.arcTo(x1, y1, x2, y2, radius);
}

void CanvasRenderingContext2D::arc(float x, float y, float radius,
                                    float startAngle, float endAngle, bool ccw) {
    currentPath_.arc(x, y, radius, startAngle, endAngle, ccw);
}

void CanvasRenderingContext2D::ellipse(float x, float y, float rx, float ry,
                                        float rotation, float startAngle, float endAngle,
                                        bool ccw) {
    currentPath_.ellipse(x, y, rx, ry, rotation, startAngle, endAngle, ccw);
}

void CanvasRenderingContext2D::rect(float x, float y, float w, float h) {
    currentPath_.rect(x, y, w, h);
}

void CanvasRenderingContext2D::roundRect(float x, float y, float w, float h,
                                          float tl, float tr, float br, float bl) {
    if (tr < 0) tr = tl;
    if (br < 0) br = tl;
    if (bl < 0) bl = tl;
    currentPath_.roundRect(x, y, w, h, tl, tr, br, bl);
}

// ==================================================================
// Compositing setup
// ==================================================================

void CanvasRenderingContext2D::applyCompositing() {
    glEnable(GL_BLEND);

    switch (state_.compositeOp) {
        case CompositeOp::SourceOver:
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
        case CompositeOp::SourceIn:
            glBlendFunc(GL_DST_ALPHA, GL_ZERO); break;
        case CompositeOp::SourceOut:
            glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_ZERO); break;
        case CompositeOp::SourceAtop:
            glBlendFunc(GL_DST_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
        case CompositeOp::DestinationOver:
            glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_ONE); break;
        case CompositeOp::DestinationIn:
            glBlendFunc(GL_ZERO, GL_SRC_ALPHA); break;
        case CompositeOp::DestinationOut:
            glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA); break;
        case CompositeOp::DestinationAtop:
            glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA); break;
        case CompositeOp::Lighter:
            glBlendFunc(GL_ONE, GL_ONE); break;
        case CompositeOp::Copy:
            glBlendFunc(GL_ONE, GL_ZERO); break;
        case CompositeOp::Xor:
            glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
        default:
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
    }
}

// ==================================================================
// Drawing: rectangles
// ==================================================================

void CanvasRenderingContext2D::clearRect(float x, float y, float w, float h) {
    nx_canvas_glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glEnable(GL_SCISSOR_TEST);
    glScissor(static_cast<int>(x), height_ - static_cast<int>(y + h),
              static_cast<int>(w), static_cast<int>(h));
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    nx_canvas_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void CanvasRenderingContext2D::fillRect(float x, float y, float w, float h) {
    nx_canvas_glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width_, height_, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    applyCompositing();

    const Color& c = state_.fillStyle.color;
    float a = state_.globalAlpha;
    glColor4f(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (c.a / 255.0f) * a);

    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();

    nx_canvas_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void CanvasRenderingContext2D::strokeRect(float x, float y, float w, float h) {
    nx_canvas_glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width_, height_, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    applyCompositing();

    const Color& c = state_.strokeStyle.color;
    float a = state_.globalAlpha;
    glColor4f(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (c.a / 255.0f) * a);
    glLineWidth(state_.lineWidth);

    glBegin(GL_LINE_LOOP);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();

    nx_canvas_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ==================================================================
// Drawing: path fill/stroke
// ==================================================================

void CanvasRenderingContext2D::fill(FillRule rule) {
    drawPathFill(currentPath_, rule);
}

void CanvasRenderingContext2D::fill(const Path2D& path, FillRule rule) {
    drawPathFill(path, rule);
}

void CanvasRenderingContext2D::stroke() {
    drawPathStroke(currentPath_);
}

void CanvasRenderingContext2D::stroke(const Path2D& path) {
    drawPathStroke(path);
}

void CanvasRenderingContext2D::clip(FillRule /*rule*/) {
    // Set stencil buffer from current path
    nx_canvas_glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    drawPathFill(currentPath_, FillRule::NonZero);

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilFunc(GL_EQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    nx_canvas_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void CanvasRenderingContext2D::clip(const Path2D& path, FillRule rule) {
    Path2D saved = currentPath_;
    currentPath_ = path;
    clip(rule);
    currentPath_ = saved;
}

bool CanvasRenderingContext2D::isPointInPath(float /*x*/, float /*y*/, FillRule /*rule*/) {
    return false; // TODO: proper point-in-path with winding
}

bool CanvasRenderingContext2D::isPointInStroke(float /*x*/, float /*y*/) {
    return false;
}

void CanvasRenderingContext2D::drawPathFill(const Path2D& path, FillRule /*rule*/) {
    if (path.empty()) return;

    nx_canvas_glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width_, height_, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    applyCompositing();

    const Color& c = state_.fillStyle.color;
    float a = state_.globalAlpha;
    glColor4f(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (c.a / 255.0f) * a);

    // Tessellate path to triangles using GL_TRIANGLE_FAN per subpath
    glBegin(GL_TRIANGLE_FAN);
    for (const auto& cmd : path.commands()) {
        switch (cmd.type) {
            case Path2D::Command::MoveTo:
                glEnd();
                glBegin(GL_TRIANGLE_FAN);
                glVertex2f(cmd.args[0], cmd.args[1]);
                break;
            case Path2D::Command::LineTo:
                glVertex2f(cmd.args[0], cmd.args[1]);
                break;
            case Path2D::Command::Arc: {
                float cx = cmd.args[0], cy = cmd.args[1], r = cmd.args[2];
                float start = cmd.args[3], end = cmd.args[4];
                bool ccw = cmd.args[5] > 0.5f;
                int segs = std::max(8, static_cast<int>(r * 0.5f));
                float delta = (end - start) / segs;
                if (ccw && delta > 0) delta = -delta;
                if (!ccw && delta < 0) delta = -delta;
                for (int i = 0; i <= segs; i++) {
                    float angle = start + delta * i;
                    glVertex2f(cx + r * std::cos(angle), cy + r * std::sin(angle));
                }
                break;
            }
            case Path2D::Command::Close:
                break;
            default:
                if (cmd.argCount >= 2) {
                    glVertex2f(cmd.args[cmd.argCount - 2], cmd.args[cmd.argCount - 1]);
                }
                break;
        }
    }
    glEnd();

    nx_canvas_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void CanvasRenderingContext2D::drawPathStroke(const Path2D& path) {
    if (path.empty()) return;

    nx_canvas_glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width_, height_, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    applyCompositing();

    const Color& c = state_.strokeStyle.color;
    float a = state_.globalAlpha;
    glColor4f(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (c.a / 255.0f) * a);
    glLineWidth(state_.lineWidth);

    glBegin(GL_LINE_STRIP);
    for (const auto& cmd : path.commands()) {
        switch (cmd.type) {
            case Path2D::Command::MoveTo:
                glEnd();
                glBegin(GL_LINE_STRIP);
                glVertex2f(cmd.args[0], cmd.args[1]);
                break;
            case Path2D::Command::LineTo:
                glVertex2f(cmd.args[0], cmd.args[1]);
                break;
            case Path2D::Command::Arc: {
                float cx = cmd.args[0], cy = cmd.args[1], r = cmd.args[2];
                float start = cmd.args[3], end = cmd.args[4];
                int segs = std::max(8, static_cast<int>(r * 0.5f));
                float delta = (end - start) / segs;
                for (int i = 0; i <= segs; i++) {
                    float angle = start + delta * i;
                    glVertex2f(cx + r * std::cos(angle), cy + r * std::sin(angle));
                }
                break;
            }
            case Path2D::Command::Close:
                // Close by restarting to first vertex not practical here — skip
                break;
            default:
                if (cmd.argCount >= 2) {
                    glVertex2f(cmd.args[cmd.argCount - 2], cmd.args[cmd.argCount - 1]);
                }
                break;
        }
    }
    glEnd();

    nx_canvas_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ==================================================================
// Text
// ==================================================================

void CanvasRenderingContext2D::fillText(const std::string& /*text*/, float /*x*/, float /*y*/,
                                         float /*maxWidth*/) {
    // Text rendering delegates to nxgfx text shaper — bridge point
}

void CanvasRenderingContext2D::strokeText(const std::string& /*text*/, float /*x*/, float /*y*/,
                                            float /*maxWidth*/) {
    // Text stroke rendering — bridge point
}

TextMetrics CanvasRenderingContext2D::measureText(const std::string& text) {
    TextMetrics m;
    // Approximate based on font size
    float fontSize = 10;
    std::istringstream iss(state_.font);
    iss >> fontSize;
    m.width = text.length() * fontSize * 0.6f;
    m.actualBoundingBoxAscent = fontSize * 0.8f;
    m.actualBoundingBoxDescent = fontSize * 0.2f;
    m.fontBoundingBoxAscent = fontSize * 0.9f;
    m.fontBoundingBoxDescent = fontSize * 0.3f;
    return m;
}

// ==================================================================
// Image drawing
// ==================================================================

void CanvasRenderingContext2D::drawImage(uint32_t textureId, float dx, float dy) {
    // Get texture dimensions
    int texW = 0, texH = 0;
    glBindTexture(GL_TEXTURE_2D, textureId);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &texW);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &texH);
    drawImage(textureId, dx, dy, static_cast<float>(texW), static_cast<float>(texH));
}

void CanvasRenderingContext2D::drawImage(uint32_t textureId, float dx, float dy,
                                           float dw, float dh) {
    drawImage(textureId, 0, 0, dw, dh, dx, dy, dw, dh);
}

void CanvasRenderingContext2D::drawImage(uint32_t textureId,
                                           float sx, float sy, float sw, float sh,
                                           float dx, float dy, float dw, float dh) {
    nx_canvas_glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width_, height_, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    applyCompositing();
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, textureId);

    float a = state_.globalAlpha;
    glColor4f(1, 1, 1, a);

    // Compute texture coordinates
    int texW = 0, texH = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &texW);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &texH);

    float u0 = (texW > 0) ? sx / texW : 0;
    float v0 = (texH > 0) ? sy / texH : 0;
    float u1 = (texW > 0) ? (sx + sw) / texW : 1;
    float v1 = (texH > 0) ? (sy + sh) / texH : 1;

    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(dx, dy);
    glTexCoord2f(u1, v0); glVertex2f(dx + dw, dy);
    glTexCoord2f(u1, v1); glVertex2f(dx + dw, dy + dh);
    glTexCoord2f(u0, v1); glVertex2f(dx, dy + dh);
    glEnd();

    glDisable(GL_TEXTURE_2D);
    nx_canvas_glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ==================================================================
// Pixel manipulation
// ==================================================================

ImageData CanvasRenderingContext2D::createImageData(int width, int height) {
    return ImageData(width, height);
}

ImageData CanvasRenderingContext2D::getImageData(int sx, int sy, int sw, int sh) {
    ImageData data(sw, sh);

    nx_canvas_glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glReadPixels(sx, height_ - sy - sh, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, data.data.data());
    nx_canvas_glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Flip vertically (OpenGL vs Canvas coordinate system)
    std::vector<uint8_t> row(sw * 4);
    for (int y = 0; y < sh / 2; y++) {
        int topOffset = y * sw * 4;
        int botOffset = (sh - 1 - y) * sw * 4;
        std::memcpy(row.data(), &data.data[topOffset], sw * 4);
        std::memcpy(&data.data[topOffset], &data.data[botOffset], sw * 4);
        std::memcpy(&data.data[botOffset], row.data(), sw * 4);
    }

    return data;
}

void CanvasRenderingContext2D::putImageData(const ImageData& data, int dx, int dy) {
    putImageData(data, dx, dy, 0, 0, data.width, data.height);
}

void CanvasRenderingContext2D::putImageData(const ImageData& data, int dx, int dy,
                                              int dirtyX, int dirtyY, int dirtyW, int dirtyH) {
    // Clamp dirty rect
    dirtyX = std::max(0, dirtyX);
    dirtyY = std::max(0, dirtyY);
    dirtyW = std::min(dirtyW, data.width - dirtyX);
    dirtyH = std::min(dirtyH, data.height - dirtyY);

    if (dirtyW <= 0 || dirtyH <= 0) return;

    // Extract dirty sub-region
    std::vector<uint8_t> subData(dirtyW * dirtyH * 4);
    for (int y = 0; y < dirtyH; y++) {
        int srcRow = (dirtyY + y) * data.width * 4 + dirtyX * 4;
        int dstRow = y * dirtyW * 4;
        std::memcpy(&subData[dstRow], &data.data[srcRow], dirtyW * 4);
    }

    // Flip vertically for OpenGL
    std::vector<uint8_t> row(dirtyW * 4);
    for (int y = 0; y < dirtyH / 2; y++) {
        int topOffset = y * dirtyW * 4;
        int botOffset = (dirtyH - 1 - y) * dirtyW * 4;
        std::memcpy(row.data(), &subData[topOffset], dirtyW * 4);
        std::memcpy(&subData[topOffset], &subData[botOffset], dirtyW * 4);
        std::memcpy(&subData[botOffset], row.data(), dirtyW * 4);
    }

    nx_canvas_glBindFramebuffer(GL_FRAMEBUFFER, fbo_);

    // Disable blending for putImageData (direct pixel write per spec)
    glDisable(GL_BLEND);

    glRasterPos2i(dx + dirtyX, height_ - (dy + dirtyY) - dirtyH);
    glDrawPixels(dirtyW, dirtyH, GL_RGBA, GL_UNSIGNED_BYTE, subData.data());

    nx_canvas_glBindFramebuffer(GL_FRAMEBUFFER, 0);
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

CanvasPattern CanvasRenderingContext2D::createPattern(uint32_t textureId, int w, int h,
                                                       RepeatMode repeat) {
    CanvasPattern p;
    p.textureId = textureId;
    p.width = w; p.height = h;
    p.repeat = repeat;
    return p;
}

} // namespace NXRender
