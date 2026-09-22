#ifndef PLAYERBOTS_QUEST_INVENTORY_RELIEF_POLICY_H
#define PLAYERBOTS_QUEST_INVENTORY_RELIEF_POLICY_H

#include <cmath>

#include "QuestObjectiveContext.h"

namespace QuestInventoryReliefPolicy
{
constexpr uint32 MinBagSlots = 6;
constexpr uint32 MaxBagPriceCopper = 500;
constexpr uint32 MoneyReserveCopper = 500;
constexpr uint32 BagPurchaseCooldownMs = 10 * 60 * 1000;

// Match Player::BuyItemFromVendorSlot: price is uint32, discount is float, and
// the multiplication is rounded as float before floor is applied.
inline uint32 DiscountedBagPrice(uint32 basePriceCopper, float reputationDiscount)
{
    return static_cast<uint32>(std::floor(basePriceCopper * reputationDiscount));
}

// Only a live, incomplete item collection objective can interrupt its source route
// for capacity recovery. A full bag must be addressed before another kill/drop.
inline bool ShouldRelieveIncompleteItem(uint32 questId, QuestObjectiveSpec const& objective,
                                       uint8 bagSpaceUsedPercent, bool alive, bool inCombat)
{
    return objective.hasLock() && objective.key.questId == questId &&
           objective.kind == QuestObjectiveKind::CollectItem &&
           bagSpaceUsedPercent >= 100 && alive && !inCombat;
}

inline bool AcceptVendorSpawn(uint32 actorMap, uint32 actorZone, uint32 actorPhaseMask,
                              uint32 spawnMap, uint32 spawnZone, uint32 spawnPhaseMask,
                              bool vendorFlag, bool stocked, bool friendly)
{
    return actorMap == spawnMap && actorZone == spawnZone &&
           (actorPhaseMask & spawnPhaseMask) != 0 && vendorFlag && stocked && friendly;
}

inline bool ShouldTryBagPurchase(bool incompleteItem, bool alive, bool inCombat,
                                 uint8 bagSpaceUsedPercent, bool safeGrayAvailable,
                                 bool emptyBagEquipSlot, bool inCooldown)
{
    return incompleteItem && alive && !inCombat && bagSpaceUsedPercent >= 100 &&
           !safeGrayAvailable && emptyBagEquipSlot && !inCooldown;
}

inline bool AcceptBagOffer(bool ordinaryContainer, uint32 containerSlots, uint32 buyCount,
                           uint32 extendedCost, bool stockAvailable, uint32 basePriceCopper,
                           uint32 discountedPriceCopper, uint32 moneyCopper)
{
    return ordinaryContainer && containerSlots >= MinBagSlots && buyCount == 1 &&
           extendedCost == 0 && stockAvailable && basePriceCopper > 0 &&
           basePriceCopper <= MaxBagPriceCopper && discountedPriceCopper > 0 &&
           discountedPriceCopper <= MaxBagPriceCopper &&
           moneyCopper >= discountedPriceCopper + MoneyReserveCopper;
}
} // namespace QuestInventoryReliefPolicy

#endif
