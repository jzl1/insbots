// ============================================================================
// sniper_overwatch.h - engine-free Sniper perch policy and geometry.
//
// This header is the single seam for sniper overwatch decisions that do not
// need Source engine symbols: candidate range/frontline rules, cache readiness
// and Home drift, 3D arrival, transit/hold deadlines, Compromised perch
// exclusion, cover scoring, Sill cover detection, Overwatch arc construction,
// and objective-box route rejection. Production code supplies an ISniperProbe
// backed by IEngineTrace; native tests supply a scripted probe.
// ============================================================================
#pragma once

#include <math.h>

namespace sniper_ow
{

static const float kArrivalXY = 24.0f;
static const float kArrivalZ = 48.0f;
static const float kHomeDrift = 500.0f;
static const float kCompromisedRadius = 400.0f;
static const float kCompromisedTime = 45.0f;
static const float kTransitMin = 15.0f;
static const float kAttackForwardCap = 250.0f;
static const float kDefendForwardMax = 900.0f;
static const float kDefendForwardRatio = 0.35f;
static const float kArcMinHalfSpan = 8.0f;
static const float kArcMaxHalfSpan = 35.0f;
static const float kSillProbeLength = 96.0f;

class ISniperProbe
{
public:
    virtual ~ISniperProbe() {}
    virtual bool HullClear(float x, float y, float z, float height) = 0;
    virtual bool WorldLOS(float x1, float y1, float z1,
                          float x2, float y2, float z2) = 0;
};

struct FrontlineRule
{
    bool allowed;
    float bonus;
};

struct Arc
{
    float centerDeg;
    float halfSpanDeg;
};

inline float NormalizeDeg(float deg)
{
    float d = fmodf(deg, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d;
}

inline float AngularDiff(float a, float b)
{
    float d = fabsf(NormalizeDeg(a) - NormalizeDeg(b));
    if (d > 180.0f) d = 360.0f - d;
    return d;
}

inline float RangeScore(float distance)
{
    if (distance < 500.0f || distance > 3300.0f) return -1.0f;
    if (distance < 1200.0f) return (distance - 500.0f) / 700.0f;
    if (distance <= 2200.0f) return 1.0f;
    return (3300.0f - distance) / 1100.0f;
}

inline int CandidateSector(float dx, float dy, int sectorCount)
{
    if (sectorCount <= 0) return 0;
    float bearing = atan2f(dy, dx);
    int sector = (int)((bearing + 3.14159265f) * (float)sectorCount
                       / (2.0f * 3.14159265f));
    if (sector < 0) sector = 0;
    if (sector >= sectorCount) sector = sectorCount - 1;
    return sector;
}

// The forward axis runs from the Objective toward enemy Home. d is the
// candidate's signed projection on that axis: negative is the friendly side,
// zero is abeam, positive is past the Objective toward enemy Home.
inline FrontlineRule EvaluateFrontline(float spotX, float spotY,
                                       float objectiveX, float objectiveY,
                                       float enemyHomeX, float enemyHomeY,
                                       bool friendlyOwned)
{
    FrontlineRule out;
    out.allowed = true;
    out.bonus = 0.0f;

    float ax = enemyHomeX - objectiveX;
    float ay = enemyHomeY - objectiveY;
    float axisLen = sqrtf(ax * ax + ay * ay);
    if (axisLen < 1.0f)
    {
        ax = 1.0f;
        ay = 0.0f;
        axisLen = 1.0f;
    }
    ax /= axisLen;
    ay /= axisLen;

    float dx = spotX - objectiveX;
    float dy = spotY - objectiveY;
    float d = dx * ax + dy * ay;

    if (friendlyOwned)
    {
        float cap = kDefendForwardRatio * axisLen;
        if (cap > kDefendForwardMax) cap = kDefendForwardMax;
        out.allowed = (d <= cap);
        return out;
    }

    out.allowed = (d <= kAttackForwardCap);
    if (d <= 0.0f)
    {
        float t = -d / 1200.0f;
        if (t > 1.0f) t = 1.0f;
        out.bonus = 12.0f * t;
    }
    return out;
}

inline bool CacheReady(bool teamHomeKnown, bool enemyHomeKnown,
                       float now, float fallbackAt)
{
    return (teamHomeKnown && enemyHomeKnown)
        || (teamHomeKnown && now >= fallbackAt);
}

// A cache only distinguishes friendly-owned objectives from all others.  A
// transition to hostile is therefore just as significant as a transition to
// neutral, and must not be filtered out as an "unknown" relation.
inline bool OwnershipFlipRequiresRebuild(int relation, bool wasFriendly)
{
    return relation != -2 && ((relation > 0) != wasFriendly);
}

inline bool HomeDrifted(float x1, float y1, float x2, float y2)
{
    float dx = x1 - x2;
    float dy = y1 - y2;
    return dx * dx + dy * dy > kHomeDrift * kHomeDrift;
}

inline bool AtPerch(float posX, float posY, float posZ,
                    float perchX, float perchY, float perchZ)
{
    float dx = posX - perchX;
    float dy = posY - perchY;
    float dz = posZ - perchZ;
    return dx * dx + dy * dy <= kArrivalXY * kArrivalXY
        && fabsf(dz) <= kArrivalZ;
}

inline float TransitDeadline(float now, float pathCost)
{
    float travel = pathCost / 100.0f + 10.0f;
    if (travel < kTransitMin) travel = kTransitMin;
    return now + travel;
}

inline float HoldDeadline(float now, float minTime, float maxTime, float roll)
{
    if (minTime < 5.0f) minTime = 5.0f;
    if (maxTime < minTime) maxTime = minTime;
    if (roll < 0.0f) roll = 0.0f;
    if (roll > 1.0f) roll = 1.0f;
    return now + minTime + (maxTime - minTime) * roll;
}

inline bool PerchCompromised(float spotX, float spotY, float spotZ,
                             float compromisedX, float compromisedY,
                             float compromisedZ, float now, float until)
{
    if (now >= until) return false;
    float dx = spotX - compromisedX;
    float dy = spotY - compromisedY;
    return dx * dx + dy * dy <= kCompromisedRadius * kCompromisedRadius;
}

inline float AssignmentScore(float perchScore, float jitter, float pathCost)
{
    return perchScore + jitter - pathCost * 0.0015f;
}

inline Arc ArcFromYaws(const float *yaws, int count)
{
    Arc out;
    out.centerDeg = 0.0f;
    out.halfSpanDeg = kArcMinHalfSpan;
    if (!yaws || count <= 0) return out;
    if (count == 1)
    {
        out.centerDeg = NormalizeDeg(yaws[0]);
        return out;
    }

    float sorted[16];
    if (count > 16) count = 16;
    for (int i = 0; i < count; i++) sorted[i] = NormalizeDeg(yaws[i]);
    for (int i = 1; i < count; i++)
    {
        float v = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > v)
        {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = v;
    }

    float largestGap = -1.0f;
    int gapAfter = 0;
    for (int i = 0; i < count; i++)
    {
        float next = (i + 1 < count) ? sorted[i + 1] : sorted[0] + 360.0f;
        float gap = next - sorted[i];
        if (gap > largestGap)
        {
            largestGap = gap;
            gapAfter = i;
        }
    }

    float span = 360.0f - largestGap;
    float start = sorted[(gapAfter + 1) % count];
    out.centerDeg = NormalizeDeg(start + span * 0.5f);
    out.halfSpanDeg = span * 0.5f;
    if (out.halfSpanDeg < kArcMinHalfSpan) out.halfSpanDeg = kArcMinHalfSpan;
    if (out.halfSpanDeg > kArcMaxHalfSpan) out.halfSpanDeg = kArcMaxHalfSpan;
    return out;
}

inline bool HasSillCover(ISniperProbe &probe, float x, float y, float z,
                         float dirX, float dirY)
{
    float len = sqrtf(dirX * dirX + dirY * dirY);
    if (len < 0.001f) return false;
    dirX /= len;
    dirY /= len;
    float lowX = x + dirX * kSillProbeLength;
    float lowY = y + dirY * kSillProbeLength;
    bool lowBlocked = !probe.WorldLOS(x, y, z + 30.0f, lowX, lowY, z + 30.0f);
    bool highClear = probe.WorldLOS(x, y, z + 58.0f, lowX, lowY, z + 58.0f);
    return lowBlocked && highClear;
}

inline float CoverScore(const bool *blocked, int count, float objectiveBearingDeg,
                        bool hasSillCover)
{
    int forward = 0;
    int rear = 0;
    int total = 0;
    for (int i = 0; i < count; i++)
    {
        if (!blocked[i]) continue;
        total++;
        float bearing = (float)i * 45.0f;
        if (AngularDiff(bearing, objectiveBearingDeg) <= 112.5f) forward++;
        else rear++;
    }
    if (forward > 4) forward = 4;
    float score = (float)forward * 3.5f + (float)rear * 1.0f;
    if (hasSillCover) score += 10.0f;
    else if (total == 0) score -= 12.0f;
    return score;
}

inline bool SniperHoldSupported(bool crouchValidated, bool hasSillCover)
{
    return crouchValidated || hasSillCover;
}

inline float ClampArcToJambs(ISniperProbe &probe, float x, float y, float z,
                             float eyeHeight, float centerDeg, float halfSpanDeg)
{
    float half = halfSpanDeg;
    if (half > kArcMaxHalfSpan) half = kArcMaxHalfSpan;
    while (half > 0.0f)
    {
        bool clear = true;
        for (int sample = 0; sample <= 4; sample++)
        {
            float frac = (float)sample / 4.0f;
            float angle = (centerDeg - half) + half * 2.0f * frac;
            float rad = angle * 3.14159265f / 180.0f;
            float px = x + cosf(rad) * kSillProbeLength;
            float py = y + sinf(rad) * kSillProbeLength;
            if (!probe.WorldLOS(x, y, z + eyeHeight, px, py, z + eyeHeight))
            {
                clear = false;
                break;
            }
        }
        if (clear)
            return half;
        half -= 2.0f;
    }
    return 0.0f;
}

inline bool AabbOverlaps(float loX, float loY, float hiX, float hiY,
                         float objLoX, float objLoY, float objHiX, float objHiY)
{
    return loX <= objHiX && hiX >= objLoX && loY <= objHiY && hiY >= objLoY;
}

struct Box2D
{
    float loX, loY, hiX, hiY;
};

inline bool PathOverlapsAnyBox(const Box2D *pathBoxes, int pathCount,
                               const Box2D *boxes, const bool *skipBox,
                               int boxCount)
{
    if (!pathBoxes || pathCount <= 0 || !boxes || boxCount <= 0)
        return false;
    for (int b = 0; b < boxCount; b++)
    {
        if (skipBox && skipBox[b]) continue;
        for (int i = 0; i < pathCount; i++)
        {
            if (AabbOverlaps(pathBoxes[i].loX, pathBoxes[i].loY,
                             pathBoxes[i].hiX, pathBoxes[i].hiY,
                             boxes[b].loX, boxes[b].loY,
                             boxes[b].hiX, boxes[b].hiY))
                return true;
        }
    }
    return false;
}

} // namespace sniper_ow
