// ============================================================================
// profile.h - optional built-in per-frame profiler for InsBots.
//
// The plugin build compiles with /DINSBOT_PROFILER so these hooks are active.
// Native nav tools (which compile nav.cpp without that define) get no-ops, so
// the profiler never changes behavior or build requirements outside the DLL.
// Enable at runtime with:  insbot set profile 1
// ============================================================================
#pragma once

#ifdef INSBOT_PROFILER

enum InsProfBucket
{
    INSPROF_FRAME = 0,      // whole GameFrame duration (FrameBegin/FrameEnd)
    INSPROF_SMOKE,          // UpdateSmokeGrenades entity sweep
    INSPROF_BUILDPATH,      // A* route searches
    INSPROF_GETPATHCOST,    // route-cost queries (capture / objective picking)
    INSPROF_FINDAREA,       // nav area under a position
    INSPROF_FINDNEAREST,    // nearest nav area fallback
    INSPROF_DOOR,           // DoorAhead trace
    INSPROF_CHOKE,          // ChokeAhead trace
    INSPROF_LOWOBSTACLE,    // LowObstacleAhead trace
    INSPROF_VISION,         // FindEnemy sight scan
    INSPROF_HEARING,        // HearingFrameUpdate global footstep sample
    INSPROF_UPDATEHEARING,  // per-bot hearing investigation
    INSPROF_TEAMSIGHT,      // shared radio sighting / support move
    INSPROF_DANGERZONES,    // firefight zone refresh
    INSPROF_CAPCOMMIT,      // capture detail refill
    INSPROF_THINK,          // per-bot Think
    INSPROF_THINKALIVE,     // per-bot ThinkAlive
    INSPROF_BUCKET_COUNT
};

class CInsProfTimer
{
public:
    CInsProfTimer(int bucket);
    ~CInsProfTimer();
private:
    CInsProfTimer(const CInsProfTimer &);
    CInsProfTimer &operator=(const CInsProfTimer &);
    int      m_bucket;
    unsigned m_startLo;  // QPC tick low/high halves when active (no 64-bit
    unsigned m_startHi;  // arithmetic - this DLL links a custom /nodefaultlib
    bool     m_active;   // CRT that lacks MSVC's __ltod3 long-long helper)
};

bool InsProf_Active();
void InsProf_SetEnabled(bool on);
void InsProf_FrameBegin();
void InsProf_FrameEnd();
void InsProf_Tick();

#define INSPROF_CONCAT_(a, b) a##b
#define INSPROF_CONCAT(a, b) INSPROF_CONCAT_(a, b)
#define INSPROF_SCOPE(b) CInsProfTimer INSPROF_CONCAT(_insprof_, __LINE__)(b)

#else

#define INSPROF_SCOPE(b) do { } while (0)

#endif
