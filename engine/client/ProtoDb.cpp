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

bool IsPartName(const std::string& n) {
    static const char* kPrefix[] = {"Hea", "Bod", "Han", "Fee", "Sho", "Bac", "Eye", "Mus", "Hai", "Bea", "Pon", "Van"};
    if (n.size() < 6) return false;
    bool ok = false;
    for (const char* p : kPrefix) if (n.compare(0, 3, p) == 0) ok = true;
    if (!ok) return false;
    for (char c : n) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    return true;
}

// Armor items store the worn parts as length-prefixed strings inside their opaque item record ("raw_hex"):
// `u16 len, name` for the part and optionally another one ending in "_LG" for its texture.
std::vector<WornPart> ParseArmorParts(const std::string& hex) {
    std::vector<uint8_t> b;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) b.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    std::vector<WornPart> out;
    for (size_t i = 0; i + 2 < b.size();) {
        const size_t len = b[i] | (static_cast<size_t>(b[i + 1]) << 8);
        if (len >= 6 && len <= 48 && i + 2 + len <= b.size()) {
            std::string s(reinterpret_cast<const char*>(&b[i + 2]), len);
            if (IsPartName(s)) {
                const bool isTex = s.size() > 3 && Lower(s.substr(s.size() - 3)) == "_lg";
                if (isTex && !out.empty() && out.back().tex.empty()) out.back().tex = s;
                else if (!isTex) out.push_back(WornPart{s, ""});
                i += 2 + len;
                continue;
            }
        }
        ++i;
    }
    return out;
}

// Items end with `heldModel, heldTexture` (u16 len + chars, see vbdecomp GameItem::ReadVersion0). We only need the model: the last
// printable length-prefixed string that looks like a model name ("WP_223Minigun"); some records carry earlier fillers ("minigun").
std::string ParseHeldModel(const std::string& hex) {
    std::vector<uint8_t> b;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) b.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    std::string best;
    for (size_t i = 0; i + 2 < b.size();) {
        const size_t len = b[i] | (static_cast<size_t>(b[i + 1]) << 8);
        if (len >= 4 && len <= 96 && i + 2 + len <= b.size()) {
            bool ok = true;
            for (size_t k = 0; k < len; ++k) { const uint8_t ch = b[i + 2 + k]; if (!std::isalnum(ch) && ch != '_') ok = false; }
            if (ok) {
                std::string s(reinterpret_cast<const char*>(&b[i + 2]), len);
                if (Lower(s.substr(0, 3)) == "wp_") best = s;
                i += 2 + len;
                continue;
            }
        }
        ++i;
    }
    return best;
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
        std::map<uint32_t, std::vector<std::string>> equip;   // critter pid -> equipment keys
        std::map<std::string, std::vector<WornPart>> armorParts;
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
            p.icon = v.value("icon", "");
            p.iconAct = v.value("icon_ground", "");
            p.desc = v.value("desc", "");
            if (p.cls == "armor") {
                std::string lk = p.key;
                std::transform(lk.begin(), lk.end(), lk.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                p.headArmor = lk.find("helmet") != std::string::npos;
                if (v.contains("armor") && v["armor"].contains("values"))
                    for (size_t i = 0; i < 7 && i < v["armor"]["values"].size(); ++i) p.armor[i] = v["armor"]["values"][i].get<int>();
            }
            if (p.cls == "weapon" && v.contains("weapon")) {
                const nlohmann::json& w = v["weapon"];
                p.range = w.value("range", 0);
                p.weaponClass = w.value("weapon_class", 0);
                p.dmgMin = w.value("dmg_min", 0.0f);
                p.dmgMax = w.value("dmg_max", 0.0f);
                if (w.contains("attack_modes"))
                    for (const auto& m : w["attack_modes"]) p.modes.push_back({m.value("type", 0), m.value("ap", 0)});
            }
            p.hasDialog = v.contains("dialog") && v["dialog"].is_string() && !v["dialog"].get<std::string>().empty();
            uint32_t pid = static_cast<uint32_t>(protos_.size());
            byKey_[Lower(p.key)] = pid;
            if (p.isCritter() && !p.isStatic && !p.name.empty() && !p.model.empty()) {
                modelVotes[Lower(p.name)][p.model]++;
                firstOf[Lower(p.name)].emplace(p.model, pid);
            } else if (p.isCritter() && p.isStatic && !p.name.empty()) {
                byName_.emplace(Lower(p.name), pid);  // static props only claim a name nobody else has
            }
            if (p.isCritter()) {
                // sockets: "socket<N>": "Part|Texture" (N orders them); equipped armor overrides a socket of the same slot
                const nlohmann::json& cj = v.contains("critter") ? v["critter"] : nlohmann::json::object();
                if (cj.contains("sockets") && cj["sockets"].is_object()) {
                    std::map<int, WornPart> byN;
                    for (auto sk = cj["sockets"].begin(); sk != cj["sockets"].end(); ++sk) {
                        const std::string val = sk.value().is_string() ? sk.value().get<std::string>() : "";
                        const size_t bar = val.find('|');
                        WornPart w{val.substr(0, bar), bar == std::string::npos ? "" : val.substr(bar + 1)};
                        if (w.part.empty()) continue;
                        byN[std::atoi(sk.key().c_str() + std::min<size_t>(6, sk.key().size()))] = std::move(w);
                    }
                    for (auto& [n, w] : byN) p.worn.push_back(std::move(w));
                }
                if (cj.contains("equipment") && cj["equipment"].is_array())
                    for (const auto& e : cj["equipment"]) equip[protos_.size()].push_back(e.is_string() ? e.get<std::string>() : "");
            }
            if (p.cls == "armor" && v.contains("item") && v["item"].contains("raw_hex") && v["item"]["raw_hex"].is_string())
                armorParts[p.key] = p.armorParts = ParseArmorParts(v["item"]["raw_hex"].get<std::string>());
            if (p.cls == "weapon" && v.contains("item") && v["item"].contains("raw_hex") && v["item"]["raw_hex"].is_string())
                p.heldModel = ParseHeldModel(v["item"]["raw_hex"].get<std::string>());
            protos_.push_back(std::move(p));
        }
        for (auto& [pid, keys] : equip) {
            for (const std::string& k : keys) {
                auto it = armorParts.find(k);
                if (it == armorParts.end()) continue;
                for (const WornPart& w : it->second) {
                    auto& worn = protos_[pid].worn;
                    worn.erase(std::remove_if(worn.begin(), worn.end(), [&](const WornPart& o) { return o.part.compare(0, 3, w.part, 0, 3) == 0; }), worn.end());
                    worn.push_back(w);
                }
            }
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
