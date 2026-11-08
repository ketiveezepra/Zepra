/**
 * @file inflate.cpp
 * @brief DEFLATE decoder (RFC 1951) plus zlib (RFC 1950) and gzip (RFC 1952).
 *
 * Huffman codes decode through a 9-bit lookup table; the few codes longer
 * than that fall back to a canonical-code walk. Reads past the end of the
 * input see zero bits, and the decoder reports Truncated as soon as it has
 * used one of them, so no read ever leaves the input buffer.
 */

#include "inflate.hpp"

#include <array>
#include <cstring>

namespace Zepra::Compress {

const char* inflateStatusName(InflateStatus status) {
    switch (status) {
        case InflateStatus::Ok: return "ok";
        case InflateStatus::Truncated: return "truncated";
        case InflateStatus::BadData: return "bad data";
        case InflateStatus::BadChecksum: return "bad checksum";
        case InflateStatus::TooLarge: return "too large";
    }
    return "unknown";
}

namespace {

constexpr int kMaxBits = 15;
constexpr int kFastBits = 9;
constexpr int kMaxLitLen = 288;
constexpr int kMaxDist = 32;

constexpr uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                    6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr uint8_t kCodeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                          11, 4, 12, 3, 13, 2, 14, 1, 15};

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    uint32_t peek(int n) {
        if (count_ < n) refill();
        return static_cast<uint32_t>(buf_ & ((uint64_t{1} << n) - 1));
    }
    void drop(int n) {
        buf_ >>= n;
        count_ -= n;
    }
    uint32_t bits(int n) {
        if (n == 0) return 0;
        uint32_t v = peek(n);
        drop(n);
        return v;
    }

    /// True once a bit from past the end of the input has been used.
    bool overrun() const { return usedBits() > size_ * 8; }

    /// Bytes used so far, a partial byte counting as whole.
    size_t usedBytes() const { return (usedBits() + 7) / 8; }

    /// Skip to the next byte boundary and hand back the unread buffered
    /// bytes, so the caller can copy stored data straight from the input.
    size_t alignAndRelease() {
        drop(count_ % 8);
        size_t pos = pos_ - static_cast<size_t>(count_ / 8);
        buf_ = 0;
        count_ = 0;
        pos_ = pos;
        return pos;
    }
    void seek(size_t pos) {
        pos_ = pos;
        buf_ = 0;
        count_ = 0;
    }

private:
    void refill() {
        while (count_ <= 56) {
            uint64_t byte = pos_ < size_ ? data_[pos_] : 0;
            ++pos_;
            buf_ |= byte << count_;
            count_ += 8;
        }
    }
    size_t usedBits() const { return pos_ * 8 - static_cast<size_t>(count_); }

    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    uint64_t buf_ = 0;
    int count_ = 0;
};

class Huffman {
public:
    /// Build from code lengths. Returns false when the lengths over-subscribe
    /// the code space. An incomplete code is accepted; reading one of its
    /// unused codes fails at decode time.
    bool build(const uint8_t* lengths, int n) {
        counts_.fill(0);
        fast_.fill(0);
        for (int i = 0; i < n; ++i) counts_[lengths[i]]++;
        counts_[0] = 0;

        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left <<= 1;
            left -= counts_[len];
            if (left < 0) return false;
        }

        std::array<uint16_t, kMaxBits + 2> offsets{};
        for (int len = 1; len <= kMaxBits; ++len) offsets[len + 1] = offsets[len] + counts_[len];
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbols_[offsets[lengths[i]]++] = static_cast<uint16_t>(i);

        // Canonical codes, bit-reversed because DEFLATE packs them MSB-first
        // into an LSB-first stream
        uint32_t code = 0;
        std::array<uint32_t, kMaxBits + 1> next{};
        for (int len = 1; len <= kMaxBits; ++len) {
            code = (code + counts_[len - 1]) << 1;
            next[len] = code;
        }
        for (int i = 0; i < n; ++i) {
            int len = lengths[i];
            if (len == 0 || len > kFastBits) continue;
            uint32_t c = next[len]++;
            uint32_t rev = 0;
            for (int b = 0; b < len; ++b) rev |= ((c >> b) & 1u) << (len - 1 - b);
            for (uint32_t fill = rev; fill < (1u << kFastBits); fill += 1u << len)
                fast_[fill] = static_cast<uint16_t>((i << 4) | len);
        }
        return true;
    }

    /// Decode one symbol, or -1 for a code the table doesn't assign.
    int decode(BitReader& in) const {
        uint32_t peeked = in.peek(kMaxBits);
        uint16_t entry = fast_[peeked & ((1u << kFastBits) - 1)];
        if (entry) {
            in.drop(entry & 15);
            return entry >> 4;
        }
        // Longer code: walk the canonical code one bit at a time
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= static_cast<int>((peeked >> (len - 1)) & 1u);
            int count = counts_[len];
            if (code - first < count) {
                in.drop(len);
                return symbols_[index + code - first];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

private:
    std::array<uint16_t, kMaxBits + 1> counts_{};
    std::array<uint16_t, kMaxLitLen> symbols_{};
    std::array<uint16_t, 1u << kFastBits> fast_{};
};

struct Inflater {
    BitReader& in;
    std::vector<uint8_t>& out;
    size_t start;
    size_t maxOut;

    bool room(size_t n) const { return out.size() - start <= maxOut && n <= maxOut - (out.size() - start); }

    InflateStatus stored() {
        size_t pos = in.alignAndRelease();
        in.seek(pos);
        uint32_t len = in.bits(16);
        uint32_t nlen = in.bits(16);
        if (in.overrun()) return InflateStatus::Truncated;
        if ((len ^ 0xFFFFu) != nlen) return InflateStatus::BadData;
        pos += 4;
        if (!room(len)) return InflateStatus::TooLarge;
        return copyStored(pos, len);
    }

    // Filled in by the caller, which owns the raw input
    const uint8_t* data = nullptr;
    size_t size = 0;

    InflateStatus copyStored(size_t pos, uint32_t len) {
        if (pos > size || size - pos < len) return InflateStatus::Truncated;
        out.insert(out.end(), data + pos, data + pos + len);
        in.seek(pos + len);
        return InflateStatus::Ok;
    }

    InflateStatus codes(const Huffman& lit, const Huffman& dist) {
        for (;;) {
            int sym = lit.decode(in);
            if (in.overrun()) return InflateStatus::Truncated;
            if (sym < 0) return InflateStatus::BadData;
            if (sym < 256) {
                if (!room(1)) return InflateStatus::TooLarge;
                out.push_back(static_cast<uint8_t>(sym));
                continue;
            }
            if (sym == 256) return InflateStatus::Ok;
            sym -= 257;
            if (sym >= 29) return InflateStatus::BadData;
            size_t len = kLenBase[sym] + in.bits(kLenExtra[sym]);
            int dsym = dist.decode(in);
            if (in.overrun()) return InflateStatus::Truncated;
            if (dsym < 0 || dsym >= 30) return InflateStatus::BadData;
            size_t d = kDistBase[dsym] + in.bits(kDistExtra[dsym]);
            if (in.overrun()) return InflateStatus::Truncated;
            if (d > out.size() - start) return InflateStatus::BadData;
            if (!room(len)) return InflateStatus::TooLarge;
            size_t from = out.size() - d;
            out.resize(out.size() + len);
            uint8_t* p = out.data();
            size_t to = out.size() - len;
            // Byte by byte: a match may overlap the bytes it is producing
            for (size_t i = 0; i < len; ++i) p[to + i] = p[from + i];
        }
    }

    InflateStatus fixed() {
        static const auto tables = [] {
            std::pair<Huffman, Huffman> t;
            uint8_t lengths[kMaxLitLen];
            for (int i = 0; i < 144; ++i) lengths[i] = 8;
            for (int i = 144; i < 256; ++i) lengths[i] = 9;
            for (int i = 256; i < 280; ++i) lengths[i] = 7;
            for (int i = 280; i < 288; ++i) lengths[i] = 8;
            t.first.build(lengths, kMaxLitLen);
            uint8_t dlengths[kMaxDist];
            for (auto& l : dlengths) l = 5;
            t.second.build(dlengths, kMaxDist);
            return t;
        }();
        return codes(tables.first, tables.second);
    }

    InflateStatus dynamic() {
        int nlen = static_cast<int>(in.bits(5)) + 257;
        int ndist = static_cast<int>(in.bits(5)) + 1;
        int ncode = static_cast<int>(in.bits(4)) + 4;
        if (in.overrun()) return InflateStatus::Truncated;
        if (nlen > 286 || ndist > 30) return InflateStatus::BadData;

        uint8_t lengths[kMaxLitLen + kMaxDist] = {};
        for (int i = 0; i < ncode; ++i) lengths[kCodeLengthOrder[i]] = static_cast<uint8_t>(in.bits(3));
        if (in.overrun()) return InflateStatus::Truncated;
        Huffman lencode;
        if (!lencode.build(lengths, 19)) return InflateStatus::BadData;

        std::memset(lengths, 0, sizeof(lengths));
        int index = 0;
        while (index < nlen + ndist) {
            int sym = lencode.decode(in);
            if (in.overrun()) return InflateStatus::Truncated;
            if (sym < 0) return InflateStatus::BadData;
            if (sym < 16) {
                lengths[index++] = static_cast<uint8_t>(sym);
                continue;
            }
            uint8_t value = 0;
            int repeat;
            if (sym == 16) {
                if (index == 0) return InflateStatus::BadData;
                value = lengths[index - 1];
                repeat = 3 + static_cast<int>(in.bits(2));
            } else if (sym == 17) {
                repeat = 3 + static_cast<int>(in.bits(3));
            } else {
                repeat = 11 + static_cast<int>(in.bits(7));
            }
            if (in.overrun()) return InflateStatus::Truncated;
            if (index + repeat > nlen + ndist) return InflateStatus::BadData;
            while (repeat--) lengths[index++] = value;
        }
        if (lengths[256] == 0) return InflateStatus::BadData;  // no end-of-block code

        Huffman lit, dist;
        if (!lit.build(lengths, nlen)) return InflateStatus::BadData;
        if (!dist.build(lengths + nlen, ndist)) return InflateStatus::BadData;
        return codes(lit, dist);
    }

    InflateStatus run() {
        for (;;) {
            uint32_t last = in.bits(1);
            uint32_t type = in.bits(2);
            if (in.overrun()) return InflateStatus::Truncated;
            InflateStatus status;
            switch (type) {
                case 0: status = stored(); break;
                case 1: status = fixed(); break;
                case 2: status = dynamic(); break;
                default: return InflateStatus::BadData;
            }
            if (status != InflateStatus::Ok) return status;
            if (last) return InflateStatus::Ok;
        }
    }
};

const std::array<uint32_t, 256>& crcTable() {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    return table;
}

uint32_t readBE32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}
uint32_t readLE32(const uint8_t* p) {
    return (uint32_t{p[3]} << 24) | (uint32_t{p[2]} << 16) | (uint32_t{p[1]} << 8) | p[0];
}

} // namespace

uint32_t adler32(const uint8_t* data, size_t size, uint32_t start) {
    uint32_t a = start & 0xFFFF, b = start >> 16;
    while (size) {
        // 5552 is the most bytes before the sums can overflow 32 bits
        size_t n = size < 5552 ? size : 5552;
        size -= n;
        while (n--) {
            a += *data++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

uint32_t crc32(const uint8_t* data, size_t size, uint32_t start) {
    const auto& table = crcTable();
    uint32_t c = start ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

InflateStatus inflateRaw(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                         size_t maxOut, size_t* consumed) {
    BitReader in(data, size);
    Inflater inflater{in, out, out.size(), maxOut};
    inflater.data = data;
    inflater.size = size;
    InflateStatus status = inflater.run();
    if (consumed) *consumed = in.usedBytes();
    return status;
}

InflateStatus inflateZlib(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                          size_t maxOut, size_t* consumed) {
    if (size < 2) return InflateStatus::Truncated;
    uint8_t cmf = data[0], flg = data[1];
    if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7) return InflateStatus::BadData;
    if (((uint32_t{cmf} << 8) | flg) % 31 != 0) return InflateStatus::BadData;
    if (flg & 0x20) return InflateStatus::BadData;  // preset dictionary: not used by any format we read

    size_t start = out.size();
    size_t used = 0;
    InflateStatus status = inflateRaw(data + 2, size - 2, out, maxOut, &used);
    if (status != InflateStatus::Ok) return status;
    size_t end = 2 + used;
    if (size - end < 4) return InflateStatus::Truncated;
    if (adler32(out.data() + start, out.size() - start) != readBE32(data + end))
        return InflateStatus::BadChecksum;
    if (consumed) *consumed = end + 4;
    return InflateStatus::Ok;
}

InflateStatus inflateGzip(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                          size_t maxOut) {
    size_t pos = 0;
    size_t total = 0;
    bool first = true;
    while (pos < size) {
        if (size - pos < 10) return first ? InflateStatus::Truncated : InflateStatus::Ok;
        const uint8_t* h = data + pos;
        if (h[0] != 0x1F || h[1] != 0x8B) {
            // Trailing bytes after a complete member are ignored, as browsers do
            return first ? InflateStatus::BadData : InflateStatus::Ok;
        }
        if (h[2] != 8 || (h[3] & 0xE0)) return InflateStatus::BadData;
        uint8_t flags = h[3];
        pos += 10;
        if (flags & 0x04) {  // FEXTRA
            if (size - pos < 2) return InflateStatus::Truncated;
            size_t xlen = data[pos] | (size_t{data[pos + 1]} << 8);
            pos += 2;
            if (size - pos < xlen) return InflateStatus::Truncated;
            pos += xlen;
        }
        for (uint8_t bit : {uint8_t{0x08}, uint8_t{0x10}}) {  // FNAME, FCOMMENT
            if (!(flags & bit)) continue;
            while (pos < size && data[pos] != 0) ++pos;
            if (pos >= size) return InflateStatus::Truncated;
            ++pos;
        }
        if (flags & 0x02) {  // FHCRC
            if (size - pos < 2) return InflateStatus::Truncated;
            pos += 2;
        }

        size_t start = out.size();
        size_t used = 0;
        InflateStatus status = inflateRaw(data + pos, size - pos, out, maxOut - total, &used);
        if (status != InflateStatus::Ok) return status;
        pos += used;
        if (size - pos < 8) return InflateStatus::Truncated;
        size_t produced = out.size() - start;
        if (crc32(out.data() + start, produced) != readLE32(data + pos) ||
            static_cast<uint32_t>(produced) != readLE32(data + pos + 4))
            return InflateStatus::BadChecksum;
        pos += 8;
        total += produced;
        first = false;
    }
    return first ? InflateStatus::Truncated : InflateStatus::Ok;
}

} // namespace Zepra::Compress
