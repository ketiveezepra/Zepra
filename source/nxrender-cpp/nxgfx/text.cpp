// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file text.cpp
 * @brief Text rendering with FreeType
 */

#include "nxgfx/text.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <unordered_map>
#include <algorithm>
#include <cstdlib>   // getenv (for WINDIR)

// Centralized OpenGL headers + constant defines
#include "nxgfx/gl_includes.h"


namespace NXRender {

// Glyph cache entry
struct GlyphInfo {
    unsigned int textureId = 0;
    int width = 0;
    int height = 0;
    int bearingX = 0;
    int bearingY = 0;
    int advance = 0;
};

struct TextRenderer::Impl {
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    std::unordered_map<uint32_t, GlyphInfo> glyphCache;  // Unicode codepoint -> glyph
    std::string currentFont;
    float currentSize = 14.0f;
    bool initialized = false;
    
    bool init() {
        if (initialized) return true;
        if (FT_Init_FreeType(&library)) return false;
        initialized = true;
        
        // Platform-specific default font search paths
        const char* defaultFonts[] = {
#ifdef _WIN32
            "C:\\Windows\\Fonts\\segoeui.ttf",   // Segoe UI (Windows 7+)
            "C:\\Windows\\Fonts\\arial.ttf",      // Arial fallback
            "C:\\Windows\\Fonts\\tahoma.ttf",     // Tahoma fallback
#elif defined(__APPLE__)
            "/System/Library/Fonts/Helvetica.ttc",
            "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/Library/Fonts/Arial.ttf",
            "/System/Library/Fonts/Geneva.ttf",
#else
            // Linux / NeolyxOS paths (unchanged)
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
#endif
            nullptr
        };
        
        for (int i = 0; defaultFonts[i]; i++) {
            if (FT_New_Face(library, defaultFonts[i], 0, &face) == 0) {
                FT_Set_Pixel_Sizes(face, 0, 14);
                return true;
            }
        }
        return true;  // Continue without font (fallback estimation)
    }
    
    void clearCache() {
        for (auto& p : glyphCache) {
            if (p.second.textureId) {
                glDeleteTextures(1, &p.second.textureId);
            }
        }
        glyphCache.clear();
    }
    
    GlyphInfo& getGlyph(uint32_t codepoint, float size) {
        uint32_t key = (codepoint << 8) | (static_cast<int>(size) & 0xFF);
        
        auto it = glyphCache.find(key);
        if (it != glyphCache.end()) {
            return it->second;
        }
        
        GlyphInfo info;
        
        if (face) {
            FT_Set_Pixel_Sizes(face, 0, static_cast<int>(size));
            
            if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER) == 0) {
                FT_GlyphSlot g = face->glyph;
                
                // Create texture for glyph
                glGenTextures(1, &info.textureId);
                glBindTexture(GL_TEXTURE_2D, info.textureId);
                
                // Use GL_ALPHA for grayscale glyph
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA,
                             g->bitmap.width, g->bitmap.rows,
                             0, GL_ALPHA, GL_UNSIGNED_BYTE, g->bitmap.buffer);
                
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                
                info.width = g->bitmap.width;
                info.height = g->bitmap.rows;
                info.bearingX = g->bitmap_left;
                info.bearingY = g->bitmap_top;
                info.advance = g->advance.x >> 6;
            }
        }
        
        glyphCache[key] = info;
        return glyphCache[key];
    }
    
    ~Impl() {
        clearCache();
        if (face) FT_Done_Face(face);
        if (library) FT_Done_FreeType(library);
    }
};

TextRenderer& TextRenderer::instance() {
    static TextRenderer instance;
    return instance;
}

TextRenderer::TextRenderer() : impl_(std::make_unique<Impl>()) {
    impl_->init();
}

TextRenderer::~TextRenderer() = default;

bool TextRenderer::loadFont(const std::string& path, const std::string& name) {
    if (!impl_->library) return false;
    
    FT_Face newFace;
    if (FT_New_Face(impl_->library, path.c_str(), 0, &newFace) != 0) {
        return false;
    }
    
    if (impl_->face) {
        FT_Done_Face(impl_->face);
    }
    impl_->face = newFace;
    impl_->currentFont = name;
    impl_->clearCache();
    return true;
}

void TextRenderer::setDefaultFont(const std::string& name) {
    impl_->currentFont = name;
}

TextMetrics TextRenderer::measure(const std::string& text, const FontStyle& style) {
    TextMetrics m;
    m.width = 0;
    m.ascent = style.size * 0.8f;
    m.descent = style.size * 0.2f;
    m.height = style.size * 1.2f;
    m.lineHeight = style.size * 1.4f;
    
    if (impl_->face) {
        FT_Set_Pixel_Sizes(impl_->face, 0, static_cast<int>(style.size));
        
        for (size_t i = 0; i < text.size(); ) {
            uint32_t c = static_cast<unsigned char>(text[i]);
            i++;
            
            // Handle UTF-8
            if (c >= 0xC0 && c < 0xE0 && i < text.size()) {
                c = ((c & 0x1F) << 6) | (text[i++] & 0x3F);
            } else if (c >= 0xE0 && c < 0xF0 && i + 1 < text.size()) {
                c = ((c & 0x0F) << 12) | ((text[i] & 0x3F) << 6) | (text[i+1] & 0x3F);
                i += 2;
            }
            
            if (FT_Load_Char(impl_->face, c, FT_LOAD_DEFAULT) == 0) {
                m.width += impl_->face->glyph->advance.x >> 6;
            } else {
                m.width += style.size * 0.6f;
            }
        }
        
        m.ascent = impl_->face->size->metrics.ascender >> 6;
        m.descent = -(impl_->face->size->metrics.descender >> 6);
        m.height = m.ascent + m.descent;
    } else {
        // Fallback estimation
        m.width = text.length() * style.size * 0.6f;
    }
    
    return m;
}

float TextRenderer::measureWidth(const std::string& text, float fontSize) {
    FontStyle style;
    style.size = fontSize;
    return measure(text, style).width;
}

void TextRenderer::render(const std::string& text, float x, float y,
                          const Color& color, const FontStyle& style) {
    if (!impl_->face) return;
    
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    glColor4ub(color.r, color.g, color.b, color.a);
    
    float penX = x;
    float penY = y;
    
    for (size_t i = 0; i < text.size(); ) {
        uint32_t c = static_cast<unsigned char>(text[i]);
        i++;
        
        // Handle UTF-8
        if (c >= 0xC0 && c < 0xE0 && i < text.size()) {
            c = ((c & 0x1F) << 6) | (text[i++] & 0x3F);
        } else if (c >= 0xE0 && c < 0xF0 && i + 1 < text.size()) {
            c = ((c & 0x0F) << 12) | ((text[i] & 0x3F) << 6) | (text[i+1] & 0x3F);
            i += 2;
        }
        
        GlyphInfo& g = impl_->getGlyph(c, style.size);
        
        if (g.textureId) {
            float xpos = penX + g.bearingX;
            float ypos = penY - g.bearingY;
            float w = g.width;
            float h = g.height;
            
            glBindTexture(GL_TEXTURE_2D, g.textureId);
            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex2f(xpos, ypos);
            glTexCoord2f(1, 0); glVertex2f(xpos + w, ypos);
            glTexCoord2f(1, 1); glVertex2f(xpos + w, ypos + h);
            glTexCoord2f(0, 1); glVertex2f(xpos, ypos + h);
            glEnd();
        }
        
        penX += g.advance;
    }
    
    glDisable(GL_TEXTURE_2D);
}

} // namespace NXRender
