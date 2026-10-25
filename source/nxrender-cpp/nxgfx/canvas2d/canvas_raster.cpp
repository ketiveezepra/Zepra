// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "canvas_raster.h"
#include <algorithm>
#include <cmath>

namespace NXRender {
namespace Canvas2D {

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;
constexpr float kTolerance = 0.025f;  // flattening error in device pixels

bool finite(float v) { return std::isfinite(v); }
bool samePoint(Pt a, Pt b) { return std::fabs(a.x - b.x) < 1e-6f && std::fabs(a.y - b.y) < 1e-6f; }
Pt map(const CanvasMatrix& m, float x, float y) {
    Pt p;
    m.map(x, y, p.x, p.y);
    return p;
}
} // namespace

// ==================================================================
// FlatPath
// ==================================================================

void FlatPath::moveTo(Pt p) {
    if (!subpaths.empty() && subpaths.back().pts.size() == 1 && !subpaths.back().closed) {
        subpaths.back().pts[0] = p;  // a lone moveTo is replaced by the next one
        return;
    }
    subpaths.push_back({{p}, false});
}

void FlatPath::lineTo(Pt p) {
    if (!hasCurrentPoint()) {
        moveTo(p);
        return;
    }
    subpaths.back().pts.push_back(p);
}

void FlatPath::close() {
    if (!hasCurrentPoint()) return;
    SubPath& sp = subpaths.back();
    sp.closed = true;
    // The next subpath starts at the first point of the closed one
    subpaths.push_back({{sp.pts.front()}, false});
}

bool FlatPath::empty() const {
    for (const auto& sp : subpaths) {
        if (sp.pts.size() > 1) return false;
    }
    return true;
}

FlatPath FlatPath::transformed(const CanvasMatrix& m) const {
    FlatPath out = *this;
    for (auto& sp : out.subpaths) {
        for (auto& p : sp.pts) p = map(m, p.x, p.y);
    }
    return out;
}

// ==================================================================
// PathBuilder
// ==================================================================

void PathBuilder::ensureStart(const CanvasMatrix& m, float x, float y) {
    if (!out_.hasCurrentPoint()) out_.moveTo(map(m, x, y));
}

void PathBuilder::moveTo(const CanvasMatrix& m, float x, float y) {
    if (!finite(x) || !finite(y)) return;
    out_.moveTo(map(m, x, y));
}

void PathBuilder::lineTo(const CanvasMatrix& m, float x, float y) {
    if (!finite(x) || !finite(y)) return;
    out_.lineTo(map(m, x, y));
}

void PathBuilder::quadTo(const CanvasMatrix& m, float cpx, float cpy, float x, float y) {
    if (!finite(cpx) || !finite(cpy) || !finite(x) || !finite(y)) return;
    ensureStart(m, cpx, cpy);
    Pt p0 = out_.currentPoint(), p1 = map(m, cpx, cpy), p2 = map(m, x, y);
    float dd = std::hypot(p0.x - 2 * p1.x + p2.x, p0.y - 2 * p1.y + p2.y);
    int n = std::clamp(static_cast<int>(std::ceil(std::sqrt(dd / (4 * kTolerance)))), 1, 256);
    for (int i = 1; i <= n; i++) {
        float t = static_cast<float>(i) / n, u = 1 - t;
        out_.lineTo({u * u * p0.x + 2 * u * t * p1.x + t * t * p2.x,
                     u * u * p0.y + 2 * u * t * p1.y + t * t * p2.y});
    }
}

void PathBuilder::cubicTo(const CanvasMatrix& m, float c1x, float c1y, float c2x, float c2y,
                          float x, float y) {
    if (!finite(c1x) || !finite(c1y) || !finite(c2x) || !finite(c2y) || !finite(x) || !finite(y)) return;
    ensureStart(m, c1x, c1y);
    Pt p0 = out_.currentPoint(), p1 = map(m, c1x, c1y), p2 = map(m, c2x, c2y), p3 = map(m, x, y);
    float dd = std::max(std::hypot(p0.x - 2 * p1.x + p2.x, p0.y - 2 * p1.y + p2.y),
                        std::hypot(p1.x - 2 * p2.x + p3.x, p1.y - 2 * p2.y + p3.y));
    int n = std::clamp(static_cast<int>(std::ceil(std::sqrt(dd * 0.75f / kTolerance))), 1, 256);
    for (int i = 1; i <= n; i++) {
        float t = static_cast<float>(i) / n, u = 1 - t;
        float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
        out_.lineTo({a * p0.x + b * p1.x + c * p2.x + d * p3.x,
                     a * p0.y + b * p1.y + c * p2.y + d * p3.y});
    }
}

void PathBuilder::ellipse(const CanvasMatrix& m, float cx, float cy, float rx, float ry,
                          float rotation, float start, float end, bool ccw) {
    if (!finite(cx) || !finite(cy) || !finite(rx) || !finite(ry) || !finite(rotation) ||
        !finite(start) || !finite(end)) return;
    float sweep;
    if (!ccw && end - start >= kTwoPi) {
        sweep = kTwoPi;
    } else if (ccw && start - end >= kTwoPi) {
        sweep = -kTwoPi;
    } else if (!ccw) {
        sweep = std::fmod(end - start, kTwoPi);
        if (sweep < 0) sweep += kTwoPi;
    } else {
        float d = std::fmod(start - end, kTwoPi);
        if (d < 0) d += kTwoPi;
        sweep = -d;
    }
    float cr = std::cos(rotation), sr = std::sin(rotation);
    auto point = [&](float a) {
        float ex = rx * std::cos(a), ey = ry * std::sin(a);
        return map(m, cx + ex * cr - ey * sr, cy + ex * sr + ey * cr);
    };
    // The arc joins the current point with a line, or starts a subpath
    out_.lineTo(point(start));
    float r = std::max(rx, ry) * m.scaleFactor();
    float step = r > kTolerance ? 2.0f * std::acos(std::max(-1.0f, 1.0f - kTolerance / r)) : kPi / 2;
    int n = std::clamp(static_cast<int>(std::ceil(std::fabs(sweep) / step)), 1, 2048);
    for (int i = 1; i <= n; i++) out_.lineTo(point(start + sweep * i / n));
}

void PathBuilder::arcTo(const CanvasMatrix& m, float x1, float y1, float x2, float y2, float radius) {
    if (!finite(x1) || !finite(y1) || !finite(x2) || !finite(y2) || !finite(radius)) return;
    if (!out_.hasCurrentPoint()) {
        moveTo(m, x1, y1);
        return;
    }
    // The last point in user space
    Pt cur = out_.currentPoint();
    if (!m.invertible()) return;
    Pt p0 = map(m.inverse(), cur.x, cur.y);
    Pt p1{x1, y1}, p2{x2, y2};
    float v1x = p0.x - p1.x, v1y = p0.y - p1.y, v2x = p2.x - p1.x, v2y = p2.y - p1.y;
    float l1 = std::hypot(v1x, v1y), l2 = std::hypot(v2x, v2y);
    float cross = v1x * v2y - v1y * v2x;
    if (samePoint(p0, p1) || samePoint(p1, p2) || radius == 0 || l1 == 0 || l2 == 0 ||
        std::fabs(cross) < 1e-6f * l1 * l2) {
        lineTo(m, x1, y1);
        return;
    }
    v1x /= l1; v1y /= l1; v2x /= l2; v2y /= l2;
    float cosTheta = std::clamp(v1x * v2x + v1y * v2y, -1.0f, 1.0f);
    float theta = std::acos(cosTheta);
    float tanDist = radius / std::tan(theta / 2);
    float centerDist = radius / std::sin(theta / 2);
    float bx = v1x + v2x, by = v1y + v2y;
    float bl = std::hypot(bx, by);
    bx /= bl; by /= bl;
    float t1x = p1.x + v1x * tanDist, t1y = p1.y + v1y * tanDist;
    float t2x = p1.x + v2x * tanDist, t2y = p1.y + v2y * tanDist;
    float ccx = p1.x + bx * centerDist, ccy = p1.y + by * centerDist;
    float a0 = std::atan2(t1y - ccy, t1x - ccx), a1 = std::atan2(t2y - ccy, t2x - ccx);
    float d = a1 - a0;
    while (d > kPi) d -= kTwoPi;
    while (d <= -kPi) d += kTwoPi;
    lineTo(m, t1x, t1y);
    ellipse(m, ccx, ccy, radius, radius, 0, a0, a0 + d, d < 0);
}

void PathBuilder::rect(const CanvasMatrix& m, float x, float y, float w, float h) {
    if (!finite(x) || !finite(y) || !finite(w) || !finite(h)) return;
    out_.moveTo(map(m, x, y));
    out_.lineTo(map(m, x + w, y));
    out_.lineTo(map(m, x + w, y + h));
    out_.lineTo(map(m, x, y + h));
    out_.close();
}

void PathBuilder::roundRect(const CanvasMatrix& m, float x, float y, float w, float h,
                            float tl, float tr, float br, float bl) {
    if (!finite(x) || !finite(y) || !finite(w) || !finite(h)) return;
    if (w < 0) { x += w; w = -w; std::swap(tl, tr); std::swap(bl, br); }
    if (h < 0) { y += h; h = -h; std::swap(tl, bl); std::swap(tr, br); }
    tl = std::max(0.0f, tl); tr = std::max(0.0f, tr);
    br = std::max(0.0f, br); bl = std::max(0.0f, bl);
    // Radii that don't fit are scaled down together
    float scale = 1.0f;
    if (tl + tr > 0) scale = std::min(scale, w / (tl + tr));
    if (bl + br > 0) scale = std::min(scale, w / (bl + br));
    if (tl + bl > 0) scale = std::min(scale, h / (tl + bl));
    if (tr + br > 0) scale = std::min(scale, h / (tr + br));
    tl *= scale; tr *= scale; br *= scale; bl *= scale;
    out_.moveTo(map(m, x + tl, y));
    out_.lineTo(map(m, x + w - tr, y));
    if (tr > 0) ellipse(m, x + w - tr, y + tr, tr, tr, 0, -kPi / 2, 0, false);
    out_.lineTo(map(m, x + w, y + h - br));
    if (br > 0) ellipse(m, x + w - br, y + h - br, br, br, 0, 0, kPi / 2, false);
    out_.lineTo(map(m, x + bl, y + h));
    if (bl > 0) ellipse(m, x + bl, y + h - bl, bl, bl, 0, kPi / 2, kPi, false);
    out_.lineTo(map(m, x, y + tl));
    if (tl > 0) ellipse(m, x + tl, y + tl, tl, tl, 0, kPi, 3 * kPi / 2, false);
    out_.close();
}

void PathBuilder::close() { out_.close(); }

void PathBuilder::addPath2D(const CanvasMatrix& m, const Path2D& path) {
    for (const auto& c : path.commands()) {
        const float* a = c.args;
        switch (c.type) {
            case Path2D::Command::MoveTo: moveTo(m, a[0], a[1]); break;
            case Path2D::Command::LineTo: lineTo(m, a[0], a[1]); break;
            case Path2D::Command::QuadTo: quadTo(m, a[0], a[1], a[2], a[3]); break;
            case Path2D::Command::CubicTo: cubicTo(m, a[0], a[1], a[2], a[3], a[4], a[5]); break;
            case Path2D::Command::ArcTo: arcTo(m, a[0], a[1], a[2], a[3], a[4]); break;
            case Path2D::Command::Arc: ellipse(m, a[0], a[1], a[2], a[2], 0, a[3], a[4], a[5] > 0.5f); break;
            case Path2D::Command::Ellipse:
                ellipse(m, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7] > 0.5f);
                break;
            case Path2D::Command::Rect: rect(m, a[0], a[1], a[2], a[3]); break;
            case Path2D::Command::Close: close(); break;
        }
    }
}

// ==================================================================
// Edges
// ==================================================================

void buildEdges(const FlatPath& path, std::vector<Edge>& edges, float& minY, float& maxY,
                float& minX, float& maxX) {
    minY = minX = 1e30f;
    maxY = maxX = -1e30f;
    for (const auto& sp : path.subpaths) {
        size_t n = sp.pts.size();
        if (n < 2) continue;
        for (size_t i = 0; i < n; i++) {
            Pt a = sp.pts[i], b = sp.pts[(i + 1) % n];  // fills close every subpath
            if (!finite(a.x) || !finite(a.y) || !finite(b.x) || !finite(b.y)) continue;
            if (a.y == b.y) continue;
            Edge e;
            e.dir = a.y < b.y ? 1 : -1;
            if (a.y > b.y) std::swap(a, b);
            e.x0 = a.x; e.y0 = a.y; e.x1 = b.x; e.y1 = b.y;
            e.dxdy = (b.x - a.x) / (b.y - a.y);
            edges.push_back(e);
            minY = std::min(minY, a.y); maxY = std::max(maxY, b.y);
            minX = std::min({minX, a.x, b.x}); maxX = std::max({maxX, a.x, b.x});
        }
    }
    std::sort(edges.begin(), edges.end(), [](const Edge& p, const Edge& q) { return p.y0 < q.y0; });
}

// ==================================================================
// Stroker
// ==================================================================

namespace {

// Each piece of a stroke outline is added with the same orientation, so the
// nonzero rule gives their union
void addPolygon(FlatPath& out, std::vector<Pt> pts) {
    if (pts.size() < 3) return;
    float area = 0;
    for (size_t i = 0; i < pts.size(); i++) {
        const Pt& a = pts[i];
        const Pt& b = pts[(i + 1) % pts.size()];
        area += a.x * b.y - b.x * a.y;
    }
    if (area < 0) std::reverse(pts.begin(), pts.end());
    out.subpaths.push_back({std::move(pts), true});
}

void addCircle(FlatPath& out, Pt c, float r, float tolerance) {
    if (r <= 0) return;
    float step = r > tolerance ? 2.0f * std::acos(std::max(-1.0f, 1.0f - tolerance / r)) : kPi / 2;
    int n = std::clamp(static_cast<int>(std::ceil(kTwoPi / step)), 8, 1024);
    std::vector<Pt> pts;
    pts.reserve(n);
    for (int i = 0; i < n; i++) {
        float a = kTwoPi * i / n;
        pts.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)});
    }
    addPolygon(out, std::move(pts));
}

void strokePolyline(FlatPath& out, const std::vector<Pt>& input, bool closed, float hw,
                    LineCap cap, LineJoin join, float miterLimit, float tolerance) {
    std::vector<Pt> pts;
    for (const Pt& p : input) {
        if (pts.empty() || !samePoint(pts.back(), p)) pts.push_back(p);
    }
    size_t n = pts.size();
    if (n < 2) return;
    size_t segs = closed ? n : n - 1;
    std::vector<Pt> dirs(segs), normals(segs);
    for (size_t i = 0; i < segs; i++) {
        Pt a = pts[i], b = pts[(i + 1) % n];
        float len = std::hypot(b.x - a.x, b.y - a.y);
        dirs[i] = {(b.x - a.x) / len, (b.y - a.y) / len};
        normals[i] = {-dirs[i].y * hw, dirs[i].x * hw};
    }
    for (size_t i = 0; i < segs; i++) {
        Pt a = pts[i], b = pts[(i + 1) % n], nn = normals[i];
        addPolygon(out, {{a.x + nn.x, a.y + nn.y}, {b.x + nn.x, b.y + nn.y},
                         {b.x - nn.x, b.y - nn.y}, {a.x - nn.x, a.y - nn.y}});
    }
    // Joins at interior points (and where a closed subpath meets itself)
    for (size_t i = closed ? 0 : 1; i < (closed ? n : n - 1); i++) {
        size_t in = (i + segs - 1) % segs, outSeg = i % segs;
        Pt p = pts[i], d0 = dirs[in], d1 = dirs[outSeg];
        float cross = d0.x * d1.y - d0.y * d1.x;
        float dot = d0.x * d1.x + d0.y * d1.y;
        if (std::fabs(cross) < 1e-6f && dot > 0) continue;  // straight on
        if (join == LineJoin::Round) {
            addCircle(out, p, hw, tolerance);
            continue;
        }
        float s = cross > 0 ? -1.0f : 1.0f;  // the outer side of the turn
        Pt n0 = normals[in], n1 = normals[outSeg];
        Pt a{p.x + s * n0.x, p.y + s * n0.y}, b{p.x + s * n1.x, p.y + s * n1.y};
        // miter length / line width = 1 / cos(turn / 2)
        float cosHalf = std::sqrt(std::max(0.0f, (1 + dot) / 2));
        if (join == LineJoin::Miter && cosHalf > 1e-6f && 1.0f / cosHalf <= miterLimit) {
            float k = 1.0f / (1 + dot);
            Pt tip{p.x + s * (n0.x + n1.x) * k, p.y + s * (n0.y + n1.y) * k};
            addPolygon(out, {p, a, tip, b});
        } else {
            addPolygon(out, {p, a, b});
        }
    }
    if (closed) return;
    if (cap == LineCap::Round) {
        addCircle(out, pts.front(), hw, tolerance);
        addCircle(out, pts.back(), hw, tolerance);
    } else if (cap == LineCap::Square) {
        auto square = [&](Pt p, Pt d, Pt nn) {
            Pt e{p.x + d.x * hw, p.y + d.y * hw};
            addPolygon(out, {{p.x + nn.x, p.y + nn.y}, {e.x + nn.x, e.y + nn.y},
                             {e.x - nn.x, e.y - nn.y}, {p.x - nn.x, p.y - nn.y}});
        };
        square(pts.front(), {-dirs.front().x, -dirs.front().y}, normals.front());
        square(pts.back(), dirs.back(), normals.back());
    }
}

// Splits a polyline into the "on" parts of a dash pattern
void dashPolyline(const std::vector<Pt>& pts, bool closed, const std::vector<float>& dash,
                  float offset, std::vector<std::vector<Pt>>& outLines) {
    float total = 0;
    for (float d : dash) total += d;
    if (total <= 0) return;
    size_t idx = 0;
    float pos = std::fmod(offset, total);
    if (pos < 0) pos += total;
    while (pos >= dash[idx]) {
        pos -= dash[idx];
        idx = (idx + 1) % dash.size();
    }
    float remaining = dash[idx] - pos;
    bool on = idx % 2 == 0;
    std::vector<Pt> current;
    if (on) current.push_back(pts.front());
    size_t n = pts.size();
    size_t segs = closed ? n : n - 1;
    for (size_t i = 0; i < segs; i++) {
        Pt a = pts[i], b = pts[(i + 1) % n];
        float len = std::hypot(b.x - a.x, b.y - a.y), t = 0;
        while (len - t > remaining) {
            t += remaining;
            Pt p{a.x + (b.x - a.x) * t / len, a.y + (b.y - a.y) * t / len};
            if (on) {
                current.push_back(p);
                outLines.push_back(std::move(current));
                current.clear();
            } else {
                current.push_back(p);
            }
            on = !on;
            idx = (idx + 1) % dash.size();
            remaining = dash[idx];
        }
        remaining -= len - t;
        if (on) current.push_back(b);
    }
    if (on && current.size() > 1) outLines.push_back(std::move(current));
}

} // namespace

FlatPath strokeOutline(const FlatPath& path, float width, LineCap cap, LineJoin join,
                       float miterLimit, const std::vector<float>& dash, float dashOffset,
                       float tolerance) {
    FlatPath out;
    if (!(width > 0)) return out;
    float hw = width / 2;
    for (const auto& sp : path.subpaths) {
        std::vector<Pt> pts;
        for (const Pt& p : sp.pts) {
            if (pts.empty() || !samePoint(pts.back(), p)) pts.push_back(p);
        }
        bool closed = sp.closed;
        if (closed && pts.size() > 1 && samePoint(pts.front(), pts.back())) pts.pop_back();
        if (pts.size() < 2) continue;
        if (closed && pts.size() < 3) closed = false;  // a closed line runs there and back
        if (!dash.empty()) {
            std::vector<std::vector<Pt>> lines;
            dashPolyline(pts, closed, dash, dashOffset, lines);
            for (const auto& line : lines) {
                strokePolyline(out, line, false, hw, cap, join, miterLimit, tolerance);
            }
        } else {
            strokePolyline(out, pts, closed, hw, cap, join, miterLimit, tolerance);
        }
    }
    return out;
}

// ==================================================================
// Paint
// ==================================================================

namespace {
void premul(const Color& c, float alpha, float out[4]) {
    float a = (c.a / 255.0f) * alpha;
    out[0] = c.r / 255.0f * a;
    out[1] = c.g / 255.0f * a;
    out[2] = c.b / 255.0f * a;
    out[3] = a;
}

constexpr int kLutSize = 1024;
} // namespace

Paint::Paint(const CanvasFillStyle& style, const CanvasMatrix& toUser, float globalAlpha)
    : style_(style), toUser_(toUser), alpha_(globalAlpha) {
    switch (style.type) {
        case CanvasFillStyle::Type::Color:
            solid_ = true;
            premul(style.color, alpha_, color_);
            nothing_ = color_[3] <= 0;
            break;
        case CanvasFillStyle::Type::Gradient: {
            const CanvasGradient& g = style.gradient;
            if (g.stops.empty()) {
                nothing_ = true;
                break;
            }
            if (g.type == CanvasGradient::Type::Linear && g.x0 == g.x1 && g.y0 == g.y1) {
                nothing_ = true;
                break;
            }
            if (g.type == CanvasGradient::Type::Radial && g.x0 == g.x1 && g.y0 == g.y1 && g.r0 == g.r1) {
                nothing_ = true;
                break;
            }
            // Stops are interpolated unpremultiplied, then premultiplied
            lut_.resize(kLutSize * 4);
            for (int i = 0; i < kLutSize; i++) {
                float t = static_cast<float>(i) / (kLutSize - 1);
                const auto& stops = g.stops;
                Color c = stops.front().color;
                if (t >= stops.back().offset) {
                    c = stops.back().color;
                } else if (t > stops.front().offset) {
                    for (size_t s = 1; s < stops.size(); s++) {
                        if (t <= stops[s].offset) {
                            const auto& a = stops[s - 1];
                            const auto& b = stops[s];
                            float span = b.offset - a.offset;
                            float u = span > 0 ? (t - a.offset) / span : 1.0f;
                            auto lerp = [u](uint8_t p, uint8_t q) {
                                return static_cast<uint8_t>(std::lround(p + (q - p) * u));
                            };
                            c = Color(lerp(a.color.r, b.color.r), lerp(a.color.g, b.color.g),
                                      lerp(a.color.b, b.color.b), lerp(a.color.a, b.color.a));
                            break;
                        }
                    }
                }
                premul(c, alpha_, &lut_[i * 4]);
            }
            break;
        }
        case CanvasFillStyle::Type::Pattern:
            if (!style.pattern.image || style.pattern.image->width <= 0 || style.pattern.image->height <= 0) {
                nothing_ = true;
            }
            break;
    }
}

void Paint::gradientColor(float t, float out[4]) const {
    t = std::clamp(t, 0.0f, 1.0f);
    const float* c = &lut_[static_cast<int>(t * (kLutSize - 1) + 0.5f) * 4];
    out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; out[3] = c[3];
}

void Paint::shade(int x, int y, float out[4]) const {
    if (solid_) {
        out[0] = color_[0]; out[1] = color_[1]; out[2] = color_[2]; out[3] = color_[3];
        return;
    }
    float ux, uy;
    toUser_.map(x + 0.5f, y + 0.5f, ux, uy);
    if (style_.type == CanvasFillStyle::Type::Gradient) {
        const CanvasGradient& g = style_.gradient;
        switch (g.type) {
            case CanvasGradient::Type::Linear: {
                float dx = g.x1 - g.x0, dy = g.y1 - g.y0;
                float t = ((ux - g.x0) * dx + (uy - g.y0) * dy) / (dx * dx + dy * dy);
                gradientColor(t, out);
                return;
            }
            case CanvasGradient::Type::Radial: {
                // Largest t with |p - c(t)| = r(t) and r(t) >= 0 (two-point conical)
                float cdx = g.x1 - g.x0, cdy = g.y1 - g.y0, dr = g.r1 - g.r0;
                float pdx = ux - g.x0, pdy = uy - g.y0;
                float a = cdx * cdx + cdy * cdy - dr * dr;
                float b = pdx * cdx + pdy * cdy + g.r0 * dr;
                float c = pdx * pdx + pdy * pdy - g.r0 * g.r0;
                float t;
                if (std::fabs(a) < 1e-6f) {
                    if (std::fabs(b) < 1e-6f) { out[0] = out[1] = out[2] = out[3] = 0; return; }
                    t = c / (2 * b);
                    if (g.r0 + t * dr < 0) { out[0] = out[1] = out[2] = out[3] = 0; return; }
                } else {
                    float disc = b * b - a * c;
                    if (disc < 0) { out[0] = out[1] = out[2] = out[3] = 0; return; }
                    float sq = std::sqrt(disc);
                    float t1 = (b + sq) / a, t2 = (b - sq) / a;
                    if (t1 < t2) std::swap(t1, t2);
                    if (g.r0 + t1 * dr >= 0) t = t1;
                    else if (g.r0 + t2 * dr >= 0) t = t2;
                    else { out[0] = out[1] = out[2] = out[3] = 0; return; }
                }
                gradientColor(t, out);
                return;
            }
            case CanvasGradient::Type::Conic: {
                float a = std::atan2(uy - g.y0, ux - g.x0) - g.angle;
                float t = std::fmod(a / kTwoPi, 1.0f);
                if (t < 0) t += 1.0f;
                gradientColor(t, out);
                return;
            }
        }
    }
    // Pattern: bilinear sample of the image through the pattern transform
    const CanvasPattern& p = style_.pattern;
    const CanvasImage& img = *p.image;
    float ix, iy;
    p.transform.map(ux, uy, ix, iy);
    bool repX = p.repeat == RepeatMode::Repeat || p.repeat == RepeatMode::RepeatX;
    bool repY = p.repeat == RepeatMode::Repeat || p.repeat == RepeatMode::RepeatY;
    if ((!repX && (ix < 0 || ix >= img.width)) || (!repY && (iy < 0 || iy >= img.height))) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    float fx = ix - 0.5f, fy = iy - 0.5f;
    int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    float tx = fx - x0, ty = fy - y0;
    auto texel = [&](int sx, int sy, int k) -> float {
        if (repX) { sx %= img.width; if (sx < 0) sx += img.width; }
        else sx = std::clamp(sx, 0, img.width - 1);
        if (repY) { sy %= img.height; if (sy < 0) sy += img.height; }
        else sy = std::clamp(sy, 0, img.height - 1);
        return img.pixels[(static_cast<size_t>(sy) * img.width + sx) * 4 + k] / 255.0f;
    };
    for (int k = 0; k < 4; k++) {
        float top = texel(x0, y0, k) * (1 - tx) + texel(x0 + 1, y0, k) * tx;
        float bottom = texel(x0, y0 + 1, k) * (1 - tx) + texel(x0 + 1, y0 + 1, k) * tx;
        out[k] = (top * (1 - ty) + bottom * ty) * alpha_;
    }
}

// ==================================================================
// Compositing
// ==================================================================

bool compositeIsUnbounded(CompositeOp op) {
    switch (op) {
        case CompositeOp::SourceIn:
        case CompositeOp::SourceOut:
        case CompositeOp::DestinationIn:
        case CompositeOp::DestinationAtop:
        case CompositeOp::Copy:
            return true;
        default:
            return false;
    }
}

namespace {
// Separable blend function B(cb, cs) on unpremultiplied channels
float blendChannel(CompositeOp op, float cb, float cs) {
    switch (op) {
        case CompositeOp::Multiply: return cb * cs;
        case CompositeOp::Screen: return cb + cs - cb * cs;
        case CompositeOp::Overlay: return cb <= 0.5f ? 2 * cs * cb : 1 - 2 * (1 - cs) * (1 - cb);
        case CompositeOp::Darken: return std::min(cb, cs);
        case CompositeOp::Lighten: return std::max(cb, cs);
        case CompositeOp::ColorDodge:
            if (cb <= 0) return 0;
            return cs >= 1 ? 1 : std::min(1.0f, cb / (1 - cs));
        case CompositeOp::ColorBurn:
            if (cb >= 1) return 1;
            return cs <= 0 ? 0 : 1 - std::min(1.0f, (1 - cb) / cs);
        case CompositeOp::HardLight: return cs <= 0.5f ? 2 * cs * cb : 1 - 2 * (1 - cs) * (1 - cb);
        case CompositeOp::SoftLight: {
            if (cs <= 0.5f) return cb - (1 - 2 * cs) * cb * (1 - cb);
            float d = cb <= 0.25f ? ((16 * cb - 12) * cb + 4) * cb : std::sqrt(cb);
            return cb + (2 * cs - 1) * (d - cb);
        }
        case CompositeOp::Difference: return std::fabs(cb - cs);
        case CompositeOp::Exclusion: return cb + cs - 2 * cb * cs;
        default: return cs;
    }
}
} // namespace

void compositePixel(uint8_t* dst, const float src[4], float coverage, CompositeOp op) {
    float s[4] = {src[0] * coverage, src[1] * coverage, src[2] * coverage, src[3] * coverage};
    float d[4] = {dst[0] / 255.0f, dst[1] / 255.0f, dst[2] / 255.0f, dst[3] / 255.0f};
    float sa = s[3], da = d[3];
    float r[4];
    // Porter-Duff: result = s * Fa + d * Fb
    auto porterDuff = [&](float fa, float fb) {
        for (int k = 0; k < 4; k++) r[k] = s[k] * fa + d[k] * fb;
    };
    switch (op) {
        case CompositeOp::SourceOver: porterDuff(1, 1 - sa); break;
        case CompositeOp::SourceIn: porterDuff(da, 0); break;
        case CompositeOp::SourceOut: porterDuff(1 - da, 0); break;
        case CompositeOp::SourceAtop: porterDuff(da, 1 - sa); break;
        case CompositeOp::DestinationOver: porterDuff(1 - da, 1); break;
        case CompositeOp::DestinationIn: porterDuff(0, sa); break;
        case CompositeOp::DestinationOut: porterDuff(0, 1 - sa); break;
        case CompositeOp::DestinationAtop: porterDuff(1 - da, sa); break;
        case CompositeOp::Lighter: for (int k = 0; k < 4; k++) r[k] = std::min(1.0f, s[k] + d[k]); break;
        case CompositeOp::Copy: porterDuff(1, 0); break;
        case CompositeOp::Xor: porterDuff(1 - da, 1 - sa); break;
        case CompositeOp::Multiply: case CompositeOp::Screen: case CompositeOp::Overlay:
        case CompositeOp::Darken: case CompositeOp::Lighten: case CompositeOp::ColorDodge:
        case CompositeOp::ColorBurn: case CompositeOp::HardLight: case CompositeOp::SoftLight:
        case CompositeOp::Difference: case CompositeOp::Exclusion:
            for (int k = 0; k < 3; k++) {
                float cs = sa > 0 ? s[k] / sa : 0, cb = da > 0 ? d[k] / da : 0;
                r[k] = s[k] * (1 - da) + d[k] * (1 - sa) + sa * da * blendChannel(op, cb, cs);
            }
            r[3] = sa + da - sa * da;
            break;
        default:  // non-separable modes are drawn as source-over
            porterDuff(1, 1 - sa);
            break;
    }
    for (int k = 0; k < 4; k++) {
        dst[k] = static_cast<uint8_t>(std::clamp(r[k], 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    // Premultiplied colour never exceeds alpha
    for (int k = 0; k < 3; k++) dst[k] = std::min(dst[k], dst[3]);
}

} // namespace Canvas2D
} // namespace NXRender
