// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file network_details.hpp
 * @brief What the Network panel shows besides the request list: the filter
 *        box's terms, a request's Headers, its Response body as text lines
 *        and its Timings as phases (the same phases draw the waterfall).
 *        No drawing here; the panel lays these out.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "network_monitor.h"

namespace ZepraBrowser::DevTools {

/// The filter box, read the way Firefox reads it: space-separated terms that
/// must all match. A plain term matches anywhere in the URL (case-insensitive),
/// "-term" must not match, and "key:value" checks one field: status-code,
/// method, domain, mime-type, larger-than (bytes, or with a k / m suffix),
/// is:failed / is:pending. An unknown key is matched against the URL as text.
class NetworkFilter {
public:
    NetworkFilter() = default;
    explicit NetworkFilter(const std::string& text);
    bool empty() const { return terms_.empty(); }
    bool matches(const zepra::NetworkEntry& entry) const;

private:
    struct Term {
        std::string key;     ///< empty for a plain URL term
        std::string value;   ///< lower case
        bool negate = false;
    };
    std::vector<Term> terms_;
};

/// One phase of a request's time, in milliseconds from when it was recorded
struct TimingPhase {
    const char* name;
    double start = 0;
    double duration = 0;
    uint32_t color = 0;
};

/// The phases of a request in order: Blocked (queued before the client
/// started, plus what the loader did around it), Redirect, DNS Resolution,
/// Connecting, TLS Setup, Sending, Waiting, Receiving. Phases that did not
/// happen are left out; for a request the loader could not measure, one
/// "Total" phase. Empty while the request is pending.
std::vector<TimingPhase> timingPhases(const zepra::NetworkEntry& entry);

/// A line of the Headers tab: a section title ("Response Headers (12)") or a
/// name and value
struct HeaderLine {
    bool section = false;
    std::string name;
    std::string value;
};

/// General (URL, method, status, sizes, connection), then the response and
/// request headers in the order they went over the wire
std::vector<HeaderLine> headerLines(const zepra::NetworkEntry& entry);

/// The Response tab's lines: JSON re-indented, other text as it came, tabs
/// expanded, every non-ASCII character as one '?' (the panel font is ASCII
/// only) and long lines wrapped at `columns`. At most `maxLines` lines.
std::vector<std::string> responseLines(const std::string& body, const std::string& contentType,
                                       size_t columns, size_t maxLines);

/// `text` re-indented two spaces per level when it looks like JSON (starts
/// with { or [); strings are copied untouched. Anything else comes back as is.
std::string prettyJson(const std::string& text);

/// A phase's length in ASCII: "0.65 ms", "12 ms", "1.78 s" (zepra::formatDuration
/// writes microseconds with a Greek mu, which the panel font can't draw)
std::string formatPhase(double ms);

/// "200 OK", "404 Not Found", "(failed)", "(pending)"
std::string statusLine(const zepra::NetworkEntry& entry);

} // namespace ZepraBrowser::DevTools
