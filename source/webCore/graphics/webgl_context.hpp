/**
 * @file webgl_context.hpp
 * @brief WebGL rendering context - OpenGL ES 2.0 / WebGL 1.0 compatible API
 * 
 * Provides a C++ wrapper around OpenGL that exposes the WebGL JavaScript API.
 * This enables 3D graphics rendering in web pages.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

#ifdef ZEPRA_HAS_OPENGL
#ifdef __APPLE__
#  ifndef GL_SILENCE_DEPRECATION
#    define GL_SILENCE_DEPRECATION
#  endif
#  include <OpenGL/gl.h>
#else
#  include <GL/gl.h>
#endif
#endif

// Windows winerror.h defines NO_ERROR as a macro (=0L).
// Undef it to avoid collision with our WebGLConstants::NO_ERROR.
#ifdef NO_ERROR
#  undef NO_ERROR
#endif

namespace Zepra::WebCore {

// =============================================================================
// WebGL Type Definitions (matching WebGL spec)
// =============================================================================

using GLboolean = uint8_t;
using GLbyte = int8_t;
using GLubyte = uint8_t;
using GLshort = int16_t;
using GLushort = uint16_t;
using GLint = int32_t;
using GLuint = uint32_t;
using GLsizei = int32_t;
using GLenum = uint32_t;
using GLfloat = float;
using GLclampf = float;
using GLintptr = intptr_t;
using GLsizeiptr = intptr_t;

// =============================================================================
// WebGL Constants
// =============================================================================

namespace WebGLConstants {
    // Clear buffer bits
    constexpr GLenum DEPTH_BUFFER_BIT = 0x00000100;
    constexpr GLenum STENCIL_BUFFER_BIT = 0x00000400;
    constexpr GLenum COLOR_BUFFER_BIT = 0x00004000;
    
    // Primitive types
    constexpr GLenum POINTS = 0x0000;
    constexpr GLenum LINES = 0x0001;
    constexpr GLenum LINE_LOOP = 0x0002;
    constexpr GLenum LINE_STRIP = 0x0003;
    constexpr GLenum TRIANGLES = 0x0004;
    constexpr GLenum TRIANGLE_STRIP = 0x0005;
    constexpr GLenum TRIANGLE_FAN = 0x0006;
    
    // Blend modes
    constexpr GLenum ZERO = 0;
    constexpr GLenum ONE = 1;
    constexpr GLenum SRC_COLOR = 0x0300;
    constexpr GLenum ONE_MINUS_SRC_COLOR = 0x0301;
    constexpr GLenum SRC_ALPHA = 0x0302;
    constexpr GLenum ONE_MINUS_SRC_ALPHA = 0x0303;
    constexpr GLenum DST_ALPHA = 0x0304;
    constexpr GLenum ONE_MINUS_DST_ALPHA = 0x0305;
    
    // Enable/Disable
    constexpr GLenum CULL_FACE = 0x0B44;
    constexpr GLenum BLEND = 0x0BE2;
    constexpr GLenum DEPTH_TEST = 0x0B71;
    constexpr GLenum SCISSOR_TEST = 0x0C11;
    
    // Buffer types
    constexpr GLenum ARRAY_BUFFER = 0x8892;
    constexpr GLenum ELEMENT_ARRAY_BUFFER = 0x8893;
    
    // Buffer usage
    constexpr GLenum STREAM_DRAW = 0x88E0;
    constexpr GLenum STATIC_DRAW = 0x88E4;
    constexpr GLenum DYNAMIC_DRAW = 0x88E8;
    
    // Data types
    constexpr GLenum BYTE = 0x1400;
    constexpr GLenum UNSIGNED_BYTE = 0x1401;
    constexpr GLenum SHORT = 0x1402;
    constexpr GLenum UNSIGNED_SHORT = 0x1403;
    constexpr GLenum INT = 0x1404;
    constexpr GLenum UNSIGNED_INT = 0x1405;
    constexpr GLenum FLOAT = 0x1406;
    
    // Shader types
    constexpr GLenum FRAGMENT_SHADER = 0x8B30;
    constexpr GLenum VERTEX_SHADER = 0x8B31;
    
    // Shader status
    constexpr GLenum COMPILE_STATUS = 0x8B81;
    constexpr GLenum LINK_STATUS = 0x8B82;
    constexpr GLenum VALIDATE_STATUS = 0x8B83;
    
    // Texture
    constexpr GLenum TEXTURE_2D = 0x0DE1;
    constexpr GLenum TEXTURE0 = 0x84C0;
    constexpr GLenum TEXTURE1 = 0x84C1;
    constexpr GLenum TEXTURE_MAG_FILTER = 0x2800;
    constexpr GLenum TEXTURE_MIN_FILTER = 0x2801;
    constexpr GLenum TEXTURE_WRAP_S = 0x2802;
    constexpr GLenum TEXTURE_WRAP_T = 0x2803;
    constexpr GLenum NEAREST = 0x2600;
    constexpr GLenum LINEAR = 0x2601;
    constexpr GLenum CLAMP_TO_EDGE = 0x812F;
    constexpr GLenum REPEAT = 0x2901;
    
    // Pixel formats
    constexpr GLenum ALPHA = 0x1906;
    constexpr GLenum RGB = 0x1907;
    constexpr GLenum RGBA = 0x1908;
    
    // Framebuffer
    constexpr GLenum FRAMEBUFFER = 0x8D40;
    constexpr GLenum RENDERBUFFER = 0x8D41;
    constexpr GLenum COLOR_ATTACHMENT0 = 0x8CE0;
    constexpr GLenum DEPTH_ATTACHMENT = 0x8D00;
    
    // Errors
    constexpr GLenum NO_ERROR = 0;
    constexpr GLenum INVALID_ENUM = 0x0500;
    constexpr GLenum INVALID_VALUE = 0x0501;
    constexpr GLenum INVALID_OPERATION = 0x0502;
    constexpr GLenum OUT_OF_MEMORY = 0x0505;
}

// =============================================================================
// WebGL Object Handles
// =============================================================================

struct WebGLShader { GLuint id = 0; };
struct WebGLProgram { GLuint id = 0; };
struct WebGLBuffer { GLuint id = 0; };
struct WebGLTexture { GLuint id = 0; };
struct WebGLFramebuffer { GLuint id = 0; };
struct WebGLRenderbuffer { GLuint id = 0; };
struct WebGLUniformLocation { GLint loc = -1; };

// =============================================================================
// WebGLRenderingContext
// =============================================================================

/**
 * @brief WebGL rendering context - mirrors the JavaScript WebGLRenderingContext API
 */
class WebGLRenderingContext {
public:
    WebGLRenderingContext();
    ~WebGLRenderingContext();
    
    /**
     * @brief Initialize the context with given dimensions
     */
    bool initialize(int width, int height);
    
    /**
     * @brief Resize the rendering surface
     */
    void resize(int width, int height);
    
    // -------------------------------------------------------------------------
    // Viewport and Clear
    // -------------------------------------------------------------------------
    
    void viewport(GLint x, GLint y, GLsizei width, GLsizei height);
    void clearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
    void clearDepth(GLclampf depth);
    void clear(GLbitfield mask);
    
    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------
    
    void enable(GLenum cap);
    void disable(GLenum cap);
    void blendFunc(GLenum sfactor, GLenum dfactor);
    void depthFunc(GLenum func);
    void cullFace(GLenum mode);
    
    // -------------------------------------------------------------------------
    // Shaders
    // -------------------------------------------------------------------------
    
    WebGLShader createShader(GLenum type);
    void shaderSource(WebGLShader shader, const std::string& source);
    void compileShader(WebGLShader shader);
    GLboolean getShaderParameter(WebGLShader shader, GLenum pname);
    std::string getShaderInfoLog(WebGLShader shader);
    void deleteShader(WebGLShader shader);
    
    // -------------------------------------------------------------------------
    // Programs
    // -------------------------------------------------------------------------
    
    WebGLProgram createProgram();
    void attachShader(WebGLProgram program, WebGLShader shader);
    void linkProgram(WebGLProgram program);
    GLboolean getProgramParameter(WebGLProgram program, GLenum pname);
    std::string getProgramInfoLog(WebGLProgram program);
    void useProgram(WebGLProgram program);
    void deleteProgram(WebGLProgram program);
    
    // -------------------------------------------------------------------------
    // Attributes
    // -------------------------------------------------------------------------
    
    GLint getAttribLocation(WebGLProgram program, const std::string& name);
    void vertexAttribPointer(GLuint index, GLint size, GLenum type, 
                             GLboolean normalized, GLsizei stride, GLintptr offset);
    void enableVertexAttribArray(GLuint index);
    void disableVertexAttribArray(GLuint index);
    
    // -------------------------------------------------------------------------
    // Uniforms
    // -------------------------------------------------------------------------
    
    WebGLUniformLocation getUniformLocation(WebGLProgram program, const std::string& name);
    void uniform1i(WebGLUniformLocation loc, GLint v);
    void uniform1f(WebGLUniformLocation loc, GLfloat v);
    void uniform2f(WebGLUniformLocation loc, GLfloat x, GLfloat y);
    void uniform3f(WebGLUniformLocation loc, GLfloat x, GLfloat y, GLfloat z);
    void uniform4f(WebGLUniformLocation loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
    void uniformMatrix4fv(WebGLUniformLocation loc, GLboolean transpose, const GLfloat* value);
    
    // -------------------------------------------------------------------------
    // Buffers
    // -------------------------------------------------------------------------
    
    WebGLBuffer createBuffer();
    void bindBuffer(GLenum target, WebGLBuffer buffer);
    void bufferData(GLenum target, const void* data, GLsizeiptr size, GLenum usage);
    void bufferSubData(GLenum target, GLintptr offset, const void* data, GLsizeiptr size);
    void deleteBuffer(WebGLBuffer buffer);
    
    // -------------------------------------------------------------------------
    // Textures
    // -------------------------------------------------------------------------
    
    WebGLTexture createTexture();
    void bindTexture(GLenum target, WebGLTexture texture);
    void activeTexture(GLenum texture);
    void texImage2D(GLenum target, GLint level, GLenum internalformat,
                    GLsizei width, GLsizei height, GLint border,
                    GLenum format, GLenum type, const void* pixels);
    void texParameteri(GLenum target, GLenum pname, GLint param);
    void deleteTexture(WebGLTexture texture);
    
    // -------------------------------------------------------------------------
    // Drawing
    // -------------------------------------------------------------------------
    
    void drawArrays(GLenum mode, GLint first, GLsizei count);
    void drawElements(GLenum mode, GLsizei count, GLenum type, GLintptr offset);
    
    // -------------------------------------------------------------------------
    // Framebuffers
    // -------------------------------------------------------------------------
    
    WebGLFramebuffer createFramebuffer();
    void bindFramebuffer(GLenum target, WebGLFramebuffer framebuffer);
    void framebufferTexture2D(GLenum target, GLenum attachment, 
                               GLenum textarget, WebGLTexture texture, GLint level);
    void deleteFramebuffer(WebGLFramebuffer framebuffer);
    
    // -------------------------------------------------------------------------
    // Error Handling
    // -------------------------------------------------------------------------
    
    GLenum getError();
    
    // -------------------------------------------------------------------------
    // Canvas Info
    // -------------------------------------------------------------------------
    
    GLsizei drawingBufferWidth() const { return width_; }
    GLsizei drawingBufferHeight() const { return height_; }
    
private:
    bool initialized_ = false;
    GLsizei width_ = 0;
    GLsizei height_ = 0;
    
    // Current state
    WebGLProgram currentProgram_;
    WebGLBuffer currentArrayBuffer_;
    WebGLBuffer currentElementBuffer_;
    WebGLTexture currentTexture_;
    
    // Error accumulator
    GLenum lastError_ = WebGLConstants::NO_ERROR;
    
    void setError(GLenum error);
};

} // namespace Zepra::WebCore
