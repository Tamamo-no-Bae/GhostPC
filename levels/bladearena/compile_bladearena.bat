@echo off
rem Compiles bladearena.map with the OpenNOD SDK into levels\bladearena\out\8_3_1a_Blade_Arena.{nil,nsd,npd} (the SDK in levels\sdk, or NOD_SDK_DIR).
rem Logs go to out\*.log. Nothing is copied into the game folder.
setlocal
cd /d "%~dp0"
set "SDK=%~dp0..\sdk"
if not "%NOD_SDK_DIR%"=="" set "SDK=%NOD_SDK_DIR%"
for %%I in ("%~dp0..\..\game") do set "ASSETS=%%~fI"
set "HERE=%~dp0"
set "HERE=%HERE:~0,-1%"
set NAME=8_3_1a_Blade_Arena
if not exist out mkdir out

echo == MAPtoNIL (world, collision, lighting)
"%SDK%\MAPtoNIL.exe" --world-only --workspace "%HERE%" --asset-root "%ASSETS%" --lighting auto --light-samples 4 --light-shadows off --light-phong 0 --sun-light 1 0.95 0.85 1.6 -1 2 -4 --ambient-light 1 1 1 0.55 "%HERE%\bladearena.map" "%HERE%\out\%NAME%.nil" > out\MAPtoNIL.log 2>&1
type out\MAPtoNIL.log

echo == MAPtoNSD (placements)
"%SDK%\MAPtoNSD.exe" --workspace "%HERE%" --asset-root "%ASSETS%" "%HERE%\bladearena.map" "%HERE%\out\%NAME%.nsd" > out\MAPtoNSD.log 2>&1
type out\MAPtoNSD.log

echo == MAPtoNPD (waypoints)
"%SDK%\MAPtoNPD.exe" --workspace "%HERE%" --asset-root "%ASSETS%" "%HERE%\bladearena.map" "%HERE%\out\%NAME%.npd" > out\MAPtoNPD.log 2>&1
type out\MAPtoNPD.log

dir out
pause
