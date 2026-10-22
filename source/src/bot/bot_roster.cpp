// ============================================================================
// bot_roster.cpp - CBotRoster: bot lifecycle, squads, sniper caches, cover DB.
//
// Everything that owns or enumerates bots lives here: add/kick, player squad
// assignment and refill, per-leader threat tracking, slot reservations, the
// sniper-perch cache, capture commitments, danger zones, and the level
// lifecycle (nav load, cover database build/free).
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
#include "engine/ivdebugoverlay.h"
#include "gametrace.h"
#include "bspflags.h"
#include "const.h"
#include "convar.h"
#include "string_t.h"
#include "bot.h"
#include "nav.h"
#include "objective.h"
#include "voice.h"
#include "profile.h"
#include "bot_internal.h"

// ---- externs this TU needs (same pattern as bot.cpp / objective.cpp) -------
extern IVEngineServer       *engine;
extern IEngineTrace         *enginetrace;
extern IBotManager          *botmanager;
extern IPlayerInfoManager   *playerinfomanager;
extern CGlobalVars          *gpGlobals;
extern IVDebugOverlay       *debugoverlay;
extern CfgVar                insbot_capture_percent;
extern CfgVar                insbot_cover_ray_cutoff;
extern CfgVar                insbot_cover_ray_len;
extern CfgVar                insbot_danger_radius;
extern CfgVar                insbot_draw;
extern CfgVar                insbot_draw_team;
extern CfgVar                insbot_freeze;
extern CfgVar                insbot_objectives;
extern CfgVar                insbot_sniper_perch_time_max;
extern CfgVar                insbot_sniper_perch_time_min;
extern CfgVar                insbot_crouch_eye_height;
extern CfgVar                insbot_detect;
extern CfgVar                insbot_learn_persist;
extern bool                  g_RoundFrozen;
extern bool                  g_HoldQuotaJoins;
extern int                   s_offset_m_hActiveWeapon;
extern int                   s_offset_m_iClip1;

CBotRoster g_Bots;

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
    int a_res = GetReservationCount(0);
    int b_res = GetReservationCount(1);
    int slot = ((a + a_res) <= (b + b_res)) ? 0 : 1;
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

// Reservation lifetime. A player who never dies (or a lost clear-event) must
// not block a slot forever.
#define INSBOT_RESERVATION_TIMEOUT 300.0f

static inline float RosterNow()
{
    return gpGlobals ? gpGlobals->curtime : 0.0f;
}

void CBotRoster::ReserveRole(int teamSlot, int roleId, int userId)
{
    if (teamSlot < 0 || teamSlot > 1 || roleId <= 0) return;
    float now = RosterNow();

    // Update existing if already present
    for (int i = 0; i < 32; i++)
    {
        if (m_reservations[i].inUse && m_reservations[i].teamSlot == teamSlot && m_reservations[i].roleId == roleId)
        {
            m_reservations[i].userId = userId;
            m_reservations[i].reserveTime = now;
            return;
        }
    }
    // Find free slot
    for (int i = 0; i < 32; i++)
    {
        if (!m_reservations[i].inUse)
        {
            m_reservations[i].inUse = true;
            m_reservations[i].teamSlot = teamSlot;
            m_reservations[i].roleId = roleId;
            m_reservations[i].userId = userId;
            m_reservations[i].reserveTime = now;
            return;
        }
    }
}

void CBotRoster::UnreserveRole(int teamSlot, int roleId)
{
    for (int i = 0; i < 32; i++)
    {
        if (m_reservations[i].inUse && m_reservations[i].teamSlot == teamSlot && m_reservations[i].roleId == roleId)
        {
            m_reservations[i].inUse = false;
            return;
        }
    }
}

void CBotRoster::UnreserveUser(int userId)
{
    if (userId <= 0) return;
    for (int i = 0; i < 32; i++)
    {
        if (m_reservations[i].inUse && m_reservations[i].userId == userId)
        {
            m_reservations[i].inUse = false;
        }
    }
}

void CBotRoster::ClearAllReservations()
{
    for (int i = 0; i < 32; i++)
        m_reservations[i].inUse = false;
}

void CBotRoster::SweepReservations()
{
    float now = RosterNow();
    for (int i = 0; i < 32; i++)
    {
        if (!m_reservations[i].inUse) continue;
        if (m_reservations[i].reserveTime <= 0.0f) continue; // no expiry
        if (now - m_reservations[i].reserveTime >= INSBOT_RESERVATION_TIMEOUT)
        {
            m_reservations[i].inUse = false;
            Msg("[insbots] slot reservation expired (teamSlot %d, role %d)\n",
                m_reservations[i].teamSlot, m_reservations[i].roleId);
        }
    }
}

bool CBotRoster::IsRoleReserved(int teamSlot, int roleId) const
{
    for (int i = 0; i < 32; i++)
    {
        if (m_reservations[i].inUse && m_reservations[i].teamSlot == teamSlot && m_reservations[i].roleId == roleId)
            return true;
    }
    return false;
}

int CBotRoster::GetReservationCount(int teamSlot) const
{
    int count = 0;
    for (int i = 0; i < 32; i++)
    {
        if (m_reservations[i].inUse && (teamSlot < 0 || m_reservations[i].teamSlot == teamSlot))
            count++;
    }
    return count;
}

bool CBotRoster::IsRoleInUse(int teamSlot, int roleId) const
{
    if (IsRoleReserved(teamSlot, roleId))
        return true;
    for (int i = 0; i < 64; i++)
    {
        if (m_bots[i].m_inUse && m_bots[i].m_teamSlot == teamSlot && m_bots[i].m_resolvedRoleId == roleId)
            return true;
    }
    return false;
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
    SweepReservations();

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

void CBotRoster::InvalidateNavMeshState(bool reloadSucceeded)
{
    ResetSniperCaches();
    ResetLadderQueues();
    ResetHadithaDoorGate();
    InvalidateAdvanceTargets();
    InsBots_ResetPhysicalPortalCache();
    InsBots_ResetPortalTransit();
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
    if (reloadSucceeded)
        m_navLoadedForMap = true;
    if (reloadSucceeded && m_mapname[0] && insbot_learn_persist.GetBool())
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
    ClearAllReservations(); // clear any stale slot reservations from previous level
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
    InsBots_ResetPortalTransit();

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
    FreeCoverDatabase();   // release prior buffers on rebuild (prevents leak on repeated ServerActivate)
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

    int maxPts = (m_coverCapacityOverride > 0) ? m_coverCapacityOverride
                                               : areaTotal * 5;
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
    bool capacityHit = false;
    int popHist[COVER_SECTORS + 1] = {0};
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
        if (capacityHit) break;
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
                    {
                        insideSpawnZone = true;
                        break;
                    }
                }
            }
            if (insideSpawnZone) { spawnSkipped++; continue; }

            unsigned char blockMask[COVER_SECTORS] = {0};
            unsigned char crouchMask[COVER_SECTORS] = {0};
            bool anyTallBlock = false;

            bool sampleInSolid = false;
            for (int d = 0; d < COVER_SECTORS; ++d)
            {
                float yr = (float)d * COVER_SECTOR_DEG * (3.14159265358979f / 180.0f);
                Vector dir(cosf(yr), sinf(yr), 0.0f);

                Vector eyeS = C + Vector(0, 0, 58);
                ray.Init(eyeS, eyeS + dir * rayLen);
                enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &wf, &tr);
                if (tr.startsolid || tr.allsolid) { sampleInSolid = true; break; }
                float distS = tr.fraction * rayLen;
                blockMask[d] = (distS < cutoff) ? 1 : 0;
                if (blockMask[d]) anyTallBlock = true;

                Vector eyeC = C + Vector(0, 0, 34);
                ray.Init(eyeC, eyeC + dir * rayLen);
                enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &wf, &tr);
                if (tr.startsolid || tr.allsolid) { sampleInSolid = true; break; }
                float distC = tr.fraction * rayLen;
                crouchMask[d] = (distC < cutoff) ? 1 : 0;
            }
            if (sampleInSolid) continue;   // skip to next sample point

            int popcount = CoverMaskPopcount(blockMask);
            int crouchPopcount = CoverMaskPopcount(crouchMask);
            samplesTried++;
            popHist[popcount]++;

            // Fix H-1: accept low cover. When standing rays show little occlusion
            // but crouch rays are mostly blocked, this is a sandbag/crate-style
            // low cover point: store it with tier from crouch protection and
            // isLowOnly = 1 so combat prefers standing fire positions.
            int tier = CoverTierForPopcount(popcount);
            bool lowOnly = false;
            if (tier == COVER_NONE)
            {
                tier = CoverTierForPopcount(crouchPopcount);
                if (tier == COVER_NONE) continue;
                lowOnly = true;
            }

            // Hard ceiling: maxPts is exactly areaTotal*5, which is the largest
            // possible sample count rather than a margin, so a future extra
            // insertion site would overrun the allocation silently.
            if (m_coverCount >= m_coverCapacity)
            {
                capacityHit = true;
                break;
            }

            CoverPoint &dst = m_coverPoints[m_coverCount];
            dst.pos = C;
            memcpy(dst.blockMask, blockMask, sizeof(dst.blockMask));
            memcpy(dst.crouchMask, crouchMask, sizeof(dst.crouchMask));
            dst.tier = tier;
            dst.isLowOnly = lowOnly ? 1 : 0;
            dst.areaIdx = (short)ai;
            m_coverCount++;
        }
    }

Msg("[insbots] cover db built: %d pts / %d areas (%d samples tried, %d spawn-skipped)%s\n",
        m_coverCount, areaTotal, samplesTried, spawnSkipped,
        capacityHit ? " [CAPACITY REACHED - points dropped]" : "");
    BotDbg("[insbots] popcount histogram:");
    for (int i = 0; i <= COVER_SECTORS; i++) BotDbg(" [%d]=%d", i, popHist[i]);
    BotDbg("\n");

    // Index the points just stored. Must come after the loop so the grid and
    // the database always describe the same set.
    BuildCoverGrid();
}

// Build the uniform grid over cover-point positions with a counting sort:
// count per cell, prefix-sum, then scatter. O(points + cells) with no
// per-point allocation, which matters because this runs on every map load.
void CBotRoster::BuildCoverGrid()
{
    FreeCoverGrid();
    if (m_coverCount <= 0 || !m_coverPoints) return;

    float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
    for (int i = 0; i < m_coverCount; i++)
    {
        const CoverPoint *cp = &m_coverPoints[i];
        if (cp->pos.x < minX) minX = cp->pos.x;
        if (cp->pos.x > maxX) maxX = cp->pos.x;
        if (cp->pos.y < minY) minY = cp->pos.y;
        if (cp->pos.y > maxY) maxY = cp->pos.y;
    }
    m_coverGridMinX = minX;
    m_coverGridMinY = minY;
    m_coverGridW = (int)((maxX - minX) / COVER_GRID_CELL) + 1;
    m_coverGridH = (int)((maxY - minY) / COVER_GRID_CELL) + 1;
    if (m_coverGridW < 1) m_coverGridW = 1;
    if (m_coverGridH < 1) m_coverGridH = 1;

    int cells = m_coverGridW * m_coverGridH;
    m_coverGridStart = (int *)malloc(sizeof(int) * (cells + 1));
    if (!m_coverGridStart) { FreeCoverGrid(); return; }
    for (int i = 0; i <= cells; i++) m_coverGridStart[i] = 0;

    for (int i = 0; i < m_coverCount; i++)
    {
        int c = CoverGridCellY(m_coverPoints[i].pos.y) * m_coverGridW
              + CoverGridCellX(m_coverPoints[i].pos.x);
        m_coverGridStart[c + 1]++;
    }
    for (int i = 1; i <= cells; i++) m_coverGridStart[i] += m_coverGridStart[i - 1];

    m_coverGridCells = (int *)malloc(sizeof(int) * m_coverCount);
    if (!m_coverGridCells) { FreeCoverGrid(); return; }
    int *cursor = (int *)malloc(sizeof(int) * cells);
    if (!cursor) { FreeCoverGrid(); return; }
    for (int i = 0; i < cells; i++) cursor[i] = m_coverGridStart[i];
    for (int i = 0; i < m_coverCount; i++)
    {
        int c = CoverGridCellY(m_coverPoints[i].pos.y) * m_coverGridW
              + CoverGridCellX(m_coverPoints[i].pos.x);
        m_coverGridCells[cursor[c]++] = i;
    }
    free(cursor);
}

void CBotRoster::FreeCoverGrid()
{
    if (m_coverGridStart) { free(m_coverGridStart); m_coverGridStart = 0; }
    if (m_coverGridCells) { free(m_coverGridCells); m_coverGridCells = 0; }
    m_coverGridW = m_coverGridH = 0;
    m_coverGridMinX = m_coverGridMinY = 0.0f;
}

void CBotRoster::FreeCoverDatabase()
{
    FreeCoverGrid();
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

void CBotRoster::ReleaseCoverOwnership(int rosterIdx)
{
    if (!m_coverOwnerSlot || rosterIdx < 0 || rosterIdx >= 64) return;
    for (int i = 0; i < m_coverCapacity; i++)
    {
        if (m_coverOwnerSlot[i] == (short)rosterIdx)
        {
            m_coverOwnerSlot[i] = -1;
            m_coverOwnedSince[i] = 0.0f;
        }
    }
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
    InsBots_ResetPortalTransit();
    g_NavMesh.Free();
    m_navLoadedForMap = false;
}

CInsBot *CBotRoster::AddBotToRole(int faction, int roleId)
{
    int targetFaction = (faction == 1) ? FACTION_IRAQI : FACTION_USMC;
    int slot = GetTeamSlotForFaction(targetFaction);
    if (IsRoleReserved(slot, roleId))
    {
        Msg("[insbots] AddBotToRole: slot %d on team %d is reserved for a human player\n", roleId, slot);
        return 0;
    }
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
