// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file css_engine.hpp
 * @brief CSS cascade, style resolution, and engine
 *
 * @see https://developer.mozilla.org/en-US/docs/Web/CSS/Cascade
 */

#pragma once

#include <functional>
#include "css/css_style_sheet.hpp"
#include "css/css_style_declaration.hpp"
#include "css/css_rule.hpp"
#include "css/css_selector.hpp"
#include "css/css_computed_style.hpp"
#include <cstdint>
#include <memory>
#include <vector>
#include <unordered_map>

namespace Zepra::WebCore {

// Forward declarations
class DOMDocument;
class DOMElement;
class DOMNode;
class DOMShadowRoot;

/**
 * @brief Style origin (cascade layer)
 */
enum class StyleOrigin {
    UserAgent,      // Browser default styles
    User,           // User preferences
    Author,         // Page stylesheets
    AuthorImportant,
    UserImportant,
    UserAgentImportant,
    Animation,
    Transition,
};

/**
 * @brief Pseudo-classes DevTools can force on an element (like the ":hov"
 *        panel), as bits of the mask forcedPseudoClasses() returns.
 */
enum class ForcedPseudo : unsigned {
    Hover  = 1u << 0,   // the element and its ancestors match :hover
    Active = 1u << 1,   // the element and its ancestors match :active
    Focus  = 1u << 2,   // the element matches :focus / :focus-visible, it and its ancestors :focus-within
};

/**
 * @brief Live user-interaction state consulted when matching :hover, :active,
 *        :focus and :focus-within. The browser updates it and restyles.
 */
struct InteractionState {
    DOMElement* hovered = nullptr;   // deepest element under the pointer
    DOMElement* active = nullptr;    // element under a pressed primary button
    DOMElement* focused = nullptr;   // element with keyboard focus

    /// States DevTools forces on an element, matched in addition to the real
    /// ones above. `element` is only compared by address until the matcher
    /// has found it in the tree it is matching (by walking down from the
    /// root), so an entry whose element was destroyed matches nothing.
    struct ForcedEntry {
        DOMElement* element = nullptr;
        unsigned states = 0;                   // ForcedPseudo bits
        // Last check that `element` is in the tree under `checkedRoot`, valid
        // while domMutationVersion() is still `checkedVersion`
        mutable const DOMNode* checkedRoot = nullptr;
        mutable uint64_t checkedVersion = 0;
        mutable bool inTree = false;
    };
    std::vector<ForcedEntry> forced;   // cleared with the rest by `interactionState() = {}`
};

/// Interaction state of the current document (single document per process)
InteractionState& interactionState();

/// Forces (on = true) or stops forcing one pseudo-class on an element. When
/// that changes anything it bumps domMutationVersion(), so the browser
/// restyles and lays out again on its next frame; returns whether it changed.
bool setForcedPseudoClass(DOMElement* element, ForcedPseudo which, bool on);
/// ForcedPseudo bits forced on exactly this element (0 when none)
unsigned forcedPseudoClasses(const DOMElement* element);
/// Stops forcing every pseudo-class on every element; bumps domMutationVersion()
/// and returns true when something was forced
bool clearForcedPseudoClasses();

/**
 * @brief Matched rule with cascade info
 */
struct MatchedRule {
    const CSSStyleRule* rule;
    StyleOrigin origin;
    Selector::Specificity specificity;
    size_t order;   // Source order
    /// Cascade layer order within the origin: layers in declaration order,
    /// unlayered rules last (kUnlayered), so they win over every layer
    static constexpr unsigned kUnlayered = ~0u;
    unsigned layer = kUnlayered;
    
    /// Compare for cascade ordering
    bool operator<(const MatchedRule& other) const;
};

/**
 * @brief CSS cascade engine
 */
class CSSCascade {
public:
    /// Collect all matching rules for element
    std::vector<MatchedRule> collectMatchingRules(
        DOMElement* element,
        const std::vector<CSSStyleSheet*>& stylesheets,
        StyleOrigin origin = StyleOrigin::Author
    );
    
    /// Collect matching rules with per-sheet origins (correct cascade)
    std::vector<MatchedRule> collectMatchingRulesWithOrigins(
        DOMElement* element,
        const std::vector<std::pair<CSSStyleSheet*, StyleOrigin>>& sheets
    );

    /// Sort by cascade order
    void sortByCascade(std::vector<MatchedRule>& rules);

    /// Compute cascaded value for property
    std::optional<std::string> cascadedValue(
        const std::string& property,
        const std::vector<MatchedRule>& rules
    );
    
    /// Check if selector matches element
    bool selectorMatches(DOMElement* element, const std::string& selector);
    
    /// Match a single selector group (handles combinators)
    bool selectorGroupMatches(DOMElement* element, const std::string& selector);
    
    /// Match compound selector (tag.class#id[attr]) against single element
    bool compoundSelectorMatches(DOMElement* element, const std::string& compound);

    /// Matching rules of a shadow tree's style sheets: :host matches its
    /// host, combinators climb from the tree's top to the host. Null (the
    /// default) for the document's sheets, where :host matches nothing.
    void setShadowScope(const DOMShadowRoot* scope) { shadowScope_ = scope; }
    
    /// Calculate specificity of a selector
    Selector::Specificity calculateSpecificity(const std::string& selector);

    /// The specificity a selector list has for an element: that of its most
    /// specific group that matches the element (Selectors 4 §17)
    Selector::Specificity matchingSpecificity(DOMElement* element, const std::string& selectorList);

    /// The order of cascade layers named by the sheets (in sheet order): each
    /// dotted path's rank, a layer's sublayers before its own rules
    /// (CSS Cascade 5 §6.4.3)
    static std::unordered_map<std::string, unsigned> layerRanks(
        const std::vector<std::pair<CSSStyleSheet*, StyleOrigin>>& sheets);

    /// The matched rules of one origin whose !important declarations apply,
    /// in the order to apply them (last wins): layer order reversed, so
    /// earlier layers win and unlayered rules lose (CSS Cascade 5 §6.4)
    static std::vector<const MatchedRule*> importantOrder(const std::vector<MatchedRule>& sorted,
                                                          StyleOrigin origin);

    /// Invalidate selector index (call when stylesheets change)
    void invalidateIndex() { indexed_ = false; }

    /// Which pseudo-element the matching is for ("before", "after", ...;
    /// empty = the element itself). With one set, a selector group matches
    /// only when it ends in that pseudo-element and the rest of the group
    /// matches the element; without one, groups ending in a pseudo-element
    /// never match.
    void setMatchPseudoElement(std::string name) { pseudoElement_ = std::move(name); }
    const std::string& matchPseudoElement() const { return pseudoElement_; }

    /// Splits a trailing pseudo-element off one selector group:
    /// "p.note::before" -> "p.note" and "before" (also the legacy one-colon
    /// :before/:after/:first-line/:first-letter). Returns "" when the group
    /// does not end in a pseudo-element; `rest` then holds the group as is.
    static std::string splitPseudoElement(const std::string& group, std::string& rest);

private:
    /// Per-rule entry for the selector index
    struct IndexedRule {
        const CSSStyleRule* rule;
        StyleOrigin origin;
        size_t order;
        unsigned layer = MatchedRule::kUnlayered;
    };
    
    /// Selector index — maps class/tag/id tokens to candidate rule indices
    struct SelectorIndex {
        std::unordered_map<std::string, std::vector<size_t>> byClass;
        std::unordered_map<std::string, std::vector<size_t>> byTag;
        std::unordered_map<std::string, std::vector<size_t>> byId;
        std::vector<size_t> universal; // complex selectors needing full match
    };

    /// Build the selector index from all stylesheets
    void buildIndex(const std::vector<std::pair<CSSStyleSheet*, StyleOrigin>>& sheets);
    
    /// Extract the rightmost key selector tokens for indexing
    void indexSelector(const std::string& selector, size_t ruleIdx);

    bool compoundMatches(DOMElement* element, const std::string& compound);

    bool indexed_ = false;
    const DOMShadowRoot* shadowScope_ = nullptr;
    bool inHostArgument_ = false;
    std::string pseudoElement_;
    SelectorIndex index_;
    std::vector<IndexedRule> indexedRules_;
};

/// Whether a property inherits by default (color, font-*, line-height, text-align, visibility...)
bool isInheritedProperty(const std::string& property);

/**
 * @brief Style resolver - computes final styles
 */
class StyleResolver {
public:
    StyleResolver();
    ~StyleResolver();

    /// Add stylesheet
    void addStyleSheet(std::shared_ptr<CSSStyleSheet> sheet, StyleOrigin origin = StyleOrigin::Author);


    /// The stylesheets in the order they were added, with their origins
    struct StyleSheetEntry {
        std::shared_ptr<CSSStyleSheet> sheet;
        StyleOrigin origin;
    };
    const std::vector<StyleSheetEntry>& styleSheets() const { return stylesheets_; }
    /// Replaces every sheet (a document's sheets changed: <style>/<link>
    /// added, removed or edited), keeping the parsed sheets given
    void setStyleSheets(std::vector<StyleSheetEntry> sheets);

    /// The style rules that match an element, sorted by the cascade (lowest priority first)
    std::vector<MatchedRule> matchedRules(DOMElement* element);

    /// Compute style for element
    CSSComputedStyle computeStyle(DOMElement* element, const CSSComputedStyle* parentStyle = nullptr);

    /// Get computed style (cached)
    const CSSComputedStyle* getComputedStyle(DOMElement* element);

    /// The computed style of one of an element's pseudo-elements ("before",
    /// "after", "marker", "first-line", "first-letter", "placeholder",
    /// "selection"), cached like getComputedStyle. It inherits from the
    /// element and takes the rules whose selectors end in that pseudo-element.
    /// ::before/::after without a `content` compute it as "none".
    /// nullptr for a name that is not one of these.
    const CSSComputedStyle* getPseudoElementStyle(DOMElement* element, const std::string& pseudo);

    /// Invalidate cached styles
    void invalidateElement(DOMElement* element);
    void invalidateAll();

    /// The style sheets of each shadow tree (its <style> elements), which
    /// replace the ones set before. A shadow tree's elements match only
    /// these (and the user-agent sheet); its host also matches their :host
    /// rules, below the document's own rules.
    void setShadowStyleSheets(std::vector<std::pair<const DOMShadowRoot*,
                                                    std::vector<std::shared_ptr<CSSStyleSheet>>>> scopes);
    bool hasShadowStyleSheets() const { return !shadowScopes_.empty(); }

private:
    /// A shadow tree's sheets and the cascade (selector index) over them;
    /// shared by every tree with the same sheets (instances of one component)
    struct ScopedCascade {
        std::vector<std::shared_ptr<CSSStyleSheet>> sheets;
        std::vector<std::pair<CSSStyleSheet*, StyleOrigin>> pairs;
        CSSCascade cascade;
    };
    std::unordered_map<const DOMShadowRoot*, std::shared_ptr<ScopedCascade>> shadowScopes_;
    /// The rules matching an element (or its pseudo-element), unsorted:
    /// the document's cascade, then its shadow tree's or host's
    std::vector<MatchedRule> collectRules(DOMElement* element, const std::string& pseudo);

    /// Which declarations of a block applyDeclarations applies
    enum class Importance { Any, Normal, Important };
    /// computeStyle() runs the cascade twice: custom properties first (var()
    /// in ordinary properties needs their final values), then the rest
    enum class Pass { All, CustomOnly, SkipCustom };
    Pass pass_ = Pass::All;
    CustomPropertyMap* declaredCustom_ = nullptr;  // the custom pass's winners
    RegisteredPropertyMap registered_;             // @property rules of every sheet

    std::vector<StyleSheetEntry> stylesheets_;
    std::unordered_map<DOMElement*, CSSComputedStyle> styleCache_;
    std::unordered_map<DOMElement*, std::unordered_map<std::string, CSSComputedStyle>> pseudoStyleCache_;
    CSSCascade cascade_;
    SelectorMatcher matcher_;
    CSSLengthContext lengthContext_;               // for math functions, set per applyProperty

    void applyDeclarations(CSSComputedStyle& style, const CSSStyleDeclaration* decl,
                           const CSSComputedStyle* parentStyle, Importance importance = Importance::Any);
    void applyProperty(CSSComputedStyle& style, const std::string& property,
                       const std::string& value, const CSSComputedStyle* parentStyle);
    CSSLength parseLength(const std::string& value);
    CSSColor parseColor(const std::string& value);
};

/**
 * @brief CSS parser (enhanced)
 */
class CSSParser {
public:
    /// Parse stylesheet
    std::unique_ptr<CSSStyleSheet> parse(const std::string& css);

    /// Parse inline style
    std::unique_ptr<CSSStyleDeclaration> parseInlineStyle(const std::string& style);

    /// Parse single selector
    Selector parseSelector(const std::string& selector);

    /// Parse media query
    std::unique_ptr<MediaList> parseMediaQuery(const std::string& query);

private:
    size_t pos_ = 0;
    std::string input_;
    /// Query lists of the `@media` blocks being parsed, outermost first
    std::vector<std::shared_ptr<const MediaQueryList>> mediaStack_;
    /// A rule other than @import was seen (later @import rules are ignored)
    bool sawRule_ = false;
    /// Path of the @layer block being parsed ("" outside any layer) and the
    /// count of anonymous layers so far (each gets a name of its own)
    std::string layerPath_;
    int anonymousLayers_ = 0;

    void skipWhitespace();
    void skipComment();
    char peek() const;
    char consume();
    bool match(char c);
    bool eof() const;

    std::unique_ptr<CSSRule> parseRule();
    std::unique_ptr<CSSStyleRule> parseStyleRule();
    std::unique_ptr<CSSMediaRule> parseMediaRule();
    std::unique_ptr<CSSFontFaceRule> parseFontFaceRule();
    std::unique_ptr<CSSKeyframesRule> parseKeyframesRule();
    std::unique_ptr<CSSImportRule> parseImportRule();
    void parseRulesInto(CSSStyleSheet* sheet, int depth);
    void skipInvalidRule();
    void parsePropertyRule(CSSStyleSheet* sheet);

    std::string parseSelector_();
    std::string parseDeclarationBlock();
    void parseDeclarations(CSSStyleDeclaration* decl);
    std::string parsePropertyValue();
    std::string parseString();
    std::string parseIdentifier();
    std::string parseUrl();
};

/**
 * @brief CSS engine - main interface
 */
class CSSEngine {
public:
    CSSEngine();
    ~CSSEngine();

    /// Initialize with document
    void initialize(DOMDocument* document);

    /// Parse and add stylesheet; `href` is the URL of a <link> sheet (empty for <style>)
    void addStyleSheet(const std::string& css, StyleOrigin origin = StyleOrigin::Author,
                       const std::string& href = "");

    /// The stylesheets in cascade order, with their origins (sheet->href() is set for <link> sheets)
    const std::vector<StyleResolver::StyleSheetEntry>& styleSheets() const { return resolver_.styleSheets(); }

    /// A usable @font-face rule. Its src URLs resolve against baseUrl: the
    /// URL of the sheet it came from, or the document URL for a <style> sheet.
    struct WebFontFace {
        FontFaceDescriptors descriptors;
        std::string baseUrl;
        StyleOrigin origin;
    };
    /// Every valid @font-face of the loaded sheets whose media match now, in
    /// cascade order (a later face for the same family and style wins)
    std::vector<WebFontFace> fontFaces() const;

    /// The style rules that match an element, sorted by the cascade (lowest priority first)
    std::vector<MatchedRule> matchedRules(DOMElement* element) { return resolver_.matchedRules(element); }

    /// Loads an @import: resolves `href` against `baseUrl`, sets `url` to the
    /// sheet's final URL and `css` to its text. False when it can't be loaded.
    using ImportFetcher = std::function<bool(const std::string& href, const std::string& baseUrl,
                                             std::string& url, std::string& css)>;
    /// Without a fetcher, @import rules are ignored
    void setImportFetcher(ImportFetcher fetcher) { importFetcher_ = std::move(fetcher); }
    /// The document URL: @import in a <style> sheet resolves against it
    void setBaseUrl(const std::string& url) { baseUrl_ = url; }

    /// Add stylesheet from link
    void addStyleSheetFromUrl(const std::string& url, StyleOrigin origin = StyleOrigin::Author);

    /// Compute styles for document
    void computeStyles();

    /// Get computed style for element
    const CSSComputedStyle* getComputedStyle(DOMElement* element);

    /// Computed style of an element's pseudo-element (see StyleResolver)
    const CSSComputedStyle* getPseudoElementStyle(DOMElement* element, const std::string& pseudo) {
        return resolver_.getPseudoElementStyle(element, pseudo);
    }

    /// Invalidate styles
    void invalidate(DOMElement* element = nullptr);

    /// Parser access
    CSSParser& parser() { return parser_; }

    /// Style resolver
    StyleResolver& resolver() { return resolver_; }

    /// Gives every shadow tree in the document its own <style> sheets
    /// (call before computing styles; a page without shadow trees costs
    /// nothing). Sheets are parsed once per distinct text.
    void syncShadowStyles(DOMDocument* document);

    /// Whether any loaded stylesheet uses :hover / :active / :focus, i.e.
    /// whether a change of interaction state can change computed styles
    bool usesHoverSelectors() const { return usesHover_; }
    bool usesActiveSelectors() const { return usesActive_; }
    bool usesFocusSelectors() const { return usesFocus_; }

    /// Check @supports condition
    bool supports(const std::string& property, const std::string& value);
    bool supportsCondition(const std::string& condition);

    /// CSS namespace
    static std::string escape(const std::string& ident);
    static bool supportsPropertySyntax(const std::string& syntax);

private:
    DOMDocument* document_ = nullptr;
    CSSParser parser_;
    StyleResolver resolver_;
    std::unordered_map<std::string, std::shared_ptr<CSSStyleSheet>> shadowSheetCache_;
    std::vector<std::shared_ptr<CSSStyleSheet>> stylesheets_;
    bool usesHover_ = false;
    bool usesActive_ = false;
    bool usesFocus_ = false;
    ImportFetcher importFetcher_;
    std::string baseUrl_;

    /// Adds a sheet after the sheets its @import rules load. `media` are the
    /// query lists of the @import rules that led here (applied to every rule);
    /// `chain` holds the URLs being imported, to stop import cycles.
    void addSheetWithImports(const std::string& css, StyleOrigin origin, const std::string& href,
                             const std::vector<std::shared_ptr<const MediaQueryList>>& media,
                             std::vector<std::string>& chain);
};

} // namespace Zepra::WebCore
