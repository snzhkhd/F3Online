#include "ClientCore.h"

#include <enet/enet.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include "common/log.h"

namespace f3 {

ClientCore::ClientCore() {}
ClientCore::~ClientCore() { Disconnect(); }

double ClientCore::Now() const {
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    return std::chrono::duration<double>(clock::now() - t0).count();
}

bool ClientCore::Connect(const std::string& host, uint16_t port, const std::string& name, const std::string& password, bool registerAccount) {
    Disconnect();
    if (enet_initialize() != 0) { failReason_ = "enet_initialize failed"; state_ = State::Failed; return false; }
    host_ = enet_host_create(nullptr, 1, 2, 0, 0);
    if (!host_) { failReason_ = "cannot create client host"; state_ = State::Failed; return false; }
    ENetAddress addr{};
    if (enet_address_set_host(&addr, host.c_str()) != 0) { failReason_ = "cannot resolve host"; state_ = State::Failed; return false; }
    addr.port = port;
    peer_ = enet_host_connect(host_, &addr, 2, 0);
    if (!peer_) { failReason_ = "no free peer slot"; state_ = State::Failed; return false; }
    name_ = name;
    password_ = password;
    register_ = registerAccount;
    state_ = State::Connecting;
    failReason_.clear();
    return true;
}

void ClientCore::Disconnect() {
    if (host_) {
        if (peer_ && state_ != State::Disconnected && state_ != State::Failed) {
            enet_peer_disconnect_now(peer_, 0);
        }
        enet_host_flush(host_);
        enet_host_destroy(host_);
        enet_deinitialize();
    }
    host_ = nullptr;
    peer_ = nullptr;
    if (state_ != State::Failed) state_ = State::Disconnected;
    critters_.clear();
    items_.clear();
    pendingInfo_.clear();
    floaters_.clear();
    inventory_.clear();
    container_.clear();
    containerOwner_ = 0;
    hasStats_ = false;
    hasSheet_ = false;
    serverTiming_ = false;
    wait_ = Wait{};
    map_ = ClientMap{};
    selfId_ = 0;
}

void ClientCore::Send(const ByteBuffer& b) {
    if (!peer_) return;
    ENetPacket* p = enet_packet_create(b.Data(), b.Size(), ENET_PACKET_FLAG_RELIABLE);
    if (p) enet_peer_send(peer_, 0, p);
}

void ClientCore::Poll() {
    if (!host_) return;
    ENetEvent ev;
    while (enet_host_service(host_, &ev, 0) > 0) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                if (password_.empty()) {
                    net::LoginReq req;
                    req.name = name_;
                    Send(net::Pack(req));
                } else {
                    gnet::Auth a;
                    a.mode = static_cast<uint8_t>(register_ ? gnet::AuthMode::Register : gnet::AuthMode::Login);
                    a.name = name_;
                    a.password = password_;
                    Send(gnet::PackG(a));
                    password_.assign(password_.size(), '\0'); password_.clear();   // not kept longer than needed
                }
                state_ = State::LoggingIn;
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE:
                Handle(ev.packet->data, ev.packet->dataLength);
                enet_packet_destroy(ev.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                if (state_ == State::Connecting) failReason_ = "connection failed";
                else if (state_ != State::Failed) failReason_ = failReason_.empty() ? "disconnected from server" : failReason_;
                state_ = State::Failed;
                peer_ = nullptr;
                break;
            default: break;
        }
    }
    if (state_ == State::Connecting && !peer_) state_ = State::Failed;

    // housekeeping: drop consumed track keys, expire floaters
    const double now = Now();
    for (auto& [id, c] : critters_) {
        auto& tr = c.track;
        while (tr.size() > 2 && tr[1].t <= now - 0.25) tr.erase(tr.begin());
        if (tr.size() == 2 && tr[1].t <= now - 0.25) {  // finished: stand still at the end
            c.offX = c.offY = 0;
            tr.erase(tr.begin());
        }
    }
    floaters_.erase(std::remove_if(floaters_.begin(), floaters_.end(), [&](const Floater& f) { return now - f.time > 1.6; }), floaters_.end());
    // the loot window closes when the player walks away from the container / corpse
    if (containerOwner_ != 0 && state_ == State::InGame) {
        const ClientCritter* me = self();
        TilePos tp{};
        bool have = false;
        if (containerCritter_) { auto it = critters_.find(containerOwner_); if (it != critters_.end()) { tp = it->second.pos; have = true; } }
        else { auto it = items_.find(containerOwner_); if (it != items_.end()) { tp = it->second.pos; have = true; } }
        if (me && have && std::max(std::abs(me->pos.x - tp.x), std::abs(me->pos.y - tp.y)) > 6) CloseContainer();
    }
}

void ClientCore::AddChat(ChatLine l) {
    l.time = Now();
    chat_.push_back(std::move(l));
    ++chatCounter_;
    while (chat_.size() > kChatHistory) chat_.pop_front();
}

void ClientCore::OnLoginOk() {
    state_ = State::InGame;
    if (!legacyOnly_) {
        gnet::ClientCaps caps;
        caps.caps = gnet::kCapContinuousMove;
        Send(gnet::PackG(caps));
    }
    Send(gnet::PackG(gnet::ReqState{}));
}

void ClientCore::ResolvePid(ClientCritter& c) {
    if (c.pid >= 0 || !protos_.loaded()) return;
    const ProtoInfo* p = c.kind == CritterKind::Player ? protos_.PlayerProto() : protos_.CritterByName(c.name);
    c.pid = protos_.PidOf(p);
}

void ClientCore::SetTrackSnap(ClientCritter& c, float x, float y) {
    c.track.clear();
    c.track.push_back(TrackKey{x, y, Now()});
    c.offX = c.offY = 0;
    c.offT = -10;
}

// Replaces the polyline while keeping the drawn position continuous (the difference fades out over kBlendTime).
void ClientCore::ReplaceTrack(ClientCritter& c, std::vector<TrackKey> keys) {
    const double now = Now();
    CritterPose before = Pose(c, now);
    c.track = std::move(keys);
    CritterPose after = Pose(c, now);
    float dx = before.x - after.x, dy = before.y - after.y;
    if (std::fabs(dx) + std::fabs(dy) > 3.0f) { c.offX = c.offY = 0; c.offT = -10; }  // too far: snap
    else { c.offX = dx; c.offY = dy; c.offT = now; }
}

void ClientCore::Handle(const uint8_t* data, size_t len) {
    if (len == 0) return;
    ByteBuffer b(data, len);
    const uint8_t rawId = b.ReadU8();
    if (rawId >= 120) { HandleGame(static_cast<GMsg>(rawId), b); return; }
    Msg id = static_cast<Msg>(rawId);
    switch (id) {
        case Msg::S2C_LoginOk: {
            net::LoginOk m;
            if (!m.Read(b)) return;
            selfId_ = m.critterId;
            stepDur_ = static_cast<double>(m.moveTicks) / std::max<int>(1, m.tickRate);
            OnLoginOk();
            break;
        }
        case Msg::S2C_LoginFail: {
            net::LoginFail m;
            if (m.Read(b)) { failReason_ = m.reason; state_ = State::Failed; }
            break;
        }
        case Msg::S2C_MapInfo: {
            net::MapInfo m;
            if (!m.Read(b)) return;
            map_.id = m.mapId; map_.name = m.name; map_.width = m.width; map_.height = m.height;
            map_.blocked = std::move(m.blocked);
            ++mapSerial_;
            critters_.clear();  // new map: everything gets re-announced by the visibility pass
            items_.clear();
            pendingInfo_.clear();
            floaters_.clear();
            containerOwner_ = 0;
            container_.clear();
            dialog_ = DialogState{};
            break;
        }
        case Msg::S2C_CritterAdd: {
            net::CritterAdd m;
            if (!m.Read(b)) return;
            ClientCritter c;
            c.id = m.id; c.name = m.name; c.kind = m.kind; c.color = m.color; c.dir = m.dir;
            c.pos = TilePos{m.x, m.y};
            ResolvePid(c);
            c.track.push_back(TrackKey{static_cast<float>(m.x), static_cast<float>(m.y), Now()});
            critters_[c.id] = std::move(c);
            break;
        }
        case Msg::S2C_CritterRemove: {
            net::CritterRemove m;
            if (m.Read(b)) {
                critters_.erase(m.id);
                if (containerCritter_ && containerOwner_ == m.id) CloseContainer();   // the corpse left the view
            }
            break;
        }
        case Msg::S2C_CritterMove: {
            net::CritterMove m;
            if (!m.Read(b)) return;
            auto it = critters_.find(m.id);
            if (it == critters_.end()) return;
            ClientCritter& c = it->second;
            TilePos to{m.x, m.y};
            c.dir = m.dir;
            const double now = Now();
            if (m.snap || TileDist(c.pos, to) > 2) {
                c.pos = to;
                SetTrackSnap(c, to.x, to.y);
                break;
            }
            c.pos = to;
            c.run = stepDur_ < 0.15;
            // Chain legacy steps into one continuous polyline, one step apart.
            CritterPose cur = Pose(c, now);
            std::vector<TrackKey> keys;
            const bool ongoing = c.track.size() >= 2 && now <= c.track.back().t + 0.6 * stepDur_;
            if (ongoing) {
                keys = c.track;
                double arrival = keys.back().t + stepDur_;
                if (arrival > now + 2.5 * stepDur_) arrival = now + 2.0 * stepDur_;  // backlog: catch up
                arrival = std::max(arrival, keys.back().t + 0.02);
                keys.push_back(TrackKey{static_cast<float>(to.x), static_cast<float>(to.y), arrival});
            } else {
                keys.push_back(TrackKey{cur.x, cur.y, now});
                keys.push_back(TrackKey{static_cast<float>(to.x), static_cast<float>(to.y), now + stepDur_});
            }
            c.track = std::move(keys);
            break;
        }
        case Msg::S2C_Chat: {
            net::ChatMsg m;
            if (!m.Read(b)) return;
            AddChat(ChatLine{m.name, m.text, m.channel, 0});
            break;
        }
        case Msg::S2C_ItemAdd: {
            net::ItemAdd m;
            if (!m.Read(b)) return;
            ClientItem it;
            it.id = m.id; it.pid = m.pid; it.name = m.name; it.pos = TilePos{m.x, m.y};
            if (auto pi = pendingInfo_.find(m.id); pi != pendingInfo_.end()) {
                it.kind = pi->second.kind; it.open = pi->second.open != 0;
                pendingInfo_.erase(pi);
            }
            items_[m.id] = std::move(it);
            break;
        }
        case Msg::S2C_ItemRemove: {
            net::ItemRemove m;
            if (m.Read(b)) {
                items_.erase(m.id); pendingInfo_.erase(m.id);
                if (!containerCritter_ && containerOwner_ == m.id) CloseContainer();
            }
            break;
        }
        case Msg::S2C_Pong: {
            net::Pong m;
            if (m.Read(b)) lastPong_ = m.token;
            break;
        }
        default: break;
    }
}

void ClientCore::HandleGame(GMsg id, ByteBuffer& b) {
    const double now = Now();
    switch (static_cast<uint8_t>(id)) {
        case static_cast<uint8_t>(GMsgAuth::S2C_AuthResult): {
            gnet::AuthResult m;
            if (!m.Read(b)) return;
            if (m.code != 0) { failReason_ = m.text; state_ = State::Failed; }
            break;
        }
        case static_cast<uint8_t>(GMsgChar::S2C_CharSheet): {
            gnet::CharSheet m;
            if (m.Read(b)) { sheet_ = m; hasSheet_ = true; }
            break;
        }
        case static_cast<uint8_t>(GMsg::S2C_Stats): {
            gnet::Stats m;
            if (!m.Read(b)) return;
            if (m.critterId == selfId_) { stats_ = m; hasStats_ = true; }
            auto it = critters_.find(m.critterId);
            if (it != critters_.end()) { it->second.hp = m.hp; it->second.maxHp = m.maxHp; }
            break;
        }
        case static_cast<uint8_t>(GMsg::S2C_Inventory): {
            gnet::Inventory m;
            if (!m.Read(b)) return;
            if (m.owner == selfId_ || selfId_ == 0) inventory_ = std::move(m.items);
            break;
        }
        case static_cast<uint8_t>(GMsgDlg::S2C_DialogOpen): {
            gnet::DialogOpen m;
            if (!m.Read(b)) return;
            dialog_ = DialogState{};
            dialog_.open = true; dialog_.npc = m.npc; dialog_.npcName = m.name;
            break;
        }
        case static_cast<uint8_t>(GMsgDlg::S2C_DialogNode): {
            gnet::DialogNode m;
            if (!m.Read(b)) return;
            dialog_.open = true; dialog_.npc = m.npc; dialog_.text = m.text;
            dialog_.replies.clear();
            for (auto& r : m.replies) dialog_.replies.push_back(r.text);
            break;
        }
        case static_cast<uint8_t>(GMsgDlg::S2C_DialogClose): dialog_ = DialogState{}; break;
        case static_cast<uint8_t>(GMsg::S2C_Container): {
            gnet::Container m;
            if (!m.Read(b)) return;
            containerOwner_ = m.owner;
            containerCritter_ = m.isCritter != 0;
            containerName_ = m.name;
            container_ = std::move(m.items);
            break;
        }
        case static_cast<uint8_t>(GMsg::S2C_ObjectInfo): {
            gnet::ObjectInfo m;
            if (!m.Read(b)) return;
            auto it = items_.find(m.id);
            if (it != items_.end()) { it->second.kind = m.kind; it->second.open = m.open != 0; }
            else pendingInfo_[m.id] = m;
            break;
        }
        case static_cast<uint8_t>(GMsg::S2C_CritterState): {
            gnet::CritterState m;
            if (!m.Read(b)) return;
            auto it = critters_.find(m.id);
            if (it == critters_.end()) return;
            ClientCritter& c = it->second;
            c.hp = m.hp; c.maxHp = m.maxHp;
            // appear: the critter just entered the view already dead -> final pose (deathT < 0), no death animation
            if (m.dead && !c.dead) { c.dead = true; c.deathT = m.appear ? -1 : now; }
            else if (!m.dead && c.dead) { c.dead = false; c.deathT = -1; }
            break;
        }
        case static_cast<uint8_t>(GMsg::S2C_Appearance): {
            gnet::Appearance m;
            if (!m.Read(b)) return;
            auto it = critters_.find(m.id);
            if (it == critters_.end()) return;
            ClientCritter& c = it->second;
            if (c.hasApp && c.appWeapon != m.weapon) c.equipT = now;   // not on the first message (appearing)
            c.hasApp = true; c.appBody = m.body; c.appHead = m.head; c.appWeapon = m.weapon;
            break;
        }
        case static_cast<uint8_t>(GMsg::S2C_CombatEvent): {
            gnet::CombatEvent m;
            if (!m.Read(b)) return;
            if (auto a = critters_.find(m.attacker); a != critters_.end()) { a->second.attackT = now; a->second.attackTarget = m.target; a->second.attackDur = m.durMs / 1000.0; }
            auto t = critters_.find(m.target);
            Floater f;
            f.target = m.target; f.time = now;
            switch (m.result) {
                case gnet::HitResult::Miss: f.text = "MISS"; f.kind = 1; break;
                case gnet::HitResult::Hit: f.text = "-" + std::to_string(m.damage); f.kind = 0; break;
                case gnet::HitResult::Critical: f.text = "CRIT -" + std::to_string(m.damage); f.kind = 2; break;
                case gnet::HitResult::Kill: f.text = "-" + std::to_string(m.damage); f.kind = 3; break;
            }
            floaters_.push_back(std::move(f));
            if (t != critters_.end()) {
                ClientCritter& c = t->second;
                if (m.result != gnet::HitResult::Miss) c.hitT = now;
                c.hp = m.targetHp;
                if (m.result == gnet::HitResult::Kill && !c.dead) { c.dead = true; c.deathT = now; }
            }
            break;
        }
        case static_cast<uint8_t>(GMsg::S2C_CellsSet): {
            gnet::CellsSet m;
            if (!m.Read(b)) return;
            for (const auto& c : m.cells)
                if (c.x >= 0 && c.y >= 0 && c.x < map_.width && c.y < map_.height)
                    map_.blocked[static_cast<size_t>(c.y) * map_.width + c.x] = c.blocked;
            break;
        }
        case static_cast<uint8_t>(GMsgRt::S2C_WaitState): {
            gnet::WaitState m;
            if (!m.Read(b)) return;
            serverTiming_ = true;
            auto conv = [&](uint32_t ms) { return now + static_cast<double>(static_cast<int32_t>(ms - m.nowMs)) / 1000.0; };
            wait_.valid = true;
            wait_.recvLocal = now;
            wait_.waitEnd = conv(m.waitEndMs);
            wait_.battleEnd = conv(m.battleEndMs);
            wait_.total = std::max(0.0, wait_.waitEnd - now);
            wait_.ap = m.ap;
            wait_.maxAp = m.maxAp;
            wait_.apRegenMs = std::max<uint32_t>(1, m.apRegenMs);
            wait_.result = m.result;
            wait_.action = m.action;
            break;
        }
        case static_cast<uint8_t>(GMsgRt::S2C_MovePath): {
            gnet::MovePath m;
            if (!m.Read(b)) return;
            serverTiming_ = true;
            auto it = critters_.find(m.id);
            if (it == critters_.end() || m.pts.size() < 2 || m.speed <= 0.001f) return;
            ClientCritter& c = it->second;
            const double age = std::max(0.0, static_cast<double>(static_cast<int32_t>(m.nowMs - m.startMs)) / 1000.0);
            double t = now - age;
            std::vector<TrackKey> keys;
            keys.reserve(m.pts.size());
            for (size_t i = 0; i < m.pts.size(); ++i) {
                const float x = m.pts[i].x - 0.5f, y = m.pts[i].y - 0.5f;  // cell units (centre +0.5) -> tile units
                if (i > 0) {
                    const float dx = x - keys.back().x, dy = y - keys.back().y;
                    t += std::sqrt(dx * dx + dy * dy) / m.speed;
                }
                keys.push_back(TrackKey{x, y, t});
            }
            const TrackKey& last = keys.back();
            c.pos = TilePos{static_cast<int16_t>(std::lround(last.x)), static_cast<int16_t>(std::lround(last.y))};
            c.run = m.run != 0;
            const TrackKey& pen = keys[keys.size() - 2];
            if (std::fabs(last.x - pen.x) + std::fabs(last.y - pen.y) > 1e-4f)
                c.dir = DirFromDelta(static_cast<int>(std::lround((last.x - pen.x) * 3)), static_cast<int>(std::lround((last.y - pen.y) * 3)), c.dir);
            ReplaceTrack(c, std::move(keys));
            break;
        }
        case static_cast<uint8_t>(GMsgRt::S2C_CritterStop): {
            gnet::CritterStop m;
            if (!m.Read(b)) return;
            serverTiming_ = true;
            auto it = critters_.find(m.id);
            if (it == critters_.end()) return;
            ClientCritter& c = it->second;
            const float x = m.x - 0.5f, y = m.y - 0.5f;
            c.pos = TilePos{static_cast<int16_t>(std::lround(x)), static_cast<int16_t>(std::lround(y))};
            c.dir = static_cast<Dir>(m.dir & 7);
            if (m.reason == 3) SetTrackSnap(c, x, y);
            else ReplaceTrack(c, {TrackKey{x, y, now}});
            break;
        }
        case static_cast<uint8_t>(GMsgView::S2C_CritterProto): {
            gnet::CritterProto m;
            if (!m.Read(b)) return;
            auto it = critters_.find(m.id);
            if (it != critters_.end()) it->second.pid = m.pid;
            break;
        }
        default: break;
    }
}

const ClientCritter* ClientCore::Find(CritterId id) const {
    auto it = critters_.find(id);
    return it == critters_.end() ? nullptr : &it->second;
}

CritterPose ClientCore::Pose(const ClientCritter& c, double t) const {
    CritterPose p;
    const auto& tr = c.track;
    if (tr.empty()) { p.x = c.pos.x; p.y = c.pos.y; return p; }
    if (t <= tr.front().t || tr.size() == 1) { p.x = tr.front().x; p.y = tr.front().y; }
    else if (t >= tr.back().t) { p.x = tr.back().x; p.y = tr.back().y; }
    else {
        size_t i = 1;
        while (i + 1 < tr.size() && tr[i].t < t) ++i;
        const TrackKey& a = tr[i - 1];
        const TrackKey& b = tr[i];
        const double dt = std::max(1e-6, b.t - a.t);
        const float u = static_cast<float>((t - a.t) / dt);
        p.x = a.x + (b.x - a.x) * u;
        p.y = a.y + (b.y - a.y) * u;
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > 1e-5f) {
            p.moving = true;
            p.speed = static_cast<float>(len / dt);
            p.hx = dx / len; p.hy = dy / len;
        }
    }
    const double age = t - c.offT;
    if (age >= 0 && age < kBlendTime) {
        const float k = 1.0f - static_cast<float>(age / kBlendTime);
        p.x += c.offX * k;
        p.y += c.offY * k;
    }
    return p;
}

void ClientCore::RenderPos(const ClientCritter& c, float& x, float& y) const {
    CritterPose p = Pose(c, Now());
    x = p.x; y = p.y;
}

bool ClientCore::IsBreak() const { return wait_.valid && Now() < wait_.waitEnd; }
double ClientCore::BreakRemaining() const { return wait_.valid ? std::max(0.0, wait_.waitEnd - Now()) : 0.0; }
bool ClientCore::InBattle() const { return wait_.valid && Now() < wait_.battleEnd; }
float ClientCore::CurrentAp() const {
    if (!wait_.valid) return hasStats_ ? static_cast<float>(stats_.sequence) : 0.0f;
    const double dtMs = (Now() - wait_.recvLocal) * 1000.0;
    double ap = wait_.ap + wait_.maxAp * dtMs / wait_.apRegenMs;
    return static_cast<float>(std::min<double>(ap, wait_.maxAp) / 100.0);
}

void ClientCore::RequestMove(TilePos target) {
    if (state_ != State::InGame) return;
    net::MoveToReq m;
    m.x = target.x; m.y = target.y;
    Send(net::Pack(m));
}

void ClientCore::RequestMoveTo(float tx, float ty, bool run) {
    if (state_ != State::InGame) return;
    if (!serverTiming_) {
        RequestMove(TilePos{static_cast<int16_t>(std::lround(tx)), static_cast<int16_t>(std::lround(ty))});
        return;
    }
    gnet::MoveToF m;
    m.x = tx + 0.5f; m.y = ty + 0.5f;  // protocol: cell units, centre +0.5
    m.run = run ? 1 : 0;
    SendG(m);
}

void ClientCore::SendChat(const std::string& text) {
    if (state_ != State::InGame || text.empty()) return;
    net::ChatReq m;
    m.text = text;
    Send(net::Pack(m));
}

void ClientCore::SendPing() {
    net::Ping p;
    p.token = static_cast<uint32_t>(Now() * 1000);
    Send(net::Pack(p));
}

void ClientCore::PickUp(ItemId id) { gnet::PickUp m; m.id = id; SendG(m); }
void ClientCore::Drop(uint32_t invId) { gnet::Drop m; m.id = invId; SendG(m); }
void ClientCore::SwapHands() {
    const gnet::InvEntry* h1 = nullptr; const gnet::InvEntry* h2 = nullptr;
    for (const auto& e : inventory_) { if (e.equipped == gnet::SlotHand1) h1 = &e; else if (e.equipped == gnet::SlotHand2) h2 = &e; }
    if (h2) Equip(h2->id, true, gnet::SlotHand1);
    else if (h1) Equip(h1->id, true, gnet::SlotHand2);
}

void ClientCore::Equip(uint32_t invId, bool on, uint32_t slot) { gnet::Equip m; m.id = invId; m.on = on ? 1 : 0; m.slot = static_cast<uint8_t>(slot); SendG(m); }
void ClientCore::Use(uint32_t objectId) { gnet::Use m; m.id = objectId; SendG(m); }
void ClientCore::Attack(CritterId id) { gnet::Attack m; m.id = id; SendG(m); }
void ClientCore::Take(uint32_t container, uint32_t entry) { gnet::Take m; m.container = container; m.entry = entry; m.isCritter = (container == containerOwner_ && containerCritter_) ? 1 : 0; SendG(m); }
void ClientCore::Put(uint32_t invEntry) {
    if (!hasContainer()) return;
    gnet::Put m; m.container = containerOwner_; m.entry = invEntry; m.isCritter = containerCritter_ ? 1 : 0; SendG(m);
}
void ClientCore::Loot(uint32_t critterId) { gnet::Loot m; m.id = critterId; SendG(m); }
void ClientCore::Talk(uint32_t critter) { gnet::Talk m; m.critter = critter; SendG(m); }
void ClientCore::DialogChoose(int index) { gnet::DialogChoose m; m.index = static_cast<uint8_t>(index); SendG(m); }
void ClientCore::DialogClose() { if (!dialog_.open) return; gnet::DialogCloseReq m; SendG(m); dialog_ = DialogState{}; }
void ClientCore::Examine(uint32_t id, bool isCritter) { gnet::Examine m; m.id = id; m.isCritter = isCritter ? 1 : 0; SendG(m); }
void ClientCore::CharCreate(const uint8_t special[7], uint16_t tagMask, uint16_t traitMask) { gnet::CharCreate m; for (int i = 0; i < 7; i++) m.special[i] = special[i]; m.tagMask = tagMask; m.traitMask = traitMask; SendG(m); }
void ClientCore::SpendSkill(int skill, int points) { gnet::SpendSkill m; m.skill = static_cast<uint8_t>(skill); m.points = static_cast<uint8_t>(points); SendG(m); }
void ClientCore::UseSkill(int skill, int kind, uint32_t target) { gnet::UseSkill m; m.skill = static_cast<uint8_t>(skill); m.kind = static_cast<uint8_t>(kind); m.target = target; SendG(m); }
void ClientCore::RequestState() { SendG(gnet::ReqState{}); }

}  // namespace f3
