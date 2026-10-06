/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_AH_BROKER_POLICY_H
#define AUTOWOW_AH_BROKER_POLICY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "GearUpgradePolicy.h"

// AutoWow.Gear.AhBroker (default 0; needs AutoWow.Trade.Enable + AutoWow.Gear.AuctionUpgrades + AutoWow.Supply.Enable
// for the house reps). Live soak S124 (Gear.CatchUp on, 30 min): 451 [AhGear] run_due, 427 run_drop -- 399
// no_reachable_auction_town (392 on map 571 Northrend), 29 town_lacks_auctioneer (Shattrath). Only 1 scan. Northrend
// and Outland have no faction AH, so a L61-80 bot stranded there can NEVER reach an auctioneer to buy the greens the
// seeder lists; cohort L60+ mean equipped ilvl 88.8 vs real green ~133-175, deaths ~100/h.
//
// The broker turns a dropped AH-gear run into a mail order. No free anything -- real gold, real items:
//   1. a bot whose AhGear run is due but has no reachable auction town (the run_drop cases) files ONE open broker
//      request (requester guid, faction, level, hunter, the AhGear budget already computed). Once per level, via the
//      existing lastAhGearLevel token (a completed file consumes it, exactly like a completed scan), so it does not
//      re-file every check window.
//   2. a broker (an existing house rep, one per faction, resident at its capital's auctioneer) drains the queue on the
//      world thread when it is at/near its auctioneer: it scans the faction AH with the SAME selection as the requester
//      would (AutoWowGear::AhPlan over offers the stock "item upgrade" scorer rates an equip and ilvl upgrade for the
//      requester's live Player*, within the request budget), buys the buyout listings with the broker's OWN gold
//      (treasury-backed, like the finished-bag market), and records a pending COD delivery per won item.
//   3. the broker mails each won item to the requester COD for the buyout price (+ FeeCopper, default 0). The
//      requester's existing errand mail stop pays the COD if it has the gold and takes the item (the stock equip pass
//      wears it), else the core returns the mail to the broker. A COD mail is accepted only when its sender is the
//      broker and the requester can pay.
//   4. rate limits: one open request per bot; at most MaxBuysPerMin buys per broker per minute; the once-per-level
//      file token above. Logs [AhBroker] request / buy / cod_paid / cod_returned.
//
// This header is the pure brain: the request record, the deterministic queue operations (string-free, never-reused
// character guids as ids), the per-request buy selection (forwarding to AhPlan), the COD price, and the requester's
// COD-accept verdict. Integer copper only, no floats, no RNG, stable explicit ordering. The runtime (the shared queue,
// the world-thread broker pass, the buys and the mails) lives in AutoWowSupply.cpp / NewRpgErrands.cpp and reuses the
// existing AH / treasury / mail primitives. OFF: every path early-returns and is byte-identical to the stock module.
namespace AutoWowBroker
{
inline constexpr std::uint8_t kPolicyVersion = 1;

struct Params
{
    std::uint32_t maxBuysPerMin = 6;  // AutoWow.Gear.AhBrokerMaxBuysPerMin: buys per broker per rolling minute
    std::uint32_t feeCopper = 0;      // AutoWow.Gear.AhBrokerFeeCopper: fixed COD surcharge over the buyout (default 0)
    std::uint32_t tickMs = 30000;     // AutoWow.Gear.AhBrokerTickMs: world-thread broker-pass period
};

// A requester's one open order. `team` is the core TeamId (0 alliance, 1 horde): string-free faction canon. `filedSec`
// is GameTime seconds at file time (service order + the staleness drop); the id that is never reused is `guid` (the
// character low guid).
struct Request
{
    std::uint32_t guid = 0;
    std::uint8_t team = 0;
    std::uint32_t level = 0;
    bool hunter = false;
    std::uint64_t budget = 0;
    std::uint32_t filedSec = 0;
};

inline constexpr std::size_t kNone = static_cast<std::size_t>(-1);

// Index of the open request of `guid`, or kNone.
[[nodiscard]] inline std::size_t Find(std::vector<Request> const& queue, std::uint32_t guid)
{
    for (std::size_t i = 0; i < queue.size(); ++i)
        if (queue[i].guid == guid)
            return i;
    return kNone;
}

// One open request per bot: a newer request of the same guid replaces the old (keeps its queue position so an in-flight
// bot cannot jump ahead by re-filing); otherwise it is appended.
inline void Upsert(std::vector<Request>& queue, Request const& req)
{
    std::size_t const i = Find(queue, req.guid);
    if (i != kNone)
        queue[i] = req;
    else
        queue.push_back(req);
}

// Drop the open request of `guid` (served, or the requester left). True when one was removed.
inline bool Erase(std::vector<Request>& queue, std::uint32_t guid)
{
    std::size_t const i = Find(queue, guid);
    if (i == kNone)
        return false;
    queue.erase(queue.begin() + i);
    return true;
}

// The team's open requests in service order: oldest first (lower filedSec), then lower guid. Deterministic, stable.
[[nodiscard]] inline std::vector<Request> OrderForTeam(std::vector<Request> const& queue, std::uint8_t team)
{
    std::vector<Request> out;
    for (Request const& r : queue)
        if (r.team == team)
            out.push_back(r);
    std::stable_sort(out.begin(), out.end(), [](Request const& a, Request const& b)
                     { return a.filedSec != b.filedSec ? a.filedSec < b.filedSec : a.guid < b.guid; });
    return out;
}

// A broker may still buy this pass: it has made fewer than MaxBuysPerMin buys in the rolling minute.
[[nodiscard]] inline bool CanBuy(Params const& p, std::uint32_t buysThisMinute)
{
    return buysThisMinute < p.maxBuysPerMin;
}

// The COD a won item is mailed for: its buyout plus the fixed fee. (COD is a uint32 field in the core; the runtime
// clamps, but at real buyouts + a small fee this never approaches the limit.)
[[nodiscard]] inline std::uint64_t CodPrice(Params const& p, std::uint64_t buyout)
{
    return buyout + p.feeCopper;
}

// The buys the broker places for one request: AutoWowGear::AhPlan over the requester's offers, within the request
// budget, at the requester's level / class. Selection is the requester's own gear logic -- the broker is only the
// buyer. `offers` are built at the faction AH against the requester's live Player* by the runtime.
[[nodiscard]] inline std::vector<AutoWowGear::AhOffer> PlanForRequest(Request const& req,
                                                                     std::vector<AutoWowGear::AhOffer> const& offers)
{
    return AutoWowGear::AhPlan(AutoWowGear::GetAh(), offers, req.level, req.hunter, req.budget);
}

// The verdict on a COD mail that reached the requester: pay and take only when the broker sent it and the requester
// can afford the COD; a mail from anyone else, or one the requester cannot pay, is returned to its sender.
enum class CodVerdict : std::uint8_t
{
    Accept = 0,       // pay the COD, take the item
    ReturnNotBroker,  // sender is not the requester's broker
    ReturnOverBudget  // the broker's mail, but the requester cannot pay the COD now
};

[[nodiscard]] inline CodVerdict DecideRequesterCod(std::uint32_t sender, std::uint32_t broker, std::uint64_t cod,
                                                   std::uint64_t requesterMoney)
{
    if (sender != broker)
        return CodVerdict::ReturnNotBroker;
    if (requesterMoney < cod)
        return CodVerdict::ReturnOverBudget;
    return CodVerdict::Accept;
}

[[nodiscard]] inline char const* CodVerdictName(CodVerdict v)
{
    switch (v)
    {
        case CodVerdict::Accept:
            return "accept";
        case CodVerdict::ReturnNotBroker:
            return "not_broker";
        case CodVerdict::ReturnOverBudget:
            return "budget";
        default:
            return "none";
    }
}

// ---- runtime (flag + params; read by AutoWowErrands::LoadConfig, used by NewRpgErrands.cpp to file requests and by
// AutoWowSupply.cpp / AutoWowTrade.cpp for the broker pass and the requester's COD pickup) -------------------------
namespace detail
{
inline bool gEnabled = false;
inline Params gParams;

// The shared request inbox: the requester (map thread) files, the broker pass (world thread) drains. Bounded so a
// flag-on run without a broker online cannot grow it without limit; the oldest-position request is dropped first.
inline std::mutex gQueueLock;
inline std::vector<Request> gQueue;
inline constexpr std::size_t kMaxOpen = 512;
}  // namespace detail
inline bool Enabled() { return detail::gEnabled; }
[[nodiscard]] inline Params const& Get() { return detail::gParams; }

// The COD-mail subject the broker sends and the requester's mail stop recognizes (string-free matching stays in the
// COD verdict; the subject only tags the mail as broker traffic so a requester never pays a stranger's COD).
inline constexpr char kBrokerSubject[] = "AutoWoW AH broker";

// Map thread (the requester, at a dropped AH-gear run): file / replace its one open request. No-op unless Enabled().
inline void File(Request const& req)
{
    if (!detail::gEnabled)
        return;
    std::lock_guard<std::mutex> guard(detail::gQueueLock);
    Upsert(detail::gQueue, req);
    while (detail::gQueue.size() > detail::kMaxOpen)
        detail::gQueue.erase(detail::gQueue.begin());
}

// The broker (its rep's map-thread auctioneer visit): the team's open requests in service order.
[[nodiscard]] inline std::vector<Request> TeamRequests(std::uint8_t team)
{
    std::lock_guard<std::mutex> guard(detail::gQueueLock);
    return OrderForTeam(detail::gQueue, team);
}

// The broker: drop a request once it is served, stale, or the requester has moved on.
inline void Forget(std::uint32_t guid)
{
    std::lock_guard<std::mutex> guard(detail::gQueueLock);
    Erase(detail::gQueue, guid);
}
}  // namespace AutoWowBroker

#endif  // AUTOWOW_AH_BROKER_POLICY_H
