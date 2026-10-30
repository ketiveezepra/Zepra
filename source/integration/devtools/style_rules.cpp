// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file style_rules.cpp
 * @brief The Rules pane's model: matched rules, cascade winners, inherited
 *        rules, and the per-element edits of their declarations.
 */
#include "style_rules.hpp"

#include <algorithm>
#include <cctype>
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

/// The declarations of a parsed block, each at its place in the block
std::vector<EditedDeclaration> declarationsOf(const CSSStyleDeclaration* decl) {
    std::vector<EditedDeclaration> out;
    for (size_t i = 0; decl && i < decl->length(); i++) {
        EditedDeclaration d;
        d.property = decl->item(i);
        d.value = decl->getPropertyValue(d.property);
        d.important = !decl->getPropertyPriority(d.property).empty();
        d.position = i;
        if (!d.value.empty()) out.push_back(std::move(d));
    }
    return out;
}

/// A block's declarations as the page has them: the rule's, or the style attribute's
std::vector<EditedDeclaration> pageDeclarations(const std::string& styleAttribute, const CSSStyleRule* rule) {
    if (rule) return declarationsOf(rule->style());
    CSSParser parser;
    auto decl = parser.parseInlineStyle(styleAttribute);
    return declarationsOf(decl.get());
}

bool sameDeclaration(const EditedDeclaration& a, const EditedDeclaration& b) {
    return a.property == b.property && a.value == b.value && a.important == b.important &&
           a.position == b.position && a.added == b.added;
}

/// Whether the page has this declaration, unchanged, at its place
bool onPage(const EditedDeclaration& d, const std::vector<EditedDeclaration>& page) {
    if (d.added) return false;
    return std::any_of(page.begin(), page.end(), [&](const EditedDeclaration& p) { return sameDeclaration(p, d); });
}

const BlockEdit* blockEditOf(const StyleEdit* edit, const CSSStyleRule* rule) {
    if (!edit) return nullptr;
    for (const auto& b : edit->blocks) {
        if (b.rule == rule) return &b;
    }
    return nullptr;
}

/// A CSS property name: an identifier, or a custom property (--name)
bool isPropertyName(const std::string& s) {
    auto nameChar = [](char c) { return std::isalnum((unsigned char)c) || c == '-' || c == '_'; };
    size_t i = 0;
    if (s.size() > 2 && s[0] == '-' && s[1] == '-') {
        i = 2;
    } else {
        if (i < s.size() && s[i] == '-') i++;
        if (i >= s.size() || !(std::isalpha((unsigned char)s[i]) || s[i] == '_')) return false;
    }
    for (; i < s.size(); i++) {
        if (!nameChar(s[i])) return false;
    }
    return true;
}

/// Whether the engine reads the value back as it is when written into a style
/// attribute: it ends a value at a `;` or `}` outside parentheses, and takes
/// "!important" from anywhere in it
bool isWritableValue(const std::string& v) {
    int depth = 0;
    for (char c : v) {
        if (c == '(') depth++;
        else if (c == ')' && --depth < 0) return false;
        else if ((c == ';' || c == '}') && depth == 0) return false;
    }
    return depth == 0 && !v.empty() && v.find("!important") == std::string::npos;
}

/// The longhands a declaration sets: none when it cannot apply (an invalid name, or a value the engine would misread)
std::vector<std::string> appliedLonghands(const std::string& property, const std::string& value) {
    if (!isPropertyName(property) || !isWritableValue(value)) return {};
    return longhandsOf(property);
}

std::string trimmed(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) b++;
    while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}

/// A property name as typed: trimmed and lowercase (a custom property keeps its case)
std::string typedProperty(const std::string& typed) {
    std::string name = trimmed(typed);
    if (name.rfind("--", 0) != 0) {
        for (char& c : name) c = (char)std::tolower((unsigned char)c);
    }
    return name;
}

/// A value as typed: trailing `;` dropped, and a trailing "!important" (any case,
/// "! important" too) taken off into the importance
EditedDeclaration typedValue(const std::string& typed) {
    std::string v = trimmed(typed);
    while (!v.empty() && (v.back() == ';' || std::isspace((unsigned char)v.back()))) v.pop_back();
    EditedDeclaration d;
    size_t bang = v.rfind('!');
    if (bang != std::string::npos) {
        std::string tail = trimmed(v.substr(bang + 1));
        for (char& c : tail) c = (char)std::tolower((unsigned char)c);
        if (tail == "important") {
            d.important = true;
            v = trimmed(v.substr(0, bang));
        }
    }
    d.value = v;
    return d;
}

/// The block's edit, started from the page's declarations when there is none yet
BlockEdit& blockEditFor(StyleEdit& edit, const CSSStyleRule* rule) {
    for (auto& b : edit.blocks) {
        if (b.rule == rule) return b;
    }
    edit.blocks.push_back({rule, pageDeclarations(edit.originalStyle, rule)});
    return edit.blocks.back();
}

/// Drops the block's edit when its declarations are the page's again
void settle(StyleEdit& edit, const CSSStyleRule* rule) {
    auto it = std::find_if(edit.blocks.begin(), edit.blocks.end(), [&](const BlockEdit& b) { return b.rule == rule; });
    if (it == edit.blocks.end()) return;
    auto page = pageDeclarations(edit.originalStyle, rule);
    if (page.size() == it->declarations.size() &&
        std::equal(page.begin(), page.end(), it->declarations.begin(), sameDeclaration)) {
        edit.blocks.erase(it);
    }
}

int indexOf(const BlockEdit& block, const std::string& property) {
    for (size_t i = 0; i < block.declarations.size(); i++) {
        if (block.declarations[i].property == property) return (int)i;
    }
    return -1;
}

void forgetDisabled(StyleEdit& edit, const CSSStyleRule* rule, const std::string& property) {
    DisabledDeclaration d{rule, property};
    edit.disabled.erase(std::remove(edit.disabled.begin(), edit.disabled.end(), d), edit.disabled.end());
}

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
    // `page` is what the page has; the edit's declarations replace them
    auto addBlock = [&](RuleBlock block, const std::vector<EditedDeclaration>& page, size_t ruleRank) {
        int blockIndex = (int)level.blocks.size();
        const CSSStyleRule* key = block.kind == RuleKind::Style ? block.rule : nullptr;
        const BlockEdit* changed = block.kind != RuleKind::Hints ? blockEditOf(edit, key) : nullptr;
        for (const EditedDeclaration& e : changed ? changed->declarations : page) {
            RuleDeclaration d;
            d.property = e.property;
            d.value = e.value;
            d.important = e.important;
            if (inheritedOnly && !setsInheritedProperty(d.property)) continue;
            d.disabled = block.kind != RuleKind::Hints && isDisabled(edit, key, d.property);
            d.added = e.added;
            d.edited = changed && !e.added && !onPage(e, page);
            RankedDeclaration r;
            r.block = blockIndex;
            r.declaration = (int)block.declarations.size();
            r.rank = {tierOf(block.kind, block.origin, d.important), ruleRank, e.position};
            for (auto& l : appliedLonghands(d.property, d.value)) {
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

    std::string inlineText = edit ? edit->originalStyle : element->getAttribute("style");
    RuleBlock inlineBlock;
    inlineBlock.kind = RuleKind::Inline;
    inlineBlock.element = element;
    inlineBlock.selector = "element";
    addBlock(std::move(inlineBlock), pageDeclarations(inlineText, nullptr), 0);

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
        addBlock(std::move(block), declarationsOf(m.rule->style()), i);
    }

    std::string hints = presentationalHintStyle(element);
    if (!hints.empty()) {
        CSSParser hintParser;
        auto hintDecl = hintParser.parseInlineStyle(hints);
        RuleBlock block;
        block.kind = RuleKind::Hints;
        block.element = element;
        block.selector = "attributes";
        addBlock(std::move(block), declarationsOf(hintDecl.get()), 0);
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

bool setDeclarationValue(StyleEdit& edit, const CSSStyleRule* rule, const std::string& property,
                         const std::string& value) {
    EditedDeclaration typed = typedValue(value);
    if (typed.value.empty()) return removeDeclaration(edit, rule, property);
    BlockEdit& block = blockEditFor(edit, rule);
    int i = indexOf(block, property);
    if (i >= 0) {
        block.declarations[i].value = typed.value;
        block.declarations[i].important = typed.important;
    }
    settle(edit, rule);
    return i >= 0;
}

bool renameDeclaration(StyleEdit& edit, const CSSStyleRule* rule, const std::string& property,
                       const std::string& newProperty) {
    std::string name = typedProperty(newProperty);
    if (name.empty()) return removeDeclaration(edit, rule, property);
    BlockEdit& block = blockEditFor(edit, rule);
    int i = indexOf(block, property);
    if (i >= 0 && name != property) {
        // One declaration per property and block: the renamed one takes the other's place in the cascade
        int other = indexOf(block, name);
        if (other >= 0) {
            block.declarations.erase(block.declarations.begin() + other);
            forgetDisabled(edit, rule, name);
            if (other < i) i--;
        }
        block.declarations[i].property = name;
        for (auto& d : edit.disabled) {
            if (d.rule == rule && d.property == property) d.property = name;
        }
    }
    settle(edit, rule);
    return i >= 0;
}

bool addDeclaration(StyleEdit& edit, const CSSStyleRule* rule, const std::string& property,
                    const std::string& value) {
    std::string name = typedProperty(property);
    EditedDeclaration d = typedValue(value);
    if (name.empty() || d.value.empty()) return false;
    BlockEdit& block = blockEditFor(edit, rule);
    int other = indexOf(block, name);
    if (other >= 0) {
        block.declarations.erase(block.declarations.begin() + other);
        forgetDisabled(edit, rule, name);
    }
    // After every declaration of the block, the page's included
    size_t next = 0;
    for (const auto& e : block.declarations) next = std::max(next, e.position + 1);
    for (const auto& e : pageDeclarations(edit.originalStyle, rule)) next = std::max(next, e.position + 1);
    d.property = name;
    d.position = next;
    d.added = true;
    block.declarations.push_back(std::move(d));
    return true;
}

bool removeDeclaration(StyleEdit& edit, const CSSStyleRule* rule, const std::string& property) {
    BlockEdit& block = blockEditFor(edit, rule);
    int i = indexOf(block, property);
    if (i >= 0) {
        block.declarations.erase(block.declarations.begin() + i);
        forgetDisabled(edit, rule, property);
    }
    settle(edit, rule);
    return i >= 0;
}

void revertStyleEdit(StyleEdit& edit) {
    edit.disabled.clear();
    edit.blocks.clear();
}

std::string styleAttributeFor(CSSEngine& css, DOMElement* element, const StyleEdit& edit) {
    // Nothing changed: the attribute as the page had it, byte for byte
    if (edit.empty()) return edit.originalStyle;
    RuleLocator locator(css);
    Level level = collectLevel(css, element, &edit, false, locator);
    auto decl = [&](const RankedDeclaration& r) -> const RuleDeclaration& {
        return level.blocks[r.block].declarations[r.declaration];
    };
    auto kind = [&](const RankedDeclaration& r) { return level.blocks[r.block].kind; };

    // The properties a switched-off, edited, added or deleted rule declaration
    // sets, closed over the declarations left for them (a shorthand brings in
    // its other longhands, so the overrides cannot beat a rule that set one of
    // those)
    std::unordered_set<std::string> affected;
    for (const auto& r : level.ranked) {
        const RuleDeclaration& d = decl(r);
        if (kind(r) == RuleKind::Style && (d.disabled || d.edited || d.added)) {
            affected.insert(r.longhands.begin(), r.longhands.end());
        }
    }
    // What the shared rule still sets where this element's edit changed or deleted the declaration
    for (const auto& block : level.blocks) {
        const BlockEdit* changed = block.kind == RuleKind::Style ? blockEditOf(&edit, block.rule) : nullptr;
        if (!changed) continue;
        auto page = declarationsOf(block.rule->style());
        for (const auto& p : page) {
            if (onPage(p, changed->declarations)) continue;
            for (auto& l : appliedLonghands(p.property, p.value)) affected.insert(std::move(l));
        }
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
        if (kind(r) != RuleKind::Inline || d.disabled || r.longhands.empty()) continue;
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
