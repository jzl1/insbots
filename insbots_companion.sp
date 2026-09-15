#pragma semicolon 1
#pragma newdecls required

#include <sourcemod>
#include <sdktools>
#include <sdkhooks>

public Plugin myinfo = 
{
    name = "InsBots Companion",
    author = "",
    description = "Consolidated companion plugin for InsBots: Version Exposer, Squad Bridge, and Slot Requester.",
    version = "1.0",
    url = ""
};

// ============================================================================
// 1. Version Exposer State & ConVars (Always ON)
// ============================================================================
ConVar g_cvInsbotVersion;
ConVar g_cvInsUtilVersion;

// ============================================================================
// 2. Squad Bridge State & Orders (Always ON)
// ============================================================================
bool g_bSquadActive[MAXPLAYERS + 1];
int  g_iSquadSprite[MAXPLAYERS + 1];
int  g_iBotSquadLeader[MAXPLAYERS + 1];
int   g_iBeamModel;   // (unused, kept to avoid renumbering)
int   g_iBeamHalo;   // (unused, kept to avoid renumbering)
Handle g_hGlowTimer[MAXPLAYERS + 1];
float g_fWaypointPos[MAXPLAYERS + 1][3];
bool  g_bHasWaypoint[MAXPLAYERS + 1];
int   g_iCoverFireMarker[MAXPLAYERS + 1];
int   g_iCoverFireMarkerAnchor[MAXPLAYERS + 1];
Handle g_hCoverFireMarkerTimer[MAXPLAYERS + 1];
float  g_flCoverFireEndTime[MAXPLAYERS + 1];
float  g_fCoverFireTarget[MAXPLAYERS + 1][3];
int    g_iDefendMarker[MAXPLAYERS + 1];
int    g_iDefendMarkerAnchor[MAXPLAYERS + 1];
Handle g_hDefendMarkerTimer[MAXPLAYERS + 1];
float  g_fDefendMarkerTarget[MAXPLAYERS + 1][3];

// ============================================================================
// 3. Slot Requester State & ConVars (Configurable)
// ============================================================================
ConVar g_cvSlotRequestEnabled;
ConVar g_cvRadioAutoPromote;
ConVar g_cvCustomSquadSprite;

// Slot-request reservations (Option B). While a reservation is pending, the
// C++ plugin keeps that slot code empty so bots cannot take it back.
int g_iPendingReservedSlot[MAXPLAYERS + 1];
int g_iPendingReservedTeam[MAXPLAYERS + 1];

public void OnPluginStart()
{
    // ---- 1. Version Exposer Setup ----
    g_cvInsbotVersion = CreateConVar("insbot_version", "Unknown", "The current version of the InsBots plugin", FCVAR_NOTIFY | FCVAR_DONTRECORD);
    g_cvInsUtilVersion = CreateConVar("ins_util_version", "Unknown", "The current version of the Ins Utilities plugin", FCVAR_NOTIFY | FCVAR_DONTRECORD);
    UpdateVersions();

    // ---- 2. Squad Bridge Commands & Listeners ----
    RegConsoleCmd("sm_squad", Command_Squad, "Toggle bot fireteam escort mode");
    RegConsoleCmd("sm_lead", Command_Squad, "Toggle bot fireteam escort mode");
    RegConsoleCmd("sm_dismiss", Command_Dismiss, "Dismiss bot fireteam to autonomous patrol");

    AddCommandListener(OnCommsKey, "comms");
    RegConsoleCmd("sm_squadorders", Command_SquadOrders, "Open squad orders menu");
    RegConsoleCmd("sm_orders", Command_SquadOrders, "Open squad orders menu (alias)");
    RegConsoleCmd("sm_mark", Command_MarkTarget, "Mark a covering-fire target via eye-ray");

    HookEvent("player_spawn", Event_PlayerSpawn, EventHookMode_Post);
    HookEvent("player_death", Event_PlayerDeath, EventHookMode_Post);
    HookEvent("player_team", Event_PlayerTeam, EventHookMode_Post);

    RegServerCmd("insbot_squad_recruit", Command_SquadRecruit, "Bridge for squad recruitment notifications");
    RegServerCmd("insbot_squad_leader_down", Command_SquadLeaderDown, "Bridge for leader-down notification");

    // ---- 3. Slot Requester Commands & ConVar ----
    g_cvRadioAutoPromote = CreateConVar("insbots_radio_auto_promote", "0", "Auto-promote any radio user to squad leader (legacy behavior)", FCVAR_NOTIFY);
    g_cvCustomSquadSprite = CreateConVar("insbots_custom_squad_sprite", "1", "Attach custom env_sprite squadmate indicator (set 0 if duplicating native commander indicator)", FCVAR_NOTIFY);
    g_cvSlotRequestEnabled = CreateConVar("ins_slot_request_enabled", "1", "Enable or disable the slot requester feature (1 = enabled, 0 = disabled)", FCVAR_NOTIFY);
    RegConsoleCmd("sm_slotrequest", Command_SlotRequestMenu, "Open menu to take a bot-occupied slot (F2)");
    RegConsoleCmd("sm_request", Command_SlotRequestMenu, "Alias for sm_slotrequest");

    for (int c = 0; c <= MaxClients; c++)
    {
        g_iPendingReservedSlot[c] = -1;
        g_iPendingReservedTeam[c] = -1;
    }
}

public void OnClientPutInServer(int client)
{
    if (client > 0 && !IsFakeClient(client))
    {
        CreateTimer(5.0, Timer_AutoBindComms, client, TIMER_FLAG_NO_MAPCHANGE);
    }
}

public Action Timer_AutoBindComms(Handle timer, any client)
{
    if (client > 0 && IsClientInGame(client) && !IsFakeClient(client))
    {
        ClientCommand(client, "bind F2 sm_slotrequest");
        ClientCommand(client, "bind F3 sm_squadorders");
        PrintToChat(client, "[Squad] F3 bound to squad orders. Type !orders anytime.");
    }
    return Plugin_Stop;
}
public void OnMapStart()
{
    for (int i = 1; i <= MaxClients; i++)
    {
        g_bSquadActive[i] = false;
        g_iSquadSprite[i] = 0;
        g_iBotSquadLeader[i] = 0;
        g_hGlowTimer[i] = INVALID_HANDLE;
        g_bHasWaypoint[i] = false;
        g_iCoverFireMarker[i] = 0;
        g_iCoverFireMarkerAnchor[i] = 0;
        g_hCoverFireMarkerTimer[i] = INVALID_HANDLE;
        g_flCoverFireEndTime[i] = 0.0;
        g_iDefendMarker[i] = 0;
        g_iDefendMarkerAnchor[i] = 0;
        g_hDefendMarkerTimer[i] = INVALID_HANDLE;
        g_iPendingReservedSlot[i] = -1;
        g_iPendingReservedTeam[i] = -1;
    }
    g_iBeamModel = PrecacheModel("sprites/glow01.spr");   // squad glow sprite model
    g_iBeamHalo = 0;
    PrecacheModel("materials/hud/indicators/attack.vmt", true);
    PrecacheModel("materials/hud/indicators/defend.vmt", true);
    InitSquadOffsets();

    // Reconciliation guard: the engine can auto-assign a bot into our reserved
    // (deliberately empty) seat via CPlayTeam::GetRandomSquad, which never
    // consults insbots. Re-assert the reservation when that happens.
    CreateTimer(2.0, Timer_EnforceReservations, _, TIMER_REPEAT | TIMER_FLAG_NO_MAPCHANGE);

    UpdateVersions();
}

public Action Timer_DrawBeamRing(Handle timer, any bot)
{
    if (bot <= 0 || bot > MaxClients || !IsClientInGame(bot) || !IsPlayerAlive(bot))
    {
        g_hGlowTimer[bot] = INVALID_HANDLE;
        return Plugin_Stop;
    }

    int leader = g_iBotSquadLeader[bot];
    if (leader <= 0 || !IsClientInGame(leader) || !g_bSquadActive[leader])
    {
        g_hGlowTimer[bot] = INVALID_HANDLE;
        return Plugin_Stop;
    }

    // Option B: attached env_sprite (billboard glow dot) parented to the bot.
    // Created once, follows automatically, only visible to the leader via TE-free
    // entity transmission control (SetTransmit hook).
    if (g_iSquadSprite[bot] <= 0 || !IsValidEntity(g_iSquadSprite[bot]))
    {
        float pos[3];
        GetClientAbsOrigin(bot, pos);
        pos[2] += 72.0;  // above head

        // Fix B: set ALL keyvalues BEFORE DispatchSpawn. Keyvalues applied after
        // spawn are ignored, so the sprite previously spawned with no model and
        // rendered invisible.
        int ent = CreateEntityByName("env_sprite");
        if (ent > 0)
        {
            DispatchKeyValue(ent, "model", "sprites/glow01.spr");
            DispatchKeyValue(ent, "rendercolor", "0 255 200");
            DispatchKeyValue(ent, "renderamt", "200");
            DispatchKeyValue(ent, "rendermode", "5");  // kRenderTransAdd
            DispatchKeyValue(ent, "scale", "0.35");
            DispatchKeyValue(ent, "spawnflags", "1");

            if (DispatchSpawn(ent))
            {
                SetEdictFlags(ent, (GetEdictFlags(ent) & ~(FL_EDICT_ALWAYS | FL_EDICT_PVSCHECK)));
                TeleportEntity(ent, pos, NULL_VECTOR, NULL_VECTOR);

                SetVariantString("!activator");
                AcceptEntityInput(ent, "SetParent", bot);
                g_iSquadSprite[bot] = ent;
                SDKHook(ent, SDKHook_SetTransmit, Hook_SpriteSetTransmit);
            }
        }
    }

    return Plugin_Continue;
}

public void OnConfigsExecuted()
{
    CreateTimer(0.5, Timer_DelayedUpdate, _, TIMER_FLAG_NO_MAPCHANGE);
}

public Action Timer_DelayedUpdate(Handle timer)
{
    UpdateVersions();
    return Plugin_Stop;
}

public void OnClientDisconnect(int client)
{
    if (client > 0 && client <= MaxClients)
    {
        RemoveCoveringFireMarker(client);
        RemoveDefendMarker(client);
        if (g_bSquadActive[client])
        {
            g_bSquadActive[client] = false;
            DemoteClientFromSquadLeader(client);
            ServerCommand("insbot squad %d 0", client);
        }
        if (g_iPendingReservedSlot[client] >= 0)
        {
            ServerCommand("insbot unreserve %d %d",
                g_iPendingReservedTeam[client], g_iPendingReservedSlot[client]);
            g_iPendingReservedSlot[client] = -1;
            g_iPendingReservedTeam[client] = -1;
        }
    }
}

// ============================================================================
// Unified Chat Command Router (OnClientSayCommand)
// ============================================================================
public Action OnClientSayCommand(int client, const char[] command, const char[] sArgs)
{
    if (client <= 0 || !IsClientInGame(client))
        return Plugin_Continue;

    char text[256];
    strcopy(text, sizeof(text), sArgs);
    StripQuotes(text);
    TrimString(text);

    // 1. Squad Commands (!squad, !lead, !dismiss) are handled automatically by RegConsoleCmd (sm_squad, sm_lead, sm_dismiss)

    // 2. Slot Request Commands (!request, !r, !slot) -> Open F2 Menu
    int idx = 0;
    while (text[idx] == ' ') idx++;

    bool isRequest = false;
    if (strncmp(text[idx], "!request", 8, false) == 0 || strncmp(text[idx], "/request", 8, false) == 0)
    {
        if (text[idx + 8] == '\0' || text[idx + 8] == ' ') isRequest = true;
    }
    else if (strncmp(text[idx], "!r", 2, false) == 0 || strncmp(text[idx], "/r", 2, false) == 0)
    {
        if (text[idx + 2] == '\0' || text[idx + 2] == ' ') isRequest = true;
    }
    else if (strncmp(text[idx], "!slot", 5, false) == 0 || strncmp(text[idx], "/slot", 5, false) == 0)
    {
        if (text[idx + 5] == '\0' || text[idx + 5] == ' ') isRequest = true;
    }

    if (isRequest)
    {
        OpenSlotRequestMenu(client);
        return Plugin_Handled;
    }

    return Plugin_Continue;
}

// ============================================================================
// Squad Lead & Voice Bridge Handlers
// ============================================================================
void AttachSquadGlow(int bot, int leader)
{
    if (bot <= 0 || bot > MaxClients || !IsClientInGame(bot))
        return;

    g_iBotSquadLeader[bot] = leader;

    if (!IsPlayerAlive(bot))
    {
        CreateTimer(0.3, Timer_DelayedAttach, bot, TIMER_FLAG_NO_MAPCHANGE);
        return;
    }

    SetGlowOutline(bot);
}

public Action Timer_DelayedAttach(Handle timer, any bot)
{
    if (bot > 0 && bot <= MaxClients && IsClientInGame(bot) && IsPlayerAlive(bot))
    {
        int leader = g_iBotSquadLeader[bot];
        if (leader > 0 && IsClientInGame(leader) && g_bSquadActive[leader])
        {
            AttachSquadGlow(bot, leader);
        }
    }
    return Plugin_Stop;
}

void RemoveSquadSprite(int bot)
{
    if (bot > 0 && bot <= MaxClients)
    {
        RemoveGlowOutline(bot);  // handles sprite deletion and timer cleanup
    }
}
void SetGlowOutline(int bot)
{
    if (bot <= 0 || bot > MaxClients || !IsClientInGame(bot) || !IsPlayerAlive(bot))
        return;

    // Kill any existing timer first, then spawn the sprite
    KillGlowTimer(bot);

    // Timer creates the sprite on first tick (bot may not be alive yet), then
    // just monitors validity. Sprite visibility is controlled by SetTransmit hook.
    g_hGlowTimer[bot] = CreateTimer(0.4, Timer_DrawBeamRing, bot, TIMER_REPEAT | TIMER_FLAG_NO_MAPCHANGE);
}

void KillGlowTimer(int bot)
{
    if (g_hGlowTimer[bot] != INVALID_HANDLE)
    {
        KillTimer(g_hGlowTimer[bot]);
        g_hGlowTimer[bot] = INVALID_HANDLE;
    }
}

void RemoveGlowOutline(int bot)
{
    if (bot <= 0 || bot > MaxClients)
        return;
    KillGlowTimer(bot);

    // Delete the attached sprite entity if it exists
    if (g_iSquadSprite[bot] > 0 && IsValidEntity(g_iSquadSprite[bot]))
    {
        int ent = g_iSquadSprite[bot];
        g_iSquadSprite[bot] = 0;
        AcceptEntityInput(ent, "Kill");   // Fix M-1: Kill alone; RemoveEdict here caused a double-free
    }
    else
    {
        g_iSquadSprite[bot] = 0;
    }
}

int FindPlayerSquad(int client)
{
    if (client <= 0 || !IsClientInGame(client))
        return -1;

    int reqTeam = GetClientTeam(client);
    if (reqTeam < 1)
        return -1;

    int firstTeamSquad = -1;

    for (int i = 1; i <= GetMaxEntities(); i++)
    {
        if (!IsValidEntity(i)) continue;

        char classname[64];
        if (!GetEntityNetClass(i, classname, sizeof(classname))) continue;

        if (StrEqual(classname, "CINSSquad"))
        {
            int team = GetEntData(i, 860, 4);
            if (team != reqTeam) continue;

            if (firstTeamSquad == -1)
                firstTeamSquad = i;

            for (int s = 0; s < 8; s++)
            {
                int playerId = GetEntData(i, 1024 + (s * 4), 4);
                if (playerId != 0 && playerId != -1)
                {
                    int ent = playerId & 0xFFF;
                    if (ent == client)
                    {
                        return i;
                    }
                }
            }
        }
    }

    LogMessage("[InsBots Companion] FindPlayerSquad FALLBACK for client %d — not enrolled in any squad entity's m_iPlayerSlots.", client);
    return firstTeamSquad;
}

void PromoteClientToSquadLeader(int client)
{
    if (client <= 0 || !IsClientInGame(client))
        return;

    int squadEnt = FindPlayerSquad(client);
    if (squadEnt > 0 && IsValidEntity(squadEnt))
    {
        if (HasEntProp(squadEnt, Prop_Send, "m_hCommander"))
        {
            SetEntPropEnt(squadEnt, Prop_Send, "m_hCommander", client);
        }
    }

    if (HasEntProp(client, Prop_Send, "m_bCommander"))
    {
        SetEntProp(client, Prop_Send, "m_bCommander", 1);
    }
}

void DemoteClientFromSquadLeader(int client)
{
    if (client <= 0 || !IsClientInGame(client))
        return;

    int squadEnt = FindPlayerSquad(client);
    if (squadEnt > 0 && IsValidEntity(squadEnt))
    {
        if (HasEntProp(squadEnt, Prop_Send, "m_hCommander"))
        {
            if (GetEntPropEnt(squadEnt, Prop_Send, "m_hCommander") == client)
            {
                SetEntPropEnt(squadEnt, Prop_Send, "m_hCommander", -1);
            }
        }
    }

    if (HasEntProp(client, Prop_Send, "m_bCommander"))
    {
        SetEntProp(client, Prop_Send, "m_bCommander", 0);
    }

    for (int bot = 1; bot <= MaxClients; bot++)
    {
        if (g_iBotSquadLeader[bot] == client)
        {
            RemoveSquadSprite(bot);
            g_iBotSquadLeader[bot] = 0;
        }
    }
}

public void Event_PlayerSpawn(Event event, const char[] name, bool dontBroadcast)
{
    int client = GetClientOfUserId(event.GetInt("userid"));
    if (client > 0 && IsClientInGame(client))
    {
        // Release the reservation only once the player actually holds the slot.
        // Clearing unconditionally on any spawn would re-open the theft window,
        // because a player can respawn (e.g. round restart) while the engine's
        // AttemptSquadUpdate is still pending.
        if (g_iPendingReservedSlot[client] >= 0)
        {
            int heldSlot = GetPlayerSquadSlotCode(client);
            if (heldSlot == g_iPendingReservedSlot[client])
            {
                ServerCommand("insbot unreserve %d %d",
                    g_iPendingReservedTeam[client], g_iPendingReservedSlot[client]);
                g_iPendingReservedSlot[client] = -1;
                g_iPendingReservedTeam[client] = -1;
            }
        }

        if (g_bSquadActive[client])
        {
            CreateTimer(0.2, Timer_ReapplyLeader, client, TIMER_FLAG_NO_MAPCHANGE);
        }
        else if (g_iBotSquadLeader[client] > 0)
        {
            int leader = g_iBotSquadLeader[client];
            if (leader > 0 && IsClientInGame(leader) && g_bSquadActive[leader])
            {
                CreateTimer(0.3, Timer_DelayedAttach, client, TIMER_FLAG_NO_MAPCHANGE);
            }
        }
    }
}

public Action Timer_ReapplyLeader(Handle timer, any client)
{
    if (client > 0 && IsClientInGame(client) && g_bSquadActive[client])
    {
        PromoteClientToSquadLeader(client);
    }
    return Plugin_Stop;
}

public void Event_PlayerTeam(Event event, const char[] name, bool dontBroadcast)
{
    int client = GetClientOfUserId(event.GetInt("userid"));
    if (client > 0 && client <= MaxClients)
    {
        if (g_bSquadActive[client])
        {
            RemoveCoveringFireMarker(client);
            RemoveDefendMarker(client);
            g_bSquadActive[client] = false;
            DemoteClientFromSquadLeader(client);
            ServerCommand("insbot squad %d 0", client);
        }
        if (g_iPendingReservedSlot[client] >= 0)
        {
            ServerCommand("insbot unreserve %d %d",
                g_iPendingReservedTeam[client], g_iPendingReservedSlot[client]);
            g_iPendingReservedSlot[client] = -1;
            g_iPendingReservedTeam[client] = -1;
        }
    }
}

public void Event_PlayerDeath(Event event, const char[] name, bool dontBroadcast)
{
    int victim = GetClientOfUserId(event.GetInt("userid"));
    if (victim <= 0 || !IsClientInGame(victim))
        return;

    RemoveSquadSprite(victim);
    RemoveCoveringFireMarker(victim);
    RemoveDefendMarker(victim);

    if (IsFakeClient(victim))
    {
        int leader = g_iBotSquadLeader[victim];
        if (leader > 0 && leader <= MaxClients && IsClientInGame(leader)
            && g_bSquadActive[leader] && leader != victim)
        {
            char victimName[64];
            GetClientName(victim, victimName, sizeof(victimName));
            PrintToChat(leader, "[Squad] Squadmate %s was killed in action!", victimName);
        }
        g_iBotSquadLeader[victim] = 0;
    }
}

public Action Command_SquadRecruit(int args)
{
    if (args >= 4)
    {
        char sLeader[16], sBot[16], sName[64], sRole[64];
        GetCmdArg(1, sLeader, sizeof(sLeader));
        GetCmdArg(2, sBot, sizeof(sBot));
        GetCmdArg(3, sName, sizeof(sName));
        GetCmdArg(4, sRole, sizeof(sRole));

        int leader = StringToInt(sLeader);
        int bot = StringToInt(sBot);

        if (leader > 0 && leader <= MaxClients && IsClientInGame(leader))
        {
            PrintToChat(leader, "[Squad] %s joined as %s.", sName, sRole);
            if (bot > 0 && bot <= MaxClients && IsClientInGame(bot))
            {
                if (g_cvCustomSquadSprite.BoolValue)
                    AttachSquadGlow(bot, leader);
            }
        }
    }
    return Plugin_Handled;
}

public Action Command_SquadLeaderDown(int args)
{
    if (args >= 1)
    {
        char sLeader[16];
        GetCmdArg(1, sLeader, sizeof(sLeader));
        int leader = StringToInt(sLeader);
        if (leader > 0 && leader <= MaxClients && IsClientInGame(leader))
        {
            PrintToChat(leader, "[Squad] Leader down — fireteam holding positions until you respawn.");
        }
    }
    return Plugin_Handled;
}
public Action Command_Squad(int client, int args)
{
    if (client <= 0 || !IsClientInGame(client))
        return Plugin_Handled;

    g_bSquadActive[client] = !g_bSquadActive[client];

    if (g_bSquadActive[client])
    {
        PromoteClientToSquadLeader(client);
        ServerCommand("insbot squad %d 1", client);
        PrintToChat(client, "[Squad] Fireteam escort ACTIVATED. Squad Leader promotion granted.");
    }
    else
    {
        DemoteClientFromSquadLeader(client);
        ServerCommand("insbot squad %d 0", client);
        PrintToChat(client, "[Squad] Fireteam escort DISMISSED.");
    }

    return Plugin_Handled;
}

public Action Command_Dismiss(int client, int args)
{
    if (client <= 0 || !IsClientInGame(client))
        return Plugin_Handled;

    RemoveCoveringFireMarker(client);
    RemoveDefendMarker(client);
    g_bSquadActive[client] = false;
    DemoteClientFromSquadLeader(client);
    ServerCommand("insbot squad %d 0", client);
    PrintToChat(client, "[Squad] Fireteam escort DISMISSED.");
    return Plugin_Handled;
}
// Replaces the old Event_CommsWaypoint hook (comms_waypoint is not a real INS event).
// !mark traces the player's eye-ray to the world and stores the hit position as
// the covering-fire target. Only useful when Covering Fire was recently selected.
public Action Command_MarkTarget(int client, int args)
{
    if (client <= 0 || !IsClientInGame(client) || IsFakeClient(client))
        return Plugin_Handled;

    if (!g_bSquadActive[client])
    {
        PrintToChat(client, "[Squad] You need an active squad first. Type !squad.");
        return Plugin_Handled;
    }

    float eyePos[3], eyeAng[3], endPos[3];
    GetClientEyePosition(client, eyePos);
    GetClientEyeAngles(client, eyeAng);

    Handle trace = TR_TraceRayFilterEx(eyePos, eyeAng, MASK_SOLID, RayType_Infinite, TraceFilter_DontHitSelf, client);
    if (TR_DidHit(trace))
    {
        int surfFlags = TR_GetSurfaceFlags(trace);
        if (!(surfFlags & (SURF_SKY | SURF_SKY2D)))
        {
            TR_GetEndPosition(endPos, trace);
            g_fWaypointPos[client][0] = endPos[0];
            g_fWaypointPos[client][1] = endPos[1];
            g_fWaypointPos[client][2] = endPos[2];
            g_bHasWaypoint[client] = true;
            PrintToChat(client, "[Squad] Target marked at %.0f %.0f %.0f. Select Covering Fire to use it.",
                endPos[0], endPos[1], endPos[2]);
        }
        else
        {
            PrintToChat(client, "[Squad] No valid surface in range to mark. Aim at a wall or floor.");
        }
    }
    else
    {
        PrintToChat(client, "[Squad] No surface in range to mark. Aim at a wall or floor.");
    }
    CloseHandle(trace);
    return Plugin_Handled;
}

public bool TraceFilter_DontHitSelf(int entity, int contentsMask, any data)
{
    return entity != data;
}

// Hide the squad sprite from everyone except the squad leader.
public Action Hook_SpriteSetTransmit(int entity, int client)
{
    for (int i = 1; i <= MaxClients; i++)
    {
        if (g_iSquadSprite[i] == entity)
        {
            int leader = g_iBotSquadLeader[i];
            if (client == leader)
                return Plugin_Continue;   // leader sees it
            return Plugin_Handled;        // everyone else doesn't
        }
    }
    return Plugin_Continue;
}

public Action OnCommsKey(int client, const char[] command, int argc)
{
    if (client <= 0 || !IsClientInGame(client) || IsFakeClient(client))
        return Plugin_Continue;

    ShowSquadOrdersMenu(client);
    return Plugin_Handled;
}


// ============================================================================
// Dual-Layer 3D Target Marker & Tactical Hint System (Covering Fire)
// ============================================================================
stock void SendNativeWaypoint(int client, int orderId, const float pos[3])
{
    if (client <= 0 || !IsClientInGame(client) || IsFakeClient(client))
        return;

    Handle msg = StartMessageOne("UnitOrder", client);
    if (msg != INVALID_HANDLE)
    {
        BfWriteNum(msg, orderId);
        BfWriteFloat(msg, pos[0]);
        BfWriteFloat(msg, pos[1]);
        BfWriteFloat(msg, pos[2]);
        EndMessage();
    }
}

void ClearNativeWaypoint(int client)
{
    if (client <= 0 || !IsClientInGame(client) || IsFakeClient(client))
        return;

    Handle msg = StartMessageOne("UnitOrder", client);
    if (msg != INVALID_HANDLE)
    {
        BfWriteNum(msg, -1); // -1 shuts off waypoint indicator
        BfWriteFloat(msg, 0.0);
        BfWriteFloat(msg, 0.0);
        BfWriteFloat(msg, 0.0);
        EndMessage();
    }
}

void SpawnCoveringFireMarker(int client, const float target[3], float duration = 5.0)
{
    if (client <= 0 || !IsClientInGame(client) || IsFakeClient(client))
        return;

    RemoveCoveringFireMarker(client, false);

    g_fCoverFireTarget[client][0] = target[0];
    g_fCoverFireTarget[client][1] = target[1];
    g_fCoverFireTarget[client][2] = target[2];
    g_flCoverFireEndTime[client] = GetGameTime() + duration;

    float pos[3];
    pos[0] = target[0];
    pos[1] = target[1];
    pos[2] = target[2] + 8.0; // lift +8u off surface to prevent geometry clipping

    int anchor = CreateEntityByName("info_target");
    if (anchor > 0)
    {
        DispatchSpawn(anchor);
        TeleportEntity(anchor, pos, NULL_VECTOR, NULL_VECTOR);
        g_iCoverFireMarkerAnchor[client] = anchor;
    }

    // 1. Spawn 3D tactical attack marker entity on physical target surface (materials/hud/indicators/attack.vmt)
    int ent = CreateEntityByName("env_sprite");
    if (ent > 0)
    {
        DispatchKeyValue(ent, "model", "materials/hud/indicators/attack.vmt");
        DispatchKeyValue(ent, "rendercolor", "255 255 255");
        DispatchKeyValue(ent, "renderamt", "255");
        DispatchKeyValue(ent, "rendermode", "2");          // kRenderTransTexture (translucent texture with alpha channel)
        DispatchKeyValue(ent, "scale", "0.30");           // calibrated 64x128 tactical strike chevron
        DispatchKeyValue(ent, "spawnflags", "1");         // SF_SPRITE_STARTON

        if (DispatchSpawn(ent))
        {
            TeleportEntity(ent, pos, NULL_VECTOR, NULL_VECTOR);

            if (anchor > 0 && IsValidEntity(anchor))
            {
                SetVariantString("!activator");
                AcceptEntityInput(ent, "SetParent", anchor);
            }

            g_iCoverFireMarker[client] = ent;
            SDKHook(ent, SDKHook_SetTransmit, Hook_CoverFireMarkerSetTransmit);

            // Visual beam pulse on mark placement (tactical red)
            if (g_iBeamModel > 0)
            {
                TE_SetupBeamRingPoint(pos, 10.0, 60.0, g_iBeamModel, 0, 0, 15, 0.4, 4.0, 0.0, {255, 60, 60, 220}, 10, 0);
                TE_SendToClient(client);
            }
        }
    }

    // 2. Launch repeating 0.2s timer for duration tracking and automatic cleanup
    g_hCoverFireMarkerTimer[client] = CreateTimer(0.1, Timer_UpdateCoveringFireMarker, GetClientUserId(client), TIMER_REPEAT | TIMER_FLAG_NO_MAPCHANGE);
}

public Action Hook_CoverFireMarkerSetTransmit(int entity, int client)
{
    for (int i = 1; i <= MaxClients; i++)
    {
        if (g_iCoverFireMarker[i] == entity)
        {
            if (client == i)
                return Plugin_Continue;   // Only squad leader sees the target marker
            return Plugin_Handled;        // Hidden from enemies and other players
        }
    }
    return Plugin_Continue;
}

public Action Timer_UpdateCoveringFireMarker(Handle timer, any userid)
{
    int client = GetClientOfUserId(userid);
    if (client <= 0 || !IsClientInGame(client) || !IsPlayerAlive(client) || !g_bSquadActive[client])
    {
        if (client > 0 && client <= MaxClients)
        {
            g_hCoverFireMarkerTimer[client] = INVALID_HANDLE;
            RemoveCoveringFireMarker(client, false);
        }
        return Plugin_Stop;
    }

    float remaining = g_flCoverFireEndTime[client] - GetGameTime();
    if (remaining <= 0.0)
    {
        g_hCoverFireMarkerTimer[client] = INVALID_HANDLE;
        RemoveCoveringFireMarker(client, true);
        return Plugin_Stop;
    }

    // Dynamic distance scaling: keep tactical chevron constant HUD-like screen height
    int ent = g_iCoverFireMarker[client];
    if (ent > 0 && IsValidEntity(ent))
    {
        float eyePos[3];
        GetClientEyePosition(client, eyePos);
        float dist = GetVectorDistance(eyePos, g_fCoverFireTarget[client]);

        // Base scale 0.30 at <=300 units; scales up linearly with distance
        float dynamicScale = 0.30 * (dist / 300.0);
        if (dynamicScale < 0.30) dynamicScale = 0.30;
        if (dynamicScale > 3.50) dynamicScale = 3.50;

        SetVariantFloat(dynamicScale);
        AcceptEntityInput(ent, "SetScale");
    }

    return Plugin_Continue;
}


void RemoveCoveringFireMarker(int client, bool notifyEnded = false)
{
    if (client <= 0 || client > MaxClients)
        return;

    if (g_hCoverFireMarkerTimer[client] != INVALID_HANDLE)
    {
        KillTimer(g_hCoverFireMarkerTimer[client]);
        g_hCoverFireMarkerTimer[client] = INVALID_HANDLE;
    }

    g_flCoverFireEndTime[client] = 0.0;

    if (g_iCoverFireMarker[client] > 0 && IsValidEntity(g_iCoverFireMarker[client]))
    {
        int ent = g_iCoverFireMarker[client];
        g_iCoverFireMarker[client] = 0;
        SDKUnhook(ent, SDKHook_SetTransmit, Hook_CoverFireMarkerSetTransmit);
        AcceptEntityInput(ent, "Kill");
    }
    g_iCoverFireMarker[client] = 0;

    if (g_iCoverFireMarkerAnchor[client] > 0 && IsValidEntity(g_iCoverFireMarkerAnchor[client]))
    {
        int anchor = g_iCoverFireMarkerAnchor[client];
        g_iCoverFireMarkerAnchor[client] = 0;
        AcceptEntityInput(anchor, "Kill");
    }
    g_iCoverFireMarkerAnchor[client] = 0;

    if (IsClientInGame(client))
    {
        ClearNativeWaypoint(client);

        if (notifyEnded && IsPlayerAlive(client))
        {
            PrintToChat(client, "[Squad] Covering fire ended.");
        }
    }
}


// ============================================================================
// 3D Waypoint & Strongpoint Defend Marker (Advance & Defend)
// ============================================================================
void SpawnDefendMarker(int client, const float target[3])
{
    if (client <= 0 || !IsClientInGame(client) || IsFakeClient(client))
        return;

    RemoveDefendMarker(client);
    RemoveCoveringFireMarker(client);

    g_fDefendMarkerTarget[client][0] = target[0];
    g_fDefendMarkerTarget[client][1] = target[1];
    g_fDefendMarkerTarget[client][2] = target[2];

    float pos[3];
    pos[0] = target[0];
    pos[1] = target[1];
    pos[2] = target[2] + 8.0; // lift +8u

    int anchor = CreateEntityByName("info_target");
    if (anchor > 0)
    {
        DispatchSpawn(anchor);
        TeleportEntity(anchor, pos, NULL_VECTOR, NULL_VECTOR);
        g_iDefendMarkerAnchor[client] = anchor;
    }

    int ent = CreateEntityByName("env_sprite");
    if (ent > 0)
    {
        DispatchKeyValue(ent, "model", "materials/hud/indicators/defend.vmt");
        DispatchKeyValue(ent, "rendercolor", "255 255 255");
        DispatchKeyValue(ent, "renderamt", "255");
        DispatchKeyValue(ent, "rendermode", "2");          // kRenderTransTexture
        DispatchKeyValue(ent, "scale", "0.35");
        DispatchKeyValue(ent, "spawnflags", "1");         // SF_SPRITE_STARTON

        if (DispatchSpawn(ent))
        {
            TeleportEntity(ent, pos, NULL_VECTOR, NULL_VECTOR);

            if (anchor > 0 && IsValidEntity(anchor))
            {
                SetVariantString("!activator");
                AcceptEntityInput(ent, "SetParent", anchor);
            }

            g_iDefendMarker[client] = ent;
            SDKHook(ent, SDKHook_SetTransmit, Hook_DefendMarkerSetTransmit);

            // Visual beam pulse on mark placement (defensive cyan/blue)
            if (g_iBeamModel > 0)
            {
                TE_SetupBeamRingPoint(pos, 10.0, 60.0, g_iBeamModel, 0, 0, 15, 0.4, 4.0, 0.0, {60, 180, 255, 220}, 10, 0);
                TE_SendToClient(client);
            }
        }
    }

    g_hDefendMarkerTimer[client] = CreateTimer(0.1, Timer_UpdateDefendMarker, GetClientUserId(client), TIMER_REPEAT | TIMER_FLAG_NO_MAPCHANGE);
}

public Action Hook_DefendMarkerSetTransmit(int entity, int client)
{
    for (int i = 1; i <= MaxClients; i++)
    {
        if (g_iDefendMarker[i] == entity)
        {
            if (client == i)
                return Plugin_Continue;   // Only squad leader sees the defend marker
            return Plugin_Handled;        // Hidden from others
        }
    }
    return Plugin_Continue;
}

public Action Timer_UpdateDefendMarker(Handle timer, any userid)
{
    int client = GetClientOfUserId(userid);
    if (client <= 0 || !IsClientInGame(client) || !IsPlayerAlive(client) || !g_bSquadActive[client])
    {
        if (client > 0 && client <= MaxClients)
        {
            g_hDefendMarkerTimer[client] = INVALID_HANDLE;
            RemoveDefendMarker(client);
        }
        return Plugin_Stop;
    }

    // Dynamic distance scaling: keep shield constant screen height
    int ent = g_iDefendMarker[client];
    if (ent > 0 && IsValidEntity(ent))
    {
        float eyePos[3];
        GetClientEyePosition(client, eyePos);
        float dist = GetVectorDistance(eyePos, g_fDefendMarkerTarget[client]);

        float dynamicScale = 0.35 * (dist / 300.0);
        if (dynamicScale < 0.35) dynamicScale = 0.35;
        if (dynamicScale > 3.50) dynamicScale = 3.50;

        SetVariantFloat(dynamicScale);
        AcceptEntityInput(ent, "SetScale");
    }

    return Plugin_Continue;
}

void RemoveDefendMarker(int client)
{
    if (client <= 0 || client > MaxClients)
        return;

    if (g_hDefendMarkerTimer[client] != INVALID_HANDLE)
     {
        KillTimer(g_hDefendMarkerTimer[client]);
        g_hDefendMarkerTimer[client] = INVALID_HANDLE;
    }

    if (g_iDefendMarker[client] > 0 && IsValidEntity(g_iDefendMarker[client]))
    {
        int ent = g_iDefendMarker[client];
        g_iDefendMarker[client] = 0;
        SDKUnhook(ent, SDKHook_SetTransmit, Hook_DefendMarkerSetTransmit);
        AcceptEntityInput(ent, "Kill");
    }
    g_iDefendMarker[client] = 0;

    if (g_iDefendMarkerAnchor[client] > 0 && IsValidEntity(g_iDefendMarkerAnchor[client]))
    {
        int anchor = g_iDefendMarkerAnchor[client];
        g_iDefendMarkerAnchor[client] = 0;
        AcceptEntityInput(anchor, "Kill");
    }
    g_iDefendMarkerAnchor[client] = 0;
}

public Action Command_SquadOrders(int client, int args)
{
    ShowSquadOrdersMenu(client);
    return Plugin_Handled;
}

void ShowSquadOrdersMenu(int client)
{
    if (client <= 0 || !IsClientInGame(client) || IsFakeClient(client))
        return;

    Menu menu = new Menu(SquadOrdersMenuHandler);

    if (!g_bSquadActive[client])
    {
        // No active squad: offer to recruit one
        menu.SetTitle("Squad System");
        menu.AddItem("R", "Recruit Fireteam (3 bots)");
    }
    else
    {
        menu.SetTitle("Squad Orders");
        menu.AddItem("1", "Move Out");
        menu.AddItem("2", "Defend / Hold Position");
        menu.AddItem("3", "Need Backup");
        menu.AddItem("4", "Covering Fire");
        menu.AddItem("5", "Advance & Defend");
        menu.AddItem("0", "Dismiss Squad");
    }

    menu.Display(client, 20);
}

public int SquadOrdersMenuHandler(Menu menu, MenuAction action, int client, int item)
{
    if (action == MenuAction_Select)
    {
        char info[8];
        menu.GetItem(item, info, sizeof(info));

        // Recruit option (shown when no squad active)
        if (StrEqual(info, "R"))
        {
            g_bSquadActive[client] = true;
            PromoteClientToSquadLeader(client);
            ServerCommand("insbot squad %d 1", client);
            PrintToChat(client, "[Squad] Fireteam escort ACTIVATED. Squad Leader promotion granted.");
            return 0;
        }

        int orderId = StringToInt(info);

        if (orderId == 0)
        {
            RemoveCoveringFireMarker(client);
            RemoveDefendMarker(client);
            g_bSquadActive[client] = false;
            DemoteClientFromSquadLeader(client);
            ServerCommand("insbot squad %d 0", client);
            PrintToChat(client, "[Squad] Fireteam dismissed.");
        }
        else if (orderId == 4)
        {
            // Auto-mark: trace eye-ray NOW, use hit position as the covering-fire target
            float eyePos[3], eyeAng[3], endPos[3];
            GetClientEyePosition(client, eyePos);
            GetClientEyeAngles(client, eyeAng);

            Handle trace = TR_TraceRayFilterEx(eyePos, eyeAng, MASK_SOLID, RayType_Infinite, TraceFilter_DontHitSelf, client);
            bool hasMark = false;
            if (TR_DidHit(trace))
            {
                int surfFlags = TR_GetSurfaceFlags(trace);
                if (!(surfFlags & (SURF_SKY | SURF_SKY2D)))
                {
                    TR_GetEndPosition(endPos, trace);
                    g_fWaypointPos[client][0] = endPos[0];
                    g_fWaypointPos[client][1] = endPos[1];
                    g_fWaypointPos[client][2] = endPos[2];
                    g_bHasWaypoint[client] = true;
                    hasMark = true;
                }
            }
            CloseHandle(trace);

            if (hasMark)
            {
                RemoveDefendMarker(client);
                ServerCommand("insbot squad %d %d %f %f %f", client, orderId,
                    g_fWaypointPos[client][0], g_fWaypointPos[client][1], g_fWaypointPos[client][2]);
                PrintToChat(client, "[Squad] Covering Fire at marked position (%.0f %.0f %.0f)",
                    g_fWaypointPos[client][0], g_fWaypointPos[client][1], g_fWaypointPos[client][2]);
                SpawnCoveringFireMarker(client, g_fWaypointPos[client], 5.0);
            }
            else
            {
                PrintToChat(client, "[Squad] No valid target. Aim at a surface and try again.");
            }
        }
        else if (orderId == 5)
        {
            float eyePos[3], eyeAng[3], endPos[3];
            GetClientEyePosition(client, eyePos);
            GetClientEyeAngles(client, eyeAng);

            Handle trace = TR_TraceRayFilterEx(eyePos, eyeAng, MASK_SOLID, RayType_Infinite, TraceFilter_DontHitSelf, client);
            bool hasMark = false;
            if (TR_DidHit(trace))
            {
                int surfFlags = TR_GetSurfaceFlags(trace);
                if (!(surfFlags & (SURF_SKY | SURF_SKY2D)))
                {
                    TR_GetEndPosition(endPos, trace);
                    g_fWaypointPos[client][0] = endPos[0];
                    g_fWaypointPos[client][1] = endPos[1];
                    g_fWaypointPos[client][2] = endPos[2];
                    g_bHasWaypoint[client] = true;
                    hasMark = true;
                }
            }
            CloseHandle(trace);

            if (hasMark)
            {
                RemoveCoveringFireMarker(client);
                SpawnDefendMarker(client, endPos);
                ServerCommand("insbot squad %d 5 %f %f %f", client, endPos[0], endPos[1], endPos[2]);
                PrintToChat(client, "[Squad] Fireteam advancing to marked waypoint and establishing defensive perimeter.");
            }
            else
            {
                PrintToChat(client, "[Squad] No valid surface in range to mark. Aim at a wall or floor.");
            }
        }
        else
        {
            RemoveCoveringFireMarker(client);
            RemoveDefendMarker(client);
            ServerCommand("insbot squad %d %d", client, orderId);
            char orderName[32];
            switch (orderId)
            {
            case 1: strcopy(orderName, sizeof(orderName), "Move Out");
            case 2: strcopy(orderName, sizeof(orderName), "Defend / Hold");
            case 3: strcopy(orderName, sizeof(orderName), "Need Backup");
            }
            PrintToChat(client, "[Squad] Order: %s", orderName);
        }
    }
    else if (action == MenuAction_End)
    {
        delete menu;
    }
    return 0;
}


// ============================================================================
// Slot Requester Logic
// ============================================================================
int g_iOff_SquadTeam = -1;
int g_iOff_SquadSlots = -1;
int g_iOff_SquadPlayerSlots = -1;

static void InitSquadOffsets()
{
    if (g_iOff_SquadTeam == -1)
    {
        g_iOff_SquadTeam = FindSendPropInfo("CINSSquad", "m_iParentTeam");
        if (g_iOff_SquadTeam == -1) g_iOff_SquadTeam = 860;

        g_iOff_SquadSlots = FindSendPropInfo("CINSSquad", "m_iSlots");
        if (g_iOff_SquadSlots == -1) g_iOff_SquadSlots = 992;

        g_iOff_SquadPlayerSlots = FindSendPropInfo("CINSSquad", "m_iPlayerSlots");
        if (g_iOff_SquadPlayerSlots == -1) g_iOff_SquadPlayerSlots = 1024;
    }
}

// Returns the pfullsetup slot code (5 + squadIndex + slot*4) the client
// currently occupies, or -1 if they are not in any squad slot.
static int GetPlayerSquadSlotCode(int client)
{
    if (client <= 0 || !IsClientInGame(client) || IsFakeClient(client))
        return -1;

    int reqTeam = GetClientTeam(client);
    if (reqTeam < 1)
        return -1;

    int myEnt = client; // SourceMod client index == entity index for players
    InitSquadOffsets();

    int squadIndex = 0;
    for (int i = 1; i <= GetMaxEntities(); i++)
    {
        if (!IsValidEntity(i))
            continue;
        char classname[64];
        if (!GetEntityNetClass(i, classname, sizeof(classname)))
            continue;
        if (!StrEqual(classname, "CINSSquad"))
            continue;

        int team = GetEntData(i, g_iOff_SquadTeam, 4);
        if (team != reqTeam)
            continue;

        for (int s = 0; s < 8; s++)
        {
            int playerId = GetEntData(i, g_iOff_SquadPlayerSlots + (s * 4), 4);
            if (playerId == 0 || playerId == -1)
                continue;
            if ((playerId & 0xFFF) == myEnt)
                return 5 + squadIndex + (s * 4);
        }
        squadIndex++;
    }
    return -1;
}

// Finds a fake client occupying the given (teamSlot, slotCode) seat.
// Returns the client index, or 0 if the seat is empty or held by a human.
static int FindBotInReservedSlot(int teamSlot, int slotCode)
{
    if (slotCode < 5 || slotCode > 34)
        return 0;

    int squadIndex = (slotCode - 5) % 4;
    int seat = (slotCode - 5) / 4;
    int wantTeam = (teamSlot == 0) ? 2 : 1; // teamSlot 0 -> engine team 2

    InitSquadOffsets();
    int seen = 0;
    for (int i = 1; i <= GetMaxEntities(); i++)
    {
        if (!IsValidEntity(i))
            continue;
        char classname[64];
        if (!GetEntityNetClass(i, classname, sizeof(classname)))
            continue;
        if (!StrEqual(classname, "CINSSquad"))
            continue;
        if (GetEntData(i, g_iOff_SquadTeam, 4) != wantTeam)
            continue;
        if (seen != squadIndex)
        {
            seen++;
            continue;
        }

        int playerId = GetEntData(i, g_iOff_SquadPlayerSlots + (seat * 4), 4);
        if (playerId == 0 || playerId == -1)
            return 0;

        int ent = playerId & 0xFFF;
        if (ent > 0 && ent <= MaxClients && IsClientInGame(ent) && IsFakeClient(ent))
            return ent;
        return 0; // a human occupies it: not our problem
    }
    return 0;
}

void GetClassNameFromId(int team, int classId, char[] name, int maxlen)
{
    if (team == 1) // USMC
    {
        switch (classId)
        {
            case 0: strcopy(name, maxlen, "Sergeant");
            case 1: strcopy(name, maxlen, "Rifleman");
            case 2: strcopy(name, maxlen, "Engineer");
            case 3: strcopy(name, maxlen, "Support");
            case 10: strcopy(name, maxlen, "Grenadier");
            case 15: strcopy(name, maxlen, "Marksman");
            default: Format(name, maxlen, "Class %d", classId);
        }
    }
    else if (team == 2) // Iraqi
    {
        switch (classId)
        {
            case 0: strcopy(name, maxlen, "Militant");
            case 1: strcopy(name, maxlen, "Rifleman");
            case 2: strcopy(name, maxlen, "Sapper");
            case 3: strcopy(name, maxlen, "Support");
            case 10: strcopy(name, maxlen, "RPG-7");
            case 15: strcopy(name, maxlen, "Sniper");
            default: Format(name, maxlen, "Class %d", classId);
        }
    }
    else
    {
        switch (classId)
        {
            case 0: strcopy(name, maxlen, "Squad Leader");
            case 1: strcopy(name, maxlen, "Rifleman");
            case 2: strcopy(name, maxlen, "Engineer / Sapper");
            case 3: strcopy(name, maxlen, "Support");
            case 10: strcopy(name, maxlen, "RPG / Grenadier");
            case 15: strcopy(name, maxlen, "Marksman / Sniper");
            default: Format(name, maxlen, "Class %d", classId);
        }
    }
}

void GetSquadDisplayName(int squadIndex, char[] name, int maxlen)
{
    Format(name, maxlen, "Squad %d", squadIndex + 1);
}

public Action Command_SlotRequestMenu(int client, int args)
{
    if (client <= 0 || !IsClientInGame(client) || IsFakeClient(client))
        return Plugin_Handled;

    OpenSlotRequestMenu(client);
    return Plugin_Handled;
}

void OpenSlotRequestMenu(int client)
{
    if (!g_cvSlotRequestEnabled.BoolValue)
    {
        PrintToChat(client, "[SlotRequester] Slot request has been disabled by admin.");
        return;
    }

    int reqTeam = GetClientTeam(client);
    if (reqTeam < 1)
    {
        PrintToChat(client, "[SlotRequester] You must be on a team to request a slot!");
        return;
    }

    InitSquadOffsets();
    Menu menu = new Menu(SlotRequestMenuHandler);
    menu.SetTitle("Select Class (Press #):");

    int availableRoleCount = 0;
    int squadIndex = 0;
    bool roleAdded[8][32];
    for (int sq = 0; sq < 8; sq++)
    {
        for (int cl = 0; cl < 32; cl++)
        {
            roleAdded[sq][cl] = false;
        }
    }

    for (int i = 1; i <= GetMaxEntities(); i++)
    {
        if (!IsValidEntity(i)) continue;

        char classname[64];
        if (!GetEntityNetClass(i, classname, sizeof(classname))) continue;

        if (StrEqual(classname, "CINSSquad"))
        {
            int team = GetEntData(i, g_iOff_SquadTeam, 4);
            if (team != reqTeam) continue;

            char squadName[32];
            GetSquadDisplayName(squadIndex, squadName, sizeof(squadName));

            for (int s = 0; s < 8; s++)
            {
                int playerId = GetEntData(i, g_iOff_SquadPlayerSlots + (s * 4), 4);
                bool isAvailable = false;

                if (playerId != 0 && playerId != -1)
                {
                    int botEnt = playerId & 0xFFF;
                    if (botEnt > 0 && botEnt <= MaxClients && IsClientInGame(botEnt) && IsFakeClient(botEnt))
                    {
                        isAvailable = true;
                    }
                }
                else
                {
                    // Empty slot is open for taking
                    isAvailable = true;
                }

                if (isAvailable)
                {
                    int code = 5 + squadIndex + (s * 4);
                    bool isReservedByOther = false;
                    for (int p = 1; p <= MaxClients; p++)
                    {
                        if (p != client && g_iPendingReservedSlot[p] == code)
                        {
                            isReservedByOther = true;
                            break;
                        }
                    }
                    if (isReservedByOther)
                        continue;

                    int slotClassId = GetEntData(i, g_iOff_SquadSlots + (s * 4), 4);
                    if (slotClassId >= 0 && slotClassId < 32 && !roleAdded[squadIndex][slotClassId])
                    {
                        roleAdded[squadIndex][slotClassId] = true;

                        char className[32];
                        GetClassNameFromId(reqTeam, slotClassId, className, sizeof(className));

                        char info[64];
                        Format(info, sizeof(info), "%d;%d;%s", squadIndex, slotClassId, className);

                        char display[64];
                        Format(display, sizeof(display), "%s - %s", className, squadName);

                        menu.AddItem(info, display);
                        availableRoleCount++;
                    }
                }
            }
            squadIndex++;
        }
    }

    if (availableRoleCount == 0)
    {
        menu.AddItem("", "No slots available on your team", ITEMDRAW_DISABLED);
    }

    // Fit up to 9 items on a single page without splitting key 8 onto page 2
    if (availableRoleCount <= 9)
    {
        menu.Pagination = MENU_NO_PAGINATION;
    }
    menu.ExitButton = true;
    menu.Display(client, 20);
}

public int SlotRequestMenuHandler(Menu menu, MenuAction action, int client, int item)
{
    if (action == MenuAction_Select)
    {
        char info[64];
        menu.GetItem(item, info, sizeof(info));

        if (!info[0])
            return 0;

        char parts[3][32];
        if (ExplodeString(info, ";", parts, 3, sizeof(parts[])) >= 3)
        {
            int targetSquadIndex = StringToInt(parts[0]);
            int targetClassId = StringToInt(parts[1]);
            char className[32];
            strcopy(className, sizeof(className), parts[2]);

            int reqTeam = GetClientTeam(client);
            if (reqTeam < 1)
            {
                PrintToChat(client, "[SlotRequester] You must be on a team to take a slot!");
                return 0;
            }

            InitSquadOffsets();
            // Find matching squad entity
            int squadIndex = 0;
            int squadEnt = -1;
            for (int i = 1; i <= GetMaxEntities(); i++)
            {
                if (!IsValidEntity(i)) continue;
                char classname[64];
                if (!GetEntityNetClass(i, classname, sizeof(classname))) continue;

                if (StrEqual(classname, "CINSSquad"))
                {
                    int team = GetEntData(i, g_iOff_SquadTeam, 4);
                    if (team == reqTeam)
                    {
                        if (squadIndex == targetSquadIndex)
                        {
                            squadEnt = i;
                            break;
                        }
                        squadIndex++;
                    }
                }
            }

            if (squadEnt == -1)
            {
                PrintToChat(client, "[SlotRequester] That squad is no longer available.");
                return 0;
            }

            // Collect empty and bot-occupied slots for this role in the target squad
            int emptySlots[8];
            int emptyCount = 0;
            int botSlots[8];
            int botEnts[8];
            int botCount = 0;

            for (int s = 0; s < 8; s++)
            {
                int slotClassId = GetEntData(squadEnt, g_iOff_SquadSlots + (s * 4), 4);
                if (slotClassId == targetClassId)
                {
                    int code = 5 + targetSquadIndex + (s * 4);
                    bool isReservedByOther = false;
                    for (int p = 1; p <= MaxClients; p++)
                    {
                        if (p != client && g_iPendingReservedSlot[p] == code)
                        {
                            isReservedByOther = true;
                            break;
                        }
                    }
                    if (isReservedByOther)
                        continue;

                    int playerId = GetEntData(squadEnt, g_iOff_SquadPlayerSlots + (s * 4), 4);
                    if (playerId == 0 || playerId == -1)
                    {
                        emptySlots[emptyCount++] = s;
                    }
                    else
                    {
                        int botEnt = playerId & 0xFFF;
                        if (botEnt > 0 && botEnt <= MaxClients && IsClientInGame(botEnt) && IsFakeClient(botEnt))
                        {
                            botSlots[botCount] = s;
                            botEnts[botCount] = botEnt;
                            botCount++;
                        }
                    }
                }
            }

            int chosenSlot = -1;
            int targetBot = 0;

            // Prioritize empty slots first, then randomize among bot slots
            if (emptyCount > 0)
            {
                int pick = (emptyCount == 1) ? 0 : GetRandomInt(0, emptyCount - 1);
                chosenSlot = emptySlots[pick];
            }
            else if (botCount > 0)
            {
                int pick = (botCount == 1) ? 0 : GetRandomInt(0, botCount - 1);
                chosenSlot = botSlots[pick];
                targetBot = botEnts[pick];
            }

            if (chosenSlot != -1)
            {
                int uiTeamIdx = reqTeam - 1;
                int magicNumber = 5 + targetSquadIndex + (chosenSlot * 4);
                char squadName[32];
                GetSquadDisplayName(targetSquadIndex, squadName, sizeof(squadName));

                // teamSlot 1 -> engine team 1 (USMC); teamSlot 0 -> engine team 2
                // (Iraqi). This mirrors TeamSlotToTeamIndex() in bot.cpp.
                int teamSlot = (reqTeam == 1) ? 1 : 0;

                // Drop any previous reservation held by this player first.
                if (g_iPendingReservedSlot[client] >= 0)
                {
                    ServerCommand("insbot unreserve %d %d",
                        g_iPendingReservedTeam[client], g_iPendingReservedSlot[client]);
                    g_iPendingReservedSlot[client] = -1;
                    g_iPendingReservedTeam[client] = -1;
                }

                // Reserve the slot code so insbots cannot re-fill it while the
                // requester is still alive waiting for their next death.
                g_iPendingReservedSlot[client] = magicNumber;
                g_iPendingReservedTeam[client] = teamSlot;
                ServerCommand("insbot reserve %d %d %d",
                    teamSlot, magicNumber, GetClientUserId(client));

                if (targetBot > 0)
                {
                    char botName[64];
                    GetClientName(targetBot, botName, sizeof(botName));
                    if (IsClientInGame(targetBot))
                    {
                        KickClient(targetBot, "Slot requested by player");
                    }

                    // Defer pfullsetup to next frame so the engine fully disconnects the bot
                    // and marks m_players[chosenSlot] == -1 before the join command executes.
                    DataPack pack = new DataPack();
                    pack.WriteCell(GetClientUserId(client));
                    pack.WriteCell(uiTeamIdx);
                    pack.WriteCell(magicNumber);
                    RequestFrame(Frame_ExecuteSlotJoin, pack);

                    PrintToChat(client, "[SlotRequester] Reserved %s in %s (from '%s'). You will switch to it after you die and respawn this round.",
                        className, squadName, botName);
                }
                else
                {
                    char joinCmd[128];
                    Format(joinCmd, sizeof(joinCmd), "pfullsetup %d %d", uiTeamIdx, magicNumber);
                    ClientCommand(client, joinCmd);

                    PrintToChat(client, "[SlotRequester] Reserved %s in %s. You will switch to it after you die and respawn this round.",
                        className, squadName);
                }
            }
            else
            {
                PrintToChat(client, "[SlotRequester] All slots for that role are currently occupied by players.");
            }
        }
    }
    else if (action == MenuAction_End)
    {
        delete menu;
    }
    return 0;
}

public void Frame_ExecuteSlotJoin(DataPack pack)
{
    pack.Reset();
    int userId = pack.ReadCell();
    int uiTeamIdx = pack.ReadCell();
    int magicNumber = pack.ReadCell();
    delete pack;

    int client = GetClientOfUserId(userId);
    if (client > 0 && IsClientInGame(client))
    {
        char joinCmd[128];
        Format(joinCmd, sizeof(joinCmd), "pfullsetup %d %d", uiTeamIdx, magicNumber);
        ClientCommand(client, joinCmd);
    }
}

// Reconciliation guard for the engine's auto-assign path. CPlayTeam::GetRandomSquad
// can place a bot into our reserved (deliberately empty) seat without consulting
// insbots, because an empty seat is a valid target. When that happens we re-assert
// the reservation so the quota skip is re-armed and the race is visible in the log.
public Action Timer_EnforceReservations(Handle timer)
{
    InitSquadOffsets();

    for (int c = 1; c <= MaxClients; c++)
    {
        if (!IsClientInGame(c) || IsFakeClient(c))
            continue;
        if (g_iPendingReservedSlot[c] < 0)
            continue;

        // If the player has already claimed the slot, release and move on.
        if (GetPlayerSquadSlotCode(c) == g_iPendingReservedSlot[c])
        {
            ServerCommand("insbot unreserve %d %d",
                g_iPendingReservedTeam[c], g_iPendingReservedSlot[c]);
            g_iPendingReservedSlot[c] = -1;
            g_iPendingReservedTeam[c] = -1;
            continue;
        }

        // Otherwise: is a BOT now sitting in our reserved seat?
        int intruder = FindBotInReservedSlot(g_iPendingReservedTeam[c], g_iPendingReservedSlot[c]);
        if (intruder > 0)
        {
            KickClient(intruder, "Slot reserved for human player");
            ServerCommand("insbot reserve %d %d %d",
                g_iPendingReservedTeam[c], g_iPendingReservedSlot[c], GetClientUserId(c));
            LogMessage("[SlotRequester] Reserved seat %d was taken by bot %N; evicted intruder and re-asserted reservation.",
                g_iPendingReservedSlot[c], intruder);
        }
    }
    return Plugin_Continue;
}

// ============================================================================
// Version Exposer Helpers
// ============================================================================
void CleanSingleLine(char[] buffer)
{
    TrimString(buffer);
    for (int i = 0; buffer[i] != '\0'; i++)
    {
        if (buffer[i] == '\r' || buffer[i] == '\n')
        {
            buffer[i] = '\0';
            break;
        }
    }
    TrimString(buffer);
}

void UpdateVersions()
{
    char buffer[128];

    // 1. Probe InsBots version
    buffer[0] = '\0';
    ServerCommandEx(buffer, sizeof(buffer), "insbot version_raw");
    CleanSingleLine(buffer);
    if (strlen(buffer) > 0 && StrContains(buffer, "Unknown command") == -1)
    {
        g_cvInsbotVersion.SetString(buffer);
    }

    // 2. Probe Ins Utilities version
    buffer[0] = '\0';
    ServerCommandEx(buffer, sizeof(buffer), "ins_util version_raw");
    CleanSingleLine(buffer);
    if (strlen(buffer) > 0 && StrContains(buffer, "Unknown command") == -1)
    {
        g_cvInsUtilVersion.SetString(buffer);
    }
}
