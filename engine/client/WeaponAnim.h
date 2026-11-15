#pragma once
// Weapon -> animation set mapping for human critters (HumM_ / HumF_ clips). Documented in docs/formats/granny.md
// ("Human animation set"). The prototype data does not say which animation set a weapon uses (35 of 51 weapons have
// no decoded attack data), so the class is derived from the prototype key; attack mode type decides burst / single.
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>
#include "ProtoDb.h"

namespace f3 {

enum class WKind { Unarmed, Gun, Melee, Throw };

struct WeaponAnim {
    WKind kind = WKind::Unarmed;
    bool twoHanded = false;   // rifles, miniguns, flamethrowers, railgun: "2HH" clips
    bool burst = false;       // first attack mode is a burst
    bool thrust = false;      // melee: stab (knives) instead of slash
    bool holds() const { return kind != WKind::Unarmed; }
    // clip name infix: Una / 1H / 2HH
    const char* set() const { return kind == WKind::Unarmed ? "Una" : twoHanded ? "2HH" : "1H"; }
    int key() const { return static_cast<int>(kind) * 4 + (twoHanded ? 2 : 0) + (burst ? 1 : 0); }
};

inline bool Has(const std::string& lk, const char* s) { return lk.find(s) != std::string::npos; }

inline WeaponAnim ClassifyWeapon(const ProtoInfo* p) {
    WeaponAnim w;
    if (!p || p->cls != "weapon") return w;
    std::string k = p->key;
    std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (Has(k, "grenade") || Has(k, "dynamite") || Has(k, "rock") || Has(k, "flare") || Has(k, "throwingknife")) {
        w.kind = WKind::Throw;
    } else if (Has(k, "knife") || Has(k, "shiv") || Has(k, "switchblade") || Has(k, "machete") || Has(k, "bat") || Has(k, "crowbar") ||
               Has(k, "hatchet") || Has(k, "wrench") || Has(k, "pipe") || Has(k, "nightstick") || Has(k, "tableleg") || Has(k, "prod")) {
        w.kind = WKind::Melee;
        w.thrust = Has(k, "knife") || Has(k, "shiv") || Has(k, "switchblade");
    } else {
        w.kind = WKind::Gun;
        w.twoHanded = Has(k, "minigun") || Has(k, "flamethrower") || Has(k, "railgun") || Has(k, "arraygun") || Has(k, "rifle") || Has(k, "shotgun") || Has(k, "gauss");
        w.burst = !p->modes.empty() && p->modes[0].type == 1;
    }
    return w;
}

inline std::vector<std::string> HumanClipNames(const std::string& P, int clipKind, const WeaponAnim& w, bool combat) {
    // clipKind: 0 idle 1 walk 2 run 3 attack 4 hit 5 death 6 equip (ClipKind order)
    const std::string S = w.set(), pre = P + "_";
    std::vector<std::string> c;
    switch (clipKind) {
        case 0:
            if (!combat) c.push_back(pre + "Def_" + S + "_Idle_01");
            c.push_back(pre + "Com_" + S + "_Idle_01");
            c.push_back(pre + "Com_Una_Idle_01");
            break;
        case 1:
            c.push_back(pre + "Com_" + S + "_Walk_01");
            c.push_back(pre + "Com_Una_Walk_01");
            break;
        case 2:   // there are no weapon run clips: the unarmed run is used with the weapon in hand
            c.push_back(pre + "Com_Una_Run_01");
            c.push_back(pre + "Com_" + S + "_Walk_01");
            break;
        case 3:
            if (w.kind == WKind::Gun) {
                if (w.burst) c.push_back(pre + "Com_" + S + "_Gun_Burst_01");
                c.push_back(pre + "Com_" + S + "_Gun_Shoot_01");
                c.push_back(pre + "Com_" + S + "_Gun_NoRecoil_01");
            } else if (w.kind == WKind::Melee) {
                c.push_back(pre + (w.thrust ? "Com_1H_Melee_Thrust_01" : "Com_1H_Melee_Slash_01"));
                c.push_back(pre + "Com_1H_Melee_Slash_01");
            } else if (w.kind == WKind::Throw) {
                c.push_back(pre + "Com_1H_Throw_01");
            }
            c.push_back(pre + "Com_Una_Punch_Jab_01");
            c.push_back(pre + "Com_Una_Kick_Snap_01");
            break;
        case 4:
            c.push_back(pre + "Com_" + S + "_DmgFr_01");
            c.push_back(pre + "Com_Una_DmgFr_01");
            break;
        case 5:
            c.push_back(pre + "Com_" + S + "_Laydown_ProneB_01");
            c.push_back(pre + "Com_Una_Laydown_ProneB_01");
            c.push_back(pre + "ProneB_Una_Death_01");
            break;
        case 6:   // draw / holster: Equip with the new weapon's set, Unequip when the hands become empty
            c.push_back(pre + "Com_" + S + (w.holds() ? "_Equip_01" : "_Unequip_01"));
            c.push_back(pre + "Com_Una_Equip_01");
            break;
        default: break;
    }
    return c;
}

}  // namespace f3
