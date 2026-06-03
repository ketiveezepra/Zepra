// Copyright (c) 2025-2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file update_checker.h
 * @brief Cross-platform update checker for Zepra Browser beta.
 *
 * Polls the KetiveeAI update API to notify users when a newer
 * beta/nightly/stable version is available. Non-blocking, runs
 * on a background thread and fires a callback on the main thread.
 */

#pragma once

#include <string>
#include <functional>
#include <memory>
#include <cstdint>

namespace zepra {
namespace update {

/** Result of a version check. */
struct UpdateInfo {
    bool        available    = false;  ///< True if a newer version exists
    std::string currentVersion;        ///< Running version, e.g. "0.1.0-beta"
    std::string latestVersion;         ///< Latest available, e.g. "0.2.0-beta"
    std::string channel;               ///< "stable" | "beta" | "nightly"
    std::string downloadUrl;           ///< Direct download link
    std::string releaseNotesUrl;       ///< Release notes page
    std::string releaseDate;           ///< ISO-8601 date string
    std::string summary;               ///< Short description of what's new
    bool        isSecurity = false;    ///< True if the update contains security fixes
};

/** Callback type. Always invoked on the thread that called checkAsync(). */
using UpdateCallback = std::function<void(const UpdateInfo& info)>;

/**
 * @brief UpdateChecker — lightweight non-blocking version checker.
 *
 * Usage:
 * @code
 *   auto checker = std::make_unique<UpdateChecker>();
 *   checker->setChannel("beta");
 *   checker->checkAsync([](const UpdateInfo& info) {
 *       if (info.available) {
 *           showUpdateBanner(info.latestVersion, info.downloadUrl);
 *       }
 *   });
 * @endcode
 */
class UpdateChecker {
public:
    UpdateChecker();
    ~UpdateChecker();

    // Non-copyable
    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

    /** Set release channel: "stable", "beta", or "nightly". Default: "beta". */
    void setChannel(const std::string& channel);

    /** Override the default update endpoint URL (useful for testing). */
    void setApiUrl(const std::string& url);

    /**
     * Perform a non-blocking version check.
     * The callback is invoked on a background thread — marshal to main thread
     * as needed for UI updates.
     */
    void checkAsync(UpdateCallback callback);

    /**
     * Blocking version check (for startup or CLI use).
     * @param timeoutMs  Maximum wait time in milliseconds. Default 5000ms.
     * @return UpdateInfo with result; info.available = false on timeout/error.
     */
    UpdateInfo checkSync(int timeoutMs = 5000);

    /** Cancel an in-progress async check. */
    void cancel();

    /** True if a check is currently in progress. */
    bool isChecking() const;

    /** Last check result (from the most recent successful check). */
    UpdateInfo lastResult() const;

    /** Human-readable error message from the last failed check (empty if OK). */
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace update
} // namespace zepra
