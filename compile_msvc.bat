@echo off
setlocal enableextensions
echo Setting up Visual Studio 32-bit environment...

:: 1. Check if cl.exe is already available in PATH (e.g. Developer Command Prompt)
where cl.exe >nul 2>&1
if %ERRORLEVEL% EQU 0 goto vcfound

:: 2. Auto-locate Visual Studio installation via vswhere.exe
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvarsall.bat" (
            call "%%i\VC\Auxiliary\Build\vcvarsall.bat" x86
            goto vcfound
        )
    )
)

:: 3. Common fallback search across drive letters
for %%d in (C D E F) do (
    if exist "%%d:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" (
        call "%%d:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86
        goto vcfound
    )
    if exist "%%d:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" (
        call "%%d:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86
        goto vcfound
    )
    if exist "%%d:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat" (
        call "%%d:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat" x86
        goto vcfound
    )
)

:vcfound
cd /d "%~dp0"
if not exist build mkdir build

:: 4. Resolve HL2SDK path: check environment variable, then relative workspace paths
if defined HL2SDK (
    set "SDK=%HL2SDK%"
) else if exist "%~dp0..\..\hl2sdk-episode1\hl2sdk-episode1" (
    set "SDK=%~dp0..\..\hl2sdk-episode1\hl2sdk-episode1"
) else if exist "%~dp0..\hl2sdk-episode1\hl2sdk-episode1" (
    set "SDK=%~dp0..\hl2sdk-episode1\hl2sdk-episode1"
) else if exist "%~dp0..\..\hl2sdk-episode1" (
    set "SDK=%~dp0..\..\hl2sdk-episode1"
) else (
    set "SDK=F:\claude\hl2sdk-episode1\hl2sdk-episode1"
)

for /f "delims=" %%i in ('powershell -NoProfile -Command "[DateTime]::UtcNow.ToString('MMM dd yyyy HH:mm:ss')" ') do set BUILD_TIME=%%i
set DEFS=/D_WIN32 /DWIN32 /D_WINDOWS /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DVERSION_SAFE_STEAM_API_INTERFACES /DINSBOT_PROFILER /DBUILD_TIMESTAMP="\"%BUILD_TIME%\""
set INCLUDES=/I source/src /I source/src/shims /I source/src/bot /I "%SDK%\public" /I "%SDK%\public\tier0" /I "%SDK%\public\tier1" /I "%SDK%\public\dlls" /I "%SDK%\public\engine" /I "%SDK%\public\vstdlib" /I "%SDK%\public\mathlib" /I "%SDK%\game_shared" /I "%SDK%\tier1"

echo Compiling insbots sources with MSVC (UTC: %BUILD_TIME%)...
cl.exe /O2 /Oi- /GS- /GR- /EHs-c- /W3 /MP %DEFS% %INCLUDES% /Fo:build/ /c source/src/crt_stub.cpp source/src/sdk_glue.cpp source/src/cfgvar.cpp source/src/nav.cpp source/src/objective.cpp source/src/voice.cpp source/src/profile.cpp source/src/bot/bot_util.cpp source/src/bot/bot_entry.cpp source/src/bot/bot_loadout.cpp source/src/bot/bot_cover.cpp source/src/bot/bot_core.cpp source/src/bot/bot_perceive.cpp source/src/bot/bot_roster.cpp source/src/bot/bot_haditha.cpp source/src/bot/bot_ladder.cpp source/src/bot/bot_body.cpp source/src/bot/bot_steer.cpp source/src/bot/bot_recover.cpp source/src/bot/bot_doorway.cpp source/src/bot/bot_path.cpp source/src/bot/bot_weapons.cpp source/src/bot/bot_squad.cpp source/src/bot/bot_think.cpp source/src/insbots.cpp "%SDK%\tier1\convar.cpp"

if %ERRORLEVEL% NEQ 0 (
    echo Compilation failed!
    exit /b %ERRORLEVEL%
)

echo Generating msvcrt import library...
(
echo LIBRARY msvcrt.dll
echo EXPORTS
echo malloc
echo calloc
echo realloc
echo free
echo memcpy
echo memmove
echo memset
echo memcmp
echo strlen
echo strcpy
echo strncpy
echo strcat
echo strncat
echo strcmp
echo strncmp
echo _stricmp
echo _strnicmp
echo strstr
echo strchr
echo strrchr
echo _strdup
echo strtok
echo strtol
echo strtod
echo atof
echo atoi
echo tolower
echo toupper
echo isspace
echo isdigit
echo isalpha
echo isalnum
echo fopen
echo fclose
echo fread
echo fwrite
echo fseek
echo ftell
echo fflush
echo fprintf
echo printf
echo sprintf
echo _snprintf
echo vsprintf
echo _vsnprintf
echo sscanf
echo sin
echo cos
echo tan
echo asin
echo acos
echo atan
echo atan2
echo sqrt
echo pow
echo exp
echo log
echo log10
echo floor
echo ceil
echo fabs
echo fmod
echo abort
echo exit
echo rand
echo fgets
echo remove
echo srand
echo time
) > build\msvcrt.def

lib.exe /def:build\msvcrt.def /out:build\msvcrt.lib /machine:x86

echo Linking insbots.dll...
link.exe /dll /machine:x86 /nodefaultlib /entry:_DllMainCRTStartup /out:insbots.dll build\crt_stub.obj build\sdk_glue.obj build\cfgvar.obj build\nav.obj build\objective.obj build\voice.obj build\profile.obj build\bot_util.obj build\bot_entry.obj build\bot_loadout.obj build\bot_cover.obj build\bot_core.obj build\bot_perceive.obj build\bot_roster.obj build\bot_haditha.obj build\bot_ladder.obj build\bot_body.obj build\bot_steer.obj build\bot_recover.obj build\bot_doorway.obj build\bot_path.obj build\bot_weapons.obj build\bot_squad.obj build\bot_think.obj build\insbots.obj build\convar.obj "%SDK%\lib\public\tier0.lib" "%SDK%\lib\public\vstdlib.lib" build\msvcrt.lib kernel32.lib

if %ERRORLEVEL% EQU 0 (
    echo SUCCESS: insbots.dll compiled and built successfully!
) else (
    echo Linking failed!
)
