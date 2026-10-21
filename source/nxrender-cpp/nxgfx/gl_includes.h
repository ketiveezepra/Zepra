// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file gl_includes.h
 * @brief Internal header — portable OpenGL includes for NXRender.
 *
 * Use this instead of bare #include "nxgfx/gl_includes.h" in any NXRender .cpp file.
 * On Windows, gl.h only exposes OpenGL 1.1 constants.  This header
 * pulls in glext.h where available and #defines any still-missing
 * constants to their official hex values.
 *
 * Linux/NeolyxOS paths are unchanged.
 */

#pragma once

// ── Platform GL headers ─────────────────────────────────────────────────────
#ifdef _WIN32
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   include <windows.h>
#   include <GL/gl.h>
#   ifdef __MINGW32__
#       include <GL/glext.h>
#   endif
#elif defined(__APPLE__)
#   ifndef GL_SILENCE_DEPRECATION
#       define GL_SILENCE_DEPRECATION
#   endif
#   include <OpenGL/gl.h>
#   include <OpenGL/glext.h>
#   include <dlfcn.h>
#   include <cstring>
#   include <string>
#else
#   include <GL/gl.h>
#endif

#ifdef __APPLE__
// macOS has no glXGetProcAddress/wglGetProcAddress: OpenGL.framework exports
// every entry point. NXRender runs in a legacy (2.1) context because it uses
// the fixed-function pipeline, and there vertex array objects exist only as
// APPLE_vertex_array_object.
inline void* nxAppleGetProcAddress(const char* name) {
    static const char* const kAppleVertexArrayFuncs[] = {
        "glGenVertexArrays", "glBindVertexArray", "glDeleteVertexArrays", "glIsVertexArray"
    };
    for (const char* fn : kAppleVertexArrayFuncs) {
        if (std::strcmp(name, fn) == 0) {
            return dlsym(RTLD_DEFAULT, (std::string(name) + "APPLE").c_str());
        }
    }
    return dlsym(RTLD_DEFAULT, name);
}
#endif

// ── OpenGL 1.2+ constants that may be absent from Windows gl.h ──────────────
#ifndef GL_CLAMP_TO_EDGE
#   define GL_CLAMP_TO_EDGE         0x812F
#endif
#ifndef GL_MULTISAMPLE
#   define GL_MULTISAMPLE           0x809D
#endif
#ifndef GL_LINE_SMOOTH
#   define GL_LINE_SMOOTH           0x0B20
#endif
#ifndef GL_LINE_SMOOTH_HINT
#   define GL_LINE_SMOOTH_HINT      0x0C52
#endif
#ifndef GL_RGBA8
#   define GL_RGBA8                 0x8058
#endif

// FBO / render target constants (OpenGL 3.0 / ARB_framebuffer_object)
#ifndef GL_FRAMEBUFFER
#   define GL_FRAMEBUFFER           0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#   define GL_COLOR_ATTACHMENT0     0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#   define GL_FRAMEBUFFER_COMPLETE  0x8CD5
#endif

// Shader constants (OpenGL 2.0)
#ifndef GL_FRAGMENT_SHADER
#   define GL_FRAGMENT_SHADER       0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#   define GL_VERTEX_SHADER         0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#   define GL_COMPILE_STATUS        0x8B81
#endif
#ifndef GL_LINK_STATUS
#   define GL_LINK_STATUS           0x8B82
#endif
#ifndef GL_INFO_LOG_LENGTH
#   define GL_INFO_LOG_LENGTH       0x8B84
#endif

// Texture formats
#ifndef GL_ALPHA
#   define GL_ALPHA                 0x1906
#endif
#ifndef GL_RED
#   define GL_RED                   0x1903
#endif
#ifndef GL_R8
#   define GL_R8                    0x8229
#endif
#ifndef GL_RG
#   define GL_RG                    0x8227
#endif
#ifndef GL_BGRA
#   define GL_BGRA                  0x80E1
#endif

// Texture wrap modes
#ifndef GL_CLAMP_TO_BORDER
#   define GL_CLAMP_TO_BORDER      0x812D
#endif

// Texture units (OpenGL 1.3)
#ifndef GL_TEXTURE0
#   define GL_TEXTURE0             0x84C0
#endif

// Misc
#ifndef GL_FUNC_ADD
#   define GL_FUNC_ADD             0x8006
#endif
#ifndef GL_STENCIL_BUFFER_BIT
#   define GL_STENCIL_BUFFER_BIT   0x00000400
#endif
