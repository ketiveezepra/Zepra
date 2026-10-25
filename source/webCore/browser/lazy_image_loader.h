// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file lazy_image_loader.h
 * @brief Deferred image loading for ZepraBrowser
 * 
 * Instead of blocking page render while loading all images,
 * this queues images for background loading and updates
 * LayoutBoxes when images are ready.
 */

#pragma once

#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace ZepraBrowser {

// Forward declaration
struct LayoutBox;

/**
 * @brief Entry for a pending image load
 */
struct PendingImage {
    LayoutBox* box;           // Target box to update
    std::string url;          // Image URL
    int priority;             // Higher = load first (viewport images get higher priority)
    int tabId;                // Tab that requested this image
    uint64_t generation = 0;  // Loader generation when queued (see cancelAll)
    uint64_t tabGeneration = 0;  // The tab's generation when queued (see cancelTab)
    bool background = false;  // background-image: fills box->bg*, no placeholder
    uint64_t sequence = 0;    // queue order: equal priorities load first come, first served
    
    bool operator<(const PendingImage& other) const {
        // Lower priority = later in queue; then later-queued = later
        if (priority != other.priority) return priority < other.priority;
        return sequence > other.sequence;
    }
};

/**
 * @brief Result of loading an image
 */
struct ImageResult {
    LayoutBox* box;
    uint32_t textureId;
    int width;
    int height;
    bool success;
    std::string error;
};

/**
 * @brief Lazy image loader with priority queue and viewport awareness
 * 
 * Images are loaded in background threads and results are polled
 * during the render loop to update LayoutBoxes.
 */
class LazyImageLoader {
public:
    LazyImageLoader();
    ~LazyImageLoader();
    
    /**
     * @brief Queue an image for loading
     * @param box Target LayoutBox to update when loaded
     * @param url Image URL
     * @param priority Higher = load first
     * @param tabId Tab ID for cancellation
     */
    void queueImage(LayoutBox* box, const std::string& url, int priority = 0, int tabId = -1);

    /**
     * @brief Queue a CSS background-image for a box (bgTextureId / bgSvgData);
     * no placeholder text, and a failed load leaves the box as it is
     */
    void queueBackground(LayoutBox* box, const std::string& url, int tabId = -1);
    
    /**
     * @brief Queue image with viewport priority
     * @param box Target box
     * @param url Image URL
     * @param inViewport True if image is currently visible
     * @param tabId Tab ID
     */
    void queueImageWithViewport(LayoutBox* box, const std::string& url, 
                                 bool inViewport, int tabId = -1);
    
    /**
     * @brief Cancel all pending images for a tab (e.g., on navigation)
     */
    void cancelTab(int tabId);
    
    /**
     * @brief Cancel all pending images
     */
    void cancelAll();
    
    /**
     * @brief Get completed images (call from main/render thread)
     * @param results Output vector for completed images
     * @param maxResults Maximum results to return (0 = all)
     */
    void pollCompleted(std::vector<ImageResult>& results, int maxResults = 10);
    
    /**
     * @brief Check if there are pending images
     */
    bool hasPending() const { return pendingCount_ > 0; }
    
    /**
     * @brief Get count of pending images
     */
    int pendingCount() const { return pendingCount_; }
    
    /**
     * @brief Start background loading threads
     */
    void start(int numThreads = 2);
    
    /**
     * @brief Stop background loading
     */
    void stop();
    
    /**
     * @brief Set callback for GPU texture creation (must be called from main thread)
     */
    using TextureCreator = std::function<uint32_t(int width, int height, const uint8_t* pixels)>;
    void setTextureCreator(TextureCreator creator) { textureCreator_ = creator; }
    
private:
    void workerThread();
    ImageResult loadImage(const PendingImage& pending);
    
    // Thread-safe queues
    std::priority_queue<PendingImage> pendingQueue_;
    std::mutex pendingMutex_;
    uint64_t nextSequence_ = 0;  // guarded by pendingMutex_
    
    std::vector<ImageResult> completedQueue_;
    std::mutex completedMutex_;
    
    // Raw image data waiting for texture creation (main thread only)
    struct RawImage {
        LayoutBox* box;
        std::vector<uint8_t> pixels;
        int width;
        int height;
        uint64_t generation = 0;
        int tabId = -1;
        uint64_t tabGeneration = 0;
        std::string url;           // key for textureCache_
        bool background = false;   // see PendingImage::background
        std::string errorText;     // non-empty: show this placeholder instead
        uint32_t errorColor = 0;
    };
    std::vector<RawImage> rawImages_;
    std::mutex rawMutex_;

    // Bumped by cancelAll(): results queued under an older generation refer
    // to layout boxes that may have been destroyed, and are dropped.
    std::atomic<uint64_t> generation_{0};

    // Per-tab generations, bumped by cancelTab() the same way
    std::unordered_map<int, uint64_t> tabGenerations_;
    std::mutex tabGenMutex_;
    uint64_t tabGeneration(int tabId);

    // Hands a failure placeholder to the main thread (workers never touch boxes)
    void queueError(const PendingImage& pending, const char* text, uint32_t color);
    
    // Worker threads
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
    std::atomic<int> pendingCount_{0};
    
    // Texture creator (main thread callback)
    TextureCreator textureCreator_;
    
    // URL cache to avoid duplicate loads, filled when an image arrives (also when
    // its box was replaced by a relayout in the meantime, so the next layout hits it)
    struct CachedTexture {
        uint32_t textureId = 0;
        int width = 0;
        int height = 0;
        std::string svgData;       // SVG images: the document (textureId stays 0)
    };
    std::unordered_map<std::string, CachedTexture> textureCache_;
    std::mutex cacheMutex_;
};

// Global instance
extern LazyImageLoader g_lazyImageLoader;

} // namespace ZepraBrowser
