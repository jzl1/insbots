// ============================================================================
// objective.cpp - see objective.h
// ============================================================================
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "tier0/dbg.h"
#include "interface.h"
#include "eiface.h"
#include "edict.h"
#include "iserverunknown.h"
#include "engine/ICollideable.h"

#include "engine/IEngineTrace.h"
#include "gametrace.h"
#include "bspflags.h"
#include "cmodel.h"

#include "objective.h"
#include "objective_order.h"
#include "cfgvar.h"
#include "nav.h"
#include "linux_offsets.h"

extern IVEngineServer *engine;
extern CGlobalVars    *gpGlobals;
extern CfgVar insbot_team_offset;     // platform-specific ownership field
extern CfgVar insbot_team_flip;       // optionally flip enemy->owner
extern CfgVar insbot_active_offset;   // Windows legacy active flag; unused on Linux
extern CfgVar insbot_owner_offset;    // m_iCapturedTeam (0x360)
extern CfgVar insbot_capture_offset;  // m_iCaptureTeam  (0x3c0)
extern CfgVar insbot_hidden_offset;   // legacy
extern CfgVar insbot_order_offset;    // m_iOrderID (profile sequence index)
extern CfgVar insbot_push_order;      // kill switch for Push ordering
extern CfgVar insbot_debug;           // verbose console logging
extern CfgVar insbot_goal_classnames; // comma-separated objective class list

// resolve the live CBaseEntity for an objective slot (verifies classname)

// Is this entity class one of the configured objective classes? The CfgVar is
// a comma-separated list (default ins_objective,obj_t1,obj_t2). Tokens are
// trimmed and compared case-insensitively without heap allocation.
static bool IsConfiguredObjectiveClass(const char *classname)
{
    if (!classname || !classname[0]) return false;
    const char *list = insbot_goal_classnames.GetString();
    if (!list || !list[0]) return false;
    size_t cnLen = strlen(classname);
    const char *p = list;
    while (*p)
    {
        while (*p == ' ' || *p == '	' || *p == ',') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '	')) end--;
        size_t len = (size_t)(end - start);
        if (len > 0 && len == cnLen && _strnicmp(classname, start, len) == 0)
            return true;
        if (*p == ',') p++;
    }
    return false;
}
static CBaseEntity *ObjEntity(int edictIndex)
{
    edict_t *e = engine->PEntityOfEntIndex(edictIndex);
    if (!e || e->IsFree()) return 0;
    const char *cn = e->GetClassName();
    if (!cn || !IsConfiguredObjectiveClass(cn)) return 0;
    IServerUnknown *unk = e->GetUnknown();
    return unk ? unk->GetBaseEntity() : 0;
}


// Fail-closed configured-offset readers. The 16..8192 range check proves only
// that the offset is plausible; a wrong offset can still point outside the
// live entity allocation, so each read is additionally guarded by a safe
// memory probe.
#ifdef _LINUX
#include <sys/uio.h>
#include <unistd.h>
static bool SafeEntityRead(const void *addr, void *out, size_t n)
{
    if (!addr || !out || n == 0) return false;
    struct iovec local = { out, n };
    struct iovec remote = { (void *)addr, n };
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)n;
}
#else
extern "C" __declspec(dllimport) unsigned long __stdcall VirtualQuery(const void *lpAddress, void *lpBuffer, unsigned long dwLength);
#define PAGE_NOACCESS          0x01
#define PAGE_READONLY          0x02
#define PAGE_READWRITE         0x04
#define PAGE_WRITECOPY         0x08
#define PAGE_EXECUTE_READ      0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD             0x100
#define MEM_COMMIT             0x1000
struct InsMemBasicInfo
{
    void *BaseAddress;
    void *AllocationBase;
    unsigned long AllocationProtect;
    unsigned long RegionSize;
    unsigned long State;
    unsigned long Protect;
    unsigned long Type;
};
static bool SafeEntityRead(const void *addr, void *out, size_t n)
{
    if (!addr || !out || n == 0) return false;
    InsMemBasicInfo mbi;
    memset(&mbi, 0, sizeof(mbi));
    if (!VirtualQuery(addr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return false;
    const char *start = (const char *)addr;
    const char *end = start + n - 1;
    const char *pageEnd = (const char *)mbi.BaseAddress + mbi.RegionSize;
    if (end >= pageEnd) return false;
    memcpy(out, addr, n);
    return true;
}
#endif

static bool TryReadEntityInt(CBaseEntity *entity, int offset, int &value)
{
    if (!entity || offset < 16 || offset > 8192) return false;
    return SafeEntityRead((const char *)entity + offset, &value, sizeof(int));
}

static bool TryReadEntityByte(CBaseEntity *entity, int offset, unsigned char &value)
{
    if (!entity || offset < 16 || offset > 8192) return false;
    return SafeEntityRead((const char *)entity + offset, &value, sizeof(unsigned char));
}
CObjectives g_Objectives;

void CObjectives::Reset()
{
    m_count = 0;
    m_scanned = false;
    m_letterCount = 0;
    m_gametype = 0;
    m_ordered = false;
    for (int i = 0; i < 3; i++) { m_home[i] = Vector(0, 0, 0); m_homeSamples[i] = 0; }
}

char CObjectives::LetterFor(const Vector &center) const
{
    int best = -1; float bd = 96.0f * 96.0f;   // within ~96u of a known brush center
    for (int i = 0; i < m_letterCount; i++)
    {
        float dx = m_letters[i].c.x - center.x, dy = m_letters[i].c.y - center.y,
              dz = m_letters[i].c.z - center.z;
        float d = dx * dx + dy * dy + dz * dz;
        if (d < bd) { bd = d; best = i; }
    }
    return best >= 0 ? m_letters[best].letter : 0;
}

// m_iOrderID is assigned by the game during round setup, so reading it once at
// ServerActivate() caches -1 for every objective and silently disables Push
// ordering. Re-read on every round start.
//
// A failed or sentinel read stays -1, which InsOrderIdValid() rejects, so it
// can never be mistaken for "objective 0".
void CObjectives::RefreshOrderIds()
{
    const int off = insbot_order_offset.GetInt();
    int resolved = 0;
    for (int i = 0; i < m_count; i++)
    {
        int oid = -1;
        CBaseEntity *ent = ObjEntity(m_objs[i].edictIndex);
        if (ent) TryReadEntityInt(ent, off, oid);
        m_objs[i].orderId = oid;
        if (InsOrderIdValid(oid)) resolved++;
    }

    if (insbot_debug.GetBool())
        Msg("[insbots] order ids refreshed: %d/%d objectives in the active profile\n",
            resolved, m_count);
}

// Read the map's default gametype from its .imc2. This reuses the exact
// file-read pattern LoadLetters() already uses - no new I/O mechanism.
//
// Push is the only sequential stock mode. Firefight and Battle maps also list
// their objectives in profile order, so ordering MUST be gated on the gametype:
// applying it unconditionally would change targeting on 7 of the 13 stock maps.
void CObjectives::LoadProfile(const char *gamedir, const char *mapname)
{
    m_gametype = 0;
    m_ordered = false;
    if (!gamedir || !mapname || !gamedir[0] || !mapname[0]) return;

    char path[512];
    _snprintf(path, sizeof(path), "%s/maps/%s.imc2", gamedir, mapname);
    FILE *fp = fopen(path, "rb");
    if (!fp) return;

    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz >= 1000000) { fclose(fp); return; }

    char *b = (char *)malloc(sz + 1);
    if (!b) { fclose(fp); return; }
    if (fread(b, 1, sz, fp) != (size_t)sz) { free(b); fclose(fp); return; }
    b[sz] = 0;
    fclose(fp);

    int ordered = 0;
    if (InsParseGametype(b, &ordered))
    {
        // Classify for diagnostics. Only Push is sequential today.
        if (strstr(b, "GAMETYPE_PUSH"))           m_gametype = 1;
        else if (strstr(b, "GAMETYPE_FIREFIGHT")) m_gametype = 2;
        else if (strstr(b, "GAMETYPE_BATTLE"))    m_gametype = 3;
        m_ordered = (ordered != 0);
    }
    free(b);

    Msg("[insbots] map gametype: %s (ordered=%d)\n",
        m_gametype == 1 ? "PUSH" : m_gametype == 2 ? "FIREFIGHT" :
        m_gametype == 3 ? "BATTLE" : "unknown/unreadable",
        m_ordered ? 1 : 0);
}

// Resolve each objective's HUD letter with zero reverse-engineering: the .imc2
// lists objective definitions in mapid order, each with a "phonetic" letter; the
// .bsp ties each ins_objective entity to a "mapid" and a brush "model" whose
// bounds give its world position. We build (brush-center -> letter) here.
void CObjectives::LoadLetters(const char *gamedir, const char *mapname)
{
    m_letterCount = 0;
    char path[512];

    // --- 1: phonetic letters, in mapid order, from the .imc2 ---
    char letters[64]; int nLetters = 0;
    _snprintf(path, sizeof(path), "%s/maps/%s.imc2", gamedir, mapname);
    FILE *fp = fopen(path, "rb");
    if (fp)
    {
        fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (sz > 0 && sz < 1000000)
        {
            char *b = (char *)malloc(sz + 1);
            if (b && fread(b, 1, sz, fp) == (size_t)sz)
            {
                b[sz] = 0;
                // each objective definition has exactly one "phonetic" "x"; they
                // appear in mapid order (ProfileData uses teamstart, not phonetic)
                const char *p = b;
                while ((p = strstr(p, "\"phonetic\"")) && nLetters < 64)
                {
                    const char *q = strchr(p + 10, '"');
                    if (q) { const char *r = strchr(q + 1, '"');
                             if (r && r > q + 1) letters[nLetters++] = q[1]; }
                    p += 10;
                }
            }
            if (b) free(b);
        }
        fclose(fp);
    }
    if (nLetters == 0) return;   // no letters to resolve

    // --- 2: from the .bsp, each ins_objective's mapid + brush-model center ---
    _snprintf(path, sizeof(path), "%s/maps/%s.bsp", gamedir, mapname);
    fp = fopen(path, "rb");
    if (!fp) return;
    int hdr[2] = {0,0};
    if (fread(hdr, 4, 2, fp) != 2) { fclose(fp); return; }
    // lump directory: lump[i] = {int ofs, len, ver; char cc[4]} at 8 + i*16
    int entOfs = 0, entLen = 0, modOfs = 0, modLen = 0;
    fseek(fp, 8 + 0 * 16, SEEK_SET);  fread(&entOfs, 4, 1, fp); fread(&entLen, 4, 1, fp);
    fseek(fp, 8 + 14 * 16, SEEK_SET); fread(&modOfs, 4, 1, fp); fread(&modLen, 4, 1, fp);
    if (entLen <= 0 || entLen > 8*1024*1024 || modLen <= 0) { fclose(fp); return; }

    // models lump: dmodel_t = { float mins[3],maxs[3],origin[3]; int hn,ff,nf } = 48 bytes
    int nModels = modLen / 48;
    float *models = (float *)malloc(modLen);
    fseek(fp, modOfs, SEEK_SET);
    if (!models || fread(models, 1, modLen, fp) != (size_t)modLen) { free(models); fclose(fp); return; }

    char *ent = (char *)malloc(entLen + 1);
    fseek(fp, entOfs, SEEK_SET);
    if (!ent || fread(ent, 1, entLen, fp) != (size_t)entLen) { free(ent); free(models); fclose(fp); return; }
    ent[entLen] = 0;
    fclose(fp);

    // walk entity blocks; for each ins_objective read model "*N" and mapid
    char *p = ent;
    while ((p = strchr(p, '{')) && m_letterCount < INSOBJ_MAX * 2)
    {
        char *e = strchr(p, '}'); if (!e) break;
        *e = 0;
        char classname[64];
        bool isObjective = false;
        const char *cp = strstr(p, "\"classname\"");
        if (cp)
        {
            const char *v = strchr(cp + 11, '"');
            if (v)
            {
                const char *vs = v + 1;
                const char *ve = strchr(vs, '"');
                if (ve && ve > vs)
                {
                    size_t cl = (size_t)(ve - vs);
                    if (cl >= sizeof(classname)) cl = sizeof(classname) - 1;
                    memcpy(classname, vs, cl);
                    classname[cl] = 0;
                    isObjective = IsConfiguredObjectiveClass(classname);
                }
            }
        }
        if (isObjective)
        {
            int modelIdx = -1, mapid = -1;
            char *mp = strstr(p, "\"model\"");
            if (mp) { char *star = strchr(mp + 7, '*'); if (star) modelIdx = atoi(star + 1); }
            char *ip = strstr(p, "\"mapid\"");
            if (ip) { char *v = strchr(ip + 7, '"'); if (v) mapid = atoi(v + 1); }   // v+1 = first char of the value
            if (modelIdx >= 0 && modelIdx < nModels && mapid >= 0 && mapid < nLetters)
            {
                float *m = models + modelIdx * 12;   // mins[0..2], maxs[3..5]
                Vector c((m[0] + m[3]) * 0.5f, (m[1] + m[4]) * 0.5f, (m[2] + m[5]) * 0.5f);
                char L = letters[mapid];
                if (L >= 'a' && L <= 'z') L = (char)(L - 'a' + 'A');   // uppercase
                m_letters[m_letterCount].c = c;
                m_letters[m_letterCount].letter = L;
                m_letterCount++;
            }
        }
        *e = '}';
        p = e + 1;
    }
    free(ent); free(models);
    Msg("[insbots] resolved %d objective letters from map data\n", m_letterCount);
}

void CObjectives::ScanEntities()
{
    m_count = 0;
    int maxEnt = gpGlobals ? gpGlobals->maxEntities : 2048;

    // pass 1: find the objective brush entities
    for (int i = 0; i < maxEnt && m_count < INSOBJ_MAX; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (!e || e->IsFree()) continue;
        const char *cn = e->GetClassName();
        if (!cn || !IsConfiguredObjectiveClass(cn)) continue;

        IServerUnknown *unk = e->GetUnknown();
        ICollideable *col = unk ? unk->GetCollideable() : 0;
        if (!col) continue;

        Vector mn, mx;
        col->WorldSpaceSurroundingBounds(&mn, &mx);
        Vector c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f);

        float dx = (mx.x - mn.x) * 0.5f, dy = (mx.y - mn.y) * 0.5f;
        float rad = sqrtf(dx * dx + dy * dy);
        if (rad < 80.0f) rad = 80.0f;
        if (rad > 500.0f) rad = 500.0f;

        m_objs[m_count].center = c;
        m_objs[m_count].radius = rad;
        m_objs[m_count].minX = mn.x;
        m_objs[m_count].maxX = mx.x;
        m_objs[m_count].minY = mn.y;
        m_objs[m_count].maxY = mx.y;
        m_objs[m_count].minZ = mn.z;
        m_objs[m_count].maxZ = mx.z;
        m_objs[m_count].edictIndex = i;
        m_objs[m_count].isSpawn = false;
        m_objs[m_count].isEnclosed = false;
        m_objs[m_count].phonetic = LetterFor(c);
        m_objs[m_count].orderId = -1;   // resolved by RefreshOrderIds()
        m_count++;
    }

    // pass 2: mark objectives that are SPAWN ZONES by proximity to player spawn
    // points (ins_spawnpoint). This is reliable and mode-independent: spawn zones
    // have a cluster of spawns on them, the capturable middle objectives don't.
    int spawnCount[INSOBJ_MAX];
    for (int k = 0; k < m_count; k++) spawnCount[k] = 0;

    for (int i = 0; i < maxEnt; i++)
    {
        edict_t *e = engine->PEntityOfEntIndex(i);
        if (!e || e->IsFree()) continue;
        const char *cn = e->GetClassName();
        if (!cn || strcmp(cn, "ins_spawnpoint") != 0) continue;

        IServerUnknown *unk = e->GetUnknown();
        ICollideable *col = unk ? unk->GetCollideable() : 0;
        if (!col) continue;
        Vector sp = col->GetCollisionOrigin();

        for (int k = 0; k < m_count; k++)
        {
            Vector d = m_objs[k].center - sp;
            // Vertical separation: a spawn only counts for this objective when
            // it is inside the brush Z interval or within 128 units of the
            // nearer bound, so a stacked floor with overlapping XY cannot mark
            // an objective on another level as a spawn zone.
            float dz = 0.0f;
            if (sp.z < m_objs[k].minZ) dz = m_objs[k].minZ - sp.z;
            else if (sp.z > m_objs[k].maxZ) dz = sp.z - m_objs[k].maxZ;
            if (d.x * d.x + d.y * d.y < 400.0f * 400.0f && dz <= 128.0f) spawnCount[k]++;
        }
    }
    for (int k = 0; k < m_count; k++)
        if (spawnCount[k] >= 4) m_objs[k].isSpawn = true;   // >=4 spawns nearby => spawn zone

    // pass 3: detect if objective is enclosed indoors or in a basement
    extern IEngineTrace *enginetrace;
    if (enginetrace)
    {
        for (int k = 0; k < m_count; k++)
        {
            Vector start = m_objs[k].center + Vector(0, 0, 24.0f);
            Vector end = start + Vector(0, 0, 800.0f);
            Ray_t ray;
            ray.Init(start, end);
            trace_t tr;
            CTraceFilterWorldOnly filter;
            enginetrace->TraceRay(ray, MASK_SOLID, &filter, &tr);
            if (tr.fraction < 1.0f && !(tr.surface.flags & SURF_SKY))
            {
                m_objs[k].isEnclosed = true;
                Msg("[insbots] objective %d (%c) detected as enclosed indoor/basement\n", k, m_objs[k].phonetic ? m_objs[k].phonetic : '?');
            }
            else
            {
                m_objs[k].isEnclosed = false;
            }
        }
    }

    m_scanned = true;
}

void CObjectives::NoteSpawn(int team, const Vector &pos)
{
    if (team < 1 || team > 2) return;
    // running average
    int n = m_homeSamples[team];
    if (n == 0) m_home[team] = pos;
    else
    {
        float a = 1.0f / (float)(n + 1);
        m_home[team] = m_home[team] * (1.0f - a) + pos * a;
    }
    if (m_homeSamples[team] < 1000000) m_homeSamples[team]++;
}

// project p onto the team1->team2 axis, normalized 0..1 (clamped a bit beyond)
float CObjectives::AxisT(const Vector &p) const
{
    if (m_homeSamples[1] == 0 || m_homeSamples[2] == 0) return 0.5f;
    Vector a = m_home[1], b = m_home[2];
    Vector ab = b - a;
    float len2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
    if (len2 < 1.0f) return 0.5f;
    Vector ap = p - a;
    float t = (ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / len2;
    return t;
}

// Read the configured ownership field from the live
// ins_objective entity. Returns 0=neutral, 1/2=owning team, -1 if unreadable.
int CObjectives::GetOwnerTeam(int objIdx) const
{
    if (objIdx < 0 || objIdx >= m_count) return -1;
    CBaseEntity *ent = ObjEntity(m_objs[objIdx].edictIndex);
    if (!ent) return -1;

    // Linux uses the verified m_iCapturedTeam at 0x360. Windows retains its
    // existing configurable field. team_flip remains an administrator option.
    int to = insbot_team_offset.GetInt();
    int v;
    if (!TryReadEntityInt(ent, to, v)) return -1;      // unreadable, not neutral
    if (v != 1 && v != 2) return 0;                 // neutral
    return insbot_team_flip.GetBool() ? (3 - v) : v;
}

int CObjectives::GetHidden(int objIdx) const
{
    if (objIdx < 0 || objIdx >= m_count) return -1;
    CBaseEntity *ent = ObjEntity(m_objs[objIdx].edictIndex);
    if (!ent) return -1;
    int off = insbot_hidden_offset.GetInt();
    unsigned char h;
    if (!TryReadEntityByte(ent, off, h)) return -1;
    return h ? 1 : 0;
}

void CObjectives::DebugDump() const
{
    Msg("[insbots] === objective DECISION dump (%d objectives) ===\n", m_count);
    Msg("  homes: team1(%.0f %.0f %.0f) samples=%d | team2(%.0f %.0f %.0f) samples=%d%s\n",
        m_home[1].x, m_home[1].y, m_home[1].z, m_homeSamples[1],
        m_home[2].x, m_home[2].y, m_home[2].z, m_homeSamples[2],
        (m_homeSamples[1] == 0 || m_homeSamples[2] == 0)
            ? "   *** ONE HOME UNKNOWN -> push axis is FLAT (depth=0.5 for all) ***" : "");

    Msg("  gametype=%s ordered=%d push_order=%d\n",
        m_gametype == 1 ? "PUSH" : m_gametype == 2 ? "FIREFIGHT" :
        m_gametype == 3 ? "BATTLE" : "unknown", m_ordered ? 1 : 0,
        insbot_push_order.GetBool() ? 1 : 0);
    Msg("  idx let  order  center                owner  raw  rel(t1/t2)  active?  spawn?  depth\n");
    for (int i = 0; i < m_count; i++)
    {
        int owner = GetOwnerTeam(i);
        bool sz = IsSpawnZone(i, 1);
        float depth = AxisT(m_objs[i].center);
        const char *ostr = owner == 0 ? "neutral" : owner == 1 ? "TEAM1" : owner == 2 ? "TEAM2" : "?";
        char lc = m_objs[i].phonetic ? m_objs[i].phonetic : '?';
        // raw stored value at the ownership offset (pre-flip) so mismatches are visible
        int raw = -999;
        CBaseEntity *ent = ObjEntity(m_objs[i].edictIndex);
        int to = insbot_team_offset.GetInt();
        if (ent) TryReadEntityInt(ent, to, raw);
        Msg("  %-4d %c    %-5d  (%6.0f %6.0f %5.0f)  %-7s  %-3d  %+d / %+d    %-7s  %-6s  %.2f\n",
            i, lc, m_objs[i].orderId, m_objs[i].center.x, m_objs[i].center.y, m_objs[i].center.z,
            ostr, raw, Relation(i, 1), Relation(i, 2),
            sz ? "NO(sz)" : "yes", m_objs[i].isSpawn ? "scan" : "-", depth);
    }

    // what an all-attacker (aggression 1) on each team would pick RIGHT NOW
    for (int team = 1; team <= 2; team++)
    {
        int g = PickGoal(team, m_home[team], -1, 1.0f, -1, -1);
        Vector push; bool hasPush = PushTarget(team, m_home[team], push);
        const char *gs = (g == PICK_ADVANCE) ? "ADVANCE (nothing to capture -> push enemy)"
                       : (g < 0) ? "none" : "objective";
        if (g >= 0)
            Msg("  team%d attacker would target OBJ %d [%c] (%.0f %.0f)\n", team, g,
                m_objs[g].phonetic ? m_objs[g].phonetic : '?',
                m_objs[g].center.x, m_objs[g].center.y);
        else
            Msg("  team%d attacker -> %s | raw=(%.0f %.0f) proxy=%s(%.0f %.0f)\n", team, gs, m_home[team].x, m_home[team].y,
                hasPush ? "reachable" : "NONE(roam)", push.x, push.y);
    }
    Msg("[insbots] Run this right AFTER a capture where bots go passive. If 'active?' shows NO(sz)\n");
    Msg("          for the points you expect them to take, the active-flag offset is wrong for this mode.\n");
}

bool CObjectives::HasHostileOrNeutral(int team) const
{
    for (int i = 0; i < m_count; i++)
    {
        if (!IsSpawnZone(i, team) && (Relation(i, team) == 0 || Relation(i, team) == -1))
            return true;
    }
    return false;
}

int CObjectives::Relation(int objIdx, int team) const
{
    int owner = GetOwnerTeam(objIdx);
    if (owner < 0) return -2;
    if (owner == 0) return 0;                 // neutral or unknown
    return (owner == team) ? 1 : -1;          // friendly / hostile
}

bool CObjectives::IsSpawnZone(int objIdx, int /*team*/) const
{
    if (objIdx < 0 || objIdx >= m_count) return true;
#ifdef _LINUX
    // Linux CINSObjective::IsValid is exactly:
    //   m_parent (0x344) != NULL && m_iOrderID (0x34c) != -1
    // and IsCapturable additionally rejects m_bHidden. The old 0x3b4 read is
    // inside m_OnCaptureTeam2's output object, not an active-objective field.
    CBaseEntity *ent = ObjEntity(m_objs[objIdx].edictIndex);
    if (ent)
    {
        int parent = 0, orderId = -1;
        if (TryReadEntityInt(ent, INS_LINUX_OBJECTIVE_PARENT, parent)
            && TryReadEntityInt(ent, INS_LINUX_OBJECTIVE_ORDER_ID, orderId))
        {
            if (parent == 0 || orderId == -1) return true;
            int hidden = GetHidden(objIdx);
            return hidden > 0;
        }
    }
#else
    // the game's "is a real objective" flag (0x3b4): 0 => spawn zone / non-
    // objective marker. This reliably separates the real capturable objectives
    // from spawn zones (confirmed against live objdebug data).
    CBaseEntity *ent = ObjEntity(m_objs[objIdx].edictIndex);
    if (ent)
    {
        int ao = insbot_active_offset.GetInt();
        int av;
        if (TryReadEntityInt(ent, ao, av))
            return (av == 0);
    }
#endif
    // fallback if unreadable: the spawn-point-proximity flag from scan time
    return m_objs[objIdx].isSpawn;
}

int CObjectives::PickGoal(int team, const Vector &botPos, int curGoal, float aggression, int spread, int avoid,
                          bool forceCapture) const
{
    if (m_count == 0) return PICK_ADVANCE; // TDM support: roam and hunt if no objectives exist
    if (m_homeSamples[team] == 0) return PICK_ADVANCE; // TDM support: roam if no spawn zones exist
    if (aggression < 0) aggression = 0; if (aggression > 1) aggression = 1;

    // desired push depth along the home1->home2 axis:
    //  team 1 defends near t=0 and attacks toward t=1; team 2 is mirrored.
    float targetT = (team == 1) ? aggression : (1.0f - aggression);

    // gather contest objectives (skip spawn zones and the avoided one)
    int elig[INSOBJ_MAX], ne = 0;
    for (int i = 0; i < m_count; i++)
        if (i != avoid && !IsSpawnZone(i, team)) elig[ne++] = i;
    if (ne == 0)
        return PICK_ADVANCE;   // no contest points at all -> push into enemy ground

    (void)targetT; (void)botPos; (void)curGoal;

    // ------------------------------------------------------------------
    // TEAM-FOCUS TARGETING (replaces per-bot distance scoring, which let
    // bots camp whatever point happened to be nearest to them).
    // ------------------------------------------------------------------
    int frnd[INSOBJ_MAX], nf = 0;
    int targ[INSOBJ_MAX], nt = 0;
    for (int k = 0; k < ne; k++)
    {
        int i = elig[k];
        int rel = Relation(i, team);
        if (rel > 0) frnd[nf++] = i;
        else if (rel == 0 || rel == -1) targ[nt++] = i;
    }

    // ------------------------------------------------------------------
    // PUSH ORDERING: on a sequential map the engine only accepts a capture
    // on the next objective in the profile's chain, so "nearest to home" can
    // commit the team to an objective that cannot be captured yet.
    //
    // Rule: among eligible (non-spawn, non-friendly) objectives take the
    // lowest valid m_iOrderID. This is the same rule for both teams - the
    // engine's gate is "lowest-order objective you do not own", which also
    // gives defenders their correct fallback target on capturingfallback
    // maps. ins_karkar is the case that forces this: its profile uses four of
    // eight entities and the order IDs are non-contiguous, so no geometric
    // or mapid-based rule reproduces the chain.
    //
    // Every failure mode falls through to the pre-existing selection below:
    // non-Push gametype (m_ordered false), ordering disabled by cvar, no
    // eligible objective carrying a valid order ID (custom maps), or an
    // unreadable field.
    // ------------------------------------------------------------------
    int orderedTarget = -1;
    if (m_ordered && insbot_push_order.GetBool() && nt > 0)
    {
        int candOrder[INSOBJ_MAX];
        bool candElig[INSOBJ_MAX];
        for (int k = 0; k < nt; k++)
        {
            candOrder[k] = m_objs[targ[k]].orderId;
            candElig[k] = true;
        }
        int pick = InsLowestOrderIndex(candOrder, candElig, nt);
        if (pick >= 0) orderedTarget = targ[pick];
    }

    // ATTACK TARGET = the nearest eligible enemy/neutral objective to our own
    // HOME. With a nav mesh loaded, "nearest" is measured by actual nav path
    // cost so a straight-line-close objective on another floor (lower
    // basement, roof-only areas) is not picked over a longer but reachable
    // route. Falls back to straight-line distance when the mesh is unavailable
    // or an objective has no nav area.
    Vector ref = (m_homeSamples[team] > 0) ? m_home[team] : botPos;
    int atkTarget = -1; float bestKey = 3.4e38f;
    int startArea = g_NavMesh.IsLoaded() ? g_NavMesh.FindArea(ref) : -1;
    if (startArea < 0 && g_NavMesh.IsLoaded())
        startArea = g_NavMesh.FindNearestArea(ref);
    // Ordered maps already know the target; skip the A* cost comparison.
    for (int k = 0; orderedTarget < 0 && k < nt; k++)
    {
        int i = targ[k];
        Vector d = m_objs[i].center - ref;
        float dist = sqrtf(d.x * d.x + d.y * d.y);
        if (startArea >= 0)
        {
            int goalArea = g_NavMesh.FindArea(m_objs[i].center);
            if (goalArea < 0) goalArea = g_NavMesh.FindNearestArea(m_objs[i].center);
            if (goalArea >= 0)
            {
                float navCost = g_NavMesh.GetPathCost(startArea, goalArea, 0);
                // With a valid NAV area at each end, a failed route means the
                // objective is unreachable from this team's home. Treating the
                // Euclidean distance as a fallback repeatedly committed bots to
                // disconnected floors/components.
                if (navCost < 0.0f) continue;
                dist = navCost;
            }
        }
        float key = dist - (Relation(i, team) == 0 ? 500.0f : 0.0f);   // prefer neutral
        if (key < bestKey) { bestKey = key; atkTarget = i; }
    }

    if (orderedTarget >= 0) atkTarget = orderedTarget;

    // A small, stable capture detail must always drive the active front-line
    // objective. This is used when a map's reqpercent needs more than one body
    // in the zone; it deliberately bypasses the ordinary attacker/defender split.
    if (forceCapture && atkTarget >= 0) return atkTarget;

    // frontline friendly points (deepest toward the enemy first)
    int def1 = -1, def2 = -1; float d1 = -3.4e38f, d2 = -3.4e38f;
    for (int k = 0; k < nf; k++)
    {
        int i = frnd[k];
        float t = AxisT(m_objs[i].center);
        float depth = (team == 1) ? t : 1.0f - t;
        if (depth > d1)      { d2 = d1; def2 = def1; d1 = depth; def1 = i; }
        else if (depth > d2) { d2 = depth; def2 = i; }
    }

    bool attacker = InsRoleAttacker(spread, aggression);

    if (attacker)
    {
        if (atkTarget >= 0) return atkTarget;   // capture the next point in the chain
        // Every capturable objective is already friendly. Do not send an
        // aggression=1 attacker back to the friendly point nearest the enemy
        // home; push into enemy territory while defenders hold owned points.
        return PICK_ADVANCE;
    }
    // defender: hold the frontline friendly point - but ONLY while there's
    // still something contested to defend against. If every point is ours
    // (nothing left to take), holding is pointless idling; push instead.
    if (def1 >= 0 && nt > 0)
    {
        if (def2 >= 0 && spread >= 0 && (spread & 1)) return def2;
        return def1;
    }
    // nothing friendly worth holding: help take the next point, else push.
    if (atkTarget >= 0) return atkTarget;
    return PICK_ADVANCE;
}

bool CObjectives::PushTarget(int team, const Vector &from, Vector &out) const
{
    int enemy = (team == 1) ? 2 : 1;
    if (m_homeSamples[enemy] <= 0) return false;
    if (!g_NavMesh.IsLoaded())
    {
        // No mesh means we cannot resolve a NAV proxy; preserve the old
        // raw-home push so bots still move toward the learned enemy territory.
        out = m_home[enemy];
        return true;
    }
    int startArea = g_NavMesh.FindArea(from);
    if (startArea < 0) startArea = g_NavMesh.FindNearestArea(from);
    if (startArea < 0) return false;
    int proxy = g_NavMesh.FindClosestReachableArea(startArea, m_home[enemy]);
    const NavAreaData *a = g_NavMesh.Area(proxy);
    if (!a) return false;
    out = a->center;
    return true;
}
