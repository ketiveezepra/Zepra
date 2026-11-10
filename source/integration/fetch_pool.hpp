// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file fetch_pool.hpp
 * @brief A bounded set of network threads for a page's subresources
 *
 * Jobs (one HTTP request each) wait in a queue and run on at most
 * `maxThreads` threads, at most `maxPerHost` at a time per host, as
 * browsers limit HTTP/1.1 connections per host. Higher priority jobs go
 * first (render-blocking stylesheets before scripts before the rest), in
 * the order they were posted within a priority.
 *
 * A job only does I/O and hands its bytes back (to a promise, an inbox, a
 * callback that locks); it never touches the DOM or the script engine,
 * which belong to the UI thread.
 */

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ZepraBrowser {

class FetchPool {
public:
    enum class Priority { High = 0, Normal = 1, Low = 2 };

    FetchPool(size_t maxThreads, size_t maxPerHost);
    /// Lets running jobs finish and drops the queued ones
    ~FetchPool();
    FetchPool(const FetchPool&) = delete;
    FetchPool& operator=(const FetchPool&) = delete;

    /// Queues `job` for `host` (see hostOf)
    void post(const std::string& host, Priority priority, std::function<void()> job);

    /// The part of a URL the per-host limit counts by: scheme://host:port
    /// (lowercased), "" for a URL without an authority (file:, data:)
    static std::string hostOf(const std::string& url);

    size_t queued() const;
    size_t running() const;
    /// The most jobs that ran at once for `host` so far (for tests)
    size_t peakRunning(const std::string& host) const;

private:
    struct Job {
        std::string host;
        std::function<void()> run;
    };

    void work();
    /// The next job whose host has a free slot, or false (lock held)
    bool takeJob(Job& out);

    const size_t maxThreads_;
    const size_t maxPerHost_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> queues_[3];
    std::unordered_map<std::string, size_t> runningPerHost_;
    std::unordered_map<std::string, size_t> peakPerHost_;
    size_t running_ = 0;
    size_t idle_ = 0;
    bool stopping_ = false;
    std::vector<std::thread> threads_;
};

} // namespace ZepraBrowser
