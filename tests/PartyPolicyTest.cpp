/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PartyPolicy.h"

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowParty;

Candidate C(std::uint32_t guid, std::uint32_t level, std::uint32_t cls, std::uint32_t zone = 17,
            std::uint8_t team = kHorde)
{
    Candidate c;
    c.guid = guid;
    c.level = level;
    c.cls = cls;
    c.team = team;
    c.map = 1;
    c.zone = zone;
    return c;
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
}
}  // namespace
