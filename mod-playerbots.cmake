# Phase 1 objective-lock unit tests — module test registration hook.
#
# modules/CMakeLists.txt include()s this file (OPTIONAL) during configure, before src/test is added,
# so appending to these GLOBAL properties is the sanctioned, non-invasive way to register module
# tests into the parent AzerothCore `unit_tests` target.
#
# Guarded by BUILD_TESTING: inert in the live-server build (BUILD_TESTING=OFF). Only a separate
# test-enabled build tree (-DBUILD_TESTING=ON, WSL/GCC) compiles these.
if (BUILD_TESTING)
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tests/QuestObjectiveResolutionTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/AutoWowQuestLedgerTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/ExactQuestAttackRecoveryPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/QuestFinisherTransitionPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/QuestFinisherPartyContractTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/QuestPartyParticipantPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/QuestFinisherBehaviorPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/QuestSpellFocusObjectiveTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/QuestPartyCohesionPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/SharedValueContextLifetimeTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/EscortQuestContractTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/CombatTelemetryContractTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/CombatPerformanceTelemetryTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/TacticalPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/TacticalPriestContractTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/TacticalClassContractTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/DeathLoopBreakerTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/ZoneProgressionPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/PullLevelCapTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/RestGateTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/TransportCrossingPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/ErrandsPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/ExactBossTargetingTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/FixtureFactoryContractTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/FixtureAccelerationControlTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/RaidLootEquipTelemetryTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/RaidFixtureControlTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/WarsongFixtureControlTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/ProbeResetPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/MageCounterspellStrategyContractTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/PartyMemberToHealPriorityTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/CriticalMemberRecoveryPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/ResurrectionTargetPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/ReleasedCorpseApproachPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/LineOfSightWaypointPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/PartyControlContractTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/ExactPartyRepairPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/PersistentRosterRecoveryTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/QuestAcquisitionPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/EncounterRoleTriggerPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/RaidInterruptResponsePolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/RaidInterruptAssignmentPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/RaidPriorityAddPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/RaidAddWavePolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/RaidTargetClaimPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/RaidSafeZoneSelectionPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/OnyxiaBreathSignalTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/AvoidAoeStrategyPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/WorkerGatherSeekTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tests/AdvanceFormationTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/ObserverControlPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/DungeonPathSafetyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/DungeonEncounterActivationPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/DungeonEncounterCompletionPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/DungeonEncounterSelectionPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/DungeonRouteReconnectPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/DungeonPullReadinessPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/DungeonDeathRecoveryPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/DTKNovosPolicyTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/DungeonTransitionPolicyTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/ShadowCombatOraclePolicyTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/ProbePlacePolicyTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tests/WorkerGatherSafetyPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/WorkerGatherDeathRecoveryPolicyTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tests/QuestSourceRotationPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/QuestStallRecoveryPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/QuestSchedulerPolicyTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/TravelIntentPolicyTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/WalkingV2PolicyTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/SurvivalRecoveryTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/FlightTrapPolicyTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/QuestGiverTravelPolicyTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/QuestGiverTravelLifecycleTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/CampaignTravelPolicyTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tests/CampaignTravelSessionTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/tests/CampaignTravelCatalogTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/EncounterTelemetryTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/DungeonNavigatorAmbientPolicyTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/DungeonProgressionInteractionPolicyTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/DungeonSpellCreditBossPolicyTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/CorpseRouteRetryPolicyTest.cpp")
 set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/PersistentCorpseApproachPolicyTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/AutoWowCohortPolicyTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/AutoWowTrainPolicyTest.cpp")
 set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/PlayerbotCommandServerContractTest.cpp")
 set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
   "${CMAKE_CURRENT_LIST_DIR}/tests/BossApproachPolicyTest.cpp"
   "${CMAKE_CURRENT_LIST_DIR}/tests/BossApproachAggroPolicyTest.cpp")
 set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tests/AutoWowOracleContractTest.cpp"
   "${CMAKE_CURRENT_LIST_DIR}/tests/AutoWowOraclePvpOpenWorldPolicyTest.cpp"
   "${CMAKE_CURRENT_LIST_DIR}/tests/AutoWowOraclePvpOpenWorldAdapterTest.cpp"
   "${CMAKE_CURRENT_LIST_DIR}/tests/AutoWowOracleQuestTravelPolicyTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tests/AutoWowOracleGatherCraftEconomyPolicyTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tests/AutoWowProfessionEconomyTelemetryTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tests/AutoWowProfessionReconciliationTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tests/AutoWowGuildTradeControlTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/AutoWow/AutoWowGuildTradeControl.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/OracleExecutorTypesTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/OracleQuestExecutorTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/AutoWowOracleRuntimePolicyTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/OracleGatherRuntimePolicyTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/tests/AutoWowOracleOwnershipGateTest.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tests/OracleGatherExecutorTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/AutoWow/OracleGatherExecutor.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tests/OraclePvpCombatExecutorTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/AutoWow/OraclePvpCombatExecutor.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tests/OracleCraftExecutorTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/AutoWow/OracleCraftExecutor.cpp")
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tests/OracleTravelExecutorTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/AutoWow/OracleTravelExecutor.cpp")
 set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_INCLUDES
    "${CMAKE_CURRENT_LIST_DIR}/src/Ai/Base"
    "${CMAKE_CURRENT_LIST_DIR}/src/Ai/World/Rpg"
    "${CMAKE_CURRENT_LIST_DIR}/src/Ai/Base/Value"
    "${CMAKE_CURRENT_LIST_DIR}/src/Bot/Factory"
    "${CMAKE_CURRENT_LIST_DIR}/src/Ai/World/Gathering"
    "${CMAKE_CURRENT_LIST_DIR}/src/Ai/Raid/Generic"
    "${CMAKE_CURRENT_LIST_DIR}/src/Ai/Dungeon/Generic"
    "${CMAKE_CURRENT_LIST_DIR}/src/AutoWow")
  # The real shared-value regression test pulls Playerbots' Tempest Keep RTTI references, matching
  # worldserver's existing modules -> scripts link dependency.
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_LIBRARIES scripts)
endif()

# GCC 13 in the WSL validation/runtime toolchain has intermittently crashed inside optimizer passes
# while compiling otherwise valid Playerbots translation units (the same sources compile under
# MSVC). Keep the core RelWithDebInfo build, but compile this large module at O0 for the stable proof
# profile. Later GCC/Clang toolchains may remove this narrow workaround after a clean optimized proof.
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
  target_compile_options(modules PRIVATE -O0)
endif()
