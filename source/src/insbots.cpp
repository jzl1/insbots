// ============================================================================
// insbots.cpp - Valve Server Plugin entry for Insurgency: Modern Infantry
//               Combat (Source 2006 / Episode 1 engine, srcds build 4044)
//
// Loads independently of MetaMod/SourceMod via addons/insbots.vdf.
// ============================================================================
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "tier0/dbg.h"
#include "interface.h"
#include "engine/iserverplugin.h"
#include "eiface.h"
#include "edict.h"
#include "dlls/iplayerinfo.h"
#include "engine/IEngineTrace.h"
#include "engine/ivdebugoverlay.h"
#include "engine/IEngineSound.h"
#include "shake.h"                 // ShakeCommand_t, needed by IEffects.h
#include "IEffects.h"              // game_shared/, for g_pEffects
#include "convar.h"
#include "icvar.h"

#include "bot.h"
#include "nav.h"
#include "objective.h"
#include "voice.h"
#include "cfgvar.h"
#include "profile.h"
#include "server_class.h"
#include "dt_send.h"
#include "turret.h"
#include "igameevents.h"
#include "linux_offsets.h"
#include "round_freeze.h"

// Defined in turret.cpp; insbots.cpp reads it to drive the freeze window.
extern CfgVar insbot_turret_freeze_fallback;

#define INSBOTS_VERSION_NUM "0.21.1"
#ifdef BUILD_TIMESTAMP
#define INSBOTS_VERSION "0.21.1 (" BUILD_TIMESTAMP ")"
#else
#define INSBOTS_VERSION "0.21.1 (" __DATE__ " " __TIME__ ")"
#endif

// ---- engine/game interface pointers ----------------------------------------
IVEngineServer       *engine = 0;
IEngineTrace         *enginetrace = 0;
// Server-side model info. Used by the turret to resolve its two pose-parameter
// INDICES BY NAME out of the loaded .mdl rather than hardcoding them; see the
// block comment on CTurret::WritePose.
IVModelInfo          *modelinfo = 0;
IPlayerInfoManager   *playerinfomanager = 0;
IBotManager          *botmanager = 0;
IServerGameDLL       *server = 0;
IGameEventManager2   *gameevents = 0;
bool                  g_RoundFrozen = false;
// Deliberately SEPARATE from g_RoundFrozen, which means "the round is over"
// rather than "freeze time" - see round_freeze.h. Nothing about bot movement
// reads this; it exists for the turret gate alone.
bool                  g_RoundFreezeTime = false;
bool                  g_RoundFreezeBitObservable = false;
static RoundFreezeWindow s_freezeWindow;
static bool           s_bRoundFreezeEventsReady = false;
static int            s_commandClientIndex = 0;   // last human client that issued a command
int s_offset_flNextAttack = -1;
int s_offset_angEyeAngles0 = -1;
int s_offset_angEyeAngles1 = -1;
int s_offset_m_hActiveWeapon = -1;
int s_offset_m_iClip1 = -1;
int s_offset_m_CollisionGroup = -1;
int s_offset_m_hMyWeapons = -1;
// ADS ground truth (int). Bit 0 of CINSPlayer::GetPlayerFlags() is driven by
// SetIronsightsState(), so it tells us whether the sights are really up.
// -1 means "unknown" (all non-Linux builds): the change-driven pulse still
// toggles the sights, but the state-verifying retry is skipped.
int s_offset_m_IronsightsState = -1;

int FindSendPropOffset(SendTable *pTable, const char *pPropName)
{
    if (!pTable) return -1;
    for (int i = 0; i < pTable->GetNumProps(); i++)
    {
        SendProp *pProp = pTable->GetProp(i);
        if (strcmp(pProp->GetName(), pPropName) == 0)
            return pProp->GetOffset();
        
        if (pProp->GetDataTable())
        {
            int offset = FindSendPropOffset(pProp->GetDataTable(), pPropName);
            if (offset >= 0)
                return pProp->GetOffset() + offset;
        }
    }
    return -1;
}
IServerPluginHelpers *helpers = 0;
ICvar                *g_pCVar = 0;
CGlobalVars          *gpGlobals = 0;
IVDebugOverlay       *debugoverlay = 0;
extern IEngineSound  *enginesound;   // defined in voice.cpp, set in Load()

// Defined here, declared extern in turret.cpp. Optional, like enginesound: a
// missing IEffects degrades the turret to damage + sound rather than failing
// the load.
IEffects             *g_pEffects = 0;
static bool  s_bLevelActive = false;
static float s_flQuotaStartTime = 0.0f;
static float s_flNextQuotaCheck = 0.0f;
static float s_flNextQuotaAdd = 0.0f;
static float s_flNextRosterSweep = 0.0f;
static float s_flQuotaStartupReleaseTime = 0.0f;
static bool  s_bQuotaInitialBatchDone = false;
bool g_HoldQuotaJoins = false;

// ---- settings (CfgVars: plugin-local, NOT registered with the engine) -------
// These used to be ~30 ConVars, but this engine's console autocomplete crashes
// when it walks that many plugin cvars (and leaves them dangling after unload).
// They are now internal settings, read/written via the single "insbot" command:
//     insbot set <name> <value>      insbot get <name>      insbot list
//
// INS:MIC join protocol (confirmed from captured class-menu commands):
//   pteamsetup <team>; pfullsetup <faction> <role-id>; creinforce.
// Role IDs are sparse rather than 0..N.  Tokens: {n}=ordinal, {c}=loop counter,
// {s}=legacy sequential sweep, {f}=map faction (0 USMC/1 Iraqi), and
// {r}=a valid sparse role ID selected per bot.
CfgVar insbot_join_cmds_a("join_cmds_a", "pteamsetup 1;pfullsetup {f} {r};creinforce", "side-A join/spawn commands");
CfgVar insbot_join_cmds_b("join_cmds_b", "pteamsetup 2;pfullsetup {f} {r};creinforce", "side-B join/spawn commands");
CfgVar insbot_join_interval("join_interval", "1.0", "seconds between join commands (minimum 0.05)");
CfgVar insbot_slot_sweep("slot_sweep", "20", "how many squad slots {s} cycles through when joining");
#ifdef _LINUX
// The Linux server_i486.so has a different squad layout: sending declinepromo
// makes CINSSquad::PromotionResponse dereference an invalid handle and crash.
// Keep only the safe respawn command on Linux; users can still re-add it if a
// future build fixes the game-side offset.
CfgVar insbot_dead_cmds("dead_cmds", "creinforce", "commands sent every 5s while dead (declinepromo omitted on Linux: crashes the game server)");
#else
CfgVar insbot_dead_cmds("dead_cmds", "creinforce;declinepromo", "commands sent every 5s while dead");
#endif
CfgVar insbot_join_timeout("join_timeout", "40", "kick+retry a bot that can't get into a squad after this many seconds (0=never)");
CfgVar insbot_join_delay("join_delay", "5", "seconds after server activation before quota bots begin joining (0=immediate)");
CfgVar insbot_quota_start_interval("quota_start_interval", "0.20", "seconds between initial quota client creations; joins release together after the roster is complete (minimum 0.05)");
CfgVar insbot_quota_add_interval("quota_add_interval", "0.25", "seconds between quota replacement bot creations after startup");
CfgVar insbot_fov("fov", "120", "bot field of view (degrees)");
CfgVar insbot_vis_range("vis_range", "2500", "max enemy spotting distance");
CfgVar insbot_target_vehicles("target_vehicles", "2", "bot anti-vehicle policy: 0=ignore Humvees, 1=anti-armor weapons only (RPG/M203), 2=all weapons when no infantry threat is visible (default)");
CfgVar insbot_reaction("reaction", "0.6", "reaction delay before firing (s)");
CfgVar insbot_aim_error("aim_error", "4.0", "aim error in degrees (0=perfect)");
CfgVar insbot_aim_speed("aim_speed", "260", "max aim turn speed (deg/s)");
CfgVar insbot_burst_time("burst_time", "0.30", "firing burst length (s)");
CfgVar insbot_burst_pause("burst_pause", "0.45", "pause between bursts (s)");
CfgVar insbot_move_speed("move_speed", "400", "movement input magnitude");
CfgVar insbot_stop_dist("stop_dist", "1000", "stop advancing when enemy closer than this");
CfgVar insbot_capture_focus("capture_focus", "1", "capture-committed bots keep pushing the objective while engaging distant enemies (0=normal combat)");
CfgVar insbot_capture_defend_range("capture_defend_range", "1000", "enemy distance at/below which a capture-committed bot stops pushing and fights (u)");
CfgVar insbot_capture_percent("capture_percent", "15", "percent of the alive team reserved as objective capturers (set to the map's reqpercent)");
CfgVar insbot_danger_avoid("danger_avoid", "0", "A* penalty per nav area inside an active firefight for capture-committed bots (0=off)");
CfgVar insbot_danger_radius("danger_radius", "450", "radius around an actively-engaging bot treated as a danger zone for capture-committed routing (u)");
CfgVar insbot_stuck_time("stuck_time", "1.0", "no-progress seconds before body-grace or broken-link recovery starts");
CfgVar insbot_stuck_dist("stuck_dist", "16", "inches of progress toward the next waypoint that counts as moving");
CfgVar insbot_unstuck_teleport("unstuck_teleport", "1", "NavBot-style last resort: warp a hopelessly-stuck bot onto its next path node (0=off)");
CfgVar insbot_avoid_vehicles("avoid_vehicles", "1", "bots steer around driveable vehicle hulls (0=off)");
CfgVar insbot_unstuck_teleport_time("unstuck_teleport_time", "3.0", "continuous-stuck seconds before teleport-unstuck");
CfgVar insbot_doorway_teleport("doorway_teleport", "1", "warp a stalled front bot past a detected doorway after doorway_teleport_time (0=off)");
CfgVar insbot_doorway_teleport_time("doorway_teleport_time", "3.0", "continuous doorway no-progress seconds before a full-hull-validated teleport");
CfgVar insbot_doorway_sprint("doorway_sprint", "1", "accurate doorway sprint alignment and kinematic modulation (0=off)");
CfgVar insbot_stuck_nocollide("stuck_nocollide", "1", "try local player-body collision grace before stuck or doorway teleport (0=off)");
CfgVar insbot_ladder_queue("ladder_queue", "1", "single-owner queues at both ends of typed NAV ladders (0=off)");
CfgVar insbot_ladder_nocollide("ladder_nocollide", "1", "confirmed ladder owner/blocker collision grace before safe teleport (0=off)");
CfgVar insbot_ads_mode("ads_mode", "1", "ironsights input: 1=edge pulse (engine raises on the button-18 release edge), 0=legacy continuous hold");
CfgVar insbot_ladder_lip_tol("ladder_lip_tol", "4.0", "vertical tolerance (units) for latching the ladder dismount near the recorded lip; clamped to 0..6, above 6 the bot latches mid-shaft and thrashes on the 18u lost-height guard");
CfgVar insbot_spawn_nocollide("spawn_nocollide", "1", "temporarily make newly spawned bots nonblocking to other grace bots (0=off)");
CfgVar insbot_spawn_nocollide_time("spawn_nocollide_time", "5.0", "unfrozen simulation seconds before spawn collision is restored once the hull is clear");
CfgVar insbot_goal_classnames("goal_classnames", "ins_objective,obj_t1,obj_t2", "objective entity classnames");
CfgVar insbot_randomize_customization("randomize_customization", "1", "randomize faction-valid player skins/accessories on each spawn (0=off)");
CfgVar insbot_randomize_loadout("randomize_loadout", "1", "randomize faction- and role-valid primary weapon loadouts on each spawn (0=off; sends -1)");
// Objective field offsets. Linux values were re-derived from the owning game
// methods and runtime SendTables; Windows retains the live-tested defaults.
#ifdef _LINUX
CfgVar insbot_team_offset("team_offset", "864", "Linux objective owner: m_iCapturedTeam (0x360)");
#else
CfgVar insbot_team_offset("team_offset", "844", "offset of the objective ownership field (0x34c)");
#endif
CfgVar insbot_team_flip("team_flip", "0", "1 = objective owner field stores the ENEMY team (flip it). Default 0: the raw field already IS the owner (confirmed on ins_baghdad).");
#ifdef _LINUX
CfgVar insbot_active_offset("active_offset", "0", "legacy raw active offset (unused on Linux; validity uses verified parent/order/hidden fields)");
#else
CfgVar insbot_active_offset("active_offset", "948", "offset of the 'is a real objective' flag (0x3b4); 0 = spawn/non-objective");
#endif
CfgVar insbot_owner_offset("owner_offset", "864", "offset of m_iCapturedTeam (0x360); permanent capture (Push)");
CfgVar insbot_capture_offset("capture_offset", "960", "offset of m_iCaptureTeam (0x3c0)");
CfgVar insbot_hidden_offset("hidden_offset", "884", "legacy");
#ifdef _LINUX
CfgVar insbot_order_offset("order_offset", "844", "Linux CINSObjective m_iOrderID (0x34C): index within the active profile's ObjectiveData");
#else
CfgVar insbot_order_offset("order_offset", "824", "CINSObjective m_iOrderID (0x338): index within the active profile's ObjectiveData");
#endif
CfgVar insbot_push_order("push_order", "1", "on Push maps only target the next objective in the engine's capture sequence (0=old nearest-to-home behaviour)");
CfgVar insbot_crouch_chance("crouch_chance", "0.20", "chance to crouch when opening fire");
CfgVar insbot_crouch_eye_height("crouch_eye_height", "32.0", "predicted crouched firing eye height above origin (units); lower values are conservative");
CfgVar insbot_sniper_overwatch("sniper_overwatch", "1", "allow sniper-class bots to discover and occupy objective overwatch perches");
CfgVar insbot_sniper_relocate_kills("sniper_relocate_kills", "10", "confirmed kills from one perch before a sniper relocates (0=disable kill trigger)");
CfgVar insbot_sniper_perch_time_min("sniper_perch_time_min", "45", "minimum seconds a sniper holds a validated perch");
CfgVar insbot_sniper_perch_time_max("sniper_perch_time_max", "75", "maximum seconds a sniper holds a validated perch");
CfgVar insbot_ironsights("ironsights", "1", "raise ironsights while engaging beyond 300u (see ads_mode for the input mechanism)");
CfgVar insbot_quota("quota", "0", "auto-maintain this many bots (0=manual)");
CfgVar insbot_reserve("reserve", "2", "keep this many free slots open for human players");
CfgVar insbot_debug("debug", "0", "print bot debug info to console");
CfgVar insbot_draw("draw", "0", "through-wall bot debug overlays");
CfgVar insbot_draw_path("draw_path", "0", "also draw nav paths (green)");
CfgVar insbot_draw_team("draw_team", "1", "reference team for objective colors: green=friendly, grey=neutral, red=hostile");
CfgVar insbot_objectives("objectives", "1", "push toward + hold objectives (0=just roam)");
CfgVar insbot_lose_time("lose_time", "1.2", "seconds tracking an enemy after losing sight");
CfgVar insbot_teamsight("teamsight", "1", "bots share enemy sightings (radio)");
CfgVar insbot_teamwork("teamwork", "1", "spread across objectives + support teammates");
CfgVar insbot_intel_time("intel_time", "8", "seconds a shared sighting stays actionable");
CfgVar insbot_intel_range("intel_range", "3000", "max units from a shared sighting a bot will investigate (0=unlimited)");
CfgVar insbot_aggression("aggression", "0.5", "0=defend own objectives .. 1=attack enemy");
// Automatic class filling. Quotas are per faction; zero preserves the normal
// class rotation. Names that do not exist on a faction are skipped.
CfgVar insbot_class_priority("class_priority", "rifleman,rpg,support,sniper,marksman,engineer,sapper,sergeant,militant,grenadier", "comma-separated automatic class fill order");
CfgVar insbot_class_quota_rpg("class_quota_rpg", "0", "Iraqi RPG-7 bots to fill per faction (0=normal rotation)");
CfgVar insbot_class_quota_grenadier("class_quota_grenadier", "0", "USMC grenadier bots to fill per faction (0=normal rotation)");
CfgVar insbot_class_quota_sniper("class_quota_sniper", "0", "Iraqi sniper bots to fill per faction (0=normal rotation)");
CfgVar insbot_class_quota_marksman("class_quota_marksman", "0", "USMC marksman bots to fill per faction (0=normal rotation)");
CfgVar insbot_class_quota_support("class_quota_support", "0", "support bots to fill per faction (0=normal rotation)");
CfgVar insbot_class_quota_engineer("class_quota_engineer", "0", "USMC engineer bots to fill per faction (0=normal rotation)");
CfgVar insbot_class_quota_sapper("class_quota_sapper", "0", "Iraqi sapper bots to fill per faction (0=normal rotation)");
CfgVar insbot_class_quota_rifleman("class_quota_rifleman", "0", "rifleman bots to fill per faction (0=normal rotation)");
CfgVar insbot_class_quota_sergeant("class_quota_sergeant", "0", "USMC sergeant bots to fill per faction (0=normal rotation)");
CfgVar insbot_class_quota_militant("class_quota_militant", "0", "Iraqi militant bots to fill per faction (0=normal rotation)");
CfgVar insbot_reload("reload", "1", "auto-reload after fire / during lulls");
CfgVar insbot_mag_fire_time("mag_fire_time", "3.0", "reload-timing scale: 3.0 = reload after a real magazine (per-weapon mag size x fire rate); lower = reload sooner");
CfgVar insbot_primary_mags("primary_mags", "7", "legacy setting; use rifle_reserve_mags, sniper_reserve_mags, shotgun_reserve_mags, pistol_reserve_mags, or rpg_reserve_mags");
CfgVar insbot_firemode("firemode", "auto", "fire-mode policy: 'auto' (fullest mode), 'range', 'burst', or 'semi'");
CfgVar insbot_recoil_control("recoil_control", "realistic", "recoil compensation: 'realistic' (imperfect - high-recoil guns spray), 'perfect' (laser), or 'aimerror' (scale off aim_error)");
CfgVar insbot_firemode_range("firemode_range", "700", "enemy distance under which range-policy uses full-auto/burst; beyond it, semi");
CfgVar insbot_sidearm_a("sidearm_a", "weapon_m9", "fallback side-A pistol (used only if the map's .imc2 faction is unknown)");
CfgVar insbot_sidearm_b("sidearm_b", "weapon_makarov", "fallback side-B pistol (used only if the map's .imc2 faction is unknown)");
CfgVar insbot_sidearm_usmc("sidearm_usmc", "weapon_m9", "USMC pistol classname (assigned by faction, read per-map from the .imc2)");
CfgVar insbot_sidearm_iraqi("sidearm_iraqi", "weapon_makarov", "Iraqi pistol classname (assigned by faction, read per-map from the .imc2)");
CfgVar insbot_safe_reload("safe_reload", "1", "only reload when safe: dry mag reloads when the enemy isn't close, and tops off in a lull");
CfgVar insbot_close_range("close_range", "600", "enemy distance considered 'close' - dry primary here means switch to sidearm, not reload");
CfgVar insbot_takecover("takecover", "1", "retreat to a spot that breaks line-of-sight while reloading or hurt");// ---- take-cover overhaul v2 (see take-cover-overhaul-design.md) ----
CfgVar insbot_cover_v2("cover_v2", "1", "use precomputed cover database + multi-threat scoring (0=legacy FindCover path)");
CfgVar insbot_cover_on_engage("cover_on_engage", "1", "seek nearby cover when engaging in the open (0=off)");
CfgVar insbot_suppress_time("suppress_time", "4", "seconds of reactive cover-seeking after being hit");
CfgVar insbot_cover_tier_weight("cover_tier_weight", "30", "scoring weight per full tier step");
CfgVar insbot_cover_dist_weight("cover_dist_weight", "0.25", "scoring penalty per unit distance from bot");
CfgVar insbot_cover_advance_penalty("cover_advance_penalty", "80", "penalty if candidate moves bot toward threat");
CfgVar insbot_cover_min_score("cover_min_score", "-20", "minimum aggregated score before falling back to legacy logic");
CfgVar insbot_cover_search_radius("cover_search_radius", "400", "candidate query radius around bot position");
CfgVar insbot_cover_max_reach("cover_max_reach", "500", "max acceptable travel distance toward chosen cover spot");
CfgVar insbot_cover_ray_len("cover_ray_len", "100", "load-time raycast length for directional occlusion tests");
CfgVar insbot_cover_ray_cutoff("cover_ray_cutoff", "90", "load-time block-distance cutoff per direction");CfgVar insbot_cover_formation_bonus("cover_formation_bonus", "25", "scoring bonus for cover points closer to squad leader");
CfgVar insbot_cover_health("cover_health", "40", "health at/below which a threatened bot breaks contact and seeks cover");
CfgVar insbot_learn_persist("learn_persist", "1", "save learned bad nav links to addons/insbots_learn_<map>.txt and restore them on map load");
CfgVar insbot_sprint("sprint", "1", "hold sprint (IN_ALT2) while moving with no enemy contact; game stamina rules apply");
CfgVar insbot_avoid_jump("avoid_jump", "4000", "A* penalty for nav areas flagged JUMP - bots route around jumps when a flat path exists (0=don't avoid)");
CfgVar insbot_avoid_crouch("avoid_crouch", "2500", "A* penalty for nav areas flagged CROUCH - bots route around crouch passages when possible (0=don't avoid)");
CfgVar insbot_avoid_marked("avoid_marked", "3000", "A* penalty for nav areas flagged AVOID (jump/crouch areas are marked AVOID by the generator) - 0=don't avoid");
CfgVar insbot_avoid_ladder("avoid_ladder", "3000", "A* penalty for ladder edges - bots route around ladders when a flat path exists (0=don't avoid)");
CfgVar insbot_avoid_narrow("avoid_narrow", "50", "A* penalty unit for narrow walk portals: x4 below 32u, x2 for 32-55u, x1 for 56-99u (0=don't avoid)");
CfgVar insbot_ladder_up_penalty("ladder_up_penalty", "500", "extra A* cost for climbing UP a ladder; descending is the easy direction");
CfgVar insbot_grenades("grenades", "0", "OPT-IN: throw grenades at range");
CfgVar insbot_grenade_a("grenade_a", "weapon_m67", "side-A grenade classname");
CfgVar insbot_grenade_b("grenade_b", "weapon_rgd5", "fallback side-B grenade (used only if the map's .imc2 faction is unknown)");
CfgVar insbot_grenade_usmc("grenade_usmc", "weapon_m67", "USMC grenade classname (assigned by faction, read per-map from the .imc2)");
CfgVar insbot_grenade_iraqi("grenade_iraqi", "weapon_rgd5", "Iraqi grenade classname (assigned by faction, read per-map from the .imc2)");
CfgVar insbot_grenade_cooldown("grenade_cooldown", "30", "min seconds between grenade throws");
CfgVar insbot_grenade_min_range("grenade_min_range", "350", "only grenade enemies farther than this");
CfgVar insbot_grenade_chance("grenade_chance", "0.25", "chance to grenade (vs keep shooting) when the chance arises; low = prefer gunfights");
CfgVar insbot_grenade_speed("grenade_speed", "1000", "grenade throw speed (units/s) used to compute the arc");
CfgVar insbot_grenade_gravity("grenade_gravity", "600", "gravity used for the grenade arc (match sv_gravity)");

// smoke grenade onto the objective (only if the bot actually has a smoke)
CfgVar insbot_smoke("smoke", "1", "bots throw a smoke onto a contested objective to cover the push (only if they carry one)");
CfgVar insbot_smoke_weapon("smoke_weapon", "weapon_m18", "smoke grenade classname (INS:MIC M18, shared by both factions)");
CfgVar insbot_smoke_chance("smoke_chance", "0.6", "chance to smoke a contested objective when the chance arises (0..1)");
CfgVar insbot_smoke_cooldown("smoke_cooldown", "35", "min seconds between a bot's smoke throws");
CfgVar insbot_smoke_range("smoke_range", "1300", "only smoke the objective when within this distance of it");
CfgVar insbot_smoke_group("smoke_group", "1", "throw a smoke at a group of enemies");
CfgVar insbot_smoke_group_min("smoke_group_min", "3", "minimum number of clustered enemies to qualify as a group");
CfgVar insbot_smoke_group_radius("smoke_group_radius", "400", "max radius between enemies to be considered the same group");
CfgVar insbot_smoke_group_chance("smoke_group_chance", "0.4", "chance to throw a smoke when a group is spotted (0..1)");
CfgVar insbot_smoke_scatter("smoke_scatter", "0.50", "fraction of objective radius to scatter smoke grenade throws (0=exact center)");
CfgVar insbot_m203("m203", "1", "allow M16A4/M203 grenadier bots to use 40mm grenade launcher in combat (0=off)");
CfgVar insbot_m203_chance("m203_chance", "0.5", "chance for a grenadier bot to fire an M203 grenade on a valid target (0..1)");
CfgVar insbot_m203_speed("m203_speed", "1570", "40mm grenade launch speed (units/s) used to compute the M203 ballistic arc");
CfgVar insbot_m203_min_range("m203_min_range", "500", "minimum safe distance (Source units) to fire an M203 40mm grenade");
CfgVar insbot_m203_max_range("m203_max_range", "3500", "maximum distance (Source units) to fire an M203 40mm grenade");
CfgVar insbot_m203_cooldown("m203_cooldown", "15", "min seconds between a bot's M203 grenade shots");
CfgVar insbot_rpg_cooldown("rpg_cooldown", "3.0", "min seconds between a bot's RPG-7 rocket shots");
CfgVar insbot_m203_obj("m203_obj", "1", "allow grenadier bots to pre-clear contested objectives with M203 grenades (0=off)");
CfgVar insbot_m203_obj_chance("m203_obj_chance", "0.4", "chance to shell a contested objective when in range (0..1)");

// patrol within an objective while holding it (instead of standing on one spot)
CfgVar insbot_patrol("patrol", "1", "a held objective's defenders may wander inside it instead of standing still");
CfgVar insbot_patrol_chance("patrol_chance", "0.5", "chance a holding bot patrols (vs holds one spot), rolled per arrival (0..1)");
CfgVar insbot_patrol_radius("patrol_radius", "0.85", "patrol spot max distance from the point centre, as a fraction of its radius (stays inside)");
CfgVar insbot_patrol_interval("patrol_interval", "7", "base seconds a patrolling bot waits before wandering to a new spot in the point");

CfgVar insbot_freeze("freeze", "0", "freeze all bot movement and actions (1=frozen, overridden by force pathfinding)");
CfgVar insbot_nav("nav", "1", "enable/disable nav mesh pathfinding system (0=turn off nav system, overridden by force pathfinding)");
CfgVar insbot_streaming("streaming", "2", "corridor queues (0=off; 1=all narrow NAV runs; 2=automatic at full-hull-verified physical doorways/corridors)");
CfgVar insbot_profile("profile", "0", "built-in per-frame profiler: measures BuildPath/GetPathCost/FindArea, traces, hearing, teamsight and prints a summary every 5 s (0=off)");
CfgVar insbot_attack("attack", "1", "enable/disable bot shooting and attacks (0=hold fire)");

// hearing / footstep investigation
CfgVar insbot_hear("hear", "1", "bots hear enemy movement (footsteps) and investigate when not engaging");
CfgVar insbot_hear_range("hear_range", "1000", "max units a moving enemy's footsteps are audible");
CfgVar insbot_hear_speed("hear_speed", "120", "min horizontal speed (units/s) for movement to count as footsteps");
CfgVar insbot_hear_timeout("hear_timeout", "6", "seconds a bot keeps investigating a heard footstep origin");
CfgVar insbot_hear_cooldown("hear_cooldown", "4", "min seconds between hearing investigations");
CfgVar insbot_detect("detect", "1", "master enemy detection toggle (vision + hearing); 0 = bots cannot detect enemies");

// ---- cvar registration glue (ep1 style) -------------------------------------
class CPluginConVarAccessor : public IConCommandBaseAccessor
{
public:
    virtual bool RegisterConCommandBase(ConCommandBase *pCommand)
    {
        pCommand->AddFlags(FCVAR_PLUGIN);
        pCommand->SetNext(0);
        g_pCVar->RegisterConCommandBase(pCommand);
        return true;
    }
};
static CPluginConVarAccessor g_ConVarAccessor;

// ---- human/slot accounting --------------------------------------------------
static int CountHumans()
{
    int h = 0;
    for (int i = 1; i <= gpGlobals->maxClients; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (!e || e->IsFree()) continue;
        IPlayerInfo *pi = playerinfomanager->GetPlayerInfo(e);
        if (pi && pi->IsConnected() && !pi->IsFakeClient()) h++;
    }
    return h;
}

// max bots allowed right now, leaving 'reserve' slots free for humans
static int BotCap()
{
    int cap = gpGlobals->maxClients - insbot_reserve.GetInt() - CountHumans();
    return cap < 0 ? 0 : cap;
}

// ---- the single "insbot" console command ------------------------------------
static void Cmd_InsBot_Add(int n)
{
    if (n < 1) n = 1;
    if (n > 32) n = 32;
    int cap = BotCap();
    int ok = 0, capped = 0;
    for (int i = 0; i < n; i++)
    {
        if (g_Bots.Count() >= cap) { capped = 1; break; }
        if (g_Bots.AddBot(0)) ok++;
    }
    if (capped)
        Msg("[insbots] added %d bot(s), %d total (slot cap reached - %d reserved for players; raise maxplayers or lower 'reserve')\n",
            ok, g_Bots.Count(), insbot_reserve.GetInt());
    else
        Msg("[insbots] added %d bot(s), %d total\n", ok, g_Bots.Count());

    char buf[32];
    _snprintf(buf, sizeof(buf), "%d", g_Bots.Count());
    insbot_quota.SetValue(buf);
}

// Parse the compact class-slot notation from class mapping.txt:
//   u2a4 = USMC, squad 2, column A, slot 4
//   i1b3 = Iraqi, cell 1, column B, slot 3
// Both factions use the same sparse role IDs in pfullsetup.
static bool ParseTargetRole(const char *arg, int &faction, int &roleId)
{
    if (!arg || !arg[0] || !arg[1] || !arg[2] || !arg[3] || arg[4]) return false;

    char fac = arg[0];
    if (fac >= 'A' && fac <= 'Z') fac = (char)(fac - 'A' + 'a');
    char column = arg[2];
    if (column >= 'A' && column <= 'Z') column = (char)(column - 'A' + 'a');

    if (fac != 'u' && fac != 'i') return false;
    if (arg[1] != '1' && arg[1] != '2') return false;
    if (column != 'a' && column != 'b') return false;
    if (arg[3] < '1' || arg[3] > '4') return false;

    faction = (fac == 'i') ? 1 : 0;
    int groupOffset = (arg[1] == '2') ? 1 : 0;
    int columnOffset = (column == 'b') ? 16 : 0;
    int rowOffset = (arg[3] - '1') * 4;
    roleId = 5 + groupOffset + columnOffset + rowOffset;
    return true;
}

static bool IsPureNumber(const char *arg)
{
    if (!arg || !arg[0]) return false;
    for (const char *p = arg; *p; p++)
        if (*p < '0' || *p > '9') return false;
    return true;
}

static void Cmd_InsBot_AddArg(const char *arg)
{
    if (!arg || !arg[0])
    {
        Cmd_InsBot_Add(1);
        return;
    }

    if (IsPureNumber(arg))
    {
        Cmd_InsBot_Add(atoi(arg));
        return;
    }

    int faction = -1, roleId = -1;
    if (!ParseTargetRole(arg, faction, roleId))
    {
        Msg("[insbots] invalid add target '%s' (use a bot count or slot code such as u2a4 / i1b3)\n", arg);
        return;
    }

    if (g_Bots.Count() >= BotCap())
    {
        Msg("[insbots] could not add %s (slot cap reached - %d reserved for players)\n",
            arg, insbot_reserve.GetInt());
        return;
    }

    CInsBot *bot = g_Bots.AddBotToRole(faction, roleId);
    if (!bot)
    {
        Msg("[insbots] could not add bot for %s\n", arg);
        return;
    }

    Msg("[insbots] added %s bot to %s (pfullsetup %d %d), %d total\n",
        bot->m_name, arg, bot->m_requestedTeamArg, roleId, g_Bots.Count());

    char buf[32];
    _snprintf(buf, sizeof(buf), "%d", g_Bots.Count());
    insbot_quota.SetValue(buf);
}

static void Cmd_InsBot_Status()
{
    Msg("[insbots] v%s | bots: %d | nav: %s (%d areas) | objectives: %d (home1 %s, home2 %s)\n",
        INSBOTS_VERSION, g_Bots.Count(),
        g_NavMesh.IsLoaded() ? "loaded" : "NOT loaded",
        g_NavMesh.Count(), g_Objectives.Count(),
        g_Objectives.m_homeSamples[1] ? "learned" : "-",
        g_Objectives.m_homeSamples[2] ? "learned" : "-");
    int atOrigin = 0;
    for (int i = 0; i < 64; i++)
    {
        CInsBot &b = g_Bots.m_bots[i];
        if (!b.m_inUse) continue;
        int team = b.m_info ? b.m_info->GetTeamIndex() : -1;
        bool dead = b.m_info ? b.m_info->IsDead() : true;
        Vector p = b.m_info ? b.m_info->GetAbsOrigin() : Vector(0, 0, 0);
        bool origin = (p.x > -48 && p.x < 48 && p.y > -48 && p.y < 48);
        if (origin) atOrigin++;

        const char *wpName = b.GetWeaponName();
        float accum = b.GetFireAccum();
        int mag = b.GetMagSize();
        int clipLeft = b.GetClipRemaining();
        int fired = mag - clipLeft;

        Msg("  %-16s team=%d %-6s weapon=%-14s mag_ammo=%d/%d (fired=%d, accum=%.2fs)%s\n",
            b.m_name, team, dead ? "dead" : "alive",
            wpName, clipLeft, mag, fired, accum,
            origin ? "  <-- at origin" : "");
    }
    if (atOrigin > 0)
        Msg("[insbots] %d bot(s) stuck at origin (no squad slot). Raise 'slot_sweep' or fix join_cmds.\n", atOrigin);
}

static void Cmd_InsBot_NavReload()
{
    // Preserve links learned since the last level load before replacing the
    // mesh. The successful load path below restores this file.
    InsBots_SaveLearn();
    char gamedir[256];
    engine->GetGameDir(gamedir, sizeof(gamedir));
    char path[512];
    _snprintf(path, sizeof(path), "%s/addons/insbots_nav/%s.nav", gamedir, g_Bots.m_mapname);
    bool ok = g_NavMesh.Load(path);
    if (!ok)
    {
        _snprintf(path, sizeof(path), "%s/maps/%s.nav", gamedir, g_Bots.m_mapname);
        ok = g_NavMesh.Load(path);
        if (ok)
        {
            Msg("[insbots] WARNING: Loaded nav from maps folder (%s). Move it to insurgency/addons/insbots_nav/ to prevent clients from downloading it!\n", path);
        }
    }
    Msg("[insbots] nav reload '%s': %s (%d areas)\n", path, ok ? "OK" : "FAILED", g_NavMesh.Count());
    g_Bots.InvalidateNavMeshState(ok);
}

// ---- take-cover overhaul v2 debug subcommands ------------------------------
static void Cmd_CoverDump(float radius)
{
    extern CBotRoster g_Bots;
    if (!g_Bots.CoverCount())
    {
        Msg("[insbots] cover database is empty (nav missing or cover_v2=0)\n");
        return;
    }
    // reference position: first connected human else origin
    Vector ref(0, 0, 0);
    bool found = false;
    for (int i = 1; i <= gpGlobals->maxClients && !found; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (!e || e->IsFree()) continue;
        IPlayerInfo *pi = playerinfomanager ? playerinfomanager->GetPlayerInfo(e) : 0;
        if (!pi || !pi->IsConnected() || pi->IsFakeClient()) continue;
        ref = pi->GetAbsOrigin();
        found = true;
    }
    int shown = 0;
    float rSq = radius * radius;
    for (int i = 0; i < g_Bots.CoverCount(); i++)
    {
        const CoverPoint *cp = g_Bots.CoverAt(i);
        if (!cp) continue;
        Vector d = cp->pos - ref;
        if (d.x*d.x + d.y*d.y + d.z*d.z > rSq) continue;
        Msg("[cover] idx=%d pos=(%.0f %.0f %.0f) tier=%d low=%u mask=",
            i, cp->pos.x, cp->pos.y, cp->pos.z, cp->tier, cp->isLowOnly);
        for (int d = 0; d < COVER_SECTORS; d++) Msg("%u", cp->blockMask[d]);
        Msg("\n");
        shown++;
    }
    Msg("[insbots] %d cover points within %.0fu of reference (total DB=%d)\n",
        shown, radius, g_Bots.CoverCount());
}

static void Cmd_CoverEval()
{
    extern CBotRoster g_Bots;
    if (!g_Bots.CoverCount()) { Msg("[insbots] cover db empty\n"); return; }
    // use first connected human as observer + aim direction
    for (int i = 1; i <= gpGlobals->maxClients; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (!e || e->IsFree()) continue;
        IPlayerInfo *pi = playerinfomanager ? playerinfomanager->GetPlayerInfo(e) : 0;
        if (!pi || !pi->IsConnected() || pi->IsFakeClient()) continue;
        Vector myPos = pi->GetAbsOrigin();
        QAngle ang = pi->GetLastUserCommand().viewangles;
        float yr = ang.y * (3.14159265358979f / 180.0f);
        Vector fakeThreat = myPos + Vector(cosf(yr), sinf(yr), 0) * 800.0f;
        float best = -1e30f; int bestIdx = -1;
        for (int k = 0; k < g_Bots.CoverCount(); k++)
        {
            const CoverPoint *cp = g_Bots.CoverAt(k);
            if (!cp) continue;
            // Was ((int)(yawDeg / 45.0f)) & 7, which is NOT centre-aligned and
            // disagreed with every other consumer by up to one sector. Use the
            // shared binning so the diagnostic reports what the bots act on.
            int sec = CoverSectorFor(cp->pos, fakeThreat);
            float s = 0.0f;
            if (cp->blockMask[sec]) s += 100.0f;
            else if (cp->crouchMask[sec]) s += 55.0f;
            else s -= 60.0f;
            s -= insbot_cover_dist_weight.GetFloat()
               * sqrtf((myPos-cp->pos).LengthSqr());
            s += insbot_cover_tier_weight.GetFloat() * cp->tier;
            if (s > best) { best = s; bestIdx = k; }
        }
        if (bestIdx >= 0)
        {
            const CoverPoint *bp = g_Bots.CoverAt(bestIdx);
            Msg("[insbots] best vs aimed threat: idx=%d tier=%d score=%.1f dist=%.0f\n",
                bestIdx, bp->tier, best,
                (bp->pos - myPos).Length());
        }
        return;
    }
    Msg("[insbots] no human connected - cannot eval\n");
}

static void Cmd_CoverOcc()
{
    extern CBotRoster g_Bots;
    if (!g_Bots.m_coverOwnerSlot) { Msg("[insbots] occupancy table empty\n"); return; }
    int n = 0;
    for (int i = 0; i < g_Bots.m_coverCapacity && n < 64; i++)
    {
        short slot = g_Bots.m_coverOwnerSlot[i];
        if (slot < 0) continue;
        CInsBot *b = (slot >= 0 && slot < 64) ? &g_Bots.m_bots[slot] : 0;
        if (!b || !b->m_inUse) continue;
        Msg("[coverocc] pt=%d owner='%s' since=%.1fs\n", i, b->m_name,
            gpGlobals ? gpGlobals->curtime - g_Bots.m_coverOwnedSince[i] : 0.f);
        n++;
    }
    Msg("[insbots] %d owned cover points\n", n);
}extern void InsBot_DebugAmmo();
static void Cmd_InsBot_List()
{
    Msg("[insbots] settings (use: insbot set <name> <value>):\n");
    for (CfgVar *c = CfgVar::s_head; c; c = c->m_next)
        Msg("  %-18s = %-28s  %s\n", c->Name(), c->GetString(), c->Help());
}

static void Cmd_InsBot()
{
    int argc = engine->Cmd_Argc();
    const char *sub = (argc > 1) ? engine->Cmd_Argv(1) : "";

    if (!sub[0] || !_stricmp(sub, "help"))
    {
        Msg("[insbots] commands:\n"
            "  insbot add [n]        add n bots to automatically selected slots (default 1)\n"
            "  insbot add <slot>     add to an exact slot, e.g. u2a4 or i1b3\n"
            "  insbot kick           kick one bot\n"
            "  insbot kickall        kick all bots\n"
            "  insbot status         bot / nav / objective status\n"
            "  insbot navreload      reload the current map's nav mesh\n"
            "  insbot forget         wipe the learned bad-link memory for this map\n"
            "  insbot objdebug       dump objective ownership fields (for tuning)\n"
            "  insbot list           list all settings and values\n"
            "  insbot set freeze 1   freeze all bots (default 0)\n"
            "  insbot set nav 0      toggle nav system (default 1)\n"
            "  insbot set attack 0   toggle shooting / attacks (default 1)\n"
            "  insbot goto           force bots to go where mouse is pointing (overrides freeze & nav)\n"
            "  insbot stop / clear   release forced pathfinding override\n"
            "  insbot goto <x y z>   force bots to coordinate\n");
        return;
    }
    if (!_stricmp(sub, "add"))      { Cmd_InsBot_AddArg(argc > 2 ? engine->Cmd_Argv(2) : 0); return; }
    if (!_stricmp(sub, "kick"))
    {
        bool k = g_Bots.KickOne();
        Msg(k ? "[insbots] kicked one bot, %d left\n" : "[insbots] no bots to kick\n", g_Bots.Count());
        char buf[32]; _snprintf(buf, sizeof(buf), "%d", g_Bots.Count()); insbot_quota.SetValue(buf);
        return;
    }
    if (!_stricmp(sub, "kickall"))
    {
        g_Bots.KickAll();
        g_NavMesh.Free();
        Msg("[insbots] kicked all bots\n");
        insbot_quota.SetValue("0");
        return;
    }
    if (!_stricmp(sub, "status"))   { Cmd_InsBot_Status(); return; }
    if (!_stricmp(sub, "navreload")){ Cmd_InsBot_NavReload(); return; }
    if (!_stricmp(sub, "forget"))   { InsBots_ForgetLearn(); Msg("[insbots] learned bad-link memory wiped for this map\n"); return; }
    if (!_stricmp(sub, "objdebug")) { g_Objectives.DebugDump(); return; }if (!_stricmp(sub, "coverdump")) { Cmd_CoverDump(argc > 2 ? (float)atof(engine->Cmd_Argv(2)) : 300.0f); return; }
if (!_stricmp(sub, "covereval")) { Cmd_CoverEval(); return; }
if (!_stricmp(sub, "coverocc")) { Cmd_CoverOcc(); return; }
    if (!_stricmp(sub, "list"))     { Cmd_InsBot_List(); return; }
    if (!_stricmp(sub, "debugammo")) { InsBot_DebugAmmo(); return; }
    if (!_stricmp(sub, "version"))  { Msg("[insbots] version %s\n", INSBOTS_VERSION); return; }
    if (!_stricmp(sub, "version_raw")) { Msg("%s\n", INSBOTS_VERSION); return; }
    if (!_stricmp(sub, "freeze"))
    {
        bool b = insbot_freeze.GetBool();
        insbot_freeze.SetValue(b ? "0" : "1");
        Msg("[insbots] Bots %s!\n", insbot_freeze.GetBool() ? "FROZEN" : "UNFROZEN");
        return;
    }
    if (!_stricmp(sub, "nav") || !_stricmp(sub, "navoff") || !_stricmp(sub, "navon") || !_stricmp(sub, "nonav"))
    {
        if (!_stricmp(sub, "nav"))
        {
            bool b = insbot_nav.GetBool();
            insbot_nav.SetValue(b ? "0" : "1");
        }
        else if (!_stricmp(sub, "navon"))
            insbot_nav.SetValue("1");
        else
            insbot_nav.SetValue("0");
        Msg("[insbots] Navigation system %s.\n", insbot_nav.GetBool() ? "ENABLED" : "DISABLED (0)");
        return;
    }
    if (!_stricmp(sub, "attack") || !_stricmp(sub, "shoot") || !_stricmp(sub, "dontshoot") || !_stricmp(sub, "holdfire") || !_stricmp(sub, "fire"))
    {
        bool b = insbot_attack.GetBool();
        insbot_attack.SetValue(b ? "0" : "1");
        Msg("[insbots] Bot attacks/shooting %s!\n", insbot_attack.GetBool() ? "ENABLED (1)" : "DISABLED / Hold Fire (0)");
        return;
    }
    if (!_stricmp(sub, "squad") || !_stricmp(sub, "squad_cmd"))
    {
        if (argc < 4)
        {
            Msg("[insbots] usage: insbot squad <leader_client_id> <order_id> (1=follow, 2=hold, 3=backup, 4=suppress, 5=moveto, 0=dismiss)\n");
            return;
        }
        int leaderIndex = atoi(engine->Cmd_Argv(2));
        int order = atoi(engine->Cmd_Argv(3));
        float wpX = (argc >= 7) ? (float)atof(engine->Cmd_Argv(4)) : 0.0f;
        float wpY = (argc >= 7) ? (float)atof(engine->Cmd_Argv(5)) : 0.0f;
        float wpZ = (argc >= 7) ? (float)atof(engine->Cmd_Argv(6)) : 0.0f;
        g_Bots.DispatchSquadOrder(leaderIndex, order, wpX, wpY, wpZ);
        Msg("[insbots] Dispatched squad order %d for leader index %d\n", order, leaderIndex);
        return;
    }
    if (!_stricmp(sub, "goto"))
    {
        edict_t *issuer = 0;
        if (s_commandClientIndex >= 1 && s_commandClientIndex <= gpGlobals->maxClients)
        {
            edict_t *ce = engine->PEntityOfEntIndex(s_commandClientIndex);
            if (ce && !ce->IsFree())
            {
                IPlayerInfo *pi = playerinfomanager ? playerinfomanager->GetPlayerInfo(ce) : 0;
                if (pi && pi->IsConnected() && !pi->IsFakeClient())
                    issuer = ce;
            }
        }
        InsBots_SetGotoCmd(issuer, argc - 2,
                           argc > 2 ? engine->Cmd_Argv(2) : 0,
                           argc > 3 ? engine->Cmd_Argv(3) : 0,
                           argc > 4 ? engine->Cmd_Argv(4) : 0);
        return;
    }
    if (!_stricmp(sub, "stop") || !_stricmp(sub, "clear") || !_stricmp(sub, "release"))
    {
        g_ForceGotoActive = false;
        Msg("[insbots] Released forced goto pathfinding override.\n");
        return;
    }
    if (!_stricmp(sub, "get"))
    {
        CfgVar *c = (argc > 2) ? CfgFind(engine->Cmd_Argv(2)) : 0;
        if (c) Msg("[insbots] %s = %s  (%s)\n", c->Name(), c->GetString(), c->Help());
        else   Msg("[insbots] unknown setting '%s' (try: insbot list)\n", argc > 2 ? engine->Cmd_Argv(2) : "");
        return;
    }
    if (!_stricmp(sub, "set"))
    {
        if (argc < 4) { Msg("[insbots] usage: insbot set <name> <value>\n"); return; }
        CfgVar *c = CfgFind(engine->Cmd_Argv(2));
        if (!c) { Msg("[insbots] unknown setting '%s' (try: insbot list)\n", engine->Cmd_Argv(2)); return; }
        // join everything after the name as the value (so quoted/spaced strings work)
        char valueBuf[256];
        int vlen = 0;
        for (int ai = 3; ai < argc && vlen < (int)sizeof(valueBuf) - 1; ai++)
        {
            const char *tok = engine->Cmd_Argv(ai);
            if (!tok) continue;
            size_t tlen = strlen(tok);
            if (ai > 3 && vlen + 1 < (int)sizeof(valueBuf))
                valueBuf[vlen++] = ' ';
            size_t room = sizeof(valueBuf) - 1 - vlen;
            if (tlen > room) tlen = room;
            memcpy(valueBuf + vlen, tok, tlen);
            vlen += (int)tlen;
        }
        valueBuf[vlen] = 0;
        c->SetValue(valueBuf);
        Msg("[insbots] %s = %s\n", c->Name(), c->GetString());
        return;
    }
    if (!_stricmp(sub, "reserve"))
    {
        if (argc < 4) { Msg("[insbots] usage: insbot reserve <teamSlot> <slotCode> [userId]\n"); return; }
        int teamSlot = atoi(engine->Cmd_Argv(2));
        int slotCode = atoi(engine->Cmd_Argv(3));
        int userId = (argc > 4) ? atoi(engine->Cmd_Argv(4)) : 0;
        if (teamSlot < 0 || teamSlot > 1) { Msg("[insbots] reserve: teamSlot must be 0 or 1\n"); return; }
        if (slotCode < 5 || slotCode > 34) { Msg("[insbots] reserve: slotCode out of range\n"); return; }
        g_Bots.ReserveRole(teamSlot, slotCode, userId);
        Msg("[insbots] Reserved teamSlot %d slotCode %d for user %d\n", teamSlot, slotCode, userId);
        return;
    }
    if (!_stricmp(sub, "unreserve"))
    {
        if (argc < 4) { Msg("[insbots] usage: insbot unreserve <teamSlot> <slotCode>\n"); return; }
        int teamSlot = atoi(engine->Cmd_Argv(2));
        int slotCode = atoi(engine->Cmd_Argv(3));
        g_Bots.UnreserveRole(teamSlot, slotCode);
        Msg("[insbots] Unreserved teamSlot %d slotCode %d\n", teamSlot, slotCode);
        return;
    }
    if (!_stricmp(sub, "unreserve_user"))
    {
        if (argc < 3) { Msg("[insbots] usage: insbot unreserve_user <userId>\n"); return; }
        int userId = atoi(engine->Cmd_Argv(2));
        g_Bots.UnreserveUser(userId);
        Msg("[insbots] Cleared reservations for user %d\n", userId);
        return;
    }
    if (!_stricmp(sub, "unreserve_all"))
    {
        g_Bots.ClearAllReservations();
        Msg("[insbots] Cleared all role reservations\n");
        return;
    }
    if (!_stricmp(sub, "turret"))
    {
        TurretHandleCommand(argc);
        return;
    }
    Msg("[insbots] unknown subcommand '%s' (try: insbot help)\n", sub);
}
static ConCommand insbot_cmd("insbot", Cmd_InsBot, "InsBots control: insbot help");

// ---- the plugin --------------------------------------------------------------
class CInsBotsPlugin : public IServerPluginCallbacks, public IGameEventListener2
{
public:
    CInsBotsPlugin() : m_iClientCommandIndex(0) {}

    virtual bool Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn gameServerFactory)
    {
        engine = (IVEngineServer *)interfaceFactory(INTERFACEVERSION_VENGINESERVER, 0);
        enginetrace = (IEngineTrace *)interfaceFactory(INTERFACEVERSION_ENGINETRACE_SERVER, 0);
        helpers = (IServerPluginHelpers *)interfaceFactory(INTERFACEVERSION_ISERVERPLUGINHELPERS, 0);
        g_pCVar = (ICvar *)interfaceFactory(VENGINE_CVAR_INTERFACE_VERSION, 0);
        debugoverlay = (IVDebugOverlay *)interfaceFactory(VDEBUG_OVERLAY_INTERFACE_VERSION, 0); // optional
        enginesound = (IEngineSound *)interfaceFactory(IENGINESOUND_SERVER_INTERFACE_VERSION, 0); // optional (bot voice)
        // Optional: without it the turret cannot resolve its pose indices and
        // degrades to "gun does not move" rather than writing a wrong slot.
        modelinfo = (IVModelInfo *)interfaceFactory(VMODELINFO_SERVER_INTERFACE_VERSION, 0);
        // Optional, like enginesound: a missing IEffects degrades the turret to
        // damage + sound only rather than failing the load.
        //
        // IEffects is a GAME-DLL interface, not an engine one: it is exposed by
        // dlls/EffectsServer.cpp (CEffectsServer), and engine.dll contains no
        // reference to "IEffects001" at all. Asking the engine factory for it
        // therefore always returns NULL, which silently disabled every effect
        // call (muzzle flash, tracer, impact) through the `if (g_pEffects)`
        // guards - the gun moved and made no visible flash.
        //
        // Ask the game factory first, then fall back to the engine factory in
        // case a different build exposes it there.
        g_pEffects = (IEffects *)gameServerFactory(IEFFECTS_INTERFACE_VERSION, 0);
        if (!g_pEffects)
            g_pEffects = (IEffects *)interfaceFactory(IEFFECTS_INTERFACE_VERSION, 0);
        Msg("[insbots] IEffects: %p%s\n", g_pEffects,
            g_pEffects ? "" : "  (absent - no muzzle flash/tracer/impact)");
        playerinfomanager = (IPlayerInfoManager *)gameServerFactory(INTERFACEVERSION_PLAYERINFOMANAGER, 0);
        botmanager = (IBotManager *)gameServerFactory(INTERFACEVERSION_PLAYERBOTMANAGER, 0);
        server = (IServerGameDLL *)gameServerFactory("ServerGameDLL006", 0);
        if (!server) server = (IServerGameDLL *)gameServerFactory("ServerGameDLL005", 0);
        if (!server) server = (IServerGameDLL *)gameServerFactory("ServerGameDLL004", 0);
        if (!server) server = (IServerGameDLL *)gameServerFactory("ServerGameDLL003", 0);
        gameevents = (IGameEventManager2 *)interfaceFactory(INTERFACEVERSION_GAMEEVENTSMANAGER2, 0);
        // Listener registration happens only after the mandatory interfaces and
        // gpGlobals are proven valid (see the interface check below), so a
        // failed Load can never leave the engine holding a listener into an
        // unloaded DLL.

        Msg("[insbots] IServerGameDLL: %p\n", server);
        Msg("[insbots] IVModelInfo: %p%s\n", modelinfo,
            modelinfo ? "" : "  (absent - the turret cannot resolve its pose parameters)");
        if (server)
        {
            ServerClass *pClass = server->GetAllServerClasses();
            while (pClass)
            {
                if (strcmp(pClass->GetName(), "CBasePlayer") == 0 || strcmp(pClass->GetName(), "CBaseCombatCharacter") == 0 || strcmp(pClass->GetName(), "CBaseEntity") == 0 || strcmp(pClass->GetName(), "CINSPlayer") == 0)
                {
                    int offset = FindSendPropOffset(pClass->m_pTable, "m_flNextAttack");
                    if (offset >= 0 && s_offset_flNextAttack == -1)
                    {
                        s_offset_flNextAttack = offset;
                        Msg("[insbots] Found m_flNextAttack at offset %d\n", offset);
                    }
                    
                    offset = FindSendPropOffset(pClass->m_pTable, "m_angEyeAngles[0]");
                    if (offset >= 0 && s_offset_angEyeAngles0 == -1) s_offset_angEyeAngles0 = offset;
                    
                    offset = FindSendPropOffset(pClass->m_pTable, "m_angEyeAngles[1]");
                    if (offset >= 0 && s_offset_angEyeAngles1 == -1) s_offset_angEyeAngles1 = offset;

                    offset = FindSendPropOffset(pClass->m_pTable, "m_CollisionGroup");
                    if (offset >= 0 && s_offset_m_CollisionGroup == -1)
                    {
                        s_offset_m_CollisionGroup = offset;
                        Msg("[insbots] Found m_CollisionGroup at offset %d\n", offset);
                    }
                }
                
                int offset_mywep = FindSendPropOffset(pClass->m_pTable, "m_hMyWeapons");
                if (offset_mywep >= 0 && s_offset_m_hMyWeapons == -1)
                {
                    s_offset_m_hMyWeapons = offset_mywep;
                    Msg("[insbots] Found m_hMyWeapons at offset %d\n", offset_mywep);
                }
                int offset_wep = FindSendPropOffset(pClass->m_pTable, "m_hActiveWeapon");
                if (offset_wep >= 0 && s_offset_m_hActiveWeapon == -1) s_offset_m_hActiveWeapon = offset_wep;

                int offset_clip = FindSendPropOffset(pClass->m_pTable, "m_iClip");
                if (offset_clip >= 0 && s_offset_m_iClip1 == -1) s_offset_m_iClip1 = offset_clip;

                if (strcmp(pClass->GetName(), "CPropVehicleDriveable") == 0 ||
                    strcmp(pClass->GetName(), "CPropVehicle") == 0)
                {
                    // Resolved BY NAME so a future build that moves the member
                    // fails closed (offset stays -1) instead of reading the
                    // wrong dword. Design spec section 3.3.
                    int off = FindSendPropOffset(pClass->m_pTable, "m_hPlayer");
                    if (off >= 0 && g_TurretOffsetDriver == -1)
                    {
                        g_TurretOffsetDriver = off;
                        Msg("[insbots] Found m_hPlayer at offset %d\n", off);
                    }
                    off = FindSendPropOffset(pClass->m_pTable, "m_flPoseParameter");
                    if (off >= 0 && g_TurretOffsetPoseParameter == -1)
                    {
                        g_TurretOffsetPoseParameter = off;
                        Msg("[insbots] Found m_flPoseParameter at offset %d\n", off);
                    }
                    // m_flAnimTime: the client-side interpolation clock. See
                    // CTurretRegistry::DriveAnimTime() for why a plugin has to
                    // advance it on this engine.
                    off = FindSendPropOffset(pClass->m_pTable, "m_flAnimTime");
                    if (off >= 0 && g_TurretOffsetAnimTime == -1)
                    {
                        g_TurretOffsetAnimTime = off;
                        Msg("[insbots] Found m_flAnimTime at offset %d\n", off);
                    }
                    else if (off < 0)
                    {
                        // Silent failure here used to disable the client pose
                        // interpolation workaround with no diagnostic. Say so
                        // once per map.
                        static bool s_loggedAnimTimeMissing = false;
                        if (!s_loggedAnimTimeMissing)
                        {
                            s_loggedAnimTimeMissing = true;
                            Msg("[insbots] WARNING: m_flAnimTime not found; "
                                "client pose interpolation workaround disabled\n");
                        }
                    }
                }

                pClass = pClass->m_pNext;
            }
#ifdef _LINUX
            // Linux eye-angle SendProps use negative proxy offsets, so they are
            // not usable as entity-memory offsets. The other four values below
            // were independently confirmed both by their owning accessors and
            // by the live recursive SendTable dump. Bind this build to the
            // verified server_i486.so layout instead of reusing Windows values.
            if (s_offset_flNextAttack >= 0 && s_offset_flNextAttack != INS_LINUX_M_FL_NEXT_ATTACK)
                Msg("[insbots] WARNING: Linux m_flNextAttack SendProp=%d, audited=%d\n",
                    s_offset_flNextAttack, INS_LINUX_M_FL_NEXT_ATTACK);
            if (s_offset_m_CollisionGroup >= 0 && s_offset_m_CollisionGroup != INS_LINUX_M_COLLISION_GROUP)
                Msg("[insbots] WARNING: Linux m_CollisionGroup SendProp=%d, audited=%d\n",
                    s_offset_m_CollisionGroup, INS_LINUX_M_COLLISION_GROUP);
            if (s_offset_m_hActiveWeapon >= 0 && s_offset_m_hActiveWeapon != INS_LINUX_M_H_ACTIVE_WEAPON)
                Msg("[insbots] WARNING: Linux m_hActiveWeapon SendProp=%d, audited=%d\n",
                    s_offset_m_hActiveWeapon, INS_LINUX_M_H_ACTIVE_WEAPON);
            if (s_offset_m_hMyWeapons >= 0 && s_offset_m_hMyWeapons != INS_LINUX_M_H_MY_WEAPONS)
                Msg("[insbots] WARNING: Linux m_hMyWeapons SendProp=%d, audited=%d\n",
                    s_offset_m_hMyWeapons, INS_LINUX_M_H_MY_WEAPONS);
            if (s_offset_m_iClip1 >= 0 && s_offset_m_iClip1 != INS_LINUX_M_I_CLIP)
                Msg("[insbots] WARNING: Linux m_iClip SendProp=%d, audited=%d\n",
                    s_offset_m_iClip1, INS_LINUX_M_I_CLIP);

            s_offset_flNextAttack = INS_LINUX_M_FL_NEXT_ATTACK;
            s_offset_angEyeAngles0 = INS_LINUX_M_ANG_EYE_ANGLES_0;
            s_offset_angEyeAngles1 = INS_LINUX_M_ANG_EYE_ANGLES_1;
            s_offset_m_CollisionGroup = INS_LINUX_M_COLLISION_GROUP;
            s_offset_m_hMyWeapons = INS_LINUX_M_H_MY_WEAPONS;
            s_offset_m_hActiveWeapon = INS_LINUX_M_H_ACTIVE_WEAPON;
            s_offset_m_iClip1 = INS_LINUX_M_I_CLIP;
            s_offset_m_IronsightsState = INS_LINUX_M_PLAYER_FLAGS;
            Msg("[insbots] Linux offsets: buttons=0x%X next=0x%X eye=0x%X/0x%X collision=0x%X weapon=0x%X mywep=0x%X clip=0x%X\n",
                INS_LINUX_M_NBUTTONS, s_offset_flNextAttack,
                s_offset_angEyeAngles0, s_offset_angEyeAngles1,
                s_offset_m_CollisionGroup, s_offset_m_hActiveWeapon,
                s_offset_m_hMyWeapons, s_offset_m_iClip1);
            Msg("[insbots] Linux ADS state offset: playerflags=0x%X (bit0 = shouldered)\n", s_offset_m_IronsightsState);
            Msg("[insbots] Linux objective offsets: parent=0x%X order=0x%X owner=0x%X hidden=0x%X capture=0x%X\n",
                INS_LINUX_OBJECTIVE_PARENT, INS_LINUX_OBJECTIVE_ORDER_ID,
                INS_LINUX_OBJECTIVE_CAPTURED_TEAM, INS_LINUX_OBJECTIVE_HIDDEN,
                INS_LINUX_OBJECTIVE_CAPTURE_TEAM);
#else
            if (s_offset_m_hMyWeapons < 0)
            {
                s_offset_m_hMyWeapons = 0x0A24; // Audited CBaseCombatCharacter::m_hMyWeapons in server.dll
                Msg("[insbots] Fallback m_hMyWeapons bound to 0x0A24 (%d)\n", s_offset_m_hMyWeapons);
            }

            // ADS ground truth on Windows: CINSPlayer::GetPlayerFlags() is
            // `mov eax,[ecx+0xACC]; ret` in the shipped server.dll, and
            // SetIronsightsState drives bit 0 via AddPlayerFlag(1)/
            // RemovePlayerFlag(1). Verified live on the Windows test server.
            s_offset_m_IronsightsState = 0x0ACC;
            Msg("[insbots] Windows ADS state offset: playerflags=0x%X (bit0 = shouldered)\n",
                s_offset_m_IronsightsState);
#endif
            if (s_offset_m_CollisionGroup < 0)
                Msg("[insbots] WARNING: m_CollisionGroup unavailable; spawn_nocollide will fail closed\n");
        }

        if (!engine || !enginetrace || !helpers || !g_pCVar || !playerinfomanager || !botmanager)
        {
            // Msg may not even work without tier0 loaded, but tier0 is always up by now
            Msg("[insbots] FAILED to get interfaces: engine=%p trace=%p helpers=%p cvar=%p pim=%p botman=%p\n",
                engine, enginetrace, helpers, g_pCVar, playerinfomanager, botmanager);
            return false;
        }

        gpGlobals = playerinfomanager->GetGlobalVars();
        if (!gpGlobals)
        {
            Msg("[insbots] FAILED to get CGlobalVars from playerinfomanager\n");
            return false;
        }

        ConCommandBaseMgr::OneTimeInit(&g_ConVarAccessor);

        srand((unsigned int)time(0));
        if (gameevents)
        {
            bool resetOk = gameevents->AddListener(this, "round_reset", true);
            bool openOk = gameevents->AddListener(this, "round_unfrozen", true);
            bool endOk = gameevents->AddListener(this, "round_end", true);
            s_bRoundFreezeEventsReady = resetOk && openOk;
            if (!s_bRoundFreezeEventsReady)
            {
                gameevents->RemoveListener(this);
                Msg("[insbots] WARNING: round freeze events unavailable; freeze-time teleport guard disabled\n");
            }
            else
                Msg("[insbots] round freeze guard ready (round_reset/unfrozen, end=%d)\n", endOk ? 1 : 0);
        }
        
        InsBots_InitWeaponCvars();

        Msg("[insbots] v%s loaded (Insurgency: Modern Infantry Combat bot plugin)\n", INSBOTS_VERSION);
        return true;
    }

    virtual void Unload(void)
    {
        if (gameevents) gameevents->RemoveListener(this);
        s_bRoundFreezeEventsReady = false;
        g_RoundFrozen = false;
        InsBots_SaveLearn();   // don't lose what the bots learned this session
        g_Bots.ResetBodyGracePlayers(true);
        g_Bots.KickAll();
        g_Bots.ClearAllReservations();
        g_NavMesh.Free();
        // CRITICAL: remove our console command from the engine's list before the
        // DLL unloads, or the engine walks a dangling pointer on the next
        // console autocomplete and crashes. UnlinkVariables strips everything we
        // registered with FCVAR_PLUGIN (just our one "insbot" command).
        if (g_pCVar) g_pCVar->UnlinkVariables(FCVAR_PLUGIN);
    }

    virtual void Pause(void) {}
    virtual void UnPause(void) {}

    virtual const char *GetPluginDescription(void)
    {
        return "InsBots " INSBOTS_VERSION " - AI players for INS:MIC";
    }

    virtual void FireGameEvent(IGameEvent *event)
    {
        if (!event) return;
        const char *name = event->GetName();
        if (!name) return;

        float now = gpGlobals ? gpGlobals->curtime : 0.0f;

        if (!strcmp(name, "round_end"))
            g_RoundFrozen = true;
        else if (!strcmp(name, "round_reset") || !strcmp(name, "round_unfrozen"))
            g_RoundFrozen = false;

        // ---- freeze window (turret gate only; see round_freeze.h) ----------
        if (!strcmp(name, "round_reset"))
        {
            s_freezeWindow.Reset( now );
            g_RoundFreezeBitObservable = false;
        }
        else if (!strcmp(name, "round_end"))
        {
            s_freezeWindow.OnRoundEnd( now );
        }
        else if (!strcmp(name, "round_unfrozen"))
        {
            s_freezeWindow.OnUnfrozen( now );
        }
        g_RoundFreezeTime = !s_freezeWindow.TurretsLive(
            now, insbot_turret_freeze_fallback.GetFloat());

        // m_iOrderID is assigned during round setup, so refresh it on every
        // round start; the values cached at ServerActivate() are still -1.
        if (!strcmp(name, "round_reset") || !strcmp(name, "round_unfrozen"))
            g_Objectives.RefreshOrderIds();

        // A new round rebuilds squads: any pending slot request is void.
        if (!strcmp(name, "round_reset") || !strcmp(name, "round_end"))
            g_Bots.ClearAllReservations();

        if (insbot_debug.GetBool()
            && (!strcmp(name, "round_reset") || !strcmp(name, "round_end")
                || !strcmp(name, "round_unfrozen")))
            Msg("[insbots] round state event: %s (frozen=%d, turrets_live=%d)\n",
                name, g_RoundFrozen ? 1 : 0, g_RoundFreezeTime ? 0 : 1);
    }

    virtual void LevelInit(char const *pMapName)
    {
        // ServerActivate marks the point at which the new map is ready. Do not
        // create quota clients while a large map is still being initialized.
        s_bLevelActive = false;
        s_flQuotaStartTime = 0.0f;
        s_flNextQuotaCheck = 0.0f;
        s_flNextQuotaAdd = 0.0f;
        s_flNextRosterSweep = 0.0f;
        s_flQuotaStartupReleaseTime = 0.0f;
        s_bQuotaInitialBatchDone = false;
        g_HoldQuotaJoins = false;
        // A newly loaded level starts before the game's round_unfrozen event.
        // Only enable this state when both required event hooks registered;
        // otherwise fail open rather than freezing bots for the whole map.
        g_RoundFrozen = s_bRoundFreezeEventsReady;
        // A newly loaded level starts before any round event. Hold turrets
        // dormant until the first round_reset arrives and closes the window
        // properly, so a pre-registered truck cannot fire during map setup.
        s_freezeWindow.Reset( gpGlobals ? gpGlobals->curtime : 0.0f );
        g_RoundFreezeBitObservable = false;
        g_RoundFreezeTime = true;
        g_Bots.LevelInit(pMapName);
        g_Objectives.Reset();
        {
            char gd[256]; engine->GetGameDir(gd, sizeof(gd));
            g_Objectives.LoadLetters(gd, pMapName);   // HUD letters from .imc2 + .bsp
            g_Objectives.LoadProfile(gd, pMapName);   // gametype -> ordering mode
        }
        Msg("[insbots] level '%s': nav %s (%d areas)\n", pMapName,
            g_NavMesh.IsLoaded() ? "loaded" : "not found - bots will roam by feel; run nav_generate if available",
            g_NavMesh.Count());
    }

    virtual void ServerActivate(edict_t *pEdictList, int edictCount, int clientMax)
    {
        g_Objectives.ScanEntities();
        g_Objectives.RefreshOrderIds();  // best-effort; round_reset re-runs it
        InsVoice_Precache();          // register the bot radio-callout waves for this level
        g_Bots.BuildCoverDatabase();  // take-cover v2: traces need loaded world
        s_flQuotaStartTime = gpGlobals ? gpGlobals->curtime : 0.0f;
        s_flNextQuotaCheck = s_flQuotaStartTime;
        s_flNextQuotaAdd = s_flQuotaStartTime;
        s_flNextRosterSweep = s_flQuotaStartTime;
        s_flQuotaStartupReleaseTime = s_flQuotaStartTime;
        s_bQuotaInitialBatchDone = false;
        g_HoldQuotaJoins = insbot_quota.GetInt() > 0;
        s_bLevelActive = true;
        Msg("[insbots] found %d objective(s) on this map\n", g_Objectives.Count());
    }

    virtual void GameFrame(bool simulating)
    {
        InsProf_SetEnabled(insbot_profile.GetBool());
        InsProf_FrameBegin();
        if (simulating && gpGlobals)
        {
            INSPROF_SCOPE(INSPROF_SMOKE);
            UpdateSmokeGrenades();
        }
        
        // Quota startup waits for map activation plus join_delay, then creates
        // fake clients in controlled waves.  Their team/class commands stay held
        // until the complete roster exists, so the round cannot begin midway
        // through class selection.  Later replacement bots are paced separately.
        if (gpGlobals && s_bLevelActive)
        {
            float now = gpGlobals->curtime;

            if (now >= s_flNextRosterSweep)
            {
                s_flNextRosterSweep = now + 0.5f;
                g_Bots.AdoptOrphans();
                if (g_Objectives.Count() == 0) g_Objectives.ScanEntities(); // late-spawned ents
            }

            if (now >= s_flNextQuotaCheck)
            {
                // Recheck every game frame so a completed startup batch releases
                // together on the following FrameUpdate; replacements are paced.
                s_flNextQuotaCheck = now;
                int cap = BotCap();             // leaves 'reserve' slots for humans
                int have = g_Bots.Count();
                int want = insbot_quota.GetInt();
                int reserved = g_Bots.GetReservationCount();
                if (want > 0)
                {
                    want -= reserved;
                    if (want < 0) want = 0;
                    if (want > cap) want = cap;
                    // Capture readiness before AddBot.  When the final client is
                    // created below, keep the hold for one complete FrameUpdate
                    // so that bot reaches the same first-command boundary as the
                    // rest of the staged batch.
                    bool quotaReadyAtFrameStart = (have >= want);
                    float delay = insbot_join_delay.GetFloat();
                    if (delay < 0.0f) delay = 0.0f;
                    float startupInterval = insbot_quota_start_interval.GetFloat();
                    if (startupInterval < 0.05f) startupInterval = 0.05f;
                    float interval = insbot_quota_add_interval.GetFloat();
                    if (interval < 0.05f) interval = 0.05f;

                    if (!s_bQuotaInitialBatchDone)
                    {
                        // Do not let the first bot's pfullsetup start a round
                        // while later bots are still being created.  This also
                        // spreads CreateBot's player-model/material work across
                        // several frames, which is essential on large maps.
                        g_HoldQuotaJoins = true;

                        if (have >= want)
                        {
                            s_bQuotaInitialBatchDone = true;
                            // Existing bots on a map change are already safe to
                            // release; newly-created rosters receive one second
                            // to finish their engine-side client initialization.
                            s_flQuotaStartupReleaseTime = now;
                        }
                        else if (now >= s_flQuotaStartTime + delay
                                 && now >= s_flNextQuotaAdd)
                        {
                            s_flNextQuotaAdd = now + startupInterval;
                            if (g_Bots.AddBot(0))
                            {
                                have++;
                                if (have >= want)
                                {
                                    s_bQuotaInitialBatchDone = true;
                                    s_flQuotaStartupReleaseTime = now + 1.0f;
                                    Msg("[insbots] initial quota roster ready; releasing class setup in 1.0 second\n");
                                }
                            }
                        }
                    }
                    else
                    {
                        g_HoldQuotaJoins = now < s_flQuotaStartupReleaseTime;

                        if (have < want && now >= s_flNextQuotaAdd)
                        {
                            s_flNextQuotaAdd = now + interval;
                            if (g_Bots.AddBot(0)) have++;
                        }

                        if (have > want && now >= s_flNextQuotaAdd)
                        {
                            s_flNextQuotaAdd = now + 0.25f;
                            g_Bots.KickOne();
                        }
                    }

                    // Preserve the old one-frame boundary for a roster that
                    // was already complete before this frame (for example an
                    // orphaned roster adopted after a map change).
                    if (quotaReadyAtFrameStart && s_bQuotaInitialBatchDone)
                        g_HoldQuotaJoins = now < s_flQuotaStartupReleaseTime;
                }
                else
                {
                    g_HoldQuotaJoins = false;
                    if (have > cap && now >= s_flNextQuotaAdd)
                    {
                        s_flNextQuotaAdd = now + 0.25f;
                        g_Bots.KickOne();         // manual bots yield room gradually
                    }
                }
            }
        }
        g_Bots.FrameUpdate(simulating);
        // Sample CINSPlayer::IsMoveFrozen() from any live player and feed the
        // window. Bit 1 of m_iPlayerFlags; bit 0 is ADS, which insbots already
        // reads at s_offset_m_IronsightsState.
        //
        // NOT restricted to a vehicle seat, deliberately. The bit is a
        // player-level flag (CINSPlayer::IsMoveFrozen), so gating the read on
        // a driver would make it unobservable whenever nobody is in a vehicle
        // - which is most of freeze time on most maps. A sample that never
        // reads true is harmless: RoundFreezeWindow::ObserveFrozenBit only
        // lets the bit CLOSE the window, never open it, so the elapsed-time
        // fallback still governs. If a live run shows the bit is only ever
        // set for seated players, report it; do not "fix" it by adding a seat
        // check, which would silently disable the input.
        if (simulating && gpGlobals && s_offset_m_IronsightsState >= 0)
        {
            bool sampled = false;
            for (int ci = 1; ci <= gpGlobals->maxClients && !sampled; ci++)
            {
                edict_t *pe = engine->PEntityOfEntIndex( ci );
                if (!pe || pe->IsFree()) continue;
                IServerUnknown *u = pe->GetUnknown();
                if (!u || !u->GetBaseEntity()) continue;
                IPlayerInfo *pi = playerinfomanager ? playerinfomanager->GetPlayerInfo( pe ) : 0;
                if (!pi || !pi->IsConnected() || pi->IsDead()) continue;

                int flags = *(int *)((char *)u->GetBaseEntity() + s_offset_m_IronsightsState);
                bool frozen = ((flags >> 1) & 1) != 0;
                s_freezeWindow.ObserveFrozenBit( frozen, gpGlobals->curtime );
                if (frozen) g_RoundFreezeBitObservable = true;
                sampled = true;
            }
            g_RoundFreezeTime = !s_freezeWindow.TurretsLive(
                gpGlobals->curtime, insbot_turret_freeze_fallback.GetFloat());
        }
        g_Turrets.FrameUpdate(simulating);
        InsProf_FrameEnd();
        InsProf_Tick();
    }

    virtual void LevelShutdown(void)
    {
        s_bLevelActive = false;
        g_RoundFrozen = s_bRoundFreezeEventsReady;
        // The freeze window is separate state from the turret registry below;
        // reset it here too so the flag cannot survive a level.
        s_freezeWindow.Reset( gpGlobals ? gpGlobals->curtime : 0.0f );
        g_RoundFreezeBitObservable = false;
        g_RoundFreezeTime = true;
        g_HoldQuotaJoins = false;
        // Drop every turret: the entities they reference are about to be freed,
        // and a stale entry would otherwise be purged one-per-frame by
        // CTurret::Validate() the next time the map loads. The registry is keyed
        // by entindex, which is NOT stable across a level, so it must never
        // survive one.
        g_Turrets.RemoveAll();
        TurretOverridesClear();
        g_Bots.LevelShutdown();
    }

    virtual void ClientActive(edict_t *pEntity) {}

    virtual void ClientDisconnect(edict_t *pEntity)
    {
        g_Bots.BodyGraceClientDisconnect(pEntity);
        CInsBot *b = g_Bots.FindByEdict(pEntity);
        if (b) b->OnRemoved();
    }

    virtual void ClientPutInServer(edict_t *pEntity, char const *playername) {}
    virtual void SetCommandClient(int index)
    {
        m_iClientCommandIndex = index;
        s_commandClientIndex = index;
        // NOTE: SetCommandClient is NOT called for fake/bot clients - only for
        // real network clients processing usercmds. Bot commands are submitted
        // via RunPlayerMove called from ThinkAlive (GameFrame path).
    }
    virtual void ClientSettingsChanged(edict_t *pEdict) {}

    virtual PLUGIN_RESULT ClientConnect(bool *bAllowConnect, edict_t *pEntity,
                                        const char *pszName, const char *pszAddress,
                                        char *reject, int maxrejectlen)
    {
        return PLUGIN_CONTINUE;
    }

    virtual PLUGIN_RESULT ClientCommand(edict_t *pEntity)
    {
        return PLUGIN_CONTINUE;
    }

    virtual PLUGIN_RESULT NetworkIDValidated(const char *pszUserName, const char *pszNetworkID)
    {
        return PLUGIN_CONTINUE;
    }

private:
    int m_iClientCommandIndex;
};

static CInsBotsPlugin g_InsBotsPlugin;

// ---- exported factory --------------------------------------------------------
// Hand-rolled CreateInterface (we don't link tier1's interface.cpp).
#ifdef _LINUX
extern "C" __attribute__((visibility("default"))) void *CreateInterface(const char *pName, int *pReturnCode)
#else
extern "C" __declspec(dllexport) void *CreateInterface(const char *pName, int *pReturnCode)
#endif
{
    if (pName && !strcmp(pName, INTERFACEVERSION_ISERVERPLUGINCALLBACKS))
    {
        if (pReturnCode) *pReturnCode = IFACE_OK;
        return &g_InsBotsPlugin;
    }
    if (pReturnCode) *pReturnCode = IFACE_FAILED;
    return 0;
}
