// findstress.cpp - diagnose FindArea cost on a real nav mesh.
// usage: findstress <map.nav>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <windows.h>

struct Vector { float x,y,z; Vector(){} Vector(float a,float b,float c):x(a),y(b),z(c){}
  Vector operator-(const Vector&o)const{return Vector(x-o.x,y-o.y,z-o.z);}
  float Length()const{return sqrtf(x*x+y*y+z*z);} };
#define private public
#define protected public
#define TESTING_NATIVE 1
#include "../src/nav.h"
#include "../src/nav.cpp"

static double NowMs()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static unsigned s_rng = 0x9e3779b9u;
static unsigned Rand() { s_rng = s_rng * 1664525u + 1013904223u; return s_rng; }
static float FRand(float a, float b) { return a + (float)(Rand() & 0xFFFF) / 65535.0f * (b - a); }

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "ins_sinjar.nav";
    CNavMesh m;
    if (!m.Load(path)) { printf("FAIL: load %s\n", path); return 1; }
    printf("loaded %d areas, grid %dx%d cell=%.0f\n", m.Count(), m.m_gridW, m.m_gridH, m.m_gridCell);

    // ---- cell occupancy stats ----
    long long sum = 0; int maxN = 0; long long over100 = 0, over250 = 0, over500 = 0;
    int cells = m.m_gridW * m.m_gridH;
    std::vector<int> dense;
    for (int c = 0; c < cells; c++)
    {
        int n = m.m_gridStart[c + 1] - m.m_gridStart[c];
        sum += n;
        if (n > maxN) maxN = n;
        if (n > 100) over100++;
        if (n > 250) over250++;
        if (n > 500) over500++;
        if (n > 200) dense.push_back(n);
    }
    printf("cells=%d avg_areas/cell=%.1f max=%d over100=%lld over250=%lld over500=%lld\n",
           cells, (double)sum / (double)cells, maxN, over100, over250, over500);
    std::sort(dense.begin(), dense.end());
    if (!dense.empty())
        printf("densest cells (asc): %d %d %d %d %d\n",
               dense[dense.size()-1], dense[dense.size()-2], dense[dense.size()-3],
               dense[dense.size()-4], dense[dense.size()-5]);

    float minX=1e30f,minY=1e30f,maxX=-1e30f,maxY=-1e30f,minZ=1e30f,maxZ=-1e30f;
    for (int i = 0; i < m.Count(); i++)
    {
        const NavAreaData *a = m.Area(i);
        if (a->lo.x < minX) minX = a->lo.x; if (a->hi.x > maxX) maxX = a->hi.x;
        if (a->lo.y < minY) minY = a->lo.y; if (a->hi.y > maxY) maxY = a->hi.y;
        if (a->lo.z < minZ) minZ = a->lo.z; if (a->hi.z > maxZ) maxZ = a->hi.z;
    }
    printf("bbox x[%.0f,%.0f] y[%.0f,%.0f] z[%.0f,%.0f]\n", minX, maxX, minY, maxY, minZ, maxZ);

    const int N = 20000;
    std::vector<Vector> onMesh, offVertical, offRandom, randomXY;
    for (int i = 0; i < N; i++)
    {
        const NavAreaData *a = m.Area((int)(Rand() % (unsigned)m.Count()));
        onMesh.push_back(Vector(a->center.x + FRand(-12,12), a->center.y + FRand(-12,12),
                                a->center.z + FRand(-4,4)));
        offVertical.push_back(Vector(a->center.x + FRand(-12,12), a->center.y + FRand(-12,12),
                                     a->center.z + 180.0f));
        offRandom.push_back(Vector(FRand(minX,maxX), FRand(minY,maxY), FRand(minZ,maxZ)));
        randomXY.push_back(Vector(FRand(minX,maxX), FRand(minY,maxY), 0.0f));
    }

    const char *names[4] = { "on-mesh", "off-vertical+180", "random-3d", "random-xy-z0" };
    std::vector<Vector> *sets[4] = { &onMesh, &offVertical, &offRandom, &randomXY };
    for (int s = 0; s < 4; s++)
    {
        double t0 = NowMs();
        int hits = 0;
        for (int i = 0; i < N; i++)
        {
            int r = m.FindArea((*sets[s])[i]);
            if (r >= 0) hits++;
        }
        double t1 = NowMs();
        printf("%-16s avg %.4f ms/call  (%d%% hit)\n", names[s],
               (t1 - t0) / (double)N, hits * 100 / N);
    }

    // FindNearestArea on the same sets
    for (int s = 0; s < 4; s++)
    {
        double t0 = NowMs();
        for (int i = 0; i < N; i++) m.FindNearestArea((*sets[s])[i]);
        double t1 = NowMs();
        printf("%-16s nearest avg %.4f ms/call\n", names[s], (t1 - t0) / (double)N);
    }
    return 0;
}
