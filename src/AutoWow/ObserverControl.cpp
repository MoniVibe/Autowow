/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ObserverControl.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "Config.h"
#include "InstanceSaveMgr.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "Playerbots.h"

namespace
{
// Observer -> watched leader. Written and read only on the world thread; the mutex is a
// cheap belt-and-suspenders guard, not a correctness requirement.
std::mutex g_observerMutex;
std::unordered_map<uint32, uint32> g_watchedLeader;

// Keep the camera just above the floor instead of reusing a bot's exact Z. The probe starts
// immediately above the watched position so multi-level collision does not prefer a distant
// overhead surface, and all movement stays bounded to the same local floor context.
float constexpr CameraTrailingDistance = 8.0f;
float constexpr CameraHeightProbeAboveLeader = 2.0f;
float constexpr CameraHeightSearchDistance = 10.0f;
float constexpr CameraVerticalOffset = 2.0f;
float constexpr MaxCameraVerticalOffset = 3.0f;
float constexpr MaxCameraFloorDelta = 8.0f;

std::string JsonString(std::string const& value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char character : value)
    {
        switch (character)
        {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (character < 0x20)
                {
                    static char const hex[] = "0123456789abcdef";
                    out << "\\u00" << hex[(character >> 4) & 0x0f] << hex[character & 0x0f];
                }
                else
                    out << static_cast<char>(character);
        }
    }
    out << '"';
    return out.str();
}

std::string Error(std::string const& code)
{
    return "{\"ok\":false,\"error\":" + JsonString(code) + "}";
}

// Parse the allow-list once per call from module configuration. An empty/absent option means
// "no observers are permitted", which is the safe default: the surface does nothing until the
// operator explicitly enrolls a guid.
bool IsAllowedObserver(uint32 guid)
{
    if (!guid)
        return false;

    std::string const configured = sConfigMgr->GetOption<std::string>("AutoWow.ObserverGuids", "");
    std::unordered_set<uint32> allowed;
    std::string token;
    for (char character : configured)
    {
        if (std::isdigit(static_cast<unsigned char>(character)))
        {
            token.push_back(character);
        }
        else if (!token.empty())
        {
            allowed.insert(static_cast<uint32>(std::strtoul(token.c_str(), nullptr, 10)));
            token.clear();
        }
    }
    if (!token.empty())
        allowed.insert(static_cast<uint32>(std::strtoul(token.c_str(), nullptr, 10)));

    return allowed.find(guid) != allowed.end();
}

// Resolve an allow-listed, online, non-bot observer. Order matters: an un-enrolled guid is
// rejected before we reveal anything else about it.
Player* ResolveObserver(uint32 guid, std::string& errorCode)
{
    if (!IsAllowedObserver(guid))
    {
        errorCode = "not_an_observer";
        return nullptr;
    }

    Player* observer = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
    if (!observer)
    {
        errorCode = "observer_not_online";
        return nullptr;
    }

    // Refuse to ever drive a Playerbots-controlled character through this surface.
    if (PlayerbotsMgr::instance().GetPlayerbotAI(observer))
    {
        errorCode = "observer_is_a_bot";
        return nullptr;
    }

    return observer;
}

bool IsProtected(Player* observer)
{
    // GM mode ON + GM-invisible: untargetable, no aggro, invisible to players. This is the
    // required precondition for relocation so the camera can never interfere.
    return observer->IsGameMaster() && !observer->isGMVisible();
}

std::string Watch(uint32 observerGuid, uint32 leaderGuid)
{
    std::string errorCode;
    Player* observer = ResolveObserver(observerGuid, errorCode);
    if (!observer)
        return Error(errorCode);

    if (!leaderGuid)
        return Error("watch_requires_leader_guid");

    Player* leader = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(leaderGuid));
    if (!leader)
        return Error("leader_not_online");

    {
        std::lock_guard<std::mutex> lock(g_observerMutex);
        g_watchedLeader[observerGuid] = leaderGuid;
    }

    std::ostringstream out;
    out << "{\"ok\":true,\"order\":\"watch\",\"observer\":" << observerGuid
        << ",\"leader\":" << leaderGuid
        << ",\"leader_name\":" << JsonString(leader->GetName()) << "}";
    return out.str();
}

std::string Release(uint32 observerGuid)
{
    std::string errorCode;
    Player* observer = ResolveObserver(observerGuid, errorCode);
    if (!observer)
        return Error(errorCode);

    uint32 leaderGuid = 0;
    {
        std::lock_guard<std::mutex> lock(g_observerMutex);
        auto const watched = g_watchedLeader.find(observerGuid);
        if (watched != g_watchedLeader.end())
            leaderGuid = watched->second;
        g_watchedLeader.erase(observerGuid);
    }

    // Observer relocation may install a temporary player-only bind so an ungrouped observer can
    // enter the exact raid copy. Remove only that matching temporary bind; permanent lockouts and
    // unrelated instance rows are never touched. A player already inside remains in the live map
    // until they leave, but cannot re-enter through this released bind.
    bool temporaryBindReleased = false;
    Player* leader = leaderGuid
        ? ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(leaderGuid))
        : nullptr;
    uint32 const instanceId = leader && leader->GetInstanceId()
        ? leader->GetInstanceId()
        : observer->GetInstanceId();
    if (instanceId)
    {
        if (InstanceSave* save = sInstanceSaveMgr->GetInstanceSave(instanceId))
        {
            InstancePlayerBind* bind = sInstanceSaveMgr->PlayerGetBoundInstance(
                observer->GetGUID(), save->GetMapId(), save->GetDifficulty());
            if (bind && !bind->perm && bind->save == save)
            {
                sInstanceSaveMgr->PlayerUnbindInstance(
                    observer->GetGUID(), save->GetMapId(), save->GetDifficulty(), true, observer);
                temporaryBindReleased = true;
            }
        }
    }

    std::ostringstream out;
    out << "{\"ok\":true,\"order\":\"release\",\"observer\":" << observerGuid
        << ",\"temporary_bind_released\":" << (temporaryBindReleased ? "true" : "false") << "}";
    return out.str();
}

std::string Protect(uint32 observerGuid)
{
    std::string errorCode;
    Player* observer = ResolveObserver(observerGuid, errorCode);
    if (!observer)
        return Error(errorCode);

    // Apply GM protection to the observer's own runtime state only. This is not a database
    // write and never touches any bot. It makes the observer untargetable, invisible, and
    // non-interfering. Relocation still independently verifies IsProtected().
    observer->SetGameMaster(true);
    observer->SetGMVisible(false);
    observer->SetGMChat(false);
    observer->SetAcceptWhispers(false);

    std::ostringstream out;
    out << "{\"ok\":true,\"order\":\"protect\",\"observer\":" << observerGuid
        << ",\"is_gm\":true,\"gm_visible\":false}";
    return out.str();
}

std::string Relocate(uint32 observerGuid)
{
    std::string errorCode;
    Player* observer = ResolveObserver(observerGuid, errorCode);
    if (!observer)
        return Error(errorCode);

    // Hard safety preconditions. Any failure means we do NOT move the observer.
    if (!IsProtected(observer))
        return Error("observer_not_protected");        // run `observe protect` or `.gm on` + `.gm visible off`
    if (observer->GetGroup())
        return Error("observer_in_group");             // grouping would split bot XP/loot; refuse
    if (observer->IsBeingTeleported())
        return Error("observer_teleport_in_progress");
    if (observer->IsInCombat())
        return Error("observer_in_combat");            // should be impossible while GM-protected; belt-and-suspenders

    uint32 leaderGuid = 0;
    {
        std::lock_guard<std::mutex> lock(g_observerMutex);
        auto it = g_watchedLeader.find(observerGuid);
        if (it != g_watchedLeader.end())
            leaderGuid = it->second;
    }
    if (!leaderGuid)
        return Error("no_watched_leader");

    Player* leader = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(leaderGuid));
    if (!leader)
        return Error("leader_not_online");

    Map* const observerMap = observer->FindMap();
    if (!observer->IsInWorld() || !observerMap ||
        observerMap->GetId() != observer->GetMapId() ||
        observerMap->GetInstanceId() != observer->GetInstanceId())
        return Error("observer_context_invalid");

    uint32 const leaderMap = leader->GetMapId();
    uint32 const leaderInstance = leader->GetInstanceId();
    Map* const leaderMapObject = leader->FindMap();
    float const lx = leader->GetPositionX();
    float const ly = leader->GetPositionY();
    float const lz = leader->GetPositionZ();
    float const lo = leader->GetOrientation();

    if (!leader->IsInWorld() || !leaderMapObject ||
        leaderMapObject->GetId() != leaderMap ||
        leaderMapObject->GetInstanceId() != leaderInstance ||
        !leader->GetPhaseMask())
        return Error("leader_context_invalid");
    if (!std::isfinite(lx) || !std::isfinite(ly) || !std::isfinite(lz) ||
        !std::isfinite(lo))
        return Error("leader_position_invalid");

    // Keep the camera behind the leader on the known approach line. Exact-footprint relocation
    // hides a stacked formation and can fire a real client's area trigger before headless bots.
    float const cameraX = lx - std::cos(lo) * CameraTrailingDistance;
    float const cameraY = ly - std::sin(lo) * CameraTrailingDistance;
    if (!std::isfinite(cameraX) || !std::isfinite(cameraY) ||
        !MapMgr::IsValidMapCoord(leaderMap, cameraX, cameraY))
        return Error("observer_destination_invalid");

    // Get the floor at the actual destination in the watched bot's phase/context. Never use the
    // bot's exact Z as the camera height: on sloped or multi-level terrain that can put a GM
    // observer inside collision. A missing or materially different floor is unsafe, so do not
    // bind or teleport the observer when the core cannot validate this local destination.
    float const cameraHeightProbeZ = lz + CameraHeightProbeAboveLeader;
    if (!std::isfinite(cameraHeightProbeZ))
        return Error("observer_height_probe_invalid");
    float const cameraFloorZ = leaderMapObject->GetHeight(
        leader->GetPhaseMask(), cameraX, cameraY,
        cameraHeightProbeZ, true, CameraHeightSearchDistance);
    if (cameraFloorZ <= INVALID_HEIGHT || !std::isfinite(cameraFloorZ))
        return Error("observer_floor_unavailable");
    if (std::fabs(cameraFloorZ - lz) > MaxCameraFloorDelta)
        return Error("observer_floor_context_invalid");

    if (CameraVerticalOffset <= 0.0f || CameraVerticalOffset > MaxCameraVerticalOffset)
        return Error("observer_height_policy_invalid");
    float const cameraZ = cameraFloorZ + CameraVerticalOffset;
    if (!std::isfinite(cameraZ) || !(cameraZ > cameraFloorZ) ||
        !MapMgr::IsValidMapCoord(leaderMap, cameraX, cameraY, cameraZ, lo))
        return Error("observer_destination_height_invalid");
    float const cameraZOffset = cameraZ - lz;
    if (!std::isfinite(cameraZOffset))
        return Error("observer_destination_height_invalid");

    bool installedTemporaryBind = false;
    if (leaderInstance)
    {
        InstanceSave* save = sInstanceSaveMgr->GetInstanceSave(leaderInstance);
        if (!save || save->GetMapId() != leaderMap)
            return Error("leader_instance_save_not_found");

        InstancePlayerBind* bind = sInstanceSaveMgr->PlayerGetBoundInstance(
            observer->GetGUID(), leaderMap, save->GetDifficulty());
        if (bind && bind->save != save && bind->perm)
            return Error("observer_permanent_instance_conflict");
        if (bind && bind->save != save)
        {
            sInstanceSaveMgr->PlayerUnbindInstance(
                observer->GetGUID(), leaderMap, save->GetDifficulty(), true, observer);
            bind = nullptr;
        }
        if (!bind)
        {
            sInstanceSaveMgr->PlayerBindToInstance(observer->GetGUID(), save, false, observer);
            installedTemporaryBind = true;
        }
    }

    bool const sameInstance = observer->GetMapId() == leaderMap &&
        observer->GetInstanceId() == leaderInstance;

    // Skip a redundant teleport only when both map and exact instance already match.
    if (sameInstance)
    {
        float const dx = observer->GetPositionX() - cameraX;
        float const dy = observer->GetPositionY() - cameraY;
        float const dz = observer->GetPositionZ() - cameraZ;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) <= 1.0f)
        {
            std::ostringstream out;
            out << "{\"ok\":true,\"order\":\"relocate\",\"observer\":" << observerGuid
                << ",\"leader\":" << leaderGuid << ",\"map\":" << leaderMap
                << ",\"instance_id\":" << leaderInstance
                << ",\"camera_offset_yards\":" << CameraTrailingDistance
                << ",\"camera_offset_z_yards\":" << cameraZOffset
                << ",\"camera_floor_z\":" << cameraFloorZ
                << ",\"camera_lift_yards\":" << CameraVerticalOffset
                << ",\"x\":" << cameraX << ",\"y\":" << cameraY << ",\"z\":" << cameraZ
                << ",\"result\":\"already_positioned\"}";
            return out.str();
        }
    }

    // Force a far transfer when map IDs match but instance IDs differ. The temporary observer-only
    // bind above makes MapMgr select the leader's exact copy without adding the observer to the raid.
    bool const forceInstanceTransfer = observer->GetMapId() == leaderMap &&
        observer->GetInstanceId() != leaderInstance;
    if (!observer->TeleportTo(leaderMap, cameraX, cameraY, cameraZ, lo,
                              TELE_TO_GM_MODE, leader, forceInstanceTransfer))
        return Error("observer_relocate_failed");

    std::ostringstream out;
    out << "{\"ok\":true,\"order\":\"relocate\",\"observer\":" << observerGuid
        << ",\"leader\":" << leaderGuid
        << ",\"leader_name\":" << JsonString(leader->GetName())
        << ",\"map\":" << leaderMap
        << ",\"instance_id\":" << leaderInstance
        << ",\"temporary_bind_installed\":" << (installedTemporaryBind ? "true" : "false")
        << ",\"camera_offset_yards\":" << CameraTrailingDistance
        << ",\"camera_offset_z_yards\":" << cameraZOffset
        << ",\"camera_floor_z\":" << cameraFloorZ
        << ",\"camera_lift_yards\":" << CameraVerticalOffset
        << ",\"x\":" << cameraX << ",\"y\":" << cameraY << ",\"z\":" << cameraZ << "}";
    return out.str();
}

std::string Status(uint32 observerGuid)
{
    std::string errorCode;
    Player* observer = ResolveObserver(observerGuid, errorCode);
    if (!observer)
        return Error(errorCode);

    uint32 leaderGuid = 0;
    {
        std::lock_guard<std::mutex> lock(g_observerMutex);
        auto it = g_watchedLeader.find(observerGuid);
        if (it != g_watchedLeader.end())
            leaderGuid = it->second;
    }

    Player* leader = leaderGuid
        ? ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(leaderGuid))
        : nullptr;

    float distance = -1.0f;
    bool sameMap = false;
    bool sameInstance = false;
    if (leader && leader->GetMapId() == observer->GetMapId())
    {
        sameMap = true;
        sameInstance = leader->GetInstanceId() == observer->GetInstanceId();
        if (sameInstance)
        {
            float const dx = observer->GetPositionX() - leader->GetPositionX();
            float const dy = observer->GetPositionY() - leader->GetPositionY();
            float const dz = observer->GetPositionZ() - leader->GetPositionZ();
            distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    }

    std::ostringstream out;
    out << "{\"ok\":true,\"order\":\"status\",\"observer\":" << observerGuid
        << ",\"name\":" << JsonString(observer->GetName())
        << ",\"protected\":" << (IsProtected(observer) ? "true" : "false")
        << ",\"is_gm\":" << (observer->IsGameMaster() ? "true" : "false")
        << ",\"gm_visible\":" << (observer->isGMVisible() ? "true" : "false")
        << ",\"in_group\":" << (observer->GetGroup() ? "true" : "false")
        << ",\"in_combat\":" << (observer->IsInCombat() ? "true" : "false")
        << ",\"position\":{\"map\":" << observer->GetMapId()
        << ",\"instance_id\":" << observer->GetInstanceId()
        << ",\"x\":" << observer->GetPositionX()
        << ",\"y\":" << observer->GetPositionY()
        << ",\"z\":" << observer->GetPositionZ() << "}"
        << ",\"watched\":{\"leader\":" << leaderGuid
        << ",\"online\":" << (leader ? "true" : "false")
        << ",\"name\":" << JsonString(leader ? leader->GetName() : "")
        << ",\"same_map\":" << (sameMap ? "true" : "false")
        << ",\"same_instance\":" << (sameInstance ? "true" : "false")
        << ",\"instance_id\":" << (leader ? leader->GetInstanceId() : 0)
        << ",\"distance\":" << distance << "}}";
    return out.str();
}
}  // namespace

namespace AutoWowObserver
{
std::string Dispatch(std::string const& sub, uint32 observerGuid, uint32 leaderGuid)
{
    if (sub == "watch")
        return Watch(observerGuid, leaderGuid);
    if (sub == "relocate")
        return Relocate(observerGuid);
    if (sub == "status")
        return Status(observerGuid);
    if (sub == "release")
        return Release(observerGuid);
    if (sub == "protect")
        return Protect(observerGuid);

    return Error("unknown_observe_subcommand");
}
}  // namespace AutoWowObserver
