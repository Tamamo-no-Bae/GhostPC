# make_coop_chronicle.py -- Misc\ghostmpcoop.nsc, the co-op chronicle (gameChronicle_LoadMPCOOP), which the build
# lacks: the single-player chronicle (ghostsp.nsc, with GhostPC's additions) with its own save slot (PROFILEID 1:
# co-op progress apart from the campaign's) and game type 1 (GAMETYPE: only cShellMissionSelect reads it). Run again
# when ghostsp.nsc changes; the release takes it from release/GhostPC_Patch/Misc like the other Misc files.
import os
import re

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
GAME = os.environ.get("GHOST_GAME_DIR", os.path.join(ROOT, "game"))
OUTS = [os.path.join(GAME, "Misc"), os.path.join(ROOT, "release", "GhostPC_Patch", "Misc")]

d = open(os.path.join(GAME, "Misc", "ghostsp.nsc"), "rb").read()
d = d.replace(b"GHOST SINGLE PLAYER CHRONICLE FILE", b"GHOST CO-OP CHRONICLE FILE (GhostPC: made from ghostsp.nsc)", 1)
d, n1 = re.subn(rb"(?m)^(PROFILEID\s+)0", rb"\g<1>1", d, count=1)
d, n2 = re.subn(rb"(?m)^(GAMETYPE\s+)0", rb"\g<1>1", d, count=1)
assert n1 == 1 and n2 == 1, "PROFILEID / GAMETYPE not found"
for o in OUTS:
    if os.path.isdir(o):
        open(os.path.join(o, "ghostmpcoop.nsc"), "wb").write(d)
        print("wrote", os.path.join(o, "ghostmpcoop.nsc"))
