/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <filesystem>
#include <fstream>
#include <iterator>

#include "DungeonGatePolicy.h"
#include "PartyPolicy.h"
#include "ZoneProgressionPolicy.h"
#include "gtest/gtest.h"

namespace
{
using namespace AutoWowParty;

std::vector<DungeonDef> Defs();
FormParams DungeonParams();

Candidate C(std::uint32_t guid, std::uint32_t level, std::uint32_t cls, std::uint32_t zone = 17,
            std::uint8_t team = kHorde, std::uint32_t zoneMinLevel = 0)
{
    Candidate c;
    c.guid = guid;
    c.level = level;
    c.cls = cls;
    c.team = team;
    c.map = 1;
    c.zone = zone;
    c.zoneMinLevel = zoneMinLevel;
    return c;
}

TEST(PartyPolicyTest, FormationRejectsMemberBelowAnotherCandidatesZoneFloor)
{
    FormParams fp;
    fp.levelSpread = 10;
    fp.hubYards = 4000;
    std::vector<Candidate> c = {
        C(62959, 59, kWarlock, 3483, kAlliance, 58),
        C(62962, 64, kRogue, 3519, kAlliance, 62),
    };
    c[0].groupQuest = true;

    EXPECT_TRUE(FormParties(c, fp, {}).empty());

    c[0].level = 62;
    ASSERT_EQ(FormParties(c, fp, {}).size(), 1u);

    c[0].zoneMinLevel = 0;  // two unknown zone brackets remain admissible
    c[1].zoneMinLevel = 0;
    c[0].level = 59;
    ASSERT_EQ(FormParties(c, fp, {}).size(), 1u);
}

TEST(PartyPolicyTest, DungeonFormationIgnoresOpenWorldZoneFloors)
{
    std::vector<Candidate> c = {
        C(1, 14, kWarrior, 17, kHorde, 40),
        C(2, 14, kPriest, 17, kHorde, 40),
        C(3, 14, kMage, 17, kHorde, 40),
    };
    ASSERT_EQ(FormParties(c, DungeonParams(), Defs()).size(), 1u);
    EXPECT_EQ(FormParties(c, DungeonParams(), Defs())[0].reason, Reason::Dungeon);
}

TEST(PartyPolicyTest, SurvivalEscapeRetiresOpenWorldGroupQuestAndPreEntryRecruitedDungeonParties)
{
    EXPECT_TRUE(ShouldLeaveForSurvival(Reason::GroupQuest, false, Phase::None, false));
    EXPECT_FALSE(ShouldLeaveForSurvival(Reason::GroupQuest, false, Phase::Approach, false));

    EXPECT_TRUE(ShouldLeaveForSurvival(Reason::Dungeon, true, Phase::None, false));
    EXPECT_TRUE(ShouldLeaveForSurvival(Reason::Dungeon, true, Phase::Approach, false));
    EXPECT_TRUE(ShouldLeaveForSurvival(Reason::Dungeon, true, Phase::Stage, false));
    EXPECT_FALSE(ShouldLeaveForSurvival(Reason::Dungeon, true, Phase::Inside, false));
    EXPECT_FALSE(ShouldLeaveForSurvival(Reason::Dungeon, true, Phase::Exit, false));

    // A native/proximity dungeon formation was not recruited into the owned run.
    EXPECT_FALSE(ShouldLeaveForSurvival(Reason::Dungeon, false, Phase::None, false));
    // Instance admission is durable evidence even if a caller presents a pre-entry phase.
    EXPECT_FALSE(ShouldLeaveForSurvival(Reason::Dungeon, true, Phase::None, true));
    EXPECT_FALSE(ShouldLeaveForSurvival(Reason::Squad, true, Phase::None, false));
}

TEST(PartyPolicyTest, SurvivalEscapeConsumptionRequiresLiveOwnedAutonomousOpenWorldRoster)
{
    SurvivalLeaveLiveFacts live{true, true, true};
    EXPECT_TRUE(ShouldConsumeSurvivalLeave(Reason::Dungeon, true, Phase::Stage, false, live));

    live.allOpenWorld = false;  // Stored Stage/instance=0 may lag physical entry.
    EXPECT_FALSE(ShouldConsumeSurvivalLeave(Reason::Dungeon, true, Phase::Stage, false, live));

    live = {false, true, true};  // Missing or changed native group ownership fails closed.
    EXPECT_FALSE(ShouldConsumeSurvivalLeave(Reason::Dungeon, true, Phase::Stage, false, live));

    live = {true, false, true};  // A user, Oracle, gather worker, or otherwise non-autonomous member owns it.
    EXPECT_FALSE(ShouldConsumeSurvivalLeave(Reason::Dungeon, true, Phase::Stage, false, live));
}

std::vector<DungeonDef> Defs() { return ParseDungeons("389:13:18:H,36:17:26:A,43:17:24:AH"); }

FormParams DungeonParams()
{
    FormParams p;
    p.dungeons = true;
    return p;
}

TEST(PartyPolicyTest, RolesPreferClassOrderThenLevelThenGuid)
{
    std::vector<Member> m = {{10, 14, kMage}, {11, 13, kPaladin}, {12, 15, kWarrior}, {13, 14, kShaman},
                             {14, 15, kPriest}};
    std::vector<Role> r = AssignRoles(m);
    EXPECT_EQ(r, (std::vector<Role>{Role::Dps, Role::Dps, Role::Tank, Role::Dps, Role::Healer}));
    EXPECT_EQ(m[ChooseLeader(m, r)].guid, 12u);  // the tank leads

    // A hybrid fills one role only: the paladin (tank rank 2) tanks, the druid heals.
    m = {{20, 14, kDruid}, {21, 14, kPaladin}};
    r = AssignRoles(m);
    EXPECT_EQ(r, (std::vector<Role>{Role::Healer, Role::Tank}));
    m = {{20, 14, kDruid}, {21, 14, kRogue}};
    r = AssignRoles(m);
    EXPECT_EQ(r, (std::vector<Role>{Role::Tank, Role::Dps}));

    // Equal class: higher level, then lower guid.
    m = {{31, 14, kWarrior}, {30, 14, kWarrior}, {32, 15, kWarrior}};
    r = AssignRoles(m);
    EXPECT_EQ(r[2], Role::Tank);
    m = {{31, 14, kWarrior}, {30, 14, kWarrior}};
    EXPECT_EQ(AssignRoles(m)[1], Role::Tank);

    // No tank: the highest level leads, lower guid on ties.
    m = {{40, 14, kMage}, {41, 15, kRogue}, {42, 15, kHunter}};
    r = AssignRoles(m);
    EXPECT_EQ(m[ChooseLeader(m, r)].guid, 41u);
}

TEST(PartyPolicyTest, DungeonListParsesAndSelectsByTeamAndBracket)
{
    std::vector<DungeonDef> d = ParseDungeons(" 389:13:18:H, bad, 36:17:26:A,43:17:24:AH,5:9:1:A,7:1:2:");
    ASSERT_EQ(d.size(), 3u);
    EXPECT_EQ(d[0].map, 389u);
    EXPECT_EQ(d[2].teams, kAlliance | kHorde);
    EXPECT_EQ(SelectDungeon(d, kHorde, 13, 16)->map, 389u);
    EXPECT_EQ(SelectDungeon(d, kAlliance, 13, 16), nullptr);  // Deadmines starts at 17
    EXPECT_EQ(SelectDungeon(d, kAlliance, 17, 19)->map, 36u);
    EXPECT_EQ(SelectDungeon(d, kHorde, 17, 19)->map, 43u);    // RFC tops at 18: list order, bracket must hold
    EXPECT_EQ(SelectDungeon(d, kHorde, 12, 14), nullptr);
}

TEST(PartyPolicyTest, QuestCapabilityInParty)
{
    EXPECT_TRUE(IsGroupQuest(kQuestTypeGroup, 0));
    EXPECT_TRUE(IsGroupQuest(kQuestTypeNormal, 3));
    EXPECT_FALSE(IsGroupQuest(kQuestTypeNormal, 0));
    EXPECT_FALSE(IsGroupQuest(kQuestTypeDungeon, 5));
    EXPECT_TRUE(QuestCapableInParty(kQuestTypeGroup, 3, 3, false));
    EXPECT_FALSE(QuestCapableInParty(kQuestTypeGroup, 5, 3, false));
    EXPECT_TRUE(QuestCapableInParty(kQuestTypeNormal, 0, 2, false));
    EXPECT_TRUE(QuestCapableInParty(kQuestTypeDungeon, 5, 3, true));
    EXPECT_FALSE(QuestCapableInParty(kQuestTypeDungeon, 5, 5, false));
    EXPECT_FALSE(QuestCapableInParty(62, 10, 5, true));  // raid
}

TEST(PartyPolicyTest, FormsDungeonPartyWithTankAndHealer)
{
    std::vector<Candidate> c = {C(5, 14, kMage), C(3, 13, kWarrior), C(9, 15, kPriest), C(7, 14, kRogue),
                                C(8, 14, kHunter), C(4, 14, kWarlock)};
    std::vector<Plan> plans = FormParties(c, DungeonParams(), Defs());
    ASSERT_EQ(plans.size(), 1u);
    Plan const& p = plans[0];
    EXPECT_EQ(p.reason, Reason::Dungeon);
    EXPECT_EQ(p.dungeonMap, 389u);
    ASSERT_EQ(p.guids.size(), 5u);
    EXPECT_TRUE(std::is_sorted(p.guids.begin(), p.guids.end()));
    EXPECT_EQ(p.leader, 3u);  // warrior tank
    EXPECT_NE(std::find(p.guids.begin(), p.guids.end(), 9u), p.guids.end());  // the only healer is taken
    EXPECT_EQ(std::count(p.roles.begin(), p.roles.end(), Role::Tank), 1);
    EXPECT_EQ(std::count(p.roles.begin(), p.roles.end(), Role::Healer), 1);
    // Deterministic: input order does not matter.
    std::reverse(c.begin(), c.end());
    std::vector<Plan> again = FormParties(c, DungeonParams(), Defs());
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(again[0].guids, p.guids);
    EXPECT_EQ(again[0].roles, p.roles);
}

TEST(PartyPolicyTest, FormationRespectsFactionLevelSpreadAndPlace)
{
    FormParams fp = DungeonParams();
    // Level spread: 13 and 17 never share a party (spread 3).
    std::vector<Candidate> c = {C(1, 13, kWarrior), C(2, 13, kPriest), C(3, 17, kMage)};
    std::vector<Plan> plans = FormParties(c, fp, Defs());
    ASSERT_EQ(plans.size(), 0u);  // two bots < dungeon min size 3, no group quest
    c.push_back(C(4, 14, kRogue));
    plans = FormParties(c, fp, Defs());
    ASSERT_EQ(plans.size(), 1u);
    EXPECT_EQ(plans[0].guids, (std::vector<std::uint32_t>{1, 2, 4}));

    // Other faction / other zone far away never join.
    c = {C(1, 14, kWarrior), C(2, 14, kPriest), C(3, 14, kMage, 17, kAlliance), C(4, 14, kRogue, 14)};
    c[3].x = 5000;
    plans = FormParties(c, fp, Defs());
    ASSERT_TRUE(plans.empty());
    // Same hub (within HubYards) across a zone border joins.
    c[3].x = 100;
    plans = FormParties(c, fp, Defs());
    ASSERT_EQ(plans.size(), 1u);
    EXPECT_EQ(plans[0].guids, (std::vector<std::uint32_t>{1, 2, 4}));
}

TEST(PartyPolicyTest, DungeonNeedsTankAndHealerElseGroupQuest)
{
    FormParams fp = DungeonParams();
    std::vector<Candidate> c = {C(1, 14, kMage), C(2, 14, kRogue), C(3, 14, kHunter)};
    EXPECT_TRUE(FormParties(c, fp, Defs()).empty());
    c[1].groupQuest = true;
    std::vector<Plan> plans = FormParties(c, fp, Defs());
    ASSERT_EQ(plans.size(), 1u);
    EXPECT_EQ(plans[0].reason, Reason::GroupQuest);
    EXPECT_EQ(plans[0].dungeonMap, 0u);
    EXPECT_EQ(plans[0].leader, 1u);  // no tank: highest level, lowest guid
    fp.requireTankHealer = false;
    plans = FormParties(c, fp, Defs());
    ASSERT_EQ(plans.size(), 1u);
    EXPECT_EQ(plans[0].reason, Reason::Dungeon);
    // Dungeons off: only group-quest parties.
    fp = FormParams{};
    c = {C(1, 14, kWarrior), C(2, 14, kPriest), C(3, 14, kMage)};
    EXPECT_TRUE(FormParties(c, fp, Defs()).empty());
}

TEST(PartyPolicyTest, MaxSizeSplitsIntoSeveralParties)
{
    std::vector<Candidate> c;
    for (std::uint32_t g = 1; g <= 10; ++g)
        c.push_back(C(g, 14, g % 5 == 1 ? kWarrior : g % 5 == 2 ? kPriest : kMage));
    std::vector<Plan> plans = FormParties(c, DungeonParams(), Defs());
    ASSERT_EQ(plans.size(), 2u);
    EXPECT_EQ(plans[0].guids.size(), 5u);
    EXPECT_EQ(plans[1].guids.size(), 5u);
    for (Plan const& p : plans)
    {
        EXPECT_EQ(std::count(p.roles.begin(), p.roles.end(), Role::Tank), 1);
        EXPECT_EQ(std::count(p.roles.begin(), p.roles.end(), Role::Healer), 1);
    }
}

TEST(PartyPolicyTest, DisbandRules)
{
    KeepParams kp;
    KeepFacts f;
    f.online = f.size = 3;
    f.minLevel = 14;
    f.maxLevel = 15;
    EXPECT_EQ(ShouldDisband(f, kp), Disband::None);
    f.unsafeZone = true;
    EXPECT_EQ(ShouldDisband(f, kp), Disband::UnsafeZone);
    f.unsafeZone = false;
    f.online = 2;
    EXPECT_EQ(ShouldDisband(f, kp), Disband::MemberOffline);
    f.online = 3;
    f.maxLevel = 18;
    EXPECT_EQ(ShouldDisband(f, kp), Disband::LevelDrift);
    f.inDungeonRun = true;
    EXPECT_EQ(ShouldDisband(f, kp), Disband::None);  // the run decides
    f.inDungeonRun = false;
    f.maxLevel = 15;
    f.separatedMs = kp.separatedMs;
    EXPECT_EQ(ShouldDisband(f, kp), Disband::Separated);
    f.separatedMs = 0;
    f.purposelessMs = kp.purposelessMs;
    EXPECT_EQ(ShouldDisband(f, kp), Disband::NoPurpose);
    f.purposelessMs = 0;
    f.ageMs = kp.maxAgeMs;
    EXPECT_EQ(ShouldDisband(f, kp), Disband::MaxAge);
    f.online = f.size = 1;
    EXPECT_EQ(ShouldDisband(f, kp), Disband::TooSmall);
}

TEST(PartyPolicyTest, TauntPriority)
{
    std::vector<Threat> t = {{100, Role::Dps, 50, 10}, {101, Role::Healer, 90, 20}, {102, Role::Tank, 10, 5},
                             {103, Role::Dps, 30, 40}};
    EXPECT_EQ(PickTauntTarget(t, 30), 101u);   // the healer's attacker first
    t[1].yards = 35;
    EXPECT_EQ(PickTauntTarget(t, 30), 100u);   // healer's mob out of reach; 103 out of reach
    EXPECT_EQ(PickTauntTarget(t, 50), 101u);
    t = {{200, Role::Dps, 40, 10}, {201, Role::Dps, 40, 8}, {199, Role::Dps, 40, 8}};
    EXPECT_EQ(PickTauntTarget(t, 30), 199u);   // equal health: nearer, then lower guid
    t = {{300, Role::Tank, 5, 1}};
    EXPECT_EQ(PickTauntTarget(t, 30), 0u);     // mobs on the tank need no taunt
    EXPECT_FALSE(TauntSpells(kWarrior).empty());
    EXPECT_TRUE(TauntSpells(kMage).empty());
}

TEST(PartyPolicyTest, HealPriority)
{
    std::vector<Wounded> w = {{1, Role::Dps, 55, true}, {2, Role::Tank, 55, true}, {3, Role::Healer, 70, true},
                              {4, Role::Dps, 10, false}};
    EXPECT_EQ(PickHealTarget(w, 60), 2u);  // equal health: tank first; the dead are skipped
    w[0].hpPct = 40;
    EXPECT_EQ(PickHealTarget(w, 60), 1u);  // lowest health wins
    EXPECT_EQ(PickHealTarget(w, 30), 0u);  // nobody below the threshold: the healer may deal damage
    EXPECT_FALSE(HealSpells(kPriest).empty());
    EXPECT_TRUE(HealSpells(kRogue).empty());
}

TEST(PartyPolicyTest, RoleStrategiesAndUndo)
{
    RoleStrategies r = StrategiesFor(kWarrior, Role::Tank);
    EXPECT_EQ(r.combat, "+tank,+tank assist,+tank face,-dps assist");
    EXPECT_EQ(r.nonCombat, "+tank assist,-dps assist");
    EXPECT_EQ(StrategiesFor(kDruid, Role::Tank).combat.substr(0, 5), "+bear");
    EXPECT_EQ(StrategiesFor(kShaman, Role::Healer).combat, "+resto,+healer dps");
    EXPECT_TRUE(StrategiesFor(kMage, Role::Tank).combat.empty());
    EXPECT_TRUE(StrategiesFor(kPriest, Role::Dps).combat.empty());

    EXPECT_EQ(StrategyUndo({"arms", "dps assist", "aoe"}, {"tank", "tank assist", "tank face", "aoe"}),
              "-tank,-tank assist,-tank face,+arms,+dps assist");
    EXPECT_EQ(StrategyUndo({"a", "b"}, {"b", "a"}), "");
}

TEST(PartyPolicyTest, EncounterMaskDiff)
{
    EXPECT_EQ(NewBosses(0b0001, 0b1011), (std::vector<std::uint32_t>{1, 3}));
    EXPECT_TRUE(NewBosses(0b11, 0b11).empty());
    EXPECT_TRUE(AllEncountersDone(0b1111, 0b1111));
    EXPECT_FALSE(AllEncountersDone(0b0111, 0b1111));
    EXPECT_FALSE(AllEncountersDone(0, 0));  // no encounter data never counts as done
}

TEST(PartyPolicyTest, RampartsRecruitRequiresFinalNativeBossState)
{
    // Static-spawn discovery yields Gargolmar and Omor (0x3). The explicit precursor gate admits
    // encounter 2, while unrelated maps and older gate kinds receive no dynamic authorization.
    EXPECT_EQ(DungeonGate::AuthorizeGateBackedEncounters(543, 0x3u), 0x7u);
    EXPECT_EQ(DungeonGate::AuthorizeGateBackedEncounters(542, 0x3u), 0x3u);
    EXPECT_EQ(DungeonGate::AuthorizeGateBackedEncounters(109, 0x3u), 0x3u);

    // Vazruden kill credit may set bit 2 before Nazan dies. The recruited run only accepts that bit
    // after the passive Herald's native boss state reaches DONE.
    EXPECT_FALSE(DungeonGate::CompletionConfirmed(543, 2, 0x7u, false));
    EXPECT_TRUE(DungeonGate::CompletionConfirmed(543, 2, 0x7u, true));
    EXPECT_TRUE(DungeonGate::CompletionConfirmed(543, 0, 0x1u, false));
}

TEST(PartyPolicyTest, LedgerFieldsAreStable)
{
    EXPECT_EQ(PartyFields(7, {62955, 62960}, {Role::Tank, Role::Healer}, 62955, Reason::Dungeon, 389, 0),
              ",\"pid\":7,\"members\":[62955,62960],\"roles\":[\"tank\",\"healer\"],\"leader\":62955,"
              "\"why\":\"dungeon\",\"dmap\":389,\"age_ms\":0");
    EXPECT_EQ(DungeonFields(7, 389, 12, 2, 5, 15, 60000, {62955}),
              ",\"pid\":7,\"dmap\":389,\"inst\":12,\"enc\":2,\"mask\":5,\"all\":15,\"dur_ms\":60000,"
              "\"members\":[62955]");
    EXPECT_STREQ(DisbandName(Disband::DungeonDone), "dungeon_done");
    EXPECT_STREQ(RunEventName(RunEvent::BossKilled), "boss_killed");
    EXPECT_STREQ(RunEventName(RunEvent::Summary), "summary");
    EXPECT_EQ(RunSummaryFields(2, 3, false, true, 4),
              ",\"bosses\":2,\"total\":3,\"completed\":0,\"wipes\":1,\"deaths\":4,\"recruit\":1");
}

// ---- recruitment (AutoWow.Dungeon.Recruit) ----
// A candidate anywhere in the world (map 1 = Kalimdor, x/y integer yards).
Candidate R(std::uint32_t guid, std::uint32_t level, std::uint32_t cls, std::int32_t x = 0, std::uint32_t map = 1,
            std::uint8_t team = kHorde)
{
    Candidate c = C(guid, level, cls, 0, team);
    c.map = map;
    c.x = x;
    return c;
}

std::vector<EntranceSpot> Known(std::size_t n) { return std::vector<EntranceSpot>(n, EntranceSpot{1, 0, 0, true}); }

TEST(PartyPolicyTest, RecruitPicksTheDungeonWithMostEligibleBots)
{
    // WC 19..26 holds 3 horde bots, RFK 30..40 holds 5; alliance bots never count for the horde.
    std::vector<DungeonDef> const defs = ParseDungeons("43:19:26:AH,47:30:40:AH");
    std::vector<Candidate> const cands = {
        R(1, 20, kPriest), R(2, 21, kRogue), R(3, 22, kMage),
        R(10, 31, kShaman), R(11, 32, kRogue), R(12, 33, kMage), R(13, 31, kWarlock), R(14, 32, kHunter),
        R(20, 20, kPriest, 0, 1, kAlliance), R(21, 20, kRogue, 0, 1, kAlliance), R(22, 20, kMage, 0, 1, kAlliance)};
    RecruitParams p;
    Plan const horde = PlanRecruit(cands, kHorde, defs, Known(2), p);
    EXPECT_EQ(horde.reason, Reason::Dungeon);
    EXPECT_EQ(horde.dungeonMap, 47u);
    EXPECT_EQ(horde.guids, (std::vector<std::uint32_t>{10, 11, 12, 13, 14}));
    // Three alliance bots are below MinSize 4.
    EXPECT_TRUE(PlanRecruit(cands, kAlliance, defs, Known(2), p).guids.empty());

    // An unknown entrance takes the dungeon out: WC is left, with 3 bots: none.
    std::vector<EntranceSpot> spots = Known(2);
    spots[1].known = false;
    EXPECT_TRUE(PlanRecruit(cands, kHorde, defs, spots, p).guids.empty());
    // MinSize 3 then takes WC.
    p.minSize = 3;
    EXPECT_EQ(PlanRecruit(cands, kHorde, defs, spots, p).dungeonMap, 43u);
}

TEST(PartyPolicyTest, RecruitNeedsAHealerCapableClass)
{
    std::vector<DungeonDef> const defs = ParseDungeons("47:30:40:AH");
    std::vector<Candidate> cands = {R(1, 31, kWarrior), R(2, 31, kRogue), R(3, 31, kMage), R(4, 31, kWarlock),
                                    R(5, 31, kHunter)};
    EXPECT_TRUE(PlanRecruit(cands, kHorde, defs, Known(1), RecruitParams{}).guids.empty());
    cands.push_back(R(6, 32, kDruid));  // a lone druid heals; the warrior still tanks
    Plan const pl = PlanRecruit(cands, kHorde, defs, Known(1), RecruitParams{});
    ASSERT_EQ(pl.guids.size(), 5u);
    EXPECT_EQ(pl.guids.back(), 6u);
    EXPECT_EQ(pl.roles.back(), Role::Healer);
    EXPECT_EQ(pl.roles.front(), Role::Tank);
    EXPECT_EQ(pl.leader, 1u);  // the tank leads
}

TEST(PartyPolicyTest, RecruitRosterPrefersHealerTankThenWalkRangeLevelGuid)
{
    std::vector<DungeonDef> const defs = ParseDungeons("209:44:54:AH");
    std::vector<EntranceSpot> const spots = {EntranceSpot{1, 0, 0, true}};
    RecruitParams p;
    p.walkYards = 1000;
    std::vector<Candidate> const cands = {
        R(1, 48, kPriest, 5000),  // a priest out of walk range, higher level
        R(2, 46, kPriest, 100),   // a priest in walk range: heals
        R(3, 47, kDruid, 100),    // tank rank 3
        R(4, 45, kWarrior, 9000), // tank rank 1 wins over walk range
        R(5, 48, kRogue, 9000),   // dps: walk range first, then level, then guid
        R(6, 45, kMage, 200),
        R(7, 45, kWarlock, 300, 0),  // another continent: out of walk range
        R(8, 44, kHunter, 50)};
    Plan const pl = PlanRecruit(cands, kHorde, defs, spots, p);
    // Healer 2, tank 4, then in-range dps by level: 3 (47), 6 (45), 8 (44).
    EXPECT_EQ(pl.guids, (std::vector<std::uint32_t>{2, 3, 4, 6, 8}));
    EXPECT_EQ(pl.roles, (std::vector<Role>{Role::Healer, Role::Dps, Role::Tank, Role::Dps, Role::Dps}));
    EXPECT_EQ(pl.leader, 4u);
    EXPECT_EQ(pl.dungeonMap, 209u);
}

TEST(PartyPolicyTest, RecruitWindowKeepsLevelSpreadInsideTheBand)
{
    // Band 30..40, spread 4: [30..34] holds 3, [35..39] holds 4 -> the higher window; ties keep list order.
    std::vector<DungeonDef> const defs = ParseDungeons("47:30:40:AH,129:37:46:AH");
    std::vector<Candidate> const cands = {R(1, 30, kPriest), R(2, 31, kRogue), R(3, 32, kMage),
                                          R(4, 36, kShaman), R(5, 37, kRogue), R(6, 38, kMage), R(7, 39, kHunter)};
    Plan const pl = PlanRecruit(cands, kHorde, defs, Known(2), RecruitParams{});
    EXPECT_EQ(pl.dungeonMap, 47u);  // RFD's [37..41] holds 3 (4 is 36): RFK's window wins
    EXPECT_EQ(pl.guids, (std::vector<std::uint32_t>{4, 5, 6, 7}));
    // Same count in both dungeons: the first listed.
    std::vector<Candidate> const tie = {R(1, 38, kPriest), R(2, 38, kRogue), R(3, 39, kMage), R(4, 39, kHunter)};
    EXPECT_EQ(PlanRecruit(tie, kHorde, defs, Known(2), RecruitParams{}).dungeonMap, 47u);
    char const* why = nullptr;
    PlanRecruit({R(1, 38, kPriest)}, kHorde, defs, Known(2), RecruitParams{}, &why);
    EXPECT_STREQ(why, "no_window");
    // Deterministic: input order does not matter.
    std::vector<Candidate> rev(cands.rbegin(), cands.rend());
    EXPECT_EQ(PlanRecruit(rev, kHorde, defs, Known(2), RecruitParams{}).guids, pl.guids);
}

Candidate T(Candidate c)  // tank-capable (the runtime's warrior / paladin / bear-form druid)
{
    c.canTank = true;
    return c;
}

TEST(PartyPolicyTest, RecruitWidePoolTakesOnlyPullableBusyBots)
{
    // Without the wide pool only free bots; with it also questing / grinding / errand / zone trip / party.
    for (std::size_t i = 0; i < std::size_t(RecruitOut::Count); ++i)
    {
        RecruitOut const o = RecruitOut(i);
        EXPECT_EQ(RecruitTakes(o, false), o == RecruitOut::Free) << RecruitOutName(o);
    }
    for (RecruitOut o : {RecruitOut::Free, RecruitOut::Party, RecruitOut::Combat, RecruitOut::ZoneMove,
                         RecruitOut::Errand})
        EXPECT_TRUE(RecruitTakes(o, true)) << RecruitOutName(o);
    for (RecruitOut o : {RecruitOut::Escort, RecruitOut::Instance, RecruitOut::Flight, RecruitOut::Dead,
                         RecruitOut::Supply, RecruitOut::Squad, RecruitOut::Run, RecruitOut::Cooldown,
                         RecruitOut::Paused, RecruitOut::Group})
        EXPECT_FALSE(RecruitTakes(o, true)) << RecruitOutName(o);
    EXPECT_FALSE(RecruitTakes(RecruitOut::Count, true));

    std::uint32_t counts[std::size_t(RecruitOut::Count)] = {};
    EXPECT_EQ(RecruitOutFields(counts), " out={}");
    counts[std::size_t(RecruitOut::Free)] = 9;
    counts[std::size_t(RecruitOut::Combat)] = 3;
    counts[std::size_t(RecruitOut::ZoneMove)] = 1;
    counts[std::size_t(RecruitOut::Supply)] = 1;
    EXPECT_EQ(RecruitOutFields(counts), " out={free=9,supply=1,combat=3,zone_move=1}");
    EXPECT_STREQ(RecruitOutName(RecruitOut::Errand), "errand");
    EXPECT_STREQ(DisbandName(Disband::Recruited), "recruited");
}

TEST(PartyPolicyTest, RecruitTankOverLevelFillsAMissingTankAfterInBandTanks)
{
    std::vector<DungeonDef> const defs = ParseDungeons("129:37:46:AH");  // RFD, max 46
    RecruitParams req;
    req.requireTank = true;
    char const* why = nullptr;
    std::vector<Candidate> cands = {R(1, 44, kPriest), R(2, 45, kRogue), R(3, 45, kMage), R(4, 46, kHunter),
                                    T(R(5, 64, kWarrior)), T(R(6, 66, kWarrior)), T(R(7, 30, kWarrior))};
    // Off (0): the L64 / L66 warriors are over the band: no tank.
    EXPECT_TRUE(PlanRecruit(cands, kHorde, defs, Known(1), req, &why).guids.empty());
    EXPECT_STREQ(why, "no_tank");
    // 17 over max 46 = 63: still none.
    req.tankOverLevel = 17;
    EXPECT_TRUE(PlanRecruit(cands, kHorde, defs, Known(1), req).guids.empty());
    // 20: the closest over-level warrior (64) tanks and leads; the window still fills the other four slots.
    req.tankOverLevel = 20;
    Plan pl = PlanRecruit(cands, kHorde, defs, Known(1), req, &why);
    EXPECT_STREQ(why, "");
    EXPECT_EQ(pl.guids, (std::vector<std::uint32_t>{1, 2, 3, 4, 5}));
    EXPECT_EQ(pl.roles, (std::vector<Role>{Role::Healer, Role::Dps, Role::Dps, Role::Dps, Role::Tank}));
    EXPECT_EQ(pl.leader, 5u);
    EXPECT_EQ(pl.tankDelta, 18);

    // An in-band tank goes first: a L45 paladin tanks (delta -1), the over-level warriors stay out.
    cands.push_back(T(R(8, 45, kPaladin)));
    pl = PlanRecruit(cands, kHorde, defs, Known(1), req);
    EXPECT_EQ(pl.leader, 8u);
    EXPECT_EQ(pl.tankDelta, -1);
    EXPECT_EQ(std::count(pl.guids.begin(), pl.guids.end(), 5u), 0);

    // The window's only healer is a paladin: it heals, the over-level warrior tanks.
    std::vector<Candidate> pal = {T(R(1, 44, kPaladin)), R(2, 45, kRogue), R(3, 45, kMage), R(4, 46, kHunter),
                                  T(R(5, 60, kWarrior))};
    pl = PlanRecruit(pal, kHorde, defs, Known(1), req);
    EXPECT_EQ(pl.roles, (std::vector<Role>{Role::Healer, Role::Dps, Role::Dps, Role::Dps, Role::Tank}));
    EXPECT_EQ(pl.tankDelta, 14);
}

TEST(PartyPolicyTest, RecruitGatherPortalsLateMembersAndDropsThoseWhoCannotCome)
{
    GatherFacts f;  // online, alive, outside, not arrived, not portaled
    EXPECT_EQ(DecideGather(f, false), GatherAct::None);    // within GatherPortalMs: wait
    EXPECT_EQ(DecideGather(f, true), GatherAct::Portal);   // S75: stuck at a flight path -> portal once
    f.portaled = true;
    EXPECT_EQ(DecideGather(f, true), GatherAct::None);     // tried: the missing-member grace decides now
    f = GatherFacts{};
    f.arrived = true;
    EXPECT_EQ(DecideGather(f, true), GatherAct::None);
    f = GatherFacts{};
    f.online = false;
    EXPECT_EQ(DecideGather(f, false), GatherAct::Drop);    // offline: drop at once
    f = GatherFacts{};
    f.alive = false;
    EXPECT_EQ(DecideGather(f, false), GatherAct::None);
    EXPECT_EQ(DecideGather(f, true), GatherAct::Drop);     // dead outside the dungeon after the deadline
    f.runDeathWitnessed = true;
    EXPECT_EQ(DecideGather(f, true), GatherAct::None);  // released run corpse: grace / wipe decide
    f.runDeathWitnessed = false;
    f.inDungeon = true;
    f.arrived = true;
    EXPECT_EQ(DecideGather(f, true), GatherAct::None);     // dead inside: the healer / grace decide

    std::vector<Role> const five = {Role::Tank, Role::Healer, Role::Dps, Role::Dps, Role::Dps};
    EXPECT_TRUE(KeepAfterDrop(five, {Role::Tank, Role::Healer, Role::Dps, Role::Dps}));  // run with 4
    EXPECT_TRUE(KeepAfterDrop(five, {Role::Tank, Role::Healer, Role::Dps}));
    EXPECT_FALSE(KeepAfterDrop(five, {Role::Tank, Role::Healer}));                     // too few
    EXPECT_FALSE(KeepAfterDrop(five, {Role::Healer, Role::Dps, Role::Dps, Role::Dps}));  // tank gone
    EXPECT_FALSE(KeepAfterDrop(five, {Role::Tank, Role::Dps, Role::Dps, Role::Dps}));    // healer gone
    // A party that never had a tank only keeps its healer.
    EXPECT_TRUE(KeepAfterDrop({Role::Healer, Role::Dps, Role::Dps, Role::Dps}, {Role::Healer, Role::Dps, Role::Dps}));
}

TEST(PartyPolicyTest, RecruitRequireTankNeedsATankBesidesTheHealer)
{
    std::vector<DungeonDef> const defs = ParseDungeons("47:30:40:AH");
    RecruitParams req;
    req.requireTank = true;
    char const* why = nullptr;

    // Priest + dps: flag off forms healer-only; RequireTank refuses with no_tank.
    std::vector<Candidate> cands = {R(1, 31, kPriest), R(2, 31, kRogue), R(3, 31, kMage), R(4, 31, kHunter)};
    EXPECT_EQ(PlanRecruit(cands, kHorde, defs, Known(1), RecruitParams{}).guids.size(), 4u);
    EXPECT_TRUE(PlanRecruit(cands, kHorde, defs, Known(1), req, &why).guids.empty());
    EXPECT_STREQ(why, "no_tank");

    // A druid without a bear form (canTank false) is no tank.
    cands.push_back(R(5, 31, kDruid));
    EXPECT_TRUE(PlanRecruit(cands, kHorde, defs, Known(1), req).guids.empty());

    // A lone paladin cannot both tank and heal.
    std::vector<Candidate> pal = {T(R(1, 31, kPaladin)), R(2, 31, kRogue), R(3, 31, kMage), R(4, 31, kHunter)};
    EXPECT_TRUE(PlanRecruit(pal, kHorde, defs, Known(1), req, &why).guids.empty());
    EXPECT_STREQ(why, "no_tank");
    // With a priest: the paladin tanks, the priest heals, the paladin leads.
    pal.push_back(R(6, 31, kPriest));
    Plan pl = PlanRecruit(pal, kHorde, defs, Known(1), req, &why);
    EXPECT_STREQ(why, "");
    ASSERT_EQ(pl.guids.size(), 5u);
    EXPECT_EQ(pl.guids.front(), 1u);
    EXPECT_EQ(pl.roles.front(), Role::Tank);
    EXPECT_EQ(pl.roles.back(), Role::Healer);
    EXPECT_EQ(pl.leader, 1u);

    // Tank first by rank (warrior 1 < paladin 2 < druid 3): two hybrids split the roles, the paladin tanks and
    // the druid heals; a warrior then tanks and the druid still heals.
    std::vector<Candidate> hy = {T(R(1, 31, kDruid)), T(R(2, 31, kPaladin)), R(3, 31, kRogue), R(4, 31, kMage)};
    pl = PlanRecruit(hy, kHorde, defs, Known(1), req);
    EXPECT_EQ(pl.roles, (std::vector<Role>{Role::Healer, Role::Tank, Role::Dps, Role::Dps}));
    hy.push_back(T(R(5, 31, kWarrior)));
    pl = PlanRecruit(hy, kHorde, defs, Known(1), req);
    EXPECT_EQ(pl.roles, (std::vector<Role>{Role::Healer, Role::Dps, Role::Dps, Role::Dps, Role::Tank}));
    EXPECT_EQ(pl.leader, 5u);
}

TEST(PartyPolicyTest, SplitSquadPlanRequiresExactExclusivePartition)
{
    std::vector<std::uint32_t> const legacy = {1, 2, 3, 4, 5};
    EXPECT_TRUE(SplitSquadPlanMatches(legacy, {{1, 2, 3}, {4, 5}, {}, {}}));
    EXPECT_TRUE(SplitSquadPlanMatches({5, 4, 3, 2, 1}, {{5, 3, 1}, {4, 2}}));
    EXPECT_FALSE(SplitSquadPlanMatches(legacy, {{1, 2, 3, 4, 5}}));       // no split
    EXPECT_FALSE(SplitSquadPlanMatches(legacy, {{1, 2, 3}, {4}}));        // partial
    EXPECT_FALSE(SplitSquadPlanMatches(legacy, {{1, 2, 3}, {4, 5, 6}}));  // superset
    EXPECT_FALSE(SplitSquadPlanMatches(legacy, {{1, 2, 3}, {3, 4, 5}}));  // duplicate membership
}

TEST(PartyPolicyTest, SplitSquadRetirementFailsClosedForControlsAndForeignOwnership)
{
    using D = SplitRetireDecision;
    using S = SplitNativeShape;
    EXPECT_EQ(DecideSplitRetire(false, S::Ungrouped, true, true, false, false), D::Defer);
    EXPECT_EQ(DecideSplitRetire(true, S::Ungrouped, true, true, false, false), D::Noop);
    EXPECT_EQ(DecideSplitRetire(true, S::ExactSuccessors, true, true, false, false), D::Noop);
    EXPECT_EQ(DecideSplitRetire(true, S::Foreign, true, true, true, true), D::Defer);
    EXPECT_EQ(DecideSplitRetire(true, S::ExactLegacy, true, false, true, true), D::Defer);
    EXPECT_EQ(DecideSplitRetire(true, S::ExactLegacy, false, true, true, true), D::Defer);
    EXPECT_EQ(DecideSplitRetire(true, S::ExactLegacy, true, true, true, false), D::RetireOwned);
    EXPECT_EQ(DecideSplitRetire(true, S::ExactLegacy, true, true, false, true), D::RetireOrphan);
    EXPECT_EQ(DecideSplitRetire(true, S::ExactLegacy, true, true, false, false), D::Defer);
    EXPECT_TRUE(ShouldDisbandOrphan(true, true, false));  // permanent squad membership does not suppress cleanup
    EXPECT_FALSE(ShouldDisbandOrphan(true, true, true));  // pending split owns native mutation
    EXPECT_FALSE(ShouldDisbandOrphan(false, true, false));
}

MaterialDriverFacts SafeMaterialDriverFacts()
{
    MaterialDriverFacts facts;
    facts.materialMode = true;
    facts.registeredSquad = true;
    facts.configuredRosterExact = true;
    facts.nativeRosterExact = true;
    facts.registeredLeaderExact = true;
    facts.nativeLeaderExact = true;
    facts.ordinaryGroup = true;
    facts.guardMask = kMaterialDriverAllGuards;
    facts.strategiesExact = false;
    return facts;
}

TEST(PartyPolicyTest, MaterialDriverIsExplicitDefaultOffAndVersionedFailClosed)
{
    MaterialDriverFacts facts = SafeMaterialDriverFacts();
    facts.materialMode = false;
    EXPECT_EQ(DecideMaterialDriver(facts), MaterialDriverDecision::Disabled);
    facts.materialMode = true;
    facts.version = kMaterialDriverPolicyVersion + 1;
    EXPECT_EQ(DecideMaterialDriver(facts), MaterialDriverDecision::Defer);
    facts.version = kMaterialDriverPolicyVersion;
    EXPECT_EQ(DecideMaterialDriver(facts), MaterialDriverDecision::Repair);
    facts.strategiesExact = true;
    EXPECT_EQ(DecideMaterialDriver(facts), MaterialDriverDecision::Noop);
}

TEST(PartyPolicyTest, MaterialDriverRequiresExactRegisteredNativeRosterAndLeaders)
{
    MaterialDriverFacts facts = SafeMaterialDriverFacts();
    bool MaterialDriverFacts::* exact[] = {
        &MaterialDriverFacts::registeredSquad,   &MaterialDriverFacts::configuredRosterExact,
        &MaterialDriverFacts::nativeRosterExact, &MaterialDriverFacts::registeredLeaderExact,
        &MaterialDriverFacts::nativeLeaderExact, &MaterialDriverFacts::ordinaryGroup};
    for (auto member : exact)
    {
        facts.*member = false;
        EXPECT_EQ(DecideMaterialDriver(facts), MaterialDriverDecision::Defer);
        facts.*member = true;
    }
}

TEST(PartyPolicyTest, MaterialDriverRejectsEveryMissingCustodySafetyAndOwnerGuard)
{
    MaterialDriverFacts facts = SafeMaterialDriverFacts();
    for (std::uint32_t bit = 1; bit <= MaterialDriverNoCampaignTravel; bit <<= 1)
    {
        facts.guardMask = kMaterialDriverAllGuards & ~bit;
        EXPECT_EQ(DecideMaterialDriver(facts), MaterialDriverDecision::Defer) << bit;
    }
    facts.guardMask = kMaterialDriverAllGuards | (1u << 31);  // unknown future owner fact also fails closed
    EXPECT_EQ(DecideMaterialDriver(facts), MaterialDriverDecision::Defer);
}

TEST(PartyPolicyTest, MaterialDriverDeltaIsMinimalAndIdempotent)
{
    MaterialDriverDelta delta = PlanMaterialDriverDelta(false, false, true);
    EXPECT_TRUE(delta.addGrind);
    EXPECT_TRUE(delta.addNewRpg);
    EXPECT_TRUE(delta.removeFollow);
    EXPECT_TRUE(delta.Any());
    delta = PlanMaterialDriverDelta(true, true, false);
    EXPECT_FALSE(delta.Any());
    delta = PlanMaterialDriverDelta(true, false, false);
    EXPECT_FALSE(delta.addGrind);
    EXPECT_TRUE(delta.addNewRpg);
    EXPECT_FALSE(delta.removeFollow);
    EXPECT_EQ(MaterialDriverRosterHash({3, 1, 2}), MaterialDriverRosterHash({1, 2, 3}));
    EXPECT_NE(MaterialDriverRosterHash({1, 2}), MaterialDriverRosterHash({1, 2, 3}));
}

TEST(PartyPolicyTest, MaterialDriverRepairsIdleTargetWithoutInspectingAlreadyExactActivePeer)
{
    MaterialDriverDelta const activePeer = PlanMaterialDriverDelta(true, true, false);
    EXPECT_FALSE(activePeer.Any());  // runtime skips its activity/owner getters before target admission

    MaterialDriverFacts idleTarget = SafeMaterialDriverFacts();
    EXPECT_EQ(DecideMaterialDriver(idleTarget), MaterialDriverDecision::Repair);

    std::uint32_t const foreignControlled = kMaterialDriverAllGuards & ~MaterialDriverMasterNull;
    EXPECT_FALSE(MaterialDriverCustodyReady(foreignControlled));  // any foreign-controlled peer blocks the crew
    EXPECT_TRUE(MaterialDriverCustodyReady(kMaterialDriverAllGuards));
}

TEST(PartyPolicyTest, MaterialDriverZoneOwnerReadDistinguishesAbsentCurrentAndInvalidState)
{
    AutoWowZoneProgression::BotState state;
    EXPECT_FALSE(AutoWowZoneProgression::ShouldDeferMaterialDriver(false, state));
    EXPECT_FALSE(AutoWowZoneProgression::ShouldDeferMaterialDriver(true, state));
    state.phase = AutoWowZoneProgression::Phase::Travel;
    EXPECT_TRUE(AutoWowZoneProgression::ShouldDeferMaterialDriver(true, state));
    state.phase = AutoWowZoneProgression::Phase::None;
    state.version = AutoWowZoneProgression::kStateVersion + 1;
    EXPECT_TRUE(AutoWowZoneProgression::ShouldDeferMaterialDriver(true, state));
}

std::string ReadModuleSource(char const* relative)
{
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path() / relative,
                        std::ios::in | std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::size_t CountText(std::string const& text, std::string const& needle)
{
    std::size_t count = 0;
    for (std::size_t at = 0; (at = text.find(needle, at)) != std::string::npos; at += needle.size())
        ++count;
    return count;
}

TEST(PartyPolicyTest, MaterialDriverRuntimeHasOnlyTheTwoAdmittedMaintenanceSites)
{
    std::string const runtime = ReadModuleSource("src/AutoWow/PartyRuntime.cpp");
    std::string const squad = ReadModuleSource("src/Ai/World/Rpg/Action/NewRpgSquad.cpp");
    ASSERT_FALSE(runtime.empty());
    ASSERT_FALSE(squad.empty());
    EXPECT_NE(squad.find("EnsureSquad(roster, gMaterialCrews)"), std::string::npos);
    EXPECT_EQ(CountText(runtime, "MaintainMaterialDriver("), 3u);  // definition + intact + new registration

    std::size_t const ensure = runtime.find("void EnsureSquad(");
    std::size_t const combat = runtime.find("void CombatUpdate(", ensure);
    ASSERT_NE(ensure, std::string::npos);
    ASSERT_NE(combat, std::string::npos);
    std::string const body = runtime.substr(ensure, combat - ensure);
    std::size_t const intactRepair = body.find("MaintainMaterialDriver(registered, roster, group, materialCrew)");
    std::size_t const intactReturn = body.find("return;", intactRepair);
    std::size_t const baseline = body.find("s.nonCombat0 = ai->GetStrategies(BOT_STATE_NON_COMBAT)");
    std::size_t const formedRepair = body.find("MaintainMaterialDriver(party, roster, group, materialCrew)");
    EXPECT_NE(intactRepair, std::string::npos);
    EXPECT_NE(intactReturn, std::string::npos);
    EXPECT_LT(intactRepair, intactReturn);
    EXPECT_NE(baseline, std::string::npos);
    EXPECT_NE(formedRepair, std::string::npos);
    EXPECT_LT(baseline, formedRepair);
    EXPECT_NE(runtime.find("StrategyUndo(s.nonCombat0"), std::string::npos);
}

TEST(PartyPolicyTest, MaterialDriverZoneOwnerGetterIsReadOnlyAndVersionQualified)
{
    std::string const source = ReadModuleSource("src/Ai/World/Rpg/Action/NewRpgZoneProgression.cpp");
    std::size_t const begin = source.find("bool MaterialDriverAdmissionBlocked(");
    std::size_t const end = source.find("void CancelTrip(", begin);
    ASSERT_NE(begin, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    std::string const body = source.substr(begin, end - begin);
    EXPECT_NE(body.find("gStates.find(guid)"), std::string::npos);
    EXPECT_NE(body.find("ShouldDeferMaterialDriver(true, it->second)"), std::string::npos);
    EXPECT_EQ(body.find("gStates["), std::string::npos);
    EXPECT_EQ(body.find("LoadState("), std::string::npos);
    EXPECT_EQ(body.find("StoreState("), std::string::npos);
}

TEST(PartyPolicyTest, MaterialDriverRepairPathHasNoControlOrGroupMutation)
{
    std::string const runtime = ReadModuleSource("src/AutoWow/PartyRuntime.cpp");
    std::size_t const guardBegin = runtime.find("std::uint32_t MaterialDriverTargetGuards(");
    std::size_t const guardEnd = runtime.find("std::string MaterialDriverChange(", guardBegin);
    std::size_t const begin = runtime.find("void MaintainMaterialDriver(");
    std::size_t const end = runtime.find("bool SuccessorsAlreadyNative(", begin);
    ASSERT_NE(guardBegin, std::string::npos);
    ASSERT_NE(guardEnd, std::string::npos);
    ASSERT_NE(begin, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    std::string const guards = runtime.substr(guardBegin, guardEnd - guardBegin);
    std::string const body = runtime.substr(begin, end - begin);
    std::size_t const legacyReturn = body.find("if (!materialCrew)");
    ASSERT_NE(legacyReturn, std::string::npos);
    EXPECT_LT(legacyReturn, body.find("Guids(party)"));
    std::size_t const exactSkip = body.find("if (!deltas[i].Any())");
    std::size_t const targetOwners = body.find("MaterialDriverTargetGuards(bots[i], ais[i])");
    ASSERT_NE(exactSkip, std::string::npos);
    ASSERT_NE(targetOwners, std::string::npos);
    EXPECT_LT(exactSkip, targetOwners);
    EXPECT_NE(body.find("MaterialDriverCustodyReady(allCustody)"), std::string::npos);
    EXPECT_NE(body.find("ChangeStrategy(change, BOT_STATE_NON_COMBAT)"), std::string::npos);
    EXPECT_NE(guards.find("AutoWowSafeRevive::ReadDiagnostic(guid)"), std::string::npos);
    EXPECT_NE(guards.find("AutoWowDeathLoop::ReadDiagnostic(guid)"), std::string::npos);
    EXPECT_NE(guards.find("MaterialDriverAdmissionBlocked(guid)"), std::string::npos);
    EXPECT_NE(guards.find("MaterialStintAdmissionBlocked(guid)"), std::string::npos);
    for (char const* forbidden : {"Reset(", "Disband(", "AddMember(", "Create(", "SetMaster(",
                                  "SetAutoWowIndependentParty(", "TeleportTo(", "Login", "Replay", "Cleanup"})
        EXPECT_EQ(body.find(forbidden), std::string::npos) << forbidden;
}

}  // namespace
