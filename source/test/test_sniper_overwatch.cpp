// ============================================================================
// test_sniper_overwatch.cpp - native tests for the engine-free sniper policy.
//
// The seam is header-only and takes an injectable probe interface, so this
// binary links against the CRT alone: no Source engine, no nav mesh, no
// engine trace. It exercises the rules that decide where a Sniper perch may
// exist, when a sniper has arrived, and how a perch is abandoned.
// ============================================================================
#include <stdio.h>
#include <math.h>

#include "../src/sniper_overwatch.h"

static int g_pass = 0;
static int g_fail = 0;

static void Check(bool ok, const char *what)
{
    if (ok) g_pass++;
    else { g_fail++; printf("  [FAIL] %s\n", what); }
}

static bool Near(float a, float b, float eps = 0.001f)
{
    return fabsf(a - b) <= eps;
}

// A scripted probe: each trace is matched against a small table of blockers.
// This keeps the tests independent of the production trace implementation.
struct TestProbe : public sniper_ow::ISniperProbe
{
    struct Segment
    {
        float x1, y1, z1, x2, y2, z2;
        bool blocked;
    };
    Segment segs[8];
    int count;

    TestProbe() : count(0) {}

    void Add(float x1, float y1, float z1, float x2, float y2, float z2, bool blocked)
    {
        if (count >= 8) return;
        Segment &s = segs[count++];
        s.x1 = x1; s.y1 = y1; s.z1 = z1;
        s.x2 = x2; s.y2 = y2; s.z2 = z2;
        s.blocked = blocked;
    }

    virtual bool HullClear(float, float, float, float) { return true; }

    virtual bool WorldLOS(float x1, float y1, float z1, float x2, float y2, float z2)
    {
        for (int i = 0; i < count; i++)
        {
            const Segment &s = segs[i];
            if (Near(x1, s.x1) && Near(y1, s.y1) && Near(z1, s.z1)
                && Near(x2, s.x2) && Near(y2, s.y2) && Near(z2, s.z2))
                return !s.blocked;
        }
        return true;
    }
};

static void Test_RangeScore()
{
    Check(sniper_ow::RangeScore(499.0f) < 0.0f, "range below 500 is rejected");
    Check(Near(sniper_ow::RangeScore(500.0f), 0.0f), "range at 500 starts at zero");
    Check(Near(sniper_ow::RangeScore(1200.0f), 1.0f), "range at 1200 reaches one");
    Check(Near(sniper_ow::RangeScore(2200.0f), 1.0f), "range at 2200 remains one");
    Check(Near(sniper_ow::RangeScore(3300.0f), 0.0f), "range at 3300 returns zero");
    Check(sniper_ow::RangeScore(3301.0f) < 0.0f, "range above 3300 is rejected");
}

static void Test_Frontline()
{
    // Objective at origin, enemy home on +X. Friendly home is behind -X.
    sniper_ow::FrontlineRule attack = sniper_ow::EvaluateFrontline(
        251.0f, 0.0f, 0.0f, 0.0f, 4000.0f, 0.0f, false);
    Check(!attack.allowed, "attacking perch 251u past the point is rejected");

    sniper_ow::FrontlineRule flank = sniper_ow::EvaluateFrontline(
        0.0f, 1400.0f, 0.0f, 0.0f, 4000.0f, 0.0f, false);
    Check(flank.allowed, "90-degree flank is allowed");

    sniper_ow::FrontlineRule friendly = sniper_ow::EvaluateFrontline(
        -600.0f, 0.0f, 0.0f, 0.0f, 4000.0f, 0.0f, false);
    Check(friendly.allowed && friendly.bonus > 0.0f,
          "friendly hemisphere receives a positive bonus");

    sniper_ow::FrontlineRule defend = sniper_ow::EvaluateFrontline(
        901.0f, 0.0f, 0.0f, 0.0f, 4000.0f, 0.0f, true);
    Check(!defend.allowed, "friendly objective rejects beyond the forward cap");
}

static void Test_HomeReadiness()
{
    Check(!sniper_ow::CacheReady(true, false, 5.0f, 10.0f),
          "cache waits for both teams before the fallback");
    Check(sniper_ow::CacheReady(true, true, 5.0f, 10.0f),
          "cache is ready when both teams have Home");
    Check(sniper_ow::CacheReady(true, false, 10.0f, 10.0f),
          "cache falls back after the timeout");
    Check(sniper_ow::HomeDrifted(0.0f, 0.0f, 501.0f, 0.0f),
          "Home drift over 500u invalidates");
    Check(!sniper_ow::HomeDrifted(0.0f, 0.0f, 499.0f, 0.0f),
          "Home drift under 500u stays valid");
}

static void Test_ArrivalAndTimers()
{
    Check(sniper_ow::AtPerch(10.0f, 10.0f, 40.0f, 0.0f, 0.0f, 0.0f),
          "24u XY and 48u Z is an arrival");
    Check(!sniper_ow::AtPerch(0.0f, 0.0f, 49.0f, 0.0f, 0.0f, 0.0f),
          "wrong floor is not an arrival");
    Check(!sniper_ow::AtPerch(25.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f),
          "25u XY is not an aperture arrival");
    Check(Near(sniper_ow::TransitDeadline(0.0f, 7000.0f), 80.0f),
          "long transit deadline scales with path cost");
    Check(Near(sniper_ow::HoldDeadline(100.0f, 45.0f, 75.0f, 0.5f), 160.0f),
          "hold deadline uses the supplied roll");
}

static void Test_CompromisedPerch()
{
    Check(sniper_ow::PerchCompromised(399.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 46.0f),
          "compromised perch excludes inside 400u while active");
    Check(!sniper_ow::PerchCompromised(401.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 46.0f),
          "compromised perch does not exclude outside 400u");
    Check(!sniper_ow::PerchCompromised(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 46.0f, 46.0f),
          "compromised perch exclusion expires");
}

static void Test_ArcAndCover()
{
    float yaws[2] = { 355.0f, 5.0f };
    sniper_ow::Arc arc = sniper_ow::ArcFromYaws(yaws, 2);
    Check(Near(arc.centerDeg, 0.0f) || Near(arc.centerDeg, 360.0f),
          "wrapped arc centres on zero");
    Check(Near(arc.halfSpanDeg, 8.0f), "arc pads to the minimum eight degrees");

    float wide[2] = { 0.0f, 180.0f };
    arc = sniper_ow::ArcFromYaws(wide, 2);
    Check(Near(arc.halfSpanDeg, 35.0f), "arc caps at thirty-five degrees");

    bool blocked[8] = { true, false, false, false, true, false, false, false };
    float score = sniper_ow::CoverScore(blocked, 8, 0.0f, false);
    Check(score > 0.0f, "forward cover scores positively");
    bool forwardOnly[8] = { true, false, false, false, false, false, false, false };
    bool rearOnly[8] = { false, false, false, false, true, false, false, false };
    Check(sniper_ow::CoverScore(forwardOnly, 8, 0.0f, false)
              > sniper_ow::CoverScore(rearOnly, 8, 0.0f, false),
          "forward cover outranks rear back-wall cover");
    bool none[8] = { false, false, false, false, false, false, false, false };
    Check(Near(sniper_ow::CoverScore(none, 8, 0.0f, false), -12.0f),
          "completely exposed perch is penalized");
    Check(sniper_ow::CoverScore(none, 8, 0.0f, true) > 0.0f,
          "Sill cover rescues an otherwise exposed perch");

    TestProbe probe;
    probe.Add(0.0f, 0.0f, 30.0f, 96.0f, 0.0f, 30.0f, true);
    probe.Add(0.0f, 0.0f, 58.0f, 96.0f, 0.0f, 58.0f, false);
    Check(sniper_ow::HasSillCover(probe, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f),
          "low blocked / high clear is Sill cover");

    TestProbe jamb;
    float jambYaw = 35.0f * 3.14159265f / 180.0f;
    jamb.Add(0.0f, 0.0f, 58.0f,
             cosf(jambYaw) * 96.0f, sinf(jambYaw) * 96.0f, 58.0f, true);
    float clamped = sniper_ow::ClampArcToJambs(jamb, 0.0f, 0.0f, 0.0f,
                                               58.0f, 0.0f, 35.0f);
    Check(clamped < 35.0f, "jamb blocks the arc edge and clamps it inward");
}

static void Test_RouteOverlap()
{
    Check(sniper_ow::AabbOverlaps(0.0f, 0.0f, 100.0f, 100.0f,
                                  -50.0f, -50.0f, 50.0f, 50.0f),
          "overlapping objective box is detected");
    Check(!sniper_ow::AabbOverlaps(100.0f, 100.0f, 200.0f, 200.0f,
                                   -50.0f, -50.0f, 50.0f, 50.0f),
          "disjoint objective box is not detected");
}

static void Test_AssignmentScore()
{
    Check(Near(sniper_ow::AssignmentScore(100.0f, 2.0f, 1000.0f), 100.5f),
          "assignment score subtracts path cost");
}

int main()
{
    Test_RangeScore();
    Test_Frontline();
    Test_HomeReadiness();
    Test_ArrivalAndTimers();
    Test_CompromisedPerch();
    Test_ArcAndCover();
    Test_RouteOverlap();
    Test_AssignmentScore();

    printf("sniper overwatch unit tests\n");
    printf("passed: %d  failed: %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
