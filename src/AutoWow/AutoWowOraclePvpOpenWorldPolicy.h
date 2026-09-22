/*
 * Pure bounded AutoWow Oracle policy for battleground and open-world decisions.
 *
 * Callers normalize live state into the value types below. The policy returns facts and intents
 * only; an executor owns every external operation. Fixed arrays, explicit counts, and stable keys
 * keep every traversal finite and permutation-independent.
 */
#ifndef MOD_PLAYERBOTS_AUTOWOW_ORACLE_PVP_OPEN_WORLD_POLICY_H
#define MOD_PLAYERBOTS_AUTOWOW_ORACLE_PVP_OPEN_WORLD_POLICY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace AutoWowOraclePvpOpenWorld
{
using Guid = std::uint64_t;
using StableId = std::uint64_t;
using Tick = std::uint64_t;
using Score = std::int32_t;

inline constexpr std::size_t kMaxSquadMembers = 16;
inline constexpr std::size_t kMaxObjectives = 16;
inline constexpr std::size_t kMaxTargets = 64;
inline constexpr std::uint16_t kPermilleMax = 1000;
inline constexpr std::uint16_t kCriticalHealthPermille = 300;
inline constexpr std::uint16_t kProtectionHealthPermille = 850;
inline constexpr std::uint8_t kMaxRecoveryAttempts = 4;
inline constexpr std::uint8_t kMaxRegroupNoProgress = 3;
inline constexpr std::uint32_t kIntentTtlTicks = 3;

enum class PlannerMode : std::uint8_t
{
    Unknown,
    Battleground,
    OpenWorld,
    Campaign
};

inline constexpr std::string_view PlannerModeName(PlannerMode mode)
{
    switch (mode)
    {
        case PlannerMode::Unknown:
            return "unknown";
        case PlannerMode::Battleground:
            return "battleground";
        case PlannerMode::OpenWorld:
            return "open_world";
        case PlannerMode::Campaign:
            return "campaign";
    }
    return "unknown";
}

enum class SquadRole : std::uint8_t
{
    Unassigned,
    Captain,
    Healer,
    Tank,
    FlagCarrier,
    Escort,
    Interceptor,
    Defender,
    Recovery,
    Damage
};

inline constexpr std::string_view SquadRoleName(SquadRole role)
{
    switch (role)
    {
        case SquadRole::Unassigned:
            return "unassigned";
        case SquadRole::Captain:
            return "captain";
        case SquadRole::Healer:
            return "healer";
        case SquadRole::Tank:
            return "tank";
        case SquadRole::FlagCarrier:
            return "flag_carrier";
        case SquadRole::Escort:
            return "escort";
        case SquadRole::Interceptor:
            return "interceptor";
        case SquadRole::Defender:
            return "defender";
        case SquadRole::Recovery:
            return "recovery";
        case SquadRole::Damage:
            return "damage";
    }
    return "unassigned";
}

enum class ObjectiveKind : std::uint8_t
{
    Capture,
    Defend,
    Escort,
    Intercept,
    Recover
};

inline constexpr std::string_view ObjectiveKindName(ObjectiveKind kind)
{
    switch (kind)
    {
        case ObjectiveKind::Capture:
            return "capture";
        case ObjectiveKind::Defend:
            return "defend";
        case ObjectiveKind::Escort:
            return "escort";
        case ObjectiveKind::Intercept:
            return "intercept";
        case ObjectiveKind::Recover:
            return "recover";
    }
    return "capture";
}

enum class IntentKind : std::uint8_t
{
    None,
    CaptureObjective,
    DefendObjective,
    EscortCarrier,
    InterceptCarrier,
    RecoverFlag,
    AttackHostile,
    DefendAlly,
    ProtectHealer,
    ProtectTank,
    Regroup,
    RecoverDeath,
    AcceptCampaignChallenge,
    DeclineCampaignChallenge,
    Disengage
};

inline constexpr std::string_view IntentKindName(IntentKind intent)
{
    switch (intent)
    {
        case IntentKind::None:
            return "none";
        case IntentKind::CaptureObjective:
            return "capture_objective";
        case IntentKind::DefendObjective:
            return "defend_objective";
        case IntentKind::EscortCarrier:
            return "escort_carrier";
        case IntentKind::InterceptCarrier:
            return "intercept_carrier";
        case IntentKind::RecoverFlag:
            return "recover_flag";
        case IntentKind::AttackHostile:
            return "attack_hostile";
        case IntentKind::DefendAlly:
            return "defend_ally";
        case IntentKind::ProtectHealer:
            return "protect_healer";
        case IntentKind::ProtectTank:
            return "protect_tank";
        case IntentKind::Regroup:
            return "regroup";
        case IntentKind::RecoverDeath:
            return "recover_death";
        case IntentKind::AcceptCampaignChallenge:
            return "accept_campaign_challenge";
        case IntentKind::DeclineCampaignChallenge:
            return "decline_campaign_challenge";
        case IntentKind::Disengage:
            return "disengage";
    }
    return "none";
}

enum class Reason : std::uint8_t
{
    None,
    Planned,
    InvalidFrame,
    NoActiveMember,
    ObjectiveUnavailable,
    NoObjectiveOwner,
    RetainedOwner,
    InvalidTarget,
    TargetDead,
    NonHostile,
    SelfDefenseNotEnabled,
    SelfDefenseNotTriggered,
    NoEligibleTarget,
    NoProtectionNeeded,
    RecoveryNotNeeded,
    RecoveryFactsMissing,
    RecoveryBudgetExhausted,
    DeadMemberNoHealer,
    RegroupNotNeeded,
    RegroupBudgetExhausted,
    ChallengeNotObserved,
    InvalidChallengeFact,
    ChallengeAccepted,
    ChallengeDeclined
};

inline constexpr std::string_view ReasonName(Reason reason)
{
    switch (reason)
    {
        case Reason::None:
            return "none";
        case Reason::Planned:
            return "planned";
        case Reason::InvalidFrame:
            return "invalid_frame";
        case Reason::NoActiveMember:
            return "no_active_member";
        case Reason::ObjectiveUnavailable:
            return "objective_unavailable";
        case Reason::NoObjectiveOwner:
            return "no_objective_owner";
        case Reason::RetainedOwner:
            return "retained_owner";
        case Reason::InvalidTarget:
            return "invalid_target";
        case Reason::TargetDead:
            return "target_dead";
        case Reason::NonHostile:
            return "non_hostile";
        case Reason::SelfDefenseNotEnabled:
            return "self_defense_not_enabled";
        case Reason::SelfDefenseNotTriggered:
            return "self_defense_not_triggered";
        case Reason::NoEligibleTarget:
            return "no_eligible_target";
        case Reason::NoProtectionNeeded:
            return "no_protection_needed";
        case Reason::RecoveryNotNeeded:
            return "recovery_not_needed";
        case Reason::RecoveryFactsMissing:
            return "recovery_facts_missing";
        case Reason::RecoveryBudgetExhausted:
            return "recovery_budget_exhausted";
        case Reason::DeadMemberNoHealer:
            return "dead_member_no_healer";
        case Reason::RegroupNotNeeded:
            return "regroup_not_needed";
        case Reason::RegroupBudgetExhausted:
            return "regroup_budget_exhausted";
        case Reason::ChallengeNotObserved:
            return "challenge_not_observed";
        case Reason::InvalidChallengeFact:
            return "invalid_challenge_fact";
        case Reason::ChallengeAccepted:
            return "challenge_accepted";
        case Reason::ChallengeDeclined:
            return "challenge_declined";
    }
    return "none";
}

inline constexpr std::uint16_t ClampPermille(std::uint16_t value)
{
    return value > kPermilleMax ? kPermilleMax : value;
}

inline constexpr Score HealthDeficit(std::uint16_t healthPermille)
{
    return static_cast<Score>(kPermilleMax - ClampPermille(healthPermille));
}

inline constexpr Score DistancePreference(std::uint16_t distancePermille)
{
    return static_cast<Score>(kPermilleMax - ClampPermille(distancePermille));
}

inline constexpr Score AddScore(Score left, Score right)
{
    Score const maximum = std::numeric_limits<Score>::max();
    Score const minimum = std::numeric_limits<Score>::min();
    if (right > 0 && left > maximum - right)
        return maximum;
    if (right < 0 && left < minimum - right)
        return minimum;
    return left + right;
}

struct SquadMemberFacts
{
    Guid guid = 0;
    bool alive = true;
    bool connected = true;
    bool canHeal = false;
    bool canTank = false;
    bool canCarryFlag = false;
    bool canEscort = false;
    bool canIntercept = false;
    bool canDefend = false;
    bool canRecover = false;
    bool underAttack = false;
    std::uint16_t healthPermille = kPermilleMax;
    std::uint16_t distanceToObjectivePermille = kPermilleMax;
};

struct SquadFacts
{
    StableId squadId = 0;
    std::array<SquadMemberFacts, kMaxSquadMembers> members{};
    std::size_t memberCount = 0;
};

struct RoleAssignment
{
    Guid guid = 0;
    SquadRole role = SquadRole::Unassigned;
};

struct SquadRolePlan
{
    bool valid = false;
    std::size_t assignmentCount = 0;
    Reason reason = Reason::NoActiveMember;
    std::array<RoleAssignment, kMaxSquadMembers> assignments{};
};

inline bool SquadFrameWithinBounds(SquadFacts const& squad)
{
    if (squad.memberCount > squad.members.size())
        return false;

    for (std::size_t index = 0; index < squad.memberCount; ++index)
    {
        if (squad.members[index].guid == 0)
            return false;
        for (std::size_t other = 0; other < index; ++other)
            if (squad.members[other].guid == squad.members[index].guid)
                return false;
    }
    return true;
}

inline bool IsActive(SquadMemberFacts const& member)
{
    return member.guid != 0 && member.alive && member.connected;
}

inline bool SupportsRole(SquadMemberFacts const& member, SquadRole role)
{
    switch (role)
    {
        case SquadRole::Healer:
            return member.canHeal;
        case SquadRole::Tank:
            return member.canTank;
        case SquadRole::FlagCarrier:
            return member.canCarryFlag;
        case SquadRole::Escort:
            return member.canEscort;
        case SquadRole::Interceptor:
            return member.canIntercept;
        case SquadRole::Defender:
            return member.canDefend;
        case SquadRole::Recovery:
            return member.canRecover;
        case SquadRole::Captain:
        case SquadRole::Damage:
        case SquadRole::Unassigned:
            return false;
    }
    return false;
}

inline bool IsUsedGuid(std::array<Guid, kMaxSquadMembers> const& used, std::size_t usedCount,
    Guid guid)
{
    for (std::size_t index = 0; index < usedCount; ++index)
        if (used[index] == guid)
            return true;
    return false;
}

inline Guid SelectRoleMember(SquadFacts const& squad, SquadRole role,
    std::array<Guid, kMaxSquadMembers> const& used, std::size_t usedCount)
{
    Guid selected = 0;
    for (std::size_t index = 0; index < squad.memberCount; ++index)
    {
        SquadMemberFacts const& member = squad.members[index];
        if (!IsActive(member) || IsUsedGuid(used, usedCount, member.guid))
            continue;
        if (role != SquadRole::Captain && role != SquadRole::Damage && !SupportsRole(member, role))
            continue;
        if (selected == 0 || member.guid < selected)
            selected = member.guid;
    }
    return selected;
}

inline void AddRoleAssignment(SquadRolePlan& plan, SquadFacts const& squad, SquadRole role,
    std::array<Guid, kMaxSquadMembers>& used, std::size_t& usedCount)
{
    if (plan.assignmentCount >= plan.assignments.size() || usedCount >= used.size())
        return;

    Guid const selected = SelectRoleMember(squad, role, used, usedCount);
    if (selected == 0)
        return;

    plan.assignments[plan.assignmentCount++] = {selected, role};
    used[usedCount++] = selected;
}

inline SquadRolePlan BuildSquadRolePlan(SquadFacts const& squad)
{
    SquadRolePlan plan;
    if (!SquadFrameWithinBounds(squad))
    {
        plan.reason = Reason::InvalidFrame;
        return plan;
    }

    std::array<Guid, kMaxSquadMembers> used{};
    std::size_t usedCount = 0;
    AddRoleAssignment(plan, squad, SquadRole::Captain, used, usedCount);
    AddRoleAssignment(plan, squad, SquadRole::Healer, used, usedCount);
    AddRoleAssignment(plan, squad, SquadRole::Tank, used, usedCount);
    AddRoleAssignment(plan, squad, SquadRole::FlagCarrier, used, usedCount);
    AddRoleAssignment(plan, squad, SquadRole::Escort, used, usedCount);
    AddRoleAssignment(plan, squad, SquadRole::Interceptor, used, usedCount);
    AddRoleAssignment(plan, squad, SquadRole::Defender, used, usedCount);
    AddRoleAssignment(plan, squad, SquadRole::Recovery, used, usedCount);

    for (std::size_t index = 0; index < squad.memberCount; ++index)
    {
        SquadMemberFacts const& member = squad.members[index];
        if (!IsActive(member) || IsUsedGuid(used, usedCount, member.guid) ||
            plan.assignmentCount >= plan.assignments.size())
            continue;
        plan.assignments[plan.assignmentCount++] = {member.guid, SquadRole::Damage};
        used[usedCount++] = member.guid;
    }

    // Role choice is independent of source order. Canonicalize the returned assignment list by
    // GUID so a caller can compare plans without first sorting a live roster.
    for (std::size_t index = 0; index < plan.assignmentCount; ++index)
    {
        std::size_t smallest = index;
        for (std::size_t other = index + 1; other < plan.assignmentCount; ++other)
            if (plan.assignments[other].guid < plan.assignments[smallest].guid)
                smallest = other;
        if (smallest != index)
        {
            RoleAssignment const swap = plan.assignments[index];
            plan.assignments[index] = plan.assignments[smallest];
            plan.assignments[smallest] = swap;
        }
    }

    plan.valid = plan.assignmentCount != 0;
    plan.reason = plan.valid ? Reason::Planned : Reason::NoActiveMember;
    return plan;
}

inline SquadRole RoleFor(SquadRolePlan const& plan, Guid guid)
{
    for (std::size_t index = 0; index < plan.assignmentCount; ++index)
        if (plan.assignments[index].guid == guid)
            return plan.assignments[index].role;
    return SquadRole::Unassigned;
}

inline SquadMemberFacts const* FindMember(SquadFacts const& squad, Guid guid)
{
    for (std::size_t index = 0; index < squad.memberCount; ++index)
        if (squad.members[index].guid == guid)
            return &squad.members[index];
    return nullptr;
}

struct ObjectiveFacts
{
    StableId objectiveId = 0;
    ObjectiveKind kind = ObjectiveKind::Capture;
    bool available = true;
    bool friendlyFlagAtBase = true;
    bool enemyFlagAtBase = true;
    bool friendlyFlagDropped = false;
    bool enemyFlagDropped = false;
    Guid friendlyFlagCarrierGuid = 0;
    Guid enemyFlagCarrierGuid = 0;
    std::uint16_t urgencyPermille = 0;
    std::uint16_t threatPermille = 0;
    Guid previousOwnerGuid = 0;
    bool previousOwnerLeaseActive = false;
};

inline bool ObjectiveIsAvailable(ObjectiveFacts const& objective)
{
    if (objective.objectiveId == 0 || !objective.available)
        return false;

    switch (objective.kind)
    {
        case ObjectiveKind::Capture:
            return objective.friendlyFlagAtBase && objective.enemyFlagCarrierGuid == 0 &&
                (objective.enemyFlagAtBase || objective.enemyFlagDropped);
        case ObjectiveKind::Defend:
            return (objective.friendlyFlagAtBase || objective.friendlyFlagCarrierGuid != 0) &&
                (objective.threatPermille != 0 || objective.friendlyFlagCarrierGuid != 0);
        case ObjectiveKind::Escort:
            return objective.enemyFlagCarrierGuid != 0;
        case ObjectiveKind::Intercept:
            return objective.friendlyFlagCarrierGuid != 0;
        case ObjectiveKind::Recover:
            return objective.friendlyFlagDropped && objective.friendlyFlagCarrierGuid == 0;
    }
    return false;
}

inline bool RoleSupportsObjective(SquadRole role, ObjectiveKind kind)
{
    switch (kind)
    {
        case ObjectiveKind::Capture:
            return role == SquadRole::FlagCarrier || role == SquadRole::Escort ||
                role == SquadRole::Damage;
        case ObjectiveKind::Defend:
            return role == SquadRole::Defender || role == SquadRole::Tank ||
                role == SquadRole::Damage;
        case ObjectiveKind::Escort:
            return role == SquadRole::Escort || role == SquadRole::Healer ||
                role == SquadRole::Tank || role == SquadRole::Damage;
        case ObjectiveKind::Intercept:
            return role == SquadRole::Interceptor || role == SquadRole::Damage;
        case ObjectiveKind::Recover:
            return role == SquadRole::Recovery || role == SquadRole::Defender ||
                role == SquadRole::Damage;
    }
    return false;
}

inline constexpr Score RoleFitScore(SquadRole role, ObjectiveKind kind)
{
    switch (kind)
    {
        case ObjectiveKind::Capture:
            if (role == SquadRole::FlagCarrier)
                return 500000;
            if (role == SquadRole::Escort)
                return 300000;
            break;
        case ObjectiveKind::Defend:
            if (role == SquadRole::Defender)
                return 500000;
            if (role == SquadRole::Tank)
                return 400000;
            break;
        case ObjectiveKind::Escort:
            if (role == SquadRole::Escort)
                return 500000;
            if (role == SquadRole::Healer || role == SquadRole::Tank)
                return 350000;
            break;
        case ObjectiveKind::Intercept:
            if (role == SquadRole::Interceptor)
                return 500000;
            break;
        case ObjectiveKind::Recover:
            if (role == SquadRole::Recovery)
                return 500000;
            if (role == SquadRole::Defender)
                return 350000;
            break;
    }
    return role == SquadRole::Damage ? 100000 : 0;
}

inline Score ScoreObjectiveOwner(ObjectiveFacts const& objective, SquadMemberFacts const& member,
    SquadRole role)
{
    Score score = RoleFitScore(role, objective.kind);
    if (score == 0 || !IsActive(member) || !RoleSupportsObjective(role, objective.kind))
        return 0;

    score = AddScore(score, static_cast<Score>(ClampPermille(objective.urgencyPermille)) * 100);
    score = AddScore(score, static_cast<Score>(ClampPermille(objective.threatPermille)) * 50);
    score = AddScore(score, static_cast<Score>(ClampPermille(member.healthPermille)) * 10);
    score = AddScore(score, DistancePreference(member.distanceToObjectivePermille));
    if (member.underAttack)
        score = AddScore(score, -50000);
    return score;
}

inline constexpr Score ObjectivePriority(ObjectiveFacts const& objective)
{
    Score base = 0;
    switch (objective.kind)
    {
        case ObjectiveKind::Intercept:
            base = 500000;
            break;
        case ObjectiveKind::Recover:
            base = 490000;
            break;
        case ObjectiveKind::Defend:
            base = 480000;
            break;
        case ObjectiveKind::Escort:
            base = 470000;
            break;
        case ObjectiveKind::Capture:
            base = 460000;
            break;
    }
    base = AddScore(base, static_cast<Score>(ClampPermille(objective.urgencyPermille)) * 100);
    return AddScore(base, static_cast<Score>(ClampPermille(objective.threatPermille)) * 50);
}

inline constexpr IntentKind IntentForObjective(ObjectiveKind kind)
{
    switch (kind)
    {
        case ObjectiveKind::Capture:
            return IntentKind::CaptureObjective;
        case ObjectiveKind::Defend:
            return IntentKind::DefendObjective;
        case ObjectiveKind::Escort:
            return IntentKind::EscortCarrier;
        case ObjectiveKind::Intercept:
            return IntentKind::InterceptCarrier;
        case ObjectiveKind::Recover:
            return IntentKind::RecoverFlag;
    }
    return IntentKind::None;
}

struct ObjectiveOwnership
{
    bool claimed = false;
    ObjectiveKind kind = ObjectiveKind::Capture;
    StableId objectiveId = 0;
    Guid ownerGuid = 0;
    Score ownerScore = 0;
    Score priority = 0;
    bool retained = false;
    Reason reason = Reason::ObjectiveUnavailable;
};

inline ObjectiveOwnership OwnObjective(ObjectiveFacts const& objective, SquadFacts const& squad,
    SquadRolePlan const& roles)
{
    ObjectiveOwnership result;
    result.kind = objective.kind;
    result.objectiveId = objective.objectiveId;
    result.priority = ObjectivePriority(objective);

    if (!ObjectiveIsAvailable(objective))
        return result;
    if (!roles.valid)
    {
        result.reason = Reason::NoObjectiveOwner;
        return result;
    }

    if (objective.previousOwnerLeaseActive && objective.previousOwnerGuid != 0)
    {
        SquadRole const previousRole = RoleFor(roles, objective.previousOwnerGuid);
        SquadMemberFacts const* previous = FindMember(squad, objective.previousOwnerGuid);
        if (previous && IsActive(*previous) && RoleSupportsObjective(previousRole, objective.kind))
        {
            result.claimed = true;
            result.ownerGuid = objective.previousOwnerGuid;
            result.ownerScore = ScoreObjectiveOwner(objective, *previous, previousRole);
            result.retained = true;
            result.reason = Reason::RetainedOwner;
            return result;
        }
    }

    for (std::size_t index = 0; index < roles.assignmentCount; ++index)
    {
        RoleAssignment const& assignment = roles.assignments[index];
        SquadMemberFacts const* member = FindMember(squad, assignment.guid);
        if (!member)
            continue;
        Score const score = ScoreObjectiveOwner(objective, *member, assignment.role);
        if (score == 0)
            continue;
        if (!result.claimed || score > result.ownerScore ||
            (score == result.ownerScore && assignment.guid < result.ownerGuid))
        {
            result.claimed = true;
            result.ownerGuid = assignment.guid;
            result.ownerScore = score;
        }
    }

    result.reason = result.claimed ? Reason::Planned : Reason::NoObjectiveOwner;
    return result;
}

struct ObjectiveSelection
{
    bool hasSelection = false;
    ObjectiveOwnership ownership;
};

inline bool BetterObjective(ObjectiveOwnership const& candidate,
    ObjectiveOwnership const& current)
{
    if (!current.claimed)
        return true;
    if (candidate.priority != current.priority)
        return candidate.priority > current.priority;
    if (candidate.ownerScore != current.ownerScore)
        return candidate.ownerScore > current.ownerScore;
    if (candidate.kind != current.kind)
        return static_cast<std::uint8_t>(candidate.kind) < static_cast<std::uint8_t>(current.kind);
    if (candidate.objectiveId != current.objectiveId)
        return candidate.objectiveId < current.objectiveId;
    return candidate.ownerGuid < current.ownerGuid;
}

inline ObjectiveSelection SelectObjective(SquadFacts const& squad, SquadRolePlan const& roles,
    std::array<ObjectiveFacts, kMaxObjectives> const& objectives, std::size_t objectiveCount)
{
    ObjectiveSelection selection;
    if (objectiveCount > objectives.size())
    {
        selection.ownership.reason = Reason::InvalidFrame;
        return selection;
    }

    for (std::size_t index = 0; index < objectiveCount; ++index)
    {
        ObjectiveOwnership const candidate = OwnObjective(objectives[index], squad, roles);
        if (!candidate.claimed)
            continue;
        if (!selection.hasSelection || BetterObjective(candidate, selection.ownership))
        {
            selection.hasSelection = true;
            selection.ownership = candidate;
        }
    }
    return selection;
}

struct TargetFacts
{
    Guid guid = 0;
    bool alive = true;
    bool hostile = false;
    bool carryingOurFlag = false;
    bool attackingSelf = false;
    bool attackingFriendlyCarrier = false;
    bool attackingHealer = false;
    bool attackingTank = false;
    bool isHealer = false;
    bool isTank = false;
    bool interruptible = false;
    std::uint16_t threatPermille = 0;
    std::uint16_t healthPermille = kPermilleMax;
    std::uint16_t distancePermille = kPermilleMax;
};

struct TargetContext
{
    PlannerMode mode = PlannerMode::Unknown;
    bool selfDefenseEnabled = false;
};

struct TargetScore
{
    bool eligible = false;
    Score score = 0;
    Reason reason = Reason::InvalidTarget;
};

inline TargetScore ScoreTarget(TargetFacts const& target, TargetContext const& context)
{
    if (target.guid == 0)
        return {false, 0, Reason::InvalidTarget};
    if (!target.alive)
        return {false, 0, Reason::TargetDead};
    if (!target.hostile)
        return {false, 0, Reason::NonHostile};
    if (context.mode == PlannerMode::Unknown)
        return {false, 0, Reason::InvalidFrame};
    if (context.mode == PlannerMode::OpenWorld)
    {
        if (!context.selfDefenseEnabled)
            return {false, 0, Reason::SelfDefenseNotEnabled};
        if (!target.attackingSelf && !target.attackingFriendlyCarrier)
            return {false, 0, Reason::SelfDefenseNotTriggered};
    }

    Score score = 100000;
    if (target.carryingOurFlag)
        score = AddScore(score, 600000);
    if (target.attackingSelf)
        score = AddScore(score, 400000);
    if (target.attackingFriendlyCarrier)
        score = AddScore(score, 300000);
    if (target.attackingHealer)
        score = AddScore(score, 290000);
    if (target.attackingTank)
        score = AddScore(score, 280000);
    if (target.isHealer)
        score = AddScore(score, 160000);
    if (target.interruptible)
        score = AddScore(score, 50000);
    if (target.isTank)
        score = AddScore(score, 20000);
    score = AddScore(score, static_cast<Score>(ClampPermille(target.threatPermille)) * 50);
    score = AddScore(score, HealthDeficit(target.healthPermille) * 10);
    score = AddScore(score, DistancePreference(target.distancePermille));
    return {true, score, Reason::Planned};
}

struct TargetChoice
{
    bool hasTarget = false;
    Guid targetGuid = 0;
    IntentKind intent = IntentKind::None;
    Score score = 0;
    Reason reason = Reason::NoEligibleTarget;
};

inline constexpr IntentKind IntentForTarget(TargetFacts const& target, PlannerMode mode)
{
    if (mode == PlannerMode::OpenWorld)
        return target.attackingSelf ? IntentKind::AttackHostile : IntentKind::DefendAlly;
    if (target.carryingOurFlag || target.attackingFriendlyCarrier)
        return IntentKind::InterceptCarrier;
    return IntentKind::AttackHostile;
}

inline TargetChoice ChooseTarget(std::array<TargetFacts, kMaxTargets> const& targets,
    std::size_t targetCount, TargetContext const& context)
{
    TargetChoice result;
    if (targetCount > targets.size())
    {
        result.reason = Reason::InvalidFrame;
        return result;
    }

    for (std::size_t index = 0; index < targetCount; ++index)
    {
        TargetFacts const& target = targets[index];
        TargetScore const scored = ScoreTarget(target, context);
        if (!scored.eligible)
            continue;
        if (!result.hasTarget || scored.score > result.score ||
            (scored.score == result.score && target.guid < result.targetGuid))
        {
            result.hasTarget = true;
            result.targetGuid = target.guid;
            result.intent = IntentForTarget(target, context.mode);
            result.score = scored.score;
            result.reason = Reason::Planned;
        }
    }
    return result;
}

struct ProtectionChoice
{
    bool hasChoice = false;
    IntentKind intent = IntentKind::None;
    Guid targetGuid = 0;
    Score score = 0;
    Reason reason = Reason::NoProtectionNeeded;
};

inline ProtectionChoice ChooseProtection(SquadFacts const& squad, SquadRolePlan const& roles)
{
    ProtectionChoice result;
    if (!roles.valid)
        return result;

    for (std::size_t index = 0; index < roles.assignmentCount; ++index)
    {
        RoleAssignment const& assignment = roles.assignments[index];
        if (assignment.role != SquadRole::Healer && assignment.role != SquadRole::Tank)
            continue;
        SquadMemberFacts const* member = FindMember(squad, assignment.guid);
        if (!member || !IsActive(*member) ||
            (!member->underAttack && member->healthPermille >= kProtectionHealthPermille))
            continue;

        Score score = member->underAttack ? 300000 : 100000;
        score = AddScore(score, HealthDeficit(member->healthPermille) * 100);
        if (assignment.role == SquadRole::Healer)
            score = AddScore(score, 10000);
        if (member->healthPermille <= kCriticalHealthPermille)
            score = AddScore(score, 10000);
        if (!result.hasChoice || score > result.score ||
            (score == result.score && assignment.guid < result.targetGuid))
        {
            result.hasChoice = true;
            result.intent = assignment.role == SquadRole::Healer ? IntentKind::ProtectHealer :
                IntentKind::ProtectTank;
            result.targetGuid = assignment.guid;
            result.score = score;
            result.reason = Reason::Planned;
        }
    }
    return result;
}

struct RecoveryFacts
{
    Guid selfGuid = 0;
    bool selfAlive = true;
    bool corpseKnown = false;
    bool deadMemberPresent = false;
    Guid deadMemberGuid = 0;
    bool healerCanRecover = false;
    bool regroupNeeded = false;
    StableId rallyPointId = 0;
    bool atRallyPoint = false;
    std::uint8_t recoveryAttempts = 0;
    std::uint8_t regroupNoProgress = 0;
};

struct RecoveryDecision
{
    IntentKind intent = IntentKind::None;
    Guid targetGuid = 0;
    StableId rallyPointId = 0;
    Score score = 0;
    Reason reason = Reason::RecoveryNotNeeded;
};

inline RecoveryDecision EvaluateRecovery(RecoveryFacts const& facts)
{
    if (!facts.selfAlive)
    {
        if (facts.selfGuid == 0 || !facts.corpseKnown)
            return {IntentKind::None, facts.selfGuid, 0, 0, Reason::RecoveryFactsMissing};
        if (facts.recoveryAttempts >= kMaxRecoveryAttempts)
            return {IntentKind::None, facts.selfGuid, 0, 0, Reason::RecoveryBudgetExhausted};
        return {IntentKind::RecoverDeath, facts.selfGuid, 0, 800000, Reason::Planned};
    }

    if (facts.deadMemberPresent)
    {
        if (facts.deadMemberGuid == 0)
            return {IntentKind::None, 0, 0, 0, Reason::RecoveryFactsMissing};
        if (!facts.healerCanRecover)
            return {IntentKind::None, facts.deadMemberGuid, 0, 0, Reason::DeadMemberNoHealer};
        if (facts.recoveryAttempts >= kMaxRecoveryAttempts)
            return {IntentKind::None, facts.deadMemberGuid, 0, 0, Reason::RecoveryBudgetExhausted};
        return {IntentKind::RecoverDeath, facts.deadMemberGuid, 0, 760000, Reason::Planned};
    }

    if (!facts.regroupNeeded || facts.atRallyPoint || facts.rallyPointId == 0)
        return {IntentKind::None, 0, facts.rallyPointId, 0, Reason::RegroupNotNeeded};
    if (facts.regroupNoProgress >= kMaxRegroupNoProgress)
        return {IntentKind::None, 0, facts.rallyPointId, 0, Reason::RegroupBudgetExhausted};
    return {IntentKind::Regroup, 0, facts.rallyPointId, 700000, Reason::Planned};
}

struct CampaignChallengeFacts
{
    bool challengeObserved = false;
    bool isCampaignChallenge = false;
    Guid challengerGuid = 0;
    bool acceptAllowed = false;
    bool safeToAccept = false;
    bool campaignBusy = false;
};

struct CampaignDecision
{
    IntentKind intent = IntentKind::None;
    Guid challengerGuid = 0;
    Score score = 0;
    Reason reason = Reason::ChallengeNotObserved;
};

inline CampaignDecision EvaluateCampaignChallenge(CampaignChallengeFacts const& facts)
{
    if (!facts.challengeObserved || !facts.isCampaignChallenge)
        return {};
    if (facts.challengerGuid == 0)
        return {IntentKind::None, 0, 0, Reason::InvalidChallengeFact};
    if (facts.acceptAllowed && facts.safeToAccept && !facts.campaignBusy)
        return {IntentKind::AcceptCampaignChallenge, facts.challengerGuid, 200000,
            Reason::ChallengeAccepted};
    return {IntentKind::DeclineCampaignChallenge, facts.challengerGuid, 200000,
        Reason::ChallengeDeclined};
}

struct OpenWorldFacts
{
    bool inOpenWorld = false;
    bool selfDefenseEnabled = false;
    Guid selfGuid = 0;
    bool selfAlive = true;
    bool hostileActionObserved = false;
    bool selfUnderAttack = false;
    bool allyUnderAttack = false;
    Guid aggressorGuid = 0;
    bool aggressorHostile = false;
    bool criticalHealth = false;
    bool outnumbered = false;
    bool disengageAvailable = false;
};

struct OpenWorldDecision
{
    IntentKind intent = IntentKind::None;
    Guid targetGuid = 0;
    Score score = 0;
    Reason reason = Reason::SelfDefenseNotTriggered;
};

inline OpenWorldDecision EvaluateOpenWorldSelfDefense(OpenWorldFacts const& facts)
{
    if (!facts.inOpenWorld)
        return {};
    if (!facts.selfDefenseEnabled)
        return {IntentKind::None, facts.aggressorGuid, 0, Reason::SelfDefenseNotEnabled};
    if (!facts.selfAlive || facts.selfGuid == 0 || facts.aggressorGuid == 0 ||
        !facts.aggressorHostile || !facts.hostileActionObserved ||
        (!facts.selfUnderAttack && !facts.allyUnderAttack))
        return {IntentKind::None, facts.aggressorGuid, 0, Reason::SelfDefenseNotTriggered};
    if (facts.selfUnderAttack && facts.criticalHealth && facts.outnumbered &&
        facts.disengageAvailable)
        return {IntentKind::Disengage, facts.aggressorGuid, 650000, Reason::Planned};
    if (facts.selfUnderAttack)
        return {IntentKind::AttackHostile, facts.aggressorGuid, 600000, Reason::Planned};
    return {IntentKind::DefendAlly, facts.aggressorGuid, 580000, Reason::Planned};
}

struct PlannerFrame
{
    Guid selfGuid = 0;
    Tick decisionKey = 0;
    PlannerMode mode = PlannerMode::Unknown;
    SquadFacts squad;
    std::array<ObjectiveFacts, kMaxObjectives> objectives{};
    std::size_t objectiveCount = 0;
    std::array<TargetFacts, kMaxTargets> targets{};
    std::size_t targetCount = 0;
    RecoveryFacts recovery;
    CampaignChallengeFacts campaign;
    OpenWorldFacts openWorld;
};

enum class PlanStatus : std::uint8_t
{
    Planned,
    Blocked,
    Invalid
};

struct PlanIntent
{
    bool valid = false;
    IntentKind intent = IntentKind::None;
    PlannerMode mode = PlannerMode::Unknown;
    Guid targetGuid = 0;
    StableId objectiveId = 0;
    Guid ownerGuid = 0;
    SquadRole role = SquadRole::Unassigned;
    Score score = 0;
    Score priority = 0;
    std::uint32_t ttlTicks = 0;
    Reason reason = Reason::None;
};

struct PlanResult
{
    PlanStatus status = PlanStatus::Blocked;
    PlanIntent intent;
};

inline bool PlannerFrameWithinBounds(PlannerFrame const& frame)
{
    if (frame.selfGuid == 0 || frame.mode == PlannerMode::Unknown ||
        !SquadFrameWithinBounds(frame.squad) || frame.objectiveCount > frame.objectives.size() ||
        frame.targetCount > frame.targets.size())
        return false;
    if (frame.recovery.selfGuid != frame.selfGuid ||
        frame.recovery.recoveryAttempts > kMaxRecoveryAttempts ||
        frame.recovery.regroupNoProgress > kMaxRegroupNoProgress)
        return false;
    if (frame.mode == PlannerMode::OpenWorld &&
        (frame.openWorld.selfGuid != frame.selfGuid || !frame.openWorld.inOpenWorld))
        return false;

    for (std::size_t index = 0; index < frame.objectiveCount; ++index)
    {
        if (frame.objectives[index].objectiveId == 0)
            return false;
        for (std::size_t other = 0; other < index; ++other)
            if (frame.objectives[other].objectiveId == frame.objectives[index].objectiveId)
                return false;
    }
    for (std::size_t index = 0; index < frame.targetCount; ++index)
    {
        if (frame.targets[index].guid == 0)
            return false;
        for (std::size_t other = 0; other < index; ++other)
            if (frame.targets[other].guid == frame.targets[index].guid)
                return false;
    }
    return true;
}

inline PlanResult MakePlannedIntent(IntentKind intent, PlannerMode mode, Guid targetGuid,
    StableId objectiveId, Guid ownerGuid, SquadRole role, Score score, Score priority,
    Reason reason = Reason::Planned)
{
    PlanResult result;
    result.status = PlanStatus::Planned;
    result.intent = {true, intent, mode, targetGuid, objectiveId, ownerGuid, role, score, priority,
        kIntentTtlTicks, reason};
    return result;
}

inline PlanResult MakeBlocked(Reason reason)
{
    PlanResult result;
    result.status = PlanStatus::Blocked;
    result.intent.reason = reason;
    return result;
}

inline PlanResult Plan(PlannerFrame const& frame)
{
    if (!PlannerFrameWithinBounds(frame))
    {
        PlanResult result;
        result.status = PlanStatus::Invalid;
        result.intent.reason = Reason::InvalidFrame;
        return result;
    }

    RecoveryDecision const recovery = EvaluateRecovery(frame.recovery);
    if (recovery.intent != IntentKind::None)
        return MakePlannedIntent(recovery.intent, frame.mode, recovery.targetGuid,
            recovery.rallyPointId, 0, SquadRole::Recovery, recovery.score, recovery.score);
    if (!frame.recovery.selfAlive)
        return MakeBlocked(recovery.reason);

    if (frame.mode == PlannerMode::OpenWorld)
    {
        OpenWorldDecision const selfDefense = EvaluateOpenWorldSelfDefense(frame.openWorld);
        if (selfDefense.intent != IntentKind::None)
            return MakePlannedIntent(selfDefense.intent, frame.mode, selfDefense.targetGuid, 0, 0,
                SquadRole::Damage, selfDefense.score, selfDefense.score);
    }

    CampaignDecision const campaign = EvaluateCampaignChallenge(frame.campaign);
    if (frame.mode == PlannerMode::Campaign && campaign.intent != IntentKind::None)
        return MakePlannedIntent(campaign.intent, frame.mode, campaign.challengerGuid, 0, 0,
            SquadRole::Captain, campaign.score, campaign.score, campaign.reason);

    SquadRolePlan const roles = BuildSquadRolePlan(frame.squad);
    ProtectionChoice const protection = ChooseProtection(frame.squad, roles);
    if (protection.hasChoice)
        return MakePlannedIntent(protection.intent, frame.mode, protection.targetGuid, 0, 0,
            RoleFor(roles, protection.targetGuid), protection.score, protection.score);

    if (frame.mode == PlannerMode::Battleground)
    {
        ObjectiveSelection const objective = SelectObjective(frame.squad, roles, frame.objectives,
            frame.objectiveCount);
        if (objective.hasSelection)
        {
            ObjectiveOwnership const& ownership = objective.ownership;
            return MakePlannedIntent(IntentForObjective(ownership.kind), frame.mode,
                ownership.ownerGuid, ownership.objectiveId, ownership.ownerGuid,
                RoleFor(roles, ownership.ownerGuid), ownership.ownerScore, ownership.priority,
                ownership.reason);
        }
    }

    TargetContext const targetContext = {frame.mode,
        frame.mode == PlannerMode::OpenWorld ? frame.openWorld.selfDefenseEnabled : false};
    TargetChoice const target = ChooseTarget(frame.targets, frame.targetCount, targetContext);
    if (target.hasTarget)
        return MakePlannedIntent(target.intent, frame.mode, target.targetGuid, 0, 0,
            SquadRole::Damage, target.score, target.score);

    if (frame.mode == PlannerMode::Campaign && campaign.intent != IntentKind::None)
        return MakePlannedIntent(campaign.intent, frame.mode, campaign.challengerGuid, 0, 0,
            SquadRole::Captain, campaign.score, campaign.score, campaign.reason);

    return MakeBlocked(target.reason);
}
} // namespace AutoWowOraclePvpOpenWorld

#endif // MOD_PLAYERBOTS_AUTOWOW_ORACLE_PVP_OPEN_WORLD_POLICY_H
