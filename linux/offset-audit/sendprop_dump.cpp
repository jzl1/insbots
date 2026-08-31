// Read-only Linux offset audit plugin for INS:MIC build 4044.
//
// This deliberately uses only IServerGameDLL::GetAllServerClasses().  It does
// not cast live game entities, write game memory, or call private game code.
#include <string.h>

#include "tier0/dbg.h"
#include "interface.h"
#include "engine/iserverplugin.h"
#include "eiface.h"
#include "edict.h"
#include "server_class.h"
#include "dt_send.h"

static const char *const kWantedProps[] = {
    "m_flNextAttack",
    "m_angEyeAngles[0]",
    "m_angEyeAngles[1]",
    "m_CollisionGroup",
    "m_hActiveWeapon",
    "m_iClip",
    "m_iOrderID",
    "m_iCapturedTeam",
    "m_iCaptureTeam",
    "m_bHidden",
    0
};

static bool IsWanted(const char *name)
{
    if (!name) return false;
    for (int i = 0; kWantedProps[i]; ++i)
        if (!strcmp(name, kWantedProps[i])) return true;
    return false;
}

static void DumpMatches(const char *serverClass, SendTable *table,
                        int parentOffset, int depth)
{
    if (!table || depth > 32) return;
    for (int i = 0; i < table->GetNumProps(); ++i)
    {
        SendProp *prop = table->GetProp(i);
        if (!prop) continue;
        const char *name = prop->GetName();
        const int absoluteOffset = parentOffset + prop->GetOffset();
        if (IsWanted(name))
        {
            Msg("[offset-audit] class=%s table=%s prop=%s offset=%d (0x%X) depth=%d\n",
                serverClass ? serverClass : "?",
                table->GetName() ? table->GetName() : "?",
                name, absoluteOffset, absoluteOffset, depth);
        }
        if (prop->GetDataTable())
            DumpMatches(serverClass, prop->GetDataTable(), absoluteOffset, depth + 1);
    }
}

class CSendPropDumpPlugin : public IServerPluginCallbacks
{
public:
    CSendPropDumpPlugin() : m_server(0) {}

    virtual bool Load(CreateInterfaceFn, CreateInterfaceFn gameServerFactory)
    {
        m_server = (IServerGameDLL *)gameServerFactory("ServerGameDLL006", 0);
        if (!m_server) m_server = (IServerGameDLL *)gameServerFactory("ServerGameDLL005", 0);
        if (!m_server) m_server = (IServerGameDLL *)gameServerFactory("ServerGameDLL004", 0);
        if (!m_server) m_server = (IServerGameDLL *)gameServerFactory("ServerGameDLL003", 0);
        if (!m_server)
        {
            Msg("[offset-audit] ERROR: no IServerGameDLL interface\n");
            return false;
        }

        Msg("[offset-audit] BEGIN recursive SendProp dump\n");
        for (ServerClass *serverClass = m_server->GetAllServerClasses();
             serverClass; serverClass = serverClass->m_pNext)
        {
            DumpMatches(serverClass->GetName(), serverClass->m_pTable, 0, 0);
        }
        Msg("[offset-audit] END recursive SendProp dump\n");
        return true;
    }

    virtual void Unload(void) {}
    virtual void Pause(void) {}
    virtual void UnPause(void) {}
    virtual const char *GetPluginDescription(void) { return "InsBots read-only SendProp offset audit"; }
    virtual void LevelInit(char const *) {}
    virtual void ServerActivate(edict_t *, int, int) {}
    virtual void GameFrame(bool) {}
    virtual void LevelShutdown(void) {}
    virtual void ClientActive(edict_t *) {}
    virtual void ClientDisconnect(edict_t *) {}
    virtual void ClientPutInServer(edict_t *, char const *) {}
    virtual void SetCommandClient(int) {}
    virtual void ClientSettingsChanged(edict_t *) {}
    virtual PLUGIN_RESULT ClientConnect(bool *, edict_t *, const char *, const char *, char *, int) { return PLUGIN_CONTINUE; }
    virtual PLUGIN_RESULT ClientCommand(edict_t *) { return PLUGIN_CONTINUE; }
    virtual PLUGIN_RESULT NetworkIDValidated(const char *, const char *) { return PLUGIN_CONTINUE; }

private:
    IServerGameDLL *m_server;
};

static CSendPropDumpPlugin s_plugin;

extern "C" __attribute__((visibility("default")))
void *CreateInterface(const char *name, int *returnCode)
{
    if (name && !strcmp(name, INTERFACEVERSION_ISERVERPLUGINCALLBACKS))
    {
        if (returnCode) *returnCode = 0;
        return &s_plugin;
    }
    if (returnCode) *returnCode = 1;
    return 0;
}
