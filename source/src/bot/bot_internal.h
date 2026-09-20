// ============================================================================
// bot_internal.h - declarations shared between bot/ translation units.
//
// This header is PRIVATE to source/src/bot/. The public bot interface lives in
// ../bot.h and must not be extended from here.
//
// A symbol belongs here only if it is referenced from more than one file under
// bot/. Symbols with a single consuming TU stay `static` in that TU.
//
// Every declaration below is transcribed verbatim from the definition that
// currently lives in bot.cpp. When a definition moves to its home TU, only its
// `static` (and any `inline`) keyword is removed - the body never changes.
// ============================================================================
#pragma once

#include "bot.h"
#include "cfgvar.h"

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

// ---- util (bot_util.cpp) ---------------------------------------------------
void BotDbg(const char *fmt, ...);
float frand(float lo, float hi);
int RandomIndex(int count);
float xyd(const Vector &a, const Vector &b);
bool CommandContainsToken(const char *command, const char *token);
float DistanceToLineSegment(const Vector& p, const Vector& a, const Vector& b);
float SignedAngleDiff(float a, float b);

// ---- map-identity predicates (bot_util.cpp) --------------------------------
bool IsHadithaMapName(const char *name);
bool IsHadithaTightDoorEdge(int fromIdx, int toIdx);
bool IsHadithaEastDoorEdge(int fromIdx, int toIdx);

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
int   CombatClassForWeapon(const WeaponProfile *wp);
int   CombatReserveMags(const WeaponProfile *wp);
float CombatValue(CfgVar *classValue, CfgVar &fallback);
float CombatRecoilControl(CombatProfile *combat, float aimError);
extern WeaponProfile s_wpDefault;
extern const char *s_BotNamesUSMC[];
extern const char *s_BotNamesIraqi[];

// ---- navigation (bot_path.cpp) ---------------------------------------------
bool   GetSeparatedTransitionPortal(int fromIdx, int toIdx, Vector &mid, Vector &axis,
                                    float &halfSpan, Vector *insideDest);
bool   PlayerInsideObjective(const ObjPoint *op, IPlayerInfo *info, const Vector &pos);
Vector ObjectiveHoldPoint(const ObjPoint *op, const Vector &pos, float angle, float scale);
Vector ObjectiveCornerHoldPoint(const ObjPoint *op, const Vector &entry, int ordinal);
float  OpenHoldYaw(const Vector &pos, float preferredYaw);
bool   PositionOnLadderLanding(int areaIdx, const Vector &pos);
bool   WalkEdgeNeedsJump(int fromIdx, int toIdx);
bool   ProbePhysicalPortal(int fromIdx, int toIdx,
                           Vector *safeCenter, float *safeHalfSpan);

// ---- doorway (bot_doorway.cpp) ---------------------------------------------
bool HasDoorwayLineOfSight(const Vector &from, const Vector &to);

// ---- haditha behaviour (bot_haditha.cpp) -----------------------------------
bool IsHadithaDoorNoSprintZone(const int *path, int pathLen, int cursor, const Vector &pos);

// ---- steering (bot_steer.cpp) ----------------------------------------------
void MoveAtWorldYaw(float worldYaw, float viewYaw, float speed, float &fwd, float &side);

// ---- body / collision groups (bot_body.cpp) --------------------------------
bool WritePlayerCollisionGroup(edict_t *edict, int collisionGroup);

// ---- ladder (bot_ladder.cpp) -----------------------------------------------
float  LadderLipTol();
Vector LadderAreaInterior(int areaIdx, const Vector &lip);

// ---- debug overlay (bot_think.cpp) -----------------------------------------
void DrawBoxTW(const Vector &mn, const Vector &mx, int r, int g, int b, float dur);

// ---- learn persistence (bot_entry.cpp) -------------------------------------
extern char s_learnPath[512];
