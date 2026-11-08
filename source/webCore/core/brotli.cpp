/**
 * @file brotli.cpp
 * @brief Brotli decoder (RFC 7932): prefix codes, block switching, context
 *        modeling, the distance ring, and the static dictionary with its
 *        word transforms.
 *
 * Section numbers in comments refer to RFC 7932. Reads past the end of the
 * input see zero bits, and decoding stops with Truncated as soon as one of
 * them has been used, so no read leaves the input.
 */

#include "brotli.hpp"

#include <array>
#include <cstring>

namespace Zepra::Compress {

namespace {

#include "brotli_dictionary.inc"
#include "brotli_tables.inc"

constexpr int kMaxCodeLength = 15;
constexpr int kFastBits = 8;
constexpr int kNumTransforms = 121;

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
    bool overrun() const { return usedBits() > size_ * 8; }

    /// Skip to a byte boundary; false if the skipped bits weren't zero (§9.2)
    bool alignZero() {
        int pad = static_cast<int>(usedBits() % 8);
        if (pad == 0) return true;
        return bits(8 - pad) == 0;
    }
    /// Byte position after alignment, for copying stored bytes directly
    size_t bytePos() const { return usedBits() / 8; }
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

/// A canonical prefix code (§3.2), decoded through an 8-bit table with a
/// bit-by-bit walk for longer codes
class PrefixCode {
public:
    /// From code lengths. A code with a single used symbol decodes it from
    /// zero bits (§3.4). False when the lengths over-subscribe the space.
    bool build(const uint16_t* lengths, int n) {
        counts_.fill(0);
        fast_.assign(1u << kFastBits, 0);
        symbols_.assign(static_cast<size_t>(n), 0);
        single_ = -1;
        int used = 0, last = -1;
        for (int i = 0; i < n; ++i) {
            if (lengths[i] > kMaxCodeLength) return false;
            if (lengths[i]) {
                counts_[lengths[i]]++;
                used++;
                last = i;
            }
        }
        if (used == 0) return false;
        if (used == 1) {
            single_ = last;
            return true;
        }
        int left = 1;
        for (int len = 1; len <= kMaxCodeLength; ++len) {
            left = (left << 1) - counts_[len];
            if (left < 0) return false;
        }
        std::array<uint16_t, kMaxCodeLength + 2> offsets{};
        for (int len = 1; len <= kMaxCodeLength; ++len) offsets[len + 1] = offsets[len] + counts_[len];
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbols_[offsets[lengths[i]]++] = static_cast<uint16_t>(i);

        uint32_t code = 0;
        std::array<uint32_t, kMaxCodeLength + 1> next{};
        for (int len = 1; len <= kMaxCodeLength; ++len) {
            code = (code + counts_[len - 1]) << 1;
            next[len] = code;
        }
        for (int i = 0; i < n; ++i) {
            int len = lengths[i];
            if (len == 0) continue;
            uint32_t c = next[len]++;
            if (len > kFastBits) continue;
            uint32_t rev = 0;
            for (int b = 0; b < len; ++b) rev |= ((c >> b) & 1u) << (len - 1 - b);
            for (uint32_t fill = rev; fill < (1u << kFastBits); fill += 1u << len)
                fast_[fill] = static_cast<uint32_t>((i << 4) | len);
        }
        return true;
    }

    int decode(BitReader& in) const {
        if (single_ >= 0) return single_;
        uint32_t peeked = in.peek(kMaxCodeLength);
        uint32_t entry = fast_[peeked & ((1u << kFastBits) - 1)];
        if (entry) {
            in.drop(static_cast<int>(entry & 15));
            return static_cast<int>(entry >> 4);
        }
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxCodeLength; ++len) {
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
    std::array<uint16_t, kMaxCodeLength + 1> counts_{};
    std::vector<uint16_t> symbols_;
    std::vector<uint32_t> fast_;
    int single_ = -1;
};

// §9.2 block counts
constexpr uint32_t kBlockCountBase[26] = {1, 5, 9, 13, 17, 25, 33, 41, 49, 65, 81, 97, 113,
                                          145, 177, 209, 241, 305, 369, 497, 753, 1265, 2289,
                                          4337, 8433, 16625};
constexpr uint8_t kBlockCountExtra[26] = {2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5,
                                          5, 5, 5, 6, 6, 7, 8, 9, 10, 11, 12, 13, 24};
// §5 insert and copy lengths
constexpr uint32_t kInsertBase[24] = {0, 1, 2, 3, 4, 5, 6, 8, 10, 14, 18, 26,
                                      34, 50, 66, 98, 130, 194, 322, 578, 1090, 2114, 6210, 22594};
constexpr uint8_t kInsertExtra[24] = {0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3,
                                      4, 4, 5, 5, 6, 7, 8, 9, 10, 12, 14, 24};
constexpr uint32_t kCopyBase[24] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 18,
                                    22, 30, 38, 54, 70, 102, 134, 198, 326, 582, 1094, 2118};
constexpr uint8_t kCopyExtra[24] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2,
                                    3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 24};
constexpr uint8_t kInsertCellBase[11] = {0, 0, 0, 0, 8, 8, 0, 16, 8, 16, 16};
constexpr uint8_t kCopyCellBase[11] = {0, 8, 0, 8, 0, 8, 16, 0, 16, 8, 16};
// §4 short distance codes: which earlier distance, and the offset added
constexpr uint8_t kDistShortIndex[16] = {0, 1, 2, 3, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1};
constexpr int8_t kDistShortOffset[16] = {0, 0, 0, 0, -1, 1, -2, 2, -3, 3, -1, 1, -2, 2, -3, 3};
// §3.5 order of the code length code lengths
constexpr uint8_t kCodeLengthOrder[18] = {1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15};
// §3.5 the fixed code for code length code lengths, indexed by the next 4 bits
constexpr uint8_t kCodeLengthCodeLength[16] = {2, 2, 2, 3, 2, 2, 2, 4, 2, 2, 2, 3, 2, 2, 2, 4};
constexpr uint8_t kCodeLengthCodeValue[16] = {0, 4, 3, 2, 0, 4, 3, 1, 0, 4, 3, 2, 0, 4, 3, 5};
// §8 dictionary: bits of the word index for each word length
constexpr uint8_t kDictBits[25] = {0, 0, 0, 0, 10, 10, 11, 11, 10, 10, 10, 10, 10,
                                   9, 9, 8, 7, 7, 8, 7, 7, 6, 6, 5, 5};

struct DictionaryOffsets {
    uint32_t offset[26] = {};
    DictionaryOffsets() {
        for (int len = 4; len <= 24; ++len)
            offset[len + 1] = offset[len] + static_cast<uint32_t>(len) * (1u << kDictBits[len]);
    }
};
const DictionaryOffsets kDictOffsets;

/// §8: uppercase one UTF-8 character in place; returns how many bytes it spans
int toUpper(uint8_t* p) {
    if (p[0] < 0xC0) {
        if (p[0] >= 'a' && p[0] <= 'z') p[0] ^= 32;
        return 1;
    }
    if (p[0] < 0xE0) {
        p[1] ^= 32;
        return 2;
    }
    p[2] ^= 5;
    return 3;
}

/// §8: a dictionary word with transform `id` applied, appended to out
void transformWord(std::vector<uint8_t>& out, const uint8_t* word, int len, int id) {
    const uint8_t* t = &kTransforms[id * 3];
    int prefix = t[0], type = t[1], suffix = t[2];
    out.insert(out.end(), kTransformAffixes[prefix], kTransformAffixes[prefix] + kTransformAffixLengths[prefix]);
    if (type <= 9) {
        int keep = len - type;
        if (keep > 0) out.insert(out.end(), word, word + keep);
    } else if (type >= 12) {
        int skip = type - 11;
        if (skip < len) out.insert(out.end(), word + skip, word + len);
    } else {
        // Two spare bytes: a multi-byte character at the end may reach past the word
        uint8_t buf[24 + 2] = {};
        std::memcpy(buf, word, static_cast<size_t>(len));
        if (type == 10) {
            if (len > 0) toUpper(buf);
        } else {
            for (int i = 0; i < len;) i += toUpper(buf + i);
        }
        out.insert(out.end(), buf, buf + len);
    }
    out.insert(out.end(), kTransformAffixes[suffix], kTransformAffixes[suffix] + kTransformAffixLengths[suffix]);
}

int bitsFor(int alphabetSize) {
    int b = 0;
    while ((1 << b) < alphabetSize) ++b;
    return b;
}

struct Decoder {
    BitReader in;
    const uint8_t* data;
    size_t size;
    std::vector<uint8_t>& out;
    size_t start;
    size_t maxOut;

    Decoder(const uint8_t* d, size_t n, std::vector<uint8_t>& o, size_t limit)
        : in(d, n), data(d), size(n), out(o), start(o.size()), maxOut(limit) {}

    size_t produced() const { return out.size() - start; }
    bool room(size_t n) const { return n <= maxOut - produced(); }

    // §3.4, §3.5
    BrotliStatus readPrefixCode(int alphabetSize, PrefixCode& code) {
        std::vector<uint16_t> lengths(static_cast<size_t>(alphabetSize), 0);
        uint32_t hskip = in.bits(2);
        if (hskip == 1) {
            int nsym = static_cast<int>(in.bits(2)) + 1;
            int bits = bitsFor(alphabetSize);
            int syms[4];
            for (int i = 0; i < nsym; ++i) {
                syms[i] = static_cast<int>(in.bits(bits));
                if (syms[i] >= alphabetSize) return BrotliStatus::BadData;
                for (int j = 0; j < i; ++j)
                    if (syms[j] == syms[i]) return BrotliStatus::BadData;
            }
            static const uint8_t kSimpleLengths[5][4] = {
                {0, 0, 0, 0}, {1, 0, 0, 0}, {1, 1, 0, 0}, {1, 2, 2, 0}, {2, 2, 2, 2}};
            const uint8_t* lens = kSimpleLengths[nsym];
            uint8_t treeSelect[4] = {1, 2, 3, 3};
            if (nsym == 4 && in.bits(1)) lens = treeSelect;
            if (nsym == 1) {
                lengths[static_cast<size_t>(syms[0])] = 1;  // a single symbol; build() makes it zero-bit
            } else {
                for (int i = 0; i < nsym; ++i) lengths[static_cast<size_t>(syms[i])] = lens[i];
            }
            if (in.overrun()) return BrotliStatus::Truncated;
            return code.build(lengths.data(), alphabetSize) ? BrotliStatus::Ok : BrotliStatus::BadData;
        }

        // Complex code: first the lengths of the code length code
        uint16_t clLengths[18] = {};
        int space = 32, nonzero = 0;
        for (int i = static_cast<int>(hskip); i < 18; ++i) {
            uint32_t peeked = in.peek(4);
            int v = kCodeLengthCodeValue[peeked];
            in.drop(kCodeLengthCodeLength[peeked]);
            clLengths[kCodeLengthOrder[i]] = static_cast<uint16_t>(v);
            if (v) {
                space -= 32 >> v;
                nonzero++;
                if (space <= 0) break;
            }
        }
        if (in.overrun()) return BrotliStatus::Truncated;
        if (!(nonzero == 1 || space == 0)) return BrotliStatus::BadData;
        PrefixCode clCode;
        if (!clCode.build(clLengths, 18)) return BrotliStatus::BadData;

        // Then the symbol lengths, with run-length codes 16 and 17
        int symbol = 0, prevLen = 8, repeat = 0, repeatLen = 0;
        int symSpace = 32768;
        while (symbol < alphabetSize && symSpace > 0) {
            int c = clCode.decode(in);
            if (in.overrun()) return BrotliStatus::Truncated;
            if (c < 0) return BrotliStatus::BadData;
            if (c < 16) {
                repeat = 0;
                lengths[static_cast<size_t>(symbol++)] = static_cast<uint16_t>(c);
                if (c) {
                    prevLen = c;
                    symSpace -= 32768 >> c;
                }
                continue;
            }
            int extraBits = c == 16 ? 2 : 3;
            int newLen = c == 16 ? prevLen : 0;
            if (repeatLen != newLen) {
                repeat = 0;
                repeatLen = newLen;
            }
            int oldRepeat = repeat;
            if (repeat > 0) repeat = (repeat - 2) << extraBits;
            repeat += static_cast<int>(in.bits(extraBits)) + 3;
            int delta = repeat - oldRepeat;
            if (in.overrun()) return BrotliStatus::Truncated;
            if (symbol + delta > alphabetSize) return BrotliStatus::BadData;
            for (int k = 0; k < delta; ++k) lengths[static_cast<size_t>(symbol++)] = static_cast<uint16_t>(repeatLen);
            if (repeatLen) symSpace -= delta * (32768 >> repeatLen);
        }
        if (symSpace != 0) return BrotliStatus::BadData;
        return code.build(lengths.data(), alphabetSize) ? BrotliStatus::Ok : BrotliStatus::BadData;
    }

    // §9.2 NBLTYPES / NTREES: a variable-length number 1..256
    uint32_t readVarUint8() {
        if (!in.bits(1)) return 1;
        uint32_t n = in.bits(3);
        if (n == 0) return 2;
        return (1u << n) + in.bits(static_cast<int>(n)) + 1;
    }

    struct Blocks {
        uint32_t ntypes = 1;
        PrefixCode typeCode, countCode;
        uint32_t type = 0, last = 0, secondLast = 1;
        uint32_t count = 1u << 24;
    };

    BrotliStatus readBlockCount(Blocks& b) {
        int sym = b.countCode.decode(in);
        if (sym < 0 || sym >= 26) return BrotliStatus::BadData;
        b.count = kBlockCountBase[sym] + in.bits(kBlockCountExtra[sym]);
        return in.overrun() ? BrotliStatus::Truncated : BrotliStatus::Ok;
    }

    BrotliStatus readBlockHeader(Blocks& b) {
        b = Blocks{};
        b.ntypes = readVarUint8();
        if (in.overrun()) return BrotliStatus::Truncated;
        if (b.ntypes < 2) return BrotliStatus::Ok;
        BrotliStatus s = readPrefixCode(static_cast<int>(b.ntypes) + 2, b.typeCode);
        if (s != BrotliStatus::Ok) return s;
        s = readPrefixCode(26, b.countCode);
        if (s != BrotliStatus::Ok) return s;
        return readBlockCount(b);
    }

    // §6: a block switch command
    BrotliStatus switchBlock(Blocks& b) {
        int sym = b.typeCode.decode(in);
        if (in.overrun()) return BrotliStatus::Truncated;
        if (sym < 0) return BrotliStatus::BadData;
        uint32_t type;
        if (sym == 0) type = b.secondLast;
        else if (sym == 1) type = (b.last + 1) % b.ntypes;
        else type = static_cast<uint32_t>(sym - 2);
        if (type >= b.ntypes) return BrotliStatus::BadData;
        b.secondLast = b.last;
        b.last = type;
        b.type = type;
        return readBlockCount(b);
    }

    // §7.3
    BrotliStatus readContextMap(size_t n, uint32_t ntrees, std::vector<uint8_t>& map) {
        map.assign(n, 0);
        if (ntrees < 2) return BrotliStatus::Ok;
        uint32_t rleMax = in.bits(1) ? in.bits(4) + 1 : 0;
        PrefixCode code;
        BrotliStatus s = readPrefixCode(static_cast<int>(ntrees + rleMax), code);
        if (s != BrotliStatus::Ok) return s;
        size_t i = 0;
        while (i < n) {
            int sym = code.decode(in);
            if (in.overrun()) return BrotliStatus::Truncated;
            if (sym < 0) return BrotliStatus::BadData;
            if (sym == 0) {
                map[i++] = 0;
            } else if (static_cast<uint32_t>(sym) <= rleMax) {
                size_t reps = (size_t{1} << sym) + in.bits(sym);
                if (in.overrun()) return BrotliStatus::Truncated;
                if (i + reps > n) return BrotliStatus::BadData;
                i += reps;  // already zero
            } else {
                map[i++] = static_cast<uint8_t>(static_cast<uint32_t>(sym) - rleMax);
            }
        }
        if (in.bits(1)) {  // inverse move-to-front
            uint8_t mtf[256];
            for (int k = 0; k < 256; ++k) mtf[k] = static_cast<uint8_t>(k);
            for (auto& v : map) {
                uint8_t idx = v, value = mtf[idx];
                v = value;
                std::memmove(mtf + 1, mtf, idx);
                mtf[0] = value;
            }
        }
        if (in.overrun()) return BrotliStatus::Truncated;
        for (uint8_t v : map)
            if (v >= ntrees) return BrotliStatus::BadData;
        return BrotliStatus::Ok;
    }

    BrotliStatus run() {
        // §9.1 stream header
        int wbits;
        if (!in.bits(1)) {
            wbits = 16;
        } else {
            uint32_t n = in.bits(3);
            if (n != 0) {
                wbits = 17 + static_cast<int>(n);
            } else {
                uint32_t m = in.bits(3);
                if (m == 1) return BrotliStatus::BadData;  // large-window Brotli, not RFC 7932
                wbits = m ? 8 + static_cast<int>(m) : 17;
            }
        }
        const size_t maxBackward = (size_t{1} << wbits) - 16;

        uint32_t distRing[4] = {16, 15, 11, 4};
        unsigned distIdx = 3;

        for (;;) {
            // §9.2 meta-block header
            bool isLast = in.bits(1);
            if (isLast && in.bits(1)) return in.overrun() ? BrotliStatus::Truncated : BrotliStatus::Ok;
            uint32_t mnibbles = in.bits(2);
            if (mnibbles == 3) {
                // Metadata: skipped
                if (isLast) return BrotliStatus::BadData;
                if (in.bits(1)) return BrotliStatus::BadData;  // reserved
                uint32_t skipBytes = in.bits(2);
                size_t skipLen = 0;
                for (uint32_t i = 0; i < skipBytes; ++i) {
                    uint32_t b = in.bits(8);
                    if (i + 1 == skipBytes && skipBytes > 1 && b == 0) return BrotliStatus::BadData;
                    skipLen |= size_t{b} << (8 * i);
                }
                if (skipBytes) skipLen += 1;
                if (!in.alignZero()) return BrotliStatus::BadData;
                if (in.overrun()) return BrotliStatus::Truncated;
                size_t pos = in.bytePos();
                if (size - pos < skipLen) return BrotliStatus::Truncated;
                in.seek(pos + skipLen);
                continue;
            }
            int nibbles = 4 + static_cast<int>(mnibbles);
            uint32_t mlenMinus1 = in.bits(nibbles * 4);
            if (nibbles > 4 && (mlenMinus1 >> ((nibbles - 1) * 4)) == 0) return BrotliStatus::BadData;
            size_t mlen = size_t{mlenMinus1} + 1;
            if (in.overrun()) return BrotliStatus::Truncated;

            if (!isLast && in.bits(1)) {
                // Uncompressed meta-block
                if (!in.alignZero()) return BrotliStatus::BadData;
                if (in.overrun()) return BrotliStatus::Truncated;
                size_t pos = in.bytePos();
                if (size - pos < mlen) return BrotliStatus::Truncated;
                if (!room(mlen)) return BrotliStatus::TooLarge;
                out.insert(out.end(), data + pos, data + pos + mlen);
                in.seek(pos + mlen);
                continue;
            }
            if (!room(mlen)) return BrotliStatus::TooLarge;

            BrotliStatus s = compressedMetaBlock(mlen, maxBackward, distRing, distIdx);
            if (s != BrotliStatus::Ok) return s;
            if (isLast) return BrotliStatus::Ok;
        }
    }

    BrotliStatus compressedMetaBlock(size_t mlen, size_t maxBackward, uint32_t* distRing, unsigned& distIdx) {
        Blocks lit, cmd, dist;
        BrotliStatus s;
        if ((s = readBlockHeader(lit)) != BrotliStatus::Ok) return s;
        if ((s = readBlockHeader(cmd)) != BrotliStatus::Ok) return s;
        if ((s = readBlockHeader(dist)) != BrotliStatus::Ok) return s;

        uint32_t npostfix = in.bits(2);
        uint32_t ndirect = in.bits(4) << npostfix;
        std::vector<uint8_t> contextModes(lit.ntypes);
        for (auto& m : contextModes) m = static_cast<uint8_t>(in.bits(2));
        if (in.overrun()) return BrotliStatus::Truncated;

        uint32_t ntreesL = readVarUint8();
        std::vector<uint8_t> cmapL;
        if ((s = readContextMap(64 * size_t{lit.ntypes}, ntreesL, cmapL)) != BrotliStatus::Ok) return s;
        uint32_t ntreesD = readVarUint8();
        std::vector<uint8_t> cmapD;
        if ((s = readContextMap(4 * size_t{dist.ntypes}, ntreesD, cmapD)) != BrotliStatus::Ok) return s;

        std::vector<PrefixCode> litCodes(ntreesL), cmdCodes(cmd.ntypes), distCodes(ntreesD);
        for (auto& c : litCodes)
            if ((s = readPrefixCode(256, c)) != BrotliStatus::Ok) return s;
        for (auto& c : cmdCodes)
            if ((s = readPrefixCode(704, c)) != BrotliStatus::Ok) return s;
        int distAlphabet = 16 + static_cast<int>(ndirect) + (48 << npostfix);
        for (auto& c : distCodes)
            if ((s = readPrefixCode(distAlphabet, c)) != BrotliStatus::Ok) return s;

        const uint32_t postfixMask = (1u << npostfix) - 1;
        size_t remaining = mlen;
        uint8_t p1 = 0, p2 = 0;
        if (out.size() > start) p1 = out.back();
        if (out.size() > start + 1) p2 = out[out.size() - 2];

        while (remaining > 0) {
            if (cmd.count == 0 && (s = switchBlock(cmd)) != BrotliStatus::Ok) return s;
            cmd.count--;
            int sym = cmdCodes[cmd.type].decode(in);
            if (in.overrun()) return BrotliStatus::Truncated;
            if (sym < 0) return BrotliStatus::BadData;
            int cell = sym >> 6;
            int insertCode = kInsertCellBase[cell] + ((sym >> 3) & 7);
            int copyCode = kCopyCellBase[cell] + (sym & 7);
            size_t insertLen = kInsertBase[insertCode] + in.bits(kInsertExtra[insertCode]);
            size_t copyLen = kCopyBase[copyCode] + in.bits(kCopyExtra[copyCode]);
            if (in.overrun()) return BrotliStatus::Truncated;
            if (insertLen > remaining) return BrotliStatus::BadData;

            for (size_t k = 0; k < insertLen; ++k) {
                if (lit.count == 0 && (s = switchBlock(lit)) != BrotliStatus::Ok) return s;
                lit.count--;
                const uint8_t* lut = kContextLookup + 512 * contextModes[lit.type];
                uint8_t ctx = lut[p1] | lut[256 + p2];
                int b = litCodes[cmapL[64 * size_t{lit.type} + ctx]].decode(in);
                if (in.overrun()) return BrotliStatus::Truncated;
                if (b < 0) return BrotliStatus::BadData;
                out.push_back(static_cast<uint8_t>(b));
                p2 = p1;
                p1 = static_cast<uint8_t>(b);
            }
            remaining -= insertLen;
            if (remaining == 0) break;  // the copy half of the last command is unused

            uint32_t dcode = 0;
            size_t distance;
            if (sym < 128) {
                distance = distRing[distIdx & 3];  // implicit distance code 0
            } else {
                if (dist.count == 0 && (s = switchBlock(dist)) != BrotliStatus::Ok) return s;
                dist.count--;
                int dctx = copyLen > 4 ? 3 : static_cast<int>(copyLen) - 2;
                int code = distCodes[cmapD[4 * size_t{dist.type} + static_cast<size_t>(dctx)]].decode(in);
                if (in.overrun()) return BrotliStatus::Truncated;
                if (code < 0) return BrotliStatus::BadData;
                dcode = static_cast<uint32_t>(code);
                if (dcode < 16) {
                    int64_t d = int64_t{distRing[(distIdx - kDistShortIndex[dcode]) & 3]} + kDistShortOffset[dcode];
                    if (d <= 0) return BrotliStatus::BadData;
                    distance = static_cast<size_t>(d);
                } else if (dcode < 16 + ndirect) {
                    distance = dcode - 15;
                } else {
                    uint32_t x = dcode - ndirect - 16;
                    int ndistbits = 1 + static_cast<int>(x >> (npostfix + 1));
                    uint32_t dextra = in.bits(ndistbits);
                    if (in.overrun()) return BrotliStatus::Truncated;
                    uint32_t hcode = x >> npostfix;
                    uint32_t lcode = x & postfixMask;
                    size_t offset = ((size_t{2} + (hcode & 1)) << ndistbits) - 4;
                    distance = ((offset + dextra) << npostfix) + lcode + ndirect + 1;
                }
            }

            size_t maxDistance = produced() < maxBackward ? produced() : maxBackward;
            if (distance > maxDistance) {
                // §8 static dictionary reference
                if (copyLen < 4 || copyLen > 24) return BrotliStatus::BadData;
                size_t wordId = distance - maxDistance - 1;
                int nbits = kDictBits[copyLen];
                size_t index = wordId & ((size_t{1} << nbits) - 1);
                size_t transform = wordId >> nbits;
                if (transform >= static_cast<size_t>(kNumTransforms)) return BrotliStatus::BadData;
                const uint8_t* word = kBrotliDictionary + kDictOffsets.offset[copyLen] + index * copyLen;
                size_t before = out.size();
                transformWord(out, word, static_cast<int>(copyLen), static_cast<int>(transform));
                size_t len = out.size() - before;
                if (len > remaining) return BrotliStatus::BadData;
                remaining -= len;
            } else {
                if (dcode != 0) {
                    distIdx = (distIdx + 1) & 3;
                    distRing[distIdx] = static_cast<uint32_t>(distance);
                }
                if (copyLen > remaining) return BrotliStatus::BadData;
                size_t from = out.size() - distance;
                out.resize(out.size() + copyLen);
                uint8_t* p = out.data();
                size_t to = out.size() - copyLen;
                for (size_t i = 0; i < copyLen; ++i) p[to + i] = p[from + i];  // may overlap itself
                remaining -= copyLen;
            }
            if (out.size() > start) p1 = out.back();
            if (out.size() > start + 1) p2 = out[out.size() - 2];
        }
        return BrotliStatus::Ok;
    }
};

} // namespace

const char* brotliStatusName(BrotliStatus status) {
    switch (status) {
        case BrotliStatus::Ok: return "ok";
        case BrotliStatus::Truncated: return "truncated";
        case BrotliStatus::BadData: return "bad data";
        case BrotliStatus::TooLarge: return "too large";
    }
    return "unknown";
}

BrotliStatus brotliDecode(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t maxOut) {
    Decoder decoder(data, size, out, maxOut);
    return decoder.run();
}

const uint8_t* brotliDictionaryData() { return kBrotliDictionary; }
size_t brotliDictionarySize() { return sizeof(kBrotliDictionary); }

} // namespace Zepra::Compress
