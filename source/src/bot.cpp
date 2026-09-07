// ============================================================================
// bot.cpp - bot brain: joining, navigation, combat
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#include "tier0/dbg.h"
#include "interface.h"
#include "eiface.h"
#include "edict.h"
#include "dlls/iplayerinfo.h"
#include "engine/iserverplugin.h"
#include "engine/IEngineTrace.h"
#include "engine/ivdebugoverlay.h"
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

extern IVEngineServer       *engine;
extern IEngineTrace         *enginetrace;
extern IPlayerInfoManager   *playerinfomanager;
extern IBotManager          *botmanager;
extern IServerPluginHelpers *helpers;
extern CGlobalVars          *gpGlobals;
extern IVDebugOverlay       *debugoverlay;
extern bool                  g_HoldQuotaJoins;
extern bool                  g_RoundFrozen;
extern int                   s_offset_m_CollisionGroup;
extern int                   s_offset_m_hMyWeapons;

bool g_ForceGotoActive = false;
Vector g_ForceGotoPos(0, 0, 0);

Vector g_SmokeGrenades[64];
int g_NumSmokeGrenades = 0;

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

static float DistanceToLineSegment(const Vector& p, const Vector& a, const Vector& b)
{
    Vector ab = b - a;
    Vector ap = p - a;
    float l2 = ab.x*ab.x + ab.y*ab.y + ab.z*ab.z;
    if (l2 == 0.0f) {
        float dx = p.x - a.x, dy = p.y - a.y, dz = p.z - a.z;
        return sqrtf(dx*dx + dy*dy + dz*dz);
    }
    float t = (ap.x*ab.x + ap.y*ab.y + ap.z*ab.z) / l2;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    Vector proj = a + ab * t;
    float dx = p.x - proj.x, dy = p.y - proj.y, dz = p.z - proj.z;
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

static float SignedAngleDiff(float a, float b)
{
    float d = fmodf(a - b, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return d;
}



// settings (CfgVars defined in insbots.cpp) - same getter surface as ConVar
#include "cfgvar.h"
extern CfgVar insbot_join_cmds_a;
extern CfgVar insbot_join_cmds_b;
extern CfgVar insbot_join_interval;
extern CfgVar insbot_slot_sweep;
extern CfgVar insbot_dead_cmds;
extern CfgVar insbot_fov;
extern CfgVar insbot_vis_range;
extern CfgVar insbot_reaction;
extern CfgVar insbot_aim_error;
extern CfgVar insbot_aim_speed;
extern CfgVar insbot_burst_time;
extern CfgVar insbot_burst_pause;
extern CfgVar insbot_move_speed;
extern CfgVar insbot_stop_dist;
extern CfgVar insbot_capture_focus;
extern CfgVar insbot_capture_defend_range;
extern CfgVar insbot_capture_percent;
extern CfgVar insbot_danger_avoid;
extern CfgVar insbot_danger_radius;
extern CfgVar insbot_streaming;
extern CfgVar insbot_goal_classnames;
extern CfgVar insbot_randomize_customization;
extern CfgVar insbot_randomize_loadout;
extern CfgVar insbot_crouch_chance;
extern CfgVar insbot_crouch_eye_height;
extern CfgVar insbot_sniper_overwatch;
extern CfgVar insbot_sniper_relocate_kills;
extern CfgVar insbot_sniper_perch_time_min;
extern CfgVar insbot_sniper_perch_time_max;
extern CfgVar insbot_ironsights;
extern int s_offset_m_hActiveWeapon;
extern int s_offset_m_iClip1;
extern CfgVar insbot_debug;
extern CfgVar insbot_draw;
extern CfgVar insbot_draw_path;
extern CfgVar insbot_objectives;
extern CfgVar insbot_lose_time;
extern CfgVar insbot_draw_team;
extern CfgVar insbot_join_timeout;
extern CfgVar insbot_teamsight;
extern CfgVar insbot_teamwork;
extern CfgVar insbot_intel_time;
extern CfgVar insbot_intel_range;
extern CfgVar insbot_aggression;
extern CfgVar insbot_class_priority;
extern CfgVar insbot_class_quota_rpg;
extern CfgVar insbot_class_quota_grenadier;
extern CfgVar insbot_class_quota_sniper;
extern CfgVar insbot_class_quota_marksman;
extern CfgVar insbot_class_quota_support;
extern CfgVar insbot_class_quota_engineer;
extern CfgVar insbot_class_quota_sapper;
extern CfgVar insbot_class_quota_rifleman;
extern CfgVar insbot_class_quota_sergeant;
extern CfgVar insbot_class_quota_militant;
extern CfgVar insbot_reload;
extern CfgVar insbot_mag_fire_time;
extern CfgVar insbot_sidearm_a;
extern CfgVar insbot_sidearm_b;
extern CfgVar insbot_sidearm_usmc;
extern CfgVar insbot_sidearm_iraqi;
extern CfgVar insbot_grenades;
extern CfgVar insbot_grenade_a;
extern CfgVar insbot_grenade_b;
extern CfgVar insbot_grenade_usmc;
extern CfgVar insbot_grenade_iraqi;
extern CfgVar insbot_grenade_cooldown;
extern CfgVar insbot_grenade_min_range;
extern CfgVar insbot_grenade_chance;
extern CfgVar insbot_grenade_speed;
extern CfgVar insbot_grenade_gravity;
extern CfgVar insbot_smoke;
extern CfgVar insbot_smoke_weapon;
extern CfgVar insbot_smoke_chance;
extern CfgVar insbot_smoke_cooldown;
extern CfgVar insbot_smoke_range;
extern CfgVar insbot_smoke_group;
extern CfgVar insbot_smoke_group_min;
extern CfgVar insbot_smoke_group_radius;
extern CfgVar insbot_smoke_group_chance;
extern CfgVar insbot_smoke_scatter;
extern CfgVar insbot_m203;
extern CfgVar insbot_m203_chance;
extern CfgVar insbot_m203_speed;
extern CfgVar insbot_m203_min_range;
extern CfgVar insbot_m203_max_range;
extern CfgVar insbot_m203_cooldown;
extern CfgVar insbot_m203_obj;
extern CfgVar insbot_m203_obj_chance;
extern CfgVar insbot_rpg_cooldown;
extern CfgVar insbot_patrol;
extern CfgVar insbot_patrol_chance;
extern CfgVar insbot_patrol_radius;
extern CfgVar insbot_patrol_interval;
extern CfgVar insbot_stuck_time;
extern CfgVar insbot_stuck_dist;
extern CfgVar insbot_unstuck_teleport;
extern CfgVar insbot_unstuck_teleport_time;
extern CfgVar insbot_doorway_teleport;
extern CfgVar insbot_doorway_teleport_time;
extern CfgVar insbot_doorway_sprint;
extern CfgVar insbot_stuck_nocollide;
extern CfgVar insbot_ladder_queue;
extern CfgVar insbot_ladder_nocollide;
extern CfgVar insbot_spawn_nocollide;
extern CfgVar insbot_spawn_nocollide_time;
extern CfgVar insbot_freeze;
extern CfgVar insbot_safe_reload;
extern CfgVar insbot_close_range;
extern CfgVar insbot_takecover;
extern CfgVar insbot_cover_health;
extern CfgVar insbot_cover_v2;
extern CfgVar insbot_suppress_time;
extern CfgVar insbot_cover_tier_weight;
extern CfgVar insbot_cover_dist_weight;
extern CfgVar insbot_cover_advance_penalty;
extern CfgVar insbot_cover_min_score;
extern CfgVar insbot_cover_search_radius;
extern CfgVar insbot_cover_max_reach;
extern CfgVar insbot_cover_ray_len;
extern CfgVar insbot_cover_ray_cutoff;extern CfgVar insbot_cover_formation_bonus;
extern CfgVar insbot_learn_persist;
extern CfgVar insbot_sprint;
extern CfgVar insbot_avoid_jump;
extern CfgVar insbot_avoid_crouch;
extern CfgVar insbot_avoid_marked;
extern CfgVar insbot_avoid_ladder;
extern CfgVar insbot_ladder_up_penalty;
extern CfgVar insbot_firemode;
extern CfgVar insbot_recoil_control;
extern CfgVar insbot_firemode_range;
extern CfgVar insbot_hear;
extern CfgVar insbot_hear_range;
extern CfgVar insbot_hear_speed;
extern CfgVar insbot_hear_timeout;
extern CfgVar insbot_hear_cooldown;
extern CfgVar insbot_detect;

// INS:MIC usercmd button bit layout:
// The stock SDK's in_buttons.h definitions do not match Insurgency's custom layout.
#define INS_IN_ATTACK    (1 << 0)   // 1 (Primary Attack / Fire / Launch)
#define INS_IN_JUMP      (1 << 1)   // 2
#define INS_IN_DUCK      (1 << 2)   // 4
#define INS_IN_PRONE     (1 << 3)   // 8
#define INS_IN_FORWARD   (1 << 4)   // 16
#define INS_IN_BACK      (1 << 5)   // 32
#define INS_IN_USE       (1 << 6)   // 64
#define INS_IN_MOVELEFT  (1 << 9)   // 512
#define INS_IN_MOVERIGHT (1 << 10)  // 1024
#define INS_IN_RELOAD    (1 << 11)  // 2048 (Reload)
#define INS_IN_SPRINT    (1 << 15)  // 32768 (Sprint)
#define INS_IN_ATTACK2   (1 << 17)  // 131072 (Secondary Attack: M203 leaf sight, Bipod)
#define INS_IN_ATTACK3   (1 << 18)  // 262144 (Tertiary Attack: Ironsights toggle)

void InsBots_SaveLearn();     // flush learned bad links to disk
void InsBots_ForgetLearn();   // wipe learned bad links (memory + file)

// ---- shared team intelligence (radio) --------------------------------------
// Per team (1,2) last-known position of each enemy entity index. Bots write
// what they see; teammates read to react. Cleared by staleness.
struct Sighting { Vector pos; float time; bool valid; };
struct TeamIntel { Sighting enemy[66]; };
static TeamIntel g_Intel[3];

static void IntelReport(int team, int enemyIdx, const Vector &pos, float now)
{
    if (team < 1 || team > 2 || enemyIdx < 1 || enemyIdx > 65) return;
    g_Intel[team].enemy[enemyIdx].pos = pos;
    g_Intel[team].enemy[enemyIdx].time = now;
    g_Intel[team].enemy[enemyIdx].valid = true;
}

static bool IntelEnemyIsLive(int team, int enemyIdx)
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
static int IntelFreshest(int team, float now, float maxAge, const Vector &listenerPos,
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

static void IntelReset() { memset(g_Intel, 0, sizeof(g_Intel)); }

// ---------------- hearing / footstep investigation ----------------
// This SDK's IPlayerInfo exposes no velocity getter, so a moving enemy's
// horizontal speed is estimated once per player per server frame. Every bot
// then reads the same cached value; listeners must not consume one another's
// origin samples.
static Vector s_hearLastOrigin[66];
static float  s_hearLastTime[66];
static float  s_hearSpeed[66];
static int    s_hearUserId[66];
static bool   s_hearHasLast[66];
static float  s_hearFrameTime = -1.0f;
// Door traces need to ignore every client entity. Build this list alongside
// the once-per-frame hearing sample rather than scanning all client edicts for
// every bot's DoorAhead call (which becomes very expensive at a 30-bot quota).
static IHandleEntity *s_tracePlayerHandles[66];
static int s_tracePlayerHandleCount = 0;

static void HearingClearSlot(int idx)
{
    if (idx < 1 || idx > 65) return;
    s_hearLastOrigin[idx] = Vector(0, 0, 0);
    s_hearLastTime[idx] = 0.0f;
    s_hearSpeed[idx] = 0.0f;
    s_hearUserId[idx] = 0;
    s_hearHasLast[idx] = false;
}

static void HearingReset()
{
    for (int i = 1; i <= 65; i++) HearingClearSlot(i);
    s_hearFrameTime = -1.0f;
    s_tracePlayerHandleCount = 0;
}

static void HearingFrameUpdate()
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

struct BodyGracePlayerState
{
    bool owned;
    int userId;
    int team;
    int originalGroup;
    unsigned long long owners;
    float cooldownRemaining;
};

static BodyGracePlayerState s_bodyGracePlayer[66];

enum TeleportFailureReason
{
    TELEPORT_OK = 0,
    TELEPORT_DISABLED,
    TELEPORT_COOLDOWN,
    TELEPORT_NO_CONTROLLER,
    TELEPORT_NAV_NOT_LOADED,
    TELEPORT_NO_CURRENT_AREA,
    TELEPORT_NO_CONNECTED_EDGE,
    TELEPORT_DEST_AREA_MISSING,
    TELEPORT_NO_SAFE_DESTINATION
};

static const char *TeleportFailureName(int reason)
{
    switch (reason)
    {
    case TELEPORT_DISABLED:            return "disabled";
    case TELEPORT_COOLDOWN:            return "cooldown";
    case TELEPORT_NO_CONTROLLER:       return "no bot controller";
    case TELEPORT_NAV_NOT_LOADED:      return "NAV not loaded";
    case TELEPORT_NO_CURRENT_AREA:     return "no current NAV area";
    case TELEPORT_NO_CONNECTED_EDGE:   return "no connected destination edge";
    case TELEPORT_DEST_AREA_MISSING:   return "destination NAV area missing";
    case TELEPORT_NO_SAFE_DESTINATION: return "no safe destination";
    default:                           return "none";
    }
}

CBotRoster g_Bots;

static const char *s_BotNamesUSMC[] = {
    "James", "John", "Robert", "Michael", "David", "William", "Richard", "Thomas",
    "Charles", "Daniel", "Matthew", "Anthony", "Joseph", "Steven", "Andrew", "Joshua",
};

static const char *s_BotNamesIraqi[] = {
    "Ahmed", "Ali", "Hassan", "Hussein", "Omar", "Abbas", "Mustafa", "Saad",
    "Tariq", "Zaid", "Kareem", "Faisal", "Qasim", "Yasin", "Bilal", "Mahmoud",
};

// In Insurgency, teamSlot 0 (pteamsetup 1) lands on engine team index 2,
// while teamSlot 1 (pteamsetup 2) lands on engine team index 1!
static inline int TeamSlotToTeamIndex(int teamSlot)
{
    return (teamSlot == 0) ? 2 : 1;
}

// The class-menu protocol does not number roles consecutively.  These are the
// sixteen IDs emitted by the stock USMC and Iraqi menus (same IDs for both).
static int JoinRoleId(int ordinal, int cycle)
{
    static const int s_roleIds[16] = {
        5, 9, 13, 17, 21, 25, 29, 33,
        6, 10, 14, 18, 22, 26, 30, 34
    };

    int roleIndex;
    if (cycle <= 0)
        roleIndex = ordinal % 16;       // unique initial role for each quota bot
    else
        roleIndex = (16 - (cycle % 16)) % 16; // retry normally-free roles 15,14,... first
    if (roleIndex < 0) roleIndex += 16;
    return s_roleIds[roleIndex];
}

static int JoinFactionArg(int teamSlot)
{
    int team = TeamSlotToTeamIndex(teamSlot);
    int faction = (team == 1 || team == 2) ? g_teamFaction[team] : FACTION_UNKNOWN;
    if (faction == FACTION_UNKNOWN)
        faction = (team == 1) ? FACTION_USMC : FACTION_IRAQI;
    return (faction == FACTION_IRAQI) ? 1 : 0;
}

// Select an exact setup role for an automatic quota bot. The role IDs are the
// real sparse menu IDs from class mapping.txt. Iraqi RPG-7 and USMC grenadier
// intentionally have independent names and quotas, even though they occupy
// equivalent launcher slots in their respective faction menus.
static int PickPriorityClassRole(int teamSlot)
{
    static const int s_rpg[]       = { 14, 30 };
    static const int s_grenadier[] = { 14, 30 };
    static const int s_sniper[]    = { 18, 34 };
    static const int s_marksman[]  = { 18, 34 };
    static const int s_support[]   = { 17, 33 };
    static const int s_engineer[]  = { 26 };
    static const int s_sapper[]    = { 26 };
    static const int s_rifleman[]  = { 9, 13, 25, 29, 10 };
    static const int s_sergeant[]  = { 5, 21, 6, 22 };
    static const int s_militant[]  = { 5, 21, 6, 22 };

    const char *p = insbot_class_priority.GetString();
    int faction = JoinFactionArg(teamSlot); // 0 USMC, 1 Iraqi
    while (p && *p)
    {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        char name[32]; int n = 0;
        while (*p && *p != ',' && n < (int)sizeof(name) - 1)
        {
            if (*p != ' ' && *p != '\t') name[n++] = *p;
            p++;
        }
        name[n] = 0;
        if (!name[0]) continue;

        const int *roles = 0;
        int roleCount = 0;
        CfgVar *quota = 0;
        if (!_stricmp(name, "rpg") && faction == 1)
            { roles = s_rpg; roleCount = 2; quota = &insbot_class_quota_rpg; }
        else if (!_stricmp(name, "grenadier") && faction == 0)
            { roles = s_grenadier; roleCount = 2; quota = &insbot_class_quota_grenadier; }
        else if (!_stricmp(name, "sniper") && faction == 1)
            { roles = s_sniper; roleCount = 2; quota = &insbot_class_quota_sniper; }
        else if (!_stricmp(name, "marksman") && faction == 0)
            { roles = s_marksman; roleCount = 2; quota = &insbot_class_quota_marksman; }
        else if (!_stricmp(name, "support"))
            { roles = s_support; roleCount = 2; quota = &insbot_class_quota_support; }
        else if (!_stricmp(name, "engineer") && faction == 0)
            { roles = s_engineer; roleCount = 1; quota = &insbot_class_quota_engineer; }
        else if (!_stricmp(name, "sapper") && faction == 1)
            { roles = s_sapper; roleCount = 1; quota = &insbot_class_quota_sapper; }
        else if (!_stricmp(name, "rifleman"))
            { roles = s_rifleman; roleCount = 5; quota = &insbot_class_quota_rifleman; }
        else if (!_stricmp(name, "sergeant") && faction == 0)
            { roles = s_sergeant; roleCount = 4; quota = &insbot_class_quota_sergeant; }
        else if (!_stricmp(name, "militant") && faction == 1)
            { roles = s_militant; roleCount = 4; quota = &insbot_class_quota_militant; }

        if (!roles || !quota) continue; // unavailable on this faction
        int wanted = quota->GetInt();
        if (wanted <= 0) continue;
        if (wanted > roleCount) wanted = roleCount; // never request duplicate slots
        int filled = g_Bots.CountRequestedRoles(teamSlot, roles, roleCount);
        if (filled < wanted) return roles[filled % roleCount];
    }
    return -1; // quotas met/off: preserve the ordinary automatic rotation
}

static int GetTeamSlotForFaction(int targetFaction)
{
    int fac0 = g_teamFaction[TeamSlotToTeamIndex(0)];
    int fac1 = g_teamFaction[TeamSlotToTeamIndex(1)];
    if (fac0 == FACTION_UNKNOWN) fac0 = FACTION_IRAQI; // slot 0 -> engine team 2
    if (fac1 == FACTION_UNKNOWN) fac1 = FACTION_USMC;  // slot 1 -> engine team 1

    if (fac0 == targetFaction) return 0;
    if (fac1 == targetFaction) return 1;
    return (targetFaction == FACTION_USMC) ? 1 : 0;
}

static int GetTeamSlotForBotName(const char *name, int defaultSlot)
{
    if (!name || !name[0]) return defaultSlot;
    const char *p = name;
    if (strncmp(p, "[BOT] ", 6) == 0) p += 6;
    while (*p == ' ' || *p == '\t') p++;

    for (size_t i = 0; i < sizeof(s_BotNamesUSMC) / sizeof(s_BotNamesUSMC[0]); i++)
    {
        if (!_stricmp(p, s_BotNamesUSMC[i]))
            return GetTeamSlotForFaction(FACTION_USMC);
    }
    for (size_t i = 0; i < sizeof(s_BotNamesIraqi) / sizeof(s_BotNamesIraqi[0]); i++)
    {
        if (!_stricmp(p, s_BotNamesIraqi[i]))
            return GetTeamSlotForFaction(FACTION_IRAQI);
    }
    return defaultSlot;
}

static float frand(float lo, float hi)
{
    return lo + (hi - lo) * ((float)rand() / (float)RAND_MAX);
}

static int RandomIndex(int count)
{
    if (count <= 1) return 0;
    return (int)(((unsigned long long)(unsigned int)rand()
                  * (unsigned long long)(unsigned int)count)
                 / ((unsigned long long)(unsigned int)RAND_MAX + 1ULL));
}

static bool CommandContainsToken(const char *command, const char *token)
{
    if (!command || !token || !token[0]) return false;
    size_t len = strlen(token);
    for (const char *p = command; *p; p++)
    {
        if (strlen(p) < len) break;
        if (_strnicmp(p, token, len) != 0) continue;
        bool left = (p == command || p[-1] == ';' || p[-1] == ' '
                     || p[-1] == '\t' || p[-1] == '\r' || p[-1] == '\n');
        char after = p[len];
        bool right = (after == 0 || after == ';' || after == ' '
                      || after == '\t' || after == '\r' || after == '\n');
        if (left && right) return true;
    }
    return false;
}

static void BotDbg(const char *fmt, ...);

static const char *CustomizationFactionName(int faction)
{
    if (faction == FACTION_USMC) return "USMC";
    if (faction == FACTION_IRAQI) return "Iraqi";
    return "unknown";
}

static const char *GetWeaponNameForId(int weaponId)
{
    switch (weaponId)
    {
    case 2: return "m16a4";
    case 3: return "m16m203";
    case 4: return "m4";
    case 5: return "m4med";
    case 8: return "m14";
    case 9: return "m249";
    case 10: return "m1014";
    case 14: return "ak47";
    case 15: return "rpk";
    case 16: return "rpg7";
    case 18: return "fnfal";
    case 19: return "sks";
    case 20: return "toz";
    case 21: return "l42a1";
    case 22: return "svd";
    case 24: return "aks74u";
    case -1: return "default";
    default: return "unknown";
    }
}

static int RollRoleWeapon(int faction, int roleId, int lastWeapon, bool haveLastWeapon)
{
    if (!insbot_randomize_loadout.GetBool())
        return -1;

    const int *choices = NULL;
    int choiceCount = 0;

    if (faction == FACTION_USMC)
    {
        switch (roleId)
        {
        case 5:
        case 6:
        case 21:
        case 22:
            // USMC Sergeant
            {
                static const int s_sergeantWeapons[] = { 5, 4 }; // M4MED, M4
                choices = s_sergeantWeapons;
                choiceCount = 2;
            }
            break;
        default:
            break;
        }
    }
    else if (faction == FACTION_IRAQI)
    {
        switch (roleId)
        {
        case 5:
        case 6:
        case 21:
        case 22:
            // Iraqi Militant
            {
                static const int s_militantWeapons[] = { 18, 24 }; // FN FAL, AKS-74U
                choices = s_militantWeapons;
                choiceCount = 2;
            }
            break;
        case 9:
        case 10:
        case 13:
        case 25:
        case 29:
            // Iraqi Rifleman
            {
                static const int s_riflemanWeapons[] = { 14, 19 }; // AK-47, SKS
                choices = s_riflemanWeapons;
                choiceCount = 2;
            }
            break;
        case 18:
        case 34:
            // Iraqi Sniper
            {
                static const int s_sniperWeapons[] = { 22, 21 }; // SVD, L42A1
                choices = s_sniperWeapons;
                choiceCount = 2;
            }
            break;
        default:
            break;
        }
    }

    if (!choices || choiceCount <= 1)
        return -1;

    int idx = RandomIndex(choiceCount);
    int weapon = choices[idx];

    // Anti-repetition across consecutive lives for this bot when choices exist
    if (haveLastWeapon && lastWeapon == weapon)
    {
        weapon = choices[(idx + 1) % choiceCount];
    }

    return weapon;
}

void CInsBot::RollCustomizationForLife()
{
    m_customizationPending = false;
    bool custActive = insbot_randomize_customization.GetBool();
    bool loadActive = insbot_randomize_loadout.GetBool();
    if ((!custActive && !loadActive) || !m_info)
        return;

    int team = m_info->GetTeamIndex();
    if (team < 1 || team > 2)
        team = TeamSlotToTeamIndex(m_teamSlot);
    if (team < 1 || team > 2)
        return;

    int faction = g_teamFaction[team];
    // Do not guess a model pool when the map faction has not been resolved.
    if (faction != FACTION_USMC && faction != FACTION_IRAQI)
        return;

    const int skinCount = (faction == FACTION_USMC) ? 12 : 10;
    const int bodyCount = (faction == FACTION_USMC) ? 8 : 6;
    int skin = RandomIndex(skinCount);
    int body = 0;
    if (faction == FACTION_USMC)
    {
        // The native USMC bodygroups have bases 1, 2, and 4. Roll each
        // binary accessory independently, then pack those choices into the
        // one body argument accepted by customize.
        int crotchpad = RandomIndex(2);
        int neckguard = RandomIndex(2);
        int eyewear = RandomIndex(2);
        body = crotchpad + neckguard * 2 + eyewear * 4;
    }
    else
    {
        // The native Iraqi bodygroups have bases 1 and 3. Headgear has three
        // choices; chest-rig state has two.
        int headgear = RandomIndex(3);
        int chestrig = RandomIndex(2);
        body = headgear + chestrig * 3;
    }

    // Keep the rolls independent in the normal case, but avoid repeating the
    // exact previous appearance for this bot when another combination exists.
    if (m_haveLastCustomization
        && m_lastCustomizationFaction == faction
        && m_lastCustomizationSkin == skin
        && m_lastCustomizationBody == body)
    {
        body = (body + 1 + RandomIndex(bodyCount - 1)) % bodyCount;
    }

    m_customizationFaction = faction;
    m_customizationSkin = skin;
    m_customizationBody = body;
    m_customizationWeapon = -1;
    m_customizationWeaponRolled = false;
    m_customizationPending = true;

    if (m_setupRoleId > 0)
    {
        UpdateCustomizationWeapon(m_setupRoleId);
    }

    BotDbg("[insbots] %s customization roll: faction=%s skin=%d body=%d weapon=%s(%d)\n",
           m_name, CustomizationFactionName(faction), skin, body,
           GetWeaponNameForId(m_customizationWeapon), m_customizationWeapon);
}

void CInsBot::UpdateCustomizationWeapon(int roleId)
{
    if (roleId <= 0) return;
    int team = m_info ? m_info->GetTeamIndex() : 0;
    if (team < 1 || team > 2)
        team = TeamSlotToTeamIndex(m_teamSlot);
    int faction = (team == 1 || team == 2) ? g_teamFaction[team] : FACTION_UNKNOWN;
    if (faction == FACTION_UNKNOWN)
        faction = (m_teamSlot == 0) ? FACTION_USMC : FACTION_IRAQI;

    if (m_customizationWeaponRolled && m_setupRoleId == roleId && m_customizationFaction == faction)
        return;

    m_setupRoleId = roleId;
    m_customizationFaction = faction;
    bool sameRoleAsLastLife = m_haveLastCustomizationWeapon
                           && (m_lastCustomizationRoleId == roleId)
                           && (m_lastCustomizationFaction == faction);
    m_customizationWeapon = RollRoleWeapon(faction, roleId, m_lastCustomizationWeapon, sameRoleAsLastLife);
    m_customizationWeaponRolled = true;

    BotDbg("[insbots] %s loadout roll: faction=%s role=%d weapon=%s(%d)\n",
           m_name, CustomizationFactionName(faction), roleId,
           GetWeaponNameForId(m_customizationWeapon), m_customizationWeapon);
}

bool CInsBot::SendCustomizationBeforeSpawn()
{
    bool custActive = insbot_randomize_customization.GetBool();
    bool loadActive = insbot_randomize_loadout.GetBool();
    if (!custActive && !loadActive)
    {
        m_customizationPending = false;
        return false;
    }
    if (!m_customizationPending)
        RollCustomizationForLife();
    if (!m_customizationPending || !helpers || !m_edict)
        return false;

    int skinArg = custActive ? m_customizationSkin : -1;
    int bodyArg = custActive ? m_customizationBody : -1;
    int weaponArg = loadActive ? m_customizationWeapon : -1;

    char cmd[96];
    _snprintf(cmd, sizeof(cmd), "customize %d %d %d",
              skinArg, bodyArg, weaponArg);
    cmd[sizeof(cmd) - 1] = 0;
    helpers->ClientCommand(m_edict, cmd);
    BotDbg("[insbots] %s -> %s\n", m_name, cmd);
    return true;
}

// ---- weapon profiles (extracted from the game's encrypted .ctx scripts) -----
// modes bitfield: SEMI=1, BURST(3rnd)=2, AUTO=4.  rpm = cyclic rate (rounds/min,
// the game's "cycle_time"); ms/round = 60000/rpm.  recoil = the script's
// recoil_scale (multiplies the global base kick).
enum { FM_SEMI = 1, FM_BURST = 2, FM_AUTO = 4 };
enum { WF_SHOTGUN = 1, WF_BOLT = 2, WF_LAUNCHER = 4, WF_SNIPER = 8, WF_EMPTY_RELOAD_ONLY = 16 };
enum WeaponCombatClass
{
    WC_RIFLE = 0,
    WC_LMG,
    WC_SNIPER,
    WC_SHOTGUN,
    WC_PISTOL,
    WC_RPG,
    WC_GRENADE_LAUNCHER,
    WC_COUNT = INSBOT_COMBAT_CLASS_COUNT
};

struct WeaponProfile
{
    const char   *cls;       // classname suffix (after "weapon_")
    short         mag;
    unsigned char modes;
    short         rpm;        // cyclic rate, rounds/min
    short         burstRpm;   // rate within a burst (0 => use rpm)
    float         recoil;
    unsigned char flags;
    unsigned char combatClass;
    CfgVar       *safe_reload_cvar;
};
static WeaponProfile s_wp[] = {
    { "m16a4",   30, FM_SEMI|FM_BURST, 700, 700, 2.7f, 0,                    WC_RIFLE,   NULL },
    { "m16m203", 30, FM_SEMI|FM_BURST, 700, 700, 2.5f, 0,                    WC_RIFLE,   NULL },
    { "m4med",   30, FM_SEMI|FM_AUTO,  700,   0, 1.8f, 0,                    WC_RIFLE,   NULL },
    { "m4",      30, FM_SEMI|FM_AUTO,  700,   0, 2.1f, 0,                    WC_RIFLE,   NULL },
    { "m14",     20, FM_SEMI,          290,   0, 2.5f, WF_SNIPER,            WC_RIFLE,   NULL },
    { "ak47",    30, FM_SEMI|FM_AUTO,  600,   0, 2.5f, 0,                    WC_RIFLE,   NULL },
    { "ak74",    30, FM_SEMI|FM_AUTO,  600,   0, 1.0f, 0,                    WC_RIFLE,   NULL },
    { "aks74u",  30, FM_SEMI|FM_AUTO,  650,   0, 2.2f, 0,                    WC_RIFLE,   NULL },
    { "sks",     10, FM_SEMI,          170,   0, 2.5f, WF_SNIPER | WF_EMPTY_RELOAD_ONLY, WC_SNIPER,  NULL },
    { "fnfal",   20, FM_SEMI,          650,   0, 2.5f, 0,                    WC_RIFLE,   NULL },
    { "rpk",     75, FM_SEMI|FM_AUTO,  600,   0, 3.5f, 0,                    WC_LMG,     NULL },
    { "m249",   200, FM_AUTO,          725,   0, 3.5f, 0,                    WC_LMG,     NULL },
    { "svd",     10, FM_SEMI,          170,   0, 2.5f, WF_SNIPER,            WC_SNIPER,  NULL },
    { "l42a1",   10, FM_SEMI,           55,   0, 2.5f, WF_BOLT|WF_SNIPER,    WC_SNIPER,  NULL },
    { "m1014",    7, FM_SEMI,          200,   0, 5.0f, WF_SHOTGUN,           WC_SHOTGUN, NULL },
    { "toz",      6, FM_SEMI,          200,   0, 5.0f, WF_SHOTGUN,           WC_SHOTGUN, NULL },
    { "m9",      15, FM_SEMI,          240,   0, 4.0f, 0,                    WC_PISTOL,  NULL },
    { "makarov", 10, FM_SEMI,          240,   0, 4.0f, 0,                    WC_PISTOL,  NULL },
    { "1911",     7, FM_SEMI,          180,   0, 5.0f, 0,                    WC_PISTOL,  NULL },
    { "rpg7",     1, FM_SEMI,           20,   0, 10.0f, WF_LAUNCHER,         WC_RPG,              NULL },
    { "m203",     1, FM_SEMI,           20,   0,  8.0f, WF_LAUNCHER,         WC_GRENADE_LAUNCHER, NULL },
};
static WeaponProfile s_wpDefault =
    { "default", 30, FM_SEMI|FM_AUTO, 600, 0, 2.5f, 0, WC_RIFLE, NULL };

struct CombatProfile
{
    const char *name;
    CfgVar *detectRange;
    CfgVar *engageRange;
    CfgVar *stopDist;
    CfgVar *reaction;
    CfgVar *aimError;
    CfgVar *aimSpeed;
    CfgVar *reserveMags;
    CfgVar *recoilControl;
    CfgVar *recoilPercent;
};

static CombatProfile s_combat[WC_COUNT] = {
    { "rifle",           NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
    { "lmg",             NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
    { "sniper",          NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
    { "shotgun",         NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
    { "pistol",          NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
    { "rpg",             NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
    { "grenadelauncher", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
};

// A negative class value inherits the old global setting.  Rifle defaults use
// inheritance so existing server.cfg tuning remains effective after upgrading.
static const char *s_combatDefaults[WC_COUNT][7] = {
    { "-1",   "-1",   "-1",   "-1",  "-1",  "-1",  "7"  },
    { "-1",   "-1",   "-1",   "-1",  "-1",  "-1",  "7"  },
    { "4000", "4000", "1600", "0.5", "1",   "210", "25" },
    { "1400", "850",  "300",  "0.5", "6.0", "320", "50" },
    { "2000", "1300", "450",  "0.3", "4.5", "300", "5"  },
    { "3000", "2400", "900",  "0.2", "0",   "360", "10" },
    { "3500", "3500", "1000", "0.2", "1.0", "300", "10" },
};

// The direct percentage is optional.  -1 follows the selected control mode.
static const char *s_combatRecoilModeDefaults[WC_COUNT] = {
    "inherit", "inherit", "inherit", "inherit", "inherit", "inherit", "inherit"
};
static const char *s_combatRecoilPercentDefaults[WC_COUNT] = {
    "-1", "-1", "-1", "-1", "-1", "100", "100"
};

static CfgVar *NewCombatCfg(const char *profile, const char *setting,
                            const char *defaultValue, const char *help)
{
    char *name = (char*)malloc(64);
    sprintf(name, "%s_%s", profile, setting);
    return new CfgVar(name, defaultValue, help);
}

void InsBots_InitWeaponCvars()
{
    static bool init = false;
    if (init) return;
    init = true;
    for (unsigned i = 0; i < sizeof(s_wp)/sizeof(s_wp[0]); i++)
    {
        char *name = (char*)malloc(64);
        sprintf(name, "reload_safe_pct_%s", s_wp[i].cls);
        s_wp[i].safe_reload_cvar = new CfgVar(name, "0.60", "Safe reload threshold (pct ammo remaining) for weapon");
    }
    s_wpDefault.safe_reload_cvar = new CfgVar("reload_safe_pct_default", "0.60", "Default safe reload threshold");

    for (int i = 0; i < WC_COUNT; i++)
    {
        CombatProfile &p = s_combat[i];
        p.detectRange = NewCombatCfg(p.name, "detect_range", s_combatDefaults[i][0], "maximum enemy detection range; -1 inherits vis_range");
        p.engageRange = NewCombatCfg(p.name, "engage_range", s_combatDefaults[i][1], "maximum range at which this weapon class may fire; -1 inherits vis_range");
        p.stopDist    = NewCombatCfg(p.name, "stop_dist",    s_combatDefaults[i][2], "distance at which this weapon class stops advancing; -1 inherits stop_dist");
        p.reaction    = NewCombatCfg(p.name, "reaction",     s_combatDefaults[i][3], "reaction delay before firing; -1 inherits reaction");
        p.aimError    = NewCombatCfg(p.name, "aim_error",    s_combatDefaults[i][4], "aim error in degrees; -1 inherits aim_error");
        p.aimSpeed    = NewCombatCfg(p.name, "aim_speed",    s_combatDefaults[i][5], "maximum aim turn speed in degrees/sec; -1 inherits aim_speed");
        p.reserveMags = NewCombatCfg(p.name, "reserve_mags", s_combatDefaults[i][6], "number of reserve magazines this weapon class may reload per life");
        p.recoilControl = NewCombatCfg(p.name, "recoil_control", s_combatRecoilModeDefaults[i], "recoil mode: inherit, realistic, perfect, aimerror, or percent");
        p.recoilPercent = NewCombatCfg(p.name, "recoil_percent", s_combatRecoilPercentDefaults[i], "direct recoil compensation percent (0-100); -1 follows recoil_control");
    }
}

static const WeaponProfile *WeaponFind(const char *name)
{
    if (!name || !name[0]) return 0;
    // Skip grenades, smoke, knives, C4 so holding a grenade doesn't overwrite the held gun profile
    if (strstr(name, "m67") || strstr(name, "m18") || strstr(name, "anm14") ||
        strstr(name, "f1") || strstr(name, "grenade") || strstr(name, "smoke") ||
        strstr(name, "knife") || strstr(name, "c4"))
        return 0;

    if (!strncmp(name, "weapon_", 7)) name += 7;
    for (unsigned i = 0; i < sizeof(s_wp) / sizeof(s_wp[0]); i++)
        if (!_stricmp(name, s_wp[i].cls)) return &s_wp[i];
    return &s_wpDefault;
}

static CombatProfile *CombatForWeapon(const WeaponProfile *wp)
{
    int cls = wp ? (int)wp->combatClass : (int)WC_RIFLE;
    if (cls < 0 || cls >= WC_COUNT) cls = WC_RIFLE;
    return &s_combat[cls];
}

static int CombatClassForWeapon(const WeaponProfile *wp)
{
    int cls = wp ? (int)wp->combatClass : (int)WC_RIFLE;
    if (cls < 0 || cls >= WC_COUNT) cls = WC_RIFLE;
    return cls;
}

static int CombatReserveMags(const WeaponProfile *wp)
{
    CombatProfile *combat = CombatForWeapon(wp);
    int mags = combat->reserveMags ? combat->reserveMags->GetInt() : 0;
    return mags < 0 ? 0 : mags;
}

static float CombatValue(CfgVar *classValue, CfgVar &fallback)
{
    float value = classValue ? classValue->GetFloat() : -1.0f;
    return value < 0.0f ? fallback.GetFloat() : value;
}

static float CombatRecoilControl(CombatProfile *combat, float aimError)
{
    const char *mode = combat && combat->recoilControl ? combat->recoilControl->GetString() : "inherit";
    if (!mode || !mode[0] || !_stricmp(mode, "inherit"))
        mode = insbot_recoil_control.GetString();

    float control;
    if (!_stricmp(mode, "perfect")) control = 0.93f;
    else if (!_stricmp(mode, "aimerror"))
    {   // low aim_error => high control
        control = 1.0f - aimError / 12.0f;
        if (control < 0.30f) control = 0.30f;
        if (control > 0.95f) control = 0.95f;
    }
    else /* realistic or percent */ control = 0.55f;

    // A specified percentage always wins, allowing an exact per-class amount.
    float percent = combat && combat->recoilPercent ? combat->recoilPercent->GetFloat() : -1.0f;
    if (percent >= 0.0f)
    {
        if (percent > 100.0f) percent = 100.0f;
        control = percent * 0.01f;
    }
    return control;
}

static void BotDbg(const char *fmt, ...)
{
    if (!insbot_debug.GetBool() || !engine) return;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    Msg("%s", buf);
}

static inline bool IsHadithaMapName(const char *name)
{
    if (!name || !name[0]) return false;
    return !_stricmp(name, "ins_haditha") || !_stricmp(name, "ins_haditha_night");
}

// ---- trace filters ----------------------------------------------------------
class CTraceFilterSkipSelf : public CTraceFilter
{
public:
    CTraceFilterSkipSelf(IHandleEntity *self) : m_self(self) {}
    virtual bool ShouldHitEntity(IHandleEntity *pEntity, int contentsMask)
    {
        return pEntity != m_self;
    }
    IHandleEntity *m_self;
};

// Geometry traces used for doors must see through every player. Otherwise a
// follower traces into the lead bot and never sees the closed door behind it.
class CTraceFilterSkipPlayers : public CTraceFilter
{
public:
    CTraceFilterSkipPlayers(IHandleEntity *self) : m_self(self) {}

    virtual bool ShouldHitEntity(IHandleEntity *pEntity, int /*contentsMask*/)
    {
        if (pEntity == m_self) return false;
        for (int i = 0; i < s_tracePlayerHandleCount; i++)
            if (pEntity == s_tracePlayerHandles[i]) return false;
        return true;
    }

private:
    IHandleEntity *m_self;
};

static IHandleEntity *EdictHandleEntity(edict_t *e)
{
    if (!e) return 0;
    IServerUnknown *unk = e->GetUnknown();
    return (IHandleEntity *)unk;
}

// ============================================================================
// CInsBot
// ============================================================================
CInsBot::CInsBot()
{
    memset(this, 0, sizeof(*this));
    m_primaryWeaponName[0] = 0;
    m_reloadingClass = -1;
    m_sidearmTemporary = false;
    m_sniperHoldUntil = 0.0f;
    m_dropHoldExpired = false;
    m_setupRoleId = -1;
    m_customizationWeapon = -1;
    m_lastCustomizationRoleId = -1;
    m_lastCustomizationWeapon = -1;
    m_m203State = 0;
    m_nextM203 = 0.0f;
    m_m203Timer = 0.0f;
    m_m203Pulse = false;
    m_areaCache = -1;
    m_areaCacheTime = -1.0f;
    m_routeRemaining = 3.4e38f;
    m_captureObj = -1;
    m_captureTeam = 0;
    m_haveAdvanceTarget = false; m_advanceTargetRefresh = 0.0f;
    m_ladderClimbIdx = -1;
    m_ladderFromArea = m_ladderToArea = -1;
    m_sniperArea = -1;
    m_sniperObj = -1;
}

bool CInsBot::IsSniper() const
{
    if (m_wp)
    {
        const WeaponProfile *wp = (const WeaponProfile *)m_wp;
        if (wp->flags & WF_SNIPER) return true;
    }
    if (m_wpName[0] && strstr(m_wpName, "sniper")) return true;
    return false;
}

float CInsBot::Now() const
{
    return gpGlobals->curtime;
}

static Vector LadderAreaInterior(int areaIdx, const Vector &lip);

static int PlayerCollisionGroup(edict_t *edict)
{
    if (!edict || edict->IsFree()) return -1;
    IServerUnknown *unknown = edict->GetUnknown();
    ICollideable *collideable = unknown ? unknown->GetCollideable() : 0;
    return collideable ? collideable->GetCollisionGroup() : -1;
}

static bool WritePlayerCollisionGroup(edict_t *edict, int collisionGroup)
{
    if (s_offset_m_CollisionGroup < 0 || !edict || edict->IsFree())
        return false;
    IServerUnknown *unknown = edict->GetUnknown();
    char *base = unknown ? (char *)unknown->GetBaseEntity() : 0;
    if (!base) return false;
    *(int *)(base + s_offset_m_CollisionGroup) = collisionGroup;
    // The offset-tracking overload depends on engine-private symbols that are
    // not linkable from a standalone server plugin. Mark the full edict dirty;
    // this is the public edict state the engine consumes on its next snapshot.
    edict->m_fStateFlags |= FL_EDICT_CHANGED | FL_FULL_EDICT_CHANGED;
    return true;
}

bool CInsBot::WriteCollisionGroup(int collisionGroup)
{
    return WritePlayerCollisionGroup(m_edict, collisionGroup);
}

static bool BodyGraceHullOverlapsAny(int clientIndex)
{
    if (!gpGlobals || !engine || !playerinfomanager
        || clientIndex < 1 || clientIndex > gpGlobals->maxClients)
        return true;
    edict_t *edict = engine->PEntityOfEntIndex(clientIndex);
    IPlayerInfo *info = (edict && !edict->IsFree())
                      ? playerinfomanager->GetPlayerInfo(edict) : 0;
    if (!info || !info->IsConnected() || info->IsDead()) return false;

    Vector pos = info->GetAbsOrigin();
    Vector mins = pos + info->GetPlayerMins();
    Vector maxs = pos + info->GetPlayerMaxs();
    int maxClients = gpGlobals->maxClients;
    if (maxClients > 65) maxClients = 65;
    for (int i = 1; i <= maxClients; i++)
    {
        if (i == clientIndex) continue;
        edict_t *otherEdict = engine->PEntityOfEntIndex(i);
        if (!otherEdict || otherEdict->IsFree()) continue;
        IPlayerInfo *other = playerinfomanager->GetPlayerInfo(otherEdict);
        if (!other || !other->IsConnected() || !other->IsPlayer()
            || other->IsObserver() || other->IsDead())
            continue;
        Vector otherPos = other->GetAbsOrigin();
        Vector otherMins = otherPos + other->GetPlayerMins();
        Vector otherMaxs = otherPos + other->GetPlayerMaxs();
        bool separate = maxs.x <= otherMins.x || mins.x >= otherMaxs.x
                     || maxs.y <= otherMins.y || mins.y >= otherMaxs.y
                     || maxs.z <= otherMins.z || mins.z >= otherMaxs.z;
        if (!separate) return true;
    }
    return false;
}

bool CBotRoster::CanAcquireBodyGracePlayer(int clientIndex, int ownerSlot)
{
    if (clientIndex < 1 || clientIndex > 64 || ownerSlot < 0 || ownerSlot >= 64
        || !engine || !playerinfomanager || !gpGlobals)
        return false;
    edict_t *edict = engine->PEntityOfEntIndex(clientIndex);
    IPlayerInfo *info = (edict && !edict->IsFree())
                      ? playerinfomanager->GetPlayerInfo(edict) : 0;
    if (!info || !info->IsConnected() || !info->IsPlayer()
        || info->IsObserver() || info->IsDead() || info->GetTeamIndex() <= 0)
        return false;

    const BodyGracePlayerState &state = s_bodyGracePlayer[clientIndex];
    int userId = info->GetUserID();
    if (state.owned)
        return state.userId == userId
            && PlayerCollisionGroup(edict) == COLLISION_GROUP_DEBRIS_TRIGGER;
    if (state.userId == userId && state.cooldownRemaining > 0.0f) return false;

    int original = PlayerCollisionGroup(edict);
    if (original < 0) return false;
    if (original == COLLISION_GROUP_DEBRIS_TRIGGER)
    {
        CInsBot *bot = FindByEdict(edict);
        return bot && bot->m_spawnNoCollideActive;
    }
    return true;
}

bool CBotRoster::AcquireBodyGracePlayer(int clientIndex, int ownerSlot)
{
    if (!CanAcquireBodyGracePlayer(clientIndex, ownerSlot)) return false;
    edict_t *edict = engine->PEntityOfEntIndex(clientIndex);
    IPlayerInfo *info = playerinfomanager->GetPlayerInfo(edict);
    BodyGracePlayerState &state = s_bodyGracePlayer[clientIndex];
    int userId = info->GetUserID();
    unsigned long long ownerBit = ((unsigned long long)1) << ownerSlot;
    if (state.owned)
    {
        state.owners |= ownerBit;
        return true;
    }
    if (state.userId != userId)
    {
        state.userId = userId;
        state.cooldownRemaining = 0.0f;
    }

    int original = PlayerCollisionGroup(edict);
    if (original == COLLISION_GROUP_DEBRIS_TRIGGER)
    {
        // A newly spawned bot can still own the same temporary group. Transfer
        // that ownership instead of saving the temporary value as its original.
        CInsBot *bot = FindByEdict(edict);
        original = bot->m_spawnOriginalCollisionGroup;
        bot->m_spawnNoCollideActive = false;
        bot->m_spawnNoCollideExpired = false;
        bot->m_spawnNoCollideElapsed = 0.0f;
        bot->m_spawnNoCollideRecoveryCheck = 0.0f;
    }
    else if (!WritePlayerCollisionGroup(edict, COLLISION_GROUP_DEBRIS_TRIGGER))
        return false;

    state.owned = true;
    state.userId = userId;
    state.team = info->GetTeamIndex();
    state.originalGroup = original;
    state.owners = ownerBit;
    BotDbg("[insbots] body grace acquired %s (client %d, group %d -> %d)\n",
           info->GetName(), clientIndex, original, COLLISION_GROUP_DEBRIS_TRIGGER);
    return true;
}

void CBotRoster::ReleaseBodyGraceOwner(int ownerSlot,
                                       unsigned long long participants)
{
    if (ownerSlot < 0 || ownerSlot >= 64) return;
    unsigned long long ownerBit = ((unsigned long long)1) << ownerSlot;
    for (int i = 1; i <= 64; i++)
    {
        unsigned long long playerBit = ((unsigned long long)1) << (i - 1);
        if (participants & playerBit)
            s_bodyGracePlayer[i].owners &= ~ownerBit;
    }
}

bool CBotRoster::HasBodyGracePlayer(edict_t *edict) const
{
    if (!edict || edict->IsFree() || !engine || !playerinfomanager) return false;
    int idx = engine->IndexOfEdict(edict);
    if (idx < 1 || idx > 64 || !s_bodyGracePlayer[idx].owned) return false;
    IPlayerInfo *info = playerinfomanager->GetPlayerInfo(edict);
    return info && info->IsConnected()
        && info->GetUserID() == s_bodyGracePlayer[idx].userId
        && PlayerCollisionGroup(edict) == COLLISION_GROUP_DEBRIS_TRIGGER;
}

void CBotRoster::UpdateBodyGracePlayers(bool countTime)
{
    if (!engine || !playerinfomanager || !gpGlobals) return;
    float dt = gpGlobals->interval_per_tick;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;
    for (int i = 1; i <= 64; i++)
    {
        BodyGracePlayerState &state = s_bodyGracePlayer[i];
        edict_t *edict = i <= gpGlobals->maxClients
                       ? engine->PEntityOfEntIndex(i) : 0;
        IPlayerInfo *info = (edict && !edict->IsFree())
                          ? playerinfomanager->GetPlayerInfo(edict) : 0;

        if (!state.owned)
        {
            if (!info || !info->IsConnected() || info->GetUserID() != state.userId)
            {
                state.userId = 0;
                state.cooldownRemaining = 0.0f;
            }
            else if (countTime && state.cooldownRemaining > 0.0f)
            {
                state.cooldownRemaining -= dt;
                if (state.cooldownRemaining < 0.0f) state.cooldownRemaining = 0.0f;
            }
            continue;
        }

        bool invalid = !info || !info->IsConnected()
                    || info->GetUserID() != state.userId;
        bool lifeEnded = invalid || info->IsDead() || info->IsObserver()
                      || info->GetTeamIndex() != state.team;
        int current = PlayerCollisionGroup(edict);
        if (current != COLLISION_GROUP_DEBRIS_TRIGGER)
        {
            // A game-side transition took collision ownership. Respect it.
            state.owned = false;
            state.owners = 0;
            state.cooldownRemaining = 3.0f;
            continue;
        }
        if (!lifeEnded && (state.owners != 0 || BodyGraceHullOverlapsAny(i)))
            continue;

        if (!WritePlayerCollisionGroup(edict, state.originalGroup))
            continue;
        BotDbg("[insbots] body grace restored %s (client %d, group %d)\n",
               info ? info->GetName() : "disconnected player", i,
               state.originalGroup);
        state.owned = false;
        state.owners = 0;
        state.cooldownRemaining = 3.0f;
    }
}

void CBotRoster::ResetBodyGracePlayers(bool restoreGroups)
{
    if (restoreGroups && engine)
    {
        for (int i = 1; i <= 64; i++)
        {
            BodyGracePlayerState &state = s_bodyGracePlayer[i];
            if (!state.owned) continue;
            edict_t *edict = engine->PEntityOfEntIndex(i);
            if (PlayerCollisionGroup(edict) == COLLISION_GROUP_DEBRIS_TRIGGER)
                WritePlayerCollisionGroup(edict, state.originalGroup);
        }
    }
    memset(s_bodyGracePlayer, 0, sizeof(s_bodyGracePlayer));
}

void CBotRoster::BodyGraceClientDisconnect(edict_t *edict)
{
    if (!edict || !engine) return;
    int idx = engine->IndexOfEdict(edict);
    if (idx < 1 || idx > 64) return;
    BodyGracePlayerState &state = s_bodyGracePlayer[idx];
    if (state.owned
        && PlayerCollisionGroup(edict) == COLLISION_GROUP_DEBRIS_TRIGGER)
        WritePlayerCollisionGroup(edict, state.originalGroup);
    memset(&state, 0, sizeof(state));
}

void CInsBot::ResetSpawnNoCollideNavigation(const Vector &pos)
{
    m_stuckSince = 0.0f;
    m_stuckStage = 0;
    m_unstuckUntil = 0.0f;
    m_haveRescueLink = false;
    m_stuckCheckPos = pos;
    m_stuckCheckTime = Now() + 0.5f;
    m_doorYieldUntil = 0.0f;
    m_doorYieldSlot = 0;
    m_chokeProgressTime = 0.0f;
    m_chokeBackoffUntil = 0.0f;
    m_blocked = false;
    m_chokeBackedOut = false;
    m_followSpeed = 0.0f;
    m_passSide = 0.0f;
    m_passUntil = 0.0f;
    m_queueLeaderSlot = 0;
    m_queueWaitSince = 0.0f;
    m_queueBreakUntil = 0.0f;
    m_dropHoldSince = 0.0f;
}

bool CInsBot::SpawnHullOverlapsPlayer(const Vector &pos) const
{
    if (!gpGlobals || !engine || !playerinfomanager || !m_info) return true;

    Vector myMins = pos + m_info->GetPlayerMins();
    Vector myMaxs = pos + m_info->GetPlayerMaxs();
    int maxClients = gpGlobals->maxClients;
    if (maxClients > 65) maxClients = 65;
    for (int i = 1; i <= maxClients; i++)
    {
        edict_t *otherEdict = engine->PEntityOfEntIndex(i);
        if (!otherEdict || otherEdict == m_edict || otherEdict->IsFree()) continue;
        IPlayerInfo *other = playerinfomanager->GetPlayerInfo(otherEdict);
        if (!other || !other->IsConnected() || !other->IsPlayer()
            || other->IsObserver() || other->IsDead())
            continue;

        Vector otherPos = other->GetAbsOrigin();
        Vector otherMins = otherPos + other->GetPlayerMins();
        Vector otherMaxs = otherPos + other->GetPlayerMaxs();
        bool separate = myMaxs.x <= otherMins.x || myMins.x >= otherMaxs.x
                     || myMaxs.y <= otherMins.y || myMins.y >= otherMaxs.y
                     || myMaxs.z <= otherMins.z || myMins.z >= otherMaxs.z;
        if (!separate) return true;
    }
    return false;
}

void CInsBot::BeginSpawnNoCollide(const Vector &pos)
{
    if (!insbot_spawn_nocollide.GetBool() || s_offset_m_CollisionGroup < 0
        || !m_edict || m_edict->IsFree() || HasBodyNoCollide())
        return;
    IServerUnknown *unknown = m_edict->GetUnknown();
    ICollideable *collideable = unknown ? unknown->GetCollideable() : 0;
    if (!collideable) return;

    int original = collideable->GetCollisionGroup();
    if (original == COLLISION_GROUP_DEBRIS_TRIGGER) return;
    m_spawnOriginalCollisionGroup = original;
    m_spawnNoCollideExpired = false;
    m_spawnNoCollideElapsed = 0.0f;
    m_spawnNoCollideRecoveryCheck = 0.0f;
    if (!WriteCollisionGroup(COLLISION_GROUP_DEBRIS_TRIGGER)) return;

    m_spawnNoCollideActive = true;
    ResetSpawnNoCollideNavigation(pos);
    BotDbg("[insbots] %s spawn collision grace started (group %d -> %d)\n",
           m_name, original, COLLISION_GROUP_DEBRIS_TRIGGER);
}

void CInsBot::EndSpawnNoCollide(bool restoreGroup)
{
    if (!m_spawnNoCollideActive) return;
    bool wasExpired = m_spawnNoCollideExpired;
    IServerUnknown *unknown = (m_edict && !m_edict->IsFree())
                            ? m_edict->GetUnknown() : 0;
    ICollideable *collideable = unknown ? unknown->GetCollideable() : 0;
    int current = collideable ? collideable->GetCollisionGroup() : -1;

    if (restoreGroup && current == COLLISION_GROUP_DEBRIS_TRIGGER
        && !WriteCollisionGroup(m_spawnOriginalCollisionGroup))
        return; // retain state and retry; never strand a live bot in grace

    m_spawnNoCollideActive = false;
    m_spawnNoCollideExpired = false;
    m_spawnNoCollideElapsed = 0.0f;
    m_spawnNoCollideRecoveryCheck = 0.0f;
    if (m_info && !wasExpired)
        ResetSpawnNoCollideNavigation(m_info->GetAbsOrigin());
    BotDbg("[insbots] %s spawn collision grace ended (group %d)\n",
           m_name, restoreGroup ? m_spawnOriginalCollisionGroup : current);
}

void CInsBot::UpdateSpawnNoCollide(const Vector &pos, bool countTime)
{
    if (!m_spawnNoCollideActive) return;
    IServerUnknown *unknown = (m_edict && !m_edict->IsFree())
                            ? m_edict->GetUnknown() : 0;
    ICollideable *collideable = unknown ? unknown->GetCollideable() : 0;
    if (!collideable || collideable->GetCollisionGroup() != COLLISION_GROUP_DEBRIS_TRIGGER)
    {
        // A game-side life/state transition changed collision ownership.
        EndSpawnNoCollide(false);
        return;
    }

    if (!m_spawnNoCollideExpired)
        ResetSpawnNoCollideNavigation(pos);
    if (countTime && !m_spawnNoCollideExpired)
    {
        float dt = gpGlobals ? gpGlobals->interval_per_tick : 0.015f;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.1f) dt = 0.1f;
        m_spawnNoCollideElapsed += dt;
    }

    float graceTime = insbot_spawn_nocollide_time.GetFloat();
    if (graceTime < 0.0f) graceTime = 0.0f;
    if (!insbot_spawn_nocollide.GetBool())
        m_spawnNoCollideElapsed = graceTime;
    if (m_spawnNoCollideElapsed < graceTime) return;

    if (!SpawnHullOverlapsPlayer(pos))
    {
        EndSpawnNoCollide(true);
        return;
    }

    if (!m_spawnNoCollideExpired)
    {
        m_spawnNoCollideExpired = true;
        m_spawnNoCollideRecoveryCheck = 0.0f;
        BotDbg("[insbots] %s spawn collision grace expired while overlapping; navigation recovery enabled\n",
               m_name);
    }

    // The timed spawn phase already exceeded every body-grace deadline. If a
    // same-team player is the first swept obstruction, transfer straight into
    // retry state. Enemy overlap deliberately fails FindBodyBlocker and merely
    // drains once the hull is clear.
    float now = Now();
    if (!m_bodyNoCollideActive && !m_bodyNoCollideEscalating
        && now >= m_spawnNoCollideRecoveryCheck)
    {
        m_spawnNoCollideRecoveryCheck = now + 0.5f;
        float yaw = m_headingYaw;
        Vector target = pos + Vector(cosf(yaw * (3.14159265f / 180.0f)),
                                     sinf(yaw * (3.14159265f / 180.0f)), 0) * 48.0f;
        if (m_hasMoveTarget)
        {
            target = m_moveTarget;
            yaw = atan2f(target.y - pos.y, target.x - pos.x)
                * (180.0f / 3.14159265f);
        }
        bool doorway = m_blocked || ChokeAhead(yaw) || DoorAhead(yaw);
        bool teleportEnabled = doorway ? insbot_doorway_teleport.GetBool()
                                       : insbot_unstuck_teleport.GetBool();
        if (teleportEnabled)
            BeginBodyNoCollide(pos, target, doorway, 0.0f);
    }
}

bool CInsBot::HasBodyNoCollide() const
{
    return g_Bots.HasBodyGracePlayer(m_edict);
}

bool CInsBot::FindBodyBlocker(const Vector &pos, const Vector &target,
                              int &blockerClient) const
{
    blockerClient = 0;
    if (!enginetrace || !engine || !playerinfomanager || !gpGlobals
        || !m_edict || !m_info)
        return false;

    Vector delta = target - pos;
    delta.z = 0.0f;
    float len = sqrtf(delta.x * delta.x + delta.y * delta.y);
    if (len < 1.0f) return false;
    delta.x /= len;
    delta.y /= len;

    // A short full-hull sweep proves that a player body, rather than nearby
    // geometry or a player behind a wall, is the first obstruction.
    Vector end = pos + delta * 48.0f;
    Ray_t ray;
    ray.Init(pos, end, m_info->GetPlayerMins(), m_info->GetPlayerMaxs());
    CTraceFilterSkipSelf filter(EdictHandleEntity(m_edict));
    trace_t tr;
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &tr);
    if (tr.fraction >= 1.0f || !tr.m_pEnt) return false;

    int maxClients = gpGlobals->maxClients;
    if (maxClients > 64) maxClients = 64;
    for (int i = 1; i <= maxClients; i++)
    {
        edict_t *edict = engine->PEntityOfEntIndex(i);
        if (!edict || edict == m_edict || edict->IsFree()) continue;
        IServerUnknown *unknown = edict->GetUnknown();
        if (!unknown || tr.m_pEnt != unknown->GetBaseEntity()) continue;

        IPlayerInfo *info = playerinfomanager->GetPlayerInfo(edict);
        if (!info || !info->IsConnected() || !info->IsPlayer()
            || info->IsObserver() || info->IsDead()
            || info->GetTeamIndex() != m_info->GetTeamIndex())
            return false;
        // Only managed InsBots and real human teammates are eligible. An
        // unrelated fake client has no recovery state or lifecycle ownership.
        if (info->IsFakeClient() && !g_Bots.FindByEdict(edict)) return false;
        blockerClient = i;
        return true;
    }
    return false;
}

static bool BodyGracePlayersConnected(IPlayerInfo *a, IPlayerInfo *b)
{
    if (!a || !b) return false;
    Vector ap = a->GetAbsOrigin(), bp = b->GetAbsOrigin();
    Vector amin = ap + a->GetPlayerMins(), amax = ap + a->GetPlayerMaxs();
    Vector bmin = bp + b->GetPlayerMins(), bmax = bp + b->GetPlayerMaxs();
    const float gap = 8.0f;
    bool separate = amax.x + gap < bmin.x || amin.x - gap > bmax.x
                 || amax.y + gap < bmin.y || amin.y - gap > bmax.y
                 || amax.z + gap < bmin.z || amin.z - gap > bmax.z;
    return !separate;
}

bool CInsBot::BeginBodyNoCollide(const Vector &pos, const Vector &target,
                                 bool doorwayRecovery, float duration,
                                 bool ladderRecovery, int confirmedBlocker)
{
    if (duration < 0.0f || m_bodyNoCollideActive || m_bodyNoCollideEscalating
        || (!ladderRecovery && (m_traversingLadder || m_ladderClimbing))
        || (!ladderRecovery && !doorwayRecovery && m_pathResult == BOT_PATH_UNREACHABLE
            && !m_hasMoveTarget && !m_haveRescueLink)
        || !m_edict || !m_info || !engine || !gpGlobals)
        return false;

    // A negative blocker is the ladder geometry-only path: it creates the
    // same durable retry obligation but deliberately grants no collision
    // lease. This keeps safe teleport retries alive across repaths/abandonment
    // without pretending that world geometry was a player blockage.
    bool blockerlessLadder = ladderRecovery && confirmedBlocker < 0;
    bool grantCollision = !blockerlessLadder
                       && insbot_stuck_nocollide.GetBool()
                       && (!ladderRecovery || insbot_ladder_nocollide.GetBool());
    if (!grantCollision && duration > 0.0f && !blockerlessLadder) return false;

    int blockerClient = confirmedBlocker;
    if (!blockerlessLadder && blockerClient <= 0
        && !FindBodyBlocker(pos, target, blockerClient))
        return false;
    int selfClient = engine->IndexOfEdict(m_edict);
    int ownerSlot = (int)(this - g_Bots.m_bots);
    if (selfClient < 1 || selfClient > 64
        || (!blockerlessLadder
            && (blockerClient < 1 || blockerClient > 64))
        || ownerSlot < 0 || ownerSlot >= 64)
        return false;

    unsigned long long participants = grantCollision
        ? (((unsigned long long)1) << (selfClient - 1))
          | (((unsigned long long)1) << (blockerClient - 1))
        : 0;
    int myTeam = m_info->GetTeamIndex();

    // Expand only through body-connected managed InsBots. A human can join
    // only as the directly proven first blocker; unrelated nearby humans are
    // never swept into a collision change. Ladder queues deliberately keep
    // only the direct pair so collision grace cannot leak through waiters.
    bool added = grantCollision && !ladderRecovery;
    while (added)
    {
        added = false;
        for (int b = 0; b < 64; b++)
        {
            CInsBot *candidate = &g_Bots.m_bots[b];
            if (!candidate->m_inUse || !candidate->m_edict || !candidate->m_info
                || candidate->m_info->IsDead()
                || candidate->m_info->GetTeamIndex() != myTeam)
                continue;
            int candidateClient = engine->IndexOfEdict(candidate->m_edict);
            if (candidateClient < 1 || candidateClient > 64) continue;
            unsigned long long candidateBit = ((unsigned long long)1)
                                           << (candidateClient - 1);
            if (participants & candidateBit) continue;
            Vector cp = candidate->m_info->GetAbsOrigin();
            float localDx = cp.x - pos.x, localDy = cp.y - pos.y;
            if (localDx * localDx + localDy * localDy > 160.0f * 160.0f
                || fabsf(cp.z - pos.z) > 72.0f)
                continue;

            for (int i = 1; i <= 64; i++)
            {
                unsigned long long memberBit = ((unsigned long long)1) << (i - 1);
                if (!(participants & memberBit)) continue;
                edict_t *memberEdict = engine->PEntityOfEntIndex(i);
                IPlayerInfo *member = (memberEdict && !memberEdict->IsFree())
                                    ? playerinfomanager->GetPlayerInfo(memberEdict) : 0;
                if (BodyGracePlayersConnected(candidate->m_info, member))
                {
                    CTraceFilterWorldAndPropsOnly worldProps;
                    Ray_t linkRay; trace_t linkTrace;
                    Vector from = cp + Vector(0, 0, 36.0f);
                    Vector to = member->GetAbsOrigin() + Vector(0, 0, 36.0f);
                    linkRay.Init(from, to);
                    enginetrace->TraceRay(linkRay, MASK_PLAYERSOLID,
                                          &worldProps, &linkTrace);
                    if (linkTrace.fraction < 1.0f) continue;
                    participants |= candidateBit;
                    added = true;
                    break;
                }
            }
        }
    }

    for (int i = 1; grantCollision && i <= 64; i++)
    {
        unsigned long long bit = ((unsigned long long)1) << (i - 1);
        if ((participants & bit)
            && !g_Bots.CanAcquireBodyGracePlayer(i, ownerSlot))
            return false;
    }

    unsigned long long acquired = 0;
    for (int i = 1; grantCollision && i <= 64; i++)
    {
        unsigned long long bit = ((unsigned long long)1) << (i - 1);
        if (!(participants & bit)) continue;
        if (!g_Bots.AcquireBodyGracePlayer(i, ownerSlot))
        {
            g_Bots.ReleaseBodyGraceOwner(ownerSlot, acquired);
            g_Bots.UpdateBodyGracePlayers(false);
            return false;
        }
        acquired |= bit;
    }

    m_bodyNoCollideActive = duration > 0.0f;
    m_bodyNoCollideAttempted = true;
    m_bodyNoCollideDoorway = doorwayRecovery;
    m_bodyNoCollideLadder = ladderRecovery;
    m_bodyNoCollideEscalating = duration <= 0.0f;
    m_bodyNoCollideElapsed = 0.0f;
    m_bodyNoCollideDuration = duration;
    m_bodyNoCollideTarget = target;
    float dx = target.x - pos.x, dy = target.y - pos.y;
    float dz = ladderRecovery ? target.z - pos.z : 0.0f;
    m_bodyNoCollideBestDist = sqrtf(dx * dx + dy * dy + dz * dz);
    m_bodyNoCollideParticipants = participants;
    m_bodyNoCollideNextRetry = duration <= 0.0f ? Now() : Now() + 0.5f;
    m_bodyNoCollideRebasePending = false;
    m_bodyNoCollideAbandoned = false;
    m_bodyNoCollideLastFailure = -1;
    m_bodyNoCollideNoSafeFailures = 0;
    m_bodyNoCollideAbandonRemaining = 0.0f;
    if (!doorwayRecovery && !ladderRecovery)
    {
        float stuckStart = m_stuckSince > 0.0f ? m_stuckSince : Now();
        float markTime = insbot_stuck_time.GetFloat();
        if (markTime < 0.2f) markTime = 0.2f;
        float teleportTime = markTime + duration;
        if (m_spawnNoCollideExpired)
            teleportTime = insbot_unstuck_teleport_time.GetFloat();
        float abandonDelay = markTime + 1.8f;
        if (abandonDelay < teleportTime + 1.0f)
            abandonDelay = teleportTime + 1.0f;
        float alreadyStuck = Now() - stuckStart;
        if (alreadyStuck < 0.0f) alreadyStuck = 0.0f;
        m_bodyNoCollideAbandonRemaining = abandonDelay - alreadyStuck;
        if (m_bodyNoCollideAbandonRemaining < 0.0f)
            m_bodyNoCollideAbandonRemaining = 0.0f;
    }

    // Cache the verified edge before any later recovery changes the path.
    if (ladderRecovery && m_ladderFromArea >= 0 && m_ladderToArea >= 0)
    {
        m_rescueFromArea = m_ladderFromArea;
        m_rescueToArea = m_ladderToArea;
        m_haveRescueLink = true;
    }
    else if (g_NavMesh.IsLoaded() && m_pathLen > 0 && m_pathCursor < m_pathLen)
    {
        int here = CurrentArea(pos);
        if (here < 0) here = g_NavMesh.FindNearestArea(pos);
        int next = m_path[m_pathCursor];
        if (here >= 0 && g_NavMesh.HasConnection(here, next))
        {
            m_rescueFromArea = here;
            m_rescueToArea = next;
            m_haveRescueLink = true;
        }
    }

    int count = 0;
    for (int i = 1; i <= 64; i++)
        if (participants & (((unsigned long long)1) << (i - 1))) count++;
    IPlayerInfo *blockerInfo = blockerClient > 0
        ? playerinfomanager->GetPlayerInfo(engine->PEntityOfEntIndex(blockerClient))
        : 0;
    const char *kind = ladderRecovery ? "ladder"
                     : (doorwayRecovery ? "doorway" : "generic");
    BotDbg("[insbots] %s body recovery started (%s, %d participants, blocker=%s%s, retry in %.2fs)\n",
           m_name, kind, count,
           blockerlessLadder ? "none" : (blockerInfo ? blockerInfo->GetName() : "unknown"),
           blockerInfo && !blockerInfo->IsFakeClient() ? ", human" : "",
           duration);
    return true;
}

void CInsBot::ExpandBodyNoCollideCohort(const Vector &pos)
{
    if (m_bodyNoCollideLadder || !insbot_stuck_nocollide.GetBool()
        || !m_bodyNoCollideParticipants
        || !engine || !playerinfomanager || !enginetrace || !m_info)
        return;

    int ownerSlot = (int)(this - g_Bots.m_bots);
    int myTeam = m_info->GetTeamIndex();
    bool added = true;
    while (added)
    {
        added = false;
        for (int b = 0; b < 64; b++)
        {
            CInsBot *candidate = &g_Bots.m_bots[b];
            if (!candidate->m_inUse || !candidate->m_edict || !candidate->m_info
                || candidate->m_info->IsDead()
                || candidate->m_info->GetTeamIndex() != myTeam)
                continue;
            int candidateClient = engine->IndexOfEdict(candidate->m_edict);
            if (candidateClient < 1 || candidateClient > 64) continue;
            unsigned long long candidateBit = ((unsigned long long)1)
                                           << (candidateClient - 1);
            if (m_bodyNoCollideParticipants & candidateBit) continue;
            Vector cp = candidate->m_info->GetAbsOrigin();
            float dx = cp.x - pos.x, dy = cp.y - pos.y;
            if (dx * dx + dy * dy > 160.0f * 160.0f
                || fabsf(cp.z - pos.z) > 72.0f)
                continue;

            bool connected = false;
            for (int i = 1; i <= 64 && !connected; i++)
            {
                unsigned long long memberBit = ((unsigned long long)1) << (i - 1);
                if (!(m_bodyNoCollideParticipants & memberBit)) continue;
                edict_t *memberEdict = engine->PEntityOfEntIndex(i);
                IPlayerInfo *member = (memberEdict && !memberEdict->IsFree())
                                    ? playerinfomanager->GetPlayerInfo(memberEdict) : 0;
                if (!BodyGracePlayersConnected(candidate->m_info, member)) continue;

                CTraceFilterWorldAndPropsOnly worldProps;
                Ray_t ray; trace_t tr;
                Vector from = cp + Vector(0, 0, 36.0f);
                Vector to = member->GetAbsOrigin() + Vector(0, 0, 36.0f);
                ray.Init(from, to);
                enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &worldProps, &tr);
                connected = tr.fraction >= 1.0f;
            }
            if (!connected || !g_Bots.AcquireBodyGracePlayer(candidateClient, ownerSlot))
                continue;

            m_bodyNoCollideParticipants |= candidateBit;
            added = true;
            BotDbg("[insbots] %s body recovery added cohort bot %s with existing deadline\n",
                   m_name, candidate->m_name);
        }
    }
}

void CInsBot::EndBodyNoCollide(bool succeeded)
{
    if (m_bodyNoCollideParticipants)
    {
        int ownerSlot = (int)(this - g_Bots.m_bots);
        g_Bots.ReleaseBodyGraceOwner(ownerSlot, m_bodyNoCollideParticipants);
    }
    BotDbg("[insbots] %s body recovery %s after %.2f grace seconds\n",
           m_name, succeeded ? "completed" : "ended", m_bodyNoCollideElapsed);
    m_bodyNoCollideActive = false;
    m_bodyNoCollideElapsed = 0.0f;
    m_bodyNoCollideDuration = 0.0f;
    m_bodyNoCollideParticipants = 0;
}

void CInsBot::CompleteBodyNoCollideTeleport(const char *source)
{
    if (!m_bodyNoCollideActive && !m_bodyNoCollideEscalating
        && !m_bodyNoCollideAttempted && !m_bodyNoCollideParticipants)
        return;

    BotDbg("[insbots] %s body recovery completed by %s\n",
           m_name, source ? source : "validated teleport");
    EndBodyNoCollide(true);
    m_bodyNoCollideActive = false;
    m_bodyNoCollideAttempted = false;
    m_bodyNoCollideDoorway = false;
    m_bodyNoCollideLadder = false;
    m_bodyNoCollideEscalating = false;
    m_bodyNoCollideBestDist = 0.0f;
    m_bodyNoCollideNextRetry = 0.0f;
    m_bodyNoCollideAbandonRemaining = 0.0f;
    m_bodyNoCollideRebasePending = false;
    m_bodyNoCollideAbandoned = false;
    m_bodyNoCollideLastFailure = -1;
    m_bodyNoCollideNoSafeFailures = 0;
    m_haveRescueLink = false;
}

void CInsBot::ResetBodyNoCollideRecovery()
{
    if (m_bodyNoCollideParticipants || m_bodyNoCollideActive
        || m_bodyNoCollideEscalating)
        EndBodyNoCollide(false);
    m_bodyNoCollideActive = false;
    m_bodyNoCollideAttempted = false;
    m_bodyNoCollideDoorway = false;
    m_bodyNoCollideLadder = false;
    m_bodyNoCollideEscalating = false;
    m_bodyNoCollideElapsed = 0.0f;
    m_bodyNoCollideDuration = 0.0f;
    m_bodyNoCollideBestDist = 0.0f;
    m_bodyNoCollideNextRetry = 0.0f;
    m_bodyNoCollideAbandonRemaining = 0.0f;
    m_bodyNoCollideParticipants = 0;
    m_bodyNoCollideRebasePending = false;
    m_bodyNoCollideAbandoned = false;
    m_bodyNoCollideLastFailure = -1;
    m_bodyNoCollideNoSafeFailures = 0;
}

void CInsBot::AbandonStuckObjective(const Vector &pos, float now)
{
    int abandonedGoal = m_objGoal;
    if (abandonedGoal >= 0)
    {
        m_avoidObj = abandonedGoal;
        m_avoidUntil = now + 30.0f;
    }
    m_stuckSince = 0.0f;
    m_stuckStage = 0;
    m_bestDist = 3.4e38f;
    m_navDestChanged = false;
    if (g_NavMesh.IsLoaded()) m_goalArea = g_NavMesh.RandomArea();
    ResetDoorwayRunway();

    m_pathLen = 0;
    m_pathResult = BOT_PATH_NONE;
    m_pathContinues = false;
    m_pathCursor = 0;
    m_repathTime = 0;
    m_objGoal = -1;
                m_haveAdvanceTarget = false; m_advanceTargetRefresh = 0.0f;
    m_objRepick = now;
    m_holdUntil = 0;
    m_haveHoldSpot = false;
    m_bodyNoCollideAbandoned = true;
    m_bodyNoCollideRebasePending = m_bodyNoCollideAttempted;
    if (!m_bodyNoCollideAttempted) m_haveRescueLink = false;
    const char *goalLabel = (abandonedGoal >= 0) ? "objective"
                          : (abandonedGoal == PICK_ADVANCE ? "ADVANCE" : "none");
    BotDbg("[insbots] %s stuck recovery abandoned goal %s (goal=%d) at (%.0f %.0f %.0f); teleport obligation retained\n",
           m_name, goalLabel, abandonedGoal, pos.x, pos.y, pos.z);
}

bool CInsBot::UpdateBodyNoCollide(const Vector &pos, float now)
{
    if (!m_bodyNoCollideActive && !m_bodyNoCollideEscalating) return false;

    float activeDt = gpGlobals ? gpGlobals->interval_per_tick : 0.015f;
    if (activeDt < 0.0f) activeDt = 0.0f;
    if (activeDt > 0.1f) activeDt = 0.1f;
    if (!m_bodyNoCollideDoorway && !m_bodyNoCollideLadder
        && !m_bodyNoCollideAbandoned
        && m_bodyNoCollideAbandonRemaining > 0.0f)
    {
        m_bodyNoCollideAbandonRemaining -= activeDt;
        if (m_bodyNoCollideAbandonRemaining < 0.0f)
            m_bodyNoCollideAbandonRemaining = 0.0f;
    }

    // The body-grace toggle controls collision only. Turning it off releases
    // current leases to clear-hull drain but does not erase the independent
    // teleport deadline.
    bool ladderEnemy = m_bodyNoCollideLadder
        && EnemyNearLadder(g_NavMesh.Ladder(m_ladderClimbIdx));
    if ((!insbot_stuck_nocollide.GetBool()
         || (m_bodyNoCollideLadder && !insbot_ladder_nocollide.GetBool())
         || ladderEnemy)
        && m_bodyNoCollideParticipants)
    {
        int ownerSlot = (int)(this - g_Bots.m_bots);
        g_Bots.ReleaseBodyGraceOwner(ownerSlot, m_bodyNoCollideParticipants);
        m_bodyNoCollideParticipants = 0;
        BotDbg("[insbots] %s body collision grace draining (%s); teleport obligation retained\n",
               m_name, ladderEnemy ? "enemy entered ladder envelope" : "disabled");
    }
    if (m_bodyNoCollideParticipants && now >= m_bodyNoCollideNextRetry)
    {
        ExpandBodyNoCollideCohort(pos);
        if (m_bodyNoCollideActive)
            m_bodyNoCollideNextRetry = now + 0.5f;
    }

    if (!m_bodyNoCollideLadder && m_bodyNoCollideRebasePending
        && g_NavMesh.IsLoaded()
        && m_pathLen > 0 && m_pathCursor >= 0 && m_pathCursor < m_pathLen)
    {
        int here = CurrentArea(pos);
        if (here < 0) here = g_NavMesh.FindNearestArea(pos);
        int next = m_path[m_pathCursor];
        if (here >= 0 && g_NavMesh.HasConnection(here, next))
        {
            m_rescueFromArea = here;
            m_rescueToArea = next;
            m_haveRescueLink = true;
            if (m_haveNavDest) m_bodyNoCollideTarget = m_navDest;
            float rdx = m_bodyNoCollideTarget.x - pos.x;
            float rdy = m_bodyNoCollideTarget.y - pos.y;
            m_bodyNoCollideBestDist = sqrtf(rdx * rdx + rdy * rdy);
            m_bodyNoCollideRebasePending = false;
            m_bodyNoCollideLastFailure = -1;
            m_bodyNoCollideNoSafeFailures = 0;
            BotDbg("[insbots] %s body recovery rebased onto NAV edge %d -> %d\n",
                   m_name, here, next);
        }
    }

    float dx = m_bodyNoCollideTarget.x - pos.x;
    float dy = m_bodyNoCollideTarget.y - pos.y;
    float dz = m_bodyNoCollideLadder ? m_bodyNoCollideTarget.z - pos.z : 0.0f;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    float progress = insbot_stuck_dist.GetFloat();
    if (progress < 1.0f) progress = 1.0f;
    if (dist <= 4.0f || dist < m_bodyNoCollideBestDist - progress)
    {
        bool wasLadderRecovery = m_bodyNoCollideLadder;
        EndBodyNoCollide(true);
        m_bodyNoCollideAttempted = false;
        m_bodyNoCollideEscalating = false;
        m_bodyNoCollideDoorway = false;
        m_bodyNoCollideLadder = false;
        m_bodyNoCollideAbandonRemaining = 0.0f;
        m_bodyNoCollideRebasePending = false;
        m_bodyNoCollideBestDist = dist;
        m_stuckSince = 0.0f;
        m_stuckStage = 0;
        m_haveRescueLink = false;
        m_chokeProgressPos = pos;
        m_chokeProgressTime = now;
        m_blocked = false;
        m_chokeBackedOut = false;
        m_chokeBackoffUntil = 0.0f;
        if (wasLadderRecovery && m_ladderPhase == LADDER_PHASE_RECOVERY)
        {
            const NavLadderData *ladder = g_NavMesh.Ladder(m_ladderClimbIdx);
            Vector landing = ladder
                ? LadderAreaInterior(m_ladderToArea,
                    m_ladderClimbUp ? ladder->top : ladder->bottom) : pos;
            float endHeight = m_ladderClimbUp && ladder
                            ? ladder->top.z : landing.z;
            bool atLip = ladder && (m_ladderClimbUp
                ? pos.z >= endHeight : pos.z <= endHeight);
            m_ladderPhase = atLip ? LADDER_PHASE_DISMOUNT
                                  : LADDER_PHASE_ATTACHED;
        }
        return false;
    }
    if (m_bodyNoCollideActive)
    {
        m_bodyNoCollideElapsed += activeDt;
        if (m_bodyNoCollideElapsed < m_bodyNoCollideDuration) return false;

        m_bodyNoCollideActive = false;
        m_bodyNoCollideEscalating = true;
        m_bodyNoCollideNextRetry = now;
        BotDbg("[insbots] %s body grace expired; %s teleport retry now owed\n",
               m_name, m_bodyNoCollideLadder ? "ladder"
                     : (m_bodyNoCollideDoorway ? "doorway" : "generic"));
    }

    bool enabled = m_bodyNoCollideDoorway
        ? insbot_doorway_teleport.GetBool() : insbot_unstuck_teleport.GetBool();
    if (!enabled)
    {
        if (m_bodyNoCollideLastFailure != TELEPORT_DISABLED)
            BotDbg("[insbots] %s body teleport obligation ended: administrator disabled %s teleport\n",
                   m_name, m_bodyNoCollideLadder ? "ladder"
                         : (m_bodyNoCollideDoorway ? "doorway" : "generic"));
        ResetBodyNoCollideRecovery();
        m_stuckSince = 0.0f;
        m_stuckStage = 0;
        m_haveRescueLink = false;
        m_chokeProgressPos = pos;
        m_chokeProgressTime = now;
        m_blocked = false;
        m_chokeBackedOut = false;
        m_chokeBackoffUntil = 0.0f;
        return false;
    }

    if (!m_bodyNoCollideDoorway && !m_bodyNoCollideLadder
        && !m_bodyNoCollideAbandoned
        && m_bodyNoCollideAbandonRemaining <= 0.0f)
        AbandonStuckObjective(pos, now);

    if (!m_bodyNoCollideEscalating || now < m_bodyNoCollideNextRetry) return false;
    int failureReason = TELEPORT_OK;
    bool teleported = TeleportUnstuck(pos, now, m_bodyNoCollideDoorway,
                                      &failureReason,
                                      m_bodyNoCollideNoSafeFailures >= 2);
    if (teleported)
    {
        CompleteBodyNoCollideTeleport(
            m_bodyNoCollideLadder ? "ladder teleport"
                : (m_bodyNoCollideDoorway ? "doorway teleport" : "generic teleport"));
        return true;
    }
    if (failureReason == TELEPORT_NO_SAFE_DESTINATION)
        m_bodyNoCollideNoSafeFailures++;
    else
        m_bodyNoCollideNoSafeFailures = 0;
    if (failureReason != m_bodyNoCollideLastFailure)
    {
        BotDbg("[insbots] %s %s teleport retry blocked: %s\n",
               m_name, m_bodyNoCollideLadder ? "ladder"
                     : (m_bodyNoCollideDoorway ? "doorway" : "generic"),
               TeleportFailureName(failureReason));
        m_bodyNoCollideLastFailure = failureReason;
    }
    m_bodyNoCollideNextRetry = now + 0.5f;
    return false;
}
// Per-frame cached nav-area lookup. FindArea with its off-mesh fallback (and
// world traces) is the most expensive per-bot operation on large meshes, and
// several subsystems ask for the same position in the same frame. Results
// depend only on static geometry, so same-frame + same-position reuse is
// exact; movement beyond ~4u or a new game frame recomputes.
int CInsBot::CurrentArea(const Vector &pos) const
{
    if (!gpGlobals) return g_NavMesh.FindArea(pos);
    float now = gpGlobals->curtime;
    float dx = pos.x - m_areaCachePos.x;
    float dy = pos.y - m_areaCachePos.y;
    float dz = pos.z - m_areaCachePos.z;
    if (m_areaCache >= 0 && m_areaCacheTime == now
        && dx * dx + dy * dy + dz * dz <= 16.0f)
        return m_areaCache;
    m_areaCacheTime = now;
    m_areaCachePos = pos;
    m_areaCache = g_NavMesh.FindArea(pos);
    return m_areaCache;
}


bool CInsBot::Create(const char *nameOrNull, int teamSlot, int requestedTeamArg,
                     int requestedRoleId)
{
    char name[64];
    if (nameOrNull && nameOrNull[0])
    {
        _snprintf(name, sizeof(name), "%s", nameOrNull);
        teamSlot = GetTeamSlotForBotName(nameOrNull, teamSlot);
    }
    else
    {
        int team = TeamSlotToTeamIndex(teamSlot);
        int fac = (team == 1 || team == 2) ? g_teamFaction[team] : FACTION_UNKNOWN;
        if (fac == FACTION_UNKNOWN)
            fac = (team == 1) ? FACTION_USMC : FACTION_IRAQI;

        if (fac == FACTION_USMC)
        {
            static int s_usmcCursor = 0;
            _snprintf(name, sizeof(name), "[BOT] %s",
                      s_BotNamesUSMC[(s_usmcCursor++) % (sizeof(s_BotNamesUSMC) / sizeof(s_BotNamesUSMC[0]))]);
        }
        else
        {
            static int s_iraqiCursor = 0;
            _snprintf(name, sizeof(name), "[BOT] %s",
                      s_BotNamesIraqi[(s_iraqiCursor++) % (sizeof(s_BotNamesIraqi) / sizeof(s_BotNamesIraqi[0]))]);
        }
    }
    name[sizeof(name) - 1] = 0;

    edict_t *e = botmanager->CreateBot(name);
    if (!e) return false;

    memset(this, 0, sizeof(*this));
    m_primaryWeaponName[0] = 0;
    m_reloadingClass = -1;
    m_sidearmTemporary = false;
    m_sniperHoldUntil = 0.0f;
    m_dropHoldExpired = false;
    m_avoidObj = -1;
    m_sniperArea = -1;
    m_sniperObj = -1;
    m_areaCache = -1;
    m_areaCacheTime = -1.0f;
    m_routeRemaining = 3.4e38f;
    m_edict = e;
    m_ctrl  = botmanager->GetBotController(e);
    m_info  = playerinfomanager->GetPlayerInfo(e);
    m_inUse = true;
    m_teamSlot = teamSlot;
    m_requestedTeamArg = requestedTeamArg;
    m_requestedRoleId = requestedRoleId;
    _snprintf(m_name, sizeof(m_name), "%s", name);
    m_state = BOT_JOINING;
    m_ordinal = (int)(this - g_Bots.m_bots);
    // A newly created client gets its full setup on the next frame. Waiting a
    // whole join interval can miss the short setup opportunity after a round
    // has already started. Quota batches are still held by g_HoldQuotaJoins.
    m_nextJoinCmdTime = (requestedTeamArg >= 0)
                      ? Now() : Now() + insbot_join_interval.GetFloat();
    m_joinCmdIndex = 0;
    m_enemyIndex = 0;
    m_goalArea = -1;
    m_headingYaw = frand(-180.0f, 180.0f);
    m_spawnTime = Now();
    static int s_seed = 0; m_pathSeed = ++s_seed;   // unique per bot: route diversity
    m_captureCommit = false;
    m_captureObj = -1;
    m_captureTeam = 0;
    m_haveAdvanceTarget = false; m_advanceTargetRefresh = 0.0f;
    m_ladderClimbIdx = -1;
    m_ladderFromArea = m_ladderToArea = -1;
    return (m_ctrl && m_info);
}

void CInsBot::Adopt(edict_t *e, int teamSlot)
{
    IPlayerInfo *pi = playerinfomanager->GetPlayerInfo(e);
    memset(this, 0, sizeof(*this));
    m_primaryWeaponName[0] = 0;
    m_reloadingClass = -1;
    m_sidearmTemporary = false;
    m_sniperHoldUntil = 0.0f;
    m_dropHoldExpired = false;
    m_avoidObj = -1;
    m_sniperArea = -1;
    m_sniperObj = -1;
    m_areaCache = -1;
    m_areaCacheTime = -1.0f;
    m_routeRemaining = 3.4e38f;
    m_edict = e;
    m_ctrl  = botmanager->GetBotController(e);
    m_info  = pi;
    m_inUse = true;
    m_teamSlot = teamSlot;
    m_requestedTeamArg = -1;
    m_requestedRoleId = -1;
    _snprintf(m_name, sizeof(m_name), "%s", pi ? pi->GetName() : "[BOT]");
    m_name[sizeof(m_name) - 1] = 0;
    m_state = BOT_JOINING;
    m_ordinal = (int)(this - g_Bots.m_bots);
    m_nextJoinCmdTime = Now() + insbot_join_interval.GetFloat();
    m_headingYaw = frand(-180.0f, 180.0f);
    m_goalArea = -1;
    m_spawnTime = Now();
    static int s_seed = 5000; m_pathSeed = ++s_seed;
    m_captureCommit = false;
    m_captureObj = -1;
    m_captureTeam = 0;
    m_haveAdvanceTarget = false; m_advanceTargetRefresh = 0.0f;
    m_ladderClimbIdx = -1;
    m_ladderFromArea = m_ladderToArea = -1;
}

void CInsBot::OnRemoved()
{
    ResetLadderTraversal(true);
    ResetBodyNoCollideRecovery();
    EndSpawnNoCollide(true);
    m_inUse = false;
    m_edict = 0; m_ctrl = 0; m_info = 0;
    m_captureCommit = false;
    m_captureObj = -1;
    m_captureTeam = 0;
    m_haveAdvanceTarget = false; m_advanceTargetRefresh = 0.0f;
}

Vector CInsBot::EyePos() const
{
    Vector o = m_info->GetAbsOrigin();
    o.z += 60.0f;                       // standing eye approximation
    return o;
}

Vector CInsBot::FireEyePos(bool crouched) const
{
    Vector o = m_info->GetAbsOrigin();
    float height = 60.0f;               // keep the established standing approximation
    if (crouched)
    {
        height = insbot_crouch_eye_height.GetFloat();
        // A bad live setting must not move the firing origin below the player
        // or turn a crouch check into another standing-height check.
        if (height < 20.0f) height = 20.0f;
        if (height > 60.0f) height = 60.0f;
    }
    o.z += height;
    return o;
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

void CInsBot::SendJoinCommands()
{
    // Newly created bots use pfullsetup directly. Sending pteamsetup first can
    // finalize a fresh client with an engine-selected class before our later
    // class request is processed. pfullsetup is the game's one-shot team +
    // squad-slot command, so repeat it once and only then request reinforcement.
    // Automatic adds rotate valid role IDs on each retry cycle; exact-slot adds
    // retain their requested role.
    if (m_requestedTeamArg >= 0)
    {
        char cmd[64];
        int roleId = (m_requestedRoleId > 0)
                   ? m_requestedRoleId : JoinRoleId(m_ordinal, m_joinCycles);
        UpdateCustomizationWeapon(roleId);
        if (m_joinCmdIndex == 0 || m_joinCmdIndex == 1)
            _snprintf(cmd, sizeof(cmd), "pfullsetup %d %d",
                      m_requestedTeamArg, roleId);
        else if (m_joinCmdIndex == 2)
            _snprintf(cmd, sizeof(cmd), "creinforce");
        else
        {
            m_joinCmdIndex = 0;
            m_joinCycles++;
            return;
        }

        cmd[sizeof(cmd) - 1] = 0;
        if (CommandContainsToken(cmd, "pfullsetup")
            || CommandContainsToken(cmd, "creinforce"))
            SendCustomizationBeforeSpawn();
        helpers->ClientCommand(m_edict, cmd);
        BotDbg("[insbots] %s -> targeted join cmd '%s'\n", m_name, cmd);
        m_joinCmdIndex++;
        return;
    }

    const char *list = (m_teamSlot == 0) ? insbot_join_cmds_a.GetString()
                                         : insbot_join_cmds_b.GetString();
    if (!list || !list[0]) return;

    // split by ';', pick command m_joinCmdIndex
    char buf[512];
    _snprintf(buf, sizeof(buf), "%s", list);
    buf[sizeof(buf) - 1] = 0;

    int idx = 0;
    char *tok = strtok(buf, ";");
    while (tok && idx < m_joinCmdIndex) { tok = strtok(0, ";"); idx++; }

    if (!tok)
    {
        m_joinCmdIndex = 0;      // wrap: start sequence over (menus may re-prompt)
        m_joinCycles++;
        return;
    }

    // strip leading spaces; substitute tokens:
    //   {n} -> per-side ordinal (fixed per bot: 0,1,2,..)
    //   {c} -> loop cycle counter (increments each time the list wraps)
    //   {s} -> legacy sequential slot sweep
    //   {f} -> faction argument used by pfullsetup (0 USMC, 1 Iraqi)
    //   {r} -> one of the game's 16 valid sparse role IDs
    while (*tok == ' ') tok++;
    char cmd[256]; int w = 0;
    for (const char *r = tok; *r && w < 250; r++)
    {
        if (r[0] == '{' && r[2] == '}')
        {
            int val = -1;
            if (r[1] == 'n') val = m_ordinal;
            else if (r[1] == 'c') val = m_joinCycles;
            else if (r[1] == 's')
            {
                // sweep squad slots, wrapping, so every bot keeps cycling through
                // valid slots (0..sweep-1) instead of climbing past them and
                // stalling unspawned at origin.
                int sweep = insbot_slot_sweep.GetInt();
                if (sweep < 1) sweep = 1;
                val = (m_ordinal + m_joinCycles) % sweep;
            }
            else if (r[1] == 'f') val = (m_requestedRoleId > 0)
                                             ? m_requestedTeamArg
                                             : JoinFactionArg(m_teamSlot);
            else if (r[1] == 'r')
            {
                val = (m_requestedRoleId > 0)
                    ? m_requestedRoleId
                    : JoinRoleId(m_ordinal, m_joinCycles);
                UpdateCustomizationWeapon(val);
            }
            if (val >= 0)
            {
                // Clamp to the remaining buffer: the old fixed 8-byte window
                // could write one byte past cmd[] when w was near the limit.
                int n = _snprintf(cmd + w, sizeof(cmd) - w, "%d", val);
                if (n > 0) w += n;
                r += 2;
                continue;
            }
        }
        cmd[w++] = *r;
    }
    cmd[w] = 0;

    if (cmd[0])
    {
        const char *setupStr = strstr(cmd, "pfullsetup");
        if (setupStr)
        {
            int fArg = -1, rArg = -1;
            if (sscanf(setupStr, "pfullsetup %d %d", &fArg, &rArg) == 2 && rArg > 0)
            {
                UpdateCustomizationWeapon(rArg);
            }
        }
        if (CommandContainsToken(cmd, "pfullsetup")
            || CommandContainsToken(cmd, "creinforce"))
            SendCustomizationBeforeSpawn();
        helpers->ClientCommand(m_edict, cmd);
        BotDbg("[insbots] %s -> join cmd '%s'\n", m_name, cmd);
    }
    m_joinCmdIndex++;
}

void CInsBot::ThinkJoin()
{
    float now = Now();

    // A classed bot may already have become alive while the startup batch is
    // held; let it transition normally without sending another setup command.
    if (m_info->GetTeamIndex() > 0 && !m_info->IsDead())
    {
        m_state = BOT_ALIVE;
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
        m_stuckCheckPos = m_info->GetAbsOrigin();
        m_stuckCheckTime = Now() + 1.0f;
        m_joinWarned = false;
        BotDbg("[insbots] %s spawned (team %d)\n", m_name, m_info->GetTeamIndex());
        return;
    }

    if (g_HoldQuotaJoins)
    {
        // Keep every staged bot on the same first-command boundary. Once the
        // quota is complete, their team/class sequence advances in lockstep.
        m_nextJoinCmdTime = now;
        return;
    }

    if (now >= m_nextJoinCmdTime)
    {
        float interval = insbot_join_interval.GetFloat();
        if (interval < 0.05f) interval = 0.05f;
        if (m_requestedTeamArg >= 0 && interval < 0.25f) interval = 0.25f;
        m_nextJoinCmdTime = now + interval;
        SendJoinCommands();
    }

    // are we in the game yet?
    // couldn't get into a squad in time (typical mid-round, when the team/squad
    // setup window is closed) - kick and let the quota retry at a better time,
    // so bots don't pile up unspawned at the origin.
    float timeout = insbot_join_timeout.GetFloat();
    if (timeout > 0.0f && now - m_spawnTime > timeout)
    {
        int uid = engine->GetPlayerUserId(m_edict);
        if (uid >= 0)
        {
            char c[64];
            _snprintf(c, sizeof(c), "kickid %d\n", uid);
            engine->ServerCommand(c);
        }
        Msg("[insbots] %s couldn't join in %.0fs - kicked (quota will retry)\n", m_name, timeout);
        OnRemoved();
        return;
    }

    if (!m_joinWarned && now - m_spawnTime > 25.0f)
    {
        m_joinWarned = true;
        Msg("[insbots] %s still joining after 25s (team=%d). If this persists, the squad setup window may be closed mid-round; bots usually get in at the next round.\n",
            m_name, m_info->GetTeamIndex());
    }
}

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
// two area rectangles do not touch, so GetPortalMerged cannot return a shared
// edge. Treat those one-axis, short-gap links as virtual portals. This gives
// steering an actual mouth to line up with instead of aiming diagonally at the
// landing area's center through the wall beside the opening.
static bool GetSeparatedTransitionPortal(int fromIdx, int toIdx,
                                         Vector &mid, Vector &axis,
                                         float &halfSpan, Vector *insideDest)
{
    const NavAreaData *from = g_NavMesh.Area(fromIdx);
    const NavAreaData *to = g_NavMesh.Area(toIdx);
    if (!from || !to) return false;
    // Keep this repair scoped to the verified malformed Haditha link. Applying
    // synthetic takeoff points to every map's no-portal jump/drop changes
    // traversal semantics for links whose BSP geometry has not been checked.
    if (!IsHadithaMapName(g_Bots.m_mapname)
        || from->id != 464 || to->id != 3321)
        return false;

    float overlapLo, overlapHi, gap, destDepth;
    Vector travel(0.0f, 0.0f, 0.0f);
    if (from->hi.y < to->lo.y || to->hi.y < from->lo.y)
    {
        overlapLo = from->lo.x > to->lo.x ? from->lo.x : to->lo.x;
        overlapHi = from->hi.x < to->hi.x ? from->hi.x : to->hi.x;
        if (overlapHi <= overlapLo) return false;
        axis = Vector(1.0f, 0.0f, 0.0f);
        mid.x = (overlapLo + overlapHi) * 0.5f;
        if (from->hi.y < to->lo.y)
        {
            gap = to->lo.y - from->hi.y;
            travel.y = 1.0f;
            mid.y = from->hi.y;
            destDepth = (to->hi.y - to->lo.y) * 0.5f;
            if (destDepth > 18.0f) destDepth = 18.0f;
            if (insideDest) *insideDest = Vector(mid.x, to->lo.y + destDepth, 0.0f);
        }
        else
        {
            gap = from->lo.y - to->hi.y;
            travel.y = -1.0f;
            mid.y = from->lo.y;
            destDepth = (to->hi.y - to->lo.y) * 0.5f;
            if (destDepth > 18.0f) destDepth = 18.0f;
            if (insideDest) *insideDest = Vector(mid.x, to->hi.y - destDepth, 0.0f);
        }
    }
    else if (from->hi.x < to->lo.x || to->hi.x < from->lo.x)
    {
        overlapLo = from->lo.y > to->lo.y ? from->lo.y : to->lo.y;
        overlapHi = from->hi.y < to->hi.y ? from->hi.y : to->hi.y;
        if (overlapHi <= overlapLo) return false;
        axis = Vector(0.0f, 1.0f, 0.0f);
        mid.y = (overlapLo + overlapHi) * 0.5f;
        if (from->hi.x < to->lo.x)
        {
            gap = to->lo.x - from->hi.x;
            travel.x = 1.0f;
            mid.x = from->hi.x;
            destDepth = (to->hi.x - to->lo.x) * 0.5f;
            if (destDepth > 18.0f) destDepth = 18.0f;
            if (insideDest) *insideDest = Vector(to->lo.x + destDepth, mid.y, 0.0f);
        }
        else
        {
            gap = from->lo.x - to->hi.x;
            travel.x = -1.0f;
            mid.x = from->lo.x;
            destDepth = (to->hi.x - to->lo.x) * 0.5f;
            if (destDepth > 18.0f) destDepth = 18.0f;
            if (insideDest) *insideDest = Vector(to->hi.x - destDepth, mid.y, 0.0f);
        }
    }
    else
        return false; // overlapping/stacked or diagonal: no unambiguous mouth

    // Only bridge the small discontinuities nav_generate uses for jumps and
    // drops. A longer missing edge should remain visible to stuck recovery.
    if (gap <= 0.0f || gap > 64.0f) return false;
    halfSpan = (overlapHi - overlapLo) * 0.5f;
    if (halfSpan < 8.0f || halfSpan * 2.0f >= 72.0f) return false;
    mid.z = g_NavMesh.GetZ(fromIdx, mid.x, mid.y);
    if (insideDest)
        insideDest->z = g_NavMesh.GetZ(toIdx, insideDest->x, insideDest->y);
    Vector landingProbe;
    if (insideDest) landingProbe = *insideDest;
    else
    {
        landingProbe = to->center;
        landingProbe.z = g_NavMesh.GetZ(toIdx, landingProbe.x, landingProbe.y);
    }
    // This correction is for tight one-way drops like Haditha's lower-level
    // doorway. Flat gaps and upward jumps need different takeoff physics and
    // retain the ordinary fallback/recovery behavior.
    if (mid.z - landingProbe.z < 24.0f) return false;
    return true;
}

static bool PlayerInsideObjective(const ObjPoint *op, IPlayerInfo *info,
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

static Vector ObjectiveHoldPoint(const ObjPoint *op, const Vector &pos,
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
static Vector ObjectiveCornerHoldPoint(const ObjPoint *op, const Vector &entry,
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
static float OpenHoldYaw(const Vector &pos, float preferredYaw)
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

static bool IsHadithaTightDoorEdge(int fromIdx, int toIdx)
{
    if (!IsHadithaMapName(g_Bots.m_mapname)) return false;
    const NavAreaData *from = g_NavMesh.Area(fromIdx);
    const NavAreaData *to = g_NavMesh.Area(toIdx);
    if (!from || !to) return false;
    unsigned int a = from->id, b = to->id;
    return (a == 800  && b == 464)  || (a == 464  && b == 800)
        || (a == 464  && b == 3274) || (a == 3274 && b == 464)
        || (a == 3274 && b == 235)  || (a == 235  && b == 3274)
        || (a == 464  && b == 3321)
        || (a == 3321 && b == 463);
}

// The second reported choke is the 25u corner immediately before the broad
// id320 room. Depending on which of the two tiny source tiles FindArea picks,
// the active edge can be the short south step or either eastward exit.
static bool IsHadithaEastDoorEdge(int fromIdx, int toIdx)
{
    if (!IsHadithaMapName(g_Bots.m_mapname)) return false;
    const NavAreaData *from = g_NavMesh.Area(fromIdx);
    const NavAreaData *to = g_NavMesh.Area(toIdx);
    if (!from || !to) return false;
    unsigned int a = from->id, b = to->id;
    return (a == 3295 && b == 3315)
        || (a == 3295 && b == 320)
        || (a == 3315 && b == 320);
}

// Exact FindArea selection can legitimately choose a third overlapping tile
// at a generated terrain seam. Keep the current path while the player's feet
// are still on either surface of its active edge; this prevents one ambiguous
// lookup from discarding and rebuilding an otherwise valid slope route.
static bool PositionOnAreaSurface(int areaIdx, const Vector &pos)
{
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
static bool PositionOnLadderLanding(int areaIdx, const Vector &pos)
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
    if (!PositionOnAreaSurface(toIdx, pos)) return false;
    const NavAreaData *from = g_NavMesh.Area(fromIdx);
    const NavAreaData *to = g_NavMesh.Area(toIdx);
    if (!from || !to) return false;
    Vector portal;
    if (!g_NavMesh.GetPortal(fromIdx, toIdx, portal)) return false;

    float lx = from->lo.x > to->lo.x ? from->lo.x : to->lo.x;
    float hx = from->hi.x < to->hi.x ? from->hi.x : to->hi.x;
    float ly = from->lo.y > to->lo.y ? from->lo.y : to->lo.y;
    float hy = from->hi.y < to->hi.y ? from->hi.y : to->hi.y;
    bool vertical = (hx - lx) <= (hy - ly);
    Vector normal = vertical ? Vector(1.0f, 0.0f, 0.0f)
                             : Vector(0.0f, 1.0f, 0.0f);
    Vector travel = to->center - from->center;
    if (travel.x * normal.x + travel.y * normal.y < 0.0f)
    {
        normal.x = -normal.x;
        normal.y = -normal.y;
    }
    float side = (pos.x - portal.x) * normal.x
               + (pos.y - portal.y) * normal.y;
    return side > 1.0f;
}

// Area centers on a long slope can differ by far more than jump height even
// though their shared floor seam is continuous. Only a real upward step at
// that seam should make ordinary waypoint following press +jump.
static bool WalkEdgeNeedsJump(int fromIdx, int toIdx)
{
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
    bool tight;
    float centerX, centerY, centerZ;
    float halfSpan;
};
static PhysicalPortalProbe s_physicalPortalCache[1024];
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
static bool ProbePhysicalPortal(int fromIdx, int toIdx,
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
    if (cached.mapHash == mapHash && cached.fromIdx == fromIdx
        && cached.toIdx == toIdx && now >= cached.sampleTime
        && now - cached.sampleTime <= 30.0f)
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
static bool IsHadithaDoorNoSprintZone(const int *path, int pathLen,
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
        Vector landing;
        if (GetSeparatedTransitionPortal(m_path[m_pathCursor - 1],
                                         m_path[m_pathCursor], wp,
                                         spreadAxis, spreadHalf, &landing))
        {
            virtualWp = true;
            haveWp = true;
            const NavAreaData *fromA = g_NavMesh.Area(m_path[m_pathCursor - 1]);
            const NavAreaData *toA = g_NavMesh.Area(m_path[m_pathCursor]);
            Vector travel = toA->center - fromA->center;
            float tl = sqrtf(travel.x * travel.x + travel.y * travel.y);
            if (tl > 1.0f)
            {
                travel.x /= tl; travel.y /= tl;
                float along = (wp.x - pos.x) * travel.x + (wp.y - pos.y) * travel.y;
                float lateral = fabsf((pos.x - wp.x) * -travel.y
                                    + (pos.y - wp.y) *  travel.x);
                // First center the hull just inside the source side. Once it
                // is lined up, commit across the gap to a point inside the
                // landing tile. This prevents diagonal corner-cutting.
                if (along > 32.0f || lateral > 8.0f)
                {
                    wp.x -= travel.x * 28.0f;
                    wp.y -= travel.y * 28.0f;
                    wp.z = g_NavMesh.GetZ(m_path[m_pathCursor - 1], wp.x, wp.y);
                    virtualApproach = true;
                }
                else
                    wp = landing;
            }
            else
                wp = landing;
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
        // Keep the aim inside the destination area so GetZ below always sees
        // a real floor point (merged openings can extend past a slice area).
        const NavAreaData *destA = g_NavMesh.Area(m_path[m_pathCursor]);
        if (destA && !virtualApproach)
        {
            if (wp.x < destA->lo.x) wp.x = destA->lo.x;
            else if (wp.x > destA->hi.x) wp.x = destA->hi.x;
            if (wp.y < destA->lo.y) wp.y = destA->lo.y;
            else if (wp.y > destA->hi.y) wp.y = destA->hi.y;
        }
        wp.z = g_NavMesh.GetZ(m_path[m_pathCursor], wp.x, wp.y);
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

// Drive one complete typed NAV ladder edge. Source ladder physics derives
// vertical wish-speed from view pitch while the player pushes into the ladder,
// so climbing temporarily owns both yaw and pitch. The area cursor still
// advances only after the bot physically reaches the destination landing.
static Vector LadderAreaInterior(int areaIdx, const Vector &lip)
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

    bool reachedLip = up ? pos.z >= endHeight : pos.z <= endHeight;
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
            BotDbg("[insbots] %s ladder %d dismount lost height; climb resumed\n",
                   m_name, ladderIdx);
        }
    }

    if (m_ladderPhase == LADDER_PHASE_ATTACHED && reachedLip)
    {
        m_ladderPhase = LADDER_PHASE_DISMOUNT;
        BotDbg("[insbots] %s ladder %d dismount latched\n", m_name, ladderIdx);
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
    int maxCheck = (startIdx + 2 < m_pathLen) ? startIdx + 2 : m_pathLen - 1;

    for (int i = startIdx; i < maxCheck; i++)
    {
        int fromIdx = m_path[i];
        int toIdx = m_path[i + 1];
        Vector safeCenter; float safeHalf = 0.0f;
        if (ProbePhysicalPortal(fromIdx, toIdx, &safeCenter, &safeHalf))
        {
            float dx = safeCenter.x - pos.x;
            float dy = safeCenter.y - pos.y;
            float distSq = dx * dx + dy * dy;
            if (distSq <= 220.0f * 220.0f)
            {
                const NavAreaData *fromA = g_NavMesh.Area(fromIdx);
                const NavAreaData *toA = g_NavMesh.Area(toIdx);
                if (!fromA || !toA) continue;

                Vector normal = toA->center - fromA->center;
                normal.z = 0.0f;
                float nLen = normal.Length2D();
                if (nLen > 0.001f)
                    normal /= nLen;
                else
                    normal = Vector(1.0f, 0.0f, 0.0f);

                Vector runwayPoint = safeCenter - normal * 120.0f;
                runwayPoint.z = safeCenter.z;

                outRunway.active = true;
                outRunway.center = safeCenter;
                outRunway.normal = normal;
                outRunway.runwayPoint = runwayPoint;
                outRunway.halfSpan = safeHalf;
                outRunway.distToPortal = sqrtf(distSq);

                float portalYaw = atan2f(normal.y, normal.x) * (180.0f / 3.14159265f);
                outRunway.turnAngle = fabsf(SignedAngleDiff(portalYaw, m_headingYaw));

                outRunway.isChained = false;
                if (i + 2 < m_pathLen)
                {
                    Vector nextCenter; float nextHalf = 0.0f;
                    if (ProbePhysicalPortal(m_path[i + 1], m_path[i + 2], &nextCenter, &nextHalf))
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

    Ray_t ray;
    ray.Init(start, end, Vector(-16, -16, 0), Vector(16, 16, 72));

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
    if (!runway.active) return false;
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
        if (alongNormal >= -140.0f && alongNormal <= 48.0f)
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

    CTraceFilterWorldAndPropsOnly filter;
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

static float xyd(const Vector &a, const Vector &b)
{
    float dx = a.x - b.x, dy = a.y - b.y;
    return sqrtf(dx * dx + dy * dy);
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
    CTraceFilterWorldAndPropsOnly wf;
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



// ============================================================================
// Take-Cover Overhaul v2 (see take-cover-overhaul-design.md in release folder)
// ============================================================================
float CInsBot::EvaluateCover(const CoverPoint &cp, const Vector &threatPos) const
{
    float yawDeg = atan2f(threatPos.y - cp.pos.y,
                          threatPos.x - cp.pos.x) * (180.0f / 3.14159265358979f);
    yawDeg = fmodf(yawDeg + 360.0f, 360.0f);
    int sec = ((int)(fmodf(yawDeg + 22.5f, 360.0f) / 45.0f)) & 7;   // Fix H-3: center sectors on ray directions

    float s = 0.0f;
    if (cp.blockMask[sec]) s += 100.0f;         // full standing protection
    else if (cp.crouchMask[sec]) s += 55.0f;    // protects if we duck behind it
    else                     s -= 60.0f;        // exposed from this bearing
    return s;
}

bool CInsBot::ChooseCoverSpot(const Vector &primaryThreat, Vector &outSpot, int &outTier)
{
    if (!g_Bots.m_coverPoints || g_Bots.CoverCount() <= 0) return false;

    Vector pos = m_info ? m_info->GetAbsOrigin() : Vector(0,0,0);
    float now = Now();
    int myTeam = m_info ? m_info->GetTeamIndex() : 0;

    // ---- gather threats (bounded to 5 total) ----
    Vector threats[5];
    int threatCount = 0;
    if (primaryThreat.x != 0.0f || primaryThreat.y != 0.0f)
        threats[threatCount++] = primaryThreat;

    float searchR = insbot_cover_search_radius.GetFloat();
    if (searchR < 100.0f) searchR = 100.0f;
    for (int i = 1; i <= gpGlobals->maxClients && threatCount < 5; i++)
    {
        if (!g_Intel[myTeam].enemy[i].valid) continue;
        Vector ip = g_Intel[myTeam].enemy[i].pos;
        if (now - g_Intel[myTeam].enemy[i].time > insbot_intel_time.GetFloat()) continue;
        bool isPrimary = (ip.x == primaryThreat.x && ip.y == primaryThreat.y);
        if (isPrimary) continue;
        float dSq = (ip - pos).LengthSqr();
        if (dSq > (searchR * 2.5f) * (searchR * 2.5f)) continue;
        bool dup = false;
        for (int t=0;t<threatCount;t++)
            if (threats[t].x == ip.x && threats[t].y == ip.y) { dup=true;break; }
        if (!dup) threats[threatCount++] = ip;
    }

    // ---- score candidates within search radius ----
    float maxReach = insbot_cover_max_reach.GetFloat();
    float tierW = insbot_cover_tier_weight.GetFloat();
    float distW = insbot_cover_dist_weight.GetFloat();
    float advP = insbot_cover_advance_penalty.GetFloat();
    float minScore = insbot_cover_min_score.GetFloat();

    float myT = xyd(pos, primaryThreat);
    float bestScore = minScore;
    int bestIdx = -1;
    float bestDist = 0.0f;

    for (int ci = 0; ci < g_Bots.CoverCount(); ++ci)
    {
        const CoverPoint *cp = g_Bots.CoverAt(ci);
        if (!cp) continue;

        Vector candPos = cp->pos;
        float dMeCand = sqrtf((candPos - pos).LengthSqr());
        if (dMeCand > searchR || dMeCand > maxReach) continue;

        int ownerSlot = g_Bots.CoverOwnerSlot(ci);
        if (ownerSlot >= 0 && ownerSlot != (int)(this - g_Bots.m_bots))
        {
            CInsBot *owner = (ownerSlot < 64) ? &g_Bots.m_bots[ownerSlot] : 0;
            bool aliveAndUsed = owner && owner->m_inUse && owner->m_info && !owner->m_info->IsDead();
            if (aliveAndUsed) continue;
        }

        float worst = 1e30f;
        for (int ti = 0; ti < threatCount; ++ti)
        {
            float sc = EvaluateCover(*cp, threats[ti]);
            if (sc < worst) worst = sc;
        }
        if (threatCount == 0) worst = 0.0f;

        float dT = sqrtf((candPos - primaryThreat).LengthSqr());
        if (dT < myT - 48.0f) worst -= advP;
        worst -= distW * dMeCand;
        worst += tierW * cp->tier;
        if (cp->isLowOnly) worst -= 15.0f;
        // Formation bias: squad followers prefer cover closer to their leader
        if (IsSquadFollower())
        {
            edict_t *le = engine->PEntityOfEntIndex(m_squadLeaderIndex);
            IPlayerInfo *lpi2 = (le && !le->IsFree()) ? playerinfomanager->GetPlayerInfo(le) : 0;
            if (lpi2)
            {
                Vector leaderPos = lpi2->GetAbsOrigin();
                float distLeaderToCand = xyd(candPos, leaderPos);
                float distLeaderToMe = xyd(pos, leaderPos);
                if (distLeaderToCand < distLeaderToMe)
                    worst += insbot_cover_formation_bonus.GetFloat();
            }
        }

        if (worst > bestScore) { bestScore = worst; bestIdx = ci; bestDist = dMeCand; }
    }

    if (bestIdx < 0) return false;

    const CoverPoint *bp = g_Bots.CoverAt(bestIdx);
    outSpot = bp->pos;
    outTier = bp->tier;

    int myRosterIdx = (int)(this - g_Bots.m_bots);
    if (!g_Bots.m_coverOwnerSlot || !g_Bots.m_coverOwnedSince) return false;
    g_Bots.m_coverOwnerSlot[bestIdx] = (short)myRosterIdx;
    g_Bots.m_coverOwnedSince[bestIdx] = now;

        if (insbot_debug.GetBool()) BotDbg("[insbots] %s COVER chose idx=%d tier=%d low=%u score=%.1f dist=%.0f\n",
            m_name, bestIdx, outTier, bp->isLowOnly, bestScore, bestDist);
    return true;
}
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

    CTraceFilterWorldOnly worldFilter;
    Ray_t ray; trace_t tr;

    Vector ahead = waist + dir * 96.0f;
    ray.Init(waist, ahead);
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &worldFilter, &tr);

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
            enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &worldFilter, &t2);
            if (t2.fraction > bestFrac) { bestFrac = t2.fraction; best = y; }
        }
        m_headingYaw = best;

        // low obstacle? try a jump
        Vector knee = pos; knee.z += 20.0f;
        Vector head = pos; head.z += 58.0f;
        ray.Init(knee, knee + dir * 48.0f);
        trace_t tKnee;
        enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &worldFilter, &tKnee);
        ray.Init(head, head + dir * 48.0f);
        trace_t tHead;
        enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &worldFilter, &tHead);
        if (tKnee.fraction < 1.0f && tHead.fraction >= 1.0f)
            buttons |= IN_JUMP;
    }

    SmoothNavTurn(m_headingYaw, insbot_move_speed.GetFloat(), view,
                  fwd, side, m_navTurnAngle, false);
}

// No-path fallback used by MoveToward when the nav mesh is loaded but no
// route can be built (off-mesh spawn/goal, or a point that is unreachable
// right now). Heads at the destination, but probes world geometry so the bot
// slides along walls instead of grinding into them, and re-biases to the
// direct heading as soon as the line to the goal opens up again.
void CInsBot::SteerToPoint(float destYaw, float speed, float &fwd, float &side,
                           int &buttons, QAngle &view)
{
    Vector pos = m_info->GetAbsOrigin();
    float now = Now();

    float yawRad = m_headingYaw * (3.14159265f / 180.0f);
    Vector dir(cosf(yawRad), sinf(yawRad), 0);
    Vector waist = pos; waist.z += 36.0f;

    CTraceFilterWorldOnly worldFilter;
    Ray_t ray; trace_t tr;
    ray.Init(waist, waist + dir * 96.0f);
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &worldFilter, &tr);
    bool blocked = tr.startsolid || tr.allsolid || tr.fraction < 1.0f;

    if (blocked)
    {
        // blocked: test the two 60-degree shoulders and take the more open one
        float best = m_headingYaw + 120.0f; float bestFrac = -1.0f;
        for (int s = -1; s <= 1; s += 2)
        {
            float y = m_headingYaw + 60.0f * s;
            float yr = y * (3.14159265f / 180.0f);
            Vector d2(cosf(yr), sinf(yr), 0);
            ray.Init(waist, waist + d2 * 96.0f);
            trace_t t2;
            enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &worldFilter, &t2);
            if (t2.fraction > bestFrac) { bestFrac = t2.fraction; best = y; }
        }
        m_headingYaw = best;
        m_nextHeadingChange = now + 0.45f;   // hold the deflection briefly
    }
    else if (now >= m_nextHeadingChange)
    {
        // current line open: probe the direct line to the destination and
        // re-bias toward it whenever the wall between them is gone
        float dy = destYaw * (3.14159265f / 180.0f);
        Vector dd(cosf(dy), sinf(dy), 0);
        ray.Init(waist, waist + dd * 96.0f);
        trace_t td;
        enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &worldFilter, &td);
        if (td.fraction >= 1.0f)
        {
            m_headingYaw = destYaw;
            m_nextHeadingChange = now + 0.45f;
        }
    }

    SmoothNavTurn(m_headingYaw, speed, view, fwd, side, m_navTurnAngle, false);
}

// Convert a desired world-space movement yaw into Source's view-relative
// forward/side command pair.  Combat bots keep aiming at their enemy while the
// queue moves them along (or back along) the actual navigation lane.
static void MoveAtWorldYaw(float worldYaw, float viewYaw, float speed,
                           float &fwd, float &side)
{
    float rel = (worldYaw - viewYaw) * (3.14159265f / 180.0f);
    fwd = cosf(rel) * speed;
    side = -sinf(rel) * speed;
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
                if (m_doorwayRunway.active && m_doorwayRunway.distToPortal < 140.0f)
                {
                    float doorYaw = atan2f(m_doorwayRunway.normal.y, m_doorwayRunway.normal.x) * (180.0f / 3.14159265f);
                    view.y = doorYaw;
                    view.x = 0.0f;

                    Vector toBot = pos - m_doorwayRunway.center;
                    float crossTrackError = toBot.x * (-m_doorwayRunway.normal.y) + toBot.y * (m_doorwayRunway.normal.x);

                    float strafe = -crossTrackError * 4.0f;
                    if (strafe > 60.0f) strafe = 60.0f;
                    if (strafe < -60.0f) strafe = -60.0f;

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

// ---- per-map team factions (from the .imc2) --------------------------------
int g_teamFaction[3] = { FACTION_UNKNOWN, FACTION_UNKNOWN, FACTION_UNKNOWN };

static int ScanFaction(const char *start, const char *end)
{
    const char *u = 0, *i = 0;
    for (const char *p = start; p + 10 < end; p++)
    {
        if (!u && !strncmp(p, "TEAM_USMC", 9))  u = p;
        if (!i && !strncmp(p, "TEAM_IRAQI", 10)) i = p;
    }
    if (u && (!i || u < i)) return FACTION_USMC;
    if (i) return FACTION_IRAQI;
    return FACTION_UNKNOWN;
}

void InsBots_LoadMapFactions(const char *gamedir, const char *mapname)
{
    g_teamFaction[1] = g_teamFaction[2] = FACTION_UNKNOWN;

    // PRIMARY: read the real faction out of the map's own .imc2 (works for every
    // map with one, stock or custom - no map-name table needed).
    char path[512];
    _snprintf(path, sizeof(path), "%s/maps/%s.imc2", gamedir, mapname);
    FILE *fp = fopen(path, "rb");
    if (fp)
    {
        fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (sz > 0 && sz < 1000000)
        {
            char *buf = (char *)malloc(sz + 1);
            if (buf && fread(buf, 1, sz, fp) == (size_t)sz)
            {
                buf[sz] = 0;
                char *t1 = strstr(buf, "TeamOne");
                char *t2 = strstr(buf, "TeamTwo");
                if (t1) g_teamFaction[1] = ScanFaction(t1, t2 ? t2 : buf + sz);
                if (t2) g_teamFaction[2] = ScanFaction(t2, buf + sz);
            }
            if (buf) free(buf);
        }
        fclose(fp);
    }

    // FALLBACK: if the .imc2 was missing/unreadable, correct by map name for the
    // STOCK maps. Nearly all are team1=USMC/team2=IRAQI; the two exceptions
    // (baghdad, karam) are flipped. Custom maps with no readable .imc2 stay
    // unknown and fall back to the sidearm_a/b defaults.
    if (g_teamFaction[1] == FACTION_UNKNOWN || g_teamFaction[2] == FACTION_UNKNOWN)
    {
        // every stock map and its team-1 faction (the rest is the enemy of it)
        struct MapFac { const char *map; int t1; };
        static const MapFac s_stock[] = {
            { "ins_abdallah",      FACTION_USMC  }, { "ins_almaden",       FACTION_USMC  },
            { "ins_baghdad",       FACTION_IRAQI }, { "ins_buhriz",        FACTION_USMC  },
            { "ins_haditha",       FACTION_USMC  }, { "ins_haditha_night", FACTION_USMC  },
            { "ins_hillah",        FACTION_USMC  }, { "ins_karam",         FACTION_IRAQI },
            { "ins_karkar",        FACTION_USMC  }, { "ins_ramadi",        FACTION_USMC  },
            { "ins_samawah",       FACTION_USMC  }, { "ins_samawah_day",   FACTION_USMC  },
            { "ins_sinjar",        FACTION_USMC  }, { 0, 0 }
        };
        for (int i = 0; s_stock[i].map; i++)
            if (!_stricmp(mapname, s_stock[i].map))
            {
                int t1 = s_stock[i].t1, t2 = (t1 == FACTION_USMC) ? FACTION_IRAQI : FACTION_USMC;
                if (g_teamFaction[1] == FACTION_UNKNOWN) g_teamFaction[1] = t1;
                if (g_teamFaction[2] == FACTION_UNKNOWN) g_teamFaction[2] = t2;
                Msg("[insbots] (.imc2 unread - used the stock-map faction table)\n");
                break;
            }
        // a non-stock map with no readable .imc2 stays UNKNOWN -> sidearm_a/b
    }

    const char *fn[3] = { "unknown", "USMC", "IRAQI" };
    Msg("[insbots] map factions: team1 = %s, team2 = %s\n",
        fn[g_teamFaction[1] % 3], fn[g_teamFaction[2] % 3]);
}

const char *CInsBot::SidearmName() const
{
    int team = m_info ? m_info->GetTeamIndex() : TeamSlotToTeamIndex(m_teamSlot);
    int fac  = (team == 1 || team == 2) ? g_teamFaction[team] : FACTION_UNKNOWN;
    if (fac == FACTION_USMC)  return insbot_sidearm_usmc.GetString();
    if (fac == FACTION_IRAQI) return insbot_sidearm_iraqi.GetString();
    return (m_teamSlot == 0) ? insbot_sidearm_a.GetString() : insbot_sidearm_b.GetString();
}
const char *CInsBot::GrenadeName() const
{
    int team = m_info ? m_info->GetTeamIndex() : TeamSlotToTeamIndex(m_teamSlot);
    int fac  = (team == 1 || team == 2) ? g_teamFaction[team] : FACTION_UNKNOWN;
    if (fac == FACTION_USMC)  return insbot_grenade_usmc.GetString();
    if (fac == FACTION_IRAQI) return insbot_grenade_iraqi.GetString();
    return (m_teamSlot == 0) ? insbot_grenade_a.GetString() : insbot_grenade_b.GetString();
}

// solve the launch angle to lob a projectile of speed V from 'from' so it lands
// on 'target' under gravity g. Fills yaw/pitch (Source degrees, -pitch = up).
// Returns false if the target is out of range for that speed.
static bool SolveBallisticArc(const Vector &from, const Vector &target,
                              float V, float g, float &outYaw, float &outPitch)
{
    float dx = target.x - from.x, dy = target.y - from.y;
    float d = sqrtf(dx * dx + dy * dy);      // horizontal distance
    float h = target.z - from.z;             // height difference (up = +)
    outYaw = atan2f(dy, dx) * (180.0f / 3.14159265f);
    if (d < 1.0f) { outPitch = -60.0f; return true; }   // basically straight up

    float V2 = V * V;
    float disc = V2 * V2 - g * (g * d * d + 2.0f * h * V2);
    if (disc < 0.0f) return false;           // unreachable at this speed
    float sq = sqrtf(disc);
    // lower-angle (direct) root -> flatter, faster-arriving throw
    float tanTheta = (V2 - sq) / (g * d);
    float theta = atanf(tanTheta);           // radians, up-positive
    outPitch = -theta * (180.0f / 3.14159265f);
    if (outPitch < -80.0f) outPitch = -80.0f;
    if (outPitch >  45.0f) outPitch =  45.0f;
    return true;
}

// begin a grenade throw: compute the arc to 'target', switch to the grenade,
// aim, throw, switch back. Returns having set state; StartGrenade only runs if
// a valid arc exists (checked by caller via CanGrenade path).
static const char *InsBot_stristr(const char *str, const char *substr)
{
    if (!str || !substr) return NULL;
    if (!*substr) return str;
    size_t sublen = strlen(substr);
    for (; *str; str++)
    {
        if (!_strnicmp(str, substr, sublen))
            return str;
    }
    return NULL;
}

bool CInsBot::SwitchToWeapon(const char *weaponClass, bool force)
{
    if (!weaponClass || !weaponClass[0] || !m_edict || m_edict->IsFree() || !m_edict->GetUnknown())
        return false;
    void *pPlayer = m_edict->GetUnknown()->GetBaseEntity();
    if (!pPlayer || s_offset_m_hMyWeapons < 0 || !engine)
        return false;

    char *base = (char *)pPlayer;
    const char *token = weaponClass;
    if (!_strnicmp(token, "weapon_", 7)) token += 7;

    for (int i = 0; i < 48; i++)
    {
        int h = *(int *)(base + s_offset_m_hMyWeapons + i * 4);
        if (h == -1 || h == 0) continue;
        int entIndex = h & 0xFFF;
        if (entIndex < 1 || entIndex > 2048) continue;
        edict_t *wEnt = engine->PEntityOfEntIndex(entIndex);
        if (!wEnt || wEnt->IsFree() || !wEnt->GetUnknown()) continue;
        const char *cn = wEnt->GetClassName();
        if (cn && (!_stricmp(cn, weaponClass) || (token[0] && InsBot_stristr(cn, token))))
        {
            void *pWeapon = wEnt->GetUnknown()->GetBaseEntity();
            if (pWeapon)
            {
                if (s_offset_flNextAttack >= 0)
                    *(float *)(base + s_offset_flNextAttack) = gpGlobals ? gpGlobals->curtime : 0.0f;
                void **vtable = *(void ***)pPlayer;
#ifdef _LINUX
                typedef bool (*Weapon_Switch_Fn)(void *, void *, bool);
                // Audited in Linux server_i486.so _ZTV10CINSPlayer (RVA 0x7706C0):
                // vtable[218] is Weapon_Equip, vtable[219] is Weapon_CanUse, vtable[220] is Weapon_Switch (offset 0x370).
                Weapon_Switch_Fn fnSwitch = (Weapon_Switch_Fn)vtable[220];
                return fnSwitch(pPlayer, pWeapon, force);
#else
                typedef bool (__thiscall *Weapon_Switch_Fn)(void *, void *, bool);
                // Audited in Windows server.dll CINSPlayer primary vtable (VA 0x101F75D4):
                // vtable[220] is CBasePlayer::Weapon_Switch (VA 0x10118360, offset 0x370).
                Weapon_Switch_Fn fnSwitch = (Weapon_Switch_Fn)vtable[220];
                return fnSwitch(pPlayer, pWeapon, force);
#endif
            }
            break;
        }
    }
    return false;
}

void CInsBot::StartGrenade(float now, const Vector &target)
{
    extern CfgVar insbot_attack;
    if (!m_ctrl || !m_info || !insbot_attack.GetBool()) return;

    float V = insbot_grenade_speed.GetFloat();
    float g = insbot_grenade_gravity.GetFloat();
    Vector from = EyePos();
    float yaw, pitch;
    if (!SolveBallisticArc(from, target, V, g, yaw, pitch))
        return;   // out of range - don't waste the grenade

    const char *cur = m_info->GetWeaponName();
    _snprintf(m_preNadeWeapon, sizeof(m_preNadeWeapon), "%s", cur ? cur : "");
    m_preNadeWeapon[sizeof(m_preNadeWeapon) - 1] = 0;

    if (!SwitchToWeapon(GrenadeName(), true))
    {
        m_ctrl->SetActiveWeapon(GrenadeName());
    }
    m_nadeState = 1;
    m_nadeKind = 0;             // frag
    Say(VOX_GRENADE);           // "frag out"
    m_nadeTimer = now + 0.50f;   // short switch timeout; throw as soon as it is in hand
    m_nadeAim = target;
    m_nadeYaw = yaw;
    m_nadePitch = pitch;

    if (insbot_debug.GetBool())
        BotDbg("[insbots] %s grenade arc to (%.0f %.0f %.0f) yaw=%.0f pitch=%.0f\n",
               m_name, target.x, target.y, target.z, yaw, pitch);
}

// Throw a SMOKE onto 'target' (an objective centre). We can't read a bot's
// inventory directly, so we switch to the smoke weapon and let UpdateGrenade
// confirm the switch actually took (state 1) - if the bot has no smoke the
// switch is a no-op and we quietly restore the previous weapon. Uses its own
// cooldown (m_nextSmoke), independent of frags.
void CInsBot::StartSmoke(float now, const Vector &target)
{
    extern CfgVar insbot_attack;
    if (!m_ctrl || !m_info || !insbot_attack.GetBool()) return;

    float V = insbot_grenade_speed.GetFloat();
    float g = insbot_grenade_gravity.GetFloat();
    Vector from = EyePos();
    float yaw, pitch;
    if (!SolveBallisticArc(from, target, V, g, yaw, pitch))
        return;   // can't reach the point with an arc - don't bother

    const char *cur = m_info->GetWeaponName();
    _snprintf(m_preNadeWeapon, sizeof(m_preNadeWeapon), "%s", cur ? cur : "");
    m_preNadeWeapon[sizeof(m_preNadeWeapon) - 1] = 0;

    if (!SwitchToWeapon(insbot_smoke_weapon.GetString(), true))
    {
        m_ctrl->SetActiveWeapon(insbot_smoke_weapon.GetString());
    }
    m_nadeState = 1;
    m_nadeKind  = 1;            // smoke
    m_nadeTimer = now + 0.50f;  // max wait for the switch to confirm possession
    m_nadeAim   = target;
    m_nadeYaw   = yaw;
    m_nadePitch = pitch;

    if (insbot_debug.GetBool())
        BotDbg("[insbots] %s smoking objective at (%.0f %.0f %.0f)\n",
               m_name, target.x, target.y, target.z);
}

// Decide whether to smoke the current objective this frame. Only for a CONTESTED
// point (not one we already own), within smoke_range, on a per-bot cooldown and
// a chance roll. StartSmoke then verifies the bot actually carries a smoke.
// Scan intel for clusters of enemies and optionally smoke them


bool CInsBot::HasWeapon(const char *weaponClass) const
{
    if (!weaponClass || !m_edict || m_edict->IsFree() || !m_info) return false;
    // Fail-open if inventory offset is unavailable: allow UpdateGrenade/UpdateSmoke to attempt switch
    if (s_offset_m_hMyWeapons < 0 || !engine) return true;

    IServerUnknown *unk = m_edict->GetUnknown();
    if (!unk) return true;
    char *base = (char *)unk->GetBaseEntity();
    if (!base) return true;

    // Tokenize weaponClass by stripping optional "weapon_" prefix (e.g. "weapon_m67" -> "m67")
    const char *token = weaponClass;
    if (!_strnicmp(token, "weapon_", 7)) token += 7;

    for (int i = 0; i < 48; i++)
    {
        int h = *(int *)(base + s_offset_m_hMyWeapons + i * 4);
        if (h == -1 || h == 0) continue;
        int entIndex = h & 0xFFF;
        if (entIndex < 1 || entIndex > 2048) continue;
        edict_t *wEnt = engine->PEntityOfEntIndex(entIndex);
        if (!wEnt || wEnt->IsFree()) continue;

        // Compare against ServerClass name (e.g. "CGrenadeM67", "CGrenadeRGD5", "CGrenadeM18")
        const char *cn = wEnt->GetClassName();
        if (cn)
        {
            if (!_stricmp(cn, weaponClass) || (token[0] && InsBot_stristr(cn, token)))
                return true;
        }
    }
    return false;
}

void CInsBot::MaybeSmokeEnemyGroup(float now, int myTeam)
{
    if (!insbot_smoke.GetBool() || !insbot_smoke_group.GetBool()) return;
    if (!HasWeapon(insbot_smoke_weapon.GetString())) return;
    if (m_nadeState != 0) return;                       // already throwing something
    if (now < m_nextSmoke) return;                      // on cooldown
    
    int minGroup = insbot_smoke_group_min.GetInt();
    if (minGroup < 2) minGroup = 2; // Sanity check
    float radius = insbot_smoke_group_radius.GetFloat();
    float radiusSq = radius * radius;
    float intelTime = insbot_intel_time.GetFloat();
    
    // Find clusters
    for (int i = 1; i <= 65; i++)
    {
        if (!g_Intel[myTeam].enemy[i].valid) continue;
        if (!IntelEnemyIsLive(myTeam, i))
        {
            g_Intel[myTeam].enemy[i].valid = false;
            continue;
        }
        if (now - g_Intel[myTeam].enemy[i].time > intelTime) continue;
        
        Vector center = g_Intel[myTeam].enemy[i].pos;
        int count = 1;
        Vector sum = center;
        
        for (int j = 1; j <= 65; j++)
        {
            if (i == j) continue;
            if (!g_Intel[myTeam].enemy[j].valid) continue;
            if (!IntelEnemyIsLive(myTeam, j))
            {
                g_Intel[myTeam].enemy[j].valid = false;
                continue;
            }
            if (now - g_Intel[myTeam].enemy[j].time > intelTime) continue;
            
            Vector p = g_Intel[myTeam].enemy[j].pos;
            float dx = p.x - center.x;
            float dy = p.y - center.y;
            float dz = p.z - center.z;
            if (dx*dx + dy*dy + dz*dz <= radiusSq)
            {
                count++;
                sum += p;
            }
        }
        
        if (count >= minGroup)
        {
            m_nextSmoke = now + insbot_smoke_cooldown.GetFloat();
            if (frand(0.0f, 1.0f) < insbot_smoke_group_chance.GetFloat())
            {
                Vector target = sum / (float)count;
                float scatterFactor = insbot_smoke_scatter.GetFloat();
                if (scatterFactor > 0.0f)
                {
                    float jitterR = 80.0f * (scatterFactor > 1.0f ? 1.0f : scatterFactor);
                    float angle = frand(0.0f, 6.2831853f);
                    float r = frand(0.0f, jitterR);
                    target.x += cosf(angle) * r;
                    target.y += sinf(angle) * r;
                }
                target.z += 8.0f;
                StartSmoke(now, target);
            }
            return; // Found a cluster (either smoked or failed roll), stop checking
        }
    }
}

void CInsBot::MaybeSmokeObjective(float now, const ObjPoint *op, float dist, int myTeam)
{
    if (!op) return;
    if (op->isEnclosed) return; // Indoor / basement objective: skip high-arc smoke throw
    if (!insbot_smoke.GetBool()) return;
    if (!HasWeapon(insbot_smoke_weapon.GetString())) return;
    if (m_nadeState != 0) return;                       // already throwing something
    if (now < m_nextSmoke) return;                      // on cooldown
    if (g_Objectives.Relation(m_objGoal, myTeam) > 0) return;   // already ours: no need
    if (dist > insbot_smoke_range.GetFloat()) return;   // too far to reach the point
    if (dist < op->radius * 0.5f) return;               // basically standing on it already

    m_nextSmoke = now + insbot_smoke_cooldown.GetFloat();   // spend the window either way
    if (frand(0.0f, 1.0f) < insbot_smoke_chance.GetFloat())
    {
        Vector t = op->center;
        float scatterFactor = insbot_smoke_scatter.GetFloat();
        if (scatterFactor > 0.0f)
        {
            if (scatterFactor > 1.0f) scatterFactor = 1.0f;
            float maxR = op->radius * scatterFactor;
            float angle = frand(0.0f, 6.2831853f);
            float r = frand(0.0f, maxR);
            t.x += cosf(angle) * r;
            t.y += sinf(angle) * r;

            if (enginetrace)
            {
                CTraceFilterWorldAndPropsOnly filter;
                Ray_t ray; trace_t tr;
                ray.Init(t + Vector(0.0f, 0.0f, 48.0f), t - Vector(0.0f, 0.0f, 48.0f));
                enginetrace->TraceRay(ray, MASK_SOLID, &filter, &tr);
                if (!tr.startsolid && !tr.allsolid && tr.fraction < 1.0f)
                {
                    t.z = tr.endpos.z;
                }
            }
        }
        t.z += 8.0f;             // aim just above the point surface
        StartSmoke(now, t);
    }
}

void CInsBot::RestoreGrenadeWeapon()
{
    if (!m_preNadeWeapon[0]) return;
    if (!SwitchToWeapon(m_preNadeWeapon, true))
    {
        if (m_ctrl) m_ctrl->SetActiveWeapon(m_preNadeWeapon);
    }
}

bool CInsBot::IsHoldingM203() const
{
    if (m_wp)
    {
        const WeaponProfile *wp = (const WeaponProfile *)m_wp;
        if (wp->cls && !_stricmp(wp->cls, "m16m203")) return true;
    }
    if (m_wpName[0] && strstr(m_wpName, "m16m203")) return true;
    return false;
}

bool CInsBot::CanM203Fire() const
{
    if (!IsHoldingM203()) return false;
    const WeaponProfile *m203Wp = WeaponFind("m203");
    if (!m203Wp) return false;
    int limit = CombatReserveMags(m203Wp);
    if (m_reloadMags[WC_GRENADE_LAUNCHER] >= limit) return false;

    // Overhead ceiling clearance check: verify bot is not under a low roof
    extern IEngineTrace *enginetrace;
    if (enginetrace && m_edict)
    {
        Vector eye = EyePos();
        Vector upEnd = eye + Vector(0, 0, 160.0f);
        Ray_t ray;
        ray.Init(eye, upEnd);
        trace_t tr;
        CTraceFilterSkipSelf filter(EdictHandleEntity(m_edict));
        enginetrace->TraceRay(ray, MASK_SOLID, &filter, &tr);
        if (tr.fraction < 1.0f && !(tr.surface.flags & SURF_SKY))
            return false; // Under low indoor ceiling
    }
    return true;
}

bool CInsBot::IsHoldingRPG() const
{
    if (m_wp)
    {
        const WeaponProfile *wp = (const WeaponProfile *)m_wp;
        if (wp->combatClass == WC_RPG || (wp->cls && !_stricmp(wp->cls, "rpg7"))) return true;
    }
    if (m_wpName[0] && strstr(m_wpName, "rpg7")) return true;
    return false;
}

bool CInsBot::CanRPGFire() const
{
    if (!IsHoldingRPG()) return false;
    const WeaponProfile *rpgWp = WeaponFind("rpg7");
    if (!rpgWp) return false;
    int limit = CombatReserveMags(rpgWp);
    if (m_reloadMags[WC_RPG] >= limit) return false;
    return true;
}


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

                    float yawDeg = atan2f(threatPos.y - cp->pos.y, threatPos.x - cp->pos.x) * (180.0f / 3.14159265f);
                    yawDeg = fmodf(yawDeg + 360.0f, 360.0f);
                    int sec = ((int)(fmodf(yawDeg + 22.5f, 360.0f) / 45.0f)) & 7;   // Fix H-3: center sectors on ray directions
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
        Vector threat = (m_enemyIndex) ? m_enemyLastPos : pos;
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

void CInsBot::StartM203(float now, const Vector &target)
{
    extern CfgVar insbot_attack;
    extern CfgVar insbot_m203;
    if (!m_ctrl || !m_info || !insbot_attack.GetBool() || !insbot_m203.GetBool()) return;
    if (!CanM203Fire()) return;

    float V = insbot_m203_speed.GetFloat();
    if (V < 300.0f) V = 1570.0f;
    float g = 800.0f;  // Source engine gravity
    Vector from = EyePos();
    float yaw, pitch;
    if (!SolveBallisticArc(from, target, V, g, yaw, pitch))
        return; // out of range or blocked

    // Friendly Teammate Blast-Zone Check: ensure no friendly teammate is within 320u of target
    if (playerinfomanager && engine && gpGlobals)
    {
        int myTeam = m_info->GetTeamIndex();
        for (int i = 1; i <= gpGlobals->maxClients; i++)
        {
            edict_t *te = engine->PEntityOfEntIndex(i);
            if (!te || te->IsFree() || te == m_edict) continue;
            IPlayerInfo *tp = playerinfomanager->GetPlayerInfo(te);
            if (!tp || !tp->IsConnected() || tp->IsDead() || tp->GetTeamIndex() != myTeam) continue;
            Vector tpos = tp->GetAbsOrigin();
            float tdx = tpos.x - target.x, tdy = tpos.y - target.y, tdz = tpos.z - target.z;
            if (tdx * tdx + tdy * tdy + tdz * tdz < 320.0f * 320.0f)
                return; // Friendly teammate inside 40mm blast radius!
        }
    }

    // Piecewise Swept Parabola Trajectory Clearance Check (tests true curved flight volume)
    float degToRad = 3.14159265f / 180.0f;
    float p_rad = -pitch * degToRad; // radians above horizontal
    float y_rad = yaw * degToRad;
    float Vz = V * sinf(p_rad);
    float Vxy = V * cosf(p_rad);
    float Vx = cosf(y_rad) * Vxy;
    float Vy = sinf(y_rad) * Vxy;

    float dx = target.x - from.x, dy = target.y - from.y;
    float dHoriz = sqrtf(dx * dx + dy * dy);
    if (Vxy > 1.0f && dHoriz > 1.0f)
    {
        float tFlight = dHoriz / Vxy;
        extern IEngineTrace *enginetrace;
        if (enginetrace && m_edict && tFlight > 0.05f)
        {
            const int NUM_SEGMENTS = 6;
            Vector hullMins(-2.0f, -2.0f, -2.0f);
            Vector hullMaxs( 2.0f,  2.0f,  2.0f);
            CTraceFilterSkipSelf filter(EdictHandleEntity(m_edict));

            Vector pPrev = from;
            for (int seg = 1; seg <= NUM_SEGMENTS; seg++)
            {
                float t = (float)seg / (float)NUM_SEGMENTS * tFlight;
                Vector pNext = from + Vector(Vx * t, Vy * t, Vz * t - 0.5f * g * t * t);

                Ray_t ray;
                ray.Init(pPrev, pNext, hullMins, hullMaxs);
                trace_t tr;
                enginetrace->TraceRay(ray, MASK_SOLID, &filter, &tr);

                if (tr.fraction < 1.0f && !(tr.surface.flags & SURF_SKY))
                {
                    // If blocked before reaching the final segment, the flight path is obstructed by a roof/wall
                    if (seg < NUM_SEGMENTS)
                        return;
                    // On the final arrival segment, require arriving within 85% of the destination
                    if (tr.fraction < 0.85f)
                        return;
                }
                pPrev = pNext;
            }
        }
    }

    const WeaponProfile *m203Wp = WeaponFind("m203");
    if (m203Wp) m_wp = m203Wp;

    m_m203State = 1;
    m_m203Timer = now + 0.25f;
    m_m203Aim = target;
    m_m203Yaw = yaw;
    m_m203Pitch = pitch;
    m_nextM203 = now + insbot_m203_cooldown.GetFloat();

    if (insbot_debug.GetBool())
    {
        int fired = m_reloadMags[WC_GRENADE_LAUNCHER] + 1;
        int maxMags = m203Wp ? CombatReserveMags(m203Wp) : 10;
        BotDbg("[insbots] %s M203 40mm arc to (%.0f %.0f %.0f) yaw=%.0f pitch=%.0f (round %d/%d)\n",
               m_name, target.x, target.y, target.z, yaw, pitch, fired, maxMags);
    }
}

void CInsBot::UpdateM203(float now, int &buttons, QAngle &view)
{
    view.y = m_m203Yaw;
    view.x = m_m203Pitch;
    m_curAngles = view;

    if (m_m203State == 1) // Phase 1: Aim along ballistic arc
    {
        if (now >= m_m203Timer)
        {
            m_m203State = 2; // Transition to single-tick launch pulse
        }
    }
    else if (m_m203State == 2) // Phase 2: Single-tick launch (m_wp remains m203 so RunMove executes FireLauncherDirect once)
    {
        buttons |= IN_ATTACK;
        m_m203State = 3;
        m_m203Timer = now + 0.35f; // Settle cooldown after launch
    }
    else if (m_m203State == 3) // Phase 3: Settle complete, restore rifle weapon profile
    {
        if (now >= m_m203Timer)
        {
            m_m203State = 0;
            const WeaponProfile *rifleWp = WeaponFind("m16m203");
            if (rifleWp) m_wp = rifleWp;
            m_nextM203 = now + insbot_m203_cooldown.GetFloat();
        }
    }
}

void CInsBot::MaybeM203Objective(float now, const ObjPoint *op, float dist, int myTeam)
{
    if (!op) return;
    if (op->isEnclosed) return; // Indoor / basement objective: skip high-arc artillery
    if (!insbot_m203.GetBool() || !insbot_m203_obj.GetBool()) return;
    if (!CanM203Fire()) return;
    if (m_m203State != 0 || m_nadeState != 0) return;
    if (now < m_nextM203) return;
    if (g_Objectives.Relation(m_objGoal, myTeam) > 0) return; // already ours

    float minR = insbot_m203_min_range.GetFloat();
    float maxR = insbot_m203_max_range.GetFloat();
    if (dist < minR || dist > maxR) return;

    // Friendly fire safety check: verify no living friendly teammate is inside the objective
    if (playerinfomanager && engine && gpGlobals)
    {
        for (int i = 1; i <= gpGlobals->maxClients; i++)
        {
            edict_t *te = engine->PEntityOfEntIndex(i);
            if (!te || te->IsFree() || te == m_edict) continue;
            IPlayerInfo *tp = playerinfomanager->GetPlayerInfo(te);
            if (!tp || !tp->IsConnected() || tp->IsDead() || tp->GetTeamIndex() != myTeam) continue;
            Vector tpos = tp->GetAbsOrigin();
            if (PlayerInsideObjective(op, tp, tpos) || xyd(tpos, op->center) <= op->radius)
                return; // friendly teammate inside objective, skip shelling
        }
    }

    m_nextM203 = now + insbot_m203_cooldown.GetFloat();
    if (frand(0.0f, 1.0f) < insbot_m203_obj_chance.GetFloat())
    {
        float scatterR = op->radius * 0.4f;
        Vector t = op->center + Vector(frand(-scatterR, scatterR), frand(-scatterR, scatterR), 12.0f);
        StartM203(now, t);
    }
}

void CInsBot::UpdateGrenade(float now, const Vector &target, int &buttons, QAngle &view)
{
    // A frag follows the current combat target until release instead of aiming
    // forever at the position snapshotted by StartGrenade. Smoke keeps its
    // fixed objective/group target.
    if (m_nadeKind == 0 && (m_nadeState == 1 || m_nadeState == 2))
        m_nadeAim = target;

    // Re-solve while switching and throwing because the bot and a frag target
    // may both move. Before attack starts, an invalid arc is a clean abort. If
    // attack is already held, retain the last valid arc and finish the release.
    if (m_nadeState == 1 || m_nadeState == 2)
    {
        float yaw, pitch;
        if (SolveBallisticArc(EyePos(), m_nadeAim,
                              insbot_grenade_speed.GetFloat(),
                              insbot_grenade_gravity.GetFloat(), yaw, pitch))
        {
            m_nadeYaw = yaw;
            m_nadePitch = pitch;
        }
        else if (m_nadeState == 1)
        {
            RestoreGrenadeWeapon();
            m_nadeState = 3;
            m_nadeTimer = now + 0.35f;
            return;
        }
    }

    if (m_nadeState == 1)
    {
        // Do not use a fixed settle delay: attack on the first frame the
        // requested grenade is actually active. This also verifies ownership.
        const char *wanted = m_nadeKind == 1
                           ? insbot_smoke_weapon.GetString() : GrenadeName();
        const char *held = m_info ? m_info->GetWeaponName() : 0;
        if (held && !_stricmp(held, wanted))
        {
            m_nadeState = 2;
            // Press on the first frame the grenade is active and release on
            // the next few ticks. There is no extra cook/hold phase.
            m_nadeTimer = now + 0.10f;
            if (insbot_debug.GetBool())
                BotDbg("[insbots] %s grenade in hand (%s) -> pulling pin and throwing\n", m_name, held);
        }
        else if (now >= m_nadeTimer)
        {
            if (insbot_debug.GetBool())
                BotDbg("[insbots] %s nade state 1 TIMEOUT ABORT! held='%s' wanted='%s'\n", m_name, held ? held : "null", wanted ? wanted : "null");
            // The requested grenade was not owned or the switch failed.
            RestoreGrenadeWeapon();
            m_nadeState = 3;
            m_nadeTimer = now + 0.35f;
            return;
        }
    }

    // Lock the view only during the actual release. State 1 keeps walking and
    // looking normally, so a switching bot does not stop a corridor column.
    if (m_nadeState == 2)
    {
        view.y = m_nadeYaw;
        view.x = m_nadePitch;
        m_curAngles = view;
        buttons |= IN_ATTACK;
        if (now >= m_nadeTimer)
        {
            m_nadeState = 3;
            m_nadeTimer = now + 0.35f;
            RestoreGrenadeWeapon();
        }
    }
    else if (m_nadeState == 3)
    {
        const char *wanted = m_nadeKind == 1
                           ? insbot_smoke_weapon.GetString() : GrenadeName();
        const char *held = m_info ? m_info->GetWeaponName() : 0;
        if (held && _stricmp(held, wanted))
        {
            m_preNadeWeapon[0] = 0;
            m_nadeState = 0;
            m_nadeKind = 0;
            m_nextGrenade = now + insbot_grenade_cooldown.GetFloat();
            return;
        }

        // Give SetActiveWeapon time to settle. The old helper sent lastinv in
        // this same frame, which could undo the exact switch and reselect the
        // grenade. Only use lastinv after the exact restore has really failed.
        if (now < m_nadeTimer)
            RestoreGrenadeWeapon();
        else
        {
            if (helpers) helpers->ClientCommand(m_edict, "lastinv");
            m_nadeState = 4;
            m_nadeTimer = now + 0.35f;
        }
    }
    else if (m_nadeState == 4)
    {
        const char *wanted = m_nadeKind == 1
                           ? insbot_smoke_weapon.GetString() : GrenadeName();
        const char *held = m_info ? m_info->GetWeaponName() : 0;
        if (held && _stricmp(held, wanted))
        {
            m_preNadeWeapon[0] = 0;
            m_nadeState = 0;
            m_nadeKind = 0;
            m_nextGrenade = now + insbot_grenade_cooldown.GetFloat();
        }
        else if (now >= m_nadeTimer)
        {
            // Hard fallback: keep asking for a known firearm instead of ever
            // leaving an idle bot in a grenade slot.
            RestoreGrenadeWeapon();
            if (m_ctrl) m_ctrl->SetActiveWeapon(SidearmName());
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

void CInsBot::ThinkAlive()
{
    INSPROF_SCOPE(INSPROF_THINKALIVE);
    float now = Now();

    if (m_info->IsDead())
    {
        ResetLadderTraversal(true);
        ResetBodyNoCollideRecovery();
        EndSpawnNoCollide(true);
        m_state = BOT_DEAD;
        m_enemyIndex = 0;
        ResetDoorwayRunway();

        m_pathLen = 0;
        m_pathResult = BOT_PATH_NONE;
    m_pathContinues = false;
        m_moveArrivalLatched = false;
        m_haveLastNavMoveYaw = false;
        m_notedSpawn = false;
        m_objGoal = -1;
                m_haveAdvanceTarget = false; m_advanceTargetRefresh = 0.0f;
        // fresh loadout next life
        m_onSidearm = false; m_sidearmTemporary = false; m_reloadingClass = -1; memset(m_reloadMags, 0, sizeof(m_reloadMags)); memset(m_roundsFired, 0, sizeof(m_roundsFired)); m_fireAccum = 0.0f; m_lastEngineClip = -1;
        m_reloadUntil = 0; m_launcherPostFireUntil = 0; m_rpgSpeedDumpUntil = 0; m_rpgSpeedDumpEntIndex = 0; m_nadeState = 0; m_nadeKind = 0; m_m203State = 0; m_m203Pulse = false; m_nextM203 = 0.0f; m_nextRpg = 0.0f; m_nextGrenade = 0.0f; m_nextSmoke = 0.0f; m_firing = false;
        if (m_squadLeaderIndex > 0) g_Bots.StashPendingRespawn(this); // preserve fireteam identity across respawn
        // Reset transient squad order on death — respawn should start in FOLLOW mode
        m_squadOrder = SQUAD_ORDER_FOLLOW;
        m_squadOrderTime = 0.0f;
        m_leaderInCover = false;
        m_coverFollowSet = false;
        m_nextSuppressShot = 0.0f; m_suppressLosClear = false; m_nextSuppressLosCheck = 0.0f; m_haveSuppressRepos = false; m_nextSuppressRepos = 0.0f;
        m_ironsights = false; m_ironsightTime = 0.0f;
        m_crouchFireUntil = 0; m_haveCover = false;
        m_recoilRise = 0; m_burstShots = 0; m_wp = 0; m_wpToggledMode = false; m_wpName[0] = 0;
        m_haveHoldSpot = false; m_patrolling = false;
        m_holdUntil = 0.0f; m_sniperHoldUntil = 0.0f;
        m_dropHoldSince = 0.0f; m_dropHoldExpired = false;
        m_avoidObj = -1; m_avoidUntil = 0.0f; m_nextTeleportOk = 0.0f;
        m_hearUntil = 0; m_nextHearTime = 0;   // stop investigating footsteps when dead
        m_doorYieldUntil = 0;   // leave the door queue
        m_doorYieldSlot = 0;
        m_chokeProgressTime = 0; m_chokeBackoffUntil = 0;
        m_blocked = false; m_chokeBackedOut = false;
        m_followSpeed = 0.0f; m_passSide = 0.0f; m_passUntil = 0.0f;
        m_queueLeaderSlot = 0; m_queueWaitSince = 0.0f; m_queueBreakUntil = 0.0f;
        m_havePortalSpan = false; m_chokeCacheTime = 0.0f;
        m_corridorAhead = false; m_corridorLatched = false; m_corridorAheadDist = 0.0f;
        m_dropAhead = false; m_dropAheadDist = 0.0f;
        m_lastButtons = 0;
        m_hasPending = false;
        m_nextJoinCmdTime = now + 3.0f;
        RollCustomizationForLife();
        return;
    }

    DumpPendingRpgSpeed(now);

    if (m_reloadUntil > 0.0f && now >= m_reloadUntil)
    {
        m_reloadUntil = 0.0f;
        m_fireAccum = 0.0f;
        if (m_reloadingClass >= 0 && m_reloadingClass < INSBOT_COMBAT_CLASS_COUNT)
        {
            m_roundsFired[m_reloadingClass] = 0;
            m_reloadingClass = -1;
        }
        else
        {
            int cls = m_wp ? ((const WeaponProfile *)m_wp)->combatClass : 0;
            if (cls >= 0 && cls < INSBOT_COMBAT_CLASS_COUNT) m_roundsFired[cls] = 0;
        }
    }

    Vector pos = m_info->GetAbsOrigin();

    // learn this team's home from where it spawns (once per life).
    // IMPORTANT: on the first frame(s) after respawn GetAbsOrigin can still read
    // (0,0,0) - the engine has flipped IsDead() to false but hasn't yet teleported
    // the player onto its spawn point. Sampling that frame collapses the team home
    // to the world origin (which then makes "nearest to home" mean "nearest to the
    // map origin" and breaks objective picking entirely). So don't do anything -
    // and above all don't latch the home - until we actually have a real position.
    if (!m_notedSpawn)
    {
        if (pos.x * pos.x + pos.y * pos.y + pos.z * pos.z < 1.0f)
            return;                      // not positioned yet - retry next frame

        g_Objectives.NoteSpawn(m_info->GetTeamIndex(), pos);
        m_notedSpawn = true;
        m_objGoal = -1;
                m_haveAdvanceTarget = false; m_advanceTargetRefresh = 0.0f;
        m_objRepick = now;
        m_lastHealth = m_info->GetHealth();   // baseline so we don't "shout hit" on spawn
        // reset stuck/progress tracking for the new life
        m_stuckSince = 0; m_stuckStage = 0;
        m_haveNavDest = false; m_bestDist = 3.4e38f; m_navDestChanged = false;
        m_moveArrivalLatched = false; m_haveLastNavMoveYaw = false;
        m_haveRescueLink = false; m_haveLastReportedLink = false;
        ResetLadderTraversal(true);
        m_holdUntil = 0.0f; m_sniperHoldUntil = 0.0f;
        m_dropHoldSince = 0.0f; m_dropHoldExpired = false;
        m_avoidObj = -1; m_avoidUntil = 0.0f;
        m_nextRpg = 0.0f; m_nextM203 = 0.0f;
        m_haveSniperSpot = false; m_sniperRelocateTime = 0.0f; m_sniperKills = 0;
        m_sniperArea = -1; m_sniperObj = -1; m_sniperTeam = 0;
        m_sniperStance = SNIPER_STANCE_NONE; m_sniperScore = 0.0f;
        m_sniperHearingDetour = false;
        m_sniperHearingReturnTime = 0.0f;
        m_hearUntil = 0; m_nextHearTime = 0; m_hearTarget = Vector(0, 0, 0);
        m_doorYieldUntil = 0;   // leave the door queue
        m_doorYieldSlot = 0;
        m_chokeProgressTime = 0; m_chokeBackoffUntil = 0;
        m_blocked = false; m_chokeBackedOut = false;
        m_followSpeed = 0.0f; m_passSide = 0.0f; m_passUntil = 0.0f;
        m_queueLeaderSlot = 0; m_queueWaitSince = 0.0f; m_queueBreakUntil = 0.0f;
        m_laneClass = -1; m_laneIndex = 0;
        m_havePortalSpan = false; m_chokeCacheTime = 0.0f;
        m_corridorAhead = false; m_corridorLatched = false; m_corridorAheadDist = 0.0f;
        m_dropAhead = false; m_dropAheadDist = 0.0f;
        m_spawnStandTicks = 66;
        m_nextReloadTime = now + frand(25.0f, 45.0f);
        m_fireAccum = 0.0f;
        if (m_ctrl)
        {
            // Removed givecurrentammo here too
            helpers->ClientCommand(m_edict, "creinforce");
        }
        BeginSpawnNoCollide(pos);
    }

    m_haveNavDest = false;   // set true by movement code below if a dest is chosen
    m_traversingLadder = false;
    m_navTurnAngle = 0.0f;

    extern CfgVar insbot_freeze;
    // The game suppresses player movement between round_reset and
    // round_unfrozen. Do not mistake that intentional freeze for a failed NAV
    // link, and never allow force-goto to override the round safety guard.
    // Reset ordinary stuck sampling so no pre-freeze duration fires after the
    // round opens. A proven body-recovery obligation is preserved and its
    // active-time counters pause because UpdateBodyNoCollide is not run here.
    bool manualFreeze = insbot_freeze.GetBool() && !g_ForceGotoActive;
    UpdateSpawnNoCollide(pos, !g_RoundFrozen && !manualFreeze);
    if (g_RoundFrozen || manualFreeze)
    {
        m_stuckSince = 0.0f;
        m_stuckStage = 0;
        if (!m_bodyNoCollideActive && !m_bodyNoCollideEscalating)
            m_haveRescueLink = false;
        m_haveNavDest = false;
        m_bestDist = 3.4e38f;
        m_navDestChanged = false;
        m_unstuckUntil = 0.0f;
        m_stuckCheckPos = pos;
        m_stuckCheckTime = now + 0.5f;
        m_pendingFwd = 0; m_pendingSide = 0; m_pendingButtons = 0; m_pendingView = m_curAngles;
        m_hasPending = true;
        RunMove(0, 0, 0, m_curAngles);
        return;
    }

    // ---- resolve the held weapon's profile (re-read name ~2 Hz) ----
    if (now >= m_wpNextCheck || !m_wp)
    {
        m_wpNextCheck = now + 0.5f;
        const char *wn = m_info ? m_info->GetWeaponName() : 0;
        if (!wn || !wn[0]) wn = "default";
        const WeaponProfile *found = WeaponFind(wn);
        if (found && _stricmp(wn, m_wpName))
        {
            _snprintf(m_wpName, sizeof(m_wpName), "%s", wn);
            m_wp = found;
            m_wpToggledMode = false;
            m_fireAccum = 0.0f;
            if (found->combatClass != WC_PISTOL && !(found->flags & WF_LAUNCHER))
            {
                _snprintf(m_primaryWeaponName, sizeof(m_primaryWeaponName), "%s", wn);
            }
            m_haveSniperSpot = false;
            m_sniperStance = SNIPER_STANCE_NONE;
            // No givecurrentammo here! The engine already gave us a full magazine on spawn.
        }
    }
    if (!m_wp) m_wp = &s_wpDefault;

    // ---- ammo sync: reads the true clip ammo from the engine memory ----
    if (s_offset_m_hActiveWeapon >= 0 && s_offset_m_iClip1 >= 0)
    {
        IServerUnknown *pUnk = m_edict->GetUnknown();
        if (pUnk)
        {
            void *pBase = pUnk->GetBaseEntity();
            if (pBase)
            {
                int hWeapon = *(int*)((char*)pBase + s_offset_m_hActiveWeapon);
                int weaponIndex = hWeapon & 0xFFF;
                if (weaponIndex > 0 && weaponIndex < 2048)
                {
                    edict_t *pWep = engine->PEntityOfEntIndex(weaponIndex);
                    if (pWep && !pWep->IsFree() && pWep->GetUnknown())
                    {
                        IServerNetworkable *pNet = pWep->GetUnknown()->GetNetworkable();
                        const char *wepClass = pNet ? pNet->GetClassName() : 0;
                        bool isGrenadeOrMelee = wepClass && (strstr(wepClass, "m67") || strstr(wepClass, "m18")
                                              || strstr(wepClass, "rgd5") || strstr(wepClass, "anm14")
                                              || strstr(wepClass, "f1") || strstr(wepClass, "grenade")
                                              || strstr(wepClass, "smoke") || strstr(wepClass, "knife")
                                              || strstr(wepClass, "c4"));

                        void *pWepBase = pWep->GetUnknown()->GetBaseEntity();
                        if (pWepBase)
                        {
                            int clip = *(int*)((char*)pWepBase + s_offset_m_iClip1);
                            if (clip >= 0 && !isGrenadeOrMelee)
                            {
                                const WeaponProfile *wp = (const WeaponProfile *)m_wp;
                                if (wp && wp->mag > 0)
                                {
                                    // 1. Grant reserve ammo only while this weapon class still
                                    // has AI-budget magazines available. The reload budget itself
                                    // is enforced below when the bot presses reload.
                                    int reserveClass = CombatClassForWeapon(wp);
                                    int reserveLimit = CombatReserveMags(wp);
                                    if (now > m_spawnTime + 1.0f && m_reloadMags[reserveClass] < reserveLimit)
                                    {
                                        int rosterSlot = (int)(this - g_Bots.m_bots);
                                        static float nextGiveAmmo[64] = {0};
                                        if (rosterSlot >= 0 && rosterSlot < 64 && now > nextGiveAmmo[rosterSlot])
                                        {
                                            helpers->ClientCommand(m_edict, "givecurrentammo");
                                            nextGiveAmmo[rosterSlot] = now + 5.0f;
                                        }
                                    }

                                    // 2. Enslave ordinary firearm clips to the AI tracker. Launchers
                                    // are intentionally excluded: their missile creation and weapon
                                    // state are coupled, so rewriting the clip around PrimaryAttack
                                    // can invalidate a newly launched projectile.
                                    if (!(wp->flags & WF_LAUNCHER))
                                    {
                                        int desiredClip = wp->mag - m_roundsFired[reserveClass];
                                        if (desiredClip < 0) desiredClip = 0;

                                        // If the AI is intentionally reloading, hold the engine clip at 0
                                        // so it actually plays the reload animation safely.
                                        if (now < m_reloadUntil)
                                            desiredClip = 0;

                                        *(int*)((char*)pWepBase + s_offset_m_iClip1) = desiredClip;
                                    }
                                }

                                m_lastEngineClip = *(int*)((char*)pWepBase + s_offset_m_iClip1);
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- damage callouts: "I'm hit" on a real hit, "need backup" when low ----
    {
        int hp = m_info->GetHealth();
        if (m_lastHealth > 0 && hp > 0 && hp < m_lastHealth - 8)
        {
            Say(hp <= insbot_cover_health.GetInt() ? VOX_HELP : VOX_HURT);
            m_suppressedUntil = now + insbot_suppress_time.GetFloat(); // suppression response: react regardless of remaining HP
        }
        m_lastHealth = hp;
    }

    // ---------------- enemy scan (10 Hz) ----------------
    if (!insbot_detect.GetBool() && m_enemyIndex)
        m_enemyIndex = 0;   // detection disabled: drop any tracked target immediately
    if (now >= m_nextEnemyScan)
    {
        m_nextEnemyScan = now + 0.1f;
        int e = FindEnemy();
        if (e)
        {
            if (e != m_enemyIndex)
            {
                m_enemyIndex = e;
                m_enemySeenTime = now;
                Say(VOX_ENGAGE);        // "engaging / open fire"
                if (insbot_debug.GetBool())
                {
                    edict_t *te = engine->PEntityOfEntIndex(e);
                    IPlayerInfo *tp = te ? playerinfomanager->GetPlayerInfo(te) : 0;
                    if (tp)
                    {
                        Vector tpos = tp->GetAbsOrigin();
                        BotDbg("[insbots] %s targeting %s (idx %d, team %d) at (%.0f %.0f %.0f)\n",
                               m_name, tp->GetName(), e, tp->GetTeamIndex(), tpos.x, tpos.y, tpos.z);
                    }
                }
            }
            m_enemyLastSeen = now;
            edict_t *ee = engine->PEntityOfEntIndex(e);
            IPlayerInfo *pi = ee ? playerinfomanager->GetPlayerInfo(ee) : 0;
            if (pi) m_enemyLastPos = pi->GetAbsOrigin();
        }
        else if (m_enemyIndex && now - m_enemyLastSeen > insbot_lose_time.GetFloat())
        {
            m_enemyIndex = 0;   // lost them: stop tracking/aiming (kills "ghost" aim)
        }
    }

    QAngle view = m_curAngles;
    float fwd = 0, side = 0;
    int buttons = 0;

    // ---- Fix D: leader anti-boxing — EARLY, before all other movement ----
    // Get out of the leader's way outranks stuck recovery, objective push,
    // hearing, choke queues, and formation. The previous placement inside
    // UpdateSquadFollow was starved by higher-priority branches (stuck
    // recovery, body grace, choke queues) in congested play, so it never ran.
    if (IsSquadFollower())
    {
        edict_t *le = engine->PEntityOfEntIndex(m_squadLeaderIndex);
        IPlayerInfo *lpi = (le && !le->IsFree()) ? playerinfomanager->GetPlayerInfo(le) : 0;
        if (lpi && !lpi->IsDead())
        {
            Vector lpos = lpi->GetAbsOrigin();
            Vector toBot = pos - lpos; toBot.z = 0.0f;
            float distToLeader = sqrtf(toBot.x * toBot.x + toBot.y * toBot.y);

            if (distToLeader > 1.0f && distToLeader < 150.0f)
            {
                // Direction: leader velocity if moving, else leader's view yaw.
                Vector moveDir(0, 0, 0);
                float ldt = (m_lastLeaderTime > 0.0f) ? (now - m_lastLeaderTime) : 0.0f;
                if (ldt > 0.02f && ldt < 1.0f)
                {
                    Vector lvel = (lpos - m_lastLeaderPos) / ldt;
                    float spd = sqrtf(lvel.x * lvel.x + lvel.y * lvel.y);
                    if (spd > 5.0f)
                    {
                        moveDir.x = lvel.x / spd;
                        moveDir.y = lvel.y / spd;
                    }
                }
                if (moveDir.x == 0.0f && moveDir.y == 0.0f)
                {
                    QAngle lang = lpi->GetAbsAngles();
                    float yawRad = lang.y * (3.14159265f / 180.0f);
                    moveDir.x = cosf(yawRad);
                    moveDir.y = sinf(yawRad);
                }

                float latDist = fabsf(toBot.x * (-moveDir.y) + toBot.y * moveDir.x);
                float alongDist = toBot.x * moveDir.x + toBot.y * moveDir.y;

                if (insbot_debug.GetBool())
                    BotDbg("[insbots] %s antibox: dist=%.0f lat=%.0f along=%.0f dir=(%.2f,%.2f)",
                           m_name, distToLeader, latDist, alongDist, moveDir.x, moveDir.y);

                if (latDist < 45.0f && alongDist > 0.0f && alongDist < 150.0f)
                {
                    float sideSign = (toBot.x * (-moveDir.y) + toBot.y * moveDir.x) > 0.0f ? 1.0f : -1.0f;
                    float moveYawDeg = atan2f(moveDir.y, moveDir.x) * (180.0f / 3.14159265f);
                    float sideYawDeg = moveYawDeg + (sideSign > 0.0f ? 90.0f : -90.0f);
                    float sideRad = sideYawDeg * (3.14159265f / 180.0f);
                    Vector sideDir(cosf(sideRad), sinf(sideRad), 0.0f);
                    Vector sidestepTarget = pos + sideDir * 70.0f;

                    if (SideStepOpen(sidestepTarget))
                    {
                        if (insbot_debug.GetBool()) BotDbg("[insbots] %s antibox SIDESTEP triggered", m_name);
                        m_sidestepActive = true;
                        // Face the leader while stepping aside; keep whatever aim
                        // the combat block already set this tick if engaged.
                        if (!m_enemyIndex)
                        {
                            view.y = fmodf(atan2f(toBot.y, toBot.x) * (180.0f / 3.14159265f) + 360.0f, 360.0f);
                            view.x = 0.0f;
                        }
                        MoveToward(sidestepTarget, fwd, side, buttons, view);
                        m_haveNavDest = false;
                        m_pendingFwd = fwd; m_pendingSide = side;
                        m_pendingButtons = buttons; m_pendingView = view;
                        m_hasPending = true;
                        RunMove(fwd, side, buttons, view);
                        return;
                    }
                    else
                    {
                        if (insbot_debug.GetBool()) BotDbg("[insbots] %s antibox blocked (SideStepOpen=false)", m_name);
                    }
                }
            }
        }
    }

    // Fix C: bot who spawns proning/ducking immediately stands up unless ducking is intended
    if (m_spawnStandTicks > 0)
    {
        m_spawnStandTicks--;
        buttons |= (1 << 20) | (1 << 21); // IN_WEAPON1 | IN_WEAPON2 (firemode key)
              
        if (!CeilingLowAt(pos))
            buttons |= IN_JUMP;
    }
    
    // Universal Prone Recovery: Bots do not know how to fight or navigate while prone.
    // If they ever end up prone (e.g. from map spawn, glitch, or artificial respawn), 
    // force them out of it immediately! We pulse the button every 20 ticks (300ms) 
    // to bypass input drops without turbo-toggling.
    float maxZ = m_info->GetPlayerMaxs().z;
    if (maxZ > 1.0f && maxZ < 30.0f)
    {
        if (gpGlobals->tickcount % 20 == 0)
            buttons |= IN_JUMP | IN_DUCK; // Force them out of prone using jump/duck
    }
    else if (!(buttons & IN_DUCK) && fwd > 50.0f && !CeilingLowAt(pos))
    {
        if (m_info->GetPlayerMaxs().z < 50.0f)
        {
            buttons |= IN_JUMP;
        }
    }

    bool engaging = false;

    // Capture-detail flag hoisted above the combat block so the combat
    // movement branch below can decide objective-first vs fight-first.
    int  myTeamEarly = m_info ? m_info->GetTeamIndex() : 0;
    bool captureCommitted = insbot_objectives.GetBool() && g_Objectives.IsReady()
                         && g_Objectives.HasHostileOrNeutral(myTeamEarly)
                         && g_Bots.IsCaptureCommitment(this, myTeamEarly);
    // A capture-detail bot is a stationary guaranteed body for its assigned
    // point, never a patrol unit. Clear a patrol inherited before commitment
    // even when the assigned objective itself did not change.
    if (captureCommitted) m_patrolling = false;

    // ---------------- combat ----------------
    if (m_enemyIndex && !ValidEnemy(m_enemyIndex))
    {
        edict_t *defeatedEdict = engine->PEntityOfEntIndex(m_enemyIndex);
        IPlayerInfo *defeatedInfo = (defeatedEdict && !defeatedEdict->IsFree())
                                  ? playerinfomanager->GetPlayerInfo(defeatedEdict) : 0;
        bool confirmedKill = defeatedInfo && defeatedInfo->IsDead();
        m_enemyIndex = 0;
        m_hearUntil = 0; m_nextHearTime = 0;   // stop chasing the last footstep origin
        Say(VOX_CLEAR);            // target eliminated / no longer valid: "clear"
        if (confirmedKill && IsSniper() && m_haveSniperSpot)
        {
            m_sniperKills++;
            int relocateKills = insbot_sniper_relocate_kills.GetInt();
            if (relocateKills > 0 && m_sniperKills >= relocateKills)
            {
                m_haveSniperSpot = false;
                m_sniperKills = 0;
                m_sniperRelocateTime = now + 25.0f;
            }
        }
    }

    // Fix slow-fire: covering fire now drives the NATIVE combat fire-control
    // state machine. When suppression is active and no enemy is engaged, we run
    // this block with a synthesized virtual target at the covering-fire marker;
    // the standard AUTO/SEMI burst logic then owns trigger timing, RPM round
    // accounting, recoil, and burst pauses — identical rhythm to a real fight.
    bool suppressCombat = (!m_enemyIndex && m_suppressFire && m_squadLeaderIndex > 0
                           && (g_Bots.m_leaderWaypoint[m_squadLeaderIndex].x != 0.0f
                               || g_Bots.m_leaderWaypoint[m_squadLeaderIndex].y != 0.0f));

    Vector livePos(0, 0, 0);
    Vector liveTarget(0, 0, 0);
    edict_t *ee = 0;
    IPlayerInfo *pi = 0;

    if (m_enemyIndex || suppressCombat)
    {
        ee = m_enemyIndex ? engine->PEntityOfEntIndex(m_enemyIndex) : 0;
        pi = ee ? playerinfomanager->GetPlayerInfo(ee) : 0;
        engaging = true;

        if (suppressCombat)
        {
            // No live enemy: the virtual target is the covering-fire marker
            // itself. LOS/range/reaction are waived below via suppress so the
            // standard burst machinery can run against a world position.
            livePos = g_Bots.m_leaderWaypoint[m_squadLeaderIndex];
            liveTarget = livePos;
            m_enemyLastPos = livePos;
            m_enemySeenTime = (m_enemySeenTime == 0.0f) ? now : m_enemySeenTime;
        }

        if (!suppressCombat)
        {
            livePos = pi->GetAbsOrigin();
            liveTarget = livePos; liveTarget.z += 45.0f;
        }

        // *** ghost fix: line of sight is re-checked THIS FRAME, every frame ***
        bool losNow = false;
        if (ee)
        {
            losNow = FindShootableTargetPoint(ee, livePos, false, liveTarget);
            if (losNow)
            {
                m_enemyLastSeen = now;
                m_enemyLastPos = livePos;
                // keep the team radio updated only while actually seen
                IntelReport(m_info->GetTeamIndex(), m_enemyIndex, livePos, now);
                if (m_squadLeaderIndex > 0)
                {
                    g_Bots.ReportSquadThreat(m_squadLeaderIndex, m_enemyIndex, livePos, now);
                }
            }
        }

        // aim at the live target only while visible; otherwise look at the last
        // known spot (no wall-tracking -> no "ghost" aiming through walls)
        Vector target = losNow ? liveTarget : (m_enemyLastPos + Vector(0, 0, 45));
        if (suppressCombat)
        {
            // Suppression: override the aim target to the covering-fire marker.
            // The standard burst state machine below owns trigger timing, RPM
            // round accounting, recoil, and burst pauses.
            target = g_Bots.m_leaderWaypoint[m_squadLeaderIndex];
        }

        const WeaponProfile *heldProfile = (const WeaponProfile *)m_wp;
        if (!heldProfile) heldProfile = &s_wpDefault;
        CombatProfile *combat = CombatForWeapon(heldProfile);
        float combatAimError = CombatValue(combat->aimError, insbot_aim_error);
        float combatAimSpeed = CombatValue(combat->aimSpeed, insbot_aim_speed);
        float combatReaction = CombatValue(combat->reaction, insbot_reaction);
        float combatEngageRange = CombatValue(combat->engageRange, insbot_vis_range);
        float combatStopDist = CombatValue(combat->stopDist, insbot_stop_dist);

        // Once a burst has committed to crouching, use the predicted crouched
        // firing origin for sustain checks. If the target is still shootable
        // upright, cancel only the crouch and preserve the engagement.
        bool fireLosNow = losNow;
        if (suppressCombat) fireLosNow = true;   // suppression target has no LOS concept
        else if (now < m_crouchFireUntil)
        {
            Vector crouchedTarget = liveTarget;
            fireLosNow = FindShootableTargetPoint(ee, livePos, true,
                                                  crouchedTarget);
            if (fireLosNow)
                target = crouchedTarget;
            if (!fireLosNow)
            {
                m_crouchFireUntil = 0.0f;
                fireLosNow = losNow;
                if (insbot_debug.GetBool())
                    BotDbg("[insbots] %s crouched fire visibility lost; returning upright\n", m_name);
            }
        }

        // aim error refresh
        if (now >= m_nextAimErrTime)
        {
            m_nextAimErrTime = now + frand(0.4f, 0.9f);
            float err = combatAimError;
            m_aimErrX = frand(-err, err) * 0.5f;
            m_aimErrY = frand(-err, err);
        }

        Vector eye = EyePos();
        Vector d = target - eye;
        float hyp = sqrtf(d.x * d.x + d.y * d.y);
        float dist = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
        float wantYaw   = atan2f(d.y, d.x) * (180.0f / 3.14159265f) + m_aimErrY;
        float wantPitch = -atan2f(d.z, hyp) * (180.0f / 3.14159265f) + m_aimErrX;

        // rotate toward target at limited speed
        float maxStep = combatAimSpeed * gpGlobals->interval_per_tick;
        // Constant-time wrapping avoids a permanent main-thread spin if a bad
        // engine angle ever reaches this path as +/-infinity.
        float dy = fmodf(wantYaw - m_curAngles.y, 360.0f);
        if (dy != dy) dy = 0.0f; // NaN/invalid input: do not propagate it
        if (dy > 180.0f) dy -= 360.0f;
        if (dy < -180.0f) dy += 360.0f;
        if (dy > maxStep) dy = maxStep; else if (dy < -maxStep) dy = -maxStep;
        view.y = m_curAngles.y + dy;
        float dp = wantPitch - m_curAngles.x;
        if (dp > maxStep) dp = maxStep; else if (dp < -maxStep) dp = -maxStep;
        view.x = m_curAngles.x + dp;

        bool aimed = (dy < 18.0f && dy > -18.0f && dp < 18.0f && dp > -18.0f);
        bool reactionDone = (now - m_enemySeenTime) >= combatReaction;
        bool inWeaponRange = (combatEngageRange > 0.0f && dist <= combatEngageRange);

        // ---- Reversible Sidearm Fallback (H-1 & N-4) ----
        if (m_onSidearm && m_sidearmTemporary && m_reloadUntil <= now)
        {
            const WeaponProfile *primWp = m_primaryWeaponName[0] ? WeaponFind(m_primaryWeaponName) : 0;
            int primClass = primWp ? primWp->combatClass : WC_RIFLE;
            int primLimit = primWp ? CombatReserveMags(primWp) : 7;
            bool primHasAmmo = (m_reloadMags[primClass] < primLimit);
            bool enemyClose = dist < insbot_close_range.GetFloat() && (losNow || now - m_enemyLastSeen < 2.0f);

            if (primHasAmmo && !enemyClose && (now - m_enemyLastSeen > 2.0f || dist > 350.0f))
            {
                m_onSidearm = false;
                m_sidearmTemporary = false;
                if (m_ctrl && m_primaryWeaponName[0])
                {
                    m_ctrl->SetActiveWeapon(m_primaryWeaponName);
                }
                m_reloadUntil = now + 0.5f;
                m_fireAccum = 0.0f;
            }
        }

        // ---- grenades (opt-in, and secondary to gunfights) ----
        // Only consider a grenade for a FAR enemy, once per cooldown window, and
        // even then only with a low probability - so bots shoot by default and
        // only occasionally lob a grenade to flush a distant/hiding target.
        if (m_nadeState == 0 && m_m203State == 0 && CanM203Fire() && insbot_m203.GetBool() && now >= m_nextM203
            && dist >= insbot_m203_min_range.GetFloat() && dist <= insbot_m203_max_range.GetFloat()
            && reactionDone && losNow && !m_firing && m_reloadUntil <= now)
        {
            m_nextM203 = now + insbot_m203_cooldown.GetFloat();
            if (frand(0.0f, 1.0f) < insbot_m203_chance.GetFloat())
            {
                Vector gt = losNow ? liveTarget : (m_enemyLastPos + Vector(0, 0, 20));
                StartM203(now, gt);
            }
        }
        else if (m_nadeState == 0 && m_m203State == 0 && insbot_grenades.GetBool() && now >= m_nextGrenade && HasWeapon(GrenadeName())
            && dist >= insbot_grenade_min_range.GetFloat() && reactionDone
            && losNow && aimed && !m_firing && m_reloadUntil <= now)
        {
            if (frand(0.0f, 1.0f) < insbot_grenade_chance.GetFloat())
            {
                m_nextGrenade = now + insbot_grenade_cooldown.GetFloat(); // Spend cooldown on throw
                Vector gt = losNow ? liveTarget : (m_enemyLastPos + Vector(0, 0, 20));
                StartGrenade(now, gt);
            }
            else
            {
                m_nextGrenade = now + 1.5f; // Short retry delay if chance roll failed
            }
        }

        if (m_nadeState != 0)
        {
            UpdateGrenade(now, target, buttons, view);
        }
        else if (m_m203State != 0)
        {
            UpdateM203(now, buttons, view);
            fwd = 0; side = 0;
            RunMove(fwd, side, buttons, view);
            return;
        }
        else
        {
            const WeaponProfile *wp = heldProfile;

            // ---- RPG-7 Rocket Launcher Firing & Cooldown System ----
            if (IsHoldingRPG() && !m_onSidearm)
            {
                if (dist < insbot_close_range.GetFloat())
                {
                    // Enemy in knife range (<200u): draw sidearm for close-quarters combat
                    m_onSidearm = true;
                    m_sidearmTemporary = true;
                    if (m_ctrl) m_ctrl->SetActiveWeapon(SidearmName());
                    m_reloadUntil = now + 0.5f;
                    Say(VOX_LOWAMMO);
                }
                else if (!CanRPGFire())
                {
                    // All RPG reserve rockets exhausted: switch permanently to sidearm
                    m_onSidearm = true;
                    m_sidearmTemporary = false;
                    if (m_ctrl) m_ctrl->SetActiveWeapon(SidearmName());
                    m_reloadUntil = now + 0.5f;
                    Say(VOX_LOWAMMO);
                }
                else
                {
                    bool teamClear = !IsTeammateInLineOfFire(EyePos(), m_enemyLastPos);
                    float muzzlePitchError = wantPitch - (view.x - m_recoilRise);
                    bool sprayAimed = (dy < 18.0f && dy > -18.0f && dp < 18.0f && dp > -18.0f
                                       && muzzlePitchError < 18.0f && muzzlePitchError > -18.0f);
                    bool canFireRPG = fireLosNow && inWeaponRange && aimed && sprayAimed 
                                   && reactionDone && teamClear && (now >= m_nextRpg);

                    if (canFireRPG)
                    {
                        buttons |= IN_ATTACK;
                        m_firing = false;
                        m_fireStateEnd = now + 0.35f;
                    }
                    else
                    {
                        m_curAngles = view;
                    }
                }
            }
            else
            {
                // ---- Standard Firearms Combat & Fire Control ----
                // Covering fire owns the magazine for the whole order window:
                // threshold top-offs are deferred; only an EMPTY mag reloads.
                const bool coverFireActive = IsSquadFollower() && m_squadOrder == SQUAD_ORDER_SUPPRESS;
                const char *pol = insbot_firemode.GetString();
                int mode = FM_SEMI;
                if (wp->modes & FM_AUTO) mode = FM_AUTO;
                else if (wp->modes & FM_BURST) mode = FM_BURST;
                if (!_stricmp(pol, "semi")) mode = FM_SEMI;
                else if (!_stricmp(pol, "burst") && (wp->modes & FM_BURST)) mode = FM_BURST;
                m_fireMode = mode;

                // per-round cadence and burst shape from the profile
                int   rpm    = (mode == FM_BURST && wp->burstRpm) ? wp->burstRpm : wp->rpm;
                if (rpm < 30) rpm = 30;
                float cyc    = 60.0f / (float)rpm;                    // seconds/round
                int   burstN = (mode == FM_AUTO) ? 10 : (mode == FM_BURST ? 3 : 1);

                // ---- recoil control factor: how well the bot fights the kick ----
                float control = CombatRecoilControl(combat, combatAimError);

                // ---- fire control: fresh this-frame LOS, aimed, reacted, not reloading ----
                bool isM203 = IsHoldingM203();
                bool useIronsights = insbot_ironsights.GetBool() && engaging && (dist > 300.0f) && !isM203;

                if (useIronsights) {
                    buttons |= INS_IN_ATTACK3; // Maintain ADS input continuously every frame
                    if (!m_ironsights) {
                        m_ironsights = true;
                        m_ironsightTime = now;
                    }
                } else {
                    m_ironsights = false;
                }

                bool ironsightsReady = !useIronsights || (now >= m_ironsightTime + 0.4f);

                bool teamClear = !IsTeammateInLineOfFire(EyePos(), m_enemyLastPos);
                float muzzlePitchError = wantPitch - (view.x - m_recoilRise);
                bool sprayAimed = (dy < 18.0f && dy > -18.0f && dp < 18.0f && dp > -18.0f
                                   && muzzlePitchError < 18.0f && muzzlePitchError > -18.0f);
                bool hasAmmo = (GetClipRemaining() > 0);
                // Fix L-2 / slow-fire: suppression relaxes LOS/range/reaction gates
                // so the native burst machinery suppresses the marked point through
                // the normal pipeline (rounds counted, reloads happen).
                bool suppress = m_suppressFire || suppressCombat;
                bool canStartFire   = (fireLosNow || suppress) && (inWeaponRange || suppress) && aimed && sprayAimed && (reactionDone || suppress) && (m_reloadUntil <= now) && hasAmmo && ironsightsReady && teamClear;
                bool canSustainFire = (fireLosNow || suppress) && (inWeaponRange || suppress) && sprayAimed && (m_reloadUntil <= now) && hasAmmo && ironsightsReady && teamClear;

                if (!m_firing && canStartFire && now >= m_fireStateEnd)
                {
                    // start a new spray / burst / tap
                    m_firing = true;
                    m_burstShots = 0;
                    int fireClass = wp ? wp->combatClass : 0;
                    if (fireClass >= 0 && fireClass < INSBOT_COMBAT_CLASS_COUNT)
                        m_roundsFired[fireClass]++;   // Count 1 round fired on burst/tap start
                    m_nextShotTime = now + cyc;
                    // Covering fire: SEMI weapons suppress at cyclic rate too —
                    // a real semi (FAL 650 RPM) should not be throttled by the
                    // slow combat tap pause. Windows still keep recoil sane.
                    if (mode == FM_AUTO || mode == FM_BURST || (mode == FM_SEMI && m_suppressFire))
                    {
                        // Hold trigger down continuously for the entire burst duration
                        m_fireStateEnd = now + (float)burstN * cyc;
                    }
                    else
                    {
                        // Semi-auto: tap once, schedule next tap
                        m_fireStateEnd = now + cyc;
                    }

                    bool atAssignedPerch = m_haveSniperSpot
                                        && xyd(pos, m_sniperSpot) <= 80.0f;
                    bool crouchRoll = atAssignedPerch
                        ? (m_sniperStance == SNIPER_STANCE_CROUCH)
                        : (frand(0, 1) < insbot_crouch_chance.GetFloat());
                    Vector crouchedTarget = liveTarget;
                    if (crouchRoll && FindShootableTargetPoint(ee, livePos, true,
                                                               crouchedTarget))
                        m_crouchFireUntil = m_fireStateEnd;
                    else
                    {
                        m_crouchFireUntil = 0.0f;
                        if (crouchRoll && insbot_debug.GetBool())
                            BotDbg("[insbots] %s crouch fire denied at start; standing shot preserved\n", m_name);
                    }
                }

                if (m_firing)
                {
                    if (!canSustainFire)
                    {
                        // Lost a clear shot, left this weapon class's effective
                        // range, kicked off-target by recoil, or began reloading: release the trigger now.
                        m_firing = false;
                        float pause = insbot_burst_pause.GetFloat()
                                    * (0.6f + wp->recoil * 0.18f) * (1.4f - control);
                        m_fireStateEnd = now + frand(0.8f, 1.25f) * pause;
                    }
                    else if (mode == FM_AUTO || mode == FM_BURST || (mode == FM_SEMI && m_suppressFire))
                    {
                        if (now < m_fireStateEnd)
                        {
                            // HOLD ATTACK CONTINUOUSLY -> REAL FULL AUTO / BURST SPRAY
                            buttons |= IN_ATTACK;
                            if (now >= m_nextShotTime)
                            {
                                m_nextShotTime = now + cyc;
                                int fireClass = wp ? wp->combatClass : 0;
                                if (fireClass >= 0 && fireClass < INSBOT_COMBAT_CLASS_COUNT)
                                    m_roundsFired[fireClass]++;   // Count subsequent rounds in continuous spray
                            }
                            m_recoilRise += 0.30f * wp->recoil * (1.0f - control) * gpGlobals->interval_per_tick / cyc;
                            if (m_recoilRise > 6.0f) m_recoilRise = 6.0f;
                        }
                        else
                        {
                            // Spray finished: pause between bursts for recoil / cone recovery
                            m_firing = false;
                            float pause = insbot_burst_pause.GetFloat()
                                        * (0.6f + wp->recoil * 0.18f) * (1.4f - control);
                            m_fireStateEnd = now + frand(0.8f, 1.25f) * pause;
                        }
                    }
                    else
                    {
                        // Semi-auto tap fire (covering fire uses a short marker
                        // cadence gap instead of the slow combat tap pause)
                        if (now >= m_fireStateEnd)
                        {
                            m_firing = false;
                            float pause = m_suppressFire
                                        ? frand(0.05f, 0.12f)
                                        : insbot_burst_pause.GetFloat() * (0.8f + wp->recoil * 0.15f);
                            m_fireStateEnd = now + frand(0.8f, 1.2f) * pause;
                        }
                        else if (m_burstShots == 0)
                        {
                            buttons |= IN_ATTACK;
                            m_burstShots = 1;
                            m_recoilRise += 0.30f * wp->recoil * (1.0f - control);
                            if (m_recoilRise > 6.0f) m_recoilRise = 6.0f;
                        }
                    }
                }
                if (now < m_crouchFireUntil) buttons |= IN_DUCK;

                // recoil climb decays when not actively firing this tick
                if (!(buttons & IN_ATTACK))
                {
                    m_recoilRise -= 12.0f * gpGlobals->interval_per_tick;
                    if (m_recoilRise < 0) m_recoilRise = 0;
                }
                // Save clean pre-recoil base tracking angles so recoil kick does
                // not double-accumulate into the next frame's aim calculation
                m_curAngles = view;
                view.x -= m_recoilRise;

                // ---- ammo management: unified reload logic ----
                bool magDry = false;
                bool magLow = false;

                int reserveClass = CombatClassForWeapon(wp);
                int reserveLimit = CombatReserveMags(wp);
                float ammoPct = wp->mag > 0 ? (float)(wp->mag - m_roundsFired[reserveClass]) / (float)wp->mag : 1.0f;
                float safePct = wp->safe_reload_cvar ? wp->safe_reload_cvar->GetFloat() : 0.60f;
                bool emptyReloadOnly = (wp && (wp->flags & WF_EMPTY_RELOAD_ONLY));
                float dryPct = emptyReloadOnly ? 0.99f : 0.85f;
                magDry = (1.0f - ammoPct) >= dryPct || (wp->mag - m_roundsFired[reserveClass] <= 0);
                magLow = ammoPct < safePct && !emptyReloadOnly;

                bool  enemyClose = dist < insbot_close_range.GetFloat()
                                   && (losNow || now - m_enemyLastSeen < 2.0f);
                // Safe window: enemy has been OUT OF SIGHT for at least 4.0 seconds (not just 1 frame)
                bool  safeWindow = !losNow && (now - m_enemyLastSeen >= 4.0f);
                bool reserveAvailable = m_reloadMags[reserveClass] < reserveLimit;
                if (insbot_reload.GetBool() && m_reloadUntil <= now)
                {
                    // During covering fire keep the primary up even when an enemy
                    // is close: an empty mag is reloaded in place below instead of
                    // switching to the pistol and ending the suppression.
                    if (magDry && enemyClose && !m_onSidearm && !coverFireActive)
                    {
                        // knife-range and primary is dry: no time to reload - draw the
                        // sidearm and keep shooting instead
                        m_onSidearm = true;
                        m_sidearmTemporary = true;
                        if (m_ctrl) m_ctrl->SetActiveWeapon(SidearmName());
                        m_reloadUntil = now + 0.5f;   // brief weapon-switch settle
                        m_fireAccum = 0.0f;
                        Say(VOX_LOWAMMO);   // "out of ammo" - going to the pistol
                    }
                    else if (magDry)
                    {
                        if (reserveAvailable)
                        {
                            // dry and either safe/far, or already on the pistol -> reload
                            buttons |= INS_IN_RELOAD;
                            m_reloadUntil = now + 2.5f;
                            m_reloadingClass = reserveClass;
                            BotDbg("[insbots] %s RELOAD (magDry: fired=%d mag=%d pct=%.2f, %d/%d reserve mags)\n", m_name, m_roundsFired[reserveClass], wp->mag, wp->mag > 0 ? (float)(wp->mag - m_roundsFired[reserveClass]) / (float)wp->mag : 1.0f, m_reloadMags[reserveClass] + 1, reserveLimit);
                            m_fireAccum = 0.0f;
                            m_reloadMags[reserveClass]++;
                            Say(VOX_RELOAD);
                        }
                        else if (!m_onSidearm && reserveClass != WC_PISTOL)
                        {
                            // This primary class has used its reserve magazines.
                            m_onSidearm = true;
                            m_sidearmTemporary = false;
                            if (m_ctrl) m_ctrl->SetActiveWeapon(SidearmName());
                            m_reloadUntil = now + 0.5f;
                            m_fireAccum = 0.0f;
                            Say(VOX_LOWAMMO);
                        }
                        else
                        {
                            // Pistol (or an already-selected sidearm) is out of its
                            // configured reserve magazines: leave it dry.
                            m_firing = false;
                        }
                    }
                    else if (magLow && safeWindow && insbot_safe_reload.GetBool() && reserveAvailable
                             && !coverFireActive)
                    {
                        // low, and the enemy just broke contact for 4s+ -> top off while it's safe
                        // (never during covering fire — deferred until the order expires)
                        buttons |= INS_IN_RELOAD;
                        m_reloadUntil = now + 2.5f;
                        m_reloadingClass = reserveClass;
                        m_fireAccum = 0.0f;
                        m_reloadMags[reserveClass]++;
                        Say(VOX_RELOAD);
                        BotDbg("[insbots] %s RELOAD (magLow safeWindow: %d/%d reserve mags)\n", m_name, m_reloadMags[reserveClass], reserveLimit);
                    }
                    else if (coverFireActive && magLow && insbot_debug.GetBool())
                    {
                        BotDbg("[insbots] %s coverfire: top-off deferred (%d/%d rounds, %d/%d reserve mags)\n",
                               m_name, wp->mag - m_roundsFired[reserveClass], wp->mag,
                               m_reloadMags[reserveClass], reserveLimit);
                    }
                }
            }

            // ---- movement while engaging ----
            // If we're reloading or hurt, break contact and run to cover; otherwise
            // close the distance when far / no LOS, and hold when in a good spot.
            int   hp = m_info->GetHealth();
            bool  hurt = (hp > 0 && hp <= insbot_cover_health.GetInt());
            bool  suppressed = now < m_suppressedUntil;
        bool  reloadActive = (m_reloadUntil > now) && losNow;   // reload alone only displaces when actually under fire
        // Fix 4: a confirmed leader-in-cover take-cover command outranks the
        // follower's own generic hurt/suppress/reload cover response.
        bool  leaderCoverActive = IsSquadFollower()
                        && m_squadOrder == SQUAD_ORDER_FOLLOW
                        && m_leaderInCover;
        // Fix: an active covering-fire order is a standing order — a reload
        // mid-suppression must not pull the bot out of position. Hurt/suppressed
        // still self-preserve.
        // Occlusion reposition keeps the order sticky too: m_suppressFire is
        // false while the marker is blocked, but the bot must not be pulled
        // off its reposition path by a generic reloadActive cover-seek.
        bool  suppressHold = IsSquadFollower() && m_squadOrder == SQUAD_ORDER_SUPPRESS
                        && !hurt;
        bool  vulnerable = insbot_takecover.GetBool()
                        && (hurt || suppressed || (reloadActive && !suppressHold))
                        && !leaderCoverActive;

            // Capture-committed objective focus: keep pushing the objective
            // while engaging distant enemies. The bot still aims and fires on
            // the move (MoveToward with an active enemy steers along the nav
            // path while view tracks the target); it only stops to fight when
            // the enemy closes inside the defend range or the bot is already
            // on the point.
            bool objectiveFocus = false;
            bool inObjZone = false;
            bool captureCornerTransit = false;
            const ObjPoint *capOp = 0;
            if (captureCommitted && insbot_capture_focus.GetBool())
            {
                // Committed bots must have a target while fighting too. Prefer
                // the roster-assigned objective so the bot stays dedicated to
                // the capture point even mid-fight; fall back to the normal
                // forced pick only if the roster hasn't assigned one yet.
                if (m_captureObj >= 0)
                {
                    if (m_objGoal != m_captureObj)
                    {
                        m_objGoal = m_captureObj;
                        if (m_objGoal != PICK_ADVANCE) { m_haveAdvanceTarget = false; m_advanceTargetRefresh = 0.0f; }
                        m_objRepick = now + 5.0f;
                        m_haveHoldSpot = false;
                        m_haveSniperSpot = false;
                        m_sniperKills = 0;
                    }
                }
                else if (m_objGoal < 0 || now >= m_objRepick)
                {
                    float aggr = insbot_aggression.GetFloat();
                    int spread = insbot_teamwork.GetBool() ? m_ordinal : -1;
                    int avoid = (now < m_avoidUntil) ? m_avoidObj : -1;
                    m_objGoal = g_Objectives.PickGoal(myTeamEarly, pos, m_objGoal,
                                                      aggr, spread, avoid, true);
                    if (m_objGoal != PICK_ADVANCE) { m_haveAdvanceTarget = false; m_advanceTargetRefresh = 0.0f; }
                    m_objRepick = now + 1.0f;
                }
            if (m_objGoal >= 0)
            {
                capOp = g_Objectives.Get(m_objGoal);
                if (capOp && g_Objectives.Relation(m_objGoal, myTeamEarly) <= 0)
                {
                    float defend = insbot_capture_defend_range.GetFloat();
                    if (defend < 0.0f) defend = 0.0f;
                    inObjZone = PlayerInsideObjective(capOp, m_info, pos);
                    if (captureCommitted && inObjZone)
                    {
                        // Combat can enter this branch before the ordinary
                        // objective movement branch has created a hold spot.
                        // Do the same doorway-clearing setup here so a nearby
                        // enemy cannot make the bot freeze on the trigger lip.
                        if (!m_haveHoldSpot)
                        {
                            m_holdSpot = ObjectiveCornerHoldPoint(capOp, pos,
                                                                  m_ordinal);
                            m_haveHoldSpot = true;
                        }
                        captureCornerTransit = xyd(pos, m_holdSpot) > 20.0f;
                    }
                    objectiveFocus = !inObjZone && dist > defend;
                }
            }
            }

            if (g_ForceGotoActive)
            {
                m_haveCover = false; m_haveHoldSpot = false; m_haveSniperSpot = false;
                if (xyd(pos, g_ForceGotoPos) > 40.0f)
                    MoveToward(g_ForceGotoPos, fwd, side, buttons, view);
                else
                { fwd = 0; side = 0; }
            }
            else if (vulnerable)
            {
                // Dead bots can't capture: hurt/reloading committed bots still
                // break contact to cover exactly like everyone else.
                if (IsSniper() && hurt)
                {
                    m_haveSniperSpot = false;
                    m_sniperRelocateTime = now + 45.0f; // Threat relocation: abandon current overlook if taking heavy fire
                }
                Vector threat = losNow ? pi->GetAbsOrigin() : m_enemyLastPos;
                if (!m_haveCover || now > m_coverUntil)
                {
                    bool hadCover = m_haveCover;
                    bool found = false;
                    int  chosenTier = COVER_NONE;
                    bool chosenLow = false;
                    if (insbot_cover_v2.GetBool() && g_Bots.CoverCount() > 0)
                    {
                        int t = COVER_NONE;
                        Vector spotTmp;
                        if (ChooseCoverSpot(threat, spotTmp, t))
                        {
                            m_coverSpot = spotTmp;
                            for (int ci = 0; ci < g_Bots.CoverCount(); ++ci)
                            {
                                const CoverPoint *cpv = g_Bots.CoverAt(ci);
                                if (cpv && cpv->pos.x == m_coverSpot.x && cpv->pos.y == m_coverSpot.y)
                                { t = cpv->tier; chosenLow = cpv->isLowOnly != 0; break; }
                            }
                            found = true;
                        }
                        chosenTier = t;
                    }
                    else
                    {
                        found = FindCover(threat, m_coverSpot);
                    }
                    if (found)
                    {
                        m_haveCover = true; m_coverUntil = now + 2.5f;
                        m_chosenCoverTier = chosenTier; m_chosenCoverIsLow = chosenLow;
                        if (!hadCover) Say(VOX_COVER);   // announce only on taking cover, not each refresh
                    }
                    else m_haveCover = false;
                }
                if (m_haveCover)
                {
                    float coverDist = xyd(pos, m_coverSpot);
                    if (coverDist > 40.0f)
                        MoveToward(m_coverSpot, fwd, side, buttons, view);
                    else
                    {
                        // FindCover returns exact NAV-area centres.  Stop once
                        // safely inside the selected area instead of repeatedly
                        // running through its centre and turning back toward it.
                        fwd = 0.0f;
                        side = 0.0f;
                    }
                    // run upright TO cover (fast), only duck once we're behind it
                    if (coverDist < 56.0f || (insbot_cover_v2.GetBool() && m_chosenCoverIsLow))
                        buttons |= IN_DUCK;
                }
            }
            else if (objectiveFocus)
            {
                m_haveCover = false;
                m_haveHoldSpot = false; m_haveSniperSpot = false;
                MoveToward(capOp->center, fwd, side, buttons, view);
            }
            else if (captureCornerTransit)
            {
                m_haveCover = false;
                MoveToward(m_holdSpot, fwd, side, buttons, view);
            }
            else if (inObjZone)
            {
                // Already on the point: hold and defend it. Never chase an
                // enemy away from a capture zone we are committed to.
                m_haveCover = false; m_haveHoldSpot = false; m_haveSniperSpot = false;
                fwd = 0; side = 0;
            }
            else if (IsSquadFollower())
            {
                // Squad follower: gradual convergence toward formation slot
                // even while engaged, instead of freezing inside 250u.
                // UpdateSquadFollow skips guard-yaw when m_enemyIndex != 0,
                // so calling it here doesn't break combat aim tracking.
                edict_t *le = engine->PEntityOfEntIndex(m_squadLeaderIndex);
                IPlayerInfo *lpi = (le && !le->IsFree()) ? playerinfomanager->GetPlayerInfo(le) : 0;
                Vector leaderPos = lpi ? lpi->GetAbsOrigin() : pos;

                const float FREEZE_RADIUS   = 75.0f;
                const float CONVERGE_RADIUS = 400.0f;

                float distToLeader = xyd(pos, leaderPos);
                UpdateSquadFollow(now, fwd, side, buttons, view);

                // Fix 4: a leader-cover tick consumed by the earlier vulnerable
                // branch sets this flag; drop the generic cover output and obey
                // the leader's cover-follow command instead.
                if (m_coverFollowSet)
                {
                    m_haveCover = false;
                    // cover-follow already produced movement; do not scale it
                    // to zero here when the follower is near its leader.
                }
                else if (m_sidestepActive)
                {
                    // Fix D: anti-boxing sidestep produced movement — preserve it.
                    // Do not run freeze/converge scaling, it zeroes the sidestep.
                }
                else if (distToLeader < FREEZE_RADIUS)
                {
                    fwd *= 0.0f; side *= 0.0f;
                }
                else if (distToLeader < CONVERGE_RADIUS)
                {
                    float scale = (distToLeader - FREEZE_RADIUS) / (CONVERGE_RADIUS - FREEZE_RADIUS);
                    if (scale < 0.25f) scale = 0.25f; else if (scale > 1.0f) scale = 1.0f;
                    fwd *= scale; side *= scale;
                }
                // Beyond CONVERGE_RADIUS: full follow speed from UpdateSquadFollow
            }
            else if ((dist > combatStopDist && !(IsSniper() && m_haveSniperSpot))
                     || (!losNow && !IsSniper())
                     || (!losNow && IsSniper() && !m_haveSniperSpot
                         && now - m_enemyLastSeen > 4.5f))
            {
                m_haveCover = false;
                Vector chase = losNow ? pi->GetAbsOrigin() : m_enemyLastPos;
                // Use the same movement path as objectives and cover.  The old
                // inline chase duplicated navigation but skipped personal space
                // and narrow-passage queues entirely while a bot had an enemy.
                MoveToward(chase, fwd, side, buttons, view);
            }
            else
            {
                m_haveCover = false;
                if (IsSniper() && losNow && m_haveSniperSpot
                    && m_sniperStance == SNIPER_STANCE_CROUCH)
                {
                    // Only crouch when this perch was explicitly validated
                    // from the lower firing origin.  Half-height cover often
                    // requires the stored standing stance to preserve LOS.
                    buttons |= IN_DUCK;
                }
            }
        }
    }

    // ---------------- navigation: objectives, else roam ----------------
    if (!engaging)
    {
        m_firing = false;

        // Fix L-2/slow-fire: suppression firing is handled by the NATIVE combat
        // fire-control block above, gated on (m_enemyIndex || suppressCombat)
        // with a synthesized waypoint target. No extra trigger logic here.
        if (m_onSidearm && m_sidearmTemporary && m_reloadUntil <= now)
        {
            const WeaponProfile *primWp = m_primaryWeaponName[0] ? WeaponFind(m_primaryWeaponName) : 0;
            int primClass = primWp ? primWp->combatClass : WC_RIFLE;
            int primLimit = primWp ? CombatReserveMags(primWp) : 7;
            bool primHasAmmo = (m_reloadMags[primClass] < primLimit);
            if (primHasAmmo && (now - m_enemyLastSeen > 2.0f))
            {
                m_onSidearm = false;
                m_sidearmTemporary = false;
                if (m_ctrl && m_primaryWeaponName[0])
                {
                    m_ctrl->SetActiveWeapon(m_primaryWeaponName);
                }
                m_reloadUntil = now + 0.5f;
                m_fireAccum = 0.0f;
            }
        }

        // A smoke/grenade throw in progress (started while pushing an
        // objective, no enemy in sight): drive it to completion. Only the
        // actual throw (state 2, ~0.3s) stops the bot; while the weapon
        // switch settles (state 1) and while switching back (state 3) it
        // keeps navigating, so the column never piles up behind a frozen
        // leader. The combat path drives the throw if a fight starts.
        if (m_nadeState != 0)
        {
            UpdateGrenade(now, m_nadeAim, buttons, view);
            if (m_nadeState == 2)
            {
                fwd = 0; side = 0;
                RunMove(fwd, side, buttons, view);
                return;
            }
        }
        else if (m_m203State != 0)
        {
            UpdateM203(now, buttons, view);
            fwd = 0; side = 0;
            RunMove(fwd, side, buttons, view);
            return;
        }



        // Check for enemy groups to smoke
        MaybeSmokeEnemyGroup(now, m_info->GetTeamIndex());

        int myTeam = m_info->GetTeamIndex();

        // teamwork: investigate a fresh teammate sighting only when it is in
        // radio range and this bot is not committed to filling a capture zone.
        bool investigating = false;
        if (g_ForceGotoActive)
        {
            m_haveHoldSpot = false; m_haveSniperSpot = false;
            if (xyd(pos, g_ForceGotoPos) > 40.0f)
                MoveToward(g_ForceGotoPos, fwd, side, buttons, view);
            else
            { fwd = 0; side = 0; }
            investigating = true; // Skip normal goal roaming when goto override is active
        }
        else if (!IsSquadFollower()
                 && !(captureCommitted && insbot_capture_focus.GetBool())
                 && !m_haveSniperSpot
                 && insbot_teamsight.GetBool() && insbot_teamwork.GetBool())
        {
            INSPROF_SCOPE(INSPROF_TEAMSIGHT);
            Vector sight;
            float intelRange = insbot_intel_range.GetFloat();
            int who = IntelFreshest(myTeam, now, insbot_intel_time.GetFloat(), pos, intelRange, sight);
            if (who)
            {
                Vector d = sight - pos;
                float dist = sqrtf(d.x * d.x + d.y * d.y);
                // move to support if the contact is meaningfully far. IntelFreshest
                // already rejects reports outside intel_range.
                if (dist > 400.0f)
                {
                    MoveToward(sight, fwd, side, buttons, view);
                    investigating = true;
                }
            }
        }
        // Committed capture bots stay on the objective: heard footsteps do
        // not pull them off their assigned point (radio intel above is gated
        // the same way). They still react to enemies they actually see.
        // An assigned sniper may BEGIN a footstep detour only after reaching
        // the perch. Once it has left the perch, an already-active hearing
        // timer is allowed to finish; no fresh detour can replace it. The
        // assignment/reservation remains intact, so ordinary overwatch
        // movement brings the sniper back as soon as UpdateHearing completes.
        bool sniperAtPerch = m_haveSniperSpot
                          && xyd(pos, m_sniperSpot) <= 65.0f;
        bool sniperDetourExpired = m_haveSniperSpot && m_sniperHearingDetour
                                 && now >= m_sniperHearingReturnTime;
        if (sniperDetourExpired)
        {
            m_sniperHearingDetour = false;
            m_sniperHearingReturnTime = 0.0f;
            m_hearUntil = 0.0f;
            BotDbg("[insbots] %s footstep investigation timed out; returning to sniper perch\n",
                   m_name);
        }
        bool sniperHearingAllowed = !m_haveSniperSpot || sniperAtPerch
                                  || m_sniperHearingDetour;
        bool hearingMoved = false;
        if (!investigating && !IsSquadFollower() && !(captureCommitted && insbot_capture_focus.GetBool())
            && sniperHearingAllowed)
            hearingMoved = UpdateHearing(now, pos, fwd, side, buttons, view);
        if (hearingMoved)
        {
            // heard enemy movement: investigate the footstep origin
            investigating = true;
            if (m_haveSniperSpot && sniperAtPerch && !m_sniperHearingDetour)
            {
                m_sniperHearingDetour = true;
                float detourTime = insbot_hear_timeout.GetFloat();
                if (detourTime < 1.0f) detourTime = 1.0f;
                m_sniperHearingReturnTime = now + detourTime;
                BotDbg("[insbots] %s leaving sniper perch to investigate footsteps\n", m_name);
            }
        }
        else if (m_haveSniperSpot && m_sniperHearingDetour)
        {
            m_sniperHearingDetour = false;
            m_sniperHearingReturnTime = 0.0f;
            m_hearUntil = 0.0f;
            BotDbg("[insbots] %s footstep investigation finished; returning to sniper perch\n",
                   m_name);
        }

        bool useObj = !investigating && insbot_objectives.GetBool() && g_Objectives.IsReady();
        if (investigating)
        {
            /* movement already set above */
        }
        else if (IsSquadFollower())
        {
            // Squad Follower: maintain tactical wedge formation with human leader
            UpdateSquadFollow(now, fwd, side, buttons, view);
        }
        else if (useObj)
        {
            // (re)pick an objective to push toward
            // ATTACKER role: the moment our target becomes friendly (captured),
            // drop it and roll to the team's next attack target - attackers
            // never park on owned points. If every objective is friendly they
            // enter PICK_ADVANCE and push into enemy territory.
            float aggr = insbot_aggression.GetFloat();
            bool amAttacker = InsRoleAttacker(insbot_teamwork.GetBool() ? m_ordinal : -1, aggr);
            // Capture detail: follow the roster-assigned objective exactly.
            // The roster re-targets m_captureObj whenever the front line
            // moves, so the bot stays dedicated to the active capture point
            // instead of re-picking by distance every few seconds.
            if (captureCommitted && m_captureObj >= 0)
            {
                if (m_objGoal != m_captureObj)
                {
                    m_objGoal = m_captureObj;
                    if (m_objGoal != PICK_ADVANCE) { m_haveAdvanceTarget = false; m_advanceTargetRefresh = 0.0f; }
                    m_objRepick = now + 5.0f;
                    m_haveHoldSpot = false;
                    m_haveSniperSpot = false; m_sniperKills = 0;
                }
            }
            else if (captureCommitted && m_objGoal < 0)
            {
                m_objGoal = -1; m_objRepick = now;
                m_haveAdvanceTarget = false; m_advanceTargetRefresh = 0.0f;
                m_haveHoldSpot = false; m_haveSniperSpot = false;
            }
            if (m_objGoal >= 0 && (amAttacker || captureCommitted)
                && g_Objectives.Relation(m_objGoal, myTeam) > 0)
            {
                m_objGoal = -1; m_objRepick = now; m_haveHoldSpot = false;
                m_haveAdvanceTarget = false; m_advanceTargetRefresh = 0.0f;
                m_haveSniperSpot = false; m_sniperKills = 0; // Move up with the front line!
                // Force the roster's cached target dirty as a defensive
                // fallback; its pre-Think validity pass normally re-targets
                // the capture detail in this same frame.
                if (captureCommitted) g_Bots.MarkCaptureTargetDirty(myTeam);
            }

            // repick when we have no goal yet (-1) or the timer elapses. NOTE:
            // must be ==-1, not <0, so PICK_ADVANCE (-2) is a STABLE state that
            // isn't re-rolled every single frame.
            if (!(captureCommitted && m_captureObj >= 0)
                && (m_objGoal == -1 || now >= m_objRepick))
            {
                int spread = insbot_teamwork.GetBool() ? m_ordinal : -1;
                int avoid = (now < m_avoidUntil) ? m_avoidObj : -1;
                int newGoal = g_Objectives.PickGoal(myTeam, pos, m_objGoal,
                                                    aggr, spread, avoid, captureCommitted);
                if (newGoal != m_objGoal)
                {
                    m_haveHoldSpot = false;               // new point -> new spot
                    m_haveSniperSpot = false; m_sniperKills = 0; // drop overwatch to follow squad
                    if (newGoal >= 0) Say(VOX_MOVEUP);    // "moving up" to a fresh objective
                }
                m_objGoal = newGoal;
                if (m_objGoal != PICK_ADVANCE) { m_haveAdvanceTarget = false; m_advanceTargetRefresh = 0.0f; }
                m_objRepick = now + frand(6.0f, 10.0f);
            }

            if (m_objGoal == PICK_ADVANCE)
            {
                // Nothing left to capture or hold: push into enemy territory and
                // hunt. Advance toward the enemy home; once there, roam around it
                // so we sweep their ground and run into them (combat takes over).
                m_haveHoldSpot = false;
                if (!m_haveAdvanceTarget || now >= m_advanceTargetRefresh)
                {
                    Vector candidate;
                    if (g_Objectives.PushTarget(myTeam, pos, candidate))
                    {
                        m_advanceTarget = candidate;
                        m_haveAdvanceTarget = true;
                        m_advanceTargetRefresh = now + 5.0f;
                    }
                    else
                    {
                        m_haveAdvanceTarget = false;
                        m_advanceTargetRefresh = now + 1.0f;
                    }
                }
                if (m_haveAdvanceTarget && xyd(pos, m_advanceTarget) > 500.0f)
                {
                    MoveToward(m_advanceTarget, fwd, side, buttons, view);
                }
                else if (g_NavMesh.IsLoaded())
                {
                    if (m_goalArea < 0 || (m_pathLen > 0 && m_pathCursor >= m_pathLen))
                    {
                        int desired = g_NavMesh.RandomArea();
                        const NavAreaData *da = g_NavMesh.Area(desired);
                        int startArea = CurrentArea(pos);
                        if (startArea < 0) startArea = g_NavMesh.FindArea(pos);
                        if (startArea < 0) startArea = g_NavMesh.FindNearestArea(pos);
                        if (da && startArea >= 0)
                            m_goalArea = g_NavMesh.FindClosestReachableArea(startArea, da->center);
                        else
                            m_goalArea = desired;
                    }
                    const NavAreaData *ga = g_NavMesh.Area(m_goalArea);
                    if (ga) MoveToward(ga->center, fwd, side, buttons, view);
                    if (m_pathLen > 0 && m_pathCursor >= m_pathLen) m_goalArea = -1;
                }
                else SteerRoam(fwd, side, buttons, view);
            }
            else if (const ObjPoint *op = g_Objectives.Get(m_objGoal))
            {
                Vector d = op->center - pos;
                float distSq = d.x * d.x + d.y * d.y;
                bool insideObjective = PlayerInsideObjective(op, m_info, pos);

                // approaching a contested point: maybe smoke it to cover the push
                // (only if this bot actually carries a smoke - verified on switch)
                MaybeSmokeObjective(now, op, sqrtf(distSq), myTeam);
                MaybeM203Objective(now, op, sqrtf(distSq), myTeam);

                // Objective-specific sniper overwatch. Candidate discovery is
                // cached and trace-budgeted by the roster; while a cache is
                // still warming (or contains no valid perch), normal objective
                // movement continues without blocking the bot.
                bool overwatchActive = false;
                bool overwatchEnabled = insbot_sniper_overwatch.GetBool();
                if (m_haveSniperSpot && !overwatchEnabled)
                {
                    m_haveSniperSpot = false;
                    m_sniperKills = 0;
                }
                if (!captureCommitted && IsSniper() && overwatchEnabled
                    && now > m_sniperRelocateTime)
                {
                    if (m_haveSniperSpot
                        && (m_sniperObj != m_objGoal || m_sniperTeam != myTeam))
                        m_haveSniperSpot = false;
                    if (!m_haveSniperSpot)
                    {
                        if (g_Bots.AssignSniperPerch(this, myTeam, m_objGoal, now))
                            m_haveHoldSpot = false;
                        else
                            m_sniperRelocateTime = now + frand(1.5f, 3.0f);
                    }
                    if (m_haveSniperSpot)
                    {
                        float sdist = xyd(pos, m_sniperSpot);
                        if (sdist > 65.0f)
                        {
                            MoveToward(m_sniperSpot, fwd, side, buttons, view);
                            overwatchActive = true;
                        }
                        else
                        {
                            // Stationed at sniper overlook!
                            if (now >= m_sniperHoldUntil)
                            {
                                // Overload timer elapsed: don't sit stationary forever; pack up and move!
                                m_haveSniperSpot = false;
                                m_sniperKills = 0;
                                m_sniperRelocateTime = now + 15.0f;
                                // Perch expired: resume ordinary objective movement.
                            }
                            else
                            {
                                // Halt and use the posture whose firing origin
                                // passed the objective/approach LOS tests.
                                fwd = 0; side = 0;
                                if (m_sniperStance == SNIPER_STANCE_CROUCH)
                                    buttons |= IN_DUCK;
                                Vector scanTarget = m_sniperAimTarget;
                                float outYaw = atan2f(scanTarget.y - pos.y, scanTarget.x - pos.x) * (180.0f / 3.14159265f);
                                if (now >= m_nextHeadingChange)
                                {
                                    m_nextHeadingChange = now + frand(1.5f, 4.0f);
                                    m_headingYaw = outYaw + frand(-25.0f, 25.0f); // Scan sector across enemy approach vectors
                                }
                                view.y = m_headingYaw; view.x = 0;
                                overwatchActive = true;
                            }
                        }
                    }
                }
                if (overwatchActive)
                    goto overwatch_done;

                if (insideObjective)
                {
                    // arrived: take a DISTINCT spread spot around the point (not the
                    // center) so bots hold corners/edges instead of clumping
                    if (!m_haveHoldSpot)
                    {
                        // announce a capture only for a point we don't already own
                        if (g_Objectives.Relation(m_objGoal, myTeam) <= 0) Say(VOX_CAPTURE);
                        // roll once, on arrival, whether to patrol inside this point
                        m_patrolling = !captureCommitted && insbot_patrol.GetBool()
                                       && frand(0.0f, 1.0f) < insbot_patrol_chance.GetFloat();
                        float ang = m_ordinal * 2.399963f;   // golden angle -> even spread
                        // A committed bot is the guaranteed capture body, but
                        // trigger overlap is not a safe stopping point: it may
                        // still be occupying the doorway it entered through.
                        // Choose the farthest hull-safe corner so the bot clears
                        // the throat before it begins holding the capture.
                        m_holdSpot = captureCommitted
                                   ? ObjectiveCornerHoldPoint(op, pos, m_ordinal)
                                   : ObjectiveHoldPoint(op, pos, ang, 0.65f);
                        m_nextPatrol = now + frand(insbot_patrol_interval.GetFloat(),
                                                   insbot_patrol_interval.GetFloat() * 1.8f);
                        m_haveHoldSpot = true;
                    }
                    // patrol: once we actually OWN the point, wander to a fresh spot
                    // inside it every so often instead of standing on one mark
                    else if (m_patrolling && now >= m_nextPatrol
                             && g_Objectives.Relation(m_objGoal, myTeam) > 0)
                    {
                        float pang = frand(0.0f, 6.2831853f);
                        float scale = insbot_patrol_radius.GetFloat()
                                    * sqrtf(frand(0.0f, 1.0f));
                        m_holdSpot = ObjectiveHoldPoint(op, pos, pang, scale);
                        m_nextPatrol = now + frand(insbot_patrol_interval.GetFloat(),
                                                   insbot_patrol_interval.GetFloat() * 1.8f);
                    }

                    Vector hd = m_holdSpot - pos;
                    float hdist = sqrtf(hd.x * hd.x + hd.y * hd.y);

                    // Committed detail gets a tight arrival tolerance. The old
                    // 60u tolerance could declare a doorway-side position a
                    // valid hold before the bot had reached its interior spot.
                    float holdArrival = captureCommitted ? 20.0f : 60.0f;
                    if (hdist > holdArrival)
                    {
                        MoveToward(m_holdSpot, fwd, side, buttons, view);   // walk to my spot
                    }
                    else
                    {
                        // KEY: if this point is NOT ours yet (neutral or enemy-
                        // held) we must STAND in the zone to capture it - always,
                        // quiet or not. Only an ALREADY-CAPTURED (friendly) point
                        // is idling; on a quiet friendly point we push on instead.
                        int rel = g_Objectives.Relation(m_objGoal, myTeam);
                        bool mustCapture = (rel == 0 || rel == -1);   // neutral/hostile -> take it
                        bool activeSector = (now - m_enemyLastSeen < 12.0f);
                        // a patroller keeps holding a point it owns (wandering inside
                        // it) instead of pushing off when things go quiet
                        bool patrolHold = (m_patrolling && rel > 0);
                        // Attackers push beyond the objectives after a sweep;
                        // only a defensive role remains merely because every
                        // capturable point is friendly.
                        bool allOwned = !amAttacker
                                     && !g_Objectives.HasHostileOrNeutral(myTeam);
                        if (m_holdUntil <= now) m_holdUntil = now + frand(10.0f, 18.0f);

                        if (mustCapture || (activeSector && now < m_holdUntil) || patrolHold || allOwned)
                        {
                            // Stand on the point and scan. A committed corner
                            // holder prefers looking inward across the room;
                            // ordinary holders retain the outward tactical
                            // preference. Physical traces reject wall-facing
                            // directions for both.
                            float faceX = captureCommitted
                                        ? op->center.x - pos.x : pos.x - op->center.x;
                            float faceY = captureCommitted
                                        ? op->center.y - pos.y : pos.y - op->center.y;
                            float preferredYaw = atan2f(faceY, faceX)
                                               * (180.0f / 3.14159265f);
                            if (now >= m_nextHeadingChange)
                            {
                                m_nextHeadingChange = now + frand(2.0f, 5.0f);
                                float scanYaw = preferredYaw + frand(-25.0f, 25.0f);
                                m_headingYaw = OpenHoldYaw(pos, scanYaw);
                            }
                            view.y = m_headingYaw; view.x = 0;
                            fwd = 0; side = 0;
                        }
                        else
                        {
                            // quiet (or hold elapsed): don't camp - push toward the
                            // enemy front. If we make contact we'll re-hold.
                            m_holdUntil = 0; m_haveHoldSpot = false;
                            if (!m_haveAdvanceTarget || now >= m_advanceTargetRefresh)
                            {
                                Vector candidate;
                                if (g_Objectives.PushTarget(myTeam, pos, candidate))
                                {
                                    m_advanceTarget = candidate;
                                    m_haveAdvanceTarget = true;
                                    m_advanceTargetRefresh = now + 5.0f;
                                }
                                else
                                {
                                    m_haveAdvanceTarget = false;
                                    m_advanceTargetRefresh = now + 1.0f;
                                }
                            }
                            if (m_haveAdvanceTarget && xyd(pos, m_advanceTarget) > 400.0f)
                            {
                                MoveToward(m_advanceTarget, fwd, side, buttons, view);
                            }
                            else if (g_NavMesh.IsLoaded())
                            {
                                if (m_goalArea < 0 || (m_pathLen > 0 && m_pathCursor >= m_pathLen))
                                {
                                    int desired = g_NavMesh.RandomArea();
                                    const NavAreaData *da = g_NavMesh.Area(desired);
                                    int startArea = CurrentArea(pos);
                                    if (startArea < 0) startArea = g_NavMesh.FindArea(pos);
                                    if (startArea < 0) startArea = g_NavMesh.FindNearestArea(pos);
                                    if (da && startArea >= 0)
                                        m_goalArea = g_NavMesh.FindClosestReachableArea(startArea, da->center);
                                    else
                                        m_goalArea = desired;
                                }
                                const NavAreaData *ga = g_NavMesh.Area(m_goalArea);
                                if (ga) MoveToward(ga->center, fwd, side, buttons, view);
                                if (m_pathLen > 0 && m_pathCursor >= m_pathLen) m_goalArea = -1;
                            }
                            else SteerRoam(fwd, side, buttons, view);
                        }
                    }
                }
                else
                {
                    m_haveHoldSpot = false;
                    MoveToward(op->center, fwd, side, buttons, view);
                }
            }
            else
            {
                // no objective available; fall back to wandering
                if (g_NavMesh.IsLoaded())
                {
                    if (m_goalArea < 0 || (m_pathLen > 0 && m_pathCursor >= m_pathLen))
                    {
                        m_goalArea = g_NavMesh.RandomArea();
                    }
                    const NavAreaData *ga = g_NavMesh.Area(m_goalArea);
                    if (ga) MoveToward(ga->center, fwd, side, buttons, view);
                    if (m_pathLen > 0 && m_pathCursor >= m_pathLen) m_goalArea = -1;
                }
                else SteerRoam(fwd, side, buttons, view);
            }
        }
        else if (g_NavMesh.IsLoaded())
        {
            // no objectives: wander the mesh
            if (m_goalArea < 0 || (m_pathLen > 0 && m_pathCursor >= m_pathLen))
            {
                m_goalArea = g_NavMesh.RandomArea();
            }
            const NavAreaData *ga = g_NavMesh.Area(m_goalArea);
            if (ga) MoveToward(ga->center, fwd, side, buttons, view);
            if (m_pathLen > 0 && m_pathCursor >= m_pathLen) m_goalArea = -1;
        }
        else
        {
            SteerRoam(fwd, side, buttons, view);
        }

        // occasional reload when completely calm (never during covering fire —
        // the combat block above owns empty-mag reloads while the order is up)
        const bool coverFireActive = IsSquadFollower() && m_squadOrder == SQUAD_ORDER_SUPPRESS;
        if (now >= m_nextReloadTime)
        {
            m_nextReloadTime = now + frand(30.0f, 60.0f);
            if (now - m_enemyLastSeen > 8.0f && m_reloadUntil <= now)
            {
                const WeaponProfile *wp = (const WeaponProfile *)m_wp;
                int mag = GetMagSize();
                int left = GetClipRemaining();
                float ammoPct = mag > 0 ? (float)left / (float)mag : 1.0f;
                float safePct = wp && wp->safe_reload_cvar ? wp->safe_reload_cvar->GetFloat() : 0.60f;
                
                bool emptyReloadOnly = (wp && (wp->flags & WF_EMPTY_RELOAD_ONLY));
                bool needsReload = false;
                if (left <= 0) needsReload = true;
                else if (left < mag && ammoPct < safePct && !emptyReloadOnly)
                {
                    // check if safe
                    if (now > m_enemyLastSeen + 4.0f)
                        needsReload = true;
                }

                int reserveClass = CombatClassForWeapon(wp);
                int reserveLimit = CombatReserveMags(wp);
                bool reserveAvailable = m_reloadMags[reserveClass] < reserveLimit;
                bool launcherSettling = wp && (wp->flags & WF_LAUNCHER)
                                      && now < m_launcherPostFireUntil;
                if (insbot_reload.GetBool() && m_reloadUntil <= now && needsReload
                    && !launcherSettling && reserveAvailable && !(wp && (wp->flags & WF_LAUNCHER))
                    && !coverFireActive)
                {
                    // try to reload if safe/dry
                    buttons |= INS_IN_RELOAD;
                    m_reloadUntil = now + 2.5f;
                    m_reloadingClass = reserveClass;
                    m_fireAccum = 0.0f;
                    m_reloadMags[reserveClass]++;
                    BotDbg("[insbots] %s RELOAD\n", m_name);
                }
            }
        }
    }

overwatch_done:
    // ---------------- doors ----------------
    // Moving and a door is in the way: press +use to open it. Edge-triggered
    // with a re-press interval, since holding use does nothing extra and the
    // door may need a moment to swing.
    bool moving = (fwd > 50.0f || fwd < -50.0f || side > 50.0f || side < -50.0f);
    if (moving && !m_traversingLadder && now >= m_nextUseTime)
    {
        if (DoorAhead(view.y))
        {
            buttons |= INS_IN_USE;
            m_nextUseTime = now + 0.8f;   // let it swing; re-press if still shut
        }
        else
            m_nextUseTime = now + 0.3f;   // cheap periodic check while moving
    }

    // ---------------- doorway approach speed / sprint ----------------
    // Drop +sprint before the approach tile. The game then applies its normal
    // non-sprint run speed while the waypoint controller retains full input;
    // lowering the input itself kept leaders in the frame too long and let
    // followers compress into them.
    bool spawnTimedGrace = m_spawnNoCollideActive && !m_spawnNoCollideExpired;
    bool hadithaGate = false;
    if (!spawnTimedGrace && !m_traversingLadder)
    {
        bool collisionGrace = HasBodyNoCollide() || m_spawnNoCollideActive;
        if (collisionGrace)
        {
            float recoveryFwd = fwd, recoverySide = side;
            float oldTeleportOk = m_nextTeleportOk;
            hadithaGate = g_Bots.ControlHadithaDoor(
                this, pos, now, view.y, recoveryFwd, recoverySide);
            if (m_nextTeleportOk > oldTeleportOk)
            {
                fwd = 0.0f;
                side = 0.0f;
                buttons = 0;
            }
        }
        else
            hadithaGate = g_Bots.ControlHadithaDoor(
                this, pos, now, view.y, fwd, side);
    }
    bool hadithaNoSprint = hadithaGate || (!m_traversingLadder
        && IsHadithaDoorNoSprintZone(m_path, m_pathLen, m_pathCursor, pos));

    // Doorway apex braking timer
    if (m_doorwayRunway.active && m_doorwayRunway.turnAngle > 35.0f
        && m_doorwayRunway.distToPortal > 80.0f && m_doorwayRunway.distToPortal < 180.0f)
    {
        if (now >= m_doorwayApexBrakingUntil)
            m_doorwayApexBrakingUntil = now + 0.28f;
    }

    // CQB Threat detection at doorway
    bool cqbThreatAhead = false;
    if (m_doorwayRunway.active && m_doorwayRunway.distToPortal < 160.0f)
    {
        if (m_enemyIndex != 0) cqbThreatAhead = true;
        if (now - m_enemyLastSeen < 3.0f) cqbThreatAhead = true;
        if (m_hearUntil > now) cqbThreatAhead = true;
    }

    bool doorwaySprintAllowed = true;
    if (m_doorwayRunway.active)
    {
        if (!m_doorwayRunway.doorClear)
            doorwaySprintAllowed = false;
        if (now < m_doorwayApexBrakingUntil)
            doorwaySprintAllowed = false;
        if (cqbThreatAhead)
            doorwaySprintAllowed = false;
        if (m_doorwayRunway.isChained)
            doorwaySprintAllowed = false;
        if (TeammateInDoorwayCorridor(m_doorwayRunway, engine ? engine->IndexOfEdict(m_edict) - 1 : -1))
            doorwaySprintAllowed = false;
    }

    bool inWater = false;
    if (enginetrace)
    {
        int contents = enginetrace->GetPointContents(pos + Vector(0, 0, 16.0f));
        if (contents & (CONTENTS_WATER | CONTENTS_SLIME))
            inWater = true;
    }
    const NavAreaData *curNav = g_NavMesh.Area(CurrentArea(pos));
    if (curNav && (curNav->attributes & NAVATTR_UNDERWATER))
        inWater = true;

    // In water, strictly suppress sprint and clamp forward speed to smooth wading
    if (inWater)
    {
        buttons &= ~INS_IN_SPRINT;
        if (fwd > 220.0f) fwd = 220.0f;
    }
    else if (!hadithaNoSprint && doorwaySprintAllowed
        && !m_traversingLadder && insbot_sprint.GetBool() && !engaging && m_nadeState == 0
        && m_reloadUntil <= now && fwd > 100.0f && fabsf(side) < 60.0f
        && !(buttons & IN_DUCK))
    {
        buttons |= INS_IN_SPRINT;
    }

    // ---------------- stuck detection & recovery ----------------
    // Progress-based: a bot is "stuck" if it is trying to move but not getting
    // closer to its destination - this catches bots that oscillate or circle
    // (which raw-distance checks miss) as well as bots pinned against geometry.
    bool wantsToMove = (fwd > 10.0f || fwd < -10.0f || side > 10.0f || side < -10.0f);
    float unstuckYaw = m_haveLastNavMoveYaw ? m_lastNavMoveYaw : m_headingYaw;   // jiggle axis: path direction when available
    if (UpdateBodyNoCollide(pos, now))
    {
        RunMove(0.0f, 0.0f, 0, view);
        return;
    }
    if ((!m_spawnNoCollideActive || m_spawnNoCollideExpired)
        && now >= m_stuckCheckTime)
    {
        bool progressed = false, noProgress = false;
        float stuckDist = insbot_stuck_dist.GetFloat();   // inches that count as progress
        if (inWater)
        {
            stuckDist *= 0.50f; // Scale stuck threshold in water to accommodate fluid drag
        }
        Vector sampleDelta = pos - m_stuckCheckPos;
        float physicallyMoved = sqrtf(sampleDelta.x * sampleDelta.x + sampleDelta.y * sampleDelta.y);

        // Queue followers are intentional waiters too: they hold a gap behind
        // a lane predecessor, so they must not accumulate stuck time, reroute,
        // or abandon while the leader clears the choke.
        bool queueFollower = (m_doorYieldSlot && now < m_doorYieldUntil);
        bool ladderWait = insbot_ladder_queue.GetBool()
                       && m_ladderPhase != LADDER_PHASE_NONE
                       && !g_Bots.IsLadderOwner(this, m_ladderClimbIdx);
        bool dropHold = m_dropAhead && TeammateAtLanding()
                        && (m_dropHoldSince > 0.0f && now - m_dropHoldSince < 2.0f);
        bool collisionDraining = HasBodyNoCollide()
                              || (m_spawnNoCollideActive
                                  && m_spawnNoCollideExpired);
        bool noRouteBlock = m_pathResult == BOT_PATH_UNREACHABLE
                                && !m_hasMoveTarget && !m_haveRescueLink;
        // A ladder follower is always an intentional waiter. Unlike doorway
        // drain recovery, it must never promote itself into generic teleport
        // while a different bot owns the shaft.
        bool intentionalWait = noRouteBlock || ladderWait
                            || ((dropHold || queueFollower) && !collisionDraining);
        if (intentionalWait)
        {
            if (!m_bodyNoCollideActive && !m_bodyNoCollideEscalating)
            {
                m_stuckSince = 0.0f;
                m_stuckStage = 0;
                m_haveRescueLink = false;
            }
        }
        else if (wantsToMove && m_haveNavDest)
        {
            float ddx = m_navDest.x - pos.x, ddy = m_navDest.y - pos.y;
            float ddz = m_traversingLadder ? m_navDest.z - pos.z : 0.0f;
            float dist = sqrtf(ddx * ddx + ddy * ddy + ddz * ddz);
            if (m_traversingLadder && m_ladderClimbIdx >= 0)
            {
                const NavLadderData *ladder = g_NavMesh.Ladder(m_ladderClimbIdx);
                if (ladder)
                {
                    Vector rawLip = m_ladderClimbUp ? ladder->top : ladder->bottom;
                    Vector landing = LadderAreaInterior(m_ladderToArea, rawLip);
                    float endHeight = m_ladderClimbUp ? ladder->top.z : landing.z;
                    bool atLip = m_ladderClimbUp ? pos.z >= endHeight
                                                 : pos.z <= endHeight;
                    if (m_ladderPhase == LADDER_PHASE_DISMOUNT
                        || (m_ladderPhase == LADDER_PHASE_RECOVERY && atLip))
                    {
                        float ldx = landing.x - pos.x, ldy = landing.y - pos.y;
                        float ldz = landing.z - pos.z;
                        dist = sqrtf(ldx * ldx + ldy * ldy + ldz * ldz);
                    }
                    else
                    {
                        float remaining = m_ladderClimbUp
                            ? endHeight - pos.z : pos.z - endHeight;
                        dist = remaining > 0.0f ? remaining : 0.0f;
                    }
                }
            }
            // A new waypoint starts a new progress measurement. Comparing its
            // distance against the previous waypoint's best distance makes
            // short generated terrain tiles look continuously stuck even as
            // the path cursor advances, eventually triggering a bogus reverse
            // recovery on open ground.
            if (m_navDestChanged)
            {
                m_bestDist = dist;
                if (physicallyMoved >= 16.0f)
                {
                    progressed = true;
                }
                else
                {
                    noProgress = true;
                }
                m_navDestChanged = false;
            }
            // MoveToward establishes m_bestDist when a waypoint changes. Only
            // meaningful distance gained TOWARD that waypoint counts here;
            // backing up or sliding sideways during the jiggle must not reset
            // the continuous timer while the wall link remains uncrossed.
            else if (dist < m_bestDist - stuckDist)
            {
                m_bestDist = dist;
                progressed = true;
            }
            else noProgress = true;
            m_navDestChanged = false;
        }
        else if (wantsToMove)
        {
            // no nav destination (open roam): fall back to raw movement
            if (physicallyMoved >= stuckDist * 0.75f) progressed = true; else noProgress = true;
        }

        if (progressed)
        {
            // Waypoint churn is useful evidence for ordinary open-ground
            // navigation, but only UpdateBodyNoCollide may terminate a proven
            // body-recovery session using physical progress toward its target.
            if (!m_bodyNoCollideActive && !m_bodyNoCollideEscalating)
            {
                m_stuckSince = 0.0f;
                m_stuckStage = 0;
                m_haveRescueLink = false;
            }
        }
        else if (noProgress)
        {
            if (m_stuckSince <= 0.0f) m_stuckSince = now;
            float dur = now - m_stuckSince;

            // Body blockage receives the interval between stuck_time and the
            // teleport deadline. Geometry keeps the normal link recovery.
            float markTime = insbot_stuck_time.GetFloat();
            if (markTime < 0.2f) markTime = 0.2f;
            float teleportTime = insbot_unstuck_teleport_time.GetFloat();
            float abandonTime = markTime + 1.8f;
            if (abandonTime < teleportTime + 1.0f)
                abandonTime = teleportTime + 1.0f;

            // nudge while stuck - but only after a real stretch of no progress
            // (one missed 0.5s window is normal walking, not stuck; wiggling on
            // it made bots jump-duck-spin in circles)
            if (dur >= 0.6f && !m_traversingLadder && !HasBodyNoCollide())
            {
                m_unstuckUntil = now + 0.9f;
                // Strafe AWAY from the nearest teammate so wedged packs spread
                // instead of shoving into each other; random when alone.
                int myTeam = m_info->GetTeamIndex();
                if (m_pathLen > 0 && m_pathCursor < m_pathLen && m_hasMoveTarget)
                    unstuckYaw = atan2f(m_moveTarget.y - pos.y, m_moveTarget.x - pos.x)
                               * (180.0f / 3.14159265f);
                float yr = unstuckYaw * (3.14159265f / 180.0f);
                Vector fwdDir(cosf(yr), sinf(yr), 0);
                Vector sideDir(-sinf(yr), cosf(yr), 0);
                float nearestDist = 160.0f;
                float nearestDot = 0.0f;
                for (int i = 0; i < 64; i++)
                {
                    CInsBot *other = &g_Bots.m_bots[i];
                    if (!other->m_inUse || other == this || !other->m_info || other->m_info->IsDead())
                        continue;
                    if (other->m_info->GetTeamIndex() != myTeam) continue;
                    Vector p = other->m_info->GetAbsOrigin();
                    if (fabsf(p.z - pos.z) > 72.0f) continue;
                    float rx = p.x - pos.x, ry = p.y - pos.y;
                    float dist = sqrtf(rx * rx + ry * ry);
                    if (dist < nearestDist)
                    {
                        nearestDist = dist;
                        nearestDot = rx * sideDir.x + ry * sideDir.y;
                    }
                }
                if (nearestDist < 160.0f)
                    m_unstuckSide = nearestDot > 0.0f ? -1.0f : 1.0f;
                else
                    m_unstuckSide = (rand() & 1) ? 1.0f : -1.0f;
            }

            // Before treating the edge as broken, prove whether another player
            // body is the first obstruction and give the local cohort the
            // configured interval to separate without teleporting.
            if (dur >= markTime && m_stuckStage == 0
                && !m_bodyNoCollideAttempted && !m_traversingLadder
                && !m_ladderClimbing && !noRouteBlock && teleportTime > markTime)
            {
                float graceDuration = teleportTime - markTime;
                Vector bodyTarget = m_haveNavDest ? m_navDest
                                  : (m_hasMoveTarget ? m_moveTarget : pos);
                BeginBodyNoCollide(pos, bodyTarget, false, graceDuration);
            }

            float ladderRecoveryStart = teleportTime < markTime
                                      ? teleportTime : markTime;
            if (dur >= ladderRecoveryStart && m_stuckStage == 0
                && !m_bodyNoCollideAttempted && m_traversingLadder
                && m_ladderClimbIdx >= 0
                && g_Bots.IsLadderOwner(this, m_ladderClimbIdx))
            {
                const NavLadderData *ladder = g_NavMesh.Ladder(m_ladderClimbIdx);
                if (ladder)
                {
                    Vector lip = m_ladderClimbUp ? ladder->top : ladder->bottom;
                    if (!m_ladderClimbUp)
                        lip.z = LadderAreaInterior(m_ladderToArea, lip).z;
                    Vector bodyTarget = (m_ladderPhase == LADDER_PHASE_DISMOUNT
                                      || m_ladderPhase == LADDER_PHASE_RECOVERY)
                                      ? LadderAreaInterior(m_ladderToArea, lip) : lip;
                    int blocker = teleportTime > markTime
                        ? FindLadderBlocker(pos, bodyTarget, m_ladderClimbIdx)
                        : 0;
                    bool canGrace = teleportTime > markTime && blocker > 0
                                 && insbot_ladder_nocollide.GetBool()
                                 && insbot_stuck_nocollide.GetBool();
                    float graceDuration = teleportTime - ladderRecoveryStart;
                    if (graceDuration < 0.0f) graceDuration = 0.0f;
                    if (BeginBodyNoCollide(pos, bodyTarget, false,
                                           graceDuration, true,
                                           canGrace ? blocker : -1))
                        m_ladderPhase = LADDER_PHASE_RECOVERY;
                }
            }

            // markTime: validate/report the link we cannot cross, then drop the
            // current path and reroute. Done once per continuous stuck spell.
            if (dur >= markTime && m_stuckStage == 0
                && !m_bodyNoCollideActive && !m_bodyNoCollideEscalating)
            {
                // A queue follower never reaches here: queueing resets the
                // stuck clock above, so this only fires for a genuinely stuck
                // bot (not one holding a gap behind a teammate).
                m_stuckStage = 1;
                m_haveRescueLink = false;
                if (g_NavMesh.IsLoaded() && m_pathLen > 0 && m_pathCursor < m_pathLen)
                {
                    int here = CurrentArea(pos);
                    int next = m_path[m_pathCursor];
                    // Inside a tight doorway/corridor the player origin is
                    // often off the nav mesh (FindArea = -1), which previously
                    // made TeleportUnstuck bail: HasConnection(-1, next) is
                    // false. Fall back to the nearest nav area so the bot can
                    // still be rescued forward onto its next path node.
                    if (here < 0)
                        here = g_NavMesh.FindNearestArea(pos);
                    if (here >= 0 && g_NavMesh.HasConnection(here, next))
                    {
                        // Cache the verified edge before dropping the path so
                        // teleport remains possible even if rerouting fails.
                        m_rescueFromArea = here;
                        m_rescueToArea = next;
                        m_haveRescueLink = true;

                        float yaw = atan2f(m_navDest.y - pos.y, m_navDest.x - pos.x)
                                  * (180.0f / 3.14159265f);
                        bool transientBlock = (m_doorYieldSlot && now < m_doorYieldUntil)
                                           || DoorAhead(yaw)
                                           || TeammateBlockingPath(pos, next)
                                           || m_bodyNoCollideAttempted;
                        bool reportCooldown = m_haveLastReportedLink
                                           && m_lastReportedFrom == here
                                           && m_lastReportedTo == next
                                           && now - m_lastReportedTime < 30.0f;
                        // A typed ladder is known geometry, and ladder recovery
                        // still needs its path edge to drive the cached climb.
                        // Do not globally blacklist or erase it because one bot
                        // stalled on the shaft; the timed teleport below is the
                        // safe recovery for that physical blockage.
                        if (!m_traversingLadder && !transientBlock && !reportCooldown)
                        {
                            g_NavMesh.MarkBadConnection(here, next);
                            m_lastReportedFrom = here;
                            m_lastReportedTo = next;
                            m_lastReportedTime = now;
                            m_haveLastReportedLink = true;
                            BotDbg("[insbots] %s reported nav link %d -> %d (confidence %d)\n",
                                   m_name, here, next, g_NavMesh.BadCount(here, next));
                        }
                        else if (transientBlock)
                            BotDbg("[insbots] %s stuck link %d -> %d blocked by door/teammate; not learned\n",
                                   m_name, here, next);
                    }
                }
                if (!m_traversingLadder)
                {
                    ResetDoorwayRunway();

                    m_pathLen = 0;
                    m_pathResult = BOT_PATH_NONE;
    m_pathContinues = false;
                    m_pathCursor = 0;
                    m_repathTime = 0;
                }
                // The rebuilt path supplies the new movement axis. Do not
                // randomize the look heading: repeated recovery attempts made
                // a pinned bot visibly sweep left/right.
            }

            // unstuck_teleport_time: NavBot-style last resort. Jiggle + reroute
            // didn't free us, so warp onto the next path node (past the bad link).
            // Loop-safe: fires once per stuck spell (stage 1 -> 2); m_stuckSince is
            // NOT reset, so if we're STILL stuck the abandon below still triggers.
            bool teleported = false;
            if (!m_bodyNoCollideAttempted && m_stuckStage == 1
                && dur >= teleportTime
                && TeleportUnstuck(pos, now))
            {
                m_stuckStage = 2;
                teleported = true;
            }

            // abandonTime: hard abandon - drop the whole objective/goal, pick new
            if (!teleported && dur >= abandonTime
                && !m_bodyNoCollideAttempted)
            {
                // A queue follower never reaches here (queueing resets the
                // stuck clock above); this is a genuinely stuck bot.
                // Avoid THIS objective for a while so we don't loop back into
                // the same impassable route (e.g. an objective gated behind a
                // bad nav link); go do a different one instead.
                AbandonStuckObjective(pos, now);
            }
        }
        else
        {
            // Intentional stopping clears only ordinary stuck sampling. A
            // proven body-recovery obligation survives combat/reload/hold.
            if (!m_bodyNoCollideActive && !m_bodyNoCollideEscalating)
            {
                m_stuckSince = 0.0f;
                m_stuckStage = 0;
                m_haveRescueLink = false;
            }
            m_navDestChanged = false;
        }
        m_stuckCheckPos = pos;
        m_stuckCheckTime = now + 0.5f;
    }

    bool doorwayRecoveryOwnsMove = m_blocked || now < m_chokeBackoffUntil;
    if ((!m_spawnNoCollideActive || m_spawnNoCollideExpired)
        && !HasBodyNoCollide()
        && !m_traversingLadder && now < m_unstuckUntil
        && !(m_doorYieldSlot && now < m_doorYieldUntil)
        && !doorwayRecoveryOwnsMove)
    {
        // Generic terrain recovery keeps the route direction and nudges into
        // open side space. Doorway leaders have their own physically confirmed
        // backout/teleport controller above; reversing here after a brief slope
        // stall made healthy open-ground bots visibly walk backward.
        if (now >= m_nextUseTime) { buttons |= INS_IN_USE; m_nextUseTime = now + 0.8f; }
        if (CeilingLowAt(pos))
            buttons |= IN_DUCK;
        else
            buttons |= IN_JUMP;
        if (m_pathLen > 0 && m_pathCursor < m_pathLen && m_hasMoveTarget)
            unstuckYaw = atan2f(m_moveTarget.y - pos.y, m_moveTarget.x - pos.x)
                       * (180.0f / 3.14159265f);
        float along = insbot_move_speed.GetFloat() * 0.20f;
        // Only strafe into a side that is actually open and unoccupied;
        // jiggling into a wall or another wedged bot just boxes the pack
        // in tighter.
        float jigYr = unstuckYaw * (3.14159265f / 180.0f);
        Vector jigSide(-sinf(jigYr), cosf(jigYr), 0);
        Vector probe = pos + jigSide * (m_unstuckSide * 48.0f);
        bool sideOpen = SideStepOpen(probe);
        if (sideOpen)
        {
            int myTeam = m_info->GetTeamIndex();
            for (int i = 0; i < 64 && sideOpen; i++)
            {
                CInsBot *other = &g_Bots.m_bots[i];
                if (!other->m_inUse || other == this || !other->m_info || other->m_info->IsDead())
                    continue;
                if (other->m_info->GetTeamIndex() != myTeam) continue;
                Vector p = other->m_info->GetAbsOrigin();
                if (fabsf(p.z - probe.z) > 72.0f) continue;
                float rx = p.x - probe.x, ry = p.y - probe.y;
                if (rx * rx + ry * ry < 56.0f * 56.0f)
                    sideOpen = false;
            }
        }
        float lateral = sideOpen
                      ? m_unstuckSide * insbot_move_speed.GetFloat() * 0.45f
                      : 0.0f;
        // Recovery movement is lane-relative, but it must never steer the
        // camera. Combat keeps its enemy-facing yaw; a non-engaging bot keeps
        // its previous tick yaw instead of following alternating repath axes.
        float recoveryViewYaw = engaging ? view.y : m_curAngles.y;
        float rel = (unstuckYaw - recoveryViewYaw) * (3.14159265f / 180.0f);
        fwd  =  along * cosf(rel) - lateral * sinf(rel);
        side = -along * sinf(rel) - lateral * cosf(rel);
        if (!engaging) view.y = recoveryViewYaw;
    }

    // Prevent W-keying into enemies with an empty gun
    bool isDry = (GetClipRemaining() <= 0);
    if (!m_traversingLadder && !m_haveCover
        && ((engaging && m_reloadUntil > now) || (isDry && m_enemyIndex && !m_onSidearm)))
    {
        if (fwd > 0) fwd = 0.0f;
        side = 0.0f;
    }

    // Hard pitch clamp so the eye angles can't pass +/-89 and get normalized
    // into a wrapped value (head desync / shooting straight up).
    if (view.x > 89.0f) view.x = 89.0f;
    if (view.x < -89.0f) view.x = -89.0f;

    ValidateCrouchedFire(buttons, now);

    if (!engaging)
        m_curAngles = view;

    extern CfgVar insbot_attack;
    if (!insbot_attack.GetBool())
    {
        buttons &= ~(IN_ATTACK | INS_IN_ATTACK2 | INS_IN_ATTACK3 | INS_IN_RELOAD);
        m_firing = false;
    }

    // Store pending values so SetCommandClient can also submit them if called
    // (it is called for real clients; for fake clients the RunMove below handles it).
    m_pendingFwd     = fwd;
    m_pendingSide    = side;
    m_pendingButtons = buttons;
    m_pendingView    = view;
    m_hasPending     = true;

    RunMove(fwd, side, buttons, view);
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

// Insurgency extends CUserCmd with a client-computed muzzle angle at +0x34 and
// muzzle origin at +0x40.  CPlayerInfo::RunPlayerMove only copies the stock
// CBotCmd fields, leaving those six floats at zero; CINSPlayer::PlayerRunCommand
// then copies the zeros into these two cached player fields.  RPG PrimaryAttack
// consumes that cache verbatim, so feeding IN_ATTACK through RunPlayerMove makes
// a bot launch from an invalid transform.
//
// These player offsets and the launcher PrimaryAttack vtable slots were verified
// against the Windows srcds build 4044 server.dll shipped with INS:MIC:
//   CINSPlayer::GetMuzzle: origin +0x10A8, angle +0x10B4
//   CWeaponRPGBase::PrimaryAttack: vtable slot 212
// Keep this path launcher-only; ordinary weapons continue through RunPlayerMove.
void CInsBot::DumpPendingRpgSpeed(float now)
{
    if (m_rpgSpeedDumpUntil <= 0.0f) return;
    if (!insbot_debug.GetBool()) { m_rpgSpeedDumpUntil = 0.0f; return; }

    // Velocity is not networked for missiles in this Source branch. Measure
    // the game-owned projectile's public collision origin over two frames.
    // This reports the post-launch/post-physics velocity, rather than the raw
    // speed constant used by the weapon's Setup() routine.
    if (m_rpgSpeedDumpEntIndex > 0)
    {
        edict_t *missileEdict = engine->PEntityOfEntIndex(m_rpgSpeedDumpEntIndex);
        IServerUnknown *missileUnknown = missileEdict && !missileEdict->IsFree() ? missileEdict->GetUnknown() : 0;
        IServerNetworkable *missileNetworkable = missileUnknown ? missileUnknown->GetNetworkable() : 0;
        const char *missileClass = missileNetworkable ? missileNetworkable->GetClassName() : 0;
        ICollideable *collideable = missileUnknown ? missileUnknown->GetCollideable() : 0;
        bool isRpg = missileClass && !_stricmp(missileClass, "missile_rpg7");
        bool isM203 = missileClass && !_stricmp(missileClass, "m403_grenade");
        if (collideable && (isRpg || isM203) && now > m_rpgSpeedDumpTime)
        {
            Vector origin = collideable->GetCollisionOrigin();
            float dx = origin.x - m_rpgSpeedDumpOrigin.x;
            float dy = origin.y - m_rpgSpeedDumpOrigin.y;
            float dz = origin.z - m_rpgSpeedDumpOrigin.z;
            float elapsed = now - m_rpgSpeedDumpTime;
            if (elapsed > 0.0001f)
            {
                float vx = dx / elapsed;
                float vy = dy / elapsed;
                float vz = dz / elapsed;
                float distance = sqrtf(dx * dx + dy * dy + dz * dz);
                float speed = sqrtf(vx * vx + vy * vy + vz * vz);
                BotDbg("[insbots] %s %s velocity=(%.1f %.1f %.1f) speed=%.1f u/s "
                       "(%.1f units over %.4f s)\n",
                       m_name, missileClass, vx, vy, vz, speed, distance, elapsed);
            }
            m_rpgSpeedDumpUntil = 0.0f;
            m_rpgSpeedDumpEntIndex = 0;
            return;
        }
        m_rpgSpeedDumpEntIndex = 0; // missile expired/impacted; try to acquire again before deadline
    }

    const float maxSpawnDistanceSq = 500.0f * 500.0f;
    for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; i++)
    {
        edict_t *missileEdict = engine->PEntityOfEntIndex(i);
        if (!missileEdict || missileEdict->IsFree() || !missileEdict->GetUnknown()) continue;
        IServerUnknown *missileUnknown = missileEdict->GetUnknown();
        IServerNetworkable *missileNetworkable = missileUnknown->GetNetworkable();
        const char *missileClass = missileNetworkable ? missileNetworkable->GetClassName() : 0;
        if (!missileClass || (_stricmp(missileClass, "missile_rpg7") && _stricmp(missileClass, "m403_grenade"))) continue;

        ICollideable *collideable = missileUnknown->GetCollideable();
        if (!collideable) continue;
        Vector origin = collideable->GetCollisionOrigin();
        float dx = origin.x - m_rpgSpeedDumpMuzzle.x;
        float dy = origin.y - m_rpgSpeedDumpMuzzle.y;
        float dz = origin.z - m_rpgSpeedDumpMuzzle.z;
        if (dx * dx + dy * dy + dz * dz > maxSpawnDistanceSq) continue;

        m_rpgSpeedDumpEntIndex = i;
        m_rpgSpeedDumpOrigin = origin;
        m_rpgSpeedDumpTime = now;
        return;
    }

    if (now >= m_rpgSpeedDumpUntil)
    {
        BotDbg("[insbots] %s launcher speed dump found no nearby projectile\n", m_name);
        m_rpgSpeedDumpUntil = 0.0f;
    }
}

bool CInsBot::FireLauncherDirect(char *playerBase, const QAngle &view)
{
    if (!playerBase || s_offset_m_hActiveWeapon < 0)
        return false;

    int hWeapon = *(int*)(playerBase + s_offset_m_hActiveWeapon);
    int weaponIndex = hWeapon & 0xFFF;
    if (weaponIndex <= 0 || weaponIndex >= 2048)
        return false;

    edict_t *weaponEdict = engine->PEntityOfEntIndex(weaponIndex);
    if (!weaponEdict || weaponEdict->IsFree() || !weaponEdict->GetUnknown())
        return false;

    IServerNetworkable *networkable = weaponEdict->GetUnknown()->GetNetworkable();
    const char *className = networkable ? networkable->GetClassName() : 0;
    if (!className)
        return false;

    bool isRPG = (strstr(className, "rpg7") != NULL);
    bool isM203 = (strstr(className, "m16m203") != NULL);
    if (!isRPG && !isM203)
        return false;

    void *weaponBase = weaponEdict->GetUnknown()->GetBaseEntity();
    if (!weaponBase)
        return false;

    const float degToRad = 3.14159265f / 180.0f;
    float pitch = view.x * degToRad;
    float yaw = view.y * degToRad;
    Vector forward(cosf(pitch) * cosf(yaw),
                   cosf(pitch) * sinf(yaw),
                  -sinf(pitch));

    Vector muzzle = EyePos() + forward * 24.0f;

#ifdef _LINUX
    const int INS_MUZZLE_ORIGIN_OFFSET = INS_LINUX_MUZZLE_ORIGIN;
    const int INS_MUZZLE_ANGLE_OFFSET  = INS_LINUX_MUZZLE_ANGLES;
    const int ATTACK_SLOT = 213;
    const int OFF_SIGHTS_UP = 0x50F;
    const int OFF_SIGHT_CHANGE_TIME = 0x510;
    const int OFF_GRENADE_LOADED = 0x514;
#else
    const int INS_MUZZLE_ORIGIN_OFFSET = 0x10A8;
    const int INS_MUZZLE_ANGLE_OFFSET  = 0x10B4;
    const int ATTACK_SLOT = 212;
    const int OFF_SIGHTS_UP = 0x4FC;
    const int OFF_SIGHT_CHANGE_TIME = 0x500;
    const int OFF_GRENADE_LOADED = 0x504;
#endif

    *(float*)(playerBase + INS_MUZZLE_ORIGIN_OFFSET + 0) = muzzle.x;
    *(float*)(playerBase + INS_MUZZLE_ORIGIN_OFFSET + 4) = muzzle.y;
    *(float*)(playerBase + INS_MUZZLE_ORIGIN_OFFSET + 8) = muzzle.z;
    *(float*)(playerBase + INS_MUZZLE_ANGLE_OFFSET + 0) = view.x;
    *(float*)(playerBase + INS_MUZZLE_ANGLE_OFFSET + 4) = view.y;
    *(float*)(playerBase + INS_MUZZLE_ANGLE_OFFSET + 8) = view.z;

    void **vtable = *(void***)weaponBase;
    if (!vtable || !vtable[ATTACK_SLOT])
        return false;

    if (isM203)
    {
        *(bool*)((char*)weaponBase + OFF_SIGHTS_UP) = true;
        *(float*)((char*)weaponBase + OFF_SIGHT_CHANGE_TIME) = 0.0f;
        *(bool*)((char*)weaponBase + OFF_GRENADE_LOADED) = true;
    }

#ifdef _LINUX
    typedef void (*PrimaryAttackFn)(void *);
#else
    typedef void (__thiscall *PrimaryAttackFn)(void *);
#endif
    PrimaryAttackFn primaryAttack = (PrimaryAttackFn)vtable[ATTACK_SLOT];
    primaryAttack(weaponBase);

    if (isM203)
    {
        *(bool*)((char*)weaponBase + OFF_SIGHTS_UP) = false;
        *(float*)((char*)weaponBase + OFF_SIGHT_CHANGE_TIME) = 0.0f;
    }

    if ((isRPG || isM203) && insbot_debug.GetBool())
    {
        m_rpgSpeedDumpMuzzle = muzzle;
        m_rpgSpeedDumpUntil = Now() + 0.25f;
        m_rpgSpeedDumpEntIndex = 0;
        m_rpgSpeedDumpTime = 0.0f;
    }
    return true;
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

int CInsBot::GetRoundsFired(int cls) const
{
    if (cls < 0 || cls >= INSBOT_COMBAT_CLASS_COUNT)
    {
        const WeaponProfile *wp = (const WeaponProfile *)m_wp;
        cls = wp ? wp->combatClass : 0;
        if (cls < 0 || cls >= INSBOT_COMBAT_CLASS_COUNT) cls = 0;
    }
    return m_roundsFired[cls];
}

int CInsBot::GetMagSize() const
{
    const WeaponProfile *wp = (const WeaponProfile *)m_wp;
    return wp ? wp->mag : 30;
}

int CInsBot::GetClipRemaining() const
{
    const WeaponProfile *wp = (const WeaponProfile *)m_wp;
    int mag = wp ? wp->mag : 30;
    int cls = wp ? wp->combatClass : 0;
    if (cls < 0 || cls >= INSBOT_COMBAT_CLASS_COUNT) cls = 0;
    int left = mag - m_roundsFired[cls];
    return (left < 0) ? 0 : left;
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

// ---- through-wall debug overlay --------------------------------------------
// AddLineOverlay's noDepthTest=true makes lines render through geometry, giving
// the "wallhack" look. We draw the player bbox as 12 such lines, plus an aim
// line and (if engaging) a line to the target, and an entity text label.
static void DrawLineTW(const Vector &a, const Vector &b, int r, int g, int bl, float dur)
{
    debugoverlay->AddLineOverlay(a, b, r, g, bl, true /*noDepthTest*/, dur);
}

static void DrawBoxTW(const Vector &mn, const Vector &mx, int r, int g, int b, float dur)
{
    Vector c[8] = {
        Vector(mn.x, mn.y, mn.z), Vector(mx.x, mn.y, mn.z),
        Vector(mx.x, mx.y, mn.z), Vector(mn.x, mx.y, mn.z),
        Vector(mn.x, mn.y, mx.z), Vector(mx.x, mn.y, mx.z),
        Vector(mx.x, mx.y, mx.z), Vector(mn.x, mx.y, mx.z),
    };
    int e[12][2] = { {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4}, {0,4},{1,5},{2,6},{3,7} };
    for (int i = 0; i < 12; i++)
        DrawLineTW(c[e[i][0]], c[e[i][1]], r, g, b, dur);
}

void CInsBot::DebugDraw()
{
    if (!debugoverlay || !m_inUse || !m_edict || m_edict->IsFree() || !m_info) return;
    if (m_info->IsDead()) return;

    float dur = 0.0f;   // 0 = single frame; we redraw every frame
    Vector pos = m_info->GetAbsOrigin();
    int idx = engine->IndexOfEdict(m_edict);

    // team color
    int r = (m_teamSlot == 0) ? 255 : 64;
    int g = (m_teamSlot == 0) ? 64  : 128;
    int b = (m_teamSlot == 0) ? 64  : 255;

    // bright the box if it currently sees an enemy
    bool engaging = (m_enemyIndex != 0);
    if (engaging) { r = 255; g = 255; b = 32; }

    // Capture detail: this bot is one of the team's 15% (min 1) chosen to
    // take the next hostile/neutral objective. It stays visible through the
    // team/engaging colors so both facts are readable at once.
    bool captureCommit = m_captureCommit;

    Vector mn = pos + Vector(-16, -16, 0);
    Vector mx = pos + Vector(16, 16, 72);
    DrawBoxTW(mn, mx, r, g, b, dur);

    // Capture-committed bots get a bright white outer outline plus a vertical
    // beacon above the head, so the capture detail is obvious even inside a
    // crowd of team-colored boxes.
    if (captureCommit)
    {
        DrawBoxTW(pos + Vector(-24, -24, -4), pos + Vector(24, 24, 76), 255, 255, 255, dur);
        DrawLineTW(pos + Vector(0, 0, 84), pos + Vector(0, 0, 150), 255, 255, 255, dur);
    }

    if (IsSquadFollower())
    {
        // Squad Follower: highlight with Cyan / Emerald box and tether line to leader
        DrawBoxTW(pos + Vector(-20, -20, -2), pos + Vector(20, 20, 74), 0, 255, 200, dur);
        edict_t *le = engine->PEntityOfEntIndex(m_squadLeaderIndex);
        IPlayerInfo *lpi = (le && !le->IsFree()) ? playerinfomanager->GetPlayerInfo(le) : 0;
        if (lpi)
        {
            Vector lpos = lpi->GetAbsOrigin();
            DrawLineTW(pos + Vector(0, 0, 36), lpos + Vector(0, 0, 36), 0, 255, 200, dur);

            float refYaw = (m_squadFormationYaw != 0.0f) ? m_squadFormationYaw : lpi->GetAbsAngles().y;
            float slotAngle = refYaw + (m_squadSlot == 0 ? -120.0f : (m_squadSlot == 1 ? 120.0f : 180.0f));
            float slotDist = (m_squadSlot == 2) ? 140.0f : 90.0f;
            float rad = slotAngle * (3.14159265f / 180.0f);
            Vector targetSlot = lpos + Vector(cosf(rad) * slotDist, sinf(rad) * slotDist, 0.0f);
            DrawBoxTW(targetSlot + Vector(-10, -10, 0), targetSlot + Vector(10, 10, 20), 0, 255, 200, dur);

            const char *orderName = (m_squadOrder == SQUAD_ORDER_FOLLOW)   ? "FOLLOW" :
                                    (m_squadOrder == SQUAD_ORDER_HOLD)     ? "HOLD" :
                                    (m_squadOrder == SQUAD_ORDER_BACKUP)   ? "BACKUP" :
                                    (m_squadOrder == SQUAD_ORDER_SUPPRESS) ? "SUPPRESS" : "NONE";
            debugoverlay->AddEntityTextOverlay(idx, 4, dur, 0, 255, 200, 255,
                "SQUAD [Slot %d | %s | Lead %d]", m_squadSlot, orderName, m_squadLeaderIndex);
        }
    }

    // Doorway Runway & Clearance Overlay
    if (m_doorwayRunway.active && debugoverlay)
    {
        debugoverlay->AddLineOverlay(m_doorwayRunway.runwayPoint + Vector(0, 0, 16),
                                     m_doorwayRunway.center + Vector(0, 0, 16),
                                     255, 255, 255, false, dur);

        Vector pAxis(-m_doorwayRunway.normal.y, m_doorwayRunway.normal.x, 0.0f);
        Vector p1 = m_doorwayRunway.center + pAxis * m_doorwayRunway.halfSpan + Vector(0, 0, 16);
        Vector p2 = m_doorwayRunway.center - pAxis * m_doorwayRunway.halfSpan + Vector(0, 0, 16);
        debugoverlay->AddLineOverlay(p1, p2, 64, 255, 64, false, dur);

        int br = m_doorwayRunway.doorClear ? 32 : 255;
        int bg = m_doorwayRunway.doorClear ? 128 : 32;
        int bb = m_doorwayRunway.doorClear ? 255 : 32;
        debugoverlay->AddBoxOverlay(m_doorwayRunway.center,
                                    Vector(-16, -16, 0), Vector(16, 16, 72),
                                    QAngle(0, 0, 0), br, bg, bb, 40, dur);
    }

    // aim line
    Vector eye = EyePos();
    float yr = m_curAngles.y * (3.14159265f / 180.0f);
    float pr = m_curAngles.x * (3.14159265f / 180.0f);
    Vector aim(cosf(yr) * cosf(pr), sinf(yr) * cosf(pr), -sinf(pr));
    DrawLineTW(eye, eye + aim * 250.0f, 255, 255, 0, dur);

    // line to target
    if (engaging)
    {
        edict_t *ee = engine->PEntityOfEntIndex(m_enemyIndex);
        IPlayerInfo *pi = (ee && !ee->IsFree()) ? playerinfomanager->GetPlayerInfo(ee) : 0;
        if (pi)
        {
            Vector t = pi->GetAbsOrigin(); t.z += 45.0f;
            DrawLineTW(eye, t, 255, 0, 0, dur);
        }
    }

    // line to current objective goal
    const ObjPoint *goal = g_Objectives.Get(m_objGoal);
    if (goal)
        DrawLineTW(pos + Vector(0, 0, 36), goal->center, 255, 0, 255, dur);

    if (m_haveSniperSpot)
    {
        DrawLineTW(pos + Vector(0, 0, 45), m_sniperSpot + Vector(0, 0, 8),
                   0, 220, 255, dur);
        DrawBoxTW(m_sniperSpot + Vector(-12, -12, 0),
                  m_sniperSpot + Vector(12, 12, 28), 0, 220, 255, dur);
        DrawLineTW(m_sniperSpot + Vector(0, 0,
                   m_sniperStance == SNIPER_STANCE_CROUCH
                       ? insbot_crouch_eye_height.GetFloat() : 58.0f),
                   m_sniperAimTarget, 0, 255, 160, dur);
    }


    // take-cover: chosen cover spot (line + marker box, colored by tier)
    if (m_haveCover)
    {
        int cr, cg, cb;
        switch (m_chosenCoverTier) {
        case COVER_FULL:    cr=0;   cg=255; cb=0;   break;
        case COVER_PARTIAL: cr=255; cg=180; cb=0;   break;
        case COVER_SPOT:    cr=255; cg=120; cb=0;   break;
        default:            cr=160; cg=160; cb=160; break;
        }
        DrawLineTW(pos + Vector(0,0,36), m_coverSpot + Vector(0,0,20), cr, cg, cb, dur);
        DrawBoxTW(m_coverSpot + Vector(-10,-10,0),
                  m_coverSpot + Vector( 10, 10, m_chosenCoverIsLow ? 20 : 40),
                  cr, cg, cb, dur);
    }

    // take-cover: nearby DB cover-point scatter (tier-colored crosses, gated by draw_path)
    if (insbot_draw_path.GetBool() && g_Bots.CoverCount() > 0)
    {
        float coverScanR = 300.0f;
        float coverRSq = coverScanR * coverScanR;
        for (int ci = 0; ci < g_Bots.CoverCount() && ci < 2000; ++ci)
        {
            const CoverPoint *cp = g_Bots.CoverAt(ci);
            if (!cp) continue;
            Vector d = cp->pos - pos;
            if (d.x*d.x + d.y*d.y + d.z*d.z > coverRSq) continue;
            if (m_haveCover && cp->pos.x == m_coverSpot.x && cp->pos.y == m_coverSpot.y) continue;

            int cr, cg, cb;
            switch (cp->tier) {
            case COVER_FULL:    cr=0;   cg=255; cb=0;   break;
            case COVER_PARTIAL: cr=255; cg=180; cb=0;   break;
            default:            cr=255; cg=120; cb=0;   break;
            }
            Vector t1 = cp->pos + Vector(-6,0,4);
            Vector t2 = cp->pos + Vector( 6,0,4);
            Vector t3 = cp->pos + Vector(0,-6,4);
            Vector t4 = cp->pos + Vector(0, 6,4);
            DrawLineTW(t1, t2, cr, cg, cb, dur);
            DrawLineTW(t3, t4, cr, cg, cb, dur);
        }
    }
    // path
    if (insbot_draw_path.GetBool() && g_NavMesh.IsLoaded() && m_pathLen > 1)
    {
        for (int i = m_pathCursor; i < m_pathLen - 1; i++)
        {
            const NavAreaData *a1 = g_NavMesh.Area(m_path[i]);
            const NavAreaData *a2 = g_NavMesh.Area(m_path[i + 1]);
            if (a1 && a2)
            {
                Vector p1 = a1->center; p1.z += 8;
                Vector p2 = a2->center; p2.z += 8;
                DrawLineTW(p1, p2, 0, 255, 0, dur);
            }
        }
    }

    // text label above the head
    const char *st = "?";
    switch (m_state) { case BOT_JOINING: st = "JOIN"; break; case BOT_ALIVE: st = "ALIVE"; break; case BOT_DEAD: st = "DEAD"; break; }
    debugoverlay->AddEntityTextOverlay(idx, 0, dur, 255, 255, 255, 255,
        "%s | %s%s | enemy=%d hp=%d", m_name, st,
        captureCommit ? " | CAP" : "", m_enemyIndex, m_info->GetHealth());
    if (engaging)
        debugoverlay->AddEntityTextOverlay(idx, 1, dur, 255, 220, 120, 255,
            "ENGAGING (firing=%d)", m_firing ? 1 : 0);
    if (captureCommit)
        debugoverlay->AddEntityTextOverlay(idx, 2, dur, 0, 255, 255, 255,
            "CAPTURE COMMIT");
    if (m_haveSniperSpot)
        debugoverlay->AddEntityTextOverlay(idx, 3, dur, 0, 220, 255, 255,
            "SNIPER obj=%d score=%.1f stance=%s kills=%d",
            m_sniperObj, m_sniperScore,
            m_sniperStance == SNIPER_STANCE_CROUCH ? "CROUCH" : "STAND",
            m_sniperKills);
    if (m_haveCover)
    {
        int cr, cg, cb;
        switch (m_chosenCoverTier) {
        case COVER_FULL:    cr=0;   cg=255; cb=0;   break;
        case COVER_PARTIAL: cr=255; cg=180; cb=0;   break;
        case COVER_SPOT:    cr=255; cg=120; cb=0;   break;
        default:            cr=160; cg=160; cb=160; break;
        }
        debugoverlay->AddEntityTextOverlay(idx, 5, dur, cr, cg, cb, 255,
            "COVER tier=%d%s dist=%.0f",
            m_chosenCoverTier,
            m_chosenCoverIsLow ? " LOW" : "",
            (m_coverSpot - pos).Length());
    }
}

// ============================================================================
// CBotRoster
// ============================================================================

void CBotRoster::AssignPlayerSquad(int leaderIndex)
{
    if (leaderIndex <= 0 || leaderIndex > gpGlobals->maxClients) return;
    edict_t *le = engine->PEntityOfEntIndex(leaderIndex);
    if (!le || le->IsFree() || !le->GetUnknown()) return;
    IPlayerInfo *lpi = playerinfomanager ? playerinfomanager->GetPlayerInfo(le) : 0;
    if (!lpi || !lpi->IsConnected() || lpi->IsDead()) return;

    int leaderTeam = lpi->GetTeamIndex();
    Vector leaderPos = lpi->GetAbsOrigin();

    for (int i = 0; i < 64; i++)
    {
        if (m_bots[i].m_state == BOT_ALIVE && m_bots[i].m_squadLeaderIndex == leaderIndex)
        {
            m_bots[i].ClearSquadAssignment();
        }
        // Fix H2-a: a full re-recruit is a wipe-and-rebuild. Release the dead
        // original's pending-respawn claim so RecallFollower cannot re-attach
        // it later and create a 4th bot in a slot the new fireteam already owns.
        else if (m_bots[i].m_squadRespawnPendingForLeader == leaderIndex)
        {
            m_bots[i].m_squadRespawnPendingForLeader = 0;
            m_bots[i].m_squadRespawnSlot = 0;
        }
    }

    // Fix H2-a (part 2): RecallFollower has a SECONDARY match path via the
    // name+roster-index member tables, which the flag clear above does not
    // touch. Clear them for this leader too, or the dead original still
    // re-attaches and creates a 4th bot.
    for (int s = 0; s < 3; s++)
    {
        m_leaderMemberBotIdx[leaderIndex][s] = -1;
        m_leaderMemberName[leaderIndex][s][0] = '\0';
        m_leaderMemberDeathTime[leaderIndex][s] = 0.0f;
    }

    int activeOrder = (leaderIndex > 0 && leaderIndex <= 64) ? m_leaderSquadOrder[leaderIndex] : SQUAD_ORDER_FOLLOW;
    if (activeOrder <= 0) activeOrder = SQUAD_ORDER_FOLLOW;

    int assignedCount = 0;
    while (assignedCount < 3)
    {
        int bestIdx = -1;
        float bestDistSq = 3.4e38f;
        for (int i = 0; i < 64; i++)
        {
            if (m_bots[i].m_state == BOT_ALIVE && m_bots[i].m_info
                && m_bots[i].m_info->GetTeamIndex() == leaderTeam
                && m_bots[i].m_squadLeaderIndex == 0
                && !m_bots[i].m_captureCommit
                && !m_bots[i].IsHoldingRPG())
            {
                Vector bpos = m_bots[i].m_info->GetAbsOrigin();
                float dx = bpos.x - leaderPos.x, dy = bpos.y - leaderPos.y;
                float dsq = dx * dx + dy * dy;
                if (dsq < bestDistSq)
                {
                    bestDistSq = dsq;
                    bestIdx = i;
                }
            }
        }
        if (bestIdx >= 0)
        {
            m_bots[bestIdx].SetSquadAssignment(leaderIndex, assignedCount, activeOrder);
            if (engine)
            {
                const char *slotNames[3] = { "Left Flank", "Right Flank", "Rear Guard" };
                const char *roleStr = (assignedCount >= 0 && assignedCount < 3) ? slotNames[assignedCount] : "Escort";
                int botClientIdx = (m_bots[bestIdx].m_edict) ? engine->IndexOfEdict(m_bots[bestIdx].m_edict) : (bestIdx + 1);
                char cmdBuf[160];
                _snprintf(cmdBuf, sizeof(cmdBuf), "insbot_squad_recruit %d %d \"%s\" \"%s\"\n",
                          leaderIndex, botClientIdx, m_bots[bestIdx].m_name, roleStr);
                engine->ServerCommand(cmdBuf);
            }
            assignedCount++;
        }
        else break;
    }
}

void CBotRoster::RefillPlayerSquad(int leaderIndex)
{
    if (leaderIndex <= 0 || leaderIndex > gpGlobals->maxClients) return;
    edict_t *le = engine->PEntityOfEntIndex(leaderIndex);
    if (!le || le->IsFree() || !le->GetUnknown()) return;
    IPlayerInfo *lpi = playerinfomanager ? playerinfomanager->GetPlayerInfo(le) : 0;
    if (!lpi || !lpi->IsConnected() || lpi->IsDead()) return;

    int leaderTeam = lpi->GetTeamIndex();
    Vector leaderPos = lpi->GetAbsOrigin();

    bool slotFilled[3] = {false, false, false};
    for (int i = 0; i < 64; i++)
    {
        if (m_bots[i].m_state == BOT_ALIVE && m_bots[i].m_squadLeaderIndex == leaderIndex)
        {
            int s = m_bots[i].m_squadSlot;
            if (s >= 0 && s < 3) slotFilled[s] = true;
        }
        // Fix H-2: a slot whose original owner is dead-but-pending-respawn is
        // still owned. Recruiting into it here would double-book the slot when
        // RecallFollower re-attaches the original after the reinforcement wave.
        else if (m_bots[i].m_squadRespawnPendingForLeader == leaderIndex)
        {
            int s = m_bots[i].m_squadRespawnSlot;
            if (s >= 0 && s < 3) slotFilled[s] = true;
        }
    }

    int activeOrder = (leaderIndex > 0 && leaderIndex <= 64) ? m_leaderSquadOrder[leaderIndex] : SQUAD_ORDER_FOLLOW;
    if (activeOrder <= 0) activeOrder = SQUAD_ORDER_FOLLOW;

    for (int s = 0; s < 3; s++)
    {
        if (!slotFilled[s])
        {
            int bestIdx = -1;
            float bestDistSq = 3.4e38f;
            for (int i = 0; i < 64; i++)
            {
                if (m_bots[i].m_state == BOT_ALIVE && m_bots[i].m_info
                    && m_bots[i].m_info->GetTeamIndex() == leaderTeam
                    && m_bots[i].m_squadLeaderIndex == 0
                    && !m_bots[i].m_captureCommit
                    && m_bots[i].m_squadRespawnPendingForLeader == 0
                    && !m_bots[i].IsHoldingRPG())
                {
                    Vector bpos = m_bots[i].m_info->GetAbsOrigin();
                    float dx = bpos.x - leaderPos.x, dy = bpos.y - leaderPos.y;
                    float dsq = dx * dx + dy * dy;
                    if (dsq < bestDistSq)
                    {
                        bestDistSq = dsq;
                        bestIdx = i;
                    }
                }
            }
            if (bestIdx >= 0)
            {
                m_bots[bestIdx].SetSquadAssignment(leaderIndex, s, activeOrder);
                slotFilled[s] = true;
                m_bots[bestIdx].Say(VOX_MOVEUP);

                if (engine)
                {
                    const char *slotNames[3] = { "Left Flank", "Right Flank", "Rear Guard" };
                    const char *roleStr = (s >= 0 && s < 3) ? slotNames[s] : "Escort";
                    int botClientIdx = (m_bots[bestIdx].m_edict) ? engine->IndexOfEdict(m_bots[bestIdx].m_edict) : (bestIdx + 1);
                    char cmdBuf[160];
                    _snprintf(cmdBuf, sizeof(cmdBuf), "insbot_squad_recruit %d %d \"%s\" \"%s\"\n",
                              leaderIndex, botClientIdx, m_bots[bestIdx].m_name, roleStr);
                    engine->ServerCommand(cmdBuf);
                }
            }
        }
    }
}

void CBotRoster::DispatchSquadOrder(int leaderIndex, int order, float wpX, float wpY, float wpZ)
{
    if (leaderIndex <= 0 || leaderIndex > 64) return;

    if ((order == SQUAD_ORDER_SUPPRESS || order == SQUAD_ORDER_MOVETO) && (wpX != 0.0f || wpY != 0.0f || wpZ != 0.0f))
        m_leaderWaypoint[leaderIndex] = Vector(wpX, wpY, wpZ);
    else if (order != SQUAD_ORDER_SUPPRESS && order != SQUAD_ORDER_MOVETO)
        m_leaderWaypoint[leaderIndex] = Vector(0, 0, 0);

    if (order == SQUAD_ORDER_NONE)
    {
        m_leaderActive[leaderIndex] = false;
        m_leaderSquadOrder[leaderIndex] = SQUAD_ORDER_NONE;
        for (int i = 0; i < 64; i++)
        {
            if (m_bots[i].m_squadLeaderIndex == leaderIndex)
            {
                m_bots[i].ClearSquadAssignment();
            }
            // Fix H2-a (dismiss): dead-and-pending members must also lose their
            // claims on dismissal, or RecallFollower re-attaches them later and
            // a dismissed squad quietly grows a 4th bot on the next wave.
            else if (m_bots[i].m_squadRespawnPendingForLeader == leaderIndex)
            {
                m_bots[i].m_squadRespawnPendingForLeader = 0;
                m_bots[i].m_squadRespawnSlot = 0;
            }
        }
        for (int s = 0; s < 3; s++)
        {
            m_leaderMemberBotIdx[leaderIndex][s] = -1;
            m_leaderMemberName[leaderIndex][s][0] = '\0';
            m_leaderMemberDeathTime[leaderIndex][s] = 0.0f;
        }
        m_leaderWaypoint[leaderIndex] = Vector(0, 0, 0);
        return;
    }

    m_leaderActive[leaderIndex] = true;
    m_leaderSquadOrder[leaderIndex] = order;

    int followerCount = 0;
    for (int i = 0; i < 64; i++)
    {
        if (m_bots[i].m_state == BOT_ALIVE && m_bots[i].m_squadLeaderIndex == leaderIndex)
            followerCount++;
    }
    if (followerCount == 0)
    {
        AssignPlayerSquad(leaderIndex);
    }

    edict_t *le = engine->PEntityOfEntIndex(leaderIndex);
    IPlayerInfo *lpi = (le && !le->IsFree()) ? playerinfomanager->GetPlayerInfo(le) : 0;
    Vector lpos = lpi ? lpi->GetAbsOrigin() : Vector(0, 0, 0);
    QAngle lang = lpi ? lpi->GetAbsAngles() : QAngle(0, 0, 0);

    for (int i = 0; i < 64; i++)
    {
        if (m_bots[i].m_state == BOT_ALIVE && m_bots[i].m_squadLeaderIndex == leaderIndex)
        {
            m_bots[i].m_squadOrder = order;
            m_bots[i].m_squadOrderTime = gpGlobals ? gpGlobals->curtime : 0.0f;

            if (order == SQUAD_ORDER_HOLD)
            {
                m_bots[i].m_squadHoldPos = m_bots[i].m_info ? m_bots[i].m_info->GetAbsOrigin() : lpos;
                float sectorAngle = lang.y + (m_bots[i].m_squadSlot == 0 ? -90.0f : (m_bots[i].m_squadSlot == 1 ? 90.0f : 180.0f));
                m_bots[i].m_squadScanYaw = sectorAngle;
                m_bots[i].Say(VOX_COVER);
            }
            else if (order == SQUAD_ORDER_BACKUP)
            {
                m_bots[i].Say(VOX_HELP);
            }
            else if (order == SQUAD_ORDER_FOLLOW)
            {
                m_bots[i].Say(VOX_MOVEUP);
            }
            else if (order == SQUAD_ORDER_SUPPRESS)
            {
                m_bots[i].Say(VOX_ENGAGE);
            }
            else if (order == SQUAD_ORDER_MOVETO)
            {
                m_bots[i].Say(VOX_COVER);
            }
        }
    }
}

void CBotRoster::UpdateSquadRoster(float now)
{
    static float s_nextSquadRosterCheck = 0.0f;
    if (now < s_nextSquadRosterCheck) return;
    s_nextSquadRosterCheck = now + 1.0f;

    for (int clientIdx = 1; clientIdx <= gpGlobals->maxClients; clientIdx++)
    {
        if (clientIdx > 64 || !m_leaderActive[clientIdx]) continue;

        edict_t *e = engine->PEntityOfEntIndex(clientIdx);
        if (!e || e->IsFree() || !e->GetUnknown()) { m_leaderActive[clientIdx] = false; continue; }
        IPlayerInfo *pi = playerinfomanager ? playerinfomanager->GetPlayerInfo(e) : 0;
        if (!pi || !pi->IsConnected() || pi->IsFakeClient()) { m_leaderActive[clientIdx] = false; continue; }
        if (pi->IsDead()) continue;

        int followerCount = 0;
        for (int i = 0; i < 64; i++)
        {
            if (m_bots[i].m_state == BOT_ALIVE && m_bots[i].m_squadLeaderIndex == clientIdx)
            {
                followerCount++;
            }
            // Pending-respawn original members still own their slot.
            else if (m_bots[i].m_squadRespawnPendingForLeader == clientIdx)
            {
                followerCount++;
            }
        }

        if (followerCount < 3)
        {
            RefillPlayerSquad(clientIdx);
        }
    }
}

void CBotRoster::ResetPlayerSquads()
{
    for (int L = 0; L < 65; L++)
    {
        for (int S = 0; S < 3; S++)
        {
            m_leaderMemberBotIdx[L][S] = -1;
            m_leaderMemberName[L][S][0] = '\0';
            m_leaderMemberDeathTime[L][S] = 0.0f;
        }
        m_leaderActive[L] = false;
        m_leaderSquadOrder[L] = 0;
        m_leaderWaypoint[L] = Vector(0, 0, 0);
        m_leaderSquadThreatEnemy[L] = 0;
        m_leaderSquadThreatPos[L] = Vector(0, 0, 0);
        m_leaderSquadThreatTime[L] = 0.0f;
    }
}

void CBotRoster::ReportSquadThreat(int leaderIndex, int enemyIdx, const Vector &pos, float now)
{
    if (leaderIndex <= 0 || leaderIndex > 64 || enemyIdx <= 0 || enemyIdx > 64) return;
    m_leaderSquadThreatEnemy[leaderIndex] = enemyIdx;
    m_leaderSquadThreatPos[leaderIndex] = pos;
    m_leaderSquadThreatTime[leaderIndex] = now;
}

int CBotRoster::GetSquadThreat(int leaderIndex, float now, float maxAge, Vector &outPos) const
{
    if (leaderIndex <= 0 || leaderIndex > 64) return 0;
    int enemyIdx = m_leaderSquadThreatEnemy[leaderIndex];
    if (enemyIdx <= 0 || enemyIdx > 64) return 0;
    if (now - m_leaderSquadThreatTime[leaderIndex] > maxAge) return 0;

    edict_t *le = engine->PEntityOfEntIndex(leaderIndex);
    IPlayerInfo *lpi = (le && !le->IsFree()) ? playerinfomanager->GetPlayerInfo(le) : 0;
    int leaderTeam = lpi ? lpi->GetTeamIndex() : 0;
    if (leaderTeam >= 1 && leaderTeam <= 2)
    {
        if (!IntelEnemyIsLive(leaderTeam, enemyIdx))
            return 0;
    }

    outPos = m_leaderSquadThreatPos[leaderIndex];
    return enemyIdx;
}

bool CBotRoster::IsSquadContact(int leaderIndex, int enemyIdx, float now) const
{
    if (leaderIndex <= 0 || leaderIndex > 64 || enemyIdx <= 0 || enemyIdx > 64) return false;

    // 1. Direct active threat match within 3.5s
    if (m_leaderSquadThreatEnemy[leaderIndex] == enemyIdx
        && (now - m_leaderSquadThreatTime[leaderIndex] <= 3.5f))
    {
        edict_t *le = engine->PEntityOfEntIndex(leaderIndex);
        IPlayerInfo *lpi = (le && !le->IsFree()) ? playerinfomanager->GetPlayerInfo(le) : 0;
        int leaderTeam = lpi ? lpi->GetTeamIndex() : 0;
        if (leaderTeam < 1 || leaderTeam > 2 || IntelEnemyIsLive(leaderTeam, enemyIdx))
            return true;
    }

    // 2. Check if any fellow squadmate is actively targeting this enemy
    for (int slot = 0; slot < 3; slot++)
    {
        int botIdx = m_leaderMemberBotIdx[leaderIndex][slot];
        if (botIdx >= 0 && botIdx < Count())
        {
            if (m_bots[botIdx].m_state == BOT_ALIVE && m_bots[botIdx].m_enemyIndex == enemyIdx)
                return true;
        }
    }
    return false;
}

void CBotRoster::UpdateSquadLeaderThreats(float now)
{
    for (int L = 1; L <= gpGlobals->maxClients; L++)
    {
        if (L > 64 || !m_leaderActive[L]) continue;
        edict_t *le = engine->PEntityOfEntIndex(L);
        if (!le || le->IsFree() || !le->GetUnknown()) continue;
        IPlayerInfo *lpi = playerinfomanager ? playerinfomanager->GetPlayerInfo(le) : 0;
        if (!lpi || !lpi->IsConnected() || lpi->IsDead()) continue;

        CBotCmd cmd = lpi->GetLastUserCommand();
        Vector eyePos = lpi->GetAbsOrigin();
        eyePos.z += (lpi->GetPlayerMaxs().z < 40.0f) ? 36.0f : 64.0f;

        float yawRad = cmd.viewangles.y * (3.14159265f / 180.0f);
        float pitchRad = -cmd.viewangles.x * (3.14159265f / 180.0f);
        Vector fwd(cosf(pitchRad) * cosf(yawRad), cosf(pitchRad) * sinf(yawRad), sinf(pitchRad));

        int myTeam = lpi->GetTeamIndex();
        int bestEnemy = 0;
        float bestDot = 0.965f; // ~15 deg cone
        Vector bestPos(0, 0, 0);

        for (int e = 1; e <= gpGlobals->maxClients; e++)
        {
            if (e == L) continue;
            edict_t *ee = engine->PEntityOfEntIndex(e);
            if (!ee || ee->IsFree() || !ee->GetUnknown()) continue;
            IPlayerInfo *epi = playerinfomanager ? playerinfomanager->GetPlayerInfo(ee) : 0;
            if (!epi || !epi->IsConnected() || !epi->IsPlayer() || epi->IsDead() || epi->IsObserver() || epi->IsHLTV()) continue;
            if (epi->GetTeamIndex() == myTeam || epi->GetTeamIndex() <= 0) continue;

            Vector ePos = epi->GetAbsOrigin() + Vector(0, 0, 45.0f);
            Vector toE = ePos - eyePos;
            float dist = sqrtf(toE.x * toE.x + toE.y * toE.y + toE.z * toE.z);
            if (dist > 1800.0f || dist < 10.0f) continue;
            toE.x /= dist; toE.y /= dist; toE.z /= dist;
            float dot = fwd.x * toE.x + fwd.y * toE.y + fwd.z * toE.z;
            if (dot > bestDot)
            {
                bestDot = dot;
                bestEnemy = e;
                bestPos = epi->GetAbsOrigin();
            }
        }

        if (bestEnemy > 0)
        {
            ReportSquadThreat(L, bestEnemy, bestPos, now);
        }
    }
}

void CBotRoster::StashPendingRespawn(CInsBot *bot)
{
    if (!bot) return;
    int leaderIdx = bot->m_squadLeaderIndex;
    if (leaderIdx <= 0 || leaderIdx > 64) return;
    int slot = bot->m_squadSlot;
    if (slot < 0 || slot > 2) slot = 0;

    m_leaderMemberBotIdx[leaderIdx][slot] = (int)(bot - m_bots);
    _snprintf(m_leaderMemberName[leaderIdx][slot], sizeof(m_leaderMemberName[leaderIdx][slot]),
              "%s", bot->m_name);
    m_leaderMemberName[leaderIdx][slot][63] = '\0';
    m_leaderMemberDeathTime[leaderIdx][slot] = gpGlobals ? gpGlobals->curtime : 0.0f;

    bot->m_squadRespawnPendingForLeader = leaderIdx;
    bot->m_squadRespawnSlot = slot;
    bot->ClearSquadAssignment();
}

void CBotRoster::RecallFollower(CInsBot *bot)
{
    if (!bot || !bot->m_inUse) return;

    for (int L = 1; L < 65; L++)
    {
        if (!m_leaderActive[L]) continue;
        for (int S = 0; S < 3; S++)
        {
            bool match = false;
            if (bot->m_squadRespawnPendingForLeader == L && bot->m_squadRespawnSlot == S)
                match = true;   // primary: explicit respawn-pending flag
            else if (bot->m_name[0] && m_leaderMemberName[L][S][0]
                     && !strcmp(bot->m_name, m_leaderMemberName[L][S])
                     && m_leaderMemberBotIdx[L][S] == (int)(bot - m_bots))
                match = true;   // secondary: index+name pair confirms identity

            if (!match) continue;

            int activeOrder = m_leaderSquadOrder[L];
            if (activeOrder <= 0) activeOrder = SQUAD_ORDER_FOLLOW;
            // Fix C-note: never resurrect a stale SUPPRESS on respawn — respawned
            // followers always rejoin in FOLLOW, even if the roster still says SUPPRESS.
            if (activeOrder == SQUAD_ORDER_SUPPRESS) activeOrder = SQUAD_ORDER_FOLLOW;

            bot->SetSquadAssignment(L, S, activeOrder);
            bot->m_squadRespawnPendingForLeader = 0;
            bot->m_squadRespawnSlot = 0;
            m_leaderMemberDeathTime[L][S] = 0.0f;

            bot->Say((activeOrder == SQUAD_ORDER_MOVETO) ? VOX_COVER : VOX_MOVEUP);
            if (engine)
            {
                const char *slotNames[3] = { "Left Flank", "Right Flank", "Rear Guard" };
                char cmdBuf[160];
                int botClientIdx = bot->m_edict ? engine->IndexOfEdict(bot->m_edict) : ((int)(bot - m_bots) + 1);
                _snprintf(cmdBuf, sizeof(cmdBuf), "insbot_squad_recruit %d %d \"%s\" \"%s\"\n",
                          L, botClientIdx, bot->m_name, slotNames[S]);
                engine->ServerCommand(cmdBuf);
            }
            return;
        }
    }
    // Failed recall: leader gone or roster cleared; fall through autonomous.
    bot->m_squadRespawnPendingForLeader = 0;
    bot->m_squadRespawnSlot = 0;
}
CInsBot *CBotRoster::AddBot(const char *nameOrNull)
{
    // choose side: alternate by current live counts
    int a = 0, b = 0;
    for (int i = 0; i < 64; i++)
        if (m_bots[i].m_inUse) (m_bots[i].m_teamSlot == 0 ? a : b)++;
    int slot = (a <= b) ? 0 : 1;
    slot = GetTeamSlotForBotName(nameOrNull, slot);

    for (int i = 0; i < 64; i++)
    {
        if (!m_bots[i].m_inUse)
        {
            int setupTeamArg = TeamSlotToTeamIndex(slot) - 1;
            int priorityRoleId = nameOrNull ? -1 : PickPriorityClassRole(slot);
            if (!m_bots[i].Create(nameOrNull, slot, setupTeamArg, priorityRoleId))
            {
                m_bots[i].m_inUse = false;
                return 0;
            }
            m_bots[i].m_ordinal = i;
            return &m_bots[i];
        }
    }
    return 0;
}

CInsBot *CBotRoster::AddBotToRole(int faction, int roleId)
{
    int targetFaction = (faction == 1) ? FACTION_IRAQI : FACTION_USMC;
    int slot = GetTeamSlotForFaction(targetFaction);
    // pfullsetup's first argument is the UI team index (engine team - 1),
    // not a stable USMC/Iraqi faction number. Factions can swap engine teams
    // between maps, so derive it only after resolving the requested faction.
    int setupTeamArg = TeamSlotToTeamIndex(slot) - 1;

    for (int i = 0; i < 64; i++)
    {
        if (!m_bots[i].m_inUse)
        {
            if (!m_bots[i].Create(0, slot, setupTeamArg, roleId))
            {
                m_bots[i].m_inUse = false;
                return 0;
            }
            m_bots[i].m_ordinal = i;
            return &m_bots[i];
        }
    }
    return 0;
}

void CBotRoster::KickAll()
{
    for (int i = 0; i < 64; i++)
    {
        if (m_bots[i].m_inUse && m_bots[i].m_edict)
        {
            int userid = engine->GetPlayerUserId(m_bots[i].m_edict);
            if (userid >= 0)
            {
                char cmd[64];
                _snprintf(cmd, sizeof(cmd), "kickid %d\n", userid);
                engine->ServerCommand(cmd);
            }
            m_bots[i].OnRemoved();
        }
    }
}

bool CBotRoster::KickOne()
{
    for (int i = 63; i >= 0; i--)
    {
        if (m_bots[i].m_inUse && m_bots[i].m_edict)
        {
            int userid = engine->GetPlayerUserId(m_bots[i].m_edict);
            if (userid >= 0)
            {
                char cmd[64];
                _snprintf(cmd, sizeof(cmd), "kickid %d\n", userid);
                engine->ServerCommand(cmd);
            }
            m_bots[i].OnRemoved();
            return true;
        }
    }
    return false;
}

int CBotRoster::Count() const
{
    int n = 0;
    for (int i = 0; i < 64; i++) if (m_bots[i].m_inUse) n++;
    return n;
}

int CBotRoster::CountTeam(int team) const
{
    int n = 0;
    for (int i = 0; i < 64; i++)
    {
        const CInsBot &bot = m_bots[i];
        if (bot.m_inUse && bot.m_info && bot.m_info->GetTeamIndex() == team)
            n++;
    }
    return n;
}

int CBotRoster::CountRequestedRoles(int teamSlot, const int *roleIds, int roleCount) const
{
    if (!roleIds || roleCount <= 0) return 0;
    int n = 0;
    for (int i = 0; i < 64; i++)
    {
        const CInsBot &bot = m_bots[i];
        if (!bot.m_inUse || bot.m_teamSlot != teamSlot || bot.m_requestedRoleId <= 0)
            continue;
        for (int k = 0; k < roleCount; k++)
            if (bot.m_requestedRoleId == roleIds[k]) { n++; break; }
    }
    return n;
}

void CBotRoster::ResetSniperCaches()
{
    for (int team = 0; team < 3; team++)
        for (int obj = 0; obj < INSBOT_SNIPER_OBJECTIVES; obj++)
        {
            SniperObjectiveCache &cache = m_sniperCache[team][obj];
            cache.requested = false;
            cache.ready = false;
            cache.pendingCount = cache.pendingCursor = 0;
            cache.bestCount = cache.accepted = cache.rejected = 0;
        }
    m_sniperCacheTeamCursor = 1;
    m_sniperCacheObjCursor = 0;
}

void CBotRoster::RequestSniperCache(int team, int obj)
{
    if (team < 1 || team > 2 || obj < 0 || obj >= INSBOT_SNIPER_OBJECTIVES)
        return;
    SniperObjectiveCache &cache = m_sniperCache[team][obj];
    if (cache.requested) return;
    cache.requested = true;
    BuildSniperCacheCandidates(team, obj, cache);
}

static float SniperRangeScore(float distance)
{
    if (distance < 500.0f || distance > 3300.0f) return -1.0f;
    if (distance < 1200.0f) return (distance - 500.0f) / 700.0f;
    if (distance <= 2200.0f) return 1.0f;
    return (3300.0f - distance) / 1100.0f;
}

void CBotRoster::BuildSniperCacheCandidates(int team, int obj,
                                             SniperObjectiveCache &cache)
{
    cache.ready = false;
    cache.pendingCount = cache.pendingCursor = 0;
    cache.bestCount = cache.accepted = cache.rejected = 0;
    const ObjPoint *op = g_Objectives.Get(obj);
    if (!op || !g_NavMesh.IsLoaded()) { cache.requested = false; cache.ready = false; return; }

    int objectiveArea = g_NavMesh.FindNearestArea(op->center);
    int objectiveComponent = g_NavMesh.ComponentForArea(objectiveArea);
    for (int i = 0; i < g_NavMesh.GetTotalSniperSpotCount(); i++)
    {
        const HidingSpot *spot = g_NavMesh.GetSniperSpot(i);
        if (!spot || spot->areaIdx < 0) continue;
        if (objectiveComponent >= 0
            && g_NavMesh.ComponentForArea(spot->areaIdx) != objectiveComponent)
            continue;
        float dx = spot->pos.x - op->center.x;
        float dy = spot->pos.y - op->center.y;
        float distance = sqrtf(dx * dx + dy * dy);
        float range = SniperRangeScore(distance);
        if (range < 0.0f) continue;

        const NavAreaData *area = g_NavMesh.Area(spot->areaIdx);
        float elevation = spot->pos.z - op->minZ;
        float score = range * 60.0f;
        if (elevation > 0.0f)
        {
            if (elevation > 400.0f) elevation = 400.0f;
            score += elevation * 0.035f;
        }
        if (spot->flags & SNIPER_SPOT_IDEAL) score += 30.0f;
        else if (spot->flags & SNIPER_SPOT_GOOD) score += 20.0f;
        else if (spot->flags & SNIPER_SPOT_IN_COVER) score += 8.0f;
        if (spot->flags & SNIPER_SPOT_CORNER) score += 3.0f;
        if (area && area->connCount <= 3) score += 4.0f;

        bool duplicate = false;
        for (int p = 0; p < cache.pendingCount; p++)
        {
            float qx = cache.pending[p].pos.x - spot->pos.x;
            float qy = cache.pending[p].pos.y - spot->pos.y;
            if (qx * qx + qy * qy < 32.0f * 32.0f)
            {
                duplicate = true;
                if (score > cache.pending[p].cheapScore)
                {
                    cache.pending[p].pos = spot->pos;
                    cache.pending[p].areaIdx = spot->areaIdx;
                    cache.pending[p].cheapScore = score;
                    cache.pending[p].flags = spot->flags;
                }
                break;
            }
        }
        if (duplicate) continue;

        int dst = cache.pendingCount;
        if (dst >= INSBOT_SNIPER_PENDING)
        {
            dst = 0;
            for (int p = 1; p < cache.pendingCount; p++)
                if (cache.pending[p].cheapScore < cache.pending[dst].cheapScore) dst = p;
            if (score <= cache.pending[dst].cheapScore) continue;
        }
        else cache.pendingCount++;
        cache.pending[dst].pos = spot->pos;
        cache.pending[dst].areaIdx = spot->areaIdx;
        cache.pending[dst].cheapScore = score;
        cache.pending[dst].flags = spot->flags;
    }
    if (cache.pendingCount == 0) cache.ready = true;
}

static bool SniperHullClear(const Vector &pos, float height)
{
    CTraceFilterWorldAndPropsOnly filter;
    Ray_t ray;
    trace_t tr;
    ray.Init(pos + Vector(0, 0, 1), pos + Vector(0, 0, 1),
             Vector(-16, -16, 0), Vector(16, 16, height));
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &tr);
    return !tr.startsolid && !tr.allsolid && tr.fraction >= 1.0f;
}

static bool SniperWorldLOS(const Vector &from, const Vector &to)
{
    CTraceFilterWorldAndPropsOnly filter;
    Ray_t ray;
    trace_t tr;
    ray.Init(from, to);
    enginetrace->TraceRay(ray, MASK_SHOT & ~(CONTENTS_HITBOX | CONTENTS_WINDOW),
                          &filter, &tr);
    return !tr.startsolid && tr.fraction > 0.985f;
}

bool CBotRoster::EvaluateSniperCandidate(int team, int obj,
                                         const SniperPendingCandidate &candidate,
                                         SniperPerch &out)
{
    const ObjPoint *op = g_Objectives.Get(obj);
    if (!op || !enginetrace) return false;

    Vector targets[8];
    float targetZ = op->minZ + 45.0f;
    targets[0] = Vector(op->center.x, op->center.y, targetZ);
    targets[1] = Vector(op->minX + 24.0f, op->center.y, targetZ);
    targets[2] = Vector(op->maxX - 24.0f, op->center.y, targetZ);
    targets[3] = Vector(op->center.x, op->minY + 24.0f, targetZ);
    targets[4] = Vector(op->center.x, op->maxY - 24.0f, targetZ);

    Vector approach(1, 0, 0);
    int enemyTeam = 3 - team;
    if (enemyTeam >= 1 && enemyTeam <= 2 && g_Objectives.m_homeSamples[enemyTeam] > 0)
    {
        approach = g_Objectives.m_home[enemyTeam] - op->center;
        approach.z = 0.0f;
        float length = sqrtf(approach.x * approach.x + approach.y * approach.y);
        if (length > 1.0f) { approach.x /= length; approach.y /= length; }
    }
    Vector lateral(-approach.y, approach.x, 0);
    for (int a = 0; a < 3; a++)
    {
        float side = (a == 1) ? 180.0f : (a == 2 ? -180.0f : 0.0f);
        Vector probe = op->center + approach * 350.0f + lateral * side;
        int areaIdx = g_NavMesh.FindNearestArea(probe);
        const NavAreaData *area = g_NavMesh.Area(areaIdx);
        targets[5 + a] = area ? area->center + Vector(0, 0, 45.0f)
                              : Vector(probe.x, probe.y, targetZ);
    }

    float crouchEye = insbot_crouch_eye_height.GetFloat();
    if (crouchEye < 20.0f) crouchEye = 20.0f;
    if (crouchEye > 60.0f) crouchEye = 60.0f;
    const float heights[2] = { 58.0f, crouchEye };
    int bestVisible = 0;
    int bestUseful = 0;
    int bestStance = SNIPER_STANCE_NONE;
    Vector bestTarget = targets[0];
    for (int stance = 0; stance < 2; stance++)
    {
        float hullHeight = stance == 0 ? 72.0f : 36.0f;
        if (!SniperHullClear(candidate.pos, hullHeight)) continue;
        int visible = 0, useful = 0;
        Vector preferred = targets[0];
        bool preferredUseful = false;
        for (int t = 0; t < 8; t++)
        {
            if (!SniperWorldLOS(candidate.pos + Vector(0, 0, heights[stance]), targets[t])) continue;
            visible++;
            if (t > 0) useful++;
            if (t >= 5 || (!preferredUseful && t > 0))
            {
                preferred = targets[t];
                preferredUseful = true;
            }
        }
        if (visible >= 2 && useful >= 1 && (visible > bestVisible
            || (visible == bestVisible && stance == 0)))
        {
            bestVisible = visible;
            bestUseful = useful;
            bestStance = stance == 0 ? SNIPER_STANCE_STAND : SNIPER_STANCE_CROUCH;
            bestTarget = preferred;
        }
    }
    if (bestStance == SNIPER_STANCE_NONE) return false;

    int coverHits = 0;
    Vector coverStart = candidate.pos + Vector(0, 0, 44.0f);
    for (int i = 0; i < 8; i++)
    {
        float angle = (float)i * 0.78539816f;
        Vector end = coverStart + Vector(cosf(angle) * 160.0f, sinf(angle) * 160.0f, 0);
        if (!SniperWorldLOS(coverStart, end)) coverHits++;
    }
    if (coverHits >= 7) return false;

    out.pos = candidate.pos;
    out.aimTarget = bestTarget;
    out.areaIdx = candidate.areaIdx;
    out.flags = candidate.flags;
    out.stance = (unsigned char)bestStance;
    out.visibleRays = (unsigned char)bestVisible;
    out.score = candidate.cheapScore + bestVisible * 9.0f + bestUseful * 2.0f;
    if (coverHits >= 2 && coverHits <= 5) out.score += coverHits * 3.0f;
    else if (coverHits == 0) out.score -= 12.0f;
    return true;
}

void CBotRoster::UpdateSniperCaches(float /*now*/)
{
    if (!g_NavMesh.IsLoaded() || !g_Objectives.IsReady()) return;
    int budget = 3;
    for (int searched = 0; searched < 2 * INSBOT_SNIPER_OBJECTIVES && budget > 0; searched++)
    {
        int team = m_sniperCacheTeamCursor;
        int obj = m_sniperCacheObjCursor;
        if (++m_sniperCacheObjCursor >= INSBOT_SNIPER_OBJECTIVES)
        {
            m_sniperCacheObjCursor = 0;
            m_sniperCacheTeamCursor = (m_sniperCacheTeamCursor == 1) ? 2 : 1;
        }
        SniperObjectiveCache &cache = m_sniperCache[team][obj];
        if (!cache.requested || cache.ready) continue;
        while (cache.pendingCursor < cache.pendingCount && budget-- > 0)
        {
            SniperPerch perch;
            if (EvaluateSniperCandidate(team, obj, cache.pending[cache.pendingCursor], perch))
            {
                cache.accepted++;
                int insert = 0;
                while (insert < cache.bestCount && cache.best[insert].score >= perch.score) insert++;
                if (insert < INSBOT_SNIPER_BEST)
                {
                    int end = cache.bestCount < INSBOT_SNIPER_BEST ? cache.bestCount : INSBOT_SNIPER_BEST - 1;
                    for (int k = end; k > insert; k--) cache.best[k] = cache.best[k - 1];
                    cache.best[insert] = perch;
                    if (cache.bestCount < INSBOT_SNIPER_BEST) cache.bestCount++;
                }
            }
            else cache.rejected++;
            cache.pendingCursor++;
        }
        if (cache.pendingCursor >= cache.pendingCount)
        {
            cache.ready = true;
            BotDbg("[insbots] sniper cache team=%d obj=%d candidates=%d accepted=%d best=%d\n",
                   team, obj, cache.pendingCount, cache.accepted, cache.bestCount);
        }
    }
}

bool CBotRoster::IsSniperPerchReserved(const CInsBot *requester, int team,
                                        const Vector &pos) const
{
    for (int i = 0; i < 64; i++)
    {
        const CInsBot &other = m_bots[i];
        if (&other == requester || !other.m_inUse || !other.m_info
            || other.m_info->IsDead() || other.m_info->GetTeamIndex() != team
            || !other.m_haveSniperSpot)
            continue;
        float dx = other.m_sniperSpot.x - pos.x;
        float dy = other.m_sniperSpot.y - pos.y;
        if (dx * dx + dy * dy < 288.0f * 288.0f) return true;
    }
    return false;
}

bool CBotRoster::AssignSniperPerch(CInsBot *bot, int team, int obj, float now)
{
    if (!bot || team < 1 || team > 2 || obj < 0 || obj >= INSBOT_SNIPER_OBJECTIVES)
        return false;
    RequestSniperCache(team, obj);
    SniperObjectiveCache &cache = m_sniperCache[team][obj];
    if (!cache.ready || cache.bestCount <= 0) return false;
    Vector botPos = bot->m_info->GetAbsOrigin();
    int botArea = bot->CurrentArea(botPos);
    if (botArea < 0) botArea = g_NavMesh.FindNearestArea(botPos);
    int best = -1;
    float bestScore = -3.4e38f;
    for (int i = 0; i < cache.bestCount; i++)
    {
        const SniperPerch &perch = cache.best[i];
        if (IsSniperPerchReserved(bot, team, perch.pos)) continue;
        float dx = perch.pos.x - botPos.x, dy = perch.pos.y - botPos.y, dz = perch.pos.z - botPos.z;
        if (dx * dx + dy * dy + dz * dz > 6000.0f * 6000.0f) continue;
        float pathCost = g_NavMesh.GetPathCost(botArea, perch.areaIdx, 0);
        if (pathCost < 0.0f || pathCost > 6000.0f) continue;
        unsigned int hash = (unsigned int)(bot->m_ordinal + 1) * 2654435761u
                          ^ (unsigned int)(i + 17) * 2246822519u;
        float jitter = (float)(hash % 1000) * 0.004f;
        float score = perch.score + jitter - pathCost * 0.0015f;
        if (score > bestScore) { bestScore = score; best = i; }
    }
    if (best < 0) return false;
    const SniperPerch &perch = cache.best[best];
    bot->m_sniperSpot = perch.pos;
    bot->m_sniperAimTarget = perch.aimTarget;
    bot->m_sniperArea = perch.areaIdx;
    bot->m_sniperObj = obj;
    bot->m_sniperTeam = team;
    bot->m_sniperStance = perch.stance;
    bot->m_sniperScore = perch.score;
    bot->m_sniperAssignedTime = now;
    bot->m_sniperKills = 0;
    bot->m_haveSniperSpot = true;
    // A newly-created assignment always starts with a clean route. A hearing
    // search that began before assignment must not qualify as the one
    // permitted stationed-sniper detour.
    bot->m_hearUntil = 0.0f;
    bot->m_sniperHearingDetour = false;
    bot->m_sniperHearingReturnTime = 0.0f;
    float minTime = insbot_sniper_perch_time_min.GetFloat();
    float maxTime = insbot_sniper_perch_time_max.GetFloat();
    if (minTime < 5.0f) minTime = 5.0f;
    if (maxTime < minTime) maxTime = minTime;
    bot->m_sniperHoldUntil = now + frand(minTime, maxTime);
    BotDbg("[insbots] %s assigned sniper perch obj=%d score=%.1f stance=%s\n",
           bot->m_name, obj, perch.score,
           perch.stance == SNIPER_STANCE_CROUCH ? "crouch" : "stand");
    return true;
}

void CBotRoster::UpdateDangerZones(float now)
{
    INSPROF_SCOPE(INSPROF_DANGERZONES);
    NavClearDangerZones();
    float radius = insbot_danger_radius.GetFloat();
    extern CfgVar insbot_danger_avoid;
    if (radius <= 0.0f || !insbot_objectives.GetBool() || insbot_danger_avoid.GetFloat() <= 0.0f) return;

    // Every bot actively fighting (enemy seen within the last 2s) marks the
    // fight area: its own position plus the enemy's last known position, so
    // the whole exchange is covered, not just one side. Zones are rebuilt
    // every frame, so they disappear as soon as the fight ends (the InCombat
    // window gives a short tail while a bot is still tracking a just-lost
    // enemy). Capture-committed bots route around these via g_navDangerPenalty.
    for (int i = 0; i < 64; i++)
    {
        const CInsBot &bot = m_bots[i];
        if (!bot.m_inUse || !bot.m_info || bot.m_info->IsDead()) continue;
        if (!bot.InCombat(now)) continue;
        NavAddDangerZone(bot.m_info->GetAbsOrigin(), radius);
        const Vector &ep = bot.GetEnemyLastPos();
        if (ep.x != 0.0f || ep.y != 0.0f || ep.z != 0.0f)
            NavAddDangerZone(ep, radius);
    }
}

void CBotRoster::UpdateCaptureCommitments(float now)
{
    INSPROF_SCOPE(INSPROF_CAPCOMMIT);
    for (int team = 1; team <= 2; team++)
    {
        // ---- validity pass (team-local) ----
        // A bot that died, disconnected, switched teams, or gave up on its
        // assigned objective (stuck-learning avoidance) loses its commitment
        // immediately so the slot can be refilled. Only bots of THIS team are
        // touched here - the old code cleared the other team's commitments
        // every frame, which made the capture detail flicker on and off.
        for (int i = 0; i < 64; i++)
        {
            CInsBot &bot = m_bots[i];
            if (!bot.m_captureCommit) continue;
            // A bot that is gone/unknown is invalid for any team; clear it
            // here. A bot on the OTHER team must be left alone - its own
            // team's pass owns it. (The old code cleared anyone whose team
            // didn't match, which wiped both teams' commitments every frame
            // and made the capture detail flicker.)
            if (!bot.m_inUse || !bot.m_info || bot.m_info->IsDead())
            {
                bot.m_captureCommit = false;
                bot.m_captureObj = -1;
                bot.m_captureTeam = 0;
                continue;
            }
            if (bot.m_captureTeam != team) continue;
            if (bot.m_info->GetTeamIndex() != team)
            {
                bot.m_captureCommit = false;
                bot.m_captureObj = -1;
                bot.m_captureTeam = 0;
                continue;
            }
            // Released its assigned point as unreachable: let a fresh bot try.
            if (bot.m_captureObj >= 0 && bot.IsAvoiding(bot.m_captureObj, now))
            {
                bot.m_captureCommit = false;
                bot.m_captureObj = -1;
                bot.m_captureTeam = 0;
            }
        }

        // Ownership can change between the one-second target refreshes. A
        // cached target that just became friendly is invalid immediately, so
        // dirty it before the availability check. This same FrameUpdate will
        // choose the next neutral/hostile point or clear the capture detail.
        int cachedTarget = m_captureTarget[team];
        if (cachedTarget >= 0 && g_Objectives.Relation(cachedTarget, team) > 0)
        {
            m_captureTarget[team] = -1;
            m_captureUpdateTime[team] = -999.0f;
        }

        if (!insbot_objectives.GetBool() || !g_Objectives.IsReady()
            || !g_Objectives.HasHostileOrNeutral(team))
        {
            for (int i = 0; i < 64; i++)
            {
                if (!m_bots[i].m_inUse || !m_bots[i].m_info
                    || m_bots[i].m_info->GetTeamIndex() != team)
                    continue;
                m_bots[i].m_captureCommit = false;
                m_bots[i].m_captureObj = -1;
                m_bots[i].m_captureTeam = 0;
            }
            continue;
        }

        // ---- the team's single active capture target (cached 1s) ----
        // Same choice as a force-capture PickGoal: the nearest hostile/neutral
        // point to the team's home. Every committed bot is dedicated to this
        // one point, so the capture detail actually concentrates on it.
        // PickGoal runs A* for its cost comparison, so it is throttled to
        // once per second per team instead of every frame.
        if (now >= m_captureUpdateTime[team] + 1.0f)
        {
            m_captureUpdateTime[team] = now;
            m_captureTarget[team] = g_Objectives.PickGoal(team, Vector(0, 0, 0), -1,
                                                          1.0f, -1, -1, true);
            // Preserve the core commitment invariant locally even if target
            // selection changes later: committed bots may only be assigned a
            // neutral or hostile objective for their team.
            if (m_captureTarget[team] >= 0
                && g_Objectives.Relation(m_captureTarget[team], team) > 0)
                m_captureTarget[team] = -1;
            if (m_captureTarget[team] < 0 && g_Objectives.m_homeSamples[team] == 0)
            {
                // No home samples yet (round just started): PickGoal's early
                // return would disable the detail. Fall back to the nearest
                // hostile/neutral objective to the team's alive bots so
                // capture commitments still work from the very first seconds.
                Vector ref(0, 0, 0);
                int nRef = 0;
                for (int i = 0; i < 64; i++)
                {
                    const CInsBot &bot = m_bots[i];
                    if (!bot.m_inUse || !bot.m_info || bot.m_info->IsDead()
                        || bot.m_info->GetTeamIndex() != team)
                        continue;
                    Vector p = bot.m_info->GetAbsOrigin();
                    ref.x += p.x; ref.y += p.y; ref.z += p.z;
                    nRef++;
                }
                if (nRef > 0)
                {
                    ref.x /= (float)nRef; ref.y /= (float)nRef; ref.z /= (float)nRef;
                    int best = -1; float bestD = 3.4e38f;
                    for (int k = 0; k < g_Objectives.Count(); k++)
                    {
                        const ObjPoint *op = g_Objectives.Get(k);
                        if (!op || g_Objectives.IsSpawnZone(k, team)) continue;
                        if (g_Objectives.Relation(k, team) > 0) continue;
                        Vector d = op->center - ref;
                        float ds = d.x * d.x + d.y * d.y + d.z * d.z * 4.0f;
                        if (ds < bestD) { bestD = ds; best = k; }
                    }
                    m_captureTarget[team] = best;
                }
            }
        }
        int capTarget = m_captureTarget[team];
        if (capTarget < 0)
        {
            for (int i = 0; i < 64; i++)
            {
                if (!m_bots[i].m_inUse || !m_bots[i].m_info
                    || m_bots[i].m_info->GetTeamIndex() != team)
                    continue;
                m_bots[i].m_captureCommit = false;
                m_bots[i].m_captureObj = -1;
                m_bots[i].m_captureTeam = 0;
            }
            continue;
        }

        const ObjPoint *capOp = g_Objectives.Get(capTarget);
        if (!capOp) continue;
        Vector capPos = capOp->center;

        // ---- keep the same bots; re-target them when the front line moves ----
        int teamBots = 0, committed = 0;
        for (int i = 0; i < 64; i++)
        {
            CInsBot &bot = m_bots[i];
            if (!bot.m_inUse || !bot.m_info || bot.m_info->IsDead()
                || bot.m_info->GetTeamIndex() != team)
                continue;
            teamBots++;
            if (bot.m_captureCommit)
            {
                bot.m_captureObj = capTarget;   // re-target to the active point
                bot.m_captureTeam = team;
                committed++;
            }
        }
        if (teamBots < 1) continue;

        // reqpercent is based on team population, not just this plugin's bot
        // roster. Count every living player so mixed human/bot teams do not
        // reserve too few bots to start a capture. The commitment percentage
        // is configurable for maps that use 5% instead of
        // the common 15%; cap at the number of available bots.
        int teamPlayers = 0;
        if (gpGlobals)
        {
            for (int i = 1; i <= gpGlobals->maxClients; i++)
            {
                edict_t *e = engine->PEntityOfEntIndex(i);
                if (!e || e->IsFree()) continue;
                IPlayerInfo *pi = playerinfomanager->GetPlayerInfo(e);
                if (!pi || !pi->IsConnected() || pi->IsDead() || pi->IsObserver() || pi->IsHLTV()
                    || pi->GetTeamIndex() != team) continue;
                teamPlayers++;
            }
        }
        if (teamPlayers < teamBots) teamPlayers = teamBots;
        float capturePercent = insbot_capture_percent.GetFloat();
        if (capturePercent < 0.0f) capturePercent = 0.0f;
        if (capturePercent > 100.0f) capturePercent = 100.0f;
        int required = (int)ceilf((float)teamPlayers * capturePercent / 100.0f);
        if (required < 1) required = 1;
        if (required > teamBots) required = teamBots;

        // Team shrank (mass casualty): the detail is overstaffed for the new
        // 15% requirement. Release the excess once, keeping the committed
        // bots closest to the objective (they are the ones actually making
        // progress). This only happens after a size change - it does not
        // reshuffle a healthy, fully-staffed detail.
        if (committed > required)
        {
            int excess = committed - required;
            for (int rel = 0; rel < excess; rel++)
            {
                int worstIdx = -1; float worstD = -1.0f;
                for (int i = 0; i < 64; i++)
                {
                    CInsBot &bot = m_bots[i];
                    if (!bot.m_captureCommit || !bot.m_info || bot.m_info->IsDead()
                        || bot.m_info->GetTeamIndex() != team)
                        continue;
                    Vector d = bot.m_info->GetAbsOrigin() - capPos;
                    float dist = sqrtf(d.x * d.x + d.y * d.y);
                    if (dist > worstD) { worstD = dist; worstIdx = i; }
                }
                if (worstIdx < 0) break;
                m_bots[worstIdx].m_captureCommit = false;
                m_bots[worstIdx].m_captureObj = -1;
                m_bots[worstIdx].m_captureTeam = 0;
            }
            committed = required;   // the stale count no longer matches reality
        }

        // Fully staffed: stable detail. No periodic re-picking (the old code
        // reshuffled the whole set every 0.75s, so bots flickered in and out
        // of the capture detail).
        if (committed >= required) continue;

        // Understaffed: refill quickly (0.25s), but not every frame (A* cost).
        if (now < m_captureRefillTime[team] + 0.25f) continue;
        m_captureRefillTime[team] = now;

        int goalArea = g_NavMesh.IsLoaded() ? g_NavMesh.FindArea(capPos) : -1;
        if (goalArea < 0)
            goalArea = g_NavMesh.IsLoaded() ? g_NavMesh.FindNearestArea(capPos) : -1;

        int toAdd = required - committed;
        // Each refill slot re-scans all 64 bots, so the same (botArea,
        // goalArea) A* query repeats dozens of times per tick. Memoize the
        // results for this refill only - nav costs cannot change mid-tick
        // (same globals/danger state), and link learning invalidates any
        // longer-lived cache.
        struct CostMemo { int from; int to; float cost; };
        CostMemo memo[64]; int memoN = 0;
        for (int slot = 0; slot < toAdd; slot++)
        {
            int bestIdx = -1;
            float bestKey = 3.4e38f;
            int bestOrdinal = 0;
            for (int i = 0; i < 64; i++)
            {
                CInsBot &bot = m_bots[i];
                if (!bot.m_inUse || !bot.m_info || bot.m_info->IsDead()
                    || bot.m_info->GetTeamIndex() != team || bot.m_captureCommit
                    || bot.IsSquadFollower())
                    continue;
                // A bot currently avoiding the target (couldn't reach it) is
                // not a useful pick - it stays out until the avoidance
                // expires, so it can't be instantly recommitted to the same
                // point it just gave up on.
                if (bot.IsAvoiding(capTarget, now))
                    continue;

                Vector p = bot.m_info->GetAbsOrigin();
                Vector d = p - capPos;
                float dist = sqrtf(d.x * d.x + d.y * d.y);
                bool reachable = true;
                if (goalArea >= 0)
                {
                    int botArea = bot.CurrentArea(p);
                    if (botArea < 0)
                        botArea = g_NavMesh.FindNearestArea(p);
                    if (botArea >= 0)
                    {
                        float navCost = -1.0f;
                        int m = 0;
                        for (; m < memoN; m++)
                            if (memo[m].from == botArea && memo[m].to == goalArea) break;
                        if (m < memoN)
                            navCost = memo[m].cost;
                        else
                        {
                            navCost = g_NavMesh.GetPathCost(botArea, goalArea, 0);
                            if (memoN < 64)
                            {
                                memo[memoN].from = botArea;
                                memo[memoN].to = goalArea;
                                memo[memoN].cost = navCost;
                                memoN++;
                            }
                        }
                        if (navCost >= 0.0f)
                            dist = navCost;
                        else
                            reachable = false;
                    }
                }
                if (!reachable) continue;
                // A bot actively fighting is a worse pick for a long march.
                float key = dist + (bot.InCombat(now) ? 250.0f : 0.0f);
                // Preserve overwatch specialists when any non-sniper can fill
                // the capture body. The finite penalty still lets a sniper be
                // selected when the team has no other reachable bot.
                if (bot.IsSniper()) key += 100000.0f;

                if (bestIdx < 0 || key < bestKey - 0.5f
                    || (fabsf(key - bestKey) <= 0.5f && bot.m_ordinal < bestOrdinal))
                {
                    bestIdx = i;
                    bestKey = key;
                    bestOrdinal = bot.m_ordinal;
                }
            }
            if (bestIdx < 0) break;
            m_bots[bestIdx].m_captureCommit = true;
            m_bots[bestIdx].m_captureObj = capTarget;
            m_bots[bestIdx].m_captureTeam = team;
        }
    }
}

bool CBotRoster::IsCaptureCommitment(const CInsBot *bot, int team) const
{
    if (!bot || team < 1 || team > 2) return false;
    if (!bot->m_captureCommit || bot->m_captureObj < 0 || bot->m_captureTeam != team) return false;
    if (!bot->m_info || bot->m_info->GetTeamIndex() != team) return false;
    return true;
}

CInsBot *CBotRoster::FindByEdict(edict_t *e)
{
    for (int i = 0; i < 64; i++)
        if (m_bots[i].m_inUse && m_bots[i].m_edict == e) return &m_bots[i];
    return 0;
}

// after a map change the engine keeps fake clients connected but our roster
// was cleared; find our orphans (name prefix "[BOT]") and take control again
void CBotRoster::AdoptOrphans()
{
    for (int i = 1; i <= gpGlobals->maxClients; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (!e || e->IsFree()) continue;
        IPlayerInfo *pi = playerinfomanager->GetPlayerInfo(e);
        if (!pi || !pi->IsConnected() || !pi->IsFakeClient() || pi->IsHLTV()) continue;
        const char *nm = pi->GetName();
        if (!nm || strncmp(nm, "[BOT]", 5) != 0) continue;
        if (FindByEdict(e)) continue;
        if (!botmanager->GetBotController(e))
        {
            // Orphan lost its bot controller across the map change (common in some Source branches).
            // Kick it so the quota system can freshly recreate it with a valid controller.
            char kcmd[64];
            _snprintf(kcmd, sizeof(kcmd), "kickid %d\n", pi->GetUserID());
            engine->ServerCommand(kcmd);
            continue;
        }

        int a = 0, b = 0;
        for (int k = 0; k < 64; k++)
            if (m_bots[k].m_inUse) (m_bots[k].m_teamSlot == 0 ? a : b)++;

        for (int k = 0; k < 64; k++)
        {
            if (!m_bots[k].m_inUse)
            {
                int slot = (a <= b) ? 0 : 1;
                slot = GetTeamSlotForBotName(nm, slot);
                m_bots[k].Adopt(e, slot);
                m_bots[k].m_ordinal = k;
                break;
            }
        }
    }
}

void CBotRoster::FrameUpdate(bool simulating)
{
    bool bodyGraceTime = simulating && !g_RoundFrozen
        && !(insbot_freeze.GetBool() && !g_ForceGotoActive);
    UpdateBodyGracePlayers(bodyGraceTime);

    if (simulating)
    {
        HearingFrameUpdate();
        UpdateSniperCaches(gpGlobals ? gpGlobals->curtime : 0.0f);
        // Refresh the firefight zones before any bot thinks this frame, so a
        // committed bot repathing this frame sees the current fights.
        UpdateDangerZones(gpGlobals ? gpGlobals->curtime : 0.0f);
        // Refresh the 15% capture detail before any bot thinks this frame, so
        // every bot sees the same commitment set.
        UpdateCaptureCommitments(gpGlobals ? gpGlobals->curtime : 0.0f);
        UpdateSquadRoster(gpGlobals ? gpGlobals->curtime : 0.0f);
        UpdateSquadLeaderThreats(gpGlobals ? gpGlobals->curtime : 0.0f);
    }

    for (int i = 0; i < 64; i++)
        if (m_bots[i].m_inUse)
            m_bots[i].Think(simulating);

    // A recovery request can release or teleport during Think. Restore any
    // now-clear participants in the same frame without advancing cooldowns.
    UpdateBodyGracePlayers(false);

    // Queue ordering reads one coherent previous-frame snapshot. Refresh all
    // ranks only after every Think call so a same-frame repath cannot make two
    // predecessor decisions observe different rank generations.
    if (simulating)
    {
        for (int i = 0; i < 64; i++)
            if (m_bots[i].m_inUse)
                m_bots[i].RefreshRouteRank();
    }

    // Do not perform CRT file I/O from GameFrame on this Episode 1 build.
    // Learned links stay in memory and are persisted at LevelShutdown/unload.

    if (simulating && insbot_draw.GetBool() && debugoverlay)
    {
        for (int i = 0; i < 64; i++)
        {
            if (!m_bots[i].m_inUse) continue;
            m_bots[i].DebugDraw();

            // Fix 5/6: draw a magenta box above the leader when m_leaderInCover is true
            // so the player can visually confirm the detection is working.
            if (m_bots[i].m_leaderInCover && m_bots[i].m_squadLeaderIndex > 0
                && m_bots[i].m_squadLeaderIndex <= gpGlobals->maxClients)
            {
                edict_t *le = engine->PEntityOfEntIndex(m_bots[i].m_squadLeaderIndex);
                IPlayerInfo *lpi = (le && !le->IsFree()) ? playerinfomanager->GetPlayerInfo(le) : 0;
                if (lpi)
                {
                    Vector lp = lpi->GetAbsOrigin();
                    Vector mn = lp + Vector(-14, -14, 82);
                    Vector mx = lp + Vector(14, 14, 100);
                    DrawBoxTW(mn, mx, 255, 0, 255, 0.0f);   // magenta
                    debugoverlay->AddTextOverlay(lp + Vector(0, 0, 110), 0, 0.0f,
                        "LEADER IN COVER");
                }
            }
        }

        // objective points. Real (capturable) objectives are boxed by owner
        // (grey=neutral, cyan=team1, orange=team2). Spawn zones (hidden) are drawn
        // dim and labelled SPAWN - bots never treat those as goals.
        for (int i = 0; i < g_Objectives.Count(); i++)
        {
            const ObjPoint *o = g_Objectives.Get(i);
            if (!o) continue;
            float r = o->radius;
            Vector mn = o->center + Vector(-r, -r, 0);
            Vector mx = o->center + Vector(r, r, 0);
            mn.z = o->minZ; mx.z = o->maxZ;
            int owner = g_Objectives.GetOwnerTeam(i);
            bool hidden = g_Objectives.IsSpawnZone(i, 0);

            // label with the HUD phonetic letter (A/B/C) when we resolved one from
            // the map's .imc2, otherwise fall back to the raw scan index (#N)
            char objlbl[16];
            if (o->phonetic)
                _snprintf(objlbl, sizeof(objlbl), "%c", o->phonetic);
            else
                _snprintf(objlbl, sizeof(objlbl), "#%d", i);

            if (hidden)
            {
                DrawBoxTW(mn, mx, 70, 70, 70, 0.0f);          // spawn zone: dim grey
                debugoverlay->AddTextOverlay(o->center, 0, 0.0f, "OBJ %s SPAWN (owner=%d)", objlbl, owner);
            }
            else
            {
                // color relative to the reference team (default 1) so it matches
                // the HUD: green=friendly, grey=neutral, red=hostile
                int dt = insbot_draw_team.GetInt();
                int cr, cg, cb;
                const char *rel;
                if (owner == 0)       { cr = 180; cg = 180; cb = 180; rel = "NEUTRAL"; }
                else if (owner == dt) { cr = 0;   cg = 220; cb = 0;   rel = "FRIENDLY"; }
                else                  { cr = 230; cg = 20;  cb = 20;  rel = "HOSTILE"; }
                DrawBoxTW(mn, mx, cr, cg, cb, 0.0f);
                debugoverlay->AddTextOverlay(o->center, 0, 0.0f, "OBJ %s %s (owner=%d)", objlbl, rel, owner);
            }
        }
    }
}

// learned-bad-link file for the current map (persists runtime discoveries)
static char s_learnPath[512] = {0};

void InsBots_SaveLearn()
{
    if (s_learnPath[0] && g_NavMesh.Count() > 0 && insbot_learn_persist.GetBool())
        g_NavMesh.SaveBadLinks(s_learnPath);
}

void InsBots_ForgetLearn()
{
    g_NavMesh.ClearBadLinks();
    if (s_learnPath[0]) remove(s_learnPath);
}

void CBotRoster::InvalidateNavMeshState()
{
    ResetSniperCaches();
    ResetLadderQueues();
    ResetHadithaDoorGate();
    InvalidateAdvanceTargets();
    InsBots_ResetPhysicalPortalCache();
    for (int i = 0; i < 64; i++)
    {
        if (m_bots[i].m_inUse)
        {
            m_bots[i].ResetDoorwayRunway();
            m_bots[i].m_pathLen = 0;
            m_bots[i].m_pathResult = BOT_PATH_NONE;
            m_bots[i].m_pathContinues = false;
            m_bots[i].m_pathCursor = 0;
            m_bots[i].m_goalArea = -1;
            m_bots[i].m_repathTime = 0.0f;
            m_bots[i].m_hasMoveTarget = false;
            m_bots[i].ResetLadderTraversal(true);
        }
    }
    if (m_mapname[0] && g_NavMesh.IsLoaded() && insbot_learn_persist.GetBool())
    {
        char gamedir[256];
        if (engine) engine->GetGameDir(gamedir, sizeof(gamedir));
        char path[512];
        _snprintf(path, sizeof(path), "%s/addons/insbots_learn_%s.txt", gamedir, m_mapname);
        g_NavMesh.LoadBadLinks(path);
    }
}

void CBotRoster::LevelInit(const char *mapname)
{
    _snprintf(m_mapname, sizeof(m_mapname), "%s", mapname ? mapname : "");
    ResetPlayerSquads();  // clear fireteam rosters for fresh level
    m_mapname[sizeof(m_mapname) - 1] = 0;

    g_ForceGotoActive = false;
    g_ForceGotoPos = Vector(0.0f, 0.0f, 0.0f);

    // bots don't survive a level change server-side
    for (int i = 0; i < 64; i++)
        if (m_bots[i].m_inUse) m_bots[i].OnRemoved();
    ResetBodyGracePlayers(true);
    ResetLadderQueues();
    IntelReset();
    HearingReset();
    m_captureUpdateTime[0] = m_captureUpdateTime[1] = m_captureUpdateTime[2] = -999.0f;
    m_captureRefillTime[0] = m_captureRefillTime[1] = m_captureRefillTime[2] = -999.0f;
    m_captureTarget[0] = m_captureTarget[1] = m_captureTarget[2] = -1;
    ResetHadithaDoorGate();
    ResetSniperCaches();
    InsBots_ResetPhysicalPortalCache();

    // try to load maps/<map>.nav from the game dir
    char gamedir[256];
    engine->GetGameDir(gamedir, sizeof(gamedir));
    char path[512];
    _snprintf(path, sizeof(path), "%s/addons/insbots_nav/%s.nav", gamedir, m_mapname);
    m_navLoadedForMap = g_NavMesh.Load(path);
    if (!m_navLoadedForMap)
    {
        _snprintf(path, sizeof(path), "%s/maps/%s.nav", gamedir, m_mapname);
        m_navLoadedForMap = g_NavMesh.Load(path);
        if (m_navLoadedForMap)
        {
            Msg("[insbots] WARNING: Loaded nav from maps folder (%s). Move it to insurgency/addons/insbots_nav/ to prevent clients from downloading it!\n", path);
        }
    }

    // learn which faction each team number is on THIS map (flips per map)
    InsBots_LoadMapFactions(gamedir, m_mapname);


    // restore what the bots learned about this map in earlier sessions
    _snprintf(s_learnPath, sizeof(s_learnPath), "%s/addons/insbots_learn_%s.txt",
              gamedir, m_mapname);
    if (m_navLoadedForMap && insbot_learn_persist.GetBool())
    {
        int n = g_NavMesh.LoadBadLinks(s_learnPath);
        if (n > 0)
            Msg("[insbots] restored %d learned bad nav links for %s\n", n, m_mapname);
    }
}

void CBotRoster::BuildCoverDatabase()
{
    m_coverCount = 0;
    m_coverCapacity = 0;

    if (!g_NavMesh.IsLoaded())
    {
        Msg("[insbots] cover db: nav not loaded, database empty\n");
        return;
    }

    float rayLen = insbot_cover_ray_len.GetFloat();
    float cutoff = insbot_cover_ray_cutoff.GetFloat();
    if (rayLen < 50.0f) rayLen = 50.0f;
    if (cutoff < 30.0f) cutoff = 30.0f;
    if (cutoff > rayLen) cutoff = rayLen;

    int areaTotal = g_NavMesh.Count();
    if (areaTotal <= 0) { Msg("[insbots] cover db: no areas\n"); return; }

    int maxPts = areaTotal * 5;
    m_coverPoints = (CoverPoint *)malloc(sizeof(CoverPoint) * maxPts);
    m_coverOwnerSlot = (short *)malloc(sizeof(short) * maxPts);
    m_coverOwnedSince = (float *)malloc(sizeof(float) * maxPts);
    if (!m_coverPoints || !m_coverOwnerSlot || !m_coverOwnedSince)
    {
        Msg("[insbots] cover db: malloc failed (%d points capacity)\n", maxPts);
        free(m_coverPoints); free(m_coverOwnerSlot); free(m_coverOwnedSince);
        m_coverPoints = 0; m_coverOwnerSlot = 0; m_coverOwnedSince = 0;
        return;
    }
    m_coverCapacity = maxPts;
    for (int i = 0; i < maxPts; i++) { m_coverOwnerSlot[i] = -1; m_coverOwnedSince[i] = 0.0f; }

    int spawnSkipped = 0;
    int samplesTried = 0;
    int popHist[9] = {0};
    static const float DIR_YAW[8] = { 0.f,45.f,90.f,135.f,180.f,225.f,270.f,315.f };
    static const float SAMPLE_FRAC[5][2] =
    {
        { 0.5f, 0.5f },   // centre
        { 0.15f,0.15f },  { 0.85f,0.15f },
        { 0.15f,0.85f },  { 0.85f,0.85f }
    };

    CTraceFilterWorldAndPropsOnly wf;
    Ray_t ray; trace_t tr;

    for (int ai = 0; ai < g_NavMesh.Count(); ++ai)
    {
        const NavAreaData *A = g_NavMesh.Area(ai);
        if (!A) continue;
        
        

        float w = A->hi.x - A->lo.x;
        float hh = A->hi.y - A->lo.y;

        for (int smp = 0; smp < 5; ++smp)
        {
            Vector C(A->lo.x + w * SAMPLE_FRAC[smp][0],
                     A->lo.y + hh * SAMPLE_FRAC[smp][1],
                     A->center.z);

                        bool insideSpawnZone = false;
            for (int j = 0; j < g_Objectives.Count(); ++j)
            {
                const ObjPoint *op2 = g_Objectives.Get(j);
                if (!op2) continue;
                if (C.x >= op2->minX && C.x <= op2->maxX && C.y >= op2->minY && C.y <= op2->maxY)
                {
                    if (g_Objectives.IsSpawnZone(j, 1) || g_Objectives.IsSpawnZone(j, 2))
                        insideSpawnZone = true;
                    break;
                }
            }
            if (insideSpawnZone) { spawnSkipped++; continue; }

unsigned char blockMask[8] = {0};
            unsigned char crouchMask[8] = {0};
            int popcount = 0;
            bool anyTallBlock = false;

            for (int d = 0; d < 8; ++d)
            {
                float yr = DIR_YAW[d] * (3.14159265358979f / 180.0f);
                Vector dir(cosf(yr), sinf(yr), 0.0f);

                Vector eyeS = C + Vector(0, 0, 58);
                ray.Init(eyeS, eyeS + dir * rayLen);
                enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &wf, &tr);
                float distS = tr.fraction * rayLen;
                blockMask[d] = (distS < cutoff) ? 1 : 0;
                if (blockMask[d]) anyTallBlock = true;

                Vector eyeC = C + Vector(0, 0, 34);
                ray.Init(eyeC, eyeC + dir * rayLen);
                enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &wf, &tr);
                float distC = tr.fraction * rayLen;
                crouchMask[d] = (distC < cutoff) ? 1 : 0;
            }

            for (int d = 0; d < 8; ++d) if (blockMask[d]) popcount++;
            int crouchPopcount = 0;
            for (int d = 0; d < 8; ++d) if (crouchMask[d]) crouchPopcount++;
            samplesTried++;
            popHist[popcount]++;

            // Fix H-1: accept low cover. When standing rays show little occlusion
            // but crouch rays are mostly blocked, this is a sandbag/crate-style
            // low cover point: store it with tier from crouch protection and
            // isLowOnly = 1 so combat prefers standing fire positions.
            int tier = COVER_NONE;
            bool lowOnly = false;
            if (popcount >= 2)
            {
                if      (popcount >= 7) tier = COVER_FULL;
                else if (popcount >= 4) tier = COVER_PARTIAL;
                else                    tier = COVER_SPOT;
                lowOnly = false;
            }
            else if (crouchPopcount >= 2)
            {
                if      (crouchPopcount >= 7) tier = COVER_FULL;
                else if (crouchPopcount >= 4) tier = COVER_PARTIAL;
                else                          tier = COVER_SPOT;
                lowOnly = true;
            }
            else continue;

            CoverPoint &dst = m_coverPoints[m_coverCount];
            dst.pos = C;
            memcpy(dst.blockMask, blockMask, sizeof(blockMask));
            memcpy(dst.crouchMask, crouchMask, sizeof(crouchMask));
            dst.tier = tier;
            dst.isLowOnly = lowOnly ? 1 : 0;
            dst.areaIdx = (short)ai;
            m_coverCount++;
        }
    }

Msg("[insbots] cover db built: %d pts / %d areas (%d samples tried, %d spawn-skipped)\n",
        m_coverCount, areaTotal, samplesTried, spawnSkipped);
    BotDbg("[insbots] popcount histogram: [0]=%d [1]=%d [2]=%d [3]=%d [4]=%d [5+]=%d\n",
           popHist[0], popHist[1], popHist[2], popHist[3], popHist[4],
           popHist[5]+popHist[6]+popHist[7]+popHist[8]);
}

void CBotRoster::FreeCoverDatabase()
{
    if (m_coverPoints) { free(m_coverPoints); m_coverPoints = 0; }
    if (m_coverOwnerSlot) { free(m_coverOwnerSlot); m_coverOwnerSlot = 0; }
    if (m_coverOwnedSince) { free(m_coverOwnedSince); m_coverOwnedSince = 0; }
    m_coverCount = 0;
    m_coverCapacity = 0;
}

bool CBotRoster::IsCoverOccupiedByOther(int coverIdx, int myRosterIdx) const
{
    short slot = CoverOwnerSlot(coverIdx);
    if (slot < 0 || slot == myRosterIdx) return false;
    const CInsBot *o = (slot < 64) ? &m_bots[slot] : 0;
    return o && o->m_inUse && o->m_info && !o->m_info->IsDead();
}
void CBotRoster::LevelShutdown()
{
    InsBots_SaveLearn();
    ResetPlayerSquads();
    FreeCoverDatabase();
    g_ForceGotoActive = false;
    g_ForceGotoPos = Vector(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 64; i++)
        if (m_bots[i].m_inUse) m_bots[i].OnRemoved();
    ResetBodyGracePlayers(true);
    ResetLadderQueues();
    HearingReset();
    ResetHadithaDoorGate();
    ResetSniperCaches();
    InsBots_ResetPhysicalPortalCache();
    g_NavMesh.Free();
    m_navLoadedForMap = false;
}

void InsBot_DebugAmmo()
{
    Msg("[insbots] AMMO DEBUG INFO:\n");
    Msg("  s_offset_m_hActiveWeapon: %d\n", s_offset_m_hActiveWeapon);
    Msg("  s_offset_m_iClip1: %d\n", s_offset_m_iClip1);
}

void InsBots_SetGotoCmd(edict_t *issuer, int argc, const char *arg1, const char *arg2, const char *arg3)
{
    if (argc >= 1 && arg1 && (!_stricmp(arg1, "clear") || !_stricmp(arg1, "off") || !_stricmp(arg1, "stop") || !_stricmp(arg1, "0")))
    {
        g_ForceGotoActive = false;
        Msg("[insbots] Released forced goto pathfinding override.\n");
        return;
    }
    if (argc >= 3 && arg1 && arg2 && arg3 && _stricmp(arg1, "here") && _stricmp(arg1, "aim"))
    {
        float gx = 0.0f, gy = 0.0f, gz = 0.0f;
        if (sscanf(arg1, "%f", &gx) == 1 && sscanf(arg2, "%f", &gy) == 1 && sscanf(arg3, "%f", &gz) == 1)
        {
            g_ForceGotoPos = Vector(gx, gy, gz);
            g_ForceGotoActive = true;
            Msg("[insbots] Forced all bots to pathfind to coordinate: (%.1f, %.1f, %.1f)\n", g_ForceGotoPos.x, g_ForceGotoPos.y, g_ForceGotoPos.z);
            return;
        }
    }
    
    // Trace the issuing human's eye view or location; server-console commands
    // fall back to the first connected human.
    edict_t *traceEdict = issuer;
    if (!traceEdict)
    {
        for (int i = 1; i <= gpGlobals->maxClients; i++)
        {
            edict_t *e2 = engine->PEntityOfEntIndex(i);
            if (!e2 || e2->IsFree()) continue;
            IPlayerInfo *pi2 = playerinfomanager->GetPlayerInfo(e2);
            if (pi2 && pi2->IsConnected() && !pi2->IsFakeClient())
            {
                traceEdict = e2;
                break;
            }
        }
    }
    if (traceEdict)
    {
        edict_t *e = traceEdict;
        IPlayerInfo *pi = playerinfomanager->GetPlayerInfo(e);
        if (pi && pi->IsConnected() && !pi->IsFakeClient())
        {
            if (argc >= 1 && arg1 && !_stricmp(arg1, "me"))
            {
                g_ForceGotoPos = pi->GetAbsOrigin();
                g_ForceGotoActive = true;
                Msg("[insbots] Forced all bots to pathfind to player position: (%.1f, %.1f, %.1f)\n", g_ForceGotoPos.x, g_ForceGotoPos.y, g_ForceGotoPos.z);
                return;
            }
            Vector eye = pi->GetAbsOrigin() + Vector(0, 0, 64);
            float pitch = 0.0f, yaw = 0.0f;
            if (s_offset_angEyeAngles0 >= 0 && s_offset_angEyeAngles1 >= 0 && e->GetUnknown() && e->GetUnknown()->GetBaseEntity())
            {
                char *pBase = (char*)e->GetUnknown()->GetBaseEntity();
                pitch = *(float*)(pBase + s_offset_angEyeAngles0);
                yaw   = *(float*)(pBase + s_offset_angEyeAngles1);
            }
            else
            {
                QAngle ang = pi->GetLastUserCommand().viewangles;
                pitch = ang.x; yaw = ang.y;
            }
            float radPitch = pitch * (3.14159265f / 180.0f);
            float radYaw   = yaw * (3.14159265f / 180.0f);
            Vector fwd(cosf(radPitch) * cosf(radYaw), cosf(radPitch) * sinf(radYaw), -sinf(radPitch));

            Ray_t ray; trace_t tr;
            ray.Init(eye, eye + fwd * 8192.0f);
            CTraceFilterWorldOnly wf;
            enginetrace->TraceRay(ray, MASK_SOLID, &wf, &tr);

            g_ForceGotoPos = tr.endpos;
            g_ForceGotoActive = true;
            Msg("[insbots] Forced all bots to pathfind to mouse point / aim location: (%.1f, %.1f, %.1f) [Overrides freeze & nav toggle!]\n", g_ForceGotoPos.x, g_ForceGotoPos.y, g_ForceGotoPos.z);
            return;
        }
    }
    Msg("[insbots] Could not trace player angle (are you in-game?). Usage: insbot goto OR insbot stop\n");
}
