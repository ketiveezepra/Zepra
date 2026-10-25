// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

/**
 * @file webgl_bindings.cpp
 * @brief getContext("webgl"): the WebGLRenderingContext object for page scripts
 *
 * Calls are recorded into NXRender's WebGLRenderingContext, which replays
 * them with GL when the canvas is painted. What a script can ask back
 * without waiting for the GPU is answered here: attribute locations are
 * assigned from the vertex shader's declarations (and bound before the
 * program links), uniform locations name a declared uniform, and compile
 * and link status come from the driver when a GL context is current
 * (always, in the browser), else they are optimistic.
 */

#include "scripting/canvas_bindings.hpp"
#include "browser/dom.hpp"
#include "nxgfx/webgl/webgl_context.h"

#include "builtins/data_view.hpp"
#include "builtins/typed_array.hpp"
#include "runtime/execution/js_error.hpp"
#include "runtime/execution/vm.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"
#include "runtime/objects/well_known_symbols.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

namespace Zepra::WebCore {

using namespace Zepra::Runtime;
using Builtins::TypedArrayObject;
using Builtins::TypedArrayType;
namespace NX = NXRender;
using NX::WebGLCommand;
using NX::WebGLOp;

namespace {

// Recorded drawing is replayed early once this much is waiting (a canvas
// that is animated but never painted would otherwise grow without bound)
constexpr size_t kMaxPendingBytes = size_t(64) << 20;

// ------------------------------------------------------------------
// WebGL constants (WebGL 1.0 IDL)
// ------------------------------------------------------------------

struct Constant { const char* name; uint32_t value; };
const Constant kConstants[] = {
    {"DEPTH_BUFFER_BIT", 0x0100}, {"STENCIL_BUFFER_BIT", 0x0400}, {"COLOR_BUFFER_BIT", 0x4000},
    {"POINTS", 0}, {"LINES", 1}, {"LINE_LOOP", 2}, {"LINE_STRIP", 3}, {"TRIANGLES", 4},
    {"TRIANGLE_STRIP", 5}, {"TRIANGLE_FAN", 6},
    {"ZERO", 0}, {"ONE", 1}, {"SRC_COLOR", 0x0300}, {"ONE_MINUS_SRC_COLOR", 0x0301}, {"SRC_ALPHA", 0x0302},
    {"ONE_MINUS_SRC_ALPHA", 0x0303}, {"DST_ALPHA", 0x0304}, {"ONE_MINUS_DST_ALPHA", 0x0305},
    {"DST_COLOR", 0x0306}, {"ONE_MINUS_DST_COLOR", 0x0307}, {"SRC_ALPHA_SATURATE", 0x0308},
    {"FUNC_ADD", 0x8006}, {"BLEND_EQUATION", 0x8009}, {"BLEND_EQUATION_RGB", 0x8009},
    {"BLEND_EQUATION_ALPHA", 0x883D}, {"FUNC_SUBTRACT", 0x800A}, {"FUNC_REVERSE_SUBTRACT", 0x800B},
    {"BLEND_DST_RGB", 0x80C8}, {"BLEND_SRC_RGB", 0x80C9}, {"BLEND_DST_ALPHA", 0x80CA}, {"BLEND_SRC_ALPHA", 0x80CB},
    {"CONSTANT_COLOR", 0x8001}, {"ONE_MINUS_CONSTANT_COLOR", 0x8002}, {"CONSTANT_ALPHA", 0x8003},
    {"ONE_MINUS_CONSTANT_ALPHA", 0x8004}, {"BLEND_COLOR", 0x8005},
    {"ARRAY_BUFFER", 0x8892}, {"ELEMENT_ARRAY_BUFFER", 0x8893}, {"ARRAY_BUFFER_BINDING", 0x8894},
    {"ELEMENT_ARRAY_BUFFER_BINDING", 0x8895}, {"STREAM_DRAW", 0x88E0}, {"STATIC_DRAW", 0x88E4},
    {"DYNAMIC_DRAW", 0x88E8}, {"BUFFER_SIZE", 0x8764}, {"BUFFER_USAGE", 0x8765},
    {"CURRENT_VERTEX_ATTRIB", 0x8626}, {"FRONT", 0x0404}, {"BACK", 0x0405}, {"FRONT_AND_BACK", 0x0408},
    {"CULL_FACE", 0x0B44}, {"BLEND", 0x0BE2}, {"DITHER", 0x0BD0}, {"STENCIL_TEST", 0x0B90},
    {"DEPTH_TEST", 0x0B71}, {"SCISSOR_TEST", 0x0C11}, {"POLYGON_OFFSET_FILL", 0x8037},
    {"SAMPLE_ALPHA_TO_COVERAGE", 0x809E}, {"SAMPLE_COVERAGE", 0x80A0},
    {"NO_ERROR", 0}, {"INVALID_ENUM", 0x0500}, {"INVALID_VALUE", 0x0501}, {"INVALID_OPERATION", 0x0502},
    {"OUT_OF_MEMORY", 0x0505}, {"CW", 0x0900}, {"CCW", 0x0901},
    {"LINE_WIDTH", 0x0B21}, {"ALIASED_POINT_SIZE_RANGE", 0x846D}, {"ALIASED_LINE_WIDTH_RANGE", 0x846E},
    {"CULL_FACE_MODE", 0x0B45}, {"FRONT_FACE", 0x0B46}, {"DEPTH_RANGE", 0x0B70}, {"DEPTH_WRITEMASK", 0x0B72},
    {"DEPTH_CLEAR_VALUE", 0x0B73}, {"DEPTH_FUNC", 0x0B74}, {"STENCIL_CLEAR_VALUE", 0x0B91},
    {"STENCIL_FUNC", 0x0B92}, {"STENCIL_FAIL", 0x0B94}, {"STENCIL_PASS_DEPTH_FAIL", 0x0B95},
    {"STENCIL_PASS_DEPTH_PASS", 0x0B96}, {"STENCIL_REF", 0x0B97}, {"STENCIL_VALUE_MASK", 0x0B93},
    {"STENCIL_WRITEMASK", 0x0B98}, {"STENCIL_BACK_FUNC", 0x8800}, {"STENCIL_BACK_FAIL", 0x8801},
    {"STENCIL_BACK_PASS_DEPTH_FAIL", 0x8802}, {"STENCIL_BACK_PASS_DEPTH_PASS", 0x8803},
    {"STENCIL_BACK_REF", 0x8CA3}, {"STENCIL_BACK_VALUE_MASK", 0x8CA4}, {"STENCIL_BACK_WRITEMASK", 0x8CA5},
    {"VIEWPORT", 0x0BA2}, {"SCISSOR_BOX", 0x0C10}, {"COLOR_CLEAR_VALUE", 0x0C22}, {"COLOR_WRITEMASK", 0x0C23},
    {"UNPACK_ALIGNMENT", 0x0CF5}, {"PACK_ALIGNMENT", 0x0D05}, {"MAX_TEXTURE_SIZE", 0x0D33},
    {"MAX_VIEWPORT_DIMS", 0x0D3A}, {"SUBPIXEL_BITS", 0x0D50}, {"RED_BITS", 0x0D52}, {"GREEN_BITS", 0x0D53},
    {"BLUE_BITS", 0x0D54}, {"ALPHA_BITS", 0x0D55}, {"DEPTH_BITS", 0x0D56}, {"STENCIL_BITS", 0x0D57},
    {"POLYGON_OFFSET_UNITS", 0x2A00}, {"POLYGON_OFFSET_FACTOR", 0x8038}, {"TEXTURE_BINDING_2D", 0x8069},
    {"SAMPLE_BUFFERS", 0x80A8}, {"SAMPLES", 0x80A9}, {"SAMPLE_COVERAGE_VALUE", 0x80AA},
    {"SAMPLE_COVERAGE_INVERT", 0x80AB}, {"COMPRESSED_TEXTURE_FORMATS", 0x86A3},
    {"DONT_CARE", 0x1100}, {"FASTEST", 0x1101}, {"NICEST", 0x1102}, {"GENERATE_MIPMAP_HINT", 0x8192},
    {"BYTE", 0x1400}, {"UNSIGNED_BYTE", 0x1401}, {"SHORT", 0x1402}, {"UNSIGNED_SHORT", 0x1403},
    {"INT", 0x1404}, {"UNSIGNED_INT", 0x1405}, {"FLOAT", 0x1406},
    {"DEPTH_COMPONENT", 0x1902}, {"ALPHA", 0x1906}, {"RGB", 0x1907}, {"RGBA", 0x1908},
    {"LUMINANCE", 0x1909}, {"LUMINANCE_ALPHA", 0x190A}, {"UNSIGNED_SHORT_4_4_4_4", 0x8033},
    {"UNSIGNED_SHORT_5_5_5_1", 0x8034}, {"UNSIGNED_SHORT_5_6_5", 0x8363},
    {"FRAGMENT_SHADER", 0x8B30}, {"VERTEX_SHADER", 0x8B31}, {"MAX_VERTEX_ATTRIBS", 0x8869},
    {"MAX_VERTEX_UNIFORM_VECTORS", 0x8DFB}, {"MAX_VARYING_VECTORS", 0x8DFC},
    {"MAX_COMBINED_TEXTURE_IMAGE_UNITS", 0x8B4D}, {"MAX_VERTEX_TEXTURE_IMAGE_UNITS", 0x8B4C},
    {"MAX_TEXTURE_IMAGE_UNITS", 0x8872}, {"MAX_FRAGMENT_UNIFORM_VECTORS", 0x8DFD},
    {"SHADER_TYPE", 0x8B4F}, {"DELETE_STATUS", 0x8B80}, {"LINK_STATUS", 0x8B82}, {"VALIDATE_STATUS", 0x8B83},
    {"ATTACHED_SHADERS", 0x8B85}, {"ACTIVE_UNIFORMS", 0x8B86}, {"ACTIVE_ATTRIBUTES", 0x8B89},
    {"SHADING_LANGUAGE_VERSION", 0x8B8C}, {"CURRENT_PROGRAM", 0x8B8D},
    {"NEVER", 0x0200}, {"LESS", 0x0201}, {"EQUAL", 0x0202}, {"LEQUAL", 0x0203}, {"GREATER", 0x0204},
    {"NOTEQUAL", 0x0205}, {"GEQUAL", 0x0206}, {"ALWAYS", 0x0207},
    {"KEEP", 0x1E00}, {"REPLACE", 0x1E01}, {"INCR", 0x1E02}, {"DECR", 0x1E03}, {"INVERT", 0x150A},
    {"INCR_WRAP", 0x8507}, {"DECR_WRAP", 0x8508},
    {"VENDOR", 0x1F00}, {"RENDERER", 0x1F01}, {"VERSION", 0x1F02},
    {"NEAREST", 0x2600}, {"LINEAR", 0x2601}, {"NEAREST_MIPMAP_NEAREST", 0x2700},
    {"LINEAR_MIPMAP_NEAREST", 0x2701}, {"NEAREST_MIPMAP_LINEAR", 0x2702}, {"LINEAR_MIPMAP_LINEAR", 0x2703},
    {"TEXTURE_MAG_FILTER", 0x2800}, {"TEXTURE_MIN_FILTER", 0x2801}, {"TEXTURE_WRAP_S", 0x2802},
    {"TEXTURE_WRAP_T", 0x2803}, {"TEXTURE_2D", 0x0DE1}, {"TEXTURE", 0x1702}, {"TEXTURE_CUBE_MAP", 0x8513},
    {"TEXTURE_BINDING_CUBE_MAP", 0x8514}, {"TEXTURE_CUBE_MAP_POSITIVE_X", 0x8515},
    {"TEXTURE_CUBE_MAP_NEGATIVE_X", 0x8516}, {"TEXTURE_CUBE_MAP_POSITIVE_Y", 0x8517},
    {"TEXTURE_CUBE_MAP_NEGATIVE_Y", 0x8518}, {"TEXTURE_CUBE_MAP_POSITIVE_Z", 0x8519},
    {"TEXTURE_CUBE_MAP_NEGATIVE_Z", 0x851A}, {"MAX_CUBE_MAP_TEXTURE_SIZE", 0x851C},
    {"ACTIVE_TEXTURE", 0x84E0}, {"REPEAT", 0x2901}, {"CLAMP_TO_EDGE", 0x812F}, {"MIRRORED_REPEAT", 0x8370},
    {"FLOAT_VEC2", 0x8B50}, {"FLOAT_VEC3", 0x8B51}, {"FLOAT_VEC4", 0x8B52}, {"INT_VEC2", 0x8B53},
    {"INT_VEC3", 0x8B54}, {"INT_VEC4", 0x8B55}, {"BOOL", 0x8B56}, {"BOOL_VEC2", 0x8B57}, {"BOOL_VEC3", 0x8B58},
    {"BOOL_VEC4", 0x8B59}, {"FLOAT_MAT2", 0x8B5A}, {"FLOAT_MAT3", 0x8B5B}, {"FLOAT_MAT4", 0x8B5C},
    {"SAMPLER_2D", 0x8B5E}, {"SAMPLER_CUBE", 0x8B60},
    {"VERTEX_ATTRIB_ARRAY_ENABLED", 0x8622}, {"VERTEX_ATTRIB_ARRAY_SIZE", 0x8623},
    {"VERTEX_ATTRIB_ARRAY_STRIDE", 0x8624}, {"VERTEX_ATTRIB_ARRAY_TYPE", 0x8625},
    {"VERTEX_ATTRIB_ARRAY_NORMALIZED", 0x886A}, {"VERTEX_ATTRIB_ARRAY_POINTER", 0x8645},
    {"VERTEX_ATTRIB_ARRAY_BUFFER_BINDING", 0x889F},
    {"IMPLEMENTATION_COLOR_READ_TYPE", 0x8B9A}, {"IMPLEMENTATION_COLOR_READ_FORMAT", 0x8B9B},
    {"COMPILE_STATUS", 0x8B81}, {"LOW_FLOAT", 0x8DF0}, {"MEDIUM_FLOAT", 0x8DF1}, {"HIGH_FLOAT", 0x8DF2},
    {"LOW_INT", 0x8DF3}, {"MEDIUM_INT", 0x8DF4}, {"HIGH_INT", 0x8DF5},
    {"FRAMEBUFFER", 0x8D40}, {"RENDERBUFFER", 0x8D41}, {"RGBA4", 0x8056}, {"RGB5_A1", 0x8057},
    {"RGB565", 0x8D62}, {"DEPTH_COMPONENT16", 0x81A5}, {"STENCIL_INDEX8", 0x8D48}, {"DEPTH_STENCIL", 0x84F9},
    {"COLOR_ATTACHMENT0", 0x8CE0}, {"DEPTH_ATTACHMENT", 0x8D00}, {"STENCIL_ATTACHMENT", 0x8D20},
    {"DEPTH_STENCIL_ATTACHMENT", 0x821A}, {"NONE", 0}, {"FRAMEBUFFER_COMPLETE", 0x8CD5},
    {"FRAMEBUFFER_INCOMPLETE_ATTACHMENT", 0x8CD6}, {"FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT", 0x8CD7},
    {"FRAMEBUFFER_INCOMPLETE_DIMENSIONS", 0x8CD9}, {"FRAMEBUFFER_UNSUPPORTED", 0x8CDD},
    {"FRAMEBUFFER_BINDING", 0x8CA6}, {"RENDERBUFFER_BINDING", 0x8CA7}, {"MAX_RENDERBUFFER_SIZE", 0x84E8},
    {"INVALID_FRAMEBUFFER_OPERATION", 0x0506},
    {"UNPACK_FLIP_Y_WEBGL", 0x9240}, {"UNPACK_PREMULTIPLY_ALPHA_WEBGL", 0x9241}, {"CONTEXT_LOST_WEBGL", 0x9242},
    {"UNPACK_COLORSPACE_CONVERSION_WEBGL", 0x9243}, {"BROWSER_DEFAULT_WEBGL", 0x9244},
};
constexpr uint32_t kTexture0 = 0x84C0;

// ------------------------------------------------------------------
// JS objects
// ------------------------------------------------------------------

enum class HandleKind { Shader, Program, Buffer, Texture, Framebuffer, Renderbuffer };

struct ShaderInfo {
    uint32_t type = 0;
    std::string source;
    bool compiled = false;
    bool deleted = false;
};

struct ProgramInfo {
    uint32_t vertexShader = 0, fragmentShader = 0;
    std::map<std::string, uint32_t> boundAttribs;  // bindAttribLocation
    bool linked = false;
    bool deleted = false;
    std::map<std::string, int> attribLocations;    // after a link
    std::set<std::string> uniforms;
    uint32_t generation = 0;                        // bumps on every link
};

class WebGLContextObject : public Object {
public:
    NX::WebGLRenderingContext* gl = nullptr;
    // What the script can read back without the GPU
    std::unordered_map<uint32_t, ShaderInfo> shaders;
    std::unordered_map<uint32_t, ProgramInfo> programs;
    std::unordered_map<uint32_t, Object*> wrappers;  // handle -> its JS object (rooted on this object)
    uint32_t currentProgram = 0, arrayBuffer = 0, elementBuffer = 0, activeTexture = 0;
    std::map<uint32_t, uint32_t> textures2D;
    std::set<uint32_t> enabled{0x0BD0};  // DITHER
    float clearColor[4] = {0, 0, 0, 0};
    int viewport[4] = {0, 0, 0, 0};
    int scissor[4] = {0, 0, 0, 0};
    bool unpackFlipY = false;
    int unpackAlignment = 4;
    uint32_t error = 0;
    void setError(uint32_t e) { if (!error) error = e; }
};

class WebGLHandleObject : public Object {
public:
    WebGLHandleObject(HandleKind k, uint32_t h, WebGLContextObject* o) : kind(k), handle(h), owner(o) {}
    HandleKind kind;
    uint32_t handle;
    WebGLContextObject* owner;
};

class WebGLUniformLocationObject : public Object {
public:
    WebGLUniformLocationObject(uint32_t p, std::string n, uint32_t g)
        : program(p), name(std::move(n)), generation(g) {}
    uint32_t program;
    std::string name;
    uint32_t generation;
};

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------

void defineHidden(Object* obj, const std::string& name, Value value) {
    PropertyDescriptor desc;
    desc.value = value;
    desc.attributes = PropertyAttribute::Writable | PropertyAttribute::Configurable;
    obj->defineProperty(name, desc);
}

void defineConstant(Object* obj, const std::string& name, Value value) {
    PropertyDescriptor desc;
    desc.value = value;
    desc.attributes = PropertyAttribute::Enumerable;
    obj->defineProperty(name, desc);
}

Value str(const std::string& s) { return Value::string(new String(s)); }

template <typename T>
T* objectAs(const Value& v) {
    return v.isObject() ? dynamic_cast<T*>(v.asObject()) : nullptr;
}

WebGLContextObject* self(const FunctionCallInfo& info, const char* method) {
    auto* ctx = objectAs<WebGLContextObject>(info.thisValue());
    if (!ctx || !ctx->gl) {
        throw JSError("TypeError", std::string("Failed to execute '") + method +
                                   "' on 'WebGLRenderingContext': Illegal invocation");
    }
    return ctx;
}

uint32_t u32(const FunctionCallInfo& info, size_t i) {
    double d = info.argument(i).toNumber();
    if (!std::isfinite(d)) return 0;
    return static_cast<uint32_t>(static_cast<int64_t>(std::trunc(d)));
}

int32_t i32(const FunctionCallInfo& info, size_t i) { return static_cast<int32_t>(u32(info, i)); }
float f32(const FunctionCallInfo& info, size_t i) { return static_cast<float>(info.argument(i).toNumber()); }
bool flag(const FunctionCallInfo& info, size_t i) { return info.argument(i).toBoolean(); }

// The handle of a WebGL object argument of `kind`; 0 for null. A wrong kind or another
// context's object throws, as the IDL's type check does.
uint32_t handleOf(WebGLContextObject* ctx, const Value& v, HandleKind kind, const char* method) {
    if (v.isNull() || v.isUndefined()) return 0;
    auto* h = objectAs<WebGLHandleObject>(v);
    if (!h || h->kind != kind) {
        throw JSError("TypeError", std::string("Failed to execute '") + method +
                                   "' on 'WebGLRenderingContext': parameter is not of the expected type.");
    }
    if (h->owner != ctx) {
        ctx->setError(0x0502);  // INVALID_OPERATION
        return 0;
    }
    return h->handle;
}

// The bytes of an ArrayBuffer or ArrayBufferView
bool bytesOf(const Value& v, const uint8_t*& data, size_t& size) {
    if (auto* ta = objectAs<TypedArrayObject>(v)) {
        data = ta->data();
        size = ta->byteLength();
        return true;
    }
    if (auto* buffer = objectAs<Builtins::ArrayBufferObject>(v)) {
        data = buffer->data();
        size = buffer->byteLength();
        return true;
    }
    if (auto* view = objectAs<Builtins::DataViewObject>(v)) {
        data = view->buffer() ? view->buffer()->data() + view->byteOffset() : nullptr;
        size = view->buffer() ? view->byteLength() : 0;
        return data != nullptr;
    }
    return false;
}

// Float32Array or sequence<GLfloat> (Int32Array or sequence<GLint> for `ints`)
template <typename T>
std::vector<T> numbersOf(const Value& v, const char* method) {
    std::vector<T> out;
    if (auto* ta = objectAs<TypedArrayObject>(v)) {
        out.reserve(ta->length());
        for (uint32_t i = 0; i < ta->length(); ++i) out.push_back(static_cast<T>(ta->get(i).toNumber()));
        return out;
    }
    if (v.isObject() && v.asObject()->isArray()) {
        Object* a = v.asObject();
        out.reserve(a->length());
        for (uint32_t i = 0; i < a->length(); ++i) out.push_back(static_cast<T>(a->get(i).toNumber()));
        return out;
    }
    throw JSError("TypeError", std::string("Failed to execute '") + method +
                               "' on 'WebGLRenderingContext': The provided value cannot be converted to a sequence.");
}

template <typename T>
std::vector<uint8_t> rawBytes(const std::vector<T>& values) {
    std::vector<uint8_t> out(values.size() * sizeof(T));
    if (!out.empty()) std::memcpy(out.data(), values.data(), out.size());
    return out;
}

// Bytes per pixel of a texImage2D format/type pair; 0 when unsupported
size_t bytesPerPixel(uint32_t format, uint32_t type) {
    if (type == 0x8033 || type == 0x8034 || type == 0x8363) return 2;  // packed 16-bit
    size_t channels = format == 0x1908 ? 4 : format == 0x1907 ? 3 : format == 0x190A ? 2
                    : (format == 0x1909 || format == 0x1906) ? 1 : 0;
    size_t size = type == 0x1401 ? 1 : type == 0x1406 ? 4 : 0;
    return channels * size;
}

// Pixels for texImage2D: rows padded to the unpack alignment, flipped when the page asked
std::vector<uint8_t> texturePixels(WebGLContextObject* ctx, const uint8_t* data, size_t size,
                                   int width, int height, uint32_t format, uint32_t type) {
    std::vector<uint8_t> out(data, data + size);
    const size_t bpp = bytesPerPixel(format, type);
    if (!ctx->unpackFlipY || bpp == 0 || height < 2) return out;
    const size_t align = static_cast<size_t>(std::max(ctx->unpackAlignment, 1));
    const size_t row = (static_cast<size_t>(width) * bpp + align - 1) / align * align;
    if (row * static_cast<size_t>(height) > size) return out;
    for (int y = 0; y < height / 2; ++y) {
        std::swap_ranges(out.begin() + y * row, out.begin() + y * row + row,
                         out.begin() + (height - 1 - y) * row);
    }
    return out;
}

void record(WebGLContextObject* ctx, WebGLCommand command) {
    ctx->gl->record(std::move(command));
    if (ctx->gl->pendingBytes() > kMaxPendingBytes && NX::WebGLRenderingContext::glAvailable()) {
        ctx->gl->flush();
    }
}

WebGLCommand cmd(WebGLOp op, std::initializer_list<uint32_t> u = {}, std::initializer_list<float> f = {}) {
    WebGLCommand c;
    c.op = op;
    size_t i = 0;
    for (uint32_t v : u) c.u[i++] = v;
    i = 0;
    for (float v : f) c.f[i++] = v;
    return c;
}

} // namespace

// ==================================================================
// Prototype
// ==================================================================

void CanvasRegistry::installWebGL(Object* window) {
    auto* proto = new Object();
    webglPrototype_ = proto;
    root_->set("webglPrototype", Value::object(proto));

    // WebGLRenderingContext: not constructible, but present for feature tests
    // (`if (window.WebGLRenderingContext)`) and its constants
    auto* ctor = new Function("WebGLRenderingContext", [](const FunctionCallInfo&) -> Value {
        throw JSError("TypeError", "Illegal constructor");
    }, 0);
    defineHidden(ctor, "prototype", Value::object(proto));
    defineHidden(proto, "constructor", Value::object(ctor));
    for (const Constant& c : kConstants) {
        defineConstant(proto, c.name, Value::number(c.value));
        defineConstant(ctor, c.name, Value::number(c.value));
    }
    PropertyDescriptor tag;
    tag.value = str("WebGLRenderingContext");
    tag.attributes = PropertyAttribute::Configurable;
    proto->defineProperty(symbolPropertyKey(WellKnownSymbols::ToStringTag), tag);
    if (vm_) vm_->setGlobal("WebGLRenderingContext", Value::object(ctor));
    if (window) window->set("WebGLRenderingContext", Value::object(ctor));

    auto def = [proto](const char* name, size_t arity, BuiltinFn fn) {
        defineHidden(proto, name, Value::object(new Function(name, std::move(fn), arity)));
    };
    auto getter = [proto](const char* name, BuiltinFn fn) {
        PropertyDescriptor desc;
        desc.getter = Value::object(new Function(std::string("get ") + name, std::move(fn), 0));
        desc.attributes = PropertyAttribute::Configurable | PropertyAttribute::Enumerable;
        proto->defineProperty(name, desc);
    };

    // A new WebGL object of `kind`, created in GL by `op` at the next flush
    auto create = [](WebGLContextObject* ctx, HandleKind kind, WebGLOp op, uint32_t arg = 0) -> Value {
        const uint32_t handle = ctx->gl->newHandle();
        auto* obj = new WebGLHandleObject(kind, handle, ctx);
        ctx->wrappers[handle] = obj;
        defineHidden(ctx, "__h" + std::to_string(handle), Value::object(obj));
        record(ctx, cmd(op, {handle, arg}));
        return Value::object(obj);
    };

    // ---- Context ----
    getter("canvas", [](const FunctionCallInfo& info) -> Value {
        return self(info, "canvas")->get("__canvas");
    });
    getter("drawingBufferWidth", [](const FunctionCallInfo& info) -> Value {
        return Value::number(self(info, "drawingBufferWidth")->gl->drawingBufferWidth());
    });
    getter("drawingBufferHeight", [](const FunctionCallInfo& info) -> Value {
        return Value::number(self(info, "drawingBufferHeight")->gl->drawingBufferHeight());
    });
    def("getContextAttributes", 0, [](const FunctionCallInfo& info) -> Value {
        const NX::WebGLContextAttributes& a = self(info, "getContextAttributes")->gl->attributes();
        auto* out = new Object();
        out->set("alpha", Value::boolean(a.alpha));
        out->set("depth", Value::boolean(a.depth));
        out->set("stencil", Value::boolean(a.stencil));
        out->set("antialias", Value::boolean(a.antialias));
        out->set("premultipliedAlpha", Value::boolean(a.premultipliedAlpha));
        out->set("preserveDrawingBuffer", Value::boolean(a.preserveDrawingBuffer));
        out->set("failIfMajorPerformanceCaveat", Value::boolean(false));
        out->set("powerPreference", str("default"));
        return Value::object(out);
    });
    def("isContextLost", 0, [](const FunctionCallInfo&) -> Value { return Value::boolean(false); });
    def("getSupportedExtensions", 0, [](const FunctionCallInfo&) -> Value { return Value::object(new Array()); });
    def("getExtension", 1, [](const FunctionCallInfo&) -> Value { return Value::null(); });
    def("getError", 0, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "getError");
        uint32_t e = ctx->error;
        ctx->error = 0;
        return Value::number(e);
    });
    def("flush", 0, [](const FunctionCallInfo& info) -> Value { self(info, "flush"); return Value::undefined(); });
    def("finish", 0, [](const FunctionCallInfo& info) -> Value { self(info, "finish"); return Value::undefined(); });
    def("hint", 2, [](const FunctionCallInfo& info) -> Value { self(info, "hint"); return Value::undefined(); });

    def("getParameter", 1, [this](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "getParameter");
        auto ints = [this](std::initializer_list<int> v) {
            TypedArrayObject* a = newTypedArray(TypedArrayType::Int32, v.size());
            uint32_t i = 0;
            for (int x : v) a->set(i++, Value::number(x));
            return Value::object(a);
        };
        auto floats = [this](std::initializer_list<float> v) {
            TypedArrayObject* a = newTypedArray(TypedArrayType::Float32, v.size());
            uint32_t i = 0;
            for (float x : v) a->set(i++, Value::number(x));
            return Value::object(a);
        };
        auto wrapper = [ctx](uint32_t handle) {
            auto it = ctx->wrappers.find(handle);
            return it == ctx->wrappers.end() ? Value::null() : Value::object(it->second);
        };
        switch (u32(info, 0)) {
            case 0x1F00: return str("Zepra");                                  // VENDOR
            case 0x1F01: return str("NXRender WebGL");                         // RENDERER
            case 0x1F02: return str("WebGL 1.0 (OpenGL ES 2.0 on NXRender)");  // VERSION
            case 0x8B8C: return str("WebGL GLSL ES 1.0 (OpenGL ES GLSL ES 1.0 on NXRender)");
            case 0x0D33: case 0x84E8: case 0x851C: return Value::number(4096);  // texture/renderbuffer sizes
            case 0x0D3A: return ints({4096, 4096});
            case 0x8869: return Value::number(16);                              // MAX_VERTEX_ATTRIBS
            case 0x8B4D: case 0x8872: return Value::number(8);                  // texture units
            case 0x8B4C: return Value::number(4);
            case 0x8DFB: return Value::number(256);
            case 0x8DFC: return Value::number(8);
            case 0x8DFD: return Value::number(224);
            case 0x0D50: return Value::number(4);
            case 0x0D52: case 0x0D53: case 0x0D54: return Value::number(8);
            case 0x0D55: return Value::number(ctx->gl->attributes().alpha ? 8 : 0);
            case 0x0D56: return Value::number(ctx->gl->attributes().depth ? 24 : 0);
            case 0x0D57: return Value::number(ctx->gl->attributes().stencil ? 8 : 0);
            case 0x80A8: case 0x80A9: return Value::number(0);                  // SAMPLE_BUFFERS, SAMPLES
            case 0x846D: return floats({1, 64});
            case 0x846E: return floats({1, 1});
            case 0x0BA2: return ints({ctx->viewport[0], ctx->viewport[1], ctx->viewport[2], ctx->viewport[3]});
            case 0x0C10: return ints({ctx->scissor[0], ctx->scissor[1], ctx->scissor[2], ctx->scissor[3]});
            case 0x0C22: return floats({ctx->clearColor[0], ctx->clearColor[1], ctx->clearColor[2], ctx->clearColor[3]});
            case 0x0CF5: return Value::number(ctx->unpackAlignment);
            case 0x9240: return Value::boolean(ctx->unpackFlipY);
            case 0x84E0: return Value::number(kTexture0 + ctx->activeTexture);
            case 0x8B8D: return wrapper(ctx->currentProgram);
            case 0x8894: return wrapper(ctx->arrayBuffer);
            case 0x8895: return wrapper(ctx->elementBuffer);
            case 0x8069: return wrapper(ctx->textures2D[ctx->activeTexture]);
            case 0x8CA6: case 0x8CA7: return Value::null();                     // default framebuffer
            case 0x86A3: return Value::object(newTypedArray(TypedArrayType::Uint32, 0));
            case 0x0BE2: case 0x0B44: case 0x0B71: case 0x0BD0: case 0x8037: case 0x0C11: case 0x0B90:
                return Value::boolean(ctx->enabled.count(u32(info, 0)) > 0);
            default:
                ctx->setError(0x0500);  // INVALID_ENUM
                return Value::null();
        }
    });
    def("isEnabled", 1, [](const FunctionCallInfo& info) -> Value {
        return Value::boolean(self(info, "isEnabled")->enabled.count(u32(info, 0)) > 0);
    });

    // ---- Shaders ----
    def("createShader", 1, [create](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "createShader");
        uint32_t type = u32(info, 0);
        if (type != NX::GL::VERTEX_SHADER && type != NX::GL::FRAGMENT_SHADER) {
            ctx->setError(0x0500);
            return Value::null();
        }
        Value shader = create(ctx, HandleKind::Shader, WebGLOp::CreateShader, type);
        ctx->shaders[objectAs<WebGLHandleObject>(shader)->handle].type = type;
        return shader;
    });
    def("shaderSource", 2, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "shaderSource");
        uint32_t s = handleOf(ctx, info.argument(0), HandleKind::Shader, "shaderSource");
        if (!s) return Value::undefined();
        ctx->shaders[s].source = info.argument(1).toString();
        WebGLCommand c = cmd(WebGLOp::ShaderSource, {s});
        c.text = ctx->shaders[s].source;
        record(ctx, std::move(c));
        return Value::undefined();
    });
    def("getShaderSource", 1, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "getShaderSource");
        uint32_t s = handleOf(ctx, info.argument(0), HandleKind::Shader, "getShaderSource");
        return s ? str(ctx->shaders[s].source) : Value::null();
    });
    def("compileShader", 1, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "compileShader");
        uint32_t s = handleOf(ctx, info.argument(0), HandleKind::Shader, "compileShader");
        if (!s) return Value::undefined();
        ctx->shaders[s].compiled = !ctx->shaders[s].source.empty();
        record(ctx, cmd(WebGLOp::CompileShader, {s}));
        // With GL at hand the status and log are the driver's
        if (NX::WebGLRenderingContext::glAvailable()) ctx->gl->flush();
        return Value::undefined();
    });
    def("getShaderParameter", 2, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "getShaderParameter");
        uint32_t s = handleOf(ctx, info.argument(0), HandleKind::Shader, "getShaderParameter");
        if (!s) return Value::null();
        const ShaderInfo& shader = ctx->shaders[s];
        switch (u32(info, 1)) {
            case 0x8B81: {  // COMPILE_STATUS
                auto result = ctx->gl->shaderResult(s);
                return Value::boolean(result.known ? result.ok : shader.compiled);
            }
            case 0x8B4F: return Value::number(shader.type);
            case 0x8B80: return Value::boolean(shader.deleted);
            default: ctx->setError(0x0500); return Value::null();
        }
    });
    def("getShaderInfoLog", 1, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "getShaderInfoLog");
        uint32_t s = handleOf(ctx, info.argument(0), HandleKind::Shader, "getShaderInfoLog");
        return s ? str(ctx->gl->shaderResult(s).log) : Value::null();
    });
    def("getShaderPrecisionFormat", 2, [](const FunctionCallInfo& info) -> Value {
        self(info, "getShaderPrecisionFormat");
        const bool isFloat = u32(info, 1) <= 0x8DF2;
        auto* out = new Object();
        out->set("rangeMin", Value::number(isFloat ? 127 : 31));
        out->set("rangeMax", Value::number(isFloat ? 127 : 30));
        out->set("precision", Value::number(isFloat ? 23 : 0));
        return Value::object(out);
    });
    def("deleteShader", 1, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "deleteShader");
        uint32_t s = handleOf(ctx, info.argument(0), HandleKind::Shader, "deleteShader");
        if (!s) return Value::undefined();
        ctx->shaders[s].deleted = true;
        record(ctx, cmd(WebGLOp::DeleteShader, {s}));
        return Value::undefined();
    });

    // ---- Programs ----
    def("createProgram", 0, [create](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "createProgram");
        Value program = create(ctx, HandleKind::Program, WebGLOp::CreateProgram);
        ctx->programs[objectAs<WebGLHandleObject>(program)->handle];
        return program;
    });
    def("attachShader", 2, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "attachShader");
        uint32_t p = handleOf(ctx, info.argument(0), HandleKind::Program, "attachShader");
        uint32_t s = handleOf(ctx, info.argument(1), HandleKind::Shader, "attachShader");
        if (!p || !s) return Value::undefined();
        ProgramInfo& program = ctx->programs[p];
        uint32_t& slot = ctx->shaders[s].type == NX::GL::VERTEX_SHADER ? program.vertexShader : program.fragmentShader;
        if (slot) { ctx->setError(0x0502); return Value::undefined(); }
        slot = s;
        record(ctx, cmd(WebGLOp::AttachShader, {p, s}));
        return Value::undefined();
    });
    def("detachShader", 2, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "detachShader");
        uint32_t p = handleOf(ctx, info.argument(0), HandleKind::Program, "detachShader");
        uint32_t s = handleOf(ctx, info.argument(1), HandleKind::Shader, "detachShader");
        if (!p || !s) return Value::undefined();
        ProgramInfo& program = ctx->programs[p];
        if (program.vertexShader == s) program.vertexShader = 0;
        if (program.fragmentShader == s) program.fragmentShader = 0;
        record(ctx, cmd(WebGLOp::DetachShader, {p, s}));
        return Value::undefined();
    });
    def("bindAttribLocation", 3, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "bindAttribLocation");
        uint32_t p = handleOf(ctx, info.argument(0), HandleKind::Program, "bindAttribLocation");
        uint32_t index = u32(info, 1);
        if (!p) return Value::undefined();
        if (index >= 16) { ctx->setError(0x0501); return Value::undefined(); }
        ctx->programs[p].boundAttribs[info.argument(2).toString()] = index;
        return Value::undefined();
    });
    def("linkProgram", 1, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "linkProgram");
        uint32_t p = handleOf(ctx, info.argument(0), HandleKind::Program, "linkProgram");
        if (!p) return Value::undefined();
        ProgramInfo& program = ctx->programs[p];
        const ShaderInfo* vs = program.vertexShader ? &ctx->shaders[program.vertexShader] : nullptr;
        const ShaderInfo* fs = program.fragmentShader ? &ctx->shaders[program.fragmentShader] : nullptr;
        program.linked = vs && fs && vs->compiled && fs->compiled;
        program.generation++;
        program.attribLocations.clear();
        program.uniforms.clear();
        if (vs) {
            // Attributes get the locations the page bound, the rest the lowest free ones in
            // declaration order; they are bound in GL before the link so both sides agree
            std::set<uint32_t> used;
            for (const auto& [name, index] : program.boundAttribs) used.insert(index);
            uint32_t next = 0;
            for (const std::string& name : NX::WebGLRenderingContext::declaredNames(vs->source, "attribute")) {
                auto bound = program.boundAttribs.find(name);
                uint32_t index;
                if (bound != program.boundAttribs.end()) {
                    index = bound->second;
                } else {
                    while (used.count(next)) ++next;
                    index = next;
                    used.insert(index);
                }
                program.attribLocations[name] = static_cast<int>(index);
            }
            for (const std::string& name : NX::WebGLRenderingContext::declaredNames(vs->source, "uniform")) program.uniforms.insert(name);
        }
        if (fs) {
            for (const std::string& name : NX::WebGLRenderingContext::declaredNames(fs->source, "uniform")) program.uniforms.insert(name);
        }
        for (const auto& [name, index] : program.attribLocations) {
            WebGLCommand c = cmd(WebGLOp::BindAttribLocation, {p, static_cast<uint32_t>(index)});
            c.text = name;
            record(ctx, std::move(c));
        }
        record(ctx, cmd(WebGLOp::LinkProgram, {p}));
        if (NX::WebGLRenderingContext::glAvailable()) {
            ctx->gl->flush();
            auto result = ctx->gl->programResult(p);
            if (result.known) program.linked = result.ok;
        }
        return Value::undefined();
    });
    def("getProgramParameter", 2, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "getProgramParameter");
        uint32_t p = handleOf(ctx, info.argument(0), HandleKind::Program, "getProgramParameter");
        if (!p) return Value::null();
        const ProgramInfo& program = ctx->programs[p];
        switch (u32(info, 1)) {
            case 0x8B82: return Value::boolean(program.linked);
            case 0x8B83: return Value::boolean(program.linked);  // VALIDATE_STATUS
            case 0x8B80: return Value::boolean(program.deleted);
            case 0x8B85: return Value::number((program.vertexShader ? 1 : 0) + (program.fragmentShader ? 1 : 0));
            case 0x8B89: return Value::number(static_cast<double>(program.attribLocations.size()));
            case 0x8B86: return Value::number(static_cast<double>(program.uniforms.size()));
            default: ctx->setError(0x0500); return Value::null();
        }
    });
    def("getProgramInfoLog", 1, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "getProgramInfoLog");
        uint32_t p = handleOf(ctx, info.argument(0), HandleKind::Program, "getProgramInfoLog");
        return p ? str(ctx->gl->programResult(p).log) : Value::null();
    });
    def("validateProgram", 1, [](const FunctionCallInfo& info) -> Value { self(info, "validateProgram"); return Value::undefined(); });
    def("useProgram", 1, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "useProgram");
        uint32_t p = handleOf(ctx, info.argument(0), HandleKind::Program, "useProgram");
        if (p && !ctx->programs[p].linked) { ctx->setError(0x0502); return Value::undefined(); }
        ctx->currentProgram = p;
        record(ctx, cmd(WebGLOp::UseProgram, {p}));
        return Value::undefined();
    });
    def("deleteProgram", 1, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "deleteProgram");
        uint32_t p = handleOf(ctx, info.argument(0), HandleKind::Program, "deleteProgram");
        if (!p) return Value::undefined();
        ctx->programs[p].deleted = true;
        if (ctx->currentProgram == p) ctx->currentProgram = 0;
        record(ctx, cmd(WebGLOp::DeleteProgram, {p}));
        return Value::undefined();
    });
    def("getAttribLocation", 2, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "getAttribLocation");
        uint32_t p = handleOf(ctx, info.argument(0), HandleKind::Program, "getAttribLocation");
        if (!p || !ctx->programs[p].linked) { ctx->setError(0x0502); return Value::number(-1); }
        const auto& locations = ctx->programs[p].attribLocations;
        auto it = locations.find(info.argument(1).toString());
        return Value::number(it == locations.end() ? -1 : it->second);
    });
    def("getUniformLocation", 2, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "getUniformLocation");
        uint32_t p = handleOf(ctx, info.argument(0), HandleKind::Program, "getUniformLocation");
        if (!p || !ctx->programs[p].linked) { ctx->setError(0x0502); return Value::null(); }
        const std::string name = info.argument(1).toString();
        // "u", "u[2]" and "u.field" all name the declared uniform u
        const std::string base = name.substr(0, name.find_first_of("[."));
        if (!ctx->programs[p].uniforms.count(base)) return Value::null();
        return Value::object(new WebGLUniformLocationObject(p, name, ctx->programs[p].generation));
    });

    // ---- Uniforms ----
    // uniform{1,2,3,4}{f,i}, uniform{1,2,3,4}{f,i}v and uniformMatrix{2,3,4}fv
    auto uniform = [](WebGLContextObject* ctx, const Value& location, NX::UniformKind kind,
                      std::vector<uint8_t> data, uint32_t count, bool transpose) {
        auto* loc = objectAs<WebGLUniformLocationObject>(location);
        if (!loc) return;  // null location: ignored, as the spec says
        auto program = ctx->programs.find(loc->program);
        if (loc->program != ctx->currentProgram || program == ctx->programs.end() ||
            program->second.generation != loc->generation) {
            ctx->setError(0x0502);
            return;
        }
        WebGLCommand c = cmd(WebGLOp::Uniform, {loc->program, static_cast<uint32_t>(kind), count, transpose ? 1u : 0u});
        c.text = loc->name;
        c.data = std::move(data);
        record(ctx, std::move(c));
    };
    static const char* const kFloatNames[] = {"uniform1f", "uniform2f", "uniform3f", "uniform4f"};
    static const char* const kIntNames[] = {"uniform1i", "uniform2i", "uniform3i", "uniform4i"};
    static const char* const kFloatVNames[] = {"uniform1fv", "uniform2fv", "uniform3fv", "uniform4fv"};
    static const char* const kIntVNames[] = {"uniform1iv", "uniform2iv", "uniform3iv", "uniform4iv"};
    for (uint32_t n = 1; n <= 4; ++n) {
        const auto fkind = static_cast<NX::UniformKind>(static_cast<int>(NX::UniformKind::F1) + n - 1);
        const auto ikind = static_cast<NX::UniformKind>(static_cast<int>(NX::UniformKind::I1) + n - 1);
        const char* fname = kFloatNames[n - 1];
        const char* iname = kIntNames[n - 1];
        const char* fvname = kFloatVNames[n - 1];
        const char* ivname = kIntVNames[n - 1];
        def(fname, n + 1, [uniform, fkind, n, fname](const FunctionCallInfo& info) -> Value {
            WebGLContextObject* ctx = self(info, fname);
            std::vector<float> v;
            for (uint32_t i = 0; i < n; ++i) v.push_back(f32(info, i + 1));
            uniform(ctx, info.argument(0), fkind, rawBytes(v), 1, false);
            return Value::undefined();
        });
        def(iname, n + 1, [uniform, ikind, n, iname](const FunctionCallInfo& info) -> Value {
            WebGLContextObject* ctx = self(info, iname);
            std::vector<int32_t> v;
            for (uint32_t i = 0; i < n; ++i) v.push_back(i32(info, i + 1));
            uniform(ctx, info.argument(0), ikind, rawBytes(v), 1, false);
            return Value::undefined();
        });
        def(fvname, 2, [uniform, fkind, n, fvname](const FunctionCallInfo& info) -> Value {
            WebGLContextObject* ctx = self(info, fvname);
            std::vector<float> v = numbersOf<float>(info.argument(1), fvname);
            if (v.empty() || v.size() % n) { ctx->setError(0x0501); return Value::undefined(); }
            const uint32_t count = static_cast<uint32_t>(v.size() / n);
            uniform(ctx, info.argument(0), fkind, rawBytes(v), count, false);
            return Value::undefined();
        });
        def(ivname, 2, [uniform, ikind, n, ivname](const FunctionCallInfo& info) -> Value {
            WebGLContextObject* ctx = self(info, ivname);
            std::vector<int32_t> v = numbersOf<int32_t>(info.argument(1), ivname);
            if (v.empty() || v.size() % n) { ctx->setError(0x0501); return Value::undefined(); }
            const uint32_t count = static_cast<uint32_t>(v.size() / n);
            uniform(ctx, info.argument(0), ikind, rawBytes(v), count, false);
            return Value::undefined();
        });
    }
    static const char* const kMatrixNames[] = {"uniformMatrix2fv", "uniformMatrix3fv", "uniformMatrix4fv"};
    for (uint32_t n = 2; n <= 4; ++n) {
        const auto kind = static_cast<NX::UniformKind>(static_cast<int>(NX::UniformKind::Mat2) + n - 2);
        const char* name = kMatrixNames[n - 2];
        def(name, 3, [uniform, kind, n, name](const FunctionCallInfo& info) -> Value {
            WebGLContextObject* ctx = self(info, name);
            // WebGL 1 requires transpose to be false
            if (flag(info, 1)) { ctx->setError(0x0501); return Value::undefined(); }
            std::vector<float> v = numbersOf<float>(info.argument(2), name);
            if (v.empty() || v.size() % (n * n)) { ctx->setError(0x0501); return Value::undefined(); }
            const uint32_t count = static_cast<uint32_t>(v.size() / (n * n));
            uniform(ctx, info.argument(0), kind, rawBytes(v), count, false);
            return Value::undefined();
        });
    }

    // ---- Vertex attributes ----
    def("enableVertexAttribArray", 1, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "enableVertexAttribArray");
        uint32_t index = u32(info, 0);
        if (index >= 16) { ctx->setError(0x0501); return Value::undefined(); }
        record(ctx, cmd(WebGLOp::EnableVertexAttribArray, {index}));
        return Value::undefined();
    });
    def("disableVertexAttribArray", 1, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "disableVertexAttribArray");
        uint32_t index = u32(info, 0);
        if (index >= 16) { ctx->setError(0x0501); return Value::undefined(); }
        record(ctx, cmd(WebGLOp::DisableVertexAttribArray, {index}));
        return Value::undefined();
    });
    def("vertexAttribPointer", 6, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "vertexAttribPointer");
        uint32_t index = u32(info, 0), size = u32(info, 1);
        if (index >= 16 || size < 1 || size > 4 || i32(info, 4) < 0 || i32(info, 4) > 255 || i32(info, 5) < 0) {
            ctx->setError(0x0501);
            return Value::undefined();
        }
        if (!ctx->arrayBuffer) { ctx->setError(0x0502); return Value::undefined(); }
        record(ctx, cmd(WebGLOp::VertexAttribPointer, {index, size, u32(info, 2), flag(info, 3) ? 1u : 0u,
                                                        u32(info, 4), u32(info, 5)}));
        return Value::undefined();
    });
    static const char* const kAttribNames[] = {"vertexAttrib1f", "vertexAttrib2f", "vertexAttrib3f", "vertexAttrib4f"};
    static const char* const kAttribVNames[] = {"vertexAttrib1fv", "vertexAttrib2fv", "vertexAttrib3fv", "vertexAttrib4fv"};
    for (uint32_t n = 1; n <= 4; ++n) {
        const char* name = kAttribNames[n - 1];
        const char* vname = kAttribVNames[n - 1];
        def(name, n + 1, [n, name](const FunctionCallInfo& info) -> Value {
            WebGLContextObject* ctx = self(info, name);
            float v[4] = {0, 0, 0, 1};
            for (uint32_t i = 0; i < n; ++i) v[i] = f32(info, i + 1);
            if (u32(info, 0) >= 16) { ctx->setError(0x0501); return Value::undefined(); }
            record(ctx, cmd(WebGLOp::VertexAttrib4f, {u32(info, 0)}, {v[0], v[1], v[2], v[3]}));
            return Value::undefined();
        });
        def(vname, 2, [n, vname](const FunctionCallInfo& info) -> Value {
            WebGLContextObject* ctx = self(info, vname);
            std::vector<float> values = numbersOf<float>(info.argument(1), vname);
            if (values.size() < n || u32(info, 0) >= 16) { ctx->setError(0x0501); return Value::undefined(); }
            float v[4] = {0, 0, 0, 1};
            for (uint32_t i = 0; i < n; ++i) v[i] = values[i];
            record(ctx, cmd(WebGLOp::VertexAttrib4f, {u32(info, 0)}, {v[0], v[1], v[2], v[3]}));
            return Value::undefined();
        });
    }

    // ---- Buffers ----
    def("createBuffer", 0, [create](const FunctionCallInfo& info) -> Value {
        return create(self(info, "createBuffer"), HandleKind::Buffer, WebGLOp::CreateBuffer);
    });
    def("bindBuffer", 2, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "bindBuffer");
        uint32_t target = u32(info, 0);
        uint32_t b = handleOf(ctx, info.argument(1), HandleKind::Buffer, "bindBuffer");
        if (target != NX::GL::ARRAY_BUFFER && target != NX::GL::ELEMENT_ARRAY_BUFFER) {
            ctx->setError(0x0500);
            return Value::undefined();
        }
        (target == NX::GL::ARRAY_BUFFER ? ctx->arrayBuffer : ctx->elementBuffer) = b;
        record(ctx, cmd(WebGLOp::BindBuffer, {target, b}));
        return Value::undefined();
    });
    def("bufferData", 3, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "bufferData");
        uint32_t target = u32(info, 0);
        uint32_t bound = target == NX::GL::ARRAY_BUFFER ? ctx->arrayBuffer
                       : target == NX::GL::ELEMENT_ARRAY_BUFFER ? ctx->elementBuffer : 0;
        if (!bound) { ctx->setError(target == NX::GL::ARRAY_BUFFER || target == NX::GL::ELEMENT_ARRAY_BUFFER ? 0x0502 : 0x0500); return Value::undefined(); }
        WebGLCommand c = cmd(WebGLOp::BufferData, {target, u32(info, 2), 0});
        const Value source = info.argument(1);
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (source.isNumber()) {
            double n = source.toNumber();
            if (!(n >= 0) || n > double(1u << 30)) { ctx->setError(0x0501); return Value::undefined(); }
            c.u[2] = static_cast<uint32_t>(n);
        } else if (bytesOf(source, data, size)) {
            c.data.assign(data, data + size);
            c.u[2] = static_cast<uint32_t>(size);
        } else {
            throw JSError("TypeError", "Failed to execute 'bufferData' on 'WebGLRenderingContext': parameter 2 is not of type 'ArrayBuffer'.");
        }
        record(ctx, std::move(c));
        return Value::undefined();
    });
    def("bufferSubData", 3, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "bufferSubData");
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!bytesOf(info.argument(2), data, size)) {
            throw JSError("TypeError", "Failed to execute 'bufferSubData' on 'WebGLRenderingContext': parameter 3 is not of type 'ArrayBuffer'.");
        }
        WebGLCommand c = cmd(WebGLOp::BufferSubData, {u32(info, 0), u32(info, 1)});
        c.data.assign(data, data + size);
        record(ctx, std::move(c));
        return Value::undefined();
    });
    def("deleteBuffer", 1, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "deleteBuffer");
        uint32_t b = handleOf(ctx, info.argument(0), HandleKind::Buffer, "deleteBuffer");
        if (!b) return Value::undefined();
        if (ctx->arrayBuffer == b) ctx->arrayBuffer = 0;
        if (ctx->elementBuffer == b) ctx->elementBuffer = 0;
        record(ctx, cmd(WebGLOp::DeleteBuffer, {b}));
        return Value::undefined();
    });
    def("isBuffer", 1, [](const FunctionCallInfo& info) -> Value {
        auto* h = objectAs<WebGLHandleObject>(info.argument(0));
        return Value::boolean(h && h->kind == HandleKind::Buffer && h->owner == self(info, "isBuffer"));
    });

    // ---- Textures ----
    def("createTexture", 0, [create](const FunctionCallInfo& info) -> Value {
        return create(self(info, "createTexture"), HandleKind::Texture, WebGLOp::CreateTexture);
    });
    def("activeTexture", 1, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "activeTexture");
        uint32_t unit = u32(info, 0);
        if (unit < kTexture0 || unit >= kTexture0 + 8) { ctx->setError(0x0500); return Value::undefined(); }
        ctx->activeTexture = unit - kTexture0;
        record(ctx, cmd(WebGLOp::ActiveTexture, {unit}));
        return Value::undefined();
    });
    def("bindTexture", 2, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "bindTexture");
        uint32_t target = u32(info, 0);
        uint32_t t = handleOf(ctx, info.argument(1), HandleKind::Texture, "bindTexture");
        if (target == NX::GL::TEXTURE_2D) ctx->textures2D[ctx->activeTexture] = t;
        record(ctx, cmd(WebGLOp::BindTexture, {target, t}));
        return Value::undefined();
    });
    def("texParameteri", 3, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "texParameteri");
        record(ctx, cmd(WebGLOp::TexParameteri, {u32(info, 0), u32(info, 1), u32(info, 2)}));
        return Value::undefined();
    });
    def("texParameterf", 3, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "texParameterf");
        record(ctx, cmd(WebGLOp::TexParameteri, {u32(info, 0), u32(info, 1), u32(info, 2)}));
        return Value::undefined();
    });
    def("texImage2D", 9, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "texImage2D");
        // Only the (…, width, height, border, format, type, ArrayBufferView?) form: images,
        // canvases and video are not texture sources yet
        if (info.argumentCount() < 9) return Value::undefined();
        const int width = i32(info, 3), height = i32(info, 4);
        if (width < 0 || height < 0 || i32(info, 5) != 0) { ctx->setError(0x0501); return Value::undefined(); }
        WebGLCommand c = cmd(WebGLOp::TexImage2D, {u32(info, 0), u32(info, 1), u32(info, 2), u32(info, 3),
                                                    u32(info, 4), u32(info, 6), u32(info, 7)});
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (bytesOf(info.argument(8), data, size)) {
            c.data = texturePixels(ctx, data, size, width, height, u32(info, 6), u32(info, 7));
        }
        record(ctx, std::move(c));
        return Value::undefined();
    });
    def("texSubImage2D", 9, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "texSubImage2D");
        if (info.argumentCount() < 9) return Value::undefined();
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!bytesOf(info.argument(8), data, size)) return Value::undefined();
        WebGLCommand c = cmd(WebGLOp::TexSubImage2D, {u32(info, 0), u32(info, 1), u32(info, 2), u32(info, 3),
                                                       u32(info, 4), u32(info, 5), u32(info, 6), u32(info, 7)});
        c.data = texturePixels(ctx, data, size, i32(info, 4), i32(info, 5), u32(info, 6), u32(info, 7));
        record(ctx, std::move(c));
        return Value::undefined();
    });
    def("generateMipmap", 1, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "generateMipmap");
        record(ctx, cmd(WebGLOp::GenerateMipmap, {u32(info, 0)}));
        return Value::undefined();
    });
    def("deleteTexture", 1, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "deleteTexture");
        uint32_t t = handleOf(ctx, info.argument(0), HandleKind::Texture, "deleteTexture");
        if (!t) return Value::undefined();
        for (auto& [unit, handle] : ctx->textures2D) {
            if (handle == t) handle = 0;
        }
        record(ctx, cmd(WebGLOp::DeleteTexture, {t}));
        return Value::undefined();
    });
    def("pixelStorei", 2, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "pixelStorei");
        const uint32_t pname = u32(info, 0);
        if (pname == NX::GL::UNPACK_FLIP_Y_WEBGL) ctx->unpackFlipY = info.argument(1).toBoolean();
        if (pname == NX::GL::UNPACK_ALIGNMENT) {
            const int v = i32(info, 1);
            if (v != 1 && v != 2 && v != 4 && v != 8) { ctx->setError(0x0501); return Value::undefined(); }
            ctx->unpackAlignment = v;
            record(ctx, cmd(WebGLOp::PixelStorei, {pname, static_cast<uint32_t>(v)}));
        }
        return Value::undefined();
    });

    // ---- Framebuffers: only the default one (render-to-texture is not supported) ----
    def("bindFramebuffer", 2, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "bindFramebuffer");
        if (!info.argument(1).isNull() && !info.argument(1).isUndefined()) ctx->setError(0x0502);
        return Value::undefined();
    });
    def("bindRenderbuffer", 2, [](const FunctionCallInfo& info) -> Value { self(info, "bindRenderbuffer"); return Value::undefined(); });
    def("createFramebuffer", 0, [](const FunctionCallInfo& info) -> Value { self(info, "createFramebuffer"); return Value::null(); });
    def("createRenderbuffer", 0, [](const FunctionCallInfo& info) -> Value { self(info, "createRenderbuffer"); return Value::null(); });
    def("checkFramebufferStatus", 1, [](const FunctionCallInfo& info) -> Value {
        self(info, "checkFramebufferStatus");
        return Value::number(0x8CD5);  // FRAMEBUFFER_COMPLETE (the default framebuffer)
    });

    // ---- Fixed-function state ----
    def("viewport", 4, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "viewport");
        if (i32(info, 2) < 0 || i32(info, 3) < 0) { ctx->setError(0x0501); return Value::undefined(); }
        for (int i = 0; i < 4; ++i) ctx->viewport[i] = i32(info, i);
        record(ctx, cmd(WebGLOp::Viewport, {u32(info, 0), u32(info, 1), u32(info, 2), u32(info, 3)}));
        return Value::undefined();
    });
    def("scissor", 4, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "scissor");
        if (i32(info, 2) < 0 || i32(info, 3) < 0) { ctx->setError(0x0501); return Value::undefined(); }
        for (int i = 0; i < 4; ++i) ctx->scissor[i] = i32(info, i);
        record(ctx, cmd(WebGLOp::Scissor, {u32(info, 0), u32(info, 1), u32(info, 2), u32(info, 3)}));
        return Value::undefined();
    });
    def("clearColor", 4, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "clearColor");
        for (int i = 0; i < 4; ++i) ctx->clearColor[i] = std::clamp(f32(info, i), 0.0f, 1.0f);
        record(ctx, cmd(WebGLOp::ClearColor, {}, {ctx->clearColor[0], ctx->clearColor[1], ctx->clearColor[2], ctx->clearColor[3]}));
        return Value::undefined();
    });
    def("clearDepth", 1, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "clearDepth"), cmd(WebGLOp::ClearDepth, {}, {std::clamp(f32(info, 0), 0.0f, 1.0f)}));
        return Value::undefined();
    });
    def("clearStencil", 1, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "clearStencil"), cmd(WebGLOp::ClearStencil, {u32(info, 0)}));
        return Value::undefined();
    });
    def("clear", 1, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "clear");
        uint32_t mask = u32(info, 0);
        if (mask & ~(NX::GL::COLOR_BUFFER_BIT | NX::GL::DEPTH_BUFFER_BIT | NX::GL::STENCIL_BUFFER_BIT)) {
            ctx->setError(0x0501);
            return Value::undefined();
        }
        record(ctx, cmd(WebGLOp::Clear, {mask}));
        return Value::undefined();
    });
    static const uint32_t kCaps[] = {0x0BE2, 0x0B44, 0x0B71, 0x0BD0, 0x8037, 0x809E, 0x80A0, 0x0C11, 0x0B90};
    def("enable", 1, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "enable");
        uint32_t cap = u32(info, 0);
        if (std::find(std::begin(kCaps), std::end(kCaps), cap) == std::end(kCaps)) { ctx->setError(0x0500); return Value::undefined(); }
        ctx->enabled.insert(cap);
        record(ctx, cmd(WebGLOp::Enable, {cap}));
        return Value::undefined();
    });
    def("disable", 1, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "disable");
        uint32_t cap = u32(info, 0);
        if (std::find(std::begin(kCaps), std::end(kCaps), cap) == std::end(kCaps)) { ctx->setError(0x0500); return Value::undefined(); }
        ctx->enabled.erase(cap);
        record(ctx, cmd(WebGLOp::Disable, {cap}));
        return Value::undefined();
    });
    def("blendFunc", 2, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "blendFunc"), cmd(WebGLOp::BlendFuncSeparate, {u32(info, 0), u32(info, 1), u32(info, 0), u32(info, 1)}));
        return Value::undefined();
    });
    def("blendFuncSeparate", 4, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "blendFuncSeparate"), cmd(WebGLOp::BlendFuncSeparate, {u32(info, 0), u32(info, 1), u32(info, 2), u32(info, 3)}));
        return Value::undefined();
    });
    def("blendEquation", 1, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "blendEquation"), cmd(WebGLOp::BlendEquationSeparate, {u32(info, 0), u32(info, 0)}));
        return Value::undefined();
    });
    def("blendEquationSeparate", 2, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "blendEquationSeparate"), cmd(WebGLOp::BlendEquationSeparate, {u32(info, 0), u32(info, 1)}));
        return Value::undefined();
    });
    def("blendColor", 4, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "blendColor"), cmd(WebGLOp::BlendColor, {}, {f32(info, 0), f32(info, 1), f32(info, 2), f32(info, 3)}));
        return Value::undefined();
    });
    def("depthFunc", 1, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "depthFunc"), cmd(WebGLOp::DepthFunc, {u32(info, 0)}));
        return Value::undefined();
    });
    def("depthMask", 1, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "depthMask"), cmd(WebGLOp::DepthMask, {flag(info, 0) ? 1u : 0u}));
        return Value::undefined();
    });
    def("depthRange", 2, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "depthRange"), cmd(WebGLOp::DepthRange, {}, {std::clamp(f32(info, 0), 0.0f, 1.0f), std::clamp(f32(info, 1), 0.0f, 1.0f)}));
        return Value::undefined();
    });
    def("colorMask", 4, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "colorMask"), cmd(WebGLOp::ColorMask, {flag(info, 0) ? 1u : 0u, flag(info, 1) ? 1u : 0u,
                                                                  flag(info, 2) ? 1u : 0u, flag(info, 3) ? 1u : 0u}));
        return Value::undefined();
    });
    def("stencilFunc", 3, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "stencilFunc"), cmd(WebGLOp::StencilFuncSeparate, {0x0408, u32(info, 0), u32(info, 1), u32(info, 2)}));
        return Value::undefined();
    });
    def("stencilFuncSeparate", 4, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "stencilFuncSeparate"), cmd(WebGLOp::StencilFuncSeparate, {u32(info, 0), u32(info, 1), u32(info, 2), u32(info, 3)}));
        return Value::undefined();
    });
    def("stencilOp", 3, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "stencilOp"), cmd(WebGLOp::StencilOpSeparate, {0x0408, u32(info, 0), u32(info, 1), u32(info, 2)}));
        return Value::undefined();
    });
    def("stencilOpSeparate", 4, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "stencilOpSeparate"), cmd(WebGLOp::StencilOpSeparate, {u32(info, 0), u32(info, 1), u32(info, 2), u32(info, 3)}));
        return Value::undefined();
    });
    def("stencilMask", 1, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "stencilMask"), cmd(WebGLOp::StencilMaskSeparate, {0x0408, u32(info, 0)}));
        return Value::undefined();
    });
    def("stencilMaskSeparate", 2, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "stencilMaskSeparate"), cmd(WebGLOp::StencilMaskSeparate, {u32(info, 0), u32(info, 1)}));
        return Value::undefined();
    });
    def("cullFace", 1, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "cullFace"), cmd(WebGLOp::CullFace, {u32(info, 0)}));
        return Value::undefined();
    });
    def("frontFace", 1, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "frontFace"), cmd(WebGLOp::FrontFace, {u32(info, 0)}));
        return Value::undefined();
    });
    def("lineWidth", 1, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "lineWidth"), cmd(WebGLOp::LineWidth, {}, {f32(info, 0)}));
        return Value::undefined();
    });
    def("polygonOffset", 2, [](const FunctionCallInfo& info) -> Value {
        record(self(info, "polygonOffset"), cmd(WebGLOp::PolygonOffset, {}, {f32(info, 0), f32(info, 1)}));
        return Value::undefined();
    });

    // ---- Drawing ----
    def("drawArrays", 3, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "drawArrays");
        if (i32(info, 1) < 0 || i32(info, 2) < 0) { ctx->setError(0x0501); return Value::undefined(); }
        if (!ctx->currentProgram) { ctx->setError(0x0502); return Value::undefined(); }
        record(ctx, cmd(WebGLOp::DrawArrays, {u32(info, 0), u32(info, 1), u32(info, 2)}));
        return Value::undefined();
    });
    def("drawElements", 4, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "drawElements");
        if (i32(info, 1) < 0 || i32(info, 3) < 0) { ctx->setError(0x0501); return Value::undefined(); }
        if (!ctx->currentProgram || !ctx->elementBuffer) { ctx->setError(0x0502); return Value::undefined(); }
        record(ctx, cmd(WebGLOp::DrawElements, {u32(info, 0), u32(info, 1), u32(info, 2), u32(info, 3)}));
        return Value::undefined();
    });
    def("readPixels", 7, [](const FunctionCallInfo& info) -> Value {
        WebGLContextObject* ctx = self(info, "readPixels");
        auto* out = objectAs<TypedArrayObject>(info.argument(6));
        if (!out) {
            throw JSError("TypeError", "Failed to execute 'readPixels' on 'WebGLRenderingContext': parameter 7 is not of type 'ArrayBufferView'.");
        }
        const int w = i32(info, 2), h = i32(info, 3);
        if (w < 0 || h < 0) { ctx->setError(0x0501); return Value::undefined(); }
        if (u32(info, 4) != NX::GL::RGBA || u32(info, 5) != NX::GL::UNSIGNED_BYTE) { ctx->setError(0x0500); return Value::undefined(); }
        const size_t need = static_cast<size_t>(w) * h * 4;
        if (out->byteLength() < need) { ctx->setError(0x0502); return Value::undefined(); }
        // Without GL (headless) the pixels read back as transparent black
        if (!ctx->gl->readPixels(i32(info, 0), i32(info, 1), w, h, out->data())) std::memset(out->data(), 0, need);
        return Value::undefined();
    });
}

// ==================================================================
// A context for one canvas
// ==================================================================

Object* CanvasRegistry::createWebGLContext(CanvasSurface& surface, Object* canvasWrapper, const Value& attributes) {
    NX::WebGLContextAttributes attrs;
    if (attributes.isObject()) {
        Object* a = attributes.asObject();
        auto read = [a](const char* name, bool& out) {
            Value v = a->get(name);
            if (!v.isUndefined()) out = v.toBoolean();
        };
        read("alpha", attrs.alpha);
        read("depth", attrs.depth);
        read("stencil", attrs.stencil);
        read("antialias", attrs.antialias);
        read("premultipliedAlpha", attrs.premultipliedAlpha);
        read("preserveDrawingBuffer", attrs.preserveDrawingBuffer);
    }
    const DOMElement* canvas = elementsById_[surface.id];
    const int width = attributeSize(canvas, "width"), height = attributeSize(canvas, "height");
    surface.webgl = std::make_unique<NX::WebGLRenderingContext>(width, height, attrs);

    auto* obj = new WebGLContextObject();
    obj->gl = surface.webgl.get();
    obj->setPrototype(webglPrototype_);
    obj->viewport[2] = obj->scissor[2] = width;
    obj->viewport[3] = obj->scissor[3] = height;
    defineHidden(obj, "__canvas", Value::object(canvasWrapper));
    return obj;
}

} // namespace Zepra::WebCore
