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
    // RVA inside server_i486.so, NOT an absolute address. That .so is an ELF
    // ET_DYN (PIE), so it is loaded at a randomised base and the RVA must be
    // added to the live module base. See InsTurretAllocPooledString below.
    #define INS_TURRET_ALLOCPOOLEDSTRING_RVA  INS_LINUX_TURRET_ALLOCPOOLEDSTRING
#else
    #define INS_TURRET_OTD_SLOT            INSBOTS_WIN_TURRET_OTD_SLOT
    #define INS_TURRET_ACCEPTINPUT_SLOT    INSBOTS_WIN_TURRET_ACCEPTINPUT_SLOT
    // RVA inside server.dll, NOT an absolute address. The shipped server.dll has
    // a preferred base of 0x10000000, but a listen server / client process loads
    // it wherever the loader likes (measured: 0x0EEC0000 on the live install).
    // The old absolute 0x1007DC10 was unmapped in that process. See
    // InsTurretAllocPooledString below.
    #define INS_TURRET_ALLOCPOOLEDSTRING_RVA  INSBOTS_WIN_TURRET_ALLOCPOOLEDSTRING_RVA
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

// ---- variant_t, hand-built to the MEASURED layout ---------------------------
// sizeof == 20 (0x14). A 12-byte union followed by CHandle<CBaseEntity> then
// fieldtype_t (dlls/variant_t.h). AcceptInput takes this BY VALUE, so the
// argument block is 4+4+4+20+4 = 36 = 0x24 - which is the callee's `ret 0x24`.
//
// The union is written as three dwords rather than a real union because the
// only field this plugin ever sets is the pooled string, and a POD-of-dwords
// keeps the copy semantics trivially identical to the shipped class (it is
// trivially copyable, so MSVC/Itanium pass it as a plain 20-byte stack copy).
#pragma pack(push, 4)
struct InsTurretVariant
{
    unsigned long m_union0;   // +0x00  string_t iszVal / int iVal / float flVal
    unsigned long m_union1;   // +0x04  union tail / padding
    unsigned long m_union2;   // +0x08  union tail / padding
    unsigned long m_eVal;     // +0x0C  CHandle<CBaseEntity>, 0 == INVALID
    int           m_fieldType;// +0x10  fieldtype_t
};
#pragma pack(pop)

COMPILE_TIME_ASSERT( sizeof( InsTurretVariant ) == 0x14 );
COMPILE_TIME_ASSERT( offsetof( InsTurretVariant, m_eVal )      == 0x0C );
COMPILE_TIME_ASSERT( offsetof( InsTurretVariant, m_fieldType ) == 0x10 );

typedef char InsTurretVariant_size_must_be_0x14[
    ( sizeof( InsTurretVariant ) == 0x14 ) ? 1 : -1 ];

// ---- calling conventions ----------------------------------------------------
// On Windows both of these are __thiscall: `this` in ECX and the CALLEE pops
// the stack arguments. AcceptInput ends in `ret 0x24` (36 bytes =
// 4+4+4+20+4). Declaring it without __thiscall makes the compiler emit a
// redundant `add esp,0x24`. Linux has no __thiscall; both are cdecl there.
//
// THE `variant_t` ARGUMENT IS PASSED DIFFERENTLY ON THE TWO PLATFORMS. This is
// not a style choice; it is the ABI, and getting it wrong corrupts the stack.
//
//   Windows (MSVC): a 20-byte class with a user-provided copy constructor but
//   no destructor is still passed INLINE on the stack. The callee's
//   `ret 0x24` pops 4+4+4+20+4 = 36 bytes, and its body reads all five
//   variant dwords. Declaring it `const void *` (4 bytes) made the plugin push
//   4+4+4+4+4 = 20 while the callee popped 36 - a 16-byte ESP shift per shot.
//   Two shots is 32 bytes and the return address is gone, which is exactly the
//   observed "fires twice, then the game dies". So on Windows the 20-byte
//   struct MUST be passed by value.
//
//   Linux (Itanium C++ ABI): the same class is NON-trivial
//   (`__is_trivially_copyable(variant_t) == 0`, because CHandle has
//   user-declared constructors), so GCC passes it by INVISIBLE REFERENCE - a
//   4-byte pointer to a caller-made temporary. Verified by compiling the real
//   class and reading the generated code: the call pushes 6 arguments
//   (24 bytes), not 9. So on Linux a POINTER is correct and the 20-byte
//   inline form would be wrong.
//
// The old code used the pointer form everywhere: correct on Linux, 16-byte
// stack corruption on Windows. The fix is per-platform, not global.
#ifdef _LINUX
    typedef int  (*InsTurretOnTakeDamageFn)( void *, const void * );
    typedef bool (*InsTurretAcceptInputFn)( void *, const char *, void *,
                                            void *, const void *, int );
    typedef const void *(*InsTurretGetRefEHandleFn)( void * );
#else
    typedef int  (__thiscall *InsTurretOnTakeDamageFn)( void *, const void * );
    typedef bool (__thiscall *InsTurretAcceptInputFn)( void *, const char *,
                                                       void *, void *,
                                                       InsTurretVariant, int );
    typedef const void *(__thiscall *InsTurretGetRefEHandleFn)( void * );
#endif

// CBaseEntity::GetRefEHandle() is the third virtual this plugin calls through
// a vtable, and it needs the same __thiscall treatment as the two above.
//
// It is the EASIEST of the three to get wrong, because it takes exactly one
// argument: a cdecl declaration compiles happily and *looks* right, but the
// callee never reads the pushed argument at all - it reads `this` out of ECX,
// which still holds the function pointer it was just called through. The
// handle that comes back is garbage, so the damage is attributed to a null
// attacker and CINSPlayer::OnTakeDamage discards every shot while the gun
// still fires, still ejects brass and still plays audio.
//
// It also fails in the most confusing direction possible: on Linux `this` IS
// the first stack argument, so the identical cdecl declaration is CORRECT
// there. The symptom is therefore "works on Linux, silently does no damage on
// Windows" - which is exactly what was reported live. Do not "simplify" this
// back to one shared typedef.

// AllocPooledString returns string_t BY VALUE, and string_t is a struct, so
// the ABI is a hidden return pointer: the result slot is the FIRST argument.
// The report originally proposed `int32_t f(const char*)`; all 78 call sites
// in the shipped Windows server.dll disagree. This shape is the same on both
// platforms, so one typedef covers both.
typedef void (*InsTurretAllocPooledStringFn)( const char **ppOut, const char *pszValue );

// The two values a caller must not guess.
#define INS_TURRET_FIELD_STRING   2    // fieldtype_t: VOID=0, FLOAT=1, STRING=2
#define INS_TURRET_DMG_BULLET     2    // DMG_BULLET
