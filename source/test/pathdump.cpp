// pathdump.cpp - dump the A* route between two world positions on a real nav.
// usage: pathdump <map.nav> <sx> <sy> <sz> <gx> <gy> <gz>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

struct Vector { float x,y,z; Vector(){} Vector(float a,float b,float c):x(a),y(b),z(c){}
  Vector operator-(const Vector&o)const{return Vector(x-o.x,y-o.y,z-o.z);}
  Vector operator+(const Vector&o)const{return Vector(x+o.x,y+o.y,z+o.z);}
  float Length()const{return sqrtf(x*x+y*y+z*z);} };
#define private public
#define protected public
#define TESTING_NATIVE 1
#include "../src/nav.h"
#include "../src/nav.cpp"

static float PortalWidth(const CNavMesh &m, int a, int b)
{
    int li=-1; bool up=false;
    if (m.GetLadderConnection(a,b,li,up)) return 9999.0f;
    Vector mid, axis; float half=0;
    if (m.GetPortalMerged(a,b,mid,axis,half)) return half*2.0f;
    const NavAreaData *A=m.Area(a), *B=m.Area(b);
    if (!A||!B) return 9999.0f;
    float lx=A->lo.x>B->lo.x?A->lo.x:B->lo.x, hx=A->hi.x<B->hi.x?A->hi.x:B->hi.x;
    float ly=A->lo.y>B->lo.y?A->lo.y:B->lo.y, hy=A->hi.y<B->hi.y?A->hi.y:B->hi.y;
    float wx=hx-lx, wy=hy-ly;
    if (wx<0) wx=0; if (wy<0) wy=0;
    return wx>wy?wx:wy;
}

int main(int argc, char **argv)
{
    if (argc < 8) { printf("usage: pathdump <map.nav> sx sy sz gx gy gz\\n"); return 1; }
    CNavMesh m;
    if (!m.Load(argv[1])) { printf("FAIL load\\n"); return 1; }
    Vector start(atof(argv[2]), atof(argv[3]), atof(argv[4]));
    Vector goal(atof(argv[5]), atof(argv[6]), atof(argv[7]));
    int s = m.FindArea(start);
    if (s < 0) s = m.FindNearestArea(start);
    int g = m.FindArea(goal);
    if (g < 0) g = m.FindNearestArea(goal);
    printf("mesh %d areas; start=(%.1f %.1f %.1f) area=%d; goal=(%.1f %.1f %.1f) area=%d\\n",
           m.Count(), start.x, start.y, start.z, s, goal.x, goal.y, goal.z, g);
    if (s < 0 || g < 0) return 1;
    int path[512];
    int n = m.BuildPath(s, g, path, 512, 0);
    printf("path length %d\\n", n);

    if (n == 0)
    {
        int proxy = m.FindClosestReachableArea(s, goal);
        const NavAreaData *pa = proxy >= 0 ? m.Area(proxy) : 0;
        printf("unreachable direct; reachable proxy area=%d", proxy);
        if (pa) printf(" id=%u center=(%.1f %.1f %.1f)", pa->id, pa->center.x, pa->center.y, pa->center.z);
        printf("\n");
        int pn = 0;
        if (proxy >= 0) pn = m.BuildPath(s, proxy, path, 512, 0);
        printf("proxy path length %d\n", pn);
    }


    double totalDist = 0.0;
    int tiny=0, tight=0, narrow=0, wide=0;
    float minW = 9999.0f;
    for (int i = 0; i < n; i++)
    {
        const NavAreaData *a = m.Area(path[i]);
        float w = a->hi.x - a->lo.x, h = a->hi.y - a->lo.y;
        const char *flags = "";
        if (a->attributes & NAVATTR_CROUCH) flags = "CROUCH";
        else if (a->attributes & NAVATTR_JUMP) flags = "JUMP";
        else if (a->attributes & NAVATTR_PRECISE) flags = "PRECISE";
        else if (a->attributes & NAVATTR_AVOID) flags = "AVOID";
        printf("%4d idx=%5d id=%-6u center=(%7.1f %7.1f %7.1f) size=(%5.1f x %5.1f) conn=%d %s\\n",
               i, path[i], a->id, a->center.x, a->center.y, a->center.z, w, h, a->connCount, flags);
        if (i + 1 < n)
        {
            int li = -1; bool up = false;
            bool ladder = m.GetLadderConnection(path[i], path[i+1], li, up);
            Vector mid, axis; float half = 0;
            bool portal = !ladder && m.GetPortalMerged(path[i], path[i+1], mid, axis, half);
            float pw = portal ? half*2.0f : PortalWidth(m, path[i], path[i+1]);
            totalDist += (m.Area(path[i+1])->center - a->center).Length();
            if (pw < 32.0f) tiny++; else if (pw < 56.0f) tight++; else if (pw < 100.0f) narrow++; else wide++;
            if (pw < minW) minW = pw;
            if (ladder)
                printf("     -> LADDER %d %s (len %.1f)\\n", li, up ? "up" : "down",
                       m.Ladder(li) ? m.Ladder(li)->length : 0.0f);
            else if (portal)
                printf("     -> portal width %.1f at (%.1f %.1f %.1f) axis=(%.1f %.1f %.1f)%s\\n",
                       half * 2.0f, mid.x, mid.y, mid.z, axis.x, axis.y, axis.z,
                       pw < 56.0f ? "  <<< NARROW" : "");
            else
                printf("     -> NO PORTAL (width fallback %.1f)  <<<\\n", pw);
        }
    }
    printf("SUMMARY: center-dist %.0f  portals: <32u=%d 32-55=%d 56-99=%d >=100=%d min=%.1f\\n",
           totalDist, tiny, tight, narrow, wide, minW);
    printf("NARROW EDGES (<56u):\\n");
    for (int i = 0; i < n-1; i++)
    {
        float pw = PortalWidth(m, path[i], path[i+1]);
        if (pw < 56.0f)
        {
            const NavAreaData *a = m.Area(path[i]), *b = m.Area(path[i+1]);
            printf("  %3d -> %3d  width %.1f  at (%7.1f %7.1f %7.1f) -> (%7.1f %7.1f %7.1f)\\n",
                   path[i], path[i+1], pw, a->center.x, a->center.y, a->center.z,
                   b->center.x, b->center.y, b->center.z);
        }
    }
    return 0;
}
