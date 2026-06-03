// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "input/clipboard.h"
#include <algorithm>

#ifdef __linux__
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <cstring>
#include <iostream>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstring>
#endif

namespace NXRender {
namespace Input {

Clipboard& Clipboard::instance() {
    static Clipboard clip;
    return clip;
}

bool Clipboard::init(void* display, unsigned long window) {
#ifdef __linux__
    display_ = display;
    window_ = window;

    Display* dpy = static_cast<Display*>(display);
    atomClipboard_ = XInternAtom(dpy, "CLIPBOARD", False);
    atomTargets_ = XInternAtom(dpy, "TARGETS", False);
    atomUtf8_ = XInternAtom(dpy, "UTF8_STRING", False);
    atomNxClip_ = XInternAtom(dpy, "NX_CLIPBOARD", False);

    return true;
#elif defined(_WIN32)
    // Win32 clipboard doesn't need a display/window — it's global.
    // Store params for API compatibility but ignore them.
    (void)display;
    (void)window;
    return true;
#else
    (void)display;
    (void)window;
    return false;
#endif
}

void Clipboard::shutdown() {
    display_ = nullptr;
    window_ = 0;
    ownsClipboard_ = false;
    ownsPrimary_ = false;
    clipboardText_.clear();
    primaryText_.clear();
}

void Clipboard::setText(const std::string& text, ClipboardSelection sel) {
#ifdef __linux__
    if (!display_) return;
    Display* dpy = static_cast<Display*>(display_);

    Atom selection = (sel == ClipboardSelection::Primary)
                     ? XA_PRIMARY
                     : static_cast<Atom>(atomClipboard_);

    XSetSelectionOwner(dpy, selection, static_cast<Window>(window_), CurrentTime);

    Window owner = XGetSelectionOwner(dpy, selection);
    if (owner == static_cast<Window>(window_)) {
        if (sel == ClipboardSelection::Clipboard) {
            clipboardText_ = text;
            ownsClipboard_ = true;
        } else {
            primaryText_ = text;
            ownsPrimary_ = true;
        }
    }
#elif defined(_WIN32)
    // Win32 has no PRIMARY selection; store it in-memory for consistency
    if (sel == ClipboardSelection::Primary) {
        primaryText_ = text;
        ownsPrimary_ = true;
        return;
    }

    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();

    // Convert UTF-8 to wide string for CF_UNICODETEXT
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                   static_cast<int>(text.size()), nullptr, 0);
    if (wlen <= 0) { CloseClipboard(); return; }

    SIZE_T bytes = static_cast<SIZE_T>(wlen + 1) * sizeof(wchar_t);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hg) { CloseClipboard(); return; }

    wchar_t* dst = static_cast<wchar_t*>(GlobalLock(hg));
    if (!dst) { GlobalFree(hg); CloseClipboard(); return; }

    MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                        static_cast<int>(text.size()), dst, wlen);
    dst[wlen] = L'\0';
    GlobalUnlock(hg);

    SetClipboardData(CF_UNICODETEXT, hg);
    CloseClipboard();

    clipboardText_ = text;
    ownsClipboard_ = true;
#endif
}

void Clipboard::getText(std::function<void(const std::string&)> callback, ClipboardSelection sel) {
#ifdef __linux__
    if (!display_ || !callback) return;

    // If we own it, return directly
    if (sel == ClipboardSelection::Clipboard && ownsClipboard_) {
        callback(clipboardText_);
        return;
    }
    if (sel == ClipboardSelection::Primary && ownsPrimary_) {
        callback(primaryText_);
        return;
    }

    Display* dpy = static_cast<Display*>(display_);
    Atom selection = (sel == ClipboardSelection::Primary)
                     ? XA_PRIMARY
                     : static_cast<Atom>(atomClipboard_);

    pendingCallback_ = std::move(callback);
    pendingSelection_ = sel;

    XConvertSelection(dpy, selection,
                      static_cast<Atom>(atomUtf8_),
                      static_cast<Atom>(atomNxClip_),
                      static_cast<Window>(window_),
                      CurrentTime);
    XFlush(dpy);
#elif defined(_WIN32)
    if (!callback) return;

    // PRIMARY — in-memory only on Windows
    if (sel == ClipboardSelection::Primary) {
        callback(ownsPrimary_ ? primaryText_ : std::string());
        return;
    }

    // Win32 clipboard read is synchronous — no event roundtrip needed
    if (!OpenClipboard(nullptr)) { callback(""); return; }

    HANDLE hd = GetClipboardData(CF_UNICODETEXT);
    if (!hd) { CloseClipboard(); callback(""); return; }

    const wchar_t* wide = static_cast<const wchar_t*>(GlobalLock(hd));
    if (!wide) { GlobalUnlock(hd); CloseClipboard(); callback(""); return; }

    int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1,
                                     nullptr, 0, nullptr, nullptr);
    std::string result;
    if (needed > 1) {
        result.resize(static_cast<size_t>(needed - 1));
        WideCharToMultiByte(CP_UTF8, 0, wide, -1,
                            &result[0], needed, nullptr, nullptr);
    }
    GlobalUnlock(hd);
    CloseClipboard();
    callback(result);
#else
    if (callback) callback("");
#endif
}

const std::string& Clipboard::ownedText(ClipboardSelection sel) const {
    static const std::string empty;
    if (sel == ClipboardSelection::Clipboard) return ownsClipboard_ ? clipboardText_ : empty;
    return ownsPrimary_ ? primaryText_ : empty;
}

bool Clipboard::ownsSelection(ClipboardSelection sel) const {
    return (sel == ClipboardSelection::Clipboard) ? ownsClipboard_ : ownsPrimary_;
}

void Clipboard::handleSelectionRequest(void* event) {
#ifdef __linux__
    XSelectionRequestEvent* req = static_cast<XSelectionRequestEvent*>(event);
    Display* dpy = static_cast<Display*>(display_);

    XSelectionEvent response;
    response.type = SelectionNotify;
    response.requestor = req->requestor;
    response.selection = req->selection;
    response.target = req->target;
    response.time = req->time;
    response.property = None;

    const std::string* data = nullptr;
    if (req->selection == static_cast<Atom>(atomClipboard_) && ownsClipboard_) {
        data = &clipboardText_;
    } else if (req->selection == XA_PRIMARY && ownsPrimary_) {
        data = &primaryText_;
    }

    if (data) {
        if (req->target == static_cast<Atom>(atomTargets_)) {
            // Report supported target types
            Atom targets[] = { static_cast<Atom>(atomUtf8_), XA_STRING };
            XChangeProperty(dpy, req->requestor, req->property,
                            XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<unsigned char*>(targets), 2);
            response.property = req->property;
        } else if (req->target == static_cast<Atom>(atomUtf8_) || req->target == XA_STRING) {
            XChangeProperty(dpy, req->requestor, req->property,
                            req->target, 8, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(data->c_str()),
                            static_cast<int>(data->size()));
            response.property = req->property;
        }
    }

    XSendEvent(dpy, req->requestor, False, 0, reinterpret_cast<XEvent*>(&response));
    XFlush(dpy);
#endif
}

void Clipboard::handleSelectionNotify(void* event) {
#ifdef __linux__
    XSelectionEvent* sel = static_cast<XSelectionEvent*>(event);
    Display* dpy = static_cast<Display*>(display_);

    if (sel->property == None) {
        if (pendingCallback_) {
            pendingCallback_("");
            pendingCallback_ = nullptr;
        }
        return;
    }

    Atom actualType;
    int actualFormat;
    unsigned long items, bytesAfter;
    unsigned char* data = nullptr;

    XGetWindowProperty(dpy, static_cast<Window>(window_),
                       static_cast<Atom>(atomNxClip_),
                       0, 1024 * 1024, True,
                       AnyPropertyType,
                       &actualType, &actualFormat,
                       &items, &bytesAfter, &data);

    std::string text;
    if (data && items > 0) {
        text.assign(reinterpret_cast<char*>(data), items);
    }
    if (data) XFree(data);

    if (pendingCallback_) {
        pendingCallback_(text);
        pendingCallback_ = nullptr;
    }
#endif
}

void Clipboard::handleSelectionClear(void* event) {
#ifdef __linux__
    XSelectionClearEvent* clear = static_cast<XSelectionClearEvent*>(event);

    if (clear->selection == static_cast<Atom>(atomClipboard_)) {
        ownsClipboard_ = false;
    } else if (clear->selection == XA_PRIMARY) {
        ownsPrimary_ = false;
    }
#endif
}

} // namespace Input
} // namespace NXRender
