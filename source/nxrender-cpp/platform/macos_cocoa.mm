// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file macos_cocoa.mm
 * @brief NXRender macOS platform backend (Cocoa + NSOpenGL)
 *
 * macOS equivalent of linux_x11.cpp and windows_wgl.cpp.
 *
 * Rules:
 *  - This file ONLY compiles on __APPLE__.
 *  - No SDL, no GLFW, no external windowing library.
 *  - Same event contract as the other backends: top-left origin in window
 *    points, and KeyDown carries its printable ASCII character in textInput.
 *  - The GL context is a legacy (2.1) profile: NXRender uses the
 *    fixed-function pipeline and GLSL 1.20.
 */

#ifdef __APPLE__

#ifndef GL_SILENCE_DEPRECATION
#  define GL_SILENCE_DEPRECATION
#endif

#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>  // kVK_* virtual key codes
#include <OpenGL/gl.h>

#include "platform.h"
#include "platform/macos_clipboard.h"
#include "input/events.h"
#include "nxrender_cpp.h"

#include <iostream>
#include <string>

// Internal event dispatcher function defined in nxrender.cpp
namespace NXRender {
    void dispatchEvent(const Event& event);
}

using namespace NXRender;

// =============================================================================
// Key and modifier mapping
// =============================================================================

static KeyCode mapKeyCode(unsigned short keyCode) {
    switch (keyCode) {
        case kVK_ANSI_A: return KeyCode::A;  case kVK_ANSI_B: return KeyCode::B;
        case kVK_ANSI_C: return KeyCode::C;  case kVK_ANSI_D: return KeyCode::D;
        case kVK_ANSI_E: return KeyCode::E;  case kVK_ANSI_F: return KeyCode::F;
        case kVK_ANSI_G: return KeyCode::G;  case kVK_ANSI_H: return KeyCode::H;
        case kVK_ANSI_I: return KeyCode::I;  case kVK_ANSI_J: return KeyCode::J;
        case kVK_ANSI_K: return KeyCode::K;  case kVK_ANSI_L: return KeyCode::L;
        case kVK_ANSI_M: return KeyCode::M;  case kVK_ANSI_N: return KeyCode::N;
        case kVK_ANSI_O: return KeyCode::O;  case kVK_ANSI_P: return KeyCode::P;
        case kVK_ANSI_Q: return KeyCode::Q;  case kVK_ANSI_R: return KeyCode::R;
        case kVK_ANSI_S: return KeyCode::S;  case kVK_ANSI_T: return KeyCode::T;
        case kVK_ANSI_U: return KeyCode::U;  case kVK_ANSI_V: return KeyCode::V;
        case kVK_ANSI_W: return KeyCode::W;  case kVK_ANSI_X: return KeyCode::X;
        case kVK_ANSI_Y: return KeyCode::Y;  case kVK_ANSI_Z: return KeyCode::Z;
        case kVK_ANSI_0: return KeyCode::Num0; case kVK_ANSI_1: return KeyCode::Num1;
        case kVK_ANSI_2: return KeyCode::Num2; case kVK_ANSI_3: return KeyCode::Num3;
        case kVK_ANSI_4: return KeyCode::Num4; case kVK_ANSI_5: return KeyCode::Num5;
        case kVK_ANSI_6: return KeyCode::Num6; case kVK_ANSI_7: return KeyCode::Num7;
        case kVK_ANSI_8: return KeyCode::Num8; case kVK_ANSI_9: return KeyCode::Num9;
        case kVK_F1:  return KeyCode::F1;  case kVK_F2:  return KeyCode::F2;
        case kVK_F3:  return KeyCode::F3;  case kVK_F4:  return KeyCode::F4;
        case kVK_F5:  return KeyCode::F5;  case kVK_F6:  return KeyCode::F6;
        case kVK_F7:  return KeyCode::F7;  case kVK_F8:  return KeyCode::F8;
        case kVK_F9:  return KeyCode::F9;  case kVK_F10: return KeyCode::F10;
        case kVK_F11: return KeyCode::F11; case kVK_F12: return KeyCode::F12;
        case kVK_LeftArrow:  return KeyCode::Left;
        case kVK_RightArrow: return KeyCode::Right;
        case kVK_UpArrow:    return KeyCode::Up;
        case kVK_DownArrow:  return KeyCode::Down;
        case kVK_Home:       return KeyCode::Home;
        case kVK_End:        return KeyCode::End;
        case kVK_PageUp:     return KeyCode::PageUp;
        case kVK_PageDown:   return KeyCode::PageDown;
        case kVK_Delete:        return KeyCode::Backspace;  // the key labelled "delete"
        case kVK_ForwardDelete: return KeyCode::Delete;
        case kVK_Help:          return KeyCode::Insert;
        case kVK_Return:
        case kVK_ANSI_KeypadEnter: return KeyCode::Enter;
        case kVK_Tab:        return KeyCode::Tab;
        case kVK_Escape:     return KeyCode::Escape;
        case kVK_Space:      return KeyCode::Space;
        case kVK_Shift:
        case kVK_RightShift:   return KeyCode::Shift;
        case kVK_Control:
        case kVK_RightControl: return KeyCode::Ctrl;
        case kVK_Option:
        case kVK_RightOption:  return KeyCode::Alt;
        case kVK_Command:
        case kVK_RightCommand: return KeyCode::Meta;
        case kVK_CapsLock:   return KeyCode::CapsLock;
        case kVK_ANSI_Comma:        return KeyCode::Comma;
        case kVK_ANSI_Period:       return KeyCode::Period;
        case kVK_ANSI_Slash:        return KeyCode::Slash;
        case kVK_ANSI_Backslash:    return KeyCode::Backslash;
        case kVK_ANSI_Semicolon:    return KeyCode::Semicolon;
        case kVK_ANSI_Quote:        return KeyCode::Quote;
        case kVK_ANSI_LeftBracket:  return KeyCode::LeftBracket;
        case kVK_ANSI_RightBracket: return KeyCode::RightBracket;
        case kVK_ANSI_Minus:        return KeyCode::Minus;
        case kVK_ANSI_Equal:        return KeyCode::Equals;
        case kVK_ANSI_Grave:        return KeyCode::Backtick;
        default: return KeyCode::Unknown;
    }
}

// The browser's shortcuts test `ctrl` (Ctrl+L, Ctrl+C, ...); on macOS users
// press Command for those, so Command sets ctrl as well as meta.
static Modifiers mapModifiers(NSEventModifierFlags flags) {
    Modifiers m;
    m.shift = (flags & NSEventModifierFlagShift) != 0;
    m.alt   = (flags & NSEventModifierFlagOption) != 0;
    m.meta  = (flags & NSEventModifierFlagCommand) != 0;
    m.ctrl  = (flags & NSEventModifierFlagControl) != 0 || m.meta;
    return m;
}

static MouseButton mapMouseButton(NSInteger buttonNumber) {
    switch (buttonNumber) {
        case 0: return MouseButton::Left;
        case 1: return MouseButton::Right;
        case 2: return MouseButton::Middle;
        case 3: return MouseButton::X1;
        case 4: return MouseButton::X2;
        default: return MouseButton::None;
    }
}

// =============================================================================
// Content view: GL surface, coordinate flip, file drops
// =============================================================================

@interface NXRenderView : NSView
@end

@implementation NXRenderView

- (BOOL)isFlipped { return YES; }  // top-left origin, like X11 and Win32
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    (void)sender;
    return NSDragOperationCopy;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSArray<NSURL*>* urls = [[sender draggingPasteboard]
        readObjectsForClasses:@[[NSURL class]]
                      options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    if (urls.count == 0) return NO;

    Event event;
    event.type = EventType::FileDrop;
    NSPoint p = [self convertPoint:[sender draggingLocation] fromView:nil];
    event.dropX = static_cast<float>(p.x);
    event.dropY = static_cast<float>(p.y);
    for (NSURL* url in urls) {
        if (const char* path = url.fileSystemRepresentation) {
            event.droppedFiles.push_back(path);
        }
    }
    NXRender::dispatchEvent(event);
    return YES;
}

@end

// =============================================================================
// Window delegate: close, resize, focus
// =============================================================================

@interface NXRenderWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, weak) NSOpenGLContext* glContext;
@end

@implementation NXRenderWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    Event event;
    event.type = EventType::Close;
    NXRender::dispatchEvent(event);
    return NO;  // the app quits and Platform::shutdown() closes the window
}

- (void)windowDidResize:(NSNotification*)notification {
    NSWindow* window = notification.object;
    [self.glContext update];
    NSSize size = window.contentView.bounds.size;
    Event event;
    event.type = EventType::Resize;
    event.window.width  = static_cast<int>(size.width);
    event.window.height = static_cast<int>(size.height);
    NXRender::dispatchEvent(event);
}

- (void)windowDidBecomeKey:(NSNotification*)notification {
    (void)notification;
    Event event;
    event.type = EventType::Focus;
    NXRender::dispatchEvent(event);
}

- (void)windowDidResignKey:(NSNotification*)notification {
    (void)notification;
    Event event;
    event.type = EventType::Blur;
    NXRender::dispatchEvent(event);
}

@end

// =============================================================================
// MacOSCocoaPlatform : public Platform
// =============================================================================

namespace NXRender {

class MacOSCocoaPlatform : public Platform {
public:
    MacOSCocoaPlatform() = default;
    ~MacOSCocoaPlatform() override { shutdown(); }

    bool init(int width, int height, const std::string& title) override {
        @autoreleasepool {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            installMenu();

            NSRect frame = NSMakeRect(0, 0, width, height);
            NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                      NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
            window_ = [[NSWindow alloc] initWithContentRect:frame
                                                  styleMask:style
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
            if (!window_) return false;
            window_.releasedWhenClosed = NO;
            window_.title = [NSString stringWithUTF8String:title.c_str()];
            window_.acceptsMouseMovedEvents = YES;

            view_ = [[NXRenderView alloc] initWithFrame:frame];
            // Render at one pixel per point, matching the X11 and Win32 backends
            view_.wantsBestResolutionOpenGLSurface = NO;
            [view_ registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
            window_.contentView = view_;

            NSOpenGLPixelFormatAttribute attrs[] = {
                NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersionLegacy,
                NSOpenGLPFADoubleBuffer,
                NSOpenGLPFAColorSize, 24,
                NSOpenGLPFAAlphaSize, 8,
                NSOpenGLPFADepthSize, 24,
                NSOpenGLPFAStencilSize, 8,
                NSOpenGLPFAAccelerated,
                0
            };
            NSOpenGLPixelFormat* format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
            if (!format) {
                std::cerr << "[Platform] No suitable NSOpenGL pixel format" << std::endl;
                return false;
            }
            glContext_ = [[NSOpenGLContext alloc] initWithFormat:format shareContext:nil];
            if (!glContext_) return false;
            glContext_.view = view_;
            [glContext_ makeCurrentContext];
            GLint swapInterval = 1;
            [glContext_ setValues:&swapInterval forParameter:NSOpenGLContextParameterSwapInterval];

            delegate_ = [[NXRenderWindowDelegate alloc] init];
            delegate_.glContext = glContext_;
            window_.delegate = delegate_;

            [window_ center];
            [window_ makeKeyAndOrderFront:nil];
            [window_ makeFirstResponder:view_];
            [NSApp finishLaunching];
            [NSApp activateIgnoringOtherApps:YES];

            currentCursor_ = [NSCursor arrowCursor];

            std::cout << "[Platform] Cocoa/NSOpenGL Window created ("
                      << width << "x" << height << ")" << std::endl;
            return true;
        }
    }

    void shutdown() override {
        @autoreleasepool {
            if (glContext_) {
                [NSOpenGLContext clearCurrentContext];
                [glContext_ clearDrawable];
                glContext_ = nil;
            }
            if (window_) {
                window_.delegate = nil;
                [window_ close];
                window_ = nil;
            }
            view_ = nil;
            delegate_ = nil;
        }
    }

    void pollEvents() override {
        @autoreleasepool {
            while (NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                    untilDate:[NSDate distantPast]
                                                       inMode:NSDefaultRunLoopMode
                                                      dequeue:YES]) {
                if (ev.window == window_ && handleEvent(ev)) continue;
                [NSApp sendEvent:ev];
            }
        }
    }

    void swapBuffers() override {
        if (glContext_) [glContext_ flushBuffer];
    }

    void setTitle(const std::string& title) override {
        if (window_) window_.title = [NSString stringWithUTF8String:title.c_str()];
    }

    void setCursor(CursorType type) override {
        switch (type) {
            case CursorType::Arrow:     currentCursor_ = [NSCursor arrowCursor];         break;
            case CursorType::Hand:      currentCursor_ = [NSCursor pointingHandCursor];  break;
            case CursorType::Text:      currentCursor_ = [NSCursor IBeamCursor];         break;
            case CursorType::Crosshair: currentCursor_ = [NSCursor crosshairCursor];     break;
        }
        [currentCursor_ set];
    }

private:
    // Returns true when the event is fully handled here and must not be
    // forwarded to NSApp (key events, so Cocoa does not beep on them).
    bool handleEvent(NSEvent* ev) {
        switch (ev.type) {
            case NSEventTypeKeyDown: {
                // Command shortcuts first go to the menu (Cmd+Q, Cmd+H, Cmd+M)
                if ((ev.modifierFlags & NSEventModifierFlagCommand) &&
                    [[NSApp mainMenu] performKeyEquivalent:ev]) {
                    return true;
                }
                Event event;
                event.type = EventType::KeyDown;
                event.key.key = mapKeyCode(ev.keyCode);
                event.key.repeat = ev.isARepeat;
                event.key.modifiers = mapModifiers(ev.modifierFlags);

                // Printable ASCII travels with the KeyDown, as on X11 and Win32;
                // other characters are delivered as TextInput.
                std::string nonAscii;
                if (!event.key.modifiers.meta && !(ev.modifierFlags & NSEventModifierFlagControl)) {
                    NSString* chars = ev.characters;
                    const char* utf8 = chars.UTF8String;
                    if (utf8 && utf8[0]) {
                        unsigned char c0 = static_cast<unsigned char>(utf8[0]);
                        if (utf8[1] == '\0' && c0 >= 32 && c0 < 127) {
                            event.textInput = utf8;
                        } else if (c0 >= 0x80) {
                            nonAscii = utf8;
                        }
                    }
                }
                NXRender::dispatchEvent(event);
                if (!nonAscii.empty()) {
                    Event text;
                    text.type = EventType::TextInput;
                    text.textInput = nonAscii;
                    NXRender::dispatchEvent(text);
                }
                return true;
            }

            case NSEventTypeKeyUp: {
                Event event;
                event.type = EventType::KeyUp;
                event.key.key = mapKeyCode(ev.keyCode);
                event.key.modifiers = mapModifiers(ev.modifierFlags);
                NXRender::dispatchEvent(event);
                return true;
            }

            case NSEventTypeMouseMoved:
            case NSEventTypeLeftMouseDragged:
            case NSEventTypeRightMouseDragged:
            case NSEventTypeOtherMouseDragged: {
                NSPoint p;
                if (!contentPoint(ev, p)) return false;
                [currentCursor_ set];
                Event event;
                event.type = EventType::MouseMove;
                event.mouse.x = static_cast<float>(p.x);
                event.mouse.y = static_cast<float>(p.y);
                event.mouse.modifiers = mapModifiers(ev.modifierFlags);
                NXRender::dispatchEvent(event);
                return false;
            }

            case NSEventTypeLeftMouseDown:
            case NSEventTypeRightMouseDown:
            case NSEventTypeOtherMouseDown:
            case NSEventTypeLeftMouseUp:
            case NSEventTypeRightMouseUp:
            case NSEventTypeOtherMouseUp: {
                NSPoint p;
                if (!contentPoint(ev, p)) return false;  // title bar: let Cocoa drag/resize
                bool down = ev.type == NSEventTypeLeftMouseDown ||
                            ev.type == NSEventTypeRightMouseDown ||
                            ev.type == NSEventTypeOtherMouseDown;
                Event event;
                event.type = down ? EventType::MouseDown : EventType::MouseUp;
                event.mouse.x = static_cast<float>(p.x);
                event.mouse.y = static_cast<float>(p.y);
                event.mouse.button = mapMouseButton(ev.buttonNumber);
                event.mouse.modifiers = mapModifiers(ev.modifierFlags);
                NXRender::dispatchEvent(event);
                return false;
            }

            case NSEventTypeScrollWheel: {
                NSPoint p;
                if (!contentPoint(ev, p)) return false;
                CGFloat dy = ev.scrollingDeltaY;
                // Trackpads report pixel deltas; scale to the per-notch units
                // the other backends use (one wheel notch = 1.0)
                if (ev.hasPreciseScrollingDeltas) dy /= 10.0;
                if (dy == 0) return true;
                Event event;
                event.type = EventType::MouseWheel;
                event.mouse.x = static_cast<float>(p.x);
                event.mouse.y = static_cast<float>(p.y);
                event.mouse.wheelDelta = static_cast<float>(dy);
                event.mouse.modifiers = mapModifiers(ev.modifierFlags);
                NXRender::dispatchEvent(event);
                return true;
            }

            default:
                return false;
        }
    }

    // Converts the event location to view points (top-left origin) and
    // reports whether it lies inside the content area.
    bool contentPoint(NSEvent* ev, NSPoint& out) const {
        if (!view_) return false;
        out = [view_ convertPoint:ev.locationInWindow fromView:nil];
        return NSPointInRect(out, view_.bounds);
    }

    void installMenu() {
        if ([NSApp mainMenu]) return;
        NSMenu* menuBar = [[NSMenu alloc] init];
        NSMenuItem* appItem = [[NSMenuItem alloc] init];
        [menuBar addItem:appItem];
        NSMenu* appMenu = [[NSMenu alloc] init];
        [appMenu addItemWithTitle:@"Hide" action:@selector(hide:) keyEquivalent:@"h"];
        [appMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
        [appMenu addItem:[NSMenuItem separatorItem]];
        [appMenu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
        appItem.submenu = appMenu;
        [NSApp setMainMenu:menuBar];
    }

    NSWindow* window_ = nil;
    NXRenderView* view_ = nil;
    NSOpenGLContext* glContext_ = nil;
    NXRenderWindowDelegate* delegate_ = nil;
    NSCursor* currentCursor_ = nil;
};

// =============================================================================
// Factory (matches linux_x11.cpp and windows_wgl.cpp)
// =============================================================================

Platform* createPlatform() {
    return new MacOSCocoaPlatform();
}

// =============================================================================
// Clipboard (NSPasteboard), used by the browser's ZepraBrowser::Clipboard
// =============================================================================

bool macClipboardSetText(const std::string& text) {
    @autoreleasepool {
        NSString* str = [[NSString alloc] initWithBytes:text.data()
                                                 length:text.size()
                                               encoding:NSUTF8StringEncoding];
        if (!str) return false;
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        return [pb setString:str forType:NSPasteboardTypeString];
    }
}

std::string macClipboardGetText() {
    @autoreleasepool {
        NSString* str = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        const char* utf8 = str.UTF8String;
        return utf8 ? std::string(utf8) : std::string();
    }
}

} // namespace NXRender

#endif // __APPLE__
