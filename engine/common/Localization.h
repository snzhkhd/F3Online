#pragma once
// Localization: string tables per language (data/locale/<lang>/*.json, flat {"key": "text with {placeholders}"}), a fallback chain
// selected language -> en -> key, {name} placeholders ({{ and }} are literal braces), and the coverage check behind
// `f3_server --check-locale`. Used by the server (per-player language) and the client (one global language). See docs/localization.md.
#include <filesystem>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace f3::loc {

// One placeholder value; numbers are formatted as plain integers.
struct Arg {
    std::string key, value;
    Arg(const char* k, std::string v) : key(k), value(std::move(v)) {}
    Arg(const char* k, const char* v) : key(k), value(v ? v : "") {}
    Arg(const char* k, int v) : key(k), value(std::to_string(v)) {}
    Arg(const char* k, unsigned v) : key(k), value(std::to_string(v)) {}
    Arg(const char* k, long long v) : key(k), value(std::to_string(v)) {}
    Arg(const char* k, size_t v) : key(k), value(std::to_string(v)) {}
};
using Args = std::initializer_list<Arg>;
using ArgList = std::vector<Arg>;   // same, built at run time (Lua binding)

class Locale {
public:
    // Loads root/<lang>/*.json for every language directory (files merged in name order). Returns false when nothing was found or a
    // file is not a flat string object (the message names the file); languages loaded so far stay.
    bool Load(const std::filesystem::path& root, std::string* err = nullptr);
    bool Loaded() const { return !tables_.empty(); }
    std::vector<std::string> Languages() const;
    bool Has(const std::string& lang) const { return tables_.count(lang) != 0; }
    const std::map<std::string, std::string>* Table(const std::string& lang) const;

    // "ru-RU" / "RU" -> "ru"; anything that is not 2-3 letters -> "en". Unknown but well formed codes are kept (they fall back to en).
    static std::string Normalize(const std::string& lang);
    // Fallback chain lookup: lang -> en. nullptr when neither has the key.
    const std::string* Find(const std::string& lang, const std::string& key) const;
    std::string Tr(const std::string& lang, const std::string& key) const;                                   // ... -> the key itself
    std::string TrOr(const std::string& lang, const std::string& key, const std::string& fallback) const;    // ... -> fallback
    std::string Format(const std::string& lang, const std::string& key, Args args = {}) const;
    std::string FormatList(const std::string& lang, const std::string& key, const ArgList& args) const;
    // Replaces {name} in `tmpl`; unknown placeholders stay as they are, "{{" and "}}" give literal braces.
    static std::string Subst(const std::string& tmpl, Args args);
    static std::string SubstList(const std::string& tmpl, const ArgList& args);
    static std::set<std::string> Placeholders(const std::string& tmpl);   // names used in {name}

    // Looks for data/locale (containing en/) in the cwd and the directory of `exePath` and their parents; empty if none.
    static std::filesystem::path FindDir(const std::filesystem::path& exePath);

private:
    std::map<std::string, std::map<std::string, std::string>> tables_;
};

// Process wide instance and language for programs with a single UI language (the client).
Locale& Global();
std::string& GlobalLang();
inline std::string T(const std::string& key) { return Global().Tr(GlobalLang(), key); }
inline std::string T(const std::string& key, Args args) { return Global().Format(GlobalLang(), key, args); }
inline std::string TOr(const std::string& key, const std::string& fallback) { return Global().TrOr(GlobalLang(), key, fallback); }

// ---- coverage check (f3_server --check-locale)
struct CheckOptions {
    std::set<std::string> dataKeys;   // keys whose English base text lives in data/*.json (perk.<id>.name, trait.<id>.desc, ...)
    std::filesystem::path srcDir;     // engine sources (and optionally scripts) scanned for key literals; empty = skip the usage check
    std::vector<std::filesystem::path> extraSrcDirs;
    bool strict = false;              // missing translations count as errors
};
struct CheckResult {
    int errors = 0, warnings = 0;
    std::string text;                 // the report
};
CheckResult Check(const Locale& l, const CheckOptions& o);

}  // namespace f3::loc
