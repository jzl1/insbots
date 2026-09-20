// ============================================================================
// bot_weapons.cpp - weapon switching, grenades, smoke, M203 and RPG.
// ============================================================================
#include <stdio.h>
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
#include "voice.h"
#include "linux_offsets.h"
#include "bot_internal.h"

// ---- externs this TU needs (same pattern as bot.cpp / objective.cpp) -------
extern IVEngineServer       *engine;
extern IEngineTrace         *enginetrace;
extern IPlayerInfoManager   *playerinfomanager;
extern CGlobalVars          *gpGlobals;
extern IServerPluginHelpers *helpers;
extern int                   s_offset_m_hMyWeapons;
extern int                   s_offset_m_hActiveWeapon;
extern CfgVar                insbot_attack;
extern CfgVar                insbot_debug;
extern CfgVar                insbot_grenade_cooldown;
extern CfgVar                insbot_grenade_gravity;
extern CfgVar                insbot_grenade_speed;
extern CfgVar                insbot_intel_time;
extern CfgVar                insbot_m203;
extern CfgVar                insbot_m203_cooldown;
extern CfgVar                insbot_m203_max_range;
extern CfgVar                insbot_m203_min_range;
extern CfgVar                insbot_m203_obj;
extern CfgVar                insbot_m203_obj_chance;
extern CfgVar                insbot_m203_speed;
extern CfgVar                insbot_smoke;
extern CfgVar                insbot_smoke_chance;
extern CfgVar                insbot_smoke_cooldown;
extern CfgVar                insbot_smoke_group;
extern CfgVar                insbot_smoke_group_chance;
extern CfgVar                insbot_smoke_group_min;
extern CfgVar                insbot_smoke_group_radius;
extern CfgVar                insbot_smoke_range;
extern CfgVar                insbot_smoke_scatter;
extern CfgVar                insbot_smoke_weapon;

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
