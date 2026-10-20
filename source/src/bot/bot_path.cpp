// ============================================================================
// bot_path.cpp - NAV route building, waypoint advance and portal geometry.
//
// Path building is a plain A* over the NAV mesh, but "am I there yet" is not:
// waypoints complete only when the engine says the bot is standing on the
// destination area, and a physically verified doorway additionally requires
// crossing the shared portal plane. The portal probes here supply the real
// hull-safe opening so steering can line up with it instead of the generator's
// arbitrary tile slice.
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "tier0/dbg.h"
#include "interface.h"
#include "eiface.h"
#include "edict.h"
#include "server_class.h"
#include "iservernetworkable.h"
#include "dlls/iplayerinfo.h"
#include "engine/iserverplugin.h"
#include "engine/IEngineTrace.h"
#include "gametrace.h"
#include "bspflags.h"
#include "const.h"
#include "convar.h"
#include "string_t.h"
#include "in_buttons.h"
#include "bot.h"
#include "nav.h"
#include "objective.h"
#include "profile.h"
#include "bot_internal.h"
#include "turret.h"

// ---- externs this TU needs (same pattern as bot.cpp / objective.cpp) -------
extern IVEngineServer       *engine;
extern IEngineTrace         *enginetrace;
extern IPlayerInfoManager   *playerinfomanager;
extern CGlobalVars          *gpGlobals;
extern CfgVar                insbot_avoid_jump;
extern CfgVar                insbot_avoid_crouch;
extern CfgVar                insbot_avoid_marked;
extern CfgVar                insbot_avoid_ladder;
extern CfgVar                insbot_ladder_up_penalty;
extern CfgVar                insbot_danger_avoid;
extern CfgVar                insbot_streaming;

void CInsBot::UpdatePathTo(const Vector &goal, bool force)
{
    if (!g_NavMesh.IsLoaded()) { m_pathResult = BOT_PATH_NONE; return; }
    // A typed ladder phase owns its route edge from approach through landing
    // clearance. On the shaft the origin is often off-mesh, while in a queue a
    // periodic route-diversity rebuild can otherwise swap out the edge and its
    // owner. Keep the path stable until physical completion advances it.
    if (m_ladderPhase != LADDER_PHASE_NONE && m_pathLen > 0 && !force) return;
    if (!force && Now() < m_repathTime) return;
    // Capture-committed bots repath a bit more often so they can react to a
    // firefight that just started near their route (the danger zones refresh
    // every frame; the path itself only needs a second or so to adapt).
    // Keep the proven 0.18.11 cadence on every mesh. The heap-based current A*
    // averages about 1ms/path on Sinjar; the former 2.4x large-map throttle
    // left a terrain mismatch stale for as long as 7.2s, while desynchronized
    // 1.5-3.0s rebuilds remain a small fraction of one frame.
    m_repathTime = Now() + frand(1.5f, 3.0f);

    int start = CurrentArea(m_info->GetAbsOrigin());
    int end   = g_NavMesh.FindArea(goal);
    // Objective centers are often slightly off the nav mesh (generated tiles
    // don't cover every brush). Route to the nearest tile instead of dropping
    // the path, which used to make the bot beeline straight at the point and
    // grind into a wall.
    if (end < 0) end = g_NavMesh.FindNearestArea(goal);
    if (start >= 0 && end >= 0
        && g_NavMesh.ComponentForArea(start) != g_NavMesh.ComponentForArea(end))
    {
        // A goal center can snap to a disconnected tile. Route to the nearest
        // reachable point instead of repeatedly failing the same disconnected
        // A* query.
        int reachable = g_NavMesh.FindClosestReachableArea(start, goal);
        if (reachable >= 0) end = reachable;
    }
    if (start < 0 || end < 0)
    {
        // off-mesh right now (spawn point not covered, or goal truly off the
        // mesh): fall back to the wall-aware direct walk and retry quickly.
        ResetDoorwayRunway();

        m_pathLen = 0;
        m_pathResult = BOT_PATH_NONE;
    m_pathContinues = false;
        m_pathCursor = 0;
        m_hasMoveTarget = false;
        m_repathTime = Now() + 0.2f;
        m_pathResult = BOT_PATH_OFFMESH;
        return;
    }

    // push the current jump/crouch avoidance strength into the pathfinder
    g_navJumpPenalty   = insbot_avoid_jump.GetFloat();
    g_navCrouchPenalty = insbot_avoid_crouch.GetFloat();
    g_navAvoidPenalty  = insbot_avoid_marked.GetFloat();
    g_navLadderPenalty   = insbot_avoid_ladder.GetFloat();
    g_navLadderUpPenalty = insbot_ladder_up_penalty.GetFloat();
    // Capture-committed bots route around active firefights (dynamic zones
    // refreshed each frame by CBotRoster::UpdateDangerZones).
    g_navDangerPenalty = insbot_danger_avoid.GetFloat();
    g_navDangerEnabled = m_captureCommit && g_navDangerPenalty > 0.0f;

    m_pathContinues = false;
    m_pathLen = g_NavMesh.BuildPath(start, end, m_path, INSBOT_MAX_PATH, m_pathSeed, &m_pathContinues);
    g_navDangerEnabled = false; // the flag only lives for this bot's BuildPath call
    if (m_pathLen <= 0)
    {
        // unreachable right now: retry soon instead of beelining for seconds
        m_pathCursor = 0;
        m_pathContinues = false;
        m_hasMoveTarget = false;
        m_repathTime = Now() + 0.2f;
        m_pathResult = BOT_PATH_UNREACHABLE;
        return;
    }
    m_pathCursor = (m_pathLen > 1) ? 1 : 0;
    m_hasMoveTarget = false;
    m_pathResult = BOT_PATH_VALID;
    // A rebuilt route may leave from a different edge. Do not let a nearly
    // coincident first waypoint inherit the previous route's travel heading.
    m_haveLastNavMoveYaw = false;
}

// Some generated NAV links intentionally cross a short floor gap/drop but the
// two area rectangles do not touch. The typed edge record classifies these as
// drops; expose the source-side takeoff and a conservative landing point so
// steering can line up with the real mouth instead of the landing center.
bool GetSeparatedTransitionPortal(int fromIdx, int toIdx,
                                         Vector &mid, Vector &axis,
                                         float &halfSpan, Vector *insideDest)
{
    Vector landing;
    Vector hintTakeoff, hintLanding;
    const bool hasHint = g_NavMesh.GetFixupTransition(
        fromIdx, toIdx, hintTakeoff, hintLanding);
    if (!g_NavMesh.GetTransitionGeometry(fromIdx, toIdx, mid, landing,
                                         axis, halfSpan))
    {
        if (!hasHint) return false;
        mid = hintTakeoff;
        landing = hintLanding;
        axis = landing - mid;
        float axisLen = sqrtf(axis.x * axis.x + axis.y * axis.y);
        if (axisLen < 0.5f) return false;
        axis.x /= axisLen; axis.y /= axisLen; axis.z = 0.0f;
        halfSpan = 16.0f;
    }
    else if (hasHint)
    {
        mid = hintTakeoff;
        landing = hintLanding;
        axis = landing - mid;
        float axisLen = sqrtf(axis.x * axis.x + axis.y * axis.y);
        if (axisLen >= 0.5f)
        {
            axis.x /= axisLen; axis.y /= axisLen; axis.z = 0.0f;
        }
    }
    if (insideDest) *insideDest = landing;
    return true;
}

bool PlayerInsideObjective(const ObjPoint *op, IPlayerInfo *info,
                                  const Vector &pos)
{
    if (!op || !info) return false;
    // The game capture code keys presence from the player's origin. Testing
    // any hull overlap made the bot declare capture presence 16-20u before its
    // origin actually entered the trigger, especially at a doorway edge.
    // Keep a tiny numeric tolerance, but require the origin itself to be inside
    // the real rectangular brush bounds.
    const float tolerance = 2.0f;
    return pos.x >= op->minX - tolerance && pos.x <= op->maxX + tolerance
        && pos.y >= op->minY - tolerance && pos.y <= op->maxY + tolerance
        && pos.z >= op->minZ - 8.0f && pos.z <= op->maxZ + 8.0f;
}

Vector ObjectiveHoldPoint(const ObjPoint *op, const Vector &pos,
                                 float angle, float scale)
{
    if (!op) return pos;
    // Keep a standing hull inside the real rectangular trigger. The old
    // radius was the brush half-diagonal, so narrow objectives could generate
    // hold points hundreds of units outside their capture volume.
    float halfX = (op->maxX - op->minX) * 0.5f - 20.0f;
    float halfY = (op->maxY - op->minY) * 0.5f - 20.0f;
    if (halfX < 0.0f) halfX = 0.0f;
    if (halfY < 0.0f) halfY = 0.0f;
    if (scale < 0.0f) scale = 0.0f;
    if (scale > 0.9f) scale = 0.9f;
    Vector out(op->center.x + cosf(angle) * halfX * scale,
               op->center.y + sinf(angle) * halfY * scale,
               pos.z);
    return out;
}

// Pick a hull-safe objective corner away from the bot's entry position. A bot
// that has only just crossed a doorway may already overlap the trigger; sending
// committed capture detail toward the opposite corner clears its hull from the
// throat before it is allowed to become stationary.
Vector ObjectiveCornerHoldPoint(const ObjPoint *op, const Vector &entry,
                                       int ordinal)
{
    if (!op) return entry;

    const float margin = 24.0f; // 16u standing hull half-width + clearance
    float minX = op->minX + margin, maxX = op->maxX - margin;
    float minY = op->minY + margin, maxY = op->maxY - margin;
    if (minX > maxX) minX = maxX = op->center.x;
    if (minY > maxY) minY = maxY = op->center.y;

    Vector corners[4] = {
        Vector(minX, minY, entry.z), Vector(maxX, minY, entry.z),
        Vector(maxX, maxY, entry.z), Vector(minX, maxY, entry.z)
    };
    int preferred = ordinal & 3;
    int best = preferred;
    float bestDist = -1.0f;
    for (int n = 0; n < 4; n++)
    {
        int i = (preferred + n) & 3;
        float dx = corners[i].x - entry.x, dy = corners[i].y - entry.y;
        float d2 = dx * dx + dy * dy;
        if (d2 > bestDist + 1.0f)
        {
            best = i;
            bestDist = d2;
        }
    }
    return corners[best];
}

// Pick a hold-facing direction that is physically usable. Corner holders
// naturally sit near two walls; blindly facing outward from the objective
// points them into one of those walls. Prefer the tactical yaw, but sample
// nearby alternatives at waist and eye height and turn toward open space when
// the preferred direction is obstructed.
float OpenHoldYaw(const Vector &pos, float preferredYaw)
{
    static const float offsets[] = {
        0.0f, -30.0f, 30.0f, -60.0f, 60.0f,
        -90.0f, 90.0f, -120.0f, 120.0f, 180.0f
    };
    CTraceFilterWorldAndPropsOnly filter;
    Ray_t ray;
    float bestYaw = preferredYaw;
    float bestScore = -1.0f;
    for (int i = 0; i < (int)(sizeof(offsets) / sizeof(offsets[0])); i++)
    {
        float yaw = preferredYaw + offsets[i];
        float yr = yaw * (3.14159265f / 180.0f);
        Vector dir(cosf(yr), sinf(yr), 0.0f);
        trace_t waistTrace, eyeTrace;
        Vector waist = pos + Vector(0, 0, 36.0f);
        Vector eye = pos + Vector(0, 0, 62.0f);
        ray.Init(waist, waist + dir * 320.0f);
        enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &waistTrace);
        ray.Init(eye, eye + dir * 320.0f);
        enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &eyeTrace);

        float waistClear = (waistTrace.startsolid || waistTrace.allsolid)
                         ? 0.0f : waistTrace.fraction;
        float eyeClear = (eyeTrace.startsolid || eyeTrace.allsolid)
                       ? 0.0f : eyeTrace.fraction;
        // Eye clearance dominates wall avoidance; a low obstacle at waist
        // height can still be useful cover. Small turn cost preserves the
        // preferred tactical direction when several directions are open.
        float score = waistClear * 0.35f + eyeClear * 0.65f
                    - fabsf(offsets[i]) / 720.0f;
        if (score > bestScore)
        {
            bestScore = score;
            bestYaw = yaw;
        }
    }
    return bestYaw;
}

// The second reported choke is the 25u corner immediately before the broad
// destination room. Depending on which of the two tiny source tiles FindArea picks,
// the active edge can be the short south step or either eastward exit.
// Exact FindArea selection can legitimately choose a third overlapping tile
// at a generated terrain seam. Keep the current path while the player's feet
// are still on either surface of its active edge; this prevents one ambiguous
// lookup from discarding and rebuilding an otherwise valid slope route.
static bool PositionOnAreaSurface(int areaIdx, const Vector &pos,
                                  bool requireLanding = false)
{
    if (requireLanding)
        return g_NavMesh.IsOnAreaLanding(areaIdx, pos, 8.0f);
    const NavAreaData *a = g_NavMesh.Area(areaIdx);
    if (!a || pos.x < a->lo.x - 24.0f || pos.x > a->hi.x + 24.0f
        || pos.y < a->lo.y - 24.0f || pos.y > a->hi.y + 24.0f)
        return false;
    float floorZ = g_NavMesh.GetZ(areaIdx, pos.x, pos.y);
    return floorZ <= pos.z + 64.0f && floorZ >= pos.z - 64.0f;
}

// Ladder ownership needs a stricter feet-on-floor test than ordinary terrain
// seam handling. The latter intentionally accepts an origin up to 48 units
// above a sloped tile; using it for a descending ladder would release the
// shaft while the bot was still several rungs above the lower landing.
bool PositionOnLadderLanding(int areaIdx, const Vector &pos)
{
    const NavAreaData *a = g_NavMesh.Area(areaIdx);
    if (!a || pos.x < a->lo.x || pos.x > a->hi.x
        || pos.y < a->lo.y || pos.y > a->hi.y)
        return false;
    float floorZ = g_NavMesh.GetZ(areaIdx, pos.x, pos.y);
    return fabsf(floorZ - pos.z) <= 8.0f;
}

// Generated slope tiles frequently overlap at their exact seam, so FindArea
// may select a third tile even after the bot has entered the planned
// destination. Accept a directed crossing of the exact (unmerged) portal only
// when the bot is physically inside/on that destination surface. This retains
// the old wall safety while preventing a crossed slope edge from pulling the
// bot back toward a stale waypoint.
static bool CrossedExactPortalIntoArea(int fromIdx, int toIdx,
                                       const Vector &pos)
{
    const NavAreaData *from = g_NavMesh.Area(fromIdx);
    const NavAreaData *to = g_NavMesh.Area(toIdx);
    if (!from || !to) return false;
    NavEdgeMeta meta;
    if (!g_NavMesh.GetEdgeMeta(fromIdx, toIdx, meta) || !meta.valid)
        return false;

    // A drop/jump is complete only when the feet are on the destination
    // surface. The old +/-64u surface window let a shallow stacked drop clear
    // while the bot was still standing on the upper floor.
    if (meta.kind == NAV_EDGE_DROP || meta.kind == NAV_EDGE_JUMP)
        return PositionOnAreaSurface(toIdx, pos, true);
    if (meta.kind != NAV_EDGE_WALK) return false;
    if (!PositionOnAreaSurface(toIdx, pos)) return false;

    float nx = meta.normalX;
    float ny = meta.normalY;
    float nl = sqrtf(nx * nx + ny * ny);
    if (nl < 0.5f) return false;
    nx /= nl; ny /= nl;
    float side = (pos.x - meta.midX) * nx
               + (pos.y - meta.midY) * ny;
    return side > 1.0f;
}

// Area centers on a long slope can differ by far more than jump height even
// though their shared floor seam is continuous. Only a real upward step at
// that seam should make ordinary waypoint following press +jump.
bool WalkEdgeNeedsJump(int fromIdx, int toIdx)
{
    if (!g_NavMesh.CanJumpFrom(fromIdx)) return false;
    Vector seam;
    if (!g_NavMesh.GetPortal(fromIdx, toIdx, seam)) return false;
    float fromZ = g_NavMesh.GetZ(fromIdx, seam.x, seam.y);
    float toZ = g_NavMesh.GetZ(toIdx, seam.x, seam.y);
    return toZ > fromZ + 16.0f;
}

struct PhysicalPortalProbe
{
    unsigned int mapHash;
    int fromIdx, toIdx;
    float sampleTime;
    unsigned int stateSignature;
    bool tight;
    float centerX, centerY, centerZ;
    float halfSpan;
};
static PhysicalPortalProbe s_physicalPortalCache[1024];

// Cheap hull trace used as a dynamic-state token for the cached probe. A
// closed door, a moving prop, or a destroyed breakable changes the hit entity,
// fraction, or end position and therefore invalidates the cached result.
static unsigned int PhysicalPortalStateSignature(int fromIdx, int toIdx)
{
    if (!enginetrace) return 0;
    const NavAreaData *from = g_NavMesh.Area(fromIdx);
    const NavAreaData *to = g_NavMesh.Area(toIdx);
    if (!from || !to) return 0;
    Vector portal;
    if (!g_NavMesh.GetPortal(fromIdx, toIdx, portal)) return 0;
    Vector travel = to->center - from->center;
    float len = sqrtf(travel.x * travel.x + travel.y * travel.y);
    if (len < 0.5f) return 0;
    Vector normal(travel.x / len, travel.y / len, 0.0f);
    Vector start = portal - normal * 56.0f;
    Vector end = portal + normal * 56.0f;
    start.z = g_NavMesh.GetZ(fromIdx, start.x, start.y) + 8.0f;
    end.z = g_NavMesh.GetZ(toIdx, end.x, end.y) + 8.0f;

    CTraceFilterWorldAndPropsOnly filter;
    Ray_t ray;
    trace_t tr;
    ray.Init(start, end, Vector(-16.0f, -16.0f, 0.0f), Vector(16.0f, 16.0f, 72.0f));
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &tr);

    unsigned int hash = 2166136261u;
    hash = (hash ^ (unsigned int)(size_t)tr.m_pEnt) * 16777619u;
    hash = (hash ^ (unsigned int)(int)(tr.fraction * 100000.0f)) * 16777619u;
    hash = (hash ^ (unsigned int)(int)(tr.endpos.x * 16.0f)) * 16777619u;
    hash = (hash ^ (unsigned int)(int)(tr.endpos.y * 16.0f)) * 16777619u;
    hash = (hash ^ (unsigned int)(int)(tr.endpos.z * 16.0f)) * 16777619u;
    hash = (hash ^ (unsigned int)tr.startsolid) * 16777619u;
    hash = (hash ^ (unsigned int)tr.allsolid) * 16777619u;
    return hash;
}

void InsBots_ResetPhysicalPortalCache()
{
    memset(s_physicalPortalCache, 0, sizeof(s_physicalPortalCache));
}

// Detect the actual hull-safe band through an active NAV portal. Generated
// meshes often split open ground into 25u tiles, so NAV width alone is not
// physical doorway evidence. Cheap waist+upper lateral traces first require
// wall-height geometry on both sides; only then do full 32x72 hull sweeps
// locate the usable center band. The shared cache keeps this bounded per
// map/edge, not per bot.
bool ProbePhysicalPortal(int fromIdx, int toIdx,
                                Vector *safeCenter, float *safeHalfSpan)
{
    if (!enginetrace) return false;
    const NavAreaData *from = g_NavMesh.Area(fromIdx);
    const NavAreaData *to = g_NavMesh.Area(toIdx);
    if (!from || !to) return false;

    unsigned int mapHash = 2166136261u;
    for (const char *p = g_Bots.m_mapname; p && *p; p++)
        mapHash = (mapHash ^ (unsigned char)*p) * 16777619u;
    unsigned int key = mapHash ^ ((unsigned int)fromIdx * 73856093u)
                               ^ ((unsigned int)toIdx * 19349663u);
    PhysicalPortalProbe &cached = s_physicalPortalCache[key & 1023u];
    float now = gpGlobals ? gpGlobals->curtime : 0.0f;
    const unsigned int stateSignature = PhysicalPortalStateSignature(fromIdx, toIdx);
    if (cached.mapHash == mapHash && cached.fromIdx == fromIdx
        && cached.toIdx == toIdx && now >= cached.sampleTime
        && now - cached.sampleTime <= 30.0f
        && cached.stateSignature == stateSignature)
    {
        if (cached.tight)
        {
            if (safeCenter)
                *safeCenter = Vector(cached.centerX, cached.centerY, cached.centerZ);
            if (safeHalfSpan) *safeHalfSpan = cached.halfSpan;
        }
        return cached.tight;
    }

    cached.mapHash = mapHash;
    cached.fromIdx = fromIdx;
    cached.toIdx = toIdx;
    cached.sampleTime = now;
    cached.stateSignature = stateSignature;
    cached.tight = false;
    cached.halfSpan = 0.0f;

    Vector portal;
    if (!g_NavMesh.GetPortal(fromIdx, toIdx, portal))
        return false;
    float lx = from->lo.x > to->lo.x ? from->lo.x : to->lo.x;
    float hx = from->hi.x < to->hi.x ? from->hi.x : to->hi.x;
    float ly = from->lo.y > to->lo.y ? from->lo.y : to->lo.y;
    float hy = from->hi.y < to->hi.y ? from->hi.y : to->hi.y;
    // Use the exact shared edge for the physical probe. Merged portals are
    // directional and can shift toward a neighboring terrain slice before any
    // physical doorway has actually been proven.
    Vector axis = ((hx - lx) <= (hy - ly))
                ? Vector(0.0f, 1.0f, 0.0f)
                : Vector(1.0f, 0.0f, 0.0f);
    float axisLen = sqrtf(axis.x * axis.x + axis.y * axis.y);
    if (axisLen < 0.5f) return false;
    axis.x /= axisLen; axis.y /= axisLen; axis.z = 0.0f;

    CTraceFilterWorldAndPropsOnly filter;
    Ray_t ray; trace_t tr;
    Vector waist = portal + Vector(0, 0, 44.0f);
    Vector upper = portal + Vector(0, 0, 68.0f);
    bool sideWall[2] = { false, false };
    for (int s = 0; s < 2; s++)
    {
        float sign = s ? -1.0f : 1.0f;
        Vector waistEnd = waist + axis * (sign * 128.0f);
        ray.Init(waist, waistEnd);
        enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &tr);
        float waistHit = (!tr.startsolid && !tr.allsolid)
                       ? tr.fraction * 128.0f : 9999.0f;
        Vector upperEnd = upper + axis * (sign * 128.0f);
        ray.Init(upper, upperEnd);
        enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &tr);
        float upperHit = (!tr.startsolid && !tr.allsolid)
                       ? tr.fraction * 128.0f : 9999.0f;
        // A real frame/wall blocks both heights at roughly the same lateral
        // depth. A hillside often intersects the waist ray only.
        sideWall[s] = waistHit < 127.9f && upperHit < 127.9f
                   && fabsf(waistHit - upperHit) <= 32.0f;
    }
    if (!sideWall[0] || !sideWall[1]) return false;

    Vector normal(axis.y, -axis.x, 0.0f);
    Vector travel = to->center - from->center;
    if (travel.x * normal.x + travel.y * normal.y < 0.0f)
    {
        normal.x = -normal.x;
        normal.y = -normal.y;
    }

    const int sampleCount = 25;
    const float sampleStep = 8.0f;
    const float sampleMin = -96.0f;
    bool clear[sampleCount];
    Vector mins(-16.0f, -16.0f, 0.0f);
    Vector maxs( 16.0f,  16.0f, 72.0f);
    for (int i = 0; i < sampleCount; i++)
    {
        float off = sampleMin + i * sampleStep;
        Vector candidate = portal + axis * off;
        Vector start = candidate - normal * 56.0f;
        Vector end = candidate + normal * 56.0f;
        start.z = g_NavMesh.GetZ(fromIdx, start.x, start.y) + 8.0f;
        end.z = g_NavMesh.GetZ(toIdx, end.x, end.y) + 8.0f;
        ray.Init(start, end, mins, maxs);
        enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &tr);
        clear[i] = !tr.startsolid && !tr.allsolid && tr.fraction >= 0.999f;
    }

    int bestFirst = -1, bestLast = -1;
    float bestScore = 3.4e38f;
    for (int i = 0; i < sampleCount; )
    {
        if (!clear[i]) { i++; continue; }
        int first = i;
        while (i + 1 < sampleCount && clear[i + 1]) i++;
        int last = i++;
        // A run touching either boundary is open terrain, not a doorway.
        if (first == 0 || last == sampleCount - 1) continue;
        float firstRun = sampleMin + first * sampleStep;
        float lastRun = sampleMin + last * sampleStep;
        float runCenter = (firstRun + lastRun) * 0.5f;
        float runSpan = lastRun - firstRun;
        float score = fabsf(runCenter) - runSpan * 0.10f;
        if (score < bestScore)
        {
            bestScore = score;
            bestFirst = first;
            bestLast = last;
        }
    }
    if (bestFirst < 0) return false;

    float firstRun = sampleMin + bestFirst * sampleStep;
    float lastRun = sampleMin + bestLast * sampleStep;
    float centerOff = (firstRun + lastRun) * 0.5f;
    float centerSpan = lastRun - firstRun;
    // The hull-center span excludes 16u on each side. A >=96u band means the
    // raw opening is at least 128u and does not need single-file treatment.
    if (centerSpan >= 96.0f) return false;

    cached.tight = true;
    Vector physicalCenter = portal + axis * centerOff;
    physicalCenter.z = g_NavMesh.GetZ(toIdx, physicalCenter.x, physicalCenter.y);
    cached.centerX = physicalCenter.x;
    cached.centerY = physicalCenter.y;
    cached.centerZ = physicalCenter.z;
    cached.halfSpan = (centerSpan + 32.0f) * 0.5f;
    if (safeCenter) *safeCenter = physicalCenter;
    if (safeHalfSpan) *safeHalfSpan = cached.halfSpan;
    return true;
}

// Source-style turning is a doorway tool, not a global movement mode. NAV
// portal width cannot identify a physical corridor on finely tessellated maps
// such as Sinjar, where ordinary open ground is built from 25u tiles. Use the
// cached BSP/door probes and require them to point along this active edge.
bool CInsBot::SourceTurnForEdge(int fromIdx, int toIdx, const Vector &pos) const
{
    if (fromIdx < 0 || toIdx < 0) return false;
    if (IsHadithaTightDoorEdge(fromIdx, toIdx)
        || IsHadithaEastDoorEdge(fromIdx, toIdx))
        return true;

    // Portal-relative hull sampling still works after a bot has aimed into a
    // jamb, where ChokeAhead rejects a center ray blocked inside 40u. Open
    // terrain is excluded because it lacks enclosing walls on both sides.
    Vector portal;
    bool havePortal = g_NavMesh.GetPortal(fromIdx, toIdx, portal);
    float ndx = 0.0f, ndy = 0.0f;
    if (havePortal) { ndx = portal.x - pos.x; ndy = portal.y - pos.y; }
    Vector physicalCenter;
    if (havePortal && ndx * ndx + ndy * ndy <= 360.0f * 360.0f
        && ProbePhysicalPortal(fromIdx, toIdx, &physicalCenter, 0))
    {
        float pdx = physicalCenter.x - pos.x;
        float pdy = physicalCenter.y - pos.y;
        if (pdx * pdx + pdy * pdy <= 320.0f * 320.0f)
            return true;
    }

    if (!m_chokeCached && !m_doorCached) return false;
    if (m_chokeCacheTime <= 0.0f || Now() > m_chokeCacheTime + 0.20f)
        return false;

    if (!havePortal)
        return false;

    float dx = portal.x - pos.x, dy = portal.y - pos.y;
    if (dx * dx + dy * dy > 240.0f * 240.0f) return false;
    float edgeYaw = atan2f(dy, dx) * (180.0f / 3.14159265f);
    float yawDiff = fmodf(edgeYaw - m_chokeCacheYaw, 360.0f);
    if (yawDiff > 180.0f) yawDiff -= 360.0f;
    if (yawDiff < -180.0f) yawDiff += 360.0f;
    return fabsf(yawDiff) <= 35.0f;
}

void CInsBot::RefreshRouteRank()
{
    m_routeRemaining = 3.4e38f;
    if (!m_inUse || !m_info || m_info->IsDead() || !g_NavMesh.IsLoaded()
        || m_pathLen <= 0 || m_pathCursor < 0 || m_pathCursor >= m_pathLen)
        return;

    Vector point = m_info->GetAbsOrigin();
    float total = 0.0f;
    int first = m_pathCursor > 0 ? m_pathCursor - 1 : m_pathCursor;
    for (int k = first; k + 1 < m_pathLen; k++)
    {
        Vector nextPoint, axis; float half = 0.0f;
        if (!g_NavMesh.GetPortalMerged(m_path[k], m_path[k + 1],
                                       nextPoint, axis, half)
            && !GetSeparatedTransitionPortal(m_path[k], m_path[k + 1],
                                             nextPoint, axis, half, 0))
        {
            const NavAreaData *a = g_NavMesh.Area(m_path[k + 1]);
            if (!a) continue;
            nextPoint = a->center;
        }
        float dx = point.x - nextPoint.x, dy = point.y - nextPoint.y;
        total += sqrtf(dx * dx + dy * dy);
        point = nextPoint;
    }
    m_routeRemaining = total;
}

bool CInsBot::TeammateBlockingPath(const Vector &pos, int toArea) const
{
    const NavAreaData *to = g_NavMesh.Area(toArea);
    if (!to || !engine || !playerinfomanager || !gpGlobals) return false;

    Vector target = to->center;
    int fromArea = CurrentArea(pos);
    Vector portal;
    if (g_NavMesh.GetPortal(fromArea, toArea, portal)) target = portal;

    float dx = target.x - pos.x, dy = target.y - pos.y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f) return false;
    dx /= len; dy /= len;
    float sx = -dy, sy = dx;
    int myTeam = m_info ? m_info->GetTeamIndex() : 0;

    for (int i = 1; i <= gpGlobals->maxClients; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (!e || e == m_edict || e->IsFree()) continue;
        IPlayerInfo *pi = playerinfomanager->GetPlayerInfo(e);
        if (!pi || !pi->IsConnected() || pi->IsDead() || pi->GetTeamIndex() != myTeam)
            continue;
        Vector p = pi->GetAbsOrigin();
        if (fabsf(p.z - pos.z) > 72.0f) continue;
        float rx = p.x - pos.x, ry = p.y - pos.y;
        float ahead = rx * dx + ry * dy;
        float lateral = fabsf(rx * sx + ry * sy);
        if (ahead > -8.0f && ahead < 112.0f && lateral < 56.0f)
            return true;
    }
    return false;
}

// True when a driveable vehicle hull is the first thing blocking the bot's
// route. Used by the stuck-link reporter: a vehicle is a transient obstacle,
// exactly like a door or a teammate, and must NOT be learned as a broken nav
// connection. A vehicle-caused stall used to call MarkBadConnection, which
// increments a squad-wide confidence counter; at confidence 3 the edge pays
// +500000 cost (nav.cpp:1445-1450) and stays effectively blocked for every
// bot long after the truck has driven away.
bool CInsBot::VehicleBlockingRoute(const Vector &pos, float moveYaw) const
{
    if (!enginetrace || !m_info) return false;

    // Aim at the next nav area's portal when there is one: that is the
    // direction the link's validity actually depends on. Fall back to the
    // movement heading.
    Vector target;
    bool haveTarget = false;
    if (m_pathLen > 0 && m_pathCursor < m_pathLen)
    {
        int fromArea = CurrentArea(pos);
        if (fromArea >= 0 &&
            g_NavMesh.GetPortal(fromArea, m_path[m_pathCursor], target))
            haveTarget = true;
    }
    float yaw = moveYaw;
    if (haveTarget)
        yaw = atan2f(target.y - pos.y, target.x - pos.x) * (180.0f / 3.14159265f);

    float yr = yaw * (3.14159265f / 180.0f);
    Vector dir(cosf(yr), sinf(yr), 0);

    // Same hull sweep the steering probes use, so the two agree on what
    // "blocked" means.
    CTraceFilterSteerObstacles filter( const_cast<CInsBot *>( this ) );
    Ray_t ray; trace_t tr;
    Vector from = pos + Vector(0, 0, 36);
    ray.Init(from, from + dir * 200.0f,
             Vector(-16.0f, -16.0f, 0.0f), Vector(16.0f, 16.0f, 72.0f));
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &tr);
    if (tr.fraction >= 1.0f || !tr.m_pEnt) return false;

    IServerUnknown *unk = (IServerUnknown *)tr.m_pEnt;
    IServerNetworkable *net = unk ? unk->GetNetworkable() : 0;
    ServerClass *sc = net ? net->GetServerClass() : 0;
    if (!sc || !IsDriveableClass( sc->GetName() )) return false;
    return true;
}

bool CInsBot::AdvanceWaypoint()
{
    if (m_pathLen <= 0 || m_pathCursor >= m_pathLen) return false;

    Vector pos = m_info->GetAbsOrigin();

    // A waypoint is complete only after the engine says the bot is physically
    // standing on that NAV area. The old 40-unit portal-radius test advanced the
    // cursor while a bot was still on the near side of a wall, so later failure
    // learning blamed the wrong (often non-adjacent) link.
    int actualArea = CurrentArea(pos);
    // Strict area membership is the base test. At a physically confirmed
    // doorway, require the bot to cross the shared portal plane while inside
    // the destination rectangle. Open terrain keeps the 0.18.11 membership
    // rule; tiny NAV tiles alone no longer opt every transition into this.
    bool sourceTurn = false;
    while (m_pathCursor < m_pathLen)
    {
        int toIdx = m_path[m_pathCursor];
        int reachedLadderIdx = -1; bool reachedLadderUp = false;
        bool reachedLadderEdge = m_pathCursor > 0
            && g_NavMesh.GetLadderConnection(m_path[m_pathCursor - 1], toIdx,
                                              reachedLadderIdx, reachedLadderUp);
        sourceTurn = m_pathCursor > 0
            && SourceTurnForEdge(m_path[m_pathCursor - 1], toIdx, pos);
        bool reached = false;
        bool havePortal = false;
        NavEdgeMeta edgeMeta;
        bool haveEdgeMeta = m_pathCursor > 0
            && g_NavMesh.GetEdgeMeta(m_path[m_pathCursor - 1], toIdx, edgeMeta);
        bool landingOnlyEdge = haveEdgeMeta && edgeMeta.valid
            && (edgeMeta.kind == NAV_EDGE_DROP || edgeMeta.kind == NAV_EDGE_JUMP);
        Vector pm, paxis; float ph;
        if (m_pathCursor > 0)
            havePortal = g_NavMesh.GetPortalMerged(m_path[m_pathCursor - 1],
                                                   toIdx, pm, paxis, ph);
        if (havePortal && sourceTurn)
        {
            const NavAreaData *toA = g_NavMesh.Area(toIdx);
            if (toA
                && pos.x >= toA->lo.x && pos.x <= toA->hi.x
                && pos.y >= toA->lo.y && pos.y <= toA->hi.y
                && pos.z >= toA->lo.z - 80.0f && pos.z <= toA->hi.z + 80.0f)
            {
                Vector n(paxis.y, -paxis.x, 0.0f);
                const NavAreaData *fromA = g_NavMesh.Area(m_path[m_pathCursor - 1]);
                if (fromA)
                {
                    Vector dc = toA->center - fromA->center;
                    if (dc.x * n.x + dc.y * n.y < 0.0f)
                    {
                        n.x = -n.x;
                        n.y = -n.y;
                    }
                    float side = (pos.x - pm.x) * n.x + (pos.y - pm.y) * n.y;
                    if (side > 0.0f)
                        reached = true;
                }
            }
        }
        else
        {
            if (landingOnlyEdge)
                reached = CrossedExactPortalIntoArea(m_path[m_pathCursor - 1],
                                                     toIdx, pos);
            else
                reached = (actualArea == toIdx)
                       || (m_pathCursor > 0
                           && CrossedExactPortalIntoArea(m_path[m_pathCursor - 1],
                                                         toIdx, pos));
        }
        // Typed ladder completion belongs to TraverseLadder's physical FSM.
        // FindArea may accept a landing surface while the feet are still up to
        // five units below it; never let that clear climb ownership early.
        if (reachedLadderEdge)
        {
            bool ladderPhysicallyComplete = m_ladderClimbIdx == reachedLadderIdx
                && m_ladderPhase != LADDER_PHASE_NONE
                && m_ladderCompletionReady;
            if (!ladderPhysicallyComplete) reached = false;
        }
        if (!reached) break;
        // Reaching the destination area of a ladder edge means the climb is
        // over; clear the persistent state so normal navigation resumes.
        if (reachedLadderEdge && m_ladderClimbIdx == reachedLadderIdx)
            ResetLadderTraversal(true);
        else if (m_ladderClimbing)
            ResetLadderTraversal(true);
        if (m_pathCursor > 0)
            g_NavMesh.MarkGoodConnection(m_path[m_pathCursor - 1], m_path[m_pathCursor]);
        m_pathCursor++;
        // Cursor identity is authoritative even when two dense/overlapping
        // portal points are less than the positional change threshold below.
        // The stuck sampler must start fresh for the newly active edge.
        m_navDestChanged = true;
    }
        if (m_pathCursor >= m_pathLen)
    {
        if (m_pathContinues)
        {
            // BuildPath returned a truncated prefix: the route continues past
            // the buffer, so repath immediately from the actual prefix
            // endpoint instead of treating the prefix end as the final goal.
            ResetDoorwayRunway();

            m_pathLen = 0;
            m_pathResult = BOT_PATH_NONE;
            m_pathCursor = 0;
            m_pathContinues = false;
            m_repathTime = 0.0f;
            m_hasMoveTarget = false;
            return false;
        }
        m_hasMoveTarget = false;
        return false;
    }

    sourceTurn = m_pathCursor > 0
        && SourceTurnForEdge(m_path[m_pathCursor - 1],
                             m_path[m_pathCursor], pos);

    // If physics pushed us onto an area outside the planned edge, discard the
    // stale path. UpdatePathTo will rebuild from the verified current area.
    int edgeLadder = -1; bool edgeUp = false;
    bool ladderEdge = m_pathCursor > 0
        && g_NavMesh.GetLadderConnection(m_path[m_pathCursor - 1],
                                         m_path[m_pathCursor], edgeLadder, edgeUp);
    // At a confirmed doorway, overlapping tiles that contain the bot's origin
    // do not invalidate the path; the portal-plane test owns the crossing.
    bool physicallyOnEdge = false;
    if (m_pathCursor > 0)
    {
        physicallyOnEdge = PositionOnAreaSurface(m_path[m_pathCursor - 1], pos)
                         || PositionOnAreaSurface(m_path[m_pathCursor], pos);
    }
    // While climbing, do not invalidate the path just because FindArea returns
    // a different area (the player origin on a ladder is not a walkable point).
    if (!m_ladderClimbing && actualArea >= 0 && m_pathCursor > 0
        && actualArea != m_path[m_pathCursor - 1] && !ladderEdge
        && !physicallyOnEdge)
    {
        ResetDoorwayRunway();

        m_pathLen = 0;
        m_pathResult = BOT_PATH_NONE;
    m_pathContinues = false;
        m_pathCursor = 0;
        m_repathTime = 0.0f;
        m_hasMoveTarget = false;
        return false;
    }

    // Aim at the PORTAL (shared edge) between the previous path area and the
    // current one - the natural crossing point through a doorway/gap - instead
    // of the area center. Falls back to the area center if no portal.
    Vector wp;
    bool haveWp = false;
    bool virtualWp = false;
    bool virtualApproach = false;
    Vector spreadAxis(0.0f, 0.0f, 0.0f);
    float spreadHalf = 0.0f;
    if (m_pathCursor >= 1)
    {
        Vector mergedWp;
        haveWp = g_NavMesh.GetPortalMerged(m_path[m_pathCursor - 1],
                                           m_path[m_pathCursor], mergedWp,
                                           spreadAxis, spreadHalf);
        if (haveWp)
        {
            // Ordinary terrain keeps the exact shared-edge midpoint used by
            // 0.18.11. GetPortalMerged is directional and shifts thousands of
            // Sinjar waypoints toward neighboring 25u slices; retain its span
            // only for physical doorway capacity/lane calculations.
            if (!g_NavMesh.GetPortal(m_path[m_pathCursor - 1],
                                     m_path[m_pathCursor], wp))
                wp = mergedWp;
        }
    }
    if (!haveWp && m_pathCursor >= 1)
    {
        NavTransitionWaypoint transition;
        if (g_NavMesh.GetTransitionWaypoint(m_path[m_pathCursor - 1],
                                            m_path[m_pathCursor], pos,
                                            transition))
        {
            virtualWp = true;
            haveWp = true;
            wp = transition.point;
            spreadAxis = transition.axis;
            spreadHalf = transition.halfSpan;
            virtualApproach = transition.virtualApproach;
        }
    }
    if (!haveWp)
    {
        const NavAreaData *a = g_NavMesh.Area(m_path[m_pathCursor]);
        if (!a)
        {
            ResetDoorwayRunway();

            m_pathLen = 0;
            m_pathResult = BOT_PATH_NONE;
            m_pathCursor = 0;
            m_pathContinues = false;
            return false;
        }
        wp = a->center;
        m_havePortalSpan = false;
        m_corridorAhead = false;
        m_corridorLatched = false;
        m_corridorAheadDist = 0.0f;
    }

    // Replace the nominal NAV midpoint/width with the full-hull-verified
    // physical opening. This centers a bot that approached off-axis before it
    // can scrape a jamb, and makes lane capacity reflect the real doorway
    // rather than the generator's arbitrary tile slice width.
    float portalDx = wp.x - pos.x, portalDy = wp.y - pos.y;
    if (haveWp && !virtualWp && m_pathCursor > 0
        && portalDx * portalDx + portalDy * portalDy <= 360.0f * 360.0f)
    {
        Vector safeCenter; float safeHalf = 0.0f;
        if (ProbePhysicalPortal(m_path[m_pathCursor - 1],
                                m_path[m_pathCursor],
                                &safeCenter, &safeHalf))
        {
            wp = safeCenter;
            spreadHalf = safeHalf;
        }
    }

    float dx = wp.x - pos.x, dy = wp.y - pos.y;
    // Open terrain keeps the old "step 48u into the destination" extension.
    // At a confirmed doorway, aim exactly at the portal plane and let the
    // crossing test advance the cursor after the hull enters the opening.
    if (!sourceTurn && !virtualWp
        && dx * dx + dy * dy < 40.0f * 40.0f && m_pathCursor > 0)
    {
        // Keep moving a short distance INSIDE the destination area until the
        // strict area lookup confirms the crossing. This avoids stopping on the
        // portal while also preventing a wall-side proximity false positive.
        const NavAreaData *next = g_NavMesh.Area(m_path[m_pathCursor]);
        if (next)
        {
            float ix = next->center.x - wp.x;
            float iy = next->center.y - wp.y;
            float ilen = sqrtf(ix * ix + iy * iy);
            if (ilen > 1.0f)
            {
                float step = ilen < 48.0f ? ilen : 48.0f;
                wp.x += ix * (step / ilen);
                wp.y += iy * (step / ilen);
                wp.z = g_NavMesh.GetZ(m_path[m_pathCursor], wp.x, wp.y);
            }
        }
    }

    // Spread same-direction bots across the opening instead of making every
    // bot converge on one point. Width-based lanes keep real hull clearance:
    //   < 72u  -> single file (all aim at the opening center)
    //   72-108 -> two lanes at +/-span/4  (36u+ apart)
    //   >=108  -> three lanes at -span/3, 0, +span/3 (36u+ apart)
    if (haveWp)
    {
        // Give the otherwise world-axis-aligned portal span a travel-relative
        // sign.  A sticky lane index must remain on the same physical side of
        // the route through a 90-degree turn; using raw +X/+Y axes swaps left
        // and right at half of all corners and makes adjacent lanes cross.
        Vector laneAxis = spreadAxis;
        if (m_pathCursor > 0)
        {
            const NavAreaData *fromA = g_NavMesh.Area(m_path[m_pathCursor - 1]);
            const NavAreaData *toA = g_NavMesh.Area(m_path[m_pathCursor]);
            if (fromA && toA)
            {
                float tx = toA->center.x - fromA->center.x;
                float ty = toA->center.y - fromA->center.y;
                float tl = sqrtf(tx * tx + ty * ty);
                if (tl > 1.0f)
                {
                    // left of travel = (-ty,+tx); positive lane offsets always
                    // mean route-left, negative offsets route-right.
                    float leftX = -ty / tl, leftY = tx / tl;
                    if (laneAxis.x * leftX + laneAxis.y * leftY < 0.0f)
                    {
                        laneAxis.x = -laneAxis.x;
                        laneAxis.y = -laneAxis.y;
                    }
                }
            }
        }
        m_portalAxis = laneAxis;
        m_portalHalfSpan = spreadHalf;
        m_havePortalSpan = true;

        if (insbot_streaming.GetInt() != 0)
        {
        // ---- corridor-approach prediction ----
        // Look ahead along the path: if the next few portals/areas form a
        // contiguous narrow run (a tight corridor), the column must merge to
        // single-file BEFORE the mouth. Doorways are isolated single narrow
        // portals and are already handled by the width classes below; a
        // corridor is a run of at least two narrow edges, and geometry probes
        // cannot see it from outside a mouth wider than ~72-80u.
        // Re-scan at ~7 Hz (path data only changes on repath, so a 150ms
        // window is plenty and saves the portal walk on every frame).
        if (Now() >= m_corridorScanTime)
        {
            m_corridorScanTime = Now() + 0.15f;
            m_corridorAhead = false;
            m_corridorAheadDist = 0.0f;
            int narrowRun = 0;
            bool firstEdgeNarrow = false;
            float travel = 0.0f;
            Vector scanPos = pos;
            int scan = (m_pathCursor > 0) ? m_pathCursor - 1 : m_pathCursor;
            for (int k = scan; k + 1 < m_pathLen && k - scan < 6; k++)
            {
                Vector pMid, pAxis; float pHalf;
                if (!g_NavMesh.GetPortalMerged(m_path[k], m_path[k + 1], pMid, pAxis, pHalf)
                    && !GetSeparatedTransitionPortal(m_path[k], m_path[k + 1],
                                                     pMid, pAxis, pHalf, 0))
                    break;   // ladder edge or ambiguous missing portal
                float sdx = pMid.x - scanPos.x, sdy = pMid.y - scanPos.y;
                travel += sqrtf(sdx * sdx + sdy * sdy);
                scanPos = pMid;

                // Streaming only engages for TIGHT corridors (<96u): narrower
                // than two 32u hulls can pass side-by-side. Wider passages
                // stay on legacy-style open movement.
                bool narrow = (pHalf * 2.0f) < 96.0f;
                if (!narrow)
                {
                    // A portal can be wide while the area behind it is a narrow
                    // corridor slice (common at corridor mouths). Measure that
                    // area's cross-section PERPENDICULAR to route travel. The
                    // old min(width,depth) test called a 50x500 open cross-tile
                    // a corridor even when the bot crossed its 50u dimension
                    // through a 500u-wide opening.
                    const NavAreaData *fa = g_NavMesh.Area(m_path[k]);
                    const NavAreaData *na = g_NavMesh.Area(m_path[k + 1]);
                    if (fa && na)
                    {
                        float w = na->hi.x - na->lo.x;
                        float d = na->hi.y - na->lo.y;
                        float tx = na->center.x - fa->center.x;
                        float ty = na->center.y - fa->center.y;
                        float tl = sqrtf(tx * tx + ty * ty);
                        if (tl > 1.0f)
                        {
                            float lateralWidth = fabsf(-ty / tl) * w
                                               + fabsf( tx / tl) * d;
                            if (lateralWidth < 96.0f) narrow = true;
                        }
                    }
                }

                if (k == scan) firstEdgeNarrow = narrow;

                if (narrow)
                {
                    if (narrowRun == 0) m_corridorAheadDist = travel;
                    narrowRun++;
                    if (narrowRun >= 2 && m_corridorAheadDist <= 500.0f)
                    {
                        m_corridorAhead = true;
                        m_corridorLatched = true;
                        break;
                    }
                }
                else if (narrowRun > 0)
                    break;   // isolated narrow edge = doorway, not a corridor
            }
            // Once a genuine run was detected, keep streaming through its last
            // remaining narrow edge. Requiring two FUTURE edges on every scan
            // switched mode 2 back to classic steering while bots were still
            // inside/crossing the corridor exit.
            if (!m_corridorAhead)
            {
                if (m_corridorLatched && firstEdgeNarrow)
                {
                    m_corridorAhead = true;
                    m_corridorAheadDist = 0.0f;
                }
                else
                    m_corridorLatched = false;
            }
        }

        }

        int streamingMode = insbot_streaming.GetInt();
        bool physicalCorridor = m_pathCursor > 0
            && SourceTurnForEdge(m_path[m_pathCursor - 1],
                                 m_path[m_pathCursor], pos);
        bool streamingActive = streamingMode == 1
            || (streamingMode == 2 && physicalCorridor);
        if (streamingActive)
        {
        int slot = (int)(this - g_Bots.m_bots);
        float span = spreadHalf * 2.0f;
        float off = 0.0f;
        // Sticky lanes: keep the same lane through a corridor of same-width
        // portals so bots stop weaving side-to-side (and crossing paths) at
        // every doorway. The lane only changes when the opening's width class
        // changes (single-file <-> two-lane <-> three-lane).
        int laneClass = (span >= 108.0f) ? 2 : (span >= 72.0f ? 1 : 0);
        // About to enter a tight corridor: force single-file at the mouth
        // even when the mouth itself is wide enough for lanes, so the column
        // forms one line instead of arriving side-by-side.
        if (streamingActive && m_corridorAhead && m_corridorAheadDist <= 320.0f)
            laneClass = 0;
        if (laneClass != m_laneClass)
        {
            m_laneClass = laneClass;
            if (laneClass == 2)      m_laneIndex = slot % 3;
            else if (laneClass == 1) m_laneIndex = slot & 1;
            else                     m_laneIndex = 0;
        }
        int lane = m_laneIndex;
        if (laneClass == 2)
            off = (float)(lane - 1) * (span / 3.0f);
        else if (laneClass == 1)
            off = lane ? span * 0.25f : -span * 0.25f;
        float maxOff = spreadHalf - 18.0f;
        if (maxOff < 0.0f) maxOff = 0.0f;
        if (off > maxOff) off = maxOff;
        else if (off < -maxOff) off = -maxOff;
        if (off != 0.0f)
        {
            wp.x += laneAxis.x * off;
            wp.y += laneAxis.y * off;
        }
        }
        // Keep the aim inside the owning area. A virtual drop approach belongs
        // to the source floor until the bot is committed across the gap; using
        // destination Z here made the approach point float below the ledge.
        const NavAreaData *destA = g_NavMesh.Area(m_path[m_pathCursor]);
        const NavAreaData *sourceA = m_pathCursor > 0
            ? g_NavMesh.Area(m_path[m_pathCursor - 1]) : 0;
        const NavAreaData *clampA = virtualApproach ? sourceA : destA;
        if (clampA)
        {
            if (wp.x < clampA->lo.x) wp.x = clampA->lo.x;
            else if (wp.x > clampA->hi.x) wp.x = clampA->hi.x;
            if (wp.y < clampA->lo.y) wp.y = clampA->lo.y;
            else if (wp.y > clampA->hi.y) wp.y = clampA->hi.y;
        }
        wp.z = g_NavMesh.GetZ(virtualApproach ? m_path[m_pathCursor - 1]
                                             : m_path[m_pathCursor],
                              wp.x, wp.y);
    }
    // ---- elevation-drop lookahead ----
    // If the waypoint sits notably below the bot's feet, the path drops off a
    // step/ledge (Haditha's stacked areas around (-2612,-83) drop ~15-30u
    // between connected tiles). Followers must hold at the edge until the
    // landing clears, otherwise they walk off onto a teammate below and stack.
    m_dropAhead = false;
    m_dropAheadDist = 0.0f;
    // Keep drop/jump queues active in both streaming modes. This is independent
    // of the removed Baghdad gap workaround and works with ordinary connected
    // NAV areas, including a mapper-added bridge/takeoff tile.
    int dropStreamingMode = insbot_streaming.GetInt();
    if (dropStreamingMode != 0 && !m_traversingLadder
        && wp.z < pos.z - 24.0f)
    {
        float dd = sqrtf((wp.x - pos.x) * (wp.x - pos.x)
                       + (wp.y - pos.y) * (wp.y - pos.y));
        if (dd < 64.0f)
        {
            m_dropAhead = true;
            m_dropAheadDist = dd < 40.0f ? 40.0f : dd;
        }
    }
    m_moveTarget = wp;
    m_hasMoveTarget = true;
    return true;
}
