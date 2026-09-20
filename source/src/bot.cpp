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
void InsBots_ForgetLearn();   // wipe learned bad links (memory + file)

const char *s_BotNamesUSMC[] = {
    "James", "John", "Robert", "Michael", "David", "William", "Richard", "Thomas",
    "Charles", "Daniel", "Matthew", "Anthony", "Joseph", "Steven", "Andrew", "Joshua",
};

const char *s_BotNamesIraqi[] = {
    "Ahmed", "Ali", "Hassan", "Hussein", "Omar", "Abbas", "Mustafa", "Saad",
    "Tariq", "Zaid", "Kareem", "Faisal", "Qasim", "Yasin", "Bilal", "Mahmoud",
};

// In Insurgency, teamSlot 0 (pteamsetup 1) lands on engine team index 2,
// while teamSlot 1 (pteamsetup 2) lands on engine team index 1!
int TeamSlotToTeamIndex(int teamSlot)
{
    return (teamSlot == 0) ? 2 : 1;
}

// The class-menu protocol does not number roles consecutively.  These are the
// sixteen IDs emitted by the stock USMC and Iraqi menus (same IDs for both).
static int JoinRoleId(int ordinal, int cycle, int teamSlot)
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

    // Pass 1: Find a candidate that is neither reserved nor currently requested by an active bot
    for (int offset = 0; offset < 16; offset++)
    {
        int candidate = s_roleIds[(roleIndex + offset) % 16];
        if (!g_Bots.IsRoleReserved(teamSlot, candidate) && !g_Bots.IsRoleInUse(teamSlot, candidate))
            return candidate;
    }

    // Pass 2: Fallback to any unreserved candidate
    for (int offset = 0; offset < 16; offset++)
    {
        int candidate = s_roleIds[(roleIndex + offset) % 16];
        if (!g_Bots.IsRoleReserved(teamSlot, candidate))
            return candidate;
    }
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
int PickPriorityClassRole(int teamSlot)
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
        if (filled < wanted)
        {
            for (int r = 0; r < roleCount; r++)
            {
                int candidate = roles[(filled + r) % roleCount];
                if (!g_Bots.IsRoleReserved(teamSlot, candidate))
                    return candidate;
            }
            // Every candidate for this class is reserved; fall through to the
            // next priority entry rather than stealing a reserved slot.
        }
    }
    return -1; // quotas met/off: preserve the ordinary automatic rotation
}

int GetTeamSlotForFaction(int targetFaction)
{
    int fac0 = g_teamFaction[TeamSlotToTeamIndex(0)];
    int fac1 = g_teamFaction[TeamSlotToTeamIndex(1)];
    if (fac0 == FACTION_UNKNOWN) fac0 = FACTION_IRAQI; // slot 0 -> engine team 2
    if (fac1 == FACTION_UNKNOWN) fac1 = FACTION_USMC;  // slot 1 -> engine team 1

    if (fac0 == targetFaction) return 0;
    if (fac1 == targetFaction) return 1;
    return (targetFaction == FACTION_USMC) ? 1 : 0;
}

int GetTeamSlotForBotName(const char *name, int defaultSlot)
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

void BotDbg(const char *fmt, ...);

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
    m_customizationPending = false;
    return true;
}

// ---- weapon profiles (extracted from the game's encrypted .ctx scripts) -----
// modes bitfield: SEMI=1, BURST(3rnd)=2, AUTO=4.  rpm = cyclic rate (rounds/min,
// the game's "cycle_time"); ms/round = 60000/rpm.  recoil = the script's
// recoil_scale (multiplies the global base kick).
enum { FM_SEMI = 1, FM_BURST = 2, FM_AUTO = 4 };
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
WeaponProfile s_wpDefault =
    { "default", 30, FM_SEMI|FM_AUTO, 600, 0, 2.5f, 0, WC_RIFLE, NULL };

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

CombatProfile *CombatForWeapon(const WeaponProfile *wp)
{
    int cls = wp ? (int)wp->combatClass : (int)WC_RIFLE;
    if (cls < 0 || cls >= WC_COUNT) cls = WC_RIFLE;
    return &s_combat[cls];
}

int CombatClassForWeapon(const WeaponProfile *wp)
{
    int cls = wp ? (int)wp->combatClass : (int)WC_RIFLE;
    if (cls < 0 || cls >= WC_COUNT) cls = WC_RIFLE;
    return cls;
}

int CombatReserveMags(const WeaponProfile *wp)
{
    CombatProfile *combat = CombatForWeapon(wp);
    int mags = combat->reserveMags ? combat->reserveMags->GetInt() : 0;
    return mags < 0 ? 0 : mags;
}

float CombatValue(CfgVar *classValue, CfgVar &fallback)
{
    float value = classValue ? classValue->GetFloat() : -1.0f;
    return value < 0.0f ? fallback.GetFloat() : value;
}

float CombatRecoilControl(CombatProfile *combat, float aimError)
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

void CInsBot::SendJoinCommands()
{
    if (!helpers || !m_edict) return;
    float now = Now();

    // If class setup has already been sent, do NOT repeat pteamsetup or pfullsetup!
    // Simply pulse creinforce once every 2 seconds as a heartbeat while waiting for the wave.
    if (m_classSetupSent)
    {
        if (now >= m_nextReinforcePulse)
        {
            m_nextReinforcePulse = now + 2.0f;
            helpers->ClientCommand(m_edict, "creinforce");
            BotDbg("[insbots] %s -> wave wait heartbeat 'creinforce'\n", m_name);
        }
        return;
    }

    // Newly created bots use pfullsetup directly. Sending pteamsetup first can
    // finalize a fresh client with an engine-selected class before our later
    // class request is processed. Once pfullsetup has been transmitted, pulse
    // creinforce and wait for the wave.
    if (m_requestedTeamArg >= 0)
    {
        char cmd[64];
        int roleId = (m_requestedRoleId > 0)
                   ? m_requestedRoleId : JoinRoleId(m_ordinal, m_joinCycles, m_teamSlot);
        if (roleId > 0)
            m_resolvedRoleId = roleId;
        UpdateCustomizationWeapon(roleId);
        if (m_joinCmdIndex == 0)
        {
            _snprintf(cmd, sizeof(cmd), "pfullsetup %d %d",
                      m_requestedTeamArg, roleId);
            cmd[sizeof(cmd) - 1] = 0;
            SendCustomizationBeforeSpawn();
            helpers->ClientCommand(m_edict, cmd);
            BotDbg("[insbots] %s -> targeted setup '%s'\n", m_name, cmd);
            m_joinCmdIndex++;
            return;
        }
        else if (m_joinCmdIndex == 1)
        {
            helpers->ClientCommand(m_edict, "creinforce");
            BotDbg("[insbots] %s -> targeted 'creinforce'\n", m_name);
            m_classSetupSent = true;
            m_nextReinforcePulse = now + 2.0f;
            m_joinCmdIndex = 0;
            return;
        }
        else
        {
            m_classSetupSent = true;
            m_nextReinforcePulse = now + 2.0f;
            m_joinCmdIndex = 0;
            return;
        }
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
        // Entire sequence was sent once: mark class setup complete and enter wave wait
        m_classSetupSent = true;
        m_nextReinforcePulse = now + 2.0f;
        m_joinCmdIndex = 0;
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
                    : JoinRoleId(m_ordinal, m_joinCycles, m_teamSlot);
                if (val > 0)
                    m_resolvedRoleId = val;
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
static bool InsBotIsCarriedWeapon(const char *entityClassName)
{
    return entityClassName && _strnicmp(entityClassName, "weapon_", 7) == 0;
}

// Resolve one m_hMyWeapons slot to a validated, carried weapon edict, or NULL.
// pOutIndex (optional) receives the resolved entity index for diagnostics.
static edict_t *InsBotResolveWeaponSlot(char *playerBase, int slot, int *pOutIndex)
{
    if (pOutIndex) *pOutIndex = 0;
    if (!playerBase || !engine || s_offset_m_hMyWeapons < 0) return 0;
    if (slot < 0 || slot >= INS_MAX_WEAPONS) return 0;
    int h = *(int *)(playerBase + s_offset_m_hMyWeapons + slot * 4);
    if (h == -1 || h == 0) return 0;
    int entIndex = h & 0xFFF;
    if (entIndex < 1 || entIndex >= 2048) return 0;
    edict_t *e = engine->PEntityOfEntIndex(entIndex);
    if (!e || e->IsFree()) return 0;
    if (!InsBotIsCarriedWeapon(e->GetClassName())) return 0;
    if (pOutIndex) *pOutIndex = entIndex;
    return e;
}

void CInsBot::PruneDuplicateWeapons()
{
    if (!m_edict || m_edict->IsFree() || s_offset_m_hMyWeapons < 0 || !engine) return;
    IServerUnknown *un = m_edict->GetUnknown();
    if (!un) return;
    CBaseEntity *playerEnt = un->GetBaseEntity();
    if (!playerEnt) return;

    char *base = (char *)playerEnt;
    const int MAX_WEAPONS = 7;

    char seenClasses[8][32];
    int seenCount = 0;

    for (int i = 0; i < MAX_WEAPONS; i++)
    {
        int h = *(int *)(base + s_offset_m_hMyWeapons + i * 4);
        if (h == -1 || h == 0) continue;
        int entIdx = h & 0xFFF;
        if (entIdx <= gpGlobals->maxClients || entIdx >= 2048) continue;

        edict_t *wEdict = engine->PEntityOfEntIndex(entIdx);
        if (!wEdict || wEdict->IsFree()) continue;
        const char *cls = wEdict->GetClassName();
        if (!cls || !cls[0]) continue;

        bool duplicate = false;
        for (int s = 0; s < seenCount; s++)
        {
            if (!_stricmp(seenClasses[s], cls))
            {
                duplicate = true;
                break;
            }
        }

        if (duplicate)
        {
            BotDbg("[insbots] %s pruning duplicate weapon %s (entindex %d)\n",
                   m_name, cls, entIdx);
            *(int *)(base + s_offset_m_hMyWeapons + i * 4) = -1;
            engine->RemoveEdict(wEdict);
        }
        else if (seenCount < 8)
        {
            strncpy(seenClasses[seenCount], cls, sizeof(seenClasses[0]) - 1);
            seenClasses[seenCount][sizeof(seenClasses[0]) - 1] = 0;
            seenCount++;
        }
    }
}

void CInsBot::ThinkJoin()
{
    float now = Now();

    // A classed bot may already have become alive while the startup batch is
    // held; let it transition normally without sending another setup command.
    if (m_info->GetTeamIndex() > 0 && !m_info->IsDead())
    {
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

    for (int i = 0; i < INS_MAX_WEAPONS; i++)
    {
        // Validated resolver: rejects empty slots, recycled edict indices and
        // anything that is not a carried CBaseCombatWeapon. Handing a thrown
        // grenade (or any other non-weapon) to Weapon_Switch() is what crashed
        // the server - see InsBotIsCarriedWeapon() above.
        edict_t *wEnt = InsBotResolveWeaponSlot(base, i, 0);
        if (!wEnt) continue;
        const char *cn = wEnt->GetClassName();
        // EXACT match only. The old code also accepted any classname merely
        // CONTAINING the token ("m18"), which is how the thrown "grenade_m18"
        // projectile aliased the carried "weapon_m18".
        if (cn && !_stricmp(cn, weaponClass))
        {
            void *pWeapon = wEnt->GetUnknown()->GetBaseEntity();
            if (pWeapon)
            {
                if (insbot_debug.GetBool())
                {
                    IServerNetworkable *dbgNet = wEnt->GetUnknown()->GetNetworkable();
                    ServerClass *dbgSc = dbgNet ? dbgNet->GetServerClass() : 0;
                    BotDbg("[insbots] %s weapon switch -> %s [%s]\n", m_name, cn,
                           (dbgSc && dbgSc->GetName()) ? dbgSc->GetName() : "?");
                }
                if (s_offset_flNextAttack >= 0)
                    *(float *)(base + s_offset_flNextAttack) = gpGlobals ? gpGlobals->curtime : 0.0f;
                void **vtable = *(void ***)pPlayer;
#ifdef _LINUX
                typedef bool (*Weapon_Switch_Fn)(void *, void *, bool);
                // Audited in Linux server_i486.so _ZTV10CINSPlayer (RVA 0x7706C0):
                // In GCC Itanium ABI, symbol _ZTV has 2 prefix entries (offset-to-top, RTTI).
                // Relocation slot 218 (offset 0x770A30) is _ZN11CBasePlayer13Weapon_SwitchEP17CBaseCombatWeaponb (VA 0x49D65C).
                Weapon_Switch_Fn fnSwitch = (Weapon_Switch_Fn)vtable[218];
                return fnSwitch(pPlayer, pWeapon, force);
#else
                typedef bool (__thiscall *Weapon_Switch_Fn)(void *, void *, bool);
                // Audited in Windows server.dll CINSPlayer primary vtable (VA 0x101F75D4):
                // slot 217 (VA 0x101054E0, offset 0x364) is CBasePlayer::Weapon_Switch(pWeapon, viewmodelindex=0), ending with ret 8.
                // slot 220 (VA 0x10118360, offset 0x370) takes 4 parameters and ends with ret 0x10 (corrupts ESP).
                Weapon_Switch_Fn fnSwitch = (Weapon_Switch_Fn)vtable[217];
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

    for (int i = 0; i < INS_MAX_WEAPONS; i++)
    {
        // Validated resolver: only a carried "weapon_*" item can satisfy
        // HasWeapon, so thrown ordnance ("grenade_m18") no longer aliases it.
        edict_t *wEnt = InsBotResolveWeaponSlot(base, i, 0);
        if (!wEnt) continue;
        const char *cn = wEnt->GetClassName();
        if (cn && !_stricmp(cn, weaponClass))   // exact: see SwitchToWeapon()
            return true;
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

    // ADS intent is re-derived every frame (the combat block only runs for the
    // standard-firearms branch). Clearing it here stops a bot that stops
    // engaging, switches to a launcher or loses its enemy from holding a stale
    // request, which would latch the sights up and re-pulse forever.
    m_wantAds = false;

    if (m_info->IsDead())
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
