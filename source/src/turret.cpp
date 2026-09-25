// ============================================================================
// turret.cpp - the Humvee auto-turret subsystem.
//
// A self-contained subsystem that BORROWS PATTERNS from bot_perceive.cpp and
// bot_think.cpp rather than sharing code with CInsBot. CInsBot is welded to
// fake clients (m_info, m_ctrl, RunPlayerMove, squad, nav, loadout); a turret
// has no client slot, so sharing would become "if turret" branches throughout
// the bot. See design spec section 4 for the full argument.
//
// ---- BORROWED INVARIANTS ----------------------------------------------
// These mirror insbots logic. If you change one side, change both.
//   target stickiness 0.64x      <- bot_perceive.cpp:611
//   sight mask MASK_SHOT minus
//     CONTENTS_HITBOX|WINDOW     <- bot_perceive.cpp:392
//   origin-stack rejection       <- bot_perceive.cpp:569
//   chest/torso/head fractions   <- bot_perceive.cpp:353
// -----------------------------------------------------------------------
// ============================================================================
#include "turret.h"
#include "turret_math.h"
#include "turret_abi.h"
#include "cfgvar.h"

#include "engine/iserverplugin.h"
#include "engine/IEngineTrace.h"
#include "engine/IEngineSound.h"
#include "shake.h"                 // ShakeCommand_t, forward-declared by IEffects.h
#include "IEffects.h"              // game_shared/, not engine/
#include "beam_flags.h"            // FBEAM_FADEOUT
#include "irecipientfilter.h"      // IRecipientFilter (voice.cpp:18 does the same)
#include "dlls/iplayerinfo.h"
#include "dt_send.h"
#include "server_class.h"
#include "iservernetworkable.h"    // IServerNetworkable::GetServerClass()
#include "iserverentity.h"         // IServerEntity::GetModelName()
#include "string_t.h"              // STRING()
#include "edict.h"
#include "const.h"                 // ENT_ENTRY_MASK, INVALID_EHANDLE_INDEX
#include "tier0/dbg.h"
#include "engine/ivmodelinfo.h"    // IVModelInfo::GetStudiomodel, for the pose lookup
#include "studio.h"                // studiohdr_t / mstudioposeparamdesc_t (ivmodelinfo.h only forward-declares)

#include <string.h>
#include <stdlib.h>      // rand(), atoi(), atof()
#include <stdio.h>
#include <math.h>

// ---- engine globals owned by insbots.cpp -----------------------------------
extern IVEngineServer      *engine;
extern IVModelInfo         *modelinfo;       // defined in insbots.cpp; optional
extern IPlayerInfoManager  *playerinfomanager;
extern IEngineTrace        *enginetrace;
extern CGlobalVars         *gpGlobals;
extern IEngineSound        *enginesound;      // defined in voice.cpp
extern IServerGameDLL      *server;           // for GetAllServerClasses()
extern int                  FindSendPropOffset( SendTable *, const char * );

// CfgVars that live in insbots.cpp and are read by the turret. Declared here
// the way bot_perceive.cpp:45-47 declares the ones it borrows.
extern CfgVar insbot_vis_range;
extern CfgVar insbot_fov;

// Shared smoke-occlusion predicate, defined in bot/bot_perceive.cpp and
// declared in bot.h. Declared here rather than including bot.h so this TU does
// not drag in the whole CInsBot surface.
extern bool LineGoesThroughSmoke( const Vector &from, const Vector &to );

int g_TurretOffsetDriver        = -1;
int g_TurretOffsetPoseParameter = -1;

CTurretRegistry g_Turrets;

// ---------------------------------------------------------------------------
// Forward declarations.
//
// The firing block (below) is deliberately kept together - gate, burst, trace,
// damage and the four effects are one story - and it sits above the perception
// helpers it borrows from. These declarations let that ordering stand without
// splitting the block apart.
// ---------------------------------------------------------------------------
static ICollideable   *TurretCollideable( CTurret *t );
static Vector          TurretMuzzlePosition( CTurret *t );
static QAngle          TurretMuzzleAngles( CTurret *t );
static void            TurretApplyDamage( CTurret *t, int targetIdx,
                                          const Vector &dir, float dmg );
static void           *TurretGetVtableSlot( void *pObject, int slot );

// ---------------------------------------------------------------------------
// Trace filter for the turret's own rays. DEFINED HERE, above Fire(), because
// the firing trace constructs one by value and an incomplete type cannot be
// instantiated.
//
// CTraceFilterSimple (game_shared/util_shared.cpp:273) is NOT usable from a
// standalone server plugin: its ShouldHitEntity calls CBaseEntity::ShouldCollide
// and g_pGameRules->ShouldCollide, and a VSP cannot link game-DLL symbols.
//
// The one rule that actually matters here is design section 6.4: the turret
// must never hit its own vehicle, and must never shoot the player riding in it.
// Boarding moves a player to COLLISION_GROUP_IN_VEHICLE (player.cpp:4804) and
// CGameRules::ShouldCollide returns false unconditionally whenever either side
// carries that group (game_shared/gamerules.cpp:606), which is what makes a
// rider untraceable to the engine. CTraceFilterSkipSelf would bypass all of
// that - it returns `pEntity != m_self` and consults nothing - so it must not
// be reused here.
//
// Rather than dereferencing arbitrary IHandleEntity pointers inside the filter
// (the engine hands it entities of unknown concrete type), the handles to skip
// are collected UP FRONT from edicts - which is safe - and the filter itself
// only ever does pointer comparisons.
// ---------------------------------------------------------------------------
#define INS_TURRET_SKIP_MAX 70        // the vehicle + up to 66 clients + slack

class CTurretTraceFilter : public CTraceFilter
{
public:
    CTurretTraceFilter() : m_n(0) {}

    void Add( IHandleEntity *h )
    {
        if (h && m_n < INS_TURRET_SKIP_MAX) m_skip[m_n++] = h;
    }

    virtual bool ShouldHitEntity( IHandleEntity *pEntity, int /*contentsMask*/ )
    {
        if (!pEntity) return false;
        for (int i = 0; i < m_n; i++)
            if (pEntity == m_skip[i]) return false;
        return true;
    }

private:
    IHandleEntity *m_skip[INS_TURRET_SKIP_MAX];
    int            m_n;
};

static void TurretBuildSkipSet( CTurret *t, CTurretTraceFilter &filter );

// ---------------------------------------------------------------------------
// The single source of truth for "is this entity a driveable Humvee".
//
// GetServerClass()->GetName() returns the C++ class name
// ("CPropVehicleDriveable"), NOT the map-facing classname
// ("prop_vehicle_driveable"). Measured live on the Linux VM: the design spec
// used the map classname and was wrong. Keep every call site going through
// this helper so the string is never duplicated again.
static bool IsDriveableClass( const char *cls )
{
    return cls && strcmp( cls, "CPropVehicleDriveable" ) == 0;
}

// ---------------------------------------------------------------------------
// Per-turret overrides. Follows the existing per-weapon-class pattern
// (NewCombatCfg, bot_loadout.cpp:512): the lookup falls back to the global
// CfgVar. Entindex is the ONLY identifier - a registry ordinal would silently
// shift when a Humvee is removed.
// ---------------------------------------------------------------------------
#define INS_TURRET_MAX_OVERRIDES 128

struct TurretOverride
{
    int         entindex;
    char        name[64];
    float       value;
    bool        used;
};

static TurretOverride s_overrides[INS_TURRET_MAX_OVERRIDES];

static void TurretSetOverride( int entindex, const char *name, float value )
{
    // Replace an existing entry for this (entindex, name) pair first.
    for (int i = 0; i < INS_TURRET_MAX_OVERRIDES; i++)
    {
        if (!s_overrides[i].used) continue;
        if (s_overrides[i].entindex != entindex) continue;
        if (_stricmp( s_overrides[i].name, name ) != 0) continue;
        s_overrides[i].value = value;
        return;
    }
    for (int i = 0; i < INS_TURRET_MAX_OVERRIDES; i++)
    {
        if (s_overrides[i].used) continue;
        s_overrides[i].used = true;
        s_overrides[i].entindex = entindex;
        strncpy( s_overrides[i].name, name, sizeof(s_overrides[i].name) - 1 );
        s_overrides[i].name[sizeof(s_overrides[i].name) - 1] = 0;
        s_overrides[i].value = value;
        return;
    }
}

// Returns the per-turret value if one exists, else the global.
static float TurretValue( CTurret *t, const char *name, float global )
{
    for (int i = 0; i < INS_TURRET_MAX_OVERRIDES; i++)
    {
        if (!s_overrides[i].used) continue;
        if (s_overrides[i].entindex != t->EntIndex()) continue;
        if (_stricmp( s_overrides[i].name, name ) != 0) continue;
        return s_overrides[i].value;
    }
    return global;
}

// Entindex is not stable across a level, so an override must never outlive one.
// Called from the same place the registry is cleared.
void TurretOverridesClear()
{
    for (int i = 0; i < INS_TURRET_MAX_OVERRIDES; i++)
        s_overrides[i].used = false;
}

// The Humvee's model, used to tell OUR vehicle from the other things that share
// CPropVehicleDriveable. That class is not Humvee-specific: the stock buggy,
// the civilian cars and the M939 all spawn as `prop_vehicle_driveable`, and a
// map can contain any of them. Registering by class alone would silently arm a
// turret on every parked civilian car on the map.
//
// The comparison is a SUFFIX match on the model path because the engine stores
// the full path ("models/vehicles/humvee_vehicle.mdl"); matching the tail keeps
// it working if the model moves between search paths.
#define INS_TURRET_HUMVEE_MODEL_TAIL "humvee_vehicle.mdl"

static bool IsHumveeModel( const char *model )
{
    if (!model || !*model) return false;
    size_t n = strlen( model );
    size_t t = sizeof( INS_TURRET_HUMVEE_MODEL_TAIL ) - 1;   // excludes the NUL
    if (n < t) return false;
    // Case-insensitive: model paths are lower-case in this mod, but a repack
    // could differ in case and the engine treats paths case-insensitively.
    return _stricmp( model + (n - t), INS_TURRET_HUMVEE_MODEL_TAIL ) == 0;
}

// Read an entity's model path through the public IServerEntity interface.
// CBaseEntity implements IServerEntity (dlls/baseentity.h:303) and IServerEntity
// derives from IServerUnknown (public/iserverentity.h:25), so the
// IServerUnknown we already hold can be cast to it.
static const char *EntityModelName( edict_t *e )
{
    if (!e || e->IsFree()) return 0;
    IServerUnknown *unk = e->GetUnknown();
    if (!unk) return 0;
    IServerEntity *se = (IServerEntity *)unk;
    return STRING( se->GetModelName() );
}

// ---- CfgVars ---------------------------------------------------------------
// Reached as `insbot set turret_damage 50`. CfgVar, never ConVar: see
// cfgvar.h. Defaults are copied from design spec section 5.6.
CfgVar insbot_turret_enabled      ("turret_enabled",      "1",    "master switch for the Humvee auto-turret (0=off)");
CfgVar insbot_turret_aim_speed    ("turret_aim_speed",    "90",   "turret traverse speed (deg/s)");
CfgVar insbot_turret_aim_error    ("turret_aim_error",    "4.0",  "turret aim error in degrees (0=perfect)");
CfgVar insbot_turret_engage_range ("turret_engage_range", "1500", "turret firing gate distance (units)");
CfgVar insbot_turret_damage       ("turret_damage",       "50",   "turret damage per bullet (real M249 is 151.2; see design section 15)");
CfgVar insbot_turret_burst_time   ("turret_burst_time",   "0.30", "turret firing burst length (s)");
CfgVar insbot_turret_burst_pause  ("turret_burst_pause",  "0.45", "turret pause between bursts (s)");
CfgVar insbot_turret_scan_speed   ("turret_scan_speed",   "20",   "turret idle scan speed (deg/s)");

// Fire-gate tolerances and spread. The design calls these tunable but not
// part of the core eight; they are CfgVars all the same so a playtest can
// adjust them without a rebuild.
CfgVar insbot_turret_fire_tol_yaw   ("turret_fire_tolerance_yaw",   "5.0",   "turret fire gate: max yaw error (deg)");
CfgVar insbot_turret_fire_tol_pitch ("turret_fire_tolerance_pitch", "4.0",   "turret fire gate: max pitch error (deg)");
CfgVar insbot_turret_spread         ("turret_spread",               "0.230", "turret bullet spread cone (deg; M249 real value)");

// ---------------------------------------------------------------------------
CTurret::CTurret()
{
    m_entIndex = 0;
    m_team = 0;
    m_armed = false;
    m_validated = false;
    m_target = 0;
    m_targetLastSeen = 0.0f;
    m_targetLastPos.Init();
    m_curYaw = 0.0f;
    m_curPitch = 0.0f;
    m_desiredYaw = 0.0f;
    m_desiredPitch = 0.0f;
    m_nextAcquire = 0.0f;
    m_burstUntil = 0.0f;
    m_pauseUntil = 0.0f;
    m_scanBearing = 0.0f;
    m_aimErrorYaw = 0.0f;
    m_aimErrorPitch = 0.0f;
    m_residualYaw = 0.0f;
    m_residualPitch = 0.0f;
    m_poseYaw = -1;
    m_posePitch = -1;
    m_state = TURRET_IDLE;
}

void CTurret::Reset( int entindex, int team )
{
    m_entIndex = entindex;
    m_team = team;
    m_armed = (team > 0 && team < 3);
    m_validated = false;
    m_target = 0;
    m_targetLastSeen = 0.0f;
    m_targetLastPos.Init();
    m_curYaw = 0.0f;
    m_curPitch = 0.0f;
    m_desiredYaw = 0.0f;
    m_desiredPitch = 0.0f;
    m_nextAcquire = 0.0f;
    m_burstUntil = 0.0f;
    m_pauseUntil = 0.0f;
    m_scanBearing = 0.0f;
    m_aimErrorYaw = 0.0f;
    m_aimErrorPitch = 0.0f;
    m_residualYaw = 0.0f;
    m_residualPitch = 0.0f;
    m_poseYaw = -1;
    m_posePitch = -1;
    m_state = TURRET_IDLE;
}

// ---------------------------------------------------------------------------
CTurretRegistry::CTurretRegistry()
{
    m_count = 0;
    m_nextDiscover = 0.0f;
}

CTurret *CTurretRegistry::Find( int entindex )
{
    for (int i = 0; i < m_count; i++)
        if (m_turrets[i].EntIndex() == entindex) return &m_turrets[i];
    return 0;
}

bool CTurretRegistry::Add( int entindex, int team )
{
    if (entindex < 1) return false;
    if (Find( entindex )) return false;             // already registered
    if (m_count >= INS_TURRET_MAX) return false;
    m_turrets[m_count].Reset( entindex, team );
    m_count++;
    return true;
}

void CTurretRegistry::Remove( int entindex )
{
    for (int i = 0; i < m_count; i++)
    {
        if (m_turrets[i].EntIndex() != entindex) continue;
        // Swap-with-last: order is not meaningful and this keeps removal O(1).
        m_turrets[i] = m_turrets[m_count - 1];
        m_count--;
        return;
    }
}

void CTurretRegistry::RemoveAll()
{
    m_count = 0;
}

// ---------------------------------------------------------------------------
// Step 1 of the spec's per-frame tick: validate, then deregister if the slot
// has been recycled or emptied. The `remove` command is the primary mechanism;
// this is the backstop.
bool CTurret::Validate()
{
    if (!engine) return false;
    edict_t *e = engine->PEntityOfEntIndex( m_entIndex );
    if (!e || e->IsFree()) return false;
    IServerUnknown *unk = e->GetUnknown();
    if (!unk || !unk->GetBaseEntity()) return false;
    // GetServerClass() lives on IServerNetworkable, not IServerUnknown - go
    // through GetNetworkable() (bot_weapons.cpp:142 does the same).
    IServerNetworkable *net = unk->GetNetworkable();
    const char *cls = (net && net->GetServerClass()) ? net->GetServerClass()->GetName() : 0;
    if (!IsDriveableClass( cls )) return false;
    return true;
}

// Arm on the first boarding player with a play team. Latch once, never
// re-latch: a player cannot disarm an enemy turret by climbing in, and the
// turret keeps defending after its driver dies. Design spec sections 5.1, 16.2.
void CTurret::Arm()
{
    if (m_armed) return;
    edict_t *e = engine->PEntityOfEntIndex( m_entIndex );
    if (!e || e->IsFree()) return;
    IServerUnknown *unk = e->GetUnknown();
    if (!unk) return;
    void *pVehicle = unk->GetBaseEntity();
    if (!pVehicle) return;

    // CPropVehicleDriveable::m_hPlayer is a networked CNetworkHandle and
    // GetDriver() is virtual (vehicle_base.h:195, :233). Reading the handle
    // through the SendProp offset avoids depending on the C++ layout.
    if (g_TurretOffsetDriver < 0) return;
    unsigned long hPlayer = *(unsigned long *)((char *)pVehicle + g_TurretOffsetDriver);
    if (hPlayer == INVALID_EHANDLE_INDEX || hPlayer == 0) return;   // no driver

    int idx = (int)(hPlayer & ENT_ENTRY_MASK);                     // EHANDLE index bits
    if (idx < 1 || idx > gpGlobals->maxClients) return;
    edict_t *pe = engine->PEntityOfEntIndex( idx );
    if (!pe || pe->IsFree()) return;
    IPlayerInfo *pi = playerinfomanager ? playerinfomanager->GetPlayerInfo( pe ) : 0;
    if (!pi || !pi->IsConnected()) return;

    int t = pi->GetTeamIndex();
    // A spectator or unassigned player has no meaningful team, so boarding in
    // that state leaves the turret unarmed rather than defaulting it. Mirrors
    // ValidEnemy's t > 0 && t < 3 (bot_perceive.cpp:565).
    if (t <= 0 || t >= 3) return;

    SetTeam( t );
    Msg( "[insbots] turret %d armed, team %d (driver %d)\n", m_entIndex, t, idx );
}

// ---------------------------------------------------------------------------
void CTurret::FrameUpdate( bool simulating )
{
    if (!insbot_turret_enabled.GetBool()) return;
    if (!simulating) return;
    if (!gpGlobals) return;

    if (!Validate())
    {
        g_Turrets.Remove( m_entIndex );
        return;
    }
    if (!m_armed) Arm();

    float now = gpGlobals->curtime;
    float dt  = gpGlobals->frametime;
    if (dt <= 0.0f || dt > 0.25f) dt = 0.0f;   // guard against a hitch

    if (m_armed)
    {
        Acquire( now );     // 3 - throttled to ~10 Hz
        VerifyLOS();        // 4
        Aim( now );         // 5
        Traverse( dt );     // 6 - before fire, so the muzzle the player
        WritePose();        // 7   sees is the one the trace uses
        Fire( now );        // 8
    }
}

void CTurretRegistry::FrameUpdate( bool simulating )
{
    if (gpGlobals) Discover( gpGlobals->curtime );

    // Iterate BACKWARD: FrameUpdate can remove the current entry (swap-with-
    // last), and a forward loop would then skip the swapped-in turret.
    for (int i = m_count - 1; i >= 0; i--)
        m_turrets[i].FrameUpdate( simulating );
}

// ---------------------------------------------------------------------------
// Registration (design section 5.1).
//
// "A Humvee is spawned with no owner and no turret. The turret is armed by the
// first player to board it, and that player's team becomes the owning team."
//
// The design also says the spawner sends NO registration command - only a
// `remove` line on cleanup - and `insbot turret add` is documented as a manual
// test hook that "is not part of the normal path". Nothing else in the plan
// ever calls CTurretRegistry::Add, so without this sweep a Humvee spawned in
// normal play would never enter the registry and the whole feature would stay
// dormant. This closes that gap.
//
// The sweep is the natural place for it: it needs no new hook, no new console
// command and no change to the spawner, and it keys off exactly the event the
// design names - a player being IN the driver seat.
//
// Throttled: this walks every entity on the map, which is far too much work to
// do every frame for an event that happens a handful of times per round.
// ---------------------------------------------------------------------------
void CTurretRegistry::Discover( float now )
{
    if (now < m_nextDiscover) return;
    m_nextDiscover = now + 0.25f;               // 4 Hz
    if (!engine || !gpGlobals || !playerinfomanager) return;
    if (g_TurretOffsetDriver < 0) return;       // cannot read the driver yet
    if (m_count >= INS_TURRET_MAX) return;

    // Entity 0 is the world, so start at 1. The upper bound is the engine's own
    // entity count, not maxClients: a vehicle is a non-client entity.
    int maxEdicts = engine->GetEntityCount();
    for (int i = 1; i < maxEdicts; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex( i );
        if (!e || e->IsFree()) continue;
        if (Find( i )) continue;                // already a turret

        IServerUnknown *unk = e->GetUnknown();
        if (!unk) continue;
        IServerNetworkable *net = unk->GetNetworkable();
        const char *cls = (net && net->GetServerClass())
                        ? net->GetServerClass()->GetName() : 0;
        if (!IsDriveableClass( cls )) continue;

        // Only OUR Humvee. The buggy and the civilian cars share the class.
        if (!IsHumveeModel( EntityModelName( e ) )) continue;

        void *pVehicle = unk->GetBaseEntity();
        if (!pVehicle) continue;

        // The boarding event: a live driver already in the seat. Same handle
        // read CTurret::Arm uses, so there is one interpretation of m_hPlayer.
        unsigned long hPlayer =
            *(unsigned long *)((char *)pVehicle + g_TurretOffsetDriver);
        if (hPlayer == INVALID_EHANDLE_INDEX || hPlayer == 0) continue;

        int idx = (int)(hPlayer & ENT_ENTRY_MASK);
        if (idx < 1 || idx > gpGlobals->maxClients) continue;
        edict_t *pe = engine->PEntityOfEntIndex( idx );
        if (!pe || pe->IsFree()) continue;
        IPlayerInfo *pi = playerinfomanager->GetPlayerInfo( pe );
        if (!pi || !pi->IsConnected()) continue;
        int team = pi->GetTeamIndex();
        if (team <= 0 || team >= 3) continue;    // spectator/unassigned: inert

        if (Add( i, team ))
            Msg( "[insbots] turret %d discovered (driver %d, team %d)\n",
                 i, idx, team );
    }
}

// ---------------------------------------------------------------------------
void CTurretRegistry::Status()
{
    if (m_count == 0) { Msg( "[insbots] no turrets registered\n" ); return; }
    for (int i = 0; i < m_count; i++)
    {
        CTurret *t = &m_turrets[i];
        Msg( "[insbots] turret ent=%d team=%d armed=%d state=%s target=%d "
             "yaw=%.1f pitch=%.1f err=%.1f/%.1f aim=%.1f/%.1f\n",
             t->EntIndex(), t->Team(), t->Armed() ? 1 : 0,
             t->m_state == TURRET_ENGAGING ? "ENGAGING" : "IDLE",
             t->m_target, t->m_curYaw, t->m_curPitch,
             t->m_residualYaw, t->m_residualPitch,
             t->m_aimErrorYaw, t->m_aimErrorPitch );
    }
}

// ---------------------------------------------------------------------------
// `insbot turret <add|remove|status|set> [entindex] [name] [value]`
bool TurretHandleCommand( int argc )
{
    const char *a1 = (argc > 2) ? engine->Cmd_Argv( 2 ) : 0;
    if (!a1) { Msg( "[insbots] usage: insbot turret <add|remove|status|set>\n" ); return true; }

    if (!_stricmp( a1, "add" ))
    {
        // Manual escape hatch for testing only. The normal path is first-boarding
        // arming, so humvee_spawner.sp never sends this.
        if (argc < 4) { Msg( "[insbots] usage: insbot turret add <entindex> [team]\n" ); return true; }
        int idx  = atoi( engine->Cmd_Argv( 3 ) );
        int team = (argc > 4) ? atoi( engine->Cmd_Argv( 4 ) ) : 0;
        edict_t *e = engine->PEntityOfEntIndex( idx );
        if (!e || e->IsFree()) { Msg( "[insbots] turret add: no entity %d\n", idx ); return true; }
        IServerUnknown *unk = e->GetUnknown();
        IServerNetworkable *net = unk ? unk->GetNetworkable() : 0;
        const char *cls = (net && net->GetServerClass()) ? net->GetServerClass()->GetName() : 0;
        if (!IsDriveableClass( cls ))
        {
            // Guards against a recycled entity slot.
            Msg( "[insbots] turret add: entity %d is '%s', not CPropVehicleDriveable\n",
                 idx, cls ? cls : "?" );
            return true;
        }
        if (g_Turrets.Add( idx, team )) Msg( "[insbots] turret %d registered (team %d)\n", idx, team );
        else                           Msg( "[insbots] turret %d not registered\n", idx );
        return true;
    }

    if (!_stricmp( a1, "remove" ))
    {
        if (argc < 4) { Msg( "[insbots] usage: insbot turret remove <entindex>\n" ); return true; }
        int idx = atoi( engine->Cmd_Argv( 3 ) );
        g_Turrets.Remove( idx );
        Msg( "[insbots] turret %d removed\n", idx );
        return true;
    }

    if (!_stricmp( a1, "status" ))
    {
        g_Turrets.Status();
        return true;
    }

    if (!_stricmp( a1, "set" ))
    {
        // Per-turret overrides follow the existing per-weapon-class pattern
        // (NewCombatCfg, bot_loadout.cpp:512), where -1 means "inherit".
        if (argc < 6) { Msg( "[insbots] usage: insbot turret set <all|entindex> <name> <value>\n" ); return true; }
        const char *who = engine->Cmd_Argv( 3 );
        const char *nm  = engine->Cmd_Argv( 4 );
        float val       = (float)atof( engine->Cmd_Argv( 5 ) );

        CfgVar *c = CfgFind( nm );
        if (!c) { Msg( "[insbots] unknown setting '%s'\n", nm ); return true; }

        if (!_stricmp( who, "all" ))
        {
            // `all` is sugar for the global write.
            char buf[64];
            _snprintf( buf, sizeof(buf), "%.4f", val );
            buf[sizeof(buf) - 1] = 0;
            c->SetValue( buf );
            Msg( "[insbots] %s = %s (all turrets)\n", c->Name(), c->GetString() );
            return true;
        }

        int idx = atoi( who );
        if (!g_Turrets.Find( idx ))
        {
            Msg( "[insbots] turret set: no turret with entindex %d\n", idx );
            return true;
        }
        TurretSetOverride( idx, nm, val );
        Msg( "[insbots] turret %d: %s = %.4f\n", idx, nm, val );
        return true;
    }

    Msg( "[insbots] unknown turret subcommand '%s'\n", a1 );
    return true;
}

// ===========================================================================
// Firing effects. Three are straightforward IEffects/IEngineSound calls; the
// fourth (shell ejection) rides the vehicle's own DispatchEffect input.
// ===========================================================================
extern IEffects *g_pEffects;   // resolved in insbots.cpp Load()

// A Humvee on its roof tracking and firing at the sky is a visible absurdity.
// Firing is suppressed while roll exceeds ~60 deg; tracking and pose writing
// continue, so landing back on the wheels resumes instantly (design 6.2).
static bool TurretHullInverted( CTurret *t )
{
    ICollideable *col = TurretCollideable( t );
    if (!col) return true;
    QAngle a = col->GetCollisionAngles();
    float ux, uy, uz;
    InsTurretAngleUp( a.x, a.y, a.z, ux, uy, uz );
    return uz < cosf( 60.0f * (3.14159265f / 180.0f) );
}

static void TurretMuzzleFlash( const Vector &pos, const QAngle &ang )
{
    if (g_pEffects) g_pEffects->MuzzleFlash( pos, ang, 1.0f, 0 );
}

static void TurretTracer( const Vector &from, const Vector &to )
{
    if (!g_pEffects) return;
    // The real signature (game_shared/IEffects.h) is 16 parameters and takes
    // the frame/fade/noise values as bytes, not floats. A zero model index
    // makes the engine use the default laser beam sprite.
    //   Beam( start, end, modelIndex, haloIndex, frameStart, frameRate,
    //         life, width, endWidth, fadeLength, noise, r, g, b, brightness,
    //         speed )
    g_pEffects->Beam( from, to, 0, 0, 0, 0, 0.05f, 1, 1, 0, 0,
                      255, 255, 255, 255, 0 );
}

static void TurretImpact( const Vector &pos, const Vector &dir )
{
    if (g_pEffects) g_pEffects->MetalSparks( pos, dir );
}

// ---- broadcast recipient filter for the turret's gunshot -------------------
// Mirrors voice.cpp:80's CVoiceFilter. Kept local rather than reusing that one
// because that class is file-static there and is named for voice.
class CTurretSoundFilter : public IRecipientFilter
{
public:
    CTurretSoundFilter()
    {
        m_n = 0;
        int maxc = gpGlobals ? gpGlobals->maxClients : 0;
        if (maxc > 64) maxc = 64;
        for (int i = 1; i <= maxc && m_n < 64; i++)
        {
            edict_t *e = engine ? engine->PEntityOfEntIndex( i ) : 0;
            if (!e || e->IsFree()) continue;
            m_idx[m_n++] = i;
        }
    }
    virtual ~CTurretSoundFilter() {}
    virtual bool IsReliable( void ) const { return false; }
    virtual bool IsInitMessage( void ) const { return false; }
    virtual int  GetRecipientCount( void ) const { return m_n; }
    virtual int  GetRecipientIndex( int slot ) const
    {
        return (slot >= 0 && slot < m_n) ? m_idx[slot] : -1;
    }
private:
    int m_idx[64];
    int m_n;
};

static void TurretSound( const Vector &pos, int entIndex )
{
    if (!enginesound) return;
    // Emit from the vehicle's own entity index, using the M249's soundscript
    // path so the turret is identifiable by ear.
    //
    // CPASFilter is NOT usable here: it lives in the game DLL
    // (dlls/recipientfilter.h:125) and a standalone VSP cannot link it. This is
    // the same local IRecipientFilter subclass voice.cpp:80 uses - every
    // connected client, with the engine doing the positional culling.
    CTurretSoundFilter filter;
    enginesound->EmitSound( filter, entIndex, CHAN_WEAPON,
                            "Weapon_M249.Single", 1.0f, ATTN_NORM, 0, PITCH_NORM );
}

// Shell ejection, route C: fire the vehicle's own DispatchEffect input once
// per shot. The plugin supplies only a string; the game DLL resolves the
// attachment, builds CEffectData, and sends the temp entity with its own
// singleton sender. See docs/humvee-turret-shell-eject-verification.md.
static void TurretEjectShell( CTurret *t )
{
    edict_t *e = engine->PEntityOfEntIndex( t->EntIndex() );
    if (!e || e->IsFree()) return;
    IServerUnknown *unk = e->GetUnknown();
    if (!unk) return;
    void *pVehicle = unk->GetBaseEntity();
    if (!pVehicle) return;

    // The digits are a GetAttachment() number, which is 1-BASED, not the .mdl
    // index. The seven existing attachments occupy .mdl indices 0..6, so the
    // rebuilt model's appended ejection attachment lands at .mdl index 7 =
    // GetAttachment(8). RECOMPUTE after the rebuild; both numbering schemes
    // appear in the project docs and it is easy to copy the wrong one.
    const int kEjectAttachment = 8;

    char spec[64];
    _snprintf( spec, sizeof(spec), "%dRifleShellEject", kEjectAttachment );
    spec[sizeof(spec) - 1] = 0;

    // AllocPooledString returns string_t BY VALUE and string_t is a struct, so
    // the ABI is a hidden return pointer: result slot FIRST. Declaring it as
    // `string_t f(const char*)` writes through the string literal and corrupts
    // memory. See the verification doc section 5.1.1.
    InsTurretAllocPooledStringFn allocFn =
        (InsTurretAllocPooledStringFn)INS_TURRET_ALLOCPOOLEDSTRING;
    const char *pooled = 0;
    allocFn( &pooled, spec );
    if (!pooled || !pooled[0]) return;

    // variant_t: 20 bytes, union at +0x00, CHandle at +0x0C, fieldType at
    // +0x10. The zeroed tail is not cosmetic - AcceptInput may copy this into
    // an inputdata_t. FIELD_STRING is 2 (datamap.h:28-31).
    unsigned char value[20];
    memset( value, 0, sizeof(value) );
    *(const char **)(value + 0x00) = pooled;
    *(int *)(value + 0x10) = INS_TURRET_FIELD_STRING;

    InsTurretAcceptInputFn fn = (InsTurretAcceptInputFn)
        TurretGetVtableSlot( pVehicle, INS_TURRET_ACCEPTINPUT_SLOT );
    if (!fn) return;

    // pActivator is NULL: InputDispatchEffect never reads it. pCaller is the
    // vehicle itself, which only affects the entity-i/o debug line.
    //
    // On Windows this is __thiscall and the callee pops 0x24 itself, so there
    // must be NO `add esp` after the call. The typedef in turret_abi.h encodes
    // that; do not "fix" a stack imbalance here.
    fn( pVehicle, "DispatchEffect", 0, pVehicle, value, 0 );
}

// The firing gate, burst timing, and the whole shot. Ordering is deliberate:
// the pose was written in step 7 before this runs, so the muzzle the player
// sees is the one the trace uses.
void CTurret::Fire( float now )
{
    if (!insbot_turret_enabled.GetBool()) return;
    if (!m_target) return;
    if (TurretHullInverted( this )) return;

    // Burst state machine: 0.30 s firing, 0.45 s pause, reusing the bots'
    // tuned defaults.
    if (now < m_pauseUntil) return;
    if (m_burstUntil == 0.0f)
    {
        m_burstUntil = now + insbot_turret_burst_time.GetFloat();
    }
    else if (now > m_burstUntil)
    {
        m_burstUntil = 0.0f;
        m_pauseUntil = now + insbot_turret_burst_pause.GetFloat();
        return;
    }

    // Fire gate: aligned to the tolerance AND with a clear line of sight AND
    // within engagement range. The inherited bot gate is +/-18 deg, which is
    // ~400 units of lateral slop at 1500 units - far too loose here.
    float tolYaw   = insbot_turret_fire_tol_yaw.GetFloat();
    float tolPitch = insbot_turret_fire_tol_pitch.GetFloat();
    tolYaw   = TurretValue( this, "turret_fire_tolerance_yaw",   tolYaw );
    tolPitch = TurretValue( this, "turret_fire_tolerance_pitch", tolPitch );
    if (!InsTurretAimed( m_residualYaw, m_residualPitch, tolYaw, tolPitch )) return;
    if (!VerifyLOS()) return;

    edict_t *te = engine->PEntityOfEntIndex( m_target );
    if (!te || te->IsFree()) return;
    IPlayerInfo *pi = playerinfomanager ? playerinfomanager->GetPlayerInfo( te ) : 0;
    // IsAlive() does not exist on this interface - see TurretIsValidEnemy.
    if (!pi || pi->IsDead()) return;

    Vector from = TurretMuzzlePosition( this );
    Vector aim  = m_targetLastPos;
    float  dist = ( aim - from ).Length();
    if (dist > TurretValue( this, "turret_engage_range",
                            insbot_turret_engage_range.GetFloat() )) return;  // firing gate

    // Spread: a small cone, defaulting to the M249's real spread_degrees.
    float spread = TurretValue( this, "turret_spread", insbot_turret_spread.GetFloat() );
    Vector dir = ( aim - from );
    InsTurretNormalize( dir.x, dir.y, dir.z );
    if (spread > 0.0f)
    {
        float r  = spread * (3.14159265f / 180.0f);
        float a  = ((float)(rand() % 3600) / 3600.0f) * 6.2831853f;
        float rr = ((float)(rand() % 1000) / 1000.0f) * r;
        dir.x += cosf( a ) * rr;
        dir.y += sinf( a ) * rr;
        InsTurretNormalize( dir.x, dir.y, dir.z );
    }

    // Trace the shot so the tracer and impact land where the bullet goes, and
    // so the vehicle itself and any boarded rider are excluded.
    trace_t tr;
    Ray_t ray;
    ray.Init( from, from + dir * dist );
    CTurretTraceFilter fireFilter;
    TurretBuildSkipSet( this, fireFilter );
    enginetrace->TraceRay( ray, MASK_SHOT, &fireFilter, &tr );

    Vector hitPos = tr.fraction < 1.0f ? tr.endpos : ( from + dir * dist );
    TurretApplyDamage( this, m_target, dir,
                       TurretValue( this, "turret_damage",
                                    insbot_turret_damage.GetFloat() ) );
    TurretMuzzleFlash( from, TurretMuzzleAngles( this ) );
    TurretTracer( from, hitPos );
    if (tr.fraction < 1.0f) TurretImpact( tr.endpos, dir );
    TurretSound( from, m_entIndex );
    TurretEjectShell( this );
}

// ===========================================================================
// Damage delivery. OnTakeDamage is virtual (baseentity.h:831) and this is the
// path SDKHooks_TakeDamage itself uses, so the engine's damage path is proven;
// what was unproven was our ability to call it. See
// docs/humvee-turret-damage-audit.md.
//
// The slot is per-platform (Windows 53 / Linux 54) and the struct is
// hand-built. Guessing either one is a server crash, not a bug.
// ===========================================================================

// A minimal stand-in for CTakeDamageInfo's vtable. The shipped class has one
// virtual (_GetClassName); the damage path reads the vptr but never calls it
// in the bullet path, so a single-entry table is sufficient and is what the
// live probe used successfully on both platforms.
static const char *TurretInfoGetClassName( void * )
{
    return "CTakeDamageInfo";
}
static void *g_TurretInfoVtable[1] = { (void *)TurretInfoGetClassName };

static void *TurretGetVtableSlot( void *pObject, int slot )
{
    if (!pObject) return 0;
    void **vtable = *(void ***)pObject;
    if (!vtable) return 0;
    return vtable[slot];
}

// EHANDLE is CHandle<CBaseEntity>: a single index with the serial number in
// the high bits. Reading the entity's own handle this way avoids depending on
// the C++ member layout, which differs per platform.
static unsigned long TurretMakeHandle( edict_t *e )
{
    if (!e || e->IsFree()) return 0xFFFFFFFFul;
    IServerUnknown *unk = e->GetUnknown();
    if (!unk) return 0xFFFFFFFFul;
    void *pEnt = unk->GetBaseEntity();
    if (!pEnt) return 0xFFFFFFFFul;

    // CBaseEntity::GetRefEHandle() is virtual. Windows slot 2, Linux slot 3:
    // MSVC emits ONE destructor where Itanium emits two, so every entry from
    // index 2 on shifts down by one. See humvee-turret-damage-audit.md.
#ifdef _LINUX
    const int kGetRefEHandleSlot = 3;
#else
    const int kGetRefEHandleSlot = 2;
#endif
    typedef void *(*GetRefEHandleFn)( void * );
    GetRefEHandleFn fn = (GetRefEHandleFn)TurretGetVtableSlot( pEnt, kGetRefEHandleSlot );
    if (!fn) return 0xFFFFFFFFul;
    unsigned long *pHandle = (unsigned long *)fn( pEnt );
    return pHandle ? *pHandle : 0xFFFFFFFFul;
}

static void TurretApplyDamage( CTurret *t, int targetIdx, const Vector &dir, float dmg )
{
    edict_t *te = engine->PEntityOfEntIndex( targetIdx );
    if (!te || te->IsFree()) return;
    IServerUnknown *unk = te->GetUnknown();
    if (!unk) return;
    void *pTarget = unk->GetBaseEntity();
    if (!pTarget) return;

    // The attacker must be non-null, but it does NOT have to be a player:
    // CINSRules::FPlayerCanTakeDamage consults PlayerRelationship, which
    // returns GR_TEAMMATE whenever the attacker is not a player, so a
    // prop_vehicle_driveable attacker is always allowed. Passing the vehicle
    // is therefore both simpler and correct - and it gives the damage an
    // origin at the truck. See humvee-turret-damage-audit.md section 8.4.
    edict_t *ve = engine->PEntityOfEntIndex( t->EntIndex() );
    unsigned long attackerHandle = TurretMakeHandle( ve );
    if (attackerHandle == 0xFFFFFFFFul)
    {
        // Fail closed: a null attacker makes CINSPlayer::OnTakeDamage refuse
        // outright, and on some paths dereference null.
        return;
    }

    InsTurretTakeDamageInfo info;
    memset( &info, 0, sizeof(info) );
    info.m_pVtable         = (void *)g_TurretInfoVtable;
    info.m_hInflictor      = attackerHandle;
    info.m_hAttacker       = attackerHandle;
    info.m_flDamage        = dmg;
    info.m_flMaxDamage     = dmg;
    info.m_bitsDamageType  = INS_TURRET_DMG_BULLET;
    info.m_iCustomKillType = 0;
    info.m_iAmmoType       = -1;
    info.m_vecDamageForce.x = dir.x * dmg;
    info.m_vecDamageForce.y = dir.y * dmg;
    info.m_vecDamageForce.z = dir.z * dmg;

    InsTurretOnTakeDamageFn fn =
        (InsTurretOnTakeDamageFn)TurretGetVtableSlot( pTarget, INS_TURRET_OTD_SLOT );
    if (!fn) return;
    fn( pTarget, &info );
}

// ===========================================================================
// Perception - acquisition with the turret's own line of sight.
//
// BORROWED from bot_perceive.cpp; see the BORROWED INVARIANTS block at the top
// of this file. The deliberate changes are (a) the ray origin is the MUZZLE,
// not an eye position, and (b) the trace filter applies the engine's standard
// collision rules, which CTraceFilterSkipSelf deliberately does not.
// ===========================================================================

// Collect the vehicle itself plus every client currently marked as riding in a
// vehicle. Reading the group through ICollideable on an edict is the same thing
// bot_body.cpp:80 does, and the group is the engine's own "aboard" marker.
static void TurretBuildSkipSet( CTurret *t, CTurretTraceFilter &filter )
{
    edict_t *ve = engine->PEntityOfEntIndex( t->EntIndex() );
    if (ve && !ve->IsFree()) filter.Add( (IHandleEntity *)ve->GetUnknown() );

    int maxClients = gpGlobals->maxClients;
    if (maxClients > 66) maxClients = 66;
    for (int i = 1; i <= maxClients; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex( i );
        if (!e || e->IsFree()) continue;
        IServerUnknown *unk = e->GetUnknown();
        if (!unk) continue;
        ICollideable *col = unk->GetCollideable();
        if (col && col->GetCollisionGroup() == COLLISION_GROUP_IN_VEHICLE)
            filter.Add( (IHandleEntity *)unk );
    }
}

// Resolve trace_t::m_pEnt to an edict.
//
// tr.m_pEnt is declared CBaseEntity* (public/gametrace.h), and CBaseEntity is
// only forward-declared in a plugin TU - we cannot touch it. But every entity
// IServerUnknown derives from IHandleEntity, so the pointer converts directly,
// and the chain from there is public: IServerUnknown -> GetNetworkable() ->
// GetEdict(). bot_perceive.cpp:443 casts the same way.
static edict_t *EdictFromTraceEnt( CBaseEntity *ent )
{
    if (!ent) return 0;
    IServerUnknown *unk = (IServerUnknown *)(IHandleEntity *)ent;
    IServerNetworkable *net = unk->GetNetworkable();
    return net ? net->GetEdict() : 0;
}

// Strict per-frame validity: a live enemy player still in the game. Mirrors
// CInsBot::ValidEnemy (bot_perceive.cpp:556-577), including the team range.
//
// NOTE: IsAlive() does not exist on IPlayerInfo in the Episode 1 SDK - the
// interface exposes IsDead() (public/dlls/iplayerinfo.h). The plan said
// IsAlive(); that was wrong.
static bool TurretIsValidEnemy( CTurret *t, int idx )
{
    if (!gpGlobals || !engine || !playerinfomanager) return false;
    if (idx < 1 || idx > gpGlobals->maxClients) return false;
    edict_t *e = engine->PEntityOfEntIndex( idx );
    if (!e || e->IsFree()) return false;
    IPlayerInfo *pi = playerinfomanager->GetPlayerInfo( e );
    if (!pi) return false;
    // NOTE: the plan specified `pi->IsFakeClient() -> reject`. That is wrong
    // for this mod: every insbots bot IS a fake client (bot_entry.cpp:172-183
    // counts humans as `IsConnected() && !IsFakeClient()`), so rejecting fake
    // clients would make the turret blind to the bots it exists to fight.
    // CInsBot::ValidEnemy (bot_perceive.cpp:562) therefore requires
    // IsPlayer() and does NOT test IsFakeClient. This follows the proven rule.
    if (!pi->IsConnected() || !pi->IsPlayer()) return false;
    if (pi->IsDead() || pi->IsObserver() || pi->IsHLTV()) return false;
    if (pi->GetTeamIndex() <= 0 || pi->GetTeamIndex() >= 3) return false;
    if (pi->GetTeamIndex() == t->Team()) return false;
    if (pi->GetHealth() <= 0) return false;

    // The origin-stack rejection: a fake client that connected but never got a
    // squad slot piles up at the world origin and is invisible. Targeting it is
    // the "ghost" bug (bot_perceive.cpp:569-574).
    Vector p = pi->GetAbsOrigin();
    if (p.x > -48.0f && p.x < 48.0f &&
        p.y > -48.0f && p.y < 48.0f &&
        p.z > -48.0f && p.z < 48.0f) return false;

    return true;
}

// Chest, torso and head, as fractions of the target's hull height. Mirrors
// BuildEnemyTargetPoints (bot_perceive.cpp:341-379) rather than using fixed
// offsets, so a crouched or prone target is still hit: the plan's fixed
// 36/52/64 offsets assume a standing 72-unit hull and would aim over a
// crouched enemy.
static int TurretTargetPoints( edict_t *e, Vector out[3] )
{
    IPlayerInfo *pi = playerinfomanager->GetPlayerInfo( e );
    if (!pi) return 0;

    Vector base = pi->GetAbsOrigin();
    float top = 72.0f;                        // standing INS player hull height
    float h = pi->GetPlayerMaxs().z;
    if (h >= 16.0f && h <= 96.0f) top = h;

    float heights[3];
    heights[0] = top * 0.625f;                // chest: 45 on a standing hull
    heights[1] = top * 0.80f;                 // upper torso / shoulders
    heights[2] = top - 8.0f;                  // head, inside the current hull

    int count = 0;
    for (int i = 0; i < 3; i++)
    {
        float z = heights[i];
        if (z < 8.0f || z > top) continue;

        bool duplicate = false;
        for (int j = 0; j < count; j++)
            if (fabsf( out[j].z - (base.z + z) ) < 1.0f) { duplicate = true; break; }
        if (duplicate) continue;

        out[count] = base;
        out[count].z += z;
        count++;
    }
    return count;
}

// GetCollideable() lives on IServerUnknown, not on edict_t. Always go through
// unk, as bot_body.cpp:84 and bot_perceive.cpp:76 do.
static ICollideable *TurretCollideable( CTurret *t )
{
    edict_t *e = engine->PEntityOfEntIndex( t->EntIndex() );
    if (!e || e->IsFree()) return 0;
    IServerUnknown *unk = e->GetUnknown();
    return unk ? unk->GetCollideable() : 0;
}

// The muzzle, computed from the turret's own pivot and the angles it just
// wrote. The `muzzle` attachment exists to document this geometry and is the
// fallback if the arithmetic proves too coarse in game - but reading it needs
// a GetAttachment vtable call, so it is not the default (design section 8).
//
// PIVOT is the turret ring centre in the vehicle's LOCAL space. It is MEASURED,
// not guessed: the shipped .mdl's own bone table, composed through its parents,
// puts turret_yaw (bone 9) at
//
//     (-0.50, 3.22, 86.23)      [x = lateral, y = fore/aft, z = up]
//
// which agrees with docs/humvee-turret-feasibility.md section 5.3 once that
// document's (x, height, y) tuple ordering is accounted for.
//
// THE PLAN HAD THIS WRONG. It carried (0, 86.2, 96), reading the 86.2 as a
// FORWARD offset and pairing it with a 96 height. 86.2 is the ring's HEIGHT;
// the ring sits essentially over the hull centre (y = 3.22), not 86 units
// forward. Using the plan's value put the muzzle ~83 units ahead of and 10
// units below the real ring, which is why the firing trace was inconsistent.
//
// PIVOT is the turret RING centre; PITCH_PIVOT is the elevation hinge the gun
// rotates about. Both are MEASURED from the shipped model's own bone table
// (temp/humvee_turret_rig/check_muzzle_math.py prints them), in the vehicle's
// LOCAL space:
//
//     ring        (-0.505,   3.215,  86.230)   [x lateral, y fore/aft, z up]
//     pitch hinge (-0.505,  21.550, 103.800)
//     muzzle tip  (-0.941,  35.234, 106.380)
//
// The muzzle must be computed FROM THE PITCH HINGE, not the ring. The gun
// rotates about the hinge, so `ring + dir * |ring->muzzle|` is only correct at
// one elevation: the two vectors differ by ~21 degrees, which at 1500 units is
// a visibly misplaced tracer origin. `hinge + dir * |hinge->muzzle|` is correct
// at every elevation, because the hinge is the fixed point of the rotation and
// the barrel direction from it IS the aim direction once the pose levels the
// gun (which the rebuilt model's poses do - see gen_turret.py's REST_ELEV).
//
// MUZZLE_FORWARD was 62.0, a guess made while no `muzzle` attachment existed.
// The rebuilt model has one, and the measured hinge-to-muzzle distance is
// 13.93 units.
#define INS_TURRET_PIVOT_LOCAL_X        ( -0.50f)
#define INS_TURRET_PIVOT_LOCAL_Y        (  3.22f)
#define INS_TURRET_PIVOT_LOCAL_Z        ( 86.23f)
#define INS_TURRET_PITCH_PIVOT_LOCAL_Y  ( 21.55f)
#define INS_TURRET_PITCH_PIVOT_LOCAL_Z  (103.80f)
#define INS_TURRET_MUZZLE_FORWARD       ( 13.93f)

static Vector TurretMuzzlePosition( CTurret *t )
{
    ICollideable *col = TurretCollideable( t );
    if (!col) return Vector( 0, 0, 0 );

    // The elevation hinge, NOT the ring: the gun rotates about this point, so it
    // is the fixed origin the muzzle direction is measured from. The lateral
    // offset is shared with the ring - the model puts both on x = -0.505.
    Vector pivotLocal( INS_TURRET_PIVOT_LOCAL_X,
                       INS_TURRET_PITCH_PIVOT_LOCAL_Y,
                       INS_TURRET_PITCH_PIVOT_LOCAL_Z );

    // Vehicle-local -> world, using the same entity transform the engine uses.
    QAngle vehAngles = col->GetCollisionAngles();
    Vector vehOrigin = col->GetCollisionOrigin();

    // Local math rather than the mathlib helpers: see the note in turret_math.h
    // (Linux resolves them from the engine, Windows cannot link them).
    float vm[3][4];
    InsTurretAngleMatrix( vehAngles.x, vehAngles.y, vehAngles.z,
                          vehOrigin.x, vehOrigin.y, vehOrigin.z, vm );

    float px, py, pz;
    InsTurretVectorTransform( pivotLocal.x, pivotLocal.y, pivotLocal.z,
                              vm, px, py, pz );

    float yawRad   = t->m_curYaw   * (3.14159265f / 180.0f);
    float pitchRad = t->m_curPitch * (3.14159265f / 180.0f);

    // Barrel direction in the vehicle's LOCAL frame, then rotated to world by
    // the vehicle's own transform.
    //
    // CONVENTION - this must match what Aim() produces and what WritePose()
    // stores, because m_flPoseParameter is a LOCAL rotation relative to the
    // hull.
    //
    // Aim() computes `localYaw = worldYaw - hullYaw - 90`, so the barrel's
    // world bearing is `localYaw + hullYaw + 90`. At localYaw = 0 that is
    // bearing 90, i.e. the entity's LOCAL +Y - and that is where this model's
    // barrel actually points at rest: the M249's geometry sits 32 units
    // forward of the turret ring and only 8.6 behind it, and the QC puts the
    // bonnet at model +Y. The FOV test agrees independently:
    // `yawRad = m_curYaw + hullYaw + 90` with `fwd = (cos, sin)`.
    //
    // So, for a plugin yaw of `theta`:
    //   yaw 0     -> (0,+1,0), out over the bonnet
    //   yaw +90   -> (-1,0,0), local -X
    //   yaw -90   -> (+1,0,0), local +X
    //   pitch     -> positive is DOWN: +20 depresses, -55 elevates
    //
    // THIS WAS WRONG. It previously read `(sin, -cos)`, which is the mirror of
    // the above: at yaw 0 it pointed local -Y, i.e. 180 degrees out from both
    // Aim() and the FOV test. The comment justifying it ("the mesh's front is
    // -Y") came from the spawner's facing note, which is about where the ENGINE
    // expects a vehicle's front to sit for steering purposes - not about where
    // this mesh's barrel points. The error put the computed muzzle ~62 units
    // behind the ring, so the tracer, the muzzle flash and the LOS trace origin
    // all sat inside or behind the cab. Damage was unaffected because the
    // firing trace is drawn from the target side.
    //
    // See temp/humvee_turret_rig/yaw_convention.py, which derives this from the
    // gun geometry and the compiled model rather than from prose.
    Vector dirLocal( -sinf( yawRad ) * cosf( pitchRad ),
                      cosf( yawRad ) * cosf( pitchRad ),
                     -sinf( pitchRad ) );

    float dx, dy, dz;
    InsTurretVectorRotate( dirLocal.x, dirLocal.y, dirLocal.z, vm, dx, dy, dz );

    return Vector( px, py, pz ) + Vector( dx, dy, dz ) * INS_TURRET_MUZZLE_FORWARD;
}

static QAngle TurretMuzzleAngles( CTurret *t )
{
    return QAngle( t->m_curPitch, t->m_curYaw, 0.0f );
}

// Kept as the shared entry point so the earlier tasks' call sites do not move.
static Vector TurretRayOrigin( CTurret *t )
{
    return TurretMuzzlePosition( t );
}

// LOS from the turret's own muzzle. It does NOT fire at targets only a
// teammate can see (design section 6.4). The mask matches bot_perceive.cpp:392
// so a target acquired through glass also passes the final firing gate.
static bool TurretIsVisible( CTurret *t, int idx, const Vector &from )
{
    if (!enginetrace) return false;
    edict_t *e = engine->PEntityOfEntIndex( idx );
    if (!e || e->IsFree()) return false;

    Vector pts[3];
    int n = TurretTargetPoints( e, pts );
    if (n <= 0) return false;

    const unsigned int sightMask = MASK_SHOT & ~( CONTENTS_HITBOX | CONTENTS_WINDOW );

    CTurretTraceFilter filter;
    TurretBuildSkipSet( t, filter );

    for (int i = 0; i < n; i++)
    {
        // Smoke blocks the turret exactly as it blocks a bot (design section
        // 6.3 lists smoke occlusion as inherited from FindEnemy). Shared
        // predicate, so the 140u threshold has one home.
        if (LineGoesThroughSmoke( from, pts[i] )) continue;

        trace_t tr;
        Ray_t ray;
        ray.Init( from, pts[i] );
        enginetrace->TraceRay( ray, sightMask, &filter, &tr );
        if (tr.fraction >= 0.98f) return true;      // clear line to this point

        if (EdictFromTraceEnt( tr.m_pEnt ) == e) return true;       // reached it
    }
    return false;
}

// Acquisition: team hostility, FOV cone, closest-first, with the current
// target's 0.64x squared-distance bonus. Throttled to ~10 Hz by the caller.
void CTurret::Acquire( float now )
{
    if (now < m_nextAcquire) return;
    m_nextAcquire = now + 0.1f;                 // ~10 Hz (design section 5.3)

    // Keep the current target while it stays valid and recent. Memory is
    // ~2 s (design section 6.2), after which we resume scanning.
    if (m_target)
    {
        if (TurretIsValidEnemy( this, m_target ) &&
            (now - m_targetLastSeen) < 2.0f )
        {
            edict_t *te = engine->PEntityOfEntIndex( m_target );
            if (te && !te->IsFree())
            {
                Vector pts[3];
                if (TurretTargetPoints( te, pts ) > 1) m_targetLastPos = pts[1];
                m_state = TURRET_ENGAGING;
                return;
            }
        }
        m_target = 0;
        m_state = TURRET_IDLE;
        // Resume the idle sweep from where the gun is actually pointing, so a
        // lost target does not snap the ring back to a stale bearing
        // (design section 6.2: "continue from current bearing, no snap-back").
        m_scanBearing = m_curYaw;
    }

    Vector from = TurretRayOrigin( this );
    float  bestDist = 1.0e12f;
    int    best = 0;
    const float spotRange = insbot_vis_range.GetFloat();

    // The FOV cone is measured from the turret's CURRENT aim, not the hull's
    // heading: a ring-mounted gun engages what it is pointing at (design
    // section 6.3, mirroring bot_perceive.cpp:588-591).
    //
    // m_curYaw is a LOCAL angle, so it is converted back to a world bearing
    // here using the inverse of Aim()'s mapping. Comparing a local yaw against
    // a world direction would rotate the cone with the truck instead of with
    // the gun.
    float hullYaw = 0.0f;
    {
        ICollideable *col = TurretCollideable( this );
        if (col) hullYaw = col->GetCollisionAngles().y;
    }
    float yawRad = (m_curYaw + hullYaw + 90.0f) * (3.14159265f / 180.0f);
    Vector fwd( cosf( yawRad ), sinf( yawRad ), 0 );
    float cosFov = cosf( insbot_fov.GetFloat() * 0.5f * (3.14159265f / 180.0f) );

    for (int i = 1; i <= gpGlobals->maxClients; i++)
    {
        if (!TurretIsValidEnemy( this, i )) continue;
        edict_t *e = engine->PEntityOfEntIndex( i );
        IPlayerInfo *pi = playerinfomanager->GetPlayerInfo( e );
        Vector target = pi->GetAbsOrigin();

        Vector d = target - from;
        float dist = d.Length();
        if (dist > spotRange) continue;                 // spot range 2500

        // FOV test, skipped for the target we already hold so a target that
        // drifts to the cone edge is not dropped mid-track.
        if (i != m_target)
        {
            Vector flat( d.x, d.y, 0 );
            float flatLen = flat.Length();
            if (flatLen > 1.0f && (flat.x * fwd.x + flat.y * fwd.y) / flatLen < cosFov)
                continue;
        }

        if (!TurretIsVisible( this, i, from )) continue;

        float scoreDistSq = dist * dist;
        if (i == m_target) scoreDistSq *= 0.64f;        // 20% distance advantage
        if (scoreDistSq > bestDist) continue;
        bestDist = scoreDistSq;
        best = i;
    }

    if (best)
    {
        m_target = best;
        m_targetLastSeen = now;
        edict_t *e = engine->PEntityOfEntIndex( best );
        Vector pts[3];
        if (e && !e->IsFree() && TurretTargetPoints( e, pts ) > 1)
            m_targetLastPos = pts[1];
        m_state = TURRET_ENGAGING;
        // Aim error is re-rolled on the bots' 0.4-0.9 s cadence, not per
        // frame: a per-frame reroll averages out to a laser (design 6.2).
        float e0 = TurretValue( this, "turret_aim_error",
                                insbot_turret_aim_error.GetFloat() );
        m_aimErrorYaw   = ((float)(rand() % 2001) / 1000.0f - 1.0f) * e0;
        m_aimErrorPitch = ((float)(rand() % 2001) / 1000.0f - 1.0f) * e0;
    }
}

// ===========================================================================
// Aim solver and pose writing (design section 6.2).
// ===========================================================================

// Re-verify LOS each frame against the target's chest/torso/head, so a target
// that steps behind cover is dropped rather than shot through a wall.
bool CTurret::VerifyLOS()
{
    if (!m_target) return false;
    if (!TurretIsValidEnemy( this, m_target ))
    {
        m_target = 0;
        m_state = TURRET_IDLE;
        return false;
    }
    Vector from = TurretRayOrigin( this );
    if (!TurretIsVisible( this, m_target, from ))
    {
        // Keep aiming at the last known position for the memory window; the
        // fire gate is separately blocked because VerifyLOS returned false.
        return false;
    }
    edict_t *e = engine->PEntityOfEntIndex( m_target );
    if (e && !e->IsFree())
    {
        Vector pts[3];
        if (TurretTargetPoints( e, pts ) > 1) m_targetLastPos = pts[1];
    }
    m_targetLastSeen = gpGlobals->curtime;
    return true;
}

// Desired yaw/pitch from the muzzle to the aim point, plus the aim error.
void CTurret::Aim( float now )
{
    Vector from = TurretRayOrigin( this );
    Vector want;

    if (m_target)
    {
        want = m_targetLastPos;
    }
    else
    {
        // No target: sweep slowly across the arc. A continuous sweep avoids a
        // reversal state machine and can acquire from any direction
        // (design section 6.2).
        //
        // The sweep advances the LOCAL bearing, because that is what the pose
        // parameter consumes: a world-space sweep would make the ring drift
        // whenever the hull turned. m_scanBearing is re-seeded from the live aim
        // in Acquire() at the moment a target is lost, which is what gives
        // "continue from the current bearing, no snap back" (design 6.2).
        m_scanBearing = InsTurretNormalizeAngle(
            m_scanBearing + TurretValue( this, "turret_scan_speed",
                                         insbot_turret_scan_speed.GetFloat() )
                            * gpGlobals->frametime );
        float rad = m_scanBearing * (3.14159265f / 180.0f);
        want = from + Vector( sinf( rad ) * 1000.0f, -cosf( rad ) * 1000.0f, 0.0f );
    }

    Vector d = want - from;
    float horiz     = sqrtf( d.x * d.x + d.y * d.y );
    float wantPitch = -atan2f( d.z, horiz ) * (180.0f / 3.14159265f);

    // WORLD -> LOCAL yaw. m_flPoseParameter drives a rotation RELATIVE TO THE
    // HULL, and the barrel's local zero points along the mesh's -Y, so a world
    // bearing must have the hull's own yaw subtracted and then be shifted by
    // the 90 degrees between "world +X is zero" and "local -Y is zero":
    //
    //     localYaw = (worldYaw - hullYaw) - 90
    //
    // Without this the turret pointed 90 degrees off, and worse, it tracked in
    // world space so the whole ring drifted as the truck turned. The bots never
    // hit this because their eye angles ARE world angles.
    float wantYaw;
    if (m_target)
    {
        float wantYawWorld = atan2f( d.y, d.x ) * (180.0f / 3.14159265f);
        float hullYaw = 0.0f;
        ICollideable *col = TurretCollideable( this );
        if (col) hullYaw = col->GetCollisionAngles().y;
        wantYaw = InsTurretNormalizeAngle( wantYawWorld - hullYaw - 90.0f );
    }
    else
    {
        // The sweep already produced a local bearing; deriving it back from the
        // synthetic world point would re-introduce the hull yaw we just removed.
        wantYaw = m_scanBearing;
    }

    if (m_target)
    {
        wantYaw   += m_aimErrorYaw;
        wantPitch += m_aimErrorPitch;
    }

    m_desiredYaw   = InsTurretNormalizeAngle( wantYaw );
    m_desiredPitch = InsTurretClampPitch( wantPitch );
    (void)now;
}

// Rate-limited traverse, every frame. The rate is a pintle mount on a truck:
// deliberately heavier than a soldier's 260 deg/s (design section 6.2).
void CTurret::Traverse( float dt )
{
    float maxStep = TurretValue( this, "turret_aim_speed",
                                 insbot_turret_aim_speed.GetFloat() ) * dt;
    m_curYaw   = InsTurretStepAngle( m_curYaw,   m_desiredYaw,   maxStep );
    m_curPitch = InsTurretStepAngle( m_curPitch, m_desiredPitch, maxStep );
    m_curPitch = InsTurretClampPitch( m_curPitch );

    m_residualYaw   = InsTurretAngleDelta( m_curYaw,   m_desiredYaw );
    m_residualPitch = InsTurretAngleDelta( m_curPitch, m_desiredPitch );
}

// Write the aim into the engine's own pose-parameter network array. The array
// is CNetworkArray(float, m_flPoseParameter, 24) and is sent as an 11-bit
// 0..1 float array, so writing here is what makes the gun move for every
// player. The values are written as NORMALISED 0..1 controls, matching what
// Studio_SetPoseParameter stores and what the client's bone solver reads.
//
// THE INDICES ARE RESOLVED FROM THE MODEL BY NAME, NOT HARDCODED.
//
// The shipped model declares ELEVEN pose parameters and all eleven belong to
// the engine's own vehicle physics: four wheel heights (0..3), four wheel spins
// (4..7), steer (8), action (9) and gauge (10). CFourWheelVehiclePhysics
// resolves those by name and writes them every frame. The turret's two are
// appended by the model rebuild at indices 11 and 12.
//
// An earlier version of this file wrote slots 0 and 1, on the belief that the
// model declared no pose parameters at all. That belief came from reading the
// wrong studiohdr_t offset: the probe used +0xF8, while numlocalposeparameters
// is at +0x12C (public/studio.h, studiohdr_t). The true count is 11, so writing
// 0 and 1 meant writing `vehicle_wheel_fl_height` and `vehicle_wheel_fr_height`
// - the two parameters the engine is simultaneously driving to hold the front
// suspension up. Nothing crashed and nothing failed to build; the visible
// result is wheels fighting the plugin.
//
// Resolving by name means the code cannot be broken by the rebuild renumbering
// its parameters, and it also fails SAFE on a model that has not been rebuilt:
// if `vehicle_turret_yaw` is absent the lookup returns -1 and WritePose() does
// nothing, instead of clobbering whichever parameter happens to sit at the
// hardcoded index.
void CTurret::WritePose()
{
    if (g_TurretOffsetPoseParameter < 0) return;
    edict_t *e = engine->PEntityOfEntIndex( m_entIndex );
    if (!e || e->IsFree()) return;
    IServerUnknown *unk = e->GetUnknown();
    if (!unk) return;
    void *pVehicle = unk->GetBaseEntity();
    if (!pVehicle) return;

    // Resolve once per turret. A model swap or a level change resets the turret
    // (Reset() clears these), so a stale index cannot survive.
    if (m_poseYaw < 0 || m_posePitch < 0)
    {
        if (!modelinfo) return;
        IServerEntity *se = e->GetIServerEntity();
        if (!se) return;
        const model_t *mod = modelinfo->GetModel( se->GetModelIndex() );
        if (!mod) return;
        const studiohdr_t *hdr = modelinfo->GetStudiomodel( mod );
        if (!hdr) return;

        int yawIdx = -1, pitchIdx = -1;
        const int n = hdr->numlocalposeparameters;
        for (int i = 0; i < n; ++i)
        {
            const char *name = hdr->pLocalPoseParameter( i )->pszName();
            if (!name) continue;
            if (!strcmp( name, "vehicle_turret_yaw" ))   yawIdx   = i;
            if (!strcmp( name, "vehicle_turret_pitch" )) pitchIdx = i;
        }

        if (yawIdx < 0 || pitchIdx < 0)
        {
            // The model has not been rebuilt, or is a different vehicle. Stay
            // silent after the first report so this cannot spam per frame.
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                Msg("[insbots] turret: model '%s' declares no vehicle_turret_yaw/pitch "
                    "pose parameters - the gun cannot move. Rebuild the model.\n",
                    EntityModelName( e ));
            }
            return;
        }

        m_poseYaw   = yawIdx;
        m_posePitch = pitchIdx;
        Msg("[insbots] turret: pose parameters resolved - yaw=%d pitch=%d (%d declared)\n",
            m_poseYaw, m_posePitch, n);
    }

    float *pose = (float *)((char *)pVehicle + g_TurretOffsetPoseParameter);

    // Declared ranges must match the QC's $poseparameter lines exactly.
    const float kYawStart   = -180.0f, kYawEnd   =  180.0f;
    const float kPitchStart =  -55.0f, kPitchEnd =   20.0f;

    pose[m_poseYaw]   = InsTurretNormPose( m_curYaw,   kYawStart,   kYawEnd );
    pose[m_posePitch] = InsTurretNormPose( m_curPitch, kPitchStart, kPitchEnd );
}
