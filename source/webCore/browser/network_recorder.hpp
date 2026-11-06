// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file network_recorder.hpp
 * @brief Where WebCore loaders report their requests (the DevTools Network panel)
 *
 * The browser installs one recorder; the image loader threads and the
 * script context's fetch/XMLHttpRequest report every request through it.
 * willSendRequest/didFinishRequest may be called from any thread, so an
 * implementation records under its own lock.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Zepra::WebCore {

enum class ResourceKind { Document, Stylesheet, Script, Image, Font, Fetch, Xhr, Media, Other };

struct ResourceRequestInfo {
    int tabId = -1;                 ///< the tab that asked, -1 when unknown (the active tab)
    std::string method = "GET";
    std::string url;
    ResourceKind kind = ResourceKind::Other;
};

using HeaderList = std::vector<std::pair<std::string, std::string>>;

/// Where a request's time went, in milliseconds; see Zepra::Networking::HttpTimings
struct ResourceTimings {
    bool known = false;             ///< false: the loader measured none of these
    double redirect = 0;
    double dns = -1, connect = -1, tls = -1;   ///< -1: a kept-alive connection (or no TLS)
    double send = 0, wait = 0, receive = 0;
};

struct ResourceResponseInfo {
    int status = 0;                 ///< 0 with a non-empty error: the request failed before a response
    std::string statusText;
    std::string mimeType;           ///< Content-Type as sent
    size_t bodySize = 0;            ///< decoded body bytes
    size_t transferredBytes = 0;    ///< bytes read off the connection, 0 when unknown
    std::string error;              ///< non-empty when the request failed
    HeaderList requestHeaders;      ///< as sent on the final hop
    HeaderList responseHeaders;     ///< as received
    std::string bodyPreview;        ///< the start of a text body (empty for binary bodies)
    bool bodyTruncated = false;
    ResourceTimings timings;
};

class NetworkRecorder {
public:
    virtual ~NetworkRecorder() = default;
    /// @return an id for didFinishRequest, 0 when the request is not recorded
    virtual uint64_t willSendRequest(const ResourceRequestInfo& request) = 0;
    virtual void didFinishRequest(uint64_t id, const ResourceResponseInfo& response) = 0;
};

namespace detail {
inline std::atomic<NetworkRecorder*>& networkRecorderSlot() {
    static std::atomic<NetworkRecorder*> recorder{nullptr};
    return recorder;
}
inline std::atomic<bool>& cacheDisabledSlot() {
    static std::atomic<bool> disabled{false};
    return disabled;
}
}

/// The recorder loaders report to (null: requests go unrecorded). The browser
/// sets it once at startup, before any loader thread runs.
inline void setNetworkRecorder(NetworkRecorder* recorder) { detail::networkRecorderSlot().store(recorder); }
inline NetworkRecorder* networkRecorder() { return detail::networkRecorderSlot().load(); }

/// DevTools "Disable cache": while set, every request asks for a fresh copy
/// (Cache-Control/Pragma: no-cache) and images loaded by an earlier page load
/// are fetched again instead of reused. Read from any thread.
inline void setCacheDisabled(bool disabled) { detail::cacheDisabledSlot().store(disabled); }
inline bool cacheDisabled() { return detail::cacheDisabledSlot().load(); }

/// Whether a Content-Type is text the Network panel can show (HTML, CSS,
/// scripts, JSON, XML, SVG, plain text)
inline bool isTextMimeType(const std::string& mimeType) {
    std::string t;
    for (char c : mimeType) {
        if (c == ';') break;
        t += (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    }
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (t.compare(0, 5, "text/") == 0) return true;
    for (const char* s : {"json", "javascript", "ecmascript", "xml", "svg"}) {
        if (t.find(s) != std::string::npos) return true;
    }
    return false;
}

/// The start of a text body, at most `limit` bytes (empty for a binary type);
/// `truncated` is set when the body was longer
inline std::string textBodyPreview(const std::string& mimeType, const char* data, size_t size,
                                   size_t limit, bool& truncated) {
    truncated = false;
    if (!data || !size || !isTextMimeType(mimeType)) return {};
    truncated = size > limit;
    return std::string(data, truncated ? limit : size);
}

} // namespace Zepra::WebCore
