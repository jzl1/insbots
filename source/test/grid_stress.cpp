// native stress test: load a real .nav, compare grid FindArea/FindNearestArea
// against the old full-scan implementations on thousands of positions.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
struct Vector { float x,y,z; Vector(){} Vector(float a,float b,float c):x(a),y(b),z(c){}
  Vector operator-(const Vector&o)const{return Vector(x-o.x,y-o.y,z-o.z);}
  float Length()const{return sqrtf(x*x+y*y+z*z);} };
#define private public
#define protected public
#define TESTING_NATIVE 1
#include "../src/nav.h"
#include "../src/nav.cpp"

// exact copies of the pre-grid implementations
static int LinearFindArea(const CNavMesh &m, const Vector &pos)
{
    int best = -1;
    float bestSurfaceZ = -3.4e38f;
    const float testZ = pos.z + 5.0f;
    for (int i = 0; i < m.Count(); i++)
    {
        const NavAreaData *a = m.Area(i);
        if (pos.x < a->lo.x || pos.x > a->hi.x) continue;
        if (pos.y < a->lo.y || pos.y > a->hi.y) continue;
        float surfaceZ = m.GetZ(i, pos.x, pos.y);
        if (surfaceZ > testZ) continue;
        if (surfaceZ < pos.z - 120.0f) continue;
        if (surfaceZ > bestSurfaceZ) { bestSurfaceZ = surfaceZ; best = i; }
    }
    if (best >= 0) return best;
    float bestD = 3.4e38f;
    for (int i = 0; i < m.Count(); i++)
    {
        const NavAreaData *a = m.Area(i);
        float x = pos.x, y = pos.y;
        if (x < a->lo.x) x = a->lo.x; else if (x > a->hi.x) x = a->hi.x;
        if (y < a->lo.y) y = a->lo.y; else if (y > a->hi.y) y = a->hi.y;
        Vector closest(x, y, m.GetZ(i, x, y));
        Vector d = closest - pos;
        float ds = d.x*d.x + d.y*d.y + d.z*d.z*4.0f;
        if (ds < bestD) { bestD = ds; best = i; }
    }
    return best;
}

static int LinearNearest(const CNavMesh &m, const Vector &pos)
{
    int best = -1;
    float bestD = 3.4e38f;
    for (int i = 0; i < m.Count(); i++)
    {
        const NavAreaData *a = m.Area(i);
        float x = pos.x, y = pos.y;
        if (x < a->lo.x) x = a->lo.x; else if (x > a->hi.x) x = a->hi.x;
        if (y < a->lo.y) y = a->lo.y; else if (y > a->hi.y) y = a->hi.y;
        Vector closest(x, y, m.GetZ(i, x, y));
        Vector d = closest - pos;
        float ds = d.x*d.x + d.y*d.y + d.z*d.z*4.0f;
        if (ds < bestD) { bestD = ds; best = i; }
    }
    return best;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "ins_sinjar.nav";
    CNavMesh m;
    if (!m.Load(path)) { printf("FAIL: load %s\n", path); return 1; }
    printf("loaded %d areas, %d ladders, %d sniper spots\n", m.Count(), m.LadderCount(), m.GetTotalSniperSpotCount());

    float minX=1e30f,minY=1e30f,maxX=-1e30f,maxY=-1e30f;
    std::vector<Vector> pts;
    for (int i = 0; i < m.Count(); i += 200)
    {
        const NavAreaData *a = m.Area(i);
        if (a->lo.x < minX) minX = a->lo.x;
        if (a->lo.y < minY) minY = a->lo.y;
        if (a->hi.x > maxX) maxX = a->hi.x;
        if (a->hi.y > maxY) maxY = a->hi.y;
        pts.push_back(a->center);
        pts.push_back(Vector(a->lo.x, a->lo.y, a->center.z));
        pts.push_back(Vector(a->hi.x, a->lo.y, a->center.z));
        pts.push_back(Vector(a->lo.x, a->hi.y, a->center.z));
        pts.push_back(Vector(a->hi.x, a->hi.y, a->center.z));
        float mx=(a->lo.x+a->hi.x)*0.5f, my=(a->lo.y+a->hi.y)*0.5f;
        pts.push_back(Vector(mx, a->lo.y, a->center.z));
        pts.push_back(Vector(mx, a->hi.y, a->center.z));
        pts.push_back(Vector(a->lo.x, my, a->center.z));
        pts.push_back(Vector(a->hi.x, my, a->center.z));
    }
    int degenerate = 0;
    int nanCount = 0;
    int infCount = 0;
    for (int i = 0; i < m.Count(); i++)
    {
        const NavAreaData *a = m.Area(i);
        if (a->lo.x > a->hi.x || a->lo.y > a->hi.y) degenerate++;
        if (!(a->lo.x == a->lo.x) || !(a->lo.y == a->lo.y) || !(a->lo.z == a->lo.z) ||
            !(a->hi.x == a->hi.x) || !(a->hi.y == a->hi.y) || !(a->hi.z == a->hi.z) ||
            !(a->center.x == a->center.x) || !(a->center.y == a->center.y) || !(a->center.z == a->center.z))
            nanCount++;
        if (a->lo.x > 3.4e38f || a->lo.x < -3.4e38f || a->lo.y > 3.4e38f || a->lo.y < -3.4e38f ||
            a->hi.x > 3.4e38f || a->hi.x < -3.4e38f || a->hi.y > 3.4e38f || a->hi.y < -3.4e38f ||
            a->center.x > 3.4e38f || a->center.x < -3.4e38f || a->center.y > 3.4e38f || a->center.y < -3.4e38f)
            infCount++;
    }
    printf("degenerate areas: %d, nan areas: %d, inf areas: %d\n", degenerate, nanCount, infCount);
    unsigned rng = 12345u;
    for (int i = 0; i < 2000; i++)
    {
        rng = rng * 1664525u + 1013904223u;
        float px = minX + (float)(rng & 0xFFFF) / 65535.0f * (maxX-minX);
        rng = rng * 1664525u + 1013904223u;
        float py = minY + (float)(rng & 0xFFFF) / 65535.0f * (maxY-minY);
        rng = rng * 1664525u + 1013904223u;
        float pz = (float)(rng & 0xFFF) / 4095.0f * 1000.0f - 100.0f;
        pts.push_back(Vector(px, py, pz));
        float bx = minX + ((i & 0x1F) * 512.0f);
        float by = minY + ((i & 0x1F) * 512.0f);
        pts.push_back(Vector(bx, by, pz));
    }

    int mismatches = 0, firstBad = -1;
    for (size_t i = 0; i < pts.size(); i++)
    {
        int g = m.FindArea(pts[i]);
        int l = LinearFindArea(m, pts[i]);
        if (g != l)
        {
            mismatches++;
            if (firstBad < 0)
            {
                firstBad = (int)i;
                const NavAreaData *ga = g >= 0 ? m.Area(g) : 0;
                const NavAreaData *la = l >= 0 ? m.Area(l) : 0;
                printf("first mismatch p=(%.2f %.2f %.2f) g=%d l=%d\n", pts[i].x, pts[i].y, pts[i].z, g, l);
                if (ga) printf("  grid area: lo=(%.1f %.1f %.1f) hi=(%.1f %.1f %.1f) id=%u\n", ga->lo.x, ga->lo.y, ga->lo.z, ga->hi.x, ga->hi.y, ga->hi.z, ga->id);
                if (la) printf("  lin  area: lo=(%.1f %.1f %.1f) hi=(%.1f %.1f %.1f) id=%u\n", la->lo.x, la->lo.y, la->lo.z, la->hi.x, la->hi.y, la->hi.z, la->id);
                printf("  grid: min=(%.1f %.1f) cell=%.1f w=%d h=%d\n", m.m_gridMinX, m.m_gridMinY, m.m_gridCell, m.m_gridW, m.m_gridH);
                int cx = (int)((pts[i].x - m.m_gridMinX) / m.m_gridCell);
                int cy = (int)((pts[i].y - m.m_gridMinY) / m.m_gridCell);
                if (cx < 0) cx = 0; else if (cx >= m.m_gridW) cx = m.m_gridW - 1;
                if (cy < 0) cy = 0; else if (cy >= m.m_gridH) cy = m.m_gridH - 1;
                printf("  point cell: (%d,%d)\n", cx, cy);
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                    {
                        int yy = cy + dy, xx = cx + dx;
                        if (yy < 0 || yy >= m.m_gridH || xx < 0 || xx >= m.m_gridW) continue;
                        int cell = yy * m.m_gridW + xx;
                        bool has0 = false, has3243 = false;
                        for (int k = m.m_gridStart[cell]; k < m.m_gridStart[cell + 1]; k++)
                        {
                            if (m.m_gridCells[k] == 0) has0 = true;
                            if (m.m_gridCells[k] == 3243) has3243 = true;
                        }
                        printf("    cell(%d,%d) n=%d has0=%d has3243=%d\n", xx, yy, m.m_gridStart[cell+1]-m.m_gridStart[cell], has0?1:0, has3243?1:0);
                    }
                printf("  GetZ(0,p)=%.2f GetZ(3243,p)=%.2f testZ=%.2f\n",
                       m.GetZ(0, pts[i].x, pts[i].y), m.GetZ(3243, pts[i].x, pts[i].y), pts[i].z + 5.0f);
                break;
            }
        }
        int gn = m.FindNearestArea(pts[i]);
        int ln = LinearNearest(m, pts[i]);
        if (gn != ln) { mismatches++; if (firstBad < 0) firstBad = (int)i; }
    }
    printf("tested %d point-pairs, mismatches=%d", (int)pts.size(), mismatches);
    if (firstBad >= 0)
        printf(" first at %d: (%.2f %.2f %.2f)", firstBad, pts[firstBad].x, pts[firstBad].y, pts[firstBad].z);
    printf("\n");

    // hammer A* on the real mesh (catches heap corruption that a point test
    // cannot; also exercises the grid-built mesh the same way the plugin does)
    unsigned seed2 = 999u;
    int route[512];
    int okPath = 0, failPath = 0;
    for (int t = 0; t < 500; t++)
    {
        seed2 = seed2 * 1664525u + 1013904223u;
        int s = (int)(seed2 % (unsigned)m.Count());
        seed2 = seed2 * 1664525u + 1013904223u;
        int g = (int)(seed2 % (unsigned)m.Count());
        int n = m.BuildPath(s, g, route, 512, t);
        if (n > 0) okPath++; else failPath++;
        float c = m.GetPathCost(s, g, t);
        if (c < -0.5f && n > 0) failPath++;
    }
    printf("A* hammer: ok=%d fail=%d\n", okPath, failPath);
    return mismatches == 0 ? 0 : 2;
}
