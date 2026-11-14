#include "ClientCore.h"

#include <enet/enet.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include "common/log.h"

namespace f3 {

ClientCore::ClientCore() {}
ClientCore::~ClientCore() { Disconnect(); }

double ClientCore::Now() const {
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    return std::chrono::duration<double>(clock::now() - t0).count();
}

bool ClientCore::Connect(const std::string& host, uint16_t port, const std::string& name) {
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
                net::LoginReq req;
                req.name = name_;
                Send(net::Pack(req));
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
}

void ClientCore::AddChat(ChatLine l) {
    l.time = Now();
    chat_.push_back(std::move(l));
    ++chatCounter_;
    while (chat_.size() > 100) chat_.pop_front();
}

void ClientCore::Handle(const uint8_t* data, size_t len) {
    if (len == 0) return;
    ByteBuffer b(data, len);
    Msg id = static_cast<Msg>(b.ReadU8());
    switch (id) {
        case Msg::S2C_LoginOk: {
            net::LoginOk m;
            if (!m.Read(b)) return;
            selfId_ = m.critterId;
            moveDur_ = static_cast<double>(m.moveTicks) / std::max<int>(1, m.tickRate);
            state_ = State::InGame;
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
            critters_.clear();  // new map: everything gets re-announced by the visibility pass
            items_.clear();
            break;
        }
        case Msg::S2C_CritterAdd: {
            net::CritterAdd m;
            if (!m.Read(b)) return;
            ClientCritter c;
            c.id = m.id; c.name = m.name; c.kind = m.kind; c.color = m.color; c.dir = m.dir;
            c.pos = c.from = TilePos{m.x, m.y};
            critters_[c.id] = std::move(c);
            break;
        }
        case Msg::S2C_CritterRemove: {
            net::CritterRemove m;
            if (m.Read(b)) critters_.erase(m.id);
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
            if (m.snap || TileDist(c.pos, to) > 2) {
                c.pos = c.from = to;
                c.moveDur = 0;
            } else {
                float rx, ry;
                RenderPos(c, rx, ry);  // continue smoothly from where it is drawn now
                c.from = TilePos{static_cast<int16_t>(std::lround(rx)), static_cast<int16_t>(std::lround(ry))};
                c.pos = to;
                c.moveStart = Now();
                c.moveDur = moveDur_;
            }
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
            items_[m.id] = ClientItem{m.id, m.pid, m.name, TilePos{m.x, m.y}};
            break;
        }
        case Msg::S2C_ItemRemove: {
            net::ItemRemove m;
            if (m.Read(b)) items_.erase(m.id);
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

const ClientCritter* ClientCore::Find(CritterId id) const {
    auto it = critters_.find(id);
    return it == critters_.end() ? nullptr : &it->second;
}

void ClientCore::RenderPos(const ClientCritter& c, float& x, float& y) const {
    if (c.moveDur <= 0) { x = c.pos.x; y = c.pos.y; return; }
    float t = static_cast<float>((Now() - c.moveStart) / c.moveDur);
    t = std::clamp(t, 0.0f, 1.0f);
    x = c.from.x + (c.pos.x - c.from.x) * t;
    y = c.from.y + (c.pos.y - c.from.y) * t;
}

void ClientCore::RequestMove(TilePos target) {
    if (state_ != State::InGame) return;
    net::MoveToReq m;
    m.x = target.x; m.y = target.y;
    Send(net::Pack(m));
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

}  // namespace f3
