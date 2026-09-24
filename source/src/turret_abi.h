// ============================================================================
// turret_abi.h - the binary-level contracts the Humvee auto-turret depends on.
//
// Everything here is a LAYOUT or an ADDRESS, not behaviour. It is separated
// from turret.cpp because each item below was measured from the shipped
// binaries and each one, if wrong, crashes the server rather than failing a
// test. Read docs/humvee-turret-damage-audit.md and
// docs/humvee-turret-shell-eject-verification.md before changing any value.
//
// TWO RULES THAT HAVE ALREADY BITTEN IN THIS PROJECT:
//   1. Do NOT include "takedamageinfo.h" and construct CTakeDamageInfo from it.
//      The header in this SDK tree declares m_flBaseDamage, which the shipped
//      game DLLs do not have. Using it shifts m_bitsDamageType from 0x38 to
//      0x3C and delivers garbage damage types.
//   2. Do NOT assume the Windows and Linux calling conventions match.
//      AcceptInput is __thiscall on Windows (it pops its own 36 bytes) and
//      plain cdecl on Linux (the caller pops). Getting this wrong shifts the
//      stack on every shot.
// ============================================================================
#pragma once

#include "linux_offsets.h"
#include "tier0/dbg.h"

// ---- per-platform scalars ---------------------------------------------------
#ifdef _LINUX
    #define INS_TURRET_OTD_SLOT            INS_LINUX_TURRET_OTD_SLOT
    #define INS_TURRET_ACCEPTINPUT_SLOT    INS_LINUX_TURRET_ACCEPTINPUT_SLOT
    #define INS_TURRET_ALLOCPOOLEDSTRING   ((unsigned long)INS_LINUX_TURRET_ALLOCPOOLEDSTRING)
#else
    #define INS_TURRET_OTD_SLOT            INSBOTS_WIN_TURRET_OTD_SLOT
    #define INS_TURRET_ACCEPTINPUT_SLOT    INSBOTS_WIN_TURRET_ACCEPTINPUT_SLOT
    #define INS_TURRET_ALLOCPOOLEDSTRING   INSBOTS_WIN_TURRET_ALLOCPOOLEDSTRING
#endif

// ---- CTakeDamageInfo, hand-built to the MEASURED layout --------------------
// sizeof == 0x48 (72). The SDK header's 68 is wrong for these builds.
// Offsets are identical on Windows and Linux - this is plain data with one
// vptr, so it does NOT pick up the vtable-slot delta CBaseEntity has.
#pragma pack(push, 4)
struct InsTurretVector
{
    float x, y, z;
};

struct InsTurretTakeDamageInfo
{
    void *          m_pVtable;             // +0x00  vptr (_GetClassName)
    InsTurretVector m_vecDamageForce;      // +0x04
    InsTurretVector m_vecDamagePosition;   // +0x10
    InsTurretVector m_vecReportedPosition; // +0x1C
    unsigned long   m_hInflictor;          // +0x28  EHANDLE
    unsigned long   m_hAttacker;           // +0x2C  EHANDLE
    float           m_flDamage;            // +0x30
    float           m_flMaxDamage;         // +0x34
    int             m_bitsDamageType;      // +0x38
    int             m_iCustomKillType;     // +0x3C
    int             m_iAmmoType;           // +0x40
    unsigned char   m_bUnknown44;          // +0x44  copied as a byte
    unsigned char   m_pad[3];              // +0x45  pad to 0x48
};
#pragma pack(pop)

// COMPILE_TIME_ASSERT is a no-op in release builds (tier0/dbg.h:461-468), so
// the size check is repeated as a negative-array trick that holds in every
// configuration. Both must stay.
COMPILE_TIME_ASSERT( sizeof( InsTurretTakeDamageInfo ) == 0x48 );
COMPILE_TIME_ASSERT( offsetof( InsTurretTakeDamageInfo, m_vecDamageForce )      == 0x04 );
COMPILE_TIME_ASSERT( offsetof( InsTurretTakeDamageInfo, m_vecDamagePosition )   == 0x10 );
COMPILE_TIME_ASSERT( offsetof( InsTurretTakeDamageInfo, m_vecReportedPosition ) == 0x1C );
COMPILE_TIME_ASSERT( offsetof( InsTurretTakeDamageInfo, m_hInflictor )          == 0x28 );
COMPILE_TIME_ASSERT( offsetof( InsTurretTakeDamageInfo, m_hAttacker )           == 0x2C );
COMPILE_TIME_ASSERT( offsetof( InsTurretTakeDamageInfo, m_flDamage )            == 0x30 );
COMPILE_TIME_ASSERT( offsetof( InsTurretTakeDamageInfo, m_flMaxDamage )         == 0x34 );
COMPILE_TIME_ASSERT( offsetof( InsTurretTakeDamageInfo, m_bitsDamageType )      == 0x38 );
COMPILE_TIME_ASSERT( offsetof( InsTurretTakeDamageInfo, m_iCustomKillType )     == 0x3C );
COMPILE_TIME_ASSERT( offsetof( InsTurretTakeDamageInfo, m_iAmmoType )           == 0x40 );

// Release-safe equivalent of the sizeof assertion above.
typedef char InsTurretTakeDamageInfo_size_must_be_0x48[
    ( sizeof( InsTurretTakeDamageInfo ) == 0x48 ) ? 1 : -1 ];

// ---- calling conventions ----------------------------------------------------
// On Windows both of these are __thiscall: `this` in ECX and the CALLEE pops
// the stack arguments. AcceptInput ends in `ret 0x24` (36 bytes =
// 4+4+4+20+4). Declaring it without __thiscall makes the compiler emit a
// redundant `add esp,0x24`. Linux has no __thiscall; both are cdecl there.
#ifdef _LINUX
    typedef int  (*InsTurretOnTakeDamageFn)( void *, const void * );
    typedef bool (*InsTurretAcceptInputFn)( void *, const char *, void *,
                                            void *, const void *, int );
#else
    typedef int  (__thiscall *InsTurretOnTakeDamageFn)( void *, const void * );
    typedef bool (__thiscall *InsTurretAcceptInputFn)( void *, const char *,
                                                       void *, void *,
                                                       const void *, int );
#endif

// AllocPooledString returns string_t BY VALUE, and string_t is a struct, so
// the ABI is a hidden return pointer: the result slot is the FIRST argument.
// The report originally proposed `int32_t f(const char*)`; all 78 call sites
// in the shipped Windows server.dll disagree. This shape is the same on both
// platforms, so one typedef covers both.
typedef void (*InsTurretAllocPooledStringFn)( const char **ppOut, const char *pszValue );

// The two values a caller must not guess.
#define INS_TURRET_FIELD_STRING   2    // fieldtype_t: VOID=0, FLOAT=1, STRING=2
#define INS_TURRET_DMG_BULLET     2    // DMG_BULLET
