# make_flame_pickup.py -- the flamethrower pickup's model (user: as the spider mine pickup is a small spider mine on the
# ground, the flamethrower pickup should be the rifle). Writes 3D\Models\PU_FlameRifle.nod (game\ and assets\) from
# the build's gaussRifle_1.nod, the rifle as Nova holds it (her 43-bone skeleton, at hand height):
#   - its rigid vertex set (set 0, type 0: model-space rifle, bone 0) and the three groups drawn from it (rifle body,
#     strap and lights); the skinned group 3 (set 1, a bit of her glove) and its indices are left out
#   - one bone, the root at the origin, as the pickup models have (PU_SpiderMine.nod); the groups' one-bone palettes
#     (+0x21) point at it instead of Nova's hand (28)
#   - levelled (see below), centred on the origin across the ground, its lowest point on it
#   - a still motion set, PU_still.nms (PU_generic.nms spins pickups)
#   - no markers (the rifle's muzzle / hand markers hang on Nova's bones)
# make_ghostpc_templates.py gives powFlamethrower this model. Safe to run again.
import math
import os
import struct

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
GAME = os.environ.get("GHOST_GAME_DIR", os.path.join(ROOT, "game"))
SRC = os.path.join(GAME, "3D", "Models", "gaussRifle_1.nod")
OUT = "PU_FlameRifle.nod"
VSIZE = {0: 32, 1: 36, 2: 48}
LIFT = 0.0

d = open(SRC, "rb").read()
assert struct.unpack_from("<I", d, 0)[0] == 10, "not a version 10 model"
nmat, nbones, nsets, ngroups = d[4], d[5], d[6], d[7]
sets = [struct.unpack_from("<II", d, 0x24 + 8 * i) for i in range(nsets)]
nidx = struct.unpack_from("<I", d, 0x44)[0]
assert sets[0][0] == 0, "gaussRifle_1.nod: set 0 is not the rigid set"
mats = d[92:92 + 32 * nmat]
o = 92 + 32 * nmat
bone0 = bytearray(d[o:o + 64])
o += 64 * nbones
vstart = []
for typ, cnt in sets:
    vstart.append(o)
    o += VSIZE[typ] * cnt
istart = o
gstart = istart + 2 * nidx
groups = [d[gstart + 56 * g:gstart + 56 * g + 56] for g in range(ngroups)]

# keep the groups on set 0; their indices come first (indices are stored group by group, LOD by LOD)
keep, nkeep = [], 0
for g in groups:
    lods = struct.unpack_from("<12H", g, 4)
    n = sum(lods[3 * l] + lods[3 * l + 1] for l in range(4))
    if g[0x36] != 0:
        break   # (the skinned group is last; anything after it would need its indices skipped)
    g = bytearray(g)
    g[0x21] = 0   # its bone palette: Nova's hand (28) in the rifle, the root here (the only bone; it drew garbage, user)
    keep.append(bytes(g))
    nkeep += n
assert len(keep) == 3, "expected three rigid groups, found %d" % len(keep)

nv = sets[0][1]
verts = bytearray(d[vstart[0]:vstart[0] + 32 * nv])
pos = [struct.unpack_from("<3f", verts, 32 * i) for i in range(nv)]


# Level (user: it lay at an angle, one end off the floor): as held, the rifle is pitched about 14.5 degrees and
# rolled a little. The pitch (about y) and roll (about x) that make it lowest, in half degrees, turn its positions and
# normals so it lies flat.
def turn(v, pitch, roll):
    cp, sp, cr, sr = math.cos(pitch), math.sin(pitch), math.cos(roll), math.sin(roll)
    x, y, z = v
    x1, z1 = x * cp + z * sp, -x * sp + z * cp
    return (x1, y * cr - z1 * sr, y * sr + z1 * cr)


def height(pitch, roll):
    zs = [turn(p, pitch, roll)[2] for p in pos]
    return max(zs) - min(zs)


_, pitch, roll = min((height(math.radians(a / 2), math.radians(b / 2)), math.radians(a / 2), math.radians(b / 2))
                     for a in range(-60, 61) for b in range(-60, 61))
for i in range(nv):
    struct.pack_into("<3f", verts, 32 * i, *turn(pos[i], pitch, roll))
    struct.pack_into("<3f", verts, 32 * i + 12, *turn(struct.unpack_from("<3f", verts, 32 * i + 12), pitch, roll))
pos = [struct.unpack_from("<3f", verts, 32 * i) for i in range(nv)]
print("levelled: pitch %.1f, roll %.1f degrees" % (math.degrees(pitch), math.degrees(roll)))
lo = [min(p[k] for p in pos) for k in range(3)]
hi = [max(p[k] for p in pos) for k in range(3)]
shift = (-(lo[0] + hi[0]) / 2, -(lo[1] + hi[1]) / 2, -lo[2] + LIFT)
for i, p in enumerate(pos):
    struct.pack_into("<3f", verts, 32 * i, p[0] + shift[0], p[1] + shift[1], p[2] + shift[2])
bounds = [lo[k] + shift[k] for k in range(3)] + [hi[k] + shift[k] for k in range(3)]

struct.pack_into("<12f", bone0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1)
bone0[60:64] = b"\xff\xff\xff\x00"   # no sibling, child or parent

head = bytearray(92)
struct.pack_into("<I", head, 0, 10)
head[4:8] = bytes([nmat, 1, 1, len(keep)])
struct.pack_into("<6f", head, 0x0c, *bounds)
struct.pack_into("<II", head, 0x24, 0, nv)
struct.pack_into("<I", head, 0x44, nkeep)
body = bytes(head) + mats + bytes(bone0) + bytes(verts) + d[istart:istart + 2 * nkeep] + b"".join(keep)
body += struct.pack("<II", 0, 0)   # (no markers)

# Lying still (user: it spun): the pickups' motion set PU_generic.nms turns them (stand = PU_spin.nad). The rifle gets
# PU_still.nms, whose stand is PU_still.nad: one frame, no tracks (the format of szlachta_bindpose.nad).
STILL_NAD = struct.pack("<IIIfII", 4, 0, 0, 1.0, 0, 0)
STILL_NMS = ("; PU_still.nms -- GhostPC: the flamethrower pickup (PU_FlameRifle.nod) lies still; PU_generic.nms spins\r\n"
             "; (scripts/make_flame_pickup.py)\r\n\r\nstand\t\t\t\tPU_still.nad\t\tlowpriority\r\n")

for base in (GAME, os.path.join(ROOT, "assets")):
    out = os.path.join(base, "3D", "Models")
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, OUT), "wb").write(body)
    for sub, name, data in (("Anims", "PU_still.nad", STILL_NAD), ("Motions", "PU_still.nms", STILL_NMS.encode("latin-1"))):
        os.makedirs(os.path.join(base, "3D", sub), exist_ok=True)
        open(os.path.join(base, "3D", sub, name), "wb").write(data)
print("wrote %s: %d vertices, %d indices, %.2f x %.2f x %.2f m" % (OUT, nv, nkeep, hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]))
