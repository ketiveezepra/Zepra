// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file web_storage.cpp
 * @brief Storage areas for localStorage/sessionStorage (see web_storage.hpp)
 */

#include "scripting/web_storage.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

namespace Zepra::WebCore {

namespace fs = std::filesystem;

namespace {

constexpr const char* kFileHeader = "zepra-localstorage 1\n";

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

}  // namespace

// ----------------------------------------------------------------------------
// StorageArea
// ----------------------------------------------------------------------------

std::optional<std::string> StorageArea::getItem(const std::string& key) const {
    auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    return it->second;
}

bool StorageArea::setItem(const std::string& key, const std::string& value) {
    auto it = values_.find(key);
    size_t used = used_;
    if (it != values_.end()) {
        if (it->second == value) return true;
        used -= key.size() + it->second.size();
    }
    used += key.size() + value.size();
    if (used > quota_) return false;
    if (it == values_.end()) {
        order_.push_back(key);
        values_.emplace(key, value);
    } else {
        it->second = value;
    }
    used_ = used;
    dirty = true;
    return true;
}

void StorageArea::removeItem(const std::string& key) {
    auto it = values_.find(key);
    if (it == values_.end()) return;
    used_ -= key.size() + it->second.size();
    values_.erase(it);
    order_.erase(std::find(order_.begin(), order_.end(), key));
    dirty = true;
}

void StorageArea::clear() {
    if (order_.empty()) return;
    order_.clear();
    values_.clear();
    used_ = 0;
    dirty = true;
}

std::optional<std::string> StorageArea::key(size_t index) const {
    if (index >= order_.size()) return std::nullopt;
    return order_[index];
}

std::string StorageArea::serialize() const {
    std::string out = kFileHeader;
    for (const std::string& key : order_) {
        const std::string& value = values_.at(key);
        out += std::to_string(key.size()) + ":" + key;
        out += std::to_string(value.size()) + ":" + value;
    }
    return out;
}

std::optional<StorageArea> StorageArea::deserialize(const std::string& bytes, size_t quota) {
    const std::string header = kFileHeader;
    if (bytes.compare(0, header.size(), header) != 0) return std::nullopt;
    size_t pos = header.size();
    auto field = [&](std::string& out) {
        size_t colon = bytes.find(':', pos);
        if (colon == std::string::npos || colon == pos || colon - pos > 12) return false;
        size_t length = 0;
        for (size_t i = pos; i < colon; i++) {
            if (!std::isdigit(static_cast<unsigned char>(bytes[i]))) return false;
            length = length * 10 + static_cast<size_t>(bytes[i] - '0');
        }
        if (length > bytes.size() - colon - 1) return false;
        out = bytes.substr(colon + 1, length);
        pos = colon + 1 + length;
        return true;
    };
    StorageArea area(quota);
    while (pos < bytes.size()) {
        std::string key, value;
        if (!field(key) || !field(value)) return std::nullopt;
        // A file over the quota (it was raised by hand, or shrank) keeps what fits
        area.setItem(key, value);
    }
    area.dirty = false;
    return area;
}

// ----------------------------------------------------------------------------
// Origins
// ----------------------------------------------------------------------------

std::string storageOriginOf(const std::string& url) {
    size_t colon = url.find(':');
    if (colon == std::string::npos || colon == 0) return "";
    std::string scheme = lower(url.substr(0, colon));
    for (char c : scheme) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.') return "";
    }
    if (scheme == "file") return "file://";
    if (url.compare(colon, 3, "://") != 0) return "";   // about:, data:, javascript:, blob:
    size_t start = colon + 3;
    size_t end = url.find_first_of("/?#", start);
    std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    // Leave out credentials
    if (size_t at = authority.rfind('@'); at != std::string::npos) authority = authority.substr(at + 1);
    authority = lower(authority);
    if (authority.empty()) return "";
    // A default port is the same origin as none
    std::string host = authority, port;
    size_t portColon = authority.rfind(':');
    if (portColon != std::string::npos && authority.find(']', portColon) == std::string::npos) {
        host = authority.substr(0, portColon);
        port = authority.substr(portColon + 1);
    }
    if ((scheme == "http" && port == "80") || (scheme == "https" && port == "443")) port.clear();
    return scheme + "://" + host + (port.empty() ? "" : ":" + port);
}

// ----------------------------------------------------------------------------
// The process-wide localStorage areas
// ----------------------------------------------------------------------------

namespace {

using Clock = std::chrono::steady_clock;

struct LocalArea {
    StorageArea area;
    Clock::time_point lastWrite{};
};

struct LocalStorageState {
    std::string directory;
    std::unordered_map<std::string, std::unique_ptr<LocalArea>> areas;
};

LocalStorageState& state() {
    static LocalStorageState s;
    return s;
}

/// "https://a.test:8080" -> "https%3A%2F%2Fa.test%3A8080.localstorage": a
/// file name on every platform that no other origin maps to
std::string fileNameFor(const std::string& origin) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string name;
    for (unsigned char c : origin) {
        if (std::isalnum(c) || c == '.' || c == '-' || c == '_') {
            name += static_cast<char>(c);
        } else {
            name += '%';
            name += kHex[c >> 4];
            name += kHex[c & 15];
        }
    }
    return name + ".localstorage";
}

bool writeArea(const std::string& origin, LocalArea& entry) {
    const std::string& dir = state().directory;
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path path = fs::path(dir) / fileNameFor(origin);
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        const std::string bytes = entry.area.serialize();
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    // Replace in one step: a crash mid-write leaves the old file
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(temp, path, ec);
        if (ec) return false;
    }
    entry.area.dirty = false;
    entry.lastWrite = Clock::now();
    return true;
}

}  // namespace

void setLocalStorageDirectory(const std::string& directory) {
    flushLocalStorage(true);
    state().directory = directory;
    state().areas.clear();
}

StorageArea* localStorageFor(const std::string& origin) {
    if (origin.empty()) return nullptr;
    auto& areas = state().areas;
    auto it = areas.find(origin);
    if (it != areas.end()) return &it->second->area;
    auto entry = std::make_unique<LocalArea>();
    if (!state().directory.empty()) {
        std::ifstream in(fs::path(state().directory) / fileNameFor(origin), std::ios::binary);
        if (in) {
            std::stringstream bytes;
            bytes << in.rdbuf();
            if (auto area = StorageArea::deserialize(bytes.str())) {
                entry->area = std::move(*area);
            } else {
                std::cerr << "[Storage] unreadable localStorage file for " << origin << ", starting empty" << std::endl;
            }
        }
    }
    StorageArea* area = &entry->area;
    areas.emplace(origin, std::move(entry));
    return area;
}

void flushLocalStorage(bool force) {
    if (state().directory.empty()) return;
    const auto now = Clock::now();
    for (auto& [origin, entry] : state().areas) {
        if (!entry->area.dirty) continue;
        if (!force && now - entry->lastWrite < std::chrono::seconds(1)) continue;
        if (!writeArea(origin, *entry)) {
            std::cerr << "[Storage] could not write localStorage for " << origin << std::endl;
        }
    }
}

void resetLocalStorageForTesting() {
    state().areas.clear();
}

} // namespace Zepra::WebCore
