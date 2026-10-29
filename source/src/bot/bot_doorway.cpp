// ============================================================================
// bot_doorway.cpp - doorway, choke and low-obstacle geometry probes.
//
// Bots have to press +use at INS:MIC doors (they never open by touch), duck
// under real overhangs, hop genuine low obstacles, and serialize through
// single-hull gaps. Everything here answers a physical question about the
// geometry just ahead so the steering and choke controllers can act on it.
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

// ---- externs this TU needs (same pattern as bot.cpp / objective.cpp) -------
extern IVEngineServer       *engine;
extern IEngineTrace         *enginetrace;
extern IPlayerInfoManager   *playerinfomanager;
extern CGlobalVars          *gpGlobals;
extern CfgVar                insbot_doorway_sprint;

// probe straight ahead at knee and head height along yawDeg; if something
// blocks the knees but not the head/upper-body, it's a low obstacle we can hop.
//
// Uses MASK_PLAYERSOLID (not MASK_PLAYERSOLID_BRUSHONLY) so prop_physics /
// prop_static sandbags, crates and other non-BSP obstacles are included in the
// collision test. Without this, bots would see a prop wall as "knee clear" and
// waste ticks trying to jump something they can never vault.
//
// Three probes:
//  knee  (z+22): must be BLOCKED  - there is an obstacle to hop
//  waist (z+38): must be CLEAR    - not a full-height wall
//  upper (z+62): must be CLEAR    - genuine clearance above the obstacle
bool CInsBot::LowObstacleAhead(float yawDeg) const
{
    INSPROF_SCOPE(INSPROF_LOWOBSTACLE);
    Vector pos = m_info->GetAbsOrigin();
    float yr = yawDeg * (3.14159265f / 180.0f);
    Vector dir(cosf(yr), sinf(yr), 0);

    // Use MASK_PLAYERSOLID so props (sandbags, crates, barrels) are caught.
    // MASK_PLAYERSOLID_BRUSHONLY only tests BSP brushes and misses all model-
    // based props, which is exactly the class of object bots were hopping at.
    CTraceFilterWorldOnly wf;
    Ray_t ray; trace_t t;

    // Multi-height low obstacle detection:
    // 1. shin (z+14): catches 14u-24u curbs, step lips, and elevation seams (sv_stepsize is 18u)
    Vector shin = pos + Vector(0, 0, 14.0f);
    ray.Init(shin, shin + dir * 42.0f);
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &wf, &t);
    bool shinBlocked = (t.fraction < 1.0f);

    // 2. knee (z+22): catches crates, sandbags, and obstacles up to 34u
    Vector knee  = pos + Vector(0, 0, 22.0f);
    ray.Init(knee, knee + dir * 42.0f);
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &wf, &t);
    bool kneeBlocked = (t.fraction < 1.0f);

    if (!shinBlocked && !kneeBlocked) return false;    // nothing blocking at step or knee height

    // waist: z+38 - must be clear (full wall blocks here too - not hoppable)
    Vector waist = pos + Vector(0, 0, 38);
    ray.Init(waist, waist + dir * 42.0f);
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &wf, &t);
    if (t.fraction < 1.0f) return false;    // wall continues above knee - can't hop

    // upper: z+62 - must also be clear (confirms genuine headroom above the obstacle)
    Vector upper = pos + Vector(0, 0, 62);
    ray.Init(upper, upper + dir * 42.0f);
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &wf, &t);
    if (t.fraction < 1.0f) return false;

    // A rising displacement also intersects the horizontal knee ray while
    // leaving waist/head clear. Sample its floor profile before calling it a
    // jump obstacle: gradual height gain is a slope, while a crate/step has an
    // abrupt vertical increment between samples.
    float groundZ[4];
    const float sampleDist[4] = { 0.0f, 14.0f, 28.0f, 42.0f };
    CTraceFilterWorldAndPropsOnly gf;
    for (int i = 0; i < 4; i++)
    {
        Vector p = pos + dir * sampleDist[i];
        ray.Init(p + Vector(0, 0, 80), p - Vector(0, 0, 64));
        enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &gf, &t);
        if (t.startsolid || t.allsolid || t.fraction >= 1.0f)
            return true;
        groundZ[i] = t.endpos.z;
    }
    float totalRise = groundZ[3] - groundZ[0];
    if (totalRise > 4.0f && totalRise <= 48.0f)
    {
        bool gradual = true;
        for (int i = 1; i < 4; i++)
        {
            float step = groundZ[i] - groundZ[i - 1];
            if (step > 16.0f || step < -18.0f) { gradual = false; break; }
        }
        if (gradual) return false;
    }
    return true;
}

// Overhang ahead at head height while the knee path is clear -> we need to duck
// to pass under it. This is a PHYSICAL check: bots only crouch for things that
// are actually there, not because nav_generate over-flagged an area as CROUCH.
bool CInsBot::LowCeilingAhead(float yawDeg) const
{
    Vector pos = m_info->GetAbsOrigin();
    float yr = yawDeg * (3.14159265f / 180.0f);
    Vector dir(cosf(yr), sinf(yr), 0);

    CTraceFilterWorldOnly wf;
    Ray_t ray; trace_t t;

    Vector knee = pos + Vector(0, 0, 24);
    Vector head = pos + Vector(0, 0, 62);

    ray.Init(knee, knee + dir * 45.0f);
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &wf, &t);
    bool kneeClear = (t.fraction >= 1.0f);

    ray.Init(head, head + dir * 45.0f);
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &wf, &t);
    bool headBlocked = (t.fraction < 1.0f);

    // head blocked but knees clear = low overhang (a wall blocks both = not a duck case)
    return kneeClear && headBlocked;
}

// no standing headroom at p (used to confirm a CROUCH-flagged waypoint really is low)
bool CInsBot::CeilingLowAt(const Vector &p) const
{
    CTraceFilterWorldOnly wf;
    Ray_t ray; trace_t t;
    ray.Init(p + Vector(0, 0, 20), p + Vector(0, 0, 70));
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &wf, &t);
    return t.fraction < 1.0f;
}

// Closed door directly ahead? INS:MIC doors are func_door_rotating with the
// "Use Opens" spawnflag (1280 on every real door in the stock maps), so bots
// must press +use at them - they never open by touch. Entity-aware trace at
// waist height along the movement direction.

static bool IsDoorClosedOrClosing(void *doorBase, const char *cn)
{
    if (!doorBase || !cn) return true;
    if (!strncmp(cn, "func_door", 9))
    {
#ifdef _LINUX
        int state = *(int *)((char *)doorBase + 0x344);
#else
        int state = *(int *)((char *)doorBase + 0x330);
#endif
        // TS_AT_TOP = 0 (open), TS_GOING_UP = 2 (opening)
        if (state == 0 || state == 2)
            return false;
        return true;
    }
    else if (!strncmp(cn, "prop_door", 9))
    {
#ifdef _LINUX
        int state = *(int *)((char *)doorBase + 0x5F4);
#else
        int state = *(int *)((char *)doorBase + 0x5E0);
#endif
        // DOOR_STATE_OPENING = 1, DOOR_STATE_OPEN = 2
        if (state == 1 || state == 2)
            return false;
        return true;
    }
    return true;
}


bool HasDoorwayLineOfSight(const Vector &from, const Vector &to)
{
    if (!enginetrace) return true;
    Ray_t ray;
    ray.Init(from + Vector(0, 0, 36.0f), to + Vector(0, 0, 36.0f));
    CTraceFilterWorldOnly filter;
    trace_t tr;
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &filter, &tr);
    return !tr.startsolid && !tr.allsolid && tr.fraction >= 0.95f;
}

bool CInsBot::ProbeUpcomingDoorwayRunway(DoorwayRunwayState &outRunway)
{
    if (!insbot_doorway_sprint.GetBool())
    {
        outRunway.active = false;
        return false;
    }
    if (m_pathLen <= 0 || m_pathCursor >= m_pathLen || !m_info)
    {
        outRunway.active = false;
        return false;
    }

    Vector pos = m_info->GetAbsOrigin();
    int startIdx = (m_pathCursor > 0) ? m_pathCursor - 1 : 0;
    int maxCheck = (m_pathCursor + 2 < m_pathLen) ? m_pathCursor + 2 : m_pathLen - 1;

    for (int i = startIdx; i <= maxCheck && i + 1 < m_pathLen; i++)
    {
        int fromIdx = m_path[i];
        int toIdx = m_path[i + 1];
        Vector safeCenter; float safeHalf = 0.0f;
        if (g_NavMesh.GetPhysicalPortal(fromIdx, toIdx, safeCenter, safeHalf))
        {
            float dx = safeCenter.x - pos.x;
            float dy = safeCenter.y - pos.y;
            float distSq = dx * dx + dy * dy;
            if (distSq <= 220.0f * 220.0f)
            {
                const NavAreaData *fromA = g_NavMesh.Area(fromIdx);
                const NavAreaData *toA = g_NavMesh.Area(toIdx);
                if (!fromA || !toA) continue;

                float lx = fromA->lo.x > toA->lo.x ? fromA->lo.x : toA->lo.x;
                float hx = fromA->hi.x < toA->hi.x ? fromA->hi.x : toA->hi.x;
                float ly = fromA->lo.y > toA->lo.y ? fromA->lo.y : toA->lo.y;
                float hy = fromA->hi.y < toA->hi.y ? fromA->hi.y : toA->hi.y;

                Vector seamAxis = ((hx - lx) <= (hy - ly))
                                ? Vector(0.0f, 1.0f, 0.0f)
                                : Vector(1.0f, 0.0f, 0.0f);
                Vector normal(seamAxis.y, -seamAxis.x, 0.0f);
                Vector toTravel = toA->center - fromA->center;
                if (toTravel.x * normal.x + toTravel.y * normal.y < 0.0f)
                {
                    normal.x = -normal.x;
                    normal.y = -normal.y;
                }

                // Check half-space approach: if bot is already past portal threshold, skip to next edge
                float dotNormal = (pos.x - safeCenter.x) * normal.x + (pos.y - safeCenter.y) * normal.y;
                if (dotNormal > 16.0f)
                    continue;

                Vector runwayPoint = safeCenter - normal * 120.0f;
                runwayPoint.z = safeCenter.z;

                bool prevBraked = m_doorwayRunway.active
                    && (fabsf(m_doorwayRunway.center.x - safeCenter.x) < 4.0f)
                    && (fabsf(m_doorwayRunway.center.y - safeCenter.y) < 4.0f)
                    && m_doorwayRunway.apexBraked;

                outRunway.active = true;
                outRunway.center = safeCenter;
                outRunway.normal = normal;
                outRunway.runwayPoint = runwayPoint;
                outRunway.halfSpan = safeHalf;
                outRunway.distToPortal = sqrtf(distSq);
                outRunway.apexBraked = prevBraked;
                int currentEdgeIdx = (m_pathCursor > 0) ? (m_pathCursor - 1) : 0;
                outRunway.isImmediateEdge = (i == currentEdgeIdx);

                float portalYaw = atan2f(normal.y, normal.x) * (180.0f / 3.14159265f);
                float travelYaw = m_haveLastNavMoveYaw ? m_lastNavMoveYaw : m_curAngles.y;
                outRunway.turnAngle = fabsf(SignedAngleDiff(portalYaw, travelYaw));

                outRunway.isChained = false;
                if (i + 2 < m_pathLen)
                {
                    Vector nextCenter; float nextHalf = 0.0f;
                    if (g_NavMesh.GetPhysicalPortal(m_path[i + 1],
                                                    m_path[i + 2],
                                                    nextCenter, nextHalf))
                    {
                        float cdx = nextCenter.x - safeCenter.x, cdy = nextCenter.y - safeCenter.y;
                        if (cdx * cdx + cdy * cdy <= 130.0f * 130.0f)
                            outRunway.isChained = true;
                    }
                }
                return true;
            }
        }
    }
    outRunway.active = false;
    return false;
}

bool CInsBot::VerifyDoorwayHullClearance(DoorwayRunwayState &runway)
{
    if (!enginetrace || !runway.active) return true;

    Vector start = runway.center - runway.normal * 30.0f;
    Vector end   = runway.center + runway.normal * 48.0f;
    start.z += 16.0f;
    end.z   += 16.0f;

    Ray_t ray;
    // Elevated sweep: 16u off floor up to 72u eye height (56u ray height)
    ray.Init(start, end, Vector(-16, -16, 0), Vector(16, 16, 56));

    CTraceFilterSkipPlayers filter(EdictHandleEntity(m_edict));
    trace_t tr;
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &tr);

    if (tr.fraction < 1.0f)
    {
        if (tr.m_pEnt)
        {
            IServerUnknown *unk = (IServerUnknown *)tr.m_pEnt;
            if (unk && unk->GetNetworkable())
            {
                const char *cn = unk->GetNetworkable()->GetClassName();
                if (cn && (!strncmp(cn, "func_door", 9) || !strncmp(cn, "prop_door", 9)))
                {
                    runway.isSwingDoor = true;
                    runway.doorClear = false;
                    return false;
                }
            }
        }
        runway.doorClear = false;
        return false;
    }

    runway.doorClear = true;
    return true;
}

bool CInsBot::TeammateInDoorwayCorridor(const DoorwayRunwayState &runway, int mySlot) const
{
    if (!runway.active || !m_info) return false;
    Vector pos = m_info->GetAbsOrigin();
    Vector toMe = pos - runway.center;
    float myAlongNormal = toMe.x * runway.normal.x + toMe.y * runway.normal.y;

    for (int i = 0; i < gpGlobals->maxClients; i++)
    {
        if (i == mySlot) continue;
        const CInsBot *other = &g_Bots.m_bots[i];
        if (!other || !other->m_inUse || !other->m_info || other->m_info->IsDead())
            continue;
        if (other->m_teamSlot != m_teamSlot) continue;

        Vector otherPos = other->m_info->GetAbsOrigin();
        Vector toOther = otherPos - runway.center;
        float distSq = toOther.x * toOther.x + toOther.y * toOther.y;
        if (distSq > 160.0f * 160.0f) continue;

        float alongNormal = toOther.x * runway.normal.x + toOther.y * runway.normal.y;
        // Only count teammates who are ahead of me along the approach corridor
        if (alongNormal > (myAlongNormal + 16.0f) && alongNormal <= 48.0f)
        {
            return true;
        }
    }
    return false;
}

bool CInsBot::DoorAhead(float yawDeg, Vector *doorOut) const
{
    INSPROF_SCOPE(INSPROF_DOOR);
    if (!m_edict || !m_info) return false;
    Vector pos = m_info->GetAbsOrigin();
    float yr = yawDeg * (3.14159265f / 180.0f);
    Vector dir(cosf(yr), sinf(yr), 0);
    Vector start = pos + Vector(0, 0, 36);

    CTraceFilterSkipPlayers f(EdictHandleEntity(m_edict));
    Ray_t ray; trace_t tr;
    ray.Init(start, start + dir * 180.0f);
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &f, &tr);
    if (tr.fraction >= 1.0f || !tr.m_pEnt) return false;

    IServerUnknown *unk = (IServerUnknown *)tr.m_pEnt;
    IServerNetworkable *net = unk->GetNetworkable();
    if (!net) return false;
    edict_t *e = net->GetEdict();
    if (!e) return false;
    const char *cn = e->GetClassName();
    bool isDoor = !strncmp(cn, "func_door", 9) || !strncmp(cn, "prop_door", 9);
    if (isDoor)
    {
        void *base = unk->GetBaseEntity();
        if (!IsDoorClosedOrClosing(base, cn))
            return false; // Door is already OPEN or currently OPENING! Do not touch or close!
        if (doorOut) *doorOut = tr.endpos;
        return true;
    }
    return false;
}

// Robust doorway/archway/chokepoint detection. Casts a center ray plus two
// side rays (offset 36u, clear of the 32u player hull) forward up to 200u.
// It also samples floor and lateral clearance farther ahead.  That second test
// keeps working after a bot enters a tight corridor and recognizes open-sided
// planks, where ordinary wall-only choke traces see nothing at all.
bool CInsBot::ChokeAhead(float yawDeg, Vector *chokeOut) const
{
    INSPROF_SCOPE(INSPROF_CHOKE);
    if (!m_info) return false;
    Vector pos = m_info->GetAbsOrigin();
    float yr = yawDeg * (3.14159265f / 180.0f);
    Vector fwdDir(cosf(yr), sinf(yr), 0);
    Vector sideDir(-sinf(yr), cosf(yr), 0);
    Vector waist = pos + Vector(0, 0, 36);

    CTraceFilterWorldAndPropsOnly wf;
    Ray_t ray; trace_t tr;
    const float maxDist = 200.0f;

    // center lane: must stay open at least 40u, otherwise it's a wall or
    // closed door right in front (DoorAhead handles the +use case), not a
    // passable chokepoint.
    ray.Init(waist, waist + fwdDir * maxDist);
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &wf, &tr);
    float centerHit = tr.fraction * maxDist;
    if (tr.startsolid || centerHit < 40.0f) return false;

    // side lanes: offset 36u from the bot's centerline. A 48u IMIC door's
    // jamb sits ~24u from center, so the probe clears the hull and still
    // meets the frame; wider openings/corridors are caught too.
    float sideHit[2];
    float sideUpperHit[2];
    bool sideTraceValid[2] = { true, true };
    for (int s = 0; s < 2; s++)
    {
        Vector start = waist + sideDir * (s ? -36.0f : 36.0f);
        ray.Init(start, start + fwdDir * maxDist);
        enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &wf, &tr);
        if (tr.startsolid || tr.allsolid || tr.fraction <= 0.001f)
        {
            sideTraceValid[s] = false;
            sideHit[s] = 0.0f;
        }
        else
            sideHit[s] = tr.fraction * maxDist;

        Vector upperStart = pos + Vector(0, 0, 62)
                          + sideDir * (s ? -36.0f : 36.0f);
        ray.Init(upperStart, upperStart + fwdDir * maxDist);
        enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &wf, &tr);
        sideUpperHit[s] = (!tr.startsolid && !tr.allsolid)
                        ? tr.fraction * maxDist : 0.0f;
    }

    // both sides must close in before the center lane does, and roughly at
    // the same depth, or it's just a wall beside a passage.
    bool wallChoke = sideTraceValid[0] && sideTraceValid[1]
                  && sideHit[0] < maxDist && sideHit[1] < maxDist
                  && sideHit[0] < centerHit - 20.0f
                  && sideHit[1] < centerHit - 20.0f
                  && sideUpperHit[0] < maxDist && sideUpperHit[1] < maxDist
                  && fabsf(sideUpperHit[0] - sideHit[0]) <= 40.0f
                  && fabsf(sideUpperHit[1] - sideHit[1]) <= 40.0f
                  && fabsf(sideHit[0] - sideHit[1]) <= 90.0f;
    if (wallChoke)
    {
        if (chokeOut)
        {
            float d = sideHit[0] < sideHit[1] ? sideHit[0] : sideHit[1];
            chokeOut->x = waist.x + fwdDir.x * d;
            chokeOut->y = waist.y + fwdDir.y * d;
            chokeOut->z = pos.z;
        }
        return true;
    }

    // Cross-section test: the center must have walkable support while both
    // shoulder lanes are unavailable because of either solid geometry or a
    // drop.  This catches the inside of a corridor as well as narrow planks.
    const float samples[3] = { 48.0f, 96.0f, 144.0f };
    for (int n = 0; n < 3; n++)
    {
        Vector center = pos + fwdDir * samples[n];
        Vector top = center + Vector(0, 0, 24);
        Vector bottom = center - Vector(0, 0, 72);
        ray.Init(top, bottom);
        enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &wf, &tr);
        if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f)
            continue;                       // no supported center lane here

        int unavailableSides = 0;
        for (int s = 0; s < 2; s++)
        {
            Vector lateral = center + sideDir * (s ? -40.0f : 40.0f);

            // A real side wall blocks both waist and upper-body rays. Rising
            // terrain often blocks only the waist ray and must remain usable.
            ray.Init(center + Vector(0, 0, 36), lateral + Vector(0, 0, 36));
            enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &wf, &tr);
            bool waistBlocked = tr.startsolid || tr.allsolid || tr.fraction < 1.0f;
            ray.Init(center + Vector(0, 0, 62), lateral + Vector(0, 0, 62));
            enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &wf, &tr);
            bool upperBlocked = tr.startsolid || tr.allsolid || tr.fraction < 1.0f;
            if (waistBlocked && upperBlocked)
            {
                unavailableSides++;
                continue;
            }

            // Use a deeper support probe than the old 72u ray. Rolling ground
            // beside the route is not a plank edge; only a substantial drop
            // with no support in 144u counts as an unavailable shoulder.
            ray.Init(lateral + Vector(0, 0, 24), lateral - Vector(0, 0, 144));
            enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &wf, &tr);
            if (!tr.startsolid && !tr.allsolid && tr.fraction >= 1.0f)
                unavailableSides++;         // open edge/drop beside a plank
        }

        if (unavailableSides == 2)
        {
            if (chokeOut)
            {
                chokeOut->x = center.x;
                chokeOut->y = center.y;
                chokeOut->z = pos.z;
            }
            return true;
        }
    }
    return false;
}
