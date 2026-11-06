// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file devtools_network_tests.cpp
 * @brief The Network panel's details: filter terms, the Headers tab, the
 *        Response tab's lines (JSON, wrapping, ASCII only), the Timings
 *        phases, and what the browser keeps of a text body.
 */

#include <gtest/gtest.h>

#include "browser/network_recorder.hpp"
#include "devtools/network_details.hpp"

#include <string>
#include <vector>

using namespace ZepraBrowser::DevTools;

namespace {

zepra::NetworkEntry entry(const std::string& method, const std::string& url, int status,
                          const std::string& contentType = "text/html", size_t size = 1000) {
    zepra::NetworkEntry e;
    e.request.method = method;
    e.request.url = url;
    e.response.status_code = status;
    e.response.status_text = status == 200 ? "OK" : status == 404 ? "Not Found" : "";
    e.response.content_type = contentType;
    e.response.content_length = size;
    e.completed = true;
    return e;
}

std::vector<std::string> names(const std::vector<TimingPhase>& phases) {
    std::vector<std::string> out;
    for (const auto& p : phases) out.push_back(p.name);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Filter box
// ---------------------------------------------------------------------------

TEST(NetworkFilter, PlainTermsMatchTheUrlIgnoringCase) {
    auto css = entry("GET", "https://en.wikipedia.org/w/load.php?modules=Skin.Vector&only=styles", 200, "text/css");
    auto img = entry("GET", "https://upload.wikimedia.org/logo.PNG", 200, "image/png");
    EXPECT_TRUE(NetworkFilter("").matches(css));
    EXPECT_TRUE(NetworkFilter("   ").empty());
    EXPECT_TRUE(NetworkFilter("load.php").matches(css));
    EXPECT_TRUE(NetworkFilter("SKIN.vector").matches(css));
    EXPECT_FALSE(NetworkFilter("load.php").matches(img));
    EXPECT_TRUE(NetworkFilter("logo.png").matches(img));
    // Every term must match
    EXPECT_TRUE(NetworkFilter("wikipedia styles").matches(css));
    EXPECT_FALSE(NetworkFilter("wikipedia logo").matches(css));
    // A full URL is a plain term, not a key
    EXPECT_TRUE(NetworkFilter("https://upload").matches(img));
}

TEST(NetworkFilter, NegatedTermsExclude) {
    auto css = entry("GET", "https://example.test/site.css", 200, "text/css");
    auto js = entry("GET", "https://example.test/app.js", 200, "text/javascript");
    NetworkFilter notCss("-.css");
    EXPECT_FALSE(notCss.matches(css));
    EXPECT_TRUE(notCss.matches(js));
    // A lone "-" is a plain term
    EXPECT_FALSE(NetworkFilter("-").matches(js));
}

TEST(NetworkFilter, KeyedTerms) {
    auto ok = entry("GET", "https://news.ycombinator.com/news.css", 200, "text/css; charset=utf-8", 2048);
    auto missing = entry("POST", "https://api.example.test/v1/items", 404, "application/json", 10);
    auto failed = entry("GET", "https://down.example.test/", 0);
    failed.completed = true;
    failed.failed = true;
    auto pending = entry("GET", "https://slow.example.test/", 0);
    pending.completed = false;

    EXPECT_TRUE(NetworkFilter("status-code:404").matches(missing));
    EXPECT_FALSE(NetworkFilter("status-code:404").matches(ok));
    EXPECT_FALSE(NetworkFilter("status-code:0").matches(failed)) << "a failed request has no status";
    EXPECT_TRUE(NetworkFilter("method:post").matches(missing));
    EXPECT_TRUE(NetworkFilter("METHOD:GET").matches(ok));
    EXPECT_TRUE(NetworkFilter("domain:ycombinator").matches(ok));
    EXPECT_FALSE(NetworkFilter("domain:css").matches(ok)) << "only the host";
    EXPECT_TRUE(NetworkFilter("mime-type:json").matches(missing));
    EXPECT_TRUE(NetworkFilter("larger-than:1k").matches(ok));
    EXPECT_FALSE(NetworkFilter("larger-than:2k").matches(ok));
    EXPECT_FALSE(NetworkFilter("larger-than:lots").matches(ok)) << "not a size";
    EXPECT_TRUE(NetworkFilter("is:failed").matches(failed));
    EXPECT_FALSE(NetworkFilter("is:failed").matches(ok));
    EXPECT_TRUE(NetworkFilter("is:pending").matches(pending));
    EXPECT_TRUE(NetworkFilter("-status-code:200 -is:failed").matches(missing));
    EXPECT_FALSE(NetworkFilter("-status-code:200").matches(ok));
    // An unknown key is URL text
    EXPECT_TRUE(NetworkFilter("v1:items").matches(entry("GET", "https://x.test/v1:items", 200)));
}

// ---------------------------------------------------------------------------
// Timings
// ---------------------------------------------------------------------------

TEST(NetworkTimings, PhasesOfANewHttpsConnection) {
    auto e = entry("GET", "https://example.test/", 200);
    e.response.duration_ms = 500;
    auto& t = e.response.timings;
    t.known = true;
    t.dns = 20;
    t.connect = 80;
    t.tls = 120;
    t.send = 1;
    t.wait = 200;
    t.receive = 49;
    auto phases = timingPhases(e);
    EXPECT_EQ(names(phases), (std::vector<std::string>{"Blocked", "DNS Resolution", "Connecting", "TLS Setup",
                                                       "Sending", "Waiting", "Receiving"}));
    EXPECT_DOUBLE_EQ(phases[0].duration, 30) << "what the client did not measure";
    // Back to back, ending at the request's total
    for (size_t i = 1; i < phases.size(); i++) {
        EXPECT_DOUBLE_EQ(phases[i].start, phases[i - 1].start + phases[i - 1].duration);
    }
    EXPECT_DOUBLE_EQ(phases.back().start + phases.back().duration, 500);
}

TEST(NetworkTimings, AKeptAliveConnectionSkipsDnsConnectAndTls) {
    auto e = entry("GET", "http://example.test/a.css", 200);
    e.response.duration_ms = 60;
    auto& t = e.response.timings;
    t.known = true;
    t.send = 0.5;
    t.wait = 50;
    t.receive = 9.5;
    EXPECT_EQ(names(timingPhases(e)), (std::vector<std::string>{"Sending", "Waiting", "Receiving"}));

    t.redirect = 120;
    e.response.duration_ms = 200;
    auto phases = timingPhases(e);
    EXPECT_EQ(names(phases), (std::vector<std::string>{"Blocked", "Redirect", "Sending", "Waiting", "Receiving"}));
    EXPECT_DOUBLE_EQ(phases[0].duration, 20);
}

TEST(NetworkTimings, UnmeasuredAndPendingRequests) {
    auto e = entry("GET", "file:///tmp/page.html", 200);
    e.response.duration_ms = 3;
    auto phases = timingPhases(e);
    ASSERT_EQ(phases.size(), 1u);
    EXPECT_STREQ(phases[0].name, "Total");
    EXPECT_DOUBLE_EQ(phases[0].duration, 3);

    e.completed = false;
    EXPECT_TRUE(timingPhases(e).empty());

    // A failed request shows how long it took to fail
    e.failed = true;
    e.response.duration_ms = 10000;
    EXPECT_EQ(names(timingPhases(e)), (std::vector<std::string>{"Total"}));
}

TEST(NetworkTimings, PhaseLengthsAreAscii) {
    EXPECT_EQ(formatPhase(0.649), "0.65 ms");
    EXPECT_EQ(formatPhase(9.4), "9.40 ms");
    EXPECT_EQ(formatPhase(240.4), "240 ms");
    EXPECT_EQ(formatPhase(1780), "1.78 s");
    EXPECT_EQ(formatPhase(-1), "0.00 ms");
}

// ---------------------------------------------------------------------------
// Headers
// ---------------------------------------------------------------------------

TEST(NetworkHeaders, GeneralThenResponseThenRequestHeaders) {
    auto e = entry("GET", "https://news.ycombinator.com/", 200, "text/html; charset=utf-8", 34000);
    e.response.transferred_bytes = 35000;
    e.response.headers = {{"Server", "nginx"}, {"Set-Cookie", "a=1"}, {"Set-Cookie", "b=2"}};
    e.request.headers = {{"Host", "news.ycombinator.com"}, {"Cache-Control", "no-cache"}};
    e.response.timings.known = true;
    e.response.timings.dns = -1;

    auto lines = headerLines(e);
    std::vector<std::string> sections;
    for (const auto& l : lines) if (l.section) sections.push_back(l.name);
    EXPECT_EQ(sections, (std::vector<std::string>{"General", "Response Headers (3)", "Request Headers (2)"}));

    auto value = [&](const std::string& name) {
        for (const auto& l : lines) if (!l.section && l.name == name) return l.value;
        return std::string("<none>");
    };
    EXPECT_EQ(value("Request URL"), "https://news.ycombinator.com/");
    EXPECT_EQ(value("Status Code"), "200 OK");
    EXPECT_EQ(value("Transferred"), zepra::formatSize(35000) + " (" + zepra::formatSize(34000) + " size)");
    EXPECT_EQ(value("Connection"), "kept alive (reused)");
    EXPECT_EQ(value("Security"), "HTTPS");
    EXPECT_EQ(value("Cache-Control"), "no-cache");

    // Repeated headers keep their order
    std::vector<std::string> cookies;
    for (const auto& l : lines) if (l.name == "Set-Cookie") cookies.push_back(l.value);
    EXPECT_EQ(cookies, (std::vector<std::string>{"a=1", "b=2"}));
}

TEST(NetworkHeaders, FailedAndPendingRequests) {
    auto e = entry("GET", "http://down.example.test/", 0);
    e.failed = true;
    e.error = "DNS resolution failed";
    auto lines = headerLines(e);
    EXPECT_EQ(statusLine(e), "(failed)");
    bool error = false, transferred = false;
    for (const auto& l : lines) {
        error |= l.name == "Error" && l.value == "DNS resolution failed";
        transferred |= l.name == "Transferred";
    }
    EXPECT_TRUE(error);
    EXPECT_FALSE(transferred);

    e.failed = false;
    e.completed = false;
    EXPECT_EQ(statusLine(e), "(pending)");
    EXPECT_EQ(statusLine(entry("GET", "http://x.test/", 404)), "404 Not Found");
}

// ---------------------------------------------------------------------------
// Response
// ---------------------------------------------------------------------------

TEST(NetworkResponse, JsonIsReindented) {
    EXPECT_EQ(prettyJson(R"({"a":1,"b":[true,null],"c":{},"d":"x,{y}:z"})"),
              "{\n  \"a\": 1,\n  \"b\": [\n    true,\n    null\n  ],\n  \"c\": {},\n  \"d\": \"x,{y}:z\"\n}");
    EXPECT_EQ(prettyJson(R"(["q\"uote",[]])"), "[\n  \"q\\\"uote\",\n  []\n]");
    EXPECT_EQ(prettyJson("not json"), "not json");

    auto lines = responseLines(R"({"ok":true})", "application/json; charset=utf-8", 0, 100);
    EXPECT_EQ(lines, (std::vector<std::string>{"{", "  \"ok\": true", "}"}));
    // Only JSON types are re-indented
    EXPECT_EQ(responseLines(R"({"ok":true})", "text/plain", 0, 100), (std::vector<std::string>{R"({"ok":true})"}));
}

TEST(NetworkResponse, TextLinesAreAsciiWrappedAndCapped) {
    // CRLF and LF, tabs to the next multiple of 4, UTF-8 as one '?' per character
    auto lines = responseLines("a\r\n\tb\nna\xC3\xAFve \xE2\x86\x92 x\x01", "text/html", 0, 100);
    EXPECT_EQ(lines, (std::vector<std::string>{"a", "    b", "na?ve ? x."}));

    // A minified line wraps at the column limit
    auto wrapped = responseLines(std::string(25, 'x') + "\nend", "text/css", 10, 100);
    EXPECT_EQ(wrapped, (std::vector<std::string>{"xxxxxxxxxx", "xxxxxxxxxx", "xxxxx", "end"}));

    auto capped = responseLines("1\n2\n3\n4\n5", "text/plain", 0, 3);
    EXPECT_EQ(capped, (std::vector<std::string>{"1", "2", "3"}));
    EXPECT_TRUE(responseLines("", "text/plain", 80, 10).empty());
}

TEST(NetworkResponse, OnlyTextBodiesAreKeptAndAtMostTheLimit) {
    using Zepra::WebCore::isTextMimeType;
    using Zepra::WebCore::textBodyPreview;
    EXPECT_TRUE(isTextMimeType("text/html; charset=UTF-8"));
    EXPECT_TRUE(isTextMimeType("Application/JSON"));
    EXPECT_TRUE(isTextMimeType("application/javascript"));
    EXPECT_TRUE(isTextMimeType("image/svg+xml"));
    EXPECT_FALSE(isTextMimeType("image/png"));
    EXPECT_FALSE(isTextMimeType("font/woff2"));
    EXPECT_FALSE(isTextMimeType(""));

    bool truncated = true;
    std::string body = "body { color: red }";
    EXPECT_EQ(textBodyPreview("text/css", body.data(), body.size(), 100, truncated), body);
    EXPECT_FALSE(truncated);
    EXPECT_EQ(textBodyPreview("text/css", body.data(), body.size(), 4, truncated), "body");
    EXPECT_TRUE(truncated);
    EXPECT_EQ(textBodyPreview("image/png", body.data(), body.size(), 100, truncated), "");
    EXPECT_FALSE(truncated);
}

TEST(NetworkCache, DisableCacheSwitch) {
    EXPECT_FALSE(Zepra::WebCore::cacheDisabled());
    Zepra::WebCore::setCacheDisabled(true);
    EXPECT_TRUE(Zepra::WebCore::cacheDisabled());
    Zepra::WebCore::setCacheDisabled(false);
    EXPECT_FALSE(Zepra::WebCore::cacheDisabled());
}
