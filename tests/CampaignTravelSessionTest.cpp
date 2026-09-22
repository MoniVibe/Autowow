/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "../src/AutoWow/CampaignTravelSession.h"

#include "gtest/gtest.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace AutoWowCampaignTravel;

Endpoint At(std::uint32_t map, double x, double y, double z)
{
    return {map, {x, y, z}, true};
}

CampaignTravelIntent Intent(std::uint64_t sequence, Endpoint destination)
{
    CampaignTravelIntent intent;
    intent.sequence = sequence;
    intent.purpose = Purpose::QuestAcquire;
    intent.questId = 2159;
    intent.targetEntry = 6780;
    intent.targetSpawnId = 100;
    intent.finalDestination = destination;
    intent.legTimeoutMs = 1000;
    return intent;
}

ExecutableLeg Walk(std::uint64_t id, Endpoint source, Endpoint destination)
{
    CandidateLeg facts;
    facts.stableId = id;
    facts.kind = LegKind::WalkingApproach;
    facts.source = source;
    facts.destination = destination;
    return {facts, WalkPayload{destination}};
}

ExecutableLeg Trigger(std::uint64_t id, std::uint32_t triggerId, Endpoint source,
                      Endpoint destination)
{
    CandidateLeg facts;
    facts.stableId = id;
    facts.kind = LegKind::AreaTriggerPortal;
    facts.source = source;
    facts.destination = destination;
    facts.normalTransitionValidated = true;
    facts.directedEdge = {true, source.mapId, source.coordinates,
                          destination.mapId, destination.coordinates};
    return {facts, AreaTriggerPayload{triggerId}};
}

CatalogResult Catalog(Endpoint current, std::vector<ExecutableLeg> legs)
{
    CatalogResult catalog;
    catalog.request.source = current;
    catalog.request.botMoney = 10000;
    catalog.executableLegs = std::move(legs);
    for (ExecutableLeg const& leg : catalog.executableLegs)
        catalog.request.candidates.push_back(leg.policyFacts);
    return catalog;
}

CampaignTravelObservation Seen(Endpoint position, bool flight = false)
{
    CampaignTravelObservation observation;
    observation.position = position;
    observation.inTaxiFlight = flight;
    return observation;
}

std::string Read(std::filesystem::path const& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}  // namespace

TEST(CampaignTravelSession, FreezesOneSelectedLegUntilItsObservationGateCompletes)
{
    Endpoint const source = At(0, 1.0, 2.0, 3.0);
    Endpoint const first = At(0, 10.0, 20.0, 30.0);
    Endpoint const final = At(0, 100.0, 200.0, 300.0);

    CampaignTravelSession session;
    session.Begin(Intent(1, final), 10);
    ASSERT_TRUE(session.Plan(Catalog(source, {Walk(20, source, first), Walk(30, source, final)}), 20));
    ASSERT_NE(session.GetSelectedLeg(), nullptr);
    EXPECT_EQ(session.GetSelectedLeg()->policyFacts.stableId, 20u);

    EXPECT_FALSE(session.Plan(Catalog(source, {Walk(10, source, final)}), 30));
    EXPECT_EQ(session.GetSelectedLeg()->policyFacts.stableId, 20u);

    ASSERT_TRUE(session.MarkExecutionIssued(20, 40));
    session.Observe(Seen(At(0, 5.0, 6.0, 7.0)), 50);
    EXPECT_EQ(session.GetState(), SessionState::AwaitingExpectedObservation);
    EXPECT_EQ(session.GetSelectedLeg()->policyFacts.stableId, 20u);

    session.Observe(Seen(first), 60);
    EXPECT_EQ(session.GetState(), SessionState::Planning);
    EXPECT_EQ(session.GetSelectedLeg(), nullptr);
}

TEST(CampaignTravelSession, PortalHandlerReturnDoesNotCompleteWithoutDestinationObservation)
{
    Endpoint const source = At(0, -11877.7, -3204.49, -18.49);
    Endpoint const destination = At(530, -248.149, 921.875, 84.3885);
    CampaignTravelSession session;
    session.Begin(Intent(2, destination), 100);
    ASSERT_TRUE(session.Plan(Catalog(source, {Trigger(4354, 4354, source, destination)}), 100));
    ASSERT_TRUE(session.MarkExecutionIssued(4354, 100));

    session.Observe(Seen(source), 110);
    EXPECT_EQ(session.GetState(), SessionState::AwaitingExpectedObservation);
    session.Observe(Seen(destination), 120);
    EXPECT_EQ(session.GetState(), SessionState::Arrived);
}

TEST(CampaignTravelSession, TimeoutAndUnexpectedMapFailClosed)
{
    Endpoint const source = At(0, 1.0, 2.0, 3.0);
    Endpoint const destination = At(530, 4.0, 5.0, 6.0);

    CampaignTravelSession timeout;
    CampaignTravelIntent shortIntent = Intent(3, destination);
    shortIntent.legTimeoutMs = 50;
    timeout.Begin(shortIntent, 0);
    ASSERT_TRUE(timeout.Plan(Catalog(source, {Trigger(1, 1, source, destination)}), 100));
    timeout.MarkExecutionIssued(1, 100);
    timeout.Observe(Seen(source), 151);
    EXPECT_EQ(timeout.GetState(), SessionState::Blocked);
    EXPECT_EQ(timeout.Snapshot().blocker, SessionBlocker::Timeout);

    CampaignTravelSession unexpected;
    unexpected.Begin(Intent(4, destination), 0);
    ASSERT_TRUE(unexpected.Plan(Catalog(source, {Trigger(2, 2, source, destination)}), 100));
    unexpected.MarkExecutionIssued(2, 100);
    unexpected.Observe(Seen(At(1, 7.0, 8.0, 9.0)), 110);
    EXPECT_EQ(unexpected.GetState(), SessionState::Blocked);
    EXPECT_EQ(unexpected.Snapshot().blocker, SessionBlocker::UnexpectedMap);
}

TEST(CampaignTravelSession, RejectsDuplicateIdentityAndPayloadMismatch)
{
    Endpoint const source = At(0, 1.0, 2.0, 3.0);
    Endpoint const destination = At(0, 4.0, 5.0, 6.0);

    CampaignTravelSession duplicate;
    duplicate.Begin(Intent(5, destination), 0);
    EXPECT_FALSE(duplicate.Plan(Catalog(source, {Walk(7, source, destination),
                                                  Walk(7, source, destination)}), 10));
    EXPECT_EQ(duplicate.Snapshot().blocker, SessionBlocker::PayloadMismatch);

    ExecutableLeg mismatch = Walk(8, source, destination);
    mismatch.payload = AreaTriggerPayload{123};
    CampaignTravelSession badPayload;
    badPayload.Begin(Intent(6, destination), 0);
    EXPECT_FALSE(badPayload.Plan(Catalog(source, {mismatch}), 10));
    EXPECT_EQ(badPayload.Snapshot().blocker, SessionBlocker::PayloadMismatch);
}

TEST(CampaignTravelMailbox, CopiesIntentAndSnapshotAcrossThreadLifetime)
{
    CampaignTravelMailbox mailbox;
    CampaignTravelIntent original = Intent(77, At(530, 1.0, 2.0, 3.0));
    std::atomic<bool> submitted{false};

    std::thread producer([&]
    {
        EXPECT_TRUE(mailbox.Submit(original));
        original.questId = 99999;
        original.finalDestination.coordinates.x = 99999.0;
        submitted.store(true);
    });
    producer.join();
    ASSERT_TRUE(submitted.load());

    std::optional<CampaignTravelIntent> consumed = mailbox.Consume();
    ASSERT_TRUE(consumed.has_value());
    EXPECT_EQ(consumed->questId, 2159u);
    EXPECT_DOUBLE_EQ(consumed->finalDestination.coordinates.x, 1.0);

    CampaignTravelSnapshot published;
    published.sequence = 77;
    published.state = SessionState::Planning;
    mailbox.Publish(published);
    published.state = SessionState::Blocked;
    EXPECT_EQ(mailbox.ReadSnapshot().state, SessionState::Planning);
}

TEST(CampaignTravelRuntimeSourceContract, CrossThreadSurfaceIsValueOnlyAndNewFilesForbidCheatPaths)
{
    std::filesystem::path const root = std::filesystem::path(__FILE__).parent_path().parent_path();
    std::string const playerAi = Read(root / "src/Bot/PlayerbotAI.h");
    std::string const playerAiSource = Read(root / "src/Bot/PlayerbotAI.cpp");
    std::string campaignSources = Read(root / "src/AutoWow/CampaignTravelAction.cpp");
    campaignSources += Read(root / "src/AutoWow/CampaignTravelSession.cpp");
    campaignSources += Read(root / "src/AutoWow/CampaignTravelCatalog.cpp");

    EXPECT_NE(playerAi.find("CampaignTravelIntent const& intent"), std::string::npos);
    EXPECT_NE(playerAi.find("CampaignTravelSnapshot GetCampaignTravelSnapshot() const"),
              std::string::npos);
    EXPECT_EQ(playerAi.find("Player* SubmitCampaignTravel"), std::string::npos);
    EXPECT_NE(playerAiSource.find("bot->IsAlive() && !bot->IsInCombat()"), std::string::npos);

    for (std::string const& forbidden : {
             "->TeleportTo(", "->NearTeleportTo(", "->AddPassenger(", "->SetTransport(",
             "SendLearnNewTaxiNode(", "ActivateTaxiPathTo(", "SetTaximaskNode(",
             "ModifyMoney(", "SetMoney(", "MoveRandom", "urand(", "frand("})
        EXPECT_EQ(campaignSources.find(forbidden), std::string::npos) << forbidden;
}
