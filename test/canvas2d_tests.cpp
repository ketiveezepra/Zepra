// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file canvas2d_tests.cpp
 * @brief Pixel tests for NXRender's Canvas 2D context (CPU rasterizer, no GL)
 */

#include <gtest/gtest.h>
#include "nxgfx/canvas2d/canvas_context.h"
#include <cmath>

using namespace NXRender;

namespace {

constexpr float kPi = 3.14159265358979323846f;

struct Px { int r, g, b, a; };

// Unpremultiplied pixel, as getImageData returns it
Px at(CanvasRenderingContext2D& ctx, int x, int y) {
    ImageData d = ctx.getImageData(x, y, 1, 1);
    return {d.data[0], d.data[1], d.data[2], d.data[3]};
}

int alphaAt(CanvasRenderingContext2D& ctx, int x, int y) { return at(ctx, x, y).a; }

// Sum of alpha over the canvas / 255 = covered area in pixels
double coveredArea(CanvasRenderingContext2D& ctx) {
    double sum = 0;
    const uint8_t* p = ctx.pixels();
    for (int i = 0; i < ctx.width() * ctx.height(); i++) sum += p[i * 4 + 3] / 255.0;
    return sum;
}

} // namespace

TEST(Canvas2D, NewCanvasIsTransparent) {
    CanvasRenderingContext2D ctx(30, 20);
    EXPECT_EQ(ctx.width(), 30);
    EXPECT_EQ(ctx.height(), 20);
    EXPECT_EQ(coveredArea(ctx), 0.0);
}

TEST(Canvas2D, FillRectCoversExactPixels) {
    CanvasRenderingContext2D ctx(20, 20);
    ctx.setFillStyle(Color(255, 0, 0));
    ctx.fillRect(2, 3, 5, 4);
    Px inside = at(ctx, 2, 3);
    EXPECT_EQ(inside.r, 255);
    EXPECT_EQ(inside.g, 0);
    EXPECT_EQ(inside.a, 255);
    EXPECT_EQ(alphaAt(ctx, 6, 6), 255);
    EXPECT_EQ(alphaAt(ctx, 7, 6), 0);
    EXPECT_EQ(alphaAt(ctx, 6, 7), 0);
    EXPECT_EQ(alphaAt(ctx, 1, 3), 0);
    EXPECT_NEAR(coveredArea(ctx), 20.0, 0.01);
}

TEST(Canvas2D, HalfPixelEdgesAreAntialiased) {
    CanvasRenderingContext2D ctx(10, 10);
    ctx.fillRect(1.5f, 1, 2, 2);
    EXPECT_NEAR(alphaAt(ctx, 1, 1), 128, 2);
    EXPECT_EQ(alphaAt(ctx, 2, 1), 255);
    EXPECT_NEAR(alphaAt(ctx, 3, 1), 128, 2);
}

TEST(Canvas2D, NegativeSizeRectFillsTheOtherWay) {
    CanvasRenderingContext2D ctx(10, 10);
    ctx.fillRect(5, 5, -3, -2);
    EXPECT_EQ(alphaAt(ctx, 2, 3), 255);
    EXPECT_EQ(alphaAt(ctx, 4, 4), 255);
    EXPECT_EQ(alphaAt(ctx, 5, 5), 0);
}

TEST(Canvas2D, ClearRect) {
    CanvasRenderingContext2D ctx(10, 10);
    ctx.fillRect(0, 0, 10, 10);
    ctx.clearRect(2, 2, 3, 3);
    EXPECT_EQ(alphaAt(ctx, 3, 3), 0);
    EXPECT_EQ(alphaAt(ctx, 5, 5), 255);
    EXPECT_NEAR(coveredArea(ctx), 91.0, 0.01);
}

TEST(Canvas2D, GlobalAlphaAndSourceOver) {
    CanvasRenderingContext2D ctx(4, 4);
    ctx.setFillStyle(Color(0, 0, 255));
    ctx.fillRect(0, 0, 4, 4);
    ctx.setGlobalAlpha(0.5f);
    ctx.setFillStyle(Color(255, 0, 0));
    ctx.fillRect(0, 0, 4, 4);
    Px p = at(ctx, 1, 1);
    EXPECT_NEAR(p.r, 128, 2);
    EXPECT_NEAR(p.b, 127, 2);
    EXPECT_EQ(p.a, 255);
    // Out-of-range alpha is ignored
    ctx.setGlobalAlpha(2.0f);
    EXPECT_FLOAT_EQ(ctx.globalAlpha(), 0.5f);
}

TEST(Canvas2D, TranslateAndScale) {
    CanvasRenderingContext2D ctx(40, 40);
    ctx.translate(10, 10);
    ctx.scale(2, 3);
    ctx.fillRect(0, 0, 5, 5);  // device (10,10)-(20,25)
    EXPECT_EQ(alphaAt(ctx, 10, 10), 255);
    EXPECT_EQ(alphaAt(ctx, 19, 24), 255);
    EXPECT_EQ(alphaAt(ctx, 20, 24), 0);
    EXPECT_EQ(alphaAt(ctx, 19, 25), 0);
    EXPECT_NEAR(coveredArea(ctx), 150.0, 0.05);
}

TEST(Canvas2D, RotatedSquareKeepsItsArea) {
    CanvasRenderingContext2D ctx(60, 60);
    ctx.translate(30, 30);
    ctx.rotate(kPi / 4);
    ctx.fillRect(-10, -10, 20, 20);
    EXPECT_NEAR(coveredArea(ctx), 400.0, 1.0);
    EXPECT_EQ(alphaAt(ctx, 30, 30), 255);
    // The corners now point up/down/left/right: (30, 16) is inside, (17, 17) outside
    EXPECT_EQ(alphaAt(ctx, 30, 17), 255);
    EXPECT_EQ(alphaAt(ctx, 17, 17), 0);
}

TEST(Canvas2D, SaveRestoreKeepsStyleAndTransform) {
    CanvasRenderingContext2D ctx(20, 20);
    ctx.setFillStyle(Color(0, 255, 0));
    ctx.save();
    ctx.setFillStyle(Color(255, 0, 0));
    ctx.translate(10, 0);
    ctx.restore();
    ctx.fillRect(0, 0, 2, 2);
    Px p = at(ctx, 0, 0);
    EXPECT_EQ(p.g, 255);
    EXPECT_EQ(p.r, 0);
}

TEST(Canvas2D, ConcavePathFillsCorrectly) {
    // An L shape: a triangle fan from the first point would cover the notch
    CanvasRenderingContext2D ctx(20, 20);
    ctx.beginPath();
    ctx.moveTo(0, 0);
    ctx.lineTo(10, 0);
    ctx.lineTo(10, 4);
    ctx.lineTo(4, 4);
    ctx.lineTo(4, 10);
    ctx.lineTo(0, 10);
    ctx.closePath();
    ctx.fill();
    EXPECT_EQ(alphaAt(ctx, 8, 2), 255);
    EXPECT_EQ(alphaAt(ctx, 2, 8), 255);
    EXPECT_EQ(alphaAt(ctx, 8, 8), 0);
    EXPECT_NEAR(coveredArea(ctx), 64.0, 0.05);
}

TEST(Canvas2D, NonZeroAndEvenOdd) {
    auto twoSquares = [](CanvasRenderingContext2D& ctx) {
        ctx.beginPath();
        ctx.rect(0, 0, 10, 10);
        ctx.rect(2, 2, 6, 6);  // same direction
    };
    CanvasRenderingContext2D nz(10, 10);
    twoSquares(nz);
    nz.fill(FillRule::NonZero);
    EXPECT_EQ(alphaAt(nz, 5, 5), 255);

    CanvasRenderingContext2D eo(10, 10);
    twoSquares(eo);
    eo.fill(FillRule::EvenOdd);
    EXPECT_EQ(alphaAt(eo, 5, 5), 0);
    EXPECT_EQ(alphaAt(eo, 1, 1), 255);
    EXPECT_NEAR(coveredArea(eo), 64.0, 0.05);
}

TEST(Canvas2D, ArcAreaMatchesCircle) {
    CanvasRenderingContext2D ctx(100, 100);
    ctx.beginPath();
    ctx.arc(50, 50, 30, 0, 2 * kPi);
    ctx.fill();
    EXPECT_NEAR(coveredArea(ctx), kPi * 900, 6.0);
    EXPECT_EQ(alphaAt(ctx, 50, 50), 255);
    EXPECT_EQ(alphaAt(ctx, 50, 18), 0);
    EXPECT_EQ(alphaAt(ctx, 50, 21), 255);
}

TEST(Canvas2D, HalfArcCounterClockwise) {
    // From 0 to pi counter-clockwise is the top half (y up is negative)
    CanvasRenderingContext2D ctx(100, 100);
    ctx.beginPath();
    ctx.arc(50, 50, 20, 0, kPi, true);
    ctx.fill();
    EXPECT_EQ(alphaAt(ctx, 50, 40), 255);
    EXPECT_EQ(alphaAt(ctx, 50, 60), 0);
    EXPECT_NEAR(coveredArea(ctx), kPi * 400 / 2, 4.0);
}

TEST(Canvas2D, StrokeLineWidth) {
    CanvasRenderingContext2D ctx(20, 20);
    ctx.setLineWidth(4);
    ctx.beginPath();
    ctx.moveTo(2, 10);
    ctx.lineTo(18, 10);
    ctx.stroke();
    EXPECT_EQ(alphaAt(ctx, 10, 8), 255);
    EXPECT_EQ(alphaAt(ctx, 10, 11), 255);
    EXPECT_EQ(alphaAt(ctx, 10, 7), 0);
    EXPECT_EQ(alphaAt(ctx, 10, 12), 0);
    // Butt caps end at the end points
    EXPECT_EQ(alphaAt(ctx, 1, 10), 0);
    EXPECT_NEAR(coveredArea(ctx), 64.0, 0.05);
}

TEST(Canvas2D, OnePixelLineOnPixelBoundaryIsHalfCovered) {
    // Chrome draws this as two half-transparent rows
    CanvasRenderingContext2D ctx(10, 10);
    ctx.beginPath();
    ctx.moveTo(0, 5);
    ctx.lineTo(10, 5);
    ctx.stroke();
    EXPECT_NEAR(alphaAt(ctx, 5, 4), 128, 2);
    EXPECT_NEAR(alphaAt(ctx, 5, 5), 128, 2);
}

TEST(Canvas2D, SquareAndRoundCaps) {
    CanvasRenderingContext2D sq(30, 20);
    sq.setLineWidth(4);
    sq.setLineCap(LineCap::Square);
    sq.beginPath();
    sq.moveTo(10, 10);
    sq.lineTo(20, 10);
    sq.stroke();
    EXPECT_EQ(alphaAt(sq, 8, 10), 255);
    EXPECT_EQ(alphaAt(sq, 21, 10), 255);
    EXPECT_NEAR(coveredArea(sq), 56.0, 0.1);

    CanvasRenderingContext2D rd(30, 20);
    rd.setLineWidth(4);
    rd.setLineCap(LineCap::Round);
    rd.beginPath();
    rd.moveTo(10, 10);
    rd.lineTo(20, 10);
    rd.stroke();
    EXPECT_NEAR(coveredArea(rd), 40.0 + kPi * 4, 0.6);
}

TEST(Canvas2D, ClosedStrokeHasMiterCorners) {
    CanvasRenderingContext2D ctx(30, 30);
    ctx.setLineWidth(4);
    ctx.strokeRect(5, 5, 20, 20);
    // The outer corner (3,3)-(5,5) is filled by the miter join
    EXPECT_EQ(alphaAt(ctx, 3, 3), 255);
    EXPECT_EQ(alphaAt(ctx, 26, 26), 255);
    EXPECT_EQ(alphaAt(ctx, 15, 15), 0);
    EXPECT_NEAR(coveredArea(ctx), 24.0 * 24 - 16.0 * 16, 0.2);

    CanvasRenderingContext2D bevel(30, 30);
    bevel.setLineWidth(4);
    bevel.setLineJoin(LineJoin::Bevel);
    bevel.strokeRect(5, 5, 20, 20);
    EXPECT_EQ(alphaAt(bevel, 3, 3), 0);
    EXPECT_NEAR(coveredArea(bevel), 24.0 * 24 - 16.0 * 16 - 4 * 2.0, 0.3);
}

TEST(Canvas2D, ScaledStrokeIsScaled) {
    CanvasRenderingContext2D ctx(40, 40);
    ctx.scale(4, 1);
    ctx.setLineWidth(2);
    ctx.beginPath();
    ctx.moveTo(5, 0);
    ctx.lineTo(5, 40);
    ctx.stroke();
    // x = 20 +- 4 in device space
    EXPECT_EQ(alphaAt(ctx, 16, 20), 255);
    EXPECT_EQ(alphaAt(ctx, 23, 20), 255);
    EXPECT_EQ(alphaAt(ctx, 15, 20), 0);
    EXPECT_EQ(alphaAt(ctx, 24, 20), 0);
}

TEST(Canvas2D, LineDash) {
    CanvasRenderingContext2D ctx(40, 10);
    ctx.setLineWidth(2);
    ctx.setLineDash({5, 5});
    ctx.beginPath();
    ctx.moveTo(0, 5);
    ctx.lineTo(40, 5);
    ctx.stroke();
    EXPECT_EQ(alphaAt(ctx, 2, 5), 255);
    EXPECT_EQ(alphaAt(ctx, 7, 5), 0);
    EXPECT_EQ(alphaAt(ctx, 12, 5), 255);
    EXPECT_NEAR(coveredArea(ctx), 40.0, 0.1);
    // An odd list repeats; negative values are ignored
    ctx.setLineDash({1, 2, 3});
    EXPECT_EQ(ctx.getLineDash().size(), 6u);
    ctx.setLineDash({1, -2});
    EXPECT_EQ(ctx.getLineDash().size(), 6u);
}

TEST(Canvas2D, LinearGradient) {
    CanvasRenderingContext2D ctx(101, 4);
    CanvasGradient g = ctx.createLinearGradient(0, 0, 101, 0);
    g.addColorStop(0, Color(255, 0, 0));
    g.addColorStop(1, Color(0, 0, 255));
    ctx.setFillStyle(g);
    ctx.fillRect(0, 0, 101, 4);
    Px left = at(ctx, 0, 1), mid = at(ctx, 50, 1), right = at(ctx, 100, 1);
    EXPECT_GE(left.r, 250);
    EXPECT_LE(left.b, 5);
    EXPECT_NEAR(mid.r, 128, 3);
    EXPECT_NEAR(mid.b, 127, 3);
    EXPECT_GE(right.b, 250);
    EXPECT_EQ(mid.a, 255);
}

TEST(Canvas2D, GradientIsInUserSpace) {
    // The gradient follows the transform in effect when filling
    CanvasRenderingContext2D ctx(100, 4);
    CanvasGradient g = ctx.createLinearGradient(0, 0, 50, 0);
    g.addColorStop(0, Color(0, 0, 0));
    g.addColorStop(1, Color(255, 255, 255));
    ctx.setFillStyle(g);
    ctx.scale(2, 1);
    ctx.fillRect(0, 0, 50, 4);
    EXPECT_NEAR(at(ctx, 50, 1).r, 128, 4);
}

TEST(Canvas2D, RadialGradient) {
    CanvasRenderingContext2D ctx(100, 100);
    CanvasGradient g = ctx.createRadialGradient(50, 50, 0, 50, 50, 50);
    g.addColorStop(0, Color(255, 255, 255));
    g.addColorStop(1, Color(0, 0, 0));
    ctx.setFillStyle(g);
    ctx.fillRect(0, 0, 100, 100);
    EXPECT_GE(at(ctx, 50, 50).r, 250);
    EXPECT_NEAR(at(ctx, 75, 50).r, 128, 4);
    EXPECT_LE(at(ctx, 99, 50).r, 5);
    EXPECT_LE(at(ctx, 0, 0).r, 1);  // padded beyond r1
}

TEST(Canvas2D, GradientWithoutStopsPaintsNothing) {
    CanvasRenderingContext2D ctx(10, 10);
    ctx.setFillStyle(ctx.createLinearGradient(0, 0, 10, 0));
    ctx.fillRect(0, 0, 10, 10);
    EXPECT_EQ(coveredArea(ctx), 0.0);
}

TEST(Canvas2D, ClipRestrictsDrawing) {
    CanvasRenderingContext2D ctx(20, 20);
    ctx.save();
    ctx.beginPath();
    ctx.rect(5, 5, 5, 5);
    ctx.clip();
    ctx.fillRect(0, 0, 20, 20);
    ctx.restore();
    EXPECT_NEAR(coveredArea(ctx), 25.0, 0.01);
    ctx.fillRect(0, 0, 1, 1);  // the clip ended with restore()
    EXPECT_EQ(alphaAt(ctx, 0, 0), 255);
}

TEST(Canvas2D, CopyCompositeClearsOutsideTheShape) {
    CanvasRenderingContext2D ctx(10, 10);
    ctx.fillRect(0, 0, 10, 10);
    ctx.setGlobalCompositeOperation(CompositeOp::Copy);
    ctx.setFillStyle(Color(255, 0, 0));
    ctx.fillRect(0, 0, 2, 2);
    EXPECT_EQ(at(ctx, 1, 1).r, 255);
    EXPECT_EQ(alphaAt(ctx, 5, 5), 0);
}

TEST(Canvas2D, DestinationOutErases) {
    CanvasRenderingContext2D ctx(10, 10);
    ctx.fillRect(0, 0, 10, 10);
    ctx.setGlobalCompositeOperation(CompositeOp::DestinationOut);
    ctx.fillRect(0, 0, 5, 10);
    EXPECT_EQ(alphaAt(ctx, 2, 2), 0);
    EXPECT_EQ(alphaAt(ctx, 7, 2), 255);
}

TEST(Canvas2D, ImageDataRoundTrip) {
    CanvasRenderingContext2D ctx(4, 4);
    ImageData d = ctx.createImageData(2, 2);
    d.setPixel(0, 0, 10, 20, 30, 255);
    d.setPixel(1, 1, 200, 100, 50, 128);
    ctx.putImageData(d, 1, 1);
    Px a = at(ctx, 1, 1), b = at(ctx, 2, 2);
    EXPECT_EQ(a.r, 10);
    EXPECT_EQ(a.b, 30);
    EXPECT_EQ(b.a, 128);
    EXPECT_NEAR(b.r, 200, 2);
    EXPECT_EQ(alphaAt(ctx, 2, 1), 0);
    // Out-of-bounds reads are transparent
    ImageData out = ctx.getImageData(-1, -1, 2, 2);
    EXPECT_EQ(out.getA(0, 0), 0);
}

TEST(Canvas2D, DrawImageScales) {
    CanvasRenderingContext2D src(2, 2);
    src.setFillStyle(Color(0, 255, 0));
    src.fillRect(0, 0, 2, 2);
    auto image = src.snapshot();
    CanvasRenderingContext2D ctx(20, 20);
    ctx.drawImage(*image, 5, 5, 10, 10);
    EXPECT_EQ(at(ctx, 10, 10).g, 255);
    EXPECT_EQ(alphaAt(ctx, 4, 10), 0);
    EXPECT_NEAR(coveredArea(ctx), 100.0, 0.5);
}

TEST(Canvas2D, PointInPath) {
    CanvasRenderingContext2D ctx(20, 20);
    ctx.beginPath();
    ctx.rect(5, 5, 10, 10);
    EXPECT_TRUE(ctx.isPointInPath(10, 10));
    EXPECT_FALSE(ctx.isPointInPath(2, 2));
    ctx.setLineWidth(2);
    EXPECT_TRUE(ctx.isPointInStroke(5, 10));
    EXPECT_FALSE(ctx.isPointInStroke(10, 10));
}

TEST(Canvas2D, ResizeClears) {
    CanvasRenderingContext2D ctx(10, 10);
    ctx.fillRect(0, 0, 10, 10);
    uint64_t v = ctx.version();
    ctx.resize(5, 6);
    EXPECT_GT(ctx.version(), v);
    EXPECT_EQ(ctx.width(), 5);
    EXPECT_EQ(coveredArea(ctx), 0.0);
}

TEST(Canvas2D, HugeCanvasGetsNoBitmap) {
    CanvasRenderingContext2D ctx(30000, 30000);
    EXPECT_EQ(ctx.width(), 0);
    ctx.fillRect(0, 0, 10, 10);  // no crash
}

TEST(Canvas2D, FontParsing) {
    CanvasRenderingContext2D ctx(10, 10);
    EXPECT_EQ(ctx.font(), "10px sans-serif");
    EXPECT_TRUE(ctx.setFont("bold 20px Arial, sans-serif"));
    EXPECT_EQ(ctx.font(), "bold 20px Arial, sans-serif");
    EXPECT_FALSE(ctx.setFont("nonsense"));
    EXPECT_FALSE(ctx.setFont("20px"));
    EXPECT_EQ(ctx.font(), "bold 20px Arial, sans-serif");
    EXPECT_TRUE(ctx.setFont("italic 12pt/1.5 \"Times New Roman\", serif"));
}

TEST(Canvas2D, FillTextDrawsGlyphs) {
    CanvasRenderingContext2D ctx(200, 60);
    ctx.setFont("40px sans-serif");
    ctx.fillText("Hello", 10, 45);
    double area = coveredArea(ctx);
    if (area == 0) GTEST_SKIP() << "no system font";
    CanvasTextMetrics m = ctx.measureText("Hello");
    EXPECT_GT(m.width, 60);
    EXPECT_LT(m.width, 140);
    EXPECT_GT(area, 300);
    // Nothing above the em box or left of the start
    for (int y = 0; y < 60; y++) EXPECT_EQ(alphaAt(ctx, 5, y), 0);
    // Centre alignment moves it
    CanvasRenderingContext2D centred(200, 60);
    centred.setFont("40px sans-serif");
    centred.setTextAlign(TextAlign2D::Center);
    centred.fillText("Hello", 100, 45);
    EXPECT_NEAR(coveredArea(centred), area, area * 0.02);
}
