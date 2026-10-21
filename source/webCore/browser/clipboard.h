// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * clipboard.h - Native Clipboard Implementation for ZepraBrowser
 * 
 * Platform-specific clipboard support:
 *   Linux/NeolyxOS: X11 selections (CLIPBOARD + PRIMARY)
 *   Windows:        Win32 clipboard API (OpenClipboard / SetClipboardData)
 */

#ifndef ZEPRA_CLIPBOARD_H
#define ZEPRA_CLIPBOARD_H

#include <string>
#include <cstring>

// ============================================================================
// Platform: Linux / NeolyxOS  (X11 selections)
// ============================================================================
#ifndef _WIN32

#include <X11/Xlib.h>
#include <X11/Xatom.h>

namespace ZepraBrowser {

class Clipboard {
public:
    static Clipboard& instance() {
        static Clipboard inst;
        return inst;
    }
    
    // Initialize with X display
    void init(Display* display, Window window) {
        display_ = display;
        window_ = window;
        
        if (display_) {
            clipboard_ = XInternAtom(display_, "CLIPBOARD", False);
            targets_ = XInternAtom(display_, "TARGETS", False);
            utf8_ = XInternAtom(display_, "UTF8_STRING", False);
            xselData_ = XInternAtom(display_, "XSEL_DATA", False);
        }
    }
    
    // Copy text to clipboard
    bool copy(const std::string& text) {
        if (!display_ || !window_) return false;
        
        copiedText_ = text;
        
        XSetSelectionOwner(display_, clipboard_, window_, CurrentTime);
        XSetSelectionOwner(display_, XA_PRIMARY, window_, CurrentTime);
        XFlush(display_);
        return true;
    }
    
    // Paste from clipboard
    std::string paste() {
        if (!display_ || !window_) return "";
        
        Window owner = XGetSelectionOwner(display_, clipboard_);
        if (owner == window_) {
            return copiedText_;
        }
        
        if (owner == None) {
            return "";
        }
        
        XConvertSelection(display_, clipboard_, utf8_, xselData_, window_, CurrentTime);
        XFlush(display_);
        
        XEvent event;
        for (int i = 0; i < 50; i++) {
            if (XCheckTypedWindowEvent(display_, window_, SelectionNotify, &event)) {
                if (event.xselection.selection == clipboard_ && 
                    event.xselection.property != None) {
                    return getSelectionData(event.xselection.property);
                }
                return "";
            }
            usleep(10000);
        }
        
        return "";
    }
    
    // Handle X11 SelectionRequest events
    void handleSelectionRequest(XSelectionRequestEvent& event) {
        if (!display_) return;
        
        // ICCCM: obsolete clients send property None and expect the target name
        Atom property = (event.property != None) ? event.property : event.target;

        XSelectionEvent response = {};
        response.type = SelectionNotify;
        response.display = event.display;
        response.requestor = event.requestor;
        response.selection = event.selection;
        response.target = event.target;
        response.time = event.time;
        response.property = None;
        
        if (event.target == targets_) {
            Atom targets[] = { targets_, utf8_, XA_STRING };
            XChangeProperty(display_, event.requestor, property,
                           XA_ATOM, 32, PropModeReplace,
                           (unsigned char*)targets, 3);
            response.property = property;
        }
        else if (event.target == utf8_ || event.target == XA_STRING) {
            XChangeProperty(display_, event.requestor, property,
                           event.target, 8, PropModeReplace,
                           (unsigned char*)copiedText_.c_str(), copiedText_.length());
            response.property = property;
        }
        
        XSendEvent(display_, event.requestor, False, 0, (XEvent*)&response);
        XFlush(display_);
    }
    
    const std::string& getCopiedText() const { return copiedText_; }
    
private:
    Clipboard() = default;
    
    std::string getSelectionData(Atom property) {
        Atom actual_type;
        int actual_format;
        unsigned long nitems, bytes_after;
        unsigned char* data = nullptr;
        
        if (XGetWindowProperty(display_, window_, property, 0, ~0L, True,
                              AnyPropertyType, &actual_type, &actual_format,
                              &nitems, &bytes_after, &data) == Success) {
            if (data && nitems > 0) {
                std::string result((char*)data, nitems);
                XFree(data);
                return result;
            }
            if (data) XFree(data);
        }
        return "";
    }
    
    Display* display_ = nullptr;
    Window window_ = 0;
    Atom clipboard_ = 0;
    Atom targets_ = 0;
    Atom utf8_ = 0;
    Atom xselData_ = 0;
    std::string copiedText_;
};

} // namespace ZepraBrowser

// ============================================================================
// Platform: Windows  (Win32 clipboard API)
// ============================================================================
#else  // _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ZepraBrowser {

class Clipboard {
public:
    static Clipboard& instance() {
        static Clipboard inst;
        return inst;
    }
    
    // Windows doesn't need X11-style init; store HWND for OpenClipboard
    void init(void* /*display*/ = nullptr, void* hwnd = nullptr) {
        hwnd_ = static_cast<HWND>(hwnd);
    }
    
    bool copy(const std::string& text) {
        if (!OpenClipboard(hwnd_)) return false;
        EmptyClipboard();
        
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
        if (!hMem) { CloseClipboard(); return false; }
        
        char* dst = static_cast<char*>(GlobalLock(hMem));
        std::memcpy(dst, text.c_str(), text.size() + 1);
        GlobalUnlock(hMem);
        
        SetClipboardData(CF_TEXT, hMem);
        CloseClipboard();
        
        copiedText_ = text;
        return true;
    }
    
    std::string paste() {
        if (!OpenClipboard(hwnd_)) return "";
        
        HANDLE hData = GetClipboardData(CF_TEXT);
        if (!hData) { CloseClipboard(); return ""; }
        
        const char* src = static_cast<const char*>(GlobalLock(hData));
        std::string result = src ? src : "";
        GlobalUnlock(hData);
        CloseClipboard();
        return result;
    }
    
    const std::string& getCopiedText() const { return copiedText_; }
    
private:
    Clipboard() = default;
    HWND hwnd_ = nullptr;
    std::string copiedText_;
};

} // namespace ZepraBrowser

#endif // _WIN32

#endif // ZEPRA_CLIPBOARD_H
