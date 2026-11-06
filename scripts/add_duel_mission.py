# add_duel_mission.py -- adds the Vehicle Duel (8_2_1a_Vehicle_Duel, levels/duel) to the competitive chronicle
# (Misc\ghostmpcomp.nsc, after the Protoss Arena) and its name and objective to game.nls, in the game folder and the
# release copy. Safe to run again (nothing is added twice).
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
GAME = os.environ.get("GHOST_GAME_DIR", os.path.join(ROOT, "game"))
REL = os.path.join(ROOT, "release", "GhostPC_Patch")
NSC = [os.path.join(GAME, "Misc", "ghostmpcomp.nsc"), os.path.join(REL, "Misc", "ghostmpcomp.nsc")]
NLS = [os.path.join(GAME, "Strings", "game.nls"), os.path.join(GAME, "Misc", "game.nls"),
       os.path.join(REL, "Strings", "game.nls")]

STRINGS = [("8_2_DUEL", "Vehicle Duel"),
           ("8_2_OBJ00", "Defeat your opponent.")]
MISSION = """
// GhostPC: vehicle duel, two players, a Vulture and a Dragoon each and one Goliath in the middle (8_2_1a_Vehicle_Duel.gpc)
MISSION 	      8_2_DUEL	8_2_1a_Vehicle_Duel
LOADSCREEN     821a_MenuImage
LOADTEXT       027_VultureVehicle.nlt
OBJECTIVE      8_2_OBJ00 1
STATS		      100	100   1
AMMO           200 50 150 1000
POWERS		   cloak speed sight
SIGHTFLAGS		interactive critical threat footprint cloaked
INVENTORY	   canister	3
ENDMISSION
"""

for path in NSC:
    if not os.path.exists(path):
        continue
    d = open(path, "rb").read().decode("latin-1")
    if "8_2_DUEL" in d:
        print(path + ": already there")
        continue
    eol = "\r\n" if "\r\n" in d else "\n"
    arena = d.index("8_1_1a_Protoss_Arena")
    end = d.index("ENDMISSION", arena) + len("ENDMISSION")
    d = d[:end] + eol + MISSION.rstrip("\n").replace("\n", eol) + d[end:]
    open(path, "wb").write(d.encode("latin-1"))
    print(path + ": mission added after the Protoss Arena")

for path in NLS:
    if not os.path.exists(path):
        continue
    lines = open(path, "rb").read().split(b"\n")
    keys = {l.split(None, 1)[0] for l in lines if l.strip()}
    anchor = next(i for i, l in enumerate(lines) if l.startswith(b"8_1_OBJ00"))
    cr = b"\r" if lines[anchor].endswith(b"\r") else b""
    new = [(k.ljust(33) + v).encode("latin-1") + cr for k, v in STRINGS if k.encode() not in keys]
    if new:
        lines[anchor + 1:anchor + 1] = new
        open(path, "wb").write(b"\n".join(lines))
    print("%s: %d string(s) added" % (path, len(new)))
