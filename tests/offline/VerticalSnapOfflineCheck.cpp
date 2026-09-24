/*
 * Offline replay of the Walking V2 mover over the extracted mmaps (AutoWow.Travel.VerticalSnap evidence).
 * Not part of unit_tests: it needs a DataDir with mmaps/ and reads nothing else.
 *
 * Build (WSL, from the module root; AC_CORE / AC_BUILD = the core source / build trees):
 *   c++ -O2 -std=gnu++20 -w -I$AC_CORE/deps/recastnavigation/Detour/Include -Isrc/Ai/World/Rpg \
 *       tests/offline/VerticalSnapOfflineCheck.cpp $AC_BUILD/deps/recastnavigation/Detour/libDetour.a -o vsnap
 * Run:   ./vsnap /root/p1data < cases.txt      (one case per line: map sx sy gx gy gz label)
 *
 * What is real and what is emulated:
 *   - real: the navmesh tiles, Detour, AutoWowVerticalSnap::Snap (NavmeshSnap.h), WalkingV2Policy chunk fan
 *     and admission, TravelIntentPolicy segment admission and the replan budget;
 *   - emulated (a line-by-line port of AzerothCore PathGenerator for a Player source, fresh generator):
 *     start/end poly lookup (3/5/3 then 3/50/3), far-from-poly (7 yd), findPath (74 polys), FindSmoothPath
 *     (4 yd steps, 74 points, steer z held at the previous step, +0.5 yd lift, IsWalkableClimb with the
 *     collision height), BuildPointPath type rules; chunk-probe height = navmesh floor at or below bot z + 50
 *     (the runtime asks the vmap/grid height);
 *   - walking a committed segment is modelled as arriving at its endpoint; combat, mobs, danger areas and
 *     the prepared quest-walk step are not modelled.
 * The bot is placed on each navmesh floor under the logged (x, y) of the give-up line.
 */

#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "NavmeshSnap.h"
#include "TravelIntentPolicy.h"
#include "WalkingV2Policy.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
constexpr std::uint32_t kNormal = 0x01, kIncomplete = 0x04, kNoPath = 0x08, kNotUsingPath = 0x10, kShort = 0x20,
                        kFarStart = 0x40, kFarEnd = 0x80;
constexpr std::uint32_t kTypeOk = kNormal | kIncomplete | kFarStart | kFarEnd;
constexpr int kMaxPolys = 74, kMaxPoints = 74;
constexpr float kStep = 4.0f, kSlop = 0.3f, kPathFinderDis = 70.0f, kInteract = 5.5f, kCollisionHeight = 2.0f;

struct TileHeader
{
    std::uint32_t magic, dtVersion, mmapVersion, size;
    char usesLiquids, pad[3];
    char recastConfig[36];
};
static_assert(sizeof(TileHeader) == 56);

std::string gDataDir;
std::map<std::uint32_t, dtNavMesh*> gMeshes;

dtNavMesh* LoadMap(std::uint32_t mapId)
{
    auto it = gMeshes.find(mapId);
    if (it != gMeshes.end())
        return it->second;
    char fn[512];
    std::snprintf(fn, sizeof fn, "%s/mmaps/%03u.mmap", gDataDir.c_str(), mapId);
    FILE* f = std::fopen(fn, "rb");
    if (!f)
        return gMeshes[mapId] = nullptr;
    dtNavMeshParams params;
    bool const ok = std::fread(&params, sizeof params, 1, f) == 1;
    std::fclose(f);
    dtNavMesh* mesh = dtAllocNavMesh();
    if (!ok || dtStatusFailed(mesh->init(&params)))
        return gMeshes[mapId] = nullptr;
    for (int x = 0; x < 64; ++x)
        for (int y = 0; y < 64; ++y)
        {
            std::snprintf(fn, sizeof fn, "%s/mmaps/%03u%02i%02i.mmtile", gDataDir.c_str(), mapId, x, y);
            FILE* t = std::fopen(fn, "rb");
            if (!t)
                continue;
            TileHeader h;
            if (std::fread(&h, sizeof h, 1, t) == 1)
            {
                unsigned char* data = static_cast<unsigned char*>(dtAlloc(h.size, DT_ALLOC_PERM));
                if (std::fread(data, h.size, 1, t) == 1)
                    mesh->addTile(data, h.size, DT_TILE_FREE_DATA, 0, nullptr);
                else
                    dtFree(data);
            }
            std::fclose(t);
        }
    return gMeshes[mapId] = mesh;
}

struct V3
{
    float x = 0, y = 0, z = 0;
};
float Dist3(V3 a, V3 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }

// ---- PathGenerator port (Player source) ----------------------------------------------------------------
struct Gen
{
    dtNavMeshQuery* q;
    dtQueryFilter filter;
    bool slopeCheck;

    dtPolyRef PolyAt(float const* p, float* dist) const
    {
        float ext[3] = {3.0f, 5.0f, 3.0f};
        float closest[3];
        dtPolyRef ref = 0;
        if (dtStatusSucceed(q->findNearestPoly(p, ext, &filter, &ref, closest)) && ref)
        {
            *dist = dtVdist(closest, p);
            return ref;
        }
        ext[1] = 50.0f;
        if (dtStatusSucceed(q->findNearestPoly(p, ext, &filter, &ref, closest)) && ref)
        {
            *dist = dtVdist(closest, p);
            return ref;
        }
        *dist = 1e30f;
        return 0;
    }

    static bool InRangeYZX(float const* a, float const* b, float r, float h)
    {
        float const dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
        return (dx * dx + dz * dz) < r * r && std::fabs(dy) < h;
    }

    static std::uint32_t Fixup(dtPolyRef* path, std::uint32_t npath, std::uint32_t maxPath, dtPolyRef const* visited,
                               std::uint32_t nvisited)
    {
        int fp = -1, fv = -1;
        for (int i = int(npath) - 1; i >= 0; --i)
        {
            bool found = false;
            for (int j = int(nvisited) - 1; j >= 0; --j)
                if (path[i] == visited[j])
                {
                    fp = i;
                    fv = j;
                    found = true;
                }
            if (found)
                break;
        }
        if (fp == -1 || fv == -1)
            return npath;
        std::uint32_t const req = nvisited - fv;
        std::uint32_t const orig = std::uint32_t(fp + 1) < npath ? fp + 1 : npath;
        std::uint32_t size = npath > orig ? npath - orig : 0;
        if (req + size > maxPath)
            size = maxPath - req;
        if (size)
            std::memmove(path + req, path + orig, size * sizeof(dtPolyRef));
        for (std::uint32_t i = 0; i < req; ++i)
            path[i] = visited[(nvisited - 1) - i];
        return req + size;
    }

    bool Steer(float const* start, float const* end, dtPolyRef const* path, std::uint32_t n, float* steer,
               unsigned char& flag) const
    {
        float sp[9];
        unsigned char sf[3];
        dtPolyRef spp[3];
        int ns = 0;
        if (dtStatusFailed(q->findStraightPath(start, end, path, n, sp, sf, spp, &ns, 3)) || !ns)
            return false;
        int k = 0;
        while (k < ns)
        {
            if ((sf[k] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) || !InRangeYZX(&sp[k * 3], start, kSlop, 1000.0f))
                break;
            ++k;
        }
        if (k >= ns)
            return false;
        dtVcopy(steer, &sp[k * 3]);
        steer[1] = start[1];  // keep Z value (PathGenerator::GetSteerTarget)
        flag = sf[k];
        return true;
    }

    static bool WalkableClimb(float const* a, float const* b)
    {
        float const floorDist = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[2] - b[2]) * (a[2] - b[2]));
        float const deg = std::atan(std::fabs(b[1] - a[1]) / std::fabs(floorDist)) * 180.0f / float(M_PI);
        return std::fabs(b[1] - a[1]) <= kCollisionHeight - kCollisionHeight * (deg / 100);
    }

    // FindSmoothPath: returns dtStatus-like {ok, steep} and the points.
    void Smooth(float const* startPos, float const* endPos, dtPolyRef const* polyPath, std::uint32_t polyCount,
                std::vector<float>& out, bool& failed, bool& steep) const
    {
        failed = steep = false;
        out.clear();
        dtPolyRef polys[kMaxPolys];
        std::memcpy(polys, polyPath, sizeof(dtPolyRef) * polyCount);
        std::uint32_t npolys = polyCount;
        float iter[3], target[3];
        if (polyCount > 1)
        {
            if (dtStatusFailed(q->closestPointOnPolyBoundary(polys[0], startPos, iter)) ||
                dtStatusFailed(q->closestPointOnPolyBoundary(polys[npolys - 1], endPos, target)))
            {
                failed = true;
                return;
            }
        }
        else
        {
            dtVcopy(iter, startPos);
            dtVcopy(target, endPos);
        }
        auto push = [&](float const* p) { out.insert(out.end(), p, p + 3); };
        push(iter);
        while (npolys && out.size() / 3 < kMaxPoints)
        {
            float steer[3];
            unsigned char flag = 0;
            if (!Steer(iter, target, polys, npolys, steer, flag))
                break;
            bool const endOfPath = (flag & DT_STRAIGHTPATH_END) != 0;
            bool const offMesh = (flag & DT_STRAIGHTPATH_OFFMESH_CONNECTION) != 0;
            float delta[3];
            dtVsub(delta, steer, iter);
            float len = dtMathSqrtf(dtVdot(delta, delta));
            len = ((endOfPath || offMesh) && len < kStep) ? 1.0f : kStep / len;
            float moveTgt[3];
            dtVmad(moveTgt, iter, delta, len);
            float result[3];
            dtPolyRef visited[16];
            int nvisited = 0;
            if (dtStatusFailed(q->moveAlongSurface(polys[0], iter, moveTgt, &filter, result, visited, &nvisited, 16)))
            {
                failed = true;
                return;
            }
            npolys = Fixup(polys, npolys, kMaxPolys, visited, nvisited);
            q->getPolyHeight(polys[0], result, &result[1]);
            result[1] += 0.5f;
            dtVcopy(iter, result);
            if (slopeCheck && !WalkableClimb(iter, steer))
            {
                out.resize(out.size() - 3);
                failed = steep = true;
                return;
            }
            if (endOfPath && InRangeYZX(iter, steer, kSlop, 1.0f))
            {
                dtVcopy(iter, target);
                if (out.size() / 3 < kMaxPoints)
                    push(iter);
                break;
            }
            if (out.size() / 3 < kMaxPoints)
                push(iter);
        }
        failed = out.size() / 3 >= kMaxPoints;  // "this is most likely a loop"
    }

    // CalculatePath from `from` to `to`: path type and actual end.
    std::uint32_t Calculate(V3 from, V3 to, V3& end) const
    {
        float startPoint[3] = {from.y, from.z, from.x};
        float endPoint[3] = {to.y, to.z, to.x};
        end = to;
        float dStart, dEnd;
        dtPolyRef const startPoly = PolyAt(startPoint, &dStart);
        dtPolyRef const endPoly = PolyAt(endPoint, &dEnd);
        if (!startPoly || !endPoly)
            return kNormal | kNotUsingPath;  // Player: "can fly" shortcut
        std::uint32_t type = kNormal;
        bool const farStart = dStart > 7.0f, farEnd = dEnd > 7.0f;
        if (farStart || farEnd)
        {
            float c[3];
            if (dtStatusSucceed(q->closestPointOnPoly(endPoly, endPoint, c, nullptr)))
            {
                dtVcopy(endPoint, c);
                end = {c[2], c[0], c[1]};
            }
            type = kIncomplete | (farStart ? kFarStart : 0) | (farEnd ? kFarEnd : 0);
        }
        dtPolyRef polys[kMaxPolys];
        int n = 0;
        if (startPoly == endPoly)
        {
            polys[0] = startPoly;
            n = 1;
        }
        else
        {
            if (dtStatusFailed(q->findPath(startPoly, endPoly, startPoint, endPoint, &filter, polys, &n, kMaxPolys)) || !n)
                return kNoPath;
            type = (polys[n - 1] == endPoly && !(type & kIncomplete)) ? kNormal : kIncomplete;
            type |= (farStart ? kFarStart : 0) | (farEnd ? kFarEnd : 0);
        }
        std::vector<float> pts;
        bool failed, steep;
        Smooth(startPoint, endPoint, polys, std::uint32_t(n), pts, failed, steep);
        std::size_t const count = pts.size() / 3;
        if (n == 1 && count == 1 && !steep)
            return type;  // start + end
        if (count < 2 || failed)
        {
            if (count > 0 && steep)
            {
                end = {pts[(count - 1) * 3 + 2], pts[(count - 1) * 3], pts[(count - 1) * 3 + 1]};
                return type | kIncomplete;
            }
            end = to;
            return type | kNoPath;
        }
        if (count >= kMaxPoints)
            return type | kShort;
        end = {pts[(count - 1) * 3 + 2], pts[(count - 1) * 3], pts[(count - 1) * 3 + 1]};
        return type;
    }

    // Navmesh floor at (x, y) at or below zTop (stands in for Map::GetHeight(x, y, z + 50, vmap, 100)).
    bool FloorBelow(float x, float y, float zTop, float& z) const
    {
        float const c[3] = {y, zTop - 50.0f, x};
        float const ext[3] = {1.0f, 50.0f, 1.0f};
        dtPolyRef polys[64];
        int n = 0;
        q->queryPolygons(c, ext, &filter, polys, &n, 64);
        bool found = false;
        for (int i = 0; i < n; ++i)
        {
            float h;
            float const p[3] = {y, 0.0f, x};
            if (dtStatusSucceed(q->getPolyHeight(polys[i], p, &h)) && h <= zTop && (!found || h > z))
            {
                z = h;
                found = true;
            }
        }
        return found;
    }
};

// ---- MoveFarToIntentV2 replay (no combat, segment = arrive at its end) ------------------------------------
struct Outcome
{
    bool arrived = false;
    std::uint32_t segments = 0;
    std::uint32_t failures = 0;
    V3 at;
};

Outcome Replay(Gen const& gen, V3 bot, V3 goalIn, bool verticalSnap)
{
    using namespace TravelIntentPolicy;
    V3 dest = goalIn;
    if (verticalSnap)
        AutoWowVerticalSnap::Snap(*gen.q, goalIn.x, goalIn.y, goalIn.z, dest.x, dest.y, dest.z);
    Params const params{5, 90000, 20};
    Intent intent;
    Outcome o;
    for (int tick = 0; tick < 200; ++tick)
    {
        o.at = bot;
        if (Dist3(bot, goalIn) <= kInteract)
        {
            o.arrived = true;
            return o;
        }
        Point const goal = MakePoint(0, dest.x, dest.y, dest.z);
        Point const here = MakePoint(0, bot.x, bot.y, bot.z);
        (void)Observe(intent, goal, DistanceYards(here, goal), std::uint32_t(tick) * 3000u, params);
        // Final approach: MoveTo(dest) - the movement generator paths without the slope check.
        if (Dist3(bot, dest) < kPathFinderDis)
        {
            Gen flat = gen;
            flat.slopeCheck = false;
            V3 end;
            std::uint32_t const t = flat.Calculate(bot, dest, end);
            if ((t & ~kTypeOk) || Dist3(end, bot) < 1.0f)
                return o;  // MoveTo refused / no movement: the real mover would count toward no-progress
            bot = end;
            ++o.segments;
            continue;
        }
        bool committed = false;
        if (WalkingV2Policy::UseChunks(here, goal))
        {
            for (std::size_t k = 0; k < WalkingV2Policy::kChunkProbes && !committed; ++k)
            {
                Point const probe = WalkingV2Policy::ChunkProbe(here, goal, k);
                float pz = bot.z;
                gen.FloorBelow(float(probe.x), float(probe.y), bot.z + 50.0f, pz);
                V3 end;
                std::uint32_t const t = gen.Calculate(bot, {float(probe.x), float(probe.y), pz}, end);
                if (t & ~kTypeOk)
                    continue;
                Point const cand = MakePoint(0, end.x, end.y, end.z);
                if (WalkingV2Policy::AdmitChunk(here, cand, goal, false))
                {
                    Commit(intent, here, cand, DistanceYards(cand, goal));
                    bot = end;
                    committed = true;
                }
            }
        }
        if (!committed)
        {
            V3 end;
            std::uint32_t const t = gen.Calculate(bot, dest, end);
            Point const cand = MakePoint(0, end.x, end.y, end.z);
            if (!(t & ~kTypeOk) && Dist3(dest, end) + 5.0f < Dist3(bot, dest) &&
                Admit(intent, here, cand, DistanceYards(cand, goal), false, params))
            {
                Commit(intent, here, cand, DistanceYards(cand, goal));
                bot = end;
                committed = true;
            }
        }
        if (committed)
        {
            ++o.segments;
            CompleteSegment(intent);
            continue;
        }
        if (NoteFailure(intent, 0, params))
        {
            o.failures = intent.failures;
            return o;  // intent_replan_exhausted
        }
    }
    return o;
}
}  // namespace

int main(int argc, char** argv)
{
    gDataDir = argc > 1 ? argv[1] : "/root/p1data";
    std::string line;
    int passes = 0, offPasses = 0, cases = 0;
    while (std::getline(std::cin, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream in(line);
        std::uint32_t mapId;
        float sx, sy, gx, gy, gz;
        std::string label;
        in >> mapId >> sx >> sy >> gx >> gy >> gz >> label;
        dtNavMesh* mesh = LoadMap(mapId);
        if (!mesh)
        {
            std::printf("%-16s map %u: no mmap\n", label.c_str(), mapId);
            continue;
        }
        dtNavMeshQuery* query = dtAllocNavMeshQuery();
        query->init(mesh, 4096);
        Gen gen{query, {}, true};
        gen.filter.setIncludeFlags(AutoWowVerticalSnap::kIncludeFlags);
        gen.filter.setExcludeFlags(0);

        V3 snapped;
        bool const snapOk = AutoWowVerticalSnap::Snap(*query, gx, gy, gz, snapped.x, snapped.y, snapped.z);
        std::printf("%-16s map=%u goal=(%.1f,%.1f,%.1f) snap=%s(%.1f,%.1f,%.1f) d=%.1f\n", label.c_str(), mapId, gx, gy,
                    gz, snapOk ? "" : "NONE", snapped.x, snapped.y, snapped.z, snapOk ? Dist3({gx, gy, gz}, snapped) : 0.f);
        // Every navmesh floor under the logged (x, y).
        float const col[3] = {sy, 0.0f, sx};
        float const colExt[3] = {1.5f, 2000.0f, 1.5f};
        dtPolyRef polys[64];
        int n = 0;
        query->queryPolygons(col, colExt, &gen.filter, polys, &n, 64);
        for (int i = 0; i < n; ++i)
        {
            float h;
            float const p[3] = {sy, 0.0f, sx};
            if (dtStatusFailed(query->getPolyHeight(polys[i], p, &h)))
                continue;
            ++cases;
            V3 const start{sx, sy, h};
            Outcome const off = Replay(gen, start, {gx, gy, gz}, false);
            Gen flat = gen;
            flat.slopeCheck = false;
            Outcome const on = Replay(flat, start, {gx, gy, gz}, true);
            passes += on.arrived ? 1 : 0;
            offPasses += off.arrived ? 1 : 0;
            std::printf("  start z=%6.1f  OFF: %-8s segments=%-3u failures=%u at (%.0f,%.0f,%.0f)  |  ON: %-8s segments=%-3u "
                        "failures=%u at (%.0f,%.0f,%.0f) goal3d=%.1f\n",
                        h, off.arrived ? "ARRIVED" : "GAVE_UP", off.segments, off.failures, off.at.x, off.at.y, off.at.z,
                        on.arrived ? "ARRIVED" : "GAVE_UP", on.segments, on.failures, on.at.x, on.at.y, on.at.z,
                        Dist3(on.at, {gx, gy, gz}));
        }
        dtFreeNavMeshQuery(query);
    }
    std::printf("arrived: OFF %d / %d start floors, ON %d / %d\n", offPasses, cases, passes, cases);
    return 0;
}
