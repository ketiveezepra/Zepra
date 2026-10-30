// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file style_rules.hpp
 * @brief What the Inspector's Rules pane shows for an element: its style
 *        attribute, the style rules that match it (from the engine's cascade),
 *        which declarations lose the cascade, and the inherited rules of its
 *        ancestors. Also builds the style attribute that switches declarations
 *        off for one element without touching the shared stylesheets.
 */
#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "css/css_engine.hpp"

namespace Zepra::WebCore { class DOMElement; }

namespace ZepraBrowser::DevTools {

/// One declaration of a rule block
struct RuleDeclaration {
    std::string property;
    std::string value;
    bool important = false;
    bool overridden = false;   ///< loses the cascade for every property it sets
    bool disabled = false;     ///< switched off in the panel
};

enum class RuleKind {
    Inline,   ///< the style attribute, shown as `element { }`
    Style,    ///< a matched style rule
    Hints,    ///< presentational attributes (bgcolor, width, align...): not drawn, but they can set a value
};

struct RuleBlock {
    RuleKind kind = RuleKind::Style;
    const Zepra::WebCore::CSSStyleRule* rule = nullptr;   ///< the rule (Style only)
    Zepra::WebCore::DOMElement* element = nullptr;        ///< the element the block applies to
    std::string selector;                                 ///< "element" for the style attribute
    Zepra::WebCore::StyleOrigin origin = Zepra::WebCore::StyleOrigin::Author;
    std::string source;          ///< "site.css, rule 4", "<style> 1, rule 2", "user agent, rule 31"
    std::vector<RuleDeclaration> declarations;
};

/// Rules of one ancestor that set inherited properties (only those declarations are kept)
struct InheritedRules {
    Zepra::WebCore::DOMElement* ancestor = nullptr;
    std::vector<RuleBlock> blocks;
};

/// A declaration in ElementRules: group -1 is the element's own blocks, otherwise inherited[group]
struct DeclarationRef {
    int group = -1;
    int block = -1;
    int declaration = -1;
    bool valid() const { return block >= 0 && declaration >= 0; }
    bool operator==(const DeclarationRef& o) const {
        return group == o.group && block == o.block && declaration == o.declaration;
    }
};

struct ElementRules {
    std::vector<RuleBlock> blocks;            ///< `element { }` first, then matched rules, most specific first
    std::vector<InheritedRules> inherited;    ///< nearest ancestor first
    std::unordered_map<std::string, DeclarationRef> winners;   ///< longhand property -> the declaration that sets it

    /// The declaration that sets a property (a shorthand answers for its first longhand)
    DeclarationRef winner(const std::string& property) const;
    const RuleBlock* block(const DeclarationRef& ref) const;
    const RuleDeclaration* declaration(const DeclarationRef& ref) const;
};

/// A declaration switched off in the panel: its rule (null for the style attribute) and property
struct DisabledDeclaration {
    const Zepra::WebCore::CSSStyleRule* rule = nullptr;
    std::string property;
    bool operator==(const DisabledDeclaration& o) const { return rule == o.rule && property == o.property; }
};

/// What the panel changed on one element
struct StyleEdit {
    std::string originalStyle;   ///< the style attribute before the first toggle
    bool hadStyle = false;       ///< whether the element had a style attribute then
    std::string writtenStyle;    ///< the attribute as the panel last wrote it
    std::vector<DisabledDeclaration> disabled;
};

/// The panel's edit for an element, or null
using StyleEditLookup = std::function<const StyleEdit*(const Zepra::WebCore::DOMElement*)>;

/// Everything the Rules pane shows for an element. The matched rules come from
/// the engine's cascade (CSSEngine::matchedRules); an element with an edit is
/// shown with its original style attribute and the edit's switched-off declarations.
ElementRules collectElementRules(Zepra::WebCore::CSSEngine& css, Zepra::WebCore::DOMElement* element,
                                 const StyleEditLookup& edits = {});

/// The style attribute that switches the edit's declarations off on this element
/// only: the original style attribute, plus, for each property a switched-off rule
/// declaration set, the declarations that are left for it in cascade order (as
/// !important, so they beat the shared rule).
std::string styleAttributeFor(Zepra::WebCore::CSSEngine& css, Zepra::WebCore::DOMElement* element,
                              const StyleEdit& edit);

/// The longhand properties a property sets ("margin" -> margin-top, -right, -bottom, -left)
std::vector<std::string> longhandsOf(const std::string& property);

} // namespace ZepraBrowser::DevTools
