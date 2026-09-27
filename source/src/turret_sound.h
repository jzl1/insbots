// ============================================================================
// turret_sound.h - the Humvee auto-turret's M249 gunshot constants.
//
// These values are the turret's whole gunshot sound. They are the exact
// parameters Valve's "Weapon_M249.Single" soundscript resolves to, so a turret
// burst and a player's M249 are indistinguishable by ear:
//
//     scripts/game_sounds_weapons.txt:410-420
//         "Weapon_M249.Single"
//         {
//             "channel"    "CHAN_WEAPON"
//             "volume"     "1.0"
//             "soundlevel" "SNDLVL_150dB"     <- Rank 1
//             "pitch"      "95,105"           <- Rank 3
//             "rndwave" { "wave" "^weapons/m249/m249-1.wav" }   <- Rank 2
//         }
//
// Rank 4 (the muzzle origin) is not here - it is a runtime Vector, not a
// constant, and it lives at the EmitSound call in turret.cpp.
//
// WHY THIS IS A HEADER AND NOT INLINE CONSTANTS:
// The '^' prefix and the pitch spread are both silent-failure hazards. Drop the
// '^' and the precache key no longer matches the emit key, so the shot plays
// nothing and logs nothing. Change the pitch range and the turret stops
// matching the player's M249. Keeping them here - with no SDK or engine
// includes - lets temp/turret_offline/test_turret_sound.cpp assert both in
// milliseconds, the same way turret_math.h is tested.
//
// ---------------------------------------------------------------------------
// THE '^' IS LOAD-BEARING. DO NOT "CLEAN IT UP".
//
// '^' is CHAR_DISTVARIANT (public/soundchars.h:19): a distance-variant encoded
// stereo wav whose LEFT channel is the close-range crack and whose RIGHT
// channel is the distant boom. The engine cross-fades between them by distance.
//
// The same prefix must appear in BOTH the precache call and the emit call.
// Valve does exactly this:
//
//     SoundEmitterSystem.cpp:385   PrecacheSound( GetWaveName(...) )   <- '^' kept
//     SoundEmitterSystem.cpp:536   enginesound->EmitSound( ... params.soundname ... )
//     SoundEmitterSystem.cpp:589   enginesound->EmitSound( ... ep.m_pSoundName ... )
//
// GetWaveName() returns the wave string verbatim, prefix included. The engine's
// PSkipSoundChars() (soundchars.h:37-45) strips the prefix when resolving the
// file on disk - but the PRECACHE KEY is the prefixed string. Precache and emit
// therefore have to agree, or the lookup misses.
//
// Measured on the shipped asset (2ch, 16-bit, 44100 Hz, 1.944 s):
//     CH0 (close) peak 32767  rms 6501.6
//     CH1 (far)   peak 32767  rms 3467.7
// ---------------------------------------------------------------------------
#ifndef INS_TURRET_SOUND_H
#define INS_TURRET_SOUND_H

// Rank 2: distance-variant wave. The '^' is CHAR_DISTVARIANT - see above.
#define INS_TURRET_M249_WAVE  "^weapons/m249/m249-1.wav"

// Rank 1: SNDLVL_150dB, matching the soundscript. This is deliberately NOT
// SNDLVL_GUNFIRE (140, i.e. 0.27 attenuation): 150dB is 0.2 attenuation and is
// what Weapon_M249.Single actually declares (public/soundflags.h:90-91).
// The value is a soundlevel_t, so turret.cpp must call the soundlevel_t
// overload of EmitSound - passing a float would select the attenuation
// overload and re-derive a different level.
#define INS_TURRET_M249_SOUNDLEVEL  150

// Rank 3: the soundscript's "pitch" "95,105" spread, inclusive.
#define INS_TURRET_M249_PITCH_MIN   95
#define INS_TURRET_M249_PITCH_MAX   105

// True iff `wave` carries the leading distance-variant prefix.
// NULL-safe and empty-string-safe so it can be called from the test harness
// and from a future diagnostic without a guard at every call site.
inline bool InsTurretSoundWaveHasDistVariant( const char *wave )
{
    return wave != 0 && wave[0] == '^';
}

// Map an arbitrary integer roll into the inclusive 95..105 spread.
//
// The roll is bounded here rather than trusting the caller's modulus. A caller
// doing `95 + (rand() % 11)` is correct only if rand() returns a non-negative
// value, which the C standard does not guarantee; a negative result would put
// the pitch below 95 and desync the turret from the player's M249. This
// function is total: every input maps into the spread.
inline int InsTurretSoundPitch( int roll )
{
    const int span = INS_TURRET_M249_PITCH_MAX - INS_TURRET_M249_PITCH_MIN + 1; // 11
    int m = roll % span;
    if (m < 0) m += span;
    return INS_TURRET_M249_PITCH_MIN + m;
}

#endif // INS_TURRET_SOUND_H
