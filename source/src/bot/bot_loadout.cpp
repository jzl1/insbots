// ============================================================================
// bot_loadout.cpp - join commands, role selection, customization, weapon cvars.
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
extern IVEngineServer       *engine;
extern IPlayerInfoManager   *playerinfomanager;
extern CGlobalVars          *gpGlobals;
extern IServerPluginHelpers *helpers;
extern int                   s_offset_m_hMyWeapons;
extern CfgVar                insbot_class_priority;
extern CfgVar                insbot_class_quota_rpg;
extern CfgVar                insbot_class_quota_grenadier;
extern CfgVar                insbot_class_quota_sniper;
extern CfgVar                insbot_class_quota_marksman;
extern CfgVar                insbot_class_quota_support;
extern CfgVar                insbot_class_quota_engineer;
extern CfgVar                insbot_class_quota_sapper;
extern CfgVar                insbot_class_quota_rifleman;
extern CfgVar                insbot_class_quota_sergeant;
extern CfgVar                insbot_class_quota_militant;
extern CfgVar                insbot_randomize_loadout;
extern CfgVar                insbot_randomize_customization;
extern CfgVar                insbot_join_cmds_a;
extern CfgVar                insbot_join_cmds_b;
extern CfgVar                insbot_join_interval;
extern CfgVar                insbot_join_timeout;
extern CfgVar                insbot_slot_sweep;
extern CfgVar                insbot_recoil_control;
extern bool                  g_HoldQuotaJoins;

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
// modes bitfield FM_* is declared in bot_internal.h.  rpm = cyclic rate
// (rounds/min, the game's "cycle_time"); ms/round = 60000/rpm.  recoil = the
// script's recoil_scale (multiplies the global base kick).
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

const WeaponProfile *WeaponFind(const char *name)
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

// INS:MIC's m_hMyWeapons is a SEVEN-element array, NOT the SDK's MAX_WEAPONS (48);
// INS_MAX_WEAPONS is declared in bot_internal.h. Reading past the 7th slot walks
// into adjacent player members and can resolve a garbage handle to a live
// weapon_* entity, so that bound must stay 7.
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
edict_t *InsBotResolveWeaponSlot(char *playerBase, int slot, int *pOutIndex)
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
