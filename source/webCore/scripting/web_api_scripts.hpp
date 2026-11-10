// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file web_api_scripts.hpp
 * @brief Script source installing TextEncoder, TextDecoder and a minimal
 *        ReadableStream on a page's global object (each only when missing).
 */

#pragma once

namespace Zepra::WebCore {

/// The script that defines the APIs; evaluated once per page realm
const char* webApiScripts();

} // namespace Zepra::WebCore
