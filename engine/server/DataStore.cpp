#include "DataStore.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>
#include "common/log.h"

namespace f3 {

using nlohmann::json;
namespace fs = std::filesystem;

std::string DataStore::Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static bool ReadJson(const fs::path& p, json& out, std::string& err) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { err = "cannot open " + p.string(); return false; }
    try {
        out = json::parse(f);
    } catch (const std::exception& e) {
        err = p.string() + ": " + e.what();
        return false;
    }
    return true;
}

static PClass ClassFromString(const std::string& s) {
    if (s == "critter") return PClass::Critter;
    if (s == "weapon") return PClass::Weapon;
    if (s == "armor") return PClass::Armor;
    if (s == "ammo") return PClass::Ammo;
    if (s == "door") return PClass::Door;
    if (s == "container") return PClass::Container;
    if (s == "usable") return PClass::Usable;
    return PClass::Item;
}

static std::vector<std::string> StrArray(const json& j, const char* key) {
    std::vector<std::string> v;
    if (j.contains(key) && j[key].is_array())
        for (auto& e : j[key])
            if (e.is_string()) v.push_back(e.get<std::string>());
    return v;
}

static AttackModeDef ParseMode(const json& m) {
    AttackModeDef a;
    a.type = m.value("type", 0);
    a.ap = m.value("ap", 10);
    a.dmgMin = m.value("dmg_min", 0);
    a.dmgMax = m.value("dmg_max", 0);
    return a;
}

bool DataStore::Load(const fs::path& dir, std::string& err) {
    dir_ = dir;
    json manifest;
    if (!ReadJson(dir / "manifest.json", manifest, err)) return false;
    for (auto& m : manifest["maps"]) mapNames_.push_back(m.value("name", std::string()));
    startMap_ = manifest.value("start_map", std::string());
    startEntry_ = manifest.value("start_entry", 0);

    json protos;
    if (!ReadJson(dir / "protos.json", protos, err)) return false;
    // nlohmann::json objects are key-sorted (std::map), which defines the stable pid order.
    for (auto it = protos.begin(); it != protos.end(); ++it) {
        const json& j = it.value();
        Proto p;
        p.pid = static_cast<uint16_t>(protos_.size());
        p.key = it.key();
        p.cls = ClassFromString(j.value("class", std::string()));
        p.model = j.value("model", std::string());
        p.name = j.value("name", std::string());
        p.desc = j.value("desc", std::string());
        p.name2 = j.value("name2", std::string());
        p.nameId = j.value("name_id", 0);
        p.descId = j.value("desc_id", 0);
        p.script = j.value("script", std::string());
        p.dialog = j.value("dialog", std::string());
        p.team = j.value("team", 0);
        p.isStatic = j.value("static", false);
        p.inventory = StrArray(j, "inventory");
        if (j.contains("critter")) {
            const json& c = j["critter"];
            if (c.contains("special") && c["special"].size() == 7)
                for (int i = 0; i < 7; i++) p.special[i] = c["special"][i].get<int>();
            if (c.contains("derived_overrides"))
                for (auto& d : c["derived_overrides"]) p.derived.emplace_back(d[0].get<int>(), d[1].get<int>());
            if (c.contains("attack_modes"))
                for (auto& m : c["attack_modes"]) p.modes.push_back(ParseMode(m));
            p.equipment = StrArray(c, "equipment");
        }
        if (j.contains("weapon")) {
            const json& w = j["weapon"];
            p.weaponClass = w.value("weapon_class", 0);
            p.caliber = w.value("caliber", -1);
            p.range = std::max(1, w.value("range", 1));
            p.wDmgMin = w.value("dmg_min", 0.0f);
            p.wDmgMax = w.value("dmg_max", 0.0f);
            if (w.contains("attack_modes"))
                for (auto& m : w["attack_modes"]) p.modes.push_back(ParseMode(m));
        }
        if (j.contains("armor") && j["armor"].contains("values")) {
            const json& v = j["armor"]["values"];
            for (size_t i = 0; i < 7 && i < v.size(); i++) p.armor[i] = v[i].get<int>();
        }
        if (j.contains("ammo")) {
            const json& a = j["ammo"];
            p.ammoCaliber = a.value("caliber", -1);
            p.ammoDmgMin = a.value("dmg_min", 0);
            p.ammoDmgMax = a.value("dmg_max", 0);
        }
        switch (p.cls) {  // no weights in the decoded data yet: plausible defaults
            case PClass::Weapon: p.weight = 4; break;
            case PClass::Armor: p.weight = 8; break;
            default: p.weight = 1; break;
        }
        byKey_[Lower(p.key)] = p.pid;
        protos_.push_back(std::move(p));
    }

    // English.stf as id<TAB>text
    {
        std::ifstream f(dir / "strings.tsv", std::ios::binary);
        std::string line;
        while (std::getline(f, line)) {
            size_t t = line.find('\t');
            if (t == std::string::npos) continue;
            strings_[std::atoi(line.substr(0, t).c_str())] = line.substr(t + 1);
        }
    }
    // Scripts/globals.ini: "Name=value ;comment"
    {
        std::ifstream f(dir / "scripts" / "globals.ini");
        std::string line;
        while (std::getline(f, line)) {
            size_t sc = line.find(';');
            if (sc != std::string::npos) line.resize(sc);
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto trim = [](std::string s) {
                size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
                return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
            };
            std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
            if (!k.empty() && !v.empty()) globals_[k] = std::atof(v.c_str());
        }
    }
    loaded_ = true;
    log::Info("data: %zu prototypes, %zu maps, %zu globals, %zu strings from %s", protos_.size(), mapNames_.size(),
              globals_.size(), strings_.size(), dir.string().c_str());
    return true;
}

const Proto* DataStore::FindProto(const std::string& key) const {
    auto it = byKey_.find(Lower(key));
    return it == byKey_.end() ? nullptr : &protos_[it->second];
}

std::string DataStore::String(int id) const {
    auto it = strings_.find(id);
    return it == strings_.end() ? std::string() : it->second;
}

bool DataStore::LoadMap(const std::string& name, MapDef& out, std::string& err) const {
    json j;
    if (!ReadJson(dir_ / "maps" / (name + ".json"), j, err)) return false;
    out = MapDef{};
    out.name = j.value("name", name);
    out.width = j.value("width", 0);
    out.height = j.value("height", 0);
    if (out.width <= 0 || out.height <= 0 || static_cast<long long>(out.width) * out.height > 4000000) { err = "bad map size"; return false; }
    {
        fs::path wp = dir_ / "maps" / j.value("walk", name + ".walk");
        std::ifstream f(wp, std::ios::binary);
        char hdr[12];
        if (!f.read(hdr, 12) || std::memcmp(hdr, "WALK", 4) != 0) { err = "bad walk file " + wp.string(); return false; }
        uint32_t w, h;
        std::memcpy(&w, hdr + 4, 4);
        std::memcpy(&h, hdr + 8, 4);
        if (static_cast<int>(w) != out.width || static_cast<int>(h) != out.height) { err = "walk size mismatch"; return false; }
        out.walk.resize(static_cast<size_t>(w) * h);
        if (!f.read(reinterpret_cast<char*>(out.walk.data()), static_cast<std::streamsize>(out.walk.size()))) { err = "short walk file"; return false; }
    }
    for (auto& e : j["entry_points"]) {
        EntryPointDef ep;
        ep.id = e.value("id", 0);
        for (auto& s : e["slots"]) ep.slots.push_back({s.value("x", 0.f), s.value("z", 0.f), s.value("yaw", 0.f)});
        out.entries.push_back(std::move(ep));
    }
    for (auto& r : j["regions"]) {
        RegionDef rd;
        rd.kind = r.value("kind", 0);
        rd.y = r.value("y", 0.f);
        rd.script = r.value("script", std::string());
        rd.targetMap = r.value("target_map", std::string());
        rd.targetEntry = r.value("target_entry", 0);
        for (auto& p : r["pts"]) rd.pts.emplace_back(p[0].get<float>(), p[1].get<float>());
        out.regions.push_back(std::move(rd));
    }
    for (auto& e : j["entities"]) {
        EntityDef ed;
        ed.name = e.value("name", std::string());
        ed.proto = e.value("proto", std::string());
        ed.type = e.value("type", std::string());
        ed.script = e.value("script", std::string());
        ed.dialog = e.value("dialog", std::string());
        ed.skin = e.value("skin", std::string());
        ed.x = e.value("x", 0.f); ed.y = e.value("y", 0.f); ed.z = e.value("z", 0.f); ed.yaw = e.value("yaw", 0.f);
        ed.team = e.value("team", 0);
        ed.inventory = StrArray(e, "inventory");
        out.entities.push_back(std::move(ed));
    }
    for (auto& p : j["paths"]) {
        std::vector<std::pair<float, float>> pts;
        for (auto& q : p["pts"]) pts.emplace_back(q[0].get<float>(), q[1].get<float>());
        out.paths.push_back(std::move(pts));
    }
    return true;
}

}  // namespace f3
