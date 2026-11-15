#include "Account.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include "common/log.h"

namespace f3 {
namespace crypto {

namespace {
constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    size_t fill = 0;
    uint64_t total = 0;
    void Block(const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25), ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void Update(const uint8_t* p, size_t n) {
        total += n;
        while (n) {
            size_t k = std::min(n, 64 - fill);
            std::memcpy(buf + fill, p, k);
            fill += k; p += k; n -= k;
            if (fill == 64) { Block(buf); fill = 0; }
        }
    }
    std::vector<uint8_t> Final() {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        Update(&pad, 1);
        uint8_t z = 0;
        while (fill != 56) Update(&z, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
        Update(len, 8);
        std::vector<uint8_t> out(32);
        for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j));
        return out;
    }
};
}  // namespace

std::vector<uint8_t> Sha256(const uint8_t* data, size_t len) {
    Sha s;
    s.Update(data, len);
    return s.Final();
}

std::vector<uint8_t> HmacSha256(const std::vector<uint8_t>& key, const uint8_t* data, size_t len) {
    std::vector<uint8_t> k = key;
    if (k.size() > 64) k = Sha256(k.data(), k.size());
    k.resize(64, 0);
    uint8_t ip[64], op[64];
    for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    Sha in;
    in.Update(ip, 64); in.Update(data, len);
    auto ih = in.Final();
    Sha out;
    out.Update(op, 64); out.Update(ih.data(), ih.size());
    return out.Final();
}

std::vector<uint8_t> Pbkdf2Sha256(const std::string& password, const std::vector<uint8_t>& salt, int iterations, size_t outLen) {
    std::vector<uint8_t> key(password.begin(), password.end()), out;
    for (uint32_t block = 1; out.size() < outLen; block++) {
        std::vector<uint8_t> s = salt;
        s.push_back((uint8_t)(block >> 24)); s.push_back((uint8_t)(block >> 16)); s.push_back((uint8_t)(block >> 8)); s.push_back((uint8_t)block);
        auto u = HmacSha256(key, s.data(), s.size());
        auto t = u;
        for (int i = 1; i < iterations; i++) {
            u = HmacSha256(key, u.data(), u.size());
            for (size_t j = 0; j < t.size(); j++) t[j] ^= u[j];
        }
        out.insert(out.end(), t.begin(), t.end());
    }
    out.resize(outLen);
    return out;
}

std::string Hex(const std::vector<uint8_t>& v) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : v) { s += d[b >> 4]; s += d[b & 15]; }
    return s;
}

std::vector<uint8_t> FromHex(const std::string& s) {
    std::vector<uint8_t> out;
    auto val = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        int a = val(s[i]), b = val(s[i + 1]);
        if (a < 0 || b < 0) return {};
        out.push_back((uint8_t)(a * 16 + b));
    }
    return out;
}

std::vector<uint8_t> RandomBytes(size_t n) {
    std::random_device rd;   // MSVC: rand_s (CSPRNG)
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = (uint8_t)(rd() & 0xff);
    return v;
}

bool ConstantTimeEq(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size()) return false;
    uint8_t d = 0;
    for (size_t i = 0; i < a.size(); i++) d |= a[i] ^ b[i];
    return d == 0;
}
}  // namespace crypto

bool WriteFileAtomic(const std::filesystem::path& path, const std::string& text) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(text.data(), (std::streamsize)text.size());
        f.flush();
        if (!f) return false;
    }
    if (fs::exists(path, ec)) {
        fs::path bak = path;
        bak += ".bak";
        fs::copy_file(path, bak, fs::copy_options::overwrite_existing, ec);
    }
    fs::rename(tmp, path, ec);   // replaces an existing file on Windows and POSIX
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

// ------------------------------------------------------------------------------------------------ AccountStore

static int64_t NowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void AccountStore::Init(const std::filesystem::path& savesDir, int iterations) {
    std::error_code ec;
    dir_ = savesDir / "accounts";
    std::filesystem::create_directories(dir_, ec);
    if (ec) { log::Error("cannot create %s", dir_.string().c_str()); dir_.clear(); }
    iter_ = std::max(1000, iterations);
}

bool AccountStore::ValidName(const std::string& n) {
    if (n.size() < 2 || n.size() > 16) return false;
    for (char c : n) if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
    return true;
}

std::string AccountStore::Key(const std::string& name) {
    std::string k = name;
    for (char& c : k) c = (char)std::tolower((unsigned char)c);
    return k;
}

std::filesystem::path AccountStore::PathFor(const std::string& name) const { return dir_ / ("acc_" + Key(name) + ".json"); }
bool AccountStore::Exists(const std::string& name) const { std::error_code ec; return ValidName(name) && std::filesystem::exists(PathFor(name), ec); }

bool AccountStore::Limited(const std::string& key, int64_t now) {
    auto it = fails_.find(key);
    return it != fails_.end() && now < it->second.lockedUntil;
}

void AccountStore::NoteFail(const std::string& key, int64_t now) {
    Fail& f = fails_[key];
    f.times.push_back(now);
    f.times.erase(std::remove_if(f.times.begin(), f.times.end(), [&](int64_t t) { return now - t > windowSec_; }), f.times.end());
    if ((int)f.times.size() >= maxFails_) { f.lockedUntil = now + lockSec_; f.times.clear(); }
}

AccountStore::Result AccountStore::Register(const std::string& name, const std::string& password, const std::string& ip, nlohmann::json first) {
    if (!Ready()) return Result::Error;
    if (!ValidName(name)) return Result::InvalidName;
    if (password.size() < 4 || password.size() > 64) return Result::WeakPassword;
    if (Limited("ip:" + ip, NowSec())) return Result::RateLimited;
    std::error_code ec;
    if (std::filesystem::exists(PathFor(name), ec)) return Result::NameTaken;
    auto salt = crypto::RandomBytes(16);
    nlohmann::json j;
    j["name"] = name;
    j["algo"] = "pbkdf2-sha256";
    j["iterations"] = iter_;
    j["salt"] = crypto::Hex(salt);
    j["hash"] = crypto::Hex(crypto::Pbkdf2Sha256(password, salt, iter_));
    j["characters"] = nlohmann::json::array({std::move(first)});
    return WriteFileAtomic(PathFor(name), j.dump(1)) ? Result::Ok : Result::Error;
}

AccountStore::Result AccountStore::Login(const std::string& name, const std::string& password, const std::string& ip, nlohmann::json* out) {
    if (!Ready()) return Result::Error;
    const int64_t now = NowSec();
    const std::string kn = "acc:" + Key(name), ki = "ip:" + ip;
    if (Limited(kn, now) || Limited(ki, now)) return Result::RateLimited;
    if (!ValidName(name) || password.size() > 128) { NoteFail(ki, now); return Result::BadCredentials; }
    nlohmann::json j;
    {
        std::ifstream f(PathFor(name), std::ios::binary);
        if (f) j = nlohmann::json::parse(f, nullptr, false);
    }
    // unknown accounts burn the same hashing time so the response does not reveal whether the name exists
    bool known = j.is_object() && j.contains("salt") && j.contains("hash") && j.contains("iterations");
    std::vector<uint8_t> salt = known ? crypto::FromHex(j["salt"].get<std::string>()) : crypto::RandomBytes(16);
    int it = known ? std::clamp(j["iterations"].get<int>(), 1000, 5000000) : iter_;
    auto got = crypto::Pbkdf2Sha256(password, salt, it);
    if (!known || !crypto::ConstantTimeEq(got, crypto::FromHex(j["hash"].get<std::string>()))) {
        NoteFail(ki, now);
        NoteFail(kn, now);
        return Result::BadCredentials;
    }
    fails_.erase(kn);
    if (out) {
        *out = nlohmann::json::object();
        (*out)["account_name"] = j.value("name", name);
        if (j.contains("characters") && j["characters"].is_array() && !j["characters"].empty()) *out = j["characters"][0];
    }
    return Result::Ok;
}

bool AccountStore::SaveCharacter(const std::string& name, size_t index, const nlohmann::json& ch) {
    if (!Ready() || !ValidName(name)) return false;
    nlohmann::json j;
    {
        std::ifstream f(PathFor(name), std::ios::binary);
        if (!f) return false;
        j = nlohmann::json::parse(f, nullptr, false);
    }
    if (!j.is_object()) return false;
    if (!j.contains("characters") || !j["characters"].is_array()) j["characters"] = nlohmann::json::array();
    while (j["characters"].size() <= index) j["characters"].push_back(nlohmann::json::object());
    j["characters"][index] = ch;
    return WriteFileAtomic(PathFor(name), j.dump(1));
}

}  // namespace f3
