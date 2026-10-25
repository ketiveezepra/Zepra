// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// SVG DOM Extraction — Extracts embedded <svg> elements from HTML DOM
// for rendering via nxsvg. Uses DOMElement::outerHTML() for serialization.

#pragma once

#include <string>
#include <cctype>
#include <cstdio>
#include "browser/dom.hpp"

namespace ZepraBrowser::UI {

using namespace Zepra::WebCore;

// Extract viewBox dimensions from an <svg> element's attributes.
// Falls back to width/height attrs, then default 24x24.
struct SvgDimensions {
    float width = 24;
    float height = 24;
};

inline SvgDimensions getSvgDimensions(const DOMElement* svgElement) {
    SvgDimensions dim;
    
    std::string w = svgElement->getAttribute("width");
    std::string h = svgElement->getAttribute("height");
    // The HTML tokenizer lowercases attribute names
    std::string vb = svgElement->getAttribute("viewbox");
    if (vb.empty()) vb = svgElement->getAttribute("viewBox");
    
    if (!w.empty()) { try { dim.width = std::stof(w); } catch (...) {} }
    if (!h.empty()) { try { dim.height = std::stof(h); } catch (...) {} }
    
    // viewBox overrides when explicit size not set
    if (!vb.empty() && (w.empty() || h.empty())) {
        float vx, vy, vw, vh;
        if (sscanf(vb.c_str(), "%f %f %f %f", &vx, &vy, &vw, &vh) >= 4) {
            if (w.empty()) dim.width = vw;
            if (h.empty()) dim.height = vh;
        }
    }
    
    return dim;
}

// Serialize an <svg> element subtree to an XML string for nxsvg.
// The HTML tokenizer lowercased the attribute names; SVG's mixed-case ones
// get their case back (HTML "adjust SVG attributes", §13.2.6.3)
inline std::string extractSvgString(const DOMElement* svgElement) {
    if (!svgElement) return "";
    std::string svg = svgElement->outerHTML();
    static const char* const kMixedCase[] = {
        "viewBox", "preserveAspectRatio", "gradientUnits", "gradientTransform", "patternUnits",
        "patternContentUnits", "patternTransform", "clipPathUnits", "maskUnits", "maskContentUnits",
        "markerWidth", "markerHeight", "markerUnits", "refX", "refY", "stdDeviation", "textLength",
        "lengthAdjust", "startOffset", "spreadMethod", "pathLength", "baseFrequency", "numOctaves"};
    for (const char* name : kMixedCase) {
        std::string lower(name);
        for (char& c : lower) c = (char)std::tolower((unsigned char)c);
        std::string from = " " + lower + "=\"", to = std::string(" ") + name + "=\"";
        for (size_t pos = svg.find(from); pos != std::string::npos; pos = svg.find(from, pos + to.size()))
            svg.replace(pos, from.size(), to);
    }
    return svg;
}

} // namespace ZepraBrowser::UI
