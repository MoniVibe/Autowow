/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// AutoWow.Party / AutoWow.Dungeon runtime (policy: PartyPolicy.h). Formation, supervision and the dungeon
// run state machine run on the world thread (after the map updates); the leader's entrance walk runs in
// its New-RPG tick (NewRpgParty.cpp) and the role overrides in the bot AI tick (CombatUpdate). Shared
// state is one mutex-guarded table, never held while calling into bot AI or the core.

#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "AutoWowOracleRuntime.h"
#include "AutoWowQuestLedger.h"
#include "DungeonProbePolicy.h"
#include "SupplyPolicy.h"
#include "UnstickPolicy.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DungeonNavigator.h"
#include "ErrandsPolicy.h"
#include "GameTime.h"
#include "Group.h"
#include "GroupMgr.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PartyPolicy.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "ZoneProgressionPolicy.h"

namespace AutoWowParty
{
namespace
{
struct Params
{
    std::uint32_t checkIntervalMs = 30000;
    FormParams form;
    KeepParams keep;
    std::uint32_t separatedYards = 500;
    std::uint64_t rejoinCooldownMs = 600000;
    std::uint32_t healPct = 60;
    std::uint32_t tauntYards = 30;
    std::uint32_t maxWalkYards = 6000;
    std::uint32_t togetherYards = 60;
    std::uint64_t approachTimeoutMs = 1800000;
    std::uint32_t approachStuckTicks = 60;
    std::uint32_t stageYards = 40;
    std::uint64_t stageTimeoutMs = 120000;
    std::uint64_t memberGraceMs = 180000;  // AutoWow.Dungeon.MemberGraceMs: Inside, a member dead / outside this long
    std::uint64_t insideTimeoutMs = 3600000;
    std::uint64_t exitTimeoutMs = 120000;
    std::uint64_t runCooldownMs = 3600000;
    bool portalFallback = false;
    bool disbandOrphans = true;
    // AutoWow.Dungeon.Recruit
    std::uint32_t recruitIntervalMs = 600000;
    std::uint64_t recruitCooldownMs = 2700000;
    std::uint32_t recruitMaxParties = 1;  // per faction at once
    bool recruitWidePool = false;         // AutoWow.Dungeon.RecruitWidePool
    std::uint64_t recruitGatherPortalMs = 0;  // AutoWow.Dungeon.RecruitGatherPortalMs (0 = off)
    RecruitParams recruit;
};

struct Entrance
{
    std::uint32_t trigger = 0;
    std::uint32_t map = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Slot
{
    std::uint32_t guid = 0;
    std::uint32_t cls = 0;
    Role role = Role::Dps;
    std::vector<std::string> combat0, nonCombat0;  // strategy lists before the party (restored on disband)
};

struct Party
{
    std::uint8_t version = kStateVersion;
    std::uint32_t id = 0;  // run-scoped, never reused (ledger run id + pid is unique)
    std::uint32_t leader = 0;
    Reason why = Reason::None;
    std::uint32_t dungeonMap = 0;
    std::uint64_t formedMs = 0;
    std::vector<Slot> slots;  // guid ascending
    std::uint64_t separatedSinceMs = 0;
    std::uint64_t purposelessSinceMs = 0;
    // dungeon run
    Phase phase = Phase::None;
    std::uint64_t phaseMs = 0;
    std::uint64_t runStartMs = 0;
    std::uint64_t nextRunMs = 0;
    std::uint32_t instance = 0;
    std::uint32_t mask = 0;
    std::uint32_t allMask = 0;
    std::uint32_t stuckTicks = 0;
    bool approachGaveUp = false;
    bool leaderRpgOff = false;
    bool stagePortalDone = false;  // this Stage phase already used its portal fallback
    std::uint64_t missingSinceMs = 0;  // Inside: a member dead or outside since (0 = everyone in)
    std::vector<std::uint32_t> hearthTried;
    // AutoWow.Unstick.V2: fold of the members' quest logs and since when it held (0 = not looked yet).
    std::uint64_t questSig = 0;
    std::uint64_t questSinceMs = 0;
    // AutoWow.Dungeon.Recruit (state v3): one run, then released; run summary counters.
    bool recruited = false;
    std::uint8_t team = 0;
    std::uint32_t unavailable = 0;  // encounters the leader's navigator set aside (not clearable by this party)
    std::uint32_t deaths = 0;
    std::uint32_t deadBits = 0;     // slot index bit = that member is dead now
    bool wiped = false;
    bool completed = false;
    // AutoWow.Dungeon.RecruitGatherPortalMs: the gather start (Stage reached, 0 = not yet) and the members the
    // gather portal already moved (guids).
    std::uint64_t gatherMs = 0;
    std::vector<std::uint32_t> gatherPortaled;
};

struct QuestOffer
{
    std::uint32_t quest = 0;
    std::uint32_t minLevel = 0;
    std::int32_t level = 0;
};

Params gParams;
std::vector<DungeonDef> gDungeons;
std::map<std::uint32_t, Entrance> gEntrances;                                   // dungeon map -> entrance
std::unordered_map<std::uint32_t, std::vector<QuestOffer>> gZoneGroupQuests;   // zone -> group quests

// Guarded by gLock (bot AI threads read the roster; the world thread owns every mutation).
std::mutex gLock;
std::map<std::uint32_t, Party> gParties;                 // id -> party (id ascending = formation order)
std::unordered_map<std::uint32_t, std::uint32_t> gOf;    // member guid -> party id
std::unordered_map<std::uint32_t, std::uint64_t> gLeftMs;
std::unordered_map<std::uint32_t, std::uint64_t> gNextCastMs;
std::uint32_t gNextId = 1;
std::unordered_set<std::uint32_t> gSquadGuids;  // AutoWow.Squad rosters (EnsureSquad), bounded by its config
std::uint32_t gSinceForm = 0;
std::uint32_t gSinceSupervise = 0;
std::uint32_t gSinceRecruit = 0;
std::unordered_map<std::uint32_t, std::uint64_t> gRecruitReadyMs;  // guid -> earliest next recruit (gLock)
std::map<std::uint32_t, std::uint32_t> gClearable;                 // dungeon map -> clearable bits (world thread)

constexpr std::uint32_t kSuperviseMs = 1000;
constexpr std::uint32_t kBearForm = 5487, kDireBearForm = 9634;
constexpr std::uint32_t kCastSpacingMs = 1000;
constexpr char const* kFollowerNc = "+follow,-new rpg,-grind,-move random";

std::uint64_t NowMs() { return static_cast<std::uint64_t>(std::max<int64>(0, GameTime::GetGameTimeMS().count())); }

Player* Find(std::uint32_t guid) { return ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(guid)); }

PlayerbotAI* AiOf(Player* p) { return p ? PlayerbotsMgr::instance().GetPlayerbotAI(p) : nullptr; }

std::uint8_t TeamOf(Player* p) { return p->GetTeamId() == TEAM_ALLIANCE ? kAlliance : kHorde; }

std::int32_t Yd(float v) { return static_cast<std::int32_t>(v); }

std::uint32_t HpPct(Unit* u) { return u->GetMaxHealth() ? std::uint32_t(u->GetHealth() * 100 / u->GetMaxHealth()) : 0; }

bool HasGroupQuestInLog(Player* bot)
{
    for (std::uint16_t i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        std::uint32_t const id = bot->GetQuestSlotQuestId(i);
        Quest const* q = id ? sObjectMgr->GetQuestTemplate(id) : nullptr;
        if (q && IsGroupQuest(q->GetType(), q->GetSuggestedPlayers()) &&
            bot->GetQuestStatus(id) == QUEST_STATUS_INCOMPLETE)
            return true;
    }
    return false;
}

// A group quest in the log, or one offered in the bot's zone that it can take at its level.
bool GroupQuestSignal(Player* bot)
{
    if (HasGroupQuestInLog(bot))
        return true;
    auto const it = gZoneGroupQuests.find(bot->GetZoneId());
    if (it == gZoneGroupQuests.end())
        return false;
    std::int32_t const lvl = static_cast<std::int32_t>(bot->GetLevel());
    for (QuestOffer const& o : it->second)
    {
        if (o.minLevel > bot->GetLevel() || o.level > lvl + 2 || o.level < lvl - 5)
            continue;
        Quest const* q = sObjectMgr->GetQuestTemplate(o.quest);
        if (q && !bot->GetQuestRewardStatus(o.quest) && bot->GetQuestStatus(o.quest) == QUEST_STATUS_NONE &&
            bot->CanTakeQuest(q, false))
            return true;
    }
    return false;
}

void Emit(Party const& p, bool dungeon, char const* reason, std::string const& fields)
{
    if (!AutoWowQuestLedger::Enabled())
        return;
    Player* who = Find(p.leader);
    for (std::size_t i = 0; !who && i < p.slots.size(); ++i)
        who = Find(p.slots[i].guid);
    if (!who)
        return;
    if (dungeon)
        AutoWowQuestLedger::EmitDungeon(who, reason, fields);
    else
        AutoWowQuestLedger::EmitParty(who, reason, fields);
}

std::vector<std::uint32_t> Guids(Party const& p)
{
    std::vector<std::uint32_t> g;
    for (Slot const& s : p.slots)
        g.push_back(s.guid);
    return g;
}

void EmitRun(Party const& p, RunEvent e, std::int32_t encounter, std::uint64_t now)
{
    Emit(p, true, RunEventName(e),
         DungeonFields(p.id, p.dungeonMap, p.instance, encounter, p.mask, p.allMask,
                       p.runStartMs && now >= p.runStartMs ? now - p.runStartMs : 0, Guids(p)));
    LOG_INFO("playerbots", "[Party] pid={} dungeon {} map={} inst={} enc={} mask={}/{}", p.id, RunEventName(e),
             p.dungeonMap, p.instance, encounter, p.mask, p.allMask);
}

// Follower / role strategies (idempotent; re-applied when an engine rebuild dropped them).
void ApplyMode(Party const& p, Slot const& s, Player* bot, PlayerbotAI* ai, bool follow)
{
    if (s.guid != p.leader)
    {
        if (ai->GetMaster() != Find(p.leader))
            ai->SetMaster(Find(p.leader));
        if (follow && !ai->HasStrategy("follow", BOT_STATE_NON_COMBAT))
            ai->ChangeStrategy(kFollowerNc, BOT_STATE_NON_COMBAT);
    }
    if (!detail::gRoles || s.role == Role::Dps)
        return;
    RoleStrategies const r = StrategiesFor(s.cls, s.role);
    char const* marker = s.role == Role::Tank ? "tank assist" : nullptr;
    if (s.role == Role::Healer)
        marker = s.cls == kShaman || s.cls == kDruid ? "resto" : "heal";
    if (r.combat.empty() || ai->HasStrategy(marker, BOT_STATE_COMBAT))
        return;
    ai->ChangeStrategy(r.combat, BOT_STATE_COMBAT);
    if (!r.nonCombat.empty())
        ai->ChangeStrategy(r.nonCombat, BOT_STATE_NON_COMBAT);
    LOG_INFO("playerbots", "[Party] pid={} bot={} role={} strategies {}", p.id, bot->GetName(), RoleName(s.role),
             r.combat);
}

void Restore(Slot const& s, Player* bot, PlayerbotAI* ai)
{
    ai->SetAutoWowPaused(false);
    ai->SetMaster(nullptr);
    std::string const c = StrategyUndo(s.combat0, ai->GetStrategies(BOT_STATE_COMBAT));
    std::string const n = StrategyUndo(s.nonCombat0, ai->GetStrategies(BOT_STATE_NON_COMBAT));
    if (!c.empty())
        ai->ChangeStrategy(c, BOT_STATE_COMBAT);
    if (!n.empty())
        ai->ChangeStrategy(n, BOT_STATE_NON_COMBAT);
    LOG_INFO("playerbots", "[Party] bot={} restored combat={} nc={}", bot->GetName(), c, n);
}

// Forget the party (caller holds no lock); disband the core group when it is still exactly ours.
void Dissolve(std::uint32_t id, Disband why, std::uint64_t now)
{
    Party p;
    {
        std::lock_guard<std::mutex> guard(gLock);
        auto const it = gParties.find(id);
        if (it == gParties.end())
            return;
        p = it->second;
        gParties.erase(it);
        for (Slot const& s : p.slots)
        {
            gOf.erase(s.guid);
            // AutoWow.Unstick.V2: a stalled party's members stay out of formation for StallMs more, so the same
            // pinned party does not re-form at the end of the ordinary rejoin cooldown.
            gLeftMs[s.guid] = why == Disband::Stalled ? now + AutoWowUnstickV2::detail::gParams.partyStallMs : now;
        }
    }
    Group* group = nullptr;
    bool exact = true;
    for (Slot const& s : p.slots)
        if (Player* bot = Find(s.guid))
        {
            if (PlayerbotAI* ai = AiOf(bot))
                Restore(s, bot, ai);
            if (!group)
                group = bot->GetGroup();
            else if (bot->GetGroup() != group)
                exact = false;
        }
    if (group)
    {
        for (Group::MemberSlot const& m : group->GetMemberSlots())
            if (std::none_of(p.slots.begin(), p.slots.end(),
                             [&](Slot const& s) { return s.guid == m.guid.GetCounter(); }))
                exact = false;
        if (exact && !group->isRaidGroup() && !group->isBGGroup() && !group->isBFGroup() && !group->isLFGGroup())
            group->Disband();  // deletes the group
    }
    Emit(p, false, DisbandName(why),
         PartyFields(p.id, Guids(p), [&] { std::vector<Role> r; for (Slot const& s : p.slots) r.push_back(s.role); return r; }(),
                     p.leader, p.why, p.dungeonMap, now - p.formedMs));
    LOG_INFO("playerbots", "[Party] pid={} disbanded reason={} group_disbanded={}", p.id, DisbandName(why),
             group && exact);
    if (p.recruited)
    {
        // AutoWow.Dungeon.Recruit: the run's summary row, and a cooldown so questing is not starved.
        std::uint32_t const all = p.allMask & ~p.unavailable;
        Emit(p, true, RunEventName(RunEvent::Summary),
             DungeonFields(p.id, p.dungeonMap, p.instance, -1, p.mask, all,
                           p.runStartMs && now >= p.runStartMs ? now - p.runStartMs : 0, Guids(p)) +
                 RunSummaryFields(AutoWowDungeonProbe::Bits(p.mask & all), AutoWowDungeonProbe::Bits(all), p.completed,
                                  p.wiped, p.deaths));
        std::lock_guard<std::mutex> guard(gLock);
        for (Slot const& s : p.slots)
            gRecruitReadyMs[s.guid] = now + gParams.recruitCooldownMs;
    }
}

// A bot-only ordinary group of at most five that no cohort party owns: every member online, a playerbot
// without a real-player master, not oracle-managed. The core persists groups across a restart, this registry
// does not; such a group would keep its bots out of formation (and out of the bridge `independent` arm).
// ponytail: also matches bridge `party` rosters of non-managed bots; AutoWow.Party.DisbandOrphans = 0 keeps them.
bool IsOrphan(Group* g)
{
    if (g->isRaidGroup() || g->isBGGroup() || g->isBFGroup() || g->isLFGGroup() || g->GetMembersCount() > 5)
        return false;
    for (Group::MemberSlot const& m : g->GetMemberSlots())
    {
        PlayerbotAI* ai = AiOf(ObjectAccessor::FindPlayer(m.guid));
        if (!ai || ai->IsRealPlayer() || ai->HasRealPlayerMaster() ||
            AutoWowOracleRuntime::IsManagedBot(m.guid.GetCounter()) ||
            AutoWowDungeonProbe::IsProbeBot(m.guid.GetCounter()))  // the probe runner owns its party
            return false;
        std::lock_guard<std::mutex> guard(gLock);
        if (gOf.count(m.guid.GetCounter()))
            return false;
    }
    return true;
}

// AutoWow.Unstick.V2: quest-log fold of the party members (quest ids, status, kill / item counters; slot order).
std::uint64_t QuestSig(std::vector<Player*> const& bots)
{
    std::uint64_t h = AutoWowUnstickV2::kFoldSeed;
    for (Player* bot : bots)
    {
        QuestStatusMap const& statusMap = bot->getQuestStatusMap();
        for (std::uint16_t slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            std::uint32_t const id = bot->GetQuestSlotQuestId(slot);
            auto const it = id ? statusMap.find(id) : statusMap.end();
            if (it == statusMap.end())
                continue;
            h = AutoWowUnstickV2::Fold(h, (std::uint64_t(id) << 8) | it->second.Status);
            for (std::size_t k = 0; k < QUEST_OBJECTIVES_COUNT; ++k)
                h = AutoWowUnstickV2::Fold(h, it->second.CreatureOrGOCount[k]);
            for (std::size_t k = 0; k < QUEST_ITEM_OBJECTIVES_COUNT; ++k)
                h = AutoWowUnstickV2::Fold(h, it->second.ItemCount[k]);
        }
    }
    return h;
}

// AutoWow.Unstick.V2: an unpartied cohort bot alive on an instance map (a society party was inside when the
// soak stopped; this registry does not survive a restart) goes to its hearthstone bind. The owner ruling
// allows the portal. Not a dungeon probe (runner registry or the configured guid range), not oracle-managed.
bool SendStrandeeHome(Player* bot, PlayerbotAI* ai)
{
    std::uint32_t const g = bot->GetGUID().GetCounter();
    AutoWowUnstickV2::Params const& up = AutoWowUnstickV2::detail::gParams;
    if (!bot->IsInWorld() || !bot->GetMap() || !bot->GetMap()->Instanceable() || bot->InBattleground() ||
        bot->GetMap()->IsBattlegroundOrArena() || !bot->IsAlive() || bot->IsBeingTeleported() || bot->GetGroup() ||
        !ai->IsAutoWowIndependentParty() || ai->IsRealPlayer() || ai->HasRealPlayerMaster() ||
        AutoWowOracleRuntime::IsManagedBot(g) ||
        AutoWowDungeonProbe::IsProbeBot(g) || (g >= up.strandSkipMin && g <= up.strandSkipMax))
        return false;
    {
        std::lock_guard<std::mutex> guard(gLock);
        if (gOf.count(g))
            return false;
    }
    std::uint32_t const fromZone = bot->GetZoneId();
    std::uint32_t toZone = bot->m_homebindAreaId;
    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(bot->m_homebindAreaId); area && area->zone)
        toZone = area->zone;
    LOG_INFO("playerbots", "[Unstick] bot={} instance_strand map={} zone={} -> home map={} zone={}", bot->GetName(),
             bot->GetMapId(), fromZone, bot->m_homebindMapId, toZone);
    if (AutoWowQuestLedger::Enabled())
        AutoWowQuestLedger::EmitZoneMove(bot, "instance_strand",
                                         AutoWowZoneProgression::LedgerFields(fromZone, toZone, 0, true,
                                                                              AutoWowZoneProgression::Mode::Portal));
    bot->TeleportTo(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ, bot->GetOrientation());
    return true;
}

// Core group, registry entry, follower / role modes and the ledger `formed` row of a plan (Form, Recruit).
// `party` carries any preset fields; returns the new party id, 0 = not formed.
std::uint32_t Materialize(Plan const& pl, Party party, std::uint64_t now)
{
    Player* leader = Find(pl.leader);
    PlayerbotAI* leaderAI = AiOf(leader);
    if (!leaderAI)
        return 0;
    party.leader = pl.leader;
    party.why = pl.reason;
    party.dungeonMap = pl.dungeonMap;
    party.formedMs = now;
    std::vector<Player*> bots;
    for (std::size_t i = 0; i < pl.guids.size(); ++i)
    {
        Player* bot = Find(pl.guids[i]);
        PlayerbotAI* ai = AiOf(bot);
        if (!ai)
            break;
        Slot s;
        s.guid = pl.guids[i];
        s.cls = bot->getClass();
        s.role = pl.roles[i];
        s.combat0 = ai->GetStrategies(BOT_STATE_COMBAT);
        s.nonCombat0 = ai->GetStrategies(BOT_STATE_NON_COMBAT);
        party.slots.push_back(std::move(s));
        bots.push_back(bot);
    }
    if (bots.size() != pl.guids.size())
        return 0;

    // Core group: the bridge CreateParty idiom (bot-only, ordinary party).
    Group* group = new Group;
    if (!group->Create(leader))
    {
        delete group;
        return 0;
    }
    sGroupMgr->AddGroup(group);
    bool ok = true;
    for (Player* bot : bots)
        if (bot != leader && !group->AddMember(bot))
            ok = false;
    if (!ok)
    {
        group->Disband();
        return 0;
    }
    group->SetLootMethod(NEED_BEFORE_GREED);
    group->SetLootThreshold(ITEM_QUALITY_UNCOMMON);

    {
        std::lock_guard<std::mutex> guard(gLock);
        party.id = gNextId++;
        for (Slot const& s : party.slots)
            gOf[s.guid] = party.id;
        gParties[party.id] = party;
    }
    for (std::size_t i = 0; i < bots.size(); ++i)
    {
        PlayerbotAI* ai = AiOf(bots[i]);
        ai->SetAutoWowIndependentParty(true);  // native group maintenance leaves the roster alone
        if (bots[i] == leader)
        {
            ai->SetMaster(nullptr);
            ai->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);
        }
        ApplyMode(party, party.slots[i], bots[i], ai, true);
    }
    std::vector<Role> roles;
    for (Slot const& s : party.slots)
        roles.push_back(s.role);
    Emit(party, false, "formed",
         PartyFields(party.id, pl.guids, roles, party.leader, party.why, party.dungeonMap, 0) +
             (party.recruited ? ",\"recruit\":1" : ""));
    LOG_INFO("playerbots", "[Party] pid={} formed why={} leader={} members={} dmap={}", party.id,
             ReasonName(party.why), leader->GetName(), pl.guids.size(), party.dungeonMap);
    return party.id;
}

// ---- formation ------------------------------------------------------------------------------------------
void Form(std::uint64_t now)
{
    std::vector<Candidate> cands;
    for (auto const& [guid, bot] : ObjectAccessor::GetPlayers())
    {
        PlayerbotAI* ai = AiOf(bot);
        if (Group* g = ai && bot->IsInWorld() ? bot->GetGroup() : nullptr; g && gParams.disbandOrphans && IsOrphan(g))
        {
            std::vector<Player*> members;
            for (Group::MemberSlot const& m : g->GetMemberSlots())
                members.push_back(ObjectAccessor::FindPlayer(m.guid));
            LOG_INFO("playerbots", "[Party] orphan bot group of {} ({} members) disbanded", bot->GetName(), members.size());
            g->Disband();  // deletes the group
            for (Player* m : members)
                if (PlayerbotAI* mai = AiOf(m))
                    mai->SetMaster(nullptr);
            continue;
        }
        if (AutoWowUnstickV2::Enabled() && ai && SendStrandeeHome(bot, ai))
            continue;
        if (!ai || !bot->IsInWorld() || !ai->IsAutoWowIndependentParty() || ai->IsRealPlayer() ||
            ai->IsAutoWowPaused() || bot->GetGroup() || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() ||
            bot->IsBeingTeleported() || bot->InBattleground() || !bot->GetMap() || bot->GetMap()->Instanceable())
            continue;
        std::uint32_t const g = guid.GetCounter();
        if (AutoWowOracleRuntime::IsManagedBot(g) || AutoWowDungeonProbe::IsProbeBot(g) ||
            (AutoWowZoneProgression::Enabled() && AutoWowZoneProgression::Active(g)) ||
            (AutoWowErrands::Enabled() && AutoWowErrands::Active(g)) ||
            // Supply reps and artisans stay home: a party took the Horde artisan from Orgrimmar to Thousand
            // Needles and turned off its supply step (soak-s41-full-r1). An apprentice artisan (below
            // AutoWow.Supply.ArtisanMinLevel) is an ordinary adventurer here (ActiveRoleOf).
            (AutoWowSupply::Enabled() && AutoWowSupply::ActiveRoleOf(bot).role != AutoWowSupply::Role::None))
            continue;
        {
            std::lock_guard<std::mutex> guard(gLock);
            auto const left = gLeftMs.find(g);
            if (gOf.count(g) || gSquadGuids.count(g) ||
                (left != gLeftMs.end() && now < left->second + gParams.rejoinCooldownMs))
                continue;
        }
        Candidate c;
        c.guid = g;
        c.level = bot->GetLevel();
        c.cls = bot->getClass();
        c.team = TeamOf(bot);
        c.map = bot->GetMapId();
        c.zone = bot->GetZoneId();
        c.x = Yd(bot->GetPositionX());
        c.y = Yd(bot->GetPositionY());
        c.groupQuest = GroupQuestSignal(bot);
        cands.push_back(c);
    }
    FormParams fp = gParams.form;
    fp.dungeons = detail::gDungeons;
    for (Plan const& plan : FormParties(cands, fp, gDungeons))
    {
        // The dungeon needs a known walkable entrance on the party's map; else the party quests together.
        Plan pl = plan;
        if (pl.reason == Reason::Dungeon)
        {
            auto const e = gEntrances.find(pl.dungeonMap);
            Player* lead = Find(pl.leader);
            if (e == gEntrances.end() || !lead || e->second.map != lead->GetMapId())
            {
                bool quest = false;
                for (Candidate const& c : cands)
                    if (std::find(pl.guids.begin(), pl.guids.end(), c.guid) != pl.guids.end())
                        quest = quest || c.groupQuest;
                if (!quest)
                    continue;
                pl.reason = Reason::GroupQuest;
                pl.dungeonMap = 0;
            }
        }
        Materialize(pl, Party{}, now);
    }
}

// ---- dungeon run (world thread) -------------------------------------------------------------------------
void EndRun(Party& p, std::uint64_t now)
{
    p.phase = Phase::None;
    p.phaseMs = now;
    p.stagePortalDone = false;
    p.nextRunMs = now + gParams.runCooldownMs;
    p.hearthTried.clear();
    for (Slot const& s : p.slots)
        if (PlayerbotAI* ai = AiOf(Find(s.guid)))
        {
            ai->SetAutoWowPaused(false);
            if (s.guid == p.leader && p.leaderRpgOff)
                ai->ChangeStrategy("+new rpg", BOT_STATE_NON_COMBAT);
        }
    p.leaderRpgOff = false;
}

void SetPhase(Party& p, Phase ph, std::uint64_t now)
{
    LOG_INFO("playerbots", "[Party] pid={} phase {} -> {}", p.id, std::uint32_t(p.phase), std::uint32_t(ph));
    p.phase = ph;
    p.phaseMs = now;
    p.stagePortalDone = false;
    p.missingSinceMs = 0;
}

// AutoWow.Dungeon.Recruit: the dungeon's clearable encounter bits, as the probe runner scores them
// (DungeonProbeRuntime.cpp): a kill-credit encounter counts only when its creature has a static spawn on the map
// (a script-summoned boss such as RFK Grubbis can never be reached). Cached per map.
std::uint32_t ClearableOf(std::uint32_t map)
{
    if (auto const it = gClearable.find(map); it != gClearable.end())
        return it->second;
    std::vector<AutoWowDungeonProbe::EncounterRecord> records;
    if (DungeonEncounterList const* list = sObjectMgr->GetDungeonEncounterList(map, DUNGEON_DIFFICULTY_NORMAL))
    {
        std::unordered_set<std::uint32_t> credits, spawned;
        for (DungeonEncounter const* enc : *list)
            if (enc->creditType == ENCOUNTER_CREDIT_KILL_CREATURE)
                credits.insert(enc->creditEntry);
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            if (data.mapid == map)
                for (std::uint32_t id : {data.id, data.id2, data.id3})
                    if (id && credits.count(id))
                        spawned.insert(id);
        for (DungeonEncounter const* enc : *list)
        {
            bool const kill = enc->creditType == ENCOUNTER_CREDIT_KILL_CREATURE;
            records.push_back({enc->dbcEntry->encounterIndex, kill, kill && spawned.count(enc->creditEntry) > 0});
        }
    }
    return gClearable[map] = AutoWowDungeonProbe::ClearableMask(records);
}

// Step through an entrance trigger like a client does (the stock DungeonTransition waits for the whole
// party standing still inside the trigger at once, which never held in soak-s26-full-r1: 2/2 stage_failed).
void FireTrigger(Player* bot, PlayerbotAI* ai, std::uint32_t triggerId)
{
    bot->StopMoving();
    WorldPacket packet(CMSG_AREATRIGGER);
    packet << triggerId;
    packet.rpos(0);
    bot->GetSession()->HandleAreaTriggerOpcode(packet);
    ai->SetAutoWowPaused(false);
}

// Members outside on the entrance map walk into the trigger and step through it.
void StepIn(Player* bot, PlayerbotAI* ai, AreaTrigger const* trigger, Entrance const& e)
{
    if (!bot->IsAlive() || bot->IsInCombat() || bot->GetMapId() != e.map || bot->GetInstanceId())
        return;
    if (bot->IsInAreaTriggerRadius(trigger))
        FireTrigger(bot, ai, e.trigger);
    else if (!bot->isMoving())
    {
        ai->SetAutoWowPaused(false);
        bot->GetMotionMaster()->MovePoint(0, e.x, e.y, e.z);
    }
}

// One supervision step of a dungeon run. Returns a disband reason (None = keep the party).
Disband RunStep(Party& p, std::vector<Player*> const& bots, Player* leader, std::uint64_t now)
{
    Entrance const e = gEntrances.count(p.dungeonMap) ? gEntrances.at(p.dungeonMap) : Entrance{};
    AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(e.trigger);
    PlayerbotAI* leaderAI = AiOf(leader);

    // Entered (from Approach / Stage): the stock DungeonTransition admitted the leader.
    if ((p.phase == Phase::Approach || p.phase == Phase::Stage) && leader->GetMapId() == p.dungeonMap &&
        leader->GetInstanceId())
    {
        SetPhase(p, Phase::Inside, now);
        p.instance = leader->GetInstanceId();
        p.allMask = 0;
        if (DungeonEncounterList const* list = sObjectMgr->GetDungeonEncounterList(p.dungeonMap, DUNGEON_DIFFICULTY_NORMAL))
            for (DungeonEncounter const* enc : *list)
                p.allMask |= 1u << enc->dbcEntry->encounterIndex;
        if (p.recruited)
            p.allMask &= ClearableOf(p.dungeonMap);
        InstanceMap* im = leader->GetMap()->ToInstanceMap();
        InstanceScript* script = im ? im->GetInstanceScript() : nullptr;
        p.mask = script ? script->GetCompletedEncounterMask() : 0;
        if (leaderAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
        {
            leaderAI->ChangeStrategy("-new rpg", BOT_STATE_NON_COMBAT);  // the dungeon navigator owns the leader
            p.leaderRpgOff = true;
        }
        EmitRun(p, RunEvent::Entered, -1, now);
        return Disband::None;
    }

    switch (p.phase)
    {
        case Phase::Approach:
        {
            std::int64_t const dx = Yd(leader->GetPositionX()) - Yd(e.x), dy = Yd(leader->GetPositionY()) - Yd(e.y);
            if (leader->GetMapId() == e.map && dx * dx + dy * dy <= std::int64_t(gParams.stageYards) * gParams.stageYards)
            {
                SetPhase(p, Phase::Stage, now);
                for (std::size_t i = 0; i < bots.size(); ++i)
                    if (bots[i] != leader)
                        AiOf(bots[i])->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);
                return Disband::None;
            }
            if (!p.approachGaveUp && now < p.phaseMs + gParams.approachTimeoutMs)
                return Disband::None;
            if (gParams.portalFallback)
            {
                // Owner ruling: a portal fallback for a spent long approach, logged and ledgered.
                EmitRun(p, RunEvent::PortalFallback, -1, now);
                for (Player* bot : bots)
                    if (bot->IsAlive() && !bot->IsInCombat())
                    {
                        if (AutoWowQuestLedger::Enabled())
                            AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Contaminated, 0, "party_portal_fallback");
                        bot->TeleportTo(e.map, e.x, e.y, e.z, bot->GetOrientation());
                    }
                SetPhase(p, Phase::Stage, now);
                for (Player* bot : bots)
                    if (bot != leader)
                        AiOf(bot)->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);
                return Disband::None;
            }
            EmitRun(p, RunEvent::ApproachGaveUp, -1, now);
            EndRun(p, now);
            return Disband::None;
        }
        case Phase::Stage:
        {
            if (now >= p.phaseMs + gParams.stageTimeoutMs || !trigger)
            {
                LOG_INFO("playerbots", "[Party] pid={} stage_failed leader_map={} leader_yd={} in_trigger={} alive={} combat={}",
                         p.id, leader->GetMapId(), trigger ? int(leader->GetExactDist2d(e.x, e.y)) : -1,
                         trigger && leader->IsInAreaTriggerRadius(trigger), leader->IsAlive(), leader->IsInCombat());
                EmitRun(p, RunEvent::StageFailed, -1, now);
                EndRun(p, now);
                return Disband::None;
            }
            // Owner ruling: a logged portal fallback. A leader that cannot path into the trigger (Wailing
            // Caverns cave, soak-s27-full-r1: stuck 39 yd off) is placed on it at half the stage timeout.
            if (gParams.portalFallback && !p.stagePortalDone && now >= p.phaseMs + gParams.stageTimeoutMs / 2 &&
                leader->IsAlive() && !leader->IsInCombat() && leader->GetMapId() == e.map &&
                !leader->IsInAreaTriggerRadius(trigger))
            {
                p.stagePortalDone = true;
                EmitRun(p, RunEvent::PortalFallback, -1, now);
                LOG_INFO("playerbots", "[Party] pid={} stage portal leader_yd={}", p.id, int(leader->GetExactDist2d(e.x, e.y)));
                // The whole party: a trigger the leader cannot walk into is out of the followers' reach too
                // (soak-s32-full-r1: 2/2 Wailing Caverns runs entered with the leader alone, then abandoned).
                for (Player* b : bots)
                    if (b->IsAlive() && !b->IsInCombat() && b->GetMapId() == e.map)
                        b->TeleportTo(e.map, e.x, e.y, e.z, b->GetOrientation());
                return Disband::None;
            }
            // Every member steps into the trigger volume; followers hold there (paused) until the stock
            // DungeonTransition admits them (it clears the pause). The leader holds via its PartyStep.
            for (Player* bot : bots)
            {
                if (!bot->IsAlive() || bot->IsInCombat() || bot->GetMapId() != e.map || bot->GetInstanceId())
                    continue;
                PlayerbotAI* ai = AiOf(bot);
                if (bot->IsInAreaTriggerRadius(trigger))
                {
                    if (bot == leader)
                    {
                        // Only with the whole party alive out here: the dungeon navigator will not lead
                        // while a member is dead or elsewhere (soak-s29-full-r1: a ghost left behind
                        // stalled a Ragefire run at the entrance for 20+ min).
                        if (std::all_of(bots.begin(), bots.end(), [&e](Player* b)
                                        { return b->IsAlive() && b->GetMapId() == e.map; }))
                            FireTrigger(bot, ai, e.trigger);
                        continue;
                    }
                    if (bot->isMoving())
                        bot->StopMoving();
                    ai->SetAutoWowPaused(true);
                }
                else if (!bot->isMoving())
                {
                    ai->SetAutoWowPaused(false);
                    bot->GetMotionMaster()->MovePoint(0, e.x, e.y, e.z);
                }
            }
            return Disband::None;
        }
        case Phase::Inside:
        {
            bool anyAlive = false;
            for (Player* bot : bots)
            {
                anyAlive = anyAlive || bot->IsAlive();
                PlayerbotAI* ai = AiOf(bot);
                if (trigger && bot != leader)
                    StepIn(bot, ai, trigger, e);
                if (bot != leader && bot->GetMapId() == p.dungeonMap && !ai->IsAutoWowPaused() &&
                    !ai->HasStrategy("follow", BOT_STATE_NON_COMBAT))
                    ai->ChangeStrategy("+follow", BOT_STATE_NON_COMBAT);
            }
            // RecruitGatherPortalMs: a member outside counts as missing only once its gather portal was tried.
            bool const gather = p.recruited && gParams.recruitGatherPortalMs;
            bool const missing = std::any_of(bots.begin(), bots.end(), [&p, gather](Player* b)
            {
                return !b->IsAlive() ||
                       (b->GetMapId() != p.dungeonMap &&
                        (!gather || std::find(p.gatherPortaled.begin(), p.gatherPortaled.end(),
                                              std::uint32_t(b->GetGUID().GetCounter())) != p.gatherPortaled.end()));
            });
            if (!missing)
                p.missingSinceMs = 0;
            else if (!p.missingSinceMs)
                p.missingSinceMs = now;
            else if (anyAlive && now >= p.missingSinceMs + gParams.memberGraceMs)
            {
                // The navigator waits for the whole party; bots do not corpse-run into instances yet.
                LOG_INFO("playerbots", "[Party] pid={} dungeon member missing {} ms: abandon", p.id, now - p.missingSinceMs);
                EmitRun(p, RunEvent::Abandoned, -1, now);
                SetPhase(p, Phase::Exit, now);
                return Disband::None;
            }
            if (!anyAlive)
            {
                p.wiped = true;
                EmitRun(p, RunEvent::Wiped, -1, now);
                EndRun(p, now);
                return Disband::None;
            }
            if (leader->GetMapId() != p.dungeonMap)
            {
                if (!leader->IsAlive())
                    return Disband::None;  // released outside: its corpse run decides
                EmitRun(p, RunEvent::Abandoned, -1, now);
                EndRun(p, now);
                return Disband::None;
            }
            InstanceMap* im = leader->GetMap()->ToInstanceMap();
            InstanceScript* script = im ? im->GetInstanceScript() : nullptr;
            std::uint32_t const mask = script ? script->GetCompletedEncounterMask() : p.mask;
            for (std::uint32_t enc : NewBosses(p.mask, mask))
            {
                p.mask |= 1u << enc;
                EmitRun(p, RunEvent::BossKilled, std::int32_t(enc), now);
            }
            // Recruited runs: the encounters the leader's navigator set aside (friendly credit, key blocked) do not
            // hold the completion, as in the probe runner (0 for every other party).
            if (p.recruited)
                p.unavailable = GetDungeonNavigatorUnavailableMask(p.leader, p.dungeonMap, p.instance) & p.allMask;
            if (AllEncountersDone(p.mask, p.allMask & ~p.unavailable))
            {
                p.completed = true;
                EmitRun(p, RunEvent::Completed, -1, now);
                SetPhase(p, Phase::Exit, now);
            }
            else if (now >= p.phaseMs + gParams.insideTimeoutMs)
            {
                EmitRun(p, RunEvent::Abandoned, -1, now);
                SetPhase(p, Phase::Exit, now);
            }
            return Disband::None;
        }
        case Phase::Exit:
        {
            // Out like players: the hearthstone when ready; the rest leave with the disband (the core boots a
            // player without the instance's group after its grace timer).
            bool inside = false;
            for (Player* bot : bots)
            {
                if (bot->GetMapId() != p.dungeonMap)
                    continue;
                inside = true;
                if (!bot->IsAlive() || bot->IsInCombat() ||
                    std::find(p.hearthTried.begin(), p.hearthTried.end(), bot->GetGUID().GetCounter()) != p.hearthTried.end())
                    continue;
                p.hearthTried.push_back(bot->GetGUID().GetCounter());
                AiOf(bot)->DoSpecificAction("hearthstone", Event("autowow party"), true);
            }
            if (!inside || now >= p.phaseMs + gParams.exitTimeoutMs)
                return Disband::DungeonDone;
            return Disband::None;
        }
        case Phase::None:
            break;
    }
    return Disband::None;
}

// AutoWow.Dungeon.RecruitGatherPortalMs (recruited parties in a run): drop members that cannot come, portal the
// ones not arrived GatherPortalMs after the gather start (Stage). False = the run was given up (logged).
bool RecruitGather(Party& p, std::uint64_t now)
{
    if (p.phase == Phase::None || p.phase == Phase::Exit)
        return true;
    if (!p.gatherMs && (p.phase == Phase::Stage || p.phase == Phase::Inside))
        p.gatherMs = now;
    bool const deadline = p.gatherMs && now >= p.gatherMs + gParams.recruitGatherPortalMs;
    Entrance const e = gEntrances.at(p.dungeonMap);
    std::vector<std::pair<std::uint32_t, char const*>> drops;
    for (Slot const& s : p.slots)
    {
        Player* bot = Find(s.guid);
        GatherFacts f;
        f.online = bot != nullptr;
        if (bot)
        {
            f.alive = bot->IsAlive();
            f.inDungeon = bot->GetMapId() == p.dungeonMap;
            f.arrived = f.inDungeon || (bot->GetMapId() == e.map &&
                                        bot->GetExactDist2d(e.x, e.y) <= float(gParams.stageYards));
            f.portaled = std::find(p.gatherPortaled.begin(), p.gatherPortaled.end(), s.guid) != p.gatherPortaled.end();
        }
        // The leader stays unless it is offline (then the run is given up below).
        switch (s.guid == p.leader && f.online ? GatherAct::None : DecideGather(f, deadline))
        {
            case GatherAct::Drop:
                drops.emplace_back(s.guid, f.online ? "dead_elsewhere" : "offline");
                break;
            case GatherAct::Portal:
            {
                p.gatherPortaled.push_back(s.guid);
                bool const taxi = bot->IsInFlight();
                if (taxi)
                {
                    // End the taxi cleanly (as Player::TeleportTo does) before the portal.
                    bot->GetMotionMaster()->MovementExpired();
                    bot->CleanupAfterTaxiFlight();
                }
                if (PlayerbotAI* ai = AiOf(bot))
                    ai->rpgInfo.ChangeToIdle();  // a pending flight-travel status must not resume
                if (AutoWowQuestLedger::Enabled())
                    AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Contaminated, 0, "party_recruit_gather_portal");
                LOG_INFO("playerbots", "[Party] pid={} gather portal member={} map={} yd={} taxi={} after_ms={}", p.id,
                         bot->GetName(), bot->GetMapId(),
                         bot->GetMapId() == e.map ? std::int64_t(bot->GetExactDist2d(e.x, e.y)) : -1, taxi,
                         now - p.gatherMs);
                bot->TeleportTo(e.map, e.x, e.y, e.z, bot->GetOrientation());
                break;
            }
            case GatherAct::None:
                break;
        }
    }
    if (drops.empty())
        return true;
    std::vector<Role> before, after;
    bool leaderDropped = false;
    for (Slot const& s : p.slots)
    {
        before.push_back(s.role);
        bool const dropped = std::any_of(drops.begin(), drops.end(), [&s](auto const& d) { return d.first == s.guid; });
        leaderDropped = leaderDropped || (dropped && s.guid == p.leader);
        if (!dropped)
            after.push_back(s.role);
    }
    if (leaderDropped || !KeepAfterDrop(before, after))
    {
        LOG_INFO("playerbots", "[Party] pid={} recruit gather abandon why={} dropped={}", p.id,
                 leaderDropped ? "leader_gone" : "roles_short", drops.size());
        EmitRun(p, RunEvent::Abandoned, -1, now);
        if (p.phase == Phase::Inside)
            SetPhase(p, Phase::Exit, now);
        else
            EndRun(p, now);
        return false;
    }
    Group* group = nullptr;
    if (Player* leader = Find(p.leader))
        group = leader->GetGroup();
    for (auto const& [guid, why] : drops)
    {
        auto const it = std::find_if(p.slots.begin(), p.slots.end(), [g = guid](Slot const& s) { return s.guid == g; });
        if (Player* bot = Find(guid))
            if (PlayerbotAI* ai = AiOf(bot))
                Restore(*it, bot, ai);
        if (group)
            group->RemoveMember(ObjectGuid::Create<HighGuid::Player>(guid));
        LOG_INFO("playerbots", "[Party] pid={} recruit gather dropped member={} why={} left={}", p.id, guid, why,
                 p.slots.size() - 1);
        Emit(p, false, "dropped",
             ",\"pid\":" + std::to_string(p.id) + ",\"member\":" + std::to_string(guid) + ",\"drop\":\"" + why +
                 "\",\"left\":" + std::to_string(p.slots.size() - 1));
        p.slots.erase(it);
        std::lock_guard<std::mutex> guard(gLock);
        gOf.erase(guid);
        gLeftMs[guid] = now;
        gRecruitReadyMs[guid] = now + gParams.recruitCooldownMs;
    }
    return true;
}

// ---- supervision ----------------------------------------------------------------------------------------
void Supervise(std::uint32_t id, std::uint64_t now)
{
    Party p;
    {
        std::lock_guard<std::mutex> guard(gLock);
        auto const it = gParties.find(id);
        if (it == gParties.end())
            return;
        p = it->second;
    }
    if (p.why == Reason::Squad)
        return;  // EnsureSquad owns it
    // A recruited party's gather portal crosses maps: a member in transit is out of the world for a moment and
    // must not read as offline (every other teleport of this runtime stays on one map).
    if (p.recruited)
        for (Slot const& s : p.slots)
            if (Player* b = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(s.guid));
                b && (!b->IsInWorld() || b->IsBeingTeleported()))
                return;
    if (p.recruited && gParams.recruitGatherPortalMs)
    {
        bool const keep = RecruitGather(p, now);
        {
            std::lock_guard<std::mutex> guard(gLock);
            if (auto const it = gParties.find(id); it != gParties.end())
            {
                // The leader thread only writes the approach fields; keep its latest values.
                p.stuckTicks = it->second.stuckTicks;
                p.approachGaveUp = p.approachGaveUp || it->second.approachGaveUp;
                it->second = p;
            }
        }
        if (!keep && p.phase == Phase::None)
        {
            Dissolve(id, Disband::DungeonDone, now);  // given up before entry: released like any recruited run
            return;
        }
    }
    std::vector<Player*> bots;
    Player* leader = nullptr;
    Group* group = nullptr;
    bool sameGroup = true;
    KeepFacts f;
    f.size = std::uint32_t(p.slots.size());
    f.minLevel = ~0u;
    for (Slot const& s : p.slots)
    {
        Player* bot = Find(s.guid);
        if (!bot || !bot->IsInWorld() || !AiOf(bot))
            continue;
        ++f.online;
        bots.push_back(bot);
        f.minLevel = std::min<std::uint32_t>(f.minLevel, bot->GetLevel());
        f.maxLevel = std::max<std::uint32_t>(f.maxLevel, bot->GetLevel());
        if (s.guid == p.leader)
            leader = bot;
        if (!group)
            group = bot->GetGroup();
        if (!bot->GetGroup() || bot->GetGroup() != group)
            sameGroup = false;
    }
    if (!leader || !sameGroup || !group || group->GetMembersCount() != p.slots.size())
        f.online = 0;  // the roster broke up (logout, kick, left): let it go
    f.ageMs = now - p.formedMs;
    f.inDungeonRun = p.phase != Phase::None;

    Disband why = Disband::None;
    if (p.recruited && f.online == f.size && f.inDungeonRun)
        for (std::size_t i = 0; i < bots.size() && i < 32; ++i)
        {
            std::uint32_t const bit = 1u << i;
            bool const dead = !bots[i]->IsAlive();
            if (dead && !(p.deadBits & bit))
                ++p.deaths;
            p.deadBits = dead ? p.deadBits | bit : p.deadBits & ~bit;
        }
    if (f.online == f.size && f.inDungeonRun)
        why = RunStep(p, bots, leader, now);
    if (p.recruited && why == Disband::None && f.inDungeonRun && p.phase == Phase::None)
        why = Disband::DungeonDone;  // one run per recruitment: released after completion, wipe or give-up
    if (f.online == f.size && !f.inDungeonRun)
    {
        // Separation from the leader (followers follow; a long gap means someone is stuck or dead far away).
        bool apart = false;
        for (Player* bot : bots)
            if (bot != leader && (bot->GetMapId() != leader->GetMapId() ||
                                  bot->GetExactDist2d(leader) > float(gParams.separatedYards)))
                apart = true;
        p.separatedSinceMs = apart ? (p.separatedSinceMs ? p.separatedSinceMs : now) : 0;
        f.separatedMs = p.separatedSinceMs ? now - p.separatedSinceMs : 0;

        bool purpose = false;
        if (p.why == Reason::Dungeon)
            purpose = detail::gDungeons && SelectDungeon(gDungeons, TeamOf(leader), f.minLevel, f.maxLevel) != nullptr;
        for (std::size_t i = 0; !purpose && i < bots.size(); ++i)
            purpose = GroupQuestSignal(bots[i]);
        p.purposelessSinceMs = purpose ? 0 : (p.purposelessSinceMs ? p.purposelessSinceMs : now);
        f.purposelessMs = p.purposelessSinceMs ? now - p.purposelessSinceMs : 0;

        // Start a dungeon run: everyone alive, out of combat, together on the entrance's map, in walk range.
        auto const e = gEntrances.find(p.dungeonMap);
        if (p.why == Reason::Dungeon && detail::gDungeons && now >= p.nextRunMs && e != gEntrances.end() &&
            leader->GetMapId() == e->second.map && !leader->GetMap()->Instanceable() && !leader->IsInFlight() &&
            SelectDungeon(gDungeons, TeamOf(leader), f.minLevel, f.maxLevel) &&
            leader->GetExactDist2d(e->second.x, e->second.y) <= float(gParams.maxWalkYards))
        {
            bool ready = true;
            for (Player* bot : bots)
                ready = ready && bot->IsAlive() && !bot->IsInCombat() && bot->GetMapId() == leader->GetMapId() &&
                        bot->GetExactDist2d(leader) <= float(gParams.togetherYards);
            if (ready)
            {
                SetPhase(p, Phase::Approach, now);
                p.runStartMs = now;
                p.stuckTicks = 0;
                p.approachGaveUp = false;
                p.instance = p.mask = p.allMask = 0;
                LOG_INFO("playerbots", "[Party] pid={} dungeon approach map={} trigger={} yards={}", p.id,
                         p.dungeonMap, e->second.trigger, std::uint32_t(leader->GetExactDist2d(e->second.x, e->second.y)));
            }
        }
        why = ShouldDisband(f, gParams.keep);
        // AutoWow.Unstick.V2: a group-quest party whose quest logs have not moved for StallMs, or whose leader's
        // zone graduation gave up after the party formed, lets go (soak-s49..s51: a pinned party kept its
        // members purposeful while the leader sat in town).
        if (why == Disband::None && AutoWowUnstickV2::Enabled() && p.why == Reason::GroupQuest &&
            (AutoWowUnstickV2::PartyStalled(p.questSig, p.questSinceMs, QuestSig(bots), now,
                                            AutoWowUnstickV2::detail::gParams.partyStallMs) ||
             AutoWowUnstickV2::GaveUpSince(p.leader, p.formedMs)))
            why = Disband::Stalled;
    }
    else if (f.online < f.size)
        why = ShouldDisband(f, gParams.keep);

    if (why == Disband::None && f.online == f.size && p.phase != Phase::Stage)
        for (std::size_t i = 0; i < bots.size(); ++i)
        {
            Slot const* s = nullptr;
            for (Slot const& sl : p.slots)
                if (sl.guid == bots[i]->GetGUID().GetCounter())
                    s = &sl;
            // Followers follow while questing and on the way in; inside, RunStep re-arms follow after admission.
            if (s)
                ApplyMode(p, *s, bots[i], AiOf(bots[i]), p.phase == Phase::None || p.phase == Phase::Approach);
        }

    {
        std::lock_guard<std::mutex> guard(gLock);
        auto const it = gParties.find(id);
        if (it == gParties.end())
            return;
        // The leader thread only writes the approach fields; keep its latest values.
        p.stuckTicks = it->second.stuckTicks;
        p.approachGaveUp = p.approachGaveUp || it->second.approachGaveUp;
        it->second = p;
    }
    if (why != Disband::None)
    {
        if (p.phase != Phase::None)
        {
            Party copy = p;
            EndRun(copy, now);
        }
        Dissolve(id, why, now);
    }
}

// ---- recruitment (world thread, AutoWow.Dungeon.Recruit) ------------------------------------------------
// A cohort bot's recruit standing (RecruitOut, first match); Count = not a cohort bot (not independent, a real
// player, oracle-managed or a probe bot). `pid`: the formation party of RecruitOut::Party. Without the wide pool
// RecruitOut::Free is exactly the dparty eligibility (independent, alive, ungrouped, out of combat, on a
// continent, not on a zone trip / errand / supply role / flight / escort / squad, past its cooldown).
RecruitOut Classify(Player* bot, PlayerbotAI* ai, std::uint64_t now, std::uint32_t* pid)
{
    std::uint32_t const g = bot->GetGUID().GetCounter();
    if (!ai || !bot->IsInWorld() || !ai->IsAutoWowIndependentParty() || ai->IsRealPlayer() ||
        AutoWowOracleRuntime::IsManagedBot(g) || AutoWowDungeonProbe::IsProbeBot(g))
        return RecruitOut::Count;
    bool squad = false, inRun = false, cooling = false;
    std::uint32_t party = 0;
    {
        std::lock_guard<std::mutex> guard(gLock);
        squad = gSquadGuids.count(g) > 0;
        if (auto const it = gOf.find(g); it != gOf.end())
        {
            Party const& p = gParties.at(it->second);
            party = it->second;
            inRun = p.recruited || p.phase != Phase::None || p.why == Reason::Squad;
        }
        auto const ready = gRecruitReadyMs.find(g);
        cooling = ready != gRecruitReadyMs.end() && now < ready->second;
    }
    if (squad)
        return RecruitOut::Squad;
    if (AutoWowSupply::Enabled() && AutoWowSupply::ActiveRoleOf(bot).role != AutoWowSupply::Role::None)
        return RecruitOut::Supply;
    if (inRun)
        return RecruitOut::Run;
    if (bot->InBattleground() || !bot->GetMap() || bot->GetMap()->Instanceable())
        return RecruitOut::Instance;
    if (auto const* quest = std::get_if<NewRpgInfo::DoQuest>(&ai->rpgInfo.data);
        quest && quest->objectiveRuntime.phase == QuestActionPhase::EscortEvent)
        return RecruitOut::Escort;
    if (bot->IsInFlight() || bot->IsBeingTeleported())
        return RecruitOut::Flight;
    if (!bot->IsAlive())
        return RecruitOut::Dead;
    if (cooling)
        return RecruitOut::Cooldown;
    if (ai->IsAutoWowPaused())
        return RecruitOut::Paused;
    if (party)
    {
        *pid = party;
        return RecruitOut::Party;
    }
    if (bot->GetGroup())
        return RecruitOut::Group;
    if (bot->IsInCombat())
        return RecruitOut::Combat;
    if (AutoWowZoneProgression::Enabled() && AutoWowZoneProgression::Active(g))
        return RecruitOut::ZoneMove;
    if (AutoWowErrands::Enabled() && AutoWowErrands::Active(g))
        return RecruitOut::Errand;
    return RecruitOut::Free;
}

// A recruited party starts its run at once. Everyone on the entrance's map within MaxWalkYards: the Approach
// walk (leader walks, followers follow; ApproachTimeoutMs / stuck ticks then the ordinary portal fallback).
// Otherwise (another continent, beyond walk range) the whole party takes a logged portal to the entrance (owner
// ruling: portals are an acceptable logged travel fallback; ledger `dungeon` portal_fallback and `contaminated`
// party_recruit_portal), then Stage.
void StartRecruitRun(std::uint32_t id, std::uint64_t now)
{
    Party p;
    {
        std::lock_guard<std::mutex> guard(gLock);
        auto const it = gParties.find(id);
        if (it == gParties.end())
            return;
        p = it->second;
    }
    Entrance const e = gEntrances.at(p.dungeonMap);  // PlanRecruit only picks dungeons with a known entrance
    std::vector<Player*> bots;
    bool walk = true;
    for (Slot const& s : p.slots)
        if (Player* bot = Find(s.guid))
        {
            bots.push_back(bot);
            walk = walk && bot->GetMapId() == e.map && bot->GetExactDist2d(e.x, e.y) <= float(gParams.maxWalkYards);
        }
    p.runStartMs = now;
    p.stuckTicks = 0;
    p.approachGaveUp = false;
    p.instance = p.mask = p.allMask = 0;
    if (walk)
        SetPhase(p, Phase::Approach, now);
    else
    {
        EmitRun(p, RunEvent::PortalFallback, -1, now);
        for (Player* bot : bots)
        {
            if (AutoWowQuestLedger::Enabled())
                AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Contaminated, 0, "party_recruit_portal");
            bot->TeleportTo(e.map, e.x, e.y, e.z, bot->GetOrientation());
        }
        SetPhase(p, Phase::Stage, now);
        for (Player* bot : bots)
            if (bot->GetGUID().GetCounter() != p.leader)
                AiOf(bot)->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);
    }
    LOG_INFO("playerbots", "[Party] pid={} recruit run map={} gather={}", p.id, p.dungeonMap, walk ? "walk" : "portal");
    std::lock_guard<std::mutex> guard(gLock);
    if (auto const it = gParties.find(id); it != gParties.end())
        it->second = p;
}

// Per faction below RecruitMaxParties active recruited parties: plan (PlanRecruit) and form one party.
void Recruit(std::uint64_t now)
{
    std::uint32_t active[3] = {};
    {
        std::lock_guard<std::mutex> guard(gLock);
        for (auto const& [id, party] : gParties)
            if (party.recruited && party.team < 3)
                ++active[party.team];
    }
    bool const wide = gParams.recruitWidePool;
    std::uint32_t outs[3][std::size_t(RecruitOut::Count)] = {};
    std::uint32_t tanks[3] = {}, tanksTaken[3] = {};
    std::map<std::uint32_t, std::uint32_t> pullParty;  // guid -> formation party dissolved if it is recruited
    std::unordered_set<std::uint32_t> zoneMovers;       // zone trips cancelled if recruited
    std::vector<Candidate> cands;
    for (auto const& [guid, bot] : ObjectAccessor::GetPlayers())
    {
        std::uint32_t pid = 0;
        RecruitOut const o = Classify(bot, AiOf(bot), now, &pid);
        if (o == RecruitOut::Count)
            continue;
        std::uint8_t const t = TeamOf(bot);
        ++outs[t][std::size_t(o)];
        std::uint32_t const cls = bot->getClass();
        // AutoWow.Dungeon.RequireTank: warrior, paladin, or a druid that knows Bear / Dire Bear Form.
        bool const canTank = cls == kWarrior || cls == kPaladin ||
                             (cls == kDruid && (bot->HasSpell(kBearForm) || bot->HasSpell(kDireBearForm)));
        tanks[t] += canTank;
        if (!RecruitTakes(o, wide))
            continue;
        tanksTaken[t] += canTank;
        if (o == RecruitOut::Party)
            pullParty[guid.GetCounter()] = pid;
        if (o == RecruitOut::ZoneMove)
            zoneMovers.insert(guid.GetCounter());
        Candidate c;
        c.guid = guid.GetCounter();
        c.level = bot->GetLevel();
        c.cls = bot->getClass();
        c.team = TeamOf(bot);
        c.map = bot->GetMapId();
        c.zone = bot->GetZoneId();
        c.x = Yd(bot->GetPositionX());
        c.y = Yd(bot->GetPositionY());
        c.canTank = canTank;
        cands.push_back(c);
    }
    std::vector<EntranceSpot> spots;
    for (DungeonDef const& d : gDungeons)
    {
        auto const e = gEntrances.find(d.map);
        spots.push_back(e == gEntrances.end() ? EntranceSpot{}
                                              : EntranceSpot{e->second.map, Yd(e->second.x), Yd(e->second.y), true});
    }
    for (std::uint8_t const team : {kAlliance, kHorde})
    {
        std::uint32_t const eligible = std::uint32_t(
            std::count_if(cands.begin(), cands.end(), [team](Candidate const& c) { return c.team == team; }));
        if (active[team] >= gParams.recruitMaxParties)
            continue;
        char const* why = "";
        Plan const pl = PlanRecruit(cands, team, gDungeons, spots, gParams.recruit, &why);
        bool const tanked = std::find(pl.roles.begin(), pl.roles.end(), Role::Tank) != pl.roles.end();
        LOG_INFO("playerbots", "[Party] recruit team={} eligible={} dmap={} members={} why={} tanks={}/{} wide={} "
                 "tank_dlvl={}{}", team, eligible, pl.dungeonMap, pl.guids.size(), why, tanksTaken[team], tanks[team],
                 wide, tanked ? std::to_string(pl.tankDelta) : std::string("none"), RecruitOutFields(outs[team]));
        if (pl.guids.empty())
            continue;
        // RecruitWidePool: leave the formation party, drop the zone trip (an errand resumes or times out later).
        for (std::uint32_t const g : pl.guids)
        {
            if (auto const it = pullParty.find(g); it != pullParty.end())
                Dissolve(it->second, Disband::Recruited, now);  // no-op when already dissolved
            if (zoneMovers.count(g))
                AutoWowZoneProgression::CancelTrip(g, now);
        }
        Party party;
        party.recruited = true;
        party.team = team;
        if (std::uint32_t const id = Materialize(pl, party, now))
            StartRecruitRun(id, now);
    }
}
}  // namespace

// ---- public ---------------------------------------------------------------------------------------------
void LoadConfig()
{
    detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Party.Enable", false);
    detail::gDungeons = detail::gEnabled && sConfigMgr->GetOption<bool>("AutoWow.Dungeon.Enable", false);
    detail::gRoles = detail::gEnabled && sConfigMgr->GetOption<bool>("AutoWow.Party.Roles", true);
    Params& p = gParams;
    p.checkIntervalMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Party.CheckIntervalMs", 30000);
    p.form.levelSpread = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Party.LevelSpread", 3);
    p.form.hubYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Party.HubYards", 300);
    p.form.minSize = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Party.MinSize", 2);
    p.form.maxSize = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Party.MaxSize", 5);
    p.form.dungeonMinSize = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Dungeon.MinSize", 3);
    p.form.requireTankHealer = sConfigMgr->GetOption<bool>("AutoWow.Dungeon.RequireTankHealer", true);
    p.keep.levelSpread = p.form.levelSpread;
    p.keep.separatedMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Party.SeparatedMs", 600000);
    p.keep.purposelessMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Party.PurposelessMs", 1800000);
    p.keep.maxAgeMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Party.MaxAgeMs", 7200000);
    p.separatedYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Party.SeparatedYards", 500);
    p.rejoinCooldownMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Party.RejoinCooldownMs", 600000);
    p.healPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Party.HealPct", 60);
    p.tauntYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Party.TauntYards", 30);
    p.maxWalkYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Dungeon.MaxWalkYards", 6000);
    p.togetherYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Dungeon.TogetherYards", 60);
    p.approachTimeoutMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Dungeon.ApproachTimeoutMs", 1800000);
    p.approachStuckTicks = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Dungeon.ApproachStuckTicks", 60);
    p.stageYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Dungeon.StageYards", 40);
    p.stageTimeoutMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Dungeon.StageTimeoutMs", 120000);
    p.memberGraceMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Dungeon.MemberGraceMs", 180000);
    p.insideTimeoutMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Dungeon.InsideTimeoutMs", 3600000);
    p.exitTimeoutMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Dungeon.ExitTimeoutMs", 120000);
    p.runCooldownMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Dungeon.CooldownMs", 3600000);
    p.portalFallback = sConfigMgr->GetOption<bool>("AutoWow.Dungeon.PortalFallback", false);
    p.disbandOrphans = sConfigMgr->GetOption<bool>("AutoWow.Party.DisbandOrphans", true);
    detail::gRecruit = detail::gDungeons && sConfigMgr->GetOption<bool>("AutoWow.Dungeon.Recruit", false);
    if (detail::gRecruit)
    {
        p.recruitIntervalMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Dungeon.RecruitIntervalMs", 600000);
        p.recruitCooldownMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Dungeon.RecruitCooldownMs", 2700000);
        p.recruitMaxParties = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Dungeon.RecruitMaxParties", 1);
        p.recruit.levelSpread = p.form.levelSpread;
        p.recruit.minSize = p.form.dungeonMinSize;
        p.recruit.maxSize = p.form.maxSize;
        p.recruit.walkYards = p.maxWalkYards;
        p.recruit.requireTank = sConfigMgr->GetOption<bool>("AutoWow.Dungeon.RequireTank", false);
        detail::gRecruitWalk = sConfigMgr->GetOption<bool>("AutoWow.Dungeon.RecruitStragglerWalk", false);
        p.recruitWidePool = sConfigMgr->GetOption<bool>("AutoWow.Dungeon.RecruitWidePool", false);
        p.recruitGatherPortalMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Dungeon.RecruitGatherPortalMs", 0);
        p.recruit.tankOverLevel = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Dungeon.RecruitTankOverLevel", 0);
    }
    gDungeons = ParseDungeons(sConfigMgr->GetOption<std::string>(
        "AutoWow.Dungeon.List", "389:13:18:H,36:17:26:A,43:17:24:AH,33:18:25:AH,48:20:30:AH,34:22:30:A"));
    if (!detail::gEnabled)
        return;

    // Entrances: the lowest exterior area trigger (continent side) that ports into each listed dungeon.
    gEntrances.clear();
    for (auto const& [triggerId, tp] : sObjectMgr->GetAllAreaTriggerTeleports())
    {
        AreaTrigger const* at = sObjectMgr->GetAreaTrigger(triggerId);
        MapEntry const* from = at ? sMapStore.LookupEntry(at->map) : nullptr;
        if (!from || !from->IsContinent() ||
            std::none_of(gDungeons.begin(), gDungeons.end(), [&](DungeonDef const& d) { return d.map == tp.target_mapId; }))
            continue;
        auto const it = gEntrances.find(tp.target_mapId);
        if (it == gEntrances.end() || triggerId < it->second.trigger)
            gEntrances[tp.target_mapId] = {triggerId, at->map, at->x, at->y, at->z};
    }
    // Group quests by zone (formation signal).
    gZoneGroupQuests.clear();
    for (auto const& [id, q] : sObjectMgr->GetQuestTemplates())
        if (q && q->GetZoneOrSort() > 0 && IsGroupQuest(q->GetType(), q->GetSuggestedPlayers()))
            gZoneGroupQuests[std::uint32_t(q->GetZoneOrSort())].push_back({id, q->GetMinLevel(), q->GetQuestLevel()});
    for (auto& [zone, list] : gZoneGroupQuests)
        std::sort(list.begin(), list.end(), [](QuestOffer const& a, QuestOffer const& b) { return a.quest < b.quest; });
    LOG_INFO("server.loading", ">> AutoWow.Party: {} dungeons listed, {} entrances, {} zones with group quests, "
             "dungeons={} roles={}", gDungeons.size(), gEntrances.size(), gZoneGroupQuests.size(), detail::gDungeons,
             detail::gRoles);
}

void WorldUpdate(std::uint32_t diff)
{
    gSinceSupervise += diff;
    gSinceForm += diff;
    if (detail::gRecruit)
        gSinceRecruit += diff;
    if (gSinceSupervise < kSuperviseMs)
        return;
    gSinceSupervise = 0;
    std::uint64_t const now = NowMs();
    std::vector<std::uint32_t> ids;
    {
        std::lock_guard<std::mutex> guard(gLock);
        for (auto const& [id, party] : gParties)
            ids.push_back(id);
    }
    for (std::uint32_t id : ids)
        Supervise(id, now);
    if (detail::gRecruit && gSinceRecruit >= gParams.recruitIntervalMs)
    {
        gSinceRecruit = 0;
        Recruit(now);
    }
    if (gSinceForm >= gParams.checkIntervalMs)
    {
        gSinceForm = 0;
        Form(now);
    }
}

std::uint32_t PartySize(std::uint32_t guid, bool* dungeonParty)
{
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gOf.find(guid);
    if (it == gOf.end())
        return 0;
    Party const& p = gParties.at(it->second);
    if (dungeonParty)
        *dungeonParty = p.why == Reason::Dungeon;
    return std::uint32_t(p.slots.size());
}

bool InRecruitedParty(std::uint32_t guid)
{
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gOf.find(guid);
    return it != gOf.end() && gParties.at(it->second).recruited;
}

bool GetLeaderOrder(std::uint32_t guid, LeaderOrder& out)
{
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gOf.find(guid);
    if (it == gOf.end())
        return false;
    Party const& p = gParties.at(it->second);
    if (p.leader != guid || p.why == Reason::Squad)
        return false;
    out.phase = p.phase;
    auto const e = gEntrances.find(p.dungeonMap);
    if (e != gEntrances.end())
    {
        out.map = e->second.map;
        out.x = e->second.x;
        out.y = e->second.y;
        out.z = e->second.z;
    }
    return true;
}

void NoteApproachTick(std::uint32_t guid, bool stuck)
{
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gOf.find(guid);
    if (it == gOf.end())
        return;
    Party& p = gParties.at(it->second);
    p.stuckTicks = stuck ? p.stuckTicks + 1 : 0;
    if (p.stuckTicks >= gParams.approachStuckTicks)
        p.approachGaveUp = true;
}

void EnsureSquad(std::vector<std::uint32_t> const& roster)
{
    std::uint64_t const now = NowMs();
    // The online roster: in world, a bot not managed by the oracle, not in a battleground or instance.
    std::vector<std::uint32_t> online;
    for (std::uint32_t const g : roster)
    {
        Player* bot = Find(g);
        PlayerbotAI* ai = AiOf(bot);
        if (bot && ai && bot->IsInWorld() && !ai->IsRealPlayer() && !AutoWowOracleRuntime::IsManagedBot(g) &&
            !bot->InBattleground() && bot->GetMap() && !bot->GetMap()->Instanceable())
            online.push_back(g);
    }
    std::uint32_t id = 0;
    std::vector<std::uint32_t> slots;
    {
        std::lock_guard<std::mutex> guard(gLock);
        gSquadGuids.insert(roster.begin(), roster.end());
        for (auto const& [pid, party] : gParties)
            if (party.why == Reason::Squad && std::find(roster.begin(), roster.end(), party.leader) != roster.end())
            {
                id = pid;
                for (Slot const& s : party.slots)
                    slots.push_back(s.guid);
            }
    }
    if (id)
    {
        Group* group = nullptr;
        bool intact = slots == online;
        for (std::uint32_t const g : slots)
        {
            Player* bot = Find(g);
            Group* mine = bot ? bot->GetGroup() : nullptr;
            if (!group)
                group = mine;
            intact = intact && mine && mine == group;
        }
        if (intact && group && group->GetMembersCount() == slots.size())
            return;
        Dissolve(id, Disband::MemberOffline, now);
    }
    // A bot-only orphan group of roster members only (the core persists groups across a restart, this registry
    // does not) is disbanded; members in any other group stay out until it ends.
    for (std::uint32_t const g : online)
        if (Group* og = Find(g)->GetGroup(); og && IsOrphan(og) &&
            std::all_of(og->GetMemberSlots().begin(), og->GetMemberSlots().end(), [&](Group::MemberSlot const& m)
                        { return std::find(roster.begin(), roster.end(), m.guid.GetCounter()) != roster.end(); }))
        {
            std::vector<Player*> members;
            for (Group::MemberSlot const& m : og->GetMemberSlots())
                members.push_back(ObjectAccessor::FindPlayer(m.guid));
            LOG_INFO("playerbots", "[Party] orphan squad group of {} ({} members) disbanded", Find(g)->GetName(),
                     members.size());
            og->Disband();  // deletes the group
            for (Player* m : members)
                if (PlayerbotAI* mai = AiOf(m))
                    mai->SetMaster(nullptr);
        }
    std::vector<Player*> bots;
    for (std::uint32_t const g : online)
        if (Player* bot = Find(g); bot && !bot->GetGroup())
            bots.push_back(bot);
    if (bots.size() < 2)
        return;
    Party party;
    party.leader = static_cast<std::uint32_t>(bots.front()->GetGUID().GetCounter());  // lowest online guid
    party.why = Reason::Squad;
    party.formedMs = now;
    for (Player* bot : bots)
    {
        PlayerbotAI* ai = AiOf(bot);
        Slot s;
        s.guid = static_cast<std::uint32_t>(bot->GetGUID().GetCounter());
        s.cls = bot->getClass();
        s.combat0 = ai->GetStrategies(BOT_STATE_COMBAT);
        s.nonCombat0 = ai->GetStrategies(BOT_STATE_NON_COMBAT);
        party.slots.push_back(std::move(s));
    }
    // Core group: the bridge CreateParty idiom (bot-only, ordinary party), as Form.
    Group* group = new Group;
    if (!group->Create(bots.front()))
    {
        delete group;
        return;
    }
    sGroupMgr->AddGroup(group);
    for (Player* bot : bots)
        if (bot != bots.front() && !group->AddMember(bot))
        {
            group->Disband();
            return;
        }
    group->SetLootMethod(NEED_BEFORE_GREED);
    group->SetLootThreshold(ITEM_QUALITY_UNCOMMON);
    {
        std::lock_guard<std::mutex> guard(gLock);
        party.id = gNextId++;
        for (Slot const& s : party.slots)
            gOf[s.guid] = party.id;
        gParties[party.id] = party;
    }
    std::vector<Role> roles;
    for (Player* bot : bots)
    {
        PlayerbotAI* ai = AiOf(bot);
        ai->SetAutoWowIndependentParty(true);  // native group maintenance leaves the roster alone
        ai->SetMaster(nullptr);                // no follower mode: every member runs its own loop
        roles.push_back(Role::Dps);
    }
    Emit(party, false, "formed", PartyFields(party.id, Guids(party), roles, party.leader, party.why, 0, 0));
    LOG_INFO("playerbots", "[Party] pid={} formed why=squad leader={} members={}", party.id, bots.front()->GetName(),
             bots.size());
}

void CombatUpdate(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!bot || !bot->IsAlive() || !bot->IsInCombat() || bot->IsNonMeleeSpellCast(false))
        return;
    std::uint32_t const guid = bot->GetGUID().GetCounter();
    std::uint64_t const now = NowMs();
    std::vector<std::pair<std::uint32_t, Role>> roster;
    Role mine = Role::Dps;
    {
        std::lock_guard<std::mutex> guard(gLock);
        auto const it = gOf.find(guid);
        if (it == gOf.end())
            return;
        auto const next = gNextCastMs.find(guid);
        if (next != gNextCastMs.end() && now < next->second)
            return;
        for (Slot const& s : gParties.at(it->second).slots)
        {
            roster.emplace_back(s.guid, s.role);
            if (s.guid == guid)
                mine = s.role;
        }
    }
    if (mine == Role::Dps)
        return;

    // Members on this bot's map only (same map thread).
    std::vector<std::pair<Player*, Role>> party;
    for (auto const& [g, role] : roster)
        if (Player* m = Find(g); m && m->IsInWorld() && m->GetMap() == bot->GetMap())
            party.emplace_back(m, role);

    Unit* target = nullptr;
    std::vector<char const*> spells;
    if (mine == Role::Tank)
    {
        std::vector<Threat> threats;
        std::vector<Unit*> units;
        for (auto const& [m, role] : party)
        {
            if (m == bot || !m->IsAlive())
                continue;
            for (Unit* a : m->getAttackers())
                if (a && a->IsAlive() && a->GetTypeId() == TYPEID_UNIT && a->GetVictim() == m)
                {
                    threats.push_back({a->GetGUID().GetCounter(), role, HpPct(m), std::uint32_t(bot->GetDistance(a))});
                    units.push_back(a);
                }
        }
        std::uint32_t const mob = PickTauntTarget(threats, gParams.tauntYards);
        for (std::size_t i = 0; mob && i < threats.size(); ++i)
            if (threats[i].mob == mob)
                target = units[i];
        spells = TauntSpells(bot->getClass());
    }
    else
    {
        std::vector<Wounded> wounded;
        for (auto const& [m, role] : party)
            if (bot->GetDistance(m) <= 40.0f)
                wounded.push_back({m->GetGUID().GetCounter(), role, HpPct(m), m->IsAlive()});
        if (std::uint32_t const g = PickHealTarget(wounded, gParams.healPct))
            for (auto const& [m, role] : party)
                if (m->GetGUID().GetCounter() == g)
                    target = m;
        spells = HealSpells(bot->getClass());
    }
    if (!target)
        return;
    for (char const* spell : spells)
        if (botAI->CanCastSpell(spell, target) && botAI->CastSpell(spell, target))
        {
            std::lock_guard<std::mutex> guard(gLock);
            gNextCastMs[guid] = now + kCastSpacingMs;
            LOG_DEBUG("playerbots", "[Party] bot={} {} {} on {}", bot->GetName(), RoleName(mine), spell, target->GetName());
            return;
        }
}
}  // namespace AutoWowParty
