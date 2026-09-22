/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef _PLAYERBOT_DUNGEON_SPELL_CREDIT_BOSS_POLICY_H
#define _PLAYERBOT_DUNGEON_SPELL_CREDIT_BOSS_POLICY_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace DungeonSpellCreditBoss
{
struct EncounterGroupFacts
{
    std::uint32_t mapId = 0;
    std::uint32_t encounterId = 0;
    bool complete = false;
    bool hasKillCreatureCredit = false;
};

struct StaticSpawnFacts
{
    std::uint32_t mapId = 0;
    std::uint32_t spawnMask = 0;
    std::uint32_t spawnId = 0;
    std::uint32_t entry = 0;
    std::string scriptName;
    bool mappedByKillCreatureCredit = false;
};

struct Binding
{
    bool selected = false;
    std::uint32_t encounterId = 0;
    std::uint32_t spawnId = 0;
    std::uint32_t entry = 0;
    std::string scriptName;
};

namespace Detail
{
inline bool HasBossScriptPrefix(std::string const& scriptName)
{
    static constexpr char Prefix[] = "boss_";
    return scriptName.size() >= sizeof(Prefix) - 1 &&
        scriptName.compare(0, sizeof(Prefix) - 1, Prefix) == 0;
}

inline bool SameEncounterFacts(EncounterGroupFacts const& left, EncounterGroupFacts const& right)
{
    return left.mapId == right.mapId && left.encounterId == right.encounterId &&
        left.complete == right.complete &&
        left.hasKillCreatureCredit == right.hasKillCreatureCredit;
}

inline bool SameSpawnFacts(StaticSpawnFacts const& left, StaticSpawnFacts const& right)
{
    return left.mapId == right.mapId && left.spawnMask == right.spawnMask &&
        left.spawnId == right.spawnId && left.entry == right.entry &&
        left.scriptName == right.scriptName &&
        left.mappedByKillCreatureCredit == right.mappedByKillCreatureCredit;
}
}

// This policy deliberately knows nothing about navmesh state or encounter-completion mechanisms.
// It only authorizes the uniquely attributable static spawn; ordinary navigation and activation
// remain responsible for proving that the selected spawn can be reached and attacked.
inline Binding Select(std::uint32_t mapId, std::uint32_t activeSpawnMask,
    std::vector<EncounterGroupFacts> const& encounterInput,
    std::vector<StaticSpawnFacts> const& spawnInput)
{
    if (!mapId || !activeSpawnMask)
        return {};

    std::map<std::uint32_t, EncounterGroupFacts> encounterGroups;
    bool conflictingEncounterFacts = false;
    for (EncounterGroupFacts const& facts : encounterInput)
    {
        if (facts.mapId != mapId)
            continue;

        auto const [itr, inserted] = encounterGroups.emplace(facts.encounterId, facts);
        conflictingEncounterFacts = conflictingEncounterFacts ||
            (!inserted && !Detail::SameEncounterFacts(itr->second, facts));
    }
    if (conflictingEncounterFacts)
        return {};

    std::vector<std::uint32_t> unmatchedEncounterIds;
    for (auto const& [encounterId, facts] : encounterGroups)
    {
        if (!facts.complete && !facts.hasKillCreatureCredit)
            unmatchedEncounterIds.push_back(encounterId);
    }
    if (unmatchedEncounterIds.size() != 1)
        return {};

    std::map<std::uint32_t, StaticSpawnFacts> activeSpawns;
    bool conflictingSpawnFacts = false;
    for (StaticSpawnFacts const& facts : spawnInput)
    {
        if (facts.mapId != mapId || !(facts.spawnMask & activeSpawnMask) ||
            !facts.spawnId || !facts.entry)
        {
            continue;
        }

        auto const [itr, inserted] = activeSpawns.emplace(facts.spawnId, facts);
        conflictingSpawnFacts = conflictingSpawnFacts ||
            (!inserted && !Detail::SameSpawnFacts(itr->second, facts));
    }
    if (conflictingSpawnFacts)
        return {};

    StaticSpawnFacts const* unmatchedBoss = nullptr;
    for (auto const& [spawnId, facts] : activeSpawns)
    {
        (void)spawnId;
        if (facts.mappedByKillCreatureCredit || !Detail::HasBossScriptPrefix(facts.scriptName))
            continue;
        if (unmatchedBoss)
            return {};
        unmatchedBoss = &facts;
    }
    if (!unmatchedBoss)
        return {};

    return {true, unmatchedEncounterIds.front(), unmatchedBoss->spawnId, unmatchedBoss->entry,
        unmatchedBoss->scriptName};
}
}

#endif
