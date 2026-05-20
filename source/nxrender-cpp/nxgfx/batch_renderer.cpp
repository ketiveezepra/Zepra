// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "nxgfx/batch_renderer.h"
#include "nxgfx/shader.h"
#include <GL/gl.h>
#include <cstring>
#include <iostream>

#ifdef __linux__
#include <GL/glx.h>
#elif _WIN32
#include <windows.h>
#endif

namespace NXRender {

// GL extension function types for VBO/VAO
typedef void  (*PFNGLGENBUFFERSPROC)(GLsizei, GLuint*);
typedef void  (*PFNGLBINDBUFFERPROC)(GLenum, GLuint);
typedef void  (*PFNGLBUFFERDATAPROC)(GLenum, GLsizeiptr, const void*, GLenum);
typedef void  (*PFNGLBUFFERSUBDATAPROC)(GLenum, GLintptr, GLsizeiptr, const void*);
typedef void  (*PFNGLDELETEBUFFERSPROC)(GLsizei, const GLuint*);
typedef void  (*PFNGLGENVERTEXARRAYSPROC)(GLsizei, GLuint*);
typedef void  (*PFNGLBINDVERTEXARRAYPROC)(GLuint);
typedef void  (*PFNGLDELETEVERTEXARRAYSPROC)(GLsizei, const GLuint*);
typedef void  (*PFNGLENABLEVERTEXATTRIBARRAYPROC)(GLuint);
typedef void  (*PFNGLVERTEXATTRIBPOINTERPROC)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
typedef void  (*PFNGLDRAWARRAYSPROC)(GLenum, GLint, GLsizei);
typedef void  (*PFNGLDRAWELEMENTSPROC)(GLenum, GLsizei, GLenum, const void*);

#define NXB_GL_FUNC(type, name) static type nxb_##name = nullptr

NXB_GL_FUNC(PFNGLGENBUFFERSPROC, glGenBuffers);
NXB_GL_FUNC(PFNGLBINDBUFFERPROC, glBindBuffer);
NXB_GL_FUNC(PFNGLBUFFERDATAPROC, glBufferData);
NXB_GL_FUNC(PFNGLBUFFERSUBDATAPROC, glBufferSubData);
NXB_GL_FUNC(PFNGLDELETEBUFFERSPROC, glDeleteBuffers);
NXB_GL_FUNC(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays);
NXB_GL_FUNC(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray);
NXB_GL_FUNC(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays);
NXB_GL_FUNC(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray);
NXB_GL_FUNC(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer);

#undef NXB_GL_FUNC

static bool s_batchGLLoaded = false;

static void loadBatchGLFunctions() {
    if (s_batchGLLoaded) return;
    s_batchGLLoaded = true;

#if defined(__linux__)
    #define BLOAD(name) nxb_##name = reinterpret_cast<decltype(nxb_##name)>( \
        glXGetProcAddress(reinterpret_cast<const GLubyte*>(#name)))
#elif defined(_WIN32)
    #define BLOAD(name) nxb_##name = reinterpret_cast<decltype(nxb_##name)>( \
        wglGetProcAddress(#name))
#else
    #define BLOAD(name) (void)0
#endif

    BLOAD(glGenBuffers);
    BLOAD(glBindBuffer);
    BLOAD(glBufferData);
    BLOAD(glBufferSubData);
    BLOAD(glDeleteBuffers);
    BLOAD(glGenVertexArrays);
    BLOAD(glBindVertexArray);
    BLOAD(glDeleteVertexArrays);
    BLOAD(glEnableVertexAttribArray);
    BLOAD(glVertexAttribPointer);

    #undef BLOAD
}

// ==================================================================
// BatchRenderer
// ==================================================================

BatchRenderer::BatchRenderer() {
    vertices_.resize(kMaxVertices);
}

BatchRenderer::~BatchRenderer() {
    shutdown();
}

bool BatchRenderer::init() {
    loadBatchGLFunctions();

    if (!nxb_glGenBuffers || !nxb_glBindBuffer || !nxb_glBufferData) {
        std::cerr << "[BatchRenderer] Failed to load GL buffer functions" << std::endl;
        return false;
    }

    // Create VBO
    nxb_glGenBuffers(1, &vbo_);
    nxb_glBindBuffer(0x8892 /* GL_ARRAY_BUFFER */, vbo_);
    nxb_glBufferData(0x8892, static_cast<GLsizeiptr>(kMaxVertices * sizeof(BatchVertex)),
                     nullptr, 0x88E8 /* GL_DYNAMIC_DRAW */);

    // Create VAO if available
    if (nxb_glGenVertexArrays && nxb_glBindVertexArray) {
        nxb_glGenVertexArrays(1, &vao_);
        nxb_glBindVertexArray(vao_);

        // Position (location 0)
        if (nxb_glEnableVertexAttribArray && nxb_glVertexAttribPointer) {
            nxb_glEnableVertexAttribArray(0);
            nxb_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(BatchVertex),
                                       reinterpret_cast<void*>(offsetof(BatchVertex, x)));
            // TexCoord (location 1)
            nxb_glEnableVertexAttribArray(1);
            nxb_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(BatchVertex),
                                       reinterpret_cast<void*>(offsetof(BatchVertex, u)));
            // Color (location 2)
            nxb_glEnableVertexAttribArray(2);
            nxb_glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(BatchVertex),
                                       reinterpret_cast<void*>(offsetof(BatchVertex, colorPacked)));
        }

        nxb_glBindVertexArray(0);
    }

    nxb_glBindBuffer(0x8892, 0);

    std::cout << "[BatchRenderer] Initialized (VBO=" << vbo_ << ", VAO=" << vao_
              << ", maxVerts=" << kMaxVertices << ")" << std::endl;

    return true;
}

void BatchRenderer::shutdown() {
    if (vao_ && nxb_glDeleteVertexArrays) {
        nxb_glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (vbo_ && nxb_glDeleteBuffers) {
        nxb_glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
}

void BatchRenderer::begin(int viewportWidth, int viewportHeight) {
    viewportWidth_ = viewportWidth;
    viewportHeight_ = viewportHeight;
    vertexCount_ = 0;
    active_ = true;
    resetStats();
}

void BatchRenderer::end() {
    if (!active_) return;
    if (vertexCount_ > 0) {
        internalFlush();
    }
    active_ = false;
}

void BatchRenderer::flush() {
    if (vertexCount_ > 0) {
        internalFlush();
    }
}

void BatchRenderer::setTexture(uint32_t textureId) {
    if (textureId != currentTexture_) {
        if (vertexCount_ > 0) {
            internalFlush();
            stats_.textureChanges++;
        }
        currentTexture_ = textureId;
    }
}

void BatchRenderer::setShader(ShaderProgram* shader) {
    if (shader != currentShader_) {
        if (vertexCount_ > 0) internalFlush();
        currentShader_ = shader;
    }
}

void BatchRenderer::setBlendMode(int mode) {
    if (mode != currentBlendMode_) {
        if (vertexCount_ > 0) {
            internalFlush();
            stats_.blendModeChanges++;
        }
        currentBlendMode_ = mode;
    }
}

void BatchRenderer::setClipRect(const Rect& rect) {
    if (!hasClipRect_ || currentClipRect_.x != rect.x || currentClipRect_.y != rect.y ||
        currentClipRect_.width != rect.width || currentClipRect_.height != rect.height) {
        if (vertexCount_ > 0) internalFlush();
        currentClipRect_ = rect;
        hasClipRect_ = true;
    }
}

void BatchRenderer::clearClipRect() {
    if (hasClipRect_) {
        if (vertexCount_ > 0) internalFlush();
        hasClipRect_ = false;
    }
}

void BatchRenderer::pushQuad(float x, float y, float w, float h, const Color& color) {
    ensureCapacity(6); // 2 triangles = 6 vertices

    BatchVertex tl(x, y, 0, 0, color);
    BatchVertex tr(x + w, y, 1, 0, color);
    BatchVertex bl(x, y + h, 0, 1, color);
    BatchVertex br(x + w, y + h, 1, 1, color);

    // Triangle 1: TL, TR, BL
    vertices_[vertexCount_++] = tl;
    vertices_[vertexCount_++] = tr;
    vertices_[vertexCount_++] = bl;

    // Triangle 2: TR, BR, BL
    vertices_[vertexCount_++] = tr;
    vertices_[vertexCount_++] = br;
    vertices_[vertexCount_++] = bl;
}

void BatchRenderer::pushTexturedQuad(float x, float y, float w, float h,
                                      float u0, float v0, float u1, float v1,
                                      const Color& tint) {
    ensureCapacity(6);

    BatchVertex tl(x, y, u0, v0, tint);
    BatchVertex tr(x + w, y, u1, v0, tint);
    BatchVertex bl(x, y + h, u0, v1, tint);
    BatchVertex br(x + w, y + h, u1, v1, tint);

    vertices_[vertexCount_++] = tl;
    vertices_[vertexCount_++] = tr;
    vertices_[vertexCount_++] = bl;
    vertices_[vertexCount_++] = tr;
    vertices_[vertexCount_++] = br;
    vertices_[vertexCount_++] = bl;
}

void BatchRenderer::pushTriangle(float x0, float y0, float x1, float y1,
                                  float x2, float y2, const Color& color) {
    ensureCapacity(3);

    vertices_[vertexCount_++] = BatchVertex(x0, y0, 0, 0, color);
    vertices_[vertexCount_++] = BatchVertex(x1, y1, 0, 0, color);
    vertices_[vertexCount_++] = BatchVertex(x2, y2, 0, 0, color);
}

void BatchRenderer::pushVertices(const BatchVertex* verts, size_t count) {
    ensureCapacity(count);
    memcpy(&vertices_[vertexCount_], verts, count * sizeof(BatchVertex));
    vertexCount_ += count;
}

void BatchRenderer::ensureCapacity(size_t count) {
    if (vertexCount_ + count > kMaxVertices) {
        internalFlush();
        stats_.bufferFullFlushes++;
    }
}

void BatchRenderer::buildProjectionMatrix(float width, float height, float* m) {
    // Orthographic projection: origin top-left, Y-down
    memset(m, 0, 16 * sizeof(float));
    m[0]  =  2.0f / width;
    m[5]  = -2.0f / height;
    m[10] = -1.0f;
    m[12] = -1.0f;
    m[13] =  1.0f;
    m[14] =  0.0f;
    m[15] =  1.0f;
}

void BatchRenderer::internalFlush() {
    if (vertexCount_ == 0 || !vbo_) return;

    // Bind shader
    ShaderProgram* shader = currentShader_;
    if (!shader) shader = Shaders::solidColor();
    if (!shader || !shader->isValid()) return;

    shader->bind();

    // Set projection matrix
    float proj[16];
    buildProjectionMatrix(static_cast<float>(viewportWidth_), static_cast<float>(viewportHeight_), proj);
    shader->setUniformMat4("uProjection", proj);

    // Bind texture
    if (currentTexture_ != 0) {
        glBindTexture(GL_TEXTURE_2D, currentTexture_);
        shader->setUniform1i("uTexture", 0);
    }

    // Set clip rect
    if (hasClipRect_) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(static_cast<int>(currentClipRect_.x),
                  viewportHeight_ - static_cast<int>(currentClipRect_.y + currentClipRect_.height),
                  static_cast<int>(currentClipRect_.width),
                  static_cast<int>(currentClipRect_.height));
    }

    // Upload vertices
    nxb_glBindBuffer(0x8892 /* GL_ARRAY_BUFFER */, vbo_);
    nxb_glBufferSubData(0x8892, 0,
                         static_cast<GLsizeiptr>(vertexCount_ * sizeof(BatchVertex)),
                         vertices_.data());

    // Bind VAO and draw
    if (vao_ && nxb_glBindVertexArray) {
        nxb_glBindVertexArray(vao_);
    }

    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertexCount_));

    if (vao_ && nxb_glBindVertexArray) {
        nxb_glBindVertexArray(0);
    }

    nxb_glBindBuffer(0x8892, 0);

    if (hasClipRect_) {
        glDisable(GL_SCISSOR_TEST);
    }

    if (currentTexture_ != 0) {
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    ShaderProgram::unbind();

    // Update stats
    stats_.drawCalls++;
    stats_.vertexCount += static_cast<uint32_t>(vertexCount_);
    stats_.triangleCount += static_cast<uint32_t>(vertexCount_ / 3);
    stats_.batchBreaks++;

    vertexCount_ = 0;
}

void BatchRenderer::resetStats() {
    stats_ = BatchStats{};
}

} // namespace NXRender
