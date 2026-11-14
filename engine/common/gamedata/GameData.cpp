#include "GameData.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace f3::gd {

namespace {
uint32_t Rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    bool ok = n <= 0 || std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

std::string CStr(const std::vector<uint8_t>& d, size_t off) {
    if (off >= d.size()) return {};
    size_t e = off;
    while (e < d.size() && d[e]) ++e;
    return std::string(reinterpret_cast<const char*>(&d[off]), e - off);
}
}  // namespace

std::string ToLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

GameData::~GameData() {
    for (auto& g : grps_) if (g.f) std::fclose(g.f);
}

bool GameData::Open(const std::string& dir, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    dir_ = dir;
    std::string base = dir;
    if (!base.empty() && base.back() != '/' && base.back() != '\\') base += '/';
    std::vector<uint8_t> d;
    if (!ReadWholeFile(base + "resource.rht", d) || d.size() < 20) return fail("cannot read " + base + "resource.rht");
    if (Rd32(&d[0]) != 1 || Rd32(&d[4]) != 1) return fail("resource.rht: wrong version");
    uint32_t count = Rd32(&d[8]), grpBase = Rd32(&d[12]), nameBase = Rd32(&d[16]);
    if (20 + static_cast<uint64_t>(count) * 20 > d.size() || grpBase > d.size() || nameBase > d.size())
        return fail("resource.rht: corrupt");

    std::map<std::string, int> grpIds;
    res_.clear(); grps_.clear(); byName_.clear();
    res_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* r = &d[20 + static_cast<size_t>(i) * 20];
        ResourceInfo& x = res_[i];
        x.index = Rd32(r + 4);
        x.type = Rd32(r + 8);
        x.name = CStr(d, static_cast<size_t>(nameBase) + Rd32(r + 12));
        x.grp = CStr(d, static_cast<size_t>(grpBase) + Rd32(r + 16));
        auto it = grpIds.find(x.grp);
        if (it == grpIds.end()) {
            Grp g;
            g.name = x.grp;
            g.path = base + "data/" + x.grp + ".grp";
            FILE* f = std::fopen(g.path.c_str(), "rb");
            if (f) {
                uint8_t h[12];
                if (std::fread(h, 1, 12, f) == 12 && Rd32(h) == 2 && Rd32(h + 4) == 1) {
                    uint32_t n = Rd32(h + 8);
                    std::vector<uint8_t> tab(static_cast<size_t>(n) * 8);
                    if (n == 0 || std::fread(tab.data(), 1, tab.size(), f) == tab.size()) {
                        g.entries.resize(n);
                        for (uint32_t k = 0; k < n; ++k) g.entries[k] = {Rd32(&tab[k * 8]), Rd32(&tab[k * 8 + 4])};
                        g.f = f;
                        f = nullptr;
                    }
                }
                if (f) std::fclose(f);
            }
            it = grpIds.emplace(x.grp, static_cast<int>(grps_.size())).first;
            grps_.push_back(std::move(g));
        }
        x.grpId = it->second;
        const Grp& g = grps_[x.grpId];
        if (x.index < g.entries.size()) { x.offset = g.entries[x.index].first; x.size = g.entries[x.index].second; }
        else x.grpId = -1;  // grp missing / index out of range: unreadable
        byName_.emplace(ToLower(x.name), i);
    }
    return true;
}

const ResourceInfo* GameData::Find(const std::string& name, uint32_t type, const char* grpHint) const {
    auto search = [&](const std::string& key) -> const ResourceInfo* {
        auto range = byName_.equal_range(key);
        const ResourceInfo* best = nullptr;
        for (auto it = range.first; it != range.second; ++it) {
            const ResourceInfo& r = res_[it->second];
            if (r.type != type || r.grpId < 0) continue;
            if (grpHint && ToLower(r.grp) != ToLower(grpHint)) continue;
            if (!best || &r < best) best = &r;
        }
        return best;
    };
    std::string key = ToLower(name);
    if (const ResourceInfo* r = search(key)) return r;
    size_t dot = key.rfind('.');
    if (dot != std::string::npos && key.size() - dot <= 5) return search(key.substr(0, dot));
    return nullptr;
}

bool GameData::Read(const ResourceInfo& r, std::vector<uint8_t>& out) const {
    out.clear();
    if (r.grpId < 0) return false;
    std::lock_guard<std::mutex> lk(mu_);
    FILE* f = grps_[r.grpId].f;
    if (!f) return false;
    out.resize(r.size);
#ifdef _WIN32
    if (_fseeki64(f, r.offset, SEEK_SET) != 0) return false;
#else
    if (fseeko(f, r.offset, SEEK_SET) != 0) return false;
#endif
    return r.size == 0 || std::fread(out.data(), 1, out.size(), f) == out.size();
}

bool GameData::Read(const std::string& name, uint32_t type, std::vector<uint8_t>& out, const char* grpHint) const {
    const ResourceInfo* r = Find(name, type, grpHint);
    return r && Read(*r, out);
}

std::vector<const ResourceInfo*> GameData::List(uint32_t type) const {
    std::vector<const ResourceInfo*> v;
    for (const auto& r : res_) if (r.type == type && r.grpId >= 0) v.push_back(&r);
    return v;
}

}  // namespace f3::gd
