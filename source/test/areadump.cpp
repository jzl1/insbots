// areadump.cpp - inspect specific nav areas by id and their connections.
// usage: areadump <map.nav> <id> [<id>...]
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
    const NavAreaData *A=m.Area(a), *B=m.Area(b);
    if (!A||!B) return 9999.0f;
    float lx=A->lo.x>B->lo.x?A->lo.x:B->lo.x, hx=A->hi.x<B->hi.x?A->hi.x:B->hi.x;
    float ly=A->lo.y>B->lo.y?A->lo.y:B->lo.y, hy=A->hi.y<B->hi.y?A->hi.y:B->hi.y;
    float wx=hx-lx, wy=hy-ly;
    if (wx<0) wx=0; if (wy<0) wy=0;
    return wx>wy?wx:wy;
}

static int IndexOfId(const CNavMesh &m, unsigned int id)
{
    for (int i=0;i<m.Count();i++) if (m.Area(i)->id==id) return i;
    return -1;
}

int main(int argc, char **argv)
{
    if (argc<3) { printf("usage: areadump <map.nav> <id> [id...]\n"); return 1; }
    CNavMesh m;
    if (!m.Load(argv[1])) { printf("FAIL load\n"); return 1; }
    printf("mesh %d areas\n", m.Count());
    for (int a=2;a<argc;a++)
    {
        unsigned int id=(unsigned int)strtoul(argv[a],0,10);
        int idx=IndexOfId(m,id);
        printf("\n=== AREA ID %u (idx %d) ===\n", id, idx);
        if (idx<0) continue;
        const NavAreaData *A=m.Area(idx);
        float w=A->hi.x-A->lo.x, h=A->hi.y-A->lo.y;
        const char *flags="";
        if (A->attributes & NAVATTR_CROUCH) flags="CROUCH";
        else if (A->attributes & NAVATTR_JUMP) flags="JUMP";
        else if (A->attributes & NAVATTR_PRECISE) flags="PRECISE";
        else if (A->attributes & NAVATTR_AVOID) flags="AVOID";
        printf("center=(%.1f %.1f %.1f) lo=(%.1f %.1f %.1f) hi=(%.1f %.1f %.1f) size=(%.1f x %.1f) conn=%d %s\n",
               A->center.x,A->center.y,A->center.z,A->lo.x,A->lo.y,A->lo.z,
               A->hi.x,A->hi.y,A->hi.z,w,h,A->connCount,flags);
        printf("corner-z nw=%.3f ne=%.3f sw=%.3f se=%.3f\n",
               A->lo.z,A->neZ,A->swZ,A->hi.z);
        for (int k=0;k<A->connCount;k++)
        {
            int nb=A->connIdx[k];
            const NavAreaData *B=m.Area(nb);
            if (!B) continue;
            int li=-1; bool up=false; bool ladder=m.GetLadderConnection(idx,nb,li,up);
            float pw=PortalWidth(m,idx,nb);
            const char *nflags="";
            if (B->attributes & NAVATTR_CROUCH) nflags=" CROUCH";
            else if (B->attributes & NAVATTR_JUMP) nflags=" JUMP";
            else if (B->attributes & NAVATTR_PRECISE) nflags=" PRECISE";
            printf("  -> id=%-6u idx=%5d center=(%7.1f %7.1f %7.1f) size=(%5.1f x %5.1f) portal=%.1f%s%s\n",
                   B->id,nb,B->center.x,B->center.y,B->center.z,
                   B->hi.x-B->lo.x,B->hi.y-B->lo.y,pw,nflags,
                   ladder ? (up?" LADDER_UP":" LADDER_DOWN") : "");
        }
    }
    return 0;
}
