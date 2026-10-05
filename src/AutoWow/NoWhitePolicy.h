/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_NO_WHITE_POLICY_H
#define AUTOWOW_NO_WHITE_POLICY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "GearUpgradePolicy.h"

class Player;

// AutoWow.Gear.NoWhite (default 0). Lane AY census (live S107b): cohort 62955-63004 (avg L67) main hands 2 white /
// grey, 41 green, 7 blue, avg ilvl 98 = 0.85 of the green curve, 11 bots > 25% behind, 4 > 50% (L71 on ilvl 47);
// squad (avg L40) 4 white / grey, avg ilvl 27 = 0.56 of the curve; four artisans on starter weapons. The Gear.Upgrades
// floor (FarBelow: 50% of the 0.7 x L vendor-WHITE DPS curve) lets whites pass and vendors sell whites only.
// With the flag on a weapon slot is under the floor when it is empty, grey / white (quality <= 1) or below FloorPct of
// the per-band GREEN item-level curve (GreenIlvl). Slots: the main hand; a hunter's ranged; a dual wielder's off hand
// when it holds a weapon. Bots: the cohort, the squad roster and the supply roles (artisans / reps) too. Sources, in
// order, for a slot under the floor:
//   (a) bag / bank weapons the bot owns that improve on the worn one (world pass, equipped at once);
//   (b) auction buyouts (the AuctionUpgrades errand path, weapons first, whites dropped; the treasury grant tops the
//       purse to reserve + one item cap with Supply.OutfitGear) - errand bots only, waited for AuctionWaitPasses;
//   (c) a hand-me-down weapon from a same-faction non-role member's loose bags (mailed, AutoWow gear flow subject);
//   (d) a weapon order to the Smiths house (WeaponOrderPolicy.h queue; the smith line consumes it);
//   (e) a quest choice reward that is a weapon clearing the floor wins the choice (BestRewardIndex);
//   (f) a vendor white only as a last resort: the worn weapon is missing / grey / starter-level (ilvl <= StarterIlvl)
//       and (a), (b) and (c) found nothing (logged).
// Logs "[NoWhite]"; ledger errand rows weapon_floor (a) / handdown_weapon (c) / smith_order (d), trade rows ah_weapon.
//
// Integer item levels / percents only, no floats, no RNG; members in ascending guid; candidate ties break on lower
// holder guid, then lower item guid.
namespace AutoWowNoWhite
{
inline constexpr std::uint8_t kPolicyVersion = 1;
inline constexpr std::uint8_t kQualityGreen = 2;  // ITEM_QUALITY_UNCOMMON

struct Params
{
    std::uint32_t floorPct = 100;          // AutoWow.Gear.NoWhiteFloorPct: % of GreenIlvl a weapon must reach
    std::uint32_t tickMs = 60000;          // AutoWow.Gear.NoWhiteTickMs: world pass period
    std::uint32_t auctionWaitPasses = 20;  // AutoWow.Gear.NoWhiteAuctionWaitPasses: passes an errand bot gets for (b)
    std::uint32_t retryPasses = 60;        // AutoWow.Gear.NoWhiteRetryPasses: passes before the sources start over
    std::uint32_t maxMails = 10;           // AutoWow.Gear.NoWhiteMaxMails: hand-me-down mails per pass
    std::uint32_t starterIlvl = 5;         // AutoWow.Gear.NoWhiteStarterIlvl: worn at / under this = starter-level
};

// Green item level by level band: L+5 to L57 (AH census: req 35 -> 40, 49 -> 54), 62 + 4.5 x (L - 58) to L70 (Outland
// greens), 130 + 6.3 x (L - 71) to L80 (Northrend greens). Integer: x 9 / 2 and x 63 / 10, truncated.
inline constexpr std::uint32_t kBand1Top = 57;
inline constexpr std::uint32_t kBand2Base = 62, kBand2From = 58, kBand2Num = 9, kBand2Den = 2, kBand2Top = 70;
inline constexpr std::uint32_t kBand3Base = 130, kBand3From = 71, kBand3Num = 63, kBand3Den = 10;

[[nodiscard]] inline std::uint32_t GreenIlvl(std::uint32_t level)
{
    if (level <= kBand1Top)
        return level + 5;
    if (level <= kBand2Top)
        return kBand2Base + kBand2Num * (level - kBand2From) / kBand2Den;
    return kBand3Base + kBand3Num * (level - kBand3From) / kBand3Den;
}

[[nodiscard]] inline std::uint32_t FloorIlvl(Params const& p, std::uint32_t level)
{
    return static_cast<std::uint32_t>(std::uint64_t(GreenIlvl(level)) * p.floorPct / 100);
}

// What a weapon slot holds (present = an item is there; weapon = it is ITEM_CLASS_WEAPON).
struct Worn
{
    bool present = false;
    bool weapon = false;
    std::uint8_t quality = 0;
    std::uint32_t ilvl = 0;
};

[[nodiscard]] inline bool Clears(Params const& p, std::uint32_t quality, std::uint32_t ilvl, std::uint32_t level)
{
    return quality >= kQualityGreen && ilvl >= FloorIlvl(p, level);
}

[[nodiscard]] inline bool BelowFloor(Params const& p, Worn const& w, std::uint32_t level)
{
    return !w.present || !Clears(p, w.quality, w.ilvl, level);
}

// A green+ candidate improves on the worn piece: the slot is empty, the worn one is grey / white, or it is higher.
[[nodiscard]] inline bool Improves(std::uint32_t quality, std::uint32_t ilvl, Worn const& w)
{
    return quality >= kQualityGreen && (!w.present || w.quality < kQualityGreen || ilvl > w.ilvl);
}

inline constexpr std::uint8_t kSlotMainHand = AutoWowGear::kSlotMainHand;  // 15
inline constexpr std::uint8_t kSlotOffHand = AutoWowGear::kSlotOffHand;    // 16
inline constexpr std::uint8_t kSlotRanged = AutoWowGear::kSlotRanged;      // 17

// Bit per weapon slot (1 << EQUIPMENT_SLOT_*) under the floor.
[[nodiscard]] inline std::uint32_t FloorSlots(Params const& p, std::uint32_t level, Worn const& mainHand,
                                              Worn const& offHand, Worn const& ranged, bool hunter, bool dualWield)
{
    std::uint32_t mask = 0;
    if (BelowFloor(p, mainHand, level))
        mask |= 1u << kSlotMainHand;
    if (dualWield && offHand.present && offHand.weapon && BelowFloor(p, offHand, level))
        mask |= 1u << kSlotOffHand;
    if (hunter && BelowFloor(p, ranged, level))
        mask |= 1u << kSlotRanged;
    return mask;
}

// INVTYPE_* (ItemTemplate.h) a weapon slot takes: main hand one-hand / main-hand / two-hand (a two-hander only with
// the off hand free), off hand one-hand / off-hand for a dual wielder, ranged bow / gun / crossbow / wand / thrown.
inline constexpr std::uint32_t kInvWeapon = 13, kInvRanged = 15, kInv2H = 17, kInvMainHand = 21, kInvOffHand = 22,
                               kInvThrown = 25, kInvRangedRight = 26;

[[nodiscard]] inline bool Fits(std::uint32_t invType, std::uint8_t slot, bool dualWield, bool offHandFree)
{
    switch (slot)
    {
        case kSlotMainHand:
            return invType == kInvWeapon || invType == kInvMainHand || (invType == kInv2H && offHandFree);
        case kSlotOffHand:
            return dualWield && (invType == kInvWeapon || invType == kInvOffHand);
        case kSlotRanged:
            return invType == kInvRanged || invType == kInvRangedRight || invType == kInvThrown;
        default:
            return false;
    }
}

// One weapon the bot could get for a slot (own bags / bank, or another member's loose bags).
struct Candidate
{
    std::uint32_t holder = 0;  // guid-low of the holder (the bot itself for (a))
    std::uint32_t itemGuid = 0;
    std::uint32_t item = 0;
    std::uint8_t quality = 0;
    std::uint32_t ilvl = 0;
};

inline constexpr std::size_t kNone = static_cast<std::size_t>(-1);

// The candidate that Improves on `worn`: clearing the floor first, then higher ilvl, higher quality, lower holder,
// lower item guid.
[[nodiscard]] inline std::size_t PickCandidate(Params const& p, std::uint32_t level, Worn const& worn,
                                               std::vector<Candidate> const& cands)
{
    std::size_t best = kNone;
    for (std::size_t i = 0; i < cands.size(); ++i)
    {
        Candidate const& c = cands[i];
        if (!Improves(c.quality, c.ilvl, worn))
            continue;
        if (best == kNone)
        {
            best = i;
            continue;
        }
        Candidate const& b = cands[best];
        bool const cc = Clears(p, c.quality, c.ilvl, level), bc = Clears(p, b.quality, b.ilvl, level);
        if (cc != bc)
        {
            if (cc)
                best = i;
            continue;
        }
        if (c.ilvl != b.ilvl)
        {
            if (c.ilvl > b.ilvl)
                best = i;
            continue;
        }
        if (c.quality != b.quality)
        {
            if (c.quality > b.quality)
                best = i;
            continue;
        }
        if (c.holder != b.holder ? c.holder < b.holder : c.itemGuid < b.itemGuid)
            best = i;
    }
    return best;
}

// ---- source order (per bot, world pass) --------------------------------------------------------------------------
enum Source : std::uint8_t
{
    SrcBag = 1u << 0,       // (a) tried
    SrcAuction = 1u << 1,   // (b) an auctioneer scan ran for this bot (errands)
    SrcHandDown = 1u << 2,  // (c) tried
    SrcSmith = 1u << 3,     // (d) order posted
};

enum class Step : std::uint8_t
{
    Bag,
    WaitAuction,
    HandDown,
    SmithOrder,
    Idle,  // every source tried: wait for the retry
};

struct Track
{
    std::uint32_t level = 0;   // the bot's level when the sources started
    std::uint32_t passes = 0;  // world passes since then
    std::uint8_t tried = 0;    // Source bits
};

// A new level or RetryPasses since the start begins the sources again; else the pass is counted.
inline void Advance(Params const& p, Track& t, std::uint32_t level)
{
    if (t.level != level || t.passes >= p.retryPasses)
        t = Track{level, 0, 0};
    ++t.passes;
}

// The next source in order; an errand bot gets AuctionWaitPasses for its auction run before the hand-me-downs.
[[nodiscard]] inline Step NextStep(Params const& p, Track const& t, bool errandBot)
{
    if (!(t.tried & SrcBag))
        return Step::Bag;
    if (errandBot && !(t.tried & SrcAuction) && t.passes < p.auctionWaitPasses)
        return Step::WaitAuction;
    if (!(t.tried & SrcHandDown))
        return Step::HandDown;
    if (!(t.tried & SrcSmith))
        return Step::SmithOrder;
    return Step::Idle;
}

// (f): a vendor white only for a missing / grey / starter-level weapon once own bags, the auction house (errand bots)
// and the hand-me-downs found nothing.
[[nodiscard]] inline bool VendorLastResort(Params const& p, Track const& t, bool errandBot, Worn const& w)
{
    bool const starter = !w.present || w.quality == 0 || w.ilvl <= p.starterIlvl;
    std::uint8_t const need = SrcBag | SrcHandDown | (errandBot ? SrcAuction : 0);
    return starter && (t.tried & need) == need;
}

// ---- (e) quest choice reward --------------------------------------------------------------------------------------
struct RewardChoice
{
    std::uint8_t slot = 0xFF;  // weapon slot it Fits (15 / 16 / 17), 0xFF = not a weapon the bot can wear
    bool usable = false;       // CanUseItem / RequiredLevel ok
    std::uint8_t quality = 0;
    std::uint32_t ilvl = 0;
};

// The choice that is a usable weapon clearing the floor and improving on what its slot wears (worn[slot - 15]):
// higher ilvl, then lower index. kNone = none (the stock choice stands).
[[nodiscard]] inline std::size_t PickFloorReward(Params const& p, std::uint32_t level,
                                                 std::vector<RewardChoice> const& choices, std::array<Worn, 3> const& worn)
{
    std::size_t best = kNone;
    for (std::size_t i = 0; i < choices.size(); ++i)
    {
        RewardChoice const& c = choices[i];
        if (!c.usable || c.slot < kSlotMainHand || c.slot > kSlotRanged || !Clears(p, c.quality, c.ilvl, level) ||
            !Improves(c.quality, c.ilvl, worn[c.slot - kSlotMainHand]))
            continue;
        if (best == kNone || c.ilvl > choices[best].ilvl)
            best = i;
    }
    return best;
}

// ---- (b) auction --------------------------------------------------------------------------------------------------
// Drops grey / white offers for the weapon slots (the AH never sells the bot a white weapon).
inline void DropWhiteWeapons(std::vector<AutoWowGear::AhOffer>& offers)
{
    std::vector<AutoWowGear::AhOffer> out;
    for (AutoWowGear::AhOffer const& o : offers)
        if (o.quality >= kQualityGreen || (o.slot != kSlotMainHand && o.slot != kSlotOffHand && o.slot != kSlotRanged))
            out.push_back(o);
    offers.swap(out);
}

// Copper the treasury grant tops an auction-bound floor bot up to: the gear reserve plus one auction item cap.
[[nodiscard]] inline std::uint64_t AhAllowanceCopper(AutoWowGear::AhParams const& ah, std::uint32_t level)
{
    return AutoWowGear::ReserveCopper(level) + AutoWowGear::AhItemCap(ah, level);
}

// ---- ledger ---------------------------------------------------------------------------------------------------------
// Trailing fields of the errand rows weapon_floor / handdown_weapon / smith_order (AutoWowQuestLedger.h).
[[nodiscard]] inline std::string LedgerFields(std::uint8_t slot, std::uint32_t item, std::uint32_t ilvl,
                                              std::uint32_t quality, std::uint32_t floorIlvl, std::uint32_t from,
                                              std::uint32_t oid)
{
    return ",\"slot\":" + std::to_string(slot) + ",\"item\":" + std::to_string(item) + ",\"ilvl\":" +
           std::to_string(ilvl) + ",\"quality\":" + std::to_string(quality) + ",\"floor\":" +
           std::to_string(floorIlvl) + ",\"from\":" + std::to_string(from) + ",\"oid\":" + std::to_string(oid);
}

// ---- runtime (flag + params read by AutoWowErrands::LoadConfig; AutoWowNoWhite.cpp) ---------------------------------
namespace detail
{
inline bool gEnabled = false;
inline Params gParams;
}  // namespace detail
inline bool Enabled() { return detail::gEnabled; }
inline Params const& Get() { return detail::gParams; }

Worn WornOf(Player* bot, std::uint8_t slot);
std::uint32_t FloorSlotsOf(Player* bot);  // FloorSlots for the bot (0 with the flag off)
void MarkAuctionTried(std::uint32_t guid);  // errands: an auctioneer scan ran for the bot
bool VendorWhiteAllowed(Player* bot, std::uint8_t slot);  // (f) for the bot's slot
void WorldUpdate(std::uint32_t diff);  // world thread, maps idle
}  // namespace AutoWowNoWhite

#endif  // AUTOWOW_NO_WHITE_POLICY_H
