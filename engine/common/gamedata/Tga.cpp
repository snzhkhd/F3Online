#include "Tga.h"

#include <cstring>

namespace f3::gd {

bool DecodeTga(const uint8_t* d, size_t size, Image& out) {
    if (size < 18) return false;
    const uint8_t idLen = d[0], cmType = d[1], it = d[2];
    uint16_t cmStart, cmLen, w, h;
    std::memcpy(&cmStart, d + 3, 2); std::memcpy(&cmLen, d + 5, 2);
    std::memcpy(&w, d + 12, 2); std::memcpy(&h, d + 14, 2);
    const uint8_t cmBits = d[7], bpp = d[16], desc = d[17];
    if (!(it == 1 || it == 2 || it == 3 || it == 9 || it == 10 || it == 11)) return false;
    if (!w || !h) return false;
    if (!(bpp == 8 || bpp == 15 || bpp == 16 || bpp == 24 || bpp == 32)) return false;
    size_t pos = 18 + idLen;
    const int cmBytes = (cmBits + 7) / 8;
    std::vector<uint8_t> pal;
    if (cmType) {
        size_t pend = pos + static_cast<size_t>(cmLen) * cmBytes;
        if (pend > size) return false;
        pal.assign(d + pos, d + pend);
        pos = pend;
    }
    const int Bpp = (bpp + 7) / 8;
    std::vector<uint8_t> raw(static_cast<size_t>(w) * h * Bpp);
    if (it < 9) {
        if (pos + raw.size() > size) return false;
        std::memcpy(raw.data(), d + pos, raw.size());
    } else {
        size_t o = 0;
        while (o < raw.size()) {
            if (pos >= size) return false;
            uint8_t hd = d[pos++];
            int cnt = (hd & 0x7f) + 1;
            if (hd & 0x80) {
                if (pos + Bpp > size) return false;
                for (int i = 0; i < cnt && o < raw.size(); ++i) { std::memcpy(&raw[o], d + pos, Bpp); o += Bpp; }
                pos += Bpp;
            } else {
                size_t nb = static_cast<size_t>(cnt) * Bpp;
                if (pos + nb > size) return false;
                size_t c = nb < raw.size() - o ? nb : raw.size() - o;
                std::memcpy(&raw[o], d + pos, c);
                o += c; pos += nb;
            }
        }
    }
    out.w = w; out.h = h;
    out.hasAlpha = (bpp == 32);
    out.rgba.assign(static_cast<size_t>(w) * h * 4, 255);
    const bool topFirst = (desc & 0x20) != 0;
    const bool rightFirst = (desc & 0x10) != 0;
    for (int y = 0; y < h; ++y) {
        int sy = topFirst ? y : h - 1 - y;
        for (int x = 0; x < w; ++x) {
            int sx = rightFirst ? w - 1 - x : x;
            const uint8_t* s = &raw[(static_cast<size_t>(sy) * w + sx) * Bpp];
            uint8_t* o = &out.rgba[(static_cast<size_t>(y) * w + x) * 4];
            if (it == 1 || it == 9) {
                size_t k = static_cast<size_t>(s[0] - cmStart) * cmBytes;
                if (k + cmBytes <= pal.size()) {
                    o[0] = pal[k + 2]; o[1] = pal[k + 1]; o[2] = pal[k];
                    if (cmBytes == 4) o[3] = pal[k + 3];
                }
            } else if (it == 3 || it == 11) {
                o[0] = o[1] = o[2] = s[0];
            } else if (bpp == 24) {
                o[0] = s[2]; o[1] = s[1]; o[2] = s[0];
            } else if (bpp == 32) {
                o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = s[3];
            } else {
                uint16_t v = static_cast<uint16_t>(s[0] | (s[1] << 8));
                o[0] = static_cast<uint8_t>(((v >> 10) & 31) * 255 / 31);
                o[1] = static_cast<uint8_t>(((v >> 5) & 31) * 255 / 31);
                o[2] = static_cast<uint8_t>((v & 31) * 255 / 31);
            }
        }
    }
    return true;
}

}  // namespace f3::gd
