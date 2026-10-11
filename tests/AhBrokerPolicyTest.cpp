/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AhBrokerPolicy.h"
#include "gtest/gtest.h"

#include <atomic>
#include <thread>

namespace
{
using namespace AutoWowBroker;

Request R(std::uint32_t guid, std::uint8_t team, std::uint32_t level, std::uint64_t budget, std::uint32_t filedSec,
          bool hunter = false)
{
    Request r;
    r.guid = guid;
    r.team = team;
    r.level = level;
    r.hunter = hunter;
    r.budget = budget;
    r.filedSec = filedSec;
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

// Service order: oldest (lower filedSec) first, ties on lower guid; only the asked team; stable + deterministic.
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

TEST(AhBrokerGear, RejectsNorthrendFillerEvenForEmptySlots)
{
    Params p;
    for (std::uint32_t const ilvl : {21u, 30u, 36u, 49u, 65u})
        EXPECT_FALSE(MeetsGearFloor(p, 70, ilvl));
    EXPECT_FALSE(MeetsGearFloor(p, 75, 154));
    EXPECT_TRUE(MeetsGearFloor(p, 75, 155));
    EXPECT_FALSE(MeetsGearFloor(p, 78, 173));
    EXPECT_TRUE(MeetsGearFloor(p, 78, 174));
}

TEST(AhBrokerGear, UsesExpansionCurveRegardlessOfRunDueFlag)
{
    Params p;
    bool const saved = AutoWowGear::detail::gCatchUpEnabled;
    for (bool const enabled : {false, true})
    {
        AutoWowGear::detail::gCatchUpEnabled = enabled;
        EXPECT_FALSE(MeetsGearFloor(p, 70, 115));
        EXPECT_TRUE(MeetsGearFloor(p, 70, 116));
        EXPECT_FALSE(MeetsGearFloor(p, 71, 129));
        EXPECT_TRUE(MeetsGearFloor(p, 71, 130));
    }
    AutoWowGear::detail::gCatchUpEnabled = saved;
}

TEST(AhBrokerGear, ConfigurableFloorRoundsUpAndClamps)
{
    Params p;
    p.minIlvlPct = 75;
    EXPECT_FALSE(MeetsGearFloor(p, 78, 130));
    EXPECT_TRUE(MeetsGearFloor(p, 78, 131));
    p.minIlvlPct = 0;
    EXPECT_TRUE(MeetsGearFloor(p, 78, 21));
    p.minIlvlPct = 0xffffffffu;
    EXPECT_FALSE(MeetsGearFloor(p, 78, 173));
    EXPECT_TRUE(MeetsGearFloor(p, 78, 174));
    EXPECT_TRUE(MeetsGearFloor(p, 78, 0xffffffffu));
}

TEST(AhBrokerGear, LowLevelsKeepTheirGreenCurve)
{
    Params p;
    EXPECT_FALSE(MeetsGearFloor(p, 10, 14));
    EXPECT_TRUE(MeetsGearFloor(p, 10, 15));
    EXPECT_FALSE(MeetsGearFloor(p, 58, 61));
    EXPECT_TRUE(MeetsGearFloor(p, 58, 62));
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

class AhBrokerRenewal : public ::testing::Test
{
protected:
    void SetUp() override
    {
        std::lock_guard<std::mutex> guard(detail::gQueueLock);
        _enabled = detail::gEnabled;
        _queue = detail::gQueue;
        _expired = detail::gUnservedExpired;
        detail::gEnabled = true;
        detail::gQueue.clear();
        detail::gUnservedExpired.clear();
    }

    void TearDown() override
    {
        std::lock_guard<std::mutex> guard(detail::gQueueLock);
        detail::gEnabled = _enabled;
        detail::gQueue = _queue;
        detail::gUnservedExpired = _expired;
    }

    bool _enabled = false;
    std::vector<Request> _queue;
    std::vector<Request> _expired;
};

TEST_F(AhBrokerRenewal, UnservedLevel80ReassessesOnceWithFreshBudget)
{
    Request const request = R(9001, 0, 80, 10000000, 1);
    File(request);
    ASSERT_TRUE(ExpireUnserved(request));
    ASSERT_TRUE(ConsumeUnservedExpiry(request.guid, 80, 80));
    EXPECT_FALSE(ConsumeUnservedExpiry(request.guid, 80, 80));
    AutoWowGear::AhParams const params;
    EXPECT_TRUE(AutoWowGear::AhRunDue(params, 80, 0, 10000000, 0, 0));
    EXPECT_FALSE(AutoWowGear::AhRunDue(params, 80, 0, 0, 0, 0));
}

TEST_F(AhBrokerRenewal, ServedRequestHasNoRenewalAuthority)
{
    Request const request = R(9001, 0, 80, 10000000, 1);
    File(request);
    Forget(request.guid);
    EXPECT_FALSE(ExpireUnserved(request));
    EXPECT_FALSE(ConsumeUnservedExpiry(request.guid, 80, 80));
}

TEST_F(AhBrokerRenewal, StaleSnapshotPreservesEveryReplacementTerm)
{
    Request const original = R(9001, 0, 80, 10000000, 1);
    for (unsigned term = 0; term < 5; ++term)
    {
        File(original);
        Request replacement = original;
        switch (term)
        {
            case 0: replacement.team = 1; break;
            case 1: replacement.level = 79; break;
            case 2: replacement.hunter = true; break;
            case 3: replacement.budget = 9999999; break;
            case 4: replacement.filedSec = 2; break;
        }
        File(replacement);
        EXPECT_FALSE(ExpireUnserved(original));
        std::vector<Request> const pending = TeamRequests(replacement.team);
        ASSERT_EQ(pending.size(), 1u);
        EXPECT_EQ(pending[0].team, replacement.team);
        EXPECT_EQ(pending[0].level, replacement.level);
        EXPECT_EQ(pending[0].hunter, replacement.hunter);
        EXPECT_EQ(pending[0].budget, replacement.budget);
        EXPECT_EQ(pending[0].filedSec, replacement.filedSec);
        EXPECT_FALSE(ConsumeUnservedExpiry(original.guid, 80, 80));
        Forget(original.guid);
    }
}

TEST_F(AhBrokerRenewal, NewRequestClearsOldExpiry)
{
    Request const request = R(9001, 0, 80, 10000000, 1);
    File(request);
    ASSERT_TRUE(ExpireUnserved(request));
    Request replacement = request;
    replacement.filedSec = 2;
    File(replacement);
    EXPECT_FALSE(ConsumeUnservedExpiry(request.guid, 80, 80));
    ASSERT_EQ(TeamRequests(0).size(), 1u);
    EXPECT_EQ(TeamRequests(0)[0].filedSec, 2u);
}

TEST_F(AhBrokerRenewal, LevelMismatchCannotRenewOldToken)
{
    Request const request = R(9001, 0, 80, 10000000, 1);
    File(request);
    ASSERT_TRUE(ExpireUnserved(request));
    EXPECT_FALSE(ConsumeUnservedExpiry(request.guid, 80, 79));
    EXPECT_TRUE(ConsumeUnservedExpiry(request.guid, 80, 80));
    File(request);
    ASSERT_TRUE(ExpireUnserved(request));
    EXPECT_FALSE(ConsumeUnservedExpiry(request.guid, 79, 79));
    EXPECT_FALSE(ConsumeUnservedExpiry(request.guid, 80, 80));
}

TEST_F(AhBrokerRenewal, DisabledFeatureDoesNotExpireOrConsume)
{
    Request const request = R(9001, 0, 80, 10000000, 1);
    File(request);
    detail::gEnabled = false;
    EXPECT_FALSE(ExpireUnserved(request));
    EXPECT_FALSE(ConsumeUnservedExpiry(request.guid, 80, 80));
    EXPECT_EQ(TeamRequests(0).size(), 1u);
    EXPECT_TRUE(detail::gUnservedExpired.empty());
    detail::gEnabled = true;
    ASSERT_TRUE(ExpireUnserved(request));
    detail::gEnabled = false;
    EXPECT_FALSE(ConsumeUnservedExpiry(request.guid, 80, 80));
    detail::gEnabled = true;
    EXPECT_TRUE(ConsumeUnservedExpiry(request.guid, 80, 80));
}

TEST_F(AhBrokerRenewal, ExpiryStorageRemainsBounded)
{
    for (std::uint32_t guid = 1; guid <= detail::kMaxOpen + 1; ++guid)
    {
        Request const request = R(guid, 0, 80, 10000000, 1);
        File(request);
        ASSERT_TRUE(ExpireUnserved(request));
    }
    EXPECT_EQ(detail::gUnservedExpired.size(), detail::kMaxOpen);
    EXPECT_TRUE(TeamRequests(0).empty());
    EXPECT_FALSE(ConsumeUnservedExpiry(1, 80, 80));
    EXPECT_TRUE(ConsumeUnservedExpiry(detail::kMaxOpen + 1, 80, 80));
}

TEST_F(AhBrokerRenewal, ConcurrentConsumptionHasOneWinner)
{
    Request const request = R(9001, 0, 80, 10000000, 1);
    File(request);
    ASSERT_TRUE(ExpireUnserved(request));
    std::atomic<unsigned> winners{0};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 8; ++i)
        threads.emplace_back([&]()
        {
            if (ConsumeUnservedExpiry(request.guid, 80, 80))
                ++winners;
        });
    for (std::thread& thread : threads)
        thread.join();
    EXPECT_EQ(winners.load(), 1u);
}
}  // namespace
