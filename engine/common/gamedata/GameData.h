#pragma once
// Reader for the original Van Buren tech-demo install: resource.rht (name table) + data\*.grp
// (raw blob archives). See docs/formats/containers.md. No dependencies beyond the STL.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace f3::gd {

// Resource type ids from resource.rht.
enum ResType : uint32_t {
    kResB3d = 100, kResTga = 200, kResScriptA = 300, kResTre = 400, kResMeshGr2 = 600, kResAnimGr2 = 700,
    kResIni = 800, kResWav = 1100, kResScriptB = 1200, kResEmap = 1300, kResCrt = 1400, kResVeg = 1500,
    kResGui = 1600, kResMapData = 1700, kResItm = 1800, kResWea = 2400, kResArm = 2500, kResDor = 2600,
    kResUse = 2700, kResAmo = 3500, kResCon = 3600,
};

struct ResourceInfo {
    std::string name, grp;
    uint32_t type = 0, index = 0, offset = 0, size = 0;
    int grpId = -1;
};

std::string ToLower(std::string s);

class GameData {
public:
    GameData() = default;
    ~GameData();
    GameData(const GameData&) = delete;
    GameData& operator=(const GameData&) = delete;

    // dir = original install directory containing resource.rht and data\. Returns false and fills err.
    bool Open(const std::string& dir, std::string* err = nullptr);
    bool IsOpen() const { return !res_.empty(); }
    const std::string& dir() const { return dir_; }

    // Case-insensitive lookup. If `name` is not found verbatim, a trailing extension (.tga, .dds, .ctx, ...)
    // is stripped and the lookup retried. grpHint (e.g. "Maps") restricts the search when non-null.
    const ResourceInfo* Find(const std::string& name, uint32_t type, const char* grpHint = nullptr) const;
    bool Read(const ResourceInfo& r, std::vector<uint8_t>& out) const;
    bool Read(const std::string& name, uint32_t type, std::vector<uint8_t>& out, const char* grpHint = nullptr) const;

    const std::vector<ResourceInfo>& resources() const { return res_; }
    // All resources of one type (e.g. kResTre) - used to enumerate maps.
    std::vector<const ResourceInfo*> List(uint32_t type) const;

private:
    struct Grp {
        std::string name, path;
        FILE* f = nullptr;
        std::vector<std::pair<uint32_t, uint32_t>> entries;
    };
    std::string dir_;
    std::vector<ResourceInfo> res_;
    std::vector<Grp> grps_;
    std::unordered_multimap<std::string, uint32_t> byName_;
    mutable std::mutex mu_;
};

}  // namespace f3::gd
