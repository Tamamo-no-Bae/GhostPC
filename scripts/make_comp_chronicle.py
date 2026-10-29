# make_comp_chronicle.py -- Misc\ghostmpcomp.nsc, the competitive chronicle (gameChronicle_LoadMPCOMP), which the
# build lacks: the competitive levels only (GhostPC's Protoss arena), its own save slot (PROFILEID 2) and game type 2
# (cShellMissionSelect; GhostPC's CompetitiveRevive and IsMissionUnlocked read it too). Also adds the arena's mission
# name and objective to game.nls (after the Zoo's lines, as make_coop_chronicle leaves the strings alone). Run again
# after editing; the release takes the files from release/GhostPC_Patch/Misc and /Strings like the other patch files.
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
GAME = os.environ.get("GHOST_GAME_DIR", os.path.join(ROOT, "game"))
REL = os.path.join(ROOT, "release", "GhostPC_Patch")
MISC_OUTS = [os.path.join(GAME, "Misc"), os.path.join(REL, "Misc")]
NLS = [os.path.join(GAME, "Strings", "game.nls"), os.path.join(GAME, "Misc", "game.nls"),
       os.path.join(REL, "Strings", "game.nls")]

STRINGS = [("8_1_ARENA", "Protoss Arena Test"),
           ("8_1_OBJ00", "Defeat your opponent."),
           ("MNU_SEL_PLAYERS", "Number of Players")]

# the single-player chronicle's header (format notes) up to its IDENTIFICATION block
sp = open(os.path.join(GAME, "Misc", "ghostsp.nsc"), "rb").read().decode("latin-1")
header = sp[:sp.index("// IDENTIFICATION")]
header = header.replace("GHOST SINGLE PLAYER CHRONICLE FILE",
                        "GHOST COMPETITIVE CHRONICLE FILE (GhostPC)", 1)
body = """// IDENTIFICATION
// Specifies which slot this chronicle saves its data in, and what game type this chronicle is designed for
//

PROFILEID		2
GAMETYPE		   2

// MISSIONS
//

// GhostPC: symmetric Protoss arena, two players, random respawn points (8_1_1a_Protoss_Arena.gpc)
MISSION 	      8_1_ARENA	8_1_1a_Protoss_Arena
LOADSCREEN     811a_MenuImage
LOADTEXT       090_HealthPowerup.nlt
OBJECTIVE      8_1_OBJ00 1
STATS		      100	100   1
AMMO           200 50 150 1000
PSIBLADE       1
POWERS		   cloak speed sight
SIGHTFLAGS		interactive critical threat footprint cloaked
INVENTORY	   canister	3
ENDMISSION
"""
data = (header + body).replace("\r\n", "\n")
eol = "\r\n" if "\r\n" in sp else "\n"
data = data.replace("\n", eol).encode("latin-1")
for o in MISC_OUTS:
    if os.path.isdir(o):
        open(os.path.join(o, "ghostmpcomp.nsc"), "wb").write(data)
        print("wrote", os.path.join(o, "ghostmpcomp.nsc"))

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
