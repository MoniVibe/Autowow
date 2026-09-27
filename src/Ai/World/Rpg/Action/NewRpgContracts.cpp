/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// AutoWow.Contracts runtime (policy: AutoWow/ContractsPolicy.h): the spawn index, issue / travel / hunt /
// end of hunt contracts, kill credit.

#include <algorithm>
#include <mutex>
#include <unordered_map>

#include "AutoWowOracleRuntime.h"
#include "AutoWowQuestLedger.h"
#include "Config.h"
#include "ContractsPolicy.h"
#include "DBCStores.h"
#include "DeathLoopBreaker.h"
#include "DungeonProbePolicy.h"
#include "ErrandsPolicy.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "NewRpgBaseAction.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ZoneProgressionPolicy.h"

namespace AutoWowContracts
{
namespace
{
inline constexpr std::uint64_t kSearchRetryMs = 60000;   // no-contract bot: one issue search per minute
inline constexpr std::uint64_t kQuestRecheckMs = 60000;  // hunting bot: quest work recheck stride
inline constexpr std::uint32_t kMaxStuck = 8;            // stuck walk ticks before the contract is abandoned
inline constexpr std::uint32_t kMaxPickTries = 16;       // ranked clusters checked (danger / zone) per search
inline constexpr std::uint32_t kZoneLevelMargin = 3;     // skip anchors in zones bracketed above level + 3

// Read-only after LoadConfig (world init). Per map, spawn id ascending.
std::unordered_map<std::uint32_t, std::vector<Spawn>> gIndex;

// Bot AI updates and kill hooks run on map threads. Touched only with the flag on.
std::mutex gLock;
std::unordered_map<std::uint32_t, BotState> gStates;
std::uint32_t gNextId = 0;

// The teams whose players may attack a creature of this faction template (not friendly to the team's
// player faction: human 1, orc 2). Without loot only the teams it is hostile to: GrindTargetValue skips a
// lootless creature the bot is not hostile with (faction template stands in for the bot's reaction).
std::uint8_t HuntTeams(uint32 faction, bool hasLoot)
{
    FactionTemplateEntry const* f = sFactionTemplateStore.LookupEntry(faction);
    FactionTemplateEntry const* alliance = sFactionTemplateStore.LookupEntry(1);
    FactionTemplateEntry const* horde = sFactionTemplateStore.LookupEntry(2);
    if (!f || !alliance || !horde)
        return 0;
    std::uint8_t teams = 0;
    if (!f->IsFriendlyTo(*alliance) && (hasLoot || f->IsHostileTo(*alliance)))
        teams |= kAlliance;
    if (!f->IsFriendlyTo(*horde) && (hasLoot || f->IsHostileTo(*horde)))
        teams |= kHorde;
    return teams;
}

void BuildIndex()
{
    uint32 const start = getMSTime();
    std::size_t total = 0;
    gIndex.clear();
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        Spawn s;
        if (!HuntSpawn(static_cast<std::uint32_t>(spawnId), data, s))
            continue;
        gIndex[data.mapid].push_back(s);
        ++total;
    }
    for (auto& [map, spawns] : gIndex)
        std::sort(spawns.begin(), spawns.end(), [](Spawn const& a, Spawn const& b) { return a.spawnId < b.spawnId; });
    LOG_INFO("server.loading", "[Contracts] hunt index: {} spawns on {} maps in {} ms", total, gIndex.size(),
             GetMSTimeDiffToNow(start));
}

BotState LoadState(std::uint32_t guid)
{
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gStates.find(guid);
    return it == gStates.end() ? BotState{} : it->second;
}

// Bounded: beyond kMaxTrackedBots a new bot is not stored (it never holds a contract).
bool StoreState(std::uint32_t guid, BotState const& s)
{
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gStates.find(guid);
    if (it != gStates.end())
    {
        it->second = s;
        return true;
    }
    if (gStates.size() >= kMaxTrackedBots)
        return false;
    gStates.emplace(guid, s);
    return true;
}

std::uint32_t NextId()
{
    std::lock_guard<std::mutex> guard(gLock);
    return ++gNextId;
}
}  // namespace

bool HuntSpawn(std::uint32_t spawnId, CreatureData const& data, Spawn& out)
{
    uint32 const serviceFlags = ~uint32(UNIT_NPC_FLAG_GOSSIP);
    uint32 const blockedUnitFlags = UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC;
    if (std::find(sPlayerbotAIConfig.randomBotMaps.begin(), sPlayerbotAIConfig.randomBotMaps.end(), data.mapid) ==
        sPlayerbotAIConfig.randomBotMaps.end())
        return false;
    CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(data.id);
    if (!ct || ct->rank != CREATURE_ELITE_NORMAL || !ct->minlevel || ct->minlevel > ct->maxlevel)
        return false;
    if (ct->type == CREATURE_TYPE_CRITTER || ct->type == CREATURE_TYPE_TOTEM ||
        ct->type == CREATURE_TYPE_NON_COMBAT_PET || ct->type == CREATURE_TYPE_GAS_CLOUD)
        return false;
    if (ct->HasFlagsExtra(CREATURE_FLAG_EXTRA_TRIGGER | CREATURE_FLAG_EXTRA_CIVILIAN))
        return false;
    uint32 npcflag = 0, unitFlags = 0, dynamicFlags = 0;
    ObjectMgr::ChooseCreatureFlags(ct, npcflag, unitFlags, dynamicFlags, &data);  // spawn overrides
    if ((npcflag & serviceFlags) || (unitFlags & blockedUnitFlags))
        return false;
    std::uint8_t const teams = HuntTeams(ct->faction, ct->lootid != 0);
    if (!teams)
        return false;
    out.spawnId = spawnId;
    out.entry = data.id;
    out.x = static_cast<std::int32_t>(data.posX);
    out.y = static_cast<std::int32_t>(data.posY);
    out.z = static_cast<std::int32_t>(data.posZ);
    out.minLevel = ct->minlevel;
    out.maxLevel = ct->maxlevel;
    out.teams = teams;
    return true;
}

void LoadConfig()
{
    detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Contracts.Enable", false);
    Params& p = detail::gParams;
    p.kills = std::max<std::uint32_t>(1, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Contracts.Kills", 15));
    p.levelBelow = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Contracts.LevelBelow", 3);
    p.levelAbove = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Contracts.LevelAbove", 1);
    p.levelGapMin = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Contracts.LevelGapMin", 0);
    p.searchYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Contracts.SearchYards", 700);
    p.clusterYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Contracts.ClusterYards", 120);
    p.leashYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Contracts.LeashYards", 150);
    p.timeoutMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Contracts.TimeoutMs", 1200000);
    p.stallMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Contracts.StallMs", 480000);
    p.cooldownMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Contracts.CooldownMs", 300000);
    p.minClusterSpawns = std::max<std::uint32_t>(1, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Contracts.MinClusterSpawns", 6));
    if (detail::gEnabled)
        BuildIndex();
}

BotState Snapshot(std::uint32_t guid)
{
    if (!Enabled())
        return BotState{};
    return LoadState(guid);
}

void CreditKill(std::uint32_t guid, std::uint32_t entry)
{
    if (!Enabled())
        return;
    std::uint64_t const now = static_cast<std::uint64_t>(std::max<int64>(0, GameTime::GetGameTimeMS().count()));
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gStates.find(guid);
    if (it != gStates.end())
        NoteKill(it->second, entry, now);
}
}  // namespace AutoWowContracts

bool NewRpgBaseAction::ContractStep()
{
    using namespace AutoWowContracts;
    using AutoWowContracts::BotState;  // other AutoWow policies also name these
    using AutoWowContracts::Params;
    using AutoWowContracts::Phase;
    uint32 const guid = bot->GetGUID().GetCounter();
    if (!bot->IsAlive() || bot->IsInFlight() || !botAI->IsAutoWowIndependentParty() ||
        AutoWowOracleRuntime::IsManagedBot(guid) || AutoWowDungeonProbe::IsProbeBot(guid) || !bot->GetMap() ||
        bot->GetMap()->Instanceable() || bot->GetTransport())
        return false;

    Params const& p = detail::gParams;
    std::uint64_t const now = static_cast<std::uint64_t>(std::max<int64>(0, GameTime::GetGameTimeMS().count()));
    BotState s = LoadState(guid);
    NewRpgInfo& info = botAI->rpgInfo;
    std::uint32_t const level = bot->GetLevel();
    std::int32_t const bx = static_cast<std::int32_t>(bot->GetPositionX());
    std::int32_t const by = static_cast<std::int32_t>(bot->GetPositionY());
    // Another trip (zone progression, errand run) or a party owns the bot.
    Group const* group = bot->GetGroup();
    bool const onTrip = AutoWowZoneProgression::Enabled() && AutoWowZoneProgression::Active(guid);
    bool const onErrand = AutoWowErrands::Enabled() && AutoWowErrands::Active(guid);
    bool const inParty = group && group->GetMembersCount() > 1;
    bool const busy = onTrip || onErrand || inParty;

    auto emit = [&](Reason r, char const* cause)
    {
        if (AutoWowQuestLedger::Enabled())
            AutoWowQuestLedger::EmitContract(bot, ReasonName(r), LedgerFields(s, now, cause));
        LOG_INFO("playerbots", "[Contracts] bot={} {} cid={} lvl={} map={} anchor=({},{}) spawn={} entries={} "
                 "kills={}/{} ms={}", bot->GetName(), ReasonName(r), s.id, level, s.map, s.x, s.y, s.anchorSpawn,
                 s.entryCount, s.kills, s.target, now >= s.startMs ? now - s.startMs : 0);
    };

    if (s.phase == Phase::None)
    {
        // Issue only at the RPG machine's own choice point, with no live quest work.
        if (busy || bot->IsInCombat() || info.GetStatus() != RPG_IDLE || now < s.nextSearchMs)
            return false;
        s.nextSearchMs = now + kSearchRetryMs;
        if (!StoreState(guid, s) || CheckRpgStatusAvailable(RPG_DO_QUEST))
            return false;
        auto const index = gIndex.find(bot->GetMapId());
        if (index == gIndex.end())
            return false;
        std::uint8_t const team = bot->GetTeamId() == TEAM_ALLIANCE ? kAlliance : kHorde;
        std::vector<Cluster> const ranked =
            RankClusters(p, Candidates(p, index->second, team, level, bx, by), bx, by);
        Map* map = bot->GetMap();
        uint32 const phaseMask = bot->GetPhaseMask();
        float const height = bot->GetCollisionHeight();
        std::uint32_t tries = 0;
        Cluster const* pick = PickCluster(ranked,
            [&](Cluster const& c)
            {
                if (++tries > kMaxPickTries || AnchorCooling(p, s, bot->GetMapId(), c.x, c.y, now) ||
                    AutoWowDeathLoop::IsDangerous(guid, bot->GetMapId(), float(c.x), float(c.y)))
                    return true;
                // ponytail: water checked on the anchor only, at issue time (the index is built before any map
                // terrain exists); member spawns in water still count toward density. Per-spawn liquid flags
                // in the index if water packs keep getting picked.
                if (map->IsInWater(phaseMask, float(c.x), float(c.y), float(c.z), height))
                    return true;
                std::uint32_t const zoneLow =
                    AutoWowDeathLoop::ZoneMinLevel(AutoWowDeathLoop::ZoneAt(map, float(c.x), float(c.y)));
                return zoneLow > level + kZoneLevelMargin;
            });
        if (!pick)
            return false;
        Issue(s, p, *pick, bot->GetMapId(), NextId(), now);
        s.nextQuestMs = now + kQuestRecheckMs;
        StoreState(guid, s);
        emit(Reason::Issued, "");
    }

    // An errand run (sell / repair / restock, then a return leg) pauses the contract instead of ending it:
    // soak-s36-full-r1 abandoned 20 of 25 contracts as busy. Its clocks (timeout, no-kill watchdog) stop
    // too; one pause longer than TimeoutMs falls through and expires on the unshifted start.
    if (onErrand && !onTrip && !inParty && s.phase != Phase::None &&
        (!s.paused || now < s.pauseSinceMs + p.timeoutMs))
    {
        Pause(s, now);
        StoreState(guid, s);
        return false;
    }
    if (!onErrand)
        Resume(s, now);
    bool const danger = AutoWowDeathLoop::IsDangerous(guid, s.map, float(s.x), float(s.y));
    Reason r = Judge(p, s, now, danger);
    char const* why = r == Reason::Expired ? ExpiredCause(p, s, now, danger) : "";
    if (r == Reason::Issued)
    {
        why = onTrip ? "zone_trip" : inParty ? "party" : onErrand ? "errand" : bot->GetMapId() != s.map ? "map"
            : Displaced(p, s, bx, by) ? "displaced" : s.stuck > kMaxStuck ? "stuck" : "";
        if (*why)
            r = Reason::Abandoned;
    }
    // Quest work is back (a quest picked up while hunting, a deferral expired): the quest loop wins.
    if (r == Reason::Issued && s.phase == Phase::Hunt && now >= s.nextQuestMs && !bot->IsInCombat())
    {
        s.nextQuestMs = now + kQuestRecheckMs;
        if (CheckRpgStatusAvailable(RPG_DO_QUEST))
        {
            r = Reason::Abandoned;
            why = "quest_work";
        }
    }
    if (r != Reason::Issued)
    {
        if (r == Reason::Abandoned)
            LOG_INFO("playerbots", "[Contracts] bot={} abandon_cause={} cid={} kills={} phase={}", bot->GetName(), why,
                     s.id, s.kills, std::uint32_t(s.phase));
        emit(r, why);
        Finish(s, p, now);
        StoreState(guid, s);
        info.ChangeToIdle();
        return true;
    }
    if (bot->IsInCombat())
    {
        StoreState(guid, s);
        return false;  // combat owns the bot; kills are credited by the kill hook
    }

    s.phase = NextPhase(p, s, bot->GetMapId(), bx, by);
    if (s.phase == Phase::Travel)
    {
        // To the anchor (or back inside the leash): the no-teleport long walk, in Idle.
        if (WalkLeg(WorldPosition(s.map, float(s.x), float(s.y), float(s.z))))
            ++s.stuck;
        StoreState(guid, s);
        return true;
    }
    // Hunt: the wander status keeps the RPG machine at the anchor (no random GO_GRIND / flight pick) and lets
    // "attack anything" pull out-of-aggro targets; GrindTargetValue narrows them to the contract entries.
    StoreState(guid, s);
    if (info.GetStatus() != RPG_WANDER_RANDOM)
    {
        info.ChangeToWanderRandom();
        return true;
    }
    return false;
}
