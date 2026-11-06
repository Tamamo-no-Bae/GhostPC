# add_vehicle_mission.py -- adds the vehicle test map (9_2_1a_Vehicle_Test, levels/vehicles) to the single-player
# chronicle (Misc\ghostsp.nsc, after the Enemy Zoo) and its name and objective to game.nls, in the game folder and the
# release copy. Safe to run again (nothing is added twice). Then run make_coop_chronicle.py to carry it into co-op.
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
GAME = os.environ.get("GHOST_GAME_DIR", os.path.join(ROOT, "game"))
REL = os.path.join(ROOT, "release", "GhostPC_Patch")
NSC = [os.path.join(GAME, "Misc", "ghostsp.nsc"), os.path.join(REL, "Misc", "ghostsp.nsc")]
NLS = [os.path.join(GAME, "Strings", "game.nls"), os.path.join(GAME, "Misc", "game.nls"),
       os.path.join(REL, "Strings", "game.nls")]

STRINGS = [("9_2_VEHICLES", "Vehicle Test map"),
           ("9_2_OBJ00", "Drive the vehicles around the track.")]
MISSION = """
// GhostPC test level: a walled field with a pavement track around a solid block, for the big vehicles
MISSION 	      9_2_VEHICLES	9_2_1a_Vehicle_Test
LOADSCREEN     921a_MenuImage
LOADTEXT       027_VultureVehicle.nlt
OBJECTIVE      9_2_OBJ00 1
STATS		      100	100   1
AMMO           200 50 150 1000
POWERS		   cloak speed sight
SIGHTFLAGS		interactive critical threat footprint burrowed cloaked
CALLDOWNS	   identify comsat
INVENTORY	   canister	3
INVENTORY	   spidermine	3
INVENTORY	   mantis	3
INVENTORY	   firefly	3
ENDMISSION
"""

for path in NSC:
    if not os.path.exists(path):
        continue
    d = open(path, "rb").read().decode("latin-1")
    if "9_2_VEHICLES" in d:
        print(path + ": already there")
        continue
    eol = "\r\n" if "\r\n" in d else "\n"
    zoo = d.index("9_1_1a_Enemy_Zoo")
    end = d.index("ENDMISSION", zoo) + len("ENDMISSION")
    d = d[:end] + eol + MISSION.rstrip("\n").replace("\n", eol) + d[end:]
    open(path, "wb").write(d.encode("latin-1"))
    print(path + ": mission added after the Enemy Zoo")

for path in NLS:
    if not os.path.exists(path):
        continue
    lines = open(path, "rb").read().split(b"\n")
    keys = {l.split(None, 1)[0] for l in lines if l.strip()}
    anchor = next(i for i, l in enumerate(lines) if l.startswith(b"9_1_OBJ00"))
    cr = b"\r" if lines[anchor].endswith(b"\r") else b""
    new = [(k.ljust(33) + v).encode("latin-1") + cr for k, v in STRINGS if k.encode() not in keys]
    if new:
        lines[anchor + 1:anchor + 1] = new
        open(path, "wb").write(b"\n".join(lines))
    print("%s: %d string(s) added" % (path, len(new)))
