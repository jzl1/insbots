// ============================================================================
// bot.h - one AI-controlled fake client
// ============================================================================
#pragma once

#include "eiface.h"
#include "dlls/iplayerinfo.h"
#include "vector.h"
#include <math.h>   // atan2f/fmodf for the inline cover sector helpers below

struct ObjPoint;

#define INSBOT_MAX_PATH 512
#define INSBOT_COMBAT_CLASS_COUNT 7
#define INSBOT_SNIPER_OBJECTIVES 24
#define INSBOT_SNIPER_PENDING 128
#define INSBOT_SNIPER_BEST 16

enum SniperStance
{
    SNIPER_STANCE_NONE = 0,
    SNIPER_STANCE_STAND,
    SNIPER_STANCE_CROUCH
};

struct SniperPendingCandidate
{
    Vector pos;
    int areaIdx;
    float cheapScore;
    unsigned char flags;
};

struct SniperPerch
{
    Vector pos;
    Vector aimTarget;
    int areaIdx;
    float score;
    unsigned char stance;
    unsigned char flags;
    unsigned char visibleRays;
};

struct SniperObjectiveCache
{
    bool requested;
    bool ready;
    int pendingCount;
    int pendingCursor;
    int bestCount;
    int accepted;
    int rejected;
    SniperPendingCandidate pending[INSBOT_SNIPER_PENDING];
    SniperPerch best[INSBOT_SNIPER_BEST];
};

// ============================================================================
// Cover Point Database (take-cover overhaul v2)
// ============================================================================
enum CoverTier {
    COVER_NONE    = 0,
    COVER_SPOT    = 1,   // corner peek, protects one narrow arc
    COVER_PARTIAL = 2,   // protects a wide-ish arc
    COVER_FULL    = 3    // bunker-grade, blocks most directions
};

// Number of equal angular sectors a cover point samples. BuildCoverDatabase
// casts one ray per sector, centred on that sector's bearing; EVERY consumer
// must bin a threat with CoverSectorFor() so the sampler and the scorers cannot
// drift apart. Raising this raises mask fidelity at the cost of build rays and
// bytes per point - see the plan's Measured Basis.
#define COVER_SECTORS 12
#define COVER_SECTOR_DEG (360.0f / (float)COVER_SECTORS)

// Minimum sector hits for each tier, held as fractions of COVER_SECTORS
// (7/8, 4/8, 2/8) so the classification stays meaningful if the sector count
// changes. The rounding is a ceiling, which reproduces 7/4/2 exactly at 8.
#define COVER_TIER_FULL_MIN    ((COVER_SECTORS * 7 + 7) / 8)
#define COVER_TIER_PARTIAL_MIN ((COVER_SECTORS * 4 + 7) / 8)
#define COVER_TIER_SPOT_MIN    ((COVER_SECTORS * 2 + 7) / 8)

struct CoverPoint {
    Vector        pos;
    unsigned char blockMask[COVER_SECTORS];  // standing-eye occlusion per sector
    unsigned char crouchMask[COVER_SECTORS]; // crouch-eye occlusion per sector
    int           tier;          // CoverTier value at load time
    unsigned char isLowOnly;     // 1 if protection only below crouch height (crates/sandbags)
    short         areaIdx;       // owning nav area
};

// Bin the bearing from `from` to `to` into the sector whose sampled ray is
// angularly closest. Ties resolve toward the higher sector index, which is what
// the previous inline `(yaw + half) / width` arithmetic did.
inline int CoverSectorFor(const Vector &from, const Vector &to)
{
    float yawDeg = atan2f(to.y - from.y, to.x - from.x) * (180.0f / 3.14159265358979f);
    yawDeg = fmodf(yawDeg + 360.0f, 360.0f);
    int sec = (int)(fmodf(yawDeg + COVER_SECTOR_DEG * 0.5f, 360.0f) / COVER_SECTOR_DEG);
    return sec % COVER_SECTORS;
}

inline int CoverMaskPopcount(const unsigned char *mask)
{
    int n = 0;
    for (int d = 0; d < COVER_SECTORS; d++) if (mask[d]) n++;
    return n;
}

inline int CoverTierForPopcount(int popcount)
{
    if (popcount >= COVER_TIER_FULL_MIN)    return COVER_FULL;
    if (popcount >= COVER_TIER_PARTIAL_MIN) return COVER_PARTIAL;
    if (popcount >= COVER_TIER_SPOT_MIN)    return COVER_SPOT;
    return COVER_NONE;
}

// Does this point hide a bot standing at cp.pos from a threat at threatPos?
// `ducked` selects the crouch-eye mask, which is what makes low cover work.
inline bool CoverPointBlocks(const CoverPoint &cp, const Vector &threatPos, bool ducked)
{
    int sec = CoverSectorFor(cp.pos, threatPos);
    if (cp.blockMask[sec]) return true;
    return ducked && cp.crouchMask[sec] != 0;
}

// ============================================================================
// Doorway Runway & Sprint State
// ============================================================================
struct DoorwayRunwayState
{
    bool   active;
    Vector center;
    Vector normal;
    Vector runwayPoint;
    float  halfSpan;
    float  distToPortal;
    float  turnAngle;
    bool   isSwingDoor;
    bool   doorClear;
    bool   isChained;
    bool   apexBraked;
    bool   isImmediateEdge;
};

enum BotState
{
    BOT_JOINING = 0,     // sending team/class commands until alive
    BOT_ALIVE,           // playing
    BOT_DEAD,            // waiting on respawn wave
};

enum LadderPhase
{
    LADDER_PHASE_NONE = 0,
    LADDER_PHASE_APPROACH,
    LADDER_PHASE_QUEUED,
    LADDER_PHASE_ATTACHED,
    LADDER_PHASE_DISMOUNT,
    LADDER_PHASE_RECOVERY,
};

enum BotPathResult
{
    BOT_PATH_NONE = 0,
    BOT_PATH_VALID,
    BOT_PATH_OFFMESH,
    BOT_PATH_UNREACHABLE
};

struct LadderQueueState
{
    int ownerSlot;       // roster slot + 1; zero means unowned
    int lastDirection;   // +1 up, -1 down; alternate when both ends wait
};


enum SquadOrder
{
    SQUAD_ORDER_NONE = 0,
    SQUAD_ORDER_FOLLOW = 1,   // Form up in wedge and follow leader
    SQUAD_ORDER_HOLD = 2,     // Hold current position / perimeter defense
    SQUAD_ORDER_BACKUP = 3,   // Break cover and sprint directly to leader
    SQUAD_ORDER_SUPPRESS = 4, // Suppressive fire in leader aim direction
    SQUAD_ORDER_MOVETO = 5    // Advance to waypoint and hold defensive perimeter
};
class CInsBot
{
public:
    CInsBot();

    bool Create(const char *name, int teamSlot, int requestedTeamArg = -1,
                int requestedRoleId = -1);         // teamSlot: 0 = side A, 1 = side B
    void Adopt(edict_t *e, int teamSlot);          // re-attach to an orphaned fake client
    void OnRemoved();

    void Think(bool simulating);
    void DebugDraw();

    // Pending move: computed in GameFrame/ThinkAlive, submitted in SetCommandClient.
    // SetCommandClient is called by the engine RIGHT BEFORE each bot's physics
    // simulation ? submitting our RunPlayerMove there means CommandContextCount > 0
    // so the engine processes OUR command and skips RunNullCommand entirely.
    // This prevents RunNullCommand from generating m_afButtonReleased=IN_ATTACK,
    // which was causing full-auto weapons to trigger their reload-on-trigger-release
    // behavior after every single shot.
    float   m_pendingFwd;
    float   m_pendingSide;
    int     m_pendingButtons;
    QAngle  m_pendingView;
    bool    m_hasPending;      // true after first ThinkAlive computes a move
    int     m_lastButtons;     // to restore state across RunNullCommand

    void    RunMovePending();  // called from SetCommandClient

    edict_t        *m_edict;
    IBotController *m_ctrl;
    IPlayerInfo    *m_info;
    bool            m_inUse;
    char            m_name[64];
    int             m_teamSlot;      // 0/1 -> join cmd set A/B
    int             m_ordinal;       // per-side ordinal for {n} substitution
    int             m_requestedTeamArg; // targeted add: pfullsetup UI-team argument, or -1
    int             m_requestedRoleId;     // targeted add: exact sparse role ID, or -1
    bool            m_captureCommit; // roster-managed: currently in the capture detail
    int             m_captureObj;    // roster-managed: objective index this bot is dedicated to, -1 = none
    int             m_captureTeam;   // team that assigned the commitment; prevents team-switch carryover
    const char     *GetWeaponName() const { return m_wpName[0] ? m_wpName : (m_info ? m_info->GetWeaponName() : "unknown"); }
    bool            IsSniper() const;
    float           GetFireAccum() const { return m_fireAccum; }
    int             GetMagSize() const;
    int             GetClipRemaining() const;
    int             GetRoundsFired(int cls = -1) const;
    bool            InCombat(float now) const { return m_enemyIndex != 0 && now - m_enemyLastSeen < 2.0f; }
    const Vector   &GetEnemyLastPos() const { return m_enemyLastPos; }
    int             GetAvoidObj() const { return m_avoidObj; }
    float           GetAvoidUntil() const { return m_avoidUntil; }
    bool            IsAvoiding(int obj, float now) const { return m_avoidObj == obj && now < m_avoidUntil; }
    int             CurrentArea(const Vector &pos) const; // per-frame cached FindArea

private:
    friend class CBotRoster;
    // join logic
    float  m_nextJoinCmdTime;
    int    m_joinCmdIndex;
    int    m_joinCycles;
    int    m_resolvedRoleId;       // actual role currently occupied/setup for this bot
    float  m_lastAliveCheck;
    bool   m_joinWarned;
    bool   m_classSetupSent;       // true once pfullsetup has been transmitted for this life
    float  m_nextReinforcePulse;   // throttles creinforce heartbeat while waiting in wave queue

    // Model and loadout customization selected for the next life.
    bool   m_customizationPending;
    bool   m_customizationWeaponRolled;
    int    m_customizationFaction;
    int    m_customizationSkin;
    int    m_customizationBody;
    int    m_customizationWeapon;
    int    m_setupRoleId;
    bool   m_haveLastCustomization;
    int    m_lastCustomizationFaction;
    int    m_lastCustomizationSkin;
    int    m_lastCustomizationBody;
    bool   m_haveLastCustomizationWeapon;
    int    m_lastCustomizationRoleId;
    int    m_lastCustomizationWeapon;

    BotState m_state;

    // navigation
    int    m_path[INSBOT_MAX_PATH];
    int    m_pathLen;
    int    m_pathCursor;
    BotPathResult m_pathResult;
    bool   m_pathContinues;   // BuildPath returned a truncated prefix; keep repathing
    int    m_goalArea;
    float  m_repathTime;
    Vector m_moveTarget;       // current waypoint in world space
    bool   m_hasMoveTarget;
    bool   m_moveArrivalLatched; // inside the final-destination arrival dead zone
    Vector m_moveArrivalTarget;  // logical destination that owns the arrival latch
    float  m_lastNavMoveYaw;     // last non-degenerate immediate-target direction
    bool   m_haveLastNavMoveYaw;
    bool   m_traversingLadder; // this frame's path edge is a typed NAV ladder
    bool   m_ladderClimbing;   // persistent: bot is attached to a ladder shaft
    int    m_ladderClimbIdx;   // ladder record index currently being climbed
    bool   m_ladderClimbUp;
    int    m_ladderFromArea;   // cached typed edge for endpoint-aware path-loss recovery
    int    m_ladderToArea;
    int    m_ladderPhase;
    bool   m_ladderContactConfirmed;
    bool   m_ladderCompletionReady;
    bool   m_ladderReturnToSource;
    Vector m_ladderQueuePoint;

    // objective play
    int    m_objGoal;          // index into g_Objectives, -1 = none
    float  m_objRepick;        // next time to re-evaluate objective choice
    float  m_holdUntil;        // holding on an objective until this time
    bool   m_notedSpawn;       // recorded this life's spawn pos for home-learning
    int    m_avoidObj;         // objective to avoid (couldn't reach it), -1 = none
    float  m_avoidUntil;       // avoid m_avoidObj until this time

    // cached PICK_ADVANCE reachable proxy (per bot: different spawn
    // components can have different directed frontiers)
    Vector m_advanceTarget;
    bool   m_haveAdvanceTarget;
    float  m_advanceTargetRefresh;
    float  m_nextDecline;      // next time to decline a squad-leader promotion
    int    m_pathSeed;         // per-bot A* cost jitter seed (route diversity)

    // roaming fallback (no nav mesh)
    float  m_headingYaw;
    float  m_nextHeadingChange;

    // Blocked-episode anchoring for the shoulder probes. The shoulders are
    // always measured from the heading held when the episode BEGAN, never
    // from a heading a previous deflection already moved the bot to -
    // measuring from the deflected heading adds another 60 degrees on every
    // re-probe, which produced a 6-value spin cycle at speed 0.
    //
    // These used to be file-scope statics indexed by m_ordinal and were never
    // reset on client-slot reuse, so a fast respawn inherited the previous
    // life's episode. Adopt() memsets the whole object, so members are clean
    // by construction.
    float  m_steerBaseYaw;     // heading the current blocked episode started at
    float  m_steerBaseTime;    // when the current episode was last probed
    float  m_steerLastYaw;     // heading after the last deflection

    // vehicle-avoidance episode (bot_steer.cpp)
    bool   m_vehicleDeflect;      // true while deflecting around a vehicle hull
    float  m_vehicleDeflectUntil; // hold expiry for the current deflection
    float  m_vehicleDeflectYaw;   // committed world heading for this hold
    float  m_vehicleLastProbe;    // next time the forward vehicle probe may run

    // combat
    int    m_enemyIndex;       // entity index of current enemy, 0 = none

    // ---- anti-vehicle threat channel (SEPARATE from m_enemyIndex) ----------
    // A vehicle is a non-client edict: its entindex is > maxClients and
    // playerinfomanager->GetPlayerInfo() returns NULL for it. Passing such an
    // index into m_enemyIndex crashes at bot_think.cpp:795 (unchecked pi) and
    // would overflow g_Intel[team].enemy[] (size 66) and
    // m_leaderSquadThreatEnemy[] (size 65).
    //
    // This channel is therefore deliberately parallel and never shares a sink
    // with the infantry index. It holds a Humvee hull, not a player.
    int    m_vehicleEnemyIndex;   // entindex of the targeted Humvee, 0 = none
    float  m_vehicleSeenTime;     // when the current vehicle was acquired
    float  m_vehicleLastSeen;     // last confirmed line of sight
    Vector m_vehicleLastPos;      // last known hull origin
    float  m_enemySeenTime;    // when we first saw current enemy (reaction delay)
    float  m_enemyLastSeen;
    Vector   m_vecLastOrigin;     // to detect being stuck
    float    m_flOldNextAttack;   // to spoof RunNullCommand
    Vector m_enemyLastPos;
    float  m_nextEnemyScan;
    float  m_nextVehicleScan;     // 10 Hz vehicle threat scan
    float  m_fireStateEnd;     // burst/pause timer
    bool   m_firing;
    bool   m_ironsights;
    float  m_ironsightTime;
    bool   m_wantAds;          // ADS intent; the engine raises ironsights on a button-18 RELEASED edge
    float  m_aimErrX, m_aimErrY;
    float  m_nextAimErrTime;

    // stuck detection
    Vector m_stuckCheckPos;
    float  m_stuckCheckTime;
    int    m_stuckStage;
    float  m_unstuckUntil;
    float  m_unstuckSide;
    float  m_stuckSince;       // when the current continuous stuck began (0=not stuck)
    Vector m_navDest;          // the destination movement is currently aiming at
    bool   m_haveNavDest;      // set this frame if a nav destination was chosen
    float  m_bestDist;         // closest 2D distance reached to m_navDest (progress)
    bool   m_navDestChanged;   // waypoint changed since the previous stuck sample
    float  m_nextTeleportOk;   // teleport-unstuck cooldown (NavBot-style last resort)
    int    m_rescueFromArea;   // validated failed link cached before rerouting
    int    m_rescueToArea;
    bool   m_haveRescueLink;
    int    m_lastReportedFrom; // per-bot anti-spam for learned-link confidence
    int    m_lastReportedTo;
    float  m_lastReportedTime;
    bool   m_haveLastReportedLink;

    // Spawn crowd grace: use a trigger-capable non-player collision group for
    // a bounded amount of active round time, then restore only from a clear hull.
    bool   m_spawnNoCollideActive;
    bool   m_spawnNoCollideExpired;
    float  m_spawnNoCollideElapsed;
    float  m_spawnNoCollideRecoveryCheck;
    int    m_spawnOriginalCollisionGroup;

    // Body-block recovery owns one lease over a local player cohort. The
    // collision-group state itself is roster-managed because humans and bots
    // can participate in overlapping recovery requests.
    bool   m_bodyNoCollideActive;
    bool   m_bodyNoCollideAttempted;
    bool   m_bodyNoCollideDoorway;
    bool   m_bodyNoCollideLadder;
    bool   m_bodyNoCollideEscalating;
    float  m_bodyNoCollideElapsed;
    float  m_bodyNoCollideDuration;
    float  m_bodyNoCollideBestDist;
    float  m_bodyNoCollideNextRetry;
    float  m_bodyNoCollideAbandonRemaining;
    Vector m_bodyNoCollideTarget;
#ifdef _LINUX
    unsigned long long m_bodyNoCollideParticipants;
#else
    unsigned long long m_bodyNoCollideParticipants;
#endif
    bool   m_bodyNoCollideRebasePending;
    bool   m_bodyNoCollideAbandoned;
    int    m_bodyNoCollideLastFailure;
    int    m_bodyNoCollideNoSafeFailures;

    // objective hold: distinct spread spot so bots don't clump on the center
    Vector m_holdSpot;
    bool   m_haveHoldSpot;
    bool   m_patrolling;       // this hold: wander inside the point vs stand one spot
    float  m_nextPatrol;       // next time a patrolling bot repicks a spot in the point

    // Phase 3: Overwatch & Sniper AI state
    Vector m_sniperSpot;       // coordinates of assigned tactical overlook
    bool   m_haveSniperSpot;   // currently stationed or en route to overlook
    float  m_sniperRelocateTime; // threat cooldown to abandon current nest (shoot & scoot)
    int    m_sniperKills;      // kills achieved from current vantage point
    int    m_sniperArea;       // NAV area that owns the selected perch
    int    m_sniperObj;        // objective this assignment was scored for
    int    m_sniperTeam;       // team that owns the assignment
    unsigned char m_sniperStance; // validated standing/crouched firing origin
    float  m_sniperScore;
    float  m_sniperAssignedTime;
    Vector m_sniperAimTarget;  // best validated objective/approach ray
    bool   m_sniperHearingDetour; // left a held perch for a bounded footstep search
    float  m_sniperHearingReturnTime; // hard deadline; audible footsteps cannot extend it
    float  m_sniperHoldUntil;  // isolated perch tenure timer (45-75s)

    // hearing / footstep investigation
    Vector m_hearTarget;       // last known origin of heard enemy movement
    float  m_hearUntil;        // keep investigating the origin until this time
    float  m_nextHearTime;     // cooldown before listening for new footsteps
    float  m_doorYieldUntil;   // hold position at a door/choke while a teammate clears it
    int    m_doorYieldSlot;    // roster slot + 1 that began this door courtesy cycle; 0 = none
    Vector m_chokeProgressPos; // last progress sample while leading a narrow passage
    float  m_chokeProgressTime;
    float  m_chokeBackoffUntil;// briefly reverse when the passage leader cannot advance

    // corridor streaming (v0.19.4): sticky lanes, committed passing, smooth
    // follow speed, and throttled geometry probes
    int    m_laneIndex;         // sticky lane index across portals (0..2)
    int    m_laneClass;         // last portal width class: 0 single, 1 two-lane, 2 three-lane
    Vector m_portalAxis;        // current portal spread axis (set by AdvanceWaypoint)
    float  m_portalHalfSpan;    // current portal opening half-width
    bool   m_havePortalSpan;    // true when the current waypoint is a real portal
    float  m_passSide;          // committed pass direction (+1/-1), 0 = none
    float  m_passUntil;         // pass commitment expiry
    float  m_followSpeed;       // smoothed follow speed for streaming
    int    m_queueLeaderSlot;   // current predecessor slot + 1 (watchdog identity)
    float  m_queueWaitSince;    // continuous no-progress queue wait start
    Vector m_queueProgressPos;  // last follower position that reset the watchdog
    float  m_queueBreakUntil;   // temporarily ignore queue leaders so stuck recovery can run
    float  m_routeRemaining;    // scalar remaining path length; queue edges must decrease it
    float  m_dropHoldSince;     // when the current drop-landing hold started (0 = none)
    bool   m_blocked;           // leader state: blocked at a choke with no lane predecessor
    bool   m_chokeBackedOut;    // backout already triggered for the current stall
    float  m_chokeCacheTime;    // next refresh time for door/choke/low-obstacle probes
    float  m_chokeCacheYaw;     // yaw the cache was computed at
    bool   m_doorCached;
    bool   m_chokeCached;
    bool   m_lowObstacleCached;
    DoorwayRunwayState m_doorwayRunway;
    float  m_doorwayApexBrakingUntil;
    // corridor-approach prediction: the path ahead is about to enter a tight
    // corridor (a contiguous run of narrow portals/areas), so the column
    // merges to single-file and stops passing before the mouth
    bool   m_corridorAhead;
    bool   m_corridorLatched;   // stay streaming through the final narrow edge/exit
    // per-frame current-area cache (FindArea + off-mesh fallback is the
    // dominant per-frame cost on 25k-area maps; exact same-frame reuse)
    mutable int    m_areaCache;
    mutable Vector m_areaCachePos;
    mutable float  m_areaCacheTime;
    float  m_corridorAheadDist;  // distance from bot to the first narrow portal
    float  m_corridorScanTime;   // throttle for the corridor lookahead (~7 Hz)
    // elevation-drop ahead: the current waypoint sits notably below the
    // bot's feet (steps/drops), so followers must hold at the edge until the
    // landing is clear instead of walking off onto a teammate below
    bool   m_dropAhead;
    float  m_dropAheadDist;
    bool   m_dropHoldExpired;  // true after 2s drop-hold expires; prevents 1-tick re-arm oscillation

    // cover: a spot that breaks line-of-sight to the current threat
    Vector m_coverSpot;
    bool   m_haveCover;
    float  m_coverUntil;       // re-evaluate cover choice after this time

    // weapons
    const void *m_wp;          // cached WeaponProfile* for the held weapon
    float  m_wpNextCheck;      // when to re-read the weapon name
    char   m_wpName[64];       // classname of held weapon
    bool   m_wpToggledMode;    // sent +firemode for this weapon
    int    m_fireMode;         // chosen fire mode this engagement (FM_*)
    int    m_burstShots;       // rounds pressed in the current burst
    float  m_recoilRise;       // accumulated uncompensated recoil climb (deg)
    float  m_crouchFireUntil;  // hold crouch through this burst (not a 1-frame blip)
    float  m_fireAccum;        // accumulated continuous-fire time since last reload
    int    m_roundsFired[INSBOT_COMBAT_CLASS_COUNT]; // actual rounds fired from current clip per combat class
    char   m_primaryWeaponName[64]; // remembered primary weapon classname for sidearm restoration
    int    m_reloadingClass;   // combat class currently reloading (-1 = none)
    int    m_lastEngineClip;   // last raw engine clip value (for detecting refills)
    float  m_nextShotTime;     // timer for next round in spray
    int    m_reloadMags[INSBOT_COMBAT_CLASS_COUNT]; // reserve magazines consumed per weapon class this life
    bool   m_onSidearm;        // fell back to pistol
    bool   m_sidearmTemporary; // drew pistol for close contact (<200u); reversible when enemy retreats
    float  m_reloadUntil;      // currently reloading until this time
    float  m_launcherPostFireUntil; // keep RPG launch isolated from reload/switch commands
    float  m_rpgSpeedDumpUntil; // next-frame window for diagnostic missile-speed read
    Vector m_rpgSpeedDumpMuzzle;
    int    m_rpgSpeedDumpEntIndex;
    Vector m_rpgSpeedDumpOrigin;
    float  m_rpgSpeedDumpTime;
    float  m_nextGrenade;      // earliest time we may throw next grenade
    int    m_nadeState;        // 0 none, 1 switch-to, 2 throw, 3 exact restore, 4 lastinv fallback
    float  m_nadeTimer;
    Vector m_nadeAim;
    float  m_nadeYaw, m_nadePitch;   // computed ballistic launch angles
    int    m_nadeKind;         // 0 = frag (thrown at enemy), 1 = smoke (onto objective)
    float  m_nextSmoke;        // earliest time we may throw a smoke
    char   m_preNadeWeapon[64];// weapon held before any grenade attempt; restored on finish/abort
    float  m_nextM203;         // earliest time we may fire next M203 grenade
    float  m_nextRpg;          // earliest time we may fire next RPG-7 rocket
    // ---- Squad Escort & Fireteam AI (v0.20.0) ----
    int    m_squadLeaderIndex; // Entity index of human squad leader (0 = none / autonomous)
    int    m_squadOrder;       // Current squad order (1=FOLLOW, 2=HOLD, 3=BACKUP, 4=SUPPRESS)
    int    m_squadSlot;        // Formation slot index (0 = Left Wing, 1 = Right Wing, 2 = Rear Security)
    float  m_squadOrderTime;   // Timestamp of last received order
    Vector m_squadHoldPos;     // Position for perimeter defense when order is HOLD
    float  m_squadScanYaw;     // Outward facing yaw for perimeter defense
    Vector m_lastLeaderPos;    // Previous known leader origin for velocity tracking
    float  m_lastLeaderTime;   // Timestamp of last leader position sample
    Vector m_lastLeaderMoveDir; // Normalized XY movement dir (anti-boxing path check)
    float  m_squadFormationYaw;// Motion-damped formation orientation heading
    float  m_lastSquadTime;    // Timestamp of last squad follow think
    int    m_squadRespawnPendingForLeader; // leaderIndex or 0 = none
    int    m_squadRespawnSlot;             // original slot 0..2
    float  m_deadLeaderFallbackSince;      // 0 = inactive fallback session
    Vector m_deadLeaderCorpsePos;
    bool   m_leaderInCover;               // true when leader detected crouched/proned + stationary
    bool   m_coverFollowSet;              // tick-local: UpdateSquadFollow took cover (Fix 4 handoff)
    bool   m_sidestepActive;              // tick-local: anti-boxing sidestep produced movement this tick
    bool   m_suppressFire;                // tick-local: covering-fire cadence drives IN_ATTACK (L-2)
    float  m_nextSuppressShot;            // next allowed covering-fire shot time (weapon cadence)
    bool   m_suppressLosClear;            // last eye->marker occlusion probe result (covering fire)
    float  m_nextSuppressLosCheck;        // next probe time (0 = immediate)
    Vector m_suppressReposPoint;          // current reposition goal while marker is occluded
    bool   m_haveSuppressRepos;           // reposition goal valid
    float  m_nextSuppressRepos;           // throttle for reposition re-selection
    float  m_leaderCoverCheckTime;        // throttle for leader-cover detection (1 Hz)
    int    m_m203State;        // 0=idle, 1=raising sight, 2=firing, 3=lowering sight
    float  m_m203Timer;
    Vector m_m203Aim;
    float  m_m203Yaw, m_m203Pitch;
    bool   m_m203Pulse;

    // misc
    float  m_nextUseTime;      // next time we may check-for/press +use (doors)
    float  m_nextReloadTime;
    QAngle m_curAngles;
    float  m_navTurnAngle;    // remaining non-combat navigation turn in degrees
    float  m_spawnTime;
    int    m_spawnStandTicks; // stand-up jump pulse on spawn to break spawn-prone stance
int    m_lastHealth;       // health last frame, for "I'm hit"/"need backup" callouts
float  m_suppressedUntil;   // >0 while reacting to recent damage (suppression response)
int    m_chosenCoverTier;   // tier of last accepted cover choice (v2 only)
bool   m_chosenCoverIsLow;  // true if accepted cover is low-only (crate/sandbag)

    // internals
    void  ThinkJoin();
    void  PruneDuplicateWeapons();
    void  ThinkAlive();
    // ThinkAlive phases. These touch no ThinkAlive locals except `now`, so they
    // were lifted out verbatim to keep the 2.4k-line body readable.
    void  ThinkDeadReset(float now);
    void  ThinkWeaponAndAmmo(float now);
    void  RollCustomizationForLife();
    void  UpdateCustomizationWeapon(int roleId);
    bool  SendCustomizationBeforeSpawn();
    bool  IsHoldingM203() const;
    bool  IsHoldingRPG() const;
    bool  CanRPGFire() const;
    bool  CanM203Fire() const;
    void  UpdateSquadFollow(float now, float &fwd, float &side, int &buttons, QAngle &view);
    bool  IsSquadFollower() const { return m_squadLeaderIndex > 0; }
    void  SetSquadAssignment(int leaderIndex, int slot, int order);
    void  ClearSquadAssignment();
    void  StartM203(float now, const Vector &target);
    void  UpdateM203(float now, int &buttons, QAngle &view);
    void  MaybeM203Objective(float now, const ObjPoint *op, float dist, int myTeam);
    void  RunMove(float fwd, float side, int buttons, const QAngle &view);
    bool  FireLauncherDirect(char *playerBase, const QAngle &view);
    void  DumpPendingRpgSpeed(float now);
    bool  WriteCollisionGroup(int collisionGroup);
    void  BeginSpawnNoCollide(const Vector &pos);
    void  UpdateSpawnNoCollide(const Vector &pos, bool countTime);
    void  EndSpawnNoCollide(bool restoreGroup);
    bool  SpawnHullOverlapsPlayer(const Vector &pos) const;
    void  ResetSpawnNoCollideNavigation(const Vector &pos);
    bool  FindBodyBlocker(const Vector &pos, const Vector &target,
                          int &blockerClient) const;
    bool  BeginBodyNoCollide(const Vector &pos, const Vector &target,
                             bool doorwayRecovery, float duration,
                             bool ladderRecovery = false,
                             int confirmedBlocker = 0);
    void  ExpandBodyNoCollideCohort(const Vector &pos);
    bool  UpdateBodyNoCollide(const Vector &pos, float now);
    void  EndBodyNoCollide(bool succeeded);
    void  CompleteBodyNoCollideTeleport(const char *source);
    void  ResetBodyNoCollideRecovery();
    void  AbandonStuckObjective(const Vector &pos, float now);
    bool  HasBodyNoCollide() const;
    void  ResetLadderTraversal(bool releaseOwner = true);
    Vector LadderQueueStandPoint(const Vector &lip, int rank) const;
    bool  ConfirmLadderContact(const Vector &pos, float nx, float ny) const;
    int   FindLadderBlocker(const Vector &pos, const Vector &target,
                            int ladderIdx) const;
    bool  EnemyNearLadder(const struct NavLadderData *ladder) const;
    bool  IsVisible(const Vector &from, const Vector &to, edict_t *target) const;
    bool  FindVisibleTargetPoint(edict_t *target, const Vector &origin,
                                 Vector &out) const;
    bool  FindShootableTargetPoint(edict_t *target, const Vector &origin,
                                   bool crouched, Vector &out) const;
    bool  CanShootNow(edict_t *enemy, const Vector &targetPt,
                      bool crouched = false) const;
    void  ValidateCrouchedFire(int &buttons, float now);
    bool  LineGoesThroughSmoke(const Vector &from, const Vector &to) const;
    bool  IsTeammateInLineOfFire(const Vector &from, const Vector &to) const;
    Vector EyePos() const;
    Vector FireEyePos(bool crouched) const;
    int   FindEnemy();
    bool  ValidEnemy(int idx) const;   // strict per-frame target validation

    // Anti-vehicle channel. See the members above for why this is separate.
    bool  ValidVehicleEnemy(int idx) const;
    int   FindVehicleEnemy();
    bool  BuildVehicleTargetPoints(edict_t *vehicle, Vector *points,
                                   int maxPoints) const;
    bool  FindShootableVehiclePoint(edict_t *vehicle, bool crouched,
                                    Vector &out) const;
    void  UpdatePathTo(const Vector &goal, bool force);
    bool  AdvanceWaypoint();
    void  RefreshRouteRank();  // coherent post-think snapshot for queue ordering
    bool  TraverseLadder(int fromArea, int toArea, float &fwd, float &side,
                         int &buttons, QAngle &view, Vector &navTarget);
    void  SteerRoam(float &fwd, float &side, int &buttons, QAngle &view);
    void  SteerToPoint(float destYaw, float speed, float &fwd, float &side,
                       int &buttons, QAngle &view); // wall-aware no-path fallback
    bool  UpdateHearing(float now, const Vector &pos, float &fwd, float &side, int &buttons, QAngle &view);
    bool  ScanHearableEnemy(const Vector &pos, Vector &outPos);
    void  SendJoinCommands();
    const char *SidearmName() const;
    const char *GrenadeName() const;
    bool  HasWeapon(const char *weaponClass) const;
    // Anti-vehicle trigger policy. Reads insbot_target_vehicles and the held
    // weapon: only explosives (RPG-7, M203) bypass the 0.05 hull armor scale,
    // so small arms are not worth the magazine.
    bool  VehicleEngageWeaponAllowed() const;
    void  StartGrenade(float now, const Vector &target);
    void  StartSmoke(float now, const Vector &target);   // smoke throw (verifies possession)
    void  MaybeSmokeObjective(float now, const struct ObjPoint *op, float dist, int myTeam);
    void  MaybeSmokeEnemyGroup(float now, int myTeam);
    void  UpdateGrenade(float now, const Vector &target, int &buttons, QAngle &view);
    bool  SwitchToWeapon(const char *weaponClass, bool force = true);
    void  RestoreGrenadeWeapon();
    bool  LowObstacleAhead(float yawDeg) const;   // curb/crate in front -> jump
    bool  LowCeilingAhead(float yawDeg) const;    // overhang at head height -> duck
    bool  CeilingLowAt(const Vector &p) const;    // standing headroom missing at p
    bool  DoorAhead(float yawDeg, Vector *doorOut = 0) const; // closed door in front -> press +use
    bool  ChokeAhead(float yawDeg, Vector *chokeOut = 0) const;   // robust doorway/archway/chokepoint detection
    bool  SourceTurnForEdge(int fromArea, int toArea, const Vector &pos) const; // automatic only at physical chokes
    void  ResetDoorwayRunway();
    bool  ProbeUpcomingDoorwayRunway(DoorwayRunwayState &outRunway);
    bool  VerifyDoorwayHullClearance(DoorwayRunwayState &runway);
    bool  TeammateInDoorwayCorridor(const DoorwayRunwayState &runway, int mySlot) const;
    bool  SafeStandSpot(const Vector &dest, float minPlayerSep = 56.0f) const; // room to stand at dest (teleport safety)
    bool  FindSafeTeleportSpot(int fromArea, int destArea, const Vector &pos,
                               Vector &dest) const;
    bool  SideStepOpen(const Vector &probe) const; // open side step: clear waist path + walkable landing
    bool  ClearBehind(float moveYaw, float minGap = 60.0f) const; // room to back up along -moveYaw
    bool  TeammateBehind(float yawDeg, float range = 160.0f, float lateralRange = 72.0f) const;
    bool  TeammateAtLanding() const; // same-team bot near the current (drop) waypoint below
    bool  TeleportUnstuck(const Vector &pos, float now,
                          bool doorwayRecovery = false,
                          int *failureReason = 0,
                          bool expandForward = false); // full-hull warp past a bad link
    void  OnRelocated(const char *reason); // unified state reset after any teleport
    bool  TransferHadithaDoor(float now); // semaphore owner: safe hop over verified 464->3321 gap
    bool  TransferHadithaEastDoor(float now); // stalled leader: safe hop from id3295/3315 into id320
    bool  TeammateBlockingPath(const Vector &pos, int toArea) const;
    // vehicle-avoidance steering (bot_steer.cpp)
    bool  SteerAroundVehicle(float moveYaw, float viewYaw,
                             float &fwd, float &side, int &buttons);
    // vehicle blocking test for stuck-link reporting (bot_path.cpp)
    bool  VehicleBlockingRoute(const Vector &pos, float moveYaw) const;
    bool  WorldClear(const Vector &a, const Vector &b) const; // world-only LOS between 2 points
    bool  CanShootMarker(const Vector &marker) const;         // covering-fire probe: eye->marker world trace
    bool  FindCover(const Vector &threat, Vector &out) const; // nearby spot hidden from threat
    bool  IsPositionExposed(const Vector &myPos, const Vector &threatPos) const;
float EvaluateCover(const CoverPoint &cp, const Vector &threatPos) const;
bool  ChooseCoverSpot(const Vector &primaryThreat, Vector &outSpot, int &outTier);
    void  ApplyCrowdSteering(float moveYaw, float viewYaw, float &fwd, float &side,
                             int &buttons); // shared nav/combat spacing + choke queue
    void  ApplyCrowdSteeringClassic(float moveYaw, float viewYaw, float &fwd,
                                    float &side, int &buttons);
    void  MoveToward(const Vector &dest, float &fwd, float &side, int &buttons, QAngle &view);
    void  Say(int ev);         // emit a radio callout (VoiceEvent) for this bot
    float Now() const;
};

// Cell size of the cover spatial index. Any size is CORRECT - the query
// derives its cell range from the probe disc itself - so this is purely a
// tuning value. Twice the 75u probe radius keeps the visited set small
// without producing a long per-cell list.
#define COVER_GRID_CELL 150.0f

class CBotRoster
{
public:
    CBotRoster() : m_navLoadedForMap(false)
    {
        m_mapname[0] = 0;
        // The cover database and its index are normally zero-initialised by
        // being a static global, but initialise them explicitly: FreeCoverGrid
        // and FreeCoverDatabase run on the first BuildCoverDatabase and must
        // not read garbage.
        m_coverPoints = 0;
        m_coverCount = 0;
        m_coverCapacity = 0;
        m_coverOwnerSlot = 0;
        m_coverOwnedSince = 0;
        m_coverGridW = m_coverGridH = 0;
        m_coverGridMinX = m_coverGridMinY = 0.0f;
        m_coverGridStart = 0;
        m_coverGridCells = 0;
        // Test-only override for the cover-point capacity (see Task 4). Zero
        // means "derive the capacity from the nav mesh" (the production path).
        m_coverCapacityOverride = 0;
        m_captureUpdateTime[0] = m_captureUpdateTime[1] = m_captureUpdateTime[2] = -999.0f;
        m_captureRefillTime[0] = m_captureRefillTime[1] = m_captureRefillTime[2] = 0.0f;
        m_captureTarget[0] = m_captureTarget[1] = m_captureTarget[2] = -1;
        m_hadithaDoorOwner = 0;
        m_hadithaDoorOwnerSince = 0.0f;
        m_hadithaDoorProgressPos = Vector(0, 0, 0);
        m_hadithaDoorProgressTime = 0.0f;
        m_hadithaDoorClearUntil = 0.0f;
        m_hadithaDoorCommitted = false;
        for (int i = 0; i < 10000; i++)
        {
            m_ladderQueues[i].ownerSlot = 0;
            m_ladderQueues[i].lastDirection = 0;
        }
        m_sniperCacheTeamCursor = 1;
        m_sniperCacheObjCursor = 0;
        ResetSniperCaches();
        memset(m_leaderActive, 0, sizeof(m_leaderActive));
        memset(m_leaderSquadOrder, 0, sizeof(m_leaderSquadOrder));
        ResetPlayerSquads();
        for (int i = 0; i < 32; i++)
        {
            m_reservations[i].inUse = false;
            m_reservations[i].teamSlot = -1;
            m_reservations[i].roleId = 0;
            m_reservations[i].userId = 0;
            m_reservations[i].reserveTime = 0.0f;
        }
    }

    void  AssignPlayerSquad(int leaderIndex);
    void  RefillPlayerSquad(int leaderIndex);
    void  DispatchSquadOrder(int leaderIndex, int order, float wpX = 0.0f, float wpY = 0.0f, float wpZ = 0.0f);
    void  UpdateSquadRoster(float now);
    // Original-membership table: tracks which bot originally filled each
    // fireteam slot per leader, surviving follower death + respawn.
    // Identity matched via roster index (primary) and bot name (secondary).
    int   m_leaderMemberBotIdx[65][3];
    char  m_leaderMemberName[65][3][64];
    float m_leaderMemberDeathTime[65][3];   // 0 = alive/no pending death
    void  StashPendingRespawn(CInsBot *bot);
    void  RecallFollower(CInsBot *bot);
    void  ResetPlayerSquads();
    bool  m_leaderActive[65];
    int   m_leaderSquadOrder[65];
    Vector m_leaderWaypoint[65];     // covering-fire marker position

    // Squad-wide active threat sharing (Crossfire & Sector Assist)
    int    m_leaderSquadThreatEnemy[65];     // Active enemy index shared across fireteam
    Vector m_leaderSquadThreatPos[65];       // Position of the active shared threat
    float  m_leaderSquadThreatTime[65];      // Timestamp of last detection/engagement

    void   ReportSquadThreat(int leaderIndex, int enemyIdx, const Vector &pos, float now);
    int    GetSquadThreat(int leaderIndex, float now, float maxAge, Vector &outPos) const;
    bool   IsSquadContact(int leaderIndex, int enemyIdx, float now) const;
    void   UpdateSquadLeaderThreats(float now);
    CInsBot *AddBot(const char *nameOrNull);
    CInsBot *AddBotToRole(int faction, int roleId);
    void     KickAll();
    bool     KickOne();
    void     FrameUpdate(bool simulating);
    void     AdoptOrphans();
    void     LevelInit(const char *mapname);
    void     LevelShutdown();
    int      Count() const;
    int      CountTeam(int team) const;
    int      CountRequestedRoles(int teamSlot, const int *roleIds, int roleCount) const;
    // Reserve 15% of each active bot team (minimum one) for the next point
    // that still needs capturing. Selection is refreshed periodically from
    // the alive bots nearest to a hostile/neutral objective (see
    // UpdateCaptureCommitments), not by join order.
    void     UpdateCaptureCommitments(float now);
    void     UpdateDangerZones(float now); // rebuild per-frame firefight zones used by committed-bot routing
    bool     IsCaptureCommitment(const CInsBot *bot, int team) const;
    // Force the next UpdateCaptureCommitments to re-pick this team's active
    // capture target immediately (used when a committed bot sees its point
    // got captured, so the detail re-targets without waiting up to 1s).
    void     MarkCaptureTargetDirty(int team)
    {
        if (team >= 1 && team <= 2) m_captureUpdateTime[team] = -999.0f;
    }
    CInsBot *FindByEdict(edict_t *e);
    bool     CanAcquireBodyGracePlayer(int clientIndex, int ownerSlot);
    bool     AcquireBodyGracePlayer(int clientIndex, int ownerSlot);
#ifdef _LINUX
    void     ReleaseBodyGraceOwner(int ownerSlot,
                                   unsigned long long participants);
#else
    void     ReleaseBodyGraceOwner(int ownerSlot,
                                   unsigned long long participants);
#endif
    bool     HasBodyGracePlayer(edict_t *e) const;
    void     UpdateBodyGracePlayers(bool countTime);
    void     ResetBodyGracePlayers(bool restoreGroups);
    void     BodyGraceClientDisconnect(edict_t *e);
    bool     ClaimLadder(CInsBot *bot, int ladderIdx, bool up,
                         const Vector &pos);
    bool     IsLadderOwner(const CInsBot *bot, int ladderIdx) const;
    void     ReleaseLadder(CInsBot *bot, int ladderIdx, bool completed);
    void     ResetLadderQueues();
    bool     ControlHadithaDoor(CInsBot *bot, const Vector &pos, float now,
                                float viewYaw, float &fwd, float &side);
    void     ResetHadithaDoorGate();
    void     InvalidateAdvanceTargets();
    void     InvalidateNavMeshState();
    void     ResetSniperCaches();
    void     RequestSniperCache(int team, int obj);
    void     UpdateSniperCaches(float now);
    bool     AssignSniperPerch(CInsBot *bot, int team, int obj, float now);

// ---- Cover Point Database (take-cover overhaul v2) ----
void     BuildCoverDatabase();
void     FreeCoverDatabase();
int      CoverCount() const { return m_coverCount; }
const CoverPoint *CoverAt(int i) const { return (i >= 0 && i < m_coverCount) ? &m_coverPoints[i] : 0; }
bool     IsCoverOccupiedByOther(int coverIdx, int myRosterIdx) const;
    void     ReleaseCoverOwnership(int rosterIdx);
int      CoverOwnerSlot(int coverIdx) const { return (coverIdx >= 0 && coverIdx < m_coverCapacity) ? m_coverOwnerSlot[coverIdx] : -1; }
struct CoverPoint *m_coverPoints;
    int      m_coverCount;
    int      m_coverCapacity;
    // Test-only override for the cover-point capacity. BuildCoverDatabase
    // normally sizes the table at areaTotal * 5, which is exactly the largest
    // possible sample count and therefore unreachable. Setting this to a
    // smaller positive value lets a test driver exercise the capacity guard.
    // Zero means "derive the capacity from the nav mesh" (production path).
    int      m_coverCapacityOverride;
    short   *m_coverOwnerSlot;       // roster slot or -1 per cover point (allocated lazily)
float   *m_coverOwnedSince;

    // ---- Cover spatial index ------------------------------------------------
    // Uniform grid over cover-point positions. IsPositionExposed visits only
    // the cells its 75u probe disc overlaps instead of scanning every point.
    // Built by BuildCoverGrid (called from BuildCoverDatabase), freed by
    // FreeCoverGrid. m_coverGridCells == 0 means "no index"; consumers fall
    // back to a linear scan.
    int    m_coverGridW, m_coverGridH;
    float  m_coverGridMinX, m_coverGridMinY;
    int   *m_coverGridStart;   // W*H+1 prefix offsets into m_coverGridCells
    int   *m_coverGridCells;   // cover-point indices, grouped by cell
    void   BuildCoverGrid();
    void   FreeCoverGrid();
    int CoverGridCellX(float x) const
    {
        int c = (int)((x - m_coverGridMinX) / COVER_GRID_CELL);
        if (c < 0) c = 0;
        if (c >= m_coverGridW) c = m_coverGridW - 1;
        return c;
    }
    int CoverGridCellY(float y) const
    {
        int c = (int)((y - m_coverGridMinY) / COVER_GRID_CELL);
        if (c < 0) c = 0;
        if (c >= m_coverGridH) c = m_coverGridH - 1;
        return c;
    }

    CInsBot  m_bots[64];
    char     m_mapname[128];
    bool     m_navLoadedForMap;

    // ---- Slot-request reservations (Option B) ----
    // A human took a bot/slot in the F2 menu and is waiting for their next
    // natural death to claim it. The roster must not re-fill that slot code
    // while the reservation is live.
    struct RoleReservation
    {
        bool  inUse;
        int   teamSlot;
        int   roleId;      // pfullsetup slot code: 5 + squadIndex + slot*4
        int   userId;      // SourceMod GetClientUserId() of the requester (0 if unknown)
        float reserveTime; // gpGlobals->curtime at reservation; 0 means "no expiry"
    };
    RoleReservation m_reservations[32];

    void ReserveRole(int teamSlot, int roleId, int userId);
    void UnreserveRole(int teamSlot, int roleId);
    void UnreserveUser(int userId);
    void ClearAllReservations();
    void SweepReservations();
    bool IsRoleReserved(int teamSlot, int roleId) const;
    int  GetReservationCount(int teamSlot = -1) const;
    bool IsRoleInUse(int teamSlot, int roleId) const;

    float    m_captureUpdateTime[3];  // per-team throttle for target refresh (1s)
    float    m_captureRefillTime[3];  // per-team throttle for refilling slots (0.25s)
    int      m_captureTarget[3];      // per-team cached active capture objective
    int      m_hadithaDoorOwner;      // roster slot + 1; one bot owns the physical throat
    float    m_hadithaDoorOwnerSince; // acquisition time for stalled-owner recovery
    Vector   m_hadithaDoorProgressPos;// independent physical-progress sample for the owner
    float    m_hadithaDoorProgressTime;// continuous owner no-progress start
    float    m_hadithaDoorClearUntil; // clearance interval before handing off
    bool     m_hadithaDoorCommitted;  // owner reached staging line and began straight commit
    LadderQueueState m_ladderQueues[10000];
    SniperObjectiveCache m_sniperCache[3][INSBOT_SNIPER_OBJECTIVES];
    int      m_sniperCacheTeamCursor;
    int      m_sniperCacheObjCursor;

private:
    void     BuildSniperCacheCandidates(int team, int obj, SniperObjectiveCache &cache);
    bool     EvaluateSniperCandidate(int team, int obj,
                                     const SniperPendingCandidate &candidate,
                                     SniperPerch &out);
    bool     IsSniperPerchReserved(const CInsBot *requester, int team,
                                   const Vector &pos) const;
};

extern void UpdateSmokeGrenades();

// Smoke occlusion, exposed so the Humvee auto-turret applies the SAME rule the
// bots do (bot_perceive.cpp:424) instead of growing a second copy of the 140u
// threshold. One definition, two consumers.
extern bool LineGoesThroughSmoke(const Vector &from, const Vector &to);

extern CBotRoster g_Bots;
extern int s_offset_flNextAttack;
extern int s_offset_m_IronsightsState;
extern int s_offset_angEyeAngles0;
extern int s_offset_angEyeAngles1;

// Humvee auto-turret offsets, resolved by name in insbots.cpp Load().
extern int g_TurretOffsetDriver;
extern int g_TurretOffsetPoseParameter;
// m_flAnimTime is a vehicle-layout fact, not a turret fact: the sweep that
// advances it runs on every driveable, turret or not. See DriveAnimTime().
extern int g_TurretOffsetAnimTime;

// learned-bad-link persistence (implemented in bot.cpp)
void InsBots_SaveLearn();
void InsBots_ResetPhysicalPortalCache();
void InsBots_ForgetLearn();

// per-map faction of each team number, read from the map's .imc2 config (the
// TEAM_USMC/TEAM_IRAQI assignment to TeamOne/TeamTwo FLIPS between maps, so it
// must be read per map, not assumed). Index 1 = team 1, 2 = team 2.
enum { FACTION_UNKNOWN = 0, FACTION_USMC = 1, FACTION_IRAQI = 2 };
extern int g_teamFaction[3];
void InsBots_InitWeaponCvars();
void InsBots_LoadMapFactions(const char *gamedir, const char *mapname);

// Forced pathfinding override coordinates
extern bool g_ForceGotoActive;
extern Vector g_ForceGotoPos;
void InsBots_SetGotoCmd(edict_t *issuer, int argc, const char *arg1 = 0, const char *arg2 = 0, const char *arg3 = 0);
