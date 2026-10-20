// ============================================================================
// nav.cpp - .nav loader (v4..v9) + A* over area centers
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "nav.h"
#include "profile.h"
#ifndef TESTING_NATIVE
#include "cfgvar.h"
#include "bot.h"
#include "engine/IEngineTrace.h"
#include "gametrace.h"
#include "bspflags.h"
#endif

CNavMesh g_NavMesh;
#ifndef TESTING_NATIVE
extern CfgVar insbot_nav;
extern IEngineTrace *enginetrace;
#endif

bool CNavMesh::IsLoaded() const
{
#ifdef TESTING_NATIVE
    return m_count > 0;
#else
    return m_count > 0 && (insbot_nav.GetBool() || g_ForceGotoActive);
#endif
}

// jump/crouch avoidance penalties (added to A* edge cost for areas flagged
// NAVATTR_JUMP / NAVATTR_CROUCH). The plugin overwrites these from cvars; the
// defaults make bots strongly prefer flat routes. Big = avoid, 0 = don't care.
float g_navJumpPenalty   = 4000.0f;
float g_navCrouchPenalty = 2500.0f;
float g_navAvoidPenalty  = 3000.0f;   // NAVATTR_AVOID areas (e.g. jump/crouch marked avoid)
float g_navLadderPenalty   = 3000.0f; // ladder edges cost extra so A* prefers flat routes when available
float g_navLadderUpPenalty = 500.0f;  // climbing up is slower/riskier than descending

// Dynamic firefight-avoidance zones (capture-committed bots only). The roster
// refreshes these every frame from bots that are actively engaging; the A*
// penalty below makes a committed bot detour around a fight when a flat
// alternative exists, but still push through when the fight blocks the only
// route. Zones expire automatically because they are rebuilt from scratch
// each frame.
// 64 slots = up to 32 simultaneous two-sided firefights before a zone is
// silently dropped (each fight adds the bot origin + the enemy last-seen).
#define INSBOT_MAX_DANGER_ZONES 64
static Vector s_dangerPos[INSBOT_MAX_DANGER_ZONES];
static float  s_dangerRadius[INSBOT_MAX_DANGER_ZONES];
static int    s_dangerCount = 0;

float g_navDangerPenalty = 6000.0f; // added per area inside a danger zone
bool  g_navDangerEnabled = false;   // set per-bot right before BuildPath

void CNavMesh::RefreshCostParamsFromGlobals()
{
    NavCostParams next;
    next.jumpPenalty = g_navJumpPenalty;
    next.crouchPenalty = g_navCrouchPenalty;
    next.avoidPenalty = g_navAvoidPenalty;
    next.ladderPenalty = g_navLadderPenalty;
    next.ladderUpPenalty = g_navLadderUpPenalty;
    next.dangerPenalty = g_navDangerPenalty;
    next.dangerEnabled = g_navDangerEnabled;

    if (next.jumpPenalty != m_costParams.jumpPenalty
        || next.crouchPenalty != m_costParams.crouchPenalty
        || next.avoidPenalty != m_costParams.avoidPenalty
        || next.ladderPenalty != m_costParams.ladderPenalty
        || next.ladderUpPenalty != m_costParams.ladderUpPenalty
        || next.dangerPenalty != m_costParams.dangerPenalty
        || next.dangerEnabled != m_costParams.dangerEnabled)
    {
        m_costParams = next;
        InvalidateCostCache();
    }
}

void NavClearDangerZones()
{
    s_dangerCount = 0;
}

void NavAddDangerZone(const Vector &center, float radius)
{
    if (s_dangerCount < INSBOT_MAX_DANGER_ZONES)
    {
        s_dangerPos[s_dangerCount] = center;
        s_dangerRadius[s_dangerCount] = (radius < 0.0f) ? 0.0f : radius;
        s_dangerCount++;
    }
}

#define NAV_MAGIC 0xFEEDFACE

// ---- little binary reader ---------------------------------------------------
struct BinReader
{
    const unsigned char *p, *end;
    bool ok;
    BinReader(const unsigned char *data, unsigned int len) : p(data), end(data + len), ok(true) {}
    bool Get(void *out, unsigned int n)
    {
        if (!ok || p + n > end) { ok = false; return false; }
        memcpy(out, p, n); p += n; return true;
    }
    unsigned int   U32() { unsigned int v = 0;   Get(&v, 4); return v; }
    unsigned short U16() { unsigned short v = 0; Get(&v, 2); return v; }
    unsigned char  U8()  { unsigned char v = 0;  Get(&v, 1); return v; }
    float          F32() { float v = 0;          Get(&v, 4); return v; }
    void Skip(unsigned int n) { if (p + n > end) ok = false; else p += n; }
};

// ---- allocation helpers for NAV loading ------------------------------------
#ifndef TESTING_NATIVE
#define NAVLOAD_ERR(...) Msg(__VA_ARGS__)
#else
#define NAVLOAD_ERR(...) printf(__VA_ARGS__)
#endif

#define NAV_MAX_CONNECTIONS_PER_AREA 512

// realloc that never leaks or overwrites the original block on failure.
static bool CheckedRealloc(void **target, size_t bytes)
{
    void *tmp = realloc(*target, bytes);
    if (!tmp) return false;
    *target = tmp;
    return true;
}

// Every temporary allocation owned by CNavMesh::Load lives here so every
// failure path frees the same set exactly once. All members start zeroed.
struct NavLoadScratch
{
    unsigned char  *data;
    unsigned int   *connIds;        // flat serialized connection IDs
    int            *connOffsets;    // areaCount + 1 prefix offsets
    int             connTotal;
    int             connCapacity;
    unsigned int  **ladderUpIds;
    unsigned int  **ladderDownIds;
    int            *ladderUpCounts;
    int            *ladderDownCounts;
    unsigned int   *ladderAreaIds;
    int            *edgeIdx;
    unsigned char  *edgeBad;
    int            *edgeLadder;
    NavEdgeMeta    *edgeMeta;
    int            *edgeOffsets;
    int            *edgeCounts;
    int            *extraOffsets;
    int            *extraCounts;
    int             edgeTotal;
    int             edgeCapacity;
};

static void FreeNavLoadScratch(NavLoadScratch &s, unsigned int areaCount)
{
    for (unsigned int i = 0; i < areaCount; i++)
    {
        if (s.ladderUpIds)  { if (s.ladderUpIds[i])  free(s.ladderUpIds[i]);  s.ladderUpIds[i] = 0; }
        if (s.ladderDownIds){ if (s.ladderDownIds[i])free(s.ladderDownIds[i]);s.ladderDownIds[i] = 0; }
    }
    if (s.connIds)       free(s.connIds);
    if (s.connOffsets)   free(s.connOffsets);
    if (s.ladderUpIds)   free(s.ladderUpIds);
    if (s.ladderDownIds) free(s.ladderDownIds);
    if (s.ladderUpCounts)    free(s.ladderUpCounts);
    if (s.ladderDownCounts)  free(s.ladderDownCounts);
    if (s.ladderAreaIds) free(s.ladderAreaIds);
    if (s.edgeIdx)       free(s.edgeIdx);
    if (s.edgeBad)       free(s.edgeBad);
    if (s.edgeLadder)    free(s.edgeLadder);
    if (s.edgeMeta)      free(s.edgeMeta);
    if (s.edgeOffsets)   free(s.edgeOffsets);
    if (s.edgeCounts)    free(s.edgeCounts);
    if (s.extraOffsets)  free(s.extraOffsets);
    if (s.extraCounts)   free(s.extraCounts);
    if (s.data)          free(s.data);
    memset(&s, 0, sizeof(s));
}
void CNavMesh::Free()
{
    InvalidateCostCache();
    if (m_areas)
    {
        for (int i = 0; i < m_count; i++)
        {
            if (m_areas[i].hidingSpots) free(m_areas[i].hidingSpots);
        }
        free(m_areas);
    }
    if (m_connIdx) free(m_connIdx);
    if (m_connBad) free(m_connBad);
    if (m_connLadder) free(m_connLadder);
    if (m_connMeta) free(m_connMeta);
    if (m_areaIdTable) free(m_areaIdTable);
    if (m_sniperSpots) free(m_sniperSpots);
    if (m_ladders) free(m_ladders);
    if (m_gridStart) free(m_gridStart);
    if (m_gridCells) free(m_gridCells);
    m_ladders = 0; m_ladderCount = 0;
    m_connIdx = 0; m_connBad = 0; m_connLadder = 0; m_connMeta = 0;
    m_connCount = 0;
    m_areaIdTable = 0; m_areaIdTableSize = 0;
    m_sniperSpots = 0; m_sniperSpotCount = 0;
    m_gridStart = 0; m_gridCells = 0;
    m_gridW = m_gridH = 0;
    m_areas = 0; m_count = 0;
    for (int i = 0; i < 64; i++)
    {
        m_fbCache[i].key = 0;
        m_fbCache[i].area = -1;
    }
}

static unsigned int HashNavAreaId(unsigned int value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

bool CNavMesh::BuildAreaIdMap()
{
    if (m_areaIdTable) free(m_areaIdTable);
    m_areaIdTable = 0;
    m_areaIdTableSize = 0;
    if (m_count <= 0) return true;

    int tableSize = 16;
    while (tableSize < m_count * 2) tableSize <<= 1;
    m_areaIdTable = (int *)calloc((size_t)tableSize, sizeof(int));
    if (!m_areaIdTable) return false;
    m_areaIdTableSize = tableSize;

    for (int i = 0; i < m_count; i++)
    {
        unsigned int slot = HashNavAreaId(m_areas[i].id) & (unsigned int)(tableSize - 1);
        while (m_areaIdTable[slot])
        {
            int existing = m_areaIdTable[slot] - 1;
            if (m_areas[existing].id == m_areas[i].id)
            {
                NAVLOAD_ERR("[insbots] nav load failed: duplicate area id %u at indices %d and %d\n",
                            m_areas[i].id, existing, i);
                free(m_areaIdTable);
                m_areaIdTable = 0;
                m_areaIdTableSize = 0;
                return false;
            }
            slot = (slot + 1) & (unsigned int)(tableSize - 1);
        }
        m_areaIdTable[slot] = i + 1;
    }
    return true;
}

int CNavMesh::IndexOfId(unsigned int id) const
{
    if (!m_areaIdTable || m_areaIdTableSize <= 0) return -1;
    unsigned int slot = HashNavAreaId(id) & (unsigned int)(m_areaIdTableSize - 1);
    for (int probe = 0; probe < m_areaIdTableSize; probe++)
    {
        int stored = m_areaIdTable[slot];
        if (!stored) return -1;
        int index = stored - 1;
        if (m_areas[index].id == id) return index;
        slot = (slot + 1) & (unsigned int)(m_areaIdTableSize - 1);
    }
    return -1;
}

// uniform spatial index (512u cells) over area 2D bounds. FindArea /
// FindNearestArea are called per bot per frame; on sinjar (~25k areas) the
// old full scan was the main lag source on large maps. An area is inserted
// into every cell its rectangle overlaps, so point lookups only touch local
// cells.
void CNavMesh::BuildGrid()
{
    const float CELL = 512.0f;
    if (m_gridStart) { free(m_gridStart); m_gridStart = 0; }
    if (m_gridCells) { free(m_gridCells); m_gridCells = 0; }
    m_gridW = m_gridH = 0;
    m_gridCell = CELL;

    if (!m_areas || m_count <= 0)
        return;

    float minX = m_areas[0].lo.x, maxX = m_areas[0].hi.x;
    float minY = m_areas[0].lo.y, maxY = m_areas[0].hi.y;
    for (int i = 1; i < m_count; i++)
    {
        const NavAreaData &a = m_areas[i];
        if (a.lo.x < minX) minX = a.lo.x;
        if (a.hi.x > maxX) maxX = a.hi.x;
        if (a.lo.y < minY) minY = a.lo.y;
        if (a.hi.y > maxY) maxY = a.hi.y;
    }
    // pad so the outermost area boundaries land inside the grid
    minX -= CELL; minY -= CELL;
    maxX += CELL; maxY += CELL;

    int w = (int)((maxX - minX) / CELL) + 1;
    int h = (int)((maxY - minY) / CELL) + 1;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 512) w = 512;   // 512*512 cells @512u covers any Source map
    if (h > 512) h = 512;

    const int cellCount = w * h;
    int *counts = (int *)calloc(cellCount, sizeof(int));
    if (!counts) return;

    m_gridStart = (int *)malloc((cellCount + 1) * sizeof(int));
    if (!m_gridStart) { free(counts); return; }

    m_gridW = w; m_gridH = h;
    m_gridMinX = minX; m_gridMinY = minY;

    for (int i = 0; i < m_count; i++)
    {
        const NavAreaData &a = m_areas[i];
        int x0 = (int)((a.lo.x - minX) / CELL); if (x0 < 0) x0 = 0; else if (x0 >= w) x0 = w - 1;
        int x1 = (int)((a.hi.x - minX) / CELL); if (x1 < 0) x1 = 0; else if (x1 >= w) x1 = w - 1;
        int y0 = (int)((a.lo.y - minY) / CELL); if (y0 < 0) y0 = 0; else if (y0 >= h) y0 = h - 1;
        int y1 = (int)((a.hi.y - minY) / CELL); if (y1 < 0) y1 = 0; else if (y1 >= h) y1 = h - 1;
        for (int cy = y0; cy <= y1; cy++)
            for (int cx = x0; cx <= x1; cx++)
                counts[cy * w + cx]++;
    }

    int total = 0;
    for (int c = 0; c < cellCount; c++)
    {
        m_gridStart[c] = total;
        total += counts[c];
    }
    m_gridStart[cellCount] = total;

    m_gridCells = (int *)malloc(total * sizeof(int));
    int *cursor = 0;
    if (m_gridCells)
    {
        cursor = (int *)malloc(cellCount * sizeof(int));
        if (!cursor)
        {
            free(m_gridCells); m_gridCells = 0;
        }
    }
    if (!m_gridCells)
    {
        free(counts); free(m_gridStart); m_gridStart = 0;
        m_gridW = m_gridH = 0;
        return;
    }
    memcpy(cursor, m_gridStart, cellCount * sizeof(int));

    for (int i = 0; i < m_count; i++)
    {
        const NavAreaData &a = m_areas[i];
        int x0 = (int)((a.lo.x - minX) / CELL); if (x0 < 0) x0 = 0; else if (x0 >= w) x0 = w - 1;
        int x1 = (int)((a.hi.x - minX) / CELL); if (x1 < 0) x1 = 0; else if (x1 >= w) x1 = w - 1;
        int y0 = (int)((a.lo.y - minY) / CELL); if (y0 < 0) y0 = 0; else if (y0 >= h) y0 = h - 1;
        int y1 = (int)((a.hi.y - minY) / CELL); if (y1 < 0) y1 = 0; else if (y1 >= h) y1 = h - 1;
        for (int cy = y0; cy <= y1; cy++)
        {
            const int row = cy * w;
            for (int cx = x0; cx <= x1; cx++)
                m_gridCells[cursor[row + cx]++] = i;
        }
    }

    free(counts);
    free(cursor);
}

bool CNavMesh::Load(const char *path)
{
    Free();

    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 16) { fclose(fp); return false; }

    NavLoadScratch s;
    memset(&s, 0, sizeof(s));

    s.data = (unsigned char *)malloc(size);
    if (!s.data) { fclose(fp); return false; }
    if (fread(s.data, 1, size, fp) != (size_t)size) { FreeNavLoadScratch(s, 0); fclose(fp); return false; }
    fclose(fp);

    BinReader r(s.data, (unsigned int)size);

    if (r.U32() != NAV_MAGIC) { FreeNavLoadScratch(s, 0); return false; }
    unsigned int version = r.U32();
    if (version < 4 || version > 9) { FreeNavLoadScratch(s, 0); return false; }

    r.U32();                       // bsp file size (v4+), ignored

    if (version >= 5)              // place directory
    {
        unsigned short placeCount = r.U16();
        for (int i = 0; i < placeCount && r.ok; i++)
        {
            unsigned short len = r.U16();
            r.Skip(len);
        }
    }

    unsigned int areaCount = r.U32();
    if (!r.ok || areaCount == 0 || areaCount > 250000) { FreeNavLoadScratch(s, 0); return false; }

    m_areas = (NavAreaData *)calloc(areaCount, sizeof(NavAreaData));
    if (!m_areas) { FreeNavLoadScratch(s, 0); return false; }
    m_count = (int)areaCount;

    // temporary connection id storage (one contiguous buffer)
    s.connOffsets = (int *)calloc(areaCount + 1, sizeof(int));
    s.ladderUpIds = (unsigned int **)calloc(areaCount, sizeof(unsigned int *));
    s.ladderDownIds = (unsigned int **)calloc(areaCount, sizeof(unsigned int *));
    s.ladderUpCounts = (int *)calloc(areaCount, sizeof(int));
    s.ladderDownCounts = (int *)calloc(areaCount, sizeof(int));
    if (!s.connOffsets || !s.ladderUpIds || !s.ladderDownIds ||
        !s.ladderUpCounts || !s.ladderDownCounts)
    {
        NAVLOAD_ERR("[insbots] nav load failed: out of memory allocating area scratch buffers\n");
        FreeNavLoadScratch(s, areaCount);
        Free();
        return false;
    }

    for (unsigned int i = 0; i < areaCount && r.ok; i++)
    {
        NavAreaData &a = m_areas[i];
        a.id = r.U32();
        a.attributes = (version <= 8) ? r.U8() : r.U16();

        a.lo.x = r.F32(); a.lo.y = r.F32(); a.lo.z = r.F32();
        a.hi.x = r.F32(); a.hi.y = r.F32(); a.hi.z = r.F32();
        a.neZ = r.F32();
        a.swZ = r.F32();
        a.center = Vector((a.lo.x + a.hi.x) * 0.5f, (a.lo.y + a.hi.y) * 0.5f, (a.lo.z + a.hi.z) * 0.5f);

        // connections: 4 directions. Every serialized ID is consumed so the
        // parser position stays correct; a mesh with more than
        // NAV_MAX_CONNECTIONS_PER_AREA links for one area is rejected instead
        // of silently truncating the link set.
        int total = 0;
        int serializedConnectionCount = 0;
        unsigned int ids[NAV_MAX_CONNECTIONS_PER_AREA];
        bool connectionOverflow = false;
        for (int d = 0; d < NAV_DIR_COUNT && r.ok; d++)
        {
            unsigned int c = r.U32();
            if (c > 10000) { r.ok = false; break; }
            for (unsigned int k = 0; k < c && r.ok; k++)
            {
                unsigned int id = r.U32();
                serializedConnectionCount++;
                if (total < NAV_MAX_CONNECTIONS_PER_AREA) ids[total++] = id;
                else if (!connectionOverflow)
                {
                    connectionOverflow = true;
                    NAVLOAD_ERR("[insbots] nav area %u has %d serialized connections (max %d); rejecting mesh\n", a.id, serializedConnectionCount, NAV_MAX_CONNECTIONS_PER_AREA);
                }
            }
        }
        if (connectionOverflow) { r.ok = false; break; }
        s.connOffsets[i] = s.connTotal;
        if (total)
        {
            int needed = s.connTotal + total;
            if (needed > s.connCapacity)
            {
                int capacity = s.connCapacity > 0 ? s.connCapacity : 4096;
                while (capacity < needed) capacity *= 2;
                if (!CheckedRealloc((void **)&s.connIds,
                                    (size_t)capacity * sizeof(unsigned int)))
                {
                    r.ok = false;
                    break;
                }
                s.connCapacity = capacity;
            }
            memcpy(s.connIds + s.connTotal, ids, (size_t)total * sizeof(unsigned int));
            s.connTotal = needed;
        }

        // hiding spots (Phase 1: parse and retain coordinates & sniper flags)
        unsigned char hides = r.U8();
        a.hidingSpotCount = hides;
        a.hidingSpots = 0;
        if (hides > 0 && r.ok)
        {
            a.hidingSpots = (HidingSpot *)calloc(hides, sizeof(HidingSpot));
            if (!a.hidingSpots) { r.ok = false; break; }
            for (int h = 0; h < hides && r.ok; h++)
            {
                HidingSpot &sp = a.hidingSpots[h];
                sp.id = (version == 1) ? (unsigned)h : r.U32();
                sp.pos.x = r.F32(); sp.pos.y = r.F32(); sp.pos.z = r.F32();
                sp.flags = (version == 1) ? 0 : r.U8();
                sp.areaIdx = (int)i;
            }
        }

        // approach areas
        unsigned char approaches = r.U8();
        r.Skip(approaches * (4 + 4 + 1 + 4 + 1));

        // encounter paths
        unsigned int enc = r.U32();
        if (enc > 10000) r.ok = false;
        for (unsigned int e = 0; e < enc && r.ok; e++)
        {
            r.Skip(4 + 1 + 4 + 1);          // from,fromDir,to,toDir
            unsigned char spots = r.U8();
            r.Skip(spots * (4 + 1));        // id + t
        }

        if (version >= 5) r.U16();          // place index

        if (version >= 7)                    // per-area ladder connections (up/down)
        {
            for (int d = 0; d < 2 && r.ok; d++)
            {
                unsigned int c = r.U32();
                if (c > 10000) { r.ok = false; break; }
                a.ladderConnCount += (int)c;
                unsigned int *ladderIds = c ? (unsigned int *)malloc(c * sizeof(unsigned int)) : 0;
                if (c && !ladderIds) { r.ok = false; break; }
                for (unsigned int k = 0; k < c && r.ok; k++) ladderIds[k] = r.U32();
                if (d == 0) { s.ladderUpIds[i] = ladderIds; s.ladderUpCounts[i] = (int)c; }
                else        { s.ladderDownIds[i] = ladderIds; s.ladderDownCounts[i] = (int)c; }
            }
        }

        if (version >= 8) r.Skip(2 * 4);    // earliest occupy times (2 teams)
    }
    s.connOffsets[areaCount] = s.connTotal;

    // Mesh-level ladder list. Source v7-v9 stores 60 bytes per ladder:
    // id,width,top,bottom,length,dir and five connecting area IDs.
    if (version >= 7 && r.ok)
    {
        unsigned int ladderCount = r.U32();
        if (ladderCount > 10000) r.ok = false;
        if (r.ok && ladderCount)
        {
            m_ladders = (NavLadderData *)calloc(ladderCount, sizeof(NavLadderData));
            s.ladderAreaIds = (unsigned int *)calloc(ladderCount * 5, sizeof(unsigned int));
            if (!m_ladders || !s.ladderAreaIds) r.ok = false;
            else m_ladderCount = (int)ladderCount;
        }
        for (unsigned int li = 0; li < ladderCount && r.ok; li++)
        {
            NavLadderData &l = m_ladders[li];
            l.id = r.U32(); l.width = r.F32();
            l.top.x = r.F32(); l.top.y = r.F32(); l.top.z = r.F32();
            l.bottom.x = r.F32(); l.bottom.y = r.F32(); l.bottom.z = r.F32();
            l.length = r.F32(); l.dir = (int)r.U32();
            for (int endpoint = 0; endpoint < 5; endpoint++)
                s.ladderAreaIds[li * 5 + endpoint] = r.U32();
        }
    }

    if (!r.ok)
    {
        FreeNavLoadScratch(s, areaCount);
        Free();
        return false;
    }

    if (!BuildAreaIdMap())
    {
        FreeNavLoadScratch(s, areaCount);
        Free();
        return false;
    }

    // Resolve all serialized connections into one flat edge buffer. The
    // capacity includes the maximum four directed edges a ladder can append.
    s.edgeCapacity = s.connTotal + m_ladderCount * 4;
    s.edgeOffsets = (int *)calloc(areaCount + 1, sizeof(int));
    s.edgeCounts = (int *)calloc(areaCount, sizeof(int));
    s.extraOffsets = (int *)calloc(areaCount, sizeof(int));
    s.extraCounts = (int *)calloc(areaCount, sizeof(int));
    if (!s.edgeOffsets || !s.edgeCounts || !s.extraOffsets || !s.extraCounts)
    {
        NAVLOAD_ERR("[insbots] nav load failed: out of memory allocating edge offsets\n");
        FreeNavLoadScratch(s, areaCount);
        Free();
        return false;
    }
    if (s.edgeCapacity > 0)
    {
        s.edgeIdx = (int *)malloc((size_t)s.edgeCapacity * sizeof(int));
        s.edgeBad = (unsigned char *)calloc((size_t)s.edgeCapacity, 1);
        s.edgeLadder = (int *)calloc((size_t)s.edgeCapacity, sizeof(int));
        s.edgeMeta = (NavEdgeMeta *)calloc((size_t)s.edgeCapacity, sizeof(NavEdgeMeta));
        if (!s.edgeIdx || !s.edgeBad || !s.edgeLadder || !s.edgeMeta)
        {
            NAVLOAD_ERR("[insbots] nav load failed: out of memory allocating flat edge storage\n");
            FreeNavLoadScratch(s, areaCount);
            Free();
            return false;
        }
    }
    for (int i = 0; i < m_count; i++)
    {
        int offset = s.edgeTotal;
        for (int k = s.connOffsets[i]; k < s.connOffsets[i + 1]; k++)
        {
            int idx = IndexOfId(s.connIds[k]);
            if (idx < 0 || idx == i) continue;   // missing or self-referential
            int slot = s.edgeTotal++;
            s.edgeIdx[slot] = idx;
            s.edgeBad[slot] = 0;
            s.edgeLadder[slot] = 0;
            memset(&s.edgeMeta[slot], 0, sizeof(NavEdgeMeta));
        }
        s.edgeOffsets[i] = offset;
        s.edgeCounts[i] = s.edgeTotal - offset;
    }
    s.edgeOffsets[areaCount] = s.edgeTotal;

    // Resolve ladder endpoints, then append the directed connections that are
    // explicitly present in each area's up/down ladder list. Top-behind is
    // retained as metadata but is not a climb-up destination, matching Source.
    for (int li = 0; li < m_ladderCount; li++)
    {
        NavLadderData &l = m_ladders[li];
        l.topForwardArea = IndexOfId(s.ladderAreaIds[li * 5 + 0]);
        l.topLeftArea    = IndexOfId(s.ladderAreaIds[li * 5 + 1]);
        l.topRightArea   = IndexOfId(s.ladderAreaIds[li * 5 + 2]);
        l.topBehindArea  = IndexOfId(s.ladderAreaIds[li * 5 + 3]);
        l.bottomArea     = IndexOfId(s.ladderAreaIds[li * 5 + 4]);
    }
    for (int i = 0; i < m_count; i++)
    {
        for (int direction = 0; direction < 2; direction++)
        {
            unsigned int *ids = direction == 0 ? s.ladderUpIds[i] : s.ladderDownIds[i];
            int count = direction == 0 ? s.ladderUpCounts[i] : s.ladderDownCounts[i];
            for (int k = 0; k < count; k++)
            {
                int li = -1;
                for (int q = 0; q < m_ladderCount; q++)
                    if (m_ladders[q].id == ids[k]) { li = q; break; }
                if (li < 0) continue;
                NavLadderData &l = m_ladders[li];
                int targets[3]; int targetCount = 0;
                if (direction == 0)
                {
                    if (l.bottomArea != i) continue;
                    targets[targetCount++] = l.topForwardArea;
                    targets[targetCount++] = l.topLeftArea;
                    targets[targetCount++] = l.topRightArea;
                }
                else
                {
                    targets[targetCount++] = l.bottomArea;
                }
                for (int t = 0; t < targetCount; t++)
                {
                    int target = targets[t];
                    if (target < 0 || target == i) continue;
                    // A typed ladder is only meaningful when its destination
                    // landing and source floor are vertically compatible with
                    // the recorded lips. Checking both sides matters for a
                    // descending-only reference: its bottom may be valid even
                    // when the serialized top begins far above empty space.
                    // Fail closed without touching learned-link confidence:
                    // malformed records must not trap a runtime ladder queue.
                    const NavAreaData &sourceArea = m_areas[i];
                    const NavAreaData &targetArea = m_areas[target];
                    float sourceX = direction == 0 ? l.bottom.x : l.top.x;
                    float sourceY = direction == 0 ? l.bottom.y : l.top.y;
                    if (sourceX < sourceArea.lo.x) sourceX = sourceArea.lo.x;
                    else if (sourceX > sourceArea.hi.x) sourceX = sourceArea.hi.x;
                    if (sourceY < sourceArea.lo.y) sourceY = sourceArea.lo.y;
                    else if (sourceY > sourceArea.hi.y) sourceY = sourceArea.hi.y;
                    float sourceLipZ = direction == 0 ? l.bottom.z : l.top.z;
                    float sourceLandingZ = GetZ(i, sourceX, sourceY);
                    if (fabsf(sourceLandingZ - sourceLipZ) > 96.0f)
                    {
                        NAVLOAD_ERR("[insbots] ladder %u ignored: source area %u is %.0fu from the recorded %s lip\n",
                                    l.id, sourceArea.id, fabsf(sourceLandingZ - sourceLipZ),
                                    direction == 0 ? "bottom" : "top");
                        continue;
                    }
                    float px = direction == 0 ? l.top.x : l.bottom.x;
                    float py = direction == 0 ? l.top.y : l.bottom.y;
                    if (px < targetArea.lo.x) px = targetArea.lo.x;
                    else if (px > targetArea.hi.x) px = targetArea.hi.x;
                    if (py < targetArea.lo.y) py = targetArea.lo.y;
                    else if (py > targetArea.hi.y) py = targetArea.hi.y;
                    float lipZ = direction == 0 ? l.top.z : l.bottom.z;
                    float landingZ = GetZ(target, px, py);
                    if (fabsf(landingZ - lipZ) > 96.0f)
                    {
                        NAVLOAD_ERR("[insbots] ladder %u ignored: landing area %u is %.0fu from the recorded %s lip\n",
                                    l.id, targetArea.id, fabsf(landingZ - lipZ),
                                    direction == 0 ? "top" : "bottom");
                        continue;
                    }
                    bool duplicate = false;
                    int ladderCode = direction == 0 ? li + 1 : -(li + 1);
                    int areaEdgeOffset = s.edgeOffsets[i];
                    for (int e = 0; e < s.edgeCounts[i]; e++)
                    {
                        int edgeSlot = areaEdgeOffset + e;
                        if (s.edgeIdx[edgeSlot] == target)
                        {
                            // nav_generate may also serialize a conventional
                            // area edge between the same two landings. Upgrade
                            // it to a typed ladder edge so movement does not try
                            // to walk straight through the vertical transition.
                            s.edgeLadder[edgeSlot] = ladderCode;
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate)
                    {
                        for (int e = 0; e < s.extraCounts[i]; e++)
                        {
                            int edgeSlot = s.extraOffsets[i] + e;
                            if (s.edgeIdx[edgeSlot] == target)
                            {
                                s.edgeLadder[edgeSlot] = ladderCode;
                                duplicate = true;
                                break;
                            }
                        }
                    }
                    if (duplicate) continue;
                    if (s.edgeTotal >= s.edgeCapacity)
                    {
                        NAVLOAD_ERR("[insbots] nav load failed: ladder edge capacity exceeded for area %u\n", m_areas[i].id);
                        FreeNavLoadScratch(s, areaCount);
                        Free();
                        return false;
                    }
                    if (s.extraCounts[i] == 0)
                        s.extraOffsets[i] = s.edgeTotal;
                    int slot = s.edgeTotal++;
                    s.edgeIdx[slot] = target;
                    s.edgeBad[slot] = 0;
                    s.edgeLadder[slot] = ladderCode;
                    memset(&s.edgeMeta[slot], 0, sizeof(NavEdgeMeta));
                    s.extraCounts[i]++;
                }
            }
        }
    }

    // Publish the flat edge storage. Per-area pointers are views only; Free()
    // releases the four backing arrays once.
    m_connCount = s.edgeTotal;
    if (m_connCount > 0)
    {
        m_connIdx = (int *)malloc((size_t)m_connCount * sizeof(int));
        m_connBad = (unsigned char *)calloc((size_t)m_connCount, 1);
        m_connLadder = (int *)calloc((size_t)m_connCount, sizeof(int));
        m_connMeta = (NavEdgeMeta *)calloc((size_t)m_connCount, sizeof(NavEdgeMeta));
        if (!m_connIdx || !m_connBad || !m_connLadder || !m_connMeta)
        {
            NAVLOAD_ERR("[insbots] nav load failed: out of memory compacting flat edge storage\n");
            FreeNavLoadScratch(s, areaCount);
            Free();
            return false;
        }
    }
    int dst = 0;
    for (int i = 0; i < m_count; i++)
    {
        NavAreaData &a = m_areas[i];
        a.connOffset = dst;
        a.connCount = s.edgeCounts[i] + s.extraCounts[i];
        if (a.connCount > 0)
        {
            a.connIdx = &m_connIdx[a.connOffset];
            a.connBad = &m_connBad[a.connOffset];
            a.connLadder = &m_connLadder[a.connOffset];
            a.connMeta = &m_connMeta[a.connOffset];
        }
        else
        {
            a.connIdx = 0; a.connBad = 0; a.connLadder = 0; a.connMeta = 0;
        }
        for (int k = 0; k < s.edgeCounts[i]; k++)
        {
            int src = s.edgeOffsets[i] + k;
            m_connIdx[dst] = s.edgeIdx[src];
            m_connBad[dst] = s.edgeBad[src];
            m_connLadder[dst] = s.edgeLadder[src];
            m_connMeta[dst] = s.edgeMeta[src];
            dst++;
        }
        for (int k = 0; k < s.extraCounts[i]; k++)
        {
            int src = s.extraOffsets[i] + k;
            m_connIdx[dst] = s.edgeIdx[src];
            m_connBad[dst] = s.edgeBad[src];
            m_connLadder[dst] = s.edgeLadder[src];
            m_connMeta[dst] = s.edgeMeta[src];
            dst++;
        }
    }
    FreeNavLoadScratch(s, areaCount);

    // Classify every resolved connection once. Portal queries are hot paths
    // (steering, doorway lanes and stuck recovery), so they must not re-derive
    // rectangle relationships or mistake stacked floors for walkable seams.
    for (int i = 0; i < m_count; i++)
    {
        for (int c = 0; c < m_areas[i].connCount; c++)
            BuildEdgeMetadata(i, c, m_areas[i].connMeta[c]);
    }

    // Build weak connected components once.  They are only a cheap rejection
    // test; a particular sniper still needs a successful directed A* route.
    int *componentParent = (int *)malloc(m_count * sizeof(int));
    if (!componentParent)
    {
        NAVLOAD_ERR("[insbots] nav load failed: out of memory building components\n");
        Free();
        return false;
    }
    for (int i = 0; i < m_count; i++) componentParent[i] = i;
    for (int i = 0; i < m_count; i++)
    {
        for (int c = 0; c < m_areas[i].connCount; c++)
        {
            int a = i, b = m_areas[i].connIdx[c];
            if (b < 0 || b >= m_count) continue;
            while (componentParent[a] != a) { componentParent[a] = componentParent[componentParent[a]]; a = componentParent[a]; }
            while (componentParent[b] != b) { componentParent[b] = componentParent[componentParent[b]]; b = componentParent[b]; }
            if (a != b) componentParent[b] = a;
        }
    }
    for (int i = 0; i < m_count; i++)
    {
        int componentRoot = i;
        while (componentParent[componentRoot] != componentRoot)
            componentRoot = componentParent[componentRoot];
        m_areas[i].component = componentRoot;
    }
    free(componentParent);

    // ---- Build Global Sniper Candidate Directory ----
    // Keep annotated and generated candidates together.  Objective-specific
    // visibility, cover and route scoring is intentionally deferred and
    // staggered by CBotRoster so NAV loading performs no engine traces.
    int totalHiding = 0;
    for (int i = 0; i < m_count; i++) totalHiding += m_areas[i].hidingSpotCount;
    int tempSniperCap = totalHiding + m_count * 5;
    HidingSpot *tempSnipers = (HidingSpot *)calloc(tempSniperCap, sizeof(HidingSpot));
    if (!tempSnipers)
    {
        NAVLOAD_ERR("[insbots] nav load failed: out of memory building sniper overlook directory\n");
        Free();
        return false;
    }
    int foundSnipers = 0;
    for (int i = 0; i < m_count; i++)
    {
        NavAreaData &a = m_areas[i];
        for (int h = 0; h < a.hidingSpotCount; h++)
        {
            if (!(a.hidingSpots[h].flags & SNIPER_SPOT_EXPOSED) && foundSnipers < tempSniperCap)
                tempSnipers[foundSnipers++] = a.hidingSpots[h];
        }

        HidingSpot sp;
        sp.id = a.id;
        sp.areaIdx = i;
        sp.flags = SNIPER_SPOT_GENERATED;
        sp.pos = a.center;
        if (foundSnipers < tempSniperCap) tempSnipers[foundSnipers++] = sp;

        const float width = a.hi.x - a.lo.x;
        const float depth = a.hi.y - a.lo.y;
        if (width >= 96.0f && depth >= 96.0f)
        {
            const float inset = 24.0f;
            const float xs[4] = { a.lo.x + inset, a.hi.x - inset, a.hi.x - inset, a.lo.x + inset };
            const float ys[4] = { a.lo.y + inset, a.lo.y + inset, a.hi.y - inset, a.hi.y - inset };
            for (int k = 0; k < 4 && foundSnipers < tempSniperCap; k++)
            {
                sp.id = a.id * 5u + (unsigned int)k + 1u;
                sp.pos = Vector(xs[k], ys[k], GetZ(i, xs[k], ys[k]));
                sp.flags = SNIPER_SPOT_GENERATED | SNIPER_SPOT_CORNER;
                tempSnipers[foundSnipers++] = sp;
            }
        }
    }
    if (foundSnipers > 0)
    {
        m_sniperSpots = (HidingSpot *)malloc(foundSnipers * sizeof(HidingSpot));
        if (!m_sniperSpots)
        {
            NAVLOAD_ERR("[insbots] nav load failed: out of memory storing sniper overlook directory\n");
            free(tempSnipers);
            Free();
            return false;
        }
        memcpy(m_sniperSpots, tempSnipers, foundSnipers * sizeof(HidingSpot));
        m_sniperSpotCount = foundSnipers;
    }
    else
    {
        m_sniperSpots = 0; m_sniperSpotCount = 0;
    }
    free(tempSnipers);

    BuildGrid();
    return true;
}
static const float NAV_LOOKUP_TIE_EPS = 0.001f;

bool CNavMesh::CanJumpFrom(int areaIdx) const
{
    const NavAreaData *a = Area(areaIdx);
    return !a || !(a->attributes & NAVATTR_NO_JUMP);
}

int CNavMesh::FindArea(const Vector &pos) const
{
    INSPROF_SCOPE(INSPROF_FINDAREA);
    int best = -1;
    float bestSurfaceZ = -3.4e38f;
    const float testZ = pos.z + 5.0f;

    // Match Source's CNavMesh::GetNavArea semantics: among areas whose exact
    // 2D bounds contain the position, use the highest NAV surface immediately
    // beneath it.  The old lookup expanded every rectangle by 8 units and
    // compared against center.z.  Across a thin wall that let the rectangle on
    // the far side win even though the bot was physically standing on this one.
    // The spatial grid limits this pass to the 3x3 cells around the position
    // instead of scanning every area on the map.
    if (m_gridStart && m_gridW > 0 && m_gridH > 0)
    {
        int cx = (int)((pos.x - m_gridMinX) / m_gridCell);
        int cy = (int)((pos.y - m_gridMinY) / m_gridCell);
        if (cx < 0) cx = 0; else if (cx >= m_gridW) cx = m_gridW - 1;
        if (cy < 0) cy = 0; else if (cy >= m_gridH) cy = m_gridH - 1;
        for (int dy = -1; dy <= 1; dy++)
        {
            const int yy = cy + dy;
            if (yy < 0 || yy >= m_gridH) continue;
            for (int dx = -1; dx <= 1; dx++)
            {
                const int xx = cx + dx;
                if (xx < 0 || xx >= m_gridW) continue;
                const int cell = yy * m_gridW + xx;
                for (int k = m_gridStart[cell]; k < m_gridStart[cell + 1]; k++)
                {
                    const int i = m_gridCells[k];
                    const NavAreaData &a = m_areas[i];
                    if (pos.x < a.lo.x || pos.x > a.hi.x) continue;
                    if (pos.y < a.lo.y || pos.y > a.hi.y) continue;

                    float surfaceZ = GetZ(i, pos.x, pos.y);
                    if (surfaceZ > testZ) continue;          // area is above the bot
                    if (surfaceZ < pos.z - 120.0f) continue; // too far beneath the bot
                    if (surfaceZ > bestSurfaceZ + NAV_LOOKUP_TIE_EPS
                        || (fabsf(surfaceZ - bestSurfaceZ) <= NAV_LOOKUP_TIE_EPS
                            && (best < 0 || i < best)))
                    {
                        bestSurfaceZ = surfaceZ;
                        best = i;
                    }
                }
            }
        }
        if (best >= 0) return best;
    }
    else
    {
        for (int i = 0; i < m_count; i++)
        {
            const NavAreaData &a = m_areas[i];
            if (pos.x < a.lo.x || pos.x > a.hi.x) continue;
            if (pos.y < a.lo.y || pos.y > a.hi.y) continue;

            float surfaceZ = GetZ(i, pos.x, pos.y);
            if (surfaceZ > testZ) continue;          // area is above the bot
            if (surfaceZ < pos.z - 120.0f) continue; // too far beneath the bot
            if (surfaceZ > bestSurfaceZ + NAV_LOOKUP_TIE_EPS
                || (fabsf(surfaceZ - bestSurfaceZ) <= NAV_LOOKUP_TIE_EPS
                    && (best < 0 || i < best)))
            {
                bestSurfaceZ = surfaceZ;
                best = i;
            }
        }
        if (best >= 0) return best;
    }

    // The position is genuinely off the mesh. Find the nearest point ON an
    // area rather than its center. In the live plugin, require a clear
    // player-solid brush trace so an otherwise-near rectangle cannot be chosen
    // through a structural wall or floor. Native NAV tests have no engine trace
    // interface, so they exercise the geometric half of this fallback only.
    //
    // Cost guard: the old code ran a full engine trace for EVERY candidate that
    // beat the current best, so a single off-mesh call on a dense map could
    // issue hundreds of traces (a bad frame peaked near 900 ms on ins_sinjar).
    // We now collect the four nearest tiles geometrically, validate at most
    // those four with traces, and cache the result for the same ~64u spot so
    // stuck retry loops do not re-trace every 0.2s.
    struct Cand { int idx; float ds; };
    Cand cand[4];
    for (int i = 0; i < 4; i++) { cand[i].idx = -1; cand[i].ds = 3.4e38f; }

    if (m_gridStart && m_gridW > 0 && m_gridH > 0)
    {
        int cx = (int)((pos.x - m_gridMinX) / m_gridCell);
        int cy = (int)((pos.y - m_gridMinY) / m_gridCell);
        if (cx < 0) cx = 0; else if (cx >= m_gridW) cx = m_gridW - 1;
        if (cy < 0) cy = 0; else if (cy >= m_gridH) cy = m_gridH - 1;
        // expand ring by ring (Chebyshev distance) so candidates stay local;
        // stop once the ring is farther than the 4th-best candidate found.
        for (int ring = 0; ; ring++)
        {
            int y0 = cy - ring, y1 = cy + ring;
            int x0 = cx - ring, x1 = cx + ring;
            if (y0 < 0) y0 = 0;
            if (y1 >= m_gridH) y1 = m_gridH - 1;
            if (x0 < 0) x0 = 0;
            if (x1 >= m_gridW) x1 = m_gridW - 1;
            bool any = false;
            float minCellD2 = 3.4e38f;
            for (int yy = y0; yy <= y1; yy++)
            {
                for (int xx = x0; xx <= x1; xx++)
                {
                    const int dcx = (xx < cx) ? cx - xx : xx - cx;
                    const int dcy = (yy < cy) ? cy - yy : yy - cy;
                    const int d = (dcx > dcy) ? dcx : dcy;
                    if (d != ring) continue;
                    any = true;

                    const float cellX0 = m_gridMinX + xx * m_gridCell;
                    const float cellY0 = m_gridMinY + yy * m_gridCell;
                    float ex = 0, ey = 0;
                    if (pos.x < cellX0) ex = cellX0 - pos.x;
                    else if (pos.x > cellX0 + m_gridCell) ex = pos.x - (cellX0 + m_gridCell);
                    if (pos.y < cellY0) ey = cellY0 - pos.y;
                    else if (pos.y > cellY0 + m_gridCell) ey = pos.y - (cellY0 + m_gridCell);
                    const float cd = ex * ex + ey * ey;
                    if (cd < minCellD2) minCellD2 = cd;

                    const int cell = yy * m_gridW + xx;
                    for (int k = m_gridStart[cell]; k < m_gridStart[cell + 1]; k++)
                    {
                        const int i = m_gridCells[k];
                        const NavAreaData &a = m_areas[i];
                        float x = pos.x;
                        float y = pos.y;
                        if (x < a.lo.x) x = a.lo.x; else if (x > a.hi.x) x = a.hi.x;
                        if (y < a.lo.y) y = a.lo.y; else if (y > a.hi.y) y = a.hi.y;
                        Vector closest(x, y, GetZ(i, x, y));
                        Vector d2 = closest - pos;
                        float ds = d2.x * d2.x + d2.y * d2.y + d2.z * d2.z * 4.0f;
                        bool betterTail = ds < cand[3].ds - NAV_LOOKUP_TIE_EPS
                            || (fabsf(ds - cand[3].ds) <= NAV_LOOKUP_TIE_EPS
                                && (cand[3].idx < 0 || i < cand[3].idx));
                        if (!betterTail) continue;
                        int j = 3;
                        while (j > 0
                               && (ds < cand[j - 1].ds - NAV_LOOKUP_TIE_EPS
                                   || (fabsf(ds - cand[j - 1].ds) <= NAV_LOOKUP_TIE_EPS
                                       && (cand[j - 1].idx < 0 || i < cand[j - 1].idx))))
                        {
                            cand[j] = cand[j - 1];
                            j--;
                        }
                        cand[j].idx = i;
                        cand[j].ds = ds;
                    }
                }
            }
            if (!any) break;
            if (cand[3].ds < 3.4e37f && minCellD2 > cand[3].ds) break;
        }
    }
    else
    {
        for (int i = 0; i < m_count; i++)
        {
            const NavAreaData &a = m_areas[i];
            float x = pos.x;
            float y = pos.y;
            if (x < a.lo.x) x = a.lo.x; else if (x > a.hi.x) x = a.hi.x;
            if (y < a.lo.y) y = a.lo.y; else if (y > a.hi.y) y = a.hi.y;
            Vector closest(x, y, GetZ(i, x, y));
            Vector d = closest - pos;
            float ds = d.x * d.x + d.y * d.y + d.z * d.z * 4.0f;
            bool betterTail = ds < cand[3].ds - NAV_LOOKUP_TIE_EPS
                || (fabsf(ds - cand[3].ds) <= NAV_LOOKUP_TIE_EPS
                    && (cand[3].idx < 0 || i < cand[3].idx));
            if (!betterTail) continue;
            int j = 3;
            while (j > 0
                   && (ds < cand[j - 1].ds - NAV_LOOKUP_TIE_EPS
                       || (fabsf(ds - cand[j - 1].ds) <= NAV_LOOKUP_TIE_EPS
                           && (cand[j - 1].idx < 0 || i < cand[j - 1].idx))))
            {
                cand[j] = cand[j - 1];
                j--;
            }
            cand[j].idx = i;
            cand[j].ds = ds;
        }
    }

    if (cand[0].idx < 0) return -1;

    // Quantized fallback cache: repeated queries from the same off-mesh spot
    // (stuck-learning retries every 0.2s, several subsystems per frame) reuse
    // the validated answer without running engine traces again. The cached
    // area must still be one of the four nearest candidates and the caller
    // must remain within 8u. Sinjar's generated terrain uses many 25u tiles;
    // the old 40u reuse radius could carry one tile across several ground
    // seams and make path following lag behind the player's real position.
    int key = ((int)(pos.x / 64.0f) * 73856093) ^ ((int)(pos.y / 64.0f) * 19349663)
            ^ ((int)(pos.z / 128.0f) * 83492791);
    FallbackEntry &fe = m_fbCache[key & 63];
    if (fe.key == key && fe.area >= 0)
    {
        float dx = pos.x - fe.pos.x, dy = pos.y - fe.pos.y, dz = pos.z - fe.pos.z;
        bool stillCandidate = false;
        for (int i = 0; i < 4 && cand[i].idx >= 0; i++)
            if (cand[i].idx == fe.area) { stillCandidate = true; break; }
        if (stillCandidate && dx * dx + dy * dy + dz * dz <= 8.0f * 8.0f)
            return fe.area;
    }

    int chosen = -1;
    for (int i = 0; i < 4 && cand[i].idx >= 0; i++)
    {
#ifndef TESTING_NATIVE
        if (enginetrace)
        {
            const NavAreaData &a = m_areas[cand[i].idx];
            float x = pos.x;
            float y = pos.y;
            if (x < a.lo.x) x = a.lo.x; else if (x > a.hi.x) x = a.hi.x;
            if (y < a.lo.y) y = a.lo.y; else if (y > a.hi.y) y = a.hi.y;
            Vector closest(x, y, GetZ(cand[i].idx, x, y));
            Vector from = pos + Vector(0, 0, 36.0f);
            Vector to = closest + Vector(0, 0, 36.0f);
            Ray_t ray;
            trace_t tr;
            CTraceFilterWorldOnly filter;
            ray.Init(from, to);
            enginetrace->TraceRay(ray, MASK_PLAYERSOLID_BRUSHONLY, &filter, &tr);
            if (tr.startsolid || tr.allsolid || tr.fraction < 0.999f)
                continue;
        }
#endif
        chosen = cand[i].idx;
        break;
    }
    // All close tiles were wall-blocked. If the nearest is genuinely close (a
    // plank/doorway edge where the +36u trace is a false negative), still hand
    // the bot that tile rather than returning -1 and forcing a 0.2s retry
    // loop. Far-through-wall picks stay rejected.
    if (chosen < 0 && cand[0].ds <= 150.0f * 150.0f)
        chosen = cand[0].idx;

    if (chosen >= 0)
    {
        fe.key = key;
        fe.pos = pos;
        fe.area = chosen;
    }
    return chosen;
}

// Nearest area to a point regardless of line-of-sight. Used for GOAL
// positions (e.g. objective centers) that sit just off the mesh, so the bot
// routes to the closest tile instead of beelining at the off-mesh point
// through walls.
int CNavMesh::FindNearestArea(const Vector &pos) const
{
    INSPROF_SCOPE(INSPROF_FINDNEAREST);
    int best = -1;
    float bestD = 3.4e38f;
    if (m_gridStart && m_gridW > 0 && m_gridH > 0)
    {
        int cx = (int)((pos.x - m_gridMinX) / m_gridCell);
        int cy = (int)((pos.y - m_gridMinY) / m_gridCell);
        if (cx < 0) cx = 0; else if (cx >= m_gridW) cx = m_gridW - 1;
        if (cy < 0) cy = 0; else if (cy >= m_gridH) cy = m_gridH - 1;
        for (int ring = 0; ; ring++)
        {
            int y0 = cy - ring, y1 = cy + ring;
            int x0 = cx - ring, x1 = cx + ring;
            if (y0 < 0) y0 = 0;
            if (y1 >= m_gridH) y1 = m_gridH - 1;
            if (x0 < 0) x0 = 0;
            if (x1 >= m_gridW) x1 = m_gridW - 1;
            bool any = false;
            float minCellD2 = 3.4e38f;
            for (int yy = y0; yy <= y1; yy++)
            {
                for (int xx = x0; xx <= x1; xx++)
                {
                    const int dcx = (xx < cx) ? cx - xx : xx - cx;
                    const int dcy = (yy < cy) ? cy - yy : yy - cy;
                    const int d = (dcx > dcy) ? dcx : dcy;
                    if (d != ring) continue;
                    any = true;

                    const float cellX0 = m_gridMinX + xx * m_gridCell;
                    const float cellY0 = m_gridMinY + yy * m_gridCell;
                    float ex = 0, ey = 0;
                    if (pos.x < cellX0) ex = cellX0 - pos.x;
                    else if (pos.x > cellX0 + m_gridCell) ex = pos.x - (cellX0 + m_gridCell);
                    if (pos.y < cellY0) ey = cellY0 - pos.y;
                    else if (pos.y > cellY0 + m_gridCell) ey = pos.y - (cellY0 + m_gridCell);
                    const float cd = ex * ex + ey * ey;
                    if (cd < minCellD2) minCellD2 = cd;

                    const int cell = yy * m_gridW + xx;
                    for (int k = m_gridStart[cell]; k < m_gridStart[cell + 1]; k++)
                    {
                        const int i = m_gridCells[k];
                        const NavAreaData &a = m_areas[i];
                        float x = pos.x;
                        float y = pos.y;
                        if (x < a.lo.x) x = a.lo.x; else if (x > a.hi.x) x = a.hi.x;
                        if (y < a.lo.y) y = a.lo.y; else if (y > a.hi.y) y = a.hi.y;
                        Vector closest(x, y, GetZ(i, x, y));
                        Vector d2 = closest - pos;
                        float ds = d2.x * d2.x + d2.y * d2.y + d2.z * d2.z * 4.0f;
                        if (ds < bestD - NAV_LOOKUP_TIE_EPS
                            || (fabsf(ds - bestD) <= NAV_LOOKUP_TIE_EPS
                                && (best < 0 || i < best)))
                        { bestD = ds; best = i; }
                    }
                }
            }
            if (!any) break;
            if (best >= 0 && minCellD2 > bestD) break;
        }
    }
    else
    {
        for (int i = 0; i < m_count; i++)
        {
            const NavAreaData &a = m_areas[i];
            float x = pos.x;
            float y = pos.y;
            if (x < a.lo.x) x = a.lo.x; else if (x > a.hi.x) x = a.hi.x;
            if (y < a.lo.y) y = a.lo.y; else if (y > a.hi.y) y = a.hi.y;
            Vector closest(x, y, GetZ(i, x, y));
            Vector d = closest - pos;
            float ds = d.x * d.x + d.y * d.y + d.z * d.z * 4.0f;
            if (ds < bestD - NAV_LOOKUP_TIE_EPS
                || (fabsf(ds - bestD) <= NAV_LOOKUP_TIE_EPS
                    && (best < 0 || i < best)))
            { bestD = ds; best = i; }
        }
    }
    return best;
}

float CNavMesh::GetZ(int idx, float x, float y) const
{
    if (idx < 0 || idx >= m_count) return 0;
    const NavAreaData &a = m_areas[idx];
    // corners: lo(nw meaning north-west low corner) - use bilinear between
    // z(lo)=lo.z at (lo.x,lo.y), neZ at (hi.x,lo.y), swZ at (lo.x,hi.y), hi.z at (hi.x,hi.y)
    float dx = a.hi.x - a.lo.x, dy = a.hi.y - a.lo.y;
    float u = dx > 1 ? (x - a.lo.x) / dx : 0; if (u < 0) u = 0; if (u > 1) u = 1;
    float v = dy > 1 ? (y - a.lo.y) / dy : 0; if (v < 0) v = 0; if (v > 1) v = 1;
    float n = a.lo.z + (a.neZ - a.lo.z) * u;
    float s = a.swZ + (a.hi.z - a.swZ) * u;
    return n + (s - n) * v;
}

bool CNavMesh::BuildEdgeMetadata(int fromIdx, int connSlot, NavEdgeMeta &out) const
{
    memset(&out, 0, sizeof(out));
    out.kind = NAV_EDGE_MALFORMED;
    out.valid = 0;
    if (fromIdx < 0 || fromIdx >= m_count) return false;
    const NavAreaData &a = m_areas[fromIdx];
    if (connSlot < 0 || connSlot >= a.connCount || !a.connIdx) return false;
    int toIdx = a.connIdx[connSlot];
    if (toIdx < 0 || toIdx >= m_count || toIdx == fromIdx) return false;
    const NavAreaData &b = m_areas[toIdx];

    // Ladder records are vertical transitions, not rectangle-overlap portals.
    // Keep the serialized ladder index in connLadder and expose only its type
    // and source-side geometry here.
    int ladderCode = a.connLadder ? a.connLadder[connSlot] : 0;
    if (ladderCode != 0)
    {
        int ladderIdx = (ladderCode > 0 ? ladderCode : -ladderCode) - 1;
        out.kind = ladderCode > 0 ? NAV_EDGE_LADDER_UP : NAV_EDGE_LADDER_DOWN;
        if (ladderIdx < 0 || ladderIdx >= m_ladderCount) return true;
        const NavLadderData &ladder = m_ladders[ladderIdx];
        const Vector &source = ladderCode > 0 ? ladder.bottom : ladder.top;
        const Vector &target = ladderCode > 0 ? ladder.top : ladder.bottom;
        out.midX = source.x;
        out.midY = source.y;
        out.midZ = GetZ(fromIdx, source.x, source.y);
        out.zDelta = target.z - source.z;
        out.halfSpan = ladder.width * 0.5f;
        if (ladder.dir == NAV_NORTH || ladder.dir == NAV_SOUTH)
        {
            out.axisX = 1.0f;
            out.normalY = ladder.dir == NAV_NORTH ? 1.0f : -1.0f;
        }
        else
        {
            out.axisY = 1.0f;
            out.normalX = ladder.dir == NAV_EAST ? 1.0f : -1.0f;
        }
        out.valid = 1;
        return true;
    }

    float ox0 = a.lo.x > b.lo.x ? a.lo.x : b.lo.x;
    float ox1 = a.hi.x < b.hi.x ? a.hi.x : b.hi.x;
    float oy0 = a.lo.y > b.lo.y ? a.lo.y : b.lo.y;
    float oy1 = a.hi.y < b.hi.y ? a.hi.y : b.hi.y;
    float overlapX = ox1 - ox0;
    float overlapY = oy1 - oy0;
    const float contactEps = 0.0f;
    const float walkGap = 4.0f;
    const float transitionGap = 64.0f;
    const float elevationThreshold = 24.0f;

    // A positive-area rectangle overlap has no unique shared boundary. It is
    // either a vertical transition (stacked floors) or malformed/duplicate
    // geometry, never an ordinary walk portal.
    if (overlapX > contactEps && overlapY > contactEps)
    {
        out.midX = (ox0 + ox1) * 0.5f;
        out.midY = (oy0 + oy1) * 0.5f;
        float sourceZ = GetZ(fromIdx, out.midX, out.midY);
        float targetZ = GetZ(toIdx, out.midX, out.midY);
        out.midZ = sourceZ;
        out.zDelta = targetZ - sourceZ;
        if (overlapX >= overlapY)
        {
            out.axisX = 1.0f;
            out.halfSpan = overlapX * 0.5f;
        }
        else
        {
            out.axisY = 1.0f;
            out.halfSpan = overlapY * 0.5f;
        }
        if (out.zDelta < -elevationThreshold)
        {
            out.kind = NAV_EDGE_DROP;
            out.valid = 1;
        }
        else if (out.zDelta > elevationThreshold)
        {
            out.kind = NAV_EDGE_JUMP;
            out.valid = 1;
        }
        return true;
    }

    // Boundary normal along X: rectangles are separated/touching in X and
    // overlap laterally along Y.
    if (overlapY > contactEps)
    {
        float sourceX, targetX, gap;
        if (a.hi.x <= b.lo.x)
        {
            sourceX = a.hi.x;
            targetX = b.lo.x;
            gap = targetX - sourceX;
        }
        else if (b.hi.x <= a.lo.x)
        {
            sourceX = a.lo.x;
            targetX = b.hi.x;
            gap = sourceX - targetX;
        }
        else
        {
            sourceX = targetX = (ox0 + ox1) * 0.5f;
            gap = 0.0f;
        }
        if (gap > transitionGap) return true;

        out.midY = (oy0 + oy1) * 0.5f;
        out.axisY = 1.0f;
        out.halfSpan = (oy1 - oy0) * 0.5f;
        if (b.center.x > a.center.x) out.normalX = 1.0f;
        else if (b.center.x < a.center.x) out.normalX = -1.0f;
        float sourceZ = GetZ(fromIdx, sourceX, out.midY);
        float targetZ = GetZ(toIdx, targetX, out.midY);
        out.zDelta = targetZ - sourceZ;
        out.midZ = sourceZ;
        if (gap <= walkGap
            && out.zDelta >= -elevationThreshold
            && out.zDelta <= elevationThreshold)
        {
            out.kind = NAV_EDGE_WALK;
            out.midX = (sourceX + targetX) * 0.5f;
            out.midZ = targetZ;
        }
        else if (out.zDelta < -elevationThreshold)
        {
            out.kind = NAV_EDGE_DROP;
            out.midX = sourceX;
        }
        else
        {
            // A horizontal gap needs a jump even when both floors are level;
            // an upward step over the threshold is also a jump.
            out.kind = NAV_EDGE_JUMP;
            out.midX = sourceX;
        }
        out.valid = 1;
        return true;
    }

    // Boundary normal along Y: rectangles are separated/touching in Y and
    // overlap laterally along X.
    if (overlapX > contactEps)
    {
        float sourceY, targetY, gap;
        if (a.hi.y <= b.lo.y)
        {
            sourceY = a.hi.y;
            targetY = b.lo.y;
            gap = targetY - sourceY;
        }
        else if (b.hi.y <= a.lo.y)
        {
            sourceY = a.lo.y;
            targetY = b.hi.y;
            gap = sourceY - targetY;
        }
        else
        {
            sourceY = targetY = (oy0 + oy1) * 0.5f;
            gap = 0.0f;
        }
        if (gap > transitionGap) return true;

        out.midX = (ox0 + ox1) * 0.5f;
        out.axisX = 1.0f;
        out.halfSpan = (ox1 - ox0) * 0.5f;
        if (b.center.y > a.center.y) out.normalY = 1.0f;
        else if (b.center.y < a.center.y) out.normalY = -1.0f;
        float sourceZ = GetZ(fromIdx, out.midX, sourceY);
        float targetZ = GetZ(toIdx, out.midX, targetY);
        out.zDelta = targetZ - sourceZ;
        out.midZ = sourceZ;
        if (gap <= walkGap
            && out.zDelta >= -elevationThreshold
            && out.zDelta <= elevationThreshold)
        {
            out.kind = NAV_EDGE_WALK;
            out.midY = (sourceY + targetY) * 0.5f;
            out.midZ = targetZ;
        }
        else if (out.zDelta < -elevationThreshold)
        {
            out.kind = NAV_EDGE_DROP;
            out.midY = sourceY;
        }
        else
        {
            out.kind = NAV_EDGE_JUMP;
            out.midY = sourceY;
        }
        out.valid = 1;
        return true;
    }

    return true;
}

bool CNavMesh::GetEdgeMeta(int fromIdx, int toIdx, NavEdgeMeta &out) const
{
    memset(&out, 0, sizeof(out));
    out.kind = NAV_EDGE_MALFORMED;
    if (fromIdx < 0 || fromIdx >= m_count) return false;
    const NavAreaData &a = m_areas[fromIdx];
    if (!a.connIdx || !a.connMeta) return false;
    bool found = false;
    NavEdgeMeta first;
    memset(&first, 0, sizeof(first));
    first.kind = NAV_EDGE_MALFORMED;
    for (int k = 0; k < a.connCount; k++)
    {
        if (a.connIdx[k] != toIdx) continue;
        if (!found)
        {
            first = a.connMeta[k];
            found = true;
        }
        if (a.connMeta[k].valid)
        {
            out = a.connMeta[k];
            return true;
        }
    }
    if (found)
    {
        out = first;
        return true;
    }
    return false;
}

bool CNavMesh::GetTransitionGeometry(int fromIdx, int toIdx, Vector &takeoff,
                                     Vector &landing, Vector &axis,
                                     float &halfSpan) const
{
    const NavAreaData *from = Area(fromIdx);
    const NavAreaData *to = Area(toIdx);
    if (!from || !to) return false;
    NavEdgeMeta meta;
    if (!GetEdgeMeta(fromIdx, toIdx, meta) || !meta.valid
        || meta.kind != NAV_EDGE_DROP || meta.zDelta >= -24.0f)
        return false;

    float nx = meta.normalX;
    float ny = meta.normalY;
    float normalLen = sqrtf(nx * nx + ny * ny);
    if (normalLen < 0.5f) return false;
    nx /= normalLen;
    ny /= normalLen;

    float gap = 0.0f;
    float inset = 0.0f;
    landing.x = meta.midX;
    landing.y = meta.midY;
    if (ny < -0.5f)
    {
        gap = from->lo.y - to->hi.y;
        inset = (to->hi.y - to->lo.y) * 0.5f;
        if (inset > 18.0f) inset = 18.0f;
        landing.y = to->hi.y - inset;
    }
    else if (ny > 0.5f)
    {
        gap = to->lo.y - from->hi.y;
        inset = (to->hi.y - to->lo.y) * 0.5f;
        if (inset > 18.0f) inset = 18.0f;
        landing.y = to->lo.y + inset;
    }
    else if (nx < -0.5f)
    {
        gap = from->lo.x - to->hi.x;
        inset = (to->hi.x - to->lo.x) * 0.5f;
        if (inset > 18.0f) inset = 18.0f;
        landing.x = to->hi.x - inset;
    }
    else if (nx > 0.5f)
    {
        gap = to->lo.x - from->hi.x;
        inset = (to->hi.x - to->lo.x) * 0.5f;
        if (inset > 18.0f) inset = 18.0f;
        landing.x = to->lo.x + inset;
    }
    else
        return false;

    // Match the long-standing Haditha repair envelope: a real short gap,
    // narrow enough for one hull, with a meaningful drop.
    if (gap <= 0.0f || gap > 64.0f) return false;
    if (meta.halfSpan < 8.0f || meta.halfSpan >= 36.0f) return false;

    takeoff.x = meta.midX;
    takeoff.y = meta.midY;
    takeoff.z = meta.midZ;
    landing.z = GetZ(toIdx, landing.x, landing.y);
    if (takeoff.z - landing.z < 24.0f) return false;
    axis = Vector(meta.axisX, meta.axisY, 0.0f);
    halfSpan = meta.halfSpan;
    return true;
}

bool CNavMesh::GetTransitionWaypoint(int fromIdx, int toIdx, const Vector &pos,
                                     NavTransitionWaypoint &out) const
{
    memset(&out, 0, sizeof(out));
    if (!GetTransitionGeometry(fromIdx, toIdx, out.takeoff, out.landing,
                               out.axis, out.halfSpan))
        return false;
    NavEdgeMeta meta;
    if (!GetEdgeMeta(fromIdx, toIdx, meta)) return false;
    float nx = meta.normalX;
    float ny = meta.normalY;
    float normalLen = sqrtf(nx * nx + ny * ny);
    if (normalLen < 0.5f) return false;
    nx /= normalLen;
    ny /= normalLen;
    float along = (out.takeoff.x - pos.x) * nx + (out.takeoff.y - pos.y) * ny;
    float lateral = fabsf((pos.x - out.takeoff.x) * -ny
                        + (pos.y - out.takeoff.y) *  nx);
    out.virtualApproach = along > 32.0f || lateral > 8.0f;
    if (out.virtualApproach)
    {
        const NavAreaData *from = Area(fromIdx);
        out.point.x = out.takeoff.x - nx * 28.0f;
        out.point.y = out.takeoff.y - ny * 28.0f;
        if (from)
        {
            if (out.point.x < from->lo.x) out.point.x = from->lo.x;
            else if (out.point.x > from->hi.x) out.point.x = from->hi.x;
            if (out.point.y < from->lo.y) out.point.y = from->lo.y;
            else if (out.point.y > from->hi.y) out.point.y = from->hi.y;
        }
        out.point.z = GetZ(fromIdx, out.point.x, out.point.y);
    }
    else
        out.point = out.landing;
    return true;
}

bool CNavMesh::IsOnAreaLanding(int areaIdx, const Vector &pos, float maxZDelta) const
{
    const NavAreaData *a = Area(areaIdx);
    if (!a) return false;
    if (pos.x < a->lo.x || pos.x > a->hi.x
        || pos.y < a->lo.y || pos.y > a->hi.y)
        return false;
    float floorZ = GetZ(areaIdx, pos.x, pos.y);
    return fabsf(floorZ - pos.z) <= maxZDelta;
}

bool CNavMesh::CrossedEdgeIntoArea(int fromIdx, int toIdx, const Vector &pos) const
{
    const NavAreaData *from = Area(fromIdx);
    const NavAreaData *to = Area(toIdx);
    if (!from || !to) return false;
    NavEdgeMeta meta;
    if (!GetEdgeMeta(fromIdx, toIdx, meta) || !meta.valid) return false;

    if (meta.kind == NAV_EDGE_DROP || meta.kind == NAV_EDGE_JUMP)
        return IsOnAreaLanding(toIdx, pos, 8.0f);
    if (meta.kind != NAV_EDGE_WALK) return false;

    if (pos.x < to->lo.x - 24.0f || pos.x > to->hi.x + 24.0f
        || pos.y < to->lo.y - 24.0f || pos.y > to->hi.y + 24.0f)
        return false;
    float floorZ = GetZ(toIdx, pos.x, pos.y);
    if (fabsf(floorZ - pos.z) > 64.0f) return false;

    float nx = meta.normalX;
    float ny = meta.normalY;
    float normalLen = sqrtf(nx * nx + ny * ny);
    if (normalLen < 0.5f)
    {
        nx = meta.axisY;
        ny = -meta.axisX;
        normalLen = sqrtf(nx * nx + ny * ny);
    }
    if (normalLen < 0.5f) return false;
    nx /= normalLen;
    ny /= normalLen;
    float side = (pos.x - meta.midX) * nx + (pos.y - meta.midY) * ny;
    return side > 1.0f;
}

int CNavMesh::RandomArea() const
{
    if (!m_count) return -1;
    return rand() % m_count;
}

// ---- bad-connection memory (persistent, per-connection, shared by all bots) --
// Stored as confidence on the connection itself and shared by the whole squad.
// One report is tentative, confirmed links alter A*, and successful traversal
// rehabilitates them. A link remains usable if it is genuinely the only route.
void CNavMesh::MarkBadConnection(int fromIdx, int toIdx)
{
    if (fromIdx < 0 || fromIdx >= m_count) return;
    NavAreaData &a = m_areas[fromIdx];
    if (!a.connBad) return;
    for (int k = 0; k < a.connCount; k++)
        if (a.connIdx[k] == toIdx)
        {
            if (a.connBad[k] < 255)
            {
                a.connBad[k]++;
                InvalidateCostCache();
            }
            return;
        }
}

void CNavMesh::MarkGoodConnection(int fromIdx, int toIdx)
{
    if (fromIdx < 0 || fromIdx >= m_count) return;
    NavAreaData &a = m_areas[fromIdx];
    if (!a.connBad) return;
    for (int k = 0; k < a.connCount; k++)
        if (a.connIdx[k] == toIdx)
        {
            if (a.connBad[k] > 0)
            {
                a.connBad[k]--;
                InvalidateCostCache();
            }
            return;
        }
}

bool CNavMesh::HasConnection(int fromIdx, int toIdx) const
{
    if (fromIdx < 0 || fromIdx >= m_count) return false;
    const NavAreaData &a = m_areas[fromIdx];
    if (!a.connIdx) return false;
    for (int k = 0; k < a.connCount; k++)
        if (a.connIdx[k] == toIdx) return true;
    return false;
}

int CNavMesh::BadCount(int fromIdx, int toIdx) const
{
    if (fromIdx < 0 || fromIdx >= m_count) return 0;
    const NavAreaData &a = m_areas[fromIdx];
    if (!a.connBad) return 0;
    for (int k = 0; k < a.connCount; k++)
        if (a.connIdx[k] == toIdx) return a.connBad[k];
    return 0;
}

// ---- learned-bad-link persistence ------------------------------------------
// Text file, one line per bad directed link: "<fromAreaId> <toAreaId> <count>".
// Human-readable on purpose so server admins can inspect or hand-edit it.
bool CNavMesh::SaveBadLinks(const char *path) const
{
    if (!path || !path[0] || !m_areas || m_count <= 0) return false;
    // Always truncate/write the file, even when no confirmed links remain. This
    // replaces legacy or rehabilitated data instead of letting stale penalties
    // reappear on the next map load.
    FILE *fp = fopen(path, "w");
    if (!fp) return false;
    fprintf(fp, "# insbots learned bad nav links v2 (fromAreaId toAreaId confidence)\n");
    for (int i = 0; i < m_count; i++)
    {
        const NavAreaData &a = m_areas[i];
        // The loader stores at most 512 resolved links per area. Treat anything
        // outside that invariant as damaged state instead of walking past either
        // allocation during the timed save.
        if (!a.connBad || !a.connIdx || a.connCount <= 0 || a.connCount > 512) continue;
        for (int k = 0; k < a.connCount; k++)
        {
            // One report is tentative runtime evidence. Persist only a link
            // confirmed by at least two independent stuck events.
            if (a.connBad[k] < 2) continue;
            int toIdx = a.connIdx[k];
            if (toIdx < 0 || toIdx >= m_count) continue;
            fprintf(fp, "%u %u %d\n", a.id, m_areas[toIdx].id, (int)a.connBad[k]);
        }
    }
    fclose(fp);
    return true;
}

int CNavMesh::LoadBadLinks(const char *path)
{
    if (!m_count) return 0;
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char line[256];
    // v1 accumulated temporary congestion and repeated reports from the broken
    // stuck-stage loop. Do not import those poisoned scores into the repaired
    // confidence model.
    if (!fgets(line, sizeof(line), fp) || !strstr(line, "bad nav links v2"))
    {
        fclose(fp);
        return 0;
    }
    int applied = 0;
    while (fgets(line, sizeof(line), fp))
    {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned int fromId = 0, toId = 0; int cnt = 0;
        if (sscanf(line, "%u %u %d", &fromId, &toId, &cnt) != 3) continue;
        if (cnt <= 0) continue;
        int fi = IndexOfId(fromId), ti = IndexOfId(toId);
        if (fi < 0 || ti < 0) continue;
        NavAreaData &a = m_areas[fi];
        if (!a.connBad) continue;
        for (int k = 0; k < a.connCount; k++)
            if (a.connIdx[k] == ti)
            {
                int v = (int)a.connBad[k] + cnt;
                a.connBad[k] = (unsigned char)(v > 255 ? 255 : v);
                applied++;
                break;
            }
    }
    fclose(fp);
    if (applied > 0) InvalidateCostCache();
    return applied;
}

void CNavMesh::ClearBadLinks()
{
    InvalidateCostCache();
    for (int i = 0; i < m_count; i++)
        if (m_areas[i].connBad)
            memset(m_areas[i].connBad, 0, m_areas[i].connCount);
}

bool CNavMesh::GetPortal(int fromIdx, int toIdx, Vector &out) const
{
    const NavAreaData *a = Area(fromIdx);
    if (!a || !a->connMeta) return false;
    for (int k = 0; k < a->connCount; k++)
    {
        if (a->connIdx[k] != toIdx) continue;
        const NavEdgeMeta &meta = a->connMeta[k];
        if (!meta.valid || meta.kind != NAV_EDGE_WALK) continue;
        out.x = meta.midX;
        out.y = meta.midY;
        out.z = meta.midZ;
        return true;
    }
    return false;
}

bool CNavMesh::GetPortalMerged(int fromIdx, int toIdx, Vector &mid, Vector &axis,
                               float &halfSpan) const
{
    const NavAreaData *a = Area(fromIdx);
    if (!a || !a->connMeta) return false;

    const NavEdgeMeta *base = 0;
    for (int k = 0; k < a->connCount; k++)
    {
        if (a->connIdx[k] != toIdx) continue;
        const NavEdgeMeta &meta = a->connMeta[k];
        if (meta.valid && meta.kind == NAV_EDGE_WALK)
        {
            base = &meta;
            break;
        }
    }
    if (!base) return false;

    // A portal slice is an interval along its tangent axis. Merge only
    // collinear, contiguous walk slices on the same boundary plane and with
    // compatible elevation deltas. Non-walk and malformed records never
    // participate, even when their 2D rectangles happen to overlap.
    const float axisX = base->axisX;
    const float axisY = base->axisY;
    const bool xNormal = axisY > 0.5f;
    const float plane = xNormal ? base->midX : base->midY;
    float u0 = (xNormal ? base->midY : base->midX) - base->halfSpan;
    float u1 = (xNormal ? base->midY : base->midX) + base->halfSpan;
    const float mergeGap = 4.0f;
    const float planeTolerance = 1.0f;
    const float elevationTolerance = 8.0f;

    for (int k = 0; k < a->connCount; k++)
    {
        int otherIdx = a->connIdx[k];
        if (otherIdx < 0 || otherIdx >= m_count || otherIdx == toIdx) continue;
        const NavEdgeMeta &meta = a->connMeta[k];
        if (!meta.valid || meta.kind != NAV_EDGE_WALK) continue;
        if (fabsf(meta.axisX - axisX) > 0.001f
            || fabsf(meta.axisY - axisY) > 0.001f)
            continue;
        float otherPlane = xNormal ? meta.midX : meta.midY;
        if (fabsf(otherPlane - plane) > planeTolerance) continue;
        if (fabsf(meta.zDelta - base->zDelta) > elevationTolerance) continue;
        float cu0 = (xNormal ? meta.midY : meta.midX) - meta.halfSpan;
        float cu1 = (xNormal ? meta.midY : meta.midX) + meta.halfSpan;
        if (cu0 > u1 + mergeGap || cu1 < u0 - mergeGap) continue;
        if (cu0 < u0) u0 = cu0;
        if (cu1 > u1) u1 = cu1;
    }

    if (xNormal)
    {
        mid.x = plane;
        mid.y = (u0 + u1) * 0.5f;
        axis = Vector(0.0f, 1.0f, 0.0f);
    }
    else
    {
        mid.x = (u0 + u1) * 0.5f;
        mid.y = plane;
        axis = Vector(1.0f, 0.0f, 0.0f);
    }
    halfSpan = (u1 - u0) * 0.5f;
    mid.z = GetZ(toIdx, mid.x, mid.y);
    return true;
}

bool CNavMesh::GetLadderConnection(int fromIdx, int toIdx, int &ladderIdx, bool &up) const
{
    ladderIdx = -1; up = false;
    if (fromIdx < 0 || fromIdx >= m_count) return false;
    const NavAreaData &a = m_areas[fromIdx];
    if (!a.connMeta) return false;
    for (int k = 0; k < a.connCount; k++)
    {
        if (a.connIdx[k] != toIdx) continue;
        const NavEdgeMeta &meta = a.connMeta[k];
        if (!meta.valid
            || (meta.kind != NAV_EDGE_LADDER_UP && meta.kind != NAV_EDGE_LADDER_DOWN))
            continue;
        if (!a.connLadder || a.connLadder[k] == 0) continue;
        int code = a.connLadder[k];
        up = meta.kind == NAV_EDGE_LADDER_UP;
        ladderIdx = (code > 0 ? code : -code) - 1;
        return ladderIdx >= 0 && ladderIdx < m_ladderCount;
    }
    return false;
}

int CNavMesh::BuildPath(int startIdx, int goalIdx, int *path, int maxLen, int seed, bool *truncated)
{
    INSPROF_SCOPE(INSPROF_BUILDPATH);
    if (startIdx < 0 || goalIdx < 0 || startIdx >= m_count || goalIdx >= m_count) return 0;
    if (!path || maxLen <= 0) return 0;
    if (startIdx == goalIdx) { path[0] = startIdx; return 1; }
    if (m_areas[startIdx].component != m_areas[goalIdx].component) return 0;

    RefreshCostParamsFromGlobals();
    m_mark++;
    const Vector &goalPos = m_areas[goalIdx].center;

    // binary min-heap open list keyed by f, with lazy deletion (improved nodes
    // are re-pushed; stale duplicates are skipped on pop). Scales to the large
    // generated meshes (tens of thousands of areas) where a linear scan choked.
        static int *open = 0; static int openCap = 0;
    if (m_count < 0 || (size_t)m_count > ((size_t)0x7FFFFFFF / 2 - 16)) return 0;
    int need = m_count * 2 + 16;
    if (openCap < need)
    {
        if (!CheckedRealloc((void **)&open, (size_t)need * sizeof(int))) return 0;
        openCap = need;
    }
    int openCount = 0;

    NavAreaData *A = m_areas;
    A[startIdx].g = 0;
    Vector d0 = goalPos - A[startIdx].center;
    A[startIdx].f = d0.Length();
    A[startIdx].parent = -1;
    A[startIdx].openMark = m_mark;
    open[openCount++] = startIdx;

    int guard = 0;
    while (openCount > 0 && guard++ < 2000000)
    {
        // heap extract-min (pop root, move last up, sift down)
        int cur = open[0];
        open[0] = open[--openCount];
        {
            int pi = 0;
            for (;;)
            {
                int l = pi * 2 + 1, r = l + 1, sm = pi;
                if (l < openCount && A[open[l]].f < A[open[sm]].f) sm = l;
                if (r < openCount && A[open[r]].f < A[open[sm]].f) sm = r;
                if (sm == pi) break;
                int t = open[sm]; open[sm] = open[pi]; open[pi] = t; pi = sm;
            }
        }
        // lazy deletion: a stale duplicate for an already-expanded node
        if (A[cur].closedMark == m_mark) continue;

        if (cur == goalIdx)
        {
            // Two-pass reconstruction: count the whole parent chain first so a
            // route longer than the caller buffer is returned as a true prefix
            // (start -> ... -> prefixEnd) with *truncated set, letting the
            // caller continue pathfinding from the prefix end next frame.
            int totalNodes = 0;
            for (int c = cur; c != -1; c = A[c].parent)
            {
                if (++totalNodes > m_count) return 0;   // corrupt parent chain
            }
            int emitCount = totalNodes < maxLen ? totalNodes : maxLen;
            if (truncated) *truncated = (totalNodes > maxLen);
            int skip = totalNodes - emitCount;
            int c = cur;
            for (int i = 0; i < skip; i++) c = A[c].parent;
            for (int i = 0; i < emitCount; i++)
            {
                path[i] = c;
                c = A[c].parent;
            }
            for (int i = 0; i < emitCount / 2; i++)
            {
                int t = path[i];
                path[i] = path[emitCount - 1 - i];
                path[emitCount - 1 - i] = t;
            }
            return emitCount;
        }
        A[cur].closedMark = m_mark;

        for (int k = 0; k < A[cur].connCount; k++)
        {
            int nb = A[cur].connIdx[k];
            if (A[nb].closedMark == m_mark) continue;
            Vector step = A[nb].center - A[cur].center;
            float cost = step.Length();

            // ---- ladder-aware A* logic ----
            // Typed ladder edges are NOT costed like walk edges: the actual
            // climb distance is the ladder record length (never shorter than
            // the vertical span between the two area centres, so a short or
            // stale record cannot make a ladder path artificially cheap), an
            // avoidance penalty keeps flat routes preferred when one exists,
            // and climbing UP costs extra because it is the slow/risky
            // direction. Descents are the "free" direction (no big-drop
            // penalty - it is a controlled ladder descent, not a fall).
            int ladderIdx = -1; bool ladderUp = false;
            bool ladderEdge = GetLadderConnection(cur, nb, ladderIdx, ladderUp);
            if (ladderEdge)
            {
                const NavLadderData *ladder = Ladder(ladderIdx);
                float climbLen = ladder ? ladder->length : 0.0f;
                float minClimb = fabsf(step.z);
                if (climbLen < minClimb) climbLen = minClimb;
                cost = climbLen + m_costParams.ladderPenalty;
                if (ladderUp) cost += m_costParams.ladderUpPenalty;
            }
            else
            {
                // discourage big drops (falling off roofs)
                if (step.z < -120.0f) cost += 400.0f;
            }
            // AVOID jump/crouch areas: large additive penalties so bots route
            // around anything needing a jump or a crouch when a flat path exists,
            // but still use them if it's the only way. Tunable via the globals.
            if (A[nb].attributes & NAVATTR_CROUCH) cost += m_costParams.crouchPenalty;
            if (A[nb].attributes & NAVATTR_JUMP)   cost += m_costParams.jumpPenalty;
            if (A[nb].attributes & NAVATTR_AVOID)  cost += m_costParams.avoidPenalty;
            // Dynamic firefight avoidance for capture-committed bots: areas
            // inside an active engagement pay a large additive cost so A*
            // prefers a flat detour around the fight. z distance is weighted
            // so a firefight on another floor does not block travel above or
            // below it. Zones are rebuilt every frame by the roster.
            if (m_costParams.dangerEnabled && m_costParams.dangerPenalty > 0.0f && s_dangerCount > 0)
            {
                const Vector &p = A[nb].center;
                for (int z = 0; z < s_dangerCount; z++)
                {
                    float dx = p.x - s_dangerPos[z].x;
                    float dy = p.y - s_dangerPos[z].y;
                    float dz = p.z - s_dangerPos[z].z;
                    float r  = s_dangerRadius[z];
                    if (dx*dx + dy*dy + dz*dz*4.0f < r*r)
                    {
                        cost += m_costParams.dangerPenalty;
                        break;
                    }
                }
            }
            // Confidence model: one report is tentative and costs nothing; two
            // reports strongly discourage the edge; three or more effectively
            // hard-block it whenever any alternative exists. It remains a soft
            // cost so an isolated objective can still be reached/rescued when
            // this is literally the graph's only connection.
            unsigned char bad = A[cur].connBad ? A[cur].connBad[k] : 0;
            if (bad >= 2)
            {
                int confidence = (int)bad - 1;
                if (confidence > 8) confidence = 8; // no unbounded penalty arms race
                cost += (float)confidence * 3500.0f;
                if (bad >= 3) cost += 500000.0f;
            }
            // per-bot jitter: diversifies routes so bots don't all take the same
            // shortest path (surfaces bad links faster; spreads the bots out)
            if (seed)
            {
                unsigned int h = (unsigned int)cur * 2654435761u
                               + (unsigned int)nb * 40503u
                               + (unsigned int)seed * 2246822519u;
                cost += (float)(h & 0x3F);   // 0..63 units of noise
            }
            float g = A[cur].g + cost;
            if (A[nb].openMark == m_mark && g >= A[nb].g) continue;   // no improvement
            A[nb].g = g;
            Vector h = goalPos - A[nb].center;
            A[nb].f = g + h.Length();
            A[nb].parent = cur;
            A[nb].openMark = m_mark;
            // heap push nb (re-push on improvement = lazy decrease-key). Grow if
            // duplicates ever exceed the cap.
                        if (openCount >= openCap)
            {
                int newCap = openCap * 2 + 16;
                if (newCap <= openCap || !CheckedRealloc((void **)&open, (size_t)newCap * sizeof(int)))
                    return 0;
                openCap = newCap;
            }
            int ci = openCount++;
            open[ci] = nb;
            while (ci > 0)
            {
                int par = (ci - 1) >> 1;
                if (A[open[par]].f <= A[open[ci]].f) break;
                int t = open[par]; open[par] = open[ci]; open[ci] = t; ci = par;
            }
        }
    }
    return 0;
}

float CNavMesh::GetPathCost(int startIdx, int goalIdx, int seed)
{
    INSPROF_SCOPE(INSPROF_GETPATHCOST);
    if (startIdx < 0 || goalIdx < 0 || startIdx >= m_count || goalIdx >= m_count)
        return -1.0f;
    if (startIdx == goalIdx) return 0.0f;

    RefreshCostParamsFromGlobals();
    if (m_areas[startIdx].component != m_areas[goalIdx].component)
        return -1.0f;

    // Deterministic (seed 0, no danger zones) queries are cached: capture
    // refill and objective picking re-ask the same pairs dozens of times per
    // second, and each miss on a 25k-area mesh is a full A*. Costs only change
    // when learned links change or the mesh reloads, both of which bump the
    // stamp. Per-bot jitter (seed) and danger routing are never cached.
    if (seed == 0 && !m_costParams.dangerEnabled)
    {
        for (int i = 0; i < 256; i++)
        {
            if (m_costCache[i].stamp == m_costStamp
                && m_costCache[i].from == startIdx
                && m_costCache[i].to == goalIdx)
                {
                m_costCache[i].seq = ++m_costSeq;
                return m_costCache[i].cost;
            }
        }
    }

    m_mark++;
    const Vector &goalPos = m_areas[goalIdx].center;

    // Same binary min-heap open list as BuildPath (lazy deletion; stale
    // duplicates are skipped on pop).
        static int *open = 0; static int openCap = 0;
    if (m_count < 0 || (size_t)m_count > ((size_t)0x7FFFFFFF / 2 - 16)) return -1.0f;
    int need = m_count * 2 + 16;
    if (openCap < need)
    {
        if (!CheckedRealloc((void **)&open, (size_t)need * sizeof(int))) return -1.0f;
        openCap = need;
    }
    int openCount = 0;

    NavAreaData *A = m_areas;
    A[startIdx].g = 0;
    Vector d0 = goalPos - A[startIdx].center;
    A[startIdx].f = d0.Length();
    A[startIdx].parent = -1;
    A[startIdx].openMark = m_mark;
    open[openCount++] = startIdx;

    int guard = 0;
    while (openCount > 0 && guard++ < 2000000)
    {
        int cur = open[0];
        open[0] = open[--openCount];
        {
            int pi = 0;
            for (;;)
            {
                int l = pi * 2 + 1, r = l + 1, sm = pi;
                if (l < openCount && A[open[l]].f < A[open[sm]].f) sm = l;
                if (r < openCount && A[open[r]].f < A[open[sm]].f) sm = r;
                if (sm == pi) break;
                int t = open[sm]; open[sm] = open[pi]; open[pi] = t; pi = sm;
            }
        }
        if (A[cur].closedMark == m_mark) continue;
        if (cur == goalIdx)
        {
            float result = A[cur].g;
            if (seed == 0 && !m_costParams.dangerEnabled)
            {
                int slot = -1;
                unsigned int oldestSeq = 0xFFFFFFFFu;
                for (int i = 0; i < 256; i++)
                {
                    if (m_costCache[i].stamp != m_costStamp)
                    {
                        slot = i;
                        break;
                    }
                    if (m_costCache[i].seq < oldestSeq)
                    {
                        oldestSeq = m_costCache[i].seq;
                        slot = i;
                    }
                }
                if (slot < 0) slot = 0;
                m_costCache[slot].from = startIdx;
                m_costCache[slot].to = goalIdx;
                m_costCache[slot].cost = result;
                m_costCache[slot].stamp = m_costStamp;
        m_costCache[slot].seq = ++m_costSeq;
            }
            return result;
        }
        A[cur].closedMark = m_mark;

        for (int k = 0; k < A[cur].connCount; k++)
        {
            int nb = A[cur].connIdx[k];
            if (A[nb].closedMark == m_mark) continue;
            Vector step = A[nb].center - A[cur].center;
            float cost = step.Length();

            int ladderIdx = -1; bool ladderUp = false;
            bool ladderEdge = GetLadderConnection(cur, nb, ladderIdx, ladderUp);
            if (ladderEdge)
            {
                const NavLadderData *ladder = Ladder(ladderIdx);
                float climbLen = ladder ? ladder->length : 0.0f;
                float minClimb = fabsf(step.z);
                if (climbLen < minClimb) climbLen = minClimb;
                cost = climbLen + m_costParams.ladderPenalty;
                if (ladderUp) cost += m_costParams.ladderUpPenalty;
            }
            else
            {
                if (step.z < -120.0f) cost += 400.0f;
            }
            if (A[nb].attributes & NAVATTR_CROUCH) cost += m_costParams.crouchPenalty;
            if (A[nb].attributes & NAVATTR_JUMP)   cost += m_costParams.jumpPenalty;
            if (A[nb].attributes & NAVATTR_AVOID)  cost += m_costParams.avoidPenalty;
            if (m_costParams.dangerEnabled && m_costParams.dangerPenalty > 0.0f && s_dangerCount > 0)
            {
                const Vector &p = A[nb].center;
                for (int z = 0; z < s_dangerCount; z++)
                {
                    float dx = p.x - s_dangerPos[z].x;
                    float dy = p.y - s_dangerPos[z].y;
                    float dz = p.z - s_dangerPos[z].z;
                    float r  = s_dangerRadius[z];
                    if (dx*dx + dy*dy + dz*dz*4.0f < r*r)
                    {
                        cost += m_costParams.dangerPenalty;
                        break;
                    }
                }
            }
            unsigned char bad = A[cur].connBad ? A[cur].connBad[k] : 0;
            if (bad >= 2)
            {
                int confidence = (int)bad - 1;
                if (confidence > 8) confidence = 8;
                cost += (float)confidence * 3500.0f;
                if (bad >= 3) cost += 500000.0f;
            }
            if (seed)
            {
                unsigned int h = (unsigned int)cur * 2654435761u
                               + (unsigned int)nb * 40503u
                               + (unsigned int)seed * 2246822519u;
                cost += (float)(h & 0x3F);
            }
            float g = A[cur].g + cost;
            if (A[nb].openMark == m_mark && g >= A[nb].g) continue;
            A[nb].g = g;
            Vector h = goalPos - A[nb].center;
            A[nb].f = g + h.Length();
            A[nb].parent = cur;
            A[nb].openMark = m_mark;
                        if (openCount >= openCap)
            {
                int newCap = openCap * 2 + 16;
                if (newCap <= openCap || !CheckedRealloc((void **)&open, (size_t)newCap * sizeof(int)))
                    return -1.0f;
                openCap = newCap;
            }
            int ci = openCount++;
            open[ci] = nb;
            while (ci > 0)
            {
                int par = (ci - 1) >> 1;
                if (A[open[par]].f <= A[open[ci]].f) break;
                int t = open[par]; open[par] = open[ci]; open[ci] = t; ci = par;
            }
        }
    }
    if (seed == 0 && !m_costParams.dangerEnabled)
    {
        int slot = -1;
        unsigned int oldestSeq = 0xFFFFFFFFu;
        for (int i = 0; i < 256; i++)
        {
            if (m_costCache[i].stamp != m_costStamp)
            {
                slot = i;
                break;
            }
            if (m_costCache[i].seq < oldestSeq)
            {
                oldestSeq = m_costCache[i].seq;
                slot = i;
            }
        }
        if (slot < 0) slot = 0;
        m_costCache[slot].from = startIdx;
        m_costCache[slot].to = goalIdx;
        m_costCache[slot].cost = -1.0f;
        m_costCache[slot].stamp = m_costStamp;
        m_costCache[slot].seq = ++m_costSeq;
    }
    return -1.0f;
}

int CNavMesh::FindClosestReachableArea(int startIdx, const Vector &desired)
{
    if (startIdx < 0 || startIdx >= m_count || !m_areas || m_count <= 0)
        return -1;

    // Reusable BFS queue grown to at least m_count entries. Use the same
    // checked-allocation pattern as BuildPath so OOM fails closed.
    static int *frontier = 0;
    static int frontierCap = 0;
    if (frontierCap < m_count)
    {
        if (!CheckedRealloc((void **)&frontier, (size_t)m_count * sizeof(int)))
            return -1;
        frontierCap = m_count;
    }

    m_mark++;
    const unsigned int stamp = m_mark;
    NavAreaData *A = m_areas;
    int head = 0, tail = 0;
    A[startIdx].closedMark = stamp;
    frontier[tail++] = startIdx;

    int best = startIdx;
    float bestDs = 0.0f;
    {
        const NavAreaData &a = A[startIdx];
        float x = desired.x;
        float y = desired.y;
        if (x < a.lo.x) x = a.lo.x; else if (x > a.hi.x) x = a.hi.x;
        if (y < a.lo.y) y = a.lo.y; else if (y > a.hi.y) y = a.hi.y;
        Vector closest(x, y, GetZ(startIdx, x, y));
        Vector d = closest - desired;
        bestDs = d.x * d.x + d.y * d.y + d.z * d.z * 4.0f;
    }

    while (head < tail)
    {
        int cur = frontier[head++];
        NavAreaData &a = A[cur];
        {
            float x = desired.x;
            float y = desired.y;
            if (x < a.lo.x) x = a.lo.x; else if (x > a.hi.x) x = a.hi.x;
            if (y < a.lo.y) y = a.lo.y; else if (y > a.hi.y) y = a.hi.y;
            Vector closest(x, y, GetZ(cur, x, y));
            Vector d = closest - desired;
            float ds = d.x * d.x + d.y * d.y + d.z * d.z * 4.0f;
            if (ds < bestDs || (ds == bestDs && cur < best))
            {
                bestDs = ds;
                best = cur;
            }
        }
        for (int k = 0; k < a.connCount; k++)
        {
            int nb = a.connIdx[k];
            if (nb < 0 || nb >= m_count) continue;
            if (A[nb].closedMark == stamp) continue;
            A[nb].closedMark = stamp;
            frontier[tail++] = nb;
        }
    }
    return best;
}
// Phase 1: Query nearby tactical sniper overlooks within maxDist of origin
int CNavMesh::GetSniperSpots(const Vector &origin, float maxDist, HidingSpot *outSpots, int maxOut) const
{
    if (!m_sniperSpots || m_sniperSpotCount <= 0 || maxOut <= 0 || !outSpots) return 0;
    float maxDistSq = maxDist * maxDist;
    int found = 0;
    for (int i = 0; i < m_sniperSpotCount && found < maxOut; i++)
    {
        float dx = m_sniperSpots[i].pos.x - origin.x;
        float dy = m_sniperSpots[i].pos.y - origin.y;
        float dz = m_sniperSpots[i].pos.z - origin.z;
        if (dx * dx + dy * dy + dz * dz <= maxDistSq)
        {
            outSpots[found++] = m_sniperSpots[i];
        }
    }
    return found;
}
