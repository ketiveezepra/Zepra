// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file style_invalidation.cpp
 * @brief Selector feature sets and the targeted style pass (see
 *        style_invalidation.hpp)
 */

#include "css/style_invalidation.hpp"
#include "css/css_engine.hpp"
#include "css/computed_style_values.hpp"
#include "css/media_query.hpp"
#include "browser/dom.hpp"
#include "browser/style_change_log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>

namespace Zepra::WebCore {

// ============================================================================
// Selector scanning
// ============================================================================

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

bool isIdentChar(unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_' || c >= 0x80;
}

void appendUtf8(std::string& out, unsigned long cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// An identifier at s[i] with its escapes resolved (CSS Syntax §4.3.7)
std::string readIdent(const std::string& s, size_t& i) {
    std::string out;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\\' && i + 1 < s.size()) {
            i++;
            if (std::isxdigit(static_cast<unsigned char>(s[i]))) {
                unsigned long cp = 0;
                int digits = 0;
                while (i < s.size() && digits < 6 && std::isxdigit(static_cast<unsigned char>(s[i]))) {
                    char h = s[i];
                    cp = cp * 16 + (unsigned long)(std::isdigit(static_cast<unsigned char>(h)) ? h - '0' : (std::tolower(h) - 'a' + 10));
                    i++;
                    digits++;
                }
                if (i < s.size() && isSpace(s[i])) i++;
                appendUtf8(out, cp);
            } else {
                out += s[i++];
            }
            continue;
        }
        if (!isIdentChar(c)) break;
        out += static_cast<char>(c);
        i++;
    }
    return out;
}

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The end of a bracketed or quoted part starting at s[i] ('(', '[', '"', '\'')
size_t skipGroup(const std::string& s, size_t i) {
    char open = s[i];
    if (open == '"' || open == '\'') {
        for (i++; i < s.size(); i++) {
            if (s[i] == '\\') { i++; continue; }
            if (s[i] == open) return i + 1;
        }
        return s.size();
    }
    char close = open == '(' ? ')' : ']';
    int depth = 0;
    for (; i < s.size(); i++) {
        char c = s[i];
        if (c == '\\') { i++; continue; }
        if (c == '"' || c == '\'') { i = skipGroup(s, i) - 1; continue; }
        if (c == open) depth++;
        else if (c == close && --depth == 0) return i + 1;
    }
    return s.size();
}

// The pseudo-classes whose match depends on an element's place among its siblings
bool isStructuralPseudo(const std::string& name) {
    static const std::unordered_set<std::string> kStructural = {
        "first-child", "last-child", "only-child", "nth-child", "nth-last-child",
        "first-of-type", "last-of-type", "only-of-type", "nth-of-type", "nth-last-of-type",
        "empty", "blank",
    };
    return kStructural.count(name) > 0;
}

std::vector<std::string> splitClassTokens(const std::string& value) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < value.size()) {
        while (i < value.size() && isSpace(value[i])) i++;
        size_t j = i;
        while (j < value.size() && !isSpace(value[j])) j++;
        if (j > i) out.push_back(value.substr(i, j - i));
        i = j;
    }
    return out;
}

uint8_t widest(uint8_t position) {
    // Has > Sibling > Ancestor > Subject (each covers more elements)
    if (position & SelectorFeatures::Has) return SelectorFeatures::Has;
    if (position & SelectorFeatures::Sibling) return SelectorFeatures::Sibling;
    if (position & SelectorFeatures::Ancestor) return SelectorFeatures::Ancestor;
    return SelectorFeatures::Subject;
}

}  // namespace

std::vector<std::string> splitSelectorList(const std::string& list) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= list.size(); i++) {
        if (i < list.size()) {
            char c = list[i];
            if (c == '\\') { i++; continue; }
            if (c == '(' || c == '[' || c == '"' || c == '\'') { i = skipGroup(list, i) - 1; continue; }
            if (c != ',') continue;
        }
        size_t a = start, b = i;
        while (a < b && isSpace(list[a])) a++;
        while (b > a && isSpace(list[b - 1])) b--;
        if (b > a) parts.push_back(list.substr(a, b - a));
        start = i + 1;
    }
    return parts;
}

void SelectorFeatures::clear() {
    classes_.clear();
    ids_.clear();
    attributes_.clear();
    pseudoClasses_.clear();
    structural_ = siblingCombinators_ = has_ = opaque_ = false;
    hasSubjects_ = SubjectKeys();
    hasTags_.clear();
    hasSubjectUniversal_ = hasArgumentUniversal_ = false;
}

bool SelectorFeatures::mayAffectHas(const DOMElement* element) const {
    if (!has_ || !element) return false;
    if (hasArgumentUniversal_) return true;
    if (hasTags_.count(lowerAscii(element->tagName()))) return true;
    for (const auto& [name, value] : element->attributes()) {
        if (attributePositions(lowerAscii(name)) & Has) return true;
        if (name == "id" && (idPositions(value) & Has)) return true;
        if (name == "class") {
            for (const std::string& token : splitClassTokens(value)) {
                if (classPositions(token) & Has) return true;
            }
        }
    }
    return false;
}

void SelectorFeatures::addSelectorList(const std::string& list) {
    for (const std::string& selector : splitSelectorList(list)) addComplex(selector, Subject);
}

// One complex selector: its compounds, right to left; `context` is the
// position of the compound whose argument this selector is (Subject at the top)
void SelectorFeatures::addComplex(const std::string& s, uint8_t context) {
    struct Part { std::string compound; char combinator; };   // combinator to its right (0: none)
    std::vector<Part> parts;
    std::string current;
    char pending = 0;   // combinator seen since the last compound
    bool sawSpace = false;
    auto finish = [&] {
        if (current.empty()) return;
        if (!parts.empty()) parts.back().combinator = pending ? pending : ' ';
        parts.push_back({current, 0});
        current.clear();
        pending = 0;
    };
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            if (sawSpace && !current.empty()) { finish(); }
            sawSpace = false;
            current += c;
            current += s[++i];
            continue;
        }
        if (c == '(' || c == '[' || c == '"' || c == '\'') {
            if (sawSpace && !current.empty()) finish();
            sawSpace = false;
            size_t end = skipGroup(s, i);
            current += s.substr(i, end - i);
            i = end - 1;
            continue;
        }
        if (isSpace(c)) { sawSpace = true; continue; }
        if (c == '>' || c == '+' || c == '~' || c == '|') {
            // "||" (column) and "|" (a namespace) are rare: a namespace bar
            // stays in the compound
            if (c == '|' && !(i + 1 < s.size() && s[i + 1] == '|')) { current += c; sawSpace = false; continue; }
            if (c == '|') i++;
            if (!current.empty()) finish();
            pending = c == '|' ? ' ' : c;
            sawSpace = false;
            continue;
        }
        if (sawSpace && !current.empty()) finish();
        sawSpace = false;
        current += c;
    }
    finish();
    if (parts.empty()) return;
    for (size_t k = 0; k < parts.size(); k++) {
        uint8_t position;
        if (k + 1 == parts.size()) {
            position = context;
        } else {
            char comb = parts[k].combinator;
            if (comb == '+' || comb == '~') siblingCombinators_ = true;
            uint8_t own = comb == '+' || comb == '~' ? Sibling : Ancestor;
            position = widest(static_cast<uint8_t>(own | context));
        }
        addCompound(parts[k].compound, position);
    }
}

void SelectorFeatures::addCompound(const std::string& s, uint8_t position) {
    // Inside :has(), a compound with no class, id, attribute or tag could
    // match any inserted element
    bool keyed = false;
    auto mark = [&](std::unordered_map<std::string, uint8_t>& map, const std::string& name) {
        if (name.empty()) return;
        map[name] |= position;
        keyed = true;
    };
    struct HasKeyCheck {
        SelectorFeatures& f;
        uint8_t position;
        bool& keyed;
        ~HasKeyCheck() {
            if ((position & Has) && !keyed) f.hasArgumentUniversal_ = true;
        }
    } check{*this, position, keyed};
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c == '.') {
            i++;
            mark(classes_, readIdent(s, i));
        } else if (c == '#') {
            i++;
            mark(ids_, readIdent(s, i));
        } else if (c == '[') {
            size_t end = skipGroup(s, i);
            size_t j = i + 1;
            while (j < end && isSpace(s[j])) j++;
            std::string name = readIdent(s, j);
            // [ns|name], [*|name]
            if (j < end && s[j] == '|' && (j + 1 >= end || s[j + 1] != '=')) {
                j++;
                name = readIdent(s, j);
            } else if (name.empty() && j < end && s[j] == '*') {
                j++;
                if (j < end && s[j] == '|') j++;
                name = readIdent(s, j);
            }
            mark(attributes_, lowerAscii(name));
            i = end;
        } else if (c == ':') {
            bool element = i + 1 < s.size() && s[i + 1] == ':';
            i += element ? 2 : 1;
            std::string name = lowerAscii(readIdent(s, i));
            std::string args;
            if (i < s.size() && s[i] == '(') {
                size_t end = skipGroup(s, i);
                args = s.substr(i + 1, end > i + 2 ? end - i - 2 : 0);
                i = end;
            }
            if (element) {
                // ::slotted(x) styles the slotted element; ::part(x) a part
                if (name == "slotted" && !args.empty()) {
                    for (const auto& sel : splitSelectorList(args)) addComplex(sel, position);
                } else if (name == "part") {
                    mark(attributes_, "part");
                }
                continue;
            }
            // The legacy one-colon pseudo-elements
            if (name == "before" || name == "after" || name == "first-line" || name == "first-letter") continue;
            mark(pseudoClasses_, name);
            if (isStructuralPseudo(name)) structural_ = true;
            if (name == "has") {
                has_ = true;
                // The elements :has() styles: those with this compound's key
                if (!hasSubjects_.addSelectorList(s)) hasSubjectUniversal_ = true;
                for (const auto& sel : splitSelectorList(args)) {
                    // Relative selectors start with a combinator: "> .a", "+ .b"
                    size_t a = 0;
                    while (a < sel.size() && (isSpace(sel[a]) || sel[a] == '>' || sel[a] == '+' || sel[a] == '~')) a++;
                    addComplex(sel.substr(a), Has);
                }
            } else if (name == "not" || name == "is" || name == "where" || name == "matches" ||
                       name == "-webkit-any" || name == "-moz-any" || name == "host" || name == "host-context" ||
                       name == "nth-child" || name == "nth-last-child") {
                std::string list = args;
                if (name == "nth-child" || name == "nth-last-child") {
                    // An+B of S
                    size_t of = lowerAscii(args).find(" of ");
                    list = of == std::string::npos ? std::string() : args.substr(of + 4);
                }
                // host-context() looks at the host's ancestors
                uint8_t inner = name == "host-context" ? widest(static_cast<uint8_t>(position | Ancestor)) : position;
                for (const auto& sel : splitSelectorList(list)) addComplex(sel, inner);
            } else if (name == "dir" || name == "lang") {
                // Inherited from the nearest ancestor that says
                mark(attributes_, name);
                attributes_[name] |= widest(static_cast<uint8_t>(position | Ancestor));
            } else if (name == "link" || name == "any-link" || name == "visited" || name == "local-link") {
                mark(attributes_, "href");
            }
        } else if (c == '*' || isIdentChar(static_cast<unsigned char>(c)) || c == '\\' || c == '|') {
            // The type selector (it never changes on an element)
            if (c == '*' || c == '|') {
                i++;
            } else {
                std::string tag = lowerAscii(readIdent(s, i));
                if ((position & Has) && !tag.empty()) {
                    hasTags_.insert(tag);
                    keyed = true;
                }
            }
        } else if (c == '&') {
            // Nesting: the parent rule's selector is not known here
            opaque_ = true;
            i++;
        } else {
            i++;
        }
    }
}

// ============================================================================
// Subject keys
// ============================================================================

bool SubjectKeys::addSelectorList(const std::string& list) {
    for (const std::string& selector : splitSelectorList(list)) {
        // The subject compound: after the last top-level combinator or space
        size_t start = 0;
        for (size_t i = 0; i < selector.size(); i++) {
            char c = selector[i];
            if (c == '\\') { i++; continue; }
            if (c == '(' || c == '[' || c == '"' || c == '\'') { i = skipGroup(selector, i) - 1; continue; }
            if (isSpace(c) || c == '>' || c == '+' || c == '~') start = i + 1;
        }
        std::string compound = selector.substr(start);
        std::string id, cls, attr, tag;
        size_t i = 0;
        while (i < compound.size()) {
            char c = compound[i];
            if (c == '#') {
                i++;
                std::string v = readIdent(compound, i);
                if (id.empty()) id = v;
            } else if (c == '.') {
                i++;
                std::string v = readIdent(compound, i);
                if (cls.empty()) cls = v;
            } else if (c == '[') {
                size_t end = skipGroup(compound, i);
                size_t j = i + 1;
                while (j < end && isSpace(compound[j])) j++;
                std::string v = lowerAscii(readIdent(compound, j));
                if (attr.empty() && j < end && compound[j] != '|') attr = v;
                i = end;
            } else if (c == ':') {
                bool element = i + 1 < compound.size() && compound[i + 1] == ':';
                i += element ? 2 : 1;
                std::string name = lowerAscii(readIdent(compound, i));
                if (i < compound.size() && compound[i] == '(') i = skipGroup(compound, i);
                if (!element && name == "root" && tag.empty()) tag = "html";
            } else if (i == 0 && isIdentChar(static_cast<unsigned char>(c))) {
                tag = lowerAscii(readIdent(compound, i));
                if (i < compound.size() && compound[i] == '|') {   // ns|tag
                    i++;
                    tag = lowerAscii(readIdent(compound, i));
                }
            } else {
                i++;
            }
        }
        if (!id.empty()) ids_.insert(id);
        else if (!cls.empty()) classes_.insert(cls);
        else if (!attr.empty()) attributes_.insert(attr);
        else if (!tag.empty()) tags_.insert(tag);
        else return false;
    }
    return true;
}

bool SubjectKeys::mayMatch(const DOMElement* element) const {
    if (!element) return false;
    if (!tags_.empty() && tags_.count(lowerAscii(element->tagName()))) return true;
    if (!ids_.empty()) {
        auto it = element->attributes().find("id");
        if (it != element->attributes().end() && ids_.count(it->second)) return true;
    }
    if (!attributes_.empty()) {
        for (const auto& [name, value] : element->attributes()) {
            if (attributes_.count(lowerAscii(name))) return true;
        }
    }
    if (!classes_.empty()) {
        auto it = element->attributes().find("class");
        if (it != element->attributes().end()) {
            const std::string& v = it->second;
            size_t i = 0;
            while (i < v.size()) {
                while (i < v.size() && isSpace(v[i])) i++;
                size_t j = i;
                while (j < v.size() && !isSpace(v[j])) j++;
                if (j > i && classes_.count(v.substr(i, j - i))) return true;
                i = j;
            }
        }
    }
    return false;
}

// ============================================================================
// StyleResolver: the sheet list and the cached styles
// ============================================================================

namespace {

// A sheet's selector features, its keys, and whether it can be added or
// removed without restyling everything (no layers, keyframes, @property)
struct SheetInfo {
    SelectorFeatures features;
    SubjectKeys keys;
    bool keyed = true;
    bool local = true;
};

void scanSheet(const CSSStyleSheet* sheet, SheetInfo& info) {
    if (!sheet) return;
    if (!sheet->propertyRegistrations().empty()) info.local = false;
    CSSRuleList* rules = sheet->cssRules();
    if (!rules) return;
    for (size_t i = 0; i < rules->length(); i++) {
        const CSSRule* rule = rules->item(i);
        if (!rule) continue;
        if (rule->type() == CSSRuleType::Keyframes) info.local = false;
        auto* style = dynamic_cast<const CSSStyleRule*>(rule);
        if (!style) continue;
        if (!style->layer().empty()) info.local = false;
        const std::string selector = style->selectorText();
        info.features.addSelectorList(selector);
        if (info.keyed && !info.keys.addSelectorList(selector)) info.keyed = false;
        // content: attr(x) reads an attribute of the element
        if (const CSSStyleDeclaration* decl = style->style()) {
            std::string content = decl->getPropertyValue("content");
            for (size_t at = content.find("attr("); at != std::string::npos; at = content.find("attr(", at + 5)) {
                size_t j = at + 5;
                while (j < content.size() && isSpace(content[j])) j++;
                std::string name = lowerAscii(readIdent(content, j));
                if (!name.empty()) info.features.addSelectorList("[" + name + "]");
            }
        }
    }
}

}  // namespace

void SubjectKeys::merge(const SubjectKeys& other) {
    classes_.insert(other.classes_.begin(), other.classes_.end());
    ids_.insert(other.ids_.begin(), other.ids_.end());
    tags_.insert(other.tags_.begin(), other.tags_.end());
    attributes_.insert(other.attributes_.begin(), other.attributes_.end());
}

void SelectorFeatures::merge(const SelectorFeatures& other) {
    hasSubjects_.merge(other.hasSubjects_);
    hasTags_.insert(other.hasTags_.begin(), other.hasTags_.end());
    hasSubjectUniversal_ = hasSubjectUniversal_ || other.hasSubjectUniversal_;
    hasArgumentUniversal_ = hasArgumentUniversal_ || other.hasArgumentUniversal_;
    for (const auto& [k, v] : other.classes_) classes_[k] |= v;
    for (const auto& [k, v] : other.ids_) ids_[k] |= v;
    for (const auto& [k, v] : other.attributes_) attributes_[k] |= v;
    for (const auto& [k, v] : other.pseudoClasses_) pseudoClasses_[k] |= v;
    structural_ = structural_ || other.structural_;
    siblingCombinators_ = siblingCombinators_ || other.siblingCombinators_;
    has_ = has_ || other.has_;
    opaque_ = opaque_ || other.opaque_;
}

StyleResolver::SheetChange StyleResolver::takeSheetChange() {
    SheetChange change;
    if (shadowScopesChanged_) {
        shadowScopesChanged_ = false;
        featuresValid_ = false;
        change.full = true;
    }
    auto same = [](const std::vector<StyleSheetEntry>& a, const std::vector<StyleSheetEntry>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            if (a[i].sheet != b[i].sheet || a[i].origin != b[i].origin) return false;
        }
        return true;
    };
    if (same(stylesheets_, styledSheets_)) return change;
    change.any = true;
    featuresValid_ = false;
    std::vector<StyleSheetEntry> before = std::move(styledSheets_);
    styledSheets_ = stylesheets_;
    if (change.full) return change;

    // The sheets common to both lists must keep their order; the others
    // came or went. A sheet listed twice is not worth the bookkeeping.
    std::unordered_map<const CSSStyleSheet*, int> inBefore, inNow;
    for (const auto& e : before) if (++inBefore[e.sheet.get()] > 1) { change.full = true; return change; }
    for (const auto& e : styledSheets_) if (++inNow[e.sheet.get()] > 1) { change.full = true; return change; }
    std::vector<std::pair<const CSSStyleSheet*, StyleOrigin>> keptBefore, keptNow;
    std::vector<const StyleSheetEntry*> changed;
    for (const auto& e : before) {
        if (inNow.count(e.sheet.get())) keptBefore.push_back({e.sheet.get(), e.origin});
        else changed.push_back(&e);
    }
    for (const auto& e : styledSheets_) {
        if (inBefore.count(e.sheet.get())) keptNow.push_back({e.sheet.get(), e.origin});
        else changed.push_back(&e);
    }
    if (keptBefore != keptNow) {
        change.full = true;
        return change;
    }
    for (const StyleSheetEntry* entry : changed) {
        if (entry->origin != StyleOrigin::Author) {
            change.full = true;
            return change;
        }
        SheetInfo info;
        scanSheet(entry->sheet.get(), info);
        if (!info.keyed || !info.local) {
            change.full = true;
            return change;
        }
        // The keys of every changed sheet go into one set
        CSSRuleList* rules = entry->sheet ? entry->sheet->cssRules() : nullptr;
        for (size_t i = 0; rules && i < rules->length(); i++) {
            if (auto* style = dynamic_cast<const CSSStyleRule*>(rules->item(i))) {
                change.keys.addSelectorList(style->selectorText());
            }
        }
    }
    return change;
}

const SelectorFeatures& StyleResolver::selectorFeatures() {
    if (featuresValid_) return features_;
    features_.clear();
    // Each sheet's features are worked out once (sheets are shared between
    // style passes, and pages add sheets one at a time)
    std::unordered_map<const CSSStyleSheet*, SheetFeatures> kept;
    auto add = [&](const std::shared_ptr<CSSStyleSheet>& sheet) {
        if (!sheet) return;
        auto it = sheetFeatures_.find(sheet.get());
        SheetFeatures entry;
        if (it != sheetFeatures_.end()) {
            entry = it->second;
        } else {
            SheetInfo info;
            scanSheet(sheet.get(), info);
            entry.sheet = sheet;
            entry.features = std::make_shared<SelectorFeatures>(std::move(info.features));
        }
        features_.merge(*entry.features);
        kept[sheet.get()] = std::move(entry);
    };
    for (const auto& e : stylesheets_) add(e.sheet);
    for (const auto& [root, scope] : shadowScopes_) {
        if (!scope) continue;
        for (const auto& sheet : scope->sheets) add(sheet);
    }
    sheetFeatures_ = std::move(kept);
    featuresValid_ = true;
    return features_;
}

size_t StyleResolver::invalidateSubtree(DOMElement* root) {
    size_t dropped = 0;
    std::vector<DOMNode*> stack{root};
    while (!stack.empty()) {
        DOMNode* node = stack.back();
        stack.pop_back();
        if (node->nodeType() == NodeType::Element) {
            auto* element = static_cast<DOMElement*>(node);
            dropped += styleCache_.erase(element);
            pseudoStyleCache_.erase(element);
            if (DOMShadowRoot* shadow = element->shadowRoot()) stack.push_back(shadow);
            // A slot's assigned nodes inherit from it (the flat tree)
            if (shadowRootsExist() && element->tagName() == "slot") {
                DOMNode* top = element;
                while (top->parentNode()) top = top->parentNode();
                if (DOMShadowRoot* shadow = asShadowRoot(top)) {
                    if (DOMElement* host = shadow->host()) {
                        for (const auto& child : host->childNodes()) {
                            if (assignedSlot(child.get()) == element) stack.push_back(child.get());
                        }
                    }
                }
            }
        }
        for (const auto& child : node->childNodes()) {
            if (child->nodeType() == NodeType::Element) stack.push_back(child.get());
        }
    }
    return dropped;
}

void StyleResolver::pruneStyles(const std::unordered_set<const DOMElement*>& live) {
    for (auto it = styleCache_.begin(); it != styleCache_.end();) {
        it = live.count(it->first) ? std::next(it) : styleCache_.erase(it);
    }
    for (auto it = pseudoStyleCache_.begin(); it != pseudoStyleCache_.end();) {
        it = live.count(it->first) ? std::next(it) : pseudoStyleCache_.erase(it);
    }
}

// ============================================================================
// CSSEngine: the style pass
// ============================================================================

namespace {

// Attributes the cascade reads besides selectors: presentational hints
// (width=, bgcolor=, ...) and the style attribute; a change restyles the
// element (and its subtree, which may inherit)
bool isStyleBearingAttribute(const std::string& name) {
    static const std::unordered_set<std::string> kNames = {
        "style", "align", "valign", "bgcolor", "background", "border", "bordercolor", "cellpadding",
        "cellspacing", "color", "face", "size", "width", "height", "hspace", "vspace", "nowrap",
        "text", "link", "vlink", "alink", "marginheight", "marginwidth", "leftmargin", "topmargin",
        "rightmargin", "bottommargin", "hidden", "dir", "lang", "type", "frameborder", "start",
        "reversed", "clear", "noshade", "rules", "frame", "char", "span", "cols", "rows",
    };
    return kNames.count(name) > 0;
}

// The pseudo-classes whose match an attribute (or a state change) affects
const std::vector<std::string>& pseudoClassesOf(const std::string& name) {
    static const std::unordered_map<std::string, std::vector<std::string>> kMap = {
        {"checked", {"checked", "default", "indeterminate"}},
        {"selected", {"checked", "default"}},
        {"disabled", {"disabled", "enabled", "read-only", "read-write", "valid", "invalid"}},
        {"readonly", {"read-only", "read-write", "valid", "invalid"}},
        {"required", {"required", "optional", "valid", "invalid", "user-valid", "user-invalid"}},
        {"value", {"placeholder-shown", "valid", "invalid", "in-range", "out-of-range", "user-valid",
                   "user-invalid", "blank", "autofill", "checked", "indeterminate", "default"}},
        {"min", {"valid", "invalid", "in-range", "out-of-range"}},
        {"max", {"valid", "invalid", "in-range", "out-of-range"}},
        {"step", {"valid", "invalid"}},
        {"pattern", {"valid", "invalid"}},
        {"minlength", {"valid", "invalid"}},
        {"maxlength", {"valid", "invalid"}},
        {"multiple", {"valid", "invalid"}},
        {"type", {"checked", "default", "indeterminate", "placeholder-shown", "valid", "invalid",
                  "in-range", "out-of-range", "read-only", "read-write", "required", "optional"}},
        {"placeholder", {"placeholder-shown"}},
        {"open", {"open", "closed", "popover-open", "modal"}},
        {"popover", {"popover-open"}},
        {"popover-open", {"popover-open", "open"}},
        {"href", {"link", "any-link", "visited", "local-link"}},
        {"contenteditable", {"read-only", "read-write"}},
        {"id", {"target", "target-within"}},
        {"defined", {"defined"}},
        {"hover", {"hover"}},
        {"active", {"active"}},
        {"focus", {"focus", "focus-visible", "focus-within"}},
    };
    static const std::vector<std::string> kNone;
    auto it = kMap.find(name);
    return it == kMap.end() ? kNone : it->second;
}

std::vector<std::string> classTokens(const std::string& value) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < value.size()) {
        while (i < value.size() && isSpace(value[i])) i++;
        size_t j = i;
        while (j < value.size() && !isSpace(value[j])) j++;
        if (j > i) out.push_back(value.substr(i, j - i));
        i = j;
    }
    return out;
}

// Children of a node, then its shadow tree, depth first (not template contents)
template <typename F>
void walkTree(DOMNode* root, F&& visit) {
    std::vector<DOMNode*> stack{root};
    while (!stack.empty()) {
        DOMNode* node = stack.back();
        stack.pop_back();
        if (!visit(node)) continue;
        if (node->nodeType() == NodeType::Element) {
            if (DOMShadowRoot* shadow = static_cast<DOMElement*>(node)->shadowRoot()) stack.push_back(shadow);
        }
        const auto& children = node->childNodes();
        for (auto it = children.rbegin(); it != children.rend(); ++it) stack.push_back(it->get());
    }
}

// The element's parent for styles: its parent element, or a shadow root's host
DOMElement* styleParent(DOMNode* node) {
    DOMNode* parent = node ? node->parentNode() : nullptr;
    if (!parent) return nullptr;
    if (parent->nodeType() == NodeType::Element) return static_cast<DOMElement*>(parent);
    if (DOMShadowRoot* shadow = asShadowRoot(parent)) return shadow->host();
    return nullptr;
}

bool envVar(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}

}  // namespace

bool CSSEngine::styleStatsEnabled() {
    static const bool on = envVar("ZEPRA_STYLE_STATS");
    return on;
}

bool selectorMatcherEvaluatesHas();   // css_cascade.cpp

void CSSEngine::applyStyleInvalidations() {
    if (!document_) return;
    auto started = std::chrono::steady_clock::now();
    StyleRecalcStats& stats = resolver_.stats();
    stats.passes++;
    static const bool alwaysFull = envVar("ZEPRA_FULL_RESTYLE");

    StyleChangeLog log = takeStyleChanges(document_);
    const uint64_t version = domMutationVersion();
    const uint64_t described = describedStyleChangeCount();
    // A version step nobody described (a bare noteDomMutation()) could be anything
    const bool undescribed = version - styledVersion_ > described - styledDescribed_;
    styledVersion_ = version;
    styledDescribed_ = described;
    const MediaEnvironment& env = mediaEnvironment();
    // Viewport units and media queries follow the viewport
    const bool viewportChanged = !styledOnce_ || env != styledEnvironment_;
    styledEnvironment_ = env;
    styledOnce_ = true;
    StyleResolver::SheetChange sheets = resolver_.takeSheetChange();

    auto finish = [&](bool full, size_t dropped) {
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        stats.invalidationMs += ms;
        if (full) stats.fullPasses++;
        else if (dropped || !log.changes.empty() || sheets.any) stats.targetedPasses++;
        stats.stylesDropped += dropped;
        lastPassFull_ = full;
        lastPassDropped_ = dropped;
    };
    lastPassReason_ = alwaysFull ? "forced" : log.full ? "log" : undescribed ? "undescribed change"
                    : viewportChanged ? "viewport" : sheets.full ? "sheets" : "";
    if (alwaysFull || log.full || undescribed || viewportChanged || sheets.full) {
        size_t cached = resolver_.cachedStyleCount();
        resolver_.invalidateAll();
        finish(true, cached);
        return;
    }
    if (log.changes.empty() && !sheets.any) {
        finish(false, 0);
        return;
    }

    const SelectorFeatures& features = resolver_.selectorFeatures();
    // While the matcher takes :has() as matching, DOM changes cannot change it
    const bool hasMatters = features.hasSelector() && selectorMatcherEvaluatesHas();

    // Which recorded nodes are still in the document (only those are followed)
    std::unordered_set<const DOMNode*> targets;
    for (const StyleChange& c : log.changes) {
        if (c.node) targets.insert(c.node);
        if (c.parent) targets.insert(c.parent);
    }
    std::unordered_set<const DOMNode*> live;
    size_t elementCount = 0;
    walkTree(document_, [&](DOMNode* node) {
        if (node->nodeType() == NodeType::Element) elementCount++;
        if (targets.count(node)) live.insert(node);
        return true;
    });

    bool full = false;
    const char* why = "";
    std::unordered_set<DOMElement*> roots;
    auto asElement = [](DOMNode* n) -> DOMElement* {
        return n && n->nodeType() == NodeType::Element ? static_cast<DOMElement*>(n) : nullptr;
    };
    // The element and its subtree
    auto restyle = [&](DOMNode* node) {
        if (DOMElement* e = asElement(node)) roots.insert(e);
        else if (DOMShadowRoot* shadow = asShadowRoot(node)) { if (shadow->host()) roots.insert(shadow->host()); }
        else if (node) { full = true; why = "a change at the document"; }
    };
    // The element's parent's children (sibling positions)
    auto restyleSiblings = [&](DOMNode* node) {
        DOMNode* parent = node ? node->parentNode() : nullptr;
        if (parent) restyle(parent);
    };
    // A change :has() can see restyles the elements :has() styles among
    // `from` and its ancestors (and their earlier siblings, for :has(+ x))
    auto restyleHasSubjects = [&](DOMNode* from) {
        if (features.hasSubjectUniversal()) {
            full = true;
            why = ":has() on a compound without a key";
            return;
        }
        for (DOMElement* a = asElement(from) ? asElement(from) : styleParent(from); a; a = styleParent(a)) {
            if (features.hasSubjects().mayMatch(a)) roots.insert(a);
            if (!features.siblingCombinators()) continue;
            if (DOMNode* parent = a->parentNode()) {
                for (const auto& sibling : parent->childNodes()) {
                    if (sibling.get() == a) break;
                    DOMElement* s = asElement(sibling.get());
                    if (s && features.hasSubjects().mayMatch(s)) roots.insert(s);
                }
            }
        }
    };
    auto byPosition = [&](DOMElement* e, uint8_t positions) {
        if ((positions & SelectorFeatures::Has) && hasMatters) {
            if (DOMNode* parent = e->parentNode()) restyleHasSubjects(parent);
        }
        if (positions & SelectorFeatures::Sibling) restyleSiblings(e);
        else if (positions & (SelectorFeatures::Subject | SelectorFeatures::Ancestor)) roots.insert(e);
    };
    // Whether an inserted subtree holds an element some :has() argument names
    auto subtreeMayAffectHas = [&](DOMNode* root) {
        bool found = false;
        walkTree(root, [&](DOMNode* node) {
            if (found) return false;
            if (DOMElement* e = asElement(node); e && features.mayAffectHas(e)) found = true;
            return !found;
        });
        return found;
    };
    auto pseudoPositions = [&](const std::string& name) {
        uint8_t p = features.pseudoClassPositions(name);
        for (const std::string& pseudo : pseudoClassesOf(name)) p |= features.pseudoClassPositions(pseudo);
        return p;
    };
    const bool structure = features.structural() || features.siblingCombinators();

    for (const StyleChange& c : log.changes) {
        if (full) break;
        switch (c.kind) {
        case StyleChange::Kind::ChildInserted: {
            if (!live.count(c.parent)) break;
            const bool element = live.count(c.node) && c.node->nodeType() == NodeType::Element;
            if (element) {
                // A node that moved here keeps no style computed for its old place
                roots.insert(static_cast<DOMElement*>(c.node));
                if (hasMatters && subtreeMayAffectHas(c.node)) restyleHasSubjects(c.parent);
            }
            if (element ? structure : features.structural()) restyle(c.parent);
            break;
        }
        case StyleChange::Kind::ChildRemoved:
            if (!live.count(c.parent)) break;
            // The removed subtree may be gone: assume :has() saw it
            if (hasMatters && (!c.node || !live.count(c.node) || c.node->nodeType() == NodeType::Element)) {
                restyleHasSubjects(c.parent);
            }
            if (structure) restyle(c.parent);
            break;
        case StyleChange::Kind::Attribute: {
            DOMElement* e = live.count(c.node) ? asElement(c.node) : nullptr;
            if (!e) break;
            const std::string name = lowerAscii(c.name);
            uint8_t positions = features.attributePositions(name);
            if (name == "class") {
                std::vector<std::string> before = classTokens(c.oldValue);
                std::vector<std::string> after = classTokens(e->getAttribute("class"));
                std::sort(before.begin(), before.end());
                std::sort(after.begin(), after.end());
                std::vector<std::string> flipped;
                std::set_symmetric_difference(before.begin(), before.end(), after.begin(), after.end(),
                                              std::back_inserter(flipped));
                for (const std::string& token : flipped) positions |= features.classPositions(token);
            } else if (name == "id") {
                positions |= features.idPositions(c.oldValue) | features.idPositions(e->getAttribute("id"));
            }
            for (const std::string& pseudo : pseudoClassesOf(name)) positions |= features.pseudoClassPositions(pseudo);
            if (isStyleBearingAttribute(name)) positions |= SelectorFeatures::Subject;
            if (features.opaque()) positions |= SelectorFeatures::Sibling;
            byPosition(e, positions);
            break;
        }
        case StyleChange::Kind::State: {
            DOMElement* e = live.count(c.node) ? asElement(c.node) : nullptr;
            if (!e) break;
            uint8_t positions = pseudoPositions(c.name);
            if (features.opaque()) positions |= SelectorFeatures::Sibling;
            byPosition(e, positions);
            break;
        }
        case StyleChange::Kind::Interaction: {
            // The elements whose state flipped: those on one chain only
            std::vector<DOMElement*> before, after;
            for (DOMElement* e = live.count(c.node) ? asElement(c.node) : nullptr; e; e = styleParent(e)) before.push_back(e);
            for (DOMElement* e = live.count(c.parent) ? asElement(c.parent) : nullptr; e; e = styleParent(e)) after.push_back(e);
            std::unordered_set<DOMElement*> inBefore(before.begin(), before.end()), inAfter(after.begin(), after.end());
            uint8_t positions = pseudoPositions(c.name);
            if (features.opaque()) positions |= SelectorFeatures::Sibling;
            if (!positions) break;
            for (DOMElement* e : before) if (!inAfter.count(e)) byPosition(e, positions);
            for (DOMElement* e : after) if (!inBefore.count(e)) byPosition(e, positions);
            break;
        }
        case StyleChange::Kind::Subtree:
            if (live.count(c.node)) restyle(c.node);
            break;
        case StyleChange::Kind::Text:
            if (features.structural() && live.count(c.parent)) restyle(c.parent);
            break;
        }
    }
    // The root's font size is what rem is of: a restyled root restyles all
    if (DOMElement* html = document_->documentElement(); html && roots.count(html)) {
        full = true;
        why = "the root element restyles";
    }
    if (full) {
        lastPassReason_ = why;
        size_t cached = resolver_.cachedStyleCount();
        resolver_.invalidateAll();
        finish(true, cached);
        return;
    }

    // Drop the styles under each root (a root inside another one's subtree
    // is covered by it), and those of elements a changed sheet may match
    size_t dropped = 0;
    const bool keyed = sheets.any && !sheets.keys.empty();
    walkTree(document_, [&](DOMNode* node) {
        DOMElement* e = asElement(node);
        if (!e) return true;
        if (roots.count(e) || (keyed && sheets.keys.mayMatch(e))) {
            dropped += resolver_.invalidateSubtree(e);
            return false;
        }
        return true;
    });
    // Styles of elements no longer in the document: dropped now and then
    if (resolver_.cachedStyleCount() > elementCount + elementCount / 2 + 256) {
        std::unordered_set<const DOMElement*> elements;
        elements.reserve(elementCount);
        walkTree(document_, [&](DOMNode* node) {
            if (DOMElement* e = asElement(node)) elements.insert(e);
            return true;
        });
        resolver_.pruneStyles(elements);
    }
    finish(false, dropped);
    if (envVar("ZEPRA_STYLE_VERIFY")) verifyTargetedPass(log.changes);
}

// ZEPRA_STYLE_VERIFY=1 (debugging): after a targeted pass, every element's
// style is compared with a restyle from scratch; differences are logged
// with the changes the pass saw
void CSSEngine::verifyTargetedPass(const std::vector<StyleChange>& changes) {
    std::vector<std::pair<DOMElement*, std::vector<std::pair<std::string, std::string>>>> kept;
    walkTree(document_, [&](DOMNode* node) {
        if (node->nodeType() != NodeType::Element) return true;
        auto* e = static_cast<DOMElement*>(node);
        if (const CSSComputedStyle* style = resolver_.getComputedStyle(e)) kept.push_back({e, computedStyleProperties(*style)});
        return true;
    });
    resolver_.invalidateAll();
    int reported = 0;
    for (const auto& [element, values] : kept) {
        const CSSComputedStyle* fresh = resolver_.getComputedStyle(element);
        if (!fresh) continue;
        auto now = computedStyleProperties(*fresh);
        if (now == values) continue;
        std::string diff;
        for (size_t i = 0; i < now.size() && i < values.size(); i++) {
            if (now[i] != values[i]) diff += " " + now[i].first + ": " + values[i].second + " -> " + now[i].second;
        }
        if (reported++ < 10) {
            std::cerr << "[StyleVerify] stale <" << element->tagName() << " id=" << element->getAttribute("id")
                      << " class=" << element->getAttribute("class").substr(0, 80) << ">" << diff.substr(0, 300) << std::endl;
        }
    }
    if (reported) {
        std::cerr << "[StyleVerify] " << reported << " stale styles after a pass with " << changes.size() << " changes:";
        for (size_t i = 0; i < changes.size() && i < 12; i++) {
            const StyleChange& c = changes[i];
            std::cerr << " " << (int)c.kind << ":" << c.name;
        }
        std::cerr << std::endl;
    }
}

} // namespace Zepra::WebCore
