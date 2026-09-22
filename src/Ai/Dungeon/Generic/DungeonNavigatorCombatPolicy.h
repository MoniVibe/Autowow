/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_DUNGEONNAVIGATORCOMBATPOLICY_H
#define PLAYERBOTS_DUNGEONNAVIGATORCOMBATPOLICY_H

#include <cstdint>

namespace DungeonNavigatorCombat
{
constexpr std::uint32_t DefaultAmbiguousTimeoutMs = 30'000;

struct Signals
{
    bool combatFlag = false;
    bool petCombatFlag = false;
    bool liveVictim = false;
    bool liveAttackers = false;
    bool combatRelationships = false;
    bool liveThreat = false;
    bool combatCast = false;
};

enum class Decision
{
    Allow,
    BlockActive,
    BlockAmbiguous,
    RecoverAndAllow,
    AllowRecovered
};

inline constexpr bool HasAuthoritativeActivity(Signals const& signals)
{
    return signals.liveVictim || signals.liveAttackers || signals.liveThreat ||
        signals.combatCast;
}

// Raw combat flags and relationship-only state are intentionally insufficient to freeze the convoy
// forever. Live victims, attackers, threat, or casts remain hard blocks regardless of age, while an
// ambiguous observation must survive a full hysteresis window before the normal AI recovery path may
// run.
inline constexpr Decision Evaluate(Signals const& signals, std::uint32_t ambiguousDurationMs,
                                   bool recoveryIssued,
                                   std::uint32_t timeoutMs = DefaultAmbiguousTimeoutMs)
{
    if (HasAuthoritativeActivity(signals))
        return Decision::BlockActive;
    if (!signals.combatFlag && !signals.petCombatFlag && !signals.combatRelationships)
        return Decision::Allow;
    if (ambiguousDurationMs < timeoutMs)
        return Decision::BlockAmbiguous;
    return recoveryIssued ? Decision::AllowRecovered : Decision::RecoverAndAllow;
}
}

#endif
