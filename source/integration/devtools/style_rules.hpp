// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file style_rules.hpp
 * @brief What the Inspector's Rules pane shows for an element: its style
 *        attribute, the style rules that match it (from the engine's cascade),
 *        which declarations lose the cascade, and the inherited rules of its
 *        ancestors. Also builds the style attribute that switches declarations
 *        off, or edits, adds and deletes them, for one element without
 *        touching the shared stylesheets.
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
    bool edited = false;       ///< its property, value or importance was changed in the panel
    bool added = false;        ///< added in the panel
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

/// A declaration of a block the panel changed
struct EditedDeclaration {
    std::string property;
    std::string value;           ///< without "!important"
    bool important = false;
    size_t position = 0;         ///< its place in the block: the original's, or after the block's own when added
    bool added = false;
};

/// Every declaration a block has on one element after the panel changed it
struct BlockEdit {
    const Zepra::WebCore::CSSStyleRule* rule = nullptr;   ///< null for the style attribute
    std::vector<EditedDeclaration> declarations;          ///< in position order
};

/// What the panel changed on one element
struct StyleEdit {
    std::string originalStyle;   ///< the style attribute before the first change
    bool hadStyle = false;       ///< whether the element had a style attribute then
    std::string writtenStyle;    ///< the attribute as the panel last wrote it
    std::vector<DisabledDeclaration> disabled;
    std::vector<BlockEdit> blocks;   ///< blocks whose declarations differ from the page's

    /// Nothing is changed any more: the element should get its original style attribute back
    bool empty() const { return disabled.empty() && blocks.empty(); }
};

// Editing declarations for one element. `rule` is the matched rule the
// declaration is in, null for the style attribute (`element { }`); `property`
// is the declaration's property as the pane shows it. Edits to the style
// attribute are edits to the element's own inline style; edits to a rule apply
// to this element only, at the rule's place in the cascade. The shared
// stylesheets are never modified. Each returns false when the block has no
// such declaration (addDeclaration: when the name or value is empty). Write
// the result with styleAttributeFor() afterwards; an edit that is empty()
// again means the original attribute.

/// Changes a declaration's value. A trailing "!important" makes it important;
/// an empty value deletes the declaration. A switched-off declaration stays off.
bool setDeclarationValue(StyleEdit& edit, const Zepra::WebCore::CSSStyleRule* rule,
                         const std::string& property, const std::string& value);

/// Renames a declaration's property, keeping its place. An empty name deletes
/// it; a declaration of the new property in the same block is replaced.
bool renameDeclaration(StyleEdit& edit, const Zepra::WebCore::CSSStyleRule* rule,
                       const std::string& property, const std::string& newProperty);

/// Adds a declaration after the block's others ("red !important" sets
/// importance). One of the same property in the block is replaced.
bool addDeclaration(StyleEdit& edit, const Zepra::WebCore::CSSStyleRule* rule,
                    const std::string& property, const std::string& value);

/// Deletes a declaration
bool removeDeclaration(StyleEdit& edit, const Zepra::WebCore::CSSStyleRule* rule,
                       const std::string& property);

/// Forgets every change: the edit becomes empty()
void revertStyleEdit(StyleEdit& edit);

/// The panel's edit for an element, or null
using StyleEditLookup = std::function<const StyleEdit*(const Zepra::WebCore::DOMElement*)>;

/// Everything the Rules pane shows for an element. The matched rules come from
/// the engine's cascade (CSSEngine::matchedRules); an element with an edit is
/// shown with its original style attribute, the edit's changed declarations
/// (marked edited/added) and its switched-off ones.
ElementRules collectElementRules(Zepra::WebCore::CSSEngine& css, Zepra::WebCore::DOMElement* element,
                                 const StyleEditLookup& edits = {});

/// The style attribute that applies the edit on this element only: the
/// (edited) style attribute, plus, for each property a switched-off, edited,
/// added or deleted rule declaration sets, the declarations left for it in
/// cascade order (as !important, so they beat the shared rule). An empty()
/// edit gives back the original attribute exactly.
std::string styleAttributeFor(Zepra::WebCore::CSSEngine& css, Zepra::WebCore::DOMElement* element,
                              const StyleEdit& edit);

/// The longhand properties a property sets ("margin" -> margin-top, -right, -bottom, -left)
std::vector<std::string> longhandsOf(const std::string& property);

} // namespace ZepraBrowser::DevTools
