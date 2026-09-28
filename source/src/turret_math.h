// ============================================================================
// turret_math.h - pure aim/geometry math for the Humvee auto-turret.
//
// Deliberately free of engine types so it can be compiled and tested on the
// host in milliseconds (temp/turret_offline/run_tests.ps1). turret.cpp is the
// only consumer. Rules implemented here are from design spec section 6.2.
// ============================================================================
#pragma once

#include <math.h>

// Wrap into (-180, 180]. NaN maps to 0 so a bad engine angle cannot poison the
// aim state - the same guard bot_think.cpp uses for its wrap.
inline float InsTurretNormalizeAngle( float a )
{
    if (a != a) return 0.0f;                       // NaN
    float r = fmodf( a, 360.0f );
    if (r > 180.0f)  r -= 360.0f;
    if (r <= -180.0f) r += 360.0f;
    return r;
}

// Shortest signed rotation from `from` to `to`.
inline float InsTurretAngleDelta( float from, float to )
{
    return InsTurretNormalizeAngle( to - from );
}

// Move `current` toward `want` by at most `maxStep`, taking the short way.
//
// The result is re-wrapped into (-180, 180]. Without that the scan sweep, which
// adds to the bearing every frame, grows without bound: measured live, the idle
// scan walked 179 -> 453 -> 854 degrees, and the written pose control clamped at
// 1.0 long before the turret had come round. Keeping the angle canonical also
// keeps the 0..1 pose normalisation meaningful.
inline float InsTurretStepAngle( float current, float want, float maxStep )
{
    if (maxStep < 0.0f) maxStep = -maxStep;
    float d = InsTurretAngleDelta( current, want );
    if (d >  maxStep) d =  maxStep;
    if (d < -maxStep) d = -maxStep;
    return InsTurretNormalizeAngle( current + d );
}

// Hard hull limits: past +47 deg the muzzle enters the hull roof, and the
// measured bone range is about -55 .. +20 (design section 6.2).
#define INS_TURRET_PITCH_MIN  (-55.0f)
#define INS_TURRET_PITCH_MAX  ( 20.0f)

inline float InsTurretClampPitch( float pitch )
{
    if (pitch != pitch) return 0.0f;               // NaN
    if (pitch < INS_TURRET_PITCH_MIN) return INS_TURRET_PITCH_MIN;
    if (pitch > INS_TURRET_PITCH_MAX) return INS_TURRET_PITCH_MAX;
    return pitch;
}

// The fire gate. The inherited bot gate is +/-18 deg, which is far too loose
// for a mounted gun: at 1500 units it is ~400 units of lateral slop. The
// turret uses +/-5 yaw and +/-4 pitch (design section 6.2).
inline bool InsTurretAimed( float dYaw, float dPitch,
                            float yawTol, float pitchTol )
{
    if (dYaw   >  yawTol   || dYaw   < -yawTol)   return false;
    if (dPitch >  pitchTol || dPitch < -pitchTol) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Aim error. `turret_aim_error` is the half-width of a uniform roll applied to
// the desired bearing; it is NOT an error measurement (see turret.h's note on
// why it must stay separate from m_residual*).
//
// PITCH IS HALF OF YAW. The bots roll `m_aimErrX = frand(-err, err) * 0.5f` for
// pitch and full width for yaw (bot_think.cpp:863-864). A vertical miss is more
// visible than a horizontal one at the same angle, so the bots damp it; the
// turret mirrors the rule so the two aim models stay comparable.
// ---------------------------------------------------------------------------
inline float InsTurretAimOffsetDeg( float errorDeg, bool pitch )
{
    if (errorDeg != errorDeg) return 0.0f;         // NaN
    if (errorDeg < 0.0f) errorDeg = -errorDeg;     // a negative setting is a width
    return pitch ? errorDeg * 0.5f : errorDeg;
}

// unit in [0,1] -> [-half, +half]. Out-of-range units are clamped rather than
// wrapped: a wrapped roll would put a garbage unit back inside the cone.
inline float InsTurretAimErrorRoll( float halfWidthDeg, float unit )
{
    if (halfWidthDeg != halfWidthDeg) return 0.0f; // NaN
    if (unit != unit) return 0.0f;                 // NaN
    if (unit < 0.0f) unit = 0.0f;
    if (unit > 1.0f) unit = 1.0f;
    return (unit * 2.0f - 1.0f) * halfWidthDeg;
}

// The acquisition cone's cosine, for the `dot / len < cosFov` rejection in the
// turret's own search loop.
//
// THE TURRET DOES NOT USE THE BOTS' `fov`. `insbot_fov` defaults to 120 degrees
// and is the bots' shared setting (insbots.cpp:136); a turret inheriting it
// could only see +/-60 degrees from wherever the gun happened to be pointing,
// so on a 20 deg/s idle sweep an enemy standing behind the truck went unnoticed
// until the barrel physically swept past them - about 18 seconds on average,
// and 36 in the worst case. The turret has its own `turret_acquire_fov`,
// defaulting to 360.
//
// cos(180) is -1, and a normalised dot product can never be less than -1, so
// returning -1 for a full circle makes every candidate pass the test. That is
// the whole mechanism behind "aware of all directions": the loop, its ordering
// and its LOS gate are untouched, only this one predicate stops rejecting.
inline float InsTurretConeCos( float fovDegrees )
{
    if (fovDegrees != fovDegrees) return -1.0f;      // NaN -> omnidirectional
    if (fovDegrees >= 360.0f)     return -1.0f;      // full circle
    if (fovDegrees < 0.0f)        fovDegrees = 0.0f;
    return cosf( fovDegrees * 0.5f * (3.14159265358979323846f / 180.0f) );
}

// value -> 0..1 control, matching Studio_SetPoseParameter's own normalisation
// (public/bone_setup.cpp:4186-4189) so a value written here is what the bone
// solver will read back.
inline float InsTurretNormPose( float value, float start, float end )
{
    float span = end - start;
    if (span == 0.0f || span != span) return 0.0f;
    float t = (value - start) / span;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

// ===========================================================================
// Local replacements for the mathlib helpers the muzzle geometry needs.
//
// WHY THESE EXIST: AngleVectors / AngleMatrix / VectorTransform / VectorRotate
// live in mathlib. On Linux the engine's own tier0_i486.so exports them and the
// plugin resolves them at load; on Windows they are NOT in tier0.lib or
// vstdlib.lib, so the link fails with LNK2019 unless mathlib.lib is pulled in -
// a new build dependency for four small functions.
//
// These are transcribed from mathlib/mathlib_base.cpp (the same code the engine
// runs on Linux) so both platforms compute the muzzle identically, and they are
// plain float math so the host-side test harness can check them.
//
// CONVENTION, taken from mathlib: the matrix is m[row][col] where COLUMN c is
// basis axis c, so column 0 is forward, 1 is left/right and 2 is up.
//   VectorTransform: out[r] = in.x*m[r][0] + in.y*m[r][1] + in.z*m[r][2] + m[r][3]
//   VectorRotate:    out[r] = in.x*m[r][0] + in.y*m[r][1] + in.z*m[r][2]
// ===========================================================================

#define INS_TURRET_DEG2RAD (3.14159265358979323846f / 180.0f)
#define INS_TURRET_RAD2DEG (180.0f / 3.14159265358979323846f)

// ---------------------------------------------------------------------------
// Pitch demand and reachability.
//
// THE DEMAND IS THE UNCLAMPED ANGLE. This distinction is the whole point of
// these two helpers. Aim() used to compute the demand, clamp it, and keep only
// the clamped value - so a target 15 degrees outside the arc produced a
// residual of 0 and the fire gate reported a perfect aim. The demand must
// survive separately from the clamped pose target.
//
// Three call sites must agree on what the demand is, and they are the reason
// this lives here rather than inline in turret.cpp:
//   1. Aim()                 - drives the pose (clamped)
//   2. the acquisition search - rejects candidates the arc cannot cover
//   3. the hold logic        - releases a held target that became unreachable
//
// Positive is DOWN, matching InsTurretBarrelDirLocal's `dz = -sinf(pitch)`.
// ---------------------------------------------------------------------------
inline float InsTurretDemandPitch( float dz, float horiz )
{
    float p = -atan2f( dz, horiz ) * INS_TURRET_RAD2DEG;
    if (p != p) return 0.0f;                 // NaN cannot poison the aim state
    return p;
}

// Can the elevation arc cover this demand?
//
// The epsilon is deliberate and is NOT slack in the arc: at a demand exactly
// on the limit, float rounding in the atan2/scale above can land a hair
// outside, and rejecting that target would drop a legitimately reachable
// enemy. Half a degree of slack is far below the fire gate's own +/-4 degree
// tolerance, so a target admitted by it is still gated by the residual test.
//
// NaN is unreachable: a bad demand must fail closed rather than lock the gun.
#define INS_TURRET_PITCH_REACH_EPS (0.5f)

inline bool InsTurretPitchReachable( float pitchDeg )
{
    if (pitchDeg != pitchDeg) return false;                  // NaN
    return (pitchDeg >= INS_TURRET_PITCH_MIN - INS_TURRET_PITCH_REACH_EPS) &&
           (pitchDeg <= INS_TURRET_PITCH_MAX + INS_TURRET_PITCH_REACH_EPS);
}

// ---------------------------------------------------------------------------
// The barrel's direction in the VEHICLE'S LOCAL frame, from the turret's own
// local yaw/pitch. This is the single source of truth for "which way the gun
// points": TurretMuzzlePosition() offsets the hinge along it, and Fire() traces
// the round along it. Because both take the same vector, the round can never
// leave the barrel at an angle the pose does not show - and the aim error, which
// lives in m_curYaw/m_curPitch by the time this is called, becomes a real miss.
//
// CONVENTION (derived from the gun geometry and the compiled model, not from
// prose - see temp/humvee_turret_rig/yaw_convention.py, quoted in
// TurretBarrelPose's own comment in turret.cpp):
//   yaw 0    -> ( 0, +1, 0)  local +Y, out over the bonnet
//   yaw +90  -> (-1,  0, 0)  local -X
//   yaw -90  -> (+1,  0, 0)  local +X
//   pitch    -> positive is DOWN: +20 depresses, -55 elevates
//
// `Aim()` produces a matching local yaw as `worldYaw - hullYaw - 90`, so this
// vector's world bearing is `localYaw + hullYaw + 90` once rotated by the
// vehicle matrix - the same inverse mapping the acquisition FOV test uses.
// ---------------------------------------------------------------------------
inline void InsTurretBarrelDirLocal( float yawDeg, float pitchDeg,
                                     float &dx, float &dy, float &dz )
{
    float y  = yawDeg   * INS_TURRET_DEG2RAD;
    float p  = pitchDeg * INS_TURRET_DEG2RAD;
    float cp = cosf( p );
    dx = -sinf( y ) * cp;
    dy =  cosf( y ) * cp;
    dz = -sinf( p );
}

// QAngle is (pitch, yaw, roll); the caller passes them in that order.
inline void InsTurretAngleMatrix( float pitch, float yaw, float roll,
                                  float ox, float oy, float oz,
                                  float m[3][4] )
{
    float sp = sinf( pitch * INS_TURRET_DEG2RAD ), cp = cosf( pitch * INS_TURRET_DEG2RAD );
    float sy = sinf( yaw   * INS_TURRET_DEG2RAD ), cy = cosf( yaw   * INS_TURRET_DEG2RAD );
    float sr = sinf( roll  * INS_TURRET_DEG2RAD ), cr = cosf( roll  * INS_TURRET_DEG2RAD );

    // matrix = (YAW * PITCH) * ROLL
    m[0][0] = cp*cy;
    m[1][0] = cp*sy;
    m[2][0] = -sp;

    m[0][1] = sr*sp*cy + cr*-sy;
    m[1][1] = sr*sp*sy + cr*cy;
    m[2][1] = sr*cp;

    m[0][2] = cr*sp*cy + -sr*-sy;
    m[1][2] = cr*sp*sy + -sr*cy;
    m[2][2] = cr*cp;

    m[0][3] = ox;
    m[1][3] = oy;
    m[2][3] = oz;
}

inline void InsTurretVectorTransform( float ix, float iy, float iz,
                                      const float m[3][4],
                                      float &ox, float &oy, float &oz )
{
    ox = ix*m[0][0] + iy*m[0][1] + iz*m[0][2] + m[0][3];
    oy = ix*m[1][0] + iy*m[1][1] + iz*m[1][2] + m[1][3];
    oz = ix*m[2][0] + iy*m[2][1] + iz*m[2][2] + m[2][3];
}

inline void InsTurretVectorRotate( float ix, float iy, float iz,
                                   const float m[3][4],
                                   float &ox, float &oy, float &oz )
{
    ox = ix*m[0][0] + iy*m[0][1] + iz*m[0][2];
    oy = ix*m[1][0] + iy*m[1][1] + iz*m[1][2];
    oz = ix*m[2][0] + iy*m[2][1] + iz*m[2][2];
}

// The `up` output of AngleVectors (mathlib_base.cpp:2712).
inline void InsTurretAngleUp( float pitch, float yaw, float roll,
                              float &ux, float &uy, float &uz )
{
    float sp = sinf( pitch * INS_TURRET_DEG2RAD ), cp = cosf( pitch * INS_TURRET_DEG2RAD );
    float sr = sinf( roll  * INS_TURRET_DEG2RAD ), cr = cosf( roll  * INS_TURRET_DEG2RAD );
    ux = -sp;
    uy = sr*cp;
    uz = cr*cp;
}

// Vector::NormalizeInPlace goes through the pfVectorNormalize function pointer,
// which mathlib initialises - another symbol a VSP cannot rely on linking.
// Returns the original length so a caller can detect a degenerate vector.
inline float InsTurretNormalize( float &x, float &y, float &z )
{
    float len = sqrtf( x*x + y*y + z*z );
    if (len > 0.0f) { x /= len; y /= len; z /= len; }
    return len;
}
