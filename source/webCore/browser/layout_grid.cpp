// Copyright (c) 2026 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file layout_grid.cpp
 * @brief CSS Grid layout (https://www.w3.org/TR/css-grid-1/): track lists
 *        (lengths, %, fr, auto, min/max-content, minmax(), fit-content(),
 *        repeat() with counts, auto-fill and auto-fit, line names),
 *        grid-template-areas, line-based and named placement, auto
 *        placement (row or column flow, sparse), implicit tracks, gaps,
 *        track sizing, and item and content alignment.
 *
 * Simplifications: dense packing places like sparse; subgrid and masonry
 * are not supported; baseline alignment is start; items spanning several
 * tracks spread their size over the spanned intrinsic tracks evenly.
 */

#include "browser/layout_engine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace ZepraBrowser {

namespace {

constexpr float kInfinity = std::numeric_limits<float>::infinity();

// --- Track lists -----------------------------------------------------------

struct TrackSize {
    enum Kind { Fixed, Percent, Fr, Auto, MinContent, MaxContent, FitContent };
    Kind kind = Auto;
    float value = 0;  // px for Fixed and FitContent, % for Percent, the factor for Fr
};

struct Track {
    TrackSize min, max;
};

struct TrackList {
    std::vector<Track> tracks;
    std::vector<std::vector<std::string>> names;  // names[i]: names of line i (tracks.size() + 1)
    // repeat(auto-fill | auto-fit, ...), expanded once the count is known
    int autoAt = -1;  // index in `tracks` where the repetition goes
    bool autoFit = false;
    std::vector<Track> autoTracks;
    std::vector<std::vector<std::string>> autoNames;  // names before each repeated track, + after the last
};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Splits on whitespace (and commas when `commas`), keeping (...) and [...] together
std::vector<std::string> splitTop(const std::string& s, bool commas = false) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char c : s) {
        if (c == '(' || c == '[') depth++;
        if (c == ')' || c == ']') depth = std::max(0, depth - 1);
        bool sep = depth == 0 && (std::isspace(static_cast<unsigned char>(c)) || (commas && c == ','));
        if (sep) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool parseNumber(const std::string& s, float& value, std::string& unit) {
    size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
    bool digits = false;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) {
        digits = true;
        i++;
    }
    if (!digits) return false;
    try {
        value = std::stof(s.substr(0, i));
    } catch (...) {
        return false;
    }
    unit = lower(s.substr(i));
    return true;
}

// A length in px; false for anything else
bool parseLengthPx(const std::string& token, float fontSize, float& px) {
    float v;
    std::string unit;
    if (!parseNumber(token, v, unit)) return false;
    float vpW = layoutViewportWidth(), vpH = layoutViewportHeight();
    if (unit == "px" || (unit.empty() && v == 0)) px = v;
    else if (unit == "em") px = v * fontSize;
    else if (unit == "rem") px = v * 16.0f;
    else if (unit == "vw") px = v * vpW / 100.0f;
    else if (unit == "vh") px = v * vpH / 100.0f;
    else if (unit == "vmin") px = v * std::min(vpW, vpH) / 100.0f;
    else if (unit == "vmax") px = v * std::max(vpW, vpH) / 100.0f;
    else if (unit == "pt") px = v * 96.0f / 72.0f;
    else if (unit == "ch" || unit == "ex") px = v * fontSize * 0.5f;
    else return false;
    return true;
}

bool parseBreadth(const std::string& raw, float fontSize, TrackSize& out) {
    std::string t = lower(raw);
    if (t == "auto") { out = {TrackSize::Auto, 0}; return true; }
    if (t == "min-content") { out = {TrackSize::MinContent, 0}; return true; }
    if (t == "max-content") { out = {TrackSize::MaxContent, 0}; return true; }
    float v;
    std::string unit;
    if (parseNumber(t, v, unit)) {
        if (unit == "fr") { out = {TrackSize::Fr, std::max(0.0f, v)}; return true; }
        if (unit == "%") { out = {TrackSize::Percent, v}; return true; }
    }
    float px;
    if (parseLengthPx(t, fontSize, px)) { out = {TrackSize::Fixed, std::max(0.0f, px)}; return true; }
    return false;
}

std::string inside(const std::string& token, size_t open) {
    size_t close = token.rfind(')');
    if (close == std::string::npos || close <= open) return "";
    return token.substr(open + 1, close - open - 1);
}

bool parseTrack(const std::string& token, float fontSize, Track& out) {
    std::string t = lower(token);
    if (t.rfind("minmax(", 0) == 0) {
        auto args = splitTop(inside(token, 6), true);
        if (args.size() != 2) return false;
        if (!parseBreadth(args[0], fontSize, out.min) || !parseBreadth(args[1], fontSize, out.max)) return false;
        if (out.min.kind == TrackSize::Fr) out.min = {TrackSize::Auto, 0};  // a flexible minimum is invalid; auto
        return true;
    }
    if (t.rfind("fit-content(", 0) == 0) {
        float px;
        std::string arg = inside(token, 11);
        if (!parseLengthPx(arg, fontSize, px)) return false;
        out.min = {TrackSize::Auto, 0};
        out.max = {TrackSize::FitContent, px};
        return true;
    }
    TrackSize s;
    if (!parseBreadth(token, fontSize, s)) return false;
    // A bare fr track is minmax(auto, Nfr)
    out.min = s.kind == TrackSize::Fr ? TrackSize{TrackSize::Auto, 0} : s;
    out.max = s;
    return true;
}

std::vector<std::string> lineNames(const std::string& token) {
    std::vector<std::string> out;
    for (const auto& n : splitTop(token.substr(1, token.size() - 2))) out.push_back(n);
    return out;
}

// grid-template-columns / -rows (and grid-auto-* without names or repeat)
TrackList parseTrackList(const std::string& text, float fontSize) {
    TrackList list;
    list.names.emplace_back();
    std::string t = lower(text);
    if (t.empty() || t == "none") return list;
    for (const auto& token : splitTop(text)) {
        std::string lt = lower(token);
        if (token.front() == '[' && token.back() == ']') {
            for (auto& n : lineNames(token)) list.names.back().push_back(n);
            continue;
        }
        if (lt.rfind("repeat(", 0) == 0) {
            std::string body = inside(token, 6);
            size_t comma = std::string::npos;
            int depth = 0;
            for (size_t i = 0; i < body.size(); ++i) {
                if (body[i] == '(' || body[i] == '[') depth++;
                else if (body[i] == ')' || body[i] == ']') depth--;
                else if (body[i] == ',' && depth == 0) { comma = i; break; }
            }
            if (comma == std::string::npos) continue;
            std::string countText = lower(splitTop(body.substr(0, comma)).empty() ? "" : splitTop(body.substr(0, comma))[0]);
            std::vector<Track> tracks;
            std::vector<std::vector<std::string>> names(1);
            for (const auto& part : splitTop(body.substr(comma + 1))) {
                if (part.front() == '[' && part.back() == ']') {
                    for (auto& n : lineNames(part)) names.back().push_back(n);
                    continue;
                }
                Track tr;
                if (!parseTrack(part, fontSize, tr)) continue;
                tracks.push_back(tr);
                names.emplace_back();
            }
            if (tracks.empty()) continue;
            if (countText == "auto-fill" || countText == "auto-fit") {
                if (list.autoAt >= 0) continue;  // only one auto repetition is allowed
                list.autoAt = static_cast<int>(list.tracks.size());
                list.autoFit = countText == "auto-fit";
                list.autoTracks = tracks;
                list.autoNames = names;
                continue;
            }
            int count = 0;
            try { count = std::stoi(countText); } catch (...) { count = 0; }
            count = std::clamp(count, 0, 10000);
            for (int r = 0; r < count; ++r) {
                for (auto& n : names[0]) list.names.back().push_back(n);
                for (size_t k = 0; k < tracks.size(); ++k) {
                    list.tracks.push_back(tracks[k]);
                    list.names.push_back(names[k + 1]);
                }
            }
            continue;
        }
        Track tr;
        if (!parseTrack(token, fontSize, tr)) continue;
        list.tracks.push_back(tr);
        list.names.emplace_back();
    }
    return list;
}

float definiteSize(const TrackSize& s, float available) {
    if (s.kind == TrackSize::Fixed) return s.value;
    if (s.kind == TrackSize::Percent && available >= 0) return s.value * available / 100.0f;
    return -1;
}

// Expands an auto repetition to fill `available` (CSS Grid §7.2.3.2)
void expandAutoRepeat(TrackList& list, float available, float gap) {
    if (list.autoAt < 0) return;
    int count = 1;
    if (available >= 0) {
        float others = 0;
        for (const Track& t : list.tracks) {
            float s = definiteSize(t.max, available);
            if (s < 0) s = definiteSize(t.min, available);
            others += std::max(0.0f, s);
        }
        float repeatSize = 0;
        bool ok = true;
        for (const Track& t : list.autoTracks) {
            float s = definiteSize(t.max, available);
            float m = definiteSize(t.min, available);
            if (s < 0 || (m >= 0 && m > s)) s = m;
            if (s < 0) { ok = false; break; }
            repeatSize += s;
        }
        size_t n = list.autoTracks.size();
        size_t fixedCount = list.tracks.size();
        if (ok && repeatSize > 0) {
            // Largest count whose tracks and gaps fit
            float free = available - others - gap * static_cast<float>(fixedCount);
            float per = repeatSize + gap * static_cast<float>(n);
            count = std::max(1, static_cast<int>(std::floor((free + gap) / per)));
            count = std::min(count, 10000);
        }
    }
    std::vector<Track> tracks(list.tracks.begin(), list.tracks.begin() + list.autoAt);
    std::vector<std::vector<std::string>> names(list.names.begin(), list.names.begin() + list.autoAt + 1);
    for (int r = 0; r < count; ++r) {
        for (auto& n : list.autoNames[0]) names.back().push_back(n);
        for (size_t k = 0; k < list.autoTracks.size(); ++k) {
            tracks.push_back(list.autoTracks[k]);
            names.push_back(list.autoNames[k + 1]);
        }
    }
    for (auto& n : list.names[list.autoAt]) names.back().push_back(n);
    for (size_t i = static_cast<size_t>(list.autoAt); i < list.tracks.size(); ++i) {
        tracks.push_back(list.tracks[i]);
        names.push_back(list.names[i + 1]);
    }
    list.tracks = std::move(tracks);
    list.names = std::move(names);
}

// --- Areas and placement ---------------------------------------------------

struct Area {
    int rowStart, rowEnd, colStart, colEnd;  // 0-based lines
};

// grid-template-areas: "a a b" "c c b"
bool parseAreas(const std::string& text, std::map<std::string, Area>& areas, int& rows, int& cols) {
    rows = cols = 0;
    std::vector<std::vector<std::string>> grid;
    size_t i = 0;
    while (i < text.size()) {
        char q = text[i];
        if (q != '"' && q != '\'') { ++i; continue; }
        size_t end = text.find(q, i + 1);
        if (end == std::string::npos) break;
        std::vector<std::string> cells;
        std::string row = text.substr(i + 1, end - i - 1), cell;
        for (char c : row) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!cell.empty()) cells.push_back(cell);
                cell.clear();
            } else if (c == '.' && (cell.empty() || cell.find_first_not_of('.') == std::string::npos)) {
                cell += c;  // a run of dots is one null cell token
            } else {
                if (!cell.empty() && cell.find_first_not_of('.') == std::string::npos) {
                    cells.push_back(cell);
                    cell.clear();
                }
                cell += c;
            }
        }
        if (!cell.empty()) cells.push_back(cell);
        grid.push_back(cells);
        i = end + 1;
    }
    if (grid.empty()) return false;
    cols = static_cast<int>(grid[0].size());
    for (const auto& r : grid)
        if (static_cast<int>(r.size()) != cols) return false;  // rows must match
    rows = static_cast<int>(grid.size());
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const std::string& name = grid[r][c];
            if (name[0] == '.') continue;
            auto it = areas.find(name);
            if (it == areas.end()) areas[name] = {r, r + 1, c, c + 1};
            else {
                it->second.rowEnd = std::max(it->second.rowEnd, r + 1);
                it->second.colEnd = std::max(it->second.colEnd, c + 1);
            }
        }
    }
    return true;
}

struct LineSpec {
    bool isAuto = true;
    int line = 0;        // 1-based, negative from the end; 0 when not a number
    int span = 0;        // > 0 for span N
    std::string name;
};

LineSpec parseLine(const std::string& text) {
    LineSpec s;
    auto parts = splitTop(text);
    if (parts.empty() || lower(parts[0]) == "auto") return s;
    bool span = false;
    for (const auto& p : parts) {
        std::string lp = lower(p);
        if (lp == "span") { span = true; continue; }
        int n = 0;
        bool isNum = !p.empty() && (std::isdigit(static_cast<unsigned char>(p[0])) || p[0] == '-' || p[0] == '+');
        if (isNum) {
            try { n = std::stoi(p); } catch (...) { isNum = false; }
        }
        if (isNum) {
            if (span) s.span = std::max(1, n);
            else s.line = n;
        } else {
            s.name = p;
        }
    }
    if (span && s.span == 0) s.span = 1;
    s.isAuto = !span && s.line == 0 && s.name.empty();
    if (span) s.line = 0;
    return s;
}

struct Item {
    LayoutBox* box;
    int rowStart = -1, rowSpan = 1, colStart = -1, colSpan = 1;
};

// Resolves one axis of an item against the explicit grid; start = -1 when
// it is to be auto-placed (span is still set)
void resolveAxis(const LineSpec& start, const LineSpec& end, int explicitTracks, const TrackList& tracks,
                 const std::map<std::string, Area>& areas, bool rowsAxis, int& outStart, int& outSpan) {
    auto namedLine = [&](const std::string& name, bool isStart) -> int {
        auto it = areas.find(name);
        if (it != areas.end()) {
            const Area& a = it->second;
            return rowsAxis ? (isStart ? a.rowStart : a.rowEnd) : (isStart ? a.colStart : a.colEnd);
        }
        std::string suffixed = name + (isStart ? "-start" : "-end");
        for (size_t i = 0; i < tracks.names.size(); ++i)
            for (const auto& n : tracks.names[i])
                if (n == name || n == suffixed) return static_cast<int>(i);
        // area-start / area-end written out
        auto cut = name.rfind(isStart ? "-start" : "-end");
        if (cut != std::string::npos) {
            auto it2 = areas.find(name.substr(0, cut));
            if (it2 != areas.end()) {
                const Area& a = it2->second;
                return rowsAxis ? (isStart ? a.rowStart : a.rowEnd) : (isStart ? a.colStart : a.colEnd);
            }
        }
        return -1;
    };
    auto lineIndex = [&](const LineSpec& s, bool isStart) -> int {
        if (s.span > 0 || s.isAuto) return -1;
        if (!s.name.empty()) return namedLine(s.name, isStart);
        if (s.line > 0) return s.line - 1;
        if (s.line < 0) return std::max(0, explicitTracks + 1 + s.line);
        return -1;
    };
    int a = lineIndex(start, true), b = lineIndex(end, false);
    if (a >= 0 && b >= 0) {
        if (b < a) std::swap(a, b);
        if (a == b) b = a + 1;
        outStart = a;
        outSpan = b - a;
    } else if (a >= 0) {
        outStart = a;
        outSpan = end.span > 0 ? end.span : 1;
    } else if (b >= 0) {
        int span = start.span > 0 ? start.span : 1;
        outStart = std::max(0, b - span);
        outSpan = b - outStart;
    } else {
        outStart = -1;
        outSpan = start.span > 0 ? start.span : (end.span > 0 ? end.span : 1);
    }
    outSpan = std::clamp(outSpan, 1, 1000);
}

// Occupied cells, growing along the major axis as needed
struct Occupancy {
    int minorCount;
    std::vector<std::vector<bool>> rows;  // rows[major][minor]
    bool free(int major, int minor, int majorSpan, int minorSpan) {
        if (minor < 0 || minor + minorSpan > minorCount) return false;
        for (int r = major; r < major + majorSpan; ++r) {
            if (r >= static_cast<int>(rows.size())) continue;
            for (int c = minor; c < minor + minorSpan; ++c)
                if (rows[r][c]) return false;
        }
        return true;
    }
    void mark(int major, int minor, int majorSpan, int minorSpan) {
        while (static_cast<int>(rows.size()) < major + majorSpan) rows.emplace_back(minorCount, false);
        for (int r = major; r < major + majorSpan; ++r)
            for (int c = minor; c < minor + minorSpan && c < minorCount; ++c) rows[r][c] = true;
    }
};

// Auto placement (CSS Grid §8.5), sparse, along rows or columns
void placeItems(std::vector<Item>& items, bool columnFlow, int& rowCount, int& colCount) {
    auto majorStart = [&](Item& it) -> int& { return columnFlow ? it.colStart : it.rowStart; };
    auto minorStart = [&](Item& it) -> int& { return columnFlow ? it.rowStart : it.colStart; };
    auto majorSpan = [&](Item& it) { return columnFlow ? it.colSpan : it.rowSpan; };
    auto minorSpan = [&](Item& it) { return columnFlow ? it.rowSpan : it.colSpan; };

    // The minor axis is as wide as the explicit grid and every item needs
    int minorCount = columnFlow ? rowCount : colCount;
    for (Item& it : items) {
        int start = minorStart(it);
        minorCount = std::max(minorCount, (start >= 0 ? start : 0) + minorSpan(it));
    }
    minorCount = std::max(minorCount, 1);
    Occupancy occ{minorCount, {}};

    // 1. Fully placed items
    for (Item& it : items)
        if (majorStart(it) >= 0 && minorStart(it) >= 0) occ.mark(majorStart(it), minorStart(it), majorSpan(it), minorSpan(it));

    // 2. Items locked to a major track (row in row flow): first free minor slot
    for (Item& it : items) {
        if (majorStart(it) < 0 || minorStart(it) >= 0) continue;
        int m = 0;
        while (!occ.free(majorStart(it), m, majorSpan(it), minorSpan(it)) && m + minorSpan(it) <= minorCount) ++m;
        if (m + minorSpan(it) > minorCount) m = 0;
        minorStart(it) = m;
        occ.mark(majorStart(it), m, majorSpan(it), minorSpan(it));
    }

    // 3. The rest, with a cursor
    int curMajor = 0, curMinor = 0;
    for (Item& it : items) {
        if (majorStart(it) >= 0) continue;
        int ms = majorSpan(it), ns = minorSpan(it);
        if (minorStart(it) >= 0) {
            // Locked to a minor track: advance the major axis until it fits
            if (minorStart(it) < curMinor) curMajor++;
            curMinor = minorStart(it);
            while (!occ.free(curMajor, curMinor, ms, ns)) curMajor++;
        } else {
            for (;;) {
                if (curMinor + ns > minorCount) {
                    curMajor++;
                    curMinor = 0;
                    continue;
                }
                if (occ.free(curMajor, curMinor, ms, ns)) break;
                curMinor++;
            }
            minorStart(it) = curMinor;
        }
        majorStart(it) = curMajor;
        occ.mark(curMajor, curMinor, ms, ns);
        curMinor += ns;
    }

    int majorCount = columnFlow ? colCount : rowCount;
    for (Item& it : items) majorCount = std::max(majorCount, majorStart(it) + majorSpan(it));
    if (columnFlow) { colCount = majorCount; rowCount = minorCount; }
    else { rowCount = majorCount; colCount = minorCount; }
}

// --- Track sizing (CSS Grid §11, simplified) --------------------------------

struct Contribution {
    int start, span;
    float minContent, maxContent;
};

bool intrinsicMin(const TrackSize& s) {
    return s.kind == TrackSize::Auto || s.kind == TrackSize::MinContent || s.kind == TrackSize::MaxContent;
}
bool intrinsicMax(const TrackSize& s) {
    return s.kind == TrackSize::Auto || s.kind == TrackSize::MinContent || s.kind == TrackSize::MaxContent ||
           s.kind == TrackSize::FitContent;
}

/// Sizes of `tracks` for the items' contributions; `available` < 0 when the
/// container's size on this axis is not definite. `stretch`: auto tracks
/// share the space left over (justify/align-content: normal).
std::vector<float> sizeTracks(const std::vector<Track>& tracks, const std::vector<Contribution>& items, float available,
                              float gap, bool stretch, const std::vector<bool>& collapsed) {
    size_t n = tracks.size();
    std::vector<float> base(n, 0), limit(n, 0);
    float gaps = 0;
    {
        int open = 0;
        for (size_t i = 0; i < n; ++i)
            if (!collapsed[i]) open++;
        gaps = gap * static_cast<float>(std::max(0, open - 1));
    }
    float space = available >= 0 ? std::max(0.0f, available - gaps) : -1;

    for (size_t i = 0; i < n; ++i) {
        const Track& t = tracks[i];
        float mn = definiteSize(t.min, available);
        float mx = definiteSize(t.max, available);
        base[i] = mn >= 0 ? mn : 0;
        if (t.max.kind == TrackSize::Fr) limit[i] = kInfinity;
        else if (mx >= 0) limit[i] = mx;
        else if (t.max.kind == TrackSize::Percent) limit[i] = kInfinity;  // indefinite %: auto
        else limit[i] = 0;
        if (t.max.kind == TrackSize::Percent && available < 0) limit[i] = 0;
    }
    auto isFlex = [&](size_t i) { return tracks[i].max.kind == TrackSize::Fr; };
    auto maxIsIntrinsic = [&](size_t i) {
        return intrinsicMax(tracks[i].max) || (tracks[i].max.kind == TrackSize::Percent && available < 0);
    };

    // Items in one track first, then spanning items by span
    std::vector<const Contribution*> order;
    for (const auto& c : items) order.push_back(&c);
    std::stable_sort(order.begin(), order.end(), [](const Contribution* a, const Contribution* b) { return a->span < b->span; });
    std::vector<float> intrinsicLimit(n, 0);
    for (const Contribution* c : order) {
        int s = c->start, e = std::min<int>(c->start + c->span, static_cast<int>(n));
        if (s >= e) continue;
        // Minimum contributions grow intrinsic minimums
        float have = 0;
        std::vector<size_t> grow;
        for (int i = s; i < e; ++i) {
            if (collapsed[i]) continue;
            have += base[i];
            if (intrinsicMin(tracks[i].min)) grow.push_back(static_cast<size_t>(i));
        }
        have += gap * static_cast<float>(std::max(0, e - s - 1));
        float want = c->minContent;
        for (int i = s; i < e; ++i)
            if (tracks[i].min.kind == TrackSize::MaxContent) want = std::max(want, c->maxContent);
        if (want > have && !grow.empty()) {
            float each = (want - have) / static_cast<float>(grow.size());
            for (size_t i : grow) base[i] += each;
        }
        // Maximum contributions grow intrinsic limits
        float haveLimit = 0;
        std::vector<size_t> growLimit;
        bool spansFlex = false;
        for (int i = s; i < e; ++i) {
            if (collapsed[i]) continue;
            if (isFlex(i)) spansFlex = true;
            if (maxIsIntrinsic(i)) growLimit.push_back(static_cast<size_t>(i));
            else haveLimit += std::isinf(limit[i]) ? 0 : limit[i];
        }
        if (spansFlex || growLimit.empty()) continue;
        float limitWant = c->maxContent - gap * static_cast<float>(std::max(0, e - s - 1)) - haveLimit;
        float per = std::max(0.0f, limitWant) / static_cast<float>(growLimit.size());
        for (size_t i : growLimit) {
            float v = per;
            if (tracks[i].max.kind == TrackSize::MinContent) v = c->minContent / static_cast<float>(growLimit.size());
            intrinsicLimit[i] = std::max(intrinsicLimit[i], v);
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (maxIsIntrinsic(i)) {
            limit[i] = intrinsicLimit[i];
            if (tracks[i].max.kind == TrackSize::FitContent) limit[i] = std::min(limit[i], tracks[i].max.value);
        }
        if (!std::isinf(limit[i])) limit[i] = std::max(limit[i], base[i]);
        if (collapsed[i]) base[i] = limit[i] = 0;
    }

    // Maximize: grow non-flex tracks toward their limits (equal shares)
    auto used = [&]() {
        float u = 0;
        for (size_t i = 0; i < n; ++i) u += base[i];
        return u;
    };
    float free = space >= 0 ? space - used() : kInfinity;
    for (int guard = 0; guard < 64 && free > 0.01f; ++guard) {
        std::vector<size_t> open;
        for (size_t i = 0; i < n; ++i)
            if (!collapsed[i] && !isFlex(i) && base[i] < limit[i] - 0.01f) open.push_back(i);
        if (open.empty()) break;
        if (std::isinf(free)) {
            for (size_t i : open) base[i] = limit[i];
            break;
        }
        float share = free / static_cast<float>(open.size());
        for (size_t i : open) {
            float g = std::min(share, limit[i] - base[i]);
            base[i] += g;
            free -= g;
        }
    }

    // Flexible tracks share what is left (CSS Grid §11.7)
    float frSum = 0;
    for (size_t i = 0; i < n; ++i)
        if (isFlex(i) && !collapsed[i]) frSum += tracks[i].max.value;
    if (frSum > 0) {
        float frSize = 0;
        if (space >= 0) {
            std::vector<bool> inflexible(n, false);
            for (int guard = 0; guard < 64; ++guard) {
                float leftover = space, sum = 0;
                for (size_t i = 0; i < n; ++i) {
                    if (collapsed[i]) continue;
                    if (isFlex(i) && !inflexible[i]) sum += tracks[i].max.value;
                    else leftover -= base[i];
                }
                // A sum of factors below 1 takes only that share of the space
                frSize = sum > 0 ? std::max(0.0f, leftover) / std::max(sum, 1.0f) : 0;
                bool changed = false;
                for (size_t i = 0; i < n; ++i) {
                    if (collapsed[i] || !isFlex(i) || inflexible[i]) continue;
                    if (frSize * tracks[i].max.value < base[i]) {
                        inflexible[i] = true;
                        changed = true;
                    }
                }
                if (!changed) break;
            }
        } else {
            // Indefinite: each fr track as large as its content needs
            for (size_t i = 0; i < n; ++i) {
                if (!isFlex(i) || collapsed[i] || tracks[i].max.value <= 0) continue;
                float content = std::max(base[i], intrinsicLimit[i]);
                frSize = std::max(frSize, content / std::max(tracks[i].max.value, 1.0f));
            }
            // fr tracks also take items' max-content
            for (const auto& c : items) {
                int s = c.start, e = std::min<int>(c.start + c.span, static_cast<int>(n));
                float fr = 0, fixed = 0;
                for (int i = s; i < e; ++i) {
                    if (isFlex(i)) fr += tracks[i].max.value;
                    else fixed += base[i];
                }
                if (fr > 0) frSize = std::max(frSize, (c.maxContent - fixed - gap * static_cast<float>(e - s - 1)) / std::max(fr, 1.0f));
            }
        }
        for (size_t i = 0; i < n; ++i)
            if (isFlex(i) && !collapsed[i]) base[i] = std::max(base[i], frSize * tracks[i].max.value);
    } else if (stretch && space >= 0) {
        // Stretch auto tracks into the leftover space
        float left = space - used();
        std::vector<size_t> autos;
        for (size_t i = 0; i < n; ++i)
            if (!collapsed[i] && tracks[i].max.kind == TrackSize::Auto) autos.push_back(i);
        if (left > 0.01f && !autos.empty())
            for (size_t i : autos) base[i] += left / static_cast<float>(autos.size());
    }
    return base;
}

// Offsets of each line along an axis, with content alignment
// (0 start/normal, 1 end, 2 center, 3 space-between, 4 space-around, 5 space-evenly)
std::vector<float> trackOffsets(const std::vector<float>& sizes, const std::vector<bool>& collapsed, float gap,
                                float available, int align) {
    size_t n = sizes.size();
    int open = 0;
    float total = 0;
    for (size_t i = 0; i < n; ++i) {
        total += sizes[i];
        if (!collapsed[i]) open++;
    }
    total += gap * static_cast<float>(std::max(0, open - 1));
    float free = available >= 0 ? std::max(0.0f, available - total) : 0;
    float start = 0, between = 0;
    switch (align) {
        case 1: start = free; break;
        case 2: start = free / 2; break;
        case 3: between = open > 1 ? free / static_cast<float>(open - 1) : 0; break;
        case 4: between = open > 0 ? free / static_cast<float>(open) : 0; start = between / 2; break;
        case 5: between = free / static_cast<float>(open + 1); start = between; break;
        default: break;
    }
    std::vector<float> lines(n + 1, 0);
    float pos = start;
    bool first = true;
    for (size_t i = 0; i < n; ++i) {
        if (!collapsed[i]) {
            if (!first) pos += gap + between;
            first = false;
        }
        lines[i] = pos;
        pos += sizes[i];
    }
    lines[n] = pos;
    return lines;
}

// From the start of track `from` to the end of track `to - 1` (the gaps
// between them included, not the one after)
float areaSize(const std::vector<float>& lines, const std::vector<float>& sizes, int from, int to) {
    if (to <= from) return 0;
    return lines[static_cast<size_t>(to - 1)] + sizes[static_cast<size_t>(to - 1)] - lines[static_cast<size_t>(from)];
}

struct GridSetup {
    TrackList cols, rows;
    std::vector<Track> autoCols, autoRows;
    std::map<std::string, Area> areas;
    std::vector<Item> items;
    int explicitCols = 0, explicitRows = 0;
    int colCount = 0, rowCount = 0;
    std::vector<Track> colTracks, rowTracks;
    std::vector<bool> colCollapsed, rowCollapsed;
};

std::vector<Track> autoTrackList(const std::string& text, float fontSize) {
    TrackList l = parseTrackList(text, fontSize);
    if (l.tracks.empty()) l.tracks.push_back({{TrackSize::Auto, 0}, {TrackSize::Auto, 0}});
    return l.tracks;
}

bool inFlowItem(const LayoutBox& child) {
    return child.type != LayoutType::None && child.positionType != 2 && child.positionType != 3;
}

// Parses the container, places its items and lists the tracks
void setupGrid(LayoutBox& box, float availableWidth, float availableHeight, GridSetup& g) {
    float fs = box.fontSize;
    g.cols = parseTrackList(box.gridTemplateColumns, fs);
    g.rows = parseTrackList(box.gridTemplateRows, fs);
    expandAutoRepeat(g.cols, availableWidth, box.columnGap);
    expandAutoRepeat(g.rows, availableHeight, box.rowGap);
    g.autoCols = autoTrackList(box.gridAutoColumns, fs);
    g.autoRows = autoTrackList(box.gridAutoRows, fs);
    int areaRows = 0, areaCols = 0;
    if (!box.gridTemplateAreas.empty() && lower(box.gridTemplateAreas) != "none")
        parseAreas(box.gridTemplateAreas, g.areas, areaRows, areaCols);
    g.explicitCols = std::max(static_cast<int>(g.cols.tracks.size()), areaCols);
    g.explicitRows = std::max(static_cast<int>(g.rows.tracks.size()), areaRows);

    std::vector<LayoutBox*> boxes;
    for (auto& child : box.children)
        if (inFlowItem(child)) boxes.push_back(&child);
    std::stable_sort(boxes.begin(), boxes.end(), [](const LayoutBox* a, const LayoutBox* b) { return a->order < b->order; });
    for (LayoutBox* child : boxes) {
        Item it{child};
        resolveAxis(parseLine(child->gridColumnStart), parseLine(child->gridColumnEnd), g.explicitCols, g.cols, g.areas,
                    false, it.colStart, it.colSpan);
        resolveAxis(parseLine(child->gridRowStart), parseLine(child->gridRowEnd), g.explicitRows, g.rows, g.areas, true,
                    it.rowStart, it.rowSpan);
        g.items.push_back(it);
    }
    g.colCount = g.explicitCols;
    g.rowCount = g.explicitRows;
    for (const Item& it : g.items) {
        if (it.colStart >= 0) g.colCount = std::max(g.colCount, it.colStart + it.colSpan);
        if (it.rowStart >= 0) g.rowCount = std::max(g.rowCount, it.rowStart + it.rowSpan);
    }
    placeItems(g.items, box.gridAutoFlowColumn, g.rowCount, g.colCount);

    auto trackAt = [](const TrackList& list, int explicitCount, const std::vector<Track>& autos, int i) {
        if (i < static_cast<int>(list.tracks.size())) return list.tracks[i];
        int k = i - explicitCount;
        if (k < 0) k = i - static_cast<int>(list.tracks.size());  // area-only explicit tracks are auto-sized
        return autos[static_cast<size_t>(std::max(0, k)) % autos.size()];
    };
    for (int i = 0; i < g.colCount; ++i) g.colTracks.push_back(trackAt(g.cols, g.explicitCols, g.autoCols, i));
    for (int i = 0; i < g.rowCount; ++i) g.rowTracks.push_back(trackAt(g.rows, g.explicitRows, g.autoRows, i));

    // Collapsed auto-fit tracks are marked by markAutoFit
    g.colCollapsed.assign(static_cast<size_t>(g.colCount), false);
    g.rowCollapsed.assign(static_cast<size_t>(g.rowCount), false);
}

int alignOf(const LayoutBox& item, int containerValue) {
    return item.alignSelf >= 0 ? item.alignSelf : containerValue;
}
int justifyOf(const LayoutBox& item, int containerValue) {
    return item.justifySelf >= 0 ? item.justifySelf : containerValue;
}

bool autoWidth(const LayoutBox& b) { return !b.cssWidth.isSet() || b.cssWidth.isAuto(); }
bool autoHeight(const LayoutBox& b) { return !b.cssHeight.isSet() || b.cssHeight.isAuto(); }

} // namespace

// Records the tracks an auto-fit repetition added so empty ones collapse
static void markAutoFit(GridSetup& g, const std::string& colText, const std::string& rowText, float fs,
                        float availableWidth, float availableHeight, float colGap, float rowGap) {
    auto range = [&](const std::string& text, float available, float gap, int& from, int& to) {
        TrackList raw = parseTrackList(text, fs);
        from = to = -1;
        if (raw.autoAt < 0 || !raw.autoFit) return;
        size_t before = raw.tracks.size();
        expandAutoRepeat(raw, available, gap);
        from = raw.autoAt;
        to = from + static_cast<int>(raw.tracks.size() - before);
    };
    int c0, c1, r0, r1;
    range(colText, availableWidth, colGap, c0, c1);
    range(rowText, availableHeight, rowGap, r0, r1);
    auto apply = [&](int from, int to, std::vector<bool>& collapsed, bool cols) {
        if (from < 0) return;
        std::vector<bool> used(collapsed.size(), false);
        for (const Item& it : g.items) {
            int s = cols ? it.colStart : it.rowStart, n = cols ? it.colSpan : it.rowSpan;
            for (int i = s; i < s + n && i < static_cast<int>(used.size()); ++i) used[static_cast<size_t>(i)] = true;
        }
        for (int i = from; i < to && i < static_cast<int>(collapsed.size()); ++i)
            if (!used[static_cast<size_t>(i)]) collapsed[static_cast<size_t>(i)] = true;
    };
    apply(c0, c1, g.colCollapsed, true);
    apply(r0, r1, g.rowCollapsed, false);
}

void gridIntrinsicWidths(const LayoutBox& constBox, float& minW, float& maxW) {
    LayoutBox& box = const_cast<LayoutBox&>(constBox);  // placement reads children only
    GridSetup g;
    setupGrid(box, -1, -1, g);
    std::vector<Contribution> contribs;
    for (const Item& it : g.items) {
        float cmin, cmax;
        intrinsicWidths(*it.box, cmin, cmax);
        float m = it.box->marginLeft + it.box->marginRight;
        contribs.push_back({it.colStart, it.colSpan, cmin + m, cmax + m});
    }
    auto sum = [&](const std::vector<float>& sizes) {
        float s = 0;
        for (float v : sizes) s += v;
        return s + box.columnGap * static_cast<float>(std::max(0, static_cast<int>(sizes.size()) - 1));
    };
    // Max-content: every track at its max-content size; min-content: at its minimum
    std::vector<Contribution> minContribs = contribs;
    for (auto& c : minContribs) c.maxContent = c.minContent;
    maxW = sum(sizeTracks(g.colTracks, contribs, -1, box.columnGap, false, g.colCollapsed));
    minW = sum(sizeTracks(g.colTracks, minContribs, -1, box.columnGap, false, g.colCollapsed));
}

float layoutGrid(LayoutBox& box, float contentWidth, float contentX, float contentY) {
    float definiteHeight = -1;
    if (box.cssHeight.isSet() && !box.cssHeight.isAuto() && box.height > 0)
        definiteHeight = std::max(0.0f, box.height - box.paddingTop - box.paddingBottom - box.borderTop - box.borderBottom);

    GridSetup g;
    setupGrid(box, contentWidth, definiteHeight, g);
    markAutoFit(g, box.gridTemplateColumns, box.gridTemplateRows, box.fontSize, contentWidth, definiteHeight,
                box.columnGap, box.rowGap);

    // Columns from the items' widths
    std::vector<Contribution> colContribs;
    for (const Item& it : g.items) {
        float cmin, cmax;
        intrinsicWidths(*it.box, cmin, cmax);
        float m = it.box->marginLeft + it.box->marginRight;
        colContribs.push_back({it.colStart, it.colSpan, cmin + m, cmax + m});
    }
    bool stretchCols = box.justifyContent == 0;
    std::vector<float> colSizes =
        sizeTracks(g.colTracks, colContribs, contentWidth, box.columnGap, stretchCols, g.colCollapsed);
    std::vector<float> colLines = trackOffsets(colSizes, g.colCollapsed, box.columnGap, contentWidth, box.justifyContent);

    // Each item at its area's width, then rows from their heights
    std::vector<Contribution> rowContribs;
    std::vector<float> areaWidths;
    for (Item& it : g.items) {
        LayoutBox& c = *it.box;
        int last = std::min<int>(it.colStart + it.colSpan, static_cast<int>(colSizes.size()));
        float aw = std::max(0.0f, areaSize(colLines, colSizes, it.colStart, last));
        areaWidths.push_back(aw);
        float m = c.marginLeft + c.marginRight;
        int justify = justifyOf(c, box.justifyItems);
        if (c.isImage || !autoWidth(c)) {
            layoutBlock(c, aw, 0);
        } else if (justify == 0) {
            layoutBoxAtWidth(c, std::max(0.0f, aw - m));
        } else {
            float cmin, cmax;
            intrinsicWidths(c, cmin, cmax);
            layoutBoxAtWidth(c, std::max(0.0f, std::min(cmax, std::max(cmin, aw - m))));
        }
        float h = c.height + c.marginTop + c.marginBottom;
        rowContribs.push_back({it.rowStart, it.rowSpan, h, h});
    }
    bool stretchRows = box.alignContent == 0;
    std::vector<float> rowSizes =
        sizeTracks(g.rowTracks, rowContribs, definiteHeight, box.rowGap, stretchRows && definiteHeight >= 0, g.rowCollapsed);
    // align-content: 0 normal, 1 start, 2 end, 3 center, 4 space-between,
    // 5 space-around, 6 space-evenly (trackOffsets numbers them from start)
    static const int kRowAlign[] = {0, 0, 1, 2, 3, 4, 5};
    int rowAlign = box.alignContent >= 0 && box.alignContent <= 6 ? kRowAlign[box.alignContent] : 0;
    std::vector<float> rowLines = trackOffsets(rowSizes, g.rowCollapsed, box.rowGap, definiteHeight, rowAlign);

    // Place and align each item in its area
    for (size_t k = 0; k < g.items.size(); ++k) {
        Item& it = g.items[k];
        LayoutBox& c = *it.box;
        int lastRow = std::min<int>(it.rowStart + it.rowSpan, static_cast<int>(rowSizes.size()));
        float ah = std::max(0.0f, areaSize(rowLines, rowSizes, it.rowStart, lastRow));
        float aw = areaWidths[k];
        float ax = contentX + colLines[static_cast<size_t>(it.colStart)];
        float ay = contentY + rowLines[static_cast<size_t>(it.rowStart)];

        int align = alignOf(c, box.alignItems);
        if (align == 0 && autoHeight(c) && !c.isImage) c.height = std::max(c.height, ah - c.marginTop - c.marginBottom);
        float freeY = ah - c.height - c.marginTop - c.marginBottom;
        float dy = align == 2 ? freeY : align == 3 ? freeY / 2 : 0;
        c.y = ay + c.marginTop + std::max(0.0f, dy);

        int justify = justifyOf(c, box.justifyItems);
        float freeX = aw - c.width - c.marginLeft - c.marginRight;
        float dx = justify == 2 ? freeX : justify == 3 ? freeX / 2 : 0;
        c.x = ax + c.marginLeft + std::max(0.0f, dx);
    }

    float used = rowLines.empty() ? 0 : rowLines.back();
    return contentY + used;
}

} // namespace ZepraBrowser
