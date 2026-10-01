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

// ---- externs this TU needs (same pattern as bot.cpp / objective.cpp) -------
extern IVEngineServer       *engine;
extern IEngineTrace         *enginetrace;
extern IPlayerInfoManager   *playerinfomanager;
extern CGlobalVars          *gpGlobals;
extern CfgVar                insbot_move_speed;
extern CfgVar                insbot_sprint;
extern CfgVar                insbot_streaming;
extern CfgVar                insbot_doorway_teleport;
extern CfgVar                insbot_doorway_teleport_time;
extern CfgVar                insbot_stuck_time;
extern CfgVar                insbot_debug;

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
        // blocked: test left/right 60 degrees, pick the more open one
        float best = m_headingYaw + 120.0f; float bestFrac = -1.0f;
        for (int s = -1; s <= 1; s += 2)
        {
            float y = m_headingYaw + 60.0f * s;
            float yr = y * (3.14159265f / 180.0f);
            Vector d2(cosf(yr), sinf(yr), 0);
            ray.Init(waist, waist + d2 * 96.0f);
            trace_t t2;
            enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &steerFilter, &t2);
            if (t2.fraction > bestFrac) { bestFrac = t2.fraction; best = y; }
        }
        while (best > 180.0f) best -= 360.0f;
        while (best < -180.0f) best += 360.0f;
        m_headingYaw = best;

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
// The episode ends the moment the forward line is clear, so the base is
// re-captured for the next obstacle. A gap longer than one hold means the bot
// left this path (nav branch, SteerRoam, teleport), so the base is re-captured
// rather than reusing a stale heading.
static float s_steerBaseYaw[64]  = { 0 };
static float s_steerBaseTime[64] = { 0 };
static float s_steerLastYaw[64]  = { 0 };

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
    int mySlot = (m_ordinal >= 0 && m_ordinal < 64) ? m_ordinal : -1;
    bool baseFresh = (mySlot >= 0)
                  && (now - s_steerBaseTime[mySlot]) <= 0.75f
                  && s_steerLastYaw[mySlot] == m_headingYaw;

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
        float baseYaw = baseFresh ? s_steerBaseYaw[mySlot] : m_headingYaw;
        if (!baseFresh)
        {
            if (mySlot >= 0)
            {
                s_steerBaseYaw[mySlot]  = m_headingYaw;
                s_steerLastYaw[mySlot]  = m_headingYaw;
            }
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
        if (mySlot >= 0)
        {
            s_steerBaseYaw[mySlot]  = baseYaw;
            s_steerBaseTime[mySlot] = now;
            s_steerLastYaw[mySlot]  = m_headingYaw;
        }
    }
    else
    {
        // Forward line is clear: the blocked episode is over. Invalidate the
        // base so the next episode re-captures it from the live heading.
        if (mySlot >= 0) s_steerBaseTime[mySlot] = -1000.0f;

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

void CInsBot::ApplyCrowdSteering(float moveYaw, float viewYaw, float &fwd,
                                 float &side, int &buttons)
{
    int streamingMode = insbot_streaming.GetInt();
    Vector pos = m_info->GetAbsOrigin();
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
    bool hadithaDoorEdge = myCur >= 0 && myNext >= 0
                        && IsHadithaTightDoorEdge(myCur, myNext);
    bool hadithaEastDoorEdge = myCur >= 0 && myNext >= 0
                            && IsHadithaEastDoorEdge(myCur, myNext);
    if (hadithaDoorEdge || hadithaEastDoorEdge)
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
            // frame. Haditha remains covered by its explicit edge verifier.
            bool physicalDoorChain = physicalPassage
                                  || IsHadithaTightDoorEdge(myCur, myNext);
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
            // whole team into rooms before short passages such as Haditha's
            // id468->id581 plank entrance. An 80u center gap still leaves about
            // 48u between 32u hulls while letting the queue fit and flow.
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
        // top of each other at elevation steps (Haditha (-2612,-83)).
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
                    // this branch. Haditha's east corner keeps its verified
                    // multi-slot landing; every other detected doorway uses
                    // ordinary TeleportUnstuck and its on-path hull checks.
                    float teleportDelay = insbot_doorway_teleport_time.GetFloat();
                    if (teleportDelay < 0.2f) teleportDelay = 0.2f;
                    float bodyStart = insbot_stuck_time.GetFloat();
                    if (bodyStart < 0.2f) bodyStart = 0.2f;
                    if (!hadithaEastDoorEdge && !m_bodyNoCollideAttempted
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
                    bool transferred = transferDue && (hadithaEastDoorEdge
                        ? TransferHadithaEastDoor(now)
                        : TeleportUnstuck(pos, now, true));
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
            if (cur && !ladderMove)
            {
                // Crouch only on imminent physical contact. nav_generate
                // over-flags CROUCH on corridor tiles, so the flag alone no
                // longer ducks bots while they stream through a passage; the
                // LowCeilingAhead trace below only fires when a real overhang
                // is within a few feet.
                if (cur->attributes & NAVATTR_JUMP)   buttons |= IN_JUMP;
            }
            if (!ladderMove && LowCeilingAhead(mvYaw)) buttons |= IN_DUCK;
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
                            edgeStep = m_moveTarget.z > pos.z + 16.0f;
                    }
                }
            }
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
    if (!m_traversingLadder
        && (!m_spawnNoCollideActive || m_spawnNoCollideExpired))
    {
        bool collisionGrace = HasBodyNoCollide() || m_spawnNoCollideActive;
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
            if (m_dropAhead)
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
}
