#include "../src/AutoWow/AutoWowOracleGatherCraftEconomyPolicy.h"

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowOracleGatherCraftEconomy;

AutoWowOracleGatherCraftEconomyInput BaseInput()
{
    AutoWowOracleGatherCraftEconomyInput input;
    input.ownerGuid = 42;
    input.factVersion = 7;
    input.epoch = 3;
    input.tick = 100;
    input.mapId = 1;
    input.budget.goldAvailable = 100000;
    input.budget.maxSpend = 100000;
    return input;
}

void AddSkill(AutoWowOracleGatherCraftEconomyInput& input, Profession profession,
    std::uint32_t skill, bool learned = true)
{
    input.skills[input.skillCount++] = {profession, skill, 300, learned};
}

void AddMaterial(AutoWowOracleGatherCraftEconomyInput& input, MaterialFact const& material)
{
    input.materials[input.materialCount++] = material;
}

void AddLegalNode(AutoWowOracleGatherCraftEconomyInput& input, NodeId nodeId, ItemId itemId,
    std::uint16_t priority, bool legal = true)
{
    input.gatherNodes[input.gatherNodeCount++] = {
        nodeId, static_cast<std::uint32_t>(nodeId), 1, itemId, Profession::Mining, 100, 9000, priority, true, true};
    LegalMovementRouteFact route;
    route.nodeId = nodeId;
    route.mapId = 1;
    route.routeCost = static_cast<std::uint32_t>(nodeId % 10 + 1);
    route.waypointCount = 1;
    route.waypoints[0] = {1, static_cast<std::uint32_t>(nodeId)};
    route.pathCalculated = true;
    route.pathComplete = legal;
    route.usesOnlyLegalMovement = legal;
    route.endpointsLegal = legal;
    input.gatherRoutes[input.gatherRouteCount++] = route;
}

void AddMiningTool(AutoWowOracleGatherCraftEconomyInput& input)
{
    input.tools[input.toolCount++] = {9000, Profession::Mining, 1, true, true};
}

PlannedIntent const* FindIntent(AutoWowOracleGatherCraftEconomyPlan const& plan, IntentKind kind)
{
    for (std::size_t index = 0; index < plan.intentCount; ++index)
        if (plan.intents[index].kind == kind)
            return &plan.intents[index];
    return nullptr;
}

AutoWowOracle::WorldReadFrame OracleFrame(
    AutoWowOracleGatherCraftEconomyInput const& input, AutoWowOracle::SquadId squadId = 77)
{
    AutoWowOracle::WorldReadFrame frame;
    frame.version = input.factVersion;
    frame.tick = input.tick;
    frame.epoch = input.epoch;
    frame.scope = {AutoWowOracle::ScopeKind::PersistentCampaign, squadId, input.ownerGuid};
    frame.bot = {input.ownerGuid, input.botAlive, input.inCombat, input.mapId, 2};
    return frame;
}
}

TEST(AutoWowOracleGatherCraftEconomyPolicyTest, RetryBackoffIsBoundedAndDeterministic)
{
    PlannerBudget budget;
    budget.maxAttempts = 4;
    budget.initialBackoffTicks = 2;
    budget.maxBackoffTicks = 5;

    EXPECT_EQ(RetryForAttempt(budget, 0).backoffTicks, 2u);
    EXPECT_EQ(RetryForAttempt(budget, 1).backoffTicks, 4u);
    EXPECT_EQ(RetryForAttempt(budget, 2).backoffTicks, 5u);
    EXPECT_FALSE(RetryForAttempt(budget, 4).allowed);
    EXPECT_EQ(RetryForAttempt(budget, 2).backoffTicks, RetryForAttempt(budget, 2).backoffTicks);
}

TEST(AutoWowOracleGatherCraftEconomyPolicyTest, SelectsDeterministicLegalGatherRouteAndReservations)
{
    AutoWowOracleGatherCraftEconomyInput input = BaseInput();
    AddSkill(input, Profession::Mining, 125);
    AddMiningTool(input);
    AddMaterial(input, {200, 0, 0, 0, 0, 0, 0, 0, false, false, false});
    input.requests[0] = {200, 3, 40};
    input.requestCount = 1;
    AddLegalNode(input, 90, 200, 100, false);
    AddLegalNode(input, 80, 200, 50, true);

    AutoWowOracleGatherCraftEconomyInput const before = input;
    AutoWowOracleGatherCraftEconomyPlan const first = Plan(input);
    AutoWowOracleGatherCraftEconomyPlan const second = Plan(input);

    ASSERT_EQ(first.receipt.status, PlanStatus::Planned);
    ASSERT_EQ(first.intentCount, 2u);
    ASSERT_EQ(first.reservationCount, 2u);
    ASSERT_EQ(first.gatherNodesPlanned, 1u);
    EXPECT_EQ(first.intents[0].kind, IntentKind::GatherRoute);
    EXPECT_EQ(first.intents[0].targetGuid, 80u);
    EXPECT_EQ(first.intents[0].action, "move to travel target");
    EXPECT_EQ(first.intents[1].kind, IntentKind::GatherSource);
    EXPECT_EQ(first.intents[1].targetGuid, 80u);
    EXPECT_EQ(first.intents[1].action, "worker gather seek");
    EXPECT_EQ(first.reservations[0].kind, ReservationKind::Item);
    EXPECT_EQ(first.reservations[0].itemId, 200u);
    EXPECT_EQ(first.reservations[1].kind, ReservationKind::Node);
    EXPECT_EQ(first.reservations[1].nodeId, 80u);

    ASSERT_EQ(second.intentCount, first.intentCount);
    for (std::size_t index = 0; index < first.intentCount; ++index)
    {
        EXPECT_EQ(second.intents[index].kind, first.intents[index].kind);
        EXPECT_EQ(second.intents[index].targetGuid, first.intents[index].targetGuid);
        EXPECT_EQ(second.intents[index].itemId, first.intents[index].itemId);
        EXPECT_EQ(second.intents[index].retry.backoffTicks, first.intents[index].retry.backoffTicks);
    }
    EXPECT_EQ(input.reservations.nodeCount, before.reservations.nodeCount);
    EXPECT_EQ(input.reservations.itemCount, before.reservations.itemCount);
    EXPECT_EQ(input.gatherNodes[0].nodeId, before.gatherNodes[0].nodeId);

#if AUTOWOW_ORACLE_GCE_HAS_CONTRACT
    AutoWowOracle::OracleCandidate const candidate = ToOracleCandidate(first.intents[0]);
    EXPECT_EQ(candidate.domain, AutoWowOracle::Domain::Navigation);
    EXPECT_EQ(candidate.intent, AutoWowOracle::IntentCode::Navigate);
    EXPECT_EQ(candidate.operation, AutoWowOracle::OperationCode::GatherRoute);
    EXPECT_EQ(candidate.resource, AutoWowOracle::LeaseResource::Transition);
    EXPECT_EQ(candidate.priority, first.intents[0].priority);
    EXPECT_EQ(candidate.ttlTicks, first.intents[0].ttlTicks);
    EXPECT_EQ(candidate.targetGuid, first.intents[0].targetGuid);
    EXPECT_EQ(candidate.itemId, first.intents[0].itemId);
    EXPECT_EQ(candidate.action, first.intents[0].action);
    EXPECT_EQ(candidate.qualifier, first.intents[0].qualifier);
    EXPECT_FALSE(candidate.executorAvailable);
    EXPECT_TRUE(candidate.route.required);
    EXPECT_TRUE(candidate.route.pathComplete);
#endif
}

TEST(AutoWowOracleGatherCraftEconomyPolicyTest, EnforcesProfessionSkillToolAndLegalRouteFacts)
{
    AutoWowOracleGatherCraftEconomyInput input = BaseInput();
    AddSkill(input, Profession::Mining, 50);
    AddMaterial(input, {201, 0, 0, 0, 0, 0, 0, 0, false, false, false});
    input.requests[0] = {201, 1, 10};
    input.requestCount = 1;
    AddLegalNode(input, 81, 201, 1, true);
    input.gatherNodes[0].requiredSkill = 100;

    AutoWowOracleGatherCraftEconomyPlan plan = Plan(input);
    EXPECT_EQ(plan.receipt.status, PlanStatus::Blocked);
    EXPECT_EQ(plan.receipt.reason, PlanBlockReason::MissingSkill);

    input.skills[0].skill = 125;
    plan = Plan(input);
    EXPECT_EQ(plan.receipt.reason, PlanBlockReason::MissingTool);

    AddMiningTool(input);
    input.gatherRoutes[0].pathComplete = false;
    plan = Plan(input);
    EXPECT_EQ(plan.receipt.reason, PlanBlockReason::NoLegalMovementRoute);

    input.gatherRoutes[0].pathComplete = true;
    plan = Plan(input);
    EXPECT_EQ(plan.receipt.status, PlanStatus::Planned);
}

// AutoWow.Gather.AnySkill: a learned gathering skill below the node requirement no longer blocks the plan.
TEST(AutoWowOracleGatherCraftEconomyPolicyTest, AnyGatherSkillDropsOnlyTheRequirementGate)
{
    AutoWowOracleGatherCraftEconomyInput input = BaseInput();
    AddSkill(input, Profession::Mining, 4);
    AddMaterial(input, {201, 0, 0, 0, 0, 0, 0, 0, false, false, false});
    input.requests[0] = {201, 1, 10};
    input.requestCount = 1;
    AddLegalNode(input, 81, 201, 1, true);
    input.gatherNodes[0].requiredSkill = 125;
    AddMiningTool(input);
    EXPECT_EQ(Plan(input).receipt.reason, PlanBlockReason::MissingSkill);  // flag off: stock gate

    input.anyGatherSkill = true;
    EXPECT_EQ(Plan(input).receipt.status, PlanStatus::Planned);

    input.skills[0].learned = false;  // still needs the profession
    EXPECT_EQ(Plan(input).receipt.reason, PlanBlockReason::MissingSkill);
}

TEST(AutoWowOracleGatherCraftEconomyPolicyTest, BuildsBoundedCraftDependencyDagInTopologicalOrder)
{
    AutoWowOracleGatherCraftEconomyInput input = BaseInput();
    AddSkill(input, Profession::Alchemy, 300);

    RecipeFact intermediate;
    intermediate.recipeId = 20;
    intermediate.outputItemId = 800;
    intermediate.outputQuantity = 1;
    intermediate.profession = Profession::Alchemy;
    intermediate.requiredSkill = 100;
    intermediate.inputCount = 1;
    intermediate.inputs[0] = {600, 2};
    intermediate.known = true;
    input.recipes[input.recipeCount++] = intermediate;

    RecipeFact finalRecipe;
    finalRecipe.recipeId = 30;
    finalRecipe.outputItemId = 900;
    finalRecipe.outputQuantity = 1;
    finalRecipe.profession = Profession::Alchemy;
    finalRecipe.requiredSkill = 200;
    finalRecipe.inputCount = 2;
    finalRecipe.inputs[0] = {800, 2};
    finalRecipe.inputs[1] = {700, 3};
    finalRecipe.known = true;
    input.recipes[input.recipeCount++] = finalRecipe;

    DemandSelection const demand{true, 900, 2, 50, 0};
    DependencyDag const dag = BuildDependencyDag(input, demand);
    ASSERT_TRUE(dag.valid);
    ASSERT_EQ(dag.nodeCount, 2u);
    ASSERT_EQ(dag.topologicalCount, 2u);
    EXPECT_EQ(dag.nodes[dag.topologicalOrder[0]].outputItemId, 800u);
    EXPECT_EQ(dag.nodes[dag.topologicalOrder[1]].outputItemId, 900u);
    ASSERT_EQ(dag.sourceNeedCount, 2u);
    EXPECT_EQ(dag.sourceNeeds[0].itemId, 600u);
    EXPECT_EQ(dag.sourceNeeds[0].quantity, 8u);
    EXPECT_EQ(dag.sourceNeeds[1].itemId, 700u);
    EXPECT_EQ(dag.sourceNeeds[1].quantity, 6u);

    AddMaterial(input, {600, 0, 0, 0, 0, 0, 1, 0, true, false, false});
    AddMaterial(input, {700, 0, 0, 0, 0, 0, 1, 0, true, false, false});
    input.requests[0] = {900, 2, 50};
    input.requestCount = 1;
    input.access.vendorAvailable = true;
    input.access.vendorGuid = 505;
    AutoWowOracleGatherCraftEconomyPlan const craftPlan = Plan(input);
    ASSERT_EQ(craftPlan.receipt.status, PlanStatus::Planned);
    std::size_t craftIntentCount = 0;
    for (std::size_t index = 0; index < craftPlan.intentCount; ++index)
    {
        if (craftPlan.intents[index].kind != IntentKind::Craft)
            continue;
        ASSERT_LT(craftIntentCount, 2u);
        EXPECT_EQ(craftPlan.intents[index].itemId, craftIntentCount == 0 ? 800u : 900u);
        ++craftIntentCount;
    }
    EXPECT_EQ(craftIntentCount, 2u);

    AutoWowOracleGatherCraftEconomyInput cycle = BaseInput();
    AddSkill(cycle, Profession::Alchemy, 300);
    RecipeFact first;
    first.recipeId = 1;
    first.outputItemId = 1;
    first.outputQuantity = 1;
    first.profession = Profession::Alchemy;
    first.known = true;
    first.inputCount = 1;
    first.inputs[0] = {2, 1};
    cycle.recipes[cycle.recipeCount++] = first;
    RecipeFact second = first;
    second.recipeId = 2;
    second.outputItemId = 2;
    second.inputs[0] = {1, 1};
    cycle.recipes[cycle.recipeCount++] = second;
    DependencyDag const cyclicDag = BuildDependencyDag(cycle, {true, 1, 1, 1, 0});
    EXPECT_FALSE(cyclicDag.valid);
    EXPECT_EQ(cyclicDag.failure, PlanBlockReason::DependencyCycle);
}

TEST(AutoWowOracleGatherCraftEconomyPolicyTest, EmitsBankMailVendorAndAuctionAcquisitionIntents)
{
    AutoWowOracleGatherCraftEconomyInput bank = BaseInput();
    AddMaterial(bank, {300, 0, 3, 0, 0, 0, 0, 0, false, false, false});
    bank.requests[0] = {300, 3, 1};
    bank.requestCount = 1;
    bank.access.bankAvailable = true;
    bank.access.bankGuid = 501;
    AutoWowOracleGatherCraftEconomyPlan bankPlan = Plan(bank);
    ASSERT_EQ(bankPlan.receipt.status, PlanStatus::Planned);
    EXPECT_NE(FindIntent(bankPlan, IntentKind::BankWithdraw), nullptr);

    AutoWowOracleGatherCraftEconomyInput mail = BaseInput();
    AddMaterial(mail, {301, 0, 0, 2, 0, 0, 0, 0, false, false, false});
    mail.requests[0] = {301, 2, 1};
    mail.requestCount = 1;
    mail.access.mailboxAvailable = true;
    mail.access.mailboxGuid = 502;
    AutoWowOracleGatherCraftEconomyPlan mailPlan = Plan(mail);
    ASSERT_EQ(mailPlan.receipt.status, PlanStatus::Planned);
    EXPECT_NE(FindIntent(mailPlan, IntentKind::MailReceive), nullptr);

    AutoWowOracleGatherCraftEconomyInput vendor = BaseInput();
    AddMaterial(vendor, {302, 0, 0, 0, 0, 0, 5, 0, true, false, false});
    vendor.requests[0] = {302, 3, 1};
    vendor.requestCount = 1;
    vendor.access.vendorAvailable = true;
    vendor.access.vendorGuid = 503;
    vendor.budget.goldAvailable = 100;
    vendor.budget.goldReserve = 10;
    vendor.budget.maxSpend = 20;
    AutoWowOracleGatherCraftEconomyPlan vendorPlan = Plan(vendor);
    ASSERT_EQ(vendorPlan.receipt.status, PlanStatus::Planned);
    EXPECT_NE(FindIntent(vendorPlan, IntentKind::VendorBuy), nullptr);
    EXPECT_EQ(vendorPlan.projectedSpend, 15u);

    AutoWowOracleGatherCraftEconomyInput auction = BaseInput();
    AddMaterial(auction, {303, 0, 0, 0, 0, 0, 0, 6, false, true, true});
    auction.requests[0] = {303, 2, 1};
    auction.requestCount = 1;
    auction.access.auctionAvailable = true;
    auction.access.auctioneerGuid = 504;
    auction.budget.goldAvailable = 100;
    auction.budget.maxSpend = 20;
    AutoWowOracleGatherCraftEconomyPlan auctionPlan = Plan(auction);
    ASSERT_EQ(auctionPlan.receipt.status, PlanStatus::Planned);
    EXPECT_NE(FindIntent(auctionPlan, IntentKind::AuctionBuy), nullptr);
    EXPECT_EQ(auctionPlan.projectedSpend, 0u);
}

TEST(AutoWowOracleGatherCraftEconomyPolicyTest, EmitsDemandMailAndDeterministicDisposalIntents)
{
    AutoWowOracleGatherCraftEconomyInput mail = BaseInput();
    AddMaterial(mail, {400, 5, 0, 0, 0, 0, 0, 0, false, false, false});
    mail.requests[0] = {400, 1, 10};
    mail.requestCount = 1;
    mail.guildDemands[0] = {400, 2, 0, 5, 99, true};
    mail.guildDemandCount = 1;
    mail.access.mailboxAvailable = true;
    mail.access.mailboxGuid = 601;
    EXPECT_NE(FindIntent(Plan(mail), IntentKind::MailSend), nullptr);

    AutoWowOracleGatherCraftEconomyInput bank = BaseInput();
    AddMaterial(bank, {401, 5, 0, 0, 0, 0, 0, 0, false, false, false});
    bank.requests[0] = {401, 1, 10};
    bank.requestCount = 1;
    bank.access.bankAvailable = true;
    EXPECT_NE(FindIntent(Plan(bank), IntentKind::BankDeposit), nullptr);

    AutoWowOracleGatherCraftEconomyInput vendor = BaseInput();
    AddMaterial(vendor, {402, 5, 0, 0, 0, 0, 1, 0, true, false, true});
    vendor.requests[0] = {402, 1, 10};
    vendor.requestCount = 1;
    vendor.access.vendorAvailable = true;
    EXPECT_NE(FindIntent(Plan(vendor), IntentKind::VendorSell), nullptr);

    AutoWowOracleGatherCraftEconomyInput auction = BaseInput();
    AddMaterial(auction, {403, 5, 0, 0, 0, 0, 0, 2, false, true, true});
    auction.requests[0] = {403, 1, 10};
    auction.requestCount = 1;
    auction.access.auctionAvailable = true;
    EXPECT_NE(FindIntent(Plan(auction), IntentKind::AuctionSell), nullptr);
}

TEST(AutoWowOracleGatherCraftEconomyPolicyTest, RejectsConflictingNodeAndItemReservations)
{
    AutoWowOracleGatherCraftEconomyInput nodeConflict = BaseInput();
    AddSkill(nodeConflict, Profession::Mining, 125);
    AddMiningTool(nodeConflict);
    AddMaterial(nodeConflict, {500, 0, 0, 0, 0, 0, 0, 0, false, false, false});
    nodeConflict.requests[0] = {500, 1, 1};
    nodeConflict.requestCount = 1;
    AddLegalNode(nodeConflict, 700, 500, 1, true);
    nodeConflict.reservations.nodes[0] = {true, 700, 99, 110, 1};
    nodeConflict.reservations.nodeCount = 1;
    AutoWowOracleGatherCraftEconomyPlan const blockedNode = Plan(nodeConflict);
    EXPECT_EQ(blockedNode.receipt.status, PlanStatus::Blocked);
    EXPECT_EQ(blockedNode.receipt.reason, PlanBlockReason::NodeReservationConflict);
    EXPECT_EQ(blockedNode.intentCount, 0u);

    AutoWowOracleGatherCraftEconomyInput itemConflict = nodeConflict;
    itemConflict.reservations.nodes[0] = {};
    itemConflict.reservations.items[0] = {true, 500, 99, 1, 110, 1};
    itemConflict.reservations.itemCount = 1;
    AutoWowOracleGatherCraftEconomyPlan const blockedItem = Plan(itemConflict);
    EXPECT_EQ(blockedItem.receipt.status, PlanStatus::Blocked);
    EXPECT_EQ(blockedItem.receipt.reason, PlanBlockReason::ItemReservationConflict);
}

#if AUTOWOW_ORACLE_GCE_HAS_CONTRACT
TEST(AutoWowOracleGatherCraftEconomyPolicyTest, AdapterMapsSharedDomainsAndPreservesIntentFields)
{
    PlannedIntent route;
    route.kind = IntentKind::GatherRoute;
    route.domain = IntentDomain::Navigation;
    route.targetGuid = 80;
    route.nodeId = 80;
    route.nodeEntry = 12345;
    route.nodeMapId = 1;
    route.itemId = 200;
    route.priority = 91;
    route.ttlTicks = 12;
    route.action = NativeActionName(route.kind);
    route.qualifier = IntentQualifier(route.kind);
    route.route.nodeId = 80;
    route.route.mapId = 1;
    route.route.routeCost = 3;
    route.route.waypointCount = 1;
    route.route.waypoints[0] = {1, 80};
    route.route.pathCalculated = true;
    route.route.pathComplete = true;
    route.route.usesOnlyLegalMovement = true;
    route.route.endpointsLegal = true;

    AutoWowOracle::OracleCandidate const routeCandidate = ToOracleCandidate(route);
    EXPECT_EQ(routeCandidate.domain, AutoWowOracle::Domain::Navigation);
    EXPECT_EQ(routeCandidate.intent, AutoWowOracle::IntentCode::Navigate);
    EXPECT_EQ(routeCandidate.resource, AutoWowOracle::LeaseResource::Transition);

    PlannedIntent source = route;
    source.kind = IntentKind::GatherSource;
    source.domain = IntentDomain::Gathering;
    source.action = NativeActionName(source.kind);
    source.qualifier = IntentQualifier(source.kind);
    AutoWowOracle::OracleCandidate const sourceCandidate = ToOracleCandidate(source);
    EXPECT_EQ(sourceCandidate.domain, AutoWowOracle::Domain::Gathering);
    EXPECT_EQ(sourceCandidate.intent, AutoWowOracle::IntentCode::GatherSource);
    EXPECT_EQ(sourceCandidate.operation, AutoWowOracle::OperationCode::GatherSource);
    EXPECT_EQ(sourceCandidate.gather.goal, AutoWowOracle::GatherGoal::ObtainMaterial);
    EXPECT_EQ(sourceCandidate.resource, AutoWowOracle::LeaseResource::QuestGather);
    EXPECT_FALSE(sourceCandidate.executorAvailable);

    PlannedIntent craft = source;
    craft.kind = IntentKind::Craft;
    craft.domain = IntentDomain::CraftingEconomyItems;
    craft.action = NativeActionName(craft.kind);
    craft.qualifier = IntentQualifier(craft.kind);
    AutoWowOracle::OracleCandidate const craftCandidate = ToOracleCandidate(craft);
    EXPECT_EQ(craftCandidate.domain, AutoWowOracle::Domain::CraftingEconomyItems);
    EXPECT_EQ(craftCandidate.intent, AutoWowOracle::IntentCode::CraftEconomyItems);
    EXPECT_EQ(craftCandidate.resource, AutoWowOracle::LeaseResource::QuestGather);
    EXPECT_FALSE(craftCandidate.executorAvailable);

    PlannedIntent economy = craft;
    economy.kind = IntentKind::VendorBuy;
    economy.targetGuid = 505;
    economy.action = NativeActionName(economy.kind);
    economy.qualifier = IntentQualifier(economy.kind);
    AutoWowOracle::OracleCandidate const economyCandidate = ToOracleCandidate(economy);
    EXPECT_EQ(economyCandidate.resource, AutoWowOracle::LeaseResource::QuestGather);
    EXPECT_EQ(economyCandidate.targetGuid, economy.targetGuid);
    EXPECT_EQ(economyCandidate.itemId, economy.itemId);
    EXPECT_FALSE(economyCandidate.executorAvailable);

    EXPECT_EQ(routeCandidate.priority, route.priority);
    EXPECT_EQ(routeCandidate.ttlTicks, route.ttlTicks);
    EXPECT_EQ(routeCandidate.action, route.action);
    EXPECT_EQ(routeCandidate.qualifier, route.qualifier);
    EXPECT_TRUE(routeCandidate.requiresBotAlive);
    EXPECT_TRUE(routeCandidate.requiresSameMap);
}

TEST(AutoWowOracleGatherCraftEconomyPolicyTest, UnsupportedGatherRouteRemainsVisibleButUnleased)
{
    AutoWowOracleGatherCraftEconomyInput input = BaseInput();
    AddSkill(input, Profession::Mining, 125);
    AddMiningTool(input);
    AddMaterial(input, {200, 0, 0, 0, 0, 0, 0, 0, false, false, false});
    input.requests[0] = {200, 3, 40};
    input.requestCount = 1;
    AddLegalNode(input, 80, 200, 50, true);

    AutoWowOracleGatherCraftEconomyPlan const policy = Plan(input);
    ASSERT_EQ(policy.receipt.status, PlanStatus::Planned);
    AutoWowOracle::WorldReadFrame frame = OracleFrame(input);
    ASSERT_TRUE(AutoWowOracle::AddCandidate(frame, ToOracleCandidate(policy.intents[0])));
    AutoWowOracle::PlanResult const shared = AutoWowOracle::Plan(frame);
    EXPECT_FALSE(shared.hasDecision);
    EXPECT_EQ(shared.receipt.status, AutoWowOracle::ReceiptStatus::Blocked);
    EXPECT_EQ(shared.receipt.reason, AutoWowOracle::ReceiptReason::ExecutorBlocked);
    EXPECT_EQ(shared.receipt.operation, AutoWowOracle::OperationCode::GatherRoute);

    AutoWowOracle::OracleArbiter<> arbiter;
    AutoWowOracle::Decision const blockedDecision =
        AutoWowOracle::MakeDecision(frame, ToOracleCandidate(policy.intents[0]));
    EXPECT_FALSE(arbiter.Acquire(frame, blockedDecision).hasLease);
    EXPECT_EQ(arbiter.ActiveBotLeaseCount(), 0U);

    AutoWowOracle::WorldReadFrame otherFrame = OracleFrame(input);
    otherFrame.scope.botGuid = 43;
    otherFrame.bot.guid = 43;
    AutoWowOracle::OracleCandidate const otherCandidate = ToOracleCandidate(policy.intents[0]);
    AutoWowOracle::Decision const otherDecision = AutoWowOracle::MakeDecision(otherFrame, otherCandidate);
    EXPECT_FALSE(arbiter.Acquire(otherFrame, otherDecision).hasLease);
}

TEST(AutoWowOracleGatherCraftEconomyPolicyTest, UnsupportedCraftAndEconomyPlansBlockWithoutLeases)
{
    AutoWowOracleGatherCraftEconomyInput input = BaseInput();
    PlannedIntent templateIntent;
    templateIntent.domain = IntentDomain::CraftingEconomyItems;
    templateIntent.itemId = 200;
    templateIntent.targetGuid = 505;
    templateIntent.priority = 80;
    templateIntent.ttlTicks = 8;

    auto expectBlocked = [&](IntentKind kind)
    {
        PlannedIntent intent = templateIntent;
        intent.kind = kind;
        intent.action = NativeActionName(kind);
        intent.qualifier = IntentQualifier(kind);
        AutoWowOracle::OracleCandidate const candidate = ToOracleCandidate(intent);
        EXPECT_TRUE(candidate.available);
        EXPECT_FALSE(candidate.executorAvailable);

        AutoWowOracle::WorldReadFrame frame = OracleFrame(input);
        ASSERT_TRUE(AutoWowOracle::AddCandidate(frame, candidate));
        AutoWowOracle::PlanResult const plan = AutoWowOracle::Plan(frame);
        EXPECT_FALSE(plan.hasDecision);
        EXPECT_EQ(plan.receipt.status, AutoWowOracle::ReceiptStatus::Blocked);
        EXPECT_EQ(plan.receipt.reason, AutoWowOracle::ReceiptReason::ExecutorBlocked);
        EXPECT_EQ(plan.receipt.operation, candidate.operation);

        AutoWowOracle::Decision const decision = AutoWowOracle::MakeDecision(frame, candidate);
        ASSERT_TRUE(decision.valid);
        AutoWowOracle::OracleArbiter<> arbiter;
        AutoWowOracle::LeaseResult const lease = arbiter.Acquire(frame, decision);
        EXPECT_FALSE(lease.hasLease);
        EXPECT_EQ(lease.receipt.reason, AutoWowOracle::ReceiptReason::ExecutorBlocked);
    };

    expectBlocked(IntentKind::Craft);
    expectBlocked(IntentKind::BankWithdraw);
    expectBlocked(IntentKind::MailReceive);
    expectBlocked(IntentKind::VendorBuy);
}

TEST(AutoWowOracleGatherCraftEconomyPolicyTest, RejectsInvalidRouteAtAdapter)
{
    PlannedIntent invalid;
    invalid.kind = IntentKind::GatherRoute;
    invalid.action = NativeActionName(invalid.kind);
    invalid.qualifier = IntentQualifier(invalid.kind);
    invalid.route.nodeId = 700;
    invalid.nodeId = 700;
    invalid.nodeEntry = 12345;
    invalid.nodeMapId = 1;
    invalid.route.mapId = 1;
    invalid.route.pathCalculated = true;
    invalid.route.pathComplete = false;
    invalid.route.usesOnlyLegalMovement = true;
    invalid.route.endpointsLegal = true;
    AutoWowOracle::OracleCandidate candidate = ToOracleCandidate(invalid);
    EXPECT_TRUE(candidate.available);
    EXPECT_FALSE(candidate.executorAvailable);
    EXPECT_TRUE(TryToOracleCandidate(invalid, candidate));
}

TEST(AutoWowOracleGatherCraftEconomyPolicyTest, AuctionPathsRemainExplicitButExecutorBlocked)
{
    AutoWowOracleGatherCraftEconomyInput buy = BaseInput();
    AddMaterial(buy, {303, 0, 0, 0, 0, 0, 0, 6, false, true, true});
    buy.requests[0] = {303, 2, 1};
    buy.requestCount = 1;
    buy.access.auctionAvailable = true;
    buy.access.auctioneerGuid = 504;
    AutoWowOracleGatherCraftEconomyPlan const buyPlan = Plan(buy);
    ASSERT_EQ(buyPlan.receipt.status, PlanStatus::Planned);
    EXPECT_EQ(buyPlan.projectedSpend, 0u);
    PlannedIntent const* buyIntent = FindIntent(buyPlan, IntentKind::AuctionBuy);
    ASSERT_NE(buyIntent, nullptr);
    AutoWowOracle::OracleCandidate const buyCandidate = ToOracleCandidate(*buyIntent);
    EXPECT_TRUE(buyCandidate.available);
    EXPECT_FALSE(buyCandidate.executorAvailable);
    AutoWowOracle::WorldReadFrame buyFrame = OracleFrame(buy);
    ASSERT_TRUE(AutoWowOracle::AddCandidate(buyFrame, buyCandidate));
    AutoWowOracle::PlanResult const buyShared = AutoWowOracle::Plan(buyFrame);
    EXPECT_FALSE(buyShared.hasDecision);
    EXPECT_EQ(buyShared.receipt.status, AutoWowOracle::ReceiptStatus::Blocked);
    EXPECT_EQ(buyShared.receipt.reason, AutoWowOracle::ReceiptReason::ExecutorBlocked);
    AutoWowOracle::Decision const missingBuyDecision = AutoWowOracle::MakeDecision(buyFrame, buyCandidate);
    ASSERT_TRUE(missingBuyDecision.valid);
    EXPECT_EQ(AutoWowOracle::Detail::ValidateDecision(buyFrame, missingBuyDecision),
        AutoWowOracle::ReceiptReason::ExecutorBlocked);
    AutoWowOracle::OracleArbiter<> buyArbiter;
    EXPECT_FALSE(buyArbiter.Acquire(buyFrame, missingBuyDecision).hasLease);

    AutoWowOracleGatherCraftEconomyInput sell = BaseInput();
    AddMaterial(sell, {403, 5, 0, 0, 0, 0, 0, 2, false, true, true});
    sell.requests[0] = {403, 1, 10};
    sell.requestCount = 1;
    sell.access.auctionAvailable = true;
    sell.access.auctioneerGuid = 505;
    AutoWowOracleGatherCraftEconomyPlan const sellPlan = Plan(sell);
    ASSERT_EQ(sellPlan.receipt.status, PlanStatus::Planned);
    EXPECT_EQ(sellPlan.projectedSpend, 0u);
    PlannedIntent const* sellIntent = FindIntent(sellPlan, IntentKind::AuctionSell);
    ASSERT_NE(sellIntent, nullptr);
    AutoWowOracle::OracleCandidate const sellCandidate = ToOracleCandidate(*sellIntent);
    EXPECT_FALSE(sellCandidate.executorAvailable);
    AutoWowOracle::WorldReadFrame sellFrame = OracleFrame(sell);
    ASSERT_TRUE(AutoWowOracle::AddCandidate(sellFrame, sellCandidate));
    AutoWowOracle::PlanResult const sellShared = AutoWowOracle::Plan(sellFrame);
    EXPECT_FALSE(sellShared.hasDecision);
    EXPECT_EQ(sellShared.receipt.status, AutoWowOracle::ReceiptStatus::Blocked);
    EXPECT_EQ(sellShared.receipt.reason, AutoWowOracle::ReceiptReason::ExecutorBlocked);
}
#endif
