#include "AmxHost.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include "Server.h"
#include "common/log.h"

namespace f3 {

namespace {
AmxCell FBits(float f) { AmxCell c; std::memcpy(&c, &f, 4); return c; }
float FVal(AmxCell c) { float f; std::memcpy(&f, &c, 4); return f; }
std::mt19937& Rng() { static std::mt19937 r{12345}; return r; }
}  // namespace

AmxHost::AmxHost(Server& server, std::filesystem::path scriptsDir) : server_(server), dir_(std::move(scriptsDir)) {
    available_ = std::filesystem::exists(dir_);
    BindNatives();
}

Critter* AmxHost::CritterOf(AmxCell h) {
    if (h <= 0 || (h & 0x40000000)) return nullptr;
    return server_.GetWorld().GetCritter(static_cast<CritterId>(h));
}

void AmxHost::BindNatives() {
    auto& N = natives_;
    // ---- Pawn float module (cells hold IEEE-754 bits)
    N["float"] = [](AmxInstance&, const AmxCell* a, int) { return FBits(static_cast<float>(a[0])); };
    N["floatadd"] = [](AmxInstance&, const AmxCell* a, int) { return FBits(FVal(a[0]) + FVal(a[1])); };
    N["floatsub"] = [](AmxInstance&, const AmxCell* a, int) { return FBits(FVal(a[0]) - FVal(a[1])); };
    N["floatmul"] = [](AmxInstance&, const AmxCell* a, int) { return FBits(FVal(a[0]) * FVal(a[1])); };
    N["floatdiv"] = [](AmxInstance&, const AmxCell* a, int) { return FBits(FVal(a[1]) != 0 ? FVal(a[0]) / FVal(a[1]) : 0.f); };
    N["floatfract"] = [](AmxInstance&, const AmxCell* a, int) { float f = FVal(a[0]); return FBits(f - std::floor(f)); };
    N["floatround"] = [](AmxInstance&, const AmxCell* a, int n) {
        float f = FVal(a[0]);
        int mode = n > 1 ? a[1] : 0;  // 0 round, 1 floor, 2 ceil, 3 truncate
        return static_cast<AmxCell>(mode == 1 ? std::floor(f) : mode == 2 ? std::ceil(f) : mode == 3 ? std::trunc(f) : std::round(f));
    };
    N["floatcmp"] = [](AmxInstance&, const AmxCell* a, int) { float x = FVal(a[0]), y = FVal(a[1]); return x < y ? -1 : (x > y ? 1 : 0); };

    // ---- engine API (signatures inferred from usage, see docs/server-data.md)
    N["Myself"] = [](AmxInstance& m, const AmxCell*, int) { return Handle(m.userData); };
    N["Protagonist"] = [this](AmxInstance&, const AmxCell*, int) -> AmxCell {
        for (auto& [id, c] : server_.GetWorld().Critters())
            if (c->IsPlayer() && c->mapId != 0) return static_cast<AmxCell>(id);
        return 0;
    };
    N["RandomNumber"] = [](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
        AmxCell lo = n > 0 ? a[0] : 0, hi = n > 1 ? a[1] : 100;
        if (hi < lo) std::swap(lo, hi);
        return std::uniform_int_distribution<AmxCell>(lo, hi)(Rng());
    };
    // Globals are per player while a conversation runs (the protagonist's quest state, persisted with the character);
    // outside a conversation (behaviour scripts) they are the shared world table.
    auto scope = [this]() -> Critter* { return active_ ? server_.GetWorld().GetCritter(active_->pc) : nullptr; };
    N["GetGlobal"] = [this, scope](AmxInstance& m, const AmxCell* a, int n) -> AmxCell { return n > 0 ? static_cast<AmxCell>(server_.GetGlobalFor(scope(), m.GetString(a[0]))) : 0; };
    N["SetGlobal"] = [this, scope](AmxInstance& m, const AmxCell* a, int n) -> AmxCell { if (n > 1) server_.SetGlobalFor(scope(), m.GetString(a[0]), a[1]); return 0; };
    N["GetCurrentHP"] = [this](AmxInstance&, const AmxCell* a, int) -> AmxCell { Critter* c = CritterOf(a[0]); return c ? c->hp : 0; };
    N["GetMaxHP"] = [this](AmxInstance&, const AmxCell* a, int) -> AmxCell { Critter* c = CritterOf(a[0]); return c ? c->maxHp : 0; };
    N["AddHP"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
        Critter* c = CritterOf(a[0]);
        if (!c || n < 2) return 0;
        if (a[1] >= 0) server_.Heal(*c, a[1]); else server_.Damage(*c, -a[1], nullptr);
        return 1;
    };
    N["IsDead"] = [this](AmxInstance&, const AmxCell* a, int) -> AmxCell { Critter* c = CritterOf(a[0]); return c ? c->dead : 0; };
    N["HasDied"] = N["IsDead"];
    N["GetTeam"] = [this](AmxInstance&, const AmxCell* a, int) -> AmxCell { Critter* c = CritterOf(a[0]); return c ? c->team : 0; };
    N["GetGender"] = [](AmxInstance&, const AmxCell*, int) -> AmxCell { return 0; };
    N["GetRace"] = [](AmxInstance&, const AmxCell*, int) -> AmxCell { return 0; };
    N["GetLOS"] = [](AmxInstance&, const AmxCell*, int) -> AmxCell { return 1; };
    N["Distance"] = [this](AmxInstance&, const AmxCell* a, int) -> AmxCell {  // world units (cells / 2) as float (guess)
        Critter* x = CritterOf(a[0]); Critter* y = CritterOf(a[1]);
        if (!x || !y || x->mapId != y->mapId) return FBits(9999.f);
        float dx = static_cast<float>(x->pos.x - y->pos.x), dy = static_cast<float>(x->pos.y - y->pos.y);
        return FBits(std::sqrt(dx * dx + dy * dy) / 2.f);
    };
    N["GetPosition"] = [this](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {  // (entity, float[3] out) x,y(height),z in world units
        Critter* c = CritterOf(a[0]);
        if (!c || n < 2) return 0;
        const Map* mp = server_.GetWorld().GetMap(c->mapId);
        const float ox = mp ? mp->originX : 0.f, oz = mp ? mp->originZ : 0.f;   // original world coordinates = cell / 2 + grid origin
        m.Set(a[1], FBits(c->pos.x / 2.f + ox)); m.Set(a[1] + 4, FBits(0.f)); m.Set(a[1] + 8, FBits(c->pos.y / 2.f + oz));
        return 1;
    };
    N["GetFacing"] = [this](AmxInstance&, const AmxCell* a, int) -> AmxCell { Critter* c = CritterOf(a[0]); return FBits(c ? static_cast<float>(static_cast<int>(c->dir) * 45) : 0.f); };
    N["DestroyEntity"] = [this](AmxInstance&, const AmxCell* a, int) -> AmxCell {
        if (Critter* c = CritterOf(a[0])) { if (c->IsNpc()) server_.RemoveCritter(c->id); return 1; }
        return 0;
    };
    // game clock: the demo starts at 13:13 (sys.ini [World]); 1 real second = 1 game second
    N["GetTimeStamp"] = [this](AmxInstance&, const AmxCell*, int) -> AmxCell { return static_cast<AmxCell>(server_.Tick() / static_cast<uint64_t>(server_.GetConfig().tickRate)); };
    N["SecondsPassed"] = [this](AmxInstance&, const AmxCell* a, int) -> AmxCell {
        AmxCell now = static_cast<AmxCell>(server_.Tick() / static_cast<uint64_t>(server_.GetConfig().tickRate));
        return now - a[0];
    };
    N["Hour"] = [this](AmxInstance&, const AmxCell*, int) -> AmxCell {
        uint64_t s = server_.Tick() / static_cast<uint64_t>(server_.GetConfig().tickRate) + 13 * 3600 + 13 * 60;
        return static_cast<AmxCell>((s / 3600) % 24);
    };
    N["Minute"] = [this](AmxInstance&, const AmxCell*, int) -> AmxCell {
        uint64_t s = server_.Tick() / static_cast<uint64_t>(server_.GetConfig().tickRate) + 13 * 3600 + 13 * 60;
        return static_cast<AmxCell>((s / 60) % 60);
    };
    N["GetMapID"] = [this](AmxInstance& m, const AmxCell*, int) -> AmxCell {
        auto it = instances_.find(m.userData);
        return it == instances_.end() ? 0 : it->second.map;
    };
    // ---- teams (the scripts create/seed them on every spawn; kept as accept-and-ignore stubs)
    N["CreateTeam"] = [](AmxInstance&, const AmxCell*, int) -> AmxCell { static AmxCell next = 100; return next++; };
    // ---- doors: handle = object id | 0x40000000
    auto doorOf = [this](AmxCell h) -> Item* {
        if (!(h & 0x40000000)) return nullptr;
        Item* it = server_.FindObject(static_cast<ItemId>(h & 0x3FFFFFFF));
        return it && it->kind == ObjKind::Door ? it : nullptr;
    };
    N["UnlockDoor"] = [this, doorOf](AmxInstance&, const AmxCell* a, int n) -> AmxCell { if (Item* d = n > 0 ? doorOf(a[0]) : nullptr) { server_.SetDoorLocked(*d, false); return 1; } return 0; };
    N["LockDoor"] = [this, doorOf](AmxInstance&, const AmxCell* a, int n) -> AmxCell { if (Item* d = n > 0 ? doorOf(a[0]) : nullptr) { server_.SetDoorLocked(*d, true); return 1; } return 0; };
    N["IsLocked"] = [doorOf](AmxInstance&, const AmxCell* a, int n) -> AmxCell { Item* d = n > 0 ? doorOf(a[0]) : nullptr; return d && d->locked ? 1 : 0; };
    for (const char* n : {"AddToTeam", "SetTeamAttitude", "SetTeamRank", "CreateSquad", "AddToSquad", "SetSquadAttitude", "SetSquadRank",
                          "ClearActions", "AttachSound", "RemoveSound", "SetValue"})
        N[n] = [](AmxInstance&, const AmxCell*, int) -> AmxCell { return 0; };

    // ---- dialogue natives (.scra). Argument order = reverse of the push order in the listing (a[0] is the last pushed value).
    //   SetCurrentNodeName(name$)  SetNodeTextFunction(publicName$)  RegisterNodeText(stfId)
    //   RegisterReply(flag, stfId, actionPublic$, logicPublic$)   AddEntry(taskId, stfId)   OpenTask(taskId, ?)   CloseTask(taskId, ?)
    N["SetCurrentNodeName"] = [this](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {
        if (active_ && n > 0) { active_->node = m.GetString(a[0]); active_->textId = 0; }
        return 0;
    };
    N["SetNodeTextFunction"] = [this](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {
        if (active_ && n > 0) active_->textFn = m.GetString(a[0]);
        return 0;
    };
    N["RegisterNodeText"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
        if (active_ && n > 0) active_->textId = a[0];
        return 0;
    };
    N["RegisterReply"] = [this](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {
        if (!active_ || n < 4) return 0;
        DlgReply r;
        r.flag = a[0]; r.textId = a[1]; r.action = m.GetString(a[2]); r.logic = m.GetString(a[3]);
        active_->replies.push_back(std::move(r));
        return 1;
    };
    N["PCSpeaker"] = [this](AmxInstance&, const AmxCell*, int) -> AmxCell { return active_ ? static_cast<AmxCell>(active_->pc) : 0; };
    auto playerOf = [this](AmxInstance&) -> Critter* {
        if (active_) return server_.GetWorld().GetCritter(active_->pc);
        for (auto& [id, c] : server_.GetWorld().Critters()) if (c->IsPlayer() && c->mapId != 0) return c.get();
        return nullptr;
    };
    // quest journal: task state lives in the globals table as "Task<id>" (1 open, 2 closed) so Lua and scripts can read it
    N["OpenTask"] = [this, playerOf](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {
        if (n < 1) return 0;
        server_.SetGlobalFor(playerOf(m), "Task" + std::to_string(a[0]), 1);
        if (Critter* p = playerOf(m)) server_.Tell(*p, "New task: " + DlgText(a[0]));
        return 1;
    };
    N["CloseTask"] = [this, playerOf](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {
        if (n < 1) return 0;
        server_.SetGlobalFor(playerOf(m), "Task" + std::to_string(a[0]), 2);
        if (Critter* p = playerOf(m)) server_.Tell(*p, "Task completed: " + DlgText(a[0]));
        return 1;
    };
    N["AddEntry"] = [this, playerOf](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {
        if (n < 2) return 0;
        server_.SetGlobalFor(playerOf(m), "TaskEntry" + std::to_string(a[0]), a[1]);
        if (Critter* p = playerOf(m)) server_.Tell(*p, "Journal: " + DlgText(a[1]));
        return 1;
    };
    N["IsInDialogue"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell { return n > 0 && a[0] > 0 && InDialog(static_cast<CritterId>(a[0])) ? 1 : 0; };
    N["StartStore"] = [this, playerOf](AmxInstance& m, const AmxCell*, int) -> AmxCell {
        if (Critter* p = playerOf(m)) server_.Tell(*p, "(trading is not implemented yet)");
        return 0;
    };
    N["StartDialogue"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {  // (npc, pc): called by behaviours such as 00_TutorialFrank
        if (n < 2) return 0;
        Critter* npc = CritterOf(a[0]); Critter* pc = CritterOf(a[1]);
        return npc && pc && pc->IsPlayer() && StartDialog(*pc, *npc) ? 1 : 0;
    };
}

// ------------------------------------------------------------------ dialogue engine
//
// Conversation graph of a .scra (see docs/amx-scripts.md): OnStartDialogue picks the first node (RL1_N1 -> N<k>); a node function
// N<k> runs NA<k>, names the node (SetCurrentNodeName), sets the reply builder (SetNodeTextFunction "NU<k>_N<k>") and registers
// the NPC text (RegisterNodeText). The reply builder registers the visible replies (RegisterReply(flag, text, RA, RL)). Choosing a
// reply runs its action public RA, then its logic public RL, which sets globals (LA...) and enters the next node N<n>.
// SetNodeTextFunction("EndDialogue") ends the conversation.

std::string AmxHost::DlgText(int id) const {
    std::string s = server_.Data().String(id);
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {   // strip {c255} style colour / format tags
        if (s[i] == '{') { size_t e = s.find('}', i); if (e != std::string::npos) { i = e; continue; } }
        out.push_back(s[i]);
    }
    return out;
}

bool AmxHost::InDialog(CritterId id) const {
    if (dialogs_.count(id)) return true;
    for (auto& [pc, d] : dialogs_) if (d.npc == id) return true;
    return false;
}

AmxHost::DlgView AmxHost::DialogView(CritterId pc) const {
    DlgView v;
    auto it = dialogs_.find(pc);
    if (it == dialogs_.end()) return v;
    v.open = true; v.textId = it->second.textId; v.text = DlgText(it->second.textId); v.node = it->second.node;
    for (auto& r : it->second.replies) v.replies.push_back(DlgText(r.textId));
    return v;
}

bool AmxHost::StartDialog(Critter& pc, Critter& npc) {
    if (!available_ || npc.dialog.empty() || !pc.IsPlayer()) return false;
    std::string base = npc.dialog;
    if (base.size() > 4 && DataStore::Lower(base.substr(base.size() - 4)) == ".amx") base.resize(base.size() - 4);
    std::shared_ptr<AmxScript> s;
    auto it = dlgScripts_.find(base);
    if (it != dlgScripts_.end()) s = it->second;
    else {
        std::filesystem::path p = dir_ / (base + ".scra");
        std::string err;
        if (std::filesystem::exists(p)) s = AmxScript::Load(p.string(), err);
        if (!s) log::Warn("amx dialogue '%s' not loadable %s", base.c_str(), err.c_str());
        dlgScripts_[base] = s;
    }
    if (!s || !s->HasPublic("OnStartDialogue")) return false;
    if (dialogs_.count(pc.id)) DialogClose(pc);
    Dlg d;
    d.pc = pc.id; d.npc = npc.id;
    d.inst = std::make_unique<AmxInstance>(s, &natives_);
    d.inst->userData = npc.id;
    Dlg* prev = active_;
    auto& slot = dialogs_[pc.id] = std::move(d);
    active_ = &slot;
    slot.inst->Call("OnStartDialogue");
    active_ = prev;
    DlgView v = DialogView(pc.id);
    log::Info("amx dialogue: %s <-> %s script '%s' node '%s'", pc.name.c_str(), npc.name.c_str(), base.c_str(), v.node.c_str());
    gnet::DialogOpen o; o.npc = npc.id; o.name = npc.name;
    server_.SendBuf(pc, gnet::PackG(o));
    Present(pc);
    return true;
}

void AmxHost::Present(Critter& pc) {
    auto it = dialogs_.find(pc.id);
    if (it == dialogs_.end()) return;
    Dlg& d = it->second;
    d.replies.clear();
    if (d.textFn.empty() || d.textFn == "EndDialogue" || !d.inst->Script().HasPublic(d.textFn)) { DialogClose(pc); return; }
    Dlg* prev = active_;
    active_ = &d;
    d.inst->Call(d.textFn);   // NU<k>_N<k>: registers the replies whose conditions hold
    active_ = prev;
    gnet::DialogNode m;
    m.npc = d.npc; m.textId = static_cast<uint32_t>(d.textId); m.text = DlgText(d.textId);
    if (d.replies.empty()) {   // nothing selectable: give the player a way out
        DlgReply r; r.textId = 0; r.logic = "EndDialogue";
        d.replies.push_back(r);
    }
    for (auto& r : d.replies) {
        gnet::DialogNode::Reply pr; pr.replyId = static_cast<uint32_t>(r.textId);
        pr.text = r.textId ? DlgText(r.textId) : std::string("[Leave]");
        m.replies.push_back(std::move(pr));
        if (m.replies.size() >= 24) break;
    }
    server_.SendBuf(pc, gnet::PackG(m));
}

void AmxHost::DialogChoose(Critter& pc, int index) {
    auto it = dialogs_.find(pc.id);
    if (it == dialogs_.end()) return;
    Dlg& d = it->second;
    if (index < 0 || static_cast<size_t>(index) >= d.replies.size()) return;
    DlgReply r = d.replies[static_cast<size_t>(index)];
    if (r.logic == "EndDialogue" || (r.logic.empty() && r.action.empty())) { DialogClose(pc); return; }
    d.replies.clear();
    Dlg* prev = active_;
    active_ = &d;
    const CritterId id = pc.id;
    if (!r.action.empty() && d.inst->Script().HasPublic(r.action)) d.inst->Call(r.action);
    if (!r.logic.empty() && d.inst->Script().HasPublic(r.logic)) d.inst->Call(r.logic);
    active_ = prev;
    if (dialogs_.count(id)) Present(pc);
}

void AmxHost::DialogClose(Critter& pc, bool notify) {
    if (!dialogs_.erase(pc.id)) return;
    if (notify) server_.SendBuf(pc, gnet::PackG(gnet::DialogClose{}));
}

std::shared_ptr<AmxScript> AmxHost::GetScript(const std::string& name) {
    auto it = scripts_.find(name);
    if (it != scripts_.end()) return it->second;
    std::shared_ptr<AmxScript> s;
    for (const char* ext : {".scrb", ".scra"}) {
        std::filesystem::path p = dir_ / (name + ext);
        if (!std::filesystem::exists(p)) continue;
        std::string err;
        s = AmxScript::Load(p.string(), err);
        if (!s) log::Warn("amx: %s: %s", p.string().c_str(), err.c_str());
        break;
    }
    scripts_[name] = s;
    return s;
}

std::unique_ptr<AmxInstance> AmxHost::NewInstance(const std::string& scriptName, uint64_t key) {
    std::string base = scriptName;
    if (base.size() > 4 && DataStore::Lower(base.substr(base.size() - 4)) == ".amx") base.resize(base.size() - 4);
    if (base.empty() || base[0] == '<') return nullptr;
    auto s = GetScript(base);
    if (!s) return nullptr;
    auto inst = std::make_unique<AmxInstance>(s, &natives_);
    inst->userData = key;
    return inst;
}

void AmxHost::AttachCritter(const Critter& c) {
    if (!available_ || c.script.empty()) return;
    auto inst = NewInstance(c.script, c.id);
    if (!inst) return;
    Entry e;
    e.inst = std::move(inst);
    e.map = c.mapId;
    e.name = c.script;
    instances_[c.id] = std::move(e);
}

void AmxHost::AttachObject(const Item& it) {
    if (!available_ || it.script.empty()) return;
    uint64_t key = it.id | kObjectBit;
    auto inst = NewInstance(it.script, key);
    if (!inst) return;
    Entry e;
    e.inst = std::move(inst);
    e.map = it.mapId;
    e.name = it.script;
    instances_[key] = std::move(e);
}

void AmxHost::Detach(uint64_t key) {
    instances_.erase(key);
    for (auto it = dialogs_.begin(); it != dialogs_.end();) {   // a critter that leaves ends its conversations
        if (it->first == key || it->second.npc == key) {
            if (it->first != key) if (Critter* p = server_.GetWorld().GetCritter(it->first)) server_.SendBuf(*p, gnet::PackG(gnet::DialogClose{}));
            if (active_ == &it->second) active_ = nullptr;
            it = dialogs_.erase(it);
        } else ++it;
    }
}

void AmxHost::Tick(uint64_t tick, const std::unordered_set<MapId>& active) {
    int rate = server_.GetConfig().tickRate;
    for (auto it = dialogs_.begin(); it != dialogs_.end();) {   // walking away or a dead partner ends a conversation
        Critter* p = server_.GetWorld().GetCritter(it->first);
        Critter* n = server_.GetWorld().GetCritter(it->second.npc);
        if (p && n && !p->dead && !n->dead && p->mapId == n->mapId && TileDist(p->pos, n->pos) <= 12) { ++it; continue; }
        if (p) server_.SendBuf(*p, gnet::PackG(gnet::DialogClose{}));
        it = dialogs_.erase(it);
    }
    for (auto& [key, e] : instances_) {
        if (!active.count(e.map)) continue;
        if (!e.created) {
            e.created = true;
            e.inst->Call("OnCreate");
            e.inst->Call("OnLoad");
            continue;
        }
        if (tick % static_cast<uint64_t>(rate) == key % static_cast<uint64_t>(rate)) e.inst->Call("OnUpdate");
    }
}

void AmxHost::CallObject(ItemId id, const char* pub) {
    auto it = instances_.find(id | kObjectBit);
    if (it == instances_.end() || !it->second.created || !it->second.inst->Script().HasPublic(pub)) return;
    it->second.inst->Call(pub);
}

void AmxHost::OnDeath(const Critter& c) {
    auto it = instances_.find(c.id);
    if (it == instances_.end()) return;
    it->second.inst->Call("OnDeath");
}

std::string AmxHost::RunTest(const std::string& script, const std::string& pub) {
    auto inst = NewInstance(script, 0);
    if (!inst) return "cannot load script '" + script + "'";
    if (!inst->Script().HasPublic(pub)) return "no public '" + pub + "'";
    AmxInstance::SetTrace(true);
    AmxCell ret = 0;
    bool ok = inst->Call(pub, {}, &ret);
    AmxInstance::SetTrace(false);
    return std::string(ok ? "ok" : "error: " + inst->LastError()) + ", returned " + std::to_string(ret);
}

}  // namespace f3
