// ============================================================================
// vehicle_steer_math.h - pure rules for the bots' vehicle-avoidance steering.
//
// Deliberately free of engine types so it compiles and runs on the host in
// milliseconds (temp/vehicle_steer/run.ps1). bot_steer.cpp is the only
// consumer. Keeping the decision arithmetic here means a live-test failure is
// never caused by the arithmetic - only by the trace plumbing around it.
// ============================================================================
#pragma once

#include <math.h>

// How much of the forward probe a shoulder must clear before it is usable.
// 0.60 of a 200u probe is 120u, which is what the bot travels during one
// 0.45 s hold at 0.65 x 400 u/s move speed. A shoulder that clears less than
// that cannot be walked and would just grind, re-creating the spin class of
// bug this work exists to remove.
#define INS_VEHICLE_MIN_SHOULDER_FRAC 0.60f

// One deflection hold, in seconds. Matches the proven SteerToPoint hold.
#define INS_VEHICLE_HOLD_SECONDS 0.45f

// Throttle multiplier while diverting. A 60-degree deflection is a deliberate
// manoeuvre, not the 22-degree crowd nudge that justifies 0.90; 0.65 matches
// the width-aware pass in bot_steer.cpp.
#define INS_VEHICLE_SPEED_SCALE 0.65f

// Probe length for the forward and shoulder sweeps.
#define INS_VEHICLE_PROBE_DIST 200.0f

// Pick a shoulder heading, or refuse.
//
// baseYaw     heading the blocked episode began at; shoulders are +-60 from it
// leftFrac    clearance fraction (0..1) of the baseYaw + 60 sweep
// rightFrac   clearance fraction (0..1) of the baseYaw - 60 sweep
// minFrac     minimum clearance a shoulder needs to be usable
// hitLateral  signed lateral offset of the blocking hit from the waypoint
//             line; positive means the hit is to the LEFT of the line
// outYaw      chosen heading when the function returns true
//
// Returns false when neither shoulder clears minFrac, in which case the
// caller must NOT deflect: the existing stuck recovery is the correct
// fallback, and a least-bad deflection would just grind.
//
// Tie-break: when both shoulders are usable and equally clear, steer AWAY
// from the hit. A wide vehicle usually leaves both sides open, so the plain
// "first one wins" rule would always pick left.
inline bool InsSteerPickShoulder(float baseYaw, float leftFrac, float rightFrac,
                                 float minFrac, float hitLateral,
                                 float &outYaw)
{
    bool leftOk  = leftFrac  >= minFrac;
    bool rightOk = rightFrac >= minFrac;

    if (!leftOk && !rightOk) return false;

    // Away from the hit: hit on the left -> prefer the right shoulder.
    bool preferRight = hitLateral > 0.0f;
    bool preferLeft  = hitLateral < 0.0f;

    float chosen;
    if (leftOk && rightOk)
    {
        // A clearly better shoulder wins outright so the bot does not hug the
        // truck; only an effective tie lets the hit side decide.
        const float epsilon = 0.05f;
        if (leftFrac > rightFrac + epsilon)      chosen = baseYaw + 60.0f;
        else if (rightFrac > leftFrac + epsilon) chosen = baseYaw - 60.0f;
        else if (preferRight)                    chosen = baseYaw - 60.0f;
        else if (preferLeft)                     chosen = baseYaw + 60.0f;
        else                                     chosen = baseYaw + 60.0f;
    }
    else if (leftOk)  chosen = baseYaw + 60.0f;
    else              chosen = baseYaw - 60.0f;

    while (chosen > 180.0f)  chosen -= 360.0f;
    while (chosen < -180.0f) chosen += 360.0f;
    outYaw = chosen;
    return true;
}

// Signed lateral offset of a point from a line through `origin` along
// `yawDeg`. Positive means the point is to the LEFT of the line.
// Used for the tie-break input above.
inline float InsSteerLateralOffset(float originX, float originY, float yawDeg,
                                   float pointX, float pointY)
{
    const float kDegToRad = 3.14159265f / 180.0f;
    float yr = yawDeg * kDegToRad;
    float sideX = -sinf(yr);
    float sideY =  cosf(yr);
    return (pointX - originX) * sideX + (pointY - originY) * sideY;
}

// True when a bot should consider the vehicle controller at all this frame.
// A stationary bot has no movement to adjust, and a map with no driveables
// must cost nothing.
inline bool InsSteerShouldRunVehicleCheck(int vehicleCount, bool isMoving,
                                          bool avoidEnabled)
{
    if (!avoidEnabled) return false;
    if (vehicleCount <= 0) return false;
    return isMoving;
}
