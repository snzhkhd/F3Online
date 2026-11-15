#pragma once
// English.stf string table of the original game (id -> text). Format: u32 version (3), u32, u32 count, then count records of
// 16 bytes {u32 offset, u32 length, ...} (ids are 1-based), text bytes (Latin-1) at the offsets. Used for the UI texts the .gui
// layouts reference by id (Label::stringReference, tooltip title / body ids).
#include <cstdint>
#include <string>
#include <vector>

namespace f3::gd {

class Stf {
public:
    bool Load(const std::string& path);
    bool ok() const { return ok_; }
    uint32_t count() const { return count_; }
    // text of an id as UTF-8 (the file is Latin-1); empty for 0 / 0xFFFFFFFF / unknown ids
    std::string Get(uint32_t id) const;

private:
    std::vector<uint8_t> data_;
    uint32_t count_ = 0;
    bool ok_ = false;
};

}  // namespace f3::gd
