// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file webgl_bindings_test.cpp
 * @brief getContext("webgl"): the script-facing API and the commands it
 *        records for NXRender (no GL context here, so nothing is replayed)
 */

#include <gtest/gtest.h>
#include "scripting/script_context.hpp"
#include "scripting/canvas_bindings.hpp"
#include "html/html_parser.hpp"
#include "nxgfx/webgl/webgl_context.h"

#include <cstring>

using namespace Zepra::WebCore;
using NXRender::WebGLOp;

namespace {

const char* kTriangle = R"JS(
var canvas = document.getElementById('c');
var gl = canvas.getContext('webgl');
var vs = gl.createShader(gl.VERTEX_SHADER);
gl.shaderSource(vs, 'attribute vec2 a_position;\n attribute vec3 a_color; // per vertex\n varying vec3 v_color;\n' +
                    'void main() { v_color = a_color; gl_Position = vec4(a_position, 0.0, 1.0); }');
gl.compileShader(vs);
var fs = gl.createShader(gl.FRAGMENT_SHADER);
gl.shaderSource(fs, 'precision mediump float;\n varying vec3 v_color;\n uniform float u_alpha;\n' +
                    'void main() { gl_FragColor = vec4(v_color, u_alpha); }');
gl.compileShader(fs);
var program = gl.createProgram();
gl.attachShader(program, vs);
gl.attachShader(program, fs);
gl.linkProgram(program);
var buffer = gl.createBuffer();
gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
var vertices = new Float32Array([0, 0.5, 1, 0, 0,  -0.5, -0.5, 0, 1, 0,  0.5, -0.5, 0, 0, 1]);
gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.STATIC_DRAW);
gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);
gl.clearColor(0.1, 0.1, 0.1, 1);
gl.clear(gl.COLOR_BUFFER_BIT);
gl.useProgram(program);
var aPos = gl.getAttribLocation(program, 'a_position');
var aColor = gl.getAttribLocation(program, 'a_color');
gl.enableVertexAttribArray(aPos);
gl.vertexAttribPointer(aPos, 2, gl.FLOAT, false, 20, 0);
gl.enableVertexAttribArray(aColor);
gl.vertexAttribPointer(aColor, 3, gl.FLOAT, false, 20, 8);
gl.uniform1f(gl.getUniformLocation(program, 'u_alpha'), 1);
gl.drawArrays(gl.TRIANGLES, 0, 3);
)JS";

} // namespace

class WebGLBindingsTest : public ::testing::Test {
protected:
    void SetUp() override {
        HTMLParser parser;
        doc_ = parser.parse("<html><body><canvas id=\"c\" width=\"64\" height=\"32\"></canvas>"
                            "<canvas id=\"d\"></canvas></body></html>");
        ASSERT_TRUE(doc_);
        ctx_ = std::make_unique<ScriptContext>();
        ctx_->initialize(doc_.get());
    }

    std::string eval(const std::string& code) {
        auto r = ctx_->evaluate(code, "<test>");
        EXPECT_TRUE(r.success) << code << "\n -> " << r.error;
        return r.value;
    }

    NXRender::WebGLRenderingContext* native(const char* id) {
        CanvasSurface* s = ctx_->canvases()->find(doc_->getElementById(id));
        return s ? s->webgl.get() : nullptr;
    }

    std::vector<const NXRender::WebGLCommand*> commands(WebGLOp op) {
        std::vector<const NXRender::WebGLCommand*> out;
        for (const auto& c : native("c")->pending()) {
            if (c.op == op) out.push_back(&c);
        }
        return out;
    }

    std::unique_ptr<DOMDocument> doc_;
    std::unique_ptr<ScriptContext> ctx_;
};

TEST_F(WebGLBindingsTest, GetContextCreatesOneWebGLContext) {
    EXPECT_EQ(eval("var c = document.getElementById('c'); var gl = c.getContext('webgl');"
                   "gl === c.getContext('experimental-webgl') && gl instanceof WebGLRenderingContext"), "true");
    EXPECT_EQ(eval("document.getElementById('c').getContext('2d')"), "null");
    EXPECT_EQ(eval("gl.canvas.width === 64 && gl.drawingBufferWidth === 64 && gl.drawingBufferHeight === 32"), "true");
    EXPECT_EQ(eval("gl.TRIANGLES === 4 && WebGLRenderingContext.COLOR_BUFFER_BIT === 16384 && gl.FLOAT === 5126"), "true");
    EXPECT_EQ(eval("Object.prototype.toString.call(gl)"), "[object WebGLRenderingContext]");
    EXPECT_EQ(eval("document.getElementById('d').getContext('2d') !== null && document.getElementById('d').getContext('webgl')"), "null");
    EXPECT_EQ(eval("document.getElementById('c').getContext('webgl2')"), "null");
    ASSERT_NE(native("c"), nullptr);
    EXPECT_EQ(native("c")->drawingBufferWidth(), 64);
    EXPECT_EQ(native("d"), nullptr);
}

TEST_F(WebGLBindingsTest, TrianglePageRecordsItsDrawing) {
    eval(kTriangle);
    EXPECT_EQ(eval("gl.getShaderParameter(vs, gl.COMPILE_STATUS) && gl.getProgramParameter(program, gl.LINK_STATUS)"), "true");
    // Attributes get locations in declaration order, and they are bound before the link
    EXPECT_EQ(eval("aPos + ',' + aColor + ',' + gl.getAttribLocation(program, 'nope')"), "0,1,-1");
    auto binds = commands(WebGLOp::BindAttribLocation);
    ASSERT_EQ(binds.size(), 2u);
    EXPECT_EQ(binds[0]->text + "=" + std::to_string(binds[0]->u[1]), "a_color=1");
    EXPECT_EQ(binds[1]->text + "=" + std::to_string(binds[1]->u[1]), "a_position=0");
    EXPECT_EQ(eval("gl.getUniformLocation(program, 'u_alpha') !== null && gl.getUniformLocation(program, 'u_missing')"), "null");
    EXPECT_EQ(eval("gl.getError()"), "0");

    auto data = commands(WebGLOp::BufferData);
    ASSERT_EQ(data.size(), 1u);
    ASSERT_EQ(data[0]->data.size(), 15u * 4);
    float first[2];
    std::memcpy(first, data[0]->data.data(), sizeof first);
    EXPECT_EQ(first[0], 0.0f);
    EXPECT_EQ(first[1], 0.5f);

    auto pointers = commands(WebGLOp::VertexAttribPointer);
    ASSERT_EQ(pointers.size(), 2u);
    EXPECT_EQ(pointers[1]->u[1], 3u);   // size
    EXPECT_EQ(pointers[1]->u[5], 8u);   // offset
    auto uniforms = commands(WebGLOp::Uniform);
    ASSERT_EQ(uniforms.size(), 1u);
    EXPECT_EQ(uniforms[0]->text, "u_alpha");
    auto draws = commands(WebGLOp::DrawArrays);
    ASSERT_EQ(draws.size(), 1u);
    EXPECT_EQ(draws[0]->u[2], 3u);
    auto clears = commands(WebGLOp::ClearColor);
    ASSERT_EQ(clears.size(), 1u);
    EXPECT_FLOAT_EQ(clears[0]->f[0], 0.1f);
}

TEST_F(WebGLBindingsTest, ErrorsAreReportedThroughGetError) {
    eval("var gl = document.getElementById('c').getContext('webgl');");
    EXPECT_EQ(eval("gl.drawArrays(gl.TRIANGLES, 0, 3); gl.getError() === gl.INVALID_OPERATION && gl.getError() === gl.NO_ERROR"), "true");
    EXPECT_EQ(eval("gl.enable(12345); gl.getError() === gl.INVALID_ENUM"), "true");
    EXPECT_EQ(eval("gl.bufferData(gl.ARRAY_BUFFER, 4, gl.STATIC_DRAW); gl.getError() === gl.INVALID_OPERATION"), "true");
    EXPECT_EQ(eval("var t = false; try { gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer()); gl.bufferData(gl.ARRAY_BUFFER, [1, 2], gl.STATIC_DRAW); } catch (e) { t = e instanceof TypeError; } t"), "true");
    EXPECT_EQ(eval("var t2 = false; try { gl.attachShader({}, null); } catch (e) { t2 = e instanceof TypeError; } t2"), "true");
    EXPECT_TRUE(commands(WebGLOp::DrawArrays).empty());
}

TEST_F(WebGLBindingsTest, ParametersAndReadPixels) {
    eval("var gl = document.getElementById('c').getContext('webgl'); gl.clearColor(1, 0.5, 2, 0); gl.enable(gl.BLEND);");
    EXPECT_EQ(eval("var v = gl.getParameter(gl.VIEWPORT); v instanceof Int32Array && v.join()"), "0,0,64,32");
    EXPECT_EQ(eval("Array.from(gl.getParameter(gl.COLOR_CLEAR_VALUE)).join()"), "1,0.5,1,0");
    EXPECT_EQ(eval("gl.isEnabled(gl.BLEND) && !gl.isEnabled(gl.DEPTH_TEST) && gl.getParameter(gl.MAX_VERTEX_ATTRIBS) === 16"), "true");
    EXPECT_EQ(eval("typeof gl.getParameter(gl.VERSION) === 'string' && gl.getContextAttributes().depth"), "true");
    // Without a GL context the pixels read back as transparent black
    EXPECT_EQ(eval("var px = new Uint8Array(4 * 4); px[0] = 9; gl.readPixels(0, 0, 2, 2, gl.RGBA, gl.UNSIGNED_BYTE, px); px[0]"), "0");
}

TEST_F(WebGLBindingsTest, ResizingTheCanvasResizesTheDrawingBuffer) {
    eval("var c = document.getElementById('c'); var gl = c.getContext('webgl'); c.width = 100;");
    EXPECT_EQ(eval("gl.drawingBufferWidth"), "100");
    EXPECT_EQ(native("c")->drawingBufferWidth(), 100);
}

TEST(WebGLShaderTranslation, GLSLESBecomesGLSL120) {
    const std::string out = NXRender::WebGLRenderingContext::translateShader(
        "#version 100\n#extension GL_OES_standard_derivatives : enable\n"
        "precision mediump float; // default\nuniform highp vec4 c;\n"
        "void main() { gl_FragColor = c; }\n");
    EXPECT_EQ(out.rfind("#version 120\n", 0), 0u) << out;
    EXPECT_EQ(out.find("#version 100"), std::string::npos) << out;
    EXPECT_EQ(out.find("precision mediump float"), std::string::npos) << out;
    EXPECT_EQ(out.find("#extension"), std::string::npos) << out;
    EXPECT_NE(out.find("#define highp"), std::string::npos);
    EXPECT_NE(out.find("uniform highp vec4 c;"), std::string::npos);
}

TEST(WebGLShaderTranslation, DeclaredNames) {
    const std::string vs = "attribute vec2 a_pos, a_uv;\n/* attribute vec4 a_old; */\n"
                           "attribute highp vec3 a_normal;\nuniform mat4 u_mvp;\nuniform vec4 u_lights[4];\n"
                           "void main() { gl_Position = u_mvp * vec4(a_pos, 0.0, 1.0); }";
    auto attributes = NXRender::WebGLRenderingContext::declaredNames(vs, "attribute");
    ASSERT_EQ(attributes.size(), 3u);
    EXPECT_EQ(attributes[0], "a_pos");
    EXPECT_EQ(attributes[1], "a_uv");
    EXPECT_EQ(attributes[2], "a_normal");
    auto uniforms = NXRender::WebGLRenderingContext::declaredNames(vs, "uniform");
    ASSERT_EQ(uniforms.size(), 2u);
    EXPECT_EQ(uniforms[1], "u_lights");
}

TEST(CanvasImageData, DataIsAUint8ClampedArray) {
    HTMLParser parser;
    auto doc = parser.parse("<html><body><canvas id=\"c\" width=\"4\" height=\"4\"></canvas></body></html>");
    ScriptContext ctx;
    ctx.initialize(doc.get());
    auto r = ctx.evaluate(
        "var g = document.getElementById('c').getContext('2d');"
        "var img = g.createImageData(2, 2); img.data[0] = 300; img.data[3] = 255;"
        "g.putImageData(img, 0, 0);"
        "var back = g.getImageData(0, 0, 1, 1).data;"
        "(img.data instanceof Uint8ClampedArray) + ',' + img.data.length + ',' + back[0] + ',' + back[3]", "<test>");
    EXPECT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.value, "true,16,255,255");
}
