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

std::string expandCSSVariables(const std::string& css) {
    // Parse :root { --var: value; } declarations
    std::unordered_map<std::string, std::string> variables;
    
    // Find :root block
    size_t rootStart = css.find(":root");
    if (rootStart != std::string::npos) {
        size_t braceStart = css.find('{', rootStart);
        size_t braceEnd = css.find('}', braceStart);
        if (braceStart != std::string::npos && braceEnd != std::string::npos) {
            std::string rootBlock = css.substr(braceStart + 1, braceEnd - braceStart - 1);
            
            // Parse --var: value; pairs
            size_t pos = 0;
            while ((pos = rootBlock.find("--", pos)) != std::string::npos) {
                size_t colonPos = rootBlock.find(':', pos);
                if (colonPos == std::string::npos) break;
                
                std::string varName = rootBlock.substr(pos, colonPos - pos);
                // Trim varName
                while (!varName.empty() && std::isspace(varName.back())) varName.pop_back();
                
                size_t valueStart = colonPos + 1;
                size_t semicolonPos = rootBlock.find(';', valueStart);
                if (semicolonPos == std::string::npos) semicolonPos = rootBlock.length();
                
                std::string value = rootBlock.substr(valueStart, semicolonPos - valueStart);
                // Trim value
                while (!value.empty() && std::isspace(value.front())) value = value.substr(1);
                while (!value.empty() && std::isspace(value.back())) value.pop_back();
                
                // Store raw value
                variables[varName] = value;
                
                pos = semicolonPos + 1;
            }
        }
    }
    
    // Iteratively resolve variables (handles --a: var(--b))
    bool changed = true;
    int iterations = 0;
    while (changed && iterations < 10) { // Limit iterations to prevent infinite loops
        changed = false;
        for (auto& [name, val] : variables) {
            size_t varPos = 0;
            std::string newVal = val;
            while ((varPos = newVal.find("var(--", varPos)) != std::string::npos) {
                size_t endPos = newVal.find(')', varPos);
                if (endPos == std::string::npos) break;
                
                std::string refName = newVal.substr(varPos + 4, endPos - (varPos + 4));
                if (variables.count(refName)) {
                    newVal.replace(varPos, endPos - varPos + 1, variables[refName]);
                    changed = true;
                } else {
                    varPos++; // Skip if not found to avoid infinite loop on missing var
                }
            }
            if (newVal != val) {
                val = newVal;
                changed = true;
            }
        }
        iterations++;
    }
    
    if (!variables.empty()) {
        std::cout << "[CSS] Resolved " << variables.size() << " CSS variables after " << iterations << " iterations" << std::endl;
    }
    
    // Replace all var(--name) with resolved values in the full CSS
    std::string result = css;
    for (const auto& [varName, value] : variables) {
        std::string varRef = "var(" + varName + ")";
        size_t pos = 0;
        while ((pos = result.find(varRef, pos)) != std::string::npos) {
            result.replace(pos, varRef.length(), value);
            pos += value.length();
        }
    }
    
    return result;
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
            // EXPAND CSS VARIABLES
            css = expandCSSVariables(css);
            // POLYFILL SHORTHANDS
            css = polyfillCSSShorthands(css);
            
            styles.push_back(css);
            std::cout << "[CSS] Extracted, expanded & polyfilled inline style: " << css.size() << " bytes" << std::endl;
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
