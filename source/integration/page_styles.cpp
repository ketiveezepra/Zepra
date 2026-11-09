// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file page_styles.cpp
 * @brief The document's <style>/<link> sheets, incrementally (see page_styles.hpp)
 */

#include "page_styles.hpp"

#include "browser/dom.hpp"

#include <algorithm>
#include <cctype>

namespace ZepraBrowser {

using namespace Zepra::WebCore;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// rel="stylesheet" (not an alternate one, which only applies when chosen)
bool isStylesheetLink(const DOMElement* el) {
    std::string rel = lower(el->getAttribute("rel"));
    bool sheet = false, alternate = false;
    size_t i = 0;
    while (i < rel.size()) {
        while (i < rel.size() && isSpace(rel[i])) i++;
        size_t start = i;
        while (i < rel.size() && !isSpace(rel[i])) i++;
        std::string token = rel.substr(start, i - start);
        if (token == "stylesheet") sheet = true;
        if (token == "alternate") alternate = true;
    }
    return sheet && !alternate;
}

constexpr int kMaxImportDepth = 16;

} // namespace

std::vector<std::string> importedUrls(const std::string& css) {
    // @import rules come before every other rule, so the part before the
    // first block is enough; comments are skipped
    std::vector<std::string> urls;
    size_t i = 0;
    const size_t n = css.size();
    while (i < n) {
        if (css.compare(i, 2, "/*") == 0) {
            size_t end = css.find("*/", i + 2);
            if (end == std::string::npos) break;
            i = end + 2;
            continue;
        }
        if (css[i] == '{') break;
        if (css[i] != '@' || lower(css.substr(i, 7)) != "@import") {
            i++;
            continue;
        }
        i += 7;
        while (i < n && isSpace(css[i])) i++;
        std::string url;
        if (i + 4 <= n && lower(css.substr(i, 4)) == "url(") {
            i += 4;
            while (i < n && isSpace(css[i])) i++;
            if (i < n && (css[i] == '"' || css[i] == '\'')) {
                char q = css[i++];
                size_t end = css.find(q, i);
                if (end == std::string::npos) break;
                url = css.substr(i, end - i);
                i = end + 1;
            } else {
                size_t end = css.find(')', i);
                if (end == std::string::npos) break;
                url = css.substr(i, end - i);
                while (!url.empty() && isSpace(url.back())) url.pop_back();
                i = end;
            }
        } else if (i < n && (css[i] == '"' || css[i] == '\'')) {
            char q = css[i++];
            size_t end = css.find(q, i);
            if (end == std::string::npos) break;
            url = css.substr(i, end - i);
            i = end + 1;
        }
        if (!url.empty()) urls.push_back(url);
    }
    return urls;
}

PageStyleSheets::PageStyleSheets(std::string documentUrl, Fetch fetch, Resolve resolve)
    : documentUrl_(std::move(documentUrl)), fetch_(std::move(fetch)), resolve_(std::move(resolve)) {}

std::vector<PageStyleSheets::Source> PageStyleSheets::collect(DOMDocument* document) const {
    std::vector<Source> sources;
    if (!document) return sources;
    // Tree order over the document (not shadow trees: they have their own
    // <style> sheets; not template contents, which are not in the tree)
    std::vector<const DOMNode*> stack{document};
    while (!stack.empty()) {
        const DOMNode* node = stack.back();
        stack.pop_back();
        const auto& children = node->childNodes();
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            if ((*it)->nodeType() == NodeType::Element) stack.push_back(it->get());
        }
        if (node->nodeType() != NodeType::Element) continue;
        auto* el = const_cast<DOMElement*>(static_cast<const DOMElement*>(node));
        std::string tag = lower(el->tagName());
        if (tag == "style") {
            Source s;
            s.element = el;
            s.text = el->textContent();
            sources.push_back(std::move(s));
        } else if (tag == "link" && isStylesheetLink(el) && !el->hasAttribute("disabled")) {
            std::string href = el->getAttribute("href");
            if (href.find_first_not_of(" \t\n\r\f") == std::string::npos) continue;
            Source s;
            s.element = el;
            s.link = true;
            s.url = resolve_ ? resolve_(documentUrl_, href) : href;
            sources.push_back(std::move(s));
        }
    }
    return sources;
}

void PageStyleSheets::request(const std::string& url) {
    if (url.empty() || resources_.count(url)) return;
    resources_[url];
    pending_++;
    if (fetch_) fetch_(url);
}

void PageStyleSheets::requestImports(const std::string& css, const std::string& base) {
    for (const auto& href : importedUrls(css)) request(resolve_ ? resolve_(base, href) : href);
}

void PageStyleSheets::resourceArrived(const std::string& url, bool ok, std::string text, const std::string& finalUrl) {
    auto it = resources_.find(url);
    if (it == resources_.end() || it->second.arrived) return;
    Resource& r = it->second;
    r.arrived = true;
    r.ok = ok;
    r.text = ok ? std::move(text) : std::string();
    r.base = finalUrl.empty() ? url : finalUrl;
    if (pending_) pending_--;
    if (!r.ok) return;
    std::vector<std::string> imports;
    for (const auto& href : importedUrls(r.text)) imports.push_back(resolve_ ? resolve_(r.base, href) : href);
    r.imports = imports;
    for (const auto& u : imports) request(u);
}

void PageStyleSheets::requestSheets(DOMDocument* document) {
    for (const Source& s : collect(document)) {
        if (s.link) {
            request(s.url);
        } else if (!styleImports_.count(s.text)) {
            std::vector<std::string> imports;
            for (const auto& href : importedUrls(s.text)) imports.push_back(resolve_ ? resolve_(documentUrl_, href) : href);
            for (const auto& u : imports) request(u);
            styleImports_[s.text] = std::move(imports);
        }
    }
}

bool PageStyleSheets::settled(const std::string& url, int depth) const {
    auto it = resources_.find(url);
    if (it == resources_.end()) return true;   // never asked for: nothing to wait for
    if (!it->second.arrived) return false;
    if (depth >= kMaxImportDepth) return true;
    for (const auto& u : it->second.imports) {
        if (u != url && !settled(u, depth + 1)) return false;
    }
    return true;
}

bool PageStyleSheets::sourceSettled(const Source& source) const {
    if (source.link) return resources_.count(source.url) && settled(source.url);
    auto it = styleImports_.find(source.text);
    if (it == styleImports_.end()) return false;
    for (const auto& u : it->second) {
        if (!settled(u)) return false;
    }
    return true;
}

bool PageStyleSheets::allSettled(DOMDocument* document) {
    requestSheets(document);
    for (const Source& s : collect(document)) {
        if (!sourceSettled(s)) return false;
    }
    return true;
}

const std::vector<StyleResolver::StyleSheetEntry>&
PageStyleSheets::parsed(CSSEngine* css, const std::string& key, const std::string& text, const std::string& href) {
    auto it = parsed_.find(key);
    if (it != parsed_.end()) return it->second;
    // The engine parses the sheet (and the sheets it imports) at its end;
    // those entries are the sheet's, taken back off the list by sync()
    size_t before = css->styleSheets().size();
    css->addStyleSheet(text, StyleOrigin::Author, href);
    const auto& all = css->styleSheets();
    std::vector<StyleResolver::StyleSheetEntry> mine(all.begin() + (long)std::min(before, all.size()), all.end());
    return parsed_[key] = std::move(mine);
}

CSSEngine::ImportFetcher PageStyleSheets::importFetcher() {
    return [this](const std::string& href, const std::string& base, std::string& url, std::string& css) {
        url = resolve_ ? resolve_(base, href) : href;
        auto it = resources_.find(url);
        if (it == resources_.end() || !it->second.arrived || !it->second.ok) return false;
        css = it->second.text;
        // Its own imports resolve against where it really came from
        url = it->second.base;
        return true;
    };
}

bool PageStyleSheets::sync(DOMDocument* document, CSSEngine* css) {
    if (!document || !css) return false;
    requestSheets(document);
    std::vector<Source> sources = collect(document);

    // The usable sheets, in tree order, and the links settled since last time
    std::vector<std::pair<std::string, const Source*>> keys;
    for (const Source& s : sources) {
        if (!sourceSettled(s)) continue;
        if (s.link) {
            const Resource& r = resources_[s.url];
            if (notified_.insert({s.element, s.url}).second) linkEvents_.push_back({s.element, r.ok});
            if (!r.ok) continue;
            keys.push_back({"link:" + s.url, &s});
        } else {
            keys.push_back({"style:" + s.text, &s});
        }
    }
    std::vector<std::string> order;
    order.reserve(keys.size() + extraSheets_.size());
    for (const auto& k : keys) order.push_back(k.first);
    for (const auto& extra : extraSheets_) order.push_back("extra:" + extra);
    if (order == applied_ && appliedTo_ == css) return false;

    // The user-agent (and user) sheets stay first; then the author sheets
    std::vector<StyleResolver::StyleSheetEntry> entries;
    for (const auto& e : css->styleSheets()) {
        if (e.origin != StyleOrigin::Author) entries.push_back(e);
    }
    if (appliedTo_ != css) parsed_.clear();   // entries of another engine
    for (const auto& [key, source] : keys) {
        const auto& mine = source->link
            ? parsed(css, key, resources_[source->url].text, resources_[source->url].base)
            : parsed(css, key, source->text, "");
        entries.insert(entries.end(), mine.begin(), mine.end());
    }
    for (const auto& extra : extraSheets_) {
        const auto& mine = parsed(css, "extra:" + extra, extra, "");
        entries.insert(entries.end(), mine.begin(), mine.end());
    }
    css->resolver().setStyleSheets(std::move(entries));

    // Forget parsed sheets the document no longer has
    std::set<std::string> used(order.begin(), order.end());
    for (auto it = parsed_.begin(); it != parsed_.end();) {
        it = used.count(it->first) ? std::next(it) : parsed_.erase(it);
    }
    applied_ = std::move(order);
    appliedTo_ = css;
    return true;
}

std::vector<std::pair<DOMElement*, bool>> PageStyleSheets::takeLinkEvents() {
    std::vector<std::pair<DOMElement*, bool>> events;
    events.swap(linkEvents_);
    return events;
}

} // namespace ZepraBrowser
