// ============================================================================
// bot_entry.cpp - plugin entry points, faction loading, learn persistence.
//
// These are the free functions the rest of the plugin calls into: the
// learned-bad-link persistence hooks, per-map faction resolution, the forced
// goto-pathfinding override, and the ammo debug dump.
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
#include "gametrace.h"
#include "const.h"
#include "convar.h"
#include "bot.h"
#include "nav.h"
#include "objective.h"
#include "bot_internal.h"

// ---- externs this TU needs (same pattern as bot.cpp / objective.cpp) -------
extern IVEngineServer       *engine;
extern IEngineTrace         *enginetrace;
extern IPlayerInfoManager   *playerinfomanager;
extern CGlobalVars          *gpGlobals;
extern CfgVar                insbot_learn_persist;
extern int                   s_offset_m_hActiveWeapon;
extern int                   s_offset_m_iClip1;
extern int                   s_offset_angEyeAngles0;
extern int                   s_offset_angEyeAngles1;

bool g_ForceGotoActive = false;
Vector g_ForceGotoPos(0, 0, 0);

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

// learned-bad-link file for the current map (persists runtime discoveries)
char s_learnPath[512] = {0};

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
