/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EncounterTelemetry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <list>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>

#include "AiObjectContext.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Spell.h"
#include "ThreatManager.h"
#include "Unit.h"

namespace AutoWowEncounterTelemetry
{
namespace
{
struct UnitView
{
    bool present = false;
    std::uint64_t guid = 0;
    std::uint32_t entry = 0;
    std::string name;
    bool alive = false;
    std::uint32_t health = 0;
    std::uint32_t maxHealth = 0;
    float healthPct = 0.0f;
};

struct MemberRuntime
{
    Player* player = nullptr;
    PlayerbotAI* botAI = nullptr;
    Unit* target = nullptr;
    std::string targetSource = "none";
    std::uint64_t selectedGuid = 0;
};

struct MemberThreatFacts
{
    std::uint32_t ownedCreatures = 0;
    std::uint32_t ownedCreaturesTargetingMember = 0;
    std::uint64_t highestOwnedCreatureGuid = 0;
    float highestOwnedThreat = 0.0f;
};

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

char const* JsonBool(bool value)
{
    return value ? "true" : "false";
}

char const* DeathStateName(DeathState state)
{
    switch (state)
    {
        case DeathState::Alive: return "alive";
        case DeathState::JustDied: return "just_died";
        case DeathState::Corpse: return "corpse";
        case DeathState::Dead: return "dead";
        case DeathState::JustRespawned: return "just_respawned";
        default: return "unknown";
    }
}

char const* RankName(std::uint32_t rank)
{
    switch (rank)
    {
        case CREATURE_ELITE_NORMAL: return "normal";
        case CREATURE_ELITE_ELITE: return "elite";
        case CREATURE_ELITE_RAREELITE: return "rare_elite";
        case CREATURE_ELITE_WORLDBOSS: return "world_boss";
        case CREATURE_ELITE_RARE: return "rare";
        default: return "unknown";
    }
}

std::uint64_t GuidCounter(ObjectGuid const& guid)
{
    return static_cast<std::uint64_t>(guid.GetCounter());
}

bool IsAvailablePlayerbot(Player* player, PlayerbotAI*& botAI)
{
    botAI = player ? PlayerbotsMgr::instance().GetPlayerbotAI(player) : nullptr;
    return player && player->IsInWorld() && botAI && !botAI->IsRealPlayer();
}

UnitView MakeUnitView(Unit* unit)
{
    UnitView view;
    if (!unit || !unit->IsInWorld() || unit->IsDuringRemoveFromWorld())
        return view;

    view.present = true;
    view.guid = GuidCounter(unit->GetGUID());
    view.entry = unit->ToPlayer() ? 0u : unit->GetEntry();
    view.name = unit->GetName();
    view.alive = unit->IsAlive();
    view.health = unit->GetHealth();
    view.maxHealth = unit->GetMaxHealth();
    view.healthPct = Percent(view.health, view.maxHealth);
    return view;
}

void AppendUnitView(std::ostringstream& out, UnitView const& view)
{
    if (!view.present)
    {
        out << "null";
        return;
    }

    out << "{\"guid\":" << view.guid
        << ",\"entry\":" << view.entry
        << ",\"name\":" << JsonString(view.name)
        << ",\"alive\":" << JsonBool(view.alive)
        << ",\"health\":{\"current\":" << view.health
        << ",\"max\":" << view.maxHealth
        << ",\"pct\":" << view.healthPct << "}}";
}

std::uint32_t CurrentSpellId(Unit* unit)
{
    if (!unit)
        return 0;

    constexpr std::array<CurrentSpellTypes, 4> order = {
        CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL, CURRENT_AUTOREPEAT_SPELL, CURRENT_MELEE_SPELL};
    for (CurrentSpellTypes type : order)
    {
        Spell* spell = unit->GetCurrentSpell(type);
        if (spell && spell->GetSpellInfo())
            return spell->GetSpellInfo()->Id;
    }
    return 0;
}

MemberThreatFacts ReadMemberThreat(Player* member)
{
    MemberThreatFacts facts;
    if (!member)
        return facts;

    for (auto const& [guid, reference] : member->GetThreatMgr().GetThreatenedByMeList())
    {
        (void)guid;
        if (!reference)
            continue;
        Creature* owner = reference->GetOwner();
        if (!owner || !owner->IsInWorld() || owner->IsDuringRemoveFromWorld())
            continue;

        ++facts.ownedCreatures;
        if (owner->GetVictim() && owner->GetVictim()->GetGUID() == member->GetGUID())
            ++facts.ownedCreaturesTargetingMember;

        float const threat = reference->GetThreat();
        std::uint64_t const ownerGuid = GuidCounter(owner->GetGUID());
        if (threat > facts.highestOwnedThreat ||
            (threat == facts.highestOwnedThreat &&
             (!facts.highestOwnedCreatureGuid || ownerGuid < facts.highestOwnedCreatureGuid)))
        {
            facts.highestOwnedThreat = threat;
            facts.highestOwnedCreatureGuid = ownerGuid;
        }
    }
    return facts;
}

std::vector<std::uint64_t> ExactRoster(Player* leader, std::size_t& total, bool& truncated)
{
    std::vector<std::uint64_t> guids;
    if (Group* group = leader ? leader->GetGroup() : nullptr)
    {
        guids.reserve(group->GetMembersCount());
        for (Group::MemberSlot const& slot : group->GetMemberSlots())
            guids.push_back(GuidCounter(slot.guid));
    }
    else if (leader)
        guids.push_back(GuidCounter(leader->GetGUID()));

    std::sort(guids.begin(), guids.end());
    guids.erase(std::unique(guids.begin(), guids.end()), guids.end());
    total = guids.size();
    truncated = guids.size() > kMaxRosterGuids;
    if (truncated)
        guids.resize(kMaxRosterGuids);
    return guids;
}

std::vector<MemberRuntime> ResolveMembers(std::vector<std::uint64_t> const& roster)
{
    std::vector<MemberRuntime> members;
    members.reserve(std::min(roster.size(), kMaxMembers));
    for (std::uint64_t guid : roster)
    {
        if (members.size() == kMaxMembers)
            break;

        Player* player = ObjectAccessor::FindPlayer(
            ObjectGuid::Create<HighGuid::Player>(static_cast<ObjectGuid::LowType>(guid)));
        PlayerbotAI* botAI = nullptr;
        if (!IsAvailablePlayerbot(player, botAI))
            continue;

        MemberRuntime runtime;
        runtime.player = player;
        runtime.botAI = botAI;
        runtime.selectedGuid = GuidCounter(player->GetTarget());

        if (AiObjectContext* context = botAI->GetAiObjectContext())
        {
            if (auto* currentTarget = context->GetValue<Unit*>("current target"))
            {
                Unit* target = currentTarget->Get();
                if (target && target->IsInWorld() && !target->IsDuringRemoveFromWorld())
                {
                    runtime.target = target;
                    runtime.targetSource = "ai_current";
                }
            }
        }

        if (!runtime.target && player->GetTarget())
        {
            runtime.target = ObjectAccessor::GetUnit(*player, player->GetTarget());
            if (runtime.target)
                runtime.targetSource = "selected";
        }
        members.push_back(std::move(runtime));
    }
    return members;
}

using MapInstance = std::pair<std::uint32_t, std::uint32_t>;

MapInstance ChooseContext(Player* leader, std::vector<MemberRuntime> const& members, std::string& source)
{
    MapInstance const leaderContext = {leader->GetMapId(), leader->GetInstanceId()};
    bool const released = leader->HasPlayerFlag(PLAYER_FLAGS_GHOST);
    if (leader->IsAlive() && !released)
    {
        source = "leader";
        return leaderContext;
    }

    struct ContextScore
    {
        std::size_t alive = 0;
        std::size_t total = 0;
    };
    std::map<MapInstance, ContextScore> scores;
    for (MemberRuntime const& runtime : members)
    {
        MapInstance const key = {runtime.player->GetMapId(), runtime.player->GetInstanceId()};
        ContextScore& score = scores[key];
        ++score.total;
        if (runtime.player->IsAlive())
            ++score.alive;
    }

    MapInstance best = leaderContext;
    ContextScore bestScore = scores[leaderContext];
    for (auto const& [key, score] : scores)
    {
        if (std::tie(score.alive, score.total) > std::tie(bestScore.alive, bestScore.total) ||
            (score.alive == bestScore.alive && score.total == bestScore.total && key < best))
        {
            best = key;
            bestScore = score;
        }
    }

    source = best == leaderContext ? "leader_dead_same_context" : "group_context_for_dead_leader";
    return best;
}

Player* ContextAnchor(Player* leader, std::vector<MemberRuntime> const& members, MapInstance context)
{
    Player* best = nullptr;
    for (MemberRuntime const& runtime : members)
    {
        Player* member = runtime.player;
        if (MapInstance{member->GetMapId(), member->GetInstanceId()} != context)
            continue;
        if (!best || (member->IsAlive() && !best->IsAlive()) ||
            (member->IsAlive() == best->IsAlive() && GuidCounter(member->GetGUID()) < GuidCounter(best->GetGUID())))
            best = member;
    }
    return best ? best : leader;
}

void AppendMember(std::ostringstream& out, MemberRuntime const& runtime, MapInstance context)
{
    Player* member = runtime.player;
    Powers const powerType = member->getPowerType();
    std::uint32_t const health = member->GetHealth();
    std::uint32_t const maxHealth = member->GetMaxHealth();
    std::uint32_t const mana = member->GetPower(POWER_MANA);
    std::uint32_t const maxMana = member->GetMaxPower(POWER_MANA);
    std::uint32_t const power = member->GetPower(powerType);
    std::uint32_t const maxPower = member->GetMaxPower(powerType);
    bool const tank = PlayerbotAI::IsTank(member);
    bool const healer = PlayerbotAI::IsHeal(member);
    bool const dps = PlayerbotAI::IsDps(member);
    MemberThreatFacts const threat = ReadMemberThreat(member);

    out << "{\"guid\":" << GuidCounter(member->GetGUID())
        << ",\"name\":" << JsonString(member->GetName())
        << ",\"alive\":" << JsonBool(member->IsAlive())
        << ",\"death_state\":" << JsonString(DeathStateName(member->getDeathState()))
        << ",\"released\":" << JsonBool(member->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        << ",\"in_combat\":" << JsonBool(member->IsInCombat())
        << ",\"roles\":{\"tank\":" << JsonBool(tank)
        << ",\"healer\":" << JsonBool(healer)
        << ",\"dps\":" << JsonBool(dps)
        << ",\"ranged\":" << JsonBool(PlayerbotAI::IsRanged(member))
        << ",\"main_tank\":" << JsonBool(PlayerbotAI::IsMainTank(member))
        << ",\"explicit_main_tank\":" << JsonBool(PlayerbotAI::IsExplicitMainTank(member))
        << ",\"assist_tank\":" << JsonBool(PlayerbotAI::IsAssistTank(member)) << "}"
        << ",\"health\":{\"current\":" << health
        << ",\"max\":" << maxHealth
        << ",\"pct\":" << Percent(health, maxHealth) << "}"
        << ",\"mana\":{\"current\":" << mana
        << ",\"max\":" << maxMana
        << ",\"pct\":" << Percent(mana, maxMana) << "}"
        << ",\"power\":{\"type\":" << static_cast<std::uint32_t>(powerType)
        << ",\"current\":" << power
        << ",\"max\":" << maxPower << "}"
        << ",\"position\":{\"map_id\":" << member->GetMapId()
        << ",\"instance_id\":" << member->GetInstanceId()
        << ",\"in_context\":"
        << JsonBool(MapInstance{member->GetMapId(), member->GetInstanceId()} == context)
        << ",\"x\":" << member->GetPositionX()
        << ",\"y\":" << member->GetPositionY()
        << ",\"z\":" << member->GetPositionZ()
        << ",\"orientation\":" << member->GetOrientation() << "}"
        << ",\"target\":{\"source\":" << JsonString(runtime.targetSource)
        << ",\"selected_guid\":" << runtime.selectedGuid
        << ",\"unit\":";
    AppendUnitView(out, MakeUnitView(runtime.target));
    out << "},\"victim\":";
    AppendUnitView(out, MakeUnitView(member->GetVictim()));
    out << ",\"active_spell_id\":" << CurrentSpellId(member)
        << ",\"threat\":{\"owned_creatures\":" << threat.ownedCreatures
        << ",\"owned_creatures_targeting_member\":" << threat.ownedCreaturesTargetingMember
        << ",\"highest_owned_creature_guid\":" << threat.highestOwnedCreatureGuid
        << ",\"highest_owned_threat\":" << threat.highestOwnedThreat << "}}";
}

void AppendEncounterUnit(std::ostringstream& out, Creature* creature, CandidateSignal const& signal,
                         ThreatAggregate const& threat)
{
    CreatureTemplate const* creatureTemplate = creature->GetCreatureTemplate();
    std::uint32_t const rank = creatureTemplate ? creatureTemplate->rank : CREATURE_ELITE_NORMAL;
    UnitView const victim = MakeUnitView(creature->GetVictim());
    std::uint32_t const health = creature->GetHealth();
    std::uint32_t const maxHealth = creature->GetMaxHealth();

    out << "{\"guid\":" << signal.guid
        << ",\"entry\":" << creature->GetEntry()
        << ",\"name\":" << JsonString(creature->GetName())
        << ",\"rank\":{\"id\":" << rank
        << ",\"name\":" << JsonString(RankName(rank))
        << ",\"boss\":" << JsonBool(signal.boss) << "}"
        << ",\"alive\":" << JsonBool(creature->IsAlive())
        << ",\"health\":{\"current\":" << health
        << ",\"max\":" << maxHealth
        << ",\"pct\":" << Percent(health, maxHealth) << "}"
        << ",\"position\":{\"x\":" << creature->GetPositionX()
        << ",\"y\":" << creature->GetPositionY()
        << ",\"z\":" << creature->GetPositionZ()
        << ",\"orientation\":" << creature->GetOrientation()
        << ",\"distance\":" << signal.distance << "}"
        << ",\"in_combat\":" << JsonBool(creature->IsInCombat())
        << ",\"engaged_by_group\":" << JsonBool(signal.engaged)
        << ",\"victim\":";
    AppendUnitView(out, victim);
    out << ",\"active_spell_id\":" << CurrentSpellId(creature)
        << ",\"group_links\":{\"threatening_members\":" << threat.threateningMembers
        << ",\"targeting_members\":" << threat.targetingMembers
        << ",\"threatening_or_targeting_members\":" << threat.threateningOrTargetingMembers
        << ",\"victim_links\":" << threat.victimLinks
        << ",\"combat_links\":" << threat.combatLinks
        << ",\"involved_members\":" << threat.involvedMembers << "}}";
}
}  // namespace

float Percent(std::uint32_t current, std::uint32_t maximum)
{
    if (!maximum)
        return 0.0f;
    std::uint32_t const bounded = std::min(current, maximum);
    return static_cast<float>(bounded) * 100.0f / static_cast<float>(maximum);
}

bool IsCandidateEligible(CandidateSignal const& candidate, float radius)
{
    if (!candidate.guid || !candidate.inWorld || !candidate.alive || !std::isfinite(candidate.distance) ||
        !std::isfinite(radius) || radius < 0.0f || candidate.distance < 0.0f || candidate.distance > radius)
        return false;
    return candidate.engaged || (candidate.boss && candidate.hostile);
}

CandidateSelection SelectCandidates(std::vector<CandidateSignal> candidates, std::size_t cap, float radius)
{
    std::map<std::uint64_t, CandidateSignal> merged;
    for (CandidateSignal const& candidate : candidates)
    {
        if (!candidate.guid)
            continue;
        auto [iterator, inserted] = merged.emplace(candidate.guid, candidate);
        if (inserted)
            continue;

        CandidateSignal& existing = iterator->second;
        existing.inWorld = existing.inWorld || candidate.inWorld;
        existing.alive = existing.alive || candidate.alive;
        existing.hostile = existing.hostile || candidate.hostile;
        existing.boss = existing.boss || candidate.boss;
        existing.engaged = existing.engaged || candidate.engaged;
        if (std::isfinite(candidate.distance) &&
            (!std::isfinite(existing.distance) || candidate.distance < existing.distance))
            existing.distance = candidate.distance;
    }

    CandidateSelection selection;
    selection.candidates.reserve(merged.size());
    for (auto const& [guid, candidate] : merged)
    {
        (void)guid;
        if (IsCandidateEligible(candidate, radius))
            selection.candidates.push_back(candidate);
    }

    std::sort(selection.candidates.begin(), selection.candidates.end(),
              [](CandidateSignal const& left, CandidateSignal const& right)
    {
        if (left.boss != right.boss)
            return left.boss > right.boss;
        if (left.engaged != right.engaged)
            return left.engaged > right.engaged;
        if (left.distance != right.distance)
            return left.distance < right.distance;
        return left.guid < right.guid;
    });

    selection.eligibleBeforeCap = selection.candidates.size();
    selection.truncated = selection.candidates.size() > cap;
    if (selection.truncated)
        selection.candidates.resize(cap);
    return selection;
}

ThreatAggregate AggregateThreat(std::vector<MemberEngagementSignal> signals)
{
    std::map<std::uint64_t, MemberEngagementSignal> merged;
    for (MemberEngagementSignal const& signal : signals)
    {
        if (!signal.memberGuid)
            continue;
        MemberEngagementSignal& aggregate = merged[signal.memberGuid];
        aggregate.memberGuid = signal.memberGuid;
        aggregate.threatens = aggregate.threatens || signal.threatens;
        aggregate.targets = aggregate.targets || signal.targets;
        aggregate.victimLink = aggregate.victimLink || signal.victimLink;
        aggregate.combatLink = aggregate.combatLink || signal.combatLink;
    }

    ThreatAggregate aggregate;
    for (auto const& [guid, signal] : merged)
    {
        (void)guid;
        aggregate.threateningMembers += signal.threatens ? 1u : 0u;
        aggregate.targetingMembers += signal.targets ? 1u : 0u;
        aggregate.victimLinks += signal.victimLink ? 1u : 0u;
        aggregate.combatLinks += signal.combatLink ? 1u : 0u;
        aggregate.threateningOrTargetingMembers += (signal.threatens || signal.targets) ? 1u : 0u;
        aggregate.involvedMembers +=
            (signal.threatens || signal.targets || signal.victimLink || signal.combatLink) ? 1u : 0u;
    }
    return aggregate;
}

std::string Build(Player* leader, PlayerbotAI* leaderAI)
{
    if (!leader || !leaderAI || !leader->IsInWorld() || leaderAI->IsRealPlayer())
        return "{\"ok\":false,\"error\":\"leader_playerbot_unavailable\"}";

    std::size_t rosterTotal = 0;
    bool rosterTruncated = false;
    std::vector<std::uint64_t> const roster = ExactRoster(leader, rosterTotal, rosterTruncated);
    std::vector<MemberRuntime> const members = ResolveMembers(roster);
    bool const membersTruncated = members.size() > kMaxMembers;

    std::string contextSource;
    MapInstance const context = ChooseContext(leader, members, contextSource);
    Player* contextAnchor = ContextAnchor(leader, members, context);
    Map* contextMap = contextAnchor ? contextAnchor->GetMap() : nullptr;
    std::uint32_t const difficulty = contextMap ? static_cast<std::uint32_t>(contextMap->GetDifficulty()) : 0u;

    std::vector<MemberRuntime const*> contextMembers;
    for (MemberRuntime const& runtime : members)
        if (MapInstance{runtime.player->GetMapId(), runtime.player->GetInstanceId()} == context)
            contextMembers.push_back(&runtime);

    std::map<std::uint64_t, Creature*> nearby;
    for (MemberRuntime const* runtime : contextMembers)
    {
        std::list<Creature*> found;
        Acore::AllWorldObjectsInRange check(runtime->player, kEncounterRadius);
        Acore::CreatureListSearcher<Acore::AllWorldObjectsInRange> searcher(runtime->player, found, check);
        Cell::VisitObjects(runtime->player, searcher, kEncounterRadius);
        for (Creature* creature : found)
            if (creature)
                nearby.emplace(GuidCounter(creature->GetGUID()), creature);
    }

    std::vector<CandidateSignal> signals;
    std::map<std::uint64_t, ThreatAggregate> threatByCreature;
    signals.reserve(nearby.size());
    for (auto const& [guid, creature] : nearby)
    {
        if (!creature || !creature->IsInWorld() || creature->IsDuringRemoveFromWorld() || !creature->IsAlive())
            continue;

        float minimumDistance = std::numeric_limits<float>::infinity();
        bool hostile = false;
        std::vector<MemberEngagementSignal> memberSignals;
        memberSignals.reserve(contextMembers.size());
        for (MemberRuntime const* runtime : contextMembers)
        {
            Player* member = runtime->player;
            minimumDistance = std::min(minimumDistance, member->GetDistance(creature));
            hostile = hostile || member->IsHostileTo(creature) || creature->IsHostileTo(member);

            bool const combatLink = creature->IsInCombatWith(member) || member->IsInCombatWith(creature);
            bool const victimLink = creature->GetVictim() == member || member->GetVictim() == creature;
            bool const threatens = creature->GetThreatMgr().IsThreatenedBy(member);
            bool const targetsCreature = runtime->target == creature || member->GetTarget() == creature->GetGUID();
            bool const targets = targetsCreature && (combatLink || member->IsInCombat() || creature->IsInCombat());
            memberSignals.push_back(
                {GuidCounter(member->GetGUID()), threatens, targets, victimLink, combatLink});
        }

        ThreatAggregate const aggregate = AggregateThreat(std::move(memberSignals));
        CreatureTemplate const* creatureTemplate = creature->GetCreatureTemplate();
        bool const boss = creature->isWorldBoss() || creature->IsDungeonBoss() ||
                          (creatureTemplate && creatureTemplate->rank == CREATURE_ELITE_WORLDBOSS);
        bool const engaged = aggregate.involvedMembers > 0;
        signals.push_back({guid, true, true, hostile || engaged, boss, engaged, minimumDistance});
        threatByCreature.emplace(guid, aggregate);
    }

    CandidateSelection const selected = SelectCandidates(std::move(signals));

    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "{\"ok\":true,\"schema\":" << JsonString(kSchema)
        << ",\"version\":" << kVersion
        << ",\"leader_guid\":" << GuidCounter(leader->GetGUID())
        << ",\"map_id\":" << context.first
        << ",\"instance_id\":" << context.second
        << ",\"difficulty\":" << difficulty
        << ",\"timestamp_unix\":" << GameTime::GetGameTime().count()
        << ",\"server_tick_ms\":" << GameTime::GetGameTimeMS().count()
        << ",\"context\":{\"source\":" << JsonString(contextSource)
        << ",\"radius_yards\":" << kEncounterRadius
        << ",\"same_context_online_playerbots\":" << contextMembers.size() << "}"
        << ",\"roster\":{\"total\":" << rosterTotal
        << ",\"guids_truncated\":" << JsonBool(rosterTruncated)
        << ",\"guids\":[";
    for (std::size_t index = 0; index < roster.size(); ++index)
    {
        if (index)
            out << ',';
        out << roster[index];
    }

    out << "],\"online_playerbots\":" << members.size()
        << ",\"members_truncated\":" << JsonBool(membersTruncated)
        << ",\"members\":[";
    for (std::size_t index = 0; index < members.size(); ++index)
    {
        if (index)
            out << ',';
        AppendMember(out, members[index], context);
    }

    out << "]},\"encounter_units\":{\"eligible_before_cap\":" << selected.eligibleBeforeCap
        << ",\"count\":" << selected.candidates.size()
        << ",\"cap\":" << kMaxEncounterUnits
        << ",\"truncated\":" << JsonBool(selected.truncated)
        << ",\"units\":[";
    bool firstEncounterUnit = true;
    for (CandidateSignal const& signal : selected.candidates)
    {
        auto const creature = nearby.find(signal.guid);
        auto const threat = threatByCreature.find(signal.guid);
        if (creature == nearby.end() || threat == threatByCreature.end())
            continue;
        if (!firstEncounterUnit)
            out << ',';
        firstEncounterUnit = false;
        AppendEncounterUnit(out, creature->second, signal, threat->second);
    }
    out << "]}}";
    return out.str();
}
}  // namespace AutoWowEncounterTelemetry
