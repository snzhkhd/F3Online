#pragma once
// UTF-8 aware text layout helpers of the client: word wrapping with a measuring callback (never splits a code point), used by the chat
// log, dialogue, loot / item texts, speech bubbles and nameplates. See docs/localization.md ("Text layout").
#include <functional>
#include <string>
#include <vector>
#include "common/Localization.h"
#include "common/utf8.h"

namespace f3::txt {

// Greedy word wrap. `width(s)` returns the pixel width of s. Words are the text between spaces (a word keeps its trailing space), '\n'
// starts a new paragraph, a word wider than maxW is cut between code points. Empty paragraphs give an empty line when keepEmpty.
template <class Width>
std::vector<std::string> Wrap(const std::string& text, float maxW, Width&& width, bool keepEmpty = true) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const std::string para = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        std::string cur;
        size_t p = 0;
        while (p < para.size()) {
            const size_t sp = para.find(' ', p);
            std::string word = para.substr(p, sp == std::string::npos ? std::string::npos : sp - p + 1);
            p = sp == std::string::npos ? para.size() : sp + 1;
            while (width(word) > maxW && utf8::Count(word) > 1) {   // overlong word: hard split at a code point boundary
                size_t n = utf8::Count(word);
                while (n > 1 && width(utf8::Truncate(word, n)) > maxW) --n;
                if (!cur.empty()) { lines.push_back(cur); cur.clear(); }
                const std::string head = utf8::Truncate(word, n);
                lines.push_back(head);
                word = word.substr(head.size());
            }
            if (!cur.empty() && width(cur + word) > maxW) { lines.push_back(cur); cur.clear(); }
            cur += word;
        }
        if (!cur.empty() || keepEmpty) lines.push_back(cur);
    }
    if (!keepEmpty) return lines;
    while (lines.size() > 1 && lines.back().empty()) lines.pop_back();
    return lines;
}

// Shortens to fit maxW with a trailing "..." (code point safe).
template <class Width>
std::string Ellipsize(std::string s, float maxW, Width&& width) {
    if (width(s) <= maxW) return s;
    size_t n = utf8::Count(s);
    while (n > 1 && width(utf8::Truncate(s, n) + "...") > maxW) --n;
    return utf8::Truncate(s, n) + "...";
}

// Translated name of a skill (skill.<English name>.name, falls back to the English name).
inline std::string SkillLabel(const std::string& en) { return loc::TOr("skill." + en + ".name", en); }

// Splits a template like "HP {n} / 100" at "{n}" into the text before and after it (for the rolling number draw).
inline void SplitNum(const std::string& t, std::string& pre, std::string& post) {
    const size_t p = t.find("{n}");
    if (p == std::string::npos) { pre = t; post.clear(); return; }
    pre = t.substr(0, p);
    post = t.substr(p + 3);
}

}  // namespace f3::txt
