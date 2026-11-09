// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file load_trace.hpp
 * @brief ZEPRA_TRACE_LOAD=1: timestamps of a page load on stderr
 *
 * Every line is "[LoadTrace] +<ms since the navigation started> <event>".
 * The browser marks the load's steps (HTML received, styles ready, first
 * paint, DOMContentLoaded, load) and calls frameStart() once per frame; a
 * gap of 50 ms or more between two frames is a stall of the UI thread and is
 * logged with its length. summary lines give time to first paint and the
 * longest stall so far (at load and 10, 20 and 30 s after navigation).
 * Without the variable every call returns at once.
 */

#pragma once

#include <string>

namespace ZepraBrowser::LoadTrace {

/// Whether ZEPRA_TRACE_LOAD is set (and not "0")
bool enabled();

/// A navigation started: times count from now, the stall maximum resets
void beginNavigation(const std::string& url);

/// Logs `event` with its time
void mark(const std::string& event);

/// The UI thread starts a frame (call first thing each frame)
void frameStart();

/// The new document's boxes exist; the next painted frame is the first paint
void documentLaidOut();

/// A frame was presented; `pagePainted` when it drew the page's boxes
void framePresented(bool pagePainted);

/// Logs time to first paint and the longest stall so far
void summary(const std::string& why);

} // namespace ZepraBrowser::LoadTrace
