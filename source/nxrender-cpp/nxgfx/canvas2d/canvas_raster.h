// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

// Internals of CanvasRenderingContext2D: flattened paths, the scanline
// coverage rasterizer, the stroker and paint/compositing.

#pragma once

#include "canvas_context.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <cstdint>

namespace NXRender {
namespace Canvas2D {

struct Pt { float x, y; };

/// A polygon outline per subpath, in device pixels (or user units while
/// stroking); curves are already flattened to line segments
struct SubPath {
    std::vector<Pt> pts;
    bool closed = false;
};

struct FlatPath {
    std::vector<SubPath> subpaths;

    bool hasCurrentPoint() const { return !subpaths.empty() && !subpaths.back().pts.empty(); }
    Pt currentPoint() const { return subpaths.back().pts.back(); }
    void moveTo(Pt p);
    /// Starts a subpath at p when there is no current point
    void lineTo(Pt p);
    void close();
    bool empty() const;
    FlatPath transformed(const CanvasMatrix& m) const;
};

/// Builds a FlatPath from user-space path commands: points are mapped by
/// the matrix in effect when each command is added, as the spec says
class PathBuilder {
public:
    explicit PathBuilder(FlatPath& out) : out_(out) {}

    void moveTo(const CanvasMatrix& m, float x, float y);
    void lineTo(const CanvasMatrix& m, float x, float y);
    void quadTo(const CanvasMatrix& m, float cpx, float cpy, float x, float y);
    void cubicTo(const CanvasMatrix& m, float c1x, float c1y, float c2x, float c2y, float x, float y);
    void arcTo(const CanvasMatrix& m, float x1, float y1, float x2, float y2, float radius);
    void ellipse(const CanvasMatrix& m, float cx, float cy, float rx, float ry, float rotation,
                 float start, float end, bool ccw);
    void rect(const CanvasMatrix& m, float x, float y, float w, float h);
    void roundRect(const CanvasMatrix& m, float x, float y, float w, float h,
                   float tl, float tr, float br, float bl);
    void close();
    void addPath2D(const CanvasMatrix& m, const Path2D& path);

private:
    FlatPath& out_;
    void ensureStart(const CanvasMatrix& m, float x, float y);
};

/// Per-pixel coverage (0..255) for clip() regions
struct ClipMask {
    int width = 0, height = 0;
    std::vector<uint8_t> coverage;
};

/// Calls span(y, x0, x1, coverage) for each pixel row the path covers:
/// coverage[i] (0..1) is for pixel x0 + i. Coverage is computed with 16
/// sub-scanlines per row and exact horizontal span ends.
template <typename SpanFn>
void rasterize(const FlatPath& path, FillRule rule, int width, int height, SpanFn&& span);

/// Outline of the stroke of a path as polygons, filled with the nonzero rule.
/// `tolerance` is the flattening error allowed for round joins and caps,
/// in the units of the path.
FlatPath strokeOutline(const FlatPath& path, float width, LineCap cap, LineJoin join,
                       float miterLimit, const std::vector<float>& dash, float dashOffset,
                       float tolerance);

/// Resolves a fill style to premultiplied colour per pixel
class Paint {
public:
    /// `toUser` maps device pixels to the user space the style is defined in
    Paint(const CanvasFillStyle& style, const CanvasMatrix& toUser, float globalAlpha);
    bool paintsNothing() const { return nothing_; }
    /// Premultiplied RGBA (0..1) at the centre of device pixel (x, y)
    void shade(int x, int y, float out[4]) const;
    bool isSolid() const { return solid_; }

private:
    const CanvasFillStyle& style_;  // outlives the Paint
    CanvasMatrix toUser_;
    float alpha_;
    bool solid_ = false;
    bool nothing_ = false;
    float color_[4] = {0, 0, 0, 0};
    std::vector<float> lut_;  // 256 premultiplied RGBA entries for gradients
    void gradientColor(float t, float out[4]) const;
};

/// dst (premultiplied RGBA8) = op(src scaled by coverage, dst)
void compositePixel(uint8_t* dst, const float src[4], float coverage, CompositeOp op);
/// True for ops that also change pixels outside the shape (source-in, copy, ...)
bool compositeIsUnbounded(CompositeOp op);

/// Coverage spans of an axis-aligned rectangle in device pixels (exact
/// area coverage), in the same form rasterize() produces
template <typename SpanFn>
void rectSpans(float x0, float y0, float x1, float y1, int width, int height, SpanFn&& span) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    int ix0 = std::max(0, static_cast<int>(std::floor(x0)));
    int ix1 = std::min(width, static_cast<int>(std::ceil(x1)));
    int iy0 = std::max(0, static_cast<int>(std::floor(y0)));
    int iy1 = std::min(height, static_cast<int>(std::ceil(y1)));
    if (ix0 >= ix1 || iy0 >= iy1) return;
    std::vector<float> columns(ix1 - ix0), row(ix1 - ix0);
    for (int x = ix0; x < ix1; x++) {
        columns[x - ix0] = std::clamp(std::min(x1, x + 1.0f) - std::max(x0, static_cast<float>(x)), 0.0f, 1.0f);
    }
    for (int y = iy0; y < iy1; y++) {
        float cy = std::clamp(std::min(y1, y + 1.0f) - std::max(y0, static_cast<float>(y)), 0.0f, 1.0f);
        if (cy <= 0) continue;
        if (cy >= 1.0f) {
            span(y, ix0, ix1, columns.data());
            continue;
        }
        for (size_t i = 0; i < row.size(); i++) row[i] = columns[i] * cy;
        span(y, ix0, ix1, row.data());
    }
}

// ------------------------------------------------------------------
// Rasterizer (template, so span callbacks inline)
// ------------------------------------------------------------------

struct Edge {
    float x0, y0, x1, y1;  // y0 < y1
    float dxdy;
    int dir;
};

void buildEdges(const FlatPath& path, std::vector<Edge>& edges, float& minY, float& maxY,
                float& minX, float& maxX);

template <typename SpanFn>
void rasterize(const FlatPath& path, FillRule rule, int width, int height, SpanFn&& span) {
    constexpr int kSub = 16;
    std::vector<Edge> edges;
    float minY, maxY, minX, maxX;
    buildEdges(path, edges, minY, maxY, minX, maxX);
    if (edges.empty()) return;
    int y0 = std::max(0, static_cast<int>(std::floor(minY)));
    int y1 = std::min(height, static_cast<int>(std::ceil(maxY)));
    int x0 = std::max(0, static_cast<int>(std::floor(minX)));
    int x1 = std::min(width, static_cast<int>(std::ceil(maxX)) + 1);
    if (y0 >= y1 || x0 >= x1) return;
    const int spanW = x1 - x0;

    std::vector<float> acc(spanW + 2), runs(spanW + 2), cov(spanW);
    struct Crossing { float x; int dir; };
    std::vector<Crossing> crossings;
    std::vector<const Edge*> active;
    size_t next = 0;  // edges are sorted by y0
    const float w = 1.0f / kSub;

    auto addSpan = [&](float a, float b) {
        a = std::max(a, static_cast<float>(x0));
        b = std::min(b, static_cast<float>(x1));
        if (b <= a) return;
        float fa = a - x0, fb = b - x0;
        int ia = static_cast<int>(fa), ib = static_cast<int>(fb);
        if (ia == ib) {
            acc[ia] += (fb - fa) * w;
            return;
        }
        acc[ia] += (ia + 1 - fa) * w;
        runs[ia + 1] += w;
        runs[ib] -= w;
        acc[ib] += (fb - ib) * w;
    };

    for (int y = y0; y < y1; y++) {
        std::fill(acc.begin(), acc.end(), 0.0f);
        std::fill(runs.begin(), runs.end(), 0.0f);
        bool any = false;
        for (int s = 0; s < kSub; s++) {
            float sy = y + (s + 0.5f) * w;
            while (next < edges.size() && edges[next].y0 <= sy) active.push_back(&edges[next++]);
            crossings.clear();
            for (size_t i = 0; i < active.size();) {
                const Edge* e = active[i];
                if (e->y1 <= sy) {
                    active[i] = active.back();
                    active.pop_back();
                    continue;
                }
                if (e->y0 <= sy) crossings.push_back({e->x0 + (sy - e->y0) * e->dxdy, e->dir});
                i++;
            }
            if (crossings.size() < 2) continue;
            std::sort(crossings.begin(), crossings.end(),
                      [](const Crossing& p, const Crossing& q) { return p.x < q.x; });
            int winding = 0;
            for (size_t i = 0; i + 1 < crossings.size(); i++) {
                winding += crossings[i].dir;
                bool inside = rule == FillRule::NonZero ? winding != 0 : (winding & 1) != 0;
                if (inside) {
                    addSpan(crossings[i].x, crossings[i + 1].x);
                    any = true;
                }
            }
        }
        if (!any) continue;
        float run = 0;
        int first = -1, last = -1;
        for (int i = 0; i < spanW; i++) {
            run += runs[i];
            float c = std::min(1.0f, acc[i] + run);
            cov[i] = c;
            if (c > 0.0005f) {
                if (first < 0) first = i;
                last = i;
            }
        }
        if (first >= 0) span(y, x0 + first, x0 + last + 1, cov.data() + first);
    }
}

} // namespace Canvas2D
} // namespace NXRender
