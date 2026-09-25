#ifndef INSBOTS_LINUX_OFFSETS_H
#define INSBOTS_LINUX_OFFSETS_H

// Verified against the 32-bit Linux INS:MIC server binary:
//   insurgency/bin/server_i486.so
//   SHA-256 7afe49221a6f15d583261d9f019b706d88160ad67e6ca1141f5990f7152ffe70
//
// SendProp-backed values were also confirmed by loading the read-only audit
// plugin from linux/offset-audit in a disposable overlay of the game tree.
// Keep this table Linux-only: the Windows server.dll layouts are different.
#ifdef _LINUX
enum InsBotsLinuxOffset
{
    INS_LINUX_M_COLLISION_GROUP       = 0x01F0,
    INS_LINUX_M_NBUTTONS              = 0x0778,
    INS_LINUX_M_FL_NEXT_ATTACK        = 0x0A30,
    INS_LINUX_M_H_MY_WEAPONS          = 0x0A38,
    INS_LINUX_M_H_ACTIVE_WEAPON       = 0x0A54,
    // CINSPlayer::SetIronsightsState() drives AddPlayerFlag(1)/
    // RemovePlayerFlag(1), and IsMoveFrozen() is (flags >> 1) & 1. Bit 0
    // of this int therefore tracks the shouldered (ironsights) state.
    INS_LINUX_M_PLAYER_FLAGS          = 0x0AE0,
    INS_LINUX_M_ANG_EYE_ANGLES_0      = 0x0A6C,
    INS_LINUX_M_ANG_EYE_ANGLES_1      = 0x0A70,
    INS_LINUX_M_I_CLIP                = 0x04CC,

    INS_LINUX_OBJECTIVE_PARENT        = 0x0344,
    INS_LINUX_OBJECTIVE_ORDER_ID      = 0x034C,
    INS_LINUX_OBJECTIVE_CAPTURED_TEAM = 0x0360,
    INS_LINUX_OBJECTIVE_HIDDEN        = 0x0374,
    INS_LINUX_OBJECTIVE_CAPTURE_TEAM  = 0x03C0,

    INS_LINUX_MUZZLE_ORIGIN           = 0x10BC,
    INS_LINUX_MUZZLE_ANGLES           = 0x10C8,
    INS_LINUX_RPG_PRIMARY_ATTACK_SLOT = 213,

    // ---- Humvee auto-turret (see turret_abi.h) ----------------------------
    // Measured from the shipped server_i486.so and confirmed live on srcds.
    INS_LINUX_TURRET_OTD_SLOT          = 54,        // CBaseEntity::OnTakeDamage
    INS_LINUX_TURRET_ACCEPTINPUT_SLOT  = 35,        // CBaseEntity::AcceptInput
    INS_LINUX_TURRET_ALLOCPOOLEDSTRING = 0x44256E   // _Z17AllocPooledStringPKc
};

#define INSBOTS_LINUX_SERVER_SHA256 \
    "7afe49221a6f15d583261d9f019b706d88160ad67e6ca1141f5990f7152ffe70"
#endif

// Windows values. These are NOT offsets into a Linux layout - they are the
// Windows equivalents, measured from the shipped server.dll. They live outside
// the _LINUX guard because turret_abi.h consumes them on both platforms and
// picks the right one.
//
//   OnTakeDamage slot 53        - read from the engine's own TakeDamage call
//                                 site, then confirmed live on Windows srcds.
//   AcceptInput  slot 34        - derived by walking the CBaseEntity vtable
//                                 through RTTI, cross-checked against
//                                 SourceMod's published gamedata.
    //   AllocPooledString RVA 0x7DC10 - derived via the dumpgamestringtable ->
    //                                 CGameStringPool route; see the shell-eject
    //                                 verification doc section 5.1. NOTE this is
    //                                 an RVA: server.dll's preferred base is
    //                                 0x10000000, so the old absolute form was
    //                                 0x1007DC10. It must be rebased onto the
    //                                 LIVE module base - a listen server loads
    //                                 server.dll at 0x0EEC0000, where the
    //                                 absolute address is unmapped.
#define INSBOTS_WIN_TURRET_OTD_SLOT          53
#define INSBOTS_WIN_TURRET_ACCEPTINPUT_SLOT  34
#define INSBOTS_WIN_TURRET_ALLOCPOOLEDSTRING_RVA 0x7DC10ul

#endif
