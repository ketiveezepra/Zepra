// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file style_change_log.hpp
 * @brief What DOM changes can do to styles, recorded for the CSS engine
 *
 * The DOM records each change that can change what selectors match (a
 * child inserted or removed, an attribute set, a pseudo-class state
 * flipped) in the log of the node's document, if a CSS engine follows that
 * document. On its next style pass the engine (CSSEngine::computeStyles)
 * drops the cached styles of only the elements the change can reach: the
 * element, its subtree, or its parent's children, as the selectors in use
 * require (style_invalidation.cpp).
 *
 * A change made without saying what it was (noteDomMutation()) makes every
 * followed document restyle all of its elements, as before this log
 * existed. The pointers in a log are compared, never followed, until the
 * engine has found them in its document's tree: a recorded node may have
 * been destroyed since.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Zepra::WebCore {

class DOMDocument;
class DOMElement;
class DOMNode;

struct StyleChange {
    enum class Kind : uint8_t {
        ChildInserted,   ///< `node` went into `parent`
        ChildRemoved,    ///< a child left `parent` (`node`: the removed one)
        Attribute,       ///< `name` on element `node` changed; `oldValue` was its value
        State,           ///< pseudo-class `name` may match element `node` differently
        Interaction,     ///< :hover/:active/:focus (`name`) moved from `node` to `parent` (and their ancestors)
        Subtree,         ///< every style in element `node`'s subtree may change
        Text,            ///< the text of a child of `parent` changed (:empty)
    };
    Kind kind = Kind::Subtree;
    DOMNode* node = nullptr;
    DOMNode* parent = nullptr;
    std::string name;
    std::string oldValue;
};

struct StyleChangeLog {
    /// Something changed that the log cannot describe: restyle everything
    bool full = false;
    std::vector<StyleChange> changes;
};

/// The CSS engine of `document` follows its changes (on: start a log that
/// asks for a full restyle first; off: drop it)
void trackStyleChanges(const DOMDocument* document, bool on);
/// The changes recorded for `document` since the last call (a document
/// nobody follows has none: `full` is set)
StyleChangeLog takeStyleChanges(const DOMDocument* document);
/// Records a change in the log of `context`'s document (bumping
/// domMutationVersion(), so the browser lays the page out again)
void noteStyleChange(const DOMNode* context, StyleChange change);
/// Shorthands for the common cases
void noteChildInserted(DOMNode* parent, DOMNode* child);
void noteChildRemoved(DOMNode* parent, DOMNode* child);
void noteAttributeChange(DOMElement* element, const std::string& name, const std::string& oldValue);
void noteStateChange(DOMElement* element, const std::string& pseudoClass);
void noteSubtreeStyleChange(DOMElement* element);
/// :hover, :active or :focus (`pseudoClass`) moved from `from` to `to` (either
/// may be null) in `document`; neither element is followed unless the
/// engine finds it in the tree
void noteInteractionChange(const DOMDocument* document, DOMElement* from, DOMElement* to,
                           const char* pseudoClass);
/// Every followed document must restyle all of its elements
void noteFullStyleChange();
/// How many changes were recorded through noteStyleChange() so far. Each
/// bumps domMutationVersion() once, so a version that moved further than
/// this count saw changes made with a bare noteDomMutation(), which nobody
/// described: the engine restyles everything then.
uint64_t describedStyleChangeCount();

} // namespace Zepra::WebCore
