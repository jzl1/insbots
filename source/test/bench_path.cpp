// bench_path.cpp - A* / nav-mesh benchmark against a real .nav mesh.
//
// usage: bench_path <map.nav> [scenario] [iters]
//   scenarios: long   - uniform random start/goal pairs (map-crossing, seed jitter)
//              local  - nearby pairs (<1500u), seed jitter
//              obj    - random starts to 4 map-spread goal areas, seed jitter
//              mixed  - 70% long / 20% local / 10% obj (default)
//              danger - mixed, but with active firefight zones (capture-committed)
//              cost   - GetPathCost(seed=0): miss-heavy then hit-heavy
//              find   - FindArea / FindNearestArea per-call timing
//              all    - run everything (default)
#define NOMINMAX
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <windows.h>

struct Vector
{
    float x, y, z;
    Vector() {}
    Vector(float a, float b, float c) : x(a), y(b), z(c) {}
    Vector operator-(const Vector &o) const { return Vector(x - o.x, y - o.y, z - o.z); }
    float Length() const { return sqrtf(x * x + y * y + z * z); }
    float Dist2D(const Vector &o) const { float dx = x - o.x, dy = y - o.y; return sqrtf(dx * dx + dy * dy); }
};

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

static unsigned s_rng = 0x12345678u;
static unsigned Rand()
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng;
}

struct SampleStats
{
    double avg, med, p95, max;
};

static SampleStats Compute(const std::vector<double> &v)
{
    SampleStats s = { 0, 0, 0, 0 };
    if (v.empty()) return s;
    double sum = 0;
    s.max = v[0];
    for (size_t i = 0; i < v.size(); i++)
    {
        sum += v[i];
        if (v[i] > s.max) s.max = v[i];
    }
    s.avg = sum / (double)v.size();
    std::vector<double> c = v;
    std::sort(c.begin(), c.end());
    s.med = c[c.size() / 2];
    size_t p95Index = (size_t)(c.size() * 0.95);
    if (p95Index >= c.size()) p95Index = c.size() - 1;
    s.p95 = c[p95Index];
    return s;
}

static void PrintStats(const char *what, const std::vector<double> &ms,
                       const std::vector<double> &exp, const std::vector<double> &push,
                       const std::vector<double> &len, int ok, int fail)
{
    SampleStats t = Compute(ms), e = Compute(exp), p = Compute(push), l = Compute(len);
    printf("%-28s ok=%d fail=%d  ms avg=%.3f med=%.3f p95=%.3f max=%.3f\n",
           what, ok, fail, t.avg, t.med, t.p95, t.max);
    printf("    expansions avg=%.0f p95=%.0f max=%.0f | pushes avg=%.0f p95=%.0f max=%.0f | pathlen avg=%.1f max=%.0f\n",
           e.avg, e.p95, e.max, p.avg, p.p95, p.max, l.avg, l.max);
    // 30 bots, scaled repath delay on >25k-area meshes = 3.6-7.2s -> ~5.5 paths/s total.
    double perSec[] = { 5.5, 10.0, 20.0 };
    printf("    est. frame cost @%.1f paths/s: %.2f ms/frame | @%.0f: %.2f ms/frame | @%.0f: %.2f ms/frame\n",
           perSec[0], t.avg * perSec[0],
           perSec[1], t.avg * perSec[1], perSec[2], t.avg * perSec[2]);
    printf("    (%.1f%% of a 16.7ms frame @5.5 paths/s, %.1f%% @10, %.1f%% @20)\n",
           t.avg * perSec[0] / 16.7 * 100.0, t.avg * perSec[1] / 16.7 * 100.0, t.avg * perSec[2] / 16.7 * 100.0);
}

// kind: 0=long, 1=local, 2=obj-goal cluster
static void RunBuildPath(CNavMesh &m, int iters, int kind, bool danger, const char *label)
{
    std::vector<double> ms, exp, push, len;
    int ok = 0, fail = 0;
    int route[512];

    std::vector<int> goals;
    if (kind == 2)
    {
        // 4 well-separated goal areas spread across the map
        for (int tries = 0; tries < 200000 && (int)goals.size() < 4; tries++)
        {
            int g = (int)(Rand() % (unsigned)m.Count());
            bool good = true;
            for (size_t k = 0; k < goals.size(); k++)
            {
                const NavAreaData *a = m.Area(goals[k]), *b = m.Area(g);
                if (a->center.Dist2D(b->center) < 4000.0f) { good = false; break; }
            }
            if (good) goals.push_back(g);
        }
        printf("%s: goal areas =", label);
        for (size_t k = 0; k < goals.size(); k++) printf(" %d", goals[k]);
        printf("\n");
    }

    if (danger)
    {
        NavClearDangerZones();
        for (int z = 0; z < 4; z++)
        {
            const NavAreaData *a = m.Area((int)(Rand() % (unsigned)m.Count()));
            NavAddDangerZone(a->center, 500.0f);
        }
        g_navDangerEnabled = true;
    }

    for (int i = 0; i < iters; i++)
    {
        int s = (int)(Rand() % (unsigned)m.Count());
        int g;
        if (kind == 2 && !goals.empty())
            g = goals[Rand() % (unsigned)goals.size()];
        else if (kind == 1)
        {
            const NavAreaData *a = m.Area(s);
            g = -1;
            for (int t = 0; t < 80; t++)
            {
                int c = (int)(Rand() % (unsigned)m.Count());
                const NavAreaData *b = m.Area(c);
                if (a->center.Dist2D(b->center) <= 1500.0f && fabsf(a->center.z - b->center.z) <= 200.0f)
                { g = c; break; }
            }
            if (g < 0) g = (int)(Rand() % (unsigned)m.Count());
        }
        else
            g = (int)(Rand() % (unsigned)m.Count());

        int seed = 1 + (i % 32); // distinct per-bot seeds like the plugin
        double t0 = NowMs();
        int n = m.BuildPath(s, g, route, 512, seed);
        double dt = NowMs() - t0;
        if (n > 0) ok++; else fail++;

        // count expansions (closedMark) and heap pushes (openMark) from the last search
        unsigned mark = m.m_mark;
        double ec = 0, pc = 0;
        for (int k = 0; k < m.m_count; k++)
        {
            if (m.m_areas[k].closedMark == mark) ec++;
            if (m.m_areas[k].openMark == mark) pc++;
        }
        ms.push_back(dt);
        exp.push_back(ec);
        push.push_back(pc);
        len.push_back((double)(n > 0 ? n : 0));
    }

    if (danger) g_navDangerEnabled = false;
    PrintStats(label, ms, exp, push, len, ok, fail);
}

static void RunCost(CNavMesh &m, int iters)
{
    printf("\n== GetPathCost(seed=0) ==\n");
    std::vector<double> cold, warm;
    int ok = 0;

    // miss-heavy: mostly unique pairs (cache holds only 256 entries)
    for (int i = 0; i < iters; i++)
    {
        int s = (int)(Rand() % (unsigned)m.Count());
        int g = (int)(Rand() % (unsigned)m.Count());
        double t0 = NowMs();
        float c = m.GetPathCost(s, g, 0);
        double dt = NowMs() - t0;
        if (c >= 0.0f) ok++;
        cold.push_back(dt);
    }
    SampleStats cs = Compute(cold);
    printf("%-28s ok=%d  ms avg=%.4f med=%.4f p95=%.4f max=%.4f\n",
           "unique pairs (cache miss)", ok, cs.avg, cs.med, cs.p95, cs.max);

    // hit-heavy: 32 bot-area -> 4 goal pairs repeated (objective picking pattern)
    std::vector<std::pair<int,int> > pairs;
    for (int b = 0; b < 32; b++)
    {
        int s = (int)(Rand() % (unsigned)m.Count());
        for (int gk = 0; gk < 4; gk++)
            pairs.push_back(std::make_pair(s, (int)(Rand() % (unsigned)m.Count())));
    }
    m.InvalidateCostCache(); // fresh stamps so warmup fills distinct slots
    // warm the cache: one miss per pair first, then time pure hits
    for (size_t wi = 0; wi < pairs.size(); wi++)
        m.GetPathCost(pairs[wi].first, pairs[wi].second, 0);
    int hits = 0, miss = 0;
    for (int i = 0; i < iters; i++)
    {
        const std::pair<int,int> &p = pairs[i % pairs.size()];
        double t0 = NowMs();
        float c = m.GetPathCost(p.first, p.second, 0);
        double dt = NowMs() - t0;
        if (dt < 0.05) hits++; else miss++;
        warm.push_back(dt);
    }
    SampleStats ws = Compute(warm);
    printf("%-28s ok=%d  ms avg=%.4f med=%.4f p95=%.4f max=%.4f  (sub-0.05ms hits=%d, slower=%d)\n",
           "repeated pairs (cache hit)", ok, ws.avg, ws.med, ws.p95, ws.max, hits, miss);
    printf("    cold: %.1f paths/s would be %.2f ms/frame | warm: %.1f paths/s = %.2f ms/frame\n",
           cs.avg ? 1.0 / cs.avg * 1000.0 : 0.0, cs.avg * 10.0, ws.avg ? 1.0 / ws.avg * 1000.0 : 0.0, ws.avg * 1000.0);
}

static void RunFind(CNavMesh &m)
{
    printf("\n== FindArea / FindNearestArea (spatial grid) ==\n");
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f, minZ = 1e30f, maxZ = -1e30f;
    for (int i = 0; i < m.Count(); i++)
    {
        const NavAreaData *a = m.Area(i);
        if (a->lo.x < minX) minX = a->lo.x;
        if (a->lo.y < minY) minY = a->lo.y;
        if (a->hi.x > maxX) maxX = a->hi.x;
        if (a->hi.y > maxY) maxY = a->hi.y;
        if (a->lo.z < minZ) minZ = a->lo.z;
        if (a->hi.z > maxZ) maxZ = a->hi.z;
    }
    std::vector<Vector> pts;
    const int N = 100000;
    for (int i = 0; i < N; i++)
    {
        float px = minX + (float)(Rand() % 65536) / 65535.0f * (maxX - minX);
        float py = minY + (float)(Rand() % 65536) / 65535.0f * (maxY - minY);
        float pz = minZ + (float)(Rand() % 65536) / 65535.0f * (maxZ - minZ);
        pts.push_back(Vector(px, py, pz));
    }
    volatile int sink = 0;
    const int batch = 100;
    std::vector<double> areaSamples, nearestSamples;
    double t0 = NowMs();
    sink += m.FindArea(pts[0]);
    const double areaCold = (NowMs() - t0) * 1000.0;
    for (int i = 0; i < N; i += batch)
    {
        t0 = NowMs();
        for (int k = 0; k < batch; k++) sink += m.FindArea(pts[i + k]);
        areaSamples.push_back((NowMs() - t0) * 1000.0 / batch);
    }
    t0 = NowMs();
    sink += m.FindNearestArea(pts[0]);
    const double nearestCold = (NowMs() - t0) * 1000.0;
    for (int i = 0; i < N; i += batch)
    {
        t0 = NowMs();
        for (int k = 0; k < batch; k++) sink += m.FindNearestArea(pts[i + k]);
        nearestSamples.push_back((NowMs() - t0) * 1000.0 / batch);
    }
    SampleStats area = Compute(areaSamples), nearest = Compute(nearestSamples);
    printf("FindArea: cold=%.3f us | warm avg=%.3f med=%.3f p95=%.3f max=%.3f us/call\n",
           areaCold, area.avg, area.med, area.p95, area.max);
    printf("FindNearestArea: cold=%.3f us | warm avg=%.3f med=%.3f p95=%.3f max=%.3f us/call\n",
           nearestCold, nearest.avg, nearest.med, nearest.p95, nearest.max);
    printf("    sink=%d, samples=%d batches of %d\n", sink, (int)areaSamples.size(), batch);
}

static void RunReachable(CNavMesh &m, int iters)
{
    std::vector<double> ms;
    int ok = 0, fail = 0;
    for (int i = 0; i < iters; i++)
    {
        int s = (int)(Rand() % (unsigned)m.Count());
        int g = (int)(Rand() % (unsigned)m.Count());
        double t0 = NowMs();
        int r = m.FindClosestReachableArea(s, m.Area(g)->center);
        double dt = NowMs() - t0;
        if (r >= 0) ok++; else fail++;
        ms.push_back(dt);
    }
    SampleStats s = Compute(ms);
    printf("reachable-area: ok=%d fail=%d ms avg=%.4f med=%.4f p95=%.4f max=%.4f\n",
           ok, fail, s.avg, s.med, s.p95, s.max);
}

static void MeshSummary(CNavMesh &m)
{
    long long conns = 0;
    int maxConn = 0;
    for (int i = 0; i < m.Count(); i++)
    {
        conns += m.Area(i)->connCount;
        if (m.Area(i)->connCount > maxConn) maxConn = m.Area(i)->connCount;
    }
    printf("mesh: %d areas, %d ladders, %d sniper spots, %lld connections (avg %.1f, max %d)\n",
           m.Count(), m.LadderCount(), m.GetTotalSniperSpotCount(), conns,
           m.Count() ? (double)conns / m.Count() : 0.0, maxConn);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "ins_sinjar.nav";
    const char *scenario = argc > 2 ? argv[2] : "all";
    int iters = argc > 3 ? atoi(argv[3]) : 800;
    if (iters < 50) iters = 50;

    CNavMesh m;
    double t0 = NowMs();
    if (!m.Load(path)) { printf("FAIL: load %s\n", path); return 1; }
    printf("loaded %s in %.1f ms\n", path, NowMs() - t0);
    MeshSummary(m);

    if (strcmp(scenario, "long") == 0 || strcmp(scenario, "all") == 0)
    {
        printf("\n== BuildPath: long-haul random pairs (seed jitter) ==\n");
        RunBuildPath(m, iters, 0, false, "long-haul");
    }
    if (strcmp(scenario, "local") == 0 || strcmp(scenario, "all") == 0)
    {
        printf("\n== BuildPath: local pairs (<1500u, seed jitter) ==\n");
        RunBuildPath(m, iters, 1, false, "local");
    }
    if (strcmp(scenario, "obj") == 0 || strcmp(scenario, "all") == 0)
    {
        printf("\n== BuildPath: starts -> 4 objective-like goals (seed jitter) ==\n");
        RunBuildPath(m, iters, 2, false, "objective-cluster");
    }
    if (strcmp(scenario, "danger") == 0 || strcmp(scenario, "all") == 0)
    {
        printf("\n== BuildPath: mixed with 4 firefight danger zones (capture-committed) ==\n");
        RunBuildPath(m, iters, 0, true, "danger-zones");
    }
    if (strcmp(scenario, "mixed") == 0)
    {
        printf("\n== BuildPath: mixed (70%% long / 20%% local / 10%% obj) ==\n");
        std::vector<double> ms, exp, push, len;
        int ok = 0, fail = 0;
        int route[512];
        std::vector<int> goals;
        for (int tries = 0; tries < 200000 && (int)goals.size() < 4; tries++)
        {
            int g = (int)(Rand() % (unsigned)m.Count());
            bool good = true;
            for (size_t k = 0; k < goals.size(); k++)
                if (m.Area(goals[k])->center.Dist2D(m.Area(g)->center) < 4000.0f) { good = false; break; }
            if (good) goals.push_back(g);
        }
        for (int i = 0; i < iters; i++)
        {
            int roll = i % 10;
            int kind = roll < 7 ? 0 : (roll < 9 ? 1 : 2);
            int s = (int)(Rand() % (unsigned)m.Count());
            int g;
            if (kind == 2 && !goals.empty())
                g = goals[Rand() % (unsigned)goals.size()];
            else if (kind == 1)
            {
                const NavAreaData *a = m.Area(s);
                g = -1;
                for (int t = 0; t < 80; t++)
                {
                    int c = (int)(Rand() % (unsigned)m.Count());
                    const NavAreaData *b = m.Area(c);
                    if (a->center.Dist2D(b->center) <= 1500.0f && fabsf(a->center.z - b->center.z) <= 200.0f)
                    { g = c; break; }
                }
                if (g < 0) g = (int)(Rand() % (unsigned)m.Count());
            }
            else
                g = (int)(Rand() % (unsigned)m.Count());
            int seed = 1 + (i % 32);
            double t = NowMs();
            int n = m.BuildPath(s, g, route, 512, seed);
            double dt = NowMs() - t;
            if (n > 0) ok++; else fail++;
            unsigned mark = m.m_mark;
            double ec = 0, pc = 0;
            for (int k = 0; k < m.m_count; k++)
            {
                if (m.m_areas[k].closedMark == mark) ec++;
                if (m.m_areas[k].openMark == mark) pc++;
            }
            ms.push_back(dt); exp.push_back(ec); push.push_back(pc); len.push_back(n > 0 ? n : 0);
        }
        PrintStats("mixed", ms, exp, push, len, ok, fail);
    }
    if (strcmp(scenario, "cost") == 0 || strcmp(scenario, "all") == 0)
        RunCost(m, iters);
    if (strcmp(scenario, "find") == 0 || strcmp(scenario, "all") == 0)
        RunFind(m);
    if (strcmp(scenario, "reachable") == 0)
        RunReachable(m, iters);
    return 0;
}
