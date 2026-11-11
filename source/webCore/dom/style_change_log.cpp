// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file style_change_log.cpp
 * @brief The per-document logs of style-relevant DOM changes (see
 *        style_change_log.hpp)
 */

#include "browser/style_change_log.hpp"
#include "browser/dom.hpp"

#include <unordered_map>

namespace Zepra::WebCore {

namespace {

/// A log keeps this many changes; past that the document restyles fully
/// (a page rebuilding itself is cheaper to restyle than to sort through)
constexpr size_t kMaxLoggedChanges = 50000;

/// Logs of the followed documents, by address (only compared)
std::unordered_map<const DOMDocument*, StyleChangeLog>& logs() {
    static std::unordered_map<const DOMDocument*, StyleChangeLog> all;
    return all;
}

StyleChangeLog* logOf(const DOMNode* node) {
    auto& all = logs();
    if (!node || all.empty()) return nullptr;
    const DOMDocument* doc = node->nodeType() == NodeType::Document ? static_cast<const DOMDocument*>(node)
                                                                     : node->ownerDocument();
    auto it = all.find(doc);
    return it != all.end() ? &it->second : nullptr;
}

}  // namespace

// Changes recorded through noteStyleChange (each bumps domMutationVersion
// once): a version step without one of these was a change nobody described
static uint64_t s_describedChanges = 0;
uint64_t describedStyleChangeCount() { return s_describedChanges; }

void trackStyleChanges(const DOMDocument* document, bool on) {
    if (!document) return;
    if (on) {
        StyleChangeLog& log = logs()[document];
        log.changes.clear();
        log.full = true;
    } else {
        logs().erase(document);
    }
}

StyleChangeLog takeStyleChanges(const DOMDocument* document) {
    StyleChangeLog out;
    auto it = logs().find(document);
    if (it == logs().end()) {
        out.full = true;
        return out;
    }
    std::swap(out, it->second);
    return out;
}

void noteStyleChange(const DOMNode* context, StyleChange change) {
    noteDomMutation();
    s_describedChanges++;
    StyleChangeLog* log = logOf(context);
    if (!log || log->full) return;
    if (log->changes.size() >= kMaxLoggedChanges) {
        log->full = true;
        log->changes.clear();
        log->changes.shrink_to_fit();
        return;
    }
    log->changes.push_back(std::move(change));
}

void noteChildInserted(DOMNode* parent, DOMNode* child) {
    StyleChange c;
    c.kind = StyleChange::Kind::ChildInserted;
    c.parent = parent;
    c.node = child;
    noteStyleChange(parent, std::move(c));
}

void noteChildRemoved(DOMNode* parent, DOMNode* child) {
    StyleChange c;
    c.kind = StyleChange::Kind::ChildRemoved;
    c.parent = parent;
    c.node = child;
    noteStyleChange(parent, std::move(c));
}

void noteAttributeChange(DOMElement* element, const std::string& name, const std::string& oldValue) {
    StyleChange c;
    c.kind = StyleChange::Kind::Attribute;
    c.node = element;
    c.name = name;
    // Only class and id changes are compared token by token
    if (name == "class" || name == "id") c.oldValue = oldValue;
    noteStyleChange(element, std::move(c));
}

void noteStateChange(DOMElement* element, const std::string& pseudoClass) {
    StyleChange c;
    c.kind = StyleChange::Kind::State;
    c.node = element;
    c.name = pseudoClass;
    noteStyleChange(element, std::move(c));
}

void noteSubtreeStyleChange(DOMElement* element) {
    StyleChange c;
    c.kind = StyleChange::Kind::Subtree;
    c.node = element;
    noteStyleChange(element, std::move(c));
}

void noteInteractionChange(const DOMDocument* document, DOMElement* from, DOMElement* to,
                           const char* pseudoClass) {
    StyleChange c;
    c.kind = StyleChange::Kind::Interaction;
    c.node = from;
    c.parent = to;
    c.name = pseudoClass;
    noteStyleChange(document, std::move(c));
}

void noteTextStyleChange(DOMNode* node) {
    StyleChange c;
    c.kind = StyleChange::Kind::Text;
    c.parent = node ? node->parentNode() : nullptr;
    c.node = node;
    noteStyleChange(node, std::move(c));
}

void noteElementStateChange(DOMElement* element, const char* pseudoClass) {
    noteStateChange(element, pseudoClass);
}

void noteFullStyleChange() {
    noteDomMutation();
    for (auto& [doc, log] : logs()) {
        log.full = true;
        log.changes.clear();
    }
}

} // namespace Zepra::WebCore
