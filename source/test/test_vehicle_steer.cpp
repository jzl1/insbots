// ============================================================================
// test_vehicle_steer.cpp - host tests for the vehicle-avoidance pure rules.
//
// Needs no engine, no SDK, no nav mesh and no srcds: vehicle_steer_math.h is
// the whole surface under test, so this links against the CRT alone.
//
// If the link fails on tier0 or vstdlib, the test is accidentally calling
// engine code - fix the test, do not add those libraries.
//
// Build: see F:\claude\temp\vehicle_steer\run.ps1
// ============================================================================
#include <stdio.h>
#include <math.h>

#include "vehicle_steer_math.h"

static int g_pass = 0;
static int g_fail = 0;

static void Check(const char *what, bool ok)
{
    if (ok) { g_pass++; printf("  PASS  %s\n", what); }
    else    { g_fail++; printf("  FAIL  %s\n", what); }
}

static bool Near(float a, float b, float eps = 0.001f) { return fabsf(a - b) < eps; }

// ---- shoulder refusal ------------------------------------------------------
static void Test_Refusal()
{
    float yaw = 0.0f;
    // Neither shoulder clears the bar: refuse, do not pick the least bad.
    Check("both blocked -> refuse",
          !InsSteerPickShoulder(0.0f, 0.10f, 0.20f,
                                INS_VEHICLE_MIN_SHOULDER_FRAC, 0.0f, yaw));
    Check("exactly at the bar on one side -> use it",
          InsSteerPickShoulder(0.0f, 0.60f, 0.10f,
                               INS_VEHICLE_MIN_SHOULDER_FRAC, 0.0f, yaw));
    Check("just under the bar on both -> refuse",
          !InsSteerPickShoulder(0.0f, 0.599f, 0.599f,
                                INS_VEHICLE_MIN_SHOULDER_FRAC, 0.0f, yaw));
}

// ---- single usable shoulder ------------------------------------------------
static void Test_SingleShoulder()
{
    float yaw = 0.0f;
    Check("only left clears -> +60",
          InsSteerPickShoulder(0.0f, 0.90f, 0.10f,
                               INS_VEHICLE_MIN_SHOULDER_FRAC, 0.0f, yaw)
          && Near(yaw, 60.0f));
    Check("only right clears -> -60",
          InsSteerPickShoulder(0.0f, 0.10f, 0.90f,
                               INS_VEHICLE_MIN_SHOULDER_FRAC, 0.0f, yaw)
          && Near(yaw, -60.0f));
    Check("left is clearly better -> +60",
          InsSteerPickShoulder(0.0f, 0.95f, 0.70f,
                               INS_VEHICLE_MIN_SHOULDER_FRAC, 0.0f, yaw)
          && Near(yaw, 60.0f));
    Check("right is clearly better -> -60",
          InsSteerPickShoulder(0.0f, 0.70f, 0.95f,
                               INS_VEHICLE_MIN_SHOULDER_FRAC, 0.0f, yaw)
          && Near(yaw, -60.0f));
}

// ---- tie-break away from the hit -------------------------------------------
static void Test_TieBreak()
{
    float yaw = 0.0f;
    // Equal clearance, hit to the LEFT (+lateral) -> prefer the RIGHT shoulder.
    Check("equal, hit left -> -60",
          InsSteerPickShoulder(0.0f, 0.80f, 0.80f,
                               INS_VEHICLE_MIN_SHOULDER_FRAC, 5.0f, yaw)
          && Near(yaw, -60.0f));
    // Equal clearance, hit to the RIGHT (-lateral) -> prefer the LEFT shoulder.
    Check("equal, hit right -> +60",
          InsSteerPickShoulder(0.0f, 0.80f, 0.80f,
                               INS_VEHICLE_MIN_SHOULDER_FRAC, -5.0f, yaw)
          && Near(yaw, 60.0f));
    // Centred hit -> deterministic left.
    Check("equal, hit centred -> +60",
          InsSteerPickShoulder(0.0f, 0.80f, 0.80f,
                               INS_VEHICLE_MIN_SHOULDER_FRAC, 0.0f, yaw)
          && Near(yaw, 60.0f));
    // A clearly better shoulder must win over the tie-break.
    Check("better left beats hit-side preference",
          InsSteerPickShoulder(0.0f, 0.95f, 0.75f,
                               INS_VEHICLE_MIN_SHOULDER_FRAC, -5.0f, yaw)
          && Near(yaw, 60.0f));
}

// ---- base yaw and wrapping -------------------------------------------------
static void Test_BaseYaw()
{
    float yaw = 0.0f;
    Check("base 90, left only -> 150",
          InsSteerPickShoulder(90.0f, 0.90f, 0.10f,
                               INS_VEHICLE_MIN_SHOULDER_FRAC, 0.0f, yaw)
          && Near(yaw, 150.0f));
    Check("base 90, right only -> 30",
          InsSteerPickShoulder(90.0f, 0.10f, 0.90f,
                               INS_VEHICLE_MIN_SHOULDER_FRAC, 0.0f, yaw)
          && Near(yaw, 30.0f));
    Check("base 170, left only -> wraps to -130",
          InsSteerPickShoulder(170.0f, 0.90f, 0.10f,
                               INS_VEHICLE_MIN_SHOULDER_FRAC, 0.0f, yaw)
          && Near(yaw, -130.0f));
    Check("base -170, right only -> wraps to 130",
          InsSteerPickShoulder(-170.0f, 0.10f, 0.90f,
                               INS_VEHICLE_MIN_SHOULDER_FRAC, 0.0f, yaw)
          && Near(yaw, 130.0f));
}

// ---- lateral offset sign ---------------------------------------------------
static void Test_LateralOffset()
{
    // Facing +X (yaw 0). A point at +Y is to the LEFT.
    Check("point +Y is left of +X heading",
          InsSteerLateralOffset(0.0f, 0.0f, 0.0f, 0.0f, 10.0f) > 0.0f);
    Check("point -Y is right of +X heading",
          InsSteerLateralOffset(0.0f, 0.0f, 0.0f, 0.0f, -10.0f) < 0.0f);
    Check("point on the line has zero offset",
          Near(InsSteerLateralOffset(0.0f, 0.0f, 0.0f, 10.0f, 0.0f), 0.0f));
    // Facing +Y (yaw 90). A point at -X is to the LEFT.
    Check("point -X is left of +Y heading",
          InsSteerLateralOffset(0.0f, 0.0f, 90.0f, -10.0f, 0.0f) > 0.0f);
}

// ---- run gate --------------------------------------------------------------
static void Test_RunGate()
{
    Check("enabled, vehicles present, moving -> run",
          InsSteerShouldRunVehicleCheck(1, true, true));
    Check("disabled -> skip",
          !InsSteerShouldRunVehicleCheck(1, true, false));
    Check("no vehicles -> skip",
          !InsSteerShouldRunVehicleCheck(0, true, true));
    Check("stationary -> skip",
          !InsSteerShouldRunVehicleCheck(1, false, true));
    Check("negative count -> skip",
          !InsSteerShouldRunVehicleCheck(-1, true, true));
}

int main()
{
    printf("vehicle_steer_math tests\n");
    Test_Refusal();
    Test_SingleShoulder();
    Test_TieBreak();
    Test_BaseYaw();
    Test_LateralOffset();
    Test_RunGate();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
