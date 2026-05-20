// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file windows_wgl.cpp
 * @brief NXRender Windows/WGL platform backend
 *
 * Windows equivalent of linux_x11.cpp.
 * Implements the Platform abstract interface using Win32 + WGL.
 *
 * Rules:
 *  - This file ONLY compiles on _WIN32.
 *  - No SDL, no GLFW, no external windowing library.
 *  - Linux code is never modified or included here.
 */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM, GET_Y_LPARAM
#include <GL/gl.h>

#include "platform.h"
#include "input/events.h"
#include "nxrender_cpp.h"

#include <string>
#include <iostream>
#include <cstring>

// Internal event dispatcher function defined in nxrender.cpp
namespace NXRender {
    void dispatchEvent(const Event& event);
}

namespace NXRender {

// =============================================================================
// Key mapping  (Win32 VK → NXRender KeyCode)
// =============================================================================

static KeyCode mapVKey(WPARAM vk) {
    // Letters (A-Z)
    if (vk >= 'A' && vk <= 'Z') return static_cast<KeyCode>(
        static_cast<int>(KeyCode::A) + (vk - 'A'));
    // Numbers (0-9)
    if (vk >= '0' && vk <= '9') return static_cast<KeyCode>(
        static_cast<int>(KeyCode::Num0) + (vk - '0'));

    switch (vk) {
        case VK_ESCAPE:    return KeyCode::Escape;
        case VK_RETURN:    return KeyCode::Enter;
        case VK_BACK:      return KeyCode::Backspace;
        case VK_TAB:       return KeyCode::Tab;
        case VK_SPACE:     return KeyCode::Space;
        case VK_LEFT:      return KeyCode::Left;
        case VK_RIGHT:     return KeyCode::Right;
        case VK_UP:        return KeyCode::Up;
        case VK_DOWN:      return KeyCode::Down;
        case VK_HOME:      return KeyCode::Home;
        case VK_END:       return KeyCode::End;
        case VK_PRIOR:     return KeyCode::PageUp;
        case VK_NEXT:      return KeyCode::PageDown;
        case VK_DELETE:    return KeyCode::Delete;
        case VK_INSERT:    return KeyCode::Insert;
        case VK_F1:        return KeyCode::F1;
        case VK_F2:        return KeyCode::F2;
        case VK_F3:        return KeyCode::F3;
        case VK_F4:        return KeyCode::F4;
        case VK_F5:        return KeyCode::F5;
        case VK_F6:        return KeyCode::F6;
        case VK_F7:        return KeyCode::F7;
        case VK_F8:        return KeyCode::F8;
        case VK_F9:        return KeyCode::F9;
        case VK_F10:       return KeyCode::F10;
        case VK_F11:       return KeyCode::F11;
        case VK_F12:       return KeyCode::F12;
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:    return KeyCode::Shift;
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:  return KeyCode::Ctrl;
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:     return KeyCode::Alt;
        case VK_LWIN:
        case VK_RWIN:      return KeyCode::Meta;
        case VK_OEM_COMMA:   return KeyCode::Comma;
        case VK_OEM_PERIOD:  return KeyCode::Period;
        case VK_OEM_2:       return KeyCode::Slash;
        case VK_OEM_5:       return KeyCode::Backslash;
        case VK_OEM_1:       return KeyCode::Semicolon;
        case VK_OEM_7:       return KeyCode::Quote;
        case VK_OEM_4:       return KeyCode::LeftBracket;
        case VK_OEM_6:       return KeyCode::RightBracket;
        case VK_OEM_MINUS:   return KeyCode::Minus;
        case VK_OEM_PLUS:    return KeyCode::Equals;
        case VK_OEM_3:       return KeyCode::Backtick;
        default:
            return KeyCode::Unknown;
    }
}

static MouseButton mapMouseButton(UINT msg) {
    switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: return MouseButton::Left;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: return MouseButton::Right;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: return MouseButton::Middle;
        default: return MouseButton::None;
    }
}

static Modifiers getModifiers() {
    Modifiers m;
    m.shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    m.ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    m.alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
    m.meta  = (GetKeyState(VK_LWIN)    & 0x8000) != 0 ||
              (GetKeyState(VK_RWIN)    & 0x8000) != 0;
    return m;
}

// =============================================================================
// Win32 Window Procedure
// =============================================================================

// Forward declarations
static HWND  g_hwnd  = nullptr;
static bool  g_running = true;

static LRESULT CALLBACK ZepraWndProc(HWND hwnd, UINT msg,
                                      WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        // ---- Window sizing --------------------------------------------------
        case WM_SIZE: {
            Event event;
            event.type = EventType::Resize;
            event.window.width  = LOWORD(lParam);
            event.window.height = HIWORD(lParam);
            NXRender::dispatchEvent(event);
            return 0;
        }

        // ---- Close / Quit ---------------------------------------------------
        case WM_CLOSE: {
            Event event;
            event.type = EventType::Close;
            NXRender::dispatchEvent(event);
            g_running = false;
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        }

        // ---- Focus ----------------------------------------------------------
        case WM_SETFOCUS: {
            Event event;
            event.type = EventType::Focus;
            NXRender::dispatchEvent(event);
            return 0;
        }

        case WM_KILLFOCUS: {
            Event event;
            event.type = EventType::Blur;
            NXRender::dispatchEvent(event);
            return 0;
        }

        // ---- Keyboard -------------------------------------------------------
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            Event event;
            event.type      = EventType::KeyDown;
            event.key.key   = mapVKey(wParam);
            event.key.repeat = (lParam & (1 << 30)) != 0;
            event.key.modifiers = getModifiers();
            NXRender::dispatchEvent(event);
            return 0;
        }

        case WM_KEYUP:
        case WM_SYSKEYUP: {
            Event event;
            event.type      = EventType::KeyUp;
            event.key.key   = mapVKey(wParam);
            event.key.repeat = false;
            event.key.modifiers = getModifiers();
            NXRender::dispatchEvent(event);
            return 0;
        }

        case WM_CHAR: {
            if (wParam >= 32) {
                Event event;
                event.type = EventType::TextInput;
                // Convert codepoint to UTF-8 string
                char buf[4];
                if (wParam < 0x80) {
                    buf[0] = static_cast<char>(wParam);
                    event.textInput = std::string(buf, 1);
                } else if (wParam < 0x800) {
                    buf[0] = static_cast<char>(0xC0 | (wParam >> 6));
                    buf[1] = static_cast<char>(0x80 | (wParam & 0x3F));
                    event.textInput = std::string(buf, 2);
                } else {
                    buf[0] = static_cast<char>(0xE0 | (wParam >> 12));
                    buf[1] = static_cast<char>(0x80 | ((wParam >> 6) & 0x3F));
                    buf[2] = static_cast<char>(0x80 | (wParam & 0x3F));
                    event.textInput = std::string(buf, 3);
                }
                NXRender::dispatchEvent(event);
            }
            return 0;
        }

        // ---- Mouse ----------------------------------------------------------
        case WM_MOUSEMOVE: {
            Event event;
            event.type    = EventType::MouseMove;
            event.mouse.x = static_cast<float>(GET_X_LPARAM(lParam));
            event.mouse.y = static_cast<float>(GET_Y_LPARAM(lParam));
            event.mouse.modifiers = getModifiers();
            NXRender::dispatchEvent(event);
            return 0;
        }

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN: {
            Event event;
            event.type         = EventType::MouseDown;
            event.mouse.x      = static_cast<float>(GET_X_LPARAM(lParam));
            event.mouse.y      = static_cast<float>(GET_Y_LPARAM(lParam));
            event.mouse.button  = mapMouseButton(msg);
            event.mouse.modifiers = getModifiers();
            NXRender::dispatchEvent(event);
            SetCapture(hwnd);
            return 0;
        }

        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP: {
            Event event;
            event.type         = EventType::MouseUp;
            event.mouse.x      = static_cast<float>(GET_X_LPARAM(lParam));
            event.mouse.y      = static_cast<float>(GET_Y_LPARAM(lParam));
            event.mouse.button  = mapMouseButton(msg);
            event.mouse.modifiers = getModifiers();
            NXRender::dispatchEvent(event);
            ReleaseCapture();
            return 0;
        }

        case WM_MOUSEWHEEL: {
            Event event;
            event.type = EventType::MouseWheel;
            event.mouse.x = static_cast<float>(GET_X_LPARAM(lParam));
            event.mouse.y = static_cast<float>(GET_Y_LPARAM(lParam));
            event.mouse.wheelDelta = static_cast<float>(
                GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
            event.mouse.modifiers = getModifiers();
            NXRender::dispatchEvent(event);
            return 0;
        }

        // ---- File drop (WM_DROPFILES) ---------------------------------------
        case WM_DROPFILES: {
            HDROP hDrop = reinterpret_cast<HDROP>(wParam);
            UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            if (count > 0) {
                Event event;
                event.type = EventType::FileDrop;
                POINT pt;
                DragQueryPoint(hDrop, &pt);
                event.dropX = static_cast<float>(pt.x);
                event.dropY = static_cast<float>(pt.y);
                for (UINT i = 0; i < count; i++) {
                    wchar_t buf[MAX_PATH];
                    DragQueryFileW(hDrop, i, buf, MAX_PATH);
                    // Convert wide to narrow (UTF-8 simplified: ASCII subset)
                    std::string path;
                    for (int j = 0; buf[j]; j++)
                        path += static_cast<char>(buf[j] & 0x7F);
                    event.droppedFiles.push_back(path);
                }
                NXRender::dispatchEvent(event);
            }
            DragFinish(hDrop);
            return 0;
        }

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// =============================================================================
// WindowsWGLPlatform : public Platform
// =============================================================================

class WindowsWGLPlatform : public Platform {
public:
    WindowsWGLPlatform() = default;

    ~WindowsWGLPlatform() override { shutdown(); }

    // -------------------------------------------------------------------------
    bool init(int width, int height, const std::string& title) override {
        // Register window class (once per process)
        HINSTANCE hInstance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        wc.lpfnWndProc   = ZepraWndProc;
        wc.hInstance     = hInstance;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"ZepraWGLWindow";
        if (!RegisterClassExW(&wc)) {
            if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        }

        // Create window
        DWORD style = WS_OVERLAPPEDWINDOW;
        RECT  rect  = { 0, 0, width, height };
        AdjustWindowRect(&rect, style, FALSE);

        std::wstring wtitle(title.begin(), title.end());
        hwnd_ = CreateWindowExW(
            WS_EX_ACCEPTFILES,   // Enable drag-and-drop
            L"ZepraWGLWindow", wtitle.c_str(),
            style,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top,
            nullptr, nullptr, hInstance, nullptr
        );
        if (!hwnd_) return false;

        g_hwnd = hwnd_;

        // Create WGL context
        hdc_ = GetDC(hwnd_);
        if (!hdc_) return false;

        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize      = sizeof(pfd);
        pfd.nVersion   = 1;
        pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = 24;
        pfd.cStencilBits = 8;
        pfd.iLayerType = PFD_MAIN_PLANE;

        int pf = ChoosePixelFormat(hdc_, &pfd);
        if (!pf || !SetPixelFormat(hdc_, pf, &pfd)) return false;

        hglrc_ = wglCreateContext(hdc_);
        if (!hglrc_) return false;

        if (!wglMakeCurrent(hdc_, hglrc_)) return false;

        // Create cursors
        cursorArrow_ = LoadCursor(nullptr, IDC_ARROW);
        cursorHand_  = LoadCursor(nullptr, IDC_HAND);
        cursorText_  = LoadCursor(nullptr, IDC_IBEAM);
        cursorCross_ = LoadCursor(nullptr, IDC_CROSS);

        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);

        std::cout << "[Platform] Win32/WGL Window created ("
                  << width << "x" << height << ")" << std::endl;
        return true;
    }

    // -------------------------------------------------------------------------
    void shutdown() override {
        if (hglrc_) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(hglrc_);
            hglrc_ = nullptr;
        }
        if (hdc_ && hwnd_) {
            ReleaseDC(hwnd_, hdc_);
            hdc_ = nullptr;
        }
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
            g_hwnd = nullptr;
        }
    }

    // -------------------------------------------------------------------------
    void pollEvents() override {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = false;
                return;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // -------------------------------------------------------------------------
    void swapBuffers() override {
        if (hdc_) ::SwapBuffers(hdc_);
    }

    // -------------------------------------------------------------------------
    void setTitle(const std::string& title) override {
        if (hwnd_) {
            std::wstring wtitle(title.begin(), title.end());
            SetWindowTextW(hwnd_, wtitle.c_str());
        }
    }

    // -------------------------------------------------------------------------
    void setCursor(CursorType type) override {
        if (!hwnd_) return;
        HCURSOR c = cursorArrow_;
        switch (type) {
            case CursorType::Arrow:     c = cursorArrow_; break;
            case CursorType::Hand:      c = cursorHand_;  break;
            case CursorType::Text:      c = cursorText_;  break;
            case CursorType::Crosshair: c = cursorCross_; break;
        }
        SetCursor(c);
    }

private:
    HWND   hwnd_   = nullptr;
    HDC    hdc_    = nullptr;
    HGLRC  hglrc_  = nullptr;
    HCURSOR cursorArrow_  = nullptr;
    HCURSOR cursorHand_   = nullptr;
    HCURSOR cursorText_   = nullptr;
    HCURSOR cursorCross_  = nullptr;
};

// =============================================================================
// Factory (matches linux_x11.cpp signature exactly)
// =============================================================================

Platform* createPlatform() {
    return new WindowsWGLPlatform();
}

} // namespace NXRender

#endif // _WIN32
