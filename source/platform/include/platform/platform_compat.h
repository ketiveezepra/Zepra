// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file platform_compat.h
 * @brief Cross-platform compatibility layer (POSIX ↔ Win32)
 *
 * Include this ONCE in any source file that uses POSIX-specific APIs.
 * It provides transparent shims so the same code compiles on:
 *   - Linux / NeolyxOS (native POSIX)
 *   - Windows (Win32 API shims)
 *   - macOS (native POSIX)
 *
 * ⚠  NEVER hardcode paths or OS assumptions outside this header.
 *    Use the zepra_platform_*() helper functions instead.
 *
 * Security: All path expansions are bounds-checked.
 *           No user-controlled format strings.
 */

#pragma once

// ============================================================================
// Platform Detection (compile-time)
// ============================================================================

#if defined(_WIN32) || defined(_WIN64)
#  define ZEPRA_OS_WINDOWS 1
#  define ZEPRA_OS_LINUX   0
#  define ZEPRA_OS_MACOS   0
#elif defined(__APPLE__) && defined(__MACH__)
#  define ZEPRA_OS_WINDOWS 0
#  define ZEPRA_OS_LINUX   0
#  define ZEPRA_OS_MACOS   1
#elif defined(__linux__)
#  define ZEPRA_OS_WINDOWS 0
#  define ZEPRA_OS_LINUX   1
#  define ZEPRA_OS_MACOS   0
#else
#  define ZEPRA_OS_WINDOWS 0
#  define ZEPRA_OS_LINUX   1   // Default to POSIX
#  define ZEPRA_OS_MACOS   0
#endif

// ============================================================================
// POSIX ↔ Win32 Header Shims
// ============================================================================

#if ZEPRA_OS_WINDOWS

// Win32 core (include before anything else)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX   // Prevent min/max macro conflicts with <algorithm>
#  endif
#  include <windows.h>

// POSIX-equivalent Win32 headers
#  include <io.h>         // _access, _open, _close, _read, _write
#  include <process.h>    // _getpid
#  include <direct.h>     // _mkdir, _getcwd, _chdir
#  include <sys/types.h>
#  include <sys/stat.h>   // _stat, _fstat (available on MSVC)

// ---- POSIX function shims ----

// File access checks
#  ifndef F_OK
#    define F_OK 0
#  endif
#  ifndef R_OK
#    define R_OK 4
#  endif
#  ifndef W_OK
#    define W_OK 2
#  endif

// access() → _access()
#  ifndef access
#    define access _access
#  endif

// getpid() → _getpid()
#  ifndef getpid
#    define getpid _getpid
#  endif

// getcwd() → _getcwd()
#  ifndef getcwd
#    define getcwd _getcwd
#  endif

// mkdir() → _mkdir() (Windows mkdir has no mode parameter)
#  ifndef ZEPRA_MKDIR_DEFINED
#    define ZEPRA_MKDIR_DEFINED
#    include <cstdlib>
     static inline int zepra_mkdir(const char* path, unsigned int /*mode*/) {
         return _mkdir(path);
     }
#    define mkdir(path, mode) zepra_mkdir(path, mode)
#  endif

// sleep() / usleep() → Sleep()
static inline void zepra_sleep(unsigned int seconds) {
    Sleep(seconds * 1000u);
}
static inline void zepra_usleep(unsigned int microseconds) {
    Sleep(microseconds / 1000u);
}
#  ifndef sleep
#    define sleep(s) zepra_sleep(s)
#  endif
#  ifndef usleep
#    define usleep(us) zepra_usleep(us)
#  endif

// ssize_t (not defined on MSVC)
#  ifndef _SSIZE_T_DEFINED
#    define _SSIZE_T_DEFINED
     typedef intptr_t ssize_t;
#  endif

#else
// ---- POSIX (Linux / NeolyxOS / macOS) ----
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

// ============================================================================
// Cross-platform Helpers (use these instead of raw OS APIs)
// ============================================================================

#include <string>
#include <cstdlib>
#include <cstring>

namespace Zepra::Platform {

/**
 * @brief Get the user's home directory (runtime-detected, never hardcoded)
 * @return Home directory path, or empty string if unavailable
 *
 * Linux/NeolyxOS: $HOME
 * Windows:        %USERPROFILE%
 * macOS:          $HOME
 */
static inline std::string getHomeDirectory() {
#if ZEPRA_OS_WINDOWS
    const char* home = std::getenv("USERPROFILE");
    if (!home) {
        // Fallback: combine HOMEDRIVE + HOMEPATH
        const char* drive = std::getenv("HOMEDRIVE");
        const char* path  = std::getenv("HOMEPATH");
        if (drive && path) {
            return std::string(drive) + std::string(path);
        }
    }
    return home ? std::string(home) : std::string();
#else
    const char* home = std::getenv("HOME");
    return home ? std::string(home) : std::string();
#endif
}

/**
 * @brief Get Zepra's per-user configuration directory (not created here)
 * @return Directory path, or empty string if unavailable
 *
 * Linux/NeolyxOS: $XDG_CONFIG_HOME/zepra, else ~/.config/zepra
 * Windows:        %APPDATA%\Zepra
 * macOS:          ~/Library/Application Support/Zepra
 */
static inline std::string getUserConfigDirectory() {
#if ZEPRA_OS_WINDOWS
    const char* appData = std::getenv("APPDATA");
    if (appData && *appData) return std::string(appData) + "\\Zepra";
    std::string home = getHomeDirectory();
    return home.empty() ? std::string() : home + "\\AppData\\Roaming\\Zepra";
#elif ZEPRA_OS_MACOS
    std::string home = getHomeDirectory();
    return home.empty() ? std::string() : home + "/Library/Application Support/Zepra";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg == '/') return std::string(xdg) + "/zepra";
    std::string home = getHomeDirectory();
    return home.empty() ? std::string() : home + "/.config/zepra";
#endif
}

/**
 * @brief Expand tilde (~) in a path to the user's home directory
 * @param path Input path (may start with ~)
 * @return Expanded path, or original if no ~ prefix
 */
static inline std::string expandHomePath(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    std::string home = getHomeDirectory();
    if (home.empty()) return path;  // Can't expand — return as-is
    return home + path.substr(1);
}

/**
 * @brief Convert a file:// URL to a local filesystem path
 *
 * Decodes %XX escapes and expands a leading ~. On Windows the drive form
 * file:///C:/dir/page.html becomes C:/dir/page.html (the slash before the
 * drive letter is part of the URL, not the path).
 */
static inline std::string fileUrlToPath(const std::string& url) {
    std::string rest = url.compare(0, 7, "file://") == 0 ? url.substr(7) : url;
    if (rest.compare(0, 10, "localhost/") == 0) rest = rest.substr(9);

    auto hexValue = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string path;
    path.reserve(rest.size());
    for (size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size() &&
            hexValue(rest[i + 1]) >= 0 && hexValue(rest[i + 2]) >= 0) {
            path += static_cast<char>(hexValue(rest[i + 1]) * 16 + hexValue(rest[i + 2]));
            i += 2;
        } else {
            path += rest[i];
        }
    }

#if ZEPRA_OS_WINDOWS
    if (path.size() >= 3 && path[0] == '/' && path[2] == ':' &&
        ((path[1] >= 'A' && path[1] <= 'Z') || (path[1] >= 'a' && path[1] <= 'z'))) {
        path.erase(0, 1);
    }
#endif
    return expandHomePath(path);
}

/**
 * @brief Get the path separator for the current OS
 */
static inline char pathSeparator() {
#if ZEPRA_OS_WINDOWS
    return '\\';
#else
    return '/';
#endif
}

/**
 * @brief Normalize path separators for the current OS
 */
static inline std::string normalizePath(const std::string& path) {
    std::string result = path;
#if ZEPRA_OS_WINDOWS
    for (char& c : result) {
        if (c == '/') c = '\\';
    }
#else
    for (char& c : result) {
        if (c == '\\') c = '/';
    }
#endif
    return result;
}

/**
 * @brief Get system page size (for memory allocation)
 */
static inline size_t getPageSize() {
#if ZEPRA_OS_WINDOWS
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<size_t>(si.dwPageSize);
#else
    return static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
}

/**
 * @brief Check if a file exists
 */
static inline bool fileExists(const char* path) {
#if ZEPRA_OS_WINDOWS
    return _access(path, 0) == 0;
#else
    return access(path, F_OK) == 0;
#endif
}

/**
 * @brief Create a directory (cross-platform, with mode on POSIX)
 */
static inline int createDirectory(const char* path) {
#if ZEPRA_OS_WINDOWS
    return _mkdir(path);
#else
    return ::mkdir(path, 0755);
#endif
}

} // namespace Zepra::Platform
