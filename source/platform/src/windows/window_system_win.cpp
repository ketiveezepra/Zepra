// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file window_system_win.cpp
 * @brief Windows implementation of Zepra::Platform::WindowSystem
 *
 * Uses pure Win32 (no SDL, no GLFW, no Qt).
 * This file is compiled ONLY on _WIN32.
 * Linux/NeolyxOS code is never modified.
 */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// GET_X_LPARAM / GET_Y_LPARAM
#include <windowsx.h>

#include "platform/window_system.hpp"

#include <unordered_map>
#include <string>
#include <stdexcept>

namespace Zepra::Platform {

// =============================================================================
// Internal Win32 window state
// =============================================================================

struct WindowHandle {
    HWND  hwnd   = nullptr;
    int   id     = 0;
    bool  focused = false;

    WindowSystem::ResizeCallback onResize;
    WindowSystem::CloseCallback  onClose;
    WindowSystem::FocusCallback  onFocus;
};

static std::unordered_map<HWND, WindowHandle*> g_handles;
static int g_nextWindowId = 1;
static constexpr wchar_t kClassName[] = L"ZepraWindowClass";
static bool g_classRegistered = false;

// =============================================================================
// Window Procedure
// =============================================================================

static LRESULT CALLBACK ZepraWndProc(HWND hwnd, UINT msg,
                                      WPARAM wParam, LPARAM lParam) {
    auto it = g_handles.find(hwnd);
    WindowHandle* wh = (it != g_handles.end()) ? it->second : nullptr;

    switch (msg) {
        case WM_SIZE: {
            if (wh && wh->onResize) {
                wh->onResize(LOWORD(lParam), HIWORD(lParam));
            }
            return 0;
        }

        case WM_CLOSE: {
            if (wh && wh->onClose) {
                wh->onClose();
            }
            // Default: destroy the window
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY: {
            g_handles.erase(hwnd);
            PostQuitMessage(0);
            return 0;
        }

        case WM_SETFOCUS: {
            if (wh) {
                wh->focused = true;
                if (wh->onFocus) wh->onFocus(true);
            }
            return 0;
        }

        case WM_KILLFOCUS: {
            if (wh) {
                wh->focused = false;
                if (wh->onFocus) wh->onFocus(false);
            }
            return 0;
        }

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// =============================================================================
// Register the window class once
// =============================================================================

static void ensureClassRegistered() {
    if (g_classRegistered) return;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc   = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = ZepraWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);   // ignore if already registered
    g_classRegistered = true;
}

// =============================================================================
// Factory (called by WindowSystem::create())
// =============================================================================

std::unique_ptr<WindowSystem> WindowSystem::create() {
    return std::make_unique<WindowSystemWin>();
}

// =============================================================================
// WindowSystemWin implementation
// =============================================================================

WindowHandle* WindowSystemWin::createWindow(const WindowConfig& config) {
    ensureClassRegistered();

    HINSTANCE hInst = GetModuleHandleW(nullptr);

    DWORD style = WS_OVERLAPPEDWINDOW;
    if (!config.resizable)  style &= ~WS_THICKFRAME;
    if (!config.decorated)  style  = WS_POPUP;
    if (config.fullscreen)  style  = WS_POPUP | WS_VISIBLE;

    RECT rect = { 0, 0, config.width, config.height };
    AdjustWindowRect(&rect, style, FALSE);

    int posX = (config.x < 0) ? CW_USEDEFAULT : config.x;
    int posY = (config.y < 0) ? CW_USEDEFAULT : config.y;

    std::wstring wtitle(config.title.begin(), config.title.end());
    HWND hwnd = CreateWindowExW(
        0, kClassName, wtitle.c_str(),
        style,
        posX, posY,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInst, nullptr
    );
    if (!hwnd) return nullptr;

    auto* wh  = new WindowHandle();
    wh->hwnd  = hwnd;
    wh->id    = g_nextWindowId++;
    g_handles[hwnd] = wh;

    if (config.maximized) ShowWindow(hwnd, SW_MAXIMIZE);
    else if (config.fullscreen) {
        // Borderless fullscreen
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(hwnd, HWND_TOP, 0, 0, sw, sh, SWP_FRAMECHANGED);
        ShowWindow(hwnd, SW_SHOW);
    }

    return wh;
}

void WindowSystemWin::destroyWindow(WindowHandle* window) {
    if (!window) return;
    if (window->hwnd) {
        g_handles.erase(window->hwnd);
        DestroyWindow(window->hwnd);
    }
    delete window;
}

void WindowSystemWin::showWindow(WindowHandle* window) {
    if (window && window->hwnd) ShowWindow(window->hwnd, SW_SHOW);
}

void WindowSystemWin::hideWindow(WindowHandle* window) {
    if (window && window->hwnd) ShowWindow(window->hwnd, SW_HIDE);
}

void WindowSystemWin::minimizeWindow(WindowHandle* window) {
    if (window && window->hwnd) ShowWindow(window->hwnd, SW_MINIMIZE);
}

void WindowSystemWin::maximizeWindow(WindowHandle* window) {
    if (window && window->hwnd) ShowWindow(window->hwnd, SW_MAXIMIZE);
}

void WindowSystemWin::restoreWindow(WindowHandle* window) {
    if (window && window->hwnd) ShowWindow(window->hwnd, SW_RESTORE);
}

void WindowSystemWin::setFullscreen(WindowHandle* window, bool fullscreen) {
    if (!window || !window->hwnd) return;
    if (fullscreen) {
        DWORD style = WS_POPUP | WS_VISIBLE;
        SetWindowLongW(window->hwnd, GWL_STYLE, style);
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(window->hwnd, HWND_TOP, 0, 0, sw, sh, SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(window->hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
        SetWindowPos(window->hwnd, nullptr, 100, 100, 1024, 768,
                     SWP_FRAMECHANGED | SWP_NOZORDER);
    }
}

void WindowSystemWin::setTitle(WindowHandle* window, const std::string& title) {
    if (!window || !window->hwnd) return;
    std::wstring wt(title.begin(), title.end());
    SetWindowTextW(window->hwnd, wt.c_str());
}

void WindowSystemWin::setSize(WindowHandle* window, int width, int height) {
    if (!window || !window->hwnd) return;
    RECT r;
    GetWindowRect(window->hwnd, &r);
    MoveWindow(window->hwnd, r.left, r.top, width, height, TRUE);
}

void WindowSystemWin::setPosition(WindowHandle* window, int x, int y) {
    if (!window || !window->hwnd) return;
    RECT r;
    GetWindowRect(window->hwnd, &r);
    MoveWindow(window->hwnd, x, y, r.right - r.left, r.bottom - r.top, TRUE);
}

void WindowSystemWin::getSize(WindowHandle* window, int& width, int& height) {
    width = height = 0;
    if (!window || !window->hwnd) return;
    RECT r;
    GetClientRect(window->hwnd, &r);
    width  = r.right - r.left;
    height = r.bottom - r.top;
}

void WindowSystemWin::getPosition(WindowHandle* window, int& x, int& y) {
    x = y = 0;
    if (!window || !window->hwnd) return;
    RECT r;
    GetWindowRect(window->hwnd, &r);
    x = r.left;
    y = r.top;
}

void WindowSystemWin::focusWindow(WindowHandle* window) {
    if (window && window->hwnd) SetForegroundWindow(window->hwnd);
}

bool WindowSystemWin::isFocused(WindowHandle* window) {
    if (!window || !window->hwnd) return false;
    return window->focused;
}

void* WindowSystemWin::getNativeHandle(WindowHandle* window) {
    return window ? window->hwnd : nullptr;
}

void WindowSystemWin::setOnResize(WindowHandle* window, ResizeCallback callback) {
    if (window) window->onResize = std::move(callback);
}

void WindowSystemWin::setOnClose(WindowHandle* window, CloseCallback callback) {
    if (window) window->onClose = std::move(callback);
}

void WindowSystemWin::setOnFocus(WindowHandle* window, FocusCallback callback) {
    if (window) window->onFocus = std::move(callback);
}

} // namespace Zepra::Platform

#endif // _WIN32
