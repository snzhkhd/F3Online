#pragma once
// Client-side preview of the character values while the creation screen is open (the server recomputes everything when the
// character is created). Same formulas as the server defaults (engine/server/Character.cpp, scripts/config.lua).
#include <algorithm>

namespace f3 {

// base value of skill `i` (index into kSkillNames) from SPECIAL sp[0..6] = S P E C I A L
inline int PreviewSkillBase(int i, const int sp[7]) {
    const int S = sp[0], P = sp[1], E = sp[2], C = sp[3], I = sp[4], A = sp[5];
    switch (i) {
        case 0: return 5 + 4 * A;          case 1: return 20 + 2 * (S + A);   case 2: return 30 + 2 * (S + A);
        case 3: return 4 * C;              case 4: return 5 + 4 * C;          case 5: return 5 + 3 * C + I;
        case 6: return 3 * I;              case 7: return 2 * (P + I);        case 8: return 5 + 2 * (E + I);
        case 9: return 4 * I;              case 10: return 10 + P + A;        case 11: return 5 + 3 * A;
        case 12: return 3 + 3 * A;         default: return 10 + P + A;        // 13 Traps
    }
}
inline int PreviewMaxHp(const int sp[7]) { return 15 + sp[0] + 2 * sp[2]; }
inline int PreviewAp(const int sp[7]) { return 5 + sp[5] / 2; }
inline int PreviewCarry(const int sp[7]) { return 25 + 25 * sp[0]; }
inline int PreviewHealRate(const int sp[7]) { return std::max(1, sp[2] / 3); }

}  // namespace f3
