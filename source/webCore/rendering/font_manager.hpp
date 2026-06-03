// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file font_manager.hpp
 * @brief Font registry, fallback chain, and LRU glyph metrics cache
 *
 * The FontManager is the central point for:
 *  - Registering system and web fonts
 *  - Resolving CSS font queries (family, weight, style, size)
 *  - Managing a font fallback chain for Unicode coverage
 *  - Caching FontFace instances and glyph metrics
 *
 * Usage:
 *   FontManager& fm = FontManager::instance();
 *   fm.registerSystemFonts();
 *   auto face = fm.resolveFont("Arial", 16, FontWeight::Bold, FontStyle::Normal);
 *   auto bm = face->renderGlyph(U'A');
 */

#pragma once

#include "rendering/font_face.hpp"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <list>
#include <functional>
#include <optional>
#include <mutex>

namespace Zepra::Fonts {

// ============================================================================
// Font Query / Descriptor
// ============================================================================

enum class FontWeight : int {
    Thin        = 100,
    ExtraLight  = 200,
    Light       = 300,
    Normal      = 400,
    Medium      = 500,
    SemiBold    = 600,
    Bold        = 700,
    ExtraBold   = 800,
    Black       = 900
};

struct FontDescriptor {
    std::string  family;
    int          pixelSize = 16;
    FontWeight   weight    = FontWeight::Normal;
    bool         italic    = false;

    bool operator==(const FontDescriptor& o) const {
        return family == o.family && pixelSize == o.pixelSize &&
               weight == o.weight && italic == o.italic;
    }
};

struct FontDescriptorHash {
    size_t operator()(const FontDescriptor& d) const noexcept {
        size_t h = std::hash<std::string>{}(d.family);
        h ^= std::hash<int>{}(d.pixelSize) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(static_cast<int>(d.weight)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(d.italic) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// ============================================================================
// Registered Font Entry
// ============================================================================

struct RegisteredFont {
    std::string   path;           ///< File path (empty for memory-loaded)
    std::string   family;         ///< Canonical family name (lowercase)
    std::string   displayName;    ///< Display name as-is from font metadata
    FontWeight    weight    = FontWeight::Normal;
    bool          italic    = false;
    bool          isSystem  = true;
    std::vector<uint8_t> data;    ///< Non-empty for memory-loaded fonts
};

// ============================================================================
// LRU Cache for FontFace instances
// ============================================================================

template<typename Key, typename Value, typename Hash = std::hash<Key>>
class LRUCache {
public:
    explicit LRUCache(size_t capacity) : capacity_(capacity) {}

    /**
     * @brief Get a cached value if present
     * @return pointer to value or nullptr
     */
    Value* get(const Key& key) {
        auto it = map_.find(key);
        if (it == map_.end()) return nullptr;
        // Move to front (most recently used)
        order_.splice(order_.begin(), order_, it->second.listIt);
        return &it->second.value;
    }

    /**
     * @brief Insert or update a value
     */
    void put(const Key& key, Value value) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second.value = std::move(value);
            order_.splice(order_.begin(), order_, it->second.listIt);
            return;
        }
        if (map_.size() >= capacity_) {
            // Evict least recently used
            auto last = order_.back();
            order_.pop_back();
            map_.erase(last);
        }
        order_.push_front(key);
        map_[key] = { std::move(value), order_.begin() };
    }

    void clear() {
        map_.clear();
        order_.clear();
    }

    size_t size() const { return map_.size(); }

private:
    struct Entry {
        Value value;
        typename std::list<Key>::iterator listIt;
    };
    size_t capacity_;
    std::list<Key> order_;
    std::unordered_map<Key, Entry, Hash> map_;
};

// ============================================================================
// FontManager
// ============================================================================

class FontManager {
public:
    /**
     * @brief Singleton accessor
     */
    static FontManager& instance();

    /**
     * @brief Initialize the font system
     * Must be called before any font is resolved.
     * Initializes FreeType and registers system fonts.
     */
    bool initialize();

    /**
     * @brief Scan the OS font directories and register all found fonts
     * Platform-specific paths:
     *   Windows:  C:\Windows\Fonts
     *   Linux:    /usr/share/fonts, ~/.fonts
     *   macOS:    /System/Library/Fonts, ~/Library/Fonts
     */
    void registerSystemFonts();

    /**
     * @brief Register a font from a file path
     */
    bool registerFontFile(const std::string& path, bool isSystem = false);

    /**
     * @brief Register a font from memory (e.g., downloaded web font)
     * @param data    Raw font bytes (TTF / OTF / WOFF)
     * @param family  Fallback family name (if font metadata unavailable)
     */
    bool registerFontMemory(std::vector<uint8_t> data, const std::string& family = "");

    /**
     * @brief Resolve a font for a given CSS query
     *
     * Resolves the best matching font face, creating and caching it as needed.
     * Falls back through the registered fallback chain if exact match unavailable.
     *
     * @param families   CSS font-family list (comma-separated families)
     * @param pixelSize  Size in pixels
     * @param weight     Font weight (400 = normal, 700 = bold)
     * @param italic     Whether italic style is desired
     * @return Shared pointer to the resolved FontFace (never null — falls back to default)
     */
    std::shared_ptr<FontFace> resolveFont(
        const std::string& families,
        int pixelSize,
        FontWeight weight = FontWeight::Normal,
        bool italic = false
    );

    /**
     * @brief Set the default fallback font (used when nothing else matches)
     */
    void setDefaultFont(const std::string& path);

    /**
     * @brief Set the system generic fallback list for CSS generic families:
     *   serif, sans-serif, monospace, cursive, fantasy, system-ui
     */
    void setGenericFamily(const std::string& generic, const std::string& family);

    /**
     * @brief Get all registered font families (for CSS @font-face enumeration)
     */
    std::vector<std::string> registeredFamilies() const;

    /**
     * @brief Get the default font face (fallback for all rendering)
     */
    std::shared_ptr<FontFace> defaultFont(int pixelSize = 16);

    /**
     * @brief Measure text width using the resolved font
     */
    int measureText(const std::string& text, const FontDescriptor& desc);

    /**
     * @brief Shutdown the font manager and release all faces
     */
    void shutdown();

    /**
     * @brief Clear the face cache (e.g., after DPI change)
     */
    void clearCache();

private:
    FontManager() = default;
    ~FontManager() { shutdown(); }
    FontManager(const FontManager&) = delete;
    FontManager& operator=(const FontManager&) = delete;

    /**
     * @brief Try to load a face matching the given descriptor
     */
    std::shared_ptr<FontFace> tryLoadFace(const FontDescriptor& desc);

    /**
     * @brief Find the best registered font for the given family+weight+italic
     */
    const RegisteredFont* findBestMatch(
        const std::string& family,
        FontWeight weight,
        bool italic
    ) const;

    /**
     * @brief Parse a comma-separated CSS family list into individual families
     */
    static std::vector<std::string> parseFamilyList(const std::string& families);

    /**
     * @brief Normalize family name for comparison
     */
    static std::string normalizeFamily(const std::string& name);

    bool initialized_ = false;

    // All registered fonts
    std::vector<RegisteredFont> registry_;

    // Generic family map (e.g., "sans-serif" -> "DejaVu Sans")
    std::unordered_map<std::string, std::string> genericFamilies_;

    // Default fallback font path
    std::string defaultFontPath_;

    // LRU cache of loaded font faces (max 128 faces)
    LRUCache<FontDescriptor, std::shared_ptr<FontFace>, FontDescriptorHash> faceCache_{128};

    // Default face cache
    std::shared_ptr<FontFace> defaultFace_;

    mutable std::mutex mutex_;
};

} // namespace Zepra::Fonts
