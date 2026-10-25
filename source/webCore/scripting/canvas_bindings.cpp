// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "scripting/canvas_bindings.hpp"
#include "browser/dom.hpp"
#include "css/css_computed_style.hpp"
#include "nxgfx/canvas2d/canvas_context.h"

#include "runtime/execution/vm.hpp"
#include "runtime/execution/js_error.hpp"
#include "runtime/objects/function.hpp"
#include "runtime/objects/object.hpp"
#include "runtime/objects/value.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Zepra::WebCore {

using namespace Zepra::Runtime;
using Args = std::vector<Value>;
namespace NX = NXRender;

namespace {

constexpr int kDefaultWidth = 300;
constexpr int kDefaultHeight = 150;

// JS objects that carry native data
class CanvasGradientObject : public Object {
public:
    explicit CanvasGradientObject(NX::CanvasGradient g)
        : gradient(std::make_shared<NX::CanvasGradient>(std::move(g))) {}
    std::shared_ptr<NX::CanvasGradient> gradient;
};

class CanvasPatternObject : public Object {
public:
    explicit CanvasPatternObject(NX::CanvasPattern p) : pattern(std::move(p)) {}
    NX::CanvasPattern pattern;
};

class Path2DObject : public Object {
public:
    Path2DObject() : path(std::make_shared<NX::Path2D>()) {}
    std::shared_ptr<NX::Path2D> path;
};

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------

void defineHidden(Object* obj, const std::string& name, Value value) {
    PropertyDescriptor desc;
    desc.value = value;
    desc.attributes = PropertyAttribute::Writable | PropertyAttribute::Configurable;
    obj->defineProperty(name, desc);
}

void method(Object* obj, const char* name, size_t arity, NativeFn fn) {
    defineHidden(obj, name, Value::object(createNativeFunction(name, std::move(fn), arity)));
}

void accessor(Object* obj, const std::string& name, NativeFn getter, NativeFn setter) {
    PropertyDescriptor desc;
    desc.getter = Value::object(createNativeFunction("get " + name, std::move(getter), 0));
    if (setter) desc.setter = Value::object(createNativeFunction("set " + name, std::move(setter), 1));
    desc.attributes = PropertyAttribute::Configurable | PropertyAttribute::Enumerable;
    obj->defineProperty(name, desc);
}

Value str(const std::string& s) { return Value::string(new String(s)); }

void need(const Args& args, size_t n, const char* name) {
    if (args.size() < n) {
        throw JSError("TypeError", std::string("Failed to execute '") + name + "' on 'CanvasRenderingContext2D': " +
                                       std::to_string(n) + " arguments required, but only " +
                                       std::to_string(args.size()) + " present.");
    }
}

float num(const Args& args, size_t i, float fallback = NAN) {
    return i < args.size() ? static_cast<float>(args[i].toNumber()) : fallback;
}

template <typename T>
T* objectAs(const Value& v) {
    return v.isObject() ? dynamic_cast<T*>(v.asObject()) : nullptr;
}

// Length of an array or array-like object
size_t lengthOf(Object* obj) {
    if (obj->isArray()) return obj->length();
    double n = obj->get("length").toNumber();
    return n > 0 && std::isfinite(n) ? static_cast<size_t>(n) : 0;
}

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// rgb()/rgba()/hsl()/hsla() with comma or space syntax and an optional
// alpha (number or percentage), rounded as browsers round it
bool parseColorFunction(const std::string& s, NX::Color& out) {
    size_t open = s.find('('), close = s.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open || close + 1 != s.size()) return false;
    std::string name = s.substr(0, open);
    bool hsl = name == "hsl" || name == "hsla";
    if (!hsl && name != "rgb" && name != "rgba") return false;
    std::string inner = s.substr(open + 1, close - open - 1);
    for (char& c : inner) {
        if (c == ',' || c == '/') c = ' ';
    }
    std::vector<std::pair<double, bool>> parts;  // value, is percentage
    const char* p = inner.c_str();
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char* end = nullptr;
        double v = std::strtod(p, &end);
        if (end == p) return false;
        p = end;
        bool percent = false;
        if (*p == '%') { percent = true; p++; }
        else if (hsl && parts.empty() && std::strncmp(p, "deg", 3) == 0) p += 3;
        if (*p && *p != ' ') return false;
        parts.push_back({v, percent});
    }
    if (parts.size() != 3 && parts.size() != 4) return false;
    auto clamp01 = [](double v) { return std::clamp(v, 0.0, 1.0); };
    double alpha = parts.size() == 4 ? clamp01(parts[3].second ? parts[3].first / 100 : parts[3].first) : 1.0;
    double r, g, b;
    if (!hsl) {
        auto channel = [&](const std::pair<double, bool>& c) {
            return clamp01(c.second ? c.first / 100 : c.first / 255);
        };
        r = channel(parts[0]); g = channel(parts[1]); b = channel(parts[2]);
    } else {
        double h = std::fmod(parts[0].first, 360.0);
        if (h < 0) h += 360;
        double sat = clamp01(parts[1].first / 100), light = clamp01(parts[2].first / 100);
        auto f = [&](double n) {
            double k = std::fmod(n + h / 30, 12.0);
            double a = sat * std::min(light, 1 - light);
            return light - a * std::max(-1.0, std::min({k - 3, 9 - k, 1.0}));
        };
        r = f(0); g = f(8); b = f(4);
    }
    auto byte = [](double v) { return static_cast<uint8_t>(std::lround(v * 255)); };
    out = NX::Color(byte(r), byte(g), byte(b), byte(alpha));
    return true;
}

// A CSS colour, or false when the string isn't one
bool parseColor(const std::string& input, NX::Color& out) {
    std::string s = lowerAscii(input);
    s.erase(0, s.find_first_not_of(" \t\n\r\f"));
    s.erase(s.find_last_not_of(" \t\n\r\f") + 1);
    if (s.empty()) return false;
    if (s == "transparent") {
        out = NX::Color(0, 0, 0, 0);
        return true;
    }
    if (s == "currentcolor") {
        out = NX::Color(0, 0, 0);
        return true;
    }
    if (s.rfind("rgb", 0) == 0 || s.rfind("hsl", 0) == 0) return parseColorFunction(s, out);
    CSSColor c = CSSColor::parse(s);
    // CSSColor::parse answers black for anything it doesn't know
    bool isBlack = c.r == 0 && c.g == 0 && c.b == 0 && c.a == 255;
    if (isBlack && s != "black" && s[0] != '#' && s.rfind("rgb", 0) != 0 && s.rfind("hsl", 0) != 0) return false;
    if (s[0] == '#') {
        size_t n = s.size() - 1;
        if ((n != 3 && n != 4 && n != 6 && n != 8) ||
            s.find_first_not_of("0123456789abcdef", 1) != std::string::npos) return false;
    }
    out = NX::Color(c.r, c.g, c.b, c.a);
    return true;
}

// The canvas spec's serialization: #rrggbb when opaque, else rgba(r, g, b, a)
std::string serializeColor(const NX::Color& c) {
    char buf[64];
    if (c.a == 255) {
        std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
        return buf;
    }
    // Shortest alpha that reads back as the same byte
    std::string alpha = "0";
    for (int digits = 1; digits <= 4; digits++) {
        char a[16];
        std::snprintf(a, sizeof a, "%.*f", digits, c.a / 255.0);
        if (static_cast<int>(std::lround(std::atof(a) * 255)) == c.a || digits == 4) {
            alpha = a;
            while (alpha.size() > 1 && alpha.back() == '0') alpha.pop_back();
            if (alpha.back() == '.') alpha.pop_back();
            break;
        }
    }
    std::snprintf(buf, sizeof buf, "rgba(%d, %d, %d, %s)", c.r, c.g, c.b, alpha.c_str());
    return buf;
}

struct Named { const char* name; int value; };

const Named kCompositeOps[] = {
    {"source-over", int(NX::CompositeOp::SourceOver)}, {"source-in", int(NX::CompositeOp::SourceIn)},
    {"source-out", int(NX::CompositeOp::SourceOut)}, {"source-atop", int(NX::CompositeOp::SourceAtop)},
    {"destination-over", int(NX::CompositeOp::DestinationOver)}, {"destination-in", int(NX::CompositeOp::DestinationIn)},
    {"destination-out", int(NX::CompositeOp::DestinationOut)}, {"destination-atop", int(NX::CompositeOp::DestinationAtop)},
    {"lighter", int(NX::CompositeOp::Lighter)}, {"copy", int(NX::CompositeOp::Copy)}, {"xor", int(NX::CompositeOp::Xor)},
    {"multiply", int(NX::CompositeOp::Multiply)}, {"screen", int(NX::CompositeOp::Screen)},
    {"overlay", int(NX::CompositeOp::Overlay)}, {"darken", int(NX::CompositeOp::Darken)},
    {"lighten", int(NX::CompositeOp::Lighten)}, {"color-dodge", int(NX::CompositeOp::ColorDodge)},
    {"color-burn", int(NX::CompositeOp::ColorBurn)}, {"hard-light", int(NX::CompositeOp::HardLight)},
    {"soft-light", int(NX::CompositeOp::SoftLight)}, {"difference", int(NX::CompositeOp::Difference)},
    {"exclusion", int(NX::CompositeOp::Exclusion)}, {"hue", int(NX::CompositeOp::Hue)},
    {"saturation", int(NX::CompositeOp::Saturation)}, {"color", int(NX::CompositeOp::ColorOp)},
    {"luminosity", int(NX::CompositeOp::Luminosity)},
};
const Named kLineCaps[] = {{"butt", int(NX::LineCap::Butt)}, {"round", int(NX::LineCap::Round)},
                           {"square", int(NX::LineCap::Square)}};
const Named kLineJoins[] = {{"miter", int(NX::LineJoin::Miter)}, {"round", int(NX::LineJoin::Round)},
                            {"bevel", int(NX::LineJoin::Bevel)}};
const Named kTextAligns[] = {{"start", int(NX::TextAlign2D::Start)}, {"end", int(NX::TextAlign2D::End)},
                             {"left", int(NX::TextAlign2D::Left)}, {"right", int(NX::TextAlign2D::Right)},
                             {"center", int(NX::TextAlign2D::Center)}};
const Named kBaselines[] = {{"top", int(NX::TextBaseline::Top)}, {"hanging", int(NX::TextBaseline::Hanging)},
                            {"middle", int(NX::TextBaseline::Middle)}, {"alphabetic", int(NX::TextBaseline::Alphabetic)},
                            {"ideographic", int(NX::TextBaseline::Ideographic)}, {"bottom", int(NX::TextBaseline::Bottom)}};

template <size_t N>
bool lookup(const Named (&table)[N], const std::string& name, int& out) {
    for (const auto& entry : table) {
        if (name == entry.name) {
            out = entry.value;
            return true;
        }
    }
    return false;
}

template <size_t N>
const char* nameOf(const Named (&table)[N], int value) {
    for (const auto& entry : table) {
        if (entry.value == value) return entry.name;
    }
    return table[0].name;
}

NX::FillRule fillRule(const Value& v) {
    return v.isString() && v.toString() == "evenodd" ? NX::FillRule::EvenOdd : NX::FillRule::NonZero;
}

// Path methods shared by the context and Path2D objects; target() returns
// the native object they draw into
template <typename Target>
void definePathMethods(Object* obj, Target target) {
    method(obj, "closePath", 0, [target](Context*, const Args&) -> Value {
        target()->closePath();
        return Value::undefined();
    });
    method(obj, "moveTo", 2, [target](Context*, const Args& a) -> Value {
        need(a, 2, "moveTo");
        target()->moveTo(num(a, 0), num(a, 1));
        return Value::undefined();
    });
    method(obj, "lineTo", 2, [target](Context*, const Args& a) -> Value {
        need(a, 2, "lineTo");
        target()->lineTo(num(a, 0), num(a, 1));
        return Value::undefined();
    });
    method(obj, "quadraticCurveTo", 4, [target](Context*, const Args& a) -> Value {
        need(a, 4, "quadraticCurveTo");
        target()->quadraticCurveTo(num(a, 0), num(a, 1), num(a, 2), num(a, 3));
        return Value::undefined();
    });
    method(obj, "bezierCurveTo", 6, [target](Context*, const Args& a) -> Value {
        need(a, 6, "bezierCurveTo");
        target()->bezierCurveTo(num(a, 0), num(a, 1), num(a, 2), num(a, 3), num(a, 4), num(a, 5));
        return Value::undefined();
    });
    method(obj, "arcTo", 5, [target](Context*, const Args& a) -> Value {
        need(a, 5, "arcTo");
        if (num(a, 4) < 0) throw JSError("RangeError", "The radius provided (" + a[4].toString() + ") is negative.");
        target()->arcTo(num(a, 0), num(a, 1), num(a, 2), num(a, 3), num(a, 4));
        return Value::undefined();
    });
    method(obj, "arc", 5, [target](Context*, const Args& a) -> Value {
        need(a, 5, "arc");
        if (num(a, 2) < 0) throw JSError("RangeError", "The radius provided (" + a[2].toString() + ") is negative.");
        target()->arc(num(a, 0), num(a, 1), num(a, 2), num(a, 3), num(a, 4), a.size() > 5 && a[5].toBoolean());
        return Value::undefined();
    });
    method(obj, "ellipse", 7, [target](Context*, const Args& a) -> Value {
        need(a, 7, "ellipse");
        if (num(a, 2) < 0 || num(a, 3) < 0) throw JSError("RangeError", "The radius provided is negative.");
        target()->ellipse(num(a, 0), num(a, 1), num(a, 2), num(a, 3), num(a, 4), num(a, 5), num(a, 6),
                          a.size() > 7 && a[7].toBoolean());
        return Value::undefined();
    });
    method(obj, "rect", 4, [target](Context*, const Args& a) -> Value {
        need(a, 4, "rect");
        target()->rect(num(a, 0), num(a, 1), num(a, 2), num(a, 3));
        return Value::undefined();
    });
    method(obj, "roundRect", 4, [target](Context*, const Args& a) -> Value {
        need(a, 4, "roundRect");
        // radii: a number or a list of 1-4 numbers (top-left first, clockwise)
        float r[4] = {0, 0, 0, 0};
        std::vector<float> list;
        if (a.size() > 4 && a[4].isObject()) {
            Object* arr = a[4].asObject();
            size_t n = lengthOf(arr);
            for (size_t i = 0; i < n && i < 4; i++) list.push_back(static_cast<float>(arr->get(static_cast<uint32_t>(i)).toNumber()));
        } else if (a.size() > 4 && !a[4].isUndefined()) {
            list.push_back(num(a, 4));
        }
        if (list.size() == 1) r[0] = r[1] = r[2] = r[3] = list[0];
        else if (list.size() == 2) { r[0] = r[2] = list[0]; r[1] = r[3] = list[1]; }
        else if (list.size() == 3) { r[0] = list[0]; r[1] = r[3] = list[1]; r[2] = list[2]; }
        else if (list.size() == 4) { r[0] = list[0]; r[1] = list[1]; r[2] = list[2]; r[3] = list[3]; }
        for (float v : r) {
            if (v < 0) throw JSError("RangeError", "A radius provided to roundRect is negative.");
        }
        target()->roundRect(num(a, 0), num(a, 1), num(a, 2), num(a, 3), r[0], r[1], r[2], r[3]);
        return Value::undefined();
    });
}

// ImageData as a plain object: width, height and data (an array of bytes)
Object* makeImageData(const NX::ImageData& d) {
    auto* obj = new Object();
    obj->set("width", Value::number(d.width));
    obj->set("height", Value::number(d.height));
    std::vector<Value> bytes;
    bytes.reserve(d.data.size());
    for (uint8_t b : d.data) bytes.push_back(Value::number(b));
    obj->set("data", Value::object(new Array(std::move(bytes))));
    return obj;
}

bool readImageData(const Value& v, NX::ImageData& out) {
    if (!v.isObject()) return false;
    Object* obj = v.asObject();
    int w = static_cast<int>(obj->get("width").toNumber());
    int h = static_cast<int>(obj->get("height").toNumber());
    Value data = obj->get("data");
    if (w <= 0 || h <= 0 || !data.isObject()) return false;
    out = NX::ImageData(w, h);
    Object* bytes = data.asObject();
    size_t n = static_cast<size_t>(w) * h * 4;
    for (size_t i = 0; i < n; i++) {
        double b = bytes->get(static_cast<uint32_t>(i)).toNumber();
        out.data[i] = static_cast<uint8_t>(std::isnan(b) ? 0 : std::clamp(std::lround(b), 0L, 255L));
    }
    return true;
}

Object* makeGradient(NX::CanvasGradient g) {
    auto* obj = new CanvasGradientObject(std::move(g));
    auto gradient = obj->gradient;
    method(obj, "addColorStop", 2, [gradient](Context*, const Args& a) -> Value {
        if (a.size() < 2) throw JSError("TypeError", "Failed to execute 'addColorStop' on 'CanvasGradient': 2 arguments required.");
        double offset = a[0].toNumber();
        if (!(offset >= 0 && offset <= 1)) {
            throw JSError("IndexSizeError", "The provided value (" + a[0].toString() + ") is outside the range (0.0, 1.0).");
        }
        NX::Color color;
        if (!parseColor(a[1].toString(), color)) {
            throw JSError("SyntaxError", "The value provided ('" + a[1].toString() + "') could not be parsed as a color.");
        }
        gradient->addColorStop(static_cast<float>(offset), color);
        return Value::undefined();
    });
    return obj;
}

} // namespace

// ==================================================================
// CanvasRegistry
// ==================================================================

CanvasRegistry::CanvasRegistry(VM* vm, Object* window) : vm_(vm) {
    // Context objects live as long as the page: rooted in a global
    root_ = new Object();
    if (vm_) vm_->setGlobal("__zepraCanvases__", Value::object(root_));

    // new Path2D(), new Path2D(path), new Path2D(svgPathData)
    auto* path2dCtor = createNativeFunction("Path2D", [](Context*, const Args& a) -> Value {
        auto* obj = new Path2DObject();
        if (!a.empty()) {
            if (auto* other = objectAs<Path2DObject>(a[0])) *obj->path = *other->path;
            else if (a[0].isString()) *obj->path = NX::Path2D(a[0].toString());
        }
        auto path = obj->path;
        definePathMethods(obj, [path] { return path.get(); });
        method(obj, "addPath", 1, [path](Context*, const Args& args) -> Value {
            if (auto* other = !args.empty() ? objectAs<Path2DObject>(args[0]) : nullptr) path->addPath(*other->path);
            return Value::undefined();
        });
        return Value::object(obj);
    }, 1);
    if (vm_) vm_->setGlobal("Path2D", Value::object(path2dCtor));
    if (window) window->set("Path2D", Value::object(path2dCtor));
}

CanvasRegistry::~CanvasRegistry() {
    if (!releaseTexture_) return;
    for (auto& entry : surfaces_) {
        if (entry.second->textureId) releaseTexture_(entry.second->textureId);
    }
}

int CanvasRegistry::attributeSize(const DOMElement* canvas, const char* name) {
    const int fallback = std::string(name) == "width" ? kDefaultWidth : kDefaultHeight;
    if (!canvas || !canvas->hasAttribute(name)) return fallback;
    // The rules for parsing non-negative integers
    std::string v = canvas->getAttribute(name);
    size_t i = v.find_first_not_of(" \t\n\r\f");
    if (i == std::string::npos) return fallback;
    if (v[i] == '+') i++;
    if (i >= v.size() || !std::isdigit(static_cast<unsigned char>(v[i]))) return fallback;
    long long n = 0;
    while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
        n = n * 10 + (v[i] - '0');
        if (n > 0x7fffffff) return fallback;
        i++;
    }
    return static_cast<int>(n);
}

int CanvasRegistry::idFor(const DOMElement* canvas) {
    for (const auto& [id, element] : elementsById_) {
        if (element == canvas) return id;
    }
    int id = nextId_++;
    elementsById_[id] = canvas;
    return id;
}

CanvasSurface* CanvasRegistry::find(const DOMElement* canvas) {
    auto it = surfaces_.find(canvas);
    return it == surfaces_.end() ? nullptr : it->second.get();
}

CanvasSurface* CanvasRegistry::surfaceById(int id) {
    auto it = elementsById_.find(id);
    return it == elementsById_.end() ? nullptr : find(it->second);
}

CanvasSurface& CanvasRegistry::surfaceFor(DOMElement* canvas) {
    auto& slot = surfaces_[canvas];
    if (!slot) {
        slot = std::make_unique<CanvasSurface>();
        slot->id = idFor(canvas);
        slot->context2d = std::make_unique<NX::CanvasRenderingContext2D>(
            attributeSize(canvas, "width"), attributeSize(canvas, "height"));
    }
    return *slot;
}

void CanvasRegistry::attributeChanged(DOMElement* canvas, const std::string& name) {
    if (name != "width" && name != "height") return;
    if (CanvasSurface* s = find(canvas)) {
        // Setting either dimension resets the bitmap and the context state
        s->context2d->resize(attributeSize(canvas, "width"), attributeSize(canvas, "height"));
        if (s->jsContext) {
            defineHidden(s->jsContext, "__fillStyle", Value::undefined());
            defineHidden(s->jsContext, "__strokeStyle", Value::undefined());
            defineHidden(s->jsContext, "__styleStack", Value::object(new Array()));
        }
    }
}

void CanvasRegistry::decorate(Object* wrapper, DOMElement* canvas) {
    if (!wrapper || !canvas) return;
    defineHidden(wrapper, "__canvasId", Value::number(idFor(canvas)));
    for (const char* dim : {"width", "height"}) {
        std::string name = dim;
        accessor(wrapper, name,
            [canvas, name](Context*, const Args&) -> Value {
                return Value::number(attributeSize(canvas, name.c_str()));
            },
            [this, canvas, name](Context*, const Args& a) -> Value {
                double v = a.empty() ? 0 : a[0].toNumber();
                long long n = std::isfinite(v) && v >= 0 && v <= 0x7fffffff ? static_cast<long long>(v) : 0;
                canvas->setAttribute(name, std::to_string(n));
                attributeChanged(canvas, name);
                return Value::undefined();
            });
    }
    method(wrapper, "getContext", 1, [this, canvas, wrapper](Context*, const Args& a) -> Value {
        if (a.empty()) throw JSError("TypeError", "Failed to execute 'getContext' on 'HTMLCanvasElement': 1 argument required.");
        std::string type = a[0].toString();
        if (type != "2d") return Value::null();  // webgl, webgpu, bitmaprenderer: not supported
        CanvasSurface& s = surfaceFor(canvas);
        if (!s.jsContext) {
            s.jsContext = createContext2D(s, wrapper);
            root_->set("c" + std::to_string(s.id), Value::object(s.jsContext));
        }
        return Value::object(s.jsContext);
    });
    method(wrapper, "toDataURL", 0, [](Context*, const Args&) -> Value {
        return str("data:,");  // encoding the bitmap is not supported yet
    });
}

// ==================================================================
// CanvasRenderingContext2D object
// ==================================================================

Object* CanvasRegistry::createContext2D(CanvasSurface& surface, Object* canvasWrapper) {
    auto* obj = new Object();
    NX::CanvasRenderingContext2D* ctx = surface.context2d.get();
    // The JS values of fillStyle/strokeStyle (strings or gradient/pattern
    // objects) and their save() stack, kept on the object so the collector sees them
    defineHidden(obj, "__fillStyle", Value::undefined());
    defineHidden(obj, "__strokeStyle", Value::undefined());
    defineHidden(obj, "__styleStack", Value::object(new Array()));
    defineHidden(obj, "canvas", Value::object(canvasWrapper));

    // Gradients stay live after assignment: copy their stops in before drawing
    auto syncStyles = [obj, ctx] {
        if (auto* g = objectAs<CanvasGradientObject>(obj->get("__fillStyle"))) ctx->setFillStyle(*g->gradient);
        if (auto* g = objectAs<CanvasGradientObject>(obj->get("__strokeStyle"))) ctx->setStrokeStyle(*g->gradient);
    };
    auto setStyle = [obj, ctx](const Value& v, bool fill) {
        const char* slot = fill ? "__fillStyle" : "__strokeStyle";
        if (auto* g = objectAs<CanvasGradientObject>(v)) {
            fill ? ctx->setFillStyle(*g->gradient) : ctx->setStrokeStyle(*g->gradient);
            defineHidden(obj, slot, v);
        } else if (auto* p = objectAs<CanvasPatternObject>(v)) {
            fill ? ctx->setFillStyle(p->pattern) : ctx->setStrokeStyle(p->pattern);
            defineHidden(obj, slot, v);
        } else {
            NX::Color color;
            if (!parseColor(v.toString(), color)) return;  // invalid values are ignored
            fill ? ctx->setFillStyle(color) : ctx->setStrokeStyle(color);
            defineHidden(obj, slot, Value::undefined());
        }
    };
    auto getStyle = [obj, ctx](bool fill) -> Value {
        Value v = obj->get(fill ? "__fillStyle" : "__strokeStyle");
        if (v.isObject()) return v;
        const NX::CanvasFillStyle& s = fill ? ctx->fillStyle() : ctx->strokeStyle();
        return str(serializeColor(s.color));
    };

    // --- State ---
    method(obj, "save", 0, [obj, ctx](Context*, const Args&) -> Value {
        ctx->save();
        Value stack = obj->get("__styleStack");
        if (auto* arr = objectAs<Array>(stack)) {
            arr->push(obj->get("__fillStyle"));
            arr->push(obj->get("__strokeStyle"));
        }
        return Value::undefined();
    });
    method(obj, "restore", 0, [obj, ctx](Context*, const Args&) -> Value {
        ctx->restore();
        if (auto* arr = objectAs<Array>(obj->get("__styleStack"))) {
            if (arr->length() >= 2) {
                Value stroke = arr->pop();
                Value fill = arr->pop();
                defineHidden(obj, "__strokeStyle", stroke);
                defineHidden(obj, "__fillStyle", fill);
            }
        }
        return Value::undefined();
    });
    method(obj, "reset", 0, [obj, ctx](Context*, const Args&) -> Value {
        ctx->reset();
        defineHidden(obj, "__fillStyle", Value::undefined());
        defineHidden(obj, "__strokeStyle", Value::undefined());
        defineHidden(obj, "__styleStack", Value::object(new Array()));
        return Value::undefined();
    });

    // --- Transforms ---
    method(obj, "scale", 2, [ctx](Context*, const Args& a) -> Value {
        need(a, 2, "scale");
        ctx->scale(num(a, 0), num(a, 1));
        return Value::undefined();
    });
    method(obj, "rotate", 1, [ctx](Context*, const Args& a) -> Value {
        need(a, 1, "rotate");
        ctx->rotate(num(a, 0));
        return Value::undefined();
    });
    method(obj, "translate", 2, [ctx](Context*, const Args& a) -> Value {
        need(a, 2, "translate");
        ctx->translate(num(a, 0), num(a, 1));
        return Value::undefined();
    });
    method(obj, "transform", 6, [ctx](Context*, const Args& a) -> Value {
        need(a, 6, "transform");
        ctx->transform(num(a, 0), num(a, 1), num(a, 2), num(a, 3), num(a, 4), num(a, 5));
        return Value::undefined();
    });
    method(obj, "setTransform", 6, [ctx](Context*, const Args& a) -> Value {
        if (a.empty() || a[0].isUndefined()) {
            ctx->resetTransform();
        } else if (a[0].isObject()) {
            // A DOMMatrix-like object: a..f
            Object* m = a[0].asObject();
            auto field = [m](const char* k, float fallback) {
                Value v = m->get(k);
                return v.isUndefined() ? fallback : static_cast<float>(v.toNumber());
            };
            ctx->setTransform(field("a", 1), field("b", 0), field("c", 0), field("d", 1), field("e", 0), field("f", 0));
        } else {
            need(a, 6, "setTransform");
            ctx->setTransform(num(a, 0), num(a, 1), num(a, 2), num(a, 3), num(a, 4), num(a, 5));
        }
        return Value::undefined();
    });
    method(obj, "resetTransform", 0, [ctx](Context*, const Args&) -> Value {
        ctx->resetTransform();
        return Value::undefined();
    });
    method(obj, "getTransform", 0, [ctx](Context*, const Args&) -> Value {
        const NX::CanvasMatrix& m = ctx->currentTransform();
        auto* r = new Object();
        const std::pair<const char*, float> fields[] = {
            {"a", m.a}, {"b", m.b}, {"c", m.c}, {"d", m.d}, {"e", m.e}, {"f", m.f},
            {"m11", m.a}, {"m12", m.b}, {"m21", m.c}, {"m22", m.d}, {"m41", m.e}, {"m42", m.f}};
        for (const auto& [k, v] : fields) r->set(k, Value::number(v));
        r->set("is2D", Value::boolean(true));
        r->set("isIdentity", Value::boolean(m.a == 1 && m.b == 0 && m.c == 0 && m.d == 1 && m.e == 0 && m.f == 0));
        return Value::object(r);
    });

    // --- Styles ---
    accessor(obj, "fillStyle",
        [getStyle](Context*, const Args&) -> Value { return getStyle(true); },
        [setStyle](Context*, const Args& a) -> Value {
            if (!a.empty()) setStyle(a[0], true);
            return Value::undefined();
        });
    accessor(obj, "strokeStyle",
        [getStyle](Context*, const Args&) -> Value { return getStyle(false); },
        [setStyle](Context*, const Args& a) -> Value {
            if (!a.empty()) setStyle(a[0], false);
            return Value::undefined();
        });

    auto numberProperty = [obj](const char* name, std::function<float()> get, std::function<void(float)> set) {
        accessor(obj, name,
            [get](Context*, const Args&) -> Value { return Value::number(get()); },
            [set](Context*, const Args& a) -> Value {
                if (!a.empty()) set(static_cast<float>(a[0].toNumber()));
                return Value::undefined();
            });
    };
    numberProperty("lineWidth", [ctx] { return ctx->lineWidth(); }, [ctx](float v) { ctx->setLineWidth(v); });
    numberProperty("miterLimit", [ctx] { return ctx->miterLimit(); }, [ctx](float v) { ctx->setMiterLimit(v); });
    numberProperty("lineDashOffset", [ctx] { return ctx->lineDashOffset(); }, [ctx](float v) { ctx->setLineDashOffset(v); });
    numberProperty("globalAlpha", [ctx] { return ctx->globalAlpha(); }, [ctx](float v) { ctx->setGlobalAlpha(v); });

    // Shadows are accepted (and read back) but not drawn yet
    auto shadow = std::make_shared<std::array<float, 3>>(std::array<float, 3>{0, 0, 0});
    auto shadowColor = std::make_shared<NX::Color>(0, 0, 0, 0);
    numberProperty("shadowBlur", [shadow] { return (*shadow)[0]; },
                   [shadow, ctx](float v) { if (std::isfinite(v) && v >= 0) { (*shadow)[0] = v; ctx->setShadowBlur(v); } });
    numberProperty("shadowOffsetX", [shadow] { return (*shadow)[1]; },
                   [shadow, ctx](float v) { if (std::isfinite(v)) { (*shadow)[1] = v; ctx->setShadowOffsetX(v); } });
    numberProperty("shadowOffsetY", [shadow] { return (*shadow)[2]; },
                   [shadow, ctx](float v) { if (std::isfinite(v)) { (*shadow)[2] = v; ctx->setShadowOffsetY(v); } });
    accessor(obj, "shadowColor",
        [shadowColor](Context*, const Args&) -> Value { return str(serializeColor(*shadowColor)); },
        [shadowColor, ctx](Context*, const Args& a) -> Value {
            NX::Color c;
            if (!a.empty() && parseColor(a[0].toString(), c)) {
                *shadowColor = c;
                ctx->setShadowColor(c);
            }
            return Value::undefined();
        });

    auto enumProperty = [obj](const char* name, auto& table, std::function<int()> get, std::function<void(int)> set) {
        accessor(obj, name,
            [&table, get](Context*, const Args&) -> Value { return str(nameOf(table, get())); },
            [&table, set](Context*, const Args& a) -> Value {
                int v;
                if (!a.empty() && a[0].isString() && lookup(table, a[0].toString(), v)) set(v);
                return Value::undefined();
            });
    };
    enumProperty("lineCap", kLineCaps, [ctx] { return int(ctx->lineCap()); },
                 [ctx](int v) { ctx->setLineCap(NX::LineCap(v)); });
    enumProperty("lineJoin", kLineJoins, [ctx] { return int(ctx->lineJoin()); },
                 [ctx](int v) { ctx->setLineJoin(NX::LineJoin(v)); });
    enumProperty("globalCompositeOperation", kCompositeOps, [ctx] { return int(ctx->globalCompositeOperation()); },
                 [ctx](int v) { ctx->setGlobalCompositeOperation(NX::CompositeOp(v)); });
    enumProperty("textAlign", kTextAligns, [ctx] { return int(ctx->textAlign()); },
                 [ctx](int v) { ctx->setTextAlign(NX::TextAlign2D(v)); });
    enumProperty("textBaseline", kBaselines, [ctx] { return int(ctx->textBaseline()); },
                 [ctx](int v) { ctx->setTextBaseline(NX::TextBaseline(v)); });

    accessor(obj, "font",
        [ctx](Context*, const Args&) -> Value { return str(ctx->font()); },
        [ctx](Context*, const Args& a) -> Value {
            if (!a.empty()) ctx->setFont(a[0].toString());
            return Value::undefined();
        });
    auto direction = std::make_shared<std::string>("inherit");
    accessor(obj, "direction",
        [direction](Context*, const Args&) -> Value { return str(*direction); },
        [direction, ctx](Context*, const Args& a) -> Value {
            std::string d = a.empty() ? "" : a[0].toString();
            if (d == "ltr" || d == "rtl" || d == "inherit") {
                *direction = d;
                ctx->setDirection(d == "rtl" ? "rtl" : "ltr");
            }
            return Value::undefined();
        });
    auto smoothing = std::make_shared<bool>(true);
    accessor(obj, "imageSmoothingEnabled",
        [smoothing](Context*, const Args&) -> Value { return Value::boolean(*smoothing); },
        [smoothing](Context*, const Args& a) -> Value {
            if (!a.empty()) *smoothing = a[0].toBoolean();
            return Value::undefined();
        });
    auto filter = std::make_shared<std::string>("none");
    accessor(obj, "filter",
        [filter](Context*, const Args&) -> Value { return str(*filter); },
        [filter, ctx](Context*, const Args& a) -> Value {
            if (!a.empty()) {
                *filter = a[0].toString();
                ctx->setFilter(*filter);
            }
            return Value::undefined();
        });

    method(obj, "setLineDash", 1, [ctx](Context*, const Args& a) -> Value {
        need(a, 1, "setLineDash");
        std::vector<float> segments;
        if (a[0].isObject()) {
            Object* arr = a[0].asObject();
            size_t n = lengthOf(arr);
            for (size_t i = 0; i < n && i < 1024; i++) {
                segments.push_back(static_cast<float>(arr->get(static_cast<uint32_t>(i)).toNumber()));
            }
        }
        ctx->setLineDash(segments);
        return Value::undefined();
    });
    method(obj, "getLineDash", 0, [ctx](Context*, const Args&) -> Value {
        std::vector<Value> out;
        for (float v : ctx->getLineDash()) out.push_back(Value::number(v));
        return Value::object(new Array(std::move(out)));
    });

    // --- Paths ---
    method(obj, "beginPath", 0, [ctx](Context*, const Args&) -> Value {
        ctx->beginPath();
        return Value::undefined();
    });
    definePathMethods(obj, [ctx] { return ctx; });

    method(obj, "fill", 0, [ctx, syncStyles](Context*, const Args& a) -> Value {
        syncStyles();
        if (auto* p = !a.empty() ? objectAs<Path2DObject>(a[0]) : nullptr) {
            ctx->fill(*p->path, fillRule(a.size() > 1 ? a[1] : Value::undefined()));
        } else {
            ctx->fill(fillRule(a.empty() ? Value::undefined() : a[0]));
        }
        return Value::undefined();
    });
    method(obj, "stroke", 0, [ctx, syncStyles](Context*, const Args& a) -> Value {
        syncStyles();
        if (auto* p = !a.empty() ? objectAs<Path2DObject>(a[0]) : nullptr) ctx->stroke(*p->path);
        else ctx->stroke();
        return Value::undefined();
    });
    method(obj, "clip", 0, [ctx](Context*, const Args& a) -> Value {
        if (auto* p = !a.empty() ? objectAs<Path2DObject>(a[0]) : nullptr) {
            ctx->clip(*p->path, fillRule(a.size() > 1 ? a[1] : Value::undefined()));
        } else {
            ctx->clip(fillRule(a.empty() ? Value::undefined() : a[0]));
        }
        return Value::undefined();
    });
    method(obj, "isPointInPath", 2, [ctx](Context*, const Args& a) -> Value {
        if (!a.empty() && objectAs<Path2DObject>(a[0])) return Value::boolean(false);  // Path2D form: not supported
        need(a, 2, "isPointInPath");
        return Value::boolean(ctx->isPointInPath(num(a, 0), num(a, 1), fillRule(a.size() > 2 ? a[2] : Value::undefined())));
    });
    method(obj, "isPointInStroke", 2, [ctx](Context*, const Args& a) -> Value {
        if (!a.empty() && objectAs<Path2DObject>(a[0])) return Value::boolean(false);
        need(a, 2, "isPointInStroke");
        return Value::boolean(ctx->isPointInStroke(num(a, 0), num(a, 1)));
    });

    // --- Rectangles ---
    method(obj, "clearRect", 4, [ctx](Context*, const Args& a) -> Value {
        need(a, 4, "clearRect");
        ctx->clearRect(num(a, 0), num(a, 1), num(a, 2), num(a, 3));
        return Value::undefined();
    });
    method(obj, "fillRect", 4, [ctx, syncStyles](Context*, const Args& a) -> Value {
        need(a, 4, "fillRect");
        syncStyles();
        ctx->fillRect(num(a, 0), num(a, 1), num(a, 2), num(a, 3));
        return Value::undefined();
    });
    method(obj, "strokeRect", 4, [ctx, syncStyles](Context*, const Args& a) -> Value {
        need(a, 4, "strokeRect");
        syncStyles();
        ctx->strokeRect(num(a, 0), num(a, 1), num(a, 2), num(a, 3));
        return Value::undefined();
    });

    // --- Text ---
    auto textMethod = [ctx, syncStyles](bool stroke) {
        return [ctx, syncStyles, stroke](Context*, const Args& a) -> Value {
            need(a, 3, stroke ? "strokeText" : "fillText");
            float maxWidth = -1;
            if (a.size() > 3 && !a[3].isUndefined()) {
                maxWidth = num(a, 3);
                if (!(maxWidth > 0)) return Value::undefined();  // NaN, 0 or negative draws nothing
            }
            syncStyles();
            if (stroke) ctx->strokeText(a[0].toString(), num(a, 1), num(a, 2), maxWidth);
            else ctx->fillText(a[0].toString(), num(a, 1), num(a, 2), maxWidth);
            return Value::undefined();
        };
    };
    method(obj, "fillText", 3, textMethod(false));
    method(obj, "strokeText", 3, textMethod(true));
    method(obj, "measureText", 1, [ctx](Context*, const Args& a) -> Value {
        need(a, 1, "measureText");
        NX::CanvasTextMetrics m = ctx->measureText(a[0].toString());
        auto* r = new Object();
        const std::pair<const char*, float> fields[] = {
            {"width", m.width}, {"actualBoundingBoxLeft", m.actualBoundingBoxLeft},
            {"actualBoundingBoxRight", m.actualBoundingBoxRight}, {"actualBoundingBoxAscent", m.actualBoundingBoxAscent},
            {"actualBoundingBoxDescent", m.actualBoundingBoxDescent}, {"fontBoundingBoxAscent", m.fontBoundingBoxAscent},
            {"fontBoundingBoxDescent", m.fontBoundingBoxDescent}, {"emHeightAscent", m.emHeightAscent},
            {"emHeightDescent", m.emHeightDescent}, {"hangingBaseline", m.hangingBaseline},
            {"alphabeticBaseline", m.alphabeticBaseline}, {"ideographicBaseline", m.ideographicBaseline}};
        for (const auto& [k, v] : fields) r->set(k, Value::number(v));
        return Value::object(r);
    });

    // --- Gradients and patterns ---
    method(obj, "createLinearGradient", 4, [ctx](Context*, const Args& a) -> Value {
        need(a, 4, "createLinearGradient");
        return Value::object(makeGradient(ctx->createLinearGradient(num(a, 0), num(a, 1), num(a, 2), num(a, 3))));
    });
    method(obj, "createRadialGradient", 6, [ctx](Context*, const Args& a) -> Value {
        need(a, 6, "createRadialGradient");
        if (num(a, 2) < 0 || num(a, 5) < 0) throw JSError("IndexSizeError", "The radius provided is negative.");
        return Value::object(makeGradient(ctx->createRadialGradient(num(a, 0), num(a, 1), num(a, 2),
                                                                     num(a, 3), num(a, 4), num(a, 5))));
    });
    method(obj, "createConicGradient", 3, [ctx](Context*, const Args& a) -> Value {
        need(a, 3, "createConicGradient");
        return Value::object(makeGradient(ctx->createConicGradient(num(a, 0), num(a, 1), num(a, 2))));
    });
    // The pixels of a canvas wrapper passed as an image (null for anything else)
    auto imageOf = [this](const Value& v) -> std::shared_ptr<NX::CanvasImage> {
        if (!v.isObject()) return nullptr;
        Value id = v.asObject()->get("__canvasId");
        if (!id.isNumber()) return nullptr;
        CanvasSurface* s = surfaceById(static_cast<int>(id.toNumber()));
        if (s) return s->context2d->snapshot();
        // A canvas that has no context yet is transparent
        auto blank = std::make_shared<NX::CanvasImage>();
        return blank;
    };
    method(obj, "createPattern", 2, [ctx, imageOf](Context*, const Args& a) -> Value {
        need(a, 2, "createPattern");
        auto image = imageOf(a[0]);
        if (!image || image->width == 0 || image->height == 0) return Value::null();
        std::string rep = a[1].isNull() || a[1].isUndefined() ? "" : a[1].toString();
        NX::RepeatMode mode;
        if (rep.empty() || rep == "repeat") mode = NX::RepeatMode::Repeat;
        else if (rep == "repeat-x") mode = NX::RepeatMode::RepeatX;
        else if (rep == "repeat-y") mode = NX::RepeatMode::RepeatY;
        else if (rep == "no-repeat") mode = NX::RepeatMode::NoRepeat;
        else throw JSError("SyntaxError", "The provided type ('" + rep + "') is not one of 'repeat', 'no-repeat', 'repeat-x', or 'repeat-y'.");
        auto* p = new CanvasPatternObject(ctx->createPattern(image, mode));
        auto* pattern = &p->pattern;
        method(p, "setTransform", 0, [pattern](Context*, const Args& args) -> Value {
            NX::CanvasMatrix m;
            if (!args.empty() && args[0].isObject()) {
                Object* o = args[0].asObject();
                auto field = [o](const char* k, float fallback) {
                    Value v = o->get(k);
                    return v.isUndefined() ? fallback : static_cast<float>(v.toNumber());
                };
                m = {field("a", 1), field("b", 0), field("c", 0), field("d", 1), field("e", 0), field("f", 0)};
            }
            // The pattern maps user space to image pixels through the inverse
            if (m.invertible()) pattern->transform = m.inverse();
            return Value::undefined();
        });
        return Value::object(p);
    });
    method(obj, "drawImage", 3, [ctx, imageOf](Context*, const Args& a) -> Value {
        need(a, 3, "drawImage");
        auto image = imageOf(a[0]);
        if (!image || image->width == 0 || image->height == 0) return Value::undefined();  // <img> etc.: not supported
        if (a.size() >= 9) {
            ctx->drawImage(*image, num(a, 1), num(a, 2), num(a, 3), num(a, 4), num(a, 5), num(a, 6), num(a, 7), num(a, 8));
        } else if (a.size() >= 5) {
            ctx->drawImage(*image, num(a, 1), num(a, 2), num(a, 3), num(a, 4));
        } else {
            ctx->drawImage(*image, num(a, 1), num(a, 2));
        }
        return Value::undefined();
    });

    // --- Pixels ---
    method(obj, "createImageData", 1, [ctx](Context*, const Args& a) -> Value {
        need(a, 1, "createImageData");
        int w, h;
        if (a[0].isObject()) {
            w = static_cast<int>(a[0].asObject()->get("width").toNumber());
            h = static_cast<int>(a[0].asObject()->get("height").toNumber());
        } else {
            need(a, 2, "createImageData");
            w = static_cast<int>(std::fabs(num(a, 0)));
            h = static_cast<int>(std::fabs(num(a, 1)));
        }
        if (w <= 0 || h <= 0) throw JSError("IndexSizeError", "The source width or height is 0.");
        return Value::object(makeImageData(ctx->createImageData(w, h)));
    });
    method(obj, "getImageData", 4, [ctx](Context*, const Args& a) -> Value {
        need(a, 4, "getImageData");
        int sw = static_cast<int>(num(a, 2)), sh = static_cast<int>(num(a, 3));
        if (sw == 0 || sh == 0) throw JSError("IndexSizeError", "The source width or height is 0.");
        return Value::object(makeImageData(ctx->getImageData(static_cast<int>(num(a, 0)), static_cast<int>(num(a, 1)), sw, sh)));
    });
    method(obj, "putImageData", 3, [ctx](Context*, const Args& a) -> Value {
        need(a, 3, "putImageData");
        NX::ImageData data;
        if (!readImageData(a[0], data)) throw JSError("TypeError", "Failed to execute 'putImageData': parameter 1 is not of type 'ImageData'.");
        int dx = static_cast<int>(num(a, 1)), dy = static_cast<int>(num(a, 2));
        if (a.size() >= 7) {
            ctx->putImageData(data, dx, dy, static_cast<int>(num(a, 3)), static_cast<int>(num(a, 4)),
                              static_cast<int>(num(a, 5)), static_cast<int>(num(a, 6)));
        } else {
            ctx->putImageData(data, dx, dy);
        }
        return Value::undefined();
    });
    return obj;
}

} // namespace Zepra::WebCore
