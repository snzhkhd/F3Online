// Server side of the data-driven content layer (data/*.json): loading, /reload, stat modifiers of traits and perks, the perk
// spend flow, quest definitions, the client sync and the dev chat commands. Loader / validation: Content.cpp.
#include <algorithm>
#include <bitset>
#include <sstream>
#include "Scripting.h"
#include "Server.h"
#include "common/log.h"

namespace f3 {

namespace {
std::string Lc(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
}  // namespace

int Server::TraitMaxEff() const { return content_.hasTraitMax ? content_.traitMax : rt_.TraitMax; }

int Server::Mod(const Critter& c, int stat) const {
    if (!c.IsPlayer()) return 0;
    int sum = 0;
    if (c.traitMask)
        for (const TraitDef& t : content_.traits)
            if ((c.traitMask >> t.index) & 1u)
                for (const StatMod& e : t.effects)
                    if (e.stat == stat) sum += e.add;
    for (const auto& [id, rank] : c.perks)
        if (const PerkDef* p = content_.FindPerk(id))
            for (const StatMod& e : p->effects)
                if (e.stat == stat) sum += e.add * rank;
    return sum;
}

int Server::PerkRank(const Critter& c, const std::string& id) const {
    const PerkDef* p = content_.FindPerk(id);
    auto it = c.perks.find(p ? p->id : id);
    return it == c.perks.end() ? 0 : it->second;
}

bool Server::PerkAvailable(const Critter& c, const PerkDef& p, std::string* why) const {
    auto no = [&](const std::string& s) { if (why) *why = s; return false; };
    if (PerkRank(c, p.id) >= p.ranks) return no("already at the maximum rank");
    if (c.level < p.req.level) return no("requires level " + std::to_string(p.req.level));
    static const char* const sp[7] = {"Strength", "Perception", "Endurance", "Charisma", "Intelligence", "Agility", "Luck"};
    for (int i = 0; i < 7; i++)
        if (p.req.special[i] > 0 && c.special[i] < p.req.special[i]) return no(std::string("requires ") + sp[i] + " " + std::to_string(p.req.special[i]));
    for (const auto& [skill, min] : p.req.skills)
        if (SkillValue(c, skill) < min) return no(std::string("requires ") + kSkillNames[skill] + " " + std::to_string(min));
    for (const std::string& r : p.req.perks) {
        const PerkDef* rp = content_.FindPerk(r);
        if (!rp || PerkRank(c, rp->id) < 1) return no("requires the perk " + (rp ? rp->name : r));
    }
    for (const std::string& r : p.req.traits) {
        const TraitDef* t = content_.FindTrait(r);
        if (!t || !((c.traitMask >> t->index) & 1u)) return no("requires the trait " + (t ? t->name : r));
    }
    return true;
}

bool Server::TakePerk(Critter& c, const std::string& idIn) {
    if (!c.IsPlayer() || !c.created || c.dead) return false;
    const PerkDef* p = content_.FindPerk(idIn);
    if (!p) { Tell(c, "Perks: unknown perk '" + idIn + "'. /perks lists them."); return false; }
    if (c.perkPoints < 1) { Tell(c, "Perks: you have no perk points (one every " + std::to_string(rt_.PerkRate) + " levels)."); return false; }
    std::string why;
    if (!PerkAvailable(c, *p, &why)) { Tell(c, "Perks: " + p->name + " " + why + "."); return false; }
    const int rank = ++c.perks[p->id];
    --c.perkPoints;
    const int oldMax = c.maxHp;
    RecalcDerived(c);
    if (c.maxHp > oldMax) c.hp = std::min(c.maxHp, c.hp + (c.maxHp - oldMax));
    Tell(c, "Perk gained: " + p->name + (p->ranks > 1 ? " (rank " + std::to_string(rank) + "/" + std::to_string(p->ranks) + ")" : std::string()) + ".");
    if (!p->hook.empty() && scripting_) scripting_->Call(p->hook.c_str(), &c, p->id, rank);   // optional Lua hook: fn(critter, perk_id, rank)
    SendStats(c);
    SendCritterState(c);
    SendCharSheet(c);
    return true;
}

bool Server::QuestStart(Critter& c, const std::string& id) {
    const QuestDef* q = content_.FindQuest(id);
    if (!q) return false;
    return QuestAdd(c, q->id, q->title, q->text);
}

void Server::SpawnDataNpcs() {
    size_t n = 0;
    for (const NpcRec& r : content_.npcs) {
        const Proto* p = data_.FindProto(r.id);
        if (!p || p->cls != PClass::Critter) continue;
        for (const NpcSpawn& s : r.spawns) {
            Map* m = world_.FindMapByName(s.map);
            if (!m) { log::Warn("data: npc %s: map '%s' is not loaded, spawn skipped", r.id.c_str(), s.map.c_str()); continue; }
            for (int k = 0; k < s.count; ++k)
                if (SpawnCritterProto(m->id, *p, TilePos{static_cast<int16_t>(s.x), static_cast<int16_t>(s.y)}, nullptr)) ++n;
        }
    }
    if (n) log::Info("data: %zu npc(s) spawned from data/npcs", n);
}

// ------------------------------------------------------------------ loading

bool Server::LoadContent() {
    if (cfg_.contentDir.empty()) { log::Info("data: no data/ folder found, built-in content defaults"); return true; }
    ContentReport rep;
    ContentDb nc;
    const bool ok = ContentDb::Build(cfg_.contentDir, data_.Loaded() ? &data_ : nullptr, cfg_.lenient, nc, rep);
    for (const std::string& w : rep.warnings) log::Warn("data: %s", w.c_str());
    for (const std::string& e : rep.errors) log::Error("data: %s", e.c_str());
    if (!ok) { log::Error("data: %zu error(s) in %s; fix the files or start with --lenient", rep.errors.size(), cfg_.contentDir.string().c_str()); return false; }
    if (nc.protosBuilt) data_.Commit(std::move(nc.protoSet));
    content_ = std::move(nc);
    ++contentRev_;
    log::Info("data: %s (%s)", content_.Summary().c_str(), cfg_.contentDir.string().c_str());
    return true;
}

std::vector<std::string> Server::ReloadContent() {
    std::vector<std::string> out;
    if (cfg_.contentDir.empty()) { out.push_back("data: no data/ folder is configured (start the server with --content DIR)"); return out; }
    ContentReport rep;
    ContentDb nc;
    const bool ok = ContentDb::Build(cfg_.contentDir, data_.Loaded() ? &data_ : nullptr, cfg_.lenient, nc, rep);
    for (const std::string& w : rep.warnings) log::Warn("data: %s", w.c_str());
    for (const std::string& e : rep.errors) log::Error("data: %s", e.c_str());
    if (!ok) {
        out.push_back("data reload FAILED: " + std::to_string(rep.errors.size()) + " error(s), the previous data stays active");
        for (size_t i = 0; i < rep.errors.size() && i < 6; ++i) out.push_back("  " + rep.errors[i]);
        if (rep.errors.size() > 6) out.push_back("  ... (" + std::to_string(rep.errors.size() - 6) + " more in the server log)");
        return out;
    }
    const std::vector<std::string> diff = ContentDb::Diff(content_, nc);
    if (nc.protosBuilt) data_.Commit(std::move(nc.protoSet));   // existing instances keep their pid; new spawns / pickups use the new numbers
    content_ = std::move(nc);
    ++contentRev_;
    out.push_back("data reloaded: " + content_.Summary());
    if (diff.empty()) out.push_back("  no changes");
    for (const std::string& d : diff) out.push_back("  " + d);
    if (!rep.warnings.empty()) out.push_back("  " + std::to_string(rep.warnings.size()) + " warning(s), see the server log");
    for (auto& [id, c] : world_.Critters()) {
        if (!c->IsPlayer() || !c->created || c->mapId == 0) continue;
        const int oldMax = c->maxHp;
        RecalcDerived(*c);
        if (c->maxHp > oldMax) c->hp = std::min(c->maxHp, c->hp + (c->maxHp - oldMax));
        SendContentDefs(*c);
        SendStats(*c);
        SendCritterState(*c);
        SendCharSheet(*c);
    }
    for (const std::string& l : out) log::Info("%s", l.c_str());
    return out;
}

std::string Server::ReloadAll(const std::string& whatIn) {
    const std::string what = whatIn.empty() ? "all" : Lc(whatIn);
    std::string rep;
    if (what == "scripts" || what == "all") {
        const bool ok = scripting_ && scripting_->Reload();
        rep += ok ? "scripts reloaded\n" : "scripts reload FAILED (previous scripts kept)\n";
    }
    if (what == "data" || what == "all") for (const std::string& l : ReloadContent()) rep += l + "\n";
    if (what != "data" && what != "scripts" && what != "all") rep = "usage: reload [data|scripts|all]\n";
    while (!rep.empty() && rep.back() == '\n') rep.pop_back();
    return rep;
}

// ------------------------------------------------------------------ client sync

void Server::SendContentDefs(Critter& c) {
    if (!c.peer || !c.IsPlayer()) return;
    gnet::ContentDefs m;
    m.first = 1;
    m.schema = static_cast<uint16_t>(content_.schema);
    m.revision = contentRev_;
    m.traitMax = static_cast<uint8_t>(std::clamp(TraitMaxEff(), 0, 16));
    for (const TraitDef& t : content_.traits) m.traits.push_back({static_cast<uint8_t>(t.index), t.id, t.name, t.desc});
    for (const PerkDef& p : content_.perks) m.perks.push_back({p.id, p.name, p.desc, static_cast<uint8_t>(p.ranks), static_cast<uint8_t>(std::min(255, p.req.level))});
    auto addProto = [&](const std::string& key, bool isNpc, bool onlyIfNamed) {
        const Proto* p = data_.FindProto(key);
        if (!p) return;
        gnet::ContentDefs::Item it;
        it.pid = p->pid;
        if (p->fromData) {   // new prototype: the client clones its base (the base record names it)
            const NpcRec* nr = isNpc ? content_.FindNpc(key) : nullptr;
            const std::string base = nr ? nr->base : [&] { for (const ItemRec& r : content_.items) if (Lc(r.id) == Lc(key)) return r.base; return std::string(); }();
            const Proto* b = data_.FindProto(base);
            // a base that is itself a data record still has a pid the client learns from an earlier entry of this list
            if (!b) return;
            it.basePid = b->pid;
        } else if (onlyIfNamed && p->name.empty()) return;
        it.key = p->key; it.name = p->name; it.desc = p->desc;
        it.range = static_cast<uint16_t>(std::clamp(p->range, 0, 65535));
        it.dmgMin = p->wDmgMin; it.dmgMax = p->wDmgMax;
        for (int i = 0; i < 7; ++i) it.armor[i] = static_cast<uint16_t>(std::clamp(p->armor[i], 0, 65535));
        m.items.push_back(std::move(it));
    };
    for (const ItemRec& r : content_.items) addProto(r.id, false, false);
    for (const NpcRec& r : content_.npcs) addProto(r.id, true, true);
    std::sort(m.items.begin(), m.items.end(), [](const gnet::ContentDefs::Item& a, const gnet::ContentDefs::Item& b) { return a.pid < b.pid; });
    // chunks of 40 items; the first message carries traits and perks
    std::vector<gnet::ContentDefs::Item> all = std::move(m.items);
    m.items.clear();
    size_t i = 0;
    do {
        for (size_t k = 0; k < 40 && i < all.size(); ++k, ++i) m.items.push_back(all[i]);
        Send(c, gnet::PackG(m));
        m.first = 0; m.traits.clear(); m.perks.clear(); m.items.clear();
    } while (i < all.size());
}

// ------------------------------------------------------------------ chat commands

bool Server::ContentChat(Critter& c, const std::string& text) {
    if (text.size() < 2 || text[0] != '/') return false;
    std::istringstream is(text.substr(1));
    std::string cmd;
    is >> cmd;
    cmd = Lc(cmd);
    if (cmd != "perks" && cmd != "perk" && cmd != "traits" && cmd != "reload") return false;
    if (cmd == "reload") {
        if (!cfg_.devCommands) { Tell(c, "Dev commands are disabled on this server (start it with --dev-commands or --dev-kit)."); return true; }
        std::string what;
        is >> what;
        const std::string rep = ReloadAll(what);
        std::istringstream ls(rep);
        for (std::string l; std::getline(ls, l);) Tell(c, l);
        return true;
    }
    if (cmd == "traits") {
        if (content_.traits.empty()) { Tell(c, "Traits: none defined (data/traits)."); return true; }
        Tell(c, "Traits (choose up to " + std::to_string(TraitMaxEff()) + " at creation):");
        for (const TraitDef& t : content_.traits)
            Tell(c, std::string(((c.traitMask >> t.index) & 1u) ? "[x] " : "[ ] ") + t.id + " - " + t.name + (t.desc.empty() ? "" : ": " + t.desc));
        return true;
    }
    if (cmd == "perk") {
        std::string sub, id;
        is >> sub;
        if (Lc(sub) == "take") is >> id; else id = sub;
        if (id.empty()) { Tell(c, "Usage: /perk take <id>   (/perks lists them)"); return true; }
        TakePerk(c, id);
        return true;
    }
    // /perks
    if (content_.perks.empty()) { Tell(c, "Perks: none defined (data/perks)."); return true; }
    Tell(c, "Perks: " + std::to_string(c.perkPoints) + " perk point(s). [*] taken  [+] available  [ ] locked.   /perk take <id>");
    for (const PerkDef& p : content_.perks) {
        std::string why;
        const int rank = PerkRank(c, p.id);
        const bool avail = PerkAvailable(c, p, &why);
        std::string line = std::string(rank > 0 && !avail ? "[*] " : avail ? "[+] " : "[ ] ") + p.id + " - " + p.name;
        if (p.ranks > 1) line += " (rank " + std::to_string(rank) + "/" + std::to_string(p.ranks) + ")";
        else if (rank > 0) line += " (taken)";
        if (!p.desc.empty()) line += ": " + p.desc;
        if (!avail && rank < p.ranks) line += " [" + why + "]";
        Tell(c, line);
    }
    return true;
}

}  // namespace f3
