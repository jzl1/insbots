// ============================================================================
// navdoctor - validates a Source .nav mesh against the map's REAL collision
// geometry (.bsp), by simulating a player-sized hull walking every connection.
// Connections that no hull (standing or crouched, with step-ups and jumps) can
// physically traverse are cut, and a repaired .nav is written.
//
// This catches the class of defect nav_generate is famous for: connections
// through openings too small for a player (windows, fences, grates), because
// the nav file itself stores only floor rectangles - no walls, no clearance.
// ============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

struct Vector { float x,y,z; Vector():x(0),y(0),z(0){} Vector(float a,float b,float c):x(a),y(b),z(c){}
  Vector operator-(const Vector&o)const{return Vector(x-o.x,y-o.y,z-o.z);}
  Vector operator+(const Vector&o)const{return Vector(x+o.x,y+o.y,z+o.z);}
  Vector operator*(float s)const{return Vector(x*s,y*s,z*s);}
  float Length()const{return sqrtf(x*x+y*y+z*z);} };
static float dot(const Vector&a,const Vector&b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static Vector cross(const Vector&a,const Vector&b){return Vector(a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x);}
static float xyd(const Vector&a,const Vector&b){float dx=a.x-b.x,dy=a.y-b.y;return sqrtf(dx*dx+dy*dy);}

#define TESTING_NATIVE 1
#include "../src/nav.h"
#include "../src/nav.cpp"

// ---------------------------------------------------------------- BSP formats
#pragma pack(push,1)
struct lump_t { int ofs,len,version; char fourCC[4]; };
struct dheader_t { int ident,version; lump_t lumps[64]; int mapRev; };
struct dplane_t { Vector n; float d; int type; };
struct dnode_t { int planenum; int children[2]; short mins[3],maxs[3];
                 unsigned short firstface,numfaces; short area,pad; };
struct dedge_t { unsigned short v[2]; };
struct dbrush_t { int firstside,numsides,contents; };
struct dbrushside_t { unsigned short planenum; short texinfo,dispinfo,bevel; };
struct dmodel_t { Vector mins,maxs,origin; int headnode,firstface,numfaces; };
struct dface_t { unsigned short planenum; unsigned char side,onNode; int firstedge;
                 short numedges,texinfo,dispinfo,fogvol; unsigned char styles[4];
                 int lightofs; float area; int lmins[2],lsize[2]; int origFace;
                 unsigned short numPrims,firstPrim; unsigned int smoothing; };
struct dtexinfo_t { float vecs[2][4]; float lightmapVecs[2][4]; int flags; int texdata; };
struct ddispvert_t { Vector vec; float dist,alpha; };
#pragma pack(pop)

#define C_SOLID 0x1
#define C_WINDOW 0x2
#define C_GRATE 0x8
#define C_PLAYERCLIP 0x10000
#define C_MONSTERCLIP 0x20000
#define SOLID_MASK (C_SOLID|C_WINDOW|C_GRATE|C_PLAYERCLIP|C_MONSTERCLIP)

// ---------------------------------------------------------------- collision db
struct BrushSideP { Vector n; float d; };
struct CBrush { int firstSide,numSides; Vector lo,hi; bool isObstacle; };
struct Tri { Vector a,b,c; };

static std::vector<BrushSideP> g_sides;      // flattened side planes per brush
static std::vector<CBrush>     g_brushes;    // world solid brushes only
static std::vector<Tri>        g_tris;       // displacement triangles

// spatial grid
static float g_worldLo[3], g_worldHi[3];
static int   g_gx, g_gy;                     // cells in x,y (z ignored)
static const float CELL = 256.0f;
static std::vector<std::vector<int> > g_cellBrush, g_cellTri;

static int cellOf(float x,float y){
  int cx=(int)((x-g_worldLo[0])/CELL), cy=(int)((y-g_worldLo[1])/CELL);
  if(cx<0)cx=0; if(cy<0)cy=0; if(cx>=g_gx)cx=g_gx-1; if(cy>=g_gy)cy=g_gy-1;
  return cy*g_gx+cx;
}

static void gridInsert(std::vector<std::vector<int> >&grid,const Vector&lo,const Vector&hi,int idx){
  int cx0=(int)((lo.x-g_worldLo[0])/CELL), cy0=(int)((lo.y-g_worldLo[1])/CELL);
  int cx1=(int)((hi.x-g_worldLo[0])/CELL), cy1=(int)((hi.y-g_worldLo[1])/CELL);
  if(cx0<0)cx0=0; if(cy0<0)cy0=0; if(cx1>=g_gx)cx1=g_gx-1; if(cy1>=g_gy)cy1=g_gy-1;
  for(int cy=cy0;cy<=cy1;cy++)for(int cx=cx0;cx<=cx1;cx++)
    grid[cy*g_gx+cx].push_back(idx);
}

// ---------------------------------------------------------------- BSP loading
static bool loadBsp(const char*path){
  FILE*fp=fopen(path,"rb"); if(!fp){printf("no bsp\n");return false;}
  fseek(fp,0,SEEK_END); long sz=ftell(fp); fseek(fp,0,SEEK_SET);
  unsigned char*d=(unsigned char*)malloc(sz);
  if(fread(d,1,sz,fp)!=(size_t)sz){fclose(fp);return false;}
  fclose(fp);
  dheader_t*h=(dheader_t*)d;
  if(h->ident!=('V'|('B'<<8)|('S'<<16)|('P'<<24))){printf("bad magic\n");return false;}
  printf("BSP version %d\n",h->version);

  #define LUMP(i,T) ((T*)(d+h->lumps[i].ofs)); int n_##T = h->lumps[i].len/(int)sizeof(T)
  dplane_t   *planes = LUMP(1,dplane_t);   (void)n_dplane_t;
  Vector     *verts  = (Vector*)(d+h->lumps[3].ofs);
  dnode_t    *nodes  = LUMP(5,dnode_t);    (void)n_dnode_t;
  dtexinfo_t *texinfo= LUMP(6,dtexinfo_t); (void)n_dtexinfo_t;
  dface_t    *faces  = LUMP(7,dface_t);    (void)n_dface_t;
  dedge_t    *edges  = LUMP(12,dedge_t);   (void)n_dedge_t;
  int        *surfedges=(int*)(d+h->lumps[13].ofs);
  dmodel_t   *models = LUMP(14,dmodel_t);  int nModels=h->lumps[14].len/(int)sizeof(dmodel_t);
  unsigned short*leafbrushes=(unsigned short*)(d+h->lumps[17].ofs);
  dbrush_t   *brushes= LUMP(18,dbrush_t);  int nBrushes=h->lumps[18].len/(int)sizeof(dbrush_t);
  dbrushside_t*bsides= LUMP(19,dbrushside_t);(void)n_dbrushside_t;
  unsigned char*dispinfo=d+h->lumps[26].ofs; int nDisp=h->lumps[26].len/176;
  ddispvert_t*dispverts=(ddispvert_t*)(d+h->lumps[33].ofs);

  // leaf stride: v0=56 (has ambient cube), v1=32
  int leafStride = (h->lumps[10].version==0)?56:32;
  unsigned char*leafs=d+h->lumps[10].ofs;
  int nLeafs=h->lumps[10].len/leafStride;
  printf("models=%d brushes=%d leafs=%d(stride %d) disp=%d\n",nModels,nBrushes,nLeafs,leafStride,nDisp);

  // -- world brush set: walk model 0's node tree, collect leafbrushes
  std::vector<unsigned char> isWorld(nBrushes,0);
  {
    std::vector<int> stack; stack.push_back(models[0].headnode);
    while(!stack.empty()){
      int nn=stack.back(); stack.pop_back();
      if(nn>=0){ stack.push_back(nodes[nn].children[0]); stack.push_back(nodes[nn].children[1]); }
      else {
        int li=-1-nn; if(li>=nLeafs) continue;
        unsigned char*L=leafs+li*leafStride;
        unsigned short flb=*(unsigned short*)(L+24);
        unsigned short nlb=*(unsigned short*)(L+26);
        for(int k=0;k<nlb;k++){ int b=leafbrushes[flb+k]; if(b<nBrushes) isWorld[b]=1; }
      }
    }
  }

  // -- solid world brushes & structural entity walls (func_wall, func_detail, func_door)
  // We strictly whitelist physical wall entities while rejecting giant spawn blockers or visibility portals.
  std::vector<unsigned char> isStructuralWall(nBrushes,0);
  if(h->lumps[0].len > 0 && h->lumps[0].ofs > 0){
    std::vector<int> wallModels;
    const char* entStr = (const char*)(d + h->lumps[0].ofs);
    int entLen = h->lumps[0].len;
    int idx = 0;
    while(idx < entLen && entStr[idx] != '\0'){
      const char* blockStart = strchr(entStr + idx, '{');
      if(!blockStart) break;
      const char* blockEnd = strchr(blockStart, '}');
      if(!blockEnd) break;
      std::string block(blockStart, blockEnd - blockStart + 1);
      idx = (int)(blockEnd - entStr) + 1;

      size_t mPos = block.find("\"model\"");
      if(mPos == std::string::npos) continue;
      size_t valPos = block.find("\"", mPos + 7);
      if(valPos == std::string::npos) continue;
      size_t valEnd = block.find("\"", valPos + 1);
      if(valEnd == std::string::npos) continue;

      std::string modelVal = block.substr(valPos + 1, valEnd - valPos - 1);
      if(!modelVal.empty() && modelVal[0] == '*'){
        int mNum = atoi(modelVal.c_str() + 1);
        if(mNum > 0 && mNum < nModels){
          std::string lower = block;
          for(size_t c=0; c<lower.size(); c++) lower[c] = (char)tolower(lower[c]);
          
          // Must match a genuine physical wall classname
          bool isWall = (lower.find("func_wall") != std::string::npos ||
                         lower.find("func_detail") != std::string::npos ||
                         lower.find("func_breakable") != std::string::npos ||
                         lower.find("func_door") != std::string::npos ||
                         (lower.find("func_brush") != std::string::npos && lower.find("\"solidity\" \"1\"") == std::string::npos && lower.find("\"solid\" \"0\"") == std::string::npos));
          if(!isWall) continue;

          // Reject ANY entity with dynamic blocker/portal keywords or gigantic building-sized dimensions (>600u)
          if(lower.find("blocker") != std::string::npos ||
             lower.find("portal") != std::string::npos ||
             lower.find("cluster") != std::string::npos ||
             lower.find("zone") != std::string::npos ||
             lower.find("avoid") != std::string::npos ||
             lower.find("smoke") != std::string::npos ||
             lower.find("trigger") != std::string::npos ||
             lower.find("water") != std::string::npos ||
             lower.find("illusionary") != std::string::npos) {
            continue;
          }
          float bx = (&models[mNum].maxs.x)[0] - (&models[mNum].mins.x)[0];
          float by = (&models[mNum].maxs.x)[1] - (&models[mNum].mins.x)[1];
          float bz = (&models[mNum].maxs.x)[2] - (&models[mNum].mins.x)[2];
          if(bx > 600.0f || by > 600.0f || bz > 600.0f) continue; // Skip room/building sized optimization volumes!

          wallModels.push_back(mNum);
        }
      }
    }
    std::vector<int> stack;
    for(size_t i=0; i<wallModels.size(); i++){
      int m = wallModels[i];
      if(m >= 0 && m < nModels) stack.push_back(models[m].headnode);
    }
    while(!stack.empty()){
      int nn=stack.back(); stack.pop_back();
      if(nn>=0){ stack.push_back(nodes[nn].children[0]); stack.push_back(nodes[nn].children[1]); }
      else {
        int li=-1-nn; if(li>=nLeafs) continue;
        unsigned char*L=leafs+li*leafStride;
        unsigned short flb=*(unsigned short*)(L+24);
        unsigned short nlb=*(unsigned short*)(L+26);
        for(int k=0;k<nlb;k++){ int b=leafbrushes[flb+k]; if(b<nBrushes) isStructuralWall[b]=1; }
      }
    }
  }

  int worldCount=0;
  for(int b=0;b<nBrushes;b++){
    if(!isWorld[b] && !isStructuralWall[b]) continue;
    if(!(brushes[b].contents & SOLID_MASK)) continue;
    bool isSky = false;
    for(int s=0;s<brushes[b].numsides;s++){
      int ti = bsides[brushes[b].firstside+s].texinfo;
      if(ti >= 0 && ti < n_dtexinfo_t && (texinfo[ti].flags & 0x4)) isSky = true;
    }
    if(isSky) continue;
    CBrush cb; cb.firstSide=(int)g_sides.size(); cb.numSides=brushes[b].numsides;
    cb.lo=Vector(1e9f,1e9f,1e9f); cb.hi=Vector(-1e9f,-1e9f,-1e9f);
    cb.isObstacle = (!isWorld[b] || (brushes[b].contents & (C_PLAYERCLIP | C_MONSTERCLIP)) != 0);
    for(int s=0;s<brushes[b].numsides;s++){
      const dplane_t&pl=planes[bsides[brushes[b].firstside+s].planenum];
      BrushSideP sp; sp.n=pl.n; sp.d=pl.d; g_sides.push_back(sp);
      // axial planes give the AABB
      if(pl.n.x> .999f){ if(pl.d<cb.hi.x||cb.hi.x<-1e8f)cb.hi.x=pl.d; }
      if(pl.n.x<-.999f){ float v=-pl.d; if(v>cb.lo.x||cb.lo.x>1e8f)cb.lo.x=v; }
      if(pl.n.y> .999f){ if(pl.d<cb.hi.y||cb.hi.y<-1e8f)cb.hi.y=pl.d; }
      if(pl.n.y<-.999f){ float v=-pl.d; if(v>cb.lo.y||cb.lo.y>1e8f)cb.lo.y=v; }
      if(pl.n.z> .999f){ if(pl.d<cb.hi.z||cb.hi.z<-1e8f)cb.hi.z=pl.d; }
      if(pl.n.z<-.999f){ float v=-pl.d; if(v>cb.lo.z||cb.lo.z>1e8f)cb.lo.z=v; }
    }
    if(cb.lo.x>cb.hi.x||cb.lo.y>cb.hi.y||cb.lo.z>cb.hi.z){ // degenerate: huge box
      cb.lo=Vector(-16384,-16384,-16384); cb.hi=Vector(16384,16384,16384);
    }
    g_brushes.push_back(cb); worldCount++;
  }
  printf("solid brushes (world + structural walls) kept=%d of %d\n",worldCount,nBrushes);

  // -- displacement triangles
  int triCount=0;
  for(int i=0;i<nDisp;i++){
    unsigned char*di=dispinfo+i*176;
    Vector startPos=*(Vector*)(di+0);
    int vertStart=*(int*)(di+12);
    int power=*(int*)(di+20);
    unsigned short mapFace=*(unsigned short*)(di+36);
    const dface_t&f=faces[mapFace];
    if(f.numedges!=4) continue;
    Vector c[4];
    for(int e=0;e<4;e++){
      int se=surfedges[f.firstedge+e];
      c[e]= se>=0 ? verts[edges[se].v[0]] : verts[edges[-se].v[1]];
    }
    // rotate so c[0] is the disp start position
    int best=0; float bd=1e18f;
    for(int k=0;k<4;k++){ float dd=(c[k]-startPos).Length(); if(dd<bd){bd=dd;best=k;} }
    Vector q[4]; for(int k=0;k<4;k++) q[k]=c[(best+k)&3];
    int size=(1<<power)+1;
    std::vector<Vector> P(size*size);
    for(int y=0;y<size;y++)for(int x=0;x<size;x++){
      float u=(float)x/(size-1), v=(float)y/(size-1);
      Vector base = q[0]*((1-u)*(1-v)) + q[1]*(u*(1-v)) + q[2]*(u*v) + q[3]*((1-u)*v);
      const ddispvert_t&dv=dispverts[vertStart+y*size+x];
      P[y*size+x]= base + dv.vec*dv.dist;
    }
    for(int y=0;y<size-1;y++)for(int x=0;x<size-1;x++){
      Tri t1={P[y*size+x],P[y*size+x+1],P[(y+1)*size+x]};
      Tri t2={P[y*size+x+1],P[(y+1)*size+x+1],P[(y+1)*size+x]};
      g_tris.push_back(t1); g_tris.push_back(t2); triCount+=2;
    }
  }
  printf("displacement tris=%d\n",triCount);

  // -- grid
  for(int a=0;a<3;a++){ g_worldLo[a]=(&models[0].mins.x)[a]-64; g_worldHi[a]=(&models[0].maxs.x)[a]+64; }
  g_gx=(int)((g_worldHi[0]-g_worldLo[0])/CELL)+1;
  g_gy=(int)((g_worldHi[1]-g_worldLo[1])/CELL)+1;
  g_cellBrush.resize(g_gx*g_gy); g_cellTri.resize(g_gx*g_gy);
  for(int b=0;b<(int)g_brushes.size();b++) gridInsert(g_cellBrush,g_brushes[b].lo,g_brushes[b].hi,b);
  for(int t=0;t<(int)g_tris.size();t++){
    Vector lo=g_tris[t].a,hi=g_tris[t].a;
    const Vector*vv[3]={&g_tris[t].a,&g_tris[t].b,&g_tris[t].c};
    for(int k=1;k<3;k++){ const Vector&p=*vv[k];
      if(p.x<lo.x)lo.x=p.x; if(p.y<lo.y)lo.y=p.y; if(p.z<lo.z)lo.z=p.z;
      if(p.x>hi.x)hi.x=p.x; if(p.y>hi.y)hi.y=p.y; if(p.z>hi.z)hi.z=p.z; }
    gridInsert(g_cellTri,lo,hi,t);
  }
  printf("grid %dx%d cells\n",g_gx,g_gy);
  // note: 'd' stays allocated (verts/planes pointers no longer needed - we copied)
  return true;
}

// ---------------------------------------------------------------- tracing
static thread_local unsigned g_stamp=1;
static thread_local std::vector<unsigned> g_brushStamp, g_triStamp;

// swept AABB vs brushes; box given as feet-origin: lo=(-hw,-hw,0) hi=(hw,hw,hh)
struct TraceRes { float frac; bool startsolid; };

static TraceRes traceBrushes(const Vector&start,const Vector&end,float hw,float hh, bool ignoreObstacles = false){
  TraceRes r; r.frac=1.0f; r.startsolid=false;
  float lo[3]={-hw,-hw,0}, hi[3]={hw,hw,hh};
  // gather cells along the swept box
  float sl[2]={ (start.x<end.x?start.x:end.x)-hw, (start.y<end.y?start.y:end.y)-hw };
  float sh[2]={ (start.x>end.x?start.x:end.x)+hw, (start.y>end.y?start.y:end.y)+hw };
  int cx0=(int)((sl[0]-g_worldLo[0])/CELL), cy0=(int)((sl[1]-g_worldLo[1])/CELL);
  int cx1=(int)((sh[0]-g_worldLo[0])/CELL), cy1=(int)((sh[1]-g_worldLo[1])/CELL);
  if(cx0<0)cx0=0; if(cy0<0)cy0=0; if(cx1>=g_gx)cx1=g_gx-1; if(cy1>=g_gy)cy1=g_gy-1;
  g_stamp++;
  if(g_brushStamp.size()<g_brushes.size()) g_brushStamp.resize(g_brushes.size(),0);
  float zl=(start.z<end.z?start.z:end.z)-4, zh=(start.z>end.z?start.z:end.z)+hh+4;
  for(int cy=cy0;cy<=cy1;cy++)for(int cx=cx0;cx<=cx1;cx++){
    const std::vector<int>&cell=g_cellBrush[cy*g_gx+cx];
    for(size_t ci=0;ci<cell.size();ci++){
      int b=cell[ci];
      if(g_brushStamp[b]==g_stamp) continue; g_brushStamp[b]=g_stamp;
      const CBrush&cb=g_brushes[b];
      if(ignoreObstacles && cb.isObstacle) continue;
      if(cb.hi.z<zl||cb.lo.z>zh) continue;
      float enter=-1.0f, leave=2.0f; bool startout=false; bool skip=false;
      for(int s=0;s<cb.numSides;s++){
        const BrushSideP&sp=g_sides[cb.firstSide+s];
        // support offset: push plane out by box extent
        float ofs= (sp.n.x<0?hi[0]:lo[0])*sp.n.x
                 + (sp.n.y<0?hi[1]:lo[1])*sp.n.y
                 + (sp.n.z<0?hi[2]:lo[2])*sp.n.z;
        float dd=sp.d - ofs;
        float d1=dot(start,sp.n)-dd, d2=dot(end,sp.n)-dd;
        if(d1>0) startout=true;
        if(d1>0&&d2>=d1){skip=true;break;}           // fully in front, moving away
        if(d1<=0&&d2<=0) continue;                    // fully behind this plane
        if(d1>d2){ float f=(d1-0.03125f)/(d1-d2); if(f>enter)enter=f; }
        else     { float f=(d1+0.03125f)/(d1-d2); if(f<leave)leave=f; }
      }
      if(skip) continue;
      if(!startout){ r.startsolid=true; continue; }
      if(enter<leave && enter>-1.0f && enter<r.frac) r.frac= enter<0?0:enter;
    }
  }
  return r;
}

// ray vs displacement tris (Moller-Trumbore), several rays covering the hull
static float rayTris(const Vector&start,const Vector&end){
  float best=1.0f;
  Vector dir=end-start;
  float sl[2]={ (start.x<end.x?start.x:end.x)-1, (start.y<end.y?start.y:end.y)-1 };
  float sh[2]={ (start.x>end.x?start.x:end.x)+1, (start.y>end.y?start.y:end.y)+1 };
  int cx0=(int)((sl[0]-g_worldLo[0])/CELL), cy0=(int)((sl[1]-g_worldLo[1])/CELL);
  int cx1=(int)((sh[0]-g_worldLo[0])/CELL), cy1=(int)((sh[1]-g_worldLo[1])/CELL);
  if(cx0<0)cx0=0; if(cy0<0)cy0=0; if(cx1>=g_gx)cx1=g_gx-1; if(cy1>=g_gy)cy1=g_gy-1;
  g_stamp++;
  if(g_triStamp.size()<g_tris.size()) g_triStamp.resize(g_tris.size(),0);
  for(int cy=cy0;cy<=cy1;cy++)for(int cx=cx0;cx<=cx1;cx++){
    const std::vector<int>&cell=g_cellTri[cy*g_gx+cx];
    for(size_t ci=0;ci<cell.size();ci++){
      int t=cell[ci];
      if(g_triStamp[t]==g_stamp) continue; g_triStamp[t]=g_stamp;
      const Tri&T=g_tris[t];
      Vector e1=T.b-T.a, e2=T.c-T.a;
      Vector p=cross(dir,e2); float det=dot(e1,p);
      if(det>-1e-6f&&det<1e-6f) continue;
      float inv=1.0f/det;
      Vector tv=start-T.a;
      float u=dot(tv,p)*inv; if(u<-0.02f||u>1.02f) continue;
      Vector q=cross(tv,e1);
      float v=dot(dir,q)*inv; if(v<-0.02f||u+v>1.02f) continue;
      float f=dot(e2,q)*inv;
      if(f>0.0f&&f<best) best=f;
    }
  }
  return best;
}

static TraceRes traceHull(const Vector&start,const Vector&end,float hw,float hh, bool ignoreObstacles = false){
  TraceRes r=traceBrushes(start,end,hw,hh,ignoreObstacles);
  // displacement: rays from hull corners at foot + top, and center
  static const float cf[5][2]={{0,0},{1,1},{1,-1},{-1,1},{-1,-1}};
  for(int zi=0;zi<2;zi++){
    float z= zi? hh-2.0f : 2.0f;
    for(int k=0;k<5;k++){
      Vector o(cf[k][0]*(hw-1),cf[k][1]*(hw-1),z);
      float f=rayTris(start+o,end+o);
      if(f<r.frac) r.frac=f;
    }
  }
  return r;
}

static Vector lerpV(const Vector&a,const Vector&b,float t){ return a+(b-a)*t; }

// find ground z at (x,y) starting from zhint upward a bit; robust against the
// probe starting embedded in geometry (nav z a touch below the real floor)
static bool groundAt(float x,float y,float zhint,float&outZ){
  for(int lift=0;lift<4;lift++){
    Vector s(x,y,zhint+40+lift*20), e(x,y,zhint-240);
    TraceRes r=traceHull(s,e,14.0f,4.0f);
    if(r.startsolid) continue;              // started inside something: lift & retry
    if(r.frac>=1.0f) return false;
    outZ = s.z + (e.z-s.z)*r.frac;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- walk sim
// Simulate game movement from S to E: step-ups to 18, jump-ups to 50 when
// blocked, falls allowed to maxDrop below current. Returns true if E reached.
static bool walkSim(Vector S,Vector E,float hw,float hh,float maxDrop,bool allowJump){
  // settle S on ground
  float gz;
  if(groundAt(S.x,S.y,S.z,gz)) S.z=gz+1.0f;
  float best=xyd(S,E); int stall=0;
  Vector cur=S;
  for(int it=0;it<80;it++){
    float d=xyd(cur,E);
    if(d<26.0f && cur.z>E.z-maxDrop-40 && cur.z<E.z+90) return true;
    Vector dir=E-cur; dir.z=0;
    float L=sqrtf(dir.x*dir.x+dir.y*dir.y); if(L<1e-3f) return true;
    dir.x/=L; dir.y/=L;
    float step= L<14?L:14;

    // raise (step)
    float up=18.0f;
    TraceRes ur=traceHull(cur,cur+Vector(0,0,up),hw,hh);
    Vector raised=cur+Vector(0,0,up*ur.frac);
    // forward
    TraceRes fr=traceHull(raised,raised+dir*step,hw,hh);
    if(fr.frac<0.12f && allowJump){
      // try a jump-height climb
      TraceRes jr=traceHull(cur,cur+Vector(0,0,50),hw,hh);
      Vector jraised=cur+Vector(0,0,50*jr.frac);
      TraceRes jf=traceHull(jraised,jraised+dir*step,hw,hh);
      if(jf.frac>fr.frac){ raised=jraised; fr=jf; }
    }
    if(fr.frac<0.08f){
      // blocked; tiny sidestep to defeat corner-catching, else fail via stall
      Vector side(-dir.y,dir.x,0);
      TraceRes s1=traceHull(raised,raised+side*10.0f,hw,hh);
      if(s1.frac>0.5f) raised=raised+side*(10.0f*s1.frac);
      else{
        TraceRes s2=traceHull(raised,raised+side*-10.0f,hw,hh);
        if(s2.frac>0.5f) raised=raised+side*(-10.0f*s2.frac);
      }
    }
    Vector fwd=raised+dir*(step*fr.frac);
    // drop
    float dropLen=(raised.z-cur.z)+18.0f+maxDrop;
    TraceRes dr=traceHull(fwd,fwd+Vector(0,0,-dropLen),hw,hh);
    if(dr.frac>=1.0f) return false;              // walked off into a void
    cur=fwd+Vector(0,0,-dropLen*dr.frac+0.5f);

    float nb=xyd(cur,E);
    if(nb<best-2.0f){best=nb;stall=0;}
    else if(++stall>=5) return false;
  }
  return false;
}


// ============================================================================
// NAV GENERATOR
// ============================================================================
#include <cstdint>
#include <atomic>
#include <chrono>
#ifdef _OPENMP
#include <omp.h>
#endif

static void printProgress(const char* label, int current, int total, std::chrono::steady_clock::time_point startTime){
  if(total <= 0) return;
  if(current != total && (current % 15) != 0 && (current * 100 / total) == ((current - 1) * 100 / total)) return;
  
  int percent = (int)((long long)current * 100 / total);
  if(percent > 100) percent = 100;
  
  int barWidth = 35;
  int filled = (percent * barWidth) / 100;
  
  auto now = std::chrono::steady_clock::now();
  double elapsed = std::chrono::duration<double>(now - startTime).count();
  
  printf("\r%-16s [", label);
  for(int b=0; b<barWidth; b++){
    if(b < filled) printf("=");
    else if(b == filled) printf(">");
    else printf(" ");
  }
  printf("] %3d%% (%d/%d) %.1fs", percent, current, total, elapsed);
  if(current == total) printf("\n");
  fflush(stdout);
}

// standing headroom at a ground point: 2 = stand, 1 = crouch-only, 0 = blocked
static int headroom(float x, float y, float gz){
  Vector feet(x,y,gz+2.0f);
  TraceRes r = traceHull(feet, feet+Vector(0,0,72.0f), 8.0f, 4.0f);
  if(r.startsolid) return 0;
  float clear = 72.0f * r.frac;
  if(clear >= 68.0f) return 2;
  if(clear >= 36.0f) return 1;
  return 0;
}

// connection type between two ground (feet) points, using the WALK SIMULATOR so
static int connType(const Vector&a, const Vector&b){
  float dz = b.z - a.z;
  if(dz > 18.0f)   return 0;                // max step/stair height in Source Engine is 18u; prevents climbing onto crates & roofs!
  if(dz < -96.0f)  return 0;                // max safe tactical drop height (~8 feet); prevents dropping off ledges onto out-of-bounds terrain!
  
  // Impenetrable full-body standing barrier test between adjacent tiles.
  // We test a 24u-wide cylinder spanning from just above max step height (18.0u) up to head elevation (68.0u).
  // This guarantees zero connections can form through low 24u walls, window frames, railings, or door jambs!
  Vector stepA = a + Vector(0,0,18.0f), stepB = b + Vector(0,0,18.0f);
  TraceRes wRes = traceHull(stepA, stepB, 12.0f, 50.0f, false);
  if(wRes.startsolid || wRes.frac < 0.98f) return 0; // Blocked by structural wall, door partition, or obstacle!
  
  // Floor-level ankle check on level ground (prevents connecting across thin ground dividing partitions)
  if(fabs(dz) < 12.0f) {
    Vector footA = a + Vector(0,0,8.0f), footB = b + Vector(0,0,8.0f);
    TraceRes fRes = traceHull(footA, footB, 6.0f, 8.0f, false);
    if(fRes.startsolid || fRes.frac < 0.98f) return 0; // Blocked by low floor barrier or dividing molding
  }

  if(dz <= 18.0f && dz >= -36.0f) return 1; // normal walk / slope / step connection
  return 3; // drop-down (dz < -36.0f)
}

// ------- .nav v9 writer helpers -------
static void W32(std::vector<unsigned char>&o, uint32_t v){ o.insert(o.end(),(unsigned char*)&v,(unsigned char*)&v+4); }
static void W16(std::vector<unsigned char>&o, uint16_t v){ o.insert(o.end(),(unsigned char*)&v,(unsigned char*)&v+2); }
static void W8 (std::vector<unsigned char>&o, uint8_t v){ o.push_back(v); }
static void WF (std::vector<unsigned char>&o, float v){ o.insert(o.end(),(unsigned char*)&v,(unsigned char*)&v+4); }

#define NAVATTR_CROUCH 0x01
#define NAVATTR_JUMP   0x02
#define NAVATTR_AVOID  0x800   // Source NAV_MESH_AVOID

struct GCell { int ix, iy; float z; unsigned char state; };  // state 2 stand, 1 crouch

// parse ins_spawnpoint / info_player_* origins from the BSP entity lump as seeds
static void parseSeeds(const char*path, std::vector<Vector>&seeds){
  FILE*fp=fopen(path,"rb"); if(!fp) return;
  int hdr[2]; if(fread(hdr,4,2,fp)!=2){fclose(fp);return;}
  int lofs, llen; fseek(fp,8,SEEK_SET); // lump 0 entry at offset 8
  fread(&lofs,4,1,fp); fread(&llen,4,1,fp);
  if(llen<=0||llen>64*1024*1024){fclose(fp);return;}
  std::vector<char> buf(llen+1);
  fseek(fp,lofs,SEEK_SET); if(fread(&buf[0],1,llen,fp)!=(size_t)llen){fclose(fp);return;}
  fclose(fp); buf[llen]=0;
  std::string s(&buf[0], llen);
  size_t p=0;
  while((p=s.find('{',p))!=std::string::npos){
    size_t q=s.find('}',p); if(q==std::string::npos) break;
    std::string blk=s.substr(p,q-p);
    if(blk.find("intermission")!=std::string::npos ||
       blk.find("observer")!=std::string::npos ||
       blk.find("camera")!=std::string::npos ||
       blk.find("logo")!=std::string::npos ||
       blk.find("view")!=std::string::npos ||
       blk.find("sky")!=std::string::npos ||
       blk.find("spectator")!=std::string::npos){
      p=q+1; continue;
    }
    if(blk.find("spawn")!=std::string::npos ||
       blk.find("player")!=std::string::npos ||
       blk.find("objective")!=std::string::npos ||
       blk.find("control_point")!=std::string::npos ||
       blk.find("weapon_")!=std::string::npos ||
       blk.find("item_")!=std::string::npos ||
       blk.find("checkpoint")!=std::string::npos ||
       blk.find("flag")!=std::string::npos ||
       blk.find("buyzone")!=std::string::npos){
      size_t op=blk.find("\"origin\"");
      if(op!=std::string::npos){
        size_t v1=blk.find('"',op+8); size_t v2=blk.find('"',v1+1);
        if(v1!=std::string::npos&&v2!=std::string::npos){
          float x,y,z; if(sscanf(blk.substr(v1+1,v2-v1-1).c_str(),"%f %f %f",&x,&y,&z)==3)
            seeds.push_back(Vector(x,y,z));
        }
      }
    }
    p=q+1;
  }
}

int main(int argc,char**argv){
  const char* bsp = argc>1?argv[1]:"map.bsp";
  const char* out = argc>2?argv[2]:"map.nav";
  float G = argc>3?atof(argv[3]):32.0f;   // grid resolution (default 32 for doorways and narrow paths)
  if(G <= 0.0f || G > 1024.0f) G = 32.0f;
  bool markAvoid=false;                   // pass "avoid" to flag jump/crouch areas AVOID
  for(int i=1;i<argc;i++) if(!strcmp(argv[i],"avoid")) markAvoid=true;
  if(!loadBsp(bsp)) return 1;

  int NX = (int)((g_worldHi[0]-g_worldLo[0])/G)+1;
  int NY = (int)((g_worldHi[1]-g_worldLo[1])/G)+1;
  float loZ = g_worldLo[2], hiZ = g_worldHi[2];
  printf("grid %dx%d @ %.0fu, z[%.0f..%.0f]\n",NX,NY,G,loZ,hiZ);

  // ---- multi-level walkable cell sampling (Multicore Parallelized) ----
  std::vector<GCell> cells;
  std::vector<std::vector<int> > col(NX*NY);   // cell indices per column
  std::vector<std::vector<GCell> > rowCells(NY);
  std::atomic<int> sampleProgress(0);
  auto tSample = std::chrono::steady_clock::now();

#ifdef _OPENMP
  printf("\n[Multicore Engine] OpenMP enabled (%d cores max)\n", omp_get_max_threads());
#endif

  #pragma omp parallel for schedule(dynamic, 1)
  for(int iy=0;iy<NY;iy++){
    for(int ix=0;ix<NX;ix++){
      float x=g_worldLo[0]+ix*G+G*0.5f, y=g_worldLo[1]+iy*G+G*0.5f;
      float z=hiZ+16.0f; int guard=0;
      while(z>loZ+8.0f && guard++<24){
        Vector s(x,y,z), e(x,y,loZ-16.0f);
        TraceRes r=traceHull(s,e,8.0f,4.0f,true);
        if(r.frac>=1.0f) break;
        float gz = s.z + (e.z-s.z)*r.frac;
        
        // ---- Stance Support Validation (Ban Railings, Lamps, Wires, and Narrow Ledges) ----
        // A character cannot stand on a skinny railing or lamp fixture. We check 4 surrounding points at distance 12u.
        // If at least 3 of 4 points do not have floor within +-20u of gz, it's a railing or lamp!
        int supportCount = 0;
        const float sx[4] = {-12.0f, 12.0f, 0.0f, 0.0f}, sy[4] = {0.0f, 0.0f, -12.0f, 12.0f};
        for(int sp=0; sp<4; sp++){
          Vector ss(x + sx[sp], y + sy[sp], gz + 20.0f), se(x + sx[sp], y + sy[sp], gz - 20.0f);
          TraceRes sr = traceHull(ss, se, 2.0f, 2.0f, true);
          if(!sr.startsolid && sr.frac < 1.0f) supportCount++;
        }
        if(supportCount < 3){
          z = gz - 28.0f; continue; // Rejected: narrow railing, street lamp, wire, or fence!
        }

        int hr=headroom(x,y,gz);
        if(hr>0){
          GCell c; c.ix=ix; c.iy=iy; c.z=gz; c.state=(unsigned char)hr;
          rowCells[iy].push_back(c);
        }
        z = gz - 28.0f;   // continue below to find lower floors
      }
    }
    int done = ++sampleProgress;
    #pragma omp critical(prog_sample)
    {
      printProgress("Sampling Geometry", done, NY, tSample);
    }
  }
  
  for(int iy=0; iy<NY; iy++){
    for(size_t c=0; c<rowCells[iy].size(); c++){
      col[iy*NX + rowCells[iy][c].ix].push_back((int)cells.size());
      cells.push_back(rowCells[iy][c]);
    }
  }
  printf("walkable cells: %d\n",(int)cells.size());
  if(cells.empty()){ printf("no walkable ground found\n"); return 2; }

  // ---- connections (collision-validated so no through-wall links) ----
  std::vector<std::vector<int> > conn(cells.size());
  std::vector<unsigned short> attr(cells.size(), 0);   // u16: must hold NAVATTR_AVOID (0x800)
  for(size_t i=0;i<cells.size();i++) if(cells[i].state==1) attr[i]|=NAVATTR_CROUCH;
  const int DX[4]={1,-1,0,0}, DY[4]={0,0,1,-1};
  std::vector<unsigned char> jumpFlag(cells.size(), 0);
  std::atomic<long> links(0);
  std::atomic<int> connProgress(0);
  auto tConn = std::chrono::steady_clock::now();

  #pragma omp parallel for schedule(dynamic, 16)
  for(int i=0;i<(int)cells.size();i++){
    GCell&ci=cells[i];
    float cx=g_worldLo[0]+ci.ix*G+G*0.5f, cy=g_worldLo[1]+ci.iy*G+G*0.5f;
    Vector a(cx,cy,ci.z);
    long myLinks = 0;
    for(int d=0;d<4;d++){
      bool connectedDir = false;
      for(int step=1; step<=2 && !connectedDir; step++){
        int nx=ci.ix+step*DX[d], ny=ci.iy+step*DY[d];
        if(nx<0||ny<0||nx>=NX||ny>=NY) continue;
        const std::vector<int>&nc=col[ny*NX+nx];
        for(size_t k=0;k<nc.size();k++){
          int j=nc[k]; GCell&cj=cells[j];
          float bx=g_worldLo[0]+cj.ix*G+G*0.5f, by=g_worldLo[1]+cj.iy*G+G*0.5f;
          Vector b(bx,by,cj.z);
          int t=connType(a,b);
          if(!t) continue;
          conn[i].push_back(j); myLinks++;
          if(t==2) jumpFlag[j] = 1;   // must jump UP into cj -> mark cj
          connectedDir = true;
        }
      }
    }
    links += myLinks;
    int done = ++connProgress;
    #pragma omp critical(prog_conn)
    {
      printProgress("Building Links", done, (int)cells.size(), tConn);
    }
  }
  for(size_t i=0; i<cells.size(); i++) if(jumpFlag[i]) attr[i] |= NAVATTR_JUMP;
  printf("connections: %ld\n",(long)links);

  // ---- Forward-Only Reachability BFS from Player Seeds ----
  // Using a directed forward BFS prevents searching backwards up impossible one-way drops,
  // permanently eliminating inaccessible rooftops and out-of-bounds ledges!
  std::vector<Vector> seeds; parseSeeds(bsp, seeds);
  printf("seeds: %d\n", (int)seeds.size());

  std::vector<unsigned char> reach(cells.size(), 0);
  std::vector<int> q;
  if(!seeds.empty()){
    for(size_t s=0; s<seeds.size(); s++){
      float bestDist = 1e9f; int bestC = -1;
      for(size_t i=0; i<cells.size(); i++){
        float cx=g_worldLo[0]+cells[i].ix*G+G*0.5f, cy=g_worldLo[1]+cells[i].iy*G+G*0.5f;
        float dx = cx - seeds[s].x, dy = cy - seeds[s].y, dz = cells[i].z - seeds[s].z;
        if(fabs(dz) > 350.0f) continue;
        float dist2 = dx*dx + dy*dy + dz*dz;
        if(dist2 < bestDist){ bestDist = dist2; bestC = (int)i; }
      }
      if(bestC != -1 && bestDist < 250000.0f && !reach[bestC]){
        reach[bestC] = 1; q.push_back(bestC);
      }
    }
  }

  // Fallback if map has no usable spawn entities: start from the center-most cells of the largest undirected component
  if(q.empty()){
    std::vector<std::vector<int> > undir(cells.size());
    for(size_t i=0;i<cells.size();i++)
      for(size_t k=0;k<conn[i].size();k++){ int j=conn[i][k]; undir[i].push_back(j); undir[j].push_back((int)i); }
    std::vector<int> comp(cells.size(), -1); int bestComp=-1, bestSize=0, nComp=0;
    for(size_t s=0;s<cells.size();s++){
      if(comp[s]!=-1) continue;
      std::vector<int> tq; tq.push_back((int)s); comp[s]=nComp; int sz=0;
      for(size_t h=0;h<tq.size();h++){ int i=tq[h]; sz++;
        for(size_t k=0;k<undir[i].size();k++){ int j=undir[i][k]; if(comp[j]==-1){comp[j]=nComp;tq.push_back(j);} } }
      if(sz>bestSize){bestSize=sz;bestComp=nComp;}
      nComp++;
    }
    for(size_t i=0;i<cells.size();i++) if(comp[i]==bestComp){ reach[i]=1; q.push_back((int)i); }
    printf("no spawn seeds matched, defaulted to largest region (%d cells)\n", (int)q.size());
  } else {
    // Run forward directed BFS from spawns
    for(size_t h=0; h<q.size(); h++){
      int i = q[h];
      for(size_t k=0; k<conn[i].size(); k++){
        int j = conn[i][k];
        if(!reach[j]){ reach[j] = 1; q.push_back(j); }
      }
    }
    printf("kept (forward reachable from spawn points): %d of %d cells\n", (int)q.size(), (int)cells.size());
  }

  // ---- Greedy Rectangular Auto-Merge ----
  struct GenArea {
    int id, active;
    unsigned short attr;
    float loX, loY, loZ, hiX, hiY, hiZ;
    std::vector<int> conn;
  };
  std::vector<GenArea> genAreas;
  std::vector<int> cellToGen(cells.size(), -1);
  int jumpAreas=0, crouchAreas=0, avoidAreas=0;
  for(size_t i=0; i<cells.size(); i++){
    if(!reach[i]) continue;
    if(markAvoid && (attr[i]&(NAVATTR_JUMP|NAVATTR_CROUCH))) attr[i]|=NAVATTR_AVOID;
    if(attr[i]&NAVATTR_JUMP) jumpAreas++;
    if(attr[i]&NAVATTR_CROUCH) crouchAreas++;
    if(attr[i]&NAVATTR_AVOID) avoidAreas++;

    GenArea ga;
    ga.id = (int)genAreas.size(); ga.active = 1; ga.attr = attr[i];
    ga.loX = g_worldLo[0]+cells[i].ix*G; ga.hiX = ga.loX + G;
    ga.loY = g_worldLo[1]+cells[i].iy*G; ga.hiY = ga.loY + G;
    ga.loZ = cells[i].z; ga.hiZ = cells[i].z;
    cellToGen[i] = (int)genAreas.size();
    genAreas.push_back(ga);
  }
  printf("attrs before merge: jump=%d crouch=%d avoid=%d%s\n", jumpAreas, crouchAreas, avoidAreas,
         markAvoid?" (jump/crouch marked AVOID)":"");

  for(size_t i=0; i<cells.size(); i++){
    if(cellToGen[i] < 0) continue;
    GenArea& ga = genAreas[cellToGen[i]];
    for(size_t k=0; k<conn[i].size(); k++){
      int tg = cellToGen[conn[i][k]];
      if(tg >= 0 && tg != ga.id) ga.conn.push_back(tg);
    }
  }

  std::vector<std::vector<int> > incoming(genAreas.size());
  for(size_t n=0; n<genAreas.size(); n++){
    for(size_t m=0; m<genAreas[n].conn.size(); m++){
      incoming[genAreas[n].conn[m]].push_back((int)n);
    }
  }

  bool merged = true;
  int mergeCount = 0;
  int mergePass = 0;
  auto tMerge = std::chrono::steady_clock::now();
  while(merged){
    merged = false;
    mergePass++;
    for(size_t i=0; i<genAreas.size(); i++){
      if(mergePass == 1){
        printProgress("Merging Areas", (int)(i + 1), (int)genAreas.size(), tMerge);
      }
      if(!genAreas[i].active) continue;
      GenArea& a = genAreas[i];
      for(size_t k=0; k<a.conn.size(); k++){
        int j = a.conn[k];
        if(j <= (int)i || !genAreas[j].active) continue;
        GenArea& b = genAreas[j];
        if(a.attr != b.attr) continue;
        if(fabs(a.loZ - b.loZ) > 3.0f || fabs(a.hiZ - b.hiZ) > 3.0f) continue;

        bool canMergeX = false, canMergeY = false;
        if(fabs(a.loY - b.loY) < 1.0f && fabs(a.hiY - b.hiY) < 1.0f){
          if(fabs(a.hiX - b.loX) < 1.0f || fabs(b.hiX - a.loX) < 1.0f) canMergeX = true;
        }
        if(fabs(a.loX - b.loX) < 1.0f && fabs(a.hiX - b.hiX) < 1.0f){
          if(fabs(a.hiY - b.loY) < 1.0f || fabs(b.hiY - a.loY) < 1.0f) canMergeY = true;
        }
        if(!canMergeX && !canMergeY) continue;

        bool bConnectsToA = false;
        for(size_t m=0; m<b.conn.size(); m++) if(b.conn[m] == (int)i){ bConnectsToA = true; break; }
        if(!bConnectsToA) continue;

        if(b.loX < a.loX) a.loX = b.loX;
        if(b.hiX > a.hiX) a.hiX = b.hiX;
        if(b.loY < a.loY) a.loY = b.loY;
        if(b.hiY > a.hiY) a.hiY = b.hiY;
        if(b.loZ < a.loZ) a.loZ = b.loZ;
        if(b.hiZ > a.hiZ) a.hiZ = b.hiZ;

        for(size_t m=0; m<b.conn.size(); m++){
          int target = b.conn[m];
          if(target == (int)i || target == j) continue;
          bool dup = false;
          for(size_t t=0; t<a.conn.size(); t++) if(a.conn[t] == target){ dup = true; break; }
          if(!dup){
            a.conn.push_back(target);
            incoming[target].push_back((int)i);
          }
        }
        for(size_t idx=0; idx<incoming[j].size(); idx++){
          int n = incoming[j][idx];
          if(n < 0 || n >= (int)genAreas.size() || !genAreas[n].active || n == (int)i || n == j) continue;
          for(size_t m=0; m<genAreas[n].conn.size(); m++){
            if(genAreas[n].conn[m] == j){
              genAreas[n].conn[m] = (int)i;
              incoming[i].push_back(n);
              for(size_t d=0; d<m; d++){
                if(genAreas[n].conn[d] == (int)i){
                  genAreas[n].conn.erase(genAreas[n].conn.begin() + m);
                  m--; break;
                }
              }
            }
          }
        }
        for(size_t t=0; t<a.conn.size(); t++){
          if(a.conn[t] == j){ a.conn.erase(a.conn.begin() + t); t--; }
        }
        b.active = 0;
        merged = true;
        mergeCount++;
        break;
      }
    }
  }
  printProgress("Merging Areas", (int)genAreas.size(), (int)genAreas.size(), tMerge);
  
  int finalCount = 0;
  std::vector<int> finalId(genAreas.size(), -1);
  for(size_t i=0; i<genAreas.size(); i++){
    if(genAreas[i].active) finalId[i] = finalCount++;
  }
  printf("auto-merged %d tiles -> final area count: %d\n", mergeCount, finalCount);

  // ---- write v9 .nav ----
  std::vector<unsigned char> o;
  W32(o,0xFEEDFACE); W32(o,9); W32(o,0);    // magic, version, bsp size
  W16(o,0);                                 // place-name directory count
  W32(o,(uint32_t)finalCount);              // area count
  long totalLinks = 0;
  for(size_t i=0; i<genAreas.size(); i++){
    if(!genAreas[i].active) continue;
    GenArea& c = genAreas[i];
    float cx = (c.loX + c.hiX) * 0.5f, cy = (c.loY + c.hiY) * 0.5f;
    W32(o,(uint32_t)(finalId[i]+1));        // id
    W16(o,c.attr);                          // attributes
    WF(o,c.loX); WF(o,c.loY); WF(o,c.loZ);  // lo
    WF(o,c.hiX); WF(o,c.hiY); WF(o,c.hiZ);  // hi
    WF(o,c.hiZ); WF(o,c.loZ);               // neZ, swZ
    std::vector<uint32_t> dir[4];
    for(size_t k=0; k<c.conn.size(); k++){
      int j = c.conn[k]; if(finalId[j]<0 || j==(int)i) continue;
      GenArea& cj = genAreas[j];
      int dd = 0; // 0=North (-Y), 1=East (+X), 2=South (+Y), 3=West (-X)
      if(fabs(cj.loX - c.hiX) <= 4.0f) dd = 1;      // East (+X)
      else if(fabs(cj.hiX - c.loX) <= 4.0f) dd = 3; // West (-X)
      else if(fabs(cj.loY - c.hiY) <= 4.0f) dd = 2; // South (+Y)
      else if(fabs(cj.hiY - c.loY) <= 4.0f) dd = 0; // North (-Y)
      else {
        float dx = ((cj.loX+cj.hiX)-(c.loX+c.hiX));
        float dy = ((cj.loY+cj.hiY)-(c.loY+c.hiY));
        if(fabs(dx) > fabs(dy)) dd = (dx > 0) ? 1 : 3;
        else dd = (dy > 0) ? 2 : 0;
      }
      dir[dd].push_back((uint32_t)(finalId[j]+1));
      totalLinks++;
    }
    for(int d=0;d<4;d++){ W32(o,(uint32_t)dir[d].size()); for(size_t m=0;m<dir[d].size();m++) W32(o,dir[d][m]); }
    W8(o,0);            // hiding spots
    W8(o,0);            // approach areas
    W32(o,0);           // encounter paths
    W16(o,0);           // place index
    W32(o,0); W32(o,0); // ladder connections (up/down)
    WF(o,0); WF(o,0);   // earliest-occupy times
  }
  W32(o,0);             // mesh ladder count

  FILE*fo=fopen(out,"wb"); if(!fo){printf("cannot write %s\n",out);return 3;}
  fwrite(&o[0],1,o.size(),fo); fclose(fo);
  printf("wrote %s: %d areas, %ld links, %d jump / %d crouch marked (%d bytes)\n",
    out,finalCount,totalLinks,jumpAreas,crouchAreas,(int)o.size());

  // ---- self-verify: load with the plugin's own parser + A* sanity ----
  CNavMesh m;
  if(!m.Load(out)){ printf("*** VERIFY FAIL: plugin parser could not load the output!\n"); return 4; }
  printf("verify: parser loaded %d areas\n", m.Count());
  int okp=0; unsigned rng=999; int path[512];
  for(int t=0;t<40;t++){
    rng=rng*1103515245+12345; int s=(rng>>8)%m.Count();
    rng=rng*1103515245+12345; int g=(rng>>8)%m.Count();
    if(m.BuildPath(s,g,path,512)>0) okp++;
  }
  printf("verify: A* connected %d/40 random pairs\n", okp);
  return 0;
}
