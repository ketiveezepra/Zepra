// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#pragma once

#include "nxgfx/color.h"
#include "nxgfx/math/transform2d.h"
#include <vector>
#include <string>
#include <cstdint>
#include <memory>

namespace NXRender {

// ==================================================================
// Canvas 2D Rendering Context
// HTML Canvas 2D Context specification
//
// The bitmap lives in memory (premultiplied RGBA, width * height * 4
// bytes) and is drawn by a scanline rasterizer, so drawing never needs a
// GL context. The embedder uploads it to a texture when version() changes.
// ==================================================================

enum class LineCap : uint8_t { Butt, Round, Square };
enum class LineJoin : uint8_t { Miter, Round, Bevel };
enum class TextBaseline : uint8_t { Top, Hanging, Middle, Alphabetic, Ideographic, Bottom };
enum class TextAlign2D : uint8_t { Start, End, Left, Right, Center };
enum class FillRule : uint8_t { NonZero, EvenOdd };
enum class CompositeOp : uint8_t {
    SourceOver, SourceIn, SourceOut, SourceAtop,
    DestinationOver, DestinationIn, DestinationOut, DestinationAtop,
    Lighter, Copy, Xor, Multiply, Screen, Overlay,
    Darken, Lighten, ColorDodge, ColorBurn,
    HardLight, SoftLight, Difference, Exclusion,
    Hue, Saturation, ColorOp, Luminosity,
};

enum class RepeatMode : uint8_t { Repeat, RepeatX, RepeatY, NoRepeat };

/// Affine transform in canvas order: x' = a*x + c*y + e, y' = b*x + d*y + f
struct CanvasMatrix {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

    void map(float x, float y, float& ox, float& oy) const {
        ox = a * x + c * y + e;
        oy = b * x + d * y + f;
    }
    /// this * other: other is applied first
    CanvasMatrix operator*(const CanvasMatrix& o) const {
        return {a * o.a + c * o.b, b * o.a + d * o.b,
                a * o.c + c * o.d, b * o.c + d * o.d,
                a * o.e + c * o.f + e, b * o.e + d * o.f + f};
    }
    float determinant() const { return a * d - b * c; }
    bool invertible() const;
    CanvasMatrix inverse() const;
    bool isFinite() const;
    /// Average scale factor, for choosing curve flattening detail
    float scaleFactor() const;
};

struct CanvasGradient {
    enum class Type { Linear, Radial, Conic } type = Type::Linear;
    float x0 = 0, y0 = 0, r0 = 0;
    float x1 = 0, y1 = 0, r1 = 0;
    float angle = 0; // Conic

    struct Stop {
        float offset;
        Color color;
    };
    std::vector<Stop> stops;

    void addColorStop(float offset, const Color& color);
};

/// Pixels a pattern or drawImage reads (premultiplied RGBA, row-major)
struct CanvasImage {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;
};

struct CanvasPattern {
    std::shared_ptr<const CanvasImage> image;
    RepeatMode repeat = RepeatMode::Repeat;
    CanvasMatrix transform;
};

struct CanvasFillStyle {
    enum class Type { Color, Gradient, Pattern } type = Type::Color;
    Color color;
    CanvasGradient gradient;
    CanvasPattern pattern;
};

/// Unpremultiplied RGBA, row-major (the layout of ImageData.data)
struct ImageData {
    int width = 0, height = 0;
    std::vector<uint8_t> data;

    ImageData() = default;
    ImageData(int w, int h);

    uint8_t getR(int x, int y) const;
    uint8_t getG(int x, int y) const;
    uint8_t getB(int x, int y) const;
    uint8_t getA(int x, int y) const;

    void setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
};

struct CanvasTextMetrics {
    float width = 0;
    float actualBoundingBoxLeft = 0;
    float actualBoundingBoxRight = 0;
    float actualBoundingBoxAscent = 0;
    float actualBoundingBoxDescent = 0;
    float fontBoundingBoxAscent = 0;
    float fontBoundingBoxDescent = 0;
    float emHeightAscent = 0;
    float emHeightDescent = 0;
    float hangingBaseline = 0;
    float alphabeticBaseline = 0;
    float ideographicBaseline = 0;
};

// ==================================================================
// Path2D — reusable subpaths (user-space commands)
// ==================================================================

class Path2D {
public:
    Path2D();
    Path2D(const std::string& svgPathData);

    void moveTo(float x, float y);
    void lineTo(float x, float y);
    void quadraticCurveTo(float cpx, float cpy, float x, float y);
    void bezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y);
    void arcTo(float x1, float y1, float x2, float y2, float radius);
    void arc(float x, float y, float radius, float startAngle, float endAngle, bool ccw = false);
    void ellipse(float x, float y, float rx, float ry, float rotation,
                 float startAngle, float endAngle, bool ccw = false);
    void rect(float x, float y, float w, float h);
    void roundRect(float x, float y, float w, float h,
                   float tl, float tr, float br, float bl);
    void closePath();

    void addPath(const Path2D& other);

    struct Command {
        enum Type {
            MoveTo, LineTo, QuadTo, CubicTo, ArcTo, Arc, Ellipse, Rect, Close
        } type;
        float args[8];
        int argCount = 0;
    };

    const std::vector<Command>& commands() const { return commands_; }
    bool empty() const { return commands_.empty(); }

private:
    std::vector<Command> commands_;
    void parseSVGPathData(const std::string& d);
};

namespace Canvas2D { struct FlatPath; struct ClipMask; }

// ==================================================================
// CanvasRenderingContext2D
// ==================================================================

class CanvasRenderingContext2D {
public:
    CanvasRenderingContext2D(int width, int height);
    ~CanvasRenderingContext2D();

    int width() const { return width_; }
    int height() const { return height_; }
    /// New bitmap size: clears the bitmap and resets all state (as setting
    /// canvas.width or canvas.height does)
    void resize(int w, int h);

    /// Premultiplied RGBA, width() * height() * 4 bytes, top row first
    const uint8_t* pixels() const { return pixels_.data(); }
    /// Writes the bitmap unpremultiplied (for a GL_RGBA texture) to out
    void copyUnpremultiplied(uint8_t* out) const;
    /// Increases whenever the bitmap changes
    uint64_t version() const { return version_; }

    // State
    void save();
    void restore();
    void reset();

    // Transform
    void scale(float x, float y);
    void rotate(float angle);
    void translate(float x, float y);
    void transform(float a, float b, float c, float d, float e, float f);
    void setTransform(float a, float b, float c, float d, float e, float f);
    void resetTransform();
    Math::Transform2D getTransform() const;
    const CanvasMatrix& currentTransform() const { return state_.transform; }

    // Compositing
    void setGlobalAlpha(float alpha);
    float globalAlpha() const;
    void setGlobalCompositeOperation(CompositeOp op);
    CompositeOp globalCompositeOperation() const;

    // Fill/Stroke style
    void setFillStyle(const Color& color);
    void setFillStyle(const CanvasGradient& gradient);
    void setFillStyle(const CanvasPattern& pattern);
    void setStrokeStyle(const Color& color);
    void setStrokeStyle(const CanvasGradient& gradient);
    void setStrokeStyle(const CanvasPattern& pattern);
    const CanvasFillStyle& fillStyle() const { return state_.fillStyle; }
    const CanvasFillStyle& strokeStyle() const { return state_.strokeStyle; }

    // Line style
    void setLineWidth(float width);
    float lineWidth() const;
    void setLineCap(LineCap cap);
    LineCap lineCap() const { return state_.lineCap; }
    void setLineJoin(LineJoin join);
    LineJoin lineJoin() const { return state_.lineJoin; }
    void setMiterLimit(float limit);
    float miterLimit() const { return state_.miterLimit; }
    void setLineDash(const std::vector<float>& segments);
    std::vector<float> getLineDash() const;
    void setLineDashOffset(float offset);
    float lineDashOffset() const { return state_.lineDashOffset; }

    // Shadow (stored; shadows are not drawn yet)
    void setShadowColor(const Color& color);
    void setShadowBlur(float blur);
    void setShadowOffsetX(float x);
    void setShadowOffsetY(float y);

    // Text
    /// Returns false (and keeps the old font) when the value can't be parsed
    bool setFont(const std::string& font);
    const std::string& font() const { return state_.font; }
    void setTextAlign(TextAlign2D align);
    TextAlign2D textAlign() const { return state_.textAlign; }
    void setTextBaseline(TextBaseline baseline);
    TextBaseline textBaseline() const { return state_.textBaseline; }
    void setDirection(const std::string& dir);

    // Path operations
    void beginPath();
    void closePath();
    void moveTo(float x, float y);
    void lineTo(float x, float y);
    void quadraticCurveTo(float cpx, float cpy, float x, float y);
    void bezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y);
    void arcTo(float x1, float y1, float x2, float y2, float radius);
    void arc(float x, float y, float radius, float startAngle, float endAngle, bool ccw = false);
    void ellipse(float x, float y, float rx, float ry, float rotation,
                 float startAngle, float endAngle, bool ccw = false);
    void rect(float x, float y, float w, float h);
    void roundRect(float x, float y, float w, float h,
                   float tl, float tr = -1, float br = -1, float bl = -1);

    // Drawing paths
    void fill(FillRule rule = FillRule::NonZero);
    void fill(const Path2D& path, FillRule rule = FillRule::NonZero);
    void stroke();
    void stroke(const Path2D& path);
    void clip(FillRule rule = FillRule::NonZero);
    void clip(const Path2D& path, FillRule rule = FillRule::NonZero);
    /// x, y in canvas pixels (not transformed), as the spec says
    bool isPointInPath(float x, float y, FillRule rule = FillRule::NonZero);
    bool isPointInStroke(float x, float y);

    // Rectangles
    void clearRect(float x, float y, float w, float h);
    void fillRect(float x, float y, float w, float h);
    void strokeRect(float x, float y, float w, float h);

    // Text
    void fillText(const std::string& text, float x, float y, float maxWidth = -1);
    void strokeText(const std::string& text, float x, float y, float maxWidth = -1);
    CanvasTextMetrics measureText(const std::string& text);

    // Image drawing (source in its own pixels; destination in user space)
    void drawImage(const CanvasImage& image, float dx, float dy);
    void drawImage(const CanvasImage& image, float dx, float dy, float dw, float dh);
    void drawImage(const CanvasImage& image, float sx, float sy, float sw, float sh,
                   float dx, float dy, float dw, float dh);
    /// The current bitmap as a drawImage/pattern source
    std::shared_ptr<CanvasImage> snapshot() const;

    // Pixel manipulation
    ImageData createImageData(int width, int height);
    ImageData getImageData(int sx, int sy, int sw, int sh);
    void putImageData(const ImageData& data, int dx, int dy);
    void putImageData(const ImageData& data, int dx, int dy,
                      int dirtyX, int dirtyY, int dirtyW, int dirtyH);

    // Gradient/pattern creation
    CanvasGradient createLinearGradient(float x0, float y0, float x1, float y1);
    CanvasGradient createRadialGradient(float x0, float y0, float r0,
                                         float x1, float y1, float r1);
    CanvasGradient createConicGradient(float startAngle, float x, float y);
    CanvasPattern createPattern(std::shared_ptr<const CanvasImage> image, RepeatMode repeat);

    // Filter (stored; filters are not applied yet)
    void setFilter(const std::string& filter);

private:
    int width_, height_;
    std::vector<uint8_t> pixels_;
    uint64_t version_ = 1;

    // State stack
    struct State {
        CanvasMatrix transform;
        CanvasFillStyle fillStyle;
        CanvasFillStyle strokeStyle;
        float lineWidth = 1.0f;
        LineCap lineCap = LineCap::Butt;
        LineJoin lineJoin = LineJoin::Miter;
        float miterLimit = 10.0f;
        std::vector<float> lineDash;
        float lineDashOffset = 0;
        float globalAlpha = 1.0f;
        CompositeOp compositeOp = CompositeOp::SourceOver;
        Color shadowColor = Color(0, 0, 0, 0);
        float shadowBlur = 0;
        float shadowOffsetX = 0, shadowOffsetY = 0;
        std::string font = "10px sans-serif";
        TextAlign2D textAlign = TextAlign2D::Start;
        TextBaseline textBaseline = TextBaseline::Alphabetic;
        std::string direction = "ltr";
        std::string filter = "none";
        std::shared_ptr<const Canvas2D::ClipMask> clip;  // null = no clip
    };

    State state_;
    std::vector<State> stateStack_;
    std::unique_ptr<Canvas2D::FlatPath> currentPath_;

    void paintPath(const Canvas2D::FlatPath& path, FillRule rule, const CanvasFillStyle& style);
    /// Composites `style` through coverage spans: spans(fn) calls
    /// fn(y, x0, x1, coverage) for each covered run of pixels
    template <typename Spans>
    void paintCoverage(Spans&& spans, const CanvasFillStyle& style);
    /// The device rectangle of a user-space one, when the transform keeps it
    /// axis-aligned
    bool deviceRect(float x, float y, float w, float h, float out[4]) const;
    void strokePath(const Canvas2D::FlatPath& path);
    void drawText(const std::string& text, float x, float y, float maxWidth, bool stroke);
    void changed() { ++version_; }
};

} // namespace NXRender
