// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file css_animations.cpp
 * @brief CSS animations and transitions over time (see css_animations.hpp)
 */

#include "css/css_animations.hpp"

#include "browser/dom.hpp"
#include "css/computed_style_values.hpp"
#include "css/css_engine.hpp"
#include "css/css_rule.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace Zepra::WebCore {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\f"), b = s.find_last_not_of(" \t\r\n\f");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// A comma-separated list; commas inside functions stay
std::vector<std::string> commaList(const std::string& value) {
    std::vector<std::string> out;
    std::string item;
    int depth = 0;
    for (char c : value) {
        if (c == '(') depth++;
        else if (c == ')' && depth > 0) depth--;
        if (c == ',' && depth == 0) {
            out.push_back(trim(item));
            item.clear();
            continue;
        }
        item += c;
    }
    if (!trim(item).empty() || !out.empty()) out.push_back(trim(item));
    return out;
}

// Item i of a list that repeats to the length of its main list (CSS
// Animations 1 §4.2, CSS Transitions 1 §2)
std::string at(const std::vector<std::string>& list, size_t i, const char* initial) {
    if (list.empty() || (list.size() == 1 && list[0].empty())) return initial;
    return list[i % list.size()];
}

double cubicBezier(double x1, double y1, double x2, double y2, double x) {
    // The curve's t for x by Newton steps, then bisection when they stall
    auto bx = [&](double t) { return 3 * (1 - t) * (1 - t) * t * x1 + 3 * (1 - t) * t * t * x2 + t * t * t; };
    auto by = [&](double t) { return 3 * (1 - t) * (1 - t) * t * y1 + 3 * (1 - t) * t * t * y2 + t * t * t; };
    if (x <= 0) {
        // Outside [0, 1] the curve continues along its end tangents
        double slope = x1 > 0 ? y1 / x1 : (x2 > 0 ? y2 / x2 : 0);
        return slope * x;
    }
    if (x >= 1) {
        double slope = x2 < 1 ? (y2 - 1) / (x2 - 1) : (x1 < 1 ? (y1 - 1) / (x1 - 1) : 0);
        return 1 + slope * (x - 1);
    }
    double t = x;
    for (int i = 0; i < 8; i++) {
        double err = bx(t) - x;
        if (std::fabs(err) < 1e-7) return by(t);
        double d = 3 * (1 - t) * (1 - t) * x1 + 6 * (1 - t) * t * (x2 - x1) + 3 * t * t * (1 - x2);
        if (std::fabs(d) < 1e-6) break;
        t -= err / d;
    }
    double lo = 0, hi = 1;
    t = x;
    for (int i = 0; i < 40; i++) {
        double v = bx(t);
        if (std::fabs(v - x) < 1e-7) break;
        if (v < x) lo = t;
        else hi = t;
        t = (lo + hi) / 2;
    }
    return by(t);
}

// A value's text split into the numbers in it and the text around them
struct Shape {
    std::vector<std::string> text;  // text.size() == numbers.size() + 1
    std::vector<double> numbers;
};

Shape shapeOf(const std::string& v) {
    Shape s;
    std::string current;
    size_t i = 0;
    while (i < v.size()) {
        char c = v[i];
        bool starts = std::isdigit((unsigned char)c) || (c == '.' && i + 1 < v.size() && std::isdigit((unsigned char)v[i + 1])) ||
                      ((c == '-' || c == '+') && i + 1 < v.size() &&
                       (std::isdigit((unsigned char)v[i + 1]) || v[i + 1] == '.'));
        // Not inside a name (h1, translate3d, #fff stays text)
        bool inWord = i > 0 && (std::isalpha((unsigned char)v[i - 1]) || v[i - 1] == '#' || v[i - 1] == '-' ||
                                std::isdigit((unsigned char)v[i - 1]));
        if (starts && !inWord) {
            const char* begin = v.c_str() + i;
            char* end = nullptr;
            double n = std::strtod(begin, &end);
            if (end != begin) {
                s.text.push_back(current);
                current.clear();
                s.numbers.push_back(n);
                i += (size_t)(end - begin);
                continue;
            }
        }
        current += c;
        i++;
    }
    s.text.push_back(current);
    return s;
}

std::string number(double v) {
    if (std::fabs(v) < 1e-9) v = 0;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

bool isColorProperty(const std::string& p) {
    return p.find("color") != std::string::npos || p == "fill" || p == "stroke";
}

bool isTransformProperty(const std::string& p) { return p == "transform"; }

// The identity of each function of a transform list, for `none`
std::string identityOf(const std::string& list) {
    std::string out;
    size_t pos = 0;
    while (pos < list.size()) {
        size_t open = list.find('(', pos);
        if (open == std::string::npos) break;
        std::string name = lower(trim(list.substr(pos, open - pos)));
        size_t close = list.find(')', open);
        if (close == std::string::npos) break;
        std::vector<std::string> args = commaList(list.substr(open + 1, close - open - 1));
        std::string zero = name.rfind("scale", 0) == 0 ? "1" : "0";
        std::string identity;
        if (name == "matrix") identity = "1, 0, 0, 1, 0, 0";
        else if (name == "matrix3d") identity = "1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1";
        else if (name == "rotate3d" && args.size() == 4) identity = args[0] + ", " + args[1] + ", " + args[2] + ", 0deg";
        else {
            for (size_t i = 0; i < args.size(); i++) {
                // Keep each argument's unit: translate(0px, 0%)
                Shape s = shapeOf(args[i]);
                std::string unit = s.text.size() > 1 ? trim(s.text.back()) : "";
                identity += (i ? ", " : "") + zero + (zero == "0" ? unit : "");
            }
        }
        if (!out.empty()) out += ' ';
        out += name + "(" + identity + ")";
        pos = close + 1;
    }
    return out;
}

}  // namespace

double cssTimeMs(const std::string& token) {
    std::string t = lower(trim(token));
    char* end = nullptr;
    double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || !std::isfinite(v)) return 0;
    std::string unit = trim(end);
    if (unit == "ms") return v;
    if (unit == "s") return v * 1000.0;
    return 0;
}

double cssEasing(const std::string& function, double t, bool before) {
    std::string f = lower(trim(function));
    if (f.empty() || f == "linear") return t;
    if (f == "ease") return cubicBezier(0.25, 0.1, 0.25, 1, t);
    if (f == "ease-in") return cubicBezier(0.42, 0, 1, 1, t);
    if (f == "ease-out") return cubicBezier(0, 0, 0.58, 1, t);
    if (f == "ease-in-out") return cubicBezier(0.42, 0, 0.58, 1, t);
    size_t open = f.find('('), close = f.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) {
        if (f == "step-start") f = "steps(1, start)";
        else if (f == "step-end") f = "steps(1, end)";
        else return t;
        open = f.find('(');
        close = f.rfind(')');
    }
    std::string name = trim(f.substr(0, open));
    std::vector<std::string> args = commaList(f.substr(open + 1, close - open - 1));
    if (name == "cubic-bezier" && args.size() == 4) {
        double v[4];
        for (int i = 0; i < 4; i++) v[i] = std::strtod(args[i].c_str(), nullptr);
        if (v[0] < 0 || v[0] > 1 || v[2] < 0 || v[2] > 1) return t;
        return cubicBezier(v[0], v[1], v[2], v[3], t);
    }
    if (name == "steps" && !args.empty()) {
        long steps = std::strtol(args[0].c_str(), nullptr, 10);
        std::string position = args.size() > 1 ? args[1] : "end";
        if (steps < 1) return t;
        bool jumpStart = position == "start" || position == "jump-start" || position == "jump-both";
        bool jumpEnd = position == "end" || position == "jump-end" || position == "jump-both";
        long jumps = steps + (position == "jump-both" ? 1 : 0) - (position == "jump-none" ? 1 : 0);
        if (jumps < 1) jumps = 1;
        double current = std::floor(t * steps);
        if (jumpStart) current += 1;
        // At an exact step, the before flag holds the value of the step below
        if (before && std::fmod(t * steps, 1.0) == 0 && jumpStart) current -= 1;
        if (t >= 0 && current < 0) current = 0;
        if (t <= 1 && current > jumps) current = (double)jumps;
        (void)jumpEnd;
        return current / jumps;
    }
    if (name == "linear" && args.size() >= 2) {
        // linear(): straight segments between the stops (equal spacing)
        std::vector<double> ys;
        for (const std::string& a : args) ys.push_back(std::strtod(a.c_str(), nullptr));
        double x = std::clamp(t, 0.0, 1.0) * (ys.size() - 1);
        size_t i = std::min((size_t)x, ys.size() - 2);
        return ys[i] + (ys[i + 1] - ys[i]) * (x - i);
    }
    return t;
}

std::string interpolateCssValue(const std::string& property, const std::string& from, const std::string& to, double t) {
    std::string a = trim(from), b = trim(to);
    if (a == b) return a;
    if (isColorProperty(property)) {
        CSSColor ca = CSSColor::parse(a), cb = CSSColor::parse(b);
        // Premultiplied, so a fade from transparent keeps the colour (CSS Color 4 §12.3)
        double aa = ca.a / 255.0, ab = cb.a / 255.0;
        double alpha = std::clamp(aa + (ab - aa) * t, 0.0, 1.0);
        auto channel = [&](uint8_t x, uint8_t y) {
            if (alpha <= 0) return 0.0;
            double v = (x * aa + (y * ab - x * aa) * t) / alpha;
            return std::clamp(std::round(v), 0.0, 255.0);
        };
        char buf[64];
        std::snprintf(buf, sizeof buf, "rgba(%d, %d, %d, %s)", (int)channel(ca.r, cb.r), (int)channel(ca.g, cb.g),
                      (int)channel(ca.b, cb.b), number(alpha).c_str());
        return buf;
    }
    if (isTransformProperty(property)) {
        if (lower(a) == "none" && lower(b) != "none") a = identityOf(b);
        else if (lower(b) == "none" && lower(a) != "none") b = identityOf(a);
    }
    Shape sa = shapeOf(a), sb = shapeOf(b);
    // A bare 0 is a length of the other side's unit: translateX(0) and
    // translateX(200px) interpolate
    if (sa.numbers.size() == sb.numbers.size()) {
        auto unitOf = [](const std::string& text) {
            size_t n = 0;
            while (n < text.size() && (std::isalpha((unsigned char)text[n]) || text[n] == '%')) n++;
            return text.substr(0, n);
        };
        for (size_t i = 0; i < sa.numbers.size(); i++) {
            std::string ua = unitOf(sa.text[i + 1]), ub = unitOf(sb.text[i + 1]);
            if (ua.empty() && !ub.empty() && sa.numbers[i] == 0) sa.text[i + 1] = ub + sa.text[i + 1];
            else if (ub.empty() && !ua.empty() && sb.numbers[i] == 0) sb.text[i + 1] = ua + sb.text[i + 1];
        }
    }
    bool same = sa.numbers.size() == sb.numbers.size() && !sa.numbers.empty();
    for (size_t i = 0; same && i < sa.text.size(); i++) {
        if (lower(trim(sa.text[i])) != lower(trim(sb.text[i]))) same = false;
    }
    if (!same) {
        // A bare 0 against a length: 0 takes the other side's unit
        return t < 0.5 ? from : to;
    }
    std::string out;
    for (size_t i = 0; i < sa.numbers.size(); i++) {
        out += sa.text[i];
        out += number(sa.numbers[i] + (sb.numbers[i] - sa.numbers[i]) * t);
    }
    out += sa.text.back();
    return out;
}

// ---------------------------------------------------------------------------
// The controller
// ---------------------------------------------------------------------------

namespace {

bool connectedTo(const DOMNode* node, const DOMDocument* document) {
    for (const DOMNode* n = node; n; n = n->parentNode()) {
        if (n == document) return true;
        if (n->nodeType() == NodeType::DocumentFragment) {
            // A shadow root: on through its host
            if (auto* root = dynamic_cast<const DOMShadowRoot*>(n)) n = root->host();
            if (!n) return false;
            if (n == document) return true;
        }
    }
    return false;
}

const DOMElement* parentElementOf(const DOMElement* element) {
    DOMNode* p = element->parentNode();
    if (p && p->nodeType() == NodeType::DocumentFragment) {
        if (auto* root = dynamic_cast<DOMShadowRoot*>(p)) return root->host();
        return nullptr;
    }
    return p && p->nodeType() == NodeType::Element ? static_cast<const DOMElement*>(p) : nullptr;
}

std::map<std::string, std::string> valuesOf(const CSSComputedStyle& style) {
    std::map<std::string, std::string> out;
    for (auto& [name, value] : computedStyleProperties(style)) out.emplace(name, value);
    return out;
}

// Shorthands transition-property may name, by the longhands the computed
// style lists
const std::vector<std::string>& longhandsOf(const std::string& name) {
    static const std::unordered_map<std::string, std::vector<std::string>> map = {
        {"border-color", {"border-top-color"}},
        {"border", {"border-top-color", "border-top-width"}},
        {"background", {"background-color"}},
        {"margin", {"margin-top", "margin-right", "margin-bottom", "margin-left"}},
        {"padding", {"padding-top", "padding-right", "padding-bottom", "padding-left"}},
        {"inset", {"top", "right", "bottom", "left"}},
    };
    static const std::vector<std::string> none;
    auto it = map.find(name);
    return it == map.end() ? none : it->second;
}

// Properties CSS transitions animate (discrete ones such as display do not
// transition by default, CSS Transitions 2 §3)
bool transitionable(const std::string& name, const std::string& a, const std::string& b) {
    if (name == "display" || name == "position" || name == "visibility" || name == "font-family" ||
        name == "content" || name == "cursor" || name == "pointer-events" || name == "white-space" ||
        name == "float" || name == "overflow-x" || name == "overflow-y" || name == "box-sizing" ||
        name.rfind("--", 0) == 0 || name.find("style") != std::string::npos)
        return false;
    if (isColorProperty(name) || name == "transform") return true;
    return !shapeOf(a).numbers.empty() && !shapeOf(b).numbers.empty();
}

}  // namespace

void CSSAnimationController::clear() {
    states_.clear();
    events_.clear();
    document_ = nullptr;
}

bool CSSAnimationController::active() const {
    for (const auto& [element, state] : states_) {
        if (!state.transitions.empty()) return true;
        for (const Animation& a : state.animations) {
            if (!a.paused && a.phase != 2) return true;
        }
    }
    return false;
}

std::vector<CSSAnimationEvent> CSSAnimationController::takeEvents() {
    std::vector<CSSAnimationEvent> out;
    out.swap(events_);
    return out;
}

std::vector<CSSAnimationController::Keyframe> CSSAnimationController::keyframesFor(
    const std::string& name, const std::map<std::string, std::string>& base, StyleResolver& resolver) const {
    std::vector<Keyframe> frames;
    const CSSKeyframesRule* rule = resolver.keyframesRule(name);
    CSSRuleList* list = rule ? rule->cssRules() : nullptr;
    std::set<std::string> properties;
    for (size_t i = 0; list && i < list->length(); i++) {
        auto* frame = dynamic_cast<const CSSKeyframeRule*>(list->item(i));
        if (!frame || !frame->style()) continue;
        const CSSStyleDeclaration& decl = *frame->style();
        for (float offset : frame->offsets()) {
            // Keyframes at the same offset merge, later ones winning (§3)
            auto it = std::find_if(frames.begin(), frames.end(), [&](const Keyframe& k) { return k.offset == offset; });
            if (it == frames.end()) {
                frames.push_back({offset, {}, ""});
                it = frames.end() - 1;
            }
            for (size_t p = 0; p < decl.length(); p++) {
                std::string prop = decl.item(p);
                if (prop == "animation-timing-function") {
                    it->easing = decl.getPropertyValue(prop);
                    continue;
                }
                it->values[prop] = decl.getPropertyValue(prop);
                properties.insert(prop);
            }
        }
    }
    if (frames.empty()) return frames;
    std::sort(frames.begin(), frames.end(), [](const Keyframe& a, const Keyframe& b) { return a.offset < b.offset; });
    // Properties missing at 0% or 100% take the element's own value there (§3)
    for (double edge : {0.0, 1.0}) {
        auto it = std::find_if(frames.begin(), frames.end(), [&](const Keyframe& k) { return k.offset == edge; });
        if (it == frames.end()) it = frames.insert(edge == 0 ? frames.begin() : frames.end(), Keyframe{edge, {}, ""});
        for (const std::string& prop : properties) {
            if (it->values.count(prop)) continue;
            auto b = base.find(prop);
            it->values[prop] = b != base.end() ? b->second : "";
        }
    }
    return frames;
}

void CSSAnimationController::updateElement(DOMElement* element, const CSSComputedStyle& style,
                                           const CSSComputedStyle* parentStyle, StyleResolver& resolver,
                                           double nowMs) {
    bool animates = !style.animationName.empty() && lower(trim(style.animationName)) != "none";
    bool transitions = !style.transitionProperty.empty() && lower(trim(style.transitionProperty)) != "none";
    auto found = states_.find(element);
    if (!animates && !transitions && found == states_.end()) return;
    ElementState& state = states_[element];
    state.seen = true;

    // The values the element has without its animations and transitions
    CSSComputedStyle baseStyle = element->cssAnimationStyle().empty() ? style
                                                                     : resolver.computeBaseStyle(element, parentStyle);
    std::map<std::string, std::string> base = valuesOf(baseStyle);

    // --- Animations: one per animation-name entry; one with the same name
    // at the same place keeps running, with its new timing
    std::string key = style.animationName + "|" + style.animationDuration + "|" + style.animationDelay + "|" +
                      style.animationIterationCount + "|" + style.animationDirection + "|" + style.animationFillMode +
                      "|" + style.animationTimingFunction + "|" + style.animationPlayState;
    if (key != state.animationKey || animates) {
        auto names = animates ? commaList(style.animationName) : std::vector<std::string>{};
        auto durations = commaList(style.animationDuration), delays = commaList(style.animationDelay);
        auto counts = commaList(style.animationIterationCount), directions = commaList(style.animationDirection);
        auto fills = commaList(style.animationFillMode), easings = commaList(style.animationTimingFunction);
        auto states = commaList(style.animationPlayState);
        std::vector<Animation> next;
        for (size_t i = 0; i < names.size(); i++) {
            const std::string& name = names[i];
            if (name.empty() || lower(name) == "none") continue;
            Animation a;
            auto old = std::find_if(state.animations.begin(), state.animations.end(),
                                    [&](const Animation& x) { return x.name == name; });
            if (old != state.animations.end()) a = *old;
            else a.startMs = nowMs;
            a.name = name;
            a.durationMs = std::max(0.0, cssTimeMs(at(durations, i, "0s")));
            a.delayMs = cssTimeMs(at(delays, i, "0s"));
            std::string count = lower(at(counts, i, "1"));
            a.iterations = count == "infinite" ? INFINITY : std::max(0.0, std::strtod(count.c_str(), nullptr));
            a.direction = lower(at(directions, i, "normal"));
            a.fill = lower(at(fills, i, "none"));
            a.easing = at(easings, i, "ease");
            bool paused = lower(at(states, i, "running")) == "paused";
            if (paused && !a.paused) a.pausedAtMs = nowMs;
            if (!paused && a.paused) a.startMs += nowMs - a.pausedAtMs;  // resumes where it stopped
            a.paused = paused;
            a.keyframes = keyframesFor(name, base, resolver);
            next.push_back(std::move(a));
        }
        state.animations = std::move(next);
        state.animationKey = key;
    }

    // --- Transitions: a transitioned property whose value changed since
    // the last style change starts from where it is now (CSS Transitions 1 §3)
    std::set<std::string> watched;
    std::map<std::string, size_t> indexOf;
    auto properties = transitions ? commaList(style.transitionProperty) : std::vector<std::string>{};
    auto tDurations = commaList(style.transitionDuration), tDelays = commaList(style.transitionDelay);
    auto tEasings = commaList(style.transitionTimingFunction);
    for (size_t i = 0; i < properties.size(); i++) {
        std::string p = lower(properties[i]);
        if (p == "all") {
            for (const auto& [name, value] : base) {
                watched.insert(name);
                indexOf[name] = i;
            }
        } else if (!longhandsOf(p).empty()) {
            for (const std::string& l : longhandsOf(p)) {
                watched.insert(l);
                indexOf[l] = i;
            }
        } else {
            watched.insert(p);
            indexOf[p] = i;
        }
    }
    // Transitions of properties no longer transitioned are cancelled (§3)
    state.transitions.erase(std::remove_if(state.transitions.begin(), state.transitions.end(),
                                           [&](const Transition& t) { return !watched.count(t.property); }),
                            state.transitions.end());
    std::map<std::string, std::string> nextBase;
    for (const std::string& name : watched) {
        auto now = base.find(name);
        if (now == base.end()) continue;
        nextBase[name] = now->second;
        auto before = state.baseValues.find(name);
        if (before == state.baseValues.end() || before->second == now->second) continue;
        size_t i = indexOf[name];
        double duration = std::max(0.0, cssTimeMs(at(tDurations, i, "0s")));
        double delay = cssTimeMs(at(tDelays, i, "0s"));
        auto running = std::find_if(state.transitions.begin(), state.transitions.end(),
                                    [&](const Transition& t) { return t.property == name; });
        std::string from = before->second;
        if (running != state.transitions.end()) {
            if (running->to == now->second) continue;
            // From the running transition's current value
            auto applied = state.applied.find(name);
            if (applied != state.applied.end()) from = applied->second;
            state.transitions.erase(running);
        }
        if (duration + delay <= 0 || !transitionable(name, from, now->second)) continue;
        Transition t;
        t.property = name;
        t.from = from;
        t.to = now->second;
        t.easing = at(tEasings, i, "ease");
        t.startMs = nowMs;
        t.durationMs = duration;
        t.delayMs = delay;
        state.transitions.push_back(t);
        events_.push_back({element, "transitionrun", name, 0});
    }
    state.baseValues = std::move(nextBase);
}

bool CSSAnimationController::update(DOMDocument* document, StyleResolver& resolver, double nowMs) {
    if (document != document_) {
        clear();
        document_ = document;
    }
    if (!document) return false;
    for (auto& [element, state] : states_) state.seen = false;
    // Every element in tree order, through shadow trees; not under display: none
    struct Item {
        DOMNode* node;
        const CSSComputedStyle* parentStyle;
    };
    std::vector<Item> stack{{document, nullptr}};
    while (!stack.empty()) {
        Item item = stack.back();
        stack.pop_back();
        const CSSComputedStyle* style = item.parentStyle;
        if (item.node->nodeType() == NodeType::Element) {
            auto* element = static_cast<DOMElement*>(item.node);
            style = resolver.getComputedStyle(element);
            if (!style || style->display == DisplayValue::None) continue;
            updateElement(element, *style, item.parentStyle, resolver, nowMs);
            // (updateElement may compute other styles: the pointer stays
            // valid, cached styles are not dropped while computing)
            if (DOMShadowRoot* root = element->shadowRoot()) stack.push_back({root, style});
        }
        const auto& children = item.node->childNodes();
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            if ((*it)->nodeType() == NodeType::Element || (*it)->nodeType() == NodeType::DocumentFragment)
                stack.push_back({it->get(), style});
        }
    }
    // Elements gone (removed, display: none) lose their animations; their
    // animated values go with them
    for (auto it = states_.begin(); it != states_.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        // (still alive: removed nodes are freed after the restyle that
        // follows their removal, destroyed ones were forgotten)
        if (!it->first->cssAnimationStyle().empty()) it->first->setCssAnimationStyle("");
        it = states_.erase(it);
    }
    return active();
}

bool CSSAnimationController::updateElements(DOMDocument* document, const std::vector<DOMElement*>& elements,
                                            StyleResolver& resolver, double nowMs) {
    if (!document || document != document_) return active();
    for (DOMElement* element : elements) {
        if (!element || !connectedTo(element, document)) continue;
        const CSSComputedStyle* style = resolver.getComputedStyle(element);
        if (!style || style->display == DisplayValue::None) continue;
        const DOMElement* parent = parentElementOf(element);
        const CSSComputedStyle* parentStyle = parent ? resolver.getComputedStyle(const_cast<DOMElement*>(parent)) : nullptr;
        updateElement(element, *style, parentStyle, resolver, nowMs);
    }
    return active();
}

void CSSAnimationController::sample(DOMElement* element, ElementState& state, double nowMs) {
    std::map<std::string, std::string> values;
    // Animations, later names over earlier ones (§4.2)
    for (Animation& a : state.animations) {
        double now = a.paused ? a.pausedAtMs : nowMs;
        double local = now - a.startMs - a.delayMs;
        double active = a.durationMs * a.iterations;
        if (a.durationMs <= 0 || a.iterations == 0) active = 0;
        int phase = local < 0 ? 0 : (std::isfinite(active) && local >= active) ? 2 : 1;
        // Iteration progress (Web Animations 1 §4.8)
        double overall = 0;
        if (phase == 0) overall = 0;
        else if (phase == 2) overall = a.iterations;
        else overall = a.durationMs > 0 ? local / a.durationMs : 0;
        long iteration = (long)std::floor(overall);
        double progress = overall - iteration;
        if (phase == 2 && a.iterations > 0 && progress == 0 && std::isfinite(a.iterations)) {
            progress = 1;
            iteration -= 1;
        }
        if (iteration < 0) iteration = 0;
        bool forward = a.direction == "normal" || (a.direction == "alternate" && iteration % 2 == 0) ||
                       (a.direction == "alternate-reverse" && iteration % 2 == 1);
        double directed = forward ? progress : 1 - progress;

        // Events (§4.6): start on leaving the before phase, iteration on a
        // new iteration while active, end on reaching the after phase
        double elapsed = std::max(0.0, std::min(local, std::isfinite(active) ? active : local)) / 1000.0;
        if (phase != a.phase) {
            if ((a.phase <= 0) && phase >= 1) events_.push_back({element, "animationstart", a.name, 0});
            if (phase == 2) events_.push_back({element, "animationend", a.name, elapsed});
            a.phase = phase;
            a.iteration = iteration;
        } else if (phase == 1 && iteration != a.iteration) {
            events_.push_back({element, "animationiteration", a.name, iteration * a.durationMs / 1000.0});
            a.iteration = iteration;
        }

        bool applies = phase == 1 || (phase == 0 && (a.fill == "backwards" || a.fill == "both")) ||
                       (phase == 2 && (a.fill == "forwards" || a.fill == "both"));
        if (!applies || a.keyframes.empty()) continue;
        // The keyframes around the progress, eased between them by the
        // first one's timing function (§3.9.1)
        size_t k = 0;
        while (k + 2 < a.keyframes.size() && a.keyframes[k + 1].offset <= directed) k++;
        const Keyframe& from = a.keyframes[k];
        const Keyframe& to = a.keyframes[std::min(k + 1, a.keyframes.size() - 1)];
        double span = to.offset - from.offset;
        double local01 = span > 0 ? (directed - from.offset) / span : 1.0;
        double eased = cssEasing(from.easing.empty() ? a.easing : from.easing, local01, !forward);
        for (const auto& [prop, fromValue] : from.values) {
            auto toValue = to.values.find(prop);
            values[prop] = toValue == to.values.end() ? fromValue
                                                      : interpolateCssValue(prop, fromValue, toValue->second, eased);
        }
    }
    // Transitions over animations (CSS Cascade 5 §6.2)
    for (auto it = state.transitions.begin(); it != state.transitions.end();) {
        Transition& t = *it;
        double local = nowMs - t.startMs - t.delayMs;
        if (local >= 0 && !t.started) {
            t.started = true;
            events_.push_back({element, "transitionstart", t.property, 0});
        }
        if (local >= t.durationMs) {
            events_.push_back({element, "transitionend", t.property, t.durationMs / 1000.0});
            it = state.transitions.erase(it);
            continue;
        }
        double p = local <= 0 ? 0 : t.durationMs > 0 ? local / t.durationMs : 1;
        values[t.property] = interpolateCssValue(t.property, t.from, t.to, cssEasing(t.easing, p));
        ++it;
    }
    if (values == state.applied) return;
    // Only opacity and transforms changed: the element just repaints
    bool paintOnly = true;
    std::set<std::string> names;
    for (const auto& [k, v] : values) names.insert(k);
    for (const auto& [k, v] : state.applied) names.insert(k);
    for (const std::string& name : names) {
        auto a = values.find(name), b = state.applied.find(name);
        bool changed = a == values.end() || b == state.applied.end() || a->second != b->second;
        if (changed && !isPaintOnlyProperty(name)) paintOnly = false;
    }
    state.applied = values;
    std::string css;
    for (const auto& [prop, value] : values) {
        if (value.empty()) continue;
        css += prop + ": " + value + "; ";
    }
    if (!css.empty()) css.pop_back();
    element->setCssAnimationStyle(css, paintOnly);
}

void CSSAnimationController::tick(double nowMs) {
    for (auto& [element, state] : states_) sample(element, state, nowMs);
}

}  // namespace Zepra::WebCore
