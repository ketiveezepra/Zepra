// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file style_rules.cpp
 * @brief The Rules pane's model: matched rules, cascade winners, inherited rules.
 */
#include "style_rules.hpp"

#include <algorithm>
#include <tuple>
#include <unordered_set>

#include "browser/dom.hpp"
#include "css/presentational_hints.hpp"

using namespace Zepra::WebCore;

namespace ZepraBrowser::DevTools {

namespace {

const std::vector<std::string> kSides = {"top", "right", "bottom", "left"};

/// The value a property has when no declaration sets it (for the overrides of a switched-off declaration)
std::string initialValueOf(const std::string& longhand) {
    static const std::unordered_map<std::string, std::string> values = {
        {"display", "inline"}, {"position", "static"}, {"float", "none"}, {"clear", "none"},
        {"box-sizing", "content-box"}, {"opacity", "1"}, {"z-index", "auto"},
        {"width", "auto"}, {"height", "auto"}, {"min-width", "auto"}, {"min-height", "auto"},
        {"max-width", "none"}, {"max-height", "none"},
        {"top", "auto"}, {"right", "auto"}, {"bottom", "auto"}, {"left", "auto"},
        {"overflow-x", "visible"}, {"overflow-y", "visible"},
        {"background-color", "transparent"}, {"background-image", "none"},
        {"background-repeat", "repeat"}, {"background-position", "0% 0%"}, {"background-size", "auto"},
        {"background-attachment", "scroll"},
        {"outline-style", "none"}, {"outline-width", "0"}, {"outline-color", "currentcolor"},
        {"flex-direction", "row"}, {"flex-wrap", "nowrap"}, {"flex-grow", "0"}, {"flex-shrink", "1"},
        {"flex-basis", "auto"}, {"order", "0"}, {"justify-content", "flex-start"},
        {"align-items", "stretch"}, {"align-content", "stretch"}, {"align-self", "auto"},
        {"row-gap", "0"}, {"column-gap", "0"},
        {"text-decoration-line", "none"}, {"text-decoration-style", "solid"},
        {"text-decoration-color", "currentcolor"}, {"vertical-align", "baseline"},
        {"box-shadow", "none"}, {"transform", "none"}, {"transition", "none"},
    };
    auto it = values.find(longhand);
    if (it != values.end()) return it->second;
    for (const auto& side : kSides) {
        if (longhand == "margin-" + side || longhand == "padding-" + side) return "0";
        if (longhand == "border-" + side + "-width") return "0";
        if (longhand == "border-" + side + "-style") return "none";
        if (longhand == "border-" + side + "-color") return "currentcolor";
    }
    if (longhand.find("radius") != std::string::npos) return "0";
    return "initial";
}

bool setsInheritedProperty(const std::string& property) {
    for (const auto& l : longhandsOf(property)) {
        if (isInheritedProperty(l)) return true;
    }
    return false;
}

// Cascade tiers, lowest first (CSS Cascade 4, 6.4): normal declarations by
// origin with the style attribute above the author rules, then !important ones
// in reverse origin order with the style attribute above the author rules
int tierOf(RuleKind kind, StyleOrigin origin, bool important) {
    if (kind == RuleKind::Hints) return 2;
    if (kind == RuleKind::Inline) return important ? 6 : 4;
    switch (origin) {
        case StyleOrigin::UserAgent: return important ? 8 : 0;
        case StyleOrigin::User: return important ? 7 : 1;
        default: return important ? 5 : 3;
    }
}

/// A declaration of one element's blocks with its place in the cascade
struct RankedDeclaration {
    int block = 0;
    int declaration = 0;
    std::tuple<int, size_t, size_t> rank;   // tier, rule index in cascade order, index in the block
    std::vector<std::string> longhands;
};

struct Level {
    std::vector<RuleBlock> blocks;
    std::vector<RankedDeclaration> ranked;
};

/// Where every style rule of the engine's sheets is: its source label
class RuleLocator {
public:
    explicit RuleLocator(CSSEngine& css) : css_(css) {}
    std::string sourceOf(const CSSStyleRule* rule) {
        if (!built_) build();
        auto it = labels_.find(rule);
        return it != labels_.end() ? it->second : std::string();
    }
private:
    void build() {
        built_ = true;
        int styleElementIndex = 0;
        for (const auto& entry : css_.styleSheets()) {
            const CSSStyleSheet* sheet = entry.sheet.get();
            if (!sheet || !sheet->cssRules()) continue;
            std::string name;
            if (entry.origin == StyleOrigin::UserAgent) {
                name = "user agent";
            } else if (!sheet->href().empty()) {
                name = sheet->href();
                size_t q = name.find_first_of("?#");
                if (q != std::string::npos) name.erase(q);
                size_t slash = name.find_last_of("/\\");
                if (slash != std::string::npos && slash + 1 < name.size()) name = name.substr(slash + 1);
            } else {
                name = "<style> " + std::to_string(++styleElementIndex);
            }
            CSSRuleList* rules = sheet->cssRules();
            for (size_t i = 0; i < rules->length(); i++) {
                if (auto* styleRule = dynamic_cast<const CSSStyleRule*>(rules->item(i))) {
                    labels_[styleRule] = name + ", rule " + std::to_string(i + 1);
                }
            }
        }
    }
    CSSEngine& css_;
    bool built_ = false;
    std::unordered_map<const CSSStyleRule*, std::string> labels_;
};

bool isDisabled(const StyleEdit* edit, const CSSStyleRule* rule, const std::string& property) {
    if (!edit) return false;
    DisabledDeclaration d{rule, property};
    return std::find(edit->disabled.begin(), edit->disabled.end(), d) != edit->disabled.end();
}

/// The blocks of one element in display order with their declarations ranked.
/// `inheritedOnly` keeps only declarations that set an inherited property.
Level collectLevel(CSSEngine& css, DOMElement* element, const StyleEdit* edit, bool inheritedOnly,
                   RuleLocator& locator) {
    Level level;
    auto addBlock = [&](RuleBlock block, const CSSStyleDeclaration* decl, size_t ruleRank) {
        int blockIndex = (int)level.blocks.size();
        const CSSStyleRule* key = block.kind == RuleKind::Style ? block.rule : nullptr;
        for (size_t i = 0; decl && i < decl->length(); i++) {
            RuleDeclaration d;
            d.property = decl->item(i);
            d.value = decl->getPropertyValue(d.property);
            d.important = !decl->getPropertyPriority(d.property).empty();
            if (d.value.empty()) continue;
            if (inheritedOnly && !setsInheritedProperty(d.property)) continue;
            d.disabled = block.kind != RuleKind::Hints && isDisabled(edit, key, d.property);
            RankedDeclaration r;
            r.block = blockIndex;
            r.declaration = (int)block.declarations.size();
            r.rank = {tierOf(block.kind, block.origin, d.important), ruleRank, i};
            for (auto& l : longhandsOf(d.property)) {
                if (!inheritedOnly || isInheritedProperty(l)) r.longhands.push_back(std::move(l));
            }
            block.declarations.push_back(std::move(d));
            level.ranked.push_back(std::move(r));
        }
        // The element's own `element { }` and rules show even when empty, as in Firefox
        bool keep = !block.declarations.empty() || (!inheritedOnly && block.kind != RuleKind::Hints);
        if (keep) {
            level.blocks.push_back(std::move(block));
        } else {
            // Nothing of this block is shown: drop its ranked entries
            level.ranked.erase(std::remove_if(level.ranked.begin(), level.ranked.end(),
                                              [&](const RankedDeclaration& r) { return r.block == blockIndex; }),
                               level.ranked.end());
        }
    };

    CSSParser parser;
    std::string inlineText = edit ? edit->originalStyle : element->getAttribute("style");
    auto inlineDecl = parser.parseInlineStyle(inlineText);
    RuleBlock inlineBlock;
    inlineBlock.kind = RuleKind::Inline;
    inlineBlock.element = element;
    inlineBlock.selector = "element";
    addBlock(std::move(inlineBlock), inlineDecl.get(), 0);

    std::vector<MatchedRule> matched = css.matchedRules(element);
    for (size_t i = matched.size(); i-- > 0;) {
        const MatchedRule& m = matched[i];
        if (!m.rule) continue;
        RuleBlock block;
        block.kind = RuleKind::Style;
        block.rule = m.rule;
        block.element = element;
        block.selector = m.rule->selectorText();
        block.origin = m.origin;
        block.source = locator.sourceOf(m.rule);
        addBlock(std::move(block), m.rule->style(), i);
    }

    std::string hints = presentationalHintStyle(element);
    if (!hints.empty()) {
        CSSParser hintParser;
        auto hintDecl = hintParser.parseInlineStyle(hints);
        RuleBlock block;
        block.kind = RuleKind::Hints;
        block.element = element;
        block.selector = "attributes";
        addBlock(std::move(block), hintDecl.get(), 0);
    }
    return level;
}

/// The winning declaration (index into level.ranked) of every longhand, skipping switched-off ones
std::unordered_map<std::string, size_t> levelWinners(const Level& level) {
    std::unordered_map<std::string, size_t> winners;
    for (size_t i = 0; i < level.ranked.size(); i++) {
        const RankedDeclaration& r = level.ranked[i];
        if (level.blocks[r.block].declarations[r.declaration].disabled) continue;
        for (const auto& l : r.longhands) {
            auto it = winners.find(l);
            if (it == winners.end() || level.ranked[it->second].rank < r.rank) winners[l] = i;
        }
    }
    return winners;
}

} // namespace

std::vector<std::string> longhandsOf(const std::string& property) {
    auto sides = [](const std::string& prefix, const std::string& suffix) {
        std::vector<std::string> out;
        for (const auto& s : kSides) out.push_back(prefix + s + suffix);
        return out;
    };
    if (property == "margin") return sides("margin-", "");
    if (property == "padding") return sides("padding-", "");
    if (property == "inset") return {"top", "right", "bottom", "left"};
    if (property == "border-width") return sides("border-", "-width");
    if (property == "border-style") return sides("border-", "-style");
    if (property == "border-color") return sides("border-", "-color");
    if (property == "border") {
        std::vector<std::string> out;
        for (const char* part : {"-width", "-style", "-color"}) {
            for (auto& l : sides("border-", part)) out.push_back(std::move(l));
        }
        return out;
    }
    for (const auto& s : kSides) {
        if (property == "border-" + s) return {property + "-width", property + "-style", property + "-color"};
    }
    if (property == "border-radius") {
        return {"border-top-left-radius", "border-top-right-radius", "border-bottom-right-radius",
                "border-bottom-left-radius"};
    }
    if (property == "background") {
        return {"background-color", "background-image", "background-repeat", "background-position",
                "background-size", "background-attachment"};
    }
    if (property == "font") {
        return {"font-style", "font-variant", "font-weight", "font-size", "line-height", "font-family"};
    }
    if (property == "list-style") return {"list-style-type", "list-style-position", "list-style-image"};
    if (property == "overflow") return {"overflow-x", "overflow-y"};
    if (property == "flex") return {"flex-grow", "flex-shrink", "flex-basis"};
    if (property == "flex-flow") return {"flex-direction", "flex-wrap"};
    if (property == "gap" || property == "grid-gap") return {"row-gap", "column-gap"};
    if (property == "outline") return {"outline-width", "outline-style", "outline-color"};
    if (property == "text-decoration") {
        return {"text-decoration-line", "text-decoration-style", "text-decoration-color"};
    }
    return {property};
}

DeclarationRef ElementRules::winner(const std::string& property) const {
    auto longhands = longhandsOf(property);
    auto it = winners.find(longhands.front());
    return it != winners.end() ? it->second : DeclarationRef{};
}

const RuleBlock* ElementRules::block(const DeclarationRef& ref) const {
    if (!ref.valid()) return nullptr;
    const std::vector<RuleBlock>* list = &blocks;
    if (ref.group >= 0) {
        if (ref.group >= (int)inherited.size()) return nullptr;
        list = &inherited[ref.group].blocks;
    }
    return ref.block < (int)list->size() ? &(*list)[ref.block] : nullptr;
}

const RuleDeclaration* ElementRules::declaration(const DeclarationRef& ref) const {
    const RuleBlock* b = block(ref);
    if (!b || ref.declaration >= (int)b->declarations.size()) return nullptr;
    return &b->declarations[ref.declaration];
}

ElementRules collectElementRules(CSSEngine& css, DOMElement* element, const StyleEditLookup& edits) {
    ElementRules out;
    if (!element) return out;
    RuleLocator locator(css);
    auto editOf = [&](const DOMElement* e) { return edits ? edits(e) : nullptr; };

    // The element's own blocks: every property
    Level own = collectLevel(css, element, editOf(element), false, locator);
    auto ownWinners = levelWinners(own);
    for (const auto& [longhand, idx] : ownWinners) {
        const RankedDeclaration& r = own.ranked[idx];
        out.winners[longhand] = DeclarationRef{-1, r.block, r.declaration};
    }

    // Ancestors: only inherited properties, and only those no nearer element sets
    std::vector<Level> levels;
    for (DOMNode* n = element->parentNode(); n; n = n->parentNode()) {
        if (n->nodeType() != NodeType::Element) continue;
        auto* ancestor = static_cast<DOMElement*>(n);
        Level level = collectLevel(css, ancestor, editOf(ancestor), true, locator);
        if (level.blocks.empty()) continue;
        int group = (int)out.inherited.size();
        for (const auto& [longhand, idx] : levelWinners(level)) {
            if (out.winners.count(longhand)) continue;
            const RankedDeclaration& r = level.ranked[idx];
            out.winners[longhand] = DeclarationRef{group, r.block, r.declaration};
        }
        out.inherited.push_back({ancestor, level.blocks});
        levels.push_back(std::move(level));
    }
    out.blocks = std::move(own.blocks);

    // A declaration is overridden when it sets none of its properties
    auto markOverridden = [&](int group, const Level& level, std::vector<RuleBlock>& blocks) {
        for (const auto& r : level.ranked) {
            RuleDeclaration& d = blocks[r.block].declarations[r.declaration];
            if (d.disabled) continue;
            DeclarationRef self{group, r.block, r.declaration};
            bool wins = false;
            for (const auto& l : r.longhands) {
                auto it = out.winners.find(l);
                if (it != out.winners.end() && it->second == self) { wins = true; break; }
            }
            d.overridden = !wins;
        }
    };
    markOverridden(-1, own, out.blocks);
    for (size_t g = 0; g < levels.size(); g++) markOverridden((int)g, levels[g], out.inherited[g].blocks);
    return out;
}

std::string styleAttributeFor(CSSEngine& css, DOMElement* element, const StyleEdit& edit) {
    RuleLocator locator(css);
    Level level = collectLevel(css, element, &edit, false, locator);
    auto decl = [&](const RankedDeclaration& r) -> const RuleDeclaration& {
        return level.blocks[r.block].declarations[r.declaration];
    };
    auto kind = [&](const RankedDeclaration& r) { return level.blocks[r.block].kind; };

    // The properties a switched-off rule declaration set, closed over the
    // declarations left for them (a shorthand brings in its other longhands,
    // so the overrides cannot beat a rule that set one of those)
    std::unordered_set<std::string> affected;
    for (const auto& r : level.ranked) {
        if (decl(r).disabled && kind(r) == RuleKind::Style) affected.insert(r.longhands.begin(), r.longhands.end());
    }
    std::vector<const RankedDeclaration*> chain;
    if (!affected.empty()) {
        bool grew = true;
        while (grew) {
            grew = false;
            for (const auto& r : level.ranked) {
                if (decl(r).disabled) continue;
                bool touches = std::any_of(r.longhands.begin(), r.longhands.end(),
                                           [&](const std::string& l) { return affected.count(l) > 0; });
                if (!touches) continue;
                for (const auto& l : r.longhands) grew |= affected.insert(l).second;
            }
        }
        for (const auto& r : level.ranked) {
            if (decl(r).disabled) continue;
            if (std::any_of(r.longhands.begin(), r.longhands.end(),
                            [&](const std::string& l) { return affected.count(l) > 0; })) {
                chain.push_back(&r);
            }
        }
        std::stable_sort(chain.begin(), chain.end(),
                         [](const RankedDeclaration* a, const RankedDeclaration* b) { return a->rank < b->rank; });
    }

    // Written in cascade order; a property written twice keeps only its last
    // place, since a style attribute applies a property at its first position
    std::vector<std::pair<std::string, std::string>> out;
    auto put = [&](const std::string& property, const std::string& value) {
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [&](const auto& p) { return p.first == property; }),
                  out.end());
        out.push_back({property, value});
    };
    std::vector<std::string> sortedAffected(affected.begin(), affected.end());
    std::sort(sortedAffected.begin(), sortedAffected.end());
    for (const auto& l : sortedAffected) {
        put(l, (isInheritedProperty(l) ? std::string("inherit") : initialValueOf(l)) + " !important");
    }
    for (const RankedDeclaration* r : chain) put(decl(*r).property, decl(*r).value + " !important");
    for (const auto& r : level.ranked) {
        const RuleDeclaration& d = decl(r);
        if (kind(r) != RuleKind::Inline || d.disabled) continue;
        if (std::find(chain.begin(), chain.end(), &r) != chain.end()) continue;
        put(d.property, d.value + (d.important ? " !important" : ""));
    }

    std::string style;
    for (const auto& [property, value] : out) {
        if (!style.empty()) style += " ";
        style += property + ": " + value + ";";
    }
    return style;
}

} // namespace ZepraBrowser::DevTools
