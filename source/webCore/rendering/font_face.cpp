// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file font_face.cpp
 * @brief FreeType font face implementation
 *
 * When ZEPRA_HAS_FREETYPE is defined, all FreeType calls are active.
 * Without it, every method returns safe defaults (graceful degradation).
 */

#include "rendering/font_face.hpp"
#include <cstring>
#include <stdexcept>

namespace Zepra::Fonts {

// ============================================================================
// FontLibrary
// ============================================================================

FontLibrary& FontLibrary::instance() {
    static FontLibrary inst;
    return inst;
}

bool FontLibrary::initialize() {
    if (initialized_) return true;

#ifdef ZEPRA_HAS_FREETYPE
    FT_Error error = FT_Init_FreeType(&ftLibrary_);
    if (error) {
        // FT_Error is an integer; non-zero = failure
        ftLibrary_ = nullptr;
        return false;
    }
    initialized_ = true;
    return true;
#else
    // No FreeType — still "initialized" but in stub mode
    initialized_ = true;
    return true;
#endif
}

void FontLibrary::shutdown() {
    if (!initialized_) return;
#ifdef ZEPRA_HAS_FREETYPE
    if (ftLibrary_) {
        FT_Done_FreeType(ftLibrary_);
        ftLibrary_ = nullptr;
    }
#endif
    initialized_ = false;
}

// ============================================================================
// FontFace — Constructor / Destructor / Move
// ============================================================================

FontFace::FontFace() = default;

FontFace::~FontFace() {
    cleanup();
}

FontFace::FontFace(FontFace&& other) noexcept
    : filePath_(std::move(other.filePath_))
    , familyName_(std::move(other.familyName_))
    , styleName_(std::move(other.styleName_))
    , pixelSize_(other.pixelSize_)
    , ascender_(other.ascender_)
    , descender_(other.descender_)
    , lineHeight_(other.lineHeight_)
    , xHeight_(other.xHeight_)
    , capHeight_(other.capHeight_)
    , unitsPerEm_(other.unitsPerEm_)
    , loaded_(other.loaded_)
    , fontData_(std::move(other.fontData_))
    , ftFace_(other.ftFace_)
{
    other.ftFace_ = nullptr;
    other.loaded_ = false;
}

FontFace& FontFace::operator=(FontFace&& other) noexcept {
    if (this != &other) {
        cleanup();
        filePath_    = std::move(other.filePath_);
        familyName_  = std::move(other.familyName_);
        styleName_   = std::move(other.styleName_);
        pixelSize_   = other.pixelSize_;
        ascender_    = other.ascender_;
        descender_   = other.descender_;
        lineHeight_  = other.lineHeight_;
        xHeight_     = other.xHeight_;
        capHeight_   = other.capHeight_;
        unitsPerEm_  = other.unitsPerEm_;
        loaded_      = other.loaded_;
        fontData_    = std::move(other.fontData_);
        ftFace_      = other.ftFace_;
        other.ftFace_ = nullptr;
        other.loaded_ = false;
    }
    return *this;
}

void FontFace::cleanup() {
#ifdef ZEPRA_HAS_FREETYPE
    if (ftFace_) {
        FT_Done_Face(ftFace_);
        ftFace_ = nullptr;
    }
#endif
    loaded_ = false;
}

// ============================================================================
// FontFace — Loading
// ============================================================================

bool FontFace::load(const std::string& path, int faceIndex) {
    cleanup();
    filePath_ = path;

#ifdef ZEPRA_HAS_FREETYPE
    FontLibrary& lib = FontLibrary::instance();
    if (!lib.isInitialized()) {
        if (!lib.initialize()) return false;
    }

    FT_Error err = FT_New_Face(lib.ftLibrary(), path.c_str(), faceIndex, &ftFace_);
    if (err || !ftFace_) {
        ftFace_ = nullptr;
        return false;
    }

    // Select Unicode charmap (required for codepoint lookups)
    FT_Select_Charmap(ftFace_, FT_ENCODING_UNICODE);

    familyName_ = ftFace_->family_name ? ftFace_->family_name : "";
    styleName_  = ftFace_->style_name  ? ftFace_->style_name  : "";
    unitsPerEm_ = static_cast<int>(ftFace_->units_per_EM);

    // Set default pixel size
    setPixelSize(pixelSize_);
    loaded_ = true;
    return true;
#else
    // Stub: pretend we loaded successfully
    familyName_ = "Fallback";
    styleName_  = "Regular";
    unitsPerEm_ = 2048;
    ascender_   = pixelSize_ * 3 / 4;
    descender_  = -(pixelSize_ / 4);
    lineHeight_ = pixelSize_ + 2;
    loaded_ = true;
    return true;
#endif
}

bool FontFace::loadFromMemory(const std::vector<uint8_t>& data, int faceIndex) {
    cleanup();
    fontData_ = data; // Keep alive

#ifdef ZEPRA_HAS_FREETYPE
    FontLibrary& lib = FontLibrary::instance();
    if (!lib.isInitialized()) {
        if (!lib.initialize()) return false;
    }

    FT_Error err = FT_New_Memory_Face(
        lib.ftLibrary(),
        fontData_.data(),
        static_cast<FT_Long>(fontData_.size()),
        faceIndex,
        &ftFace_
    );
    if (err || !ftFace_) {
        ftFace_ = nullptr;
        fontData_.clear();
        return false;
    }

    FT_Select_Charmap(ftFace_, FT_ENCODING_UNICODE);

    familyName_ = ftFace_->family_name ? ftFace_->family_name : "";
    styleName_  = ftFace_->style_name  ? ftFace_->style_name  : "";
    unitsPerEm_ = static_cast<int>(ftFace_->units_per_EM);

    setPixelSize(pixelSize_);
    loaded_ = true;
    return true;
#else
    familyName_ = "Memory Font";
    styleName_  = "Regular";
    unitsPerEm_ = 2048;
    ascender_   = pixelSize_ * 3 / 4;
    descender_  = -(pixelSize_ / 4);
    lineHeight_ = pixelSize_ + 2;
    loaded_ = true;
    return true;
#endif
}

// ============================================================================
// FontFace — Size
// ============================================================================

void FontFace::setPixelSize(int pixelSize) {
    pixelSize_ = pixelSize;
    if (!loaded_ && !ftFace_) return;

#ifdef ZEPRA_HAS_FREETYPE
    if (ftFace_) {
        FT_Set_Pixel_Sizes(ftFace_, 0, static_cast<FT_UInt>(pixelSize));
        updateMetrics();
    }
#else
    ascender_   = pixelSize * 3 / 4;
    descender_  = -(pixelSize / 4);
    lineHeight_ = pixelSize + 2;
    xHeight_    = pixelSize / 2;
    capHeight_  = pixelSize * 2 / 3;
#endif
}

void FontFace::setPointSize(float pointSize, int dpi) {
    // Convert points to pixels: px = pt * dpi / 72
    int pixels = static_cast<int>(pointSize * dpi / 72.0f + 0.5f);
    setPixelSize(pixels);
}

void FontFace::updateMetrics() {
#ifdef ZEPRA_HAS_FREETYPE
    if (!ftFace_) return;

    // FreeType gives metrics in 26.6 fixed point (1/64 pixel units)
    auto scaledMetrics = ftFace_->size->metrics;

    ascender_   = static_cast<int>(scaledMetrics.ascender  >> 6);
    descender_  = static_cast<int>(scaledMetrics.descender >> 6);
    lineHeight_ = static_cast<int>(scaledMetrics.height    >> 6);

    // xHeight: height of lowercase 'x'
    uint32_t xIdx = FT_Get_Char_Index(ftFace_, 'x');
    if (xIdx && FT_Load_Glyph(ftFace_, xIdx, FT_LOAD_NO_BITMAP) == 0) {
        xHeight_ = static_cast<int>(ftFace_->glyph->metrics.height >> 6);
    } else {
        xHeight_ = ascender_ / 2;
    }

    // capHeight: height of 'H'
    uint32_t hIdx = FT_Get_Char_Index(ftFace_, 'H');
    if (hIdx && FT_Load_Glyph(ftFace_, hIdx, FT_LOAD_NO_BITMAP) == 0) {
        capHeight_ = static_cast<int>(ftFace_->glyph->metrics.height >> 6);
    } else {
        capHeight_ = ascender_ * 2 / 3;
    }
#endif
}

// ============================================================================
// FontFace — Glyph rendering
// ============================================================================

uint32_t FontFace::glyphIndex(char32_t codepoint) const {
#ifdef ZEPRA_HAS_FREETYPE
    if (!ftFace_) return 0;
    return FT_Get_Char_Index(ftFace_, static_cast<FT_ULong>(codepoint));
#else
    return static_cast<uint32_t>(codepoint);
#endif
}

bool FontFace::hasGlyph(char32_t codepoint) const {
#ifdef ZEPRA_HAS_FREETYPE
    if (!ftFace_) return false;
    return FT_Get_Char_Index(ftFace_, static_cast<FT_ULong>(codepoint)) != 0;
#else
    return true; // stub says yes to everything
#endif
}

GlyphMetrics FontFace::getGlyphMetrics(char32_t codepoint) const {
    GlyphMetrics m;
#ifdef ZEPRA_HAS_FREETYPE
    if (!ftFace_) return m;

    uint32_t idx = FT_Get_Char_Index(ftFace_, static_cast<FT_ULong>(codepoint));
    if (!idx) return m;

    int flags = FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP;
    if (FT_Load_Glyph(ftFace_, idx, flags) != 0) return m;

    auto& gm = ftFace_->glyph->metrics;
    m.width    = static_cast<int>(gm.width   >> 6);
    m.height   = static_cast<int>(gm.height  >> 6);
    m.bearingX = static_cast<int>(gm.horiBearingX >> 6);
    m.bearingY = static_cast<int>(gm.horiBearingY >> 6);
    m.advance  = static_cast<int>(gm.horiAdvance  >> 6);
    m.advanceY = static_cast<int>(gm.vertAdvance  >> 6);
    m.isEmpty  = (m.width == 0 || m.height == 0);
#else
    // Stub metrics
    m.width    = pixelSize_ / 2;
    m.height   = pixelSize_;
    m.bearingX = 0;
    m.bearingY = pixelSize_ - 2;
    m.advance  = pixelSize_ / 2 + 1;
    m.isEmpty  = false;
#endif
    return m;
}

GlyphBitmap FontFace::renderGlyph(char32_t codepoint, bool hints) const {
    GlyphBitmap result;
#ifdef ZEPRA_HAS_FREETYPE
    if (!ftFace_) return result;

    uint32_t idx = FT_Get_Char_Index(ftFace_, static_cast<FT_ULong>(codepoint));
    if (!idx) return result;

    int loadFlags = hints ? FT_LOAD_DEFAULT : FT_LOAD_NO_HINTING;
    if (FT_Load_Glyph(ftFace_, idx, loadFlags) != 0) return result;

    FT_GlyphSlot slot = ftFace_->glyph;

    // Render to anti-aliased bitmap
    if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) return result;

    FT_Bitmap& bm = slot->bitmap;

    result.width  = bm.width;
    result.height = bm.rows;
    result.pitch  = bm.pitch;

    // Copy bitmap data (FreeType's buffer may be reused on the next call)
    size_t dataSize = static_cast<size_t>(std::abs(bm.pitch)) * bm.rows;
    result.data.resize(dataSize);

    if (bm.pitch > 0) {
        // Top-to-bottom storage
        std::memcpy(result.data.data(), bm.buffer, dataSize);
    } else {
        // Bottom-to-top storage — flip it
        int absPitch = std::abs(bm.pitch);
        for (int row = 0; row < static_cast<int>(bm.rows); ++row) {
            std::memcpy(
                result.data.data() + row * absPitch,
                bm.buffer + (bm.rows - 1 - row) * absPitch,
                absPitch
            );
        }
        result.pitch = absPitch;
    }

    // Metrics
    auto& gm = slot->metrics;
    result.metrics.width    = result.width;
    result.metrics.height   = result.height;
    result.metrics.bearingX = slot->bitmap_left;
    result.metrics.bearingY = slot->bitmap_top;
    result.metrics.advance  = static_cast<int>(slot->advance.x >> 6);
    result.metrics.advanceY = static_cast<int>(slot->advance.y >> 6);
    result.metrics.isEmpty  = (result.width == 0 || result.height == 0);
    result.valid = true;
#else
    // Stub: generate a simple filled rectangle for a glyph
    result.metrics = getGlyphMetrics(codepoint);
    result.width   = result.metrics.width;
    result.height  = result.metrics.height;
    result.pitch   = result.width;
    if (result.width > 0 && result.height > 0) {
        result.data.assign(static_cast<size_t>(result.width * result.height), 0xFF);
    }
    result.valid = true;
#endif
    return result;
}

int FontFace::getKerning(char32_t left, char32_t right) const {
#ifdef ZEPRA_HAS_FREETYPE
    if (!ftFace_ || !FT_HAS_KERNING(ftFace_)) return 0;

    uint32_t leftIdx  = FT_Get_Char_Index(ftFace_, static_cast<FT_ULong>(left));
    uint32_t rightIdx = FT_Get_Char_Index(ftFace_, static_cast<FT_ULong>(right));
    if (!leftIdx || !rightIdx) return 0;

    FT_Vector kern;
    FT_Error err = FT_Get_Kerning(ftFace_, leftIdx, rightIdx, FT_KERNING_DEFAULT, &kern);
    if (err) return 0;

    return static_cast<int>(kern.x >> 6);
#else
    return 0;
#endif
}

// ============================================================================
// FontFace — Properties
// ============================================================================

bool FontFace::isBold() const {
#ifdef ZEPRA_HAS_FREETYPE
    return ftFace_ && (ftFace_->style_flags & FT_STYLE_FLAG_BOLD);
#else
    return false;
#endif
}

bool FontFace::isItalic() const {
#ifdef ZEPRA_HAS_FREETYPE
    return ftFace_ && (ftFace_->style_flags & FT_STYLE_FLAG_ITALIC);
#else
    return false;
#endif
}

bool FontFace::isFixedWidth() const {
#ifdef ZEPRA_HAS_FREETYPE
    return ftFace_ && FT_IS_FIXED_WIDTH(ftFace_);
#else
    return false;
#endif
}

// ============================================================================
// FontFace — Text measurement
// ============================================================================

int FontFace::measureText(const std::string& text) const {
    if (!loaded_) return 0;

    int totalWidth = 0;
    char32_t prevCP = 0;

    // Simple UTF-8 to codepoint iteration
    const uint8_t* p = reinterpret_cast<const uint8_t*>(text.data());
    const uint8_t* end = p + text.size();

    while (p < end) {
        char32_t cp = 0;

        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = (*p++ & 0x1F);
            if (p < end) cp = (cp << 6) | (*p++ & 0x3F);
        } else if ((*p & 0xF0) == 0xE0) {
            cp = (*p++ & 0x0F);
            if (p < end) cp = (cp << 6) | (*p++ & 0x3F);
            if (p < end) cp = (cp << 6) | (*p++ & 0x3F);
        } else if ((*p & 0xF8) == 0xF0) {
            cp = (*p++ & 0x07);
            if (p < end) cp = (cp << 6) | (*p++ & 0x3F);
            if (p < end) cp = (cp << 6) | (*p++ & 0x3F);
            if (p < end) cp = (cp << 6) | (*p++ & 0x3F);
        } else {
            ++p; // skip bad byte
            continue;
        }

        // Apply kerning
        if (prevCP) {
            totalWidth += getKerning(prevCP, cp);
        }

        // Add advance
        GlyphMetrics m = getGlyphMetrics(cp);
        totalWidth += m.advance;
        prevCP = cp;
    }

    return totalWidth;
}

} // namespace Zepra::Fonts
