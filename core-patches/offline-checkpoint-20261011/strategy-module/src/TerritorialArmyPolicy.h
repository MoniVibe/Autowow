/* AutoWoW territorial militia proposals. GPL v2 or later. */
#ifndef AUTOWOW_TERRITORIAL_ARMY_POLICY_H
#define AUTOWOW_TERRITORIAL_ARMY_POLICY_H

#include "BankPolicy.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace AutoWowStrategy
{
enum class ArmyOrder : std::uint8_t { Hold, Patrol, Defend, Attack, Escort };
enum class BattleMode : std::uint8_t { Strategic, Physical };

struct MilitiaStats
{
    std::uint32_t barracks = 0, armory = 0, equipmentGrade = 0;
    std::uint32_t capacity = 0, health = 0, attack = 0, armor = 0;
};

// Prototype campaign points, not native creature-template health or damage.
inline MilitiaStats MilitiaProfile(std::uint32_t barracks, std::uint32_t armory, std::uint32_t equipmentGrade)
{
    if (!barracks || barracks > 10 || armory > 5 || equipmentGrade > 3)
        return {};
    return {barracks, armory, equipmentGrade, 10 * barracks, 100 + 20 * equipmentGrade,
            10 + 3 * equipmentGrade + armory, 2 + equipmentGrade + armory};
}

inline bool SameMilitiaProfile(MilitiaStats const& a, MilitiaStats const& b)
{
    return a.barracks == b.barracks && a.armory == b.armory && a.equipmentGrade == b.equipmentGrade &&
           a.capacity == b.capacity && a.health == b.health && a.attack == b.attack && a.armor == b.armor;
}

struct ArmyTerms
{
    std::uint64_t squadId = 0;
    BankOwner owner;
    std::uint32_t homeTerritory = 0, profileId = 0;
    MilitiaStats profile;
    std::uint32_t weaponEntry = 0, armorEntry = 0, foodEntry = 0;
};

struct ArmyTroop
{
    std::uint64_t id = 0;
    std::uint32_t health = 0, experience = 0;
};

struct ArmySquad
{
    ArmyTerms terms;
    std::uint64_t revision = 0, lastEventId = 0, activeBattleId = 0, rations = 0;
    ArmyOrder order = ArmyOrder::Hold;
    std::uint32_t orderTarget = 0, routeId = 0, morale = 100;
    std::vector<ArmyTroop> troops;
};

struct ArmyStock { BankBalance weapons, armor, food; };

// Trusted current internal facts, never client assertions. IDs must be unique in the durable global troop ledger.
struct ArmyFacts
{
    std::uint64_t expectedRevision = 0, eventId = 0;
    bool ownerAuthorized = false, exactSnapshotVerified = false;
    bool profileAndStockVerified = false, uniqueTroopIdsVerified = false;
};

struct ArmyDecision { bool accepted = false; ArmySquad squad; ArmyStock stock; BankBalance food; };

inline bool ValidArmyTerms(ArmyTerms const& t)
{
    auto const p = MilitiaProfile(t.profile.barracks, t.profile.armory, t.profile.equipmentGrade);
    return t.squadId && ValidOwner(t.owner) && t.homeTerritory && t.profileId && p.capacity &&
           SameMilitiaProfile(t.profile, p) && t.weaponEntry && t.armorEntry && t.foodEntry &&
           t.weaponEntry != t.armorEntry && t.weaponEntry != t.foodEntry && t.armorEntry != t.foodEntry;
}

inline bool ValidArmyOrder(ArmyOrder order)
{
    return order == ArmyOrder::Hold || order == ArmyOrder::Patrol || order == ArmyOrder::Defend ||
           order == ArmyOrder::Attack || order == ArmyOrder::Escort;
}

inline bool ValidArmySquad(ArmySquad const& s)
{
    if (!ValidArmyTerms(s.terms) || !s.revision || !s.lastEventId || s.morale > 100 ||
        !ValidArmyOrder(s.order) || !s.orderTarget || s.troops.empty() || s.troops.size() > s.terms.profile.capacity)
        return false;
    std::uint64_t previous = 0;
    for (auto const& unit : s.troops)
    {
        if (unit.id <= previous || unit.health > s.terms.profile.health || unit.experience > 65535)
            return false;
        previous = unit.id;
    }
    return true;
}

inline std::uint32_t LivingTroops(ArmySquad const& s)
{
    return static_cast<std::uint32_t>(std::count_if(s.troops.begin(), s.troops.end(),
                                                 [](ArmyTroop const& u) { return u.health != 0; }));
}

inline bool ArmyAuthorized(ArmySquad const& s, ArmyFacts const& f)
{
    return ValidArmySquad(s) && f.ownerAuthorized && f.exactSnapshotVerified && f.profileAndStockVerified &&
           f.expectedRevision == s.revision && f.eventId > s.lastEventId &&
           s.revision != std::numeric_limits<std::uint64_t>::max() && !s.activeBattleId;
}

inline bool ArmyStockMatches(BankBalance const& b, ArmyTerms const& t, std::uint32_t entry)
{
    return ValidBalance(b) && SameOwner(b.owner, t.owner) && b.asset.kind == BankAssetKind::Item &&
           b.asset.itemEntry == entry;
}

inline ArmyDecision PlanArmyMuster(ArmyTerms const& t, ArmySquad const& empty, ArmyStock const& stock,
                                   ArmyFacts const& f, std::vector<std::uint64_t> ids, std::uint64_t rations)
{
    if (!ValidArmyTerms(t) || empty.terms.squadId || empty.revision || empty.lastEventId ||
        empty.activeBattleId || empty.rations || !empty.troops.empty() || !f.ownerAuthorized ||
        !f.exactSnapshotVerified || !f.profileAndStockVerified || !f.uniqueTroopIdsVerified ||
        f.expectedRevision || !f.eventId || ids.empty() || ids.size() > t.profile.capacity || rations < ids.size() ||
        !ArmyStockMatches(stock.weapons, t, t.weaponEntry) || !ArmyStockMatches(stock.armor, t, t.armorEntry) ||
        !ArmyStockMatches(stock.food, t, t.foodEntry) || stock.weapons.available < ids.size() ||
        stock.armor.available < ids.size() || stock.food.available < rations)
        return {};
    std::sort(ids.begin(), ids.end());
    if (!ids.front() || std::adjacent_find(ids.begin(), ids.end()) != ids.end())
        return {};
    ArmyDecision result;
    result.accepted = true;
    result.squad.terms = t;
    result.squad.revision = 1;
    result.squad.lastEventId = f.eventId;
    result.squad.rations = rations;
    result.squad.orderTarget = t.homeTerritory;
    for (auto id : ids) result.squad.troops.push_back({id, t.profile.health, 0});
    result.stock = stock;
    result.stock.weapons.available -= ids.size();
    result.stock.armor.available -= ids.size();
    result.stock.food.available -= rations;
    return result;
}

inline ArmyDecision PlanArmyOrder(ArmySquad const& s, ArmyFacts const& f, ArmyOrder order,
                                  std::uint32_t target, std::uint32_t route)
{
    if (!ArmyAuthorized(s, f) || !LivingTroops(s) || !ValidArmyOrder(order) || !target ||
        (order != ArmyOrder::Hold && !route) || !s.rations)
        return {};
    ArmyDecision result;
    result.accepted = true;
    result.squad = s;
    result.squad.order = order;
    result.squad.orderTarget = target;
    result.squad.routeId = route;
    ++result.squad.revision;
    result.squad.lastEventId = f.eventId;
    return result;
}

inline ArmyDecision PlanArmyResupply(ArmySquad const& s, BankBalance const& food, ArmyFacts const& f,
                                     std::uint64_t quantity)
{
    if (!ArmyAuthorized(s, f) || !LivingTroops(s) || !quantity ||
        !ArmyStockMatches(food, s.terms, s.terms.foodEntry) || food.available < quantity ||
        quantity > std::numeric_limits<std::uint64_t>::max() - s.rations)
        return {};
    ArmyDecision result;
    result.accepted = true;
    result.squad = s;
    result.food = food;
    result.squad.rations += quantity;
    result.food.available -= quantity;
    ++result.squad.revision;
    result.squad.lastEventId = f.eventId;
    return result;
}

struct BattleTerms
{
    std::uint64_t battleId = 0;
    std::uint32_t objectiveId = 0;
    std::uint64_t ownershipVersion = 0;
    BattleMode mode = BattleMode::Strategic;
    std::uint32_t attackerOrigin = 0, defenderOrigin = 0;
};

struct BattleSnapshot
{
    BattleTerms terms;
    ArmySquad attacker, defender;
    BankOwner originalOwner;
    std::uint64_t revision = 0, lastEventId = 0;
    bool completed = false;
};

struct BattleFacts
{
    std::uint64_t expectedRevision = 0, eventId = 0;
    bool ownersAuthorized = false, exactSnapshotAndOwnershipVerified = false, uniqueBattleClaimVerified = false;
    bool objectiveAndRouteVerified = false, noPhysicalActorsVerified = false;
};

struct PhysicalBattleReceipt
{
    std::uint64_t battleId = 0;
    bool exactRosterOutcomeVerified = false, uniqueOutcomeVerified = false, objectiveControlVerified = false;
    std::vector<ArmyTroop> attacker, defender;
};

struct ArmyBattleDecision
{
    bool accepted = false;
    BattleSnapshot battle;
    ArmySquad attacker, defender;
    BankOwner territoryOwner;
    std::uint64_t ownershipVersion = 0;
};

inline bool SameArmySquad(ArmySquad const& a, ArmySquad const& b)
{
    auto const& x = a.terms;
    auto const& y = b.terms;
    if (x.squadId != y.squadId || !SameOwner(x.owner, y.owner) || x.homeTerritory != y.homeTerritory ||
        x.profileId != y.profileId || !SameMilitiaProfile(x.profile, y.profile) || x.weaponEntry != y.weaponEntry ||
        x.armorEntry != y.armorEntry || x.foodEntry != y.foodEntry || a.revision != b.revision ||
        a.lastEventId != b.lastEventId || a.activeBattleId != b.activeBattleId || a.rations != b.rations ||
        a.order != b.order || a.orderTarget != b.orderTarget || a.routeId != b.routeId ||
        a.morale != b.morale || a.troops.size() != b.troops.size())
        return false;
    for (std::size_t i = 0; i < a.troops.size(); ++i)
        if (a.troops[i].id != b.troops[i].id || a.troops[i].health != b.troops[i].health ||
            a.troops[i].experience != b.troops[i].experience)
            return false;
    return true;
}

inline bool SameBattleTerms(BattleTerms const& a, BattleTerms const& b)
{
    return a.battleId == b.battleId && a.objectiveId == b.objectiveId && a.ownershipVersion == b.ownershipVersion &&
           a.mode == b.mode && a.attackerOrigin == b.attackerOrigin && a.defenderOrigin == b.defenderOrigin;
}

inline bool ValidBattleTerms(BattleTerms const& t)
{
    return t.battleId && t.objectiveId && t.ownershipVersion &&
           t.ownershipVersion != std::numeric_limits<std::uint64_t>::max() &&
           t.attackerOrigin && t.defenderOrigin && t.attackerOrigin != t.defenderOrigin &&
           (t.mode == BattleMode::Strategic || t.mode == BattleMode::Physical);
}

inline bool BattleAuthorized(BattleFacts const& f)
{
    return f.eventId && f.ownersAuthorized && f.exactSnapshotAndOwnershipVerified &&
           f.uniqueBattleClaimVerified && f.objectiveAndRouteVerified;
}

inline ArmyBattleDecision PlanArmyBattleStart(BattleTerms const& t, BattleSnapshot const& empty,
                                             ArmySquad const& a, ArmySquad const& d,
                                             BankOwner const& currentOwner, BattleFacts const& f)
{
    if (!ValidBattleTerms(t) ||
        empty.terms.battleId || empty.revision || empty.lastEventId || empty.completed || f.expectedRevision ||
        !BattleAuthorized(f) || !ValidArmySquad(a) || !ValidArmySquad(d) || a.activeBattleId || d.activeBattleId ||
        a.terms.squadId == d.terms.squadId || a.terms.owner.faction == d.terms.owner.faction ||
        !SameOwner(currentOwner, d.terms.owner) || a.order != ArmyOrder::Attack || d.order != ArmyOrder::Defend ||
        a.orderTarget != t.objectiveId || d.orderTarget != t.objectiveId || !a.routeId || !d.routeId ||
        !LivingTroops(a) || !LivingTroops(d) || a.rations < LivingTroops(a) || d.rations < LivingTroops(d) ||
        a.revision >= std::numeric_limits<std::uint64_t>::max() - 1 ||
        d.revision >= std::numeric_limits<std::uint64_t>::max() - 1 ||
        (t.mode == BattleMode::Strategic && !f.noPhysicalActorsVerified))
        return {};
    for (auto const& x : a.troops)
        for (auto const& y : d.troops)
            if (x.id == y.id) return {};
    ArmyBattleDecision result;
    result.accepted = true;
    result.attacker = a;
    result.defender = d;
    for (auto* squad : {&result.attacker, &result.defender})
    {
        squad->rations -= LivingTroops(*squad);
        squad->activeBattleId = t.battleId;
        ++squad->revision;
        // Squad and battle event sequences are independent; captured squad sequence is retained.
    }
    result.battle = {t, result.attacker, result.defender, currentOwner, 1, f.eventId, false};
    result.territoryOwner = currentOwner;
    result.ownershipVersion = t.ownershipVersion;
    return result;
}

inline void ApplyCampaignDamage(ArmySquad& s, std::uint64_t damage)
{
    for (auto& unit : s.troops)
    {
        auto const lost = std::min<std::uint64_t>(unit.health, damage);
        unit.health -= static_cast<std::uint32_t>(lost);
        damage -= lost;
    }
}

inline void ResolveStrategicBattle(ArmySquad& a, ArmySquad& d)
{
    for (std::uint32_t round = 0; round < 128 && LivingTroops(a) && LivingTroops(d); ++round)
    {
        auto const attackA = std::uint64_t(LivingTroops(a)) * a.terms.profile.attack;
        auto const attackD = std::uint64_t(LivingTroops(d)) * d.terms.profile.attack;
        auto const armorA = std::uint64_t(LivingTroops(a)) * a.terms.profile.armor;
        auto const armorD = std::uint64_t(LivingTroops(d)) * d.terms.profile.armor;
        auto const damageD = attackA > armorD ? attackA - armorD : 1;
        auto const damageA = attackD > armorA ? attackD - armorA : 1;
        ApplyCampaignDamage(a, damageA);
        ApplyCampaignDamage(d, damageD);
    }
}

inline bool ValidPhysicalTroops(std::vector<ArmyTroop> const& after, ArmySquad const& before)
{
    if (after.size() != before.troops.size()) return false;
    for (std::size_t i = 0; i < after.size(); ++i)
        if (after[i].id != before.troops[i].id || after[i].health > before.troops[i].health ||
            after[i].experience != before.troops[i].experience)
            return false;
    return true;
}

inline ArmyBattleDecision PlanArmyBattleResolve(BattleTerms const& t, BattleSnapshot const& b,
                                               ArmySquad const& a, ArmySquad const& d, BankOwner const& owner,
                                               BattleFacts const& f, PhysicalBattleReceipt const& physical)
{
    if (!ValidBattleTerms(t) || !BattleAuthorized(f) || !b.revision || b.completed ||
        f.expectedRevision != b.revision ||
        f.eventId <= b.lastEventId || b.revision == std::numeric_limits<std::uint64_t>::max() ||
        !SameBattleTerms(t, b.terms) || !SameOwner(owner, b.originalOwner) ||
        !ValidArmySquad(a) || !ValidArmySquad(d) || !SameArmySquad(a, b.attacker) ||
        !SameArmySquad(d, b.defender) || a.activeBattleId != t.battleId || d.activeBattleId != t.battleId ||
        a.revision == std::numeric_limits<std::uint64_t>::max() ||
        d.revision == std::numeric_limits<std::uint64_t>::max())
        return {};
    ArmyBattleDecision result;
    result.attacker = a;
    result.defender = d;
    if (t.mode == BattleMode::Strategic)
    {
        if (!f.noPhysicalActorsVerified) return {};
        ResolveStrategicBattle(result.attacker, result.defender);
    }
    else if (t.mode == BattleMode::Physical)
    {
        if (physical.battleId != t.battleId || !physical.exactRosterOutcomeVerified ||
            !physical.uniqueOutcomeVerified || !physical.objectiveControlVerified ||
            !ValidPhysicalTroops(physical.attacker, a) || !ValidPhysicalTroops(physical.defender, d))
            return {};
        result.attacker.troops = physical.attacker;
        result.defender.troops = physical.defender;
    }
    else return {};
    bool const captured = LivingTroops(result.attacker) && !LivingTroops(result.defender);
    auto finish = [](ArmySquad& s, ArmySquad const& original, bool won)
    {
        auto const losses = LivingTroops(original) - LivingTroops(s);
        auto const penalty = 10 * losses;
        s.morale = penalty >= s.morale ? 0 : s.morale - penalty;
        if (won) s.morale = std::min(100u, s.morale + 5);
        for (auto& troop : s.troops)
            if (troop.health && troop.experience < 65535) ++troop.experience;
        s.activeBattleId = 0;
        s.order = ArmyOrder::Hold;
        s.routeId = 0;
        ++s.revision;
    };
    finish(result.attacker, a, captured);
    finish(result.defender, d, !captured && LivingTroops(result.defender));
    result.accepted = true;
    result.battle = b;
    result.battle.completed = true;
    ++result.battle.revision;
    result.battle.lastEventId = f.eventId;
    result.battle.attacker = result.attacker;
    result.battle.defender = result.defender;
    result.territoryOwner = captured ? a.terms.owner : owner;
    result.ownershipVersion = t.ownershipVersion + (captured ? 1 : 0);
    return result;
}
}
#endif
