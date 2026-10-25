// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file devtools_panel.hpp
 * @brief The browser's docked developer tools: Inspector, Console, Network.
 *
 * The panel owns its state (selection, expansion, scroll positions, console
 * input) and draws itself with primitives the browser lends it, reading the
 * active page's DOM, computed styles, layout boxes, script context, console
 * log and network monitor directly. It never changes the page.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "input/events.h"

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
    std::vector<std::string>* consoleLog = nullptr;   ///< Tab::consoleLog ("[level] message" lines)
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

    // Layout of the last draw (for hit testing)
    Rect panel_, toolbar_, pickerButton_, closeButton_, tree_, styles_, consoleList_, consoleInputBox_,
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

    // Network
    float networkScroll_ = 0, networkMax_ = 0;
    int networkSelected_ = -1;

    void syncWithDocument(const Page& page);
    void rebuildRows(const Page& page);
    void drawToolbar(const Painter& p, const Page& page, float mouseX, float mouseY);
    void drawInspector(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY);
    void drawTree(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY);
    void drawStyles(const Painter& p, const Page& page, const Rect& area);
    void drawConsole(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY);
    void drawNetwork(const Painter& p, const Page& page, const Rect& area, float mouseX, float mouseY);
    void evaluateConsoleInput(const Page& page);
    Zepra::WebCore::DOMElement* elementForStyles() const;
};

} // namespace ZepraBrowser::DevTools
