// ============================================================================
// bot_core.cpp - CInsBot lifecycle, construction, and basic accessors.
//
// The small, widely-called surface of a single bot: is-it-a-sniper, clock,
// nav-area lookup with a per-frame cache, create/adopt/remove, and eye
// positions. Everything here is called from several other bot/ translation
// units, so nothing in this file is `static`.
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
#include "convar.h"
#include "bot.h"
#include "nav.h"
#include "objective.h"
#include "bot_internal.h"

// ---- externs this TU needs (same pattern as bot.cpp / objective.cpp) -------
extern IBotManager          *botmanager;
extern IPlayerInfoManager   *playerinfomanager;
extern CGlobalVars          *gpGlobals;
extern CfgVar                insbot_join_interval;
extern CfgVar                insbot_crouch_eye_height;

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

bool CInsBot::WriteCollisionGroup(int collisionGroup)
{
    return WritePlayerCollisionGroup(m_edict, collisionGroup);
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
    m_resolvedRoleId = requestedRoleId;
    _snprintf(m_name, sizeof(m_name), "%s", name);
    m_state = BOT_JOINING;
    m_ordinal = (int)(this - g_Bots.m_bots);
    // A newly created client gets its full setup on the next frame. Waiting a
    // whole join interval can miss the short setup opportunity after a round
    // has already started. Quota batches are still held by g_HoldQuotaJoins.
    m_nextJoinCmdTime = (requestedTeamArg >= 0)
                      ? Now() : Now() + insbot_join_interval.GetFloat();
    m_joinCmdIndex = 0;
    m_classSetupSent = false;
    m_nextReinforcePulse = 0.0f;
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
    m_resolvedRoleId = -1;
    _snprintf(m_name, sizeof(m_name), "%s", pi ? pi->GetName() : "[BOT]");
    m_name[sizeof(m_name) - 1] = 0;
    m_state = BOT_JOINING;
    m_ordinal = (int)(this - g_Bots.m_bots);
    m_nextJoinCmdTime = Now() + insbot_join_interval.GetFloat();
    m_classSetupSent = false;
    m_nextReinforcePulse = 0.0f;
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
    g_Bots.ReleaseCoverOwnership((int)(this - g_Bots.m_bots));
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
