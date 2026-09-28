// ============================================================================
// test_turret_aim_frame.cpp - unit tests for the turret's aim FRAME math.
//
// The pose parameter the turret writes is a rotation RELATIVE TO THE HULL: the
// engine composes q_track * q_rest (bone_setup.cpp:889). A demand therefore has
// to be expressed in the hull's own frame. Solving it in world space and
// subtracting only the hull's yaw leaves the hull's pitch and roll in the
// answer, which is the defect these tests pin.
//
// The live fixture below is the exact geometry measured on the Linux test VM
// (see F:\claude\temp\turret_aim_error_result.md): hull pitch -1.55, yaw 0.29,
// roll 0.60, muzzle (-6988.86, 1219.62, 161.06), aim point (-6400, 1200, 127.6).
// With the old world-space formula the barrel landed 1.52 deg high and 40 of 40
// rounds missed; with the hull-local formula the error collapses to zero.
//
// These are pure functions: no engine, no SDK, no srcds. They link against the
// CRT alone. If the link fails on tier0 or vstdlib, the test is accidentally
// calling engine code - fix the test, do not add those libraries.
//
// Build (x86 MSVC environment, from any directory):
//   cl.exe /nologo /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS
//          /I <repo>\source\src
//          <repo>\source\test\test_turret_aim_frame.cpp
//          /Fe:test_turret_aim_frame.exe
// The only include is "turret_math.h" from source/src; this file needs no
// engine, no SDK and no srcds, and links against the CRT alone.
// ============================================================================
#include "turret_math.h"
#include <stdio.h>
#include <math.h>

static int g_pass = 0;
static int g_fail = 0;

static void Check(const char *what, bool ok, float got, float want)
{
    if (ok) { g_pass++; printf("  PASS  %s\n", what); }
    else    { g_fail++; printf("  FAIL  %s (got %.4f, want %.4f)\n", what, got, want); }
}

static bool Near(float a, float b, float eps = 0.001f) { return fabsf(a - b) < eps; }

// The world elevation of a direction, in degrees. This is the quantity the live
// probe measures, so the tests assert against it directly.
static float ElevationDeg(float x, float y, float z)
{
    return atan2f(z, sqrtf(x * x + y * y)) * INS_TURRET_RAD2DEG;
}

// Rotate a local direction to world, exactly as TurretBarrelPose does.
static void LocalToWorld(float hullPitch, float hullYaw, float hullRoll,
                         float yawDeg, float pitchDeg,
                         float &wx, float &wy, float &wz)
{
    float lx, ly, lz;
    InsTurretBarrelDirLocal(yawDeg, pitchDeg, lx, ly, lz);
    float m[3][4];
    InsTurretAngleMatrix(hullPitch, hullYaw, hullRoll, 0.0f, 0.0f, 0.0f, m);
    InsTurretVectorRotate(lx, ly, lz, m, wx, wy, wz);
}

int main()
{
    // ---- 1. The inverse round-trips the forward form exactly ----------------
    // InsTurretBarrelAnglesForLocalDir must recover the yaw/pitch that
    // InsTurretBarrelDirLocal produced. Any sign slip here moves the gun.
    {
        float worstYaw = 0.0f, worstPitch = 0.0f;
        const float yaws[]    = { 0.0f, 90.0f, -90.0f, 180.0f, -180.0f, 37.5f, -142.25f };
        const float pitches[] = { -55.0f, -17.5f, 0.0f, 10.0f, 20.0f };
        for (int i = 0; i < 7; i++)
        {
            for (int j = 0; j < 5; j++)
            {
                float dx, dy, dz, y, p;
                InsTurretBarrelDirLocal(yaws[i], pitches[j], dx, dy, dz);
                InsTurretBarrelAnglesForLocalDir(dx, dy, dz, y, p);
                float dyaw   = fabsf(InsTurretAngleDelta(y, yaws[i]));
                float dpitch = fabsf(InsTurretAngleDelta(p, pitches[j]));
                if (dyaw   > worstYaw)   worstYaw   = dyaw;
                if (dpitch > worstPitch) worstPitch = dpitch;
            }
        }
        Check("inverse round-trips yaw (35 combinations)",   worstYaw   < 0.01f, worstYaw,   0.0f);
        Check("inverse round-trips pitch (35 combinations)", worstPitch < 0.01f, worstPitch, 0.0f);
    }

    // ---- 2. On a LEVEL (yaw-only) hull the new solve matches the old one ----
    // This is the no-regression guarantee. The old formula was
    //   wantYaw   = worldYaw - hullYaw - 90
    //   wantPitch = InsTurretDemandPitch( dz, horiz )
    // and on a hull with zero pitch and roll it is algebraically identical to
    // the transpose. If this test ever fails, the fix changed level-hull
    // behaviour and must not ship.
    {
        float worstYaw = 0.0f, worstPitch = 0.0f;
        const float hullYaws[] = { 0.0f, 0.29f, 45.0f, -117.0f, 179.0f };
        const float dirs[][3]  = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
                                   { 1.0f, 1.0f, -0.5f }, { -3.0f, 2.0f, 1.0f },
                                   { 0.0f, -1.0f, 0.0f }, { 5.0f, -7.0f, -2.0f } };
        for (int i = 0; i < 5; i++)
        {
            for (int j = 0; j < 6; j++)
            {
                float wx = dirs[j][0], wy = dirs[j][1], wz = dirs[j][2];

                float oldYaw = InsTurretNormalizeAngle(
                    atan2f(wy, wx) * INS_TURRET_RAD2DEG - hullYaws[i] - 90.0f);
                float oldPitch = InsTurretDemandPitch(wz, sqrtf(wx * wx + wy * wy));

                float lx, ly, lz, newYaw, newPitch;
                InsTurretWorldDirToHullLocal(wx, wy, wz, 0.0f, hullYaws[i], 0.0f,
                                             lx, ly, lz);
                InsTurretBarrelAnglesForLocalDir(lx, ly, lz, newYaw, newPitch);

                float dy = fabsf(InsTurretAngleDelta(newYaw, oldYaw));
                float dp = fabsf(InsTurretAngleDelta(newPitch, oldPitch));
                if (dy > worstYaw)   worstYaw   = dy;
                if (dp > worstPitch) worstPitch = dp;
            }
        }
        Check("level hull: new yaw matches the old formula",   worstYaw   < 0.001f, worstYaw,   0.0f);
        Check("level hull: new pitch matches the old formula", worstPitch < 0.001f, worstPitch, 0.0f);
    }

    // ---- 3. REGRESSION: the exact live defect ------------------------------
    // Measured on the Linux VM: hull pitch -1.55, yaw 0.29, roll 0.60, muzzle
    // (-6988.86, 1219.62, 161.06), aim point (-6400, 1200, 127.6). The barrel
    // landed 1.52 deg high and 40 of 40 rounds missed.
    {
        const float hullPitch = -1.55f, hullYaw = 0.29f, hullRoll = 0.60f;
        const float muz[3] = { -6988.86f, 1219.62f, 161.06f };
        const float aim[3] = { -6400.0f,  1200.0f,  127.6f };

        float wx = aim[0] - muz[0];
        float wy = aim[1] - muz[1];
        float wz = aim[2] - muz[2];
        float wantElev = ElevationDeg(wx, wy, wz);

        // (a) the OLD formula: world-space demand, hull yaw subtracted by hand.
        float oldPitch = InsTurretDemandPitch(wz, sqrtf(wx * wx + wy * wy));
        float bx, by, bz;
        LocalToWorld(hullPitch, hullYaw, hullRoll, -92.20f, oldPitch, bx, by, bz);
        float oldErr = ElevationDeg(bx, by, bz) - wantElev;

        // (b) the NEW formula: rotate into the hull frame first.
        float lx, ly, lz, newYaw, newPitch;
        InsTurretWorldDirToHullLocal(wx, wy, wz, hullPitch, hullYaw, hullRoll,
                                     lx, ly, lz);
        InsTurretBarrelAnglesForLocalDir(lx, ly, lz, newYaw, newPitch);
        LocalToWorld(hullPitch, hullYaw, hullRoll, newYaw, newPitch, bx, by, bz);
        float newErr = ElevationDeg(bx, by, bz) - wantElev;

        Check("live fixture: old formula is ~1.52 deg off",
              fabsf(oldErr) > 1.4f && fabsf(oldErr) < 1.7f, oldErr, -1.52f);
        Check("live fixture: new formula lands on the target",
              fabsf(newErr) < 0.01f, newErr, 0.0f);
        Check("live fixture: new local pitch is 4.78",
              Near(newPitch, 4.78f, 0.05f), newPitch, 4.78f);
        Check("live fixture: new local yaw is -92.25",
              Near(newYaw, -92.25f, 0.05f), newYaw, -92.25f);
    }

    // ---- 4. Degenerate input must not produce NaN --------------------------
    // A NaN pose angle would propagate into the network array and freeze the
    // gun; every helper in turret_math.h fails closed, and these two must too.
    {
        float y = 1.0f, p = 1.0f;
        InsTurretBarrelAnglesForLocalDir(NAN, 0.0f, 0.0f, y, p);
        Check("inverse: NaN input -> 0/0", Near(y, 0.0f) && Near(p, 0.0f), y + p, 0.0f);

        InsTurretBarrelAnglesForLocalDir(0.0f, 0.0f, 0.0f, y, p);
        Check("inverse: zero vector -> 0/0", Near(y, 0.0f) && Near(p, 0.0f), y + p, 0.0f);

        float lx, ly, lz, ny, np;
        InsTurretWorldDirToHullLocal(NAN, NAN, NAN, NAN, NAN, NAN, lx, ly, lz);
        InsTurretBarrelAnglesForLocalDir(lx, ly, lz, ny, np);
        Check("frame convert: all-NaN -> finite 0/0",
              ny == ny && np == np && Near(ny, 0.0f) && Near(np, 0.0f), ny + np, 0.0f);
    }

    // ---- 5. A rolled hull must not move the barrel off a level target ------
    // Same target, four different hull attitudes. The new solve must land the
    // barrel on the demand in every one.
    {
        const float muz[3] = { -6988.86f, 1219.62f, 161.06f };
        const float aim[3] = { -6400.0f,  1200.0f,  127.6f };
        float wx = aim[0] - muz[0], wy = aim[1] - muz[1], wz = aim[2] - muz[2];
        float wantElev = ElevationDeg(wx, wy, wz);

        const float attitudes[][3] = { { 0.0f,   0.0f,   0.0f },
                                       { -1.55f, 0.29f,  0.60f },
                                       { 6.0f,   90.0f, -4.0f },
                                       { -12.0f, -35.0f, 9.0f } };
        float worst = 0.0f;
        for (int i = 0; i < 4; i++)
        {
            float lx, ly, lz, ny, np, bx, by, bz;
            InsTurretWorldDirToHullLocal(wx, wy, wz,
                                         attitudes[i][0], attitudes[i][1], attitudes[i][2],
                                         lx, ly, lz);
            InsTurretBarrelAnglesForLocalDir(lx, ly, lz, ny, np);
            LocalToWorld(attitudes[i][0], attitudes[i][1], attitudes[i][2],
                         ny, np, bx, by, bz);
            float err = fabsf(ElevationDeg(bx, by, bz) - wantElev);
            if (err > worst) worst = err;
        }
        Check("four hull attitudes all land on the demand", worst < 0.05f, worst, 0.0f);
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
