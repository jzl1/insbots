// native test harness for the sdk_glue V_strncat bounds handling
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "../src/sdk_glue.cpp"

static int g_failures = 0;
static void Check(bool cond, const char *what)
{
    if (!cond) { printf("FAIL: %s\n", what); g_failures++; }
    else printf("ok: %s\n", what);
}

int main()
{
    // null destination is a no-op
    Check(V_strncat(0, "abc", 8, -1) == 0, "null dest returns null");

    // zero-size buffer is a no-op
    char z[4] = {'x', 'y', 'z', 'w'};
    V_strncat(z, "abc", 0, -1);
    Check(memcmp(z, "xyzw", 4) == 0, "zero-size buffer untouched");

    // empty destination
    char e[8] = "";
    V_strncat(e, "abc", sizeof(e), -1);
    Check(strcmp(e, "abc") == 0, "empty destination appends");

    // exact fit
    char x[8] = "ab";
    V_strncat(x, "cdef", sizeof(x), -1);
    Check(strcmp(x, "abcdef") == 0, "exact fit appends and terminates");

    // truncation
    char t[6] = "ab";
    V_strncat(t, "cdefgh", sizeof(t), -1);
    Check(strcmp(t, "abcde") == 0, "truncated append terminates");

    // unterminated full destination
    char u[4] = {'a', 'b', 'c', 'd'};
    V_strncat(u, "XYZ", sizeof(u), -1);
    Check(strcmp(u, "abc") == 0, "unterminated buffer terminated, no append");

    // null source is treated as empty
    char n[8] = "ab";
    V_strncat(n, 0, sizeof(n), -1);
    Check(strcmp(n, "ab") == 0, "null source is empty");

    // maxToCopy limits the copy
    char m[8] = "a";
    V_strncat(m, "bcdef", sizeof(m), 2);
    Check(strcmp(m, "abc") == 0, "maxToCopy limits copy");

    // maxToCopy zero appends nothing
    char mz[8] = "ab";
    V_strncat(mz, "cdef", sizeof(mz), 0);
    Check(strcmp(mz, "ab") == 0, "maxToCopy zero appends nothing");

    // maxToCopy -1 is unlimited
    char u2[16] = "a";
    V_strncat(u2, "bcdef", sizeof(u2), -1);
    Check(strcmp(u2, "abcdef") == 0, "maxToCopy -1 unlimited");

    if (g_failures) { printf("%d FAILURE(S)\n", g_failures); return 1; }
    printf("ALL OK\n");
    return 0;
}