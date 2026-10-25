// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "webgl_context.h"
#include "nxgfx/gl_includes.h"
#ifdef __linux__
#include <GL/glx.h>
#elif defined(__APPLE__)
#include <OpenGL/OpenGL.h>
#endif
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <regex>
#include <type_traits>

#ifndef APIENTRY
#define APIENTRY
#endif

namespace NXRender {

// ==================================================================
// GL 2.0+ function loading (same pattern as shader.cpp)
// ==================================================================

namespace {

typedef GLuint (APIENTRY *PFN_CreateShader)(GLenum);
typedef void   (APIENTRY *PFN_ShaderSource)(GLuint, GLsizei, const GLchar**, const GLint*);
typedef void   (APIENTRY *PFN_CompileShader)(GLuint);
typedef void   (APIENTRY *PFN_GetShaderiv)(GLuint, GLenum, GLint*);
typedef void   (APIENTRY *PFN_GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef GLuint (APIENTRY *PFN_CreateProgram)();
typedef void   (APIENTRY *PFN_AttachShader)(GLuint, GLuint);
typedef void   (APIENTRY *PFN_DetachShader)(GLuint, GLuint);
typedef void   (APIENTRY *PFN_BindAttribLocation)(GLuint, GLuint, const GLchar*);
typedef void   (APIENTRY *PFN_LinkProgram)(GLuint);
typedef void   (APIENTRY *PFN_GetProgramiv)(GLuint, GLenum, GLint*);
typedef void   (APIENTRY *PFN_GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void   (APIENTRY *PFN_UseProgram)(GLuint);
typedef void   (APIENTRY *PFN_DeleteShader)(GLuint);
typedef void   (APIENTRY *PFN_DeleteProgram)(GLuint);
typedef GLint  (APIENTRY *PFN_GetUniformLocation)(GLuint, const GLchar*);
typedef void   (APIENTRY *PFN_UniformNfv)(GLint, GLsizei, const GLfloat*);
typedef void   (APIENTRY *PFN_UniformNiv)(GLint, GLsizei, const GLint*);
typedef void   (APIENTRY *PFN_UniformMatrixNfv)(GLint, GLsizei, GLboolean, const GLfloat*);
typedef void   (APIENTRY *PFN_EnableVertexAttribArray)(GLuint);
typedef void   (APIENTRY *PFN_DisableVertexAttribArray)(GLuint);
typedef void   (APIENTRY *PFN_VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
typedef void   (APIENTRY *PFN_VertexAttrib4f)(GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void   (APIENTRY *PFN_GetVertexAttribiv)(GLuint, GLenum, GLint*);
typedef void   (APIENTRY *PFN_GenBuffers)(GLsizei, GLuint*);
typedef void   (APIENTRY *PFN_DeleteBuffers)(GLsizei, const GLuint*);
typedef void   (APIENTRY *PFN_BindBuffer)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_BufferData)(GLenum, ptrdiff_t, const void*, GLenum);
typedef void   (APIENTRY *PFN_BufferSubData)(GLenum, ptrdiff_t, ptrdiff_t, const void*);
typedef void   (APIENTRY *PFN_GenFramebuffers)(GLsizei, GLuint*);
typedef void   (APIENTRY *PFN_DeleteFramebuffers)(GLsizei, const GLuint*);
typedef void   (APIENTRY *PFN_BindFramebuffer)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void   (APIENTRY *PFN_FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY *PFN_CheckFramebufferStatus)(GLenum);
typedef void   (APIENTRY *PFN_GenRenderbuffers)(GLsizei, GLuint*);
typedef void   (APIENTRY *PFN_DeleteRenderbuffers)(GLsizei, const GLuint*);
typedef void   (APIENTRY *PFN_BindRenderbuffer)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void   (APIENTRY *PFN_GenerateMipmap)(GLenum);
typedef void   (APIENTRY *PFN_ActiveTexture)(GLenum);
typedef void   (APIENTRY *PFN_BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
typedef void   (APIENTRY *PFN_BlendEquationSeparate)(GLenum, GLenum);
typedef void   (APIENTRY *PFN_BlendColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void   (APIENTRY *PFN_StencilFuncSeparate)(GLenum, GLenum, GLint, GLuint);
typedef void   (APIENTRY *PFN_StencilOpSeparate)(GLenum, GLenum, GLenum, GLenum);
typedef void   (APIENTRY *PFN_StencilMaskSeparate)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_BindVertexArray)(GLuint);
typedef void   (APIENTRY *PFN_RenderbufferStorageMultisample)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void   (APIENTRY *PFN_BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

struct GLFunctions {
    PFN_CreateShader CreateShader = nullptr;
    PFN_ShaderSource ShaderSource = nullptr;
    PFN_CompileShader CompileShader = nullptr;
    PFN_GetShaderiv GetShaderiv = nullptr;
    PFN_GetShaderInfoLog GetShaderInfoLog = nullptr;
    PFN_CreateProgram CreateProgram = nullptr;
    PFN_AttachShader AttachShader = nullptr;
    PFN_DetachShader DetachShader = nullptr;
    PFN_BindAttribLocation BindAttribLocation = nullptr;
    PFN_LinkProgram LinkProgram = nullptr;
    PFN_GetProgramiv GetProgramiv = nullptr;
    PFN_GetProgramInfoLog GetProgramInfoLog = nullptr;
    PFN_UseProgram UseProgram = nullptr;
    PFN_DeleteShader DeleteShader = nullptr;
    PFN_DeleteProgram DeleteProgram = nullptr;
    PFN_GetUniformLocation GetUniformLocation = nullptr;
    PFN_UniformNfv Uniformfv[4] = {};
    PFN_UniformNiv Uniformiv[4] = {};
    PFN_UniformMatrixNfv UniformMatrixfv[3] = {};  // 2, 3, 4
    PFN_EnableVertexAttribArray EnableVertexAttribArray = nullptr;
    PFN_DisableVertexAttribArray DisableVertexAttribArray = nullptr;
    PFN_VertexAttribPointer VertexAttribPointer = nullptr;
    PFN_VertexAttrib4f VertexAttrib4f = nullptr;
    PFN_GetVertexAttribiv GetVertexAttribiv = nullptr;
    PFN_GenBuffers GenBuffers = nullptr;
    PFN_DeleteBuffers DeleteBuffers = nullptr;
    PFN_BindBuffer BindBuffer = nullptr;
    PFN_BufferData BufferData = nullptr;
    PFN_BufferSubData BufferSubData = nullptr;
    PFN_GenFramebuffers GenFramebuffers = nullptr;
    PFN_DeleteFramebuffers DeleteFramebuffers = nullptr;
    PFN_BindFramebuffer BindFramebuffer = nullptr;
    PFN_FramebufferTexture2D FramebufferTexture2D = nullptr;
    PFN_FramebufferRenderbuffer FramebufferRenderbuffer = nullptr;
    PFN_CheckFramebufferStatus CheckFramebufferStatus = nullptr;
    PFN_GenRenderbuffers GenRenderbuffers = nullptr;
    PFN_DeleteRenderbuffers DeleteRenderbuffers = nullptr;
    PFN_BindRenderbuffer BindRenderbuffer = nullptr;
    PFN_RenderbufferStorage RenderbufferStorage = nullptr;
    PFN_GenerateMipmap GenerateMipmap = nullptr;
    PFN_ActiveTexture ActiveTexture = nullptr;
    PFN_BlendFuncSeparate BlendFuncSeparate = nullptr;
    PFN_BlendEquationSeparate BlendEquationSeparate = nullptr;
    PFN_BlendColor BlendColor = nullptr;
    PFN_StencilFuncSeparate StencilFuncSeparate = nullptr;
    PFN_StencilOpSeparate StencilOpSeparate = nullptr;
    PFN_StencilMaskSeparate StencilMaskSeparate = nullptr;
    PFN_BindVertexArray BindVertexArray = nullptr;  // optional
    PFN_RenderbufferStorageMultisample RenderbufferStorageMultisample = nullptr;  // optional (antialias)
    PFN_BlitFramebuffer BlitFramebuffer = nullptr;  // optional (antialias)
    bool loaded = false;
};

GLFunctions g;

void* getProc(const char* name) {
#ifdef __linux__
    return reinterpret_cast<void*>(glXGetProcAddress(reinterpret_cast<const GLubyte*>(name)));
#elif defined(_WIN32)
    return reinterpret_cast<void*>(wglGetProcAddress(name));
#elif defined(__APPLE__)
    return nxAppleGetProcAddress(name);
#else
    (void)name;
    return nullptr;
#endif
}

// Loads the entry points once a context is current; false when a required one is missing
bool loadFunctions() {
    if (g.loaded) return true;
    #define LOAD(field, name) g.field = reinterpret_cast<std::remove_reference_t<decltype(g.field)>>(getProc(name))
    LOAD(CreateShader, "glCreateShader");
    LOAD(ShaderSource, "glShaderSource");
    LOAD(CompileShader, "glCompileShader");
    LOAD(GetShaderiv, "glGetShaderiv");
    LOAD(GetShaderInfoLog, "glGetShaderInfoLog");
    LOAD(CreateProgram, "glCreateProgram");
    LOAD(AttachShader, "glAttachShader");
    LOAD(DetachShader, "glDetachShader");
    LOAD(BindAttribLocation, "glBindAttribLocation");
    LOAD(LinkProgram, "glLinkProgram");
    LOAD(GetProgramiv, "glGetProgramiv");
    LOAD(GetProgramInfoLog, "glGetProgramInfoLog");
    LOAD(UseProgram, "glUseProgram");
    LOAD(DeleteShader, "glDeleteShader");
    LOAD(DeleteProgram, "glDeleteProgram");
    LOAD(GetUniformLocation, "glGetUniformLocation");
    LOAD(Uniformfv[0], "glUniform1fv");
    LOAD(Uniformfv[1], "glUniform2fv");
    LOAD(Uniformfv[2], "glUniform3fv");
    LOAD(Uniformfv[3], "glUniform4fv");
    LOAD(Uniformiv[0], "glUniform1iv");
    LOAD(Uniformiv[1], "glUniform2iv");
    LOAD(Uniformiv[2], "glUniform3iv");
    LOAD(Uniformiv[3], "glUniform4iv");
    LOAD(UniformMatrixfv[0], "glUniformMatrix2fv");
    LOAD(UniformMatrixfv[1], "glUniformMatrix3fv");
    LOAD(UniformMatrixfv[2], "glUniformMatrix4fv");
    LOAD(EnableVertexAttribArray, "glEnableVertexAttribArray");
    LOAD(DisableVertexAttribArray, "glDisableVertexAttribArray");
    LOAD(VertexAttribPointer, "glVertexAttribPointer");
    LOAD(VertexAttrib4f, "glVertexAttrib4f");
    LOAD(GetVertexAttribiv, "glGetVertexAttribiv");
    LOAD(GenBuffers, "glGenBuffers");
    LOAD(DeleteBuffers, "glDeleteBuffers");
    LOAD(BindBuffer, "glBindBuffer");
    LOAD(BufferData, "glBufferData");
    LOAD(BufferSubData, "glBufferSubData");
    LOAD(GenFramebuffers, "glGenFramebuffers");
    LOAD(DeleteFramebuffers, "glDeleteFramebuffers");
    LOAD(BindFramebuffer, "glBindFramebuffer");
    LOAD(FramebufferTexture2D, "glFramebufferTexture2D");
    LOAD(FramebufferRenderbuffer, "glFramebufferRenderbuffer");
    LOAD(CheckFramebufferStatus, "glCheckFramebufferStatus");
    LOAD(GenRenderbuffers, "glGenRenderbuffers");
    LOAD(DeleteRenderbuffers, "glDeleteRenderbuffers");
    LOAD(BindRenderbuffer, "glBindRenderbuffer");
    LOAD(RenderbufferStorage, "glRenderbufferStorage");
    LOAD(GenerateMipmap, "glGenerateMipmap");
    LOAD(ActiveTexture, "glActiveTexture");
    LOAD(BlendFuncSeparate, "glBlendFuncSeparate");
    LOAD(BlendEquationSeparate, "glBlendEquationSeparate");
    LOAD(BlendColor, "glBlendColor");
    LOAD(StencilFuncSeparate, "glStencilFuncSeparate");
    LOAD(StencilOpSeparate, "glStencilOpSeparate");
    LOAD(StencilMaskSeparate, "glStencilMaskSeparate");
    LOAD(BindVertexArray, "glBindVertexArray");
    LOAD(RenderbufferStorageMultisample, "glRenderbufferStorageMultisample");
    LOAD(BlitFramebuffer, "glBlitFramebuffer");
    if (!g.RenderbufferStorageMultisample) LOAD(RenderbufferStorageMultisample, "glRenderbufferStorageMultisampleEXT");
    if (!g.BlitFramebuffer) LOAD(BlitFramebuffer, "glBlitFramebufferEXT");
    #undef LOAD
    // Older drivers expose the framebuffer object calls only as EXT_framebuffer_object
    if (!g.GenFramebuffers) {
        #define LOAD_EXT(field, name) if (!g.field) g.field = reinterpret_cast<std::remove_reference_t<decltype(g.field)>>(getProc(name))
        LOAD_EXT(GenFramebuffers, "glGenFramebuffersEXT");
        LOAD_EXT(DeleteFramebuffers, "glDeleteFramebuffersEXT");
        LOAD_EXT(BindFramebuffer, "glBindFramebufferEXT");
        LOAD_EXT(FramebufferTexture2D, "glFramebufferTexture2DEXT");
        LOAD_EXT(FramebufferRenderbuffer, "glFramebufferRenderbufferEXT");
        LOAD_EXT(CheckFramebufferStatus, "glCheckFramebufferStatusEXT");
        LOAD_EXT(GenRenderbuffers, "glGenRenderbuffersEXT");
        LOAD_EXT(DeleteRenderbuffers, "glDeleteRenderbuffersEXT");
        LOAD_EXT(BindRenderbuffer, "glBindRenderbufferEXT");
        LOAD_EXT(RenderbufferStorage, "glRenderbufferStorageEXT");
        LOAD_EXT(GenerateMipmap, "glGenerateMipmapEXT");
        #undef LOAD_EXT
    }
    g.loaded = g.CreateShader && g.ShaderSource && g.CompileShader && g.LinkProgram && g.UseProgram &&
               g.GetUniformLocation && g.VertexAttribPointer && g.GenBuffers && g.BufferData &&
               g.GenFramebuffers && g.BindFramebuffer && g.FramebufferTexture2D && g.ActiveTexture &&
               g.GenRenderbuffers && g.RenderbufferStorage && g.FramebufferRenderbuffer;
    return g.loaded;
}

// GL constants that gl.h of GL 1.1 lacks
constexpr GLenum kFramebuffer = 0x8D40;
constexpr GLenum kRenderbuffer = 0x8D41;
constexpr GLenum kColorAttachment0 = 0x8CE0;
constexpr GLenum kDepthAttachment = 0x8D00;
constexpr GLenum kStencilAttachment = 0x8D20;
constexpr GLenum kDepth24Stencil8 = 0x88F0;
constexpr GLenum kDepthComponent24 = 0x81A6;
constexpr GLenum kFramebufferBinding = 0x8CA6;
constexpr GLenum kReadFramebuffer = 0x8CA8;
constexpr GLenum kDrawFramebuffer = 0x8CA9;
constexpr GLenum kReadFramebufferBinding = 0x8CAA;
constexpr GLenum kMaxSamples = 0x8D57;
constexpr GLenum kRenderbufferBinding = 0x8CA7;
constexpr GLenum kFramebufferComplete = 0x8CD5;
constexpr GLenum kCurrentProgram = 0x8B8D;
constexpr GLenum kArrayBufferBinding = 0x8894;
constexpr GLenum kElementArrayBufferBinding = 0x8895;
constexpr GLenum kActiveTexture = 0x84E0;
constexpr GLenum kTextureBindingCubeMap = 0x8514;
constexpr GLenum kVertexArrayBinding = 0x85B5;
constexpr GLenum kVertexProgramPointSize = 0x8642;
constexpr GLenum kPointSprite = 0x8861;
constexpr GLenum kCompileStatus = 0x8B81;
constexpr GLenum kLinkStatus = 0x8B82;
constexpr GLenum kInfoLogLength = 0x8B84;
constexpr GLenum kVertexAttribArrayEnabled = 0x8622;
constexpr GLenum kFront = 0x0404;
constexpr GLenum kBack = 0x0405;
constexpr GLenum kFrontAndBack = 0x0408;
constexpr int kMaxUnits = 8;

int32_t asInt(uint32_t v) { return static_cast<int32_t>(v); }

// The browser's GL state around a flush
class SavedGLState {
public:
    SavedGLState() {
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
        glGetIntegerv(kCurrentProgram, &program_);
        glGetIntegerv(kArrayBufferBinding, &arrayBuffer_);
        glGetIntegerv(kFramebufferBinding, &framebuffer_);
        readFramebuffer_ = framebuffer_;
        if (g.BlitFramebuffer) glGetIntegerv(kReadFramebufferBinding, &readFramebuffer_);
        glGetIntegerv(kRenderbufferBinding, &renderbuffer_);
        glGetIntegerv(kActiveTexture, &activeTexture_);
        if (g.BindVertexArray) {
            glGetIntegerv(kVertexArrayBinding, &vertexArray_);
            g.BindVertexArray(0);
        }
        // Element buffer binding belongs to the vertex array: read it on VAO 0
        glGetIntegerv(kElementArrayBufferBinding, &elementBuffer_);
        for (int unit = 0; unit < kMaxUnits; ++unit) {
            g.ActiveTexture(GL::TEXTURE0 + unit);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &textures2D_[unit]);
            glGetIntegerv(kTextureBindingCubeMap, &texturesCube_[unit]);
        }
        for (GLuint i = 0; i < 16 && g.GetVertexAttribiv; ++i) {
            g.GetVertexAttribiv(i, kVertexAttribArrayEnabled, &attribEnabled_[i]);
        }
        while (glGetError() != GL_NO_ERROR) {}
    }

    ~SavedGLState() {
        for (int unit = 0; unit < kMaxUnits; ++unit) {
            g.ActiveTexture(GL::TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, textures2D_[unit]);
            glBindTexture(GL::TEXTURE_CUBE_MAP, texturesCube_[unit]);
        }
        g.ActiveTexture(activeTexture_);
        for (GLuint i = 0; i < 16; ++i) {
            if (attribEnabled_[i]) g.EnableVertexAttribArray(i);
            else g.DisableVertexAttribArray(i);
        }
        g.BindBuffer(GL::ELEMENT_ARRAY_BUFFER, elementBuffer_);
        if (g.BindVertexArray) g.BindVertexArray(vertexArray_);
        g.BindBuffer(GL::ARRAY_BUFFER, arrayBuffer_);
        g.UseProgram(program_);
        g.BindRenderbuffer(kRenderbuffer, renderbuffer_);
        g.BindFramebuffer(kFramebuffer, framebuffer_);
        if (readFramebuffer_ != framebuffer_) g.BindFramebuffer(kReadFramebuffer, readFramebuffer_);
        glPopClientAttrib();
        glPopAttrib();
    }

private:
    GLint program_ = 0, arrayBuffer_ = 0, elementBuffer_ = 0, framebuffer_ = 0, readFramebuffer_ = 0, renderbuffer_ = 0;
    GLint activeTexture_ = GL::TEXTURE0, vertexArray_ = 0;
    GLint textures2D_[kMaxUnits] = {}, texturesCube_[kMaxUnits] = {};
    GLint attribEnabled_[16] = {};
};

std::string stripComments(const std::string& source) {
    std::string out;
    out.reserve(source.size());
    for (size_t i = 0; i < source.size(); ++i) {
        if (source.compare(i, 2, "//") == 0) {
            while (i < source.size() && source[i] != '\n') ++i;
            if (i < source.size()) out += '\n';
        } else if (source.compare(i, 2, "/*") == 0) {
            size_t end = source.find("*/", i + 2);
            // Keep line breaks so line numbers in driver logs still match
            for (size_t j = i; j < std::min(end, source.size()); ++j) {
                if (source[j] == '\n') out += '\n';
            }
            out += ' ';
            if (end == std::string::npos) break;
            i = end + 1;
        } else {
            out += source[i];
        }
    }
    return out;
}

} // namespace

// ==================================================================
// Construction
// ==================================================================

WebGLRenderingContext::WebGLRenderingContext(int width, int height, WebGLContextAttributes attributes)
    : width_(std::max(width, 0)), height_(std::max(height, 0)), attributes_(attributes) {
    state_.viewport[2] = state_.scissor[2] = width_;
    state_.viewport[3] = state_.scissor[3] = height_;
    state_.caps[GL::DITHER] = true;
}

WebGLRenderingContext::~WebGLRenderingContext() {
    if (!glAvailable() || !g.loaded) return;
    for (auto& [handle, name] : shaders_) g.DeleteShader(name);
    for (auto& [handle, name] : programs_) g.DeleteProgram(name);
    for (auto& [handle, name] : buffers_) g.DeleteBuffers(1, &name);
    for (auto& [handle, name] : textures_) glDeleteTextures(1, &name);
    releaseDrawingBuffer();
}

bool WebGLRenderingContext::glAvailable() {
#ifdef _WIN32
    return wglGetCurrentContext() != nullptr;
#elif defined(__linux__)
    return glXGetCurrentContext() != nullptr;
#elif defined(__APPLE__)
    return CGLGetCurrentContext() != nullptr;
#else
    return false;
#endif
}

void WebGLRenderingContext::resize(int width, int height) {
    width_ = std::max(width, 0);
    height_ = std::max(height, 0);
    // Resizing resets the viewport only if the page never set it, but always clears
    needsClear_ = true;
}

void WebGLRenderingContext::record(WebGLCommand command) {
    pendingBytes_ += sizeof(WebGLCommand) + command.data.size() + command.text.size();
    pending_.push_back(std::move(command));
}

WebGLRenderingContext::CompileResult WebGLRenderingContext::shaderResult(uint32_t shader) const {
    auto it = shaderResults_.find(shader);
    return it == shaderResults_.end() ? CompileResult{} : it->second;
}

WebGLRenderingContext::CompileResult WebGLRenderingContext::programResult(uint32_t program) const {
    auto it = programResults_.find(program);
    return it == programResults_.end() ? CompileResult{} : it->second;
}

// ==================================================================
// GLSL
// ==================================================================

std::string WebGLRenderingContext::translateShader(const std::string& source) {
    std::string body = stripComments(source);
    // GLSL 1.20 has no precision statements; #version 100 and OES extensions
    // (standard derivatives are core in 1.20) go away too
    static const std::regex precision(R"(\bprecision\s+(lowp|mediump|highp)\s+\w+\s*;)");
    static const std::regex version(R"((^|\n)[ \t]*#[ \t]*version[^\n]*)");
    static const std::regex extension(R"((^|\n)[ \t]*#[ \t]*extension[ \t]+GL_OES_\w+[^\n]*)");
    body = std::regex_replace(body, precision, "");
    body = std::regex_replace(body, version, "$1");
    body = std::regex_replace(body, extension, "$1");
    return "#version 120\n"
           "#define lowp\n"
           "#define mediump\n"
           "#define highp\n"
           "#line 0\n" + body;
}

std::vector<std::string> WebGLRenderingContext::declaredNames(const std::string& source, const std::string& qualifier) {
    std::vector<std::string> names;
    const std::string body = stripComments(source);
    // qualifier [precision] type name[, name][[n]] ;
    const std::regex declaration("\\b" + qualifier + R"(\s+(?:(?:lowp|mediump|highp)\s+)?\w+\s+([^;{}()]+);)");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), declaration); it != std::sregex_iterator(); ++it) {
        std::string list = (*it)[1].str();
        size_t start = 0;
        while (start <= list.size()) {
            size_t comma = list.find(',', start);
            std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            size_t a = 0;
            while (a < item.size() && std::isspace(static_cast<unsigned char>(item[a]))) ++a;
            size_t b = a;
            while (b < item.size() && (std::isalnum(static_cast<unsigned char>(item[b])) || item[b] == '_')) ++b;
            if (b > a && std::find(names.begin(), names.end(), item.substr(a, b - a)) == names.end()) {
                names.push_back(item.substr(a, b - a));
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    return names;
}

// ==================================================================
// Drawing buffer
// ==================================================================

void WebGLRenderingContext::releaseDrawingBuffer() {
    if (fbo_) g.DeleteFramebuffers(1, &fbo_);
    if (msFbo_) g.DeleteFramebuffers(1, &msFbo_);
    if (msColor_) g.DeleteRenderbuffers(1, &msColor_);
    if (depthStencil_) g.DeleteRenderbuffers(1, &depthStencil_);
    if (colorTexture_) glDeleteTextures(1, &colorTexture_);
    fbo_ = msFbo_ = msColor_ = depthStencil_ = colorTexture_ = 0;
    bufferWidth_ = bufferHeight_ = 0;
}

void WebGLRenderingContext::ensureDrawingBuffer() {
    if (fbo_ && bufferWidth_ == width_ && bufferHeight_ == height_) return;
    releaseDrawingBuffer();

    glGenTextures(1, &colorTexture_);
    g.ActiveTexture(GL::TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, colorTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, attributes_.alpha ? GL_RGBA8 : GL_RGB8, width_, height_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    g.GenFramebuffers(1, &fbo_);
    g.BindFramebuffer(kFramebuffer, fbo_);
    g.FramebufferTexture2D(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, colorTexture_, 0);

    // antialias: draw into a 4x multisampled framebuffer, resolved into the texture after each flush
    GLint samples = 0;
    if (attributes_.antialias && g.RenderbufferStorageMultisample && g.BlitFramebuffer) {
        glGetIntegerv(kMaxSamples, &samples);
        samples = std::min(samples, 4);
    }
    auto storage = [&](GLenum format) {
        if (samples > 1) g.RenderbufferStorageMultisample(kRenderbuffer, samples, format, width_, height_);
        else g.RenderbufferStorage(kRenderbuffer, format, width_, height_);
    };
    if (samples > 1) {
        g.GenRenderbuffers(1, &msColor_);
        g.BindRenderbuffer(kRenderbuffer, msColor_);
        storage(attributes_.alpha ? GL_RGBA8 : GL_RGB8);
        g.GenFramebuffers(1, &msFbo_);
        g.BindFramebuffer(kFramebuffer, msFbo_);
        g.FramebufferRenderbuffer(kFramebuffer, kColorAttachment0, kRenderbuffer, msColor_);
    }
    if (attributes_.depth || attributes_.stencil) {
        g.GenRenderbuffers(1, &depthStencil_);
        g.BindRenderbuffer(kRenderbuffer, depthStencil_);
        storage(attributes_.stencil ? kDepth24Stencil8 : kDepthComponent24);
        g.FramebufferRenderbuffer(kFramebuffer, kDepthAttachment, kRenderbuffer, depthStencil_);
        if (attributes_.stencil) {
            g.FramebufferRenderbuffer(kFramebuffer, kStencilAttachment, kRenderbuffer, depthStencil_);
        }
    }
    if (g.CheckFramebufferStatus && g.CheckFramebufferStatus(kFramebuffer) != kFramebufferComplete) {
        std::cerr << "[WebGL] drawing buffer " << width_ << "x" << height_ << " is incomplete" << std::endl;
    }
    bufferWidth_ = width_;
    bufferHeight_ = height_;
    needsClear_ = true;
}

// Transparent black, depth 1, stencil 0, whatever masks and scissor the page set
void WebGLRenderingContext::clearDrawingBuffer() {
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFFFFFFFF);
    glClearColor(0, 0, 0, 0);
    glClearDepth(1);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

// ==================================================================
// Replay
// ==================================================================

uint32_t WebGLRenderingContext::glName(const std::unordered_map<uint32_t, uint32_t>& names, uint32_t handle) const {
    auto it = names.find(handle);
    return it == names.end() ? 0 : it->second;
}

int WebGLRenderingContext::uniformLocation(uint32_t program, const std::string& name) {
    auto key = std::make_pair(program, name);
    auto it = uniformLocations_.find(key);
    if (it != uniformLocations_.end()) return it->second;
    GLuint id = glName(programs_, program);
    int location = id ? g.GetUniformLocation(id, name.c_str()) : -1;
    uniformLocations_[key] = location;
    return location;
}

void WebGLRenderingContext::applyState() {
    const State& s = state_;
    g.UseProgram(glName(programs_, s.program));
    for (GLuint i = 0; i < 16; ++i) {
        const AttribState& a = s.attribs[i];
        if (g.VertexAttrib4f) g.VertexAttrib4f(i, a.value[0], a.value[1], a.value[2], a.value[3]);
        if (a.enabled) {
            g.BindBuffer(GL::ARRAY_BUFFER, glName(buffers_, a.buffer));
            g.VertexAttribPointer(i, a.size, a.type, a.normalized, a.stride,
                                  reinterpret_cast<const void*>(static_cast<uintptr_t>(a.offset)));
            g.EnableVertexAttribArray(i);
        } else {
            g.DisableVertexAttribArray(i);
        }
    }
    g.BindBuffer(GL::ARRAY_BUFFER, glName(buffers_, s.arrayBuffer));
    g.BindBuffer(GL::ELEMENT_ARRAY_BUFFER, glName(buffers_, s.elementBuffer));
    for (int unit = 0; unit < kMaxUnits; ++unit) {
        g.ActiveTexture(GL::TEXTURE0 + unit);
        auto t2 = s.textures2D.find(unit);
        auto tc = s.texturesCube.find(unit);
        glBindTexture(GL_TEXTURE_2D, t2 == s.textures2D.end() ? 0 : glName(textures_, t2->second));
        glBindTexture(GL::TEXTURE_CUBE_MAP, tc == s.texturesCube.end() ? 0 : glName(textures_, tc->second));
        glDisable(GL_TEXTURE_2D);
    }
    g.ActiveTexture(GL::TEXTURE0 + s.activeTexture);

    glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    glScissor(s.scissor[0], s.scissor[1], s.scissor[2], s.scissor[3]);
    glClearColor(s.clearColor[0], s.clearColor[1], s.clearColor[2], s.clearColor[3]);
    glClearDepth(s.clearDepth);
    glClearStencil(s.clearStencil);
    // Fixed-function parts of the browser's pipeline that would still act on a program's output
    for (GLenum cap : {GLenum(GL_ALPHA_TEST), GLenum(GL_COLOR_LOGIC_OP), GLenum(GL_LIGHTING), GLenum(GL_FOG),
                       GLenum(GL_TEXTURE_2D), GLenum(GL_LINE_SMOOTH), GLenum(GL_POLYGON_SMOOTH)}) {
        glDisable(cap);
    }
    glEnable(kVertexProgramPointSize);
    glEnable(kPointSprite);
    for (GLenum cap : {GL::BLEND, GL::CULL_FACE, GL::DEPTH_TEST, GL::DITHER, GL::POLYGON_OFFSET_FILL,
                       GL::SAMPLE_ALPHA_TO_COVERAGE, GL::SAMPLE_COVERAGE, GL::SCISSOR_TEST, GL::STENCIL_TEST}) {
        auto it = s.caps.find(cap);
        if (it != s.caps.end() && it->second) glEnable(cap);
        else glDisable(cap);
    }
    if (g.BlendFuncSeparate) g.BlendFuncSeparate(s.blend[0], s.blend[1], s.blend[2], s.blend[3]);
    if (g.BlendEquationSeparate) g.BlendEquationSeparate(s.blendEquation[0], s.blendEquation[1]);
    if (g.BlendColor) g.BlendColor(s.blendColor[0], s.blendColor[1], s.blendColor[2], s.blendColor[3]);
    glDepthFunc(s.depthFunc);
    glDepthMask(s.depthMask);
    glDepthRange(s.depthRange[0], s.depthRange[1]);
    glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
    for (int face = 0; face < 2; ++face) {
        GLenum f = face == 0 ? kFront : kBack;
        if (g.StencilFuncSeparate) g.StencilFuncSeparate(f, s.stencilFunc[face][0], asInt(s.stencilFunc[face][1]), s.stencilFunc[face][2]);
        if (g.StencilOpSeparate) g.StencilOpSeparate(f, s.stencilOp[face][0], s.stencilOp[face][1], s.stencilOp[face][2]);
        if (g.StencilMaskSeparate) g.StencilMaskSeparate(f, s.stencilMask[face]);
    }
    glCullFace(s.cullFace);
    glFrontFace(s.frontFace);
    glLineWidth(s.lineWidth);
    glPolygonOffset(s.polygonOffset[0], s.polygonOffset[1]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, s.unpackAlignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
}

bool WebGLRenderingContext::flush() {
    if (!glAvailable() || !loadFunctions()) return false;
    if (width_ == 0 || height_ == 0) {
        pending_.clear();
        pendingBytes_ = 0;
        return true;
    }
    const bool drawsSomething = !pending_.empty();
    if (!drawsSomething && fbo_ && bufferWidth_ == width_ && bufferHeight_ == height_) return true;

    SavedGLState saved;
    ensureDrawingBuffer();
    g.BindFramebuffer(kFramebuffer, msFbo_ ? msFbo_ : fbo_);
    if (needsClear_ || (drawsSomething && presented_ && !attributes_.preserveDrawingBuffer)) {
        clearDrawingBuffer();
        needsClear_ = false;
    }
    presented_ = false;
    applyState();

    std::vector<WebGLCommand> commands;
    commands.swap(pending_);
    pendingBytes_ = 0;
    for (const WebGLCommand& command : commands) execute(command);

    if (msFbo_) {
        // Resolve the samples into the texture the browser composites
        glDisable(GL_SCISSOR_TEST);
        g.BindFramebuffer(kReadFramebuffer, msFbo_);
        g.BindFramebuffer(kDrawFramebuffer, fbo_);
        g.BlitFramebuffer(0, 0, width_, height_, 0, 0, width_, height_, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    return true;
}

bool WebGLRenderingContext::readPixels(int x, int y, int width, int height, uint8_t* rgba) {
    if (!flush() || !fbo_) return false;
    SavedGLState saved;
    g.BindFramebuffer(kFramebuffer, fbo_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return true;
}

void WebGLRenderingContext::execute(const WebGLCommand& c) {
    State& s = state_;
    const uint32_t* u = c.u;
    switch (c.op) {
        // ---- Shaders and programs ----
        case WebGLOp::CreateShader:
            shaders_[u[0]] = g.CreateShader(u[1]);
            shaderTypes_[u[0]] = u[1];
            break;
        case WebGLOp::ShaderSource: {
            GLuint id = glName(shaders_, u[0]);
            if (!id) break;
            const std::string translated = translateShader(c.text);
            const char* text = translated.c_str();
            g.ShaderSource(id, 1, &text, nullptr);
            break;
        }
        case WebGLOp::CompileShader: {
            GLuint id = glName(shaders_, u[0]);
            if (!id) break;
            g.CompileShader(id);
            GLint ok = 0, length = 0;
            g.GetShaderiv(id, kCompileStatus, &ok);
            g.GetShaderiv(id, kInfoLogLength, &length);
            CompileResult result{true, ok == GL_TRUE, {}};
            if (length > 1) {
                result.log.resize(static_cast<size_t>(length));
                g.GetShaderInfoLog(id, length, nullptr, &result.log[0]);
                result.log.resize(std::strlen(result.log.c_str()));
            }
            if (!result.ok) std::cerr << "[WebGL] shader did not compile:\n" << result.log << std::endl;
            shaderResults_[u[0]] = std::move(result);
            break;
        }
        case WebGLOp::DeleteShader: {
            auto it = shaders_.find(u[0]);
            if (it != shaders_.end()) { g.DeleteShader(it->second); shaders_.erase(it); }
            break;
        }
        case WebGLOp::CreateProgram:
            programs_[u[0]] = g.CreateProgram();
            break;
        case WebGLOp::AttachShader:
            if (glName(programs_, u[0]) && glName(shaders_, u[1])) g.AttachShader(glName(programs_, u[0]), glName(shaders_, u[1]));
            break;
        case WebGLOp::DetachShader:
            if (g.DetachShader && glName(programs_, u[0]) && glName(shaders_, u[1])) g.DetachShader(glName(programs_, u[0]), glName(shaders_, u[1]));
            break;
        case WebGLOp::BindAttribLocation:
            if (glName(programs_, u[0])) g.BindAttribLocation(glName(programs_, u[0]), u[1], c.text.c_str());
            break;
        case WebGLOp::LinkProgram: {
            GLuint id = glName(programs_, u[0]);
            if (!id) break;
            g.LinkProgram(id);
            GLint ok = 0, length = 0;
            g.GetProgramiv(id, kLinkStatus, &ok);
            g.GetProgramiv(id, kInfoLogLength, &length);
            CompileResult result{true, ok == GL_TRUE, {}};
            if (length > 1) {
                result.log.resize(static_cast<size_t>(length));
                g.GetProgramInfoLog(id, length, nullptr, &result.log[0]);
                result.log.resize(std::strlen(result.log.c_str()));
            }
            if (!result.ok) std::cerr << "[WebGL] program did not link:\n" << result.log << std::endl;
            programResults_[u[0]] = std::move(result);
            for (auto it = uniformLocations_.begin(); it != uniformLocations_.end();) {
                it = it->first.first == u[0] ? uniformLocations_.erase(it) : std::next(it);
            }
            // Linking the current program makes the new executable current
            if (s.program == u[0]) g.UseProgram(id);
            break;
        }
        case WebGLOp::UseProgram:
            s.program = u[0];
            g.UseProgram(glName(programs_, u[0]));
            break;
        case WebGLOp::DeleteProgram: {
            auto it = programs_.find(u[0]);
            if (it != programs_.end()) { g.DeleteProgram(it->second); programs_.erase(it); }
            if (s.program == u[0]) s.program = 0;
            break;
        }

        // ---- Buffers ----
        case WebGLOp::CreateBuffer: {
            GLuint id = 0;
            g.GenBuffers(1, &id);
            buffers_[u[0]] = id;
            break;
        }
        case WebGLOp::BindBuffer:
            if (u[0] == GL::ARRAY_BUFFER) s.arrayBuffer = u[1];
            else s.elementBuffer = u[1];
            g.BindBuffer(u[0], glName(buffers_, u[1]));
            break;
        case WebGLOp::BufferData:
            if (c.data.empty()) {
                std::vector<uint8_t> zeros(u[2], 0);
                g.BufferData(u[0], static_cast<ptrdiff_t>(zeros.size()), zeros.data(), u[1]);
            } else {
                g.BufferData(u[0], static_cast<ptrdiff_t>(c.data.size()), c.data.data(), u[1]);
            }
            break;
        case WebGLOp::BufferSubData:
            if (g.BufferSubData) g.BufferSubData(u[0], static_cast<ptrdiff_t>(u[1]), static_cast<ptrdiff_t>(c.data.size()), c.data.data());
            break;
        case WebGLOp::DeleteBuffer: {
            auto it = buffers_.find(u[0]);
            if (it != buffers_.end()) { g.DeleteBuffers(1, &it->second); buffers_.erase(it); }
            if (s.arrayBuffer == u[0]) s.arrayBuffer = 0;
            if (s.elementBuffer == u[0]) s.elementBuffer = 0;
            for (AttribState& a : s.attribs) {
                if (a.buffer == u[0]) a.buffer = 0;
            }
            break;
        }

        // ---- Textures ----
        case WebGLOp::CreateTexture: {
            GLuint id = 0;
            glGenTextures(1, &id);
            textures_[u[0]] = id;
            break;
        }
        case WebGLOp::BindTexture:
            (u[0] == GL::TEXTURE_CUBE_MAP ? s.texturesCube : s.textures2D)[s.activeTexture] = u[1];
            glBindTexture(u[0], glName(textures_, u[1]));
            break;
        case WebGLOp::ActiveTexture:
            s.activeTexture = std::min<uint32_t>(u[0] - GL::TEXTURE0, kMaxUnits - 1);
            g.ActiveTexture(GL::TEXTURE0 + s.activeTexture);
            break;
        case WebGLOp::TexImage2D:
            glTexImage2D(u[0], asInt(u[1]), asInt(u[2]), asInt(u[3]), asInt(u[4]), 0, u[5], u[6],
                         c.data.empty() ? nullptr : c.data.data());
            break;
        case WebGLOp::TexSubImage2D:
            if (!c.data.empty()) glTexSubImage2D(u[0], asInt(u[1]), asInt(u[2]), asInt(u[3]), asInt(u[4]), asInt(u[5]), u[6], u[7], c.data.data());
            break;
        case WebGLOp::TexParameteri:
            glTexParameteri(u[0], u[1], asInt(u[2]));
            break;
        case WebGLOp::GenerateMipmap:
            if (g.GenerateMipmap) g.GenerateMipmap(u[0]);
            break;
        case WebGLOp::DeleteTexture: {
            auto it = textures_.find(u[0]);
            if (it != textures_.end()) { glDeleteTextures(1, &it->second); textures_.erase(it); }
            for (auto* bindings : {&s.textures2D, &s.texturesCube}) {
                for (auto& [unit, handle] : *bindings) {
                    if (handle == u[0]) handle = 0;
                }
            }
            break;
        }

        // ---- Attributes and uniforms ----
        case WebGLOp::EnableVertexAttribArray:
            if (u[0] < 16) { s.attribs[u[0]].enabled = true; g.EnableVertexAttribArray(u[0]); }
            break;
        case WebGLOp::DisableVertexAttribArray:
            if (u[0] < 16) { s.attribs[u[0]].enabled = false; g.DisableVertexAttribArray(u[0]); }
            break;
        case WebGLOp::VertexAttribPointer:
            if (u[0] < 16) {
                AttribState& a = s.attribs[u[0]];
                a.size = asInt(u[1]); a.type = u[2]; a.normalized = u[3] != 0;
                a.stride = asInt(u[4]); a.offset = u[5]; a.buffer = s.arrayBuffer;
                g.VertexAttribPointer(u[0], a.size, a.type, a.normalized, a.stride,
                                      reinterpret_cast<const void*>(static_cast<uintptr_t>(a.offset)));
            }
            break;
        case WebGLOp::VertexAttrib4f:
            if (u[0] < 16) {
                std::copy(c.f, c.f + 4, s.attribs[u[0]].value);
                if (g.VertexAttrib4f) g.VertexAttrib4f(u[0], c.f[0], c.f[1], c.f[2], c.f[3]);
            }
            break;
        case WebGLOp::Uniform: {
            // A location belongs to its program, which must be the current one
            if (u[0] != s.program) break;
            int location = uniformLocation(u[0], c.text);
            if (location < 0) break;
            const auto kind = static_cast<UniformKind>(u[1]);
            const GLsizei count = static_cast<GLsizei>(u[2]);
            const auto* floats = reinterpret_cast<const GLfloat*>(c.data.data());
            const auto* ints = reinterpret_cast<const GLint*>(c.data.data());
            switch (kind) {
                case UniformKind::F1: case UniformKind::F2: case UniformKind::F3: case UniformKind::F4:
                    if (auto fn = g.Uniformfv[static_cast<int>(kind) - static_cast<int>(UniformKind::F1)]) fn(location, count, floats);
                    break;
                case UniformKind::I1: case UniformKind::I2: case UniformKind::I3: case UniformKind::I4:
                    if (auto fn = g.Uniformiv[static_cast<int>(kind) - static_cast<int>(UniformKind::I1)]) fn(location, count, ints);
                    break;
                case UniformKind::Mat2: case UniformKind::Mat3: case UniformKind::Mat4:
                    if (auto fn = g.UniformMatrixfv[static_cast<int>(kind) - static_cast<int>(UniformKind::Mat2)]) {
                        fn(location, count, u[3] ? GL_TRUE : GL_FALSE, floats);
                    }
                    break;
            }
            break;
        }

        // ---- Fixed state ----
        case WebGLOp::Viewport:
            for (int i = 0; i < 4; ++i) s.viewport[i] = asInt(u[i]);
            glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
            break;
        case WebGLOp::Scissor:
            for (int i = 0; i < 4; ++i) s.scissor[i] = asInt(u[i]);
            glScissor(s.scissor[0], s.scissor[1], s.scissor[2], s.scissor[3]);
            break;
        case WebGLOp::ClearColor:
            std::copy(c.f, c.f + 4, s.clearColor);
            glClearColor(c.f[0], c.f[1], c.f[2], c.f[3]);
            break;
        case WebGLOp::ClearDepth:
            s.clearDepth = c.f[0];
            glClearDepth(c.f[0]);
            break;
        case WebGLOp::ClearStencil:
            s.clearStencil = asInt(u[0]);
            glClearStencil(s.clearStencil);
            break;
        case WebGLOp::Clear:
            glClear(u[0] & (GL::COLOR_BUFFER_BIT | GL::DEPTH_BUFFER_BIT | GL::STENCIL_BUFFER_BIT));
            break;
        case WebGLOp::Enable:
        case WebGLOp::Disable:
            s.caps[u[0]] = c.op == WebGLOp::Enable;
            if (s.caps[u[0]]) glEnable(u[0]);
            else glDisable(u[0]);
            break;
        case WebGLOp::BlendFuncSeparate:
            std::copy(u, u + 4, s.blend);
            if (g.BlendFuncSeparate) g.BlendFuncSeparate(u[0], u[1], u[2], u[3]);
            break;
        case WebGLOp::BlendEquationSeparate:
            s.blendEquation[0] = u[0]; s.blendEquation[1] = u[1];
            if (g.BlendEquationSeparate) g.BlendEquationSeparate(u[0], u[1]);
            break;
        case WebGLOp::BlendColor:
            std::copy(c.f, c.f + 4, s.blendColor);
            if (g.BlendColor) g.BlendColor(c.f[0], c.f[1], c.f[2], c.f[3]);
            break;
        case WebGLOp::DepthFunc:
            s.depthFunc = u[0];
            glDepthFunc(u[0]);
            break;
        case WebGLOp::DepthMask:
            s.depthMask = u[0] != 0;
            glDepthMask(s.depthMask);
            break;
        case WebGLOp::DepthRange:
            s.depthRange[0] = c.f[0]; s.depthRange[1] = c.f[1];
            glDepthRange(c.f[0], c.f[1]);
            break;
        case WebGLOp::ColorMask:
            for (int i = 0; i < 4; ++i) s.colorMask[i] = u[i] != 0;
            glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
            break;
        case WebGLOp::StencilFuncSeparate:
            for (int face = 0; face < 2; ++face) {
                if (u[0] == kFrontAndBack || u[0] == (face == 0 ? kFront : kBack)) {
                    s.stencilFunc[face][0] = u[1]; s.stencilFunc[face][1] = u[2]; s.stencilFunc[face][2] = u[3];
                }
            }
            if (g.StencilFuncSeparate) g.StencilFuncSeparate(u[0], u[1], asInt(u[2]), u[3]);
            break;
        case WebGLOp::StencilOpSeparate:
            for (int face = 0; face < 2; ++face) {
                if (u[0] == kFrontAndBack || u[0] == (face == 0 ? kFront : kBack)) {
                    s.stencilOp[face][0] = u[1]; s.stencilOp[face][1] = u[2]; s.stencilOp[face][2] = u[3];
                }
            }
            if (g.StencilOpSeparate) g.StencilOpSeparate(u[0], u[1], u[2], u[3]);
            break;
        case WebGLOp::StencilMaskSeparate:
            for (int face = 0; face < 2; ++face) {
                if (u[0] == kFrontAndBack || u[0] == (face == 0 ? kFront : kBack)) s.stencilMask[face] = u[1];
            }
            if (g.StencilMaskSeparate) g.StencilMaskSeparate(u[0], u[1]);
            break;
        case WebGLOp::CullFace:
            s.cullFace = u[0];
            glCullFace(u[0]);
            break;
        case WebGLOp::FrontFace:
            s.frontFace = u[0];
            glFrontFace(u[0]);
            break;
        case WebGLOp::LineWidth:
            s.lineWidth = c.f[0];
            glLineWidth(c.f[0]);
            break;
        case WebGLOp::PolygonOffset:
            s.polygonOffset[0] = c.f[0]; s.polygonOffset[1] = c.f[1];
            glPolygonOffset(c.f[0], c.f[1]);
            break;
        case WebGLOp::PixelStorei:
            if (u[0] == GL::UNPACK_ALIGNMENT) {
                s.unpackAlignment = asInt(u[1]);
                glPixelStorei(GL_UNPACK_ALIGNMENT, s.unpackAlignment);
            }
            break;

        // ---- Drawing ----
        case WebGLOp::DrawArrays:
            if (s.program) glDrawArrays(u[0], asInt(u[1]), asInt(u[2]));
            break;
        case WebGLOp::DrawElements:
            if (s.program && s.elementBuffer) {
                glDrawElements(u[0], asInt(u[1]), u[2], reinterpret_cast<const void*>(static_cast<uintptr_t>(u[3])));
            }
            break;
    }
}

} // namespace NXRender
