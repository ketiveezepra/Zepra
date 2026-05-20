// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "nxgfx/render_target.h"
#include <GL/gl.h>
#include <cstring>
#include <iostream>
#include <algorithm>

#ifdef __linux__
#include <GL/glx.h>
#elif _WIN32
#include <windows.h>
#endif

namespace NXRender {

// GL extension function pointers
typedef void (*PFNGLGENFRAMEBUFFERSPROC)(GLsizei, GLuint*);
typedef void (*PFNGLBINDFRAMEBUFFERPROC)(GLenum, GLuint);
typedef void (*PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (*PFNGLFRAMEBUFFERRENDERBUFFERPROC)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (*PFNGLCHECKFRAMEBUFFERSTATUSPROC)(GLenum);
typedef void (*PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei, const GLuint*);
typedef void (*PFNGLGENRENDERBUFFERSPROC)(GLsizei, GLuint*);
typedef void (*PFNGLBINDRENDERBUFFERPROC)(GLenum, GLuint);
typedef void (*PFNGLRENDERBUFFERSTORAGEPROC)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (*PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void (*PFNGLDELETERENDERBUFFERSPROC)(GLsizei, const GLuint*);
typedef void (*PFNGLBLITFRAMEBUFFERPROC)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef void (*PFNGLGETINTEGERVPROC_)(GLenum, GLint*);

#define RT_GL_FRAMEBUFFER 0x8D40
#define RT_GL_READ_FRAMEBUFFER 0x8CA8
#define RT_GL_DRAW_FRAMEBUFFER 0x8CA9
#define RT_GL_COLOR_ATTACHMENT0 0x8CE0
#define RT_GL_DEPTH_ATTACHMENT 0x8D00
#define RT_GL_STENCIL_ATTACHMENT 0x8D20
#define RT_GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define RT_GL_RENDERBUFFER 0x8D41
#define RT_GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define RT_GL_DEPTH_COMPONENT16 0x81A5
#define RT_GL_DEPTH24_STENCIL8  0x88F0
#define RT_GL_FRAMEBUFFER_BINDING 0x8CA6
#define RT_GL_CLAMP_TO_EDGE 0x812F
#define RT_GL_RGBA16F 0x881A
#define RT_GL_HALF_FLOAT 0x140B

static PFNGLGENFRAMEBUFFERSPROC rt_glGenFramebuffers = nullptr;
static PFNGLBINDFRAMEBUFFERPROC rt_glBindFramebuffer = nullptr;
static PFNGLFRAMEBUFFERTEXTURE2DPROC rt_glFramebufferTexture2D = nullptr;
static PFNGLFRAMEBUFFERRENDERBUFFERPROC rt_glFramebufferRenderbuffer = nullptr;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC rt_glCheckFramebufferStatus = nullptr;
static PFNGLDELETEFRAMEBUFFERSPROC rt_glDeleteFramebuffers = nullptr;
static PFNGLGENRENDERBUFFERSPROC rt_glGenRenderbuffers = nullptr;
static PFNGLBINDRENDERBUFFERPROC rt_glBindRenderbuffer = nullptr;
static PFNGLRENDERBUFFERSTORAGEPROC rt_glRenderbufferStorage = nullptr;
static PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC rt_glRenderbufferStorageMultisample = nullptr;
static PFNGLDELETERENDERBUFFERSPROC rt_glDeleteRenderbuffers = nullptr;
static PFNGLBLITFRAMEBUFFERPROC rt_glBlitFramebuffer = nullptr;

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
    RTLOAD(glFramebufferRenderbuffer);
    RTLOAD(glCheckFramebufferStatus);
    RTLOAD(glDeleteFramebuffers);
    RTLOAD(glGenRenderbuffers);
    RTLOAD(glBindRenderbuffer);
    RTLOAD(glRenderbufferStorage);
    RTLOAD(glRenderbufferStorageMultisample);
    RTLOAD(glDeleteRenderbuffers);
    RTLOAD(glBlitFramebuffer);
    #undef RTLOAD
}

// ==================================================================
// RenderTarget
// ==================================================================

RenderTarget::RenderTarget() {}

RenderTarget::~RenderTarget() {
    destroy();
}

bool RenderTarget::create(const RenderTargetDesc& desc) {
    loadRTGLFunctions();
    if (!rt_glGenFramebuffers || !rt_glBindFramebuffer) return false;
    if (desc.width <= 0 || desc.height <= 0) return false;

    destroy();
    desc_ = desc;

    return createAttachments();
}

bool RenderTarget::createAttachments() {
    // Determine GL format
    GLenum internalFormat = GL_RGBA;
    GLenum format = GL_RGBA;
    GLenum type = GL_UNSIGNED_BYTE;

    switch (desc_.colorFormat) {
        case RenderTargetFormat::RGBA8:
            internalFormat = GL_RGBA; format = GL_RGBA; type = GL_UNSIGNED_BYTE;
            break;
        case RenderTargetFormat::RGBA16F:
            internalFormat = RT_GL_RGBA16F; format = GL_RGBA; type = RT_GL_HALF_FLOAT;
            break;
        case RenderTargetFormat::R8:
            internalFormat = GL_RED; format = GL_RED; type = GL_UNSIGNED_BYTE;
            break;
        case RenderTargetFormat::RG8:
            internalFormat = GL_RG; format = GL_RG; type = GL_UNSIGNED_BYTE;
            break;
        default:
            break;
    }

    if (desc_.msaaSamples > 0 && rt_glRenderbufferStorageMultisample) {
        // MSAA path: create an MSAA FBO + a resolve FBO
        rt_glGenFramebuffers(1, &msaaFbo_);
        rt_glBindFramebuffer(RT_GL_FRAMEBUFFER, msaaFbo_);

        // MSAA color renderbuffer
        rt_glGenRenderbuffers(1, &msaaColorRbo_);
        rt_glBindRenderbuffer(RT_GL_RENDERBUFFER, msaaColorRbo_);
        rt_glRenderbufferStorageMultisample(RT_GL_RENDERBUFFER, desc_.msaaSamples,
                                            internalFormat, desc_.width, desc_.height);
        rt_glFramebufferRenderbuffer(RT_GL_FRAMEBUFFER, RT_GL_COLOR_ATTACHMENT0,
                                      RT_GL_RENDERBUFFER, msaaColorRbo_);

        if (desc_.hasDepth || desc_.hasStencil) {
            GLenum depthFormat = desc_.hasStencil ? RT_GL_DEPTH24_STENCIL8 : RT_GL_DEPTH_COMPONENT16;
            GLenum attachment = desc_.hasStencil ? RT_GL_DEPTH_STENCIL_ATTACHMENT : RT_GL_DEPTH_ATTACHMENT;

            rt_glGenRenderbuffers(1, &depthRbo_);
            rt_glBindRenderbuffer(RT_GL_RENDERBUFFER, depthRbo_);
            rt_glRenderbufferStorageMultisample(RT_GL_RENDERBUFFER, desc_.msaaSamples,
                                                depthFormat, desc_.width, desc_.height);
            rt_glFramebufferRenderbuffer(RT_GL_FRAMEBUFFER, attachment,
                                          RT_GL_RENDERBUFFER, depthRbo_);
        }

        GLenum status = rt_glCheckFramebufferStatus(RT_GL_FRAMEBUFFER);
        if (status != RT_GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "[RenderTarget] MSAA FBO incomplete: 0x" << std::hex << status << std::dec << std::endl;
            destroy();
            return false;
        }

        // Resolve FBO with a regular texture
        rt_glGenFramebuffers(1, &fbo_);
        rt_glBindFramebuffer(RT_GL_FRAMEBUFFER, fbo_);

        glGenTextures(1, &resolvedTexture_);
        glBindTexture(GL_TEXTURE_2D, resolvedTexture_);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, desc_.width, desc_.height,
                     0, format, type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, RT_GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, RT_GL_CLAMP_TO_EDGE);

        rt_glFramebufferTexture2D(RT_GL_FRAMEBUFFER, RT_GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, resolvedTexture_, 0);

        status = rt_glCheckFramebufferStatus(RT_GL_FRAMEBUFFER);
        if (status != RT_GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "[RenderTarget] Resolve FBO incomplete" << std::endl;
            destroy();
            return false;
        }

        rt_glBindFramebuffer(RT_GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);

    } else {
        // Non-MSAA path
        rt_glGenFramebuffers(1, &fbo_);
        rt_glBindFramebuffer(RT_GL_FRAMEBUFFER, fbo_);

        // Color texture
        glGenTextures(1, &colorTexture_);
        glBindTexture(GL_TEXTURE_2D, colorTexture_);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, desc_.width, desc_.height,
                     0, format, type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, RT_GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, RT_GL_CLAMP_TO_EDGE);

        rt_glFramebufferTexture2D(RT_GL_FRAMEBUFFER, RT_GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, colorTexture_, 0);

        // Depth/stencil
        if (desc_.hasDepth || desc_.hasStencil) {
            GLenum depthFormat = desc_.hasStencil ? RT_GL_DEPTH24_STENCIL8 : RT_GL_DEPTH_COMPONENT16;
            GLenum attachment = desc_.hasStencil ? RT_GL_DEPTH_STENCIL_ATTACHMENT : RT_GL_DEPTH_ATTACHMENT;

            rt_glGenRenderbuffers(1, &depthRbo_);
            rt_glBindRenderbuffer(RT_GL_RENDERBUFFER, depthRbo_);
            rt_glRenderbufferStorage(RT_GL_RENDERBUFFER, depthFormat, desc_.width, desc_.height);
            rt_glFramebufferRenderbuffer(RT_GL_FRAMEBUFFER, attachment,
                                          RT_GL_RENDERBUFFER, depthRbo_);
        }

        GLenum status = rt_glCheckFramebufferStatus(RT_GL_FRAMEBUFFER);
        if (status != RT_GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "[RenderTarget] FBO incomplete: 0x" << std::hex << status << std::dec << std::endl;
            destroy();
            return false;
        }

        rt_glBindFramebuffer(RT_GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    return true;
}

void RenderTarget::destroy() {
    if (colorTexture_) { glDeleteTextures(1, &colorTexture_); colorTexture_ = 0; }
    if (resolvedTexture_) { glDeleteTextures(1, &resolvedTexture_); resolvedTexture_ = 0; }
    if (depthRbo_ && rt_glDeleteRenderbuffers) { rt_glDeleteRenderbuffers(1, &depthRbo_); depthRbo_ = 0; }
    if (stencilRbo_ && rt_glDeleteRenderbuffers) { rt_glDeleteRenderbuffers(1, &stencilRbo_); stencilRbo_ = 0; }
    if (msaaColorRbo_ && rt_glDeleteRenderbuffers) { rt_glDeleteRenderbuffers(1, &msaaColorRbo_); msaaColorRbo_ = 0; }
    if (fbo_ && rt_glDeleteFramebuffers) { rt_glDeleteFramebuffers(1, &fbo_); fbo_ = 0; }
    if (msaaFbo_ && rt_glDeleteFramebuffers) { rt_glDeleteFramebuffers(1, &msaaFbo_); msaaFbo_ = 0; }
}

void RenderTarget::bind() {
    if (!fbo_) return;

    // Save current state
    glGetIntegerv(RT_GL_FRAMEBUFFER_BINDING, &prevFbo_);
    glGetIntegerv(GL_VIEWPORT, prevViewport_);

    uint32_t targetFbo = msaaFbo_ ? msaaFbo_ : fbo_;
    rt_glBindFramebuffer(RT_GL_FRAMEBUFFER, targetFbo);
    glViewport(0, 0, desc_.width, desc_.height);
}

void RenderTarget::unbind() {
    rt_glBindFramebuffer(RT_GL_FRAMEBUFFER, static_cast<uint32_t>(prevFbo_));
    glViewport(prevViewport_[0], prevViewport_[1], prevViewport_[2], prevViewport_[3]);
}

void RenderTarget::clear(float r, float g, float b, float a) {
    glClearColor(r, g, b, a);
    GLbitfield mask = GL_COLOR_BUFFER_BIT;
    if (desc_.hasDepth) mask |= GL_DEPTH_BUFFER_BIT;
    if (desc_.hasStencil) mask |= GL_STENCIL_BUFFER_BIT;
    glClear(mask);
}

void RenderTarget::resolve() {
    if (!msaaFbo_ || !fbo_ || !rt_glBlitFramebuffer) return;

    rt_glBindFramebuffer(RT_GL_READ_FRAMEBUFFER, msaaFbo_);
    rt_glBindFramebuffer(RT_GL_DRAW_FRAMEBUFFER, fbo_);
    rt_glBlitFramebuffer(0, 0, desc_.width, desc_.height,
                          0, 0, desc_.width, desc_.height,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
    rt_glBindFramebuffer(RT_GL_FRAMEBUFFER, 0);
}

bool RenderTarget::readPixels(int x, int y, int width, int height,
                                uint8_t* buffer, int channels) const {
    if (!fbo_ || !buffer) return false;

    uint32_t readFbo = fbo_;
    rt_glBindFramebuffer(RT_GL_FRAMEBUFFER, readFbo);

    GLenum format = (channels == 4) ? GL_RGBA : (channels == 3) ? GL_RGB : GL_RED;
    glReadPixels(x, y, width, height, format, GL_UNSIGNED_BYTE, buffer);

    rt_glBindFramebuffer(RT_GL_FRAMEBUFFER, 0);
    return true;
}

bool RenderTarget::resize(int width, int height) {
    if (width == desc_.width && height == desc_.height) return true;
    RenderTargetDesc newDesc = desc_;
    newDesc.width = width;
    newDesc.height = height;
    return create(newDesc);
}

void RenderTarget::bindColorTexture(int unit) const {
    uint32_t tex = resolvedTexture_ ? resolvedTexture_ : colorTexture_;
    if (!tex) return;
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
}

// ==================================================================
// RenderTargetPool
// ==================================================================

RenderTargetPool& RenderTargetPool::instance() {
    static RenderTargetPool pool;
    return pool;
}

RenderTarget* RenderTargetPool::acquire(int width, int height,
                                         RenderTargetFormat format, int msaa) {
    // Look for a matching available target
    for (auto& entry : pool_) {
        if (!entry.inUse && entry.target &&
            entry.target->width() == width &&
            entry.target->height() == height &&
            entry.target->desc().colorFormat == format &&
            entry.target->desc().msaaSamples == msaa) {
            entry.inUse = true;
            entry.lastUsedFrame = currentFrame_;
            activeCount_++;
            return entry.target;
        }
    }

    // Create a new one
    auto* rt = new RenderTarget();
    RenderTargetDesc desc(width, height, format);
    desc.msaaSamples = msaa;

    if (!rt->create(desc)) {
        delete rt;
        return nullptr;
    }

    PoolEntry entry;
    entry.target = rt;
    entry.inUse = true;
    entry.lastUsedFrame = currentFrame_;
    pool_.push_back(entry);
    activeCount_++;

    return rt;
}

void RenderTargetPool::release(RenderTarget* rt) {
    for (auto& entry : pool_) {
        if (entry.target == rt && entry.inUse) {
            entry.inUse = false;
            entry.lastUsedFrame = currentFrame_;
            activeCount_--;
            return;
        }
    }
}

void RenderTargetPool::flush() {
    for (auto& entry : pool_) {
        delete entry.target;
    }
    pool_.clear();
    activeCount_ = 0;
}

void RenderTargetPool::trim(int maxAge) {
    currentFrame_++;

    pool_.erase(
        std::remove_if(pool_.begin(), pool_.end(),
            [this, maxAge](PoolEntry& entry) {
                if (!entry.inUse && (currentFrame_ - entry.lastUsedFrame) > maxAge) {
                    delete entry.target;
                    return true;
                }
                return false;
            }),
        pool_.end());
}

} // namespace NXRender
