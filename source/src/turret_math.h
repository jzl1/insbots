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
