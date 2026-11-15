#include "Stf.h"
#include <cstdio>
#include <cstring>

namespace f3::gd {

bool Stf::Load(const std::string& path) {
    ok_ = false;
    data_.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 12 || n > (64 << 20)) { std::fclose(f); return false; }
    data_.resize(static_cast<size_t>(n));
    const bool read = std::fread(data_.data(), 1, data_.size(), f) == data_.size();
    std::fclose(f);
    uint32_t v = 0;
    if (!read) return false;
    std::memcpy(&v, data_.data(), 4);
    std::memcpy(&count_, data_.data() + 8, 4);
    if (v != 3 || 12 + 16ull * count_ > data_.size()) return false;
    return ok_ = true;
}

std::string Stf::Get(uint32_t id) const {
    if (!ok_ || id == 0 || id > count_) return {};
    uint32_t off, len;
    const size_t rec = 12 + 16ull * (id - 1);
    std::memcpy(&off, data_.data() + rec, 4);
    std::memcpy(&len, data_.data() + rec + 4, 4);
    if (static_cast<uint64_t>(off) + len > data_.size()) return {};
    std::string out;
    for (uint32_t i = 0; i < len; ++i) {
        const unsigned char ch = data_[off + i];
        if (ch == 0) break;
        if (ch < 0x80) out.push_back(static_cast<char>(ch));
        else { out.push_back(static_cast<char>(0xC0 | (ch >> 6))); out.push_back(static_cast<char>(0x80 | (ch & 0x3F))); }
    }
    return out;
}

}  // namespace f3::gd
