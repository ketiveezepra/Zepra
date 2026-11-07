// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file web_storage.hpp
 * @brief The data behind localStorage and sessionStorage: a storage area per
 *        origin with string keys and values in insertion order and a quota.
 *
 * localStorage areas are shared by every page of an origin in the process
 * and, once a directory is set (setLocalStorageDirectory), kept on disk: an
 * area is read the first time a page of its origin uses it and written back
 * by flushLocalStorage(), which the browser calls as it runs and on exit.
 * Without a directory (unit tests) they stay in memory.
 *
 * Used from the main thread only.
 */

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Zepra::WebCore {

class StorageArea {
public:
    /// Browsers allow about 5 MB per origin (counted here in UTF-8 bytes of
    /// keys and values)
    static constexpr size_t kDefaultQuota = 5 * 1024 * 1024;

    explicit StorageArea(size_t quota = kDefaultQuota) : quota_(quota) {}

    std::optional<std::string> getItem(const std::string& key) const;
    /// False (nothing changes) when the new value would go over the quota
    bool setItem(const std::string& key, const std::string& value);
    void removeItem(const std::string& key);
    void clear();
    std::optional<std::string> key(size_t index) const;
    size_t length() const { return order_.size(); }
    size_t usedBytes() const { return used_; }

    /// Set by every change, cleared by whoever saves the area
    bool dirty = false;

    /// The area as bytes on disk and back: a version line, then each entry's
    /// key and value as "<byte length>:<bytes>" (any byte allowed)
    std::string serialize() const;
    static std::optional<StorageArea> deserialize(const std::string& bytes, size_t quota = kDefaultQuota);

private:
    std::vector<std::string> order_;
    std::unordered_map<std::string, std::string> values_;
    size_t used_ = 0;
    size_t quota_;
};

/// The origin localStorage uses for a page URL: "scheme://host[:port]" with
/// the scheme and host lowercased and a default port left out; "file://" for
/// every local file (as other browsers share it); empty for URLs without one
/// (about:blank, data:), whose pages get a storage that is not kept
std::string storageOriginOf(const std::string& url);

/// Where localStorage is kept ("" keeps it in memory only, the default)
void setLocalStorageDirectory(const std::string& directory);
/// The process-wide localStorage area of `origin` (loaded from disk on first
/// use); null for an empty origin
StorageArea* localStorageFor(const std::string& origin);
/// Writes the changed areas to disk. Unless `force`, an area is written at
/// most once a second however often it changes; on exit pass true.
void flushLocalStorage(bool force = false);
/// Forgets every area in memory (tests); does not touch the disk
void resetLocalStorageForTesting();

} // namespace Zepra::WebCore
