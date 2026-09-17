// Host unit test for the engine-free Push-order helpers.
// Build: F:\claude\temp\build_test_objective_order.bat
#include <stdio.h>
#include <string.h>

#include "objective_order.h"

static int g_fail = 0;
static int g_pass = 0;

static void Check(bool cond, const char *msg)
{
    if (cond) { g_pass++; printf("ok   %s\n", msg); }
    else      { g_fail++; printf("FAIL %s\n", msg); }
}

int main()
{
    // ---- InsOrderIdValid: both sentinel spellings are invalid ----
    Check(InsOrderIdValid(0),            "order 0 is valid");
    Check(InsOrderIdValid(3),            "order 3 is valid");
    Check(!InsOrderIdValid(-1),          "order -1 (int sentinel) is invalid");
    Check(!InsOrderIdValid(255),         "order 255 (byte sentinel) is invalid");
    Check(!InsOrderIdValid(-999),        "order -999 is invalid");
    Check(InsOrderIdValid(254),          "order 254 is valid");

    // ---- InsParseGametype ----
    int ordered = -1;
    const char *push = "ProfileData\n{\n\t\"Push\"\n\t{\n\t\t\"gametype\" \t\"GAMETYPE_PUSH\"\n\t}\n}\n";
    Check(InsParseGametype(push, &ordered) == 1, "parses GAMETYPE_PUSH");
    Check(ordered == 1,                          "PUSH sets ordered=1");

    ordered = -1;
    const char *ff = "ProfileData\n{\n\t\"Firefight - Iraq\"\n\t{\n\t\t\"gametype\"\t\t\"GAMETYPE_FIREFIGHT\"\n\t}\n}\n";
    Check(InsParseGametype(ff, &ordered) == 1,   "parses GAMETYPE_FIREFIGHT");
    Check(ordered == 0,                          "FIREFIGHT sets ordered=0");

    ordered = -1;
    const char *battle = "\"gametype\"\t\t\"GAMETYPE_BATTLE\"";
    Check(InsParseGametype(battle, &ordered) == 1, "parses GAMETYPE_BATTLE");
    Check(ordered == 0,                            "BATTLE sets ordered=0");

    ordered = -1;
    const char *strike = "\"gametype\" \t\"GAMETYPE_STRIKE\"";
    Check(InsParseGametype(strike, &ordered) == 1, "parses GAMETYPE_STRIKE");
    Check(ordered == 0,                            "STRIKE sets ordered=0 (no stock maps; PUSH-only by design)");

    ordered = -1;
    Check(InsParseGametype("no gametype here", &ordered) == 0, "missing gametype returns 0");
    Check(ordered == 0,                                        "missing gametype leaves ordered=0");

    ordered = -1;
    Check(InsParseGametype(0, &ordered) == 0, "null buffer returns 0");
    Check(ordered == 0,                       "null buffer leaves ordered=0");

    ordered = -1;
    Check(InsParseGametype("\"gametype\"   \"\"", &ordered) == 0, "empty gametype value returns 0");

    // ---- InsLowestOrderIndex ----
    {
        // karkar shape: sentinels interleaved, eligible subset non-contiguous
        const int order[8]     = { 2, 0, 255, 3, 255, 255, 255, 1 };
        const bool elig[8]     = { true, false, false, true, false, false, false, true };
        // eligible entries are idx 0 (order 2), idx 3 (order 3), idx 7 (order 1)
        Check(InsLowestOrderIndex(order, elig, 8) == 7, "karkar: lowest eligible order is idx 7 (order 1)");
    }
    {
        const int order[4] = { 0, 1, 2, 3 };
        const bool elig[4] = { false, true, true, true };
        Check(InsLowestOrderIndex(order, elig, 4) == 1, "contiguous: skips ineligible idx 0");
    }
    {
        const int order[3] = { 255, -1, 255 };
        const bool elig[3] = { true, true, true };
        Check(InsLowestOrderIndex(order, elig, 3) == -1, "all sentinels returns -1");
    }
    {
        const int order[2] = { 5, 1 };
        const bool elig[2] = { false, false };
        Check(InsLowestOrderIndex(order, elig, 2) == -1, "no eligible entries returns -1");
    }
    {
        const int order[3] = { 2, 2, 2 };
        const bool elig[3] = { false, true, true };
        Check(InsLowestOrderIndex(order, elig, 3) == 1, "ties resolve to the first eligible index");
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
