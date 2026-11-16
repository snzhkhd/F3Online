#include "Server.h"

#include <enet/enet.h>

#include <algorithm>
#include <random>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <sstream>
#include <thread>
#include <unordered_set>
#include "Scripting.h"
#include "common/log.h"
#include "common/utf8.h"

namespace f3 {

namespace {
uint32_t ColorFromName(const std::string& n) {
    uint32_t h = 2166136261u;
    for (unsigned char ch : n) { h ^= ch; h *= 16777619u; }
    uint32_t r = 80 + (h & 0x7F), g = 80 + ((h >> 8) & 0x7F), b = 80 + ((h >> 16) & 0x7F);
    return (r << 24) | (g << 16) | (b << 8) | 0xFFu;
}

// Player text is UTF-8: invalid sequences, control characters and text spoofing code points are dropped, the length is counted in code points.
std::string SanitizeText(const std::string& in, size_t maxLen) { return utf8::SanitizeText(in, maxLen); }

// Names: 2..kMaxNameLen code points, Latin or Cyrillic letters, digits and '_'.
bool ValidName(const std::string& n) { return utf8::ValidName(n, 2, kMaxNameLen); }

Critter* PeerCritter(World& w, ENetPeer* peer) {
    CritterId id = static_cast<CritterId>(reinterpret_cast<uintptr_t>(peer->data));
    return id ? w.GetCritter(id) : nullptr;
}
}  // namespace

namespace {
uint32_t g_serverSeed = 0;
}
void SetServerSeed(uint32_t seed) { g_serverSeed = seed; }
uint32_t ServerSeed() {
    if (!g_serverSeed) { g_serverSeed = std::random_device{}(); if (!g_serverSeed) g_serverSeed = 1; }
    return g_serverSeed;
}

Server::Server(Config cfg) : cfg_(std::move(cfg)) {}

Server::~Server() {
    if (host_) {
        for (size_t i = 0; i < host_->peerCount; ++i)
            if (host_->peers[i].state == ENET_PEER_STATE_CONNECTED) enet_peer_disconnect_now(&host_->peers[i], 0);
        enet_host_flush(host_);
        enet_host_destroy(host_);
        enet_deinitialize();
    }
}

bool Server::Init() {
    LoadLocale();
    if (enet_initialize() != 0) { log::Error("enet_initialize failed"); return false; }
    ENetAddress addr{};
    addr.host = ENET_HOST_ANY;
    if (!cfg_.bindHost.empty() && enet_address_set_host(&addr, cfg_.bindHost.c_str()) != 0) {
        log::Error("cannot resolve bind address '%s'", cfg_.bindHost.c_str());
        enet_deinitialize();
        return false;
    }
    addr.port = cfg_.port;
    host_ = enet_host_create(&addr, static_cast<size_t>(cfg_.maxClients), 2, 0, 0);
    if (!host_) { log::Error("cannot listen on port %u (already in use?)", cfg_.port); enet_deinitialize(); return false; }

    if (!cfg_.noData) {
        std::filesystem::path dd = cfg_.dataDir;
        std::string err;
        if (!dd.empty() && !std::filesystem::exists(dd / "manifest.json")) {
            log::Error("data dir %s has no manifest.json (run f3_dataimport)", dd.string().c_str());
            return false;
        }
        if (!dd.empty()) {
            if (!data_.Load(dd, err)) { log::Error("data: %s", err.c_str()); return false; }
            if (!LoadContent()) return false;   // data/*.json on top of the original prototypes (before the world is spawned)
            amx_ = std::make_unique<AmxHost>(*this, dd / "scripts");
            if (!LoadWorldData()) { log::Error("data: no maps could be loaded"); return false; }
            SpawnDataNpcs();
        }
    }
    if (!data_.Loaded() && !LoadContent()) return false;   // no gamedata: traits / perks / skills / quests only
    if (cfg_.savesDir.empty()) cfg_.savesDir = cfg_.scriptsDir.parent_path() / "saves";
    if (!OpenStore()) return false;   // <saves>/f3.db (+ import of old JSON saves); a damaged file is refused, never replaced
    LoadWorldState();   // door / container state per map (table world_objects)
    SetServerSeed(cfg_.seed);
    log::Info("rng seed: %u%s", ServerSeed(), cfg_.seed ? "" : " (random; replay with --seed)");
    accounts_.ConfigureLimit(cfg_.maxLoginFails, 300, 30);
    if (cfg_.devKit) { cfg_.allowGuest = true; cfg_.devCommands = true; }
    nextSaveTick_ = static_cast<uint64_t>(std::max(0, cfg_.saveIntervalSec)) * cfg_.tickRate;
    log::Info("accounts: %s (guest login %s, autosave %ds)", cfg_.savesDir.string().c_str(), cfg_.allowGuest ? "ALLOWED" : "off", cfg_.saveIntervalSec);
    scripting_ = std::make_unique<Scripting>(*this, cfg_.scriptsDir);
    if (!scripting_->Load()) { log::Error("scripts failed to load from %s", cfg_.scriptsDir.string().c_str()); return false; }
    if (!world_.FirstMap()) { log::Error("scripts created no map (call create_map in on_init)"); return false; }
    BindObjects();   // data/objects: bindings -> map objects, scripts/objects/*.lua
    log::Info("f3_server listening on UDP %u, %d Hz, %zu map(s)", cfg_.port, cfg_.tickRate, world_.Maps().size());
    return true;
}

void Server::PostCommand(std::string cmd) {
    std::lock_guard<std::mutex> lk(cmdMutex_);
    commands_.push_back(std::move(cmd));
}

void Server::PostTask(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(cmdMutex_);
    tasks_.push_back(std::move(fn));
}

void Server::Run() {
    using clock = std::chrono::steady_clock;
    running_ = true;
    const auto step = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / cfg_.tickRate));
    const auto start = clock::now();
    auto next = start;
    while (running_) {
        HandleEvents(1);
        ProcessCommands();
        auto now = clock::now();
        int guard = 0;
        while (now >= next && guard++ < 5) { DoTick(); next += step; }
        if (now >= next) next = now;  // fell too far behind: drop the backlog
        if (cfg_.maxRunSeconds > 0 && now - start > std::chrono::seconds(cfg_.maxRunSeconds)) break;
    }
    SaveAllPlayers();
    SaveWorldState();
    log::Info("server stopping");
}

// ---------------------------------------------------------------- network

void Server::HandleEvents(int timeoutMs) {
    ENetEvent ev;
    int r = enet_host_service(host_, &ev, static_cast<enet_uint32>(timeoutMs));
    while (r > 0) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                ev.peer->data = nullptr;
                log::Info("peer connected (slot %u)", static_cast<unsigned>(ev.peer - host_->peers));
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                HandlePacket(ev.peer, ev.packet->data, ev.packet->dataLength);
                enet_packet_destroy(ev.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                OnDisconnect(ev.peer);
                break;
            default: break;
        }
        r = enet_host_service(host_, &ev, 0);
    }
}

void Server::SendRaw(ENetPeer* peer, const ByteBuffer& b) {
    ENetPacket* p = enet_packet_create(b.Data(), b.Size(), ENET_PACKET_FLAG_RELIABLE);
    if (p) enet_peer_send(peer, 0, p);
}

void Server::Send(Critter& to, const ByteBuffer& b) {
    if (to.peer) SendRaw(to.peer, b);
}

void Server::HandlePacket(ENetPeer* peer, const uint8_t* data, size_t len) {
    if (len == 0 || len > kMaxPacketSize) { enet_peer_disconnect_later(peer, 0); return; }
    ByteBuffer b(data, len);
    Msg id = static_cast<Msg>(b.ReadU8());
    Critter* c = PeerCritter(world_, peer);
    bool ok = true;
    if (c && !c->created) {   // character creation pending (the session is not in the world): only ping, state requests, caps and the creation itself
        const uint8_t raw = static_cast<uint8_t>(id);
        if (raw == static_cast<uint8_t>(GMsg::C2S_ReqState)) { SendStats(*c); SendCharSheet(*c); return; }   // no inventory / wait state / map yet
        if (raw != static_cast<uint8_t>(Msg::C2S_Ping) && raw != static_cast<uint8_t>(GMsgRt::C2S_ClientCaps) && raw != static_cast<uint8_t>(GMsgChar::C2S_CharCreate))
            return;
    }

    switch (id) {
        case Msg::C2S_Login: {
            net::LoginReq m;
            ok = m.Read(b);
            if (ok && !c) OnLogin(peer, m);
            break;
        }
        case Msg::C2S_MoveTo: {
            net::MoveToReq m;
            ok = m.Read(b);
            if (ok && c) OnMoveReq(*c, m);
            break;
        }
        case Msg::C2S_Chat: {
            net::ChatReq m;
            ok = m.Read(b);
            if (ok && c) OnChatReq(*c, m.text);
            break;
        }
        case Msg::C2S_Ping: {
            net::Ping m;
            ok = m.Read(b);
            if (ok) SendRaw(peer, net::Pack(net::Pong{m.token}));
            break;
        }
        default:
            if (static_cast<uint8_t>(id) == static_cast<uint8_t>(GMsgAuth::C2S_Auth)) {
                gnet::Auth m;
                ok = m.Read(b);
                if (ok && !c) OnAuth(peer, m);
            } else if (static_cast<uint8_t>(id) >= 20 && static_cast<uint8_t>(id) < 48) {
                if (c) HandleGamePacket(peer, c, static_cast<GMsg>(id), b, ok);
            } else {
                ok = false;
            }
            break;
    }
    if (!ok) {
        log::Warn("malformed/unknown packet (id %u), disconnecting peer", static_cast<unsigned>(id));
        enet_peer_disconnect_later(peer, 0);
    }
}

std::string Server::AnnouncedLang(const std::string& lang) const { return lang.empty() ? cfg_.lang : loc::Locale::Normalize(lang); }

void Server::OnLogin(ENetPeer* peer, const net::LoginReq& req) {
    const std::string lang = AnnouncedLang(req.lang);
    auto fail = [&](const char* key, const char* english) {
        log::Info("login rejected: %s", english);
        SendRaw(peer, net::Pack(net::LoginFail{loc_.TrOr(lang, key, english)}));
        enet_peer_disconnect_later(peer, 0);
    };
    if (req.version != kProtocolVersion) return fail("auth.version_mismatch", "protocol version mismatch");
    if (!cfg_.allowGuest) return fail("auth.guest_disabled", gnet::AuthCodeText(gnet::AuthCode::GuestDisabled));
    if (!ValidName(req.name)) return fail("auth.invalid_name", "invalid name (2-16 letters, digits or _)");
    if (world_.FindPlayerByName(req.name)) return fail("auth.name_in_use", "name already in use");
    if (accounts_.Exists(req.name)) return fail("auth.name_registered", "that name belongs to a registered account");
    if (!StartSession(peer, req.name, nullptr, std::string(), lang)) enet_peer_disconnect_later(peer, 0);
}

bool Server::StartSession(ENetPeer* peer, const std::string& name, const nlohmann::json* saved, const std::string& account, const std::string& lang) {
    auto fail = [&](const char* key, const char* english) {
        log::Info("login rejected: %s", english);
        SendRaw(peer, net::Pack(net::LoginFail{loc_.TrOr(lang, key, english)}));
        return false;
    };
    if (world_.FindPlayerByName(name)) return fail("auth.name_in_use", "name already in use");
    const bool restore = saved && saved->is_object() && saved->contains("map");
    if (!world_.FirstMap()) return fail("auth.no_map", "server has no map");

    Critter* c = world_.CreateCritter(name, CritterKind::Player);
    c->peer = peer;
    c->lang = lang.empty() ? cfg_.lang : lang;
    c->account = account;
    c->color = ColorFromName(name);
    peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(c->id));
    if (data_.Loaded()) {
        if (const Proto* pc = data_.FindProto("Default_PC.CRT")) InitCritterStats(*c, *pc);
        c->team = 1;
        c->script.clear();
        c->dialog.clear();
    }
    if (restore) ApplyCharacter(*c, *saved);
    else if (!account.empty() && rt_.RequireCharacterCreation) {   // new account character: SPECIAL 5 everywhere + free points to allocate
        c->created = false;
        for (int i = 0; i < 7; i++) c->special[i] = rt_.SpecialBase;
        c->unspentSpecial = rt_.CharPoints;
        RecalcDerived(*c);
        c->hp = c->maxHp;
    }

    if (!c->created) {
        // Character creation BEFORE the world: the critter exists (so the session can be addressed) but is not placed on any map,
        // therefore invisible, never ticked and never damaged. The client gets LoginOk + Stats + CharSheet (created = 0) and shows
        // the creation screen; EnterWorld runs from CreateCharacter once a valid CharCreate arrived.
        if (restore) creatingSaved_[c->id] = *saved;   // legacy save made in the old flow: keep its map / position for afterwards
        net::LoginOk ok;
        ok.critterId = c->id;
        ok.tickRate = static_cast<uint16_t>(cfg_.tickRate);
        ok.moveTicks = static_cast<uint16_t>(cfg_.moveTicks);
        SendMsg(*c, ok);
        SendContentDefs(*c);
        SendStats(*c);
        SendCharSheet(*c);
        log::Info("player '%s' (id %u) logged in, waiting in character creation (not in the world)", c->name.c_str(), c->id);
        return true;
    }
    net::LoginOk ok;
    ok.critterId = c->id;
    ok.tickRate = static_cast<uint16_t>(cfg_.tickRate);
    ok.moveTicks = static_cast<uint16_t>(cfg_.moveTicks);
    SendMsg(*c, ok);
    SendContentDefs(*c);
    EnterWorld(*c, restore ? saved : nullptr);
    return true;
}

void Server::EnterWorld(Critter& crit, const nlohmann::json* saved) {
    Critter* c = &crit;
    ENetPeer* peer = c->peer;
    const std::string name = c->name;
    const std::string account = c->account;
    const bool restore = saved && saved->is_object() && saved->contains("map");
    Map* m = nullptr;
    if (restore) m = world_.FindMapByName(saved->value("map", std::string()));
    if (!m) m = data_.Loaded() ? world_.FindMapByName(data_.StartMap()) : nullptr;
    if (!m) m = world_.FirstMap();
    if (!m) { TellK(*c, "msg.no_map"); return; }
    TilePos spot = m->spawn;
    Dir dir = c->dir;
    bool placed = false;
    if (restore && m->InBounds(saved->value("x", -1), saved->value("y", -1))) {
        TilePos want{static_cast<int16_t>(saved->value("x", 0)), static_cast<int16_t>(saved->value("y", 0))};
        if (!m->IsBlocked(want.x, want.y) && m->IsFree(want)) {
            spot = want;
            dir = static_cast<Dir>(saved->value("dir", 0) & 7);
            placed = true;
        }
    }
    if (!placed) {
        spot = m->spawn;
        if (!PickEntryPos(*m, data_.Loaded() ? data_.StartEntry() : 0, spot, dir)) m->FindFreeNear(m->spawn, 10, spot);
    }
    c->dir = dir;
    PlaceCritter(*c, *m, spot);

    SendMapInfo(*c, *m);
    SendStats(*c);
    SendCharSheet(*c);
    SendInventory(*c);
    if (c->dead) SendCritterState(*c);

    CritterId cid = c->id;
    if (!scripting_->OnLogin(*c)) {
        log::Info("login of '%s' vetoed by script", name.c_str());
        TellK(*c, "msg.login_refused");
        c->account.clear();   // never overwrite a saved character with a vetoed session
        OnDisconnect(peer);
        enet_peer_disconnect_later(peer, 0);
        return;
    }
    if (Critter* cc = world_.GetCritter(cid)) {
        log::Info("player '%s' (id %u%s) entered map %u at %d,%d", cc->name.c_str(), cc->id, account.empty() ? ", guest" : restore ? ", restored" : ", new", cc->mapId, cc->pos.x, cc->pos.y);
        if (Map* mm = world_.GetMap(cc->mapId)) UpdateVisibility(*mm);
        PipOnLogin(*cc);
        SavePlayer(*cc);
    }
}

void Server::OnDisconnect(ENetPeer* peer) {
    Critter* c = PeerCritter(world_, peer);
    peer->data = nullptr;
    if (!c) return;
    CritterId id = c->id;
    if (IsCreating(*c)) {   // left during character creation: never was in the world, nothing to save (the account stays "new")
        log::Info("player '%s' left during character creation", c->name.c_str());
        creatingSaved_.erase(id);
        c->peer = nullptr;
        RemoveCritter(id);
        return;
    }
    log::Info("player '%s' left", c->name.c_str());
    if (!c->account.empty()) Audit("logout", c->name, {{"map", c->mapId}, {"level", c->level}});
    c->peer = nullptr;
    scripting_->OnLogout(*c);
    PipOnLogout(*c);
    SavePlayer(*c);
    RemoveCritter(id);
}

void Server::OnMoveReq(Critter& c, const net::MoveToReq& req) {
    if (c.msgBudget <= 0) return;
    --c.msgBudget;
    Map* m = world_.GetMap(c.mapId);
    if (!m || c.dead) return;
    const gnet::ActionResult gate = CheckAction(c, static_cast<uint8_t>(Msg::C2S_MoveTo), 0);
    if (gate == gnet::ActionResult::Busy) {   // WaitState (Busy) was sent; the walk starts when the wait is over
        c.pending = Pending{}; c.talkTarget = 0;
        c.target = 0;
        c.qmove = Critter::QueuedMove{true, false, false, static_cast<float>(req.x), static_cast<float>(req.y), NowMs()};
        return;
    }
    if (gate != gnet::ActionResult::Ok) {
        if (!(c.caps & gnet::kCapContinuousMove)) { net::CritterMove mv{c.id, c.pos.x, c.pos.y, c.dir, 0}; SendMsg(c, mv); }
        return;  // dead / out of AP: rejected
    }
    DoMoveCell(c, TilePos{req.x, req.y});
}

void Server::DoMoveCell(Critter& c, TilePos target) {
    c.qmove.has = false;
    c.pending = Pending{}; c.talkTarget = 0;
    c.target = 0;
    if (!MoveCritterTo(c, target)) {
        // Tell the client where it really is so it can drop any local assumption.
        if (c.caps & gnet::kCapContinuousMove) { if (c.mv.active) SendMovePath(c, &c); else SendCritterStop(c, 1, &c); }
        else { net::CritterMove mv{c.id, c.pos.x, c.pos.y, c.dir, 0}; SendMsg(c, mv); }
    }
}

void Server::OnMoveReqF(Critter& c, const gnet::MoveToF& req) {
    if (c.msgBudget <= 0) return;
    --c.msgBudget;
    const gnet::ActionResult gate = CheckAction(c, static_cast<uint8_t>(GMsgRt::C2S_MoveToF), 0);
    if (gate == gnet::ActionResult::Busy) {   // WaitState (Busy) was sent; the walk starts when the wait is over
        c.pending = Pending{}; c.talkTarget = 0;
        c.target = 0;
        c.qmove = Critter::QueuedMove{true, true, req.run != 0, req.x, req.y, NowMs()};
        return;
    }
    if (gate != gnet::ActionResult::Ok) return;
    DoMoveF(c, req.x, req.y, req.run != 0);
}

void Server::DoMoveF(Critter& c, float x, float y, bool run) {
    c.qmove.has = false;
    c.pending = Pending{}; c.talkTarget = 0;
    c.target = 0;
    if (run && !rt_.RunOnCombat && NowMs() < c.battleEndMs) run = false;
    if (!StartMove(c, x, y, run, true)) { if (c.mv.active) SendMovePath(c, &c); else SendCritterStop(c, 1, &c); }
}

void Server::RunQueuedMove(Critter& c) {
    if (!c.qmove.has || NowMs() < c.waitEndMs) return;
    const Critter::QueuedMove q = c.qmove;
    c.qmove.has = false;
    if (NowMs() - q.atMs > 3000) return;   // stale
    CritterId id = c.id;
    if (q.f) DoMoveF(c, q.x, q.y, q.run);
    else DoMoveCell(c, TilePos{static_cast<int16_t>(q.x), static_cast<int16_t>(q.y)});
    (void)id;
}

void Server::OnChatReq(Critter& c, const std::string& raw) {
    if (c.msgBudget <= 0) return;
    --c.msgBudget;
    std::string text = SanitizeText(raw, kMaxChatLen);
    if (text.empty()) return;
    CritterId id = c.id;
    if (LangChat(c, text)) return;      // /lang
    if (ContentChat(c, text)) return;   // /perks /perk /traits /reload
    if (PipChat(c, text)) return;   // /friend /friends /mail /inbox /maildel /quests
    if (!scripting_->OnChat(c, text)) return;
    text = SanitizeText(text, kMaxChatLen);
    if (text.empty()) return;
    if (Critter* cc = world_.GetCritter(id)) Say(*cc, text);
}

net::CritterAdd Server::MakeAdd(const Critter& c) const {
    net::CritterAdd a;
    a.id = c.id; a.name = c.name; a.kind = c.kind; a.x = c.pos.x; a.y = c.pos.y; a.dir = c.dir; a.color = c.color;
    return a;
}

void Server::SendMapInfo(Critter& player, const Map& m) {
    net::MapInfo mi;
    mi.mapId = m.id; mi.name = m.name;
    mi.width = static_cast<uint16_t>(m.width); mi.height = static_cast<uint16_t>(m.height);
    mi.blocked = m.blocked;
    mi.viewCells = static_cast<uint16_t>(ViewCells(player));
    SendMsg(player, mi);
}

// ------------------------------------------------------------- game ops

Map* Server::CreateMap(const std::string& name, int w, int h, std::vector<uint8_t> blocked, TilePos spawn) {
    Map* m = world_.CreateMap(name, w, h, std::move(blocked), spawn);
    log::Info("map %u '%s' created (%dx%d)", m->id, m->name.c_str(), w, h);
    return m;
}

void Server::PlaceCritter(Critter& c, Map& m, TilePos pos) {
    c.mapId = m.id;
    c.pos = pos;
    c.SnapToCell();
    c.mv.active = false;
    c.visCritters.clear();
    c.visItems.clear();
    m.critters.push_back(&c);
    m.dirty = true;
}

void Server::UnplaceCritter(Critter& c) {
    Map* m = world_.GetMap(c.mapId);
    if (!m) return;
    m->critters.erase(std::remove(m->critters.begin(), m->critters.end(), &c), m->critters.end());
    for (Critter* p : m->critters)
        if (p->IsPlayer() && p->visCritters.erase(c.id)) SendMsg(*p, net::CritterRemove{c.id});
    m->dirty = true;
    c.mapId = 0;
    c.mv.active = false;
    c.visCritters.clear();
    c.visItems.clear();
}

Critter* Server::SpawnNpc(MapId mapId, const std::string& name, TilePos pos, const std::string& type, uint32_t color) {
    Map* m = world_.GetMap(mapId);
    if (!m || !m->InBounds(pos.x, pos.y)) { log::Warn("spawn_npc: bad map/position"); return nullptr; }
    TilePos spot = pos;
    if (!m->IsFree(pos) && !m->FindFreeNear(pos, 5, spot)) { log::Warn("spawn_npc: no free tile near %d,%d", pos.x, pos.y); return nullptr; }
    Critter* c = world_.CreateCritter(name, CritterKind::Npc);
    c->npcType = type;
    c->color = color;
    PlaceCritter(*c, *m, spot);
    return c;
}

ItemId Server::SpawnItem(MapId mapId, uint16_t pid, const std::string& name, TilePos pos) {
    Map* m = world_.GetMap(mapId);
    if (!m || !m->InBounds(pos.x, pos.y)) return 0;
    Item it;
    it.id = world_.NextItemId();
    it.pid = pid; it.name = name; it.mapId = mapId; it.pos = pos;
    m->items[it.id] = it;
    m->dirty = true;
    return it.id;
}

void Server::RemoveCritter(CritterId id) {
    Critter* c = world_.GetCritter(id);
    if (!c) return;
    if (amx_) amx_->DetachCritter(id);
    CloseDataDialogsOf(id);
    UnplaceCritter(*c);
    if (c->peer) {
        c->peer->data = nullptr;
        enet_peer_disconnect_later(c->peer, 0);
    }
    world_.DestroyCritter(id);
}

bool Server::Teleport(Critter& c, MapId mapId, TilePos pos) {
    Map* dst = world_.GetMap(mapId);
    if (!dst || !dst->InBounds(pos.x, pos.y) || dst->IsBlocked(pos.x, pos.y)) return false;
    TilePos spot = pos;
    Critter* occ = dst->CritterAt(pos);
    if (occ && occ != &c && !dst->FindFreeNear(pos, 3, spot)) return false;

    if (c.mapId == mapId) {
        c.pos = spot;
        c.SnapToCell();
        c.mv.active = false;
        dst->dirty = true;
        SendLegacyMove(c, 1);       // snap: cancels any path on every client
        SendCritterStop(c, 2);
        return true;
    }
    UnplaceCritter(c);
    PlaceCritter(c, *dst, spot);
    if (c.IsPlayer()) SendMapInfo(c, *dst);
    return true;
}

void Server::Say(Critter& speaker, const std::string& text) {
    Map* m = world_.GetMap(speaker.mapId);
    if (!m) return;
    net::ChatMsg msg;
    msg.from = speaker.id; msg.name = speaker.name; msg.text = text; msg.channel = ChatChannel::Say;
    ByteBuffer buf = net::Pack(msg);
    std::vector<CritterId> npcsNear;
    for (Critter* p : m->critters) {
        if (!InRange(p->pos, speaker.pos, std::max(ViewCells(*p), ViewCells(speaker))) && p != &speaker) continue;
        if (p->IsPlayer()) Send(*p, buf);
        else if (p != &speaker && TileDist(p->pos, speaker.pos) <= 24) npcsNear.push_back(p->id);
    }
    // NPCs only react to players: avoids NPC<->NPC feedback loops.
    if (speaker.IsPlayer()) {
        CritterId sid = speaker.id;
        for (CritterId nid : npcsNear) {
            Critter* npc = world_.GetCritter(nid);
            Critter* sp = world_.GetCritter(sid);
            if (npc && sp) scripting_->OnNpcHeard(*npc, *sp, text);
        }
    }
}

void Server::Tell(Critter& player, const std::string& text) {
    net::ChatMsg msg;
    msg.text = text;
    if (msg.text.size() > 1000) {   // the wire limit is 1024 bytes: cut at a code point boundary
        size_t cut = 1000;
        while (cut > 0 && (static_cast<unsigned char>(msg.text[cut]) & 0xC0) == 0x80) --cut;
        msg.text.resize(cut);
    }
    msg.channel = ChatChannel::System;
    SendMsg(player, msg);
}

void Server::BroadcastK(const std::string& key, loc::Args args) {
    for (auto& [id, c] : world_.Critters())
        if (c->IsPlayer() && c->mapId != 0) TellK(*c, key, args);
}

// /lang [code]: shows or changes the language of this player's server messages (the client announces its own language at login).
bool Server::LangChat(Critter& c, const std::string& text) {
    if (text.size() < 5 || text.compare(0, 5, "/lang") != 0 || (text.size() > 5 && text[5] != ' ')) return false;
    std::string list;
    for (const std::string& l : loc_.Languages()) list += (list.empty() ? "" : ", ") + l;
    const std::string want = text.size() > 6 ? loc::Locale::Normalize(text.substr(6)) : std::string();
    if (want.empty()) TellK(c, "msg.lang.current", {{"lang", c.lang}, {"list", list}});
    else if (!loc_.Has(want)) TellK(c, "msg.lang.unknown", {{"lang", text.substr(6)}, {"list", list}});
    else {
        c.lang = want;
        TellK(c, "msg.lang.set", {{"lang", want}});
        SendContentDefs(c);   // perk / trait names follow the language
        SendPip(c);           // so do the quest titles
    }
    return true;
}

std::string Server::Stf(const Critter& c, int id) const {
    if (const std::string* o = loc_.Find(LangOf(c), "stf." + std::to_string(id))) return *o;
    return data_.String(id);
}

std::string Server::SkillName(const Critter& c, int skill) const {
    if (skill < 0 || skill >= kSkillCount) return "?";
    return loc_.TrOr(LangOf(c), std::string("skill.") + kSkillNames[skill] + ".name", kSkillNames[skill]);
}

bool Server::LoadLocale() {
    std::filesystem::path dir = cfg_.localeDir;
    if (dir.empty() && !cfg_.contentDir.empty() && std::filesystem::exists(cfg_.contentDir / "locale")) dir = cfg_.contentDir / "locale";
    if (dir.empty()) dir = loc::Locale::FindDir(cfg_.scriptsDir);
    std::string err;
    if (dir.empty()) { log::Warn("localization: no data/locale folder found, messages show their keys"); return false; }
    const bool ok = loc_.Load(dir, &err);
    if (!ok) log::Warn("localization: %s", err.c_str());
    cfg_.lang = loc::Locale::Normalize(cfg_.lang);
    std::string langs;
    for (const auto& l : loc_.Languages()) langs += (langs.empty() ? "" : ", ") + l;
    log::Info("localization: %s (languages: %s; default %s)", dir.string().c_str(), langs.c_str(), cfg_.lang.c_str());
    return ok;
}

void Server::Broadcast(const std::string& text) {
    for (auto& [id, c] : world_.Critters())
        if (c->IsPlayer() && c->mapId != 0) Tell(*c, text);
}

// ----------------------------------------------------------------- tick

void Server::StepCritter(Critter& c) {
    if (c.dead || c.isStatic) { c.qmove.has = false; StopMove(c, 1); return; }
    if (c.qmove.has) RunQueuedMove(c);
    if (!c.mv.active && c.IsPlayer() && c.inRegion < 0 && NowMs() >= c.regionLockUntilMs && NowMs() >= c.battleEndMs) {   // standing in a transition after the lock/battle flag ran out
        const CritterId rid = c.id; const MapId rm = c.mapId;
        CheckRegions(c);
        Critter* rc = world_.GetCritter(rid);
        if (!rc || rc->mapId != rm) return;
    }
    if (c.mv.active) {
        CritterId id = c.id;
        AdvanceMove(c, NowMs());
        Critter* cc = world_.GetCritter(id);
        if (!cc || cc->mv.active || cc->mapId == 0) return;
        if (cc->pending.type != PendingType::None) RunPending(*cc);
        if (cc->talkTarget) RunTalk(*cc);
        return;
    }
    if (c.pending.type != PendingType::None) RunPending(c);
    if (c.talkTarget) RunTalk(c);
}

void Server::UpdateVisibility(Map& m) {
    m.dirty = false;
    for (Critter* p : m.critters) {
        if (!p->IsPlayer()) continue;
        std::unordered_set<CritterId> now;
        for (Critter* c : m.critters)
            if (c == p || InRange(c->pos, p->pos, ViewCells(*p))) now.insert(c->id);
        // self first so the client always knows who it is before anyone else
        if (!p->visCritters.count(p->id)) {
            SendMsg(*p, MakeAdd(*p));
            if (p->protoPid >= 0) Send(*p, gnet::PackG(gnet::CritterProto{p->id, static_cast<uint16_t>(p->protoPid)}));
            SendAppearance(*p, p);
        }
        for (Critter* c : m.critters)
            if (c != p && now.count(c->id) && !p->visCritters.count(c->id)) {
                SendMsg(*p, MakeAdd(*c));
                if (c->protoPid >= 0) Send(*p, gnet::PackG(gnet::CritterProto{c->id, static_cast<uint16_t>(c->protoPid)}));
                SendAppearance(*c, p);
                if (p->caps & gnet::kCapContinuousMove) {
                    SendCritterStop(*c, 3, p);  // exact float position (CritterAdd only has the cell)
                    if (c->mv.active) SendMovePath(*c, p);
                }
                if (c->dead || c->hp != c->maxHp) {
                    gnet::CritterState st{c->id, static_cast<int16_t>(c->hp), static_cast<int16_t>(c->maxHp), static_cast<uint8_t>(c->dead), 1};   // appear = 1: dead critters come in already dead (final pose)
                    Send(*p, gnet::PackG(st));
                }
            }
        for (CritterId old : p->visCritters)
            if (!now.count(old)) SendMsg(*p, net::CritterRemove{old});
        p->visCritters = std::move(now);

        std::unordered_set<ItemId> nowItems;
        for (auto& [iid, it] : m.items)
            if (InRange(it.pos, p->pos, ViewCells(*p)) && !ObjectHiddenFor(*p, it)) nowItems.insert(iid);   // hidden_until of a data/objects binding
        for (ItemId iid : nowItems)
            if (!p->visItems.count(iid)) {
                const Item& it = m.items[iid];
                net::ItemAdd ia;
                ia.id = it.id; ia.pid = it.pid; ia.name = ObjectName(*p, it); ia.x = it.pos.x; ia.y = it.pos.y;
                SendMsg(*p, ia);
                if (it.kind != ObjKind::Ground) SendObjectInfo(*p, it);
            }
        for (ItemId old : p->visItems)
            if (!nowItems.count(old)) SendMsg(*p, net::ItemRemove{old});
        p->visItems = std::move(nowItems);
    }
}

void Server::DoTick() {
    ++tick_;
    if (cfg_.saveIntervalSec > 0 && tick_ >= nextSaveTick_) {
        nextSaveTick_ = tick_ + static_cast<uint64_t>(cfg_.saveIntervalSec) * cfg_.tickRate;
        SaveAllPlayers();
        SaveWorldState();
    }
    std::vector<CritterId> ids;
    ids.reserve(world_.Critters().size());
    for (auto& [id, c] : world_.Critters()) ids.push_back(id);

    // Only maps with players are simulated (NPC scripts, combat, movement).
    std::unordered_set<MapId> active;
    for (auto& m : world_.Maps())
        for (Critter* p : m->critters)
            if (p->IsPlayer()) { active.insert(m->id); break; }

    for (CritterId id : ids) {
        Critter* c = world_.GetCritter(id);
        if (c && c->IsPlayer() && tick_ % 5 == 0 && c->msgBudget < 10) ++c->msgBudget;   // also while waiting in character creation
        if (!c || c->mapId == 0) continue;
        if (!active.count(c->mapId)) continue;
        if (c->dead) {
            if (c->IsPlayer() && tick_ - c->deadSince >= static_cast<uint64_t>(cfg_.tickRate) * 5) Respawn(*c);
            else if (c->IsNpc() && tick_ - c->deadSince >= static_cast<uint64_t>(cfg_.tickRate) * 90) RemoveCritter(id);
            continue;
        }
        StepCritter(*c);
        if (Critter* cc = world_.GetCritter(id)) TickCombat(*cc);
    }
    for (CritterId id : ids) {
        Critter* c = world_.GetCritter(id);
        if (c && c->IsNpc() && c->mapId != 0 && !c->dead && active.count(c->mapId)) scripting_->OnNpcTick(*c, tick_);
    }
    if (tick_ % 10 == 0)
        for (MapId mid : active)
            if (Map* dm = world_.GetMap(mid)) TickObjects(*dm);
    TickDataDialogs();
    if (amx_) amx_->Tick(tick_, active);
    scripting_->OnTick(tick_);
    if (tick_ % static_cast<uint64_t>(cfg_.tickRate) == 0) scripting_->PollConfig();  // hot reload of config.lua

    for (auto& m : world_.Maps())
        if (m->dirty) UpdateVisibility(*m);
    enet_host_flush(host_);
}

// ------------------------------------------------------------- console

void Server::ProcessCommands() {
    std::vector<std::string> cmds;
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lk(cmdMutex_);
        cmds.swap(commands_);
        tasks.swap(tasks_);
    }
    for (auto& c : cmds) ExecCommand(c);
    for (auto& t : tasks) t();
}

void Server::DbCommand(const std::string& cmd, std::istringstream& ss) {
    if (!store_.IsOpen()) { log::Info("no database"); return; }
    if (cmd == "dbinfo") {
        const DbInfo d = store_.Info();
        log::Info("database %s: schema v%d, journal %s, %lld bytes (+ %lld WAL), %lld pages of %lld (%lld free)", d.path.c_str(), d.schemaVersion, d.journalMode.c_str(),
                  static_cast<long long>(d.fileBytes), static_cast<long long>(d.walBytes), static_cast<long long>(d.pageCount), static_cast<long long>(d.pageSize),
                  static_cast<long long>(d.freePages));
        std::string line;
        for (const TableCount& t : d.tables) line += (line.empty() ? "" : ", ") + t.name + " " + std::to_string(t.rows);
        log::Info("rows: %s", line.c_str());
        return;
    }
    int n = 20;   // events [n] [kind] [actor]
    std::string kind, actor, tok;
    if (ss >> tok) { if (std::isdigit(static_cast<unsigned char>(tok[0]))) { n = std::max(1, std::atoi(tok.c_str())); ss >> kind; } else kind = tok; }
    ss >> actor;
    const auto rows = store_.RecentEvents(n, kind, actor);
    log::Info("%zu event(s) (newest first, of %lld)", rows.size(), static_cast<long long>(store_.EventCount()));
    for (const EventRow& e : rows) {
        char tb[32] = "?";
        const std::time_t t = static_cast<std::time_t>(e.ts);
        if (const std::tm* tm = std::localtime(&t)) std::strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", tm);
        log::Info("  #%lld %s %-12s %-16s %s", static_cast<long long>(e.id), tb, e.kind.c_str(), e.actor.c_str(), e.data.c_str());
    }
}

void Server::ExecCommand(const std::string& line) {
    std::istringstream ss(line);
    std::string cmd;
    ss >> cmd;
    if (cmd.empty()) return;
    if (cmd == "reload") {   // reload [data|scripts|all] (default all)
        std::string what;
        ss >> what;
        const std::string rep = ReloadAll(what);
        std::istringstream ls(rep);
        for (std::string l; std::getline(ls, l);) log::Info("%s", l.c_str());
    } else if (cmd == "status") {
        size_t players = 0, npcs = 0;
        for (auto& [id, c] : world_.Critters()) (c->IsPlayer() ? players : npcs)++;
        log::Info("tick %llu, %zu player(s), %zu npc(s), %zu map(s)", static_cast<unsigned long long>(tick_), players, npcs, world_.Maps().size());
        for (auto& [id, c] : world_.Critters())
            if (c->IsPlayer()) log::Info("  #%u %s map %u at %d,%d", c->id, c->name.c_str(), c->mapId, c->pos.x, c->pos.y);
    } else if (cmd == "amx") {  // amx <script name without extension> <public>   (traces every native call)
        std::vector<std::string> tok;
        for (std::string t; ss >> t;) tok.push_back(t);
        std::string sc, pub;
        if (tok.size() >= 2) { pub = tok.back(); for (size_t i = 0; i + 1 < tok.size(); ++i) sc += (i ? " " : "") + tok[i]; }  // script names may contain spaces
        if (!amx_ || sc.empty() || pub.empty()) log::Info("usage: amx <script> <public>   (needs game data); %zu amx instances", amx_ ? amx_->Count() : 0);
        else log::Info("amx %s.%s: %s", sc.c_str(), pub.c_str(), amx_->RunTest(sc, pub).c_str());
    } else if (cmd == "dbinfo" || cmd == "events") {
        DbCommand(cmd, ss);
    } else if (cmd == "say") {
        std::string rest;
        std::getline(ss, rest);
        Broadcast("[server]" + rest);
    } else if (cmd == "quit" || cmd == "stop" || cmd == "exit") {
        RequestStop();
    } else {
        log::Info("commands: reload [data|scripts|all] | status | dbinfo | events [n] [kind] | amx <script> <public> | say <text> | quit");
    }
}

}  // namespace f3
