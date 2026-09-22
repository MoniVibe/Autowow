/*
 * Pure policy for the bounded Probe Lab reset surface. Live world mutation stays in
 * ProbeResetControl.cpp so these decisions can be unit-tested without a server.
 */
#ifndef MOD_PLAYERBOTS_PROBE_RESET_POLICY_H
#define MOD_PLAYERBOTS_PROBE_RESET_POLICY_H

#include <array>
#include <cstdint>
#include <string_view>

namespace AutoWowProbeReset
{
struct ExteriorRoute
{
    std::string_view name;
    std::uint32_t map;
    float x;
    float y;
    float z;
    float orientation;
};

// This allowlist intentionally contains only exterior staging routes. Interior entry and
// first-pack coordinates from the general bridge route table are excluded by construction.
inline constexpr std::array<ExteriorRoute, 5> kExteriorRoutes = {{
    {"uk-exterior", 571, 1257.11f, -4854.48f, 37.248f, 0.274f},
    {"nexus-exterior", 571, 3902.79f, 6985.69f, 75.0f, 0.0f},
    {"dtk-exterior", 571, 4775.32f, -2017.16f, 235.0f, 0.0f},
    {"ony-exterior", 1, -4747.17f, -3753.27f, 49.8122f, 0.0f},
    {"voa-exterior", 571, 5494.88f, 2839.81f, 420.811f, 0.0f},
}};

inline constexpr ExteriorRoute const* ResolveExteriorRoute(std::string_view name)
{
    for (ExteriorRoute const& route : kExteriorRoutes)
        if (route.name == name)
            return &route;
    return nullptr;
}

struct CombatSignals
{
    bool combatFlag = false;
    bool victim = false;
    bool attackers = false;
    bool combatManagerReferences = false;
    bool nonMeleeCast = false;
};

enum class CombatPolicy
{
    ClearStaleFlag,
    Ready,
    RefuseActive
};

inline constexpr CombatPolicy ClassifyCombat(CombatSignals const& state)
{
    if (state.victim || state.attackers || state.combatManagerReferences || state.nonMeleeCast)
        return CombatPolicy::RefuseActive;
    return state.combatFlag ? CombatPolicy::ClearStaleFlag : CombatPolicy::Ready;
}

struct GroupSignals
{
    bool battleground = false;
    bool battlefield = false;
    bool lfg = false;
    bool foreignMember = false;
};

inline constexpr bool MayDisbandGroup(GroupSignals const& state)
{
    return !state.battleground && !state.battlefield && !state.lfg && !state.foreignMember;
}

enum class BindPolicy
{
    Ready,
    ClearTemporary,
    RefusePermanent
};

inline constexpr BindPolicy ClassifyTargetBind(bool present, bool permanent)
{
    if (!present)
        return BindPolicy::Ready;
    return permanent ? BindPolicy::RefusePermanent : BindPolicy::ClearTemporary;
}
} // namespace AutoWowProbeReset

#endif
