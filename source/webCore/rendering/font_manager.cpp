// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file font_manager.cpp
 * @brief Font manager implementation — registry, resolution, caching
 */

#include "rendering/font_manager.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cassert>

#ifdef _WIN32
#   include <windows.h>
#   include <shlobj.h>  // for SHGetFolderPath
#endif

namespace Zepra::Fonts {

// ============================================================================
// Singleton
// ============================================================================

FontManager& FontManager::instance() {
    static FontManager inst;
    return inst;
}

// ============================================================================
// Initialize
// ============================================================================

bool FontManager::initialize() {
    if (initialized_) return true;

    // Initialize FreeType
    if (!FontLibrary::instance().initialize()) {
        return false;
    }

    // Set up CSS generic family defaults
    // These are overridden by registerSystemFonts() with actual found fonts
    genericFamilies_["serif"]      = "Times New Roman";
    genericFamilies_["sans-serif"] = "Arial";
    genericFamilies_["monospace"]  = "Courier New";
    genericFamilies_["cursive"]    = "Comic Sans MS";
    genericFamilies_["fantasy"]    = "Impact";
    genericFamilies_["system-ui"]  = "Segoe UI";

    initialized_ = true;
    registerSystemFonts();
    return true;
}

// ============================================================================
// System Font Discovery
// ============================================================================

static std::vector<std::string> getSystemFontPaths() {
    std::vector<std::string> paths;

#ifdef _WIN32
    // Windows font directory
    char winDir[MAX_PATH];
    if (GetWindowsDirectoryA(winDir, MAX_PATH)) {
        paths.push_back(std::string(winDir) + "\\Fonts");
    }
    // Also check user fonts (Windows 10+)
    char appData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appData))) {
        paths.push_back(std::string(appData) + "\\Microsoft\\Windows\\Fonts");
    }

#elif defined(__APPLE__)
    paths.push_back("/System/Library/Fonts");
    paths.push_back("/Library/Fonts");
    // User fonts
    const char* home = getenv("HOME");
    if (home) {
        paths.push_back(std::string(home) + "/Library/Fonts");
    }

#else
    // Linux / XDG
    paths.push_back("/usr/share/fonts");
    paths.push_back("/usr/local/share/fonts");
    paths.push_back("/usr/share/truetype");

    // User fonts
    const char* home = getenv("HOME");
    if (home) {
        paths.push_back(std::string(home) + "/.fonts");
        paths.push_back(std::string(home) + "/.local/share/fonts");
    }

    // XDG data dirs
    const char* xdgDirs = getenv("XDG_DATA_DIRS");
    if (xdgDirs) {
        std::istringstream ss(xdgDirs);
        std::string dir;
        while (std::getline(ss, dir, ':')) {
            paths.push_back(dir + "/fonts");
        }
    }
#endif

    return paths;
}

void FontManager::registerSystemFonts() {
    std::vector<std::string> fontPaths = getSystemFontPaths();

    int registered = 0;
    for (const auto& dir : fontPaths) {
        if (!std::filesystem::exists(dir)) continue;
        try {
            for (auto& entry : std::filesystem::recursive_directory_iterator(
                    dir,
                    std::filesystem::directory_options::skip_permission_denied)) {
                if (!entry.is_regular_file()) continue;
                auto ext = entry.path().extension().string();
                // Convert to lowercase for comparison
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".ttf" || ext == ".otf" || ext == ".ttc" ||
                    ext == ".woff" || ext == ".woff2") {
                    if (registerFontFile(entry.path().string(), /*isSystem=*/true))
                        ++registered;
                }
            }
        } catch (...) {
            // Skip inaccessible directories
        }
    }

    // Update generic families with actual found fonts
    auto updateGeneric = [&](const std::string& generic,
                              const std::initializer_list<std::string>& candidates) {
        for (const auto& candidate : candidates) {
            std::string normalized = normalizeFamily(candidate);
            for (const auto& reg : registry_) {
                if (reg.family == normalized) {
                    genericFamilies_[generic] = candidate;
                    return;
                }
            }
        }
    };

    // Platform-specific priority lists for CSS generic families
#ifdef _WIN32
    updateGeneric("serif",      {"Georgia", "Times New Roman", "Palatino Linotype"});
    updateGeneric("sans-serif", {"Segoe UI", "Arial", "Calibri", "Tahoma"});
    updateGeneric("monospace",  {"Consolas", "Courier New", "Lucida Console"});
    updateGeneric("system-ui",  {"Segoe UI", "Tahoma", "Arial"});
#elif defined(__APPLE__)
    updateGeneric("serif",      {"Georgia", "Times New Roman"});
    updateGeneric("sans-serif", {"Helvetica Neue", "Helvetica", "Arial"});
    updateGeneric("monospace",  {"Menlo", "Monaco", "Courier New"});
    updateGeneric("system-ui",  {"San Francisco", "Helvetica Neue"});
#else
    updateGeneric("serif",      {"DejaVu Serif", "Liberation Serif", "FreeSerif"});
    updateGeneric("sans-serif", {"DejaVu Sans", "Liberation Sans", "Ubuntu", "Noto Sans"});
    updateGeneric("monospace",  {"DejaVu Sans Mono", "Liberation Mono", "Ubuntu Mono"});
    updateGeneric("system-ui",  {"Ubuntu", "Cantarell", "Noto Sans"});
#endif

    // Set default fallback font
    if (!registry_.empty() && defaultFontPath_.empty()) {
        // Pick the first sans-serif we found
        auto it = genericFamilies_.find("sans-serif");
        std::string preferred = (it != genericFamilies_.end()) ? it->second : "";

        for (const auto& reg : registry_) {
            if (!preferred.empty() && reg.family == normalizeFamily(preferred)) {
                defaultFontPath_ = reg.path;
                break;
            }
        }
        if (defaultFontPath_.empty()) {
            defaultFontPath_ = registry_.front().path;
        }
    }
}

// ============================================================================
// Font Registration
// ============================================================================

bool FontManager::registerFontFile(const std::string& path, bool isSystem) {
    // Quick probe: try to load the face to extract metadata
    // Use a temporary face so we don't pollute the cache
    FontFace probe;
    if (!probe.load(path)) return false;

    RegisteredFont reg;
    reg.path        = path;
    reg.displayName = probe.familyName();
    reg.family      = normalizeFamily(probe.familyName());
    reg.weight      = probe.isBold() ? FontWeight::Bold : FontWeight::Normal;
    reg.italic      = probe.isItalic();
    reg.isSystem    = isSystem;

    std::lock_guard<std::mutex> lock(mutex_);
    registry_.push_back(std::move(reg));
    return true;
}

bool FontManager::registerFontMemory(std::vector<uint8_t> data, const std::string& family) {
    FontFace probe;
    if (!probe.loadFromMemory(data)) return false;

    RegisteredFont reg;
    reg.displayName = !probe.familyName().empty() ? probe.familyName() : family;
    reg.family      = normalizeFamily(reg.displayName);
    reg.weight      = probe.isBold() ? FontWeight::Bold : FontWeight::Normal;
    reg.italic      = probe.isItalic();
    reg.isSystem    = false;
    reg.data        = std::move(data);

    std::lock_guard<std::mutex> lock(mutex_);
    registry_.push_back(std::move(reg));
    return true;
}

// ============================================================================
// Font Resolution
// ============================================================================

std::string FontManager::normalizeFamily(const std::string& name) {
    std::string result = name;
    // Trim leading/trailing whitespace and quotes
    while (!result.empty() && (result.front() == '"' || result.front() == '\'' ||
                                result.front() == ' '))
        result.erase(result.begin());
    while (!result.empty() && (result.back() == '"' || result.back() == '\'' ||
                                result.back() == ' '))
        result.pop_back();
    // Lowercase
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

std::vector<std::string> FontManager::parseFamilyList(const std::string& families) {
    std::vector<std::string> result;
    std::string current;
    bool inQuote = false;
    char quoteChar = 0;

    for (char c : families) {
        if (!inQuote && (c == '"' || c == '\'')) {
            inQuote = true;
            quoteChar = c;
        } else if (inQuote && c == quoteChar) {
            inQuote = false;
        } else if (!inQuote && c == ',') {
            std::string trimmed = current;
            // Trim whitespace
            while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
            while (!trimmed.empty() && trimmed.back()  == ' ') trimmed.pop_back();
            if (!trimmed.empty()) result.push_back(trimmed);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        while (!current.empty() && current.front() == ' ') current.erase(current.begin());
        while (!current.empty() && current.back()  == ' ') current.pop_back();
        if (!current.empty()) result.push_back(current);
    }
    return result;
}

const RegisteredFont* FontManager::findBestMatch(
        const std::string& family,
        FontWeight weight,
        bool italic) const {

    const RegisteredFont* bestExact   = nullptr;
    const RegisteredFont* bestWeight  = nullptr;
    const RegisteredFont* bestFamily  = nullptr;

    for (const auto& reg : registry_) {
        if (reg.family != family) continue;

        if (!bestFamily) bestFamily = &reg;

        // Try exact match
        if (reg.weight == weight && reg.italic == italic) {
            bestExact = &reg;
            break;
        }

        // Try weight match (ignore italic)
        if (reg.weight == weight && !bestWeight) {
            bestWeight = &reg;
        }
    }

    if (bestExact)  return bestExact;
    if (bestWeight) return bestWeight;
    return bestFamily;
}

std::shared_ptr<FontFace> FontManager::resolveFont(
        const std::string& families,
        int pixelSize,
        FontWeight weight,
        bool italic) {

    if (!initialized_) initialize();

    auto familyList = parseFamilyList(families);

    // Try each family in order
    for (const auto& rawFamily : familyList) {
        std::string family = normalizeFamily(rawFamily);

        // Resolve CSS generic families
        auto genericIt = genericFamilies_.find(family);
        if (genericIt != genericFamilies_.end()) {
            family = normalizeFamily(genericIt->second);
        }

        FontDescriptor desc;
        desc.family    = family;
        desc.pixelSize = pixelSize;
        desc.weight    = weight;
        desc.italic    = italic;

        // Check cache first
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (auto* cached = faceCache_.get(desc)) {
                return *cached;
            }
        }

        // Try to load
        auto face = tryLoadFace(desc);
        if (face) {
            std::lock_guard<std::mutex> lock(mutex_);
            faceCache_.put(desc, face);
            return face;
        }
    }

    // Nothing matched — return default font
    return defaultFont(pixelSize);
}

std::shared_ptr<FontFace> FontManager::tryLoadFace(const FontDescriptor& desc) {
    std::lock_guard<std::mutex> lock(mutex_);
    const RegisteredFont* reg = findBestMatch(desc.family, desc.weight, desc.italic);
    if (!reg) return nullptr;

    auto face = std::make_shared<FontFace>();
    bool ok = false;

    if (!reg->data.empty()) {
        ok = face->loadFromMemory(reg->data);
    } else if (!reg->path.empty()) {
        ok = face->load(reg->path);
    }

    if (!ok) return nullptr;
    face->setPixelSize(desc.pixelSize);
    return face;
}

// ============================================================================
// Default Font
// ============================================================================

std::shared_ptr<FontFace> FontManager::defaultFont(int pixelSize) {
    if (!initialized_) initialize();

    // Check if we already have a cached default face at this size
    FontDescriptor desc;
    desc.family    = "__default__";
    desc.pixelSize = pixelSize;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto* cached = faceCache_.get(desc)) {
            return *cached;
        }
    }

    auto face = std::make_shared<FontFace>();
    bool ok = false;

    if (!defaultFontPath_.empty()) {
        ok = face->load(defaultFontPath_);
    }

    if (!ok) {
        // Last resort: try well-known paths
        static const std::vector<std::string> lastResort = {
#ifdef _WIN32
            "C:\\Windows\\Fonts\\arial.ttf",
            "C:\\Windows\\Fonts\\segoeui.ttf",
            "C:\\Windows\\Fonts\\tahoma.ttf",
#elif defined(__APPLE__)
            "/System/Library/Fonts/Helvetica.ttc",
            "/System/Library/Fonts/Arial.ttf",
#else
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
        };
        for (const auto& path : lastResort) {
            if (face->load(path)) { ok = true; break; }
        }
    }

    if (ok) {
        face->setPixelSize(pixelSize);
    }
    // Even if loading failed, the stub implementation still works

    std::lock_guard<std::mutex> lock(mutex_);
    faceCache_.put(desc, face);
    return face;
}

// ============================================================================
// Misc
// ============================================================================

void FontManager::setDefaultFont(const std::string& path) {
    defaultFontPath_ = path;
    std::lock_guard<std::mutex> lock(mutex_);
    defaultFace_.reset(); // Force reload
}

void FontManager::setGenericFamily(const std::string& generic, const std::string& family) {
    std::lock_guard<std::mutex> lock(mutex_);
    genericFamilies_[generic] = family;
}

std::vector<std::string> FontManager::registeredFamilies() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    for (const auto& reg : registry_) {
        if (std::find(result.begin(), result.end(), reg.displayName) == result.end())
            result.push_back(reg.displayName);
    }
    return result;
}

int FontManager::measureText(const std::string& text, const FontDescriptor& desc) {
    auto face = resolveFont(desc.family, desc.pixelSize, desc.weight, desc.italic);
    if (!face) return 0;
    return face->measureText(text);
}

void FontManager::clearCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    faceCache_.clear();
    defaultFace_.reset();
}

void FontManager::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        faceCache_.clear();
        defaultFace_.reset();
        registry_.clear();
    }
    initialized_ = false;
}

} // namespace Zepra::Fonts
