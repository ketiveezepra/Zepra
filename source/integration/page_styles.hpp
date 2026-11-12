// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file page_styles.hpp
 * @brief The document's style sheets, kept in line with its <style> and
 *        <link rel=stylesheet> elements as they come and go
 *
 * HTML: a <style> element's sheet exists while it is connected and follows
 * its text; a <link rel=stylesheet> starts a fetch when it is connected (or
 * its href changes), its sheet applies once it arrived, and it gets `load`
 * (or `error`) then. Sheets apply in tree order.
 *
 * sync() walks the document before a style pass: it starts fetches for new
 * links (and the @import rules of new sheets, so the engine's import
 * fetcher never waits on the network), and, when the list of usable sheets
 * changed, hands the CSS engine its author sheets in tree order. A sheet's
 * text is parsed once and reused while it stays in the document.
 *
 * Network answers come back through resourceArrived() on the UI thread; the
 * owner relays them from its fetch threads. Nothing here blocks.
 */

#pragma once

#include "css/css_engine.hpp"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Zepra::WebCore {
class DOMDocument;
class DOMElement;
}

namespace ZepraBrowser {

class PageStyleSheets {
public:
    /// Starts fetching a style sheet; the answer comes to resourceArrived()
    using Fetch = std::function<void(const std::string& url)>;
    /// Resolves `href` against `base`
    using Resolve = std::function<std::string(const std::string& base, const std::string& href)>;

    PageStyleSheets(std::string documentUrl, Fetch fetch, Resolve resolve);

    /// Sheets added after the document's (the browser's raw-HTML fallbacks)
    void setExtraSheets(std::vector<std::string> sheets) { extraSheets_ = std::move(sheets); }

    /// A fetch finished (on the UI thread). `finalUrl` after redirects.
    void resourceArrived(const std::string& url, bool ok, std::string text, const std::string& finalUrl);

    /// Starts fetches for the document's sheets (and their imports) not
    /// requested yet. sync() does it too.
    void requestSheets(Zepra::WebCore::DOMDocument* document);

    /// Whether every sheet the document has now arrived (or failed), with
    /// its imports: the render-blocking ones, at load
    bool allSettled(Zepra::WebCore::DOMDocument* document);

    /// Brings `css`'s author sheets in line with the document. True when
    /// they changed (styles need computing again).
    bool sync(Zepra::WebCore::DOMDocument* document, Zepra::WebCore::CSSEngine* css);

    /// The engine's @import fetcher: an imported sheet that arrived
    Zepra::WebCore::CSSEngine::ImportFetcher importFetcher();

    /// <link> elements whose sheet arrived (true) or failed (false) since
    /// the last call, connected when sync() saw them; fire load/error at them
    std::vector<std::pair<Zepra::WebCore::DOMElement*, bool>> takeLinkEvents();

    /// Fetches started and not answered yet
    size_t pendingFetches() const { return pending_; }

private:
    struct Resource {
        bool arrived = false;
        bool ok = false;
        std::string text;
        std::string base;                  // final URL: its @imports resolve against it
        std::vector<std::string> imports;  // resolved
    };
    /// One sheet the document has: a <style> (its text) or a <link> (its URL)
    struct Source {
        Zepra::WebCore::DOMElement* element = nullptr;
        bool link = false;
        std::string text;   // <style>
        std::string url;    // <link>
        std::string media;  // its media attribute (empty: all)
    };
    /// Whether a source's media attribute matches now (a sheet whose media
    /// does not match is still fetched, but does not block rendering)
    static bool mediaMatches(const Source& source);

    std::vector<Source> collect(Zepra::WebCore::DOMDocument* document) const;
    void request(const std::string& url);
    void requestImports(const std::string& css, const std::string& base);
    /// Arrived (or failed) with all its imports
    bool settled(const std::string& url, int depth = 0) const;
    bool sourceSettled(const Source& source) const;
    /// The engine's entries for one sheet text (with its imported sheets), parsed once
    const std::vector<Zepra::WebCore::StyleResolver::StyleSheetEntry>&
        parsed(Zepra::WebCore::CSSEngine* css, const std::string& key, const std::string& text, const std::string& href,
               const std::string& media = "");

    std::string documentUrl_;
    Fetch fetch_;
    Resolve resolve_;
    std::vector<std::string> extraSheets_;
    std::unordered_map<std::string, Resource> resources_;
    std::unordered_map<std::string, std::vector<std::string>> styleImports_;  // <style> text -> imports
    std::unordered_map<std::string, std::vector<Zepra::WebCore::StyleResolver::StyleSheetEntry>> parsed_;
    std::vector<std::string> applied_;   // keys of the sheets the engine has, in order
    Zepra::WebCore::CSSEngine* appliedTo_ = nullptr;
    size_t pending_ = 0;
    std::set<std::pair<Zepra::WebCore::DOMElement*, std::string>> notified_;
    std::vector<std::pair<Zepra::WebCore::DOMElement*, bool>> linkEvents_;
};

/// The URLs of the @import rules at the start of a sheet, as written
std::vector<std::string> importedUrls(const std::string& css);

} // namespace ZepraBrowser
