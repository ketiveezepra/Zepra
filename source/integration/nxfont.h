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
        
        for (size_t i = 0; i < text.size();) {
            uint32_t c = nextCodepoint(text, i);
            if (c == ' ') { x += spaceAdvance() + letterSpacing + wordSpacing; continue; }
            const Glyph* found = glyphFor(c);
            if (!found) continue;
            const Glyph& glyph = *found;
            
            if (glyph.textureId && glyph.width > 0 && glyph.height > 0) {
                // Calculate position
                // x is pen position, bearingX is horizontal offset
                // y is baseline, bearingY is distance from baseline to top of glyph
                float xpos = x + glyph.bearingX;
                float ypos = y - glyph.bearingY;  // Baseline - bearingY = top of glyph
                float w = (float)glyph.width;
                float h = (float)glyph.height;
                
                glBindTexture(GL_TEXTURE_2D, glyph.textureId);
                glColor4ub(red, green, blue, 255);
                
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
        }
        
        // Restore OpenGL state
        glBindTexture(GL_TEXTURE_2D, 0);
        if (!texEnabled) glDisable(GL_TEXTURE_2D);
        if (!blendEnabled) glDisable(GL_BLEND);
        
        return x - startX;
    }
    
    // Measure text width
    float measureText(const std::string& text, float letterSpacing = 0, float wordSpacing = 0) {
        float width = 0;
        for (size_t i = 0; i < text.size();) {
            uint32_t c = nextCodepoint(text, i);
            if (c == ' ') { width += spaceAdvance() + letterSpacing + wordSpacing; continue; }
            if (const Glyph* glyph = glyphFor(c)) width += glyph->advance + letterSpacing;
        }
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
    const Glyph* glyphFor(uint32_t c) {
        if (c < 32 || c == 0x7F || (c >= 0x80 && c < 0xA0)) return nullptr;
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

    void cacheGlyph(uint32_t c) {
        if (!face_) return;
        
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
