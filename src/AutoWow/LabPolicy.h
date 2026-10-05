/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_LAB_POLICY_H
#define AUTOWOW_LAB_POLICY_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

// Combat lab (AutoWow.Lab.Enable / AutoWow.Lab.Trace, both default 0). The owner plays a reference fight on a
// Lab<Class> character against N identical mobs summoned on a ring around a fixed marker; the same character then
// replays the fight as a self-bot (.playerbots bot self) and lab-compare.py diffs the two runs from
// lab-trace.jsonl. Value-only helpers here (LabPolicyTest); runtime in LabControl.cpp.
namespace AutoWowLab
{
inline constexpr std::uint32_t kMaxSpawnCount = 8;
inline constexpr std::uint32_t kMaxMobLevel = 83;

struct SpawnArgs
{
    std::uint32_t entry = 0;
    std::uint32_t count = 0;
    std::uint32_t level = 0;  // 0 in the text = the configured lab level
};

// "<entry> <count> [level]": entry > 0, count 1..kMaxSpawnCount, level 1..kMaxMobLevel (omitted -> defaultLevel).
[[nodiscard]] inline bool ParseSpawnArgs(std::string_view text, std::uint32_t defaultLevel, SpawnArgs& out)
{
    std::istringstream in{std::string(text)};
    long long entry = 0, count = 0, level = 0;
    if (!(in >> entry >> count))
        return false;
    if (!(in >> level))
        level = defaultLevel;
    std::string rest;
    if (in.clear(), in >> rest)
        return false;
    if (entry <= 0 || entry > 0xFFFFFF || count < 1 || count > kMaxSpawnCount || level < 1 || level > kMaxMobLevel)
        return false;
    out.entry = static_cast<std::uint32_t>(entry);
    out.count = static_cast<std::uint32_t>(count);
    out.level = static_cast<std::uint32_t>(level);
    return true;
}

struct Marker
{
    std::uint32_t map = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f, o = 0.0f;
};

// "map x y z o" (o optional, radians).
[[nodiscard]] inline bool ParseMarker(std::string_view text, Marker& out)
{
    std::istringstream in{std::string(text)};
    Marker m;
    if (!(in >> m.map >> m.x >> m.y >> m.z))
        return false;
    if (!(in >> m.o))
        m.o = 0.0f;
    out = m;
    return true;
}

// Fixed spawn slot i of count on a ring of `radius` around the marker, starting straight ahead of the marker
// facing and going counter-clockwise; slots never depend on the caller's position. Returns (dx, dy).
[[nodiscard]] inline std::pair<float, float> RingOffset(std::uint32_t i, std::uint32_t count, float radius,
                                                        float facing)
{
    if (!count)
        return {0.0f, 0.0f};
    double const angle = static_cast<double>(facing) + 6.283185307179586 * (i % count) / count;
    return {static_cast<float>(radius * std::cos(angle)), static_cast<float>(radius * std::sin(angle))};
}

// AutoWow.Lab.KitIlvl "classId:ilvl,..." (e.g. "4:79,1:89") -> the item level cap for cls, 0 = uncapped.
[[nodiscard]] inline std::uint32_t KitIlvlFor(std::string_view map, std::uint32_t cls)
{
    std::size_t pos = 0;
    while (pos < map.size())
    {
        std::size_t const end = std::min(map.find(',', pos), map.size());
        std::string_view const item = map.substr(pos, end - pos);
        std::size_t const colon = item.find(':');
        if (colon != std::string_view::npos)
        {
            std::uint32_t c = 0, v = 0;
            bool ok = colon > 0 && colon + 1 < item.size();
            for (std::size_t k = 0; ok && k < item.size(); ++k)
            {
                char const ch = item[k];
                if (k == colon)
                    continue;
                if (ch == ' ')
                    continue;
                if (ch < '0' || ch > '9')
                    ok = false;
                else if (k < colon)
                    c = c * 10 + static_cast<std::uint32_t>(ch - '0');
                else
                    v = v * 10 + static_cast<std::uint32_t>(ch - '0');
            }
            if (ok && c == cls)
                return v;
        }
        pos = end + 1;
    }
    return 0;
}

// Scenario label carried on every trace row of a run: "<entry>x<count>@<level>", adds appended with '+'.
[[nodiscard]] inline std::string ScenarioPart(SpawnArgs const& a)
{
    return std::to_string(a.entry) + "x" + std::to_string(a.count) + "@" + std::to_string(a.level);
}

// Damage that takes the victim from >= pct to < pct of max health (one "lowhp" row per crossing).
[[nodiscard]] inline bool CrossesBelowPct(std::uint32_t health, std::uint32_t damage, std::uint32_t maxHealth,
                                          std::uint32_t pct)
{
    if (!maxHealth)
        return false;
    std::uint64_t const before = static_cast<std::uint64_t>(health) * 100;
    std::uint64_t const after = static_cast<std::uint64_t>(health > damage ? health - damage : 0) * 100;
    std::uint64_t const line = static_cast<std::uint64_t>(maxHealth) * pct;
    return before >= line && after < line;
}

[[nodiscard]] inline bool InRadius(float dx, float dy, float radius)
{
    return dx * dx + dy * dy <= radius * radius;
}

// Only Lab<Class> characters (the owner's lab set) may be kitted.
[[nodiscard]] inline bool IsLabName(std::string_view name)
{
    return name.size() > 3 && name.substr(0, 3) == "Lab";
}
}  // namespace AutoWowLab

#endif
