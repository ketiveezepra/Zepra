// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "font_fallback.h"
#include <hb.h>
#include <hb-ft.h>
#include <cstring>
#include <algorithm>
#include <cstdlib>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <sys/stat.h>
#  define ZEPRA_STAT _stat
#  define ZEPRA_S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  define ZEPRA_STAT stat
#  define ZEPRA_S_ISDIR(m) S_ISDIR(m)
#endif

namespace NXRender {
namespace Text {

FontFallbackManager& FontFallbackManager::instance() {
    static FontFallbackManager instance;
    return instance;
}

FontFallbackManager::FontFallbackManager() {}

FontFallbackManager::~FontFallbackManager() {
    shutdown();
}

bool FontFallbackManager::initialize() {
    if (ftLibrary_) return true;
    if (FT_Init_FreeType(&ftLibrary_) != 0) {
        return false;
    }

    // Auto-discover system fonts
    discoverSystemFonts();
    return true;
}

void FontFallbackManager::shutdown() {
    for (auto& pair : hbFontCache_) {
        hb_font_destroy(pair.second);
    }
    hbFontCache_.clear();

    for (auto& pair : faceCache_) {
        FT_Done_Face(pair.second);
    }
    faceCache_.clear();

    if (ftLibrary_) {
        FT_Done_FreeType(ftLibrary_);
        ftLibrary_ = nullptr;
    }
}

// ==================================================================
// Font registration
// ==================================================================

bool FontFallbackManager::registerFont(const std::string& family,
                                         const std::string& path,
                                         bool isBold, bool isItalic) {
    // Validate file exists
    struct ZEPRA_STAT st;
    if (ZEPRA_STAT(path.c_str(), &st) != 0) return false;

    registry_[family].push_back({family, path, isBold, isItalic});
    return true;
}

// ==================================================================
// System font discovery
// ==================================================================

void FontFallbackManager::discoverSystemFonts() {
#ifdef _WIN32
    // Windows font directories
    char winDir[MAX_PATH];
    if (GetWindowsDirectoryA(winDir, MAX_PATH)) {
        std::string fontDir = std::string(winDir) + "\\Fonts";
        scanFontDirectory(fontDir);
    }
    // User fonts (Windows 10+)
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData) {
        std::string userFonts = std::string(localAppData) + "\\Microsoft\\Windows\\Fonts";
        scanFontDirectory(userFonts);
    }
#elif defined(__APPLE__)
    static const char* fontDirs[] = {
        "/System/Library/Fonts",
        "/System/Library/Fonts/Supplemental",
        "/Library/Fonts",
        nullptr
    };
    for (int i = 0; fontDirs[i]; i++) {
        scanFontDirectory(fontDirs[i]);
    }
    if (const char* home = std::getenv("HOME")) {
        scanFontDirectory(std::string(home) + "/Library/Fonts");
    }
#else
    static const char* fontDirs[] = {
        "/usr/share/fonts",
        "/usr/local/share/fonts",
        "/usr/share/fonts/truetype",
        "/usr/share/fonts/opentype",
        nullptr
    };
    for (int i = 0; fontDirs[i]; i++) {
        scanFontDirectory(fontDirs[i]);
    }
#endif
}

void FontFallbackManager::scanFontDirectory(const std::string& dirPath) {
#ifdef _WIN32
    std::string searchPattern = dirPath + "\\*";
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPattern.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (findData.cFileName[0] == '.') continue;
        std::string fullPath = dirPath + "\\" + findData.cFileName;

        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scanFontDirectory(fullPath);
            continue;
        }

        std::string name(findData.cFileName);
        size_t dot = name.rfind('.');
        if (dot == std::string::npos) continue;

        std::string ext = name.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".ttf" && ext != ".otf" && ext != ".ttc") continue;

        FT_Face probeFace;
        if (FT_New_Face(ftLibrary_, fullPath.c_str(), 0, &probeFace) == 0) {
            if (probeFace->family_name) {
                std::string family = probeFace->family_name;
                bool bold = (probeFace->style_flags & FT_STYLE_FLAG_BOLD) != 0;
                bool italic = (probeFace->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
                if (registry_.find(family) == registry_.end()) {
                    registry_[family].push_back({family, fullPath, bold, italic});
                }
            }
            FT_Done_Face(probeFace);
        }
    } while (FindNextFileA(hFind, &findData));

    FindClose(hFind);
#else
    DIR* dir = opendir(dirPath.c_str());
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;

        std::string fullPath = dirPath + "/" + entry->d_name;
        struct stat st;
        if (stat(fullPath.c_str(), &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scanFontDirectory(fullPath);
            continue;
        }

        std::string name(entry->d_name);
        size_t dot = name.rfind('.');
        if (dot == std::string::npos) continue;

        std::string ext = name.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".ttf" && ext != ".otf" && ext != ".ttc") continue;

        FT_Face probeFace;
        if (FT_New_Face(ftLibrary_, fullPath.c_str(), 0, &probeFace) == 0) {
            if (probeFace->family_name) {
                std::string family = probeFace->family_name;
                bool bold = (probeFace->style_flags & FT_STYLE_FLAG_BOLD) != 0;
                bool italic = (probeFace->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
                if (registry_.find(family) == registry_.end()) {
                    registry_[family].push_back({family, fullPath, bold, italic});
                }
            }
            FT_Done_Face(probeFace);
        }
    }

    closedir(dir);
#endif
}

// ==================================================================
// Cache key
// ==================================================================

std::string FontFallbackManager::buildKey(const std::string& family,
                                            bool isBold, bool isItalic,
                                            float size) const {
    return family + (isBold ? "_B" : "_N") + (isItalic ? "_I" : "_N") +
           "_" + std::to_string(static_cast<int>(size));
}

// ==================================================================
// Face resolution
// ==================================================================

FT_Face FontFallbackManager::getFace(const std::string& family,
                                       bool isBold, bool isItalic) {
    std::string key = buildKey(family, isBold, isItalic, 0.0f);

    auto it = faceCache_.find(key);
    if (it != faceCache_.end()) return it->second;

    auto regIt = registry_.find(family);
    if (regIt == registry_.end()) {
        // Try common aliases
        return tryFamilyAlias(family, isBold, isItalic);
    }

    const auto& fonts = regIt->second;
    const FontRegistration* bestMatch = nullptr;

    // Exact match first
    for (const auto& reg : fonts) {
        if (reg.isBold == isBold && reg.isItalic == isItalic) {
            bestMatch = &reg;
            break;
        }
    }

    // Relax: match weight only
    if (!bestMatch) {
        for (const auto& reg : fonts) {
            if (reg.isBold == isBold) {
                bestMatch = &reg;
                break;
            }
        }
    }

    // Any variant
    if (!bestMatch && !fonts.empty()) bestMatch = &fonts[0];

    if (bestMatch) {
        FT_Face face;
        if (FT_New_Face(ftLibrary_, bestMatch->path.c_str(), 0, &face) == 0) {
            faceCache_[key] = face;
            return face;
        }
    }

    return nullptr;
}

FT_Face FontFallbackManager::tryFamilyAlias(const std::string& family,
                                              bool isBold, bool isItalic) {
    static const std::pair<const char*, const char*> aliases[] = {
        {"sans-serif", "DejaVu Sans"},
        {"serif", "DejaVu Serif"},
        {"monospace", "DejaVu Sans Mono"},
        {"Arial", "Liberation Sans"},
        {"Helvetica", "Liberation Sans"},
        {"Times New Roman", "Liberation Serif"},
        {"Courier New", "Liberation Mono"},
    };

    for (const auto& [alias, canonical] : aliases) {
        if (family == alias) {
            auto it = registry_.find(canonical);
            if (it != registry_.end()) {
                return getFace(canonical, isBold, isItalic);
            }
        }
    }
    return nullptr;
}

// ==================================================================
// HarfBuzz font creation
// ==================================================================

hb_font_t* FontFallbackManager::getHbFont(const std::string& family,
                                             bool isBold, bool isItalic,
                                             float size) {
    std::string key = buildKey(family, isBold, isItalic, size);

    auto it = hbFontCache_.find(key);
    if (it != hbFontCache_.end()) return it->second;

    FT_Face face = getFace(family, isBold, isItalic);
    if (!face) return nullptr;

    FT_Set_Char_Size(face, 0, static_cast<FT_F26Dot6>(size * 64.0f), 96, 96);

    hb_font_t* hbFont = hb_ft_font_create(face, nullptr);
    if (hbFont) {
        hb_ft_font_set_load_flags(hbFont, FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING);
        hbFontCache_[key] = hbFont;
        return hbFont;
    }

    return nullptr;
}

// ==================================================================
// Codepoint coverage check
// ==================================================================

bool FontFallbackManager::hasGlyph(const std::string& family,
                                     bool isBold, bool isItalic,
                                     uint32_t codepoint) {
    FT_Face face = getFace(family, isBold, isItalic);
    if (!face) return false;
    return FT_Get_Char_Index(face, codepoint) != 0;
}

std::string FontFallbackManager::findFontForCodepoint(uint32_t codepoint) {
    // Search all registered fonts for coverage
    for (const auto& pair : registry_) {
        const auto& family = pair.first;
        const auto& regs = pair.second;
        if (regs.empty()) continue;

        FT_Face face = getFace(family, false, false);
        if (face && FT_Get_Char_Index(face, codepoint) != 0) {
            return family;
        }
    }
    return "";
}

size_t FontFallbackManager::registeredFamilyCount() const {
    return registry_.size();
}

size_t FontFallbackManager::cachedFaceCount() const {
    return faceCache_.size();
}

} // namespace Text
} // namespace NXRender
