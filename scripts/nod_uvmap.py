# nod_uvmap.py model.nod out.png tex1 [tex2 ...] : each material's LOD-0 triangles drawn in UV space over each texture
# (rows: materials, columns: textures) -- shows which texture a material's UVs were made for.
# NOD: tModelFile header (+0x24 vertex sets as (type, count), +0x44 index count); 56-byte mesh groups hold 4 LODs of
# (strip count, list count, vertex count) + the group's vertex count; indices and vertices follow group by group.
import os
import struct
import sys

from PIL import Image, ImageDraw

G = os.environ.get("GHOST_GAME_DIR", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "game"))
STRIDE = {0: 32, 1: 36, 2: 48}


def load(name):
    d = open(os.path.join(G, "3D", "Models", name), "rb").read()
    sc, bc, vsc, mgc = d[4], d[5], d[6], d[7]
    shaders = [d[92 + 32 * i:92 + 32 * i + 32].split(b"\0")[0].decode("latin1") for i in range(sc)]
    sets = [struct.unpack_from("<II", d, 0x24 + 8 * i) for i in range(vsc)]   # (type, count)
    ni = struct.unpack_from("<I", d, 0x44)[0]
    o = 92 + 32 * sc + 64 * bc
    uv = []
    for t, c in sets:
        s = STRIDE[t]
        uv.append([struct.unpack_from("<2f", d, o + s * i + 24) for i in range(c)])
        o += s * c
    idx = struct.unpack_from("<%dH" % ni, d, o)
    o += 2 * ni
    tris = {}
    ipos, vbase = 0, [0] * vsc
    for g in range(mgc):
        r = o + 56 * g
        sh, vs = struct.unpack_from("<I", d, r)[0], d[r + 0x36]
        w = struct.unpack_from("<13H", d, r + 4)
        for lod in range(4):
            ns, nl = w[3 * lod], w[3 * lod + 1]
            if lod == 0:
                strip, lst = idx[ipos:ipos + ns], idx[ipos + ns:ipos + ns + nl]
                tl = tris.setdefault(sh, [])
                for i in range(len(strip) - 2):
                    a, b, c = strip[i], strip[i + 1], strip[i + 2]
                    if a != b and b != c and a != c:
                        tl.append((vs, vbase[vs], a, b, c))
                for i in range(0, len(lst) - 2, 3):
                    tl.append((vs, vbase[vs], lst[i], lst[i + 1], lst[i + 2]))
            ipos += ns + nl
        vbase[vs] += w[12]
    return shaders, uv, tris


def main():
    model, out, texs = sys.argv[1], sys.argv[2], sys.argv[3:]
    shaders, uv, tris = load(model)
    S = 256
    im = Image.new("RGB", (S * len(texs), S * len(shaders)), "black")
    dr = ImageDraw.Draw(im)
    for r, name in enumerate(shaders):
        for c, t in enumerate(texs):
            im.paste(Image.open(os.path.join(G, "3D", "Materials", t + ".dds")).convert("RGB").resize((S, S)), (c * S, r * S))
            for vs, base, a, b, cc in tris.get(r, []):
                dr.polygon([(c * S + (uv[vs][base + k][0] % 1.0) * S, r * S + (uv[vs][base + k][1] % 1.0) * S)
                            for k in (a, b, cc)], outline=(255, 0, 255))
            dr.text((c * S + 3, r * S + 3), "%s / %s" % (name, t), fill=(255, 255, 0))
    im.save(out)
    print({shaders[k]: len(v) for k, v in tris.items()})


if __name__ == "__main__":
    main()
