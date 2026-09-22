/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RaidFearSignal.h"

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace
{
using Clock = std::chrono::steady_clock;

struct Signal
{
    uint32 casterEntry = 0;
    uint32 spellId = 0;
    Clock::time_point expiresAt{};
};

constexpr auto SignalLifetime = std::chrono::seconds(30);
std::mutex signalMutex;
std::unordered_map<uint32, Signal> activeSignals;
std::unordered_set<uint32> knownFearCasters;
}

bool RaidFearSignal::Record(uint32 instanceId, uint32 casterEntry, uint32 spellId)
{
    if (!instanceId || !casterEntry || !spellId)
        return false;

    std::lock_guard<std::mutex> guard(signalMutex);
    Clock::time_point const now = Clock::now();
    Signal& signal = activeSignals[instanceId];
    bool const isNew = signal.casterEntry != casterEntry || signal.spellId != spellId || now >= signal.expiresAt;
    signal = Signal{casterEntry, spellId, now + SignalLifetime};
    knownFearCasters.insert(casterEntry);
    return isNew;
}

bool RaidFearSignal::RecordImminent(uint32 instanceId, uint32 casterEntry)
{
    if (!instanceId || !casterEntry)
        return false;

    std::lock_guard<std::mutex> guard(signalMutex);
    Clock::time_point const now = Clock::now();
    Signal& signal = activeSignals[instanceId];
    bool const isNew = signal.casterEntry != casterEntry || now >= signal.expiresAt;
    signal = Signal{casterEntry, signal.casterEntry == casterEntry ? signal.spellId : 0, now + SignalLifetime};
    return isNew;
}

uint32 RaidFearSignal::GetActiveSpell(uint32 instanceId)
{
    if (!instanceId)
        return 0;

    std::lock_guard<std::mutex> guard(signalMutex);
    auto const found = activeSignals.find(instanceId);
    if (found == activeSignals.end())
        return 0;
    if (Clock::now() >= found->second.expiresAt)
    {
        activeSignals.erase(found);
        return 0;
    }
    return found->second.spellId;
}

bool RaidFearSignal::IsActive(uint32 instanceId)
{
    if (!instanceId)
        return false;

    std::lock_guard<std::mutex> guard(signalMutex);
    auto const found = activeSignals.find(instanceId);
    if (found == activeSignals.end())
        return false;
    if (Clock::now() >= found->second.expiresAt)
    {
        activeSignals.erase(found);
        return false;
    }
    return true;
}

bool RaidFearSignal::IsKnownFearCaster(uint32 casterEntry)
{
    if (!casterEntry)
        return false;

    std::lock_guard<std::mutex> guard(signalMutex);
    return knownFearCasters.find(casterEntry) != knownFearCasters.end();
}

void RaidFearSignal::ClearInstance(uint32 instanceId)
{
    if (!instanceId)
        return;

    std::lock_guard<std::mutex> guard(signalMutex);
    activeSignals.erase(instanceId);
}
