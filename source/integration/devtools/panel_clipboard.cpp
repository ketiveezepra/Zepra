// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file panel_clipboard.cpp
 * @brief The system clipboard for the DevTools panel. Kept in its own file:
 *        clipboard.h brings in the platform headers (X11's `None`, windows.h)
 *        that clash with names in the panel's sources.
 */
#include "panel_internal.hpp"

#include "browser/clipboard.h"

namespace ZepraBrowser::DevTools {

void copyToClipboard(const std::string& text) {
    ZepraBrowser::Clipboard::instance().copy(text);
}

std::string clipboardText() {
    return ZepraBrowser::Clipboard::instance().paste();
}

} // namespace ZepraBrowser::DevTools
