#include "ProtoDb.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include "Settings.h"
#include "common/log.h"

namespace f3 {

namespace {
std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool FileExists(const std::string& p) { std::ifstream f(p, std::ios::binary); return f.good(); }
}  // namespace

std::string ProtoDb::FindFile(const std::string& hint) {
    std::vector<std::string> cands;
    if (!hint.empty()) {
        cands.push_back(hint);
        cands.push_back(hint + "/protos.json");
        cands.push_back(hint + "/gamedata/server/protos.json");
    }
    for (const std::string& base : SearchRoots())
        cands.push_back(base + "/gamedata/server/protos.json");
    for (const auto& c : cands)
        if (FileExists(c) && c.size() > 5 && c.substr(c.size() - 5) == ".json") return c;
    return {};
}

bool ProtoDb::Load(const std::string& hint, std::string* err) {
    protos_.clear(); byKey_.clear(); byName_.clear();
    path_ = FindFile(hint);
    if (path_.empty()) { if (err) *err = "gamedata/server/protos.json not found"; return false; }
    try {
        std::ifstream f(path_, std::ios::binary);
        nlohmann::json j = nlohmann::json::parse(f);
        std::map<std::string, std::map<std::string, int>> modelVotes;  // name -> model -> count
        std::unordered_map<std::string, std::unordered_map<std::string, uint32_t>> firstOf;  // name -> model -> first pid
        for (auto it = j.begin(); it != j.end(); ++it) {
            const nlohmann::json& v = it.value();
            ProtoInfo p;
            p.key = it.key();
            p.cls = v.value("class", "");
            p.model = v.value("model", "");
            p.name = v.value("name", "");
            p.skin = v.value("skin", "");
            p.isStatic = v.value("static", false);
            uint32_t pid = static_cast<uint32_t>(protos_.size());
            byKey_[Lower(p.key)] = pid;
            if (p.isCritter() && !p.isStatic && !p.name.empty() && !p.model.empty()) {
                modelVotes[Lower(p.name)][p.model]++;
                firstOf[Lower(p.name)].emplace(p.model, pid);
            } else if (p.isCritter() && p.isStatic && !p.name.empty()) {
                byName_.emplace(Lower(p.name), pid);  // static props only claim a name nobody else has
            }
            protos_.push_back(std::move(p));
        }
        for (auto& [name, votes] : modelVotes) {
            std::string best; int bc = -1;
            for (auto& [m, c] : votes) if (c > bc) { bc = c; best = m; }
            byName_[name] = firstOf[name][best];
        }
    } catch (const std::exception& e) {
        if (err) *err = std::string("protos.json: ") + e.what();
        protos_.clear();
        return false;
    }
    log::Info("protos: %zu prototypes from %s", protos_.size(), path_.c_str());
    return true;
}

const ProtoInfo* ProtoDb::ByKey(const std::string& key) const {
    auto it = byKey_.find(Lower(key));
    return it == byKey_.end() ? nullptr : &protos_[it->second];
}

const ProtoInfo* ProtoDb::CritterByName(const std::string& name) const {
    auto it = byName_.find(Lower(name));
    return it == byName_.end() ? nullptr : &protos_[it->second];
}

}  // namespace f3
