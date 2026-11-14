// grptool - lists / extracts the Van Buren (F3 tech demo) .grp containers using resource.rht,
// and dumps English.stf string tables. See docs/formats/containers.md for the format spec.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
namespace fs = std::filesystem;

static uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static bool readFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    auto n = (size_t)f.tellg();
    f.seekg(0);
    out.resize(n);
    if (n) f.read((char*)out.data(), n);
    return (bool)f;
}

// ---------------------------------------------------------------- grp
struct GrpEntry { uint32_t offset, size; };
struct Grp {
    std::string name;
    fs::path path;
    std::vector<GrpEntry> entries;
    bool loaded = false;
};

static bool loadGrp(Grp& g) {
    std::ifstream f(g.path, std::ios::binary);
    if (!f) { fprintf(stderr, "warn: cannot open %s\n", g.path.string().c_str()); return false; }
    uint8_t h[12];
    f.read((char*)h, 12);
    if (!f || rd32(h) != 2 || rd32(h + 4) != 1) { fprintf(stderr, "warn: %s: bad grp header\n", g.path.string().c_str()); return false; }
    uint32_t n = rd32(h + 8);
    std::vector<uint8_t> tab((size_t)n * 8);
    if (n) f.read((char*)tab.data(), tab.size());
    if (!f) { fprintf(stderr, "warn: %s: truncated table\n", g.path.string().c_str()); return false; }
    g.entries.resize(n);
    for (uint32_t i = 0; i < n; i++) g.entries[i] = {rd32(&tab[i * 8]), rd32(&tab[i * 8 + 4])};
    g.loaded = true;
    return true;
}

// ---------------------------------------------------------------- rht
struct Res {
    uint32_t type = 0, index = 0;
    std::string name, grp;
    std::string path;  // "<grp>/<name>.<ext>" assigned after sniffing
    uint32_t size = 0, offset = 0;
    bool valid = false;
    std::string ext;
};

static std::string sniffExt(uint32_t type, const uint8_t* b, size_t n) {
    auto is = [&](const char* m) { size_t l = strlen(m); return n >= l && !memcmp(b, m, l); };
    switch (type) {
        case 100: return is("B3D ") ? "b3d" : (is("# G3") ? "g3d" : "bin");
        case 200:
            if (is("BM")) return "bmp";
            if (n >= 3 && b[1] <= 1 && (b[2] == 1 || b[2] == 2 || b[2] == 3 || b[2] == 9 || b[2] == 10 || b[2] == 11)) return "tga";
            if (is("DDS ")) return "dds";
            return "img";
        case 300: return "scra";
        case 400: return "tre";
        case 600: return "mesh.gr2";
        case 700: return "anim.gr2";
        case 800: return "ini";
        case 1100: return is("RIFF") ? "wav" : (is("OggS") ? "ogg" : "snd");
        case 1200: return "scrb";
        case 1300: return "emap";
        case 1400: case 1800: case 2400: case 2500: case 2600: case 2700: case 3500: case 3600: return "een";
        case 1500: return "veg";
        case 1600: return "gui";
        case 1700: return "mapdata";
        case 1900: case 2000: case 2100: case 2200: case 2300: return "sndscript";
        case 3700: return "devil";
    }
    return "bin";
}

struct Db {
    fs::path dir;
    std::vector<Res> res;
    std::map<std::string, Grp> grps;
};

static std::string cstr(const std::vector<uint8_t>& d, size_t off) {
    if (off >= d.size()) return {};
    size_t e = off;
    while (e < d.size() && d[e]) e++;
    return std::string((const char*)&d[off], e - off);
}

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static bool loadDb(const fs::path& dir, Db& db) {
    db.dir = dir;
    std::vector<uint8_t> d;
    if (!readFile(dir / "resource.rht", d) || d.size() < 20) { fprintf(stderr, "error: cannot read %s\n", (dir / "resource.rht").string().c_str()); return false; }
    if (rd32(&d[0]) != 1 || rd32(&d[4]) != 1) { fprintf(stderr, "error: resource.rht wrong version\n"); return false; }
    uint32_t count = rd32(&d[8]), grpBase = rd32(&d[12]), nameBase = rd32(&d[16]);
    if (20 + (uint64_t)count * 20 > d.size() || grpBase > d.size() || nameBase > d.size()) { fprintf(stderr, "error: resource.rht corrupt\n"); return false; }
    db.res.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* r = &d[20 + (size_t)i * 20];
        Res& x = db.res[i];
        x.index = rd32(r + 4);
        x.type = rd32(r + 8);
        x.name = cstr(d, (size_t)nameBase + rd32(r + 12));
        x.grp = cstr(d, (size_t)grpBase + rd32(r + 16));
    }
    for (auto& x : db.res) {
        auto it = db.grps.find(x.grp);
        if (it == db.grps.end()) {
            Grp g; g.name = x.grp; g.path = dir / "data" / (x.grp + ".grp");
            loadGrp(g);
            it = db.grps.emplace(x.grp, std::move(g)).first;
        }
        Grp& g = it->second;
        if (g.loaded && x.index < g.entries.size()) {
            x.offset = g.entries[x.index].offset; x.size = g.entries[x.index].size; x.valid = true;
        } else if (g.loaded) {
            fprintf(stderr, "warn: %s/%s index %u out of range\n", x.grp.c_str(), x.name.c_str(), x.index);
        }
    }
    // sniff extension from first bytes, and make unique paths
    std::map<std::string, std::ifstream> files;
    std::map<std::string, int> used;
    for (auto& x : db.res) {
        uint8_t b[16] = {0}; size_t n = 0;
        if (x.valid) {
            auto it = files.find(x.grp);
            if (it == files.end()) {
                it = files.emplace(std::piecewise_construct, std::forward_as_tuple(x.grp),
                                   std::forward_as_tuple(db.grps[x.grp].path, std::ios::binary)).first;
            }
            n = std::min<size_t>(16, x.size);
            it->second.clear(); it->second.seekg(x.offset); it->second.read((char*)b, (std::streamsize)n);
            if (!it->second) n = 0;
        }
        x.ext = sniffExt(x.type, b, n);
        std::string p = x.grp + "/" + x.name + "." + x.ext;
        int k = used[lower(p)]++;
        if (k) p = x.grp + "/" + x.name + "~" + std::to_string(x.index) + "." + x.ext;
        x.path = p;
    }
    return true;
}

static bool readEntry(const Db& db, const Res& r, std::vector<uint8_t>& out) {
    out.clear();
    if (!r.valid) return false;
    std::ifstream f(db.grps.at(r.grp).path, std::ios::binary);
    out.resize(r.size);
    f.seekg(r.offset);
    if (r.size) f.read((char*)out.data(), r.size);
    return (bool)f;
}

// ---------------------------------------------------------------- stf
// cp1252 -> UTF-8 (strings in English.stf use Windows-1252, e.g. 0x96 en dash)
static void appendUtf8(std::string& s, uint8_t c) {
    static const uint16_t hi[32] = {0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0x8D, 0x17D, 0x8F,
                                    0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178};
    uint32_t cp = c < 0x80 ? c : (c < 0xA0 ? hi[c - 0x80] : c);
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
    else { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
}

static int dumpStf(const fs::path& p) {
    std::vector<uint8_t> d;
    if (!readFile(p, d) || d.size() < 12) { fprintf(stderr, "error: cannot read %s\n", p.string().c_str()); return 1; }
    if (rd32(&d[0]) != 3 || rd32(&d[4]) != 1) { fprintf(stderr, "error: unsupported stf version\n"); return 1; }
    uint32_t n = rd32(&d[8]);
    if (12 + (uint64_t)n * 16 > d.size()) { fprintf(stderr, "error: stf truncated\n"); return 1; }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* r = &d[12 + (size_t)i * 16];
        uint32_t off = rd32(r), len = rd32(r + 4);
        if ((uint64_t)off + len > d.size()) { fprintf(stderr, "warn: string %u out of range\n", i); continue; }
        std::string s;
        for (uint32_t k = 0; k < len; k++) {
            char c = (char)d[off + k];
            if (c == '\n') s += "\\n"; else if (c == '\r') s += "\\r"; else if (c == '\t') s += "\\t"; else if (c == '\\') s += "\\\\"; else appendUtf8(s, (uint8_t)c);
        }
        printf("%u\t%s\n", i, s.c_str());
    }
    return 0;
}

// ---------------------------------------------------------------- main
static void usage() {
    fprintf(stderr,
            "grptool - Van Buren tech demo container tool\n"
            "  grptool list <gamedir>               list all resources (grp, index, type, size, path) + type summary\n"
            "  grptool extract <gamedir> <outdir>   extract everything to <outdir>/<grp>/<name>.<ext>\n"
            "  grptool cat <gamedir> <name>         write one resource to stdout (name or grp/name.ext; case-insens.)\n"
            "  grptool stf <file.stf|gamedir>       dump string table as 'id<TAB>text'\n");
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 1; }
    std::string cmd = argv[1];
    if (cmd == "stf") {
        fs::path a = argv[2];
        if (fs::is_directory(a)) a /= "English.stf";
        return dumpStf(a);
    }
    Db db;
    if (!loadDb(argv[2], db)) return 1;
    if (cmd == "list") {
        std::map<std::string, std::pair<int, uint64_t>> sum;
        for (auto& r : db.res) {
            printf("%s\t%u\t%u\t%u\t%s\n", r.grp.c_str(), r.index, r.type, r.size, r.path.c_str());
            auto& s = sum[r.ext]; s.first++; s.second += r.size;
        }
        fprintf(stderr, "\n%zu resources\n", db.res.size());
        for (auto& kv : sum) fprintf(stderr, "  %-10s %6d  %12llu bytes\n", kv.first.c_str(), kv.second.first, (unsigned long long)kv.second.second);
        return 0;
    }
    if (cmd == "extract" && argc >= 4) {
        fs::path out = argv[3];
        std::map<std::string, int> sum;
        std::vector<uint8_t> buf;
        int bad = 0;
        for (auto& r : db.res) {
            if (!readEntry(db, r, buf)) { fprintf(stderr, "warn: cannot read %s\n", r.path.c_str()); bad++; continue; }
            fs::path p = out / fs::u8path(r.path);
            fs::create_directories(p.parent_path());
            std::ofstream f(p, std::ios::binary);
            if (!buf.empty()) f.write((const char*)buf.data(), (std::streamsize)buf.size());
            if (!f) { fprintf(stderr, "warn: cannot write %s\n", p.string().c_str()); bad++; continue; }
            sum[r.ext]++;
        }
        for (auto& kv : sum) printf("%-10s %d\n", kv.first.c_str(), kv.second);
        if (bad) fprintf(stderr, "%d failures\n", bad);
        return bad ? 2 : 0;
    }
    if (cmd == "cat" && argc >= 4) {
        std::string q = lower(argv[3]);
        for (auto& c : q) if (c == '\\') c = '/';
        std::vector<const Res*> hit;
        for (auto& r : db.res)
            if (lower(r.path) == q || lower(r.name + "." + r.ext) == q) hit.push_back(&r);
        if (hit.empty())
            for (auto& r : db.res) if (lower(r.name) == q) hit.push_back(&r);
        if (hit.empty()) { fprintf(stderr, "not found: %s\n", argv[3]); return 1; }
        if (hit.size() > 1) {
            fprintf(stderr, "ambiguous, use grp/name.ext:\n");
            for (auto* h : hit) fprintf(stderr, "  %s\n", h->path.c_str());
            return 2;
        }
        std::vector<uint8_t> buf;
        if (!readEntry(db, *hit[0], buf)) { fprintf(stderr, "read error\n"); return 1; }
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        if (!buf.empty()) fwrite(buf.data(), 1, buf.size(), stdout);
        return 0;
    }
    usage();
    return 1;
}
