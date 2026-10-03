/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// AutoWow.Squad runtime (policy: AutoWow/SquadPolicy.h): the source index, the world-thread team tick (squad party,
// demand, stint start / end) and the members' New-RPG step (walk together to the anchor, hunt / gather there).

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "AutoWowGuildsPolicy.h"
#include "AutoWowOracleRuntime.h"
#include "AutoWowQuestLedger.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "DeathLoopBreaker.h"
#include "ErrandsPolicy.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Log.h"
#include "NewRpgBaseAction.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PartyPolicy.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SquadPolicy.h"
#include "SupplyPolicy.h"
#include "StringFormat.h"
#include "Timer.h"
#include "ZoneProgressionPolicy.h"

namespace AutoWowSquad
{
namespace
{
inline constexpr std::uint32_t kMaxStuck = 8;      // the leader's stuck walk ticks before the stint ends
inline constexpr std::uint32_t kMaxPickTries = 16;  // ranked clusters checked (cooldown / danger / water / zone)
inline constexpr std::uint32_t kNodeMinChance = 50;  // gameobject loot rows at least this likely make a node source

struct MemberState
{
    bool hunting = false;         // inside the leash at the last step (Hunt)
    std::uint32_t stuck = 0;      // stuck walk ticks this stint
    std::uint64_t nextHoldLogMs = 0;
    std::uint64_t benchUntilMs = 0;  // AutoWow.Squad.LevelWindow (2)/(5): sits stints out until then (world tick)
    bool benchEscape = false;        // (5): one escape trip pending for the new bench (member step takes it)
};

// Read-only after LoadConfig. Legacy mode keeps the original two rosters/states. Material-crews mode owns
// eight fixed team/kind slots and exposes only their sorted team unions through Roster().
bool gMaterialCrews = false;
std::array<std::vector<std::uint32_t>, kTeamCount> gRoster;  // team union; legacy roster when the opt-in is off
std::unordered_map<std::uint32_t, std::size_t> gTeamOf;      // legacy roster guid -> team index
std::array<std::vector<std::uint32_t>, kCrewCount> gCrewRoster;
std::array<std::vector<std::uint32_t>, kTeamCount> gLegacyRoster;  // opt-in migration provenance only
std::array<bool, kTeamCount> gTransitionPending = {};
std::unordered_map<std::uint32_t, std::size_t> gCrewOf;  // material crew guid -> fixed crew slot
// item -> map -> source spawns (spawn id ascending): creatures (cloth, leather) or nodes (herb, ore; level 1..1).
std::unordered_map<std::uint32_t, std::unordered_map<std::uint32_t, std::vector<Spawn>>> gIndex;
std::unordered_map<std::uint32_t, std::uint32_t> gNodeReq;  // node gameobject entry -> lock skill value

// The world thread writes the team state, members' map threads read copies and write their member state.
std::mutex gLock;
std::array<TeamState, kTeamCount> gTeams;
std::array<TeamState, kCrewCount> gCrewStates;
std::unordered_map<std::uint32_t, MemberState> gMembers;  // roster guids only (never grows after LoadConfig)
std::uint32_t gNextId = 0;  // process-scoped; never reset on config reload and never wraps

// World thread only.
std::uint32_t gTickAcc = 0;
bool gFirstTick = true;

std::uint64_t NowMs() { return static_cast<std::uint64_t>(std::max<int64>(0, GameTime::GetGameTimeMS().count())); }
std::uint32_t Low(Player* p) { return static_cast<std::uint32_t>(p->GetGUID().GetCounter()); }
Player* Online(std::uint32_t guid)
{
    return ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
}

void Emit(Player* who, Reason r, TeamState const& s, Kind kind, std::uint32_t item, std::uint32_t count,
          std::uint64_t now, char const* cause = "", std::string const& extra = std::string())
{
    LOG_INFO("playerbots", "[Squad] {} player={} sid={} kind={} item={} count={} target={} gathered={} map={} "
             "anchor=({},{}) spawn={}{}{}", ReasonName(r), who ? who->GetName() : "-", s.id, KindName(kind), item,
             count, s.target, s.gathered, s.map, s.x, s.y, s.anchorSpawn, *cause ? " cause=" : "", cause);
    if (who && AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::EmitSquad(who, ReasonName(r), LedgerFields(s, kind, item, count, now, cause) + extra);
}

std::string ItemList(bool (*pick)(Kind))
{
    std::string out;
    for (Material const& m : kMaterials)
        if (pick(m.kind))
            out += (out.empty() ? "" : ",") + std::to_string(m.item);
    return out;
}

// Rows (entry, item) of `sql` into out[entry].
void Collect(std::string const& sql, std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>& out)
{
    if (QueryResult result = WorldDatabase.Query(sql))
        do
        {
            Field* f = result->Fetch();
            out[f[0].Get<uint32>()].push_back(f[1].Get<uint32>());
        } while (result->NextRow());
}

// Herbalism / mining lock skill of a node template; false for any other lock (chests, quest objects).
bool NodeReq(GameObjectTemplate const* t, std::uint32_t& req)
{
    LockEntry const* lock = t ? sLockStore.LookupEntry(t->GetLockId()) : nullptr;
    if (!lock)
        return false;
    for (std::size_t j = 0; j < MAX_LOCK_CASE; ++j)
        if (lock->Type[j] == LOCK_KEY_SKILL && (lock->Index[j] == LOCKTYPE_HERBALISM || lock->Index[j] == LOCKTYPE_MINING))
        {
            req = lock->Skill[j];
            return true;
        }
    return false;
}

// Source index (world init): creature entries whose loot (cloth) or skinning loot (leather) holds a material, direct
// rows and one reference level, at least MinDropPct (grouped rows count); node templates whose loot holds a herb /
// ore under a herbalism / mining lock. Then their spawns on the random-bot maps (creatures through the contracts
// spawn filter).
void BuildIndex()
{
    uint32 const start = getMSTime();
    std::uint32_t const pct = detail::gParams.minDropPct;
    std::string const cloth = ItemList([](Kind k) { return k == Kind::Cloth; });
    std::string const leather = ItemList([](Kind k) { return k == Kind::Leather; });
    std::string const nodes = ItemList([](Kind k) { return NodeKind(k); });
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> creatureItems, nodeItems;
    for (auto const& [table, column, items] : {std::array<std::string, 3>{"creature_loot_template", "lootid", cloth},
                                               std::array<std::string, 3>{"skinning_loot_template", "skinloot", leather}})
    {
        Collect(Acore::StringFormat("SELECT ct.entry, l.Item FROM creature_template ct JOIN {} l ON l.Entry = ct.{} "
                                    "WHERE ct.{} <> 0 AND l.Reference = 0 AND l.QuestRequired = 0 AND l.Item IN ({}) "
                                    "AND (l.Chance >= {} OR l.GroupId > 0)", table, column, column, items, pct),
                creatureItems);
        Collect(Acore::StringFormat("SELECT ct.entry, r.Item FROM creature_template ct JOIN {} l ON l.Entry = ct.{} "
                                    "JOIN reference_loot_template r ON r.Entry = l.Reference WHERE ct.{} <> 0 AND "
                                    "l.Reference <> 0 AND r.QuestRequired = 0 AND r.Item IN ({}) AND (r.Chance >= {} OR "
                                    "r.GroupId > 0)", table, column, column, items, pct),
                creatureItems);
    }
    Collect(Acore::StringFormat("SELECT gt.entry, l.Item FROM gameobject_template gt JOIN gameobject_loot_template l ON "
                                "l.Entry = gt.Data1 WHERE gt.type = 3 AND gt.Data1 <> 0 AND l.Reference = 0 AND "
                                "l.Item IN ({}) AND l.Chance >= {}", nodes, kNodeMinChance),
            nodeItems);
    for (auto* m : {&creatureItems, &nodeItems})
        for (auto& [entry, items] : *m)
        {
            std::sort(items.begin(), items.end());
            items.erase(std::unique(items.begin(), items.end()), items.end());
        }
    for (auto const& [entry, items] : nodeItems)
        if (std::uint32_t req = 0; NodeReq(sObjectMgr->GetGameObjectTemplate(entry), req))
            gNodeReq[entry] = req;

    std::size_t creatures = 0, nodeSpawns = 0;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        auto const it = creatureItems.find(data.id);
        Spawn s;
        if (it == creatureItems.end() || !AutoWowContracts::HuntSpawn(static_cast<std::uint32_t>(spawnId), data, s))
            continue;
        for (std::uint32_t const item : it->second)
            gIndex[item][data.mapid].push_back(s);
        ++creatures;
    }
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        auto const it = nodeItems.find(data.id);
        if (it == nodeItems.end() || !gNodeReq.count(data.id) ||
            std::find(sPlayerbotAIConfig.randomBotMaps.begin(), sPlayerbotAIConfig.randomBotMaps.end(), data.mapid) ==
                sPlayerbotAIConfig.randomBotMaps.end())
            continue;
        Spawn s;
        s.spawnId = static_cast<std::uint32_t>(spawnId);
        s.entry = data.id;
        s.x = static_cast<std::int32_t>(data.posX);
        s.y = static_cast<std::int32_t>(data.posY);
        s.z = static_cast<std::int32_t>(data.posZ);
        s.minLevel = s.maxLevel = 1;  // nodes: no level; the anchor zone bracket is checked at pick
        s.teams = AutoWowContracts::kAlliance | AutoWowContracts::kHorde;
        for (std::uint32_t const item : it->second)
            gIndex[item][data.mapid].push_back(s);
        ++nodeSpawns;
    }
    for (auto& [item, maps] : gIndex)
        for (auto& [map, spawns] : maps)
            std::sort(spawns.begin(), spawns.end(), [](Spawn const& a, Spawn const& b) { return a.spawnId < b.spawnId; });
    std::string perItem;
    for (Material const& m : kMaterials)
    {
        std::size_t n = 0;
        if (auto const it = gIndex.find(m.item); it != gIndex.end())
            for (auto const& [map, spawns] : it->second)
                n += spawns.size();
        perItem += " " + std::to_string(m.item) + "=" + std::to_string(n);
    }
    LOG_INFO("server.loading", "[Squad] source index: {} creature entries, {} node entries ({} with a gather lock), {} "
             "creature spawns, {} node spawns in {} ms; spawns per item:{}", creatureItems.size(), nodeItems.size(),
             gNodeReq.size(), creatures, nodeSpawns, GetMSTimeDiffToNow(start), perItem);
}

bool HasTool(Player* p, std::uint32_t const* first, std::uint32_t const* last)
{
    return std::any_of(first, last, [p](std::uint32_t item) { return p->HasItemCount(item, 1); });
}

std::uint32_t Held(std::vector<Player*> const& members, std::uint32_t item)
{
    std::uint32_t n = 0;
    for (Player* m : members)
        n += m->GetItemCount(item);  // bags, not the bank
    return n;
}

void ResetMembers(std::vector<std::uint32_t> const& roster)
{
    std::lock_guard<std::mutex> guard(gLock);
    for (std::uint32_t const g : roster)
        if (auto const it = gMembers.find(g); it != gMembers.end())
        {
            it->second.hunting = false;
            it->second.stuck = 0;
        }
}

bool Benched(std::uint32_t guid)
{
    std::uint64_t const now = NowMs();
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gMembers.find(guid);
    return it != gMembers.end() && BenchActive(it->second.benchUntilMs, now);
}

// (5): consumes the pending escape trip of a new bench.
bool TakeBenchEscape(std::uint32_t guid)
{
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gMembers.find(guid);
    if (it == gMembers.end() || !it->second.benchEscape)
        return false;
    it->second.benchEscape = false;
    return true;
}

// AutoWow.Squad.LevelWindow: the straight line (x, y) -> (ax, ay) on `map` crosses a zone bracketed more than
// LevelWindow above `level` (the zone at (x, y) excepted; HardEscape's SegmentCrossesDanger sampling).
bool CrossesHighZone(Map* map, std::uint32_t level, std::int32_t x, std::int32_t y, std::int32_t ax, std::int32_t ay)
{
    std::uint32_t const window = detail::gParams.levelWindow;
    return AutoWowZoneProgression::SegmentCrossesDanger(
        x, y, ax, ay, AutoWowZoneProgression::kDangerStepYards, AutoWowDeathLoop::ZoneAt(map, float(x), float(y)),
        [map](std::int32_t sx, std::int32_t sy) { return AutoWowDeathLoop::ZoneAt(map, float(sx), float(sy)); },
        [level, window](std::uint32_t z) { return ZoneTooHigh(AutoWowDeathLoop::ZoneMinLevel(z), level, window); });
}

// The nearest workable, level-safe source of the top demanded material the squad can work; true = issued.
bool Search(TeamState& s, std::vector<Want> const& demand, std::vector<Player*> const& members, Player* leader,
            std::uint32_t avg, Skills const& skills, bool alliance, std::uint64_t now)
{
    Params const& p = detail::gParams;
    Map* map = leader->GetMap();
    if (!map || map->Instanceable() || !leader->IsInWorld() || !leader->IsAlive() || leader->IsInFlight())
        return false;
    std::uint32_t const mapId = leader->GetMapId(), leaderGuid = Low(leader);
    std::int32_t const bx = static_cast<std::int32_t>(leader->GetPositionX());
    std::int32_t const by = static_cast<std::int32_t>(leader->GetPositionY());
    std::uint8_t const team = alliance ? AutoWowContracts::kAlliance : AutoWowContracts::kHorde;
    AutoWowContracts::Params const cp = ClusterParams(p);
    uint32 const phaseMask = leader->GetPhaseMask();
    float const height = leader->GetCollisionHeight();
    for (Want const& w : demand)
    {
        auto const item = gIndex.find(w.item);
        if (item == gIndex.end())
            continue;
        auto const onMap = item->second.find(mapId);
        if (onMap == item->second.end())
            continue;
        std::vector<Spawn> const work = Workable(onMap->second, w.kind, skills, [](std::uint32_t entry)
            {
                auto const it = gNodeReq.find(entry);
                return it == gNodeReq.end() ? ~0u : it->second;
            });
        std::vector<Cluster> const ranked =
            RankNearest(AutoWowContracts::RankClusters(cp, AutoWowContracts::Candidates(cp, work, team, avg, bx, by), bx, by));
        std::uint32_t tries = 0;
        Cluster const* pick = AutoWowContracts::PickCluster(ranked,
            [&](Cluster const& c)
            {
                if (++tries > kMaxPickTries || AnchorCooling(p, s, mapId, c.x, c.y, now) ||
                    AutoWowDeathLoop::IsDangerous(leaderGuid, mapId, float(c.x), float(c.y)))
                    return true;
                // ponytail: water checked on the anchor only (as contracts); per-spawn liquid flags if water
                // sources keep getting picked.
                if (map->IsInWater(phaseMask, float(c.x), float(c.y), float(c.z), height))
                    return true;
                std::uint32_t const zone = AutoWowDeathLoop::ZoneAt(map, float(c.x), float(c.y));
                if (ZoneTooHigh(AutoWowDeathLoop::ZoneMinLevel(zone), avg, ZoneMargin(p)))
                    return true;
                // AutoWow.Squad.LevelWindow (1): no cooling death-cluster zone, no over-level zone on the leader's line.
                return LevelWindowOn(p) && (ZoneCooling(s, zone, now) || CrossesHighZone(map, avg, bx, by, c.x, c.y));
            });
        if (!pick)
            continue;
        std::uint32_t id = 0;
        {
            std::lock_guard<std::mutex> guard(gLock);
            if (!NextStintId(gNextId, id))
            {
                LOG_ERROR("server.loading", "[Squad] stint id exhausted; refusing new stints");
                return false;
            }
        }
        Issue(s, w, *pick, mapId, leaderGuid, id, Held(members, w.item), now);
        s.anchorZone = AutoWowDeathLoop::ZoneAt(map, float(pick->x), float(pick->y));
        Emit(leader, Reason::Stint, s, w.kind, w.item, w.count, now, "",
             ",\"avg_level\":" + std::to_string(avg) + ",\"spawns\":" + std::to_string(pick->spawns) +
                 ",\"members\":" + std::to_string(members.size()));
        return true;
    }
    return false;
}

// One world tick of a team's squad.
void TeamTick(std::size_t slot, std::uint64_t now)
{
    Params const& p = detail::gParams;
    bool const alliance = (gMaterialCrews ? CrewTeam(slot) : slot) == 0;
    std::vector<std::uint32_t> const& roster = gMaterialCrews ? gCrewRoster[slot] : gRoster[slot];
    AutoWowParty::EnsureSquad(roster);

    std::vector<Player*> members;  // online, guid ascending
    for (std::uint32_t const g : roster)
        if (Player* m = Online(g); m && m->IsInWorld())
            members.push_back(m);
    TeamState s;
    std::uint32_t leaderStuck = 0;
    std::vector<std::uint64_t> benchUntil(members.size(), 0);  // AutoWow.Squad.LevelWindow (5)
    {
        std::lock_guard<std::mutex> guard(gLock);
        s = gMaterialCrews ? gCrewStates[slot] : gTeams[slot];
        if (auto const it = gMembers.find(s.leader); it != gMembers.end())
            leaderStuck = it->second.stuck;
        for (std::size_t k = 0; k < members.size(); ++k)
            if (auto const it = gMembers.find(Low(members[k])); it != gMembers.end())
                benchUntil[k] = it->second.benchUntilMs;
    }
    Player* leader = members.empty() ? nullptr : members.front();
    // AutoWow.Squad.LevelWindow (5): the lowest unbenched member leads (anchors are searched around it).
    for (std::size_t k = 0; LevelWindowOn(p) && k < members.size(); ++k)
        if (!BenchActive(benchUntil[k], now))
        {
            leader = members[k];
            break;
        }

    std::vector<Want> const demand =
        leader && AutoWowSupply::Enabled()
            ? (gMaterialCrews ? RankDemandForKind(AutoWowSupply::MaterialDemand(alliance), p.minDemand, CrewKind(slot))
                              : RankDemand(AutoWowSupply::MaterialDemand(alliance), p.minDemand))
            : std::vector<Want>{};
    std::vector<std::uint32_t> levels;
    Skills skills;
    skills.anySkill = sPlayerbotAIConfig.autoWowGatherAnySkill;
    std::vector<std::array<std::int32_t, 2>> spread;
    bool onAnchorMap = false;
    std::vector<char const*> bench;  // AutoWow.Squad.LevelWindow (2), per member ("" = takes part)
    bool anchorCluster = false;      // AutoWow.Squad.LevelWindow (4)
    std::size_t benched = 0;         // (5): held benches + new ones
    for (Player* m : members)
    {
        bool const held = BenchActive(benchUntil[bench.size()], now);  // (5): no re-evaluation while benched
        char const* cause = "";
        if (LevelWindowOn(p) && s.phase == Phase::Stint && m->GetMapId() == s.map)
        {
            std::uint32_t const deaths = AutoWowDeathLoop::RecentDeaths(Low(m));
            std::int32_t const mx = static_cast<std::int32_t>(m->GetPositionX());
            std::int32_t const my = static_cast<std::int32_t>(m->GetPositionY());
            anchorCluster = anchorCluster || AnchorDeathCluster(p, s, m->GetMapId(), mx, my, deaths);
            if (!held)
                cause = BenchCause(p, CrossesHighZone(m->GetMap(), m->GetLevel(), mx, my, s.x, s.y), deaths);
        }
        bench.push_back(cause);
        bool const out = held || *cause;
        benched += out ? 1 : 0;
        levels.push_back(m->GetLevel());
        if (m->HasSkill(SKILL_HERBALISM))
            skills.herbalism = std::max<std::uint32_t>(skills.herbalism, m->GetSkillValue(SKILL_HERBALISM));
        if (m->HasSkill(SKILL_MINING) && HasTool(m, std::begin(kMiningPicks), std::end(kMiningPicks)))
            skills.mining = std::max<std::uint32_t>(skills.mining, m->GetSkillValue(SKILL_MINING));
        if (m->HasSkill(SKILL_SKINNING) && HasTool(m, std::begin(kSkinningKnives), std::end(kSkinningKnives)))
            skills.skinning = std::max<std::uint32_t>(skills.skinning, m->GetSkillValue(SKILL_SKINNING));
        if (m != leader && !out && m->IsAlive() && m->GetMapId() == leader->GetMapId())
            spread.push_back({static_cast<std::int32_t>(m->GetPositionX()), static_cast<std::int32_t>(m->GetPositionY())});
        onAnchorMap = onAnchorMap || (!out && m->GetMapId() == s.map);
    }
    if (LevelWindowOn(p))
    {
        std::vector<std::size_t> newly;
        {
            std::lock_guard<std::mutex> guard(gLock);
            for (std::size_t k = 0; k < members.size(); ++k)
                if (auto const it = gMembers.find(Low(members[k])); it != gMembers.end())
                    if (*bench[k])
                    {
                        newly.push_back(k);
                        it->second.benchUntilMs = BenchUntil(p, now);
                        it->second.benchEscape = true;
                    }
        }
        for (std::size_t const k : newly)
            Emit(members[k], Reason::Bench, s, s.kind, s.item, members[k]->GetLevel(), now, bench[k]);
    }
    std::uint32_t const avg = AvgLevel(levels);
    if (leader)
    {
        s.leader = Low(leader);
        std::int32_t const lx = static_cast<std::int32_t>(leader->GetPositionX());
        std::int32_t const ly = static_cast<std::int32_t>(leader->GetPositionY());
        s.together = s.phase != Phase::Stint || Together(p, lx, ly, s.x, s.y, spread);
    }

    if (s.phase == Phase::Stint)
    {
        NoteHeld(s, Held(members, s.item));
        bool const danger = AutoWowDeathLoop::IsDangerous(s.leader, s.map, float(s.x), float(s.y)) || anchorCluster;
        char const* cause = EndCause(p, s, now, Demanded(demand, s.item), onAnchorMap, danger, leaderStuck > kMaxStuck);
        if (!*cause && TooManyBenched(p, benched, members.size()))
            cause = "benched";  // AutoWow.Squad.LevelWindow (5)
        if (*cause)
        {
            Emit(leader, Reason::Gather, s, s.kind, s.item, s.gathered, now, cause);
            if (anchorCluster && std::string_view(cause) == "danger")
                CoolZone(s, p, s.anchorZone, now);  // AutoWow.Squad.LevelWindow (4): the squad leaves the zone
            Finish(s, p, now);
            ResetMembers(roster);
        }
    }
    if (s.phase == Phase::None && leader && TooManyBenched(p, benched, members.size()))
    {
        // AutoWow.Squad.LevelWindow (5): no stint while most of the squad sits out; the members quest.
        if (s.holding)
            Emit(leader, Reason::Release, s, gMaterialCrews ? CrewKind(slot) : Kind::Cloth, 0,
                 static_cast<std::uint32_t>(benched), now, "benched");
        s.holding = false;
    }
    else if (s.phase == Phase::None && now >= s.nextSearchMs && leader)
    {
        s.nextSearchMs = now + p.searchRetryMs;
        bool const had = s.holding;
        s.holding = false;
        if (!Search(s, demand, members, leader, avg, skills, alliance, now) && had)
            Emit(leader, Reason::Release, s, gMaterialCrews ? CrewKind(slot) : Kind::Cloth, 0,
                 static_cast<std::uint32_t>(demand.size()), now, demand.empty() ? "no_demand" : "no_source",
                 ",\"avg_level\":" + std::to_string(avg));
    }
    std::lock_guard<std::mutex> guard(gLock);
    if (gMaterialCrews)
        gCrewStates[slot] = s;
    else
        gTeams[slot] = s;
}

bool Hunting(std::uint32_t guid)
{
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gMembers.find(guid);
    return it != gMembers.end() && it->second.hunting;
}

void NoteStep(std::uint32_t guid, bool hunting, bool stuck)
{
    std::lock_guard<std::mutex> guard(gLock);
    if (auto const it = gMembers.find(guid); it != gMembers.end())
    {
        it->second.hunting = hunting;
        it->second.stuck += stuck ? 1 : 0;
    }
}
}  // namespace

void LoadConfig()
{
    detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Squad.Enable", false);
    gMaterialCrews = sConfigMgr->GetOption<bool>("AutoWow.Squad.MaterialCrews", false);
    Params& p = detail::gParams;
    p.tickMs = std::max<std::uint32_t>(1000, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Squad.TickMs", 30000));
    p.stintMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Squad.StintMs", 1800000);
    p.levelAbove = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Squad.LevelAbove", 1);
    p.levelBelow = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Squad.LevelBelow", 255);
    p.zoneLevelMargin = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Squad.ZoneLevelMargin", 3);
    p.searchYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Squad.SearchYards", 2500);
    p.clusterYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Squad.ClusterYards", 120);
    p.minClusterSpawns = std::max<std::uint32_t>(1, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Squad.MinClusterSpawns", 5));
    p.leashYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Squad.LeashYards", 150);
    p.togetherYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Squad.TogetherYards", 60);
    p.minDemand = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Squad.MinDemand", 5);
    p.minDropPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Squad.MinDropPct", 5);
    p.cooldownMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Squad.CooldownMs", 600000);
    p.searchRetryMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Squad.SearchRetryMs", 60000);
    p.levelWindow = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Squad.LevelWindow", 0);
    p.deathCluster = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Squad.DeathCluster", 3);
    p.dangerZoneMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Squad.DangerZoneMs", 3600000);
    p.benchMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Squad.BenchMs", 1800000);
    for (auto& r : gRoster)
        r.clear();
    for (auto& r : gCrewRoster)
        r.clear();
    for (auto& r : gLegacyRoster)
        r.clear();
    gTransitionPending = {};
    gTeamOf.clear();
    gCrewOf.clear();
    gIndex.clear();
    gNodeReq.clear();
    gTickAcc = 0;
    gFirstTick = true;
    {
        std::lock_guard<std::mutex> guard(gLock);
        gTeams = {};
        gCrewStates = {};
        gMembers.clear();
    }
    if (!detail::gEnabled)
        return;

    if (!gMaterialCrews)
    {
        // Preserve the legacy parser and duplicate handling exactly when the opt-in is absent/off.
        for (std::size_t t = 0; t < kTeamCount; ++t)
        {
            std::string const key = t == 0 ? "AutoWow.Squad.Alliance" : "AutoWow.Squad.Horde";
            std::string const text = sConfigMgr->GetOption<std::string>(key, "");
            std::vector<AutoWowGuilds::GuidRange> ranges;
            if (text.empty())
                continue;
            if (!AutoWowGuilds::ParseGuidRanges(text, ranges))
            {
                LOG_ERROR("server.loading", "[Squad] bad {} '{}': no roster", key, text);
                continue;
            }
            std::vector<std::uint32_t>& roster = gRoster[t];
            for (AutoWowGuilds::GuidRange const& r : ranges)
                for (std::uint64_t g = r.lo; g <= r.hi && roster.size() < kMaxMembers; ++g)
                    if (!gTeamOf.count(static_cast<std::uint32_t>(g)))
                        roster.push_back(static_cast<std::uint32_t>(g));
            std::sort(roster.begin(), roster.end());
            roster.erase(std::unique(roster.begin(), roster.end()), roster.end());
            for (std::uint32_t const g : roster)
            {
                gTeamOf[g] = t;
                std::lock_guard<std::mutex> guard(gLock);
                gMembers[g] = MemberState{};
            }
        }
    }
    else
    {
        std::array<std::vector<std::uint32_t>, kCrewCount> configured;
        bool parsed = true;
        // The old keys are provenance for retiring only the exact persisted legacy squad during this split.
        for (std::size_t team = 0; team < kTeamCount; ++team)
        {
            std::string const key = team == 0 ? "AutoWow.Squad.Alliance" : "AutoWow.Squad.Horde";
            std::string const text = sConfigMgr->GetOption<std::string>(key, "");
            std::vector<AutoWowGuilds::GuidRange> ranges;
            if (text.empty())
                continue;
            if (!AutoWowGuilds::ParseGuidRanges(text, ranges))
            {
                LOG_ERROR("server.loading", "[Squad] material crews rejected: bad legacy provenance {} '{}'", key,
                          text);
                parsed = false;
                continue;
            }
            std::vector<GuidSpan> spans;
            for (AutoWowGuilds::GuidRange const& range : ranges)
                spans.push_back(GuidSpan{range.lo, range.hi});
            std::vector<std::uint32_t> legacy;
            if (!ExpandGuidSpansBounded(spans, kMaxMembers, legacy))
            {
                LOG_ERROR("server.loading", "[Squad] material crews rejected: legacy provenance {} exceeds {} members",
                          key, kMaxMembers);
                parsed = false;
                continue;
            }
            gLegacyRoster[team] = legacy;
        }
        for (std::size_t crew = 0; crew < kCrewCount; ++crew)
        {
            std::string const team = CrewTeam(crew) == 0 ? "Alliance" : "Horde";
            Kind const kind = CrewKind(crew);
            char const* const kindKey = kind == Kind::Cloth  ? "Cloth"
                                        : kind == Kind::Herb ? "Herb"
                                        : kind == Kind::Ore  ? "Ore"
                                                             : "Leather";
            std::string const key = "AutoWow.Squad." + team + "." + kindKey;
            std::string const text = sConfigMgr->GetOption<std::string>(key, "");
            if (text.empty())
                continue;
            std::vector<AutoWowGuilds::GuidRange> ranges;
            if (!AutoWowGuilds::ParseGuidRanges(text, ranges))
            {
                LOG_ERROR("server.loading", "[Squad] material crews rejected: bad {} '{}'", key, text);
                parsed = false;
                continue;
            }
            for (AutoWowGuilds::GuidRange const& range : ranges)
            {
                std::uint64_t const count = range.hi >= range.lo ? static_cast<std::uint64_t>(range.hi) -
                                                                       static_cast<std::uint64_t>(range.lo) + 1
                                                                 : 0;
                if (!count || count > kMaxCrewMembers || configured[crew].size() + count > kMaxCrewMembers)
                {
                    LOG_ERROR("server.loading", "[Squad] material crews rejected: {} exceeds {} members", key,
                              kMaxCrewMembers);
                    parsed = false;
                    break;
                }
                for (std::uint64_t guid = range.lo; guid <= range.hi; ++guid)
                    configured[crew].push_back(static_cast<std::uint32_t>(guid));
            }
        }
        CrewLayout const layout = parsed ? BuildCrewLayout(configured) : CrewLayout{};
        if (!parsed || !layout.Valid())
        {
            LOG_ERROR("server.loading", "[Squad] disabled: invalid material-crew layout error={}",
                      parsed ? static_cast<unsigned>(layout.error) : 255u);
            detail::gEnabled = false;
            return;
        }
        gCrewRoster = layout.crews;
        gRoster = layout.teams;
        for (std::size_t team = 0; team < kTeamCount; ++team)
        {
            if (!gLegacyRoster[team].empty() && !AutoWowParty::SameGuidSet(gLegacyRoster[team], gRoster[team]))
            {
                LOG_ERROR("server.loading", "[Squad] disabled: material crew union does not match legacy {} roster",
                          team == 0 ? "Alliance" : "Horde");
                detail::gEnabled = false;
                return;
            }
            std::vector<std::vector<std::uint32_t>> successors;
            for (std::size_t kind = 0; kind < kKindCount; ++kind)
                successors.push_back(gCrewRoster[team * kKindCount + kind]);
            gTransitionPending[team] = AutoWowParty::SplitSquadPlanMatches(gLegacyRoster[team], successors);
        }
        for (std::size_t crew = 0; crew < kCrewCount; ++crew)
            for (std::uint32_t const guid : gCrewRoster[crew])
                gCrewOf[guid] = crew;
        {
            std::lock_guard<std::mutex> guard(gLock);
            for (auto const& team : gRoster)
                for (std::uint32_t const guid : team)
                    gMembers[guid] = MemberState{};
        }
        for (std::size_t crew = 0; crew < kCrewCount; ++crew)
            if (!gCrewRoster[crew].empty())
            {
                std::string roster;
                for (std::uint32_t const guid : gCrewRoster[crew])
                    roster += (roster.empty() ? "" : ",") + std::to_string(guid);
                LOG_INFO("server.loading", "[Squad] material crew team={} kind={} roster={}",
                         CrewTeam(crew) == 0 ? "alliance" : "horde", KindName(CrewKind(crew)), roster);
            }
    }
    if (gRoster[0].empty() && gRoster[1].empty())
    {
        LOG_ERROR("server.loading", "[Squad] disabled: configured layout has no guids");
        detail::gEnabled = false;
        return;
    }
    if (!AutoWowSupply::Enabled())
        LOG_ERROR("server.loading", "[Squad] AutoWow.Supply is off: no material demand, the squad only quests");
    BuildIndex();
    if (gMaterialCrews)
        LOG_INFO("server.loading",
                 "[Squad] enabled: mode=material_crews alliance={} horde={} stint_ms={} level_above={} "
                 "search_yards={} leash={} min_demand={} level_window={} death_cluster={}",
                 gRoster[0].size(), gRoster[1].size(), p.stintMs, p.levelAbove, p.searchYards, p.leashYards,
                 p.minDemand, p.levelWindow, p.deathCluster);
    else
        LOG_INFO("server.loading",
                 "[Squad] enabled: alliance={} horde={} stint_ms={} level_above={} search_yards={} "
                 "leash={} min_demand={} level_window={} death_cluster={}",
                 gRoster[0].size(), gRoster[1].size(), p.stintMs, p.levelAbove, p.searchYards, p.leashYards,
                 p.minDemand, p.levelWindow, p.deathCluster);
}

void WorldUpdate(std::uint32_t diff)
{
    if (!detail::gEnabled)
        return;
    gTickAcc += diff;
    // The first tick runs at once: EnsureSquad registers the rosters before the first cohort party formation.
    if (!gFirstTick && gTickAcc < detail::gParams.tickMs)
        return;
    gFirstTick = false;
    gTickAcc = 0;
    std::uint64_t const now = NowMs();
    if (gMaterialCrews)
    {
        for (std::size_t team = 0; team < kTeamCount; ++team)
        {
            if (gTransitionPending[team])
            {
                std::vector<std::vector<std::uint32_t>> successors;
                for (std::size_t kind = 0; kind < kKindCount; ++kind)
                    successors.push_back(gCrewRoster[team * kKindCount + kind]);
                if (!AutoWowParty::RetireSplitSquad(gLegacyRoster[team], successors))
                    continue;
                gTransitionPending[team] = false;
            }
            for (std::size_t kind = 0; kind < kKindCount; ++kind)
            {
                std::size_t const crew = team * kKindCount + kind;
                if (!gCrewRoster[crew].empty())
                    TeamTick(crew, now);
            }
        }
    }
    else
        for (std::size_t team = 0; team < kTeamCount; ++team)
            if (!gRoster[team].empty())
                TeamTick(team, now);
}

bool IsMember(std::uint32_t guid)
{
    return detail::gEnabled && (gMaterialCrews ? gCrewOf.count(guid) != 0 : gTeamOf.count(guid) != 0);
}
std::vector<std::uint32_t> Roster(bool alliance) { return detail::gEnabled ? gRoster[alliance ? 0 : 1] : std::vector<std::uint32_t>{}; }

TeamState SnapshotOf(std::uint32_t guid)
{
    if (!detail::gEnabled)
        return TeamState{};
    if (gMaterialCrews)
    {
        auto const it = gCrewOf.find(guid);
        if (it == gCrewOf.end())
            return TeamState{};
        std::lock_guard<std::mutex> guard(gLock);
        return gCrewStates[it->second];
    }
    auto const it = gTeamOf.find(guid);
    if (it == gTeamOf.end())
        return TeamState{};
    std::lock_guard<std::mutex> guard(gLock);
    return gTeams[it->second];
}

TeamState SnapshotFor(std::uint32_t guid, std::uint32_t map)
{
    TeamState const s = SnapshotOf(guid);
    return LevelWindowOn(detail::gParams) ? FilterView(detail::gParams, s, map, Benched(guid)) : s;
}

bool HoldsTier(Player* bot)
{
    std::uint32_t const guid = Low(bot);
    if (!IsMember(guid) || (LevelWindowOn(detail::gParams) && Benched(guid)))  // LevelWindow (2): no hold
        return false;
    TeamState const s = SnapshotOf(guid);
    bool const dangerous =
        AutoWowDeathLoop::IsDangerous(guid, bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY()) ||
        AutoWowDeathLoop::ZoneMinLevel(bot->GetZoneId()) > bot->GetLevel() + detail::gParams.zoneLevelMargin;
    if (!Holds(s, dangerous))
        return false;
    std::uint64_t const now = NowMs();
    bool log = false;
    {
        std::lock_guard<std::mutex> guard(gLock);
        if (auto const it = gMembers.find(guid); it != gMembers.end() && now >= it->second.nextHoldLogMs)
        {
            it->second.nextHoldLogMs = now + detail::gParams.holdLogMs;
            log = true;
        }
    }
    if (log)
        Emit(bot, Reason::Hold, s, s.kind, s.item, bot->GetLevel(), now, "",
             ",\"zone\":" + std::to_string(bot->GetZoneId()));
    return true;
}

void NoteDelivered(Player* bot, std::uint32_t item, std::uint32_t count, std::uint32_t rep)
{
    if (!bot || !IsMember(Low(bot)))
        return;
    std::size_t const m = MaterialIndex(item);
    Emit(bot, Reason::Deliver, SnapshotOf(Low(bot)), m == kNoMaterial ? Kind::Cloth : kMaterials[m].kind, item, count,
         NowMs(), "", ",\"to\":" + std::to_string(rep));
}
}  // namespace AutoWowSquad

bool NewRpgBaseAction::SquadStep()
{
    using AutoWowSquad::Step;
    uint32 const guid = bot->GetGUID().GetCounter();
    if (!AutoWowSquad::IsMember(guid) || !bot->IsAlive() || bot->IsInFlight() || !bot->GetMap() ||
        bot->GetMap()->Instanceable() || bot->GetTransport() || AutoWowOracleRuntime::IsManagedBot(guid))
        return false;
    // AutoWow.Squad.LevelWindow (2)/(5): a benched member runs its own loop; once per bench it takes a death-loop
    // escape trip to a level-fitting hub (ZoneProgression), out of the zone it kept dying in.
    if (AutoWowSquad::LevelWindowOn(AutoWowSquad::detail::gParams) && AutoWowSquad::Benched(guid))
    {
        AutoWowSquad::NoteStep(guid, false, false);
        if (AutoWowDeathLoop::EscapeEnabled() && AutoWowZoneProgression::Enabled() && !bot->IsInCombat() &&
            !AutoWowZoneProgression::Active(guid) && !(AutoWowErrands::Enabled() && AutoWowErrands::Active(guid)) &&
            AutoWowSquad::TakeBenchEscape(guid) && DeathLoopEscape())
        {
            LOG_INFO("playerbots", "[Squad] bench escape player={} lvl={} zone={}", bot->GetName(), bot->GetLevel(),
                     bot->GetZoneId());
            return true;
        }
        return false;
    }
    AutoWowSquad::TeamState const s = AutoWowSquad::SnapshotOf(guid);
    if (s.phase != AutoWowSquad::Phase::Stint)
        return false;
    // A zone-progression trip or an errand run goes first (the errand sell stop is the delivery); combat owns the
    // bot (kills and loot happen there).
    if ((AutoWowZoneProgression::Enabled() && AutoWowZoneProgression::Active(guid)) ||
        (AutoWowErrands::Enabled() && AutoWowErrands::Active(guid)) || bot->IsInCombat())
        return false;
    NewRpgInfo& info = botAI->rpgInfo;
    Step const step = AutoWowSquad::MemberStep(AutoWowSquad::detail::gParams, s, guid == s.leader,
                                               AutoWowSquad::Hunting(guid), bot->GetMapId(),
                                               static_cast<std::int32_t>(bot->GetPositionX()),
                                               static_cast<std::int32_t>(bot->GetPositionY()));
    switch (step)
    {
        case Step::Idle:
            AutoWowSquad::NoteStep(guid, false, false);
            return false;
        case Step::Wait:
            // The leader holds until the members are within TogetherYards (world tick sets `together`).
            AutoWowSquad::NoteStep(guid, false, false);
            if (bot->isMoving())
                bot->StopMoving();
            if (info.GetStatus() != RPG_IDLE)
                info.ChangeToIdle();
            return true;
        case Step::Travel:
        {
            // To the anchor (or back inside the leash): the no-teleport long walk, in Idle.
            bool const stuck = WalkLeg(WorldPosition(s.map, float(s.x), float(s.y), float(s.z)));
            AutoWowSquad::NoteStep(guid, false, stuck);
            return true;
        }
        case Step::Hunt:
            // The wander status keeps the RPG machine at the anchor; GrindTargetValue narrows targets to the
            // source, the gathering detour takes nodes, the loot / skinning strategies loot.
            AutoWowSquad::NoteStep(guid, true, false);
            if (info.GetStatus() != RPG_WANDER_RANDOM)
            {
                info.ChangeToWanderRandom();
                return true;
            }
            return false;
    }
    return false;
}

bool NewRpgBaseAction::SquadWander()
{
    uint32 const guid = bot->GetGUID().GetCounter();
    if (!AutoWowSquad::IsMember(guid) || !AutoWowSquad::Hunting(guid))
        return false;
    AutoWowSquad::TeamState const s = AutoWowSquad::SnapshotOf(guid);
    if (s.phase != AutoWowSquad::Phase::Stint || s.map != bot->GetMapId())
        return false;
    Position const anchor(float(s.x), float(s.y), float(s.z));
    return MoveRandomNear(float(AutoWowSquad::detail::gParams.leashYards) / 2, MovementPriority::MOVEMENT_NORMAL,
                          nullptr, &anchor);
}
