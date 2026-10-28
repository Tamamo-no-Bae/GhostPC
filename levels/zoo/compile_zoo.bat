@echo off
rem Compiles zoo.map with the OpenNOD SDK into levels\zoo\out\9_1_1a_Enemy_Zoo.{nil,nsd,npd} (same as compile_zoo.sh).
rem Logs go to out\*.log. Nothing is copied into the game folder.
setlocal
cd /d "%~dp0"
set "SDK=C:\Users\Tamamo\AppData\Local\Temp\claude\C--Users-Tamamo-Documents-Claude-Work-SCGhost\3f842a37-afa2-462a-9484-fdfdacea9732\scratchpad\sdk\opennod\OpenNOD-SDK-0.6.7-windows-x64"
if not "%NOD_SDK_DIR%"=="" set "SDK=%NOD_SDK_DIR%"
for %%I in ("%~dp0..\..\game") do set "ASSETS=%%~fI"
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
