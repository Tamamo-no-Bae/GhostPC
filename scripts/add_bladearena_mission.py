# add_bladearena_mission.py -- adds the Psi Blade arena (8_3_1a_Blade_Arena, levels/bladearena) to the competitive chronicle
# (Misc\ghostmpcomp.nsc, after the Vehicle Duel) and its name and objective to game.nls, in the game folder and the
# release copy. Safe to run again (nothing is added twice).
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
GAME = os.environ.get("GHOST_GAME_DIR", os.path.join(ROOT, "game"))
REL = os.path.join(ROOT, "release", "GhostPC_Patch")
NSC = [os.path.join(GAME, "Misc", "ghostmpcomp.nsc"), os.path.join(REL, "Misc", "ghostmpcomp.nsc")]
NLS = [os.path.join(GAME, "Strings", "game.nls"), os.path.join(GAME, "Misc", "game.nls"),
       os.path.join(REL, "Strings", "game.nls")]

STRINGS = [("8_3_BLADE", "Psi Blade Arena"),
           ("8_3_OBJ00", "Defeat your opponent with the Psi Blade.")]
MISSION = """
// GhostPC: Psi Blade arena, two players, the blade only (8_3_1a_Blade_Arena.gpc: psiblade_start, noweapon for the guns)
MISSION 	      8_3_BLADE	8_3_1a_Blade_Arena
LOADSCREEN     831a_MenuImage
LOADTEXT       033_PsiBlade.nlt
OBJECTIVE      8_3_OBJ00 1
STATS		      100	100   1
AMMO           200 50 150 1000
PSIBLADE       1
POWERS		   cloak speed sight
SIGHTFLAGS		interactive critical threat footprint cloaked
ENDMISSION
"""

for path in NSC:
    if not os.path.exists(path):
        continue
    d = open(path, "rb").read().decode("latin-1")
    if "8_3_BLADE" in d:
        print(path + ": already there")
        continue
    eol = "\r\n" if "\r\n" in d else "\n"
    arena = d.index("8_2_1a_Vehicle_Duel")
    end = d.index("ENDMISSION", arena) + len("ENDMISSION")
    d = d[:end] + eol + MISSION.rstrip("\n").replace("\n", eol) + d[end:]
    open(path, "wb").write(d.encode("latin-1"))
    print(path + ": mission added after the Vehicle Duel")

for path in NLS:
    if not os.path.exists(path):
        continue
    lines = open(path, "rb").read().split(b"\n")
    keys = {l.split(None, 1)[0] for l in lines if l.strip()}
    anchor = next(i for i, l in enumerate(lines) if l.startswith(b"8_2_OBJ00"))
    cr = b"\r" if lines[anchor].endswith(b"\r") else b""
    new = [(k.ljust(33) + v).encode("latin-1") + cr for k, v in STRINGS if k.encode() not in keys]
    if new:
        lines[anchor + 1:anchor + 1] = new
        open(path, "wb").write(b"\n".join(lines))
    print("%s: %d string(s) added" % (path, len(new)))
