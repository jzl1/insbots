// ============================================================================
// test_cover_math.cpp - unit tests for the shared cover sector/tier helpers.
//
// These are the pure functions every cover consumer shares. They need no
// engine, no nav mesh and no cover database: bot.h's inline helpers are the
// whole surface under test, so this links against the CRT alone.
//
// Build (repository root, x86 MSVC environment - see the plan's Task 1 Step 2).
//
// The test must not reference any engine symbol. If the link fails on tier0 or
// vstdlib, the test is accidentally calling engine code - fix the test, do not
// add those libraries.
// ============================================================================
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "bot.h"

static int g_pass = 0;
static int g_fail = 0;

static void Check(bool ok, const char *what)
{
    if (ok) g_pass++;
    else { g_fail++; printf("  [FAIL] %s\n", what); }
}

// ---------------------------------------------------------------------------
// The contract of CoverSectorFor, brute-forced over every whole degree:
//
//   1. the chosen sector's centre is NEVER more than half a sector width from
//      the bearing;
//   2. when one centre is strictly nearer, that is the one chosen;
//   3. on an exact tie between two adjacent centres, the sector toward
//      INCREASING bearing wins - which is what the previous inline
//      `(yaw + half) / width` arithmetic did, and what the helper documents.
//
// All three are stated geometrically, so none of them re-derives the formula
// under test. The tie rule matters: at 12 sectors the halfway bearings are
// 15, 45, 75, ... 345, and a test assuming "lower index wins" would disagree
// with the implementation on 11 of them.
// ---------------------------------------------------------------------------
static void Test_NearestCentre()
{
    Vector from(0.0f, 0.0f, 0.0f);
    int offByTooMuch = 0, nonTieWrong = 0, tieWrong = 0, tiesSeen = 0;
    const float half = COVER_SECTOR_DEG * 0.5f;
    const float eps = 1e-3f;

    for (int deg = 0; deg < 360; deg++)
    {
        float rad = (float)deg * (3.14159265358979f / 180.0f);
        Vector to(cosf(rad) * 100.0f, sinf(rad) * 100.0f, 0.0f);
        int got = CoverSectorFor(from, to);
        if (got < 0 || got >= COVER_SECTORS) { offByTooMuch++; continue; }

        // (1) never off by more than half a sector
        float centre = (float)got * COVER_SECTOR_DEG;
        float diff = fabsf((float)deg - centre);
        if (diff > 180.0f) diff = 360.0f - diff;
        if (diff > half + eps) offByTooMuch++;

        // the strictly nearest centre, by brute force
        int want = 0;
        float bestDiff = 1e9f;
        for (int d = 0; d < COVER_SECTORS; d++)
        {
            float c = (float)d * COVER_SECTOR_DEG;
            float dd = fabsf((float)deg - c);
            if (dd > 180.0f) dd = 360.0f - dd;
            if (dd < bestDiff) { bestDiff = dd; want = d; }
        }

        if (bestDiff < half - eps)
        {
            // (2) unambiguous nearest wins
            if (got != want) nonTieWrong++;
        }
        else
        {
            // (3) exact tie between sector d and d+1; increasing bearing wins.
            //     Derive d from the bearing itself: a tie sits at d*w + w/2.
            tiesSeen++;
            int d = (int)(((float)deg / COVER_SECTOR_DEG) - 0.5f);
            int expect = (d + 1) % COVER_SECTORS;
            if (got != expect) tieWrong++;
        }
    }

    printf("  bearings more than half a sector off-centre: %d\n", offByTooMuch);
    printf("  unambiguous bearings binned wrongly        : %d\n", nonTieWrong);
    printf("  exact ties seen / binned wrongly           : %d / %d\n", tiesSeen, tieWrong);
    Check(offByTooMuch == 0, "no bearing bins further than half a sector from its centre");
    Check(nonTieWrong == 0, "every unambiguous bearing bins to its strictly nearest ray");
    Check(tieWrong == 0, "exact ties round toward increasing bearing");
}

// Each sector's own ray bearing must bin back to that sector. This is what
// keeps the builder and the scorers in agreement.
static void Test_RayBearingsSelfBin()
{
    Vector from(0.0f, 0.0f, 0.0f);
    int bad = 0;
    for (int d = 0; d < COVER_SECTORS; d++)
    {
        float rad = (float)d * COVER_SECTOR_DEG * (3.14159265358979f / 180.0f);
        Vector to(cosf(rad) * 250.0f, sinf(rad) * 250.0f, 0.0f);
        if (CoverSectorFor(from, to) != d) bad++;
    }
    printf("  ray bearings that did not self-bin: %d\n", bad);
    Check(bad == 0, "each sector's own ray bearing bins to that sector");
}

// NOTE: Task 1 carried a Test_LegacyEquivalence here that proved the shared
// helper reproduced the old 8-sector inline formula exactly. It was deleted
// when the sector count rose to 12, because the binning intentionally changed
// and keeping it would have asserted the superseded behaviour forever.

static void Test_Popcount()
{
    unsigned char m[COVER_SECTORS];
    for (int i = 0; i < COVER_SECTORS; i++) m[i] = 0;
    Check(CoverMaskPopcount(m) == 0, "an all-zero mask counts 0");

    for (int i = 0; i < COVER_SECTORS; i++) m[i] = 1;
    Check(CoverMaskPopcount(m) == COVER_SECTORS, "an all-one mask counts every sector");

    for (int i = 0; i < COVER_SECTORS; i++) m[i] = 0;
    m[0] = 1;
    m[COVER_SECTORS - 1] = 1;
    Check(CoverMaskPopcount(m) == 2, "popcount is position-independent");

    for (int i = 0; i < COVER_SECTORS; i++) m[i] = (i & 1) ? 1 : 0;
    Check(CoverMaskPopcount(m) == COVER_SECTORS / 2, "an alternating mask counts half");
}

static void Test_TierThresholds()
{
    Check(CoverTierForPopcount(0) == COVER_NONE, "0 hits is not cover");
    Check(CoverTierForPopcount(COVER_TIER_SPOT_MIN - 1) == COVER_NONE,
          "one hit below the spot threshold is rejected");
    Check(CoverTierForPopcount(COVER_TIER_SPOT_MIN) == COVER_SPOT,
          "the spot threshold yields COVER_SPOT");
    Check(CoverTierForPopcount(COVER_TIER_PARTIAL_MIN) == COVER_PARTIAL,
          "the partial threshold yields COVER_PARTIAL");
    Check(CoverTierForPopcount(COVER_TIER_FULL_MIN) == COVER_FULL,
          "the full threshold yields COVER_FULL");
    Check(CoverTierForPopcount(COVER_SECTORS) == COVER_FULL,
          "a fully blocked mask is COVER_FULL");
    Check(COVER_TIER_SPOT_MIN < COVER_TIER_PARTIAL_MIN, "thresholds are ordered");
    Check(COVER_TIER_PARTIAL_MIN < COVER_TIER_FULL_MIN, "thresholds are ordered");
    Check(COVER_TIER_FULL_MIN <= COVER_SECTORS, "the full threshold is reachable");
}

static void Test_MaskGeometry()
{
    CoverPoint cp;
    Check(sizeof(cp.blockMask) == COVER_SECTORS, "blockMask has one byte per sector");
    Check(sizeof(cp.crouchMask) == COVER_SECTORS, "crouchMask has one byte per sector");
    Check(sizeof(cp.blockMask) == sizeof(cp.crouchMask), "both masks are the same size");
}

static void Test_BlocksPredicate()
{
    CoverPoint cp;
    memset(&cp, 0, sizeof(cp));
    cp.pos = Vector(0.0f, 0.0f, 0.0f);
    Vector threat(300.0f, 0.0f, 0.0f);                 // due +X
    Check(CoverSectorFor(cp.pos, threat) == 0, "a due-east threat bins to sector 0");

    Check(!CoverPointBlocks(cp, threat, false), "an empty mask blocks nothing while standing");
    Check(!CoverPointBlocks(cp, threat, true),  "an empty mask blocks nothing while ducked");

    cp.blockMask[0] = 1;
    Check(CoverPointBlocks(cp, threat, false), "a standing block stops a standing bot");
    Check(CoverPointBlocks(cp, threat, true),  "a standing block also stops a ducked bot");

    cp.blockMask[0] = 0;
    cp.crouchMask[0] = 1;
    Check(!CoverPointBlocks(cp, threat, false),
          "crouch-only cover does NOT stop a standing bot");
    Check(CoverPointBlocks(cp, threat, true),
          "crouch-only cover DOES stop a ducked bot");

    cp.crouchMask[0] = 0;
    cp.blockMask[(COVER_SECTORS / 2) % COVER_SECTORS] = 1;
    Check(!CoverPointBlocks(cp, threat, false),
          "a block on the opposite bearing protects nothing");
}

int main()
{
    printf("cover math unit tests (COVER_SECTORS=%d)\n", COVER_SECTORS);
    Test_NearestCentre();
    Test_RayBearingsSelfBin();
    Test_Popcount();
    Test_TierThresholds();
    Test_MaskGeometry();
    Test_BlocksPredicate();
    printf("\npassed: %d  failed: %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
