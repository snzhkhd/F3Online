// amxdis - disassembler / lister for the original Van Buren Pawn scripts (.scra dialogues, .scrb behaviours).
//
//   amxdis <file|dir> [-o <outdir>]      readable listing (header, publics, natives, data strings, disassembly)
//   amxdis --summary <dir>               per script: publics and natives actually called
//   amxdis --natives <dir> [--bound <AmxHost.cpp>]
//                                        native usage table (call sites, files, observed argument counts, bound by server?)
//
// Format: AMX file_version 6, 32-bit cells, non-compact, defsize 40 (name stored inline after a 4-byte address).
// Opcode numbering is the standard Pawn 3.x table (same as engine/server/Amx.cpp).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// name, number of fixed operands (-2 = file/symbol record, -3 = case table)
#define OPS(X) \
    X(none,0) X(load.pri,1) X(load.alt,1) X(load.s.pri,1) X(load.s.alt,1) X(lref.pri,1) X(lref.alt,1) X(lref.s.pri,1) X(lref.s.alt,1) \
    X(load.i,0) X(lodb.i,1) X(const.pri,1) X(const.alt,1) X(addr.pri,1) X(addr.alt,1) X(stor.pri,1) X(stor.alt,1) X(stor.s.pri,1) \
    X(stor.s.alt,1) X(sref.pri,1) X(sref.alt,1) X(sref.s.pri,1) X(sref.s.alt,1) X(stor.i,0) X(strb.i,1) X(lidx,0) X(lidx.b,1) \
    X(idxaddr,0) X(idxaddr.b,1) X(align.pri,1) X(align.alt,1) X(lctrl,1) X(sctrl,1) X(move.pri,0) X(move.alt,0) X(xchg,0) \
    X(push.pri,0) X(push.alt,0) X(push.r,1) X(push.c,1) X(push,1) X(push.s,1) X(pop.pri,0) X(pop.alt,0) X(stack,1) X(heap,1) \
    X(proc,0) X(ret,0) X(retn,0) X(call,1) X(call.pri,0) X(jump,1) X(jrel,1) X(jzer,1) X(jnz,1) X(jeq,1) X(jneq,1) X(jless,1) \
    X(jleq,1) X(jgrtr,1) X(jgeq,1) X(jsless,1) X(jsleq,1) X(jsgrtr,1) X(jsgeq,1) X(shl,0) X(shr,0) X(sshr,0) X(shl.c.pri,1) \
    X(shl.c.alt,1) X(shr.c.pri,1) X(shr.c.alt,1) X(smul,0) X(sdiv,0) X(sdiv.alt,0) X(umul,0) X(udiv,0) X(udiv.alt,0) X(add,0) \
    X(sub,0) X(sub.alt,0) X(and,0) X(or,0) X(xor,0) X(not,0) X(neg,0) X(invert,0) X(add.c,1) X(smul.c,1) X(zero.pri,0) \
    X(zero.alt,0) X(zero,1) X(zero.s,1) X(sign.pri,0) X(sign.alt,0) X(eq,0) X(neq,0) X(less,0) X(leq,0) X(grtr,0) X(geq,0) \
    X(sless,0) X(sleq,0) X(sgrtr,0) X(sgeq,0) X(eq.c.pri,1) X(eq.c.alt,1) X(inc.pri,0) X(inc.alt,0) X(inc,1) X(inc.s,1) \
    X(inc.i,0) X(dec.pri,0) X(dec.alt,0) X(dec,1) X(dec.s,1) X(dec.i,0) X(movs,1) X(cmps,1) X(fill,1) X(halt,1) X(bounds,1) \
    X(sysreq.pri,0) X(sysreq.c,1) X(file,-2) X(line,2) X(symbol,-2) X(srange,2) X(jump.pri,0) X(switch,1) X(casetbl,-3) \
    X(swap.pri,0) X(swap.alt,0) X(pushaddr,1) X(nop,0) X(sysreq.d,1) X(symtag,1) X(break,0)
struct OpInfo { const char* name; int n; };
#define X(a, b) {#a, b},
static const OpInfo kOps[] = {OPS(X)};
#undef X
static const int kNumOps = (int)(sizeof(kOps) / sizeof(kOps[0]));
enum { OP_PUSH_C = 39, OP_CALL = 49, OP_JUMP = 51, OP_JZER = 53, OP_JSGEQ = 64, OP_CONST_PRI = 11, OP_CONST_ALT = 12,
       OP_SYSREQ_C = 123, OP_FILE = 124, OP_SYMBOL = 126, OP_SWITCH = 129, OP_CASETBL = 130 };

struct Script {
    std::string name;
    uint16_t flags = 0, defsize = 0;
    uint8_t fver = 0, aver = 0;
    int32_t cod = 0, dat = 0, hea = 0, stp = 0, cip = 0;
    std::vector<int32_t> code;               // cells
    std::vector<uint8_t> data;               // DAT image
    std::vector<std::pair<std::string, int32_t>> publics;
    std::vector<std::string> natives;
    std::map<int32_t, std::string> strings;  // data address -> decoded string
    std::set<int32_t> strCells;              // data addresses covered by strings
};

template <class T> static T rd(const std::vector<uint8_t>& b, size_t o) { T v{}; if (o + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + o, sizeof(T)); return v; }

static bool printable(int c) { return c == 9 || c == 10 || (c >= 32 && c < 127) || (c >= 160 && c < 256); }

static void findStrings(Script& s) {
    size_t nc = s.data.size() / 4;
    auto cell = [&](size_t i) { return rd<int32_t>(s.data, i * 4); };
    for (size_t i = 0; i < nc;) {
        int32_t c = cell(i);
        if (c == 0) { ++i; continue; }
        size_t j = i; std::string str;  // unpacked: one char per cell
        while (j < nc && cell(j) > 0 && cell(j) < 256 && printable(cell(j))) str.push_back((char)cell(j++));
        if (j < nc && cell(j) == 0 && str.size() >= 2) {
            s.strings[(int32_t)(i * 4)] = str;
            for (size_t k = i; k <= j; ++k) s.strCells.insert((int32_t)k * 4);
            i = j + 1; continue;
        }
        if ((uint32_t)c > 0xFF) {  // packed: 4 chars per cell, first char in the high byte
            std::string p; size_t k = i; bool ok = true, term = false;
            for (; k < nc && !term; ++k) {
                uint32_t v = (uint32_t)cell(k);
                for (int b = 3; b >= 0; --b) {
                    int ch = (v >> (8 * b)) & 0xFF;
                    if (ch == 0) { term = true; break; }
                    if (!printable(ch)) { ok = false; break; }
                    p.push_back((char)ch);
                }
                if (!ok) break;
            }
            if (ok && term && p.size() >= 3) {
                s.strings[(int32_t)(i * 4)] = p;
                for (size_t q = i; q < k; ++q) s.strCells.insert((int32_t)q * 4);
                i = k; continue;
            }
        }
        ++i;
    }
}

static bool load(const fs::path& path, Script& s, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open"; return false; }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 56 || rd<uint16_t>(d, 4) != 0xF1E0) { err = "not AMX"; return false; }
    s.name = path.filename().string();
    s.fver = d[6]; s.aver = d[7]; s.flags = rd<uint16_t>(d, 8); s.defsize = rd<uint16_t>(d, 10);
    s.cod = rd<int32_t>(d, 12); s.dat = rd<int32_t>(d, 16); s.hea = rd<int32_t>(d, 20); s.stp = rd<int32_t>(d, 24); s.cip = rd<int32_t>(d, 28);
    int32_t pub = rd<int32_t>(d, 32), nat = rd<int32_t>(d, 36), lib = rd<int32_t>(d, 40);
    if (s.defsize != 40 || s.cod < 0 || s.dat < s.cod || s.hea < s.dat || (size_t)s.hea > d.size()) { err = "unsupported/bad header"; return false; }
    s.code.resize((size_t)(s.dat - s.cod) / 4);
    std::memcpy(s.code.data(), d.data() + s.cod, s.code.size() * 4);
    s.data.assign(d.begin() + s.dat, d.begin() + s.hea);
    auto nm = [&](int32_t o) { const char* p = (const char*)d.data() + o + 4; return std::string(p, strnlen(p, 36)); };
    for (int32_t o = pub; o + 40 <= nat; o += 40) s.publics.push_back({nm(o), rd<int32_t>(d, o)});
    for (int32_t o = nat; o + 40 <= lib; o += 40) s.natives.push_back(nm(o));
    findStrings(s);
    return true;
}

struct Insn { int32_t addr = 0; int op = 0; std::vector<int32_t> args; };

static std::vector<Insn> decode(const Script& s) {
    std::vector<Insn> v;
    size_t i = 0;
    while (i < s.code.size()) {
        Insn in; in.addr = (int32_t)i * 4; in.op = s.code[i];
        if (in.op < 0 || in.op >= kNumOps) { in.op = -1; v.push_back(in); ++i; continue; }
        int k = kOps[in.op].n;
        size_t cnt = 0;
        if (k >= 0) cnt = (size_t)k;
        else if (k == -3) cnt = i + 1 < s.code.size() ? 2 + 2 * (size_t)s.code[i + 1] : 0;       // num, default, num pairs
        else if (k == -2) cnt = i + 1 < s.code.size() ? 1 + ((size_t)s.code[i + 1] + 3) / 4 : 0;  // size, bytes
        for (size_t a = 0; a < cnt && i + 1 + a < s.code.size(); ++a) in.args.push_back(s.code[i + 1 + a]);
        v.push_back(in);
        i += 1 + cnt;
    }
    return v;
}

static std::string esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) { if (c == '\n') o += "\\n"; else if (c == '\t') o += "\\t"; else if (c == '"') o += "\\\""; else o += (char)c; }
    return o;
}

static bool isJump(int op) { return op == OP_JUMP || (op >= OP_JZER && op <= OP_JSGEQ); }

struct NativeUse { int calls = 0; std::set<int> argc; };
// the push.c right before sysreq.c holds 4*argc
static std::map<std::string, NativeUse> nativeUses(const Script& s, const std::vector<Insn>& ins) {
    std::map<std::string, NativeUse> m;
    for (size_t i = 0; i < ins.size(); ++i) {
        if (ins[i].op != OP_SYSREQ_C || ins[i].args.empty()) continue;
        int idx = ins[i].args[0];
        if (idx < 0 || (size_t)idx >= s.natives.size()) continue;
        auto& u = m[s.natives[(size_t)idx]];
        u.calls++;
        if (i > 0 && ins[i - 1].op == OP_PUSH_C && !ins[i - 1].args.empty()) u.argc.insert(ins[i - 1].args[0] / 4);
    }
    return m;
}

static void listing(const Script& s, std::ostream& o) {
    auto ins = decode(s);
    o << "; " << s.name << "  AMX file_version " << (int)s.fver << " amx_version " << (int)s.aver << " flags 0x" << std::hex << s.flags << std::dec
      << " defsize " << s.defsize << "\n";
    o << "; code " << s.code.size() * 4 << " bytes, data " << s.data.size() << " bytes, stack+heap " << (s.stp - s.hea) << " bytes, entry cip " << s.cip << "\n\n";
    std::map<int32_t, std::string> funcs;
    std::set<int32_t> pubAddr;
    for (auto& [n, a] : s.publics) { funcs[a] = n; pubAddr.insert(a); }
    std::set<int32_t> labels;
    for (auto& in : ins) {
        if (in.op == OP_CALL && !in.args.empty() && !funcs.count(in.args[0])) { char b[32]; snprintf(b, sizeof b, "f_%04x", in.args[0]); funcs[in.args[0]] = b; }
        if (isJump(in.op) && !in.args.empty()) labels.insert(in.args[0]);
        if (in.op == OP_CASETBL && in.args.size() >= 2) {
            labels.insert(in.args[1]);
            for (size_t k = 3; k < in.args.size(); k += 2) labels.insert(in.args[k]);
        }
    }
    o << "; ---- publics (" << s.publics.size() << ")\n";
    for (auto& [n, a] : s.publics) o << ";   " << n << " @" << a << "\n";
    o << "\n; ---- natives (" << s.natives.size() << ")  [index name]\n";
    for (size_t i = 0; i < s.natives.size(); ++i) o << ";   " << i << " " << s.natives[i] << "\n";
    o << "\n; ---- data strings (address: text)\n";
    for (auto& [a, t] : s.strings) o << ";   @" << a << ": \"" << esc(t) << "\"\n";
    std::string other;
    for (size_t i = 0; i < s.data.size() / 4; ++i) {
        int32_t v = rd<int32_t>(s.data, i * 4);
        if (v != 0 && !s.strCells.count((int32_t)i * 4)) other += " @" + std::to_string(i * 4) + "=" + std::to_string(v);
    }
    if (!other.empty()) o << ";   other nonzero cells:" << other << "\n";
    o << "\n; ---- code\n";
    auto strRef = [&](int32_t v) -> std::string {
        if (v < 8) return "";
        auto it = s.strings.find(v);
        return it == s.strings.end() ? "" : "  ; \"" + esc(it->second) + "\"";
    };
    for (auto& in : ins) {
        if (funcs.count(in.addr)) o << "\n" << funcs[in.addr] << ":" << (pubAddr.count(in.addr) ? "  ; public" : "") << "\n";
        if (labels.count(in.addr)) o << "L_" << std::hex << in.addr << std::dec << ":\n";
        if (in.op == OP_FILE || in.op == OP_SYMBOL || in.op == 125 || in.op == 127) continue;  // debug records (file/line/symbol/srange)
        char buf[32]; snprintf(buf, sizeof buf, "  %05x  ", in.addr);
        o << buf;
        if (in.op < 0) { o << "<invalid opcode>\n"; continue; }
        o << kOps[in.op].name;
        std::string note;
        if (in.op == OP_CALL && !in.args.empty()) o << " " << funcs[in.args[0]];
        else if (isJump(in.op) && !in.args.empty()) { char b[32]; snprintf(b, sizeof b, " L_%x", in.args[0]); o << b; }
        else if (in.op == OP_SYSREQ_C && !in.args.empty()) {
            int idx = in.args[0];
            o << " " << idx;
            note = "  ; " + ((idx >= 0 && (size_t)idx < s.natives.size()) ? s.natives[(size_t)idx] : std::string("?"));
        } else if (in.op == OP_SWITCH && !in.args.empty()) { char b[32]; snprintf(b, sizeof b, " T_%x", in.args[0]); o << b; }
        else if (in.op == OP_CASETBL) {
            o << " n=" << (in.args.empty() ? 0 : in.args[0]);
            if (in.args.size() >= 2) { char b[32]; snprintf(b, sizeof b, " default=L_%x", in.args[1]); o << b; }
            for (size_t k = 2; k + 1 < in.args.size(); k += 2) { char b[48]; snprintf(b, sizeof b, " %d->L_%x", in.args[k], in.args[k + 1]); o << b; }
        } else if (in.op == OP_FILE || in.op == OP_SYMBOL) { /* debug records: not printed */ }
        else {
            for (int32_t a : in.args) o << " " << a;
            if ((in.op == OP_PUSH_C || in.op == OP_CONST_PRI || in.op == OP_CONST_ALT) && !in.args.empty()) note = strRef(in.args[0]);
        }
        o << note << "\n";
    }
}

static std::vector<fs::path> scriptsIn(const fs::path& dir) {
    std::vector<fs::path> v;
    for (auto& e : fs::directory_iterator(dir)) {
        auto x = e.path().extension().string();
        std::transform(x.begin(), x.end(), x.begin(), ::tolower);
        if (e.is_regular_file() && (x == ".scra" || x == ".scrb")) v.push_back(e.path());
    }
    std::sort(v.begin(), v.end());
    return v;
}

static std::set<std::string> boundNatives(const std::string& file) {
    std::set<std::string> b;
    std::ifstream f(file);
    if (!f) return b;
    std::string txt((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::regex re("N\\[\"(\\w+)\"\\]");
    for (auto it = std::sregex_iterator(txt.begin(), txt.end(), re); it != std::sregex_iterator(); ++it) b.insert((*it)[1]);
    // accept-and-ignore stubs: for (const char* n : {"A", "B", ...})
    size_t p = txt.find("for (const char* n : {");
    while (p != std::string::npos) {
        size_t e = txt.find("})", p);
        if (e == std::string::npos) break;
        std::string blk = txt.substr(p, e - p);
        std::regex q("\"(\\w+)\"");
        for (auto it = std::sregex_iterator(blk.begin(), blk.end(), q); it != std::sregex_iterator(); ++it) b.insert((*it)[1]);
        p = txt.find("for (const char* n : {", e);
    }
    return b;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: amxdis <file|dir> [-o outdir] | --summary <dir> | --natives <dir> [--bound AmxHost.cpp]\n");
        return 2;
    }
    if (std::string(kOps[OP_SYSREQ_C].name) != "sysreq.c" || std::string(kOps[OP_CASETBL].name) != "casetbl" ||
        std::string(kOps[OP_JSGEQ].name) != "jsgeq" || std::string(kOps[OP_JZER].name) != "jzer") {
        fprintf(stderr, "internal error: opcode table mismatch\n");
        return 3;
    }
    std::string mode = argv[1];
    if (mode == "--summary" || mode == "--natives") {
        if (argc < 3) return 2;
        std::string boundFile = "engine/server/AmxHost.cpp";
        for (int i = 3; i + 1 < argc; ++i) if (!strcmp(argv[i], "--bound")) boundFile = argv[i + 1];
        std::map<std::string, NativeUse> total; std::map<std::string, int> scriptsUsing;
        for (auto& p : scriptsIn(argv[2])) {
            Script s; std::string err;
            if (!load(p, s, err)) { fprintf(stderr, "%s: %s\n", p.string().c_str(), err.c_str()); continue; }
            auto uses = nativeUses(s, decode(s));
            if (mode == "--summary") {
                printf("%s\n  publics:", s.name.c_str());
                for (auto& [n, a] : s.publics) printf(" %s", n.c_str());
                printf("\n  natives:");
                for (auto& [n, u] : uses) printf(" %s(%d)", n.c_str(), u.calls);
                printf("\n");
            }
            for (auto& [n, u] : uses) { auto& t = total[n]; t.calls += u.calls; t.argc.insert(u.argc.begin(), u.argc.end()); scriptsUsing[n]++; }
        }
        if (mode == "--natives") {
            auto bound = boundNatives(boundFile);
            std::vector<std::string> names;
            for (auto& [n, u] : total) names.push_back(n);
            std::sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) { return total[a].calls > total[b].calls; });
            printf("%-28s %7s %6s  %-10s %s\n", "native", "calls", "files", "argc", "bound");
            int nb = 0;
            for (auto& n : names) {
                std::string ac; for (int a : total[n].argc) ac += (ac.empty() ? "" : ",") + std::to_string(a);
                bool isB = bound.count(n) != 0; nb += isB;
                printf("%-28s %7d %6d  %-10s %s\n", n.c_str(), total[n].calls, scriptsUsing[n], ac.c_str(), isB ? "yes" : "-");
            }
            printf("# %zu natives used, %d bound by the server (%s)\n", names.size(), nb, boundFile.c_str());
        }
        return 0;
    }
    fs::path in = argv[1], outDir;
    for (int i = 2; i + 1 < argc; ++i) if (!strcmp(argv[i], "-o")) outDir = argv[i + 1];
    std::vector<fs::path> files = fs::is_directory(in) ? scriptsIn(in) : std::vector<fs::path>{in};
    if (!outDir.empty()) fs::create_directories(outDir);
    int bad = 0;
    for (auto& p : files) {
        Script s; std::string err;
        if (!load(p, s, err)) { fprintf(stderr, "%s: %s\n", p.string().c_str(), err.c_str()); ++bad; continue; }
        if (outDir.empty()) listing(s, std::cout);
        else { std::ofstream o(outDir / (p.filename().string() + ".txt")); listing(s, o); }
    }
    return bad ? 1 : 0;
}
