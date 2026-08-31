// pathwide.cpp - compare normal A* vs a wide-route A* between two points.
// usage: pathwide <map.nav> sx sy sz gx gy gz [narrowPenalty]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

struct Vector { float x,y,z; Vector(){} Vector(float a,float b,float c):x(a),y(b),z(c){}
  Vector operator-(const Vector&o)const{return Vector(x-o.x,y-o.y,z-o.z);}
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
    // fallback: raw overlap along the shorter axis
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
    if (argc<8) { printf("usage: pathwide <map.nav> sx sy sz gx gy gz [narrowPenalty]\n"); return 1; }
    CNavMesh m;
    if (!m.Load(argv[1])) { printf("FAIL load\n"); return 1; }
    Vector start(atof(argv[2]), atof(argv[3]), atof(argv[4]));
    Vector goal(atof(argv[5]), atof(argv[6]), atof(argv[7]));
    float narrowPenalty = argc>8 ? (float)atof(argv[8]) : 1200.0f;
    int s=m.FindArea(start); if(s<0) s=m.FindNearestArea(start);
    int g=m.FindArea(goal);  if(g<0) g=m.FindNearestArea(goal);
    printf("mesh %d areas; start area=%d goal area=%d\n", m.Count(), s, g);
    if (s<0||g<0) return 1;

    int nrm[512], wide[512];
    int nN = m.BuildPath(s,g,nrm,512,0);
    double normDist=0;
    for (int i=0;i<nN-1;i++) normDist += (m.Area(nrm[i+1])->center - m.Area(nrm[i])->center).Length();
    printf("normal path: %d areas, center-dist %.0f\n", nN, normDist);
    int nTiny=0, nTight=0, nNarrow=0, nNorm=0;
    float minPortal=9999;
    for (int i=0;i<nN-1;i++)
    {
        float w=PortalWidth(m,nrm[i],nrm[i+1]);
        if (w<32) nTiny++; else if (w<56) nTight++; else if (w<100) nNarrow++; else nNorm++;
        if (w<minPortal) minPortal=w;
    }
    printf("  portals: <32u=%d  32-55=%d  56-99=%d  >=100=%d  (min %.1f)\n",
           nTiny,nTight,nNarrow,nNorm,minPortal);

    // Dijkstra preferring wide openings: cost = distance + penalty per narrow edge
    const int N=m.Count();
    float *dist=new float[N];
    int *prev=new int[N];
    bool *done=new bool[N];
    for (int i=0;i<N;i++) { dist[i]=3.4e38f; prev[i]=-1; done[i]=false; }
    dist[s]=0;
    for (;;)
    {
        int u=-1; float bd=3.4e38f;
        for (int i=0;i<N;i++) if(!done[i] && dist[i]<bd) { bd=dist[i]; u=i; }
        if (u<0) break;
        done[u]=true;
        if (u==g) break;
        const NavAreaData *A=m.Area(u);
        if (!A) continue;
        for (int k=0;k<A->connCount;k++)
        {
            int v=A->connIdx[k];
            if (v<0||v>=N||done[v]) continue;
            Vector step=m.Area(v)->center - A->center;
            float cost=step.Length();
            float w=PortalWidth(m,u,v);
            if (w<32.0f) cost += narrowPenalty*4.0f;
            else if (w<56.0f) cost += narrowPenalty*2.0f;
            else if (w<100.0f) cost += narrowPenalty;
            if (A->attributes & NAVATTR_CROUCH) cost += g_navCrouchPenalty;
            if (A->attributes & NAVATTR_JUMP) cost += g_navJumpPenalty;
            if (A->attributes & NAVATTR_AVOID) cost += g_navAvoidPenalty;
            if (dist[u]+cost < dist[v]) { dist[v]=dist[u]+cost; prev[v]=u; }
        }
    }
    if (dist[g]>=3.4e37f) { printf("wide-route: unreachable with penalties\n"); return 0; }
    int cnt=0;
    for (int c=g; c!=-1 && cnt<512; c=prev[c]) wide[cnt++]=c;
    // reverse
    for (int i=0;i<cnt/2;i++) { int t=wide[i]; wide[i]=wide[cnt-1-i]; wide[cnt-1-i]=t; }
    double wideDist=0;
    for (int i=0;i<cnt-1;i++) wideDist += (m.Area(wide[i+1])->center - m.Area(wide[i])->center).Length();
    printf("wide-route: %d areas (cost %.0f, center-dist %.0f, +%.0f%%)\n", cnt, dist[g], wideDist, (wideDist-normDist)*100.0/(normDist>0?normDist:1));
    int wTiny=0,wTight=0,wNarrow=0,wNorm=0;
    float wMin=9999;
    for (int i=0;i<cnt-1;i++)
    {
        float w=PortalWidth(m,wide[i],wide[i+1]);
        if (w<32) wTiny++; else if (w<56) wTight++; else if (w<100) wNarrow++; else wNorm++;
        if (w<wMin) wMin=w;
    }
    printf("  portals: <32u=%d  32-55=%d  56-99=%d  >=100=%d  (min %.1f)\n",
           wTiny,wTight,wNarrow,wNorm,wMin);
    printf("wide path areas:");
    for (int i=0;i<cnt;i++) printf(" %d", wide[i]);
    printf("\n");
    delete[] dist; delete[] prev; delete[] done;
    return 0;
}
