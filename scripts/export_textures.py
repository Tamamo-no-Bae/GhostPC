# export_textures.py <out dir> <texture> [...]  (names in game\3D\Materials, without .dds)
# Writes each DDS as <name>_color.png (RGB, opaque: what to paint), <name>_alpha.png (the alpha channel as greyscale:
# for most 3D textures a gloss / glow / mask channel, not transparency) and <name>_rgba.png (both, as stored).
# Many viewers draw the RGBA file over black using that alpha, which makes the textures look almost black.
import os
import sys

from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
GAME = os.environ.get("GHOST_GAME_DIR", os.path.join(ROOT, "game"))


def export(name, out):
    im = Image.open(os.path.join(GAME, "3D", "Materials", name + ".dds")).convert("RGBA")
    os.makedirs(out, exist_ok=True)
    im.convert("RGB").save(os.path.join(out, name + "_color.png"))
    alpha = im.getchannel("A")
    lo, hi = alpha.getextrema()
    if lo < 255:   # a fully opaque texture has no mask worth saving
        alpha.save(os.path.join(out, name + "_alpha.png"))
        im.save(os.path.join(out, name + "_rgba.png"))
    return im.size, (lo, hi)


if __name__ == "__main__":
    out = sys.argv[1]
    for n in sys.argv[2:]:
        size, (lo, hi) = export(n, out)
        print("%-20s %dx%d alpha %d..%d" % (n, size[0], size[1], lo, hi))
