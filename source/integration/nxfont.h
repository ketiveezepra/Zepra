// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
// NxFont - Custom Font Renderer for Zepra Browser / NeolyxOS
// No SDL dependency - Uses FreeType directly + OpenGL
// 
// FIXED: Proper glyph texture rendering with alpha blending

#pragma once

#include <string>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <cmath>
#include <cstdint>
#include <memory>
#include <iostream>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H
#include FT_OUTLINE_H

#include "rendering/font_manager.hpp"

#ifdef __APPLE__
#  ifndef GL_SILENCE_DEPRECATION
#    define GL_SILENCE_DEPRECATION
#  endif
#  include <OpenGL/gl.h>
#else
#  include <GL/gl.h>
#endif

namespace nxfont {

// Glyph info cached for rendering
struct Glyph {
    GLuint textureId = 0;
    int width = 0;
    int height = 0;
    int bearingX = 0;
    int bearingY = 0;
    int advance = 0;  // In pixels (already converted from 26.6)
    bool color = false;  // a colour bitmap (emoji): drawn as is, not tinted
    float scale = 1;     // bitmap pixels to screen pixels (emoji strikes are big)
};

// Font class - loads TTF and renders to OpenGL textures
class Font {
public:
    Font() : ft_(nullptr), face_(nullptr), size_(16) {}
    ~Font() { cleanup(); }

    /// `syntheticBold` / `syntheticItalic` draw the face emboldened or slanted,
    /// for a weight or style the family has no face for
    bool load(const std::string& path, int size = 16, bool syntheticBold = false, bool syntheticItalic = false) {
        if (FT_Init_FreeType(&ft_)) {
            std::cerr << "[NxFont] Failed to init FreeType\n";
            return false;
        }
        
        if (FT_New_Face(ft_, path.c_str(), 0, &face_)) {
            std::cerr << "[NxFont] Failed to load font: " << path << "\n";
            FT_Done_FreeType(ft_);
            ft_ = nullptr;
            return false;
        }
        return finishLoad(path, size, syntheticBold, syntheticItalic);
    }

    /// A font from bytes in memory (a web font); `data` must outlive the font
    bool loadMemory(std::shared_ptr<const std::vector<uint8_t>> data, const std::string& name, int size,
                    bool syntheticBold = false, bool syntheticItalic = false) {
        if (!data || data->empty() || FT_Init_FreeType(&ft_)) return false;
        if (FT_New_Memory_Face(ft_, data->data(), static_cast<FT_Long>(data->size()), 0, &face_)) {
            FT_Done_FreeType(ft_);
            ft_ = nullptr;
            return false;
        }
        memory_ = std::move(data);
        return finishLoad(name, size, syntheticBold, syntheticItalic);
    }

private:
    bool finishLoad(const std::string& path, int size, bool syntheticBold, bool syntheticItalic) {
        FT_Set_Pixel_Sizes(face_, 0, size);
        size_ = size;
        path_ = path;
        syntheticBold_ = syntheticBold;
        syntheticItalic_ = syntheticItalic;
        // The face's own space width (a monospace font needs it)
        if (FT_Load_Char(face_, ' ', FT_LOAD_DEFAULT) == 0) {
            spaceAdvance_ = (face_->glyph->advance.x >> 6) + (syntheticBold_ ? emboldenPx() : 0);
        }
        
        // Pre-cache ASCII characters
        for (unsigned char c = 32; c < 127; c++) {
            cacheGlyph(c);
        }
        
        std::cout << "[NxFont] Loaded: " << path << " @ " << size << "px (" 
                  << glyphs_.size() << " glyphs)\n";
        return true;
    }

public:
    
    void cleanup() {
        for (auto& pair : glyphs_) {
            if (pair.second.textureId) {
                glDeleteTextures(1, &pair.second.textureId);
            }
        }
        glyphs_.clear();
        
        for (FT_Face face : fallbacks_) FT_Done_Face(face);
        fallbacks_.clear();
        fallbacksLoaded_ = false;
        if (emojiFace_) { FT_Done_Face(emojiFace_); emojiFace_ = nullptr; }
        emojiLoaded_ = false;
        if (face_) { FT_Done_Face(face_); face_ = nullptr; }
        if (ft_) { FT_Done_FreeType(ft_); ft_ = nullptr; }
        memory_.reset();
    }
    
    // Draw text at position with color
    // Returns width of rendered text
    /// `letterSpacing` is added after every character, `wordSpacing` after
    /// every space (CSS Text 3 §8)
    float drawText(const std::string& text, float x, float y, uint8_t red, uint8_t green, uint8_t blue,
                   float letterSpacing = 0, float wordSpacing = 0) {
        if (!face_) return 0;
        
        // Save OpenGL state
        GLboolean texEnabled, blendEnabled;
        glGetBooleanv(GL_TEXTURE_2D, &texEnabled);
        glGetBooleanv(GL_BLEND, &blendEnabled);
        
        glEnable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        
        float startX = x;
        
        eachGlyph(text, [&](const Glyph* found) {
            if (!found) { x += spaceAdvance() + letterSpacing + wordSpacing; return; }
            const Glyph& glyph = *found;
            
            if (glyph.textureId && glyph.width > 0 && glyph.height > 0) {
                // Calculate position
                // x is pen position, bearingX is horizontal offset
                // y is baseline, bearingY is distance from baseline to top of glyph
                float xpos = x + glyph.bearingX * glyph.scale;
                float ypos = y - glyph.bearingY * glyph.scale;  // Baseline - bearingY = top of glyph
                float w = (float)glyph.width * glyph.scale;
                float h = (float)glyph.height * glyph.scale;
                
                glBindTexture(GL_TEXTURE_2D, glyph.textureId);
                if (glyph.color) glColor4ub(255, 255, 255, 255);
                else glColor4ub(red, green, blue, 255);
                
                // Render textured quad
                glBegin(GL_QUADS);
                // Top-left
                glTexCoord2f(0.0f, 0.0f);
                glVertex2f(xpos, ypos);
                // Top-right
                glTexCoord2f(1.0f, 0.0f);
                glVertex2f(xpos + w, ypos);
                // Bottom-right
                glTexCoord2f(1.0f, 1.0f);
                glVertex2f(xpos + w, ypos + h);
                // Bottom-left
                glTexCoord2f(0.0f, 1.0f);
                glVertex2f(xpos, ypos + h);
                glEnd();
            }
            
            x += glyph.advance + letterSpacing;
        });
        
        // Restore OpenGL state
        glBindTexture(GL_TEXTURE_2D, 0);
        if (!texEnabled) glDisable(GL_TEXTURE_2D);
        if (!blendEnabled) glDisable(GL_BLEND);
        
        return x - startX;
    }
    
    /// Calls `f(glyph)` for each glyph of `text` in order (nullptr for a space),
    /// emoji sequences shaped into their ligatures
    template <typename F>
    void eachGlyph(const std::string& text, F f) {
        for (size_t i = 0; i < text.size();) {
            uint32_t c = nextCodepoint(text, i);
            if (c == ' ') { f(nullptr); continue; }
            if (wantsEmoji(c, text, i) && emojiFace()) {
                size_t j = i;
                std::vector<const Glyph*> seq = emojiSequence(c, text, j);
                if (!seq.empty()) {
                    i = j;
                    for (const Glyph* g : seq) if (g) f(g);
                    continue;
                }
            }
            if (const Glyph* g = glyphFor(c)) f(g);
        }
    }

    // Measure text width, with the same spacing as drawText
    float measureText(const std::string& text, float letterSpacing = 0, float wordSpacing = 0) {
        float width = 0;
        eachGlyph(text, [&](const Glyph* glyph) {
            width += glyph ? glyph->advance + letterSpacing : spaceAdvance() + letterSpacing + wordSpacing;
        });
        return width;
    }
    
    int getLineHeight() const { return (int)(size_ * 1.2f); }
    const std::string& path() const { return path_; }
    int getSize() const { return size_; }

private:
    /// The next character of UTF-8 `text` at `i` (advanced past it); a
    /// broken sequence gives U+FFFD and moves one byte
    static uint32_t nextCodepoint(const std::string& text, size_t& i) {
        unsigned char lead = static_cast<unsigned char>(text[i]);
        int extra = lead < 0x80 ? 0 : (lead >> 5) == 0x6 ? 1 : (lead >> 4) == 0xE ? 2 : (lead >> 3) == 0x1E ? 3 : -1;
        if (extra == 0) {
            i++;
            return lead;
        }
        if (extra < 0 || i + extra >= text.size()) {
            i++;
            return 0xFFFD;
        }
        uint32_t c = lead & (0x3F >> extra);
        for (int k = 1; k <= extra; k++) {
            unsigned char b = static_cast<unsigned char>(text[i + k]);
            if ((b & 0xC0) != 0x80) {
                i++;
                return 0xFFFD;
            }
            c = (c << 6) | (b & 0x3F);
        }
        i += extra + 1;
        return c;
    }

    float spaceAdvance() const { return spaceAdvance_ > 0 ? spaceAdvance_ : size_ * 0.3f; }
    /// How far synthetic bold thickens strokes, in pixels
    int emboldenPx() const { return std::max(1, size_ / 24); }

    /// The glyph for a character: from this font, else from the first
    /// fallback font that has it, else the font's missing-glyph box. Null
    /// for control characters, which draw nothing.
    const Glyph* glyphFor(uint32_t key) {
        uint32_t c = key & ~kEmojiVariant;
        if (c < 32 || c == 0x7F || (c >= 0x80 && c < 0xA0)) return nullptr;
        // Variation selectors and the zero-width joiner shape emoji; alone
        // they draw nothing
        if ((c >= 0xFE00 && c <= 0xFE0F) || c == 0x200D || (c >= 0xE0020 && c <= 0xE007F)) return nullptr;
        c = key;
        auto it = glyphs_.find(c);
        if (it == glyphs_.end()) {
            cacheGlyph(c);
            it = glyphs_.find(c);
            if (it == glyphs_.end()) return nullptr;
        }
        return &it->second;
    }

    /// Fonts tried for characters this one lacks (symbols, scripts it does
    /// not cover), opened at this size the first time one is needed
    FT_Face faceWith(uint32_t c) {
        if (FT_Get_Char_Index(face_, c)) return face_;
        if (!fallbacksLoaded_) {
            fallbacksLoaded_ = true;
            static const char* const kFallbacks[] = {
#ifdef _WIN32
                "C:\\Windows\\Fonts\\seguisym.ttf",
                "C:\\Windows\\Fonts\\arialuni.ttf",
                "C:\\Windows\\Fonts\\msyh.ttc",
                "C:\\Windows\\Fonts\\meiryo.ttc",
                "C:\\Windows\\Fonts\\malgun.ttf",
                "C:\\Windows\\Fonts\\Nirmala.ttf",
#elif defined(__APPLE__)
                "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
                "/System/Library/Fonts/Apple Symbols.ttf",
                "/System/Library/Fonts/PingFang.ttc",
                "/System/Library/Fonts/Hiragino Sans GB.ttc",
                "/System/Library/Fonts/AppleSDGothicNeo.ttc",
#else
                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                "/usr/share/fonts/TTF/DejaVuSans.ttf",
                "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                "/usr/share/fonts/truetype/noto/NotoSansSymbols-Regular.ttf",
                "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf",
                "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
                "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
                "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
                "/usr/share/fonts/wenquanyi/wqy-zenhei/wqy-zenhei.ttc",
                "/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf",
                "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf",
                "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
                "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
                "/usr/share/fonts/truetype/unifont/unifont.ttf",
                "/usr/share/fonts/opentype/unifont/unifont.otf",
#endif
                nullptr
            };
            for (int i = 0; kFallbacks[i]; i++) {
                if (path_ == kFallbacks[i]) continue;
                FT_Face face = nullptr;
                if (FT_New_Face(ft_, kFallbacks[i], 0, &face) == 0) {
                    FT_Set_Pixel_Sizes(face, 0, size_);
                    fallbacks_.push_back(face);
                }
            }
        }
        for (FT_Face face : fallbacks_) {
            if (FT_Get_Char_Index(face, c)) return face;
        }
        return nullptr;
    }

    /// Characters drawn from the colour emoji font when there is one: those
    /// whose default presentation is emoji (Unicode TR51, Emoji_Presentation),
    /// the pictographs of U+1F000-1FAFF and these in the BMP
    /// Glyph-cache key bit for a character drawn in emoji presentation
    static constexpr uint32_t kEmojiVariant = 0x80000000u;

    static bool isEmoji(uint32_t c) {
        if (c >= 0x1F000 && c <= 0x1FAFF) return true;
        if (c < 0x231A || c > 0x2B55) return false;
        static const uint32_t kRanges[][2] = {
            {0x231A, 0x231B}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE},
            {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1},
            {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4},
            {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD},
            {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E},
            {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
            {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55},
        };
        for (const auto& r : kRanges)
            if (c >= r[0] && c <= r[1]) return true;
        return false;
    }

    /// The character at `i` asks for emoji presentation: one that has it by
    /// default, or any followed by U+FE0F (VARIATION SELECTOR-16, as in ❤️)
    static bool wantsEmoji(uint32_t c, const std::string& text, size_t next) {
        if (isEmoji(c)) return true;
        return next + 3 <= text.size() && (unsigned char)text[next] == 0xEF &&
               (unsigned char)text[next + 1] == 0xB8 && (unsigned char)text[next + 2] == 0x8F;
    }

    /// The system's colour emoji font, opened once (CBDT/sbix bitmap
    /// strikes, or COLR layers that FreeType renders in colour)
    FT_Face emojiFace() {
        if (emojiLoaded_) return emojiFace_;
        emojiLoaded_ = true;
        static const char* const kEmojiFonts[] = {
#ifdef _WIN32
            "C:\\Windows\\Fonts\\seguiemj.ttf",
#elif defined(__APPLE__)
            "/System/Library/Fonts/Apple Color Emoji.ttc",
#else
            "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
            "/usr/share/fonts/noto/NotoColorEmoji.ttf",
            "/usr/share/fonts/google-noto-emoji/NotoColorEmoji.ttf",
            "/usr/share/fonts/noto-emoji/NotoColorEmoji.ttf",
            "/usr/share/fonts/TTF/NotoColorEmoji.ttf",
            "/usr/share/fonts/truetype/twemoji/TwitterColorEmoji-SVGinOT.ttf",
#endif
            nullptr
        };
        for (int i = 0; kEmojiFonts[i]; i++) {
            FT_Face face = nullptr;
            if (FT_New_Face(ft_, kEmojiFonts[i], 0, &face) != 0) continue;
            if (FT_HAS_FIXED_SIZES(face) && !FT_IS_SCALABLE(face)) {
                // Bitmap strikes: the one closest above the text size
                int best = 0;
                for (int k = 0; k < face->num_fixed_sizes; k++) {
                    int h = face->available_sizes[k].height, bh = face->available_sizes[best].height;
                    if ((h >= size_ && (bh < size_ || h < bh)) || (bh < size_ && h > bh)) best = k;
                }
                FT_Select_Size(face, best);
            } else {
                FT_Set_Pixel_Sizes(face, 0, size_);
            }
            emojiFace_ = face;
            break;
        }
        return emojiFace_;
    }

    /// A colour emoji glyph: its BGRA (premultiplied) bitmap as a straight
    /// RGBA texture, scaled to the text size. False when there is none.
    bool cacheColorGlyph(uint32_t c) {
        FT_Face face = emojiFace();
        FT_UInt index = face ? FT_Get_Char_Index(face, c) : 0;
        if (!index) return false;
        return cacheColorGlyphIndex(index, c | kEmojiVariant);
    }

    /// The emoji font's glyph `index` as a colour glyph, cached under `key`
    bool cacheColorGlyphIndex(FT_UInt index, uint32_t key) {
        FT_Face face = emojiFace();
        if (!face || FT_Load_Glyph(face, index, FT_LOAD_COLOR | FT_LOAD_RENDER)) return false;
        FT_GlyphSlot slot = face->glyph;
        FT_Bitmap& bitmap = slot->bitmap;
        if (bitmap.pixel_mode != FT_PIXEL_MODE_BGRA || !bitmap.buffer || bitmap.width == 0) return false;
        Glyph glyph;
        glyph.color = true;
        glyph.width = (int)bitmap.width;
        glyph.height = (int)bitmap.rows;
        glyph.bearingX = slot->bitmap_left;
        glyph.bearingY = slot->bitmap_top;
        // A strike drawn at about the text's em, as other browsers size emoji
        float strike = face->size->metrics.y_ppem > 0 ? (float)face->size->metrics.y_ppem : (float)glyph.height;
        glyph.scale = std::min(1.0f, (float)size_ * 1.15f / strike);
        glyph.advance = (int)std::lround((slot->advance.x >> 6) * glyph.scale);
        std::vector<uint8_t> rgba((size_t)glyph.width * glyph.height * 4);
        for (int row = 0; row < glyph.height; row++) {
            const uint8_t* src = bitmap.buffer + (size_t)row * bitmap.pitch;
            for (int col = 0; col < glyph.width; col++) {
                uint8_t b = src[col * 4], g = src[col * 4 + 1], r = src[col * 4 + 2], a = src[col * 4 + 3];
                uint8_t* d = &rgba[((size_t)row * glyph.width + col) * 4];
                d[0] = a ? (uint8_t)std::min(255, r * 255 / a) : 0;
                d[1] = a ? (uint8_t)std::min(255, g * 255 / a) : 0;
                d[2] = a ? (uint8_t)std::min(255, b * 255 / a) : 0;
                d[3] = a;
            }
        }
        glGenTextures(1, &glyph.textureId);
        glBindTexture(GL_TEXTURE_2D, glyph.textureId);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, glyph.width, glyph.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
        glyphs_[key] = glyph;
        return true;
    }

    /// Cache key for a glyph of the emoji font by index (after ligatures)
    static constexpr uint32_t kEmojiGlyphIndex = 0x40000000u;

    const Glyph* emojiGlyphByIndex(FT_UInt index) {
        uint32_t key = kEmojiGlyphIndex | index;
        auto it = glyphs_.find(key);
        if (it == glyphs_.end()) {
            if (!cacheColorGlyphIndex(index, key)) glyphs_[key] = Glyph();  // remember the miss
            it = glyphs_.find(key);
        }
        return &it->second;
    }

    // -------------------------------------------------------------------------
    // Emoji sequences: skin tones, flags, ZWJ families and keycaps are
    // ligatures in the emoji font (OpenType GSUB lookup type 4, possibly in
    // type 7 extensions). They are read once and applied to each sequence.
    // -------------------------------------------------------------------------

    struct Ligature {
        std::vector<uint16_t> rest;  // the components after the first
        uint16_t glyph = 0;
    };

    static uint16_t be16(const std::vector<uint8_t>& t, size_t at) {
        return at + 1 < t.size() ? (uint16_t)((t[at] << 8) | t[at + 1]) : 0;
    }
    static uint32_t be32(const std::vector<uint8_t>& t, size_t at) {
        return ((uint32_t)be16(t, at) << 16) | be16(t, at + 2);
    }

    /// The glyphs an OpenType coverage table lists, in coverage-index order
    static std::vector<uint16_t> coverageGlyphs(const std::vector<uint8_t>& t, size_t at) {
        std::vector<uint16_t> out;
        uint16_t format = be16(t, at);
        if (format == 1) {
            uint16_t n = be16(t, at + 2);
            for (uint16_t i = 0; i < n; i++) out.push_back(be16(t, at + 4 + 2 * i));
        } else if (format == 2) {
            uint16_t n = be16(t, at + 2);
            for (uint16_t i = 0; i < n; i++) {
                size_t r = at + 4 + 6 * i;
                uint16_t first = be16(t, r), last = be16(t, r + 2), startIndex = be16(t, r + 4);
                if (last < first || last - first > 0x4000) continue;
                if (out.size() < (size_t)startIndex + (last - first + 1)) out.resize((size_t)startIndex + (last - first + 1));
                for (uint32_t g = first; g <= last; g++) out[startIndex + (g - first)] = (uint16_t)g;
            }
        }
        return out;
    }

    void loadEmojiLigatures() {
        if (ligaturesLoaded_) return;
        ligaturesLoaded_ = true;
        FT_Face face = emojiFace();
        if (!face) return;
        FT_ULong length = 0;
        if (FT_Load_Sfnt_Table(face, TTAG_GSUB, 0, nullptr, &length) || length < 10) return;
        std::vector<uint8_t> t(length);
        if (FT_Load_Sfnt_Table(face, TTAG_GSUB, 0, t.data(), &length)) return;
        size_t lookupList = be16(t, 8);
        uint16_t lookups = be16(t, lookupList);
        for (uint16_t l = 0; l < lookups; l++) {
            size_t lookup = lookupList + be16(t, lookupList + 2 + 2 * l);
            uint16_t type = be16(t, lookup), subtables = be16(t, lookup + 4);
            for (uint16_t k = 0; k < subtables; k++) {
                size_t sub = lookup + be16(t, lookup + 6 + 2 * k);
                uint16_t subType = type;
                if (type == 7) {  // extension: the real subtable is further away
                    subType = be16(t, sub + 2);
                    sub += be32(t, sub + 4);
                }
                if (subType != 4 || be16(t, sub) != 1 || sub >= t.size()) continue;
                std::vector<uint16_t> firsts = coverageGlyphs(t, sub + be16(t, sub + 2));
                uint16_t sets = be16(t, sub + 4);
                for (uint16_t i = 0; i < sets && i < firsts.size(); i++) {
                    size_t set = sub + be16(t, sub + 6 + 2 * i);
                    uint16_t count = be16(t, set);
                    for (uint16_t j = 0; j < count; j++) {
                        size_t lig = set + be16(t, set + 2 + 2 * j);
                        Ligature L;
                        L.glyph = be16(t, lig);
                        uint16_t components = be16(t, lig + 2);
                        for (uint16_t c = 1; c < components; c++) L.rest.push_back(be16(t, lig + 4 + 2 * (c - 1)));
                        ligatures_[firsts[i]].push_back(std::move(L));
                    }
                }
            }
        }
    }

    /// Glyph indices for a sequence of glyphs, with the font's ligatures
    /// applied left to right, the longest match first
    std::vector<uint16_t> applyLigatures(std::vector<uint16_t> glyphs) {
        loadEmojiLigatures();
        for (size_t i = 0; i < glyphs.size();) {
            auto it = ligatures_.find(glyphs[i]);
            const Ligature* best = nullptr;
            if (it != ligatures_.end()) {
                for (const Ligature& L : it->second) {
                    if (L.rest.empty() || i + 1 + L.rest.size() > glyphs.size()) continue;
                    if (!std::equal(L.rest.begin(), L.rest.end(), glyphs.begin() + (long)i + 1)) continue;
                    if (!best || L.rest.size() > best->rest.size()) best = &L;
                }
            }
            if (!best) { i++; continue; }
            // Replace the matched run and look again here: a ligature's result
            // can start a longer one (ZWJ families are built up that way)
            glyphs.erase(glyphs.begin() + (long)i + 1, glyphs.begin() + (long)(i + 1 + best->rest.size()));
            glyphs[i] = best->glyph;
        }
        return glyphs;
    }

    static bool isRegionalIndicator(uint32_t c) { return c >= 0x1F1E6 && c <= 0x1F1FF; }
    static bool isSkinTone(uint32_t c) { return c >= 0x1F3FB && c <= 0x1F3FF; }

    /// An emoji sequence starting with `first` (its following characters
    /// read from `text` at `i`, which moves past them): its glyphs after the
    /// font's ligatures. Empty when the emoji font lacks the first character.
    std::vector<const Glyph*> emojiSequence(uint32_t first, const std::string& text, size_t& i) {
        FT_Face face = emojiFace();
        std::vector<uint16_t> gids;
        auto add = [&](uint32_t c) {
            if (FT_UInt g = FT_Get_Char_Index(face, c)) gids.push_back((uint16_t)g);
        };
        add(first);
        if (gids.empty()) return {};
        bool flag = isRegionalIndicator(first);
        size_t j = i;
        while (j < text.size()) {
            size_t k = j;
            uint32_t c = nextCodepoint(text, k);
            bool part = false;
            if (flag) {
                part = isRegionalIndicator(c);
                flag = false;
            } else if (c == 0xFE0F || c == 0x20E3 || isSkinTone(c) || (c >= 0xE0020 && c <= 0xE007F)) {
                part = true;
            } else if (c == 0x200D && k < text.size()) {
                // A joiner and the character after it
                add(c);
                j = k;
                c = nextCodepoint(text, k);
                part = true;
            }
            if (!part) break;
            if (c != 0xFE0F) add(c);
            j = k;
        }
        i = j;
        std::vector<const Glyph*> out;
        for (uint16_t g : applyLigatures(gids)) out.push_back(emojiGlyphByIndex(g));
        return out;
    }

    void cacheGlyph(uint32_t c) {
        if (!face_) return;
        if (c & kEmojiVariant) {
            // A colour glyph, else the plain one
            if (cacheColorGlyph(c & ~kEmojiVariant)) return;
            const Glyph* plain = glyphFor(c & ~kEmojiVariant);
            if (plain) glyphs_[c] = *plain;
            return;
        }
        
        // Load glyph with rendering; a character no font has shows the
        // missing-glyph box rather than vanishing
        FT_Face face = faceWith(c);
        bool synthetic = syntheticBold_ || syntheticItalic_;
        FT_Int32 flags = synthetic ? (FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP) : FT_LOAD_RENDER;
        FT_Error error = face ? FT_Load_Char(face, c, flags)
                              : FT_Load_Glyph(face = face_, 0, flags);
        if (error) return;
        
        FT_GlyphSlot slot = face->glyph;
        if (synthetic && slot->format == FT_GLYPH_FORMAT_OUTLINE) {
            if (syntheticItalic_) {
                FT_Matrix shear = {0x10000, 0x0366A, 0, 0x10000};  // about 12 degrees
                FT_Outline_Transform(&slot->outline, &shear);
            }
            if (syntheticBold_) {
                FT_Pos strength = emboldenPx() * 64;
                FT_Outline_Embolden(&slot->outline, strength);
                slot->advance.x += strength;
            }
        }
        if (slot->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL)) return;
        FT_Bitmap& bitmap = slot->bitmap;
        
        Glyph glyph;
        glyph.width = bitmap.width;
        glyph.height = bitmap.rows;
        glyph.bearingX = slot->bitmap_left;
        glyph.bearingY = slot->bitmap_top;
        glyph.advance = slot->advance.x >> 6;  // Convert from 26.6 fixed point
        
        if (glyph.width > 0 && glyph.height > 0 && bitmap.buffer) {
            // Create RGBA texture from grayscale bitmap
            // This ensures proper alpha blending with any background
            std::vector<uint8_t> rgbaData(glyph.width * glyph.height * 4);
            
            for (int row = 0; row < glyph.height; row++) {
                for (int col = 0; col < glyph.width; col++) {
                    int srcIdx = row * bitmap.pitch + col;
                    int dstIdx = (row * glyph.width + col) * 4;
                    uint8_t alpha = bitmap.buffer[srcIdx];
                    
                    // White text with alpha from glyph
                    rgbaData[dstIdx + 0] = 255;    // R
                    rgbaData[dstIdx + 1] = 255;    // G
                    rgbaData[dstIdx + 2] = 255;    // B
                    rgbaData[dstIdx + 3] = alpha;  // A
                }
            }
            
            // Create OpenGL texture
            glGenTextures(1, &glyph.textureId);
            glBindTexture(GL_TEXTURE_2D, glyph.textureId);
            
            // Upload RGBA texture
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 
                         glyph.width, glyph.height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, rgbaData.data());
            
            // Set texture parameters
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        
        glyphs_[c] = glyph;
    }
    
    FT_Library ft_;
    FT_Face face_;
    int size_;
    std::string path_;
    std::unordered_map<uint32_t, Glyph> glyphs_;
    std::vector<FT_Face> fallbacks_;
    bool fallbacksLoaded_ = false;
    FT_Face emojiFace_ = nullptr;
    bool emojiLoaded_ = false;
    std::unordered_map<uint16_t, std::vector<Ligature>> ligatures_;
    bool ligaturesLoaded_ = false;
    bool syntheticBold_ = false;
    bool syntheticItalic_ = false;
    float spaceAdvance_ = 0;
    std::shared_ptr<const std::vector<uint8_t>> memory_;  // a memory face's bytes
};

// FontManager - manages multiple fonts
class FontManager {
public:
    static FontManager& instance() {
        static FontManager inst;
        return inst;
    }
    
    bool loadFont(const std::string& name, const std::string& path, int size = 16) {
        auto font = std::make_unique<Font>();
        if (!font->load(path, size)) {
            return false;
        }
        fonts_[name] = std::move(font);
        if (!defaultFont_) defaultFont_ = fonts_[name].get();
        return true;
    }
    
    Font* getFont(const std::string& name) {
        auto it = fonts_.find(name);
        return (it != fonts_.end()) ? it->second.get() : nullptr;
    }
    
    Font* defaultFont() { return defaultFont_; }
    
    // Try common system fonts with specific key and size
    bool loadSystemFont(const std::string& key, int size) {
        const char* paths[] = {
#ifdef _WIN32
            "C:\\Windows\\Fonts\\segoeui.ttf",
            "C:\\Windows\\Fonts\\arial.ttf",
            "C:\\Windows\\Fonts\\tahoma.ttf",
            "C:\\Windows\\Fonts\\verdana.ttf",
            "C:\\Windows\\Fonts\\calibri.ttf",
#elif defined(__APPLE__)
            "/System/Library/Fonts/Helvetica.ttc",
            "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/Library/Fonts/Arial.ttf",
            "/System/Library/Fonts/Geneva.ttf",
#else
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
#endif
            nullptr
        };
        
        for (int i = 0; paths[i]; i++) {
            if (loadFont(key, paths[i], size)) {
                return true;
            }
        }
        // Be quiet on failure as we might try multiple paths
        return false;
    }

    // Legacy compatibility (loads to "default")
    bool loadSystemFont(int size = 14) {
        bool ok = loadSystemFont("default", size);
        if (!ok) std::cerr << "[NxFont] No system font found!\n";
        return ok;
    }
    
    /// The font for CSS text: `families` is the font-family list, matched
    /// against the installed fonts; a weight of 600 or more is bold. A family
    /// with no bold or italic face is drawn emboldened or slanted. Text whose
    /// families aren't installed uses the system font (in the wanted style).
    Font* getFontFor(const std::string& families, int weight, bool italic, int size) {
        // The page's @font-face fonts changed: what a family maps to may have too
        uint64_t generation = Zepra::Fonts::FontManager::instance().webFontGeneration();
        if (generation != webFontGeneration_) {
            styled_.clear();
            webFontGeneration_ = generation;
        }
        std::string key = families + '\x1f' + std::to_string(weight) + (italic ? "i" : "n") + std::to_string(size);
        auto it = styled_.find(key);
        if (it != styled_.end()) return it->second ? it->second.get() : getSystemFont(size);

        bool bold = weight >= 600;
        const auto* reg = Zepra::Fonts::FontManager::instance().match(families, weight, italic);
        std::unique_ptr<Font> font;
        if (reg) {
            bool regBold = reg->weight >= Zepra::Fonts::FontWeight::SemiBold;
            font = std::make_unique<Font>();
            bool ok = reg->data.empty()
                ? font->load(reg->path, size, bold && !regBold, italic && !reg->italic)
                : font->loadMemory(std::make_shared<const std::vector<uint8_t>>(reg->data), reg->displayName,
                                   size, bold && !regBold, italic && !reg->italic);
            if (!ok) font.reset();
        }
        if (!font && (bold || italic)) {
            // The system font, emboldened or slanted
            Font* system = getSystemFont(size);
            if (system && !system->path().empty()) {
                font = std::make_unique<Font>();
                if (!font->load(system->path(), size, bold, italic)) font.reset();
            }
        }
        Font* result = font.get();
        styled_[key] = std::move(font);  // null: use the system font
        return result ? result : getSystemFont(size);
    }

    // Get or load system font of specific size
    Font* getSystemFont(int size) {
        std::string key = "sys_" + std::to_string(size);
        auto it = fonts_.find(key);
        if (it != fonts_.end()) return it->second.get();
        
        if (loadSystemFont(key, size)) {
            return fonts_[key].get();
        }
        
        // Fallback to default if size load fails
        return defaultFont_;
    }
    
private:
    FontManager() = default;
    std::unordered_map<std::string, std::unique_ptr<Font>> fonts_;
    Font* defaultFont_ = nullptr;
    std::unordered_map<std::string, std::unique_ptr<Font>> styled_;  // getFontFor()
    uint64_t webFontGeneration_ = 0;
};

} // namespace nxfont
