// ============================================================================
// bot_cover.cpp - Cover System V2: exposure evaluation and spot selection.
//
// Works against the cover database built by CBotRoster::BuildCoverDatabase
// (see bot_roster.cpp). The 8-sector masks there drive both the "am I exposed
// right now" check and the scored search for a better spot.
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
#include "in_buttons.h"
#include "bot.h"
#include "nav.h"
#include "objective.h"
#include "bot_internal.h"

// ---- externs this TU needs (same pattern as bot.cpp / objective.cpp) -------
extern IVEngineServer       *engine;
extern IPlayerInfoManager   *playerinfomanager;
extern CGlobalVars          *gpGlobals;
extern CfgVar                insbot_cover_search_radius;
extern CfgVar                insbot_cover_max_reach;
extern CfgVar                insbot_cover_tier_weight;
extern CfgVar                insbot_cover_dist_weight;
extern CfgVar                insbot_cover_advance_penalty;
extern CfgVar                insbot_cover_min_score;
extern CfgVar                insbot_cover_formation_bonus;
extern CfgVar                insbot_intel_time;
extern CfgVar                insbot_debug;

bool CInsBot::IsPositionExposed(const Vector &myPos, const Vector &threatPos) const
{
    if (!g_Bots.m_coverPoints || g_Bots.CoverCount() <= 0) return true;

    float searchR = 75.0f;
    for (int ci = 0; ci < g_Bots.CoverCount(); ci++)
    {
        const CoverPoint *cp = g_Bots.CoverAt(ci);
        if (!cp) continue;
        if (xyd(cp->pos, myPos) > searchR) continue;

        float yawDeg = atan2f(threatPos.y - cp->pos.y, threatPos.x - cp->pos.x) * (180.0f / 3.14159265358979f);
        yawDeg = fmodf(yawDeg + 360.0f, 360.0f);
        int sec = ((int)(fmodf(yawDeg + 22.5f, 360.0f) / 45.0f)) & 7;

        // If a nearby point blocks this bearing while standing, or if it is low
        // cover and the bot is already ducked, the bot is not exposed in the open.
        if (cp->blockMask[sec] || (cp->crouchMask[sec] && (m_lastButtons & IN_DUCK)))
            return false;
    }
    return true; // no protective cover point within 75u
}

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
        if (!IntelEnemyIsLive(myTeam, i)) continue;
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

        // Guard: if primaryThreat is unset (zero-vector), skip the advance penalty
        // to avoid penalizing based on distance to world origin.
        if (primaryThreat.x != 0.0f || primaryThreat.y != 0.0f || primaryThreat.z != 0.0f)
        {
            float dT = sqrtf((candPos - primaryThreat).LengthSqr());
            if (dT < myT - 48.0f) worst -= advP;
        }
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
    g_Bots.ReleaseCoverOwnership(myRosterIdx);   // release any previously held spot
    g_Bots.m_coverOwnerSlot[bestIdx] = (short)myRosterIdx;
    g_Bots.m_coverOwnedSince[bestIdx] = now;

        if (insbot_debug.GetBool()) BotDbg("[insbots] %s COVER chose idx=%d tier=%d low=%u score=%.1f dist=%.0f\n",
            m_name, bestIdx, outTier, bp->isLowOnly, bestScore, bestDist);
    return true;
}
