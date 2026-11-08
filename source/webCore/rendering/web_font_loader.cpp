/**
 * @file web_font_loader.cpp
 * @brief @font-face loading: source selection, download rounds, decoding
 *        and registration.
 */

#include "rendering/web_font_loader.hpp"

#include "browser/data_url.hpp"
#include "rendering/font_manager.hpp"
#include "rendering/web_font_decoder.hpp"

#include <unordered_map>

namespace Zepra::Fonts {

using WebCore::FontFaceDescriptors;
using WebCore::FontFaceSource;
using WebCore::FontFaceStyle;

void TextCoverage::addUtf8(const std::string& text) {
    const auto* s = reinterpret_cast<const unsigned char*>(text.data());
    size_t n = text.size();
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i];
        uint32_t cp;
        size_t len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { ++i; continue; }
        if (i + len > n) break;
        bool ok = true;
        for (size_t k = 1; k < len; ++k) {
            if ((s[i + k] & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if (!ok) { ++i; continue; }
        i += len;
        // Whitespace and controls never pick a font
        if (cp > 0x20 && cp != 0x7F && cp <= 0x10FFFF) codePoints_.insert(cp);
    }
}

bool TextCoverage::wants(const FontFaceDescriptors& face) const {
    if (face.unicodeRanges.empty() || codePoints_.empty()) return true;
    for (uint32_t cp : codePoints_)
        if (face.coversCodePoint(cp)) return true;
    return false;
}

bool isSupportedFontTech(const std::vector<std::string>& techs) {
    for (const auto& t : techs)
        if (t != "variations" && t != "features-opentype" && t != "features-aat") return false;
    return true;
}

namespace {

struct Pending {
    const WebCore::CSSEngine::WebFontFace* face;
    std::vector<const FontFaceSource*> sources;  // the usable ones, in order
    size_t next = 0;
    std::vector<std::string> errors;
};

WebFontFaceInfo infoFor(const FontFaceDescriptors& d) {
    WebFontFaceInfo info;
    info.family = d.family;
    info.weightMin = d.weightMin;
    info.weightMax = d.weightMax;
    info.italic = d.style != FontFaceStyle::Normal;
    return info;
}

bool registerBytes(const FontFaceDescriptors& d, const uint8_t* data, size_t size, std::string& error) {
    auto decoded = decodeWebFont(data, size);
    if (!decoded.ok) {
        error = decoded.error;
        return false;
    }
    if (!FontManager::instance().registerWebFont(infoFor(d), std::move(decoded.sfnt))) {
        error = "FreeType could not load the font";
        return false;
    }
    return true;
}

} // namespace

WebFontLoadResult loadWebFonts(const std::vector<WebCore::CSSEngine::WebFontFace>& faces,
                               const TextCoverage* text,
                               const WebFontUrlResolver& resolve,
                               const WebFontBatchFetcher& fetch) {
    WebFontLoadResult result;
    std::vector<Pending> pending;
    for (const auto& face : faces) {
        if (text && !text->wants(face.descriptors)) {
            result.skipped++;
            continue;
        }
        Pending p{&face, {}, 0, {}};
        for (const auto& src : face.descriptors.sources) {
            if (src.isLocal) {
                p.sources.push_back(&src);
                continue;
            }
            bool formatOk = src.formats.empty();
            for (const auto& f : src.formats)
                if (isSupportedFontFormatHint(f)) formatOk = true;
            if (formatOk && isSupportedFontTech(src.techs)) p.sources.push_back(&src);
        }
        if (p.sources.empty()) {
            result.failed++;
            result.messages.push_back("@font-face '" + face.descriptors.family +
                                      "': no src in a supported format");
            continue;
        }
        pending.push_back(std::move(p));
    }

    // Responses by URL: two faces naming the same file download it once
    std::unordered_map<std::string, WebFontResponse> fetched;

    while (!pending.empty()) {
        // Settle what needs no download (local() and data: URLs); collect the rest
        std::vector<std::string> round;
        std::unordered_map<std::string, bool> inRound;
        std::vector<Pending> waiting;
        for (auto& p : pending) {
            const FontFaceDescriptors& d = p.face->descriptors;
            bool done = false;
            while (!done && p.next < p.sources.size()) {
                const FontFaceSource& src = *p.sources[p.next];
                if (src.isLocal) {
                    p.next++;
                    bool italic = d.style != FontFaceStyle::Normal;
                    const RegisteredFont* installed =
                        FontManager::instance().findInstalled(src.localName, d.weightMin, italic);
                    if (installed && !installed->path.empty() &&
                        FontManager::instance().registerWebFont(infoFor(d), {}, installed->path)) {
                        done = true;
                    } else {
                        p.errors.push_back("local(" + src.localName + ") is not installed");
                    }
                    continue;
                }
                if (src.url.compare(0, 5, "data:") == 0) {
                    p.next++;
                    std::vector<uint8_t> bytes;
                    std::string type, error;
                    if (!WebCore::decodeDataUrl(src.url, bytes, type)) {
                        p.errors.push_back("bad data: URL");
                    } else if (registerBytes(d, bytes.data(), bytes.size(), error)) {
                        done = true;
                    } else {
                        p.errors.push_back("data: URL: " + error);
                    }
                    continue;
                }
                std::string url = resolve ? resolve(p.face->baseUrl, src.url) : src.url;
                if (!fetched.count(url) && !inRound.count(url)) {
                    inRound[url] = true;
                    round.push_back(url);
                }
                break;  // wait for this download
            }
            if (done) {
                result.loaded++;
            } else if (p.next >= p.sources.size()) {
                result.failed++;
                std::string why;
                for (const auto& e : p.errors) why += (why.empty() ? "" : "; ") + e;
                result.messages.push_back("@font-face '" + d.family + "' failed: " + why);
            } else {
                waiting.push_back(std::move(p));
            }
        }
        pending.clear();
        if (waiting.empty()) break;

        if (!round.empty()) {
            std::vector<WebFontResponse> responses;
            if (fetch) responses = fetch(round);
            for (size_t i = 0; i < round.size(); ++i) {
                WebFontResponse r = i < responses.size() ? std::move(responses[i]) : WebFontResponse{};
                if (!r.ok && r.error.empty()) r.error = "download failed";
                fetched[round[i]] = std::move(r);
            }
        }

        // Every waiting face now has its download; try it, or move on
        for (auto& p : waiting) {
            const FontFaceSource& src = *p.sources[p.next++];
            std::string url = resolve ? resolve(p.face->baseUrl, src.url) : src.url;
            const WebFontResponse& r = fetched[url];
            std::string error = r.error;
            if (r.ok && registerBytes(p.face->descriptors, r.data.data(), r.data.size(), error)) {
                result.loaded++;
                continue;
            }
            p.errors.push_back(url + ": " + error);
            pending.push_back(std::move(p));  // next round tries its next source
        }
        // A face whose sources are now used up is reported by the next pass
    }
    return result;
}

} // namespace Zepra::Fonts
