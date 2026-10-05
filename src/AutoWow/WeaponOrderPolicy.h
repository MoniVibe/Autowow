/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_WEAPON_ORDER_POLICY_H
#define AUTOWOW_WEAPON_ORDER_POLICY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// AutoWow.Gear.NoWhite source (d): a weapon ORDER for the Smiths house. The NoWhite world pass (AutoWowNoWhite.cpp)
// posts one order per (bot, weapon slot) still under the floor after its own bags, the auction house and the
// hand-me-downs; the smith line (lane smithfocus) consumes the queue: Pending() in id order, then Fill() once the
// weapon is mailed (or Cancel()). The pass cancels a bot's orders once its slot clears the floor. This header is the
// whole contract: the queue is in-memory (orders do not survive a restart: the pass re-posts them).
//
// Retrofit notes: order ids are run-scoped and never reused (monotonic from 1); Pending() is ordered by id (FIFO,
// deterministic); a version field guards consumers; no strings in the order.
namespace AutoWowWeaponOrder
{
inline constexpr std::uint8_t kPolicyVersion = 1;
inline constexpr std::size_t kMaxOrders = 256;  // the queue refuses beyond this (Post returns 0)

struct Order
{
    std::uint32_t id = 0;       // run-scoped, never reused
    std::uint32_t guid = 0;     // the bot the weapon is for (guid-low)
    std::uint8_t slot = 0;      // EQUIPMENT_SLOT_MAINHAND 15 | OFFHAND 16 | RANGED 17
    std::uint8_t cls = 0;       // CLASS_* of the bot (the smith picks a usable weapon type)
    std::uint32_t level = 0;    // the bot's level at posting (RequiredLevel <= level)
    std::uint32_t minIlvl = 0;  // the floor's item level (AutoWowNoWhite::FloorIlvl); quality >= green
    bool alliance = false;
};

class Queue
{
public:
    // The open order of (guid, slot) if any (its id), else a new one; 0 when the queue is full.
    std::uint32_t Post(Order o)
    {
        for (Order const& x : orders_)
            if (x.guid == o.guid && x.slot == o.slot)
                return x.id;
        if (orders_.size() >= kMaxOrders)
            return 0;
        o.id = nextId_++;
        orders_.push_back(o);  // ids ascend: the vector stays in id order
        return o.id;
    }

    [[nodiscard]] std::vector<Order> Pending(bool alliance) const
    {
        std::vector<Order> out;
        for (Order const& x : orders_)
            if (x.alliance == alliance)
                out.push_back(x);
        return out;
    }

    [[nodiscard]] bool Has(std::uint32_t guid, std::uint8_t slot) const
    {
        return std::any_of(orders_.begin(), orders_.end(),
                           [&](Order const& x) { return x.guid == guid && x.slot == slot; });
    }

    // Removes the order (filled or cancelled); false when unknown.
    bool Remove(std::uint32_t id)
    {
        auto const it = std::find_if(orders_.begin(), orders_.end(), [id](Order const& x) { return x.id == id; });
        if (it == orders_.end())
            return false;
        orders_.erase(it);
        return true;
    }

    [[nodiscard]] std::size_t Size() const { return orders_.size(); }

private:
    std::vector<Order> orders_;
    std::uint32_t nextId_ = 1;
};

// ---- runtime queue (any thread; the NoWhite world pass posts / cancels, the smith line reads / fills) ----------
namespace detail
{
inline std::mutex gLock;
inline Queue gQueue;
}  // namespace detail

inline std::uint32_t Post(Order const& o)
{
    std::lock_guard<std::mutex> guard(detail::gLock);
    return detail::gQueue.Post(o);
}
inline std::vector<Order> Pending(bool alliance)
{
    std::lock_guard<std::mutex> guard(detail::gLock);
    return detail::gQueue.Pending(alliance);
}
inline bool Has(std::uint32_t guid, std::uint8_t slot)
{
    std::lock_guard<std::mutex> guard(detail::gLock);
    return detail::gQueue.Has(guid, slot);
}
inline bool Fill(std::uint32_t id)
{
    std::lock_guard<std::mutex> guard(detail::gLock);
    return detail::gQueue.Remove(id);
}
inline bool Cancel(std::uint32_t id) { return Fill(id); }
}  // namespace AutoWowWeaponOrder

#endif  // AUTOWOW_WEAPON_ORDER_POLICY_H
