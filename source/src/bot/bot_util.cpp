// ============================================================================
// bot_util.cpp - small shared helpers used across the bot/ translation units.
//
// These are the lowest-level primitives in the bot tree: maths, randomness,
// command-token matching, debug logging, and the two map-identity predicates.
// Everything here is consumed by at least one other bot/ TU, so nothing in
// this file is `static`.
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#include "tier0/dbg.h"
#include "interface.h"
#include "eiface.h"
#include "edict.h"
#include "convar.h"
#include "bot.h"
#include "nav.h"
#include "objective.h"
#include "bot_internal.h"

// Engine interface and the debug cvar are declared extern in each TU that
// needs them (see bot.cpp and objective.cpp for the same pattern).
extern IVEngineServer *engine;
extern CfgVar insbot_debug;

float DistanceToLineSegment(const Vector& p, const Vector& a, const Vector& b)
{
    Vector ab = b - a;
    Vector ap = p - a;
    float l2 = ab.x*ab.x + ab.y*ab.y + ab.z*ab.z;
    if (l2 == 0.0f) {
        float dx = p.x - a.x, dy = p.y - a.y, dz = p.z - a.z;
        return sqrtf(dx*dx + dy*dy + dz*dz);
    }
    float t = (ap.x*ab.x + ap.y*ab.y + ap.z*ab.z) / l2;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    Vector proj = a + ab * t;
    float dx = p.x - proj.x, dy = p.y - proj.y, dz = p.z - proj.z;
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

float SignedAngleDiff(float a, float b)
{
    float d = fmodf(a - b, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return d;
}

float frand(float lo, float hi)
{
    return lo + (hi - lo) * ((float)rand() / (float)RAND_MAX);
}

int RandomIndex(int count)
{
    if (count <= 1) return 0;
    return (int)(((unsigned long long)(unsigned int)rand()
                  * (unsigned long long)(unsigned int)count)
                 / ((unsigned long long)(unsigned int)RAND_MAX + 1ULL));
}

bool CommandContainsToken(const char *command, const char *token)
{
    if (!command || !token || !token[0]) return false;
    size_t len = strlen(token);
    for (const char *p = command; *p; p++)
    {
        if (strlen(p) < len) break;
        if (_strnicmp(p, token, len) != 0) continue;
        bool left = (p == command || p[-1] == ';' || p[-1] == ' '
                     || p[-1] == '\t' || p[-1] == '\r' || p[-1] == '\n');
        char after = p[len];
        bool right = (after == 0 || after == ';' || after == ' '
                      || after == '\t' || after == '\r' || after == '\n');
        if (left && right) return true;
    }
    return false;
}

void BotDbg(const char *fmt, ...)
{
    if (!insbot_debug.GetBool() || !engine) return;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    Msg("%s", buf);
}

float xyd(const Vector &a, const Vector &b)
{
    float dx = a.x - b.x, dy = a.y - b.y;
    return sqrtf(dx * dx + dy * dy);
}

bool IsHadithaMapName(const char *name)
{
    // Keep the original map-name recognition as a fallback.  The fixup sidecar
    // adds data-driven edges, but a missing sidecar must not silently disable
    // the entire Haditha door controller.
    if (name && (!_stricmp(name, "ins_haditha") ||
                 !_stricmp(name, "ins_haditha_night")))
        return true;
    return g_NavMesh.HasFixupKind(NAV_FIXUP_TIGHT_EDGE);
}

bool IsHadithaTightDoorEdge(int fromIdx, int toIdx)
{
    if (g_NavMesh.HasFixupEdge(NAV_FIXUP_TIGHT_EDGE, fromIdx, toIdx))
        return true;
    if (!IsHadithaMapName(g_Bots.m_mapname)) return false;

    const NavAreaData *from = g_NavMesh.Area(fromIdx);
    const NavAreaData *to = g_NavMesh.Area(toIdx);
    if (!from || !to) return false;
    unsigned int a = from->id, b = to->id;
    return (a == 800  && b == 464)  || (a == 464  && b == 800)
        || (a == 464  && b == 3274) || (a == 3274 && b == 464)
        || (a == 3274 && b == 235)  || (a == 235  && b == 3274)
        || (a == 464  && b == 3321)
        || (a == 3321 && b == 463);
}

bool IsHadithaEastDoorEdge(int fromIdx, int toIdx)
{
    if (g_NavMesh.HasFixupEdge(NAV_FIXUP_EAST_EDGE, fromIdx, toIdx))
        return true;
    if (!IsHadithaMapName(g_Bots.m_mapname)) return false;

    const NavAreaData *from = g_NavMesh.Area(fromIdx);
    const NavAreaData *to = g_NavMesh.Area(toIdx);
    if (!from || !to) return false;
    unsigned int a = from->id, b = to->id;
    // The old Haditha mesh routes through a seam of 25u tiles before the
    // 3295/3315 pair.  Treat the whole chain as the same east-door choke.
    return (a == 463  && b == 3289) || (a == 3289 && b == 463)
        || (a == 3289 && b == 3319) || (a == 3319 && b == 3289)
        || (a == 3319 && b == 3293) || (a == 3293 && b == 3319)
        || (a == 3293 && b == 3315) || (a == 3315 && b == 3293)
        || (a == 3295 && b == 3315) || (a == 3315 && b == 3295)
        || (a == 3295 && b == 320)  || (a == 320  && b == 3295)
        || (a == 3315 && b == 320)  || (a == 320  && b == 3315);
}
