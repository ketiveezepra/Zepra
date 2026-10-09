// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file bookmark_manager.cpp
 * @brief Bookmark management implementation
 */

#include "storage/bookmark_manager.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <map>
#include <iostream>
#include <iomanip>

namespace ZepraBrowser {

// Implementation details
struct BookmarkManager::Impl {
    std::map<int, Bookmark> bookmarks;
    std::map<int, BookmarkFolder> folders;
    int nextId = 1;
    int nextFolderId = 1;
    std::string filepath;
    
    // Quick lookup
    std::map<std::string, int> urlToId;
};

// Singleton
BookmarkManager& BookmarkManager::instance() {
    static BookmarkManager instance;
    return instance;
}

BookmarkManager::BookmarkManager() : impl_(std::make_unique<Impl>()) {
    std::cout << "[BookmarkManager] Initialized" << std::endl;
}

BookmarkManager::~BookmarkManager() = default;

int BookmarkManager::addBookmark(const std::string& url, const std::string& title,
                                 const std::string& folder) {
    // Check if already exists
    if (impl_->urlToId.count(url)) {
        std::cout << "[BookmarkManager] Bookmark already exists: " << url << std::endl;
        return impl_->urlToId[url];
    }
    
    Bookmark bookmark;
    bookmark.id = impl_->nextId++;
    bookmark.url = url;
    bookmark.title = title.empty() ? url : title;
    bookmark.folder = folder;
    bookmark.dateAdded = std::chrono::system_clock::now();
    
    impl_->bookmarks[bookmark.id] = bookmark;
    impl_->urlToId[url] = bookmark.id;
    
    std::cout << "[BookmarkManager] Added: " << title << " (" << url << ")" << std::endl;
    
    return bookmark.id;
}

bool BookmarkManager::removeBookmark(int id) {
    auto it = impl_->bookmarks.find(id);
    if (it == impl_->bookmarks.end()) {
        return false;
    }
    
    impl_->urlToId.erase(it->second.url);
    impl_->bookmarks.erase(it);
    
    std::cout << "[BookmarkManager] Removed bookmark ID: " << id << std::endl;
    return true;
}

bool BookmarkManager::hasBookmark(const std::string& url) const {
    return impl_->urlToId.count(url) > 0;
}

Bookmark* BookmarkManager::getBookmark(int id) {
    auto it = impl_->bookmarks.find(id);
    if (it != impl_->bookmarks.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<Bookmark> BookmarkManager::getAllBookmarks() const {
    std::vector<Bookmark> result;
    result.reserve(impl_->bookmarks.size());
    
    for (const auto& pair : impl_->bookmarks) {
        result.push_back(pair.second);
    }
    
    // Sort by date added (newest first)
    std::sort(result.begin(), result.end(), 
             [](const Bookmark& a, const Bookmark& b) {
                 return a.dateAdded > b.dateAdded;
             });
    
    return result;
}

std::vector<Bookmark> BookmarkManager::getBookmarksInFolder(const std::string& folder) const {
    std::vector<Bookmark> result;
    
    for (const auto& pair : impl_->bookmarks) {
        if (pair.second.folder == folder) {
            result.push_back(pair.second);
        }
    }
    
    return result;
}

std::vector<Bookmark> BookmarkManager::searchBookmarks(const std::string& query) const {
    std::vector<Bookmark> result;
    std::string lowerQuery = query;
    std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);
    
    for (const auto& pair : impl_->bookmarks) {
        std::string lowerTitle = pair.second.title;
        std::string lowerUrl = pair.second.url;
        std::transform(lowerTitle.begin(), lowerTitle.end(), lowerTitle.begin(), ::tolower);
        std::transform(lowerUrl.begin(), lowerUrl.end(), lowerUrl.begin(), ::tolower);
        
        if (lowerTitle.find(lowerQuery) != std::string::npos ||
            lowerUrl.find(lowerQuery) != std::string::npos) {
            result.push_back(pair.second);
        }
    }
    
    return result;
}

int BookmarkManager::createFolder(const std::string& name, int parentId) {
    BookmarkFolder folder;
    folder.id = impl_->nextFolderId++;
    folder.name = name;
    folder.parentId = parentId;
    
    impl_->folders[folder.id] = folder;
    
    std::cout << "[BookmarkManager] Created folder: " << name << std::endl;
    return folder.id;
}

bool BookmarkManager::removeFolder(int id) {
    auto it = impl_->folders.find(id);
    if (it == impl_->folders.end()) {
        return false;
    }
    
    // Remove all bookmarks in this folder
    std::vector<int> toRemove;
    for (const auto& pair : impl_->bookmarks) {
        if (pair.second.parentId == id) {
            toRemove.push_back(pair.first);
        }
    }
    
    for (int bookmarkId : toRemove) {
        removeBookmark(bookmarkId);
    }
    
    impl_->folders.erase(it);
    return true;
}

std::vector<BookmarkFolder> BookmarkManager::getFolders() const {
    std::vector<BookmarkFolder> result;
    result.reserve(impl_->folders.size());
    
    for (const auto& pair : impl_->folders) {
        result.push_back(pair.second);
    }
    
    return result;
}

bool BookmarkManager::save(const std::string& filepath) {
    std::ofstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "[BookmarkManager] Failed to save: " << filepath << std::endl;
        return false;
    }
    
    // Simple JSON format
    file << "{\n";
    file << "  \"bookmarks\": [\n";
    
    bool first = true;
    for (const auto& pair : impl_->bookmarks) {
        if (!first) file << ",\n";
        first = false;
        
        const Bookmark& b = pair.second;
        file << "    {\n";
        file << "      \"id\": " << b.id << ",\n";
        file << "      \"url\": \"" << b.url << "\",\n";
        file << "      \"title\": \"" << b.title << "\",\n";
        file << "      \"folder\": \"" << b.folder << "\"\n";
        file << "    }";
    }
    
    file << "\n  ]\n";
    file << "}\n";
    
    impl_->filepath = filepath;
    std::cout << "[BookmarkManager] Saved " << impl_->bookmarks.size() 
              << " bookmarks to " << filepath << std::endl;
    
    return true;
}

bool BookmarkManager::load(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cout << "[BookmarkManager] No existing bookmarks file: " << filepath << std::endl;
        return false;
    }
    
    impl_->filepath = filepath;
    
    // Parse the JSON written by save()
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    
    size_t loaded = 0;
    size_t pos = 0;
    while ((pos = content.find("\"url\": \"", pos)) != std::string::npos) {
        auto extractField = [&](const std::string& field, size_t searchFrom) -> std::string {
            std::string needle = "\"" + field + "\": \"";
            auto fpos = content.find(needle, searchFrom);
            if (fpos == std::string::npos) return "";
            fpos += needle.size();
            auto end = content.find('"', fpos);
            if (end == std::string::npos) return "";
            return content.substr(fpos, end - fpos);
        };
        auto extractInt = [&](const std::string& field, size_t searchFrom) -> int {
            std::string needle = "\"" + field + "\": ";
            auto fpos = content.find(needle, searchFrom);
            if (fpos == std::string::npos) return 0;
            fpos += needle.size();
            try { return std::stoi(content.substr(fpos)); }
            catch (...) { return 0; }
        };
        
        // Find the enclosing object boundaries
        auto objStart = content.rfind('{', pos);
        auto objEnd = content.find('}', pos);
        if (objStart == std::string::npos || objEnd == std::string::npos) break;
        
        std::string url = extractField("url", objStart);
        if (url.empty()) { pos = objEnd + 1; continue; }
        
        Bookmark bookmark;
        bookmark.id = extractInt("id", objStart);
        if (bookmark.id == 0) bookmark.id = impl_->nextId++;
        else if (bookmark.id >= impl_->nextId) impl_->nextId = bookmark.id + 1;
        bookmark.url = url;
        bookmark.title = extractField("title", objStart);
        bookmark.folder = extractField("folder", objStart);
        bookmark.dateAdded = std::chrono::system_clock::now();
        
        impl_->bookmarks[bookmark.id] = bookmark;
        impl_->urlToId[url] = bookmark.id;
        loaded++;
        
        pos = objEnd + 1;
    }
    
    std::cout << "[BookmarkManager] Loaded " << loaded << " bookmarks from " << filepath << std::endl;
    return true;
}

bool BookmarkManager::exportToHTML(const std::string& filepath) {
    std::ofstream file(filepath);
    if (!file.is_open()) {
        return false;
    }
    
    file << "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n";
    file << "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n";
    file << "<TITLE>Bookmarks</TITLE>\n";
    file << "<H1>Bookmarks</H1>\n";
    file << "<DL><p>\n";
    
    for (const auto& pair : impl_->bookmarks) {
        const Bookmark& b = pair.second;
        file << "  <DT><A HREF=\"" << b.url << "\">" << b.title << "</A>\n";
    }
    
    file << "</DL><p>\n";
    
    std::cout << "[BookmarkManager] Exported to HTML: " << filepath << std::endl;
    return true;
}

bool BookmarkManager::importFromHTML(const std::string& filepath) {
    // TODO: HTML parsing
    std::cout << "[BookmarkManager] Import from HTML: " << filepath << std::endl;
    return false;
}

bool BookmarkManager::exportToJSON(const std::string& filepath) {
    return save(filepath);
}

bool BookmarkManager::importFromJSON(const std::string& filepath) {
    return load(filepath);
}

} // namespace ZepraBrowser
