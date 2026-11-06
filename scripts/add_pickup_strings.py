# add_pickup_strings.py -- the pickup messages for GhostPC's weapon pickups (user: the flamethrower pickup said "Flame
# Ammo"; the Psi Blade should say its name too). cEffectManager::AddPickupFX shows the string named by its first
# argument as the minimap hint; the flamethrower's pickup passed POWFTHROWER ("Flame Ammo", the ammo pickup's).
# Adds after POWFTHROWER in Strings\game.nls (game\ and the release copy; the build reads Strings\, Misc\game.nls is
# the original and stays as it is):
#   POWFLAMETHROWER   Flamethrower   (powFlamethrower's DESCRIPTIONID, and src/vehicles.cpp FlameAmmoActivate)
#   POWPSIBLADE       Psi Blade      (powPsiBlade's DESCRIPTIONID, and FlameAmmoActivate's Psi Blade branch)
# Safe to run again.
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
GAME = os.environ.get("GHOST_GAME_DIR", os.path.join(ROOT, "game"))
NLS = [os.path.join(GAME, "Strings", "game.nls"), os.path.join(ROOT, "release", "GhostPC_Patch", "Strings", "game.nls")]
STRINGS = [("POWFLAMETHROWER", "Flamethrower"), ("POWPSIBLADE", "Psi Blade")]

for path in NLS:
    if not os.path.exists(path):
        continue
    lines = open(path, "rb").read().split(b"\n")
    keys = {l.split(None, 1)[0] for l in lines if l.strip()}
    anchor = next(i for i, l in enumerate(lines) if l.startswith(b"POWFTHROWER"))
    cr = b"\r" if lines[anchor].endswith(b"\r") else b""
    new = [(k.ljust(33) + v).encode("latin-1") + cr for k, v in STRINGS if k.encode() not in keys]
    if new:
        lines[anchor + 1:anchor + 1] = new
        open(path, "wb").write(b"\n".join(lines))
    print("%s: %d line(s) added" % (path, len(new)))
