#include "Amx.h"

#include <cstring>
#include <fstream>
#include <set>
#include "common/log.h"

namespace f3 {

static bool g_trace = false;
void AmxInstance::SetTrace(bool on) { g_trace = on; }

namespace {
enum Op {
    NONE, LOAD_PRI, LOAD_ALT, LOAD_S_PRI, LOAD_S_ALT, LREF_PRI, LREF_ALT, LREF_S_PRI, LREF_S_ALT, LOAD_I, LODB_I, CONST_PRI,
    CONST_ALT, ADDR_PRI, ADDR_ALT, STOR_PRI, STOR_ALT, STOR_S_PRI, STOR_S_ALT, SREF_PRI, SREF_ALT, SREF_S_PRI, SREF_S_ALT,
    STOR_I, STRB_I, LIDX, LIDX_B, IDXADDR, IDXADDR_B, ALIGN_PRI, ALIGN_ALT, LCTRL, SCTRL, MOVE_PRI, MOVE_ALT, XCHG, PUSH_PRI,
    PUSH_ALT, PUSH_R, PUSH_C, PUSH, PUSH_S, POP_PRI, POP_ALT, STACK, HEAP, PROC, RET, RETN, CALL, CALL_PRI, JUMP, JREL, JZER,
    JNZ, JEQ, JNEQ, JLESS, JLEQ, JGRTR, JGEQ, JSLESS, JSLEQ, JSGRTR, JSGEQ, SHL, SHR, SSHR, SHL_C_PRI, SHL_C_ALT, SHR_C_PRI,
    SHR_C_ALT, SMUL, SDIV, SDIV_ALT, UMUL, UDIV, UDIV_ALT, ADD, SUB, SUB_ALT, AND, OR, XOR, NOT, NEG, INVERT, ADD_C, SMUL_C,
    ZERO_PRI, ZERO_ALT, ZERO, ZERO_S, SIGN_PRI, SIGN_ALT, EQ, NEQ, LESS, LEQ, GRTR, GEQ, SLESS, SLEQ, SGRTR, SGEQ, EQ_C_PRI,
    EQ_C_ALT, INC_PRI, INC_ALT, INC, INC_S, INC_I, DEC_PRI, DEC_ALT, DEC, DEC_S, DEC_I, MOVS, CMPS, FILL, HALT, BOUNDS,
    SYSREQ_PRI, SYSREQ_C, FILE_, LINE, SYMBOL, SRANGE, JUMP_PRI, SWITCH, CASETBL, SWAP_PRI, SWAP_ALT, PUSHADDR, NOP, SYSREQ_D,
    SYMTAG, BREAK
};

template <class T> T RdLE(const std::vector<uint8_t>& b, size_t o) {
    T v{};
    std::memcpy(&v, b.data() + o, sizeof(T));
    return v;
}
}  // namespace

// ------------------------------------------------------------------ program

std::shared_ptr<AmxScript> AmxScript::Load(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return nullptr; }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 56) { err = "too small"; return nullptr; }
    uint32_t size = RdLE<uint32_t>(d, 0);
    uint16_t magic = RdLE<uint16_t>(d, 4);
    uint16_t flags = RdLE<uint16_t>(d, 8), defsize = RdLE<uint16_t>(d, 10);
    int32_t cod = RdLE<int32_t>(d, 12), dat = RdLE<int32_t>(d, 16), hea = RdLE<int32_t>(d, 20), stp = RdLE<int32_t>(d, 24);
    int32_t pub = RdLE<int32_t>(d, 32), nat = RdLE<int32_t>(d, 36), lib = RdLE<int32_t>(d, 40);
    if (magic != 0xF1E0 || size > d.size() || defsize != 40 || (flags & 0x04)) { err = "unsupported AMX header"; return nullptr; }
    if (cod < 0 || dat < cod || hea < dat || stp < hea || static_cast<size_t>(hea) > d.size()) { err = "bad AMX sections"; return nullptr; }
    auto s = std::make_shared<AmxScript>();
    s->name_ = path;
    size_t ncode = static_cast<size_t>(dat - cod) / 4;
    s->code_.resize(ncode);
    std::memcpy(s->code_.data(), d.data() + cod, ncode * 4);
    s->data_.assign(d.begin() + dat, d.begin() + hea);
    s->hea_ = hea - dat;
    s->stp_ = stp - dat;
    auto name = [&](int32_t o) {
        const char* p = reinterpret_cast<const char*>(d.data() + o + 4);
        return std::string(p, strnlen(p, 36));
    };
    for (int32_t o = pub; o + 40 <= nat; o += 40) s->publics_[name(o)] = RdLE<int32_t>(d, o);
    for (int32_t o = nat; o + 40 <= lib; o += 40) s->natives_.push_back(name(o));
    return s;
}

std::vector<std::string> AmxScript::PublicNames() const {
    std::vector<std::string> v;
    for (auto& [k, _] : publics_) v.push_back(k);
    return v;
}

// ------------------------------------------------------------------ instance

AmxInstance::AmxInstance(std::shared_ptr<AmxScript> script, const std::unordered_map<std::string, AmxNative>* natives)
    : script_(std::move(script)), natives_(natives) {
    mem_.assign(static_cast<size_t>(script_->stp_), 0);
    std::memcpy(mem_.data(), script_->data_.data(), script_->data_.size());
    hea_ = script_->hea_;
    bound_.resize(script_->natives_.size(), nullptr);
    warned_.assign(script_->natives_.size(), false);
    for (size_t i = 0; i < script_->natives_.size(); ++i) {
        auto it = natives_->find(script_->natives_[i]);
        if (it != natives_->end()) bound_[i] = &it->second;
    }
}

bool AmxInstance::Fail(const char* why) {
    err_ = why;
    return false;
}

AmxCell* AmxInstance::Cell(AmxCell addr) {
    if (addr < 0 || static_cast<size_t>(addr) + 4 > mem_.size()) return nullptr;
    return reinterpret_cast<AmxCell*>(mem_.data() + addr);  // x86/ARM: unaligned access tolerated
}

AmxCell AmxInstance::Get(AmxCell addr) const {
    if (addr < 0 || static_cast<size_t>(addr) + 4 > mem_.size()) return 0;
    AmxCell v;
    std::memcpy(&v, mem_.data() + addr, 4);
    return v;
}

void AmxInstance::Set(AmxCell addr, AmxCell v) {
    if (addr < 0 || static_cast<size_t>(addr) + 4 > mem_.size()) return;
    std::memcpy(mem_.data() + addr, &v, 4);
}

bool AmxInstance::SetString(AmxCell addr, const std::string& s, size_t maxCells) {
    if (s.size() + 1 > maxCells || addr < 0 || static_cast<size_t>(addr) + 4 * (s.size() + 1) > mem_.size()) return false;
    for (size_t i = 0; i < s.size(); ++i) Set(addr + static_cast<AmxCell>(4 * i), static_cast<unsigned char>(s[i]));
    Set(addr + static_cast<AmxCell>(4 * s.size()), 0);
    return true;
}

AmxCell AmxInstance::Alloc(int cells) {
    if (cells <= 0 || static_cast<size_t>(hea_) + 4 * static_cast<size_t>(cells) > mem_.size() / 2) return -1;   // keep the stack half free
    AmxCell a = hea_;
    for (int i = 0; i < cells; ++i) Set(a + 4 * i, 0);
    hea_ += 4 * cells;
    return a;
}

std::string AmxInstance::GetString(AmxCell addr, size_t maxLen) const {
    std::string s;
    AmxCell first = Get(addr);
    bool packed = static_cast<uint32_t>(first) > 0xFF;  // Pawn: packed strings hold up to 4 chars per cell, first char in the high byte
    for (size_t i = 0; s.size() < maxLen; ++i) {
        AmxCell c = Get(addr + static_cast<AmxCell>(4 * i));
        if (!packed) {
            if (c == 0) break;
            s.push_back(static_cast<char>(c));
        } else {
            bool end = false;
            for (int b = 3; b >= 0; --b) {
                char ch = static_cast<char>((static_cast<uint32_t>(c) >> (8 * b)) & 0xFF);
                if (ch == 0) { end = true; break; }
                s.push_back(ch);
            }
            if (end) break;
        }
        if (addr + static_cast<AmxCell>(4 * i) >= static_cast<AmxCell>(mem_.size())) break;
    }
    return s;
}

bool AmxInstance::Call(const std::string& pub, const std::vector<AmxCell>& args, AmxCell* ret) {
    auto it = script_->publics_.find(pub);
    if (it == script_->publics_.end()) return false;
    err_.clear();
    stk_ = static_cast<AmxCell>(mem_.size());
    frm_ = stk_;
    // push args right-to-left, then the byte count, then the sentinel return address
    auto push = [&](AmxCell v) { stk_ -= 4; Set(stk_, v); };
    for (size_t i = args.size(); i-- > 0;) push(args[i]);
    push(static_cast<AmxCell>(args.size() * 4));
    push(-1);
    steps_ = 0;
    bool ok = Run(it->second);
    if (ret) *ret = pri_;
    if (!ok) log::Warn("amx %s.%s: %s", script_->name_.c_str(), pub.c_str(), err_.c_str());
    return ok;
}

bool AmxInstance::Run(int32_t cipStart) {
    const std::vector<AmxCell>& code = script_->code_;
    int32_t cip = cipStart / 4;
    auto push = [&](AmxCell v) { stk_ -= 4; Set(stk_, v); };
    auto pop = [&]() { AmxCell v = Get(stk_); stk_ += 4; return v; };
    auto jump = [&](AmxCell addr) { cip = addr / 4; };
    auto U = [](AmxCell v) { return static_cast<uint32_t>(v); };
    constexpr int kMaxSteps = 4000000;

    while (true) {
        if (cip < 0 || static_cast<size_t>(cip) >= code.size()) return Fail("cip out of range");
        if (++steps_ > kMaxSteps) return Fail("instruction limit exceeded");
        AmxCell op = code[cip++];
        auto arg = [&](int k = 0) { return (static_cast<size_t>(cip + k) < code.size()) ? code[cip + k] : 0; };
        switch (op) {
            case NONE: case NOP: case BREAK: break;
            case LOAD_PRI: pri_ = Get(arg()); ++cip; break;
            case LOAD_ALT: alt_ = Get(arg()); ++cip; break;
            case LOAD_S_PRI: pri_ = Get(frm_ + arg()); ++cip; break;
            case LOAD_S_ALT: alt_ = Get(frm_ + arg()); ++cip; break;
            case LREF_PRI: pri_ = Get(Get(arg())); ++cip; break;
            case LREF_ALT: alt_ = Get(Get(arg())); ++cip; break;
            case LREF_S_PRI: pri_ = Get(Get(frm_ + arg())); ++cip; break;
            case LREF_S_ALT: alt_ = Get(Get(frm_ + arg())); ++cip; break;
            case LOAD_I: pri_ = Get(pri_); break;
            case LODB_I: {
                int n = arg(); ++cip;
                if (n < 1 || n > 4 || pri_ < 0 || static_cast<size_t>(pri_) + static_cast<size_t>(n) > mem_.size()) return Fail("memory access");
                AmxCell v = 0;
                std::memcpy(&v, mem_.data() + pri_, static_cast<size_t>(n));
                pri_ = v;
                break;
            }
            case CONST_PRI: pri_ = arg(); ++cip; break;
            case CONST_ALT: alt_ = arg(); ++cip; break;
            case ADDR_PRI: pri_ = frm_ + arg(); ++cip; break;
            case ADDR_ALT: alt_ = frm_ + arg(); ++cip; break;
            case STOR_PRI: Set(arg(), pri_); ++cip; break;
            case STOR_ALT: Set(arg(), alt_); ++cip; break;
            case STOR_S_PRI: Set(frm_ + arg(), pri_); ++cip; break;
            case STOR_S_ALT: Set(frm_ + arg(), alt_); ++cip; break;
            case SREF_PRI: Set(Get(arg()), pri_); ++cip; break;
            case SREF_ALT: Set(Get(arg()), alt_); ++cip; break;
            case SREF_S_PRI: Set(Get(frm_ + arg()), pri_); ++cip; break;
            case SREF_S_ALT: Set(Get(frm_ + arg()), alt_); ++cip; break;
            case STOR_I: Set(alt_, pri_); break;
            case STRB_I: {
                int n = arg(); ++cip;
                if (alt_ < 0 || static_cast<size_t>(alt_) + n > mem_.size()) return Fail("memory access");
                std::memcpy(mem_.data() + alt_, &pri_, static_cast<size_t>(n));
                break;
            }
            case LIDX: pri_ = Get(alt_ + pri_ * 4); break;
            case LIDX_B: pri_ = Get(alt_ + (pri_ << arg())); ++cip; break;
            case IDXADDR: pri_ = alt_ + pri_ * 4; break;
            case IDXADDR_B: pri_ = alt_ + (pri_ << arg()); ++cip; break;
            case ALIGN_PRI: { int n = arg(); ++cip; if (n < 4) pri_ ^= (4 - n); break; }
            case ALIGN_ALT: { int n = arg(); ++cip; if (n < 4) alt_ ^= (4 - n); break; }
            case LCTRL: {
                switch (arg()) {
                    case 0: pri_ = 0; break;  // COD
                    case 1: pri_ = 0; break;  // DAT (addresses are DAT-relative)
                    case 2: pri_ = hea_; break;
                    case 3: pri_ = static_cast<AmxCell>(mem_.size()); break;
                    case 4: pri_ = stk_; break;
                    case 5: pri_ = frm_; break;
                    case 6: pri_ = (cip + 1) * 4; break;
                    default: pri_ = 0; break;
                }
                ++cip;
                break;
            }
            case SCTRL: {
                int which = arg(); ++cip;
                switch (which) {
                    case 2: hea_ = pri_; break;
                    case 4: stk_ = pri_; break;
                    case 5: frm_ = pri_; break;
                    case 6: jump(pri_); break;
                    default: break;
                }
                break;
            }
            case MOVE_PRI: pri_ = alt_; break;
            case MOVE_ALT: alt_ = pri_; break;
            case XCHG: std::swap(pri_, alt_); break;
            case PUSH_PRI: push(pri_); break;
            case PUSH_ALT: push(alt_); break;
            case PUSH_R: { int n = arg(); ++cip; for (int i = 0; i < n; i++) push(pri_); break; }
            case PUSH_C: push(arg()); ++cip; break;
            case PUSH: push(Get(arg())); ++cip; break;
            case PUSH_S: push(Get(frm_ + arg())); ++cip; break;
            case POP_PRI: pri_ = pop(); break;
            case POP_ALT: alt_ = pop(); break;
            case STACK: alt_ = stk_; stk_ += arg(); ++cip; if (stk_ > static_cast<AmxCell>(mem_.size())) { static char eb[96]; std::snprintf(eb, sizeof eb, "stack underflow (cip %d, stk %d, arg %d, mem %d)", (cip - 2) * 4, stk_, arg(-1), (int)mem_.size()); return Fail(eb); } if (stk_ < hea_) return Fail("stack overflow"); break;
            case HEAP: alt_ = hea_; hea_ += arg(); ++cip; if (hea_ < 0 || hea_ > stk_) return Fail("heap overflow"); break;
            case PROC: push(frm_); frm_ = stk_; break;
            case RET: { frm_ = pop(); AmxCell r = pop(); if (r == -1) return true; jump(r); break; }
            case RETN: {
                frm_ = pop();
                AmxCell r = pop();
                AmxCell n = Get(stk_);
                stk_ += 4 + n;
                if (r == -1) return true;
                jump(r);
                break;
            }
            case CALL: { AmxCell target = arg(); push((cip + 1) * 4); jump(target); break; }
            case CALL_PRI: push(cip * 4); jump(pri_); break;
            case JUMP: jump(arg()); break;
            case JREL: cip += arg() / 4 + 1; break;
            case JZER: if (pri_ == 0) jump(arg()); else ++cip; break;
            case JNZ: if (pri_ != 0) jump(arg()); else ++cip; break;
            case JEQ: if (pri_ == alt_) jump(arg()); else ++cip; break;
            case JNEQ: if (pri_ != alt_) jump(arg()); else ++cip; break;
            case JLESS: if (U(pri_) < U(alt_)) jump(arg()); else ++cip; break;
            case JLEQ: if (U(pri_) <= U(alt_)) jump(arg()); else ++cip; break;
            case JGRTR: if (U(pri_) > U(alt_)) jump(arg()); else ++cip; break;
            case JGEQ: if (U(pri_) >= U(alt_)) jump(arg()); else ++cip; break;
            case JSLESS: if (pri_ < alt_) jump(arg()); else ++cip; break;
            case JSLEQ: if (pri_ <= alt_) jump(arg()); else ++cip; break;
            case JSGRTR: if (pri_ > alt_) jump(arg()); else ++cip; break;
            case JSGEQ: if (pri_ >= alt_) jump(arg()); else ++cip; break;
            case SHL: pri_ = static_cast<AmxCell>(U(pri_) << (alt_ & 31)); break;
            case SHR: pri_ = static_cast<AmxCell>(U(pri_) >> (alt_ & 31)); break;
            case SSHR: pri_ >>= (alt_ & 31); break;
            case SHL_C_PRI: pri_ = static_cast<AmxCell>(U(pri_) << (arg() & 31)); ++cip; break;
            case SHL_C_ALT: alt_ = static_cast<AmxCell>(U(alt_) << (arg() & 31)); ++cip; break;
            case SHR_C_PRI: pri_ = static_cast<AmxCell>(U(pri_) >> (arg() & 31)); ++cip; break;
            case SHR_C_ALT: alt_ = static_cast<AmxCell>(U(alt_) >> (arg() & 31)); ++cip; break;
            case SMUL: pri_ = static_cast<AmxCell>(U(pri_) * U(alt_)); break;
            case UMUL: pri_ = static_cast<AmxCell>(U(pri_) * U(alt_)); break;
            case SDIV: case SDIV_ALT: {
                AmxCell a = op == SDIV ? pri_ : alt_, b = op == SDIV ? alt_ : pri_;  // dividend, divisor
                if (b == 0) return Fail("divide by zero");
                if (a == INT32_MIN && b == -1) return Fail("division overflow");
                AmxCell q = a / b, r = a % b;
                if (r != 0 && ((r < 0) != (b < 0))) { --q; r += b; }  // Pawn: remainder takes the divisor's sign
                pri_ = q; alt_ = r;
                break;
            }
            case UDIV: case UDIV_ALT: {
                uint32_t a = U(op == UDIV ? pri_ : alt_), b = U(op == UDIV ? alt_ : pri_);
                if (b == 0) return Fail("divide by zero");
                pri_ = static_cast<AmxCell>(a / b); alt_ = static_cast<AmxCell>(a % b);
                break;
            }
            case ADD: pri_ = static_cast<AmxCell>(U(pri_) + U(alt_)); break;
            case SUB: pri_ = static_cast<AmxCell>(U(pri_) - U(alt_)); break;
            case SUB_ALT: pri_ = static_cast<AmxCell>(U(alt_) - U(pri_)); break;
            case AND: pri_ &= alt_; break;
            case OR: pri_ |= alt_; break;
            case XOR: pri_ ^= alt_; break;
            case NOT: pri_ = !pri_; break;
            case NEG: pri_ = static_cast<AmxCell>(0u - U(pri_)); break;
            case INVERT: pri_ = ~pri_; break;
            case ADD_C: pri_ = static_cast<AmxCell>(U(pri_) + U(arg())); ++cip; break;
            case SMUL_C: pri_ = static_cast<AmxCell>(U(pri_) * U(arg())); ++cip; break;
            case ZERO_PRI: pri_ = 0; break;
            case ZERO_ALT: alt_ = 0; break;
            case ZERO: Set(arg(), 0); ++cip; break;
            case ZERO_S: Set(frm_ + arg(), 0); ++cip; break;
            case SIGN_PRI: if (pri_ & 0x80) pri_ |= ~0xFF; break;
            case SIGN_ALT: if (alt_ & 0x80) alt_ |= ~0xFF; break;
            case EQ: pri_ = pri_ == alt_; break;
            case NEQ: pri_ = pri_ != alt_; break;
            case LESS: pri_ = U(pri_) < U(alt_); break;
            case LEQ: pri_ = U(pri_) <= U(alt_); break;
            case GRTR: pri_ = U(pri_) > U(alt_); break;
            case GEQ: pri_ = U(pri_) >= U(alt_); break;
            case SLESS: pri_ = pri_ < alt_; break;
            case SLEQ: pri_ = pri_ <= alt_; break;
            case SGRTR: pri_ = pri_ > alt_; break;
            case SGEQ: pri_ = pri_ >= alt_; break;
            case EQ_C_PRI: pri_ = pri_ == arg(); ++cip; break;
            case EQ_C_ALT: pri_ = alt_ == arg(); ++cip; break;
            case INC_PRI: ++pri_; break;
            case INC_ALT: ++alt_; break;
            case INC: Set(arg(), Get(arg()) + 1); ++cip; break;
            case INC_S: Set(frm_ + arg(), Get(frm_ + arg()) + 1); ++cip; break;
            case INC_I: Set(pri_, Get(pri_) + 1); break;
            case DEC_PRI: --pri_; break;
            case DEC_ALT: --alt_; break;
            case DEC: Set(arg(), Get(arg()) - 1); ++cip; break;
            case DEC_S: Set(frm_ + arg(), Get(frm_ + arg()) - 1); ++cip; break;
            case DEC_I: Set(pri_, Get(pri_) - 1); break;
            case MOVS: {
                int n = arg(); ++cip;
                if (pri_ < 0 || alt_ < 0 || static_cast<size_t>(pri_) + n > mem_.size() || static_cast<size_t>(alt_) + n > mem_.size()) return Fail("memory access");
                std::memmove(mem_.data() + alt_, mem_.data() + pri_, static_cast<size_t>(n));
                break;
            }
            case CMPS: {
                int n = arg(); ++cip;
                if (pri_ < 0 || alt_ < 0 || static_cast<size_t>(pri_) + n > mem_.size() || static_cast<size_t>(alt_) + n > mem_.size()) return Fail("memory access");
                pri_ = std::memcmp(mem_.data() + alt_, mem_.data() + pri_, static_cast<size_t>(n));
                break;
            }
            case FILL: {
                int n = arg(); ++cip;
                if (alt_ < 0 || static_cast<size_t>(alt_) + n > mem_.size()) return Fail("memory access");
                for (int i = 0; i + 4 <= n; i += 4) Set(alt_ + i, pri_);
                break;
            }
            case HALT: pri_ = arg(); return Fail("halt");
            case BOUNDS: if (U(pri_) > U(arg())) return Fail("array index out of bounds"); ++cip; break;
            case SYSREQ_PRI: case SYSREQ_C: {
                AmxCell idx;
                if (op == SYSREQ_C) { idx = arg(); ++cip; } else idx = pri_;
                if (idx < 0 || static_cast<size_t>(idx) >= bound_.size()) return Fail("bad native index");
                AmxCell nbytes = Get(stk_);
                int argc = nbytes / 4;
                std::vector<AmxCell> a(static_cast<size_t>(argc > 0 ? argc : 0));
                for (int i = 0; i < argc; ++i) a[static_cast<size_t>(i)] = Get(stk_ + 4 + 4 * i);
                if (g_trace) {
                    std::string line = script_->natives_[static_cast<size_t>(idx)] + "(";
                    for (int i = 0; i < argc; ++i) line += (i ? ", " : "") + std::to_string(a[static_cast<size_t>(i)]);
                    log::Info("amx trace[%s]: %s)%s", script_->name_.c_str(), line.c_str(), bound_[static_cast<size_t>(idx)] ? "" : " [unbound]");
                }
                if (bound_[static_cast<size_t>(idx)]) {
                    pri_ = (*bound_[static_cast<size_t>(idx)])(*this, a.data(), argc);
                } else {
                    static std::set<std::string> warnedNames;  // once per native name, across instances
                    if (warnedNames.insert(script_->natives_[static_cast<size_t>(idx)]).second)
                        log::Info("amx: unimplemented native %s (returns 0)", script_->natives_[static_cast<size_t>(idx)].c_str());
                    pri_ = 0;
                }
                break;
            }
            case FILE_: case SYMBOL: { int sz = arg(); cip += 1 + (sz + 3) / 4; break; }
            case LINE: case SRANGE: cip += 2; break;
            case SYMTAG: ++cip; break;
            case JUMP_PRI: jump(pri_); break;
            case SWITCH: {
                int t = arg() / 4;
                if (t < 0 || static_cast<size_t>(t) + 3 > code.size()) return Fail("bad switch table");
                int num = code[static_cast<size_t>(t) + 1];
                AmxCell dest = code[static_cast<size_t>(t) + 2];
                for (int k = 0; k < num; ++k) {
                    size_t o = static_cast<size_t>(t) + 3 + 2 * static_cast<size_t>(k);
                    if (o + 1 >= code.size()) return Fail("bad switch table");
                    if (code[o] == pri_) { dest = code[o + 1]; break; }
                }
                jump(dest);
                break;
            }
            case SWAP_PRI: { AmxCell t = Get(stk_); Set(stk_, pri_); pri_ = t; break; }
            case SWAP_ALT: { AmxCell t = Get(stk_); Set(stk_, alt_); alt_ = t; break; }
            case PUSHADDR: push(frm_ + arg()); ++cip; break;
            default: return Fail("invalid opcode");
        }
    }
}

}  // namespace f3
