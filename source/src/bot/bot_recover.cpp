// ============================================================================
// bot_recover.cpp - stuck recovery, safe teleport and local spacing queries.
//
// When a bot wedges, the cheapest correct fix is a short forward teleport onto
// a validated NAV node: full standing hull clear of world geometry and other
// living players. Everything here either picks that destination or answers a
// local "is this space free" question for the steering and choke controllers.
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
#include "turret.h"

// ---- externs this TU needs (same pattern as bot.cpp / objective.cpp) -------
extern IVEngineServer       *engine;
extern IEngineTrace         *enginetrace;
extern IPlayerInfoManager   *playerinfomanager;
extern CGlobalVars          *gpGlobals;
extern CfgVar                insbot_doorway_teleport;
extern CfgVar                insbot_unstuck_teleport;

// Is there room for a full standing player hull at 'dest'? Used to validate a
// teleport target so we never warp a bot into world geometry, a solid prop, or
// another living player. Player occupancy is checked explicitly because the
// world/prop trace deliberately ignores dynamic entities.
bool CInsBot::SafeStandSpot(const Vector &dest, float minPlayerSep) const
{
    if (!enginetrace || !engine || !playerinfomanager || !gpGlobals) return false;

    // A Source player hull is 32 units wide. Keep a little extra separation so
    // two rescued bots do not begin overlapped and remain mutually wedged.
    // TeleportUnstuck relaxes minPlayerSep inside tiny areas (corridors/doorway
    // tiles) after the strict pass fails, so a rescue can still land past the
    // same bad link instead of leaving the bot stuck forever.
    const float playerClearanceSq = minPlayerSep * minPlayerSep;
    for (int i = 1; i <= gpGlobals->maxClients; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (!e || e == m_edict || e->IsFree()) continue;
        IPlayerInfo *pi = playerinfomanager->GetPlayerInfo(e);
        if (!pi || !pi->IsConnected() || pi->IsObserver() || pi->IsDead())
            continue;

        Vector occupied = pi->GetAbsOrigin();
        if (fabsf(occupied.z - dest.z) >= 72.0f) continue;
        float dx = occupied.x - dest.x, dy = occupied.y - dest.y;
        if (dx * dx + dy * dy < playerClearanceSq) return false;
    }

    // Vehicle-aware: TeleportUnstuck must never warp a bot INTO a Humvee
    // hull. CTraceFilterWorldAndPropsOnly cannot see the vehicle, so this
    // check used to answer "safe" for a spot inside the truck - the worst
    // outcome in the whole recovery flow.
    CTraceFilterSteerObstacles filter( const_cast<CInsBot *>( this ) );
    Ray_t ray; trace_t tr;
    Vector mins(-16.0f, -16.0f, 0.0f);
    Vector maxs( 16.0f,  16.0f, 72.0f);
    ray.Init(dest, dest, mins, maxs);
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &tr);
    return !tr.startsolid && !tr.allsolid;
}

bool CInsBot::FindSafeTeleportSpot(int fromArea, int destArea,
                                   const Vector &pos, Vector &dest) const
{
    const NavAreaData *da = g_NavMesh.Area(destArea);
    if (!da) return false;

    Vector base = da->center;
    Vector portal;
    bool havePortal = g_NavMesh.GetPortal(fromArea, destArea, portal);
    if (havePortal)
    {
        base = portal;
        float dx = da->center.x - portal.x, dy = da->center.y - portal.y;
        float len = sqrtf(dx * dx + dy * dy);
        if (len > 1.0f)
        {
            float step = len < 48.0f ? len : 48.0f;
            base.x += dx * (step / len);
            base.y += dy * (step / len);
        }
    }

    float fx = havePortal ? da->center.x - portal.x : da->center.x - pos.x;
    float fy = havePortal ? da->center.y - portal.y : da->center.y - pos.y;
    float flen = sqrtf(fx * fx + fy * fy);
    if (flen > 1.0f) { fx /= flen; fy /= flen; }
    else             { fx = 1.0f; fy = 0.0f; }
    float sx = -fy, sy = fx;

    static const float forwardOffset[] =
        { 0, 0, 0, 64, 64, 64, 128, 128, 128, 192, 192, 192, 0, 0, 256, 256 };
    static const float sideOffset[] =
        { 0, 64, -64, 0, 64, -64, 0, 64, -64, 0, 64, -64, 128, -128, 64, -64 };
    const int candidateCount = sizeof(forwardOffset) / sizeof(forwardOffset[0]);
    int mySlot = (int)(this - g_Bots.m_bots);
    int start = mySlot % candidateCount;

    float minX = da->lo.x + 17.0f, maxX = da->hi.x - 17.0f;
    float minY = da->lo.y + 17.0f, maxY = da->hi.y - 17.0f;
    if (minX > maxX) minX = maxX = da->center.x;
    if (minY > maxY) minY = maxY = da->center.y;

    bool tightArea = (da->hi.x - da->lo.x) < 110.0f
                  || (da->hi.y - da->lo.y) < 110.0f;
    static const float clearanceTiers[] = { 56.0f, 40.0f, 34.0f };
    int tierCount = tightArea ? 3 : 1;
    for (int tier = 0; tier < tierCount; tier++)
    {
        for (int pass = 0; pass < 2; pass++)
        {
            Vector origin = pass == 0 ? base : da->center;
            for (int n = 0; n < candidateCount; n++)
            {
                int c = (start + n) % candidateCount;
                Vector tryDest = origin;
                tryDest.x += fx * forwardOffset[c] + sx * sideOffset[c];
                tryDest.y += fy * forwardOffset[c] + sy * sideOffset[c];
                if (tryDest.x < minX) tryDest.x = minX;
                if (tryDest.x > maxX) tryDest.x = maxX;
                if (tryDest.y < minY) tryDest.y = minY;
                if (tryDest.y > maxY) tryDest.y = maxY;
                tryDest.z = g_NavMesh.GetZ(destArea, tryDest.x, tryDest.y) + 8.0f;

                float dx = tryDest.x - pos.x, dy = tryDest.y - pos.y;
                float dz = tryDest.z - pos.z;
                if (dx * dx + dy * dy + dz * dz > 600.0f * 600.0f) continue;
                if (!SafeStandSpot(tryDest, clearanceTiers[tier])) continue;
                dest = tryDest;
                return true;
            }
        }
    }
    return false;
}

// NavBot-style rescue. A body-recovery session owns its cached edge so an
// unrelated periodic repath cannot shadow the doorway it is still clearing.
void CInsBot::ResetDoorwayRunway()
{
    memset(&m_doorwayRunway, 0, sizeof(m_doorwayRunway));
    m_doorwayApexBrakingUntil = 0.0f;
}

void CInsBot::OnRelocated(const char *reason)
{
    ResetDoorwayRunway();
    m_bestDist = 3.4e38f;
    ResetDoorwayRunway();

    m_pathLen = 0;
    m_pathResult = BOT_PATH_NONE;
    m_pathContinues = false;
    m_pathCursor = 0;
    m_repathTime = 0.0f;
    m_hasMoveTarget = false;
    m_haveRescueLink = false;
    m_stuckSince = 0.0f;
    m_stuckStage = 0;
    m_unstuckUntil = 0.0f;
    ResetLadderTraversal(true);
    CompleteBodyNoCollideTeleport(reason);
}

bool CInsBot::TeleportUnstuck(const Vector &pos, float now,
                              bool doorwayRecovery, int *failureReason,
                              bool expandForward)
{
    if (failureReason) *failureReason = TELEPORT_OK;
    if ((doorwayRecovery && !insbot_doorway_teleport.GetBool())
        || (!doorwayRecovery && !insbot_unstuck_teleport.GetBool()))
    {
        if (failureReason) *failureReason = TELEPORT_DISABLED;
        return false;
    }

    if (now < m_nextTeleportOk)
    {
        if (failureReason) *failureReason = TELEPORT_COOLDOWN;
        return false;
    }
    if (!m_ctrl)
    {
        if (failureReason) *failureReason = TELEPORT_NO_CONTROLLER;
        return false;
    }
    if (!g_NavMesh.IsLoaded())
    {
        if (failureReason) *failureReason = TELEPORT_NAV_NOT_LOADED;
        return false;
    }

    int here = CurrentArea(pos);
    if (here < 0) here = g_NavMesh.FindNearestArea(pos);
    if (here < 0)
    {
        if (failureReason) *failureReason = TELEPORT_NO_CURRENT_AREA;
        return false;
    }

    int destNode = -1;
    int destPathIndex = -1;
    bool sessionOwnsEdge = (m_bodyNoCollideActive || m_bodyNoCollideEscalating)
                         && m_haveRescueLink && here == m_rescueFromArea
                         && g_NavMesh.HasConnection(here, m_rescueToArea);
    if (m_ladderPhase != LADDER_PHASE_NONE && m_ladderToArea >= 0
        && g_NavMesh.Area(m_ladderToArea))
        destNode = m_ladderToArea;
    else if (sessionOwnsEdge)
        destNode = m_rescueToArea;
    else if (m_pathLen > 0 && m_pathCursor >= 0 && m_pathCursor < m_pathLen)
    {
        int nextNode = m_path[m_pathCursor];
        if (g_NavMesh.HasConnection(here, nextNode))
        {
            destNode = nextNode;
            destPathIndex = m_pathCursor;
        }
        else if (m_pathCursor + 1 < m_pathLen
                 && g_NavMesh.HasConnection(here, m_path[m_pathCursor + 1]))
        {
            destNode = m_path[m_pathCursor + 1];
            destPathIndex = m_pathCursor + 1;
        }
        else if (m_pathCursor - 1 >= 0
                 && g_NavMesh.HasConnection(here, m_path[m_pathCursor - 1]))
        {
            destNode = m_path[m_pathCursor - 1];
            destPathIndex = m_pathCursor - 1;
        }
    }
    else if (m_haveRescueLink && here == m_rescueFromArea
             && g_NavMesh.HasConnection(here, m_rescueToArea))
        destNode = m_rescueToArea;
    if (destNode < 0)
    {
        if (failureReason) *failureReason = TELEPORT_NO_CONNECTED_EDGE;
        return false;
    }
    if (!g_NavMesh.Area(destNode))
    {
        if (failureReason) *failureReason = TELEPORT_DEST_AREA_MISSING;
        return false;
    }

    int destinations[2] = { destNode, -1 };
    if (expandForward)
    {
        int suffixStart = m_pathCursor;
        if (suffixStart < 0) suffixStart = 0;
        if (destPathIndex < suffixStart)
        {
            // A cached session edge may also appear in the live path. Only an
            // occurrence at or ahead of the cursor can authorize expansion;
            // an older duplicate would point behind the bot.
            for (int i = suffixStart; i < m_pathLen; i++)
            {
                if (m_path[i] == destNode)
                {
                    destPathIndex = i;
                    break;
                }
            }
        }
        if (destPathIndex >= suffixStart && destPathIndex + 1 < m_pathLen
            && g_NavMesh.HasConnection(destNode,
                                       m_path[destPathIndex + 1]))
            destinations[1] = m_path[destPathIndex + 1];
    }

    Vector dest;
    int usedNode = -1;
    for (int i = 0; i < 2 && destinations[i] >= 0; i++)
    {
        int fromArea = i == 0 ? here : destinations[i - 1];
        if (FindSafeTeleportSpot(fromArea, destinations[i], pos, dest))
        {
            usedNode = destinations[i];
            break;
        }
    }
    if (usedNode < 0)
    {
        if (failureReason) *failureReason = TELEPORT_NO_SAFE_DESTINATION;
        return false;
    }

    m_ctrl->SetAbsOrigin(dest);
    m_nextTeleportOk = now + 2.5f;
    OnRelocated(doorwayRecovery ? "doorway teleport" : "teleport-unstuck");
    BotDbg("[insbots] %s teleport-unstuck onto node %d (%.0f %.0f %.0f)%s\n",
           m_name, usedNode, dest.x, dest.y, dest.z,
           usedNode != destNode ? " via expanded forward search" : "");
    return true;
}

// world-only line of sight between two points (ignores entities): true = clear
bool CInsBot::WorldClear(const Vector &a, const Vector &b) const
{
    CTraceFilterWorldOnly wf;
    Ray_t ray; trace_t t;
    ray.Init(a, b);
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &wf, &t);
    return t.fraction >= 1.0f;
}

bool CInsBot::CanShootMarker(const Vector &marker) const
{
    // Trace from the firing eye to the marker. Same mask as CanShootNow so
    // smoke (non-solid particle) and glass (CONTENTS_WINDOW stripped) pass;
    // brushes/doors/props block.
    Ray_t ray;
    ray.Init(FireEyePos(false), marker);
    CTraceFilterSkipSelf filter(EdictHandleEntity(m_edict));
    trace_t tr;
    enginetrace->TraceRay(ray, MASK_SHOT & ~(CONTENTS_HITBOX | CONTENTS_WINDOW), &filter, &tr);
    return tr.fraction >= 0.985f;
}

// Is 'probe' a safe sidestep landing spot? Requires a clear horizontal path at
// waist height plus walkable ground beneath, so crowd steering never shoves a
// bot into a wall, a gap, or off a ledge. Dynamic players are checked by the
// caller; world/prop traces deliberately ignore them.
bool CInsBot::SideStepOpen(const Vector &probe) const
{
    if (!enginetrace || !m_info) return false;
    // Vehicle-aware: a lateral move must not be validated against a filter
    // that cannot see the Humvee. CTraceFilterWorldAndPropsOnly returns false
    // from ShouldHitEntity for every entity, so the truck was invisible here
    // and this function could answer "open" with the hull right there.
    CTraceFilterSteerObstacles wf( const_cast<CInsBot *>( this ) );
    Ray_t ray; trace_t tr;
    // Sweep the full standing player hull along the sidestep. A point ray at
    // waist height can clear a jamb/prop while the bot's 32u-wide hull clips
    // it, which made the supposedly-safe pass shove bots into doorway walls.
    Vector from = m_info->GetAbsOrigin();
    Vector mins(-16.0f, -16.0f, 0.0f);
    Vector maxs( 16.0f,  16.0f, 72.0f);
    ray.Init(from, probe, mins, maxs);
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &wf, &tr);
    if (tr.startsolid || tr.allsolid || tr.fraction < 1.0f) return false;

    // Floor check is NOT optional: the lateral sweep alone would happily clear
    // a ledge edge, and a sidestep is the manoeuvre most likely to walk a bot
    // off one. Keep both traces.
    Vector to = probe + Vector(0, 0, 36);
    ray.Init(to + Vector(0, 0, 24), to - Vector(0, 0, 72));
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &wf, &tr);
    return !tr.startsolid && !tr.allsolid && tr.fraction < 1.0f;
}

// Is the lane behind this bot clear enough to back into? Checks the horizontal
// trace for walls/props and any teammate within minGap behind, so reversing out
// of a doorway/choke never rams into the following pack (the classic
// multi-bot corner/doorway box-in).
bool CInsBot::ClearBehind(float moveYaw, float minGap) const
{
    if (!enginetrace || !m_info) return false;
    Vector pos = m_info->GetAbsOrigin();
    float yr = moveYaw * (3.14159265f / 180.0f);
    Vector fwdDir(cosf(yr), sinf(yr), 0);
    Vector sideDir(-sinf(yr), cosf(yr), 0);

    // Vehicle-aware check FIRST. WorldClear is deliberately world-only (its
    // other caller, FindCoverSpot, wants a truck in the line to read as "not
    // exposed"), so the vehicle test belongs here instead of inside it.
    // Without this, a back-out could report "clear" with the truck directly
    // behind and reverse straight into it.
    {
        CTraceFilterSteerObstacles vf( const_cast<CInsBot *>( this ) );
        Ray_t vray; trace_t vtr;
        Vector vFrom = pos + Vector(0, 0, 36);
        Vector vTo = pos - fwdDir * 64.0f + Vector(0, 0, 36);
        vray.Init(vFrom, vTo);
        enginetrace->TraceRay(vray, MASK_PLAYERSOLID, &vf, &vtr);
        if (vtr.fraction < 1.0f && vtr.m_pEnt)
        {
            IServerUnknown *unk = (IServerUnknown *)vtr.m_pEnt;
            IServerNetworkable *net = unk ? unk->GetNetworkable() : 0;
            ServerClass *sc = net ? net->GetServerClass() : 0;
            if (sc && IsDriveableClass( sc->GetName() ))
                return false;
        }
    }

    Vector from = pos + Vector(0, 0, 36);
    Vector to = pos - fwdDir * 64.0f;
    to.z += 36.0f;
    if (!WorldClear(from, to)) return false;

    int myTeam = m_info->GetTeamIndex();
    for (int i = 0; i < 64; i++)
    {
        CInsBot *other = &g_Bots.m_bots[i];
        if (!other->m_inUse || other == this || !other->m_info || other->m_info->IsDead())
            continue;
        if (other->m_info->GetTeamIndex() != myTeam) continue;
        Vector p = other->m_info->GetAbsOrigin();
        if (fabsf(p.z - pos.z) > 72.0f) continue;
        float rx = p.x - pos.x, ry = p.y - pos.y;
        float behind = -(rx * fwdDir.x + ry * fwdDir.y);
        float lateral = fabsf(rx * sideDir.x + ry * sideDir.y);
        if (behind > -8.0f && behind < minGap && lateral < 56.0f)
            return false;
    }
    return true;
}

// Is a same-team bot close behind along -yawDeg (roughly in the same lane)?
// Used to keep a leader from reversing into its follower while a pair is
// queuing for, or already inside, a tight corridor/doorway. Wider than
// ClearBehind (which only protects the immediate 60u): when the column is
// forming, the bot ahead must never walk backward into the person behind it.
bool CInsBot::TeammateBehind(float yawDeg, float range, float lateralRange) const
{
    if (!m_info) return false;
    Vector pos = m_info->GetAbsOrigin();
    float yr = yawDeg * (3.14159265f / 180.0f);
    Vector fwd(cosf(yr), sinf(yr), 0);
    Vector side(-sinf(yr), cosf(yr), 0);
    int myTeam = m_info->GetTeamIndex();
    for (int i = 0; i < 64; i++)
    {
        const CInsBot *other = &g_Bots.m_bots[i];
        if (!other->m_inUse || other == this || !other->m_info || other->m_info->IsDead())
            continue;
        if (other->m_info->GetTeamIndex() != myTeam) continue;
        Vector p = other->m_info->GetAbsOrigin();
        if (fabsf(p.z - pos.z) > 72.0f) continue;
        float rx = p.x - pos.x, ry = p.y - pos.y;
        float behind = -(rx * fwd.x + ry * fwd.y);
        float lateral = fabsf(rx * side.x + ry * side.y);
        if (behind >= 0.0f && behind <= range && lateral <= lateralRange)
            return true;
    }
    return false;
}

// Is a same-team bot already at/near the current waypoint's LOWER landing
// (used before walking off an elevation drop)? When one is, the bot must hold
// at the edge instead of falling onto it. The landing is the drop waypoint
// (m_moveTarget), which sits below the bot's feet.
bool CInsBot::TeammateAtLanding() const
{
    if (!m_info || !m_hasMoveTarget) return false;
    int myTeam = m_info->GetTeamIndex();
    for (int i = 0; i < 64; i++)
    {
        const CInsBot *other = &g_Bots.m_bots[i];
        if (!other->m_inUse || other == this || !other->m_info || other->m_info->IsDead())
            continue;
        if (other->m_info->GetTeamIndex() != myTeam) continue;
        Vector p = other->m_info->GetAbsOrigin();
        // On the landing (or still on the slope/step just above it). Bots on
        // the higher floor behind the edge are excluded by the upper bound.
        if (p.z < m_moveTarget.z - 80.0f || p.z > m_moveTarget.z + 100.0f)
            continue;
        // The landing can be only ~50u from the drop edge, so a z-window +
        // 110u radius alone catches bots still standing at the edge (same z
        // as the walker) and the whole column deadlocks holding at the drop.
        // Require the teammate to actually be BELOW the walker's feet.
        Vector pos = m_info->GetAbsOrigin();
        if (p.z >= pos.z - 20.0f)
            continue;
        float dx = p.x - m_moveTarget.x, dy = p.y - m_moveTarget.y;
        if (dx * dx + dy * dy < 110.0f * 110.0f)
            return true;
    }
    return false;
}

// Find a nearby nav spot that BREAKS line-of-sight to 'threat' (i.e. cover).
// Searches the bot's area plus one/two rings of neighbours, prefers the closest
// spot that is hidden and not nearer to the enemy. Falls back to a short retreat
// straight away from the threat when there's no mesh.
bool CInsBot::FindCover(const Vector &threat, Vector &out) const
{
    Vector pos = m_info->GetAbsOrigin();
    Vector threatEye = threat; threatEye.z += 45.0f;

    if (g_NavMesh.IsLoaded())
    {
        int here = CurrentArea(pos);
        if (here >= 0)
        {
            int cand[160]; int nc = 0;
            const NavAreaData *a = g_NavMesh.Area(here);
            for (int k = 0; k < a->connCount && nc < 160; k++) cand[nc++] = a->connIdx[k];
            int ring1 = nc;
            for (int r = 0; r < ring1 && nc < 160; r++)
            {
                const NavAreaData *b = g_NavMesh.Area(cand[r]); if (!b) continue;
                for (int k = 0; k < b->connCount && nc < 160; k++)
                {
                    int j = b->connIdx[k];
                    if (j == here) continue;
                    bool dup = false;
                    for (int q = 0; q < nc; q++) if (cand[q] == j) { dup = true; break; }
                    if (!dup) cand[nc++] = j;
                }
            }
            float myT = xyd(pos, threat);
            float bestD = 1e18f; int best = -1;
            for (int i = 0; i < nc; i++)
            {
                const NavAreaData *c = g_NavMesh.Area(cand[i]); if (!c) continue;
                Vector eye = c->center; eye.z += 58.0f;
                if (WorldClear(eye, threatEye)) continue;            // still exposed -> not cover
                if (xyd(c->center, threat) < myT - 48.0f) continue;  // don't advance toward the enemy
                float d = xyd(c->center, pos);
                if (d < bestD) { bestD = d; best = cand[i]; }
            }
            if (best >= 0) { out = g_NavMesh.Area(best)->center; return true; }
        }
    }

    // fallback: a short hop directly away from the threat
    Vector away = pos - threat; away.z = 0;
    float L = sqrtf(away.x * away.x + away.y * away.y);
    if (L < 1.0f) return false;
    away.x /= L; away.y /= L;
    out = pos + away * 200.0f; out.z = pos.z;
    return true;
}
