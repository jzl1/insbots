// terrain_audit.cpp - summarize walk-link elevation changes in a Source NAV.
// usage: terrain_audit <map.nav>
#include <cstdio>
#include <cstdlib>
#include <cmath>

struct Vector
{
    float x, y, z;
    Vector() {}
    Vector(float a, float b, float c) : x(a), y(b), z(c) {}
    Vector operator-(const Vector &o) const { return Vector(x-o.x, y-o.y, z-o.z); }
    float Length() const { return sqrtf(x*x + y*y + z*z); }
};

#define private public
#define protected public
#define TESTING_NATIVE 1
#include "../src/nav.h"
#include "../src/nav.cpp"

static void Bucket(float value, int *counts)
{
    if (value < 2.0f) counts[0]++;
    else if (value < 6.0f) counts[1]++;
    else if (value < 12.0f) counts[2]++;
    else if (value < 18.0f) counts[3]++;
    else if (value < 24.0f) counts[4]++;
    else if (value < 36.0f) counts[5]++;
    else counts[6]++;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("usage: terrain_audit <map.nav>\n");
        return 1;
    }

    CNavMesh mesh;
    if (!mesh.Load(argv[1]))
    {
        printf("FAIL load\n");
        return 1;
    }

    int centerBuckets[7] = {0};
    int seamBuckets[7] = {0};
    int links = 0;
    int portalLinks = 0;
    int uphillOver36 = 0;
    int centerLookupMismatch = 0;
    int portalLookupOther = 0;
    int portalLookupFrom = 0;
    int portalLookupTo = 0;
    int mergedEdges = 0;
    int mergedShift8 = 0;
    int mergedShift16 = 0;
    int mergedShift24 = 0;
    int mergedShift40 = 0;
    float maxMergedShift = 0.0f;
    float maxCenterDz = 0.0f;
    float maxSeamDz = 0.0f;

    for (int i = 0; i < mesh.Count(); i++)
    {
        const NavAreaData *a = mesh.Area(i);
        if (!a) continue;
        Vector centerProbe(a->center.x, a->center.y,
                           mesh.GetZ(i, a->center.x, a->center.y));
        if (mesh.FindArea(centerProbe) != i) centerLookupMismatch++;
        for (int k = 0; k < a->connCount; k++)
        {
            int j = a->connIdx[k];
            if (j < 0 || j <= i) continue;
            int ladder = -1; bool up = false;
            if (mesh.GetLadderConnection(i, j, ladder, up)) continue;
            const NavAreaData *b = mesh.Area(j);
            if (!b) continue;

            links++;
            float centerDz = fabsf(b->center.z - a->center.z);
            Bucket(centerDz, centerBuckets);
            if (centerDz > maxCenterDz) maxCenterDz = centerDz;
            if (b->center.z > a->center.z + 36.0f
                || a->center.z > b->center.z + 36.0f)
                uphillOver36++;

            Vector mid, axis; float half = 0.0f;
            if (mesh.GetPortalMerged(i, j, mid, axis, half))
            {
                portalLinks++;
                float za = mesh.GetZ(i, mid.x, mid.y);
                float zb = mesh.GetZ(j, mid.x, mid.y);
                float seamDz = fabsf(zb - za);
                Bucket(seamDz, seamBuckets);
                if (seamDz > maxSeamDz) maxSeamDz = seamDz;
                Vector seamProbe(mid.x, mid.y, zb);
                int found = mesh.FindArea(seamProbe);
                if (found == i) portalLookupFrom++;
                else if (found == j) portalLookupTo++;
                else portalLookupOther++;
            }
        }
    }

    // GetPortalMerged is directional because it expands through the source
    // area's neighbor list. Compare the resulting clamped steering point with
    // the exact shared-edge midpoint used by 0.18.11 on every directed edge.
    for (int i = 0; i < mesh.Count(); i++)
    {
        const NavAreaData *a = mesh.Area(i);
        if (!a) continue;
        for (int k = 0; k < a->connCount; k++)
        {
            int j = a->connIdx[k];
            int ladder = -1; bool up = false;
            if (j < 0 || mesh.GetLadderConnection(i, j, ladder, up)) continue;
            Vector exact, merged, axis; float half = 0.0f;
            if (!mesh.GetPortal(i, j, exact)
                || !mesh.GetPortalMerged(i, j, merged, axis, half))
                continue;
            const NavAreaData *dest = mesh.Area(j);
            if (!dest) continue;
            if (merged.x < dest->lo.x) merged.x = dest->lo.x;
            else if (merged.x > dest->hi.x) merged.x = dest->hi.x;
            if (merged.y < dest->lo.y) merged.y = dest->lo.y;
            else if (merged.y > dest->hi.y) merged.y = dest->hi.y;
            float dx = merged.x - exact.x, dy = merged.y - exact.y;
            float shift = sqrtf(dx * dx + dy * dy);
            mergedEdges++;
            if (shift > 8.0f) mergedShift8++;
            if (shift > 16.0f) mergedShift16++;
            if (shift > 24.0f) mergedShift24++;
            if (shift > 40.0f) mergedShift40++;
            if (shift > maxMergedShift) maxMergedShift = shift;
        }
    }

    const char *labels[7] = {"<2", "2-5", "6-11", "12-17", "18-23", "24-35", ">=36"};
    printf("areas=%d walk-links=%d portal-links=%d\n", mesh.Count(), links, portalLinks);
    printf("center dz: max=%.1f over36=%d\n", maxCenterDz, uphillOver36);
    for (int i = 0; i < 7; i++) printf("  %-5s %d\n", labels[i], centerBuckets[i]);
    printf("portal seam dz: max=%.1f\n", maxSeamDz);
    for (int i = 0; i < 7; i++) printf("  %-5s %d\n", labels[i], seamBuckets[i]);
    printf("lookup ambiguity: center-mismatch=%d portal-from=%d portal-to=%d portal-other=%d\n",
           centerLookupMismatch, portalLookupFrom, portalLookupTo, portalLookupOther);
    printf("merged waypoint displacement: directed=%d >8=%d >16=%d >24=%d >40=%d max=%.1f\n",
           mergedEdges, mergedShift8, mergedShift16, mergedShift24,
           mergedShift40, maxMergedShift);
    return 0;
}
