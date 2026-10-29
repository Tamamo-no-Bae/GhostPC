"""Generates the Quake III MAP for the competitive Protoss arena (level 8_1_1a_Protoss_Arena).

Layout (game units = metres, X east, Y north, Z up; north is up), mirror-symmetric about y = 0:

                                  +------+
                                  | north|  side room: spawn (player 1), ammo, health, psi, powerups
                                  | room |
          +---+-----------+-------+-  -  +-------+-----------+
          |   |side corr. +                                  |
          | L +===========+        []            []          |
    +-----+ a |                                              |
    |left | r |  (solid)            central arena            |
    |room   g |                     4 pillars [] ,           |
    +-----+ e |                     pickups scattered        |
          |   +===========+        []            []          |
          |   |side corr. +                                  |
          +---+-----------+-------+-  -  +-------+-----------+
                                  | south|  side room: spawn (player 2)
                                  | room |
                                  +------+

The world origin is the arena's centre (open floor: the game creates the players there before the level script moves
them to their spawns, and spawning inside collision gave NaN physics on the Zoo). Protoss (Aiur, AR_*) textures; only
plain DDS textures (MAPtoNIL 0.6.7 rejects names that are also .nsa materials).

Run:  python make_arena_map.py   ->  writes arena.map and out_levelscript.gpc / out_effects.nlfx next to this script.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "arena.map")
GAME = os.environ.get("GHOST_GAME_DIR") or next(
    (g for g in (os.path.join(HERE, "..", "..", "game"), os.path.join(HERE, ".."), os.path.join(HERE, "..", ".."))
     if os.path.exists(os.path.join(g, "Misc"))),   # GhostPC workspace, or the patch's mapsrc/arena in the game folder
    os.path.join(HERE, ".."))
os.environ.setdefault("GHOST_GAME_DIR", GAME)   # for notdump
sys.path.insert(0, os.path.join(HERE, "..", "..", "scripts"))
sys.path.insert(0, os.path.join(HERE, ".."))   # mapsrc/notdump.py in the patch
sys.path.insert(0, HERE)
import notdump  # noqa: E402  (Global.not reader)

T = 1.0                  # wall thickness
H_ARENA, H_ROOM = 10.0, 5.0   # ceiling heights: the arena, everything else
DOOR_H = 4.0             # height of the openings between areas
OFFSET = (0.0, 0.0, -0.3)     # floors 0.3 m below the origin, as on the Zoo

# interior extents (x0, y0, x1, y1)
ARENA = (-17, -18, 17, 18)
PILLARS = [(cx - 3.5, cy - 3.5, cx + 3.5, cy + 3.5) for cx in (-9, 9) for cy in (-9, 9)]
NORTH = (-4, 18, 4, 27)
SOUTH = (-4, -27, 4, -18)
CORR_N = (-21.5, 13, -17, 17)      # side corridors: arena west wall -> large corridor
CORR_S = (-21.5, -17, -17, -13)
BIG = (-26.5, -17, -21.5, 17)      # the large corridor
LEFT = (-35, -4.5, -26.5, 4.5)     # the room both corridors lead to
ROOM_DOOR = 6.0                    # width of the side rooms' openings into the arena (rooms are 8 m wide)
LEFT_DOOR = 4.0                    # large corridor -> left room

MAT = {
    "floor_arena": "AR_FloorSwirly_blu_01", "wall_arena": "AR_Plated_blu_gld_01",
    "pillar": "AR_Decor_Vert_Blu_01",
    "floor_room": "AR_Plated_Tiles_blu", "wall_room": "AR_Striped_blu_gld_01",
    "floor_corr": "AR_Groovy_blu_01", "wall_corr": "AR_Hall_gld_01", "wall_corr2": "AR_Hall_gld_02",
    "ceiling": "AR_Pattern_HComb_gld",
}
TILE = 4.0   # metres per texture repeat


def dds_width(name):
    """texture width from its DDS header, searched like the game does (MaterialDirs.txt)"""
    for d in open(os.path.join(GAME, "MaterialDirs.txt"), encoding="latin1").read().split():
        f = os.path.join(GAME, *d.split(chr(92)), name + ".dds")
        if os.path.exists(f):
            with open(f, "rb") as h:
                hdr = h.read(20)
            return int.from_bytes(hdr[16:20], "little")
    raise SystemExit("texture not found: " + name)


TEX_SCALE = {m: TILE / dds_width(m) for m in MAT.values()}


def dds_size(name):
    """(width, height) from the DDS header"""
    for d in open(os.path.join(GAME, "MaterialDirs.txt"), encoding="latin1").read().split():
        f = os.path.join(GAME, *d.split(chr(92)), name + ".dds")
        if os.path.exists(f):
            with open(f, "rb") as h:
                hdr = h.read(20)
            return int.from_bytes(hdr[16:20], "little"), int.from_bytes(hdr[12:16], "little")
    raise SystemExit("texture not found: " + name)


class Fitted:
    """A brush material fitted to its faces instead of tiled from the world origin: per side face kind ("x": the
    faces facing +-x, whose texture runs along y; "y": facing +-y, along x) a texture placed so that one repeat is
    w x h metres, starting at u0 along the face and with its top edge at height vtop (Quake standard projection:
    s = u / scale + offset, t = -z / scale + offset). Top and bottom faces keep the plain material."""
    def __init__(self, name, **faces):
        self.name, self.faces = name, faces   # faces: kind -> (material, u0, vtop, w, h)

    def __repr__(self):
        return "Fitted(%s)" % self.name


def mat_name(m):
    return m.name if isinstance(m, Fitted) else m

# ---------------------------------------------------------------------------------------------------------------
# Geometry
brushes = []    # (x0, y0, z0, x1, y1, z1, material)


def box(x0, y0, z0, x1, y1, z1, mat):
    if x1 - x0 > 1e-6 and y1 - y0 > 1e-6 and z1 - z0 > 1e-6:
        brushes.append((x0, y0, z0, x1, y1, z1, mat))


PANEL = 4.0   # target width of a paired-texture panel (AR_Hall_gld_01 | _02: the two halves of one arch)


def wall(x0, y0, x1, y1, mat, height, gaps=(), vis=None, pair=None, room_h=None):
    """Wall box (thin along one axis) from the floor up to height, with openings (centre, width) along its long axis;
    a lintel closes each above DOOR_H. With pair (two materials) and vis (the visible span along the wall, without
    the ends hidden in the walls it meets), each piece is cut into an even number of panels, about PANEL wide, that
    alternate the two materials, each texture filling one panel from the floor to the ceiling (room_h)."""
    along_x = (x1 - x0) >= (y1 - y0)
    lo, hi = (x0, x1) if along_x else (y0, y1)
    pieces, cur = [], lo
    for c, w in sorted(gaps):
        a, b = c - w / 2, c + w / 2
        assert lo <= a and b <= hi, "opening outside its wall"
        pieces += [(cur, a, 0.0), (a, b, DOOR_H)]
        cur = b
    pieces.append((cur, hi, 0.0))
    for s0, s1, z0 in pieces:
        if s1 - s0 < 1e-6:
            continue
        v0, v1 = (max(s0, vis[0]), min(s1, vis[1])) if pair else (s0, s1)
        if not pair or v1 - v0 < 1e-6:
            segs = [(s0, s1, mat)]
        else:
            n = max(2, 2 * round((v1 - v0) / (2 * PANEL)))
            w = (v1 - v0) / n
            cuts = [v0 + w * i for i in range(n + 1)]
            cuts[0], cuts[-1] = s0, s1   # the end panels reach into the hidden wall ends
            kind = "y" if along_x else "x"
            segs = [(cuts[i], cuts[i + 1], Fitted(pair[0], **{kind: (pair[i % 2], v0 + w * i, room_h, w, room_h)}))
                    for i in range(n)]
        for a, b, m in segs:
            if along_x:
                box(a, y0, z0, b, y1, height, m)
            else:
                box(x0, a, z0, x1, b, height, m)


def area(r, floor_mat, ceil_h):
    """floor slab and ceiling slab of an area (reaching under / over its walls)"""
    box(r[0] - T, r[1] - T, -1, r[2] + T, r[3] + T, 0, floor_mat)
    box(r[0] - T, r[1] - T, ceil_h, r[2] + T, r[3] + T, ceil_h + 1, MAT["ceiling"])


area(ARENA, MAT["floor_arena"], H_ARENA)
for r in (NORTH, SOUTH, LEFT):
    area(r, MAT["floor_room"], H_ROOM)
for r in (CORR_N, CORR_S, BIG):
    area(r, MAT["floor_corr"], H_ROOM)

A, R, C = MAT["wall_arena"], MAT["wall_room"], MAT["wall_corr"]
HALL = dict(pair=(MAT["wall_corr"], MAT["wall_corr2"]), room_h=H_ROOM)   # the corridors' paired arch panels
ax0, ay0, ax1, ay1 = ARENA
# arena: north / south walls open into the side rooms, the west wall into the side corridors, the east wall closed
wall(ax0 - T, ay1, ax1 + T, ay1 + T, A, H_ARENA + 1, gaps=((0.0, ROOM_DOOR),))
wall(ax0 - T, ay0 - T, ax1 + T, ay0, A, H_ARENA + 1, gaps=((0.0, ROOM_DOOR),))
wall(ax1, ay0, ax1 + T, ay1, A, H_ARENA + 1)
wall(ax0 - T, ay0, ax0, ay1, A, H_ARENA + 1,
     gaps=(((CORR_N[1] + CORR_N[3]) / 2, CORR_N[3] - CORR_N[1]), ((CORR_S[1] + CORR_S[3]) / 2, CORR_S[3] - CORR_S[1])))
# side rooms (their arena-side wall is the arena's)
for r, back in ((NORTH, (NORTH[3], NORTH[3] + T)), (SOUTH, (SOUTH[1] - T, SOUTH[1]))):
    wall(r[0] - T, back[0], r[2] + T, back[1], R, H_ROOM + 1)
    lo, hi = (r[1], r[3] + T) if r is NORTH else (r[1] - T, r[3])
    wall(r[0] - T, lo, r[0], hi, R, H_ROOM + 1)
    wall(r[2], lo, r[2] + T, hi, R, H_ROOM + 1)
# side corridors (the arena's west wall closes their east end; open west into the large corridor)
for r in (CORR_N, CORR_S):
    wall(r[0], r[3], r[2], r[3] + T, C, H_ROOM + 1, vis=(r[0], r[2]), **HALL)
    wall(r[0], r[1] - T, r[2], r[1], C, H_ROOM + 1, vis=(r[0], r[2]), **HALL)
# large corridor: east wall open to the side corridors, west wall's door into the left room
bx0, by0, bx1, by1 = BIG
wall(bx0 - T, by1, bx1 + T, by1 + T, C, H_ROOM + 1, vis=(bx0, bx1), **HALL)
wall(bx0 - T, by0 - T, bx1 + T, by0, C, H_ROOM + 1, vis=(bx0, bx1), **HALL)
wall(bx1, by0, bx1 + T, by1, C, H_ROOM + 1, vis=(by0, by1), **HALL,
     gaps=(((CORR_N[1] + CORR_N[3]) / 2, CORR_N[3] - CORR_N[1]), ((CORR_S[1] + CORR_S[3]) / 2, CORR_S[3] - CORR_S[1])))
wall(bx0 - T, by0, bx0, by1, C, H_ROOM + 1, vis=(by0, by1), **HALL, gaps=((0.0, LEFT_DOOR),))
# left room (its east wall is the large corridor's west wall)
lx0, ly0, lx1, ly1 = LEFT
wall(lx0 - T, ly1, lx1, ly1 + T, R, H_ROOM + 1)
wall(lx0 - T, ly0 - T, lx1, ly0, R, H_ROOM + 1)
wall(lx0 - T, ly0, lx0, ly1, R, H_ROOM + 1)
# the four pillars, floor to ceiling
# fitted: 4 repeats along each 7 m face from its corner, 3 up from the floor (the texture is 1:2), so the pattern
# meets at the corners and the floor
PILLAR_W, PILLAR_H = (PILLARS[0][2] - PILLARS[0][0]) / 4, H_ARENA / 3
for p in PILLARS:
    m = MAT["pillar"]
    box(p[0], p[1], 0, p[2], p[3], H_ARENA,
        Fitted(m, x=(m, p[1], H_ARENA, PILLAR_W, PILLAR_H), y=(m, p[0], H_ARENA, PILLAR_W, PILLAR_H)))


def brush_text(b):
    x0, y0, z0, x1, y1, z1, m = b
    x0, x1 = x0 + OFFSET[0], x1 + OFFSET[0]
    y0, y1 = y0 + OFFSET[1], y1 + OFFSET[1]
    z0, z1 = z0 + OFFSET[2], z1 + OFFSET[2]
    f = lambda v: ("%g" % v)
    base = mat_name(m)
    plain = " nod/%s 0 0 0 %.6g %.6g 0 0 0" % (base, TEX_SCALE[base], TEX_SCALE[base])

    def tex(kind):   # "x": a face facing +-x (u along y), "y": facing +-y (u along x), "z": top / bottom
        if not isinstance(m, Fitted) or kind not in m.faces:
            return plain
        name, u0, vtop, w, h = m.faces[kind]
        tw, th = dds_size(name)
        su, sv = w / tw, h / th
        u0 += OFFSET[1] if kind == "x" else OFFSET[0]
        vtop += OFFSET[2]
        ou, ov = (-u0 / su) % tw, (vtop / sv) % th   # texel 0 at u0; the texture's top row at vtop
        return " nod/%s %.4f %.4f 0 %.6g %.6g 0 0 0" % (name, ou, ov, su, sv)
    p = lambda x, y, z: "( %s %s %s )" % (f(x), f(y), f(z))
    lines = [
        p(x0, y0, z0) + " " + p(x1, y0, z0) + " " + p(x1, y1, z0) + tex("z"),   # bottom
        p(x0, y0, z1) + " " + p(x0, y1, z1) + " " + p(x1, y1, z1) + tex("z"),   # top
        p(x0, y0, z0) + " " + p(x0, y1, z0) + " " + p(x0, y1, z1) + tex("x"),   # west
        p(x1, y0, z0) + " " + p(x1, y0, z1) + " " + p(x1, y1, z1) + tex("x"),   # east
        p(x0, y0, z0) + " " + p(x0, y0, z1) + " " + p(x1, y0, z1) + tex("y"),   # south
        p(x0, y1, z0) + " " + p(x1, y1, z0) + " " + p(x1, y1, z1) + tex("y"),   # north
    ]
    return "{\n" + "\n".join(lines) + "\n}\n"


# ---------------------------------------------------------------------------------------------------------------
# Entities
entities = []


def ent(classname, origin, **kv):
    e = {"classname": classname, "origin": "%g %g %g" % tuple(origin[k] + OFFSET[k] for k in range(3))}
    e.update({k: str(v) for k, v in kv.items()})
    entities.append(e)


def template_type(name):
    """native type = template modifier 20 ("type"), inherited through basedon"""
    seen, n = set(), name
    while n and n.lower() not in seen:
        seen.add(n.lower())
        off = notdump.find(n)
        if off is None:
            raise SystemExit("unknown template: " + name)
        props = dict(notdump.props(off))
        if "type" in props:
            return int(props["type"])
        n = props.get("basedon")
    return 0


PLACED = []


def tmpl(name, origin, yaw=0):
    """native template placement, written the way nodsdk-radiant does (MAPtoNSD: case-sensitive Global.not names)"""
    if not any(nm == name for nm, _ in notdump.idx):
        raise SystemExit("template name case: " + name)
    PLACED.append((name, tuple(origin), yaw))
    ent("nod_template_instance", origin, nod_authoring="map-template-v1", nod_template=name,
        nod_template_archive="global.not", nod_template_archives="", nod_template_type=template_type(name),
        angles="0 %g 0" % yaw)


def mirrored(name, x, y):
    """a pickup and its mirror image across y = 0 (one, if it is on the axis)"""
    tmpl(name, (x, y, 0))
    if abs(y) > 1e-6:
        tmpl(name, (x, -y, 0))


# spawns (the level script moves the players there): player 1 in the north room facing south, player 2 in the south
# room facing north; yaw as the level script takes it (0 = +Y)
SPAWN_1 = (0.0, 24.0, 180.0)
SPAWN_2 = (0.0, -24.0, 0.0)
tmpl("ghost", (SPAWN_1[0], SPAWN_1[1], 0.1), 270)   # the editor's player placement (MAPtoNSD writes no player start)


def facing_centre(x, y):
    """level-script yaw (0 = +Y, 90 = -X) looking at the arena's centre"""
    return math.degrees(math.atan2(x, -y)) % 360 if (x or y) else 0.0


# competitive respawns (the level script's "respawn" lines; GhostPC revives a dead player at a random one, not the one
# nearest the other player): both side rooms, the left room, the large and side corridors, the arena's corners and
# east side; mirrored like the rest
RESPAWNS = [(0.0, 24.0), (0.0, -24.0), (-30.75, 0.0), (-24.0, 14.0), (-24.0, -14.0), (-24.0, 6.0), (-24.0, -6.0),
            (-19.25, 15.0), (-19.25, -15.0), (14.0, 15.0), (14.0, -15.0), (-14.0, 15.0), (-14.0, -15.0), (14.0, 0.0)]
assert {(x, round(-y, 3)) for x, y in RESPAWNS} == {(x, round(y, 3)) for x, y in RESPAWNS}, "respawns not mirrored"

# side rooms (north listed, south mirrored): health and psi along the west wall, ammo along the east, powerups in the
# back corners, the spawn point (x 0, y 24) left clear
for name, x, y in [("powHealth", -3.2, 19.5), ("powHealth", -3.2, 21.0), ("powEnergy", -3.2, 22.5), ("powEnergy", -3.2, 24.0),
                   ("powGauss_2x", 3.2, 19.5), ("powGauss_2x", 3.2, 21.0), ("powSnipe_2x", 3.2, 22.5), ("powGrenade", 3.2, 24.0),
                   ("powSpiderMine", -3.2, 26.2), ("powProtection", 3.2, 26.2)]:
    mirrored(name, x, y)
# left room: health and psi along the west wall, ammo and powerups along the north and south walls (mirrored)
for name, x, y in [("powHealth", -34.2, 0.0), ("powHealth", -34.2, 1.5), ("powEnergy", -34.2, 3.0),
                   ("powGauss_2x", -32.5, 3.7), ("powSnipe_2x", -31.0, 3.7), ("powGrenade", -29.5, 3.7),
                   ("powCanisterConcussion", -28.0, 3.7)]:
    mirrored(name, x, y)
tmpl("powProtection", (-31.0, 0.0, 0))
tmpl("powSpiderMine", (-29.0, 0.0, 0))
# central arena, scattered symmetrically (the origin, where the players are created, stays clear)
for name, x, y in [("powHealth", 0.0, 9.0), ("powGauss_2x", -14.0, 3.0), ("powGauss_2x", 14.0, 3.0),
                   ("powSnipe_2x", 9.0, 15.5), ("powSnipe_2x", -9.0, 15.5)]:
    mirrored(name, x, y)
tmpl("powEnergy", (-9.0, 0.0, 0))
tmpl("powEnergy", (9.0, 0.0, 0))

# lights: (x, y, z, r, g, b, intensity, radius) - a cool blue-white Protoss light
blue = (0.75, 0.85, 1.0)
LIGHTS = [(x, y, 8.5) + blue + (20, 22) for x in (-12, 0, 12) for y in (-13, 0, 13)]          # arena
LIGHTS += [(0, cy, 4) + blue + (8, 10) for cy in (22.5, -22.5)]                               # side rooms
LIGHTS += [(-19.25, cy, 4) + blue + (5, 6) for cy in (15, -15)]                               # side corridors
LIGHTS += [(-24, y, 4) + blue + (6, 9) for y in (-12, 0, 12)]                                 # large corridor
LIGHTS += [(-30.75, y, 4) + blue + (8, 9) for y in (-2, 2)]                                   # left room
assert len(LIGHTS) <= 64, len(LIGHTS)
for x, y, z, r, g, b, i, rad in LIGHTS:
    ent("nod_static_light", (x, y, z), _color="%g %g %g" % (r, g, b), light=i, radius=rad)

# waypoints (MAPtoNPD): a grid over the arena's open floor (no AI here; the level just gets a valid .npd)
wp, links, ids = [], [], {}
for i, x in enumerate(range(-14, 15, 7)):
    for j, y in enumerate(range(-15, 16, 5)):
        if any(p[0] - 1 < x < p[2] + 1 and p[1] - 1 < y < p[3] + 1 for p in PILLARS) or (x, y) == (0, 0):
            continue
        ids[(i, j)] = len(wp) + 1
        wp.append((x, y, 0.5))
for n, (x, y, z) in enumerate(wp, 1):
    ent("nod_npd_waypoint", (x, y, z), nod_authoring="npd-world-v1", nod_space="world", nod_waypoint_id=n, nod_flags=0)
for (i, j), a in ids.items():
    for di, dj in ((1, 0), (0, 1)):
        b = ids.get((i + di, j + dj))
        if b:
            links += [(a, b), (b, a)]
for a, b in links:
    m = [(wp[a - 1][k] + wp[b - 1][k]) / 2 for k in range(3)]
    entities.append({"classname": "nod_npd_link", "nod_authoring": "npd-world-v1", "nod_space": "world",
                     "origin": "%g %g %g" % tuple(m[k] + OFFSET[k] for k in range(3)), "nod_from": str(a),
                     "nod_to": str(b), "nod_link_flags": "0"})


def tri_estimate(spacing):
    """upper-bound estimate: every brush face subdivided into spacing-sized quads (2 triangles each)"""
    tris = 0
    for x0, y0, z0, x1, y1, z1, m in brushes:
        dims = (x1 - x0, y1 - y0, z1 - z0)
        for a, b in ((0, 1), (0, 1), (0, 2), (0, 2), (1, 2), (1, 2)):
            tris += 2 * max(1, math.ceil(dims[a] / spacing)) * max(1, math.ceil(dims[b] / spacing))
    return tris


TRI_TARGET = 1800   # MAPtoNIL rejected ~3100 on the Zoo; stay well below
SPACING = 2.0
while tri_estimate(SPACING) > TRI_TARGET and SPACING < 128:
    SPACING += 1.0


def symmetry_check():
    """every brush and placement has its mirror image across y = 0"""
    key = lambda b: tuple(round(v, 3) for v in b[:6]) + (mat_name(b[6]),)
    have = {key(b) for b in brushes}
    for b in brushes:
        x0, y0, z0, x1, y1, z1, m = b
        assert key((x0, -y1, z0, x1, -y0, z1, m)) in have, "brush without a mirror: %r" % (b,)
    spots = {(n, round(o[0], 3), round(o[1], 3)) for n, o, _ in PLACED if n != "ghost"}
    for n, x, y in spots:
        assert (n, x, round(-y, 3)) in spots, "placement without a mirror: %s %g %g" % (n, x, y)


symmetry_check()

with open(OUT, "w", newline="\n") as f:
    f.write("// Protoss arena (8_1_1a_Protoss_Arena) - generated by make_arena_map.py, do not edit by hand\n")
    f.write('{\n"classname" "worldspawn"\n"nod_light_spacing" "%g"\n' % SPACING)
    for b in brushes:
        f.write(brush_text(b))
    f.write("}\n")
    for e in entities:
        f.write("{\n")
        for k, v in e.items():
            f.write('"%s" "%s"\n' % (k, v))
        f.write("}\n")

# GhostPC level script (src/levelscript.cpp): the players are created at the origin; move them to their spawns
with open(os.path.join(HERE, "out_levelscript.gpc"), "w", newline="\r\n") as f:
    f.write("# GhostPC level script for 8_1_1a_Protoss_Arena (generated by make_arena_map.py)\n")
    f.write("player_start %g %g %g %g\n" % (SPAWN_1[0] + OFFSET[0], SPAWN_1[1] + OFFSET[1], OFFSET[2], SPAWN_1[2]))
    f.write("player2_start %g %g %g %g\n" % (SPAWN_2[0] + OFFSET[0], SPAWN_2[1] + OFFSET[1], OFFSET[2], SPAWN_2[2]))
    for x, y in RESPAWNS:
        f.write("respawn %g %g %g %g\n" % (x + OFFSET[0], y + OFFSET[1], OFFSET[2], facing_centre(x, y)))

# effect list: the pickups' effects plus the original lists' (only names in Misc\game.nfx)
nfx = open(os.path.join(GAME, "Misc", "game.nfx"), "rb").read().lower()
effects = [l.strip() for l in open(os.path.join(GAME, "Levels", "2_2_2a_Threat_BaseUpper.nlfx"), encoding="latin1") if l.strip()]
for n in sorted({p[0] for p in PLACED}):
    seen, t = set(), n
    while t and t.lower() not in seen:
        seen.add(t.lower())
        props = notdump.props(notdump.find(t))
        effects += [v for k, v in props if k.upper().startswith("EFFECT") and v]
        t = dict(props).get("basedon")
unique = []
for e in effects:
    if e.lower() not in [u.lower() for u in unique] and e.lower().encode() in nfx:
        unique.append(e)
with open(os.path.join(HERE, "out_effects.nlfx"), "w", newline="\r\n") as f:
    f.write("\n".join(unique) + "\n")

print("brushes %d, entities %d (lights %d, pickups %d, waypoints %d, links %d)" % (
    len(brushes), len(entities), len(LIGHTS), len(PLACED) - 1, len(wp), len(links)))
print("light spacing %g -> triangle estimate %d (upper bound, limit 4096)" % (SPACING, tri_estimate(SPACING)))
print("symmetric about y = 0; wrote", OUT)
