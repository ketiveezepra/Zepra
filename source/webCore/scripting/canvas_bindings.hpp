// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file canvas_bindings.hpp
 * @brief <canvas> for page scripts: width/height, getContext("2d") and the
 *        CanvasRenderingContext2D, CanvasGradient, CanvasPattern, Path2D and
 *        ImageData objects, backed by NXRender's Canvas 2D context.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace NXRender { class CanvasRenderingContext2D; }
namespace Zepra::Runtime { class Object; class VM; }

namespace Zepra::WebCore {

class DOMElement;

/// The bitmap of one <canvas> element and the page's 2D context object for it
struct CanvasSurface {
    std::unique_ptr<NXRender::CanvasRenderingContext2D> context2d;
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
};

} // namespace Zepra::WebCore
