// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file platform.h
 * @brief Platform abstraction interface for NXRender
 */

#pragma once

#include <functional>
#include <string>

namespace NXRender {

class Platform {
public:
    virtual ~Platform() = default;

    /**
     * @brief Initialize platform window and context
     * @return true if successful
     */
    virtual bool init(int width, int height, const std::string& title) = 0;

    /**
     * @brief Cleanup platform resources
     */
    virtual void shutdown() = 0;

    /**
     * @brief Polls for pending events from the OS
     * This will dispatch events to the internal event system
     */
    virtual void pollEvents() = 0;

    /**
     * @brief Swap buffers (if double buffered)
     */
    virtual void swapBuffers() = 0;
    
    /**
     * @brief Set window title
     */
    virtual void setTitle(const std::string& title) = 0;
    
    /**
     * @brief Set mouse cursor type
     */
    enum class CursorType { Arrow, Hand, Text, Crosshair };
    virtual void setCursor(CursorType type) { (void)type; }

    /**
     * @brief Native window-system handles, for OS integrations such as the
     *        clipboard (X11: Display* and Window). Null/0 when not applicable.
     */
    virtual void* nativeDisplay() const { return nullptr; }
    virtual unsigned long nativeWindow() const { return 0; }

    /**
     * @brief Receives raw OS events the backend does not handle itself
     *        (X11: XEvent*, e.g. SelectionRequest).
     */
    void setNativeEventHook(std::function<void(void*)> hook) { nativeEventHook_ = std::move(hook); }

protected:
    std::function<void(void*)> nativeEventHook_;
};

// Factory function implemented by backend
Platform* createPlatform();

} // namespace NXRender
