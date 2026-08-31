// ============================================================================
// voice.cpp - bot radio callouts (see voice.h)
//
// Emits the game's own INS:MIC vox/*.wav lines from a bot's entity via
// IEngineSound, so bots audibly call out combat and objective events using the
// exact voices real players hear. Faction-aware (USMC vs Iraqi), toggle-able,
// and throttled so a squad doesn't shout in unison.
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tier0/dbg.h"
#include "interface.h"
#include "eiface.h"
#include "edict.h"
#include "dlls/iplayerinfo.h"
#include "irecipientfilter.h"
#include "engine/IEngineSound.h"
#include "soundflags.h"

#include "voice.h"
#include "cfgvar.h"

extern IVEngineServer *engine;
extern CGlobalVars    *gpGlobals;
extern IPlayerInfoManager *playerinfomanager;
IEngineSound          *enginesound = 0;   // set in insbots.cpp Load() (optional)

// ---- toggles ---------------------------------------------------------------
// master + two category switches (combat chatter vs objective/support chatter),
// plus an anti-spam gap. All live in the same CfgVar registry as the rest.
CfgVar insbot_voice("voice", "1", "bots call out on the radio (engage/reload/grenade/cover/capture/...); 0 = silent");
CfgVar insbot_voice_combat("voice_combat", "1", "combat callouts: engaging, reloading, out of ammo, frag out, hit, take cover, clear");
CfgVar insbot_voice_objective("voice_objective", "1", "objective/support callouts: securing, moving up, need backup");
CfgVar insbot_voice_interval("voice_interval", "4.0", "minimum seconds between one bot's callouts (anti-spam)");

// ---- faction wave tables ---------------------------------------------------
// Real shipped files under sound/vox/<faction>/normal/ - verified present. Up to
// 5 variants + a null terminator per event. Emitting the raw wave means nothing
// extra needs installing on clients (the game already ships and precaches these).
#define VN 6
static const char *const s_usmc[VOX_COUNT][VN] =
{
    /* ENGAGE  */ { "vox/usmc/normal/engaging.wav", "vox/usmc/normal/engage.wav", "vox/usmc/normal/returnfire.wav", "vox/usmc/normal/openingfire.wav", 0 },
    /* RELOAD  */ { "vox/usmc/normal/reloading1.wav", "vox/usmc/normal/reloading2.wav", "vox/usmc/normal/reloading3.wav", "vox/usmc/normal/reloading4.wav", "vox/usmc/normal/reloading5.wav", 0 },
    /* LOWAMMO */ { "vox/usmc/normal/outofammo.wav", 0 },
    /* GRENADE */ { "vox/usmc/normal/fragout.wav", "vox/usmc/normal/fragout2.wav", 0 },
    /* HURT    */ { "vox/usmc/normal/hit.wav", "vox/usmc/normal/injured.wav", 0 },
    /* HELP    */ { "vox/usmc/normal/needbackup.wav", "vox/usmc/normal/corpsman1.wav", "vox/usmc/normal/corpsman2.wav", 0 },
    /* COVER   */ { "vox/usmc/normal/takecover.wav", "vox/usmc/normal/takecover2.wav", "vox/usmc/normal/takecover3.wav", "vox/usmc/normal/takingcover.wav", 0 },
    /* CAPTURE */ { "vox/usmc/normal/secure.wav", "vox/usmc/normal/standingby.wav", 0 },
    /* MOVEUP  */ { "vox/usmc/normal/move1.wav", "vox/usmc/normal/move2.wav", "vox/usmc/normal/gogogo.wav", "vox/usmc/normal/moveout.wav", "vox/usmc/normal/movingout.wav", 0 },
    /* CLEAR   */ { "vox/usmc/normal/clear1.wav", "vox/usmc/normal/clear2.wav", "vox/usmc/normal/clear3.wav", 0 },
};
static const char *const s_iraqi[VOX_COUNT][VN] =
{
    /* ENGAGE  */ { "vox/iraqi/normal/engaging.wav", "vox/iraqi/normal/engagetheenemy.wav", "vox/iraqi/normal/openfire.wav", "vox/iraqi/normal/openingfire.wav", "vox/iraqi/normal/fireatwill.wav", 0 },
    /* RELOAD  */ { "vox/iraqi/normal/reloading.wav", 0 },
    /* LOWAMMO */ { "vox/iraqi/normal/outofammo.wav", 0 },
    /* GRENADE */ { "vox/iraqi/normal/fragout.wav", 0 },
    /* HURT    */ { "vox/iraqi/normal/hit.wav", "vox/iraqi/normal/injured.wav", 0 },
    /* HELP    */ { "vox/iraqi/normal/requestingreinforcements.wav", "vox/iraqi/normal/requestingfiresupport.wav", "vox/iraqi/normal/mandown.wav", 0 },
    /* COVER   */ { "vox/iraqi/normal/takecover.wav", "vox/iraqi/normal/takingcover.wav", 0 },
    /* CAPTURE */ { "vox/iraqi/normal/securing.wav", "vox/iraqi/normal/objectivesecured.wav", "vox/iraqi/normal/inposition.wav", 0 },
    /* MOVEUP  */ { "vox/iraqi/normal/moving.wav", 0 },
    /* CLEAR   */ { "vox/iraqi/normal/clear.wav", 0 },
};

static int WaveCount(const char *const *list)
{
    int n = 0;
    while (n < VN && list[n]) n++;
    return n;
}

// ---- broadcast recipient filter --------------------------------------------
// Sends the sound to every connected client; the engine then plays it positional
// from the bot with CHAN_VOICE attenuation, so only players in earshot hear it.
class CVoiceFilter : public IRecipientFilter
{
public:
    CVoiceFilter()
    {
        m_n = 0;
        int maxc = gpGlobals ? gpGlobals->maxClients : 0;
        for (int i = 1; i <= maxc && m_n < 64; i++)
        {
            edict_t *e = engine->PEntityOfEntIndex(i);
            if (!e || e->IsFree()) continue;
            IPlayerInfo *pi = playerinfomanager ? playerinfomanager->GetPlayerInfo(e) : 0;
            if (pi && pi->IsConnected() && !pi->IsFakeClient()) m_idx[m_n++] = i;
        }
    }
    virtual ~CVoiceFilter() {}
    virtual bool IsReliable(void) const { return false; }
    virtual bool IsInitMessage(void) const { return false; }
    virtual int  GetRecipientCount(void) const { return m_n; }
    virtual int  GetRecipientIndex(int slot) const { return (slot >= 0 && slot < m_n) ? m_idx[slot] : -1; }
private:
    int m_idx[64];
    int m_n;
};

// ---- throttle state --------------------------------------------------------
static float s_lastBot[68];       // per-entindex time of last callout
static float s_lastAny = -1000.f; // any bot's last callout (team-wide spacing)

void InsVoice_Precache()
{
    if (!enginesound) return;
    for (int f = 0; f < 2; f++)
        for (int ev = 0; ev < VOX_COUNT; ev++)
        {
            const char *const *list = f ? s_iraqi[ev] : s_usmc[ev];
            for (int i = 0; i < VN && list[i]; i++)
                enginesound->PrecacheSound(list[i], true);
        }
    for (int i = 0; i < 68; i++) s_lastBot[i] = -1000.f;
    s_lastAny = -1000.f;
}

void InsVoice_Say(edict_t *speaker, int faction, int ev, float now)
{
    if (!enginesound || !engine || !speaker) return;
    if (ev < 0 || ev >= VOX_COUNT) return;
    if (!insbot_voice.GetBool()) return;

    // category gating
    bool combat = (ev == VOX_ENGAGE || ev == VOX_RELOAD || ev == VOX_LOWAMMO ||
                   ev == VOX_GRENADE || ev == VOX_HURT || ev == VOX_COVER || ev == VOX_CLEAR);
    if (combat && !insbot_voice_combat.GetBool()) return;
    if (!combat && !insbot_voice_objective.GetBool()) return;   // CAPTURE / MOVEUP / HELP

    int idx = engine->IndexOfEdict(speaker);
    if (idx < 1 || idx > 64) return;

    // per-bot anti-spam gap, plus a short team-wide gap so a squad doesn't all
    // shout the same instant
    float gap = insbot_voice_interval.GetFloat();
    if (gap < 0.5f) gap = 0.5f;
    if (now - s_lastBot[idx] < gap) return;
    if (now - s_lastAny < 0.5f) return;

    // pick a wave for this faction; fall back to the other faction if a line is
    // missing for one side (e.g. Iraqi has no dedicated 'clear' variety)
    const char *const *list = (faction == 2) ? s_iraqi[ev] : s_usmc[ev];
    int n = WaveCount(list);
    if (n == 0)
    {
        list = (faction == 2) ? s_usmc[ev] : s_iraqi[ev];
        n = WaveCount(list);
        if (n == 0) return;
    }
    const char *wave = list[rand() % n];

    CVoiceFilter filter;
    int pitch = 95 + (rand() % 11);   // 95..105, matching the soundscript spread
    enginesound->EmitSound(filter, idx, CHAN_VOICE, wave, 0.72f,
                           (soundlevel_t)SNDLVL_TALKING, 0, pitch);

    s_lastBot[idx] = now;
    s_lastAny = now;
}
