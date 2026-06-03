// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file clipboard.cpp
 * @brief Clipboard implementation — cross-platform
 *
 * Windows: Win32 OpenClipboard / SetClipboardData / GetClipboardData
 * Linux:   In-memory stub (X11 clipboard requires X11 window handle — deferred)
 * NeolyxOS: In-memory stub (NeolyxOS clipboard IPC TBD)
 */

#include "platform/clipboard.hpp"
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // DragQueryFileW, HDROP
#endif

namespace Zepra::Platform {

Clipboard& Clipboard::instance() {
    static Clipboard instance;
    return instance;
}

// =============================================================================
// setText / getText
// =============================================================================

bool Clipboard::setText(const std::string& text) {
#ifdef _WIN32
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();

    // Allocate global memory for the text
    SIZE_T bytes = text.size() + 1;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hg) {
        CloseClipboard();
        return false;
    }
    void* ptr = GlobalLock(hg);
    if (!ptr) {
        GlobalFree(hg);
        CloseClipboard();
        return false;
    }
    memcpy(ptr, text.c_str(), bytes);
    GlobalUnlock(hg);

    // CF_TEXT for ANSI; also set CF_UNICODETEXT for wide-string apps
    if (!SetClipboardData(CF_TEXT, hg)) {
        GlobalFree(hg);
        CloseClipboard();
        return false;
    }

    // Also set Unicode version for full compatibility
    std::wstring wide(text.begin(), text.end());
    SIZE_T wbytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL hgW = GlobalAlloc(GMEM_MOVEABLE, wbytes);
    if (hgW) {
        void* wptr = GlobalLock(hgW);
        if (wptr) {
            memcpy(wptr, wide.c_str(), wbytes);
            GlobalUnlock(hgW);
            SetClipboardData(CF_UNICODETEXT, hgW);
        } else {
            GlobalFree(hgW);
        }
    }

    CloseClipboard();
    return true;
#else
    // In-memory fallback for Linux/NeolyxOS
    memoryClipboard_ = text;
    return true;
#endif
}

std::optional<std::string> Clipboard::getText() {
#ifdef _WIN32
    if (!OpenClipboard(nullptr)) return std::nullopt;

    // Prefer Unicode
    HANDLE hd = GetClipboardData(CF_UNICODETEXT);
    if (hd) {
        const wchar_t* wide = static_cast<const wchar_t*>(GlobalLock(hd));
        if (wide) {
            // Convert wide to UTF-8 (narrow for now — full UTF-8 needs WideCharToMultiByte)
            int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1,
                                             nullptr, 0, nullptr, nullptr);
            std::string result(needed - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide, -1,
                                result.data(), needed, nullptr, nullptr);
            GlobalUnlock(hd);
            CloseClipboard();
            return result;
        }
        GlobalUnlock(hd);
    }

    // Fallback to ANSI
    hd = GetClipboardData(CF_TEXT);
    if (hd) {
        const char* text = static_cast<const char*>(GlobalLock(hd));
        if (text) {
            std::string result(text);
            GlobalUnlock(hd);
            CloseClipboard();
            return result;
        }
        GlobalUnlock(hd);
    }

    CloseClipboard();
    return std::nullopt;
#else
    if (memoryClipboard_.empty()) return std::nullopt;
    return memoryClipboard_;
#endif
}

// =============================================================================
// HTML clipboard (Windows CF_HTML format)
// =============================================================================

bool Clipboard::setHtml(const std::string& html) {
#ifdef _WIN32
    // CF_HTML requires a specific header format
    static const UINT CF_HTML_FORMAT = RegisterClipboardFormatW(L"HTML Format");

    std::string wrapped =
        "Version:0.9\r\n"
        "StartHTML:00000097\r\n"
        "EndHTML:XXXXXXXXXX\r\n"
        "StartFragment:00000133\r\n"
        "EndFragment:XXXXXXXXXX\r\n"
        "<html><body>\r\n"
        "<!--StartFragment-->" + html + "<!--EndFragment-->\r\n"
        "</body></html>";

    if (!OpenClipboard(nullptr)) return false;
    // Also set plain text
    setText(html);  // Re-opens clipboard internally — close first
    CloseClipboard();

    if (!OpenClipboard(nullptr)) return false;
    SIZE_T bytes = wrapped.size() + 1;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hg) {
        void* ptr = GlobalLock(hg);
        if (ptr) {
            memcpy(ptr, wrapped.c_str(), bytes);
            GlobalUnlock(hg);
            SetClipboardData(CF_HTML_FORMAT, hg);
        } else {
            GlobalFree(hg);
        }
    }
    CloseClipboard();
    return true;
#else
    memoryHtml_ = html;
    return true;
#endif
}

std::optional<std::string> Clipboard::getHtml() {
#ifdef _WIN32
    static const UINT CF_HTML_FORMAT = RegisterClipboardFormatW(L"HTML Format");
    if (!OpenClipboard(nullptr)) return std::nullopt;
    HANDLE hd = GetClipboardData(CF_HTML_FORMAT);
    if (!hd) { CloseClipboard(); return std::nullopt; }
    const char* data = static_cast<const char*>(GlobalLock(hd));
    std::optional<std::string> result;
    if (data) result = std::string(data);
    GlobalUnlock(hd);
    CloseClipboard();
    return result;
#else
    if (memoryHtml_.empty()) return std::nullopt;
    return memoryHtml_;
#endif
}

// =============================================================================
// Image, Files, hasFormat, availableFormats, clear
// =============================================================================

bool Clipboard::setImage(const std::vector<uint8_t>& /*pngData*/) {
    // TODO: convert PNG to HBITMAP and set CF_BITMAP on Windows
    return false;
}

std::optional<std::vector<uint8_t>> Clipboard::getImage() {
    return std::nullopt;
}

bool Clipboard::setFiles(const std::vector<std::string>& /*paths*/) {
    // TODO: DROPFILES on Windows
    return false;
}

std::optional<std::vector<std::string>> Clipboard::getFiles() {
#ifdef _WIN32
    if (!OpenClipboard(nullptr)) return std::nullopt;
    HANDLE hd = GetClipboardData(CF_HDROP);
    if (!hd) { CloseClipboard(); return std::nullopt; }
    HDROP drop = static_cast<HDROP>(GlobalLock(hd));
    if (!drop) { CloseClipboard(); return std::nullopt; }

    std::vector<std::string> files;
    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        wchar_t buf[MAX_PATH];
        DragQueryFileW(drop, i, buf, MAX_PATH);
        int needed = WideCharToMultiByte(CP_UTF8, 0, buf, -1,
                                         nullptr, 0, nullptr, nullptr);
        std::string path(needed - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, buf, -1,
                            path.data(), needed, nullptr, nullptr);
        files.push_back(std::move(path));
    }
    GlobalUnlock(hd);
    CloseClipboard();
    return files;
#else
    return std::nullopt;
#endif
}

bool Clipboard::hasFormat(const char* format) {
#ifdef _WIN32
    UINT cf = 0;
    if (strcmp(format, "text/plain") == 0)  cf = CF_TEXT;
    else if (strcmp(format, "text/html") == 0) cf = RegisterClipboardFormatW(L"HTML Format");
    else if (strcmp(format, "image/png") == 0) cf = CF_BITMAP;
    else if (strcmp(format, "Files") == 0)     cf = CF_HDROP;
    if (cf == 0) return false;
    return IsClipboardFormatAvailable(cf) != 0;
#else
    (void)format;
    return false;
#endif
}

std::vector<std::string> Clipboard::availableFormats() {
    std::vector<std::string> formats;
#ifdef _WIN32
    if (IsClipboardFormatAvailable(CF_TEXT) || IsClipboardFormatAvailable(CF_UNICODETEXT))
        formats.push_back("text/plain");
    if (IsClipboardFormatAvailable(RegisterClipboardFormatW(L"HTML Format")))
        formats.push_back("text/html");
    if (IsClipboardFormatAvailable(CF_BITMAP) || IsClipboardFormatAvailable(CF_DIB))
        formats.push_back("image/png");
    if (IsClipboardFormatAvailable(CF_HDROP))
        formats.push_back("Files");
#endif
    return formats;
}

void Clipboard::clear() {
#ifdef _WIN32
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        CloseClipboard();
    }
#else
    memoryClipboard_.clear();
    memoryHtml_.clear();
#endif
}

} // namespace Zepra::Platform
