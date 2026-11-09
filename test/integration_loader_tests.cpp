// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file integration_loader_tests.cpp
 * @brief The browser's page loader pieces: the fetch pool (per-host limit,
 *        priorities) and PageStyleSheets (<style>/<link> sheets kept in
 *        line with the document, @import preloading, link load/error)
 */

#include <gtest/gtest.h>

#include "fetch_pool.hpp"
#include "page_styles.hpp"

#include "browser/dom.hpp"
#include "css/css_engine.hpp"
#include "html/html_parser.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

using namespace Zepra::WebCore;
using ZepraBrowser::FetchPool;
using ZepraBrowser::PageStyleSheets;

TEST(FetchPool, HostOf) {
    EXPECT_EQ(FetchPool::hostOf("https://WWW.YouTube.com/s/a.js?x"), "https://www.youtube.com");
    EXPECT_EQ(FetchPool::hostOf("http://user:pw@example.test:8080/x"), "http://example.test:8080");
    EXPECT_EQ(FetchPool::hostOf("file:///tmp/a.css"), "");
    EXPECT_EQ(FetchPool::hostOf("data:text/css,a"), "");
}

TEST(FetchPool, LimitsJobsPerHostAndRunsEveryJob) {
    std::atomic<int> done{0};
    {
        FetchPool pool(8, 2);
        for (int i = 0; i < 6; i++) {
            pool.post("https://a.test", FetchPool::Priority::Normal, [&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                done++;
            });
            pool.post("https://b.test", FetchPool::Priority::Normal, [&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                done++;
            });
        }
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (done < 12 && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_EQ(pool.peakRunning("https://a.test"), 2u);
        EXPECT_EQ(pool.peakRunning("https://b.test"), 2u);
    }
    EXPECT_EQ(done, 12);
}

TEST(FetchPool, HigherPriorityGoesFirst) {
    FetchPool pool(1, 6);
    std::mutex m;
    std::condition_variable cv;
    bool release = false;
    std::vector<std::string> order;
    // The only thread is busy; the queue fills meanwhile
    pool.post("h", FetchPool::Priority::Normal, [&] {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [&] { return release; });
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto record = [&](const char* what) { return [&, what] { std::lock_guard<std::mutex> l(m); order.push_back(what); }; };
    pool.post("h", FetchPool::Priority::Low, record("low"));
    pool.post("h", FetchPool::Priority::Normal, record("script1"));
    pool.post("h", FetchPool::Priority::High, record("css"));
    pool.post("h", FetchPool::Priority::Normal, record("script2"));
    {
        std::lock_guard<std::mutex> lock(m);
        release = true;
    }
    cv.notify_all();
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        {
            std::lock_guard<std::mutex> l(m);
            if (order.size() == 4 || std::chrono::steady_clock::now() > until) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::lock_guard<std::mutex> l(m);
    EXPECT_EQ(order, (std::vector<std::string>{"css", "script1", "script2", "low"}));
}

TEST(PageStyles, ImportedUrls) {
    EXPECT_EQ(ZepraBrowser::importedUrls("/* @import 'no.css'; */ @import url(\"a.css\") screen; @import 'b.css';"
                                         "@IMPORT url( c.css ); body { } @import 'late.css';"),
              (std::vector<std::string>{"a.css", "b.css", "c.css"}));
    EXPECT_TRUE(ZepraBrowser::importedUrls("body{color:red}").empty());
}

namespace {

std::string resolveTest(const std::string& base, const std::string& href) {
    if (href.find("://") != std::string::npos) return href;
    return base.substr(0, base.rfind('/') + 1) + href;
}

struct StylePage {
    std::unique_ptr<DOMDocument> doc;
    CSSEngine css;
    std::vector<std::string> requested;
    std::unique_ptr<PageStyleSheets> styles;

    explicit StylePage(const std::string& html) {
        HTMLParser parser;
        doc = parser.parse(html);
        css.initialize(doc.get());
        css.setBaseUrl("https://site.test/index.html");
        styles = std::make_unique<PageStyleSheets>(
            "https://site.test/index.html", [this](const std::string& url) { requested.push_back(url); }, resolveTest);
        css.setImportFetcher(styles->importFetcher());
        css.addStyleSheet("div { color: rgb(0, 0, 0); }", StyleOrigin::UserAgent);
    }
    std::string color(const char* id) {
        styles->sync(doc.get(), &css);
        css.computeStyles();
        const CSSComputedStyle* s = css.getComputedStyle(doc->getElementById(id));
        if (!s) return "none";
        return std::to_string((int)s->color.r) + "," + std::to_string((int)s->color.g) + "," + std::to_string((int)s->color.b);
    }
};

DOMElement* make(DOMDocument* doc, const char* tag, std::map<std::string, std::string> attrs, const char* text = nullptr) {
    auto el = doc->createElement(tag);
    for (const auto& [k, v] : attrs) el->setAttribute(k, v);
    if (text) el->appendChild(doc->createTextNode(text));
    DOMElement* raw = el.get();
    doc->head()->appendChild(std::move(el));
    return raw;
}

}  // namespace

TEST(PageStyles, LinksWaitForTheirSheetAndImportsInTreeOrder) {
    StylePage page(R"(<html><head><link rel="stylesheet" href="a.css"><style>#x{color:rgb(2,2,2)}</style>
        <link rel="alternate stylesheet" href="alt.css"></head><body><div id="x"></div><div id="y"></div></body></html>)");
    EXPECT_FALSE(page.styles->allSettled(page.doc.get()));
    EXPECT_EQ(page.requested, (std::vector<std::string>{"https://site.test/a.css"}));
    EXPECT_EQ(page.color("x"), "2,2,2");   // the <style> applies, the link is not here yet
    page.styles->resourceArrived("https://site.test/a.css", true,
                                 "@import 'sub/i.css'; #x{color:rgb(1,1,1)} #y{color:rgb(5,5,5)}",
                                 "https://cdn.test/css/a.css");
    // Its import resolves against where the sheet came from, and holds it back
    EXPECT_EQ(page.requested.back(), "https://cdn.test/css/sub/i.css");
    EXPECT_FALSE(page.styles->allSettled(page.doc.get()));
    EXPECT_EQ(page.color("x"), "2,2,2");
    EXPECT_TRUE(page.styles->takeLinkEvents().empty());
    page.styles->resourceArrived("https://cdn.test/css/sub/i.css", true, "#y{color:rgb(9,9,9)}", "");
    EXPECT_TRUE(page.styles->allSettled(page.doc.get()));
    // Tree order: the link's sheet (after its import) comes before the <style>
    EXPECT_EQ(page.color("x"), "2,2,2");
    EXPECT_EQ(page.color("y"), "5,5,5");
    auto events = page.styles->takeLinkEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events[0].second);
    EXPECT_EQ(page.styles->pendingFetches(), 0u);
}

TEST(PageStyles, DynamicStyleAndLinkElements) {
    StylePage page(R"(<html><head><style id="s">#x{color:rgb(1,1,1)}</style></head>
        <body><div id="x"></div></body></html>)");
    EXPECT_EQ(page.color("x"), "1,1,1");
    // Text change
    DOMElement* s = page.doc->getElementById("s");
    while (!s->childNodes().empty()) s->removeChild(s->childNodes().front().get());
    s->appendChild(page.doc->createTextNode("#x{color:rgb(3,3,3)}"));
    EXPECT_EQ(page.color("x"), "3,3,3");
    // Inserted <style> wins (later in tree order); removing it restores
    DOMElement* added = make(page.doc.get(), "style", {}, "#x{color:rgb(4,4,4)}");
    EXPECT_EQ(page.color("x"), "4,4,4");
    page.doc->head()->removeChild(added);
    EXPECT_EQ(page.color("x"), "3,3,3");
    // Inserted links: fetched, applied once here, load / error
    DOMElement* good = make(page.doc.get(), "link", {{"rel", "stylesheet"}, {"href", "dyn.css"}});
    DOMElement* bad = make(page.doc.get(), "link", {{"rel", "Stylesheet"}, {"href", "missing.css"}});
    EXPECT_EQ(page.color("x"), "3,3,3");
    EXPECT_EQ(page.requested, (std::vector<std::string>{"https://site.test/dyn.css", "https://site.test/missing.css"}));
    page.styles->resourceArrived("https://site.test/missing.css", false, "", "");
    page.styles->resourceArrived("https://site.test/dyn.css", true, "#x{color:rgb(6,6,6)}", "");
    EXPECT_EQ(page.color("x"), "6,6,6");
    auto events = page.styles->takeLinkEvents();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].first, good);
    EXPECT_TRUE(events[0].second);
    EXPECT_EQ(events[1].first, bad);
    EXPECT_FALSE(events[1].second);
    // Nothing changed: no new sheets, no new events
    EXPECT_FALSE(page.styles->sync(page.doc.get(), &page.css));
    EXPECT_TRUE(page.styles->takeLinkEvents().empty());
    page.doc->head()->removeChild(good);
    EXPECT_EQ(page.color("x"), "3,3,3");
}

// The tree builder gathers character tokens before putting them into the
// text node: a big inline script must not take quadratic time, and text
// split by tags, misnested formatting or foster parenting stays right
TEST(HtmlParserText, LongTextIsLinearAndTextStaysInPlace) {
    std::string code(400000, 'x');
    std::string html = "<html><head><script id=s>var a='" + code + "';</script></head><body>"
                       "<p id=p>one <b>two<i>three</b>four</i> five</p>"
                       "<table id=t><tr><td>cell</td></tr>stray</table>&amp;&lt;end</body></html>";
    auto start = std::chrono::steady_clock::now();
    HTMLParser parser;
    auto doc = parser.parse(html);
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    ASSERT_TRUE(doc);
    EXPECT_LT(ms, 2000.0);
    EXPECT_EQ(doc->getElementById("s")->textContent().size(), code.size() + 9);
    EXPECT_EQ(doc->getElementById("p")->textContent(), "one twothreefour five");
    EXPECT_EQ(doc->getElementById("t")->textContent(), "cell");
    std::string body = doc->body()->textContent();
    EXPECT_NE(body.find("stray"), std::string::npos);
    EXPECT_EQ(body.substr(body.size() - 5), "&<end");
}
