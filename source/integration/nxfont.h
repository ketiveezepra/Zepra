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

#include <GL/gl.h>

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

    bool load(const std::string& path, int size = 16) {
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
        
        FT_Set_Pixel_Sizes(face_, 0, size);
        size_ = size;
        path_ = path;
        
        // Pre-cache ASCII characters
        for (unsigned char c = 32; c < 127; c++) {
            cacheGlyph(c);
        }
        
        std::cout << "[NxFont] Loaded: " << path << " @ " << size << "px (" 
                  << glyphs_.size() << " glyphs)\n";
        return true;
    }
    
    void cleanup() {
        for (auto& pair : glyphs_) {
            if (pair.second.textureId) {
                glDeleteTextures(1, &pair.second.textureId);
            }
        }
        glyphs_.clear();
        
        if (face_) { FT_Done_Face(face_); face_ = nullptr; }
        if (ft_) { FT_Done_FreeType(ft_); ft_ = nullptr; }
    }
    
    // Draw text at position with color
    // Returns width of rendered text
    float drawText(const std::string& text, float x, float y, uint8_t red, uint8_t green, uint8_t blue) {
        if (!face_) return 0;
        
        // Save OpenGL state
        GLboolean texEnabled, blendEnabled;
        glGetBooleanv(GL_TEXTURE_2D, &texEnabled);
        glGetBooleanv(GL_BLEND, &blendEnabled);
        
        glEnable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        
        float startX = x;
        
        for (unsigned char c : text) {
            if (c < 32 || c > 126) {
                if (c == ' ') x += size_ * 0.3f;
                continue;
            }
            
            // Get or cache glyph
            auto it = glyphs_.find(c);
            if (it == glyphs_.end()) {
                cacheGlyph(c);
                it = glyphs_.find(c);
                if (it == glyphs_.end()) continue;
            }
            
            const Glyph& glyph = it->second;
            
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
            
            x += glyph.advance;
        }
        
        // Restore OpenGL state
        glBindTexture(GL_TEXTURE_2D, 0);
        if (!texEnabled) glDisable(GL_TEXTURE_2D);
        if (!blendEnabled) glDisable(GL_BLEND);
        
        return x - startX;
    }
    
    // Measure text width
    float measureText(const std::string& text) {
        float width = 0;
        for (unsigned char c : text) {
            if (c == ' ') {
                width += size_ * 0.3f;
                continue;
            }
            auto it = glyphs_.find(c);
            if (it != glyphs_.end()) {
                width += it->second.advance;
            }
        }
        return width;
    }
    
    int getLineHeight() const { return (int)(size_ * 1.2f); }
    int getSize() const { return size_; }

private:
    void cacheGlyph(unsigned char c) {
        if (!face_) return;
        
        // Load glyph with rendering
        if (FT_Load_Char(face_, c, FT_LOAD_RENDER)) {
            std::cerr << "[NxFont] Failed to load glyph: " << (int)c << "\n";
            return;
        }
        
        FT_GlyphSlot slot = face_->glyph;
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
    std::unordered_map<unsigned char, Glyph> glyphs_;
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
};

} // namespace nxfont
