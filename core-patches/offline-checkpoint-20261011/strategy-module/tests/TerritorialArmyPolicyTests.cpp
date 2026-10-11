/* AutoWoW territorial militia regressions. GPL v2 or later. */
#ifdef NDEBUG
#error "Army tests require assertions"
#endif
#include "TerritorialArmyPolicy.h"
#include <cassert>
#include <iostream>
#include <limits>
using namespace AutoWowStrategy;
namespace
{
ArmyTerms Terms(bool horde = false)
{
    ArmyTerms t;
    t.squadId = horde ? 2 : 1;
    t.owner = {BankOwnerKind::Guild, horde ? SocietyFaction::Horde : SocietyFaction::Alliance, horde ? 20u : 10u};
    t.homeTerritory = horde ? 200 : 100;
    t.profileId = 77;
    t.profile = MilitiaProfile(1, 0, horde ? 0 : 2);
    t.weaponEntry = 1000; t.armorEntry = 1001; t.foodEntry = 1002;
    return t;
}
ArmyStock Stock(ArmyTerms const& t)
{
    return {{t.owner, {BankAssetKind::Item, t.weaponEntry}, 20, 3, 100},
            {t.owner, {BankAssetKind::Item, t.armorEntry}, 20, 3, 100},
            {t.owner, {BankAssetKind::Item, t.foodEntry}, 100, 7, 200}};
}
ArmyFacts Facts(ArmySquad const& s = {})
{
    return {s.revision, s.lastEventId + 1, true, true, true, true};
}
ArmySquad Squad(bool horde = false)
{
    auto t = Terms(horde);
    auto made = PlanArmyMuster(t, {}, Stock(t), Facts(), horde ? std::vector<std::uint64_t>{21, 22, 23} :
                                                                           std::vector<std::uint64_t>{11, 12, 13}, 6);
    assert(made.accepted);
    auto order = PlanArmyOrder(made.squad, Facts(made.squad), horde ? ArmyOrder::Defend : ArmyOrder::Attack, 200, 9);
    assert(order.accepted);
    return order.squad;
}
BattleTerms Battle(BattleMode mode = BattleMode::Strategic)
{
    return {900, 200, 1, mode, 100, 200};
}
BattleFacts BF(BattleSnapshot const& b = {})
{
    return {b.revision, b.lastEventId + 1, true, true, true, true, true};
}
void ConservedMusterAndSupplies()
{
    auto t = Terms();
    auto made = PlanArmyMuster(t, {}, Stock(t), Facts(), {11, 12, 13}, 6);
    assert(made.accepted && made.squad.rations == 6 && made.squad.troops.size() == 3);
    assert(made.stock.weapons.available == 17 && made.stock.armor.available == 17 && made.stock.food.available == 94);
    assert(made.stock.weapons.reserved == 3 && made.stock.food.reserved == 7);
    assert(made.squad.terms.profile.health == 140 && made.squad.terms.profile.attack == 16);
    auto supplied = PlanArmyResupply(made.squad, made.stock.food, Facts(made.squad), 4);
    assert(supplied.accepted && supplied.squad.rations == 10 && supplied.food.available == 90);
    assert(supplied.food.reserved == 7);
    assert(!PlanArmyMuster(t, {}, Stock(t), Facts(), {11, 11}, 6).accepted);
    auto f = Facts(); f.ownerAuthorized = false;
    assert(!PlanArmyMuster(t, {}, Stock(t), f, {11}, 6).accepted);
    auto wrong = Stock(t); wrong.food.owner = Terms(true).owner;
    assert(!PlanArmyMuster(t, {}, wrong, Facts(), {11}, 6).accepted);
    auto poor = Stock(t); poor.weapons.available = 0;
    assert(!PlanArmyMuster(t, {}, poor, Facts(), {11}, 6).accepted);
    assert(!PlanArmyMuster(t, {}, Stock(t), Facts(), std::vector<std::uint64_t>(11, 1), 11).accepted);
    t.profile = MilitiaProfile(0, 0, 0);
    assert(!PlanArmyMuster(t, {}, Stock(t), Facts(), {11}, 1).accepted);
}
void ModeLockAndBattleConservation()
{
    auto a = Squad(), d = Squad(true);
    auto start = PlanArmyBattleStart(Battle(), {}, a, d, Terms(true).owner, BF());
    assert(start.accepted && start.attacker.rations == 3 && start.defender.rations == 3);
    assert(start.attacker.activeBattleId == 900 && start.defender.activeBattleId == 900);
    assert(!PlanArmyOrder(start.attacker, Facts(start.attacker), ArmyOrder::Patrol, 100, 9).accepted);
    assert(!PlanArmyResupply(start.attacker, Stock(Terms()).food, Facts(start.attacker), 1).accepted);
    auto changedMode = Battle(BattleMode::Physical);
    assert(!PlanArmyBattleResolve(changedMode, start.battle, start.attacker, start.defender,
                                 Terms(true).owner, BF(start.battle), {}).accepted);
    auto resolved = PlanArmyBattleResolve(Battle(), start.battle, start.attacker, start.defender,
                                         Terms(true).owner, BF(start.battle), {});
    assert(resolved.accepted && resolved.battle.completed && !resolved.attacker.activeBattleId);
    assert(LivingTroops(resolved.attacker) > 0 && LivingTroops(resolved.defender) == 0);
    assert(SameOwner(resolved.territoryOwner, Terms().owner) && resolved.ownershipVersion == 2);
    assert(resolved.defender.troops.size() == 3 && resolved.defender.troops[0].health == 0);
    assert(resolved.attacker.troops[0].health < Terms().profile.health);
    assert(!PlanArmyBattleResolve(Battle(), resolved.battle, resolved.attacker, resolved.defender,
                                 resolved.territoryOwner, BF(resolved.battle), {}).accepted);
    auto stale = BF(start.battle); stale.expectedRevision = 0;
    assert(!PlanArmyBattleResolve(Battle(), start.battle, start.attacker, start.defender,
                                 Terms(true).owner, stale, {}).accepted);
    std::cout << "PILOT territory200: Alliance militia captured; survivors=" << LivingTroops(resolved.attacker)
              << ", Horde survivors=" << LivingTroops(resolved.defender) << "; food consumed=6\n";
}
void NativePhysicalProofAndDraw()
{
    auto a = Squad(), d = Squad(true);
    auto t = Battle(BattleMode::Physical);
    auto start = PlanArmyBattleStart(t, {}, a, d, Terms(true).owner, BF());
    assert(start.accepted);
    PhysicalBattleReceipt receipt;
    assert(!PlanArmyBattleResolve(t, start.battle, start.attacker, start.defender,
                                 Terms(true).owner, BF(start.battle), receipt).accepted);
    receipt.battleId = 900; receipt.exactRosterOutcomeVerified = true; receipt.uniqueOutcomeVerified = true;
    receipt.objectiveControlVerified = true;
    receipt.attacker = start.attacker.troops; receipt.defender = start.defender.troops;
    for (auto& unit : receipt.defender) unit.health = 0;
    auto outcome = PlanArmyBattleResolve(t, start.battle, start.attacker, start.defender,
                                        Terms(true).owner, BF(start.battle), receipt);
    assert(outcome.accepted && SameOwner(outcome.territoryOwner, a.terms.owner));
    receipt.attacker[0].id = 999;
    assert(!PlanArmyBattleResolve(t, start.battle, start.attacker, start.defender,
                                 Terms(true).owner, BF(start.battle), receipt).accepted);
    receipt.attacker = start.attacker.troops; receipt.attacker[0].health += 1;
    assert(!PlanArmyBattleResolve(t, start.battle, start.attacker, start.defender,
                                 Terms(true).owner, BF(start.battle), receipt).accepted);
    receipt.attacker = start.attacker.troops;
    for (auto& unit : receipt.attacker) unit.health = 0;
    outcome = PlanArmyBattleResolve(t, start.battle, start.attacker, start.defender,
                                   Terms(true).owner, BF(start.battle), receipt);
    assert(outcome.accepted && SameOwner(outcome.territoryOwner, d.terms.owner));
    auto b = Battle(); auto f = BF(); f.noPhysicalActorsVerified = false;
    assert(!PlanArmyBattleStart(b, {}, a, d, d.terms.owner, f).accepted);
    d.troops[0].id = a.troops[0].id;
    assert(!PlanArmyBattleStart(b, {}, a, d, d.terms.owner, BF()).accepted);
}
void CorruptRecoveryAndUnauthorizedOwnershipRefuse()
{
    auto a = Squad(), d = Squad(true);
    auto start = PlanArmyBattleStart(Battle(), {}, a, d, d.terms.owner, BF());
    assert(start.accepted);
    auto corrupt = start;
    corrupt.battle.terms.ownershipVersion = std::numeric_limits<std::uint64_t>::max();
    assert(!PlanArmyBattleResolve(corrupt.battle.terms, corrupt.battle, corrupt.attacker, corrupt.defender,
                                 d.terms.owner, BF(corrupt.battle), {}).accepted);
    corrupt = start;
    corrupt.attacker.revision = std::numeric_limits<std::uint64_t>::max();
    corrupt.battle.attacker = corrupt.attacker;
    assert(!PlanArmyBattleResolve(Battle(), corrupt.battle, corrupt.attacker, corrupt.defender,
                                 d.terms.owner, BF(corrupt.battle), {}).accepted);
    auto f = BF(); f.ownersAuthorized = false;
    assert(!PlanArmyBattleStart(Battle(), {}, a, d, d.terms.owner, f).accepted);
    f = BF(start.battle); f.eventId = start.battle.lastEventId;
    assert(!PlanArmyBattleResolve(Battle(), start.battle, start.attacker, start.defender,
                                 d.terms.owner, f, {}).accepted);
    assert(!PlanArmyBattleResolve(Battle(), start.battle, start.attacker, start.defender,
                                 a.terms.owner, BF(start.battle), {}).accepted);
    f = BF(start.battle); f.noPhysicalActorsVerified = false;
    assert(!PlanArmyBattleResolve(Battle(), start.battle, start.attacker, start.defender,
                                 d.terms.owner, f, {}).accepted);
    auto food = Stock(a.terms).food;
    a.rations = std::numeric_limits<std::uint64_t>::max();
    assert(!PlanArmyResupply(a, food, Facts(a), 1).accepted);
}
}
int main()
{
    ConservedMusterAndSupplies(); ModeLockAndBattleConservation(); NativePhysicalProofAndDraw();
    CorruptRecoveryAndUnauthorizedOwnershipRefuse();
    std::cout << "PASS militia: stock, orders, mode lock, attrition, casualties, native refusals, replay\n";
}
