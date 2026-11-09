// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
#include "css_utils.h"
#include <iostream>
#include <unordered_map>
#include <algorithm>
#include <cctype>

namespace ZepraBrowser {

uint32_t cssColorToRGB(const Zepra::WebCore::CSSColor& c) {
    return (c.r << 16) | (c.g << 8) | c.b;
}

namespace {

uint32_t decodeUtf8(const std::string& s, size_t& i) {
    unsigned char lead = (unsigned char)s[i];
    int extra = lead < 0x80 ? 0 : (lead >> 5) == 0x6 ? 1 : (lead >> 4) == 0xE ? 2 : (lead >> 3) == 0x1E ? 3 : -1;
    if (extra == 0) { i++; return lead; }
    if (extra < 0 || i + extra >= s.size()) { i++; return 0xFFFD; }
    uint32_t c = lead & (0x3F >> extra);
    for (int k = 1; k <= extra; k++) c = (c << 6) | ((unsigned char)s[i + k] & 0x3F);
    i += extra + 1;
    return c;
}

void encodeUtf8(uint32_t c, std::string& out) {
    if (c < 0x80) out += (char)c;
    else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) {
        out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F));
    } else {
        out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F));
        out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F));
    }
}

uint32_t toUpper(uint32_t c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;       // Latin-1
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x17F && (c & 1) && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F &&
        !(c >= 0x139 && c <= 0x148) && !(c >= 0x179)) return c - 1;  // Latin Extended-A pairs
    if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) && !(c & 1)) return c - 1;
    if (c == 0x3C2) return 0x3A3;                                  // final sigma
    if (c >= 0x3B1 && c <= 0x3C9) return c - 32;                   // Greek
    if (c >= 0x430 && c <= 0x44F) return c - 32;                   // Cyrillic
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}

uint32_t toLower(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c == 0x178) return 0xFF;
    if (c >= 0x100 && c <= 0x17F && !(c & 1) && c != 0x130 && c != 0x138 && !(c >= 0x139 && c <= 0x148) &&
        !(c >= 0x179)) return c + 1;
    if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) && (c & 1)) return c + 1;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

bool isLetter(uint32_t c) { return toUpper(c) != c || toLower(c) != c || c == 0xDF; }

} // namespace

std::string applyTextTransform(const std::string& text, const std::string& transform) {
    std::string t = transform;
    for (char& ch : t) ch = (char)std::tolower((unsigned char)ch);
    bool upper = t == "uppercase", lowerCase = t == "lowercase", capitalize = t == "capitalize";
    if (!upper && !lowerCase && !capitalize) return text;
    std::string out;
    out.reserve(text.size());
    bool wordStart = true;
    for (size_t i = 0; i < text.size();) {
        uint32_t c = decodeUtf8(text, i);
        if (upper) {
            if (c == 0xDF) { out += "SS"; continue; }  // ß
            encodeUtf8(toUpper(c), out);
        } else if (lowerCase) {
            encodeUtf8(toLower(c), out);
        } else {
            encodeUtf8(wordStart && isLetter(c) ? toUpper(c) : c, out);
            if (isLetter(c) || (c >= '0' && c <= '9')) wordStart = false;
            else if (c == ' ' || c == '\t' || c == '\n' || c == 0xA0) wordStart = true;
        }
    }
    return out;
}

std::string polyfillCSSShorthands(const std::string& css) {
    std::string result = css;
    
    // background is left alone: the style resolver splits the shorthand
    // into colour, image, repeat, position and size (renaming it to
    // background-color turned "#fff center / cover" into black)

    // padding and margin shorthands are left alone: the style resolver
    // expands them, including the two- to four-value forms
    
    return result;
}

std::vector<std::string> extractStylesFromRawHTML(const std::string& html) {
    std::vector<std::string> styles;
    
    // Create lowercase version SAFELY (avoid UB on UTF-8 bytes)
    std::string lowerHtml = html;
    for (char& c : lowerHtml) {
        // Cast to unsigned char before tolower to avoid UB with negative char values (UTF-8)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    
    // Extract <style>...</style> content
    size_t pos = 0;
    while ((pos = lowerHtml.find("<style", pos)) != std::string::npos) {
        size_t tagEnd = lowerHtml.find('>', pos);
        if (tagEnd == std::string::npos) break;
        
        size_t contentStart = tagEnd + 1;
        size_t contentEnd = lowerHtml.find("</style>", contentStart);
        if (contentEnd == std::string::npos) break;
        
        std::string css = html.substr(contentStart, contentEnd - contentStart);
        if (!css.empty()) {
            // POLYFILL SHORTHANDS
            css = polyfillCSSShorthands(css);
            
            styles.push_back(css);
            std::cout << "[CSS] Extracted & polyfilled inline style: " << css.size() << " bytes" << std::endl;
        }
        pos = contentEnd + 8;
    }
    
    return styles;
}

// Helper for case-insensitive find
static size_t findCaseInsensitive(const std::string& str, const std::string& sub, size_t pos = 0) {
    if (sub.empty() || pos >= str.size()) return std::string::npos;
    
    auto it = std::search(
        str.begin() + pos, str.end(),
        sub.begin(), sub.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == 
                   std::tolower(static_cast<unsigned char>(b));
        }
    );
    
    if (it == str.end()) return std::string::npos;
    return it - str.begin();
}

std::string extractBodyInlineStyle(const std::string& html) {
    size_t bodyPos = findCaseInsensitive(html, "<body");
    if (bodyPos == std::string::npos) return "";
    
    size_t closePos = html.find(">", bodyPos);
    if (closePos == std::string::npos) return "";
    
    // Extract tag content
    std::string tagContent = html.substr(bodyPos, closePos - bodyPos);
    
    // Find style attribute (case insensitive key)
    size_t stylePos = findCaseInsensitive(tagContent, "style=");
    if (stylePos != std::string::npos) {
        size_t quoteStart = stylePos + 6;
        while (quoteStart < tagContent.length() && isspace(tagContent[quoteStart])) quoteStart++;
        
        if (quoteStart < tagContent.length()) {
            char quote = tagContent[quoteStart];
            if (quote == '"' || quote == '\'') {
                 size_t endQuote = tagContent.find(quote, quoteStart + 1);
                 if (endQuote != std::string::npos) {
                     return "body { " + tagContent.substr(quoteStart + 1, endQuote - (quoteStart + 1)) + " }";
                 }
            }
        }
    }
    
    // bgcolor and the other legacy attributes are presentational hints
    // (css/presentational_hints.cpp)
    return "";
}

std::string stripOuterTags(const std::string& html) {
    std::string clean = html;
    
    auto removeRegion = [&](const std::string& startTag, const std::string& endTag, bool removeContent) {
        size_t pos = 0;
        while ((pos = findCaseInsensitive(clean, startTag, pos)) != std::string::npos) {
            size_t tagEnd = clean.find(">", pos);
            if (tagEnd == std::string::npos) break;
            
            if (removeContent) {
                size_t closing = findCaseInsensitive(clean, endTag, tagEnd);
                if (closing != std::string::npos) {
                    size_t closingEnd = clean.find(">", closing);
                    if (closingEnd != std::string::npos) {
                        clean.erase(pos, closingEnd - pos + 1);
                        continue; 
                    }
                }
            } else {
                clean.erase(pos, tagEnd - pos + 1);
                size_t closing = findCaseInsensitive(clean, endTag);
                if (closing != std::string::npos) {
                     size_t closingEnd = clean.find(">", closing);
                     if (closingEnd != std::string::npos) {
                         clean.erase(closing, closingEnd - closing + 1);
                     }
                }
                continue;
            }
            pos++;
        }
    };
    
    size_t docPos = findCaseInsensitive(clean, "<!DOCTYPE");
    if (docPos != std::string::npos) {
        size_t docEnd = clean.find(">", docPos);
        if (docEnd != std::string::npos) {
            clean.erase(docPos, docEnd - docPos + 1);
        }
    }
    
    removeRegion("<head", "</head>", true);
    removeRegion("<html", "</html>", false);
    removeRegion("<body", "</body>", false);
    
    return clean;
}

} // namespace ZepraBrowser
