// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file macos_clipboard.h
 * @brief System clipboard (NSPasteboard) access for C++ code on macOS.
 *
 * Implemented in macos_cocoa.mm. Strings are UTF-8.
 */

#pragma once

#ifdef __APPLE__

#include <string>

namespace NXRender {

bool macClipboardSetText(const std::string& text);
std::string macClipboardGetText();

} // namespace NXRender

#endif // __APPLE__
