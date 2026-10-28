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

namespace Zepra::WebCore {

enum class ResourceKind { Document, Stylesheet, Script, Image, Font, Fetch, Xhr, Media, Other };

struct ResourceRequestInfo {
    int tabId = -1;                 ///< the tab that asked, -1 when unknown (the active tab)
    std::string method = "GET";
    std::string url;
    ResourceKind kind = ResourceKind::Other;
};

struct ResourceResponseInfo {
    int status = 0;                 ///< 0 with a non-empty error: the request failed before a response
    std::string statusText;
    std::string mimeType;           ///< Content-Type as sent
    size_t bodySize = 0;            ///< decoded body bytes
    size_t transferredBytes = 0;    ///< bytes read off the connection, 0 when unknown
    std::string error;              ///< non-empty when the request failed
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
}

/// The recorder loaders report to (null: requests go unrecorded). The browser
/// sets it once at startup, before any loader thread runs.
inline void setNetworkRecorder(NetworkRecorder* recorder) { detail::networkRecorderSlot().store(recorder); }
inline NetworkRecorder* networkRecorder() { return detail::networkRecorderSlot().load(); }

} // namespace Zepra::WebCore
