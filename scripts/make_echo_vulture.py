# make_echo_vulture.py -- Echo on the Vulture: the player Vulture's model, novaVulture.nod, has Nova riding it (her
# suit's materials NovaReduxFinal_gloss / _glo / _Lights / _hot), so Echo (player 2, nova_ice.nod) rode it as Nova
# (user). Writes, into game\ and assets\:
#   3D\Models\novaVulture_echo.nod   novaVulture.nod with those four materials renamed to Echo's (NovaIce_*); vehicles.cpp
#                                    puts it on a Vulture while Echo rides it
#   Materials\ghostpc.nsa            NovaIce_hot (NovaReduxFinal_hot, her head marked hot in sight mode, on her texture
#                                    NovaIce); the other three exist already (make_skin.py)
# Safe to run again.
import os
import re

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
GAME = os.environ.get("GHOST_GAME_DIR", os.path.join(ROOT, "game"))
OUTS = [GAME, os.path.join(ROOT, "assets")]
RENAME = {"NovaReduxFinal_gloss": "NovaIce_gloss", "NovaReduxFinal_glo": "NovaIce_glo",
          "NovaReduxFinal_Lights": "NovaIce_Lights", "NovaReduxFinal_hot": "NovaIce_hot"}
HOT = """NovaIce_hot
{
; Echo's head marked hot in sight mode (NovaReduxFinal_hot on her texture; novaVulture_echo.nod, make_echo_vulture.py)
	shader normal
	meshType 7
	texture NovaIce
	sightForceHot
	sound creep
}"""

d = bytearray(open(os.path.join(GAME, "3D", "Models", "novaVulture.nod"), "rb").read())
for i in range(d[4]):
    o = 92 + 32 * i
    n = bytes(d[o:o + 32]).split(b"\0")[0].decode("latin1")
    d[o:o + 32] = RENAME.get(n, n).encode("latin1").ljust(32, b"\0")
    print("  material %s -> %s" % (n, RENAME.get(n, n)))
for base in OUTS:
    out = os.path.join(base, "3D", "Models")
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, "novaVulture_echo.nod"), "wb").write(bytes(d))
    p = os.path.join(base, "Materials", "ghostpc.nsa")
    s = open(p, encoding="latin1", newline="").read()
    if re.search(r"(?m)^NovaIce_hot\s*$", s):
        print(p + ": NovaIce_hot already there")
        continue
    nl = "\r\n" if "\r\n" in s else "\n"
    s = s.rstrip("\r\n") + nl + nl + HOT.replace("\n", nl) + nl
    open(p, "w", encoding="latin1", newline="").write(s)
    print(p + ": NovaIce_hot added")
print("wrote novaVulture_echo.nod")
