// ============================================================================
// navdoctor - validates a Source .nav mesh against the map's REAL collision
// geometry (.bsp), by simulating a player-sized hull walking every connection.
// Connections that no hull (standing or crouched, with step-ups and jumps) can
// physically traverse are cut. Higher destination areas whose every known
// incoming route exceeds the bot jump rise are removed entirely. Areas inside
// verified tight corridors and doorways are marked PRECISE, and a repaired .nav
// is written.
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
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

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
struct ddispvert_t { Vector vec; float dist,alpha; };
#pragma pack(pop)

#define C_SOLID 0x1
#define C_WINDOW 0x2
#define C_GRATE 0x8
#define C_PLAYERCLIP 0x10000
#define C_LADDER 0x20000000
#define SOLID_MASK (C_SOLID|C_WINDOW|C_GRATE|C_PLAYERCLIP)

// ---------------------------------------------------------------- collision db
struct BrushSideP { Vector n; float d; };
struct CBrush { int firstSide,numSides; Vector lo,hi; };
struct Tri { Vector a,b,c; };

static std::vector<BrushSideP> g_sides;      // flattened side planes per brush
static std::vector<CBrush>     g_brushes;    // world solid brushes only
static std::vector<Tri>        g_tris;       // displacement triangles

struct BspLadderBounds { Vector mins,maxs; int normalAxis; };
static std::vector<BspLadderBounds> g_bspLadders;

// spatial grid
static float g_worldLo[3], g_worldHi[3];
static int   g_gx, g_gy;                     // cells in x,y (z ignored)
static const float CELL = 256.0f;
static std::vector<std::vector<int> > g_cellBrush, g_cellTri;
static unsigned g_stamp=1;
static std::vector<unsigned> g_brushStamp, g_triStamp;

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

static bool entityValue(const std::string&block,const char*key,std::string&value){
  std::string needle="\""+std::string(key)+"\"";
  size_t p=block.find(needle);if(p==std::string::npos)return false;
  p+=needle.size();while(p<block.size()&&(block[p]==' '||block[p]=='\t'||block[p]=='\r'||block[p]=='\n'))p++;
  if(p>=block.size()||block[p]!='\"')return false;p++;
  size_t e=block.find('\"',p);if(e==std::string::npos)return false;
  value=block.substr(p,e-p);return true;
}

static bool entityFloat(const std::string&block,const char*key,float&value){
  std::string text;if(!entityValue(block,key,text))return false;value=(float)atof(text.c_str());return true;
}

// ---------------------------------------------------------------- BSP loading
static bool loadBsp(const char*path){
  // Batch mode reuses this process for many maps. Do not let collision from a
  // previous BSP leak into the next map or retain its large scratch buffers.
  g_sides.clear();g_brushes.clear();g_tris.clear();g_bspLadders.clear();
  g_cellBrush.clear();g_cellTri.clear();g_brushStamp.clear();g_triStamp.clear();
  g_gx=g_gy=0;g_stamp=1;
  FILE*fp=fopen(path,"rb"); if(!fp){printf("no bsp\n");return false;}
  fseek(fp,0,SEEK_END); long sz=ftell(fp); fseek(fp,0,SEEK_SET);
  unsigned char*d=(unsigned char*)malloc(sz);
  if(!d){fclose(fp);printf("out of memory loading bsp\n");return false;}
  if(fread(d,1,sz,fp)!=(size_t)sz){free(d);fclose(fp);return false;}
  fclose(fp);
  dheader_t*h=(dheader_t*)d;
  if(h->ident!=('V'|('B'<<8)|('S'<<16)|('P'<<24))){printf("bad magic\n");free(d);return false;}
  printf("BSP version %d\n",h->version);

  // VBSP preserves climbable brushes as info_ladder entities containing the
  // absolute mins/maxs used by Source's own nav_generate ladder pass.
  g_bspLadders.clear();
  if(h->lumps[0].ofs>=0&&h->lumps[0].len>0&&h->lumps[0].ofs+h->lumps[0].len<=sz){
    std::string entities((const char*)d+h->lumps[0].ofs,(size_t)h->lumps[0].len);
    size_t p=0;
    while((p=entities.find('{',p))!=std::string::npos){
      size_t e=entities.find('}',p+1);if(e==std::string::npos)break;
      std::string block=entities.substr(p,e-p+1),classname;
      if(entityValue(block,"classname",classname)&&classname=="info_ladder"){
        BspLadderBounds ladder;ladder.normalAxis=-1;
        if(entityFloat(block,"mins.x",ladder.mins.x)&&entityFloat(block,"mins.y",ladder.mins.y)&&
           entityFloat(block,"mins.z",ladder.mins.z)&&entityFloat(block,"maxs.x",ladder.maxs.x)&&
           entityFloat(block,"maxs.y",ladder.maxs.y)&&entityFloat(block,"maxs.z",ladder.maxs.z))
          g_bspLadders.push_back(ladder);
      }
      p=e+1;
    }
  }
  int entityLadderCount=(int)g_bspLadders.size();

  #define LUMP(i,T) ((T*)(d+h->lumps[i].ofs)); int n_##T = h->lumps[i].len/(int)sizeof(T)
  dplane_t   *planes = LUMP(1,dplane_t);   (void)n_dplane_t;
  Vector     *verts  = (Vector*)(d+h->lumps[3].ofs);
  dnode_t    *nodes  = LUMP(5,dnode_t);    (void)n_dnode_t;
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
        unsigned short flb=*(unsigned short*)(L+24);   // firstleafbrush
        unsigned short nlb=*(unsigned short*)(L+26);   // numleafbrushes
        for(int k=0;k<nlb;k++){ int b=leafbrushes[flb+k]; if(b<nBrushes) isWorld[b]=1; }
      }
    }
  }

  // -- solid world brushes. NOTE: we deliberately do NOT try to exclude
  // displacement base brushes by the brushside dispinfo field (its "none"
  // sentinel is unreliable across compilers). The engine's own collision set is
  // "brushes referenced by the world model's leaves" - exactly what we walk -
  // and the ground self-test below verifies the result against the nav's floor
  // heights before any cutting is allowed.
  int worldCount=0;
  for(int b=0;b<nBrushes;b++){
    if(!isWorld[b]) continue;
    if(!(brushes[b].contents & SOLID_MASK)) continue;
    CBrush cb; cb.firstSide=(int)g_sides.size(); cb.numSides=brushes[b].numsides;
    cb.lo=Vector(1e9f,1e9f,1e9f); cb.hi=Vector(-1e9f,-1e9f,-1e9f);
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
    // Older IMIC maps commonly compile climb volumes as world brushes with
    // CONTENTS_LADDER but do not emit info_ladder entities.  Treat the brush
    // bounds as the same authoritative ladder volume; otherwise discovery
    // reports zero ladders on maps such as ins_samawah.
    if(brushes[b].contents&C_LADDER){
      BspLadderBounds ladder;ladder.mins=cb.lo;ladder.maxs=cb.hi;
      ladder.normalAxis=-1;float bestTilt=0.0f;
      // Pitched ladder brushes have an inflated AABB along their surface
      // normal, so comparing X/Y extents rotates their facing by 90 degrees.
      // Their broad climb faces retain a mostly-horizontal plane normal with
      // a modest Z component.  Recover the dominant horizontal normal axis
      // from that plane and reserve the AABB heuristic for vertical brushes.
      for(int s=0;s<cb.numSides;s++){
        const BrushSideP&sp=g_sides[cb.firstSide+s];
        float az=fabsf(sp.n.z),h2=sp.n.x*sp.n.x+sp.n.y*sp.n.y;
        if(az<0.05f||az>0.90f||h2<0.50f||az<=bestTilt)continue;
        bestTilt=az;ladder.normalAxis=fabsf(sp.n.x)>=fabsf(sp.n.y)?0:1;
      }
      g_bspLadders.push_back(ladder);
    }
    g_brushes.push_back(cb); worldCount++;
  }
  printf("world solid brushes kept=%d of %d\n",worldCount,nBrushes);
  printf("BSP ladders: info_ladder=%d CONTENTS_LADDER brushes=%d total=%d\n",
    entityLadderCount,(int)g_bspLadders.size()-entityLadderCount,(int)g_bspLadders.size());

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
  // All brush planes and displacement triangles have been copied into the
  // collision database. Release the raw BSP now, which is essential when a
  // batch contains several large maps.
  free(d);
  return true;
}

// ---------------------------------------------------------------- tracing
// swept AABB vs brushes; box given as feet-origin: lo=(-hw,-hw,0) hi=(hw,hw,hh)
struct TraceRes { float frac; bool startsolid; };

static TraceRes traceBrushes(const Vector&start,const Vector&end,float hw,float hh){
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

static TraceRes traceHull(const Vector&start,const Vector&end,float hw,float hh){
  TraceRes r=traceBrushes(start,end,hw,hh);
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

// Detect a genuinely constrained passage axis using world-brush collision.
// Both opposite sides must hit: a nav area merely beside one wall is not a
// corridor. Width includes the real player hull, not just center travel room.
static bool tightPassageAxis(const Vector&feet,float dx,float dy,float hh,float&outWidth){
  static const float HULL_HALF_WIDTH=16.4f;
  static const float PROBE_DISTANCE=64.0f;
  static const float MAX_PRECISE_PASSAGE_WIDTH=112.0f;
  Vector step(dx*PROBE_DISTANCE,dy*PROBE_DISTANCE,0);
  TraceRes positive=traceBrushes(feet,feet+step,HULL_HALF_WIDTH,hh);
  TraceRes negative=traceBrushes(feet,feet-step,HULL_HALF_WIDTH,hh);
  if(positive.startsolid||negative.startsolid) return false;
  bool hitPositive=positive.frac<0.999f, hitNegative=negative.frac<0.999f;
  outWidth=HULL_HALF_WIDTH*2.0f+PROBE_DISTANCE*(positive.frac+negative.frac);
  return hitPositive&&hitNegative&&outWidth<=MAX_PRECISE_PASSAGE_WIDTH+0.5f;
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

struct MoveEval {
  bool pass,standPass,crouchPass,noJumpPass,tooHigh;
  float minRise;
  MoveEval():pass(false),standPass(false),crouchPass(false),noJumpPass(false),tooHigh(false),minRise(1e9f){}
};

// Evaluate a directed area crossing at three points along its portal. The four
// outcomes let the caller distinguish ordinary walking, crouch-only movement,
// and a movement that genuinely needs the 50-unit jump attempt.
static MoveEval evaluateMove(const CNavMesh&m,int fromIdx,int toIdx){
  MoveEval result;
  const NavAreaData*a=m.Area(fromIdx),*b=m.Area(toIdx); if(!a||!b)return result;
  float lx=a->lo.x>b->lo.x?a->lo.x:b->lo.x, hx=a->hi.x<b->hi.x?a->hi.x:b->hi.x;
  float ly=a->lo.y>b->lo.y?a->lo.y:b->lo.y, hy=a->hi.y<b->hi.y?a->hi.y:b->hi.y;
  bool adjacent=!(lx>hx+24.0f||ly>hy+24.0f);
  float drop=a->center.z-b->center.z;
  float maxDrop=drop>0?drop+120.0f:90.0f;
  bool crouchA=(a->attributes&NAVATTR_CROUCH)!=0,crouchB=(b->attributes&NAVATTR_CROUCH)!=0;
  static const float TS[3]={0.5f,0.2f,0.8f};
  for(int s=0;s<3;s++){
    Vector S,E;
    if(adjacent){
      float px=lx==hx?lx:lx+(hx-lx)*TS[s];
      float py=ly==hy?ly:ly+(hy-ly)*TS[s];
      if(hx-lx<0.1f&&hy-ly<0.1f&&s>0)break;
      float amx=(a->lo.x+a->hi.x)*0.5f,amy=(a->lo.y+a->hi.y)*0.5f;
      float bmx=(b->lo.x+b->hi.x)*0.5f,bmy=(b->lo.y+b->hi.y)*0.5f;
      float dax=amx-px,day=amy-py,al=sqrtf(dax*dax+day*day);if(al>1){dax/=al;day/=al;}
      float dbx=bmx-px,dby=bmy-py,bl=sqrtf(dbx*dbx+dby*dby);if(bl>1){dbx/=bl;dby/=bl;}
      float ain=al<20?al:20,bin=bl<20?bl:20;
      S=Vector(px+dax*ain,py+day*ain,m.GetZ(fromIdx,px+dax*ain,py+day*ain)+2);
      E=Vector(px+dbx*bin,py+dby*bin,m.GetZ(toIdx,px+dbx*bin,py+dby*bin)+2);
    }else{
      if(s>0)break;
      float bx=b->center.x,by=b->center.y;
      float cx=bx<a->lo.x?a->lo.x:(bx>a->hi.x?a->hi.x:bx);
      float cy=by<a->lo.y?a->lo.y:(by>a->hi.y?a->hi.y:by);
      S=Vector(cx,cy,m.GetZ(fromIdx,cx,cy)+2);
      E=Vector(bx,by,m.GetZ(toIdx,bx,by)+2);
    }
    float rise=E.z-S.z;if(rise<result.minRise)result.minRise=rise;
    bool standNoJump=false,standAny=false;
    if(!crouchA&&!crouchB){
      standNoJump=walkSim(S,E,15.0f,70.0f,maxDrop,false);
      standAny=standNoJump||walkSim(S,E,15.0f,70.0f,maxDrop,true);
    }
    bool crouchNoJump=walkSim(S,E,15.0f,34.0f,maxDrop,false);
    bool crouchAny=crouchNoJump||walkSim(S,E,15.0f,34.0f,maxDrop,true);
    result.standPass=result.standPass||standAny;
    result.crouchPass=result.crouchPass||crouchAny;
    result.noJumpPass=result.noJumpPass||standNoJump||crouchNoJump;
  }
  result.tooHigh=result.minRise>50.5f;
  result.pass=!result.tooHigh&&(result.standPass||result.crouchPass);
  return result;
}

static bool hasDirectedLink(const CNavMesh&m,int fromIdx,int toIdx){
  const NavAreaData*a=m.Area(fromIdx);if(!a)return false;
  for(int k=0;k<a->connCount;k++)if(a->connIdx[k]==toIdx)return true;
  return false;
}

// Exact/near edge direction for conservative automatic link discovery.
// Source NAV directions: north=0, east=1, south=2, west=3.
static int nearbyEdgeDirection(const NavAreaData*a,const NavAreaData*b){
  // nav_generate samples on a 25u grid, so two otherwise adjacent rectangles
  // can legitimately be separated by one complete generation step. Keep a
  // small floating-point allowance; evaluateMove still has to prove that a
  // real player hull can cross before the link is written.
  static const float MAX_GAP=26.0f,MIN_OVERLAP=20.0f;
  float xOverlap=(a->hi.x<b->hi.x?a->hi.x:b->hi.x)-(a->lo.x>b->lo.x?a->lo.x:b->lo.x);
  float yOverlap=(a->hi.y<b->hi.y?a->hi.y:b->hi.y)-(a->lo.y>b->lo.y?a->lo.y:b->lo.y);
  if(yOverlap>=MIN_OVERLAP&&fabsf(a->hi.x-b->lo.x)<=MAX_GAP)return 1;
  if(yOverlap>=MIN_OVERLAP&&fabsf(a->lo.x-b->hi.x)<=MAX_GAP)return 3;
  if(xOverlap>=MIN_OVERLAP&&fabsf(a->hi.y-b->lo.y)<=MAX_GAP)return 2;
  if(xOverlap>=MIN_OVERLAP&&fabsf(a->lo.y-b->hi.y)<=MAX_GAP)return 0;
  return -1;
}

// A short walk simulation can stop within one 14u step of its target. Across a
// 25u generation gap that tolerance used to count "reached the far area" even
// when the bot had only walked up to a wall. Before proposing a missing link,
// trace torso and head lines between points just inside both candidate areas.
// Low jumpable cover may block the torso only; a structural wall blocks both.
static bool edgeCrossingHasNoWall(const CNavMesh&m,int fromIdx,int toIdx,int dir){
  const NavAreaData*a=m.Area(fromIdx),*b=m.Area(toIdx);if(!a||!b)return false;
  float ax,ay,bx,by;
  if(dir==NAV_EAST||dir==NAV_WEST){
    float lo=a->lo.y>b->lo.y?a->lo.y:b->lo.y;
    float hi=a->hi.y<b->hi.y?a->hi.y:b->hi.y;
    if(hi<=lo)return false;ay=by=(lo+hi)*0.5f;
    if(dir==NAV_EAST){ax=a->hi.x-2.0f;bx=b->lo.x+2.0f;}
    else{ax=a->lo.x+2.0f;bx=b->hi.x-2.0f;}
  }else{
    float lo=a->lo.x>b->lo.x?a->lo.x:b->lo.x;
    float hi=a->hi.x<b->hi.x?a->hi.x:b->hi.x;
    if(hi<=lo)return false;ax=bx=(lo+hi)*0.5f;
    if(dir==NAV_SOUTH){ay=a->hi.y-2.0f;by=b->lo.y+2.0f;}
    else{ay=a->lo.y+2.0f;by=b->hi.y-2.0f;}
  }
  float az=m.GetZ(fromIdx,ax,ay),bz=m.GetZ(toIdx,bx,by);
  TraceRes torso=traceBrushes(Vector(ax,ay,az+36.0f),Vector(bx,by,bz+36.0f),0.0f,0.0f);
  TraceRes head=traceBrushes(Vector(ax,ay,az+60.0f),Vector(bx,by,bz+60.0f),0.0f,0.0f);
  return !torso.startsolid&&!head.startsolid&&torso.frac>=0.995f&&head.frac>=0.995f;
}

// ---------------------------------------------------------------- validation
struct CutRec { unsigned fromId,toId; Vector fromC,toC; float distStuck; };
struct AddRec { unsigned fromId,toId; int direction; Vector fromC,toC; bool jump,crouch; };
struct LadderPlan {
  unsigned id; float width; Vector top,bottom; float length; int dir;
  unsigned topForward,topLeft,topRight,topBehind,bottomArea;
  bool isNew;
};

static bool containsId(const std::vector<unsigned>&ids,unsigned id){
  for(size_t i=0;i<ids.size();i++) if(ids[i]==id) return true;
  return false;
}

static bool containsCut(const std::vector<CutRec>&cuts,unsigned from,unsigned to){
  for(size_t i=0;i<cuts.size();i++)if(cuts[i].fromId==from&&cuts[i].toId==to)return true;
  return false;
}

static int areaIndexById(const CNavMesh&m,unsigned id){
  for(int i=0;i<m.Count();i++)if(m.Area(i)->id==id)return i;
  return -1;
}

static unsigned areaIdOrZero(const CNavMesh&m,int idx){
  const NavAreaData*a=m.Area(idx);return a?a->id:0;
}

static void ladderNormal(int dir,float&nx,float&ny){
  nx=ny=0;
  if(dir==NAV_NORTH)ny=-1;else if(dir==NAV_SOUTH)ny=1;
  else if(dir==NAV_EAST)nx=1;else if(dir==NAV_WEST)nx=-1;
}

static void ladderDirVector(int dir,float&x,float&y){
  x=y=0;if(dir==NAV_NORTH)y=-1;else if(dir==NAV_SOUTH)y=1;
  else if(dir==NAV_EAST)x=1;else if(dir==NAV_WEST)x=-1;
}

// Source's GetNavArea(pos, beneathLimit): select the highest NAV surface that
// contains the XY sample and lies no farther than beneathLimit below it.
static int ladderAreaUnderPoint(const CNavMesh&m,const Vector&pos,float beneathLimit,int exclude){
  int best=-1;float bestZ=-1e30f;
  for(int i=0;i<m.Count();i++){
    if(i==exclude)continue;const NavAreaData*a=m.Area(i);
    if(pos.x<a->lo.x-0.1f||pos.x>a->hi.x+0.1f||pos.y<a->lo.y-0.1f||pos.y>a->hi.y+0.1f)continue;
    float z=m.GetZ(i,pos.x,pos.y);float below=pos.z-z;
    if(below < -18.0f || below > beneathLimit)continue;
    if(z>bestZ){bestZ=z;best=i;}
  }
  return best;
}

// Faithful counterpart of Source 2006 findFirstAreaInDirection(): move in
// 25-unit generation steps, stop at structural BSP collision, and return the
// first NAV surface beneath a sample. This is substantially safer than a
// radial nearest-area search around a ladder lip.
static int firstLadderAreaInDirection(const CNavMesh&m,const Vector&start,int dir,
                                      float range,float beneathLimit,int exclude){
  float dx,dy;ladderDirVector(dir,dx,dy);int steps=(int)(range/25.0f+0.5f);
  for(int s=1;s<=steps;s++){
    Vector p=start+Vector(dx*25.0f*s,dy*25.0f*s,0);
    // The SDK uses a zero-width MASK_PLAYERSOLID_BRUSHONLY line here. A hull
    // probe can start embedded in the ladder lip/ceiling and falsely reject a
    // clear dismount direction.
    TraceRes tr=traceBrushes(start,p,0.0f,0.0f);
    if(tr.startsolid||tr.frac<0.995f)break;
    int area=ladderAreaUnderPoint(m,p,beneathLimit,exclude);if(area>=0)return area;
  }
  return -1;
}

// Find a floor area near one of Source's ladder connection probes. The closest
// point on the rectangle is used (not its center), and a small hull trace keeps
// the search from selecting a landing through a structural wall.
static int nearestLadderArea(const CNavMesh&m,const Vector&probe,float refZ,
                             float radius,float minDz,float maxDz,int exclude){
  int best=-1;float bestScore=1e30f;
  for(int i=0;i<m.Count();i++){
    if(i==exclude)continue;const NavAreaData*a=m.Area(i);
    float x=probe.x,y=probe.y;
    if(x<a->lo.x)x=a->lo.x;else if(x>a->hi.x)x=a->hi.x;
    if(y<a->lo.y)y=a->lo.y;else if(y>a->hi.y)y=a->hi.y;
    float z=m.GetZ(i,x,y),dz=z-refZ;if(dz<minDz||dz>maxDz)continue;
    float dx=x-probe.x,dy=y-probe.y,ds=dx*dx+dy*dy;if(ds>radius*radius)continue;
    Vector start(probe.x,probe.y,refZ+36.0f),end(x,y,z+36.0f);
    TraceRes tr=traceHull(start,end,2.0f,2.0f);
    if(tr.startsolid||tr.frac<0.995f)continue;
    float score=ds+dz*dz*0.35f;if(score<bestScore){bestScore=score;best=i;}
  }
  return best;
}

static void discoverLadders(const CNavMesh&m,std::vector<LadderPlan>&plans){
  unsigned nextId=1;
  for(int i=0;i<m.LadderCount();i++)if(m.Ladder(i)->id>=nextId)nextId=m.Ladder(i)->id+1;
  int existingMatched=0,repaired=0,rejected=0;
  std::vector<unsigned char> matchedExisting(m.LadderCount(),0);
  printf("\n=== BSP ladders to mark/connect ===\n");
  for(int li=0;li<m.LadderCount();li++){
    const NavLadderData*l=m.Ladder(li);
    printf("  NAV ladder %u stored at (%.0f %.0f %.0f..%.0f), dir=%d\n",
      l->id,l->bottom.x,l->bottom.y,l->bottom.z,l->top.z,l->dir);
  }
  for(size_t bi=0;bi<g_bspLadders.size();bi++){
    const BspLadderBounds&b=g_bspLadders[bi];
    Vector top((b.mins.x+b.maxs.x)*0.5f,(b.mins.y+b.maxs.y)*0.5f,b.maxs.z);
    Vector bottom(top.x,top.y,b.mins.z);
    float xSize=b.maxs.x-b.mins.x,ySize=b.maxs.y-b.mins.y;
    int dir;float width;
    bool normalAlongX=b.normalAxis==0||(b.normalAxis<0&&xSize<=ySize);
    if(!normalAlongX){
      // Match Source 2006 CreateLadder exactly: a zero-width, brush-only line
      // offset by GenerationStepSize, pulled 12.5u in from both ends.
      Vector s=bottom+Vector(0,25,12.5f),e=top+Vector(0,25,-12.5f);
      TraceRes tr=traceBrushes(s,e,0.0f,0.0f);dir=(tr.startsolid||tr.frac<0.995f)?NAV_NORTH:NAV_SOUTH;width=xSize;
    }else{
      Vector s=bottom+Vector(25,0,12.5f),e=top+Vector(25,0,-12.5f);
      TraceRes tr=traceBrushes(s,e,0.0f,0.0f);dir=(tr.startsolid||tr.frac<0.995f)?NAV_WEST:NAV_EAST;width=ySize;
    }
    float nx,ny;ladderNormal(dir,nx,ny);
    Vector bottomProbe=bottom+Vector(nx*16,ny*16,25);
    int bottomIdx=nearestLadderArea(m,bottomProbe,bottom.z,100,-90,70,-1);
    Vector topBase=top+Vector(nx*16,ny*16,25);
    float beneath=(top.z-bottom.z+16.0f)<120.0f?(top.z-bottom.z+16.0f):120.0f;
    // The engine generator only searches 75u and leaves otherwise usable map
    // ladders unconnected. NavDoctor extends the collision-checked search to
    // 150u; it still walks one axis at a time and stops at the first BSP wall.
    int tf=firstLadderAreaInDirection(m,topBase,(dir+2)%4,150,beneath,bottomIdx);
    int tl=firstLadderAreaInDirection(m,topBase,(dir+3)%4,150,beneath,bottomIdx);
    int tr=firstLadderAreaInDirection(m,topBase,(dir+1)%4,150,beneath,bottomIdx);
    int tb=firstLadderAreaInDirection(m,topBase,dir,150,beneath,bottomIdx);
    if(tl==tf)tl=-1;if(tr==tf||tr==tl)tr=-1;if(tb==tf||tb==tl||tb==tr)tb=-1;

    // Match existing records before deciding whether collision discovery has
    // both ends. An in-game generated ladder is authoritative evidence for an
    // endpoint that NavDoctor's incomplete static-prop collision cannot see.
    int match=-1;float best=1e30f;
    for(int li=0;li<m.LadderCount();li++){
      if(matchedExisting[li])continue;
      const NavLadderData*l=m.Ladder(li);
      // nav_generate may shorten the climb at either lip after doing player-
      // hull clearance traces. Match primarily in XY, then by vertical span
      // midpoint, instead of requiring the adjusted endpoints to equal the
      // raw info_ladder bounds.
      float horiz=xyd(l->top,top)+xyd(l->bottom,bottom);
      float navMid=(l->top.z+l->bottom.z)*0.5f,entMid=(top.z+bottom.z)*0.5f;
      float d=horiz+fabsf(navMid-entMid)*0.5f;
      if(horiz<48.0f&&fabsf(navMid-entMid)<160.0f&&d<best){best=d;match=li;}
    }
    if(match>=0){
      // One NAV record may represent only one BSP ladder. Without this claim,
      // vertically stacked entities at the same XY both matched the lower
      // record, leaving the upper ladder unwritten and sometimes inheriting
      // the lower ladder's direction/endpoints.
      matchedExisting[match]=1;
      const NavLadderData*l=m.Ladder(match);
      if(bottomIdx<0)bottomIdx=l->bottomArea;
      if(tf<0)tf=l->topForwardArea;if(tl<0)tl=l->topLeftArea;
      if(tr<0)tr=l->topRightArea;if(tb<0)tb=l->topBehindArea;
    }
    if(bottomIdx<0||(tf<0&&tl<0&&tr<0)){
      printf("  reject BSP ladder at (%.0f %.0f %.0f..%.0f): bottom=%d top=%d/%d/%d behind=%d dir=%d (no safe two-ended NAV landing)\n",
        top.x,top.y,bottom.z,top.z,bottomIdx,tf,tl,tr,tb,dir);rejected++;continue;
    }
    LadderPlan p;
    if(match>=0){
      const NavLadderData*l=m.Ladder(match);p.id=l->id;p.width=l->width;p.top=l->top;p.bottom=l->bottom;
      p.length=l->length;p.dir=dir;p.topForward=areaIdOrZero(m,l->topForwardArea);
      p.topLeft=areaIdOrZero(m,l->topLeftArea);p.topRight=areaIdOrZero(m,l->topRightArea);
      p.topBehind=areaIdOrZero(m,l->topBehindArea);p.bottomArea=areaIdOrZero(m,l->bottomArea);p.isNew=false;
      unsigned before[5]={p.topForward,p.topLeft,p.topRight,p.topBehind,p.bottomArea};
      // Collision-confirmed discovery replaces stale directional slots. Keep
      // an existing endpoint only where the offline BSP model cannot verify a
      // replacement (for example, a landing supported by a static prop).
      if(tf>=0)p.topForward=areaIdOrZero(m,tf);if(tl>=0)p.topLeft=areaIdOrZero(m,tl);
      if(tr>=0)p.topRight=areaIdOrZero(m,tr);if(tb>=0)p.topBehind=areaIdOrZero(m,tb);
      if(bottomIdx>=0)p.bottomArea=areaIdOrZero(m,bottomIdx);
      if(before[0]!=p.topForward||before[1]!=p.topLeft||before[2]!=p.topRight||before[3]!=p.topBehind||
         before[4]!=p.bottomArea||l->dir!=p.dir)repaired++;
      existingMatched++;
    }else{
      p.id=nextId++;p.width=width;p.top=top;p.bottom=bottom;p.length=(top-bottom).Length();p.dir=dir;
      p.topForward=areaIdOrZero(m,tf);p.topLeft=areaIdOrZero(m,tl);p.topRight=areaIdOrZero(m,tr);
      p.topBehind=areaIdOrZero(m,tb);p.bottomArea=areaIdOrZero(m,bottomIdx);p.isNew=true;
    }
    plans.push_back(p);
    printf("  %s ladder %u (%.0f %.0f %.0f..%.0f) bottom=%u top=%u/%u/%u behind=%u dir=%d\n",
      p.isNew?"add":"keep/repair",p.id,p.top.x,p.top.y,p.bottom.z,p.top.z,p.bottomArea,
      p.topForward,p.topLeft,p.topRight,p.topBehind,p.dir);
  }
  if(g_bspLadders.empty())printf("  none in BSP\n");
  printf("ladder scan: BSP=%d usable=%d new=%d existing=%d endpoint-repaired=%d rejected=%d\n",
    (int)g_bspLadders.size(),(int)plans.size(),(int)plans.size()-existingMatched,existingMatched,repaired,rejected);
}

static int ufRoot(std::vector<int>&parent,int x){
  while(parent[x]!=x){parent[x]=parent[parent[x]];x=parent[x];}return x;
}

static void ufJoin(std::vector<int>&parent,std::vector<int>&sizes,int a,int b){
  a=ufRoot(parent,a);b=ufRoot(parent,b);if(a==b)return;
  if(sizes[a]<sizes[b]){int t=a;a=b;b=t;}parent[b]=a;sizes[a]+=sizes[b];
}

static void reportPlannedIslands(const CNavMesh&m,const std::vector<CutRec>&cuts,
                                 const std::vector<AddRec>&adds,const std::vector<unsigned>&removed){
  std::vector<int> parent(m.Count()),sizes(m.Count(),1);
  for(int i=0;i<m.Count();i++)parent[i]=i;
  for(int i=0;i<m.Count();i++){
    const NavAreaData*a=m.Area(i);if(containsId(removed,a->id))continue;
    for(int k=0;k<a->connCount;k++){
      int j=a->connIdx[k];const NavAreaData*b=m.Area(j);
      if(!b||containsId(removed,b->id)||containsCut(cuts,a->id,b->id))continue;
      ufJoin(parent,sizes,i,j);
    }
  }
  for(size_t i=0;i<adds.size();i++){
    if(containsId(removed,adds[i].fromId)||containsId(removed,adds[i].toId))continue;
    int a=areaIndexById(m,adds[i].fromId),b=areaIndexById(m,adds[i].toId);
    if(a>=0&&b>=0)ufJoin(parent,sizes,a,b);
  }
  int largest=-1,largestSize=0,components=0,active=0;
  for(int i=0;i<m.Count();i++)if(!containsId(removed,m.Area(i)->id)){
    active++;int r=ufRoot(parent,i);if(r==i){components++;if(sizes[r]>largestSize){largest=r;largestSize=sizes[r];}}
  }
  printf("\n=== planned weak-connectivity islands ===\n");
  printf("active areas=%d components=%d largest=%d\n",active,components,largestSize);
  int shown=0;
  for(int i=0;i<m.Count();i++)if(!containsId(removed,m.Area(i)->id)&&ufRoot(parent,i)==i&&i!=largest){
    if(shown<40)printf("  island root area %u: %d areas\n",m.Area(i)->id,sizes[i]);
    shown++;
  }
  if(shown==0)printf("  none outside main component\n");
  else if(shown>40)printf("  ... and %d more islands\n",shown-40);
  printf("islands outside main component=%d (reported only; not auto-removed)\n",shown);
}

static int processOneMap(int argc,char**argv){
#ifdef NAVDOCTOR_SAFE
  printf("navdoctor_safe: low global trust is handled conservatively; uncertain links are kept and elevation snapping is disabled.\n");
#endif
  const char*bspPath=argc>1?argv[1]:"ins_baghdad.bsp";
  const char*navPath=argc>2?argv[2]:"ins_baghdad.nav";
  const char*outPath=argc>3?argv[3]:"ins_baghdad_repaired.nav";
  float focusX=argc>4?atof(argv[4]):-1300.0f, focusY=argc>5?atof(argv[5]):-420.0f;
  // manual cuts: pairs of area IDs from argv[6..], each cut in BOTH directions
  std::vector<unsigned> manualIds;
  for(int a=6;a<argc;a++) manualIds.push_back((unsigned)strtoul(argv[a],0,10));

  if(!loadBsp(bspPath)) return 1;
  CNavMesh m;
  if(!m.Load(navPath)){printf("FAIL nav load\n");return 1;}
  printf("nav areas=%d existing ladders=%d\n",m.Count(),m.LadderCount());
  std::vector<LadderPlan> ladderPlans;
  discoverLadders(m,ladderPlans);

  // ---- per-area ground trust: only validate connections whose endpoints our
  // collision model demonstrably understands (ground probe matches the nav's
  // floor height). Areas on geometry we can't see (static props, entity
  // brushes) are left alone - their links are KEPT, never cut.
  std::vector<unsigned char> trust(m.Count(),0);
  std::vector<std::pair<unsigned, float> > idToDeltaZ;
  {
    int ok=0,bad=0,fail=0,snapped=0; float errSum=0; int shown=0;
    for(int i=0;i<m.Count();i++){
      const NavAreaData*a=m.Area(i);
      float gz;
      if(groundAt(a->center.x,a->center.y,a->center.z+20,gz)){
        float e=fabsf(gz-a->center.z);
        errSum+=e;
        if(e<26){
          ok++; trust[i]=1;
          // Automatic Surface Snapping: If the tile is submerged under terrain or hovering,
          // lift its altitude precisely to true physical floor + 1.0u so it renders vibrantly without clicking!
          if(e > 0.5f) {
            float dz = (gz + 1.0f) - a->center.z;
            idToDeltaZ.push_back(std::make_pair(a->id, dz));
            snapped++;
          }
        }
        else{
          bad++;
          if(shown<12){
            printf("  mismatch: area %u (%.0f %.0f %.0f) my_ground=%.0f (delta %+.0f)\n",
              a->id,a->center.x,a->center.y,a->center.z,gz,gz-a->center.z);
            shown++;
          }
        }
      } else fail++;
    }
    printf("ground trust: trusted=%d off=%d no-ground=%d avg_err=%.1f (%.1f%% trusted)\n",
      ok,bad,fail,errSum/(ok+bad>0?ok+bad:1),100.0f*ok/m.Count());
    printf("automatic surface snapping: identified %d submerged/misaligned tiles for elevation healing\n", snapped);
    if(ok < m.Count()*60/100){
#ifdef NAVDOCTOR_SAFE
      // A displacement-heavy map can fail a global center-floor comparison
      // while still containing many individual links we understand perfectly.
      // Keep repairing only those links, but do not shift any tile elevations:
      // a low global trust result makes mass snapping unsafe.
      idToDeltaZ.clear();
      printf("SAFE MODE: global trust is below 60%%; skipping elevation snapping and repairing only locally trusted links.\n");
#else
      printf("ABORT: too little of the map is understood - not safe to cut\n"); return 2;
#endif
    }
  }

  // ---- tight-passage classification ----
  // PRECISE tells Source-style path following not to wander laterally away
  // from the intended line through a doorway or constrained corridor.
  std::vector<unsigned> preciseAreaIds;
  int preciseShown=0;
  printf("\n=== tight corridors / doorways to mark PRECISE (<=112u wide) ===\n");
  for(int i=0;i<m.Count();i++){
    const NavAreaData*a=m.Area(i);
    if(!trust[i]||(a->attributes&NAVATTR_PRECISE)!=0) continue;
    float hh=(a->attributes&NAVATTR_CROUCH)?34.0f:70.0f;
    Vector feet(a->center.x,a->center.y,m.GetZ(i,a->center.x,a->center.y)+2.0f);
    float widthX=0,widthY=0;
    bool tightX=tightPassageAxis(feet,1,0,hh,widthX);
    bool tightY=tightPassageAxis(feet,0,1,hh,widthY);
    if(!tightX&&!tightY) continue;
    preciseAreaIds.push_back(a->id);
    if(preciseShown<40){
      const char*axis=tightX&&tightY?"X+Y":(tightX?"X":"Y");
      float width=tightX&&tightY?(widthX<widthY?widthX:widthY):(tightX?widthX:widthY);
      printf("  precise area %u (%.0f %.0f %.0f): %s constrained, width %.1fu\n",
        a->id,a->center.x,a->center.y,a->center.z,axis,width);
      preciseShown++;
    }
  }
  if(preciseAreaIds.empty()) printf("  none\n");
  else if((int)preciseAreaIds.size()>preciseShown)
    printf("  ... and %d more\n",(int)preciseAreaIds.size()-preciseShown);
  printf("tight-passage PRECISE additions=%d\n",(int)preciseAreaIds.size());

  // ---- validate every connection ----
  std::vector<CutRec> cuts;
  // A destination is removable only when all collision-understood incoming
  // links require more vertical rise than a bot can jump, and there is no
  // safe or untrusted incoming route that might legitimately reach it.
  static const float MAX_BOT_JUMP_RISE=50.0f;
  std::vector<int> incomingSafe(m.Count(),0), incomingTooHigh(m.Count(),0);
  std::vector<int> incomingOtherBlocked(m.Count(),0), incomingUnknown(m.Count(),0);
  std::vector<unsigned char> crouchNeeded(m.Count(),0);
  std::vector<int> incomingStandPath(m.Count(),0),incomingCrouchOnly(m.Count(),0);
  std::vector<int> incomingNoJumpPath(m.Count(),0),incomingJumpOnly(m.Count(),0);
  // Center headroom catches low tunnels even when their current links happen
  // to begin/end outside the lowest part of the area.
  for(int i=0;i<m.Count();i++)if(trust[i]&&!(m.Area(i)->attributes&NAVATTR_CROUCH)){
    const NavAreaData*a=m.Area(i);
    Vector feet(a->center.x,a->center.y,m.GetZ(i,a->center.x,a->center.y)+2.0f);
    TraceRes stand=traceBrushes(feet,feet,16.4f,70.0f);
    TraceRes crouch=traceBrushes(feet,feet,16.4f,34.0f);
    if(stand.startsolid&&!crouch.startsolid)crouchNeeded[i]=1;
  }
  int total=0, tested=0, skipped=0, ladderLinks=0;
  for(int i=0;i<m.Count();i++){
    const NavAreaData*a=m.Area(i);
    for(int k=0;k<a->connCount;k++){
      int j=a->connIdx[k]; const NavAreaData*b=m.Area(j); if(!b)continue;
      total++;
      int ladderIdx=-1;bool ladderUp=false;
      if(m.GetLadderConnection(i,j,ladderIdx,ladderUp)){
        ladderLinks++;incomingSafe[j]++;incomingNoJumpPath[j]++;incomingStandPath[j]++;continue;
      }
      if(!trust[i]||!trust[j]){skipped++;incomingUnknown[j]++;continue;}   // unknown collision -> keep
      tested++;
      MoveEval move=evaluateMove(m,i,j);
      if(move.pass){
        incomingSafe[j]++;
        if(move.standPass)incomingStandPath[j]++;else if(move.crouchPass)incomingCrouchOnly[j]++;
        if(move.noJumpPass)incomingNoJumpPath[j]++;else incomingJumpOnly[j]++;
      }
      else if(move.tooHigh) incomingTooHigh[j]++;
      else incomingOtherBlocked[j]++;
      if(!move.pass){
        CutRec c; c.fromId=a->id; c.toId=b->id; c.fromC=a->center; c.toC=b->center;
        c.distStuck=xyd(Vector(a->center.x,a->center.y,0),Vector(focusX,focusY,0));
        cuts.push_back(c);
      }
    }
    if(i%1000==0){ printf("  ...area %d/%d (cuts so far %d)\n",i,m.Count(),(int)cuts.size()); fflush(stdout); }
  }
  printf("\nconnections=%d  tested-walk=%d  ladder=%d  kept-untrusted=%d  impassable=%d (%.2f%% of tested)\n",
    total,tested,ladderLinks,skipped,(int)cuts.size(),100.0f*cuts.size()/(tested?tested:1));

  // ---- conservative missing-link discovery ----
  // Candidate rectangles must share/approach a full side within 26 units (one
  // 25u nav-generation step plus float tolerance) and
  // both floors must be trusted. Each direction is simulated independently,
  // naturally producing one-way drops when the return climb is impossible.
  std::vector<AddRec> additions;
  // Track weak components after planned cuts. The extra one-generation-step
  // (24..26u) search is only allowed to bridge separate components; otherwise
  // dense generated meshes can contain thousands of harmless 25u gaps and a
  // broad repair would over-connect the map.
  std::vector<int> linkParent(m.Count()),linkSizes(m.Count(),1);
  for(int i=0;i<m.Count();i++)linkParent[i]=i;
  for(int i=0;i<m.Count();i++){
    const NavAreaData*a=m.Area(i);
    for(int k=0;k<a->connCount;k++){
      int j=a->connIdx[k];const NavAreaData*b=m.Area(j);
      if(b&&!containsCut(cuts,a->id,b->id))ufJoin(linkParent,linkSizes,i,j);
    }
  }
  std::vector<std::vector<int> > areaGrid(g_gx*g_gy);
  for(int i=0;i<m.Count();i++){
    const NavAreaData*a=m.Area(i);
    Vector lo(a->lo.x-26,a->lo.y-26,0),hi(a->hi.x+26,a->hi.y+26,0);
    gridInsert(areaGrid,lo,hi,i);
  }
  std::vector<unsigned> areaSeen(m.Count(),0);unsigned areaSeenMark=1;
  printf("\n=== verified missing connections to add ===\n");
  int additionsShown=0,wallRejected=0;
  for(int i=0;i<m.Count();i++){
    if(!trust[i])continue;
    const NavAreaData*a=m.Area(i);
    int cx0=(int)((a->lo.x-26-g_worldLo[0])/CELL),cy0=(int)((a->lo.y-26-g_worldLo[1])/CELL);
    int cx1=(int)((a->hi.x+26-g_worldLo[0])/CELL),cy1=(int)((a->hi.y+26-g_worldLo[1])/CELL);
    if(cx0<0)cx0=0;if(cy0<0)cy0=0;if(cx1>=g_gx)cx1=g_gx-1;if(cy1>=g_gy)cy1=g_gy-1;
    areaSeenMark++;if(areaSeenMark==0){for(size_t z=0;z<areaSeen.size();z++)areaSeen[z]=0;areaSeenMark=1;}
    for(int cy=cy0;cy<=cy1;cy++)for(int cx=cx0;cx<=cx1;cx++){
      const std::vector<int>&cell=areaGrid[cy*g_gx+cx];
      for(size_t q=0;q<cell.size();q++){
        int j=cell[q];if(j<=i||!trust[j]||areaSeen[j]==areaSeenMark)continue;areaSeen[j]=areaSeenMark;
        const NavAreaData*b=m.Area(j);int dirAB=nearbyEdgeDirection(a,b);if(dirAB<0)continue;
        float sideGap=0.0f;
        if(dirAB==NAV_EAST)sideGap=fabsf(a->hi.x-b->lo.x);
        else if(dirAB==NAV_WEST)sideGap=fabsf(a->lo.x-b->hi.x);
        else if(dirAB==NAV_SOUTH)sideGap=fabsf(a->hi.y-b->lo.y);
        else sideGap=fabsf(a->lo.y-b->hi.y);
        bool generationGap=sideGap>24.01f;
        if(generationGap&&ufRoot(linkParent,i)==ufRoot(linkParent,j))continue;
        // Avoid auto-linking large falls even if walkSim can technically land.
        if(fabsf(a->center.z-b->center.z)>160.0f)continue;
        if(!edgeCrossingHasNoWall(m,i,j,dirAB)){wallRejected++;continue;}
        bool linkedPair=false;
        for(int reverse=0;reverse<2;reverse++){
          int from=reverse?j:i,to=reverse?i:j;
          if(hasDirectedLink(m,from,to))continue;
          MoveEval move=evaluateMove(m,from,to);
          if(!move.pass||move.minRise<-120.0f)continue;
          AddRec add;add.fromId=m.Area(from)->id;add.toId=m.Area(to)->id;
          add.direction=reverse?(dirAB+2)%4:dirAB;add.fromC=m.Area(from)->center;add.toC=m.Area(to)->center;
          add.jump=!move.noJumpPass;add.crouch=!move.standPass&&move.crouchPass;
          additions.push_back(add);incomingSafe[to]++;
          linkedPair=true;
          if(add.jump)incomingJumpOnly[to]++;else incomingNoJumpPath[to]++;
          if(add.crouch)incomingCrouchOnly[to]++;else incomingStandPath[to]++;
          if(additionsShown<60){
            printf("  add %u -> %u dir=%d%s%s\n",add.fromId,add.toId,add.direction,
              add.jump?" JUMP":"",add.crouch?" CROUCH":"");additionsShown++;
          }
        }
        if(linkedPair)ufJoin(linkParent,linkSizes,i,j);
      }
    }
  }
  if(additions.empty())printf("  none\n");
  else if((int)additions.size()>additionsShown)printf("  ... and %d more\n",(int)additions.size()-additionsShown);
  printf("verified missing directed links=%d (wall-blocked candidate pairs rejected=%d)\n",
    (int)additions.size(),wallRejected);

  std::vector<unsigned> crouchAreaIds,jumpAreaIds;
  for(int i=0;i<m.Count();i++){
    const NavAreaData*a=m.Area(i);
    bool allKnownCrouch=incomingCrouchOnly[i]>0&&incomingStandPath[i]==0&&incomingUnknown[i]==0;
    bool allKnownJump=incomingJumpOnly[i]>0&&incomingNoJumpPath[i]==0&&incomingUnknown[i]==0;
    if((crouchNeeded[i]||allKnownCrouch)&&!(a->attributes&NAVATTR_CROUCH))crouchAreaIds.push_back(a->id);
    if(allKnownJump&&!(a->attributes&NAVATTR_JUMP))jumpAreaIds.push_back(a->id);
  }
  printf("automatic flags: CROUCH additions=%d JUMP additions=%d\n",
    (int)crouchAreaIds.size(),(int)jumpAreaIds.size());

  int oneWayDrops=0;
  for(size_t ci=0;ci<cuts.size();ci++)if(cuts[ci].toC.z>cuts[ci].fromC.z+18.0f){
    bool reverseSurvives=false;
    for(int i=0;i<m.Count()&&!reverseSurvives;i++)if(m.Area(i)->id==cuts[ci].toId)
      for(int k=0;k<m.Area(i)->connCount;k++)if(m.Area(m.Area(i)->connIdx[k])->id==cuts[ci].fromId){
        reverseSurvives=true;
        for(size_t cj=0;cj<cuts.size();cj++)if(cuts[cj].fromId==cuts[ci].toId&&cuts[cj].toId==cuts[ci].fromId)reverseSurvives=false;
        break;
      }
    if(reverseSurvives)oneWayDrops++;
  }
  for(size_t ai=0;ai<additions.size();ai++)if(additions[ai].fromC.z>additions[ai].toC.z+18.0f){
    bool reverse=false;
    int low=areaIndexById(m,additions[ai].toId),high=areaIndexById(m,additions[ai].fromId);
    if(low>=0&&high>=0&&hasDirectedLink(m,low,high)&&!containsCut(cuts,additions[ai].toId,additions[ai].fromId))reverse=true;
    for(size_t aj=0;aj<additions.size();aj++)
      if(additions[aj].fromId==additions[ai].toId&&additions[aj].toId==additions[ai].fromId)reverse=true;
    if(!reverse)oneWayDrops++;
  }
  printf("verified one-way drops (safe down, impossible return climb)=%d\n",oneWayDrops);

  // Remove only isolated high destination tiles. A single safe incoming link,
  // another kind of blocked link, an untrusted link, or a ladder connection
  // preserves the area. This deliberately favours false negatives over deleting
  // valid floors, stairs, entity-supported platforms, or ladder landings.
  std::vector<unsigned> removeAreaIds;
  printf("\n=== areas reachable only by over-height jumps (>%.0fu rise) ===\n",MAX_BOT_JUMP_RISE);
  for(int i=0;i<m.Count();i++){
    const NavAreaData*a=m.Area(i);
    if(incomingTooHigh[i]>0&&incomingSafe[i]==0&&incomingOtherBlocked[i]==0&&
       incomingUnknown[i]==0&&a->ladderConnCount==0){
      removeAreaIds.push_back(a->id);
      printf("  remove area %u (%.0f %.0f %.0f): high-in=%d, no other or ladder route\n",
        a->id,a->center.x,a->center.y,a->center.z,incomingTooHigh[i]);
    }
  }
  if(removeAreaIds.empty()) printf("  none\n");

  reportPlannedIslands(m,cuts,additions,removeAreaIds);

  // sort by distance from the focus point (user's stuck spot first)
  for(size_t i=0;i<cuts.size();i++)for(size_t j=i+1;j<cuts.size();j++)
    if(cuts[j].distStuck<cuts[i].distStuck){CutRec t=cuts[i];cuts[i]=cuts[j];cuts[j]=t;}
  printf("\n=== impassable connections (nearest to your stuck spot first) ===\n");
  for(size_t i=0;i<cuts.size()&&i<60;i++){
    CutRec&c=cuts[i];
    printf("  %u(%.0f %.0f %.0f) -> %u(%.0f %.0f %.0f)   [%.0f from focus]\n",
      c.fromId,c.fromC.x,c.fromC.y,c.fromC.z,c.toId,c.toC.x,c.toC.y,c.toC.z,c.distStuck);
  }
  if(cuts.size()>60) printf("  ... and %d more\n",(int)cuts.size()-60);

  float ratio=100.0f*cuts.size()/(total?total:1);
  if(ratio>4.0f)
    printf("\nWARNING: cut ratio %.1f%% is high - review before trusting!\n",ratio);

  // ---- focus diagnostics: exactly what the sim thinks near the stuck spot ----
  printf("\n=== focus zone detail (within 260 of %.0f,%.0f) ===\n",focusX,focusY);
  for(int i=0;i<m.Count();i++){
    const NavAreaData*a=m.Area(i);
    if(xyd(Vector(a->center.x,a->center.y,0),Vector(focusX,focusY,0))>260) continue;
    printf("area %u (%.0f %.0f %.0f) z[%.0f..%.0f] trust=%d\n",
      a->id,a->center.x,a->center.y,a->center.z,a->neZ,a->swZ,(int)trust[i]);
    for(int k=0;k<a->connCount;k++){
      int j=a->connIdx[k]; const NavAreaData*b=m.Area(j); if(!b)continue;
      int ladderIdx=-1;bool ladderUp=false;
      if(m.GetLadderConnection(i,j,ladderIdx,ladderUp)){
        const NavLadderData*l=m.Ladder(ladderIdx);
        printf("   -> %u (%.0f %.0f %.0f)  LADDER %u %s\n",b->id,
          b->center.x,b->center.y,b->center.z,l?l->id:0,ladderUp?"UP":"DOWN");
        continue;
      }
      float lx=a->lo.x>b->lo.x?a->lo.x:b->lo.x, hx=a->hi.x<b->hi.x?a->hi.x:b->hi.x;
      float ly=a->lo.y>b->lo.y?a->lo.y:b->lo.y, hy=a->hi.y<b->hi.y?a->hi.y:b->hi.y;
      bool adjacent=!(lx>hx+24.0f||ly>hy+24.0f);
      float drop=a->center.z-b->center.z;
      float maxDrop=drop>0?drop+120.0f:90.0f;
      // single midpoint sample, all hull variants, verbose
      float px=lx==hx?lx:(lx+hx)*0.5f, py=ly==hy?ly:(ly+hy)*0.5f;
      float amx=(a->lo.x+a->hi.x)*0.5f, amy=(a->lo.y+a->hi.y)*0.5f;
      float bmx=(b->lo.x+b->hi.x)*0.5f, bmy=(b->lo.y+b->hi.y)*0.5f;
      float dax=amx-px,day=amy-py,al=sqrtf(dax*dax+day*day); if(al>1){dax/=al;day/=al;}
      float dbx=bmx-px,dby=bmy-py,bl=sqrtf(dbx*dbx+dby*dby); if(bl>1){dbx/=bl;dby/=bl;}
      Vector S(px+dax*(al<20?al:20),py+day*(al<20?al:20),0);
      Vector E(px+dbx*(bl<20?bl:20),py+dby*(bl<20?bl:20),0);
      S.z=m.GetZ(i,S.x,S.y)+2; E.z=m.GetZ(j,E.x,E.y)+2;
      bool st=walkSim(S,E,15.0f,70.0f,maxDrop,true);
      bool cr=st?true:walkSim(S,E,15.0f,34.0f,maxDrop,true);
      bool wide=walkSim(S,E,16.4f,70.0f,maxDrop,true);
      bool wideCr=wide?true:walkSim(S,E,16.4f,35.4f,maxDrop,true);
      printf("   -> %u (%.0f %.0f %.0f)  adj=%d  slim: stand=%d crouch=%d | REAL-size: stand=%d crouch=%d%s\n",
        b->id,b->center.x,b->center.y,b->center.z,(int)adjacent,
        (int)st,(int)cr,(int)wide,(int)wideCr,
        (cr&&!wideCr)?"   <== passes slim hull only: TOO TIGHT for a real player":"");
    }
  }

  // ---- manual cuts (user-identified impassable pairs, both directions) ----
  for(size_t mi=0;mi+1<manualIds.size();mi+=2){
    for(int dirn=0;dirn<2;dirn++){
      unsigned f=dirn?manualIds[mi+1]:manualIds[mi];
      unsigned t=dirn?manualIds[mi]:manualIds[mi+1];
      // only add if that directed link actually exists
      bool exists=false; Vector fc,tc;
      for(int i=0;i<m.Count()&&!exists;i++){
        const NavAreaData*a=m.Area(i);
        if(a->id!=f) continue;
        fc=a->center;
        for(int k=0;k<a->connCount;k++){
          const NavAreaData*b=m.Area(a->connIdx[k]);
          if(b&&b->id==t){exists=true;tc=b->center;break;}
        }
      }
      if(exists){
        CutRec c; c.fromId=f; c.toId=t; c.fromC=fc; c.toC=tc; c.distStuck=0;
        cuts.push_back(c);
        printf("manual cut: %u -> %u\n",f,t);
      } else printf("manual cut %u -> %u: link not found (skipped)\n",f,t);
    }
  }

  // ---- rewrite the nav with cuts and elevation snapping applied ----
  if(cuts.empty() && additions.empty() && idToDeltaZ.empty() && removeAreaIds.empty() &&
     preciseAreaIds.empty() && crouchAreaIds.empty() && jumpAreaIds.empty() && ladderPlans.empty()){
    printf("nothing to cut, connect, remove, snap, or flag - nav already clean and leveled\n"); return 0;
  }

  FILE*fp=fopen(navPath,"rb");
  if(!fp){printf("re-read failed: cannot open %s\n",navPath);return 1;}
  fseek(fp,0,SEEK_END); long sz=ftell(fp); fseek(fp,0,SEEK_SET);
  unsigned char*src=(unsigned char*)malloc(sz);
  if(fread(src,1,sz,fp)!=(size_t)sz){printf("re-read fail\n");return 1;}
  fclose(fp);
  std::vector<unsigned char> out; out.reserve(sz);

  // cut lookup
  struct Pair{unsigned f,t;};
  std::vector<Pair> cutPairs;
  for(size_t i=0;i<cuts.size();i++){Pair p={cuts[i].fromId,cuts[i].toId};cutPairs.push_back(p);}
  auto is_cut = [&](unsigned F, unsigned T) -> bool {
    for(size_t q=0;q<cutPairs.size();q++) if(cutPairs[q].f==F && cutPairs[q].t==T) return true;
    return false;
  };
  auto is_removed = [&](unsigned id) -> bool { return containsId(removeAreaIds,id); };

  unsigned pos=0;
  #define RD32() (*(unsigned*)(src+pos)); pos+=4
  #define RD16() (*(unsigned short*)(src+pos)); pos+=2
  #define RD8()  (src[pos]); pos+=1
  #define CP(n)  do{ out.insert(out.end(),src+pos,src+pos+(n)); pos+=(n);}while(0)
  #define W32(v) do{ unsigned _v=(v); out.insert(out.end(),(unsigned char*)&_v,(unsigned char*)&_v+4);}while(0)
  #define W16(v) do{ unsigned short _v=(v); out.insert(out.end(),(unsigned char*)&_v,(unsigned char*)&_v+2);}while(0)
  #define WF(v)  do{ float _f=(v); out.insert(out.end(),(unsigned char*)&_f,(unsigned char*)&_f+4);}while(0)

  CP(4);                      // magic
  unsigned ver=*(unsigned*)(src+pos); CP(4);   // version
  if(ver!=9){printf("rewriter expects v9, got %u\n",ver);return 1;}
  CP(4);                      // bsp size
  unsigned short pc=*(unsigned short*)(src+pos); CP(2);   // place count
  for(int i=0;i<pc;i++){ unsigned short l=*(unsigned short*)(src+pos); CP(2); CP(l); }
  unsigned areaCount=*(unsigned*)(src+pos); pos+=4;
  unsigned keptAreaCount=areaCount-(unsigned)removeAreaIds.size();
  W32(keptAreaCount);
  int removed=0, addedWritten=0, removedAnalysisRefs=0, snappedWritten=0;
  int ladderAreaRefsAdded=0,laddersAdded=0,laddersRepaired=0;
  int preciseWritten=0,crouchWritten=0,jumpWritten=0;
  for(unsigned ai=0;ai<areaCount;ai++){
    size_t areaOutStart=out.size();
    unsigned id=*(unsigned*)(src+pos); CP(4);
    bool discardArea=is_removed(id);
    unsigned short attributes=*(unsigned short*)(src+pos); pos+=2;
    if(!discardArea&&containsId(preciseAreaIds,id)&&!(attributes&NAVATTR_PRECISE)){
      attributes|=NAVATTR_PRECISE; preciseWritten++;
    }
    if(!discardArea&&containsId(crouchAreaIds,id)&&!(attributes&NAVATTR_CROUCH)){
      attributes|=NAVATTR_CROUCH; crouchWritten++;
    }
    if(!discardArea&&containsId(jumpAreaIds,id)&&!(attributes&NAVATTR_JUMP)){
      attributes|=NAVATTR_JUMP; jumpWritten++;
    }
    W16(attributes);          // attributes (v9: u16)

    // Automatic Elevation Healing (Surface Snapping)
    float dz = 0.0f;
    for(size_t j=0; j<idToDeltaZ.size(); j++){ if(idToDeltaZ[j].first == id){ dz = idToDeltaZ[j].second; break; } }
    if(dz != 0.0f){
      float floats[8];
      for(int f=0; f<8; f++){ floats[f] = *(float*)(src+pos); pos += 4; }
      floats[2] += dz; // lo.z
      floats[5] += dz; // hi.z
      floats[6] += dz; // neZ
      floats[7] += dz; // swZ
      for(int f=0; f<8; f++) WF(floats[f]);
      if(!discardArea) snappedWritten++;
    } else {
      CP(4*6+4+4);            // lo,hi (6 floats) + neZ + swZ
    }

    for(int d=0;d<4;d++){
      unsigned cnt=*(unsigned*)(src+pos); pos+=4;
      // gather surviving ids
      std::vector<unsigned> keep;
      for(unsigned k=0;k<cnt;k++){
        unsigned to=*(unsigned*)(src+pos); pos+=4;
        if(discardArea||is_removed(to)||is_cut(id,to)) removed++;
        else keep.push_back(to);
      }
      if(!discardArea)for(size_t ad=0;ad<additions.size();ad++){
        const AddRec&add=additions[ad];
        if(add.fromId!=id||add.direction!=d||is_removed(add.toId)||is_cut(id,add.toId))continue;
        bool duplicate=false;for(size_t q=0;q<keep.size();q++)if(keep[q]==add.toId){duplicate=true;break;}
        if(!duplicate){keep.push_back(add.toId);addedWritten++;}
      }
      W32((unsigned)keep.size());
      for(size_t k2=0;k2<keep.size();k2++) W32(keep[k2]);
    }
    unsigned char hides=src[pos]; CP(1); CP(hides*(4+12+1));
    // Analysis records contain area IDs too. Drop a whole record if any of its
    // area references was removed, rather than leaving a dangling ID behind.
    unsigned char app=src[pos++];
    size_t appCountPos=out.size(); out.push_back(0); unsigned char keptApp=0;
    for(unsigned ap=0;ap<app;ap++){
      unsigned recStart=pos;
      unsigned here=*(unsigned*)(src+pos), prev=*(unsigned*)(src+pos+4), next=*(unsigned*)(src+pos+9);
      pos+=4+4+1+4+1;
      if(is_removed(here)||is_removed(prev)||is_removed(next)) removedAnalysisRefs++;
      else { out.insert(out.end(),src+recStart,src+pos); keptApp++; }
    }
    out[appCountPos]=keptApp;
    unsigned enc=*(unsigned*)(src+pos); pos+=4;
    size_t encCountPos=out.size(); W32(0); unsigned keptEnc=0;
    for(unsigned e=0;e<enc;e++){
      unsigned recStart=pos;
      unsigned from=*(unsigned*)(src+pos); pos+=4+1;
      unsigned to=*(unsigned*)(src+pos); pos+=4+1;
      unsigned char sp=src[pos++]; pos+=sp*(4+1);
      if(is_removed(from)||is_removed(to)) removedAnalysisRefs++;
      else { out.insert(out.end(),src+recStart,src+pos); keptEnc++; }
    }
    memcpy(&out[encCountPos],&keptEnc,4);
    CP(2);                    // place
    for(int d=0;d<2;d++){
      unsigned lc=*(unsigned*)(src+pos);pos+=4;std::vector<unsigned> ladderIds;
      for(unsigned lci=0;lci<lc;lci++){unsigned lid=*(unsigned*)(src+pos);pos+=4;ladderIds.push_back(lid);}
      if(!discardArea)for(size_t lp=0;lp<ladderPlans.size();lp++){
        const LadderPlan&plan=ladderPlans[lp];bool belongs=false;
        if(d==0)belongs=plan.bottomArea==id;
        else belongs=plan.topForward==id||plan.topLeft==id||plan.topRight==id;
        if(!belongs)continue;bool duplicate=false;
        for(size_t q=0;q<ladderIds.size();q++)if(ladderIds[q]==plan.id){duplicate=true;break;}
        if(!duplicate){ladderIds.push_back(plan.id);ladderAreaRefsAdded++;}
      }
      if(discardArea)ladderIds.clear();
      W32((unsigned)ladderIds.size());for(size_t q=0;q<ladderIds.size();q++)W32(ladderIds[q]);
    }
    CP(8);                    // occupy times
    if(discardArea) out.resize(areaOutStart); // parsed fully, but omit the area record
  }
  // Repair matched mesh-level records, preserve unrelated existing records,
  // then append newly discovered BSP ladders.
  if(pos+4<=(unsigned)sz){
    unsigned ladderCount=*(unsigned*)(src+pos);pos+=4;
    unsigned newCount=0;for(size_t lp=0;lp<ladderPlans.size();lp++)if(ladderPlans[lp].isNew)newCount++;
    W32(ladderCount+newCount);
    for(unsigned li=0;li<ladderCount;li++){
      if(pos+60>(unsigned)sz){printf("rewriter: truncated ladder data\n");return 1;}
      unsigned ladderId=*(unsigned*)(src+pos);const LadderPlan*plan=0;
      for(size_t lp=0;lp<ladderPlans.size();lp++)
        if(!ladderPlans[lp].isNew&&ladderPlans[lp].id==ladderId){plan=&ladderPlans[lp];break;}
      if(plan){
        pos+=60;W32(plan->id);WF(plan->width);
        WF(plan->top.x);WF(plan->top.y);WF(plan->top.z);
        WF(plan->bottom.x);WF(plan->bottom.y);WF(plan->bottom.z);
        WF(plan->length);W32((unsigned)plan->dir);
        unsigned endpoints[5]={plan->topForward,plan->topLeft,plan->topRight,plan->topBehind,plan->bottomArea};
        for(int endpoint=0;endpoint<5;endpoint++){
          if(is_removed(endpoints[endpoint])){W32(0);removedAnalysisRefs++;}else W32(endpoints[endpoint]);
        }
        laddersRepaired++;
      }else{
        CP(40);                 // id, width, top/bottom, length, direction
        for(int endpoint=0;endpoint<5;endpoint++){
          unsigned areaId=*(unsigned*)(src+pos); pos+=4;
          if(is_removed(areaId)){W32(0);removedAnalysisRefs++;}
          else W32(areaId);
        }
      }
    }
    for(size_t lp=0;lp<ladderPlans.size();lp++)if(ladderPlans[lp].isNew){
      const LadderPlan&plan=ladderPlans[lp];W32(plan.id);WF(plan.width);
      WF(plan.top.x);WF(plan.top.y);WF(plan.top.z);
      WF(plan.bottom.x);WF(plan.bottom.y);WF(plan.bottom.z);
      WF(plan.length);W32((unsigned)plan.dir);
      W32(plan.topForward);W32(plan.topLeft);W32(plan.topRight);W32(plan.topBehind);W32(plan.bottomArea);
      laddersAdded++;
    }
  }
  CP((unsigned)(sz-pos));     // any game-specific trailing data
  printf("rewrote: %d areas removed, %d links removed, %d links added, %d analysis/ladder refs cleared, %d tiles snapped, flags PRECISE=%d CROUCH=%d JUMP=%d\n",
    (int)removeAreaIds.size(),removed,addedWritten,removedAnalysisRefs,snappedWritten,
    preciseWritten,crouchWritten,jumpWritten);
  printf("ladders: new records=%d matched records rewritten=%d area up/down refs added=%d\n",
    laddersAdded,laddersRepaired,ladderAreaRefsAdded);

  FILE*fo=fopen(outPath,"wb");
  if(!fo){printf("write failed: cannot open %s (make sure its folder exists)\n",outPath);return 1;}
  if(fwrite(&out[0],1,out.size(),fo)!=out.size()){
    fclose(fo); printf("write failed: incomplete write to %s\n",outPath); return 1;
  }
  fclose(fo);
  printf("wrote %s (%d bytes, was %ld)\n",outPath,(int)out.size(),sz);

  // ---- verify: reload repaired, compare, A* sanity ----
  CNavMesh m2;
  if(!m2.Load(outPath)){printf("VERIFY FAIL: repaired nav did not parse!\n");return 3;}
  int c1=0,c2=0;
  for(int i=0;i<m.Count();i++) c1+=m.Area(i)->connCount;
  for(int i=0;i<m2.Count();i++) c2+=m2.Area(i)->connCount;
  printf("verify: areas %d->%d (expected -%d), typed conns %d->%d\n",
    m.Count(),m2.Count(),(int)removeAreaIds.size(),c1,c2);
  int ladderRecordsVerified=0,ladderEdgeFailures=0;
  for(size_t lp=0;lp<ladderPlans.size();lp++){
    const LadderPlan&plan=ladderPlans[lp];int found=-1;
    for(int li=0;li<m2.LadderCount();li++)if(m2.Ladder(li)->id==plan.id){found=li;break;}
    if(found<0){ladderEdgeFailures++;continue;}ladderRecordsVerified++;
    int bottom=areaIndexById(m2,plan.bottomArea);unsigned tops[3]={plan.topForward,plan.topLeft,plan.topRight};
    for(int ti=0;ti<3;ti++)if(tops[ti]){
      int top=areaIndexById(m2,tops[ti]),edge=-1;bool up=false;
      if(bottom<0||top<0||!m2.GetLadderConnection(bottom,top,edge,up)||!up||edge!=found)ladderEdgeFailures++;
      if(bottom<0||top<0||!m2.GetLadderConnection(top,bottom,edge,up)||up||edge!=found)ladderEdgeFailures++;
    }
  }
  printf("verify ladders: records %d/%d, directed edge failures=%d (mesh total=%d)\n",
    ladderRecordsVerified,(int)ladderPlans.size(),ladderEdgeFailures,m2.LadderCount());
  if(ladderRecordsVerified!=(int)ladderPlans.size()||ladderEdgeFailures)
    printf("WARNING: ladder verification mismatch!\n");
  int preciseVerified=0,crouchVerified=0,jumpVerified=0;
  for(int i=0;i<m2.Count();i++){
    const NavAreaData*a=m2.Area(i);
    if((a->attributes&NAVATTR_PRECISE)&&containsId(preciseAreaIds,a->id))preciseVerified++;
    if((a->attributes&NAVATTR_CROUCH)&&containsId(crouchAreaIds,a->id))crouchVerified++;
    if((a->attributes&NAVATTR_JUMP)&&containsId(jumpAreaIds,a->id))jumpVerified++;
  }
  printf("verify flags: PRECISE %d/%d CROUCH %d/%d JUMP %d/%d\n",
    preciseVerified,preciseWritten,crouchVerified,crouchWritten,jumpVerified,jumpWritten);
  if(preciseVerified!=preciseWritten||crouchVerified!=crouchWritten||jumpVerified!=jumpWritten)
    printf("WARNING: attribute flag verification mismatch!\n");

  unsigned rng=12345; int okOrig=0,okRep=0,tests=60,runTests=0;
  int path[512];
  for(int attempt=0;attempt<tests*5&&runTests<tests;attempt++){
    rng=rng*1103515245+12345;
    int s=(rng>>8)%m.Count();
    rng=rng*1103515245+12345;
    int g=(rng>>8)%m.Count();
    if(containsId(removeAreaIds,m.Area(s)->id)||containsId(removeAreaIds,m.Area(g)->id)) continue;
    int s2=m2.FindArea(m.Area(s)->center), g2=m2.FindArea(m.Area(g)->center);
    if(s2<0||g2<0) continue;
    runTests++;
    if(m.BuildPath(s,g,path,512)>0) okOrig++;
    if(m2.BuildPath(s2,g2,path,512)>0) okRep++;
  }
  printf("A* sanity on retained areas: original %d/%d, repaired %d/%d\n",okOrig,runTests,okRep,runTests);
  if(okRep<okOrig-2) printf("WARNING: repaired mesh lost connectivity!\n");
  else printf("connectivity preserved.\n");
  return 0;
}

static std::string joinPath(const std::string&a,const std::string&b){
  if(a.empty())return b;char last=a[a.size()-1];
  return (last=='\\'||last=='/')?a+b:a+"\\"+b;
}

static std::string fullPathLower(const char*path){
  char full[MAX_PATH];DWORD n=GetFullPathNameA(path,MAX_PATH,full,0);
  std::string s=(n>0&&n<MAX_PATH)?std::string(full):std::string(path);
  while(s.size()>3&&(s[s.size()-1]=='\\'||s[s.size()-1]=='/'))s.resize(s.size()-1);
  for(size_t i=0;i<s.size();i++)if(s[i]>='A'&&s[i]<='Z')s[i]=(char)(s[i]-'A'+'a');
  return s;
}

static bool fileExists(const std::string&path){
  DWORD a=GetFileAttributesA(path.c_str());return a!=INVALID_FILE_ATTRIBUTES&&!(a&FILE_ATTRIBUTE_DIRECTORY);
}

static bool ensureOutputDirectory(const char*path){
  DWORD a=GetFileAttributesA(path);
  if(a!=INVALID_FILE_ATTRIBUTES)return (a&FILE_ATTRIBUTE_DIRECTORY)!=0;
  if(CreateDirectoryA(path,0))return true;
  printf("BATCH ERROR: cannot create output directory %s (Windows error %lu)\n",path,(unsigned long)GetLastError());
  return false;
}

static int processBatch(const char*program,const char*inputDir,const char*outputDir){
  DWORD inAttr=GetFileAttributesA(inputDir);
  if(inAttr==INVALID_FILE_ATTRIBUTES||!(inAttr&FILE_ATTRIBUTE_DIRECTORY)){
    printf("BATCH ERROR: input directory does not exist: %s\n",inputDir);return 4;
  }
  if(fullPathLower(inputDir)==fullPathLower(outputDir)){
    printf("BATCH ERROR: input and output directories must differ (source NAV files are never overwritten).\n");return 4;
  }
  if(!ensureOutputDirectory(outputDir))return 4;

  std::string pattern=joinPath(inputDir,"*.nav");WIN32_FIND_DATAA fd;
  HANDLE find=FindFirstFileA(pattern.c_str(),&fd);
  if(find==INVALID_HANDLE_VALUE){
    printf("BATCH: no .nav files found in %s\n",inputDir);return 0;
  }

  int found=0,succeeded=0,skipped=0,failed=0;
  do{
    if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;
    std::string navName=fd.cFileName;
    if(navName.size()<5)continue;
    found++;
    std::string base=navName.substr(0,navName.size()-4);
    std::string bspPath=joinPath(inputDir,base+".bsp");
    std::string navPath=joinPath(inputDir,navName);
    std::string outPath=joinPath(outputDir,navName);
    if(!fileExists(bspPath)){
      printf("\n[BATCH %d] SKIP %s: matching %s.bsp was not found\n",found,navName.c_str(),base.c_str());
      skipped++;continue;
    }

    printf("\n==============================================================================\n");
    printf("[BATCH %d] %s\n",found,base.c_str());
    printf("  BSP: %s\n  NAV: %s\n  OUT: %s\n",bspPath.c_str(),navPath.c_str(),outPath.c_str());
    char*oneArgv[4];
    oneArgv[0]=(char*)program;oneArgv[1]=(char*)bspPath.c_str();
    oneArgv[2]=(char*)navPath.c_str();oneArgv[3]=(char*)outPath.c_str();
    int result=processOneMap(4,oneArgv);
    // A clean NAV can legitimately need no rewrite. Batch mode still promises
    // a complete output set, so copy that unchanged source into the separate
    // destination directory when processOneMap produced no file.
    if(result==0&&!fileExists(outPath)&&!CopyFileA(navPath.c_str(),outPath.c_str(),FALSE)){
      printf("[BATCH %d] FAILED to copy unchanged NAV (Windows error %lu)\n",
        found,(unsigned long)GetLastError());result=5;
    }
    if(result==0){succeeded++;printf("[BATCH %d] OK %s\n",found,base.c_str());}
    else{failed++;printf("[BATCH %d] FAILED %s (exit %d); continuing\n",found,base.c_str(),result);}
  }while(FindNextFileA(find,&fd));
  FindClose(find);

  printf("\n==============================================================================\n");
  printf("BATCH COMPLETE: found=%d succeeded=%d skipped-no-bsp=%d failed=%d\n",found,succeeded,skipped,failed);
  printf("Output directory: %s\n",outputDir);
  return failed?4:0;
}

int main(int argc,char**argv){
  if(argc>=2&&(!strcmp(argv[1],"--help")||!strcmp(argv[1],"-h"))){
    printf("Single map:\n  %s map.bsp map.nav repaired.nav [focusX focusY [cutAreaA cutAreaB ...]]\n",argv[0]);
    printf("Batch folder:\n  %s --batch input_maps_directory output_nav_directory\n",argv[0]);
    return 0;
  }
  if(argc>=2&&!strcmp(argv[1],"--batch")){
    if(argc!=4){
      printf("Usage: %s --batch input_maps_directory output_nav_directory\n",argv[0]);
      return 4;
    }
    return processBatch(argv[0],argv[2],argv[3]);
  }
  return processOneMap(argc,argv);
}
