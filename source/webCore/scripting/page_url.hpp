// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.

/**
 * @file page_url.hpp
 * @brief The page URL as `window.location` reads and changes it: split into
 *        the WHATWG URL components, and relative references resolved against
 *        it (RFC 3986 section 5.2, with the URL Standard's special schemes).
 */

#pragma once

#include <string>

namespace Zepra::WebCore {

struct PageUrl {
    std::string protocol;   ///< scheme and ':', lower case ("https:")
    std::string username;
    std::string password;
    std::string hostname;   ///< lower case; IPv6 keeps its brackets
    std::string port;       ///< empty for the scheme's default port
    std::string pathname;   ///< "/" at least for http(s), ws(s), ftp and file
    std::string query;      ///< without '?'
    std::string fragment;   ///< without '#'
    bool hasQuery = false;     ///< a '?' was present (possibly with nothing after it)
    bool hasFragment = false;  ///< a '#' was present
    bool hierarchical = false; ///< has an authority ("//host"); about:, data:, mailto: do not
    bool valid = false;

    std::string host() const { return port.empty() ? hostname : hostname + ":" + port; }
    std::string search() const { return query.empty() ? "" : "?" + query; }   ///< location.search
    std::string hash() const { return fragment.empty() ? "" : "#" + fragment; } ///< location.hash
    /// scheme://host for http(s), ws(s) and ftp; "null" (an opaque origin) otherwise
    std::string origin() const;
    std::string href() const;
    /// The URL without its fragment
    std::string withoutFragment() const;

    /// Splits an absolute URL; `valid` is false when `text` has no scheme
    static PageUrl parse(const std::string& text);
    /// Resolves `reference` (absolute or relative) against `base`
    static PageUrl resolve(const std::string& base, const std::string& reference);
};

} // namespace Zepra::WebCore
