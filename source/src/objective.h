// ============================================================================
// objective.h - INS:MIC objective awareness for bots
//
// Objectives are `ins_objective` brush entities carrying a mapid; the .imc2
// map config marks some as spawn zones (hidden) and the rest as neutral
// capturable points. We don't read keyvalues at runtime - instead:
//   * objective world positions come from each entity's collision bounds
//   * each team's "home" is learned from where its own bots spawn
//   * an objective sitting on top of a team's home is treated as that team's
//     spawn zone and skipped as a goal; the rest are contest points
// Bots then push from their home through the contest points and hold them.
// ============================================================================
#pragma once

#include "vector.h"

#define INSOBJ_MAX 24

// PickGoal sentinel: no objective left to capture or hold - the bot should
// push into enemy territory and hunt instead of camping a friendly point.
#define PICK_ADVANCE (-2)

struct ObjPoint
{
    Vector center;
    float  radius;      // effective hold radius derived from brush bounds
    float  minX, maxX;
    float  minY, maxY;
    float  minZ, maxZ;  // exact objective brush bounds
    int    edictIndex;
    bool   isSpawn;     // a spawn zone (has player spawns on it) - never a goal
    bool   isEnclosed;  // indoors/basement: solid ceiling overhead (skip high-arc shelling)
    char   phonetic;    // HUD letter from the .imc2 (A/B/C...), 0 if unknown
    int    orderId;     // m_iOrderID: index within the active profile's
                        // ObjectiveData block; -1 when unreadable, and the
                        // engine's own sentinel (255/-1) when the profile does
                        // not use this objective. Test with InsOrderIdValid().
};

class CObjectives
{
public:
    CObjectives() { Reset(); }

    void Reset();
    void ScanEntities();                 // find ins_objective entities (idempotent)
    bool IsReady() const { return m_count > 0 && (m_homeSamples[1] > 0 || m_homeSamples[2] > 0); }
    int  Count() const { return m_count; }
    const ObjPoint *Get(int i) const { return (i >= 0 && i < m_count) ? &m_objs[i] : 0; }

    // record a fresh spawn position for a team (1 or 2) to learn its home
    void NoteSpawn(int team, const Vector &pos);

    // stable attacker/defender role split: the aggression value sets what
    // FRACTION of the team attacks (1 = all attack, 0 = all defend), and each
    // bot's role is a stable hash of its ordinal so roles don't flap.
    // (free function so bot.cpp uses the exact same split)
    // -- declared below the class as InsRoleAttacker() --

    // choose an objective for a team-T bot currently at botPos.
    //  aggression: 0 = defend (hold objectives near own spawn), 1 = attack
    //              (push objectives near enemy spawn), 0.5 = balanced/middle.
    //  spread:     bot ordinal for team-spread (different bots favour different
    //              points); pass -1 to disable spreading.
    //  avoid:      an objective index to skip (a bot couldn't reach it), or -1.
    // returns objective index, or -1 if none suitable.
    int  PickGoal(int team, const Vector &botPos, int curGoal, float aggression, int spread, int avoid,
                  bool forceCapture = false) const;

    // is this objective essentially one team's spawn zone? (skip as a goal)
    bool IsSpawnZone(int objIdx, int team) const;

    // Reachable world position to push toward when there is nothing left to
    // capture: normally the enemy team home (where they spawn), projected
    // onto the closest NAV area reachable through outgoing links from from.
    // Returns false when the home is not learned or no reachable NAV proxy
    // exists; never promises the raw enemy-spawn coordinate.
    bool PushTarget(int team, const Vector &from, Vector &out) const;

    // live ownership of an objective from the game (m_iCapturedTeam):
    //   0 = neutral (grey), 1 = team 1, 2 = team 2, -1 = unknown/unreadable
    int  GetOwnerTeam(int objIdx) const;

    // relation to a given team: 0 = neutral, +1 = friendly (own), -1 = hostile
    //   -2 = unreadable / unknown (failsafe: do not treat as active capture target)
    int  Relation(int objIdx, int team) const;

    // checks if there is any hostile or neutral capturable objective on the map for team
    bool HasHostileOrNeutral(int team) const;

    // is this objective hidden (a spawn zone / non-capturable marker)?
    //   1 = hidden, 0 = visible/capturable, -1 = unknown
    int  GetHidden(int objIdx) const;

    // diagnostic: dump candidate ownership fields for every objective so the user
    // can match them against the HUD and tell us the right offset
    void DebugDump() const;

    // read the HUD phonetic letters (A/B/C...) from the map's .imc2 + .bsp so the
    // debug/overlay can show letters instead of scan indices. Call before scan.
    void LoadLetters(const char *gamedir, const char *mapname);
    char LetterFor(const Vector &center) const;   // nearest loaded letter, or 0

    // Read the map's default gametype from its .imc2 so sequential modes can
    // order their objectives. Call after Reset() and before ScanEntities().
    void LoadProfile(const char *gamedir, const char *mapname);

    // Re-read m_iOrderID for every objective. The game assigns these during
    // round setup, NOT at level load, so the values cached by ScanEntities()
    // are still -1 until the first round_reset. Call this again whenever a
    // round starts, otherwise every objective reads as "not in the profile"
    // and the ordering rule silently degrades to the legacy selection.
    void RefreshOrderIds();

    ObjPoint m_objs[INSOBJ_MAX];
    int      m_count;

    // gametype of the map's default profile (see LoadProfile)
    int      m_gametype;      // 1 = GAMETYPE_PUSH, 2 = GAMETYPE_FIREFIGHT,
                              // 3 = GAMETYPE_BATTLE, 0 = unknown/unreadable
    bool     m_ordered;       // true only for sequential modes (Push)

    Vector   m_home[3];        // index 1 = team1, 2 = team2
    int      m_homeSamples[3];
    bool     m_scanned;

    // objective letters resolved from map data (center -> letter)
    struct LetterEnt { Vector c; char letter; };
    LetterEnt m_letters[INSOBJ_MAX * 2];
    int       m_letterCount;

private:
    float AxisT(const Vector &p) const;   // 0 at team1 home, 1 at team2 home
};

extern CObjectives g_Objectives;

// stable attacker/defender split by aggression (shared by objective.cpp + bot.cpp)
inline bool InsRoleAttacker(int ordinal, float aggr)
{
    if (aggr <= 0.01f) return false;
    if (aggr >= 0.99f) return true;
    if (ordinal < 0)   return aggr >= 0.5f;          // teamwork off: one shared role
    unsigned int h = ((unsigned int)ordinal * 2654435761u) >> 16;
    return (float)(h % 100) < aggr * 100.0f;
}
