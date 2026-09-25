// ============================================================================
// bot_squad.cpp - squad assignment, follower behaviour, per-bot Think.
// ============================================================================
#include <stdio.h>
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
#include "voice.h"
#include "profile.h"
#include "linux_offsets.h"
#include "bot_internal.h"

// ---- externs this TU needs (same pattern as bot.cpp / objective.cpp) -------
extern IVEngineServer       *engine;
extern IEngineTrace         *enginetrace;
extern IPlayerInfoManager   *playerinfomanager;
extern CGlobalVars          *gpGlobals;
extern IServerPluginHelpers *helpers;
extern CfgVar                insbot_ads_mode;
extern CfgVar                insbot_dead_cmds;
extern CfgVar                insbot_cover_search_radius;
extern CfgVar                insbot_cover_v2;
extern CfgVar                insbot_debug;
extern CfgVar                insbot_intel_time;

// ============================================================================
// Squad Follower & Fireteam AI (v0.20.0)
// ============================================================================
void CInsBot::SetSquadAssignment(int leaderIndex, int slot, int order)
{
    m_squadLeaderIndex = leaderIndex;
    m_squadSlot = slot;
    m_squadOrder = order;
    m_squadOrderTime = Now();
    m_squadHoldPos = m_info ? m_info->GetAbsOrigin() : Vector(0, 0, 0);
    m_squadScanYaw = m_headingYaw;
    m_squadFormationYaw = 0.0f;
    m_lastLeaderTime = 0.0f;
    m_lastSquadTime = 0.0f;
    m_lastLeaderPos = Vector(0, 0, 0);
    m_haveHoldSpot = false;
    m_haveSniperSpot = false;
    m_patrolling = false;
}

void CInsBot::ClearSquadAssignment()
{
    m_squadLeaderIndex = 0;
    m_squadSlot = 0;
    m_squadOrder = SQUAD_ORDER_NONE;
    m_squadOrderTime = 0.0f;
    m_squadScanYaw = 0.0f;
    m_squadFormationYaw = 0.0f;
    m_lastLeaderTime = 0.0f;
    m_lastSquadTime = 0.0f;
    m_lastLeaderPos = Vector(0, 0, 0);
}

void CInsBot::UpdateSquadFollow(float now, float &fwd, float &side, int &buttons, QAngle &view)
{
    if (m_squadLeaderIndex <= 0) return;

    edict_t *le = engine->PEntityOfEntIndex(m_squadLeaderIndex);
    if (!le || le->IsFree() || !le->GetUnknown())
    {
        ClearSquadAssignment();
        return;
    }

    IPlayerInfo *lpi = playerinfomanager ? playerinfomanager->GetPlayerInfo(le) : 0;
    if (!lpi || !lpi->IsConnected())
    {
        ClearSquadAssignment();
        return;
    }

    if (lpi->IsDead())
    {
        // Dead-leader fallback: hold defensive wedge around last known position
        if (m_deadLeaderFallbackSince == 0.0f)
        {
            m_deadLeaderFallbackSince = now;
            m_deadLeaderCorpsePos = (m_lastLeaderTime > 0.0f) ? m_lastLeaderPos : m_info->GetAbsOrigin();
            if (engine)
            {
                char cmdBuf[80];
                _snprintf(cmdBuf, sizeof(cmdBuf), "insbot_squad_leader_down %d\n", m_squadLeaderIndex);
                engine->ServerCommand(cmdBuf);
            }
        }
        float slotAngle = m_squadFormationYaw + (m_squadSlot == 0 ? -120.0f : (m_squadSlot == 1 ? 120.0f : 180.0f));
        float slotDist  = (m_squadSlot == 2) ? 140.0f : 90.0f;
        float rad = slotAngle * (3.14159265f / 180.0f);
        Vector target = m_deadLeaderCorpsePos + Vector(cosf(rad) * slotDist, sinf(rad) * slotDist, 0.0f);
        float guardYaw = m_squadFormationYaw + (m_squadSlot == 0 ? -70.0f : (m_squadSlot == 1 ? 70.0f : 180.0f));
        float distToWedge = xyd(m_info->GetAbsOrigin(), target);
        if (distToWedge > 36.0f)
            MoveToward(target, fwd, side, buttons, view);
        else
        {
            fwd = 0.0f; side = 0.0f;
            if (!m_enemyIndex)
            {
                view.y = fmodf(guardYaw + 360.0f, 360.0f);
                view.x = 0.0f;
                m_curAngles = view;
            }
        }
        return;
    }
    else
    {
        m_deadLeaderFallbackSince = 0.0f;
    }

    Vector pos = m_info->GetAbsOrigin();
    Vector leaderPos = lpi->GetAbsOrigin();
    QAngle leaderAngles = lpi->GetAbsAngles();
    float leaderYaw = leaderAngles.y;

    // Physical stall failsafe
    float distToLeader = xyd(pos, leaderPos);
    if (distToLeader > 400.0f && m_stuckSince > 0.0f && (now - m_stuckSince > 10.0f))
    {
        ClearSquadAssignment();
        return;
    }

    // Track leader motion
    float dt = (m_lastSquadTime > 0.0f) ? (now - m_lastSquadTime) : 0.1f;
    if (dt <= 0.0f || dt > 1.0f) dt = 0.1f;

    Vector leaderVel(0, 0, 0);
    if (m_lastLeaderTime > 0.0f && (now - m_lastLeaderTime) > 0.02f)
        leaderVel = (leaderPos - m_lastLeaderPos) / (now - m_lastLeaderTime);
    m_lastLeaderPos = leaderPos;
    m_lastLeaderTime = now;

    // Fix 3: track leader's forward direction for anti-boxing path detection
    float leaderSpdNow = sqrtf(leaderVel.x * leaderVel.x + leaderVel.y * leaderVel.y);
    if (leaderSpdNow > 5.0f)  // lowered from 10 to catch slow walk
    {
        m_lastLeaderMoveDir.x = leaderVel.x / leaderSpdNow;
        m_lastLeaderMoveDir.y = leaderVel.y / leaderSpdNow;
        m_lastLeaderMoveDir.z = 0.0f;
    }

    float leaderSpeed = sqrtf(leaderVel.x * leaderVel.x + leaderVel.y * leaderVel.y);

    // Change 2: Formation heading — velocity-direction-only.
    // When leader moves: track velocity direction. When leader stops: hold
    // last movement direction. No snap to leaderYaw. Prevents 180-degree anchor flips.
    if (leaderSpeed > 45.0f)
    {
        float rawHeading = atan2f(leaderVel.y, leaderVel.x) * (180.0f / 3.14159265f);
        if (m_lastSquadTime <= 0.0f)
            m_squadFormationYaw = rawHeading;
        else
        {
            float diff = fmodf(rawHeading - m_squadFormationYaw + 540.0f, 360.0f) - 180.0f;
            float maxTurn = 160.0f * dt;
            if (diff > maxTurn) diff = maxTurn;
            else if (diff < -maxTurn) diff = -maxTurn;
            m_squadFormationYaw = fmodf(m_squadFormationYaw + diff + 360.0f, 360.0f);
        }
    }
    // else: hold last movement direction
    m_lastSquadTime = now;

    // Fix 4: Leader-in-cover detection (1 Hz throttle)
    // Primary: check if the leader is near a CoverPoint whose blockMask protects
    //   from the direction of the freshest known enemy.
    // Fallback: crouched/proned + stationary (maxs.z < 40 && speed < 30)
    if (now - m_leaderCoverCheckTime > 1.0f)
    {
        m_leaderCoverCheckTime = now;
        Vector lm = lpi->GetPlayerMaxs();
        bool crouchedOrProned = (lm.z < 40.0f);
        bool stationary = (leaderSpeed < 30.0f);

        bool inCoverByNav = false;
        if (g_Bots.m_coverPoints && g_Bots.CoverCount() > 0)
        {
            int leaderTeam = lpi->GetTeamIndex();
            float searchR = insbot_cover_search_radius.GetFloat();
            if (searchR < 100.0f) searchR = 100.0f;

            // find freshest valid enemy from the leader's team's intel
            Vector threatPos(0, 0, 0);
            float newestTime = 0.0f;
            for (int ti = 1; ti <= gpGlobals->maxClients; ti++)
            {
                if (!g_Intel[leaderTeam].enemy[ti].valid) continue;
                float age = now - g_Intel[leaderTeam].enemy[ti].time;
                if (age > insbot_intel_time.GetFloat()) continue;
                if (g_Intel[leaderTeam].enemy[ti].time > newestTime)
                {
                    newestTime = g_Intel[leaderTeam].enemy[ti].time;
                    threatPos = g_Intel[leaderTeam].enemy[ti].pos;
                }
            }

            if (newestTime > 0.0f)
            {
                // we have a threat - check CoverPoints near the leader
                for (int ci = 0; ci < g_Bots.CoverCount() && !inCoverByNav; ci++)
                {
                    const CoverPoint *cp = g_Bots.CoverAt(ci);
                    if (!cp) continue;

                    float dLeaderCand = sqrtf((cp->pos - leaderPos).LengthSqr());
                    if (dLeaderCand > 60.0f) continue;   // must be close to the leader

                    int sec = CoverSectorFor(cp->pos, threatPos);
                    if (cp->blockMask[sec])
                        inCoverByNav = true;   // a nearby cover point protects from the threat
                }
            }
        }

        bool wasInCover = m_leaderInCover;
        m_leaderInCover = inCoverByNav || (crouchedOrProned && stationary);

        // Log state changes and draw indicator when insbot_draw is enabled
        if (m_leaderInCover != wasInCover)
        {
            BotDbg("[insbots] leader %d cover=%d (nav=%d crouch=%d)",
                   m_squadLeaderIndex, m_leaderInCover ? 1 : 0,
                   inCoverByNav ? 1 : 0, (crouchedOrProned && stationary) ? 1 : 0);
        }
    }

    // Fix 4: clear the per-tick handoff flags at the start of every attempt
    m_coverFollowSet = false;
    m_sidestepActive = false;
    m_suppressFire = false;

    // Change 8: Follower cover-follow when leader is in cover.
    // Explicit HOLD/BACKUP/SUPPRESS orders are honored before this only when the
    // leader is not in cover; when they are in cover, fall through here.
    if (m_leaderInCover && m_squadOrder == SQUAD_ORDER_FOLLOW)
    {
        // Use personal enemy, shared team intel, or leader's reverse bearing.
        // Never pass the bot's own position as a threat — it produces
        // geometrically meaningless cover scores.
        Vector threat;
        if (m_enemyIndex)
        {
            threat = m_enemyLastPos;
        }
        else
        {
            // No personal enemy: query team intel for freshest known threat
            int myTeam = m_info ? m_info->GetTeamIndex() : 0;
            float now = gpGlobals->curtime;
            Vector intelPos;
            int intelIdx = IntelFreshest(myTeam, now, insbot_intel_time.GetFloat(),
                                         pos, 2000.0f, intelPos);
            if (intelIdx > 0)
            {
                threat = intelPos;
            }
            else
            {
                // No intel at all: synthesize threat behind the formation
                // (reverse of formation heading) so cover is chosen facing outward.
                float revYaw = fmodf(m_squadFormationYaw + 180.0f, 360.0f);
                float rad = revYaw * (3.14159265f / 180.0f);
                threat = pos + Vector(cosf(rad) * 500.0f, sinf(rad) * 500.0f, 0.0f);
            }
        }
        Vector coverSpot;
        int coverTier;
        if (ChooseCoverSpot(threat, coverSpot, coverTier))
        {
            float coverDist = xyd(pos, coverSpot);
            if (coverDist > 40.0f)
                MoveToward(coverSpot, fwd, side, buttons, view);
            else
            {
                fwd = 0.0f; side = 0.0f;
                if (!m_enemyIndex)
                {
                    float gy = m_squadFormationYaw + (m_squadSlot == 0 ? -70.0f : (m_squadSlot == 1 ? 70.0f : 180.0f));
                    view.y = fmodf(gy + 360.0f, 360.0f);
                    view.x = 0.0f;
                    m_curAngles = view;
                }
                // Duck behind low cover; tall cover protects while standing
                if (m_chosenCoverIsLow)
                    buttons |= IN_DUCK;
            }
            m_coverFollowSet = true;   // Fix 4 handoff (vulnerable branch may run later)
            return;   // cover-follow replaces normal formation movement
        }
        // If no cover found, fall through to normal formation movement below
    }

    // Change 1: Compute wedge slot geometry BEFORE stance mimicry (needed for distToSlot)
    float slotAngle = m_squadFormationYaw + (m_squadSlot == 0 ? -120.0f : (m_squadSlot == 1 ? 120.0f : 180.0f));
    float slotDist = (m_squadSlot == 2) ? 140.0f : 90.0f;
    float rad = slotAngle * (3.14159265f / 180.0f);
    Vector targetSlot = leaderPos + Vector(cosf(rad) * slotDist, sinf(rad) * slotDist, 0.0f);
    float guardYaw = fmodf(m_squadFormationYaw + (m_squadSlot == 0 ? -70.0f : (m_squadSlot == 1 ? 70.0f : 180.0f)) + 360.0f, 360.0f);
    float distToSlot = xyd(pos, targetSlot);
    distToLeader = xyd(pos, leaderPos);

    // Change 6: Stance mimicry — only duck when near slot AND leader stationary (Option C)
    Vector leaderMaxs = lpi->GetPlayerMaxs();
    bool leaderCrouchedOrProned = (leaderMaxs.z < 40.0f);
    bool leaderStationary = (leaderSpeed < 30.0f);
    bool nearSlot = (distToSlot < 80.0f);
    if (leaderCrouchedOrProned && leaderStationary && nearSlot)
        buttons |= IN_DUCK;

    // Order guard: only FOLLOW respects incidental cap-zone proximity.
    bool allowCaptureZoneIntegration = (m_squadOrder == SQUAD_ORDER_FOLLOW);

    if (allowCaptureZoneIntegration)
    {
        // Capture Zone Integration
        bool leaderInObjective = false;
        if (g_Objectives.IsReady())
        {
            for (int i = 0; i < g_Objectives.Count(); i++)
            {
                const ObjPoint *op = g_Objectives.Get(i);
                if (op && PlayerInsideObjective(op, lpi, leaderPos))
                {
                    leaderInObjective = true;
                    if (!m_haveHoldSpot)
                    {
                        m_holdSpot = ObjectiveCornerHoldPoint(op, pos, m_squadSlot);
                        m_haveHoldSpot = true;
                    }
                    if (xyd(pos, m_holdSpot) > 24.0f)
                        MoveToward(m_holdSpot, fwd, side, buttons, view);
                    else
                        { fwd = 0.0f; side = 0.0f; }
                    return;
                }
            }
        }
        if (!leaderInObjective)
            m_haveHoldSpot = false;
    }

    // 3. Squad Order Execution
    if (m_squadOrder == SQUAD_ORDER_HOLD)
    {
        float distToHold = xyd(pos, m_squadHoldPos);
        if (distToHold > 36.0f)
            MoveToward(m_squadHoldPos, fwd, side, buttons, view);
        else
        {
            fwd = 0.0f; side = 0.0f;
            if (!m_enemyIndex)
            {
                view.y = m_squadScanYaw;
                view.x = 0.0f;
                m_curAngles = view;
            }
        }
        return;
    }
    else if (m_squadOrder == SQUAD_ORDER_BACKUP)
    {
        if (distToLeader > 80.0f)
        {
            MoveToward(leaderPos, fwd, side, buttons, view);
            if (fwd > 100.0f && !m_traversingLadder && (!m_doorwayRunway.active || m_doorwayRunway.doorClear))
                buttons |= INS_IN_SPRINT;
        }
        else
        {
            fwd = 0.0f; side = 0.0f;
            m_squadOrder = SQUAD_ORDER_FOLLOW;
        }
        return;
    }
    else if (m_squadOrder == SQUAD_ORDER_MOVETO)
    {
        Vector wp = g_Bots.m_leaderWaypoint[m_squadLeaderIndex];
        if (wp.LengthSqr() < 1.0f)
        {
            m_squadOrder = SQUAD_ORDER_FOLLOW;
            return;
        }

        // Advance heading from leader pos to waypoint
        Vector toWp = wp - leaderPos; toWp.z = 0.0f;
        float baseYaw = (toWp.LengthSqr() > 1.0f)
            ? atan2f(toWp.y, toWp.x) * (180.0f / 3.14159265f)
            : m_squadFormationYaw;

        // 3-Point Triangular Perimeter: Slot 0 = -60 deg, Slot 1 = +60 deg, Slot 2 = 180 deg (Rear)
        float slotAng = baseYaw + (m_squadSlot == 0 ? -60.0f : (m_squadSlot == 1 ? 60.0f : 180.0f));
        float slotRad = slotAng * (3.14159265f / 180.0f);
        Vector slotPos = wp + Vector(cosf(slotRad) * 70.0f, sinf(slotRad) * 70.0f, 0.0f);
        float distToSlot = xyd(pos, slotPos);

        if (distToSlot > 40.0f)
        {
            MoveToward(slotPos, fwd, side, buttons, view);
            if (distToSlot > 160.0f && fwd > 100.0f && !m_traversingLadder && (!m_doorwayRunway.active || m_doorwayRunway.doorClear))
                buttons |= INS_IN_SPRINT;
        }
        else
        {
            // Arrived at defensive slot: latch into sector hold
            fwd = 0.0f; side = 0.0f;
            if (!m_enemyIndex)
            {
                view.y = fmodf(slotAng + 360.0f, 360.0f);
                view.x = 0.0f;
                m_curAngles = view;
            }

            // Low-cover ducking check
            if (m_chosenCoverIsLow || (insbot_cover_v2.GetBool() && distToSlot < 25.0f))
            {
                buttons |= IN_DUCK;
            }
        }
        return;
    }
    else if (m_squadOrder == SQUAD_ORDER_SUPPRESS)
    {
        if (now - m_squadOrderTime > 5.0f)
        {
            m_squadOrder = SQUAD_ORDER_FOLLOW;
            // Fix C-note: downgrade the roster-level order too, so a respawned
            // follower recalled via RecallFollower doesn't inherit a stale SUPPRESS.
            if (m_squadLeaderIndex > 0 && m_squadLeaderIndex <= 64
                && g_Bots.m_leaderSquadOrder[m_squadLeaderIndex] == SQUAD_ORDER_SUPPRESS)
            {
                g_Bots.m_leaderSquadOrder[m_squadLeaderIndex] = SQUAD_ORDER_FOLLOW;
                // Fix L-1: the covering-fire window is over — drop the marker so
                // a later coord-less SUPPRESS can't silently reuse a stale point.
                g_Bots.m_leaderWaypoint[m_squadLeaderIndex] = Vector(0, 0, 0);
            }
        }
        else
        {
            // Fix 2a: no crouch lock during covering fire — bot stays mobile
            // Aim toward leader waypoint if set, else leader's view direction.
            // Fix M-2: never tear the view off a live target — local combat
            // survival outranks static suppression.
            if (!m_enemyIndex &&
                (g_Bots.m_leaderWaypoint[m_squadLeaderIndex].x != 0.0f 
                || g_Bots.m_leaderWaypoint[m_squadLeaderIndex].y != 0.0f))
            {
                Vector wp = g_Bots.m_leaderWaypoint[m_squadLeaderIndex];
                float wpYaw = atan2f(wp.y - pos.y, wp.x - pos.x) * (180.0f / 3.14159265f);
                // Fix C6: negate pitch (Source: positive pitch = looking down) and use
                // correct eye height (~64u above origin) for the vertical delta.
                float wpPitch = atan2f(wp.z - (pos.z + 64.0f),
                    sqrtf((wp.x-pos.x)*(wp.x-pos.x) + (wp.y-pos.y)*(wp.y-pos.y))) * (180.0f / 3.14159265f);
                view.y = wpYaw;
                view.x = -wpPitch;
                m_curAngles = view;

                // Occlusion probe (throttled): a static marker has no movement
                // uncertainty, so one result per ~0.75s is sufficient. Blocked
                // probe means the marker is behind a wall — reposition until the
                // eye-line clears instead of dumping the magazine into cover.
                if (now >= m_nextSuppressLosCheck)
                {
                    bool wasClear = m_suppressLosClear;
                    m_suppressLosClear = CanShootMarker(wp);
                    m_nextSuppressLosCheck = now + 0.75f;
                    if (!m_suppressLosClear)
                    {
                        m_haveSuppressRepos = false;   // re-solve for the new occlusion
                        if (insbot_debug.GetBool() && wasClear)
                            BotDbg("[insbots] %s coverfire: marker occluded, repositioning\n", m_name);
                    }
                    else if (insbot_debug.GetBool() && !wasClear)
                        BotDbg("[insbots] %s coverfire: marker LOS clear, firing\n", m_name);
                }

                if (!m_suppressLosClear)
                {
                    // Re-select a reposition goal on a slow cadence: micro-step
                    // ring around the marker axis first, then a small nav-area
                    // fallback. The goal must have a clear eye->marker ray and
                    // be standable; cheap validation only, ~1.5s throttle.
                    if (now >= m_nextSuppressRepos || !m_haveSuppressRepos)
                    {
                        m_nextSuppressRepos = now + 1.5f;
                        m_haveSuppressRepos = false;

                        Vector toWp = wp - pos; toWp.z = 0.0f;
                        float wlen = toWp.Length();
                        if (wlen > 1.0f)
                        {
                            toWp *= 1.0f / wlen;
                            Vector right(-toWp.y, toWp.x, 0.0f);
                            const float dists[] = { 60.0f, 100.0f };
                            const float sgn[]   = { 1.0f, -1.0f };
                            float bestScore = -1.0f;
                            for (int di = 0; di < 2 && !m_haveSuppressRepos; di++)
                                for (int si2 = 0; si2 < 2 && !m_haveSuppressRepos; si2++)
                                    for (int bk = 0; bk < 2 && !m_haveSuppressRepos; bk++)
                                    {
                                        Vector cand = pos + right * (sgn[si2] * dists[di])
                                                        + toWp * (bk ? -dists[di] : 0.0f);
                                        Vector floorStart(cand.x, cand.y, cand.z + 4.0f);
                                        Vector floorEnd  = floorStart - Vector(0, 0, 64.0f);
                                        Ray_t floorRay;
                                        floorRay.Init(floorStart, floorEnd);
                                        CTraceFilterWorldOnly floorFilter;
                                        trace_t floorTr;
                                        enginetrace->TraceRay(floorRay, MASK_PLAYERSOLID_BRUSHONLY,
                                                              &floorFilter, &floorTr);
                                        if (floorTr.fraction >= 1.0f) continue;   // no floor: off ledge
                                        cand = floorTr.endpos + Vector(0, 0, 2.0f);
                                        if (!CanShootMarker(wp)) continue;
                                        float score = 200.0f - xyd(cand, pos);
                                        if (score > bestScore)
                                        {
                                            bestScore = score;
                                            m_suppressReposPoint = cand;
                                            m_haveSuppressRepos = true;
                                        }
                                    }
                            if (!m_haveSuppressRepos && g_NavMesh.IsLoaded())
                            {
                                // Nav fallback: nearest standable area center with a
                                // clear eye->marker ray (bounded radius, few traces).
                                int best = -1;
                                float bestD = 1e30f;
                                float searchR = insbot_cover_search_radius.GetFloat();
                                if (searchR < 100.0f) searchR = 100.0f;
                                for (int ai = 0; ai < g_NavMesh.Count(); ai++)
                                {
                                    const NavAreaData *na = g_NavMesh.Area(ai);
                                    if (!na) continue;
                                    float dz = pos.z - na->center.z;
                                    if (dz > 64.0f || dz < -64.0f) continue;
                                    float d = xyd(na->center, pos);
                                    if (d < 40.0f || d > searchR) continue;
                                    if (!CanShootMarker(na->center)) continue;
                                    if (d < bestD) { bestD = d; best = ai; }
                                }
                                if (best >= 0)
                                {
                                    m_suppressReposPoint = g_NavMesh.Area(best)->center;
                                    m_haveSuppressRepos = true;
                                }
                            }
                        }
                    }

                    if (m_haveSuppressRepos)
                    {
                        if (xyd(pos, m_suppressReposPoint) > 24.0f)
                        {
                            MoveToward(m_suppressReposPoint, fwd, side, buttons, view);
                            return;   // keep reposition movement — do NOT fall through
                                      // to the branch trailing fwd=0/side=0 stop
                        }
                        m_haveSuppressRepos = false;   // arrived; next probe re-checks
                    }
                    // Occluded with no goal found yet: hold position, stay aimed
                    // at the marker, wait for re-selection / probe clear.
                }
                else
                {
                    // Fix L-2 / slow-fire: suppression is now driven by the NATIVE
                    // combat fire-control block (gated on m_suppressFire with a
                    // synthesized marker target). Do NOT inject IN_ATTACK here.
                    m_suppressFire = !m_onSidearm;
                }
            }
            else if (!m_enemyIndex)
            {
                view.y = leaderYaw;
                view.x = leaderAngles.x;
                m_curAngles = view;
                if (fmodf(now, 0.4f) < 0.2f && !m_onSidearm)
                    buttons |= IN_ATTACK;
            }
            fwd = 0.0f; side = 0.0f;
            return;
        }
    }

        // Fix 3B: anti-boxing moved to ThinkAlive as an early high-priority check.
    // The in-formation version was starved by stuck recovery / choke queues and
    // never ran in congested play. See the antibox block near the top of ThinkAlive.

    // 4. SQUAD_ORDER_FOLLOW: 3-Point Tactical Wedge Formation
    if (distToLeader < 140.0f)
    {
        // Change 5: Anti-boxing sidestep — yield to moving leader
        if (leaderSpeed > 30.0f)
        {
            float mvRad = atan2f(leaderVel.y, leaderVel.x);
            Vector mvDir(cosf(mvRad), sinf(mvRad), 0.0f);
            Vector toBot = pos - leaderPos; toBot.z = 0;
            float toBotLen = toBot.Length();
            if (toBotLen > 1.0f) toBot /= toBotLen;
            float dot = toBot.x * mvDir.x + toBot.y * mvDir.y;
            if (dot > 0.3f)
            {
                float sideSign = (toBot.x * (-sinf(mvRad)) + toBot.y * cosf(mvRad)) > 0.0f ? 1.0f : -1.0f;
                float sideYaw = (mvRad * 180.0f / 3.14159265f) + (sideSign > 0.0f ? 90.0f : -90.0f);
                float sideRad = sideYaw * (3.14159265f / 180.0f);
                Vector sideDir(cosf(sideRad), sinf(sideRad), 0.0f);
                Vector sidestepTarget = pos + sideDir * 60.0f;
                if (SideStepOpen(sidestepTarget))
                {
                    MoveToward(sidestepTarget, fwd, side, buttons, view);
                }
                else
                {
                    fwd = 0.0f; side = 0.0f;
                    if (!m_enemyIndex) { view.y = guardYaw; view.x = 0.0f; m_curAngles = view; }
                }
            }
            else
            {
                fwd = 0.0f; side = 0.0f;
                if (!m_enemyIndex) { view.y = guardYaw; view.x = 0.0f; m_curAngles = view; }
            }
        }
        else
        {
            fwd = 0.0f; side = 0.0f;
            if (!m_enemyIndex) { view.y = guardYaw; view.x = 0.0f; m_curAngles = view; }
        }
    }
    else if (distToSlot > 36.0f)
    {
        MoveToward(targetSlot, fwd, side, buttons, view);

        if (!m_enemyIndex && !m_traversingLadder && m_ladderPhase == LADDER_PHASE_NONE
            && !DoorAhead(view.y) && distToSlot < 350.0f)
        {
            float mvDx = targetSlot.x - pos.x, mvDy = targetSlot.y - pos.y;
            float mvYaw = m_haveLastNavMoveYaw ? m_lastNavMoveYaw : (atan2f(mvDy, mvDx) * (180.0f / 3.14159265f));
            float moveSpeed = sqrtf(fwd * fwd + side * side);
            if (moveSpeed > 10.0f)
            {
                view.y = guardYaw;
                view.x = 0.0f;
                m_curAngles = view;
                MoveAtWorldYaw(mvYaw, view.y, moveSpeed, fwd, side);
            }
        }

        if (distToSlot > (slotDist * 1.3f) && (fabsf(fwd) > 50.0f || fabsf(side) > 50.0f) && !m_traversingLadder && !(buttons & IN_DUCK) && (!m_doorwayRunway.active || m_doorwayRunway.doorClear))
            buttons |= INS_IN_SPRINT;
        else if (distToSlot < 120.0f && (fabsf(fwd) > 0.0f || fabsf(side) > 0.0f))
        {
            float scale = 0.65f + 0.35f * (distToSlot / 120.0f);
            fwd *= scale;
            side *= scale;
        }
    }
    else
    {
        fwd = 0.0f; side = 0.0f;
        if (!m_enemyIndex)
        {
            view.y = guardYaw;
            view.x = 0.0f;
            m_curAngles = view;
        }
    }
}

// radio callout helper: resolves this bot's faction and emits the line for `ev`
// (InsVoice_Say handles the toggles + anti-spam throttle, so call it freely).
void CInsBot::Say(int ev)
{
    if (!m_edict || !m_info) return;
    int t = m_info->GetTeamIndex();
    int fac = (t >= 1 && t <= 2) ? g_teamFaction[t] : FACTION_UNKNOWN;
    InsVoice_Say(m_edict, fac, ev, Now());
}

// RunMovePending - called from SetCommandClient (engine hook fired just before
// this bot's PhysicsSimulate). Submitting RunPlayerMove here ensures the engine
// sees CommandContextCount > 0, so it processes OUR command and SKIPS
// RunNullCommand. Without this, RunNullCommand would run after GameFrame,
// generate m_afButtonReleased=IN_ATTACK, and trigger auto-reload every tick
// for full-auto weapons.
void CInsBot::RunMovePending()
{
    if (!m_hasPending) return;
    RunMove(m_pendingFwd, m_pendingSide, m_pendingButtons, m_pendingView);
}

void CInsBot::RunMove(float fwd, float side, int buttons, const QAngle &view)
{
    static int s_cmdSeq = 1;
#ifdef _LINUX
    // CBasePlayer::UpdateButtonState reads/writes m_nButtons at 0x778.
    // This removes the broad live entity-memory probe from Linux builds.
    static int s_offset_nButtons = INS_LINUX_M_NBUTTONS;
#else
    static int s_offset_nButtons = -1;
#endif
    static int s_probeStage = 0;
    static int s_probeCandidates[100];
    static int s_numCandidates = 0;

    // Use highly unique combinations that the engine will not mask out. The
    // movement bits must use INS's custom layout: stock bit 8 is +prone here.
    const int PROBE_MASK_1 = 65536 | 131072 | INS_IN_FORWARD | INS_IN_MOVELEFT;
    const int PROBE_MASK_2 = 65536 | 262144 | INS_IN_BACK | INS_IN_MOVERIGHT;

    const WeaponProfile *moveWeapon = (const WeaponProfile *)m_wp;
    bool directLauncherAttack = moveWeapon && ((moveWeapon->combatClass == WC_RPG) || (moveWeapon->combatClass == WC_GRENADE_LAUNCHER))
                              && (buttons & IN_ATTACK);
    if (directLauncherAttack)
        buttons &= ~IN_ATTACK;

    // ===== IRONSIGHTS INPUT DRIVER =====
    // Engine facts (verified by disassembly of the shipped server binaries):
    //   * CWeaponINSBase::ItemPostFrame() calls ToggleIronsights() only when the
    //     button-18 RELEASED edge is set (player+0x780 bit 18). Holding the bit
    //     forever produces no edge, which is why the old continuous hold left
    //     ADS at ~6% and sometimes latched it ON after the bot stopped aiming.
    //   * CWeaponINSBase::CanTertiaryAttack() additionally requires the PRESSED
    //     edge (player+0x77C bit 18).
    //   * CINSPlayer::SetIronsightsState() sets bit 0 of GetPlayerFlags()
    //     (AddPlayerFlag(1) / RemovePlayerFlag(1)), and IsMoveFrozen() is
    //     (flags >> 1) & 1 -- so flag bit 0 is the shouldered (ADS) state.
    // Emit a genuine press frame followed by a release frame on every intent
    // transition, then re-pulse if the measured state still disagrees.
    if (insbot_ads_mode.GetBool())
    {
        static unsigned char s_adsPhase[64];      // 0=idle, 1=press queued, 2=release queued
        static unsigned char s_adsLastWant[64];
        static unsigned char s_adsInit[64];
        static float         s_adsNextRetry[64];
        static float         s_adsLastStep[64];

        int slot = (int)(this - g_Bots.m_bots);
        if (slot >= 0 && slot < 64)
        {
            bool wantAds = m_wantAds;

            IServerUnknown *u = m_edict ? m_edict->GetUnknown() : 0;
            char *pPlayer = u ? (char*)u->GetBaseEntity() : 0;
            // The engine-state check needs the audited offset; when it is
            // unavailable the change-driven pulse still drives the toggle.
            bool haveState = (pPlayer != 0 && s_offset_m_IronsightsState >= 0);
            bool isUp = haveState ? ((*(int*)(pPlayer + s_offset_m_IronsightsState) & 1) != 0) : false;

            if (!s_adsInit[slot])
            {
                s_adsInit[slot] = 1;
                s_adsLastWant[slot] = wantAds ? 1 : 0;
            }

            // An intent change only needs a toggle when the measured state
            // actually disagrees. Pulsing unconditionally would toggle the
            // sights ON when the raise had never succeeded and the bot then
            // stopped aiming -- a spurious raise that only the retry undid.
            bool stateDisagrees = !haveState || (wantAds != isUp);

            if (wantAds != (s_adsLastWant[slot] != 0))
            {
                s_adsLastWant[slot] = wantAds ? 1 : 0;
                if (stateDisagrees)
                {
                    s_adsPhase[slot] = 1;
                    s_adsNextRetry[slot] = gpGlobals->curtime + 0.30f;
                }
            }
            // The retry exists only to recover a raise the weapon refused while
            // it was mid-transition. It must stay gated on a READABLE state:
            // without the offset (e.g. the Windows build) it cannot tell whether
            // the toggle worked, and retrying blindly would flip the sights
            // on/off every interval.
            else if (haveState && wantAds != isUp && gpGlobals->curtime >= s_adsNextRetry[slot])
            {
                // The raise can be refused while the weapon is mid-transition
                // (m_iIdleState != 0); retry, but not so fast that the retry
                // toggles an already-correct state back off.
                s_adsPhase[slot] = 1;
                s_adsNextRetry[slot] = gpGlobals->curtime + (wantAds ? 0.30f : 0.15f);
            }

            // Advance the pulse at most once per simulation frame. RunMove is
            // reachable from several branches in ThinkAlive (all but one return
            // early), so this guard keeps a press and its release on separate
            // engine ticks -- they must not collapse into the same command.
            if (s_adsLastStep[slot] != gpGlobals->curtime)
            {
                s_adsLastStep[slot] = gpGlobals->curtime;
                switch (s_adsPhase[slot])
                {
                    case 1:
                        buttons |= INS_IN_ATTACK3;     // press frame
                        s_adsPhase[slot] = 2;
                        break;
                    case 2:
                        buttons &= ~INS_IN_ATTACK3;    // release frame -> engine toggles
                        s_adsPhase[slot] = 0;
                        break;
                    default:
                        buttons &= ~INS_IN_ATTACK3;
                        break;
                }
            }
        }
    }

    CBotCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.command_number = s_cmdSeq++;
    cmd.tick_count = gpGlobals ? gpGlobals->tickcount : 0;
    cmd.random_seed = rand();
    cmd.viewangles = view;
    cmd.forwardmove = fwd;
    cmd.sidemove = side;
    cmd.upmove = 0;
    cmd.impulse = 0;
    cmd.hasbeenpredicted = false;

    bool probing = false;
    if (s_offset_nButtons == -1 && m_edict && !m_edict->IsFree() && m_edict->GetUnknown())
    {
        if (s_probeStage == 0) 
        {
            static int wait = 0;
            if (++wait > 30) // Wait for entity state to stabilize after joining
            {
                cmd.buttons = PROBE_MASK_1;
                s_probeStage = 1;
                probing = true;
            }
            else cmd.buttons = buttons;
        }
        else if (s_probeStage == 2)
        {
            cmd.buttons = PROBE_MASK_2;
            s_probeStage = 3;
            probing = true;
        }
        else cmd.buttons = buttons;
    }
    else
    {
        cmd.buttons = buttons;
    }

    // Removed duplicate RunPlayerMove here to avoid 3x simulation per tick.

    if (m_edict && !m_edict->IsFree() && m_edict->GetUnknown())
    {
        char *pPlayer = (char*)m_edict->GetUnknown()->GetBaseEntity();
        if (pPlayer)
        {
            if (probing)
            {
                int *pInts = (int*)pPlayer;
                if (s_probeStage == 1)
                {
                    // Safe memory read using OS page checks
                    for (int i = 4; i < 4000; i++)
                    {
                        // If we cross a page boundary, ensure it's readable
                        if ( (((unsigned int)&pInts[i]) & 0xFFF) == 0 )
                        {
                            // Poor man's page check: if the first byte of the page is unreadable, 
                            // we would crash. But since we can't easily check without SEH/VirtualQuery,
                            // we'll just limit the search to 1200 ints (4800 bytes) to be safer.
                            // Actually, let's just break at 1500 to avoid risking multiple pages.
                        }
                        if (i > 1500) break;
                        
                        if (pInts[i] == PROBE_MASK_1 && s_numCandidates < 100)
                            s_probeCandidates[s_numCandidates++] = i * (int)sizeof(int);
                    }
                    s_probeStage = 2;
                }
                else if (s_probeStage == 3)
                {
                    for (int i = 0; i < s_numCandidates; i++)
                    {
                        int offset = s_probeCandidates[i];
                        if (*(int*)(pPlayer + offset) == PROBE_MASK_2)
                        {
                            s_offset_nButtons = offset;
                            BotDbg("[insbots] m_nButtons EXACT offset found: 0x%X (%d)\n", offset, offset);
                            break;
                        }
                    }
                    if (s_offset_nButtons == -1)
                    {
                        BotDbg("[insbots] WARNING: m_nButtons offset NOT FOUND! Full-auto will bug out.\n");
                        // Failed to find it, reset and try again in 100 ticks
                        s_probeStage = 0;
                        s_numCandidates = 0;
                    }
                }
                if (s_numCandidates == 1)
                {
                    s_offset_nButtons = s_probeCandidates[0];
                    BotDbg("[insbots] found m_nButtons reliably at offset %d\n", s_offset_nButtons);
                }
            }

            if (s_offset_nButtons > 0)
            {
                *(int*)(pPlayer + s_offset_nButtons) = m_lastButtons;
            }
            if (s_offset_angEyeAngles0 >= 0 && s_offset_angEyeAngles1 >= 0)
            {
                *(float*)(pPlayer + s_offset_angEyeAngles0) = view.x;
                *(float*)(pPlayer + s_offset_angEyeAngles1) = view.y;
            }
            if (s_offset_flNextAttack >= 0 && m_flOldNextAttack > 0.0f)
            {
                *(float*)(pPlayer + s_offset_flNextAttack) = m_flOldNextAttack;
            }

#ifdef _LINUX
            const int INS_MUZZLE_ORIGIN_OFFSET = INS_LINUX_MUZZLE_ORIGIN;
            const int INS_MUZZLE_ANGLE_OFFSET  = INS_LINUX_MUZZLE_ANGLES;
#else
            const int INS_MUZZLE_ORIGIN_OFFSET = 0x10A8;
            const int INS_MUZZLE_ANGLE_OFFSET  = 0x10B4;
#endif
            const float degToRad = 3.14159265f / 180.0f;
            float p_rad = view.x * degToRad;
            float y_rad = view.y * degToRad;
            Vector fwdVec(cosf(p_rad) * cosf(y_rad),
                          cosf(p_rad) * sinf(y_rad),
                         -sinf(p_rad));
            Vector muzzleVec = EyePos() + fwdVec * 14.0f;

            *(float*)(pPlayer + INS_MUZZLE_ORIGIN_OFFSET + 0) = muzzleVec.x;
            *(float*)(pPlayer + INS_MUZZLE_ORIGIN_OFFSET + 4) = muzzleVec.y;
            *(float*)(pPlayer + INS_MUZZLE_ORIGIN_OFFSET + 8) = muzzleVec.z;
            *(float*)(pPlayer + INS_MUZZLE_ANGLE_OFFSET + 0)  = view.x;
            *(float*)(pPlayer + INS_MUZZLE_ANGLE_OFFSET + 4)  = view.y;
            *(float*)(pPlayer + INS_MUZZLE_ANGLE_OFFSET + 8)  = view.z;

            m_ctrl->RunPlayerMove(&cmd);
            if (directLauncherAttack)
            {
                bool launchOk = FireLauncherDirect(pPlayer, view);
                if (launchOk && moveWeapon)
                {
                    m_reloadMags[moveWeapon->combatClass]++;
                    if (moveWeapon->combatClass == WC_RPG)
                    {
                        extern CfgVar insbot_rpg_cooldown;
                        m_nextRpg = gpGlobals ? gpGlobals->curtime + insbot_rpg_cooldown.GetFloat() : 0.0f;
                    }
                    else if (moveWeapon->combatClass == WC_GRENADE_LAUNCHER)
                    {
                        extern CfgVar insbot_m203_cooldown;
                        m_nextM203 = gpGlobals ? gpGlobals->curtime + insbot_m203_cooldown.GetFloat() : 0.0f;
                    }
                }
                else
                {
                    if (moveWeapon && moveWeapon->combatClass == WC_RPG)
                        m_nextRpg = gpGlobals ? gpGlobals->curtime + 0.5f : 0.0f;
                    else if (moveWeapon && moveWeapon->combatClass == WC_GRENADE_LAUNCHER)
                        m_nextM203 = gpGlobals ? gpGlobals->curtime + 0.5f : 0.0f;
                    BotDbg("[insbots] %s launcher direct launch failed; attack suppressed\n", m_name);
                }
            }
            m_ctrl->PostClientMessagesSent();

            m_lastButtons = buttons;

            if (s_offset_nButtons > 0)
            {
                *(int*)(pPlayer + s_offset_nButtons) = 0;
            }
            if (s_offset_flNextAttack >= 0 && m_nadeState == 0)
            {
                m_flOldNextAttack = *(float*)(pPlayer + s_offset_flNextAttack);
                *(float*)(pPlayer + s_offset_flNextAttack) = gpGlobals->curtime + 999.0f;
            }
        }
        else
        {
            m_ctrl->RunPlayerMove(&cmd);
            m_ctrl->PostClientMessagesSent();
            m_lastButtons = buttons;
        }
    }
    else
    {
        m_ctrl->RunPlayerMove(&cmd);
        m_ctrl->PostClientMessagesSent();
        m_lastButtons = buttons;
    }
}

void CInsBot::Think(bool simulating)
{
    INSPROF_SCOPE(INSPROF_THINK);
    if (!m_inUse || !m_edict || m_edict->IsFree() || !m_info || !m_ctrl)
        return;
    float now = Now();

    Vector pos = m_info->GetAbsOrigin();
    if (m_notedSpawn && m_vecLastOrigin.x != 0.0f)
    {
        float dx = pos.x - m_vecLastOrigin.x;
        float dy = pos.y - m_vecLastOrigin.y;
        float dz = pos.z - m_vecLastOrigin.z;
        if (dx*dx + dy*dy + dz*dz > 300.0f * 300.0f)
        {
            // teleported (round restart or first spawn frame jump)
            memset(m_roundsFired, 0, sizeof(m_roundsFired));
            m_fireAccum = 0.0f;
            memset(m_reloadMags, 0, sizeof(m_reloadMags));
            m_stuckSince = 0.0f;
        }
    }
    m_vecLastOrigin = pos;


#ifndef _LINUX
    // periodically decline squad-leader/commander promotions so a bot never gets
    // stuck sitting in the promotion prompt instead of playing
    if (now >= m_nextDecline)
    {
        m_nextDecline = now + 4.0f;
        helpers->ClientCommand(m_edict, "declinepromo");
    }
#endif

    switch (m_state)
    {
    case BOT_JOINING:
        ThinkJoin();
        break;

    case BOT_DEAD:
    {
        ResetBodyNoCollideRecovery();
        EndSpawnNoCollide(true);
        // ThinkAlive() only runs while BOT_ALIVE, so m_wantAds would otherwise
        // keep the value it held on the frame the bot died and the ironsights
        // driver would never lower the sights. Drop the request here; the
        // RunMove below carries the release edge that lowers them.
        m_wantAds = false;
        // optional commands while dead (e.g. re-pick class for wave respawn)
        if (now >= m_nextJoinCmdTime)
        {
            m_nextJoinCmdTime = now + 5.0f;
            const char *dc = insbot_dead_cmds.GetString();
            if (dc && dc[0])
            {
                if (CommandContainsToken(dc, "creinforce"))
                    SendCustomizationBeforeSpawn();
                helpers->ClientCommand(m_edict, dc);
            }
        }
        // click through death cam / respawn prompts now and then
        if (simulating && m_ctrl)
        {
            int btn = (((int)(now * 2.0f)) & 3) == 0 ? IN_ATTACK : 0;
            RunMove(0, 0, btn, m_curAngles);
        }
        if (!m_info->IsDead() && m_info->GetTeamIndex() > 0)
        {
            g_Bots.RecallFollower(this); // restore fireteam membership if pending
            m_state = BOT_ALIVE;
            m_classSetupSent = false;
            m_nextReinforcePulse = 0.0f;
            m_customizationPending = false;
            PruneDuplicateWeapons();
            m_haveLastCustomization = true;
            m_lastCustomizationFaction = m_customizationFaction;
            m_lastCustomizationSkin = m_customizationSkin;
            m_lastCustomizationBody = m_customizationBody;
            if (m_customizationWeapon >= 0)
            {
                m_haveLastCustomizationWeapon = true;
                m_lastCustomizationFaction = m_customizationFaction;
                m_lastCustomizationRoleId = m_setupRoleId;
                m_lastCustomizationWeapon = m_customizationWeapon;
            }
            m_customizationPending = false;
            m_curAngles = m_info->GetAbsAngles();
            ResetDoorwayRunway();

            m_pathLen = 0;
            m_pathResult = BOT_PATH_NONE;
    m_pathContinues = false; m_goalArea = -1; m_enemyIndex = 0;
            m_spawnStandTicks = 66;
        }
        break;
    }

    case BOT_ALIVE:
        if (simulating)
            ThinkAlive();
        break;
    }
}
