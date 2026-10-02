// ============================================================================
// round_freeze.h - engine-free round freeze window.
//
// WHY THIS EXISTS, AND WHY IT IS NOT g_RoundFrozen
// ------------------------------------------------
// g_RoundFrozen (insbots.cpp:55) does NOT mean "freeze time". It is set true on
// round_end and false on round_reset OR round_unfrozen, so it actually means
// "the round is over" - it is false throughout the freeze window and true
// throughout the intermission. Gating turrets on it would arm them during
// freeze and disarm them during play: exactly backwards.
//
// This type tracks the real freeze window instead, from three independent
// inputs, because no single one of them is reliable:
//
//   1. round_reset / round_end / round_unfrozen events.
//      Measured live: round_unfrozen fires on the FIRST round after a map load
//      and then NOT AGAIN (3 rounds observed, 1 round_unfrozen).
//
//   2. Elapsed time against the configured freezetime.
//      This is the fallback that makes (1) survivable.
//
//   3. CINSPlayer::IsMoveFrozen() - bit 1 of m_iPlayerFlags.
//      The engine's own answer, authoritative once it has been observed set
//      at least once in the current window. Linux offset 0x0AE0, Windows
//      0x0ACC - the same field insbots already reads for ADS (bit 0); bit 1
//      is move-frozen. Sampled from any live player, not only a driver.
//
// Degradation is safe by construction: the worst case is that turrets wake a
// couple of seconds late, never that they fire into a spawn room.
// ============================================================================
#pragma once

// A degenerate freezetime (0 or negative) must still hold the window shut
// briefly, so a turret cannot arm on the same tick a round resets.
const float ROUND_FREEZE_MIN_WINDOW = 1.0f;

class RoundFreezeWindow
{
public:
    RoundFreezeWindow()
    {
        m_resetTime  = 0.0f;
        m_open       = false;
        m_ended      = false;
        m_bitSeenSet = false;
        m_bitNow     = false;
    }

    // Called on map load and on every round_reset. Re-closes the window.
    void Reset( float now )
    {
        m_resetTime  = now;
        m_open       = false;
        m_ended      = false;
        m_bitSeenSet = false;
        m_bitNow     = false;
    }

    // round_end: the intermission is not play time. This latches until the
    // next Reset (round_reset) or OnUnfrozen, because the elapsed-time
    // fallback below would otherwise reopen the window the moment the timer
    // has expired - which it always has by the time a round ends.
    void OnRoundEnd( float now )
    {
        (void)now;
        m_open  = false;
        m_ended = true;
    }

    // round_unfrozen: opens the window early when the engine does fire it.
    void OnUnfrozen( float now )
    {
        (void)now;
        m_open  = true;
        m_ended = false;
    }

    // Called every tick with any live player's IsMoveFrozen bit. Only
    // meaningful while the bit has actually been observed set at least once
    // during this window - otherwise a read of "false" would open the window
    // before the freeze even started.
    void ObserveFrozenBit( bool frozen, float now )
    {
        (void)now;
        if (frozen)
        {
            m_bitSeenSet = true;
            m_bitNow     = true;
        }
        else
        {
            m_bitNow = false;
        }
    }

    // The decision the turret gate consumes.
    bool TurretsLive( float now, float freezetime ) const
    {
        if (m_ended)
            return false;                      // round_end: intermission

        if (m_open)
            return true;                       // round_unfrozen said so

        float window = freezetime;
        if (window < ROUND_FREEZE_MIN_WINDOW)
            window = ROUND_FREEZE_MIN_WINDOW;

        if (now - m_resetTime < window)
            return false;                      // timer has not expired

        // The timer has expired. If we have ever seen the engine report a
        // freeze this round, the engine wins until it reports otherwise.
        if (m_bitSeenSet && m_bitNow)
            return false;

        return true;
    }

private:
    float m_resetTime;
    bool  m_open;
    bool  m_ended;
    bool  m_bitSeenSet;
    bool  m_bitNow;
};
