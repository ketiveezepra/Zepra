// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file load_trace.cpp
 * @brief ZEPRA_TRACE_LOAD timestamps (see load_trace.hpp). UI thread only.
 */

#include "load_trace.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ZepraBrowser::LoadTrace {

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kStallMs = 50.0;          // a frame gap this long is logged
constexpr double kWatchMs = 30000.0;       // stalls count this long after navigation
constexpr double kSummaryAtMs[] = {10000.0, 20000.0, 30000.0};

struct State {
    bool active = false;
    Clock::time_point start;
    Clock::time_point lastFrame;
    bool haveLastFrame = false;
    double longestStall = 0;
    double longestStallAt = 0;
    double firstPaint = -1;
    bool awaitingPaint = false;
    size_t summariesDone = 0;
};

State& state() {
    static State s;
    return s;
}

double msSince(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

double now() { return msSince(state().start, Clock::now()); }

} // namespace

bool enabled() {
    static const bool on = [] {
        const char* v = std::getenv("ZEPRA_TRACE_LOAD");
        return v && *v && std::strcmp(v, "0") != 0;
    }();
    return on;
}

void beginNavigation(const std::string& url) {
    if (!enabled()) return;
    State& s = state();
    s = State();
    s.active = true;
    s.start = Clock::now();
    std::fprintf(stderr, "[LoadTrace] +%8.1f navigate %s\n", 0.0, url.c_str());
}

void mark(const std::string& event) {
    if (!enabled() || !state().active) return;
    std::fprintf(stderr, "[LoadTrace] +%8.1f %s\n", now(), event.c_str());
}

void frameStart() {
    if (!enabled()) return;
    State& s = state();
    Clock::time_point t = Clock::now();
    if (s.active && s.haveLastFrame) {
        double gap = msSince(s.lastFrame, t);
        double at = msSince(s.start, t);
        if (at - gap <= kWatchMs && gap >= kStallMs) {
            if (gap > s.longestStall) {
                s.longestStall = gap;
                s.longestStallAt = at;
            }
            std::fprintf(stderr, "[LoadTrace] +%8.1f stall %.1f ms (frame gap)\n", at, gap);
        }
        while (s.summariesDone < sizeof(kSummaryAtMs) / sizeof(kSummaryAtMs[0]) &&
               at >= kSummaryAtMs[s.summariesDone]) {
            summary("t+" + std::to_string((int)(kSummaryAtMs[s.summariesDone] / 1000)) + "s");
            s.summariesDone++;
        }
    }
    s.lastFrame = t;
    s.haveLastFrame = true;
}

void documentLaidOut() {
    if (!enabled() || !state().active) return;
    if (state().firstPaint < 0) state().awaitingPaint = true;
}

void framePresented(bool pagePainted) {
    if (!enabled()) return;
    State& s = state();
    if (!s.active || !s.awaitingPaint || !pagePainted) return;
    s.awaitingPaint = false;
    s.firstPaint = now();
    std::fprintf(stderr, "[LoadTrace] +%8.1f first paint\n", s.firstPaint);
}

void summary(const std::string& why) {
    if (!enabled() || !state().active) return;
    const State& s = state();
    std::fprintf(stderr, "[LoadTrace] +%8.1f summary (%s): first paint %.1f ms, longest stall %.1f ms at +%.1f\n",
                 now(), why.c_str(), s.firstPaint, s.longestStall, s.longestStallAt);
}

} // namespace ZepraBrowser::LoadTrace
