@echo off
cd /d %~dp0
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
if not exist build mkdir build
cl /nologo /c /O2 /Oy- /MT /EHsc /Zi /W3 /wd4996 /std:c++17 /DNOMINMAX /Fobuild\ /Fdbuild\ghostpc.pdb src\*.cpp
if errorlevel 1 exit /b 1
link /nologo /OUT:build\GhostPC.exe /DEBUG /PDB:build\GhostPC.pdb /MAP:build\GhostPC.map /SUBSYSTEM:WINDOWS ^
  /BASE:0x10000000 /FIXED /DYNAMICBASE:NO /NXCOMPAT:NO /SAFESEH:NO /LARGEADDRESSAWARE ^
  build\*.obj user32.lib gdi32.lib shell32.lib ole32.lib winmm.lib ws2_32.lib dbghelp.lib
if errorlevel 1 exit /b 1
py scripts\checkhooks.py build\GhostPC.map game\Ghost.map
if errorlevel 1 exit /b 1
copy /y build\GhostPC.exe game\ >nul
echo BUILD OK
