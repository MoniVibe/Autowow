/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "CombatTelemetry.h"

#include "gtest/gtest.h"

namespace
{
using AutoWowCombatTelemetry::Snapshot;
using AutoWowCombatTelemetry::ThreatLink;
using AutoWowCombatTelemetry::UnitView;

UnitView MakeUnitView(std::uint32_t guid, char const* name, bool player, bool alive)
{
    UnitView view;
    view.present = true;
    view.guid = guid;
    view.entry = player ? 0 : 36561;
    view.name = name;
    view.isPlayer = player;
    view.alive = alive;
    view.health = alive ? 900 : 0;
    view.maxHealth = 1000;
    view.healthPct = alive ? 90.0f : 0.0f;
    return view;
}
}

TEST(CombatTelemetryContract, V0ContainsTankHealerAndResourceWitnessFields)
{
    Snapshot snapshot;
    snapshot.guid = 42;
    snapshot.name = "Tank \"One\"";
    snapshot.alive = true;
    snapshot.deathState = "alive";
    snapshot.inCombat = true;
    snapshot.mapId = 574;
    snapshot.instanceId = 91;
    snapshot.health = 900;
    snapshot.maxHealth = 1000;
    snapshot.healthPct = 90.0f;
    snapshot.mana = 300;
    snapshot.maxMana = 600;
    snapshot.manaPct = 50.0f;
    snapshot.powerType = 0;
    snapshot.power = 300;
    snapshot.maxPower = 600;
    snapshot.roleMask = 3;
    snapshot.roleTank = true;
    snapshot.roleHealer = true;
    snapshot.mainTank = true;
    snapshot.groupTanks = 1;
    snapshot.groupMembers = 5;
    snapshot.victim = MakeUnitView(9001, "Training Dummy", false, true);
    snapshot.healerTarget = MakeUnitView(43, "Injured Party Member", true, true);
    snapshot.healerIntent = "target_selected";
    snapshot.threatenedByMeCount = 1;
    snapshot.ownersTargetingBot = 1;

    ThreatLink link;
    link.source = MakeUnitView(9001, "Training Dummy", false, true);
    link.victim = MakeUnitView(42, "Tank \"One\"", true, true);
    link.threat = 1234.5f;
    link.sourceTargetsBot = true;
    snapshot.threatLinks.push_back(link);

    std::string const json = AutoWowCombatTelemetry::Serialize(snapshot);

    EXPECT_NE(json.find("\"ok\":true"), std::string::npos);
    EXPECT_NE(json.find("\"schema\":\"autowow.combat-telemetry.v0\""), std::string::npos);
    EXPECT_NE(json.find("\"version\":0"), std::string::npos);
    EXPECT_NE(json.find("\"location\":{\"map_id\":574,\"instance_id\":91}"), std::string::npos);
    EXPECT_NE(json.find("\"name\":\"Tank \\\"One\\\"\""), std::string::npos);
    EXPECT_NE(json.find("\"role\":{\"mask\":3,\"tank\":true,\"healer\":true"), std::string::npos);
    EXPECT_NE(json.find("\"mana\":{\"current\":300,\"max\":600,\"pct\":50"), std::string::npos);
    EXPECT_NE(json.find("\"healer\":{\"target\":{\"guid\":43"), std::string::npos);
    EXPECT_NE(json.find("\"victim\":{\"guid\":9001,\"entry\":36561"), std::string::npos);
    EXPECT_NE(json.find("\"intent\":\"target_selected\""), std::string::npos);
    EXPECT_NE(json.find("\"threatened_by_me\":1,\"owners_targeting_bot\":1"), std::string::npos);
    EXPECT_NE(json.find("\"source_targets_bot\":true"), std::string::npos);
}

TEST(CombatTelemetryContract, V0MakesMissingTargetsAndV1CounterStateExplicit)
{
    Snapshot snapshot;
    std::string const json = AutoWowCombatTelemetry::Serialize(snapshot);

    EXPECT_NE(json.find("\"victim\":null"), std::string::npos);
    EXPECT_NE(json.find("\"ai_target\":null"), std::string::npos);
    EXPECT_NE(json.find("\"healer\":{\"target\":null,\"intent\":\"not_healer\"}"), std::string::npos);
    EXPECT_NE(json.find("\"links\":[]"), std::string::npos);
    EXPECT_NE(json.find("\"counters\":{\"available\":true,\"schema\":\"autowow.combat-counters.v1\""), std::string::npos);
    EXPECT_NE(json.find("\"tracked\":false,\"active\":false"), std::string::npos);
    EXPECT_NE(json.find("\"window_ms\":0"), std::string::npos);
    EXPECT_NE(json.find("\"overhealing\":{\"available\":false"), std::string::npos);
    EXPECT_NE(json.find("server combat event hooks active"), std::string::npos);
}

TEST(CombatTelemetryContract, V1SerializesContributionCounters)
{
    Snapshot snapshot;
    snapshot.guid = 42;
    snapshot.counters.tracked = true;
    snapshot.counters.active = true;
    snapshot.counters.windowStartUnixMs = 1000;
    snapshot.counters.windowDurationMs = 2500;
    snapshot.counters.damageDone = 12000;
    snapshot.counters.effectiveHealing = 3400;
    snapshot.counters.damageTaken = 5600;
    snapshot.counters.deaths = 1;
    snapshot.counters.combatEntries = 2;
    snapshot.counters.combatExits = 1;
    snapshot.counters.threatSamples = 3;
    snapshot.counters.maxThreatenedByMe = 4;
    snapshot.counters.maxOwnersTargetingBot = 2;
    snapshot.counters.maxThreat = 987.5f;

    std::string const json = AutoWowCombatTelemetry::Serialize(snapshot);

    EXPECT_NE(json.find("\"damage_done\":12000"), std::string::npos);
    EXPECT_NE(json.find("\"effective_healing\":3400"), std::string::npos);
    EXPECT_NE(json.find("\"damage_taken\":5600"), std::string::npos);
    EXPECT_NE(json.find("\"deaths\":1"), std::string::npos);
    EXPECT_NE(json.find("\"entries\":2,\"exits\":1"), std::string::npos);
    EXPECT_NE(json.find("\"max_threatened_by_me\":4"), std::string::npos);
    EXPECT_NE(json.find("\"max_owners_targeting_bot\":2"), std::string::npos);
}
