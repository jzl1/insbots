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
    INS_LINUX_RPG_PRIMARY_ATTACK_SLOT = 213
};

#define INSBOTS_LINUX_SERVER_SHA256 \
    "7afe49221a6f15d583261d9f019b706d88160ad67e6ca1141f5990f7152ffe70"
#endif

#endif
