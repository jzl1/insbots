// ============================================================================
// nav.h - loader + A* pathfinding for Source .nav meshes (versions 4..9,
//         the format written by the 2006 SDK code INS:MIC was built from)
// ============================================================================
#pragma once

#ifndef TESTING_NATIVE
#include "vector.h"
#endif

enum NavDir { NAV_NORTH = 0, NAV_EAST, NAV_SOUTH, NAV_WEST, NAV_DIR_COUNT };

// attribute flags (nav.h, 2006 SDK)
enum
{
    NAVATTR_CROUCH  = 0x01,
    NAVATTR_JUMP    = 0x02,
    NAVATTR_PRECISE = 0x04,
    NAVATTR_NO_JUMP    = 0x08,
    NAVATTR_AVOID      = 0x0080,   // Source NAV_MESH_AVOID: route around unless it's the only way
    NAVATTR_UNDERWATER = 0x010000, // Source NAV_MESH_UNDERWATER
};

struct HidingSpot
{
    unsigned int   id;
    Vector         pos;
    unsigned char  flags; // 0x01=in cover, 0x02=good sniper, 0x04=ideal sniper, 0x08=exposed
    int            areaIdx;
};

enum
{
    SNIPER_SPOT_IN_COVER  = 0x01,
    SNIPER_SPOT_GOOD      = 0x02,
    SNIPER_SPOT_IDEAL     = 0x04,
    SNIPER_SPOT_EXPOSED   = 0x08,
    SNIPER_SPOT_GENERATED = 0x10,
    SNIPER_SPOT_CORNER    = 0x20
};

// Source v7+ mesh-level ladder record. Area indices are resolved after load;
// -1 means that endpoint was not connected in the NAV file.
struct NavLadderData
{
    unsigned int id;
    float        width;
    Vector       top, bottom;
    float        length;
    int          dir;             // NavDir: normal of the climbable face
    int          topForwardArea;
    int          topLeftArea;
    int          topRightArea;
    int          topBehindArea;   // metadata only; Source excludes it from climb-up
    int          bottomArea;
};

enum NavEdgeKind
{
    NAV_EDGE_WALK = 0,
    NAV_EDGE_LADDER_UP,
    NAV_EDGE_LADDER_DOWN,
    NAV_EDGE_DROP,
    NAV_EDGE_JUMP,
    NAV_EDGE_MALFORMED
};

struct NavEdgeMeta
{
    unsigned char kind;
    unsigned char valid;          // record built and usable as a typed edge
    float         midX, midY, midZ;
    float         axisX, axisY;
    float         normalX, normalY;
    float         halfSpan;
    float         zDelta;
};

struct NavAreaData
{
    unsigned int   id;
    unsigned short attributes;
    Vector         lo, hi;        // extent
    float          neZ, swZ;      // implicit corner heights
    Vector         center;

    // connection ids per direction, resolved to indices after load
    int           *connIdx;       // indices into the area array
    unsigned char *connBad;       // per-connection failure count (learned bad links)
    int           *connLadder;    // 0=walk, +(index+1)=ladder up, -(index+1)=down
    NavEdgeMeta   *connMeta;      // one typed portal record per connection
    int            connCount;
    int            ladderConnCount; // nonzero when this area is a ladder endpoint
    int            component;       // weak connected component for cheap reachability rejection

    // hiding & sniper spots (Phase 1 Overwatch AI)
    HidingSpot    *hidingSpots;
    int            hidingSpotCount;

    // scratch for A*
    float          g, f;
    int            parent;
    unsigned int   openMark, closedMark;
};

struct NavCostParams
{
    float jumpPenalty;
    float crouchPenalty;
    float avoidPenalty;
    float ladderPenalty;
    float ladderUpPenalty;
    float dangerPenalty;
    bool  dangerEnabled;

    NavCostParams()
        : jumpPenalty(4000.0f), crouchPenalty(2500.0f), avoidPenalty(3000.0f),
          ladderPenalty(3000.0f), ladderUpPenalty(500.0f), dangerPenalty(6000.0f),
          dangerEnabled(false)
    {
    }
};

class CNavMesh
{
public:
    CNavMesh() : m_areas(0), m_count(0), m_mark(0), m_ladders(0), m_ladderCount(0), m_sniperSpots(0), m_sniperSpotCount(0),
                 m_gridW(0), m_gridH(0), m_gridMinX(0), m_gridMinY(0), m_gridCell(0), m_gridStart(0), m_gridCells(0),
                 m_costStamp(0), m_costSeq(0), m_costParams()
    {
        for (int i = 0; i < 64; i++)
        {
            m_fbCache[i].key = 0;
            m_fbCache[i].area = -1;
            m_fbCache[i].pos = Vector(0, 0, 0);
        }
    }
    ~CNavMesh() { Free(); }

    bool  Load(const char *path);       // absolute or game-relative path
    void  Free();
    bool  IsLoaded() const;
    int   Count() const { return m_count; }

    // area immediately beneath a position using its interpolated surface Z;
    // off-mesh positions fall back to the nearest reachable point on an area
    int   FindArea(const Vector &pos) const;
    // nearest area to a point regardless of line-of-sight. Used for GOAL
    // positions (e.g. objective centers) that sit just off the mesh, so the
    // bot routes to the closest tile instead of beelining through walls.
    int   FindNearestArea(const Vector &pos) const;
    const NavAreaData *Area(int idx) const { return (idx >= 0 && idx < m_count) ? &m_areas[idx] : 0; }

    // A* between area indices; fills 'path' with up to maxLen indices
    // (start..goal). Returns length, or 0 on failure. When the complete route
    // contains more nodes than maxLen, the returned array is a true prefix that
    // always begins at startIdx and *truncated is set to true; the caller may
    // continue from path[maxLen-1].
    //  seed: per-bot value that adds small cost jitter, so different bots pick
    //        diverse routes (proactive path-splitting) instead of all stacking
    //        on the single shortest path.
    int   BuildPath(int startIdx, int goalIdx, int *path, int maxLen, int seed = 0, bool *truncated = 0);

    // Same cost model as BuildPath, but returns only the total path cost so
    // callers can compare routes without reconstructing a path. Returns -1
    // when start/goal is invalid or no route exists.
    float GetPathCost(int startIdx, int goalIdx, int seed = 0);

    // Follows outgoing NAV connections starting at 'startIdx' and returns the
    // reachable area whose surface/center is closest to 'desired'. Chooses the
    // closest point clamped to each area's XY rectangle with GetZ() at that
    // point, using the same vertical weighting convention as NAV lookup
    // (dx*dx + dy*dy + 4*dz*dz). Returns -1 for an invalid start or empty
    // mesh, and may return startIdx when no better reachable area exists. Does
    // not alter learned bad-link counters or call engine traces.
    int FindClosestReachableArea(int startIdx, const Vector &desired);

    // midpoint of the shared edge between two adjacent areas (the natural
    // crossing point). Returns false if they don't overlap.
    bool  GetPortal(int fromIdx, int toIdx, Vector &out) const;

    // Portal midpoint plus the direction and half-length of the shared edge,
    // with contiguous collinear slices merged into one opening (a doorway the
    // nav generator cut into several 25u segments becomes a single wider
    // passage). Used to spread same-direction bots across an opening instead
    // of aiming everyone at the same point. Returns false on ladder edges and
    // when the areas don't overlap.
    bool  GetPortalMerged(int fromIdx, int toIdx, Vector &mid, Vector &axis,
                          float &halfSpan) const;

    // Returns the typed ladder edge between two path areas. Ordinary shared-
    // edge connections return false. 'up' is true when traversing bottom->top.
    bool  GetLadderConnection(int fromIdx, int toIdx, int &ladderIdx, bool &up) const;

    // Returns the typed geometry record for a resolved connection. This also
    // returns malformed records so callers can inspect why a link has no
    // ordinary portal; check NavEdgeMeta::valid before treating it as usable.
    bool  GetEdgeMeta(int fromIdx, int toIdx, NavEdgeMeta &out) const;

    const NavLadderData *Ladder(int idx) const
        { return (idx >= 0 && idx < m_ladderCount) ? &m_ladders[idx] : 0; }
    int   LadderCount() const { return m_ladderCount; }

    // report that traversing from->to failed (bot got stuck). This is remembered
    // PERSISTENTLY for the whole map and SHARED across all bots, so once a
    // connection is proven impassable every bot routes around it.
    void  MarkBadConnection(int fromIdx, int toIdx);
    void  MarkGoodConnection(int fromIdx, int toIdx); // successful traversal rehabilitates a link
    bool  HasConnection(int fromIdx, int toIdx) const;

    // how many times this link has been reported bad (for the overlay)
    int   BadCount(int fromIdx, int toIdx) const;

    // persistence of learned bad links: what the bots discover at runtime
    // (including prop-blocked passages no offline tool can see) survives map
    // changes and restarts via a small per-map text file.
    bool  SaveBadLinks(const char *path) const;
    int   LoadBadLinks(const char *path);      // returns number of links applied
    void  ClearBadLinks();                     // zero all counters (insbot forget)

    // z height of the area surface at x,y (bilinear-ish approx)
    float GetZ(int idx, float x, float y) const;

    int   RandomArea() const;

    // Phase 1 Sniper & Overwatch Support: query nearby tactical overlooks
    int   GetSniperSpots(const Vector &origin, float maxDist, HidingSpot *outSpots, int maxOut) const;
    int   GetTotalSniperSpotCount() const { return m_sniperSpotCount; }
    const HidingSpot *GetSniperSpot(int idx) const
        { return (idx >= 0 && idx < m_sniperSpotCount) ? &m_sniperSpots[idx] : 0; }
    int   ComponentForArea(int idx) const
        { return (idx >= 0 && idx < m_count) ? m_areas[idx].component : -1; }

private:
    NavAreaData *m_areas;
    int          m_count;
    unsigned int m_mark;
    NavLadderData *m_ladders;
    int            m_ladderCount;
    HidingSpot  *m_sniperSpots;
    int          m_sniperSpotCount;

    // uniform spatial index (512u cells) over area bounds, built after load
    // so FindArea/FindNearestArea do not scan all 25k areas every frame
    // (the main lag on large maps like sinjar)
    int    m_gridW, m_gridH;
    float  m_gridMinX, m_gridMinY, m_gridCell;
    int   *m_gridStart;   // W*H+1 prefix offsets into m_gridCells
    int   *m_gridCells;   // area indices, grouped by cell

    // Off-mesh fallback result cache (FindArea). Positions are quantized
    // to ~64u so repeated queries from the same spot (stuck retries,
    // several subsystems per frame) reuse the validated tile without
    // re-running engine traces. Safe: results depend only on static
    // geometry, never on learned links.
    struct FallbackEntry { int key; Vector pos; int area; };
    mutable FallbackEntry m_fbCache[64];

    // Small persistent cache for GetPathCost(seed=0): capture refills and
    // objective picking query the same (area -> goal) pairs over and over,
    // and on a 25k-area mesh each miss is a full A*. Costs are stable until a
    // learned bad link changes, so entries live until then (or until the mesh
    // reloads). 256 entries covers every bot area for one objective.
    struct CostEntry { int from, to; float cost; unsigned int stamp; unsigned int seq; };
    CostEntry      m_costCache[256];
    unsigned int   m_costStamp;
    unsigned int m_costSeq;
    NavCostParams  m_costParams;
    void InvalidateCostCache() { m_costSeq = 0; if (++m_costStamp == 0) m_costStamp = 1; }
    void RefreshCostParamsFromGlobals();
    bool BuildEdgeMetadata(int fromIdx, int connSlot, NavEdgeMeta &out) const;

    int  IndexOfId(unsigned int id) const;
    void BuildGrid();
};

extern CNavMesh g_NavMesh;

// jump/crouch avoidance penalties applied in BuildPath (set by the plugin)
extern float g_navJumpPenalty;
extern float g_navCrouchPenalty;
extern float g_navAvoidPenalty;
// ladder-aware A* costs (set by the plugin from cvars)
extern float g_navLadderPenalty;    // additive cost per ladder edge; flat routes win when they exist
extern float g_navLadderUpPenalty;  // extra cost when climbing UP a ladder (descending is cheap)

// Dynamic firefight-avoidance zones (fed per-frame by the roster). Only
// capture-committed bots enable the penalty (g_navDangerEnabled), so normal
// navigation is never distorted by a nearby fight.
extern float g_navDangerPenalty;   // additive A* cost per area inside a danger zone
extern bool   g_navDangerEnabled;  // per-bot flag: route around active firefights
void NavClearDangerZones();
void NavAddDangerZone(const Vector &center, float radius);
