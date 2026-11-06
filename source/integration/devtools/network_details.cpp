// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file network_details.cpp
 * @brief The Network panel's filter terms, Headers, Response lines and Timings.
 */
#include "network_details.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ZepraBrowser::DevTools {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string urlHostOf(const std::string& url) {
    size_t start = url.find("://");
    start = start == std::string::npos ? 0 : start + 3;
    size_t end = url.find_first_of("/?#", start);
    std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    size_t at = host.rfind('@');
    return at == std::string::npos ? host : host.substr(at + 1);
}

// "50k" -> 51200, "2m" -> 2097152, "300" -> 300; -1 when it isn't a size
double parseSize(const std::string& s) {
    if (s.empty()) return -1;
    char* end = nullptr;
    double n = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return -1;
    std::string unit = lower(end);
    if (unit.empty() || unit == "b") return n;
    if (unit == "k" || unit == "kb") return n * 1024;
    if (unit == "m" || unit == "mb") return n * 1024 * 1024;
    return -1;
}

constexpr const char* kFilterKeys[] = {"status-code", "method", "domain", "mime-type", "larger-than", "is"};

} // namespace

// ---------------------------------------------------------------------------
// Filter
// ---------------------------------------------------------------------------

NetworkFilter::NetworkFilter(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace((unsigned char)text[i])) i++;
        size_t start = i;
        while (i < text.size() && !std::isspace((unsigned char)text[i])) i++;
        if (i == start) break;
        std::string token = text.substr(start, i - start);
        Term term;
        if (token.size() > 1 && token[0] == '-') {
            term.negate = true;
            token.erase(0, 1);
        }
        size_t colon = token.find(':');
        if (colon != std::string::npos && colon + 1 < token.size()) {
            std::string key = lower(token.substr(0, colon));
            for (const char* k : kFilterKeys) {
                if (key == k) {
                    term.key = key;
                    token.erase(0, colon + 1);
                    break;
                }
            }
        }
        term.value = lower(token);
        terms_.push_back(std::move(term));
    }
}

bool NetworkFilter::matches(const zepra::NetworkEntry& e) const {
    for (const Term& t : terms_) {
        bool m;
        if (t.key.empty()) {
            m = lower(e.request.url).find(t.value) != std::string::npos;
        } else if (t.key == "status-code") {
            m = !e.failed && e.response.status_code > 0 && std::to_string(e.response.status_code) == t.value;
        } else if (t.key == "method") {
            m = lower(e.request.method) == t.value;
        } else if (t.key == "domain") {
            m = lower(urlHostOf(e.request.url)).find(t.value) != std::string::npos;
        } else if (t.key == "mime-type") {
            m = lower(e.response.content_type).find(t.value) != std::string::npos;
        } else if (t.key == "larger-than") {
            double limit = parseSize(t.value);
            m = limit >= 0 && (double)e.response.content_length > limit;
        } else {   // is:
            m = (t.value == "failed" && e.failed) ||
                ((t.value == "pending" || t.value == "running") && !e.completed && !e.failed);
        }
        if (m == t.negate) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Timings
// ---------------------------------------------------------------------------

std::vector<TimingPhase> timingPhases(const zepra::NetworkEntry& e) {
    std::vector<TimingPhase> phases;
    if (!e.completed && !e.failed) return phases;
    const double total = std::max(0.0, e.response.duration_ms);
    const zepra::NetworkTimings& t = e.response.timings;
    if (!t.known) {
        phases.push_back({"Total", 0, total, 0x8A8AF0});
        return phases;
    }
    struct Measured { const char* name; double ms; uint32_t color; };
    const Measured measured[] = {
        {"Redirect", t.redirect > 0 ? t.redirect : -1, 0xD7B600},
        {"DNS Resolution", t.dns, 0x00B7A6},
        {"Connecting", t.connect, 0xF5A623},
        {"TLS Setup", t.tls, 0xB46EF0},
        {"Sending", t.send, 0x5B9EEB},
        {"Waiting", t.wait, 0x8A8AF0},
        {"Receiving", t.receive, 0x58C470},
    };
    double sum = 0;
    for (const auto& m : measured) if (m.ms >= 0) sum += m.ms;
    double at = 0;
    // What the client did not measure happened before it started (the loader
    // thread, a prefetch waiting its turn) or around it (decoding, recording)
    double blocked = total - sum;
    if (blocked >= 0.05) {
        phases.push_back({"Blocked", 0, blocked, 0x6E6E78});
        at = blocked;
    }
    for (const auto& m : measured) {
        if (m.ms < 0) continue;
        phases.push_back({m.name, at, m.ms, m.color});
        at += m.ms;
    }
    return phases;
}

// ---------------------------------------------------------------------------
// Headers
// ---------------------------------------------------------------------------

std::string formatPhase(double ms) {
    char buf[32];
    if (ms < 0) ms = 0;
    if (ms < 10) std::snprintf(buf, sizeof buf, "%.2f ms", ms);
    else if (ms < 1000) std::snprintf(buf, sizeof buf, "%d ms", (int)std::lround(ms));
    else std::snprintf(buf, sizeof buf, "%.2f s", ms / 1000);
    return buf;
}

std::string statusLine(const zepra::NetworkEntry& e) {
    if (e.failed) return "(failed)";
    if (!e.completed) return "(pending)";
    std::string s = std::to_string(e.response.status_code);
    if (!e.response.status_text.empty()) s += " " + e.response.status_text;
    return s;
}

std::vector<HeaderLine> headerLines(const zepra::NetworkEntry& e) {
    std::vector<HeaderLine> lines;
    auto section = [&](const std::string& title) { lines.push_back({true, title, ""}); };
    auto field = [&](const std::string& name, const std::string& value) { lines.push_back({false, name, value}); };

    section("General");
    field("Request URL", e.request.url);
    field("Request Method", e.request.method);
    field("Status Code", statusLine(e));
    if (e.failed && !e.error.empty()) field("Error", e.error);
    if (e.completed && !e.failed) {
        if (!e.response.content_type.empty()) field("Content Type", e.response.content_type);
        std::string size = zepra::formatSize(e.response.content_length);
        field("Transferred", e.response.transferred_bytes
                                 ? zepra::formatSize(e.response.transferred_bytes) + " (" + size + " size)"
                                 : size);
        if (e.response.timings.known) {
            field("Connection", e.response.timings.dns < 0 ? "kept alive (reused)" : "new");
        }
    }
    if (e.request.url.compare(0, 8, "https://") == 0) field("Security", "HTTPS");

    if (!e.response.headers.empty()) {
        section("Response Headers (" + std::to_string(e.response.headers.size()) + ")");
        for (const auto& h : e.response.headers) field(h.name, h.value);
    }
    if (!e.request.headers.empty()) {
        section("Request Headers (" + std::to_string(e.request.headers.size()) + ")");
        for (const auto& h : e.request.headers) field(h.name, h.value);
    }
    return lines;
}

// ---------------------------------------------------------------------------
// Response
// ---------------------------------------------------------------------------

std::string prettyJson(const std::string& text) {
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || (text[first] != '{' && text[first] != '[')) return text;

    std::string out;
    int depth = 0;
    auto newline = [&] {
        out += '\n';
        out.append((size_t)std::max(depth, 0) * 2, ' ');
    };
    for (size_t i = first; i < text.size(); i++) {
        char c = text[i];
        if (c == '"') {
            // A string, escapes and all
            size_t j = i + 1;
            while (j < text.size() && text[j] != '"') j += text[j] == '\\' ? 2 : 1;
            out.append(text, i, std::min(j, text.size() - 1) - i + 1);
            i = j;
            continue;
        }
        if (std::isspace((unsigned char)c)) continue;
        if (c == '{' || c == '[') {
            size_t next = text.find_first_not_of(" \t\r\n", i + 1);
            char close = c == '{' ? '}' : ']';
            if (next != std::string::npos && text[next] == close) {
                out += c;
                out += close;
                i = next;
                continue;
            }
            out += c;
            depth++;
            newline();
        } else if (c == '}' || c == ']') {
            depth--;
            newline();
            out += c;
        } else if (c == ',') {
            out += c;
            newline();
        } else if (c == ':') {
            out += ": ";
        } else {
            out += c;
        }
    }
    return out;
}

std::vector<std::string> responseLines(const std::string& body, const std::string& contentType,
                                       size_t columns, size_t maxLines) {
    std::vector<std::string> lines;
    if (body.empty() || maxLines == 0) return lines;
    const std::string text = lower(contentType).find("json") != std::string::npos ? prettyJson(body) : body;

    std::string line;
    auto push = [&] {
        lines.push_back(std::move(line));
        line.clear();
        return lines.size() < maxLines;
    };
    for (size_t i = 0; i < text.size(); i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') i++;
            if (!push()) return lines;
            continue;
        }
        if (c == '\t') {
            line.append(4 - line.size() % 4, ' ');
        } else if (c >= 0x80) {
            if ((c & 0xC0) == 0x80) continue;   // the rest of a UTF-8 sequence
            line += '?';
        } else {
            line += c < 0x20 || c == 0x7F ? '.' : (char)c;
        }
        if (columns && line.size() >= columns) {
            std::string rest = line.substr(columns);
            line.resize(columns);
            if (!push()) return lines;
            line = std::move(rest);
        }
    }
    if (!line.empty()) lines.push_back(std::move(line));
    return lines;
}

} // namespace ZepraBrowser::DevTools
