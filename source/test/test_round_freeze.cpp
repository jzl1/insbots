// Engine-free tests for the round freeze window. No SDK, no engine, no server.
#include <cstdio>
#include <cstring>
#include "round_freeze.h"

static int g_fail = 0;
static int g_pass = 0;

static void check(bool cond, const char *what)
{
    if (cond) { g_pass++; return; }
    g_fail++;
    printf("FAIL: %s\n", what);
}

// A fresh map load: turrets must be asleep until the window closes.
static void test_map_load_is_frozen()
{
    RoundFreezeWindow w;
    w.Reset(100.0f);
    check(!w.TurretsLive(100.0f, 4.0f), "at reset instant, turrets are asleep");
    check(!w.TurretsLive(103.9f, 4.0f), "just before freezetime expires, asleep");
    check( w.TurretsLive(104.1f, 4.0f), "just after freezetime expires, live");
}

// The measured real-world case: round_unfrozen never fires. The timer must
// still open the window, or turrets stay asleep for the whole round.
static void test_missing_unfrozen_event_still_opens()
{
    RoundFreezeWindow w;
    w.Reset(200.0f);
    // No OnUnfrozen call at all.
    check(!w.TurretsLive(203.0f, 4.0f), "no event, still inside window");
    check( w.TurretsLive(204.5f, 4.0f), "no event, timer opens the window");
}

// The engine's own frozen bit is authoritative when it is observable: a
// long freeze (mp_freezetime raised) must not be cut short by the timer.
static void test_frozen_bit_extends_the_window()
{
    RoundFreezeWindow w;
    w.Reset(300.0f);
    w.ObserveFrozenBit(true, 300.1f);
    check(!w.TurretsLive(304.5f, 4.0f), "bit still set: timer expiry alone does not open");
    w.ObserveFrozenBit(false, 305.0f);
    check( w.TurretsLive(305.1f, 4.0f), "bit cleared: window opens immediately");
}

// The bit must never open the window on its own before it has ever been seen
// set - a player who reads false during the intermission must not arm a turret
// early.
static void test_bit_alone_does_not_open_before_window()
{
    RoundFreezeWindow w;
    w.Reset(400.0f);
    w.ObserveFrozenBit(false, 400.5f);
    check(!w.TurretsLive(401.0f, 4.0f), "bit false but window not elapsed: asleep");
}

// round_unfrozen firing early (the normal first-round case) opens the window
// before the timer would have.
static void test_unfrozen_event_opens_early()
{
    RoundFreezeWindow w;
    w.Reset(500.0f);
    w.OnUnfrozen(501.0f);
    check(w.TurretsLive(501.1f, 4.0f), "event opens the window before the timer");
}

// round_end closes the window again for the intermission.
static void test_round_end_closes()
{
    RoundFreezeWindow w;
    w.Reset(600.0f);
    w.OnUnfrozen(601.0f);
    check(w.TurretsLive(601.1f, 4.0f), "live during play");
    w.OnRoundEnd(700.0f);
    check(!w.TurretsLive(700.1f, 4.0f), "round_end closes the window");
}

// A second round in the same map: Reset is called again on round_reset and the
// window must re-close, even though it was open a moment ago.
static void test_second_round_recloses()
{
    RoundFreezeWindow w;
    w.Reset(800.0f);
    w.OnUnfrozen(801.0f);
    check(w.TurretsLive(801.1f, 4.0f), "round 1 live");
    w.OnRoundEnd(900.0f);
    w.Reset(900.5f);                       // round 2 begins
    check(!w.TurretsLive(900.6f, 4.0f), "round 2 starts frozen again");
    check( w.TurretsLive(904.6f, 4.0f), "round 2 opens on the timer");
}

// A degenerate freezetime must not leave the window permanently shut.
static void test_zero_freezetime_still_opens()
{
    RoundFreezeWindow w;
    w.Reset(1000.0f);
    check(w.TurretsLive(1000.0f + ROUND_FREEZE_MIN_WINDOW + 0.1f, 0.0f),
          "freezetime 0 still opens after the minimum window");
}

int main()
{
    test_map_load_is_frozen();
    test_missing_unfrozen_event_still_opens();
    test_frozen_bit_extends_the_window();
    test_bit_alone_does_not_open_before_window();
    test_unfrozen_event_opens_early();
    test_round_end_closes();
    test_second_round_recloses();
    test_zero_freezetime_still_opens();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
