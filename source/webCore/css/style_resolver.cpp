// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * style_resolver.cpp - CSS Style Resolution
 * 
 * Implements style resolution per W3C CSS Cascade spec:
 * 1. Collect matching rules from all stylesheets
 * 2. Apply cascade to determine winning values
 * 3. Handle inheritance for inheritable properties
 * 4. Compute final values
 * 
 */

#include "css/custom_properties.hpp"
#include "css/css_engine.hpp"
#include "browser/dom.hpp"
#include "css/presentational_hints.hpp"
#include "css/media_query.hpp"
#include <sstream>
#include <algorithm>
#include <iostream>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <unordered_set>

namespace Zepra::WebCore {

// ============================================================================
// Helpers
// ============================================================================

// A border whose style is none or hidden computes to a width of 0
// (CSS Backgrounds 3 §3.3)
static void zeroUnstyledBorders(CSSComputedStyle& style) {
    auto none = [](BorderStyle b) { return b == BorderStyle::None || b == BorderStyle::Hidden; };
    if (none(style.borderTopStyle)) style.borderTopWidth = 0;
    if (none(style.borderRightStyle)) style.borderRightWidth = 0;
    if (none(style.borderBottomStyle)) style.borderBottomWidth = 0;
    if (none(style.borderLeftStyle)) style.borderLeftWidth = 0;
}

// Space-separated values; a function's arguments ("calc(1px + 2px) 0") stay
// together
static std::vector<std::string> splitValues(const std::string& value) {
    std::vector<std::string> parts;
    std::string token;
    int depth = 0;
    for (char c : value) {
        if (c == '(') depth++;
        else if (c == ')' && depth > 0) depth--;
        if (depth == 0 && std::isspace(static_cast<unsigned char>(c))) {
            if (!token.empty()) parts.push_back(token);
            token.clear();
            continue;
        }
        token += c;
    }
    if (!token.empty()) parts.push_back(token);
    return parts;
}

// A comma-separated list's items, trimmed; commas inside functions stay
static std::vector<std::string> splitCommaList(const std::string& value) {
    std::vector<std::string> items;
    std::string item;
    int depth = 0;
    auto flush = [&]() {
        size_t a = item.find_first_not_of(" \t\r\n\f"), b = item.find_last_not_of(" \t\r\n\f");
        items.push_back(a == std::string::npos ? "" : item.substr(a, b - a + 1));
        item.clear();
    };
    for (char c : value) {
        if (c == '(') depth++;
        else if (c == ')' && depth > 0) depth--;
        if (c == ',' && depth == 0) {
            flush();
            continue;
        }
        item += c;
    }
    flush();
    return items;
}

static std::string lowerAscii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// "1.5s", "200ms": a <time>
static bool isTimeToken(const std::string& token) {
    std::string t = lowerAscii(token);
    size_t unit = t.size() >= 2 && t.compare(t.size() - 2, 2, "ms") == 0 ? 2 : (!t.empty() && t.back() == 's' ? 1 : 0);
    if (!unit || t.size() == unit) return false;
    char* end = nullptr;
    std::string number = t.substr(0, t.size() - unit);
    std::strtod(number.c_str(), &end);
    return end && *end == '\0';
}

// The animation shorthand (CSS Animations 1 §3.11) split into the
// longhands this engine uses: per animation a duration, an easing (ignored),
// a delay, an iteration count, a direction, a fill mode, a play state
// (ignored) and a name, in any order
// A <easing-function> token (CSS Easing 1)
static bool isEasingToken(const std::string& t) {
    return t == "linear" || t == "ease" || t == "ease-in" || t == "ease-out" || t == "ease-in-out" ||
           t == "step-start" || t == "step-end" || t.rfind("cubic-bezier(", 0) == 0 || t.rfind("steps(", 0) == 0 ||
           t.rfind("linear(", 0) == 0;
}

// transition: one list entry per transition, each "property duration
// easing delay" in any order (CSS Transitions 1 §2.5)
static void setTransitionShorthand(CSSComputedStyle& style, const std::string& value) {
    std::string properties, durations, delays, easings;
    auto append = [](std::string& list, const std::string& item) {
        if (!list.empty()) list += ", ";
        list += item;
    };
    for (const std::string& single : splitCommaList(value)) {
        std::string property, duration, delay, easing;
        for (const std::string& token : splitValues(single)) {
            std::string t = lowerAscii(token);
            if (isTimeToken(t)) (duration.empty() ? duration : delay) = t;
            else if (isEasingToken(t)) easing = t;
            else if (property.empty()) property = t;
        }
        append(properties, property.empty() ? "all" : property);
        append(durations, duration.empty() ? "0s" : duration);
        append(delays, delay.empty() ? "0s" : delay);
        append(easings, easing.empty() ? "ease" : easing);
    }
    style.transitionProperty = properties;
    style.transitionDuration = durations;
    style.transitionDelay = delays;
    style.transitionTimingFunction = easings;
}

static void setAnimationShorthand(CSSComputedStyle& style, const std::string& value) {
    std::string names, durations, delays, counts, directions, fills, easings, states;
    auto append = [](std::string& list, const std::string& item) {
        if (!list.empty()) list += ", ";
        list += item;
    };
    for (const std::string& single : splitCommaList(value)) {
        std::string name, duration, delay, count, direction, fill, easing, state;
        for (const std::string& token : splitValues(single)) {
            std::string t = lowerAscii(token);
            if (isTimeToken(t)) {
                (duration.empty() ? duration : delay) = t;
            } else if (t == "infinite" || (!t.empty() && (std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '.'))) {
                count = t;
            } else if (t == "normal" || t == "reverse" || t == "alternate" || t == "alternate-reverse") {
                direction = t;
            } else if (t == "forwards" || t == "backwards" || t == "both") {
                fill = t;
            } else if (t == "none") {
                if (name.empty()) name = t;
                else fill = t;
            } else if (t == "running" || t == "paused") {
                state = t;
            } else if (isEasingToken(t)) {
                easing = t;
            } else if (t.find('(') != std::string::npos) {
                // not a keyframes name
            } else {
                name = token;   // a <keyframes-name> keeps its case
            }
        }
        append(names, name.empty() ? "none" : name);
        append(durations, duration.empty() ? "0s" : duration);
        append(delays, delay.empty() ? "0s" : delay);
        append(counts, count.empty() ? "1" : count);
        append(directions, direction.empty() ? "normal" : direction);
        append(fills, fill.empty() ? "none" : fill);
        append(easings, easing.empty() ? "ease" : easing);
        append(states, state.empty() ? "running" : state);
    }
    style.animationTimingFunction = easings;
    style.animationPlayState = states;
    style.animationName = names;
    style.animationDuration = durations;
    style.animationDelay = delays;
    style.animationIterationCount = counts;
    style.animationDirection = directions;
    style.animationFillMode = fills;
}

// Logical properties mapped to the physical ones for horizontal-tb, ltr
// (CSS Logical 1): block is top/bottom, inline is left/right. Two-value
// shorthands give start then end.
static const std::unordered_map<std::string, std::pair<const char*, const char*>> kLogicalShorthands = {
    {"margin-inline", {"margin-left", "margin-right"}},
    {"margin-block", {"margin-top", "margin-bottom"}},
    {"padding-inline", {"padding-left", "padding-right"}},
    {"padding-block", {"padding-top", "padding-bottom"}},
    {"border-inline", {"border-left", "border-right"}},
    {"border-block", {"border-top", "border-bottom"}},
    {"border-inline-width", {"border-left-width", "border-right-width"}},
    {"border-block-width", {"border-top-width", "border-bottom-width"}},
    {"border-inline-style", {"border-left-style", "border-right-style"}},
    {"border-block-style", {"border-top-style", "border-bottom-style"}},
    {"border-inline-color", {"border-left-color", "border-right-color"}},
    {"border-block-color", {"border-top-color", "border-bottom-color"}},
};
static const std::unordered_map<std::string, const char*> kLogicalLonghands = {
    {"margin-inline-start", "margin-left"},   {"margin-inline-end", "margin-right"},
    {"margin-block-start", "margin-top"},     {"margin-block-end", "margin-bottom"},
    {"padding-inline-start", "padding-left"}, {"padding-inline-end", "padding-right"},
    {"padding-block-start", "padding-top"},   {"padding-block-end", "padding-bottom"},
    {"border-inline-start", "border-left"},   {"border-inline-end", "border-right"},
    {"border-block-start", "border-top"},     {"border-block-end", "border-bottom"},
    {"border-inline-start-width", "border-left-width"}, {"border-inline-end-width", "border-right-width"},
    {"border-block-start-width", "border-top-width"},   {"border-block-end-width", "border-bottom-width"},
    {"border-inline-start-style", "border-left-style"}, {"border-inline-end-style", "border-right-style"},
    {"border-block-start-style", "border-top-style"},   {"border-block-end-style", "border-bottom-style"},
    {"border-inline-start-color", "border-left-color"}, {"border-inline-end-color", "border-right-color"},
    {"border-block-start-color", "border-top-color"},   {"border-block-end-color", "border-bottom-color"},
    {"inline-size", "width"},         {"block-size", "height"},
    {"min-inline-size", "min-width"}, {"min-block-size", "min-height"},
    {"max-inline-size", "max-width"}, {"max-block-size", "max-height"},
};

static std::string trimGridText(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// A custom ident in grid placement (a line or area name), not a number,
// span or auto
static bool isGridIdent(const std::string& s) {
    if (s.empty() || s == "auto" || s.rfind("span", 0) == 0) return false;
    return !(std::isdigit(static_cast<unsigned char>(s[0])) || s[0] == '-' || s[0] == '+');
}

// grid-row / grid-column: <start> [/ <end>]; a lone name is also the end
static void splitGridLine(const std::string& value, std::string& start, std::string& end) {
    auto slash = value.find('/');
    start = trimGridText(value.substr(0, slash));
    if (slash != std::string::npos) end = trimGridText(value.substr(slash + 1));
    else end = isGridIdent(start) ? start : "auto";
}

// justify-items/-self, align-items/-self values (normal and legacy are stretch)
static JustifyAlign parseSelfAlign(const std::string& raw, JustifyAlign normal) {
    std::string v = raw;
    if (v.rfind("safe ", 0) == 0) v = v.substr(5);
    if (v.rfind("unsafe ", 0) == 0) v = v.substr(7);
    if (v == "start" || v == "flex-start" || v == "self-start" || v == "left") return JustifyAlign::Start;
    if (v == "end" || v == "flex-end" || v == "self-end" || v == "right") return JustifyAlign::End;
    if (v == "center") return JustifyAlign::Center;
    if (v == "stretch") return JustifyAlign::Stretch;
    if (v == "baseline" || v == "first baseline" || v == "last baseline") return JustifyAlign::Baseline;
    if (v == "auto") return JustifyAlign::Auto;
    return normal;
}

// ============================================================================
// Properties that inherit (per CSS spec)
// ============================================================================

// Static list of inheritable properties (from MDN)
static const std::unordered_set<std::string> INHERITED_PROPERTIES = {
    // Typography
    "color", "font", "font-family", "font-size", "font-style", 
    "font-weight", "font-variant", "line-height", "letter-spacing",
    "word-spacing", "text-align", "text-indent", "text-transform",
    "white-space", "direction", "-webkit-text-fill-color", "-webkit-text-stroke",
    "-webkit-text-stroke-width", "-webkit-text-stroke-color",
    // Lists
    "list-style", "list-style-image", "list-style-position", "list-style-type",
    // Other
    "visibility", "cursor", "quotes", "orphans", "widows"
};

// ============================================================================
// StyleResolver Implementation
// ============================================================================

StyleResolver::StyleResolver() {
    // UA stylesheet now loaded externally via CSSEngine::addStyleSheet()
    // with StyleOrigin::UserAgent — no duplicate needed here
}

StyleResolver::~StyleResolver() = default;

void StyleResolver::addStyleSheet(std::shared_ptr<CSSStyleSheet> sheet, StyleOrigin origin) {
    if (sheet) {
        // A later @property for the same name replaces an earlier one
        for (const auto& [name, reg] : sheet->propertyRegistrations()) registered_[name] = reg;
    }
    stylesheets_.push_back({std::move(sheet), origin});
    cascade_.invalidateIndex();
}

void StyleResolver::setStyleSheets(std::vector<StyleSheetEntry> sheets) {
    registered_.clear();
    for (const auto& entry : sheets) {
        if (!entry.sheet) continue;
        for (const auto& [name, reg] : entry.sheet->propertyRegistrations()) registered_[name] = reg;
    }
    stylesheets_ = std::move(sheets);
    cascade_.invalidateIndex();
}

namespace {
// Set by the browser when CSSAnimationController runs CSS animations and
// transitions over time; without it they jump to their end state
bool s_cssAnimationsDriven = false;
}  // namespace

void setCssAnimationsDriven(bool driven) { s_cssAnimationsDriven = driven; }
bool cssAnimationsDriven() { return s_cssAnimationsDriven; }

CSSComputedStyle StyleResolver::computeBaseStyle(DOMElement* element, const CSSComputedStyle* parentStyle) {
    skipCssAnimations_ = true;
    CSSComputedStyle style = computeStyle(element, parentStyle);
    skipCssAnimations_ = false;
    return style;
}

CSSComputedStyle StyleResolver::computeStyle(DOMElement* element, const CSSComputedStyle* parentStyle) {
    CSSComputedStyle computed;
    
    if (!element) return computed;
    
    // Step 1: Start with inherited values from parent
    if (parentStyle) {
        computed = CSSComputedStyle::inherit(*parentStyle);
    }
    
    // Step 2: Collect all matching rules WITH correct per-sheet origins
    auto matchedRules = collectRules(element, "");
    
    // Step 3: Sort by cascade order
    cascade_.sortByCascade(matchedRules);
    
    // The element's presentational hints (bgcolor, width, align...) and its
    // inline style, parsed once for both cascade passes
    std::unique_ptr<CSSStyleDeclaration> hintDecl;
    std::string hints = presentationalHintStyle(element);
    if (!hints.empty()) {
        CSSParser hintParser;
        hintDecl = hintParser.parseInlineStyle(hints);
    }
    std::unique_ptr<CSSStyleDeclaration> inlineDecl;
    std::string inlineStyle = element->getAttribute("style");
    if (!inlineStyle.empty()) {
        CSSParser parser;
        inlineDecl = parser.parseInlineStyle(inlineStyle);
    }
    // The animations origin (CSS Cascade 5 §6.2): above every normal
    // declaration, below every !important one. CSS animations' keyframes
    // first, then script animations (Web Animations' effects, written by
    // element.animate()), which come later in the composite order.
    std::vector<const CSSStyleDeclaration*> cssAnimationDecls;
    std::unique_ptr<CSSStyleDeclaration> cssAnimationDecl;
    if (s_cssAnimationsDriven && !skipCssAnimations_ && !element->cssAnimationStyle().empty()) {
        // What the running CSS animations and transitions apply now
        CSSParser parser;
        cssAnimationDecl = parser.parseInlineStyle(element->cssAnimationStyle());
        if (cssAnimationDecl) cssAnimationDecls.push_back(cssAnimationDecl.get());
    }
    std::unique_ptr<CSSStyleDeclaration> scriptAnimationDecl;
    if (!element->animationStyle().empty()) {
        CSSParser parser;
        scriptAnimationDecl = parser.parseInlineStyle(element->animationStyle());
    }

    auto runCascade = [&]() {
        // Step 4: Apply the normal declarations of the matched rules in order
        // (lowest priority first). The presentational hints go after the
        // user-agent and user rules and before the first author rule.
        bool hintsApplied = false;
        auto applyHints = [&]() {
            hintsApplied = true;
            if (hintDecl) applyDeclarations(computed, hintDecl.get(), parentStyle);
        };
        for (const auto& match : matchedRules) {
            if (!hintsApplied && match.origin != StyleOrigin::UserAgent && match.origin != StyleOrigin::User) {
                applyHints();
            }
            if (match.rule && match.rule->style()) {
                applyDeclarations(computed, match.rule->style(), parentStyle, Importance::Normal);
            }
        }
        if (!hintsApplied) applyHints();

        // Step 5: Apply inline styles (above every normal rule)
        if (inlineDecl) applyDeclarations(computed, inlineDecl.get(), parentStyle, Importance::Normal);
        for (const CSSStyleDeclaration* frame : cssAnimationDecls)
            applyDeclarations(computed, frame, parentStyle, Importance::Normal);
        if (scriptAnimationDecl) applyDeclarations(computed, scriptAnimationDecl.get(), parentStyle, Importance::Normal);

        // Step 6: !important declarations, in reverse origin order: author rules,
        // the inline style, user rules, user-agent rules (CSS Cascade 4, 6.4)
        auto applyImportant = [&](StyleOrigin origin) {
            for (const MatchedRule* match : CSSCascade::importantOrder(matchedRules, origin))
                applyDeclarations(computed, match->rule->style(), parentStyle, Importance::Important);
        };
        applyImportant(StyleOrigin::Author);
        if (inlineDecl && inlineDecl->hasImportant()) {
            applyDeclarations(computed, inlineDecl.get(), parentStyle, Importance::Important);
        }
        applyImportant(StyleOrigin::User);
        applyImportant(StyleOrigin::UserAgent);
    };

    // Custom properties first: the cascade picks each one's winning value,
    // then they are resolved against each other and the inherited ones
    auto runPasses = [&]() {
        CustomPropertyMap declared;
        pass_ = Pass::CustomOnly;
        declaredCustom_ = &declared;
        runCascade();
        declaredCustom_ = nullptr;
        computed.customProperties = resolveCustomProperties(computed.customProperties, declared, &registered_);
        // Then every other property, with var() substituted from them
        pass_ = Pass::SkipCustom;
        runCascade();
        pass_ = Pass::All;
    };
    runPasses();
    // The animation-* properties are known only now: an element with
    // finished, filling CSS animations is cascaded again with their end
    // keyframes in the animations origin
    if (!s_cssAnimationsDriven) cssAnimationDecls = animationEndStates(computed);
    if (!s_cssAnimationsDriven && !cssAnimationDecls.empty()) {
        computed = parentStyle ? CSSComputedStyle::inherit(*parentStyle) : CSSComputedStyle();
        runPasses();
    }

    // <center> and align="center" stop at tables: a table doesn't pass the
    // legacy -webkit-* alignment on to its cells (cells use their own align)
    if ((computed.display == DisplayValue::Table || computed.display == DisplayValue::InlineTable) &&
        computed.legacyAlign != 0) {
        computed.textAlign = TextAlign::Start;
        computed.legacyAlign = 0;
    }

    // Font-relative margins and paddings resolve against the element's own
    // font size; layout only resolves percentages and viewport units
    auto resolveFontRelative = [&](CSSLength& len) {
        if (len.unit == CSSLength::Unit::Em || len.unit == CSSLength::Unit::Rem ||
            len.unit == CSSLength::Unit::Pt || len.unit == CSSLength::Unit::Ex ||
            len.unit == CSSLength::Unit::Ch) {
            len = CSSLength::px(len.toPx(computed.fontSize, 16.0f, 0, 0, 0));
        }
    };
    for (CSSLength* len : {&computed.marginTop, &computed.marginRight, &computed.marginBottom,
                           &computed.marginLeft, &computed.paddingTop, &computed.paddingRight,
                           &computed.paddingBottom, &computed.paddingLeft}) {
        resolveFontRelative(*len);
    }
    zeroUnstyledBorders(computed);
    
    return computed;
}

const CSSComputedStyle* StyleResolver::getComputedStyle(DOMElement* element) {
    if (!element) return nullptr;
    
    // Check cache
    auto it = styleCache_.find(element);
    if (it != styleCache_.end()) {
        return &it->second;
    }
    
    // Compute parent style first (for inheritance)
    const CSSComputedStyle* parentStyle = nullptr;
    // The flat tree parent: a slotted child inherits from its slot, a
    // shadow root's children from the host
    if (DOMElement* parent = flatTreeParentElement(element)) {
        parentStyle = getComputedStyle(parent);
    }
    
    // Compute and cache
    CSSComputedStyle style = computeStyle(element, parentStyle);
    styleCache_[element] = std::move(style);
    
    return &styleCache_[element];
}

const CSSComputedStyle* StyleResolver::getPseudoElementStyle(DOMElement* element, const std::string& pseudo) {
    static const std::unordered_set<std::string> kPseudoElements = {
        "before", "after", "marker", "first-line", "first-letter", "placeholder", "selection",
    };
    if (!element || !kPseudoElements.count(pseudo)) return nullptr;

    auto& perElement = pseudoStyleCache_[element];
    auto cached = perElement.find(pseudo);
    if (cached != perElement.end()) return &cached->second;

    // A pseudo-element inherits from its element (CSS Pseudo 4, 4.1); its
    // own initial display is inline
    const CSSComputedStyle* elementStyle = getComputedStyle(element);
    CSSComputedStyle computed = elementStyle ? CSSComputedStyle::inherit(*elementStyle) : CSSComputedStyle();
    computed.display = DisplayValue::Inline;

    auto matched = collectRules(element, pseudo);
    cascade_.sortByCascade(matched);

    // Normal declarations, then !important ones in reverse origin order (no
    // style attribute or presentational hints reach a pseudo-element)
    for (const auto& match : matched) {
        if (match.rule && match.rule->style()) {
            applyDeclarations(computed, match.rule->style(), elementStyle, Importance::Normal);
        }
    }
    for (StyleOrigin origin : {StyleOrigin::Author, StyleOrigin::User, StyleOrigin::UserAgent}) {
        for (const MatchedRule* match : CSSCascade::importantOrder(matched, origin))
            applyDeclarations(computed, match->rule->style(), elementStyle, Importance::Important);
    }

    for (CSSLength* len : {&computed.marginTop, &computed.marginRight, &computed.marginBottom,
                           &computed.marginLeft, &computed.paddingTop, &computed.paddingRight,
                           &computed.paddingBottom, &computed.paddingLeft}) {
        if (len->unit == CSSLength::Unit::Em || len->unit == CSSLength::Unit::Rem ||
            len->unit == CSSLength::Unit::Pt || len->unit == CSSLength::Unit::Ex ||
            len->unit == CSSLength::Unit::Ch) {
            *len = CSSLength::px(len->toPx(computed.fontSize, 16.0f, 0, 0, 0));
        }
    }

    zeroUnstyledBorders(computed);

    // `content: normal` computes to none on ::before/::after (CSS Content 3, 2)
    if ((pseudo == "before" || pseudo == "after") && (computed.content.empty() || computed.content == "normal")) {
        computed.content = "none";
    }

    auto [it, inserted] = perElement.emplace(pseudo, std::move(computed));
    return &it->second;
}

std::vector<MatchedRule> StyleResolver::matchedRules(DOMElement* element) {
    auto rules = collectRules(element, "");
    cascade_.sortByCascade(rules);
    return rules;
}

std::vector<MatchedRule> StyleResolver::collectRules(DOMElement* element, const std::string& pseudo) {
    std::vector<std::pair<CSSStyleSheet*, StyleOrigin>> sheetPairs;
    for (const auto& entry : stylesheets_) {
        sheetPairs.push_back({entry.sheet.get(), entry.origin});
    }
    cascade_.setMatchPseudoElement(pseudo);
    auto matched = cascade_.collectMatchingRulesWithOrigins(element, sheetPairs);
    cascade_.setMatchPseudoElement("");

    // Shadow trees (CSS Scoping §3): the document's author rules stop at a
    // shadow tree, whose own sheets apply inside it and to its host (:host)
    DOMShadowRoot* tree = asShadowRoot(treeRoot(element));
    if (tree) {
        matched.erase(std::remove_if(matched.begin(), matched.end(),
                                     [](const MatchedRule& m) { return m.origin == StyleOrigin::Author; }),
                      matched.end());
    }
    if (shadowScopes_.empty()) return matched;
    auto collectScoped = [&](const DOMShadowRoot* scope, bool forHost) {
        auto found = shadowScopes_.find(scope);
        if (found == shadowScopes_.end()) return;
        ScopedCascade& scoped = *found->second;
        scoped.cascade.setShadowScope(scope);
        scoped.cascade.setMatchPseudoElement(pseudo);
        auto rules = scoped.cascade.collectMatchingRulesWithOrigins(element, scoped.pairs);
        scoped.cascade.setMatchPseudoElement("");
        scoped.cascade.setShadowScope(nullptr);
        for (auto& rule : rules) {
            // :host rules lose to the document's rules for the host
            // (outer tree context first), whatever their specificity
            // (the lowest layer rank too: layers are compared first)
            if (forHost) {
                rule.specificity.a -= 1000;
                rule.layer = 0;
            }
            matched.push_back(rule);
        }
    };
    if (tree) collectScoped(tree, false);
    if (DOMShadowRoot* hosted = element->shadowRoot()) collectScoped(hosted, true);
    return matched;
}

void StyleResolver::setShadowStyleSheets(
        std::vector<std::pair<const DOMShadowRoot*, std::vector<std::shared_ptr<CSSStyleSheet>>>> scopes) {
    // Trees with the same sheets share one cascade (and its selector index)
    std::unordered_map<const DOMShadowRoot*, std::shared_ptr<ScopedCascade>> next;
    std::vector<std::shared_ptr<ScopedCascade>> pool;
    for (const auto& [root, existing] : shadowScopes_) pool.push_back(existing);
    for (auto& [root, sheets] : scopes) {
        if (sheets.empty()) continue;
        std::shared_ptr<ScopedCascade> match;
        auto same = [&](const std::shared_ptr<ScopedCascade>& c) { return c && c->sheets == sheets; };
        for (const auto& [r, c] : next) if (same(c)) { match = c; break; }
        if (!match) for (const auto& c : pool) if (same(c)) { match = c; break; }
        if (!match) {
            match = std::make_shared<ScopedCascade>();
            match->sheets = sheets;
            for (const auto& sheet : sheets) match->pairs.push_back({sheet.get(), StyleOrigin::Author});
        }
        next[root] = match;
    }
    shadowScopes_ = std::move(next);
    invalidateAll();
}

bool isInheritedProperty(const std::string& property) {
    return INHERITED_PROPERTIES.count(property) > 0;
}

void StyleResolver::invalidateElement(DOMElement* element) {
    styleCache_.erase(element);
    pseudoStyleCache_.erase(element);
}

void StyleResolver::invalidateAll() {
    styleCache_.clear();
    pseudoStyleCache_.clear();
}

// ============================================================================
// Property Application
// ============================================================================

void StyleResolver::applyDeclarations(CSSComputedStyle& style, 
                                       const CSSStyleDeclaration* decl,
                                       const CSSComputedStyle* parentStyle,
                                       Importance importance) {
    if (!decl) return;
    
    // Apply each property in the declaration
    for (size_t i = 0; i < decl->length(); i++) {
        std::string property = decl->item(i);
        if (importance != Importance::Any) {
            bool important = !decl->getPropertyPriority(property).empty();
            if (important != (importance == Importance::Important)) continue;
        }
        std::string value = decl->getPropertyValue(property);
        if (value.empty()) continue;

        if (isCustomPropertyName(property)) {
            if (pass_ == Pass::SkipCustom) continue;
            if (declaredCustom_) {
                (*declaredCustom_)[property] = value;  // later declarations win
            } else {
                // A single pass (pseudo-elements): resolve as it comes
                style.customProperties = resolveCustomProperties(style.customProperties, {{property, value}}, &registered_);
            }
            continue;
        }
        if (pass_ == Pass::CustomOnly) continue;
        applyProperty(style, property, value, parentStyle);
    }
}

void StyleResolver::applyProperty(CSSComputedStyle& style,
                                   const std::string& property,
                                   const std::string& rawValue,
                                   const CSSComputedStyle* parentStyle) {
    // What a math function's em/vw terms resolve against: the font size so
    // far (the parent's until font-size itself is applied) and the viewport
    lengthContext_.fontSize = style.fontSize;
    lengthContext_.viewportWidth = static_cast<float>(mediaEnvironment().width);
    lengthContext_.viewportHeight = static_cast<float>(mediaEnvironment().height);

    // Logical properties: applied as their physical counterparts
    if (auto longhand = kLogicalLonghands.find(property); longhand != kLogicalLonghands.end()) {
        applyProperty(style, longhand->second, rawValue, parentStyle);
        return;
    }
    if (auto shorthand = kLogicalShorthands.find(property); shorthand != kLogicalShorthands.end()) {
        // One value for both sides, or start then end; keywords and var()
        // apply to both as given
        auto parts = splitValues(rawValue);
        bool keyword = rawValue == "inherit" || rawValue == "initial" || rawValue == "unset";
        if (parts.empty() || parts.size() > 2 || keyword || containsVarFunction(rawValue) ||
            property.rfind("border-", 0) == 0) {
            parts = {rawValue};
        }
        applyProperty(style, shorthand->second.first, parts[0], parentStyle);
        applyProperty(style, shorthand->second.second, parts.size() > 1 ? parts[1] : parts[0], parentStyle);
        return;
    }

    // Handle 'inherit' keyword
    if (rawValue == "inherit" && parentStyle) {
        if (property == "color") style.color = parentStyle->color;
        else if (property == "font-family") style.fontFamily = parentStyle->fontFamily;
        else if (property == "font-size") style.fontSize = parentStyle->fontSize;
        else if (property == "font-weight") style.fontWeight = parentStyle->fontWeight;
        else if (property == "font-style") style.fontStyle = parentStyle->fontStyle;
        else if (property == "line-height") {
            style.lineHeight = parentStyle->lineHeight;
            style.lineHeightNormal = parentStyle->lineHeightNormal;
            style.lineHeightPx = parentStyle->lineHeightPx;
        }
        else if (property == "text-align") {
            style.textAlign = parentStyle->textAlign;
            style.legacyAlign = parentStyle->legacyAlign;
        }
        else if (property == "letter-spacing") style.letterSpacing = parentStyle->letterSpacing;
        else if (property == "word-spacing") style.wordSpacing = parentStyle->wordSpacing;
        else if (property == "white-space") style.whiteSpace = parentStyle->whiteSpace;
        else if (property == "vertical-align") style.verticalAlign = parentStyle->verticalAlign;
        else if (property == "border-spacing") {
            style.borderSpacingH = parentStyle->borderSpacingH;
            style.borderSpacingV = parentStyle->borderSpacingV;
        }
        else if (property == "border-collapse") style.borderCollapse = parentStyle->borderCollapse;
        else if (property == "visibility") style.visibility = parentStyle->visibility;
        else if (property == "cursor") style.cursor = parentStyle->cursor;
        // Not inherited by default, but 'inherit' still takes the parent's
        // value: preflight's a{text-decoration:inherit} removes the UA underline
        else if (property == "text-decoration" || property == "text-decoration-line")
            style.textDecoration = parentStyle->textDecoration.empty() ? "none" : parentStyle->textDecoration;
        else if (property == "text-transform") style.textTransform = parentStyle->textTransform;
        else if (property == "background-color") style.backgroundColor = parentStyle->backgroundColor;
        return;
    }
    
    // Handle 'initial' keyword
    if (rawValue == "initial") {
        if (property == "color") style.color = CSSColor::black();
        else if (property == "font-size") style.fontSize = 16.0f;
        else if (property == "font-weight") style.fontWeight = FontWeight::Normal;
        else if (property == "display") style.display = DisplayValue::Inline;
        else if (property == "position") style.position = PositionValue::Static;
        else if (property == "opacity") style.opacity = 1.0f;
        else if (property == "font-style") style.fontStyle = FontStyle::Normal;
        else if (property == "line-height") {
            style.lineHeight = 1.2f;
            style.lineHeightNormal = true;
            style.lineHeightPx = 0;
        }
        else if (property == "white-space") style.whiteSpace = "normal";
        else if (property == "vertical-align") style.verticalAlign = "baseline";
        else if (property == "text-align") {
            style.textAlign = TextAlign::Start;
            style.legacyAlign = 0;
        }
        return;
    }
    
    // var(): substitute the element's custom properties, then apply the
    // result as if written directly. A var() that can't be resolved makes the
    // value invalid at computed-value time: the property stays as inherited
    // or initial.
    if (containsVarFunction(rawValue)) {
        static const CustomPropertyMap kNoCustomProperties;
        std::string substituted;
        if (!substituteVarFunctions(rawValue, style.customProperties ? *style.customProperties : kNoCustomProperties,
                                    substituted) ||
            substituted.empty() || containsVarFunction(substituted)) {
            // Behaves as unset: inherited properties take the parent's value
            static const std::unordered_set<std::string> kInherited = {
                "color", "font-family", "font-size", "font-weight", "font-style", "line-height",
                "text-align", "letter-spacing", "word-spacing", "white-space", "visibility",
                "cursor", "border-spacing", "border-collapse"};
            applyProperty(style, property, kInherited.count(property) && parentStyle ? "inherit" : "initial",
                          parentStyle);
            return;
        }
        applyProperty(style, property, substituted, parentStyle);
        return;
    }
    // Use resolved value for all property checks below
    const std::string& value = rawValue;

    // =====================================================================
    // Display
    // =====================================================================
    if (property == "display") {
        if (value == "none") style.display = DisplayValue::None;
        else if (value == "block") style.display = DisplayValue::Block;
        else if (value == "inline") style.display = DisplayValue::Inline;
        else if (value == "inline-block") style.display = DisplayValue::InlineBlock;
        else if (value == "flex") style.display = DisplayValue::Flex;
        else if (value == "inline-flex") style.display = DisplayValue::InlineFlex;
        else if (value == "grid") style.display = DisplayValue::Grid;
        else if (value == "inline-grid") style.display = DisplayValue::InlineGrid;
        else if (value == "table") style.display = DisplayValue::Table;
        else if (value == "table-row") style.display = DisplayValue::TableRow;
        else if (value == "table-cell") style.display = DisplayValue::TableCell;
        else if (value == "inline-table") style.display = DisplayValue::InlineTable;
        else if (value == "table-row-group") style.display = DisplayValue::TableRowGroup;
        else if (value == "table-header-group") style.display = DisplayValue::TableHeaderGroup;
        else if (value == "table-footer-group") style.display = DisplayValue::TableFooterGroup;
        else if (value == "table-column") style.display = DisplayValue::TableColumn;
        else if (value == "table-column-group") style.display = DisplayValue::TableColumnGroup;
        else if (value == "table-caption") style.display = DisplayValue::TableCaption;
        else if (value == "list-item") style.display = DisplayValue::ListItem;
        else if (value == "contents") style.display = DisplayValue::Contents;
    }

    // =====================================================================
    // Position
    // =====================================================================
    else if (property == "position") {
        if (value == "static") style.position = PositionValue::Static;
        else if (value == "relative") style.position = PositionValue::Relative;
        else if (value == "absolute") style.position = PositionValue::Absolute;
        else if (value == "fixed") style.position = PositionValue::Fixed;
        else if (value == "sticky") style.position = PositionValue::Sticky;
    }
    else if (property == "top") { style.top = parseLength(value); }
    else if (property == "right") { style.right = parseLength(value); }
    else if (property == "bottom") { style.bottom = parseLength(value); }
    else if (property == "left") { style.left = parseLength(value); }
    else if (property == "inset") {
        // top, right, bottom, left: one to four values, like margin
        // (CSS Positioned Layout 3 §3.1.1)
        auto parts = splitValues(value);
        if (!parts.empty() && parts.size() <= 4) {
            const std::string& t = parts[0];
            const std::string& r = parts.size() > 1 ? parts[1] : t;
            const std::string& b = parts.size() > 2 ? parts[2] : t;
            const std::string& l = parts.size() > 3 ? parts[3] : r;
            style.top = parseLength(t);
            style.right = parseLength(r);
            style.bottom = parseLength(b);
            style.left = parseLength(l);
        }
    }
    else if (property == "inset-block" || property == "inset-inline") {
        // Horizontal, left-to-right writing: block is top/bottom, inline is left/right
        auto parts = splitValues(value);
        if (!parts.empty() && parts.size() <= 2) {
            CSSLength start = parseLength(parts[0]);
            CSSLength end = parseLength(parts.size() > 1 ? parts[1] : parts[0]);
            if (property == "inset-block") { style.top = start; style.bottom = end; }
            else { style.left = start; style.right = end; }
        }
    }
    else if (property == "inset-block-start") { style.top = parseLength(value); }
    else if (property == "inset-block-end") { style.bottom = parseLength(value); }
    else if (property == "inset-inline-start") { style.left = parseLength(value); }
    else if (property == "inset-inline-end") { style.right = parseLength(value); }
    else if (property == "z-index") {
        if (value == "auto") { style.zIndexAuto = true; }
        else { try { style.zIndex = std::stoi(value); style.zIndexAuto = false; } catch (...) {} }
    }
    else if (property == "float") {
        if (value == "left") style.cssFloat = 1;
        else if (value == "right") style.cssFloat = 2;
        else style.cssFloat = 0;
    }

    // =====================================================================
    // Typography
    // =====================================================================
    else if (property == "color") { style.color = parseColor(value); }
    else if (property == "font-size") {
        CSSLength len = parseLength(value);
        if (!len.isAuto()) {
            float parentFontSize = parentStyle ? parentStyle->fontSize : 16.0f;
            style.fontSize = len.toPx(parentFontSize, 16.0f, 1920, 1080, 0);
        }
    }
    else if (property == "font-family") { style.fontFamily = value; }
    else if (property == "font-weight") {
        if (value == "bold" || value == "700") style.fontWeight = FontWeight::Bold;
        else if (value == "normal" || value == "400") style.fontWeight = FontWeight::Normal;
        else if (value == "lighter" || value == "100") style.fontWeight = FontWeight::Lighter;
        else if (value == "bolder" || value == "900") style.fontWeight = FontWeight::Bolder;
        else if (value == "200") style.fontWeight = FontWeight::W200;
        else if (value == "300") style.fontWeight = FontWeight::W300;
        else if (value == "500") style.fontWeight = FontWeight::W500;
        else if (value == "600") style.fontWeight = FontWeight::W600;
        else if (value == "800") style.fontWeight = FontWeight::W800;
    }
    else if (property == "font-style") {
        if (value == "italic") style.fontStyle = FontStyle::Italic;
        else if (value == "oblique") style.fontStyle = FontStyle::Oblique;
        else style.fontStyle = FontStyle::Normal;
    }
    else if (property == "text-align") {
        // -webkit-left/center/right also align block descendants (the HTML
        // align attributes and <center> map to them)
        int legacy = 0;
        if (value == "left" || value == "start") style.textAlign = TextAlign::Left;
        else if (value == "right" || value == "end") style.textAlign = TextAlign::Right;
        else if (value == "center") style.textAlign = TextAlign::Center;
        else if (value == "justify") style.textAlign = TextAlign::Justify;
        else if (value == "-webkit-left") { style.textAlign = TextAlign::Left; legacy = 1; }
        else if (value == "-webkit-center") { style.textAlign = TextAlign::Center; legacy = 2; }
        else if (value == "-webkit-right") { style.textAlign = TextAlign::Right; legacy = 3; }
        else return;
        style.legacyAlign = legacy;
    }
    else if (property == "vertical-align") { style.verticalAlign = value; }
    else if (property == "text-decoration" || property == "text-decoration-line") {
        style.textDecoration = value;
    }
    else if (property == "text-transform") { style.textTransform = value; }
    else if (property == "letter-spacing") {
        // normal is 0 (CSS Text 3 §8)
        if (value == "normal") style.letterSpacing = 0;
        else {
            CSSLength len = parseLength(value);
            if (!len.isAuto()) style.letterSpacing = len.toPx(style.fontSize, 16.0f, 1920, 1080, 0);
        }
    }
    else if (property == "word-spacing") {
        // normal is 0 (CSS Text 3 §8)
        if (value == "normal") style.wordSpacing = 0;
        else {
            CSSLength len = parseLength(value);
            if (!len.isAuto()) style.wordSpacing = len.toPx(style.fontSize, 16.0f, 1920, 1080, 0);
        }
    }
    else if (property == "white-space") { style.whiteSpace = value; }

    // =====================================================================
    // Tables
    // =====================================================================
    else if (property == "border-spacing") {
        auto parts = splitValues(value);
        if (!parts.empty()) {
            float h = parseLength(parts[0]).toPx(style.fontSize, 16.0f, 1920, 1080, 0);
            float v = parts.size() > 1 ? parseLength(parts[1]).toPx(style.fontSize, 16.0f, 1920, 1080, 0) : h;
            style.borderSpacingH = std::max(0.0f, h);
            style.borderSpacingV = std::max(0.0f, v);
        }
    }
    else if (property == "border-collapse") { style.borderCollapse = (value == "collapse"); }
    else if (property == "table-layout") { style.tableLayoutFixed = (value == "fixed"); }
    else if (property == "line-height") {
        // A number inherits as a number (scaling with each element's font
        // size); a length or percentage computes to px here and inherits as
        // that px value (CSS Inline 3 §4.3)
        auto setNumber = [&](float n) {
            if (!(n >= 0)) return;
            style.lineHeight = n;
            style.lineHeightNormal = false;
            style.lineHeightPx = 0;
        };
        auto setPx = [&](float px) {
            if (!(px >= 0)) return;
            style.lineHeightPx = std::max(px, 0.001f);
            style.lineHeight = style.fontSize > 0 ? px / style.fontSize : 1.2f;
            style.lineHeightNormal = false;
        };
        if (value == "normal") {
            style.lineHeight = 1.2f;
            style.lineHeightNormal = true;
            style.lineHeightPx = 0;
        } else if (isCSSMathFunction(value)) {
            // calc(2 / 1.5) is a number; calc(1rem + 2px) a length
            CSSMathValue math;
            if (evaluateCSSMath(value, lengthContext_, math)) {
                if (math.isNumber) setNumber(math.number);
                else if (math.length.unit == CSSLength::Unit::Percent) setPx(math.length.value / 100.0f * style.fontSize);
                else setPx(math.length.toPx(style.fontSize, 16.0f, lengthContext_.viewportWidth,
                                            lengthContext_.viewportHeight, 0));
            }
        } else if (value.find_first_of("0123456789.") != std::string::npos) {
            char* end = nullptr;
            float val = std::strtof(value.c_str(), &end);
            std::string unit = end ? std::string(end) : "";
            if (end == value.c_str()) return;
            if (unit.empty()) setNumber(val);
            else if (unit == "%") setPx(val / 100.0f * style.fontSize);
            else {
                CSSLength len = parseLength(value);
                if (!len.isAuto()) setPx(len.toPx(style.fontSize, 16.0f, lengthContext_.viewportWidth,
                                                  lengthContext_.viewportHeight, 0));
            }
        }
    }

    // =====================================================================
    // Dimensions
    // =====================================================================
    else if (property == "width") { style.width = parseLength(value); }
    else if (property == "height") { style.height = parseLength(value); }
    else if (property == "min-width") { style.minWidth = parseLength(value); }
    else if (property == "min-height") { style.minHeight = parseLength(value); }
    else if (property == "max-width") { style.maxWidth = parseLength(value); }
    else if (property == "max-height") { style.maxHeight = parseLength(value); }

    // =====================================================================
    // Margin
    // =====================================================================
    else if (property == "margin") {
        auto parts = splitValues(value);
        if (parts.size() == 1) {
            CSSLength len = parseLength(parts[0]);
            style.marginTop = len; style.marginRight = len;
            style.marginBottom = len; style.marginLeft = len;
        } else if (parts.size() == 2) {
            style.marginTop = parseLength(parts[0]);
            style.marginRight = parseLength(parts[1]);
            style.marginBottom = parseLength(parts[0]);
            style.marginLeft = parseLength(parts[1]);
        } else if (parts.size() == 3) {
            style.marginTop = parseLength(parts[0]);
            style.marginRight = parseLength(parts[1]);
            style.marginBottom = parseLength(parts[2]);
            style.marginLeft = parseLength(parts[1]);
        } else if (parts.size() >= 4) {
            style.marginTop = parseLength(parts[0]);
            style.marginRight = parseLength(parts[1]);
            style.marginBottom = parseLength(parts[2]);
            style.marginLeft = parseLength(parts[3]);
        }
    }
    else if (property == "margin-top") { style.marginTop = parseLength(value); }
    else if (property == "margin-right") { style.marginRight = parseLength(value); }
    else if (property == "margin-bottom") { style.marginBottom = parseLength(value); }
    else if (property == "margin-left") { style.marginLeft = parseLength(value); }

    // =====================================================================
    // Padding
    // =====================================================================
    else if (property == "padding") {
        auto parts = splitValues(value);
        if (parts.size() == 1) {
            CSSLength len = parseLength(parts[0]);
            style.paddingTop = len; style.paddingRight = len;
            style.paddingBottom = len; style.paddingLeft = len;
        } else if (parts.size() == 2) {
            style.paddingTop = parseLength(parts[0]);
            style.paddingRight = parseLength(parts[1]);
            style.paddingBottom = parseLength(parts[0]);
            style.paddingLeft = parseLength(parts[1]);
        } else if (parts.size() == 3) {
            style.paddingTop = parseLength(parts[0]);
            style.paddingRight = parseLength(parts[1]);
            style.paddingBottom = parseLength(parts[2]);
            style.paddingLeft = parseLength(parts[1]);
        } else if (parts.size() >= 4) {
            style.paddingTop = parseLength(parts[0]);
            style.paddingRight = parseLength(parts[1]);
            style.paddingBottom = parseLength(parts[2]);
            style.paddingLeft = parseLength(parts[3]);
        }
    }
    else if (property == "padding-top") { style.paddingTop = parseLength(value); }
    else if (property == "padding-right") { style.paddingRight = parseLength(value); }
    else if (property == "padding-bottom") { style.paddingBottom = parseLength(value); }
    else if (property == "padding-left") { style.paddingLeft = parseLength(value); }

    // =====================================================================
    // Border
    // =====================================================================
    else if (property == "border-width") {
        CSSLength len = parseLength(value);
        float px = len.toPx(style.fontSize, 16.0f, 1920, 1080, 0);
        style.borderTopWidth = px; style.borderRightWidth = px;
        style.borderBottomWidth = px; style.borderLeftWidth = px;
    }
    else if (property == "border-top-width") {
        style.borderTopWidth = parseLength(value).toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
    else if (property == "border-right-width") {
        style.borderRightWidth = parseLength(value).toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
    else if (property == "border-bottom-width") {
        style.borderBottomWidth = parseLength(value).toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
    else if (property == "border-left-width") {
        style.borderLeftWidth = parseLength(value).toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
    else if (property == "border-color") {
        CSSColor c = parseColor(value);
        style.borderTopColor = c; style.borderRightColor = c;
        style.borderBottomColor = c; style.borderLeftColor = c;
    }
    else if (property == "border-top-color") { style.borderTopColor = parseColor(value); }
    else if (property == "border-right-color") { style.borderRightColor = parseColor(value); }
    else if (property == "border-bottom-color") { style.borderBottomColor = parseColor(value); }
    else if (property == "border-left-color") { style.borderLeftColor = parseColor(value); }
    else if (property == "border-radius") {
        // The first value (a percentage of the box is resolved when painting)
        auto parts = splitValues(value);
        CSSLength len = parseLength(parts.empty() ? value : parts[0]);
        float px = len.unit == CSSLength::Unit::Percent ? 0 : len.toPx(style.fontSize, 16.0f, 1920, 1080, 0);
        style.borderTopLeftRadius = px; style.borderTopRightRadius = px;
        style.borderBottomRightRadius = px; style.borderBottomLeftRadius = px;
        style.borderTopLeftRadiusPercent = len.unit == CSSLength::Unit::Percent ? len.value : 0;
    }
    else if (property == "border-top-left-radius") {
        CSSLength len = parseLength(value);
        bool percent = len.unit == CSSLength::Unit::Percent;
        style.borderTopLeftRadius = percent ? 0 : len.toPx(style.fontSize, 16.0f, 1920, 1080, 0);
        style.borderTopLeftRadiusPercent = percent ? len.value : 0;
    }
    else if (property == "border-top-right-radius") {
        style.borderTopRightRadius = parseLength(value).toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
    else if (property == "border-bottom-right-radius") {
        style.borderBottomRightRadius = parseLength(value).toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
    else if (property == "border-bottom-left-radius") {
        style.borderBottomLeftRadius = parseLength(value).toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
    else if (property == "border" || property == "border-top" || 
             property == "border-right" || property == "border-bottom" || 
             property == "border-left") {
        // Shorthand: "1px solid #000"
        // Parse width, style, color from space-separated values
        // Omitted parts take their initial values: medium, none, currentColor
        // (CSS Backgrounds 3 §3.5); a side without a style draws nothing
        std::istringstream ss(value);
        std::string token;
        float bw = 3;
        BorderStyle bs = BorderStyle::None;
        CSSColor bc = style.color;
        while (ss >> token) {
            if (parseBorderStyle(token, bs)) continue;
            if (token == "thin") { bw = 1; continue; }
            if (token == "medium") { bw = 3; continue; }
            if (token == "thick") { bw = 5; continue; }
            // A width ("1px", ".5em", "0") before the colour checks: unknown
            // words parse as black, so "1px" would otherwise count as a colour
            if (std::isdigit((unsigned char)token[0]) || token[0] == '.') {
                CSSLength len = parseLength(token);
                if (!len.isAuto()) bw = len.toPx(style.fontSize, 16.0f, 1920, 1080, 0);
                continue;
            }
            if (token[0] == '#' || token.substr(0,3) == "rgb" || 
                CSSColor::parse(token).a != 0 || token == "transparent") {
                bc = CSSColor::parse(token); continue;
            }
            CSSLength len = parseLength(token);
            if (!len.isAuto()) bw = len.toPx(style.fontSize, 16.0f, 1920, 1080, 0);
        }
        if (property == "border") {
            style.borderTopWidth = bw; style.borderRightWidth = bw;
            style.borderBottomWidth = bw; style.borderLeftWidth = bw;
            style.borderTopColor = bc; style.borderRightColor = bc;
            style.borderBottomColor = bc; style.borderLeftColor = bc;
            style.borderTopStyle = bs; style.borderRightStyle = bs;
            style.borderBottomStyle = bs; style.borderLeftStyle = bs;
        } else if (property == "border-top") { style.borderTopWidth = bw; style.borderTopColor = bc; style.borderTopStyle = bs; }
        else if (property == "border-right") { style.borderRightWidth = bw; style.borderRightColor = bc; style.borderRightStyle = bs; }
        else if (property == "border-bottom") { style.borderBottomWidth = bw; style.borderBottomColor = bc; style.borderBottomStyle = bs; }
        else if (property == "border-left") { style.borderLeftWidth = bw; style.borderLeftColor = bc; style.borderLeftStyle = bs; }
    }
    else if (property == "border-style") {
        // One to four values: top, right, bottom, left (like margin)
        auto parts = splitValues(value);
        std::vector<BorderStyle> v;
        for (const auto& p : parts) {
            BorderStyle b;
            if (!parseBorderStyle(p, b)) { v.clear(); break; }
            v.push_back(b);
        }
        if (!v.empty() && v.size() <= 4) {
            style.borderTopStyle = v[0];
            style.borderRightStyle = v.size() > 1 ? v[1] : v[0];
            style.borderBottomStyle = v.size() > 2 ? v[2] : v[0];
            style.borderLeftStyle = v.size() > 3 ? v[3] : style.borderRightStyle;
        }
    }
    else if (property == "border-top-style" || property == "border-right-style" ||
             property == "border-bottom-style" || property == "border-left-style") {
        BorderStyle b;
        if (parseBorderStyle(value, b)) {
            if (property == "border-top-style") style.borderTopStyle = b;
            else if (property == "border-right-style") style.borderRightStyle = b;
            else if (property == "border-bottom-style") style.borderBottomStyle = b;
            else style.borderLeftStyle = b;
        }
    }

    // =====================================================================
    // Flexbox
    // =====================================================================
    else if (property == "flex-direction") {
        if (value == "row") style.flexDirection = FlexDirection::Row;
        else if (value == "row-reverse") style.flexDirection = FlexDirection::RowReverse;
        else if (value == "column") style.flexDirection = FlexDirection::Column;
        else if (value == "column-reverse") style.flexDirection = FlexDirection::ColumnReverse;
    }
    else if (property == "flex-wrap") {
        style.flexWrap = (value == "wrap" || value == "wrap-reverse");
        style.wrapReverse = (value == "wrap-reverse");
    }
    else if (property == "justify-content") {
        style.justifyContentNormal = value == "normal";
        if (value == "flex-start" || value == "start") style.justifyContent = JustifyAlign::FlexStart;
        else if (value == "flex-end" || value == "end") style.justifyContent = JustifyAlign::FlexEnd;
        else if (value == "center") style.justifyContent = JustifyAlign::Center;
        else if (value == "space-between") style.justifyContent = JustifyAlign::SpaceBetween;
        else if (value == "space-around") style.justifyContent = JustifyAlign::SpaceAround;
        else if (value == "space-evenly") style.justifyContent = JustifyAlign::SpaceEvenly;
    }
    else if (property == "align-items") {
        if (value == "flex-start" || value == "start") style.alignItems = JustifyAlign::FlexStart;
        else if (value == "flex-end" || value == "end") style.alignItems = JustifyAlign::FlexEnd;
        else if (value == "center") style.alignItems = JustifyAlign::Center;
        else if (value == "stretch") style.alignItems = JustifyAlign::Stretch;
        else if (value == "baseline") style.alignItems = JustifyAlign::Baseline;
    }
    else if (property == "align-content") {
        if (value == "flex-start" || value == "start") style.alignContent = JustifyAlign::FlexStart;
        else if (value == "flex-end" || value == "end") style.alignContent = JustifyAlign::FlexEnd;
        else if (value == "center") style.alignContent = JustifyAlign::Center;
        else if (value == "stretch") style.alignContent = JustifyAlign::Stretch;
        else if (value == "space-between") style.alignContent = JustifyAlign::SpaceBetween;
        else if (value == "space-around") style.alignContent = JustifyAlign::SpaceAround;
    }
    else if (property == "align-self") {
        if (value == "auto") style.alignSelf = JustifyAlign::Auto;
        else if (value == "flex-start" || value == "start") style.alignSelf = JustifyAlign::Start;
        else if (value == "flex-end" || value == "end") style.alignSelf = JustifyAlign::End;
        else if (value == "center") style.alignSelf = JustifyAlign::Center;
        else if (value == "stretch") style.alignSelf = JustifyAlign::Stretch;
        else if (value == "baseline") style.alignSelf = JustifyAlign::Baseline;
    }
    else if (property == "flex-grow") {
        try { style.flexGrow = std::stof(value); } catch (...) {}
    }
    else if (property == "flex-shrink") {
        try { style.flexShrink = std::stof(value); } catch (...) {}
    }
    else if (property == "flex-basis") { style.flexBasis = parseLength(value); }
    else if (property == "flex") {
        // Shorthand: "1" or "1 0 auto" or "none" or "auto"
        if (value == "none") { style.flexGrow = 0; style.flexShrink = 0; style.flexBasis = CSSLength::auto_(); }
        else if (value == "auto") { style.flexGrow = 1; style.flexShrink = 1; style.flexBasis = CSSLength::auto_(); }
        else {
            try { style.flexGrow = std::stof(value); style.flexShrink = 1; style.flexBasis = CSSLength::px(0); } catch (...) {}
        }
    }
    else if (property == "order") {
        try { style.order = std::stoi(value); } catch (...) {}
    }
    else if (property == "gap" || property == "grid-gap") {
        // gap: <row-gap> [<column-gap>]
        auto parts = splitValues(value);
        style.gap = parseLength(parts.empty() ? value : parts[0]);
        style.rowGap = style.gap;
        style.columnGap = parts.size() > 1 ? parseLength(parts[1]) : style.gap;
    }
    else if (property == "row-gap" || property == "grid-row-gap") { style.rowGap = parseLength(value); }
    else if (property == "column-gap" || property == "grid-column-gap") { style.columnGap = parseLength(value); }

    // =====================================================================
    // Grid
    // =====================================================================
    else if (property == "grid-template-columns") { style.gridTemplateColumns = value; }
    else if (property == "grid-template-rows") { style.gridTemplateRows = value; }
    else if (property == "grid-template-areas") { style.gridTemplateAreas = value; }
    else if (property == "grid-template" || property == "grid") {
        // <rows> / <columns> (the area-string form keeps only its tracks)
        if (value == "none") {
            style.gridTemplateRows = style.gridTemplateColumns = style.gridTemplateAreas = "";
        } else if (value.find('"') == std::string::npos && value.find('\'') == std::string::npos) {
            auto slash = value.find('/');
            if (slash != std::string::npos) {
                style.gridTemplateRows = trimGridText(value.substr(0, slash));
                style.gridTemplateColumns = trimGridText(value.substr(slash + 1));
            }
        } else {
            // "a a" 40px "b c" 1fr / 1fr 2fr: areas, row sizes after each string
            std::string areas, rows;
            auto slash = value.rfind('/');
            std::string head = slash != std::string::npos && slash > value.rfind('"') ? value.substr(0, slash) : value;
            size_t i = 0;
            while (i < head.size()) {
                char q = head[i];
                if (q == '"' || q == '\'') {
                    size_t end = head.find(q, i + 1);
                    if (end == std::string::npos) break;
                    areas += head.substr(i, end - i + 1) + " ";
                    i = end + 1;
                    size_t next = head.find_first_of("\"'", i);
                    std::string size = trimGridText(head.substr(i, next == std::string::npos ? std::string::npos : next - i));
                    rows += (size.empty() ? "auto" : size) + " ";
                    i = next == std::string::npos ? head.size() : next;
                } else {
                    ++i;
                }
            }
            style.gridTemplateAreas = trimGridText(areas);
            style.gridTemplateRows = trimGridText(rows);
            if (head.size() < value.size()) style.gridTemplateColumns = trimGridText(value.substr(head.size() + 1));
        }
    }
    else if (property == "grid-auto-columns") { style.gridAutoColumns = value; }
    else if (property == "grid-auto-rows") { style.gridAutoRows = value; }
    else if (property == "grid-auto-flow") {
        style.gridAutoFlowColumn = value.find("column") != std::string::npos;
        style.gridAutoFlowDense = value.find("dense") != std::string::npos;
    }
    else if (property == "grid-column-start") { style.gridColumnStart = value; }
    else if (property == "grid-column-end") { style.gridColumnEnd = value; }
    else if (property == "grid-row-start") { style.gridRowStart = value; }
    else if (property == "grid-row-end") { style.gridRowEnd = value; }
    else if (property == "grid-column" || property == "grid-row") {
        std::string start, end;
        splitGridLine(value, start, end);
        if (property == "grid-column") { style.gridColumnStart = start; style.gridColumnEnd = end; }
        else { style.gridRowStart = start; style.gridRowEnd = end; }
    }
    else if (property == "grid-area") {
        // row-start / column-start / row-end / column-end; a missing line
        // repeats a name given before it, or is auto
        std::vector<std::string> lines;
        size_t from = 0;
        for (;;) {
            size_t slash = value.find('/', from);
            lines.push_back(trimGridText(value.substr(from, slash == std::string::npos ? std::string::npos : slash - from)));
            if (slash == std::string::npos) break;
            from = slash + 1;
        }
        auto fallback = [&](size_t i, size_t from) {
            if (i < lines.size()) return lines[i];
            return isGridIdent(lines[from]) ? lines[from] : std::string("auto");
        };
        style.gridRowStart = lines[0];
        style.gridColumnStart = fallback(1, 0);
        style.gridRowEnd = fallback(2, 0);
        style.gridColumnEnd = lines.size() > 3 ? lines[3] : (lines.size() > 1 ? fallback(3, 1) : fallback(3, 0));
    }
    else if (property == "justify-items") { style.justifyItems = parseSelfAlign(value, JustifyAlign::Stretch); }
    else if (property == "justify-self") { style.justifySelf = parseSelfAlign(value, JustifyAlign::Auto); }
    else if (property == "place-self") {
        auto parts = splitValues(value);
        if (!parts.empty()) {
            style.alignSelf = parseSelfAlign(parts[0], JustifyAlign::Auto);
            style.justifySelf = parseSelfAlign(parts.size() > 1 ? parts[1] : parts[0], JustifyAlign::Auto);
        }
    }

    // =====================================================================
    // Background & Visual
    // =====================================================================
    else if (property == "background-color") { style.backgroundColor = parseColor(value); }
    else if (property == "background-image") { style.backgroundImage = value; }
    else if (property == "background-size") { style.backgroundSize = value; }
    else if (property == "background-position") { style.backgroundPosition = value; }
    else if (property == "background-repeat") { style.backgroundRepeat = value; }
    else if (property == "background" && value != "inherit" && value != "initial" && value != "unset") {
        // Shorthand (CSS Backgrounds 3 §3.10), one layer: image, repeat,
        // position [/ size] and colour, in any order; unset parts reset
        style.backgroundImage.clear();
        style.backgroundColor = CSSColor::transparent();
        style.backgroundRepeat = "repeat";
        style.backgroundPosition.clear();
        style.backgroundSize.clear();
        style.backgroundClipText = false;
        std::vector<std::string> tokens;
        std::string token;
        int depth = 0;
        for (char c : value) {
            if (c == '(') depth++;
            if (c == ')') depth--;
            if (depth == 0 && (std::isspace((unsigned char)c) || c == '/')) {
                if (!token.empty()) tokens.push_back(token);
                token.clear();
                if (c == '/') tokens.push_back("/");
                continue;
            }
            token += c;
        }
        if (!token.empty()) tokens.push_back(token);
        bool inSize = false;
        std::string repeat;
        for (const std::string& t : tokens) {
            std::string lower = t;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            bool isNumber = !t.empty() && (std::isdigit((unsigned char)t[0]) || t[0] == '-' || t[0] == '.');
            if (t == "/") { inSize = true; continue; }
            if (lower.compare(0, 4, "url(") == 0 || lower.find("gradient(") != std::string::npos) {
                style.backgroundImage = t;
            } else if (lower == "repeat" || lower == "no-repeat" || lower == "repeat-x" || lower == "repeat-y" ||
                       lower == "space" || lower == "round") {
                repeat += (repeat.empty() ? "" : " ") + lower;
            } else if (inSize && (isNumber || lower == "auto" || lower == "cover" || lower == "contain")) {
                style.backgroundSize += (style.backgroundSize.empty() ? "" : " ") + lower;
            } else if (isNumber || lower == "left" || lower == "right" || lower == "top" || lower == "bottom" ||
                       lower == "center") {
                style.backgroundPosition += (style.backgroundPosition.empty() ? "" : " ") + lower;
            } else if (lower == "none" || lower == "scroll" || lower == "fixed" || lower == "local" ||
                       lower == "border-box" || lower == "padding-box" || lower == "content-box") {
                // image none, attachment, origin/clip: not used
            } else {
                style.backgroundColor = parseColor(t);
            }
        }
        if (!repeat.empty()) style.backgroundRepeat = repeat;
    }
    else if (property == "opacity") {
        try { style.opacity = std::stof(value); } catch (...) {}
    }
    else if (property == "box-shadow") { style.boxShadow = value; }
    else if (property == "background-clip" || property == "-webkit-background-clip") {
        std::string v = value;
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        style.backgroundClipText = v == "text";
    }
    else if (property == "-webkit-text-fill-color") {
        std::string v = value;
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        style.hasTextFillColor = v != "currentcolor";
        if (style.hasTextFillColor) style.textFillColor = CSSColor::parse(v);
    }
    else if (property == "-webkit-text-stroke-width" || property == "-webkit-text-stroke-color" ||
             property == "-webkit-text-stroke") {
        // <line-width> || <color> (Compatibility §7.2); thin/medium/thick
        // are 1/3/5px
        auto applyWidth = [&](const std::string& token) {
            std::string t = token;
            std::transform(t.begin(), t.end(), t.begin(), ::tolower);
            if (t == "thin") { style.textStrokeWidth = 1; return true; }
            if (t == "medium") { style.textStrokeWidth = 3; return true; }
            if (t == "thick") { style.textStrokeWidth = 5; return true; }
            if (t.empty() || !(std::isdigit((unsigned char)t[0]) || t[0] == '.')) return false;
            CSSLength len = parseLength(t);
            if (len.isAuto()) return false;
            style.textStrokeWidth = std::max(0.0f, len.toPx(style.fontSize, 16.0f, lengthContext_.viewportWidth,
                                                            lengthContext_.viewportHeight, 0));
            return true;
        };
        auto applyColor = [&](const std::string& token) {
            std::string t = token;
            std::transform(t.begin(), t.end(), t.begin(), ::tolower);
            style.hasTextStrokeColor = t != "currentcolor";
            if (style.hasTextStrokeColor) style.textStrokeColor = CSSColor::parse(t);
        };
        if (property == "-webkit-text-stroke-width") applyWidth(value);
        else if (property == "-webkit-text-stroke-color") applyColor(value);
        else {
            style.textStrokeWidth = 0;
            style.hasTextStrokeColor = false;
            for (const auto& part : splitValues(value)) {
                if (!applyWidth(part)) applyColor(part);
            }
        }
    }
    else if (property == "transform") { style.transform = value; }
    else if (property == "transform-origin") { style.transformOrigin = value; }
    else if (property == "translate") { style.translate = value; }
    else if (property == "rotate") { style.rotate = value; }
    else if (property == "scale") { style.scale = value; }
    else if (property == "transition") {
        style.transition = value;
        setTransitionShorthand(style, value);
    }
    else if (property == "transition-property") { style.transitionProperty = value; }
    else if (property == "transition-duration") { style.transitionDuration = value; }
    else if (property == "transition-delay") { style.transitionDelay = value; }
    else if (property == "transition-timing-function") { style.transitionTimingFunction = value; }
    else if (property == "animation-timing-function") { style.animationTimingFunction = value; }
    else if (property == "animation-play-state") { style.animationPlayState = value; }
    else if (property == "animation") {
        style.animation = value;
        setAnimationShorthand(style, value);
    }
    else if (property == "animation-name") { style.animationName = value; }
    else if (property == "animation-duration") { style.animationDuration = value; }
    else if (property == "animation-delay") { style.animationDelay = value; }
    else if (property == "animation-iteration-count") { style.animationIterationCount = value; }
    else if (property == "animation-direction") { style.animationDirection = value; }
    else if (property == "animation-fill-mode") { style.animationFillMode = value; }
    else if (property == "filter") { style.filter = value; }

    // =====================================================================
    // Overflow & Box
    // =====================================================================
    else if (property == "overflow" || property == "overflow-x" || property == "overflow-y") {
        auto keyword = [](const std::string& v) {
            if (v == "hidden") return OverflowValue::Hidden;
            if (v == "scroll") return OverflowValue::Scroll;
            if (v == "auto" || v == "overlay") return OverflowValue::Auto;
            if (v == "clip") return OverflowValue::Clip;
            return OverflowValue::Visible;
        };
        if (property == "overflow-x") style.overflowX = keyword(value);
        else if (property == "overflow-y") style.overflowY = keyword(value);
        else {
            // One value for both axes, or "x y" (CSS Overflow 3, 3.1)
            std::istringstream in(value);
            std::string first, second;
            in >> first >> second;
            style.overflowX = keyword(first);
            style.overflowY = second.empty() ? style.overflowX : keyword(second);
        }
    }
    else if (property == "box-sizing") {
        if (value == "border-box") style.boxSizing = BoxSizing::BorderBox;
        else style.boxSizing = BoxSizing::ContentBox;
    }
    else if (property == "visibility") {
        if (value == "hidden") style.visibility = Visibility::Hidden;
        else if (value == "collapse") style.visibility = Visibility::Collapse;
        else style.visibility = Visibility::Visible;
    }
    else if (property == "cursor") { style.cursor = value; }
    else if (property == "pointer-events") { style.pointerEvents = value; }

    // =====================================================================
    // Tailwind / Modern CSS Properties
    // =====================================================================
    else if (property == "text-overflow") { style.textOverflow = value; }
    else if (property == "object-fit") { style.objectFit = value; }
    else if (property == "object-position") { style.objectPosition = value; }
    else if (property == "aspect-ratio") { style.aspectRatio = value; }
    else if (property == "backdrop-filter" || property == "-webkit-backdrop-filter") {
        style.backdropFilter = value;
    }
    else if (property == "place-items") {
        // <align-items> [<justify-items>]
        style.placeItems = value;
        auto parts = splitValues(value);
        if (!parts.empty()) {
            style.alignItems = parseSelfAlign(parts[0], JustifyAlign::Stretch);
            style.justifyItems = parseSelfAlign(parts.size() > 1 ? parts[1] : parts[0], JustifyAlign::Stretch);
        }
    }
    else if (property == "place-content") {
        // <align-content> [<justify-content>]
        style.placeContent = value;
        auto parts = splitValues(value);
        if (!parts.empty()) {
            applyProperty(style, "align-content", parts[0], parentStyle);
            applyProperty(style, "justify-content", parts.size() > 1 ? parts[1] : parts[0], parentStyle);
        }
    }
    else if (property == "isolation") { style.isolation = value; }
    else if (property == "mix-blend-mode") { style.mixBlendMode = value; }
    else if (property == "will-change") { style.willChange = value; }
    else if (property == "content") { style.content = value; }
    else if (property == "user-select" || property == "-webkit-user-select") {
        style.userSelect = value;
    }
    else if (property == "appearance" || property == "-webkit-appearance") {
        style.appearance = value;
    }
    else if (property == "outline") {
        // Width, style and colour in any order; omitted parts take their
        // initial values: medium, none, currentColor (CSS UI 4 §5.1)
        float width = 3;
        std::string outlineStyle = "none";
        CSSColor color = style.color;
        std::istringstream ss(value);
        std::string token;
        while (ss >> token) {
            BorderStyle b;
            std::string t = token;
            std::transform(t.begin(), t.end(), t.begin(), ::tolower);
            if (t == "auto" || parseBorderStyle(t, b)) { outlineStyle = t; continue; }
            if (t == "thin") { width = 1; continue; }
            if (t == "medium") { width = 3; continue; }
            if (t == "thick") { width = 5; continue; }
            if (std::isdigit((unsigned char)t[0]) || t[0] == '.') {
                CSSLength len = parseLength(t);
                if (!len.isAuto()) width = len.toPx(style.fontSize, 16.0f, 1920, 1080, 0);
                continue;
            }
            color = parseColor(token);
        }
        style.outlineWidth = width;
        style.outlineStyle = outlineStyle;
        style.outlineColor = color;
    }
    else if (property == "outline-width") {
        CSSLength len = parseLength(value);
        if (!len.isAuto()) style.outlineWidth = len.toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
    else if (property == "outline-color") { style.outlineColor = parseColor(value); }
    else if (property == "outline-style") {
        std::string t = value;
        std::transform(t.begin(), t.end(), t.begin(), ::tolower);
        style.outlineStyle = t;
    }
    else if (property == "outline-offset") {
        CSSLength len = parseLength(value);
        if (!len.isAuto()) style.outlineOffset = len.toPx(style.fontSize, 16.0f, 1920, 1080, 0);
    }
}

// ============================================================================
// Value Parsing
// ============================================================================

CSSLength StyleResolver::parseLength(const std::string& value) {
    // Every unit, scientific notation and the math functions (calc(),
    // min(), max(), clamp()) live in CSSLength::parse; the context carries
    // the element's font size and the viewport for their em/vw terms
    return CSSLength::parse(value, lengthContext_);
}

CSSColor StyleResolver::parseColor(const std::string& value) {
    return CSSColor::parse(value);
}


const CSSKeyframesRule* StyleResolver::findKeyframes(const std::string& name) const {
    // The last rule of that name wins (CSS Animations 1 §3)
    const CSSKeyframesRule* found = nullptr;
    for (const auto& entry : stylesheets_) {
        CSSRuleList* rules = entry.sheet ? entry.sheet->cssRules() : nullptr;
        for (size_t i = 0; rules && i < rules->length(); i++) {
            auto* keyframes = dynamic_cast<const CSSKeyframesRule*>(rules->item(i));
            if (keyframes && keyframes->name() == name) found = keyframes;
        }
    }
    return found;
}

std::vector<const CSSStyleDeclaration*> StyleResolver::animationEndStates(const CSSComputedStyle& style) const {
    std::vector<const CSSStyleDeclaration*> frames;
    if (style.animationName.empty()) return frames;
    const auto names = splitCommaList(style.animationName);
    const auto fills = splitCommaList(style.animationFillMode);
    const auto counts = splitCommaList(style.animationIterationCount);
    const auto directions = splitCommaList(style.animationDirection);
    // A shorter list repeats to the length of animation-name (§4.2)
    auto at = [](const std::vector<std::string>& list, size_t i, const char* initial) {
        if (list.empty() || (list.size() == 1 && list[0].empty())) return std::string(initial);
        return lowerAscii(list[i % list.size()]);
    };
    for (size_t i = 0; i < names.size(); i++) {
        const std::string& name = names[i];
        if (name.empty() || lowerAscii(name) == "none") continue;
        std::string fill = at(fills, i, "none");
        if (fill != "forwards" && fill != "both") continue;
        std::string count = at(counts, i, "1");
        if (count == "infinite") continue;   // never ends
        char* end = nullptr;
        double iterations = std::strtod(count.c_str(), &end);
        if (!end || *end != '\0' || !(iterations >= 0) || !std::isfinite(iterations)) iterations = 1;
        // The direction of the last iteration decides which end it stops at
        // (§4.6); a zero count stops at the start of the first. A fractional
        // count stops at the nearest end of its last iteration.
        const std::string direction = at(directions, i, "normal");
        const long last = iterations > 0 ? static_cast<long>(std::ceil(iterations)) - 1 : 0;
        bool forwardIteration = direction == "normal" || (direction == "alternate" && last % 2 == 0) ||
                                (direction == "alternate-reverse" && last % 2 == 1);
        float progress = iterations > 0 ? 1.0f : 0.0f;
        float offset = forwardIteration ? progress : 1.0f - progress;
        const CSSKeyframesRule* keyframes = findKeyframes(name);
        CSSRuleList* list = keyframes ? keyframes->cssRules() : nullptr;
        for (size_t k = 0; list && k < list->length(); k++) {
            auto* frame = dynamic_cast<const CSSKeyframeRule*>(list->item(k));
            if (!frame || !frame->style()) continue;
            for (float o : frame->offsets()) {
                if (std::fabs(o - offset) < 1e-4f) {
                    frames.push_back(frame->style());
                    break;
                }
            }
        }
    }
    return frames;
}

} // namespace Zepra::WebCore
