// Data-driven content layer: loader + strict validation of data/*.json. Schema: docs/data-format.md.
#include "Content.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <nlohmann/json.hpp>
#include "common/log.h"
#include "common/protocol_game.h"

namespace f3 {

using nlohmann::json;
namespace fs = std::filesystem;

static_assert(kSkillSlots == kSkillCount, "skill table size");

namespace {

std::string Low(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

const char* const kStatNamesNamed[StNamedCount] = {"max_hp", "max_ap", "carry_weight", "armor_class", "sequence", "crit_chance", "melee_damage", "xp_gain", "skill_rate", "heal_rate"};
const char* const kSpecialKeys[kSpecialCount] = {"strength", "perception", "endurance", "charisma", "intelligence", "agility", "luck"};

bool ValidId(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (unsigned char c : s) if (!(std::isalnum(c) || c == '_' || c == '.' || c == '-')) return false;
    return true;
}
bool ValidHook(const std::string& s) {
    if (s.empty() || s.size() > 64 || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (unsigned char c : s) if (!(std::isalnum(c) || c == '_')) return false;
    return true;
}

const char* TypeName(const json& j) {
    switch (j.type()) {
        case json::value_t::null: return "null";
        case json::value_t::object: return "object";
        case json::value_t::array: return "array";
        case json::value_t::string: return "string";
        case json::value_t::boolean: return "boolean";
        case json::value_t::number_float: return "number";
        default: return "integer";
    }
}

// Error collector for one file. `bad` is set by the first error of the record being parsed.
struct Ctx {
    ContentReport& rep;
    std::string file;
    bool bad = false;
    void Err(const std::string& path, const std::string& msg) { rep.Error(file, path, msg); bad = true; }
};

// Strictly typed reader of one JSON object: wrong types, out of range values and unknown fields are errors with the field path.
struct Obj {
    Ctx& c;
    const json& j;
    std::string path;
    std::set<std::string> seen;
    Obj(Ctx& ctx, const json& obj, std::string p) : c(ctx), j(obj), path(std::move(p)) {}
    std::string P(const char* k) const { return path + "." + k; }
    const json* Get(const char* k) { seen.insert(k); auto it = j.find(k); return it == j.end() ? nullptr : &*it; }
    bool Has(const char* k) const { return j.contains(k); }
    std::optional<std::string> Str(const char* k, bool required = false, size_t maxLen = 1000) {
        const json* v = Get(k);
        if (!v) { if (required) c.Err(P(k), "missing required field"); return std::nullopt; }
        if (!v->is_string()) { c.Err(P(k), std::string("expected string, got ") + TypeName(*v)); return std::nullopt; }
        std::string s = v->get<std::string>();
        if (s.size() > maxLen) { c.Err(P(k), "string longer than " + std::to_string(maxLen) + " characters"); return std::nullopt; }
        if (required && s.empty()) { c.Err(P(k), "must not be empty"); return std::nullopt; }
        return s;
    }
    std::optional<int> Int(const char* k, int lo, int hi, bool required = false) {
        const json* v = Get(k);
        if (!v) { if (required) c.Err(P(k), "missing required field"); return std::nullopt; }
        if (!v->is_number_integer()) { c.Err(P(k), std::string("expected integer, got ") + TypeName(*v)); return std::nullopt; }
        const long long n = v->get<long long>();
        if (n < lo || n > hi) { c.Err(P(k), "value " + std::to_string(n) + " out of range " + std::to_string(lo) + ".." + std::to_string(hi)); return std::nullopt; }
        return static_cast<int>(n);
    }
    std::optional<float> Num(const char* k, float lo, float hi) {
        const json* v = Get(k);
        if (!v) return std::nullopt;
        if (!v->is_number()) { c.Err(P(k), std::string("expected number, got ") + TypeName(*v)); return std::nullopt; }
        const double n = v->get<double>();
        if (n < lo || n > hi) { c.Err(P(k), "value out of range"); return std::nullopt; }
        return static_cast<float>(n);
    }
    std::optional<bool> Bool(const char* k) {
        const json* v = Get(k);
        if (!v) return std::nullopt;
        if (!v->is_boolean()) { c.Err(P(k), std::string("expected boolean, got ") + TypeName(*v)); return std::nullopt; }
        return v->get<bool>();
    }
    const json* ObjField(const char* k) {
        const json* v = Get(k);
        if (!v) return nullptr;
        if (!v->is_object()) { c.Err(P(k), std::string("expected object, got ") + TypeName(*v)); return nullptr; }
        return v;
    }
    const json* ArrField(const char* k) {
        const json* v = Get(k);
        if (!v) return nullptr;
        if (!v->is_array()) { c.Err(P(k), std::string("expected array, got ") + TypeName(*v)); return nullptr; }
        return v;
    }
    std::optional<std::vector<std::string>> StrList(const char* k, size_t maxCount = 200) {
        const json* v = ArrField(k);
        if (!v) return std::nullopt;
        std::vector<std::string> out;
        if (v->size() > maxCount) { c.Err(P(k), "more than " + std::to_string(maxCount) + " entries"); return std::nullopt; }
        for (size_t i = 0; i < v->size(); ++i) {
            if (!(*v)[i].is_string()) { c.Err(P(k) + "[" + std::to_string(i) + "]", std::string("expected string, got ") + TypeName((*v)[i])); continue; }
            out.push_back((*v)[i].get<std::string>());
        }
        return out;
    }
    void Done() {   // unknown fields (typos) are errors
        for (auto it = j.begin(); it != j.end(); ++it)
            if (!seen.count(it.key())) c.Err(P(it.key().c_str()), "unknown field");
    }
};

std::vector<StatMod> ParseEffects(Obj& o) {
    std::vector<StatMod> out;
    const json* a = o.ArrField("effects");
    if (!a) return out;
    for (size_t i = 0; i < a->size(); ++i) {
        const std::string ep = o.P("effects") + "[" + std::to_string(i) + "]";
        if (!(*a)[i].is_object()) { o.c.Err(ep, std::string("expected object, got ") + TypeName((*a)[i])); continue; }
        Obj e(o.c, (*a)[i], ep);
        auto stat = e.Str("stat", true, 64);
        auto add = e.Int("add", -1000, 1000, true);
        e.Done();
        if (!stat || !add) continue;
        const int id = StatFromName(*stat);
        if (id < 0) { o.c.Err(ep + ".stat", "unknown stat '" + *stat + "' (valid: " + [] { std::string s; for (auto& n : StatNames()) s += (s.empty() ? "" : ", ") + n; return s; }() + ")"); continue; }
        out.push_back(StatMod{id, *add});
    }
    return out;
}

// ------------------------------------------------------------------ file level

struct FileDoc {
    json doc;
    std::string rel;   // path for messages
};

// Reads one data file and checks the envelope {schema, kind, records}. Returns the records array or nullptr.
const json* OpenEnvelope(Ctx& c, const fs::path& p, const char* kind, json& doc, std::set<std::string> extraKeys = {}) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { c.rep.Error(c.file, "$", "cannot open file"); return nullptr; }
    try {
        doc = json::parse(f, nullptr, true, /*ignore_comments=*/true);
    } catch (const std::exception& e) {
        c.rep.Error(c.file, "$", std::string("invalid JSON: ") + e.what());
        return nullptr;
    }
    if (!doc.is_object()) { c.rep.Error(c.file, "$", std::string("expected an object at the top level, got ") + TypeName(doc)); return nullptr; }
    bool ok = true;
    auto sch = doc.find("schema");
    if (sch == doc.end()) { c.rep.Error(c.file, "schema", "missing required field (expected " + std::to_string(kContentSchema) + ")"); ok = false; }
    else if (!sch->is_number_integer()) { c.rep.Error(c.file, "schema", std::string("expected integer, got ") + TypeName(*sch)); ok = false; }
    else if (sch->get<int>() > kContentSchema) { c.rep.Error(c.file, "schema", "schema " + std::to_string(sch->get<int>()) + " is newer than this server understands (max " + std::to_string(kContentSchema) + ")"); ok = false; }
    else if (sch->get<int>() < 1) { c.rep.Error(c.file, "schema", "invalid schema version " + std::to_string(sch->get<int>())); ok = false; }
    auto kd = doc.find("kind");
    if (kd == doc.end()) { c.rep.Error(c.file, "kind", std::string("missing required field (expected \"") + kind + "\")"); ok = false; }
    else if (!kd->is_string() || kd->get<std::string>() != kind) { c.rep.Error(c.file, "kind", std::string("expected \"") + kind + "\" (the folder this file is in), got " + kd->dump()); ok = false; }
    for (auto it = doc.begin(); it != doc.end(); ++it)
        if (it.key() != "schema" && it.key() != "kind" && it.key() != "records" && it.key() != "generator" && it.key() != "note" && !extraKeys.count(it.key())) { c.rep.Error(c.file, it.key(), "unknown top-level field"); ok = false; }
    auto rec = doc.find("records");
    if (rec == doc.end()) { c.rep.Error(c.file, "records", "missing required field"); return nullptr; }
    if (!rec->is_array()) { c.rep.Error(c.file, "records", std::string("expected array, got ") + TypeName(*rec)); return nullptr; }
    return ok ? &*rec : nullptr;
}

std::vector<fs::path> ListJson(const fs::path& dir) {
    std::vector<fs::path> v;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return v;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        const std::string n = e.path().filename().string();
        if (n.empty() || n[0] == '_' || n[0] == '.' || Low(e.path().extension().string()) != ".json") continue;   // "_name.json" = disabled / example
        v.push_back(e.path());
    }
    std::sort(v.begin(), v.end());
    return v;
}


// ------------------------------------------------------------------ dialogs (data/dialogs, docs/dialogs.md)

const char* const kDlgStats[] = {"strength", "perception", "endurance", "charisma", "intelligence", "agility", "luck", "level", "hp", "max_hp", "xp"};

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

bool ValidName(const std::string& s, size_t maxLen = 64) {   // flag / node / answer names
    if (s.empty() || s.size() > maxLen) return false;
    for (unsigned char c : s) if (!(std::isalnum(c) || c == '_' || c == '.' || c == '-')) return false;
    return true;
}

// "skill:Barter>=40", "stat:strength>=6", "item:Junk.ITM>=2", "quest:id==done", "flag:name", "flag:count>=3", "lua:fn_name"; leading '!' negates.
std::optional<DlgCond> ParseCondString(Ctx& c, const std::string& path, const std::string& in) {
    DlgCond d;
    std::string s = Trim(in);
    while (!s.empty() && s[0] == '!') { d.neg = !d.neg; s = Trim(s.substr(1)); }
    const size_t colon = s.find(':');
    if (colon == std::string::npos) { c.Err(path, "condition '" + in + "' must look like kind:argument (skill, stat, item, quest, flag, lua)"); return std::nullopt; }
    const std::string kind = Low(Trim(s.substr(0, colon)));
    std::string rest = Trim(s.substr(colon + 1));
    bool haveOp = false;
    std::string name = rest, rhs;
    for (size_t i = 0; i < rest.size(); ++i) {
        const char ch = rest[i], nx = i + 1 < rest.size() ? rest[i + 1] : 0;
        DlgCond::Op op = DlgCond::Ge;
        size_t len = 0;
        if (ch == '>' && nx == '=') { op = DlgCond::Ge; len = 2; }
        else if (ch == '<' && nx == '=') { op = DlgCond::Le; len = 2; }
        else if (ch == '=' && nx == '=') { op = DlgCond::Eq; len = 2; }
        else if (ch == '!' && nx == '=') { op = DlgCond::Ne; len = 2; }
        else if (ch == '>') { op = DlgCond::Gt; len = 1; }
        else if (ch == '<') { op = DlgCond::Lt; len = 1; }
        else if (ch == '=') { op = DlgCond::Eq; len = 1; }
        else continue;
        d.op = op;
        haveOp = true;
        name = Trim(rest.substr(0, i));
        rhs = Trim(rest.substr(i + len));
        break;
    }
    if (name.empty()) { c.Err(path, "condition '" + in + "': missing name after '" + kind + ":'"); return std::nullopt; }
    auto number = [&](int lo, int hi, int& out) {
        if (rhs.empty() || rhs.size() > 9 || rhs.find_first_not_of("-0123456789") != std::string::npos || rhs.find('-', 1) != std::string::npos) { c.Err(path, "condition '" + in + "': '" + rhs + "' is not a number"); return false; }
        const int v = std::atoi(rhs.c_str());
        if (v < lo || v > hi) { c.Err(path, "condition '" + in + "': " + std::to_string(v) + " out of range " + std::to_string(lo) + ".." + std::to_string(hi)); return false; }
        out = v;
        return true;
    };
    if (kind == "skill") {
        d.kind = DlgCond::Skill;
        int idx = -1;
        for (int i = 0; i < kSkillCount; ++i) if (Low(kSkillNames[i]) == Low(name)) idx = i;
        if (idx < 0) { c.Err(path, "condition '" + in + "': unknown skill '" + name + "'"); return std::nullopt; }
        d.name = kSkillNames[idx];
        if (!haveOp) { c.Err(path, "condition '" + in + "': needs a comparison, e.g. skill:" + d.name + ">=40"); return std::nullopt; }
        if (!number(0, 300, d.value)) return std::nullopt;
    } else if (kind == "stat") {
        d.kind = DlgCond::Stat;
        const std::string n = Low(name);
        bool ok = false;
        for (const char* k : kDlgStats) if (n == k) ok = true;
        if (!ok) { c.Err(path, "condition '" + in + "': unknown stat '" + name + "' (valid: strength, perception, endurance, charisma, intelligence, agility, luck, level, hp, max_hp, xp)"); return std::nullopt; }
        d.name = n;
        if (!haveOp) { c.Err(path, "condition '" + in + "': needs a comparison, e.g. stat:" + n + ">=5"); return std::nullopt; }
        if (!number(0, 1000000, d.value)) return std::nullopt;
    } else if (kind == "item") {
        d.kind = DlgCond::Item;
        d.name = name;
        if (!haveOp) { d.op = DlgCond::Ge; d.value = 1; }
        else if (!number(0, 100000, d.value)) return std::nullopt;
    } else if (kind == "quest") {
        d.kind = DlgCond::Quest;
        d.name = name;
        if (!ValidName(name, 48)) { c.Err(path, "condition '" + in + "': invalid quest id"); return std::nullopt; }
        if (!haveOp || (d.op != DlgCond::Eq && d.op != DlgCond::Ne)) { c.Err(path, "condition '" + in + "': use quest:<id>==state or !=state (none, active, done, failed)"); return std::nullopt; }
        const std::string st = Low(rhs);
        if (st == "none") d.value = -1;
        else if (st == "active") d.value = 0;
        else if (st == "done") d.value = 1;
        else if (st == "failed") d.value = 2;
        else { c.Err(path, "condition '" + in + "': unknown quest state '" + rhs + "' (none, active, done, failed)"); return std::nullopt; }
    } else if (kind == "flag") {
        d.kind = DlgCond::Flag;
        d.name = name;
        if (!ValidName(name)) { c.Err(path, "condition '" + in + "': invalid flag name '" + name + "' (letters, digits, _ . -)"); return std::nullopt; }
        if (!haveOp) { d.op = DlgCond::Ne; d.value = 0; }
        else if (!number(-1000000, 1000000, d.value)) return std::nullopt;
    } else if (kind == "lua") {
        d.kind = DlgCond::Lua;
        d.name = name;
        if (haveOp || !ValidHook(name)) { c.Err(path, "condition '" + in + "': lua:<function_name> expects a Lua function name"); return std::nullopt; }
    } else {
        c.Err(path, "condition '" + in + "': unknown kind '" + kind + "' (skill, stat, item, quest, flag, lua)");
        return std::nullopt;
    }
    return d;
}

std::optional<DlgCond> ParseCond(Ctx& c, const std::string& path, const json& j, int depth = 0) {
    if (depth > 6) { c.Err(path, "conditions nested too deeply"); return std::nullopt; }
    if (j.is_string()) return ParseCondString(c, path, j.get<std::string>());
    DlgCond d;
    if (j.is_array()) {
        d.kind = DlgCond::All;
        if (j.size() > 16) { c.Err(path, "more than 16 conditions"); return std::nullopt; }
        for (size_t i = 0; i < j.size(); ++i)
            if (auto k = ParseCond(c, path + "[" + std::to_string(i) + "]", j[i], depth + 1)) d.kids.push_back(std::move(*k));
        return d;
    }
    if (j.is_object() && j.size() == 1) {
        const std::string key = j.begin().key();
        const json& v = j.begin().value();
        if (key == "all" || key == "any") {
            d.kind = key == "all" ? DlgCond::All : DlgCond::Any;
            if (!v.is_array() || v.empty() || v.size() > 16) { c.Err(path + "." + key, "expected a non-empty array (at most 16 conditions)"); return std::nullopt; }
            for (size_t i = 0; i < v.size(); ++i)
                if (auto k = ParseCond(c, path + "." + key + "[" + std::to_string(i) + "]", v[i], depth + 1)) d.kids.push_back(std::move(*k));
            return d;
        }
        if (key == "not") {
            d.kind = DlgCond::Not;
            if (auto k = ParseCond(c, path + ".not", v, depth + 1)) d.kids.push_back(std::move(*k));
            else return std::nullopt;
            return d;
        }
    }
    c.Err(path, std::string("expected a condition string, an array (all of) or {\"any\": [...]} / {\"all\": [...]} / {\"not\": cond}, got ") + TypeName(j));
    return std::nullopt;
}

void ParseDlgEffects(Ctx& c, const std::string& path, const json* arr, std::vector<DlgEffect>& out) {
    if (!arr) return;
    if (arr->size() > 16) { c.Err(path, "more than 16 effects"); return; }
    static const char* const kActs[] = {"give", "take", "quest", "flag", "xp", "trade", "attack", "end", "door", "lua"};
    for (size_t i = 0; i < arr->size(); ++i) {
        const std::string ep = path + "[" + std::to_string(i) + "]";
        const json& j = (*arr)[i];
        if (!j.is_object()) { c.Err(ep, std::string("expected object, got ") + TypeName(j)); continue; }
        std::vector<std::string> present;
        for (const char* a : kActs) if (j.contains(a)) present.push_back(a);
        if (present.size() != 1) { c.Err(ep, "an effect needs exactly one of: give, take, quest, flag, xp, trade, attack, end, door, lua"); continue; }
        Obj e(c, j, ep);
        DlgEffect fx;
        const std::string& a = present[0];
        if (a == "give" || a == "take") {
            fx.kind = a == "give" ? DlgEffect::Give : DlgEffect::Take;
            if (auto s = e.Str(a.c_str(), true, 128)) fx.s = *s;
            fx.n = e.Int("count", 1, 9999).value_or(1);
        } else if (a == "quest") {
            fx.kind = DlgEffect::Quest;
            if (auto s = e.Str("quest", true, 48)) fx.s = *s;
            if (auto st = e.Str("state", true, 16)) {
                fx.action = Low(*st);
                if (fx.action != "add" && fx.action != "done" && fx.action != "fail" && fx.action != "reopen") c.Err(ep + ".state", "expected add, done, fail or reopen");
            }
            if (!fx.s.empty() && !ValidName(fx.s, 48)) c.Err(ep + ".quest", "invalid quest id");
        } else if (a == "flag") {
            fx.kind = DlgEffect::Flag;
            if (auto s = e.Str("flag", true, 64)) { fx.s = *s; if (!ValidName(fx.s)) c.Err(ep + ".flag", "invalid flag name (letters, digits, _ . -)"); }
            fx.n = 1;
            if (const json* v = e.Get("set")) {
                if (v->is_boolean()) fx.n = v->get<bool>() ? 1 : 0;
                else if (v->is_number_integer() && v->get<long long>() >= -1000000 && v->get<long long>() <= 1000000) fx.n = v->get<int>();
                else c.Err(ep + ".set", "expected true, false or an integer");
            }
        } else if (a == "xp") {
            fx.kind = DlgEffect::Xp;
            fx.n = e.Int("xp", 1, 100000, true).value_or(0);
        } else if (a == "trade" || a == "attack" || a == "end") {
            fx.kind = a == "trade" ? DlgEffect::Trade : a == "attack" ? DlgEffect::Attack : DlgEffect::End;
            auto b = e.Bool(a.c_str());
            if (b && !*b) c.Err(ep + "." + a, "expected true");
        } else if (a == "door") {
            fx.kind = DlgEffect::Door;
            const json* cell = e.Get("door");
            if (!cell || !cell->is_array() || cell->size() != 2 || !(*cell)[0].is_number_integer() || !(*cell)[1].is_number_integer() || (*cell)[0].get<int>() < 0 || (*cell)[1].get<int>() < 0)
                c.Err(ep + ".door", "expected [x, y] (a cell of the speaker's map)");
            else { fx.x = (*cell)[0].get<int>(); fx.y = (*cell)[1].get<int>(); }
            if (auto act = e.Str("action", true, 16)) {
                fx.action = Low(*act);
                if (fx.action != "open" && fx.action != "close" && fx.action != "lock" && fx.action != "unlock") c.Err(ep + ".action", "expected open, close, lock or unlock");
            }
        } else {
            fx.kind = DlgEffect::Lua;
            if (auto s = e.Str("lua", true, 64)) { fx.s = *s; if (!ValidHook(fx.s)) c.Err(ep + ".lua", "'" + fx.s + "' is not a valid Lua function name"); }
        }
        e.Done();
        out.push_back(std::move(fx));
    }
}

// text / stf of a node or answer: exactly one of the two
void ParseDlgText(Obj& o, const std::string& path, std::string& text, int& stf) {
    auto t = o.Str("text", false, 1500);
    auto s = o.Int("stf", 1, 1000000);
    if (o.Has("text") && o.Has("stf")) o.c.Err(path, "give either \"text\" or \"stf\", not both");
    else if (!o.Has("text") && !o.Has("stf")) o.c.Err(path, "missing \"text\" (our own text) or \"stf\" (id of an original English.stf string)");
    if (t) text = *t;
    if (s) stf = *s;
    if (t && Trim(*t).empty()) o.c.Err(path + ".text", "must not be empty");
}

void CollectCondRefs(const DlgCond& d, std::vector<const DlgCond*>& out) {
    out.push_back(&d);
    for (const DlgCond& k : d.kids) CollectCondRefs(k, out);
}

struct Loader {
    const fs::path dir;
    const std::string root;   // folder name used in messages
    ContentReport& rep;
    ContentDb& db;
    std::map<std::string, std::string> firstSeen;   // "kind:lowerid" -> "file path" (duplicate detection)

    Loader(const fs::path& d, ContentReport& r, ContentDb& o) : dir(d), root(d.filename().generic_string().empty() ? "data" : d.filename().generic_string()), rep(r), db(o) {}

    std::string Rel(const char* kind, const fs::path& p) { return root + "/" + kind + "/" + p.filename().generic_string(); }

    bool Dup(Ctx& c, const char* kind, const std::string& id, const std::string& path) {
        const std::string k = std::string(kind) + ":" + Low(id);
        auto it = firstSeen.find(k);
        if (it != firstSeen.end()) { c.Err(path + ".id", "duplicate id '" + id + "' (first defined in " + it->second + ")"); return true; }
        firstSeen[k] = c.file + " " + path;
        return false;
    }

    template <class F>
    void EachFile(const char* kind, std::set<std::string> extra, F&& perRecord, std::function<void(Ctx&, const json&)> onDoc = nullptr) {
        for (const fs::path& p : ListJson(dir / kind)) {
            db.files.push_back(Rel(kind, p));
            Ctx c{rep, Rel(kind, p)};
            json doc;
            const json* recs = OpenEnvelope(c, p, kind, doc, extra);
            if (onDoc) onDoc(c, doc);
            if (!recs) continue;
            for (size_t i = 0; i < recs->size(); ++i) {
                const std::string path = "records[" + std::to_string(i) + "]";
                if (!(*recs)[i].is_object()) { c.rep.Error(c.file, path, std::string("expected object, got ") + TypeName((*recs)[i])); continue; }
                c.bad = false;
                perRecord(c, (*recs)[i], path);
            }
        }
    }

    // ---- skills
    void LoadSkills() {
        EachFile("skills", {}, [&](Ctx& c, const json& j, const std::string& path) {
            Obj o(c, j, path);
            auto id = o.Str("id", true, 64);
            auto desc = o.Str("desc", false, 600);
            const json* base = o.ObjField("base");
            if (const json* orig = o.ObjField("original")) {   // the original game's bonus formula, reference only (never applied)
                Obj oo(c, *orig, path + ".original");
                oo.Int("const", -1000, 1000);
                for (int i = 0; i < kSpecialCount; ++i) oo.Int(kSpecialKeys[i], -100, 100);
                oo.Done();
            }
            SkillDef d;
            if (desc) d.desc = *desc;
            bool haveBase = false;
            int constant = 0, coef[kSpecialCount] = {};
            if (base) {
                Obj b(c, *base, path + ".base");
                if (auto v = b.Int("const", -1000, 1000)) constant = *v;
                for (int i = 0; i < kSpecialCount; ++i) if (auto v = b.Int(kSpecialKeys[i], -100, 100)) coef[i] = *v;
                b.Done();
                haveBase = true;
            }
            o.Done();
            if (!id) return;
            int idx = -1;
            for (int i = 0; i < kSkillCount; ++i) if (Low(kSkillNames[i]) == Low(*id)) idx = i;
            if (idx < 0) { c.Err(path + ".id", "unknown skill '" + *id + "' (the skill list is fixed: " + [] { std::string s; for (int i = 0; i < kSkillCount; ++i) s += std::string(i ? ", " : "") + kSkillNames[i]; return s; }() + ")"); return; }
            if (Dup(c, "skills", kSkillNames[idx], path) || c.bad) return;
            if (haveBase) { db.skills[idx].constant = constant; for (int i = 0; i < kSpecialCount; ++i) db.skills[idx].coef[i] = coef[i]; }
            if (desc) db.skills[idx].desc = *desc;
            db.fingerprint[std::string("skills:") + kSkillNames[idx]] = j.dump();
        });
    }

    // ---- traits
    void LoadTraits() {
        std::set<int> indices;
        EachFile("traits", {"max_picks"}, [&](Ctx& c, const json& j, const std::string& path) {
            Obj o(c, j, path);
            auto id = o.Str("id", true, 64);
            auto name = o.Str("name", true, 64);
            auto desc = o.Str("desc", false, 800);
            auto index = o.Int("index", 0, 15, true);
            auto hook = o.Str("hook", false, 64);
            TraitDef t;
            t.effects = ParseEffects(o);
            o.Done();
            if (id && !ValidId(*id)) c.Err(path + ".id", "invalid id '" + *id + "' (letters, digits, _ . - only, max 64)");
            if (hook && !hook->empty() && !ValidHook(*hook)) c.Err(path + ".hook", "'" + *hook + "' is not a valid Lua function name");
            if (!id || !name || !index) return;
            if (Dup(c, "traits", *id, path)) return;
            if (indices.count(*index)) { c.Err(path + ".index", "trait index " + std::to_string(*index) + " is already used by another trait"); return; }
            indices.insert(*index);
            if (c.bad) return;
            t.id = *id; t.name = *name; t.desc = desc.value_or(""); t.index = *index; t.hook = hook.value_or(""); t.file = c.file;
            db.fingerprint["traits:" + Low(t.id)] = j.dump();
            db.traits.push_back(std::move(t));
        }, [&](Ctx& c, const json& doc) {
            auto it = doc.find("max_picks");
            if (it == doc.end()) return;
            if (!it->is_number_integer() || it->get<int>() < 0 || it->get<int>() > 16) { c.rep.Error(c.file, "max_picks", "expected integer 0..16"); return; }
            const int v = it->get<int>();
            if (db.hasTraitMax && db.traitMax != v) { c.rep.Error(c.file, "max_picks", "conflicts with max_picks " + std::to_string(db.traitMax) + " of another traits file"); return; }
            db.hasTraitMax = true; db.traitMax = v;
        });
    }

    // ---- perks
    void LoadPerks() {
        EachFile("perks", {}, [&](Ctx& c, const json& j, const std::string& path) {
            Obj o(c, j, path);
            auto id = o.Str("id", true, 64);
            auto name = o.Str("name", true, 64);
            auto desc = o.Str("desc", false, 800);
            auto ranks = o.Int("ranks", 1, 10);
            auto hook = o.Str("hook", false, 64);
            auto placeholder = o.Bool("placeholder");
            PerkDef p;
            p.effects = ParseEffects(o);
            if (const json* r = o.ObjField("requires")) {
                Obj q(c, *r, path + ".requires");
                if (auto v = q.Int("level", 1, 99)) p.req.level = *v;
                if (const json* s = q.ObjField("special")) {
                    Obj so(c, *s, path + ".requires.special");
                    for (int i = 0; i < kSpecialCount; ++i) if (auto v = so.Int(kSpecialKeys[i], 1, 10)) p.req.special[i] = *v;
                    so.Done();
                }
                if (const json* s = q.ObjField("skills")) {
                    for (auto it = s->begin(); it != s->end(); ++it) {
                        const std::string sp = path + ".requires.skills." + it.key();
                        int idx = -1;
                        for (int i = 0; i < kSkillCount; ++i) if (Low(kSkillNames[i]) == Low(it.key())) idx = i;
                        if (idx < 0) { c.Err(sp, "unknown skill '" + it.key() + "'"); continue; }
                        if (!it.value().is_number_integer() || it.value().get<int>() < 1 || it.value().get<int>() > 300) { c.Err(sp, "expected integer 1..300"); continue; }
                        p.req.skills.emplace_back(idx, it.value().get<int>());
                    }
                }
                if (auto v = q.StrList("perks", 20)) p.req.perks = *v;
                if (auto v = q.StrList("traits", 20)) p.req.traits = *v;
                q.Done();
            }
            o.Done();
            if (id && !ValidId(*id)) c.Err(path + ".id", "invalid id '" + *id + "' (letters, digits, _ . - only, max 64)");
            if (hook && !hook->empty() && !ValidHook(*hook)) c.Err(path + ".hook", "'" + *hook + "' is not a valid Lua function name");
            if (!id || !name) return;
            if (Dup(c, "perks", *id, path) || c.bad) return;
            p.id = *id; p.name = *name; p.desc = desc.value_or(""); p.ranks = ranks.value_or(1); p.hook = hook.value_or(""); p.placeholder = placeholder.value_or(false); p.file = c.file;
            db.fingerprint["perks:" + Low(p.id)] = j.dump();
            db.perks.push_back(std::move(p));
        });
        // references between perks / traits (all files known now); a perk with a broken reference is dropped
        std::vector<PerkDef> keep;
        for (size_t i = 0; i < db.perks.size(); ++i) {
            PerkDef& p = db.perks[i];
            Ctx c{rep, p.file};
            for (const std::string& r : p.req.perks) {
                if (!db.FindPerk(r)) c.Err("perk '" + p.id + "'.requires.perks", "unknown perk '" + r + "'");
                else if (Low(r) == Low(p.id)) c.Err("perk '" + p.id + "'.requires.perks", "a perk cannot require itself");
            }
            for (const std::string& r : p.req.traits) if (!db.FindTrait(r)) c.Err("perk '" + p.id + "'.requires.traits", "unknown trait '" + r + "'");
            if (!c.bad) keep.push_back(p);
            else db.fingerprint.erase("perks:" + Low(p.id));
        }
        db.perks = std::move(keep);
        // requirement cycles (A needs B needs A)
        for (const PerkDef& p : db.perks) {
            std::set<std::string> seen;
            std::vector<std::string> stack{Low(p.id)};
            bool cyc = false;
            while (!stack.empty() && !cyc) {
                const PerkDef* cur = db.FindPerk(stack.back());
                stack.pop_back();
                if (!cur) continue;
                for (const std::string& r : cur->req.perks) {
                    if (Low(r) == Low(p.id)) { cyc = true; break; }
                    if (seen.insert(Low(r)).second) stack.push_back(Low(r));
                }
            }
            if (cyc) rep.Error(p.file, "perk '" + p.id + "'.requires.perks", "requirement cycle back to this perk");
        }
    }

    // ---- quests
    void LoadQuests() {
        EachFile("quests", {}, [&](Ctx& c, const json& j, const std::string& path) {
            Obj o(c, j, path);
            auto id = o.Str("id", true, 48);
            auto title = o.Str("title", true, 80);
            auto text = o.Str("text", false, 600);
            auto xp = o.Int("xp", 0, 1000000);
            QuestDef q;
            if (const json* a = o.ArrField("items")) {
                for (size_t i = 0; i < a->size(); ++i) {
                    const std::string ip = path + ".items[" + std::to_string(i) + "]";
                    if (!(*a)[i].is_object()) { c.Err(ip, std::string("expected object, got ") + TypeName((*a)[i])); continue; }
                    Obj e(c, (*a)[i], ip);
                    auto item = e.Str("item", true, 128);
                    auto cnt = e.Int("count", 1, 999);
                    e.Done();
                    if (item) q.items.emplace_back(*item, cnt.value_or(1));
                }
            }
            o.Done();
            if (id && !ValidId(*id)) c.Err(path + ".id", "invalid id '" + *id + "' (letters, digits, _ . - only, max 48)");
            if (!id || !title) return;
            if (Dup(c, "quests", *id, path) || c.bad) return;
            q.id = *id; q.title = *title; q.text = text.value_or(""); q.xp = xp.value_or(0); q.file = c.file;
            db.fingerprint["quests:" + Low(q.id)] = j.dump();
            db.quests.push_back(std::move(q));
        });
    }

    // ---- dialogs (needs the quests: references are checked against data/quests; item / speaker / stf references in CheckDialogs)
    void LoadDialogs() {
        EachFile("dialogs", {}, [&](Ctx& c, const json& j, const std::string& path) {
            Obj o(c, j, path);
            DialogDef d;
            auto id = o.Str("id", true, 64);
            if (auto v = o.Str("speaker", false, 128)) d.speaker = *v;
            std::optional<std::string> startStr;
            const json* startArr = nullptr;
            if (const json* s = o.Get("start")) {
                if (s->is_string()) startStr = s->get<std::string>();
                else if (s->is_array()) startArr = s;
                else c.Err(path + ".start", std::string("expected a node id or an array of {if, goto}, got ") + TypeName(*s));
            }
            const json* nodes = o.ArrField("nodes");
            if (nodes && nodes->empty()) c.Err(path + ".nodes", "a dialog needs at least one node");
            else if (!nodes && !o.Has("nodes")) c.Err(path + ".nodes", "missing required field");
            else if (nodes && nodes->size() > 200) c.Err(path + ".nodes", "more than 200 nodes");
            std::set<std::string> nodeIds;
            if (nodes && nodes->size() <= 200)
                for (size_t i = 0; i < nodes->size(); ++i) {
                    const std::string np = path + ".nodes[" + std::to_string(i) + "]";
                    if (!(*nodes)[i].is_object()) { c.Err(np, std::string("expected object, got ") + TypeName((*nodes)[i])); continue; }
                    Obj n(c, (*nodes)[i], np);
                    DlgNode node;
                    auto nid = n.Str("id", true, 48);
                    ParseDlgText(n, np, node.text, node.stf);
                    ParseDlgEffects(c, np + ".on_enter", n.ArrField("on_enter"), node.onEnter);
                    if (const json* as = n.ArrField("answers")) {
                        if (as->size() > 24) c.Err(np + ".answers", "more than 24 answers");
                        std::set<std::string> answerIds;
                        for (size_t k = 0; k < as->size() && k < 24; ++k) {
                            const std::string ap = np + ".answers[" + std::to_string(k) + "]";
                            if (!(*as)[k].is_object()) { c.Err(ap, std::string("expected object, got ") + TypeName((*as)[k])); continue; }
                            Obj a(c, (*as)[k], ap);
                            DlgAnswer ans;
                            if (auto aid = a.Str("id", false, 48)) {
                                ans.id = *aid;
                                if (!ValidName(ans.id, 48)) c.Err(ap + ".id", "invalid id (letters, digits, _ . -)");
                                else if (!answerIds.insert(Low(ans.id)).second) c.Err(ap + ".id", "duplicate answer id '" + ans.id + "' in this node");
                            }
                            ParseDlgText(a, ap, ans.text, ans.stf);
                            if (const json* cj = a.Get("if")) {
                                if (auto cd = ParseCond(c, ap + ".if", *cj)) { ans.hasCond = true; ans.cond = std::move(*cd); }
                            }
                            ParseDlgEffects(c, ap + ".do", a.ArrField("do"), ans.effects);
                            if (auto g = a.Str("goto", false, 48)) ans.gotoNode = *g;
                            a.Done();
                            node.answers.push_back(std::move(ans));
                        }
                    }
                    n.Done();
                    if (!nid) continue;
                    node.id = *nid;
                    if (!ValidName(node.id, 48)) { c.Err(np + ".id", "invalid node id '" + node.id + "' (letters, digits, _ . -)"); continue; }
                    if (Low(node.id) == "end") { c.Err(np + ".id", "'end' is reserved (goto \"end\" closes the dialog)"); continue; }
                    if (!nodeIds.insert(Low(node.id)).second) { c.Err(np + ".id", "duplicate node id '" + node.id + "'"); continue; }
                    d.nodes.push_back(std::move(node));
                }
            if (startArr) {
                if (startArr->empty() || startArr->size() > 16) c.Err(path + ".start", "expected 1..16 routes");
                for (size_t i = 0; i < startArr->size() && i < 16; ++i) {
                    const std::string sp = path + ".start[" + std::to_string(i) + "]";
                    if (!(*startArr)[i].is_object()) { c.Err(sp, std::string("expected object, got ") + TypeName((*startArr)[i])); continue; }
                    Obj r(c, (*startArr)[i], sp);
                    DlgRoute rt;
                    if (const json* cj = r.Get("if")) {
                        if (auto cd = ParseCond(c, sp + ".if", *cj)) { rt.hasCond = true; rt.cond = std::move(*cd); }
                    }
                    if (auto g = r.Str("goto", true, 48)) rt.gotoNode = *g;
                    r.Done();
                    d.start.push_back(std::move(rt));
                }
            } else if (startStr) {
                DlgRoute rt;
                rt.gotoNode = *startStr;
                d.start.push_back(rt);
            } else if (!d.nodes.empty()) {   // default: the node called "start", else the first one
                DlgRoute rt;
                rt.gotoNode = d.nodes.front().id;
                for (const DlgNode& n : d.nodes) if (Low(n.id) == "start") rt.gotoNode = n.id;
                d.start.push_back(rt);
            }
            o.Done();
            if (id && !ValidId(*id)) c.Err(path + ".id", "invalid id '" + *id + "' (letters, digits, _ . - only, max 64)");
            if (!id) return;
            // references inside the dialog
            auto nodeList = [&] { std::string s; for (const DlgNode& n : d.nodes) s += (s.empty() ? "" : ", ") + n.id; return s; };
            auto checkGoto = [&](const std::string& g, const std::string& where) {
                if (g.empty() || Low(g) == "end") return;
                if (!nodeIds.count(Low(g))) c.Err(where, "unknown node '" + g + "' (nodes of this dialog: " + nodeList() + ")");
            };
            for (size_t i = 0; i < d.start.size(); ++i) {
                const std::string sp = path + ".start[" + std::to_string(i) + "].goto";
                if (Low(d.start[i].gotoNode) == "end") c.Err(sp, "a dialog cannot start with 'end'");
                else checkGoto(d.start[i].gotoNode, sp);
            }
            std::set<std::string> reach;
            for (const DlgRoute& r : d.start) reach.insert(Low(r.gotoNode));
            std::vector<const DlgCond*> conds;
            auto scanFx = [&](const std::vector<DlgEffect>& fx, const std::string& where) {
                for (const DlgEffect& e : fx)
                    if (e.kind == DlgEffect::Quest && !db.FindQuest(e.s)) c.Err(where, "unknown quest '" + e.s + "' (define it in data/quests)");
            };
            for (size_t i = 0; i < d.nodes.size(); ++i) {
                const std::string np = path + ".nodes[" + std::to_string(i) + "]";
                scanFx(d.nodes[i].onEnter, np + ".on_enter");
                for (size_t k = 0; k < d.nodes[i].answers.size(); ++k) {
                    const DlgAnswer& a = d.nodes[i].answers[k];
                    const std::string ap = np + ".answers[" + std::to_string(k) + "]";
                    checkGoto(a.gotoNode, ap + ".goto");
                    reach.insert(Low(a.gotoNode));
                    scanFx(a.effects, ap + ".do");
                    if (a.hasCond) CollectCondRefs(a.cond, conds);
                    for (const DlgCond* cd : conds)
                        if (cd->kind == DlgCond::Quest && !db.FindQuest(cd->name)) c.Err(ap + ".if", "unknown quest '" + cd->name + "' (define it in data/quests)");
                    conds.clear();
                }
            }
            for (const DlgRoute& r : d.start) if (r.hasCond) CollectCondRefs(r.cond, conds);
            for (const DlgCond* cd : conds)
                if (cd->kind == DlgCond::Quest && !db.FindQuest(cd->name)) c.Err(path + ".start", "unknown quest '" + cd->name + "' (define it in data/quests)");
            if (Dup(c, "dialogs", *id, path) || c.bad) return;
            for (const DlgNode& n : d.nodes) if (!reach.count(Low(n.id))) rep.Warn(c.file, path, "dialog '" + *id + "': node '" + n.id + "' is never reached");
            d.id = *id;
            d.file = c.file;
            db.fingerprint["dialogs:" + Low(d.id)] = j.dump();
            db.dialogs.push_back(std::move(d));
        });
    }

    // ---- items / npcs (structure only; applied to the prototypes in BuildProtoSet)
    void LoadItems() {
        EachFile("items", {}, [&](Ctx& c, const json& j, const std::string& path) {
            Obj o(c, j, path);
            ItemRec r;
            r.file = c.file; r.path = path;
            auto id = o.Str("id", true, 128);
            if (auto v = o.Str("base", false, 128)) r.base = *v;
            r.name = o.Str("name", false, 120);
            r.desc = o.Str("desc", false, 1200);
            r.weight = o.Int("weight", 0, 10000);
            r.headArmor = o.Bool("head_armor");
            r.model = o.Str("model", false, 128);
            r.heldModel = o.Str("held_model", false, 128);
            if (const json* w = o.ObjField("weapon")) {
                Obj wo(c, *w, path + ".weapon");
                r.range = wo.Int("range", 1, 200);
                r.dmgMin = wo.Num("dmg_min", 0, 10000);
                r.dmgMax = wo.Num("dmg_max", 0, 10000);
                r.caliber = wo.Int("caliber", -1, 200);
                if (const json* ms = wo.ArrField("modes")) {
                    std::vector<AttackModeDef> modes;
                    if (ms->size() > 8) c.Err(path + ".weapon.modes", "more than 8 attack modes");
                    for (size_t i = 0; i < ms->size() && i < 8; ++i) {
                        const std::string mp = path + ".weapon.modes[" + std::to_string(i) + "]";
                        if (!(*ms)[i].is_object()) { c.Err(mp, std::string("expected object, got ") + TypeName((*ms)[i])); continue; }
                        Obj mo(c, (*ms)[i], mp);
                        AttackModeDef a;
                        auto type = mo.Int("type", 0, 99, true);
                        auto cost = mo.Int("cost", 0, 10000, true);
                        a.dmgMin = mo.Int("dmg_min", 0, 10000).value_or(0);
                        a.dmgMax = mo.Int("dmg_max", 0, 10000).value_or(0);
                        a.apRef = mo.Int("ap", 0, 100).value_or(-1);
                        a.shots = mo.Int("shots", 0, 50).value_or(0);
                        a.damageType = mo.Int("damage_type", 0, 5).value_or(0);
                        mo.Done();
                        if (a.dmgMin > a.dmgMax) c.Err(mp, "dmg_min is larger than dmg_max");
                        if (type && cost) { a.type = *type; a.ap = *cost; modes.push_back(a); }
                    }
                    r.modes = std::move(modes);
                }
                wo.Done();
                if (r.dmgMin && r.dmgMax && *r.dmgMin > *r.dmgMax) c.Err(path + ".weapon", "dmg_min is larger than dmg_max");
            }
            if (const json* w = o.ObjField("armor")) {
                Obj ao(c, *w, path + ".armor");
                if (const json* v = ao.ArrField("values")) {
                    std::vector<int> vals;
                    if (v->size() != 7) c.Err(path + ".armor.values", "expected 7 integers, got " + std::to_string(v->size()));
                    else for (size_t i = 0; i < 7; ++i) {
                        if (!(*v)[i].is_number_integer() || (*v)[i].get<int>() < 0 || (*v)[i].get<int>() > 1000) c.Err(path + ".armor.values[" + std::to_string(i) + "]", "expected integer 0..1000");
                        else vals.push_back((*v)[i].get<int>());
                    }
                    if (vals.size() == 7) r.armor = vals;
                }
                ao.Done();
            }
            if (const json* w = o.ObjField("ammo")) {
                Obj ao(c, *w, path + ".ammo");
                r.ammoDmgMin = ao.Int("dmg_min", 0, 10000);
                r.ammoDmgMax = ao.Int("dmg_max", 0, 10000);
                r.ammoCaliber = ao.Int("caliber", 0, 200);
                r.ammoArmorAdj = ao.Int("armor_adj", -1000, 1000);
                r.ammoAccuracyAdj = ao.Int("accuracy_adj", -1000, 1000);
                ao.Done();
                if (r.ammoDmgMin && r.ammoDmgMax && *r.ammoDmgMin > *r.ammoDmgMax) c.Err(path + ".ammo", "dmg_min is larger than dmg_max");
            }
            o.Done();
            if (!id) return;
            if (Dup(c, "protos", *id, path) || c.bad) return;
            r.id = *id;
            db.fingerprint["items:" + Low(r.id)] = j.dump();
            db.items.push_back(std::move(r));
        });
    }

    void LoadNpcs() {
        EachFile("npcs", {}, [&](Ctx& c, const json& j, const std::string& path) {
            Obj o(c, j, path);
            NpcRec r;
            r.file = c.file; r.path = path;
            auto id = o.Str("id", true, 128);
            if (auto v = o.Str("base", false, 128)) r.base = *v;
            r.name = o.Str("name", false, 120);
            r.script = o.Str("script", false, 128);
            r.dialog = o.Str("dialog", false, 128);
            r.model = o.Str("model", false, 128);
            r.team = o.Int("team", 0, 255);
            r.level = o.Int("level", 1, 99);
            r.maxHp = o.Int("max_hp", 1, 9999);
            r.xp = o.Int("xp", 0, 1000000);
            if (const json* s = o.ObjField("special")) {
                Obj so(c, *s, path + ".special");
                for (int i = 0; i < kSpecialCount; ++i) if (auto v = so.Int(kSpecialKeys[i], 1, 10)) r.special[i] = *v;
                so.Done();
            }
            r.equipment = o.StrList("equipment", 50);
            r.inventory = o.StrList("inventory", 200);
            if (const json* a = o.ArrField("spawns")) {
                for (size_t i = 0; i < a->size(); ++i) {
                    const std::string sp = path + ".spawns[" + std::to_string(i) + "]";
                    if (!(*a)[i].is_object()) { c.Err(sp, std::string("expected object, got ") + TypeName((*a)[i])); continue; }
                    Obj so(c, (*a)[i], sp);
                    auto map = so.Str("map", true, 128);
                    auto cnt = so.Int("count", 1, 50);
                    NpcSpawn s;
                    if (const json* cell = so.Get("cell")) {
                        if (!cell->is_array() || cell->size() != 2 || !(*cell)[0].is_number_integer() || !(*cell)[1].is_number_integer() || (*cell)[0].get<int>() < 0 || (*cell)[1].get<int>() < 0)
                            c.Err(sp + ".cell", "expected [x, y] with two non-negative integers");
                        else { s.x = (*cell)[0].get<int>(); s.y = (*cell)[1].get<int>(); }
                    } else c.Err(sp + ".cell", "missing required field");
                    so.Done();
                    if (map) { s.map = *map; s.count = cnt.value_or(1); r.spawns.push_back(s); }
                }
            }
            o.Done();
            if (!id) return;
            if (Dup(c, "protos", *id, path) || c.bad) return;
            r.id = *id;
            db.fingerprint["npcs:" + Low(r.id)] = j.dump();
            db.npcs.push_back(std::move(r));
        });
    }
};

// The effective prototype table: base + item / npc records. Errors name file and record path; a record with an error is not applied.
void BuildProtoSet(const DataStore& ds, ContentDb& db, ContentReport& rep) {
    ProtoSet& out = db.protoSet;
    out = ProtoSet{};
    out.protos = ds.BaseProtos();
    const size_t baseCount = out.protos.size();
    std::unordered_map<std::string, uint16_t> extraSlot;   // data-defined protos of earlier loads keep their pid
    const auto& cur = ds.AllProtos();
    for (size_t i = baseCount; i < cur.size(); ++i) {
        Proto p = cur[i];
        p.retired = true;
        extraSlot[Low(p.key)] = static_cast<uint16_t>(i);
        out.protos.push_back(std::move(p));
    }
    for (size_t i = 0; i < baseCount; ++i) out.byKey[Low(out.protos[i].key)] = static_cast<uint16_t>(i);

    auto find = [&](const std::string& key) -> Proto* {
        auto it = out.byKey.find(Low(key));
        return it == out.byKey.end() ? nullptr : &out.protos[it->second];
    };
    // returns the proto the record applies to (override or freshly cloned), nullptr after an error
    auto target = [&](Ctx& c, const std::string& path, const std::string& id, const std::string& baseKey, bool wantCritter, const char* kind) -> Proto* {
        Proto* ex = find(id);
        if (!baseKey.empty()) {
            if (ex) { c.Err(path + ".base", "'" + id + "' already exists (" + (ex->fromData ? "defined by another data record" : "original prototype") + "); remove \"base\" to override it"); return nullptr; }
            Proto* b = find(baseKey);
            if (!b) { c.Err(path + ".base", "unknown base prototype '" + baseKey + "'"); return nullptr; }
            if ((b->cls == PClass::Critter) != wantCritter) { c.Err(path + ".base", "'" + baseKey + "' is " + (wantCritter ? "not a critter" : "a critter") + "; this record kind is " + kind); return nullptr; }
            if (!ValidId(id)) { c.Err(path + ".id", "invalid id '" + id + "' (letters, digits, _ . - only)"); return nullptr; }
            Proto np = b->fromData ? *b : ds.BaseProtos()[b->pid];   // clones the ORIGINAL prototype (independent of record order); a data-defined base as defined so far
            uint16_t pid;
            auto slot = extraSlot.find(Low(id));
            if (slot != extraSlot.end()) { pid = slot->second; }
            else {
                // pids are u16 on the wire
                if (out.protos.size() >= 65000) { c.Err(path + ".id", "too many prototypes"); return nullptr; }
                pid = static_cast<uint16_t>(out.protos.size());
                out.protos.emplace_back();
            }
            np.pid = pid; np.key = id; np.fromData = true; np.retired = false;
            out.protos[pid] = std::move(np);
            out.byKey[Low(id)] = pid;
            return &out.protos[pid];
        }
        if (!ex) { c.Err(path + ".id", "unknown " + std::string(kind) + " id '" + id + "' (no such original prototype; add \"base\": \"<existing id>\" to define a new record)"); return nullptr; }
        if ((ex->cls == PClass::Critter) != wantCritter) { c.Err(path + ".id", "'" + id + "' is " + (wantCritter ? "not a critter" : "a critter") + "; put it in data/" + (wantCritter ? "items" : "npcs")); return nullptr; }
        return ex;
    };

    // pass 1: items
    for (const ItemRec& r : db.items) {
        Ctx c{rep, r.file};
        Proto* p = target(c, r.path, r.id, r.base, false, "item");
        if (!p) continue;
        const bool wantsWeapon = r.range || r.dmgMin || r.dmgMax || r.caliber || r.modes, wantsArmor = r.armor.has_value() || r.headArmor.has_value(),
                   wantsAmmo = r.ammoDmgMin || r.ammoDmgMax || r.ammoCaliber || r.ammoArmorAdj || r.ammoAccuracyAdj;
        if (wantsWeapon && p->cls != PClass::Weapon) c.Err(r.path + ".weapon", "'" + r.id + "' is not a weapon");
        if (wantsArmor && p->cls != PClass::Armor) c.Err(r.path + (r.armor ? ".armor" : ".head_armor"), "'" + r.id + "' is not armor");
        if (wantsAmmo && p->cls != PClass::Ammo) c.Err(r.path + ".ammo", "'" + r.id + "' is not ammunition");
        if (c.bad) {   // a new clone of a broken record must not linger
            if (!r.base.empty()) { out.byKey.erase(Low(r.id)); p->retired = true; }
            continue;
        }
        if (r.name) p->name = *r.name;
        if (r.desc) p->desc = *r.desc;
        if (r.weight) p->weightOverride = *r.weight;
        if (r.headArmor) p->headArmor = *r.headArmor;
        if (r.range) p->range = *r.range;
        if (r.dmgMin) p->wDmgMin = *r.dmgMin;
        if (r.dmgMax) p->wDmgMax = *r.dmgMax;
        if (r.armor) for (int i = 0; i < 7; ++i) p->armor[i] = (*r.armor)[i];
        if (r.ammoDmgMin) p->ammoDmgMin = *r.ammoDmgMin;
        if (r.ammoDmgMax) p->ammoDmgMax = *r.ammoDmgMax;
        if (r.model) p->model = *r.model;
        if (r.heldModel) p->heldModel = *r.heldModel;
        if (r.caliber) p->caliber = *r.caliber;
        if (r.modes) p->modes = *r.modes;
        if (r.ammoCaliber) p->ammoCaliber = *r.ammoCaliber;
        if (r.ammoArmorAdj) p->ammoArmorAdj = *r.ammoArmorAdj;
        if (r.ammoAccuracyAdj) p->ammoAccuracyAdj = *r.ammoAccuracyAdj;
    }
    // pass 2: npcs (their inventory / equipment may use items added in pass 1)
    const std::vector<std::string>& mapNames = ds.MapNames();
    for (const NpcRec& r : db.npcs) {
        Ctx c{rep, r.file};
        auto checkList = [&](const std::optional<std::vector<std::string>>& v, const char* field) {
            if (!v) return;
            for (size_t i = 0; i < v->size(); ++i) {
                const Proto* ip = find((*v)[i]);
                const std::string ep = r.path + "." + field + "[" + std::to_string(i) + "]";
                if (!ip) c.Err(ep, "unknown item '" + (*v)[i] + "'");
                else if (ip->cls == PClass::Critter) c.Err(ep, "'" + (*v)[i] + "' is a critter, not an item");
            }
        };
        checkList(r.equipment, "equipment");
        checkList(r.inventory, "inventory");
        for (size_t i = 0; i < r.spawns.size(); ++i)
            if (std::find(mapNames.begin(), mapNames.end(), r.spawns[i].map) == mapNames.end())
                c.Err(r.path + ".spawns[" + std::to_string(i) + "].map", "unknown map '" + r.spawns[i].map + "'");
        Proto* p = c.bad ? nullptr : target(c, r.path, r.id, r.base, true, "npc");
        if (!p || c.bad) continue;
        if (r.name) p->name = *r.name;
        if (r.script) p->script = *r.script;
        if (r.dialog) p->dialog = *r.dialog;
        if (r.model) p->model = *r.model;
        if (r.team) p->team = *r.team;
        if (r.level) p->level = *r.level;
        if (r.maxHp) p->maxHpFixed = *r.maxHp;
        if (r.xp) p->xp = *r.xp;
        for (int i = 0; i < kSpecialCount; ++i) if (r.special[i] > 0) p->special[i] = r.special[i];
        if (r.equipment) p->equipment = *r.equipment;
        if (r.inventory) p->inventory = *r.inventory;
    }
    db.protosBuilt = true;
}

// quest reward items must exist (only checkable with gamedata); a quest with a broken reference is dropped
void CheckQuestItems(ContentDb& db, ContentReport& rep, bool haveProtos) {
    std::vector<QuestDef> keep;
    for (QuestDef& q : db.quests) {
        Ctx c{rep, q.file};
        if (haveProtos)
            for (size_t i = 0; i < q.items.size(); ++i)
                if (!db.protoSet.byKey.count(Low(q.items[i].first)))
                    c.Err("quest '" + q.id + "'.items[" + std::to_string(i) + "].item", "unknown item '" + q.items[i].first + "'");
        if (!c.bad) keep.push_back(q);
        else db.fingerprint.erase("quests:" + Low(q.id));
    }
    db.quests = std::move(keep);
}

// dialogs: items, speaker prototype and original stf strings must exist (only checkable with gamedata); a dialog with a broken reference is dropped
void CheckDialogs(ContentDb& db, const DataStore* base, ContentReport& rep, bool haveProtos) {
    if (!haveProtos) return;
    std::vector<DialogDef> keep;
    for (DialogDef& d : db.dialogs) {
        Ctx c{rep, d.file};
        const std::string dp = "dialog '" + d.id + "'";
        auto item = [&](const std::string& key, const std::string& where) {
            auto it = db.protoSet.byKey.find(Low(key));
            if (it == db.protoSet.byKey.end()) c.Err(where, "unknown item '" + key + "'");
            else if (db.protoSet.protos[it->second].cls == PClass::Critter) c.Err(where, "'" + key + "' is a critter, not an item");
        };
        auto stf = [&](int id, const std::string& where) { if (id > 0 && base && base->String(id).empty()) c.Err(where, "unknown English.stf string id " + std::to_string(id)); };
        std::function<void(const DlgCond&, const std::string&)> cond = [&](const DlgCond& cd, const std::string& where) {
            if (cd.kind == DlgCond::Item) item(cd.name, where);
            for (const DlgCond& k : cd.kids) cond(k, where);
        };
        auto fx = [&](const std::vector<DlgEffect>& v, const std::string& where) { for (const DlgEffect& e : v) if (e.kind == DlgEffect::Give || e.kind == DlgEffect::Take) item(e.s, where); };
        if (!d.speaker.empty()) {
            auto it = db.protoSet.byKey.find(Low(d.speaker));
            if (it == db.protoSet.byKey.end()) c.Err(dp + ".speaker", "unknown prototype '" + d.speaker + "'");
            else if (db.protoSet.protos[it->second].cls != PClass::Critter) c.Err(dp + ".speaker", "'" + d.speaker + "' is not a critter");
        }
        for (const DlgRoute& r : d.start) if (r.hasCond) cond(r.cond, dp + ".start");
        for (const DlgNode& n : d.nodes) {
            const std::string np = dp + ".nodes['" + n.id + "']";
            stf(n.stf, np + ".stf");
            fx(n.onEnter, np + ".on_enter");
            for (size_t k = 0; k < n.answers.size(); ++k) {
                const std::string ap = np + ".answers[" + std::to_string(k) + "]";
                stf(n.answers[k].stf, ap + ".stf");
                fx(n.answers[k].effects, ap + ".do");
                if (n.answers[k].hasCond) cond(n.answers[k].cond, ap + ".if");
            }
        }
        if (!c.bad) keep.push_back(d);
        else db.fingerprint.erase("dialogs:" + Low(d.id));
    }
    db.dialogs = std::move(keep);
}

std::string JoinIds(const std::vector<std::string>& v, size_t maxN = 6) {
    std::string s;
    for (size_t i = 0; i < v.size() && i < maxN; ++i) s += (i ? ", " : "") + v[i];
    if (v.size() > maxN) s += ", ... (+" + std::to_string(v.size() - maxN) + ")";
    return s;
}

}  // namespace

// ------------------------------------------------------------------ stats

const std::vector<std::string>& StatNames() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> n;
        for (int i = 0; i < StNamedCount; ++i) n.push_back(kStatNamesNamed[i]);
        for (int i = 0; i < kSkillCount; ++i) n.push_back(std::string("skill.") + kSkillNames[i]);
        return n;
    }();
    return v;
}

int StatFromName(const std::string& name) {
    const std::string l = Low(name);
    for (int i = 0; i < StNamedCount; ++i) if (l == kStatNamesNamed[i]) return i;
    if (l.rfind("skill.", 0) == 0)
        for (int i = 0; i < kSkillCount; ++i) if (l.substr(6) == Low(kSkillNames[i])) return kStatSkillBase + i;
    return -1;
}

std::string StatName(int id) {
    if (id >= 0 && id < StNamedCount) return kStatNamesNamed[id];
    if (id >= kStatSkillBase && id < kStatSkillBase + kSkillCount) return std::string("skill.") + kSkillNames[id - kStatSkillBase];
    return "?";
}

// ------------------------------------------------------------------ ContentDb

ContentDb::ContentDb() {
    // built-in skill formulas (the placeholders Character.cpp used before the data layer); data/skills overrides them
    struct D { int c, s, p, e, ch, i, a, l; };
    static const D d[kSkillSlots] = {
        {5, 0, 0, 0, 0, 0, 4, 0},    // Firearms
        {20, 2, 0, 0, 0, 0, 2, 0},   // Melee
        {30, 2, 0, 0, 0, 0, 2, 0},   // Unarmed
        {0, 0, 0, 0, 4, 0, 0, 0},    // Barter
        {5, 0, 0, 0, 4, 0, 0, 0},    // Persuasion
        {5, 0, 0, 0, 3, 1, 0, 0},    // Deception
        {0, 0, 0, 0, 0, 3, 0, 0},    // Mechanics
        {0, 0, 2, 0, 0, 2, 0, 0},    // Medic
        {5, 0, 0, 2, 0, 2, 0, 0},    // Outdoorsman
        {0, 0, 0, 0, 0, 4, 0, 0},    // Science
        {10, 0, 1, 0, 0, 0, 1, 0},   // Security
        {5, 0, 0, 0, 0, 0, 3, 0},    // Sneak
        {3, 0, 0, 0, 0, 0, 3, 0},    // Steal
        {10, 0, 1, 0, 0, 0, 1, 0},   // Traps
    };
    for (int k = 0; k < kSkillSlots; ++k) {
        skills[k].constant = d[k].c;
        const int cf[kSpecialCount] = {d[k].s, d[k].p, d[k].e, d[k].ch, d[k].i, d[k].a, d[k].l};
        for (int i = 0; i < kSpecialCount; ++i) skills[k].coef[i] = cf[i];
    }
}

const TraitDef* ContentDb::FindTrait(const std::string& id) const {
    const std::string l = Low(id);
    for (const TraitDef& t : traits) if (Low(t.id) == l) return &t;
    return nullptr;
}
const TraitDef* ContentDb::TraitByIndex(int index) const {
    for (const TraitDef& t : traits) if (t.index == index) return &t;
    return nullptr;
}
const PerkDef* ContentDb::FindPerk(const std::string& id) const {
    const std::string l = Low(id);
    for (const PerkDef& p : perks) if (Low(p.id) == l) return &p;
    return nullptr;
}
const QuestDef* ContentDb::FindQuest(const std::string& id) const {
    const std::string l = Low(id);
    for (const QuestDef& q : quests) if (Low(q.id) == l) return &q;
    return nullptr;
}
const DialogDef* ContentDb::FindDialog(const std::string& id) const {
    const std::string l = Low(id);
    for (const DialogDef& d : dialogs) if (Low(d.id) == l) return &d;
    return nullptr;
}
const DlgNode* DialogDef::FindNode(const std::string& nodeId) const {
    const std::string l = Low(nodeId);
    for (const DlgNode& n : nodes) if (Low(n.id) == l) return &n;
    return nullptr;
}
const NpcRec* ContentDb::FindNpc(const std::string& protoKey) const {
    const std::string l = Low(protoKey);
    for (const NpcRec& n : npcs) if (Low(n.id) == l) return &n;
    return nullptr;
}

std::string ContentDb::Summary() const {
    return std::to_string(traits.size()) + " traits, " + std::to_string(perks.size()) + " perks, " + std::to_string(items.size()) + " item records, " +
           std::to_string(npcs.size()) + " npc records, " + std::to_string(quests.size()) + " quests, " + std::to_string(dialogs.size()) + " dialogs, " + std::to_string(files.size()) + " files";
}

std::vector<std::string> ContentDb::Diff(const ContentDb& a, const ContentDb& b) {
    std::vector<std::string> lines;
    static const char* const kinds[] = {"skills", "traits", "perks", "items", "npcs", "quests", "dialogs"};
    for (const char* k : kinds) {
        const std::string pre = std::string(k) + ":";
        std::vector<std::string> added, removed, changed;
        for (auto& [key, v] : b.fingerprint) {
            if (key.compare(0, pre.size(), pre) != 0) continue;
            auto it = a.fingerprint.find(key);
            if (it == a.fingerprint.end()) added.push_back(key.substr(pre.size()));
            else if (it->second != v) changed.push_back(key.substr(pre.size()));
        }
        for (auto& [key, v] : a.fingerprint)
            if (key.compare(0, pre.size(), pre) == 0 && !b.fingerprint.count(key)) removed.push_back(key.substr(pre.size()));
        if (added.empty() && removed.empty() && changed.empty()) continue;
        std::string l = std::string(k) + ": +" + std::to_string(added.size()) + " -" + std::to_string(removed.size()) + " ~" + std::to_string(changed.size());
        std::string det;
        if (!added.empty()) det += "added: " + JoinIds(added);
        if (!removed.empty()) det += std::string(det.empty() ? "" : "; ") + "removed: " + JoinIds(removed);
        if (!changed.empty()) det += std::string(det.empty() ? "" : "; ") + "changed: " + JoinIds(changed);
        lines.push_back(l + " (" + det + ")");
    }
    if (a.traitMax != b.traitMax || a.hasTraitMax != b.hasTraitMax) lines.push_back("traits: max_picks " + (b.hasTraitMax ? std::to_string(b.traitMax) : std::string("(config.lua TraitMax)")));
    return lines;
}

bool ContentDb::Build(const fs::path& dir, const DataStore* base, bool lenient, ContentDb& out, ContentReport& rep) {
    out = ContentDb{};
    out.dir = dir;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) {
        rep.Warn(dir.generic_string(), "$", "no data folder: built-in defaults only");
        return true;
    }
    Loader L(dir, rep, out);
    {   // manifest.json: schema version of the folder
        const std::string mf = L.root + "/manifest.json";
        std::ifstream f(dir / "manifest.json", std::ios::binary);
        if (!f) rep.Error(mf, "$", "missing (every data folder needs a manifest.json with \"schema\": " + std::to_string(kContentSchema) + ")");
        else {
            json m;
            try { m = json::parse(f, nullptr, true, true); } catch (const std::exception& e) { rep.Error(mf, "$", std::string("invalid JSON: ") + e.what()); m = json(); }
            if (m.is_object()) {
                if (!m.contains("schema") || !m["schema"].is_number_integer()) rep.Error(mf, "schema", "missing required integer field");
                else if (m["schema"].get<int>() > kContentSchema) rep.Error(mf, "schema", "schema " + std::to_string(m["schema"].get<int>()) + " is newer than this server understands (max " + std::to_string(kContentSchema) + ")");
                else if (m["schema"].get<int>() < 1) rep.Error(mf, "schema", "invalid schema version");
                else out.schema = m["schema"].get<int>();
                for (auto it = m.begin(); it != m.end(); ++it)
                    if (it.key() != "schema" && it.key() != "name" && it.key() != "description") rep.Error(mf, it.key(), "unknown field");
            } else if (!m.is_null()) rep.Error(mf, "$", "expected an object");
        }
        for (auto& e : fs::directory_iterator(dir, ec)) {
            const std::string n = e.path().filename().string();
            if (e.is_directory() && n != "skills" && n != "traits" && n != "perks" && n != "items" && n != "npcs" && n != "quests" && n != "dialogs" && n != "locale")
                rep.Warn(L.root + "/" + n, "$", "unknown folder ignored");
        }
    }
    L.LoadSkills();
    L.LoadTraits();
    L.LoadPerks();
    L.LoadItems();
    L.LoadNpcs();
    L.LoadQuests();
    L.LoadDialogs();
    const bool haveBase = base && base->Loaded();
    if (haveBase) BuildProtoSet(*base, out, rep);
    else if (!out.items.empty() || !out.npcs.empty() || std::any_of(out.quests.begin(), out.quests.end(), [](const QuestDef& q) { return !q.items.empty(); }))
        rep.Warn(L.root, "$", "no gamedata loaded: item / npc records and quest reward items are not validated or applied");
    CheckQuestItems(out, rep, haveBase);
    CheckDialogs(out, base, rep, haveBase);
    std::sort(out.files.begin(), out.files.end());
    out.loaded = true;
    if (!rep.errors.empty() && lenient) {
        for (std::string& e : rep.errors) rep.warnings.push_back("(lenient, record dropped or ignored) " + e);
        rep.errors.clear();
    }
    return rep.errors.empty();
}

// ------------------------------------------------------------------ tools

fs::path FindContentDir(const fs::path& exePath) {
    std::error_code ec;
    fs::path exeDir = fs::absolute(exePath, ec).parent_path();
    for (fs::path d : {fs::current_path(ec), exeDir}) {
        for (int i = 0; i < 6 && !d.empty(); ++i, d = d.parent_path()) {
            if (fs::exists(d / "data" / "manifest.json", ec)) return d / "data";
            if (d == d.root_path()) break;
        }
    }
    return {};
}

int RunValidateData(const fs::path& gamedataDir, const fs::path& contentDir, bool lenient) {
    DataStore ds;
    bool haveBase = false;
    if (!gamedataDir.empty()) {
        std::string err;
        if (!ds.Load(gamedataDir, err)) { std::cerr << "validate-data: cannot load gamedata: " << err << "\n"; return 2; }
        haveBase = true;
    } else std::cout << "validate-data: no gamedata (structure and cross references between data files only)\n";
    ContentDb db;
    ContentReport rep;
    const bool ok = ContentDb::Build(contentDir, haveBase ? &ds : nullptr, lenient, db, rep);
    for (const std::string& w : rep.warnings) std::cout << "warning: " << w << "\n";
    for (const std::string& e : rep.errors) std::cout << "error: " << e << "\n";
    std::cout << (ok ? "validate-data: OK  " : "validate-data: FAILED  ") << (db.loaded ? db.Summary() : std::string("(no data folder)"))
              << " [" << rep.errors.size() << " error(s), " << rep.warnings.size() << " warning(s)]\n";
    return ok ? 0 : 1;
}

}  // namespace f3
