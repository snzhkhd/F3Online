// GR2 container: header, section table, Oodle0 decompression, relocations.
#include <cstdio>
#include <cstring>

#include "gr2.h"

namespace gr2 {

static uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static const uint8_t kMagic[16] = {0xb8, 0x67, 0xb0, 0xca, 0xf8, 0x6d, 0xb1, 0x0f,
                                   0x84, 0x72, 0x8c, 0x7e, 0x5e, 0x19, 0x00, 0x1e};

bool File::load(const uint8_t* data, size_t size, std::string* err) {
    auto fail = [&](const char* m) {
        if (err) *err = m;
        return false;
    };
    raw.assign(data, data + size);
    if (size < 0x58 || std::memcmp(data, kMagic, 16) != 0) return fail("bad magic");
    uint32_t headerSize = rd32(data + 0x10);
    version = rd32(data + 0x20);
    fileSize = rd32(data + 0x24);
    crc = rd32(data + 0x28);
    uint32_t secArr = rd32(data + 0x2c);
    uint32_t secCount = rd32(data + 0x30);
    uint32_t rtSec = rd32(data + 0x34), rtOff = rd32(data + 0x38);
    uint32_t roSec = rd32(data + 0x3c), roOff = rd32(data + 0x40);
    tag = rd32(data + 0x44);
    if (version != 6) return fail("unsupported GR2 version (only 6 supported)");
    if (secCount == 0 || secCount > 32) return fail("bad section count");
    size_t shOff = 0x20 + (size_t)secArr;
    if (shOff + (size_t)secCount * 44 > size || headerSize > size) return fail("truncated header");
    sections.assign(secCount, SectionInfo());
    for (uint32_t i = 0; i < secCount; i++) {
        const uint8_t* p = data + shOff + i * 44;
        SectionInfo& s = sections[i];
        s.compression = rd32(p);
        s.dataOffset = rd32(p + 4);
        s.compressedSize = rd32(p + 8);
        s.decompressedSize = rd32(p + 12);
        s.alignment = rd32(p + 16);
        s.first16bit = rd32(p + 20);
        s.first8bit = rd32(p + 24);
        s.relocOffset = rd32(p + 28);
        s.relocCount = rd32(p + 32);
        s.marshallOffset = rd32(p + 36);
        s.marshallCount = rd32(p + 40);
    }
    // flat layout
    mem.assign(16, 0);
    for (auto& s : sections) {
        while (mem.size() % 16) mem.push_back(0);
        s.base = (uint32_t)mem.size();
        mem.resize(mem.size() + s.decompressedSize, 0);
    }
    for (uint32_t i = 0; i < secCount; i++) {
        SectionInfo& s = sections[i];
        if (s.decompressedSize == 0) continue;
        if ((uint64_t)s.dataOffset + s.compressedSize > size) return fail("section data out of file");
        uint8_t* dst = &mem[s.base];
        if (s.compression == 0) {
            if (s.compressedSize < s.decompressedSize) return fail("short raw section");
            std::memcpy(dst, data + s.dataOffset, s.decompressedSize);
        } else if (s.compression == 1) {
            if (!oodle0Decompress(data + s.dataOffset, s.compressedSize, s.first16bit, s.first8bit,
                                  s.decompressedSize, dst))
                return fail("Oodle0 decompression failed");
        } else {
            return fail("unsupported section compression");
        }
    }
    // relocations
    for (uint32_t i = 0; i < secCount; i++) {
        SectionInfo& s = sections[i];
        if (s.relocCount == 0) continue;
        if ((uint64_t)s.relocOffset + (uint64_t)s.relocCount * 12 > size) return fail("relocations out of file");
        for (uint32_t k = 0; k < s.relocCount; k++) {
            const uint8_t* p = data + s.relocOffset + k * 12;
            uint32_t off = rd32(p), ts = rd32(p + 4), to = rd32(p + 8);
            if (ts >= secCount || (uint64_t)off + 4 > s.decompressedSize) return fail("bad relocation");
            uint32_t v = sections[ts].base + to;
            std::memcpy(&mem[s.base + off], &v, 4);
        }
    }
    if (rtSec >= secCount || roSec >= secCount) return fail("bad root");
    rootType = sections[rtSec].base + rtOff;
    rootObject = sections[roSec].base + roOff;
    return true;
}

bool File::loadFile(const std::string& path, std::string* err) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> buf;
    uint8_t tmp[65536];
    size_t n;
    while ((n = std::fread(tmp, 1, sizeof tmp, fp)) > 0) buf.insert(buf.end(), tmp, tmp + n);
    std::fclose(fp);
    return load(buf.data(), buf.size(), err);
}

uint32_t File::u32(uint32_t off) const {
    if (!valid(off, 4)) return 0;
    return rd32(&mem[off]);
}
float File::f32(uint32_t off) const {
    uint32_t v = u32(off);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
uint16_t File::u16(uint32_t off) const { return valid(off, 2) ? (uint16_t)(mem[off] | (mem[off + 1] << 8)) : 0; }
uint8_t File::u8(uint32_t off) const { return valid(off, 1) ? mem[off] : 0; }
std::string File::cstr(uint32_t off) const {
    std::string s;
    if (!valid(off)) return s;
    while (off < mem.size() && mem[off]) s.push_back((char)mem[off++]);
    return s;
}

bool File::decompressSection(int idx, std::vector<uint8_t>& out) const {
    if (idx < 0 || idx >= (int)sections.size()) return false;
    const SectionInfo& s = sections[idx];
    out.assign(s.decompressedSize, 0);
    if (s.decompressedSize == 0) return true;
    if ((uint64_t)s.dataOffset + s.compressedSize > raw.size()) return false;
    const uint8_t* src = raw.data() + s.dataOffset;
    if (s.compression == 0) {
        if (s.compressedSize < s.decompressedSize) return false;
        std::memcpy(out.data(), src, s.decompressedSize);
        return true;
    }
    if (s.compression == 1)
        return oodle0Decompress(src, s.compressedSize, s.first16bit, s.first8bit, s.decompressedSize, out.data());
    return false;
}

}  // namespace gr2
