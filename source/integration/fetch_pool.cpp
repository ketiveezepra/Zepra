// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file fetch_pool.cpp
 * @brief Bounded network threads with a per-host limit (see fetch_pool.hpp)
 */

#include "fetch_pool.hpp"

#include <algorithm>
#include <cctype>

namespace ZepraBrowser {

FetchPool::FetchPool(size_t maxThreads, size_t maxPerHost)
    : maxThreads_(std::max<size_t>(1, maxThreads)), maxPerHost_(std::max<size_t>(1, maxPerHost)) {}

FetchPool::~FetchPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        for (auto& q : queues_) q.clear();
    }
    wake_.notify_all();
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
}

std::string FetchPool::hostOf(const std::string& url) {
    size_t scheme = url.find("://");
    if (scheme == std::string::npos) return "";
    size_t start = scheme + 3;
    size_t end = url.find_first_of("/?#", start);
    std::string host = url.substr(0, end == std::string::npos ? url.size() : end);
    // user:password@ is not part of the host
    size_t at = host.find('@', start);
    if (at != std::string::npos) host = host.substr(0, start) + host.substr(at + 1);
    if (host.size() == start) return "";
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
    return host;
}

void FetchPool::post(const std::string& host, Priority priority, std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        queues_[static_cast<int>(priority)].push_back({host, std::move(job)});
        // A new thread only when none is free, up to the limit
        if (idle_ == 0 && threads_.size() < maxThreads_) {
            threads_.emplace_back([this] { work(); });
        }
    }
    wake_.notify_all();
}

bool FetchPool::takeJob(Job& out) {
    for (auto& q : queues_) {
        for (auto it = q.begin(); it != q.end(); ++it) {
            if (runningPerHost_[it->host] >= maxPerHost_) continue;
            out = std::move(*it);
            q.erase(it);
            return true;
        }
    }
    return false;
}

void FetchPool::work() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        Job job;
        idle_++;
        wake_.wait(lock, [&] { return stopping_ || takeJob(job); });
        idle_--;
        if (stopping_ && !job.run) return;
        size_t& onHost = runningPerHost_[job.host];
        onHost++;
        running_++;
        size_t& peak = peakPerHost_[job.host];
        peak = std::max(peak, onHost);
        lock.unlock();
        job.run();
        job.run = nullptr;   // its captures go before the lock is taken again
        lock.lock();
        running_--;
        if (--runningPerHost_[job.host] == 0) runningPerHost_.erase(job.host);
        // A slot of this host is free: a job waiting for it may go now
        wake_.notify_all();
        if (stopping_) return;
    }
}

size_t FetchPool::queued() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queues_[0].size() + queues_[1].size() + queues_[2].size();
}

size_t FetchPool::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

size_t FetchPool::peakRunning(const std::string& host) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = peakPerHost_.find(host);
    return it == peakPerHost_.end() ? 0 : it->second;
}

} // namespace ZepraBrowser
