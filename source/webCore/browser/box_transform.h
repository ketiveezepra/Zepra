// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * box_transform.h - CSS Transforms 1/2 for the box tree: the 2D matrix of a
 * box's `transform` (with the translate, rotate and scale properties ahead of
 * it) about its transform-origin.
 *
 * A transform does not change layout. paintBox draws the box and everything
 * inside it through the matrix; hit testing and script geometry map points
 * and rectangles through it. 3D functions are flattened to the plane (no
 * perspective): z translations are dropped and rotations about the x and y
 * axes become the scale their projection has.
 */

#pragma once

#include <string>

namespace ZepraBrowser {

struct LayoutBox;

/// x' = a*x + c*y + e, y' = b*x + d*y + f (the CSS matrix(a, b, c, d, e, f))
struct Affine {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

    static Affine translation(float x, float y) { return {1, 0, 0, 1, x, y}; }
    static Affine scaling(float sx, float sy) { return {sx, 0, 0, sy, 0, 0}; }
    static Affine rotation(float radians);

    bool isIdentity() const;
    /// Moves points without turning or resizing them
    bool isTranslation() const;
    /// this applied after `inner`: (this * inner)(p) = this(inner(p))
    Affine operator*(const Affine& inner) const;
    /// False for a matrix that flattens the plane (scale(0)): nothing to hit
    bool invert(Affine& out) const;
    void map(float& x, float& y) const;
    /// The bounding box of a mapped rectangle
    void mapRect(float& x, float& y, float& w, float& h) const;
};

/// The viewport vw, vh, vmin and vmax in transforms are of (the layout
/// viewport: setLayoutViewport passes it on)
void setTransformViewport(float width, float height);

/// A length of a transform: pixels plus a percentage of the reference box
struct TransformLength {
    float px = 0, percent = 0;
    float resolve(float reference) const { return px + percent / 100.0f * reference; }
};

/// One length (px, %, em, rem, vw, vh, vmin, vmax, a bare 0, or calc() of
/// them); false when it is not one
bool parseTransformLength(const std::string& token, float fontSize, TransformLength& out);

/// The matrix of a transform function list ("translate(10px, 50%)
/// rotate(45deg)", "matrix(...)", "none"). Percentages are of the box's
/// border box (width, height). False when the list is not valid: an invalid
/// transform is no transform.
bool parseTransformList(const std::string& css, float width, float height, float fontSize, Affine& out);

/// The transform functions the translate, rotate and scale properties stand
/// for, in the order they apply ("translate(..) rotate(..) scale(..)"), to
/// go in front of the element's `transform` list. "" when all are none.
std::string individualTransformFunctions(const std::string& translate, const std::string& rotate,
                                         const std::string& scale);

/// The resolved value of `transform` (getComputedStyle): "none", or
/// matrix(a, b, c, d, e, f) of the list with percentages of a width x height
/// box. An invalid list resolves to "none".
std::string resolvedTransformValue(const std::string& css, float width, float height, float fontSize);

/// The transform-origin point inside a width x height box ("" is 50% 50%)
void parseTransformOrigin(const std::string& css, float width, float height, float fontSize, float& x, float& y);

/// The matrix `box` paints through when its border box is at (x, y):
/// its transform about its origin, in the coordinates (x, y) is given in.
/// False when the box has no transform (or one that does nothing).
bool boxTransform(const LayoutBox& box, float x, float y, Affine& out);

/// Takes a point from where `box` appears to where it was laid out (its
/// screenX/screenY as last painted or placed): hit testing a transformed
/// box tests the mapped point against it and its children. False when the
/// box is flattened to nothing, so it cannot be hit.
bool toBoxSpace(const LayoutBox& box, float& x, float& y);

}  // namespace ZepraBrowser
