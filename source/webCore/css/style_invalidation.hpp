// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file style_invalidation.hpp
 * @brief Which cached styles a DOM or style sheet change can reach
 *
 * The engine keeps a computed style per element and used to drop all of
 * them on every change. Now it asks, per change, what the selectors in use
 * can see of it (the design of Blink's and WebKit's rule feature sets,
 * written for this engine's string selectors):
 *
 *  - SelectorFeatures: every class, id, attribute name and pseudo-class
 *    some selector mentions, by where it sits in the selector: in the
 *    subject (rightmost) compound, in a compound left of a descendant or
 *    child combinator, left of a sibling combinator, or inside :has().
 *    A class change on an element restyles nothing when no selector names
 *    that class, the element's subtree when one names it as subject or
 *    ancestor (styles inherit, so the subtree goes with it), the subtrees
 *    of its parent's children for a sibling position, everything for
 *    :has(). Insertions and removals restyle the inserted subtree, and the
 *    parent's children only when structural pseudo-classes or sibling
 *    combinators are in use.
 *  - SubjectKeys: for a style sheet added or removed, one key per selector
 *    that every element it matches must have (its id, a class, an
 *    attribute or its tag). Elements without any key keep their styles,
 *    and a whole subtree is skipped once its root is restyled.
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Zepra::WebCore {

class DOMElement;

/// The keys of the subjects of some selectors (see the file comment)
class SubjectKeys {
public:
    /// Adds a selector list's subjects; false when one has no key (`*`,
    /// a bare pseudo-class): any element may match it
    bool addSelectorList(const std::string& selectorList);
    /// Whether `element` has one of the keys (it may match a selector)
    bool mayMatch(const DOMElement* element) const;
    bool empty() const { return classes_.empty() && ids_.empty() && tags_.empty() && attributes_.empty(); }
    void merge(const SubjectKeys& other);

private:
    std::unordered_set<std::string> classes_, ids_, tags_, attributes_;
};

class SelectorFeatures {
public:
    /// Where a feature sits, as bits (a name can be in several places)
    enum Position : uint8_t {
        Subject  = 1 << 0,   ///< the compound the rule styles
        Ancestor = 1 << 1,   ///< left of ' ' or '>'
        Sibling  = 1 << 2,   ///< left of '+' or '~'
        Has      = 1 << 3,   ///< inside :has()
    };

    /// Adds the features of a selector list ("a.b > c, d:hover")
    void addSelectorList(const std::string& selectorList);
    /// Adds another set's features
    void merge(const SelectorFeatures& other);
    void clear();

    uint8_t classPositions(const std::string& name) const { return lookup(classes_, name); }
    uint8_t idPositions(const std::string& name) const { return lookup(ids_, name); }
    uint8_t attributePositions(const std::string& name) const { return lookup(attributes_, name); }
    uint8_t pseudoClassPositions(const std::string& name) const { return lookup(pseudoClasses_, name); }
    /// :nth-child(), :first-child, :empty, ... are in use
    bool structural() const { return structural_; }
    /// '+' or '~' are in use
    bool siblingCombinators() const { return siblingCombinators_; }
    /// :has() is in use
    bool hasSelector() const { return has_; }
    /// The keys of the compounds :has() is in (the elements it can style)
    const SubjectKeys& hasSubjects() const { return hasSubjects_; }
    /// A :has() compound without a key (any element may be styled by it)
    bool hasSubjectUniversal() const { return hasSubjectUniversal_; }
    /// Whether an element inserted or removed could change a :has() match:
    /// it has a tag, class, id or attribute some :has() argument names
    bool mayAffectHas(const DOMElement* element) const;
    /// A selector the scanner could not take apart: every attribute or
    /// state change then restyles the element's parent's subtree
    bool opaque() const { return opaque_; }

private:
    static uint8_t lookup(const std::unordered_map<std::string, uint8_t>& map, const std::string& name) {
        auto it = map.find(name);
        return it == map.end() ? 0 : it->second;
    }
    void addComplex(const std::string& selector, uint8_t context);
    void addCompound(const std::string& compound, uint8_t position);

    std::unordered_map<std::string, uint8_t> classes_, ids_, attributes_, pseudoClasses_;
    SubjectKeys hasSubjects_;
    std::unordered_set<std::string> hasTags_;
    bool hasSubjectUniversal_ = false;
    bool hasArgumentUniversal_ = false;
    bool structural_ = false;
    bool siblingCombinators_ = false;
    bool has_ = false;
    bool opaque_ = false;
};

/// Splits a selector list at its top-level commas (not inside (), [] or quotes)
std::vector<std::string> splitSelectorList(const std::string& selectorList);

/// What one style pass did, for ZEPRA_STYLE_STATS and the tests
struct StyleRecalcStats {
    uint64_t passes = 0;            ///< computeStyles() calls
    uint64_t fullPasses = 0;        ///< ... that dropped every style
    uint64_t targetedPasses = 0;    ///< ... that dropped only some
    uint64_t stylesDropped = 0;     ///< cached element styles dropped by targeted passes
    uint64_t stylesComputed = 0;    ///< element styles computed (cache misses)
    double invalidationMs = 0;      ///< time spent deciding what to drop
};

} // namespace Zepra::WebCore
