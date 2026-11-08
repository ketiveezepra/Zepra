/**
 * @file font_face_descriptors.hpp
 * @brief The descriptors of an @font-face rule, parsed into values the font
 *        loader can use (CSS Fonts 4, section 4).
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Zepra::WebCore {

class CSSStyleDeclaration;

/// One entry of the src descriptor: url(...) with its hints, or local(...)
struct FontFaceSource {
    bool isLocal = false;
    std::string url;                   ///< as written; resolve against the sheet's URL
    std::string localName;             ///< for local(): a full font name
    std::vector<std::string> formats;  ///< format() hints, lowercased, may be empty
    std::vector<std::string> techs;    ///< tech() hints, lowercased, may be empty
};

struct UnicodeRange {
    uint32_t first = 0;
    uint32_t last = 0x10FFFF;
};

enum class FontFaceStyle { Normal, Italic, Oblique };

struct FontFaceDescriptors {
    std::string family;  ///< the name pages use in font-family, case kept
    std::vector<FontFaceSource> sources;
    int weightMin = 400, weightMax = 400;
    FontFaceStyle style = FontFaceStyle::Normal;
    float stretchMin = 100.f, stretchMax = 100.f;  ///< percent
    std::vector<UnicodeRange> unicodeRanges;       ///< empty means every code point
    std::string display = "auto";

    bool coversCodePoint(uint32_t cp) const;
    bool coversWeight(int weight) const { return weight >= weightMin && weight <= weightMax; }
};

/// Parse a rule's declarations. False when the rule is unusable (no valid
/// font-family or no valid src entry). An invalid optional descriptor keeps
/// its initial value, as if it were absent.
bool parseFontFaceDescriptors(const CSSStyleDeclaration& decl, FontFaceDescriptors& out);

// The pieces, exposed for tests
bool parseFontFaceFamily(const std::string& value, std::string& family);
bool parseFontFaceSrc(const std::string& value, std::vector<FontFaceSource>& sources);
bool parseFontFaceWeight(const std::string& value, int& min, int& max);
bool parseFontFaceStyle(const std::string& value, FontFaceStyle& style);
bool parseFontFaceStretch(const std::string& value, float& min, float& max);
bool parseUnicodeRange(const std::string& value, std::vector<UnicodeRange>& ranges);

} // namespace Zepra::WebCore
