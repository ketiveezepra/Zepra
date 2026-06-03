/**
 * @file clipboard.hpp
 * @brief Clipboard access
 */

#pragma once

#include <cstdint>
#include <algorithm>

#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace Zepra::Platform {

struct ClipboardFormat {
    static constexpr const char* TEXT = "text/plain";
    static constexpr const char* HTML = "text/html";
    static constexpr const char* IMAGE_PNG = "image/png";
    static constexpr const char* IMAGE_JPEG = "image/jpeg";
    static constexpr const char* FILES = "x-special/files";
};

/**
 * @class Clipboard
 * @brief System clipboard access
 */
class Clipboard {
public:
    static Clipboard& instance();
    
    // Text
    bool setText(const std::string& text);
    std::optional<std::string> getText();
    
    // HTML
    bool setHtml(const std::string& html);
    std::optional<std::string> getHtml();
    
    // Image (as PNG bytes)
    bool setImage(const std::vector<uint8_t>& pngData);
    std::optional<std::vector<uint8_t>> getImage();
    
    // Files
    bool setFiles(const std::vector<std::string>& paths);
    std::optional<std::vector<std::string>> getFiles();
    
    // Query formats
    bool hasFormat(const char* format);
    std::vector<std::string> availableFormats();
    
    // Clear
    void clear();
    
    // Watch for changes
    using ChangeCallback = std::function<void()>;
    void setOnChange(ChangeCallback callback) { onChange_ = std::move(callback); }
    
private:
    Clipboard() = default;
    ChangeCallback onChange_;
    // In-memory fallback (Linux/NeolyxOS — no X11 window handle available here)
    std::string memoryClipboard_;
    std::string memoryHtml_;
};

} // namespace Zepra::Platform
