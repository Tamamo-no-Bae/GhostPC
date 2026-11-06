r"""Generates the Quake III MAP for the Vehicle Duel (level 8_2_1a_Vehicle_Duel), a competitive map for two players,
from the user's layout drawing; built on the vehicle test map (levels/vehicles/make_vehicle_map.py: the same field,
track and block, same scale).

Layout (game units = metres, X east, Y north, Z up; north is up), mirror-symmetric about the track's centre line
x = TRACK_CX (west half = east half):

    +----------+  +--------------------------------------+  +----------+
    |          |  | grass   .------------------.         |  |          |
    |          |  |        /   pavement track   \        |  |          |
    |          |  |       |  V .------------. V  |       |  |          |
    |  spawn   +--+       |   |  +--------+  |   |       +--+  spawn   |
    |  (west)   road ======|===|==|   G    |==|===|====== road  (east)  |
    |          +--+       |   |  +--------+  |   |       +--+ (origin) |
    |          |  |       |  D '------------' D  |       |  |          |
    |          |  |        \                    /        |  |          |
    +----------+  |         '------------------'         |  +----------+
                  +--------------------------------------+

- spawn rooms (Protoss, roofed) east and west, each with a 12 m corridor through the field wall; the east one holds
  the world origin, where the game creates the players (the level script moves them to their starts);
- pavement roads (12 m) from each corridor across the grass and the track to the block;
- the solid block (40 x 80 m) has a 12 m passage through it east-west (Protoss floor, open to the sky) and, off its
  north side, an alcove (20 x 12 m) holding the Goliath (G);
- a Vulture (V) on each straight north of the road, a Dragoon (D) on each straight south of it;
- supplies: ammo, psi and spider mines in both spawn rooms, psi beside the Goliath.

Run:  python make_duel_map.py   ->  writes duel.map, out_levelscript.gpc and out_effects.nlfx next to this script and
duel_overview.png (the layout from above). Then compile_duel.bat (the user) and install_duel.py.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "duel.map")
NAME = "8_2_1a_Vehicle_Duel"
GAME = os.environ.get("GHOST_GAME_DIR") or next(
    (g for g in (os.path.join(HERE, "..", "..", "game"), os.path.join(HERE, ".."), os.path.join(HERE, "..", ".."))
     if os.path.exists(os.path.join(g, "Misc"))),   # GhostPC workspace, or the patch's mapsrc/duel in the game folder
    os.path.join(HERE, ".."))
os.environ.setdefault("GHOST_GAME_DIR", GAME)   # for notdump
sys.path.insert(0, os.path.join(HERE, "..", "..", "scripts"))
sys.path.insert(0, os.path.join(HERE, ".."))   # mapsrc/notdump.py in the patch
sys.path.insert(0, HERE)
import notdump  # noqa: E402  (Global.not reader)

T = 1.0                       # wall thickness
OFFSET = (0.0, 0.0, -0.3)     # floors 0.3 m below the origin, as on the Zoo and the arena

# the field: grass inside rock walls, open to the sky (as the vehicle test map)
FIELD = (-144.0, -80.0, -14.0, 80.0)    # interior; its east and west walls have the corridors' openings
H_FIELD = 12.0                          # wall height
TRACK_CX = (FIELD[0] + FIELD[2]) / 2    # the track's centre line (x -79): the mirror line
R_OUT, R_IN = 41.0, 26.0                # track: outer / inner radius of its curved ends (15 m of pavement)
STRAIGHT = 28.5                         # the curved ends' centres at y = +-STRAIGHT
BLOCK_HX, BLOCK_HY = 20.0, 40.0         # the solid block's half extents (40 x 80 m) around (TRACK_CX, 0)
H_BLOCK = 12.0
ARC = 16                                # segments per curved end
ROAD = 6.0                              # half width of the roads, the corridors and the block's passage (12 m)
ALCOVE_HX, ALCOVE_Y1 = 10.0, 18.0       # the Goliath's alcove: x TRACK_CX +-10, y ROAD .. 18
# east spawn room (Protoss, roofed; it holds the origin) and its corridor to the field wall (interior x0, y0, x1, y1);
# the west ones are their mirror images
SPAWN = (-5.0, -14.0, 15.0, 14.0)
CORR = (FIELD[2] + T, -ROAD, SPAWN[0] - T, ROAD)
H_ROOM = 5.0

SKY_MODEL = "sky_Robota_02.nod"   # the clear blue Robota skybox (see levels/zoo/make_zoo_map.py)

MAT = {
    "grass": "MS_Grass_Green_02", "pavement": "MS_Asphalt", "block": "GE_concrete_01", "rock": "AR_RockWall",
    "floor_spawn": "AR_Plated_Tiles_blu", "wall_spawn": "AR_Striped_blu_gld_01", "ceiling": "AR_Pattern_HComb_gld",
    "floor_corr": "AR_Groovy_blu_01", "wall_corr": "AR_Plated_blu_gld_01",
}
TILE = {"grass": 4.0, "pavement": 6.0, "block": 6.0, "rock": 8.0}   # metres per texture repeat (default 4)


def dds_width(name):
    """texture width from its DDS header, searched like the game does (MaterialDirs.txt)"""
    for d in open(os.path.join(GAME, "MaterialDirs.txt"), encoding="latin1").read().split():
        f = os.path.join(GAME, *d.split(chr(92)), name + ".dds")
        if os.path.exists(f):
            with open(f, "rb") as h:
                hdr = h.read(20)
            return int.from_bytes(hdr[16:20], "little")
    raise SystemExit("texture not found: " + name)


TEX_SCALE = {m: TILE.get(k, 4.0) / dds_width(m) for k, m in MAT.items()}

# ---------------------------------------------------------------------------------------------------------------
# Geometry: brushes are convex prisms, (polygon [(x, y) counter-clockwise], z0, z1, material); boxes are 4-gons
brushes = []
GRID = 64   # corners snapped to 1/64 m (exact in binary: shared faces come out identical on both sides)


def snap(v):
    return round(v * GRID) / GRID


def prism(pts, z0, z1, mat):
    pts = [(snap(x), snap(y)) for x, y in pts]
    area = sum(pts[i][0] * pts[i - 1][1] - pts[i - 1][0] * pts[i][1] for i in range(len(pts))) / -2
    if area < 0:
        pts.reverse()
    if abs(area) > 1e-6 and z1 - z0 > 1e-6:
        brushes.append((tuple(pts), z0, z1, mat))


def box(x0, y0, z0, x1, y1, z1, mat):
    x0, x1 = sorted((x0, x1))
    y0, y1 = sorted((y0, y1))
    if x1 - x0 > 1e-6 and y1 - y0 > 1e-6:
        prism([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], z0, z1, mat)


G, P, R, B = MAT["grass"], MAT["pavement"], MAT["rock"], MAT["block"]
cx, (fx0, fy0, fx1, fy1) = TRACK_CX, FIELD


def mx(x, s):
    """x on side s: +1 the east half as written, -1 its mirror image across x = TRACK_CX"""
    return x if s > 0 else 2 * cx - x


def ring(r, cy, a0, a1, n):
    """n + 1 points on a circle of radius r around (cx, cy), from angle a0 to a1 (degrees)"""
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)), cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n)))
            for i in range(n + 1)]


def to_rect(x, y, cy, rect):
    """where the ray from (cx, cy) through (x, y) leaves rect (x0, y0, x1, y1)"""
    dx, dy = x - cx, y - cy
    ts = []
    for k, (d, o) in enumerate(((dx, cx), (dy, cy))):
        if abs(d) > 1e-9:
            for edge in (rect[k], rect[k + 2]):
                t = (edge - o) / d
                if t > 0:
                    ts.append(t)
    t = min(t for t in ts if all(rect[j] - 1e-6 <= (cx, cy)[j] + t * (dx, dy)[j] <= rect[j + 2] + 1e-6 for j in (0, 1)))
    return cx + t * dx, cy + t * dy


def crossed(x0, x1, mat_side):
    """a floor strip across the straight section (y -STRAIGHT .. STRAIGHT) from x0 to x1: grass north and south of the
    road, pavement on it (mat_side: the strip's own material for the whole length, e.g. the track's pavement)"""
    if mat_side == P:
        box(x0, -STRAIGHT, -1.0, x1, STRAIGHT, 0.0, P)
        return
    box(x0, -STRAIGHT, -1.0, x1, -ROAD, 0.0, G)
    box(x0, -ROAD, -1.0, x1, ROAD, 0.0, P)        # the road
    box(x0, ROAD, -1.0, x1, STRAIGHT, 0.0, G)


# straights, both halves: block edge -> track (grass with the road), the track's pavement, track -> field wall
for s in (-1, 1):
    crossed(mx(cx + BLOCK_HX, s), mx(cx + R_IN, s), G)
    crossed(mx(cx + R_IN, s), mx(cx + R_OUT, s), P)
    crossed(mx(cx + R_OUT, s), mx(fx1, s), G)
# under the block (x cx +-20): the passage's and the alcove's Protoss floors, grass under the solid parts
FP = MAT["floor_corr"]
box(cx - BLOCK_HX, -STRAIGHT, -1.0, cx + BLOCK_HX, -ROAD, 0.0, G)
box(cx - BLOCK_HX, -ROAD, -1.0, cx + BLOCK_HX, ROAD, 0.0, FP)                      # the passage
box(cx - ALCOVE_HX, ROAD, -1.0, cx + ALCOVE_HX, ALCOVE_Y1, 0.0, FP)                # the alcove
for s in (-1, 1):
    box(mx(cx + BLOCK_HX, s), ROAD, -1.0, mx(cx + ALCOVE_HX, s), STRAIGHT, 0.0, G)
box(cx - ALCOVE_HX, ALCOVE_Y1, -1.0, cx + ALCOVE_HX, STRAIGHT, 0.0, G)
# curved ends (north: centre y +STRAIGHT, angles 0..180; south mirrored): pavement segments, the grass half-disc
# inside, and outside slices out to the field's walls
for sy in (1, -1):
    cy = sy * STRAIGHT
    a0, a1 = (0.0, 180.0) if sy > 0 else (180.0, 360.0)
    outer, inner = ring(R_OUT, cy, a0, a1, ARC), ring(R_IN, cy, a0, a1, ARC)
    for i in range(ARC):
        prism([inner[i], outer[i], outer[i + 1], inner[i + 1]], -1.0, 0.0, P)
    prism(inner, -1.0, 0.0, G)
    rect = (fx0, cy, fx1, fy1) if sy > 0 else (fx0, fy0, fx1, cy)
    corners = [(x, y) for x in (fx0, fx1) for y in ((cy, fy1) if sy > 0 else (fy0, cy))]
    for i in range(ARC):
        p, q = outer[i], outer[i + 1]
        pr, qr = to_rect(*p, cy, rect), to_rect(*q, cy, rect)
        angp, angq = (math.atan2(v[1] - cy, v[0] - cx) % (2 * math.pi) for v in (p, q))
        inside = [c for c in corners if c[1] != cy and angp < math.atan2(c[1] - cy, c[0] - cx) % (2 * math.pi) < angq]
        prism([p, pr] + inside + [qr, q], -1.0, 0.0, G)
# the block, cut by the passage and the alcove
box(cx - BLOCK_HX, -BLOCK_HY, 0.0, cx + BLOCK_HX, -ROAD, H_BLOCK, B)
for s in (-1, 1):
    box(mx(cx + BLOCK_HX, s), ROAD, 0.0, mx(cx + ALCOVE_HX, s), BLOCK_HY, H_BLOCK, B)
box(cx - ALCOVE_HX, ALCOVE_Y1, 0.0, cx + ALCOVE_HX, BLOCK_HY, H_BLOCK, B)
# the field's rock walls, east and west open to the corridors
box(fx0 - T, fy1, 0.0, fx1 + T, fy1 + T, H_FIELD, R)
box(fx0 - T, fy0 - T, 0.0, fx1 + T, fy0, H_FIELD, R)
for s in (-1, 1):
    x0, x1 = mx(fx1, s), mx(fx1 + T, s)
    box(x0, fy0, 0.0, x1, -ROAD, H_FIELD, R)
    box(x0, ROAD, 0.0, x1, fy1, H_FIELD, R)
    box(x0, -ROAD, H_ROOM, x1, ROAD, H_FIELD, R)   # above the opening (the corridor is only H_ROOM high)
# corridors and spawn rooms (Protoss, roofed), both sides
sx0, sy0, sx1, sy1 = SPAWN
for s in (-1, 1):
    box(mx(fx1, s), CORR[1], -1.0, mx(CORR[2], s), CORR[3], 0.0, MAT["floor_corr"])   # through the wall's opening
    box(mx(CORR[0], s), CORR[1] - T, H_ROOM, mx(CORR[2], s), CORR[3] + T, H_ROOM + 1, MAT["ceiling"])
    box(mx(CORR[0], s), CORR[3], 0.0, mx(CORR[2], s), CORR[3] + T, H_ROOM, MAT["wall_corr"])
    box(mx(CORR[0], s), CORR[1] - T, 0.0, mx(CORR[2], s), CORR[1], H_ROOM, MAT["wall_corr"])
    box(mx(sx0 - T, s), sy0 - T, -1.0, mx(sx1 + T, s), sy1 + T, 0.0, MAT["floor_spawn"])
    box(mx(sx0 - T, s), sy0 - T, H_ROOM, mx(sx1 + T, s), sy1 + T, H_ROOM + 1, MAT["ceiling"])
    box(mx(sx0 - T, s), sy1, 0.0, mx(sx1 + T, s), sy1 + T, H_ROOM, MAT["wall_spawn"])
    box(mx(sx0 - T, s), sy0 - T, 0.0, mx(sx1 + T, s), sy0, H_ROOM, MAT["wall_spawn"])
    box(mx(sx1, s), sy0, 0.0, mx(sx1 + T, s), sy1, H_ROOM, MAT["wall_spawn"])        # back wall
    box(mx(sx0 - T, s), sy0, 0.0, mx(sx0, s), -ROAD, H_ROOM, MAT["wall_spawn"])      # front wall, the
    box(mx(sx0 - T, s), ROAD, 0.0, mx(sx0, s), sy1, H_ROOM, MAT["wall_spawn"])       # corridor's opening between
assert CORR[0] == fx1 + T and CORR[2] == sx0 - T, "corridor does not join the field and the spawn room"
assert sx0 < 0 < sx1 and sy0 < 0 < sy1, "the origin (where the game creates the players) is not in the east spawn room"


def brush_text(b):
    pts, z0, z1, m = b
    f = lambda v: ("%.6f" % v).rstrip("0").rstrip(".")   # 1/64 multiples print exactly with 6 decimals
    tex = " nod/%s 0 0 0 %.6g %.6g 0 0 0" % (m, TEX_SCALE[m], TEX_SCALE[m])
    p = lambda x, y, z: "( %s %s %s )" % (f(x + OFFSET[0]), f(y + OFFSET[1]), f(z + OFFSET[2]))
    # plane points ordered as Quake / MAPtoNIL expect: (b - a) x (c - a) points into the brush
    a, b2, c = pts[0], pts[1], pts[2]
    lines = [p(*a, z0) + " " + p(*b2, z0) + " " + p(*c, z0) + tex,     # bottom (normal -z)
             p(*a, z1) + " " + p(*c, z1) + " " + p(*b2, z1) + tex]     # top (+z)
    for i in range(len(pts)):
        u, v = pts[i], pts[(i + 1) % len(pts)]
        lines.append(p(*u, z0) + " " + p(*u, z1) + " " + p(*v, z1) + tex)
    return "{\n" + "\n".join(lines) + "\n}\n"


def check_brushes():
    """convex, counter-clockwise; no two brushes overlapping (bounding boxes, for the axis-aligned boxes)"""
    for pts, z0, z1, m in brushes:
        n = len(pts)
        for i in range(n):
            (x1, y1), (x2, y2), (x3, y3) = pts[i], pts[(i + 1) % n], pts[(i + 2) % n]
            assert (x2 - x1) * (y3 - y2) - (y2 - y1) * (x3 - x2) > -1e-6, "not convex / not CCW: %r" % (pts,)
    boxes = [(min(p[0] for p in b[0]), min(p[1] for p in b[0]), b[1], max(p[0] for p in b[0]), max(p[1] for p in b[0]), b[2])
             for b in brushes if len(b[0]) == 4 and all(p[0] in (b[0][0][0], b[0][2][0]) for p in b[0])]
    for i in range(len(boxes)):
        for j in range(i):
            a, b = boxes[i], boxes[j]
            if all(min(a[k + 3], b[k + 3]) - max(a[k], b[k]) > 1e-3 for k in range(3)):
                raise SystemExit("brushes overlap: %r and %r" % (a, b))


check_brushes()

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
# GhostPC's own templates (game/Misc/Templates/ghostpc.not, scripts/make_ghostpc_templates.py) -> their basedon
GHOSTPC_TEMPLATES = {"dragoon_vehicle_player": "Goliath_player"}


def tmpl(name, origin, yaw=0):
    """native template placement, written the way nodsdk-radiant does (MAPtoNSD: case-sensitive Global.not names;
    the same archive list on every placement)"""
    if name in GHOSTPC_TEMPLATES:
        archive, ttype = "ghostpc.not", template_type(GHOSTPC_TEMPLATES[name])
    elif any(nm == name for nm, _ in notdump.idx):
        archive, ttype = "global.not", template_type(name)
    else:
        raise SystemExit("template name case: " + name)
    PLACED.append((name, tuple(origin), yaw))
    ent("nod_template_instance", origin, nod_authoring="map-template-v1", nod_template=name,
        nod_template_archive=archive, nod_template_archives="ghostpc.not", nod_template_type=ttype,
        angles="0 %g 0" % yaw)


def both(name, x, y, z=0.0, yaw=None):
    """a placement on the east half and its mirror image on the west (yaw mirrored: 360 - yaw)"""
    tmpl(name, (x, y, z), yaw or 0)
    tmpl(name, (2 * cx - x, y, z), (360 - yaw) % 360 if yaw else 0)


# player starts: player 1 in the east room, player 2 in the west, both facing their corridor (level-script yaw:
# 0 = +Y, 90 = -X)
START = (5.0, 0.0)
tmpl("ghost", (START[0], START[1], 0.1), 180)   # the editor's player placement (MAPtoNSD writes no player start)
# vehicles (template yaw: 0 faces +Y). Vultures on the straights north of the road facing south towards it, Dragoons
# south of it facing north (at their track lane's middle, 24 m from the road as drawn); the Goliath in the alcove facing
# the passage. (Player Goliaths and Dragoons are not simulated until mounted and stay where they are put: on the floor)
LANE = (R_OUT + R_IN) / 2
both("Vulture_player", cx + LANE, 24.0, 0.1, 180)
both("dragoon_vehicle_player", cx + LANE, -24.0, -0.3, 0)
tmpl("Goliath_player", (cx, (ROAD + ALCOVE_Y1) / 2, -0.2), 180)
# spawn room supplies along the north and south walls (both rooms, mirrored), the start (x 5) and the corridor's
# opening clear: ammo nearest the start, psi, then spider mines at the back
for name, x in (("powGauss_2x", 3.0), ("powSnipe_2x", 5.0), ("powGrenade", 7.0), ("powEnergy", 9.5), ("powEnergy", 11.0),
                ("powSpiderMine", 13.5)):
    for y in (sy1 - 0.8, sy0 + 0.8):
        both(name, x, y)
# psi beside the Goliath, in the alcove's back corners
both("powEnergy", cx + ALCOVE_HX - 1.5, ALCOVE_Y1 - 1.5)

# competitive respawns (level script "respawn" lines; GhostPC revives a dead player at a random one away from the other
# player): three in each spawn room
RESPAWNS = [(5.0, 8.0), (5.0, -8.0), (11.0, 0.0)]

# lights: (x, y, z, r, g, b, intensity, radius); the field is lit by the sun (compile_duel.bat)
blue = (0.75, 0.85, 1.0)
LIGHTS = []
for s in (-1, 1):
    LIGHTS += [(mx(x, s), y, 4.0) + blue + (8, 10) for x in (0.0, 10.0) for y in (-7.0, 7.0)]      # spawn room
    LIGHTS += [(mx((CORR[0] + CORR[2]) / 2, s), 0.0, 4.0) + blue + (5, 8)]                        # corridor
for x, y, z, r, g, b, i, rad in LIGHTS:
    ent("nod_static_light", (x, y, z), _color="%g %g %g" % (r, g, b), light=i, radius=rad)

# waypoints (MAPtoNPD): the track's centre line, linked both ways all round
wp = [(cx + LANE, y, 0.5) for y in (-STRAIGHT, -STRAIGHT / 3, STRAIGHT / 3)]
wp += [p + (0.5,) for p in ring(LANE, STRAIGHT, 0.0, 180.0, 8)]
wp += [(cx - LANE, y, 0.5) for y in (STRAIGHT / 3, -STRAIGHT / 3)]
wp += [p + (0.5,) for p in ring(LANE, -STRAIGHT, 180.0, 360.0, 8)][:-1]
links = []
for n, (x, y, z) in enumerate(wp, 1):
    ent("nod_npd_waypoint", (x, y, z), nod_authoring="npd-world-v1", nod_space="world", nod_waypoint_id=n, nod_flags=0)
for i in range(len(wp)):
    a, b = i + 1, (i + 1) % len(wp) + 1
    links += [(a, b), (b, a)]
for a, b in links:
    m = [(wp[a - 1][k] + wp[b - 1][k]) / 2 for k in range(3)]
    entities.append({"classname": "nod_npd_link", "nod_authoring": "npd-world-v1", "nod_space": "world",
                     "origin": "%g %g %g" % tuple(m[k] + OFFSET[k] for k in range(3)), "nod_from": str(a),
                     "nod_to": str(b), "nod_link_flags": "0"})


def tri_estimate(spacing):
    """upper-bound estimate: every face subdivided into spacing-sized quads (2 triangles each)"""
    tris = 0
    for pts, z0, z1, m in brushes:
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        w, d, h = max(xs) - min(xs), max(ys) - min(ys), z1 - z0
        tris += 2 * 2 * max(1, math.ceil(w / spacing)) * max(1, math.ceil(d / spacing))
        for i in range(len(pts)):
            e = math.dist(pts[i], pts[(i + 1) % len(pts)])
            tris += 2 * max(1, math.ceil(e / spacing)) * max(1, math.ceil(h / spacing))
    return tris


# 128, as on the vehicle test map: MAPtoNIL 0.6.8's lighting subdivision of the long rock walls gave stray triangles at
# spacings 21..64
SPACING = 128.0


def symmetry_check():
    """every brush, placement and respawn has its mirror image across x = TRACK_CX"""
    key = lambda pts, z0, z1, m: (frozenset((round(x, 2), round(y, 2)) for x, y in pts), z0, z1, m)
    have = {key(*b) for b in brushes}
    for pts, z0, z1, m in brushes:
        assert key([(2 * cx - x, y) for x, y in pts], z0, z1, m) in have, "brush without a mirror: %r %s" % (pts, m)
    spots = {(n, round(o[0], 3), round(o[1], 3)) for n, o, _ in PLACED if n != "ghost"}
    for n, x, y in spots:
        assert (n, round(2 * cx - x, 3), y) in spots, "placement without a mirror: %s %g %g" % (n, x, y)


symmetry_check()

with open(OUT, "w", newline="\n") as f:
    f.write("// Vehicle Duel map (%s) - generated by make_duel_map.py, do not edit by hand\n" % NAME)
    f.write('{\n"classname" "worldspawn"\n"nod_light_spacing" "%g"\n' % SPACING)
    f.write('"nod_sky_model" "%s"\n"nod_sky_unmasked" "1"\n' % SKY_MODEL)
    for b in brushes:
        f.write(brush_text(b))
    f.write("}\n")
    for e in entities:
        f.write("{\n")
        for k, v in e.items():
            f.write('"%s" "%s"\n' % (k, v))
        f.write("}\n")

# GhostPC level script (src/levelscript.cpp): the players are created at the origin (the east room); player 1 stays in
# the east room facing west (yaw 90 = -X), player 2 goes to the west room facing east (270)
with open(os.path.join(HERE, "out_levelscript.gpc"), "w", newline="\r\n") as f:
    f.write("# GhostPC level script for %s (generated by make_duel_map.py)\n" % NAME)
    f.write("player_start %g %g %g %g\n" % (START[0] + OFFSET[0], START[1] + OFFSET[1], OFFSET[2], 90))
    f.write("player2_start %g %g %g %g\n" % (2 * cx - START[0] + OFFSET[0], START[1] + OFFSET[1], OFFSET[2], 270))
    for x, y in RESPAWNS:
        for s, yaw in ((1, 90), (-1, 270)):
            f.write("respawn %g %g %g %g\n" % (mx(x, s) + OFFSET[0], y + OFFSET[1], OFFSET[2], yaw))

# effect list: the placements' effects plus an original list's (only names in Misc\game.nfx and ghostpc.nfx)
nfx = (open(os.path.join(GAME, "Misc", "game.nfx"), "rb").read() + open(os.path.join(GAME, "Misc", "ghostpc.nfx"), "rb").read()).lower()
effects = [l.strip() for l in open(os.path.join(GAME, "Levels", "2_2_2a_Threat_BaseUpper.nlfx"), encoding="latin1") if l.strip()]
effects += ["dragoonProj", "dragoonProjExp", "dragoonChrg"]   # the Dragoon's shot (proj_dragoon, exp_dragoonProj)
effects += ["ProtBuildDam", "psiburn"]   # the Dragoon's damage burn (EFFECT4)
for n in sorted({p[0] for p in PLACED}):
    seen, t = set(), GHOSTPC_TEMPLATES.get(n, n)
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

# overview from above (needs Pillow): grass, pavement, block, walls, Protoss floors, vehicles and supplies
try:
    from PIL import Image, ImageDraw
    S, X0, X1, Y1 = 4, 2 * cx - sx1 - 4, sx1 + 4, fy1 + 4
    img = Image.new("RGB", (int((X1 - X0) * S), int((Y1 - fy0 + 4) * S)), (255, 255, 255))
    d = ImageDraw.Draw(img)
    colour = {G: (40, 170, 70), P: (128, 128, 128), B: (0, 0, 0), R: (90, 70, 60)}
    for pts, z0, z1, m in sorted(brushes, key=lambda b: (b[2], b[3] == B)):
        if z1 <= 0 or m in colour or z0 < H_ROOM:
            if z1 > 0 and m not in colour:
                continue   # (Protoss walls: the floors show the rooms)
            d.polygon([((x - X0) * S, (Y1 - y) * S) for x, y in pts], fill=colour.get(m, (255, 230, 0)), outline=(0, 0, 0))
    dot = {"Vulture_player": (255, 220, 0), "dragoon_vehicle_player": (60, 80, 230), "Goliath_player": (230, 30, 30)}
    for n, o, _ in PLACED:
        c = dot.get(n, (255, 255, 255) if n.startswith("pow") else None)
        r = 2.5 if n in dot else 0.6
        if c:
            d.ellipse([(o[0] - r - X0) * S, (Y1 - o[1] - r) * S, (o[0] + r - X0) * S, (Y1 - o[1] + r) * S], fill=c, outline=(0, 0, 0))
    img.save(os.path.join(HERE, "duel_overview.png"))
except ImportError:
    pass

print("brushes %d, entities %d (lights %d, placements %d, waypoints %d, links %d)" % (
    len(brushes), len(entities), len(LIGHTS), len(PLACED), len(wp), len(links)))
print("light spacing %g -> triangle estimate %d (upper bound, limit 4096)" % (SPACING, tri_estimate(SPACING)))
print("symmetric about x = %g; wrote %s" % (cx, OUT))
