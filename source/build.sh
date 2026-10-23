#!/bin/bash
# Cross-compile insbots.dll (32-bit, MSVC ABI) for INS:MIC srcds on Windows.
set -e
cd "$(dirname "$0")"

SDK=hl2sdk-episode1
OUT=build
CL=/usr/lib/llvm-18/bin/clang-cl
LINK=/usr/bin/lld-link

CFLAGS="--target=i686-pc-windows-msvc /c /O2 /GS- /GR- /EHs-c- /clang:-mstack-probe-size=2147483647 -w
  /D_WIN32 /DWIN32 /D_WINDOWS /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DVERSION_SAFE_STEAM_API_INTERFACES
  -fno-builtin
  /I src/shims
  /I src/bot
  /I $SDK/public
  /I $SDK/public/tier0
  /I $SDK/public/tier1
  /I $SDK/public/dlls
  /I $SDK/public/engine
  /I $SDK/public/vstdlib
  /I $SDK/public/mathlib
  /I $SDK/game_shared
  /I $SDK/tier1"

mkdir -p $OUT

compile() {
  src=$1; obj=$OUT/$(basename ${1%.*}).obj
  echo "CC  $src"
  $CL $CFLAGS "/Fo$obj" "$src"
}

compile src/crt_stub.cpp
compile src/sdk_glue.cpp
compile src/cfgvar.cpp
compile src/nav.cpp
compile src/objective.cpp
compile src/voice.cpp
compile src/bot/bot_util.cpp
compile src/bot/bot_entry.cpp
compile src/bot/bot_loadout.cpp
compile src/bot/bot_cover.cpp
compile src/bot/bot_core.cpp
compile src/bot/bot_perceive.cpp
compile src/bot/bot_roster.cpp
compile src/bot/bot_ladder.cpp
compile src/bot/bot_body.cpp
compile src/bot/bot_steer.cpp
compile src/bot/bot_recover.cpp
compile src/bot/bot_doorway.cpp
compile src/bot/bot_path.cpp
compile src/bot/bot_weapons.cpp
compile src/bot/bot_squad.cpp
compile src/bot/bot_think.cpp
compile src/insbots.cpp
compile $SDK/tier1/convar.cpp

echo "LINK insbots.dll"
$LINK /dll /machine:x86 /nodefaultlib \
  "/entry:_DllMainCRTStartup" \
  /out:$OUT/insbots.dll \
  $OUT/crt_stub.obj $OUT/sdk_glue.obj $OUT/cfgvar.obj $OUT/nav.obj $OUT/objective.obj $OUT/voice.obj $OUT/profile.obj $OUT/bot_util.obj $OUT/bot_entry.obj $OUT/bot_loadout.obj $OUT/bot_cover.obj $OUT/bot_core.obj $OUT/bot_perceive.obj $OUT/bot_roster.obj $OUT/bot_ladder.obj $OUT/bot_body.obj $OUT/bot_steer.obj $OUT/bot_recover.obj $OUT/bot_doorway.obj $OUT/bot_path.obj $OUT/bot_weapons.obj $OUT/bot_squad.obj $OUT/bot_think.obj $OUT/insbots.obj $OUT/convar.obj \
  $SDK/lib/public/tier0.lib $SDK/lib/public/vstdlib.lib \
  $OUT/msvcrt.lib

echo OK
