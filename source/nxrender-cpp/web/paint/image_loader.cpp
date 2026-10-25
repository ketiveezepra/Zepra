// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "image_loader.h"
#include <cstring>
#include <algorithm>
#include <sstream>
#include <cctype>
#include <cmath>

#ifdef NXRENDER_HAVE_WEBP
#include <webp/decode.h>
#endif

namespace NXRender {

// ==================================================================
// ImageDecoder — default implementations
// ==================================================================

DecodedImage ImageDecoder::decodeAtSize(const uint8_t* data, size_t size,
                                          int targetWidth, int targetHeight) const {
    auto full = decode(data, size);
    if (!full.valid() || (full.width <= targetWidth && full.height <= targetHeight)) {
        return full;
    }
    return ImageTransform::resize(full, targetWidth, targetHeight);
}

std::vector<AnimationFrame> ImageDecoder::decodeAnimation(const uint8_t* data, size_t size) const {
    std::vector<AnimationFrame> frames;
    auto img = decode(data, size);
    if (img.valid()) {
        AnimationFrame f;
        f.image = std::move(img);
        f.delay = 0;
        frames.push_back(std::move(f));
    }
    return frames;
}

DecodedImage ImageDecoder::decodeProgressive(const uint8_t* data, size_t size, int /*pass*/) const {
    return decode(data, size);
}

// ==================================================================
// PNGDecoder
// ==================================================================

bool PNGDecoder::canDecode(const uint8_t* data, size_t size) const {
    if (size < 8) return false;
    return data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G';
}

ImageMetadata PNGDecoder::probe(const uint8_t* data, size_t size) const {
    ImageMetadata meta;
    meta.format = ImageFormat::PNG;
    meta.mimeType = "image/png";
    meta.fileSize = size;
    if (size >= 24) {
        // IHDR starts at offset 8, width at 16, height at 20 (big-endian)
        meta.width = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
        meta.height = (data[20] << 24) | (data[21] << 16) | (data[22] << 8) | data[23];
        meta.bitDepth = data[24];
        int colorType = data[25];
        switch (colorType) {
            case 0: meta.channels = 1; break;
            case 2: meta.channels = 3; break;
            case 3: meta.channels = 1; break; // Palette
            case 4: meta.channels = 2; break;
            case 6: meta.channels = 4; break;
            default: meta.channels = 4;
        }
    }
    return meta;
}

DecodedImage PNGDecoder::decode(const uint8_t* data, size_t size) const {
    DecodedImage result;
    auto meta = probe(data, size);
    result.width = meta.width;
    result.height = meta.height;
    result.sourceFormat = ImageFormat::PNG;
    result.stride = meta.width * 4;
    // Actual inflate + unfilter would go here
    // For structural correctness: allocate RGBA buffer
    result.pixels.resize(static_cast<size_t>(result.stride) * result.height, 0);
    (void)data; (void)size;
    return result;
}

DecodedImage PNGDecoder::decodeProgressive(const uint8_t* data, size_t size, int /*pass*/) const {
    return decode(data, size);
}

// ==================================================================
// JPEGDecoder
// ==================================================================

bool JPEGDecoder::canDecode(const uint8_t* data, size_t size) const {
    if (size < 2) return false;
    return data[0] == 0xFF && data[1] == 0xD8;
}

ImageMetadata JPEGDecoder::probe(const uint8_t* data, size_t size) const {
    ImageMetadata meta;
    meta.format = ImageFormat::JPEG;
    meta.mimeType = "image/jpeg";
    meta.fileSize = size;
    meta.channels = 3;
    meta.bitDepth = 8;

    // Scan for SOF marker to get dimensions
    size_t pos = 2;
    while (pos + 4 < size) {
        if (data[pos] != 0xFF) { pos++; continue; }
        uint8_t marker = data[pos + 1];
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            // SOF0/SOF1/SOF2
            if (pos + 9 < size) {
                meta.height = (data[pos + 5] << 8) | data[pos + 6];
                meta.width = (data[pos + 7] << 8) | data[pos + 8];
                meta.channels = data[pos + 9];
            }
            break;
        }
        // Skip to next marker
        uint16_t segLen = (data[pos + 2] << 8) | data[pos + 3];
        pos += 2 + segLen;
    }
    return meta;
}

DecodedImage JPEGDecoder::decode(const uint8_t* data, size_t size) const {
    DecodedImage result;
    auto meta = probe(data, size);
    result.width = meta.width;
    result.height = meta.height;
    result.sourceFormat = ImageFormat::JPEG;
    result.stride = meta.width * 4;
    result.pixels.resize(static_cast<size_t>(result.stride) * result.height, 255);
    (void)data;
    return result;
}

DecodedImage JPEGDecoder::decodeAtSize(const uint8_t* data, size_t size,
                                          int targetWidth, int targetHeight) const {
    // JPEG supports IDCT scaling: 1/1, 1/2, 1/4, 1/8
    auto meta = probe(data, size);
    int scale = 1;
    while (meta.width / (scale * 2) >= targetWidth &&
           meta.height / (scale * 2) >= targetHeight && scale < 8) {
        scale *= 2;
    }
    // Would pass scale to libjpeg's scale_denom
    DecodedImage result;
    result.width = meta.width / scale;
    result.height = meta.height / scale;
    result.sourceFormat = ImageFormat::JPEG;
    result.stride = result.width * 4;
    result.pixels.resize(static_cast<size_t>(result.stride) * result.height, 255);
    (void)data;
    return result;
}

// ==================================================================
// WebPDecoder
// ==================================================================

bool WebPDecoder::canDecode(const uint8_t* data, size_t size) const {
    if (size < 12) return false;
    return data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
           data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P';
}

ImageMetadata WebPDecoder::probe(const uint8_t* data, size_t size) const {
    ImageMetadata meta;
    meta.format = ImageFormat::WebP;
    meta.mimeType = "image/webp";
    meta.fileSize = size;
    meta.channels = 4;
    // VP8 header parsing simplified
    if (size > 30) {
        // Check for VP8 lossy
        if (data[12] == 'V' && data[13] == 'P' && data[14] == '8' && data[15] == ' ') {
            if (size > 26) {
                meta.width = (data[26] | (data[27] << 8)) & 0x3FFF;
                meta.height = (data[28] | (data[29] << 8)) & 0x3FFF;
            }
        }
        // VP8L lossless
        if (data[12] == 'V' && data[13] == 'P' && data[14] == '8' && data[15] == 'L') {
            if (size > 25) {
                uint32_t bits = data[21] | (data[22] << 8) | (data[23] << 16) | (data[24] << 24);
                meta.width = (bits & 0x3FFF) + 1;
                meta.height = ((bits >> 14) & 0x3FFF) + 1;
            }
        }
    }
    return meta;
}

DecodedImage WebPDecoder::decode(const uint8_t* data, size_t size) const {
    DecodedImage result;
    result.sourceFormat = ImageFormat::WebP;
#ifdef NXRENDER_HAVE_WEBP
    // Lossy, lossless and alpha; animated files decode to their first frame
    int width = 0, height = 0;
    uint8_t* rgba = WebPDecodeRGBA(data, size, &width, &height);
    if (rgba) {
        result.width = width;
        result.height = height;
        result.stride = width * 4;
        result.pixels.assign(rgba, rgba + static_cast<size_t>(result.stride) * height);
        WebPFree(rgba);
    }
#else
    // Built without libwebp: an empty (invalid) image
    (void)data;
    (void)size;
#endif
    return result;
}

std::vector<AnimationFrame> WebPDecoder::decodeAnimation(const uint8_t* data, size_t size) const {
    // Would use libwebp's animation API
    return ImageDecoder::decodeAnimation(data, size);
}

// ==================================================================
// GIFDecoder
// ==================================================================

bool GIFDecoder::canDecode(const uint8_t* data, size_t size) const {
    if (size < 6) return false;
    return (data[0] == 'G' && data[1] == 'I' && data[2] == 'F');
}

ImageMetadata GIFDecoder::probe(const uint8_t* data, size_t size) const {
    ImageMetadata meta;
    meta.format = ImageFormat::GIF;
    meta.mimeType = "image/gif";
    meta.fileSize = size;
    meta.channels = 4;
    meta.bitDepth = 8;
    if (size >= 10) {
        meta.width = data[6] | (data[7] << 8);
        meta.height = data[8] | (data[9] << 8);
    }
    // Check for animation (multiple image blocks)
    meta.animated = false;
    int imageCount = 0;
    size_t pos = 13; // Skip header + LSD
    if (size > 13 && (data[10] & 0x80)) {
        int gctSize = 3 * (1 << ((data[10] & 0x07) + 1));
        pos += gctSize;
    }
    while (pos < size && imageCount < 3) {
        if (data[pos] == 0x2C) imageCount++;
        if (data[pos] == 0x3B) break;
        pos++;
    }
    meta.animated = (imageCount > 1);
    meta.frameCount = imageCount;
    return meta;
}

DecodedImage GIFDecoder::decode(const uint8_t* data, size_t size) const {
    DecodedImage result;
    auto meta = probe(data, size);
    result.width = meta.width;
    result.height = meta.height;
    result.sourceFormat = ImageFormat::GIF;
    result.stride = meta.width * 4;
    result.pixels.resize(static_cast<size_t>(result.stride) * result.height, 0);
    (void)data;
    return result;
}

std::vector<AnimationFrame> GIFDecoder::decodeAnimation(const uint8_t* data, size_t size) const {
    // Would use LZW decompression + frame compositing
    return ImageDecoder::decodeAnimation(data, size);
}

// ==================================================================
// AVIFDecoder
// ==================================================================

bool AVIFDecoder::canDecode(const uint8_t* data, size_t size) const {
    if (size < 12) return false;
    // ISOBMFF: check for 'ftyp' box with 'avif' brand
    return data[4] == 'f' && data[5] == 't' && data[6] == 'y' && data[7] == 'p' &&
           data[8] == 'a' && data[9] == 'v' && data[10] == 'i' && data[11] == 'f';
}

ImageMetadata AVIFDecoder::probe(const uint8_t* data, size_t size) const {
    ImageMetadata meta;
    meta.format = ImageFormat::AVIF;
    meta.mimeType = "image/avif";
    meta.fileSize = size;
    meta.channels = 4;
    (void)data;
    return meta;
}

DecodedImage AVIFDecoder::decode(const uint8_t* data, size_t size) const {
    DecodedImage result;
    auto meta = probe(data, size);
    result.width = meta.width;
    result.height = meta.height;
    result.sourceFormat = ImageFormat::AVIF;
    (void)data;
    return result;
}

// ==================================================================
// ImageResource
// ==================================================================

ImageResource::ImageResource(const std::string& url) : url_(url) {}
ImageResource::~ImageResource() {}

const AnimationFrame* ImageResource::frame(size_t index) const {
    return (index < frames_.size()) ? &frames_[index] : nullptr;
}

void ImageResource::advanceFrame(float deltaMs) {
    if (frames_.size() <= 1) return;

    frameTimer_ += deltaMs / 1000.0f;
    float frameDelay = frames_[currentFrame_].delay;
    if (frameDelay <= 0) frameDelay = 0.1f;

    if (frameTimer_ >= frameDelay) {
        frameTimer_ -= frameDelay;
        currentFrame_ = (currentFrame_ + 1) % frames_.size();
    }
}

void ImageResource::setHeaderData(const ImageMetadata& meta) {
    metadata_ = meta;
    state_ = ImageLoadState::HeaderDecoded;
}

void ImageResource::setDecodedData(DecodedImage&& decoded) {
    decoded_ = std::move(decoded);
    metadata_.width = decoded_.width;
    metadata_.height = decoded_.height;
    state_ = ImageLoadState::Complete;
    if (onLoad_) onLoad_(this);
}

void ImageResource::setAnimationFrames(std::vector<AnimationFrame>&& frames) {
    frames_ = std::move(frames);
    if (!frames_.empty()) {
        decoded_ = frames_[0].image;
        metadata_.animated = true;
        metadata_.frameCount = static_cast<int>(frames_.size());
    }
    state_ = ImageLoadState::Complete;
    if (onLoad_) onLoad_(this);
}

void ImageResource::setError(const std::string& message) {
    errorMessage_ = message;
    state_ = ImageLoadState::Error;
    if (onError_) onError_(this);
}

// ==================================================================
// ImageCache
// ==================================================================

ImageCache& ImageCache::instance() {
    static ImageCache cache;
    return cache;
}

std::shared_ptr<ImageResource> ImageCache::get(const std::string& url) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(url);
    if (it != cache_.end()) {
        hits_++;
        // Move to front of LRU
        lruOrder_.erase(std::remove(lruOrder_.begin(), lruOrder_.end(), url), lruOrder_.end());
        lruOrder_.push_back(url);
        return it->second;
    }
    misses_++;
    return nullptr;
}

void ImageCache::put(const std::string& url, std::shared_ptr<ImageResource> resource) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Evict if over budget
    while (memorySizeBytes() > maxMemory_ && !lruOrder_.empty()) {
        std::string oldest = lruOrder_.front();
        lruOrder_.erase(lruOrder_.begin());
        cache_.erase(oldest);
    }

    cache_[url] = resource;
    lruOrder_.erase(std::remove(lruOrder_.begin(), lruOrder_.end(), url), lruOrder_.end());
    lruOrder_.push_back(url);
}

void ImageCache::remove(const std::string& url) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.erase(url);
    lruOrder_.erase(std::remove(lruOrder_.begin(), lruOrder_.end(), url), lruOrder_.end());
}

void ImageCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
    lruOrder_.clear();
}

size_t ImageCache::memorySizeBytes() const {
    size_t total = 0;
    for (const auto& [_, res] : cache_) {
        total += res->decoded().sizeBytes();
    }
    return total;
}

void ImageCache::evictLRU() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (memorySizeBytes() > maxMemory_ && !lruOrder_.empty()) {
        std::string oldest = lruOrder_.front();
        lruOrder_.erase(lruOrder_.begin());
        cache_.erase(oldest);
    }
}

size_t ImageCache::entryCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

// ==================================================================
// SrcsetResolver
// ==================================================================

std::vector<SrcsetCandidate> SrcsetResolver::parse(const std::string& srcset) {
    std::vector<SrcsetCandidate> result;
    std::istringstream stream(srcset);
    std::string token;

    while (std::getline(stream, token, ',')) {
        while (!token.empty() && std::isspace(token.front())) token.erase(token.begin());
        while (!token.empty() && std::isspace(token.back())) token.pop_back();

        if (token.empty()) continue;

        SrcsetCandidate candidate;
        auto space = token.rfind(' ');
        if (space != std::string::npos) {
            candidate.url = token.substr(0, space);
            std::string desc = token.substr(space + 1);
            while (!desc.empty() && std::isspace(desc.front())) desc.erase(desc.begin());

            if (!desc.empty() && desc.back() == 'w') {
                candidate.widthDescriptor = std::strtof(desc.c_str(), nullptr);
            } else if (!desc.empty() && desc.back() == 'x') {
                candidate.pixelDensity = std::strtof(desc.c_str(), nullptr);
            }
        } else {
            candidate.url = token;
            candidate.pixelDensity = 1.0f;
        }

        result.push_back(candidate);
    }
    return result;
}

std::string SrcsetResolver::selectBest(const std::vector<SrcsetCandidate>& candidates,
                                          int layoutWidth, float devicePixelRatio) {
    if (candidates.empty()) return "";

    float targetWidth = layoutWidth * devicePixelRatio;

    // Prefer width descriptors if available
    bool hasWidth = false;
    for (const auto& c : candidates) {
        if (c.widthDescriptor > 0) { hasWidth = true; break; }
    }

    if (hasWidth && targetWidth > 0) {
        // Find smallest candidate >= target width
        const SrcsetCandidate* best = nullptr;
        float bestDist = std::numeric_limits<float>::max();

        for (const auto& c : candidates) {
            if (c.widthDescriptor <= 0) continue;
            float dist = c.widthDescriptor - targetWidth;
            if (dist >= 0 && dist < bestDist) {
                bestDist = dist;
                best = &c;
            }
        }
        // Fallback: largest available
        if (!best) {
            for (const auto& c : candidates) {
                if (c.widthDescriptor > 0 &&
                    (best == nullptr || c.widthDescriptor > best->widthDescriptor)) {
                    best = &c;
                }
            }
        }
        return best ? best->url : candidates[0].url;
    }

    // Use pixel density descriptors
    const SrcsetCandidate* best = nullptr;
    float bestDiff = std::numeric_limits<float>::max();
    for (const auto& c : candidates) {
        float density = (c.pixelDensity > 0) ? c.pixelDensity : 1.0f;
        float diff = std::abs(density - devicePixelRatio);
        if (diff < bestDiff) { bestDiff = diff; best = &c; }
    }
    return best ? best->url : candidates[0].url;
}

int SrcsetResolver::parseSizes(const std::string& sizes, int viewportWidth) {
    std::istringstream stream(sizes);
    std::string entry;

    auto parseLength = [&](const std::string& s) -> int {
        float v = std::strtof(s.c_str(), nullptr);
        if (s.find("vw") != std::string::npos) return static_cast<int>(v * viewportWidth * 0.01f);
        if (s.find("em") != std::string::npos) return static_cast<int>(v * 16);
        return static_cast<int>(v); // px or bare number
    };

    while (std::getline(stream, entry, ',')) {
        while (!entry.empty() && std::isspace(entry.front())) entry.erase(entry.begin());

        auto parenOpen = entry.find('(');
        auto parenClose = entry.find(')');
        if (parenOpen != std::string::npos && parenClose != std::string::npos) {
            std::string condition = entry.substr(parenOpen + 1, parenClose - parenOpen - 1);
            std::string sizeValue = entry.substr(parenClose + 1);
            while (!sizeValue.empty() && std::isspace(sizeValue.front()))
                sizeValue.erase(sizeValue.begin());

            auto colonPos = condition.find(':');
            if (colonPos != std::string::npos) {
                std::string val = condition.substr(colonPos + 1);
                while (!val.empty() && std::isspace(val.front())) val.erase(val.begin());
                int maxWidth = static_cast<int>(std::strtof(val.c_str(), nullptr));
                if (viewportWidth <= maxWidth) {
                    return parseLength(sizeValue);
                }
            }
        } else {
            return parseLength(entry);
        }
    }
    return viewportWidth;
}

// ==================================================================
// ImageFormatDetector
// ==================================================================

ImageFormat ImageFormatDetector::detect(const uint8_t* data, size_t size) {
    if (size < 4) return ImageFormat::Unknown;

    // PNG
    if (size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G')
        return ImageFormat::PNG;
    // JPEG
    if (data[0] == 0xFF && data[1] == 0xD8)
        return ImageFormat::JPEG;
    // GIF
    if (data[0] == 'G' && data[1] == 'I' && data[2] == 'F')
        return ImageFormat::GIF;
    // WebP
    if (size >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P')
        return ImageFormat::WebP;
    // AVIF
    if (size >= 12 && data[4] == 'f' && data[5] == 't' && data[6] == 'y' && data[7] == 'p' &&
        data[8] == 'a' && data[9] == 'v' && data[10] == 'i' && data[11] == 'f')
        return ImageFormat::AVIF;
    // BMP
    if (data[0] == 'B' && data[1] == 'M')
        return ImageFormat::BMP;
    // TIFF
    if ((data[0] == 'I' && data[1] == 'I' && data[2] == 42 && data[3] == 0) ||
        (data[0] == 'M' && data[1] == 'M' && data[2] == 0 && data[3] == 42))
        return ImageFormat::TIFF;
    // ICO
    if (data[0] == 0 && data[1] == 0 && data[2] == 1 && data[3] == 0)
        return ImageFormat::ICO;
    // SVG (text-based)
    if (size >= 5 && data[0] == '<') {
        std::string header(reinterpret_cast<const char*>(data), std::min(size, (size_t)256));
        if (header.find("<svg") != std::string::npos) return ImageFormat::SVG;
    }

    return ImageFormat::Unknown;
}

std::string ImageFormatDetector::mimeType(ImageFormat format) {
    switch (format) {
        case ImageFormat::PNG: return "image/png";
        case ImageFormat::JPEG: return "image/jpeg";
        case ImageFormat::WebP: return "image/webp";
        case ImageFormat::AVIF: return "image/avif";
        case ImageFormat::GIF: return "image/gif";
        case ImageFormat::SVG: return "image/svg+xml";
        case ImageFormat::ICO: return "image/x-icon";
        case ImageFormat::BMP: return "image/bmp";
        case ImageFormat::TIFF: return "image/tiff";
        default: return "application/octet-stream";
    }
}

ImageFormat ImageFormatDetector::fromMimeType(const std::string& mime) {
    if (mime == "image/png") return ImageFormat::PNG;
    if (mime == "image/jpeg" || mime == "image/jpg") return ImageFormat::JPEG;
    if (mime == "image/webp") return ImageFormat::WebP;
    if (mime == "image/avif") return ImageFormat::AVIF;
    if (mime == "image/gif") return ImageFormat::GIF;
    if (mime == "image/svg+xml") return ImageFormat::SVG;
    if (mime == "image/x-icon" || mime == "image/vnd.microsoft.icon") return ImageFormat::ICO;
    if (mime == "image/bmp") return ImageFormat::BMP;
    if (mime == "image/tiff") return ImageFormat::TIFF;
    return ImageFormat::Unknown;
}

ImageFormat ImageFormatDetector::fromExtension(const std::string& ext) {
    std::string lower = ext;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower == ".png") return ImageFormat::PNG;
    if (lower == ".jpg" || lower == ".jpeg") return ImageFormat::JPEG;
    if (lower == ".webp") return ImageFormat::WebP;
    if (lower == ".avif") return ImageFormat::AVIF;
    if (lower == ".gif") return ImageFormat::GIF;
    if (lower == ".svg") return ImageFormat::SVG;
    if (lower == ".ico") return ImageFormat::ICO;
    if (lower == ".bmp") return ImageFormat::BMP;
    if (lower == ".tiff" || lower == ".tif") return ImageFormat::TIFF;
    return ImageFormat::Unknown;
}

// ==================================================================
// ImageTransform
// ==================================================================

DecodedImage ImageTransform::resize(const DecodedImage& src, int dstWidth, int dstHeight,
                                      Filter filter) {
    DecodedImage result;
    result.width = dstWidth;
    result.height = dstHeight;
    result.stride = dstWidth * 4;
    result.sourceFormat = src.sourceFormat;
    result.pixels.resize(static_cast<size_t>(result.stride) * dstHeight);

    if (!src.valid()) return result;

    float scaleX = static_cast<float>(src.width) / dstWidth;
    float scaleY = static_cast<float>(src.height) / dstHeight;

    for (int y = 0; y < dstHeight; y++) {
        for (int x = 0; x < dstWidth; x++) {
            float srcX = (x + 0.5f) * scaleX - 0.5f;
            float srcY = (y + 0.5f) * scaleY - 0.5f;

            int dstIdx = (y * result.stride) + x * 4;

            if (filter == Filter::Nearest) {
                int sx = std::clamp(static_cast<int>(srcX + 0.5f), 0, src.width - 1);
                int sy = std::clamp(static_cast<int>(srcY + 0.5f), 0, src.height - 1);
                int srcIdx = sy * src.stride + sx * 4;
                std::memcpy(&result.pixels[dstIdx], &src.pixels[srcIdx], 4);
            } else {
                // Bilinear
                int x0 = std::clamp(static_cast<int>(srcX), 0, src.width - 1);
                int y0 = std::clamp(static_cast<int>(srcY), 0, src.height - 1);
                int x1 = std::min(x0 + 1, src.width - 1);
                int y1 = std::min(y0 + 1, src.height - 1);
                float fx = srcX - x0;
                float fy = srcY - y0;

                for (int c = 0; c < 4; c++) {
                    float v00 = src.pixels[y0 * src.stride + x0 * 4 + c];
                    float v10 = src.pixels[y0 * src.stride + x1 * 4 + c];
                    float v01 = src.pixels[y1 * src.stride + x0 * 4 + c];
                    float v11 = src.pixels[y1 * src.stride + x1 * 4 + c];
                    float v = v00 * (1 - fx) * (1 - fy) + v10 * fx * (1 - fy) +
                              v01 * (1 - fx) * fy + v11 * fx * fy;
                    result.pixels[dstIdx + c] = static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f));
                }
            }
        }
    }

    return result;
}

DecodedImage ImageTransform::crop(const DecodedImage& src, int x, int y, int w, int h) {
    DecodedImage result;
    result.width = w;
    result.height = h;
    result.stride = w * 4;
    result.sourceFormat = src.sourceFormat;
    result.pixels.resize(static_cast<size_t>(result.stride) * h);

    for (int row = 0; row < h; row++) {
        int srcRow = y + row;
        if (srcRow < 0 || srcRow >= src.height) continue;
        int srcOffset = srcRow * src.stride + x * 4;
        int dstOffset = row * result.stride;
        int copyWidth = std::min(w, src.width - x) * 4;
        if (copyWidth > 0 && srcOffset >= 0) {
            std::memcpy(&result.pixels[dstOffset], &src.pixels[srcOffset], copyWidth);
        }
    }
    return result;
}

DecodedImage ImageTransform::applyOrientation(const DecodedImage& src, int orientation) {
    if (orientation <= 1 || orientation > 8) return src;

    DecodedImage result;
    bool swap = (orientation >= 5); // 5-8 swap width/height
    result.width = swap ? src.height : src.width;
    result.height = swap ? src.width : src.height;
    result.stride = result.width * 4;
    result.sourceFormat = src.sourceFormat;
    result.pixels.resize(static_cast<size_t>(result.stride) * result.height);

    for (int y = 0; y < src.height; y++) {
        for (int x = 0; x < src.width; x++) {
            int dx = x, dy = y;
            switch (orientation) {
                case 2: dx = src.width - 1 - x; break;
                case 3: dx = src.width - 1 - x; dy = src.height - 1 - y; break;
                case 4: dy = src.height - 1 - y; break;
                case 5: dx = y; dy = x; break;
                case 6: dx = src.height - 1 - y; dy = x; break;
                case 7: dx = src.height - 1 - y; dy = src.width - 1 - x; break;
                case 8: dx = y; dy = src.width - 1 - x; break;
            }
            int srcIdx = y * src.stride + x * 4;
            int dstIdx = dy * result.stride + dx * 4;
            std::memcpy(&result.pixels[dstIdx], &src.pixels[srcIdx], 4);
        }
    }
    return result;
}

void ImageTransform::premultiplyAlpha(DecodedImage& img) {
    for (size_t i = 0; i < img.pixels.size(); i += 4) {
        float a = img.pixels[i + 3] / 255.0f;
        img.pixels[i + 0] = static_cast<uint8_t>(img.pixels[i + 0] * a);
        img.pixels[i + 1] = static_cast<uint8_t>(img.pixels[i + 1] * a);
        img.pixels[i + 2] = static_cast<uint8_t>(img.pixels[i + 2] * a);
    }
}

void ImageTransform::unpremultiplyAlpha(DecodedImage& img) {
    for (size_t i = 0; i < img.pixels.size(); i += 4) {
        float a = img.pixels[i + 3] / 255.0f;
        if (a > 0.001f) {
            img.pixels[i + 0] = static_cast<uint8_t>(std::min(255.0f, img.pixels[i + 0] / a));
            img.pixels[i + 1] = static_cast<uint8_t>(std::min(255.0f, img.pixels[i + 1] / a));
            img.pixels[i + 2] = static_cast<uint8_t>(std::min(255.0f, img.pixels[i + 2] / a));
        }
    }
}

} // namespace NXRender
