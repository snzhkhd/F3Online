// f3_dataimport - converts the extracted Van Buren demo data (tools/grptool output) into server-ready files.
//
//   f3_dataimport --extracted DIR --stf English.stf --out gamedata/server [--only MAPNAME]
//
// Output (all under --out, never committed):
//   manifest.json           map list, start map/entry (from Engine/sys.ini)
//   maps/<name>.json        map description (entry points, regions, entities); coordinates in world units + origin_x/origin_z of the grid
//   maps/<name>.walk        "WALK" u32 w u32 h, then w*h raw mapdata class bytes (0x2f = blocked), 2 cells/unit
//   protos.json             object prototypes from all .een templates (key "<template>.<TYPE>")
//   strings.tsv             English.stf as id<TAB>text (escaped)
//   scripts/*               *.scra *.scrb *.ini (original AMX scripts and globals.ini, copied verbatim)
// Formats: docs/formats/maps.md, docs/server-data.md. The parsers are tools/mapinfo's (included below).
#define main mapinfo_main
#include "../mapinfo/main.cpp"
#undef main

static void writeText(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(s.data(), (std::streamsize)s.size());
}

static J vec2(float x, float z) { J r = J::A(); r.add(J::N(x)); r.add(J::N(z)); return r; }

static const char* classOf(const std::string& type) {
    if (type == "CRT") return "critter";
    if (type == "WEA") return "weapon";
    if (type == "ARM") return "armor";
    if (type == "AMO") return "ammo";
    if (type == "ITM") return "item";
    if (type == "DOR") return "door";
    if (type == "CON") return "container";
    if (type == "USE") return "usable";
    return "unknown";
}
static std::string typeOfGrp(const std::string& g) {
    static const std::map<std::string, std::string> m = {{"_CRT", "CRT"}, {"_CON", "CON"}, {"_USE", "USE"}, {"_DOR", "DOR"},
                                                          {"_ITM", "ITM"}, {"_WEA", "WEA"}, {"_ARM", "ARM"}, {"_AMO", "AMO"}};
    auto it = m.find(g);
    return it == m.end() ? "" : it->second;
}
static int32_t s32(u32 v) { return (int32_t)v; }
static u32 rdw(const Bytes& b, size_t i) {
    if (i * 4 + 4 > b.size()) return 0;
    return b[i * 4] | (b[i * 4 + 1] << 8) | (b[i * 4 + 2] << 16) | ((u32)b[i * 4 + 3] << 24);
}

static J attackModeJson(const Gwam& g) {
    J j = J::O();
    j.set("type", J::I(g.m[0]));          // 1 burst?, 2 single shot, 6/8/9 melee/unarmed styles (observed)
    j.set("ap", J::I(g.m[6]));            // action cost (guess)
    j.set("dmg_min", J::I(g.m[9]));       // melee/unarmed damage range (guess, matches animal data)
    j.set("dmg_max", J::I(g.m[10]));
    // fields of the original GWAM record (docs/research/vbdecomp-findings.md section 3): word 6 is the RANGE (our "ap" above is that word),
    // the action points are the later `actionPoints` field
    j.set("action_points", J::I(s32(g.bb)));
    j.set("damage_type", J::I(s32(g.m[1])));
    j.set("shots", J::I(s32(g.m[2])));
    j.set("called_shot", J::I(g.b1));
    j.set("consumed", J::I(g.b2));
    J raw = J::A();
    for (int i = 0; i < 11; i++) raw.add(J::I(s32(g.m[i])));
    j.set("raw", raw);
    return j;
}

static J protoJson(const std::string& key, const std::string& type, const Een& e, const Stf& stf) {
    J j = J::O();
    j.set("key", J::S(key));
    j.set("type", J::S(type));
    j.set("class", J::S(classOf(type)));
    j.set("model", J::S(e.model));
    if (!e.icon1.empty()) j.set("icon", J::S(e.icon1));
    if (!e.icon2.empty()) j.set("icon_ground", J::S(e.icon2));
    if (e.hasGent) {
        j.set("name_id", J::I(e.gent[0])); j.set("desc_id", J::I(e.gent[1])); j.set("name2_id", J::I(e.gent[2]));
        j.set("name", J::S(stf.get(e.gent[0])));
        j.set("desc", J::S(stf.get(e.gent[1])));
        j.set("name2", J::S(stf.get(e.gent[2])));
        if (type == "CRT") j.set("max_health", J::I(s32(e.gent[6])));   // GENT v1 maxHealth (15 for every critter template, 100 for Default_PC)
    }
    j.set("team", J::I(e.ov.team));
    if (!e.ov.aiScript.empty()) j.set("script", J::S(e.ov.aiScript));
    if (!e.ov.dialogScript.empty()) j.set("dialog", J::S(e.ov.dialogScript));
    if (!e.ov.skin.empty()) j.set("skin", J::S(e.ov.skin));
    if (!e.ov.inv.empty()) j.set("inventory", jstrs(e.ov.inv));
    if (type == "CRT") {
        std::string k = lower(key);
        bool isStatic = k.rfind("useable_", 0) == 0 || k.rfind("ds_", 0) == 0;
        j.set("static", J::B(isStatic));
    }
    if (e.hasGcre) {
        const Gcre& c = e.gcre;
        J cr = J::O();
        J sp = J::A();
        for (int i = 0; i < 7; i++) sp.add(J::I(c.special[i]));
        cr.set("special", sp);  // S,P,E,C,I,A,L (order assumed, Fallout convention)
        J st = J::A();
        for (int i = 0; i < 8; i++) st.add(J::I(c.st[i]));
        cr.set("stat_words", st);
        J d = J::A();
        for (auto& p : c.tab13) d.add(J::A().add(J::I(p.first)).add(J::I(p.second)));
        cr.set("derived_overrides", d);
        cr.set("portrait", J::S(c.portrait));
        J unc = J::A();
        for (int i = 0; i < 8; i++) unc.add(J::I(c.unc[i]));
        cr.set("uncalled_chances", unc);
        cr.set("equipment", jstrs(c.inv));
        J am = J::A();
        for (auto& g : c.modes) am.add(attackModeJson(g));
        cr.set("attack_modes", am);
        J bd = J::O();
        for (int i = 0; i < 12; i++)
            if (!c.sockPairs[i].first.empty()) bd.set("socket" + std::to_string(i), J::S(c.sockPairs[i].first + "|" + c.sockPairs[i].second));
        cr.set("sockets", bd);
        j.set("critter", cr);
    }
    for (auto& rc : e.other) {
        const Bytes& b = rc.data;
        if (rc.tag == "GITM") {
            J it = J::O();
            it.set("class", J::I(rdw(b, 0)));
            it.set("subclass", J::I(rdw(b, 1)));
            it.set("raw_hex", J::S(hex(b.data(), b.size())));
            j.set("item", it);
        } else if (rc.tag == "GIWP") {
            J w = J::O();
            J pre = J::A();
            for (int i = 0; i < 15; i++) pre.add(J::I(s32(rdw(b, i))));
            w.set("raw_prefix", pre);
            w.set("weapon_class", J::I(rdw(b, 0)));
            w.set("caliber", J::I(s32(rdw(b, 1))));  // matches GIAM word 0 of ammo; -1 = none/melee
            w.set("range", J::I(rdw(b, 2)));
            w.set("dmg_min", J::N(rdw(b, 6) / 65536.0));  // 16.16 fixed (guess)
            w.set("dmg_max", J::N(rdw(b, 7) / 65536.0));
            J modes = J::A();
            try {
                if (b.size() > 62) {
                    Rd r(b.data() + 62, b.size() - 62);
                    while (r.left() >= 12 && r.b[r.o] == 'G') modes.add(attackModeJson(parseGwam(r)));
                }
            } catch (...) {}
            w.set("attack_modes", modes);
            j.set("weapon", w);
        } else if (rc.tag == "GIAR") {
            J a = J::O();
            J v = J::A();
            for (size_t i = 0; i < b.size() / 4; i++) v.add(J::I(s32(rdw(b, i))));
            a.set("values", v);  // 7 words; meaning guessed in the server (per damage type)
            j.set("armor", a);
        } else if (rc.tag == "GIAM") {
            J a = J::O();
            a.set("caliber", J::I(rdw(b, 0)));
            a.set("dmg_min", J::I(s32(rdw(b, 1))));
            a.set("dmg_max", J::I(s32(rdw(b, 2))));
            a.set("mod", J::I(s32(rdw(b, 3))));
            J v = J::A();
            for (size_t i = 0; i < b.size() / 4; i++) v.add(J::I(s32(rdw(b, i))));
            a.set("raw", v);
            j.set("ammo", a);
        } else if (rc.tag == "GOBJ") {
            j.set("use_kind", J::I(rdw(b, 0)));  // 0 usable, 1 door, 2 container
        } else if (rc.tag == "GDOR" || rc.tag == "GCON") {
            j.set(rc.tag == "GDOR" ? "gdor_hex" : "gcon_hex", J::S(hex(b.data(), b.size())));
        }
    }
    return j;
}

static J entityJson(const Ent& e) {
    J j = J::O();
    j.set("name", J::S(e.ov.inst));
    j.set("proto", J::S(e.name));
    j.set("type", J::S(e.type));
    j.set("x", J::N(e.pos[0]));
    j.set("y", J::N(e.pos[1]));
    j.set("z", J::N(e.pos[2]));
    j.set("yaw", J::N(e.rot[0]));
    j.set("team", J::I(e.ov.team));
    if (!e.ov.aiScript.empty()) j.set("script", J::S(e.ov.aiScript));
    if (!e.ov.dialogScript.empty()) j.set("dialog", J::S(e.ov.dialogScript));
    if (!e.ov.skin.empty()) j.set("skin", J::S(e.ov.skin));
    if (!e.ov.fx.empty()) j.set("effects", jstrs(e.ov.fx));
    if (!e.ov.inv.empty()) j.set("inventory", jstrs(e.ov.inv));
    return j;
}

static std::string argv_(int argc, char** argv, const char* k, const char* def = "") {
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], k)) return argv[i + 1];
    return def;
}

int main(int argc, char** argv) {
    fs::path root = argv_(argc, argv, "--extracted", "extracted");
    fs::path stfPath = argv_(argc, argv, "--stf", "");
    fs::path out = argv_(argc, argv, "--out", "gamedata/server");
    std::string only = lower(argv_(argc, argv, "--only", ""));
    if (argc < 2 || !fs::exists(root)) {
        fprintf(stderr, "usage: f3_dataimport --extracted DIR --stf English.stf --out gamedata/server [--only MAP]\n");
        return 2;
    }
    try {
        Stf stf;
        if (!stfPath.empty()) stf.load(stfPath);
        else fprintf(stderr, "warning: no --stf, names will be empty\n");

        // ---- strings
        if (stf.ok) {
            std::string s;
            for (u32 i = 1; i <= stf.count; i++) {
                std::string t = stf.get(i), e;
                for (char c : t) {
                    if (c == '\n') e += "\\n";
                    else if (c == '\r') {}
                    else if (c == '\t') e += "\\t";
                    else e += c;
                }
                s += std::to_string(i) + "\t" + e + "\n";
            }
            writeText(out / "strings.tsv", s);
        }

        // ---- prototypes
        J protos = J::O();
        int nproto = 0, nfail = 0;
        for (const char* g : {"_CRT", "_WEA", "_ARM", "_AMO", "_ITM", "_USE", "_DOR", "_CON"}) {
            fs::path dir = root / g;
            if (!fs::exists(dir)) continue;
            std::string type = typeOfGrp(g);
            for (auto& de : fs::directory_iterator(dir)) {
                if (de.path().extension() != ".een") continue;
                std::string stem = de.path().stem().string();
                try {
                    Een e = parseEen(readFile(de.path()));
                    std::string key = stem + "." + type;
                    protos.set(key, protoJson(key, type, e, stf));
                    nproto++;
                } catch (std::exception& ex) {
                    fprintf(stderr, "proto %s: %s\n", stem.c_str(), ex.what());
                    nfail++;
                }
            }
        }
        writeText(out / "protos.json", jstr(protos));
        printf("protos: %d (%d failed)\n", nproto, nfail);

        // ---- scripts
        int nscr = 0;
        if (fs::exists(root / "Scripts")) {
            fs::create_directories(out / "scripts");
            for (auto& de : fs::directory_iterator(root / "Scripts")) {
                std::string ext = de.path().extension().string();
                if (ext == ".scra" || ext == ".scrb" || ext == ".ini") {
                    fs::copy_file(de.path(), out / "scripts" / de.path().filename(), fs::copy_options::overwrite_existing);
                    nscr++;
                }
            }
        }
        printf("scripts copied: %d\n", nscr);

        // ---- maps
        MapIndex ix = buildIndex(root);
        std::set<std::string> names;
        for (auto& kv : ix.emapByStem) names.insert(kv.first);
        for (auto& b : ix.treBases) names.insert(b);
        std::map<std::string, std::string> display;  // lowercase -> original case
        for (const char* g : {"Maps", "Engine"}) {
            if (!fs::exists(root / g)) continue;
            for (auto& de : fs::directory_iterator(root / g)) {
                std::string ext = de.path().extension().string();
                if (ext == ".emap" || ext == ".tre") display[lower(de.path().stem().string())] = de.path().stem().string();
            }
        }
        J manifest = J::O();
        J mlist = J::A();
        int nmaps = 0;
        std::set<std::string> done;
        for (const std::string& ln : names) {
            if (!only.empty() && ln != only) continue;
            std::string name = display.count(ln) ? display[ln] : ln;
            if (!ix.emapByStem.count(ln) && ix.emapByTre.count(ln)) continue;  // tre base of an emap emitted under its own stem
            MapBundle mb = loadMap(root, ix, name);
            for (auto& er : mb.errors) fprintf(stderr, "map %s: %s\n", name.c_str(), er.c_str());
            if (!mb.hasGrid) { fprintf(stderr, "map %s: no mapdata grid, skipped\n", name.c_str()); continue; }
            if (done.count(lower(name))) continue;
            done.insert(lower(name));

            u32 w = mb.grid.w, h = mb.grid.h;
            std::string walk(12 + mb.grid.cell.size(), '\0');
            memcpy(&walk[0], "WALK", 4);
            memcpy(&walk[4], &w, 4);
            memcpy(&walk[8], &h, 4);
            memcpy(&walk[12], mb.grid.cell.data(), mb.grid.cell.size());
            writeText(out / "maps" / (name + ".walk"), walk);

            J j = J::O();
            j.set("name", J::S(name));
            j.set("tre_base", J::S(mb.treBase));
            j.set("width", J::I(w));
            j.set("height", J::I(h));
            j.set("cells_per_unit", J::I(2));
            // Grid origin: world position of the min corner of cell (0,0) = min corner of the tre root bbox (docs/formats/maps.md s.2).
            // All other coordinates in this file stay in original world units; the server subtracts the origin.
            MapOrigin org = mapOrigin(mb);
            j.set("origin_x", J::N(org.x));
            j.set("origin_z", J::N(org.z));
            j.set("origin_verified", J::B(org.verified));
            j.set("walk", J::S(name + ".walk"));
            J eps = J::A();
            for (auto& ep : mb.emap.eps) {
                J o = J::O();
                o.set("id", J::I(ep.id));
                J sl = J::A();
                for (size_t k = 0; k < ep.pos.size(); k++) {
                    J s = J::O();
                    s.set("x", J::N(ep.pos[k][0]));
                    s.set("z", J::N(ep.pos[k][2]));
                    s.set("yaw", J::N(ep.yaw[k]));
                    sl.add(s);
                }
                o.set("slots", sl);
                eps.add(o);
            }
            j.set("entry_points", eps);
            J regs = J::A();
            for (auto& r : mb.emap.regions) {
                J o = J::O();
                o.set("kind", J::I(r.kind));
                const char* kn = r.kind == 0 ? "transition" : r.kind == 1 ? "trigger" : r.kind == 6 ? "volume" : "other";
                o.set("type", J::S(kn));
                J pts = J::A();
                float y = 0;
                for (auto& p : r.pts) { pts.add(vec2(p[0], p[2])); y = p[1]; }
                o.set("pts", pts);
                o.set("y", J::N(y));
                if (r.hasScript) o.set("script", J::S(r.script));
                if (r.hasTrans) { o.set("target_map", J::S(r.tmap)); o.set("target_entry", J::I(r.tentry)); }
                if (r.hasBlk) o.set("band_id", J::I(r.blkId));
                regs.add(o);
            }
            j.set("regions", regs);
            J paths = J::A();
            for (auto& p : mb.emap.paths) {
                J o = J::O();
                o.set("name", J::S(p.name));
                J pts = J::A();
                for (auto& n : p.nodes) pts.add(vec2(n.pos[0], n.pos[2]));
                o.set("pts", pts);
                paths.add(o);
            }
            j.set("paths", paths);
            J ents = J::A();
            for (auto& e : mb.emap.ents) ents.add(entityJson(e));
            j.set("entities", ents);
            writeText(out / "maps" / (name + ".json"), jstr(j));
            J mi = J::O();
            mi.set("name", J::S(name));
            mi.set("width", J::I(w));
            mi.set("height", J::I(h));
            mi.set("entities", J::I((long long)mb.emap.ents.size()));
            mi.set("has_emap", J::B(mb.hasEmap));
            mlist.add(mi);
            nmaps++;
        }
        manifest.set("maps", mlist);

        // start map from Engine/sys.ini
        std::string startMap = "00_03_Tutorial_Junktown";
        int startEntry = 0;
        fs::path sys = root / "Engine" / "sys.ini";
        if (fs::exists(sys)) {
            std::ifstream f(sys);
            std::string line;
            while (std::getline(f, line)) {
                std::string l = lower(line);
                auto val = [&]() {
                    size_t q = line.find('=');
                    std::string v = q == std::string::npos ? "" : line.substr(q + 1);
                    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
                    size_t b = v.find_first_not_of(' ');
                    return b == std::string::npos ? std::string() : v.substr(b);
                };
                if (l.rfind("start map entry point", 0) == 0) startEntry = atoi(val().c_str());
                else if (l.rfind("start map", 0) == 0) {
                    std::string v = val();
                    if (v.size() > 4 && lower(v.substr(v.size() - 4)) == ".map") v.resize(v.size() - 4);
                    startMap = v;
                }
            }
        }
        manifest.set("start_map", J::S(startMap));
        manifest.set("start_entry", J::I(startEntry));
        writeText(out / "manifest.json", jstr(manifest));
        printf("maps written: %d, start %s entry %d\n", nmaps, startMap.c_str(), startEntry);
    } catch (std::exception& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
