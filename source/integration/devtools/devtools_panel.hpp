// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file devtools_panel.hpp
 * @brief The browser's docked developer tools: Inspector, Console, Network.
 *
 * The panel owns its state (selection, expansion, scroll positions, console
 * input) and draws itself with primitives the browser lends it, reading the
 * active page's DOM, style rules, computed styles, layout boxes, script
 * context, console log and network monitor directly. It changes the page only
 * when a declaration is switched off in the Rules pane, through that element's
 * style attribute.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "input/events.h"
#include "scripting/console_format.hpp"
#include "style_rules.hpp"

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

enum class PanelTab { Inspector, Console, Network };

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
    void setActiveTab(PanelTab tab) { tab_ = tab; consoleFocused_ = (tab == PanelTab::Console); }

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
    /// Keys while the console input has focus. False when the key is not for the panel.
    bool onKey(NXRender::KeyCode key, const std::string& text, bool ctrl, bool shift, const Page& page);
    bool consoleFocused() const { return consoleFocused_; }
    void blurConsole() { consoleFocused_ = false; }
    void insertConsoleText(const std::string& text) { consoleInputText_ += text; }

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
    };

    /// A selector (declaration -1) or declaration line of the Rules pane as last drawn
    struct RuleHit {
        Rect r;
        int group = -1;          ///< -1: the element's own rules, otherwise ElementRules::inherited[group]
        int block = -1;
        int declaration = -1;
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
    Rect clearButton_;

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
    std::string consoleInputText_;
    std::vector<std::string> consoleHistory_;
    int historyIndex_ = -1;
    float consoleScroll_ = 0, consoleMax_ = 0;
    bool consoleStick_ = true;
    size_t consoleSeen_ = 0;
    bool showErrors_ = true, showWarnings_ = true, showLogs_ = true;
    int caretBlink_ = 0;
    std::vector<ConsoleRow> consoleRows_;   // the rows of the last draw (for hit testing)
    uint64_t nextConsoleId_ = 1;

    // Network
    float networkScroll_ = 0, networkMax_ = 0;
    int networkSelected_ = -1;          // index into the rows shown (after the type filter)
    int networkFilter_ = 0;             // index into kNetworkFilters (0 = all)
    std::vector<Rect> networkFilterButtons_;

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
    void evaluateConsoleInput(const Page& page);
    void buildConsoleRows(const Page& page);
    Zepra::WebCore::DOMElement* elementForStyles() const;
};

} // namespace ZepraBrowser::DevTools
