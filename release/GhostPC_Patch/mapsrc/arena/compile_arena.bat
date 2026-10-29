@echo off
rem Compiles arena.map with the OpenNOD SDK into levels\arena\out\8_1_1a_Protoss_Arena.{nil,nsd,npd} (same as
rem compile_arena.sh; the SDK as for the Zoo's compile_zoo.bat, or NOD_SDK_DIR). Logs go to out\*.log.
rem Then: python install_arena.py
setlocal
cd /d "%~dp0"
set "SDK=%~dp0..\sdk"
if not "%NOD_SDK_DIR%"=="" set "SDK=%NOD_SDK_DIR%"
if not exist "%SDK%\MAPtoNIL.exe" (
    echo OpenNOD SDK not found in "%SDK%" - set NOD_SDK_DIR to the folder holding MAPtoNIL.exe
    pause
    exit /b 1
)
for %%I in ("%~dp0..\..\game") do set "ASSETS=%%~fI"
set "HERE=%~dp0"
set "HERE=%HERE:~0,-1%"
set NAME=8_1_1a_Protoss_Arena
if not exist out mkdir out

echo == MAPtoNIL (world, collision, lighting)
"%SDK%\MAPtoNIL.exe" --world-only --workspace "%HERE%" --asset-root "%ASSETS%" --lighting auto --light-samples 4 --light-shadows off --light-phong 0 --sun-light 0.75 0.85 1 0.5 -1 2 -4 --ambient-light 0.8 0.85 1 0.4 "%HERE%\arena.map" "%HERE%\out\%NAME%.nil" > out\MAPtoNIL.log 2>&1
type out\MAPtoNIL.log

echo == MAPtoNSD (placements)
"%SDK%\MAPtoNSD.exe" --workspace "%HERE%" --asset-root "%ASSETS%" "%HERE%\arena.map" "%HERE%\out\%NAME%.nsd" > out\MAPtoNSD.log 2>&1
type out\MAPtoNSD.log

echo == MAPtoNPD (waypoints)
"%SDK%\MAPtoNPD.exe" --workspace "%HERE%" --asset-root "%ASSETS%" "%HERE%\arena.map" "%HERE%\out\%NAME%.npd" > out\MAPtoNPD.log 2>&1
type out\MAPtoNPD.log

dir out
pause
