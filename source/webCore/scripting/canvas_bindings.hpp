// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file canvas_bindings.hpp
 * @brief <canvas> for page scripts: width/height, getContext("2d") and the
 *        CanvasRenderingContext2D, CanvasGradient, CanvasPattern, Path2D and
 *        ImageData objects, backed by NXRender's Canvas 2D context, and
 *        getContext("webgl") backed by NXRender's WebGL context
 *        (webgl_bindings.cpp).
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace NXRender { class CanvasRenderingContext2D; class WebGLRenderingContext; }
namespace Zepra::Runtime { class Object; class Value; class VM; }
namespace Zepra::Builtins { class TypedArrayObject; enum class TypedArrayType : uint8_t; }

namespace Zepra::WebCore {

class DOMElement;

/// The bitmap of one <canvas> element and the page's context object for it:
/// either a 2D context or a WebGL one, created by the first getContext()
struct CanvasSurface {
    CanvasSurface();
    ~CanvasSurface();
    std::unique_ptr<NXRender::CanvasRenderingContext2D> context2d;
    std::unique_ptr<NXRender::WebGLRenderingContext> webgl;
    Zepra::Runtime::Object* jsContext = nullptr;  // rooted by CanvasRegistry
    int id = 0;
    // Owned by whoever paints the canvas: its texture and the bitmap version it holds
    uint32_t textureId = 0;
    uint64_t textureVersion = 0;
};

/// The canvases of one page (one per ScriptContext)
class CanvasRegistry {
public:
    /// Puts Path2D on the global and window objects
    CanvasRegistry(Zepra::Runtime::VM* vm, Zepra::Runtime::Object* window);
    ~CanvasRegistry();
    CanvasRegistry(const CanvasRegistry&) = delete;
    CanvasRegistry& operator=(const CanvasRegistry&) = delete;

    /// Adds width, height and getContext() to the JS wrapper of a <canvas>
    void decorate(Zepra::Runtime::Object* wrapper, DOMElement* canvas);
    /// After the width or height attribute of a canvas changed
    void attributeChanged(DOMElement* canvas, const std::string& name);

    /// The surface of a canvas whose context was created, else null
    CanvasSurface* find(const DOMElement* canvas);
    /// Called with each texture still held when the registry goes away
    void setTextureReleaser(std::function<void(uint32_t)> releaser) { releaseTexture_ = std::move(releaser); }

    /// width/height attribute as a canvas reads it (a non-negative integer, else the default)
    static int attributeSize(const DOMElement* canvas, const char* name);

    /// A zeroed typed array with the page's prototypes (Uint8ClampedArray for ImageData, ...)
    Zepra::Builtins::TypedArrayObject* newTypedArray(Zepra::Builtins::TypedArrayType type, size_t length);

private:
    Zepra::Runtime::VM* vm_;
    Zepra::Runtime::Object* root_;  // a global object that keeps the context objects alive
    int nextId_ = 1;
    std::unordered_map<const DOMElement*, std::unique_ptr<CanvasSurface>> surfaces_;
    std::unordered_map<int, const DOMElement*> elementsById_;
    std::function<void(uint32_t)> releaseTexture_;

    int idFor(const DOMElement* canvas);
    CanvasSurface& surfaceFor(DOMElement* canvas);
    CanvasSurface* surfaceById(int id);
    Zepra::Runtime::Object* createContext2D(CanvasSurface& surface, Zepra::Runtime::Object* canvasWrapper);

    // WebGL (webgl_bindings.cpp): one WebGLRenderingContext.prototype per page
    Zepra::Runtime::Object* webglPrototype_ = nullptr;
    void installWebGL(Zepra::Runtime::Object* window);
    Zepra::Runtime::Object* createWebGLContext(CanvasSurface& surface, Zepra::Runtime::Object* canvasWrapper,
                                               const Zepra::Runtime::Value& attributes);
};

} // namespace Zepra::WebCore
