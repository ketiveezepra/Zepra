// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

// Canvas text: the CSS font shorthand, font files, and glyph outlines from
// FreeType filled and stroked like any other path (so transforms apply).

#include "canvas_context.h"
#include "canvas_raster.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <unordered_map>

namespace NXRender {

namespace {

struct FontSpec {
    float size = 10;
    bool bold = false;
    bool italic = false;
    std::vector<std::string> families;
};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\n\r\f"), b = s.find_last_not_of(" \t\n\r\f");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// CSS font shorthand: [style] [variant] [weight] [stretch] size[/line-height] family[, family]*
bool parseFont(const std::string& value, FontSpec& out) {
    std::string s = trim(value);
    if (s.empty()) return false;
    FontSpec spec;
    size_t pos = 0;
    bool haveSize = false;
    while (pos < s.size()) {
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) pos++;
        size_t end = pos;
        while (end < s.size() && !std::isspace(static_cast<unsigned char>(s[end]))) end++;
        std::string token = lower(s.substr(pos, end - pos));
        if (token.empty()) break;
        if (std::isdigit(static_cast<unsigned char>(token[0])) || token[0] == '.') {
            std::string sizePart = token.substr(0, token.find('/'));
            char* unitStart = nullptr;
            float number = std::strtof(sizePart.c_str(), &unitStart);
            std::string unit = unitStart ? unitStart : "";
            if (unit.empty()) {
                // A bare number before the size is a font-weight
                if (number >= 600) spec.bold = true;
                pos = end;
                continue;
            }
            if (unit == "px") spec.size = number;
            else if (unit == "pt") spec.size = number * 4.0f / 3.0f;
            else if (unit == "pc") spec.size = number * 16.0f;
            else if (unit == "in") spec.size = number * 96.0f;
            else if (unit == "cm") spec.size = number * 96.0f / 2.54f;
            else if (unit == "mm") spec.size = number * 96.0f / 25.4f;
            else if (unit == "em" || unit == "rem") spec.size = number * 16.0f;
            else if (unit == "%") spec.size = number * 16.0f / 100.0f;
            else return false;
            haveSize = true;
            pos = end;
            break;
        }
        if (token == "italic" || token == "oblique") spec.italic = true;
        else if (token == "bold" || token == "bolder") spec.bold = true;
        else if (token == "normal" || token == "small-caps" || token == "lighter" ||
                 token.find("condensed") != std::string::npos || token.find("expanded") != std::string::npos) {
        } else {
            return false;  // a family before the size
        }
        pos = end;
    }
    if (!haveSize || !(spec.size >= 0) || !std::isfinite(spec.size)) return false;
    std::string rest = s.substr(std::min(pos, s.size()));
    std::stringstream families(rest);
    std::string family;
    while (std::getline(families, family, ',')) {
        family = trim(family);
        if (family.size() >= 2 && (family.front() == '"' || family.front() == '\'')) {
            family = family.substr(1, family.size() - 2);
        }
        if (!family.empty()) spec.families.push_back(lower(family));
    }
    if (spec.families.empty()) return false;
    out = spec;
    return true;
}

// Font files to try for a family, regular/bold/italic/bold-italic
std::vector<std::string> fontCandidates(const std::string& family, bool bold, bool italic) {
    int style = (bold ? 1 : 0) + (italic ? 2 : 0);
    std::vector<std::string> out;
#ifdef _WIN32
    const char* winDir = std::getenv("WINDIR");
    std::string dir = std::string(winDir ? winDir : "C:\\Windows") + "\\Fonts\\";
    struct Family { const char* names[4]; };
    static const std::unordered_map<std::string, Family> files = {
        {"arial", {{"arial.ttf", "arialbd.ttf", "ariali.ttf", "arialbi.ttf"}}},
        {"times new roman", {{"times.ttf", "timesbd.ttf", "timesi.ttf", "timesbi.ttf"}}},
        {"courier new", {{"cour.ttf", "courbd.ttf", "couri.ttf", "courbi.ttf"}}},
        {"consolas", {{"consola.ttf", "consolab.ttf", "consolai.ttf", "consolaz.ttf"}}},
        {"verdana", {{"verdana.ttf", "verdanab.ttf", "verdanai.ttf", "verdanaz.ttf"}}},
        {"georgia", {{"georgia.ttf", "georgiab.ttf", "georgiai.ttf", "georgiaz.ttf"}}},
        {"tahoma", {{"tahoma.ttf", "tahomabd.ttf", "tahoma.ttf", "tahomabd.ttf"}}},
        {"segoe ui", {{"segoeui.ttf", "segoeuib.ttf", "segoeuii.ttf", "segoeuiz.ttf"}}},
        {"trebuchet ms", {{"trebuc.ttf", "trebucbd.ttf", "trebucit.ttf", "trebucbi.ttf"}}},
        {"impact", {{"impact.ttf", "impact.ttf", "impact.ttf", "impact.ttf"}}},
    };
    // Chrome's defaults on Windows
    static const std::unordered_map<std::string, std::string> aliases = {
        {"sans-serif", "arial"}, {"helvetica", "arial"}, {"system-ui", "segoe ui"},
        {"serif", "times new roman"}, {"times", "times new roman"},
        {"monospace", "consolas"}, {"courier", "courier new"},
    };
    std::string name = family;
    auto alias = aliases.find(name);
    if (alias != aliases.end()) name = alias->second;
    auto it = files.find(name);
    if (it != files.end()) {
        out.push_back(dir + it->second.names[style]);
        out.push_back(dir + it->second.names[0]);
    }
#elif defined(__APPLE__)
    struct Family { const char* names[4]; };
    static const std::unordered_map<std::string, Family> files = {
        {"arial", {{"Arial.ttf", "Arial Bold.ttf", "Arial Italic.ttf", "Arial Bold Italic.ttf"}}},
        {"times new roman", {{"Times New Roman.ttf", "Times New Roman Bold.ttf",
                              "Times New Roman Italic.ttf", "Times New Roman Bold Italic.ttf"}}},
        {"courier new", {{"Courier New.ttf", "Courier New Bold.ttf", "Courier New Italic.ttf",
                          "Courier New Bold Italic.ttf"}}},
        {"verdana", {{"Verdana.ttf", "Verdana Bold.ttf", "Verdana Italic.ttf", "Verdana Bold Italic.ttf"}}},
        {"georgia", {{"Georgia.ttf", "Georgia Bold.ttf", "Georgia Italic.ttf", "Georgia Bold Italic.ttf"}}},
    };
    static const std::unordered_map<std::string, std::string> aliases = {
        {"sans-serif", "arial"}, {"helvetica", "arial"}, {"system-ui", "arial"},
        {"serif", "times new roman"}, {"times", "times new roman"},
        {"monospace", "courier new"}, {"courier", "courier new"},
    };
    std::string name = family;
    auto alias = aliases.find(name);
    if (alias != aliases.end()) name = alias->second;
    auto it = files.find(name);
    if (it != files.end()) {
        for (const char* dir : {"/System/Library/Fonts/Supplemental/", "/Library/Fonts/"}) {
            out.push_back(std::string(dir) + it->second.names[style]);
            out.push_back(std::string(dir) + it->second.names[0]);
        }
    }
    if (family == "sans-serif" || family == "helvetica") out.push_back("/System/Library/Fonts/Helvetica.ttc");
#else
    struct Family { const char* names[4]; };
    static const std::unordered_map<std::string, Family> files = {
        {"liberation sans", {{"LiberationSans-Regular.ttf", "LiberationSans-Bold.ttf",
                              "LiberationSans-Italic.ttf", "LiberationSans-BoldItalic.ttf"}}},
        {"liberation serif", {{"LiberationSerif-Regular.ttf", "LiberationSerif-Bold.ttf",
                               "LiberationSerif-Italic.ttf", "LiberationSerif-BoldItalic.ttf"}}},
        {"liberation mono", {{"LiberationMono-Regular.ttf", "LiberationMono-Bold.ttf",
                              "LiberationMono-Italic.ttf", "LiberationMono-BoldItalic.ttf"}}},
        {"dejavu sans", {{"DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSans-Oblique.ttf",
                          "DejaVuSans-BoldOblique.ttf"}}},
        {"dejavu serif", {{"DejaVuSerif.ttf", "DejaVuSerif-Bold.ttf", "DejaVuSerif-Italic.ttf",
                           "DejaVuSerif-BoldItalic.ttf"}}},
        {"dejavu sans mono", {{"DejaVuSansMono.ttf", "DejaVuSansMono-Bold.ttf",
                               "DejaVuSansMono-Oblique.ttf", "DejaVuSansMono-BoldOblique.ttf"}}},
    };
    // Metric-compatible substitutes first, then DejaVu
    static const std::unordered_map<std::string, std::vector<std::string>> aliases = {
        {"sans-serif", {"liberation sans", "dejavu sans"}}, {"arial", {"liberation sans", "dejavu sans"}},
        {"helvetica", {"liberation sans", "dejavu sans"}}, {"system-ui", {"dejavu sans", "liberation sans"}},
        {"serif", {"liberation serif", "dejavu serif"}}, {"times new roman", {"liberation serif", "dejavu serif"}},
        {"times", {"liberation serif", "dejavu serif"}},
        {"monospace", {"liberation mono", "dejavu sans mono"}}, {"courier new", {"liberation mono", "dejavu sans mono"}},
        {"courier", {"liberation mono", "dejavu sans mono"}},
    };
    std::vector<std::string> names{family};
    auto alias = aliases.find(family);
    if (alias != aliases.end()) names = alias->second;
    static const char* dirs[] = {"/usr/share/fonts/truetype/liberation/", "/usr/share/fonts/truetype/liberation2/",
                                 "/usr/share/fonts/liberation/", "/usr/share/fonts/truetype/dejavu/",
                                 "/usr/share/fonts/TTF/", "/usr/share/fonts/dejavu/"};
    for (const auto& name : names) {
        auto it = files.find(name);
        if (it == files.end()) continue;
        for (const char* dir : dirs) {
            out.push_back(std::string(dir) + it->second.names[style]);
            out.push_back(std::string(dir) + it->second.names[0]);
        }
    }
#endif
    return out;
}

// Faces are loaded once per file and kept for the life of the process
class FontCache {
public:
    static FontCache& instance() {
        static FontCache cache;
        return cache;
    }

    FT_Face face(const FontSpec& spec) {
        std::string key = (spec.bold ? "b" : "") + std::string(spec.italic ? "i" : "") + ":";
        for (const auto& f : spec.families) key += f + ",";
        auto found = bySpec_.find(key);
        if (found != bySpec_.end()) return found->second;
        FT_Face result = nullptr;
        std::vector<std::string> families = spec.families;
        families.push_back("sans-serif");
        for (const auto& family : families) {
            for (const auto& path : fontCandidates(family, spec.bold, spec.italic)) {
                if ((result = load(path))) break;
            }
            if (result) break;
        }
        bySpec_[key] = result;
        return result;
    }

private:
    FT_Library library_ = nullptr;
    std::unordered_map<std::string, FT_Face> byPath_;
    std::unordered_map<std::string, FT_Face> bySpec_;

    FT_Face load(const std::string& path) {
        auto it = byPath_.find(path);
        if (it != byPath_.end()) return it->second;
        if (!library_ && FT_Init_FreeType(&library_)) {
            library_ = nullptr;
            return nullptr;
        }
        FT_Face face = nullptr;
        if (FT_New_Face(library_, path.c_str(), 0, &face) != 0) face = nullptr;
        if (face && !FT_IS_SCALABLE(face)) {
            FT_Done_Face(face);
            face = nullptr;
        }
        byPath_[path] = face;
        return face;
    }
};

// UTF-8 to code points (invalid bytes become U+FFFD)
std::vector<uint32_t> decodeUtf8(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; len = 4; }
        else { out.push_back(0xFFFD); i++; continue; }
        if (i + len > s.size()) { out.push_back(0xFFFD); break; }
        bool ok = true;
        for (int k = 1; k < len; k++) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc >> 6) != 2) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { out.push_back(0xFFFD); i++; continue; }
        out.push_back(cp);
        i += len;
    }
    return out;
}

// Text is laid out at this many units per pixel so outlines keep detail
constexpr float kOutlineScale = 64.0f;  // 26.6 fixed point

struct LaidOutText {
    FT_Face face = nullptr;
    float size = 10;
    float width = 0;          // advance width in px
    float ascent = 0, descent = 0;  // font ascent/descent in px (descent positive)
    std::vector<std::pair<uint32_t, float>> glyphs;  // glyph index, pen x in px
};

LaidOutText layoutText(const FontSpec& spec, const std::string& text) {
    LaidOutText out;
    out.size = spec.size;
    out.face = FontCache::instance().face(spec);
    if (!out.face || spec.size <= 0) {
        out.width = text.size() * spec.size * 0.55f;
        out.ascent = spec.size * 0.8f;
        out.descent = spec.size * 0.2f;
        return out;
    }
    FT_Face face = out.face;
    float unitsPerEm = face->units_per_EM ? face->units_per_EM : 2048;
    float pxPerUnit = spec.size / unitsPerEm;
    // hhea/OS2 metrics as browsers use them
    out.ascent = face->ascender * pxPerUnit;
    out.descent = -face->descender * pxPerUnit;
    float pen = 0;
    FT_UInt previous = 0;
    bool kerning = FT_HAS_KERNING(face);
    for (uint32_t cp : decodeUtf8(text)) {
        // White space other than spaces renders as a space
        if (cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f') cp = ' ';
        FT_UInt index = FT_Get_Char_Index(face, cp);
        if (kerning && previous && index) {
            FT_Vector delta;
            if (FT_Get_Kerning(face, previous, index, FT_KERNING_UNSCALED, &delta) == 0) {
                pen += delta.x * pxPerUnit;
            }
        }
        if (FT_Load_Glyph(face, index, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) == 0) {
            out.glyphs.push_back({index, pen});
            pen += face->glyph->metrics.horiAdvance * pxPerUnit;
        }
        previous = index;
    }
    out.width = pen;
    return out;
}

struct OutlineSink {
    Canvas2D::PathBuilder* builder;
    const CanvasMatrix* matrix;
    float originX, originY, scale;  // glyph units -> user space
    float scaleX;                    // extra horizontal scale (maxWidth)

    void point(const FT_Vector* v, float& x, float& y) const {
        x = originX + v->x * scale * scaleX;
        y = originY - v->y * scale;
    }
};

int sinkMove(const FT_Vector* to, void* user) {
    auto* s = static_cast<OutlineSink*>(user);
    float x, y;
    s->point(to, x, y);
    s->builder->close();
    s->builder->moveTo(*s->matrix, x, y);
    return 0;
}

int sinkLine(const FT_Vector* to, void* user) {
    auto* s = static_cast<OutlineSink*>(user);
    float x, y;
    s->point(to, x, y);
    s->builder->lineTo(*s->matrix, x, y);
    return 0;
}

int sinkConic(const FT_Vector* control, const FT_Vector* to, void* user) {
    auto* s = static_cast<OutlineSink*>(user);
    float cx, cy, x, y;
    s->point(control, cx, cy);
    s->point(to, x, y);
    s->builder->quadTo(*s->matrix, cx, cy, x, y);
    return 0;
}

int sinkCubic(const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to, void* user) {
    auto* s = static_cast<OutlineSink*>(user);
    float ax, ay, bx, by, x, y;
    s->point(c1, ax, ay);
    s->point(c2, bx, by);
    s->point(to, x, y);
    s->builder->cubicTo(*s->matrix, ax, ay, bx, by, x, y);
    return 0;
}

} // namespace

bool CanvasRenderingContext2D::setFont(const std::string& font) {
    FontSpec spec;
    if (!parseFont(font, spec)) return false;
    state_.font = trim(font);
    return true;
}

void CanvasRenderingContext2D::fillText(const std::string& text, float x, float y, float maxWidth) {
    drawText(text, x, y, maxWidth, false);
}

void CanvasRenderingContext2D::strokeText(const std::string& text, float x, float y, float maxWidth) {
    drawText(text, x, y, maxWidth, true);
}

CanvasTextMetrics CanvasRenderingContext2D::measureText(const std::string& text) {
    FontSpec spec;
    parseFont(state_.font, spec);
    LaidOutText laid = layoutText(spec, text);
    CanvasTextMetrics m;
    m.width = laid.width;
    m.actualBoundingBoxLeft = 0;
    m.actualBoundingBoxRight = laid.width;
    m.fontBoundingBoxAscent = laid.ascent;
    m.fontBoundingBoxDescent = laid.descent;
    m.actualBoundingBoxAscent = laid.ascent;
    m.actualBoundingBoxDescent = laid.descent;
    float total = laid.ascent + laid.descent;
    m.emHeightAscent = total > 0 ? laid.size * laid.ascent / total : laid.size * 0.8f;
    m.emHeightDescent = laid.size - m.emHeightAscent;
    m.hangingBaseline = m.emHeightAscent * 0.8f;
    m.alphabeticBaseline = 0;
    m.ideographicBaseline = -m.emHeightDescent;
    return m;
}

void CanvasRenderingContext2D::drawText(const std::string& text, float x, float y,
                                        float maxWidth, bool stroke) {
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    if (maxWidth == 0) return;  // negative: no maximum
    FontSpec spec;
    parseFont(state_.font, spec);
    LaidOutText laid = layoutText(spec, text);
    if (!laid.face || laid.glyphs.empty()) return;

    float scaleX = 1.0f;
    if (maxWidth > 0 && laid.width > maxWidth) scaleX = maxWidth / laid.width;
    float width = laid.width * scaleX;

    bool rtl = state_.direction == "rtl";
    switch (state_.textAlign) {
        case TextAlign2D::Left: break;
        case TextAlign2D::Right: x -= width; break;
        case TextAlign2D::Center: x -= width / 2; break;
        case TextAlign2D::Start: if (rtl) x -= width; break;
        case TextAlign2D::End: if (!rtl) x -= width; break;
    }
    // Baselines relative to the em box, as Chrome places them
    float total = laid.ascent + laid.descent;
    float emAscent = total > 0 ? laid.size * laid.ascent / total : laid.size * 0.8f;
    float emDescent = laid.size - emAscent;
    switch (state_.textBaseline) {
        case TextBaseline::Alphabetic: break;
        case TextBaseline::Top: y += emAscent; break;
        case TextBaseline::Hanging: y += emAscent * 0.8f; break;
        case TextBaseline::Middle: y += (emAscent - emDescent) / 2; break;
        case TextBaseline::Ideographic: case TextBaseline::Bottom: y -= emDescent; break;
    }

    FT_Face face = laid.face;
    float scale = laid.size / (face->units_per_EM ? face->units_per_EM : 2048);
    Canvas2D::FlatPath path;
    Canvas2D::PathBuilder builder(path);
    OutlineSink sink{&builder, &state_.transform, 0, y, scale, scaleX};
    FT_Outline_Funcs funcs{};
    funcs.move_to = sinkMove;
    funcs.line_to = sinkLine;
    funcs.conic_to = sinkConic;
    funcs.cubic_to = sinkCubic;
    for (const auto& [index, penX] : laid.glyphs) {
        if (FT_Load_Glyph(face, index, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) continue;
        if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) continue;
        sink.originX = x + penX * scaleX;
        FT_Outline_Decompose(&face->glyph->outline, &funcs, &sink);
        builder.close();
    }
    if (stroke) strokePath(path);
    else paintPath(path, FillRule::NonZero, state_.fillStyle);
}

} // namespace NXRender
