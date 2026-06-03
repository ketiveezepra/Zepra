// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file font_face.hpp
 * @brief FreeType font face wrapper for Zepra rendering engine
 *
 * Wraps a single FreeType face (a font file + face index combination).
 * Provides glyph metrics, bitmap rasterization, and kerning.
 *
 * Usage:
 *   FontFace face;
 *   face.load("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
 *   face.setPixelSize(16);
 *   GlyphBitmap bm = face.renderGlyph('A');
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <optional>

// Only compile FreeType integration when the library is available
#ifdef ZEPRA_HAS_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H
#include FT_BITMAP_H
#endif

namespace Zepra::Fonts {

// ============================================================================
// GlyphMetrics — typographic measurements for one glyph
// ============================================================================

struct GlyphMetrics {
    int   width        = 0;  ///< Bitmap width in pixels
    int   height       = 0;  ///< Bitmap height in pixels
    int   bearingX     = 0;  ///< Horizontal distance from origin to left edge
    int   bearingY     = 0;  ///< Vertical distance from baseline to top edge
    int   advance      = 0;  ///< Horizontal advance (how far the pen moves)
    int   advanceY     = 0;  ///< Vertical advance (for vertical scripts)
    float lsb          = 0;  ///< Left side bearing (float for sub-pixel)
    bool  isEmpty      = false; ///< True for whitespace-only glyphs
};

// ============================================================================
// GlyphBitmap — rasterized glyph data
// ============================================================================

struct GlyphBitmap {
    std::vector<uint8_t> data;  ///< Alpha-channel bitmap (1 byte per pixel)
    int   width       = 0;
    int   height      = 0;
    int   pitch       = 0;     ///< Row stride in bytes (may be > width)
    GlyphMetrics metrics;
    bool  valid       = false;
};

// ============================================================================
// FontStyle flags
// ============================================================================

enum class FontStyle : uint32_t {
    Normal  = 0,
    Bold    = 1 << 0,
    Italic  = 1 << 1,
    Oblique = 1 << 2
};

// ============================================================================
// FontFace
// ============================================================================

/**
 * @brief Wraps a single FreeType face (one typeface at one size)
 *
 * Each FontFace is independent and thread-safe after construction,
 * but FreeType library initialization is global (handled by FontLibrary).
 */
class FontFace {
public:
    FontFace();
    ~FontFace();

    // Non-copyable (FreeType faces hold native resources)
    FontFace(const FontFace&) = delete;
    FontFace& operator=(const FontFace&) = delete;

    // Movable
    FontFace(FontFace&& other) noexcept;
    FontFace& operator=(FontFace&& other) noexcept;

    /**
     * @brief Load a font from a file path
     * @param path       Absolute path to .ttf / .otf / .woff file
     * @param faceIndex  Face index within the font file (usually 0)
     * @return true if loaded successfully
     */
    bool load(const std::string& path, int faceIndex = 0);

    /**
     * @brief Load a font from memory (e.g., embedded or downloaded font)
     * @param data       Raw font bytes
     * @param faceIndex  Face index within the font file (usually 0)
     * @return true if loaded successfully
     */
    bool loadFromMemory(const std::vector<uint8_t>& data, int faceIndex = 0);

    /**
     * @brief Set the pixel size for rendering
     * This sets both horizontal and vertical pixels per em.
     */
    void setPixelSize(int pixelSize);

    /**
     * @brief Set point size with DPI
     * @param pointSize  Size in typographic points
     * @param dpi        Screen DPI (72 = standard, 96 = typical desktop)
     */
    void setPointSize(float pointSize, int dpi = 96);

    /**
     * @brief Render a single Unicode codepoint to a bitmap
     * @param codepoint  Unicode codepoint (e.g., 'A' = 0x41)
     * @param hints      Apply FreeType hinting (better for small sizes)
     * @return Rasterized glyph with metrics
     */
    GlyphBitmap renderGlyph(char32_t codepoint, bool hints = true) const;

    /**
     * @brief Get metrics for a glyph without rasterizing it
     * Useful for layout calculations.
     */
    GlyphMetrics getGlyphMetrics(char32_t codepoint) const;

    /**
     * @brief Get kerning advance between two codepoints
     * @param left   Left glyph codepoint
     * @param right  Right glyph codepoint
     * @return Horizontal kerning adjustment in pixels (usually negative)
     */
    int getKerning(char32_t left, char32_t right) const;

    /**
     * @brief Check if the face has a glyph for the given codepoint
     */
    bool hasGlyph(char32_t codepoint) const;

    // ─── Face properties ──────────────────────────────────────────────────

    bool isLoaded() const { return loaded_; }
    bool isBold() const;
    bool isItalic() const;
    bool isFixedWidth() const;

    std::string familyName() const { return familyName_; }
    std::string styleName()  const { return styleName_;  }
    std::string filePath()   const { return filePath_;   }

    // ─── Metrics (all in pixels for the current size) ─────────────────────

    int ascender()   const { return ascender_;   }  ///< Distance from baseline to top of capital letters
    int descender()  const { return descender_;  }  ///< Distance from baseline to bottom of descenders (negative)
    int lineHeight() const { return lineHeight_; }  ///< Recommended line spacing
    int xHeight()    const { return xHeight_;    }  ///< Height of lowercase 'x'
    int capHeight()  const { return capHeight_;  }  ///< Height of capital letters
    int unitsPerEm() const { return unitsPerEm_; }  ///< Font design units per em
    int pixelSize()  const { return pixelSize_;  }

    /**
     * @brief Measure the width of a UTF-8 string (no wrapping)
     */
    int measureText(const std::string& text) const;

    /**
     * @brief Convert a UTF-32 codepoint to its glyph index in this face
     */
    uint32_t glyphIndex(char32_t codepoint) const;

private:
    void updateMetrics();
    void cleanup();

    std::string filePath_;
    std::string familyName_;
    std::string styleName_;

    int pixelSize_  = 16;
    int ascender_   = 0;
    int descender_  = 0;
    int lineHeight_ = 0;
    int xHeight_    = 0;
    int capHeight_  = 0;
    int unitsPerEm_ = 0;
    bool loaded_    = false;

    // Font file bytes (kept alive for memory-loaded fonts)
    std::vector<uint8_t> fontData_;

#ifdef ZEPRA_HAS_FREETYPE
    FT_Face ftFace_ = nullptr;
#else
    void* ftFace_ = nullptr; // placeholder when FreeType unavailable
#endif
};

// ============================================================================
// FontLibrary — singleton FreeType library handle
// ============================================================================

/**
 * @brief Manages the global FreeType library instance
 *
 * Must be initialized before any FontFace is created.
 * Thread-safe after initialization.
 */
class FontLibrary {
public:
    /**
     * @brief Get the global FreeType library singleton
     */
    static FontLibrary& instance();

    /**
     * @brief Initialize the FreeType library
     * @return true if initialized successfully (or already initialized)
     */
    bool initialize();

    /**
     * @brief Check if the library is ready
     */
    bool isInitialized() const { return initialized_; }

#ifdef ZEPRA_HAS_FREETYPE
    FT_Library ftLibrary() const { return ftLibrary_; }
#else
    void* ftLibrary() const { return nullptr; }
#endif

    /**
     * @brief Shut down FreeType (called at program exit)
     */
    void shutdown();

private:
    FontLibrary() = default;
    ~FontLibrary() { shutdown(); }

    FontLibrary(const FontLibrary&) = delete;
    FontLibrary& operator=(const FontLibrary&) = delete;

    bool initialized_ = false;

#ifdef ZEPRA_HAS_FREETYPE
    FT_Library ftLibrary_ = nullptr;
#else
    void* ftLibrary_ = nullptr;
#endif
};

} // namespace Zepra::Fonts
