/**
 * @file web_font_loader.hpp
 * @brief Loads a page's @font-face fonts: picks each face's first usable
 *        src entry, downloads it, decodes it (WOFF and WOFF2 included) and registers
 *        it with the FontManager so CSS font-family can use it.
 *
 * Downloads go through a batch callback so the browser can fetch every
 * font of a round in parallel. Faces whose unicode-range misses all the
 * page's text are not downloaded, as browsers do with Google Fonts' many
 * per-script subsets.
 */

#pragma once

#include "css/css_engine.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace Zepra::Fonts {

/// The code points a page's text uses, for unicode-range
class TextCoverage {
public:
    /// Add the characters of UTF-8 text (malformed bytes are skipped)
    void addUtf8(const std::string& text);
    void add(uint32_t codePoint) { codePoints_.insert(codePoint); }
    bool contains(uint32_t codePoint) const { return codePoints_.count(codePoint) != 0; }
    size_t size() const { return codePoints_.size(); }

    /// Whether the face covers any of the text. A face without unicode-range
    /// always does; so does any face when there is no text yet.
    bool wants(const WebCore::FontFaceDescriptors& face) const;

private:
    std::unordered_set<uint32_t> codePoints_;
};

struct WebFontResponse {
    bool ok = false;
    std::vector<uint8_t> data;
    std::string error;
};

/// Fetch each URL; the result has one response per URL, in order
using WebFontBatchFetcher = std::function<std::vector<WebFontResponse>(const std::vector<std::string>& urls)>;

/// Resolve a src URL against the base URL of the sheet it came from
using WebFontUrlResolver = std::function<std::string(const std::string& base, const std::string& href)>;

struct WebFontLoadResult {
    size_t loaded = 0;   ///< faces registered
    size_t failed = 0;   ///< faces none of whose sources worked
    size_t skipped = 0;  ///< faces whose unicode-range the text doesn't use
    std::vector<std::string> messages;  ///< one line per failure, for the console
};

/// Whether a src entry's tech() hints are all ones FreeType handles
bool isSupportedFontTech(const std::vector<std::string>& techs);

/// Load the faces and register them with FontManager::instance(). It does
/// not clear earlier web fonts; call FontManager::clearWebFonts() first on
/// a new page. `text` may be null to load every face.
WebFontLoadResult loadWebFonts(const std::vector<WebCore::CSSEngine::WebFontFace>& faces,
                               const TextCoverage* text,
                               const WebFontUrlResolver& resolve,
                               const WebFontBatchFetcher& fetch);

} // namespace Zepra::Fonts
