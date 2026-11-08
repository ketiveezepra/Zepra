/**
 * @file woff2_decoder.cpp
 * @brief WOFF2 unpacking (https://www.w3.org/TR/WOFF2/) with Zepra's own
 *        Brotli decoder: the table directory, font collections, and the
 *        glyf/loca and hmtx transforms rebuilt into plain TrueType tables.
 */

#include "web_font_decoder.hpp"

#include "core/brotli.hpp"

#include <algorithm>
#include <cstdlib>

namespace Zepra::Fonts {

namespace {

constexpr WebFontFormat kFmt = WebFontFormat::Woff2;
constexpr size_t kHeaderSize = 48;
constexpr size_t kSfntHeaderSize = 12;
constexpr size_t kSfntEntrySize = 16;

constexpr uint32_t tag(const char (&s)[5]) {
    return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) |
           (uint32_t(uint8_t(s[2])) << 8) | uint32_t(uint8_t(s[3]));
}

constexpr uint32_t kGlyf = tag("glyf");
constexpr uint32_t kLoca = tag("loca");
constexpr uint32_t kHmtx = tag("hmtx");
constexpr uint32_t kHhea = tag("hhea");
constexpr uint32_t kHead = tag("head");
constexpr uint32_t kTtcf = tag("ttcf");

// The 63 tags a directory entry can name by index (WOFF2 section 4.1)
constexpr uint32_t kKnownTags[63] = {
    tag("cmap"), tag("head"), tag("hhea"), tag("hmtx"), tag("maxp"), tag("name"), tag("OS/2"),
    tag("post"), tag("cvt "), tag("fpgm"), tag("glyf"), tag("loca"), tag("prep"), tag("CFF "),
    tag("VORG"), tag("EBDT"), tag("EBLC"), tag("gasp"), tag("hdmx"), tag("kern"), tag("LTSH"),
    tag("PCLT"), tag("VDMX"), tag("vhea"), tag("vmtx"), tag("BASE"), tag("GDEF"), tag("GPOS"),
    tag("GSUB"), tag("EBSC"), tag("JSTF"), tag("MATH"), tag("CBDT"), tag("CBLC"), tag("COLR"),
    tag("CPAL"), tag("SVG "), tag("sbix"), tag("acnt"), tag("avar"), tag("bdat"), tag("bloc"),
    tag("bsln"), tag("cvar"), tag("fdsc"), tag("feat"), tag("fmtx"), tag("fvar"), tag("gvar"),
    tag("hsty"), tag("just"), tag("lcar"), tag("mort"), tag("morx"), tag("opbd"), tag("prop"),
    tag("trak"), tag("Zapf"), tag("Silf"), tag("Glat"), tag("Gloc"), tag("Feat"), tag("Sill"),
};

uint32_t be32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

void put16(std::vector<uint8_t>& out, size_t at, uint16_t v) {
    out[at] = static_cast<uint8_t>(v >> 8);
    out[at + 1] = static_cast<uint8_t>(v);
}
void put32(std::vector<uint8_t>& out, size_t at, uint32_t v) {
    out[at] = static_cast<uint8_t>(v >> 24);
    out[at + 1] = static_cast<uint8_t>(v >> 16);
    out[at + 2] = static_cast<uint8_t>(v >> 8);
    out[at + 3] = static_cast<uint8_t>(v);
}
void push16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}
void push32(std::vector<uint8_t>& out, uint32_t v) {
    push16(out, static_cast<uint16_t>(v >> 16));
    push16(out, static_cast<uint16_t>(v));
}

/// Bounds-checked big-endian reads; every read fails once the data runs out
class Reader {
public:
    Reader() = default;
    Reader(const uint8_t* data, size_t size) : p_(data), n_(size) {}

    size_t left() const { return n_ - pos_; }
    size_t pos() const { return pos_; }

    bool u8(uint8_t& v) {
        if (left() < 1) return false;
        v = p_[pos_++];
        return true;
    }
    bool u16(uint16_t& v) {
        if (left() < 2) return false;
        v = be16(p_ + pos_);
        pos_ += 2;
        return true;
    }
    bool s16(int16_t& v) {
        uint16_t u;
        if (!u16(u)) return false;
        v = static_cast<int16_t>(u);
        return true;
    }
    bool u32(uint32_t& v) {
        if (left() < 4) return false;
        v = be32(p_ + pos_);
        pos_ += 4;
        return true;
    }
    bool bytes(size_t k, const uint8_t*& out) {
        if (left() < k) return false;
        out = p_ + pos_;
        pos_ += k;
        return true;
    }
    /// A sub-reader over the next k bytes
    bool sub(size_t k, Reader& out) {
        const uint8_t* s;
        if (!bytes(k, s)) return false;
        out = Reader(s, k);
        return true;
    }

    /// UIntBase128: 1 to 5 bytes, 7 bits each, no leading zeros, fits 32 bits
    bool base128(uint32_t& v) {
        uint32_t r = 0;
        for (int i = 0; i < 5; ++i) {
            uint8_t b;
            if (!u8(b)) return false;
            if (i == 0 && b == 0x80) return false;
            if (r & 0xFE000000u) return false;
            r = (r << 7) | (b & 0x7F);
            if (!(b & 0x80)) {
                v = r;
                return true;
            }
        }
        return false;
    }

    /// 255UInt16: one byte below 253, or a marker and one or two more bytes
    bool u255(uint16_t& v) {
        uint8_t code;
        if (!u8(code)) return false;
        if (code == 253) return u16(v);
        if (code == 254 || code == 255) {
            uint8_t b;
            if (!u8(b)) return false;
            v = static_cast<uint16_t>(b + (code == 254 ? 506 : 253));
            return true;
        }
        v = code;
        return true;
    }

private:
    const uint8_t* p_ = nullptr;
    size_t n_ = 0;
    size_t pos_ = 0;
};

struct Table {
    uint32_t tag = 0;
    bool transformed = false;
    uint32_t origLength = 0;
    size_t srcOffset = 0;  // in the decompressed stream
    uint32_t srcLength = 0;
    bool built = false;    // rebuilt from a transform; `data` holds it
    std::vector<uint8_t> data;
    // Output placement
    uint32_t outOffset = 0;
    uint32_t checksum = 0;
};

struct Font {
    uint32_t flavor = 0;
    std::vector<uint16_t> tables;  // indices into the table list
};

/// What a rebuilt glyf table tells the hmtx transform
struct GlyfInfo {
    uint16_t numGlyphs = 0;
    uint16_t indexFormat = 0;
    std::vector<int16_t> xMins;
};

WebFontDecodeResult fail(std::string why) {
    WebFontDecodeResult r;
    r.format = kFmt;
    r.error = "WOFF2: " + std::move(why);
    return r;
}

// Coordinate deltas of one point from its flag and 1 to 4 data bytes
// (WOFF2 section 5.2, the triplet encoding)
bool decodeTriplet(uint8_t flag, Reader& in, int32_t& dx, int32_t& dy) {
    auto withSign = [](int f, int32_t v) { return (f & 1) ? v : -v; };
    int nBytes = flag < 84 ? 1 : flag < 120 ? 2 : flag < 124 ? 3 : 4;
    const uint8_t* b;
    if (!in.bytes(static_cast<size_t>(nBytes), b)) return false;
    if (flag < 10) {
        dx = 0;
        dy = withSign(flag, ((flag & 14) << 7) + b[0]);
    } else if (flag < 20) {
        dx = withSign(flag, (((flag - 10) & 14) << 7) + b[0]);
        dy = 0;
    } else if (flag < 84) {
        int b0 = flag - 20;
        dx = withSign(flag, 1 + (b0 & 0x30) + (b[0] >> 4));
        dy = withSign(flag >> 1, 1 + ((b0 & 0x0C) << 2) + (b[0] & 0x0F));
    } else if (flag < 120) {
        int b0 = flag - 84;
        dx = withSign(flag, 1 + ((b0 / 12) << 8) + b[0]);
        dy = withSign(flag >> 1, 1 + (((b0 % 12) >> 2) << 8) + b[1]);
    } else if (flag < 124) {
        dx = withSign(flag, (b[0] << 4) + (b[1] >> 4));
        dy = withSign(flag >> 1, ((b[1] & 0x0F) << 8) + b[2]);
    } else {
        dx = withSign(flag, (b[0] << 8) + b[1]);
        dy = withSign(flag >> 1, (b[2] << 8) + b[3]);
    }
    return true;
}

/// Write a simple glyph's flags and coordinates the way TrueType stores them:
/// short deltas where they fit, runs of equal flags folded with REPEAT
void storePoints(const std::vector<int32_t>& xs, const std::vector<int32_t>& ys,
                 const std::vector<bool>& onCurve, bool overlap, std::vector<uint8_t>& out) {
    std::vector<uint8_t> flags, xBytes, yBytes;
    flags.reserve(xs.size());
    int lastFlag = -1;
    int repeat = 0;
    int32_t px = 0, py = 0;
    for (size_t i = 0; i < xs.size(); ++i) {
        int32_t dx = xs[i] - px, dy = ys[i] - py;
        px = xs[i];
        py = ys[i];
        uint8_t flag = onCurve[i] ? 0x01 : 0x00;
        if (i == 0 && overlap) flag |= 0x40;
        if (dx == 0) {
            flag |= 0x10;
        } else if (dx > -256 && dx < 256) {
            flag |= 0x02 | (dx > 0 ? 0x10 : 0);
            xBytes.push_back(static_cast<uint8_t>(std::abs(dx)));
        } else {
            push16(xBytes, static_cast<uint16_t>(dx));
        }
        if (dy == 0) {
            flag |= 0x20;
        } else if (dy > -256 && dy < 256) {
            flag |= 0x04 | (dy > 0 ? 0x20 : 0);
            yBytes.push_back(static_cast<uint8_t>(std::abs(dy)));
        } else {
            push16(yBytes, static_cast<uint16_t>(dy));
        }
        if (flag == lastFlag && repeat > 0 && repeat < 255) {
            ++flags.back();
            ++repeat;
        } else if (flag == lastFlag && repeat == 0) {
            flags.back() |= 0x08;
            flags.push_back(1);
            repeat = 1;
        } else {
            flags.push_back(flag);
            lastFlag = flag;
            repeat = 0;
        }
    }
    out.insert(out.end(), flags.begin(), flags.end());
    out.insert(out.end(), xBytes.begin(), xBytes.end());
    out.insert(out.end(), yBytes.begin(), yBytes.end());
}

/// Rebuild glyf and loca from the transformed glyf table (WOFF2 section 5.1)
bool reconstructGlyf(const uint8_t* src, size_t size, uint32_t locaLength, Table& glyfTable,
                     Table& locaTable, GlyfInfo& info, std::string& error) {
    Reader r(src, size);
    uint16_t reserved, optionFlags, numGlyphs, indexFormat;
    uint32_t sizes[7];
    if (!r.u16(reserved) || !r.u16(optionFlags) || !r.u16(numGlyphs) || !r.u16(indexFormat)) {
        error = "transformed glyf header is truncated";
        return false;
    }
    for (uint32_t& s : sizes)
        if (!r.u32(s)) {
            error = "transformed glyf header is truncated";
            return false;
        }
    if (indexFormat > 1) {
        error = "glyf has an unknown loca format";
        return false;
    }
    Reader nContours, nPoints, flagStream, glyphStream, composite, bbox, instructions, overlap;
    Reader* streams[7] = {&nContours, &nPoints, &flagStream, &glyphStream, &composite, &bbox, &instructions};
    for (int i = 0; i < 7; ++i)
        if (!r.sub(sizes[i], *streams[i])) {
            error = "glyf streams lie outside the table";
            return false;
        }
    bool hasOverlap = optionFlags & 1;
    if (hasOverlap && !r.sub((size_t{numGlyphs} + 7) >> 3, overlap)) {
        error = "glyf overlap bitmap is truncated";
        return false;
    }
    const uint8_t* bboxBitmap;
    const uint8_t* overlapBitmap = nullptr;
    if (!bbox.bytes(((size_t{numGlyphs} + 31) >> 5) << 2, bboxBitmap)) {
        error = "glyf bbox bitmap is truncated";
        return false;
    }
    if (hasOverlap) overlap.bytes(overlap.left(), overlapBitmap);
    if (locaLength != (size_t{numGlyphs} + 1) * (indexFormat ? 4 : 2)) {
        error = "loca size does not match the glyph count";
        return false;
    }

    std::vector<uint8_t>& glyf = glyfTable.data;
    std::vector<uint32_t> offsets(size_t{numGlyphs} + 1);
    info.numGlyphs = numGlyphs;
    info.indexFormat = indexFormat;
    info.xMins.assign(numGlyphs, 0);

    std::vector<uint16_t> endPts;
    std::vector<int32_t> xs, ys;
    std::vector<bool> onCurve;
    auto truncated = [&](const char* what) {
        error = std::string("glyf ") + what + " stream is truncated";
        return false;
    };

    for (uint32_t g = 0; g < numGlyphs; ++g) {
        offsets[g] = static_cast<uint32_t>(glyf.size());
        int16_t nc;
        if (!nContours.s16(nc)) return truncated("contour count");
        bool haveBBox = bboxBitmap[g >> 3] & (0x80 >> (g & 7));
        int16_t box[4] = {0, 0, 0, 0};

        if (nc == 0) {
            if (haveBBox) {
                error = "empty glyph has a bounding box";
                return false;
            }
            continue;
        }

        if (nc == -1) {
            // Composite: components are stored as-is, the bbox is explicit
            if (!haveBBox) {
                error = "composite glyph has no bounding box";
                return false;
            }
            for (int16_t& v : box)
                if (!bbox.s16(v)) return truncated("bbox");
            push16(glyf, static_cast<uint16_t>(nc));
            for (int16_t v : box) push16(glyf, static_cast<uint16_t>(v));
            bool more = true, haveInstructions = false;
            while (more) {
                uint16_t flags, glyphIndex;
                if (!composite.u16(flags) || !composite.u16(glyphIndex)) return truncated("composite");
                size_t extra = (flags & 0x0001) ? 4 : 2;
                if (flags & 0x0008) extra += 2;
                else if (flags & 0x0040) extra += 4;
                else if (flags & 0x0080) extra += 8;
                const uint8_t* rest;
                if (!composite.bytes(extra, rest)) return truncated("composite");
                push16(glyf, flags);
                push16(glyf, glyphIndex);
                glyf.insert(glyf.end(), rest, rest + extra);
                haveInstructions |= (flags & 0x0100) != 0;
                more = (flags & 0x0020) != 0;
            }
            if (haveInstructions) {
                uint16_t len;
                const uint8_t* code;
                if (!glyphStream.u255(len)) return truncated("glyph");
                if (!instructions.bytes(len, code)) return truncated("instruction");
                push16(glyf, len);
                glyf.insert(glyf.end(), code, code + len);
            }
        } else if (nc > 0) {
            endPts.clear();
            uint32_t total = 0;
            for (int c = 0; c < nc; ++c) {
                uint16_t n;
                if (!nPoints.u255(n)) return truncated("point count");
                total += n;
                if (total == 0 || total > 0xFFFF) {
                    error = "glyph has a bad point count";
                    return false;
                }
                endPts.push_back(static_cast<uint16_t>(total - 1));
            }
            xs.resize(total);
            ys.resize(total);
            onCurve.resize(total);
            int32_t x = 0, y = 0;
            for (uint32_t p = 0; p < total; ++p) {
                uint8_t flag;
                if (!flagStream.u8(flag)) return truncated("flag");
                int32_t dx, dy;
                if (!decodeTriplet(flag & 0x7F, glyphStream, dx, dy)) return truncated("glyph");
                x += dx;
                y += dy;
                if (x < -32768 || x > 32767 || y < -32768 || y > 32767) {
                    error = "glyph coordinate is out of range";
                    return false;
                }
                xs[p] = x;
                ys[p] = y;
                onCurve[p] = !(flag & 0x80);
            }
            uint16_t instructionLength;
            const uint8_t* code;
            if (!glyphStream.u255(instructionLength)) return truncated("glyph");
            if (!instructions.bytes(instructionLength, code)) return truncated("instruction");

            if (haveBBox) {
                for (int16_t& v : box)
                    if (!bbox.s16(v)) return truncated("bbox");
            } else {
                auto [xMin, xMax] = std::minmax_element(xs.begin(), xs.end());
                auto [yMin, yMax] = std::minmax_element(ys.begin(), ys.end());
                box[0] = static_cast<int16_t>(*xMin);
                box[1] = static_cast<int16_t>(*yMin);
                box[2] = static_cast<int16_t>(*xMax);
                box[3] = static_cast<int16_t>(*yMax);
            }
            push16(glyf, static_cast<uint16_t>(nc));
            for (int16_t v : box) push16(glyf, static_cast<uint16_t>(v));
            for (uint16_t e : endPts) push16(glyf, e);
            push16(glyf, instructionLength);
            glyf.insert(glyf.end(), code, code + instructionLength);
            bool overlapFlag = overlapBitmap && (overlapBitmap[g >> 3] & (0x80 >> (g & 7)));
            storePoints(xs, ys, onCurve, overlapFlag, glyf);
        } else {
            error = "glyph has a bad contour count";
            return false;
        }

        info.xMins[g] = box[0];
        glyf.resize((glyf.size() + 3) & ~size_t{3}, 0);
        if (glyf.size() > kMaxDecodedFontSize) {
            error = "font is too large";
            return false;
        }
    }
    offsets[numGlyphs] = static_cast<uint32_t>(glyf.size());
    glyfTable.built = true;

    std::vector<uint8_t>& loca = locaTable.data;
    loca.clear();
    loca.reserve(locaLength);
    if (indexFormat == 0 && glyf.size() > 0x1FFFF) {
        error = "glyf is too large for short loca offsets";
        return false;
    }
    for (uint32_t off : offsets) {
        if (indexFormat == 0) push16(loca, static_cast<uint16_t>(off >> 1));
        else push32(loca, off);
    }
    locaTable.built = true;
    (void)reserved;  // version 0; later versions would add data we skip
    return true;
}

/// Rebuild hmtx from its transform: missing side bearings are glyph xMins
/// (WOFF2 section 5.4)
bool reconstructHmtx(const uint8_t* src, size_t size, const GlyfInfo& glyf, uint16_t numHMetrics,
                     Table& out, std::string& error) {
    Reader r(src, size);
    uint8_t flags;
    if (!r.u8(flags) || (flags & 0xFC) || !(flags & 3)) {
        error = "transformed hmtx has bad flags";
        return false;
    }
    if (numHMetrics == 0 || numHMetrics > glyf.numGlyphs) {
        error = "hhea metric count does not fit the glyph count";
        return false;
    }
    std::vector<uint16_t> advances(numHMetrics);
    for (uint16_t& a : advances)
        if (!r.u16(a)) {
            error = "transformed hmtx is truncated";
            return false;
        }
    std::vector<int16_t> lsbs(glyf.numGlyphs);
    for (uint32_t g = 0; g < glyf.numGlyphs; ++g) {
        bool explicitLsb = g < numHMetrics ? !(flags & 1) : !(flags & 2);
        if (!explicitLsb) {
            lsbs[g] = glyf.xMins[g];
        } else if (!r.s16(lsbs[g])) {
            error = "transformed hmtx is truncated";
            return false;
        }
    }
    if (r.left() != 0) {
        error = "transformed hmtx has extra data";
        return false;
    }
    out.data.clear();
    for (uint32_t g = 0; g < glyf.numGlyphs; ++g) {
        if (g < numHMetrics) push16(out.data, advances[g]);
        push16(out.data, static_cast<uint16_t>(lsbs[g]));
    }
    out.built = true;
    return true;
}

uint32_t tableChecksum(const uint8_t* p, size_t n) {
    uint32_t sum = 0;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) sum += be32(p + i);
    if (i < n) {
        uint8_t last[4] = {0, 0, 0, 0};
        for (size_t k = 0; i + k < n; ++k) last[k] = p[i + k];
        sum += be32(last);
    }
    return sum;
}

void writeOffsetTable(std::vector<uint8_t>& out, size_t at, const Font& font,
                      const std::vector<Table>& tables) {
    std::vector<uint16_t> order = font.tables;
    std::sort(order.begin(), order.end(),
              [&](uint16_t a, uint16_t b) { return tables[a].tag < tables[b].tag; });
    uint16_t n = static_cast<uint16_t>(order.size());
    uint16_t entrySelector = 0;
    while ((2u << entrySelector) <= n) ++entrySelector;
    uint16_t searchRange = static_cast<uint16_t>((1u << entrySelector) * 16);
    put32(out, at, font.flavor);
    put16(out, at + 4, n);
    put16(out, at + 6, searchRange);
    put16(out, at + 8, entrySelector);
    put16(out, at + 10, static_cast<uint16_t>(n * 16 - searchRange));
    for (size_t i = 0; i < order.size(); ++i) {
        const Table& t = tables[order[i]];
        size_t e = at + kSfntHeaderSize + i * kSfntEntrySize;
        uint32_t length = t.built ? static_cast<uint32_t>(t.data.size()) : t.origLength;
        put32(out, e, t.tag);
        put32(out, e + 4, t.checksum);
        put32(out, e + 8, t.outOffset);
        put32(out, e + 12, length);
    }
}

} // namespace

WebFontDecodeResult decodeWoff2(const uint8_t* data, size_t size) {
    if (!data || size < kHeaderSize) return fail("header is truncated");
    if (be32(data) != tag("wOF2")) return fail("not a WOFF2 file");
    uint32_t flavor = be32(data + 4);
    uint32_t length = be32(data + 8);
    uint16_t numTables = be16(data + 12);
    uint16_t reserved = be16(data + 14);
    uint32_t totalCompressedSize = be32(data + 20);
    if (length > size) return fail("file is shorter than its header says");
    if (numTables == 0) return fail("file has no tables");
    if (reserved != 0) return fail("header reserved field is not zero");

    // Table directory
    Reader r(data, length);
    const uint8_t* skipped;
    r.bytes(kHeaderSize, skipped);
    std::vector<Table> tables(numTables);
    size_t streamSize = 0;
    for (Table& t : tables) {
        uint8_t flags;
        if (!r.u8(flags)) return fail("table directory is truncated");
        if ((flags & 0x3F) == 0x3F) {
            if (!r.u32(t.tag)) return fail("table directory is truncated");
        } else {
            t.tag = kKnownTags[flags & 0x3F];
        }
        unsigned version = flags >> 6;
        bool glyfOrLoca = t.tag == kGlyf || t.tag == kLoca;
        t.transformed = glyfOrLoca ? version != 3 : version != 0;
        if (t.transformed && !(glyfOrLoca ? version == 0 : (t.tag == kHmtx && version == 1)))
            return fail("table uses an unknown transform");
        if (!r.base128(t.origLength)) return fail("table directory is truncated");
        t.srcLength = t.origLength;
        if (t.transformed) {
            if (!r.base128(t.srcLength)) return fail("table directory is truncated");
            if (t.tag == kLoca && t.srcLength != 0) return fail("transformed loca is not empty");
        }
        t.srcOffset = streamSize;
        streamSize += t.srcLength;
        if (streamSize > kMaxDecodedFontSize || t.origLength > kMaxDecodedFontSize)
            return fail("font is too large");
    }

    // Fonts: one, or a collection's list of table indices per font
    std::vector<Font> fonts;
    uint32_t ttcVersion = 0;
    if (flavor == kTtcf) {
        uint16_t numFonts;
        if (!r.u32(ttcVersion) || !r.u255(numFonts)) return fail("collection directory is truncated");
        if (numFonts == 0) return fail("collection has no fonts");
        fonts.resize(numFonts);
        for (Font& f : fonts) {
            uint16_t n;
            if (!r.u255(n) || !r.u32(f.flavor)) return fail("collection directory is truncated");
            if (n == 0) return fail("collection font has no tables");
            for (uint16_t k = 0; k < n; ++k) {
                uint16_t index;
                if (!r.u255(index)) return fail("collection directory is truncated");
                if (index >= numTables) return fail("collection font names a missing table");
                f.tables.push_back(index);
            }
        }
    } else {
        fonts.resize(1);
        fonts[0].flavor = flavor;
        for (uint16_t i = 0; i < numTables; ++i) fonts[0].tables.push_back(i);
    }
    for (const Font& f : fonts) {
        std::vector<uint32_t> tags;
        for (uint16_t i : f.tables) tags.push_back(tables[i].tag);
        std::sort(tags.begin(), tags.end());
        if (std::adjacent_find(tags.begin(), tags.end()) != tags.end()) return fail("font repeats a table");
    }

    // Every table's data, Brotli-compressed as one stream
    const uint8_t* compressed;
    if (!r.bytes(totalCompressedSize, compressed)) return fail("compressed data lies outside the file");
    std::vector<uint8_t> stream;
    stream.reserve(streamSize);
    auto status = Compress::brotliDecode(compressed, totalCompressedSize, stream, streamSize);
    if (status != Compress::BrotliStatus::Ok)
        return fail(std::string("table data could not be decompressed: ") + Compress::brotliStatusName(status));
    if (stream.size() != streamSize) return fail("table data decompressed to the wrong size");

    // Undo the transforms: glyf and loca per font, then hmtx, which needs glyf
    std::vector<GlyfInfo> glyfInfo(numTables);
    std::vector<int> headLocFormat(numTables, -1);  // the loca format the rebuilt glyf uses
    std::string error;
    for (const Font& f : fonts) {
        int glyf = -1, loca = -1, hmtx = -1, hhea = -1, head = -1;
        for (uint16_t i : f.tables) {
            uint32_t t = tables[i].tag;
            if (t == kGlyf) glyf = i;
            else if (t == kLoca) loca = i;
            else if (t == kHmtx) hmtx = i;
            else if (t == kHhea) hhea = i;
            else if (t == kHead) head = i;
        }
        bool glyfT = glyf >= 0 && tables[glyf].transformed;
        bool locaT = loca >= 0 && tables[loca].transformed;
        if (glyfT != locaT)
            return fail("glyf and loca must be transformed together");
        if (glyfT && !tables[glyf].built) {
            const Table& g = tables[glyf];
            if (!reconstructGlyf(stream.data() + g.srcOffset, g.srcLength, tables[loca].origLength,
                                 tables[glyf], tables[loca], glyfInfo[glyf], error))
                return fail(error);
        }
        if (glyfT && head >= 0) headLocFormat[head] = glyfInfo[glyf].indexFormat;
        if (hmtx >= 0 && tables[hmtx].transformed && !tables[hmtx].built) {
            if (!glyfT) return fail("hmtx is transformed but glyf is not");
            if (hhea < 0 || tables[hhea].transformed || tables[hhea].origLength < 36)
                return fail("hmtx needs an hhea table");
            uint16_t numHMetrics = be16(stream.data() + tables[hhea].srcOffset + 34);
            const Table& h = tables[hmtx];
            if (!reconstructHmtx(stream.data() + h.srcOffset, h.srcLength, glyfInfo[glyf], numHMetrics,
                                 tables[hmtx], error))
                return fail(error);
        }
    }

    // Lay out the sfnt (or TTC): headers, then each table once, 4-byte aligned
    size_t headerSize = 0;
    std::vector<size_t> fontOffsets;
    if (flavor == kTtcf) {
        headerSize = 12 + 4 * fonts.size() + (ttcVersion >= 0x00020000u ? 12 : 0);
    }
    for (const Font& f : fonts) {
        fontOffsets.push_back(headerSize);
        headerSize += kSfntHeaderSize + kSfntEntrySize * f.tables.size();
    }
    WebFontDecodeResult result;
    result.format = kFmt;
    std::vector<uint8_t>& out = result.sfnt;
    out.assign(headerSize, 0);
    for (Table& t : tables) {
        const uint8_t* p = t.built ? t.data.data() : stream.data() + t.srcOffset;
        size_t n = t.built ? t.data.size() : t.srcLength;
        out.resize((out.size() + 3) & ~size_t{3}, 0);
        if (out.size() + n > kMaxDecodedFontSize) return fail("font is too large");
        t.outOffset = static_cast<uint32_t>(out.size());
        out.insert(out.end(), p, p + n);
        if (t.tag == kHead && n >= 12) {
            put32(out, t.outOffset + 8, 0);  // checkSumAdjustment, set below
            int locFormat = headLocFormat[static_cast<size_t>(&t - tables.data())];
            if (locFormat >= 0 && n >= 54) put16(out, t.outOffset + 50, static_cast<uint16_t>(locFormat));
        }
        t.checksum = tableChecksum(out.data() + t.outOffset, n);
    }
    out.resize((out.size() + 3) & ~size_t{3}, 0);

    if (flavor == kTtcf) {
        put32(out, 0, kTtcf);
        put32(out, 4, ttcVersion);
        put32(out, 8, static_cast<uint32_t>(fonts.size()));
        for (size_t i = 0; i < fonts.size(); ++i) put32(out, 12 + 4 * i, static_cast<uint32_t>(fontOffsets[i]));
    }
    for (size_t i = 0; i < fonts.size(); ++i) writeOffsetTable(out, fontOffsets[i], fonts[i], tables);

    // A single font's head carries the whole-file checksum
    if (flavor != kTtcf) {
        for (const Table& t : tables)
            if (t.tag == kHead && t.origLength >= 12)
                put32(out, t.outOffset + 8, 0xB1B0AFBAu - tableChecksum(out.data(), out.size()));
    }
    result.ok = true;
    return result;
}

} // namespace Zepra::Fonts
