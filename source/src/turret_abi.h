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

#include <stddef.h>

#include "linux_offsets.h"
#include "tier0/dbg.h"
#include "irecipientfilter.h"
#include "vector.h"

// ---- per-platform scalars ---------------------------------------------------
#ifdef _LINUX
    #define INS_TURRET_OTD_SLOT            INS_LINUX_TURRET_OTD_SLOT
    #define INS_TURRET_ACCEPTINPUT_SLOT    INS_LINUX_TURRET_ACCEPTINPUT_SLOT
    // RVA inside server_i486.so, NOT an absolute address. That .so is an ELF
    // ET_DYN (PIE), so it is loaded at a randomised base and the RVA must be
    // added to the live module base. See InsTurretAllocPooledString below.
    #define INS_TURRET_ALLOCPOOLEDSTRING_RVA  INS_LINUX_TURRET_ALLOCPOOLEDSTRING
    // server_i486.so exports TE_DispatchEffect as a dynamic symbol, so Linux
    // resolves it BY NAME through dlsym and never uses this value. It is kept
    // for the executable-range guard, which still needs a plausible RVA.
    #define INS_TURRET_TEDISPATCHEFFECT_RVA  0x0066DA24
#else
    #define INS_TURRET_OTD_SLOT            INSBOTS_WIN_TURRET_OTD_SLOT
    #define INS_TURRET_ACCEPTINPUT_SLOT    INSBOTS_WIN_TURRET_ACCEPTINPUT_SLOT
    // RVA inside server.dll, NOT an absolute address. The shipped server.dll has
    // a preferred base of 0x10000000, but a listen server / client process loads
    // it wherever the loader likes (measured: 0x0EEC0000 on the live install).
    // The old absolute 0x1007DC10 was unmapped in that process. See
    // InsTurretAllocPooledString below.
    #define INS_TURRET_ALLOCPOOLEDSTRING_RVA  INSBOTS_WIN_TURRET_ALLOCPOOLEDSTRING_RVA
    // RVA inside server.dll. Preferred base 0x10000000, so this is the old
    // absolute 0x1012C790. Must be rebased onto the LIVE module base - the
    // same trap as AllocPooledString (a listen server loads server.dll at
    // 0x0EEC0000, where the preferred-base address is unmapped).
    #define INS_TURRET_TEDISPATCHEFFECT_RVA  0x0012C790
    // The absolute dword embedded in that function's third instruction
    // (`mov ecx, <static CEffectData the function copies into>`). It is NOT a
    // fixed byte pattern: the shipped server.dll carries a BASE RELOCATION at
    // RVA 0x12C796 covering it, so the loader rewrites it to
    // (live_base + this RVA) on every load. Any prologue check must compare
    // against the rebased value, not the on-disk literal - comparing the
    // literal fails whenever the module loads off its preferred base, which is
    // exactly what a listen server does (measured: 0x0EEC0000).
    #define INS_TURRET_TEDISPATCHEFFECT_TABLE_RVA  0x002B0F24
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

// ---- CEffectData, hand-built to the MEASURED layout ------------------------
//
// sizeof == 0x60 (96). The layout is IDENTICAL on Windows and Linux - it is
// plain data with no vtable, so it does not pick up the vtable-slot delta that
// CBaseEntity has.
//
// Audited three ways in docs/humvee-turret-muzzle-tracer-feasibility.md
// section 4: the DT_EffectData RecvTable in client.dll (0x1021F720), and
// CEffectData::operator= in BOTH server.dll (0x1012C650) and server_i486.so
// (0x0066DC80).
//
// Do NOT include game_shared/effect_dispatch_data.h and construct this from
// it. That header is newer than the shipped binaries - the same trap that
// already bit CTakeDamageInfo in this project (see the header comment above).
#pragma pack(push, 4)
struct InsTurretEffectData
{
    InsTurretVector m_vOrigin;           // +0x00  tracer END   (impact point)
    InsTurretVector m_vStart;            // +0x0C  tracer START (muzzle)
    InsTurretVector m_vNormal;           // +0x18  shot direction (normalised)
    InsTurretVector m_vAngles;           // +0x24  barrel angles
    int             m_fFlags;            // +0x30  0, or TRACER_FLAG_WHIZ = 1
    int             m_nEntIndex;         // +0x34  vehicle entity index
    float           m_flScale;           // +0x38  tracer speed; 0 = default
    float           m_flMagnitude;       // +0x3C  0
    float           m_flRadius;          // +0x40  0
    int             m_nAttachmentIndex;  // +0x44  0 = use m_vStart verbatim
    short           m_nSurfaceProp;      // +0x48  0
    short           m_pad4A;             // +0x4A
    int             m_nMaterial;         // +0x4C  0
    int             m_nDamageType;       // +0x50  *** iTracerType ***
    int             m_nHitBox;           // +0x54  0 = no attachment override
    unsigned char   m_bWhiz;             // +0x58  1 = near-miss whiz sound
    unsigned char   m_nColor;            // +0x59  0
    unsigned char   m_pad5A[2];          // +0x5A
    int             m_iEffectName;       // +0x5C  filled in by DispatchEffect
};
#pragma pack(pop)

// The client's own FX_Tracer material table maps this value to
// "effects/redflare", which is the material player-fired M249 tracers use.
// The engine reads it from m_nDamageType, NOT from a dedicated tracer field.
#define INS_TURRET_TRACERTYPE_RED  3

// Compile-time proof that the hand-built layout still matches the measured
// one. If any field above is edited, these fail the build instead of silently
// sending a malformed effect that the client ignores (or worse, misreads).
#define INS_TURRET_EFFECTDATA_ASSERT( cond, msg ) \
    typedef char ins_turret_effectdata_##msg[ (cond) ? 1 : -1 ]

INS_TURRET_EFFECTDATA_ASSERT( sizeof( InsTurretEffectData ) == 0x60,
                              size_is_0x60 );
INS_TURRET_EFFECTDATA_ASSERT( offsetof( InsTurretEffectData, m_vOrigin ) == 0x00,
                              origin_is_0x00 );
INS_TURRET_EFFECTDATA_ASSERT( offsetof( InsTurretEffectData, m_vStart ) == 0x0C,
                              start_is_0x0C );
INS_TURRET_EFFECTDATA_ASSERT( offsetof( InsTurretEffectData, m_vNormal ) == 0x18,
                              normal_is_0x18 );
INS_TURRET_EFFECTDATA_ASSERT( offsetof( InsTurretEffectData, m_vAngles ) == 0x24,
                              angles_is_0x24 );
INS_TURRET_EFFECTDATA_ASSERT( offsetof( InsTurretEffectData, m_fFlags ) == 0x30,
                              flags_is_0x30 );
INS_TURRET_EFFECTDATA_ASSERT( offsetof( InsTurretEffectData, m_nEntIndex ) == 0x34,
                              entindex_is_0x34 );
INS_TURRET_EFFECTDATA_ASSERT( offsetof( InsTurretEffectData, m_flScale ) == 0x38,
                              scale_is_0x38 );
INS_TURRET_EFFECTDATA_ASSERT( offsetof( InsTurretEffectData, m_nAttachmentIndex ) == 0x44,
                              attach_is_0x44 );
INS_TURRET_EFFECTDATA_ASSERT( offsetof( InsTurretEffectData, m_nDamageType ) == 0x50,
                              damagetype_is_0x50 );
INS_TURRET_EFFECTDATA_ASSERT( offsetof( InsTurretEffectData, m_bWhiz ) == 0x58,
                              whiz_is_0x58 );
INS_TURRET_EFFECTDATA_ASSERT( offsetof( InsTurretEffectData, m_iEffectName ) == 0x5C,
                              effectname_is_0x5C );

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

// TE_DispatchEffect( IRecipientFilter &filter, float delay, const Vector &pos,
//                    const char *pName, const CEffectData &data )
//
// cdecl on BOTH platforms - unlike AcceptInput, this one does NOT become
// __thiscall on Windows, because it is a free function with no `this`. The
// argument order is confirmed by the Linux mangled name:
//   _Z17TE_DispatchEffectR16IRecipientFilterfRK6VectorPKcRK11CEffectData
// which decodes to exactly the five parameters above, in that order.
//
// `delay` is 0.0f for an immediate effect.
typedef void (*InsTurretTEDispatchEffectFn)(
    IRecipientFilter &filter,
    float delay,
    const Vector &pos,
    const char *pName,
    const InsTurretEffectData &data );

// The two values a caller must not guess.
#define INS_TURRET_FIELD_STRING   2    // fieldtype_t: VOID=0, FLOAT=1, STRING=2
#define INS_TURRET_DMG_BULLET     2    // DMG_BULLET
