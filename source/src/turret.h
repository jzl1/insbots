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
    Vector m_targetLastPos;
    float  m_curYaw;
    float  m_curPitch;
    float  m_desiredYaw;
    float  m_desiredPitch;
    float  m_nextAcquire;
    float  m_burstUntil;
    float  m_pauseUntil;
    float  m_scanBearing;
    // The random aim offset, re-rolled in Acquire and added to the desired
    // bearing in Aim. This is a TUNING value, not an error measurement.
    float  m_aimErrorYaw;
    float  m_aimErrorPitch;
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
