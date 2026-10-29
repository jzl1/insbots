// ============================================================================
// bot_steer.cpp - movement steering, crowd flow and doorway queueing.
//
// The final movement stage: turn the chosen route into a usercmd. Two regimes
// live here. Open terrain faces the waypoint and moves at full speed; a
// BSP-confirmed doorway/choke instead uses a gradual arc plus a lane queue so
// a column drains single file instead of racing into the frame side by side.
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
#include "bot_internal.h"
#include "vehicle_steer_math.h"
#include "turret.h"     // g_Turrets vehicle index, IsDriveableClass
#include "profile.h"    // INSPROF_VEHICLE bucket for the new probe

// ---- externs this TU needs (same pattern as bot.cpp / objective.cpp) -------
extern IVEngineServer       *engine;
extern IEngineTrace         *enginetrace;
extern IPlayerInfoManager   *playerinfomanager;
extern CGlobalVars          *gpGlobals;
extern CfgVar                insbot_move_speed;
extern CfgVar                insbot_avoid_vehicles;
extern CfgVar                insbot_sprint;
extern CfgVar                insbot_streaming;
extern CfgVar                insbot_doorway_teleport;
extern CfgVar                insbot_doorway_teleport_time;
extern CfgVar                insbot_stuck_time;
extern CfgVar                insbot_debug;

enum
{
    PORTAL_TRANSIT_NONE = 0,
    PORTAL_TRANSIT_OWNER,
    PORTAL_TRANSIT_HOLD,
};

enum PortalTransitPhase
{
    PT_PHASE_NONE = 0,
    PT_PHASE_APPROACH,
    PT_PHASE_QUEUED,
    PT_PHASE_COMMITTED,
    PT_PHASE_LAND,
    PT_PHASE_CLEAR,
    PT_PHASE_RECOVERY,
};

struct PortalTransitSegment
{
    unsigned int key;
    int startEdge, endEdge;
    int startArea, endArea;
    Vector entry, exit, normal, exitNormal, axis;
    float entryHalf, exitHalf;
    bool dropEntry, dropExit, jumpEntry;
};

struct PortalTransitEdgeInfo
{
    int fromArea, toArea;
    Vector entry, exit, normal, axis;
    float halfSpan;
    bool drop, jump;
};

struct PortalTransitLease
{
    bool active;
    unsigned int key;
    int ownerSlot;          // roster slot + 1
    unsigned int ownerPathSerial;
    int startArea, endArea;
    float grantTime;
    float lastProgressTime;
    Vector progressPos, entry, exit;
};

struct PortalTransitBotState
{
    int phase;
    unsigned int key;
    bool segmentValid;
    PortalTransitSegment segment;
    unsigned int pathSerial;
    bool cacheValid;
    unsigned int cachePathSerial;
    int cacheCursor, cachePathLen;
    float cacheTime;
    PortalTransitSegment cached;
    float phaseTime;
    float lastProgressTime;
    Vector progressPos;
    float blockedUntil;
    float recoveryUntil;
    unsigned int lastCompletedKey;
    float completedUntil;
};

static PortalTransitLease s_portalLeases[96];
static PortalTransitBotState s_portalBots[64];

static float PortalHorizontalDist(const Vector &a, const Vector &b);
static bool PortalLandingClear(const PortalTransitSegment &seg, int ignoreSlot);
static void ReleasePortalTransitLease(PortalTransitLease *lease);

static const float PT_ACQUIRE_RADIUS  = 140.0f;
static const float PT_COMMIT_RADIUS   = 40.0f;
static const float PT_HOLD_BEHIND     = 72.0f;
static const float PT_QUEUE_GAP       = 58.0f;
static const float PT_GROUP_RADIUS    = 96.0f;
static const float PT_STAGE_RADIUS    = 104.0f;
static const float PT_QUEUE_WAIT_MAX  = 2.5f;
static const float PT_LEASE_IDLE_MAX  = 3.0f;
static const float PT_LEASE_LIFE_MAX  = 20.0f;
static const float PT_LANDING_RESERVE_RADIUS = 48.0f;
// Crowd grace removes player blocking; it must not also saturate movement.
// Running these 25u NAV seams at 3x overshoots the tile and lands bots on
// jamb ledges, so keep normal path speed and let the grace drain the column.
static const float PT_CROWD_SPEED_SCALE = 1.0f;
static const float PT_CROWD_APPROACH_SPEED_SCALE = 1.0f;

void InsBots_ResetPortalTransit()
{
    memset(s_portalLeases, 0, sizeof(s_portalLeases));
    memset(s_portalBots, 0, sizeof(s_portalBots));
}

void InsBots_ResetPortalTransitSlot(int slot)
{
    if (slot < 0 || slot >= 64) return;
    memset(&s_portalBots[slot], 0, sizeof(s_portalBots[slot]));
    for (int i = 0; i < 96; i++)
        if (s_portalLeases[i].active && s_portalLeases[i].ownerSlot == slot + 1)
            ReleasePortalTransitLease(&s_portalLeases[i]);
}

static float PortalHorizontalDist(const Vector &a, const Vector &b)
{
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    return sqrtf(dx * dx + dy * dy);
}

static void PortalClampToArea(int areaIdx, Vector &p, float inset)
{
    const NavAreaData *a = g_NavMesh.Area(areaIdx);
    if (!a) return;
    float lx = a->lo.x + inset, hx = a->hi.x - inset;
    float ly = a->lo.y + inset, hy = a->hi.y - inset;
    if (lx > hx) { lx = hx = (a->lo.x + a->hi.x) * 0.5f; }
    if (ly > hy) { ly = hy = (a->lo.y + a->hi.y) * 0.5f; }
    if (p.x < lx) p.x = lx; else if (p.x > hx) p.x = hx;
    if (p.y < ly) p.y = ly; else if (p.y > hy) p.y = hy;
    p.z = g_NavMesh.GetZ(areaIdx, p.x, p.y);
}

static Vector PortalEdgeAxis(const NavAreaData *from, const NavAreaData *to)
{
    float lx = from->lo.x > to->lo.x ? from->lo.x : to->lo.x;
    float hx = from->hi.x < to->hi.x ? from->hi.x : to->hi.x;
    float ly = from->lo.y > to->lo.y ? from->lo.y : to->lo.y;
    float hy = from->hi.y < to->hi.y ? from->hi.y : to->hi.y;
    float ox = hx - lx, oy = hy - ly;
    if (ox < 0.0f) ox = 0.0f;
    if (oy < 0.0f) oy = 0.0f;
    return (ox >= oy) ? Vector(1.0f, 0.0f, 0.0f) : Vector(0.0f, 1.0f, 0.0f);
}

// Resolve one NAV edge to its physical opening. Walk edges require a
// full-hull-verified finite band; drops use the typed transition geometry.
// No sidecar/fixup data is consulted here.
static bool GetTransitEdgeInfo(int fromIdx, int toIdx, PortalTransitEdgeInfo &out)
{
    memset(&out, 0, sizeof(out));
    const NavAreaData *from = g_NavMesh.Area(fromIdx);
    const NavAreaData *to = g_NavMesh.Area(toIdx);
    if (!from || !to) return false;

    out.fromArea = fromIdx;
    out.toArea = toIdx;

    NavEdgeMeta meta;
    bool haveMeta = g_NavMesh.GetEdgeMeta(fromIdx, toIdx, meta);
    if (haveMeta && meta.valid && meta.kind == NAV_EDGE_DROP)
    {
        // A mapper-authored transition is authoritative for the physical
        // mouth. Generated NAV overlap midpoints can sit several units away
        // from the real opening on malformed drop links.
        Vector hintTakeoff, hintLanding;
        if (g_NavMesh.GetFixupTransition(fromIdx, toIdx,
                                         hintTakeoff, hintLanding))
        {
            float dx = hintLanding.x - hintTakeoff.x;
            float dy = hintLanding.y - hintTakeoff.y;
            float len = sqrtf(dx * dx + dy * dy);
            if (len < 0.5f)
            {
                dx = to->center.x - from->center.x;
                dy = to->center.y - from->center.y;
                len = sqrtf(dx * dx + dy * dy);
            }
            if (len < 0.5f) return false;
            out.entry = hintTakeoff;
            out.exit = hintLanding;
            out.normal = Vector(dx / len, dy / len, 0.0f);
            out.axis = Vector(meta.axisX, meta.axisY, 0.0f);
            out.halfSpan = meta.halfSpan > 0.0f ? meta.halfSpan : 16.0f;
            out.drop = true;
            return true;
        }

        Vector takeoff, landing, axis;
        float halfSpan = 0.0f;
        if (!g_NavMesh.GetTransitionGeometry(fromIdx, toIdx, takeoff, landing,
                                             axis, halfSpan))
            return false;
        if (halfSpan <= 0.0f || halfSpan >= 40.0f) return false;
        float dx = landing.x - takeoff.x;
        float dy = landing.y - takeoff.y;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.5f)
        {
            dx = to->center.x - from->center.x;
            dy = to->center.y - from->center.y;
            len = sqrtf(dx * dx + dy * dy);
            if (len < 0.5f) return false;
        }
        out.entry = takeoff;
        out.exit = landing;
        out.normal = Vector(dx / len, dy / len, 0.0f);
        out.axis = axis;
        out.halfSpan = halfSpan;
        out.drop = true;
        return true;
    }

    if (haveMeta && meta.valid && meta.kind == NAV_EDGE_JUMP)
    {
        Vector center;
        Vector axis;
        float halfSpan = 0.0f;
        if (!g_NavMesh.GetPhysicalPortal(fromIdx, toIdx, center, axis,
                                         halfSpan))
        {
            center = Vector(meta.midX, meta.midY, meta.midZ);
            axis = Vector(meta.axisX, meta.axisY, 0.0f);
            halfSpan = meta.halfSpan;
        }
        if (halfSpan <= 0.0f || halfSpan >= 40.0f) return false;
        float nx = axis.y;
        float ny = -axis.x;
        float nlen = sqrtf(nx * nx + ny * ny);
        if (nlen < 0.5f) return false;
        nx /= nlen; ny /= nlen;
        Vector travel = to->center - from->center;
        if (travel.x * nx + travel.y * ny < 0.0f)
        {
            nx = -nx;
            ny = -ny;
        }
        out.entry = center;
        out.normal = Vector(nx, ny, 0.0f);
        out.exit = out.entry + out.normal * 40.0f;
        PortalClampToArea(toIdx, out.exit, 8.0f);
        out.axis = axis;
        out.halfSpan = halfSpan;
        out.jump = WalkEdgeNeedsJump(fromIdx, toIdx);
        return true;
    }

    Vector center;
    Vector physicalAxis;
    float halfSpan = 0.0f;
    Vector mergedAxis;
    bool haveMergedAxis = false;
    bool havePhysicalAxis = g_NavMesh.GetPhysicalPortal(
        fromIdx, toIdx, center, physicalAxis, halfSpan);
    if (!havePhysicalAxis)
    {
        // Some real 50u openings sit in an open room and fail the side-wall
        // probe. The merged NAV opening is still narrower than two 32u hulls;
        // accept it only when the generator marked an endpoint PRECISE, which
        // keeps ordinary 25u open-ground tessellation out of the queue.
        Vector mergedMid;
        float mergedHalf = 0.0f;
        if (g_NavMesh.GetPortalMerged(fromIdx, toIdx, mergedMid, mergedAxis,
                                      mergedHalf)
            && mergedHalf > 0.0f && mergedHalf < 32.0f)
        {
            bool precise = ((from->attributes & NAVATTR_PRECISE) != 0)
                        || ((to->attributes & NAVATTR_PRECISE) != 0);
            if (precise)
            {
                center = mergedMid;
                halfSpan = mergedHalf;
                haveMergedAxis = true;
            }
        }

        // A sidecar-verified narrow edge is authoritative when the generic
        // hull probe cannot resolve it. Use the raw typed edge geometry rather
        // than merging with neighboring 25u tiles.
        if (halfSpan <= 0.0f
            && haveMeta && meta.valid && meta.kind == NAV_EDGE_WALK
            && meta.halfSpan > 0.0f && meta.halfSpan < 40.0f
            && g_NavMesh.HasFixupEdge(NAV_FIXUP_TIGHT_EDGE, fromIdx, toIdx))
        {
            center = Vector(meta.midX, meta.midY, meta.midZ);
            halfSpan = meta.halfSpan;
            mergedAxis = Vector(meta.axisX, meta.axisY, 0.0f);
            haveMergedAxis = true;
        }
        if (halfSpan <= 0.0f) return false;
    }
    if ((halfSpan <= 0.0f || halfSpan >= 40.0f)
        && haveMeta && meta.valid && meta.kind == NAV_EDGE_WALK
        && meta.halfSpan > 0.0f && meta.halfSpan < 40.0f
        && g_NavMesh.HasFixupEdge(NAV_FIXUP_TIGHT_EDGE, fromIdx, toIdx))
    {
        center = Vector(meta.midX, meta.midY, meta.midZ);
        halfSpan = meta.halfSpan;
        mergedAxis = Vector(meta.axisX, meta.axisY, 0.0f);
        haveMergedAxis = true;
    }
    if (halfSpan <= 0.0f || halfSpan >= 40.0f) return false;

    float nx = 0.0f, ny = 0.0f;
    if (havePhysicalAxis)
    {
        nx = physicalAxis.y;
        ny = -physicalAxis.x;
    }
    else if (haveMergedAxis)
    {
        nx = mergedAxis.y;
        ny = -mergedAxis.x;
    }
    else
    {
        nx = to->center.x - from->center.x;
        ny = to->center.y - from->center.y;
    }
    float nlen = sqrtf(nx * nx + ny * ny);
    if (nlen < 0.5f) return false;
    nx /= nlen; ny /= nlen;
    Vector travel = to->center - from->center;
    if (travel.x * nx + travel.y * ny < 0.0f)
    {
        nx = -nx;
        ny = -ny;
    }
    out.entry = center;
    out.normal = Vector(nx, ny, 0.0f);
    out.exit = out.entry + out.normal * 40.0f;
    PortalClampToArea(toIdx, out.exit, 8.0f);
    out.axis = havePhysicalAxis ? physicalAxis
             : haveMergedAxis ? mergedAxis : PortalEdgeAxis(from, to);
    out.halfSpan = halfSpan;
    if (haveMeta && meta.valid && meta.kind == NAV_EDGE_WALK)
        out.jump = WalkEdgeNeedsJump(fromIdx, toIdx);
    return true;
}

static bool PortalEdgesSameOpening(const PortalTransitEdgeInfo &a,
                                   const PortalTransitEdgeInfo &b)
{
    if (fabsf(a.entry.z - b.entry.z) > 80.0f) return false;
    if (PortalHorizontalDist(a.entry, b.entry) > PT_GROUP_RADIUS) return false;
    float dot = a.normal.x * b.normal.x + a.normal.y * b.normal.y;
    return dot >= 0.5f;
}

static unsigned int PortalQuantize(float v)
{
    return (unsigned int)(int)floorf(v * 4.0f + 0.5f);
}

static unsigned int PortalEdgeHash(const PortalTransitEdgeInfo &e)
{
    unsigned int h = 2166136261u;
    h = (h ^ PortalQuantize(e.entry.x)) * 16777619u;
    h = (h ^ PortalQuantize(e.entry.y)) * 16777619u;
    h = (h ^ PortalQuantize(e.entry.z)) * 16777619u;
    h = (h ^ PortalQuantize(e.exit.x)) * 16777619u;
    h = (h ^ PortalQuantize(e.exit.y)) * 16777619u;
    h = (h ^ PortalQuantize(e.exit.z)) * 16777619u;
    h = (h ^ (unsigned int)(e.drop ? 1 : 0)) * 16777619u;
    h = (h ^ (unsigned int)(e.jump ? 1 : 0)) * 16777619u;
    return h;
}

// Build one physical transit segment around the active edge. Contiguous
// single-file edges are merged only while their hull-verified openings are
// collinear and close together, so the several 25u NAV tiles that describe one
// doorway share one key while separate doorways stay separate.
static bool BuildPortalTransitSegment(const int *path, int pathLen, int cursor,
                                      PortalTransitSegment &out)
{
    memset(&out, 0, sizeof(out));
    if (!path || pathLen <= 0 || cursor <= 0 || cursor >= pathLen) return false;

    PortalTransitEdgeInfo active;
    if (!GetTransitEdgeInfo(path[cursor - 1], path[cursor], active))
        return false;

    PortalTransitEdgeInfo before[16];
    int beforeCount = 0;
    int startEdge = cursor - 1;
    while (startEdge > 0 && beforeCount < 16)
    {
        PortalTransitEdgeInfo prev;
        if (!GetTransitEdgeInfo(path[startEdge - 1], path[startEdge], prev)) break;
        if (!PortalEdgesSameOpening(active, prev)) break;
        before[beforeCount++] = prev;
        startEdge--;
    }

    PortalTransitEdgeInfo after[16];
    int afterCount = 0;
    int endEdge = cursor - 1;
    while (endEdge + 1 < pathLen && afterCount < 16)
    {
        PortalTransitEdgeInfo next;
        if (!GetTransitEdgeInfo(path[endEdge], path[endEdge + 1], next)) break;
        if (!PortalEdgesSameOpening(active, next)) break;
        after[afterCount++] = next;
        endEdge++;
    }

    PortalTransitEdgeInfo all[33];
    int count = 0;
    for (int i = beforeCount - 1; i >= 0; i--)
        all[count++] = before[i];
    all[count++] = active;
    for (int i = 0; i < afterCount; i++)
        all[count++] = after[i];

    out.startEdge = startEdge;
    out.endEdge = endEdge;
    out.startArea = path[startEdge];
    out.endArea = path[endEdge + 1];
    out.entry = all[0].entry;
    out.exit = all[count - 1].exit;
    out.normal = all[0].normal;
    out.exitNormal = all[count - 1].normal;
    out.axis = all[0].axis;
    out.entryHalf = all[0].halfSpan;
    out.exitHalf = all[count - 1].halfSpan;
    out.dropEntry = all[0].drop;
    out.dropExit = all[count - 1].drop;
    out.jumpEntry = all[0].jump;

    // Commutative sum/mix: opposite directions through the same opening get
    // the same lease key, while duplicate slice hashes do not XOR-cancel back
    // to the FNV offset (which would merge every two-slice doorway).
    unsigned int keySum = 0;
    unsigned int keyMix = 0;
    for (int i = 0; i < count; i++)
    {
        unsigned int eh = PortalEdgeHash(all[i]);
        keySum += eh;
        keyMix ^= eh + 0x9e3779b9u + (eh << 6) + (eh >> 2);
    }
    out.key = 2166136261u ^ keySum ^ keyMix
            ^ ((unsigned int)count * 0x85ebca6bu);
    return true;
}

static PortalTransitLease *FindPortalTransitLease(unsigned int key)
{
    for (int i = 0; i < 96; i++)
        if (s_portalLeases[i].active && s_portalLeases[i].key == key)
            return &s_portalLeases[i];
    return 0;
}

static PortalTransitLease *AllocatePortalTransitLease(unsigned int key)
{
    for (int i = 0; i < 96; i++)
    {
        if (!s_portalLeases[i].active)
        {
            memset(&s_portalLeases[i], 0, sizeof(s_portalLeases[i]));
            s_portalLeases[i].active = true;
            s_portalLeases[i].key = key;
            return &s_portalLeases[i];
        }
    }
    return 0;
}

static void ReleasePortalTransitLease(PortalTransitLease *lease)
{
    if (lease) memset(lease, 0, sizeof(*lease));
}

static bool PortalLeaseIsStale(const PortalTransitLease &lease, float now)
{
    int ownerSlot = lease.ownerSlot - 1;
    if (ownerSlot < 0 || ownerSlot >= 64) return true;

    CInsBot *owner = &g_Bots.m_bots[ownerSlot];
    if (!owner->m_inUse || !owner->m_info || owner->m_info->IsDead())
        return true;

    const PortalTransitBotState &ownerState = s_portalBots[ownerSlot];
    if (ownerState.phase < PT_PHASE_APPROACH
        || ownerState.phase > PT_PHASE_CLEAR)
        return true;
    if (ownerState.pathSerial != lease.ownerPathSerial)
        return true;

    // An owner that stops running the transit update (route completed, mode
    // changed, etc.) must not reserve the opening forever.
    if (now - lease.lastProgressTime > PT_LEASE_IDLE_MAX
        || now - lease.grantTime > PT_LEASE_LIFE_MAX)
        return true;

    // CLEAR is only bookkeeping. Once the owner is physically beyond the
    // landing, any waiter may take the opening immediately.
    if (ownerState.phase == PT_PHASE_CLEAR)
    {
        Vector p = owner->m_info->GetAbsOrigin();
        float dx = p.x - lease.exit.x;
        float dy = p.y - lease.exit.y;
        float out = dx * ownerState.segment.exitNormal.x
                  + dy * ownerState.segment.exitNormal.y;
        if (out >= 24.0f || PortalHorizontalDist(p, lease.exit) >= 36.0f)
            return true;
    }
    return false;
}

static bool PortalPathContainsSegment(const int *path, int pathLen, int cursor,
                                      int startArea, int endArea)
{
    if (!path || pathLen <= 0 || cursor <= 0 || cursor >= pathLen) return false;
    int lo = cursor - 20;
    if (lo < 0) lo = 0;
    int hi = cursor + 2;
    if (hi >= pathLen) hi = pathLen - 1;
    for (int i = lo; i <= hi; i++)
    {
        if (path[i] != startArea) continue;
        for (int j = i + 1; j < pathLen && j <= i + 24; j++)
        {
            if (path[j] != endArea) continue;
            int active = cursor - 1;
            if (active >= i - 1 && active <= j - 1) return true;
        }
    }
    return false;
}

// Pick a staging point on the already-planned approach path.  Queue distance
// is measured backward from the portal entry, so bots form a column along
// known walkable NAV centers instead of all backing into the same lip tile.
static bool PortalQueueTarget(const PortalTransitSegment &seg, const int *path,
                              int pathLen, int startEdge, float behindDist,
                              Vector &out)
{
    Vector target = seg.entry;
    if (!path || pathLen <= 0 || startEdge < 0 || startEdge >= pathLen)
        return false;

    float accumulated = 0.0f;
    Vector cursor = seg.entry;
    for (int i = startEdge; i >= 0; i--)
    {
        const NavAreaData *area = g_NavMesh.Area(path[i]);
        if (!area) continue;
        Vector next = area->center;
        float dx = next.x - cursor.x;
        float dy = next.y - cursor.y;
        float dz = next.z - cursor.z;
        float len = sqrtf(dx * dx + dy * dy + dz * dz);
        if (len < 0.5f) continue;
        if (accumulated + len >= behindDist)
        {
            float t = (behindDist - accumulated) / len;
            out = cursor + Vector(dx * t, dy * t, dz * t);
            return true;
        }
        accumulated += len;
        cursor = next;
    }
    return false;
}

// Repaths often begin at the portal's source area, leaving no approach history
// in m_path. Build a small staging tree from connected NAV areas on the near
// side instead; their centers are already verified walkable.
static bool PortalNeighborQueueTarget(const PortalTransitSegment &seg, int rank,
                                      Vector &out)
{
    int frontier[128];
    int visited[128];
    int frontierCount = 0, visitedCount = 0;
    frontier[frontierCount++] = seg.startArea;
    visited[visitedCount++] = seg.startArea;

    int candidates[32];
    float candidateDist[32];
    int candidateCount = 0;

    for (int head = 0; head < frontierCount && head < 128; head++)
    {
        int areaIdx = frontier[head];
        const NavAreaData *area = g_NavMesh.Area(areaIdx);
        if (!area) continue;
        Vector d = area->center - seg.entry;
        float behind = d.x * seg.normal.x + d.y * seg.normal.y;
        float dist = sqrtf(d.x * d.x + d.y * d.y);
        if (behind <= -8.0f && dist <= 320.0f && candidateCount < 32)
        {
            int at = candidateCount;
            while (at > 0 && dist < candidateDist[at - 1])
            {
                candidateDist[at] = candidateDist[at - 1];
                candidates[at] = candidates[at - 1];
                at--;
            }
            candidateDist[at] = dist;
            candidates[at] = areaIdx;
            candidateCount++;
        }

        for (int c = 0; c < area->connCount && frontierCount < 128; c++)
        {
            int next = area->connIdx[c];
            if (next == seg.endArea || next < 0) continue;
            const NavAreaData *nextArea = g_NavMesh.Area(next);
            if (!nextArea) continue;
            Vector nd = nextArea->center - seg.entry;
            if (nd.x * seg.normal.x + nd.y * seg.normal.y > -8.0f) continue;
            bool seen = false;
            for (int v = 0; v < visitedCount; v++)
                if (visited[v] == next) { seen = true; break; }
            if (seen) continue;
            visited[visitedCount++] = next;
            frontier[frontierCount++] = next;
        }
    }

    if (candidateCount <= 0) return false;
    int pick = rank + 1;
    if (pick >= candidateCount) pick = candidateCount - 1;
    const NavAreaData *area = g_NavMesh.Area(candidates[pick]);
    if (!area) return false;
    out = area->center;
    return true;
}

static bool PortalLandingClear(const PortalTransitSegment &seg, int ignoreSlot)
{
    for (int i = 0; i < 64; i++)
    {
        if (i == ignoreSlot) continue;
        CInsBot *other = &g_Bots.m_bots[i];
        if (!other->m_inUse || !other->m_info || other->m_info->IsDead()) continue;
        Vector p = other->m_info->GetAbsOrigin();
        float dz = fabsf(p.z - seg.exit.z);
        if (dz > 110.0f) continue;
        float dist = PortalHorizontalDist(p, seg.exit);
        if (dist > 44.0f) continue;
        float dx = p.x - seg.exit.x;
        float dy = p.y - seg.exit.y;
        float out = dx * seg.exitNormal.x + dy * seg.exitNormal.y;
        if (out >= 32.0f) continue;
        bool onLanding = g_NavMesh.IsOnAreaLanding(seg.endArea, p, 16.0f);
        if (onLanding) return false;
        // A bot just beyond a tiny 25u landing tile must not hold the lease
        // forever. Only a hull actually overlapping the landing surface blocks.
        if (dz <= 16.0f && dist < 28.0f)
            return false;
    }
    return true;
}

static bool PortalLandingReservedByOther(const PortalTransitSegment &seg,
                                         unsigned int key)
{
    for (int i = 0; i < 96; i++)
    {
        const PortalTransitLease &lease = s_portalLeases[i];
        if (!lease.active || lease.key == key) continue;
        if (fabsf(lease.exit.z - seg.exit.z) > 110.0f) continue;
        if (PortalHorizontalDist(lease.exit, seg.exit) < PT_LANDING_RESERVE_RADIUS) return true;
    }
    return false;
}

static void PortalTrackProgress(PortalTransitBotState &st, const Vector &pos,
                                float now)
{
    if (st.lastProgressTime <= 0.0f)
    {
        st.lastProgressTime = now;
        st.progressPos = pos;
        return;
    }
    float dx = pos.x - st.progressPos.x;
    float dy = pos.y - st.progressPos.y;
    float dz = pos.z - st.progressPos.z;
    if (dx * dx + dy * dy + dz * dz >= 8.0f * 8.0f)
    {
        st.lastProgressTime = now;
        st.progressPos = pos;
    }
}

static void PortalResetBotState(PortalTransitBotState &st)
{
    unsigned int lastKey = st.lastCompletedKey;
    float completedUntil = st.completedUntil;
    float blockedUntil = st.blockedUntil;
    memset(&st, 0, sizeof(st));
    st.lastCompletedKey = lastKey;
    st.completedUntil = completedUntil;
    st.blockedUntil = blockedUntil;
}

// ---------------------------------------------------------------------------
// Trace filter for steering obstacle detection.
//
// Skips the bot itself, all same-team players, and any seated vehicle riders,
// while keeping WORLD GEOMETRY, driveable vehicles (CPropVehicleDriveable) and
// enemy bodies on foot solid, so path steering reacts to vehicle hulls instead
// of walking into them.
//
// Everything else (crates, barrels, sandbags, dropped weapons, ragdolls) is
// deliberately NOT an obstacle: the steering layer must not deflect around
// clutter that the engine lets a player hull shove past. CTraceFilter inherits
// TRACE_EVERYTHING (IEngineTrace.h:54-61), which always traces BSP world
// geometry and passes each dynamic entity through ShouldHitEntity; the brush
// case therefore never reaches this filter and needs no test here.
//
// NOTE ON SCOPE. "False for everything else" and "an enemy on foot must remain
// solid" cannot both hold literally for players. The player case below is the
// resolution: players are the one non-vehicle entity kept solid, because the
// verified baseline shows bots deflecting around a player entity (verification
// §7, `blocked by ent=1 cls='player' frac=0.984`), because Change 4 names it as
// a preservation requirement, and because dropping it would send bots walking
// into enemy bodies. Delete the final two lines of SteerShouldHitEntity to get
// the strictly vehicle-only predicate.
// ---------------------------------------------------------------------------
// The single source of truth for "is this trace entity a driveable Humvee".
// Same mechanism turret.cpp uses: GetServerClass()->GetName() returns the C++
// class name "CPropVehicleDriveable", not the map-facing classname. Both live
// Humvees and destroyed wrecks keep this class (the wreck is the same entity,
// still physically solid), so wrecks are caught by the same test.
static bool SteerIsDriveableClass( const char *cls )
{
    return cls && strcmp( cls, "CPropVehicleDriveable" ) == 0;
}

// True only for the dynamic entities steering should treat as obstacles.
static bool SteerShouldHitEntity( IHandleEntity *pEntity )
{
    if (!pEntity) return false;
    IServerUnknown *unk = (IServerUnknown *)pEntity;
    IServerNetworkable *net = unk->GetNetworkable();
    if (!net) return false;

    ServerClass *sc = net->GetServerClass();
    if (sc && SteerIsDriveableClass( sc->GetName() ))
        return true;

    ICollideable *col = unk->GetCollideable();
    if (col && col->GetSolid() == SOLID_BSP)
        return true;

    // Enemy on foot stays solid; same-team players and any seated riders were
    // already removed by the skip list in ShouldHitEntity. GetClassName() is
    // the map-facing name ("player"), which is what the STEER_DEFLECT log
    // prints, and differs from GetServerClass()->GetName().
    const char *cls = net->GetClassName();
    return cls && strcmp( cls, "player" ) == 0;
}

static void BuildSteerSkipSet( CInsBot *bot, CTraceFilterSteerObstacles &filter )
{
    if (!bot || !bot->m_edict) return;

    IServerUnknown *selfUnk = bot->m_edict->GetUnknown();
    if (selfUnk) filter.Add( (IHandleEntity *)selfUnk );

    // Per-frame client classification. The old code walked every client and
    // called playerinfomanager->GetPlayerInfo() on each one, twice per bot per
    // frame (SteerRoam + SteerToPoint): ~2,048 GetPlayerInfo calls/frame at 32
    // bots. The team/seat state it needs changes at most once per frame, so it
    // is cached here and rebuilt only when gpGlobals->framecount advances.
    //
    // The cache is team-relative, so it cannot alias the all-teams
    // s_tracePlayerHandles list in bot_perceive.cpp. Semantics are preserved
    // exactly: self + same-team players + any-team seated riders are skipped;
    // an enemy on foot stays solid.
    //
    // Buckets are keyed by team index (0 = unassigned, 1, 2). ALL buckets are
    // rebuilt in the same pass: bots of both teams call into this function
    // interleaved within one frame, so a single "current team" cache would
    // miss and rebuild on nearly every call.
    static int            s_classFrame = -1;
    static int            s_classCount[3] = { 0, 0, 0 };
    static IHandleEntity *s_classSkip[3][66];

    int myTeam = bot->m_info ? bot->m_info->GetTeamIndex() : 0;
    int frame = gpGlobals ? gpGlobals->framecount : -1;
    if (frame != s_classFrame)
    {
        s_classFrame = frame;
        s_classCount[0] = s_classCount[1] = s_classCount[2] = 0;

        int maxClients = gpGlobals ? gpGlobals->maxClients : 32;
        if (maxClients > 66) maxClients = 66;

        for (int i = 1; i <= maxClients; i++)
        {
            edict_t *e = engine->PEntityOfEntIndex( i );
            if (!e || e->IsFree()) continue;
            IServerUnknown *unk = e->GetUnknown();
            if (!unk) continue;

            // Seated riders are skipped by every team: the vehicle hull, not
            // the rider, is what steering should react to.
            ICollideable *col = unk->GetCollideable();
            if (col && col->GetCollisionGroup() == COLLISION_GROUP_IN_VEHICLE)
            {
                for (int t = 0; t < 3; t++)
                    if (s_classCount[t] < 66)
                        s_classSkip[t][s_classCount[t]++] = (IHandleEntity *)unk;
                continue;
            }

            if (!playerinfomanager) continue;
            IPlayerInfo *info = playerinfomanager->GetPlayerInfo( e );
            if (!info) continue;
            int t = info->GetTeamIndex();
            if (t < 0 || t > 2) continue;
            if (s_classCount[t] < 66)
                s_classSkip[t][s_classCount[t]++] = (IHandleEntity *)unk;
        }
    }

    int bucket = (myTeam >= 0 && myTeam <= 2) ? myTeam : 0;
    for (int i = 0; i < s_classCount[bucket]; i++)
        filter.Add( s_classSkip[bucket][i] );
}

static inline edict_t *EdictFromTraceEnt( CBaseEntity *ent )
{
    if (!ent) return 0;
    IServerUnknown *unk = (IServerUnknown *)(IHandleEntity *)ent;
    IServerNetworkable *net = unk ? unk->GetNetworkable() : 0;
    return net ? net->GetEdict() : 0;
}

CTraceFilterSteerObstacles::CTraceFilterSteerObstacles( CInsBot *bot )
    : m_n(0)
{
    BuildSteerSkipSet( bot, *this );
}

bool CTraceFilterSteerObstacles::ShouldHitEntity( IHandleEntity *pEntity,
                                                  int /*contentsMask*/ )
{
    if (!pEntity) return false;
    for (int i = 0; i < m_n; i++)
        if (pEntity == m_skip[i]) return false;
    return SteerShouldHitEntity( pEntity );
}

static void Cmd_InsBot_Place()
{
    // Test scaffolding. insbot_place performs no ground trace, nav-area lookup,
    // bounds check, or teleport bookkeeping, so it can put a bot inside
    // geometry or off the map. Server-side only (rcon/server console), but it
    // must not ship unrestricted: gate it behind the debug switch.
    if (!insbot_debug.GetBool())
    {
        Msg("insbot_place is disabled; enable it with: insbot set debug 1\n");
        return;
    }

    int argc = engine->Cmd_Argc();
    if (argc < 6)
    {
        Msg("usage: insbot_place <slot> <x> <y> <z> <yaw>\n");
        return;
    }
    int slot = atoi(engine->Cmd_Argv(1));
    float x = (float)atof(engine->Cmd_Argv(2));
    float y = (float)atof(engine->Cmd_Argv(3));
    float z = (float)atof(engine->Cmd_Argv(4));
    float yaw = (float)atof(engine->Cmd_Argv(5));
    if (slot < 0 || slot >= 64 || !g_Bots.m_bots[slot].m_inUse || !g_Bots.m_bots[slot].m_ctrl)
    {
        Msg("invalid bot slot %d\n", slot);
        return;
    }
    CInsBot *b = &g_Bots.m_bots[slot];
    Vector pos(x, y, z);
    QAngle ang(0, yaw, 0);
    b->m_ctrl->SetAbsOrigin(pos);
    b->m_ctrl->SetAbsAngles(ang);
    Msg("[insbots] placed bot slot %d (%s) at (%.1f %.1f %.1f) yaw=%.1f\n",
        slot, b->m_name, x, y, z, yaw);
}
static ConCommand insbot_place_cmd("insbot_place", Cmd_InsBot_Place, "Place a bot at coordinates: insbot_place <slot> <x> <y> <z> <yaw>");

static void Cmd_InsBot_SteerStatus()
{
    // Pure diagnostic output; gated with the same switch so a release server
    // does not carry steering internals (positions, speeds, team ids).
    if (!insbot_debug.GetBool())
    {
        Msg("insbot_steer_status is disabled; enable it with: insbot set debug 1\n");
        return;
    }

    for (int i = 0; i < 64; i++)
    {
        CInsBot *b = &g_Bots.m_bots[i];
        if (!b->m_inUse || !b->m_info || b->m_info->IsDead()) continue;
        Vector pos = b->m_info->GetAbsOrigin();
        QAngle ang = b->m_info->GetAbsAngles();
        int myIdx = b->m_edict ? engine->IndexOfEdict(b->m_edict) : 0;
        float speed = (myIdx >= 1 && myIdx < 66) ? s_hearSpeed[myIdx] : 0.0f;
        Msg("[STEER_STAT] slot=%d (%s) team=%d pos=(%.1f %.1f %.1f) yaw=%.1f speed=%.1f\n",
            i, b->m_name, b->m_info->GetTeamIndex(), pos.x, pos.y, pos.z, ang.y, speed);
    }
}
static ConCommand insbot_steer_status_cmd("insbot_steer_status", Cmd_InsBot_SteerStatus, "Print steering status of all alive bots");

// Turn a non-combat bot toward its movement direction at a finite
// body-friendly rate. INS multiplies human turn input by cl_anglespeedkey
// (default 0.67) while +sprint is held; bots submit final angles, so while
// sprinting they turn faster and ease off the throttle through the corner.
//
// The important part for cornering feel: the WISH direction is steered at the
// target immediately using view-relative fwd/side, instead of holding
// fwd=speed along the current view. Holding fwd along the old view while the
// camera rotates slowly is what made bots slide wide ("butter") around nav
// corners; strafing the wish vector toward the waypoint makes the body cut the
// corner while the head turns at a human-like rate.
static void SmoothNavTurn(float targetYaw, float speed, QAngle &view,
                          float &fwd, float &side, float &remainingAngle,
                          bool sourceTurn)
{
    float turn = fmodf(targetYaw - view.y, 360.0f);
    if (turn > 180.0f) turn -= 360.0f;
    if (turn < -180.0f) turn += 360.0f;
    if (turn != turn) turn = 0.0f;
    remainingAngle = fabsf(turn);

    // Open terrain uses the proven 0.18.11 controller: face the waypoint and
    // move directly at full speed. The gradual arc below is reserved for a
    // BSP-confirmed doorway/choke where overshoot is the actual problem.
    if (!sourceTurn)
    {
        view.y = targetYaw;
        view.x = 0.0f;
        fwd = speed;
        side = 0.0f;
        remainingAngle = 0.0f;
        return;
    }

    // INS applies cl_anglespeedkey (0.67) to sprinting turn INPUT. Fake
    // clients submit final angles, so we compensate with a higher rate. With
    // vector steering the body already heads at the target, so the sprint
    // multiplier mostly controls how fast the camera catches up.
    bool sprintTurn = insbot_sprint.GetBool() && speed > 250.0f;
    float turnRate = 300.0f * (sprintTurn ? 2.5f : 1.0f);
    float maxStep = turnRate * (gpGlobals ? gpGlobals->interval_per_tick : 0.015f);
    if (turn > maxStep) turn = maxStep;
    else if (turn < -maxStep) turn = -maxStep;
    view.y += turn;
    view.x = 0.0f;

    // Ease off the throttle while the remaining turn is large so the bot
    // holds tight corners. Keep at least 70% speed so the sprint button
    // condition (fwd > 250 at the default 400 move speed) stays satisfied
    // through gentler turns.
    float turnFrac = remainingAngle > 50.0f
                   ? (remainingAngle - 50.0f) / 130.0f : 0.0f;
    if (turnFrac > 0.3f) turnFrac = 0.3f;
    float scale = speed * (1.0f - turnFrac);
    // Movement follows the gradually rotating view, carving a controlled arc
    // through the confirmed opening instead of cutting across its frame.
    fwd = scale;
    side = 0.0f;
}

void CInsBot::SteerRoam(float &fwd, float &side, int &buttons, QAngle &view)
{
    if (m_edict)
    {
        IServerUnknown *unk = m_edict->GetUnknown();
        ICollideable *col = unk ? unk->GetCollideable() : 0;
        if (col && col->GetCollisionGroup() == COLLISION_GROUP_IN_VEHICLE)
            return;
    }

    Vector pos = m_info->GetAbsOrigin();
    float now = Now();

    if (now >= m_nextHeadingChange)
    {
        m_nextHeadingChange = now + frand(3.0f, 8.0f);
        m_headingYaw += frand(-60.0f, 60.0f);
    }

    // probe ahead at waist height
    float yawRad = m_headingYaw * (3.14159265f / 180.0f);
    Vector dir(cosf(yawRad), sinf(yawRad), 0);
    Vector waist = pos; waist.z += 36.0f;

    CTraceFilterSteerObstacles steerFilter( this );
    Ray_t ray; trace_t tr;

    Vector ahead = waist + dir * 96.0f;
    ray.Init(waist, ahead);
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &steerFilter, &tr);

    if (tr.fraction < 1.0f)
    {
        // Blocked: test left/right 60 degrees from the EPISODE base heading.
        // Measuring from m_headingYaw would add another 60 degrees on every
        // re-probe, because the previous deflection already rotated it - that
        // is the 6-value spin cycle fixed in SteerToPoint by 97f2a8a.
        // SteerRoam carried the same structure on this branch and is fixed
        // the same way.
        bool baseFresh = (now - m_steerBaseTime) <= 0.75f
                      && m_steerLastYaw == m_headingYaw;
        if (!baseFresh)
        {
            m_steerBaseYaw = m_headingYaw;
            m_steerLastYaw = m_headingYaw;
        }
        float baseYaw = m_steerBaseYaw;

        float best = baseYaw + 120.0f; float bestFrac = -1.0f;
        for (int s = -1; s <= 1; s += 2)
        {
            float y = baseYaw + 60.0f * s;
            float yr = y * (3.14159265f / 180.0f);
            Vector d2(cosf(yr), sinf(yr), 0);
            ray.Init(waist, waist + d2 * 96.0f);
            trace_t t2;
            enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &steerFilter, &t2);
            if (t2.fraction > bestFrac) { bestFrac = t2.fraction; best = y; }
        }
        while (best > 180.0f) best -= 360.0f;
        while (best < -180.0f) best += 360.0f;
        m_headingYaw    = best;
        m_steerBaseYaw  = baseYaw;
        m_steerBaseTime = now;
        m_steerLastYaw  = best;

        if (insbot_debug.GetBool() && tr.m_pEnt)
        {
            edict_t *hitEdict = EdictFromTraceEnt(tr.m_pEnt);
            BotDbg("[insbots] [STEER_DEFLECT] %s (Roam) pos=(%.1f %.1f %.1f) "
                   "blocked by ent=%d cls='%s' frac=%.3f -> deflecting to bestYaw=%.1f\n",
                   m_name, pos.x, pos.y, pos.z,
                   hitEdict ? engine->IndexOfEdict(hitEdict) : 0,
                   hitEdict ? hitEdict->GetClassName() : "world",
                   tr.fraction, best);
        }

        // low obstacle? try a jump
        Vector knee = pos; knee.z += 20.0f;
        Vector head = pos; head.z += 58.0f;
        ray.Init(knee, knee + dir * 48.0f);
        trace_t tKnee;
        enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &steerFilter, &tKnee);
        ray.Init(head, head + dir * 48.0f);
        trace_t tHead;
        enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &steerFilter, &tHead);
        if (tKnee.fraction < 1.0f && tHead.fraction >= 1.0f)
            buttons |= IN_JUMP;
    }

    SmoothNavTurn(m_headingYaw, insbot_move_speed.GetFloat(), view,
                  fwd, side, m_navTurnAngle, false);
}

// Escape-episode state for SteerToPoint, one entry per bot slot.
//
// The shoulders are always measured from the heading the bot held when the
// blocked episode BEGAN, never from the heading a previous deflection already
// moved it to. Measuring from the deflected heading adds another 60 degrees on
// every re-probe, and that is precisely the 6-value spin cycle the verifier
// measured (177.4 -> 117.4 -> 57.4 -> -2.6 -> -62.6 -> -122.6 -> 177.4) at
// speed 0, with 1,516 STEER_DEFLECT lines in ~10 s. Anchoring on the episode
// base makes the same two escape headings get re-measured every hold, so a
// wedged bot keeps picking the SAME side instead of rotating, while a bot that
// has one genuinely open shoulder still follows it and moves on.
//
// No-path fallback used by MoveToward when the nav mesh is loaded but no
// route can be built (off-mesh spawn/goal, or a point that is unreachable
// right now). Heads at the destination, but probes world geometry so the bot
// slides along walls instead of grinding into them, and re-biases to the
// direct heading as soon as the line to the goal opens up again.
void CInsBot::SteerToPoint(float destYaw, float speed, float &fwd, float &side,
                           int &buttons, QAngle &view)
{
    if (m_edict)
    {
        IServerUnknown *unk = m_edict->GetUnknown();
        ICollideable *col = unk ? unk->GetCollideable() : 0;
        if (col && col->GetCollisionGroup() == COLLISION_GROUP_IN_VEHICLE)
            return;
    }

    Vector pos = m_info->GetAbsOrigin();
    float now = Now();

    // Episode bookkeeping for the shoulder probes. baseFresh is true only when
    // the previous call left the bot holding the same heading within one hold,
    // i.e. this is a continuation of the SAME blocked episode.
    bool baseFresh = (now - m_steerBaseTime) <= 0.75f
                  && m_steerLastYaw == m_headingYaw;

    float yawRad = m_headingYaw * (3.14159265f / 180.0f);
    Vector dir(cosf(yawRad), sinf(yawRad), 0);
    Vector waist = pos; waist.z += 36.0f;

    CTraceFilterSteerObstacles steerFilter( this );
    Ray_t ray; trace_t tr;
    ray.Init(waist, waist + dir * 96.0f);
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &steerFilter, &tr);
    bool blocked = tr.startsolid || tr.allsolid || tr.fraction < 1.0f;

    if (blocked)
    {
        // Re-measure the two shoulders from the episode base heading. Only
        // re-choose when the previous hold has expired, so the bot actually
        // holds each committed heading long enough to move or to be sampled as
        // stuck. The old code rewrote m_headingYaw EVERY frame to whichever
        // 60-degree shoulder was momentarily least-bad; with both shoulders
        // blocked that produced the 6-value spin at speed 0 and never let the
        // no-progress sampler reach insbot_stuck_time.
        float baseYaw = baseFresh ? m_steerBaseYaw : m_headingYaw;
        if (!baseFresh)
        {
            m_steerBaseYaw = m_headingYaw;
            m_steerLastYaw = m_headingYaw;
            baseYaw = m_headingYaw;
        }

        // The FIRST probe of an episode must run immediately: m_nextHeadingChange
        // may still hold a stale 3-8 s gate set by SteerRoam's roam-heading
        // randomizer, and honoring it here would let the bot grind into the
        // obstacle without ever deflecting. Within an episode the gate still
        // throttles re-probes to one per hold.
        bool allowProbe = !baseFresh || (now >= m_nextHeadingChange);
        if (allowProbe)
        {
            // test the two 60-degree shoulders and take the more open one
            float best = baseYaw + 120.0f; float bestFrac = -1.0f;
            for (int s = -1; s <= 1; s += 2)
            {
                float y = baseYaw + 60.0f * s;
                float yr = y * (3.14159265f / 180.0f);
                Vector d2(cosf(yr), sinf(yr), 0);
                ray.Init(waist, waist + d2 * 96.0f);
                trace_t t2;
                enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &steerFilter, &t2);
                if (t2.fraction > bestFrac) { bestFrac = t2.fraction; best = y; }
            }
            while (best > 180.0f) best -= 360.0f;
            while (best < -180.0f) best += 360.0f;
            if (tr.m_pEnt)
            {
                edict_t *hitEdict = EdictFromTraceEnt(tr.m_pEnt);
                const char *cls = hitEdict ? hitEdict->GetClassName() : "world";
                int entIdx = hitEdict ? engine->IndexOfEdict(hitEdict) : 0;
                int myIdx = m_edict ? engine->IndexOfEdict(m_edict) : 0;
                float curSpeed = (myIdx >= 1 && myIdx < 66) ? s_hearSpeed[myIdx] : 0.0f;
                BotDbg("[insbots] [STEER_DEFLECT] %s (Point) pos=(%.1f %.1f %.1f) speed=%.1f blocked by ent=%d cls='%s' frac=%.3f -> deflecting to bestYaw=%.1f\n",
                       m_name, pos.x, pos.y, pos.z, curSpeed, entIdx, cls, tr.fraction, best);
            }
            m_headingYaw = best;
            m_nextHeadingChange = now + 0.45f;   // hold the deflection briefly
        }
        m_steerBaseYaw  = baseYaw;
        m_steerBaseTime = now;
        m_steerLastYaw  = m_headingYaw;
    }
    else
    {
        // Forward line is clear: the blocked episode is over. Invalidate the
        // base so the next episode re-captures it from the live heading.
        m_steerBaseTime = -1000.0f;

        if (now >= m_nextHeadingChange)
        {
            // current line open: probe the direct line to the destination and
            // re-bias toward it whenever the wall between them is gone
            float dy = destYaw * (3.14159265f / 180.0f);
            Vector dd(cosf(dy), sinf(dy), 0);
            ray.Init(waist, waist + dd * 96.0f);
            trace_t td;
            enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &steerFilter, &td);
            if (td.fraction >= 1.0f)
            {
                m_headingYaw = destYaw;
                m_nextHeadingChange = now + 0.45f;
            }
        }
    }

    SmoothNavTurn(m_headingYaw, speed, view, fwd, side, m_navTurnAngle, false);
}

// Convert a desired world-space movement yaw into Source's view-relative
// forward/side command pair.  Combat bots keep aiming at their enemy while the
// queue moves them along (or back along) the actual navigation lane.
void MoveAtWorldYaw(float worldYaw, float viewYaw, float speed,
                           float &fwd, float &side)
{
    float rel = (worldYaw - viewYaw) * (3.14159265f / 180.0f);
    fwd = cosf(rel) * speed;
    side = -sinf(rel) * speed;
}
bool CInsBot::UpdateCrowdTransitGrace(const Vector &pos, float now)
{
    int mySlot = (int)(this - g_Bots.m_bots);
    if (!g_NavMesh.IsLoaded() || m_pathLen <= 0 || m_pathCursor <= 0
        || m_pathCursor >= m_pathLen)
    {
        if (m_crowdTransitGraceActive && now < m_crowdTransitHoldUntil)
            return true;
        EndCrowdTransitGrace();
        return false;
    }

    PortalTransitSegment seg;
    Vector crowdEntry(0, 0, 0);
    bool haveTightSegment = false;
    float bestEntryDist = 3.4e38f;
    // A crowd has to become nonblocking before it compresses into the mouth,
    // not only after every hull has reached the active edge. Scan a short
    // path-local window for a genuinely narrow raw edge and activate within a
    // local approach radius. Wide drops and open-room NAV tessellation never
    // qualify, regardless of their edge type.
    int scanEnd = m_pathCursor + 16;
    if (scanEnd >= m_pathLen) scanEnd = m_pathLen - 1;
    for (int look = m_pathCursor; look <= scanEnd; look++)
    {
        PortalTransitEdgeInfo edge;
        if (!GetTransitEdgeInfo(m_path[look - 1], m_path[look], edge))
            continue;
        if (edge.halfSpan <= 0.0f || edge.halfSpan >= 32.0f)
            continue;
        float d = PortalHorizontalDist(pos, edge.entry);
        if (d > 1000.0f || d >= bestEntryDist)
            continue;

        PortalTransitSegment candidate;
        if (!BuildPortalTransitSegment(m_path, m_pathLen, look, candidate))
            continue;
        seg = candidate;
        crowdEntry = edge.entry;
        bestEntryDist = d;
        haveTightSegment = true;
    }
    // Dense groups need collision grace even between named portals. A stacked
    // spawn or a column compressing toward the next choke otherwise spends the
    // whole approach shoving hulls instead of making forward progress.
    int nearbyCrowd = 1;
    for (int i = 0; i < 64; i++)
    {
        if (i == (int)(this - g_Bots.m_bots)) continue;
        CInsBot *other = &g_Bots.m_bots[i];
        if (!other->m_inUse || !other->m_info || other->m_info->IsDead())
            continue;
        if (other->m_info->GetTeamIndex() != m_info->GetTeamIndex())
            continue;
        Vector op = other->m_info->GetAbsOrigin();
        float dx = op.x - pos.x;
        float dy = op.y - pos.y;
        if (dx * dx + dy * dy <= 400.0f * 400.0f
            && fabsf(op.z - pos.z) <= 120.0f)
            nearbyCrowd++;
    }
    if (!haveTightSegment && nearbyCrowd < 4)
    {
        if (m_crowdTransitGraceActive && now < m_crowdTransitHoldUntil)
            return true;
        EndCrowdTransitGrace();
        return false;
    }

    m_crowdTransitHoldUntil = now + 4.0f;
    if (!m_crowdTransitOwnsCollision && !m_spawnNoCollideActive
        && !HasBodyNoCollide() && !BeginCrowdTransitGrace())
        return false;
    m_crowdTransitGraceActive = true;
    return true;
}

int CInsBot::UpdatePortalTransit(const Vector &pos, float viewYaw,
                                 float &fwd, float &side, int &buttons)
{
    int mySlot = (int)(this - g_Bots.m_bots);
    if (mySlot < 0 || mySlot >= 64) return PORTAL_TRANSIT_NONE;
    if (!m_inUse || !m_info || m_info->IsDead()) return PORTAL_TRANSIT_NONE;

    float now = Now();
    PortalTransitBotState &st = s_portalBots[mySlot];

    PortalTransitSegment seg;
    bool haveSegment = false;
    bool cacheHit = st.cacheValid
                 && st.cachePathSerial == m_pathSerial
                 && st.cacheCursor == m_pathCursor
                 && st.cachePathLen == m_pathLen
                 && now < st.cacheTime + 0.35f;
    if (cacheHit)
    {
        seg = st.cached;
        haveSegment = true;
    }
    else
    {
        haveSegment = BuildPortalTransitSegment(m_path, m_pathLen, m_pathCursor,
                                                seg);
        st.cacheValid = haveSegment;
        st.cachePathSerial = m_pathSerial;
        st.cacheCursor = m_pathCursor;
        st.cachePathLen = m_pathLen;
        st.cacheTime = now;
        if (haveSegment) st.cached = seg;
    }

    // Once the owner has crossed, its path cursor may leave the portal edge
    // before the hull leaves the landing. Keep the stored segment authoritative
    // through landing and clearance so a path rebuild cannot release the gate
    // underneath the owner and drop the next bot onto the same tile.
    if (!haveSegment && st.segmentValid
        && (st.phase == PT_PHASE_LAND
            || st.phase == PT_PHASE_CLEAR
            || st.phase == PT_PHASE_RECOVERY)
        && PortalHorizontalDist(pos, st.segment.exit) < 300.0f)
    {
        seg = st.segment;
        haveSegment = true;
    }

    // After the owner crosses the last slice its cursor advances to a wide
    // edge; keep the cached segment until CLEAR releases it.
    if (!haveSegment && st.phase != PT_PHASE_NONE && st.segmentValid
        && st.pathSerial == m_pathSerial
        && m_pathCursor >= st.segment.startEdge
        && m_pathCursor <= st.segment.endEdge + 2
        && PortalHorizontalDist(pos, st.segment.exit) < 300.0f)
    {
        seg = st.segment;
        haveSegment = true;
    }

    if (!haveSegment)
    {
        for (int i = 0; i < 96; i++)
            if (s_portalLeases[i].active && s_portalLeases[i].ownerSlot == mySlot + 1)
                ReleasePortalTransitLease(&s_portalLeases[i]);
        if (st.phase != PT_PHASE_NONE) PortalResetBotState(st);
        return PORTAL_TRANSIT_NONE;
    }

    if (st.phase != PT_PHASE_NONE && st.key != seg.key)
    {
        for (int i = 0; i < 96; i++)
            if (s_portalLeases[i].active && s_portalLeases[i].ownerSlot == mySlot + 1)
                ReleasePortalTransitLease(&s_portalLeases[i]);
        PortalResetBotState(st);
    }
    if (st.phase == PT_PHASE_NONE)
    {
        st.phase = PT_PHASE_APPROACH;
        st.phaseTime = now;
        st.lastProgressTime = now;
        st.progressPos = pos;
    }
    st.key = seg.key;
    st.segment = seg;
    st.segmentValid = true;
    st.pathSerial = m_pathSerial;

    // Drop edges have their own takeoff/landing geometry in the normal path
    // controller. During crowd grace, keep that controller authoritative and
    // use the grace only to remove player blocking; aiming the transit helper
    // at the generated landing can pin bots against the lip.
    if (m_crowdTransitGraceActive && seg.dropEntry)
        return PORTAL_TRANSIT_NONE;

    // With crowd collision grace active there is no hull to serialize. Keep
    // the portal geometry (especially for drops/jumps) but let every
    // participant steer through it independently at full speed.
    if (m_spawnNoCollideActive || HasBodyNoCollide() || m_crowdTransitGraceActive)
    {
        // If a tight seam shoves the bot outside both adjoining tiles, aim at
        // the nearest point of their shared seam rectangle. Using an
        // individual tile's corner would send the bot through the wall beside
        // the opening; the shared rectangle is the actual doorway line.
        if (m_crowdTransitGraceActive && seg.entryHalf > 0.0f
            && seg.entryHalf < 32.0f
            && !seg.dropEntry && !seg.dropExit && !seg.jumpEntry)
        {
            const NavAreaData *fromArea = g_NavMesh.Area(seg.startArea);
            const NavAreaData *toArea = g_NavMesh.Area(seg.endArea);
            if (fromArea && toArea)
            {
                float seamX0 = fromArea->lo.x > toArea->lo.x
                             ? fromArea->lo.x : toArea->lo.x;
                float seamX1 = fromArea->hi.x < toArea->hi.x
                             ? fromArea->hi.x : toArea->hi.x;
                float seamY0 = fromArea->lo.y > toArea->lo.y
                             ? fromArea->lo.y : toArea->lo.y;
                float seamY1 = fromArea->hi.y < toArea->hi.y
                             ? fromArea->hi.y : toArea->hi.y;
                if (seamX0 <= seamX1 + 1.0f && seamY0 <= seamY1 + 1.0f)
                {
                    const float outsideMargin = 4.0f;
                    bool outsideFrom =
                        pos.x < fromArea->lo.x - outsideMargin
                     || pos.x > fromArea->hi.x + outsideMargin
                     || pos.y < fromArea->lo.y - outsideMargin
                     || pos.y > fromArea->hi.y + outsideMargin;
                    bool outsideTo =
                        pos.x < toArea->lo.x - outsideMargin
                     || pos.x > toArea->hi.x + outsideMargin
                     || pos.y < toArea->lo.y - outsideMargin
                     || pos.y > toArea->hi.y + outsideMargin;
                    if (outsideFrom && outsideTo)
                    {
                        Vector target(pos.x, pos.y, pos.z);
                        if (target.x < seamX0) target.x = seamX0;
                        else if (target.x > seamX1) target.x = seamX1;
                        if (target.y < seamY0) target.y = seamY0;
                        else if (target.y > seamY1) target.y = seamY1;
                        target.z = g_NavMesh.GetZ(seg.endArea,
                                                  target.x, target.y);
                        MoveAtWorldYaw(
                            atan2f(target.y - pos.y, target.x - pos.x)
                            * (180.0f / 3.14159265f),
                            viewYaw, insbot_move_speed.GetFloat() * 0.60f,
                            fwd, side);
                        return PORTAL_TRANSIT_HOLD;
                    }
                }
            }
        }

        float dxEntry = pos.x - seg.entry.x;
        float dyEntry = pos.y - seg.entry.y;
        float signedDist = dxEntry * seg.normal.x + dyEntry * seg.normal.y;
        float distEntry = sqrtf(dxEntry * dxEntry + dyEntry * dyEntry);
        float lateralDist = fabsf(dxEntry * -seg.normal.y
                                + dyEntry * seg.normal.x);
        float distExit = PortalHorizontalDist(pos, seg.exit);
        bool crossed = seg.dropEntry
                     ? (pos.z <= seg.entry.z - 20.0f
                        || g_NavMesh.IsOnAreaLanding(seg.endArea, pos, 12.0f))
                     : (signedDist >= 2.0f);
        if (crossed && distExit <= 40.0f)
            return PORTAL_TRANSIT_NONE;

        // Committing to the far side while still laterally offset makes a
        // bot press into the wall beside a narrow opening. Center on the
        // physical mouth first; only then aim through to the landing.
        bool centered = distEntry <= 16.0f && lateralDist <= 12.0f;
        Vector target = (crossed || centered)
                      ? seg.exit : seg.entry;
        float transitSpeed = insbot_move_speed.GetFloat()
                           * PT_CROWD_SPEED_SCALE;
        // A tight stepped seam can launch a bot onto the high ledge beside the
        // corridor at full engine speed. Cap only the physical tight-edge
        // commit; the rest of the route stays fast.
        if (IsTightEdge(seg.startArea, seg.endArea) && transitSpeed > 170.0f)
            transitSpeed = 170.0f;
        MoveAtWorldYaw(
            atan2f(target.y - pos.y, target.x - pos.x)
            * (180.0f / 3.14159265f),
            viewYaw, transitSpeed, fwd, side);
        if (m_crowdTransitGraceActive && insbot_sprint.GetBool()
            && !(buttons & IN_DUCK))
            buttons |= INS_IN_SPRINT;
        if (seg.entryHalf < 32.0f && distEntry < 96.0f)
            buttons |= INS_IN_USE;
        if (seg.jumpEntry && distEntry < 72.0f)
            buttons |= IN_JUMP;
        return PORTAL_TRANSIT_HOLD;
    }

    // The transit controller owns queue state while a physical segment is
    // active. Clear the legacy queue bookkeeping so it cannot leak into the
    // next open-terrain frame.
    m_doorYieldUntil = 0.0f;
    m_doorYieldSlot = 0;
    m_queueLeaderSlot = 0;
    m_queueWaitSince = 0.0f;
    m_chokeProgressTime = 0.0f;
    m_chokeBackoffUntil = 0.0f;
    m_blocked = false;
    m_chokeBackedOut = false;
    m_followSpeed = 0.0f;

    PortalTransitLease *lease = FindPortalTransitLease(seg.key);
    if (lease)
    {
        if (lease->ownerSlot == mySlot + 1)
        {
            // A same-opening repath is still the same transit episode. Adopt
            // the new path serial so the queue wait and lease age survive it.
            lease->ownerPathSerial = m_pathSerial;
        }
        else if (PortalLeaseIsStale(*lease, now))
        {
            ReleasePortalTransitLease(lease);
            lease = 0;
        }
    }

    if (st.phase == PT_PHASE_RECOVERY)
    {
        if (now < st.recoveryUntil)
        {
            float dx = pos.x - seg.entry.x;
            float dy = pos.y - seg.entry.y;
            float signedDist = dx * seg.normal.x + dy * seg.normal.y;
            if (signedDist < -12.0f && ClearBehind(
                    atan2f(seg.exit.y - pos.y, seg.exit.x - pos.x)
                    * (180.0f / 3.14159265f)))
            {
                MoveAtWorldYaw(
                    atan2f(seg.exit.y - pos.y, seg.exit.x - pos.x)
                    * (180.0f / 3.14159265f) + 180.0f,
                    viewYaw, insbot_move_speed.GetFloat() * 0.45f, fwd, side);
            }
            else
            {
                fwd = 0.0f;
                side = 0.0f;
            }
            return PORTAL_TRANSIT_HOLD;
        }
        st.phase = PT_PHASE_NONE;
        st.segmentValid = false;
        st.cacheValid = false;
        st.blockedUntil = now + 1.0f;
        return PORTAL_TRANSIT_NONE;
    }

    float dxEntry = pos.x - seg.entry.x;
    float dyEntry = pos.y - seg.entry.y;
    float signedDist = dxEntry * seg.normal.x + dyEntry * seg.normal.y;
    float lateralDist = fabsf(dxEntry * -seg.normal.y + dyEntry * seg.normal.x);
    float distEntry = sqrtf(dxEntry * dxEntry + dyEntry * dyEntry);
    float distExit = PortalHorizontalDist(pos, seg.exit);
    float exitSpan = (seg.exit.x - seg.entry.x) * seg.normal.x
                   + (seg.exit.y - seg.entry.y) * seg.normal.y;
    bool landingClear = PortalLandingClear(seg, mySlot);
    bool tightTransit = seg.entryHalf < 32.0f || seg.dropEntry;
    // Acquire a lease only when this bot is the deterministic next candidate.
    if (!lease)
    {
        int candidate = -1;
        float bestScore = -3.4e38f;
        for (int i = 0; i < 64; i++)
        {
            CInsBot *other = &g_Bots.m_bots[i];
            if (!other->m_inUse || !other->m_info || other->m_info->IsDead()) continue;
            if (!PortalPathContainsSegment(other->m_path, other->m_pathLen,
                                           other->m_pathCursor,
                                           seg.startArea, seg.endArea))
                continue;
            Vector op = other->m_info->GetAbsOrigin();
            if (!seg.dropEntry && fabsf(op.z - seg.entry.z) > 150.0f) continue;
            float odx = op.x - seg.entry.x;
            float ody = op.y - seg.entry.y;
            float osp = odx * seg.normal.x + ody * seg.normal.y;
            float olat = fabsf(odx * -seg.normal.y + ody * seg.normal.x);
            if (olat > 120.0f) continue;
            bool inside = osp > -4.0f;
            if (!inside && s_portalBots[i].blockedUntil > now) continue;
            float score = inside
                        ? (100000.0f + osp)
                        : -PortalHorizontalDist(op, seg.entry);
            if (score > bestScore
                || (fabsf(score - bestScore) < 0.01f && (candidate < 0 || i < candidate)))
            {
                bestScore = score;
                candidate = i;
            }
        }

        bool emergencyInside = signedDist > -4.0f;
        if (candidate == mySlot
            && (emergencyInside
                || (distEntry <= PT_ACQUIRE_RADIUS
                    && !PortalLandingReservedByOther(seg, seg.key)))
            && !(seg.key == st.lastCompletedKey && now < st.completedUntil))
        {
            lease = AllocatePortalTransitLease(seg.key);
            if (lease)
            {
                lease->ownerSlot = mySlot + 1;
                lease->ownerPathSerial = m_pathSerial;
                lease->startArea = seg.startArea;
                lease->endArea = seg.endArea;
                lease->grantTime = now;
                lease->lastProgressTime = now;
                lease->progressPos = pos;
                lease->entry = seg.entry;
                lease->exit = seg.exit;
                st.phase = PT_PHASE_APPROACH;
                st.phaseTime = now;
                st.lastProgressTime = now;
                st.progressPos = pos;
            }
        }
    }

    if (lease && lease->ownerSlot == mySlot + 1)
    {
        lease->lastProgressTime = now;
        PortalTrackProgress(st, pos, now);

        if (st.phase == PT_PHASE_APPROACH)
        {
            if (!landingClear && signedDist < -8.0f)
            {
                st.phase = PT_PHASE_QUEUED;
                st.phaseTime = now;
                fwd = 0.0f;
                side = 0.0f;
                bool preserveRecovery = m_bodyNoCollideActive
                                     || m_bodyNoCollideEscalating
                                     || HasBodyNoCollide()
                                     || (m_spawnNoCollideActive
                                         && m_spawnNoCollideExpired);
                if (!preserveRecovery)
                {
                    m_stuckSince = 0.0f;
                    m_stuckStage = 0;
                    m_unstuckUntil = 0.0f;
                }
                return PORTAL_TRANSIT_HOLD;
            }
            if (distEntry <= PT_COMMIT_RADIUS)
            {
                st.phase = PT_PHASE_COMMITTED;
                st.phaseTime = now;
                st.lastProgressTime = now;
                st.progressPos = pos;
            }
            else
            {
                Vector target = m_hasMoveTarget ? m_moveTarget : seg.entry;
                MoveAtWorldYaw(
                    atan2f(target.y - pos.y, target.x - pos.x)
                    * (180.0f / 3.14159265f),
                    viewYaw, insbot_move_speed.GetFloat() * 0.9f, fwd, side);
                return PORTAL_TRANSIT_OWNER;
            }
        }

        if (st.phase == PT_PHASE_QUEUED)
        {
            bool waitedLongEnough = now - st.phaseTime >= PT_QUEUE_WAIT_MAX;
            if (!landingClear && !waitedLongEnough)
            {
                fwd = 0.0f;
                side = 0.0f;
                bool preserveRecovery = m_bodyNoCollideActive
                                     || m_bodyNoCollideEscalating
                                     || HasBodyNoCollide()
                                     || (m_spawnNoCollideActive
                                         && m_spawnNoCollideExpired);
                if (!preserveRecovery)
                {
                    m_stuckSince = 0.0f;
                    m_stuckStage = 0;
                    m_unstuckUntil = 0.0f;
                }
                return PORTAL_TRANSIT_HOLD;
            }
            if (distEntry > PT_COMMIT_RADIUS + 24.0f && !waitedLongEnough)
            {
                Vector target = m_hasMoveTarget ? m_moveTarget : seg.entry;
                MoveAtWorldYaw(
                    atan2f(target.y - pos.y, target.x - pos.x)
                    * (180.0f / 3.14159265f),
                    viewYaw, insbot_move_speed.GetFloat() * 0.85f, fwd, side);
                return PORTAL_TRANSIT_OWNER;
            }
            st.phase = PT_PHASE_COMMITTED;
            st.phaseTime = now;
            st.lastProgressTime = now;
            st.progressPos = pos;
        }

        if (st.phase == PT_PHASE_COMMITTED)
        {
            Vector target = m_hasMoveTarget ? m_moveTarget : seg.exit;
            float tdx = target.x - pos.x;
            float tdy = target.y - pos.y;
            // Stay aimed through the physical opening until the hull is
            // actually through it.  The old condition only forced the exit
            // target after the bot was already 24u past the entry plane, so a
            // bot arriving from behind kept steering back toward its next NAV
            // waypoint and ground against the lip forever.
            if (signedDist < exitSpan - 8.0f)
                target = seg.exit;
            MoveAtWorldYaw(
                atan2f(target.y - pos.y, target.x - pos.x)
                * (180.0f / 3.14159265f),
                viewYaw, insbot_move_speed.GetFloat(), fwd, side);
            if (seg.jumpEntry && distEntry < 72.0f)
                buttons |= INS_IN_JUMP;

            bool crossed = seg.dropEntry
                         ? (pos.z <= seg.entry.z - 20.0f
                            || g_NavMesh.IsOnAreaLanding(seg.endArea, pos, 12.0f))
                         : (signedDist >= 2.0f);
            if (crossed)
            {
                st.phase = PT_PHASE_LAND;
                st.phaseTime = now;
                st.lastProgressTime = now;
                st.progressPos = pos;
            }
            else if (now - st.lastProgressTime >= 2.5f)
            {
                ReleasePortalTransitLease(lease);
                st.phase = PT_PHASE_RECOVERY;
                st.phaseTime = now;
                st.recoveryUntil = now + 1.6f;
                st.blockedUntil = now + 2.6f;
                fwd = 0.0f;
                side = 0.0f;
                return PORTAL_TRANSIT_HOLD;
            }
            return PORTAL_TRANSIT_OWNER;
        }

        if (st.phase == PT_PHASE_LAND)
        {
            Vector target = seg.exit;
            if (m_hasMoveTarget)
            {
                float tdx = m_moveTarget.x - seg.exit.x;
                float tdy = m_moveTarget.y - seg.exit.y;
                if (tdx * seg.exitNormal.x + tdy * seg.exitNormal.y > -8.0f)
                    target = m_moveTarget;
            }
            Vector moved = pos;
            MoveAtWorldYaw(
                atan2f(target.y - pos.y, target.x - pos.x)
                * (180.0f / 3.14159265f),
                viewYaw, insbot_move_speed.GetFloat(), fwd, side);

            float movedExit = PortalHorizontalDist(moved, seg.exit);
            bool reached = movedExit <= 28.0f
                        || (movedExit <= 48.0f
                            && g_NavMesh.IsOnAreaLanding(seg.endArea, moved, 12.0f));
            if (reached)
            {
                st.phase = PT_PHASE_CLEAR;
                st.phaseTime = now;
                st.lastProgressTime = now;
                st.progressPos = pos;
            }
            else if (now - st.lastProgressTime >= 2.5f)
            {
                ReleasePortalTransitLease(lease);
                st.phase = PT_PHASE_RECOVERY;
                st.phaseTime = now;
                st.recoveryUntil = now + 1.6f;
                st.blockedUntil = now + 2.6f;
                fwd = 0.0f;
                side = 0.0f;
                return PORTAL_TRANSIT_HOLD;
            }
            return PORTAL_TRANSIT_OWNER;
        }

        if (st.phase == PT_PHASE_CLEAR)
        {
            int curArea = CurrentArea(pos);
            bool leftLanding = curArea >= 0
                            && curArea != seg.endArea
                            && curArea != seg.startArea;

            // Do not hand the opening to the next bot while this owner still
            // occupies a tiny landing tile. On stairs/drops the next hull can
            // otherwise arrive before the first has entered the downstream
            // NAV area, recreating the exact crowd pile-up at the lip.
            // Once the owner is far enough from the landing center, the next
            // owner may approach. PortalLandingClear still prevents it from
            // committing into a hull that overlaps the landing, so waiting
            // for a full area change here only throttled the queue (and
            // deadlocked when the final destination was inside endArea).
            if (leftLanding || distExit >= 24.0f)
            {
                ReleasePortalTransitLease(lease);
                st.lastCompletedKey = seg.key;
                st.completedUntil = now + 1.0f;
                st.phase = PT_PHASE_NONE;
                st.segmentValid = false;
                st.cacheValid = false;
                return PORTAL_TRANSIT_NONE;
            }
            if (now - st.phaseTime >= 10.0f)
            {
                ReleasePortalTransitLease(lease);
                st.lastCompletedKey = seg.key;
                st.completedUntil = now + 1.0f;
                st.phase = PT_PHASE_NONE;
                st.segmentValid = false;
                st.cacheValid = false;
                return PORTAL_TRANSIT_NONE;
            }

            Vector target = seg.exit + seg.exitNormal * 48.0f;
            if (m_hasMoveTarget)
            {
                float tdx = m_moveTarget.x - seg.exit.x;
                float tdy = m_moveTarget.y - seg.exit.y;
                if (tdx * seg.exitNormal.x + tdy * seg.exitNormal.y > 8.0f)
                    target = m_moveTarget;
            }
            MoveAtWorldYaw(
                atan2f(target.y - pos.y, target.x - pos.x)
                * (180.0f / 3.14159265f),
                viewYaw, insbot_move_speed.GetFloat() * 0.9f, fwd, side);
            if (now - st.lastProgressTime >= 2.5f)
            {
                ReleasePortalTransitLease(lease);
                st.phase = PT_PHASE_RECOVERY;
                st.phaseTime = now;
                st.recoveryUntil = now + 1.6f;
                st.blockedUntil = now + 2.6f;
                fwd = 0.0f;
                side = 0.0f;
                return PORTAL_TRANSIT_HOLD;
            }
            return PORTAL_TRANSIT_OWNER;
        }

        st.phase = PT_PHASE_APPROACH;
        st.phaseTime = now;
    }

    // Another bot owns this physical opening. A non-owner still inside the
    // throat waits behind the owner; only a bot that has physically reached
    // the far side may continue out of the opening.
    if (signedDist > -4.0f && signedDist >= exitSpan - 8.0f)
    {
        Vector target = m_hasMoveTarget ? m_moveTarget : seg.exit;
        MoveAtWorldYaw(
            atan2f(target.y - pos.y, target.x - pos.x)
            * (180.0f / 3.14159265f),
            viewYaw, insbot_move_speed.GetFloat() * 0.8f, fwd, side);
        return PORTAL_TRANSIT_HOLD;
    }

    if (now < st.blockedUntil)
    {
        fwd = 0.0f;
        side = 0.0f;
        return PORTAL_TRANSIT_HOLD;
    }

    // Crowd transit grace: non-blocking bots do not need the one-at-a-time
    // lease. Move them straight at the verified opening so a crowd can drain
    // instead of staging hundreds of units behind a single owner.
    if (tightTransit && !lease
        && (m_spawnNoCollideActive || HasBodyNoCollide())
        && distEntry <= PT_ACQUIRE_RADIUS + 40.0f)
    {
        Vector target = distEntry > PT_COMMIT_RADIUS ? seg.entry : seg.exit;
        MoveAtWorldYaw(
            atan2f(target.y - pos.y, target.x - pos.x)
            * (180.0f / 3.14159265f),
            viewYaw, insbot_move_speed.GetFloat() * 0.7f, fwd, side);
        return PORTAL_TRANSIT_HOLD;
    }

    if (distEntry > 220.0f)
        return PORTAL_TRANSIT_HOLD;

    // Reserve a deterministic column of staging points behind the lip.
    // Distance ordering gives the closest bot rank zero, so the elected owner
    // never has to pass through a teammate parked in front of the opening.
    int queueRank = 0;
    for (int i = 0; i < 64; i++)
    {
        if (i == mySlot) continue;
        CInsBot *other = &g_Bots.m_bots[i];
        if (!other->m_inUse || !other->m_info || other->m_info->IsDead()) continue;
        if (!PortalPathContainsSegment(other->m_path, other->m_pathLen,
                                       other->m_pathCursor,
                                       seg.startArea, seg.endArea))
            continue;
        Vector op = other->m_info->GetAbsOrigin();
        float odx = op.x - seg.entry.x;
        float ody = op.y - seg.entry.y;
        float osp = odx * seg.normal.x + ody * seg.normal.y;
        float olat = fabsf(odx * -seg.normal.y + ody * seg.normal.x);
        if (osp > 8.0f || olat > 140.0f) continue;
        float odist = sqrtf(odx * odx + ody * ody);
        if (odist < distEntry
            || (fabsf(odist - distEntry) < 0.01f && i < mySlot))
            queueRank++;
    }

    float behindDist = PT_HOLD_BEHIND + (float)queueRank * 44.0f;
    Vector queueTarget;
    if (!PortalQueueTarget(seg, m_path, m_pathLen, seg.startEdge, behindDist,
                           queueTarget)
        && !PortalNeighborQueueTarget(seg, queueRank, queueTarget))
        queueTarget = seg.entry - seg.normal * behindDist;
    float qdx = queueTarget.x - pos.x;
    float qdy = queueTarget.y - pos.y;
    float qdist = sqrtf(qdx * qdx + qdy * qdy);
    if (qdist > 24.0f)
    {
        MoveAtWorldYaw(
            atan2f(qdy, qdx) * (180.0f / 3.14159265f),
            viewYaw, insbot_move_speed.GetFloat() * 0.7f, fwd, side);
        return PORTAL_TRANSIT_HOLD;
    }

    fwd = 0.0f;
    side = 0.0f;
    bool preserveRecovery = m_bodyNoCollideActive
                         || m_bodyNoCollideEscalating
                         || HasBodyNoCollide()
                         || (m_spawnNoCollideActive
                             && m_spawnNoCollideExpired);
    if (!preserveRecovery)
    {
        m_stuckSince = 0.0f;
        m_stuckStage = 0;
        m_unstuckUntil = 0.0f;
    }
    return PORTAL_TRANSIT_HOLD;
}

// Signed shortest angular difference (a-b) in [-180, 180).


void CInsBot::ApplyCrowdSteeringClassic(float moveYaw, float viewYaw, float &fwd,
                                 float &side, int &buttons)
{
    Vector pos = m_info->GetAbsOrigin();
    bool moving = fabsf(fwd) > 50.0f || fabsf(side) > 50.0f;
    int mySlot = (int)(this - g_Bots.m_bots);
    float now = Now();

    if (!moving)
    {
        m_doorYieldUntil = 0.0f;
        m_doorYieldSlot = 0;
        m_queueLeaderSlot = 0;
        m_queueWaitSince = 0.0f;
        m_queueBreakUntil = 0.0f;
        m_chokeProgressTime = 0.0f;
        m_chokeBackoffUntil = 0.0f;
        return;
    }

    // Collision grace already removes the only reason for the legacy
    // single-file queue: teammate hulls cannot block the passage. Let the
    // crowd flow at path speed instead of staging it hundreds of units back.
    if (m_spawnNoCollideActive || HasBodyNoCollide() || m_crowdTransitGraceActive)
        return;

    int myTeam = m_info->GetTeamIndex();
    float yawDiff = fmodf(moveYaw - m_chokeCacheYaw, 360.0f);
    if (yawDiff > 180.0f) yawDiff -= 360.0f;
    if (yawDiff < -180.0f) yawDiff += 360.0f;
    if (now >= m_chokeCacheTime || fabsf(yawDiff) > 20.0f)
    {
        m_chokeCacheTime = now + 0.15f;
        m_chokeCacheYaw = moveYaw;
        m_doorCached = DoorAhead(moveYaw);
        m_chokeCached = ChokeAhead(moveYaw);
        m_lowObstacleCached = LowObstacleAhead(moveYaw);
    }
    bool closedDoor = m_doorCached;
    bool doorInWay = closedDoor || m_chokeCached;

    if (closedDoor && now >= m_nextUseTime)
    {
        buttons |= INS_IN_USE;
        m_nextUseTime = now + 0.8f;
    }

    if (doorInWay)
    {
        float yr = moveYaw * (3.14159265f / 180.0f);
        Vector laneFwd(cosf(yr), sinf(yr), 0);
        Vector laneSide(-sinf(yr), cosf(yr), 0);
        int predecessor = -1;
        float predecessorAhead = 99999.0f;

        for (int i = 0; i < 64; i++)
        {
            CInsBot *other = &g_Bots.m_bots[i];
            if (!other->m_inUse || other == this || !other->m_info || other->m_info->IsDead())
                continue;
            if (other->m_info->GetTeamIndex() != myTeam)
                continue;

            Vector otherPos = other->m_info->GetAbsOrigin();
            float rx = otherPos.x - pos.x;
            float ry = otherPos.y - pos.y;
            float ahead = rx * laneFwd.x + ry * laneFwd.y;
            float lateral = fabsf(rx * laneSide.x + ry * laneSide.y);
            if (ahead > 360.0f || lateral > 128.0f ||
                fabsf(otherPos.z - pos.z) > 72.0f)
                continue;

            // Overlapping hulls use roster order as a stable tie-break.
            if (ahead <= 4.0f)
            {
                if (ahead < -4.0f || i > mySlot)
                    continue;
                ahead = 4.0f;
            }

            bool oppositeFlow = false;
            if (other->m_haveNavDest)
            {
                float odx = other->m_navDest.x - otherPos.x;
                float ody = other->m_navDest.y - otherPos.y;
                float olen = sqrtf(odx * odx + ody * ody);
                if (olen > 1.0f)
                    oppositeFlow = (odx * laneFwd.x + ody * laneFwd.y) < -0.25f * olen;
            }
            if (oppositeFlow && mySlot < i)
                continue;

            if (ahead < predecessorAhead)
            {
                predecessor = i;
                predecessorAhead = ahead;
            }
        }

        if (predecessor >= 0)
        {
            m_doorYieldSlot = predecessor + 1;
            m_doorYieldUntil = now + 0.35f;
            m_chokeProgressTime = 0.0f;
            m_chokeBackoffUntil = 0.0f;
            bool preserveRecovery = m_bodyNoCollideActive
                                 || m_bodyNoCollideEscalating
                                 || HasBodyNoCollide()
                                 || (m_spawnNoCollideActive
                                     && m_spawnNoCollideExpired);
            if (!preserveRecovery)
            {
                m_stuckSince = 0.0f;
                m_stuckStage = 0;
                m_unstuckUntil = 0.0f;
            }

            if (predecessorAhead < 76.0f)
                MoveAtWorldYaw(moveYaw + 180.0f, viewYaw,
                               insbot_move_speed.GetFloat() * 0.45f, fwd, side);
            else if (predecessorAhead < 112.0f)
                fwd = side = 0.0f;
        }
        else
        {
            m_doorYieldSlot = 0;
            m_doorYieldUntil = 0.0f;
            if (m_chokeProgressTime <= 0.0f)
            {
                m_chokeProgressPos = pos;
                m_chokeProgressTime = now;
            }
            else if (xyd(pos, m_chokeProgressPos) >= 24.0f)
            {
                m_chokeProgressPos = pos;
                m_chokeProgressTime = now;
            }
            else if (now - m_chokeProgressTime >= 1.75f)
            {
                m_chokeBackoffUntil = now + 0.65f;
                m_chokeProgressTime = 0.0f;
                BotDbg("[insbots] %s narrow-passage leader blocked - backing out\n", m_name);
            }
        }
    }
    else
    {
        m_doorYieldUntil = 0.0f;
        m_doorYieldSlot = 0;
        m_chokeProgressTime = 0.0f;
        float yr = moveYaw * (3.14159265f / 180.0f);
        Vector laneFwd(cosf(yr), sinf(yr), 0);
        Vector laneSide(-sinf(yr), cosf(yr), 0);
        int crowdLeader = -1;
        for (int i = 0; i < 64; i++)
        {
            CInsBot *other = &g_Bots.m_bots[i];
            if (!other->m_inUse || other == this || !other->m_info || other->m_info->IsDead())
                continue;
            if (other->m_info->GetTeamIndex() != myTeam || i >= mySlot)
                continue;
            Vector otherPos = other->m_info->GetAbsOrigin();
            float rx = otherPos.x - pos.x, ry = otherPos.y - pos.y;
            float ahead = rx * laneFwd.x + ry * laneFwd.y;
            float lateral = fabsf(rx * laneSide.x + ry * laneSide.y);
            if (ahead > 16.0f && ahead < 112.0f && lateral < 72.0f
                && fabsf(otherPos.z - pos.z) < 72.0f)
            {
                crowdLeader = i;
                break;
            }
        }
        if (crowdLeader >= 0)
        {
            // Preserve a forward component. A pure 90-degree command can keep
            // selecting successive nearby teammates and sidestep forever,
            // never reducing the distance to the waypoint.
            float sidestepYaw = moveYaw + ((mySlot & 1) ? 55.0f : -55.0f);
            MoveAtWorldYaw(sidestepYaw, viewYaw,
                           insbot_move_speed.GetFloat() * 0.65f, fwd, side);
        }
    }

    if (now < m_chokeBackoffUntil)
    {
        MoveAtWorldYaw(moveYaw + 180.0f, viewYaw,
                       insbot_move_speed.GetFloat() * 0.55f, fwd, side);
        m_doorYieldSlot = 0;
        m_doorYieldUntil = 0.0f;
    }

    if (now >= m_chokeBackoffUntil && m_lowObstacleCached)
        buttons |= IN_JUMP;
}

// Vehicle-avoidance controller. Returns true when it took ownership of this
// frame's movement, in which case the caller must return immediately and must
// NOT run either crowd follower: the doorway runway override downstream
// rewrites fwd/side and slams the view yaw toward the portal normal, which
// would steer the bot straight into the truck.
//
// Returns false for every "not my problem" case, leaving the normal followers
// untouched.
bool CInsBot::SteerAroundVehicle(float moveYaw, float viewYaw,
                                 float &fwd, float &side, int &buttons)
{
    INSPROF_SCOPE(INSPROF_VEHICLE);

    if (!m_info || !m_edict) return false;

    // A vehicle deflection is a lateral manoeuvre, and the drop queue owns
    // that bot's movement at an elevation edge. Never shove a bot sideways
    // near a ledge.
    if (m_dropAhead) return false;

    Vector pos = m_info->GetAbsOrigin();
    float now = Now();

    // Continue an in-flight hold without re-probing.
    if (m_vehicleDeflect && now < m_vehicleDeflectUntil)
    {
        MoveAtWorldYaw(m_vehicleDeflectYaw, viewYaw,
                       insbot_move_speed.GetFloat() * INS_VEHICLE_SPEED_SCALE,
                       fwd, side);
        // The bits were computed for the ORIGINAL heading. Jumping beside a
        // hull risks landing on it; duck was earned by a real low-ceiling
        // trace and is still true, so keep it.
        buttons &= ~IN_JUMP;
        // A committed lateral lane from the corridor logic is meaningless
        // once the bot is diverting; leaving it set lets a stale lane
        // override the deflection next frame.
        m_passSide = 0.0f;
        m_passUntil = 0.0f;
        return true;
    }

    // Throttle the probe. A truck at 20 mph moves ~44u in 0.15s, negligible
    // against a 200u probe.
    if (now < m_vehicleLastProbe) return false;
    m_vehicleLastProbe = now + 0.15f;

    float yr = moveYaw * (3.14159265f / 180.0f);
    Vector dir(cosf(yr), sinf(yr), 0);
    Vector from = pos + Vector(0, 0, 36);

    // Hull sweep, not a centre ray: a waist ray can clear an obstacle the
    // bot's body would hit, which is the documented reason SideStepOpen
    // sweeps (bot_recover.cpp:349-353).
    CTraceFilterSteerObstacles filter( this );
    Ray_t ray; trace_t tr;
    ray.Init(from, from + dir * INS_VEHICLE_PROBE_DIST,
             Vector(-16.0f, -16.0f, 0.0f), Vector(16.0f, 16.0f, 72.0f));
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &tr);

    // Only a vehicle is this controller's business. A wall, a closed door or
    // a player is the existing logic's problem.
    if (tr.fraction >= 1.0f || !tr.m_pEnt) return false;

    IServerUnknown *unk = (IServerUnknown *)tr.m_pEnt;
    IServerNetworkable *net = unk ? unk->GetNetworkable() : 0;
    ServerClass *sc = net ? net->GetServerClass() : 0;
    if (!sc || !IsDriveableClass( sc->GetName() )) return false;

    // Episode anchoring: the shoulders are measured from the heading held
    // when the episode BEGAN, never from a heading a previous deflection
    // already moved the bot to. Measuring from the deflected heading adds
    // another 60 degrees per re-probe and produced a 6-value spin at speed 0.
    bool baseFresh = (now - m_steerBaseTime) <= 0.75f
                  && m_steerLastYaw == m_headingYaw;
    if (!baseFresh)
    {
        m_steerBaseYaw = m_headingYaw;
        m_steerLastYaw = m_headingYaw;
    }
    float baseYaw = m_steerBaseYaw;

    // Measure both shoulders. A shoulder must clear enough of the probe to
    // actually be walked, and must clear the floor too - the lateral sweep
    // alone would happily clear a ledge.
    float fracs[2];
    for (int s = 0; s < 2; s++)
    {
        float y = baseYaw + (s == 0 ? 60.0f : -60.0f);
        float yrr = y * (3.14159265f / 180.0f);
        Vector d2(cosf(yrr), sinf(yrr), 0);
        trace_t t2;
        ray.Init(from, from + d2 * INS_VEHICLE_PROBE_DIST,
                 Vector(-16.0f, -16.0f, 0.0f), Vector(16.0f, 16.0f, 72.0f));
        enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &t2);
        bool clear = !t2.startsolid && !t2.allsolid && t2.fraction >= 1.0f;

        // Floor check, same contract as SideStepOpen's second trace.
        if (clear)
        {
            Vector probe = pos + d2 * 48.0f;
            Vector to = probe + Vector(0, 0, 36);
            trace_t tFloor;
            ray.Init(to + Vector(0, 0, 24), to - Vector(0, 0, 72));
            enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &tFloor);
            clear = !tFloor.startsolid && !tFloor.allsolid && tFloor.fraction < 1.0f;
        }
        fracs[s] = clear ? 1.0f : 0.0f;
    }

    // Which side of the waypoint line is the hit on? Positive = left.
    float hitLateral = InsSteerLateralOffset(pos.x, pos.y, moveYaw,
                                             tr.endpos.x, tr.endpos.y);

    float chosenYaw = 0.0f;
    if (!InsSteerPickShoulder(baseYaw, fracs[0], fracs[1],
                              INS_VEHICLE_MIN_SHOULDER_FRAC, hitLateral,
                              chosenYaw))
    {
        // Neither shoulder is walkable. Do NOT deflect and do NOT stop: keep
        // the normal forward motion and let stuck recovery handle it. The
        // truck may be moving away.
        if (insbot_debug.GetBool())
        {
            edict_t *hitEdict = EdictFromTraceEnt(tr.m_pEnt);
            BotDbg("[insbots] [STEER_VEHICLE] %s pos=(%.1f %.1f %.1f) "
                   "ent=%d frac=%.3f neither shoulder walkable\n",
                   m_name, pos.x, pos.y, pos.z,
                   hitEdict ? engine->IndexOfEdict(hitEdict) : 0,
                   tr.fraction);
        }
        m_steerBaseTime = now;
        m_steerLastYaw  = m_headingYaw;
        return false;
    }

    m_headingYaw          = chosenYaw;
    m_vehicleDeflect      = true;
    m_vehicleDeflectYaw   = chosenYaw;
    m_vehicleDeflectUntil = now + INS_VEHICLE_HOLD_SECONDS;
    m_steerBaseYaw        = baseYaw;
    m_steerBaseTime       = now;
    m_steerLastYaw        = chosenYaw;

    // Stale corridor recovery state: returning early means the follower never
    // runs, so these keep whatever values they had before the truck arrived.
    // m_blocked feeds doorway detection (bot_body.cpp:477) and
    // m_chokeBackoffUntil gates the unstuck wiggle (bot_think.cpp:2798).
    m_blocked = false;
    m_chokeBackoffUntil = 0.0f;
    m_passSide = 0.0f;
    m_passUntil = 0.0f;

    if (insbot_debug.GetBool())
    {
        int myIdx = m_edict ? engine->IndexOfEdict(m_edict) : 0;
        float curSpeed = (myIdx >= 1 && myIdx < 66) ? s_hearSpeed[myIdx] : 0.0f;
        edict_t *hitEdict = EdictFromTraceEnt(tr.m_pEnt);
        BotDbg("[insbots] [STEER_VEHICLE] %s pos=(%.1f %.1f %.1f) speed=%.1f "
               "ent=%d cls='%s' frac=%.3f baseYaw=%.1f -> shoulder=%.1f "
               "lateral=%.1f buttons=0x%X\n",
               m_name, pos.x, pos.y, pos.z, curSpeed,
               hitEdict ? engine->IndexOfEdict(hitEdict) : 0,
               hitEdict ? hitEdict->GetClassName() : "world",
               tr.fraction, baseYaw, chosenYaw, hitLateral, buttons);
    }

    MoveAtWorldYaw(chosenYaw, viewYaw,
                   insbot_move_speed.GetFloat() * INS_VEHICLE_SPEED_SCALE,
                   fwd, side);
    buttons &= ~IN_JUMP;
    return true;
}

void CInsBot::ApplyCrowdSteering(float moveYaw, float viewYaw, float &fwd,
                                 float &side, int &buttons)
{
    // Vehicle avoidance owns the frame when a truck is on the route. It runs
    // BEFORE the classic dispatch and BEFORE the !nearTeammate early return,
    // because that return skips every probe and would otherwise let an
    // isolated bot walk straight into the hull.
    {
        bool moving = (fabsf(fwd) > 50.0f || fabsf(side) > 50.0f);
        if (InsSteerShouldRunVehicleCheck(g_Turrets.VehicleCount(), moving,
                                          insbot_avoid_vehicles.GetBool()))
        {
            if (SteerAroundVehicle(moveYaw, viewYaw, fwd, side, buttons))
                return;
        }
    }

    int streamingMode = insbot_streaming.GetInt();
    Vector pos = m_info->GetAbsOrigin();
    UpdateCrowdTransitGrace(pos, Now());
    if (streamingMode != 0)
    {
        int transit = UpdatePortalTransit(pos, viewYaw, fwd, side, buttons);
        if (transit != PORTAL_TRANSIT_NONE) return;
    }
    // Crowd grace removes the reason for single-file queueing. If the current
    // edge is a wide approach and the detector has already armed for a narrow
    // edge ahead, preserve the normal path steering instead of letting the
    // legacy queue stop the column before the mouth.
    if (m_crowdTransitGraceActive)
        return;
    bool physicalPassage = false;
    if (m_pathLen > 0 && m_pathCursor > 0 && m_pathCursor < m_pathLen)
        physicalPassage = SourceTurnForEdge(m_path[m_pathCursor - 1],
                                            m_path[m_pathCursor], pos);
    // Mode 2 now has full-hull physical portal evidence, so an isolated real
    // doorway can use the queue without requiring two narrow NAV edges. Small
    // open-field tiles fail the enclosing-wall probe and stay on classic flow.
    // Drop queues remain active in automatic mode so a mapper-authored jump or
    // descent does not let every bot launch at once.
    bool corridorOnly = (streamingMode == 2)
                     && !physicalPassage && !m_dropAhead;
    if (streamingMode == 0 || corridorOnly)
    {
        if (streamingMode == 0) m_corridorLatched = false;
        m_corridorAhead = false;
        m_corridorAheadDist = 0.0f;
        m_dropAhead = false;
        m_dropAheadDist = 0.0f;
        m_passSide = 0.0f;
        m_passUntil = 0.0f;
        m_followSpeed = 0.0f;
        m_queueLeaderSlot = 0;
        m_queueWaitSince = 0.0f;
        m_queueBreakUntil = 0.0f;
        m_blocked = false;
        m_chokeBackedOut = false;
        ApplyCrowdSteeringClassic(moveYaw, viewYaw, fwd, side, buttons);
        return;
    }

    bool moving = fabsf(fwd) > 50.0f || fabsf(side) > 50.0f;
    int mySlot = (int)(this - g_Bots.m_bots);
    float now = Now();

    if (!moving)
    {
        m_doorYieldUntil = 0.0f;
        m_doorYieldSlot = 0;
        m_queueLeaderSlot = 0;
        m_queueWaitSince = 0.0f;
        m_queueBreakUntil = 0.0f;
        m_chokeProgressTime = 0.0f;
        m_chokeBackoffUntil = 0.0f;
        m_blocked = false;
        m_chokeBackedOut = false;
        return;
    }

    int myTeam = m_info->GetTeamIndex();

    // ---- broad phase: any same-team bot nearby? ----
    // With no teammate within 600u there is no queue/follow interaction, so
    // the per-bot lane scan below (and its trace probes) is skipped entirely.
    // 600u (not 400u) lets a column recognize itself while still spread across
    // a large spawn room, before the bots arrive side-by-side at the doorway.
    bool nearTeammate = false;
    for (int i = 0; i < 64 && !nearTeammate; i++)
    {
        const CInsBot *other = &g_Bots.m_bots[i];
        if (!other->m_inUse || other == this || !other->m_info || other->m_info->IsDead())
            continue;
        if (other->m_info->GetTeamIndex() != myTeam) continue;
        Vector op = other->m_info->GetAbsOrigin();
        if (fabsf(op.z - pos.z) > 72.0f) continue;
        float dx = op.x - pos.x, dy = op.y - pos.y;
        if (dx * dx + dy * dy < 600.0f * 600.0f) nearTeammate = true;
    }

    // ---- drop-awareness propagation ----
    // A bot whose own waypoint is not below its feet yet has no drop flag,
    // so it would keep walking until it reaches the edge and then pack into
    // the bot already stopped there. If ANY same-team bot within 400u is
    // about to walk off a drop, treat this bot as drop-aware too so the
    // whole queue holds the gap BEFORE reaching the edge.
    bool nearbyDropAhead = false;
    if (nearTeammate)
    {
        for (int i = 0; i < 64 && !nearbyDropAhead; i++)
        {
            const CInsBot *other = &g_Bots.m_bots[i];
            if (!other->m_inUse || other == this || !other->m_info || other->m_info->IsDead())
                continue;
            if (other->m_info->GetTeamIndex() != myTeam) continue;
            if (!other->m_dropAhead || other->m_dropAheadDist > 260.0f) continue;
            Vector op = other->m_info->GetAbsOrigin();
            if (fabsf(op.z - pos.z) > 200.0f) continue;
            float dx = op.x - pos.x, dy = op.y - pos.y;
            if (dx * dx + dy * dy < 240.0f * 240.0f) nearbyDropAhead = true;
        }
    }

    // ---- cached geometry probes (10 Hz, or on a big yaw change) ----
    // ChokeAhead/DoorAhead/LowObstacleAhead each run several traces; every bot
    // used to re-run them every frame even when nobody was nearby.
    float yawDiff = fmodf(moveYaw - m_chokeCacheYaw, 360.0f);
    if (yawDiff > 180.0f) yawDiff -= 360.0f;
    if (yawDiff < -180.0f) yawDiff += 360.0f;
    if (now >= m_chokeCacheTime || fabsf(yawDiff) > 20.0f)
    {
        m_chokeCacheTime = now + 0.15f;
        m_chokeCacheYaw = moveYaw;
        m_doorCached = DoorAhead(moveYaw);
        m_lowObstacleCached = LowObstacleAhead(moveYaw);
        // Physical choke evidence also drives automatic doorway turning, so
        // keep it current even for an isolated front bot.
        m_chokeCached = ChokeAhead(moveYaw);
    }
    bool closedDoor = m_doorCached;
    bool doorInWay = !m_dropAhead
                  && (closedDoor || m_chokeCached || physicalPassage);
    // About to enter a tight corridor (path lookahead): no passing, a wider
    // predecessor window so side-by-side bots merge into one line, and a
    // slower follow so the queue forms before the mouth.
    bool corridorAhead = m_corridorAhead && m_corridorAheadDist <= 320.0f;
    // About to walk off an elevation drop (waypoint below the feet): treat
    // it like a corridor for spacing, but hold FULLY at the edge so nobody
    // lands on a teammate below.
    bool dropAhead = (m_dropAhead && m_dropAheadDist <= 320.0f)
                  || nearbyDropAhead;
    // Jump is a launch action for the committed front bot, not a generic
    // animation for every bot that can see the lip. This flag is finalized
    // after predecessor/landing checks and applied after LowObstacleAhead so
    // no later helper can re-add jump to a held follower.
    bool suppressDropJump = false;

    if (closedDoor && now >= m_nextUseTime)
    {
        buttons |= INS_IN_USE;
        m_nextUseTime = now + 0.8f;
    }

    if (!nearTeammate)
    {
        m_doorYieldUntil = 0.0f;
        m_doorYieldSlot = 0;
        m_queueLeaderSlot = 0;
        m_queueWaitSince = 0.0f;
        m_queueBreakUntil = 0.0f;
        m_chokeProgressTime = 0.0f;
        m_chokeBackoffUntil = 0.0f;
        m_blocked = false;
        m_chokeBackedOut = false;
        m_followSpeed = 0.0f;
        if (m_lowObstacleCached) buttons |= IN_JUMP;
        return;
    }

    float yr = moveYaw * (3.14159265f / 180.0f);
    Vector laneFwd(cosf(yr), sinf(yr), 0);
    Vector laneSide(-sinf(yr), cosf(yr), 0);

    // ---- same-lane predecessor scan (chokes/corridors/drops only) ----
    // The 0.19.4 column streaming ran this scan everywhere, which made bots
    // in open terrain latch onto whoever was roughly ahead and follow at
    // queue speed (plus stop for drops they were nowhere near). Now the lane
    // queue only engages for a real narrow passage or elevation step; open
    // terrain uses the simple crowd-sidestep below instead.
    bool queueContext = doorInWay || corridorAhead || dropAhead;
    int predecessor = -1;
    float predecessorAhead = 99999.0f;
    // Path-order awareness: two bots whose nav paths cross the SAME upcoming
    // portal are one queue, even while they are still spread across a wide
    // room. Without this, a column approaching a doorway only realizes it is
    // a column when bots are already side-by-side at the mouth (lateral
    // distance > 52u and nobody counts as "ahead").
    int myCur = -1, myNext = -1;
    if (m_pathLen > 0 && m_pathCursor >= 0 && m_pathCursor < m_pathLen)
    {
        myNext = m_path[m_pathCursor];
        if (m_pathCursor > 0) myCur = m_path[m_pathCursor - 1];
    }
    bool tightEdge = myCur >= 0 && myNext >= 0
                  && IsTightEdge(myCur, myNext);
    if (tightEdge)
    {
        // These physical frames are single-hull and must always use the
        // serialized choke logic even when a trace sample happens to miss the
        // thin jamb or the bot is between the two short NAV tiles.
        doorInWay = true;
        queueContext = true;
    }
    // Shared-portal reference point for along-route queue ordering.
    Vector portalMid(0.0f, 0.0f, 0.0f);
    bool havePortalMid = false;
    if (myCur >= 0 && myNext >= 0)
    {
        Vector pMid, pAxis; float pHalf = 0.0f;
        if (g_NavMesh.GetPortalMerged(myCur, myNext, pMid, pAxis, pHalf)
            || GetSeparatedTransitionPortal(myCur, myNext,
                                            pMid, pAxis, pHalf, 0))
        {
            portalMid = pMid;
            havePortalMid = true;
        }
    }
    if (queueContext && now >= m_queueBreakUntil)
    {
    for (int i = 0; i < 64; i++)
    {
        CInsBot *other = &g_Bots.m_bots[i];
        if (!other->m_inUse || other == this || !other->m_info || other->m_info->IsDead())
            continue;
        if (other->m_info->GetTeamIndex() != myTeam) continue;

        Vector otherPos = other->m_info->GetAbsOrigin();
        float rx = otherPos.x - pos.x;
        float ry = otherPos.y - pos.y;
        float ahead = rx * laneFwd.x + ry * laneFwd.y;
        float lateral = fabsf(rx * laneSide.x + ry * laneSide.y);
        // Early distance gate: only nearby bots can be in this queue. This
        // also keeps the same-route path scan below cheap.
        if (rx * rx + ry * ry > 750.0f * 750.0f) continue;

        // Global DAG invariant: every predecessor edge must decrease the same
        // stored remaining-route scalar. Near-equal ranks use roster order.
        // This prevents cycles that mix an exact shared-portal edge with a
        // geometric edge at the adjacent portal.
        float rankDelta = other->m_routeRemaining - m_routeRemaining;
        if (rankDelta > 4.0f) continue;
        if (fabsf(rankDelta) <= 4.0f && i > mySlot) continue;

        // Same-route check: both bots must be traversing the exact SAME active
        // directed edge. Scanning the other bot's entire future path allowed
        // follow edges to wrap across consecutive portals: A waited for B at
        // portal 1 while B waited for A at portal 2. Exact active-edge matching
        // gives every shared queue one scalar portal-distance ordering and also
        // removes the O(bot^2 * remaining-path) scan from every frame.
        bool pathShared = false;
        bool adjacentEdgeAhead = false;
        if (myNext >= 0 && other->m_pathLen > 0
            && other->m_pathCursor >= 0 && other->m_pathCursor < other->m_pathLen)
        {
            int otherNext = other->m_path[other->m_pathCursor];
            int otherCur = other->m_pathCursor > 0
                         ? other->m_path[other->m_pathCursor - 1] : otherNext;
            pathShared = (otherNext == myNext && (myCur < 0 || otherCur == myCur));
            // Keep one physical column continuous across very short NAV tiles
            // between consecutive doorway portals. Once the leader advances
            // its cursor it is no longer on the follower's exact edge, but it
            // is still immediately ahead on the same route. The global
            // remaining-route rank above keeps this relation acyclic.
            // Keep ownership continuous until the leader clears the first
            // area beyond any physically verified doorway. Otherwise the
            // follower loses sight of its leader the instant that leader's
            // cursor advances and enters while the rear hull is still in the
            // frame. The physical tight-edge classifier covers that case.
            bool physicalDoorChain = physicalPassage
                                  || IsTightEdge(myCur, myNext);
            adjacentEdgeAhead = physicalDoorChain && !pathShared
                              && otherCur == myNext;
            if (adjacentEdgeAhead) pathShared = true;
        }
        // Near a corridor mouth, treat bots in the adjacent lane (up to two
        // hull widths) as one queue so side-by-side bots trail instead of
        // racing into the narrows. The old 150u cone made bots in the WIDE
        // approach stop for each other even when they were not in the same
        // line, freezing the whole column before it reached the corridor.
        // Drops/steps: a teammate on the landing can be up to ~120u lower,
        // so widen the z window or the follower would not see it and would
        // walk off onto it.
        // Queue metric: for bots on the same route, use the remaining distance
        // to the shared portal (along-route order) instead of the lateral lane
        // projection. Two bots abreast in a wide room have nearly equal portal
        // distance, so they do NOT queue each other and keep moving; a bot
        // strung out behind one closer to the portal follows with a real gap.
        float metric = ahead;
        if (adjacentEdgeAhead)
        {
            float routeGap = m_routeRemaining - other->m_routeRemaining;
            if (routeGap <= 4.0f || routeGap > 300.0f || lateral > 72.0f)
                continue;
            metric = routeGap;
        }
        else if (pathShared && havePortalMid)
        {
            float mdx = pos.x - portalMid.x, mdy = pos.y - portalMid.y;
            float odx = otherPos.x - portalMid.x, ody = otherPos.y - portalMid.y;
            float myDist = sqrtf(mdx * mdx + mdy * mdy);
            float oDist  = sqrtf(odx * odx + ody * ody);
            float pathGap = myDist - oDist;   // >0: other is closer to the portal
            bool nearMouth = myDist <= 120.0f;
            bool strungOut = pathGap >= 40.0f && lateral <= 220.0f;
            bool mouthPair = nearMouth && lateral <= 60.0f;
            bool closeSameEdge = pathGap > 4.0f && lateral <= 72.0f;
            if (!strungOut && !mouthPair && !closeSameEdge) continue;
            if (pathGap <= 0.0f && !mouthPair) continue;
            metric = pathGap;
            if (mouthPair && pathGap <= 4.0f)
            {
                // Both at the mouth: the lower roster slot leads, the other
                // yields so the pair enters single-file. If the other bot is
                // clearly FARTHER from the portal (negative gap), it is not
                // ahead of me - I lead and it must not make me stop, otherwise
                // a rear bot can deadlock the whole column at the mouth.
                // ANY negative gap means the other bot is farther from the
                // portal. With a -4u tolerance, two bots within 4u of equal
                // distance can form a 2-cycle (closer follows farther and
                // vice versa), freezing the whole column at the mouth.
                if (pathGap < 0.0f) continue;
                if (i > mySlot) continue;
                metric = 4.0f;
            }
            // Positive 4-40u mouth gaps are genuine active-edge ordering now.
            // The old code ignored this band to work around rings created by
            // FUTURE-edge matching; with exact active-edge matching those rings
            // are impossible, and ignoring the band merely lets 32u hulls run
            // into each other before either bot brakes.
        }

        float maxLateral = (corridorAhead || dropAhead) ? 72.0f : 52.0f;
        float maxZDiff = (corridorAhead || dropAhead) ? 120.0f : 72.0f;
        if (pathShared)
        {
            if (fabsf(otherPos.z - pos.z) > maxZDiff)
                continue;
            // Same-route, but the other bot must still be BETWEEN me and my
            // waypoint. Without this, a same-route bot far BEHIND me can
            // become my leader through the lane gate (negative gap) when
            // the edge has no portal data, and the follow graph rings,
            // freezing the whole column at a corridor tail.
            if (!adjacentEdgeAhead)
            {
                float myWpDist = sqrtf((pos.x - m_moveTarget.x) * (pos.x - m_moveTarget.x) +
                                       (pos.y - m_moveTarget.y) * (pos.y - m_moveTarget.y));
                float oWpDist = sqrtf((otherPos.x - m_moveTarget.x) * (otherPos.x - m_moveTarget.x) +
                                      (otherPos.y - m_moveTarget.y) * (otherPos.y - m_moveTarget.y));
                if (oWpDist >= myWpDist)
                    continue;
            }
        }
        else
        {
            if (metric > 300.0f || lateral > maxLateral ||
                fabsf(otherPos.z - pos.z) > maxZDiff)
                continue;
            // A bot outside the corridor approach (or on a different route) must
            // not count as a lane predecessor just because it is roughly ahead.
            if (corridorAhead && !dropAhead && lateral > 52.0f)
            {
                bool otherCorridor = other->m_corridorAhead
                                  && other->m_corridorAheadDist <= 320.0f;
                if (!otherCorridor) continue;
            }
            // Different-route neighbor: only queue behind it if its heading
            // is broadly the same as mine AND it is actually between me and
            // my waypoint. Two bots heading to different portals can
            // otherwise follow each other by lane projection (each is ahead
            // of the other in its own lane) and deadlock the spawn cluster.
            float headingDot = 1.0f;
            if (other->m_pathLen > 0 && other->m_pathCursor >= 0
                && other->m_pathCursor < other->m_pathLen)
            {
                const NavAreaData *ja = g_NavMesh.Area(other->m_path[other->m_pathCursor]);
                if (ja)
                {
                    float jdx = ja->center.x - otherPos.x;
                    float jdy = ja->center.y - otherPos.y;
                    float jlen = sqrtf(jdx * jdx + jdy * jdy);
                    if (jlen > 1.0f)
                        headingDot = (jdx / jlen) * laneFwd.x + (jdy / jlen) * laneFwd.y;
                }
            }
            float myWpDist = sqrtf((pos.x - m_moveTarget.x) * (pos.x - m_moveTarget.x) +
                                   (pos.y - m_moveTarget.y) * (pos.y - m_moveTarget.y));
            float oWpDist = sqrtf((otherPos.x - m_moveTarget.x) * (otherPos.x - m_moveTarget.x) +
                                  (otherPos.y - m_moveTarget.y) * (otherPos.y - m_moveTarget.y));
            // Overlapping hulls use roster order as a stable tie-break.
            if (metric <= 4.0f)
            {
                if (metric < -4.0f || i > mySlot || headingDot < 0.9f || oWpDist >= myWpDist)
                    continue;
                metric = 4.0f;
            }
            else if (metric < 40.0f)
            {
                // A near neighbor that is not clearly ahead and not on my
                // route must not count as a queue leader. In a dense spawn
                // cluster these 4-40u lane projections create mutual follow
                // cycles that freeze the whole column at the corridor mouth.
                continue;
            }
            else if (headingDot < 0.9f || oWpDist >= myWpDist)
                continue;
        }

        bool oppositeFlow = false;
        if (other->m_haveNavDest)
        {
            float odx = other->m_navDest.x - otherPos.x;
            float ody = other->m_navDest.y - otherPos.y;
            float olen = sqrtf(odx * odx + ody * ody);
            if (olen > 1.0f)
                oppositeFlow = (odx * laneFwd.x + ody * laneFwd.y) < -0.25f * olen;
        }
        if (oppositeFlow && mySlot < i)
            continue;

        if (metric < predecessorAhead)
        {
            predecessor = i;
            predecessorAhead = metric;
        }
    }
    }

    // A queue is only an intentional wait while it is making physical
    // progress. A stationary leader or an unexpected follow cycle must not
    // disable stuck recovery forever. After four seconds without moving 24u,
    // ignore queue predecessors long enough for normal recovery to run.
    if (predecessor >= 0)
    {
        if (m_queueWaitSince <= 0.0f)
        {
            m_queueWaitSince = now;
            m_queueProgressPos = pos;
        }
        else if (xyd(pos, m_queueProgressPos) >= 24.0f)
        {
            m_queueWaitSince = now;
            m_queueProgressPos = pos;
        }
        m_queueLeaderSlot = predecessor + 1;
        if (now - m_queueWaitSince >= 4.0f)
        {
            BotDbg("[insbots] %s queue watchdog released stalled predecessor %d\n",
                   m_name, predecessor);
            predecessor = -1;
            predecessorAhead = 99999.0f;
            m_queueLeaderSlot = 0;
            m_queueWaitSince = 0.0f;
            m_queueBreakUntil = now + 3.2f;
        }
    }
    else if (now >= m_queueBreakUntil)
    {
        m_queueLeaderSlot = 0;
        m_queueWaitSince = 0.0f;
    }

    // Keep the jump queue fix: a follower at a connected NAV drop may approach
    // its predecessor, but only the unqueued front bot can receive jump input.
    // This gate is applied again after obstacle probing at the end of the
    // controller so LowObstacleAhead cannot silently re-add IN_JUMP.
    if (dropAhead && predecessor >= 0)
        suppressDropJump = true;

    if (predecessor >= 0)
    {
        // Following: queueing is an intentional hold. The path-gap metric
        // above only flags a genuine strung-out line or mouth pair, so a
        // follower waits for the leader instead of burning stuck time and
        // teleporting forward into it (or abandoning the objective). The
        // jiggle/unstuck wiggle is suppressed while a lane predecessor
        // exists (that is what used to make followers turn around or back
        // into the leader).
        m_doorYieldSlot = predecessor + 1;
        m_doorYieldUntil = now + 0.35f;
        m_chokeProgressTime = 0.0f;
        m_chokeBackoffUntil = 0.0f;
        m_dropHoldSince = 0.0f;
        m_blocked = false;
        m_chokeBackedOut = false;
        // Ordinary queue followers wait intentionally. Collision-draining
        // players run this controller only to keep doorway recovery alive;
        // they must retain their stuck clock or ownerless grace can suppress
        // the next recovery session forever.
        bool preserveRecovery = m_bodyNoCollideActive
                             || m_bodyNoCollideEscalating
                             || HasBodyNoCollide()
                             || (m_spawnNoCollideActive
                                 && m_spawnNoCollideExpired);
        if (!preserveRecovery)
        {
            m_stuckSince = 0.0f;
            m_stuckStage = 0;
            m_unstuckUntil = 0.0f;
        }

        // ---- width-aware committed passing ----
        // Only attempt when the current portal actually fits two 32u hulls
        // (half-span >= 56u). The lateral step is derived from the opening
        // width instead of a fixed 48u that could clip a door frame, and the
        // chosen side is committed for 0.75s so bots stop oscillating.
        bool passing = false;
        if (corridorAhead || dropAhead)
        {
            m_passSide = 0.0f;
            m_passUntil = 0.0f;
        }
        if (!(corridorAhead || dropAhead) && m_havePortalSpan && m_portalHalfSpan >= 56.0f && predecessorAhead < 112.0f)
        {
            float off = m_portalHalfSpan * 0.45f;
            if (off < 36.0f) off = 36.0f;
            float maxOff = m_portalHalfSpan - 18.0f;
            if (off > maxOff) off = maxOff;

            if (m_passSide == 0.0f || now >= m_passUntil)
            {
                float sidePref = (mySlot & 1) ? 1.0f : -1.0f;
                float options[2] = { sidePref, -sidePref };
                m_passSide = 0.0f;
                for (int pi = 0; pi < 2 && m_passSide == 0.0f; pi++)
                {
                    Vector probe = pos + laneSide * (options[pi] * off);
                    if (!SideStepOpen(probe)) continue;
                    bool occupied = false;
                    for (int i = 0; i < 64 && !occupied; i++)
                    {
                        CInsBot *other = &g_Bots.m_bots[i];
                        if (!other->m_inUse || other == this || !other->m_info || other->m_info->IsDead())
                            continue;
                        if (other->m_info->GetTeamIndex() != myTeam) continue;
                        Vector p = other->m_info->GetAbsOrigin();
                        if (fabsf(p.z - probe.z) > 72.0f) continue;
                        float rx = p.x - probe.x, ry = p.y - probe.y;
                        if (rx * rx + ry * ry < 56.0f * 56.0f)
                        {
                            occupied = true;
                            break;
                        }
                    }
                    if (!occupied) m_passSide = options[pi];
                }
                if (m_passSide != 0.0f) m_passUntil = now + 0.75f;
            }
            if (m_passSide != 0.0f)
            {
                Vector probe = pos + laneSide * (m_passSide * off);
                bool stillOpen = SideStepOpen(probe);
                for (int i = 0; i < 64 && stillOpen; i++)
                {
                    CInsBot *other = &g_Bots.m_bots[i];
                    if (!other->m_inUse || other == this || !other->m_info || other->m_info->IsDead())
                        continue;
                    if (other->m_info->GetTeamIndex() != myTeam) continue;
                    Vector p = other->m_info->GetAbsOrigin();
                    if (fabsf(p.z - probe.z) > 72.0f) continue;
                    float rx = p.x - probe.x, ry = p.y - probe.y;
                    if (rx * rx + ry * ry < 56.0f * 56.0f) stillOpen = false;
                }
                if (stillOpen)
                {
                    MoveAtWorldYaw(moveYaw + (m_passSide > 0.0f ? 45.0f : -45.0f),
                                   viewYaw, insbot_move_speed.GetFloat() * 0.65f,
                                   fwd, side);
                    passing = true;
                }
                else
                {
                    m_passSide = 0.0f;
                    m_passUntil = 0.0f;
                }
            }
        }
        else
        {
            m_passSide = 0.0f;
            m_passUntil = 0.0f;
        }

        if (!passing)
        {
            // ---- continuous gap-based follow speed ----
            // Keep a compact moving column. A 140u stop gap made an eight-bot
            // squad require nearly 1000u of empty approach space, backing the
            // whole team into rooms before short passages. An 80u center gap
            // still leaves about 48u between 32u hulls while letting the queue
            // fit and flow.
            float maxSpeed = insbot_move_speed.GetFloat();
            float target;
            if (dropAhead)
            {
                // At an elevation drop the queue distance is enforced harder
                // than a corridor so nobody launches onto the landing bot.
                if (predecessorAhead >= 260.0f)
                    target = maxSpeed * 0.85f;
                else if (predecessorAhead > 180.0f)
                    target = maxSpeed * 0.85f * (predecessorAhead - 180.0f) / 80.0f;
                else
                    target = 0.0f;
            }
            else if (corridorAhead)
            {
                // Queueing for a corridor: hold an 80u center gap so the
                // line forms before the mouth. Below the gap the follower
                // STOPS - the old 8-12% creep is exactly what compressed the
                // column into the leader and boxed the doorway. The stuck
                // system treats a stopped queue follower as waiting, and the
                // leader's own recovery opens the line. Above the gap,
                // approach at 85% max so nobody races the leader into the
                // narrows.
                if (predecessorAhead >= 160.0f)
                    target = maxSpeed * 0.85f;
                else if (predecessorAhead > 80.0f)
                    target = maxSpeed * 0.85f * (predecessorAhead - 80.0f) / 80.0f;
                else
                    target = 0.0f;
            }
            else
            {
                // Doorway queue: hold ~55u behind the leader; below it stop
                // instead of creeping into the frame.
                if (predecessorAhead >= 180.0f)
                    target = maxSpeed;
                else if (predecessorAhead > 55.0f)
                    target = maxSpeed * (0.15f + 0.85f * (predecessorAhead - 55.0f) / 125.0f);
                else
                    target = 0.0f;
            }

            float dt = gpGlobals ? gpGlobals->interval_per_tick : 0.015f;
            if (dt < 0.001f) dt = 0.001f;
            if (dt > 0.05f) dt = 0.05f;
            float k = dt * 3.0f;
            if (k > 1.0f) k = 1.0f;
            m_followSpeed += (target - m_followSpeed) * k;
            // Queue contexts hold a REAL gap: no minimum creep below the stop
            // distance - a creep was what compressed the column into the
            // leader. The predecessor scan only runs in a queue context, so
            // the zero floor never applies to open-terrain movement.
            if (target == 0.0f) m_followSpeed = 0.0f;
            float minSpeed = 0.0f;
            if (m_followSpeed < minSpeed) m_followSpeed = minSpeed;
            MoveAtWorldYaw(moveYaw, viewYaw, m_followSpeed, fwd, side);
        }
    }
    else
    {
        // ---- no lane predecessor ----
        m_doorYieldSlot = 0;
        m_doorYieldUntil = 0.0f;
        m_passSide = 0.0f;
        m_passUntil = 0.0f;

        // About to walk off a drop with a teammate already on the landing
        // below: hold at the edge. Creeping off here is what stacks bots on
        // top of each other at elevation steps.
        if (dropAhead && TeammateAtLanding())
        {
            if (!m_dropHoldExpired)
            {
                if (m_dropHoldSince <= 0.0f) m_dropHoldSince = now;
                if (now - m_dropHoldSince < 2.0f)
                {
                    fwd = 0.0f;
                    side = 0.0f;
                    if (m_dropAhead)
                        suppressDropJump = true;
                }
                else
                {
                    m_dropHoldExpired = true;
                    m_dropHoldSince = 0.0f;
                }
            }
        }
        else
        {
            m_dropHoldSince = 0.0f;
            m_dropHoldExpired = false;
        }
        if (doorInWay)
        {
            // Leader at a choke: lane-aligned progress check. Back out once per
            // stall, then hold-creep until the block clears or the stuck system
            // reroutes. Only the leader ever reverses, so the whole column can
            // never chain-back out of a doorway.
            float progress = 0.0f;
            if (m_chokeProgressTime > 0.0f)
            {
                Vector d = pos - m_chokeProgressPos;
                progress = d.x * laneFwd.x + d.y * laneFwd.y;
            }
            if (progress >= 16.0f)
            {
                m_chokeProgressPos = pos;
                m_chokeProgressTime = now;
                m_blocked = false;
                m_chokeBackedOut = false;
                m_chokeBackoffUntil = 0.0f;
            }
            else
            {
                if (m_chokeProgressTime <= 0.0f)
                {
                    m_chokeProgressPos = pos;
                    m_chokeProgressTime = now;
                }
                if (now - m_chokeProgressTime >= 0.9f)
                {
                    m_blocked = true;
                    // A doorway leader uses the configured generic recovery
                    // deadline, not a map-specific short timer. Followers are
                    // intentionally held above, so only the front bot reaches
                    // this branch. Every detected doorway uses ordinary
                    // TeleportUnstuck and its on-path hull checks.
                    float teleportDelay = insbot_doorway_teleport_time.GetFloat();
                    if (teleportDelay < 0.2f) teleportDelay = 0.2f;
                    float bodyStart = insbot_stuck_time.GetFloat();
                    if (bodyStart < 0.2f) bodyStart = 0.2f;
                    if (!tightEdge && !m_bodyNoCollideAttempted
                        && teleportDelay > bodyStart
                        && now - m_chokeProgressTime >= bodyStart)
                    {
                        Vector bodyTarget = m_haveNavDest ? m_navDest
                                          : (m_hasMoveTarget ? m_moveTarget
                                             : pos + laneFwd * 48.0f);
                        BeginBodyNoCollide(pos, bodyTarget, true,
                                           teleportDelay - bodyStart);
                    }
                    bool bodyRecoveryOwns = m_bodyNoCollideActive
                                         || m_bodyNoCollideEscalating;
                    bool transferDue = !bodyRecoveryOwns
                                    && insbot_doorway_teleport.GetBool()
                                    && now - m_chokeProgressTime >= teleportDelay;
                    bool transferred = transferDue && TeleportUnstuck(pos, now, true);
                    if (transferred)
                    {
                        m_chokeProgressTime = 0.0f;
                        m_chokeBackoffUntil = 0.0f;
                        m_chokeBackedOut = false;
                        m_blocked = false;
                        fwd = 0.0f;
                        side = 0.0f;
                        return;
                    }
                    if (!bodyRecoveryOwns && !m_chokeBackedOut)
                    {
                        m_chokeBackedOut = true;
                        m_chokeBackoffUntil = now + 0.8f;
                    }
                }
            }
        }
        else
        {
            m_chokeProgressTime = 0.0f;
            m_chokeBackoffUntil = 0.0f;
            m_blocked = false;
            m_chokeBackedOut = false;

            // Not at a choke: sidestep a nearby teammate outside our lane when
            // there's an open, unoccupied side; otherwise trail slowly instead
            // of shoving into the pack.
            int crowdLeader = -1;
            for (int i = 0; i < 64; i++)
            {
                CInsBot *other = &g_Bots.m_bots[i];
                if (!other->m_inUse || other == this || !other->m_info || other->m_info->IsDead())
                    continue;
                if (other->m_info->GetTeamIndex() != myTeam || i >= mySlot)
                    continue;
                Vector otherPos = other->m_info->GetAbsOrigin();
                float rx = otherPos.x - pos.x, ry = otherPos.y - pos.y;
                float ahead = rx * laneFwd.x + ry * laneFwd.y;
                float lateral = fabsf(rx * laneSide.x + ry * laneSide.y);
                if (ahead > 16.0f && ahead < 112.0f && lateral < 72.0f
                    && fabsf(otherPos.z - pos.z) < 72.0f)
                {
                    crowdLeader = i;
                    break;
                }
            }
            if (crowdLeader >= 0)
            {
                CInsBot *leaderBot = &g_Bots.m_bots[crowdLeader];
                int leaderClient = leaderBot->m_edict ? engine->IndexOfEdict(leaderBot->m_edict) : 0;
                float leaderSpeed = (leaderClient >= 1 && leaderClient <= 65) ? s_hearSpeed[leaderClient] : 0.0f;

                float sideChoice = ((mySlot & 1) ? 1.0f : -1.0f);
                float options[2] = { sideChoice, -sideChoice };
                bool chose = false;
                for (int oi = 0; oi < 2 && !chose; oi++)
                {
                    float s = options[oi];
                    Vector probe = pos + laneSide * (s * 48.0f);
                    if (!SideStepOpen(probe)) continue;
                    bool hasTeammate = false;
                    for (int i = 0; i < 64 && !hasTeammate; i++)
                    {
                        CInsBot *other = &g_Bots.m_bots[i];
                        if (!other->m_inUse || other == this || !other->m_info || other->m_info->IsDead())
                            continue;
                        if (other->m_info->GetTeamIndex() != myTeam) continue;
                        Vector p = other->m_info->GetAbsOrigin();
                        if (fabsf(p.z - pos.z) > 72.0f) continue;
                        float rx = p.x - probe.x, ry = p.y - probe.y;
                        if (rx * rx + ry * ry < 48.0f * 48.0f)
                        {
                            hasTeammate = true;
                            break;
                        }
                    }
                    if (!hasTeammate)
                    {
                        sideChoice = s;
                        chose = true;
                    }
                }
                if (chose)
                {
                    // Gentle 22-degree forward-angled dodge preserving 90% forward momentum
                    float sidestepYaw = moveYaw + (sideChoice > 0.0f ? 22.0f : -22.0f);
                    MoveAtWorldYaw(sidestepYaw, viewYaw,
                                   insbot_move_speed.GetFloat() * 0.90f, fwd, side);
                }
                else
                {
                    // If leader is actively running ahead, match speed smoothly at 85% instead of hard braking
                    if (leaderSpeed >= 45.0f)
                    {
                        MoveAtWorldYaw(moveYaw, viewYaw,
                                       insbot_move_speed.GetFloat() * 0.85f, fwd, side);
                    }
                    else
                    {
                        // Leader is genuinely stopped/stalled: hold back to avoid pushing
                        if (fwd > 0.0f) fwd *= 0.55f;
                        side = 0.0f;
                    }
                }
            }
        }
    }

    // Leader backout window: only the blocked leader reaches here with a live
    // backout timer. Followers return above, so a chain reversal is impossible.
    if (now < m_chokeBackoffUntil)
    {
        // Packed inside a narrow passage: never back into the follower right
        // behind you - that is what wedges a pair inside a corridor/doorway.
        // Cancel the backout, mark it used for this stall, and creep forward
        // so the stuck/teleport systems (which move the bot AHEAD) can work.
        bool packedNarrow = (doorInWay || corridorAhead || dropAhead)
                         && TeammateBehind(moveYaw, 160.0f, 72.0f);
        if (packedNarrow)
        {
            m_chokeBackoffUntil = 0.0f;
            m_chokeBackedOut = true;
            MoveAtWorldYaw(moveYaw, viewYaw,
                           insbot_move_speed.GetFloat() * 0.20f, fwd, side);
        }
        else if (ClearBehind(moveYaw))
            MoveAtWorldYaw(moveYaw + 180.0f, viewYaw,
                           insbot_move_speed.GetFloat() * 0.55f, fwd, side);
        else
            MoveAtWorldYaw(moveYaw, viewYaw,
                           insbot_move_speed.GetFloat() * 0.25f, fwd, side);
        m_doorYieldSlot = 0;
        m_doorYieldUntil = 0.0f;
    }
    else if (m_blocked && doorInWay)
    {
        // Stall persists after the single backout: hold a slow forward creep so
        // the column keeps gentle pressure (and the stuck system can still see
        // a lack of real progress and reroute if this is a dead link).
        MoveAtWorldYaw(moveYaw, viewYaw,
                       insbot_move_speed.GetFloat() * 0.15f, fwd, side);
    }

    if (now >= m_chokeBackoffUntil && m_lowObstacleCached)
        buttons |= IN_JUMP;
    if (suppressDropJump)
        buttons &= ~IN_JUMP;
}

// move toward a world point: nav path if a mesh exists, else roam-steer,
// with automatic hopping over low obstacles.
void CInsBot::MoveToward(const Vector &dest, float &fwd, float &side, int &buttons, QAngle &view)
{
    Vector pos = m_info->GetAbsOrigin();
    bool crowdTransitGrace = m_crowdTransitGraceActive;

    // A completed route and a failed route both make AdvanceWaypoint return
    // false.  The old fallback consequently kept walking at the exact logical
    // destination even after arrival.  NAV area centres (and cover spots in
    // particular) are point targets, so crossing one changed atan2's vector
    // sign and snapped both the view and forward movement through 180 degrees.
    //
    // Latch arrival at 16u and do not resume toward the same point until the
    // bot is pushed beyond 28u.  A changed logical target releases the latch
    // immediately.  Typed ladder traversal is excluded: its physical FSM must
    // retain ownership until the landing is actually clear.
    const float arriveEnter = 16.0f;
    const float arriveExit = 28.0f;
    const float targetChange = 12.0f;
    float destDx = dest.x - pos.x, destDy = dest.y - pos.y;
    float destDistSq = destDx * destDx + destDy * destDy;
    float arrivalDz = fabsf(dest.z - pos.z);
    bool arrivalEligible = m_ladderPhase == LADDER_PHASE_NONE
                        && !m_traversingLadder && arrivalDz <= 18.0f;
    if (m_moveArrivalLatched)
    {
        float tx = dest.x - m_moveArrivalTarget.x;
        float ty = dest.y - m_moveArrivalTarget.y;
        float tz = dest.z - m_moveArrivalTarget.z;
        if (tx * tx + ty * ty > targetChange * targetChange || fabsf(tz) > 64.0f)
        {
            // Do not let the normal repath throttle keep an exhausted path to
            // the previous destination alive after the caller selects a new one.
            m_moveArrivalLatched = false;
            ResetDoorwayRunway();

            m_pathLen = 0;
            m_pathResult = BOT_PATH_NONE;
    m_pathContinues = false;
            m_pathCursor = 0;
            m_hasMoveTarget = false;
            m_repathTime = 0.0f;
            m_haveLastNavMoveYaw = false;
        }
        else if (arrivalEligible && destDistSq <= arriveExit * arriveExit)
        {
            fwd = 0.0f;
            side = 0.0f;
            m_hasMoveTarget = false;
            m_haveNavDest = false;
            m_navTurnAngle = 0.0f;
            return; // preserve the current tactical/view angle
        }
        else
            m_moveArrivalLatched = false;
    }
    if (arrivalEligible && destDistSq <= arriveEnter * arriveEnter)
    {
        m_moveArrivalLatched = true;
        m_moveArrivalTarget = dest;
        // Mark the point route complete.  If the caller changes targets next
        // frame, the target-change branch above forces an immediate fresh path.
        ResetDoorwayRunway();

        m_pathLen = 0;
        m_pathResult = BOT_PATH_NONE;
    m_pathContinues = false;
        m_pathCursor = 0;
        m_hasMoveTarget = false;
        m_repathTime = 0.0f;
        m_haveNavDest = false;
        m_navTurnAngle = 0.0f;
        fwd = 0.0f;
        side = 0.0f;
        return; // an almost-zero vector must never rewrite view.y
    }

    m_havePortalSpan = false;   // AdvanceWaypoint re-populates it when a real portal exists
    Vector immTarget = dest;   // the point we're actually heading straight at now
    float moveYaw = atan2f(destDy, destDx) * (180.0f / 3.14159265f);

    if (g_NavMesh.IsLoaded())
    {
        UpdatePathTo(dest, false);
        bool haveWaypoint = AdvanceWaypoint();
        bool cachedLadderMove = false;
        if (!haveWaypoint && m_ladderPhase != LADDER_PHASE_NONE)
            cachedLadderMove = TraverseLadder(-1, -1, fwd, side, buttons, view, immTarget);

        if (cachedLadderMove)
        {
            moveYaw = atan2f(immTarget.y - pos.y, immTarget.x - pos.x)
                    * (180.0f / 3.14159265f);
        }
        else if (haveWaypoint)
        {
            immTarget = m_moveTarget;   // the current portal/waypoint
            if (m_spawnNoCollideActive || HasBodyNoCollide())
                ResetDoorwayRunway();
            else
                ProbeUpcomingDoorwayRunway(m_doorwayRunway);
            if (m_doorwayRunway.active && m_doorwayRunway.isImmediateEdge
                && m_doorwayRunway.distToPortal > 60.0f
                && m_doorwayRunway.turnAngle > 30.0f)
            {
                if (HasDoorwayLineOfSight(pos, m_doorwayRunway.runwayPoint))
                {
                    immTarget = m_doorwayRunway.runwayPoint;
                }
            }
            if (m_doorwayRunway.active)
            {
                VerifyDoorwayHullClearance(m_doorwayRunway);
            }
            bool ladderMove = m_pathCursor > 0
                && TraverseLadder(m_path[m_pathCursor - 1], m_path[m_pathCursor],
                                  fwd, side, buttons, view, immTarget);
            // Keep driving the climb even if the path edge was lost (see
            // TraverseLadder fallback) so a mid-ladder FindArea miss cannot
            // leave the bot hanging and trigger the jump-unstick logic.
            if (!ladderMove && m_ladderPhase != LADDER_PHASE_NONE)
                ladderMove = TraverseLadder(-1, -1, fwd, side, buttons, view, immTarget);
            bool sourceTurn = m_pathCursor > 0
                && SourceTurnForEdge(m_path[m_pathCursor - 1],
                                     m_path[m_pathCursor], pos);
            if (m_spawnNoCollideActive || HasBodyNoCollide())
            {
                bool keepAlignment = m_pathCursor > 0
                    && IsTightEdge(m_path[m_pathCursor - 1],
                                   m_path[m_pathCursor]);
                if (!keepAlignment) sourceTurn = false;
            }
            float immediateDx = immTarget.x - pos.x;
            float immediateDy = immTarget.y - pos.y;
            float immediateDistSq = immediateDx * immediateDx
                                  + immediateDy * immediateDy;
            float mvYaw;
            if (immediateDistSq > 16.0f * 16.0f)
            {
                mvYaw = atan2f(immediateDy, immediateDx)
                      * (180.0f / 3.14159265f);
                m_lastNavMoveYaw = mvYaw;
                m_haveLastNavMoveYaw = true;
            }
            else
            {
                // Keep crossing a tiny tile/portal along the last valid route
                // direction.  Recomputing yaw from a near-zero vector lets a
                // few units of overshoot reverse the camera every other tick.
                mvYaw = m_haveLastNavMoveYaw ? m_lastNavMoveYaw : moveYaw;
            }
            moveYaw = mvYaw;
            // Ease off before a sharp turn only when BSP traces confirmed the
            // current opening. Open-map NAV zigzags retain full speed.
            float navSpeed = insbot_move_speed.GetFloat();
            if (m_crowdTransitGraceActive
                || (m_spawnNoCollideActive && !m_spawnNoCollideExpired))
                navSpeed *= PT_CROWD_APPROACH_SPEED_SCALE;
            // Virtual transition staging points sit only a few units before a
            // drop lip. Approaching them at full speed makes the bot orbit the
            // lip as virtualApproach flips on and off each tick.
            if (m_transitionApproach)
            {
                float stageDx = m_moveTarget.x - pos.x;
                float stageDy = m_moveTarget.y - pos.y;
                float stageDist = sqrtf(stageDx * stageDx + stageDy * stageDy);
                if (stageDist < 96.0f)
                {
                    float scale = stageDist / 96.0f;
                    if (scale < 0.30f) scale = 0.30f;
                    navSpeed *= scale;
                }
            }
            if (sourceTurn && !m_traversingLadder
                && m_pathCursor + 1 < m_pathLen && m_hasMoveTarget)
            {
                Vector nextWp;
                Vector nAxis; float nHalf = 0.0f;
                bool haveNext = g_NavMesh.GetPortalMerged(m_path[m_pathCursor],
                                                          m_path[m_pathCursor + 1],
                                                          nextWp, nAxis, nHalf);
                if (!haveNext)
                    haveNext = GetSeparatedTransitionPortal(m_path[m_pathCursor],
                                                            m_path[m_pathCursor + 1],
                                                            nextWp, nAxis, nHalf, 0);
                if (!haveNext)
                {
                    const NavAreaData *na = g_NavMesh.Area(m_path[m_pathCursor + 1]);
                    if (na) { nextWp = na->center; haveNext = true; }
                }
                if (haveNext)
                {
                    float curYaw = atan2f(m_moveTarget.y - pos.y,
                                          m_moveTarget.x - pos.x) * (180.0f / 3.14159265f);
                    float nxtYaw = atan2f(nextWp.y - m_moveTarget.y,
                                          nextWp.x - m_moveTarget.x) * (180.0f / 3.14159265f);
                    float turnAng = fabsf(SignedAngleDiff(nxtYaw, curYaw));
                    float distWp = sqrtf((m_moveTarget.x - pos.x) * (m_moveTarget.x - pos.x)
                                       + (m_moveTarget.y - pos.y) * (m_moveTarget.y - pos.y));
                    if (turnAng > 35.0f && distWp < 240.0f)
                    {
                        float t = (240.0f - distWp) / 240.0f;
                        if (t < 0.0f) t = 0.0f;
                        if (t > 1.0f) t = 1.0f;
                        float minSpeed = turnAng > 75.0f ? 0.35f
                                       : (turnAng > 50.0f ? 0.45f : 0.60f);
                        navSpeed *= (1.0f - t * (1.0f - minSpeed));
                    }
                }
            }
            if (!ladderMove && m_enemyIndex)
            {
                float rel = mvYaw * (3.14159265f / 180.0f) - view.y * (3.14159265f / 180.0f);
                fwd  = cosf(rel) * navSpeed;
                side = -sinf(rel) * navSpeed;
            }
            else if (!ladderMove)
            {
                SmoothNavTurn(mvYaw, navSpeed, view,
                              fwd, side, m_navTurnAngle, sourceTurn);

                // --- Doorway runway alignment override (post-SmoothNavTurn) ---
                if (m_doorwayRunway.active)
                {
                    Vector toPortal = pos - m_doorwayRunway.center;
                    float alongNormal = toPortal.x * m_doorwayRunway.normal.x + toPortal.y * m_doorwayRunway.normal.y;
                    if (alongNormal > 16.0f)
                    {
                        // Bot has passed through portal; release camera and trajectory override
                        m_doorwayRunway.active = false;
                    }
                }

                if (m_doorwayRunway.active && m_doorwayRunway.isImmediateEdge
                    && m_doorwayRunway.distToPortal < 140.0f
                    && HasDoorwayLineOfSight(pos, m_doorwayRunway.center))
                {
                    float doorYaw = atan2f(m_doorwayRunway.normal.y, m_doorwayRunway.normal.x) * (180.0f / 3.14159265f);
                    float turnDiff = SignedAngleDiff(doorYaw, view.y);
                    float maxStep = 360.0f * (gpGlobals ? gpGlobals->interval_per_tick : 0.015f);
                    if (turnDiff > maxStep) turnDiff = maxStep;
                    else if (turnDiff < -maxStep) turnDiff = -maxStep;
                    view.y = fmodf(view.y + turnDiff + 360.0f, 360.0f);
                    view.x = 0.0f;

                    Vector toBot = pos - m_doorwayRunway.center;
                    float crossTrackError = toBot.x * (-m_doorwayRunway.normal.y) + toBot.y * (m_doorwayRunway.normal.x);

                    float strafe = -crossTrackError * 4.0f;
                    if (strafe > 55.0f) strafe = 55.0f;
                    if (strafe < -55.0f) strafe = -55.0f;

                    fwd = navSpeed;
                    side = strafe;
                }
            }

            const NavAreaData *cur = g_NavMesh.Area(m_path[m_pathCursor]);
            bool canJump = g_NavMesh.CanJumpFrom(CurrentArea(pos));
            if (cur && !ladderMove)
            {
                // Crouch only on imminent physical contact. nav_generate
                // over-flags CROUCH on corridor tiles, so the flag alone no
                // longer ducks bots while they stream through a passage; the
                // LowCeilingAhead trace below only fires when a real overhang
                // is within a few feet.
                if ((cur->attributes & NAVATTR_JUMP) && canJump)
                    buttons |= IN_JUMP;
            }
            if (!ladderMove && LowCeilingAhead(mvYaw))
                buttons |= IN_DUCK;
            // Only jump when the waypoint is genuinely above the player - well
            // above the Source engine's 21u jump height so ramps, gentle steps,
            // and multi-Z nav area centres don't cause constant mid-ramp jumping.
            // 36u = floor-to-waist; if the target is that high you actually need
            // to jump or climb to reach it.
            bool edgeStep = false;
            if (!ladderMove && m_pathCursor >= 0 && m_pathCursor < m_pathLen)
            {
                int prevArea = (m_pathCursor > 0) ? m_path[m_pathCursor - 1] : CurrentArea(pos);
                if (prevArea >= 0 && prevArea != m_path[m_pathCursor])
                {
                    edgeStep = WalkEdgeNeedsJump(prevArea, m_path[m_pathCursor]);
                    if (!edgeStep)
                    {
                        Vector seam;
                        if (!g_NavMesh.GetPortal(prevArea, m_path[m_pathCursor], seam))
                            edgeStep = m_moveTarget.z > pos.z + 12.0f;
                    }
                }
            }
            if (!canJump) edgeStep = false;
            if (edgeStep && xyd(pos, m_moveTarget) < 96.0f)
                buttons |= IN_JUMP;
        }
        else
        {
            ResetDoorwayRunway();
            // no path found (or already in dest area): walk straight at it,
            // but only when the endpoint itself is off-mesh or NAV state is
            // neutral. A valid start+goal with no directed route is
            // BOT_PATH_UNREACHABLE: direct steering would bypass the graph.
            if (m_pathResult == BOT_PATH_UNREACHABLE)
            {
                fwd = 0.0f;
                side = 0.0f;
                m_hasMoveTarget = false;
                m_haveNavDest = false;
            }
            else if (m_pathResult == BOT_PATH_NONE)
            {
                m_headingYaw = atan2f(dest.y - pos.y, dest.x - pos.x) * (180.0f / 3.14159265f);
                SteerRoam(fwd, side, buttons, view);
            }
            else
            {
                float mvYaw = atan2f(dest.y - pos.y, dest.x - pos.x) * (180.0f / 3.14159265f);
                moveYaw = mvYaw;
                if (m_enemyIndex)
                {
                    float rel = mvYaw * (3.14159265f / 180.0f) - view.y * (3.14159265f / 180.0f);
                    fwd  = cosf(rel) * insbot_move_speed.GetFloat() * 0.6f;
                    side = -sinf(rel) * insbot_move_speed.GetFloat() * 0.6f;
                }
                else
                {
                    SteerToPoint(mvYaw, insbot_move_speed.GetFloat() * 0.6f,
                                 fwd, side, buttons, view);
                }
            }
        }
    }
    else
    {
        // no mesh: bias roam heading toward the destination, keep wall-avoidance
        m_headingYaw = atan2f(dest.y - pos.y, dest.x - pos.x) * (180.0f / 3.14159265f);
        SteerRoam(fwd, side, buttons, view);
        moveYaw = view.y;
    }

    // Register the IMMEDIATE target (waypoint) for progress-based stuck
    // detection. The bot heads straight at this, so approaching it is monotonic
    // and a nav detour (advancing through waypoints) reads as progress, not
    // "stuck". Baseline resets whenever the waypoint changes.
    // reset the progress baseline whenever the waypoint meaningfully changes.
    // (was 100u - but consecutive waypoints in dense nav are CLOSER than that,
    // so the baseline never reset and bots read as falsely "stuck" while
    // walking fine, thrashing the unstuck wiggle in circles)
    Vector dd = immTarget - m_navDest;
    float targetChangeSq = dd.x * dd.x + dd.y * dd.y;
    if (m_traversingLadder) targetChangeSq += dd.z * dd.z;
    if (targetChangeSq > 24.0f * 24.0f)
    {
        float ndx = immTarget.x - pos.x, ndy = immTarget.y - pos.y;
        float ndz = m_traversingLadder ? immTarget.z - pos.z : 0.0f;
        m_bestDist = sqrtf(ndx * ndx + ndy * ndy + ndz * ndz);
        m_navDestChanged = true;
    }
    m_navDest = immTarget;
    m_haveNavDest = true;
    if (!m_traversingLadder)
    {
        bool collisionGrace = HasBodyNoCollide() || m_spawnNoCollideActive
                           || crowdTransitGrace;
        if (collisionGrace)
        {
            // Keep the choke/recovery controller alive while collision drains.
            // Preserve a connected-area drop queue because collision grace
            // removes player blocking, not the landing or jump geometry.
            float recoveryFwd = fwd, recoverySide = side;
            int recoveryButtons = buttons;
            float oldTeleportOk = m_nextTeleportOk;
            ApplyCrowdSteering(moveYaw, view.y, recoveryFwd, recoverySide,
                               recoveryButtons);
            if (m_dropAhead || crowdTransitGrace)
            {
                fwd = recoveryFwd;
                side = recoverySide;
                buttons = recoveryButtons;
            }
            if (m_nextTeleportOk > oldTeleportOk)
            {
                fwd = 0.0f;
                side = 0.0f;
                buttons = 0;
            }
        }
        else
            ApplyCrowdSteering(moveYaw, view.y, fwd, side, buttons);
    }
    if (m_crowdTransitGraceActive && !m_enemyIndex
        && (fabsf(fwd) > 10.0f || fabsf(side) > 10.0f))
    {
        float oldViewY = view.y;
        float speed = sqrtf(fwd * fwd + side * side);
        float worldMoveYaw = oldViewY + atan2f(-side, fwd) * (180.0f / 3.14159265f);
        view.y = m_haveLastNavMoveYaw ? m_lastNavMoveYaw : moveYaw;
        view.x = 0.0f;
        MoveAtWorldYaw(worldMoveYaw, view.y, speed, fwd, side);
    }
}
