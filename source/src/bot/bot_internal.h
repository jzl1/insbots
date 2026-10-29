// ============================================================================
// bot_internal.h - declarations shared between bot/ translation units.
//
// This header is PRIVATE to source/src/bot/. The public bot interface lives in
// ../bot.h and must not be extended from here.
//
// A symbol belongs here only if it is referenced from more than one file under
// bot/. Symbols with a single consuming TU stay `static` in that TU.
//
// Every declaration below is transcribed verbatim from the definition in its
// home TU. When a definition moves to that home TU, only its `static` (and any
// `inline`) keyword is removed - the body never changes.
// ============================================================================
#pragma once

#include "bot.h"
#include "cfgvar.h"
#include "engine/IEngineTrace.h"

// ---- INS:MIC usercmd button bits -------------------------------------------
// The stock SDK's in_buttons.h definitions do not match Insurgency's custom
// layout. Shared because the movement, ladder and steering TUs all build the
// same final usercmd.
#define INS_IN_ATTACK    (1 << 0)   // 1 (Primary Attack / Fire / Launch)
#define INS_IN_JUMP      (1 << 1)   // 2
#define INS_IN_DUCK      (1 << 2)   // 4
#define INS_IN_PRONE     (1 << 3)   // 8
#define INS_IN_FORWARD   (1 << 4)   // 16
#define INS_IN_BACK      (1 << 5)   // 32
#define INS_IN_USE       (1 << 6)   // 64
#define INS_IN_MOVELEFT  (1 << 9)   // 512
#define INS_IN_MOVERIGHT (1 << 10)  // 1024
#define INS_IN_RELOAD    (1 << 11)  // 2048 (Reload)
#define INS_IN_SPRINT    (1 << 15)  // 32768 (Sprint)
#define INS_IN_ATTACK2   (1 << 17)  // 131072 (Secondary Attack: M203 leaf sight, Bipod)
#define INS_IN_ATTACK3   (1 << 18)  // 262144 (Tertiary Attack: Ironsights toggle)

// ---- trace helpers shared by every bot/ TU ---------------------------------
// These were file-scope helpers in bot.cpp. They are used by more than one bot/
// translation unit, so they live here as inline definitions rather than being
// duplicated (which would give each TU a distinct type) or exported (which
// would cost a call on the hottest trace path).

// Skip only the tracing bot itself.
class CTraceFilterSkipSelf : public CTraceFilter
{
public:
    CTraceFilterSkipSelf(IHandleEntity *self) : m_self(self) {}
    virtual bool ShouldHitEntity(IHandleEntity *pEntity, int contentsMask)
    {
        return pEntity != m_self;
    }
    IHandleEntity *m_self;
};

// Geometry traces used for doors must see through every player. Otherwise a
// follower traces into the lead bot and never sees the closed door behind it.
// The client handle list is rebuilt once per frame by HearingFrameUpdate()
// (bot_perceive.cpp); these externs are what makes that list visible to the
// other TUs that trace doors.
extern IHandleEntity *s_tracePlayerHandles[66];
extern int            s_tracePlayerHandleCount;

class CTraceFilterSkipPlayers : public CTraceFilter
{
public:
    CTraceFilterSkipPlayers(IHandleEntity *self) : m_self(self) {}

    virtual bool ShouldHitEntity(IHandleEntity *pEntity, int /*contentsMask*/)
    {
        if (pEntity == m_self) return false;
        for (int i = 0; i < s_tracePlayerHandleCount; i++)
            if (pEntity == s_tracePlayerHandles[i]) return false;
        return true;
    }

private:
    IHandleEntity *m_self;
};

// Obstacle filter for movement probes: world geometry, driveable vehicle
// hulls, map brush entities and enemy players on foot are solid; the tracing
// bot itself, its teammates, and every seated vehicle rider are not.
//
// This lives in the shared header because both bot_steer.cpp (forward probes)
// and bot_recover.cpp (lateral sidestep and teleport landing checks) need the
// SAME obstacle definition. Two copies would drift, and the divergence is
// exactly the bug this replaces: SideStepOpen used
// CTraceFilterWorldAndPropsOnly, whose ShouldHitEntity always returns false,
// so a Humvee hull was invisible to every lateral move it validated.
//
// Physics props (crates, barrels, dropped weapons, ragdolls) are deliberately
// NOT obstacles here - they were excluded before this change and that
// behaviour is preserved.
#define INS_STEER_SKIP_MAX 70

class CInsBot;
class CTraceFilterSteerObstacles : public CTraceFilter
{
public:
    explicit CTraceFilterSteerObstacles( CInsBot *bot );

    void Add( IHandleEntity *h )
    {
        if (h && m_n < INS_STEER_SKIP_MAX) m_skip[m_n++] = h;
    }

    virtual bool ShouldHitEntity( IHandleEntity *pEntity, int contentsMask );

private:
    IHandleEntity *m_skip[INS_STEER_SKIP_MAX];
    int            m_n;
};

inline IHandleEntity *EdictHandleEntity(edict_t *e)
{
    if (!e) return 0;
    IServerUnknown *unk = e->GetUnknown();
    return (IHandleEntity *)unk;
}

// ---- shared value types ----------------------------------------------------
// These were defined locally in bot.cpp. They move here because several
// declarations below need them, and a private header is the right home for
// types that only bot/ translation units use.

// Per team (1,2) last-known position of each enemy entity index. Bots write
// what they see; teammates read to react. Cleared by staleness.
struct Sighting { Vector pos; float time; bool valid; };
struct TeamIntel { Sighting enemy[66]; };

enum WeaponCombatClass
{
    WC_RIFLE = 0,
    WC_LMG,
    WC_SNIPER,
    WC_SHOTGUN,
    WC_PISTOL,
    WC_RPG,
    WC_GRENADE_LAUNCHER,
    WC_COUNT = INSBOT_COMBAT_CLASS_COUNT
};

// WeaponProfile::flags bits. Shared because IsSniper() (bot_core.cpp) reads
// WF_SNIPER while the profile table itself lives in bot.cpp.
enum
{
    WF_SHOTGUN = 1,
    WF_BOLT = 2,
    WF_LAUNCHER = 4,
    WF_SNIPER = 8,
    WF_EMPTY_RELOAD_ONLY = 16
};

// WeaponProfile::modes bits. Shared because the profile table lives in
// bot_loadout.cpp while ThinkAlive (bot_think.cpp) and bot.cpp select the
// fire mode from these bits.
enum
{
    FM_SEMI = 1,
    FM_BURST = 2,
    FM_AUTO = 4
};

struct WeaponProfile
{
    const char   *cls;       // classname suffix (after "weapon_")
    short         mag;
    unsigned char modes;
    short         rpm;        // cyclic rate, rounds/min
    short         burstRpm;   // rate within a burst (0 => use rpm)
    float         recoil;
    unsigned char flags;
    unsigned char combatClass;
    CfgVar       *safe_reload_cvar;
};

struct CombatProfile
{
    const char *name;
    CfgVar *detectRange;
    CfgVar *engageRange;
    CfgVar *stopDist;
    CfgVar *reaction;
    CfgVar *aimError;
    CfgVar *aimSpeed;
    CfgVar *reserveMags;
    CfgVar *recoilControl;
    CfgVar *recoilPercent;
};

// ---- combat profile lookup (bot.cpp) ---------------------------------------
CombatProfile *CombatForWeapon(const WeaponProfile *wp);

// ---- util (bot_util.cpp) ---------------------------------------------------
void BotDbg(const char *fmt, ...);
float frand(float lo, float hi);
int RandomIndex(int count);
float xyd(const Vector &a, const Vector &b);
bool CommandContainsToken(const char *command, const char *token);
float DistanceToLineSegment(const Vector& p, const Vector& a, const Vector& b);
float SignedAngleDiff(float a, float b);

// ---- map-identity predicates (bot_util.cpp) --------------------------------
bool IsTightEdge(int fromIdx, int toIdx);

// ---- shared team intelligence (bot_perceive.cpp) ---------------------------
void IntelReport(int team, int enemyIdx, const Vector &pos, float now);
bool IntelEnemyIsLive(int team, int enemyIdx);
int  IntelFreshest(int team, float now, float maxAge, const Vector &listenerPos,
                   float maxRange, Vector &outPos);
void IntelReset();
void HearingReset();
void HearingFrameUpdate();
extern TeamIntel g_Intel[3];
extern float     s_hearSpeed[66];

// ---- loadout (bot_loadout.cpp) ---------------------------------------------
int   GetTeamSlotForBotName(const char *name, int defaultSlot);
int   TeamSlotToTeamIndex(int teamSlot);
// Called by CBotRoster::AddBot/AddBotToRole (bot_roster.cpp) as well as by the
// loadout helpers that still live in bot.cpp.
int   GetTeamSlotForFaction(int targetFaction);
int   PickPriorityClassRole(int teamSlot);
int   CombatClassForWeapon(const WeaponProfile *wp);
int   CombatReserveMags(const WeaponProfile *wp);
float CombatValue(CfgVar *classValue, CfgVar &fallback);
float CombatRecoilControl(CombatProfile *combat, float aimError);
extern WeaponProfile s_wpDefault;
// Declared with an explicit bound: CInsBot::Create() (bot_core.cpp) needs
// sizeof() on these to wrap its cursor, and an unsized extern array is an
// incomplete type in any TU that does not define it.
extern const char *s_BotNamesUSMC[16];
extern const char *s_BotNamesIraqi[16];

// ---- weapon inventory helpers (bot.cpp / bot_weapons.cpp) -------------------
// INS_MAX_WEAPONS is the m_hMyWeapons array bound in this Source branch.
#define INS_MAX_WEAPONS 7
// Defined in bot.cpp. Both bot_weapons.cpp and the loadout code that still
// lives in bot.cpp resolve carried weapons through these.
const WeaponProfile *WeaponFind(const char *name);
edict_t *InsBotResolveWeaponSlot(char *playerBase, int slot, int *pOutIndex);

// ---- navigation (bot_path.cpp) ---------------------------------------------
bool   GetSeparatedTransitionPortal(int fromIdx, int toIdx, Vector &mid, Vector &axis,
                                    float &halfSpan, Vector *insideDest);
bool   PlayerInsideObjective(const ObjPoint *op, IPlayerInfo *info, const Vector &pos);
Vector ObjectiveHoldPoint(const ObjPoint *op, const Vector &pos, float angle, float scale);
Vector ObjectiveCornerHoldPoint(const ObjPoint *op, const Vector &entry, int ordinal);
float  OpenHoldYaw(const Vector &pos, float preferredYaw);
bool   PositionOnLadderLanding(int areaIdx, const Vector &pos);
bool   WalkEdgeNeedsJump(int fromIdx, int toIdx);
// ---- doorway (bot_doorway.cpp) ---------------------------------------------
bool HasDoorwayLineOfSight(const Vector &from, const Vector &to);

// ---- steering (bot_steer.cpp) ----------------------------------------------
void MoveAtWorldYaw(float worldYaw, float viewYaw, float speed, float &fwd, float &side);

// ---- body / collision groups (bot_body.cpp) --------------------------------
bool WritePlayerCollisionGroup(edict_t *edict, int collisionGroup);
// Teleport failure codes are produced by TeleportUnstuck (bot_recover.cpp)
// and consumed by the body-recovery retry loop (bot_body.cpp), so the enum
// must be visible to both. TELEPORT_OK is the "no failure" value.
enum TeleportFailureReason
{
    TELEPORT_OK = 0,
    TELEPORT_DISABLED,
    TELEPORT_COOLDOWN,
    TELEPORT_NO_CONTROLLER,
    TELEPORT_NAV_NOT_LOADED,
    TELEPORT_NO_CURRENT_AREA,
    TELEPORT_NO_CONNECTED_EDGE,
    TELEPORT_DEST_AREA_MISSING,
    TELEPORT_NO_SAFE_DESTINATION
};
const char *TeleportFailureName(int reason);

// ---- ladder (bot_ladder.cpp) -----------------------------------------------
float  LadderLipTol();
Vector LadderAreaInterior(int areaIdx, const Vector &lip);

// ---- debug overlay (bot_think.cpp) -----------------------------------------
void DrawBoxTW(const Vector &mn, const Vector &mx, int r, int g, int b, float dur);

// ---- learn persistence (bot_entry.cpp) -------------------------------------
extern char s_learnPath[512];
