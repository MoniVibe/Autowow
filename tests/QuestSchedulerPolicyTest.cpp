/*
 * Pure quest-log scheduler tests (QuestSchedulerPolicy.h): order, slice rotation, cooldown, momentum
 * and avoid-list parsing. No world access; the runtime wiring lives in NewRpgBaseAction.
 */

#include "QuestSchedulerPolicy.h"

#include "gtest/gtest.h"

namespace
{
using namespace QuestSchedulerPolicy;

Candidate Make(std::uint32_t questId, std::uint32_t yards, bool complete = false, bool grey = false,
               std::uint32_t lastProgressMs = 0)
{
    Candidate c;
    c.questId = questId;
    c.distanceYards = yards;
    c.complete = complete;
    c.grey = grey;
    c.lastProgressMs = lastProgressMs;
    return c;
}

Observation Obs(std::uint32_t questId, std::uint16_t kills, bool complete = false)
{
    Observation o;
    o.counters.quest = questId;
    o.counters.c[0] = kills;
    o.complete = complete;
    return o;
}

constexpr std::uint32_t kNow = 10'000'000;

TEST(QuestSchedulerPolicyTest, TurnInBeatsNearerObjective)
{
    std::vector<Candidate> const cs = {Make(170, 40), Make(233, 600, true), Make(3361, 20)};
    EXPECT_EQ(cs[PickBest(cs, kNow)].questId, 233u);
}

TEST(QuestSchedulerPolicyTest, NearestObjectiveWinsWithinTier)
{
    std::vector<Candidate> const cs = {Make(170, 300), Make(3361, 120), Make(792, 450)};
    EXPECT_EQ(cs[PickBest(cs, kNow)].questId, 3361u);
}

TEST(QuestSchedulerPolicyTest, MomentumBonusBeatsSlightlyNearerQuest)
{
    std::vector<Candidate> cs = {Make(170, 100), Make(3361, 200, false, false, kNow - 60'000)};
    EXPECT_EQ(cs[PickBest(cs, kNow)].questId, 3361u);  // 200 - 150 = 50 < 100
    // Momentum expires after the window.
    cs[1].lastProgressMs = kNow - kMomentumWindowMs;
    EXPECT_EQ(cs[PickBest(cs, kNow)].questId, 170u);
}

// AutoWow.QuestScheduler.PreferGearRewards (soak-s14-full-r1: 0 greens, starter weapons at L9-15).
TEST(QuestSchedulerPolicyTest, GearRewardBonusWeaponFirst)
{
    EXPECT_EQ(GearBonusYards(false, false), 0u);
    EXPECT_EQ(GearBonusYards(false, true), kGearArmorBonusYards);
    EXPECT_EQ(GearBonusYards(true, false), kGearWeaponBonusYards);
    EXPECT_EQ(GearBonusYards(true, true), kGearWeaponBonusYards);
    EXPECT_GT(kGearWeaponBonusYards, kGearArmorBonusYards);
    EXPECT_FALSE(PreferGearRewards());  // default off

    // A weapon-reward quest 350 yd away beats a plain one at 100 yd (350 - 300 = 50).
    std::vector<Candidate> cs = {Make(170, 100), Make(3361, 350)};
    EXPECT_EQ(cs[PickBest(cs, kNow)].questId, 170u);  // no bonus: identical to the stock order
    cs[1].gearBonusYards = GearBonusYards(true, false);
    EXPECT_EQ(cs[PickBest(cs, kNow)].questId, 3361u);
    // An armor reward alone does not outweigh 250 yd.
    cs[1].gearBonusYards = GearBonusYards(false, true);
    EXPECT_EQ(cs[PickBest(cs, kNow)].questId, 170u);
    // Saturates at 0 and stacks after momentum; tiers still rule (a turn-in stays first).
    cs = {Make(170, 900, true), Make(3361, 100, false, false, kNow - 1)};
    cs[1].gearBonusYards = kGearWeaponBonusYards;
    EXPECT_EQ(std::get<1>(SortKey(cs[1], kNow)), 0u);
    EXPECT_EQ(cs[PickBest(cs, kNow)].questId, 170u);
}

TEST(QuestSchedulerPolicyTest, GreyCheapAfterLevelAppropriateAndFarGreyExcluded)
{
    std::vector<Candidate> cs = {Make(10, 50, false, true), Make(20, 900)};
    EXPECT_EQ(cs[PickBest(cs, kNow)].questId, 20u);
    EXPECT_FALSE(IsDroppableGrey(cs[0]));

    std::vector<Candidate> const farGrey = {Make(10, kCheapGreyYards + 1, false, true)};
    EXPECT_TRUE(IsDroppableGrey(farGrey[0]));
    EXPECT_EQ(PickBest(farGrey, kNow), npos);

    // A grey turn-in is still a turn-in.
    std::vector<Candidate> const greyTurnIn = {Make(10, 800, true, true), Make(20, 10)};
    EXPECT_FALSE(IsDroppableGrey(greyTurnIn[0]));
    EXPECT_EQ(greyTurnIn[PickBest(greyTurnIn, kNow)].questId, 10u);
}

TEST(QuestSchedulerPolicyTest, TieBreaksByQuestIdRegardlessOfLogOrder)
{
    std::vector<Candidate> const a = {Make(900, 50), Make(300, 50), Make(600, 50)};
    std::vector<Candidate> const b = {Make(600, 50), Make(900, 50), Make(300, 50)};
    EXPECT_EQ(a[PickBest(a, kNow)].questId, 300u);
    EXPECT_EQ(b[PickBest(b, kNow)].questId, 300u);
    EXPECT_EQ(PickBest({}, kNow), npos);
}

TEST(QuestSchedulerPolicyTest, SliceExpiresWithoutProgressAndProgressExtendsIt)
{
    Slice slice;
    EXPECT_FALSE(SliceExpired(slice, 0, kNow));  // no directive
    BeginSlice(slice, 3361, false, kNow);
    EXPECT_FALSE(SliceExpired(slice, 0, kNow + kSliceMs - 1));
    EXPECT_TRUE(SliceExpired(slice, 0, kNow + kSliceMs));
    // A counter change 4 min in re-arms the slice from that moment.
    std::uint32_t const progress = kNow + 4 * 60 * 1000;
    EXPECT_FALSE(SliceExpired(slice, progress, kNow + kSliceMs));
    EXPECT_TRUE(SliceExpired(slice, progress, progress + kSliceMs));
    // Progress older than the slice start does not extend it.
    EXPECT_TRUE(SliceExpired(slice, kNow - 1000, kNow + kSliceMs));
}

TEST(QuestSchedulerPolicyTest, TurnInSliceIsLongerAndWrapSafe)
{
    Slice slice;
    std::uint32_t const start = 0xFFFFFFFFu - 1000;  // getMSTime wraps
    BeginSlice(slice, 233, true, start);
    EXPECT_FALSE(SliceExpired(slice, 0, start + kSliceMs));
    EXPECT_TRUE(SliceExpired(slice, 0, start + kTurnInSliceMs));
}

TEST(QuestSchedulerPolicyTest, RotationCooldownExpires)
{
    BotState state;
    state.cooldown.Defer(3361, kNow, kRotateCooldownMs);
    EXPECT_TRUE(state.cooldown.IsDeferred(3361, kNow + kRotateCooldownMs - 1));
    EXPECT_FALSE(state.cooldown.IsDeferred(170, kNow));
    EXPECT_FALSE(state.cooldown.IsDeferred(3361, kNow + kRotateCooldownMs));
}

TEST(QuestSchedulerPolicyTest, MomentumBookBaselineChangeAndForget)
{
    MomentumBook book;
    book.Observe({Obs(170, 0), Obs(3361, 0)}, 1000);
    EXPECT_EQ(book.LastProgressMs(170), 0u);  // first sight is a baseline

    book.Observe({Obs(170, 2), Obs(3361, 0)}, 2000);
    EXPECT_EQ(book.LastProgressMs(170), 2000u);
    EXPECT_EQ(book.LastProgressMs(3361), 0u);

    book.Observe({Obs(170, 2), Obs(3361, 0, true)}, 3000);  // completion counts as progress
    EXPECT_EQ(book.LastProgressMs(170), 2000u);
    EXPECT_EQ(book.LastProgressMs(3361), 3000u);

    book.Observe({Obs(3361, 0, true)}, 4000);  // 170 left the log
    EXPECT_EQ(book.LastProgressMs(170), 0u);
    EXPECT_EQ(book.entries.size(), 1u);
}

// Stuck directive: 3361 never moves; the scheduler rotates it and the next pick is the other quest.
TEST(QuestSchedulerPolicyTest, StuckDirectiveRotatesToNextBest)
{
    BotState state;
    std::uint32_t now = kNow;
    state.momentum.Observe({Obs(3361, 0), Obs(170, 0)}, now);
    std::vector<Candidate> cs = {Make(3361, 300), Make(170, 350)};
    ASSERT_EQ(cs[PickBest(cs, now)].questId, 3361u);
    BeginSlice(state.slice, 3361, false, now);

    for (; !SliceExpired(state.slice, state.momentum.LastProgressMs(3361), now); now += 5000)
        state.momentum.Observe({Obs(3361, 0), Obs(170, 0)}, now);
    EXPECT_EQ(now - kNow, kSliceMs);
    state.cooldown.Defer(3361, now, kRotateCooldownMs);

    std::vector<Candidate> eligible;
    for (Candidate const& c : cs)
        if (!state.cooldown.IsDeferred(c.questId, now))
            eligible.push_back(c);
    ASSERT_EQ(eligible.size(), 1u);
    EXPECT_EQ(eligible[PickBest(eligible, now)].questId, 170u);
}

TEST(QuestSchedulerPolicyTest, ParseQuestIdListFormats)
{
    std::vector<std::uint32_t> ids;
    ASSERT_TRUE(ParseQuestIdList("19,25,28", ids));
    EXPECT_EQ(ids, (std::vector<std::uint32_t>{19, 25, 28}));
    ASSERT_TRUE(ParseQuestIdList(" 30 , 5\n# comment 99\r\n5\t7 # trailing\n", ids));
    EXPECT_EQ(ids, (std::vector<std::uint32_t>{5, 7, 30}));  // sorted, unique, comments ignored
    ASSERT_TRUE(ParseQuestIdList("", ids));
    EXPECT_TRUE(ids.empty());
    ASSERT_TRUE(ParseQuestIdList("4294967295", ids));
    EXPECT_TRUE(ContainsQuestId(ids, 4294967295u));
    EXPECT_FALSE(ContainsQuestId(ids, 1));
}

TEST(QuestSchedulerPolicyTest, ParseQuestIdListRejectsMalformed)
{
    std::vector<std::uint32_t> ids = {42};
    EXPECT_FALSE(ParseQuestIdList("1,2x,3", ids));
    EXPECT_FALSE(ParseQuestIdList("1;2", ids));
    EXPECT_FALSE(ParseQuestIdList("0", ids));
    EXPECT_FALSE(ParseQuestIdList("4294967296", ids));
    EXPECT_FALSE(ParseQuestIdList("-5", ids));
    EXPECT_EQ(ids, (std::vector<std::uint32_t>{42}));  // untouched on failure
}
}  // namespace
