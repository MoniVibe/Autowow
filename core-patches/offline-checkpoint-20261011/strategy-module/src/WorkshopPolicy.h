/* AutoWoW original workshop scheduling proposals. GPL v2 or later. */
#ifndef AUTOWOW_WORKSHOP_POLICY_H
#define AUTOWOW_WORKSHOP_POLICY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace AutoWowStrategy
{
enum class WorkshopKind : std::uint8_t { Routine = 1, Masterwork = 2, Legendary = 3 };
enum class WorkshopResult : std::uint8_t { Refused, Paused, Advanced, ReadyToCraft, Completed };

struct WorkshopProfile
{
    std::uint32_t profileId = 0;
    std::uint32_t version = 0;
    std::uint32_t recipeId = 0;
    WorkshopKind kind = WorkshopKind::Routine;
    std::uint8_t stageCount = 0;
    std::array<std::uint64_t, 5> stageDurationMs{};
    bool timingCatalogueValidated = false;
};

struct WorkshopOrder
{
    std::uint64_t orderId = 0;
    std::uint64_t crafterGuid = 0;
    std::uint32_t recipeId = 0;
    std::uint32_t profileId = 0;
    std::uint32_t profileVersion = 0;
    WorkshopKind kind = WorkshopKind::Routine;
    std::uint32_t quantity = 0;
};

// Caller persists immutable order/profile terms and every accepted cursor with compare-and-swap revision.
struct WorkshopCursor
{
    std::uint64_t orderId = 0;
    std::uint64_t revision = 0;
    std::uint32_t completedPieces = 0;
    std::uint8_t stageIndex = 0;
    std::uint64_t stageWorkedMs = 0;
    std::uint64_t lastSliceId = 0;
};

struct WorkshopWorkFacts
{
    std::uint64_t expectedRevision = 0;
    std::uint64_t sliceId = 0;
    std::uint64_t observedSpanMs = 0;
    std::uint64_t activeWorkMs = 0;
    bool continuousObservation = false;
    bool sameClockSession = false;
    bool autonomousCustodyVerified = false;
    bool aliveSafeCityWorkshop = false;
    bool materialsReserved = false;
    bool skillRecipeAndStationVerified = false;
};

// Authoritative once-per-order/unit settlement, not a cast-start, elapsed timer or queued SQL acknowledgement.
struct WorkshopCraftReceipt
{
    std::uint64_t orderId = 0;
    std::uint64_t crafterGuid = 0;
    std::uint32_t recipeId = 0;
    std::uint32_t profileId = 0;
    std::uint32_t profileVersion = 0;
    std::uint64_t expectedRevision = 0;
    std::uint32_t unitOrdinal = 0;
    std::uint64_t nativeCraftEventId = 0;
    std::uint64_t outputItemGuid = 0;
    std::uint32_t outputCount = 0;
    bool confirmedSelfCraft = false;
    bool nativeReagentsConsumed = false;
    bool exactOutputVerified = false;
    bool durableSettlementConfirmed = false;
};

struct WorkshopDecision
{
    WorkshopResult result = WorkshopResult::Refused;
    WorkshopCursor cursor{};
};

inline bool ValidWorkshopTerms(WorkshopOrder const& order, WorkshopProfile const& profile)
{
    if (!order.orderId || !order.crafterGuid || !order.recipeId || !order.quantity || !profile.profileId ||
        !profile.version || !profile.timingCatalogueValidated || order.recipeId != profile.recipeId ||
        order.profileId != profile.profileId || order.profileVersion != profile.version || order.kind != profile.kind ||
        profile.stageCount == 0 || profile.stageCount > profile.stageDurationMs.size())
        return false;

    switch (profile.kind)
    {
        case WorkshopKind::Routine:
            if (profile.stageCount != 1)
                return false;
            break;
        case WorkshopKind::Masterwork:
            if (profile.stageCount < 2)
                return false;
            break;
        case WorkshopKind::Legendary:
            if (profile.stageCount != 5)
                return false;
            break;
        default:
            return false;
    }

    std::uint64_t totalMs = 0;
    for (std::size_t stage = 0; stage < profile.stageDurationMs.size(); ++stage)
    {
        auto const duration = profile.stageDurationMs[stage];
        if (stage < profile.stageCount)
        {
            if (!duration || duration > std::numeric_limits<std::uint64_t>::max() - totalMs)
                return false;
            totalMs += duration;
        }
        else if (duration)
            return false;
    }
    return true;
}

inline bool ValidWorkshopCursor(WorkshopOrder const& order, WorkshopProfile const& profile,
    WorkshopCursor const& cursor)
{
    if (!ValidWorkshopTerms(order, profile) || cursor.orderId != order.orderId || !cursor.revision ||
        cursor.completedPieces > order.quantity || cursor.stageIndex >= profile.stageCount)
        return false;
    if (cursor.completedPieces == order.quantity)
        return cursor.stageIndex == 0 && cursor.stageWorkedMs == 0;
    auto const duration = profile.stageDurationMs[cursor.stageIndex];
    return cursor.stageWorkedMs < duration ||
        (cursor.stageWorkedMs == duration && cursor.stageIndex + 1 == profile.stageCount);
}

inline WorkshopDecision BeginWorkshopOrder(WorkshopOrder const& order, WorkshopProfile const& profile,
    bool durableReservationAdmitted)
{
    if (!durableReservationAdmitted || !ValidWorkshopTerms(order, profile))
        return {};
    return {WorkshopResult::Advanced, {order.orderId, 1, 0, 0, 0, 0}};
}

// Pure proposal: no sleep, offline catch-up, timer output, native casts, skill grants or item mutations.
// Slice IDs are durable per-order monotonic identities, never reset on login. Excess time is not banked.
inline WorkshopDecision PlanWorkshopWork(WorkshopOrder const& order, WorkshopProfile const& profile,
    WorkshopCursor const& cursor, WorkshopWorkFacts const& facts)
{
    if (!ValidWorkshopCursor(order, profile, cursor))
        return {};
    if (cursor.completedPieces == order.quantity)
        return {WorkshopResult::Completed, cursor};
    if (cursor.stageIndex + 1 == profile.stageCount &&
        cursor.stageWorkedMs == profile.stageDurationMs[cursor.stageIndex])
        return {WorkshopResult::ReadyToCraft, cursor};
    if (cursor.revision == std::numeric_limits<std::uint64_t>::max() || facts.expectedRevision != cursor.revision ||
        !facts.sliceId || facts.sliceId <= cursor.lastSliceId || !facts.observedSpanMs ||
        facts.activeWorkMs > facts.observedSpanMs)
        return {};

    auto next = cursor;
    ++next.revision;
    next.lastSliceId = facts.sliceId;
    if (!facts.continuousObservation || !facts.sameClockSession || !facts.autonomousCustodyVerified ||
        !facts.aliveSafeCityWorkshop || !facts.materialsReserved || !facts.skillRecipeAndStationVerified ||
        !facts.activeWorkMs)
        return {WorkshopResult::Paused, next};

    auto const duration = profile.stageDurationMs[cursor.stageIndex];
    auto const remaining = duration - cursor.stageWorkedMs;
    next.stageWorkedMs += facts.activeWorkMs < remaining ? facts.activeWorkMs : remaining;
    if (next.stageWorkedMs != duration)
        return {WorkshopResult::Advanced, next};
    if (next.stageIndex + 1 == profile.stageCount)
        return {WorkshopResult::ReadyToCraft, next};
    ++next.stageIndex;
    next.stageWorkedMs = 0;
    return {WorkshopResult::Advanced, next};
}

// Receipt must belong to the same ready revision and next piece. A batch never settles as one multi-output cast.
// Adapter exclusively reserves the craft attempt before dispatch and reconciles uncertainty before redispatch.
inline WorkshopDecision PlanWorkshopCraft(WorkshopOrder const& order, WorkshopProfile const& profile,
    WorkshopCursor const& cursor, WorkshopCraftReceipt const& receipt)
{
    if (!ValidWorkshopCursor(order, profile, cursor) || cursor.completedPieces == order.quantity ||
        cursor.revision == std::numeric_limits<std::uint64_t>::max() ||
        cursor.stageIndex + 1 != profile.stageCount ||
        cursor.stageWorkedMs != profile.stageDurationMs[cursor.stageIndex] || receipt.orderId != order.orderId ||
        receipt.crafterGuid != order.crafterGuid || receipt.recipeId != order.recipeId ||
        receipt.profileId != order.profileId || receipt.profileVersion != order.profileVersion ||
        receipt.expectedRevision != cursor.revision || receipt.unitOrdinal != cursor.completedPieces + 1 ||
        !receipt.nativeCraftEventId || !receipt.outputItemGuid || receipt.outputCount != 1 ||
        !receipt.confirmedSelfCraft || !receipt.nativeReagentsConsumed || !receipt.exactOutputVerified ||
        !receipt.durableSettlementConfirmed)
        return {};

    auto next = cursor;
    ++next.revision;
    ++next.completedPieces;
    next.stageIndex = 0;
    next.stageWorkedMs = 0;
    return {next.completedPieces == order.quantity ? WorkshopResult::Completed : WorkshopResult::Advanced, next};
}
} // namespace AutoWowStrategy

#endif
