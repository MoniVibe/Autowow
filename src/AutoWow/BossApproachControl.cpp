/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the
 * License.
 */

#include "BossApproachControl.h"
#include "BossApproachAggroPolicy.h"

#include "AiObjectContext.h"
#include "CellImpl.h"
#include "Creature.h"
#include "DungeonPathSafety.h"
#include "EncounterTelemetry.h"
#include "ExactBossTargetControl.h"
#include "Event.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "ThreatManager.h"
#include "TemporarySummon.h"
#include "TravelMgr.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AutoWowBossApproach
{
namespace
{
namespace Policy = AutoWowBossApproachPolicy;

constexpr float kBossStandOff = 8.0f;
constexpr float kPullStagingDistance = 55.0f;
constexpr float kProgressEpsilon = 0.25f;
constexpr float kEncounterRadius = AutoWowEncounterTelemetry::kEncounterRadius;
constexpr float kHazardScanRadius = kEncounterRadius + 50.0f;
constexpr std::size_t kMaxRosterMembers = AutoWowEncounterTelemetry::kMaxMembers;

struct RuntimeMember
{
    std::uint32_t guid = 0;
    Player* player = nullptr;
    PlayerbotAI* ai = nullptr;
    Policy::MemberFacts role;
    bool sameContext = false;
    bool transientFree = false;
    bool hadTravel = false;
    bool hadFollow = false;
};

class SlotDestination final : public TravelDestination
{
public:
    explicit SlotDestination(std::uint32_t memberGuid) : TravelDestination(0.0f, 0.0f), guid(memberGuid) {}

    bool isActive([[maybe_unused]] Player* bot) override { return true; }
    std::string const getName() override { return "AutoWowBossApproachDestination"; }
    std::string const getTitle() override { return "bounded boss approach slot " + std::to_string(guid); }

private:
    std::uint32_t guid = 0;
};

struct MemberPlan
{
    explicit MemberPlan(std::uint32_t memberGuid) : guid(memberGuid), destination(memberGuid)
    {
        destination.addPoint(&point);
    }

    std::uint32_t guid = 0;
    WorldPosition point;
    SlotDestination destination;
    bool initialized = false;
    bool hadTravel = false;
    bool hadFollow = false;
};

struct Session
{
    std::uint32_t leaderGuid = 0;
    ObjectGuid targetGuid;
    std::uint32_t targetEntry = 0;
    bool pullAuthorized = false;
    bool engaged = false;
    bool movementReleased = false;
    bool hasPreviousDistance = false;
    float previousMaxSlotDistance = std::numeric_limits<float>::infinity();
    std::uint32_t stalledPolls = 0;
    std::vector<std::uint32_t> roster;
    std::map<std::uint32_t, MemberPlan> plans;
    Result last;
};

std::unordered_map<std::uint32_t, std::unique_ptr<Session>> sessions;

char const* JsonBool(bool value)
{
    return value ? "true" : "false";
}

std::string JsonString(std::string const& value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char character : value)
    {
        switch (character)
        {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (character < 0x20)
                {
                    static char const hex[] = "0123456789abcdef";
                    out << "\\u00" << hex[(character >> 4) & 0x0f] << hex[character & 0x0f];
                }
                else
                    out << static_cast<char>(character);
        }
    }
    out << '"';
    return out.str();
}

void AppendNumber(std::ostringstream& out, float value)
{
    if (std::isfinite(value))
        out << value;
    else
        out << "null";
}

bool IsTransient(Player const* member)
{
    return !member || member->IsBeingTeleported() || member->IsInFlight() || member->IsFlying() ||
        member->HasUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
}

std::uint32_t GuidCounter(ObjectGuid const& guid)
{
    return static_cast<std::uint32_t>(guid.GetCounter());
}

std::vector<RuntimeMember> ResolveRoster(Player* leader, Group* group)
{
    std::vector<RuntimeMember> members;
    if (!leader || !group)
        return members;

    for (Group::MemberSlot const& slot : group->GetMemberSlots())
    {
        RuntimeMember runtime;
        runtime.guid = GuidCounter(slot.guid);
        runtime.role.guid = runtime.guid;
        runtime.player = ObjectAccessor::FindPlayer(slot.guid);
        if (runtime.player)
        {
            runtime.ai = PlayerbotsMgr::instance().GetPlayerbotAI(runtime.player);
            runtime.role.alive = runtime.player->IsAlive();
            runtime.role.tank = PlayerbotAI::IsTank(runtime.player);
            runtime.role.mainTank = PlayerbotAI::IsMainTank(runtime.player) ||
                PlayerbotAI::IsExplicitMainTank(runtime.player);
            runtime.role.healer = PlayerbotAI::IsHeal(runtime.player);
            runtime.role.ranged = PlayerbotAI::IsRanged(runtime.player);
            runtime.sameContext = runtime.player->IsInWorld() &&
                runtime.player->GetMapId() == leader->GetMapId() &&
                runtime.player->GetInstanceId() == leader->GetInstanceId();
            runtime.transientFree = runtime.sameContext && !IsTransient(runtime.player);
            if (runtime.ai)
            {
                runtime.hadTravel = runtime.ai->HasStrategy("travel", BOT_STATE_NON_COMBAT);
                runtime.hadFollow = runtime.ai->HasStrategy("follow", BOT_STATE_NON_COMBAT);
            }
        }
        members.push_back(runtime);
    }

    std::sort(members.begin(), members.end(), [](RuntimeMember const& left, RuntimeMember const& right)
    {
        return left.guid < right.guid;
    });
    return members;
}

std::vector<std::uint32_t> RosterGuids(std::vector<RuntimeMember> const& members)
{
    std::vector<std::uint32_t> result;
    result.reserve(members.size());
    for (RuntimeMember const& member : members)
        result.push_back(member.guid);
    return result;
}

bool SameRoster(Session const& session, std::vector<RuntimeMember> const& members)
{
    return session.roster == RosterGuids(members);
}

void ClearMemberTravel(RuntimeMember const& member, MemberPlan const* plan)
{
    if (!member.player || !member.ai || !plan)
        return;

    AiObjectContext* context = member.ai->GetAiObjectContext();
    TravelTarget* target = context ? context->GetValue<TravelTarget*>("travel target")->Get() : nullptr;
    if (target && (!plan || target->getDestination() == &plan->destination))
        target->setTarget(TravelMgr::instance().nullTravelDestination,
                          TravelMgr::instance().nullWorldPosition, true);

    member.ai->ChangeStrategy(plan->hadTravel ? "+travel" : "-travel", BOT_STATE_NON_COMBAT);
    member.ai->ChangeStrategy(plan->hadFollow ? "+follow" : "-follow", BOT_STATE_NON_COMBAT);
}

void ClearSessionMovement(Player* leader, Session& session)
{
    if (!leader || !leader->GetGroup())
        return;

    std::vector<RuntimeMember> members = ResolveRoster(leader, leader->GetGroup());
    for (RuntimeMember const& member : members)
    {
        auto const plan = session.plans.find(member.guid);
        if (plan != session.plans.end())
            ClearMemberTravel(member, &plan->second);
    }
    session.movementReleased = true;
}

Session& GetOrCreateSession(std::uint32_t leaderGuid)
{
    auto iterator = sessions.find(leaderGuid);
    if (iterator == sessions.end())
    {
        auto session = std::make_unique<Session>();
        session->leaderGuid = leaderGuid;
        session->last.leaderGuid = leaderGuid;
        iterator = sessions.emplace(leaderGuid, std::move(session)).first;
    }
    return *iterator->second;
}

void ResetSession(Player* leader, Session& session)
{
    ClearSessionMovement(leader, session);
    session.targetGuid.Clear();
    session.targetEntry = 0;
    session.pullAuthorized = false;
    session.engaged = false;
    session.movementReleased = false;
    session.hasPreviousDistance = false;
    session.previousMaxSlotDistance = std::numeric_limits<float>::infinity();
    session.stalledPolls = 0;
    session.roster.clear();
    session.plans.clear();
}

void EnsurePlans(Player* leader, Session& session, std::vector<RuntimeMember> const& members)
{
    if (SameRoster(session, members))
        return;

    ClearSessionMovement(leader, session);
    session.plans.clear();
    session.roster = RosterGuids(members);
    for (RuntimeMember const& member : members)
    {
        auto [iterator, inserted] = session.plans.emplace(member.guid, MemberPlan(member.guid));
        (void)inserted;
        iterator->second.hadTravel = member.hadTravel;
        iterator->second.hadFollow = member.hadFollow;
        iterator->second.initialized = true;
    }
}

bool IsBoss(Creature const* creature)
{
    if (!creature)
        return false;
    CreatureTemplate const* creatureTemplate = creature->GetCreatureTemplate();
    return creature->isWorldBoss() || creature->IsDungeonBoss() ||
        (creatureTemplate && creatureTemplate->rank == CREATURE_ELITE_WORLDBOSS);
}

Creature* FindSelectedBoss(Player* leader, ObjectGuid const& guid)
{
    if (!leader || guid.IsEmpty())
        return nullptr;

    Creature* creature = ObjectAccessor::GetCreature(*leader, guid);
    if (!creature || !creature->IsInWorld() || creature->IsDuringRemoveFromWorld() || !creature->IsAlive() ||
        !IsBoss(creature) || creature->GetMapId() != leader->GetMapId() ||
        creature->GetInstanceId() != leader->GetInstanceId())
        return nullptr;
    return creature;
}

struct CandidateSet
{
    std::map<std::uint64_t, Creature*> creatures;
    std::vector<AutoWowEncounterTelemetry::CandidateSignal> signals;
};

CandidateSet DiscoverBosses(Player* leader, std::vector<RuntimeMember> const& members)
{
    CandidateSet result;
    if (!leader)
        return result;

    for (RuntimeMember const& runtime : members)
    {
        Player* member = runtime.player;
        if (!member || !runtime.role.alive || !runtime.sameContext)
            continue;

        std::list<Creature*> found;
        Acore::AllWorldObjectsInRange check(member, kEncounterRadius);
        Acore::CreatureListSearcher<Acore::AllWorldObjectsInRange> searcher(member, found, check);
        Cell::VisitObjects(member, searcher, kEncounterRadius);
        for (Creature* creature : found)
        {
            if (!creature || !creature->IsInWorld() || creature->IsDuringRemoveFromWorld() ||
                !creature->IsAlive() || !IsBoss(creature) || creature->GetMapId() != leader->GetMapId() ||
                creature->GetInstanceId() != leader->GetInstanceId())
                continue;
            result.creatures.emplace(creature->GetGUID().GetCounter(), creature);
        }
    }

    for (auto const& [guid, creature] : result.creatures)
    {
        bool hostile = false;
        bool engaged = false;
        float minimumDistance = std::numeric_limits<float>::infinity();
        for (RuntimeMember const& runtime : members)
        {
            Player* member = runtime.player;
            if (!member || !runtime.role.alive || !runtime.sameContext)
                continue;

            minimumDistance = std::min(minimumDistance, member->GetDistance(creature));
            hostile = hostile || member->IsHostileTo(creature) || creature->IsHostileTo(member);
            engaged = engaged || creature->IsInCombatWith(member) || member->IsInCombatWith(creature) ||
                creature->GetVictim() == member || member->GetVictim() == creature ||
                creature->GetThreatMgr().IsThreatenedBy(member);
        }

        result.signals.push_back({guid, true, true, hostile, true, engaged, minimumDistance});
    }
    return result;
}

Creature* ChooseBoss(Player* leader, Session& session, CandidateSet const& discovered)
{
    if (Creature* current = FindSelectedBoss(leader, session.targetGuid))
    {
        auto const currentSignal = std::find_if(discovered.signals.begin(), discovered.signals.end(),
            [&session](AutoWowEncounterTelemetry::CandidateSignal const& signal)
            {
                return signal.guid == session.targetGuid.GetCounter();
            });
        if (currentSignal != discovered.signals.end() &&
            AutoWowEncounterTelemetry::IsCandidateEligible(*currentSignal))
            return current;
    }

    AutoWowEncounterTelemetry::CandidateSelection const selected =
        AutoWowEncounterTelemetry::SelectCandidates(discovered.signals);
    if (selected.candidates.empty())
        return nullptr;

    auto const creature = discovered.creatures.find(selected.candidates.front().guid);
    return creature == discovered.creatures.end() ? nullptr : creature->second;
}

bool SetPoint(WorldPosition& point, WorldPosition const& value)
{
    point.setMapId(value.GetMapId());
    point.setX(value.GetPositionX());
    point.setY(value.GetPositionY());
    point.setZ(value.GetPositionZ());
    point.setO(value.GetOrientation());
    return true;
}

struct PlannedSlot
{
    RuntimeMember const* member = nullptr;
    Policy::FormationSlot slot;
    WorldPosition point;
    float distance = 0.0f;
    Movement::PointsArray path;
};

float RequiredPullStagingDistance(Creature const* target, std::vector<RuntimeMember> const& members)
{
    float required = kPullStagingDistance;
    if (!target)
        return required;

    auto consider = [&required, target](Unit* unit, float formationBuffer)
    {
        if (!unit || !unit->IsInWorld() || !unit->IsAlive())
            return;

        // This uses the core's exact level/detection/stealth-aware aggro range, then adds the
        // hazard reach, cohort footprint, and the conservative unknown-hazard hold band. Pets are
        // included because they can be the first unit to enter a pack's detection envelope.
        float const range = target->GetAggroRange(unit) + target->GetCombatReach() +
            std::max(unit->GetCombatReach(), 1.5f) + formationBuffer +
            AutoWowBossApproachAggroPolicy::kUnknownHoldMargin;
        required = std::max(required, range);
    };

    for (RuntimeMember const& member : members)
    {
        if (!member.player || !member.role.alive || !member.sameContext)
            continue;
        consider(member.player, AutoWowBossApproachAggroPolicy::kFormationFootprintBuffer);
        consider(member.player->GetPet(), AutoWowBossApproachAggroPolicy::kPetFootprintBuffer);
        if (Guardian* guardian = member.player->GetGuardianPet())
            if (guardian != member.player->GetPet())
                consider(guardian, AutoWowBossApproachAggroPolicy::kPetFootprintBuffer);
    }
    return required;
}

struct MovementSegment
{
    Unit* unit = nullptr;
    float sourceX = 0.0f;
    float sourceY = 0.0f;
    float sourceZ = 0.0f;
    float targetX = 0.0f;
    float targetY = 0.0f;
    float targetZ = 0.0f;
    float footprint = 0.0f;
    bool tankProbe = false;
    bool petFootprint = false;
};

void AddMovementSegment(std::vector<MovementSegment>& segments, Unit* unit,
                        float sourceX, float sourceY, float sourceZ,
                        float targetX, float targetY, float targetZ,
                        float footprintBuffer, bool tankProbe, bool petFootprint)
{
    if (!unit || !unit->IsInWorld() || !unit->IsAlive())
        return;

    segments.push_back({unit, sourceX, sourceY, sourceZ, targetX, targetY, targetZ,
                        std::max(unit->GetCombatReach(), 1.5f) + footprintBuffer, tankProbe,
                        petFootprint});
}

void AddMovementSegment(std::vector<MovementSegment>& segments, Unit* unit,
                        WorldPosition const& destination, float footprintBuffer,
                        bool tankProbe, bool petFootprint)
{
    if (!unit)
        return;

    AddMovementSegment(segments, unit, unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ(),
                       destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(),
                       footprintBuffer, tankProbe, petFootprint);
}

void AddMovementPath(std::vector<MovementSegment>& segments, Unit* unit,
                     Movement::PointsArray const& path, WorldPosition const& destination,
                     float footprintBuffer, bool tankProbe, bool petFootprint)
{
    if (!unit || path.size() < 2)
    {
        AddMovementSegment(segments, unit, destination, footprintBuffer, tankProbe, petFootprint);
        return;
    }

    float sourceX = unit->GetPositionX();
    float sourceY = unit->GetPositionY();
    float sourceZ = unit->GetPositionZ();
    for (auto const& point : path)
    {
        AddMovementSegment(segments, unit, sourceX, sourceY, sourceZ, point.x, point.y, point.z,
                           footprintBuffer, tankProbe, petFootprint);
        sourceX = point.x;
        sourceY = point.y;
        sourceZ = point.z;
    }
}

float DistanceToSegment(Creature const* hazard, MovementSegment const& segment)
{
    if (!hazard || !segment.unit)
        return std::numeric_limits<float>::infinity();

    float const dx = segment.targetX - segment.sourceX;
    float const dy = segment.targetY - segment.sourceY;
    float const dz = segment.targetZ - segment.sourceZ;
    float const lengthSquared = dx * dx + dy * dy + dz * dz;
    float fraction = 0.0f;
    if (lengthSquared > 0.0001f)
    {
        fraction = ((hazard->GetPositionX() - segment.sourceX) * dx +
                    (hazard->GetPositionY() - segment.sourceY) * dy +
                    (hazard->GetPositionZ() - segment.sourceZ) * dz) / lengthSquared;
        fraction = std::clamp(fraction, 0.0f, 1.0f);
    }

    float const closestX = segment.sourceX + fraction * dx;
    float const closestY = segment.sourceY + fraction * dy;
    float const closestZ = segment.sourceZ + fraction * dz;
    // Inflate the movement corridor by the moving unit's combat reach and formation buffer here.
    // The returned value is therefore already distance-to-inflated-corridor; the pure policy adds
    // only the hazard's aggro radius and combat reach below.
    return hazard->GetDistance(closestX, closestY, closestZ) - segment.footprint;
}

std::vector<Creature*> EnumerateNearbyCreatures(std::vector<RuntimeMember> const& members)
{
    std::map<std::uint64_t, Creature*> unique;
    for (RuntimeMember const& runtime : members)
    {
        if (!runtime.player || !runtime.role.alive || !runtime.sameContext)
            continue;

        std::list<Creature*> found;
        Acore::AllWorldObjectsInRange check(runtime.player, kHazardScanRadius);
        Acore::CreatureListSearcher<Acore::AllWorldObjectsInRange> searcher(runtime.player, found, check);
        Cell::VisitObjects(runtime.player, searcher, kHazardScanRadius);
        for (Creature* creature : found)
        {
            if (!creature || !creature->IsInWorld() || creature->IsDuringRemoveFromWorld() ||
                !creature->IsAlive())
                continue;
            unique.emplace(creature->GetGUID().GetCounter(), creature);
        }
    }

    std::vector<Creature*> result;
    result.reserve(unique.size());
    for (auto const& [guid, creature] : unique)
    {
        (void)guid;
        result.push_back(creature);
    }
    return result;
}

struct AggroScan
{
    AutoWowBossApproachAggroPolicy::RiskResult risk;
    bool visibleObjectScan = false;
    std::size_t visibleCreatures = 0;
};

AggroScan ScanAggro(Player* leader, Creature* target, Session const& session,
                    std::vector<RuntimeMember> const& members,
                    std::vector<PlannedSlot> const& planned)
{
    AggroScan scan;
    if (!leader)
        return scan;

    std::vector<MovementSegment> segments;
    for (PlannedSlot const& item : planned)
    {
        if (!item.member || !item.member->player)
            continue;

        bool const tankProbe = item.slot.role == Policy::Role::MainTank ||
            item.slot.role == Policy::Role::Tank;
        AddMovementPath(segments, item.member->player, item.path, item.point,
                        AutoWowBossApproachAggroPolicy::kFormationFootprintBuffer, tankProbe, false);

        Unit* pet = item.member->player->GetPet();
        if (pet && pet->IsInWorld() && pet->IsAlive())
            AddMovementSegment(segments, pet, item.point,
                               AutoWowBossApproachAggroPolicy::kPetFootprintBuffer, tankProbe, true);

        Unit* guardian = item.member->player->GetGuardianPet();
        if (guardian && guardian != pet && guardian->IsInWorld() && guardian->IsAlive())
            AddMovementSegment(segments, guardian, item.point,
                               AutoWowBossApproachAggroPolicy::kPetFootprintBuffer, tankProbe, true);
    }

    std::vector<Unit*> cohortUnits;
    cohortUnits.reserve(members.size() * 2);
    for (RuntimeMember const& runtime : members)
    {
        if (!runtime.player || !runtime.role.alive || !runtime.sameContext)
            continue;
        cohortUnits.push_back(runtime.player);
        if (Unit* pet = runtime.player->GetPet(); pet && pet->IsInWorld() && pet->IsAlive())
            cohortUnits.push_back(pet);
        if (Unit* guardian = runtime.player->GetGuardianPet(); guardian && guardian->IsInWorld() && guardian->IsAlive() &&
            std::find(cohortUnits.begin(), cohortUnits.end(), guardian) == cohortUnits.end())
            cohortUnits.push_back(guardian);
    }

    std::vector<Creature*> nearby = EnumerateNearbyCreatures(members);
    scan.visibleObjectScan = true;
    scan.visibleCreatures = nearby.size();
    std::vector<AutoWowBossApproachAggroPolicy::HazardFacts> hazards;
    hazards.reserve(nearby.size());

    for (Creature* creature : nearby)
    {
        if (!creature || creature->GetMapId() != leader->GetMapId() ||
            creature->GetInstanceId() != leader->GetInstanceId())
            continue;

        float distanceToCorridor = std::numeric_limits<float>::infinity();
        bool tankProbe = false;
        bool petFootprint = false;
        for (MovementSegment const& segment : segments)
        {
            float const distance = DistanceToSegment(creature, segment);
            if (distance < distanceToCorridor)
            {
                distanceToCorridor = distance;
                tankProbe = segment.tankProbe;
                petFootprint = segment.petFootprint;
            }
        }
        if (!std::isfinite(distanceToCorridor))
            continue;

        bool knownHostile = false;
        bool currentlyEngagedByCohort = false;
        bool canStartAttack = false;
        float aggroRadius = 0.0f;
        bool detectionUnknown = false;
        for (Unit* cohortUnit : cohortUnits)
        {
            bool const hostile = creature->IsHostileTo(cohortUnit) || cohortUnit->IsHostileTo(creature);
            knownHostile = knownHostile || hostile;
            currentlyEngagedByCohort = currentlyEngagedByCohort ||
                creature->GetVictim() == cohortUnit || creature->IsInCombatWith(cohortUnit) ||
                cohortUnit->IsInCombatWith(creature) || creature->GetThreatMgr().IsThreatenedBy(cohortUnit);
            if (!hostile)
                continue;

            aggroRadius = std::max(aggroRadius, creature->GetAggroRange(cohortUnit));
            canStartAttack = canStartAttack || creature->CanStartAttack(cohortUnit);
            detectionUnknown = detectionUnknown || !creature->CanSeeOrDetect(cohortUnit, false, true) ||
                !cohortUnit->CanSeeOrDetect(creature, false, true) ||
                !cohortUnit->IsWithinLOSInMap(creature);
        }

        bool const scriptedUnknown = creature->IsTrigger() || creature->GetScriptId() != 0 ||
            creature->GetAIName() == "SmartAI" ||
            creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_NON_ATTACKABLE);
        if (!knownHostile && !scriptedUnknown && !detectionUnknown)
            continue;

        bool socialAssistPossible = false;
        if (knownHostile)
        {
            for (Creature* assistant : nearby)
            {
                if (!assistant || assistant == creature)
                    continue;

                if (std::any_of(cohortUnits.begin(), cohortUnits.end(),
                                [assistant, creature](Unit* enemy)
                                {
                                    return enemy && assistant->CanAssistTo(creature, enemy);
                                }))
                {
                    socialAssistPossible = true;
                    break;
                }
            }
        }

        // `distanceToCorridor` already includes the cohort-side footprint from the nearest
        // player/pet segment. Do not add the same formation buffer a second time in the policy.
        hazards.push_back({creature->GetGUID().GetCounter(), knownHostile, currentlyEngagedByCohort,
                           canStartAttack, detectionUnknown, scriptedUnknown, socialAssistPossible,
                           target && creature->GetGUID() == target->GetGUID() && session.pullAuthorized,
                           tankProbe, petFootprint, distanceToCorridor, aggroRadius,
                           creature->GetCombatReach(), 0.0f});
    }

    scan.risk = AutoWowBossApproachAggroPolicy::Evaluate(hazards, session.pullAuthorized);
    return scan;
}

bool BuildFormation(Player* leader, Creature* target, std::vector<RuntimeMember> const& members,
                    Session& session, std::vector<PlannedSlot>& planned, std::uint32_t& rejectedGuid)
{
    if (!leader || !target || !leader->GetMap())
        return false;

    std::vector<Policy::MemberFacts> roleFacts;
    roleFacts.reserve(members.size());
    for (RuntimeMember const& member : members)
        roleFacts.push_back(member.role);

    std::vector<Policy::FormationSlot> slots = Policy::BuildFormationSlots(std::move(roleFacts));
    if (slots.empty())
        return false;

    float forwardX = target->GetPositionX() - leader->GetPositionX();
    float forwardY = target->GetPositionY() - leader->GetPositionY();
    float const length = std::sqrt(forwardX * forwardX + forwardY * forwardY);
    if (length > 0.01f)
    {
        forwardX /= length;
        forwardY /= length;
    }
    else
    {
        forwardX = std::cos(leader->GetOrientation());
        forwardY = std::sin(leader->GetOrientation());
    }
    float const leftX = -forwardY;
    float const leftY = forwardX;

    planned.reserve(slots.size());
    for (Policy::FormationSlot const& slot : slots)
    {
        auto member = std::find_if(members.begin(), members.end(), [&slot](RuntimeMember const& candidate)
        {
            return candidate.guid == slot.memberGuid;
        });
        if (member == members.end() || !member->player || !member->ai || !member->sameContext ||
            !member->role.alive)
        {
            rejectedGuid = member == members.end() ? static_cast<std::uint32_t>(slot.memberGuid) : member->guid;
            return false;
        }

        // The first segment terminates at a dynamic staging ring outside the selected boss's
        // server-calculated aggro range. Only after the full cohort is assembled is the final
        // combat-position segment authorized and the intentional target exempted by ScanAggro.
        float const baseStandOff = session.pullAuthorized ? kBossStandOff :
            RequiredPullStagingDistance(target, members);
        float const trailing = baseStandOff + slot.trailing;
        float const x = target->GetPositionX() - forwardX * trailing + leftX * slot.lateral;
        float const y = target->GetPositionY() - forwardY * trailing + leftY * slot.lateral;
        float const searchTop = std::max(member->player->GetPositionZ(), target->GetPositionZ()) + 2.0f;
        float const ground = leader->GetMap()->GetHeight(member->player->GetPhaseMask(), x, y, searchTop, true, 20.0f);
        if (ground <= INVALID_HEIGHT || !std::isfinite(ground))
        {
            rejectedGuid = member->guid;
            return false;
        }

        WorldPosition point(leader->GetMapId(), x, y, ground, 0.0f);
        float const distance = member->player->GetExactDist(x, y, ground);
        Movement::PointsArray path;
        if (distance > Policy::kSlotTolerance)
        {
            AutoWowDungeonPath::ProbeResult probe = AutoWowDungeonPath::Probe(
                member->player, x, y, ground);
            if (!probe.safe)
            {
                rejectedGuid = member->guid;
                return false;
            }
            path = std::move(probe.path);
        }

        planned.push_back({&*member, slot, point, distance, std::move(path)});
    }
    (void)session;
    return true;
}

std::uint32_t InstallMovement(Player* leader, Session& session, std::vector<PlannedSlot> const& planned)
{
    std::uint32_t issued = 0;
    session.movementReleased = false;
    for (PlannedSlot const& item : planned)
    {
        RuntimeMember const& member = *item.member;
        auto plan = session.plans.find(member.guid);
        if (plan == session.plans.end() || !member.player || !member.ai)
            continue;

        SetPoint(plan->second.point, item.point);
        AiObjectContext* context = member.ai->GetAiObjectContext();
        TravelTarget* travelTarget = context ? context->GetValue<TravelTarget*>("travel target")->Get() : nullptr;
        if (!travelTarget)
            continue;

        if (travelTarget->getDestination() != &plan->second.destination ||
            travelTarget->getPosition() != &plan->second.point)
            travelTarget->setTarget(&plan->second.destination, &plan->second.point, true);
        travelTarget->setRadius(Policy::kSlotTolerance);
        travelTarget->setForced(true);

        // A temporary role slot owns non-combat movement. Remove ordinary follow so the group does
        // not stretch into a line while laggards catch up, then let the normal travel strategy
        // and MoveToTravelTargetAction perform navigation.
        member.ai->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);
        member.ai->ChangeStrategy("+travel", BOT_STATE_NON_COMBAT);
        if (item.distance > Policy::kSlotTolerance && !member.player->isMoving() &&
            member.ai->DoSpecificAction("move to travel target", Event(), true))
            ++issued;
    }
    (void)leader;
    return issued;
}

void FillCounts(Result& result, std::vector<RuntimeMember> const& members, Player* leader, Creature* target,
                std::vector<PlannedSlot> const& planned)
{
    result.rosterMembers = members.size();
    for (RuntimeMember const& member : members)
    {
        if (member.role.alive)
            ++result.livingMembers;
        if (member.role.alive && member.role.tank)
            ++result.tankMembers;
        if (member.player && member.player->IsInWorld() && member.ai && !member.ai->IsRealPlayer())
            ++result.onlinePlayerbots;
        if (member.role.alive && member.sameContext)
            ++result.sameContext;
        if (member.role.alive && member.transientFree)
            ++result.transientFree;
        if (leader && member.player && member.role.alive && member.sameContext)
        {
            float const leaderDistance = member.player->GetDistance(leader);
            result.maxLeaderDistance = std::max(result.maxLeaderDistance, leaderDistance);
            if (leaderDistance <= Policy::kCohesionRadius)
                ++result.withinCohesion;
        }
    }

    for (PlannedSlot const& item : planned)
    {
        if (!item.member || !item.member->player)
            continue;
        result.maxSlotDistance = std::max(result.maxSlotDistance, item.distance);
        if (item.distance <= Policy::kSlotTolerance)
            ++result.slotReady;
    }
    if (target && leader)
        result.targetDistance = leader->GetDistance(target);
}

void FillExactGate(Result& result, Player* leader, Creature* target,
                   std::vector<RuntimeMember> const& members)
{
    if (!leader || !target)
        return;
    for (RuntimeMember const& member : members)
    {
        if (!member.player || !member.ai || member.ai->IsRealPlayer() || !member.role.alive ||
            !member.sameContext)
            continue;
        bool const attackable = target->IsInWorld() && target->IsAlive() && target->IsHostileTo(member.player) &&
            target->isTargetableForAttack() && member.player->IsValidAttackTarget(target);
        if (attackable && member.player->GetDistance(target) <= AutoWowExactBossTarget::kMaxTargetDistance &&
            member.player->IsWithinLOSInMap(target))
            ++result.exactRangeAndLos;
    }
}

Policy::Decision Decide(Result const& result, bool targetVisible, bool pathsSafe, bool movementIssued,
                        bool progressMade, std::uint32_t stalledPolls, bool aggroSafe, bool unknownHazard)
{
    Policy::CohesionFacts facts;
    facts.rosterMembers = result.rosterMembers;
    facts.livingMembers = result.livingMembers;
    facts.onlinePlayerbots = result.onlinePlayerbots;
    facts.tankMembers = result.tankMembers;
    facts.sameContext = result.sameContext;
    facts.transientFree = result.transientFree;
    facts.withinCohesion = result.withinCohesion;
    facts.slotReady = result.slotReady;
    facts.exactRangeAndLos = result.exactRangeAndLos;
    facts.targetVisible = targetVisible;
    facts.pathsSafe = pathsSafe;
    facts.aggroSafe = aggroSafe;
    facts.unknownHazard = unknownHazard;
    facts.movementIssued = movementIssued;
    facts.progressMade = progressMade;
    facts.stalledPolls = stalledPolls;
    return Policy::Evaluate(facts);
}
}

std::string Result::Json() const
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(3)
        << "{\"ok\":" << JsonBool(ok)
        << ",\"order\":\"boss_approach\""
        << ",\"schema\":\"autowow.boss-approach.v0\""
        << ",\"phase\":\"" << Policy::PhaseName(phase) << "\""
        << ",\"reason\":" << JsonString(reason)
        << ",\"leader_guid\":" << leaderGuid
        << ",\"route_authority\":\"leader\""
        << ",\"target\":{\"visible\":" << JsonBool(targetVisible)
        << ",\"guid\":" << targetGuid
        << ",\"entry\":" << targetEntry
        << ",\"name\":" << JsonString(targetName)
        << ",\"leader_distance\":";
    AppendNumber(out, targetDistance);
    out << "}"
        << ",\"formation\":{\"mode\":\"role_bounded_dynamic\""
        << ",\"roster_members\":" << rosterMembers
        << ",\"living_members\":" << livingMembers
        << ",\"online_playerbots\":" << onlinePlayerbots
        << ",\"tank_members\":" << tankMembers
        << ",\"same_context\":" << sameContext
        << ",\"transient_free\":" << transientFree
        << ",\"within_cohesion\":" << withinCohesion
        << ",\"slot_ready\":" << slotReady
        << ",\"exact_range_and_los\":" << exactRangeAndLos
        << ",\"cohesion_radius\":" << Policy::kCohesionRadius
        << ",\"slot_tolerance\":" << Policy::kSlotTolerance
        << ",\"max_slot_distance\":";
    AppendNumber(out, maxSlotDistance);
    out << ",\"max_leader_distance\":";
    AppendNumber(out, maxLeaderDistance);
    out << "}"
        << ",\"movement\":{\"issued\":" << movementIssued
        << ",\"stalled_polls\":" << stalledPolls
        << ",\"paths_safe\":" << JsonBool(pathsSafe)
        << ",\"path_rejected_guid\":" << pathRejectedGuid << "}"
        << ",\"aggro_risk\":{\"safe\":" << JsonBool(aggroSafe)
        << ",\"pull_authorized\":" << JsonBool(pullAuthorized)
        << ",\"hazard_objects_scanned\":" << hazardObjectsScanned
        << ",\"hazards_in_corridor\":" << hazardsInCorridor
        << ",\"predicted_hazards\":" << predictedHazards
        << ",\"unknown_hazards\":" << unknownHazards
        << ",\"unknown_blocks\":" << JsonBool(unknownHazardBlocks)
        << ",\"accidental_pull_count\":" << accidentalPullCount
        << ",\"social_assist_candidates\":" << socialAssistCandidates
        << ",\"tank_probe_hazards\":" << tankProbeHazards
        << ",\"pet_hazards\":" << petHazards
        << ",\"intentional_exemptions\":" << intentionalExemptions
        << ",\"closest_margin\":";
    AppendNumber(out, closestHazardMargin);
    out << ",\"visibility_scope\":\"server_visible_grid_objects_only\""
        << ",\"unknowns_are_not_claimed_safe\":true}"
        << ",\"evidence\":{\"server_known\":[\"map_instance_membership\",\"faction_hostility\",\"core_aggro_range_including_level_difference\",\"combat_reach\",\"can_start_attack\",\"los_and_detection_api\",\"social_assist_eligibility_api\"],\"conservative\":[\"formation_and_pet_footprint_buffers\",\"unknown_hold_margin\",\"navmesh_corridor_inflation\",\"social_assist_distance_template_unknown\"],\"scripted_or_invisible_spawns\":\"unknown_not_safe\"}"
        << ",\"engaged\":" << JsonBool(engaged) << "}";
    return out.str();
}

Result Run(Player* leader, PlayerbotAI* leaderAI, bool statusOnly)
{
    Result result;
    result.leaderGuid = leader ? GuidCounter(leader->GetGUID()) : 0;
    if (!leader)
    {
        result.reason = "leader_not_online";
        return result;
    }

    auto sessionIterator = sessions.find(result.leaderGuid);
    if (statusOnly)
    {
        if (sessionIterator == sessions.end())
        {
            result.reason = "no_active_session";
            return result;
        }
        return sessionIterator->second->last;
    }

    if (!leaderAI || leaderAI->IsRealPlayer())
    {
        result.reason = "leader_not_playerbot";
        return result;
    }
    Group* group = leader->GetGroup();
    if (!group || group->GetLeaderGUID() != leader->GetGUID())
    {
        result.reason = "approach_requires_grouped_leader";
        return result;
    }
    if (!leader->IsInWorld() || !leader->IsAlive())
    {
        result.reason = "leader_not_alive_in_world";
        return result;
    }
    if (group->GetMembersCount() > kMaxRosterMembers)
    {
        result.reason = "roster_exceeds_supported_raid_size";
        return result;
    }

    std::vector<RuntimeMember> members = ResolveRoster(leader, group);
    result.rosterMembers = members.size();
    if (members.empty())
    {
        result.reason = "empty_group_roster";
        return result;
    }

    Session& session = GetOrCreateSession(result.leaderGuid);
    if (session.engaged)
    {
        if (FindSelectedBoss(leader, session.targetGuid))
            return session.last;
        ResetSession(leader, session);
    }

    CandidateSet const discovered = DiscoverBosses(leader, members);
    Creature* target = ChooseBoss(leader, session, discovered);
    if (!target)
    {
        result.reason = "no_eligible_boss_in_encounter_radius";
        result.targetVisible = false;
        session.last = result;
        return result;
    }

    ObjectGuid const targetGuid = target->GetGUID();
    if (session.targetGuid != targetGuid)
    {
        ResetSession(leader, session);
        session.targetGuid = targetGuid;
        session.targetEntry = target->GetEntry();
    }

    EnsurePlans(leader, session, members);
    result.targetVisible = true;
    result.target = target;
    result.targetGuid = targetGuid.GetCounter();
    result.targetEntry = target->GetEntry();
    result.targetName = target->GetName();
    result.targetDistance = leader->GetDistance(target);
    result.leaderRouteAuthority = true;

    std::vector<PlannedSlot> planned;
    std::uint32_t rejectedGuid = 0;
    bool const canPlanMovement = std::all_of(members.begin(), members.end(), [](RuntimeMember const& member)
    {
        return member.player && member.ai && !member.ai->IsRealPlayer() && member.role.alive &&
            member.sameContext && member.transientFree;
    });
    bool pathsSafe = true;
    AggroScan aggro;
    if (canPlanMovement)
    {
        pathsSafe = BuildFormation(leader, target, members, session, planned, rejectedGuid);
        if (pathsSafe)
        {
            aggro = ScanAggro(leader, target, session, members, planned);
            if (aggro.risk.safe)
                result.movementIssued = InstallMovement(leader, session, planned);
            else if (!session.movementReleased)
                ClearSessionMovement(leader, session);
        }
        else if (!session.movementReleased)
            ClearSessionMovement(leader, session);
    }
    else if (!session.movementReleased)
        ClearSessionMovement(leader, session);

    FillCounts(result, members, leader, target, planned);
    FillExactGate(result, leader, target, members);
    result.pathsSafe = pathsSafe;
    result.pathRejectedGuid = rejectedGuid;
    result.hazardsInCorridor = aggro.risk.hazardsInCorridor;
    result.predictedHazards = aggro.risk.predictedHazards;
    result.unknownHazards = aggro.risk.unknownHazards;
    result.accidentalPullCount = aggro.risk.accidentalPullCount;
    result.socialAssistCandidates = aggro.risk.socialAssistCandidates;
    result.tankProbeHazards = aggro.risk.tankProbeHazards;
    result.petHazards = aggro.risk.petHazards;
    result.intentionalExemptions = aggro.risk.intentionalExemptions;
    result.hazardObjectsScanned = aggro.visibleCreatures;
    result.closestHazardMargin = aggro.risk.closestMargin;
    result.aggroSafe = aggro.risk.safe;
    result.unknownHazardBlocks = aggro.risk.unknownBlocks;

    bool const stagingReady = !session.pullAuthorized && aggro.risk.safe &&
        result.rosterMembers == result.livingMembers &&
        result.rosterMembers == result.onlinePlayerbots &&
        result.rosterMembers == result.sameContext &&
        result.rosterMembers == result.transientFree &&
        result.tankMembers > 0 &&
        result.rosterMembers == result.withinCohesion &&
        result.rosterMembers == result.slotReady;
    if (stagingReady)
    {
        session.pullAuthorized = true;
        session.hasPreviousDistance = false;
        session.previousMaxSlotDistance = std::numeric_limits<float>::infinity();
        session.stalledPolls = 0;
    }
    result.pullAuthorized = session.pullAuthorized;

    bool const progressMade = !session.hasPreviousDistance ||
        result.maxSlotDistance < session.previousMaxSlotDistance - kProgressEpsilon;
    if (session.hasPreviousDistance && !progressMade && result.maxSlotDistance > Policy::kSlotTolerance)
        ++session.stalledPolls;
    else if (progressMade)
        session.stalledPolls = 0;
    session.hasPreviousDistance = true;
    session.previousMaxSlotDistance = result.maxSlotDistance;

    Policy::Decision const decision = Decide(result, true, pathsSafe, result.movementIssued > 0,
                                             progressMade, session.stalledPolls, result.aggroSafe,
                                             result.unknownHazardBlocks);
    result.phase = decision.phase;
    result.reason = decision.reason;
    if (stagingReady)
        result.reason = "staging_cohort_ready_pull_authorized";
    result.stalledPolls = session.stalledPolls;
    result.ok = decision.phase != Policy::Phase::Blocked && decision.phase != Policy::Phase::Stalled;
    result.shouldEngage = decision.phase == Policy::Phase::Ready;
    result.engaged = false;
    session.last = result;
    return result;
}

void ReleaseMovement(Player* leader)
{
    if (!leader)
        return;
    auto iterator = sessions.find(GuidCounter(leader->GetGUID()));
    if (iterator != sessions.end())
        ClearSessionMovement(leader, *iterator->second);
}

void MarkEngaged(Player* leader, bool success)
{
    if (!leader)
        return;
    auto iterator = sessions.find(GuidCounter(leader->GetGUID()));
    if (iterator == sessions.end())
        return;

    Session& session = *iterator->second;
    session.engaged = success;
    session.last.shouldEngage = false;
    session.last.engaged = success;
    session.last.ok = success;
    session.last.reason = success ? "engaged" : "engage_rejected";
}
}
