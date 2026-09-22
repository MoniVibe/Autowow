#ifndef PLAYERBOTS_RAIDSAFEZONESELECTIONPOLICY_H
#define PLAYERBOTS_RAIDSAFEZONESELECTIONPOLICY_H

#include <cstddef>
#include <limits>
#include <vector>

namespace RaidSafeZoneSelectionPolicy
{
struct Point
{
    float x = 0.0f;
    float y = 0.0f;
};

// Strict comparison makes equal-distance ties deterministic by preserving input order.
inline std::size_t SelectClosest(Point const& origin, std::vector<Point> const& zones)
{
    std::size_t bestIndex = 0;
    float bestDistanceSquared = std::numeric_limits<float>::max();
    for (std::size_t index = 0; index < zones.size(); ++index)
    {
        float const dx = origin.x - zones[index].x;
        float const dy = origin.y - zones[index].y;
        float const distanceSquared = dx * dx + dy * dy;
        if (distanceSquared < bestDistanceSquared)
        {
            bestDistanceSquared = distanceSquared;
            bestIndex = index;
        }
    }
    return bestIndex;
}

// Encounters that require the whole raid to converge on one shelter can continue to select
// from a shared anchor. Directional hazards such as Onyxia's Deep Breath should instead call
// SelectClosest with each responder's position so bots do not cross the active damage lane.
inline std::size_t SelectClosestToSharedAnchor(Point const& anchor, std::vector<Point> const& zones)
{
    return SelectClosest(anchor, zones);
}
}

#endif
