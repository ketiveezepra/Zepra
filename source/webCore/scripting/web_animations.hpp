// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file web_animations.hpp
 * @brief Script source of the Web Animations API (Animation, KeyframeEffect,
 *        DocumentTimeline, element.animate(), getAnimations()).
 */

#pragma once

namespace Zepra::WebCore {

/// The script that defines the API; evaluated once per page realm, after
/// the DOM bindings (it needs Element and __zepraSetAnimatedStyle)
const char* webAnimationsScript();

} // namespace Zepra::WebCore
