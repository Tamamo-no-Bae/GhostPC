#!/usr/bin/env bash
# Compiles zoo.map with the OpenNOD SDK into levels/zoo/out/9_1_1a_Enemy_Zoo.{nil,nsd,npd}.
# Same arguments as nodsdk-radiant's "Convert NOD level" (radiant/nodcompile.h). Output stays in this workspace;
# nothing is copied into the game folder.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="${NOD_SDK_DIR:-$HERE/sdk}"   # the OpenNOD SDK's folder (MAPtoNIL, MAPtoNSD, MAPtoNPD)
ASSETS="$(cd "${GHOST_GAME_DIR:-$HERE/../../game}" && pwd)"
MAP="$HERE/zoo.map"
OUT="$HERE/out"
NAME=9_1_1a_Enemy_Zoo
mkdir -p "$OUT"
cd "$HERE"

echo "== MAPtoNIL (world, collision, lighting)"
"$SDK/MAPtoNIL.exe" --world-only --workspace "$HERE" --asset-root "$ASSETS" \
    --lighting auto --light-samples 4 --light-shadows off --light-phong 0 \
    --sun-light 1 0.95 0.85 1 -1 2 -4 --ambient-light 1 1 1 0.35 \
    "$MAP" "$OUT/$NAME.nil" 2>&1 | tee "$OUT/MAPtoNIL.log"

echo "== MAPtoNSD (placements)"
"$SDK/MAPtoNSD.exe" --workspace "$HERE" --asset-root "$ASSETS" "$MAP" "$OUT/$NAME.nsd" 2>&1 | tee "$OUT/MAPtoNSD.log"

echo "== MAPtoNPD (waypoints)"
"$SDK/MAPtoNPD.exe" --workspace "$HERE" --asset-root "$ASSETS" "$MAP" "$OUT/$NAME.npd" 2>&1 | tee "$OUT/MAPtoNPD.log"

ls -la "$OUT"
