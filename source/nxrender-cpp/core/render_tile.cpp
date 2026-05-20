// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "render_tile.h"
#include "nxgfx/context.h"
#include <GL/gl.h>

#ifdef __linux__
#include <GL/glx.h>
#elif _WIN32
#include <windows.h>
#endif

namespace NXRender {

// FBO function pointers
typedef void (*PFNGLGENFRAMEBUFFERSPROC)(GLsizei, GLuint*);
typedef void (*PFNGLBINDFRAMEBUFFERPROC)(GLenum, GLuint);
typedef void (*PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (*PFNGLCHECKFRAMEBUFFERSTATUSPROC)(GLenum);
typedef void (*PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei, const GLuint*);

static PFNGLGENFRAMEBUFFERSPROC rt_glGenFramebuffers = nullptr;
static PFNGLBINDFRAMEBUFFERPROC rt_glBindFramebuffer = nullptr;
static PFNGLFRAMEBUFFERTEXTURE2DPROC rt_glFramebufferTexture2D = nullptr;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC rt_glCheckFramebufferStatus = nullptr;
static PFNGLDELETEFRAMEBUFFERSPROC rt_glDeleteFramebuffers = nullptr;

static bool s_rtGLLoaded = false;

static void loadRTGLFunctions() {
    if (s_rtGLLoaded) return;
    s_rtGLLoaded = true;

#if defined(__linux__)
    #define RTLOAD(name) rt_##name = reinterpret_cast<decltype(rt_##name)>( \
        glXGetProcAddress(reinterpret_cast<const GLubyte*>(#name)))
#elif defined(_WIN32)
    #define RTLOAD(name) rt_##name = reinterpret_cast<decltype(rt_##name)>( \
        wglGetProcAddress(#name))
#else
    #define RTLOAD(name) (void)0
#endif
    RTLOAD(glGenFramebuffers);
    RTLOAD(glBindFramebuffer);
    RTLOAD(glFramebufferTexture2D);
    RTLOAD(glCheckFramebufferStatus);
    RTLOAD(glDeleteFramebuffers);
    #undef RTLOAD
}

// ==================================================================
// RenderTile
// ==================================================================

RenderTile::RenderTile(int x, int y, int width, int height)
    : bounds_(static_cast<float>(x), static_cast<float>(y),
              static_cast<float>(width), static_cast<float>(height)) {
    loadRTGLFunctions();
}

RenderTile::~RenderTile() {
    destroy();
}

void RenderTile::destroy() {
    if (fbo_ != 0 && rt_glDeleteFramebuffers) {
        rt_glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    if (texture_ != 0) {
        glDeleteTextures(1, &texture_);
        texture_ = 0;
    }
    isReady_ = false;
}

bool RenderTile::ensureFBO(int width, int height) {
    if (fbo_ != 0 && fboWidth_ == width && fboHeight_ == height) return true;

    // Tear down old
    destroy();

    if (!rt_glGenFramebuffers || !rt_glBindFramebuffer || !rt_glFramebufferTexture2D) {
        return false;
    }

    fboWidth_ = width;
    fboHeight_ = height;

    // Create texture
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F); // GL_CLAMP_TO_EDGE
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Create FBO
    rt_glGenFramebuffers(1, &fbo_);
    rt_glBindFramebuffer(0x8D40 /* GL_FRAMEBUFFER */, fbo_);
    rt_glFramebufferTexture2D(0x8D40, 0x8CE0 /* GL_COLOR_ATTACHMENT0 */,
                               GL_TEXTURE_2D, texture_, 0);

    GLenum status = rt_glCheckFramebufferStatus(0x8D40);
    rt_glBindFramebuffer(0x8D40, 0);

    if (status != 0x8CD5 /* GL_FRAMEBUFFER_COMPLETE */) {
        destroy();
        return false;
    }

    return true;
}

void RenderTile::beginRecord(GpuContext* ctx) {
    (void)ctx;

    int w = static_cast<int>(bounds_.width);
    int h = static_cast<int>(bounds_.height);
    if (!ensureFBO(w, h)) return;

    isReady_ = false;

    // Save previous viewport
    glGetIntegerv(GL_VIEWPORT, savedViewport_);

    // Bind tile FBO
    if (rt_glBindFramebuffer) {
        rt_glBindFramebuffer(0x8D40, fbo_);
    }

    glViewport(0, 0, w, h);

    // Clear to transparent
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    // Push a translation so drawing happens in tile-local coords
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glTranslatef(-bounds_.x, -bounds_.y, 0.0f);
}

void RenderTile::endRecord(GpuContext* ctx) {
    (void)ctx;
    if (fbo_ == 0) return;

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    // Restore default framebuffer
    if (rt_glBindFramebuffer) {
        rt_glBindFramebuffer(0x8D40, 0);
    }

    // Restore previous viewport
    glViewport(savedViewport_[0], savedViewport_[1],
               savedViewport_[2], savedViewport_[3]);

    isReady_ = true;
    renderCount_++;
}

void RenderTile::draw(GpuContext* ctx, const Rect& targetRect) const {
    (void)ctx;
    if (!isReady_ || texture_ == 0) return;

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture_);

    // Enable blending for transparent tiles
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);
    glVertex2f(targetRect.x, targetRect.y);
    glTexCoord2f(1, 0);
    glVertex2f(targetRect.x + targetRect.width, targetRect.y);
    glTexCoord2f(1, 1);
    glVertex2f(targetRect.x + targetRect.width, targetRect.y + targetRect.height);
    glTexCoord2f(0, 1);
    glVertex2f(targetRect.x, targetRect.y + targetRect.height);
    glEnd();

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}

void RenderTile::setPriority(TilePriority priority) {
    priority_ = priority;
}

size_t RenderTile::memoryUsage() const {
    if (texture_ == 0) return 0;
    return static_cast<size_t>(fboWidth_) * static_cast<size_t>(fboHeight_) * 4;
}

} // namespace NXRender
