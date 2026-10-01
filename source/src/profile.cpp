// ============================================================================
// profile.cpp - built-in per-frame profiler (opt-in via insbot set profile 1).
//
// Measures whole GameFrame duration plus per-bucket CPU time (A*, nav lookups,
// trace helpers, vision, hearing, teamsight, capture maintenance) with
// QueryPerformanceCounter and prints a 5-second summary to the server console.
// Zero behavior change: profiling only adds timers around existing calls.
//
// NOTE: this DLL links with /nodefaultlib and a hand-rolled msvcrt.lib, which
// does NOT export MSVC's __ltod3 helper (long long -> double). All QPC math is
// therefore done with two 32-bit halves and converted to double manually, so
// the compiler never emits a 64-bit conversion intrinsic.
// ============================================================================
#ifdef _LINUX
#include <stdint.h>
#include <time.h>
#else
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <stdio.h>
#include <string.h>

#include "tier0/dbg.h"
#include "profile.h"

#ifdef INSBOT_PROFILER

namespace
{
    double s_freqD = 0.0;       // QPC ticks per second, as double
    bool   s_freqReady = false;
    bool   s_enabled = false;

    unsigned s_frameStartLo = 0, s_frameStartHi = 0;
    unsigned s_winStartLo = 0, s_winStartHi = 0;
    unsigned s_winFrames = 0;   // frames in the current print window
    double   s_maxFrameMs = 0.0;

    double s_secs[INSPROF_BUCKET_COUNT] = { 0.0 };
    int    s_calls[INSPROF_BUCKET_COUNT] = { 0 };
}

static void InsProf_EnsureFreq();

static void InsProf_QPC(unsigned &lo, unsigned &hi)
{
    InsProf_EnsureFreq();
#ifdef _LINUX
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t t = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    lo = (unsigned)t;
    hi = (unsigned)(t >> 32);
#else
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    lo = (unsigned)t.QuadPart;
    hi = (unsigned)(t.QuadPart >> 32);
#endif
}

static double InsProf_FromHalves(unsigned hi, unsigned lo)
{
    return (double)hi * 4294967296.0 + (double)lo;
}

static double InsProf_DeltaSecs(unsigned sLo, unsigned sHi,
                                unsigned eLo, unsigned eHi)
{
    unsigned lo = eLo - sLo;
    unsigned borrow = (eLo < sLo) ? 1u : 0u;
    unsigned hi = eHi - sHi - borrow;
    return InsProf_FromHalves(hi, lo) / s_freqD;
}

static void InsProf_EnsureFreq()
{
    if (!s_freqReady)
    {
#ifdef _LINUX
        s_freqD = 1000000000.0;   // CLOCK_MONOTONIC nanosecond ticks
#else
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        s_freqD = (double)(unsigned)(f.QuadPart >> 32) * 4294967296.0
                + (double)(unsigned)f.QuadPart;
#endif
        s_freqReady = true;
    }
}

bool InsProf_Active()
{
    InsProf_EnsureFreq();
    return s_enabled;
}

void InsProf_SetEnabled(bool on)
{
    if (on == s_enabled) return;
    s_enabled = on;
    if (on)
    {
        s_winFrames = 0;
        s_maxFrameMs = 0.0;
        memset(s_secs, 0, sizeof(s_secs));
        memset(s_calls, 0, sizeof(s_calls));
        InsProf_QPC(s_winStartLo, s_winStartHi);
        Msg("[insbots] profiler enabled - summary every 5s (insbot set profile 0 to stop)\n");
    }
    else
    {
        Msg("[insbots] profiler disabled\n");
    }
}

void InsProf_FrameBegin()
{
    if (s_enabled) InsProf_QPC(s_frameStartLo, s_frameStartHi);
}

void InsProf_FrameEnd()
{
    if (!s_enabled) return;
    unsigned eLo, eHi;
    InsProf_QPC(eLo, eHi);
    double ms = InsProf_DeltaSecs(s_frameStartLo, s_frameStartHi, eLo, eHi) * 1000.0;
    s_secs[INSPROF_FRAME] += ms / 1000.0;   // stored in seconds like every bucket
    s_calls[INSPROF_FRAME]++;
    if (ms > s_maxFrameMs) s_maxFrameMs = ms;
    s_winFrames++;
}

void InsProf_Tick()
{
    if (!s_enabled) return;
    unsigned eLo, eHi;
    InsProf_QPC(eLo, eHi);
    double elapsed = InsProf_DeltaSecs(s_winStartLo, s_winStartHi, eLo, eHi);
    if (elapsed < 5.0) return;

    static const char *s_bucketNames[INSPROF_BUCKET_COUNT] =
    {
        "frame",
        "smoke_scan",
        "buildpath",
        "getpathcost",
        "findarea",
        "findnearest",
        "doorahead",
        "chokeahead",
        "lowobstacle",
        "vision",
        "hearing",
        "updatehearing",
        "teamsight",
        "dangerzones",
        "capcommit",
        "think",
        "thinkalive",
        "vehicle"
    };

    double frames = (double)(s_winFrames ? s_winFrames : 1);
    Msg("[insbots] profile %.1fs: %u frames  avg %.2f ms/f  max %.2f ms/f\n",
        elapsed, s_winFrames, s_secs[INSPROF_FRAME] * 1000.0 / frames, s_maxFrameMs);
    for (int i = INSPROF_FRAME + 1; i < INSPROF_BUCKET_COUNT; i++)
    {
        if (s_calls[i] <= 0) continue;
        Msg("  %-14s %8.2f ms/f  %7d calls  %8.3f ms/call\n",
            s_bucketNames[i],
            s_secs[i] * 1000.0 / frames,
            s_calls[i],
            s_secs[i] * 1000.0 / (double)s_calls[i]);
    }

    s_winStartLo = eLo;
    s_winStartHi = eHi;
    s_winFrames = 0;
    s_maxFrameMs = 0.0;
    memset(s_secs, 0, sizeof(s_secs));
    memset(s_calls, 0, sizeof(s_calls));
}

CInsProfTimer::CInsProfTimer(int bucket)
    : m_bucket(bucket), m_startLo(0), m_startHi(0), m_active(false)
{
    if (s_enabled)
    {
        InsProf_QPC(m_startLo, m_startHi);
        m_active = true;
    }
}

CInsProfTimer::~CInsProfTimer()
{
    if (!m_active) return;
    unsigned eLo, eHi;
    InsProf_QPC(eLo, eHi);
    if (m_bucket >= 0 && m_bucket < INSPROF_BUCKET_COUNT)
    {
        s_secs[m_bucket] += InsProf_DeltaSecs(m_startLo, m_startHi, eLo, eHi);
        s_calls[m_bucket]++;
    }
}

#endif // INSBOT_PROFILER
