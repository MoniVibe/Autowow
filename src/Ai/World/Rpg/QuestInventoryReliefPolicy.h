#ifndef PLAYERBOTS_QUEST_INVENTORY_RELIEF_POLICY_H
#define PLAYERBOTS_QUEST_INVENTORY_RELIEF_POLICY_H

#include <algorithm>
#include <cmath>
#include <vector>

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

// ---- Full-bag stall relief (AutoWow.QuestFullBagRelief.Enable, default off) -------------------
// Escalation for an incomplete collect-item quest blocked by 100% bag occupancy:
//   1. vendor relief (existing TryRelieveInventoryAtVendor: safe-gray sale / bounded bag buy)
//      for at most VendorReliefBudgetMs of one full-bag episode;
//   2. destroy the lowest-value safe junk (poor quality only) to free exactly the needed slots;
//   3. defer the quest, then skip re-evaluation for an exponential backoff (cap 60 s).
// All time math is uint32 getMSTime() arithmetic (wrap-safe unsigned difference).
constexpr uint32 VendorReliefBudgetMs = 2 * 60 * 1000;
constexpr uint32 FullBagEpisodeGapMs = 30 * 1000;
constexpr uint32 DeferBackoffBaseMs = 5 * 1000;
constexpr uint32 DeferBackoffCapMs = 60 * 1000;
constexpr uint32 NeededJunkSlots = 1;  // one incoming quest item needs one free slot
constexpr uint8 PoorQuality = 0;       // ITEM_QUALITY_POOR (static_assert at the call site)

struct FullBagRelief
{
    uint32 questId = 0;
    uint32 episodeStartMs = 0;  // first full-bag observation of the current episode
    uint32 lastSeenMs = 0;      // latest full-bag observation
    uint32 retryAtMs = 0;       // no re-evaluation before this (valid only when backoffArmed)
    bool backoffArmed = false;
    uint8 defers = 0;           // consecutive defers of this quest; drives the backoff exponent
};

enum class FullBagStep : uint8
{
    Backoff,     // inside the defer backoff: set the quest aside silently, no scans, no ledger row
    TryVendor,   // vendor window still open: run the existing vendor relief path
    Fallback     // vendor window spent (or vendor made no progress): destroy junk or defer
};

inline uint32 DeferBackoffMs(uint8 defers)
{
    uint32 const shift = defers < 4 ? defers : 4;  // 5, 10, 20, 40, then capped at 60 s
    uint32 const ms = DeferBackoffBaseMs << shift;
    return ms < DeferBackoffCapMs ? ms : DeferBackoffCapMs;
}

// Records one full-bag observation of questId at nowMs and returns the next step.
inline FullBagStep ObserveFullBag(FullBagRelief& s, uint32 questId, uint32 nowMs)
{
    if (s.questId != questId)
        s = FullBagRelief{questId, nowMs, nowMs, 0, false, 0};
    else if (nowMs - s.lastSeenMs > FullBagEpisodeGapMs)
        s.episodeStartMs = nowMs;  // new episode; the defer count (backoff exponent) is kept
    s.lastSeenMs = nowMs;

    if (s.backoffArmed)
    {
        if (static_cast<int32>(nowMs - s.retryAtMs) < 0)
            return FullBagStep::Backoff;
        s.backoffArmed = false;
        s.episodeStartMs = nowMs;  // backoff over: a fresh vendor window
    }
    return nowMs - s.episodeStartMs < VendorReliefBudgetMs ? FullBagStep::TryVendor : FullBagStep::Fallback;
}

// Junk was destroyed: the stall is resolved, forget the escalation.
inline void ResolveFullBag(FullBagRelief& s) { s = FullBagRelief{}; }

// Nothing could be freed: defer and arm the next backoff. Returns the backoff length.
inline uint32 DeferFullBag(FullBagRelief& s, uint32 nowMs)
{
    uint32 const backoff = DeferBackoffMs(s.defers);
    if (s.defers < 255)
        ++s.defers;
    s.retryAtMs = nowMs + backoff;
    s.backoffArmed = true;
    return backoff;
}

// One occupied backpack/bag slot, reduced to the facts the junk policy needs.
struct BagItemFacts
{
    uint8 bag = 0;              // INVENTORY_SLOT_BAG_0 or an equipped bag slot
    uint8 slot = 0;
    uint8 quality = 0;
    bool questRelated = false;  // quest class, quest-bound, starts a quest, or a quest objective item
    bool retainedClass = false; // trade goods / reagent / recipe (kept like CanSellGrayValue)
    bool usageSafe = false;     // item usage is none / vendor / AH
    uint32 value = 0;           // vendor sell price * stack count, copper
};

inline bool IsDestroyableJunk(BagItemFacts const& item)
{
    return item.quality == PoorQuality && !item.questRelated && !item.retainedClass && item.usageSafe;
}

// Lowest total value first; ties by (bag, slot) so the choice is deterministic.
inline std::vector<BagItemFacts> SelectJunkToDestroy(std::vector<BagItemFacts> const& items, uint32 neededSlots)
{
    std::vector<BagItemFacts> junk;
    for (BagItemFacts const& item : items)
        if (IsDestroyableJunk(item))
            junk.push_back(item);
    std::sort(junk.begin(), junk.end(), [](BagItemFacts const& a, BagItemFacts const& b)
    {
        if (a.value != b.value)
            return a.value < b.value;
        if (a.bag != b.bag)
            return a.bag < b.bag;
        return a.slot < b.slot;
    });
    if (junk.size() > neededSlots)
        junk.resize(neededSlots);
    return junk;
}
} // namespace QuestInventoryReliefPolicy

#endif
