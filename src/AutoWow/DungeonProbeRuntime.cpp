/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// AutoWow.DungeonProbe runtime (policy: DungeonProbePolicy.h). World thread only: every TickMs each probe party
// steps its Prepare -> Enter -> Inside -> Exit machine. Only the accepted probe guids are ever resolved; the
// dungeon navigator (stock "dungeon navigator" instance strategy, party-leader driven) does the actual run.

#include <map>
#include <set>

#include "AutoWowBridge.h"
#include "AutoWowGuildsPolicy.h"
#include "AutoWowQuestLedger.h"
#include "CharacterCache.h"
#include "Config.h"
#include "DBCStores.h"
#include "DungeonNavigator.h"
#include "DungeonProbePolicy.h"
#include "FixtureFactoryControl.h"
#include "GameTime.h"
#include "GatheringWorkerState.h"
#include "Group.h"
#include "GroupMgr.h"
#include "InstanceSaveMgr.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PartyPolicy.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "ProbePlacePolicy.h"
#include "RandomPlayerbotMgr.h"
#include "SpellAuras.h"
#include "SquadPolicy.h"
#include "SupplyPolicy.h"
#include "World.h"

namespace AutoWowDungeonProbe
{
namespace
{
struct Params
{
    std::uint32_t tickMs = 1000;
    std::vector<std::uint32_t> queue;
    std::vector<LevelOverride> levels;
    std::uint32_t levelOver = 2;
    std::uint32_t quality = 4;  // best rung of the exact-quality gear ladder (epic)
    InsideParams inside;
    std::uint64_t stuckMs = 300000;
    std::uint32_t stuckYards = 10;
    std::uint64_t prepareTimeoutMs = 600000;
    std::uint64_t enterTimeoutMs = 180000;
    std::uint64_t exitTimeoutMs = 120000;
    std::uint32_t loops = 1;  // queue passes per party; 0 = forever
};

// Instance-side landing point of the dungeon's entrance trigger (probe teleport target).
struct Entry
{
    std::uint32_t trigger = 0;
    std::uint32_t map = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f, o = 0.0f;
};

enum class Phase : std::uint8_t
{
    Prepare = 0,
    Enter = 1,
    Inside = 2,
    Exit = 3,
    Done = 4
};

struct Member
{
    std::uint32_t guid = 0;
    std::uint32_t cls = 0;
    AutoWowParty::Role role = AutoWowParty::Role::Dps;
    bool wasAlive = true;
};

struct Probe
{
    std::uint8_t version = kStateVersion;
    std::string name;
    std::vector<Member> members;  // guid ascending
    std::uint32_t team = TEAM_NEUTRAL;  // of the first member (character cache); skips enemy-capital entrances
    std::uint32_t leader = 0;
    bool rolesKnown = false;
    std::size_t next = 0;         // queue index
    std::uint32_t pass = 0;       // completed queue passes
    Phase phase = Phase::Prepare;
    std::uint64_t phaseMs = 0;
    std::uint64_t nextLoginMs = 0;
    bool entered = false;
    std::uint64_t enteredMs = 0;
    std::uint64_t quietDeadSinceMs = 0;
    StuckTracker stuck;
    RunRecord run;
};

Params gParams;
std::map<std::uint32_t, Entry> gEntries;  // dungeon map -> entry
std::vector<Probe> gProbes;               // config order
std::uint32_t gNextRunId = 1;
std::uint32_t gSince = 0;

constexpr char const* kFollowerNc = "+follow,-new rpg,-grind,-move random";

std::uint64_t NowMs() { return static_cast<std::uint64_t>(std::max<int64>(0, GameTime::GetGameTimeMS().count())); }

// Connected, also while a far teleport has it out of the world (FindPlayer would drop it mid-teleport).
Player* Find(std::uint32_t guid)
{
    return ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
}

// In a map and not moving between maps: safe to read its map and position and to act on it.
bool Settled(Player* bot) { return bot->IsInWorld() && !bot->IsBeingTeleported(); }

PlayerbotAI* AiOf(Player* p) { return p ? PlayerbotsMgr::instance().GetPlayerbotAI(p) : nullptr; }

std::int32_t Yd(float v) { return static_cast<std::int32_t>(v); }

void Emit(Probe const& p, char const* reason, std::int32_t enc = -1, StuckPoint const* s = nullptr)
{
    std::string const fields = Fields(p.run, NowMs(), enc, s);
    LOG_INFO("playerbots", "[DProbe] party={} {} rid={} dmap={}{}", p.name, reason, p.run.rid, p.run.map, fields);
    if (!AutoWowQuestLedger::Enabled())
        return;
    Player* who = Find(p.leader);
    for (std::size_t i = 0; !who && i < p.members.size(); ++i)
        who = Find(p.members[i].guid);
    if (who)
        AutoWowQuestLedger::EmitDungeonProbe(who, reason, fields);
}

// All connected members (in member order; in transit included), or empty when anyone logged out.
std::vector<Player*> Online(Probe const& p)
{
    std::vector<Player*> bots;
    for (Member const& m : p.members)
    {
        Player* bot = Find(m.guid);
        if (!bot || !AiOf(bot))
            return {};
        bots.push_back(bot);
    }
    return bots;
}

void Revive(Player* bot)
{
    if (bot->IsAlive())
        return;
    bot->ResurrectPlayer(1.0f, false);
    bot->SpawnCorpseBones();
}

void Refresh(Player* bot)
{
    Revive(bot);
    bot->RemoveAppliedAuras([](AuraApplication const* a) { return !a->IsPositive() && !a->GetBase()->IsPassive(); });
    bot->SetHealth(bot->GetMaxHealth());
    bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));
}

// Leader leads (the dungeon navigator needs the group leader, no master, no New-RPG); followers follow it.
// Idempotent: re-applied every tick because teleports and fixture resets rebuild strategies.
void ApplyMode(Probe const& p, Member const& m, Player* bot)
{
    PlayerbotAI* ai = AiOf(bot);
    if (!ai)
        return;
    ai->SetAutoWowIndependentParty(true);  // native group maintenance leaves the roster alone; always active
    if (ai->IsAutoWowPaused())
        ai->SetAutoWowPaused(false);
    if (m.guid == p.leader)
    {
        if (ai->GetMaster())
            ai->SetMaster(nullptr);
        if (ai->HasStrategy("follow", BOT_STATE_NON_COMBAT))
            ai->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);
        if (ai->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
            ai->ChangeStrategy("-new rpg", BOT_STATE_NON_COMBAT);
    }
    else
    {
        Player* leader = Find(p.leader);
        if (leader && ai->GetMaster() != leader)
            ai->SetMaster(leader);
        if (!ai->HasStrategy("follow", BOT_STATE_NON_COMBAT) || ai->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
            ai->ChangeStrategy(kFollowerNc, BOT_STATE_NON_COMBAT);
    }
    AutoWowParty::RoleStrategies const r = AutoWowParty::StrategiesFor(m.cls, m.role);
    char const* marker = m.role == AutoWowParty::Role::Tank ? "tank assist"
                         : m.role == AutoWowParty::Role::Healer
                             ? (m.cls == AutoWowParty::kShaman || m.cls == AutoWowParty::kDruid ? "resto" : "heal")
                             : nullptr;
    if (!marker || r.combat.empty() || ai->HasStrategy(marker, BOT_STATE_COMBAT))
        return;
    ai->ChangeStrategy(r.combat, BOT_STATE_COMBAT);
    if (!r.nonCombat.empty())
        ai->ChangeStrategy(r.nonCombat, BOT_STATE_NON_COMBAT);
}

void Advance(Probe& p, std::uint64_t now)
{
    ++p.next;
    p.phase = Phase::Prepare;
    p.phaseMs = now;
    p.entered = false;
}

// A run ends: `run` row, then Exit (homebind, disband, unbind; a settled member still inside is sent again).
void Finish(Probe& p, End end, std::uint64_t now)
{
    p.run.end = end;
    Emit(p, "run");
    p.phase = Phase::Exit;
    p.phaseMs = now;
}

// A non-permanent bind to this dungeon goes, so the next entry gets a fresh instance.
void Unbind(Player* bot, std::uint32_t map)
{
    if (sInstanceSaveMgr->PlayerGetBoundInstance(bot->GetGUID(), map, DUNGEON_DIFFICULTY_NORMAL))
        sInstanceSaveMgr->PlayerUnbindInstance(bot->GetGUID(), map, DUNGEON_DIFFICULTY_NORMAL, true, bot);
}

void DisbandOwn(Probe const& p, Group* g)
{
    for (Group::MemberSlot const& s : g->GetMemberSlots())
        if (std::none_of(p.members.begin(), p.members.end(),
                         [&s](Member const& m) { return m.guid == s.guid.GetCounter(); }))
            return;  // a foreign member: never ours to disband
    g->Disband();    // deletes the group
}

// ---- Prepare ------------------------------------------------------------------------------------------------
void StepPrepare(Probe& p, std::uint64_t now)
{
    if (p.next >= gParams.queue.size())
    {
        ++p.pass;
        if (gParams.loops && p.pass >= gParams.loops)
        {
            p.phase = Phase::Done;
            for (Member const& m : p.members)
                if (Player* bot = Find(m.guid))
                {
                    if (Group* g = bot->GetGroup())
                        DisbandOwn(p, g);
                    if (PlayerbotAI* ai = AiOf(bot))
                    {
                        ai->SetMaster(nullptr);
                        ai->ChangeStrategy("-follow,-new rpg,-grind", BOT_STATE_NON_COMBAT);
                    }
                }
            LOG_INFO("playerbots", "[DProbe] party={} done after {} passes", p.name, p.pass);
            return;
        }
        p.next = 0;
    }
    std::uint32_t const map = gParams.queue[p.next];
    if (EntranceInEnemyCapital(p.team, map))
    {
        LOG_INFO("playerbots", "[DProbe] party={} map={} skipped: entrance in an enemy capital", p.name, map);
        ++p.next;
        return;
    }
    if (!p.run.rid || p.run.map != map || p.run.end != End::None)
    {
        p.run = RunRecord{};
        p.run.party = p.name;
        p.run.rid = gNextRunId++;
        p.run.map = map;
        p.run.startMs = now;
        for (Member const& m : p.members)
            p.run.members.push_back(m.guid);
        p.run.deaths.assign(p.members.size(), 0);
        p.phaseMs = now;
    }
    std::uint32_t const level = BandLevel(map, gParams.levelOver, gParams.levels,
                                          static_cast<std::uint32_t>(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL)));
    auto const e = gEntries.find(map);
    if (!level || e == gEntries.end())
    {
        LOG_ERROR("playerbots", "[DProbe] party={} map={} has no {}", p.name, map, level ? "entrance" : "level band");
        Finish(p, End::PrepareFailed, now);
        return;
    }

    // Everyone online, settled, out of combat; offline members are logged in (probe-login idiom).
    std::vector<Player*> bots;
    bool waiting = false;
    for (Member const& m : p.members)
    {
        Player* bot = Find(m.guid);
        if (!bot || !bot->IsInWorld() || !AiOf(bot))
        {
            waiting = true;
            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(m.guid);
            if (!bot && now >= p.nextLoginMs && sCharacterCache->GetCharacterAccountIdByGuid(guid))
            {
                AutoWowGather::DeactivateWorker(m.guid);
                AutoWowPolicy::SetNoTeleport(m.guid, false);
                sRandomPlayerbotMgr.AddPlayerBot(guid, 0);
            }
            continue;
        }
        if (bot->IsBeingTeleported() || bot->IsInFlight() || bot->IsInCombat())
            waiting = true;
        else if (bot->GetMap()->IsDungeon())
        {
            // Left inside a dungeon (the soak stopped mid-run): out first. Disbanding its group in there marks
            // the instance invalid for it, an instance-to-instance teleport keeps that, and the core repops it
            // at the graveyard 60 s later (S53 Deadmines: both parties abandoned at 61 s).
            waiting = true;
            Revive(bot);
            bot->TeleportTo(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ,
                            bot->GetOrientation());
        }
        else
            ApplyMode(p, m, bot);  // probe instruments never run their own New-RPG loop, not even while waiting
        bots.push_back(bot);
    }
    if (waiting && now >= p.nextLoginMs)
        p.nextLoginMs = now + 60000;
    if (waiting)
    {
        if (now >= p.phaseMs + gParams.prepareTimeoutMs)
        {
            LOG_ERROR("playerbots", "[DProbe] party={} map={} members not ready in {} ms", p.name, map,
                      gParams.prepareTimeoutMs);
            Finish(p, End::PrepareFailed, now);
        }
        return;
    }

    // Roles once (classes are fixed): tank leads (AssignRoles / ChooseLeader).
    if (!p.rolesKnown)
    {
        std::vector<AutoWowParty::Member> ms;
        for (std::size_t i = 0; i < bots.size(); ++i)
        {
            p.members[i].cls = bots[i]->getClass();
            ms.push_back({p.members[i].guid, bots[i]->GetLevel(), p.members[i].cls});
        }
        std::vector<AutoWowParty::Role> const roles = AutoWowParty::AssignRoles(ms);
        for (std::size_t i = 0; i < roles.size(); ++i)
            p.members[i].role = roles[i];
        p.leader = p.members[AutoWowParty::ChooseLeader(ms, roles)].guid;
        p.rolesKnown = true;
    }

    // A group that is not exactly ours blocks the run (never disband foreign members).
    for (Player* bot : bots)
        if (Group* g = bot->GetGroup())
        {
            DisbandOwn(p, g);
            if (bot->GetGroup())
            {
                LOG_ERROR("playerbots", "[DProbe] party={} member {} is in a foreign group", p.name, bot->GetName());
                Finish(p, End::PrepareFailed, now);
                    return;
            }
        }
    for (Player* bot : bots)
        Unbind(bot, map);  // a run cut short (soak stop) would otherwise re-enter its old instance

    // Magic setup: exact level, fixed spec, best available exact-quality gear, pet, fresh state.
    std::uint32_t quality = 5;
    for (std::size_t i = 0; i < bots.size(); ++i)
    {
        Player* bot = bots[i];
        Revive(bot);
        std::string result;
        std::uint32_t granted = 0;
        for (std::uint32_t q : QualityLadder(gParams.quality))
        {
            result = AutoWowFixture::SetLevel(p.members[i].guid, level, SpecFor(p.members[i].cls), q);
            if (result.rfind("{\"ok\":true", 0) == 0)
            {
                granted = q;
                break;
            }
        }
        if (!granted)
        {
            LOG_ERROR("playerbots", "[DProbe] party={} map={} fixture failed for {}: {}", p.name, map, bot->GetName(),
                      result);
            Finish(p, End::PrepareFailed, now);
            return;
        }
        quality = std::min(quality, granted);
        if (bot->getClass() == CLASS_HUNTER)
        {
            PlayerbotFactory factory(bot, level);
            factory.InitPet();
            factory.InitPetTalents();
        }
        Refresh(bot);
    }
    p.run.level = level;
    p.run.quality = quality;

    // A fresh bot-only party (the bridge CreateParty idiom): leader first, loot need before greed.
    Player* leader = Find(p.leader);
    Group* group = new Group;
    if (!group->Create(leader))
    {
        delete group;
        Finish(p, End::PrepareFailed, now);
        return;
    }
    sGroupMgr->AddGroup(group);
    for (Player* bot : bots)
        if (bot != leader && !group->AddMember(bot))
        {
            group->Disband();
            Finish(p, End::PrepareFailed, now);
            return;
        }
    group->SetLootMethod(NEED_BEFORE_GREED);
    group->SetLootThreshold(ITEM_QUALITY_UNCOMMON);
    for (std::size_t i = 0; i < bots.size(); ++i)
    {
        p.members[i].wasAlive = true;
        ApplyMode(p, p.members[i], bots[i]);
    }
    LOG_INFO("playerbots", "[DProbe] party={} rid={} prepared map={} level={} quality={} trigger={}", p.name, p.run.rid,
             map, level, quality, e->second.trigger);
    p.phase = Phase::Enter;
    p.phaseMs = now;
}

// ---- Enter (also re-entry after a wipe) -----------------------------------------------------------------------
void StepEnter(Probe& p, std::uint64_t now)
{
    std::vector<Player*> const bots = Online(p);
    if (bots.empty())
    {
        p.run.why = "member_offline";
        Finish(p, End::Abandoned, now);
        return;
    }
    Entry const& e = gEntries.at(p.run.map);
    Player* leader = Find(p.leader);
    bool const leaderIn = Settled(leader) && leader->GetMapId() == e.map && leader->GetInstanceId();
    bool allIn = leaderIn;
    for (std::size_t i = 0; i < bots.size(); ++i)
    {
        Player* bot = bots[i];
        if (!Settled(bot))
        {
            allIn = false;  // in transit
            continue;
        }
        ApplyMode(p, p.members[i], bot);
        bool const in = bot->GetMapId() == e.map && bot->GetInstanceId() &&
                        (!leaderIn || bot->GetInstanceId() == leader->GetInstanceId());
        allIn = allIn && in;
        // Leader first: the followers' teleport resolves the leader's instance bind (PlayerGetDestinationInstanceId).
        if (in || (bot != leader && !leaderIn))
            continue;
        Revive(bot);
        if (bot->GetMapId() == e.map)
        {
            // Another copy of the dungeon (a same-map teleport keeps the instance): out first, then in by the bind.
            LOG_INFO("playerbots", "[DProbe] party={} {} in instance {} not {}: re-entering", p.name, bot->GetName(),
                     bot->GetInstanceId(), leaderIn ? leader->GetInstanceId() : 0);
            bot->TeleportTo(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ,
                            bot->GetOrientation());
            continue;
        }
        if (AutoWowQuestLedger::Enabled())
            AutoWowQuestLedger::Emit(bot, AutoWowQuestLedger::Event::Contaminated, 0, "dprobe_teleport");
        bot->TeleportTo(e.map, e.x, e.y, e.z, e.o);
    }
    if (allIn)
    {
        p.phase = Phase::Inside;
        p.phaseMs = now;
        p.quietDeadSinceMs = 0;
        p.stuck = {};
        if (!p.entered)
        {
            p.entered = true;
            p.enteredMs = now;
            p.run.instance = leader->GetInstanceId();
            std::vector<EncounterRecord> records;
            if (DungeonEncounterList const* list = sObjectMgr->GetDungeonEncounterList(e.map, DUNGEON_DIFFICULTY_NORMAL))
            {
                std::set<std::uint32_t> credits;
                for (DungeonEncounter const* enc : *list)
                    if (enc->creditType == ENCOUNTER_CREDIT_KILL_CREATURE)
                        credits.insert(enc->creditEntry);
                std::set<std::uint32_t> spawned;
                for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
                    if (data.mapid == e.map)
                        for (std::uint32_t id : {data.id, data.id2, data.id3})
                            if (id && credits.count(id))
                                spawned.insert(id);
                for (DungeonEncounter const* enc : *list)
                {
                    bool const kill = enc->creditType == ENCOUNTER_CREDIT_KILL_CREATURE;
                    records.push_back({enc->dbcEntry->encounterIndex, kill, kill && spawned.count(enc->creditEntry)});
                    if (kill && !spawned.count(enc->creditEntry))
                        LOG_INFO("playerbots", "[DProbe] party={} map={} encounter={} boss={} no_static_spawn", p.name,
                                 e.map, enc->dbcEntry->encounterIndex, enc->creditEntry);
                }
            }
            p.run.allMask = ClearableMask(records);
            InstanceMap* im = leader->GetMap()->ToInstanceMap();
            InstanceScript* script = im ? im->GetInstanceScript() : nullptr;
            p.run.mask = script ? script->GetCompletedEncounterMask() : 0;
            Emit(p, "entered");
        }
        return;
    }
    if (now >= p.phaseMs + gParams.enterTimeoutMs)
    {
        for (Player* bot : bots)
            LOG_INFO("playerbots", "[DProbe] party={} enter_failed {} map={} inst={} teleporting={}", p.name,
                     bot->GetName(), bot->GetMapId(), bot->GetInstanceId(), bot->IsBeingTeleported());
        Finish(p, End::EnterFailed, now);
    }
}

// ---- Inside ---------------------------------------------------------------------------------------------------
void StepInside(Probe& p, std::uint64_t now)
{
    Entry const& e = gEntries.at(p.run.map);
    std::vector<Player*> const bots = Online(p);
    Player* leader = bots.empty() ? nullptr : Find(p.leader);
    InsideFacts f;
    f.size = std::uint32_t(p.members.size());
    f.online = bots.empty() ? 0 : f.size;
    f.nowMs = now;
    f.enteredMs = p.enteredMs;
    f.wipes = p.run.wipes;
    f.allMask = p.run.allMask;
    bool anyDead = false;
    for (std::size_t i = 0; i < bots.size(); ++i)
    {
        Player* bot = bots[i];
        bool const alive = bot->IsAlive();
        if (p.members[i].wasAlive && !alive)
            ++p.run.deaths[i];
        p.members[i].wasAlive = alive;
        f.alive += alive ? 1 : 0;
        anyDead = anyDead || !alive;
        f.anyInCombat = f.anyInCombat || (alive && bot->IsInCombat());
        if (Settled(bot))
            ApplyMode(p, p.members[i], bot);
    }
    bool bossKilled = false;
    if (leader && Settled(leader))
    {
        f.leaderOnDungeonMap = !leader->IsAlive() || leader->GetMapId() == e.map;
        InstanceMap* im = leader->GetMapId() == e.map ? leader->GetMap()->ToInstanceMap() : nullptr;
        InstanceScript* script = im ? im->GetInstanceScript() : nullptr;
        std::uint32_t const mask = script ? script->GetCompletedEncounterMask() : p.run.mask;
        for (std::uint32_t enc : AutoWowParty::NewBosses(p.run.mask, mask))
        {
            p.run.mask |= 1u << enc;
            bossKilled = true;
            Emit(p, "boss_killed", std::int32_t(enc));
        }
    }
    f.mask = p.run.mask;
    p.quietDeadSinceMs = anyDead && !f.anyInCombat ? (p.quietDeadSinceMs ? p.quietDeadSinceMs : now) : 0;
    f.quietDeadSinceMs = p.quietDeadSinceMs;

    StuckPoint sp;
    if (leader && Settled(leader) && leader->IsAlive() &&
        NoteProgress(p.stuck, Yd(leader->GetPositionX()), Yd(leader->GetPositionY()), Yd(leader->GetPositionZ()),
                     bossKilled || f.anyInCombat, now, gParams.stuckYards, gParams.stuckMs))
    {
        ++p.run.stucks;
        // Label by the encounter the leader's navigator actually targets; lowest undone index if it has none.
        std::int32_t const target = GetDungeonNavigatorTargetEncounter(p.leader, e.map, p.run.instance);
        sp = {leader->GetMapId(), Yd(leader->GetPositionX()), Yd(leader->GetPositionY()), Yd(leader->GetPositionZ()),
              target >= 0 ? target : NextEncounter(p.run.mask, p.run.allMask), 0};
        if (DungeonEncounterList const* list = sObjectMgr->GetDungeonEncounterList(e.map, DUNGEON_DIFFICULTY_NORMAL))
            for (DungeonEncounter const* enc : *list)
                if (std::int32_t(enc->dbcEntry->encounterIndex) == sp.next)
                    sp.boss = enc->creditEntry;
        Emit(p, "stuck", -1, &sp);
    }
    f.stucks = p.run.stucks;

    End end = End::None;
    switch (DecideInside(f, gParams.inside, end))
    {
        case Verdict::Continue:
            return;
        case Verdict::Finish:
            if (end == End::Abandoned)
                p.run.why = AbandonReason(f);
            Finish(p, end, now);
            return;
        case Verdict::Revive:
        {
            // Probe magic: the dead rise at the leader, else at the first living member (a released ghost outside
            // re-enters the instance through the leader's bind).
            Player* anchor = Settled(leader) && leader->IsAlive() && leader->GetMapId() == e.map ? leader : nullptr;
            for (std::size_t i = 0; !anchor && i < bots.size(); ++i)
                if (Settled(bots[i]) && bots[i]->IsAlive() && bots[i]->GetMapId() == e.map)
                    anchor = bots[i];
            std::uint32_t const before = p.run.revives;
            for (Player* bot : bots)
                if (!bot->IsAlive() && anchor && Settled(bot))
                {
                    Refresh(bot);
                    bot->TeleportTo(anchor->GetMapId(), anchor->GetPositionX(), anchor->GetPositionY(),
                                    anchor->GetPositionZ(), anchor->GetOrientation());
                    ++p.run.revives;
                }
            if (p.run.revives == before)
                return;  // nobody to anchor on yet: retry next tick
            p.quietDeadSinceMs = 0;
            Emit(p, "revived");
            return;
        }
        case Verdict::Wiped:
            // Probe magic: everyone rises at the instance entrance; the same instance continues (killed bosses stay
            // dead). Re-entry goes through Enter (leader first) for members released outside.
            ++p.run.wipes;
            Emit(p, "wiped");
            for (Player* bot : bots)
            {
                if (!Settled(bot))
                    continue;  // in transit: Enter revives it on landing
                Refresh(bot);
                if (bot->GetMapId() == e.map)
                    bot->TeleportTo(e.map, e.x, e.y, e.z, e.o);
            }
            for (Member& m : p.members)
                m.wasAlive = true;
            p.quietDeadSinceMs = 0;
            p.phase = Phase::Enter;
            p.phaseMs = now;
            return;
    }
}

// ---- Exit -----------------------------------------------------------------------------------------------------
void StepExit(Probe& p, std::uint64_t now)
{
    std::uint32_t const map = p.run.map;
    bool inside = false;
    for (Member const& m : p.members)
    {
        Player* bot = Find(m.guid);
        if (!bot)
            continue;
        if (!Settled(bot))
        {
            inside = true;  // in transit: wait for it to land
            continue;
        }
        ApplyMode(p, m, bot);  // a teleport ack rebuilds default strategies (New-RPG) again
        if (bot->GetMapId() == map)
        {
            Refresh(bot);
            bot->TeleportTo(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ,
                            bot->GetOrientation());
        }
        inside = inside || bot->GetMapId() == map;
    }
    if (inside && now < p.phaseMs + gParams.exitTimeoutMs)
        return;
    // Out: the party and its binds for this dungeon go, so the next visit gets a fresh instance.
    for (Member const& m : p.members)
        if (Player* bot = Find(m.guid))
        {
            if (Group* g = bot->GetGroup())
                DisbandOwn(p, g);
            Unbind(bot, map);
        }
    Advance(p, now);
}

// A guid the probe must never take: the cohort / fixture / oracle gate of the per-quest probe, a Guilds cohort
// range or configured rep, a supply role or a squad roster. nullptr = acceptable.
char const* Refusal(std::uint32_t guid, std::string const& fixtures, std::string const& oracle)
{
    if (char const* r = AutoWowProbePlace::GateGuid(true, fixtures, oracle, guid))
        return r;
    if (AutoWowGuilds::InRanges(AutoWowGuilds::Cohort(), guid))
        return "guilds_cohort";
    for (std::size_t h = 0; h < AutoWowGuilds::Houses().size(); ++h)
        if (AutoWowGuilds::ConfiguredRep(h, true) == guid || AutoWowGuilds::ConfiguredRep(h, false) == guid)
            return "guild_rep";
    if (AutoWowSupply::RoleOf(guid).role != AutoWowSupply::Role::None)
        return "supply_role";
    if (AutoWowSquad::IsMember(guid))
        return "squad_member";
    return nullptr;
}
}  // namespace

void LoadConfig()
{
    detail::gEnabled = false;
    detail::gProbeGuids.clear();
    gProbes.clear();
    std::string const text = sConfigMgr->GetOption<std::string>("AutoWow.DungeonProbe.Parties", "");
    if (text.empty())
        return;
    std::string const fixtures = sConfigMgr->GetOption<std::string>(AutoWowFixture::kFixtureGuidsConfigKey, "");
    std::string const oracle = sConfigMgr->GetOption<std::string>(AutoWowProbePlace::kOracleGuidsConfigKey, "");
    for (PartyDef const& def : ParseParties(text))
    {
        char const* refusal = nullptr;
        std::uint32_t bad = 0;
        for (std::uint32_t const g : def.guids)
            if (!refusal && (refusal = Refusal(g, fixtures, oracle)))
                bad = g;
        if (refusal)
        {
            LOG_ERROR("server.loading", "[DProbe] party {} refused: guid {} {}", def.name, bad, refusal);
            continue;
        }
        Probe p;
        p.name = def.name;
        if (CharacterCacheEntry const* c =
                sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(def.guids.front())))
            p.team = Player::TeamIdForRace(c->Race);
        for (std::uint32_t const g : def.guids)
        {
            p.members.push_back({g});
            detail::gProbeGuids.push_back(g);
        }
        gProbes.push_back(std::move(p));
    }
    std::sort(detail::gProbeGuids.begin(), detail::gProbeGuids.end());

    Params& p = gParams;
    p.tickMs = std::max<std::uint32_t>(sConfigMgr->GetOption<std::uint32_t>("AutoWow.DungeonProbe.TickMs", 1000), 100);
    p.queue = ParseQueue(sConfigMgr->GetOption<std::string>("AutoWow.DungeonProbe.Dungeons",
                                                            "36,43,389,47,48,90,129,70,109,209,230,329,349,429"));
    p.levels = ParseLevels(sConfigMgr->GetOption<std::string>("AutoWow.DungeonProbe.Levels", ""));
    p.levelOver = sConfigMgr->GetOption<std::uint32_t>("AutoWow.DungeonProbe.LevelOver", 2);
    p.quality = sConfigMgr->GetOption<std::uint32_t>("AutoWow.DungeonProbe.Quality", 4);
    p.inside.maxWipes = sConfigMgr->GetOption<std::uint32_t>("AutoWow.DungeonProbe.MaxWipes", 3);
    p.inside.stuckLimit = sConfigMgr->GetOption<std::uint32_t>("AutoWow.DungeonProbe.StuckLimit", 2);
    p.inside.runTimeoutMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.DungeonProbe.RunTimeoutMs", 3600000);
    p.inside.reviveGraceMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.DungeonProbe.ReviveGraceMs", 30000);
    p.stuckMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.DungeonProbe.StuckMs", 300000);
    p.stuckYards = sConfigMgr->GetOption<std::uint32_t>("AutoWow.DungeonProbe.StuckYards", 10);
    p.prepareTimeoutMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.DungeonProbe.PrepareTimeoutMs", 600000);
    p.enterTimeoutMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.DungeonProbe.EnterTimeoutMs", 180000);
    p.exitTimeoutMs = sConfigMgr->GetOption<std::uint32_t>("AutoWow.DungeonProbe.ExitTimeoutMs", 120000);
    p.loops = sConfigMgr->GetOption<std::uint32_t>("AutoWow.DungeonProbe.Loops", 1);

    // Entry: the lowest continent-side trigger into each queued dungeon, landing at its instance-side target.
    gEntries.clear();
    for (auto const& [triggerId, tp] : sObjectMgr->GetAllAreaTriggerTeleports())
    {
        AreaTrigger const* at = sObjectMgr->GetAreaTrigger(triggerId);
        MapEntry const* from = at ? sMapStore.LookupEntry(at->map) : nullptr;
        if (!from || !from->IsContinent() ||
            std::find(p.queue.begin(), p.queue.end(), tp.target_mapId) == p.queue.end())
            continue;
        auto const it = gEntries.find(tp.target_mapId);
        if (it == gEntries.end() || triggerId < it->second.trigger)
            gEntries[tp.target_mapId] = {triggerId, tp.target_mapId, tp.target_X, tp.target_Y, tp.target_Z,
                                         tp.target_Orientation};
    }
    detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.DungeonProbe.Enable", false) && !gProbes.empty() &&
                       !p.queue.empty();
    LOG_INFO("server.loading", ">> AutoWow.DungeonProbe: {} parties ({} guids excluded from cohort features), "
             "{} dungeons queued, {} entrances, enabled={}", gProbes.size(), detail::gProbeGuids.size(),
             p.queue.size(), gEntries.size(), detail::gEnabled);
}

void WorldUpdate(std::uint32_t diff)
{
    gSince += diff;
    if (gSince < gParams.tickMs)
        return;
    gSince = 0;
    std::uint64_t const now = NowMs();
    for (Probe& p : gProbes)
    {
        if (!p.phaseMs)
            p.phaseMs = now;
        switch (p.phase)
        {
            case Phase::Prepare: StepPrepare(p, now); break;
            case Phase::Enter: StepEnter(p, now); break;
            case Phase::Inside: StepInside(p, now); break;
            case Phase::Exit: StepExit(p, now); break;
            case Phase::Done: break;
        }
    }
}
}  // namespace AutoWowDungeonProbe
