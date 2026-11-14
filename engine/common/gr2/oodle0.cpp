// Oodle0 decoder: adaptive arithmetic (range) coder with frequency-sorted escape models + LZ-style matches.
// Every routine below mirrors one function of granny2.dll (image base 0x10000000):
//   decompress driver 0x1001d060, chunk decoder 0x1001cbe0, model init 0x1001ca70, model 0x1000e0c0,
//   decode 0x1000e6f0, rank search 0x1000e7f0, rescale 0x1000ec30, rebuild 0x1000e690, layout 0x1000e130,
//   tree add 0x1000e1c0, interval update 0x1000ddc0, uniform 0x1000e020.
#include "oodle0.h"

#include <cstring>
#include <utility>
#include <vector>

namespace gr2 {
namespace {

// ---- arithmetic decoder state ---------------------------------------------------------------------------
struct Coder {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;
    uint32_t bitBuf = 0;
    int bitCnt = 0;
    uint32_t high = 0x7fffffff, low = 0, code = 0;
    bool overrun = false;

    uint32_t word() {
        uint8_t b[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; i++) {
            if (p + i < end) b[i] = p[i];
        }
        if (p >= end) overrun = true;
        p += 4;
        return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
    }
    uint32_t bit() {  // LSB-first bit stream
        if (bitCnt == 0) {
            bitBuf = word();
            bitCnt = 32;
        }
        uint32_t b = bitBuf & 1;
        bitBuf >>= 1;
        bitCnt--;
        return b;
    }
    // First word: bit31 stays in the bit buffer, bits 0..30 give the initial code (bit0 is the code's MSB).
    void init(const uint8_t* start, const uint8_t* e) {
        p = start;
        end = e;
        uint32_t w = word();
        bitBuf = w >> 31;
        bitCnt = 1;
        uint32_t c = 0;
        for (int i = 0; i < 31; i++) c = (c << 1) | ((w >> i) & 1);
        code = c;
        low = 0;
        high = 0x7fffffff;
    }
    // Narrow the interval to [cum, cum+freq) of total and renormalize.
    void consume(uint32_t cum, uint32_t freq, uint32_t total) {
        uint32_t range = high - low + 1;
        uint32_t nh = (uint32_t)(((uint64_t)range * (cum + freq)) / total) - 1 + low;
        uint32_t nl = low + (uint32_t)(((uint64_t)range * cum) / total);
        while (((nl ^ nh) & 0x40000000) == 0) {
            nl = (nl << 1) & 0x7fffffff;
            nh = ((nh << 1) | 1) & 0x7fffffff;
            code = ((code << 1) | bit()) & 0x7fffffff;
        }
        while ((nl & 0x20000000) && !(nh & 0x20000000)) {
            nl = (nl & 0x1fffffff) << 1;
            nh = ((nh << 1) | 0x40000001) & 0x7fffffff;
            code = (((code ^ 0x20000000) << 1) | bit()) & 0x7fffffff;
        }
        low = nl & 0x7fffffff;
        high = nh & 0x7fffffff;
    }
    uint32_t uniform(uint32_t n) {
        uint64_t prod = (uint64_t)(code - low + 1) * n;
        uint32_t v = (uint32_t)((prod - 1) / (uint64_t)(high - low + 1));
        consume(v, 1, n);
        return v;
    }
};

// ---- adaptive model --------------------------------------------------------------------------------------
// Mirrors the original ushort s[] header: s[0..15] cumulative group counts (s[15] = total),
// s[0x10] = ranks in use, s[0x12] = grouped rank limit, s[0x13] = group shift, s[0x14] = group size,
// s[0x16] = alphabet size. freq[rank] (original s[0x1c+rank]), sym[rank] = symbol at that rank (rank 0 = escape).
struct Model {
    uint16_t s[0x1c] = {};
    std::vector<uint16_t> freq, sym;

    void layout(uint32_t n) {
        if (n < 6) {
            s[0x14] = 0;
            s[0x13] = 0xf;
            s[0x12] = 0;
            return;
        }
        uint32_t best = 0xffffffff, bestk = 0, k = 0, sz;
        do {
            sz = 1u << k;
            uint32_t c = (sz - 1 + n) / sz;
            if (c > 0x10) c = 0x10;
            uint32_t v = n - (c - 1) * sz;
            if (v < sz) v = sz;
            if (v < best) {
                bestk = k;
                best = v;
            }
            k++;
        } while (sz <= n && k < 0x10);
        s[0x14] = (uint16_t)(1u << bestk);
        s[0x13] = (uint16_t)bestk;
        s[0x12] = (uint16_t)(s[0x14] * 0xf);
    }

    void pairAdd(int i, uint32_t v) {
        uint32_t x = (uint32_t)s[2 * i] | ((uint32_t)s[2 * i + 1] << 16);
        x += v;
        s[2 * i] = (uint16_t)x;
        s[2 * i + 1] = (uint16_t)(x >> 16);
    }

    // add dd (both 16-bit halves) to rank r
    void add(uint32_t r, uint32_t dd) {
        uint16_t d = (uint16_t)dd;
        if (s[0x12] <= r) {
            s[0xf] += d;
            freq[r] += d;
            return;
        }
        uint32_t g = r >> s[0x13];
        if (g & 1) {
            s[g] += d;
            g++;
        }
        for (uint32_t i = g >> 1; i < 8; i++) pairAdd((int)i, dd);
        freq[r] += d;
    }

    void init(uint32_t n) {
        uint32_t cap = ((n + 5) & ~3u) + 4;
        std::memset(s, 0, sizeof s);
        freq.assign(cap, 0);
        sym.assign(cap, 0);
        s[0x16] = (uint16_t)n;
        layout(n + 1);
        add(0, 0x30003);
    }

    void rebuild(uint32_t* l) {
        if (s[0x10] != s[0x16] && freq[0] == 0) {
            freq[0] = 2;
            l[s[0x12] != 0 ? 0 : 15] += 2;
        }
        s[0] = (uint16_t)l[0];
        for (int i = 1; i < 16; i++) s[i] = (uint16_t)(s[i - 1] + (uint16_t)l[i]);
    }

    void rescale() {
        uint32_t l[16];
        uint32_t maxv = 0, maxi = 0;
        layout((uint32_t)s[0x10] + 1);
        std::memset(l, 0, sizeof l);
        freq[0] >>= 1;
        l[s[0x12] != 0 ? 0 : 15] = freq[0];
        if (s[0x10] != 0) {
            uint32_t idx = 1;
            bool early = false;
            do {
                uint16_t f = freq[idx];
                while (f < 2) {
                    if (s[0x10] <= idx) {
                        freq[idx] = 0;
                        s[0x10]--;
                        early = true;
                        break;
                    }
                    freq[idx] = freq[s[0x10]];
                    freq[s[0x10]] = 0;
                    sym[idx] = sym[s[0x10]];
                    s[0x10]--;
                    f = freq[idx];
                }
                if (early) break;
                freq[idx] = f >> 1;
                uint32_t u = freq[idx];
                if (maxv < u) {
                    maxi = idx;
                    maxv = u;
                }
                uint32_t g = idx < s[0x12] ? (idx >> s[0x13]) : 0xf;
                l[g] += u;
                idx++;
            } while (idx <= s[0x10]);
            if (maxv != 0) {
                uint32_t t;
                if (s[0x10] < s[0x12])
                    t = ((uint32_t)s[0x10] >> s[0x13]) << s[0x13];
                else
                    t = s[0x12];
                if (t == 0) t = 1;
                if (maxi != t) {
                    uint32_t fm = freq[maxi], ft = freq[t];
                    freq[t] = (uint16_t)fm;
                    uint32_t gt = t < s[0x12] ? (t >> s[0x13]) : 0xf;
                    l[gt] += fm - ft;
                    uint32_t gm = maxi < s[0x12] ? (maxi >> s[0x13]) : 0xf;
                    l[gm] += ft - fm;
                    freq[maxi] = (uint16_t)ft;
                    std::swap(sym[maxi], sym[t]);
                }
            }
        }
        rebuild(l);
    }

    // Locate the rank containing target, applying the deferred +1 tree updates. *cum = cumulative low.
    uint32_t find(uint32_t target, uint32_t* cum) {
        auto inc = [&](int from) {
            for (int i = from; i < 8; i++) pairAdd(i, 0x10001);
        };
        uint16_t t = (uint16_t)target;
        uint32_t sh = s[0x13];
        uint32_t rank, base;
        if (t < s[7]) {
            if (t < s[3]) {
                if (t < s[1]) {
                    if (t < s[0]) {
                        inc(0);
                        base = 0;
                        rank = 0;
                    } else {
                        base = s[0];
                        s[1]++;
                        inc(1);
                        rank = s[0x14];
                    }
                } else if (t < s[2]) {
                    base = s[1];
                    rank = 2u << sh;
                    inc(1);
                } else {
                    rank = 3u << sh;
                    base = s[2];
                    s[3]++;
                    inc(2);
                }
            } else {
                if (t < s[5]) {
                    if (t < s[4]) {
                        base = s[3];
                        rank = 4u << sh;
                        inc(2);
                    } else {
                        rank = 5u << sh;
                        inc(3);
                        base = s[4];
                        s[5]++;
                    }
                } else if (t < s[6]) {
                    base = s[5];
                    rank = 6u << sh;
                    inc(3);
                } else {
                    rank = 7u << sh;
                    inc(4);
                    base = s[6];
                    s[7]++;
                }
            }
        } else {
            if (t < s[0xb]) {
                if (t < s[9]) {
                    if (t < s[8]) {
                        base = s[7];
                        rank = 8u << sh;
                        inc(4);
                    } else {
                        rank = 9u << sh;
                        inc(5);
                        base = s[8];
                        s[9]++;
                    }
                } else if (t < s[10]) {
                    base = s[9];
                    rank = 10u << sh;
                    inc(5);
                } else {
                    rank = 0xbu << sh;
                    base = s[10];
                    s[0xb]++;
                    inc(6);
                }
            } else {
                if (t < s[0xd]) {
                    if (t < s[0xc]) {
                        base = s[0xb];
                        rank = 0xcu << sh;
                        inc(6);
                    } else {
                        rank = 0xdu << sh;
                        base = s[0xc];
                        s[0xd]++;
                        inc(7);
                    }
                } else if (t < s[0xe]) {
                    base = s[0xd];
                    rank = 0xeu << sh;
                    inc(7);
                } else {
                    rank = s[0x12];
                    base = s[0xe];
                    s[0xf]++;
                }
            }
        }
        uint32_t c = base;
        for (;;) {
            if (rank + 1 >= freq.size()) {  // corrupt-stream guard
                *cum = c;
                return rank < freq.size() ? rank : (uint32_t)freq.size() - 1;
            }
            uint32_t n0 = freq[rank] + c;
            if (target < n0) {
                *cum = c;
                return rank;
            }
            c = n0;
            rank++;
            n0 = freq[rank] + c;
            if (target < n0) {
                *cum = c;
                return rank;
            }
            c = n0;
            rank++;
        }
    }

    // Returns the symbol (< 0x10000) or 0x10000 + rank for an escape (caller then stores the new symbol at sym[rank]).
    uint32_t decode(Coder& cd) {
        if (s[0xf] > 0x3fff) rescale();
        uint64_t prod = (uint64_t)(cd.code - cd.low + 1) * s[0xf];
        uint32_t target = (uint32_t)((prod - 1) / (uint64_t)(cd.high - cd.low + 1));
        uint32_t cum = 0;
        uint32_t r = find(target, &cum);
        cd.consume(cum, freq[r], (uint32_t)s[0xf] - 1);
        freq[r]++;
        if (r == 0) {
            s[0x10]++;
            add(s[0x10], 0x20002);
            if (s[0x10] == s[0x16]) {
                uint32_t f0 = freq[0];
                add(0, ((uint32_t)(uint16_t)(0u - f0 - 1) << 16) | (uint16_t)(0u - f0));
            }
            return 0x10000 + s[0x10];
        }
        return sym[r];
    }
};

}  // namespace

bool oodle0Decompress(const uint8_t* src, size_t srcSize, uint32_t stop0, uint32_t stop1, uint32_t outSize,
                      uint8_t* out) {
    if (srcSize < 40) return false;
    uint32_t hdr[9];
    for (int i = 0; i < 9; i++)
        hdr[i] = src[i * 4] | (src[i * 4 + 1] << 8) | (src[i * 4 + 2] << 16) | ((uint32_t)src[i * 4 + 3] << 24);
    Coder cd;
    cd.init(src + 36, src + srcSize);
    const uint32_t stops[3] = {stop0, stop1, outSize};
    uint32_t pos = 0;
    static const uint32_t bigLen[4] = {0x80, 0xc0, 0x100, 0x200};
    for (int pass = 0; pass < 3; pass++) {
        uint32_t w0 = hdr[pass * 3], w1 = hdr[pass * 3 + 1], w2 = hdr[pass * 3 + 2];
        uint32_t nLit = w0 & 0x1ff, A = w0 >> 9;
        uint32_t B = A > 3 ? 4 : A;
        uint32_t distN = w1 >> 9;
        std::vector<Model> ctx(0x41);
        Model lit, mLow, mHigh;
        lit.init(w1 & 0x1ff);
        for (int g = 0; g < 4; g++) {
            uint32_t n = (w2 >> (24 - 8 * g)) & 0xff;
            for (int i = 0; i < 16; i++) ctx[g * 16 + i].init(n);
        }
        ctx[0x40].init(w2 & 0xff);
        mLow.init(B);
        mHigh.init(distN);
        uint32_t prev = 0, cnt = 0;  // prev = context (previous code), cnt = bytes produced in this pass
        while (pos < stops[pass]) {
            if (pos >= outSize) return false;
            uint32_t code = ctx[prev].decode(cd);
            if (code > 0x10000) {
                code = cd.uniform(0x41);
                ctx[prev].sym[ctx[prev].s[0x10]] = (uint16_t)code;
            }
            prev = code;
            if (prev > 0x40) return false;
            if (code == 0) {
                uint32_t v = lit.decode(cd);
                if (v > 0x10000) {
                    v = cd.uniform(nLit);
                    lit.sym[lit.s[0x10]] = (uint16_t)v;
                }
                out[pos++] = (uint8_t)v;
                cnt++;
            } else {
                uint32_t len = code < 0x3d ? code + 1 : bigLen[code - 0x3d];
                uint32_t lo = mLow.decode(cd);
                if (lo > 0x10000) {
                    lo = cd.uniform(B);
                    mLow.sym[mLow.s[0x10]] = (uint16_t)lo;
                }
                uint32_t hi = mHigh.decode(cd);
                if (hi > 0x10000) {
                    uint32_t m = A < cnt ? A : cnt;
                    hi = cd.uniform((m >> 2) + 1);
                    mHigh.sym[mHigh.s[0x10]] = (uint16_t)hi;
                }
                uint32_t dist = hi * 4 + lo + 1;
                if (dist > pos || pos + len > outSize) return false;
                cnt += len;
                for (uint32_t i = 0; i < len; i++, pos++) out[pos] = out[pos - dist];
            }
            if (cd.overrun && cd.p > cd.end + 16) return false;
        }
    }
    return true;
}

}  // namespace gr2
