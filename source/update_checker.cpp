// Copyright (c) 2025-2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file update_checker.cpp
 * @brief Update checker implementation — cross-platform, non-blocking.
 */

#include "update_checker.h"
#include "version.h"

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <sstream>
#include <cstring>

// Platform-specific socket headers
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   using SocketHandle = SOCKET;
#  define SOCK_INVALID  INVALID_SOCKET
#  define SOCK_CLOSE(s) closesocket(s)
#else
#  include <sys/socket.h>
#  include <netdb.h>
#  include <unistd.h>
   using SocketHandle = int;
#  define SOCK_INVALID  (-1)
#  define SOCK_CLOSE(s) ::close(s)
#endif

namespace zepra {
namespace update {

// ============================================================================
// Constants
// ============================================================================
static constexpr const char* kDefaultApiHost = "api.ketivee.com";
static constexpr const char* kDefaultApiPath = "/v1/zepra/update";
static constexpr int         kDefaultPort    = 80;   // HTTP only for update check

// ============================================================================
// Minimal HTTP GET helper (no libcurl dependency)
// ============================================================================
static std::string httpGet(const std::string& host, const std::string& path,
                            int port, int timeoutMs, std::string& outError)
{
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        outError = "WSAStartup failed";
        return {};
    }
#endif

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family   = AF_INET;

    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
        outError = "DNS resolution failed for: " + host;
#ifdef _WIN32
        WSACleanup();
#endif
        return {};
    }

    SocketHandle sock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock == SOCK_INVALID) {
        freeaddrinfo(res);
        outError = "Socket creation failed";
#ifdef _WIN32
        WSACleanup();
#endif
        return {};
    }

    // Set socket timeout
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(timeoutMs);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&tv), sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&tv), sizeof(tv));
#else
    struct timeval tv{};
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    if (::connect(sock, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
        SOCK_CLOSE(sock);
        freeaddrinfo(res);
        outError = "Connection failed to " + host;
#ifdef _WIN32
        WSACleanup();
#endif
        return {};
    }
    freeaddrinfo(res);

    // Build HTTP/1.0 request (no keep-alive needed)
    std::string req =
        "GET " + path + "?version=" ZEPRA_VERSION_STRING
        "&channel=" ZEPRA_BUILD_CHANNEL
        "&os="
#if defined(_WIN32)
        "windows"
#elif defined(__APPLE__)
        "macos"
#else
        "linux"
#endif
        " HTTP/1.0\r\n"
        "Host: " + host + "\r\n"
        "User-Agent: ZepraBrowser/" ZEPRA_VERSION_STRING "\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n\r\n";

    if (::send(sock, req.c_str(), static_cast<int>(req.size()), 0) < 0) {
        SOCK_CLOSE(sock);
        outError = "Send failed";
#ifdef _WIN32
        WSACleanup();
#endif
        return {};
    }

    // Read response
    std::string response;
    char buf[4096];
    int  n;
    while ((n = ::recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = '\0';
        response += buf;
        if (response.size() > 65536) break; // Guard against huge response
    }

    SOCK_CLOSE(sock);
#ifdef _WIN32
    WSACleanup();
#endif

    // Strip HTTP headers — find double CRLF
    auto pos = response.find("\r\n\r\n");
    if (pos != std::string::npos)
        return response.substr(pos + 4);

    pos = response.find("\n\n");
    if (pos != std::string::npos)
        return response.substr(pos + 2);

    return response;
}

// ============================================================================
// Minimal JSON field extractor (avoids pulling in nlohmann for this tiny use)
// ============================================================================
static std::string jsonField(const std::string& json, const std::string& key)
{
    // Handles both string and bool values: "key":"value" or "key":value
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};

    pos += needle.size();
    // Skip whitespace and colon
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;

    if (pos >= json.size()) return {};

    if (json[pos] == '"') {
        // String value
        ++pos;
        auto end = json.find('"', pos);
        if (end == std::string::npos) return {};
        return json.substr(pos, end - pos);
    } else {
        // Non-string (bool/number) — read until delimiter
        auto end = pos;
        while (end < json.size() && json[end] != ',' && json[end] != '}') ++end;
        std::string val = json.substr(pos, end - pos);
        // Trim whitespace
        auto s = val.find_first_not_of(" \t\r\n");
        auto e = val.find_last_not_of(" \t\r\n");
        return (s == std::string::npos) ? "" : val.substr(s, e - s + 1);
    }
}

static UpdateInfo parseResponse(const std::string& json)
{
    UpdateInfo info;
    info.currentVersion  = ZEPRA_VERSION_STRING;
    info.channel         = ZEPRA_BUILD_CHANNEL;
    info.latestVersion   = jsonField(json, "latest_version");
    info.downloadUrl     = jsonField(json, "download_url");
    info.releaseNotesUrl = jsonField(json, "release_notes_url");
    info.releaseDate     = jsonField(json, "release_date");
    info.summary         = jsonField(json, "summary");
    info.available       = (jsonField(json, "update_available") == "true");
    info.isSecurity      = (jsonField(json, "is_security")      == "true");
    return info;
}

// ============================================================================
// Impl
// ============================================================================
struct UpdateChecker::Impl {
    std::string   channel  = ZEPRA_BUILD_CHANNEL;
    std::string   apiHost  = kDefaultApiHost;
    std::string   apiPath  = kDefaultApiPath;
    int           apiPort  = kDefaultPort;

    std::atomic<bool> checking{false};
    std::atomic<bool> cancelled{false};

    mutable std::mutex  resultMtx;
    UpdateInfo          lastResult_;
    std::string         lastError_;

    std::thread worker;
};

// ============================================================================
// Public API
// ============================================================================
UpdateChecker::UpdateChecker()  : impl_(std::make_unique<Impl>()) {}
UpdateChecker::~UpdateChecker() { cancel(); }

void UpdateChecker::setChannel(const std::string& ch) { impl_->channel = ch; }

void UpdateChecker::setApiUrl(const std::string& url)
{
    // Parse host + path from URL (http://host/path)
    std::string s = url;
    if (s.substr(0, 7) == "http://")  s = s.substr(7);
    if (s.substr(0, 8) == "https://") s = s.substr(8); // plain HTTP only
    auto slash = s.find('/');
    if (slash != std::string::npos) {
        impl_->apiHost = s.substr(0, slash);
        impl_->apiPath = s.substr(slash);
    } else {
        impl_->apiHost = s;
        impl_->apiPath = "/";
    }
}

void UpdateChecker::checkAsync(UpdateCallback callback)
{
    if (impl_->checking.load()) return;

    impl_->checking  = true;
    impl_->cancelled = false;

    // Capture by value what the thread needs
    std::string host    = impl_->apiHost;
    std::string path    = impl_->apiPath + "?version=" ZEPRA_VERSION_STRING
                          + "&channel=" + impl_->channel
                          + "&os="
#if defined(_WIN32)
                          "windows"
#elif defined(__APPLE__)
                          "macos"
#else
                          "linux"
#endif
                          ;
    int         port    = impl_->apiPort;
    auto*       rawImpl = impl_.get();

    if (impl_->worker.joinable()) impl_->worker.detach();

    impl_->worker = std::thread([rawImpl, host, path, port,
                                  cb = std::move(callback)]() mutable
    {
        std::string error;
        UpdateInfo  info;

        if (!rawImpl->cancelled.load()) {
            std::string body = httpGet(host, path, port, 5000, error);
            if (!body.empty()) {
                info = parseResponse(body);
            } else {
                info.currentVersion = ZEPRA_VERSION_STRING;
                info.channel        = rawImpl->channel;
            }
        }

        {
            std::lock_guard<std::mutex> lk(rawImpl->resultMtx);
            rawImpl->lastResult_ = info;
            rawImpl->lastError_  = error;
        }

        rawImpl->checking = false;

        if (cb) cb(info);
    });
}

UpdateInfo UpdateChecker::checkSync(int timeoutMs)
{
    std::string error;
    std::string path = impl_->apiPath;
    std::string body = httpGet(impl_->apiHost, path, impl_->apiPort, timeoutMs, error);

    UpdateInfo info;
    if (!body.empty()) {
        info = parseResponse(body);
    } else {
        info.currentVersion = ZEPRA_VERSION_STRING;
        info.channel        = impl_->channel;
    }

    std::lock_guard<std::mutex> lk(impl_->resultMtx);
    impl_->lastResult_ = info;
    impl_->lastError_  = error;
    return info;
}

void UpdateChecker::cancel()
{
    impl_->cancelled = true;
    if (impl_->worker.joinable()) impl_->worker.detach();
    impl_->checking = false;
}

bool UpdateChecker::isChecking() const { return impl_->checking.load(); }

UpdateInfo UpdateChecker::lastResult() const
{
    std::lock_guard<std::mutex> lk(impl_->resultMtx);
    return impl_->lastResult_;
}

std::string UpdateChecker::lastError() const
{
    std::lock_guard<std::mutex> lk(impl_->resultMtx);
    return impl_->lastError_;
}

} // namespace update
} // namespace zepra
