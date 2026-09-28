// ============================================================================
// turret.h - public interface of the Humvee auto-turret subsystem.
//
// One turret per Humvee, identified by entindex. The owning team is latched
// from the first player to board (design spec section 5.1) and lives ONLY in
// this registry - nothing is written onto the vehicle entity.
// ============================================================================
#pragma once

#include "eiface.h"

// Hard cap. A map will never hold this many Humvees; the cap exists so a
// malformed entindex cannot index past the array.
#define INS_TURRET_MAX 64

enum TurretState
{
    TURRET_IDLE     = 0,   // scanning, no target
    TURRET_ENGAGING = 1    // has a target
};

class CTurret
{
public:
    CTurret();

    void Reset( int entindex, int team );

    int  EntIndex() const { return m_entIndex; }
    int  Team()     const { return m_team; }
    bool Armed()    const { return m_armed; }
    void SetTeam( int t ) { m_team = t; m_armed = (t > 0 && t < 3); }

    // Per-frame work. Defined in turret.cpp; split into stages so the
    // ordering constraint in the spec (traverse before fire) is visible.
    void FrameUpdate( bool simulating );

private:
    bool Validate();          // step 1 of the spec's per-frame tick
    void Arm();               // read the driver, latch the team, once
    void Acquire( float now );// throttled to ~10 Hz
    bool VerifyLOS();         // chest -> torso -> head
    void Aim( float now );    // desired yaw/pitch
    void Traverse( float dt );// rate-limited step
    void WritePose();         // m_flPoseParameter
    void Fire( float now );   // gate, then trace/damage/effects

public:
    int    m_entIndex;
    int    m_team;
    bool   m_armed;
    bool   m_validated;
    int    m_target;
    float  m_targetLastSeen;
    // When the held target first went outside the elevation arc, or 0 when it
    // is currently reachable. The hold logic releases the target once this is
    // older than `turret_unreachable_grace`, so a target that is only
    // momentarily unreachable (cresting a ridge, stepping down a slope) is
    // not dropped on the first frame.
    float  m_unreachableSince;
    Vector m_targetLastPos;
    float  m_curYaw;
    float  m_curPitch;
    float  m_desiredYaw;
    float  m_desiredPitch;
    // The UNCLAMPED elevation demand, kept separate from m_desiredPitch on
    // purpose. m_desiredPitch is what the pose is driven toward and is clamped
    // to the hull arc; this is what the target actually requires. The aim
    // residual is measured against THIS one, so a target outside the arc
    // produces a non-zero residual and the fire gate rejects it instead of
    // reporting a perfect aim. The demand is derived in the HULL'S OWN frame by
    // InsTurretWorldDirToHullLocal + InsTurretBarrelAnglesForLocalDir; see
    // Aim() in turret.cpp for why that frame is the only correct one.
    float  m_desiredPitchTrue;
    float  m_nextAcquire;
    // Per-shot rate limit (M249 ~800 RPM). This is the ONLY thing pacing the
    // turret's fire: there is no burst/pause cycle any more (see Fire()).
    float  m_nextShot;
    // Diagnostic counter: the first few shots per turret are logged so a live
    // run can distinguish "never fired" from "fired but nothing visible".
    int    m_shotsLogged;
    // Whether the last shot's trace reached the intended target. Diagnostic
    // only; read by the bounded SHOT log line in Fire().
    bool   m_shotHitTarget;
    // Every round fired, tracer or not. Drives the 1-in-3 tracer cadence that
    // matches the mod's own M249. Distinct from m_shotsLogged, which is a
    // bounded DIAGNOSTIC counter for the first few shots.
    int    m_shotsFired;
    // Diagnostic counter: the first few fire-gate rejections per turret, each
    // logged with its reason.
    int    m_rejectsLogged;
    // Diagnostic counter: the first few successful acquisitions per turret.
    // Without it a live run cannot tell "the turret never noticed the enemy"
    // from "it noticed, turned, and was blocked by the fire gate" - the two
    // look identical from the outside, which is the same gap m_shotsLogged and
    // m_rejectsLogged already close for the later stages.
    int    m_acquiresLogged;
    float  m_scanBearing;
    // The random aim offset, added to the desired bearing in Aim. This is a
    // TUNING value, not an error measurement.
    //
    // Re-rolled on the BOTS' cadence - frand(0.4, 0.9) seconds
    // (bot_think.cpp:861) - and NOT per acquisition. Rolling per acquisition has
    // two failure modes and the cadence has neither: during a sustained
    // engagement the offset would be frozen for the whole fight, and during an
    // LOS flicker (a target stepping behind a pole and back) it would be
    // re-rolled at Acquire()'s 10 Hz rate. That 10 Hz flicker re-roll is the
    // "unexplained jitter" recorded in
    // temp/humvee_turret_rig/RESULTS.md:486-492.
    float  m_aimErrorYaw;
    float  m_aimErrorPitch;
    // Deadline for the next re-roll, in gpGlobals->curtime seconds. Compared
    // against `now` in TurretRefreshAimError(), which is called every tick.
    float  m_nextAimErrTime;
    // The `turret_aim_error` SETTING (not the rolled offset) that the current
    // shot-diagnostic budget belongs to. When the setting changes, the budget is
    // reset so each value gets its own sample window - see TurretRefreshAimError.
    // -1.0f is a sentinel that no valid setting equals.
    float  m_lastAimErrSetting;
    // The live residual between where the gun points and where it wants to
    // point. Recomputed every frame in Traverse and consumed by the fire gate.
    //
    // These MUST be separate from m_aimError* above. The plan used one pair of
    // fields for both jobs, which made Aim() add the previous frame's residual
    // back into the desired bearing: the turret chased its own error, the
    // residual grew instead of converging, and the +/-5 deg fire gate was
    // almost never satisfied (measured live: err swinging to -171 deg and one
    // shot every 45 s).
    float  m_residualYaw;
    float  m_residualPitch;
    // Pose-parameter indices resolved from the MODEL by name, not hardcoded.
    // -1 means "not yet resolved", and WritePose() then does nothing rather
    // than writing to a slot that belongs to the engine's own vehicle
    // parameters.
    //
    // This must be a lookup rather than a constant. The shipped model declares
    // eleven ENGINE-driven parameters (four wheel heights, four wheel spins,
    // steer, action, gauge) at indices 0..10, and the turret's two are appended
    // at 11 and 12. Writing 0 and 1 - which an earlier version of this code
    // did - writes into `vehicle_wheel_fl_height` / `vehicle_wheel_fr_height`,
    // the two parameters CFourWheelVehiclePhysics::UpdateVehicle writes every
    // frame. That fights the engine for the suspension and is exactly the kind
    // of fault that shows up as glitching wheels rather than as a compile or
    // runtime error.
    int    m_poseYaw;
    int    m_posePitch;
    TurretState m_state;
};

class CTurretRegistry
{
public:
    CTurretRegistry();

    bool      Add( int entindex, int team );
    void      Remove( int entindex );
    void      RemoveAll();
    CTurret  *Find( int entindex );
    int       Count() const { return m_count; }
    CTurret  *At( int i ) { return &m_turrets[i]; }

    void FrameUpdate( bool simulating );
    void Discover( float now );   // find newly-boarded Humvees (design 5.1)
    // Advance m_flAnimTime on every driveable, turret or not. This is the
    // client-side interpolation clock the shipped client fails to refresh for
    // vehicles; see the definition in turret.cpp.
    void DriveAnimTime( bool simulating );
    void Status();

private:
    CTurret m_turrets[INS_TURRET_MAX];
    int     m_count;
    float   m_nextDiscover;
};

extern CTurretRegistry g_Turrets;

// Per-turret overrides are keyed by entindex, which is not stable across a
// level, so they must be dropped alongside the registry.
void TurretOverridesClear();

// Console surface: `insbot turret <add|remove|status|set> ...`
// Returns true when the subcommand was recognised and handled.
bool TurretHandleCommand( int argc );
