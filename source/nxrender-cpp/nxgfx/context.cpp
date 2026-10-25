// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file context.cpp
 * @brief GPU context implementation with OpenGL backend
 */

#include "nxgfx/context.h"
#include <algorithm>
#include "nxgfx/text.h"
#include <cmath>
#include <vector>
#include <unordered_map>

// Centralized OpenGL headers + constant defines
#include "nxgfx/gl_includes.h"

// GL extension proc loader (file-local — needed for FBO/wglGetProcAddress)
#ifdef _WIN32
#   define GET_GL_PROC(type, name) type name = (type)wglGetProcAddress(#name)
#elif defined(__APPLE__)
#   define GET_GL_PROC(type, name) type name = (type)nxAppleGetProcAddress(#name)
#else
#   include <GL/glx.h>
#   define GET_GL_PROC(type, name) type name = (type)glXGetProcAddress((const GLubyte*)#name)
#endif


namespace NXRender {

// OpenGL FBO pointers
typedef void (*PFNGLGENFRAMEBUFFERSPROC)(GLsizei n, GLuint *framebuffers);
typedef void (*PFNGLBINDFRAMEBUFFERPROC)(GLenum target, GLuint framebuffer);
typedef void (*PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
typedef GLenum (*PFNGLCHECKFRAMEBUFFERSTATUSPROC)(GLenum target);
typedef void (*PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei n, const GLuint *framebuffers);

static PFNGLGENFRAMEBUFFERSPROC nx_glGenFramebuffers = nullptr;
static PFNGLBINDFRAMEBUFFERPROC nx_glBindFramebuffer = nullptr;
static PFNGLFRAMEBUFFERTEXTURE2DPROC nx_glFramebufferTexture2D = nullptr;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC nx_glCheckFramebufferStatus = nullptr;
static PFNGLDELETEFRAMEBUFFERSPROC nx_glDeleteFramebuffers = nullptr;

static void loadGLExtensions() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    
    GET_GL_PROC(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers);
    GET_GL_PROC(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer);
    GET_GL_PROC(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D);
    GET_GL_PROC(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus);
    GET_GL_PROC(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers);
    
    nx_glGenFramebuffers = glGenFramebuffers;
    nx_glBindFramebuffer = glBindFramebuffer;
    nx_glFramebufferTexture2D = glFramebufferTexture2D;
    nx_glCheckFramebufferStatus = glCheckFramebufferStatus;
    nx_glDeleteFramebuffers = glDeleteFramebuffers;
}


// Global GPU context
static GpuContext* g_gpuContext = nullptr;

GpuContext* gpu() {
    return g_gpuContext;
}

// Implementation details
struct GpuContext::Impl {
    std::vector<Rect> clipStack;
    std::unordered_map<TextureId, GLuint> fbos;
    
    void glColor(const Color& c) {
        glColor4ub(c.r, c.g, c.b, c.a);
    }
};

GpuContext::GpuContext() : impl_(std::make_unique<Impl>()) {}
GpuContext::~GpuContext() { shutdown(); }

bool GpuContext::init(int width, int height) {
    width_ = width;
    height_ = height;
    g_gpuContext = this;
    
    // OpenGL setup
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    
    // Enable Anti-Aliasing
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    
    return true;
}

void GpuContext::shutdown() {
    if (g_gpuContext == this) {
        g_gpuContext = nullptr;
    }
}

void GpuContext::beginFrame() {
    // Setup 2D orthographic projection
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width_, height_, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

void GpuContext::endFrame() {
    glFlush();
}

void GpuContext::present() {
    // Swap buffers handled by window system
}

bool GpuContext::readPixels(int x, int y, int width, int height, std::vector<uint8_t>& rgb) {
    if (width <= 0 || height <= 0) return false;
    rgb.assign(static_cast<size_t>(width) * height * 3, 0);
    // Clear errors left by earlier drawing so the check below is about this read
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++) {}
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, width, height, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    return glGetError() == GL_NO_ERROR;
}

void GpuContext::setViewport(int x, int y, int width, int height) {
    glViewport(x, y, width, height);
    width_ = width;
    height_ = height;
}

void GpuContext::pushClip(const Rect& rect) {
    impl_->clipStack.push_back(rect);
    glEnable(GL_SCISSOR_TEST);
    glScissor(static_cast<int>(rect.x), 
              static_cast<int>(height_ - rect.y - rect.height),
              static_cast<int>(rect.width), 
              static_cast<int>(rect.height));
}

void GpuContext::popClip() {
    if (!impl_->clipStack.empty()) {
        impl_->clipStack.pop_back();
    }
    if (impl_->clipStack.empty()) {
        glDisable(GL_SCISSOR_TEST);
    } else {
        const auto& rect = impl_->clipStack.back();
        glScissor(static_cast<int>(rect.x),
                  static_cast<int>(height_ - rect.y - rect.height),
                  static_cast<int>(rect.width),
                  static_cast<int>(rect.height));
    }
}

void GpuContext::pushTransform() {
    glPushMatrix();
}

void GpuContext::popTransform() {
    glPopMatrix();
}

void GpuContext::translate(float x, float y) {
    glTranslatef(x, y, 0);
}

void GpuContext::scale(float sx, float sy) {
    glScalef(sx, sy, 1);
}

void GpuContext::rotate(float radians) {
    glRotatef(radians * 180.0f / 3.14159265f, 0, 0, 1);
}

void GpuContext::setBlendMode(BlendMode mode) {
    switch (mode) {
        case BlendMode::Normal:
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case BlendMode::Add:
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            break;
        case BlendMode::Multiply:
            glBlendFunc(GL_DST_COLOR, GL_ZERO);
            break;
        case BlendMode::NoneBlend:
            glDisable(GL_BLEND);
            return;
        default:
            break;
    }
    glEnable(GL_BLEND);
}

void GpuContext::clear(const Color& color) {
    glClearColor(color.r / 255.0f, color.g / 255.0f, 
                 color.b / 255.0f, color.a / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

// ==========================================================================
// Drawing Primitives
// ==========================================================================

void GpuContext::fillRect(const Rect& rect, const Color& color) {
    impl_->glColor(color);
    glBegin(GL_QUADS);
    glVertex2f(rect.x, rect.y);
    glVertex2f(rect.x + rect.width, rect.y);
    glVertex2f(rect.x + rect.width, rect.y + rect.height);
    glVertex2f(rect.x, rect.y + rect.height);
    glEnd();
}

void GpuContext::strokeRect(const Rect& rect, const Color& color, float lineWidth) {
    impl_->glColor(color);
    glLineWidth(lineWidth);
    glBegin(GL_LINE_LOOP);
    glVertex2f(rect.x, rect.y);
    glVertex2f(rect.x + rect.width, rect.y);
    glVertex2f(rect.x + rect.width, rect.y + rect.height);
    glVertex2f(rect.x, rect.y + rect.height);
    glEnd();
}

void GpuContext::fillRoundedRect(const Rect& rect, const Color& color, float radius) {
    fillRoundedRect(rect, color, CornerRadii(radius));
}

void GpuContext::fillRoundedRect(const Rect& rect, const Color& color, const CornerRadii& radii) {
    impl_->glColor(color);
    
    const int segments = 8;
    float r = radii.topLeft;  // Simplified: use uniform radius
    
    glBegin(GL_TRIANGLE_FAN);
    // Center
    glVertex2f(rect.x + rect.width / 2, rect.y + rect.height / 2);
    
    // Top-left corner
    for (int i = 0; i <= segments; i++) {
        float angle = 3.14159265f + (3.14159265f / 2.0f) * i / segments;
        glVertex2f(rect.x + r + r * std::cos(angle), rect.y + r + r * std::sin(angle));
    }
    
    // Top-right corner
    for (int i = 0; i <= segments; i++) {
        float angle = 3.14159265f * 1.5f + (3.14159265f / 2.0f) * i / segments;
        glVertex2f(rect.x + rect.width - r + r * std::cos(angle), 
                   rect.y + r + r * std::sin(angle));
    }
    
    // Bottom-right corner
    for (int i = 0; i <= segments; i++) {
        float angle = 0 + (3.14159265f / 2.0f) * i / segments;
        glVertex2f(rect.x + rect.width - r + r * std::cos(angle),
                   rect.y + rect.height - r + r * std::sin(angle));
    }
    
    // Bottom-left corner
    for (int i = 0; i <= segments; i++) {
        float angle = 3.14159265f / 2.0f + (3.14159265f / 2.0f) * i / segments;
        glVertex2f(rect.x + r + r * std::cos(angle),
                   rect.y + rect.height - r + r * std::sin(angle));
    }
    
    // Close
    glVertex2f(rect.x, rect.y + r);
    glEnd();
}

void GpuContext::strokeRoundedRect(const Rect& rect, const Color& color, float radius, float lineWidth) {
    impl_->glColor(color);
    glLineWidth(lineWidth);
    
    const int segments = 8;
    float r = radius;
    
    glBegin(GL_LINE_LOOP);
    // Top-left corner
    for (int i = 0; i <= segments; i++) {
        float angle = 3.14159265f + (3.14159265f / 2.0f) * i / segments;
        glVertex2f(rect.x + r + r * std::cos(angle), rect.y + r + r * std::sin(angle));
    }
    // Top-right corner
    for (int i = 0; i <= segments; i++) {
        float angle = 3.14159265f * 1.5f + (3.14159265f / 2.0f) * i / segments;
        glVertex2f(rect.x + rect.width - r + r * std::cos(angle), 
                   rect.y + r + r * std::sin(angle));
    }
    // Bottom-right corner
    for (int i = 0; i <= segments; i++) {
        float angle = 0 + (3.14159265f / 2.0f) * i / segments;
        glVertex2f(rect.x + rect.width - r + r * std::cos(angle),
                   rect.y + rect.height - r + r * std::sin(angle));
    }
    // Bottom-left corner
    for (int i = 0; i <= segments; i++) {
        float angle = 3.14159265f / 2.0f + (3.14159265f / 2.0f) * i / segments;
        glVertex2f(rect.x + r + r * std::cos(angle),
                   rect.y + rect.height - r + r * std::sin(angle));
    }
    glEnd();
}

void GpuContext::fillCircle(float cx, float cy, float radius, const Color& color) {
    impl_->glColor(color);
    const int segments = 32;
    
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy);
    for (int i = 0; i <= segments; i++) {
        float angle = 2.0f * 3.14159265f * i / segments;
        glVertex2f(cx + radius * std::cos(angle), cy + radius * std::sin(angle));
    }
    glEnd();
}

void GpuContext::strokeCircle(float cx, float cy, float radius, const Color& color, float lineWidth) {
    impl_->glColor(color);
    glLineWidth(lineWidth);
    const int segments = 32;
    
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < segments; i++) {
        float angle = 2.0f * 3.14159265f * i / segments;
        glVertex2f(cx + radius * std::cos(angle), cy + radius * std::sin(angle));
    }
    glEnd();
}

void GpuContext::drawLine(float x1, float y1, float x2, float y2, const Color& color, float lineWidth) {
    impl_->glColor(color);
    glLineWidth(lineWidth);
    glBegin(GL_LINES);
    glVertex2f(x1, y1);
    glVertex2f(x2, y2);
    glEnd();
}

void GpuContext::fillRectGradient(const Rect& rect, const Color& startColor, const Color& endColor, bool horizontal) {
    glBegin(GL_QUADS);
    if (horizontal) {
        glColor4ub(startColor.r, startColor.g, startColor.b, startColor.a);
        glVertex2f(rect.x, rect.y);
        glVertex2f(rect.x, rect.y + rect.height);
        glColor4ub(endColor.r, endColor.g, endColor.b, endColor.a);
        glVertex2f(rect.x + rect.width, rect.y + rect.height);
        glVertex2f(rect.x + rect.width, rect.y);
    } else {
        glColor4ub(startColor.r, startColor.g, startColor.b, startColor.a);
        glVertex2f(rect.x, rect.y);
        glVertex2f(rect.x + rect.width, rect.y);
        glColor4ub(endColor.r, endColor.g, endColor.b, endColor.a);
        glVertex2f(rect.x + rect.width, rect.y + rect.height);
        glVertex2f(rect.x, rect.y + rect.height);
    }
    glEnd();
}

void GpuContext::drawShadow(const Rect& rect, const Color& color, float blur, float offsetX, float offsetY) {
    // Simple shadow implementation (multiple semitransparent rects)
    int layers = static_cast<int>(blur / 2);
    if (layers < 1) layers = 1;
    
    for (int i = layers; i >= 0; i--) {
        float expand = (blur / layers) * i;
        Color shadowColor = color.withAlpha(static_cast<uint8_t>(color.a * (1.0f - (float)i / (layers + 1))));
        Rect shadowRect(rect.x + offsetX - expand, rect.y + offsetY - expand,
                        rect.width + expand * 2, rect.height + expand * 2);
        fillRect(shadowRect, shadowColor);
    }
}


// ==========================================================================
// Paths
// ==========================================================================

void GpuContext::fillPath(const std::vector<Point>& points, const Color& color) {
    if (points.size() < 3) return;
    impl_->glColor(color);
    glBegin(GL_TRIANGLE_FAN);
    for (const auto& p : points) {
        glVertex2f(p.x, p.y);
    }
    glEnd();
}

void GpuContext::strokePath(const std::vector<Point>& points, const Color& color, float lineWidth, bool closed) {
    if (points.size() < 2) return;
    impl_->glColor(color);
    glLineWidth(std::max(1.0f, lineWidth));
    
    glEnable(GL_LINE_SMOOTH);
    glBegin(closed ? GL_LINE_LOOP : GL_LINE_STRIP);
    for (const auto& p : points) {
        glVertex2f(p.x, p.y);
    }
    glEnd();
    glDisable(GL_LINE_SMOOTH);
}

void GpuContext::fillComplexPath(const std::vector<std::vector<Point>>& contours, const Color& color, const std::string& rule) {
    if (contours.empty()) return;

    // Disable color writing
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    
    // Enable stencil test
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glClear(GL_STENCIL_BUFFER_BIT); // Clear stencil for this shape

    // Configure stencil op
    glStencilFunc(GL_ALWAYS, 0, 0xFF);
    
    // For both nonzero and evenodd, INVERT works well for standard holes
    // Correct "nonzero" would require increment/decrement but INVERT is 
    // a robust approximation for most simple SVG paths with holes.
    // Ideally we'd support true nonzero winding if we had GL_INCR_WRAP/GL_DECR_WRAP logic.
    glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);

    // Draw triangle fans for every contour
    for (const auto& contour : contours) {
         if (contour.size() < 3) continue;
         glBegin(GL_TRIANGLE_FAN);
         glVertex2f(contour[0].x, contour[0].y); // Pivot
         for (const auto& p : contour) {
             glVertex2f(p.x, p.y);
         }
         // Ensure closed for stencil calculation
         glVertex2f(contour[0].x, contour[0].y);
         glEnd();
    }

    // Enable color writing
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    
    // Draw where stencil is not 0 (or odd if we used INVERT logic which is basically mod 2)
    glStencilFunc(GL_NOTEQUAL, 0, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    
    impl_->glColor(color);
    
    // Draw bounding box / fullscreen quad with fill color
    // Determine bounds
    float minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
    bool valid = false;
    for (const auto& c : contours) {
        for (const auto& p : c) {
            if (p.x < minX) minX = p.x;
            if (p.y < minY) minY = p.y;
            if (p.x > maxX) maxX = p.x;
            if (p.y > maxY) maxY = p.y;
            valid = true;
        }
    }
    
    // Fallback if no valid points (shouldn't happen due to check above)
    if (!valid) { minX=0; maxX=1; minY=0; maxY=1; }

    glBegin(GL_QUADS);
    glVertex2f(minX, minY);
    glVertex2f(maxX, minY);
    glVertex2f(maxX, maxY);
    glVertex2f(minX, maxY);
    glEnd();

    // Disable stencil test
    glDisable(GL_STENCIL_TEST);
}


// ==========================================================================
// Text (uses TextRenderer)
// ==========================================================================

void GpuContext::drawText(const std::string& text, float x, float y, const Color& color, float fontSize) {
    FontStyle style;
    style.size = fontSize;
    TextRenderer::instance().render(text, x, y, color, style);
}

Size GpuContext::measureText(const std::string& text, float fontSize) {
    FontStyle style;
    style.size = fontSize;
    TextMetrics m = TextRenderer::instance().measure(text, style);
    return Size(m.width, m.height);
}

void GpuContext::setFont(const std::string& fontFamily) {
    TextRenderer::instance().setDefaultFont(fontFamily);
}

// ==========================================================================
// Textures
// ==========================================================================

GpuContext::TextureId GpuContext::loadTexture(const std::string& path) {
    // Basic texture loading (would use stb_image in production)
    (void)path;
    return 0;  // TODO: Implement with stb_image
}

GpuContext::TextureId GpuContext::createTexture(int width, int height, const uint8_t* pixels) {
    unsigned int texId;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return texId;
}

void GpuContext::drawTexture(TextureId texture, const Rect& dest) {
    if (!texture) return;
    
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);
    glColor4ub(255, 255, 255, 255);
    
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(dest.x, dest.y);
    glTexCoord2f(1, 0); glVertex2f(dest.x + dest.width, dest.y);
    glTexCoord2f(1, 1); glVertex2f(dest.x + dest.width, dest.y + dest.height);
    glTexCoord2f(0, 1); glVertex2f(dest.x, dest.y + dest.height);
    glEnd();
    
    glDisable(GL_TEXTURE_2D);
}

void GpuContext::drawTexture(TextureId texture, const Rect& src, const Rect& dest) {
    if (!texture) return;
    
    // Assume texture is normalized 0-1 coords; src is in pixel coords
    // Would need texture dimensions to convert properly
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);
    glColor4ub(255, 255, 255, 255);
    
    float u1 = src.x / 256.0f;  // Simplified - assume 256x256 texture
    float v1 = src.y / 256.0f;
    float u2 = (src.x + src.width) / 256.0f;
    float v2 = (src.y + src.height) / 256.0f;
    
    glBegin(GL_QUADS);
    glTexCoord2f(u1, v1); glVertex2f(dest.x, dest.y);
    glTexCoord2f(u2, v1); glVertex2f(dest.x + dest.width, dest.y);
    glTexCoord2f(u2, v2); glVertex2f(dest.x + dest.width, dest.y + dest.height);
    glTexCoord2f(u1, v2); glVertex2f(dest.x, dest.y + dest.height);
    glEnd();
    
    glDisable(GL_TEXTURE_2D);
}

void GpuContext::destroyTexture(TextureId texture) {
    if (texture) {
        glDeleteTextures(1, &texture);
        destroyRenderTarget(texture);
    }
}

// ==========================================================================
// Render Targets
// ==========================================================================

GpuContext::TextureId GpuContext::createRenderTarget(int width, int height) {
    loadGLExtensions();
    if (!nx_glGenFramebuffers) return 0;
    
    unsigned int texId;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLuint fbo;
    nx_glGenFramebuffers(1, &fbo);
    nx_glBindFramebuffer(0x8D40 /* GL_FRAMEBUFFER */, fbo);
    nx_glFramebufferTexture2D(0x8D40, 0x8CE0 /* GL_COLOR_ATTACHMENT0 */, GL_TEXTURE_2D, texId, 0);
    
    nx_glBindFramebuffer(0x8D40, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    impl_->fbos[texId] = fbo;
    return texId;
}

void GpuContext::setRenderTarget(TextureId target) {
    if (!nx_glBindFramebuffer) return;
    
    if (target == 0) {
        nx_glBindFramebuffer(0x8D40, 0);
        glViewport(0, 0, width_, height_);
    } else {
        auto it = impl_->fbos.find(target);
        if (it != impl_->fbos.end()) {
            nx_glBindFramebuffer(0x8D40, it->second);
        }
    }
}

void GpuContext::destroyRenderTarget(TextureId target) {
    auto it = impl_->fbos.find(target);
    if (it != impl_->fbos.end() && nx_glDeleteFramebuffers) {
        nx_glDeleteFramebuffers(1, &it->second);
        impl_->fbos.erase(it);
    }
}

} // namespace NXRender
