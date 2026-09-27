/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_CLASSQUEST_POLICY_H
#define AUTOWOW_CLASSQUEST_POLICY_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class Player;

// Automatic class-quest reward grants (AutoWow.ClassQuests.Enable, default 0). Owner ruling 2026-09-27: bots
// rarely finish class quests and miss class-defining abilities, so a cohort bot (AutoWowGuilds::Cohort() ranges)
// or supply role bot gets a class quest's reward once it reaches the quest's MinLevel: the learned spells
// (player->learnSpell) and the class key item (stored once; skipped when owned anywhere, bank included). Quest
// status is never touched (questability data stays honest). Only the rows below; nothing else is ever granted.
// Mount rows (they also teach a riding skill: gold value) need AutoWow.ClassQuests.Mounts (default 0) as well.
//
// Table verified 2026-09-27 against the world DB (quest_template / quest_template_addon: AllowableClasses = one
// class, RewardSpell or RewardDisplaySpell, RewardItem1) and /root/p1data/dbc/Spell.dbc (the cast reward spell's
// SPELL_EFFECT_LEARN_SPELL (36) triggers are the learned spells; core casts RewardSpell, else
// RewardDisplaySpell). Rows with identical (class, level, spells, item) are merged: races = the OR of the chains'
// AllowableRaces (0 = any), quest = the lowest quest id of the merged chains. Left out, with the reason:
//   - rogue Poisons 2842 (quest 2359): SPELL_EFFECT_TRADE_SKILL of skill 40, which has no SkillLineAbility rows in
//     3.3.5 (poison crafting was removed): the spell is inert;
//   - priest race quests (5627-5680, 10376-10379): their reward spells are missing from the 3.3.5 Spell.dbc;
//   - rewards that teach nothing: Swift Wind aura (Call of Air: the Air Totem item row stays), Mark of the Wild
//     8257, Teleport to Azshara Tower 8250, Dominion Over Acherus 12657, Bombing Run 11102, 13863/13864 (missing).
// Totem of the Earthen Ring 46978 (Relic of the Earthen Ring 14100/14111, spell 66747 creates it from the four
// totems) is TotemCategory 21 "Master Totem" (mask 15 covers Earth 2 / Air 3 / Fire 4 / Water 5): a basic totem
// is not granted while the master is owned or granted now. Basic totems already owned are left in the bags.
//
// Value-only: integer ids and levels, fixed table order (class, level, quest), no floats, no RNG.
namespace AutoWowClassQuests
{
inline constexpr std::uint32_t kMasterTotem = 46978;  // Totem of the Earthen Ring

struct Reward
{
    std::uint32_t quest = 0;       // representative quest (lowest id of the merged chains)
    std::uint32_t classMask = 0;   // 1 << (class - 1)
    std::uint32_t raceMask = 0;    // OR of AllowableRaces; 0 = any race
    std::uint32_t level = 0;       // quest MinLevel
    std::array<std::uint32_t, 3> spells{};  // learned spells (LEARN_SPELL triggers), 0 = unused
    std::uint32_t item = 0;        // class key item (RewardItem1), 0 = none
    std::uint32_t supersededBy = 0;  // item covering this one (owned or granted now -> skip the item)
    bool mount = false;            // teaches a riding skill: also needs AutoWow.ClassQuests.Mounts
};

// clang-format off
inline constexpr std::array<Reward, 33> kRewards{{
    // Warrior (1)
    {1498,    1, 1791, 10, {71, 7386, 355}, 0, 0, false},      // Path of Defense: Defensive Stance, Sunder, Taunt
    {1719,    1,    0, 30, {2458, 20252, 0}, 0, 0, false},     // The Affray: Berserker Stance, Intercept
    // Paladin (2)
    {1785,    2, 1541, 12, {7328, 0, 0}, 0, 0, false},         // Redemption
    {1652,    2, 1613, 20, {5502, 0, 0}, 0, 0, false},         // Sense Undead
    {1661,    2, 1101, 40, {13819, 33388, 0}, 0, 0, true},     // Warhorse + Apprentice Riding
    {9712,    2,  512, 40, {34769, 33388, 0}, 0, 0, true},     // Thalassian Warhorse + Apprentice Riding
    {7647,    2, 1029, 60, {23214, 33391, 0}, 0, 0, true},     // Charger + Journeyman Riding
    {9737,    2,  512, 60, {34767, 33391, 0}, 0, 0, true},     // Thalassian Charger + Journeyman Riding
    // Hunter (4)
    {6081,    4, 1710, 10, {6991, 982, 0}, 0, 0, false},       // Training the Beast: Feed Pet, Revive Pet
    {6082,    4, 1710, 10, {1515, 883, 2641}, 0, 0, false},    // Taming the Beast: Tame Beast, Call Pet, Dismiss Pet
    // Death Knight (32)
    {12619,  32,    0, 55, {53428, 0, 0}, 0, 0, false},        // Runeforging
    {12687,  32,    0, 55, {48778, 33391, 0}, 0, 0, true},     // Acherus Deathcharger + Journeyman Riding
    {12801,  32,    0, 55, {50977, 0, 0}, 0, 0, false},        // Death Gate
    // Shaman (64)
    {1518,   64, 1263,  4, {8071, 0, 0}, 5175, kMasterTotem, false},  // Call of Earth: Stoneskin Totem, Earth Totem
    {1527,   64, 1791, 10, {3599, 0, 0}, 5176, kMasterTotem, false},  // Call of Fire: Searing Totem, Fire Totem
    {96,     64, 1714, 20, {5394, 0, 0}, 5177, kMasterTotem, false},  // Call of Water: Healing Stream, Water Totem
    {1531,   64, 1714, 30, {0, 0, 0}, 5178, kMasterTotem, false},     // Call of Air: Air Totem
    {14100,  64,    0, 30, {0, 0, 0}, kMasterTotem, 0, false},        // Relic of the Earthen Ring
    // Mage (128)
    {7463,  128,    0, 60, {10140, 0, 0}, 0, 0, false},        // Conjure Water (Rank 7)
    {9364,  128,    0, 60, {28272, 0, 0}, 0, 0, false},        // Polymorph (Pig)
    {12172, 128, 1791, 71, {53140, 0, 0}, 0, 0, false},        // Teleport: Dalaran
    // Warlock (256)
    {1470,  256,  755,  1, {688, 0, 0}, 0, 0, false},          // Summon Imp
    {1471,  256, 1631, 10, {697, 0, 0}, 0, 0, false},          // Summon Voidwalker
    {1474,  256, 1791, 20, {712, 0, 0}, 0, 0, false},          // Summon Succubus
    {1795,  256,    0, 30, {691, 0, 0}, 0, 0, false},          // Summon Felhunter
    {4490,  256,    0, 40, {5784, 33388, 0}, 0, 0, true},      // Felsteed + Apprentice Riding
    {7603,  256,    0, 50, {1122, 0, 0}, 0, 0, false},         // Inferno
    {7583,  256,    0, 60, {18540, 0, 0}, 0, 0, false},        // Ritual of Doom
    {7631,  256,  595, 60, {23161, 33391, 0}, 0, 0, true},     // Dreadsteed + Journeyman Riding
    // Druid (1024)
    {6001, 1024,   40, 10, {5487, 6795, 6807}, 0, 0, false},   // Body and Heart: Bear Form, Growl, Maul
    {6125, 1024, 1791, 14, {8946, 0, 0}, 0, 0, false},         // Power over Poison: Cure Poison
    {31,   1024,   40, 16, {1066, 0, 0}, 0, 0, false},         // Aquatic Form
    {11001, 1024,   0, 70, {40120, 0, 0}, 0, 0, true},         // Swift Flight Form (a travel form: mount rule)
}};
// clang-format on

inline constexpr std::uint32_t ClassMask(std::uint32_t cls) { return cls >= 1 && cls <= 32 ? 1u << (cls - 1) : 0; }
inline constexpr std::uint32_t RaceMask(std::uint32_t race) { return race >= 1 && race <= 32 ? 1u << (race - 1) : 0; }

inline constexpr bool Eligible(Reward const& g, std::uint32_t cls, std::uint32_t race, std::uint32_t level,
                               bool mounts)
{
    return (g.classMask & ClassMask(cls)) && (!g.raceMask || (g.raceMask & RaceMask(race))) && level >= g.level &&
           (mounts || !g.mount);
}

// One learned spell or one stored item (never both).
struct Pick
{
    std::uint32_t quest = 0;
    std::uint32_t spell = 0;
    std::uint32_t item = 0;
    std::uint32_t level = 0;  // the row's quest MinLevel
};

// What the bot is owed now, in table order. known(spell) / owns(item) are the bot's state (owns: bank included).
template <class Known, class Owns>
std::vector<Pick> Select(std::uint32_t cls, std::uint32_t race, std::uint32_t level, bool mounts, Known known,
                         Owns owns)
{
    auto grantedNow = [&](std::uint32_t item)
    {
        for (Reward const& g : kRewards)
            if (g.item == item && Eligible(g, cls, race, level, mounts))
                return true;
        return false;
    };
    std::vector<Pick> out;
    auto picked = [&out](std::uint32_t spell)
    {
        for (Pick const& p : out)
            if (p.spell == spell)
                return true;
        return false;
    };
    for (Reward const& g : kRewards)
    {
        if (!Eligible(g, cls, race, level, mounts))
            continue;
        for (std::uint32_t spell : g.spells)
            if (spell && !known(spell) && !picked(spell))
                out.push_back(Pick{g.quest, spell, 0, g.level});
        if (g.item && !owns(g.item) && !(g.supersededBy && (owns(g.supersededBy) || grantedNow(g.supersededBy))))
            out.push_back(Pick{g.quest, 0, g.item, g.level});
    }
    return out;
}

// Trailing ledger fields; the pick's quest is the row's own `quest` field.
inline std::string LedgerFields(Pick const& p)
{
    return ",\"spell\":" + std::to_string(p.spell) + ",\"item\":" + std::to_string(p.item) +
           ",\"level\":" + std::to_string(p.level);
}

namespace detail
{
inline bool gEnabled = false;
inline bool gMounts = false;
}  // namespace detail
inline bool Enabled() { return detail::gEnabled; }

// Reads AutoWow.ClassQuests.Enable / .Mounts. Once at world init.
void LoadConfig();
// Bot login (world thread) and level change (the bot's map thread): grant what Select owes a cohort / supply
// role bot. No-op when disabled or for a real player.
void Grant(Player* bot, char const* cause);
// Registers the level-change PlayerScript (every hook early-returns on the cached flag).
void AddScripts();
}  // namespace AutoWowClassQuests

#endif
