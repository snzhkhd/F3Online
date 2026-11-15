#pragma once
// Accounts: salted PBKDF2-HMAC-SHA256 password hashes + one JSON file per account under <saves>/accounts, atomic writes.
// Character data lives in the same file ("characters": [...]) so an account can own several characters later; only index 0 is used today.
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace f3 {

namespace crypto {
std::vector<uint8_t> Sha256(const uint8_t* data, size_t len);
std::vector<uint8_t> HmacSha256(const std::vector<uint8_t>& key, const uint8_t* data, size_t len);
std::vector<uint8_t> Pbkdf2Sha256(const std::string& password, const std::vector<uint8_t>& salt, int iterations, size_t outLen = 32);
std::string Hex(const std::vector<uint8_t>& v);
std::vector<uint8_t> FromHex(const std::string& s);
std::vector<uint8_t> RandomBytes(size_t n);
bool ConstantTimeEq(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);
}  // namespace crypto

// Write `text` to `path` atomically (temp file in the same directory + rename). Keeps a ".bak" of the previous version.
bool WriteFileAtomic(const std::filesystem::path& path, const std::string& text);

class AccountStore {
public:
    enum class Result { Ok, BadCredentials, NameTaken, InvalidName, WeakPassword, RateLimited, Error };

    void Init(const std::filesystem::path& savesDir, int pbkdfIterations = 10000);
    bool Ready() const { return !dir_.empty(); }
    static bool ValidName(const std::string& n);
    static std::string Key(const std::string& name);          // lower case
    Result Register(const std::string& name, const std::string& password, const std::string& ip, nlohmann::json firstCharacter);
    Result Login(const std::string& name, const std::string& password, const std::string& ip, nlohmann::json* characterOut);
    bool SaveCharacter(const std::string& name, size_t index, const nlohmann::json& ch);
    bool Exists(const std::string& name) const;
    // failed-login rate limit (per account key and per ip): maxFails within windowSec locks for lockSec
    void ConfigureLimit(int maxFails, int windowSec, int lockSec) { maxFails_ = maxFails; windowSec_ = windowSec; lockSec_ = lockSec; }

private:
    struct Fail { std::vector<int64_t> times; int64_t lockedUntil = 0; };
    bool Limited(const std::string& key, int64_t now);
    void NoteFail(const std::string& key, int64_t now);
    std::filesystem::path PathFor(const std::string& name) const;
    std::filesystem::path dir_;
    int iter_ = 10000;
    int maxFails_ = 5, windowSec_ = 300, lockSec_ = 30;
    std::map<std::string, Fail> fails_;
};

}  // namespace f3
