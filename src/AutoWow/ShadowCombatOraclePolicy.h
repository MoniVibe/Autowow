/*
 * Header-only shadow combat oracle and experience projection.
 *
 * This contract consumes plain facts and produces plain data. It has no live Playerbot integration,
 * mutation, logging, clock, random source, configuration, or I/O dependency.
 */
#ifndef MOD_PLAYERBOTS_SHADOW_COMBAT_ORACLE_POLICY_H
#define MOD_PLAYERBOTS_SHADOW_COMBAT_ORACLE_POLICY_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace AutoWowShadowCombat
{
using Guid = std::uint64_t;
using Score = std::int32_t;

inline constexpr std::uint16_t kHealthScale = 1000;
inline constexpr std::uint16_t kCriticalHealthPermille = 300;
inline constexpr std::uint16_t kInjuredHealthPermille = 850;

enum class Intent : std::uint8_t
{
    Survive,
    AvoidHazard,
    Heal,
    Interrupt,
    ControlAdd,
    ProtectTank,
    Attack
};

inline constexpr std::string_view IntentName(Intent intent)
{
    switch (intent)
    {
        case Intent::Survive:
            return "survive";
        case Intent::AvoidHazard:
            return "avoid_hazard";
        case Intent::Heal:
            return "heal";
        case Intent::Interrupt:
            return "interrupt";
        case Intent::ControlAdd:
            return "control_add";
        case Intent::ProtectTank:
            return "protect_tank";
        case Intent::Attack:
            return "attack";
    }
    return "attack";
}

struct SelfFacts
{
    Guid guid = 0;
    std::uint16_t healthPermille = kHealthScale;
    bool underDirectThreat = false;
};

struct AllyFacts
{
    Guid guid = 0;
    std::uint16_t healthPermille = kHealthScale;
    bool tank = false;
    bool underAttack = false;
};

struct EnemyFacts
{
    Guid guid = 0;
    std::uint16_t castDangerPermille = 0;
    std::uint16_t threatPermille = 0;
    bool alive = true;
    bool castingInterruptible = false;
    bool add = false;
    bool controlRecommended = false;
};

struct HazardFacts
{
    Guid stableId = 0;
    std::uint16_t dangerPermille = 0;
    bool active = false;
    bool affectsSelf = false;
};

struct ActionCandidate
{
    Intent intent = Intent::Attack;
    Guid targetGuid = 0;
    std::string_view actionName;
    bool available = true;
};

struct DecisionFrame
{
    std::uint64_t decisionKey = 0;
    SelfFacts self;
    std::vector<AllyFacts> allies;
    std::vector<EnemyFacts> enemies;
    std::vector<HazardFacts> hazards;
    std::vector<ActionCandidate> candidates;
};

struct OracleChoice
{
    bool hasChoice = false;
    Intent intent = Intent::Attack;
    Guid targetGuid = 0;
    std::string_view actionName;
    Score score = 0;
};

namespace Detail
{
inline constexpr std::uint16_t BoundedPermille(std::uint16_t value)
{
    return value > kHealthScale ? kHealthScale : value;
}

inline constexpr Score HealthDeficit(std::uint16_t healthPermille)
{
    return static_cast<Score>(kHealthScale - BoundedPermille(healthPermille));
}

inline constexpr Score BoundedSignal(std::uint16_t value)
{
    return static_cast<Score>(BoundedPermille(value));
}

inline Score ScoreCandidate(DecisionFrame const& frame, ActionCandidate const& candidate)
{
    if (!candidate.available || candidate.actionName.empty())
        return 0;

    switch (candidate.intent)
    {
        case Intent::Survive:
            if ((candidate.targetGuid == 0 || candidate.targetGuid == frame.self.guid) &&
                (frame.self.healthPermille <= kCriticalHealthPermille || frame.self.underDirectThreat))
                return 700000 + HealthDeficit(frame.self.healthPermille) +
                    (frame.self.underDirectThreat ? 1000 : 0);
            break;
        case Intent::AvoidHazard:
        {
            Score best = 0;
            for (HazardFacts const& hazard : frame.hazards)
                if (hazard.stableId == candidate.targetGuid && hazard.active && hazard.affectsSelf)
                    best = std::max(best, 650000 + BoundedSignal(hazard.dangerPermille));
            return best;
        }
        case Intent::Heal:
        {
            Score best = 0;
            for (AllyFacts const& ally : frame.allies)
            {
                if (ally.guid != candidate.targetGuid || ally.healthPermille >= kInjuredHealthPermille)
                    continue;
                Score const base = ally.healthPermille <= kCriticalHealthPermille ? 600000 : 300000;
                best = std::max(best, base + HealthDeficit(ally.healthPermille) + (ally.tank ? 1000 : 0));
            }
            return best;
        }
        case Intent::Interrupt:
        {
            Score best = 0;
            for (EnemyFacts const& enemy : frame.enemies)
                if (enemy.guid == candidate.targetGuid && enemy.alive && enemy.castingInterruptible)
                    best = std::max(best, 550000 + BoundedSignal(enemy.castDangerPermille));
            return best;
        }
        case Intent::ControlAdd:
        {
            Score best = 0;
            for (EnemyFacts const& enemy : frame.enemies)
                if (enemy.guid == candidate.targetGuid && enemy.alive && enemy.add && enemy.controlRecommended)
                    best = std::max(best, 400000 + BoundedSignal(enemy.threatPermille));
            return best;
        }
        case Intent::ProtectTank:
        {
            Score best = 0;
            for (AllyFacts const& ally : frame.allies)
                if (ally.guid == candidate.targetGuid && ally.tank && ally.underAttack)
                    best = std::max(best, 450000 + HealthDeficit(ally.healthPermille));
            return best;
        }
        case Intent::Attack:
        {
            Score best = 0;
            for (EnemyFacts const& enemy : frame.enemies)
                if (enemy.guid == candidate.targetGuid && enemy.alive)
                    best = std::max(best, 100000 + BoundedSignal(enemy.threatPermille));
            return best;
        }
    }
    return 0;
}

inline bool BetterCandidate(Score score, ActionCandidate const& candidate, OracleChoice const& best)
{
    if (!best.hasChoice || score != best.score)
        return !best.hasChoice || score > best.score;
    if (candidate.targetGuid != best.targetGuid)
        return candidate.targetGuid < best.targetGuid;
    if (candidate.actionName != best.actionName)
        return candidate.actionName < best.actionName;
    return static_cast<std::uint8_t>(candidate.intent) < static_cast<std::uint8_t>(best.intent);
}

inline constexpr std::uint64_t kKnowledgeAllyDomain = 0x4b4e4f57414c4c59ULL;
inline constexpr std::uint64_t kKnowledgeEnemyDomain = 0x4b4e4f57454e4d59ULL;
inline constexpr std::uint64_t kKnowledgeHazardDomain = 0x4b4e4f5748415a44ULL;
inline constexpr std::uint64_t kLatencyDomain = 0x4c4154454e435931ULL;
inline constexpr std::uint64_t kErrorRollDomain = 0x4552524f52524f4cULL;
inline constexpr std::uint64_t kErrorChoiceDomain = 0x4552524f5243484fULL;

inline bool AllyLess(AllyFacts const& left, AllyFacts const& right)
{
    if (left.guid != right.guid)
        return left.guid < right.guid;
    if (left.healthPermille != right.healthPermille)
        return left.healthPermille < right.healthPermille;
    if (left.tank != right.tank)
        return left.tank < right.tank;
    return left.underAttack < right.underAttack;
}

inline bool EnemyLess(EnemyFacts const& left, EnemyFacts const& right)
{
    if (left.guid != right.guid)
        return left.guid < right.guid;
    if (left.castDangerPermille != right.castDangerPermille)
        return left.castDangerPermille < right.castDangerPermille;
    if (left.threatPermille != right.threatPermille)
        return left.threatPermille < right.threatPermille;
    if (left.alive != right.alive)
        return left.alive < right.alive;
    if (left.castingInterruptible != right.castingInterruptible)
        return left.castingInterruptible < right.castingInterruptible;
    if (left.add != right.add)
        return left.add < right.add;
    return left.controlRecommended < right.controlRecommended;
}

inline bool HazardLess(HazardFacts const& left, HazardFacts const& right)
{
    if (left.stableId != right.stableId)
        return left.stableId < right.stableId;
    if (left.dangerPermille != right.dangerPermille)
        return left.dangerPermille < right.dangerPermille;
    if (left.active != right.active)
        return left.active < right.active;
    return left.affectsSelf < right.affectsSelf;
}

inline bool CandidateLess(ActionCandidate const& left, ActionCandidate const& right)
{
    if (left.targetGuid != right.targetGuid)
        return left.targetGuid < right.targetGuid;
    if (left.actionName != right.actionName)
        return left.actionName < right.actionName;
    if (left.intent != right.intent)
        return static_cast<std::uint8_t>(left.intent) < static_cast<std::uint8_t>(right.intent);
    return left.available < right.available;
}
}

inline OracleChoice ChooseOracle(DecisionFrame const& frame)
{
    OracleChoice best;
    for (ActionCandidate const& candidate : frame.candidates)
    {
        Score const score = Detail::ScoreCandidate(frame, candidate);
        if (score <= 0 || !Detail::BetterCandidate(score, candidate, best))
            continue;
        best = {true, candidate.intent, candidate.targetGuid, candidate.actionName, score};
    }
    return best;
}

enum class ComparisonGrade : std::uint8_t
{
    Exact,
    ActionOnly,
    IntentOnly,
    TargetMismatch,
    NoActual
};

inline constexpr std::string_view ComparisonGradeName(ComparisonGrade grade)
{
    switch (grade)
    {
        case ComparisonGrade::Exact:
            return "exact";
        case ComparisonGrade::ActionOnly:
            return "action_only";
        case ComparisonGrade::IntentOnly:
            return "intent_only";
        case ComparisonGrade::TargetMismatch:
            return "target_mismatch";
        case ComparisonGrade::NoActual:
            return "no_actual";
    }
    return "no_actual";
}

struct ActualChoice
{
    bool hasChoice = false;
    Intent intent = Intent::Attack;
    Guid targetGuid = 0;
    std::string_view actionName;
};

// The grades deliberately stop at coarse intent agreement. A present observation with no action or
// intent agreement is not a comparable actual and therefore receives NoActual.
inline ComparisonGrade Compare(OracleChoice const& oracle, ActualChoice const& actual)
{
    if (!actual.hasChoice)
        return ComparisonGrade::NoActual;

    bool const sameAction = oracle.actionName == actual.actionName;
    bool const sameIntent = oracle.intent == actual.intent;
    bool const sameTarget = oracle.targetGuid == actual.targetGuid;
    if (sameAction && sameIntent && sameTarget)
        return ComparisonGrade::Exact;
    if (sameAction && sameIntent)
        return ComparisonGrade::TargetMismatch;
    if (sameAction)
        return ComparisonGrade::ActionOnly;
    if (sameIntent)
        return ComparisonGrade::IntentOnly;
    return ComparisonGrade::NoActual;
}

enum class ExperienceTier : std::uint8_t
{
    Novice,
    Experienced,
    Veteran,
    Oracle
};

struct ExperienceProfile
{
    std::uint32_t minLatencyMs = 0;
    std::uint32_t maxLatencyMs = 0;
    std::uint16_t errorPermille = 0;
    std::uint16_t knowledgePerTenThousand = 0;
};

inline constexpr std::array<ExperienceProfile, 4> kExperienceProfiles = {{
    {900, 1600, 200, 6000},
    {350, 800, 80, 8000},
    {100, 300, 20, 9500},
    {0, 0, 0, 10000},
}};

inline constexpr ExperienceProfile ProfileFor(ExperienceTier tier)
{
    return kExperienceProfiles[static_cast<std::size_t>(tier)];
}

// SplitMix64 finalizer with explicit unsigned overflow gives the same result on every conforming
// platform. Domains keep knowledge, latency, and error draws independent for one seed/frame.
inline constexpr std::uint64_t DeterministicHash(
    std::uint64_t seed, std::uint64_t domain, std::uint64_t stableKey)
{
    std::uint64_t value = seed ^ domain ^ (stableKey + 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

inline constexpr bool KnowledgeAllowsFact(std::uint16_t knowledgePerTenThousand, std::uint64_t seed,
    std::uint64_t decisionKey, std::uint64_t domain, std::uint64_t stableId)
{
    if (knowledgePerTenThousand >= 10000)
        return true;
    if (knowledgePerTenThousand == 0)
        return false;
    std::uint64_t const key = DeterministicHash(decisionKey, domain, stableId);
    return DeterministicHash(seed, domain, key) % 10000ULL < knowledgePerTenThousand;
}

inline DecisionFrame ApplyKnowledgeMask(
    DecisionFrame const& frame, std::uint16_t knowledgePerTenThousand, std::uint64_t seed)
{
    DecisionFrame masked;
    masked.decisionKey = frame.decisionKey;
    masked.self = frame.self;
    masked.candidates = frame.candidates;
    masked.allies.reserve(frame.allies.size());
    masked.enemies.reserve(frame.enemies.size());
    masked.hazards.reserve(frame.hazards.size());

    for (AllyFacts const& ally : frame.allies)
        if (KnowledgeAllowsFact(knowledgePerTenThousand, seed, frame.decisionKey,
                Detail::kKnowledgeAllyDomain, ally.guid))
            masked.allies.push_back(ally);
    for (EnemyFacts const& enemy : frame.enemies)
        if (KnowledgeAllowsFact(knowledgePerTenThousand, seed, frame.decisionKey,
                Detail::kKnowledgeEnemyDomain, enemy.guid))
            masked.enemies.push_back(enemy);
    for (HazardFacts const& hazard : frame.hazards)
        if (KnowledgeAllowsFact(knowledgePerTenThousand, seed, frame.decisionKey,
                Detail::kKnowledgeHazardDomain, hazard.stableId))
            masked.hazards.push_back(hazard);

    std::sort(masked.allies.begin(), masked.allies.end(), Detail::AllyLess);
    std::sort(masked.enemies.begin(), masked.enemies.end(), Detail::EnemyLess);
    std::sort(masked.hazards.begin(), masked.hazards.end(), Detail::HazardLess);
    std::sort(masked.candidates.begin(), masked.candidates.end(), Detail::CandidateLess);
    return masked;
}

inline constexpr std::uint32_t SelectLatencyMs(
    ExperienceProfile const& profile, std::uint64_t seed, std::uint64_t decisionKey)
{
    if (profile.maxLatencyMs <= profile.minLatencyMs)
        return profile.minLatencyMs;
    std::uint64_t const width =
        static_cast<std::uint64_t>(profile.maxLatencyMs - profile.minLatencyMs) + 1ULL;
    return profile.minLatencyMs + static_cast<std::uint32_t>(
        DeterministicHash(seed, Detail::kLatencyDomain, decisionKey) % width);
}

inline constexpr bool ShouldInjectError(
    ExperienceProfile const& profile, std::uint64_t seed, std::uint64_t decisionKey)
{
    if (profile.errorPermille == 0)
        return false;
    if (profile.errorPermille >= 1000)
        return true;
    return DeterministicHash(seed, Detail::kErrorRollDomain, decisionKey) % 1000ULL <
        profile.errorPermille;
}

inline bool SameChoice(OracleChoice const& left, OracleChoice const& right)
{
    return left.hasChoice == right.hasChoice && left.intent == right.intent &&
        left.targetGuid == right.targetGuid && left.actionName == right.actionName;
}

inline OracleChoice InjectDeterministicError(OracleChoice const& preferred,
    std::vector<ActionCandidate> const& candidates, std::uint64_t seed, std::uint64_t decisionKey)
{
    if (!preferred.hasChoice)
        return preferred;

    std::vector<ActionCandidate const*> alternatives;
    alternatives.reserve(candidates.size());
    for (ActionCandidate const& candidate : candidates)
    {
        if (!candidate.available || candidate.actionName.empty())
            continue;
        OracleChoice const choice = {true, candidate.intent, candidate.targetGuid, candidate.actionName, 0};
        if (!SameChoice(preferred, choice))
            alternatives.push_back(&candidate);
    }
    if (alternatives.empty())
        return preferred;

    std::sort(alternatives.begin(), alternatives.end(), [](ActionCandidate const* left, ActionCandidate const* right)
    {
        if (left->targetGuid != right->targetGuid)
            return left->targetGuid < right->targetGuid;
        if (left->actionName != right->actionName)
            return left->actionName < right->actionName;
        return static_cast<std::uint8_t>(left->intent) < static_cast<std::uint8_t>(right->intent);
    });

    std::size_t const index = static_cast<std::size_t>(
        DeterministicHash(seed, Detail::kErrorChoiceDomain, decisionKey) % alternatives.size());
    ActionCandidate const& selected = *alternatives[index];
    return {true, selected.intent, selected.targetGuid, selected.actionName, 0};
}

struct ExperienceProjection
{
    OracleChoice choice;
    std::uint32_t latencyMs = 0;
    bool errorInjected = false;
};

inline ExperienceProjection ProjectExperience(
    DecisionFrame const& frame, ExperienceTier tier, std::uint64_t seed)
{
    ExperienceProfile const profile = ProfileFor(tier);
    DecisionFrame const knownFrame = ApplyKnowledgeMask(frame, profile.knowledgePerTenThousand, seed);
    OracleChoice const preferred = ChooseOracle(knownFrame);

    ExperienceProjection projection;
    projection.choice = preferred;
    projection.latencyMs = SelectLatencyMs(profile, seed, frame.decisionKey);
    if (ShouldInjectError(profile, seed, frame.decisionKey))
    {
        projection.choice = InjectDeterministicError(preferred, knownFrame.candidates, seed, frame.decisionKey);
        projection.errorInjected = !SameChoice(projection.choice, preferred);
    }
    return projection;
}
} // namespace AutoWowShadowCombat

#endif
