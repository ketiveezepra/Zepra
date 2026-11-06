// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file panel_internal.hpp
 * @brief What the DevTools panel's source files share: the palette, metrics
 *        and the DOM-tree helpers (not part of the panel's public interface).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "devtools_panel.hpp"

namespace Zepra::WebCore { class DOMNode; class DOMElement; }

namespace ZepraBrowser::DevTools {

// Firefox-dark-theme palette
inline constexpr uint32_t kBg          = 0x1C1B22;
inline constexpr uint32_t kBgAlt       = 0x202029;
inline constexpr uint32_t kToolbar     = 0x2B2A33;
inline constexpr uint32_t kBorder      = 0x3A3944;
inline constexpr uint32_t kText        = 0xFBFBFE;
inline constexpr uint32_t kMuted       = 0x9A9AA6;
inline constexpr uint32_t kAccent      = 0x00B3F4;
inline constexpr uint32_t kTag         = 0x75BFFF;
inline constexpr uint32_t kAttrName    = 0xFF7DE9;
inline constexpr uint32_t kAttrValue   = 0x86DE74;
inline constexpr uint32_t kComment     = 0x939393;
inline constexpr uint32_t kSelection   = 0x204E8A;
inline constexpr uint32_t kHover       = 0x2E2E38;
inline constexpr uint32_t kErrorBg     = 0x3B2222;
inline constexpr uint32_t kErrorText   = 0xFF8A8A;
inline constexpr uint32_t kWarnBg      = 0x3A3118;
inline constexpr uint32_t kWarnText    = 0xFFD36A;
inline constexpr uint32_t kResultText  = 0xB5CEA8;
inline constexpr uint32_t kInputText   = 0x9CDCFE;
inline constexpr uint32_t kOk          = 0x86DE74;

// Box-model colours (as Chrome/Firefox paint them over the page)
inline constexpr uint32_t kMarginColor  = 0xF6B26B;
inline constexpr uint32_t kBorderColor  = 0xFFE599;
inline constexpr uint32_t kPaddingColor = 0x93C47D;
inline constexpr uint32_t kContentColor = 0x6FA8DC;

inline constexpr float kToolbarH = 30;
inline constexpr float kRowH = 18;
inline constexpr float kFont = 12;
inline constexpr float kSmallFont = 11;
inline constexpr float kIndent = 14;
inline constexpr float kResizeGrip = 6;
inline constexpr float kMinHeight = 120;
inline constexpr float kRuleLineH = 16;
inline constexpr size_t kMaxHighlighted = 2000;

std::string lower(std::string s);
/// Collapses whitespace runs and trims; "" for blank text
std::string collapseText(const std::string& in);
std::string truncate(const std::string& s, size_t max);
float clampScroll(float v, float max);
/// The longest prefix of `s` that fits in `maxW`, with "..." when cut
std::string fitText(const Painter& p, const std::string& s, float size, float maxW);
LayoutBox* findBoxForElement(LayoutBox* box, const Zepra::WebCore::DOMElement* element);
/// Children the tree shows: elements, comments and non-blank text
std::vector<Zepra::WebCore::DOMNode*> shownChildren(const Zepra::WebCore::DOMNode* node);
/// An element drawn on one row: no shown children, or a single text child
bool isLeafRow(const Zepra::WebCore::DOMNode* element);
std::string elementSelector(const Zepra::WebCore::DOMElement* el);
/// The system clipboard (panel_clipboard.cpp)
void copyToClipboard(const std::string& text);
std::string clipboardText();
/// Attributes in the order the tree shows them: id, class, then by name
std::vector<std::pair<std::string, std::string>> sortedAttributes(const Zepra::WebCore::DOMElement* el);

} // namespace ZepraBrowser::DevTools
