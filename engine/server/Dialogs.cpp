// Data dialogs (data/dialogs/*.json, docs/dialogs.md): a conversation graph run through the same dialog protocol as the original AMX dialogues
// (S2C_DialogOpen / S2C_DialogNode / S2C_DialogClose, C2S_DialogChoose / C2S_DialogClose). An NPC uses one when its `dialog` names a data dialog id;
// every other `dialog` value still runs the original .scra script (AmxHost). Loader and validation: Content.cpp.
#include <algorithm>
#include "AmxHost.h"
#include "Scripting.h"
#include "Server.h"
#include "common/log.h"

namespace f3 {

namespace {
std::string Lc(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

bool Cmp(int have, DlgCond::Op op, int want) {
    switch (op) {
        case DlgCond::Ge: return have >= want;
        case DlgCond::Le: return have <= want;
        case DlgCond::Eq: return have == want;
        case DlgCond::Ne: return have != want;
        case DlgCond::Gt: return have > want;
        case DlgCond::Lt: return have < want;
    }
    return false;
}

std::string StripTags(const std::string& s) {   // {c255} style colour / format tags of the original strings
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '{') { size_t e = s.find('}', i); if (e != std::string::npos) { i = e; continue; } }
        out.push_back(s[i]);
    }
    return out;
}

void ReplaceAll(std::string& s, const std::string& from, const std::string& to) {
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
}
}  // namespace

bool Server::HasDataDialog(const Critter& npc) const { return !npc.dialog.empty() && content_.FindDialog(npc.dialog) != nullptr; }
bool Server::InDataDialog(CritterId id) const {
    if (ddlgs_.count(id)) return true;
    for (const auto& [pc, d] : ddlgs_) if (d.npc == id) return true;
    return false;
}
Server::DataDlgView Server::DataDialogView(CritterId pc) const {
    DataDlgView v;
    auto it = ddlgs_.find(pc);
    if (it == ddlgs_.end()) return v;
    v.open = true; v.dialog = it->second.dialog; v.node = it->second.node;
    return v;
}

bool Server::EvalCond(const Critter& pc, const Critter* npc, const DlgCond& d) {
    bool r = false;
    switch (d.kind) {
        case DlgCond::Skill:
            for (int i = 0; i < kSkillCount; ++i) if (Lc(kSkillNames[i]) == Lc(d.name)) r = Cmp(SkillValue(pc, i), d.op, d.value);
            break;
        case DlgCond::Stat: {
            static const char* const sp[7] = {"strength", "perception", "endurance", "charisma", "intelligence", "agility", "luck"};
            int have = 0;
            bool known = false;
            for (int i = 0; i < 7; ++i) if (d.name == sp[i]) { have = pc.special[i]; known = true; }
            if (d.name == "level") { have = pc.level; known = true; }
            else if (d.name == "hp") { have = pc.hp; known = true; }
            else if (d.name == "max_hp") { have = pc.maxHp; known = true; }
            else if (d.name == "xp") { have = pc.xp; known = true; }
            r = known && Cmp(have, d.op, d.value);
            break;
        }
        case DlgCond::Item: {
            int have = 0;
            const std::string want = Lc(d.name);
            for (const InvItem& it : pc.inventory) {
                const Proto* p = data_.GetProto(it.pid);
                if (p && Lc(p->key) == want) have += it.count;
            }
            r = Cmp(have, d.op, d.value);
            break;
        }
        case DlgCond::Quest: r = Cmp(QuestState(pc, d.name), d.op, d.value); break;
        case DlgCond::Flag: r = Cmp(static_cast<int>(GetGlobalFor(&pc, d.name)), d.op, d.value); break;
        case DlgCond::Lua: r = scripting_ && scripting_->CallHandled(d.name.c_str(), const_cast<Critter*>(&pc), const_cast<Critter*>(npc)); break;
        case DlgCond::All:
            r = true;
            for (const DlgCond& k : d.kids) if (!EvalCond(pc, npc, k)) { r = false; break; }
            break;
        case DlgCond::Any:
            for (const DlgCond& k : d.kids) if (EvalCond(pc, npc, k)) { r = true; break; }
            break;
        case DlgCond::Not: r = d.kids.empty() || !EvalCond(pc, npc, d.kids[0]); break;
    }
    return d.neg ? !r : r;
}

std::string Server::DataDlgText(const Critter& pc, const Critter* npc, const DialogDef& def, const DlgNode& node, int answer) const {
    std::string s;
    const int stf = answer < 0 ? node.stf : node.answers[static_cast<size_t>(answer)].stf;
    if (stf > 0) s = StripTags(Stf(pc, stf));
    else {
        const std::string& base = answer < 0 ? node.text : node.answers[static_cast<size_t>(answer)].text;
        std::string key = "dialog." + def.id + "." + node.id + ".";
        if (answer < 0) key += "text";
        else key += !node.answers[static_cast<size_t>(answer)].id.empty() ? node.answers[static_cast<size_t>(answer)].id : "a" + std::to_string(answer + 1);
        s = LOr(pc, key, base);
    }
    ReplaceAll(s, "{player}", pc.name);
    ReplaceAll(s, "{npc}", npc ? npc->name : std::string());
    return s;
}

void Server::DataDialogClose(Critter& pc, bool notify) {
    if (!ddlgs_.erase(pc.id)) return;
    if (notify) SendBuf(pc, gnet::PackG(gnet::DialogClose{}));
}

void Server::CloseDataDialogsOf(CritterId id) {
    for (auto it = ddlgs_.begin(); it != ddlgs_.end();) {
        if (it->first == id || it->second.npc == id) {
            if (it->first != id)
                if (Critter* p = world_.GetCritter(it->first)) SendBuf(*p, gnet::PackG(gnet::DialogClose{}));
            it = ddlgs_.erase(it);
        } else ++it;
    }
}

void Server::TickDataDialogs() {
    for (auto it = ddlgs_.begin(); it != ddlgs_.end();) {   // walking away, death or a dead partner ends a conversation
        Critter* p = world_.GetCritter(it->first);
        if (it->second.npc == 0 && it->second.obj != 0) {   // a conversation with a map object (data/objects): ends when the player walks away or the object is gone
            Map* om = p ? world_.GetMap(p->mapId) : nullptr;
            auto oi = om ? om->items.find(it->second.obj) : decltype(om->items.end()){};
            if (p && om && !p->dead && oi != om->items.end() && ObjDist(oi->second, p->pos) <= 12) { ++it; continue; }
            if (p) SendBuf(*p, gnet::PackG(gnet::DialogClose{}));
            it = ddlgs_.erase(it);
            continue;
        }
        Critter* n = world_.GetCritter(it->second.npc);
        if (p && n && !p->dead && !n->dead && p->mapId == n->mapId && TileDist(p->pos, n->pos) <= 12) { ++it; continue; }
        if (p) SendBuf(*p, gnet::PackG(gnet::DialogClose{}));
        it = ddlgs_.erase(it);
    }
}

// Runs the effects in order. Returns true when the conversation was closed by an effect (end / attack): the caller must stop.
bool Server::RunDlgEffects(Critter& pc, Critter* npc, const std::vector<DlgEffect>& fx) {
    for (const DlgEffect& e : fx) {
        switch (e.kind) {
            case DlgEffect::Give:
                if (const Proto* p = data_.FindProto(e.s)) { GiveItem(pc, *p, e.n); SendInventory(pc); SendStats(pc); }
                break;
            case DlgEffect::Take: {
                const std::string want = Lc(e.s);
                int left = e.n;
                for (size_t i = 0; i < pc.inventory.size() && left > 0;) {
                    InvItem& it = pc.inventory[i];
                    const Proto* p = data_.GetProto(it.pid);
                    if (!p || Lc(p->key) != want) { ++i; continue; }
                    if (it.equipped) EquipItem(pc, it.id, false);
                    const int n = std::min<int>(left, it.count);
                    it.count = static_cast<uint16_t>(it.count - n);
                    left -= n;
                    if (it.count == 0) pc.inventory.erase(pc.inventory.begin() + static_cast<long>(i)); else ++i;
                }
                SendInventory(pc);
                SendStats(pc);
                break;
            }
            case DlgEffect::Quest: {
                if (e.action == "add") { QuestStart(pc, e.s); break; }
                if (QuestState(pc, e.s) < 0) QuestStart(pc, e.s);   // done / fail of a task that was never added: add it first (title and rewards come from data/quests)
                QuestSet(pc, e.s, e.action == "done" ? 1 : e.action == "fail" ? 2 : 0);
                break;
            }
            case DlgEffect::Flag:
                SetGlobalFor(e.action == "world" ? nullptr : &pc, e.s, e.n);
                if (Map* fm = world_.GetMap(pc.mapId)) fm->dirty = true;   // objects hidden until a flag appear / vanish on the next visibility pass
                break;
            case DlgEffect::Xp: AddXp(pc, e.n); break;
            case DlgEffect::Trade: TellK(pc, "msg.trade_missing"); break;   // stub: there is no store yet
            case DlgEffect::Attack:
                DataDialogClose(pc);
                if (npc && npc->IsNpc() && !npc->isStatic && !npc->dead) { npc->target = pc.id; npc->nextRepathTick = 0; }
                return true;
            case DlgEffect::End: DataDialogClose(pc); return true;
            case DlgEffect::Door: {
                Map* m = world_.GetMap(npc ? npc->mapId : pc.mapId);
                if (!m || !m->InBounds(e.x, e.y)) break;
                Item* o = nullptr;
                auto dc = m->doorCell.find(e.y * m->width + e.x);
                if (dc != m->doorCell.end()) o = FindObject(dc->second);
                if (!o) for (auto& [id, it] : m->items) if ((it.kind == ObjKind::Door || it.kind == ObjKind::Container) && it.pos.x == e.x && it.pos.y == e.y) { o = &it; break; }
                if (!o) { log::Warn("dialog: no door / container at %d,%d on %s", e.x, e.y, m->name.c_str()); break; }
                if (e.action == "unlock") SetDoorLocked(*o, false, "dialog");
                else if (e.action == "lock") { Operate(nullptr, *m, *o, 0); SetDoorLocked(*o, true, "dialog"); }
                else if (e.action == "close") Operate(nullptr, *m, *o, 0);
                else { if (o->locked) SetDoorLocked(*o, false, "dialog"); Operate(nullptr, *m, *o, 1); }
                break;
            }
            case DlgEffect::Lua:
                if (scripting_) scripting_->Call(e.s.c_str(), &pc, npc);
                break;
        }
    }
    return false;
}

// Enters a node: on_enter effects, then sends the text and the answers whose conditions hold.
void Server::DataDlgEnter(Critter& pc, const std::string& nodeId) {
    auto it = ddlgs_.find(pc.id);
    if (it == ddlgs_.end()) return;
    Critter* npc = it->second.npc ? world_.GetCritter(it->second.npc) : nullptr;
    const DialogDef* def = content_.FindDialog(it->second.dialog);
    const DlgNode* node = def ? def->FindNode(nodeId) : nullptr;
    if ((it->second.npc && !npc) || !node) { DataDialogClose(pc); return; }   // removed by a reload / the speaker is gone
    it->second.node = node->id;
    const CritterId pcId = pc.id;
    if (RunDlgEffects(pc, npc, node->onEnter)) return;
    if (!ddlgs_.count(pcId)) return;
    // an on_enter effect (Lua) may have reloaded data: look the node up again
    def = content_.FindDialog(ddlgs_[pcId].dialog);
    node = def ? def->FindNode(nodeId) : nullptr;
    if (!node) { DataDialogClose(pc); return; }
    DataDlg& d = ddlgs_[pcId];
    d.shown.clear();
    gnet::DialogNode m;
    m.npc = npc ? npc->id : 0;
    m.text = DataDlgText(pc, npc, *def, *node, -1);
    for (size_t i = 0; i < node->answers.size() && m.replies.size() < 24; ++i) {
        const DlgAnswer& a = node->answers[i];
        if (a.hasCond && !EvalCond(pc, npc, a.cond)) continue;
        gnet::DialogNode::Reply r;
        r.text = DataDlgText(pc, npc, *def, *node, static_cast<int>(i));
        m.replies.push_back(std::move(r));
        d.shown.push_back(static_cast<int>(i));
    }
    if (m.replies.empty()) {   // nothing selectable: give the player a way out
        gnet::DialogNode::Reply r;
        r.text = L(pc, "dlg.leave");
        m.replies.push_back(std::move(r));
        d.shown.push_back(-1);
    }
    SendBuf(pc, gnet::PackG(m));
}

bool Server::StartDataDialog(Critter& pc, Critter& npc) {
    if (!pc.IsPlayer() || pc.dead || npc.dead) return false;
    const DialogDef* def = npc.dialog.empty() ? nullptr : content_.FindDialog(npc.dialog);
    if (!def) return false;
    if (ddlgs_.count(pc.id)) DataDialogClose(pc);
    if (amx_) amx_->DialogClose(pc);
    std::string first;
    for (const DlgRoute& r : def->start)
        if (!r.hasCond || EvalCond(pc, &npc, r.cond)) { first = r.gotoNode; break; }
    if (first.empty() || !def->FindNode(first)) { TellK(pc, "msg.dialog.nothing_to_say", {{"npc", npc.name}}); return true; }
    DataDlg d;
    d.pc = pc.id; d.npc = npc.id; d.dialog = def->id;
    ddlgs_[pc.id] = std::move(d);
    log::Info("data dialogue: %s <-> %s dialog '%s' node '%s'", pc.name.c_str(), npc.name.c_str(), def->id.c_str(), first.c_str());
    gnet::DialogOpen o;
    o.npc = npc.id;
    o.name = npc.name;
    SendBuf(pc, gnet::PackG(o));
    DataDlgEnter(pc, first);
    return true;
}

// A data dialog whose speaker is a map object (terminal, lever, ...) instead of an NPC: same protocol, npc id 0 and the object's name.
bool Server::StartObjectDialog(Critter& pc, const std::string& dialogId, const Item& obj, const std::string& speakerName) {
    if (!pc.IsPlayer() || pc.dead) return false;
    const DialogDef* def = content_.FindDialog(dialogId);
    if (!def) { log::Warn("object dialog: unknown dialog '%s'", dialogId.c_str()); return false; }
    if (ddlgs_.count(pc.id)) DataDialogClose(pc);
    if (amx_) amx_->DialogClose(pc);
    std::string first;
    for (const DlgRoute& r : def->start)
        if (!r.hasCond || EvalCond(pc, nullptr, r.cond)) { first = r.gotoNode; break; }
    if (first.empty() || !def->FindNode(first)) { TellK(pc, "msg.dialog.nothing_to_say", {{"npc", speakerName}}); return true; }
    DataDlg d;
    d.pc = pc.id; d.npc = 0; d.obj = obj.id; d.dialog = def->id;
    ddlgs_[pc.id] = std::move(d);
    log::Info("data dialogue: %s <-> object '%s' dialog '%s' node '%s'", pc.name.c_str(), speakerName.c_str(), def->id.c_str(), first.c_str());
    gnet::DialogOpen o;
    o.npc = 0;
    o.name = speakerName;
    SendBuf(pc, gnet::PackG(o));
    DataDlgEnter(pc, first);
    return true;
}

void Server::DataDialogChoose(Critter& pc, int index) {
    auto it = ddlgs_.find(pc.id);
    if (it == ddlgs_.end()) return;
    DataDlg& d = it->second;
    if (index < 0 || static_cast<size_t>(index) >= d.shown.size()) return;
    const int ai = d.shown[static_cast<size_t>(index)];
    Critter* npc = d.npc ? world_.GetCritter(d.npc) : nullptr;
    const DialogDef* def = content_.FindDialog(d.dialog);
    const DlgNode* node = def ? def->FindNode(d.node) : nullptr;
    if ((d.npc && !npc) || !node) { DataDialogClose(pc); return; }
    if (ai < 0 || static_cast<size_t>(ai) >= node->answers.size()) { DataDialogClose(pc); return; }   // the leave reply (or a node changed by a reload)
    const DlgAnswer a = node->answers[static_cast<size_t>(ai)];   // copy: effects may run Lua that reloads the data
    const CritterId pcId = pc.id;
    d.shown.clear();
    if (RunDlgEffects(pc, npc, a.effects)) return;
    if (!ddlgs_.count(pcId)) return;
    if (a.gotoNode.empty() || Lc(a.gotoNode) == "end") { DataDialogClose(pc); return; }
    DataDlgEnter(pc, a.gotoNode);
}

}  // namespace f3
