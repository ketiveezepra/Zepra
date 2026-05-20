// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "nxgfx/filter_pipeline.h"
#include "nxgfx/context.h"
#include "nxgfx/shader.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <iostream>

#include <GL/gl.h>
#ifdef __linux__
#include <GL/glx.h>
#elif _WIN32
#include <windows.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace NXRender {

// GL function pointers (reuse from batch_renderer if already loaded)
typedef void (*PFNGLGENFRAMEBUFFERSPROC)(GLsizei, GLuint*);
typedef void (*PFNGLBINDFRAMEBUFFERPROC)(GLenum, GLuint);
typedef void (*PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (*PFNGLCHECKFRAMEBUFFERSTATUSPROC)(GLenum);
typedef void (*PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei, const GLuint*);

static PFNGLGENFRAMEBUFFERSPROC nxf_glGenFramebuffers = nullptr;
static PFNGLBINDFRAMEBUFFERPROC nxf_glBindFramebuffer = nullptr;
static PFNGLFRAMEBUFFERTEXTURE2DPROC nxf_glFramebufferTexture2D = nullptr;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC nxf_glCheckFramebufferStatus = nullptr;
static PFNGLDELETEFRAMEBUFFERSPROC nxf_glDeleteFramebuffers = nullptr;

static bool s_filterGLLoaded = false;

static void loadFilterGLFunctions() {
    if (s_filterGLLoaded) return;
    s_filterGLLoaded = true;

#if defined(__linux__)
    #define FLOAD(name) nxf_##name = reinterpret_cast<decltype(nxf_##name)>( \
        glXGetProcAddress(reinterpret_cast<const GLubyte*>(#name)))
#elif defined(_WIN32)
    #define FLOAD(name) nxf_##name = reinterpret_cast<decltype(nxf_##name)>( \
        wglGetProcAddress(#name))
#else
    #define FLOAD(name) (void)0
#endif
    FLOAD(glGenFramebuffers);
    FLOAD(glBindFramebuffer);
    FLOAD(glFramebufferTexture2D);
    FLOAD(glCheckFramebufferStatus);
    FLOAD(glDeleteFramebuffers);
    #undef FLOAD
}

// ==================================================================
// FilterConfig factory methods
// ==================================================================

FilterConfig FilterConfig::gaussianBlur(float radius) {
    FilterConfig f;
    f.type = FilterType::GaussianBlur;
    f.parameters[0] = radius;
    f.paramCount = 1;
    return f;
}

FilterConfig FilterConfig::boxBlur(float radius) {
    FilterConfig f;
    f.type = FilterType::BoxBlur;
    f.parameters[0] = radius;
    f.paramCount = 1;
    return f;
}

FilterConfig FilterConfig::dropShadow(float offsetX, float offsetY, float blur, const Color& color) {
    FilterConfig f;
    f.type = FilterType::DropShadow;
    f.parameters[0] = offsetX;
    f.parameters[1] = offsetY;
    f.parameters[2] = blur;
    f.parameters[3] = color.r / 255.0f;
    f.parameters[4] = color.g / 255.0f;
    f.parameters[5] = color.b / 255.0f;
    f.parameters[6] = color.a / 255.0f;
    f.paramCount = 7;
    return f;
}

FilterConfig FilterConfig::innerShadow(float offsetX, float offsetY, float blur, const Color& color) {
    FilterConfig f;
    f.type = FilterType::InnerShadow;
    f.parameters[0] = offsetX;
    f.parameters[1] = offsetY;
    f.parameters[2] = blur;
    f.parameters[3] = color.r / 255.0f;
    f.parameters[4] = color.g / 255.0f;
    f.parameters[5] = color.b / 255.0f;
    f.parameters[6] = color.a / 255.0f;
    f.paramCount = 7;
    return f;
}

FilterConfig FilterConfig::brightness(float factor) {
    FilterConfig f;
    f.type = FilterType::Brightness;
    f.parameters[0] = factor;
    f.paramCount = 1;
    return f;
}

FilterConfig FilterConfig::contrast(float factor) {
    FilterConfig f;
    f.type = FilterType::Contrast;
    f.parameters[0] = factor;
    f.paramCount = 1;
    return f;
}

FilterConfig FilterConfig::saturate(float factor) {
    FilterConfig f;
    f.type = FilterType::Saturate;
    f.parameters[0] = factor;
    f.paramCount = 1;
    return f;
}

FilterConfig FilterConfig::hueRotate(float degrees) {
    FilterConfig f;
    f.type = FilterType::HueRotate;
    f.parameters[0] = degrees;
    f.paramCount = 1;
    return f;
}

FilterConfig FilterConfig::invert(float amount) {
    FilterConfig f;
    f.type = FilterType::Invert;
    f.parameters[0] = std::clamp(amount, 0.0f, 1.0f);
    f.paramCount = 1;
    return f;
}

FilterConfig FilterConfig::grayscale(float amount) {
    FilterConfig f;
    f.type = FilterType::Grayscale;
    f.parameters[0] = std::clamp(amount, 0.0f, 1.0f);
    f.paramCount = 1;
    return f;
}

FilterConfig FilterConfig::sepia(float amount) {
    FilterConfig f;
    f.type = FilterType::Sepia;
    f.parameters[0] = std::clamp(amount, 0.0f, 1.0f);
    f.paramCount = 1;
    return f;
}

FilterConfig FilterConfig::opacity(float amount) {
    FilterConfig f;
    f.type = FilterType::Opacity;
    f.parameters[0] = std::clamp(amount, 0.0f, 1.0f);
    f.paramCount = 1;
    return f;
}

FilterConfig FilterConfig::colorMatrix(const float matrix[20]) {
    FilterConfig f;
    f.type = FilterType::ColorMatrix;
    std::memcpy(f.parameters, matrix, 16 * sizeof(float));
    f.paramCount = 16;
    return f;
}

// ==================================================================
// FilterPipeline
// ==================================================================

FilterPipeline::FilterPipeline() {}

FilterPipeline::~FilterPipeline() {
    shutdown();
}

bool FilterPipeline::init(GpuContext* ctx) {
    ctx_ = ctx;
    loadFilterGLFunctions();

    if (!nxf_glGenFramebuffers || !nxf_glBindFramebuffer) {
        std::cerr << "[FilterPipeline] Failed to load FBO functions" << std::endl;
        return false;
    }

    return true;
}

void FilterPipeline::shutdown() {
    if (pingPong_.fbo[0] && nxf_glDeleteFramebuffers) {
        nxf_glDeleteFramebuffers(2, pingPong_.fbo);
        pingPong_.fbo[0] = pingPong_.fbo[1] = 0;
    }
    if (pingPong_.texture[0]) {
        glDeleteTextures(2, pingPong_.texture);
        pingPong_.texture[0] = pingPong_.texture[1] = 0;
    }

    ctx_ = nullptr;
}

void FilterPipeline::addFilter(const FilterConfig& filter) {
    filters_.push_back(filter);
}

void FilterPipeline::clearFilters() {
    filters_.clear();
}

bool FilterPipeline::ensurePingPong(int width, int height) {
    if (pingPong_.width == width && pingPong_.height == height &&
        pingPong_.fbo[0] != 0) {
        return true;
    }

    // Cleanup old
    if (pingPong_.fbo[0]) {
        nxf_glDeleteFramebuffers(2, pingPong_.fbo);
        glDeleteTextures(2, pingPong_.texture);
    }

    pingPong_.width = width;
    pingPong_.height = height;

    // Create two FBOs with attached textures
    for (int i = 0; i < 2; i++) {
        glGenTextures(1, &pingPong_.texture[i]);
        glBindTexture(GL_TEXTURE_2D, pingPong_.texture[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F); // GL_CLAMP_TO_EDGE
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
        glBindTexture(GL_TEXTURE_2D, 0);

        nxf_glGenFramebuffers(1, &pingPong_.fbo[i]);
        nxf_glBindFramebuffer(0x8D40 /* GL_FRAMEBUFFER */, pingPong_.fbo[i]);
        nxf_glFramebufferTexture2D(0x8D40, 0x8CE0 /* GL_COLOR_ATTACHMENT0 */,
                                    GL_TEXTURE_2D, pingPong_.texture[i], 0);

        GLenum status = nxf_glCheckFramebufferStatus(0x8D40);
        if (status != 0x8CD5 /* GL_FRAMEBUFFER_COMPLETE */) {
            std::cerr << "[FilterPipeline] FBO " << i << " incomplete: 0x"
                      << std::hex << status << std::dec << std::endl;
            nxf_glBindFramebuffer(0x8D40, 0);
            return false;
        }
    }

    nxf_glBindFramebuffer(0x8D40, 0);
    pingPong_.current = 0;
    return true;
}

void FilterPipeline::computeGaussianKernel(float radius, std::vector<float>& weights,
                                            std::vector<float>& offsets, int& sampleCount) {
    int kernelSize = static_cast<int>(std::ceil(radius * 2.0f)) | 1; // Ensure odd
    kernelSize = std::min(kernelSize, 63); // Cap
    sampleCount = (kernelSize + 1) / 2;

    float sigma = radius / 2.0f;
    if (sigma < 0.001f) sigma = 0.001f;

    weights.resize(static_cast<size_t>(sampleCount));
    offsets.resize(static_cast<size_t>(sampleCount));

    float totalWeight = 0;
    for (int i = 0; i < sampleCount; i++) {
        float x = static_cast<float>(i);
        float w = std::exp(-(x * x) / (2.0f * sigma * sigma));
        weights[static_cast<size_t>(i)] = w;
        offsets[static_cast<size_t>(i)] = x;
        totalWeight += (i == 0) ? w : w * 2.0f;
    }

    // Normalize
    for (int i = 0; i < sampleCount; i++) {
        weights[static_cast<size_t>(i)] /= totalWeight;
    }
}

uint32_t FilterPipeline::applyGaussianBlur(uint32_t sourceTexture, int width, int height,
                                            float radius, bool horizontal) {
    if (!ensurePingPong(width, height)) return sourceTexture;

    std::vector<float> weights, offsets;
    int sampleCount;
    computeGaussianKernel(radius, weights, offsets, sampleCount);

    // Bind destination FBO
    nxf_glBindFramebuffer(0x8D40, pingPong_.currentFBO());
    glViewport(0, 0, width, height);

    // Bind source texture
    glBindTexture(GL_TEXTURE_2D, sourceTexture);

    // Use blur shader
    if (blurShader_ && blurShader_->isValid()) {
        blurShader_->bind();
        blurShader_->setUniform1i("uTexture", 0);

        float dir[2] = {0, 0};
        if (horizontal) dir[0] = 1.0f / static_cast<float>(width);
        else dir[1] = 1.0f / static_cast<float>(height);

        blurShader_->setUniform2f("uDirection", dir[0], dir[1]);
        blurShader_->setUniform1i("uSampleCount", sampleCount);
        for (int wi = 0; wi < sampleCount && wi < 32; wi++) {
            char wname[32], oname[32];
            snprintf(wname, sizeof(wname), "uWeights[%d]", wi);
            snprintf(oname, sizeof(oname), "uOffsets[%d]", wi);
            blurShader_->setUniform1f(wname, weights[static_cast<size_t>(wi)]);
            blurShader_->setUniform1f(oname, offsets[static_cast<size_t>(wi)]);
        }
    }

    renderFullscreenQuad();

    if (blurShader_) ShaderProgram::unbind();
    nxf_glBindFramebuffer(0x8D40, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    uint32_t result = pingPong_.currentTexture();
    pingPong_.swap();
    return result;
}

uint32_t FilterPipeline::applyColorMatrix(uint32_t sourceTexture, int width, int height,
                                           const float matrix[20]) {
    if (!ensurePingPong(width, height)) return sourceTexture;

    nxf_glBindFramebuffer(0x8D40, pingPong_.currentFBO());
    glViewport(0, 0, width, height);
    glBindTexture(GL_TEXTURE_2D, sourceTexture);

    if (colorMatrixShader_ && colorMatrixShader_->isValid()) {
        colorMatrixShader_->bind();
        colorMatrixShader_->setUniform1i("uTexture", 0);
        colorMatrixShader_->setUniformMat4("uColorMatrix", matrix);
        // Bias vector (last column of 4x5 matrix)
        float bias[4] = {matrix[4], matrix[9], matrix[14], matrix[19]};
        colorMatrixShader_->setUniform4f("uBias", bias[0], bias[1], bias[2], bias[3]);
    }

    renderFullscreenQuad();

    if (colorMatrixShader_) ShaderProgram::unbind();
    nxf_glBindFramebuffer(0x8D40, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    uint32_t result = pingPong_.currentTexture();
    pingPong_.swap();
    return result;
}

uint32_t FilterPipeline::applyBrightnessContrast(uint32_t sourceTexture, int width, int height,
                                                  float brightness, float contrast) {
    // Build a color matrix for brightness/contrast
    float b = brightness;
    float c = contrast;
    float t = (1.0f - c) * 0.5f + b;

    float matrix[20] = {
        c, 0, 0, 0, t,
        0, c, 0, 0, t,
        0, 0, c, 0, t,
        0, 0, 0, 1, 0
    };

    return applyColorMatrix(sourceTexture, width, height, matrix);
}

uint32_t FilterPipeline::apply(uint32_t sourceTexture, int width, int height) {
    if (filters_.empty()) return sourceTexture;
    if (!ensurePingPong(width, height)) return sourceTexture;

    uint32_t current = sourceTexture;

    for (const auto& filter : filters_) {
        switch (filter.type) {
            case FilterType::GaussianBlur: {
                float radius = filter.parameters[0];
                // Two-pass separable blur
                current = applyGaussianBlur(current, width, height, radius, true);
                current = applyGaussianBlur(current, width, height, radius, false);
                break;
            }
            case FilterType::BoxBlur: {
                float radius = filter.parameters[0];
                // Box blur via 3 passes of 1/3 radius (approximation)
                float pass = radius / 3.0f;
                for (int i = 0; i < 3; i++) {
                    current = applyGaussianBlur(current, width, height, pass, true);
                    current = applyGaussianBlur(current, width, height, pass, false);
                }
                break;
            }
            case FilterType::Brightness:
                current = applyBrightnessContrast(current, width, height,
                                                   filter.parameters[0] - 1.0f, 1.0f);
                break;
            case FilterType::Contrast:
                current = applyBrightnessContrast(current, width, height,
                                                   0.0f, filter.parameters[0]);
                break;
            case FilterType::Grayscale: {
                float a = filter.parameters[0];
                float ia = 1.0f - a;
                float matrix[20] = {
                    0.2126f + 0.7874f * ia, 0.7152f - 0.7152f * ia, 0.0722f - 0.0722f * ia, 0, 0,
                    0.2126f - 0.2126f * ia, 0.7152f + 0.2848f * ia, 0.0722f - 0.0722f * ia, 0, 0,
                    0.2126f - 0.2126f * ia, 0.7152f - 0.7152f * ia, 0.0722f + 0.9278f * ia, 0, 0,
                    0,                       0,                       0,                       1, 0
                };
                current = applyColorMatrix(current, width, height, matrix);
                break;
            }
            case FilterType::Sepia: {
                float a = filter.parameters[0];
                float ia = 1.0f - a;
                float matrix[20] = {
                    0.393f + 0.607f * ia, 0.769f - 0.769f * ia, 0.189f - 0.189f * ia, 0, 0,
                    0.349f - 0.349f * ia, 0.686f + 0.314f * ia, 0.168f - 0.168f * ia, 0, 0,
                    0.272f - 0.272f * ia, 0.534f - 0.534f * ia, 0.131f + 0.869f * ia, 0, 0,
                    0,                     0,                     0,                     1, 0
                };
                current = applyColorMatrix(current, width, height, matrix);
                break;
            }
            case FilterType::Invert: {
                float a = filter.parameters[0];
                float matrix[20] = {
                    1.0f - 2.0f * a, 0, 0, 0, a,
                    0, 1.0f - 2.0f * a, 0, 0, a,
                    0, 0, 1.0f - 2.0f * a, 0, a,
                    0, 0, 0,               1, 0
                };
                current = applyColorMatrix(current, width, height, matrix);
                break;
            }
            case FilterType::HueRotate: {
                float deg = filter.parameters[0];
                float rad = deg * static_cast<float>(M_PI) / 180.0f;
                float cosA = std::cos(rad);
                float sinA = std::sin(rad);

                float matrix[20] = {
                    0.213f + cosA * 0.787f - sinA * 0.213f,
                    0.715f - cosA * 0.715f - sinA * 0.715f,
                    0.072f - cosA * 0.072f + sinA * 0.928f, 0, 0,
                    0.213f - cosA * 0.213f + sinA * 0.143f,
                    0.715f + cosA * 0.285f + sinA * 0.140f,
                    0.072f - cosA * 0.072f - sinA * 0.283f, 0, 0,
                    0.213f - cosA * 0.213f - sinA * 0.787f,
                    0.715f - cosA * 0.715f + sinA * 0.715f,
                    0.072f + cosA * 0.928f + sinA * 0.072f, 0, 0,
                    0, 0, 0, 1, 0
                };
                current = applyColorMatrix(current, width, height, matrix);
                break;
            }
            case FilterType::Saturate: {
                float s = filter.parameters[0];
                float matrix[20] = {
                    0.213f + 0.787f * s, 0.715f - 0.715f * s, 0.072f - 0.072f * s, 0, 0,
                    0.213f - 0.213f * s, 0.715f + 0.285f * s, 0.072f - 0.072f * s, 0, 0,
                    0.213f - 0.213f * s, 0.715f - 0.715f * s, 0.072f + 0.928f * s, 0, 0,
                    0,                    0,                    0,                    1, 0
                };
                current = applyColorMatrix(current, width, height, matrix);
                break;
            }
            case FilterType::Opacity: {
                float a = filter.parameters[0];
                float matrix[20] = {
                    1, 0, 0, 0, 0,
                    0, 1, 0, 0, 0,
                    0, 0, 1, 0, 0,
                    0, 0, 0, a, 0
                };
                current = applyColorMatrix(current, width, height, matrix);
                break;
            }
            case FilterType::ColorMatrix:
                current = applyColorMatrix(current, width, height, filter.parameters);
                break;
            default:
                break;
        }
    }

    return current;
}

void FilterPipeline::renderFullscreenQuad() {
    // Simple fullscreen triangle strip
    static const float vertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f
    };

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 4 * sizeof(float), vertices);
    glTexCoordPointer(2, GL_FLOAT, 4 * sizeof(float), vertices + 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}

} // namespace NXRender
