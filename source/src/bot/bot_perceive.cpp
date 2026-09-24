// ============================================================================
// bot_perceive.cpp - hearing, shared intel, smoke tracking, enemy acquisition.
//
// The perception layer. Bots write what they see into the shared per-team
// intel table; teammates read it to react. Footstep speed is sampled once per
// frame here (IPlayerInfo exposes no velocity getter) and every bot reads the
// cached value.
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
extern CfgVar                insbot_hear;
extern CfgVar                insbot_hear_range;
extern CfgVar                insbot_hear_speed;
extern CfgVar                insbot_hear_timeout;
extern CfgVar                insbot_hear_cooldown;
extern CfgVar                insbot_detect;
extern CfgVar                insbot_fov;
extern CfgVar                insbot_vis_range;
extern CfgVar                insbot_debug;
extern CfgVar                insbot_close_range;
extern CfgVar                insbot_crouch_eye_height;
extern CfgVar                insbot_objectives;
extern CfgVar                insbot_intel_time;

Vector g_SmokeGrenades[64];
int g_NumSmokeGrenades = 0;

TeamIntel g_Intel[3];

void UpdateSmokeGrenades()
{
    g_NumSmokeGrenades = 0;
    for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (!e || e->IsFree()) continue;
        
        IServerUnknown *unk = e->GetUnknown();
        if (!unk) continue;
        
        IServerNetworkable *net = unk->GetNetworkable();
        if (!net) continue;
        
        const char *cls = net->GetClassName();
        if (cls && (strstr(cls, "Smoke") || strstr(cls, "smoke") || strstr(cls, "SmokeGrenade")))
        {
            ICollideable *col = unk->GetCollideable();
            if (col && g_NumSmokeGrenades < 64)
            {
                g_SmokeGrenades[g_NumSmokeGrenades++] = col->GetCollisionOrigin();
            }
        }
    }
}

void IntelReport(int team, int enemyIdx, const Vector &pos, float now)
{
    if (team < 1 || team > 2 || enemyIdx < 1 || enemyIdx > 65) return;
    g_Intel[team].enemy[enemyIdx].pos = pos;
    g_Intel[team].enemy[enemyIdx].time = now;
    g_Intel[team].enemy[enemyIdx].valid = true;
}

bool IntelEnemyIsLive(int team, int enemyIdx)
{
    if (team < 1 || team > 2 || enemyIdx < 1 || enemyIdx > 65
        || !engine || !playerinfomanager)
        return false;
    edict_t *enemyEdict = engine->PEntityOfEntIndex(enemyIdx);
    IPlayerInfo *enemyInfo = (enemyEdict && !enemyEdict->IsFree())
                           ? playerinfomanager->GetPlayerInfo(enemyEdict) : 0;
    return enemyInfo && enemyInfo->IsConnected() && !enemyInfo->IsDead()
        && !enemyInfo->IsObserver() && !enemyInfo->IsHLTV()
        && enemyInfo->GetTeamIndex() > 0 && enemyInfo->GetTeamIndex() != team;
}

// freshest actionable sighting for a team; returns enemy index or 0
int IntelFreshest(int team, float now, float maxAge, const Vector &listenerPos,
                         float maxRange, Vector &outPos)
{
    if (team < 1 || team > 2) return 0;
    int best = 0; float bestT = -1;
    for (int i = 1; i <= 65; i++)
    {
        Sighting &s = g_Intel[team].enemy[i];
        if (!s.valid) continue;
        // A last-known position is useful only while the referenced player is
        // still a living enemy.  Without this validation, a confirmed kill
        // remained actionable until intel_time expired and pulled teammates
        // toward the corpse's last position.
        if (!IntelEnemyIsLive(team, i))
        {
            s.valid = false;
            continue;
        }
        if (now - s.time > maxAge) { s.valid = false; continue; }
        if (maxRange > 0.0f)
        {
            float dx = s.pos.x - listenerPos.x, dy = s.pos.y - listenerPos.y;
            if (dx * dx + dy * dy > maxRange * maxRange) continue;
        }
        if (s.time > bestT) { bestT = s.time; best = i; outPos = s.pos; }
    }
    return best;
}

void IntelReset() { memset(g_Intel, 0, sizeof(g_Intel)); }

// ---------------- hearing / footstep investigation ----------------
// This SDK's IPlayerInfo exposes no velocity getter, so a moving enemy's
// horizontal speed is estimated once per player per server frame. Every bot
// then reads the same cached value; listeners must not consume one another's
// origin samples.
static Vector s_hearLastOrigin[66];
static float  s_hearLastTime[66];
float  s_hearSpeed[66];
static int    s_hearUserId[66];
static bool   s_hearHasLast[66];
static float  s_hearFrameTime = -1.0f;
// Door traces need to ignore every client entity. Build this list alongside
// the once-per-frame hearing sample rather than scanning all client edicts for
// every bot's DoorAhead call (which becomes very expensive at a 30-bot quota).
IHandleEntity *s_tracePlayerHandles[66];
int s_tracePlayerHandleCount = 0;

static void HearingClearSlot(int idx)
{
    if (idx < 1 || idx > 65) return;
    s_hearLastOrigin[idx] = Vector(0, 0, 0);
    s_hearLastTime[idx] = 0.0f;
    s_hearSpeed[idx] = 0.0f;
    s_hearUserId[idx] = 0;
    s_hearHasLast[idx] = false;
}

void HearingReset()
{
    for (int i = 1; i <= 65; i++) HearingClearSlot(i);
    s_hearFrameTime = -1.0f;
    s_tracePlayerHandleCount = 0;
}

void HearingFrameUpdate()
{
    INSPROF_SCOPE(INSPROF_HEARING);
    if (!gpGlobals || !engine || !playerinfomanager) return;

    float now = gpGlobals->curtime;
    if (s_hearFrameTime >= 0.0f)
    {
        if (now < s_hearFrameTime) HearingReset();       // map/time restart
        else if (now == s_hearFrameTime) return;          // duplicate GameFrame
    }
    s_hearFrameTime = now;

    int maxClients = gpGlobals->maxClients;
    if (maxClients > 65) maxClients = 65;
    s_tracePlayerHandleCount = 0;
    for (int i = 1; i <= maxClients; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (e && !e->IsFree())
        {
            IServerUnknown *unk = e->GetUnknown();
            if (unk && s_tracePlayerHandleCount < 66)
                s_tracePlayerHandles[s_tracePlayerHandleCount++] = (IHandleEntity *)unk;
        }
        IPlayerInfo *pi = (e && !e->IsFree()) ? playerinfomanager->GetPlayerInfo(e) : 0;
        if (!pi || !pi->IsConnected() || !pi->IsPlayer() || pi->IsHLTV()
            || pi->IsObserver() || pi->IsDead() || pi->GetTeamIndex() <= 0
            || pi->GetHealth() <= 0)
        {
            HearingClearSlot(i);
            continue;
        }

        Vector p = pi->GetAbsOrigin();
        if (p.x * p.x + p.y * p.y + p.z * p.z < 1.0f)
        {
            HearingClearSlot(i);   // not positioned yet after spawn/map load
            continue;
        }

        int userId = pi->GetUserID();
        if (!s_hearHasLast[i] || s_hearUserId[i] != userId)
        {
            s_hearLastOrigin[i] = p;
            s_hearLastTime[i] = now;
            s_hearSpeed[i] = 0.0f;
            s_hearUserId[i] = userId;
            s_hearHasLast[i] = true;
            continue;
        }

        float dt = now - s_hearLastTime[i];
        if (dt <= 0.0f)
        {
            s_hearLastOrigin[i] = p;
            s_hearLastTime[i] = now;
            s_hearSpeed[i] = 0.0f;
            continue;
        }

        float dx = p.x - s_hearLastOrigin[i].x;
        float dy = p.y - s_hearLastOrigin[i].y;
        float dist = sqrtf(dx * dx + dy * dy);
        s_hearSpeed[i] = (dist > 400.0f) ? 0.0f : dist / dt;
        s_hearLastOrigin[i] = p;
        s_hearLastTime[i] = now;
    }

    for (int i = maxClients + 1; i <= 65; i++) HearingClearSlot(i);
}

// Find the nearest audible enemy footstep source; returns true and outPos if any.
bool CInsBot::ScanHearableEnemy(const Vector &pos, Vector &outPos)
{
    if (!insbot_hear.GetBool()) return false;
    if (!insbot_detect.GetBool()) return false;   // detection disabled
    float range = insbot_hear_range.GetFloat();
    float minSpeed = insbot_hear_speed.GetFloat();

    int best = 0;
    float bestDistSq = range * range;

    for (int i = 1; i <= gpGlobals->maxClients; i++)
    {
        if (i > 65 || !s_hearHasLast[i]) continue;
        if (!ValidEnemy(i)) continue;
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (!e || e->IsFree()) continue;
        IPlayerInfo *pi = playerinfomanager->GetPlayerInfo(e);
        if (!pi) continue;

        Vector p = pi->GetAbsOrigin();
        float dx = p.x - pos.x, dy = p.y - pos.y, dz = p.z - pos.z;
        float distSq = dx * dx + dy * dy + dz * dz;
        if (distSq > range * range) continue;

        float speed = s_hearSpeed[i];
        if (speed < minSpeed) continue;

        // walking is only audible up close; sprinting carries further
        float effRange = range * (0.35f + 0.65f * (speed < 300.0f ? speed / 300.0f : 1.0f));
        if (distSq > effRange * effRange) continue;

        if (distSq < bestDistSq)
        {
            bestDistSq = distSq;
            best = i;
            outPos = p;
        }
    }
    return best != 0;
}

// Investigate the last heard footstep origin; returns true if movement was
// consumed this frame. Only runs while not actively engaging a target.
bool CInsBot::UpdateHearing(float now, const Vector &pos, float &fwd, float &side, int &buttons, QAngle &view)
{
    INSPROF_SCOPE(INSPROF_UPDATEHEARING);
    if (!insbot_hear.GetBool() || !insbot_detect.GetBool())
    {
        m_hearUntil = 0; m_nextHearTime = 0;   // detection disabled: no investigations
        return false;
    }
    if (m_enemyIndex) return false;   // actively engaging: combat path drives us

    if (m_hearUntil > now)
    {
        // refresh the source while it is still audible, otherwise walk to the
        // last known origin
        Vector live;
        if (ScanHearableEnemy(pos, live))
        {
            m_hearTarget = live;
            m_hearUntil = now + insbot_hear_timeout.GetFloat();
        }
        Vector hd = m_hearTarget - pos;
        if (sqrtf(hd.x * hd.x + hd.y * hd.y) < 120.0f)
        {
            m_hearUntil = 0;          // arrived: stop the hunt, sweep visually
            return false;
        }
        MoveToward(m_hearTarget, fwd, side, buttons, view);
        return true;
    }

    if (now < m_nextHearTime) return false;

    Vector origin;
    if (!ScanHearableEnemy(pos, origin)) return false;

    m_hearTarget = origin;
    m_hearUntil = now + insbot_hear_timeout.GetFloat();
    m_nextHearTime = now + insbot_hear_cooldown.GetFloat();
    MoveToward(origin, fwd, side, buttons, view);
    return true;
}

// Build a small set of points inside the target's current player hull. The
// old single chest point could be below the top of a low wall when the ray
// descended from the bot's eye over a long distance. Keep the chest first so
// open-space aim remains unchanged, then try the upper torso and head for
// targets that are visible over partial cover. A live maxs height keeps the
// points honest when the enemy is crouched or prone.
static int BuildEnemyTargetPoints(edict_t *target, const Vector &origin,
                                  Vector *points, int maxPoints)
{
    if (!points || maxPoints <= 0) return 0;

    float top = 72.0f; // standing INS player hull height
    if (target && playerinfomanager)
    {
        IPlayerInfo *pi = playerinfomanager->GetPlayerInfo(target);
        if (pi)
        {
            float h = pi->GetPlayerMaxs().z;
            if (h >= 16.0f && h <= 96.0f)
                top = h;
        }
    }

    float heights[3];
    heights[0] = top * 0.625f; // chest: 45 on a standing 72-unit hull
    heights[1] = top * 0.80f;  // upper torso/shoulders
    heights[2] = top - 8.0f;   // head, inside the current hull

    int count = 0;
    for (int i = 0; i < 3 && count < maxPoints; i++)
    {
        float h = heights[i];
        if (h < 8.0f || h > top) continue;

        bool duplicate = false;
        for (int j = 0; j < count; j++)
        {
            if (fabsf(points[j].z - (origin.z + h)) < 1.0f)
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        points[count] = origin;
        points[count].z += h;
        count++;
    }
    return count;
}

bool CInsBot::IsVisible(const Vector &from, const Vector &to, edict_t *target) const
{
    Ray_t ray;
    ray.Init(from, to);
    CTraceFilterSkipSelf filter(EdictHandleEntity(m_edict));
    trace_t tr;
    // Glass uses CONTENTS_WINDOW. Humans can visually acquire targets through
    // it, so omit that content from the bot sight trace while retaining solid,
    // moveable and debris blockers. The actual weapon trace still belongs to
    // the game, allowing breakable glass to absorb/break from the first shots.
    const unsigned int sightMask = MASK_SHOT & ~(CONTENTS_HITBOX | CONTENTS_WINDOW);
    enginetrace->TraceRay(ray, sightMask, &filter, &tr);
    if (tr.fraction >= 0.98f)
        return true;
    if (target)
    {
        IServerUnknown *unk = target->GetUnknown();
        if (unk && tr.m_pEnt == unk->GetBaseEntity())
            return true;
    }
    return false;
}

bool CInsBot::FindVisibleTargetPoint(edict_t *target, const Vector &origin,
                                     Vector &out) const
{
    Vector points[3];
    int count = BuildEnemyTargetPoints(target, origin, points, 3);
    Vector from = EyePos();
    for (int i = 0; i < count; i++)
    {
        if (IsVisible(from, points[i], target))
        {
            out = points[i];
            return true;
        }
    }
    return false;
}

// Immediate this-frame LOS gate for firing: trace the selected firing stance
// from the bot's eye approximation to targetPt right now.
bool CInsBot::LineGoesThroughSmoke(const Vector &from, const Vector &to) const
{
    return ::LineGoesThroughSmoke( from, to );
}

// Free-function form of the same test, for consumers that are not a CInsBot
// (the Humvee auto-turret). The member above delegates here so there is exactly
// one 140u threshold and one loop in the binary.
bool LineGoesThroughSmoke(const Vector &from, const Vector &to)
{
    for (int k = 0; k < g_NumSmokeGrenades; k++)
    {
        float distToSmoke = DistanceToLineSegment(g_SmokeGrenades[k], from, to);
        if (distToSmoke < 140.0f) return true;
    }
    return false;
}

bool CInsBot::IsTeammateInLineOfFire(const Vector &from, const Vector &to) const
{
    Ray_t ray;
    ray.Init(from, to);
    CTraceFilterSkipSelf filter(EdictHandleEntity(m_edict));
    trace_t tr;
    enginetrace->TraceRay(ray, MASK_SHOT, &filter, &tr);
    if (tr.fraction < 1.0f && tr.m_pEnt)
    {
        IServerUnknown *unk = (IServerUnknown *)tr.m_pEnt;
        if (unk) {
            IServerNetworkable *net = unk->GetNetworkable();
            if (net) {
                edict_t *e = net->GetEdict();
                if (e && !e->IsFree()) {
                    int index = engine->IndexOfEdict(e);
                    if (index >= 1 && index <= gpGlobals->maxClients) {
                        IPlayerInfo *pi = playerinfomanager->GetPlayerInfo(e);
                        if (pi && pi->GetTeamIndex() == m_info->GetTeamIndex()) {
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

bool CInsBot::CanShootNow(edict_t *enemy, const Vector &targetPt,
                          bool crouched) const
{
    Ray_t ray; ray.Init(FireEyePos(crouched), targetPt);
    CTraceFilterSkipSelf filter(EdictHandleEntity(m_edict));
    trace_t tr;
    // Match IsVisible: a target acquired through glass must pass this final
    // firing gate too. Engine ballistics still decide whether a shot breaks,
    // penetrates or stops on the particular glass surface.
    const unsigned int fireMask = MASK_SHOT & ~(CONTENTS_HITBOX | CONTENTS_WINDOW);
    enginetrace->TraceRay(ray, fireMask, &filter, &tr);
    if (tr.fraction >= 0.985f) return true;
    if (enemy)
    {
        IServerUnknown *unk = enemy->GetUnknown();
        if (unk && tr.m_pEnt == unk->GetBaseEntity()) return true;
    }
    return false;
}

bool CInsBot::FindShootableTargetPoint(edict_t *target, const Vector &origin,
                                       bool crouched, Vector &out) const
{
    Vector points[3];
    int count = BuildEnemyTargetPoints(target, origin, points, 3);
    for (int i = 0; i < count; i++)
    {
        if (CanShootNow(target, points[i], crouched))
        {
            out = points[i];
            return true;
        }
    }
    return false;
}

// Final stance-aware fire gate. Movement and combat can independently request
// IN_DUCK, so the random combat crouch check alone is not enough: validate the
// exact attack+duck command after all movement/recovery helpers have run.
void CInsBot::ValidateCrouchedFire(int &buttons, float now)
{
    if (!(buttons & IN_ATTACK) || !(buttons & IN_DUCK) || !m_firing
        || m_nadeState != 0 || !m_enemyIndex)
        return;

    bool randomCrouch = now < m_crouchFireUntil;

    if (!ValidEnemy(m_enemyIndex))
    {
        buttons &= ~IN_ATTACK;
        m_firing = false;
        m_crouchFireUntil = 0.0f;
        return;
    }

    edict_t *enemy = engine->PEntityOfEntIndex(m_enemyIndex);
    IPlayerInfo *pi = enemy ? playerinfomanager->GetPlayerInfo(enemy) : 0;
    if (!pi)
    {
        buttons &= ~IN_ATTACK;
        m_firing = false;
        m_crouchFireUntil = 0.0f;
        return;
    }

    Vector targetOrigin = pi->GetAbsOrigin();
    Vector target;
    if (FindShootableTargetPoint(enemy, targetOrigin, true, target)) return;

    // Crouching is opportunistic. If the same shot is clear while upright,
    // release duck and preserve the shot. If neither stance is clear, retain
    // ducking when it was requested by movement/cover but never fire blind.
    bool standingClear = FindShootableTargetPoint(enemy, targetOrigin,
                                                  false, target);
    m_crouchFireUntil = 0.0f;
    if (standingClear)
    {
        buttons &= ~IN_DUCK;
        if (insbot_debug.GetBool())
            BotDbg("[insbots] %s crouch fire denied (%s); standing shot preserved\n",
                   m_name, randomCrouch ? "combat roll" : "other crouch request");
    }
    else
    {
        buttons &= ~IN_ATTACK;
        m_firing = false;
        if (insbot_debug.GetBool())
            BotDbg("[insbots] %s fire denied from crouched and standing stances (%s)\n",
                   m_name, randomCrouch ? "combat roll" : "other crouch request");
    }
}

// strict per-frame validity: must be a live enemy player still in the game
bool CInsBot::ValidEnemy(int idx) const
{
    if (idx < 1 || idx > gpGlobals->maxClients) return false;
    edict_t *e = engine->PEntityOfEntIndex(idx);
    if (!e || e->IsFree() || e == m_edict) return false;
    IPlayerInfo *pi = playerinfomanager->GetPlayerInfo(e);
    if (!pi || !pi->IsConnected() || !pi->IsPlayer()) return false;
    if (pi->IsDead() || pi->IsObserver() || pi->IsHLTV()) return false;
    int t = pi->GetTeamIndex();
    if (t <= 0 || t >= 3) return false;                 // must be a play team
    if (t == m_info->GetTeamIndex()) return false;      // not a teammate
    if (pi->GetHealth() <= 0) return false;

    // reject entities sitting at/near the world origin - these are fake clients
    // that connected but never got a squad slot (they pile up at 0,0,0). Targeting
    // them is the "ghost" bug: bots shooting at an invisible stack at origin.
    Vector p = pi->GetAbsOrigin();
    if (p.x > -48.0f && p.x < 48.0f && p.y > -48.0f && p.y < 48.0f && p.z > -48.0f && p.z < 48.0f)
        return false;

    return true;
}

// scan for closest visible enemy player; returns entity index or 0
int CInsBot::FindEnemy()
{
    INSPROF_SCOPE(INSPROF_VISION);
    if (!insbot_detect.GetBool()) return 0;   // detection disabled
    int myTeam = m_info->GetTeamIndex();
    Vector myEye = EyePos();

    // facing direction for FOV test
    float yawRad = m_curAngles.y * (3.14159265f / 180.0f);
    Vector fwd(cosf(yawRad), sinf(yawRad), 0);

    float cosFov = cosf(insbot_fov.GetFloat() * 0.5f * (3.14159265f / 180.0f));
    CombatProfile *combat = CombatForWeapon((const WeaponProfile *)m_wp);
    float maxRange = CombatValue(combat->detectRange, insbot_vis_range);
    if (maxRange < 1.0f) return 0;

    int best = 0;
    float bestDist = maxRange * maxRange;

    for (int i = 1; i <= gpGlobals->maxClients; i++)
    {
        if (!ValidEnemy(i)) continue;
        edict_t *e = engine->PEntityOfEntIndex(i);
        IPlayerInfo *pi = playerinfomanager->GetPlayerInfo(e);

        Vector pos = pi->GetAbsOrigin();
        Vector chest = pos; chest.z += 45.0f;
        Vector d = chest - myEye;
        float distSq = d.x * d.x + d.y * d.y + d.z * d.z;
        
        float scoreDistSq = distSq;
        if (i == m_enemyIndex) {
            scoreDistSq *= 0.64f; // Give current target a 20% distance advantage (0.8^2)
        }
        
        if (scoreDistSq > bestDist) continue;

        // FOV check (skip if we already have this enemy locked)
        if (i != m_enemyIndex)
        {
            Vector dn = d;
            float len = sqrtf(distSq);
            if (len > 1.0f)
            {
                dn.x /= len; dn.y /= len; dn.z /= len;
                float dot = dn.x * fwd.x + dn.y * fwd.y;
                if (dot < cosFov && len > 250.0f)   // close enemies get "heard"
                    continue;
            }
        }

        Vector visiblePoint;
        if (!FindVisibleTargetPoint(e, pos, visiblePoint)) continue;
        
        bool smokeBlocked = false;
        for (int k = 0; k < g_NumSmokeGrenades; k++)
        {
            float distToSmoke = DistanceToLineSegment(g_SmokeGrenades[k], myEye, visiblePoint);
            if (distToSmoke < 140.0f) { smokeBlocked = true; break; }
        }
        if (smokeBlocked) continue;

        // seen: report to team radio and remember
        float now = gpGlobals ? gpGlobals->curtime : 0.0f;
        IntelReport(myTeam, i, pos, now);

        bestDist = scoreDistSq;
        best = i;
    }

    // PASS 1 result: if an enemy was spotted in our assigned sector, report to squad and return
    if (best != 0)
    {
        if (IsSquadFollower())
        {
            float now = gpGlobals ? gpGlobals->curtime : 0.0f;
            edict_t *be = engine->PEntityOfEntIndex(best);
            IPlayerInfo *bpi = be ? playerinfomanager->GetPlayerInfo(be) : 0;
            Vector bpos = bpi ? bpi->GetAbsOrigin() : myEye;
            g_Bots.ReportSquadThreat(m_squadLeaderIndex, best, bpos, now);
        }
        return best;
    }

    // =========================================================================
    // PASS 2: Squad Crossfire & Contact Assist
    // If our sector is quiet, check if squadmates or leader are engaging an
    // off-axis threat that we have clear line-of-sight to.
    // =========================================================================
    if (IsSquadFollower())
    {
        float now = gpGlobals ? gpGlobals->curtime : 0.0f;
        for (int i = 1; i <= gpGlobals->maxClients; i++)
        {
            if (!ValidEnemy(i)) continue;
            if (!g_Bots.IsSquadContact(m_squadLeaderIndex, i, now)) continue;

            edict_t *e = engine->PEntityOfEntIndex(i);
            IPlayerInfo *pi = playerinfomanager ? playerinfomanager->GetPlayerInfo(e) : 0;
            if (!pi) continue;

            Vector pos = pi->GetAbsOrigin();
            Vector chest = pos; chest.z += 45.0f;
            Vector d = chest - myEye;
            float distSq = d.x * d.x + d.y * d.y + d.z * d.z;

            if (distSq > bestDist) continue;

            Vector visiblePoint;
            if (!FindVisibleTargetPoint(e, pos, visiblePoint)) continue;

            bool smokeBlocked = false;
            for (int k = 0; k < g_NumSmokeGrenades; k++)
            {
                float distToSmoke = DistanceToLineSegment(g_SmokeGrenades[k], myEye, visiblePoint);
                if (distToSmoke < 140.0f) { smokeBlocked = true; break; }
            }
            if (smokeBlocked) continue;

            bestDist = distSq;
            best = i;
        }
    }

    return best;
}
