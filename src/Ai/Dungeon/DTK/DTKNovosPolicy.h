/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_DTK_NOVOS_POLICY_H
#define PLAYERBOTS_DTK_NOVOS_POLICY_H

namespace DTKNovosPolicy
{
// Arcane Field is applied by a triggered cast and remains as an aura during Novos' shield phase.
// Requiring a current cast makes the encounter strategy disappear between AI ticks.
constexpr bool IsShieldPhase(bool inCombat, bool hasArcaneFieldAura)
{
    return inCombat && hasArcaneFieldAura;
}
}

#endif
