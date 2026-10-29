#!/usr/bin/env bash
# Compiles arena.map with the OpenNOD SDK into levels/arena/out/8_1_1a_Protoss_Arena.{nil,nsd,npd} (same arguments as
# the Zoo's compile_zoo.sh; an indoor level: the "sun" is a dim fill without shadows, the static lights do the rest).
# Then: python install_arena.py
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="${NOD_SDK_DIR:-/c/Users/Tamamo/AppData/Local/Temp/claude/C--Users-Tamamo-Documents-Claude-Work-SCGhost/3f842a37-afa2-462a-9484-fdfdacea9732/scratchpad/sdk/opennod/OpenNOD-SDK-0.6.7-windows-x64}"   # as compile_zoo.bat; NOD_SDK_DIR overrides
ASSETS="$(cd "${GHOST_GAME_DIR:-$HERE/../../game}" && pwd)"
MAP="$HERE/arena.map"
OUT="$HERE/out"
NAME=8_1_1a_Protoss_Arena
mkdir -p "$OUT"
cd "$HERE"

echo "== MAPtoNIL (world, collision, lighting)"
"$SDK/MAPtoNIL.exe" --world-only --workspace "$HERE" --asset-root "$ASSETS" \
    --lighting auto --light-samples 4 --light-shadows off --light-phong 0 \
    --sun-light 0.75 0.85 1 0.5 -1 2 -4 --ambient-light 0.8 0.85 1 0.4 \
    "$MAP" "$OUT/$NAME.nil" 2>&1 | tee "$OUT/MAPtoNIL.log"

echo "== MAPtoNSD (placements)"
"$SDK/MAPtoNSD.exe" --workspace "$HERE" --asset-root "$ASSETS" "$MAP" "$OUT/$NAME.nsd" 2>&1 | tee "$OUT/MAPtoNSD.log"

echo "== MAPtoNPD (waypoints)"
"$SDK/MAPtoNPD.exe" --workspace "$HERE" --asset-root "$ASSETS" "$MAP" "$OUT/$NAME.npd" 2>&1 | tee "$OUT/MAPtoNPD.log"

ls -la "$OUT"
