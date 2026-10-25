// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file canvas_bindings_test.cpp
 * @brief <canvas> for page scripts: getContext("2d") draws into the
 *        element's NXRender bitmap
 */

#include <gtest/gtest.h>
#include "scripting/script_context.hpp"
#include "scripting/canvas_bindings.hpp"
#include "html/html_parser.hpp"
#include "nxgfx/canvas2d/canvas_context.h"

using namespace Zepra::WebCore;

class CanvasBindingsTest : public ::testing::Test {
protected:
    void SetUp() override {
        HTMLParser parser;
        doc_ = parser.parse("<html><body><canvas id=\"c\" width=\"40\" height=\"20\"></canvas>"
                            "<canvas id=\"d\"></canvas><div id=\"x\"></div></body></html>");
        ASSERT_TRUE(doc_);
        ctx_ = std::make_unique<ScriptContext>();
        ctx_->initialize(doc_.get());
        ctx_->setConsoleHandler([this](const std::string& level, const std::string& msg) {
            logs_.push_back(level + ": " + msg);
        });
    }

    std::string eval(const std::string& code) {
        auto r = ctx_->evaluate(code, "<test>");
        EXPECT_TRUE(r.success) << code << "\n -> " << r.error;
        return r.value;
    }

    NXRender::CanvasRenderingContext2D* native(const char* id) {
        CanvasSurface* s = ctx_->canvases()->find(doc_->getElementById(id));
        return s ? s->context2d.get() : nullptr;
    }

    // Unpremultiplied pixel of canvas #c
    std::array<int, 4> pixel(int x, int y) {
        NXRender::ImageData d = native("c")->getImageData(x, y, 1, 1);
        return {d.data[0], d.data[1], d.data[2], d.data[3]};
    }

    std::unique_ptr<DOMDocument> doc_;
    std::unique_ptr<ScriptContext> ctx_;
    std::vector<std::string> logs_;
};

TEST_F(CanvasBindingsTest, SizeFromAttributesAndDefaults) {
    EXPECT_EQ(eval("document.getElementById('c').width"), "40");
    EXPECT_EQ(eval("document.getElementById('c').height"), "20");
    EXPECT_EQ(eval("document.getElementById('d').width"), "300");
    EXPECT_EQ(eval("document.getElementById('d').height"), "150");
}

TEST_F(CanvasBindingsTest, GetContextReturnsTheSameObject) {
    EXPECT_EQ(eval("var c = document.getElementById('c');"
                   "c.getContext('2d') === document.getElementById('c').getContext('2d')"), "true");
    EXPECT_EQ(eval("document.getElementById('c').getContext('bogus')"), "null");
    EXPECT_EQ(eval("typeof document.getElementById('x').getContext"), "undefined");
    EXPECT_EQ(eval("document.getElementById('c').getContext('2d').canvas.width"), "40");
    ASSERT_NE(native("c"), nullptr);
    EXPECT_EQ(native("c")->width(), 40);
    EXPECT_EQ(native("d"), nullptr);
}

TEST_F(CanvasBindingsTest, FillRectDrawsIntoTheBitmap) {
    eval("var g = document.getElementById('c').getContext('2d');"
         "g.fillStyle = '#ff0000'; g.fillRect(0, 0, 10, 10);"
         "g.fillStyle = 'rgba(0, 0, 255, 0.5)'; g.fillRect(20, 0, 10, 10);");
    auto red = pixel(5, 5), blue = pixel(25, 5), empty = pixel(15, 5);
    EXPECT_EQ(red[0], 255);
    EXPECT_EQ(red[3], 255);
    EXPECT_EQ(blue[2], 255);
    EXPECT_NEAR(blue[3], 128, 1);
    EXPECT_EQ(empty[3], 0);
}

TEST_F(CanvasBindingsTest, StyleGettersSerialize) {
    eval("var g = document.getElementById('c').getContext('2d');");
    EXPECT_EQ(eval("g.fillStyle"), "#000000");
    EXPECT_EQ(eval("g.fillStyle = 'red'; g.fillStyle"), "#ff0000");
    EXPECT_EQ(eval("g.fillStyle = 'rgba(0, 128, 0, 0.5)'; g.fillStyle"), "rgba(0, 128, 0, 0.5)");
    EXPECT_EQ(eval("g.fillStyle = 'hsl(120 100% 25% / 50%)'; g.fillStyle"), "rgba(0, 128, 0, 0.5)");
    EXPECT_EQ(eval("g.fillStyle = 'not a colour'; g.fillStyle"), "rgba(0, 128, 0, 0.5)");
    EXPECT_EQ(eval("g.lineWidth = 3; g.lineWidth"), "3");
    EXPECT_EQ(eval("g.lineWidth = -1; g.lineWidth"), "3");
    EXPECT_EQ(eval("g.lineCap = 'round'; g.lineCap"), "round");
    EXPECT_EQ(eval("g.lineCap = 'bogus'; g.lineCap"), "round");
    EXPECT_EQ(eval("g.globalCompositeOperation = 'multiply'; g.globalCompositeOperation"), "multiply");
    EXPECT_EQ(eval("g.font = 'bold 12px serif'; g.font"), "bold 12px serif");
    EXPECT_EQ(eval("g.textAlign = 'center'; g.textAlign"), "center");
}

TEST_F(CanvasBindingsTest, SaveRestoreStylesAndTransform) {
    eval("var g = document.getElementById('c').getContext('2d');"
         "g.fillStyle = 'lime'; g.save(); g.fillStyle = 'red'; g.translate(20, 0); g.restore();"
         "g.fillRect(0, 0, 4, 4);");
    EXPECT_EQ(eval("g.fillStyle"), "#00ff00");
    auto p = pixel(1, 1);
    EXPECT_EQ(p[1], 255);
    EXPECT_EQ(pixel(21, 1)[3], 0);
}

TEST_F(CanvasBindingsTest, PathsArcsAndStroke) {
    eval("var g = document.getElementById('c').getContext('2d');"
         "g.beginPath(); g.arc(10, 10, 8, 0, Math.PI * 2); g.fill();"
         "g.strokeStyle = 'blue'; g.lineWidth = 2;"
         "g.beginPath(); g.moveTo(25, 2); g.lineTo(35, 2); g.lineTo(35, 15); g.stroke();");
    EXPECT_EQ(pixel(10, 10)[3], 255);
    EXPECT_EQ(pixel(0, 0)[3], 0);
    EXPECT_EQ(pixel(30, 2)[2], 255);
    EXPECT_EQ(pixel(35, 10)[2], 255);
    EXPECT_EQ(pixel(30, 10)[3], 0);
}

TEST_F(CanvasBindingsTest, GradientsStayLive) {
    eval("var g = document.getElementById('c').getContext('2d');"
         "var grad = g.createLinearGradient(0, 0, 40, 0);"
         "g.fillStyle = grad;"
         "grad.addColorStop(0, 'black'); grad.addColorStop(1, 'white');"
         "g.fillRect(0, 0, 40, 20);");
    EXPECT_EQ(eval("g.fillStyle === grad"), "true");
    EXPECT_LT(pixel(1, 5)[0], 20);
    EXPECT_NEAR(pixel(20, 5)[0], 131, 6);
    EXPECT_GT(pixel(39, 5)[0], 240);
    // A bad colour stop throws
    auto r = ctx_->evaluate("grad.addColorStop(0.5, 'nope')", "<test>");
    EXPECT_FALSE(r.success);
}

TEST_F(CanvasBindingsTest, ResizingClearsTheBitmap) {
    eval("var c = document.getElementById('c'); var g = c.getContext('2d');"
         "g.fillRect(0, 0, 40, 20); c.width = 50;");
    EXPECT_EQ(native("c")->width(), 50);
    EXPECT_EQ(eval("c.width"), "50");
    EXPECT_EQ(pixel(1, 1)[3], 0);
    eval("c.setAttribute('height', '30')");
    EXPECT_EQ(native("c")->height(), 30);
}

TEST_F(CanvasBindingsTest, ImageDataRoundTrip) {
    eval("var g = document.getElementById('c').getContext('2d');"
         "g.fillStyle = 'rgb(10, 20, 30)'; g.fillRect(0, 0, 2, 2);"
         "var d = g.getImageData(0, 0, 2, 2);");
    EXPECT_EQ(eval("d.width + 'x' + d.height + ':' + d.data.length"), "2x2:16");
    EXPECT_EQ(eval("d.data[0] + ',' + d.data[1] + ',' + d.data[2] + ',' + d.data[3]"), "10,20,30,255");
    eval("var e = g.createImageData(1, 1); e.data[0] = 255; e.data[3] = 255; g.putImageData(e, 5, 5);");
    EXPECT_EQ(pixel(5, 5)[0], 255);
}

TEST_F(CanvasBindingsTest, MeasureTextAndPath2D) {
    eval("var g = document.getElementById('c').getContext('2d');");
    EXPECT_EQ(eval("g.measureText('hello').width > 0"), "true");
    eval("var p = new Path2D(); p.rect(0, 0, 5, 5); g.fill(p);");
    EXPECT_EQ(pixel(2, 2)[3], 255);
    eval("var q = new Path2D('M 10 0 L 20 0 L 20 10 Z'); g.fill(q);");
    EXPECT_EQ(pixel(18, 2)[3], 255);
    EXPECT_EQ(pixel(11, 8)[3], 0);
}

TEST_F(CanvasBindingsTest, MissingArgumentsThrow) {
    eval("var g = document.getElementById('c').getContext('2d');");
    EXPECT_EQ(eval("var msg = ''; try { g.fillRect(1, 2); } catch (e) { msg = 'threw'; } msg"), "threw");
}

TEST_F(CanvasBindingsTest, DrawImageFromAnotherCanvas) {
    eval("var src = document.getElementById('d').getContext('2d');"
         "src.fillStyle = 'blue'; src.fillRect(0, 0, 300, 150);"
         "var g = document.getElementById('c').getContext('2d');"
         "g.drawImage(document.getElementById('d'), 0, 0, 30, 15);");
    EXPECT_EQ(pixel(10, 10)[2], 255);
    EXPECT_EQ(pixel(35, 10)[3], 0);
}

TEST_F(CanvasBindingsTest, LineDashFromAnArray) {
    eval("var g = document.getElementById('c').getContext('2d');");
    EXPECT_EQ(eval("g.setLineDash([8, 4]); g.getLineDash().length"), "2");
    EXPECT_EQ(eval("g.getLineDash()[0]"), "8");
    eval("g.lineWidth = 2; g.beginPath(); g.moveTo(0, 10); g.lineTo(40, 10); g.stroke();");
    EXPECT_EQ(pixel(4, 10)[3], 255);
    EXPECT_EQ(pixel(10, 10)[3], 0);
}
