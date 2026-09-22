#include "OnyBreathSignal.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

namespace
{
using Clock = std::chrono::steady_clock;

struct Signal
{
    uint32 spellId = 0;
    Clock::time_point expiresAt{};
    bool moveLogged = false;
    bool arrivalLogged = false;
};

std::mutex signalMutex;
std::unordered_map<uint32, Signal> signals;
std::unordered_map<uint32, Signal> encounterSignals;
constexpr auto SignalLifetime = std::chrono::seconds(10);
}

bool OnyxiaBreathSignal::IsDeepBreathSpell(uint32 spellId)
{
    switch (spellId)
    {
        case 17086:
        case 18351:
        case 18576:
        case 18609:
        case 18564:
        case 18584:
        case 18596:
        case 18617:
            return true;
        default:
            return false;
    }
}

bool OnyxiaBreathSignal::RecordEncounter(uint32 instanceId, uint32 spellId)
{
    if (!instanceId || !IsDeepBreathSpell(spellId))
        return false;

    std::lock_guard<std::mutex> guard(signalMutex);
    Clock::time_point const now = Clock::now();
    Signal& signal = encounterSignals[instanceId];
    bool const isNew = signal.spellId != spellId || now >= signal.expiresAt;
    if (isNew)
        signal = Signal{spellId, now + SignalLifetime, false, false};
    else
        signal.expiresAt = now + SignalLifetime;
    return isNew;
}

uint32 OnyxiaBreathSignal::GetEncounterActive(uint32 instanceId)
{
    if (!instanceId)
        return 0;

    std::lock_guard<std::mutex> guard(signalMutex);
    auto const found = encounterSignals.find(instanceId);
    if (found == encounterSignals.end())
        return 0;
    if (Clock::now() >= found->second.expiresAt)
    {
        encounterSignals.erase(found);
        return 0;
    }
    return found->second.spellId;
}

void OnyxiaBreathSignal::ClearEncounter(uint32 instanceId)
{
    if (!instanceId)
        return;

    std::lock_guard<std::mutex> guard(signalMutex);
    encounterSignals.erase(instanceId);
}

void OnyxiaBreathSignal::Record(ObjectGuid botGuid, uint32 spellId)
{
    if (!botGuid || !IsDeepBreathSpell(spellId))
        return;

    std::lock_guard<std::mutex> guard(signalMutex);
    Clock::time_point const now = Clock::now();
    Signal& signal = signals[botGuid.GetCounter()];
    if (signal.spellId == spellId && now < signal.expiresAt)
    {
        signal.expiresAt = now + SignalLifetime;
        return;
    }

    signal = Signal{spellId, now + SignalLifetime, false, false};
}

uint32 OnyxiaBreathSignal::GetActive(ObjectGuid botGuid)
{
    if (!botGuid)
        return 0;

    std::lock_guard<std::mutex> guard(signalMutex);
    auto const found = signals.find(botGuid.GetCounter());
    if (found == signals.end())
        return 0;
    if (Clock::now() >= found->second.expiresAt)
    {
        signals.erase(found);
        return 0;
    }
    return found->second.spellId;
}

namespace
{
bool MarkSignalEvent(ObjectGuid botGuid, uint32 spellId, bool Signal::* marker)
{
    if (!botGuid || !OnyxiaBreathSignal::IsDeepBreathSpell(spellId))
        return false;

    std::lock_guard<std::mutex> guard(signalMutex);
    auto const found = signals.find(botGuid.GetCounter());
    if (found == signals.end() || found->second.spellId != spellId || Clock::now() >= found->second.expiresAt ||
        found->second.*marker)
    {
        return false;
    }

    found->second.*marker = true;
    return true;
}
}

bool OnyxiaBreathSignal::MarkMove(ObjectGuid botGuid, uint32 spellId)
{
    return MarkSignalEvent(botGuid, spellId, &Signal::moveLogged);
}

bool OnyxiaBreathSignal::MarkArrival(ObjectGuid botGuid, uint32 spellId)
{
    return MarkSignalEvent(botGuid, spellId, &Signal::arrivalLogged);
}

void OnyxiaBreathSignal::Clear(ObjectGuid botGuid)
{
    if (!botGuid)
        return;

    std::lock_guard<std::mutex> guard(signalMutex);
    signals.erase(botGuid.GetCounter());
}
