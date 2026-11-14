#pragma once
// TGA decoder: types 1/2/3/9/10/11, 8/15/16/24/32 bpp, RLE, palette, honours the origin bit.
#include <cstdint>
#include <vector>

namespace f3::gd {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // w*h*4, top row first
    bool hasAlpha = false;      // source had a real alpha channel (32 bpp)
};

bool DecodeTga(const uint8_t* data, size_t size, Image& out);

}  // namespace f3::gd
