// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#pragma once

#include "web/box/box_tree.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <cstdint>

namespace NXRender {
namespace Web {

// ==================================================================
// Table Layout (CSS 2.1 §17)
// ==================================================================

class TableLayout {
public:
    struct Column {
        float minWidth = 0;
        float maxWidth = 0;
        float fixedWidth = 0;
        float percentWidth = 0;
        float resolvedWidth = 0;
        bool hasFixed = false;
        bool hasPercent = false;
        int span = 1;
    };

    struct Row {
        float height = 0;
        float minHeight = 0;
        float fixedHeight = 0;
        bool hasFixedHeight = false;
        std::vector<BoxNode*> cells;
    };

    struct Cell {
        BoxNode* node = nullptr;
        int rowIndex = 0, colIndex = 0;
        int rowSpan = 1, colSpan = 1;
        float minContentWidth = 0;
        float maxContentWidth = 0;
        float minContentHeight = 0;
    };

    // Build table structure from box tree
    void buildTable(BoxNode* tableBox);

    // Fixed table layout algorithm (CSS 2.1 §17.5.2.1)
    void layoutFixed(float availableWidth);

    // Auto table layout algorithm (CSS 2.1 §17.5.2.2)
    void layoutAuto(float availableWidth);

    // Distribute column widths
    void distributeWidth(float availableWidth);

    // Compute row heights after column layout
    void computeRowHeights();

    // Position cells
    void positionCells(float tableX, float tableY);

    // Table properties
    float borderSpacingH = 0;
    float borderSpacingV = 0;
    bool borderCollapse = false;

    // Accessors
    int columnCount() const { return static_cast<int>(columns_.size()); }
    int rowCount() const { return static_cast<int>(rows_.size()); }
    float totalWidth() const;
    float totalHeight() const;

private:
    std::vector<Column> columns_;
    std::vector<Row> rows_;
    std::vector<Cell> cells_;
    BoxNode* tableBox_ = nullptr;

    // Collect cells from table row groups (thead, tbody, tfoot, tr)
    void collectCells(BoxNode* node, int& currentRow);

    // Intrinsic width computation
    void computeIntrinsicWidths();

    // Collapsed border resolution
    float resolveCollapsedBorder(BoxNode* cell1, BoxNode* cell2, bool horizontal) const;
};

// ==================================================================
// Pseudo-element renderer (::before, ::after, ::marker)
// ==================================================================

class PseudoElementRenderer {
public:
    // Generate ::before pseudo-element box for a node
    static std::unique_ptr<BoxNode> generateBefore(BoxNode* parent, const ComputedValues& pseudoStyle);

    // Generate ::after pseudo-element box for a node
    static std::unique_ptr<BoxNode> generateAfter(BoxNode* parent, const ComputedValues& pseudoStyle);

    // Generate ::marker pseudo-element for list items
    static std::unique_ptr<BoxNode> generateMarker(BoxNode* listItem, int ordinal);

    // Generate ::first-line pseudo-element styling
    static void applyFirstLine(BoxNode* block, const ComputedValues& firstLineStyle);

    // Generate ::first-letter pseudo-element
    static std::unique_ptr<BoxNode> generateFirstLetter(BoxNode* block, const ComputedValues& letterStyle);

    // Generate ::placeholder pseudo-element for inputs
    static std::unique_ptr<BoxNode> generatePlaceholder(BoxNode* input, const std::string& text);

    // Generate ::selection pseudo-element styling (applied during paint)
    struct SelectionStyle {
        uint32_t color = 0xFFFFFFFF;
        uint32_t background = 0x3390FFFF;
    };
    static SelectionStyle getSelectionStyle(const BoxNode* node);

    // Process content property value
    // Handles: "string", url(), attr(), counter(), counters(), open-quote, close-quote
    static std::string processContent(const std::string& contentValue, const BoxNode* parent);

private:
    // Parse content() functions
    static std::string parseContentFunction(const std::string& func, const BoxNode* parent);

    // Quote nesting level
    static int quoteDepth_;
};

// ==================================================================
// CSS Counters (CSS Lists Level 3)
// ==================================================================

class CSSCounterManager {
public:
    static CSSCounterManager& instance();

    // counter-reset: creates/resets a counter
    void resetCounter(const std::string& name, int value = 0);

    // counter-increment: increments a counter
    void incrementCounter(const std::string& name, int amount = 1);

    // counter-set: sets a counter to a value
    void setCounter(const std::string& name, int value);

    // counter(name) / counter(name, style)
    std::string counterValue(const std::string& name, const std::string& style = "decimal") const;

    // counters(name, separator) / counters(name, separator, style)
    std::string countersValue(const std::string& name, const std::string& separator,
                                 const std::string& style = "decimal") const;

    // Push/pop scope (for nested lists)
    void pushScope();
    void popScope();

    // Format a number according to list-style-type
    static std::string formatNumber(int value, const std::string& style);

    // Get list item marker text
    static std::string markerText(int ordinal, const std::string& listStyleType);

private:
    CSSCounterManager() = default;

    struct CounterScope {
        std::unordered_map<std::string, int> counters;
    };
    std::vector<CounterScope> scopes_;

    int getCounterValue(const std::string& name) const;

    // Number formatting helpers
    static std::string toRoman(int value, bool upper);
    static std::string toAlpha(int value, bool upper);
    static std::string toGreek(int value);
    static std::string toCJK(int value);
};

// ==================================================================
// Text effects (text-decoration, text-shadow, box-shadow)
// ==================================================================

struct TextDecoration {
    enum class Line : uint8_t { None = 0, Underline = 1, Overline = 2, LineThrough = 4 };
    enum class Style : uint8_t { Solid, Double, Dotted, Dashed, Wavy };

    uint8_t lines = 0;          // Bitmask of Line values
    Style style = Style::Solid;
    uint32_t color = 0;         // RGBA
    float thickness = 1;

    bool hasUnderline() const { return lines & static_cast<uint8_t>(Line::Underline); }
    bool hasOverline() const { return lines & static_cast<uint8_t>(Line::Overline); }
    bool hasLineThrough() const { return lines & static_cast<uint8_t>(Line::LineThrough); }

    static TextDecoration parse(const std::string& value);
};

struct Shadow {
    float offsetX = 0;
    float offsetY = 0;
    float blurRadius = 0;
    float spreadRadius = 0;
    uint32_t color = 0x00000080;
    bool inset = false;

    static std::vector<Shadow> parse(const std::string& value);
};

class TextEffects {
public:
    // Compute text-decoration positions
    struct DecorationLine {
        float x, y, width;
        float thickness;
        TextDecoration::Style style;
        uint32_t color;
    };

    static std::vector<DecorationLine> computeDecorations(
        const BoxNode* textNode, float baselineY, float fontSize);

    // Compute text-shadow render params
    struct ShadowParams {
        float offsetX, offsetY, blurRadius;
        uint32_t color;
    };
    static std::vector<ShadowParams> computeTextShadows(const BoxNode* node);

    // Compute box-shadow render params
    static std::vector<Shadow> computeBoxShadows(const BoxNode* node);

    // text-overflow: ellipsis handling
    static std::string applyTextOverflow(const std::string& text, float maxWidth,
                                             float fontSize, const std::string& overflow);

    // word-break / overflow-wrap
    enum class WordBreak { Normal, BreakAll, KeepAll, BreakWord };
    static WordBreak parseWordBreak(const std::string& value);

    // hyphens
    static bool shouldHyphenate(const std::string& word, const std::string& lang);
    static std::vector<int> hyphenationPoints(const std::string& word, const std::string& lang);

    // text-transform
    static std::string applyTextTransform(const std::string& text, const std::string& transform);

    // letter-spacing / word-spacing
    static float computeLetterSpacing(const ComputedValues& cv);
    static float computeWordSpacing(const ComputedValues& cv);

    // writing-mode
    enum class WritingMode { HorizontalTB, VerticalRL, VerticalLR };
    static WritingMode parseWritingMode(const std::string& value);
};

// ==================================================================
// Image/Media pipeline
// ==================================================================

class ImageRenderer {
public:
    // object-fit modes
    enum class ObjectFit : uint8_t { Fill, Contain, Cover, None, ScaleDown };

    // Compute the drawn rect for an image within its box
    struct DrawRect {
        float srcX, srcY, srcW, srcH; // Source region
        float dstX, dstY, dstW, dstH; // Destination region
    };

    static DrawRect computeObjectFit(ObjectFit fit,
                                        float imageWidth, float imageHeight,
                                        float boxWidth, float boxHeight,
                                        float objectPositionX = 0.5f,
                                        float objectPositionY = 0.5f);

    static ObjectFit parseObjectFit(const std::string& value);

    // object-position parsing
    struct ObjectPosition {
        float x = 0.5f; // 0..1 (percentage)
        float y = 0.5f;
    };
    static ObjectPosition parseObjectPosition(const std::string& value);

    // aspect-ratio property
    struct AspectRatio {
        float ratio = 0;    // width/height, 0 = auto
        bool hasRatio = false;
    };
    static AspectRatio parseAspectRatio(const std::string& value);

    // Compute size with aspect-ratio constraint
    static Size computeSizeWithAspectRatio(float availableW, float availableH,
                                              const AspectRatio& ratio,
                                              const ComputedValues& cv);

    // Image loading states
    enum class LoadState { Idle, Loading, Loaded, Error };

    // Lazy loading
    enum class Loading { Eager, Lazy };
    static Loading parseLoading(const std::string& value);

    // Image decode priority
    enum class Decoding { Auto, Sync, Async };
    static Decoding parseDecoding(const std::string& value);
};

// ==================================================================
// CSS background compositor (multiple backgrounds support)
// ==================================================================

class BackgroundCompositor {
public:
    struct BackgroundLayer {
        std::string image;      // url(), linear-gradient(), etc.
        uint32_t color = 0;

        // background-position
        float positionX = 0;    // percentage (0..1)
        float positionY = 0;

        // background-size
        enum class SizeMode : uint8_t { Auto, Cover, Contain, Explicit };
        SizeMode sizeMode = SizeMode::Auto;
        float sizeWidth = 0;
        float sizeHeight = 0;

        // background-repeat
        enum class Repeat : uint8_t { RepeatBoth, RepeatX, RepeatY, NoRepeat, Space, Round };
        Repeat repeat = Repeat::RepeatBoth;

        // background-origin / background-clip
        enum class Box : uint8_t { BorderBox, PaddingBox, ContentBox };
        Box origin = Box::PaddingBox;
        Box clip = Box::BorderBox;

        // background-attachment
        enum class Attachment : uint8_t { Scroll, Fixed, Local };
        Attachment attachment = Attachment::Scroll;

        // background-blend-mode
        std::string blendMode = "normal";
    };

    // Parse background shorthand (supports multiple layers)
    static std::vector<BackgroundLayer> parse(const std::string& background);

    // Parse individual background properties
    static void parsePosition(const std::string& value, float& x, float& y);
    static void parseSize(const std::string& value, BackgroundLayer& layer);
    static BackgroundLayer::Repeat parseRepeat(const std::string& value);

    // Compute rendering rect for a background layer
    struct BackgroundRect {
        float originX, originY, originW, originH;
        float clipX, clipY, clipW, clipH;
        float tileW, tileH;
        float posX, posY;
        BackgroundLayer::Repeat repeat;
    };
    static BackgroundRect computeLayout(const BackgroundLayer& layer,
                                           const BoxNode* node,
                                           float imageW, float imageH);
};

// ==================================================================
// CSS Outline (separate from border — doesn't consume layout space)
// ==================================================================

struct Outline {
    float width = 0;
    uint32_t color = 0;
    float offset = 0;
    std::string style; // solid, dotted, dashed, etc.

    static Outline parse(const ComputedValues& cv);
    bool isVisible() const { return width > 0 && style != "none"; }
};

// ==================================================================
// Cursor management
// ==================================================================

class CursorManager {
public:
    enum class CursorType : uint8_t {
        Auto, Default, None, ContextMenu, Help, Pointer, Progress, Wait,
        Cell, Crosshair, Text, VerticalText,
        Alias, Copy, Move, NoDrop, NotAllowed, Grab, Grabbing,
        EResize, NResize, NEResize, NWResize, SResize, SEResize, SWResize, WResize,
        EWResize, NSResize, NESWResize, NWSEResize,
        ColResize, RowResize, AllScroll, ZoomIn, ZoomOut
    };

    static CursorType parseCursor(const std::string& value);
    static CursorType cursorForNode(const BoxNode* node);
};

} // namespace Web
} // namespace NXRender
