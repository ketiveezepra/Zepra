// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file devtools_panel.hpp
 * @brief The browser's docked developer tools: Inspector, Console, Network, Sources.
 *
 * The panel owns its state (selection, expansion, scroll positions, console
 * input) and draws itself with primitives the browser lends it, reading the
 * active page's DOM, style rules, computed styles, layout boxes, script
 * context, console log and network monitor directly. It changes the page only
 * when a declaration is switched off in the Rules pane, through that element's
 * style attribute. The Network panel's "Disable cache" holds while the panel
 * is open (Zepra::WebCore::setCacheDisabled).
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "console_input.hpp"
#include "input/events.h"
#include "scripting/console_format.hpp"
#include "network_details.hpp"
#include "style_rules.hpp"
#include "text_field.hpp"

namespace Zepra::WebCore {
class DOMDocument;
class DOMNode;
class DOMElement;
class CSSEngine;
class ScriptContext;
}
namespace ZepraBrowser { struct LayoutBox; }
namespace zepra { class NetworkMonitor; }

namespace ZepraBrowser::DevTools {

/// Drawing primitives the browser lends the panel. Text is drawn at a baseline.
struct Painter {
    std::function<void(float x, float y, float w, float h, uint32_t rgb, uint8_t alpha)> rect;
    std::function<void(float x1, float y1, float x2, float y2, uint32_t rgb)> line;
    std::function<void(float x1, float y1, float x2, float y2, float x3, float y3, uint32_t rgb)> triangle;
    std::function<void(const std::string& text, float x, float baseline, uint32_t rgb, float size)> text;
    std::function<float(const std::string& text, float size)> textWidth;
    std::function<void(float x, float y, float w, float h)> pushClip;
    std::function<void()> popClip;
};

/// The active page as the browser holds it. Any pointer may be null.
struct Page {
    Zepra::WebCore::DOMDocument* document = nullptr;
    Zepra::WebCore::CSSEngine* css = nullptr;
    ZepraBrowser::LayoutBox* layoutRoot = nullptr;
    Zepra::WebCore::ScriptContext* script = nullptr;
    std::vector<Zepra::WebCore::ConsoleMessage>* consoleLog = nullptr;   ///< Tab::consoleLog (the panel expands and clears it)
    zepra::NetworkMonitor* network = nullptr;
    uint64_t domVersion = 0;                           ///< Zepra::WebCore::domMutationVersion()
};

enum class PanelTab { Inspector, Console, Network, Sources };

class DevToolsPanel {
public:
    DevToolsPanel();

    bool visible() const { return visible_; }
    void show() { visible_ = true; }
    void hide();
    void toggle();
    float height() const { return height_; }
    PanelTab activeTab() const { return tab_; }
    /// Showing the Console focuses its input, as Firefox does
    void setActiveTab(PanelTab tab) {
        tab_ = tab;
        consoleFocused_ = (tab == PanelTab::Console);
        filterFocused_ = false;
    }

    /// Draws the panel in the rectangle (x, y, w, h). Call every frame it is visible.
    void draw(const Painter& p, const Page& page, float x, float y, float w, float h,
              float mouseX, float mouseY, bool mouseDown);

    /// Draws the box-model highlight of the hovered / selected element over the page,
    /// clipped to the page's content area. Call after the page is painted.
    void drawPageOverlay(const Painter& p, const Page& page, float clipX, float clipY, float clipW, float clipH);

    /// Whether (x, y) is inside the panel as last drawn
    bool contains(float x, float y) const;

    // Element picker (Inspector): the browser reports the element under the mouse
    bool pickerActive() const { return picker_; }
    void setPickerActive(bool on) { picker_ = on; if (!on) pickHover_ = nullptr; }
    void pickerHover(Zepra::WebCore::DOMElement* element) { pickHover_ = element; }
    void pickerSelect(Zepra::WebCore::DOMElement* element);

    /// Selects a node in the Inspector (expands its ancestors, scrolls it into view)
    void selectNode(Zepra::WebCore::DOMNode* node);
    Zepra::WebCore::DOMNode* selectedNode() const { return selected_; }

    // Input. The mouse handlers return true when the event was inside the panel.
    bool onMouseDown(float x, float y, const Page& page);
    void onMouseUp(float x, float y);
    bool onMouseWheel(float x, float y, float delta);
    /// Right button: the Inspector tree's context menu. True when the press was inside the panel.
    bool onRightMouseDown(float x, float y, const Page& page);
    /// Keys while a text field of the panel (console input, console filter, Network
    /// filter, an Inspector edit field) or the Inspector tree has focus. False when
    /// the key is not for the panel.
    bool onKey(NXRender::KeyCode key, const std::string& text, bool ctrl, bool shift, const Page& page);
    /// Whether the console input or the console filter takes the keys
    bool consoleFocused() const { return consoleFocused_ || filterFocused_; }
    void blurConsole() { consoleFocused_ = false; filterFocused_ = false; completionOpen_ = false; }
    /// Pastes into the focused console field (the filter takes one line)
    void insertConsoleText(const std::string& text);
    /// A text field of the panel (console input or filter, Network filter, an Inspector
    /// edit field) has the keyboard
    bool textInputFocused() const {
        return consoleFocused() || networkFilterFocused_ || field_.kind != FieldKind::Closed;
    }
    /// Pastes into the focused text field
    void insertText(const std::string& text);
    /// The page was clicked: the panel lets go of the keyboard
    void blurInputs();

    /// Opens the Sources view on a script (or document) at a line, as a console link does
    void openSource(const std::string& url, uint32_t line);

private:
    struct Row {
        Zepra::WebCore::DOMNode* node = nullptr;
        int depth = 0;
        bool closing = false;      // the "</tag>" row of an expanded element
    };
    struct Rect { float x = 0, y = 0, w = 0, h = 0; bool contains(float px, float py) const; };
    /// One drawn line of the console: a message with its values inline, a
    /// property of an expanded value, or a further line of a multi-line text
    struct ConsoleRow {
        Zepra::WebCore::ConsoleMessage* message = nullptr;
        uint64_t messageId = 0;                         ///< message->id: rows are matched by it on a click
        Zepra::WebCore::ValuePreview* node = nullptr;   ///< the property shown (null on a message row)
        int depth = 0;                                  ///< indentation (group nesting + tree depth)
        std::string line;                               ///< a continuation line's text
        bool continuation = false;
        struct Toggle { size_t value; float x0, x1; };  ///< the triangle of message->values[value]
        std::vector<Toggle> toggles;                    ///< set while drawing a message row
        float linkX0 = 0, linkX1 = 0;                   ///< the source link, when it opens a source
    };

    /// A selector (declaration -1) or declaration line of the Rules pane as last drawn
    struct RuleHit {
        Rect r;
        int group = -1;          ///< -1: the element's own rules, otherwise ElementRules::inherited[group]
        int block = -1;
        int declaration = -1;    ///< -2: the block's closing line (click: add a declaration)
        float nameX0 = 0, nameX1 = 0;   ///< where the property name was drawn (value: after nameX1)
    };
    /// A row of the Computed pane as last drawn
    struct ComputedHit {
        Rect r;
        std::string property;
    };

    // Layout of the last draw (for hit testing)
    Rect panel_, toolbar_, pickerButton_, closeButton_, tree_, rules_, styles_, consoleList_, consoleInputBox_,
         consoleToolbar_, networkList_, networkToolbar_;
    std::vector<Rect> tabButtons_;
    std::vector<Rect> levelButtons_;
    Rect clearButton_, filterBox_, completionBox_, sourceList_, sourceView_;

    bool visible_ = false;
    float height_ = 320;
    PanelTab tab_ = PanelTab::Inspector;
    bool resizing_ = false;
    float resizeGrabOffset_ = 0;

    // Inspector
    Zepra::WebCore::DOMDocument* document_ = nullptr;   // document the state below belongs to
    uint64_t domVersion_ = 0;
    std::unordered_set<const Zepra::WebCore::DOMNode*> expanded_;
    Zepra::WebCore::DOMNode* selected_ = nullptr;
    Zepra::WebCore::DOMNode* hoveredRow_ = nullptr;
    bool picker_ = false;
    Zepra::WebCore::DOMElement* pickHover_ = nullptr;
    std::vector<Row> rows_;
    float treeScroll_ = 0, treeMax_ = 0;
    float stylesScroll_ = 0, stylesMax_ = 0;
    bool scrollSelectedIntoView_ = false;

    // Rules pane
    float rulesScroll_ = 0, rulesMax_ = 0;
    std::vector<RuleHit> ruleHits_;
    std::vector<ComputedHit> computedHits_;
    ElementRules rulesModel_;                          // rules of rulesElement_ (cached)
    Zepra::WebCore::DOMElement* rulesElement_ = nullptr;
    uint64_t rulesDomVersion_ = 0;
    size_t rulesSheetCount_ = 0;
    bool rulesDirty_ = true;
    /// Declarations switched off per element (applied through its style attribute)
    std::vector<std::pair<Zepra::WebCore::DOMElement*, StyleEdit>> styleEdits_;
    /// The rule (or element, for `element { }`) whose selector was clicked, and the
    /// elements it matches, highlighted on the page
    const void* highlightKey_ = nullptr;
    std::string highlightSelector_;                     // empty: highlightKey_ is the element
    std::vector<Zepra::WebCore::DOMElement*> highlighted_;

    // Console
    bool consoleFocused_ = false;
    ConsoleEditor input_;
    std::vector<std::string> consoleHistory_;
    int historyIndex_ = -1;
    std::string historyDraft_;              // what was typed before walking the history
    float inputScroll_ = 0;                 // first input line shown (in lines) when it has more than fit
    float consoleScroll_ = 0, consoleMax_ = 0;
    bool consoleStick_ = true;
    size_t consoleSeen_ = 0;
    int caretBlink_ = 0;
    std::vector<ConsoleRow> consoleRows_;   // the rows of the last draw (for hit testing)
    uint64_t nextConsoleId_ = 1;
    size_t consoleHidden_ = 0;              // messages the filter hid in the last draw

    // Console filter
    ConsoleFilter filter_;
    ConsoleFilterMatcher filterMatcher_;
    bool filterFocused_ = false;

    // Console autocomplete
    bool completionOpen_ = false;
    CompletionQuery completionQuery_;
    std::vector<std::string> completions_;  // ranked candidates for completionQuery_
    int completionIndex_ = 0;
    int completionTop_ = 0;                 // first candidate shown in the popup
    std::string completionPathKey_;         // the path completionNames_ belongs to
    std::vector<std::string> completionNames_;
    Zepra::WebCore::ScriptContext* completionScript_ = nullptr;

    // Sources
    std::string sourceUrl_;
    uint32_t sourceLine_ = 0;               // the line a link pointed at (highlighted)
    bool sourceScrollToLine_ = false;
    std::shared_ptr<const std::string> sourceText_;
    std::vector<size_t> sourceLineStarts_;
    float sourceScroll_ = 0, sourceMax_ = 0, sourceListScroll_ = 0;
    std::vector<std::pair<Rect, std::string>> sourceListHits_;
    std::function<float(const std::string&, float)> textWidth_;   // the painter's, for placing the caret on a click

    // Network
    enum class NetworkDetailTab { Headers, Response, Timings };
    float networkScroll_ = 0, networkMax_ = 0;
    uint64_t networkSelectedId_ = 0;    // request id shown in the details pane (0: none)
    std::vector<uint64_t> networkRowIds_;   // the rows shown, in order (for hit testing)
    int networkFilter_ = 0;             // index into kNetworkFilters (0 = all)
    std::vector<Rect> networkFilterButtons_;
    std::string networkFilterText_;
    bool networkFilterFocused_ = false;
    bool disableCache_ = false;
    Rect networkFilterBox_, disableCacheBox_, networkDetail_, networkDetailBody_, networkDetailClose_;
    std::vector<Rect> networkDetailTabs_;
    NetworkDetailTab networkDetailTab_ = NetworkDetailTab::Headers;
    float networkDetailScroll_ = 0, networkDetailMax_ = 0;
    // The Response tab's lines for one request at one width (rebuilt when either changes)
    uint64_t responseLinesId_ = 0;
    size_t responseLinesColumns_ = 0;
    std::vector<std::string> responseLines_;

    // Inspector editing (inspector_edit.cpp): one edit field at a time, tree keys,
    // the tree's context menu, markup search, forced pseudo-classes
    enum class FieldKind { Closed, Search, Attribute, NewAttribute, Text, Html, RuleName, RuleValue, RuleNew };
    struct EditField {
        FieldKind kind = FieldKind::Closed;
        TextField text;
        Zepra::WebCore::DOMNode* node = nullptr;   ///< the node edited (tree fields)
        std::string attribute;                     ///< the attribute edited (Attribute)
        RuleHit rule;                              ///< the declaration or block edited (Rule*)
        Rect box;                                  ///< where it was last drawn
    };
    /// An attribute (or a text row) of the tree as last drawn: double-click edits it
    struct TreeHit {
        Rect r;
        Zepra::WebCore::DOMNode* node = nullptr;
        std::string attribute;                     ///< empty: the node's text
    };
    struct MenuItem { std::string label; int action; bool checked; bool separator; };
    EditField field_;
    bool treeFocused_ = false;
    std::vector<TreeHit> treeHits_;
    bool menuOpen_ = false;
    float menuX_ = 0, menuY_ = 0;
    Zepra::WebCore::DOMNode* menuNode_ = nullptr;
    std::vector<MenuItem> menuItems_;
    std::vector<Rect> menuItemRects_;
    Rect menuRect_;
    Rect searchBox_;
    std::string searchQuery_;
    std::vector<Zepra::WebCore::DOMNode*> searchMatches_;
    int searchIndex_ = -1;
    uint64_t searchDomVersion_ = 0;
    Rect hovButton_;
    bool pseudoBarOpen_ = false;
    std::vector<Rect> pseudoBoxes_;
    double lastClickTime_ = 0;
    float lastClickX_ = -1000, lastClickY_ = -1000;

    bool isDoubleClick(float x, float y);
    void openField(FieldKind kind, std::string text, bool multiline);
    void closeField();
    void commitField(const Page& page);
    bool onFieldKey(NXRender::KeyCode key, const std::string& text, bool ctrl, bool shift, const Page& page);
    void drawField(const Painter& p, float x, float y, float w, float h);
    bool onTreeKey(NXRender::KeyCode key, bool ctrl, bool shift, const Page& page);
    bool onTreeClick(float x, float y, const Page& page);
    void drawSearchBar(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY);
    void runSearch(const Page& page, int step);
    void openMenu(Zepra::WebCore::DOMNode* node, float x, float y, const Page& page);
    void drawMenu(const Painter& p, float mouseX, float mouseY);
    void runMenuAction(int action, const Page& page);
    void drawPseudoBar(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY);
    bool onPseudoBarClick(float x, float y, const Page& page);
    void beginRuleEdit(const Page& page, const RuleHit& hit, FieldKind kind);
    void commitRuleField(const Page& page, FieldKind kind, const RuleHit& hit, const std::string& text);
    /// The element's StyleEdit, created from its current style attribute when it has none
    StyleEdit& styleEditOf(Zepra::WebCore::DOMElement* owner);
    /// Writes the element's StyleEdit into its style attribute (the original one when nothing is left)
    void applyStyleEdit(const Page& page, Zepra::WebCore::DOMElement* owner);
    void forgetNode(Zepra::WebCore::DOMNode* node);

    void syncWithDocument(const Page& page);
    void rebuildRows(const Page& page);
    void drawToolbar(const Painter& p, const Page& page, float mouseX, float mouseY);
    void drawInspector(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY);
    void drawTree(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY);
    void drawStyles(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY);
    void drawRules(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY);
    const ElementRules& rulesFor(const Page& page, Zepra::WebCore::DOMElement* element);
    const StyleEdit* styleEditFor(const Zepra::WebCore::DOMElement* element) const;
    void onRulesClick(const Page& page, float x, float y);
    void toggleDeclaration(const Page& page, const RuleHit& hit);
    void highlightRule(const Page& page, const RuleBlock& block);
    void refreshHighlight(const Page& page);
    void drawConsole(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY);
    void drawNetwork(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY);
    void drawNetworkDetails(const Painter& p, const zepra::NetworkEntry& entry, const Rect& area,
                            float mouseX, float mouseY);
    void selectNetworkRequest(uint64_t id);
    void drawSources(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY);
    void evaluateConsoleInput(const Page& page);
    void buildConsoleRows(const Page& page);
    bool onConsoleInputKey(NXRender::KeyCode key, const std::string& text, bool ctrl, bool shift, const Page& page);
    bool onFilterKey(NXRender::KeyCode key, const std::string& text);
    /// Recomputes the autocomplete candidates at the caret; `force` opens the
    /// popup with nothing typed (Ctrl+Space)
    void updateCompletions(const Page& page, bool force = false);
    void acceptCompletion();
    void setFilterText(std::string text);
    Zepra::WebCore::DOMElement* elementForStyles() const;
};

} // namespace ZepraBrowser::DevTools
