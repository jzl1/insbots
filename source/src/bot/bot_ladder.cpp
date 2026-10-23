// ============================================================================
// bot_ladder.cpp - typed NAV ladder traversal.
//
// Source ladder physics derives vertical wish-speed from view pitch while the
// player pushes into the ladder, so climbing temporarily owns both yaw and
// pitch. The area cursor still advances only after the bot physically reaches
// the destination landing. Ownership is a per-ladder semaphore on the roster so
// a packed column drains one hull at a time instead of wedging on the shaft.
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

// ---- externs this TU needs (same pattern as bot.cpp / objective.cpp) -------
extern IVEngineServer       *engine;
extern IEngineTrace         *enginetrace;
extern IPlayerInfoManager   *playerinfomanager;
extern CGlobalVars          *gpGlobals;
extern CfgVar                insbot_ladder_lip_tol;
extern CfgVar                insbot_ladder_queue;
extern CfgVar                insbot_move_speed;

// Vertical tolerance used to decide the bot has reached a ladder's recorded
// lip.  The raw lip is a brush-AABB value that the player hull frequently
// cannot occupy: the landing floor can sit above it, and even when the two
// align there is a residual settle gap. Measured shortfalls at the top of a
// completed climb were
// 2.4u-3.1u, so a zero-tolerance test never fires on ascent and the bot
// thrashes in RECOVERY forever.
//
// The ceiling below is empirical, not a margin calculation.  A larger
// tolerance latches the dismount LOWER on the shaft (up: pos.z >= end - tol),
// so the bot starts walking toward the landing while it is still hanging
// below the lip.  The dismount controls stop the climb, the bot sags, and
// once it drops past endHeight - 18 the lost-height guard in TraverseLadder
// flips the phase back to ATTACHED: latch / sag / re-attach, forever.
//
// That guard is measured from endHeight, NOT from the latch point, so the
// tolerance spends the guard's budget directly. Measured on a ladder whose
// landing floor sits 2u above its recorded lip, lost-height events per segment:
//   tol 0/2/4/6 -> 0 events      tol 8 -> 8 events      tol 16 -> 33 events
// and an independent 240s run at tol 12 produced 43 latches with 38 of them
// reverting (observed sagging from a z=63.5 latch down to z=56.0, i.e. just
// past the 57.0 guard).  The safe band therefore ends between 6 and 8.
//
// The ceiling is 6u: every measured shortfall (2.4u-3.1u) is still covered
// and is closed by a single climb tick (~0.05-0.1s), while the guard keeps
// 12u of headroom.  Raising it requires re-running the sweep - the earlier
// uncapped/16u ceiling let an admin re-create the loop.
//
// Not every ladder is fixed by this: some fail an earlier XY gate in
// PositionOnLadderLanding() because the ladder top sits outside the landing
// NAV area's XY rectangle. Their residual lost-height retries are a navmesh
// problem, not a tolerance one.
float LadderLipTol()
{
    float tol = insbot_ladder_lip_tol.GetFloat();
    if (tol < 0.0f) tol = 0.0f;
    if (tol > 6.0f) tol = 6.0f;
    return tol;
}

// Drive one complete typed NAV ladder edge. Source ladder physics derives
// vertical wish-speed from view pitch while the player pushes into the ladder,
// so climbing temporarily owns both yaw and pitch. The area cursor still
// advances only after the bot physically reaches the destination landing.
Vector LadderAreaInterior(int areaIdx, const Vector &lip)
{
    const NavAreaData *area = g_NavMesh.Area(areaIdx);
    if (!area) return lip;
    float minX = area->lo.x + 18.0f, maxX = area->hi.x - 18.0f;
    float minY = area->lo.y + 18.0f, maxY = area->hi.y - 18.0f;
    if (minX > maxX) minX = maxX = area->center.x;
    if (minY > maxY) minY = maxY = area->center.y;

    float dx = area->center.x - lip.x, dy = area->center.y - lip.y;
    float len = sqrtf(dx * dx + dy * dy);
    Vector out = lip;
    if (len > 1.0f)
    {
        float step = len < 64.0f ? len : 64.0f;
        out.x += dx * (step / len);
        out.y += dy * (step / len);
    }
    if (out.x < minX) out.x = minX; else if (out.x > maxX) out.x = maxX;
    if (out.y < minY) out.y = minY; else if (out.y > maxY) out.y = maxY;
    out.z = g_NavMesh.GetZ(areaIdx, out.x, out.y);
    return out;
}

Vector CInsBot::LadderQueueStandPoint(const Vector &lip, int rank) const
{
    float remaining = 56.0f * (float)(rank + 1);
    Vector point = lip;
    int k = m_pathCursor - 1;
    if (k < 0 || k >= m_pathLen) return lip;

    // Walk backward along the bot's actual route. This lets a fifteen-bot
    // column occupy earlier NAV areas instead of collapsing every rank onto
    // the center of a shallow ladder-entry tile.
    for (; k >= 0; k--)
    {
        int areaIdx = m_path[k];
        const NavAreaData *area = g_NavMesh.Area(areaIdx);
        if (!area) break;
        Vector next = area->center;
        if (k > 0)
        {
            Vector portal;
            if (g_NavMesh.GetPortal(m_path[k], m_path[k - 1], portal))
                next = portal;
        }
        float minX = area->lo.x + 18.0f, maxX = area->hi.x - 18.0f;
        float minY = area->lo.y + 18.0f, maxY = area->hi.y - 18.0f;
        if (minX > maxX) minX = maxX = area->center.x;
        if (minY > maxY) minY = maxY = area->center.y;
        if (next.x < minX) next.x = minX; else if (next.x > maxX) next.x = maxX;
        if (next.y < minY) next.y = minY; else if (next.y > maxY) next.y = maxY;
        next.z = g_NavMesh.GetZ(areaIdx, next.x, next.y);

        float dx = next.x - point.x, dy = next.y - point.y;
        float len = sqrtf(dx * dx + dy * dy);
        if (len >= remaining && len > 0.01f)
        {
            Vector out = point + (next - point) * (remaining / len);
            out.z = g_NavMesh.GetZ(areaIdx, out.x, out.y);
            return out;
        }
        if (len > 0.01f) remaining -= len;
        point = next;
    }
    return point;
}

void CBotRoster::ResetLadderQueues()
{
    for (int i = 0; i < 10000; i++)
    {
        m_ladderQueues[i].ownerSlot = 0;
        m_ladderQueues[i].lastDirection = 0;
    }
}

bool CBotRoster::IsLadderOwner(const CInsBot *bot, int ladderIdx) const
{
    if (!bot || ladderIdx < 0 || ladderIdx >= 10000) return false;
    int slot = (int)(bot - m_bots);
    return slot >= 0 && slot < 64
        && m_ladderQueues[ladderIdx].ownerSlot == slot + 1;
}

void CBotRoster::ReleaseLadder(CInsBot *bot, int ladderIdx, bool completed)
{
    if (!bot || ladderIdx < 0 || ladderIdx >= 10000) return;
    int slot = (int)(bot - m_bots);
    LadderQueueState &state = m_ladderQueues[ladderIdx];
    if (slot < 0 || slot >= 64 || state.ownerSlot != slot + 1) return;
    state.lastDirection = bot->m_ladderClimbUp ? 1 : -1;
    state.ownerSlot = 0;
    BotDbg("[insbots] ladder %d owner %s released (%s)\n",
           ladderIdx, bot->m_name, completed ? "landing clear" : "detached");
}

bool CBotRoster::ClaimLadder(CInsBot *bot, int ladderIdx, bool up,
                             const Vector &pos)
{
    if (!bot || ladderIdx < 0 || ladderIdx >= g_NavMesh.LadderCount()
        || ladderIdx >= 10000 || !insbot_ladder_queue.GetBool())
        return true;

    LadderQueueState &state = m_ladderQueues[ladderIdx];
    if (state.ownerSlot > 0)
    {
        int oi = state.ownerSlot - 1;
        CInsBot *owner = oi >= 0 && oi < 64 ? &m_bots[oi] : 0;
        if (!owner || !owner->m_inUse || !owner->m_info || owner->m_info->IsDead()
            || owner->m_ladderClimbIdx != ladderIdx
            || owner->m_ladderPhase == LADDER_PHASE_NONE)
            state.ownerSlot = 0;
    }
    if (state.ownerSlot > 0)
        return state.ownerSlot == (int)(bot - m_bots) + 1;

    const NavLadderData *ladder = g_NavMesh.Ladder(ladderIdx);
    if (!ladder) return true;
    int attachedBest = -1, waitingBest = -1;
    float attachedMetric = 3.4e38f, waitingMetric = 3.4e38f;
    bool haveUp = false, haveDown = false;

    // First pass discovers both waiting directions so consecutive owners can
    // alternate when traffic is queued at both ends.
    for (int i = 0; i < 64; i++)
    {
        CInsBot *candidate = &m_bots[i];
        if (!candidate->m_inUse || !candidate->m_info || candidate->m_info->IsDead())
            continue;
        int cli = -1; bool cup = false;
        bool same = candidate->m_ladderClimbIdx == ladderIdx
                 && candidate->m_ladderPhase != LADDER_PHASE_NONE;
        if (!same && candidate->m_ladderPhase != LADDER_PHASE_NONE
            && candidate->m_pathCursor > 0
            && candidate->m_pathCursor < candidate->m_pathLen)
            same = g_NavMesh.GetLadderConnection(
                candidate->m_path[candidate->m_pathCursor - 1],
                candidate->m_path[candidate->m_pathCursor], cli, cup)
                && cli == ladderIdx;
        else if (same) cup = candidate->m_ladderClimbUp;
        if (!same) continue;
        if (cup) haveUp = true; else haveDown = true;
    }
    int desiredDirection = 0;
    if (haveUp && haveDown && state.lastDirection)
        desiredDirection = -state.lastDirection;

    for (int i = 0; i < 64; i++)
    {
        CInsBot *candidate = &m_bots[i];
        if (!candidate->m_inUse || !candidate->m_info || candidate->m_info->IsDead())
            continue;
        int cli = -1; bool cup = false;
        bool same = candidate->m_ladderClimbIdx == ladderIdx
                 && candidate->m_ladderPhase != LADDER_PHASE_NONE;
        if (!same && candidate->m_pathCursor > 0
            && candidate->m_pathCursor < candidate->m_pathLen)
            same = g_NavMesh.GetLadderConnection(
                candidate->m_path[candidate->m_pathCursor - 1],
                candidate->m_path[candidate->m_pathCursor], cli, cup)
                && cli == ladderIdx;
        else if (same) cup = candidate->m_ladderClimbUp;
        if (!same) continue;

        Vector cp = candidate->m_info->GetAbsOrigin();
        Vector end = cup ? ladder->top : ladder->bottom;
        float ex = cp.x - end.x, ey = cp.y - end.y, ez = cp.z - end.z;
        float metric = sqrtf(ex * ex + ey * ey + ez * ez);
        bool attached = candidate->m_ladderContactConfirmed
                     || candidate->m_ladderPhase == LADDER_PHASE_ATTACHED
                     || candidate->m_ladderPhase == LADDER_PHASE_DISMOUNT
                     || candidate->m_ladderPhase == LADDER_PHASE_RECOVERY;
        if (attached)
        {
            if (metric < attachedMetric - 0.01f
                || (fabsf(metric - attachedMetric) <= 0.01f && i < attachedBest))
            {
                attachedMetric = metric;
                attachedBest = i;
            }
            continue;
        }
        int direction = cup ? 1 : -1;
        if (desiredDirection && direction != desiredDirection) continue;
        Vector start = cup ? ladder->bottom : ladder->top;
        float sx = cp.x - start.x, sy = cp.y - start.y, sz = cp.z - start.z;
        metric = sqrtf(sx * sx + sy * sy + sz * sz);
        if (metric < waitingMetric - 0.01f
            || (fabsf(metric - waitingMetric) <= 0.01f && i < waitingBest))
        {
            waitingMetric = metric;
            waitingBest = i;
        }
    }

    int best = attachedBest >= 0 ? attachedBest : waitingBest;
    if (best < 0) best = (int)(bot - m_bots);
    state.ownerSlot = best + 1;
    CInsBot *owner = &m_bots[best];
    BotDbg("[insbots] ladder %d owner: %s (%s)\n", ladderIdx,
           owner->m_name, owner->m_ladderClimbUp ? "up" : "down");

    // Multiple legacy occupants are drained deterministically: the owner
    // finishes, every already-attached non-owner heads back to its source.
    for (int i = 0; i < 64; i++)
    {
        CInsBot *candidate = &m_bots[i];
        if (i == best || !candidate->m_inUse) continue;
        if (candidate->m_ladderClimbIdx == ladderIdx
            && candidate->m_ladderContactConfirmed)
            candidate->m_ladderReturnToSource = true;
    }
    return best == (int)(bot - m_bots);
}

void CInsBot::ResetLadderTraversal(bool releaseOwner)
{
    int ladderIdx = m_ladderClimbIdx;
    if (releaseOwner && ladderIdx >= 0)
        g_Bots.ReleaseLadder(this, ladderIdx, m_ladderCompletionReady);
    m_ladderClimbing = false;
    m_ladderClimbIdx = -1;
    m_ladderClimbUp = false;
    m_ladderFromArea = m_ladderToArea = -1;
    m_ladderPhase = LADDER_PHASE_NONE;
    m_ladderContactConfirmed = false;
    m_ladderCompletionReady = false;
    m_ladderReturnToSource = false;
}

bool CInsBot::ConfirmLadderContact(const Vector &pos, float nx, float ny) const
{
    if (!enginetrace || !m_info || !m_edict) return false;
    Vector toward(-nx, -ny, 0.0f);
    Ray_t ray; trace_t tr;
    ray.Init(pos, pos + toward * 4.0f,
             m_info->GetPlayerMins(), m_info->GetPlayerMaxs());
    CTraceFilterSkipSelf filter(EdictHandleEntity(m_edict));
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID | CONTENTS_LADDER, &filter, &tr);
    return tr.fraction < 1.0f && (tr.contents & CONTENTS_LADDER) != 0;
}

bool CInsBot::EnemyNearLadder(const NavLadderData *ladder) const
{
    if (!ladder || !gpGlobals || !engine || !playerinfomanager || !m_info)
        return true;
    int maxClients = gpGlobals->maxClients > 64 ? 64 : gpGlobals->maxClients;
    for (int i = 1; i <= maxClients; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (!e || e == m_edict || e->IsFree()) continue;
        IPlayerInfo *pi = playerinfomanager->GetPlayerInfo(e);
        if (!pi || !pi->IsConnected() || pi->IsDead() || pi->IsObserver()
            || pi->GetTeamIndex() == m_info->GetTeamIndex()) continue;
        Vector p = pi->GetAbsOrigin();
        float t = ladder->top.z > ladder->bottom.z + 1.0f
                ? (p.z - ladder->bottom.z) / (ladder->top.z - ladder->bottom.z)
                : 0.0f;
        if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
        Vector axis = ladder->bottom + (ladder->top - ladder->bottom) * t;
        float dx = p.x - axis.x, dy = p.y - axis.y;
        if (dx * dx + dy * dy <= 192.0f * 192.0f
            && p.z >= ladder->bottom.z - 96.0f
            && p.z <= ladder->top.z + 96.0f)
            return true;
    }
    return false;
}

int CInsBot::FindLadderBlocker(const Vector &pos, const Vector &target,
                               int ladderIdx) const
{
    if (!enginetrace || !engine || !playerinfomanager || !gpGlobals
        || !m_edict || !m_info || EnemyNearLadder(g_NavMesh.Ladder(ladderIdx)))
        return 0;
    Vector delta = target - pos;
    float len = delta.Length();
    if (len < 1.0f) return 0;
    if (len > 64.0f) delta *= 64.0f / len;
    Ray_t ray; trace_t tr;
    ray.Init(pos, pos + delta, m_info->GetPlayerMins(), m_info->GetPlayerMaxs());
    CTraceFilterSkipSelf filter(EdictHandleEntity(m_edict));
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &tr);
    if (tr.fraction >= 1.0f || !tr.m_pEnt) return 0;
    int maxClients = gpGlobals->maxClients > 64 ? 64 : gpGlobals->maxClients;
    for (int i = 1; i <= maxClients; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (!e || e == m_edict || e->IsFree()) continue;
        IServerUnknown *unknown = e->GetUnknown();
        if (!unknown || tr.m_pEnt != unknown->GetBaseEntity()) continue;
        IPlayerInfo *pi = playerinfomanager->GetPlayerInfo(e);
        if (!pi || !pi->IsConnected() || pi->IsDead() || pi->IsObserver()
            || pi->GetTeamIndex() != m_info->GetTeamIndex()) return 0;
        if (pi->IsFakeClient() && !g_Bots.FindByEdict(e)) return 0;
        return i;
    }
    return 0;
}

bool CInsBot::TraverseLadder(int fromArea, int toArea, float &fwd, float &side,
                             int &buttons, QAngle &view, Vector &navTarget)
{
    int ladderIdx = -1; bool up = false;
    bool typedEdge = g_NavMesh.GetLadderConnection(fromArea, toArea, ladderIdx, up);
    if (!typedEdge)
    {
        bool edgeMissing = (fromArea < 0 || toArea < 0 || !g_NavMesh.HasConnection(fromArea, toArea));
        if (m_ladderPhase == LADDER_PHASE_NONE || m_ladderClimbIdx < 0 || !edgeMissing)
        {
            if (m_ladderPhase != LADDER_PHASE_NONE)
                ResetLadderTraversal(true);
            return false;
        }
        ladderIdx = m_ladderClimbIdx;
        up = m_ladderClimbUp;
        fromArea = m_ladderFromArea;
        toArea = m_ladderToArea;
    }
    else
    {
        if (m_ladderPhase != LADDER_PHASE_NONE
            && (m_ladderClimbIdx != ladderIdx || m_ladderClimbUp != up
                || m_ladderFromArea != fromArea || m_ladderToArea != toArea))
        {
            ResetLadderTraversal(true);
        }
        m_ladderClimbIdx = ladderIdx;
        m_ladderClimbUp = up;
        m_ladderFromArea = fromArea;
        m_ladderToArea = toArea;
        if (m_ladderPhase == LADDER_PHASE_NONE)
        {
            m_ladderPhase = LADDER_PHASE_APPROACH;
            m_ladderContactConfirmed = false;
            m_ladderCompletionReady = false;
            m_ladderReturnToSource = false;
            BotDbg("[insbots] %s ladder %d approach started (%s)\n",
                   m_name, ladderIdx, up ? "up" : "down");
        }
    }

    const NavLadderData *ladder = g_NavMesh.Ladder(ladderIdx);
    if (!ladder) return false;

    float nx = 0.0f, ny = 0.0f;
    if      (ladder->dir == NAV_NORTH) ny = -1.0f;
    else if (ladder->dir == NAV_SOUTH) ny =  1.0f;
    else if (ladder->dir == NAV_EAST)  nx =  1.0f;
    else if (ladder->dir == NAV_WEST)  nx = -1.0f;
    else return false;

    Vector pos = m_info->GetAbsOrigin();
    Vector startLip = up ? ladder->bottom : ladder->top;
    Vector endLip = up ? ladder->top : ladder->bottom;
    Vector sourcePoint = LadderAreaInterior(fromArea, startLip);
    Vector landingPoint = LadderAreaInterior(toArea, endLip);
    float sourceHeight = sourcePoint.z;
    // NAV ladder bottoms are commonly buried inside the lower floor. Downward
    // traversal ends when the feet reach that floor, not when the origin sinks
    // to the serialized brush-AABB bottom. The raw top remains authoritative
    // for climbing up so dismount cannot begin one or two rungs early.
    float endHeight = up ? ladder->top.z : landingPoint.z;

    float sdx = sourcePoint.x - startLip.x, sdy = sourcePoint.y - startLip.y;
    float slen = sqrtf(sdx * sdx + sdy * sdy);
    if (slen <= 1.0f) { sdx = nx; sdy = ny; slen = 1.0f; }
    Vector approach = startLip + Vector(sdx * (2.0f / slen),
                                        sdy * (2.0f / slen), 4.0f);

    // Distance to the actual ladder axis at this height. Most Source ladders
    // are vertical, but using the interpolated axis also handles edited records
    // whose top and bottom XY coordinates differ slightly.
    float spanZ = ladder->top.z - ladder->bottom.z;
    float axisT = spanZ > 1.0f ? (pos.z - ladder->bottom.z) / spanZ : 0.0f;
    if (axisT < 0.0f) axisT = 0.0f; else if (axisT > 1.0f) axisT = 1.0f;
    Vector axis = ladder->bottom + (ladder->top - ladder->bottom) * axisT;
    float ldx = pos.x - axis.x, ldy = pos.y - axis.y;
    float ladderDistSq = ldx * ldx + ldy * ldy;

    buttons &= ~(IN_ATTACK | INS_IN_ATTACK2 | INS_IN_ATTACK3 | INS_IN_RELOAD | IN_JUMP | IN_DUCK | INS_IN_SPRINT
               | INS_IN_PRONE | INS_IN_FORWARD | INS_IN_BACK
               | INS_IN_MOVELEFT | INS_IN_MOVERIGHT);
    m_firing = false;
    m_ironsights = false;
    m_traversingLadder = true;

    float hullMaxZ = m_info->GetPlayerMaxs().z;
    if (hullMaxZ > 1.0f && hullMaxZ < 30.0f)
    {
        float qx = approach.x - pos.x, qy = approach.y - pos.y;
        view.y = atan2f(qy, qx) * (180.0f / 3.14159265f); view.x = 0.0f;
        fwd = side = 0.0f;
        if (gpGlobals && gpGlobals->tickcount % 20 == 0)
            buttons |= IN_JUMP | IN_DUCK;
        navTarget = approach;
        return true;
    }

    bool owner = g_Bots.ClaimLadder(this, ladderIdx, up, pos);
    if (!owner)
    {
        // A legacy second occupant drains back to its source instead of
        // freezing on the shaft. New contenders never attach before ownership.
        if (m_ladderContactConfirmed && m_ladderReturnToSource)
        {
            bool sourceReached = up ? pos.z <= sourceHeight + 4.0f
                                    : pos.z >= sourceHeight - 4.0f;
            if (sourceReached && PositionOnLadderLanding(fromArea, pos))
            {
                ResetLadderTraversal(false);
                return false;
            }
            view.y = atan2f(-ny, -nx) * (180.0f / 3.14159265f);
            view.x = up ? 65.0f : -65.0f;
            fwd = insbot_move_speed.GetFloat(); side = 0.0f;
            buttons |= INS_IN_FORWARD;
            navTarget = startLip;
            return true;
        }

        int rank = 0;
        float myDx = pos.x - startLip.x, myDy = pos.y - startLip.y;
        float myDist = sqrtf(myDx * myDx + myDy * myDy);
        for (int i = 0; i < 64; i++)
        {
            CInsBot *other = &g_Bots.m_bots[i];
            if (other == this || !other->m_inUse || !other->m_info
                || other->m_info->IsDead() || other->m_ladderClimbIdx != ladderIdx
                || other->m_ladderClimbUp != up
                || other->m_ladderPhase == LADDER_PHASE_NONE
                || g_Bots.IsLadderOwner(other, ladderIdx)) continue;
            Vector op = other->m_info->GetAbsOrigin();
            float ox = op.x - startLip.x, oy = op.y - startLip.y;
            float od = sqrtf(ox * ox + oy * oy);
            if (od < myDist - 0.01f
                || (fabsf(od - myDist) <= 0.01f
                    && i < (int)(this - g_Bots.m_bots))) rank++;
        }
        m_ladderPhase = LADDER_PHASE_QUEUED;
        m_ladderQueuePoint = LadderQueueStandPoint(startLip, rank);
        float qx = m_ladderQueuePoint.x - pos.x, qy = m_ladderQueuePoint.y - pos.y;
        view.y = atan2f(qy, qx) * (180.0f / 3.14159265f); view.x = 0.0f;
        float qd = sqrtf(qx * qx + qy * qy);
        fwd = qd > 12.0f ? insbot_move_speed.GetFloat() * 0.45f : 0.0f;
        side = 0.0f;
        if (fwd > 0.0f) buttons |= INS_IN_FORWARD;
        navTarget = m_ladderQueuePoint;
        return true;
    }

    if (m_ladderPhase == LADDER_PHASE_QUEUED)
    {
        m_ladderPhase = LADDER_PHASE_APPROACH;
        BotDbg("[insbots] %s ladder %d queue released to approach\n",
               m_name, ladderIdx);
    }

    bool contactNow = ConfirmLadderContact(pos, nx, ny);
    bool shaftContinuity = m_ladderContactConfirmed
        && pos.z > ladder->bottom.z - 16.0f
        && pos.z < ladder->top.z + 40.0f
        && ladderDistSq < 96.0f * 96.0f;
    if (!m_ladderContactConfirmed && contactNow)
    {
        m_ladderContactConfirmed = true;
        m_ladderClimbing = true;
        m_ladderPhase = LADDER_PHASE_ATTACHED;
        BotDbg("[insbots] %s ladder %d physical contact confirmed\n",
               m_name, ladderIdx);
    }
    bool verticallyOnLadder = contactNow || shaftContinuity;
    if (!verticallyOnLadder && m_ladderPhase != LADDER_PHASE_DISMOUNT)
    {
        float adx = approach.x - pos.x, ady = approach.y - pos.y;
        // Descending attachment needs to look down into the face once at the
        // lip. From farther away, keep a level view so the 2u attach probe is
        // not aimed into the floor before the hull reaches the ladder.
        if (!up && adx*adx+ady*ady < 40.0f*40.0f
            && fabsf(pos.z-ladder->top.z) < 80.0f)
        {
            view.y = atan2f(-ny, -nx) * (180.0f / 3.14159265f);
            view.x = 65.0f;
        }
        else
        {
            view.y = atan2f(ady, adx) * (180.0f / 3.14159265f);
            view.x = 0.0f;
        }
        fwd = insbot_move_speed.GetFloat(); side = 0.0f;
        buttons |= INS_IN_FORWARD;
        navTarget = approach;
        return true;
    }

    float lipTol = LadderLipTol();
    bool reachedLip = up ? (pos.z >= endHeight - lipTol)
                         : (pos.z <= endHeight + lipTol);
    if (m_ladderPhase == LADDER_PHASE_RECOVERY
        && !m_bodyNoCollideActive && !m_bodyNoCollideEscalating
        && !m_bodyNoCollideAttempted)
        m_ladderPhase = reachedLip ? LADDER_PHASE_DISMOUNT
                                   : LADDER_PHASE_ATTACHED;

    if (m_ladderPhase == LADDER_PHASE_DISMOUNT)
    {
        bool lostHeight = up ? pos.z < endHeight - 18.0f
                             : pos.z > endHeight + 18.0f;
        if (lostHeight && verticallyOnLadder)
        {
            m_ladderPhase = LADDER_PHASE_ATTACHED;
            BotDbg("[insbots] %s ladder %d dismount lost height; climb resumed (z=%.2f endH=%.2f)\n",
                   m_name, ladderIdx, pos.z, endHeight);
        }
    }

    if (m_ladderPhase == LADDER_PHASE_ATTACHED && reachedLip)
    {
        m_ladderPhase = LADDER_PHASE_DISMOUNT;
        BotDbg("[insbots] %s ladder %d dismount latched (z=%.2f top=%.2f floor=%.2f tol=%.1f)\n", m_name, ladderIdx, pos.z, ladder->top.z, landingPoint.z, lipTol);
    }

    bool dismountControl = m_ladderPhase == LADDER_PHASE_DISMOUNT
                        || (m_ladderPhase == LADDER_PHASE_RECOVERY && reachedLip);
    if (!dismountControl)
    {
        view.y = atan2f(-ny, -nx) * (180.0f / 3.14159265f);
        view.x = up ? -65.0f : 65.0f;
        fwd = insbot_move_speed.GetFloat(); side = 0.0f;
        buttons |= INS_IN_FORWARD;
        navTarget = endLip;
        return true;
    }

    // Dismount toward the exact destination NAV area. A ladder may connect to
    // top-forward, top-left, or top-right; the former fixed BACK command only
    // worked for the descending-only top-behind side.
    float edx = landingPoint.x - pos.x, edy = landingPoint.y - pos.y;
    if (edx*edx+edy*edy < 4.0f)
    {
        edx = up ? -nx : nx;
        edy = up ? -ny : ny;
    }
    view.y = atan2f(edy, edx) * (180.0f / 3.14159265f);
    view.x = 0.0f;
    fwd = insbot_move_speed.GetFloat(); side = 0.0f;
    buttons |= INS_IN_FORWARD;
    navTarget = landingPoint;

    float awayLen = sqrtf((landingPoint.x - endLip.x) * (landingPoint.x - endLip.x)
                        + (landingPoint.y - endLip.y) * (landingPoint.y - endLip.y));
    float clearNeed = awayLen < 64.0f ? awayLen : 64.0f;
    float clearDist = 0.0f;
    if (awayLen > 1.0f)
        clearDist = ((pos.x - endLip.x) * (landingPoint.x - endLip.x)
                   + (pos.y - endLip.y) * (landingPoint.y - endLip.y)) / awayLen;
    float landingDx = landingPoint.x - pos.x, landingDy = landingPoint.y - pos.y;
    bool onLanding = PositionOnLadderLanding(toArea, pos);
    bool clearLip = clearDist >= clearNeed - 4.0f
                 || landingDx * landingDx + landingDy * landingDy <= 20.0f * 20.0f;
    if (onLanding && clearLip)
    {
        m_ladderCompletionReady = true;
        m_ladderClimbing = false;
        m_ladderContactConfirmed = false;
        BotDbg("[insbots] %s ladder %d destination landing clear\n",
               m_name, ladderIdx);
    }
    return true;
}
