// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * svg_raster_tests.cpp - SVG documents rasterized through SvgRaster (nanosvg
 * plus the rewrite pass), checked pixel by pixel. The cases follow
 * test/imagetest/svg/*.svg, whose pages compare the browser with Chrome.
 */

#include <gtest/gtest.h>

#include "browser/svg_raster.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace ZepraBrowser;

namespace {

struct Pixel {
    int r, g, b, a;
};

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    Pixel at(int x, int y) const {
        const uint8_t* p = &rgba[((size_t)y * width + x) * 4];
        return {p[0], p[1], p[2], p[3]};
    }
};

Image raster(const std::string& svg, int w, int h, uint32_t currentColor = 0, bool tint = false) {
    Image img;
    img.width = w;
    img.height = h;
    SvgRaster::Options options;
    options.currentColor = currentColor;
    options.whiteIsCurrentColor = tint;
    EXPECT_TRUE(SvgRaster::rasterize(svg, w, h, options, img.rgba));
    if (img.rgba.size() != (size_t)w * h * 4) img.rgba.assign((size_t)w * h * 4, 0);
    return img;
}

// Opaque colour within 'tolerance' per channel
void expectColor(const Pixel& p, int r, int g, int b, int a = 255, int tolerance = 3) {
    EXPECT_NEAR(p.r, r, tolerance);
    EXPECT_NEAR(p.g, g, tolerance);
    EXPECT_NEAR(p.b, b, tolerance);
    EXPECT_NEAR(p.a, a, tolerance);
}

std::string square(const std::string& fill) {
    return "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'><rect width='10' height='10' fill='" +
           fill + "'/></svg>";
}

} // namespace

TEST(SvgColors, CurrentColorComesFromTheElement) {
    expectColor(raster(square("currentColor"), 10, 10, 0x00AA77).at(5, 5), 0x00, 0xAA, 0x77);
    // The color attribute changes it for the subtree
    std::string svg = "<svg width='10' height='10' color='#f00'><g color='#00f'>"
                      "<rect width='10' height='10' fill='currentColor'/></g></svg>";
    expectColor(raster(svg, 10, 10, 0x00FF00).at(5, 5), 0, 0, 255);
}

TEST(SvgColors, FunctionalAndHexForms) {
    expectColor(raster(square("hsl(240, 100%, 50%)"), 10, 10).at(5, 5), 0, 0, 255);
    expectColor(raster(square("rgb(50%, 20%, 80%)"), 10, 10).at(5, 5), 128, 51, 204);
    expectColor(raster(square("rgb(255 128 0)"), 10, 10).at(5, 5), 255, 128, 0);
    expectColor(raster(square("rebeccapurple"), 10, 10).at(5, 5), 102, 51, 153);
    expectColor(raster(square("cornflowerblue"), 10, 10).at(5, 5), 100, 149, 237);
    // Colours with alpha: straight RGB, alpha in the alpha channel
    expectColor(raster(square("rgba(255, 0, 0, 0.5)"), 10, 10).at(5, 5), 255, 0, 0, 127);
    expectColor(raster(square("hsla(120, 100%, 25%, 0.5)"), 10, 10).at(5, 5), 0, 128, 0, 127);
    expectColor(raster(square("#11aa3380"), 10, 10).at(5, 5), 0x11, 0xAA, 0x33, 128);
    expectColor(raster(square("#f008"), 10, 10).at(5, 5), 255, 0, 0, 136);
    EXPECT_EQ(raster(square("transparent"), 10, 10).at(5, 5).a, 0);
}

TEST(SvgColors, NestedOpacityMultiplies) {
    std::string svg = "<svg width='10' height='10'><g opacity='0.5'>"
                      "<rect width='10' height='10' fill='#000' opacity='0.5'/></g></svg>";
    EXPECT_NEAR(raster(svg, 10, 10).at(5, 5).a, 64, 2);
}

TEST(SvgGradients, BoundingBoxNumbersAreFractions) {
    // y2="1" is the bottom of the shape, not 1px
    std::string svg = "<svg width='10' height='100'><defs><linearGradient id='v' x1='0' y1='0' x2='0' y2='1'>"
                      "<stop offset='0' stop-color='#00f'/><stop offset='1' stop-color='#f00'/></linearGradient></defs>"
                      "<rect width='10' height='100' fill='url(#v)'/></svg>";
    Image img = raster(svg, 10, 100);
    EXPECT_GT(img.at(5, 2).b, 230);
    EXPECT_GT(img.at(5, 97).r, 230);
    Pixel mid = img.at(5, 50);
    EXPECT_NEAR(mid.r, 128, 12);
    EXPECT_NEAR(mid.b, 128, 12);
}

TEST(SvgGradients, StopStyleAndStopOpacity) {
    std::string svg = "<svg width='100' height='10'><defs><linearGradient id='h'>"
                      "<stop offset='0%' style='stop-color:#0c0'/>"
                      "<stop offset='100%' style='stop-color:#000;stop-opacity:0.2'/></linearGradient></defs>"
                      "<rect width='100' height='10' fill='url(#h)'/></svg>";
    Image img = raster(svg, 100, 10);
    expectColor(img.at(0, 5), 0, 0xCC, 0, 255, 8);
    EXPECT_NEAR(img.at(99, 5).a, 51, 8);
}

TEST(SvgGradients, GradientTransformRotatesInTheBox) {
    // Rotated 90 degrees about the centre: the gradient runs top to bottom
    std::string svg = "<svg width='100' height='100'><defs><linearGradient id='g' "
                      "gradientTransform='rotate(90 0.5 0.5)'><stop offset='0' stop-color='#00f'/>"
                      "<stop offset='1' stop-color='#f00'/></linearGradient></defs>"
                      "<rect width='100' height='100' fill='url(#g)'/></svg>";
    Image img = raster(svg, 100, 100);
    EXPECT_GT(img.at(50, 2).b, 230);
    EXPECT_GT(img.at(50, 97).r, 230);
    EXPECT_NEAR(img.at(2, 50).r, img.at(97, 50).r, 6);
}

TEST(SvgGradients, RadialFocalPoint) {
    // The first stop sits at the focus (30%, 30%), not at the centre
    std::string svg = "<svg width='100' height='100'><defs><radialGradient id='r' fx='0.3' fy='0.3'>"
                      "<stop offset='0' stop-color='#fff'/><stop offset='1' stop-color='#000'/></radialGradient></defs>"
                      "<rect width='100' height='100' fill='url(#r)'/></svg>";
    Image img = raster(svg, 100, 100);
    EXPECT_GT(img.at(30, 30).r, 240);
    EXPECT_LT(img.at(50, 50).r, 200);
    EXPECT_GT(img.at(40, 40).r, img.at(60, 60).r);
}

TEST(SvgStructure, GroupTransformsNest) {
    std::string svg = "<svg width='20' height='20'><g transform='translate(10 0)'><g transform='scale(0.5)'>"
                      "<rect width='20' height='20' fill='#f00'/></g></g></svg>";
    Image img = raster(svg, 20, 20);
    EXPECT_EQ(img.at(5, 5).a, 0);
    expectColor(img.at(15, 5), 255, 0, 0);
    EXPECT_EQ(img.at(15, 15).a, 0);
}

TEST(SvgStructure, UseCopiesTheElementAndInheritsFill) {
    std::string svg = "<svg xmlns:xlink='http://www.w3.org/1999/xlink' width='30' height='10'>"
                      "<defs><rect id='r' width='10' height='10'/></defs>"
                      "<use href='#r' fill='#f00'/><use xlink:href='#r' x='20' fill='#00f'/></svg>";
    Image img = raster(svg, 30, 10);
    expectColor(img.at(5, 5), 255, 0, 0);
    EXPECT_EQ(img.at(15, 5).a, 0);  // the <defs> original is not drawn
    expectColor(img.at(25, 5), 0, 0, 255);
}

TEST(SvgStructure, SymbolViewBoxMapsOntoTheUseSize) {
    std::string svg = "<svg width='40' height='40'><symbol id='s' viewBox='0 0 2 2'>"
                      "<rect width='1' height='1' fill='#0f0'/></symbol>"
                      "<use href='#s' x='0' y='0' width='40' height='40'/></svg>";
    Image img = raster(svg, 40, 40);
    expectColor(img.at(10, 10), 0, 255, 0);
    EXPECT_EQ(img.at(30, 30).a, 0);
}

TEST(SvgStructure, StyleSheetClassesAndIds) {
    std::string svg = "<svg width='30' height='10'><style><![CDATA[ .a { fill: #00f } #b { fill: #f00 } "
                      "rect.c { fill: #0f0 } ]]></style>"
                      "<rect class='a' width='10' height='10' fill='#000'/>"
                      "<rect id='b' x='10' width='10' height='10'/>"
                      "<rect class='c' x='20' width='10' height='10' style='fill:#ff0'/></svg>";
    Image img = raster(svg, 30, 10);
    expectColor(img.at(5, 5), 0, 0, 255);    // CSS beats the presentation attribute
    expectColor(img.at(15, 5), 255, 0, 0);
    expectColor(img.at(25, 5), 255, 255, 0); // inline style beats CSS
}

TEST(SvgStructure, HtmlParserOutput) {
    // What an inline <svg> looks like after the HTML parser: lowercased names,
    // and the siblings of a "/>" element nested inside it
    std::string svg = "<svg width='20' height='10' viewbox='0 0 20 10'><defs><lineargradient id='g'>"
                      "<stop offset='0' stop-color='#f00'><stop offset='1' stop-color='#f00'></stop></stop>"
                      "</lineargradient><rect id='r' width='10' height='10'></rect></defs>"
                      "<use href='#r' fill='url(#g)'><use href='#r' x='10' fill='#00f'></use></use></svg>";
    Image img = raster(svg, 20, 10);
    expectColor(img.at(5, 5), 255, 0, 0);
    expectColor(img.at(15, 5), 0, 0, 255);
}

TEST(SvgStructure, UnpaintedElementsAreDropped) {
    std::string svg = "<svg width='10' height='10'><clipPath id='c'><rect width='10' height='10'/></clipPath>"
                      "<mask id='m'><rect width='10' height='10' fill='#fff'/></mask></svg>";
    EXPECT_EQ(raster(svg, 10, 10).at(5, 5).a, 0);
}

TEST(SvgIcons, WhitePaintTakesTheTint) {
    std::string svg = "<svg width='10' height='10' fill='none'><rect width='5' height='10' fill='white'/>"
                      "<rect x='5' width='5' height='10' fill='#271C1C'/></svg>";
    Image img = raster(svg, 10, 10, 0x3366CC, true);
    expectColor(img.at(2, 5), 0x33, 0x66, 0xCC);
    expectColor(img.at(7, 5), 0x27, 0x1C, 0x1C);
}

TEST(SvgSize, IntrinsicSize) {
    float w = 0, h = 0;
    ASSERT_TRUE(SvgRaster::intrinsicSize("<svg width='120' height='60' viewBox='0 0 20 10'/>", w, h));
    EXPECT_FLOAT_EQ(w, 120);
    EXPECT_FLOAT_EQ(h, 60);
    // viewBox only: fitted into 300x150
    ASSERT_TRUE(SvgRaster::intrinsicSize("<?xml version='1.0'?><svg viewBox='0 0 120 18'/>", w, h));
    EXPECT_FLOAT_EQ(w, 300);
    EXPECT_FLOAT_EQ(h, 45);
    ASSERT_TRUE(SvgRaster::intrinsicSize("<svg viewBox='0 0 24 24'/>", w, h));
    EXPECT_FLOAT_EQ(w, 150);
    EXPECT_FLOAT_EQ(h, 150);
    ASSERT_TRUE(SvgRaster::intrinsicSize("<svg width='48' viewBox='0 0 24 12'/>", w, h));
    EXPECT_FLOAT_EQ(h, 24);
    ASSERT_TRUE(SvgRaster::intrinsicSize("<svg width='0.5in' height='12pt'/>", w, h));
    EXPECT_FLOAT_EQ(w, 48);
    EXPECT_FLOAT_EQ(h, 16);
    ASSERT_TRUE(SvgRaster::intrinsicSize("<svg/>", w, h));
    EXPECT_FLOAT_EQ(w, 300);
    EXPECT_FLOAT_EQ(h, 150);
    EXPECT_FALSE(SvgRaster::intrinsicSize("<html></html>", w, h));
}

TEST(SvgSize, ScaledUniformlyAndCentred) {
    // A 20x10 document in a 20x20 box: drawn 20x10, centred vertically
    std::string svg = "<svg width='20' height='10'><rect width='20' height='10' fill='#f00'/></svg>";
    Image img = raster(svg, 20, 20);
    EXPECT_EQ(img.at(10, 2).a, 0);
    expectColor(img.at(10, 10), 255, 0, 0);
    EXPECT_EQ(img.at(10, 17).a, 0);
}
