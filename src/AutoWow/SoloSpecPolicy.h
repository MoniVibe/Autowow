/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_SOLO_SPEC_POLICY_H
#define AUTOWOW_SOLO_SPEC_POLICY_H

#include <cstdint>

class Player;

// AutoWow.Cohort.SoloSpec (default 0). S109-S113 (364 bot-h, 797 deaths): six off-spec cohort bots (Subtlety
// rogue, Arcane mages, Restoration druid, Discipline priests) took 304 deaths; died-per-fight Sub 61.8%, Arcane
// 26.9%, Resto 24.2%, Disc 15.3% vs Combat 19.4%, Frost 5.7%, Balance 7.8%. On login a cohort bot whose talent
// tree is not a solo-leveling tree is reset (free) and re-talented by the stock premade template of its class's
// solo spec (PlayerbotFactory::InitTalentsBySpecNo); its strategies follow the new tree on the login reset.
// Value-only below the runtime section; unit-tested (SoloSpecPolicyTest).
namespace AutoWowSoloSpec
{
inline constexpr std::int32_t kKeep = -1;

// Core class id (CLASS_WARRIOR 1 ... CLASS_DRUID 11) -> allowed talent tabs (bit per tab, PlayerbotAI.h
// *_TAB_* order) and the premade spec index (AiPlayerbot.PremadeSpecName.<class>.<n>) applied otherwise.
struct SoloRow
{
    std::uint8_t allowedTabs = 0;  // 0 = class not handled
    std::int32_t specNo = kKeep;
};

inline constexpr SoloRow RowOf(std::uint32_t classId)
{
    switch (classId)
    {
        case 1: return {0b011, 0};   // warrior: Arms / Fury -> 1.0 arms pve
        case 2: return {0b100, 2};   // paladin: Retribution -> 2.2 ret pve
        case 3: return {0b001, 0};   // hunter: Beast Mastery -> 3.0 bm pve
        case 4: return {0b010, 1};   // rogue: Combat -> 4.1 combat pve
        case 5: return {0b100, 2};   // priest: Shadow -> 5.2 shadow pve (5.6 with PriestShadowLevelingSpec)
        case 6: return {0b101, 2};   // death knight: Blood / Unholy -> 6.2 unholy pve
        case 7: return {0b010, 1};   // shaman: Enhancement -> 7.1 enh pve
        case 8: return {0b110, 2};   // mage: Fire / Frost -> 8.2 frost pve
        case 9: return {0b011, 0};   // warlock: Affliction / Demonology -> 9.0 affli pve
        case 11: return {0b011, 3};  // druid: Balance / Feral -> 11.3 cat pve
        default: return {};
    }
}

// The premade spec to apply, or kKeep. `tab` = the tree holding most points (AiFactory::GetPlayerSpecTab);
// `spentPoints` = talent points spent (0: nothing to reset; the stock factory picks at level 10).
inline std::int32_t SpecToApply(std::uint32_t classId, std::uint32_t tab, std::uint32_t spentPoints)
{
    SoloRow const row = RowOf(classId);
    if (!row.allowedTabs || !spentPoints || tab > 2 || ((row.allowedTabs >> tab) & 1u))
        return kKeep;
    return row.specNo;
}

// ---- runtime (SoloSpec.cpp) -------------------------------------------------------------------------
// Reads AutoWow.Cohort.SoloSpec / SoloSpecExempt (guid ranges, "62967,62990-62995"). World init.
void LoadConfig();
bool Enabled();
// World thread, from PlayerbotHolder::OnBotLogin before the strategy reset. A non-exempt cohort bot
// (AutoWowGuilds::Cohort() ranges, AI-driven) outside any group and instance gets its solo spec; logs [SoloSpec].
void OnLogin(Player* bot);
}  // namespace AutoWowSoloSpec

#endif
