#pragma once
// UTF-8 helpers shared by server and client: decoding, length in code points, truncation at code point boundaries, validation
// and the text rules for player input (names, chat). Header only. Cyrillic is a first class citizen (names may use it, chat is
// UTF-8 end to end); everything outside Latin / Cyrillic stays out of names but is allowed in chat text (minus control characters).
#include <cstdint>
#include <string>

namespace f3::utf8 {

// Decodes the code point at s[i] and advances i. Returns false (and advances one byte, cp = 0xFFFD) on an invalid, overlong,
// surrogate or out of range sequence.
inline bool Next(const std::string& s, size_t& i, uint32_t& cp) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    auto bad = [&]() { cp = 0xFFFD; ++i; return false; };
    if (c < 0x80) { cp = c; ++i; return true; }
    int n = 0;
    uint32_t v = 0, minv = 0;
    if ((c & 0xE0) == 0xC0) { n = 1; v = c & 0x1F; minv = 0x80; }
    else if ((c & 0xF0) == 0xE0) { n = 2; v = c & 0x0F; minv = 0x800; }
    else if ((c & 0xF8) == 0xF0) { n = 3; v = c & 0x07; minv = 0x10000; }
    else return bad();
    if (i + static_cast<size_t>(n) >= s.size()) return bad();
    for (int k = 1; k <= n; ++k) {
        const unsigned char d = static_cast<unsigned char>(s[i + k]);
        if ((d & 0xC0) != 0x80) return bad();
        v = (v << 6) | (d & 0x3F);
    }
    if (v < minv || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return bad();
    i += static_cast<size_t>(n) + 1;
    cp = v;
    return true;
}

inline void Append(std::string& out, uint32_t cp) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { out.push_back(static_cast<char>(0xE0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else { out.push_back(static_cast<char>(0xF0 | (cp >> 18))); out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
}

inline bool Valid(const std::string& s) {
    for (size_t i = 0; i < s.size();) { uint32_t cp; if (!Next(s, i, cp)) return false; }
    return true;
}

// Number of code points (an invalid byte counts as one).
inline size_t Count(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++n) { uint32_t cp; Next(s, i, cp); }
    return n;
}

// Byte offset after the first `maxCp` code points (s.size() if it has fewer).
inline size_t OffsetOf(const std::string& s, size_t maxCp) {
    size_t i = 0, n = 0;
    while (i < s.size() && n < maxCp) { uint32_t cp; Next(s, i, cp); ++n; }
    return i;
}
inline std::string Truncate(const std::string& s, size_t maxCp) { return s.substr(0, OffsetOf(s, maxCp)); }

// Removes the last code point (editing: backspace).
inline void PopBack(std::string& s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    s.resize(i);
}

// Basic multilingual helpers (Latin + Cyrillic only; no locale tables).
inline bool IsCyrillic(uint32_t cp) { return cp >= 0x400 && cp <= 0x4FF; }
inline bool IsLetter(uint32_t cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= 0x410 && cp <= 0x44F) || cp == 0x401 || cp == 0x451;
}
inline bool IsDigit(uint32_t cp) { return cp >= '0' && cp <= '9'; }
inline uint32_t Lower(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if (cp >= 0x410 && cp <= 0x42F) return cp + 32;
    if (cp >= 0x400 && cp <= 0x40F) return cp + 80;
    return cp;
}
inline std::string Lower(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) { uint32_t cp; Next(s, i, cp); Append(out, Lower(cp)); }
    return out;
}

// Code points that never belong into chat / names: C0 / C1 controls, DEL, line / paragraph separators, bidi overrides and
// isolates (text spoofing), zero width and BOM characters, non characters, the replacement character itself.
inline bool IsForbiddenInText(uint32_t cp) {
    if (cp < 32 || cp == 127 || (cp >= 0x80 && cp < 0xA0)) return true;
    if (cp == 0x2028 || cp == 0x2029) return true;
    if (cp >= 0x202A && cp <= 0x202E) return true;
    if (cp >= 0x2066 && cp <= 0x2069) return true;
    if (cp >= 0x200B && cp <= 0x200F) return true;
    if (cp == 0xFEFF || cp == 0xFFFD || cp == 0xFFFE || cp == 0xFFFF) return true;
    if (cp >= 0xE000 && cp <= 0xF8FF) return true;   // private use
    return false;
}

// Player text (chat, mail): drops invalid sequences and forbidden code points, trims spaces, keeps at most maxCp code points.
inline std::string SanitizeText(const std::string& in, size_t maxCp) {
    std::string out;
    for (size_t i = 0; i < in.size();) {
        uint32_t cp;
        if (!Next(in, i, cp)) continue;
        if (IsForbiddenInText(cp)) continue;
        Append(out, cp);
    }
    const size_t b = out.find_first_not_of(' ');
    if (b == std::string::npos) return {};
    const size_t e = out.find_last_not_of(' ');
    out = out.substr(b, e - b + 1);
    return Truncate(out, maxCp);
}

// Character / account names: minCp..maxCp code points, Latin or Cyrillic letters, digits and '_'.
inline bool ValidName(const std::string& n, size_t minCp = 2, size_t maxCp = 16) {
    size_t count = 0;
    for (size_t i = 0; i < n.size();) {
        uint32_t cp;
        if (!Next(n, i, cp)) return false;
        if (!(IsLetter(cp) || IsDigit(cp) || cp == '_')) return false;
        ++count;
    }
    return count >= minCp && count <= maxCp;
}

}  // namespace f3::utf8
