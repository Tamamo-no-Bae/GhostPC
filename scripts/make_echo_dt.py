# make_echo_dt.py <darktemplar.png> <suit.png> -- Echo's Dark Templar armour (user's textures), for the Psi Blade in
# co-op (src/psiblade.cpp: Echo, whose own model is nova_ice.nod, becomes this instead of Nova's nova_DT43):
#   3D\Materials\EchoDarkTemplar.dds   NovaDarkTemplar's layout (helmet, straps, cloth, claws), with its alpha
#   3D\Materials\Echo_DT_New.dds       Nova_DT_New's layout (suit, lights, hair, rifle), with its alpha
#                                      (the alpha is the gloss / glow mask the shaders read: the painted PNGs have none)
#   3D\Models\nova_DT43_echo.nod       nova_DT43.nod with its seven materials renamed to Echo's (mesh, skeleton,
#                                      markers and animations stay)
#   3D\Models\nova_DT43_echo_shield.nod  its shield shell (cEffectPsiShield loads "<model>_shield"): nova_DT43's,
#                                      the same mesh
#   Materials\ghostpc.nsa              the seven Echo materials, as Nova's DT ones with Echo's textures
# Written into game\ and assets\ (the tracked copies). DXT5, as the originals.
import os
import re
import shutil
import sys

from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
GAME = os.environ.get("GHOST_GAME_DIR", os.path.join(ROOT, "game"))
OUTS = [GAME, os.path.join(ROOT, "assets")]

RENAME = {   # nova_DT43's materials -> Echo's (shorter than 32 characters)
    "Nova_DT_Armor_gloss": "Echo_DT_Armor_gloss",
    "Nova_DT_Armor_light": "Echo_DT_Armor_light",
    "Nova_DT_New": "Echo_DT_New",
    "Nova_DT_New_full": "Echo_DT_New_full",
    "Nova_DT_gloss": "Echo_DT_gloss",
    "nova_DT_glow": "echo_DT_glow",
    "ghostRifle_unarmed": "echoRifle_unarmed",
}
TEXTURES = {"Nova_DT_New": "Echo_DT_New", "NovaDarkTemplar": "EchoDarkTemplar"}


def texture(src, original, dst):
    orig = Image.open(os.path.join(GAME, "3D", "Materials", original + ".dds")).convert("RGBA")
    im = Image.open(src).convert("RGB").resize(orig.size, Image.LANCZOS).convert("RGBA")
    im.putalpha(orig.getchannel("A"))
    for base in OUTS:
        out = os.path.join(base, "3D", "Materials", dst + ".dds")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        im.save(out, pixel_format="DXT5")
    print("wrote %s.dds %s (alpha from %s)" % (dst, im.size, original))


def model():
    models = os.path.join(GAME, "3D", "Models")
    d = bytearray(open(os.path.join(models, "nova_DT43.nod"), "rb").read())
    for i in range(d[4]):
        o = 92 + 32 * i
        n = bytes(d[o:o + 32]).split(b"\0")[0].decode("latin1")
        new = RENAME.get(n, n)
        assert len(new) < 32
        d[o:o + 32] = new.encode("latin1").ljust(32, b"\0")
        print("  material %s -> %s" % (n, new))
    for base in OUTS:
        dst = os.path.join(base, "3D", "Models")
        os.makedirs(dst, exist_ok=True)
        open(os.path.join(dst, "nova_DT43_echo.nod"), "wb").write(bytes(d))
        shutil.copyfile(os.path.join(models, "nova_DT43_shield.nod"), os.path.join(dst, "nova_DT43_echo_shield.nod"))
    print("wrote nova_DT43_echo.nod, nova_DT43_echo_shield.nod")


def materials():
    for base in OUTS:
        p = os.path.join(base, "Materials", "ghostpc.nsa")
        s = open(p, encoding="latin1", newline="").read()
        if "Echo_DT_New" in s:
            print(p + ": Echo's materials already there")
            continue
        nl = "\r\n" if "\r\n" in s else "\n"
        blocks = []
        for nova, echo in RENAME.items():
            m = re.search(r"(?m)^" + re.escape(nova) + r"\r?\n\{.*?\}", s, re.S)
            assert m, nova
            b = m.group(0)
            b = b.replace(nova, echo, 1)
            for t_nova, t_echo in TEXTURES.items():
                b = re.sub(r"(texture\s+)" + re.escape(t_nova) + r"\b", r"\g<1>" + t_echo, b)
            blocks.append(b)
        head = ["", "// Echo in Dark Templar armour (nova_DT43_echo.nod, scripts\\make_echo_dt.py): Nova's DT materials with Echo's",
                "// textures (EchoDarkTemplar, Echo_DT_New), for the Psi Blade in co-op"]
        s = s.rstrip("\r\n") + nl + nl.join(head) + nl + (nl + nl).join(blocks) + nl
        open(p, "w", encoding="latin1", newline="").write(s)
        print(p + ": %d materials added" % len(blocks))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__.splitlines()[0])
    texture(sys.argv[1], "NovaDarkTemplar", "EchoDarkTemplar")
    texture(sys.argv[2], "Nova_DT_New", "Echo_DT_New")
    model()
    materials()
