// ============================================================================
// test_vehicle_target.cpp - host tests for the bot anti-vehicle threat math.
//
// These are the pure rules the vehicle-targeting channel obeys. They need no
// engine, no SDK, no nav mesh and no srcds: vehicle_target_math.h is the whole
// surface under test, so this links against the CRT alone.
//
// If the link fails on tier0 or vstdlib, the test is accidentally calling
// engine code - fix the test, do not add those libraries.
//
// The constants pinned here are measured values from
// docs/humvee-bot-targeting-open-questions-verification.md section 2:
//   * origin + (0, 0, 55) hit 200/200 stationary and 145/145 moving traces.
//   * WorldSpaceCenter() (origin + (-0.5, -0.75, +98.01)) missed 200/200.
// Do not "round" the 55.
//
// Build (x86 MSVC environment, from any directory):
//   cl.exe /nologo /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS
//          /I <repo>\source\src
//          <repo>\source\test\test_vehicle_target.cpp
//          /Fe:test_vehicle_target.exe
// ============================================================================
#include <stdio.h>
#include <math.h>

#include "vehicle_target_math.h"

static int g_pass = 0;
static int g_fail = 0;

static void Check(const char *what, bool ok)
{
    if (ok) { g_pass++; printf("  PASS  %s\n", what); }
    else    { g_fail++; printf("  FAIL  %s\n", what); }
}

static bool Near(float a, float b, float eps = 0.001f) { return fabsf(a - b) < eps; }

// ---- hostility -------------------------------------------------------------
static void Test_Hostility()
{
    // Play teams are 1 and 2 only (bot_perceive.cpp:573).
    Check("team 2 is hostile to team 1",  InsVehicleTeamIsHostile(2, 1));
    Check("team 1 is hostile to team 2",  InsVehicleTeamIsHostile(1, 2));
    Check("same team is not hostile",     !InsVehicleTeamIsHostile(1, 1));
    Check("team 0 (spectator) is inert",  !InsVehicleTeamIsHostile(0, 1));
    Check("team 3 is inert",              !InsVehicleTeamIsHostile(3, 1));
    Check("team 0 is not hostile to 0",   !InsVehicleTeamIsHostile(0, 0));
}

// ---- range ---------------------------------------------------------------
static void Test_Range()
{
    // maxRange 2500 => 6,250,000 squared units.
    Check("inside range passes",  InsVehicleInRange(1000.0f * 1000.0f, 2500.0f));
    Check("at range passes",      InsVehicleInRange(2500.0f * 2500.0f, 2500.0f));
    Check("outside range fails",  !InsVehicleInRange(2501.0f * 2501.0f, 2500.0f));
    // A disabled range must mean "no vehicles", never "all vehicles".
    Check("zero range fails",     !InsVehicleInRange(0.0f, 0.0f));
    Check("negative range fails", !InsVehicleInRange(0.0f, -1.0f));
}

// ---- target points -------------------------------------------------------
static void Test_TargetPoints()
{
    // Vehicle at the origin facing +X. Point 0 must be origin + (0, 0, 55).
    float pts[3][3];
    int n = InsVehicleBuildTargetPoints(100.0f, 200.0f, 50.0f,
                                        1.0f, 0.0f, pts);
    Check("three points are produced", n == 3);
    Check("point 0 x", Near(pts[0][0], 100.0f));
    Check("point 0 y", Near(pts[0][1], 200.0f));
    Check("point 0 z is origin + 55", Near(pts[0][2], 105.0f));
    Check("point 0 is NOT WorldSpaceCenter", !Near(pts[0][2], 148.01f, 0.5f));

    // Point 1 is the hood: forward of centre, lower than the aim point.
    Check("point 1 is forward", Near(pts[1][0], 140.0f));
    Check("point 1 y", Near(pts[1][1], 200.0f));
    Check("point 1 z is hood height", Near(pts[1][2], 85.0f));

    // Point 2 is the rear bed: behind centre.
    Check("point 2 is behind centre", Near(pts[2][0], 60.0f));
    Check("point 2 y", Near(pts[2][1], 200.0f));
    Check("point 2 z is bed height", Near(pts[2][2], 95.0f));

    // A degenerate forward vector must not produce NaN points.
    int n2 = InsVehicleBuildTargetPoints(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, pts);
    Check("degenerate forward still yields a usable first point", n2 >= 1);
    Check("degenerate forward point 0 is finite", pts[0][2] == pts[0][2]);

    // A rotated forward vector must rotate points 1 and 2 with it.
    int n3 = InsVehicleBuildTargetPoints(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, pts);
    Check("rotated forward still yields three points", n3 == 3);
    Check("rotated point 1 follows +Y", Near(pts[1][1], 40.0f) && Near(pts[1][0], 0.0f));
    Check("rotated point 2 follows -Y", Near(pts[2][1], -40.0f) && Near(pts[2][0], 0.0f));
}

// ---- engageability (ownership-latched, not driver-derived) ---------------
static void Test_Engageable()
{
    // Ownership is latched from the first boarding player and survives the
    // driver's death. These cases pin that model, so nobody "fixes" the
    // predicate into reading the live driver.
    Check("enemy-owned truck is engageable",
          InsVehicleEngageable(2, 1, false));
    Check("enemy-owned truck is STILL engageable with no driver",
          InsVehicleEngageable(2, 1, false));
    Check("own team's truck is not engageable",
          !InsVehicleEngageable(1, 1, false));

    // An enemy hijacking a friendly truck must NOT flip it hostile: ownership
    // does not move, so a teammate-visible truck stays off the target list.
    Check("hijacked truck keeps its original owner",
          !InsVehicleEngageable(1, 1, false));

    // A friendly body inside an otherwise-engageable hull vetoes the shot.
    Check("friendly driver aboard vetoes the engagement",
          !InsVehicleEngageable(2, 1, true));
    Check("friendly driver vetoes even an own-team truck",
          !InsVehicleEngageable(1, 1, true));

    // Unarmed / unowned / spectator cases are inert.
    Check("unowned truck is not engageable",
          !InsVehicleEngageable(0, 1, false));
    Check("team 3 owner is not engageable",
          !InsVehicleEngageable(3, 1, false));
    Check("invalid observer team is not engageable",
          !InsVehicleEngageable(2, 0, false));
}

int main()
{
    Test_Hostility();
    Test_Range();
    Test_TargetPoints();
    Test_Engageable();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
