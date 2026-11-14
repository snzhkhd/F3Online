#pragma once
// Little-endian, bounds-checked serialization buffer. Reads past the end set a
// sticky failure flag and return zero values, so packet parsers can read
// everything and check Ok() once. Host is assumed little-endian (x86/x64/ARM Windows).
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace f3 {

class ByteBuffer {
public:
    ByteBuffer() = default;
    ByteBuffer(const uint8_t* data, size_t size) : data_(data, data + size) {}

    void WriteU8(uint8_t v) { data_.push_back(v); }
    void WriteU16(uint16_t v) { WriteRaw(&v, 2); }
    void WriteU32(uint32_t v) { WriteRaw(&v, 4); }
    void WriteI16(int16_t v) { WriteRaw(&v, 2); }
    void WriteString(const std::string& s) {
        uint16_t n = static_cast<uint16_t>(s.size() > 0xFFFF ? 0xFFFF : s.size());
        WriteU16(n);
        data_.insert(data_.end(), s.begin(), s.begin() + n);
    }
    void WriteBytes(const uint8_t* p, size_t n) { data_.insert(data_.end(), p, p + n); }

    uint8_t ReadU8() { uint8_t v = 0; ReadRaw(&v, 1); return v; }
    uint16_t ReadU16() { uint16_t v = 0; ReadRaw(&v, 2); return v; }
    uint32_t ReadU32() { uint32_t v = 0; ReadRaw(&v, 4); return v; }
    int16_t ReadI16() { int16_t v = 0; ReadRaw(&v, 2); return v; }
    std::string ReadString(size_t maxLen = 0xFFFF) {
        uint16_t n = ReadU16();
        if (fail_ || n > maxLen || n > Remaining()) { fail_ = true; return {}; }
        std::string s(reinterpret_cast<const char*>(data_.data() + rpos_), n);
        rpos_ += n;
        return s;
    }
    bool ReadBytes(uint8_t* out, size_t n) { return ReadRaw(out, n); }

    bool Ok() const { return !fail_; }
    size_t Remaining() const { return data_.size() - rpos_; }
    const uint8_t* Data() const { return data_.data(); }
    size_t Size() const { return data_.size(); }

private:
    void WriteRaw(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        data_.insert(data_.end(), b, b + n);
    }
    bool ReadRaw(void* out, size_t n) {
        if (fail_ || Remaining() < n) { fail_ = true; return false; }
        std::memcpy(out, data_.data() + rpos_, n);
        rpos_ += n;
        return true;
    }
    std::vector<uint8_t> data_;
    size_t rpos_ = 0;
    bool fail_ = false;
};

}  // namespace f3
