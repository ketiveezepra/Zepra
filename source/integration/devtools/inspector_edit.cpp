// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file inspector_edit.cpp
 * @brief The Inspector's editing: one edit field at a time (attributes, text,
 *        Edit as HTML, Rules names and values, the markup search box), tree
 *        keys, the tree's context menu and forced :hover/:active/:focus.
 *        The DOM and style changes themselves are in dom_edit.cpp and
 *        style_rules.cpp; this file is the panel side.
 */
#include "devtools_panel.hpp"
#include "panel_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "browser/dom.hpp"
#include "css/css_engine.hpp"
#include "dom_edit.hpp"

using namespace Zepra::WebCore;
using NXRender::KeyCode;

namespace ZepraBrowser::DevTools {

namespace {

enum MenuAction {
    kEditHtml, kAddAttribute, kEditText, kCopyOuterHtml, kCopySelector, kDuplicate, kDelete,
    kForceHover, kForceActive, kForceFocus,
};

constexpr float kSearchH = 26;
constexpr int kMaxHtmlLines = 12;

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

ForcedPseudo forcedFor(int action) {
    return action == kForceHover ? ForcedPseudo::Hover : action == kForceActive ? ForcedPseudo::Active
                                                                                : ForcedPseudo::Focus;
}

} // namespace

// ---------------------------------------------------------------------------
// Keyboard focus and the edit field
// ---------------------------------------------------------------------------

void DevToolsPanel::blurConsole() {
    consoleFocused_ = false;
    treeFocused_ = false;
    menuOpen_ = false;
    // A click on the page ends an edit, as in Firefox; the search keeps its text
    if (field_.kind != FieldKind::Closed && field_.kind != FieldKind::Search) closeField();
    else if (field_.kind == FieldKind::Search) field_.kind = FieldKind::Closed;
}

void DevToolsPanel::insertConsoleText(const std::string& text) {
    if (field_.kind != FieldKind::Closed) field_.text.insert(text);
    else consoleInputText_ += text;
}

bool DevToolsPanel::isDoubleClick(float x, float y) {
    double t = nowSeconds();
    bool twice = t - lastClickTime_ < 0.4 && std::abs(x - lastClickX_) < 5 && std::abs(y - lastClickY_) < 5;
    lastClickTime_ = twice ? 0 : t;   // a third click starts over
    lastClickX_ = x;
    lastClickY_ = y;
    return twice;
}

// Opens an empty field of `kind` holding `text` (all selected); the caller then
// says what it edits (node, attribute or rule)
void DevToolsPanel::openField(FieldKind kind, std::string text, bool multiline) {
    field_ = EditField{};
    field_.kind = kind;
    field_.text = TextField(std::move(text), multiline);
    consoleFocused_ = false;
    menuOpen_ = false;
}

void DevToolsPanel::closeField() {
    field_ = EditField{};
}

void DevToolsPanel::forgetNode(DOMNode* node) {
    if (!node) return;
    auto inside = [&](DOMNode* n) {
        for (DOMNode* p = n; p; p = p->parentNode()) if (p == node) return true;
        return false;
    };
    if (selected_ && inside(selected_)) selected_ = node->parentNode();
    if (field_.node && inside(field_.node)) closeField();
    if (menuNode_ && inside(menuNode_)) { menuNode_ = nullptr; menuOpen_ = false; }
    searchMatches_.erase(std::remove_if(searchMatches_.begin(), searchMatches_.end(), inside), searchMatches_.end());
    rulesDirty_ = true;
}

// Applies the field's text. The DOM edits bump domMutationVersion(), so the
// browser lays the page out again and the tree follows on the next draw.
void DevToolsPanel::commitField(const Page& page) {
    std::string text = field_.text.text();
    FieldKind kind = field_.kind;
    DOMNode* node = field_.node;
    auto* el = node && node->nodeType() == NodeType::Element ? static_cast<DOMElement*>(node) : nullptr;
    switch (kind) {
        case FieldKind::Search:
            return;
        case FieldKind::Attribute:
            if (el) replaceAttribute(el, field_.attribute, text);
            break;
        case FieldKind::NewAttribute:
            if (el) replaceAttribute(el, "", text);
            break;
        case FieldKind::Text:
            if (node && node->nodeType() == NodeType::Text) setText(static_cast<DOMText*>(node), text);
            break;
        case FieldKind::Html:
            if (node) {
                DOMNode* parent = node->parentNode();
                forgetNode(node);
                DOMNode* first = replaceWithHtml(node, text);
                selectNode(first ? first : parent);
            }
            break;
        case FieldKind::RuleName:
        case FieldKind::RuleValue:
        case FieldKind::RuleNew:
            commitRuleField(page, kind, field_.rule, text);
            break;
        case FieldKind::Closed:
            break;
    }
    rulesDirty_ = true;
    closeField();
}

bool DevToolsPanel::onFieldKey(KeyCode key, const std::string& text, bool ctrl, bool shift, const Page& page) {
    if (key == KeyCode::Enter && shift && field_.kind == FieldKind::Search) {
        runSearch(page, -1);
        return true;
    }
    switch (field_.text.onKey(key, text, ctrl, shift)) {
        case TextField::Result::Ignored:
            // Not a key for the field: the tree keeps its arrows, the browser its shortcuts
            return field_.kind != FieldKind::Search && !ctrl;
        case TextField::Result::Copy:
            if (field_.text.hasSelection()) copyToClipboard(field_.text.selectedText());
            return true;
        case TextField::Result::Cut:
            if (!field_.text.lastCut().empty()) copyToClipboard(field_.text.lastCut());
            break;
        case TextField::Result::Paste:
            field_.text.insert(clipboardText());
            break;
        case TextField::Result::Commit:
            if (field_.kind == FieldKind::Search) runSearch(page, +1);
            else commitField(page);
            return true;
        case TextField::Result::Cancel:
            if (field_.kind == FieldKind::Search) { field_.kind = FieldKind::Closed; treeFocused_ = true; }
            else closeField();
            return true;
        case TextField::Result::Next:
        case TextField::Result::Previous:
            // Tab from a name goes on to its value, as in Firefox's Rules view
            if (field_.kind == FieldKind::RuleName) {
                RuleHit hit = field_.rule;
                commitField(page);
                beginRuleEdit(page, hit, FieldKind::RuleValue);
            } else if (field_.kind != FieldKind::Search) {
                commitField(page);
            }
            return true;
        default:
            break;
    }
    if (field_.kind == FieldKind::Search && field_.text.text() != searchQuery_) {
        searchQuery_ = field_.text.text();
        runSearch(page, 0);
    }
    return true;
}

// The open field at (x, y, w, h): its text, the selection and the caret.
// A one-line field scrolls sideways to keep the caret in view.
void DevToolsPanel::drawField(const Painter& p, float x, float y, float w, float h) {
    field_.box = {x, y, w, h};
    p.rect(x, y, w, h, kBg, 255);
    p.rect(x, y, w, 1, kAccent, 255);
    p.rect(x, y + h - 1, w, 1, kAccent, 255);
    p.rect(x, y, 1, h, kAccent, 255);
    p.rect(x + w - 1, y, 1, h, kAccent, 255);
    p.pushClip(x + 1, y + 1, w - 2, h - 2);

    const TextField& t = field_.text;
    std::vector<std::string> lines = t.lines();
    if (lines.empty()) lines.push_back("");
    size_t caretLine = t.caretLine(), caretCol = t.caretColumn();
    float lineH = kRowH;
    float firstLine = 0;
    int visibleLines = std::max(1, (int)((h - 4) / lineH));
    if ((int)caretLine >= visibleLines) firstLine = (float)(caretLine - visibleLines + 1);
    float caretX = p.textWidth(lines[std::min(caretLine, lines.size() - 1)].substr(0, caretCol), kFont);
    float shift = t.multiline() ? 0 : std::max(0.0f, caretX - (w - 14));

    size_t selA = t.selectionStart(), selB = t.selectionEnd();
    size_t offset = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string& line = lines[i];
        float ly = y + 2 + (i - firstLine) * lineH;
        if (ly + lineH >= y && ly <= y + h) {
            size_t a = std::max(selA, offset), b = std::min(selB, offset + line.size());
            if (a < b) {
                float sx = p.textWidth(line.substr(0, a - offset), kFont);
                float sw = p.textWidth(line.substr(a - offset, b - a), kFont);
                p.rect(x + 6 + sx - shift, ly, std::max(2.0f, sw), lineH, kSelection, 255);
            }
            p.text(line, x + 6 - shift, ly + 13, kText, kFont);
        }
        offset += line.size() + 1;   // the "\n" between lines
    }
    if ((caretBlink_ / 30) % 2 == 0) {
        float cy = y + 2 + (caretLine - firstLine) * lineH;
        p.rect(x + 6 + caretX - shift, cy + 2, 1, lineH - 4, kText, 255);
    }
    p.popClip();
}

// ---------------------------------------------------------------------------
// Tree: clicks and keys
// ---------------------------------------------------------------------------

// A click in the tree: the triangle opens or closes, a row selects, a double
// click on an attribute or a text edits it, a double click on a tag edits it as HTML
bool DevToolsPanel::onTreeClick(float x, float y, const Page& page) {
    treeFocused_ = true;
    if (field_.kind != FieldKind::Closed && field_.box.contains(x, y)) return true;
    if (field_.kind != FieldKind::Closed && field_.kind != FieldKind::Search) commitField(page);
    else if (field_.kind == FieldKind::Search) field_.kind = FieldKind::Closed;

    int idx = (int)std::floor((y - (tree_.y + 2) + treeScroll_) / kRowH);
    if (idx < 0 || idx >= (int)rows_.size()) return true;
    const Row& row = rows_[idx];
    float arrowEnd = tree_.x + 8 + row.depth * kIndent + 12;
    bool element = row.node->nodeType() == NodeType::Element;
    if (element && !row.closing && x < arrowEnd && !isLeafRow(row.node)) {
        if (expanded_.count(row.node)) expanded_.erase(row.node);
        else expanded_.insert(row.node);
        return true;
    }
    bool twice = isDoubleClick(x, y) && selected_ == row.node;
    selected_ = row.node;
    if (!twice) return true;
    for (const TreeHit& hit : treeHits_) {
        if (!hit.r.contains(x, y)) continue;
        if (hit.node->nodeType() == NodeType::Text) {
            openField(FieldKind::Text, static_cast<DOMText*>(hit.node)->data(), false);
        } else if (!hit.attribute.empty()) {
            auto* el = static_cast<DOMElement*>(hit.node);
            openField(FieldKind::Attribute, hit.attribute + "=\"" + el->getAttribute(hit.attribute) + "\"", false);
            field_.attribute = hit.attribute;
        }
        field_.node = hit.node;
        return true;
    }
    if (element && !row.closing) {
        openField(FieldKind::Html, outerHtmlOf(row.node), true);
        field_.node = row.node;
    } else if (element) {
        if (expanded_.count(row.node)) expanded_.erase(row.node);
    }
    return true;
}

// Firefox's markup-view keys: arrows move and fold, F2 edits as HTML,
// Delete removes the node, Ctrl+F searches, Ctrl+C copies outerHTML
bool DevToolsPanel::onTreeKey(KeyCode key, bool ctrl, bool shift, const Page& page) {
    (void)shift;
    if (ctrl) {
        if (key == KeyCode::F) { openField(FieldKind::Search, searchQuery_, false); return true; }
        if (key == KeyCode::C && selected_) {
            copyToClipboard(outerHtmlOf(selected_));
            return true;
        }
        if (key == KeyCode::Z) {
            if (DOMNode* restored = undoLastEdit()) selectNode(restored);
            rulesDirty_ = true;
            return true;
        }
        return false;
    }
    int at = -1;
    for (size_t i = 0; i < rows_.size(); i++) {
        if (rows_[i].node == selected_ && !rows_[i].closing) { at = (int)i; break; }
    }
    auto selectRow = [&](int i) {
        while (i >= 0 && i < (int)rows_.size() && rows_[i].closing) i += (i > at ? 1 : -1);
        if (i >= 0 && i < (int)rows_.size()) { selected_ = rows_[i].node; scrollSelectedIntoView_ = true; }
    };
    bool element = selected_ && selected_->nodeType() == NodeType::Element;
    switch (key) {
        case KeyCode::Up:   if (at > 0) selectRow(at - 1); return true;
        case KeyCode::Down: if (at >= 0) selectRow(at + 1); else selectRow(0); return true;
        case KeyCode::Home: selectRow(0); return true;
        case KeyCode::End:  selectRow((int)rows_.size() - 1); return true;
        case KeyCode::Left:
            if (element && expanded_.count(selected_) && !isLeafRow(selected_)) expanded_.erase(selected_);
            else if (selected_ && selected_->parentNode() && selected_->parentNode()->nodeType() == NodeType::Element)
                selectNode(selected_->parentNode());
            return true;
        case KeyCode::Right:
            if (element && !isLeafRow(selected_)) {
                if (!expanded_.count(selected_)) expanded_.insert(selected_);
                else selectRow(at + 1);
            }
            return true;
        case KeyCode::Enter:
            if (element && !isLeafRow(selected_)) {
                if (expanded_.count(selected_)) expanded_.erase(selected_); else expanded_.insert(selected_);
            }
            return true;
        case KeyCode::F2:
            if (selected_) {
                openField(FieldKind::Html, outerHtmlOf(selected_), true);
                field_.node = selected_;
            }
            return true;
        case KeyCode::Delete:
        case KeyCode::Backspace:
            if (selected_) {
                DOMNode* gone = selected_;
                DOMNode* next = gone->nextSibling() ? gone->nextSibling() : gone->parentNode();
                forgetNode(gone);
                if (removeNode(gone)) selectNode(next);
            }
            return true;
        case KeyCode::Escape:
            treeFocused_ = false;
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Markup search
// ---------------------------------------------------------------------------

void DevToolsPanel::drawSearchBar(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY) {
    (void)mouseX; (void)mouseY;
    if (searchDomVersion_ != page.domVersion && !searchQuery_.empty()) runSearch(page, 0);
    p.rect(area.x, area.y, area.w, area.h, kToolbar, 255);
    p.rect(area.x, area.y + area.h - 1, area.w, 1, kBorder, 255);
    std::string count;
    if (!searchQuery_.empty()) {
        count = searchMatches_.empty() ? "No matches"
                                       : std::to_string(searchIndex_ + 1) + " of " + std::to_string(searchMatches_.size());
    }
    float countW = count.empty() ? 0 : p.textWidth(count, kSmallFont) + 16;
    searchBox_ = {area.x + 6, area.y + 4, area.w - 12 - countW, area.h - 8};
    if (field_.kind == FieldKind::Search) {
        drawField(p, searchBox_.x, searchBox_.y, searchBox_.w, searchBox_.h);
    } else {
        p.rect(searchBox_.x, searchBox_.y, searchBox_.w, searchBox_.h, kBg, 255);
        if (searchQuery_.empty()) p.text("Search HTML", searchBox_.x + 6, searchBox_.y + 13, kMuted, kFont);
        else p.text(fitText(p, searchQuery_, kFont, searchBox_.w - 12), searchBox_.x + 6, searchBox_.y + 13, kText, kFont);
    }
    if (!count.empty()) p.text(count, area.x + area.w - countW, area.y + 17, kMuted, kSmallFont);
}

// Finds the query again (step 0) or moves to the next (+1) / previous (-1) match
void DevToolsPanel::runSearch(const Page& page, int step) {
    if (step == 0 || searchDomVersion_ != page.domVersion) {
        DOMNode* current = searchIndex_ >= 0 && searchIndex_ < (int)searchMatches_.size() ? searchMatches_[searchIndex_] : nullptr;
        searchMatches_ = page.document && !searchQuery_.empty()
            ? searchMarkup(page.document, searchQuery_) : std::vector<DOMNode*>{};
        // A text drawn inside its element's row is found as that element
        for (DOMNode*& n : searchMatches_) {
            if (n->nodeType() == NodeType::Text && n->parentNode() && isLeafRow(n->parentNode())) n = n->parentNode();
        }
        searchMatches_.erase(std::unique(searchMatches_.begin(), searchMatches_.end()), searchMatches_.end());
        searchDomVersion_ = page.domVersion;
        auto it = std::find(searchMatches_.begin(), searchMatches_.end(), current);
        searchIndex_ = it != searchMatches_.end() ? (int)(it - searchMatches_.begin()) : (searchMatches_.empty() ? -1 : 0);
        if (step == 0 && searchIndex_ >= 0) selectNode(searchMatches_[searchIndex_]);
    }
    if (step != 0 && !searchMatches_.empty()) {
        int n = (int)searchMatches_.size();
        searchIndex_ = ((searchIndex_ < 0 ? 0 : searchIndex_ + step) % n + n) % n;
        selectNode(searchMatches_[searchIndex_]);
    }
}

// ---------------------------------------------------------------------------
// Context menu
// ---------------------------------------------------------------------------

void DevToolsPanel::openMenu(DOMNode* node, float x, float y, const Page& page) {
    (void)page;
    menuNode_ = node;
    menuItems_.clear();
    bool element = node->nodeType() == NodeType::Element;
    auto* el = element ? static_cast<DOMElement*>(node) : nullptr;
    auto add = [&](const std::string& label, int action, bool checked = false) {
        menuItems_.push_back({label, action, checked, false});
    };
    auto separator = [&] { menuItems_.push_back({"", -1, false, true}); };
    if (element) {
        add("Edit As HTML", kEditHtml);
        add("Add Attribute", kAddAttribute);
    } else if (node->nodeType() == NodeType::Text) {
        add("Edit Text", kEditText);
    }
    add("Copy outerHTML", kCopyOuterHtml);
    if (element) add("Copy CSS Selector", kCopySelector);
    separator();
    add("Duplicate Node", kDuplicate);
    add("Delete Node", kDelete);
    if (element) {
        separator();
        unsigned forced = forcedPseudoClasses(el);
        add(":hover", kForceHover, forced & (unsigned)ForcedPseudo::Hover);
        add(":active", kForceActive, forced & (unsigned)ForcedPseudo::Active);
        add(":focus", kForceFocus, forced & (unsigned)ForcedPseudo::Focus);
    }
    menuX_ = x;
    menuY_ = y;
    menuOpen_ = true;
}

void DevToolsPanel::drawMenu(const Painter& p, float mouseX, float mouseY) {
    if (!menuOpen_) return;
    float w = 180, itemH = 20, sepH = 7;
    float h = 6;
    for (const MenuItem& item : menuItems_) h += item.separator ? sepH : itemH;
    // Inside the panel, below and right of the pointer when there is room
    float x = std::min(menuX_, panel_.x + panel_.w - w - 4);
    float y = menuY_ + h <= panel_.y + panel_.h ? menuY_ : std::max(panel_.y + kToolbarH, menuY_ - h);
    menuRect_ = {x, y, w, h};
    p.rect(x + 2, y + 2, w, h, 0x000000, 90);   // shadow
    p.rect(x, y, w, h, kToolbar, 255);
    p.rect(x, y, w, 1, kBorder, 255); p.rect(x, y + h - 1, w, 1, kBorder, 255);
    p.rect(x, y, 1, h, kBorder, 255); p.rect(x + w - 1, y, 1, h, kBorder, 255);
    menuItemRects_.clear();
    float iy = y + 3;
    for (const MenuItem& item : menuItems_) {
        if (item.separator) {
            p.rect(x + 6, iy + 3, w - 12, 1, kBorder, 255);
            menuItemRects_.push_back({0, 0, 0, 0});
            iy += sepH;
            continue;
        }
        Rect r{x + 1, iy, w - 2, itemH};
        if (r.contains(mouseX, mouseY)) p.rect(r.x, r.y, r.w, r.h, kSelection, 255);
        if (item.checked) p.text("x", x + 10, iy + 14, kAccent, kFont);
        p.text(item.label, x + 24, iy + 14, kText, kFont);
        menuItemRects_.push_back(r);
        iy += itemH;
    }
}

void DevToolsPanel::runMenuAction(int action, const Page& page) {
    DOMNode* node = menuNode_;
    menuOpen_ = false;
    if (!node || !document_ || !document_->contains(node)) return;
    auto* el = node->nodeType() == NodeType::Element ? static_cast<DOMElement*>(node) : nullptr;
    switch (action) {
        case kEditHtml:
            openField(FieldKind::Html, outerHtmlOf(node), true);
            field_.node = node;
            break;
        case kAddAttribute:
            openField(FieldKind::NewAttribute, "", false);
            field_.node = node;
            expanded_.insert(node);
            break;
        case kEditText:
            openField(FieldKind::Text, static_cast<DOMText*>(node)->data(), false);
            field_.node = node;
            break;
        case kCopyOuterHtml:
            copyToClipboard(outerHtmlOf(node));
            break;
        case kCopySelector:
            if (el) copyToClipboard(uniqueSelector(el));
            break;
        case kDuplicate:
            if (DOMNode* copy = duplicateNode(node)) selectNode(copy);
            break;
        case kDelete: {
            DOMNode* next = node->nextSibling() ? node->nextSibling() : node->parentNode();
            forgetNode(node);
            if (removeNode(node)) selectNode(next);
            break;
        }
        case kForceHover:
        case kForceActive:
        case kForceFocus:
            if (el) {
                ForcedPseudo which = forcedFor(action);
                bool on = !(forcedPseudoClasses(el) & (unsigned)which);
                setForcedPseudoClass(el, which, on);
            }
            break;
    }
    rulesDirty_ = true;
    (void)page;
}

bool DevToolsPanel::onRightMouseDown(float x, float y, const Page& page) {
    if (!visible_ || !panel_.contains(x, y)) {
        menuOpen_ = false;
        return false;
    }
    menuOpen_ = false;
    if (tab_ != PanelTab::Inspector || !tree_.contains(x, y)) return true;
    int idx = (int)std::floor((y - (tree_.y + 2) + treeScroll_) / kRowH);
    if (idx < 0 || idx >= (int)rows_.size()) return true;
    if (field_.kind != FieldKind::Closed && field_.kind != FieldKind::Search) commitField(page);
    selected_ = rows_[idx].node;
    treeFocused_ = true;
    openMenu(selected_, x, y, page);
    return true;
}

// ---------------------------------------------------------------------------
// Forced pseudo-classes (":hov" in the Rules header)
// ---------------------------------------------------------------------------

void DevToolsPanel::drawPseudoBar(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY) {
    (void)page;
    pseudoBoxes_.clear();
    p.rect(area.x, area.y, area.w, area.h, kBgAlt, 255);
    p.rect(area.x, area.y + area.h - 1, area.w, 1, kBorder, 255);
    DOMElement* el = elementForStyles();
    unsigned forced = el ? forcedPseudoClasses(el) : 0;
    const char* labels[] = {":hover", ":active", ":focus"};
    ForcedPseudo kinds[] = {ForcedPseudo::Hover, ForcedPseudo::Active, ForcedPseudo::Focus};
    float x = area.x + 12;
    for (int i = 0; i < 3; i++) {
        float labelW = p.textWidth(labels[i], kSmallFont);
        Rect r{x - 2, area.y + 2, 16 + labelW + 4, area.h - 4};
        if (r.contains(mouseX, mouseY)) p.rect(r.x, r.y, r.w, r.h, kHover, 255);
        float cx = x, cy = area.y + area.h / 2 - 5;
        p.rect(cx, cy, 10, 1, kMuted, 255); p.rect(cx, cy + 9, 10, 1, kMuted, 255);
        p.rect(cx, cy, 1, 10, kMuted, 255); p.rect(cx + 9, cy, 1, 10, kMuted, 255);
        if (forced & (unsigned)kinds[i]) p.rect(cx + 2, cy + 2, 6, 6, kAccent, 255);
        p.text(labels[i], x + 16, area.y + area.h / 2 + 4, kText, kSmallFont);
        pseudoBoxes_.push_back(r);
        x += r.w + 14;
    }
}

bool DevToolsPanel::onPseudoBarClick(float x, float y, const Page& page) {
    (void)page;
    DOMElement* el = elementForStyles();
    if (!el) return false;
    ForcedPseudo kinds[] = {ForcedPseudo::Hover, ForcedPseudo::Active, ForcedPseudo::Focus};
    for (size_t i = 0; i < pseudoBoxes_.size() && i < 3; i++) {
        if (!pseudoBoxes_[i].contains(x, y)) continue;
        bool on = !(forcedPseudoClasses(el) & (unsigned)kinds[i]);
        setForcedPseudoClass(el, kinds[i], on);
        rulesDirty_ = true;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Rules: names, values, new declarations
// ---------------------------------------------------------------------------

void DevToolsPanel::beginRuleEdit(const Page& page, const RuleHit& hit, FieldKind kind) {
    DOMElement* el = elementForStyles();
    if (!el) return;
    const ElementRules& rules = rulesFor(page, el);
    const RuleBlock* block = rules.block({hit.group, hit.block, 0});
    if (!block || block->kind == RuleKind::Hints) return;
    std::string text;
    if (kind != FieldKind::RuleNew) {
        const RuleDeclaration* d = rules.declaration({hit.group, hit.block, hit.declaration});
        if (!d) return;
        text = kind == FieldKind::RuleName ? d->property : d->value + (d->important ? " !important" : "");
    }
    openField(kind, text, false);
    field_.rule = hit;
}

StyleEdit& DevToolsPanel::styleEditOf(DOMElement* owner) {
    auto it = std::find_if(styleEdits_.begin(), styleEdits_.end(), [&](const auto& e) { return e.first == owner; });
    if (it != styleEdits_.end()) return it->second;
    StyleEdit edit;
    edit.hadStyle = owner->hasAttribute("style");
    edit.originalStyle = owner->getAttribute("style");
    styleEdits_.push_back({owner, std::move(edit)});
    return styleEdits_.back().second;
}

void DevToolsPanel::applyStyleEdit(const Page& page, DOMElement* owner) {
    auto it = std::find_if(styleEdits_.begin(), styleEdits_.end(), [&](const auto& e) { return e.first == owner; });
    if (it == styleEdits_.end() || !page.css) return;
    StyleEdit& edit = it->second;
    if (edit.empty()) {
        // Nothing changed any more: the original attribute, exactly
        if (edit.hadStyle) owner->setAttribute("style", edit.originalStyle);
        else owner->removeAttribute("style");
        styleEdits_.erase(it);
    } else {
        edit.writtenStyle = styleAttributeFor(*page.css, owner, edit);
        owner->setAttribute("style", edit.writtenStyle);
    }
    rulesDirty_ = true;
}

// A Rules field's text applied to the element the block belongs to (an inherited
// block's ancestor); the shared stylesheets are never changed
void DevToolsPanel::commitRuleField(const Page& page, FieldKind kind, const RuleHit& hit, const std::string& text) {
    DOMElement* el = elementForStyles();
    if (!el || !page.css) return;
    const ElementRules& rules = rulesFor(page, el);
    const RuleBlock* block = rules.block({hit.group, hit.block, 0});
    if (!block || block->kind == RuleKind::Hints || !block->element) return;
    DOMElement* owner = block->element;
    const CSSStyleRule* rule = block->kind == RuleKind::Style ? block->rule : nullptr;
    std::string property;
    if (kind != FieldKind::RuleNew) {
        const RuleDeclaration* d = rules.declaration({hit.group, hit.block, hit.declaration});
        if (!d) return;
        property = d->property;
    }
    StyleEdit& edit = styleEditOf(owner);
    switch (kind) {
        case FieldKind::RuleValue:
            setDeclarationValue(edit, rule, property, text);
            break;
        case FieldKind::RuleName:
            renameDeclaration(edit, rule, property, text);
            break;
        case FieldKind::RuleNew: {
            // "name: value" (a name alone waits for its value: nothing is added)
            size_t colon = text.find(':');
            if (colon == std::string::npos) break;
            auto trim = [](std::string v) {
                size_t a = v.find_first_not_of(" \t"), b = v.find_last_not_of(" \t;");
                return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
            };
            addDeclaration(edit, rule, trim(text.substr(0, colon)), trim(text.substr(colon + 1)));
            break;
        }
        default:
            break;
    }
    applyStyleEdit(page, owner);
}

} // namespace ZepraBrowser::DevTools
