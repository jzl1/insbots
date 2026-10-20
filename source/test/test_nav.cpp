// native test harness: synthesize a v9 nav file, parse it, run A*
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
struct Vector { float x,y,z; Vector(){} Vector(float a,float b,float c):x(a),y(b),z(c){}
  Vector operator-(const Vector&o)const{return Vector(x-o.x,y-o.y,z-o.z);}
  float Length()const{return sqrtf(x*x+y*y+z*z);} };
#define TESTING_NATIVE 1
#include "../src/nav.h"
#include "../src/nav.cpp"

static void put32(FILE*f,unsigned v){fwrite(&v,4,1,f);} 
static void put16(FILE*f,unsigned short v){fwrite(&v,2,1,f);} 
static void put8(FILE*f,unsigned char v){fwrite(&v,1,1,f);} 
static void putf(FILE*f,float v){fwrite(&v,4,1,f);} 

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
      Vector desired(550.f, 50.f, 0.f);
      int proxy = md.FindClosestReachableArea(0, desired);
      if (proxy != 2) { printf("FAIL: reachable proxy=%d\n", proxy); return 1; }
      int pn = md.BuildPath(0, proxy, dpath, 16);
      if (pn <= 0 || dpath[pn - 1] != 2) { printf("FAIL: proxy path len=%d\n", pn); return 1; }
      int ps = md.FindClosestReachableArea(3, desired);
      if (ps != 3) { printf("FAIL: protected start proxy=%d\n", ps); return 1; }
  }
  remove(dirPath);
    // --- NAVATTR_AVOID (0x0080) verification ---
  const char *normalForkPath = "test_nav_fork_norm.tmp";
  WriteForkFixture(normalForkPath, 0);
  CNavMesh mNorm;
  if (!mNorm.Load(normalForkPath)) { printf("FAIL: fork normal load\n"); return 1; }
  int pathBuf[16];
  int pLen = mNorm.BuildPath(0, 3, pathBuf, 16);
  if (pLen != 3 || pathBuf[1] != 1) { printf("FAIL: normal path should pick Area 2\n"); return 1; }
  remove(normalForkPath);
  const char *avoidForkPath = "test_nav_fork_avoid.tmp";
  WriteForkFixture(avoidForkPath, 0x0080);
  CNavMesh mAvoid;
  if (!mAvoid.Load(avoidForkPath)) { printf("FAIL: fork avoid load\n"); return 1; }
  pLen = mAvoid.BuildPath(0, 3, pathBuf, 16);
  if (pLen != 3 || pathBuf[1] != 2) { printf("FAIL: avoid path should detour to Area 3\n"); return 1; }
  printf("VERIFIED: NAVATTR_AVOID (0x0080) successfully detours around avoid areas!\n");
  remove(avoidForkPath);

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
