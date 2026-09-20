// ============================================================================
// bot_haditha.cpp - Haditha map-specific doorway handling.
//
// Haditha ships two malformed NAV doorways whose openings are narrower than a
// player hull, so bots wedge in them and ordinary unstuck recovery makes the
// jam worse. Everything here is keyed to the immutable NAV ids verified on that
// map (464 -> 3321 -> 463 -> 3277 and the 3295/3315 -> 320 east door) and is
// inert on every other map.
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "tier0/dbg.h"
#include "interface.h"
#include "eiface.h"
#include "edict.h"
#include "dlls/iplayerinfo.h"
#include "engine/iserverplugin.h"
#include "engine/IEngineTrace.h"
#include "gametrace.h"
#include "bspflags.h"
#include "const.h"
#include "convar.h"
#include "string_t.h"
#include "bot.h"
#include "nav.h"
#include "objective.h"
#include "bot_internal.h"

// ---- externs this TU needs (same pattern as bot.cpp / objective.cpp) -------
extern IVEngineServer       *engine;
extern IEngineTrace         *enginetrace;
extern IPlayerInfoManager   *playerinfomanager;
extern CGlobalVars          *gpGlobals;
extern IBotManager          *botmanager;
extern CfgVar                insbot_doorway_teleport;
extern CfgVar                insbot_doorway_teleport_time;

// MoveAtWorldYaw lives in bot_steer.cpp.
void MoveAtWorldYaw(float worldYaw, float viewYaw, float speed, float &fwd, float &side);

// Locate the verified malformed link in the bot's immediate remaining path.
// Keeping this independent of the active cursor edge is essential: followers
// one short NAV tile behind the owner must obey the same physical gate.
static bool FindHadithaDoorInPath(const int *path, int pathLen, int cursor,
                                  int &doorFrom, int &doorTo, Vector &mouth)
{
    if (!IsHadithaMapName(g_Bots.m_mapname)) return false;
    if (!path || pathLen < 2 || cursor < 0 || cursor >= pathLen) return false;

    int first = cursor > 0 ? cursor - 1 : cursor;
    doorFrom = -1; doorTo = -1;
    for (int k = first; k + 1 < pathLen && k <= cursor + 2; k++)
    {
        const NavAreaData *from = g_NavMesh.Area(path[k]);
        const NavAreaData *to = g_NavMesh.Area(path[k + 1]);
        if (from && to && from->id == 464 && to->id == 3321)
        {
            doorFrom = path[k];
            doorTo = path[k + 1];
            break;
        }
    }
    if (doorFrom < 0) return false;

    Vector axis; float halfSpan = 0.0f;
    if (!g_NavMesh.GetPortalMerged(doorFrom, doorTo, mouth, axis, halfSpan)
        && !GetSeparatedTransitionPortal(doorFrom, doorTo,
                                         mouth, axis, halfSpan, 0))
        return false;

    return true;
}

static bool PathContainsHadithaDoor(const int *path, int pathLen)
{
    if (!path || pathLen < 2) return false;
    for (int k = 0; k + 1 < pathLen; k++)
    {
        const NavAreaData *from = g_NavMesh.Area(path[k]);
        const NavAreaData *to = g_NavMesh.Area(path[k + 1]);
        if (from && to && from->id == 464 && to->id == 3321) return true;
    }
    return false;
}

// Once several hulls overlap, AdvanceWaypoint can invalidate their paths
// before the route-based gate sees them. Keep a physical source-side envelope
// for recovery so an existing jam cannot make the semaphore disappear. The
// Y>=25 boundary excludes bots that have already dropped onto the lower tile.
static bool IsHadithaDoorSourceEnvelope(const Vector &pos)
{
    return IsHadithaMapName(g_Bots.m_mapname)
        && fabsf(pos.x + 2592.5f) < 100.0f
        && pos.y >= 25.0f && pos.y < 225.0f
        && pos.z > -240.0f && pos.z < 0.0f;
}

// The malformed lower Haditha doorway is almost straight, so the generic
// sharp-corner slowdown does not engage even though its one-hull opening needs
// a careful approach. Detect bots within 160u whose next few path edges include
// the verified gap, allowing the final command builder to withhold +sprint for
// the entire approaching column instead of only the bot already at the lip.
bool IsHadithaDoorNoSprintZone(const int *path, int pathLen,
                                      int cursor, const Vector &pos)
{
    int doorFrom, doorTo; Vector mouth;
    if (!FindHadithaDoorInPath(path, pathLen, cursor,
                               doorFrom, doorTo, mouth))
        return false;

    float dx = mouth.x - pos.x, dy = mouth.y - pos.y;
    float dist = sqrtf(dx * dx + dy * dy);
    return dist < 160.0f;
}

// Deterministic traversal for the owner of Haditha's one-bot doorway gate.
// Unlike generic unstuck teleport this is not cvar/timer/path-edge heuristic
// driven: the roster semaphore has already proved that this bot alone owns the
// throat. We still require the exact verified path link, proximity to its
// mouth, a connected destination NAV area, and a completely clear player hull.
bool CInsBot::TransferHadithaDoor(float now)
{
    if (!insbot_doorway_teleport.GetBool() || now < m_nextTeleportOk
        || !m_ctrl || !m_info || !g_NavMesh.IsLoaded())
        return false;

    int fromIdx, toIdx; Vector mouth;
    if (!FindHadithaDoorInPath(m_path, m_pathLen, m_pathCursor,
                               fromIdx, toIdx, mouth))
    {
        // Stuck recovery may have discarded the owner's path while it was
        // physically blocking the gate. The semaphore still owns that bot, so
        // recover the same immutable, map-verified link by NAV ID.
        fromIdx = toIdx = -1;
        for (int i = 0; i < g_NavMesh.Count(); i++)
        {
            const NavAreaData *area = g_NavMesh.Area(i);
            if (!area) continue;
            if (area->id == 464) fromIdx = i;
            else if (area->id == 3321) toIdx = i;
            if (fromIdx >= 0 && toIdx >= 0) break;
        }
        Vector axis; float halfSpan = 0.0f;
        if (fromIdx < 0 || toIdx < 0
            || !GetSeparatedTransitionPortal(fromIdx, toIdx,
                                             mouth, axis, halfSpan, 0))
            return false;
    }

    Vector pos = m_info->GetAbsOrigin();
    float mdx = pos.x - mouth.x, mdy = pos.y - mouth.y;
    if (sqrtf(mdx * mdx + mdy * mdy) > 230.0f) return false;

    // Recover beyond the 25u landing tile. If a bot that already crossed is
    // slow to leave id3321, using that tile as the transfer target makes the
    // safety check reject every later owner and the whole gate stays wedged.
    // The immutable verified chain is 464 -> 3321 -> 463 -> 3277.
    int exitIdx = -1, clearIdx = -1;
    for (int i = 0; i < g_NavMesh.Count(); i++)
    {
        const NavAreaData *area = g_NavMesh.Area(i);
        if (!area) continue;
        if (area->id == 463) exitIdx = i;
        else if (area->id == 3277) clearIdx = i;
        if (exitIdx >= 0 && clearIdx >= 0) break;
    }
    const NavAreaData *destArea = g_NavMesh.Area(clearIdx);
    if (!destArea || destArea->id != 3277
        || !g_NavMesh.HasConnection(fromIdx, toIdx)
        || !g_NavMesh.HasConnection(toIdx, exitIdx)
        || !g_NavMesh.HasConnection(exitIdx, clearIdx))
        return false;

    // Do not make recovery depend on one fixed landing remaining empty.  A
    // dense column can fill that spot faster than ordinary steering drains
    // it, causing SafeStandSpot() to reject every later owner forever. Search
    // a small grid down the verified, BSP-probed exit passage instead. Each
    // candidate must lie inside an actual NAV area and pass the same standing
    // hull/world/player separation test as the old single destination.
    static const float exitY[] = { -145.0f, -112.5f, -75.0f, -37.5f };
    static const float exitX[] = { -2612.5f, -2572.5f, -2592.5f };
    Vector dest;
    bool foundDest = false;
    for (int yi = 0; yi < (int)(sizeof(exitY) / sizeof(exitY[0])) && !foundDest; yi++)
    {
        for (int xi = 0; xi < (int)(sizeof(exitX) / sizeof(exitX[0])); xi++)
        {
            Vector candidate(exitX[xi], exitY[yi], destArea->center.z);
            int candidateArea = g_NavMesh.FindNearestArea(candidate);
            const NavAreaData *ca = g_NavMesh.Area(candidateArea);
            if (!ca) continue;
            // FindNearestArea is allowed to return a nearby tile; require the
            // point itself to be represented so a transfer never lands off-nav.
            if (candidate.x < ca->lo.x - 1.0f || candidate.x > ca->hi.x + 1.0f
                || candidate.y < ca->lo.y - 1.0f || candidate.y > ca->hi.y + 1.0f)
                continue;
            candidate.z = g_NavMesh.GetZ(candidateArea,
                                          candidate.x, candidate.y) + 8.0f;
            float dx = candidate.x - pos.x, dy = candidate.y - pos.y;
            float dz = candidate.z - pos.z;
            if (dx * dx + dy * dy + dz * dz > 500.0f * 500.0f) continue;
            if (!SafeStandSpot(candidate, 34.0f)) continue;
            dest = candidate;
            foundDest = true;
            break;
        }
    }
    if (!foundDest) return false;

    m_ctrl->SetAbsOrigin(dest);
    m_nextTeleportOk = now + 1.0f;
    OnRelocated("Haditha doorway transfer");
    BotDbg("[insbots] %s Haditha doorway owner transferred (%.0f %.0f %.0f)\n",
           m_name, dest.x, dest.y, dest.z);
    return true;
}

// Full-hull-safe recovery for Haditha's second reported choke, centered near
// (-2484,-69). The source is the two-tile 3295/3315 corner and the destination
// is the broad id320 room east of the 25u opening. This is only called for the
// front queue bot after the ordinary choke progress timer proves a stall.
bool CInsBot::TransferHadithaEastDoor(float now)
{
    if (!insbot_doorway_teleport.GetBool() || now < m_nextTeleportOk
        || !m_ctrl || !m_info || !g_NavMesh.IsLoaded()
        || !IsHadithaMapName(g_Bots.m_mapname))
        return false;

    Vector pos = m_info->GetAbsOrigin();
    float mdx = pos.x + 2475.0f, mdy = pos.y + 75.0f;
    if (mdx * mdx + mdy * mdy > 150.0f * 150.0f) return false;

    int sourceIdx = CurrentArea(pos);
    if (sourceIdx < 0) sourceIdx = g_NavMesh.FindNearestArea(pos);
    const NavAreaData *source = g_NavMesh.Area(sourceIdx);
    if (!source || (source->id != 3295 && source->id != 3315)) return false;

    int destIdx = -1;
    for (int i = 0; i < g_NavMesh.Count(); i++)
    {
        const NavAreaData *area = g_NavMesh.Area(i);
        if (area && area->id == 320) { destIdx = i; break; }
    }
    const NavAreaData *destArea = g_NavMesh.Area(destIdx);
    if (!destArea || !g_NavMesh.HasConnection(sourceIdx, destIdx)) return false;

    // BSP-expanded standing-hull probes show the full id320 interior clear.
    // Use the far/east column first, then spread down its long Y axis when a
    // previously transferred bot is still occupying a slot.
    static const float exitX[] = { -2417.5f, -2437.5f, -2457.5f };
    static const float exitY[] =
        { -187.5f, -137.5f, -87.5f, -37.5f };
    Vector dest;
    bool foundDest = false;
    for (int xi = 0; xi < (int)(sizeof(exitX) / sizeof(exitX[0])) && !foundDest; xi++)
    {
        for (int yi = 0; yi < (int)(sizeof(exitY) / sizeof(exitY[0])); yi++)
        {
            Vector candidate(exitX[xi], exitY[yi], destArea->center.z);
            if (candidate.x < destArea->lo.x + 16.0f
                || candidate.x > destArea->hi.x - 16.0f
                || candidate.y < destArea->lo.y + 16.0f
                || candidate.y > destArea->hi.y - 16.0f)
                continue;
            candidate.z = g_NavMesh.GetZ(destIdx,
                                          candidate.x, candidate.y) + 8.0f;
            if (!SafeStandSpot(candidate, 34.0f)) continue;
            dest = candidate;
            foundDest = true;
            break;
        }
    }
    if (!foundDest) return false;

    m_ctrl->SetAbsOrigin(dest);
    m_nextTeleportOk = now + 1.0f;
    OnRelocated("Haditha east doorway transfer");
    BotDbg("[insbots] %s Haditha east doorway transferred (%.0f %.0f %.0f)\n",
           m_name, dest.x, dest.y, dest.z);
    return true;
}

void CBotRoster::ResetHadithaDoorGate()
{
    m_hadithaDoorOwner = 0;
    m_hadithaDoorOwnerSince = 0.0f;
    m_hadithaDoorProgressPos = Vector(0, 0, 0);
    m_hadithaDoorProgressTime = 0.0f;
    m_hadithaDoorClearUntil = 0.0f;
    m_hadithaDoorCommitted = false;
}

void CBotRoster::InvalidateAdvanceTargets()
{
    for (int i = 0; i < 64; i++)
    {
        if (!m_bots[i].m_inUse) continue;
        m_bots[i].m_haveAdvanceTarget = false;
        m_bots[i].m_advanceTargetRefresh = 0.0f;
    }
}

// A spatial semaphore for Haditha's malformed 464->3321 doorway. Distributed
// predecessor queues lose identity as soon as the leader advances onto the
// next tiny NAV tile; this roster-level owner persists until the bot is
// physically below the lip. Every other eligible bot receives a hard hold at
// the final command stage, after ordinary crowd steering has finished.
bool CBotRoster::ControlHadithaDoor(CInsBot *bot, const Vector &pos, float now,
                                    float viewYaw, float &fwd, float &side)
{
    if (!bot || !IsHadithaMapName(m_mapname) || !insbot_doorway_teleport.GetBool()) return false;

    // BSP-expanded 32u hull samples put the physical opening midpoint five
    // units left of the generated NAV overlap midpoint.
    const float mouthX = -2592.5f;
    const float mouthY = 50.0f;
    const float stageY = 78.0f;

    // Release only after authoritative physical evidence that the owner is on
    // the lower landing. The short clearance interval lets its 32u hull move
    // away before the next bot is admitted.
    if (m_hadithaDoorOwner > 0)
    {
        int oi = m_hadithaDoorOwner - 1;
        CInsBot *owner = (oi >= 0 && oi < 64) ? &m_bots[oi] : 0;
        bool valid = owner && owner->m_inUse && owner->m_info
                  && !owner->m_info->IsDead();
        bool release = !valid;
        if (valid)
        {
            Vector op = owner->m_info->GetAbsOrigin();
            // Do not hand the gate to a follower while the old owner's rear
            // hull can still overlap the throat.  The small landing ends near
            // Y=-12; Y=-64 is safely into the wider destination passage.
            bool crossed = op.y <= -64.0f && op.z < -150.0f
                        && fabsf(op.x - mouthX) < 64.0f;
            float odist = xyd(op, Vector(mouthX, mouthY, op.z));
            bool leftGate = odist > 300.0f;
            bool stallExpired = (now - m_hadithaDoorOwnerSince > 6.0f);
            bool outOfZone = (odist >= 240.0f && !m_hadithaDoorCommitted && now - m_hadithaDoorOwnerSince > 2.0f);
            if (crossed || leftGate || stallExpired || outOfZone)
            {
                release = true;
            }
        }
        if (release)
        {
            m_hadithaDoorOwner = 0;
            m_hadithaDoorOwnerSince = 0.0f;
            m_hadithaDoorProgressTime = 0.0f;
            m_hadithaDoorClearUntil = now + 0.45f;
            m_hadithaDoorCommitted = false;
        }
    }

    int mySlot = (int)(bot - m_bots);
    bool isOwner = m_hadithaDoorOwner == mySlot + 1;

    int myFrom, myTo; Vector myMouth(mouthX, mouthY, pos.z);
    bool onDoorRoute = FindHadithaDoorInPath(bot->m_path, bot->m_pathLen,
                                             bot->m_pathCursor,
                                             myFrom, myTo, myMouth);
    if (onDoorRoute) { myMouth.x = mouthX; myMouth.y = mouthY; }
    bool inSourceEnvelope = IsHadithaDoorSourceEnvelope(pos);
    bool frontPocket = inSourceEnvelope
                    && fabsf(pos.x - mouthX) < 72.0f && pos.y < 180.0f;
    bool spatialRecovery = inSourceEnvelope
        && (m_hadithaDoorOwner > 0 || now < m_hadithaDoorClearUntil
            || frontPocket || bot->m_pathLen <= 0
            || PathContainsHadithaDoor(bot->m_path, bot->m_pathLen));
    bool doorEligible = onDoorRoute || spatialRecovery;
    float myDist = (doorEligible || isOwner) ? xyd(pos, myMouth) : 99999.0f;
    bool inGateZone = (doorEligible && myDist < 230.0f)
                   || (isOwner && myDist < 240.0f);

    // Pick the physically closest eligible bot, not whichever Think() happens
    // to run first. This gives the front hull ownership and prevents a bot
    // behind it from trying to push through to claim the throat.
    if (m_hadithaDoorOwner == 0 && now >= m_hadithaDoorClearUntil)
    {
        int best = -1;
        float bestDist = 99999.0f;
        for (int i = 0; i < 64; i++)
        {
            CInsBot *candidate = &m_bots[i];
            if (!candidate->m_inUse || !candidate->m_info
                || candidate->m_info->IsDead())
                continue;
            Vector cp = candidate->m_info->GetAbsOrigin();
            int cf, ct; Vector cm(mouthX, mouthY, cp.z);
            bool routeCandidate = FindHadithaDoorInPath(
                candidate->m_path, candidate->m_pathLen,
                candidate->m_pathCursor, cf, ct, cm);
            bool spatialCandidate = IsHadithaDoorSourceEnvelope(cp)
                && ((fabsf(cp.x - mouthX) < 72.0f && cp.y < 180.0f)
                    || candidate->m_pathLen <= 0
                    || PathContainsHadithaDoor(candidate->m_path,
                                               candidate->m_pathLen));
            if (!routeCandidate && !spatialCandidate) continue;
            cm.x = mouthX; cm.y = mouthY;
            float d = xyd(cp, cm);
            if (d >= 230.0f) continue;
            if (d < bestDist - 0.5f || (fabsf(d - bestDist) <= 0.5f && i < best))
            {
                best = i;
                bestDist = d;
            }
        }
        if (best >= 0)
        {
            m_hadithaDoorOwner = best + 1;
            m_hadithaDoorOwnerSince = now;
            m_hadithaDoorProgressPos = m_bots[best].m_info->GetAbsOrigin();
            m_hadithaDoorProgressTime = now;
            m_hadithaDoorCommitted = false;
            BotDbg("[insbots] Haditha doorway owner: %s\n", m_bots[best].m_name);
        }
    }

    if (!inGateZone) return false;

    if (m_hadithaDoorOwner != mySlot + 1)
    {
        // Hard hold. Mark it as an intentional queue wait so generic stuck
        // recovery cannot repath or jiggle the follower back into the owner.
        bot->m_doorYieldSlot = m_hadithaDoorOwner > 0
                             ? m_hadithaDoorOwner : mySlot + 1;
        bot->m_doorYieldUntil = now + 0.30f;
        bot->m_followSpeed = 0.0f;
        bot->m_queueLeaderSlot = bot->m_doorYieldSlot;
        // Do not try to reverse a packed column. The rear may have no physical
        // space, making a synchronized retreat ineffective. Hold followers in
        // place and let the owner's validated forward transfer drain the front.
        fwd = 0.0f;
        side = 0.0f;
        return true;
    }

    bot->m_doorYieldSlot = 0;
    bot->m_doorYieldUntil = 0.0f;
    // The semaphore, not generic stuck recovery, owns this transition.
    if (!bot->m_bodyNoCollideActive && !bot->m_bodyNoCollideEscalating)
    {
        bot->m_stuckSince = 0.0f;
        bot->m_stuckStage = 0;
        bot->m_haveRescueLink = false;
    }

    // The owner gets the same configured continuous-no-progress budget as any
    // other doorway leader. ControlHadithaDoor suppresses generic stuck state
    // while it owns the bot, so maintain the equivalent progress clock here.
    if (m_hadithaDoorProgressTime <= 0.0f
        || xyd(pos, m_hadithaDoorProgressPos) >= 16.0f)
    {
        m_hadithaDoorProgressPos = pos;
        m_hadithaDoorProgressTime = now;
    }

    // Two-stage approach: acquire the clear staging point first, then commit
    // straight south exactly once. Directly aiming at the mouth from the
    // preceding east/west tiles began the turn too early and carved an arc
    // into the right jamb.
    // An owner already past the staging line must not reverse into followers;
    // center laterally at its current Y instead.
    Vector stage(mouthX, pos.y < stageY - 8.0f ? pos.y : stageY, pos.z);
    float stageDist = xyd(pos, stage);
    if (!m_hadithaDoorCommitted)
    {
        if (stageDist <= 6.0f)
        {
            m_hadithaDoorCommitted = true;
            // Keep OwnerSince as the acquisition time.  Resetting it here
            // gave a slow/pinned owner another full timeout and let pressure
            // rebuild behind it before recovery could drain the throat.
        }
        else
        {
            float yaw = atan2f(stage.y - pos.y, stage.x - pos.x)
                      * (180.0f / 3.14159265f);
            MoveAtWorldYaw(yaw, viewYaw, 180.0f, fwd, side);
        }
    }
    if (m_hadithaDoorCommitted)
    {
        // Once centered, commit straight through at non-sprint speed. This is
        // the sole bot allowed to touch the throat; if world/crowd collision
        // still prevents the drop, the timed safe transfer below drains it.
        MoveAtWorldYaw(-90.0f, viewYaw, 160.0f, fwd, side);
    }
    float teleportDelay = insbot_doorway_teleport_time.GetFloat();
    if (teleportDelay < 0.2f) teleportDelay = 0.2f;
    if (now - m_hadithaDoorProgressTime >= teleportDelay && myDist <= 230.0f
        && bot->TransferHadithaDoor(now))
    {
        fwd = 0.0f;
        side = 0.0f;
        m_hadithaDoorOwner = 0;
        m_hadithaDoorOwnerSince = 0.0f;
        m_hadithaDoorProgressTime = 0.0f;
        m_hadithaDoorClearUntil = now + 0.45f;
        m_hadithaDoorCommitted = false;
    }
    return true;
}
