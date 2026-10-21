// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "platform/display_info.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>

#ifdef __linux__
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#endif

namespace NXRender {

DisplayInfo& DisplayInfo::instance() {
    static DisplayInfo info;
    return info;
}

void DisplayInfo::queryFromOS() {
#ifdef __linux__
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        std::cerr << "[DisplayInfo] Failed to open X11 display, using defaults" << std::endl;
        metrics_.screenWidth = 1920;
        metrics_.screenHeight = 1080;
        metrics_.dpi = 96.0f;
        metrics_.devicePixelRatio = 1.0f;
        return;
    }

    int screen = DefaultScreen(display);
    metrics_.screenWidth = DisplayWidth(display, screen);
    metrics_.screenHeight = DisplayHeight(display, screen);

    // Compute DPI from physical screen dimensions
    float xftDpi = 0.0f;
    {
        int widthMm = DisplayWidthMM(display, screen);
        if (widthMm > 0) {
            xftDpi = static_cast<float>(metrics_.screenWidth) * 25.4f / static_cast<float>(widthMm);
        } else {
            xftDpi = 96.0f;
        }
    }

    // Fallback: compute DPI from physical dimensions
    if (xftDpi <= 0.0f) {
        int widthMm = DisplayWidthMM(display, screen);
        if (widthMm > 0) {
            xftDpi = static_cast<float>(metrics_.screenWidth) * 25.4f / static_cast<float>(widthMm);
        } else {
            xftDpi = 96.0f;
        }
    }

    metrics_.dpi = xftDpi;
    metrics_.devicePixelRatio = xftDpi / 96.0f;

    // Clamp DPR to sane range
    if (metrics_.devicePixelRatio < 0.5f) metrics_.devicePixelRatio = 1.0f;
    if (metrics_.devicePixelRatio > 4.0f) metrics_.devicePixelRatio = 4.0f;

    // Try XRandR for more accurate primary monitor info
    int rrMajor = 0, rrMinor = 0;
    if (XRRQueryVersion(display, &rrMajor, &rrMinor) && (rrMajor > 1 || (rrMajor == 1 && rrMinor >= 2))) {
        XRRScreenResources* resources = XRRGetScreenResources(display, DefaultRootWindow(display));
        if (resources) {
            RROutput primary = XRRGetOutputPrimary(display, DefaultRootWindow(display));
            if (primary) {
                XRROutputInfo* outputInfo = XRRGetOutputInfo(display, resources, primary);
                if (outputInfo && outputInfo->crtc) {
                    XRRCrtcInfo* crtcInfo = XRRGetCrtcInfo(display, resources, outputInfo->crtc);
                    if (crtcInfo) {
                        metrics_.screenWidth = static_cast<int>(crtcInfo->width);
                        metrics_.screenHeight = static_cast<int>(crtcInfo->height);

                        // Recompute DPR with more accurate screen size if Xft.dpi wasn't set
                        if (outputInfo->mm_width > 0 && xftDpi <= 0.0f) {
                            float physDpi = static_cast<float>(crtcInfo->width) * 25.4f / 
                                           static_cast<float>(outputInfo->mm_width);
                            metrics_.dpi = physDpi;
                            metrics_.devicePixelRatio = physDpi / 96.0f;
                            if (metrics_.devicePixelRatio < 0.5f) metrics_.devicePixelRatio = 1.0f;
                            if (metrics_.devicePixelRatio > 4.0f) metrics_.devicePixelRatio = 4.0f;
                        }

                        XRRFreeCrtcInfo(crtcInfo);
                    }
                }
                if (outputInfo) XRRFreeOutputInfo(outputInfo);
            }
            XRRFreeScreenResources(resources);
        }
    }

    XCloseDisplay(display);

    std::cout << "[DisplayInfo] Screen: " << metrics_.screenWidth << "x" << metrics_.screenHeight
              << " DPI: " << metrics_.dpi
              << " DPR: " << metrics_.devicePixelRatio << std::endl;
#elif defined(_WIN32)
    // Win32: query display metrics via system APIs
    metrics_.screenWidth = GetSystemMetrics(SM_CXSCREEN);
    metrics_.screenHeight = GetSystemMetrics(SM_CYSCREEN);

    // GetDpiForSystem() requires Windows 10 1607+, use fallback if unavailable
    HDC hdc = GetDC(nullptr);
    if (hdc) {
        metrics_.dpi = static_cast<float>(GetDeviceCaps(hdc, LOGPIXELSX));
        ReleaseDC(nullptr, hdc);
    } else {
        metrics_.dpi = 96.0f;
    }

    metrics_.devicePixelRatio = metrics_.dpi / 96.0f;
    if (metrics_.devicePixelRatio < 0.5f) metrics_.devicePixelRatio = 1.0f;
    if (metrics_.devicePixelRatio > 4.0f) metrics_.devicePixelRatio = 4.0f;

    std::cout << "[DisplayInfo] Screen: " << metrics_.screenWidth << "x" << metrics_.screenHeight
              << " DPI: " << metrics_.dpi
              << " DPR: " << metrics_.devicePixelRatio << std::endl;
#elif defined(__APPLE__)
    // Screen size in points; device pixel ratio from the current display mode
    CGDirectDisplayID displayId = CGMainDisplayID();
    metrics_.screenWidth = static_cast<int>(CGDisplayPixelsWide(displayId));
    metrics_.screenHeight = static_cast<int>(CGDisplayPixelsHigh(displayId));

    metrics_.devicePixelRatio = 1.0f;
    if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayId)) {
        size_t points = CGDisplayModeGetWidth(mode);
        size_t pixels = CGDisplayModeGetPixelWidth(mode);
        if (points > 0 && pixels > 0) {
            metrics_.devicePixelRatio = static_cast<float>(pixels) / static_cast<float>(points);
        }
        CGDisplayModeRelease(mode);
    }

    // Physical DPI from the reported panel width (millimetres)
    CGSize sizeMm = CGDisplayScreenSize(displayId);
    if (sizeMm.width > 0.0) {
        metrics_.dpi = static_cast<float>(metrics_.screenWidth * metrics_.devicePixelRatio
                                          / (sizeMm.width / 25.4));
    } else {
        metrics_.dpi = 72.0f * metrics_.devicePixelRatio;
    }

    if (metrics_.devicePixelRatio < 0.5f) metrics_.devicePixelRatio = 1.0f;
    if (metrics_.devicePixelRatio > 4.0f) metrics_.devicePixelRatio = 4.0f;

    std::cout << "[DisplayInfo] Screen: " << metrics_.screenWidth << "x" << metrics_.screenHeight
              << " DPI: " << metrics_.dpi
              << " DPR: " << metrics_.devicePixelRatio << std::endl;
#else
    metrics_.screenWidth = 1920;
    metrics_.screenHeight = 1080;
    metrics_.dpi = 96.0f;
    metrics_.devicePixelRatio = 1.0f;
#endif
}

void DisplayInfo::setWindowSize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (metrics_.windowWidth == width && metrics_.windowHeight == height) return;
    metrics_.windowWidth = width;
    metrics_.windowHeight = height;
    notifyObservers();
}

void DisplayInfo::setScreenSize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    metrics_.screenWidth = width;
    metrics_.screenHeight = height;
    notifyObservers();
}

void DisplayInfo::setDPI(float dpi) {
    if (dpi <= 0.0f) return;
    metrics_.dpi = dpi;
    metrics_.devicePixelRatio = dpi / 96.0f;
    if (metrics_.devicePixelRatio < 0.5f) metrics_.devicePixelRatio = 1.0f;
    if (metrics_.devicePixelRatio > 4.0f) metrics_.devicePixelRatio = 4.0f;
    notifyObservers();
}

void DisplayInfo::setDevicePixelRatio(float ratio) {
    if (ratio < 0.5f || ratio > 4.0f) return;
    metrics_.devicePixelRatio = ratio;
    metrics_.dpi = ratio * 96.0f;
    notifyObservers();
}

void DisplayInfo::setMetrics(const DisplayMetrics& metrics) {
    metrics_ = metrics;
    notifyObservers();
}

uint32_t DisplayInfo::addObserver(DisplayChangeCallback callback) {
    std::lock_guard<std::mutex> lock(observerMutex_);
    uint32_t id = nextObserverId_++;
    observers_.push_back({id, std::move(callback)});
    return id;
}

void DisplayInfo::removeObserver(uint32_t id) {
    std::lock_guard<std::mutex> lock(observerMutex_);
    observers_.erase(
        std::remove_if(observers_.begin(), observers_.end(),
                       [id](const Observer& o) { return o.id == id; }),
        observers_.end());
}

void DisplayInfo::notifyObservers() {
    std::lock_guard<std::mutex> lock(observerMutex_);
    for (const auto& observer : observers_) {
        observer.callback(metrics_);
    }
}

} // namespace NXRender
