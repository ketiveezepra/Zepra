// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file lazy_image_loader.cpp
 * @brief Implementation of deferred image loading
 */

#include "lazy_image_loader.h"
#include "layout_engine.h"
#include "svg_raster.h"
#include "web/paint/image_loader.h"
#include <iostream>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <cctype>
#include <cmath>
#include <cstdlib>
// HTTP client for fetching images
#include "http_client.hpp"

// Image decoding (stb_image is already included in main file, just declare here)
extern "C" {
    unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, 
                                          int* x, int* y, int* channels_in_file, 
                                          int desired_channels);
    void stbi_image_free(void* retval_from_stbi_load);
    const char* stbi_failure_reason(void);
}

namespace ZepraBrowser {

// Global instance
LazyImageLoader g_lazyImageLoader;

LazyImageLoader::LazyImageLoader() {
    std::cout << "[LazyImageLoader] Initialized" << std::endl;
}

LazyImageLoader::~LazyImageLoader() {
    stop();
}

void LazyImageLoader::start(int numThreads) {
    if (running_) return;
    
    running_ = true;
    
    for (int i = 0; i < numThreads; i++) {
        workers_.emplace_back(&LazyImageLoader::workerThread, this);
    }
    
    std::cout << "[LazyImageLoader] Started " << numThreads << " worker threads" << std::endl;
}

void LazyImageLoader::stop() {
    if (!running_) return;
    
    running_ = false;
    
    // Wake up all workers
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        // Clear queue
        while (!pendingQueue_.empty()) pendingQueue_.pop();
    }
    
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    
    std::cout << "[LazyImageLoader] Stopped" << std::endl;
}

// The loaded image on its box: the <img> itself, or the box's background
static void applyImage(LayoutBox* box, bool background, uint32_t textureId, const std::string& svgData,
                       int width, int height) {
    if (background) {
        box->bgTextureId = textureId;
        box->bgSvgData = svgData;
        box->bgNaturalWidth = (float)width;
        box->bgNaturalHeight = (float)height;
        return;
    }
    box->textureId = textureId;
    box->svgData = svgData;
    box->color = 0x000000;  // an SVG image is its own document: currentColor starts black
    box->naturalWidth = (float)width;
    box->naturalHeight = (float)height;
    box->text = "";
    box->hasBgColor = false;  // drop the loading placeholder's grey box
    box->isImage = true;
}

void LazyImageLoader::queueBackground(LayoutBox* box, const std::string& url, int tabId) {
    if (!box || url.empty()) return;
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        auto it = textureCache_.find(url);
        if (it != textureCache_.end()) {
            applyImage(box, true, it->second.textureId, it->second.svgData, it->second.width, it->second.height);
            return;
        }
    }
    PendingImage pending;
    pending.box = box;
    pending.url = url;
    pending.priority = 0;
    pending.tabId = tabId;
    pending.generation = generation_.load();
    pending.tabGeneration = tabGeneration(tabId);
    pending.background = true;
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pending.sequence = nextSequence_++;
    pendingQueue_.push(pending);
    pendingCount_++;
}

void LazyImageLoader::queueImage(LayoutBox* box, const std::string& url, 
                                  int priority, int tabId) {
    if (!box || url.empty()) return;
    
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        auto it = textureCache_.find(url);
        if (it != textureCache_.end()) {
            // Already loaded - update box directly
            applyImage(box, false, it->second.textureId, it->second.svgData, it->second.width, it->second.height);
            invalidateIntrinsicWidths();
            // std::cout << "[LazyImageLoader] Cache hit: " << url.substr(url.rfind('/') + 1) << std::endl;
            return;
        }
    }
    
    PendingImage pending;
    pending.box = box;
    pending.url = url;
    pending.priority = priority;
    pending.tabId = tabId;
    pending.generation = generation_.load();
    pending.tabGeneration = tabGeneration(tabId);
    
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending.sequence = nextSequence_++;
    pendingQueue_.push(pending);
        pendingCount_++;
    }
    
    // Set placeholder
    box->text = "[IMG: Loading...]";
    box->color = 0xAAAAAA;
}

void LazyImageLoader::queueImageWithViewport(LayoutBox* box, const std::string& url, 
                                              bool inViewport, int tabId) {
    // Viewport images get priority 100, others get 0
    int priority = inViewport ? 100 : 0;
    queueImage(box, url, priority, tabId);
}

uint64_t LazyImageLoader::tabGeneration(int tabId) {
    std::lock_guard<std::mutex> lock(tabGenMutex_);
    return tabGenerations_[tabId];
}

void LazyImageLoader::cancelTab(int tabId) {
    {
        std::lock_guard<std::mutex> lock(tabGenMutex_);
        ++tabGenerations_[tabId];
    }
    // Drop the tab's queued loads; ones a worker already took are dropped in pollCompleted
    std::lock_guard<std::mutex> lock(pendingMutex_);
    std::priority_queue<PendingImage> kept;
    while (!pendingQueue_.empty()) {
        if (pendingQueue_.top().tabId != tabId) kept.push(pendingQueue_.top());
        pendingQueue_.pop();
    }
    pendingQueue_.swap(kept);
    pendingCount_ = (int)pendingQueue_.size();
}

void LazyImageLoader::cancelAll() {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    while (!pendingQueue_.empty()) {
        pendingQueue_.pop();
    }
    pendingCount_ = 0;
    // Images already taken by a worker still point at the old boxes
    ++generation_;
    {
        std::lock_guard<std::mutex> rawLock(rawMutex_);
        rawImages_.clear();
    }
    std::cout << "[LazyImageLoader] All pending images cancelled" << std::endl;
}

void LazyImageLoader::pollCompleted(std::vector<ImageResult>& results, int maxResults) {
    // First, process raw images that need texture creation (main thread only)
    {
        std::lock_guard<std::mutex> lock(rawMutex_);
        const uint64_t currentGeneration = generation_.load();
        for (auto& raw : rawImages_) {
            // A box replaced by a relayout (or a closed tab) is gone, but the image
            // still goes into the cache: the new layout asks for the same URL
            bool boxAlive = raw.box && raw.generation == currentGeneration &&
                            raw.tabGeneration == tabGeneration(raw.tabId);
            if (!raw.errorText.empty()) {
                if (!boxAlive || raw.background) continue;
                invalidateIntrinsicWidths();  // table columns measured the placeholder
                raw.box->text = raw.errorText;
                raw.box->color = raw.errorColor;
                continue;
            }

            CachedTexture entry;
            if (raw.width < 0) {
                // SVG data (tagged with negative width); painted by the main thread at box size
                entry.svgData = std::string(raw.pixels.begin(), raw.pixels.end());
                entry.width = -raw.width;
                entry.height = raw.height;
            } else if (textureCreator_ && !raw.pixels.empty()) {
                // Raster image — create GPU texture
                entry.textureId = textureCreator_(raw.width, raw.height, raw.pixels.data());
                entry.width = raw.width;
                entry.height = raw.height;
                if (!entry.textureId) {
                    if (boxAlive && !raw.background) {
                        raw.box->text = "[IMG: GPU Error]";
                        raw.box->color = 0xFF0000;
                    }
                    continue;
                }
            } else {
                continue;
            }
            if (!raw.url.empty()) {
                std::lock_guard<std::mutex> cacheLock(cacheMutex_);
                textureCache_[raw.url] = entry;
            }
            if (!boxAlive) continue;
            if (!raw.background) invalidateIntrinsicWidths();  // sizes change; table columns measured the placeholder
            applyImage(raw.box, raw.background, entry.textureId, entry.svgData, entry.width, entry.height);
        }

        rawImages_.clear();
    }
    
    // Then return completed results
    std::lock_guard<std::mutex> lock(completedMutex_);
    int count = maxResults > 0 ? std::min(maxResults, (int)completedQueue_.size()) 
                               : (int)completedQueue_.size();
    
    for (int i = 0; i < count; i++) {
        results.push_back(completedQueue_.back());
        completedQueue_.pop_back();
    }
}

void LazyImageLoader::workerThread() {
    std::cout << "[LazyImageLoader] Worker thread started" << std::endl;
    
    while (running_) {
        PendingImage pending;
        bool hasPending = false;
        
        // Get next image from queue
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            if (!pendingQueue_.empty()) {
                pending = pendingQueue_.top();
                pendingQueue_.pop();
                pendingCount_--;
                hasPending = true;
            }
        }
        
        if (hasPending) {
            // Load the image
            ImageResult result = loadImage(pending);
            
            // Add to completed queue
            {
                std::lock_guard<std::mutex> lock(completedMutex_);
                completedQueue_.push_back(result);
            }
        } else {
            // No work - sleep briefly
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    
    std::cout << "[LazyImageLoader] Worker thread stopped" << std::endl;
}

void LazyImageLoader::queueError(const PendingImage& pending, const char* text, uint32_t color) {
    if (!pending.box) return;
    std::lock_guard<std::mutex> lock(rawMutex_);
    RawImage raw;
    raw.box = pending.box;
    raw.width = 0;
    raw.height = 0;
    raw.generation = pending.generation;
    raw.tabId = pending.tabId;
    raw.tabGeneration = pending.tabGeneration;
    raw.background = pending.background;
    raw.errorText = text;

    raw.errorColor = color;
    rawImages_.push_back(std::move(raw));
}

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percentDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0) {
            out += (char)(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

bool base64Decode(const std::string& in, std::vector<uint8_t>& out) {
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=' || std::isspace((unsigned char)c)) continue;
        else return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)(acc >> bits));
        }
    }
    return true;
}

// data:[<mediatype>][;base64],<data>
bool decodeDataUrl(const std::string& url, std::vector<uint8_t>& data, std::string& contentType) {
    size_t comma = url.find(',');
    if (comma == std::string::npos) return false;
    std::string meta = url.substr(5, comma - 5);
    std::transform(meta.begin(), meta.end(), meta.begin(), ::tolower);
    bool isBase64 = meta.size() >= 7 && meta.compare(meta.size() - 7, 7, ";base64") == 0;
    contentType = meta.substr(0, meta.find(';'));
    std::string payload = percentDecode(url.substr(comma + 1));
    if (isBase64) return base64Decode(payload, data);
    data.assign(payload.begin(), payload.end());
    return true;
}

// The format from the first bytes; "" when they say nothing
std::string sniffFormat(const std::vector<uint8_t>& data) {
    const uint8_t* b = data.data();
    size_t n = data.size();
    if (n >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return "png";
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return "jpeg";
    if (n >= 6 && std::memcmp(b, "GIF8", 4) == 0) return "gif";
    if (n >= 12 && std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WEBP", 4) == 0) return "webp";
    if (n >= 2 && b[0] == 'B' && b[1] == 'M') return "bmp";
    // SVG: text whose first element (after a BOM, <?xml?>, comments, doctype) is <svg
    std::string head((const char*)b, std::min(n, (size_t)4096));
    size_t i = (head.size() >= 3 && (uint8_t)head[0] == 0xEF && (uint8_t)head[1] == 0xBB &&
                (uint8_t)head[2] == 0xBF) ? 3 : 0;
    while (i < head.size()) {
        while (i < head.size() && std::isspace((unsigned char)head[i])) i++;
        if (head.compare(i, 4, "<svg") == 0) return "svg";
        if (head.compare(i, 4, "<!--") == 0) {
            size_t e = head.find("-->", i);
            if (e == std::string::npos) break;
            i = e + 3;
        } else if (head.compare(i, 2, "<?") == 0 || head.compare(i, 2, "<!") == 0) {
            size_t e = head.find('>', i);
            if (e == std::string::npos) break;
            i = e + 1;
        } else {
            break;
        }
    }
    return "";
}

std::string formatFromType(std::string type) {
    std::transform(type.begin(), type.end(), type.begin(), ::tolower);
    if (type.find("svg") != std::string::npos) return "svg";
    if (type.find("image/") != std::string::npos) return "raster";
    return "";
}

std::string formatFromExtension(std::string url) {
    std::transform(url.begin(), url.end(), url.begin(), ::tolower);
    url = url.substr(0, url.find_first_of("?#"));
    auto ends = [&](const char* ext) {
        size_t len = std::strlen(ext);
        return url.size() >= len && url.compare(url.size() - len, len, ext) == 0;
    };
    if (ends(".svg")) return "svg";
    if (ends(".png") || ends(".jpg") || ends(".jpeg") || ends(".gif") || ends(".webp") || ends(".bmp"))
        return "raster";
    return "";
}

} // namespace

ImageResult LazyImageLoader::loadImage(const PendingImage& pending) {
    ImageResult result;
    result.box = pending.box;
    result.success = false;
    result.textureId = 0;
    result.width = 0;
    result.height = 0;

    // Guard against stale box pointer (can happen after page navigation/repaint)
    if (!pending.box) {
        result.error = "Box pointer is null";
        return result;
    }
    // Every failure leaves a placeholder, never a box stuck on "Loading..."
    auto fail = [&](const std::string& error, const char* text, uint32_t color) {
        result.error = error;
        std::cout << "[LazyImageLoader] " << error << ": " << pending.url.substr(0, 120) << std::endl;
        queueError(pending, text, color);
        return result;
    };

    try {
        std::vector<uint8_t> data;
        std::string contentType;

        if (pending.url.compare(0, 5, "data:") == 0) {
            if (!decodeDataUrl(pending.url, data, contentType))
                return fail("Bad data: URL", "[IMG: Decode Failed]", 0xFF0000);
        } else if (pending.url.substr(0, 7) == "file://") {
            // Handle file:// URLs — read from local filesystem
            std::string filePath = percentDecode(pending.url.substr(7));
            std::ifstream file(filePath, std::ios::binary);
            if (!file.is_open()) {
                // Try stripping leading slash if present
                if (!filePath.empty() && filePath[0] == '/') {
                    file.open(filePath.substr(1), std::ios::binary);
                }
            }
            if (!file.is_open()) return fail("File not found", "[IMG: Not Found]", 0xFF6600);
            data = std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                                         std::istreambuf_iterator<char>());
        } else {
            // HTTP fetch; certificates are verified like page loads (#14)
            Zepra::Networking::HttpClientConfig config;
            config.connectTimeoutMs = 10000;
            config.readTimeoutMs = 30000;
            config.followRedirects = true;
            config.maxRedirects = 10;
            config.verifySsl = true;
            config.userAgent = "ZepraBrowser/1.0 (NeolyxOS)";

            Zepra::Networking::HttpClient client(config);
            Zepra::Networking::HttpRequest request(Zepra::Networking::HttpMethod::GET, pending.url);
            auto response = client.send(request);
            if (!response.isSuccess()) {
                std::string error = response.error().empty()
                    ? "HTTP " + std::to_string(response.statusCode()) : response.error();
                return fail(error, "[IMG: Load Failed]", 0xFF6600);
            }
            data = response.body();
            contentType = response.header("Content-Type");
            // A body shorter than its Content-Length (NXNet's TLS reader can stop
            // early) is still decoded: a cut GIF or JPEG shows its top part
            std::string length = response.header("Content-Length");
            if (!length.empty() && data.size() < std::strtoull(length.c_str(), nullptr, 10)) {
                std::cout << "[LazyImageLoader] Truncated (" << data.size() << " of " << length
                          << " bytes): " << pending.url.substr(0, 120) << std::endl;
            }

        }

        if (data.empty()) return fail("Empty response", "[IMG: Load Failed]", 0xFF6600);

        // Format: the bytes first (servers and file names get it wrong), then
        // Content-Type / data: media type, then the URL's extension
        std::string format = sniffFormat(data);
        if (format.empty()) format = formatFromType(contentType);
        if (format.empty()) format = formatFromExtension(pending.url);

        // Handle SVG — queue data for main-thread processing (never write box from worker)
        if (format == "svg") {
            std::string svgStr(data.begin(), data.end());
            float svgW = 0, svgH = 0;
            if (!SvgRaster::intrinsicSize(svgStr, svgW, svgH))
                return fail("Not an SVG document", "[IMG: Decode Failed]", 0xFF0000);
            int w = std::max(1, (int)std::lround(svgW)), h = std::max(1, (int)std::lround(svgH));

            // Queue SVG data for main thread (do NOT write to box here)
            {
                std::lock_guard<std::mutex> lock(rawMutex_);
                RawImage raw;
                raw.box = pending.box;
                raw.generation = pending.generation;
                raw.tabId = pending.tabId;
                raw.tabGeneration = pending.tabGeneration;
                raw.background = pending.background;
                raw.url = pending.url;
                // Store SVG XML as pixel data marker; negative width = SVG flag
                raw.pixels.assign(data.begin(), data.end());
                raw.width = -w;
                raw.height = h;
                rawImages_.push_back(std::move(raw));
            }

            result.success = true;
            result.width = w;
            result.height = h;
            return result;
        }

        // Unknown format - show warning instead of trying to decode
        if (format.empty()) {
            return fail("Unknown image format (" + (contentType.empty() ? std::string("no type") : contentType) + ")",
                        "[IMG: Unknown]", 0xFF6600);  // Orange for warnings
        }

        // Decode raster image: WebP through libwebp (NXRender's WebPDecoder),
        // PNG, JPEG, GIF (first frame) and BMP through stb_image
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        if (format == "webp") {
            NXRender::DecodedImage image = NXRender::WebPDecoder().decode(data.data(), data.size());
            if (!image.valid()) return fail("WebP decode failed", "[IMG: Decode Failed]", 0xFF0000);
            w = image.width;
            h = image.height;
            rgba = std::move(image.pixels);
        } else {
            int channels;
            unsigned char* pixels = stbi_load_from_memory(data.data(), (int)data.size(),
                                                           &w, &h, &channels, 4); // Force RGBA
            if (!pixels) return fail(stbi_failure_reason(), "[IMG: Decode Failed]", 0xFF0000);
            rgba.assign(pixels, pixels + (size_t)w * h * 4);
            stbi_image_free(pixels);
        }

        result.width = w;
        result.height = h;
        result.success = true;

        // Queue raw pixels for main thread texture creation
        {
            std::lock_guard<std::mutex> lock(rawMutex_);
            RawImage raw;
            raw.box = pending.box;
            raw.generation = pending.generation;
            raw.tabId = pending.tabId;
            raw.tabGeneration = pending.tabGeneration;
            raw.background = pending.background;
            raw.url = pending.url;
            raw.width = w;
            raw.height = h;
            raw.pixels = std::move(rgba);
            rawImages_.push_back(std::move(raw));
        }

    } catch (const std::exception& e) {
        result.error = e.what();
        queueError(pending, "[IMG: Error]", 0xFF0000);
    }

    return result;
}

} // namespace ZepraBrowser
