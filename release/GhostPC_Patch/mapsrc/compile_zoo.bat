@echo off
rem Compiles zoo.map with the OpenNOD SDK into mapsrc\out\9_1_1a_Enemy_Zoo.{nil,nsd,npd}; install_zoo.py then
rem installs them. The SDK: set NOD_SDK_DIR to the folder with MAPtoNIL.exe / MAPtoNSD.exe / MAPtoNPD.exe, or put
rem them in mapsrc\sdk. The game folder: GHOST_GAME_DIR, else the folder above mapsrc.
setlocal
cd /d "%~dp0"
set "SDK=%~dp0sdk"
if not "%NOD_SDK_DIR%"=="" set "SDK=%NOD_SDK_DIR%"
for %%I in ("%~dp0..") do set "ASSETS=%%~fI"
if not "%GHOST_GAME_DIR%"=="" set "ASSETS=%GHOST_GAME_DIR%"
if not exist "%SDK%\MAPtoNIL.exe" (echo OpenNOD SDK not found in "%SDK%": set NOD_SDK_DIR & pause & exit /b 1)
set "HERE=%~dp0"
set "HERE=%HERE:~0,-1%"
set NAME=9_1_1a_Enemy_Zoo
if not exist out mkdir out

echo == MAPtoNIL (world, collision, lighting)
"%SDK%\MAPtoNIL.exe" --world-only --workspace "%HERE%" --asset-root "%ASSETS%" --lighting auto --light-samples 4 --light-shadows off --light-phong 0 --sun-light 1 0.95 0.85 1 -1 2 -4 --ambient-light 1 1 1 0.35 "%HERE%\zoo.map" "%HERE%\out\%NAME%.nil" > out\MAPtoNIL.log 2>&1
type out\MAPtoNIL.log

echo == MAPtoNSD (placements)
"%SDK%\MAPtoNSD.exe" --workspace "%HERE%" --asset-root "%ASSETS%" "%HERE%\zoo.map" "%HERE%\out\%NAME%.nsd" > out\MAPtoNSD.log 2>&1
type out\MAPtoNSD.log

echo == MAPtoNPD (waypoints)
"%SDK%\MAPtoNPD.exe" --workspace "%HERE%" --asset-root "%ASSETS%" "%HERE%\zoo.map" "%HERE%\out\%NAME%.npd" > out\MAPtoNPD.log 2>&1
type out\MAPtoNPD.log

dir out
pause
