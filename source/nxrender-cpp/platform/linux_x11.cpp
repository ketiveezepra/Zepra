// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file linux_x11.cpp
 * @brief X11 Platform implementation for Linux
 */

#include "platform.h"
#include "input/events.h"
#include "nxrender_cpp.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h> // Required for XC_* cursors
#include <GL/glx.h>

// X11 defines 'None' as 0L, which conflicts with our enums
#ifdef None
#undef None
#endif
#ifdef Always
#undef Always
#endif
#include <clocale>
#include <string>
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <sstream>

// Internal event dispatcher function defined in nxrender.cpp
namespace NXRender {
    void dispatchEvent(const Event& event);
}

namespace NXRender {

class LinuxX11Platform : public Platform {
public:
    LinuxX11Platform() = default;
    
    ~LinuxX11Platform() {
        shutdown();
    }
    
    bool init(int width, int height, const std::string& title) override {
        display_ = XOpenDisplay(nullptr);
        if (!display_) {
            std::cerr << "[Platform] Failed to open X display" << std::endl;
            return false;
        }
        
        int screen = DefaultScreen(display_);
        
        // Try RGBA with multisampling
        int attribs[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 24, GLX_STENCIL_SIZE, 8, 0 };
        XVisualInfo* vi = glXChooseVisual(display_, screen, attribs);
        
        if (!vi) {
             std::cerr << "[Platform] Failed to choose visual" << std::endl;
             return false;
        }
        
        Colormap cmap = XCreateColormap(display_, RootWindow(display_, screen), vi->visual, AllocNone);
        XSetWindowAttributes swa = {};  // Zero-init all fields
        swa.colormap = cmap;
        swa.border_pixel = 0;  // Required for non-default visuals
        swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | 
                         ButtonPressMask | ButtonReleaseMask | 
                         PointerMotionMask | StructureNotifyMask;
        
        window_ = XCreateWindow(display_, RootWindow(display_, screen), 0, 0, width, height, 0,
                                vi->depth, InputOutput, vi->visual, 
                                CWColormap | CWBorderPixel | CWEventMask, &swa);
        
        XMapWindow(display_, window_);
        XStoreName(display_, window_, title.c_str());
        
        // Handle close button
        wmDeleteMessage_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display_, window_, &wmDeleteMessage_, 1);
        
        // XDnd atoms for drag-and-drop
        XdndAware_ = XInternAtom(display_, "XdndAware", False);
        XdndEnter_ = XInternAtom(display_, "XdndEnter", False);
        XdndLeave_ = XInternAtom(display_, "XdndLeave", False);
        XdndDrop_ = XInternAtom(display_, "XdndDrop", False);
        XdndPosition_ = XInternAtom(display_, "XdndPosition", False);
        XdndStatus_ = XInternAtom(display_, "XdndStatus", False);
        XdndFinished_ = XInternAtom(display_, "XdndFinished", False);
        XdndSelection_ = XInternAtom(display_, "XdndSelection", False);
        XdndActionCopy_ = XInternAtom(display_, "XdndActionCopy", False);
        XdndTypeList_ = XInternAtom(display_, "XdndTypeList", False);
        textUriList_ = XInternAtom(display_, "text/uri-list", False);
        
        // Declare XDnd support (version 5)
        Atom xdndVersion = 5;
        XChangeProperty(display_, window_, XdndAware_, XA_ATOM, 32, 
                        PropModeReplace, (unsigned char*)&xdndVersion, 1);
        
        // Create GL context
        glContext_ = glXCreateContext(display_, vi, nullptr, GL_TRUE);
        glXMakeCurrent(display_, window_, glContext_);
        XFree(vi);
        unthrottleSwapsIfAsked();
        
        // Explicitly re-apply event mask after GLX context binding
        // Required on some X11/GLX implementations where CWEventMask alone
        // is insufficient with non-default visuals
        XSelectInput(display_, window_, ExposureMask | KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | StructureNotifyMask);
        
        openInputMethod();

        // Create Cursors
        cursorArrow_ = XCreateFontCursor(display_, XC_left_ptr);
        cursorHand_ = XCreateFontCursor(display_, XC_hand2);
        cursorText_ = XCreateFontCursor(display_, XC_xterm);
        XDefineCursor(display_, window_, cursorArrow_);
        
        std::cout << "[Platform] X11 Window created (" << width << "x" << height << ")" << std::endl;
        return true;
    }
    
    void shutdown() override {
        if (display_) {
            if (inputContext_) XDestroyIC(inputContext_);
            if (inputMethod_) XCloseIM(inputMethod_);
            inputContext_ = nullptr;
            inputMethod_ = nullptr;
            glXMakeCurrent(display_, 0, nullptr);
            if (glContext_) glXDestroyContext(display_, glContext_);
            if (window_) XDestroyWindow(display_, window_);
            XCloseDisplay(display_);
            display_ = nullptr;
        }
    }
    
    void pollEvents() override {
        while (XPending(display_) > 0) {
            XEvent ev;
            XNextEvent(display_, &ev);
            // The input method takes the keys of dead-key and compose
            // sequences, and sends back the character they make
            if (XFilterEvent(&ev, 0)) continue;
            
            if (ev.type == ClientMessage) {
                if ((Atom)ev.xclient.data.l[0] == wmDeleteMessage_) {
                    Event event;
                    event.type = EventType::Close;
                    NXRender::dispatchEvent(event);
                } else if (ev.xclient.message_type == XdndEnter_) {
                    // File drag entered - store source window
                    xdndSourceWindow_ = ev.xclient.data.l[0];
                } else if (ev.xclient.message_type == XdndPosition_) {
                    // Send XdndStatus to accept the drop
                    XClientMessageEvent m = {};
                    m.type = ClientMessage;
                    m.display = display_;
                    m.window = xdndSourceWindow_;
                    m.message_type = XdndStatus_;
                    m.format = 32;
                    m.data.l[0] = window_;  // Target window
                    m.data.l[1] = 1;        // Accept drop
                    m.data.l[2] = 0;        // Rectangle
                    m.data.l[3] = 0;
                    m.data.l[4] = XdndActionCopy_;
                    XSendEvent(display_, xdndSourceWindow_, False, NoEventMask, (XEvent*)&m);
                    XFlush(display_);
                } else if (ev.xclient.message_type == XdndDrop_) {
                    // Request the file list
                    XConvertSelection(display_, XdndSelection_, textUriList_, 
                                     XdndSelection_, window_, ev.xclient.data.l[2]);
                }
            } else if (ev.type == ConfigureNotify) {
                // Window resize
                Event event;
                event.type = EventType::Resize;
                event.window.width = ev.xconfigure.width;
                event.window.height = ev.xconfigure.height;
                NXRender::dispatchEvent(event);
            } else if (ev.type == MotionNotify) {
                Event event;
                event.type = EventType::MouseMove;
                event.mouse.x = (float)ev.xmotion.x;
                event.mouse.y = (float)ev.xmotion.y;
                event.mouse.modifiers = getModifiers(ev.xmotion.state);
                NXRender::dispatchEvent(event);
            } else if (ev.type == ButtonPress) {
                Event event;
                event.type = EventType::MouseDown;
                event.mouse.x = (float)ev.xbutton.x;
                event.mouse.y = (float)ev.xbutton.y;
                event.mouse.button = mapButton(ev.xbutton.button);
                event.mouse.modifiers = getModifiers(ev.xbutton.state);
                
                // Scroll wheel acts as button 4/5
                if (ev.xbutton.button == 4) {
                    event.type = EventType::MouseWheel;
                    event.mouse.wheelDelta = 1.0f;
                } else if (ev.xbutton.button == 5) {
                    event.type = EventType::MouseWheel;
                    event.mouse.wheelDelta = -1.0f;
                }
                
                NXRender::dispatchEvent(event);
            } else if (ev.type == ButtonRelease) {
                if (ev.xbutton.button == 4 || ev.xbutton.button == 5) return; // Ignore wheel release
                
                Event event;
                event.type = EventType::MouseUp;
                event.mouse.x = (float)ev.xbutton.x;
                event.mouse.y = (float)ev.xbutton.y;
                event.mouse.button = mapButton(ev.xbutton.button);
                event.mouse.modifiers = getModifiers(ev.xbutton.state);
                NXRender::dispatchEvent(event);
            } else if (ev.type == KeyPress) {
                // The key and the text it types, as UTF-8: through the input
                // method when there is one (any layout, compose and dead
                // keys), else Latin-1 from XLookupString
                char textBuffer[64];
                KeySym keysym = NoSymbol;
                std::string typed;
                if (inputContext_) {
                    Status status = 0;
                    int count = Xutf8LookupString(inputContext_, &ev.xkey, textBuffer, sizeof(textBuffer) - 1,
                                                  &keysym, &status);
                    if (status != XLookupKeySym && status != XLookupBoth) keysym = NoSymbol;
                    if ((status == XLookupChars || status == XLookupBoth) && count > 0) typed.assign(textBuffer, count);
                } else {
                    int count = XLookupString(&ev.xkey, textBuffer, sizeof(textBuffer), &keysym, nullptr);
                    for (int i = 0; i < count; i++) {
                        unsigned char c = static_cast<unsigned char>(textBuffer[i]);
                        if (c < 0x80) {
                            typed += static_cast<char>(c);
                        } else {
                            typed += static_cast<char>(0xC0 | (c >> 6));
                            typed += static_cast<char>(0x80 | (c & 0x3F));
                        }
                    }
                }
                if (keysym == NoSymbol) keysym = XLookupKeysym(&ev.xkey, 0);
                
                // Dispatch KeyDown event with the KeySym
                Event keyEvent;
                keyEvent.type = EventType::KeyDown;
                keyEvent.key.key = mapKey(keysym);
                keyEvent.key.modifiers = getModifiers(ev.xkey.state);
                
                // Printable text only: Ctrl+letter gives control characters
                if (!typed.empty() && static_cast<unsigned char>(typed[0]) >= 32 && typed[0] != 0x7F) {
                    keyEvent.textInput = typed;
                }
                
                NXRender::dispatchEvent(keyEvent);
                
            } else if (ev.type == KeyRelease) {
                // Ignore auto-repeat release (if needed)
                // X11 sends Release then immediate Press for repeat. 
                // We'll just dispatch for now.
                
                KeySym key = XLookupKeysym(&ev.xkey, 0);
                Event event;
                event.type = EventType::KeyUp;
                event.key.key = mapKey(key);
                event.key.modifiers = getModifiers(ev.xkey.state);
                NXRender::dispatchEvent(event);
            } else if (ev.type == SelectionNotify) {
                // XDnd file list received
                if (ev.xselection.property == XdndSelection_) {
                    Atom type;
                    int format;
                    unsigned long numItems, bytesLeft;
                    unsigned char* data = nullptr;
                    
                    XGetWindowProperty(display_, window_, XdndSelection_, 0, 65536, False,
                                      AnyPropertyType, &type, &format, &numItems, &bytesLeft, &data);
                    
                    if (data && numItems > 0) {
                        std::string uriList((char*)data, numItems);
                        Event event;
                        event.type = EventType::FileDrop;
                        
                        // Parse URI list (one file per line)
                        std::istringstream iss(uriList);
                        std::string line;
                        while (std::getline(iss, line)) {
                            if (line.empty() || line[0] == '#') continue;  // Skip comments
                            // Remove trailing \r if present
                            if (!line.empty() && line.back() == '\r') line.pop_back();
                            // Remove file:// prefix
                            if (line.substr(0, 7) == "file://") {
                                std::string path = line.substr(7);
                                // URL decode %XX sequences
                                std::string decoded;
                                for (size_t i = 0; i < path.length(); i++) {
                                    if (path[i] == '%' && i + 2 < path.length()) {
                                        int hex = std::stoi(path.substr(i+1, 2), nullptr, 16);
                                        decoded += (char)hex;
                                        i += 2;
                                    } else {
                                        decoded += path[i];
                                    }
                                }
                                event.droppedFiles.push_back(decoded);
                            }
                        }
                        
                        if (!event.droppedFiles.empty()) {
                            NXRender::dispatchEvent(event);
                        }
                        XFree(data);
                    }
                    
                    // Send XdndFinished
                    XClientMessageEvent m = {};
                    m.type = ClientMessage;
                    m.display = display_;
                    m.window = xdndSourceWindow_;
                    m.message_type = XdndFinished_;
                    m.format = 32;
                    m.data.l[0] = window_;
                    m.data.l[1] = 1;  // Accepted
                    m.data.l[2] = XdndActionCopy_;
                    XSendEvent(display_, xdndSourceWindow_, False, NoEventMask, (XEvent*)&m);
                    XFlush(display_);
                }
            } else if (ev.type == SelectionRequest) {
                // Another client wants a selection we own (clipboard copy)
                if (nativeEventHook_) nativeEventHook_(&ev);
            }
        }
    }

    // A window the compositor finds covered gets its buffer swaps paced at
    // about one a second (vsync'd glXSwapBuffers blocks that long). Page
    // captures (ZEPRA_CAPTURE) and ZEPRA_NO_VSYNC=1 turn the swap interval
    // to 0, so animation timing does not depend on what covers the window;
    // the run loop paces frames at 60 a second itself.
    void unthrottleSwapsIfAsked() {
        const char* capture = std::getenv("ZEPRA_CAPTURE");
        const char* noVsync = std::getenv("ZEPRA_NO_VSYNC");
        bool wanted = (capture && *capture) || (noVsync && std::string(noVsync) == "1");
        if (!wanted || !display_ || !window_) return;
        using SwapIntervalEXT = void (*)(Display*, GLXDrawable, int);
        using SwapIntervalMESA = int (*)(unsigned int);
        auto ext = reinterpret_cast<SwapIntervalEXT>(
            glXGetProcAddress(reinterpret_cast<const GLubyte*>("glXSwapIntervalEXT")));
        auto mesa = reinterpret_cast<SwapIntervalMESA>(
            glXGetProcAddress(reinterpret_cast<const GLubyte*>("glXSwapIntervalMESA")));
        const char* ext_list = glXQueryExtensionsString(display_, DefaultScreen(display_));
        std::string extensions = ext_list ? ext_list : "";
        if (ext && extensions.find("GLX_EXT_swap_control") != std::string::npos) {
            ext(display_, window_, 0);
            std::cout << "[Platform] vsync off (GLX_EXT_swap_control)" << std::endl;
        } else if (mesa && extensions.find("GLX_MESA_swap_control") != std::string::npos) {
            int rc = mesa(0);
            std::cout << "[Platform] vsync off (GLX_MESA_swap_control): " << (rc == 0 ? "ok" : "failed") << std::endl;
        } else {
            std::cerr << "[Platform] vsync could not be turned off: no swap control extension" << std::endl;
        }
    }

    void* nativeDisplay() const override { return display_; }
    unsigned long nativeWindow() const override { return window_; }
    
    void swapBuffers() override {
        if (display_ && window_) {
            glXSwapBuffers(display_, window_);
        }
    }
    
    void setTitle(const std::string& title) override {
        if (display_ && window_) {
            XStoreName(display_, window_, title.c_str());
        }
    }
    
    void setCursor(CursorType type) override {
        if (!display_ || !window_) return;
        Cursor c = cursorArrow_;
        switch (type) {
            case CursorType::Arrow: c = cursorArrow_; break;
            case CursorType::Hand:  c = cursorHand_;  break;
            case CursorType::Text:  c = cursorText_;  break;
            default: break;
        }
        XDefineCursor(display_, window_, c);
        XFlush(display_);
    }

private:
    Display* display_ = nullptr;
    Window window_ = 0;
    XIM inputMethod_ = nullptr;
    XIC inputContext_ = nullptr;
    GLXContext glContext_ = nullptr;
    Atom wmDeleteMessage_ = 0;
    Cursor cursorArrow_, cursorHand_, cursorText_;
    
    // XDnd atoms
    Atom XdndAware_ = 0, XdndEnter_ = 0, XdndLeave_ = 0, XdndDrop_ = 0;
    Atom XdndPosition_ = 0, XdndStatus_ = 0, XdndFinished_ = 0, XdndSelection_ = 0;
    Atom XdndActionCopy_ = 0, XdndTypeList_ = 0, textUriList_ = 0;
    Window xdndSourceWindow_ = 0;
    
    Modifiers getModifiers(unsigned int state) {
        Modifiers m;
        m.shift = (state & ShiftMask);
        m.ctrl = (state & ControlMask);
        m.alt = (state & Mod1Mask);
        m.meta = (state & Mod4Mask);
        return m;
    }
    
    MouseButton mapButton(unsigned int button) {
        switch (button) {
            case 1: return MouseButton::Left;
            case 2: return MouseButton::Middle;
            case 3: return MouseButton::Right;
            default: return MouseButton::None;
        }
    }
    
    /// An input method for typing text in the user's layout and locale;
    /// without one, keys type Latin-1 only
    void openInputMethod() {
        if (!std::setlocale(LC_CTYPE, "")) std::setlocale(LC_CTYPE, "C.UTF-8");
        XSetLocaleModifiers("");
        inputMethod_ = XOpenIM(display_, nullptr, nullptr, nullptr);
        if (!inputMethod_) {
            XSetLocaleModifiers("@im=none");
            inputMethod_ = XOpenIM(display_, nullptr, nullptr, nullptr);
        }
        if (!inputMethod_) return;
        inputContext_ = XCreateIC(inputMethod_, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                                  XNClientWindow, window_, XNFocusWindow, window_, nullptr);
        if (inputContext_) XSetICFocus(inputContext_);
    }

    KeyCode mapKey(KeySym key) {
        // Simplified mapping for common keys
        if (key >= XK_a && key <= XK_z) return (KeyCode)((int)KeyCode::A + (key - XK_a));
        if (key >= XK_A && key <= XK_Z) return (KeyCode)((int)KeyCode::A + (key - XK_A)); // Normalize
        if (key >= XK_0 && key <= XK_9) return (KeyCode)((int)KeyCode::Num0 + (key - XK_0));
        
        switch (key) {
            case XK_Return: return KeyCode::Enter;
            case XK_BackSpace: return KeyCode::Backspace;
            case XK_Tab: return KeyCode::Tab;
            case XK_Escape: return KeyCode::Escape;
            case XK_space: return KeyCode::Space;
            case XK_Left: return KeyCode::Left;
            case XK_Right: return KeyCode::Right;
            case XK_Up: return KeyCode::Up;
            case XK_Down: return KeyCode::Down;
            case XK_Shift_L: case XK_Shift_R: return KeyCode::Shift;
            case XK_Control_L: case XK_Control_R: return KeyCode::Ctrl;
            case XK_Alt_L: case XK_Alt_R: return KeyCode::Alt;
            case XK_Home: case XK_KP_Home: return KeyCode::Home;
            case XK_End: case XK_KP_End: return KeyCode::End;
            case XK_Page_Up: case XK_KP_Page_Up: return KeyCode::PageUp;
            case XK_Page_Down: case XK_KP_Page_Down: return KeyCode::PageDown;
            case XK_Delete: case XK_KP_Delete: return KeyCode::Delete;
            case XK_Insert: case XK_KP_Insert: return KeyCode::Insert;
            case XK_KP_Left: return KeyCode::Left;
            case XK_KP_Right: return KeyCode::Right;
            case XK_KP_Up: return KeyCode::Up;
            case XK_KP_Down: return KeyCode::Down;
            case XK_KP_Enter: return KeyCode::Enter;
            case XK_ISO_Left_Tab: return KeyCode::Tab;   // Shift+Tab
            case XK_Super_L: case XK_Super_R: case XK_Meta_L: case XK_Meta_R: return KeyCode::Meta;
            case XK_Caps_Lock: return KeyCode::CapsLock;
            case XK_F1: return KeyCode::F1;
            case XK_F2: return KeyCode::F2;
            case XK_F3: return KeyCode::F3;
            case XK_F4: return KeyCode::F4;
            case XK_F5: return KeyCode::F5;
            case XK_F6: return KeyCode::F6;
            case XK_F7: return KeyCode::F7;
            case XK_F8: return KeyCode::F8;
            case XK_F9: return KeyCode::F9;
            case XK_F10: return KeyCode::F10;
            case XK_F11: return KeyCode::F11;
            case XK_F12: return KeyCode::F12;
            // Punctuation, shifted or not (XLookupString gives the shifted keysym)
            case XK_comma: case XK_less: return KeyCode::Comma;
            case XK_period: case XK_greater: return KeyCode::Period;
            case XK_slash: case XK_question: return KeyCode::Slash;
            case XK_backslash: case XK_bar: return KeyCode::Backslash;
            case XK_semicolon: case XK_colon: return KeyCode::Semicolon;
            case XK_apostrophe: case XK_quotedbl: return KeyCode::Quote;
            case XK_bracketleft: case XK_braceleft: return KeyCode::LeftBracket;
            case XK_bracketright: case XK_braceright: return KeyCode::RightBracket;
            case XK_minus: case XK_underscore: return KeyCode::Minus;
            case XK_equal: case XK_plus: return KeyCode::Equals;
            case XK_grave: case XK_asciitilde: return KeyCode::Backtick;
            default:
                return KeyCode::Unknown;
        }
    }
};

// Factory
Platform* createPlatform() {
    return new LinuxX11Platform();
}

} // namespace NXRender
