/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "../src/Ai/Base/Actions/DungeonDeathRecoveryPolicy.h"

#include "gtest/gtest.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
std::string ReadRepopActionBody()
{
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path() /
                        "src/Ai/Base/Actions/ReleaseSpiritAction.cpp");
    std::string const source{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
    std::size_t const start = source.find("bool RepopAction::Execute");
    std::size_t const end = source.find("bool RepopAction::isUseful", start);
    if (start == std::string::npos || end == std::string::npos)
        return {};
    return source.substr(start, end - start);
}
}

TEST(DungeonDeathRecoveryPolicy, WaitsForAliveHealerInSameDungeonInstance)
{
    EXPECT_TRUE(DungeonDeathRecovery::ShouldWaitForHealer(true, true, true));
}

TEST(DungeonDeathRecoveryPolicy, ReleasesWhenRecoveryTeamIsUnavailable)
{
    EXPECT_FALSE(DungeonDeathRecovery::ShouldWaitForHealer(false, true, true));
    EXPECT_FALSE(DungeonDeathRecovery::ShouldWaitForHealer(true, false, true));
    EXPECT_FALSE(DungeonDeathRecovery::ShouldWaitForHealer(true, true, false));
}

TEST(DungeonDeathRecoveryPolicy, OrdinaryWorldDeathsNeverEnterInstanceRecovery)
{
    using namespace DungeonDeathRecovery;
    ReleasedRecoveryFacts facts;
    facts.grouped = true;
    facts.releasedGhost = true;
    facts.ingressPortalAvailable = true;

    EXPECT_EQ(EvaluateReleasedRecovery(facts), ReleasedRecoveryStep::OrdinaryDeath);
    facts.corpseInDungeonOrRaid = true;
    facts.grouped = false;
    EXPECT_EQ(EvaluateReleasedRecovery(facts), ReleasedRecoveryStep::OrdinaryDeath);
    facts.grouped = true;
    facts.releasedGhost = false;
    EXPECT_EQ(EvaluateReleasedRecovery(facts), ReleasedRecoveryStep::OrdinaryDeath);
}

TEST(DungeonDeathRecoveryPolicy, WalksToEntranceThenUsesNormalAreaTrigger)
{
    using namespace DungeonDeathRecovery;
    ReleasedRecoveryFacts facts;
    facts.grouped = true;
    facts.releasedGhost = true;
    facts.corpseInDungeonOrRaid = true;
    facts.ingressPortalAvailable = true;

    EXPECT_EQ(EvaluateReleasedRecovery(facts), ReleasedRecoveryStep::ApproachEntrance);
    facts.insideIngressPortal = true;
    EXPECT_EQ(EvaluateReleasedRecovery(facts), ReleasedRecoveryStep::ActivateEntrance);
}

TEST(DungeonDeathRecoveryPolicy, MovementTransferAndBackoffDoNotSpendMoreAttempts)
{
    using namespace DungeonDeathRecovery;
    ReleasedRecoveryFacts facts;
    facts.grouped = true;
    facts.releasedGhost = true;
    facts.corpseInDungeonOrRaid = true;
    facts.ingressPortalAvailable = true;
    facts.entranceAttempts = MaxEntranceAttempts - 1;

    facts.movementOrTransferInProgress = true;
    EXPECT_EQ(EvaluateReleasedRecovery(facts), ReleasedRecoveryStep::EntranceInProgress);
    facts.movementOrTransferInProgress = false;
    facts.retryBackoffActive = true;
    EXPECT_EQ(EvaluateReleasedRecovery(facts), ReleasedRecoveryStep::RetryBackoff);
}

TEST(DungeonDeathRecoveryPolicy, ReenteringCorpseMapHandsBackToNormalCorpseReclaim)
{
    using namespace DungeonDeathRecovery;
    ReleasedRecoveryFacts facts;
    facts.grouped = true;
    facts.releasedGhost = true;
    facts.corpseInDungeonOrRaid = true;
    facts.onCorpseMap = true;
    facts.entranceAttempts = MaxEntranceAttempts;
    facts.elapsedSeconds = MaxCorpseRunSeconds;

    EXPECT_EQ(EvaluateReleasedRecovery(facts), ReleasedRecoveryStep::ResumeCorpseApproach);
}

TEST(DungeonDeathRecoveryPolicy, MissingEntranceAndBudgetsFailClosed)
{
    using namespace DungeonDeathRecovery;
    ReleasedRecoveryFacts facts;
    facts.grouped = true;
    facts.releasedGhost = true;
    facts.corpseInDungeonOrRaid = true;

    EXPECT_EQ(EvaluateReleasedRecovery(facts), ReleasedRecoveryStep::MissingEntrance);

    facts.ingressPortalAvailable = true;
    facts.entranceAttempts = MaxEntranceAttempts;
    EXPECT_EQ(EvaluateReleasedRecovery(facts), ReleasedRecoveryStep::Exhausted);

    facts.entranceAttempts = 0;
    facts.elapsedSeconds = MaxCorpseRunSeconds;
    EXPECT_EQ(EvaluateReleasedRecovery(facts), ReleasedRecoveryStep::Exhausted);
}

TEST(DungeonDeathRecoveryPolicy, DtkExteriorReentryExhaustsOnlyAfterFourBoundedAttempts)
{
    using namespace DungeonDeathRecovery;
    ReleasedRecoveryFacts facts;
    facts.grouped = true;
    facts.releasedGhost = true;
    facts.corpseInDungeonOrRaid = true;
    facts.ingressPortalAvailable = true;

    for (std::uint8_t attempt = 0; attempt < MaxEntranceAttempts; ++attempt)
    {
        facts.entranceAttempts = attempt;
        EXPECT_EQ(EvaluateReleasedRecovery(facts), ReleasedRecoveryStep::ApproachEntrance);
    }

    facts.entranceAttempts = MaxEntranceAttempts;
    EXPECT_EQ(EvaluateReleasedRecovery(facts), ReleasedRecoveryStep::Exhausted);
}

TEST(DungeonDeathRecoveryPolicy, RuntimeUsesPersistentCorpseLocationAndNormalMovementProtocol)
{
    std::string const body = ReadRepopActionBody();
    ASSERT_FALSE(body.empty());

    EXPECT_NE(body.find("GetCorpseLocation().GetMapId()"), std::string::npos);
    EXPECT_NE(body.find("MoveTo(bot->GetMapId()"), std::string::npos);
    EXPECT_NE(body.find("DoSpecificAction(\"area trigger\""), std::string::npos);
    EXPECT_NE(body.find("last area trigger"), std::string::npos);
    EXPECT_NE(body.find("phase=exhausted"), std::string::npos);
    EXPECT_EQ(body.find("TeleportTo"), std::string::npos);
    EXPECT_EQ(body.find("ResurrectPlayer"), std::string::npos);
    EXPECT_EQ(body.find("CharacterDatabase"), std::string::npos);
}
