/**
 * @file window_system.hpp
 * @brief Platform-independent window management
 */

#pragma once

#include <string>
#include <functional>
#include <memory>

namespace Zepra::Platform {

struct WindowConfig {
    std::string title = "Zepra Browser";
    int width = 1024;
    int height = 768;
    int x = -1;  // -1 = centered
    int y = -1;
    bool resizable = true;
    bool decorated = true;
    bool maximized = false;
    bool fullscreen = false;
};

struct WindowHandle;

/**
 * @class WindowSystem
 * @brief Abstract interface for platform window management
 */
class WindowSystem {
public:
    virtual ~WindowSystem() = default;
    
    // Factory method
    static std::unique_ptr<WindowSystem> create();
    
    // Window lifecycle
    virtual WindowHandle* createWindow(const WindowConfig& config) = 0;
    virtual void destroyWindow(WindowHandle* window) = 0;
    
    // Window state
    virtual void showWindow(WindowHandle* window) = 0;
    virtual void hideWindow(WindowHandle* window) = 0;
    virtual void minimizeWindow(WindowHandle* window) = 0;
    virtual void maximizeWindow(WindowHandle* window) = 0;
    virtual void restoreWindow(WindowHandle* window) = 0;
    virtual void setFullscreen(WindowHandle* window, bool fullscreen) = 0;
    
    // Properties
    virtual void setTitle(WindowHandle* window, const std::string& title) = 0;
    virtual void setSize(WindowHandle* window, int width, int height) = 0;
    virtual void setPosition(WindowHandle* window, int x, int y) = 0;
    virtual void getSize(WindowHandle* window, int& width, int& height) = 0;
    virtual void getPosition(WindowHandle* window, int& x, int& y) = 0;
    
    // Focus
    virtual void focusWindow(WindowHandle* window) = 0;
    virtual bool isFocused(WindowHandle* window) = 0;
    
    // Native handle (for platform-specific operations)
    virtual void* getNativeHandle(WindowHandle* window) = 0;
    
    // Event callbacks
    using ResizeCallback = std::function<void(int width, int height)>;
    using CloseCallback = std::function<void()>;
    using FocusCallback = std::function<void(bool focused)>;
    
    virtual void setOnResize(WindowHandle* window, ResizeCallback callback) = 0;
    virtual void setOnClose(WindowHandle* window, CloseCallback callback) = 0;
    virtual void setOnFocus(WindowHandle* window, FocusCallback callback) = 0;
};

// Platform-specific implementations
#ifdef __linux__
class WindowSystemLinux : public WindowSystem {
public:
    WindowHandle* createWindow(const WindowConfig& config) override;
    void destroyWindow(WindowHandle* window) override;
    void showWindow(WindowHandle* window) override;
    void hideWindow(WindowHandle* window) override;
    void minimizeWindow(WindowHandle* window) override;
    void maximizeWindow(WindowHandle* window) override;
    void restoreWindow(WindowHandle* window) override;
    void setFullscreen(WindowHandle* window, bool fullscreen) override;
    void setTitle(WindowHandle* window, const std::string& title) override;
    void setSize(WindowHandle* window, int width, int height) override;
    void setPosition(WindowHandle* window, int x, int y) override;
    void getSize(WindowHandle* window, int& width, int& height) override;
    void getPosition(WindowHandle* window, int& x, int& y) override;
    void focusWindow(WindowHandle* window) override;
    bool isFocused(WindowHandle* window) override;
    void* getNativeHandle(WindowHandle* window) override;
    void setOnResize(WindowHandle* window, ResizeCallback callback) override;
    void setOnClose(WindowHandle* window, CloseCallback callback) override;
    void setOnFocus(WindowHandle* window, FocusCallback callback) override;
};
#endif

#ifdef _WIN32
class WindowSystemWin : public WindowSystem {
public:
    WindowHandle* createWindow(const WindowConfig& config) override;
    void destroyWindow(WindowHandle* window) override;
    void showWindow(WindowHandle* window) override;
    void hideWindow(WindowHandle* window) override;
    void minimizeWindow(WindowHandle* window) override;
    void maximizeWindow(WindowHandle* window) override;
    void restoreWindow(WindowHandle* window) override;
    void setFullscreen(WindowHandle* window, bool fullscreen) override;
    void setTitle(WindowHandle* window, const std::string& title) override;
    void setSize(WindowHandle* window, int width, int height) override;
    void setPosition(WindowHandle* window, int x, int y) override;
    void getSize(WindowHandle* window, int& width, int& height) override;
    void getPosition(WindowHandle* window, int& x, int& y) override;
    void focusWindow(WindowHandle* window) override;
    bool isFocused(WindowHandle* window) override;
    void* getNativeHandle(WindowHandle* window) override;
    void setOnResize(WindowHandle* window, ResizeCallback callback) override;
    void setOnClose(WindowHandle* window, CloseCallback callback) override;
    void setOnFocus(WindowHandle* window, FocusCallback callback) override;
};
#endif

#ifdef __APPLE__
class WindowSystemMacOS : public WindowSystem {
    // macOS implementation
};
#endif

} // namespace Zepra::Platform
