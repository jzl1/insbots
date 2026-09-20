// ============================================================================
// bot_body.cpp - player-body collision groups and body-recovery leases.
//
// Two related mechanisms live here. Body grace temporarily moves a crowded
// player's collision group to COLLISION_GROUP_DEBRIS_TRIGGER so a bot can walk
// out of a jam instead of being pinned; the lease is reference-counted by owner
// slot so several recovering bots cannot release each other's grace early.
// Spawn no-collide is the same trick applied for a short window after a bot
// spawns inside a human, with its own expiry and navigation reset.
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
extern int                   s_offset_m_CollisionGroup;
extern CfgVar                insbot_doorway_teleport;
extern CfgVar                insbot_ladder_nocollide;
extern CfgVar                insbot_spawn_nocollide;
extern CfgVar                insbot_spawn_nocollide_time;
extern CfgVar                insbot_stuck_dist;
extern CfgVar                insbot_stuck_nocollide;
extern CfgVar                insbot_stuck_time;
extern CfgVar                insbot_unstuck_teleport;
extern CfgVar                insbot_unstuck_teleport_time;

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

const char *TeleportFailureName(int reason)
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

static int PlayerCollisionGroup(edict_t *edict)
{
    if (!edict || edict->IsFree()) return -1;
    IServerUnknown *unknown = edict->GetUnknown();
    ICollideable *collideable = unknown ? unknown->GetCollideable() : 0;
    return collideable ? collideable->GetCollisionGroup() : -1;
}

bool WritePlayerCollisionGroup(edict_t *edict, int collisionGroup)
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
            float lipTol = LadderLipTol();
            bool atLip = ladder && (m_ladderClimbUp
                ? pos.z >= endHeight - lipTol : pos.z <= endHeight + lipTol);
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
