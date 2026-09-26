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

#ifdef _LINUX
#include <link.h>        // dl_iterate_phdr, for the live module base
#else
// VirtualQuery is already used by objective.cpp the same way; declare it here
// rather than pulling in windows.h (which fights the SDK headers).
extern "C" __declspec(dllimport) unsigned long __stdcall VirtualQuery(
    const void *lpAddress, void *lpBuffer, unsigned long dwLength);
struct InsTurretMemBasicInfo
{
    void *BaseAddress;
    void *AllocationBase;
    unsigned long AllocationProtect;
    unsigned long RegionSize;
    unsigned long State;
    unsigned long Protect;
    unsigned long Type;
};
#define INS_TURRET_MEM_COMMIT 0x1000
#endif

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
//
// `insbot_fov` used to be borrowed here as the turret's acquisition cone. It is
// deliberately NOT any more: it is the bots' shared setting (default 120), and
// the turret has its own `turret_acquire_fov` (default 360). Re-adding the
// borrow would silently reintroduce a +/-60 degree blind spot behind the gun.
extern CfgVar insbot_vis_range;

// Shared smoke-occlusion predicate, defined in bot/bot_perceive.cpp and
// declared in bot.h. Declared here rather than including bot.h so this TU does
// not drag in the whole CInsBot surface.
extern bool LineGoesThroughSmoke( const Vector &from, const Vector &to );

int g_TurretOffsetDriver        = -1;
int g_TurretOffsetPoseParameter = -1;
int g_TurretOffsetAnimTime      = -1;

// CPropVehicleDriveable's ServerClass, cached so the per-frame sweep can test
// an entity's class with a pointer compare instead of a string walk.
static ServerClass *s_vehicleClass = 0;

// Which edict slots currently hold a driveable. Rebuilt on a timer by
// RebuildVehicleIndex(); the per-frame sweep reads it so it does not have to
// probe every edict on the map. Grown, never shrunk, so the index is always at
// least as long as the engine's entity count.
static unsigned char *s_vehicleIdx      = 0;
static int            s_vehicleIdxCount = 0;
static float          s_vehicleIdxNext  = 0.0f;
static int            s_vehicleIdxFound = -1;   // last logged count

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
static bool            TurretBarrelPose( CTurret *t, Vector &hingeOut,
                                         Vector &dirOut );
static edict_t        *EdictFromTraceEnt( CBaseEntity *ent );
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
// NOTE: turret_burst_time and turret_burst_pause were REMOVED here. The turret
// is automatic now - see the note in Fire(). They had been copied from the
// bots' own `burst_time` / `burst_pause`, which are dead settings (declared but
// never read), so the turret was pacing itself off a mechanism that no longer
// existed. Do not reintroduce a burst cycle without a reason that comes from
// the turret's own design rather than from the bots'.
CfgVar insbot_turret_scan_speed   ("turret_scan_speed",   "20",   "turret idle scan speed (deg/s)");
// The turret's own acquisition cone, deliberately separate from the bots'
// `fov` (default 120, `insbots.cpp:136`). The default here is a full circle:
// the ring can see every direction at once, so the idle sweep only decides
// where the gun points while nothing is visible, never what it is able to
// notice. Narrowing this below 360 restores a directional cone without a
// rebuild.
CfgVar insbot_turret_acquire_fov  ("turret_acquire_fov",  "360",  "turret acquisition cone (deg; 360=omnidirectional)");

// Fire-gate tolerances and spread. The design calls these tunable but not
// part of the core eight; they are CfgVars all the same so a playtest can
// adjust them without a rebuild.
CfgVar insbot_turret_fire_tol_yaw   ("turret_fire_tolerance_yaw",   "5.0",   "turret fire gate: max yaw error (deg)");
CfgVar insbot_turret_fire_tol_pitch ("turret_fire_tolerance_pitch", "4.0",   "turret fire gate: max pitch error (deg)");
CfgVar insbot_turret_spread         ("turret_spread",               "0.230", "turret bullet spread cone (deg; M249 real value)");

// Re-roll the aim offset on the bots' cadence. Called once per tick from
// FrameUpdate, before Aim(), so the offset a tick aims with is the one the
// re-roll for that tick produced.
//
// WHY THIS IS NOT IN Acquire(). Acquire() is throttled to 10 Hz and returns
// early while a target is held, so a roll there is either frozen for a whole
// engagement or re-rolled at 10 Hz during an LOS flicker. Neither is the bots'
// behaviour. This function is the only roll site, and it is cadence-gated.
//
// The 0.4-0.9 s window and the `rand()/RAND_MAX` shape are the bots' own
// `frand(0.4f, 0.9f)` at bot_think.cpp:861, written out here rather than
// calling frand() because that lives in bot/ and bot_internal.h declares itself
// private to that directory - and the turret deliberately borrows the bots'
// patterns instead of sharing their code (see the file header). The other
// random rolls in this file (the spread cone, in Fire) use rand() the same way.
//
// DEFINED HERE, BELOW THE CfgVar BLOCK, on purpose: it reads
// insbot_turret_aim_error, which does not exist above this point.
static void TurretRefreshAimError( CTurret *t, float now )
{
    float err = TurretValue( t, "turret_aim_error",
                             insbot_turret_aim_error.GetFloat() );

    // A change to the SETTING restarts the bounded SHOT diagnostic, so a live
    // run that sweeps several values gets a fresh sample budget per value. This
    // must sit ABOVE the cadence gate: the setting can change on any tick, not
    // only on a re-roll tick. Without it, the first value tested would spend the
    // whole budget at 800 RPM (~13 shots/s) and the later values would log
    // nothing at all.
    if (err != t->m_lastAimErrSetting)
    {
        t->m_lastAimErrSetting = err;
        t->m_shotsLogged = 0;
    }

    if (now < t->m_nextAimErrTime) return;

    // Same 0.4-0.9 s window as bot_think.cpp:861.
    t->m_nextAimErrTime = now + 0.4f
                        + 0.5f * ((float)rand() / (float)RAND_MAX);

    t->m_aimErrorYaw = InsTurretAimErrorRoll(
        InsTurretAimOffsetDeg( err, false ), (float)rand() / (float)RAND_MAX );
    t->m_aimErrorPitch = InsTurretAimErrorRoll(
        InsTurretAimOffsetDeg( err, true ),  (float)rand() / (float)RAND_MAX );
}

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
    m_nextShot = 0.0f;
    m_shotsLogged = 0;
    m_shotHitTarget = false;
    m_shotsFired = 0;
    m_rejectsLogged = 0;
    m_acquiresLogged = 0;
    m_scanBearing = 0.0f;
    m_aimErrorYaw = 0.0f;
    m_aimErrorPitch = 0.0f;
    m_nextAimErrTime = 0.0f;
    m_lastAimErrSetting = -1.0f;
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
    m_nextShot = 0.0f;
    m_shotsLogged = 0;
    m_shotHitTarget = false;
    m_shotsFired = 0;
    m_rejectsLogged = 0;
    m_acquiresLogged = 0;
    m_scanBearing = 0.0f;
    m_aimErrorYaw = 0.0f;
    m_aimErrorPitch = 0.0f;
    m_nextAimErrTime = 0.0f;
    m_lastAimErrSetting = -1.0f;
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
        TurretRefreshAimError( this, now ); // 2b - 0.4-0.9 s cadence, before Aim
        Acquire( now );     // 3 - throttled to ~10 Hz
        VerifyLOS();        // 4
        Aim( now );         // 5
        Traverse( dt );     // 6 - before fire, so the muzzle the player
        WritePose();        // 7   sees is the one the trace uses
        Fire( now );        // 8
    }
}

// ---------------------------------------------------------------------------
// Refresh the set of edict indices that currently hold a driveable.
//
// Split out of DriveAnimTime() so the expensive class test (GetUnknown, two
// virtual calls, one pointer compare) runs twice a second instead of every
// frame. The map is stable for long stretches, so this is close to free.
//
// The buffer is grown but never shrunk: a slot is always reachable, and a
// shrinking entity count (a level with fewer edicts) simply leaves a tail of
// stale falses that the per-frame loop never reads because it is bounded by
// the CURRENT entity count.
static void RebuildVehicleIndex( int maxEdicts )
{
    if (maxEdicts <= 0) return;

    if (s_vehicleIdxCount < maxEdicts)
    {
        // Ask for headroom (and round up) rather than an exact fit, so a map
        // whose entity count creeps up does not realloc on every rebuild.
        int want = maxEdicts + 256;
        want &= ~15;                        // round up to a 16-byte multiple

        unsigned char *grown = (unsigned char *)realloc( s_vehicleIdx, (size_t)want );
        if (!grown) return;                 // keep the old index on OOM
        s_vehicleIdx      = grown;
        s_vehicleIdxCount = want;
    }

    memset( s_vehicleIdx, 0, (size_t)s_vehicleIdxCount );

    int found = 0;
    for (int i = 1; i < maxEdicts; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex( i );
        if (!e || e->IsFree()) continue;

        IServerUnknown *unk = e->GetUnknown();
        if (!unk) continue;
        IServerNetworkable *net = unk->GetNetworkable();
        if (!net || !net->GetServerClass()) continue;

        // Compare the class POINTER, not the name: GetAllServerClasses() is a
        // list of unique classes, so an address match is a class test with no
        // per-entity string walk.
        if (net->GetServerClass() == s_vehicleClass)
        {
            s_vehicleIdx[ i ] = 1;
            found++;
        }
    }

    // Only on change: a map holds a handful of vehicles and they spawn and
    // despawn rarely, so this is a few lines per round rather than spam. It is
    // the only evidence that the sweep below has anything to act on.
    if (found != s_vehicleIdxFound)
    {
        s_vehicleIdxFound = found;
        Msg( "[insbots] vehicle interpolation: tracking %d driveable(s)\n", found );
    }
}

void CTurretRegistry::FrameUpdate( bool simulating )
{
    if (gpGlobals) Discover( gpGlobals->curtime );
    DriveAnimTime( simulating );

    // Iterate BACKWARD: FrameUpdate can remove the current entry (swap-with-
    // last), and a forward loop would then skip the swapped-in turret.
    for (int i = m_count - 1; i >= 0; i--)
        m_turrets[i].FrameUpdate( simulating );
}

// ---------------------------------------------------------------------------
// Advance m_flAnimTime on every driveable vehicle.
//
// THIS IS A WORKAROUND FOR A SHIPPED-CLIENT BUG, NOT FOR OUR MODEL.
//
// The client latches its interpolated variables (and therefore interpolates
// the pose parameters that drive the wheels, the steering and the turret) only
// when it sees m_flAnimTime change between packets - see
// C_BaseEntity::PostDataUpdate, which calls
// OnLatchInterpolatedVariables(LATCH_ANIMATION_VAR) on
// `m_flAnimTime != m_flOldAnimTime`.
//
// C_BaseEntity::PreDataUpdate normally refreshes that value itself:
//
//     if ( !IsSelfAnimating() )
//         m_flAnimTime = engine->GetLastTimeStamp();
//
// ...but the shipped client.dll has that branch INVERTED. Measured from
// client.dll at VA 0x1019B48E (file offset 0x19A88E), verified byte for byte:
//
//     0x1019B48E  8b 13               mov  edx, [ebx]
//     0x1019B490  8b 82 1c 01 00 00   mov  eax, [edx+0x11C]   ; IsSelfAnimating
//     0x1019B498  ff d0               call eax
//     0x1019B49A  5b                  pop  ebx
//     0x1019B49B  84 c0               test al, al
//     0x1019B49D  74 10               je   0x1019B4AF        ; skips the store
//     0x1019B4AC  d9 5e 58            fstp dword [esi+0x58]   ; m_flAnimTime
//
// and C_PropVehicleDriveable::IsSelfAnimating() is a constant false
// (client.dll VA 0x101C8150 = `32 c0 c3`, xor al,al; ret). So for every
// vehicle the store is skipped on every packet, m_flAnimTime stays pinned at
// whatever the server sent at spawn, the latch never fires, and
// m_iv_flPoseParameter is never interpolated.
//
// The visible result is the reported wheel defect: uninterpolated 20 Hz pose
// steps that read as a burnout while stopping, and a steering pose that stays
// wherever the last packet left it. Nothing here touches the physics - the
// server's own wheel simulation is correct, which is why the truck drives
// properly while the model looks wrong.
//
// Because the client only ever SKIPS the store, a server-side write is not
// clobbered: the value we send is the one the client keeps. Writing it here
// makes the comparison true, which restores the latch and with it normal
// interpolation for the whole entity - the vehicle's own pose parameters AND
// the turret's, which ride in the same array.
//
// Both values are resolved BY NAME in Load() and this no-ops when either is
// missing, so a layout change fails closed instead of scribbling on a
// neighbouring field.
//
// ONE write per vehicle per frame, at gpGlobals->curtime, which is exactly
// what the engine's own CBaseAnimating::StudioFrameAdvance() assigns. The
// sent value is truncated to TICK_INTERVAL by the 8-bit proxy
// (SendProxy_AnimTime), so at any tick rate this collapses to a tick count
// and the client reconstructs tick * TICK_INTERVAL: one distinct value per
// tick, which is precisely the cadence the interpolator wants.
//
// NOT DONE, deliberately:
//   - No NetworkStateChanged()/edict flags. The animation table is marked
//     SPROP_CHANGES_OFTEN, and every driveable here is either moving or
//     having its pose parameters written by CFourWheelVehiclePhysics every
//     tick, so the entity is already in the packet. Forcing a full update
//     would ship every property of every vehicle every tick for no gain.
//   - No "only while driven" gate. A parked truck still has its steering and
//     wheel poses written by the physics each tick, so it can show the same
//     unlatched stepping, and the truck you are watching while it rolls to a
//     stop is the case that was reported.
void CTurretRegistry::DriveAnimTime( bool simulating )
{
    if (!simulating) return;
    if (!engine || !gpGlobals) return;
    if (g_TurretOffsetAnimTime < 0) return;

    // Bounded: gpGlobals->frametime is 0 whenever a frame is skipped or the
    // server is not simulating, and a hitch must not stamp a large jump into
    // the interpolation clock.
    float dt = gpGlobals->frametime;
    if (dt <= 0.0f || dt > 0.25f) return;

    // Resolve the class once, lazily rather than in Load(), so a map with no
    // vehicles costs nothing and a late interface still resolves. A failed
    // lookup is NOT latched: it simply retries next frame.
    if (!s_vehicleClass)
    {
        if (!server) return;
        for (ServerClass *c = server->GetAllServerClasses(); c; c = c->m_pNext)
            if (IsDriveableClass( c->GetName() ))
            {
                s_vehicleClass = c;
                break;
            }
        if (!s_vehicleClass) return;
    }

    const float now = gpGlobals->curtime;
    const int maxEdicts = engine->GetEntityCount();

    // Rebuild the vehicle index set on a timer rather than every frame. The
    // test itself is a class-pointer compare, but reaching it costs a
    // GetUnknown() plus two virtual calls per edict, and a large map runs
    // thousands of edicts for a handful of vehicles.
    if (now >= s_vehicleIdxNext)
    {
        s_vehicleIdxNext = now + 0.5f;
        RebuildVehicleIndex( maxEdicts );
    }
    if (!s_vehicleIdx) return;

    // Entity 0 is the world; vehicles are non-client entities, so the upper
    // bound is the engine's own entity count and not maxClients.
    for (int i = 1; i < maxEdicts; i++)
    {
        if (i >= s_vehicleIdxCount || !s_vehicleIdx[ i ]) continue;

        edict_t *e = engine->PEntityOfEntIndex( i );
        if (!e || e->IsFree()) continue;

        IServerUnknown *unk = e->GetUnknown();
        if (!unk) continue;
        void *pVehicle = unk->GetBaseEntity();
        if (!pVehicle) continue;

        // The member is a FIELD_TIME float and the SendProxy loads it with
        // `fld dword ptr [eax+0x28]`, so this has to be written as seconds.
        // (Writing a tick count here would be reinterpreted as a denormal
        // float and truncated to 0 by TIME_TO_TICKS.)
        *(float *)((char *)pVehicle + g_TurretOffsetAnimTime) = now;
    }
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

static InsTurretTEDispatchEffectFn InsTurretTEDispatchEffect();

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

// ---- broadcast recipient filter for the turret's gunshot and tracer --------
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

// Fire a REAL Insurgency M249 red tracer.
//
// This is the client's own effect-dispatch path, not a particle imitation.
// The plugin hands a DispatchEffect temp-entity to the game DLL, which sends
// it to every client in `filter`; each client runs its registered
// "Tracer" callback, which calls FX_Tracer, which maps m_nDamageType == 3 to
// "effects/redflare" - the material player-fired M249 rounds use.
//
// Everything here was measured, not assumed. See
// docs/humvee-turret-muzzle-tracer-feasibility.md:
//   section 2 - why routes A/B/B2/C/D were rejected and E2 chosen
//   section 3 - TracerCallback (client.dll 0x100B8A30) and FX_Tracer
//               (0x100B87D0), including the material table and the 128-unit
//               distance gate
//   section 4 - the CEffectData layout, audited against three binaries
//
// THREE FIELDS ARE LOAD-BEARING, and getting any of them wrong makes the
// effect silently invisible rather than failing loudly:
//
//   m_vStart != m_vOrigin   FX_Tracer returns early if the two points are
//                           closer than 128 units. (This is exactly why the
//                           AcceptInput("DispatchEffect") route cannot work:
//                           InputDispatchEffect sets m_vStart = m_vOrigin, so
//                           the distance is always 0.)
//   m_nDamageType == 3      iTracerType. 0 is TRACERTYPE_NONE, which returns
//                           before drawing anything at all.
//   m_nHitBox == 0          leaves GetTracerOrigin using m_vStart verbatim
//                           instead of overriding it from a weapon attachment.
static void TurretSendTracer( const Vector &from, const Vector &to,
                              const Vector &dir, int entIndex )
{
    // A short shot is not worth a tracer, and FX_Tracer would drop it anyway.
    // Check here so we do not pay for a temp-entity the client will discard.
    Vector delta = to - from;
    if (InsTurretNormalize( delta.x, delta.y, delta.z ) < 128.0f) return;

    InsTurretTEDispatchEffectFn send = InsTurretTEDispatchEffect();
    if (!send) return;

    InsTurretEffectData data;
    memset( &data, 0, sizeof(data) );

    data.m_vOrigin          = *(const InsTurretVector *)&to;
    data.m_vStart           = *(const InsTurretVector *)&from;
    data.m_vNormal          = *(const InsTurretVector *)&dir;
    data.m_nEntIndex        = entIndex;
    data.m_nDamageType      = INS_TURRET_TRACERTYPE_RED;
    data.m_bWhiz            = 1;    // near-miss crack for players on the line
    data.m_flScale          = 0.0f; // let FX_Tracer apply its own 5000 u/s

    // Broadcast to every connected client, not a PAS filter. The impact point
    // is frequently far from the truck and often occluded; a PAS filter built
    // from it would drop the tracer for the players most likely to be looking
    // at the fight. This is the same filter the gunshot already uses, so the
    // tracer and the report always reach the same audience.
    CTurretSoundFilter filter;
    send( filter, 0.0f, from, "Tracer", data );
}

static void TurretImpact( const Vector &pos, const Vector &dir )
{
    if (g_pEffects) g_pEffects->MetalSparks( pos, dir );
}

static void TurretSound( const Vector &pos, int entIndex )
{
    if (!enginesound) return;
    // Emit from the vehicle's own entity index, using the M249's report so the
    // turret is identifiable by ear.
    //
    // CPASFilter is NOT usable here: it lives in the game DLL
    // (dlls/recipientfilter.h:125) and a standalone VSP cannot link it. This is
    // the same local IRecipientFilter subclass voice.cpp:80 uses - every
    // connected client, with the engine doing the positional culling.
    //
    // THE NAME MUST BE A RAW .wav PATH, NOT A SOUNDSCRIPT NAME.
    // IEngineSound exposes only PrecacheSound(pSample) - there is no
    // PrecacheScriptSound on this interface (the SDK's PrecacheScriptSound is a
    // CBaseEntity static inside the game DLL). Passing the soundscript name
    // "Weapon_M249.Single" therefore precached nothing, and EmitSound's lookup
    // of it failed every shot:
    //
    //   SV_StartSound: Weapon_M249.Single not precached (0)
    //   Failed to load sound "weapon_m249.single", file probably missing...
    //
    // "Weapon_M249.Single" is a key in scripts/game_sounds_weapons.txt whose
    // rndwave points at ^weapons/m249/m249-1.wav, so that .wav is what must be
    // precached and emitted. This mirrors the known-good voice.cpp:117/159,
    // which precaches and emits the same raw wave path.
    static const char *kM249Shot = "weapons/m249/m249-1.wav";
    enginesound->PrecacheSound( kM249Shot, true );
    CTurretSoundFilter filter;
    enginesound->EmitSound( filter, entIndex, CHAN_WEAPON,
                            kM249Shot, 1.0f, ATTN_NORM, 0, PITCH_NORM );
}

// Shell ejection, route C: fire the vehicle's own DispatchEffect input once
// per shot. The plugin supplies only a string; the game DLL resolves the
// attachment, builds CEffectData, and sends the temp entity with its own
// singleton sender. See docs/humvee-turret-shell-eject-verification.md.
// ---------------------------------------------------------------------------
// Live module base for the game DLL (server.dll / server_i486.so).
//
// INS_TURRET_ALLOCPOOLEDSTRING_RVA is an RVA, not an address. The shipped
// server.dll has a preferred base of 0x10000000, so the original constant was
// written as 0x1007DC10 - and that is only correct when the loader happens to
// honour the preferred base. A dedicated server often does; a listen server
// does not. Measured on the live install, server.dll loaded at 0x0EEC0000 and
// 0x1007DC10 was NOT mapped in the process at all, so the eject call would
// jump into unmapped memory. Linux is worse: server_i486.so is an ELF ET_DYN
// (PIE) and is always relocated.
//
// Resolution is fail-closed: if the base cannot be established, or the target
// is not committed executable memory, the caller gets 0 and skips the eject
// rather than jumping somewhere arbitrary.
// ---------------------------------------------------------------------------

#ifdef _LINUX
struct InsTurretPhdrSearch
{
    unsigned long base;      // relocation base of the game DLL
    unsigned long execLo;    // RVA range of its executable PT_LOAD segment
    unsigned long execHi;
    unsigned long bestSize;  // total PT_LOAD memsz of the best candidate
    bool          found;
};

static int InsTurretPhdrCallback( struct dl_phdr_info *info, size_t, void *data )
{
    InsTurretPhdrSearch *s = (InsTurretPhdrSearch *)data;
    const char *name = info->dlpi_name ? info->dlpi_name : "";
    const char *leaf = strrchr( name, '/' );
    leaf = leaf ? leaf + 1 : name;
    if (strcmp( leaf, "server_i486.so" ) != 0) return 0;

    // TWO modules share this filename: the ~9 MB game DLL
    // (insurgency/bin/server_i486.so) and the ~400 KB MetaMod shim
    // (addons/metamod/bin/server_i486.so). Matching the first hit could pick
    // the shim, whose base + 0x44256E is far outside its own image - a jump
    // into unmapped memory. So score every candidate by total PT_LOAD size and
    // keep the largest, and remember that image's executable segment so the
    // caller can prove the target RVA really is code.
    unsigned long total = 0, lo = 0, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; i++)
    {
        const Elf32_Phdr *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD) continue;
        total += ph->p_memsz;
        if (ph->p_flags & PF_X)
        {
            if (lo == 0 || ph->p_vaddr < lo) lo = ph->p_vaddr;
            if ((unsigned long)(ph->p_vaddr + ph->p_memsz) > hi)
                hi = (unsigned long)(ph->p_vaddr + ph->p_memsz);
        }
    }
    if (total > s->bestSize)
    {
        s->bestSize = total;
        s->base     = (unsigned long)info->dlpi_addr;
        s->execLo   = lo;
        s->execHi   = hi;
        s->found    = true;
    }
    return 0;                // keep looking; the biggest wins
}

static unsigned long InsTurretGameDllBase()
{
    InsTurretPhdrSearch s;
    s.base = 0; s.execLo = 0; s.execHi = 0; s.bestSize = 0; s.found = false;
    dl_iterate_phdr( InsTurretPhdrCallback, &s );
    if (!s.found) return 0;

    // Prove the RVA lands inside that image's executable segment before we
    // ever hand it out as a function pointer. Fail closed.
    const unsigned long rva = INS_TURRET_ALLOCPOOLEDSTRING_RVA;
    if (rva < s.execLo || rva >= s.execHi) return 0;
    return s.base;
}

static bool InsTurretIsExecutable( unsigned long addr )
{
    // The RVA/segment check in InsTurretGameDllBase is the real guard; this is
    // the final "did the addition wrap or land outside user space" belt.
    return addr != 0 && addr < 0xC0000000ul;
}
#else
// ---- Windows: walk the loader's own module list out of the PEB ------------
// We cannot link kernel32's GetModuleHandle here (the DLL deliberately imports
// only msvcrt + kernel32 with a hand-written import lib, and adding a second
// import entry risks the load path that has already bitten this project), so
// read the documented PEB fields directly.
//
//   fs:[0x30] -> PEB
//   PEB+0x0C  -> PEB_LDR_DATA
//   Ldr+0x0C  -> InLoadOrderModuleList (LIST_ENTRY)
//   LDR_DATA_TABLE_ENTRY: +0x18 DllBase, +0x2C BaseDllName (UNICODE_STRING),
//                         +0x30 FullDllName
// These offsets are fixed for 32-bit Windows XP..10 and are the same ones the
// repo already relies on indirectly through its SendTable walks.
struct InsTurretUnicodeString
{
    unsigned short Length;
    unsigned short MaximumLength;
    unsigned short *Buffer;
};

struct InsTurretLdrEntry
{
    unsigned long       InLoadOrderLinks[2];
    unsigned long       InMemoryOrderLinks[2];
    unsigned long       InInitializationOrderLinks[2];
    unsigned long       DllBase;
    unsigned long       EntryPoint;
    unsigned long       SizeOfImage;
    InsTurretUnicodeString FullDllName;
    InsTurretUnicodeString BaseDllName;
};

static bool InsTurretLeafIsServerDll( const unsigned short *buf, unsigned short lenChars )
{
    // Compare the leaf name against "server.dll", case-insensitively.
    const char *want = "server.dll";
    int wlen = 10;

    // find the last '\' or '/' in the wide name
    int leaf = 0;
    for (int i = 0; i < (int)lenChars; i++)
    {
        if (buf[i] == '\\' || buf[i] == '/') leaf = i + 1;
    }
    int n = (int)lenChars - leaf;
    if (n != wlen) return false;
    for (int i = 0; i < wlen; i++)
    {
        unsigned short c = buf[leaf + i];
        if (c >= 'A' && c <= 'Z') c = (unsigned short)(c - 'A' + 'a');
        if (c != (unsigned short)want[i]) return false;
    }
    return true;
}

static unsigned long InsTurretGameDllBase()
{
    unsigned long peb = 0;
    __asm
    {
        mov eax, fs:[0x30]
        mov peb, eax
    }
    if (!peb) return 0;

    // PEB->Ldr at +0x0C, Ldr->InLoadOrderModuleList at +0x0C
    const unsigned long ldr        = *(const unsigned long *)(peb + 0x0C);
    if (!ldr) return 0;
    const unsigned long listHead   = ldr + 0x0C;

    unsigned long link = *(const unsigned long *)listHead;
    unsigned long bestBase = 0;
    unsigned long bestSize = 0;
    // Bound the walk: a corrupt list must not spin forever.
    for (int guard = 0; guard < 512 && link && link != listHead; guard++)
    {
        const InsTurretLdrEntry *e = (const InsTurretLdrEntry *)link;
        if (e->BaseDllName.Buffer && e->BaseDllName.Length)
        {
            unsigned short lenChars = (unsigned short)(e->BaseDllName.Length / 2);
            if (InsTurretLeafIsServerDll( e->BaseDllName.Buffer, lenChars ))
            {
                // Two modules are called server.dll here as well: the ~3 MB
                // game DLL (insurgency/bin/server.dll) and the ~180 KB MetaMod
                // shim (addons/metamod/bin/server.dll). Taking the first match
                // could pick the shim, whose base + 0x7DC10 lands outside its
                // image. Keep the largest image and check the RVA is inside it.
                if (e->SizeOfImage > bestSize)
                {
                    bestSize = e->SizeOfImage;
                    bestBase = e->DllBase;
                }
            }
        }
        link = *(const unsigned long *)link;
    }
    // Fail closed: the RVA must lie inside the chosen image.
    if (bestBase == 0) return 0;
    if (INS_TURRET_ALLOCPOOLEDSTRING_RVA >= bestSize) return 0;
    return bestBase;
}

static bool InsTurretIsExecutable( unsigned long addr )
{
    if (addr == 0) return false;
    InsTurretMemBasicInfo mbi;
    memset( &mbi, 0, sizeof(mbi) );
    if (!VirtualQuery( (const void *)addr, &mbi, sizeof(mbi) )) return false;
    if (mbi.State != INS_TURRET_MEM_COMMIT) return false;
    return true;
}
#endif

static InsTurretAllocPooledStringFn InsTurretAllocPooledString()
{
    static InsTurretAllocPooledStringFn cached = 0;
    static bool resolved = false;
    if (resolved) return cached;
    resolved = true;

    unsigned long base = InsTurretGameDllBase();
    if (base == 0) return 0;

    unsigned long addr = base + INS_TURRET_ALLOCPOOLEDSTRING_RVA;
    if (!InsTurretIsExecutable( addr )) return 0;

    cached = (InsTurretAllocPooledStringFn)addr;
    return cached;
}

// Resolve the game DLL's own TE_DispatchEffect.
//
// WHY THE GAME DLL AND NOT AN ENGINE INTERFACE:
// the engine's IEffects is either absent or, where present, its client half is
// an empty stub (CEffectsClient::Beam 0x1021FB20 = `ret 0x40`). The real
// tracer lives in the CLIENT's effect-dispatch callback table, which is only
// reachable by sending a DispatchEffect temp-entity - and the game DLL already
// owns that send path. See docs/humvee-turret-muzzle-tracer-feasibility.md
// section 5.1 for why routes A, B, C and D were all rejected.
//
// FAIL CLOSED. On Windows this is an RVA, so a mismatched server.dll would
// make this a jump into unrelated code. The prologue check below is not
// optional: it is the difference between "no tracer" and a server crash.
#ifdef _LINUX
// On Linux the symbol is exported, so resolve it by name. This is strictly
// better than the RVA: it survives a rebuilt server_i486.so. dlsym with a
// NULL handle searches the global symbol table, which is where the game DLL's
// exported symbols live.
#include <dlfcn.h>
static void *InsTurretResolveTEDispatchEffectSymbol()
{
    return dlsym( (void *)0,
                  "_Z17TE_DispatchEffectR16IRecipientFilterfRK6VectorPKcRK11CEffectData" );
}
#endif

static InsTurretTEDispatchEffectFn InsTurretTEDispatchEffect()
{
    static InsTurretTEDispatchEffectFn cached = 0;
    static bool resolved = false;
    if (resolved) return cached;
    resolved = true;

    unsigned long addr = 0;

#ifdef _LINUX
    addr = (unsigned long)InsTurretResolveTEDispatchEffectSymbol();
    if (addr == 0) return 0;
    if (!InsTurretIsExecutable( addr )) return 0;
#else
    unsigned long base = InsTurretGameDllBase();
    if (base == 0) return 0;
    addr = base + INS_TURRET_TEDISPATCHEFFECT_RVA;
    if (!InsTurretIsExecutable( addr )) return 0;

    // Prologue check. The audited server.dll (MD5
    // 7B0CAEA9F6E0B6C52F6D49A808F4C9B6) begins:
    //   8b 44 24 14   mov eax, [esp+0x14]   <- the `data` argument
    //   50            push eax
    //   b9 xx xx xx xx  mov ecx, <static CEffectData it copies into>
    //
    // The third instruction's immediate is NOT a stable byte pattern. The
    // shipped server.dll has a BASE RELOCATION at RVA 0x12C796 covering it
    // (verified with pefile: IMAGE_REL_BASED_HIGHLOW), so the loader rewrites
    // the operand to (live_base + 0x2B0F24) whenever the module loads anywhere
    // other than its 0x10000000 preferred base. A listen server always does:
    // measured live at 0x0EEC0000, where the on-disk literal 0x102B0F24 is
    // wrong and a literal comparison fails every time, silently disabling the
    // tracer. So compare the first two instructions as bytes, then check the
    // third instruction's opcode and its REBASED operand.
    //
    // If those do not match, this is a DIFFERENT server.dll and the RVA does
    // not describe this function. Refuse to call through it.
    const unsigned char *p = (const unsigned char *)addr;
    const unsigned char want[6] = { 0x8b, 0x44, 0x24, 0x14, 0x50, 0xb9 };
    bool ok = true;
    for (int i = 0; i < 6; i++)
        if (p[i] != want[i]) ok = false;

    unsigned long tableAddr = 0;
    if (ok)
    {
        // Unaligned 4-byte load, matching the instruction's own encoding.
        tableAddr = (unsigned long)p[6]
                  | ((unsigned long)p[7] << 8)
                  | ((unsigned long)p[8] << 16)
                  | ((unsigned long)p[9] << 24);

        const unsigned long wantTable = base + INS_TURRET_TEDISPATCHEFFECT_TABLE_RVA;
        if (tableAddr != wantTable) ok = false;
    }

    if (!ok)
    {
        Msg( "[insbots] turret: TE_DispatchEffect prologue mismatch at %p "
             "- server.dll differs from the audited build; tracer disabled\n",
             (void *)addr );
        return 0;
    }
#endif

    // Log the successful resolution exactly once (the cache above guarantees
    // that). Without it, "resolver returned 0" and "tracer sent but invisible"
    // are indistinguishable in the log - the exact ambiguity that let the
    // prologue bug above hide a working effect behind a silent early return.
    Msg( "[insbots] turret: TE_DispatchEffect resolved at %p\n", (void *)addr );

    cached = (InsTurretTEDispatchEffectFn)addr;
    return cached;
}

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
    //
    // The address is REBASED off the live game-DLL base every session; the
    // constant is an RVA, not an absolute address. Fail closed.
    InsTurretAllocPooledStringFn allocFn = InsTurretAllocPooledString();
    if (!allocFn) return;
    const char *pooled = 0;
    allocFn( &pooled, spec );
    if (!pooled || !pooled[0]) return;

    // variant_t: 20 bytes, union at +0x00, CHandle at +0x0C, fieldType at
    // +0x10. The zeroed tail is not cosmetic - AcceptInput may copy this into
    // an inputdata_t. FIELD_STRING is 2 (datamap.h:28-31).
    InsTurretVariant value;
    memset( &value, 0, sizeof(value) );
    value.m_union0    = (unsigned long)pooled;
    value.m_eVal      = 0;                          // CHandle INVALID
    value.m_fieldType = INS_TURRET_FIELD_STRING;

    InsTurretAcceptInputFn fn = (InsTurretAcceptInputFn)
        TurretGetVtableSlot( pVehicle, INS_TURRET_ACCEPTINPUT_SLOT );
    if (!fn) return;

    // pActivator is NULL: InputDispatchEffect never reads it. pCaller is the
    // vehicle itself, which only affects the entity-i/o debug line.
    //
    // PER-PLATFORM ARGUMENT FORM - see the long note in turret_abi.h.
    //   Linux  : GCC passes the non-trivial variant_t by invisible reference,
    //            so we hand it a pointer (4-byte argument, 6 args total).
    //   Windows: MSVC passes it inline (20 bytes, 9 argument dwords) and the
    //            callee's `ret 0x24` pops all 36 bytes itself, so there must be
    //            NO `add esp` after the call. The typedef encodes __thiscall;
    //            do not "fix" a stack imbalance here.
#ifdef _LINUX
    fn( pVehicle, "DispatchEffect", 0, pVehicle, &value, 0 );
#else
    fn( pVehicle, "DispatchEffect", 0, pVehicle, value, 0 );
#endif
}

// The firing gate, burst timing, and the whole shot. Ordering is deliberate:
// the pose was written in step 7 before this runs, so the muzzle the player
// sees is the one the trace uses.
void CTurret::Fire( float now )
{
    // One-shot rejection diagnostic. "Never fires" has six distinct causes and
    // they are indistinguishable from the outside, so the first few rejections
    // per turret are logged with the reason. Bounded so it cannot flood.
    #define INS_TURRET_REJECT(why) \
        do { if (m_rejectsLogged < 6) { m_rejectsLogged++; \
             Msg( "[insbots] turret %d FIRE-BLOCKED: %s (target=%d res=%.1f/%.1f " \
                  "tol=%.1f/%.1f dist=%.0f)\n", m_entIndex, why, m_target, \
                  m_residualYaw, m_residualPitch, \
                  insbot_turret_fire_tol_yaw.GetFloat(), \
                  insbot_turret_fire_tol_pitch.GetFloat(), \
                  (m_targetLastPos - TurretMuzzlePosition(this)).Length() ); } \
             return; } while (0)

    if (!insbot_turret_enabled.GetBool()) return;
    if (!m_target) return;
    if (TurretHullInverted( this )) INS_TURRET_REJECT( "hull inverted/collideable null" );

    // FIRE RATE - the only thing pacing the gun.
    //
    // There used to be a burst/pause state machine here (0.30 s firing,
    // 0.45 s pause) driven by turret_burst_time / turret_burst_pause. Both
    // settings are GONE, deliberately, and the gun is now automatic: while it
    // holds a target and the fire gate is satisfied, it fires continuously at
    // the M249's sustained rate.
    //
    // Why the burst cycle was removed: it was never a design decision. Those
    // two CfgVars were copied from the BOT settings `burst_time` /
    // `burst_pause`, which are themselves dead - they are declared in
    // insbots.cpp and read by nothing, and the bot field that once consumed
    // them (m_fireStateEnd, bot.h) is likewise never touched. So the turret
    // inherited a rhythm from a mechanism that no longer exists on the bot
    // side, and the only effect of keeping it was to cap the gun at a 40% duty
    // cycle for no reason.
    //
    // The old cycle also had a real failure mode: the burst window was
    // consumed by FRAMES, not by shots, and the fire gate (aim/LOS) is
    // evaluated further down. A burst that elapsed while the gate was
    // rejecting frames therefore produced fewer shots than the window
    // implied - and at a very short burst_time it could expire without firing
    // at all.
    //
    // What replaces it is the per-shot cooldown below, which is a property of
    // the weapon rather than a policy: 800 RPM is the M249's sustained rate.
    // Without a cooldown the fire loop runs once PER FRAME (60+ rounds/second),
    // which is neither the real cadence nor kind to the temp-entity budget.
    if (now < m_nextShot) return;
    m_nextShot = now + (60.0f / 800.0f);        // 0.075 s between rounds

    // Fire gate: aligned to the tolerance AND with a clear line of sight AND
    // within engagement range. The inherited bot gate is +/-18 deg, which is
    // ~400 units of lateral slop at 1500 units - far too loose here.
    float tolYaw   = insbot_turret_fire_tol_yaw.GetFloat();
    float tolPitch = insbot_turret_fire_tol_pitch.GetFloat();
    tolYaw   = TurretValue( this, "turret_fire_tolerance_yaw",   tolYaw );
    tolPitch = TurretValue( this, "turret_fire_tolerance_pitch", tolPitch );
    if (!InsTurretAimed( m_residualYaw, m_residualPitch, tolYaw, tolPitch ))
        INS_TURRET_REJECT( "not aimed" );
    if (!VerifyLOS()) INS_TURRET_REJECT( "no line of sight from muzzle" );

    edict_t *te = engine->PEntityOfEntIndex( m_target );
    if (!te || te->IsFree()) INS_TURRET_REJECT( "target edict gone" );
    IPlayerInfo *pi = playerinfomanager ? playerinfomanager->GetPlayerInfo( te ) : 0;
    // IsAlive() does not exist on this interface - see TurretIsValidEnemy.
    if (!pi || pi->IsDead()) INS_TURRET_REJECT( "target dead/not a player" );

    Vector from = TurretMuzzlePosition( this );
    Vector aim  = m_targetLastPos;
    float  dist = ( aim - from ).Length();
    if (dist > TurretValue( this, "turret_engage_range",
                            insbot_turret_engage_range.GetFloat() ))
        INS_TURRET_REJECT( "out of engage range" );  // firing gate

    // THE ROUND LEAVES ALONG THE BARREL. This is the whole of the aim error's
    // mechanical effect: m_curYaw/m_curPitch already carry m_aimErrorYaw/Pitch,
    // because Aim() added them to the desired angles and Traverse() stepped the
    // pose onto them. Tracing along that same direction therefore sends the
    // round where the gun is actually pointing - which is up to `turret_aim_error`
    // degrees off the truth - instead of at the truth while the barrel shows
    // something else.
    //
    // Before this, `dir` was `aim - from`: the direction to the TARGET. The
    // barrel visibly pointed elsewhere, the round ignored it, and
    // `turret_aim_error 0` and `turret_aim_error 20` were identically lethal.
    //
    // The miss distance is dist * sin(error): ~56 units at 800 units and 4 deg,
    // ~166 units at 12 deg - both wider than a player hull.
    //
    // `from` is deliberately still TurretMuzzlePosition(), and `dir` comes from
    // the same TurretBarrelPose() that function uses, so the muzzle and the
    // round share one direction by construction.
    Vector hinge, dir;
    if (!TurretBarrelPose( this, hinge, dir ))
        INS_TURRET_REJECT( "no collideable for the barrel direction" );

    // Spread: a small cone, defaulting to the M249's real spread_degrees.
    // Applied ON TOP of the barrel direction, so the two error terms compose:
    // the aim error is a sustained offset, the spread is per-round jitter.
    float spread = TurretValue( this, "turret_spread", insbot_turret_spread.GetFloat() );
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
    // THE TRACE DECIDES. Before this, damage was applied to m_target whether or
    // not the round reached it, so a shot into a wall still hurt the player
    // behind it and the aim error could never produce a miss even in principle.
    //
    // The turret's own filter (CTurretTraceFilter + TurretBuildSkipSet) already
    // excludes the vehicle and any boarded player, so "the trace reached the
    // target" is the complete hit test - there is no second collision layer to
    // consult, and a rider still cannot be shot (design section 6.4).
    edict_t *hitEdict = EdictFromTraceEnt( tr.m_pEnt );
    bool hitTarget = ( hitEdict != 0 &&
                       hitEdict == engine->PEntityOfEntIndex( m_target ) );
    if (hitTarget)
    {
        TurretApplyDamage( this, m_target, dir,
                           TurretValue( this, "turret_damage",
                                        insbot_turret_damage.GetFloat() ) );
    }
    m_shotHitTarget = hitTarget;
    // Real M249 tracer, on the mod's own 1-in-3 cadence. The shot counter is
    // incremented for every round fired, tracer or not, so the pattern does
    // not drift when shots are skipped.
    m_shotsFired++;
    if ((m_shotsFired % 3) == 0)
        TurretSendTracer( from, hitPos, dir, m_entIndex );
    if (tr.fraction < 1.0f) TurretImpact( tr.endpos, dir );
    TurretSound( from, m_entIndex );
    TurretEjectShell( this );

    // Shot diagnostic. Without this there is no way to tell "the turret never
    // fired" from "it fired and nothing was visible" - both look identical from
    // the client.
    //
    // Bounded to 40 per SETTING VALUE rather than 6 per entity lifetime. The
    // live probe runs three six-second phases, and at 800 RPM (~13.3 shots/s) a
    // single phase can produce ~80 shots - so a lifetime budget would be spent
    // by the first phase and the later ones would log nothing at all.
    // TurretRefreshAimError resets this counter whenever `turret_aim_error`
    // changes, so 40 is per value; the bound still holds because a value only
    // ever gets 40 lines no matter how long it is left in place.
    if (m_shotsLogged < 40)
    {
        m_shotsLogged++;
        // `frac=` is the trace fraction and `hit=` is the VERDICT. They were
        // both called `hit=` once, which made "the trace stopped at 0.98"
        // indistinguishable from "the round reached the target".
        Msg( "[insbots] turret %d SHOT %d target=%d dist=%.0f frac=%.2f "
             "hit=%s err=%.1f/%.1f effects=%d\n",
             m_entIndex, m_shotsLogged, m_target, dist, tr.fraction,
             m_shotHitTarget ? "target" : "miss",
             m_aimErrorYaw, m_aimErrorPitch,
             g_pEffects ? 1 : 0 );
    }
    #undef INS_TURRET_REJECT
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

// Bounded diagnostic counter for the damage path. "No damage" has several
// distinct causes (no slot, rejected by the gamerules gate, wrong struct
// offsets) and they are indistinguishable without measuring the target's
// health across the call.
static int s_damageLogs = 0;

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
    // __thiscall on Windows; see InsTurretGetRefEHandleFn in turret_abi.h. A
    // plain cdecl typedef here compiles fine and returns garbage, because the
    // callee takes `this` from ECX and ignores the pushed argument entirely.
    InsTurretGetRefEHandleFn fn =
        (InsTurretGetRefEHandleFn)TurretGetVtableSlot( pEnt, kGetRefEHandleSlot );
    if (!fn) return 0xFFFFFFFFul;
    const unsigned long *pHandle = (const unsigned long *)fn( pEnt );
    return pHandle ? *pHandle : 0xFFFFFFFFul;
}

static void TurretApplyDamage( CTurret *t, int targetIdx, const Vector &dir, float dmg )
{
    // Every early return below is a SILENT no-damage path. They are logged
    // (bounded) because "the gun fires but nothing takes damage" is exactly
    // what one of them produces, and without a line here there is no way to
    // tell which one fired.
    #define INS_TURRET_DMG_BAIL(why) \
        do { if (s_damageLogs < 8) { s_damageLogs++; \
             Msg( "[insbots] turret DAMAGE-BLOCKED: %s (target=%d slot=%d)\n", \
                  why, targetIdx, INS_TURRET_OTD_SLOT ); } return; } while (0)

    edict_t *te = engine->PEntityOfEntIndex( targetIdx );
    if (!te || te->IsFree()) INS_TURRET_DMG_BAIL( "target edict gone" );
    IServerUnknown *unk = te->GetUnknown();
    if (!unk) INS_TURRET_DMG_BAIL( "target has no IServerUnknown" );
    void *pTarget = unk->GetBaseEntity();
    if (!pTarget) INS_TURRET_DMG_BAIL( "target has no CBaseEntity" );

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
        INS_TURRET_DMG_BAIL( "attacker handle invalid (GetRefEHandle slot wrong?)" );
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
    if (!fn)
    {
        if (s_damageLogs < 8) { s_damageLogs++;
            Msg( "[insbots] turret DAMAGE: no OnTakeDamage slot %d on target %d\n",
                 INS_TURRET_OTD_SLOT, targetIdx ); }
        return;
    }

    // Measure the health before/after so "no damage" is a number, not a guess.
    // IPlayerInfo::GetHealth() is the same accessor bot_perceive.cpp:575 uses.
    IPlayerInfo *tpi = playerinfomanager ? playerinfomanager->GetPlayerInfo( te ) : 0;
    int hpBefore = tpi ? tpi->GetHealth() : -1;

    int ret = fn( pTarget, &info );

    int hpAfter = tpi ? tpi->GetHealth() : -1;
    if (s_damageLogs < 8)
    {
        s_damageLogs++;
        Msg( "[insbots] turret DAMAGE: target=%d dmg=%.1f attacker=0x%08lX "
             "slot=%d ret=%d hp %d -> %d  %s\n",
             targetIdx, dmg, attackerHandle, INS_TURRET_OTD_SLOT, ret,
             hpBefore, hpAfter,
             (hpAfter >= 0 && hpBefore >= 0 && hpAfter < hpBefore) ? "LANDED" : "NO EFFECT" );
    }
    #undef INS_TURRET_DMG_BAIL
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

// The elevation hinge in world space and the barrel's world direction, both
// from the turret's LIVE pose (m_curYaw / m_curPitch - the angles the pose
// parameter is actually driving, which include the aim error).
//
// These two values are deliberately produced together. The muzzle is the hinge
// offset along the direction, and the firing trace uses that same direction, so
// "where the barrel is" and "where the round goes" cannot disagree.
//
// CONVENTION - this must match what Aim() produces and what WritePose()
// stores, because m_flPoseParameter is a LOCAL rotation relative to the hull.
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
// THIS WAS WRONG ONCE. The direction previously read `(sin, -cos)`, which is the
// mirror of the above: at yaw 0 it pointed local -Y, i.e. 180 degrees out from
// both Aim() and the FOV test. The comment justifying it ("the mesh's front is
// -Y") came from the spawner's facing note, which is about where the ENGINE
// expects a vehicle's front to sit for steering purposes - not about where
// this mesh's barrel points. The error put the computed muzzle ~62 units
// behind the ring, so the tracer, the muzzle flash and the LOS trace origin
// all sat inside or behind the cab. Damage was unaffected because the
// firing trace was drawn from the target side.
//
// See temp/humvee_turret_rig/yaw_convention.py, which derives this from the
// gun geometry and the compiled model rather than from prose.
static bool TurretBarrelPose( CTurret *t, Vector &hingeOut, Vector &dirOut )
{
    ICollideable *col = TurretCollideable( t );
    if (!col) return false;

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
    hingeOut = Vector( px, py, pz );

    // The barrel direction, local then rotated to world. See
    // InsTurretBarrelDirLocal for the convention and why it is shared.
    float lx, ly, lz;
    InsTurretBarrelDirLocal( t->m_curYaw, t->m_curPitch, lx, ly, lz );

    float dx, dy, dz;
    InsTurretVectorRotate( lx, ly, lz, vm, dx, dy, dz );
    dirOut = Vector( dx, dy, dz );
    return true;
}

static Vector TurretMuzzlePosition( CTurret *t )
{
    Vector hinge, dir;
    if (!TurretBarrelPose( t, hinge, dir )) return Vector( 0, 0, 0 );
    return hinge + dir * INS_TURRET_MUZZLE_FORWARD;
}

// TurretMuzzleAngles() lived here and fed MuzzleFlash's angles argument. It
// was removed with that call: the replacement (Sparks) takes a direction
// vector, which Fire() already has in world space from the target vector, so
// rebuilding the local (pitch, yaw) into a QAngle would only reintroduce the
// frame conversion this code has already been bitten by twice. The angles are
// still recoverable from m_curPitch/m_curYaw if a future effect needs them.

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
    float  bestDist = 1.0e12f;      // SCORE: dist^2, with the 0.64 stickiness
    float  bestRange = 0.0f;        // the winner's true distance, for the log
    float  bestBearing = 0.0f;      // the winner's WORLD bearing, for the log
    float  bestWantYaw = 0.0f;      // the LOCAL yaw the gun needs, for the log
    int    best = 0;
    const float spotRange = insbot_vis_range.GetFloat();

    // THE SEARCH IS OMNIDIRECTIONAL. The turret's own `turret_acquire_fov`
    // defaults to 360, so by default there is no directional gate at all and
    // every valid, visible, in-range enemy is a candidate no matter where the
    // gun happens to be pointing.
    //
    // WHY THE CONE WAS REMOVED RATHER THAN WIDENED: the old code borrowed the
    // bots' `insbot_fov` (120 degrees) and measured it from `m_curYaw`, the
    // gun's live bearing. On a 20 deg/s idle sweep that made detection a
    // function of how long ago the barrel had passed the target - up to 18
    // seconds for an enemy directly behind the truck, and the turret could be
    // shot to pieces from behind while its gun was still coming round. A
    // mounted ring gun has no eye and no neck; "what is behind me" is a real
    // question for it and the answer is always "visible if the hull is not in
    // the way", which is what the LOS trace below already decides.
    //
    // The setting is kept (rather than the test being deleted outright) so a
    // directional turret is still reachable with `insbot set turret_acquire_fov
    // 120` and no rebuild.
    //
    // m_curYaw is a LOCAL angle, so it is converted back to a world bearing
    // here using the inverse of Aim()'s mapping. This conversion is retained
    // even though the default cone is a full circle: with a narrowed cone the
    // comparison must still be against the gun's world bearing, not the hull's,
    // or the cone would rotate with the truck instead of with the gun
    // (design section 6.3, mirroring bot_perceive.cpp:588-591).
    float hullYaw = 0.0f;
    {
        ICollideable *col = TurretCollideable( this );
        if (col) hullYaw = col->GetCollisionAngles().y;
    }
    float yawRad = (m_curYaw + hullYaw + 90.0f) * (3.14159265f / 180.0f);
    Vector fwd( cosf( yawRad ), sinf( yawRad ), 0 );
    float cosFov = InsTurretConeCos(
        TurretValue( this, "turret_acquire_fov",
                     insbot_turret_acquire_fov.GetFloat() ) );

    for (int i = 1; i <= gpGlobals->maxClients; i++)
    {
        if (!TurretIsValidEnemy( this, i )) continue;
        edict_t *e = engine->PEntityOfEntIndex( i );
        IPlayerInfo *pi = playerinfomanager->GetPlayerInfo( e );
        Vector target = pi->GetAbsOrigin();

        Vector d = target - from;
        float dist = d.Length();
        if (dist > spotRange) continue;                 // spot range 2500

        // Directional gate. Skipped entirely for the target we already hold, so
        // a target that drifts to the cone edge is not dropped mid-track; and
        // short-circuited at cosFov < 0, which is what `turret_acquire_fov 360`
        // resolves to and what makes the default search omnidirectional.
        if (i != m_target && cosFov >= 0.0f)
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
        bestRange = dist;
        bestBearing = InsTurretNormalizeAngle(
            atan2f( d.y, d.x ) * (180.0f / 3.14159265f ) );
        bestWantYaw = InsTurretNormalizeAngle(
            bestBearing - hullYaw - 90.0f );
    }

    if (best)
    {
        // Bounded acquisition diagnostic. This is the evidence a live run needs
        // for "it is aware of all directions": an ACQUIRE line whose `want`
        // (the LOCAL yaw the gun must reach) is far from the gun's current
        // `yaw` proves the directional gate did not apply.
        //
        // `yaw` and `want` are both LOCAL and are the pair to compare. `bearing`
        // is the target's WORLD bearing, printed for orientation only - it is
        // offset from the local pair by hullYaw + 90, so it must not be
        // compared against `yaw` directly.
        if (m_acquiresLogged < 6)
        {
            m_acquiresLogged++;
            Msg( "[insbots] turret %d ACQUIRE %d target=%d dist=%.0f yaw=%.1f "
                 "want=%.1f bearing=%.1f\n",
                 m_entIndex, m_acquiresLogged, best, bestRange,
                 m_curYaw, bestWantYaw, bestBearing );
        }
        m_target = best;
        m_targetLastSeen = now;
        edict_t *e = engine->PEntityOfEntIndex( best );
        Vector pts[3];
        if (e && !e->IsFree() && TurretTargetPoints( e, pts ) > 1)
            m_targetLastPos = pts[1];
        m_state = TURRET_ENGAGING;
        // The aim offset is NOT rolled here. Acquire() is throttled to 10 Hz and
        // returns early while a target is held, so a roll at this site is either
        // frozen for a whole engagement or re-rolled at 10 Hz during an LOS
        // flicker. TurretRefreshAimError() owns the roll and runs on the bots'
        // 0.4-0.9 s cadence (bot_think.cpp:861).
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

    float newYaw   = InsTurretNormPose( m_curYaw,   kYawStart,   kYawEnd );
    float newPitch = InsTurretNormPose( m_curPitch, kPitchStart, kPitchEnd );

    // Stationary gun: nothing changed, so nothing has to be sent. This is what
    // keeps the forced full-entity update below off a parked, idle turret.
    if (pose[m_poseYaw] == newYaw && pose[m_posePitch] == newPitch)
        return;

    pose[m_poseYaw]   = newYaw;
    pose[m_posePitch] = newPitch;

    // THE TWO WRITES ABOVE ARE RAW POINTER WRITES.
    //
    // They therefore bypass CNetworkArray::Set(), which is the ONLY thing that
    // calls NetworkStateChanged() for these elements (networkvar.h:726-741).
    // With no dirty mark the new values reach a client only when something ELSE
    // dirties the vehicle - and that is precisely the situation while the truck
    // is being driven: the engine writes its own steer / speedo / wheel pose
    // parameters every tick and the moving origin keeps the edict dirty, so the
    // turret's values ride along in those updates.
    //
    // The moment the driver dies the truck comes to rest, the engine's own pose
    // writes stop changing, nothing dirties the edict, and these values are
    // never sent again. The visible result is a gun that tracks while the Humvee
    // is driven and then freezes in place once the driver dies - while the
    // server carries on acquiring, aiming and firing, because none of that
    // reads the driver (verified live: a turret on a driverless Humvee keeps
    // scanning and landing damage).
    //
    // FL_FULL_EDICT_CHANGED is REQUIRED, not optional. The offset-tracking
    // overload StateChanged(unsigned short) needs engine-private symbols that
    // are not linkable from a standalone VSP, so the full-edict flag is the
    // only public way to say "re-read this entity's properties" - see
    // bot_body.cpp:96-99 and ins_utilities' door smoothing, which use this same
    // pair for the same reason.
    e->m_fStateFlags |= FL_EDICT_CHANGED | FL_FULL_EDICT_CHANGED;
}
