#include "Localization.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <nlohmann/json.hpp>
#include "utf8.h"

namespace fs = std::filesystem;

namespace f3::loc {

bool Locale::Load(const fs::path& root, std::string* err) {
    tables_.clear();
    std::error_code ec;
    if (!fs::is_directory(root, ec)) { if (err) *err = "locale directory not found: " + root.string(); return false; }
    std::string problems;
    for (const auto& d : fs::directory_iterator(root, ec)) {
        if (!d.is_directory()) continue;
        const std::string lang = d.path().filename().string();
        std::vector<fs::path> files;
        for (const auto& f : fs::directory_iterator(d.path(), ec))
            if (f.is_regular_file() && f.path().extension() == ".json") files.push_back(f.path());
        std::sort(files.begin(), files.end());
        auto& table = tables_[lang];
        for (const auto& f : files) {
            std::ifstream in(f, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            std::string text = ss.str();
            if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) text.erase(0, 3);
            nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
            if (!j.is_object()) { problems += f.string() + ": not a JSON object\n"; continue; }
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (!it.value().is_string()) { problems += f.string() + ": '" + it.key() + "' is not a string\n"; continue; }
                table[it.key()] = it.value().get<std::string>();
            }
        }
        if (table.empty()) tables_.erase(lang);
    }
    if (err) *err = problems;
    if (tables_.empty()) { if (err && err->empty()) *err = "no languages found in " + root.string(); return false; }
    return problems.empty();
}

std::vector<std::string> Locale::Languages() const {
    std::vector<std::string> v;
    for (const auto& [k, t] : tables_) v.push_back(k);
    return v;
}

const std::map<std::string, std::string>* Locale::Table(const std::string& lang) const {
    auto it = tables_.find(lang);
    return it == tables_.end() ? nullptr : &it->second;
}

std::string Locale::Normalize(const std::string& lang) {
    std::string s;
    for (char c : lang) {
        if (c == '-' || c == '_') break;
        s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (s.size() < 2 || s.size() > 3) return "en";
    for (char c : s) if (c < 'a' || c > 'z') return "en";
    return s;
}

const std::string* Locale::Find(const std::string& lang, const std::string& key) const {
    auto it = tables_.find(lang);
    if (it != tables_.end()) {
        auto k = it->second.find(key);
        if (k != it->second.end()) return &k->second;
    }
    if (lang != "en") {
        auto en = tables_.find("en");
        if (en != tables_.end()) {
            auto k = en->second.find(key);
            if (k != en->second.end()) return &k->second;
        }
    }
    return nullptr;
}

std::string Locale::Tr(const std::string& lang, const std::string& key) const { return TrOr(lang, key, key); }

std::string Locale::TrOr(const std::string& lang, const std::string& key, const std::string& fallback) const {
    const std::string* s = Find(lang, key);
    return s ? *s : fallback;
}

std::string Locale::Format(const std::string& lang, const std::string& key, Args args) const {
    const std::string* s = Find(lang, key);
    return s ? Subst(*s, args) : key;
}

std::string Locale::FormatList(const std::string& lang, const std::string& key, const ArgList& args) const {
    const std::string* s = Find(lang, key);
    return s ? SubstList(*s, args) : key;
}

std::string Locale::Subst(const std::string& t, Args args) { return SubstList(t, ArgList(args)); }

std::string Locale::SubstList(const std::string& t, const ArgList& args) {
    std::string out;
    out.reserve(t.size() + 16);
    for (size_t i = 0; i < t.size();) {
        const char c = t[i];
        if (c == '{' && i + 1 < t.size() && t[i + 1] == '{') { out.push_back('{'); i += 2; continue; }
        if (c == '}' && i + 1 < t.size() && t[i + 1] == '}') { out.push_back('}'); i += 2; continue; }
        if (c == '{') {
            const size_t e = t.find('}', i + 1);
            if (e != std::string::npos) {
                const std::string name = t.substr(i + 1, e - i - 1);
                bool found = false;
                for (const Arg& a : args) if (a.key == name) { out += a.value; found = true; break; }
                if (!found) out += t.substr(i, e - i + 1);
                i = e + 1;
                continue;
            }
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

std::set<std::string> Locale::Placeholders(const std::string& t) {
    std::set<std::string> s;
    for (size_t i = 0; i < t.size();) {
        if (t[i] == '{' && i + 1 < t.size() && t[i + 1] == '{') { i += 2; continue; }
        if (t[i] == '}' && i + 1 < t.size() && t[i + 1] == '}') { i += 2; continue; }
        if (t[i] == '{') {
            const size_t e = t.find('}', i + 1);
            if (e != std::string::npos) { s.insert(t.substr(i + 1, e - i - 1)); i = e + 1; continue; }
        }
        ++i;
    }
    return s;
}

fs::path Locale::FindDir(const fs::path& exePath) {
    std::error_code ec;
    fs::path exeDir = fs::absolute(exePath, ec).parent_path();
    for (fs::path d : {fs::current_path(ec), exeDir}) {
        for (int i = 0; i < 7 && !d.empty(); ++i, d = d.parent_path()) {
            if (fs::exists(d / "data" / "locale" / "en", ec)) return d / "data" / "locale";
            if (d == d.root_path()) break;
        }
    }
    return {};
}

Locale& Global() { static Locale l; return l; }
std::string& GlobalLang() { static std::string s = "en"; return s; }

// ------------------------------------------------------------------------------------------------------------ check

namespace {
void ScanSources(const fs::path& dir, std::set<std::string>& literals, std::vector<std::string>& prefixes) {
    static const std::regex lit("\"([a-z][a-z0-9_]*(?:\\.[A-Za-z0-9_]*)+)\"");
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (const auto& e : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        if (!e.is_regular_file()) continue;
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".h" && ext != ".lua") continue;
        const std::string p = e.path().generic_string();
        if (p.find("/build/") != std::string::npos || p.find("/tests/") != std::string::npos) continue;
        std::ifstream in(e.path(), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        for (auto it = std::sregex_iterator(text.begin(), text.end(), lit); it != std::sregex_iterator(); ++it) {
            const std::string s = (*it)[1].str();
            if (s.back() == '.') prefixes.push_back(s); else literals.insert(s);
        }
    }
}
}  // namespace

CheckResult Check(const Locale& l, const CheckOptions& o) {
    CheckResult r;
    std::ostringstream out;
    const auto* en = l.Table("en");
    if (!en) {
        out << "ERROR: no English table (data/locale/en/*.json)\n";
        r.errors = 1;
        r.text = out.str();
        return r;
    }
    // English keys including the ones whose base text is in data/
    std::set<std::string> baseKeys;
    for (const auto& [k, v] : *en) baseKeys.insert(k);
    for (const auto& k : o.dataKeys) baseKeys.insert(k);
    std::set<std::string> namespaces;
    for (const auto& k : baseKeys) namespaces.insert(k.substr(0, k.find('.')));
    // 1. languages vs en
    for (const std::string& lang : l.Languages()) {
        if (lang == "en") continue;
        const auto* t = l.Table(lang);
        std::vector<std::string> missing, extra, badPh;
        size_t stfOverrides = 0;
        for (const auto& k : baseKeys) if (!t->count(k)) missing.push_back(k);
        for (const auto& [k, v] : *t) {
            if (k.compare(0, 4, "stf.") == 0) { ++stfOverrides; continue; }   // overrides of English.stf texts (the base is not in the tables)
            if (!baseKeys.count(k)) { extra.push_back(k); continue; }
            auto e = en->find(k);
            if (e != en->end() && Locale::Placeholders(e->second) != Locale::Placeholders(v)) badPh.push_back(k);
            if (!utf8::Valid(v)) badPh.push_back(k + " (invalid UTF-8)");
        }
        const size_t total = baseKeys.size();
        out << "language '" << lang << "': " << (total - missing.size()) << "/" << total << " keys translated";
        if (total) out << " (" << (100 * (total - missing.size()) / total) << "%)";
        if (stfOverrides) out << ", " << stfOverrides << " English.stf override(s)";
        out << "\n";
        if (!missing.empty()) {
            out << "  missing (" << missing.size() << ", English shown instead):\n";
            for (const auto& k : missing) out << "    " << k << "\n";
            if (o.strict) r.errors += static_cast<int>(missing.size()); else r.warnings += static_cast<int>(missing.size());
        }
        if (!extra.empty()) {
            out << "  not in en / data (" << extra.size() << ", stale?):\n";
            for (const auto& k : extra) out << "    " << k << "\n";
            r.warnings += static_cast<int>(extra.size());
        }
        if (!badPh.empty()) {
            out << "  ERROR placeholders differ from en / bad text (" << badPh.size() << "):\n";
            for (const auto& k : badPh) out << "    " << k << "\n";
            r.errors += static_cast<int>(badPh.size());
        }
    }
    // 2. source scan: keys used in code but not defined, defined but never used
    if (!o.srcDir.empty()) {
        std::set<std::string> lits;
        std::vector<std::string> prefixes;
        ScanSources(o.srcDir, lits, prefixes);
        for (const auto& d : o.extraSrcDirs) ScanSources(d, lits, prefixes);
        auto dynamic = [&](const std::string& k) { for (const auto& p : prefixes) if (k.compare(0, p.size(), p) == 0) return true; return false; };
        std::vector<std::string> undefined, unused;
        for (const auto& s : lits)
            if (namespaces.count(s.substr(0, s.find('.'))) && !baseKeys.count(s) && !dynamic(s)) undefined.push_back(s);
        for (const auto& [k, v] : *en)
            if (!lits.count(k) && !dynamic(k)) unused.push_back(k);
        out << "usage in " << o.srcDir.generic_string() << ": " << lits.size() << " key-like literals, " << prefixes.size() << " dynamic prefixes\n";
        if (!undefined.empty()) {
            out << "  ERROR used in code but not defined in en (" << undefined.size() << "):\n";
            for (const auto& k : undefined) out << "    " << k << "\n";
            r.errors += static_cast<int>(undefined.size());
        }
        if (!unused.empty()) {
            out << "  defined in en but not used by any source literal (" << unused.size() << "):\n";
            for (const auto& k : unused) out << "    " << k << "\n";
            r.warnings += static_cast<int>(unused.size());
        }
    }
    out << "locale check: " << r.errors << " error(s), " << r.warnings << " warning(s)\n";
    r.text = out.str();
    return r;
}

}  // namespace f3::loc
