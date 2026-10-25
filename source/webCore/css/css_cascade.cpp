// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * css_cascade.cpp - CSS Cascade Implementation
 * 
 * Implements the CSS cascade algorithm per W3C spec:
 * https://www.w3.org/TR/css-cascade-5/
 * 
 * 1. Collect matching rules for element
 * 2. Sort by cascade order (origin, specificity, source order)
 * 3. Resolve cascaded values
 */

#include "css/css_engine.hpp"
#include "browser/dom.hpp"
#include <algorithm>
#include <iostream>
#include <sstream>

namespace Zepra::WebCore {

InteractionState& interactionState() {
    static InteractionState state;
    return state;
}

// ============================================================================
// MatchedRule comparison (cascade ordering)
// ============================================================================

bool MatchedRule::operator<(const MatchedRule& other) const {
    // 1. Origin (lower origins have lower priority)
    // UserAgent < User < Author (normal)
    // Author!important > User!important > UserAgent!important
    if (origin != other.origin) {
        return static_cast<int>(origin) < static_cast<int>(other.origin);
    }
    
    // 2. Specificity (higher specificity wins)
    // Compare (a, b, c) tuple: IDs, Classes, Elements
    if (specificity.a != other.specificity.a) {
        return specificity.a < other.specificity.a;
    }
    if (specificity.b != other.specificity.b) {
        return specificity.b < other.specificity.b;
    }
    if (specificity.c != other.specificity.c) {
        return specificity.c < other.specificity.c;
    }
    
    // 3. Source order (later declarations win)
    return order < other.order;
}

// ============================================================================
// CSSCascade - Rule Collection and Sorting
// ============================================================================

std::vector<MatchedRule> CSSCascade::collectMatchingRules(
    DOMElement* element,
    const std::vector<CSSStyleSheet*>& stylesheets,
    StyleOrigin origin
) {
    std::vector<MatchedRule> matched;
    size_t ruleOrder = 0;
    
    if (!element) return matched;
    
    // Iterate all stylesheets
    for (const auto* sheet : stylesheets) {
        if (!sheet || !sheet->cssRules()) continue;
        
        // Iterate all rules in stylesheet
        CSSRuleList* rules = sheet->cssRules();
        for (size_t i = 0; i < rules->length(); i++) {
            const CSSRule* rule = rules->item(i);
            
            // Only process style rules
            const CSSStyleRule* styleRule = dynamic_cast<const CSSStyleRule*>(rule);
            if (!styleRule) continue;
            
            // Check if selector matches this element
            if (selectorMatches(element, styleRule->selectorText())) {
                MatchedRule match;
                match.rule = styleRule;
                match.origin = origin;
                match.specificity = calculateSpecificity(styleRule->selectorText());
                match.order = ruleOrder++;
                
                matched.push_back(match);
            }
        }
    }
    
    return matched;
}

std::vector<MatchedRule> CSSCascade::collectMatchingRulesWithOrigins(
    DOMElement* element,
    const std::vector<std::pair<CSSStyleSheet*, StyleOrigin>>& sheets
) {
    std::vector<MatchedRule> matched;
    
    if (!element) return matched;
    
    // Build index on first call (or after invalidation)
    if (!indexed_) {
        buildIndex(sheets);
    }
    
    // Collect candidate rule indices from the index
    std::vector<bool> checked(indexedRules_.size(), false);
    auto tryMatch = [&](size_t idx) {
        if (idx >= indexedRules_.size() || checked[idx]) return;
        checked[idx] = true;
        const auto& ir = indexedRules_[idx];
        if (selectorMatches(element, ir.rule->selectorText())) {
            MatchedRule match;
            match.rule = ir.rule;
            match.origin = ir.origin;
            match.specificity = calculateSpecificity(ir.rule->selectorText());
            match.order = ir.order;
            matched.push_back(match);
        }
    };
    
    // Lookup by element's classes
    std::string classes = element->getAttribute("class");
    if (!classes.empty()) {
        std::istringstream ss(classes);
        std::string cls;
        while (ss >> cls) {
            auto it = index_.byClass.find(cls);
            if (it != index_.byClass.end()) {
                for (size_t idx : it->second) tryMatch(idx);
            }
        }
    }
    
    // Lookup by tag name
    std::string tag = element->tagName();
    std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
    {
        auto it = index_.byTag.find(tag);
        if (it != index_.byTag.end()) {
            for (size_t idx : it->second) tryMatch(idx);
        }
    }
    
    // Lookup by ID
    std::string id = element->getAttribute("id");
    if (!id.empty()) {
        auto it = index_.byId.find(id);
        if (it != index_.byId.end()) {
            for (size_t idx : it->second) tryMatch(idx);
        }
    }
    
    // Always check universal/complex selectors
    for (size_t idx : index_.universal) tryMatch(idx);
    
    return matched;
}

void CSSCascade::buildIndex(
    const std::vector<std::pair<CSSStyleSheet*, StyleOrigin>>& sheets
) {
    index_.byClass.clear();
    index_.byTag.clear();
    index_.byId.clear();
    index_.universal.clear();
    indexedRules_.clear();
    
    size_t ruleOrder = 0;
    
    for (const auto& [sheet, origin] : sheets) {
        if (!sheet || !sheet->cssRules()) continue;
        
        CSSRuleList* rules = sheet->cssRules();
        size_t ruleCount = rules->length();
        
        for (size_t i = 0; i < ruleCount; i++) {
            const CSSRule* rule = rules->item(i);
            const CSSStyleRule* styleRule = dynamic_cast<const CSSStyleRule*>(rule);
            if (!styleRule) continue;
            
            size_t ruleIdx = indexedRules_.size();
            indexedRules_.push_back({styleRule, origin, ruleOrder++});
            // Index every selector of a list ("td, th"), not just the last
            const std::string& text = styleRule->selectorText();
            size_t partStart = 0;
            int depth = 0;
            for (size_t c = 0; c <= text.size(); c++) {
                char ch = c < text.size() ? text[c] : ',';
                if (ch == '(' || ch == '[') depth++;
                else if ((ch == ')' || ch == ']') && depth > 0) depth--;
                else if (ch == ',' && depth == 0) {
                    std::string part = text.substr(partStart, c - partStart);
                    size_t b = part.find_first_not_of(" \t\n\r");
                    if (b != std::string::npos) indexSelector(part.substr(b), ruleIdx);
                    partStart = c + 1;
                }
            }
        }
    }
    
    indexed_ = true;
}

void CSSCascade::indexSelector(const std::string& selector, size_t ruleIdx) {
    // Extract the rightmost (key) compound selector for indexing.
    // For "div > .foo .bar" the key selector is ".bar".
    // We extract class, id, and tag tokens from the rightmost part.
    
    // Find rightmost compound: split by whitespace and combinators, take last
    std::string key;
    size_t last = selector.size();
    // Walk backwards past trailing whitespace
    while (last > 0 && (selector[last-1] == ' ' || selector[last-1] == '\t')) last--;
    // Walk backwards to find start of last compound selector
    size_t start = last;
    int parenDepth = 0;
    while (start > 0) {
        char c = selector[start - 1];
        if (c == ')') { parenDepth++; start--; continue; }
        if (c == '(') { parenDepth--; start--; continue; }
        if (parenDepth > 0) { start--; continue; }
        if (c == ' ' || c == '>' || c == '+' || c == '~') break;
        start--;
    }
    key = selector.substr(start, last - start);
    
    // Now extract tokens from key selector
    bool indexed = false;
    size_t k = 0;
    size_t klen = key.size();
    
    while (k < klen) {
        if (key[k] == '.') {
            // Class selector
            k++;
            std::string className;
            while (k < klen) {
                if (key[k] == '\\' && k + 1 < klen) {
                    className += key[k + 1];
                    k += 2;
                } else if (std::isalnum(key[k]) || key[k] == '-' || key[k] == '_') {
                    className += key[k];
                    k++;
                } else {
                    break;
                }
            }
            if (!className.empty()) {
                index_.byClass[className].push_back(ruleIdx);
                indexed = true;
            }
        } else if (key[k] == '#') {
            // ID selector
            k++;
            std::string idVal;
            while (k < klen && (std::isalnum(key[k]) || key[k] == '-' || key[k] == '_')) {
                idVal += key[k++];
            }
            if (!idVal.empty()) {
                index_.byId[idVal].push_back(ruleIdx);
                indexed = true;
            }
        } else if (key[k] == '[' || key[k] == ':') {
            // Attribute or pseudo selector — skip over
            if (key[k] == '[') {
                while (k < klen && key[k] != ']') k++;
                if (k < klen) k++;
            } else {
                k++;
                if (k < klen && key[k] == ':') k++; // pseudo-element
                while (k < klen && (std::isalnum(key[k]) || key[k] == '-')) k++;
                if (k < klen && key[k] == '(') {
                    int depth = 1; k++;
                    while (k < klen && depth > 0) {
                        if (key[k] == '(') depth++;
                        else if (key[k] == ')') depth--;
                        k++;
                    }
                }
            }
        } else if (key[k] == '*') {
            k++;
        } else if (std::isalnum(key[k]) || key[k] == '-' || key[k] == '_') {
            // Tag selector
            std::string tagName;
            while (k < klen && (std::isalnum(key[k]) || key[k] == '-' || key[k] == '_')) {
                tagName += key[k++];
            }
            std::transform(tagName.begin(), tagName.end(), tagName.begin(), ::tolower);
            index_.byTag[tagName].push_back(ruleIdx);
            indexed = true;
        } else {
            k++;
        }
    }
    
    // If we couldn't extract any indexable token, add to universal list
    if (!indexed) {
        index_.universal.push_back(ruleIdx);
    }
}

void CSSCascade::sortByCascade(std::vector<MatchedRule>& rules) {
    // Sort using operator< which implements cascade ordering
    std::sort(rules.begin(), rules.end());
}

std::optional<std::string> CSSCascade::cascadedValue(
    const std::string& property,
    const std::vector<MatchedRule>& rules
) {
    // Iterate rules in reverse (highest priority last due to sort order)
    for (auto it = rules.rbegin(); it != rules.rend(); ++it) {
        if (!it->rule || !it->rule->style()) continue;
        
        std::string value = it->rule->style()->getPropertyValue(property);
        if (!value.empty()) {
            return value;
        }
    }
    
    return std::nullopt;
}

// ============================================================================
// Helper: Selector Matching (simplified)
// ============================================================================

bool CSSCascade::selectorMatches(DOMElement* element, const std::string& selector) {
    if (!element) return false;
    
    std::string sel = selector;
    
    // Trim whitespace
    size_t start = sel.find_first_not_of(" \t\n\r");
    size_t end = sel.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return false;
    sel = sel.substr(start, end - start + 1);
    
    // Handle comma-separated selector groups: "div, .class, #id"
    // Each group is tested independently — if any matches, the selector matches
    {
        size_t depth = 0;
        size_t groupStart = 0;
        for (size_t i = 0; i <= sel.size(); i++) {
            if (i < sel.size() && sel[i] == '(') depth++;
            else if (i < sel.size() && sel[i] == ')' && depth > 0) depth--;
            else if ((i == sel.size() || sel[i] == ',') && depth == 0) {
                std::string group = sel.substr(groupStart, i - groupStart);
                // Trim the group
                size_t gs = group.find_first_not_of(" \t\n\r");
                size_t ge = group.find_last_not_of(" \t\n\r");
                if (gs != std::string::npos) {
                    group = group.substr(gs, ge - gs + 1);
                    if (selectorGroupMatches(element, group)) return true;
                }
                groupStart = i + 1;
            }
        }
        return false;
    }
}

// Match a single selector group (no commas) — handles combinators
bool CSSCascade::selectorGroupMatches(DOMElement* element, const std::string& selector) {
    if (!element || selector.empty()) return false;
    
    // Tokenize selector into parts separated by combinators
    // "div > .class p.item" → ["div", ">", ".class", " ", "p.item"]
    std::vector<std::string> parts;
    std::vector<char> combinators; // ' ' = descendant, '>' = child
    
    size_t i = 0;
    size_t len = selector.size();
    while (i < len) {
        // Skip whitespace
        while (i < len && (selector[i] == ' ' || selector[i] == '\t')) i++;
        if (i >= len) break;
        
        // Check for combinator
        if (!parts.empty() && (selector[i] == '>' || selector[i] == '+' || selector[i] == '~')) {
            char comb = selector[i];
            i++;
            while (i < len && (selector[i] == ' ' || selector[i] == '\t')) i++;
            combinators.push_back(comb);
            continue;
        }
        
        // Read compound selector part
        size_t partStart = i;
        while (i < len && selector[i] != ' ' && selector[i] != '\t' && 
               selector[i] != '>' && selector[i] != '+' && selector[i] != '~') {
            if (selector[i] == '[') {
                while (i < len && selector[i] != ']') i++;
                if (i < len) i++;
            } else if (selector[i] == '(') {
                int depth = 1;
                i++;
                while (i < len && depth > 0) {
                    if (selector[i] == '(') depth++;
                    else if (selector[i] == ')') depth--;
                    i++;
                }
            } else {
                i++;
            }
        }
        
        if (i > partStart) {
            std::string part = selector.substr(partStart, i - partStart);
            if (!parts.empty() && combinators.size() < parts.size()) {
                combinators.push_back(' '); // implicit descendant combinator
            }
            parts.push_back(part);
        }
    }
    
    if (parts.empty()) return false;
    
    // Single compound selector — no combinators
    if (parts.size() == 1) {
        return compoundSelectorMatches(element, parts[0]);
    }
    
    // Match rightmost part against element, then walk up for combinators
    if (!compoundSelectorMatches(element, parts.back())) return false;
    
    DOMElement* current = element;
    for (int p = (int)parts.size() - 2; p >= 0; p--) {
        char comb = (p < (int)combinators.size()) ? combinators[p] : ' ';
        
        if (comb == '>') {
            // Child combinator — parent must match
            DOMNode* parentNode = current->parentNode();
            current = parentNode ? dynamic_cast<DOMElement*>(parentNode) : nullptr;
            if (!current || !compoundSelectorMatches(current, parts[p])) return false;
        } else if (comb == '+') {
            // Adjacent sibling combinator — previous element sibling must match
            DOMNode* prev = current->previousSibling();
            while (prev && !dynamic_cast<DOMElement*>(prev)) prev = prev->previousSibling();
            current = prev ? dynamic_cast<DOMElement*>(prev) : nullptr;
            if (!current || !compoundSelectorMatches(current, parts[p])) return false;
        } else if (comb == '~') {
            // General sibling combinator — any preceding element sibling must match
            DOMNode* sib = current->previousSibling();
            bool found = false;
            while (sib) {
                auto* sibEl = dynamic_cast<DOMElement*>(sib);
                if (sibEl && compoundSelectorMatches(sibEl, parts[p])) {
                    current = sibEl;
                    found = true;
                    break;
                }
                sib = sib->previousSibling();
            }
            if (!found) return false;
        } else {
            // Descendant combinator — any ancestor must match
            DOMNode* parentNode = current->parentNode();
            DOMElement* ancestor = parentNode ? dynamic_cast<DOMElement*>(parentNode) : nullptr;
            bool found = false;
            while (ancestor) {
                if (compoundSelectorMatches(ancestor, parts[p])) {
                    current = ancestor;
                    found = true;
                    break;
                }
                parentNode = ancestor->parentNode();
                ancestor = parentNode ? dynamic_cast<DOMElement*>(parentNode) : nullptr;
            }
            if (!found) return false;
        }
    }
    
    return true;
}

// Match a compound selector ("div.class#id[attr]") against a single element
bool CSSCascade::compoundSelectorMatches(DOMElement* element, const std::string& compound) {
    if (!element || compound.empty()) return false;
    
    // Universal selector
    if (compound == "*") return true;
    
    size_t i = 0;
    size_t len = compound.size();
    
    while (i < len) {
        if (compound[i] == '#') {
            // ID selector
            i++;
            size_t s = i;
            while (i < len && (std::isalnum(compound[i]) || compound[i] == '-' || compound[i] == '_')) i++;
            std::string id = compound.substr(s, i - s);
            if (element->getAttribute("id") != id) return false;
        }
        else if (compound[i] == '.') {
            // Class selector — handle CSS escapes (e.g. Tailwind .hover\:bg-blue-500)
            i++;
            std::string className;
            while (i < len) {
                if (compound[i] == '\\' && i + 1 < len) {
                    className += compound[i + 1]; // Escaped character is literal
                    i += 2;
                } else if (std::isalnum(compound[i]) || compound[i] == '-' || compound[i] == '_') {
                    className += compound[i];
                    i++;
                } else {
                    break;
                }
            }
            std::string classes = element->getAttribute("class");
            // Word boundary match
            size_t pos = 0;
            bool found = false;
            while ((pos = classes.find(className, pos)) != std::string::npos) {
                bool startOK = (pos == 0 || classes[pos-1] == ' ');
                bool endOK = (pos + className.length() >= classes.length() || 
                              classes[pos + className.length()] == ' ');
                if (startOK && endOK) { found = true; break; }
                pos++;
            }
            if (!found) return false;
        }
        else if (compound[i] == '[') {
            // Attribute selector
            i++;
            size_t s = i;
            while (i < len && compound[i] != '=' && compound[i] != ']' && compound[i] != '~' && compound[i] != '|') i++;
            std::string attrName = compound.substr(s, i - s);
            
            if (i < len && compound[i] == ']') {
                // [attr] — has attribute
                i++;
                if (element->getAttribute(attrName).empty() && !element->hasAttribute(attrName)) return false;
            } else if (i < len && compound[i] == '=') {
                // [attr=value]
                i++;
                std::string attrVal;
                if (i < len && (compound[i] == '"' || compound[i] == '\'')) {
                    char q = compound[i++];
                    size_t vs = i;
                    while (i < len && compound[i] != q) i++;
                    attrVal = compound.substr(vs, i - vs);
                    if (i < len) i++;
                } else {
                    size_t vs = i;
                    while (i < len && compound[i] != ']') i++;
                    attrVal = compound.substr(vs, i - vs);
                }
                if (i < len && compound[i] == ']') i++;
                if (element->getAttribute(attrName) != attrVal) return false;
            } else {
                // Skip to end of attribute selector
                while (i < len && compound[i] != ']') i++;
                if (i < len) i++;
            }
        }
        else if (compound[i] == ':') {
            i++;
            bool isPseudoElement = false;
            if (i < len && compound[i] == ':') { isPseudoElement = true; i++; }
            
            // Extract pseudo name
            size_t nameStart = i;
            while (i < len && (std::isalnum(compound[i]) || compound[i] == '-')) i++;
            std::string pseudoName = compound.substr(nameStart, i - nameStart);
            
            // Extract argument if present
            std::string pseudoArg;
            if (i < len && compound[i] == '(') {
                int depth = 1;
                i++;
                size_t argStart = i;
                while (i < len && depth > 0) {
                    if (compound[i] == '(') depth++;
                    else if (compound[i] == ')') depth--;
                    if (depth > 0) i++; else break;
                }
                pseudoArg = compound.substr(argStart, i - argStart);
                if (i < len) i++; // skip ')'
            }
            
            // Pseudo-elements (::before, ::after, and the old one-colon forms)
            // style boxes that aren't generated yet, never the element itself
            if (isPseudoElement || pseudoName == "before" || pseudoName == "after" ||
                pseudoName == "first-line" || pseudoName == "first-letter") {
                return false;
            }
            
            // UI state pseudo-classes: match the live interaction state and the
            // element's attributes (see InteractionState in css_engine.hpp)
            if (pseudoName == "hover" || pseudoName == "active" ||
                pseudoName == "focus" || pseudoName == "focus-visible" ||
                pseudoName == "focus-within") {
                const InteractionState& state = interactionState();
                auto isSelfOrAncestorOf = [element](DOMElement* target) {
                    for (DOMNode* n = target; n; n = n->parentNode()) {
                        if (n == static_cast<DOMNode*>(element)) return true;
                    }
                    return false;
                };
                bool matches = false;
                if (pseudoName == "hover") {
                    matches = state.hovered && isSelfOrAncestorOf(state.hovered);
                } else if (pseudoName == "active") {
                    matches = state.active && isSelfOrAncestorOf(state.active);
                } else if (pseudoName == "focus-within") {
                    matches = state.focused && isSelfOrAncestorOf(state.focused);
                } else {
                    matches = state.focused == element;
                }
                if (!matches) return false;
                continue;
            }
            if (pseudoName == "link" || pseudoName == "any-link" || pseudoName == "visited" ||
                pseudoName == "enabled" || pseudoName == "disabled" ||
                pseudoName == "checked" || pseudoName == "placeholder-shown" ||
                pseudoName == "read-only" || pseudoName == "read-write" ||
                pseudoName == "target" || pseudoName == "default" ||
                pseudoName == "indeterminate" ||
                pseudoName == "placeholder" || pseudoName == "selection") {
                std::string tagLower = element->tagName();
                std::transform(tagLower.begin(), tagLower.end(), tagLower.begin(), ::tolower);
                const bool isFormControl = tagLower == "input" || tagLower == "button" ||
                    tagLower == "select" || tagLower == "textarea" ||
                    tagLower == "option" || tagLower == "optgroup" || tagLower == "fieldset";
                const bool isTextField = tagLower == "input" || tagLower == "textarea";
                bool matches = false;
                if (pseudoName == "link" || pseudoName == "any-link") {
                    matches = (tagLower == "a" || tagLower == "area") && element->hasAttribute("href");
                } else if (pseudoName == "disabled") {
                    matches = isFormControl && element->hasAttribute("disabled");
                } else if (pseudoName == "enabled") {
                    matches = isFormControl && !element->hasAttribute("disabled");
                } else if (pseudoName == "checked") {
                    matches = (tagLower == "input" && element->hasAttribute("checked")) ||
                              (tagLower == "option" && element->hasAttribute("selected"));
                } else if (pseudoName == "placeholder-shown") {
                    matches = isTextField && element->hasAttribute("placeholder") &&
                              element->getAttribute("value").empty();
                } else if (pseudoName == "read-write") {
                    matches = isTextField && !element->hasAttribute("readonly") &&
                              !element->hasAttribute("disabled");
                } else if (pseudoName == "read-only") {
                    matches = !(isTextField && !element->hasAttribute("readonly") &&
                                !element->hasAttribute("disabled"));
                }
                // :visited never matches (no history styling); :target, :default,
                // :indeterminate and the legacy single-colon pseudo-elements
                // (:placeholder, :selection) are not rendered.
                if (!matches) return false;
                continue;
            }
            
            // Structural pseudo-classes — require DOM tree walks
            if (pseudoName == "first-child") {
                DOMNode* sib = element->previousSibling();
                while (sib && !dynamic_cast<DOMElement*>(sib)) sib = sib->previousSibling();
                if (sib) return false;
            } else if (pseudoName == "last-child") {
                DOMNode* sib = element->nextSibling();
                while (sib && !dynamic_cast<DOMElement*>(sib)) sib = sib->nextSibling();
                if (sib) return false;
            } else if (pseudoName == "only-child") {
                DOMNode* sib = element->previousSibling();
                while (sib && !dynamic_cast<DOMElement*>(sib)) sib = sib->previousSibling();
                if (sib) return false;
                sib = element->nextSibling();
                while (sib && !dynamic_cast<DOMElement*>(sib)) sib = sib->nextSibling();
                if (sib) return false;
            } else if (pseudoName == "nth-child" && !pseudoArg.empty()) {
                int idx = 1;
                DOMNode* sib = element->previousSibling();
                while (sib) {
                    if (dynamic_cast<DOMElement*>(sib)) idx++;
                    sib = sib->previousSibling();
                }
                if (pseudoArg == "odd") { if (idx % 2 == 0) return false; }
                else if (pseudoArg == "even") { if (idx % 2 != 0) return false; }
                else {
                    try { int n = std::stoi(pseudoArg); if (idx != n) return false; } catch (...) {}
                }
            } else if (pseudoName == "root") {
                std::string tag = element->tagName();
                std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
                if (tag != "html") return false;
            } else if (pseudoName == "empty") {
                if (element->firstChild()) return false;
            } else if (pseudoName == "not" && !pseudoArg.empty()) {
                if (compoundSelectorMatches(element, pseudoArg)) return false;
            } else if (pseudoName == "is" || pseudoName == "matches" || pseudoName == "any") {
                if (!pseudoArg.empty() && !selectorMatches(element, pseudoArg)) return false;
            } else if (pseudoName == "where") {
                // :where(sel) — same as :is() but zero specificity
                if (!pseudoArg.empty() && !selectorMatches(element, pseudoArg)) return false;
            }
        }
        else if (std::isalnum(compound[i]) || compound[i] == '-' || compound[i] == '_') {
            // Tag selector
            size_t s = i;
            while (i < len && (std::isalnum(compound[i]) || compound[i] == '-' || compound[i] == '_')) i++;
            std::string tag = compound.substr(s, i - s);
            std::string elemTag = element->tagName();
            // Case-insensitive comparison
            std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
            std::transform(elemTag.begin(), elemTag.end(), elemTag.begin(), ::tolower);
            if (elemTag != tag) return false;
        }
        else {
            i++;
        }
    }
    
    return true;
}

Selector::Specificity CSSCascade::calculateSpecificity(const std::string& selector) {
    Selector::Specificity spec = {0, 0, 0};
    
    size_t i = 0;
    while (i < selector.length()) {
        char c = selector[i];
        
        if (c == '#') {
            spec.a++;
            i++;
            while (i < selector.length() && 
                   (std::isalnum(selector[i]) || selector[i] == '-' || selector[i] == '_')) {
                i++;
            }
        } else if (c == '.') {
            spec.b++;
            i++;
            while (i < selector.length()) {
                if (selector[i] == '\\' && i + 1 < selector.length()) { i += 2; continue; }
                if (std::isalnum(selector[i]) || selector[i] == '-' || selector[i] == '_') { i++; continue; }
                break;
            }
        } else if (c == '[') {
            spec.b++;
            while (i < selector.length() && selector[i] != ']') i++;
            if (i < selector.length()) i++;
        } else if (c == ':') {
            i++;
            bool isPseudoElement = false;
            if (i < selector.length() && selector[i] == ':') { isPseudoElement = true; i++; }
            
            size_t nameStart = i;
            while (i < selector.length() && (std::isalnum(selector[i]) || selector[i] == '-')) i++;
            std::string pseudoName = selector.substr(nameStart, i - nameStart);
            
            std::string pseudoArg;
            if (i < selector.length() && selector[i] == '(') {
                int depth = 1;
                i++;
                size_t argStart = i;
                while (i < selector.length() && depth > 0) {
                    if (selector[i] == '(') depth++;
                    else if (selector[i] == ')') depth--;
                    if (depth > 0) i++; else break;
                }
                pseudoArg = selector.substr(argStart, i - argStart);
                if (i < selector.length()) i++;
            }
            
            if (isPseudoElement) {
                spec.c++;
            } else if (pseudoName == "where") {
                // :where() contributes zero specificity (CSS Selectors L4)
            } else if (pseudoName == "not" || pseudoName == "is" || pseudoName == "matches") {
                // Specificity = max specificity of arguments
                if (!pseudoArg.empty()) {
                    auto argSpec = calculateSpecificity(pseudoArg);
                    spec.a += argSpec.a;
                    spec.b += argSpec.b;
                    spec.c += argSpec.c;
                }
            } else {
                spec.b++;
            }
        } else if (std::isalpha(c) || c == '*') {
            if (c != '*') spec.c++;
            i++; // consume the first character ('*' or alpha)
            while (i < selector.length() && 
                   (std::isalnum(selector[i]) || selector[i] == '-')) {
                i++;
            }
        } else {
            i++;
        }
    }
    
    return spec;
}

} // namespace Zepra::WebCore
