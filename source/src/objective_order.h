// ============================================================================
// objective_order.h - Push-order objective targeting helpers
//
// On Push maps the engine only accepts a capture on the next objective in the
// profile's sequence. m_iOrderID is the 0-based index of an objective inside
// the ACTIVE profile's ObjectiveData block; objectives the profile does not use
// carry a sentinel.
//
// These helpers are deliberately free of engine types (no CBaseEntity, no
// IVEngineServer) so they can be unit-tested on the host. The engine-facing
// glue lives in objective.cpp.
// ============================================================================
#pragma once

#include <string.h>

// The engine marks objectives outside the active profile with -1. Verified by
// live dump on ins_karkar (2026-09-17): 8 entities read 2, 0, -1, 3, -1, -1,
// -1, 1, matching the profile's 4 objectives. The m_i prefix is a 32-bit int,
// consistent with the existing Linux IsSpawnZone() test (orderId == -1).
//
// 255 is also rejected as a defensive measure: it is what a byte-wide read of
// a -1 sentinel would yield, so rejecting both keeps this correct if the field
// is ever read as a byte.
inline bool InsOrderIdValid(int v)
{
    return v >= 0 && v != 255;
}

// Scan an .imc2 buffer for the first "gametype" value.
// Returns 1 when a value was parsed, 0 otherwise (missing/empty/null input).
// *outOrdered is set to 1 only for sequential modes (GAMETYPE_PUSH).
//
// Every stock map has exactly one gametype, inside its ProfileData block, with
// a tab between key and value. When a custom map declares several profiles the
// first one is the map's default, which is what a stock rotation uses.
//
// GAMETYPE_STRIKE is sequential in the SDK template but ships no stock maps, so
// it is intentionally not enabled here - it cannot be live-verified.
inline int InsParseGametype(const char *buf, int *outOrdered)
{
    if (outOrdered) *outOrdered = 0;
    if (!buf) return 0;

    const char *p = strstr(buf, "\"gametype\"");
    if (!p) return 0;
    p += 10;                       // strlen("\"gametype\"")
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return 0;
    p++;
    const char *end = strchr(p, '"');
    if (!end || end == p) return 0;

    const size_t n = (size_t)(end - p);
    static const char kPush[] = "GAMETYPE_PUSH";
    if (n == sizeof(kPush) - 1)
    {
        // Manual ASCII compare: keeps this header free of <strings.h> /
        // strncasecmp portability questions between MSVC and glibc.
        bool same = true;
        for (size_t i = 0; i < n; i++)
        {
            char a = p[i], b = kPush[i];
            if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
            if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
            if (a != b) { same = false; break; }
        }
        if (same && outOrdered) *outOrdered = 1;
    }
    return 1;
}

// Return the index of the eligible entry with the lowest valid orderId, or -1
// when no eligible entry carries a valid orderId (the caller then falls back to
// its normal selection). Ties resolve to the lowest index, so the result is
// deterministic frame to frame.
inline int InsLowestOrderIndex(const int *orderId, const bool *eligible, int count)
{
    if (!orderId || !eligible || count <= 0) return -1;
    int best = -1;
    int bestOrder = 0;
    for (int i = 0; i < count; i++)
    {
        if (!eligible[i]) continue;
        if (!InsOrderIdValid(orderId[i])) continue;
        if (best < 0 || orderId[i] < bestOrder)
        {
            best = i;
            bestOrder = orderId[i];
        }
    }
    return best;
}
