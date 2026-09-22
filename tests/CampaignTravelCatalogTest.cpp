/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "../src/AutoWow/CampaignTravelCatalog.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <vector>

namespace
{
using namespace AutoWowCampaignTravel;

Endpoint At(std::uint32_t map, double x, double y, double z)
{
    return {map, {x, y, z}, true};
}

AreaTriggerRecord DarkPortalForward()
{
    return {4354,
            At(0, -11877.7, -3204.49, -18.49),
            At(530, -248.149, 921.875, 84.3885), 4.0, true};
}

AreaTriggerRecord DarkPortalReturn()
{
    return {4352,
            At(530, -248.149, 921.875, 84.3885),
            At(0, -11877.7, -3204.49, -18.49), 4.0, true};
}

CatalogInput Input(Endpoint current, Endpoint destination)
{
    CatalogInput input;
    input.current = current;
    input.finalDestination = destination;
    input.botMoney = 100000;
    return input;
}

TaxiRecord Taxi()
{
    TaxiRecord taxi;
    taxi.pathId = 359;
    taxi.flightmasterSpawnId = 53178;
    taxi.flightmasterEntry = 12636;
    taxi.source = At(0, 2270.0, -5340.0, 88.0);
    taxi.destination = At(0, 1563.0, 267.0, -43.0);
    taxi.directedNodeIds = {68, 11};
    taxi.cost = 1020;
    taxi.interactionRadius = 5.0;
    taxi.allNodesKnown = true;
    return taxi;
}

TransportRecord Transport()
{
    TransportRecord transport;
    transport.entry = 176495;
    transport.taxiPathId = 1;
    transport.sourceStop = 10;
    transport.destinationStop = 20;
    transport.boardingPoint = At(0, 1.0, 2.0, 3.0);
    transport.destination = At(1, 4.0, 5.0, 6.0);
    transport.boardingRadius = 5.0;
    transport.dynamicMotionTransport = true;
    transport.directedStopsValidated = true;
    return transport;
}
}  // namespace

TEST(CampaignTravelCatalog, UsesIndependentDirectedDarkPortalRecords)
{
    AreaTriggerRecord const forward = DarkPortalForward();
    AreaTriggerRecord const reverse = DarkPortalReturn();

    CatalogInput toOutland = Input(forward.source, forward.destination);
    toOutland.areaTriggers = {forward, reverse};
    CatalogResult forwardResult = BuildCatalog(toOutland);
    ASSERT_EQ(forwardResult.issue, CatalogIssue::None);
    ASSERT_EQ(forwardResult.executableLegs.size(), 1u);
    auto const* forwardPayload =
        std::get_if<AreaTriggerPayload>(&forwardResult.executableLegs.front().payload);
    ASSERT_NE(forwardPayload, nullptr);
    EXPECT_EQ(forwardPayload->triggerId, 4354u);

    CatalogInput toAzeroth = Input(reverse.source, reverse.destination);
    toAzeroth.areaTriggers = {forward, reverse};
    CatalogResult reverseResult = BuildCatalog(toAzeroth);
    ASSERT_EQ(reverseResult.issue, CatalogIssue::None);
    ASSERT_EQ(reverseResult.executableLegs.size(), 1u);
    auto const* reversePayload =
        std::get_if<AreaTriggerPayload>(&reverseResult.executableLegs.front().payload);
    ASSERT_NE(reversePayload, nullptr);
    EXPECT_EQ(reversePayload->triggerId, 4352u);
}

TEST(CampaignTravelCatalog, NeverSynthesizesReverseWhenReturnRecordIsAbsent)
{
    AreaTriggerRecord const forward = DarkPortalForward();
    CatalogInput input = Input(forward.destination, forward.source);
    input.areaTriggers = {forward};
    CatalogResult const result = BuildCatalog(input);
    EXPECT_EQ(result.issue, CatalogIssue::None);
    EXPECT_TRUE(result.executableLegs.empty());
    EXPECT_TRUE(result.request.candidates.empty());
}

TEST(CampaignTravelCatalog, ApproachesIngressBeforeOfferingTheTransition)
{
    AreaTriggerRecord const forward = DarkPortalForward();
    CatalogInput input = Input(At(0, -12000.0, -3300.0, -18.0), forward.destination);
    input.areaTriggers = {forward};
    CatalogResult result = BuildCatalog(input);
    ASSERT_EQ(result.issue, CatalogIssue::None);
    ASSERT_EQ(result.executableLegs.size(), 1u);
    EXPECT_EQ(result.executableLegs.front().policyFacts.kind, LegKind::WalkingApproach);
    auto const* payload = std::get_if<WalkPayload>(&result.executableLegs.front().payload);
    ASSERT_NE(payload, nullptr);
    EXPECT_TRUE(SameEndpoint(payload->endpoint, forward.source));
}

TEST(CampaignTravelCatalog, RejectsTaxiCheatInstantUnknownNodesAndInsufficientMoney)
{
    TaxiRecord const taxi = Taxi();
    CatalogInput input = Input(taxi.source, taxi.destination);
    input.taxis = {taxi};

    input.taxiCheater = true;
    EXPECT_EQ(BuildCatalog(input).issue, CatalogIssue::TaxiCheatEnabled);
    input.taxiCheater = false;
    input.instantTaxi = true;
    EXPECT_EQ(BuildCatalog(input).issue, CatalogIssue::InstantTaxiEnabled);
    input.instantTaxi = false;
    input.taxis.front().allNodesKnown = false;
    EXPECT_EQ(BuildCatalog(input).issue, CatalogIssue::UnknownTaxiNodes);
    input.taxis.front().allNodesKnown = true;
    input.botMoney = taxi.cost - 1;
    EXPECT_EQ(BuildCatalog(input).issue, CatalogIssue::InsufficientTaxiMoney);
}

TEST(CampaignTravelCatalog, RejectsStaticAndMalformedTransports)
{
    TransportRecord transport = Transport();
    CatalogInput input = Input(transport.boardingPoint, transport.destination);
    input.transports = {transport};

    input.transports.front().dynamicMotionTransport = false;
    EXPECT_EQ(BuildCatalog(input).issue, CatalogIssue::StaticTransport);
    input.transports.front().dynamicMotionTransport = true;
    input.transports.front().destinationStop = input.transports.front().sourceStop;
    EXPECT_EQ(BuildCatalog(input).issue, CatalogIssue::MalformedTransport);
}

TEST(CampaignTravelCatalog, RejectsDuplicateStableIdsAndPolicyPayloadMismatch)
{
    AreaTriggerRecord first = DarkPortalForward();
    AreaTriggerRecord duplicate = first;
    CatalogInput input = Input(first.source, first.destination);
    input.areaTriggers = {first, duplicate};
    EXPECT_EQ(BuildCatalog(input).issue, CatalogIssue::DuplicateStableId);

    CandidateLeg facts;
    facts.stableId = 1;
    facts.kind = LegKind::Taxi;
    facts.source = first.source;
    facts.destination = first.destination;
    EXPECT_FALSE(ValidateExecutableLeg({facts, AreaTriggerPayload{4354}}));
}
