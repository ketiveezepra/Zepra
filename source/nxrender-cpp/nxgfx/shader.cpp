// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "nxgfx/shader.h"
#include <algorithm>
#include "nxgfx/gl_includes.h"
#include <cstring>
#include <iostream>

#ifdef __linux__
#include <GL/glx.h>
#endif

namespace NXRender {

// GL extension function types
typedef GLuint (*PFNGLCREATESHADERPROC)(GLenum);
typedef void   (*PFNGLSHADERSOURCEPROC)(GLuint, GLsizei, const GLchar**, const GLint*);
typedef void   (*PFNGLCOMPILESHADERPROC)(GLuint);
typedef void   (*PFNGLGETSHADERIVPROC)(GLuint, GLenum, GLint*);
typedef void   (*PFNGLGETSHADERINFOLOGPROC)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef GLuint (*PFNGLCREATEPROGRAMPROC)();
typedef void   (*PFNGLATTACHSHADERPROC)(GLuint, GLuint);
typedef void   (*PFNGLLINKPROGRAMPROC)(GLuint);
typedef void   (*PFNGLGETPROGRAMIVPROC)(GLuint, GLenum, GLint*);
typedef void   (*PFNGLGETPROGRAMINFOLOGPROC)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void   (*PFNGLUSEPROGRAMPROC)(GLuint);
typedef void   (*PFNGLDELETESHADERPROC)(GLuint);
typedef void   (*PFNGLDELETEPROGRAMPROC)(GLuint);
typedef GLint  (*PFNGLGETUNIFORMLOCATIONPROC)(GLuint, const GLchar*);
typedef GLint  (*PFNGLGETATTRIBLOCATIONPROC)(GLuint, const GLchar*);
typedef void   (*PFNGLUNIFORM1IPROC)(GLint, GLint);
typedef void   (*PFNGLUNIFORM1FPROC)(GLint, GLfloat);
typedef void   (*PFNGLUNIFORM2FPROC)(GLint, GLfloat, GLfloat);
typedef void   (*PFNGLUNIFORM3FPROC)(GLint, GLfloat, GLfloat, GLfloat);
typedef void   (*PFNGLUNIFORM4FPROC)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void   (*PFNGLUNIFORMMATRIX4FVPROC)(GLint, GLsizei, GLboolean, const GLfloat*);

#define NX_GL_FUNC(type, name) static type nx_##name = nullptr

NX_GL_FUNC(PFNGLCREATESHADERPROC, glCreateShader);
NX_GL_FUNC(PFNGLSHADERSOURCEPROC, glShaderSource);
NX_GL_FUNC(PFNGLCOMPILESHADERPROC, glCompileShader);
NX_GL_FUNC(PFNGLGETSHADERIVPROC, glGetShaderiv);
NX_GL_FUNC(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog);
NX_GL_FUNC(PFNGLCREATEPROGRAMPROC, glCreateProgram);
NX_GL_FUNC(PFNGLATTACHSHADERPROC, glAttachShader);
NX_GL_FUNC(PFNGLLINKPROGRAMPROC, glLinkProgram);
NX_GL_FUNC(PFNGLGETPROGRAMIVPROC, glGetProgramiv);
NX_GL_FUNC(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog);
NX_GL_FUNC(PFNGLUSEPROGRAMPROC, glUseProgram);
NX_GL_FUNC(PFNGLDELETESHADERPROC, glDeleteShader);
NX_GL_FUNC(PFNGLDELETEPROGRAMPROC, glDeleteProgram);
NX_GL_FUNC(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation);
NX_GL_FUNC(PFNGLGETATTRIBLOCATIONPROC, glGetAttribLocation);
NX_GL_FUNC(PFNGLUNIFORM1IPROC, glUniform1i);
NX_GL_FUNC(PFNGLUNIFORM1FPROC, glUniform1f);
NX_GL_FUNC(PFNGLUNIFORM2FPROC, glUniform2f);
NX_GL_FUNC(PFNGLUNIFORM3FPROC, glUniform3f);
NX_GL_FUNC(PFNGLUNIFORM4FPROC, glUniform4f);
NX_GL_FUNC(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv);

#undef NX_GL_FUNC

static bool s_glFuncsLoaded = false;

static void* getGLProc(const char* name) {
#ifdef __linux__
    return reinterpret_cast<void*>(glXGetProcAddress(reinterpret_cast<const GLubyte*>(name)));
#elif _WIN32
    return reinterpret_cast<void*>(wglGetProcAddress(name));
#elif defined(__APPLE__)
    return nxAppleGetProcAddress(name);
#else
    return nullptr;
#endif
}

static void loadShaderFunctions() {
    if (s_glFuncsLoaded) return;
    s_glFuncsLoaded = true;

    #define LOAD(name) nx_##name = reinterpret_cast<decltype(nx_##name)>(getGLProc(#name))
    LOAD(glCreateShader);
    LOAD(glShaderSource);
    LOAD(glCompileShader);
    LOAD(glGetShaderiv);
    LOAD(glGetShaderInfoLog);
    LOAD(glCreateProgram);
    LOAD(glAttachShader);
    LOAD(glLinkProgram);
    LOAD(glGetProgramiv);
    LOAD(glGetProgramInfoLog);
    LOAD(glUseProgram);
    LOAD(glDeleteShader);
    LOAD(glDeleteProgram);
    LOAD(glGetUniformLocation);
    LOAD(glGetAttribLocation);
    LOAD(glUniform1i);
    LOAD(glUniform1f);
    LOAD(glUniform2f);
    LOAD(glUniform3f);
    LOAD(glUniform4f);
    LOAD(glUniformMatrix4fv);
    #undef LOAD
}

// ==================================================================
// ShaderProgram
// ==================================================================

ShaderProgram::ShaderProgram() {
    loadShaderFunctions();
}

ShaderProgram::~ShaderProgram() {
    destroy();
}

ShaderProgram::ShaderProgram(ShaderProgram&& other) noexcept
    : programId_(other.programId_)
    , errorLog_(std::move(other.errorLog_))
    , uniformCache_(std::move(other.uniformCache_))
    , uniformValueCache_(std::move(other.uniformValueCache_)) {
    other.programId_ = 0;
}

ShaderProgram& ShaderProgram::operator=(ShaderProgram&& other) noexcept {
    if (this != &other) {
        destroy();
        programId_ = other.programId_;
        errorLog_ = std::move(other.errorLog_);
        uniformCache_ = std::move(other.uniformCache_);
        uniformValueCache_ = std::move(other.uniformValueCache_);
        other.programId_ = 0;
    }
    return *this;
}

uint32_t ShaderProgram::compileShader(uint32_t type, const char* source) {
    if (!nx_glCreateShader || !nx_glShaderSource || !nx_glCompileShader) return 0;

    GLuint shader = nx_glCreateShader(type);
    if (shader == 0) return 0;

    nx_glShaderSource(shader, 1, &source, nullptr);
    nx_glCompileShader(shader);

    GLint success = 0;
    nx_glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[1024];
        nx_glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        errorLog_ = "[Shader Compile] ";
        errorLog_ += (type == GL_VERTEX_SHADER ? "VERTEX: " : "FRAGMENT: ");
        errorLog_ += log;
        std::cerr << errorLog_ << std::endl;
        nx_glDeleteShader(shader);
        return 0;
    }

    return shader;
}

bool ShaderProgram::compile(const char* vertexSrc, const char* fragmentSrc) {
    destroy();
    errorLog_.clear();

    GLuint vs = compileShader(GL_VERTEX_SHADER, vertexSrc);
    if (vs == 0) return false;

    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentSrc);
    if (fs == 0) {
        nx_glDeleteShader(vs);
        return false;
    }

    programId_ = nx_glCreateProgram();
    nx_glAttachShader(programId_, vs);
    nx_glAttachShader(programId_, fs);
    nx_glLinkProgram(programId_);

    GLint success = 0;
    nx_glGetProgramiv(programId_, GL_LINK_STATUS, &success);
    if (!success) {
        char log[1024];
        nx_glGetProgramInfoLog(programId_, sizeof(log), nullptr, log);
        errorLog_ = "[Shader Link] ";
        errorLog_ += log;
        std::cerr << errorLog_ << std::endl;
        nx_glDeleteProgram(programId_);
        programId_ = 0;
    }

    nx_glDeleteShader(vs);
    nx_glDeleteShader(fs);

    return programId_ != 0;
}

void ShaderProgram::bind() const {
    if (nx_glUseProgram && programId_) {
        nx_glUseProgram(programId_);
    }
}

void ShaderProgram::unbind() {
    if (nx_glUseProgram) {
        nx_glUseProgram(0);
    }
}

void ShaderProgram::destroy() {
    if (programId_ && nx_glDeleteProgram) {
        nx_glDeleteProgram(programId_);
        programId_ = 0;
    }
    uniformCache_.clear();
    uniformValueCache_.clear();
}

int ShaderProgram::getUniformLocation(const char* name) {
    auto it = uniformCache_.find(name);
    if (it != uniformCache_.end()) return it->second;

    if (!nx_glGetUniformLocation || !programId_) return -1;
    int loc = nx_glGetUniformLocation(programId_, name);
    uniformCache_[name] = loc;
    return loc;
}

int ShaderProgram::getAttribLocation(const char* name) {
    if (!nx_glGetAttribLocation || !programId_) return -1;
    return nx_glGetAttribLocation(programId_, name);
}

void ShaderProgram::setUniform1i(const char* name, int value) {
    int loc = getUniformLocation(name);
    if (loc < 0) return;

    auto& cached = uniformValueCache_[loc];
    if (cached.type == 1 && cached.ival == value) return;
    cached.type = 1;
    cached.ival = value;

    if (nx_glUniform1i) nx_glUniform1i(loc, value);
}

void ShaderProgram::setUniform1f(const char* name, float value) {
    int loc = getUniformLocation(name);
    if (loc < 0) return;

    auto& cached = uniformValueCache_[loc];
    if (cached.type == 2 && cached.fval == value) return;
    cached.type = 2;
    cached.fval = value;

    if (nx_glUniform1f) nx_glUniform1f(loc, value);
}

void ShaderProgram::setUniform2f(const char* name, float x, float y) {
    int loc = getUniformLocation(name);
    if (loc < 0) return;

    auto& cached = uniformValueCache_[loc];
    if (cached.type == 3 && cached.fval4[0] == x && cached.fval4[1] == y) return;
    cached.type = 3;
    cached.fval4[0] = x;
    cached.fval4[1] = y;

    if (nx_glUniform2f) nx_glUniform2f(loc, x, y);
}

void ShaderProgram::setUniform3f(const char* name, float x, float y, float z) {
    int loc = getUniformLocation(name);
    if (loc < 0) return;

    auto& cached = uniformValueCache_[loc];
    if (cached.type == 4 && cached.fval4[0] == x && cached.fval4[1] == y && cached.fval4[2] == z) return;
    cached.type = 4;
    cached.fval4[0] = x;
    cached.fval4[1] = y;
    cached.fval4[2] = z;

    if (nx_glUniform3f) nx_glUniform3f(loc, x, y, z);
}

void ShaderProgram::setUniform4f(const char* name, float x, float y, float z, float w) {
    int loc = getUniformLocation(name);
    if (loc < 0) return;

    auto& cached = uniformValueCache_[loc];
    if (cached.type == 5 && cached.fval4[0] == x && cached.fval4[1] == y &&
        cached.fval4[2] == z && cached.fval4[3] == w) return;
    cached.type = 5;
    cached.fval4[0] = x;
    cached.fval4[1] = y;
    cached.fval4[2] = z;
    cached.fval4[3] = w;

    if (nx_glUniform4f) nx_glUniform4f(loc, x, y, z, w);
}

void ShaderProgram::setUniformMat4(const char* name, const float* matrix) {
    int loc = getUniformLocation(name);
    if (loc < 0) return;

    auto& cached = uniformValueCache_[loc];
    if (cached.type == 6 && memcmp(cached.mat4, matrix, 16 * sizeof(float)) == 0) return;
    cached.type = 6;
    memcpy(cached.mat4, matrix, 16 * sizeof(float));

    if (nx_glUniformMatrix4fv) nx_glUniformMatrix4fv(loc, 1, GL_FALSE, matrix);
}

// ==================================================================
// Built-in Shader Sources
// ==================================================================

namespace Shaders {

const char* kSolidColorVert = R"(
#version 120
attribute vec2 aPosition;
uniform mat4 uProjection;
void main() {
    gl_Position = uProjection * vec4(aPosition, 0.0, 1.0);
}
)";

const char* kSolidColorFrag = R"(
#version 120
uniform vec4 uColor;
void main() {
    gl_FragColor = uColor;
}
)";

const char* kTexturedVert = R"(
#version 120
attribute vec2 aPosition;
attribute vec2 aTexCoord;
attribute vec4 aColor;
uniform mat4 uProjection;
varying vec2 vTexCoord;
varying vec4 vColor;
void main() {
    gl_Position = uProjection * vec4(aPosition, 0.0, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
}
)";

const char* kTexturedFrag = R"(
#version 120
uniform sampler2D uTexture;
varying vec2 vTexCoord;
varying vec4 vColor;
void main() {
    gl_FragColor = texture2D(uTexture, vTexCoord) * vColor;
}
)";

const char* kRoundedRectVert = R"(
#version 120
attribute vec2 aPosition;
attribute vec2 aTexCoord;
uniform mat4 uProjection;
varying vec2 vLocalPos;
void main() {
    gl_Position = uProjection * vec4(aPosition, 0.0, 1.0);
    vLocalPos = aTexCoord;
}
)";

const char* kRoundedRectFrag = R"(
#version 120
uniform vec4 uColor;
uniform vec2 uSize;
uniform float uRadius;
varying vec2 vLocalPos;

float roundedBoxSDF(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
    vec2 halfSize = uSize * 0.5;
    vec2 p = vLocalPos * halfSize;
    float d = roundedBoxSDF(p, halfSize, uRadius);
    float aa = fwidth(d);
    float alpha = 1.0 - smoothstep(-aa, aa, d);
    gl_FragColor = uColor * vec4(1.0, 1.0, 1.0, alpha);
}
)";

const char* kTextSDFVert = R"(
#version 120
attribute vec2 aPosition;
attribute vec2 aTexCoord;
attribute vec4 aColor;
uniform mat4 uProjection;
varying vec2 vTexCoord;
varying vec4 vColor;
void main() {
    gl_Position = uProjection * vec4(aPosition, 0.0, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
}
)";

const char* kTextSDFFrag = R"(
#version 120
uniform sampler2D uTexture;
varying vec2 vTexCoord;
varying vec4 vColor;
void main() {
    float a = texture2D(uTexture, vTexCoord).r;
    gl_FragColor = vColor * vec4(1.0, 1.0, 1.0, a);
}
)";

const char* kLinearGradientVert = R"(
#version 120
attribute vec2 aPosition;
attribute vec2 aTexCoord;
uniform mat4 uProjection;
varying vec2 vTexCoord;
void main() {
    gl_Position = uProjection * vec4(aPosition, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

const char* kLinearGradientFrag = R"(
#version 120
uniform vec4 uColorStart;
uniform vec4 uColorEnd;
uniform vec2 uDirection;
varying vec2 vTexCoord;
void main() {
    float t = dot(vTexCoord, uDirection);
    t = clamp(t, 0.0, 1.0);
    gl_FragColor = mix(uColorStart, uColorEnd, t);
}
)";

const char* kRadialGradientVert = R"(
#version 120
attribute vec2 aPosition;
attribute vec2 aTexCoord;
uniform mat4 uProjection;
varying vec2 vTexCoord;
void main() {
    gl_Position = uProjection * vec4(aPosition, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

const char* kRadialGradientFrag = R"(
#version 120
uniform vec4 uColorStart;
uniform vec4 uColorEnd;
uniform vec2 uCenter;
uniform float uRadius;
varying vec2 vTexCoord;
void main() {
    float d = length(vTexCoord - uCenter);
    float t = clamp(d / uRadius, 0.0, 1.0);
    gl_FragColor = mix(uColorStart, uColorEnd, t);
}
)";

const char* kBoxShadowVert = R"(
#version 120
attribute vec2 aPosition;
attribute vec2 aTexCoord;
uniform mat4 uProjection;
varying vec2 vTexCoord;
void main() {
    gl_Position = uProjection * vec4(aPosition, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

const char* kBoxShadowFrag = R"(
#version 120
uniform vec4 uColor;
uniform vec2 uSize;
uniform float uBlur;
uniform float uRadius;
varying vec2 vTexCoord;

float roundedBoxSDF(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
    vec2 halfSize = uSize * 0.5;
    vec2 p = (vTexCoord - 0.5) * (uSize + vec2(uBlur * 4.0));
    float d = roundedBoxSDF(p, halfSize, uRadius);
    float sigma = uBlur * 0.5;
    float alpha = 1.0 - smoothstep(-sigma, sigma, d);
    alpha *= alpha;
    gl_FragColor = uColor * vec4(1.0, 1.0, 1.0, alpha);
}
)";

// Shader instances
static ShaderProgram s_solidColor;
static ShaderProgram s_textured;
static ShaderProgram s_roundedRect;
static ShaderProgram s_textSDF;
static ShaderProgram s_linearGradient;
static ShaderProgram s_radialGradient;
static ShaderProgram s_boxShadow;
static bool s_initialized = false;

bool initBuiltinShaders() {
    if (s_initialized) return true;

    bool ok = true;

    ok = s_solidColor.compile(kSolidColorVert, kSolidColorFrag) && ok;
    ok = s_textured.compile(kTexturedVert, kTexturedFrag) && ok;
    ok = s_roundedRect.compile(kRoundedRectVert, kRoundedRectFrag) && ok;
    ok = s_textSDF.compile(kTextSDFVert, kTextSDFFrag) && ok;
    ok = s_linearGradient.compile(kLinearGradientVert, kLinearGradientFrag) && ok;
    ok = s_radialGradient.compile(kRadialGradientVert, kRadialGradientFrag) && ok;
    ok = s_boxShadow.compile(kBoxShadowVert, kBoxShadowFrag) && ok;

    if (ok) {
        s_initialized = true;
        std::cout << "[Shaders] All 7 built-in shaders compiled" << std::endl;
    } else {
        std::cerr << "[Shaders] Failed to compile one or more built-in shaders" << std::endl;
    }

    return ok;
}

void destroyBuiltinShaders() {
    s_solidColor = ShaderProgram();
    s_textured = ShaderProgram();
    s_roundedRect = ShaderProgram();
    s_textSDF = ShaderProgram();
    s_linearGradient = ShaderProgram();
    s_radialGradient = ShaderProgram();
    s_boxShadow = ShaderProgram();
    s_initialized = false;
}

ShaderProgram* solidColor()     { return &s_solidColor; }
ShaderProgram* textured()       { return &s_textured; }
ShaderProgram* roundedRect()    { return &s_roundedRect; }
ShaderProgram* textSDF()        { return &s_textSDF; }
ShaderProgram* linearGradient() { return &s_linearGradient; }
ShaderProgram* radialGradient() { return &s_radialGradient; }
ShaderProgram* boxShadow()      { return &s_boxShadow; }

} // namespace Shaders

} // namespace NXRender
