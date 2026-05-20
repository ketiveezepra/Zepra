// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "webgl_context.h"
#include <GL/gl.h>
#ifdef __linux__
#include <GL/glx.h>
#elif _WIN32
#include <windows.h>
#endif
#include <cstring>

namespace NXRender {

// ==================================================================
// GL 2.0+ function loading (same pattern as shader.cpp)
// ==================================================================

typedef GLuint (*PFN_glCreateShader)(GLenum);
typedef void   (*PFN_glShaderSource)(GLuint, GLsizei, const GLchar**, const GLint*);
typedef void   (*PFN_glCompileShader)(GLuint);
typedef void   (*PFN_glGetShaderiv)(GLuint, GLenum, GLint*);
typedef void   (*PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef GLuint (*PFN_glCreateProgram)();
typedef void   (*PFN_glAttachShader)(GLuint, GLuint);
typedef void   (*PFN_glDetachShader)(GLuint, GLuint);
typedef void   (*PFN_glLinkProgram)(GLuint);
typedef void   (*PFN_glGetProgramiv)(GLuint, GLenum, GLint*);
typedef void   (*PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void   (*PFN_glUseProgram)(GLuint);
typedef void   (*PFN_glDeleteShader)(GLuint);
typedef void   (*PFN_glDeleteProgram)(GLuint);
typedef GLint  (*PFN_glGetUniformLocation)(GLuint, const GLchar*);
typedef GLint  (*PFN_glGetAttribLocation)(GLuint, const GLchar*);
typedef void   (*PFN_glUniform1f)(GLint, GLfloat);
typedef void   (*PFN_glUniform2f)(GLint, GLfloat, GLfloat);
typedef void   (*PFN_glUniform3f)(GLint, GLfloat, GLfloat, GLfloat);
typedef void   (*PFN_glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void   (*PFN_glUniform1i)(GLint, GLint);
typedef void   (*PFN_glUniformMatrix3fv)(GLint, GLsizei, GLboolean, const GLfloat*);
typedef void   (*PFN_glUniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*);
typedef void   (*PFN_glEnableVertexAttribArray)(GLuint);
typedef void   (*PFN_glDisableVertexAttribArray)(GLuint);
typedef void   (*PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
typedef void   (*PFN_glGenBuffers)(GLsizei, GLuint*);
typedef void   (*PFN_glDeleteBuffers)(GLsizei, const GLuint*);
typedef void   (*PFN_glBindBuffer)(GLenum, GLuint);
typedef void   (*PFN_glBufferData)(GLenum, GLsizeiptr, const void*, GLenum);
typedef void   (*PFN_glBufferSubData)(GLenum, GLintptr, GLsizeiptr, const void*);
typedef void   (*PFN_glGenFramebuffers)(GLsizei, GLuint*);
typedef void   (*PFN_glDeleteFramebuffers)(GLsizei, const GLuint*);
typedef void   (*PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void   (*PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void   (*PFN_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (*PFN_glCheckFramebufferStatus)(GLenum);
typedef void   (*PFN_glGenRenderbuffers)(GLsizei, GLuint*);
typedef void   (*PFN_glDeleteRenderbuffers)(GLsizei, const GLuint*);
typedef void   (*PFN_glBindRenderbuffer)(GLenum, GLuint);
typedef void   (*PFN_glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void   (*PFN_glGenerateMipmap)(GLenum);
typedef void   (*PFN_glActiveTexture)(GLenum);
typedef void   (*PFN_glBlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
typedef void   (*PFN_glBlendEquation)(GLenum);

#define NX_WGL(type, name) static type nx_##name = nullptr

NX_WGL(PFN_glCreateShader, glCreateShader);
NX_WGL(PFN_glShaderSource, glShaderSource);
NX_WGL(PFN_glCompileShader, glCompileShader);
NX_WGL(PFN_glGetShaderiv, glGetShaderiv);
NX_WGL(PFN_glGetShaderInfoLog, glGetShaderInfoLog);
NX_WGL(PFN_glCreateProgram, glCreateProgram);
NX_WGL(PFN_glAttachShader, glAttachShader);
NX_WGL(PFN_glDetachShader, glDetachShader);
NX_WGL(PFN_glLinkProgram, glLinkProgram);
NX_WGL(PFN_glGetProgramiv, glGetProgramiv);
NX_WGL(PFN_glGetProgramInfoLog, glGetProgramInfoLog);
NX_WGL(PFN_glUseProgram, glUseProgram);
NX_WGL(PFN_glDeleteShader, glDeleteShader);
NX_WGL(PFN_glDeleteProgram, glDeleteProgram);
NX_WGL(PFN_glGetUniformLocation, glGetUniformLocation);
NX_WGL(PFN_glGetAttribLocation, glGetAttribLocation);
NX_WGL(PFN_glUniform1f, glUniform1f);
NX_WGL(PFN_glUniform2f, glUniform2f);
NX_WGL(PFN_glUniform3f, glUniform3f);
NX_WGL(PFN_glUniform4f, glUniform4f);
NX_WGL(PFN_glUniform1i, glUniform1i);
NX_WGL(PFN_glUniformMatrix3fv, glUniformMatrix3fv);
NX_WGL(PFN_glUniformMatrix4fv, glUniformMatrix4fv);
NX_WGL(PFN_glEnableVertexAttribArray, glEnableVertexAttribArray);
NX_WGL(PFN_glDisableVertexAttribArray, glDisableVertexAttribArray);
NX_WGL(PFN_glVertexAttribPointer, glVertexAttribPointer);
NX_WGL(PFN_glGenBuffers, glGenBuffers);
NX_WGL(PFN_glDeleteBuffers, glDeleteBuffers);
NX_WGL(PFN_glBindBuffer, glBindBuffer);
NX_WGL(PFN_glBufferData, glBufferData);
NX_WGL(PFN_glBufferSubData, glBufferSubData);
NX_WGL(PFN_glGenFramebuffers, glGenFramebuffers);
NX_WGL(PFN_glDeleteFramebuffers, glDeleteFramebuffers);
NX_WGL(PFN_glBindFramebuffer, glBindFramebuffer);
NX_WGL(PFN_glFramebufferTexture2D, glFramebufferTexture2D);
NX_WGL(PFN_glFramebufferRenderbuffer, glFramebufferRenderbuffer);
NX_WGL(PFN_glCheckFramebufferStatus, glCheckFramebufferStatus);
NX_WGL(PFN_glGenRenderbuffers, glGenRenderbuffers);
NX_WGL(PFN_glDeleteRenderbuffers, glDeleteRenderbuffers);
NX_WGL(PFN_glBindRenderbuffer, glBindRenderbuffer);
NX_WGL(PFN_glRenderbufferStorage, glRenderbufferStorage);
NX_WGL(PFN_glGenerateMipmap, glGenerateMipmap);
NX_WGL(PFN_glActiveTexture, glActiveTexture);
NX_WGL(PFN_glBlendFuncSeparate, glBlendFuncSeparate);
NX_WGL(PFN_glBlendEquation, glBlendEquation);

#undef NX_WGL

static bool s_wglLoaded = false;

static void* wglGetProc(const char* name) {
#ifdef __linux__
    return reinterpret_cast<void*>(glXGetProcAddress(reinterpret_cast<const GLubyte*>(name)));
#elif _WIN32
    return reinterpret_cast<void*>(wglGetProcAddress(name));
#else
    (void)name;
    return nullptr;
#endif
}

static void loadWebGLFunctions() {
    if (s_wglLoaded) return;
    s_wglLoaded = true;

    #define WGL_LOAD(name) nx_##name = reinterpret_cast<decltype(nx_##name)>(wglGetProc(#name))
    WGL_LOAD(glCreateShader);
    WGL_LOAD(glShaderSource);
    WGL_LOAD(glCompileShader);
    WGL_LOAD(glGetShaderiv);
    WGL_LOAD(glGetShaderInfoLog);
    WGL_LOAD(glCreateProgram);
    WGL_LOAD(glAttachShader);
    WGL_LOAD(glDetachShader);
    WGL_LOAD(glLinkProgram);
    WGL_LOAD(glGetProgramiv);
    WGL_LOAD(glGetProgramInfoLog);
    WGL_LOAD(glUseProgram);
    WGL_LOAD(glDeleteShader);
    WGL_LOAD(glDeleteProgram);
    WGL_LOAD(glGetUniformLocation);
    WGL_LOAD(glGetAttribLocation);
    WGL_LOAD(glUniform1f);
    WGL_LOAD(glUniform2f);
    WGL_LOAD(glUniform3f);
    WGL_LOAD(glUniform4f);
    WGL_LOAD(glUniform1i);
    WGL_LOAD(glUniformMatrix3fv);
    WGL_LOAD(glUniformMatrix4fv);
    WGL_LOAD(glEnableVertexAttribArray);
    WGL_LOAD(glDisableVertexAttribArray);
    WGL_LOAD(glVertexAttribPointer);
    WGL_LOAD(glGenBuffers);
    WGL_LOAD(glDeleteBuffers);
    WGL_LOAD(glBindBuffer);
    WGL_LOAD(glBufferData);
    WGL_LOAD(glBufferSubData);
    WGL_LOAD(glGenFramebuffers);
    WGL_LOAD(glDeleteFramebuffers);
    WGL_LOAD(glBindFramebuffer);
    WGL_LOAD(glFramebufferTexture2D);
    WGL_LOAD(glFramebufferRenderbuffer);
    WGL_LOAD(glCheckFramebufferStatus);
    WGL_LOAD(glGenRenderbuffers);
    WGL_LOAD(glDeleteRenderbuffers);
    WGL_LOAD(glBindRenderbuffer);
    WGL_LOAD(glRenderbufferStorage);
    WGL_LOAD(glGenerateMipmap);
    WGL_LOAD(glActiveTexture);
    WGL_LOAD(glBlendFuncSeparate);
    WGL_LOAD(glBlendEquation);
    #undef WGL_LOAD
}

// ==================================================================
// Construction
// ==================================================================

WebGLRenderingContext::WebGLRenderingContext(int width, int height)
    : width_(width), height_(height) {
    loadWebGLFunctions();
    if (nx_glGenFramebuffers) nx_glGenFramebuffers(1, &defaultFBO_);
}

WebGLRenderingContext::~WebGLRenderingContext() {
    if (defaultFBO_ && nx_glDeleteFramebuffers) nx_glDeleteFramebuffers(1, &defaultFBO_);
}

// ==================================================================
// Shader
// ==================================================================

WebGLShader WebGLRenderingContext::createShader(uint32_t type) {
    WebGLShader s;
    s.type = type;
    if (nx_glCreateShader) s.id = nx_glCreateShader(type);
    return s;
}

void WebGLRenderingContext::shaderSource(WebGLShader& shader, const std::string& source) {
    shader.source = source;
    if (nx_glShaderSource) {
        const char* src = source.c_str();
        nx_glShaderSource(shader.id, 1, &src, nullptr);
    }
}

void WebGLRenderingContext::compileShader(WebGLShader& shader) {
    if (!nx_glCompileShader) return;
    nx_glCompileShader(shader.id);
    GLint status = 0;
    if (nx_glGetShaderiv) nx_glGetShaderiv(shader.id, GL_COMPILE_STATUS, &status);
    shader.compiled = (status == GL_TRUE);
    GLint logLen = 0;
    if (nx_glGetShaderiv) nx_glGetShaderiv(shader.id, GL_INFO_LOG_LENGTH, &logLen);
    if (logLen > 0 && nx_glGetShaderInfoLog) {
        shader.infoLog.resize(logLen);
        nx_glGetShaderInfoLog(shader.id, logLen, nullptr, &shader.infoLog[0]);
    }
}

std::string WebGLRenderingContext::getShaderInfoLog(const WebGLShader& shader) {
    return shader.infoLog;
}

bool WebGLRenderingContext::getShaderParameter(const WebGLShader& shader, uint32_t /*pname*/) {
    return shader.compiled;
}

void WebGLRenderingContext::deleteShader(WebGLShader& shader) {
    if (shader.id && nx_glDeleteShader) { nx_glDeleteShader(shader.id); shader.id = 0; }
}

// ==================================================================
// Program
// ==================================================================

WebGLProgram WebGLRenderingContext::createProgram() {
    WebGLProgram p;
    if (nx_glCreateProgram) p.id = nx_glCreateProgram();
    return p;
}

void WebGLRenderingContext::attachShader(WebGLProgram& program, const WebGLShader& shader) {
    if (nx_glAttachShader) nx_glAttachShader(program.id, shader.id);
    if (shader.type == GL::VERTEX_SHADER) program.vs = shader.id;
    else program.fs = shader.id;
}

void WebGLRenderingContext::linkProgram(WebGLProgram& program) {
    if (!nx_glLinkProgram) return;
    nx_glLinkProgram(program.id);
    GLint status = 0;
    if (nx_glGetProgramiv) nx_glGetProgramiv(program.id, GL_LINK_STATUS, &status);
    program.linked = (status == GL_TRUE);
    GLint logLen = 0;
    if (nx_glGetProgramiv) nx_glGetProgramiv(program.id, GL_INFO_LOG_LENGTH, &logLen);
    if (logLen > 0 && nx_glGetProgramInfoLog) {
        program.infoLog.resize(logLen);
        nx_glGetProgramInfoLog(program.id, logLen, nullptr, &program.infoLog[0]);
    }
}

void WebGLRenderingContext::useProgram(const WebGLProgram& program) {
    if (nx_glUseProgram) nx_glUseProgram(program.id);
}

std::string WebGLRenderingContext::getProgramInfoLog(const WebGLProgram& program) {
    return program.infoLog;
}

bool WebGLRenderingContext::getProgramParameter(const WebGLProgram& program, uint32_t /*pname*/) {
    return program.linked;
}

void WebGLRenderingContext::deleteProgram(WebGLProgram& program) {
    if (program.id && nx_glDeleteProgram) { nx_glDeleteProgram(program.id); program.id = 0; }
}

void WebGLRenderingContext::detachShader(WebGLProgram& program, const WebGLShader& shader) {
    if (nx_glDetachShader) nx_glDetachShader(program.id, shader.id);
}

// ==================================================================
// Uniform
// ==================================================================

WebGLUniformLocation WebGLRenderingContext::getUniformLocation(const WebGLProgram& program,
                                                                 const std::string& name) {
    WebGLUniformLocation loc;
    loc.program = program.id;
    if (nx_glGetUniformLocation) loc.location = nx_glGetUniformLocation(program.id, name.c_str());
    return loc;
}

void WebGLRenderingContext::uniform1f(const WebGLUniformLocation& loc, float v) {
    if (nx_glUniform1f) nx_glUniform1f(loc.location, v);
}
void WebGLRenderingContext::uniform2f(const WebGLUniformLocation& loc, float v0, float v1) {
    if (nx_glUniform2f) nx_glUniform2f(loc.location, v0, v1);
}
void WebGLRenderingContext::uniform3f(const WebGLUniformLocation& loc, float v0, float v1, float v2) {
    if (nx_glUniform3f) nx_glUniform3f(loc.location, v0, v1, v2);
}
void WebGLRenderingContext::uniform4f(const WebGLUniformLocation& loc,
                                        float v0, float v1, float v2, float v3) {
    if (nx_glUniform4f) nx_glUniform4f(loc.location, v0, v1, v2, v3);
}
void WebGLRenderingContext::uniform1i(const WebGLUniformLocation& loc, int v) {
    if (nx_glUniform1i) nx_glUniform1i(loc.location, v);
}
void WebGLRenderingContext::uniformMatrix3fv(const WebGLUniformLocation& loc,
                                               bool transpose, const float* value) {
    if (nx_glUniformMatrix3fv) nx_glUniformMatrix3fv(loc.location, 1, transpose ? GL_TRUE : GL_FALSE, value);
}
void WebGLRenderingContext::uniformMatrix4fv(const WebGLUniformLocation& loc,
                                               bool transpose, const float* value) {
    if (nx_glUniformMatrix4fv) nx_glUniformMatrix4fv(loc.location, 1, transpose ? GL_TRUE : GL_FALSE, value);
}

// ==================================================================
// Attribute
// ==================================================================

int WebGLRenderingContext::getAttribLocation(const WebGLProgram& program, const std::string& name) {
    if (nx_glGetAttribLocation) return nx_glGetAttribLocation(program.id, name.c_str());
    return -1;
}

void WebGLRenderingContext::enableVertexAttribArray(int index) {
    if (nx_glEnableVertexAttribArray) nx_glEnableVertexAttribArray(index);
}

void WebGLRenderingContext::disableVertexAttribArray(int index) {
    if (nx_glDisableVertexAttribArray) nx_glDisableVertexAttribArray(index);
}

void WebGLRenderingContext::vertexAttribPointer(int index, int size, uint32_t type,
                                                  bool normalized, int stride, size_t offset) {
    if (nx_glVertexAttribPointer)
        nx_glVertexAttribPointer(index, size, type, normalized ? GL_TRUE : GL_FALSE,
                                  stride, reinterpret_cast<const void*>(offset));
}

// ==================================================================
// Buffer
// ==================================================================

WebGLBuffer WebGLRenderingContext::createBuffer() {
    WebGLBuffer b;
    if (nx_glGenBuffers) nx_glGenBuffers(1, &b.id);
    return b;
}

void WebGLRenderingContext::bindBuffer(uint32_t target, const WebGLBuffer& buffer) {
    if (nx_glBindBuffer) nx_glBindBuffer(target, buffer.id);
}

void WebGLRenderingContext::bufferData(uint32_t target, const void* data,
                                         size_t size, uint32_t usage) {
    if (nx_glBufferData) nx_glBufferData(target, size, data, usage);
}

void WebGLRenderingContext::bufferSubData(uint32_t target, size_t offset,
                                            const void* data, size_t size) {
    if (nx_glBufferSubData) nx_glBufferSubData(target, offset, size, data);
}

void WebGLRenderingContext::deleteBuffer(WebGLBuffer& buffer) {
    if (buffer.id && nx_glDeleteBuffers) { nx_glDeleteBuffers(1, &buffer.id); buffer.id = 0; }
}

// ==================================================================
// Texture
// ==================================================================

WebGLTexture WebGLRenderingContext::createTexture() {
    WebGLTexture t;
    glGenTextures(1, &t.id);
    return t;
}

void WebGLRenderingContext::bindTexture(uint32_t target, const WebGLTexture& texture) {
    glBindTexture(target, texture.id);
}

void WebGLRenderingContext::texImage2D(uint32_t target, int level, uint32_t internalFormat,
                                         int width, int height, int border,
                                         uint32_t format, uint32_t type, const void* pixels) {
    glTexImage2D(target, level, internalFormat, width, height, border, format, type, pixels);
}

void WebGLRenderingContext::texSubImage2D(uint32_t target, int level, int xoffset, int yoffset,
                                            int width, int height, uint32_t format,
                                            uint32_t type, const void* pixels) {
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
}

void WebGLRenderingContext::texParameteri(uint32_t target, uint32_t pname, int param) {
    glTexParameteri(target, pname, param);
}

void WebGLRenderingContext::generateMipmap(uint32_t target) {
    if (nx_glGenerateMipmap) nx_glGenerateMipmap(target);
}

void WebGLRenderingContext::activeTexture(uint32_t textureUnit) {
    if (nx_glActiveTexture) nx_glActiveTexture(textureUnit);
}

void WebGLRenderingContext::deleteTexture(WebGLTexture& texture) {
    if (texture.id) { glDeleteTextures(1, &texture.id); texture.id = 0; }
}

// ==================================================================
// Framebuffer
// ==================================================================

WebGLFramebuffer WebGLRenderingContext::createFramebuffer() {
    WebGLFramebuffer fbo;
    if (nx_glGenFramebuffers) nx_glGenFramebuffers(1, &fbo.id);
    return fbo;
}

void WebGLRenderingContext::bindFramebuffer(uint32_t target, const WebGLFramebuffer& fbo) {
    if (nx_glBindFramebuffer) nx_glBindFramebuffer(target, fbo.id);
}

void WebGLRenderingContext::framebufferTexture2D(uint32_t target, uint32_t attachment,
                                                    uint32_t texTarget,
                                                    const WebGLTexture& texture, int level) {
    if (nx_glFramebufferTexture2D) nx_glFramebufferTexture2D(target, attachment, texTarget, texture.id, level);
}

void WebGLRenderingContext::framebufferRenderbuffer(uint32_t target, uint32_t attachment,
                                                      uint32_t rbTarget,
                                                      const WebGLRenderbuffer& rb) {
    if (nx_glFramebufferRenderbuffer) nx_glFramebufferRenderbuffer(target, attachment, rbTarget, rb.id);
}

uint32_t WebGLRenderingContext::checkFramebufferStatus(uint32_t target) {
    if (nx_glCheckFramebufferStatus) return nx_glCheckFramebufferStatus(target);
    return 0;
}

void WebGLRenderingContext::deleteFramebuffer(WebGLFramebuffer& fbo) {
    if (fbo.id && nx_glDeleteFramebuffers) { nx_glDeleteFramebuffers(1, &fbo.id); fbo.id = 0; }
}

// ==================================================================
// Renderbuffer
// ==================================================================

WebGLRenderbuffer WebGLRenderingContext::createRenderbuffer() {
    WebGLRenderbuffer rb;
    if (nx_glGenRenderbuffers) nx_glGenRenderbuffers(1, &rb.id);
    return rb;
}

void WebGLRenderingContext::bindRenderbuffer(uint32_t target, const WebGLRenderbuffer& rb) {
    if (nx_glBindRenderbuffer) nx_glBindRenderbuffer(target, rb.id);
}

void WebGLRenderingContext::renderbufferStorage(uint32_t target, uint32_t format,
                                                  int width, int height) {
    if (nx_glRenderbufferStorage) nx_glRenderbufferStorage(target, format, width, height);
}

void WebGLRenderingContext::deleteRenderbuffer(WebGLRenderbuffer& rb) {
    if (rb.id && nx_glDeleteRenderbuffers) { nx_glDeleteRenderbuffers(1, &rb.id); rb.id = 0; }
}

// ==================================================================
// Drawing
// ==================================================================

void WebGLRenderingContext::drawArrays(uint32_t mode, int first, int count) {
    glDrawArrays(mode, first, count);
}

void WebGLRenderingContext::drawElements(uint32_t mode, int count, uint32_t type, size_t offset) {
    glDrawElements(mode, count, type, reinterpret_cast<const void*>(offset));
}

// ==================================================================
// State
// ==================================================================

void WebGLRenderingContext::viewport(int x, int y, int w, int h) { glViewport(x, y, w, h); }
void WebGLRenderingContext::scissor(int x, int y, int w, int h) { glScissor(x, y, w, h); }
void WebGLRenderingContext::clearColor(float r, float g, float b, float a) { glClearColor(r, g, b, a); }
void WebGLRenderingContext::clearDepth(float depth) { glClearDepth(depth); }
void WebGLRenderingContext::clearStencil(int s) { glClearStencil(s); }
void WebGLRenderingContext::clear(uint32_t mask) { glClear(mask); }
void WebGLRenderingContext::enable(uint32_t cap) { glEnable(cap); }
void WebGLRenderingContext::disable(uint32_t cap) { glDisable(cap); }
void WebGLRenderingContext::blendFunc(uint32_t sfactor, uint32_t dfactor) { glBlendFunc(sfactor, dfactor); }

void WebGLRenderingContext::blendFuncSeparate(uint32_t srcRGB, uint32_t dstRGB,
                                                uint32_t srcAlpha, uint32_t dstAlpha) {
    if (nx_glBlendFuncSeparate) nx_glBlendFuncSeparate(srcRGB, dstRGB, srcAlpha, dstAlpha);
}

void WebGLRenderingContext::blendEquation(uint32_t mode) {
    if (nx_glBlendEquation) nx_glBlendEquation(mode);
}

void WebGLRenderingContext::depthFunc(uint32_t func) { glDepthFunc(func); }
void WebGLRenderingContext::depthMask(bool flag) { glDepthMask(flag ? GL_TRUE : GL_FALSE); }
void WebGLRenderingContext::stencilFunc(uint32_t func, int ref, uint32_t mask) { glStencilFunc(func, ref, mask); }
void WebGLRenderingContext::stencilOp(uint32_t fail, uint32_t zfail, uint32_t zpass) { glStencilOp(fail, zfail, zpass); }
void WebGLRenderingContext::colorMask(bool r, bool g, bool b, bool a) { glColorMask(r, g, b, a); }
void WebGLRenderingContext::cullFace(uint32_t mode) { glCullFace(mode); }
void WebGLRenderingContext::frontFace(uint32_t mode) { glFrontFace(mode); }
void WebGLRenderingContext::lineWidth(float w) { glLineWidth(w); }
void WebGLRenderingContext::polygonOffset(float factor, float units) { glPolygonOffset(factor, units); }
void WebGLRenderingContext::pixelStorei(uint32_t pname, int param) { glPixelStorei(pname, param); }

void WebGLRenderingContext::readPixels(int x, int y, int w, int h,
                                         uint32_t format, uint32_t type, void* pixels) {
    glReadPixels(x, y, w, h, format, type, pixels);
}

uint32_t WebGLRenderingContext::getError() { return glGetError(); }

// ==================================================================
// Extensions
// ==================================================================

std::vector<std::string> WebGLRenderingContext::getSupportedExtensions() {
    if (extensions_.empty()) {
        const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        if (ext) {
            std::string all(ext);
            size_t pos = 0;
            while (pos < all.size()) {
                size_t next = all.find(' ', pos);
                if (next == std::string::npos) next = all.size();
                extensions_.push_back(all.substr(pos, next - pos));
                pos = next + 1;
            }
        }
    }
    return extensions_;
}

bool WebGLRenderingContext::getExtension(const std::string& name) {
    auto exts = getSupportedExtensions();
    for (const auto& e : exts) {
        if (e == name) return true;
    }
    return false;
}

void WebGLRenderingContext::loseContext() { contextLost_ = true; }
void WebGLRenderingContext::restoreContext() { contextLost_ = false; }

} // namespace NXRender
