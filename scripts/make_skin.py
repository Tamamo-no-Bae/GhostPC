# make_skin.py <name> <suit.png> [<face.png>] -- a Nova reskin from painted textures:
#   3D\Materials\<name>.dds      the suit (NovaReduxFinal's layout, 512x512) with NovaReduxFinal's alpha (the
#                                 gloss / glow mask the suit shaders read), DXT5
#   3D\Materials\<name>Face.dds  the face and hair (NovaFace04's layout, 256x256), opaque like NovaFace04, DXT1
#   3D\Models\<model>.nod        nova.nod with its NovaReduxFinal_* / NovaFace04* materials renamed to <name>_* /
#                                 <name>Face*: the skeleton, mesh and animations stay Nova's
# The materials themselves are in assets\Materials\ghostpc.nsa. Model name: nova_<suffix of name>, e.g. NovaIce ->
# nova_ice.nod.
import os
import sys

from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
GAME = os.environ.get("GHOST_GAME_DIR", os.path.join(ROOT, "game"))
MAT = os.path.join(GAME, "3D", "Materials")
MODELS = os.path.join(GAME, "3D", "Models")


def texture(src, mask_from, size, out, fmt):
    im = Image.open(src).convert("RGB").resize(size, Image.LANCZOS)
    alpha = Image.open(os.path.join(MAT, mask_from + ".dds")).convert("RGBA").getchannel("A").resize(size)
    im = im.convert("RGBA")
    im.putalpha(alpha)
    im.save(out, pixel_format=fmt)
    print("wrote", out, size, fmt)


def model(name, face, out):
    d = bytearray(open(os.path.join(MODELS, "nova.nod"), "rb").read())
    sc = d[4]
    for i in range(sc):
        o = 92 + 32 * i
        n = bytes(d[o:o + 32]).split(b"\0")[0].decode("latin1")
        new = n.replace("NovaReduxFinal", name) if n.startswith("NovaReduxFinal") else (
            n.replace("NovaFace04", name + "Face") if face and n.startswith("NovaFace04") else n)
        assert len(new) < 32, new
        d[o:o + 32] = new.encode("latin1").ljust(32, b"\0")
        if new != n:
            print("  material %s -> %s" % (n, new))
    open(out, "wb").write(bytes(d))
    print("wrote", out)


def main():
    name, suit = sys.argv[1], sys.argv[2]
    face = sys.argv[3] if len(sys.argv) > 3 else None
    texture(suit, "NovaReduxFinal", (512, 512), os.path.join(MAT, name + ".dds"), "DXT5")
    if face:
        texture(face, "NovaFace04", (256, 256), os.path.join(MAT, name + "Face.dds"), "DXT1")
    model(name, face, os.path.join(MODELS, "nova_%s.nod" % name[4:].lower()))


if __name__ == "__main__":
    main()
