// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file media_query.hpp
 * @brief Media queries (Media Queries Level 4/5): parsing a media query list
 *        such as "screen and (min-width: 600px), print" and matching it
 *        against the page's viewport.
 *
 * A style rule inside `@media` keeps the query lists around it; the cascade
 * matches the rule only while every one of them matches the current
 * MediaEnvironment. When the viewport changes so that some live query list
 * changes its result, setMediaEnvironment() reports it (and bumps
 * domMutationVersion(), so the browser restyles the page).
 *
 * @see https://drafts.csswg.org/mediaqueries-5/
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Zepra::WebCore {

/// What media queries are matched against (CSS pixels)
struct MediaEnvironment {
    double width = 1280;    ///< viewport width
    double height = 800;    ///< viewport height
    double devicePixelRatio = 1;
    bool prefersDark = false;
    bool prefersReducedMotion = false;

    bool operator==(const MediaEnvironment& o) const {
        return width == o.width && height == o.height && devicePixelRatio == o.devicePixelRatio &&
               prefersDark == o.prefersDark && prefersReducedMotion == o.prefersReducedMotion;
    }
    bool operator!=(const MediaEnvironment& o) const { return !(*this == o); }
};

/// The environment every query is matched against
const MediaEnvironment& mediaEnvironment();

/// Changes the environment. Returns true when some live MediaQueryList now
/// matches differently; then it also calls noteDomMutation() so styles are
/// recomputed. Unchanged values do nothing.
bool setMediaEnvironment(const MediaEnvironment& env);

/// Bumped each time setMediaEnvironment() changes some list's result
uint64_t mediaMatchVersion();

/**
 * @brief A parsed media query list. An empty list matches everything; a
 *        query that cannot be parsed matches nothing ("not all"), as the
 *        spec asks, without affecting the other queries of the list.
 */
class MediaQueryList {
public:
    struct Node;  // a parsed query or condition (media_query.cpp)

    static std::shared_ptr<MediaQueryList> parse(const std::string& text);
    ~MediaQueryList();

    bool matches(const MediaEnvironment& env) const;
    bool matches() const { return matches(mediaEnvironment()); }

    /// The list serialized as CSSOM does for MediaList.mediaText (normalized
    /// whitespace and lowercase, the queries joined by ", ")
    const std::string& mediaText() const { return text_; }

private:
    MediaQueryList() = default;
    std::string text_;
    std::vector<std::shared_ptr<const Node>> queries_;  // null = "not all"
    mutable int lastResult_ = -1;                        // for change detection
    friend bool setMediaEnvironment(const MediaEnvironment&);
};

/// True when every list matches (an empty vector matches)
bool allMediaMatch(const std::vector<std::shared_ptr<const MediaQueryList>>& lists);

}  // namespace Zepra::WebCore
