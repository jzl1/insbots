// native test harness: synthesize a v9 nav file, parse it, run A*
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
#undef private
#undef protected

static void put32(FILE*f,unsigned v){fwrite(&v,4,1,f);} 
static void put16(FILE*f,unsigned short v){fwrite(&v,2,1,f);} 
static void put8(FILE*f,unsigned char v){fwrite(&v,1,1,f);} 
static void putf(FILE*f,float v){fwrite(&v,4,1,f);} 

static float PathCostByEdges(const CNavMesh &m, const int *path, int length)
{
    float total = 0.0f;
    for (int i = 0; i + 1 < length; i++)
    {
        const NavAreaData *a = m.Area(path[i]);
        int slot = -1;
        for (int k = 0; k < a->connCount; k++)
            if (a->connIdx[k] == path[i + 1]) { slot = k; break; }
        if (slot < 0) return -1.0f;
        total += m.EdgeCost(path[i], path[i + 1], slot, 0);
    }
    return total;
}

static void WriteAreaV9(FILE *f, unsigned int id,
                        float x0, float y0, float z0,
                        float x1, float y1, float z1,
                        const unsigned int conn[4][3], const int connCounts[4])
{
    put32(f, id); put16(f, 0);
    putf(f, x0); putf(f, y0); putf(f, z0);
    putf(f, x1); putf(f, y1); putf(f, z1);
    putf(f, z1); putf(f, z0);                    // neZ, swZ
    for (int d = 0; d < 4; d++)
    {
        put32(f, (unsigned)connCounts[d]);
        for (int k = 0; k < connCounts[d]; k++) put32(f, conn[d][k]);
    }
    put8(f, 0); put8(f, 0); put32(f, 0); put16(f, 0);
    put32(f, 0); put32(f, 0);
    putf(f, 1.0f); putf(f, 2.0f);
}

static bool WriteEdgeGeometryFixture(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    put32(f, 0xFEEDFACE); put32(f, 9); put32(f, 12345);
    put16(f, 0);
    put32(f, 14);

    unsigned int conn[4][3];
    int counts[4];
    memset(conn, 0, sizeof(conn));
    memset(counts, 0, sizeof(counts));

    // Adjacent walk edge (indices 0 -> 1).
    conn[NAV_EAST][0] = 102; counts[NAV_EAST] = 1;
    WriteAreaV9(f, 101, 0, 0, 0, 100, 100, 0, conn, counts);
    memset(conn, 0, sizeof(conn)); memset(counts, 0, sizeof(counts));
    conn[NAV_WEST][0] = 101; counts[NAV_WEST] = 1;
    WriteAreaV9(f, 102, 100, 0, 0, 200, 100, 0, conn, counts);

    // Split doorway slices plus a stacked upper slice at the same plane.
    // The upper slice must not join the merged walk span.
    memset(conn, 0, sizeof(conn)); memset(counts, 0, sizeof(counts));
    conn[NAV_EAST][0] = 202; conn[NAV_EAST][1] = 203; conn[NAV_EAST][2] = 204;
    counts[NAV_EAST] = 3;
    WriteAreaV9(f, 201, 0, 200, 0, 100, 300, 0, conn, counts);
    memset(conn, 0, sizeof(conn)); memset(counts, 0, sizeof(counts));
    conn[NAV_WEST][0] = 201; counts[NAV_WEST] = 1;
    WriteAreaV9(f, 202, 100, 200, 0, 200, 250, 0, conn, counts);
    WriteAreaV9(f, 203, 100, 250, 0, 200, 300, 0, conn, counts);
    WriteAreaV9(f, 204, 100, 200, 100, 200, 300, 100, conn, counts);

    // Fully stacked floors (indices 6 <-> 7): jump upward, drop downward.
    memset(conn, 0, sizeof(conn)); memset(counts, 0, sizeof(counts));
    conn[NAV_EAST][0] = 302; counts[NAV_EAST] = 1;
    WriteAreaV9(f, 301, 300, 0, 0, 400, 100, 0, conn, counts);
    memset(conn, 0, sizeof(conn)); memset(counts, 0, sizeof(counts));
    conn[NAV_WEST][0] = 301; counts[NAV_WEST] = 1;
    WriteAreaV9(f, 302, 300, 0, 100, 400, 100, 100, conn, counts);

    // Separated short drop (indices 8 -> 9).
    memset(conn, 0, sizeof(conn)); memset(counts, 0, sizeof(counts));
    conn[NAV_EAST][0] = 402; counts[NAV_EAST] = 1;
    WriteAreaV9(f, 401, 500, 0, 100, 600, 50, 100, conn, counts);
    memset(conn, 0, sizeof(conn)); memset(counts, 0, sizeof(counts));
    conn[NAV_WEST][0] = 401; counts[NAV_WEST] = 1;
    WriteAreaV9(f, 402, 620, 0, 0, 720, 50, 0, conn, counts);

    // Fully overlapping same-elevation rectangles are malformed, not walk.
    memset(conn, 0, sizeof(conn)); memset(counts, 0, sizeof(counts));
    conn[NAV_EAST][0] = 502; counts[NAV_EAST] = 1;
    WriteAreaV9(f, 501, 800, 0, 0, 900, 100, 0, conn, counts);
    memset(conn, 0, sizeof(conn)); memset(counts, 0, sizeof(counts));
    conn[NAV_WEST][0] = 501; counts[NAV_WEST] = 1;
    WriteAreaV9(f, 502, 800, 0, 0, 900, 100, 0, conn, counts);

    // Shallow fully stacked drop (indices 12 -> 13). The upper floor is only
    // 40u above the landing, inside the old +/-64u surface tolerance.
    memset(conn, 0, sizeof(conn)); memset(counts, 0, sizeof(counts));
    conn[NAV_EAST][0] = 602; counts[NAV_EAST] = 1;
    WriteAreaV9(f, 601, 1000, 0, 40, 1100, 100, 40, conn, counts);
    memset(conn, 0, sizeof(conn)); memset(counts, 0, sizeof(counts));
    conn[NAV_WEST][0] = 601; counts[NAV_WEST] = 1;
    WriteAreaV9(f, 602, 1000, 0, 0, 1100, 100, 0, conn, counts);

    put32(f, 0); // no ladders
    fclose(f);
    return true;
}

static bool WriteDuplicateIdFixture(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    put32(f, 0xFEEDFACE); put32(f, 9); put32(f, 12345);
    put16(f, 0); put32(f, 2);
    unsigned int conn[4][3];
    int counts[4];
    memset(conn, 0, sizeof(conn)); memset(counts, 0, sizeof(counts));
    WriteAreaV9(f, 7, 0, 0, 0, 100, 100, 0, conn, counts);
    WriteAreaV9(f, 7, 200, 0, 0, 300, 100, 0, conn, counts);
    put32(f, 0);
    fclose(f);
    return true;
}

static bool WriteIdResolutionFixture(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    put32(f, 0xFEEDFACE); put32(f, 9); put32(f, 12345);
    put16(f, 0); put32(f, 3);
    unsigned int conn[4][3];
    int counts[4];
    memset(conn, 0, sizeof(conn)); memset(counts, 0, sizeof(counts));
    conn[NAV_EAST][0] = 200;
    conn[NAV_EAST][1] = 999;   // missing area
    conn[NAV_EAST][2] = 100;   // self reference
    counts[NAV_EAST] = 3;
    WriteAreaV9(f, 100, 0, 0, 0, 100, 100, 0, conn, counts);
    memset(conn, 0, sizeof(conn)); memset(counts, 0, sizeof(counts));
    conn[NAV_WEST][0] = 100; counts[NAV_WEST] = 1;
    WriteAreaV9(f, 200, 100, 0, 0, 200, 100, 0, conn, counts);
    memset(conn, 0, sizeof(conn)); memset(counts, 0, sizeof(counts));
    WriteAreaV9(f, 300, 400, 0, 0, 500, 100, 0, conn, counts);

    // One ladder whose bottom endpoint is not present in the area table.
    put32(f, 1);
    put32(f, 900); putf(f, 32.0f);
    putf(f, 450); putf(f, 50); putf(f, 0);
    putf(f, 450); putf(f, 50); putf(f, 100);
    putf(f, 100); put32(f, NAV_NORTH);
    put32(f, 300); put32(f, 0); put32(f, 0); put32(f, 0); put32(f, 999);
    fclose(f);
    return true;
}

// A mesh whose area declares more connections than the loader retains must be
// rejected outright instead of being silently truncated to 512.

static bool WriteForkFixture(const char *path, unsigned short area2Attr)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    put32(f, 0xFEEDFACE); put32(f, 9); put32(f, 12345);
    put16(f, 0); put32(f, 4);
    put32(f, 1); put16(f, 0);
    putf(f, -25); putf(f, -25); putf(f, 0); putf(f, 25); putf(f, 25); putf(f, 0);
    putf(f, 0); putf(f, 0);
    put32(f, 1); put32(f, 2); put32(f, 1); put32(f, 3); put32(f, 0); put32(f, 0);
    put8(f, 0); put8(f, 0); put32(f, 0); put16(f, 0); put32(f, 0); put32(f, 0); putf(f, 0); putf(f, 0);
    put32(f, 2); put16(f, area2Attr);
    putf(f, -25); putf(f, 75); putf(f, 0); putf(f, 25); putf(f, 125); putf(f, 0);
    putf(f, 0); putf(f, 0);
    put32(f, 1); put32(f, 4); put32(f, 0); put32(f, 0); put32(f, 0);
    put8(f, 0); put8(f, 0); put32(f, 0); put16(f, 0); put32(f, 0); put32(f, 0); putf(f, 0); putf(f, 0);
    put32(f, 3); put16(f, 0);
    putf(f, 225); putf(f, 25); putf(f, 0); putf(f, 275); putf(f, 75); putf(f, 0);
    putf(f, 0); putf(f, 0);
    put32(f, 1); put32(f, 4); put32(f, 0); put32(f, 0); put32(f, 0);
    put8(f, 0); put8(f, 0); put32(f, 0); put16(f, 0); put32(f, 0); put32(f, 0); putf(f, 0); putf(f, 0);
    put32(f, 4); put16(f, 0);
    putf(f, -25); putf(f, 175); putf(f, 0); putf(f, 25); putf(f, 225); putf(f, 0);
    putf(f, 0); putf(f, 0);
    put32(f, 0); put32(f, 0); put32(f, 0); put32(f, 0);
    put8(f, 0); put8(f, 0); put32(f, 0); put16(f, 0); put32(f, 0); put32(f, 0); putf(f, 0); putf(f, 0);
    put32(f, 0);
    fclose(f);
    return true;
}

static bool WriteTwoAreaFixture(const char *path, float spacing)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    put32(f, 0xFEEDFACE); put32(f, 9); put32(f, 12345);
    put16(f, 0); put32(f, 2);
    put32(f, 1); put16(f, 0);
    putf(f, -25); putf(f, -25); putf(f, 0); putf(f, 25); putf(f, 25); putf(f, 0);
    putf(f, 0); putf(f, 0);
    put32(f, 1); put32(f, 2); put32(f, 0); put32(f, 0); put32(f, 0);
    put8(f, 0); put8(f, 0); put32(f, 0); put16(f, 0); put32(f, 0); put32(f, 0);
    putf(f, 0); putf(f, 0);
    put32(f, 2); put16(f, 0);
    putf(f, spacing - 25); putf(f, -25); putf(f, 0);
    putf(f, spacing + 25); putf(f, 25); putf(f, 0);
    putf(f, 0); putf(f, 0);
    put32(f, 1); put32(f, 1); put32(f, 0); put32(f, 0); put32(f, 0);
    put8(f, 0); put8(f, 0); put32(f, 0); put16(f, 0); put32(f, 0); put32(f, 0);
    putf(f, 0); putf(f, 0);
    put32(f, 0);
    fclose(f);
    return true;
}

static bool WriteOverflowFixture(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    put32(f, 0xFEEDFACE); put32(f, 9); put32(f, 12345); // magic, version, bspsize
    put16(f, 0);                                        // no places
    put32(f, 2);                                        // areas
    for (unsigned int id = 1; id <= 2; id++)
    {
        put32(f, id); put16(f, 0);
        putf(f, 0); putf(f, 0); putf(f, 0);             // lo
        putf(f, 100); putf(f, 100); putf(f, 0);         // hi
        putf(f, 0); putf(f, 0);                         // neZ swZ
        for (int d = 0; d < 4; d++)
        {
            if (id == 1 && d == 0)
            {
                put32(f, 513);
                for (int k = 0; k < 513; k++) put32(f, 2);
            }
            else put32(f, 0);
        }
        put8(f, 0);                                     // hiding spots
        put8(f, 0);                                     // approach areas
        put32(f, 0);                                    // encounters
        put16(f, 0);                                    // place index
        put32(f, 0); put32(f, 0);                       // ladder up/down
        putf(f, 1.f); putf(f, 2.f);                     // occupy times
    }
    put32(f, 0);                                        // ladders
    fclose(f);
    return true;
}
// A linear chain of areas long enough that no single 512-node caller buffer
// can hold the whole route. Exercises the two-pass truncated-prefix path.
static bool WriteChainFixture(const char *path, int count)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    put32(f, 0xFEEDFACE); put32(f, 9); put32(f, 12345); // magic, version, bspsize
    put16(f, 0);                                        // no places
    put32(f, (unsigned)count);
    for (int i = 0; i < count; i++)
    {
        put32(f, (unsigned)(i + 1)); put16(f, 0);
        putf(f, i * 50.0f); putf(f, 0); putf(f, 0);         // lo
        putf(f, i * 50.0f + 50.0f); putf(f, 50.0f); putf(f, 0); // hi
        putf(f, 0); putf(f, 0);                             // neZ swZ
        for (int d = 0; d < 4; d++)
        {
            if (d == 1 && i + 1 < count) { put32(f, 1); put32(f, (unsigned)(i + 2)); }
            else put32(f, 0);
        }
        put8(f, 0);                                         // hiding spots
        put8(f, 0);                                         // approach areas
        put32(f, 0);                                        // encounters
        put16(f, 0);                                        // place index
        put32(f, 0); put32(f, 0);                           // ladder up/down
        putf(f, 1.f); putf(f, 2.f);                         // occupy times
    }
    put32(f, 0);                                            // ladders
    fclose(f);
    return true;
}
// One-way protected component: main 0 -> 1 -> 2, protected 3 -> 2, and no
// link from the main component back to 3. The desired point sits at area 3.
static bool WriteDirectedFixture(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    put32(f, 0xFEEDFACE); put32(f, 9); put32(f, 12345); // magic, version, bspsize
    put16(f, 0);                                        // no places
    put32(f, 4);                                        // areas
    for (unsigned int i = 0; i < 4; i++)
    {
        unsigned int id = i + 1;
        float x0 = (i == 3) ? 500.0f : i * 100.0f;
        put32(f, id); put16(f, 0);                       // id, attributes
        putf(f, x0);     putf(f, 0);    putf(f, 0);      // lo
        putf(f, x0+100); putf(f, 100);  putf(f, 0);      // hi
        putf(f, 0); putf(f, 0);                          // neZ swZ
        for (int d = 0; d < 4; d++)
        {
            unsigned int dst = 0;
            if (i == 0 && d == 1) dst = 2;      // east 0->1
            else if (i == 1 && d == 1) dst = 3; // east 1->2
            else if (i == 3 && d == 1) dst = 3; // protected 3->2
            if (dst) { put32(f, 1); put32(f, dst); }
            else put32(f, 0);
        }
        put8(f, 0);                                     // hiding spots
        put8(f, 0);                                     // approach areas
        put32(f, 0);                                    // encounters
        put16(f, 0);                                    // place index
        put32(f, 0); put32(f, 0);                       // ladder up/down
        putf(f, 1.f); putf(f, 2.f);                     // occupy times
    }
    put32(f, 0);                                        // ladders
    fclose(f);
    return true;
}

// Two isolated areas: no path exists and neither area shares a component.
static bool WriteDisconnectedFixture(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    put32(f, 0xFEEDFACE); put32(f, 9); put32(f, 12345); // magic, version, bspsize
    put16(f, 0);                                        // no places
    put32(f, 2);                                        // areas
    for (unsigned int i = 0; i < 2; i++)
    {
        put32(f, i + 1); put16(f, 0);
        putf(f, i * 100.0f); putf(f, 0); putf(f, 0);
        putf(f, i * 100.0f + 100.0f); putf(f, 100); putf(f, 0);
        putf(f, 0); putf(f, 0);
        for (int d = 0; d < 4; d++) put32(f, 0);
        put8(f, 0); put8(f, 0); put32(f, 0); put16(f, 0);
        put32(f, 0); put32(f, 0);
        putf(f, 1.f); putf(f, 2.f);
    }
    put32(f, 0); // no ladders
    fclose(f);
    return true;
}

int main(){
  // build a 3x3 grid of 100x100 areas, 4-connected
  const char *navPath="test_nav.tmp";
  FILE*f=fopen(navPath,"wb");
  put32(f,0xFEEDFACE); put32(f,9); put32(f,12345); // magic, version, bspsize
  put16(f,1); const char*pn="TestPlace"; put16(f,(unsigned short)(strlen(pn)+1)); fwrite(pn,1,strlen(pn)+1,f); // place dir
  put32(f,10); // area count: one misleading overlap followed by the 3x3 grid

  // Regression: this area overlaps area 1 in XY and has center.z=0, but its
  // actual sloped surface is z=40 at the test point (10,10,0). The old lookup
  // compared center.z and selected this first record. Source-style lookup must
  // reject its surface as above the bot and select real area 1 beneath him.
  put32(f,10); put16(f,0);
  putf(f,0);putf(f,0);putf(f,50);
  putf(f,100);putf(f,100);putf(f,-50);
  putf(f,50);putf(f,-50);
  for(int d=0;d<4;d++) put32(f,0);
  put8(f,0); put8(f,0); put32(f,0); put16(f,1);
  put32(f,2); put32(f,100); put32(f,101); // one valid + one invalid ladder up
  put32(f,0); putf(f,0); putf(f,0);

  for(int gy=0;gy<3;gy++)for(int gx=0;gx<3;gx++){
    unsigned id=gy*3+gx+1;
    put32(f,id); put16(f,(id==5)?0x01:0); // area 5 crouch
    putf(f,gx*100.f); putf(f,gy*100.f); putf(f,0.f);          // lo
    putf(f,gx*100.f+100.f); putf(f,gy*100.f+100.f); putf(f,0.f); // hi
    putf(f,0.f); putf(f,0.f);                                  // neZ swZ
    // connections NORTH(gy-1), EAST(gx+1), SOUTH(gy+1), WEST(gx-1)
    int cn[4][2]={{0,-1},{1,0},{0,1},{-1,0}};
    for(int d=0;d<4;d++){
      int nx=gx+cn[d][0], ny=gy+cn[d][1];
      if(nx>=0&&nx<3&&ny>=0&&ny<3){put32(f,1);put32(f,ny*3+nx+1);}else put32(f,0);
    }
    put8(f,1); put32(f,77); putf(f,5);putf(f,5);putf(f,0); put8(f,3); // 1 hiding spot (id,pos,flags)
    put8(f,1); put32(f,1);put32(f,2);put8(f,0);put32(f,3);put8(f,1);  // 1 approach area
    put32(f,1); put32(f,1);put8(f,0);put32(f,2);put8(f,1); put8(f,2); // 1 encounter, 2 spots
      put32(f,77);put8(f,128); put32(f,77);put8(f,200);
    put16(f,1); // place index
    if(id==9){ put32(f,0); put32(f,2); put32(f,100); put32(f,101); }
    else { put32(f,0); put32(f,0); }
    putf(f,1.f); putf(f,2.f); // occupy times
  }
  put32(f,2); // ladder count
  put32(f,100); putf(f,32.0f);                    // id, width
  putf(f,50); putf(f,50); putf(f,80);             // top (within 96u of landing)
  putf(f,50); putf(f,50); putf(f,0);              // bottom
  putf(f,80); put32(f,NAV_NORTH);                  // length, face direction
  put32(f,9); put32(f,0); put32(f,0); put32(f,0); put32(f,10); // endpoints
  put32(f,101); putf(f,32.0f);                    // invalid landing-height record
  putf(f,50); putf(f,50); putf(f,220);
  putf(f,50); putf(f,50); putf(f,0);
  putf(f,220); put32(f,NAV_NORTH);
  put32(f,9); put32(f,0); put32(f,0); put32(f,0); put32(f,10);
  fclose(f);

  CNavMesh m;
  if(!m.Load(navPath)){printf("FAIL: load\n");return 1;}
  printf("loaded %d areas\n",m.Count());
  if(m.Count()!=10){printf("FAIL: count\n");return 1;}
  int s=m.FindArea(Vector(10,10,0));       // area 1 (0,0)
  int g=m.FindArea(Vector(290,290,0));     // area 9 (2,2)
  printf("start=%d(id=%u) goal=%d(id=%u)\n",s,m.Area(s)->id,g,m.Area(g)->id);
  if(s<0 || m.Area(s)->id!=1){printf("FAIL: surface-area lookup\n");return 1;}
  int path[64]; int n=m.BuildPath(s,g,path,64);
  printf("path len=%d:",n); for(int i=0;i<n;i++)printf(" %d(id=%u)",path[i],m.Area(path[i])->id); printf("\n");
  if(n<5){printf("FAIL: path\n");return 1;}

  // Ladder records become typed directed A* edges without masquerading as
  // ordinary shared-edge portals.
  if(m.LadderCount()!=2){printf("FAIL: ladder count\n");return 1;}
  int top=m.FindArea(Vector(290,290,0));
  int ladderIdx=-1; bool ladderUp=false;
  if(!m.GetLadderConnection(0,top,ladderIdx,ladderUp)||ladderIdx!=0||!ladderUp){
    printf("FAIL: ladder up edge\n");return 1;
  }
  if(!m.GetLadderConnection(top,0,ladderIdx,ladderUp)||ladderIdx!=0||ladderUp){
    printf("FAIL: ladder down edge\n");return 1;
  }
  // Ladder 101 names the same areas but its top lip is 220u above the
  // destination surface. It must not overwrite the valid typed edge.
  if(ladderIdx==1){printf("FAIL: invalid ladder edge accepted\n");return 1;}
  Vector fakePortal;
  if(m.GetPortal(0,top,fakePortal)){printf("FAIL: ladder exposed as portal\n");return 1;}
  int ladderPath[8];
  if(m.BuildPath(0,top,ladderPath,8)!=2){printf("FAIL: ladder A* route\n");return 1;}

  // Confidence-based bad-link learning: one report is tentative, two reports
  // reroute, a successful crossing rehabilitates it, and only v2 data persists.
  int east=m.FindArea(Vector(190,10,0));
  if(!m.HasConnection(s,east)){printf("FAIL: expected east connection\n");return 1;}
  m.MarkBadConnection(s,east);
  if(m.BadCount(s,east)!=1){printf("FAIL: tentative confidence\n");return 1;}
  int direct[64]; int dn=m.BuildPath(s,east,direct,64);
  if(dn!=2 || direct[1]!=east){printf("FAIL: tentative link changed route\n");return 1;}
  m.MarkBadConnection(s,east);
  dn=m.BuildPath(s,east,direct,64);
  if(dn<=2 || direct[1]==east){printf("FAIL: confirmed link did not reroute\n");return 1;}
  m.MarkGoodConnection(s,east);
  dn=m.BuildPath(s,east,direct,64);
  if(m.BadCount(s,east)!=1 || dn!=2 || direct[1]!=east){printf("FAIL: link rehabilitation\n");return 1;}
  m.MarkBadConnection(s,east);
  const char *learnPath="test_learn.tmp";
  if(!m.SaveBadLinks(learnPath)){printf("FAIL: save learned links\n");return 1;}
  m.ClearBadLinks();
  if(m.LoadBadLinks(learnPath)!=1 || m.BadCount(s,east)!=2){printf("FAIL: load learned links v2\n");return 1;}
  remove(learnPath);

  // crouch attr survives?
  int mid=m.FindArea(Vector(150,150,0));
  printf("mid area attr=%x\n",m.Area(mid)->attributes);
  remove(navPath);
  // Long-route continuation: a chain far longer than the caller buffer must
  // return a true start-prefix with truncated=true, and chaining rebuilds from
  // the prefix end must eventually reach the original goal.
  const int CHAIN = 1301;
  const char *chainPath = "test_nav_chain.tmp";
  if (!WriteChainFixture(chainPath, CHAIN)) { printf("FAIL: write chain fixture\n"); return 1; }
  CNavMesh mc;
  if (!mc.Load(chainPath)) { printf("FAIL: chain load\n"); return 1; }
  if (mc.Count() != CHAIN) { printf("FAIL: chain count\n"); return 1; }
  {
      int buf[512];
      bool truncated = false;
      int n = mc.BuildPath(0, CHAIN - 1, buf, 512, 0, &truncated);
      if (n != 512) { printf("FAIL: chain first chunk len %d\n", n); return 1; }
      if (!truncated) { printf("FAIL: chain not marked truncated\n"); return 1; }
      if (buf[0] != 0) { printf("FAIL: chain prefix start\n"); return 1; }
      if (buf[511] != 511) { printf("FAIL: chain prefix end %d\n", buf[511]); return 1; }
      int cur = buf[511];
      int chunks = 1;
      for (; cur != CHAIN - 1 && chunks < 20; chunks++)
      {
          bool t2 = false;
          int m = mc.BuildPath(cur, CHAIN - 1, buf, 512, 0, &t2);
          if (m <= 0 || buf[0] != cur) { printf("FAIL: chain continuation chunk\n"); return 1; }
          cur = buf[m - 1];
      }
      if (cur != CHAIN - 1) { printf("FAIL: chain did not reach goal\n"); return 1; }
      remove(chainPath);
  }
  // Overflow rejection: one area declaring 513 serialized connections must
  // fail the load (and leave the mesh empty), never silently keep 512.
  const char *overflowPath = "test_nav_overflow.tmp";
  if (!WriteOverflowFixture(overflowPath)) { printf("FAIL: write overflow fixture\n"); return 1; }
  CNavMesh mo;
  if (mo.Load(overflowPath)) { printf("FAIL: 513-connection mesh accepted\n"); return 1; }
  if (mo.Count() != 0) { printf("FAIL: failed load left areas\n"); return 1; }
  remove(overflowPath);

  // Area-ID map: duplicate IDs are ambiguous and must reject the mesh.
  const char *duplicatePath = "test_nav_duplicate_ids.tmp";
  if (!WriteDuplicateIdFixture(duplicatePath)) { printf("FAIL: write duplicate fixture\n"); return 1; }
  CNavMesh mDup;
  if (mDup.Load(duplicatePath) || mDup.Count() != 0)
  { printf("FAIL: duplicate area IDs accepted\n"); return 1; }
  remove(duplicatePath);

  // Missing/self connections are skipped; an out-of-range ladder endpoint is
  // retained as unresolved metadata rather than indexed into m_areas.
  const char *idResolvePath = "test_nav_id_resolve.tmp";
  if (!WriteIdResolutionFixture(idResolvePath)) { printf("FAIL: write id-resolution fixture\n"); return 1; }
  CNavMesh mIds;
  if (!mIds.Load(idResolvePath) || mIds.Count() != 3)
  { printf("FAIL: id-resolution load\n"); return 1; }
  if (mIds.Area(0)->connCount != 1
      || mIds.Area(mIds.Area(0)->connIdx[0])->id != 200)
  { printf("FAIL: missing/self connection was retained\n"); return 1; }
  if (mIds.LadderCount() != 1 || mIds.Ladder(0)->bottomArea != -1
      || mIds.Ladder(0)->topForwardArea != 2)
  { printf("FAIL: out-of-range ladder endpoint resolution\n"); return 1; }
  const char *sparseBadPath = "test_nav_badlinks_sparse.tmp";
  FILE *badFile = fopen(sparseBadPath, "w");
  if (!badFile) { printf("FAIL: write sparse bad links\n"); return 1; }
  fprintf(badFile, "bad nav links v2\n100 200 2\n");
  fclose(badFile);
  if (mIds.LoadBadLinks(sparseBadPath) != 1 || mIds.BadCount(0, 1) != 2)
  { printf("FAIL: sparse LoadBadLinks did not use the id map\n"); return 1; }
  remove(sparseBadPath);
  remove(idResolvePath);

  // Transactional load: failures must leave the previous mesh usable, and a
  // successful replacement must still be able to restore learned links.
  const char *txGoodPath = "test_nav_tx_good.tmp";
  const char *txBadPath = "test_nav_tx_bad.tmp";
  const char *txReloadPath = "test_nav_tx_reload.tmp";
  const char *txLearnPath = "test_nav_tx_learn.tmp";
  CNavMesh mTxMissing;
  if (mTxMissing.Load("test_nav_missing.tmp") || mTxMissing.Count() != 0)
  { printf("FAIL: first-load failure left state\n"); return 1; }
  if (!WriteForkFixture(txGoodPath, 0)) { printf("FAIL: write tx fixture\n"); return 1; }
  CNavMesh mTx;
  if (!mTx.Load(txGoodPath) || mTx.Count() != 4)
  { printf("FAIL: tx initial load\n"); return 1; }
  int txPath[16];
  if (mTx.BuildPath(0, 3, txPath, 16) <= 0)
  { printf("FAIL: tx initial path\n"); return 1; }
  FILE *badTx = fopen(txBadPath, "wb");
  if (!badTx) { printf("FAIL: write corrupt tx\n"); return 1; }
  fwrite("this is not a nav mesh", 1, 23, badTx);
  fclose(badTx);
  if (mTx.Load(txBadPath) || mTx.Count() != 4
      || mTx.BuildPath(0, 3, txPath, 16) <= 0)
  { printf("FAIL: corrupt reload replaced good mesh\n"); return 1; }
  if (mTx.LastSearchStatus() != NAV_SEARCH_OK)
  { printf("FAIL: failed reload changed search status\n"); return 1; }
  mTx.MarkBadConnection(0, 1);
  mTx.MarkBadConnection(0, 1);
  if (!mTx.SaveBadLinks(txLearnPath)) { printf("FAIL: save tx learned links\n"); return 1; }
  if (!WriteForkFixture(txReloadPath, 0)) { printf("FAIL: write tx reload fixture\n"); return 1; }
  if (!mTx.Load(txReloadPath) || mTx.Count() != 4
      || mTx.LoadBadLinks(txLearnPath) != 1 || mTx.BadCount(0, 1) != 2)
  { printf("FAIL: successful reload/learned-link restore\n"); return 1; }
  remove(txGoodPath); remove(txBadPath); remove(txReloadPath); remove(txLearnPath);

  // Reload must invalidate cached path costs even when area indices still line
  // up, otherwise a query could reuse the previous mesh's geometry.
  const char *cacheNearPath = "test_nav_cache_near.tmp";
  const char *cacheFarPath = "test_nav_cache_far.tmp";
  if (!WriteTwoAreaFixture(cacheNearPath, 100.0f)
      || !WriteTwoAreaFixture(cacheFarPath, 500.0f))
  { printf("FAIL: write reload cache fixtures\n"); return 1; }
  CNavMesh mCacheReload;
  if (!mCacheReload.Load(cacheNearPath)) { printf("FAIL: near cache load\n"); return 1; }
  float nearCost = mCacheReload.GetPathCost(0, 1, 0);
  if (!mCacheReload.Load(cacheFarPath)) { printf("FAIL: far cache load\n"); return 1; }
  float farCost = mCacheReload.GetPathCost(0, 1, 0);
  if (fabsf(nearCost - 100.0f) > 0.01f || fabsf(farCost - 500.0f) > 0.01f)
  { printf("FAIL: stale cost across reload near=%.3f far=%.3f\n", nearCost, farCost); return 1; }
  remove(cacheNearPath); remove(cacheFarPath);

    // Directed reachability frontier: the protected component is one-way into
  // the main component, so the closest reachable area to it is area 2.
  const char *dirPath = "test_nav_directed.tmp";
  if (!WriteDirectedFixture(dirPath)) { printf("FAIL: write directed fixture\n"); return 1; }
  CNavMesh md;
  if (!md.Load(dirPath)) { printf("FAIL: directed load\n"); return 1; }
  if (md.Count() != 4) { printf("FAIL: directed count\n"); return 1; }
  {
      int dpath[16];
      if (md.BuildPath(0, 3, dpath, 16) != 0) { printf("FAIL: protected route must be zero\n"); return 1; }
      if (md.LastSearchStatus() != NAV_SEARCH_UNREACHABLE)
      { printf("FAIL: unreachable search status\n"); return 1; }
      Vector desired(550.f, 50.f, 0.f);
      int proxy = md.FindClosestReachableArea(0, desired);
      if (proxy != 2) { printf("FAIL: reachable proxy=%d\n", proxy); return 1; }
      int pn = md.BuildPath(0, proxy, dpath, 16);
      if (pn <= 0 || dpath[pn - 1] != 2) { printf("FAIL: proxy path len=%d\n", pn); return 1; }
      int ps = md.FindClosestReachableArea(3, desired);
      if (ps != 3) { printf("FAIL: protected start proxy=%d\n", ps); return 1; }
  }
  remove(dirPath);
    // Component fast-reject: a disconnected goal must not enter A*.
  const char *discPath = "test_nav_disconnected.tmp";
  if (!WriteDisconnectedFixture(discPath)) { printf("FAIL: write disconnected fixture\n"); return 1; }
  CNavMesh mDisc;
  if (!mDisc.Load(discPath)) { printf("FAIL: disconnected load\n"); return 1; }
  {
      unsigned int markBefore = mDisc.m_mark;
      int discRoute[8];
      if (mDisc.BuildPath(0, 1, discRoute, 8) != 0)
      { printf("FAIL: disconnected path returned a route\n"); return 1; }
      if (mDisc.LastSearchStatus() != NAV_SEARCH_COMPONENT_MISMATCH)
      { printf("FAIL: component-mismatch path status\n"); return 1; }
      if (mDisc.m_mark != markBefore)
      { printf("FAIL: disconnected BuildPath entered A*\n"); return 1; }
      if (mDisc.GetPathCost(0, 1, 0) >= 0.0f)
      { printf("FAIL: disconnected cost was reachable\n"); return 1; }
      if (mDisc.LastSearchStatus() != NAV_SEARCH_COMPONENT_MISMATCH)
      { printf("FAIL: component-mismatch cost status\n"); return 1; }
      if (mDisc.m_mark != markBefore)
      { printf("FAIL: disconnected GetPathCost entered A*\n"); return 1; }
  }
  remove(discPath);
    // Attribute semantics: NO_JUMP denies generic jump output from that area;
    // PRECISE and UNDERWATER remain parsed and documented (the latter drives
    // the bot water-speed path in bot_think.cpp).
  const char *noJumpPath = "test_nav_no_jump.tmp";
  if (!WriteForkFixture(noJumpPath, NAVATTR_NO_JUMP)) { printf("FAIL: write no-jump fixture\n"); return 1; }
  CNavMesh mNoJump;
  if (!mNoJump.Load(noJumpPath)) { printf("FAIL: no-jump load\n"); return 1; }
  if (mNoJump.CanJumpFrom(1) || !mNoJump.CanJumpFrom(0))
  { printf("FAIL: NO_JUMP permission\n"); return 1; }
  remove(noJumpPath);
  const char *flagsPath = "test_nav_flags.tmp";
  if (!WriteForkFixture(flagsPath, NAVATTR_PRECISE))
  { printf("FAIL: write attribute fixture\n"); return 1; }
  CNavMesh mFlags;
  if (!mFlags.Load(flagsPath)
      || mFlags.Area(1)->attributes != NAVATTR_PRECISE
      || !mFlags.CanJumpFrom(1))
  { printf("FAIL: PRECISE parsing\n"); return 1; }
  remove(flagsPath);
    // --- NAVATTR_AVOID (0x0080) verification ---
  const char *normalForkPath = "test_nav_fork_norm.tmp";
  WriteForkFixture(normalForkPath, 0);
  CNavMesh mNorm;
  if (!mNorm.Load(normalForkPath)) { printf("FAIL: fork normal load\n"); return 1; }
  int pathBuf[16];
  int pLen = mNorm.BuildPath(0, 3, pathBuf, 16);
  if (pLen != 3 || pathBuf[1] != 1) { printf("FAIL: normal path should pick Area 2\n"); return 1; }
  float edgePathCost = PathCostByEdges(mNorm, pathBuf, pLen);
  float queryPathCost = mNorm.GetPathCost(0, 3, 0);
  if (edgePathCost < 0.0f || fabsf(edgePathCost - queryPathCost) > 0.01f)
  { printf("FAIL: shared path/cost disagreement %.3f vs %.3f\n", edgePathCost, queryPathCost); return 1; }
  NavClearDangerZones();
  g_navDangerEnabled = false;
  float normalCost = mNorm.GetPathCost(0, 3, 0);
  NavAddDangerZone(mNorm.Area(1)->center, 1000.0f);
  g_navDangerEnabled = true;
  float dangerCost = mNorm.GetPathCost(0, 3, 0);
  g_navDangerEnabled = false;
  NavClearDangerZones();
  if (dangerCost <= normalCost)
  { printf("FAIL: danger-zone cost did not change path cost\n"); return 1; }
  remove(normalForkPath);
  const char *avoidForkPath = "test_nav_fork_avoid.tmp";
  WriteForkFixture(avoidForkPath, 0x0080);
  CNavMesh mAvoid;
  if (!mAvoid.Load(avoidForkPath)) { printf("FAIL: fork avoid load\n"); return 1; }
  pLen = mAvoid.BuildPath(0, 3, pathBuf, 16);
  if (pLen != 3 || pathBuf[1] != 2) { printf("FAIL: avoid path should detour to Area 3\n"); return 1; }
  printf("VERIFIED: NAVATTR_AVOID (0x0080) successfully detours around avoid areas!\n");
  remove(avoidForkPath);

  // Typed nav-edge geometry: walk portals only come from a genuine shared
  // boundary. Stacked floors and separated drops are explicit transitions,
  // and split doorway slices merge only when their plane/elevation agree.
  const char *edgePath = "test_nav_edges.tmp";
  if (!WriteEdgeGeometryFixture(edgePath)) { printf("FAIL: write edge fixture\n"); return 1; }
  CNavMesh mEdges;
  if (!mEdges.Load(edgePath)) { printf("FAIL: edge fixture load\n"); return 1; }
  {
      NavEdgeMeta meta;
      Vector portal;
      if (!mEdges.GetEdgeMeta(0, 1, meta) || !meta.valid || meta.kind != NAV_EDGE_WALK)
      { printf("FAIL: adjacent edge not walk\n"); return 1; }
      if (fabsf(meta.midX - 100.0f) > 0.01f || fabsf(meta.midY - 50.0f) > 0.01f
          || fabsf(meta.halfSpan - 50.0f) > 0.01f || meta.axisY < 0.99f)
      { printf("FAIL: adjacent edge geometry\n"); return 1; }
      if (!mEdges.GetPortal(0, 1, portal)
          || fabsf(portal.x - meta.midX) > 0.01f
          || fabsf(portal.y - meta.midY) > 0.01f)
      { printf("FAIL: adjacent walk portal\n"); return 1; }

      Vector mid, axis;
      float halfSpan = 0.0f;
      if (!mEdges.GetPortalMerged(2, 3, mid, axis, halfSpan)
          || fabsf(halfSpan - 50.0f) > 0.01f || axis.y < 0.99f)
      { printf("FAIL: split doorway merge half=%.2f\n", halfSpan); return 1; }
      if (!mEdges.GetEdgeMeta(2, 5, meta) || !meta.valid || meta.kind != NAV_EDGE_JUMP)
      { printf("FAIL: stacked doorway slice not classified as jump\n"); return 1; }

      if (!mEdges.GetEdgeMeta(6, 7, meta) || !meta.valid || meta.kind != NAV_EDGE_JUMP
          || meta.zDelta < 99.0f)
      { printf("FAIL: stacked upward edge kind=%u dz=%.1f\n", (unsigned)meta.kind, meta.zDelta); return 1; }
      if (mEdges.GetPortal(6, 7, portal))
      { printf("FAIL: stacked upward edge exposed as portal\n"); return 1; }
      if (!mEdges.GetEdgeMeta(7, 6, meta) || !meta.valid || meta.kind != NAV_EDGE_DROP
          || meta.zDelta > -99.0f)
      { printf("FAIL: stacked downward edge kind=%u dz=%.1f\n", (unsigned)meta.kind, meta.zDelta); return 1; }
      if (mEdges.GetPortal(7, 6, portal))
      { printf("FAIL: stacked downward edge exposed as portal\n"); return 1; }

      if (!mEdges.GetEdgeMeta(8, 9, meta) || !meta.valid || meta.kind != NAV_EDGE_DROP
          || fabsf(meta.midX - 600.0f) > 0.01f || fabsf(meta.midY - 25.0f) > 0.01f
          || fabsf(meta.midZ - 100.0f) > 0.01f || fabsf(meta.zDelta + 100.0f) > 0.01f
          || fabsf(meta.halfSpan - 25.0f) > 0.01f)
      { printf("FAIL: separated drop geometry\n"); return 1; }
      if (mEdges.GetPortal(8, 9, portal))
      { printf("FAIL: separated drop exposed as walk portal\n"); return 1; }
      if (mEdges.GetPortalMerged(8, 9, mid, axis, halfSpan))
      { printf("FAIL: separated drop exposed as merged portal\n"); return 1; }
      NavTransitionWaypoint transition;
      if (!mEdges.GetTransitionWaypoint(8, 9, Vector(500, 25, 100), transition)
          || !transition.virtualApproach
          || fabsf(transition.point.x - 572.0f) > 0.01f
          || fabsf(transition.point.y - 25.0f) > 0.01f
          || fabsf(transition.point.z - 100.0f) > 0.01f)
      { printf("FAIL: virtual approach did not preserve source-floor Z\n"); return 1; }
      if (!mEdges.GetTransitionWaypoint(8, 9, Vector(590, 25, 100), transition)
          || transition.virtualApproach
          || fabsf(transition.point.z) > 0.01f)
      { printf("FAIL: committed transition did not use landing Z\n"); return 1; }

      if (!mEdges.GetEdgeMeta(10, 11, meta) || meta.valid
          || meta.kind != NAV_EDGE_MALFORMED)
      { printf("FAIL: full same-level overlap was not malformed\n"); return 1; }
      if (mEdges.GetPortal(10, 11, portal))
      { printf("FAIL: full same-level overlap exposed as portal\n"); return 1; }
      if (mEdges.FindArea(Vector(850, 50, 0)) != 10
          || mEdges.FindNearestArea(Vector(850, 50, 0)) != 10)
      { printf("FAIL: equal-area lookup tie did not choose the lower index\n"); return 1; }

      if (mEdges.GetTransitionWaypoint(12, 13, Vector(1050, 50, 40), transition))
      { printf("FAIL: stacked drop produced a separated-drop waypoint\n"); return 1; }
      if (mEdges.CrossedEdgeIntoArea(12, 13, Vector(1050, 50, 40)))
      { printf("FAIL: shallow stacked drop advanced while still on upper floor\n"); return 1; }
      if (!mEdges.CrossedEdgeIntoArea(12, 13, Vector(1050, 50, 0)))
      { printf("FAIL: shallow stacked drop did not complete on landing\n"); return 1; }
      if (mEdges.CrossedEdgeIntoArea(0, 1, Vector(90, 50, 0)))
      { printf("FAIL: walk edge crossed before portal plane\n"); return 1; }
      if (!mEdges.CrossedEdgeIntoArea(0, 1, Vector(110, 50, 0)))
      { printf("FAIL: walk edge did not complete after crossing\n"); return 1; }
  }
  printf("VERIFIED: typed nav edges distinguish walk, ladder, drop, jump, and malformed geometry\n");
  remove(edgePath);

  // Cost-cache coherency: changing a runtime penalty must invalidate the
  // cached path cost for an otherwise identical query.
  const char *costCachePath = "test_nav_cost_cache.tmp";
  if (!WriteForkFixture(costCachePath, NAVATTR_JUMP)) { printf("FAIL: write cost fixture\n"); return 1; }
  CNavMesh mCost;
  if (!mCost.Load(costCachePath)) { printf("FAIL: cost fixture load\n"); return 1; }
  g_navJumpPenalty = 0.0f;
  float lowCost = mCost.GetPathCost(0, 3, 0);
  g_navJumpPenalty = 10000.0f;
  float highCost = mCost.GetPathCost(0, 3, 0);
  if (lowCost < 0.0f || highCost < 0.0f || lowCost == highCost)
  {
      printf("FAIL: stale path-cost cache low=%.3f high=%.3f\n", lowCost, highCost);
      return 1;
  }
  printf("VERIFIED: path-cost cache invalidates after runtime penalty change (%.3f -> %.3f)\n", lowCost, highCost);
  remove(costCachePath);

  printf("ALL OK\n");
  return 0;
}
