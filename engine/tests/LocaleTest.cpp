// Unit-style tests of the localization layer: locale loader, fallback chain, placeholder formatting, the --check-locale coverage report,
// UTF-8 helpers (counting, truncation, validation, Cyrillic case folding, name / chat validation) and the UTF-8 safe word wrap of the
// client. No server, no network, no game data: a tiny synthetic data/locale folder is written to a temp directory.
//   f3_localetest            exit code 0 = all passed
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include "TextUtil.h"
#include "common/Localization.h"
#include "common/utf8.h"

using namespace f3;
namespace fs = std::filesystem;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void Put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}

static void TestLoader(const fs::path& root) {
    Put(root / "en" / "ui.json", R"({"menu.start":"Start","greet":"Hello, {name}! You have {n} caps.","only_en":"English only","brace":"Use {{n}} for {n}","far":"You are too far away"})");
    Put(root / "ru" / "ui.json", R"({"menu.start":"Начать","greet":"Привет, {name}! У вас {n} крышек.","brace":"Пишите {{n}} вместо {n}","far":"Вы слишком далеко"})");
    Put(root / "ru" / "more.json", R"({"extra":"Дополнительно"})");
    loc::Locale l;
    std::string err;
    CHECK(l.Load(root, &err), "load: %s", err.c_str());
    CHECK(l.Loaded() && l.Has("en") && l.Has("ru") && !l.Has("de"), "languages");
    CHECK(l.Languages().size() == 2, "language count %zu", l.Languages().size());
    // fallback chain: language -> en -> key
    CHECK(l.Tr("ru", "menu.start") == "Начать", "ru text");
    CHECK(l.Tr("en", "menu.start") == "Start", "en text");
    CHECK(l.Tr("ru", "only_en") == "English only", "missing in ru falls back to en");
    CHECK(l.Tr("de", "menu.start") == "Start", "unknown language falls back to en");
    CHECK(l.Tr("ru", "no.such.key") == "no.such.key", "missing everywhere gives the key");
    CHECK(l.TrOr("ru", "no.such.key", "base text") == "base text", "TrOr fallback");
    CHECK(l.TrOr("ru", "extra", "x") == "Дополнительно", "second file of a language is merged");
    CHECK(l.TrOr("en", "extra", "x") == "x", "key only in ru is not visible in en");
    // placeholders
    CHECK(l.Format("en", "greet", {{"name", "Ann"}, {"n", 12}}) == "Hello, Ann! You have 12 caps.", "format en");
    CHECK(l.Format("ru", "greet", {{"name", "Аня"}, {"n", 5}}) == "Привет, Аня! У вас 5 крышек.", "format ru");
    CHECK(l.Format("en", "greet", {{"name", "Ann"}}) == "Hello, Ann! You have {n} caps.", "unknown placeholder stays");
    CHECK(l.Format("en", "brace", {{"n", 3}}) == "Use {n} for 3", "escaped braces: %s", l.Format("en", "brace", {{"n", 3}}).c_str());
    CHECK(loc::Locale::Subst("{a}{b}{{c}}", {{"a", "1"}, {"b", "2"}}) == "12{c}", "Subst");
    const auto ph = loc::Locale::Placeholders("x {a} {{b}} {c}");
    CHECK(ph.size() == 2 && ph.count("a") && ph.count("c"), "Placeholders ignores escaped braces");
    // language codes
    CHECK(loc::Locale::Normalize("ru-RU") == "ru" && loc::Locale::Normalize("RU") == "ru" && loc::Locale::Normalize("en_US") == "en", "Normalize");
    CHECK(loc::Locale::Normalize("") == "en" && loc::Locale::Normalize("x") == "en" && loc::Locale::Normalize("12345") == "en", "Normalize garbage");
    // global instance used by the client
    loc::Global().Load(root);
    loc::GlobalLang() = "ru";
    CHECK(loc::T("far") == "Вы слишком далеко", "global T ru");
    CHECK(loc::T("greet", {{"name", "Ян"}, {"n", 1}}) == "Привет, Ян! У вас 1 крышек.", "global T with args");
    loc::GlobalLang() = "en";
    CHECK(loc::T("far") == "You are too far away", "global T en");
    CHECK(loc::TOr("nothing", "fb") == "fb", "global TOr");
    // a broken file is reported, the others still load
    Put(root / "ru" / "broken.json", "{ not json");
    loc::Locale l2;
    std::string err2;
    l2.Load(root, &err2);
    CHECK(!err2.empty() && err2.find("broken.json") != std::string::npos, "broken file named in the error: '%s'", err2.c_str());
    CHECK(l2.Tr("ru", "menu.start") == "Начать", "good files load despite a broken one");
    fs::remove(root / "ru" / "broken.json");
}

static void TestCheck(const fs::path& root) {
    // ru lacks "only_en" (missing), has a bad placeholder set in "greet2" and a stale key
    Put(root / "en" / "ui.json", R"({"a":"A","greet2":"Hi {name}","only_en":"x","unused_key":"never used"})");
    Put(root / "ru" / "ui.json", R"({"a":"А","greet2":"Привет {who}","stale":"Старое"})");
    fs::remove(root / "ru" / "more.json");
    const fs::path src = root / "src";
    Put(src / "code.cpp", "const char* k = \"a\"; const char* u = \"ui.missing_in_en\"; auto g = \"greet2\"; auto o = \"only_en\";\n");
    loc::Locale l;
    l.Load(root);
    loc::CheckOptions o;
    o.srcDir = src;
    loc::CheckResult r = loc::Check(l, o);
    CHECK(r.text.find("only_en") != std::string::npos, "missing key listed");
    CHECK(r.text.find("greet2") != std::string::npos && r.text.find("placeholders differ") != std::string::npos, "placeholder mismatch is an error");
    CHECK(r.text.find("stale") != std::string::npos, "extra key listed");
    CHECK(r.text.find("unused_key") != std::string::npos, "unused key listed");
    CHECK(r.errors >= 1, "errors counted (%d)", r.errors);
    o.strict = true;
    loc::CheckResult rs = loc::Check(l, o);
    CHECK(rs.errors > r.errors, "strict turns missing translations into errors (%d vs %d)", rs.errors, r.errors);
    // data keys: English base lives in data/*.json
    o.strict = false;
    o.dataKeys.insert("perk.x.name");
    loc::CheckResult rd = loc::Check(l, o);
    CHECK(rd.text.find("perk.x.name") != std::string::npos, "data key missing in ru is reported");
}

static void TestUtf8() {
    const std::string ru = "Привет";                       // 6 code points, 12 bytes
    CHECK(utf8::Count(ru) == 6 && ru.size() == 12, "Count");
    CHECK(utf8::Truncate(ru, 3) == "При" && utf8::Truncate(ru, 99) == ru && utf8::Truncate("", 3).empty(), "Truncate on code points");
    std::string s = ru;
    utf8::PopBack(s);
    CHECK(s == "Приве", "PopBack removes one whole code point");
    CHECK(utf8::Valid(ru) && utf8::Valid("abc") && !utf8::Valid("\xD0") && !utf8::Valid("\xC0\xAF") && !utf8::Valid("\xED\xA0\x80"), "Valid (truncated, overlong, surrogate)");
    CHECK(utf8::Lower("ПРИВЕТ Ёж ABC") == "привет ёж abc", "Lower folds Latin and Cyrillic: %s", utf8::Lower("ПРИВЕТ Ёж ABC").c_str());
    // names: 2-16 code points, Latin / Cyrillic letters, digits, _
    CHECK(utf8::ValidName("Сталкер") && utf8::ValidName("Anna_7") && utf8::ValidName("Ёжик"), "valid names");
    CHECK(!utf8::ValidName("A") && !utf8::ValidName("") && !utf8::ValidName("ПриветПриветПриветП"), "length in code points (2..16)");
    CHECK(utf8::ValidName("ПриветПриветПрив"), "16 Cyrillic letters = 32 bytes are fine") ;
    CHECK(!utf8::ValidName("Bad Name") && !utf8::ValidName("a\x01" "b") && !utf8::ValidName("x\xD0") && !utf8::ValidName("a-b"), "forbidden characters");
    // chat text: control characters dropped, trimmed, limited in code points
    CHECK(utf8::SanitizeText("  привет\x01 мир \x7f ", 200) == "привет мир", "SanitizeText: '%s'", utf8::SanitizeText("  привет\x01 мир \x7f ", 200).c_str());
    CHECK(utf8::SanitizeText("абвгд", 3) == "абв", "SanitizeText limit counts code points");
    CHECK(utf8::SanitizeText("ab\xD0 cd", 10) == "ab cd", "invalid sequence dropped");
    CHECK(utf8::SanitizeText("\x01\x02", 10).empty(), "only control characters");
}

// UTF-8 wrap with a fake fixed-width font: 10 px per code point
static void TestWrap() {
    auto w = [](const std::string& t) { return static_cast<float>(utf8::Count(t) * 10); };
    auto lines = txt::Wrap("Привет мир это длинная строка для проверки", 120, w);
    CHECK(lines.size() >= 3, "wrapped into %zu lines", lines.size());
    bool ok = true;
    for (const std::string& l : lines) { if (!utf8::Valid(l) || w(l) > 120) ok = false; }
    CHECK(ok, "every line is valid UTF-8 and fits");
    std::string joined;
    for (const std::string& l : lines) joined += l;
    CHECK(joined == "Привет мир это длинная строка для проверки", "no text lost: '%s'", joined.c_str());
    // an overlong word is cut between code points, never inside one
    lines = txt::Wrap("Длинносоставноеслово", 50, w);
    ok = lines.size() >= 4;
    for (const std::string& l : lines) if (!utf8::Valid(l) || w(l) > 50) ok = false;
    CHECK(ok, "hard split of a long word (%zu lines)", lines.size());
    CHECK(txt::Wrap("a\n\nb", 100, w).size() == 3, "empty paragraph kept");
    CHECK(txt::Wrap("a\n\nb", 100, w, false).size() == 2, "empty paragraph dropped");
    CHECK(txt::Wrap("", 100, w).size() == 1, "empty text gives one empty line");
    // 4-byte code points (emoji) survive too
    lines = txt::Wrap("ab \xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80 cd", 30, w);
    ok = true;
    for (const std::string& l : lines) if (!utf8::Valid(l)) ok = false;
    CHECK(ok, "4-byte code points are never split");
    const std::string e = txt::Ellipsize("Очень длинное название предмета", 100, w);
    CHECK(utf8::Valid(e) && w(e) <= 100 && e.size() > 3 && e.substr(e.size() - 3) == "...", "Ellipsize: '%s'", e.c_str());
    CHECK(txt::Ellipsize("коротко", 100, w) == "коротко", "short text unchanged");
    std::string pre, post;
    txt::SplitNum("ОЖ {n} / 100", pre, post);
    CHECK(pre == "ОЖ " && post == " / 100", "SplitNum");
}

int main() {
    const fs::path root = fs::temp_directory_path() / ("f3_localetest_" + std::to_string(static_cast<unsigned long long>(std::hash<std::string>{}(__FILE__)) % 100000));
    fs::remove_all(root);
    TestLoader(root);
    TestCheck(root);
    TestUtf8();
    TestWrap();
    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf("f3_localetest: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
