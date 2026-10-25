// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#pragma once

/**
 * @file webgl_context.h
 * @brief WebGL 1.0 for <canvas>, drawn with desktop OpenGL into an FBO
 *
 * Page scripts never touch GL: the bindings record WebGLCommands, and
 * flush() replays them into the drawing buffer (an FBO with a colour
 * texture) when a GL context is current, normally while the browser
 * paints the canvas. flush() saves the browser's GL state, applies the
 * WebGL state left by the previous flush, replays, and restores the
 * browser's state. Shaders are translated from GLSL ES 1.00 to GLSL 1.20.
 */

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace NXRender {

// WebGL constants (subset matching GL ES 2.0)
namespace GL {
    constexpr uint32_t DEPTH_BUFFER_BIT = 0x00000100;
    constexpr uint32_t STENCIL_BUFFER_BIT = 0x00000400;
    constexpr uint32_t COLOR_BUFFER_BIT = 0x00004000;
    constexpr uint32_t VERTEX_SHADER = 0x8B31;
    constexpr uint32_t FRAGMENT_SHADER = 0x8B30;
    constexpr uint32_t ARRAY_BUFFER = 0x8892;
    constexpr uint32_t ELEMENT_ARRAY_BUFFER = 0x8893;
    constexpr uint32_t STATIC_DRAW = 0x88E4;
    constexpr uint32_t DYNAMIC_DRAW = 0x88E8;
    constexpr uint32_t STREAM_DRAW = 0x88E0;
    constexpr uint32_t FLOAT = 0x1406;
    constexpr uint32_t UNSIGNED_BYTE = 0x1401;
    constexpr uint32_t UNSIGNED_SHORT = 0x1403;
    constexpr uint32_t UNSIGNED_INT = 0x1405;
    constexpr uint32_t TRIANGLES = 0x0004;
    constexpr uint32_t TRIANGLE_STRIP = 0x0005;
    constexpr uint32_t TRIANGLE_FAN = 0x0006;
    constexpr uint32_t LINES = 0x0001;
    constexpr uint32_t LINE_STRIP = 0x0003;
    constexpr uint32_t LINE_LOOP = 0x0002;
    constexpr uint32_t POINTS = 0x0000;
    constexpr uint32_t TEXTURE_2D = 0x0DE1;
    constexpr uint32_t TEXTURE_CUBE_MAP = 0x8513;
    constexpr uint32_t TEXTURE0 = 0x84C0;
    constexpr uint32_t RGBA = 0x1908;
    constexpr uint32_t RGB = 0x1907;
    constexpr uint32_t LUMINANCE = 0x1909;
    constexpr uint32_t ALPHA = 0x1906;
    constexpr uint32_t NEAREST = 0x2600;
    constexpr uint32_t LINEAR = 0x2601;
    constexpr uint32_t FRAMEBUFFER = 0x8D40;
    constexpr uint32_t RENDERBUFFER = 0x8D41;
    constexpr uint32_t DEPTH_TEST = 0x0B71;
    constexpr uint32_t BLEND = 0x0BE2;
    constexpr uint32_t STENCIL_TEST = 0x0B90;
    constexpr uint32_t SCISSOR_TEST = 0x0C11;
    constexpr uint32_t CULL_FACE = 0x0B44;
    constexpr uint32_t DITHER = 0x0BD0;
    constexpr uint32_t POLYGON_OFFSET_FILL = 0x8037;
    constexpr uint32_t SAMPLE_ALPHA_TO_COVERAGE = 0x809E;
    constexpr uint32_t SAMPLE_COVERAGE = 0x80A0;
    constexpr uint32_t UNPACK_ALIGNMENT = 0x0CF5;
    constexpr uint32_t PACK_ALIGNMENT = 0x0D05;
    constexpr uint32_t UNPACK_FLIP_Y_WEBGL = 0x9240;
    constexpr uint32_t UNPACK_PREMULTIPLY_ALPHA_WEBGL = 0x9241;
}

/// getContext("webgl", attributes)
struct WebGLContextAttributes {
    bool alpha = true;
    bool depth = true;
    bool stencil = false;
    bool antialias = true;  // 4x MSAA when the driver has multisampled renderbuffers
    bool premultipliedAlpha = true;
    bool preserveDrawingBuffer = false;
};

/// One recorded WebGL call. Object arguments are handles from newHandle().
enum class WebGLOp : uint8_t {
    CreateShader,        // u0 handle, u1 type
    ShaderSource,        // u0 shader, text
    CompileShader,       // u0 shader
    DeleteShader,        // u0 shader
    CreateProgram,       // u0 handle
    AttachShader,        // u0 program, u1 shader
    DetachShader,        // u0 program, u1 shader
    BindAttribLocation,  // u0 program, u1 index, text name
    LinkProgram,         // u0 program
    UseProgram,          // u0 program (0 = none)
    DeleteProgram,       // u0 program
    CreateBuffer,        // u0 handle
    BindBuffer,          // u0 target, u1 buffer
    BufferData,          // u0 target, u1 usage, u2 size; data (empty = zeroed)
    BufferSubData,       // u0 target, u1 offset; data
    DeleteBuffer,        // u0 buffer
    CreateTexture,       // u0 handle
    BindTexture,         // u0 target, u1 texture
    ActiveTexture,       // u0 unit enum (TEXTURE0 + i)
    TexImage2D,          // u0 target, u1 level, u2 internalformat, u3 width, u4 height, u5 format, u6 type; data
    TexSubImage2D,       // u0 target, u1 level, u2 x, u3 y, u4 width, u5 height, u6 format, u7 type; data
    TexParameteri,       // u0 target, u1 pname, u2 param
    GenerateMipmap,      // u0 target
    DeleteTexture,       // u0 texture
    EnableVertexAttribArray,   // u0 index
    DisableVertexAttribArray,  // u0 index
    VertexAttribPointer, // u0 index, u1 size, u2 type, u3 normalized, u4 stride, u5 offset
    VertexAttrib4f,      // u0 index, f0..f3
    Uniform,             // u0 program, u1 kind (UniformKind), u2 count, u3 transpose; text name; data
    Viewport,            // u0..u3 (signed as int32)
    Scissor,             // u0..u3
    ClearColor,          // f0..f3
    ClearDepth,          // f0
    ClearStencil,        // u0
    Clear,               // u0 mask
    Enable,              // u0 cap
    Disable,             // u0 cap
    BlendFuncSeparate,   // u0 srcRGB, u1 dstRGB, u2 srcAlpha, u3 dstAlpha
    BlendEquationSeparate, // u0 rgb, u1 alpha
    BlendColor,          // f0..f3
    DepthFunc,           // u0
    DepthMask,           // u0
    DepthRange,          // f0, f1
    ColorMask,           // u0..u3
    StencilFuncSeparate, // u0 face, u1 func, u2 ref, u3 mask
    StencilOpSeparate,   // u0 face, u1 fail, u2 zfail, u3 zpass
    StencilMaskSeparate, // u0 face, u1 mask
    CullFace,            // u0
    FrontFace,           // u0
    LineWidth,           // f0
    PolygonOffset,       // f0, f1
    PixelStorei,         // u0 pname, u1 param
    DrawArrays,          // u0 mode, u1 first, u2 count
    DrawElements,        // u0 mode, u1 count, u2 type, u3 offset
};

/// The value layout of a WebGLOp::Uniform
enum class UniformKind : uint8_t { F1, F2, F3, F4, I1, I2, I3, I4, Mat2, Mat3, Mat4 };

struct WebGLCommand {
    WebGLOp op;
    uint32_t u[8] = {};
    float f[4] = {};
    std::string text;
    std::vector<uint8_t> data;
};

class WebGLRenderingContext {
public:
    WebGLRenderingContext(int width, int height, WebGLContextAttributes attributes = {});
    /// Deletes the GL objects when a GL context is current (else they are left to the context)
    ~WebGLRenderingContext();
    WebGLRenderingContext(const WebGLRenderingContext&) = delete;
    WebGLRenderingContext& operator=(const WebGLRenderingContext&) = delete;

    int drawingBufferWidth() const { return width_; }
    int drawingBufferHeight() const { return height_; }
    const WebGLContextAttributes& attributes() const { return attributes_; }
    /// The canvas size changed: the drawing buffer is reallocated and cleared
    void resize(int width, int height);

    // ---- Recording (no GL needed) ----
    uint32_t newHandle() { return nextHandle_++; }
    void record(WebGLCommand command);
    size_t pendingCommands() const { return pending_.size(); }
    const std::vector<WebGLCommand>& pending() const { return pending_; }
    size_t pendingBytes() const { return pendingBytes_; }

    // ---- Execution (a GL context must be current) ----
    /// True when this thread has a current GL context
    static bool glAvailable();
    /// Replays the pending commands into the drawing buffer (and resolves it into
    /// texture() when antialiased). False without GL.
    bool flush();
    /// flush(), then reads RGBA bytes of a rectangle (bottom row first, as GL)
    bool readPixels(int x, int y, int width, int height, uint8_t* rgba);
    /// The drawing buffer's colour texture (bottom row first), 0 before the first flush
    uint32_t texture() const { return colorTexture_; }
    /// The browser composited the drawing buffer: without preserveDrawingBuffer the
    /// next frame starts from a cleared buffer
    void markPresented() { presented_ = true; }

    /// What the driver said about a shader or program; unknown until it was replayed
    struct CompileResult {
        bool known = false;
        bool ok = true;
        std::string log;
    };
    CompileResult shaderResult(uint32_t shader) const;
    CompileResult programResult(uint32_t program) const;

    // ---- GLSL ----
    /// GLSL ES 1.00 source as GLSL 1.20: #version 120, precision statements removed,
    /// lowp/mediump/highp defined away
    static std::string translateShader(const std::string& source);
    /// Names declared with `qualifier` ("attribute", "uniform") at global scope, in order
    static std::vector<std::string> declaredNames(const std::string& source, const std::string& qualifier);

private:
    struct AttribState {
        bool enabled = false;
        int size = 4;
        uint32_t type = GL::FLOAT;
        bool normalized = false;
        int stride = 0;
        uint32_t offset = 0;
        uint32_t buffer = 0;  // handle
        float value[4] = {0, 0, 0, 1};
    };
    // WebGL state, applied at the start of every flush
    struct State {
        uint32_t program = 0, arrayBuffer = 0, elementBuffer = 0;
        uint32_t activeTexture = 0;  // unit index
        std::map<uint32_t, uint32_t> textures2D, texturesCube;  // unit -> handle
        AttribState attribs[16];
        int viewport[4] = {0, 0, 0, 0};
        int scissor[4] = {0, 0, 0, 0};
        float clearColor[4] = {0, 0, 0, 0};
        float clearDepth = 1;
        int clearStencil = 0;
        std::map<uint32_t, bool> caps;
        uint32_t blend[4] = {1, 0, 1, 0};  // ONE, ZERO
        uint32_t blendEquation[2] = {0x8006, 0x8006};  // FUNC_ADD
        float blendColor[4] = {0, 0, 0, 0};
        uint32_t depthFunc = 0x0201;  // LESS
        bool depthMask = true;
        float depthRange[2] = {0, 1};
        bool colorMask[4] = {true, true, true, true};
        uint32_t stencilFunc[2][3] = {{0x0207, 0, 0xFFFFFFFF}, {0x0207, 0, 0xFFFFFFFF}};  // ALWAYS
        uint32_t stencilOp[2][3] = {{0x1E00, 0x1E00, 0x1E00}, {0x1E00, 0x1E00, 0x1E00}};  // KEEP
        uint32_t stencilMask[2] = {0xFFFFFFFF, 0xFFFFFFFF};
        uint32_t cullFace = 0x0405;   // BACK
        uint32_t frontFace = 0x0901;  // CCW
        float lineWidth = 1;
        float polygonOffset[2] = {0, 0};
        int unpackAlignment = 4;
        bool unpackFlipY = false;
    };

    void ensureDrawingBuffer();
    void releaseDrawingBuffer();
    void applyState();
    void execute(const WebGLCommand& command);
    void clearDrawingBuffer();
    uint32_t glName(const std::unordered_map<uint32_t, uint32_t>& names, uint32_t handle) const;
    int uniformLocation(uint32_t program, const std::string& name);

    int width_, height_;
    WebGLContextAttributes attributes_;
    uint32_t nextHandle_ = 1;
    std::vector<WebGLCommand> pending_;
    size_t pendingBytes_ = 0;
    State state_;
    bool presented_ = false;
    bool needsClear_ = true;

    // GL objects
    uint32_t fbo_ = 0, colorTexture_ = 0, depthStencil_ = 0;
    uint32_t msFbo_ = 0, msColor_ = 0;  // multisampled drawing buffer (antialias)
    int bufferWidth_ = 0, bufferHeight_ = 0;
    std::unordered_map<uint32_t, uint32_t> shaders_, programs_, buffers_, textures_;
    std::unordered_map<uint32_t, uint32_t> shaderTypes_;
    std::unordered_map<uint32_t, CompileResult> shaderResults_, programResults_;
    std::map<std::pair<uint32_t, std::string>, int> uniformLocations_;
};

} // namespace NXRender
