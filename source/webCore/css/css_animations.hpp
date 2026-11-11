// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file css_animations.hpp
 * @brief CSS Animations 1 and CSS Transitions 1 over time.
 *
 * The controller follows the computed styles of a document: after a restyle
 * (update) it starts, keeps or cancels each element's CSS animations (by
 * animation-name) and starts transitions where a transitionable property's
 * value changed; on each frame (tick) it samples them and writes the values
 * into the element's cssAnimationStyle(), which the cascade applies in the
 * animations origin (setCssAnimationsDriven(true)). A change of opacity or
 * transform only is a paint-only change; anything else restyles the page.
 * The events to dispatch (animationstart, animationiteration, animationend,
 * transitionrun, transitionstart, transitionend) are collected for the
 * browser to fire.
 *
 * Interpolation is per value: colors channel by channel, values of the same
 * shape (the same text around the numbers: lengths, transform lists with
 * the same functions) number by number, `none` transforms as the identity
 * of the other side's functions; other values flip at the midpoint.
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace Zepra::WebCore {

class DOMDocument;
class DOMElement;
class StyleResolver;
struct CSSComputedStyle;

/// A CSS <easing-function>: the eased progress at `t` in [0, 1]. An unknown
/// function is linear. `before` is the before flag for steps() (§2.3).
double cssEasing(const std::string& function, double t, bool before = false);

/// The value between `from` and `to` at `t` (0: from, 1: to; t may lie
/// outside [0, 1] for overshooting easings), for property `property`
std::string interpolateCssValue(const std::string& property, const std::string& from, const std::string& to, double t);

/// A time ("0.3s", "200ms") in milliseconds; 0 for anything else
double cssTimeMs(const std::string& token);

struct CSSAnimationEvent {
    DOMElement* target = nullptr;
    std::string type;          // animationstart, ..., transitionend
    std::string name;          // animationName or propertyName
    double elapsedSeconds = 0;
};

class CSSAnimationController {
public:
    /// Follows the document's new styles at time `nowMs`: every element of
    /// `document` (light and shadow trees, not under display: none). Its
    /// cached computed styles are read from `resolver`. Records of elements
    /// no longer there are dropped. Returns whether anything runs.
    bool update(DOMDocument* document, StyleResolver& resolver, double nowMs);
    /// The same for some elements only (a paint-only style change); ones
    /// not connected to `document` are skipped
    bool updateElements(DOMDocument* document, const std::vector<DOMElement*>& elements, StyleResolver& resolver,
                        double nowMs);
    /// Samples every animation and transition at `nowMs` into the elements'
    /// cssAnimationStyle(), queueing their events
    void tick(double nowMs);
    /// The events queued since the last call, in order
    std::vector<CSSAnimationEvent> takeEvents();
    /// Anything to sample on the next frame
    bool active() const;
    /// Forgets every element (a new document); touches none of them
    void clear();
    /// Forgets one element (it is being destroyed)
    void forget(const DOMElement* element) { states_.erase(const_cast<DOMElement*>(element)); }

private:
    struct Keyframe {
        double offset;
        std::map<std::string, std::string> values;
        std::string easing;  // animation-timing-function inside the keyframe ("" = the animation's)
    };
    struct Animation {
        std::string name;
        double startMs = 0, durationMs = 0, delayMs = 0, iterations = 1;
        std::string direction = "normal", fill = "none", easing = "ease";
        bool paused = false;
        double pausedAtMs = 0;
        int phase = -1;          // last reported: -1 none, 0 before, 1 active, 2 after
        long iteration = 0;      // last reported current iteration
        std::vector<Keyframe> keyframes;  // with the implicit 0% and 100% filled in
    };
    struct Transition {
        std::string property, from, to, easing;
        double startMs = 0, durationMs = 0, delayMs = 0;
        bool started = false;    // transitionstart sent
    };
    struct ElementState {
        std::string animationKey;              // the animation-* values last seen
        std::vector<Animation> animations;
        std::map<std::string, std::string> baseValues;  // transitioned properties' last values
        std::vector<Transition> transitions;
        std::map<std::string, std::string> applied;     // what cssAnimationStyle() holds
        bool seen = false;
    };

    void updateElement(DOMElement* element, const CSSComputedStyle& style, const CSSComputedStyle* parentStyle,
                       StyleResolver& resolver, double nowMs);
    std::vector<Keyframe> keyframesFor(const std::string& name, const std::map<std::string, std::string>& base,
                                       StyleResolver& resolver) const;
    void sample(DOMElement* element, ElementState& state, double nowMs);

    std::unordered_map<DOMElement*, ElementState> states_;
    std::vector<CSSAnimationEvent> events_;
    const DOMDocument* document_ = nullptr;
};

}  // namespace Zepra::WebCore
