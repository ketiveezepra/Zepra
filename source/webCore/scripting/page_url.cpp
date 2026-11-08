// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

#include "scripting/page_url.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace Zepra::WebCore {

namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Leading and trailing C0 controls and spaces go, tabs and newlines anywhere
std::string clean(const std::string& text) {
    size_t first = 0, last = text.size();
    while (first < last && static_cast<unsigned char>(text[first]) <= ' ') first++;
    while (last > first && static_cast<unsigned char>(text[last - 1]) <= ' ') last--;
    std::string out;
    out.reserve(last - first);
    for (size_t i = first; i < last; i++) {
        if (text[i] != '\t' && text[i] != '\n' && text[i] != '\r') out += text[i];
    }
    return out;
}

const char* defaultPort(const std::string& protocol) {
    if (protocol == "http:" || protocol == "ws:") return "80";
    if (protocol == "https:" || protocol == "wss:") return "443";
    if (protocol == "ftp:") return "21";
    return nullptr;
}

bool isSpecial(const std::string& protocol) {
    return defaultPort(protocol) || protocol == "file:";
}

// The scheme's length when `text` starts with one followed by ':', else 0. A
// single letter is a Windows drive ("C:/x"), not a scheme.
size_t schemeLength(const std::string& text) {
    if (text.empty() || !std::isalpha(static_cast<unsigned char>(text[0]))) return 0;
    size_t i = 1;
    while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) ||
                               text[i] == '+' || text[i] == '-' || text[i] == '.')) {
        i++;
    }
    return (i < text.size() && text[i] == ':' && i >= 2) ? i : 0;
}

// RFC 3986 5.2.4: "." and ".." segments removed
std::string removeDotSegments(const std::string& path) {
    std::vector<std::string> out;
    size_t start = path.empty() || path[0] != '/' ? 0 : 1;
    bool trailingSlash = false;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        std::string segment = path.substr(start, end - start);
        bool last = end == path.size();
        std::string folded = lower(segment);
        if (segment == "." || folded == "%2e") {
            trailingSlash = last;
        } else if (segment == ".." || folded == ".%2e" || folded == "%2e." || folded == "%2e%2e") {
            if (!out.empty()) out.pop_back();
            trailingSlash = last;
        } else {
            out.push_back(segment);
            trailingSlash = false;
        }
        start = end + 1;
    }
    std::string result = "/";
    for (size_t i = 0; i < out.size(); i++) {
        if (i) result += '/';
        result += out[i];
    }
    if (trailingSlash && result.back() != '/') result += '/';
    return result;
}

// Splits "path?query#fragment" into the URL's path, query and fragment
void splitTail(const std::string& tail, std::string& path, PageUrl& url) {
    size_t hashPos = tail.find('#');
    std::string beforeHash = tail.substr(0, hashPos);
    url.hasFragment = hashPos != std::string::npos;
    url.fragment = url.hasFragment ? tail.substr(hashPos + 1) : "";
    size_t queryPos = beforeHash.find('?');
    url.hasQuery = queryPos != std::string::npos;
    url.query = url.hasQuery ? beforeHash.substr(queryPos + 1) : "";
    path = beforeHash.substr(0, queryPos);
}

} // namespace

std::string PageUrl::origin() const {
    if (!valid || !defaultPort(protocol)) return "null";
    return protocol + "//" + host();
}

std::string PageUrl::withoutFragment() const {
    if (!valid) return "";
    std::string out = protocol;
    if (hierarchical) {
        out += "//";
        if (!username.empty() || !password.empty()) {
            out += username;
            if (!password.empty()) out += ":" + password;
            out += "@";
        }
        out += host();
    }
    out += pathname;
    if (hasQuery) out += "?" + query;
    return out;
}

std::string PageUrl::href() const {
    if (!valid) return "";
    return withoutFragment() + (hasFragment ? "#" + fragment : "");
}

PageUrl PageUrl::parse(const std::string& input) {
    PageUrl url;
    std::string text = clean(input);
    size_t schemeLen = schemeLength(text);
    if (!schemeLen) return url;
    url.protocol = lower(text.substr(0, schemeLen)) + ":";
    std::string rest = text.substr(schemeLen + 1);
    const bool special = isSpecial(url.protocol);
    if (special) std::replace(rest.begin(), rest.end(), '\\', '/');

    if (rest.compare(0, 2, "//") == 0 || (special && url.protocol != "file:")) {
        // An authority: special schemes have one even when written "http:x"
        size_t start = 0;
        while (start < rest.size() && rest[start] == '/') start++;
        if (url.protocol == "file:" || !special) start = std::min<size_t>(2, rest.size());
        size_t end = rest.find_first_of("/?#", start);
        if (end == std::string::npos) end = rest.size();
        std::string authority = rest.substr(start, end - start);
        size_t at = authority.rfind('@');
        if (at != std::string::npos) {
            std::string userinfo = authority.substr(0, at);
            size_t colon = userinfo.find(':');
            url.username = userinfo.substr(0, colon);
            if (colon != std::string::npos) url.password = userinfo.substr(colon + 1);
            authority = authority.substr(at + 1);
        }
        size_t colon = authority.rfind(':');
        size_t bracket = authority.rfind(']');
        if (colon != std::string::npos && (bracket == std::string::npos || colon > bracket)) {
            url.port = authority.substr(colon + 1);
            authority = authority.substr(0, colon);
            for (char c : url.port) {
                if (!std::isdigit(static_cast<unsigned char>(c))) return PageUrl{};
            }
            // "080" is port 80
            size_t nonZero = url.port.find_first_not_of('0');
            if (!url.port.empty()) url.port = nonZero == std::string::npos ? "0" : url.port.substr(nonZero);
            const char* fallback = defaultPort(url.protocol);
            if (fallback && url.port == fallback) url.port.clear();
        }
        url.hostname = special ? lower(authority) : authority;
        if (url.hostname.empty() && special && url.protocol != "file:") return PageUrl{};
        url.hierarchical = true;
        rest = rest.substr(end);
    }

    std::string path;
    splitTail(rest, path, url);
    if (url.hierarchical || special) {
        url.hierarchical = true;
        url.pathname = path.empty() ? (special ? "/" : "") : removeDotSegments(path);
    } else {
        url.pathname = path;   // about:blank, mailto:a@b, data:...
    }
    url.valid = true;
    return url;
}

PageUrl PageUrl::resolve(const std::string& baseText, const std::string& input) {
    std::string reference = clean(input);
    PageUrl base = parse(baseText);
    if (size_t schemeLen = schemeLength(reference)) {
        // "http:page.html" against an http base is relative to it
        const std::string rest = reference.substr(schemeLen + 1);
        if (!(base.valid && isSpecial(base.protocol) && lower(reference.substr(0, schemeLen)) + ":" == base.protocol &&
              (rest.empty() || (rest[0] != '/' && rest[0] != '\\')))) {
            return parse(reference);
        }
        reference = rest;
    }
    if (!base.valid) return PageUrl{};

    if (!base.hierarchical) {
        // An opaque URL only takes a new fragment
        if (reference.empty() || reference[0] != '#') return PageUrl{};
        base.hasFragment = true;
        base.fragment = reference.substr(1);
        return base;
    }
    if (isSpecial(base.protocol)) std::replace(reference.begin(), reference.end(), '\\', '/');
    if (reference.compare(0, 2, "//") == 0) return parse(base.protocol + reference);

    PageUrl url = base;
    url.hasFragment = false;
    url.fragment.clear();
    if (reference.empty()) return url;
    if (reference[0] == '#') {
        url.hasFragment = true;
        url.fragment = reference.substr(1);
        return url;
    }
    std::string path;
    PageUrl tail;
    splitTail(reference, path, tail);
    url.hasFragment = tail.hasFragment;
    url.fragment = tail.fragment;
    if (path.empty()) {
        // "?q": same path, new query
        url.hasQuery = tail.hasQuery;
        url.query = tail.query;
        return url;
    }
    url.hasQuery = tail.hasQuery;
    url.query = tail.query;
    if (path[0] == '/') {
        url.pathname = removeDotSegments(path);
    } else {
        size_t lastSlash = base.pathname.rfind('/');
        std::string directory = lastSlash == std::string::npos ? "/" : base.pathname.substr(0, lastSlash + 1);
        url.pathname = removeDotSegments(directory + path);
    }
    return url;
}

} // namespace Zepra::WebCore
