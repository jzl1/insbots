// ============================================================================
// vehicle_target_math.h - pure rules for the bots' anti-vehicle threat channel.
//
// Deliberately free of engine types so it compiles and runs on the host in
// milliseconds (temp/vehicle_target/run.ps1). bot_perceive.cpp is the only
// consumer. The constants are MEASURED, not chosen:
//
//   * INS_VEHICLE_AIM_HEIGHT 55.0f - origin + (0, 0, 55) hit 200/200 stationary
//     traces and 145/145 moving traces up to 624.4 u/s with zero physics-bone
//     lag, all returning hitbox == 0.
//   * WorldSpaceCenter() resolves to origin + (-0.50, -0.75, +98.01) for
//     humvee_vehicle.mdl, which is open air above the cabin roof, and missed
//     200/200 stationary traces at 950u. NEVER aim there.
//
// See docs/humvee-bot-targeting-open-questions-verification.md section 2.
// ============================================================================
#pragma once

#include <math.h>

// Measured hull aim height above the vehicle's collision origin.
#define INS_VEHICLE_AIM_HEIGHT 55.0f

// How many candidate aim points a vehicle offers: hull, hood, bed. Measured
// trace budget: at most 2 Humvees per map, so <= 6 sight rays per scan.
#define INS_VEHICLE_TARGET_POINTS 3

// Play teams are 1 and 2 only; everything else (spectator, unassigned,
// unowned) is inert. Mirrors the t > 0 && t < 3 rule in ValidEnemy.
//
// The team passed in is the truck's LATCHED OWNER from the turret registry,
// not the live driver's team. See InsVehicleEngageable below for why.
inline bool InsVehicleTeamIsHostile(int ownerTeam, int myTeam)
{
    if (ownerTeam <= 0 || ownerTeam >= 3) return false;
    if (myTeam <= 0 || myTeam >= 3) return false;
    return ownerTeam != myTeam;
}

// Squared-distance gate. A non-positive maxRange must mean "engage nothing",
// never "engage everything": a misconfigured 0 must fail closed.
inline bool InsVehicleInRange(float distSq, float maxRange)
{
    if (maxRange <= 0.0f) return false;
    return distSq <= maxRange * maxRange;
}

// The single engageability rule.
//
// OWNERSHIP IS LATCHED, and that is the model. A Humvee stays on the team of
// the first player to board it, for the rest of its life, INCLUDING after that
// driver dies (CTurret::Arm, turret.cpp:504-536). So:
//
//   * driver dies          -> still hostile, bots keep engaging. Correct.
//   * enemy hijacks it     -> ownership does NOT move, so it stays friendly to
//                             its original team. "Whoever gets in first gets the
//                             turret on their team" is the agreed rule.
//   * wrecked              -> humvee_health.sp:567 sends `insbot turret remove`,
//                             which drops the registry entry, so the caller
//                             never reaches this predicate with a wreck.
//   * never boarded        -> no owner, not in the registry, never engaged.
//
// `ownerTeam` therefore comes from the TURRET REGISTRY, not from the live
// driver. Do not "fix" this to read the driver: that would make an enemy
// hijacker flip a friendly truck to hostile, which is the behavior this rule
// exists to prevent.
//
// `friendlyDriverAboard` is the one live-driver fact that still matters. A
// teammate sitting in an owned-but-hostile-to-them truck must not be shot at:
// the hull is the target, but a friendly body inside it makes the whole
// engagement unsafe. Pass true when the seated driver is on myTeam.
inline bool InsVehicleEngageable(int ownerTeam, int myTeam,
                                 bool friendlyDriverAboard)
{
    if (friendlyDriverAboard) return false;
    if (ownerTeam <= 0 || ownerTeam >= 3) return false;
    if (myTeam <= 0 || myTeam >= 3) return false;
    return ownerTeam != myTeam;
}

// Candidate aim points, in priority order. Point 0 is the measured hull aim
// point and is ALWAYS produced; points 1 and 2 are the hood and the rear bed.
//
// `forward` is the vehicle's yaw direction, already flattened to XY by the
// caller. A degenerate forward vector is tolerated: points 1 and 2 then
// collapse onto the hull point rather than producing NaN coordinates.
//
// Returns the number of points written (1..3).
inline int InsVehicleBuildTargetPoints(float ox, float oy, float oz,
                                       float fx, float fy,
                                       float points[3][3])
{
    int count = 0;

    // Point 0: the MEASURED aim point. Never substitute WorldSpaceCenter.
    points[count][0] = ox;
    points[count][1] = oy;
    points[count][2] = oz + INS_VEHICLE_AIM_HEIGHT;
    count++;

    float len = sqrtf(fx * fx + fy * fy);
    if (len > 0.001f)
    {
        float nx = fx / len;
        float ny = fy / len;

        // Point 1: engine block / hood, 40u forward and 35u up from the origin.
        points[count][0] = ox + nx * 40.0f;
        points[count][1] = oy + ny * 40.0f;
        points[count][2] = oz + 35.0f;
        count++;

        // Point 2: rear bed, 40u behind the origin and 45u up.
        points[count][0] = ox - nx * 40.0f;
        points[count][1] = oy - ny * 40.0f;
        points[count][2] = oz + 45.0f;
        count++;
    }

    return count;
}
