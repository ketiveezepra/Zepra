// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
#pragma once

namespace Zepra::WebCore {

/**
 * ZepraUAStylesheet - Browser default styles for all HTML elements.
 *
 * TODO(NeolyxOS): When running on NeolyxOS, load from disk at
 * /sys/theme/zepra.css to allow system-level theming.
 */
struct ZepraUAStylesheet {
    static const char* getStylesheet();
    /// Extra user-agent rules for documents in quirks mode
    static const char* getQuirksStylesheet();
};

} // namespace Zepra::WebCore
