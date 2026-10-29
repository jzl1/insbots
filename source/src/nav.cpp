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
extern CGlobalVars *gpGlobals;
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
float g_navNarrowPenalty   = 50.0f;   // soft penalty for walk portals narrower than 100u

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
    next.narrowPenalty = g_navNarrowPenalty;
    next.dangerPenalty = g_navDangerPenalty;
    next.dangerEnabled = g_navDangerEnabled;

    if (next.jumpPenalty != m_costParams.jumpPenalty
        || next.crouchPenalty != m_costParams.crouchPenalty
        || next.avoidPenalty != m_costParams.avoidPenalty
        || next.ladderPenalty != m_costParams.ladderPenalty
        || next.ladderUpPenalty != m_costParams.ladderUpPenalty
        || next.narrowPenalty != m_costParams.narrowPenalty
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

static void FreeNavSearchContext(NavSearchContext &ctx)
{
    if (ctx.nodes) free(ctx.nodes);
    if (ctx.heap) free(ctx.heap);
    if (ctx.frontier) free(ctx.frontier);
    ctx.nodes = 0; ctx.nodeCapacity = 0;
    ctx.heap = 0; ctx.heapCapacity = 0;
    ctx.frontier = 0; ctx.frontierCapacity = 0;
    ctx.mark = 0;
}

static bool EnsureSearchNodes(NavSearchContext &ctx, int count)
{
    if (ctx.nodeCapacity >= count) return true;
    int capacity = ctx.nodeCapacity ? ctx.nodeCapacity : 256;
    while (capacity < count)
    {
        if (capacity > 0x3FFFFFFF) { capacity = count; break; }
        capacity *= 2;
    }
    NavSearchNode *nodes = (NavSearchNode *)realloc(
        ctx.nodes, (size_t)capacity * sizeof(NavSearchNode));
    if (!nodes) return false;
    if (capacity > ctx.nodeCapacity)
        memset(nodes + ctx.nodeCapacity, 0,
               (size_t)(capacity - ctx.nodeCapacity) * sizeof(NavSearchNode));
    ctx.nodes = nodes;
    ctx.nodeCapacity = capacity;
    return true;
}

static bool EnsureSearchHeap(NavSearchContext &ctx, int count)
{
    if (ctx.heapCapacity >= count) return true;
    int capacity = ctx.heapCapacity ? ctx.heapCapacity : 256;
    while (capacity < count)
    {
        if (capacity > 0x3FFFFFFF) { capacity = count; break; }
        capacity *= 2;
    }
    int *heap = (int *)realloc(ctx.heap, (size_t)capacity * sizeof(int));
    if (!heap) return false;
    ctx.heap = heap;
    ctx.heapCapacity = capacity;
    return true;
}

static bool EnsureSearchFrontier(NavSearchContext &ctx, int count)
{
    if (ctx.frontierCapacity >= count) return true;
    int capacity = ctx.frontierCapacity ? ctx.frontierCapacity : 256;
    while (capacity < count)
    {
        if (capacity > 0x3FFFFFFF) { capacity = count; break; }
        capacity *= 2;
    }
    int *frontier = (int *)realloc(ctx.frontier, (size_t)capacity * sizeof(int));
    if (!frontier) return false;
    ctx.frontier = frontier;
    ctx.frontierCapacity = capacity;
    return true;
}

static void AdvanceSearchMark(NavSearchContext &ctx)
{
    ctx.mark++;
    if (ctx.mark == 0)
    {
        if (ctx.nodes)
            memset(ctx.nodes, 0, (size_t)ctx.nodeCapacity * sizeof(NavSearchNode));
        ctx.mark = 1;
    }
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
void CNavMesh::ResetPhysicalPortalCache()
{
    for (int i = 0; i < 1024; i++)
    {
        m_physicalPortalCache[i].mapHash = 0;
        m_physicalPortalCache[i].fromIdx = -1;
        m_physicalPortalCache[i].toIdx = -1;
        m_physicalPortalCache[i].sampleTime = 0.0f;
        m_physicalPortalCache[i].stateSignature = 0;
        m_physicalPortalCache[i].valid = false;
        m_physicalPortalCache[i].tight = false;
        m_physicalPortalCache[i].halfSpan = 0.0f;
    }
}

CNavMesh::PhysicalPortalEntry &CNavMesh::ResolvePhysicalPortalCacheEntry(
    unsigned int key, int fromIdx, int toIdx) const
{
    // Four-way set associativity retains several unrelated edges that share
    // a hash bucket. The direct-mapped cache repeatedly evicted alternating
    // path edges, forcing full hull sweeps even while their geometry was
    // unchanged. Keep the same 1024-entry memory budget and use the oldest
    // sample as the deterministic replacement victim.
    int base = (int)(key & 1023u) & ~3;
    int oldest = base;
    for (int way = 0; way < 4; way++)
    {
        PhysicalPortalEntry &entry = m_physicalPortalCache[base + way];
        if (entry.valid && entry.mapHash == m_mapHash
            && entry.fromIdx == fromIdx && entry.toIdx == toIdx)
            return entry;
        if (!entry.valid || entry.mapHash != m_mapHash)
            return entry;
        if (entry.sampleTime < m_physicalPortalCache[oldest].sampleTime)
            oldest = base + way;
    }
    return m_physicalPortalCache[oldest];
}

void CNavMesh::Free()
{
    InvalidateCostCache();
    ResetPhysicalPortalCache();
    m_mapHash = 0;
    FreeNavSearchContext(m_searchContext);
    m_searchBusy = false;
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
    if (m_sccReach) free(m_sccReach);
    if (m_fixups) free(m_fixups);
    if (m_sniperSpots) free(m_sniperSpots);
    if (m_ladders) free(m_ladders);
    if (m_gridStart) free(m_gridStart);
    if (m_gridCells) free(m_gridCells);
    m_ladders = 0; m_ladderCount = 0;
    m_connIdx = 0; m_connBad = 0; m_connLadder = 0; m_connMeta = 0;
    m_connCount = 0;
    m_areaIdTable = 0; m_areaIdTableSize = 0;
    m_sccCount = 0; m_sccReachWords = 0; m_sccReach = 0;
    m_fixups = 0; m_fixupCount = 0;
    m_sniperSpots = 0; m_sniperSpotCount = 0;
    m_gridStart = 0; m_gridCells = 0;
    m_gridW = m_gridH = 0;
    m_areas = 0; m_count = 0;
    m_lastSearchStatus = NAV_SEARCH_OK;
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

void CNavMesh::ClearFixups()
{
    if (m_fixups) free(m_fixups);
    m_fixups = 0;
    m_fixupCount = 0;
}

bool CNavMesh::LoadFixups(const char *path)
{
    ClearFixups();
    if (!path || !path[0] || !m_areas || !m_areaIdTable) return false;
    FILE *fp = fopen(path, "r");
    if (!fp) return false;

    NavFixupRecord records[128];
    int count = 0;
    bool header = false;
    char line[512];
    bool valid = true;
    while (fgets(line, sizeof(line), fp))
    {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\r' || *p == '\n') continue;
        if (*p == '#')
        {
            if (strstr(p, "insbots nav fixups v1")) header = true;
            continue;
        }
        if (!header || count >= (int)(sizeof(records) / sizeof(records[0])))
        {
            valid = false;
            break;
        }

        char kindName[32] = {0};
        unsigned int fromId = 0, toId = 0;
        int kind = -1;
        if (sscanf(p, "edge %31s %u %u", kindName, &fromId, &toId) == 3)
        {
            if (!strcmp(kindName, "tight")) kind = NAV_FIXUP_TIGHT_EDGE;
            else if (!strcmp(kindName, "east")) continue; // legacy no-op
            else valid = false;
        }
        else if (sscanf(p, "area avoid %u", &fromId) == 1)
        {
            kind = NAV_FIXUP_AVOID_AREA;
            toId = 0;
        }
        else if (sscanf(p, "transition %u %u", &fromId, &toId) == 2)
        {
            kind = NAV_FIXUP_SEPARATED_TRANSITION;
        }
        else
        {
            valid = false;
        }
        if (!valid) break;
        if (IndexOfId(fromId) < 0
            || (kind != NAV_FIXUP_AVOID_AREA && IndexOfId(toId) < 0))
        {
            valid = false;
            break;
        }

        NavFixupRecord &rec = records[count];
        memset(&rec, 0, sizeof(rec));
        rec.kind = kind;
        rec.fromId = fromId;
        rec.toId = toId;
        if (kind == NAV_FIXUP_SEPARATED_TRANSITION)
        {
            char *takeoff = strstr(p, "takeoff");
            if (takeoff)
            {
                if (sscanf(takeoff, "takeoff %f %f %f",
                           &rec.takeoff.x, &rec.takeoff.y, &rec.takeoff.z) != 3)
                { valid = false; break; }
                rec.hasTakeoff = true;
            }
            char *landing = strstr(p, "landing");
            if (landing)
            {
                if (sscanf(landing, "landing %f %f %f",
                           &rec.landing.x, &rec.landing.y, &rec.landing.z) != 3)
                { valid = false; break; }
                rec.hasLanding = true;
            }
        }
        count++;
    }
    fclose(fp);
    if (!valid || !header || count == 0)
    {
        NAVLOAD_ERR("[insbots] nav fixups ignored: malformed %s\n", path);
        return false;
    }

    m_fixups = (NavFixupRecord *)malloc((size_t)count * sizeof(NavFixupRecord));
    if (!m_fixups)
    {
        NAVLOAD_ERR("[insbots] nav fixups ignored: allocation failed for %s\n", path);
        return false;
    }
    memcpy(m_fixups, records, (size_t)count * sizeof(NavFixupRecord));
    m_fixupCount = count;
    for (int i = 0; i < m_fixupCount; i++)
    {
        if (m_fixups[i].kind != NAV_FIXUP_AVOID_AREA) continue;
        int idx = IndexOfId(m_fixups[i].fromId);
        if (idx >= 0) m_areas[idx].attributes |= NAVATTR_AVOID;
    }
    return true;
}

bool CNavMesh::HasFixupKind(int kind) const
{
    for (int i = 0; i < m_fixupCount; i++)
        if (m_fixups[i].kind == kind) return true;
    return false;
}

bool CNavMesh::HasFixupEdge(int kind, int fromIdx, int toIdx) const
{
    const NavAreaData *from = Area(fromIdx);
    const NavAreaData *to = Area(toIdx);
    if (!from || !to) return false;
    for (int i = 0; i < m_fixupCount; i++)
    {
        const NavFixupRecord &rec = m_fixups[i];
        if (rec.kind == kind && rec.fromId == from->id && rec.toId == to->id)
            return true;
    }
    return false;
}

bool CNavMesh::GetFixupTransition(int fromIdx, int toIdx,
                                  Vector &takeoff, Vector &landing) const
{
    const NavAreaData *from = Area(fromIdx);
    const NavAreaData *to = Area(toIdx);
    if (!from || !to) return false;
    for (int i = 0; i < m_fixupCount; i++)
    {
        const NavFixupRecord &rec = m_fixups[i];
        if (rec.kind != NAV_FIXUP_SEPARATED_TRANSITION
            || rec.fromId != from->id || rec.toId != to->id)
            continue;
        if (rec.hasTakeoff && rec.hasLanding)
        {
            takeoff = rec.takeoff;
            landing = rec.landing;
            return true;
        }
        return false;
    }
    return false;
}

// uniform spatial index (512u cells) over area 2D bounds. FindArea /
// FindNearestArea are called per bot per frame; on sinjar (~25k areas) the
// old full scan was the main lag source on large maps. An area is inserted
// into every cell its rectangle overlaps, so point lookups only touch local
// cells.
void CNavMesh::BuildGrid()
{
#ifndef NAV_GRID_CELL_SIZE
#define NAV_GRID_CELL_SIZE 256.0f
#endif
    const float CELL = NAV_GRID_CELL_SIZE;
    if (m_gridStart) { free(m_gridStart); m_gridStart = 0; }
    if (m_gridCells) { free(m_gridCells); m_gridCells = 0; }
    m_gridW = m_gridH = 0;
    m_gridCell = CELL;

    if (!m_areas || m_count <= 0)
    {
        NAVLOAD_ERR("[insbots] spatial grid disabled: no nav areas\n");
        return;
    }

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
    if (!counts)
    {
        NAVLOAD_ERR("[insbots] spatial grid disabled: count allocation failed\n");
        return;
    }

    m_gridStart = (int *)malloc((cellCount + 1) * sizeof(int));
    if (!m_gridStart)
    {
        free(counts);
        NAVLOAD_ERR("[insbots] spatial grid disabled: offset allocation failed\n");
        return;
    }

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
            {
                if (counts[cy * w + cx] == 0x7FFFFFFF)
                {
                    free(counts); free(m_gridStart);
                    m_gridStart = 0; m_gridW = m_gridH = 0;
                    NAVLOAD_ERR("[insbots] spatial grid disabled: cell count overflow\n");
                    return;
                }
                counts[cy * w + cx]++;
            }
    }

    int total = 0;
    for (int c = 0; c < cellCount; c++)
    {
        if (counts[c] > 0x7FFFFFFF - total)
        {
            free(counts); free(m_gridStart);
            m_gridStart = 0; m_gridW = m_gridH = 0;
            NAVLOAD_ERR("[insbots] spatial grid disabled: total count overflow\n");
            return;
        }
        m_gridStart[c] = total;
        total += counts[c];
    }
    m_gridStart[cellCount] = total;

    if (total > 0x7FFFFFFF / (int)sizeof(int))
    {
        free(counts); free(m_gridStart);
        m_gridStart = 0; m_gridW = m_gridH = 0;
        NAVLOAD_ERR("[insbots] spatial grid disabled: cell storage overflow\n");
        return;
    }
    m_gridCells = (int *)malloc((size_t)total * sizeof(int));
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
        NAVLOAD_ERR("[insbots] spatial grid disabled: cell allocation failed\n");
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
    CNavMesh candidate;
    if (!candidate.LoadInPlace(path)) return false;
    if (path && strlen(path) + 8 < 1024)
    {
        char fixupPath[1024];
        sprintf(fixupPath, "%s.fixups", path);
        candidate.LoadFixups(fixupPath); // optional; failure means generic behavior
    }
    Free();
    TakeState(candidate);
    return true;
}

void CNavMesh::TakeState(CNavMesh &source)
{
    m_areas = source.m_areas;
    m_count = source.m_count;
    m_ladders = source.m_ladders;
    m_ladderCount = source.m_ladderCount;
    m_sniperSpots = source.m_sniperSpots;
    m_sniperSpotCount = source.m_sniperSpotCount;
    m_connIdx = source.m_connIdx;
    m_connBad = source.m_connBad;
    m_connLadder = source.m_connLadder;
    m_connMeta = source.m_connMeta;
    m_connCount = source.m_connCount;
    m_areaIdTable = source.m_areaIdTable;
    m_areaIdTableSize = source.m_areaIdTableSize;
    m_sccCount = source.m_sccCount;
    m_sccReachWords = source.m_sccReachWords;
    m_sccReach = source.m_sccReach;
    m_fixups = source.m_fixups;
    m_fixupCount = source.m_fixupCount;
    m_gridW = source.m_gridW;
    m_gridH = source.m_gridH;
    m_gridMinX = source.m_gridMinX;
    m_gridMinY = source.m_gridMinY;
    m_gridCell = source.m_gridCell;
    m_gridStart = source.m_gridStart;
    m_gridCells = source.m_gridCells;
    m_mapHash = source.m_mapHash;
    // Free() invalidated the old cache before this transfer. Keep that fresh
    // stamp: copying the candidate's zero stamp could make entries from the
    // previous mesh look valid again.
    m_costSeq = 0;
    m_costParams = source.m_costParams;
    m_lastSearchStatus = source.m_lastSearchStatus;

    source.m_areas = 0;
    source.m_count = 0;
    source.m_ladders = 0;
    source.m_ladderCount = 0;
    source.m_sniperSpots = 0;
    source.m_sniperSpotCount = 0;
    source.m_connIdx = 0;
    source.m_connBad = 0;
    source.m_connLadder = 0;
    source.m_connMeta = 0;
    source.m_connCount = 0;
    source.m_areaIdTable = 0;
    source.m_areaIdTableSize = 0;
    source.m_sccCount = 0;
    source.m_sccReachWords = 0;
    source.m_sccReach = 0;
    source.m_fixups = 0;
    source.m_fixupCount = 0;
    source.m_gridW = source.m_gridH = 0;
    source.m_gridMinX = source.m_gridMinY = 0.0f;
    source.m_gridCell = 0.0f;
    source.m_gridStart = 0;
    source.m_gridCells = 0;
    source.m_mapHash = 0;
    source.m_costStamp = 0;
    source.m_costSeq = 0;
}

bool CNavMesh::LoadInPlace(const char *path)
{
    Free();

    unsigned int mapHash = 2166136261u;
    for (const char *p = path; p && *p; p++)
        mapHash = (mapHash ^ (unsigned char)*p) * 16777619u;
    m_mapHash = mapHash;

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

    // Cache merged walk-opening geometry once. EdgeCost runs inside A* and
    // must not rescan every connection of an area to discover that a doorway
    // was cut into several collinear nav slices.
    for (int i = 0; i < m_count; i++)
    {
        for (int c = 0; c < m_areas[i].connCount; c++)
        {
            NavEdgeMeta &meta = m_areas[i].connMeta[c];
            if (!meta.valid || meta.kind != NAV_EDGE_WALK) continue;
            Vector mid, axis;
            float halfSpan = 0.0f;
            if (GetPortalMerged(i, m_areas[i].connIdx[c], mid, axis, halfSpan))
            {
                meta.costMidX = mid.x;
                meta.costMidY = mid.y;
                meta.costMidZ = mid.z;
                meta.costHalfSpan = halfSpan;
                meta.costLength = (mid - m_areas[i].center).Length()
                                  + (m_areas[m_areas[i].connIdx[c]].center - mid).Length();
            }
            else
            {
                meta.costMidX = meta.midX;
                meta.costMidY = meta.midY;
                meta.costMidZ = meta.midZ;
                meta.costHalfSpan = meta.halfSpan;
                Vector portal(meta.costMidX, meta.costMidY, meta.costMidZ);
                meta.costLength = (portal - m_areas[i].center).Length()
                                  + (m_areas[m_areas[i].connIdx[c]].center - portal).Length();
            }
        }
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

    // Strongly connected components: equal SCC ids mean a directed route
    // exists in both directions, so the common same-component lookup can skip
    // the full forward BFS entirely.
    int *sccIndex = (int *)malloc((size_t)m_count * sizeof(int));
    int *sccVisited = (int *)calloc((size_t)m_count, sizeof(int));
    int *dfsOrder = (int *)malloc((size_t)m_count * sizeof(int));
    int *dfsStack = (int *)malloc((size_t)m_count * sizeof(int));
    int *dfsEdge = (int *)malloc((size_t)m_count * sizeof(int));
    if (!sccIndex || !sccVisited || !dfsOrder || !dfsStack || !dfsEdge)
    {
        free(sccIndex); free(sccVisited); free(dfsOrder); free(dfsStack); free(dfsEdge);
        NAVLOAD_ERR("[insbots] nav load failed: out of memory building strong components\n");
        Free();
        return false;
    }

    int orderCount = 0;
    for (int root = 0; root < m_count; root++)
    {
        if (sccVisited[root]) continue;
        int depth = 0;
        dfsStack[0] = root;
        dfsEdge[0] = 0;
        sccVisited[root] = 1;
        while (depth >= 0)
        {
            const int node = dfsStack[depth];
            int &edge = dfsEdge[depth];
            if (edge < m_areas[node].connCount)
            {
                const int nb = m_areas[node].connIdx[edge++];
                if (nb >= 0 && nb < m_count && !sccVisited[nb])
                {
                    sccVisited[nb] = 1;
                    dfsStack[++depth] = nb;
                    dfsEdge[depth] = 0;
                }
                continue;
            }
            dfsOrder[orderCount++] = node;
            depth--;
        }
    }

    int *revCount = (int *)calloc((size_t)m_count, sizeof(int));
    int *revOffset = (int *)malloc((size_t)(m_count + 1) * sizeof(int));
    int *revEdges = (int *)malloc((size_t)(m_connCount > 0 ? m_connCount : 1) * sizeof(int));
    int *revCursor = (int *)malloc((size_t)m_count * sizeof(int));
    int *sccStack = (int *)malloc((size_t)m_count * sizeof(int));
    if (!revCount || !revOffset || !revEdges || !revCursor || !sccStack)
    {
        free(sccIndex); free(sccVisited); free(dfsOrder); free(dfsStack); free(dfsEdge);
        free(revCount); free(revOffset); free(revEdges); free(revCursor); free(sccStack);
        NAVLOAD_ERR("[insbots] nav load failed: out of memory building reverse edges\n");
        Free();
        return false;
    }
    for (int i = 0; i < m_count; i++)
    {
        for (int c = 0; c < m_areas[i].connCount; c++)
        {
            const int nb = m_areas[i].connIdx[c];
            if (nb >= 0 && nb < m_count) revCount[nb]++;
        }
    }
    revOffset[0] = 0;
    for (int i = 0; i < m_count; i++) revOffset[i + 1] = revOffset[i] + revCount[i];
    memcpy(revCursor, revOffset, (size_t)m_count * sizeof(int));
    for (int i = 0; i < m_count; i++)
    {
        for (int c = 0; c < m_areas[i].connCount; c++)
        {
            const int nb = m_areas[i].connIdx[c];
            if (nb >= 0 && nb < m_count) revEdges[revCursor[nb]++] = i;
        }
    }
    for (int i = 0; i < m_count; i++) sccIndex[i] = -1;

    int sccCount = 0;
    for (int oi = m_count - 1; oi >= 0; oi--)
    {
        const int root = dfsOrder[oi];
        if (sccIndex[root] >= 0) continue;
        int depth = 0;
        sccStack[depth++] = root;
        sccIndex[root] = sccCount;
        while (depth > 0)
        {
            const int node = sccStack[--depth];
            for (int e = revOffset[node]; e < revOffset[node + 1]; e++)
            {
                const int src = revEdges[e];
                if (sccIndex[src] >= 0) continue;
                sccIndex[src] = sccCount;
                sccStack[depth++] = src;
            }
        }
        sccCount++;
    }
    for (int i = 0; i < m_count; i++) m_areas[i].strongComponent = sccIndex[i];

    m_sccCount = sccCount;
    m_sccReachWords = 0;
    m_sccReach = 0;
    if (sccCount <= 512)
    {
        m_sccReachWords = (sccCount + 31) / 32;
        m_sccReach = (unsigned int *)calloc(
            (size_t)sccCount * (size_t)m_sccReachWords, sizeof(unsigned int));
        unsigned char *sccAdj = (unsigned char *)calloc(
            (size_t)sccCount * (size_t)sccCount, sizeof(unsigned char));
        int *sccStack2 = (int *)malloc((size_t)sccCount * sizeof(int));
        unsigned char *sccSeen = (unsigned char *)calloc((size_t)sccCount, sizeof(unsigned char));
        if (!m_sccReach || !sccAdj || !sccStack2 || !sccSeen)
        {
            free(m_sccReach); m_sccReach = 0;
            free(sccAdj); free(sccStack2); free(sccSeen);
            free(sccIndex); free(sccVisited); free(dfsOrder); free(dfsStack); free(dfsEdge);
            free(revCount); free(revOffset); free(revEdges); free(revCursor); free(sccStack);
            NAVLOAD_ERR("[insbots] nav load failed: out of memory building SCC reachability\n");
            Free();
            return false;
        }
        for (int i = 0; i < m_count; i++)
        {
            const int from = m_areas[i].strongComponent;
            for (int c = 0; c < m_areas[i].connCount; c++)
            {
                const int nb = m_areas[i].connIdx[c];
                if (nb < 0 || nb >= m_count) continue;
                const int to = m_areas[nb].strongComponent;
                if (from != to) sccAdj[from * sccCount + to] = 1;
            }
        }
        for (int from = 0; from < sccCount; from++)
        {
            memset(sccSeen, 0, (size_t)sccCount);
            int depth = 0;
            sccStack2[depth++] = from;
            sccSeen[from] = 1;
            while (depth > 0)
            {
                const int node = sccStack2[--depth];
                m_sccReach[from * m_sccReachWords + (node >> 5)]
                    |= (1u << (node & 31));
                for (int to = 0; to < sccCount; to++)
                {
                    if (sccAdj[node * sccCount + to] && !sccSeen[to])
                    {
                        sccSeen[to] = 1;
                        sccStack2[depth++] = to;
                    }
                }
            }
        }
        free(sccAdj); free(sccStack2); free(sccSeen);
    }

    free(sccIndex); free(sccVisited); free(dfsOrder); free(dfsStack); free(dfsEdge);
    free(revCount); free(revOffset); free(revEdges); free(revCursor); free(sccStack);

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

#ifndef TESTING_NATIVE
// Resolve a lateral scan line and facing direction even when the two NAV
// rectangles do not overlap. Malformed generator links commonly bridge a
// stairwell or a short missing strip: the shared-edge midpoint does not exist,
// but the facing sides of the two rectangles still describe where the physical
// opening must be. The hull sweep below still has to prove the actual band.
static bool NavPhysicalPortalCandidate(const CNavMesh &mesh, int fromIdx,
                                       int toIdx, Vector &portal, Vector &axis)
{
    const NavAreaData *from = mesh.Area(fromIdx);
    const NavAreaData *to = mesh.Area(toIdx);
    if (!from || !to) return false;

    Vector merged;
    Vector mergedAxis;
    float mergedHalf = 0.0f;
    bool haveMerged = mesh.GetPortalMerged(fromIdx, toIdx, merged, mergedAxis,
                                           mergedHalf);
    if (haveMerged)
    {
        portal = merged;
        axis = mergedAxis;
        return true;
    }

    const float ox0 = from->lo.x > to->lo.x ? from->lo.x : to->lo.x;
    const float ox1 = from->hi.x < to->hi.x ? from->hi.x : to->hi.x;
    const float oy0 = from->lo.y > to->lo.y ? from->lo.y : to->lo.y;
    const float oy1 = from->hi.y < to->hi.y ? from->hi.y : to->hi.y;

    if (ox1 > ox0 && (from->hi.y <= to->lo.y || to->hi.y <= from->lo.y))
    {
        axis = Vector(1.0f, 0.0f, 0.0f);
        portal.x = (ox0 + ox1) * 0.5f;
        portal.y = from->hi.y <= to->lo.y ? from->hi.y : to->lo.y;
        portal.z = from->hi.y <= to->lo.y
                 ? mesh.GetZ(fromIdx, portal.x, portal.y)
                 : mesh.GetZ(toIdx, portal.x, portal.y);
        return true;
    }
    if (oy1 > oy0 && (from->hi.x <= to->lo.x || to->hi.x <= from->lo.x))
    {
        axis = Vector(0.0f, 1.0f, 0.0f);
        portal.y = (oy0 + oy1) * 0.5f;
        portal.x = from->hi.x <= to->lo.x ? from->hi.x : to->lo.x;
        portal.z = from->hi.x <= to->lo.x
                 ? mesh.GetZ(fromIdx, portal.x, portal.y)
                 : mesh.GetZ(toIdx, portal.x, portal.y);
        return true;
    }
    return false;
}

// Vertical standing-hull probe for a separated malformed edge. A horizontal
// sweep at the source floor is wrong for a stairwell: each step is a valid
// standing surface, but the hull would collide with the rising treads. Try a
// range of start heights so a low ceiling above a landing does not make the
// whole sample read as startsolid.
static bool NavPhysicalPortalStandingHull(float x, float y, float zTop,
                                          float zBot, float hullHeight,
                                          float &floorZ)
{
    if (!enginetrace) return false;
    Vector mins(-16.0f, -16.0f, 0.0f);
    Vector maxs( 16.0f,  16.0f, hullHeight);
    CTraceFilterWorldAndPropsOnly filter;
    Ray_t ray;
    trace_t tr;
    for (float startZ = zTop; startZ >= zBot + 8.0f; startZ -= 16.0f)
    {
        ray.Init(Vector(x, y, startZ), Vector(x, y, zBot), mins, maxs);
        enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &tr);
        if (!tr.startsolid && !tr.allsolid && tr.fraction < 1.0f)
        {
            floorZ = tr.endpos.z;
            return true;
        }
    }
    return false;
}

// Cheap hull trace used as a dynamic-state token for the cached probe. A
// closed door, a moving prop, or a destroyed breakable changes the hit entity,
// fraction, or end position and therefore invalidates the cached result.
static unsigned int NavPhysicalPortalStateSignature(const CNavMesh &mesh,
                                                    int fromIdx, int toIdx)
{
    if (!enginetrace) return 0;
    const NavAreaData *from = mesh.Area(fromIdx);
    const NavAreaData *to = mesh.Area(toIdx);
    if (!from || !to) return 0;
    Vector portal, axis;
    if (!NavPhysicalPortalCandidate(mesh, fromIdx, toIdx, portal, axis))
        return 0;
    Vector travel = to->center - from->center;
    float len = sqrtf(travel.x * travel.x + travel.y * travel.y);
    if (len < 0.5f) return 0;
    Vector normal(travel.x / len, travel.y / len, 0.0f);
    Vector start = portal - normal * 56.0f;
    Vector end = portal + normal * 56.0f;
    start.z = mesh.GetZ(fromIdx, start.x, start.y) + 8.0f;
    end.z = mesh.GetZ(toIdx, end.x, end.y) + 8.0f;

    CTraceFilterWorldAndPropsOnly filter;
    Ray_t ray;
    trace_t tr;
    ray.Init(start, end, Vector(-16.0f, -16.0f, 0.0f),
             Vector(16.0f, 16.0f, 72.0f));
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &tr);

    unsigned int hash = 2166136261u;
    hash = (hash ^ (unsigned int)(size_t)tr.m_pEnt) * 16777619u;
    hash = (hash ^ (unsigned int)(int)(tr.fraction * 100000.0f)) * 16777619u;
    hash = (hash ^ (unsigned int)(int)(tr.endpos.x * 16.0f)) * 16777619u;
    hash = (hash ^ (unsigned int)(int)(tr.endpos.y * 16.0f)) * 16777619u;
    hash = (hash ^ (unsigned int)(int)(tr.endpos.z * 16.0f)) * 16777619u;
    return hash;
}
#endif

bool CNavMesh::GetPhysicalPortal(int fromIdx, int toIdx, Vector &center,
                                 float &halfSpan) const
{
    Vector axis;
    return GetPhysicalPortal(fromIdx, toIdx, center, axis, halfSpan);
}

bool CNavMesh::GetPhysicalPortal(int fromIdx, int toIdx, Vector &center,
                                 Vector &axis, float &halfSpan) const
{
    const NavAreaData *from = Area(fromIdx);
    const NavAreaData *to = Area(toIdx);
    if (!from || !to) return false;

#ifdef TESTING_NATIVE
    Vector mid;
    float mergedHalf = 0.0f;
    if (GetPortalMerged(fromIdx, toIdx, mid, axis, mergedHalf))
    {
        center = mid;
        halfSpan = mergedHalf;
        return mergedHalf > 0.0f && mergedHalf < 40.0f;
    }
    return false;
#else
    if (!enginetrace)
    {
        Vector mid;
        float mergedHalf = 0.0f;
        if (!GetPortalMerged(fromIdx, toIdx, mid, axis, mergedHalf))
            return false;
        center = mid;
        halfSpan = mergedHalf;
        return mergedHalf > 0.0f && mergedHalf < 40.0f;
    }

    unsigned int key = m_mapHash ^ ((unsigned int)fromIdx * 73856093u)
                                 ^ ((unsigned int)toIdx * 19349663u);
    PhysicalPortalEntry &cached = ResolvePhysicalPortalCacheEntry(key, fromIdx, toIdx);
    float now = gpGlobals ? gpGlobals->curtime : 0.0f;
    const unsigned int stateSignature =
        NavPhysicalPortalStateSignature(*this, fromIdx, toIdx);
    if (cached.valid && cached.mapHash == m_mapHash
        && cached.fromIdx == fromIdx && cached.toIdx == toIdx
        && now >= cached.sampleTime && now - cached.sampleTime <= 30.0f
        && cached.stateSignature == stateSignature)
    {
        if (cached.tight)
        {
            center = cached.center;
            axis = cached.axis;
            halfSpan = cached.halfSpan;
        }
        return cached.tight;
    }

    cached.mapHash = m_mapHash;
    cached.fromIdx = fromIdx;
    cached.toIdx = toIdx;
    cached.sampleTime = now;
    cached.stateSignature = stateSignature;
    cached.valid = true;
    cached.tight = false;
    cached.halfSpan = 0.0f;

    Vector portal;
    if (!NavPhysicalPortalCandidate(*this, fromIdx, toIdx, portal, axis))
        return false;
    float axisLen = sqrtf(axis.x * axis.x + axis.y * axis.y);
    if (axisLen < 0.5f) return false;
    axis.x /= axisLen; axis.y /= axisLen; axis.z = 0.0f;

    NavEdgeMeta edgeMeta;
    const bool typedEdge = GetEdgeMeta(fromIdx, toIdx, edgeMeta)
                        && edgeMeta.valid;

    CTraceFilterWorldAndPropsOnly filter;
    Ray_t ray; trace_t tr;
    Vector waist = portal + Vector(0, 0, 44.0f);
    Vector upper = portal + Vector(0, 0, 68.0f);
    bool sideWall[2] = { false, false };
    for (int s = 0; s < 2; s++)
    {
        float sign = s ? -1.0f : 1.0f;
        Vector waistEnd = waist + axis * (sign * 128.0f);
        ray.Init(waist, waistEnd);
        enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &tr);
        float waistHit = (!tr.startsolid && !tr.allsolid)
                       ? tr.fraction * 128.0f : 9999.0f;
        Vector upperEnd = upper + axis * (sign * 128.0f);
        ray.Init(upper, upperEnd);
        enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &tr);
        float upperHit = (!tr.startsolid && !tr.allsolid)
                       ? tr.fraction * 128.0f : 9999.0f;
        sideWall[s] = waistHit < 127.9f && upperHit < 127.9f
                   && fabsf(waistHit - upperHit) <= 32.0f;
    }
    if (!sideWall[0] && !sideWall[1]) return false;

    Vector normal(axis.y, -axis.x, 0.0f);
    Vector travel = to->center - from->center;
    if (travel.x * normal.x + travel.y * normal.y < 0.0f)
    {
        normal.x = -normal.x;
        normal.y = -normal.y;
    }

    const int sampleCount = 25;
    const float sampleStep = 8.0f;
    const float sampleMin = -96.0f;
    bool clear[sampleCount];
    float sampleZ[sampleCount];
    for (int i = 0; i < sampleCount; i++) sampleZ[i] = portal.z;
    Vector mins(-16.0f, -16.0f, 0.0f);
    Vector maxs( 16.0f,  16.0f, 72.0f);
    for (int i = 0; i < sampleCount; i++)
    {
        float off = sampleMin + i * sampleStep;
        Vector candidate = portal + axis * off;
        if (!typedEdge)
        {
            const float standTop = portal.z + 112.0f;
            const float standBottom = portal.z - 308.0f;
            clear[i] = NavPhysicalPortalStandingHull(
                candidate.x, candidate.y, standTop, standBottom,
                72.0f, sampleZ[i]);
            continue;
        }
        Vector start = candidate - normal * 24.0f;
        Vector end = candidate + normal * 24.0f;
        start.z = GetZ(fromIdx, start.x, start.y) + 8.0f;
        end.z = typedEdge ? GetZ(toIdx, end.x, end.y) + 8.0f
                          : GetZ(fromIdx, end.x, end.y) + 8.0f;
        ray.Init(start, end, mins, maxs);
        enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &tr);
        clear[i] = !tr.startsolid && !tr.allsolid && tr.fraction >= 0.999f;
        sampleZ[i] = tr.endpos.z;
    }

    int bestFirst = -1, bestLast = -1;
    float bestScore = 3.4e38f;
    for (int i = 0; i < sampleCount; )
    {
        if (!clear[i]) { i++; continue; }
        int first = i;
        while (i + 1 < sampleCount && clear[i + 1])
        {
            if (!typedEdge && fabsf(sampleZ[i + 1] - sampleZ[i]) > 12.0f)
                break;
            i++;
        }
        int last = i++;
        if (first == 0 || last == sampleCount - 1) continue;
        float firstRun = sampleMin + first * sampleStep;
        float lastRun = sampleMin + last * sampleStep;
        float runCenter = (firstRun + lastRun) * 0.5f;
        float runSpan = lastRun - firstRun;
        float score = fabsf(runCenter) - runSpan * 0.10f;
        if (score < bestScore)
        {
            bestScore = score;
            bestFirst = first;
            bestLast = last;
        }
    }
    if (bestFirst < 0) return false;

    float firstRun = sampleMin + bestFirst * sampleStep;
    float lastRun = sampleMin + bestLast * sampleStep;
    float centerOff = (firstRun + lastRun) * 0.5f;
    float centerSpan = lastRun - firstRun;
    if (centerSpan >= 96.0f) return false;

    cached.tight = true;
    Vector physicalCenter = portal + axis * centerOff;
    physicalCenter.z = typedEdge
                     ? TraceFloorZ(toIdx, physicalCenter.x, physicalCenter.y,
                                   portal.z)
                     : sampleZ[(bestFirst + bestLast) / 2];
    cached.center = physicalCenter;
    cached.axis = axis;
    cached.halfSpan = (centerSpan + 32.0f) * 0.5f;
    center = physicalCenter;
    halfSpan = cached.halfSpan;
    return true;
#endif
}

bool CNavMesh::GetPhysicalStep(int fromIdx, int toIdx, float &stepHeight) const
{
    stepHeight = 0.0f;
    const NavAreaData *from = Area(fromIdx);
    const NavAreaData *to = Area(toIdx);
    if (!from || !to) return false;
    NavEdgeMeta meta;
    if (!GetEdgeMeta(fromIdx, toIdx, meta) || !meta.valid
        || (meta.kind != NAV_EDGE_WALK && meta.kind != NAV_EDGE_JUMP))
        return false;

    Vector seam;
    Vector axis;
    float halfSpan = 0.0f;
    const bool havePhysical = GetPhysicalPortal(fromIdx, toIdx, seam, axis,
                                                halfSpan);
    if (!havePhysical)
    {
        // A JUMP edge without a physical opening is a real gap. Let the
        // caller keep its jump behavior instead of treating it as a step.
        if (meta.kind == NAV_EDGE_JUMP) return false;
        if (!GetPortal(fromIdx, toIdx, seam)
            && !GetPortalMerged(fromIdx, toIdx, seam, axis, halfSpan))
            return false;
    }

    float nx = havePhysical ? axis.y : meta.normalX;
    float ny = havePhysical ? -axis.x : meta.normalY;
    float nl = sqrtf(nx * nx + ny * ny);
    if (nl < 0.5f)
    {
        nx = to->center.x - from->center.x;
        ny = to->center.y - from->center.y;
        nl = sqrtf(nx * nx + ny * ny);
    }
    if (nl < 0.5f) return false;
    nx /= nl; ny /= nl;
    Vector travel = to->center - from->center;
    if (travel.x * nx + travel.y * ny < 0.0f)
    {
        nx = -nx;
        ny = -ny;
    }

    Vector fromPoint = seam;
    fromPoint.x -= nx * 6.0f;
    fromPoint.y -= ny * 6.0f;
    Vector toPoint = seam;
    toPoint.x += nx * 6.0f;
    toPoint.y += ny * 6.0f;
    if (fromPoint.x < from->lo.x) fromPoint.x = from->lo.x;
    else if (fromPoint.x > from->hi.x) fromPoint.x = from->hi.x;
    if (fromPoint.y < from->lo.y) fromPoint.y = from->lo.y;
    else if (fromPoint.y > from->hi.y) fromPoint.y = from->hi.y;
    if (toPoint.x < to->lo.x) toPoint.x = to->lo.x;
    else if (toPoint.x > to->hi.x) toPoint.x = to->hi.x;
    if (toPoint.y < to->lo.y) toPoint.y = to->lo.y;
    else if (toPoint.y > to->hi.y) toPoint.y = to->hi.y;

    float fromZ = TraceFloorZ(fromIdx, fromPoint.x, fromPoint.y, meta.midZ);
    float toZ = TraceFloorZ(toIdx, toPoint.x, toPoint.y, meta.midZ);
    stepHeight = toZ - fromZ;
    return true;
}

bool CNavMesh::IsTightEdge(int fromIdx, int toIdx) const
{
    NavEdgeMeta meta;
    if (GetEdgeMeta(fromIdx, toIdx, meta) && meta.valid
        && (meta.kind == NAV_EDGE_DROP || meta.kind == NAV_EDGE_JUMP))
    {
        Vector center;
        float halfSpan = 0.0f;
        if (GetPhysicalPortal(fromIdx, toIdx, center, halfSpan))
            return halfSpan > 0.0f && halfSpan < 36.0f;
        return meta.halfSpan > 0.0f && meta.halfSpan < 36.0f;
    }

    Vector center;
    float halfSpan = 0.0f;
    return GetPhysicalPortal(fromIdx, toIdx, center, halfSpan)
        && halfSpan > 0.0f && halfSpan < 40.0f;
}

float CNavMesh::TraceFloorZ(int areaIdx, float x, float y, float fromZ) const
{
    const NavAreaData *a = Area(areaIdx);
    if (!a) return 0.0f;
    float fallback = GetZ(areaIdx, x, y);
#ifndef TESTING_NATIVE
    if (!enginetrace) return fallback;
    float startZ = a->hi.z + 64.0f;
    if (fromZ > startZ) startZ = fromZ + 64.0f;
    float endZ = a->lo.z - 128.0f;
    Vector start(x, y, startZ);
    Vector end(x, y, endZ);
    Ray_t ray;
    trace_t tr;
    CTraceFilterWorldAndPropsOnly filter;
    ray.Init(start, end);
    enginetrace->TraceRay(ray, MASK_PLAYERSOLID, &filter, &tr);
    if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f)
        return fallback;
    return tr.endpos.z;
#else
    (void)fromZ;
    return fallback;
#endif
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
        || meta.kind != NAV_EDGE_DROP)
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

    Vector physicalCenter;
    Vector physicalAxis;
    float physicalHalfSpan = 0.0f;
    const bool havePhysical = GetPhysicalPortal(fromIdx, toIdx, physicalCenter,
                                                physicalAxis, physicalHalfSpan);
    const float widthHalf = havePhysical && physicalHalfSpan > 0.0f
                          ? physicalHalfSpan : meta.halfSpan;

    // Generic short-gap transition envelope: a real gap, narrow enough for one
    // hull, with a meaningful drop. When the physical probe found the opening,
    // its floor data decides whether this is a transition; the generated NAV
    // zDelta is only a fallback for maps without usable geometry.
    if (!havePhysical && (gap <= 0.0f || gap > 64.0f)) return false;
    if (widthHalf < 8.0f || widthHalf >= 36.0f) return false;

    if (havePhysical)
    {
        takeoff.x = physicalCenter.x;
        takeoff.y = physicalCenter.y;
        float pnx = physicalAxis.y;
        float pny = -physicalAxis.x;
        float plen = sqrtf(pnx * pnx + pny * pny);
        if (plen < 0.5f) return false;
        pnx /= plen; pny /= plen;
        Vector travel = to->center - from->center;
        if (travel.x * pnx + travel.y * pny < 0.0f)
        {
            pnx = -pnx;
            pny = -pny;
        }
        landing.x = physicalCenter.x + pnx * 6.0f;
        landing.y = physicalCenter.y + pny * 6.0f;
        if (takeoff.x < from->lo.x) takeoff.x = from->lo.x;
        else if (takeoff.x > from->hi.x) takeoff.x = from->hi.x;
        if (takeoff.y < from->lo.y) takeoff.y = from->lo.y;
        else if (takeoff.y > from->hi.y) takeoff.y = from->hi.y;
        if (landing.x < to->lo.x) landing.x = to->lo.x;
        else if (landing.x > to->hi.x) landing.x = to->hi.x;
        if (landing.y < to->lo.y) landing.y = to->lo.y;
        else if (landing.y > to->hi.y) landing.y = to->hi.y;
    }
    else
    {
        takeoff.x = meta.midX;
        takeoff.y = meta.midY;
    }
    takeoff.z = TraceFloorZ(fromIdx, takeoff.x, takeoff.y, meta.midZ);
    landing.z = TraceFloorZ(toIdx, landing.x, landing.y, meta.midZ);
    if (takeoff.z - landing.z < 24.0f) return false;
    axis = havePhysical ? physicalAxis
                        : Vector(meta.axisX, meta.axisY, 0.0f);
    halfSpan = widthHalf;

    // A sidecar transition records the physical takeoff and landing that were
    // verified against the map. Prefer it over the generated rectangle overlap,
    // which can sit well off the real mouth on malformed drop links. Reject a
    // bad hint rather than steering bots toward a point outside either area.
    Vector hintTakeoff, hintLanding;
    if (GetFixupTransition(fromIdx, toIdx, hintTakeoff, hintLanding))
    {
        bool takeoffInside = hintTakeoff.x >= from->lo.x
                          && hintTakeoff.x <= from->hi.x
                          && hintTakeoff.y >= from->lo.y
                          && hintTakeoff.y <= from->hi.y
                          && fabsf(hintTakeoff.z
                                   - GetZ(fromIdx, hintTakeoff.x, hintTakeoff.y)) <= 32.0f;
        bool landingInside = hintLanding.x >= to->lo.x
                          && hintLanding.x <= to->hi.x
                          && hintLanding.y >= to->lo.y
                          && hintLanding.y <= to->hi.y
                          && fabsf(hintLanding.z
                                   - GetZ(toIdx, hintLanding.x, hintLanding.y)) <= 32.0f;
        if (takeoffInside && landingInside)
        {
            takeoff = hintTakeoff;
            landing = hintLanding;
            takeoff.z = TraceFloorZ(fromIdx, takeoff.x, takeoff.y, takeoff.z);
            landing.z = TraceFloorZ(toIdx, landing.x, landing.y, landing.z);
            float dx = landing.x - takeoff.x;
            float dy = landing.y - takeoff.y;
            float len = sqrtf(dx * dx + dy * dy);
            if (len >= 0.5f)
                axis = Vector(dx / len, dy / len, 0.0f);
        }
    }
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
    // The staging point sits 28u behind the takeoff. Use it only while it is
    // safely ahead of the bot; when approaching off-axis near the lip, aim
    // directly at the takeoff instead of steering back to that staging point.
    // Once a bot has already dropped below the takeoff ledge, never re-arm the
    // upper virtual approach point.
    bool alreadyDropped = pos.z < out.takeoff.z - 24.0f;
    bool useStaging = !alreadyDropped && along > 48.0f;
    out.virtualApproach = !alreadyDropped
                       && (useStaging || (along > 0.0f && lateral > 8.0f));
    if (out.virtualApproach)
    {
        const NavAreaData *from = Area(fromIdx);
        if (useStaging)
        {
            out.point.x = out.takeoff.x - nx * 28.0f;
            out.point.y = out.takeoff.y - ny * 28.0f;
        }
        else
            out.point = out.takeoff;
        if (from)
        {
            if (out.point.x < from->lo.x) out.point.x = from->lo.x;
            else if (out.point.x > from->hi.x) out.point.x = from->hi.x;
            if (out.point.y < from->lo.y) out.point.y = from->lo.y;
            else if (out.point.y > from->hi.y) out.point.y = from->hi.y;
        }
        out.point.z = TraceFloorZ(fromIdx, out.point.x, out.point.y,
                                  out.takeoff.z);
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
    float floorZ = TraceFloorZ(areaIdx, pos.x, pos.y, pos.z);
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
        // Ladder edges are appended after the serialized connection set, so a
        // valid area can have more than 512 resolved connections. The flat
        // arrays are sized from connOffset + connCount; validate that span
        // instead of applying the old serialized-count limit here.
        if (!a.connBad || !a.connIdx || a.connCount <= 0
            || a.connOffset < 0 || a.connCount > m_connCount - a.connOffset)
            continue;
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
        unsigned int fromId = 0, toId = 0; long cntValue = 0;
        if (sscanf(line, "%u %u %ld", &fromId, &toId, &cntValue) != 3) continue;
        if (cntValue <= 0) continue;
        int cnt = cntValue > 255 ? 255 : (int)cntValue;
        int fi = IndexOfId(fromId), ti = IndexOfId(toId);
        if (fi < 0 || ti < 0) continue;
        NavAreaData &a = m_areas[fi];
        if (!a.connBad) continue;
        for (int k = 0; k < a.connCount; k++)
            if (a.connIdx[k] == ti)
            {
                int current = (int)a.connBad[k];
                int next = (current > 255 - cnt) ? 255 : current + cnt;
                a.connBad[k] = (unsigned char)next;
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

float CNavMesh::EdgeCost(int fromIdx, int toIdx, int connSlot, int seed) const
{
    const NavAreaData &from = m_areas[fromIdx];
    const NavAreaData &to = m_areas[toIdx];
    Vector step = to.center - from.center;
    float cost = step.Length();

    int ladderIdx = -1;
    bool ladderUp = false;
    bool ladderEdge = GetLadderConnection(fromIdx, toIdx, ladderIdx, ladderUp);
    const NavEdgeMeta *meta = (from.connMeta && connSlot >= 0) ? &from.connMeta[connSlot] : 0;
    const bool walkPortal = !ladderEdge && meta && meta->valid && meta->kind == NAV_EDGE_WALK;
    if (walkPortal)
    {
        cost = meta->costLength;
    }

    if (ladderEdge)
    {
        const NavLadderData *ladder = Ladder(ladderIdx);
        float climbLen = ladder ? ladder->length : 0.0f;
        float minClimb = fabsf(step.z);
        if (climbLen < minClimb) climbLen = minClimb;
        cost = climbLen + m_costParams.ladderPenalty;
        if (ladderUp) cost += m_costParams.ladderUpPenalty;
    }
    else if (step.z < -120.0f)
    {
        // Falling off a roof is discouraged, but not forbidden.
        cost += 400.0f;
    }

    if (walkPortal && m_costParams.narrowPenalty > 0.0f)
    {
        float width = (meta->costHalfSpan > 0.0f ? meta->costHalfSpan : meta->halfSpan) * 2.0f;
        float factor = 0.0f;
        if (width < 32.0f) factor = 4.0f;
        else if (width < 56.0f) factor = 2.0f;
        else if (width < 100.0f) factor = 1.0f;
        cost += factor * m_costParams.narrowPenalty;
    }

    if (to.attributes & NAVATTR_CROUCH) cost += m_costParams.crouchPenalty;
    if (to.attributes & NAVATTR_JUMP)   cost += m_costParams.jumpPenalty;
    if (to.attributes & NAVATTR_AVOID)  cost += m_costParams.avoidPenalty;

    if (m_costParams.dangerEnabled && m_costParams.dangerPenalty > 0.0f && s_dangerCount > 0)
    {
        const Vector &p = to.center;
        for (int z = 0; z < s_dangerCount; z++)
        {
            float dx = p.x - s_dangerPos[z].x;
            float dy = p.y - s_dangerPos[z].y;
            float dz = p.z - s_dangerPos[z].z;
            float r = s_dangerRadius[z];
            if (dx * dx + dy * dy + dz * dz * 4.0f < r * r)
            {
                cost += m_costParams.dangerPenalty;
                break;
            }
        }
    }

    unsigned char bad = (from.connBad && connSlot >= 0) ? from.connBad[connSlot] : 0;
    if (bad >= 2)
    {
        int confidence = (int)bad - 1;
        if (confidence > 8) confidence = 8;
        cost += (float)confidence * 3500.0f;
        if (bad >= 3) cost += 500000.0f;
    }

    if (seed)
    {
        unsigned int h = (unsigned int)fromIdx * 2654435761u
                       + (unsigned int)toIdx * 40503u
                       + (unsigned int)seed * 2246822519u;
        cost += (float)(h & 0x3F);
    }
    return cost;
}

NavSearchResult CNavMesh::Search(int startIdx, int goalIdx, int seed,
                                 int *path, int maxLen, bool wantPath,
                                 bool *truncated)
{
    const bool ownsContext = !m_searchBusy;
    NavSearchContext localContext;
    NavSearchContext &ctx = ownsContext ? m_searchContext : localContext;
    if (ownsContext) m_searchBusy = true;
    NavSearchResult result = SearchWithContext(
        startIdx, goalIdx, seed, path, maxLen, wantPath, truncated, ctx);
    if (ownsContext) m_searchBusy = false;
    else FreeNavSearchContext(localContext);
    return result;
}

NavSearchResult CNavMesh::SearchWithContext(int startIdx, int goalIdx, int seed,
                                            int *path, int maxLen, bool wantPath,
                                            bool *truncated, NavSearchContext &ctx)
{
    NavSearchResult result;
    result.status = NAV_SEARCH_INVALID_INPUT;
    result.cost = -1.0f;
    result.pathLength = 0;
    result.truncated = false;
    if (truncated) *truncated = false;
    if (startIdx < 0 || goalIdx < 0 || startIdx >= m_count || goalIdx >= m_count)
        return result;
    if (wantPath && (!path || maxLen <= 0)) return result;

    RefreshCostParamsFromGlobals();
    if (m_areas[startIdx].component != m_areas[goalIdx].component)
    {
        result.status = NAV_SEARCH_COMPONENT_MISMATCH;
        return result;
    }
    if (startIdx == goalIdx)
    {
        result.status = NAV_SEARCH_OK;
        result.cost = 0.0f;
        result.pathLength = wantPath ? 1 : 0;
        if (wantPath) path[0] = startIdx;
        return result;
    }

    const Vector &goalPos = m_areas[goalIdx].center;

    // Binary min-heap open list with lazy deletion. Improved nodes are
    // re-pushed; stale duplicates are skipped when popped.
    if ((size_t)m_count > ((size_t)0x7FFFFFFF / 2 - 16))
    {
        result.status = NAV_SEARCH_ALLOC_FAILURE;
        return result;
    }
    int need = m_count * 2 + 16;
    if (!EnsureSearchNodes(ctx, m_count) || !EnsureSearchHeap(ctx, need))
    {
        result.status = NAV_SEARCH_ALLOC_FAILURE;
        return result;
    }
    AdvanceSearchMark(ctx);
    int openCount = 0;
    NavAreaData *A = m_areas;
    NavSearchNode *S = ctx.nodes;
    int *open = ctx.heap;
    S[startIdx].g = 0.0f;
    S[startIdx].f = (goalPos - A[startIdx].center).Length();
    S[startIdx].parent = -1;
    S[startIdx].openMark = ctx.mark;
    open[openCount++] = startIdx;

    const int maxExpansions = 2000000;
    int expansions = 0;
    while (openCount > 0)
    {
        if (expansions++ >= maxExpansions)
        {
            result.status = NAV_SEARCH_EXPANSION_LIMIT;
            return result;
        }

        int cur = open[0];
        open[0] = open[--openCount];
        int pi = 0;
        for (;;)
        {
            int l = pi * 2 + 1;
            int r = l + 1;
            int smallest = pi;
            if (l < openCount && S[open[l]].f < S[open[smallest]].f) smallest = l;
            if (r < openCount && S[open[r]].f < S[open[smallest]].f) smallest = r;
            if (smallest == pi) break;
            int t = open[smallest];
            open[smallest] = open[pi];
            open[pi] = t;
            pi = smallest;
        }
        if (S[cur].closedMark == ctx.mark) continue;

        if (cur == goalIdx)
        {
            result.status = NAV_SEARCH_OK;
            result.cost = S[cur].g;
            if (wantPath)
            {
                int totalNodes = 0;
                for (int c = cur; c != -1; c = S[c].parent)
                {
                    if (++totalNodes > m_count)
                    {
                        result.status = NAV_SEARCH_MALFORMED_GRAPH;
                        result.cost = -1.0f;
                        return result;
                    }
                }
                int emitCount = totalNodes < maxLen ? totalNodes : maxLen;
                result.truncated = totalNodes > maxLen;
                if (truncated) *truncated = result.truncated;
                int skip = totalNodes - emitCount;
                int c = cur;
                for (int i = 0; i < skip; i++) c = S[c].parent;
                for (int i = 0; i < emitCount; i++)
                {
                    path[i] = c;
                    c = S[c].parent;
                }
                for (int i = 0; i < emitCount / 2; i++)
                {
                    int t = path[i];
                    path[i] = path[emitCount - 1 - i];
                    path[emitCount - 1 - i] = t;
                }
                result.pathLength = emitCount;
            }
            return result;
        }

        S[cur].closedMark = ctx.mark;
        for (int k = 0; k < A[cur].connCount; k++)
        {
            int nb = A[cur].connIdx[k];
            if (S[nb].closedMark == ctx.mark) continue;
            float cost = EdgeCost(cur, nb, k, seed);
            float g = S[cur].g + cost;
            if (S[nb].openMark == ctx.mark && g >= S[nb].g) continue;
            S[nb].g = g;
            S[nb].f = g + (goalPos - A[nb].center).Length();
            S[nb].parent = cur;
            S[nb].openMark = ctx.mark;

            if (openCount >= ctx.heapCapacity)
            {
                int newCap = ctx.heapCapacity * 2 + 16;
                if (newCap <= ctx.heapCapacity || !EnsureSearchHeap(ctx, newCap))
                {
                    result.status = NAV_SEARCH_ALLOC_FAILURE;
                    return result;
                }
                open = ctx.heap;
            }
            int ci = openCount++;
            open[ci] = nb;
            while (ci > 0)
            {
                int par = (ci - 1) >> 1;
                if (S[open[par]].f <= S[open[ci]].f) break;
                int t = open[par];
                open[par] = open[ci];
                open[ci] = t;
                ci = par;
            }
        }
    }
    result.status = NAV_SEARCH_UNREACHABLE;
    return result;
}

int CNavMesh::BuildPath(int startIdx, int goalIdx, int *path, int maxLen, int seed, bool *truncated)
{
    INSPROF_SCOPE(INSPROF_BUILDPATH);
    NavSearchResult result = Search(startIdx, goalIdx, seed, path, maxLen, true, truncated);
    m_lastSearchStatus = result.status;
    return result.status == NAV_SEARCH_OK ? result.pathLength : 0;
}

float CNavMesh::GetPathCost(int startIdx, int goalIdx, int seed)
{
    INSPROF_SCOPE(INSPROF_GETPATHCOST);
    if (startIdx < 0 || goalIdx < 0 || startIdx >= m_count || goalIdx >= m_count)
    {
        m_lastSearchStatus = NAV_SEARCH_INVALID_INPUT;
        return -1.0f;
    }
    if (startIdx == goalIdx)
    {
        m_lastSearchStatus = NAV_SEARCH_OK;
        return 0.0f;
    }

    RefreshCostParamsFromGlobals();
    if (m_areas[startIdx].component != m_areas[goalIdx].component)
    {
        m_lastSearchStatus = NAV_SEARCH_COMPONENT_MISMATCH;
        return -1.0f;
    }

    // Deterministic cost queries are cached. Jittered and danger-aware
    // queries deliberately bypass the cache.
    if (seed == 0 && !m_costParams.dangerEnabled)
    {
        for (int i = 0; i < 256; i++)
        {
            if (m_costCache[i].stamp == m_costStamp
                && m_costCache[i].from == startIdx
                && m_costCache[i].to == goalIdx)
            {
                m_costCache[i].seq = ++m_costSeq;
                m_lastSearchStatus = (NavSearchStatus)m_costCache[i].status;
                return m_costCache[i].cost;
            }
        }
    }

    NavSearchResult result = Search(startIdx, goalIdx, seed, 0, 0, false, 0);
    m_lastSearchStatus = result.status;
    if (seed == 0 && !m_costParams.dangerEnabled
        && result.status != NAV_SEARCH_ALLOC_FAILURE
        && result.status != NAV_SEARCH_EXPANSION_LIMIT)
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
        m_costCache[slot].cost = result.status == NAV_SEARCH_OK ? result.cost : -1.0f;
        m_costCache[slot].stamp = m_costStamp;
        m_costCache[slot].seq = ++m_costSeq;
        m_costCache[slot].status = result.status;
    }
    return result.status == NAV_SEARCH_OK ? result.cost : -1.0f;
}

bool CNavMesh::SccReachable(int fromScc, int toScc) const
{
    if (!m_sccReach || m_sccReachWords <= 0
        || fromScc < 0 || fromScc >= m_sccCount
        || toScc < 0 || toScc >= m_sccCount)
        return false;
    return (m_sccReach[fromScc * m_sccReachWords + (toScc >> 5)]
            & (1u << (toScc & 31))) != 0;
}

static float NavAreaDistanceSq(const CNavMesh &mesh, int areaIdx, const Vector &desired)
{
    const NavAreaData *a = mesh.Area(areaIdx);
    if (!a) return 3.4e38f;
    float x = desired.x;
    float y = desired.y;
    if (x < a->lo.x) x = a->lo.x; else if (x > a->hi.x) x = a->hi.x;
    if (y < a->lo.y) y = a->lo.y; else if (y > a->hi.y) y = a->hi.y;
    Vector closest(x, y, mesh.GetZ(areaIdx, x, y));
    Vector d = closest - desired;
    return d.x * d.x + d.y * d.y + d.z * d.z * 4.0f;
}

int CNavMesh::FindClosestReachableArea(int startIdx, const Vector &desired)
{
    const bool ownsContext = !m_searchBusy;
    NavSearchContext localContext;
    NavSearchContext &ctx = ownsContext ? m_searchContext : localContext;
    if (ownsContext) m_searchBusy = true;
    int result = FindClosestReachableAreaWithContext(startIdx, desired, ctx);
    if (ownsContext) m_searchBusy = false;
    else FreeNavSearchContext(localContext);
    return result;
}

int CNavMesh::FindClosestReachableAreaWithContext(int startIdx, const Vector &desired,
                                                   NavSearchContext &ctx)
{
    if (startIdx < 0 || startIdx >= m_count || !m_areas || m_count <= 0)
        return -1;

    // Component-aware spatial fallback: ring outward through the existing grid
    // and keep only areas whose SCC is reachable from the start. This avoids
    // walking a whole weak component when the graph contains one-way edges.
    if (m_sccReach && m_gridStart && m_gridW > 0 && m_gridH > 0)
    {
        if (!EnsureSearchNodes(ctx, m_count)) return -1;
        AdvanceSearchMark(ctx);
        const unsigned int stamp = ctx.mark;
        NavAreaData *A = m_areas;
        NavSearchNode *S = ctx.nodes;
        const int startScc = A[startIdx].strongComponent;
        int best = startIdx;
        float bestDs = NavAreaDistanceSq(*this, startIdx, desired);
        S[startIdx].closedMark = stamp;

        int cx = (int)((desired.x - m_gridMinX) / m_gridCell);
        int cy = (int)((desired.y - m_gridMinY) / m_gridCell);
        if (cx < 0) cx = 0; else if (cx >= m_gridW) cx = m_gridW - 1;
        if (cy < 0) cy = 0; else if (cy >= m_gridH) cy = m_gridH - 1;
        const int maxRing = (m_gridW > m_gridH) ? m_gridW : m_gridH;
        for (int ring = 0; ring < maxRing; ring++)
        {
            if (ring > 0)
            {
                const float lower = (float)(ring - 1) * m_gridCell;
                if (lower * lower > bestDs) break;
            }
            const int x0 = cx - ring, x1 = cx + ring;
            const int y0 = cy - ring, y1 = cy + ring;
            for (int yy = y0; yy <= y1; yy++)
            {
                if (yy < 0 || yy >= m_gridH) continue;
                for (int xx = x0; xx <= x1; xx++)
                {
                    if (xx < 0 || xx >= m_gridW) continue;
                    if (ring > 0 && xx != x0 && xx != x1 && yy != y0 && yy != y1)
                        continue;
                    const int cell = yy * m_gridW + xx;
                    for (int k = m_gridStart[cell]; k < m_gridStart[cell + 1]; k++)
                    {
                        const int idx = m_gridCells[k];
                        if (S[idx].closedMark == stamp) continue;
                        S[idx].closedMark = stamp;
                        if (!SccReachable(startScc, A[idx].strongComponent)) continue;
                        const float ds = NavAreaDistanceSq(*this, idx, desired);
                        if (ds < bestDs || (ds == bestDs && idx < best))
                        {
                            bestDs = ds;
                            best = idx;
                        }
                    }
                }
            }
        }
        return best;
    }

    // Directed fallback: weak components can contain one-way edges, so walk
    // the actual outgoing graph when mutual reachability is not guaranteed.
    if (!EnsureSearchNodes(ctx, m_count) || !EnsureSearchFrontier(ctx, m_count))
        return -1;
    AdvanceSearchMark(ctx);
    const unsigned int stamp = ctx.mark;
    NavAreaData *A = m_areas;
    NavSearchNode *S = ctx.nodes;
    int *frontier = ctx.frontier;
    int head = 0, tail = 0;
    S[startIdx].closedMark = stamp;
    frontier[tail++] = startIdx;

    int best = startIdx;
    float bestDs = NavAreaDistanceSq(*this, startIdx, desired);

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
            if (S[nb].closedMark == stamp) continue;
            S[nb].closedMark = stamp;
            frontier[tail++] = nb;
        }
    }
    return best;
}
