// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file network_recorder_http.hpp
 * @brief Fills a ResourceResponseInfo from an NXNet HttpResponse
 *
 * The browser's page loads and the image loader threads both report what
 * the HTTP client measured (headers as sent and received, the timing of each
 * phase, the start of a text body) the same way.
 */
#pragma once

#include "network_recorder.hpp"
#include "networking/http_response.hpp"

namespace Zepra::WebCore {

/// Text bodies kept per request for the Network panel's Response tab
constexpr size_t kBodyPreviewLimit = 64 * 1024;

inline void fillResponseDetails(ResourceResponseInfo& info, const Zepra::Networking::HttpResponse& response) {
    info.requestHeaders = response.requestHeaders();
    info.responseHeaders = response.headerList();
    const auto& body = response.body();
    info.bodyPreview = textBodyPreview(info.mimeType, reinterpret_cast<const char*>(body.data()), body.size(),
                                       kBodyPreviewLimit, info.bodyTruncated);
    const auto& t = response.timings();
    info.timings.known = true;
    info.timings.redirect = t.redirectMs;
    info.timings.dns = t.dnsMs;
    info.timings.connect = t.connectMs;
    info.timings.tls = t.tlsMs;
    info.timings.send = t.sendMs;
    info.timings.wait = t.waitMs;
    info.timings.receive = t.receiveMs;
}

} // namespace Zepra::WebCore
