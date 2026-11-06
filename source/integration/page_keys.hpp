// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file page_keys.hpp
 * @brief The KeyboardEvent members (key, code, keyCode, location, modifiers)
 *        a page sees for a key the window reported through NXRender.
 */
#pragma once

#include <string>

#include "input/events.h"
#include "scripting/dom_bindings.hpp"

namespace ZepraBrowser {

/**
 * @param key   the physical key
 * @param text  the character it typed ("" for keys that type none, and on
 *              key up); gives `key` its case and shifted symbol
 */
Zepra::WebCore::KeyEventInit domKeyFor(NXRender::KeyCode key, const std::string& text,
                                       const NXRender::Modifiers& modifiers, bool repeat = false);

} // namespace ZepraBrowser
