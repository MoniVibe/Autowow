#ifndef _PLAYERBOT_AUTOWOW_PORTAL_ADMISSION_POLICY_H
#define _PLAYERBOT_AUTOWOW_PORTAL_ADMISSION_POLICY_H

#include "Define.h"

namespace AutoWowPortalAdmission
{
enum class MemberLocation
{
    InsideExpectedInstance,
    ReadyOutside,
    SplitInstance,
    Displaced,
};

inline MemberLocation ClassifyMember(uint32 memberMap, uint32 memberInstance,
    uint32 leaderMap, uint32 leaderInstance, uint32 exteriorMap, bool withinReadinessRadius)
{
    if (memberMap == leaderMap && memberInstance == leaderInstance)
        return MemberLocation::InsideExpectedInstance;
    if (memberMap == leaderMap && memberInstance != 0 && memberInstance != leaderInstance)
        return MemberLocation::SplitInstance;
    if (memberMap == exteriorMap && memberInstance == 0 && withinReadinessRadius)
        return MemberLocation::ReadyOutside;
    return MemberLocation::Displaced;
}
}

#endif
