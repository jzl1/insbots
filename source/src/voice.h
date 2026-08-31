// ============================================================================
// voice.h - bot radio callouts
//
// Bots "speak" the game's own INS:MIC voice lines by emitting the real vox/*.wav
// files (CHAN_VOICE, positional, attenuated) from the bot's own entity. We use
// the game's shipped waves so nothing extra has to be installed on clients, and
// we pick the wave per faction (USMC vs Iraqi) so Marines and Insurgents sound
// right. Emitting the raw wave via IEngineSound avoids poking the mod's internal
// comms/UTIL_SendVoice path (which reads per-player state and differs between the
// Windows/Linux server binaries), so it's robust and can't crash the game.
// ============================================================================
#pragma once

struct edict_t;

// A bot callout event. Each maps to a faction-specific radio line; see the wave
// tables in voice.cpp for exactly which lines play.
enum VoiceEvent
{
    VOX_ENGAGE = 0,   // opened fire on an enemy            "engaging / open fire"
    VOX_RELOAD,       // starting a reload                  "reloading"
    VOX_LOWAMMO,      // primary empty / switching sidearm  "out of ammo"
    VOX_GRENADE,      // threw a grenade                    "frag out"
    VOX_HURT,         // took a solid hit                   "I'm hit"
    VOX_HELP,         // low health / needs support         "need backup / medic"
    VOX_COVER,        // taking cover under fire            "taking cover"
    VOX_CAPTURE,      // capturing / securing an objective  "securing"
    VOX_MOVEUP,       // advancing to a new objective       "moving / move up"
    VOX_CLEAR,        // enemy down / area clear            "clear"
    VOX_COUNT
};

// Precache every voice wave (call from ServerActivate, once per level).
void InsVoice_Precache();

// Have `speaker` (a bot edict) say the line for event `ev`. `faction` is
// FACTION_USMC (1) or FACTION_IRAQI (2); 0/unknown falls back to USMC. `now` is
// gpGlobals->curtime. Honors the insbot_voice* toggles and per-bot throttle, so
// callers can fire this freely on an event without worrying about spam.
void InsVoice_Say(edict_t *speaker, int faction, int ev, float now);
