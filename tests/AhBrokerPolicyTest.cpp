/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AhBrokerPolicy.h"
#include "gtest/gtest.h"

namespace
{
using namespace AutoWowBroker;

Request R(std::uint32_t guid, std::uint8_t team, std::uint32_t level, std::uint64_t budget, std::uint32_t filedMs,
          bool hunter = false)
{
    Request r;
    r.guid = guid;
    r.team = team;
    r.level = level;
    r.hunter = hunter;
    r.budget = budget;
    r.filedMs = filedMs;
    return r;
}

AutoWowGear::AhOffer O(std::uint32_t id, std::uint8_t slot, std::uint32_t gain, std::uint64_t price,
                       bool twoHand = false)
{
    AutoWowGear::AhOffer o;
    o.id = id;
    o.item = id;
    o.slot = slot;
    o.gain = gain;
    o.price = price;
    o.twoHand = twoHand;
    return o;
}

// One open request per bot: a re-file of the same guid replaces in place, a new guid appends.
TEST(AhBrokerQueue, UpsertKeepsOnePerBot)
{
    std::vector<Request> q;
    Upsert(q, R(1001, 0, 70, 5000, 10));
    Upsert(q, R(1002, 0, 72, 6000, 20));
    EXPECT_EQ(q.size(), 2u);
    // Same guid again: replaces, does not grow, keeps its slot (index 0).
    Upsert(q, R(1001, 0, 71, 9000, 30));
    EXPECT_EQ(q.size(), 2u);
    ASSERT_NE(Find(q, 1001), kNone);
    EXPECT_EQ(q[Find(q, 1001)].budget, 9000u);
    EXPECT_EQ(q[Find(q, 1001)].level, 71u);
    EXPECT_EQ(Find(q, 0u), kNone);
}

TEST(AhBrokerQueue, EraseRemovesServed)
{
    std::vector<Request> q;
    Upsert(q, R(1, 0, 70, 1, 1));
    Upsert(q, R(2, 0, 70, 1, 1));
    EXPECT_TRUE(Erase(q, 1));
    EXPECT_FALSE(Erase(q, 1));  // already gone
    EXPECT_EQ(q.size(), 1u);
    EXPECT_EQ(q.front().guid, 2u);
}

// Service order: oldest (lower filedMs) first, ties on lower guid; only the asked team; stable + deterministic.
TEST(AhBrokerQueue, OrderForTeamIsDeterministic)
{
    std::vector<Request> q;
    Upsert(q, R(50, 0, 70, 1, 200));
    Upsert(q, R(40, 1, 70, 1, 100));  // horde: filtered out below
    Upsert(q, R(30, 0, 70, 1, 100));
    Upsert(q, R(20, 0, 70, 1, 100));  // same filedMs as 30 -> lower guid first
    std::vector<Request> const ally = OrderForTeam(q, 0);
    ASSERT_EQ(ally.size(), 3u);
    EXPECT_EQ(ally[0].guid, 20u);
    EXPECT_EQ(ally[1].guid, 30u);
    EXPECT_EQ(ally[2].guid, 50u);
    EXPECT_EQ(OrderForTeam(q, 1).size(), 1u);
}

TEST(AhBrokerRate, CanBuyUntilCap)
{
    Params p;
    p.maxBuysPerMin = 6;
    EXPECT_TRUE(CanBuy(p, 0));
    EXPECT_TRUE(CanBuy(p, 5));
    EXPECT_FALSE(CanBuy(p, 6));
    EXPECT_FALSE(CanBuy(p, 7));
}

TEST(AhBrokerCod, PriceAddsFee)
{
    Params p;
    p.feeCopper = 0;
    EXPECT_EQ(CodPrice(p, 12345u), 12345u);
    p.feeCopper = 500;
    EXPECT_EQ(CodPrice(p, 12345u), 12845u);
}

// Accept only the broker's mail that the requester can pay; anyone else, or an unaffordable COD, is returned.
TEST(AhBrokerCod, RequesterVerdict)
{
    EXPECT_EQ(DecideRequesterCod(777, 777, 1000, 1000), CodVerdict::Accept);
    EXPECT_EQ(DecideRequesterCod(777, 777, 1000, 999), CodVerdict::ReturnOverBudget);
    EXPECT_EQ(DecideRequesterCod(888, 777, 1000, 100000), CodVerdict::ReturnNotBroker);  // not from the broker
    EXPECT_EQ(DecideRequesterCod(0, 777, 0, 0), CodVerdict::ReturnNotBroker);
}

// PlanForRequest is the requester's own AhPlan: picks within the request budget, in slot order, never over budget.
TEST(AhBrokerPlan, ForwardsToAhPlanWithinBudget)
{
    AutoWowGear::detail::gAhParams = AutoWowGear::AhParams{};  // maxBuys 3, priceMult 20
    std::vector<AutoWowGear::AhOffer> offers = {
        O(1, AutoWowGear::kSlotMainHand, 40, 3000),
        O(2, 4 /*chest*/, 30, 3000),
        O(3, 6 /*legs*/, 20, 3000),
    };
    // Budget covers the main hand + chest (6000) but not all three (9000); level 70 item cap = 70*70*20 = 98000.
    std::vector<AutoWowGear::AhOffer> const plan = PlanForRequest(R(1, 0, 70, 6000, 1), offers);
    std::uint64_t spent = 0;
    for (AutoWowGear::AhOffer const& o : plan)
        spent += o.price;
    EXPECT_LE(spent, 6000u);
    EXPECT_FALSE(plan.empty());
    // A zero budget buys nothing.
    EXPECT_TRUE(PlanForRequest(R(1, 0, 70, 0, 1), offers).empty());
}
}  // namespace
