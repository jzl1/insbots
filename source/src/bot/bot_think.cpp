// ============================================================================
// bot_think.cpp - CInsBot::ThinkAlive and debug overlay drawing.
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
#include "server_class.h"
#include "iservernetworkable.h"
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
extern CfgVar insbot_cover_on_engage;
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
extern CfgVar insbot_ladder_lip_tol;
extern CfgVar insbot_ads_mode;
extern int s_offset_m_IronsightsState;
extern CfgVar insbot_hear_cooldown;
extern CfgVar insbot_detect;
#include "bot_internal.h"

void InsBots_SaveLearn();     // flush learned bad links to disk
// ============================================================================
// CInsBot
// ============================================================================
// ---------------------------------------------------------------------------
// Weapon-inventory validation ("grenade switch" crash guard)
//
// CInsBot::SwitchToWeapon() hands an entity pointer to
// CBasePlayer::Weapon_Switch(), which calls CBaseCombatWeapon virtuals on it
// (vtable slot 193 == CanDeploy). Only a genuine carried item may be passed.
//
// INS:MIC registers the carried ordnance item and its thrown projectile as
// SEPARATE server classes with SEPARATE classnames:
//
//   carried : "weapon_m18"   -> CGrenadeM18         (275 vtable slots)
//   thrown  : "grenade_m18"  -> CGrenadeThrownBase  (190 vtable slots)
//
// The old lookup accepted any inventory classname that merely CONTAINED the
// weapon token, so the thrown "grenade_m18" aliased the carried "weapon_m18".
// Handing the 190-slot projectile to Weapon_Switch() made slot 193 read past
// the end of its vtable and jump into .rodata (observed: CSmokeGrenade).
//
// The fix is two-part:
//   * only "weapon_*" entities are considered (InsBotIsCarriedWeapon), and
//   * the classname must match the requested weapon EXACTLY.
//
// See docs/superpowers/plans/2026-09-11-fix-smoke-weapon-switch-crash.md.
// ---------------------------------------------------------------------------

// INS:MIC's m_hMyWeapons is a SEVEN-element array, NOT the SDK's MAX_WEAPONS (48).
//
// Verified against the shipped binaries rather than the SDK header:
//   Linux   CBasePlayer::RemoveAllWeapons @0x6CB440 does `add eax, 0xa38` and
//           iterates `cmp [ebp-0xc], 6 / jg`  -> 7 slots.
//   Windows 0x10103DDC and 0x1011CED2 both do `mov eax,[ebx+esi*4+0xa24]` with
//           a `cmp esi, 7 / jl` guard -> 7 slots, and 0x1011E60C places the next
//           member at +0xa40, i.e. 0xa40-0xa24 = 0x1C = 7 * sizeof(CHandle).
//
// Reading past the 7th slot walks into adjacent player members and can resolve a
// garbage handle to a live weapon_* entity, so this bound must stay 7.
#define INS_MAX_WEAPONS 7

// True only for an entity the game exposes as a carried inventory item, i.e. an
// entity whose classname is a "weapon_*" name.
//
// INS:MIC registers the carried item and the thrown projectile as SEPARATE
// server classes with SEPARATE classnames:
//
//   carried : classname "weapon_m18"   server class CGrenadeM18         (275 vtable slots)
//   thrown  : classname "grenade_m18"  server class CGrenadeThrownBase  (190 vtable slots)
//
// The 275-slot carried vtable extends past slot 193; the 190-slot projectile
// vtable does NOT, so the CanDeploy call reads past the end of the projectile's
// vtable and jumps into .rodata. The two are distinguished by their classname,
// so filtering on the "weapon_" prefix - plus the exact classname match required
// at the call sites - keeps the projectile out.
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

// ---------------------------------------------------------------------------
// ThinkAlive phases.
//
// These were lifted out of ThinkAlive verbatim, with no logic change, to break
// up a 2.4k-line body. Each one touches no ThinkAlive local except `now`, so
// the move is mechanical: the block was cut and pasted unchanged and the call
// sites pass `now`. Keeping them separate from ThinkAlive also makes them
// individually testable in isolation later.
// ---------------------------------------------------------------------------

// Dead-bot reset. Runs instead of the rest of ThinkAlive, and leaves the bot in
// a clean state so the next life starts fresh. Every field reset here is
// per-life state; anything that must survive death (fireteam identity, cover
// ownership bookkeeping) is handled explicitly below.
void CInsBot::ThinkDeadReset(float now)
{
    ResetLadderTraversal(true);
    ResetBodyNoCollideRecovery();
    EndSpawnNoCollide(true);
    m_state = BOT_DEAD;
    m_classSetupSent = false;
    m_nextReinforcePulse = 0.0f;
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
    m_wantAds = false;
    m_crouchFireUntil = 0; m_haveCover = false;
    m_chosenCoverTier = 0; m_chosenCoverIsLow = false;
    g_Bots.ReleaseCoverOwnership((int)(this - g_Bots.m_bots));
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
}

// Held-weapon profile resolution, engine clip synchronisation, and the
// damage callouts. Grouped because they all read the bot's live weapon state
// and must run before any combat or movement decision.
void CInsBot::ThinkWeaponAndAmmo(float now)
{
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
}

void CInsBot::ThinkAlive()
{
    INSPROF_SCOPE(INSPROF_THINKALIVE);
    float now = Now();

    // ADS intent is re-derived every frame (the combat block only runs for the
    // standard-firearms branch). Clearing it here stops a bot that stops
    // engaging, switches to a launcher or loses its enemy from holding a stale
    // request, which would latch the sights up and re-pulse forever.
    m_wantAds = false;

    if (m_info->IsDead())
    {
        ThinkDeadReset(now);
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

    ThinkWeaponAndAmmo(now);

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

                // ADS INTENT ONLY. The ATTACK3 press/release pulse is generated in
                // RunMove: CWeaponINSBase::ItemPostFrame() drives ToggleIronsights()
                // off the button-18 RELEASED edge, so a continuously held bit can
                // never raise the sights while the bot is engaging.
                if (useIronsights) {
                    if (!m_ironsights) {
                        m_ironsights = true;
                        m_ironsightTime = now;
                    }
                } else {
                    m_ironsights = false;
                }
                m_wantAds = useIronsights;

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

        // Proactive cover: if engaging in the open beyond 250u, seek cover even if healthy.
        // If already holding cover from proactive cover, stay committed until the 3.0s LOS exit.
        // Stationed snipers (at assigned perch <= 80u) are excluded: they hold their overwatch vantage point.
        bool sniperAtPerch = IsSniper() && m_haveSniperSpot && (xyd(pos, m_sniperSpot) <= 80.0f);
        bool proactiveCover = false;
        if (insbot_takecover.GetBool() && insbot_cover_on_engage.GetBool()
            && engaging && !leaderCoverActive && !sniperAtPerch)
        {
            // If this is a squad follower whose leader has moved far ahead (> 400u),
            // do not anchor in cover: prioritize keeping up with the leader.
            bool squadTetherBroken = false;
            if (IsSquadFollower())
            {
                edict_t *le = engine->PEntityOfEntIndex(m_squadLeaderIndex);
                IPlayerInfo *lpi = (le && !le->IsFree()) ? playerinfomanager->GetPlayerInfo(le) : 0;
                if (lpi && xyd(pos, lpi->GetAbsOrigin()) > 400.0f)
                    squadTetherBroken = true;
            }

            if (!squadTetherBroken)
            {
                if (m_haveCover)
                {
                    proactiveCover = true;
                }
                else if (losNow && dist > 250.0f)
                {
                    Vector threat = pi ? pi->GetAbsOrigin() : m_enemyLastPos;
                    if (IsPositionExposed(pos, threat))
                        proactiveCover = true;
                }
            }
        }

        bool  vulnerable = (insbot_takecover.GetBool()
                        && (hurt || suppressed || (reloadActive && !suppressHold))
                        && !leaderCoverActive) || proactiveCover;

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
                if (now > m_coverUntil)
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
                            // If this is proactive cover (not critically hurt), cap the dash to 350u.
                            // Running farther than 350u across open ground leaves the bot exposed too long.
                            float candDist = xyd(pos, spotTmp);
                            if (proactiveCover && candDist > 350.0f)
                            {
                                // Cover is too far; release ownership and stay in place
                                g_Bots.ReleaseCoverOwnership((int)(this - g_Bots.m_bots));
                                found = false;
                                if (insbot_debug.GetBool())
                                    BotDbg("[insbots] %s COVER rejected candDist=%.0f > 350u (open field fallback to crouch)\n",
                                           m_name, candDist);
                            }
                            else
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
                        }
                        chosenTier = t;
                    }
                    else
                    {
                        found = FindCover(threat, m_coverSpot);
                    }
                    if (found)
                    {
                        m_haveCover = true; m_coverUntil = now + 3.0f;
                        m_chosenCoverTier = chosenTier; m_chosenCoverIsLow = chosenLow;
                        if (!hadCover) Say(VOX_COVER);   // announce only on taking cover, not each refresh
                    }
                    else
                    {
                        m_haveCover = false;
                        m_chosenCoverTier = 0;
                        m_chosenCoverIsLow = false;
                        m_coverUntil = now + 1.0f; // Throttle re-queries for 1.0s
                        // Open-field fallback: if proactive cover failed (no cover within 350u),
                        // drop to crouch in place to minimize silhouette while firing.
                        if (proactiveCover)
                        {
                            buttons |= IN_DUCK;
                            if (insbot_debug.GetBool())
                                BotDbg("[insbots] %s COVER fallback: crouched in place (no cover within 350u)\n", m_name);
                        }
                    }
                }
                if (m_haveCover)
                {
                    float coverDist = xyd(pos, m_coverSpot);
                    if (coverDist > 40.0f)
                    {
                        MoveToward(m_coverSpot, fwd, side, buttons, view);
                        // Sights down while in transit: allows 100% walk speed instead of
                        // the engine's 35% ADS penalty, while weapon fire continues.
                        m_wantAds = false;
                    }
                    else
                    {
                        // FindCover returns exact NAV-area centres.  Stop once
                        // safely inside the selected area instead of repeatedly
                        // running through its centre and turning back toward it.
                        fwd = 0.0f;
                        side = 0.0f;
                        // Arrived at cover: shoulder weapon for full ADS accuracy
                        if (insbot_ironsights.GetBool() && (dist > 300.0f) && !IsHoldingM203())
                            m_wantAds = true;
                    }
                    // Run upright TO cover, duck only once near it.
                    // Low cover gets the full 56u duck radius; all other cover
                    // ducks at the 40u arrival threshold only.
                    {
                        float duckRadius = (insbot_cover_v2.GetBool() && m_chosenCoverIsLow) ? 56.0f : 40.0f;
                        if (coverDist < duckRadius)
                            buttons |= IN_DUCK;
                    }

                    static float s_lastCoverDbg[64];
                    static unsigned char s_coverArrived[64];
                    int mySlot = (int)(this - g_Bots.m_bots);
                    if (insbot_debug.GetBool() && mySlot >= 0 && mySlot < 64)
                    {
                        if (coverDist > 40.0f)
                        {
                            s_coverArrived[mySlot] = 0;
                            if (now - s_lastCoverDbg[mySlot] > 0.4f)
                            {
                                s_lastCoverDbg[mySlot] = now;
                                BotDbg("[insbots] %s COVER move: dist=%.0f pos=(%.0f,%.0f) target=(%.0f,%.0f) low=%d ads=%d\n",
                                       m_name, coverDist, pos.x, pos.y, m_coverSpot.x, m_coverSpot.y,
                                       m_chosenCoverIsLow ? 1 : 0, m_wantAds ? 1 : 0);
                            }
                        }
                        else if (!s_coverArrived[mySlot])
                        {
                            s_coverArrived[mySlot] = 1;
                            BotDbg("[insbots] %s COVER arrived: dist=%.0f pos=(%.0f,%.0f) low=%d ads=%d duck=%d\n",
                                   m_name, coverDist, pos.x, pos.y,
                                   m_chosenCoverIsLow ? 1 : 0, m_wantAds ? 1 : 0,
                                   (buttons & IN_DUCK) ? 1 : 0);
                        }
                    }

                    // Exit condition: if arrived at cover and line of sight to enemy is
                    // lost for >= lose_time, break cover and resume advancing.
                    if (!losNow && (now - m_enemyLastSeen >= insbot_lose_time.GetFloat()) && !hurt)
                    {
                        if (insbot_debug.GetBool())
                            BotDbg("[insbots] %s COVER exit: LOS lost >= %.1fs, breaking cover to advance\n",
                                   m_name, insbot_lose_time.GetFloat());
                        m_haveCover = false;
                        m_chosenCoverTier = 0;
                        m_chosenCoverIsLow = false;
                        g_Bots.ReleaseCoverOwnership((int)(this - g_Bots.m_bots));
                    }
                }
            }
            else if (objectiveFocus)
            {
                if (m_haveCover) g_Bots.ReleaseCoverOwnership((int)(this - g_Bots.m_bots));
                m_haveCover = false; m_chosenCoverTier = 0; m_chosenCoverIsLow = false;
                m_haveHoldSpot = false; m_haveSniperSpot = false;
                MoveToward(capOp->center, fwd, side, buttons, view);
            }
            else if (captureCornerTransit)
            {
                if (m_haveCover) g_Bots.ReleaseCoverOwnership((int)(this - g_Bots.m_bots));
                m_haveCover = false; m_chosenCoverTier = 0; m_chosenCoverIsLow = false;
                MoveToward(m_holdSpot, fwd, side, buttons, view);
            }
            else if (inObjZone)
            {
                // Already on the point: hold and defend it. Never chase an
                // enemy away from a capture zone we are committed to.
                if (m_haveCover) g_Bots.ReleaseCoverOwnership((int)(this - g_Bots.m_bots));
                m_haveCover = false; m_haveHoldSpot = false; m_haveSniperSpot = false; m_chosenCoverTier = 0; m_chosenCoverIsLow = false;
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
                    m_haveCover = false; m_chosenCoverTier = 0; m_chosenCoverIsLow = false;
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
                if (m_haveCover) g_Bots.ReleaseCoverOwnership((int)(this - g_Bots.m_bots));
                m_haveCover = false; m_chosenCoverTier = 0; m_chosenCoverIsLow = false;
                Vector chase = losNow ? pi->GetAbsOrigin() : m_enemyLastPos;
                // Use the same movement path as objectives and cover.  The old
                // inline chase duplicated navigation but skipped personal space
                // and narrow-passage queues entirely while a bot had an enemy.
                MoveToward(chase, fwd, side, buttons, view);
            }
            else
            {
                if (m_haveCover) g_Bots.ReleaseCoverOwnership((int)(this - g_Bots.m_bots));
                m_haveCover = false; m_chosenCoverTier = 0; m_chosenCoverIsLow = false;
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
        if (m_haveCover)
        {
            if (insbot_debug.GetBool())
                BotDbg("[insbots] %s COVER exit: combat ended, breaking cover to advance\n", m_name);
            m_haveCover = false;
            m_chosenCoverTier = 0;
            m_chosenCoverIsLow = false;
            g_Bots.ReleaseCoverOwnership((int)(this - g_Bots.m_bots));
        }
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

    // Doorway apex braking timer (single-shot per approach)
    if (m_doorwayRunway.active && !m_doorwayRunway.apexBraked
        && m_doorwayRunway.turnAngle > 35.0f
        && m_doorwayRunway.distToPortal > 80.0f && m_doorwayRunway.distToPortal < 160.0f)
    {
        m_doorwayApexBrakingUntil = now + 0.28f;
        m_doorwayRunway.apexBraked = true;
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
                    float lipTol = LadderLipTol();
                    bool atLip = m_ladderClimbUp ? pos.z >= endHeight - lipTol
                                                 : pos.z <= endHeight + lipTol;
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

// ---- through-wall debug overlay --------------------------------------------
// AddLineOverlay's noDepthTest=true makes lines render through geometry, giving
// the "wallhack" look. We draw the player bbox as 12 such lines, plus an aim
// line and (if engaging) a line to the target, and an entity text label.
static void DrawLineTW(const Vector &a, const Vector &b, int r, int g, int bl, float dur)
{
    debugoverlay->AddLineOverlay(a, b, r, g, bl, true /*noDepthTest*/, dur);
}

void DrawBoxTW(const Vector &mn, const Vector &mx, int r, int g, int b, float dur)
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
